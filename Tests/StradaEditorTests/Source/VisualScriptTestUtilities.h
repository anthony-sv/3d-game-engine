#pragma once

#include "TestUtilities.h"

#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptNodeTitles.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptView.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Hash.h"
#include "Strada/Core/Platform.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Script/VisualScriptLiterals.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// What the tests of the visual script editor's canvas, panes and windows share: the fixture node library with an analyzer of
// its templates, which stand in for the scripting runtime; a text measurer that needs no font; and projects holding a graph.
// No ImGui.
namespace Strada::Testing
{
	// Tests/Data/VisualScripting/Editor/Library.json: a verbatim subset of the scripting runtime's node library.
	inline std::filesystem::path GetFixtureLibraryPath()
	{
		return FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Editor" / "Library.json";
	}

	inline std::string const& GetFixtureLibraryText()
	{
		static std::string const s_Text = []
		{
			Result<std::string> text = FileSystem::ReadTextFile(GetFixtureLibraryPath());
			REQUIRE_MESSAGE(text.IsOk(), text.GetError());
			return text.TakeValue();
		}();
		return s_Text;
	}

	namespace Detail
	{
		inline char const* GetDirectionName(VisualScriptPinDirection direction)
		{
			return direction == VisualScriptPinDirection::In ? "In" : "Out";
		}

		// A pin as the analysis' JSON has it.
		inline Json MakeAnalysisPin(std::string const& name, VisualScriptPinDirection direction, std::string_view type)
		{
			Json pin = Json::object();
			pin["Name"] = name;
			pin["Direction"] = GetDirectionName(direction);
			pin["Type"] = std::string(type);
			return pin;
		}

		// A wildcard that accepts any type with values; the inferred pins of unknown nodes have no constraint.
		inline Json MakeWildcardPin(std::string const& name, VisualScriptPinDirection direction, bool any = true)
		{
			Json pin = MakeAnalysisPin(name, direction, VisualScriptWildcardType);
			if (any)
			{
				pin["Constraint"] = "Any";
			}
			return pin;
		}

		inline Json MakeLinkJson(VisualScriptLink const& link)
		{
			Json json = Json::object();
			json["From"] = Json::array({link.From.Node, link.From.Pin});
			json["To"] = Json::array({link.To.Node, link.To.Pin});
			return json;
		}

		// A diagnostic as the analysis' JSON has it.
		inline Json SerializeDiagnostic(VisualScriptDiagnostic const& diagnostic)
		{
			Json json = Json::object();
			json["Severity"] = diagnostic.Severity == VisualScriptSeverity::Error ? "Error" : "Warning";
			json["Code"] = diagnostic.Code;
			json["Function"] = diagnostic.Function;
			if (diagnostic.Node)
			{
				json["Node"] = *diagnostic.Node;
			}
			if (!diagnostic.Pin.empty())
			{
				json["Pin"] = diagnostic.Pin;
			}
			if (diagnostic.Direction)
			{
				json["Direction"] = GetDirectionName(*diagnostic.Direction);
			}
			if (diagnostic.Link)
			{
				json["Link"] = MakeLinkJson(*diagnostic.Link);
			}
			if (diagnostic.Variable)
			{
				json["Variable"] = *diagnostic.Variable;
			}
			json["Message"] = diagnostic.Message;
			if (!diagnostic.Suggestions.empty())
			{
				json["Suggestions"] = diagnostic.Suggestions;
			}
			return json;
		}

		// The analysis the scripting runtime gives a document, as far as the node library's templates and the document tell
		// it, shaped as the runtime shapes nodes: template pins, except for the nodes whose pins come from their settings or
		// the document (variables and locals, function signatures, Sequence counts, Switch cases, custom-event parameters,
		// Literal, Convert, Make and Break types) and split pins. Nodes of types the library does not know have the pins
		// their links and inputs name. Wildcards stay unresolved, FormatString gets no placeholder inputs and generic calls
		// keep their template pins; the only diagnostics are UnknownNodeType and UnknownPin.
		class FixtureAnalyzer
		{
		public:
			FixtureAnalyzer(VisualScriptLibrary const& library, VisualScriptDocument const& document)
				: m_Library(library),
				  m_Document(document)
			{
			}

			Json Analyze()
			{
				std::vector<std::pair<VisualScriptFunction const*, VisualScriptGraph const*>> graphs;
				for (VisualScriptFunction const& function : m_Document.Functions)
				{
					graphs.emplace_back(&function, &function.Graph);
				}
				graphs.emplace_back(nullptr, &m_Document.Graph);
				FindLatentFunctions();

				// As the runtime analyzes: every node resolved, then shaped, then the links attached, graph by graph.
				Json diagnostics = Json::array();
				std::vector<std::vector<Shape>> shapes;
				for (auto const& [owner, graph] : graphs)
				{
					std::vector<Shape>& graphShapes = shapes.emplace_back();
					for (VisualScriptNode const& node : graph->Nodes)
					{
						Shape shape;
						shape.Node = &node;
						shape.Owner = owner;
						shape.Template = m_Library.FindNode(node.Type);
						if (shape.Template == nullptr)
						{
							shape.Title = GetUnknownTitle(node.Type);
							shape.PinsComplete = false;
							diagnostics.push_back(
								MakeDiagnostic("UnknownNodeType", shape, fmt::format("unknown node type \"{}\"", node.Type)));
						}
						graphShapes.push_back(std::move(shape));
					}
				}
				for (std::vector<Shape>& graphShapes : shapes)
				{
					for (Shape& shape : graphShapes)
					{
						if (shape.Template != nullptr)
						{
							ShapeNode(shape);
						}
					}
				}
				for (size_t index = 0; index < graphs.size(); index++)
				{
					AttachLinks(*graphs[index].second, shapes[index], diagnostics);
				}

				Json nodes = Json::array();
				for (std::vector<Shape> const& graphShapes : shapes)
				{
					for (Shape const& shape : graphShapes)
					{
						nodes.push_back(WriteNode(shape));
					}
				}
				Json analysis = Json::object();
				analysis["Nodes"] = std::move(nodes);
				analysis["Diagnostics"] = std::move(diagnostics);
				return analysis;
			}

		private:
			struct Shape
			{
				VisualScriptNode const* Node = nullptr;
				// The function whose graph holds the node; null for the event graph.
				VisualScriptFunction const* Owner = nullptr;
				// Null for a type the library does not know.
				VisualScriptNodeTemplate const* Template = nullptr;
				std::string Title;
				std::string Subtitle;
				bool Pure = false;
				bool Latent = false;
				Json Pins = Json::array();
				// False when the pins are not all known (an unknown type, settings that do not shape it): links and inputs that
				// name other pins add them instead of being unknown pins.
				bool PinsComplete = true;
			};

			// The title the runtime gives a node of a type it does not know: the words of its member or kind name ("For Loop" for
			// "Flow.ForLoop", "Bar" for "Get:Strada.Foo.Bar").
			static std::string GetUnknownTitle(std::string_view type)
			{
				std::string_view name = type.substr(0, std::min(type.find('('), type.find('<')));
				if (size_t const colon = name.find(':'); colon != std::string_view::npos)
				{
					name = name.substr(colon + 1);
				}
				if (size_t const dot = name.rfind('.'); dot != std::string_view::npos && dot + 1 < name.size())
				{
					name = name.substr(dot + 1);
				}
				if (name.empty())
				{
					return std::string(type);
				}
				VisualScriptNode probe;
				probe.Type = std::string(name);
				return DeriveVisualScriptNodeTitle(probe, VisualScriptDocument());
			}

			static Json MakeDiagnostic(std::string_view code, Shape const& shape, std::string message)
			{
				VisualScriptDiagnostic diagnostic;
				diagnostic.Code = std::string(code);
				diagnostic.Function = shape.Owner != nullptr ? shape.Owner->ID : 0;
				diagnostic.Node = shape.Node->ID;
				diagnostic.Message = std::move(message);
				return SerializeDiagnostic(diagnostic);
			}

			static Json const* FindSetting(VisualScriptNode const& node, char const* key)
			{
				if (!node.Settings.is_object())
				{
					return nullptr;
				}
				auto const found = node.Settings.find(key);
				return found != node.Settings.end() ? &*found : nullptr;
			}

			static Json const* FindSetting(Shape const& shape, char const* key) { return FindSetting(*shape.Node, key); }

			// An ID setting (Variable, Function): an integer JSON number within uint32.
			static std::optional<uint32_t> ReadIDSetting(VisualScriptNode const& node, char const* key)
			{
				Json const* value = FindSetting(node, key);
				if (value == nullptr || !value->is_number_integer())
				{
					return std::nullopt;
				}
				if (value->is_number_unsigned())
				{
					uint64_t const id = value->get<uint64_t>();
					return id <= std::numeric_limits<uint32_t>::max() ? std::optional<uint32_t>(static_cast<uint32_t>(id)) : std::nullopt;
				}
				int64_t const id = value->get<int64_t>();
				return id >= 0 && id <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max())
				           ? std::optional<uint32_t>(static_cast<uint32_t>(id))
				           : std::nullopt;
			}

			// The library type a Type setting names, when accepts takes it; null otherwise.
			template<typename Accepts>
			VisualScriptTypeInfo const* ReadTypeSetting(Shape const& shape, char const* key, Accepts&& accepts) const
			{
				Json const* value = FindSetting(shape, key);
				if (value == nullptr || !value->is_string())
				{
					return nullptr;
				}
				VisualScriptTypeInfo const* type = m_Library.FindType(value->get_ref<std::string const&>());
				return type != nullptr && accepts(*type) ? type : nullptr;
			}

			// The type a variable, local or parameter declared with the type ID has: nothing for pin types, arrays and types
			// the library does not know.
			std::optional<std::string> ResolveDeclaredType(std::string_view typeId) const
			{
				if (typeId == VisualScriptExecType || typeId == VisualScriptWildcardType)
				{
					return std::nullopt;
				}
				VisualScriptTypeInfo const* type = m_Library.FindType(typeId);
				if (type == nullptr || type->Kind == VisualScriptTypeKind::Array)
				{
					return std::nullopt;
				}
				return type->ID;
			}

			// A pin typed by a declaration; a wildcard that accepts anything when the declared type is not valid.
			static Json MakeDeclaredPin(std::string const& name, VisualScriptPinDirection direction, std::optional<std::string> const& type)
			{
				return type ? MakeAnalysisPin(name, direction, *type) : MakeWildcardPin(name, direction);
			}

			static Json MakeExecIn(std::string const& name = "Execute")
			{
				return MakeAnalysisPin(name, VisualScriptPinDirection::In, VisualScriptExecType);
			}

			static Json MakeExecOut(std::string const& name = "Then")
			{
				return MakeAnalysisPin(name, VisualScriptPinDirection::Out, VisualScriptExecType);
			}

			VisualScriptFunction const* FindFunction(uint32_t id) const
			{
				auto const found = std::find_if(m_Document.Functions.begin(), m_Document.Functions.end(),
				                                [id](VisualScriptFunction const& function)
				                                {
													return function.ID == id;
												});
				return found != m_Document.Functions.end() ? &*found : nullptr;
			}

			// An impure function is latent when its graph holds a latent node or calls a latent function.
			void FindLatentFunctions()
			{
				bool changed = true;
				while (changed)
				{
					changed = false;
					for (VisualScriptFunction const& function : m_Document.Functions)
					{
						if (function.Pure || m_LatentFunctions.contains(function.ID))
						{
							continue;
						}
						bool const latent = std::any_of(function.Graph.Nodes.begin(), function.Graph.Nodes.end(),
						                                [this](VisualScriptNode const& node)
						                                {
															VisualScriptNodeTemplate const* const kind = m_Library.FindNode(node.Type);
															if (kind == nullptr)
															{
																return false;
															}
															if (kind->Latent)
															{
																return true;
															}
															std::optional<uint32_t> const callee = node.Type == "Function.Call"
							                                                                           ? ReadIDSetting(node, "Function")
							                                                                           : std::nullopt;
															return callee && m_LatentFunctions.contains(*callee);
														});
						if (latent)
						{
							m_LatentFunctions.insert(function.ID);
							changed = true;
						}
					}
				}
			}

			void AddTemplatePins(Shape& shape) const
			{
				auto const pins = shape.Template->Source.find("Pins");
				if (pins != shape.Template->Source.end() && pins->is_array())
				{
					for (Json const& pin : *pins)
					{
						shape.Pins.push_back(pin);
					}
				}
			}

			void ShapeNode(Shape& shape) const
			{
				VisualScriptNodeTemplate const& kind = *shape.Template;
				shape.Title = kind.Title;
				shape.Subtitle = kind.Subtitle;
				shape.Pure = kind.Pure;
				shape.Latent = kind.Latent;
				std::string_view const type = kind.Type;
				if (type == "Variable.Get" || type == "Variable.Set")
				{
					ShapeVariable(shape, type == "Variable.Set");
				}
				else if (type == "Function.Entry" || type == "Function.Return")
				{
					ShapeFunctionEnd(shape, type == "Function.Entry");
				}
				else if (type == "Function.Call")
				{
					ShapeFunctionCall(shape);
				}
				else if (type == "Flow.Sequence")
				{
					ShapeSequence(shape);
				}
				else if (type == "Flow.Switch")
				{
					ShapeSwitch(shape);
				}
				else if (type == "Event.Custom" || type == "Event.Send")
				{
					ShapeCustomEvent(shape, type == "Event.Send");
				}
				else if (type == "Literal")
				{
					ShapeLiteral(shape);
				}
				else if (type == "Convert")
				{
					ShapeConvert(shape);
				}
				else if (type == "Make" || type == "Break")
				{
					ShapeStruct(shape, type == "Make");
				}
				else
				{
					AddTemplatePins(shape);
				}
				SplitPins(shape, shape.Node->Split.Inputs, VisualScriptPinDirection::In);
				SplitPins(shape, shape.Node->Split.Outputs, VisualScriptPinDirection::Out);
			}

			// Typed by the variable or local of the Variable setting: a local of the function whose graph holds the node, else
			// a variable of the document.
			void ShapeVariable(Shape& shape, bool set) const
			{
				std::string const* name = nullptr;
				std::optional<std::string> type;
				std::optional<uint32_t> const id = ReadIDSetting(*shape.Node, "Variable");
				if (id && shape.Owner != nullptr)
				{
					auto const local = std::find_if(shape.Owner->Locals.begin(), shape.Owner->Locals.end(),
					                                [id](VisualScriptLocal const& candidate)
					                                {
														return candidate.ID == *id;
													});
					if (local != shape.Owner->Locals.end())
					{
						name = &local->Name;
						type = ResolveDeclaredType(local->Type);
					}
				}
				if (id && name == nullptr)
				{
					auto const variable = std::find_if(m_Document.Variables.begin(), m_Document.Variables.end(),
					                                   [id](VisualScriptVariable const& candidate)
					                                   {
														   return candidate.ID == *id;
													   });
					if (variable != m_Document.Variables.end())
					{
						name = &variable->Name;
						type = ResolveDeclaredType(variable->Type);
					}
				}
				if (set)
				{
					shape.Pins.push_back(MakeExecIn());
					shape.Pins.push_back(MakeDeclaredPin("Value", VisualScriptPinDirection::In, type));
					shape.Pins.push_back(MakeExecOut());
				}
				shape.Pins.push_back(MakeDeclaredPin("Value", VisualScriptPinDirection::Out, type));
				if (name != nullptr)
				{
					shape.Title = (set ? "Set " : "Get ") + *name;
				}
			}

			// The signature of the function whose graph holds the node: Entry outputs its inputs, Return takes its outputs.
			void ShapeFunctionEnd(Shape& shape, bool entry) const
			{
				if (shape.Owner == nullptr)
				{
					shape.PinsComplete = false;
					return;
				}
				if (!shape.Owner->Pure)
				{
					shape.Pins.push_back(entry ? MakeExecOut() : MakeExecIn());
				}
				for (VisualScriptParameter const& parameter : entry ? shape.Owner->Inputs : shape.Owner->Outputs)
				{
					shape.Pins.push_back(MakeDeclaredPin(parameter.Name,
					                                     entry ? VisualScriptPinDirection::Out : VisualScriptPinDirection::In,
					                                     ResolveDeclaredType(parameter.Type)));
				}
			}

			void ShapeFunctionCall(Shape& shape) const
			{
				std::optional<uint32_t> const id = ReadIDSetting(*shape.Node, "Function");
				VisualScriptFunction const* function = id ? FindFunction(*id) : nullptr;
				if (function == nullptr)
				{
					shape.PinsComplete = false;
					return;
				}
				if (!function->Pure)
				{
					shape.Pins.push_back(MakeExecIn());
				}
				for (VisualScriptParameter const& input : function->Inputs)
				{
					shape.Pins.push_back(MakeDeclaredPin(input.Name, VisualScriptPinDirection::In, ResolveDeclaredType(input.Type)));
				}
				if (!function->Pure)
				{
					shape.Pins.push_back(MakeExecOut());
				}
				for (VisualScriptParameter const& output : function->Outputs)
				{
					shape.Pins.push_back(MakeDeclaredPin(output.Name, VisualScriptPinDirection::Out, ResolveDeclaredType(output.Type)));
				}
				shape.Title = function->Name;
				shape.Pure = function->Pure;
				shape.Latent = m_LatentFunctions.contains(function->ID);
			}

			// Then0 to Then<Count - 1>; Count is 2 to 64, 2 by default.
			static void ShapeSequence(Shape& shape)
			{
				int64_t count = 2;
				if (Json const* value = FindSetting(shape, "Count"))
				{
					if (value->is_number_integer() && value->get<int64_t>() >= 2 && value->get<int64_t>() <= 64)
					{
						count = value->get<int64_t>();
					}
					else
					{
						shape.PinsComplete = false;
					}
				}
				shape.Pins.push_back(MakeExecIn());
				for (int64_t index = 0; index < count; index++)
				{
					shape.Pins.push_back(MakeExecOut(fmt::format("Then{}", index)));
				}
			}

			// The label of a case value of a switch on the type: a string as it is, an int in decimal, an enum value by the
			// name of its first enumerator (in decimal without one); nothing for a value that is none of the type.
			std::optional<std::string> ReadCaseLabel(VisualScriptTypeInfo const& type, Json const& value) const
			{
				if (type.ID == "string")
				{
					if (!value.is_string())
					{
						return std::nullopt;
					}
					std::string const& text = value.get_ref<std::string const&>();
					bool const control = std::any_of(text.begin(), text.end(),
					                                 [](char character)
					                                 {
														 unsigned char const code = static_cast<unsigned char>(character);
														 return code < 0x20 || code == 0x7F;
													 });
					// "Case:" and the value make a pin name of at most 128 bytes.
					return !text.empty() && text.size() <= 123 && !control ? std::optional<std::string>(text) : std::nullopt;
				}
				if (type.ID == "int")
				{
					std::optional<ScriptFieldValue> const decoded = DecodeVisualScriptLiteral(value, type.ID, {});
					return decoded ? std::optional<std::string>(std::to_string(decoded->GetInt32())) : std::nullopt;
				}
				std::optional<VisualScriptEnumInfo> const enumInfo = m_Library.FindEnum(type.ID);
				std::optional<ScriptFieldValue> const decoded =
					enumInfo ? DecodeVisualScriptEnumLiteral(value, *enumInfo) : std::optional<ScriptFieldValue>();
				if (!decoded)
				{
					return std::nullopt;
				}
				for (ScriptEnumerator const& enumerator : enumInfo->Enumerators)
				{
					if (enumerator.Value == *decoded)
					{
						return enumerator.Name;
					}
				}
				switch (decoded->GetType())
				{
					case ScriptFieldType::Int32:
						return std::to_string(decoded->GetInt32());
					case ScriptFieldType::UInt32:
						return std::to_string(decoded->GetUInt32());
					case ScriptFieldType::Int64:
						return std::to_string(decoded->GetInt64());
					case ScriptFieldType::UInt64:
						return std::to_string(decoded->GetUInt64());
					default:
						return std::nullopt;
				}
			}

			// Selection typed by the Type setting (int, string or an enum), an output per case ("Case:<label>", labelled), then
			// Default. Without Cases, an enum switch has a case per enumerator value (named by its first enumerator).
			void ShapeSwitch(Shape& shape) const
			{
				shape.Pins.push_back(MakeExecIn());
				VisualScriptTypeInfo const* type = ReadTypeSetting(shape, "Type",
				                                                   [](VisualScriptTypeInfo const& candidate)
				                                                   {
																	   return candidate.ID == "int" || candidate.ID == "string" ||
					                                                          candidate.Kind == VisualScriptTypeKind::Enum;
																   });
				if (type == nullptr)
				{
					shape.Pins.push_back(MakeWildcardPin("Selection", VisualScriptPinDirection::In));
					shape.Pins.push_back(MakeExecOut("Default"));
					shape.PinsComplete = false;
					return;
				}
				shape.Pins.push_back(MakeAnalysisPin("Selection", VisualScriptPinDirection::In, type->ID));
				std::vector<std::string> labels;
				if (Json const* cases = FindSetting(shape, "Cases"))
				{
					if (!cases->is_array())
					{
						shape.PinsComplete = false;
					}
					else
					{
						for (Json const& value : *cases)
						{
							std::optional<std::string> label = ReadCaseLabel(*type, value);
							if (!label || std::find(labels.begin(), labels.end(), *label) != labels.end())
							{
								shape.PinsComplete = false;
								continue;
							}
							labels.push_back(std::move(*label));
						}
					}
				}
				else if (type->Kind == VisualScriptTypeKind::Enum)
				{
					std::vector<ScriptFieldValue> values;
					for (ScriptEnumerator const& enumerator : type->Enumerators)
					{
						if (std::find(values.begin(), values.end(), enumerator.Value) == values.end())
						{
							values.push_back(enumerator.Value);
							labels.push_back(enumerator.Name);
						}
					}
				}
				for (std::string const& label : labels)
				{
					Json pin = MakeExecOut("Case:" + label);
					pin["Label"] = label;
					shape.Pins.push_back(std::move(pin));
				}
				shape.Pins.push_back(MakeExecOut("Default"));
			}

			// The Parameters setting of custom events: [{ "Name", "Type" }] with valid, unique names that are no reserved pin
			// names; nothing when it is not of that form.
			std::optional<std::vector<std::pair<std::string, std::optional<std::string>>>> ReadParameters(Shape const& shape) const
			{
				std::vector<std::pair<std::string, std::optional<std::string>>> parameters;
				Json const* list = FindSetting(shape, "Parameters");
				if (list == nullptr)
				{
					return parameters;
				}
				if (!list->is_array())
				{
					return std::nullopt;
				}
				std::set<std::string> const reserved = {"Execute", "Then", "Target", "Self", "Default"};
				for (Json const& parameter : *list)
				{
					if (!parameter.is_object() || parameter.size() != 2)
					{
						return std::nullopt;
					}
					auto const nameMember = parameter.find("Name");
					auto const typeMember = parameter.find("Type");
					if (nameMember == parameter.end() || typeMember == parameter.end() || !nameMember->is_string() ||
					    !typeMember->is_string())
					{
						return std::nullopt;
					}
					std::string const& name = nameMember->get_ref<std::string const&>();
					bool const repeated = std::any_of(parameters.begin(), parameters.end(),
					                                  [&name](auto const& earlier)
					                                  {
														  return earlier.first == name;
													  });
					if (!VisualScriptSerializer::IsValidName(name) || reserved.contains(name) || repeated)
					{
						return std::nullopt;
					}
					parameters.emplace_back(name, ResolveDeclaredType(typeMember->get_ref<std::string const&>()));
				}
				return parameters;
			}

			// Event.Custom: its parameters as outputs after Then, titled by its Name. Event.Send: Execute, Target, the
			// parameters as inputs, Then, titled "Send <Name>".
			void ShapeCustomEvent(Shape& shape, bool send) const
			{
				Json const* name = FindSetting(shape, "Name");
				bool const named =
					name != nullptr && name->is_string() && VisualScriptSerializer::IsValidName(name->get_ref<std::string const&>());
				auto const parameters = ReadParameters(shape);
				if (send)
				{
					shape.Pins.push_back(MakeExecIn());
					Json target = MakeAnalysisPin("Target", VisualScriptPinDirection::In, "Strada.Entity");
					target["DefaultsToSelf"] = true;
					shape.Pins.push_back(std::move(target));
				}
				else
				{
					shape.Pins.push_back(MakeExecOut());
				}
				if (parameters)
				{
					for (auto const& [parameterName, type] : *parameters)
					{
						shape.Pins.push_back(
							MakeDeclaredPin(parameterName, send ? VisualScriptPinDirection::In : VisualScriptPinDirection::Out, type));
					}
				}
				if (send)
				{
					shape.Pins.push_back(MakeExecOut());
				}
				shape.PinsComplete = parameters.has_value();
				if (named)
				{
					shape.Title = send ? "Send " + name->get<std::string>() : name->get<std::string>();
				}
			}

			// Value typed by the Type setting (a type with literals), subtitled by the type.
			void ShapeLiteral(Shape& shape) const
			{
				VisualScriptTypeInfo const* type = ReadTypeSetting(shape, "Type",
				                                                   [](VisualScriptTypeInfo const& candidate)
				                                                   {
																	   return candidate.HasLiteral;
																   });
				shape.Pins.push_back(type != nullptr ? MakeAnalysisPin("Value", VisualScriptPinDirection::Out, type->ID)
				                                     : MakeWildcardPin("Value", VisualScriptPinDirection::Out));
				if (type != nullptr)
				{
					shape.Subtitle = type->Title;
				}
			}

			// Value is a wildcard; Result is typed by To (a type with values), titled "To <type>".
			void ShapeConvert(Shape& shape) const
			{
				VisualScriptTypeInfo const* type = ReadTypeSetting(shape, "To",
				                                                   [](VisualScriptTypeInfo const& candidate)
				                                                   {
																	   return candidate.Kind != VisualScriptTypeKind::Exec;
																   });
				shape.Pins.push_back(MakeWildcardPin("Value", VisualScriptPinDirection::In));
				shape.Pins.push_back(type != nullptr ? MakeAnalysisPin("Result", VisualScriptPinDirection::Out, type->ID)
				                                     : MakeWildcardPin("Result", VisualScriptPinDirection::Out));
				if (type != nullptr)
				{
					shape.Title = "To " + type->Title;
				}
			}

			// Make: the fields of the Type setting's struct as inputs, then Value; Break: Value, then the fields as outputs.
			void ShapeStruct(Shape& shape, bool make) const
			{
				VisualScriptTypeInfo const* type = ReadTypeSetting(shape, "Type",
				                                                   [make](VisualScriptTypeInfo const& candidate)
				                                                   {
																	   return make ? candidate.Makeable : candidate.Breakable;
																   });
				VisualScriptPinDirection const valueDirection = make ? VisualScriptPinDirection::Out : VisualScriptPinDirection::In;
				if (type == nullptr)
				{
					shape.Pins.push_back(MakeWildcardPin("Value", valueDirection));
					shape.PinsComplete = false;
					return;
				}
				if (!make)
				{
					shape.Pins.push_back(MakeAnalysisPin("Value", valueDirection, type->ID));
				}
				for (VisualScriptStructField const& field : type->Fields)
				{
					shape.Pins.push_back(
						MakeAnalysisPin(field.Name, make ? VisualScriptPinDirection::In : VisualScriptPinDirection::Out, field.Type));
				}
				if (make)
				{
					shape.Pins.push_back(MakeAnalysisPin("Value", valueDirection, type->ID));
				}
				shape.Title = (make ? "Make " : "Break ") + type->Title;
			}

			static std::optional<size_t> FindPinIndex(Json const& pins, std::string_view name, VisualScriptPinDirection direction)
			{
				for (size_t index = 0; index < pins.size(); index++)
				{
					Json const& pin = pins[index];
					if (pin["Name"].get_ref<std::string const&>() == name &&
					    pin["Direction"].get_ref<std::string const&>() == GetDirectionName(direction))
					{
						return index;
					}
				}
				return std::nullopt;
			}

			// Each entry replaces the pin of its path, when its type splits (an input of a Makeable type, an output of a
			// Breakable one), by a pin per field, "<path>.<field>", in place. Entries come parents first, so a nested entry
			// splits a field pin of an earlier one.
			void SplitPins(Shape& shape, std::vector<std::string> const& entries, VisualScriptPinDirection direction) const
			{
				for (std::string const& entry : entries)
				{
					std::optional<size_t> const index = FindPinIndex(shape.Pins, entry, direction);
					if (!index)
					{
						continue;
					}
					Json const& pin = shape.Pins[*index];
					VisualScriptTypeInfo const* type = m_Library.FindType(pin["Type"].get_ref<std::string const&>());
					bool const splits = type != nullptr && !type->Fields.empty() &&
					                    (direction == VisualScriptPinDirection::In ? type->Makeable : type->Breakable);
					if (!splits)
					{
						continue;
					}
					Json pins = Json::array();
					for (size_t other = 0; other < shape.Pins.size(); other++)
					{
						if (other != *index)
						{
							pins.push_back(shape.Pins[other]);
							continue;
						}
						for (VisualScriptStructField const& field : type->Fields)
						{
							pins.push_back(MakeAnalysisPin(entry + "." + field.Name, direction, field.Type));
						}
					}
					shape.Pins = std::move(pins);
				}
			}

			// A pin a link or an input names: added to a node whose pins are not complete; an Error UnknownPin for a link that
			// names a pin the node does not have.
			static void FindPin(Shape& shape, VisualScriptPinDirection direction, std::string const& name, VisualScriptLink const* link,
			                    Json& diagnostics)
			{
				if (FindPinIndex(shape.Pins, name, direction))
				{
					return;
				}
				if (!shape.PinsComplete)
				{
					shape.Pins.push_back(MakeWildcardPin(name, direction, false));
					return;
				}
				if (link == nullptr)
				{
					return;
				}
				bool const input = direction == VisualScriptPinDirection::In;
				bool const opposite =
					FindPinIndex(shape.Pins, name, input ? VisualScriptPinDirection::Out : VisualScriptPinDirection::In).has_value();
				VisualScriptDiagnostic diagnostic;
				diagnostic.Code = "UnknownPin";
				diagnostic.Function = shape.Owner != nullptr ? shape.Owner->ID : 0;
				diagnostic.Node = shape.Node->ID;
				diagnostic.Pin = name;
				diagnostic.Direction = direction;
				diagnostic.Link = *link;
				diagnostic.Message =
					fmt::format("{} has no {} pin \"{}\"{}", shape.Title, input ? "input" : "output", name,
				                opposite ? fmt::format(" ({} is an {})", name, input ? "output" : "input") : std::string());
				diagnostics.push_back(SerializeDiagnostic(diagnostic));
			}

			static void AttachLinks(VisualScriptGraph const& graph, std::vector<Shape>& shapes, Json& diagnostics)
			{
				std::unordered_map<uint32_t, size_t> indices;
				for (size_t index = 0; index < shapes.size(); index++)
				{
					indices.emplace(shapes[index].Node->ID, index);
				}
				for (VisualScriptLink const& link : graph.Links)
				{
					auto const from = indices.find(link.From.Node);
					auto const to = indices.find(link.To.Node);
					if (from == indices.end() || to == indices.end())
					{
						continue;
					}
					FindPin(shapes[from->second], VisualScriptPinDirection::Out, link.From.Pin, &link, diagnostics);
					FindPin(shapes[to->second], VisualScriptPinDirection::In, link.To.Pin, &link, diagnostics);
				}
				for (Shape& shape : shapes)
				{
					if (!shape.PinsComplete && shape.Node->Inputs.is_object())
					{
						for (auto const& input : shape.Node->Inputs.items())
						{
							FindPin(shape, VisualScriptPinDirection::In, input.key(), nullptr, diagnostics);
						}
					}
				}
			}

			static Json WriteNode(Shape const& shape)
			{
				Json node = Json::object();
				node["ID"] = shape.Node->ID;
				node["Function"] = shape.Owner != nullptr ? shape.Owner->ID : 0;
				node["Type"] = shape.Node->Type;
				node["Resolved"] = shape.Template != nullptr ? "Exact" : "Unknown";
				node["Title"] = shape.Title;
				if (!shape.Subtitle.empty())
				{
					node["Subtitle"] = shape.Subtitle;
				}
				Json kind = "Unknown";
				if (shape.Template != nullptr)
				{
					auto const found = shape.Template->Source.find("Kind");
					kind = found != shape.Template->Source.end() ? *found : Json("Unknown");
				}
				node["Kind"] = std::move(kind);
				node["Pure"] = shape.Pure;
				node["Latent"] = shape.Latent;
				node["Pins"] = shape.Pins;
				return node;
			}

			VisualScriptLibrary const& m_Library;
			VisualScriptDocument const& m_Document;
			std::set<uint32_t> m_LatentFunctions;
		};
	}

	// The analysis of FixtureVisualScriptSemantics (Detail::FixtureAnalyzer), read as the runtime's analysis is read, with the
	// given diagnostics after its own.
	[[nodiscard]] inline Result<VisualScriptAnalysis>
	AnalyzeWithFixtureTemplates(VisualScriptLibrary const& library, VisualScriptDocument const& document,
	                            std::span<VisualScriptDiagnostic const> extraDiagnostics = {})
	{
		Json analysis = Detail::FixtureAnalyzer(library, document).Analyze();
		for (VisualScriptDiagnostic const& diagnostic : extraDiagnostics)
		{
			analysis["Diagnostics"].push_back(Detail::SerializeDiagnostic(diagnostic));
		}
		return VisualScriptAnalysis::Parse(analysis.dump(-1, ' ', false, Json::error_handler_t::replace));
	}

	// The semantics of the editor's tests: the fixture node library (Editor/Library.json) and its template analyzer, in place
	// of the scripting runtime. A test can stop and start the "runtime", read the library again (a new object, as after a
	// game-assembly change) and add diagnostics to every analysis; it sees how often each was asked for.
	class FixtureVisualScriptSemantics final : public VisualScriptSemantics
	{
	public:
		FixtureVisualScriptSemantics() { ReloadLibrary(); }

		// Null while the runtime is stopped.
		Ref<VisualScriptLibrary const> GetLibrary() override
		{
			m_LibraryCallCount++;
			return m_Available ? m_Library : nullptr;
		}

		// Fails while the runtime is stopped.
		[[nodiscard]] Result<VisualScriptAnalysis> Analyze(VisualScriptDocument const& document) override
		{
			m_AnalyzeCallCount++;
			if (!m_Available)
			{
				return Error{"the scripting runtime is not running"};
			}
			Result<VisualScriptAnalysis> analysis = AnalyzeWithFixtureTemplates(*m_Library, document, m_InjectedDiagnostics);
			if (analysis)
			{
				for (VisualScriptAnalyzedNode& node : analysis.GetValue().Nodes)
				{
					auto const passThrough = m_InjectedPassThroughs.find(node.ID);
					if (passThrough != m_InjectedPassThroughs.end() && IsDisabled(document, node.ID))
					{
						node.PassThrough = passThrough->second;
					}
				}
			}
			return analysis;
		}

		// The runtime stops (GetLibrary gives null, Analyze fails) or starts again.
		void SetAvailable(bool available) { m_Available = available; }
		bool IsAvailable() const { return m_Available; }
		// Reads the library again: GetLibrary then gives a new object.
		void ReloadLibrary()
		{
			Result<Scope<VisualScriptLibrary>> library = VisualScriptLibrary::Parse(GetFixtureLibraryText());
			REQUIRE_MESSAGE(library.IsOk(), library.GetError());
			m_Library = library.TakeValue();
		}

		// Every analysis reports the diagnostic after its own.
		void InjectDiagnostic(VisualScriptDiagnostic diagnostic) { m_InjectedDiagnostics.push_back(std::move(diagnostic)); }
		void ClearInjectedDiagnostics() { m_InjectedDiagnostics.clear(); }
		// Every analysis of a document in which the node is disabled reports the exit the flow takes through it, as the
		// scripting runtime reports a disabled node's pass-through exit (which the template analyzer does not compute).
		void InjectPassThrough(uint32_t node, std::string pin) { m_InjectedPassThroughs[node] = std::move(pin); }

		uint64_t GetLibraryCallCount() const { return m_LibraryCallCount; }
		uint64_t GetAnalyzeCallCount() const { return m_AnalyzeCallCount; }
		// The library, whether or not the runtime runs, without counting a call: for the tests' own lookups.
		VisualScriptLibrary const& GetFixtureLibrary() const { return *m_Library; }

	private:
		// Node IDs are unique in a document, functions' graphs included.
		static bool IsDisabled(VisualScriptDocument const& document, uint32_t id)
		{
			auto const disabled = [id](VisualScriptGraph const& graph)
			{
				return std::any_of(graph.Nodes.begin(), graph.Nodes.end(),
				                   [id](VisualScriptNode const& node)
				                   {
									   return node.ID == id && node.Enabled == VisualScriptNodeEnabled::Disabled;
								   });
			};
			return disabled(document.Graph) || std::any_of(document.Functions.begin(), document.Functions.end(),
			                                               [&disabled](VisualScriptFunction const& function)
			                                               {
															   return disabled(function.Graph);
														   });
		}

		Ref<VisualScriptLibrary const> m_Library;
		std::vector<VisualScriptDiagnostic> m_InjectedDiagnostics;
		std::unordered_map<uint32_t, std::string> m_InjectedPassThroughs;
		uint64_t m_LibraryCallCount = 0;
		uint64_t m_AnalyzeCallCount = 0;
		bool m_Available = true;
	};

	// The canvas' text measurer (width in canvas units of a text of a size in canvas units) for layout tests: every byte is
	// factor times the size wide, so widths are exact and the same with any font. Measurers of different factors have
	// different signatures. The canvas tests use FixedWidthMeasurer(0.5f).
	inline VisualScriptTextMeasurer FixedWidthMeasurer(float factor)
	{
		VisualScriptTextMeasurer measurer;
		measurer.Measure = [factor](std::string_view text, float sizeCu)
		{
			return static_cast<float>(text.size()) * factor * sizeCu;
		};
		measurer.Signature = Hash::Combine(Hash::FNV1a("FixedWidthMeasurer"), std::bit_cast<uint32_t>(factor));
		return measurer;
	}

	// A project made by MakeGraphProject.
	struct GraphProject
	{
		// Open in the AssetManager.
		Ref<Strada::Project> Project;
		// The visual script, registered.
		AssetHandle Graph;
		// Its file.
		std::filesystem::path File;
	};

	// Creates a project in the temporary directory (one per directory), which opens it in the AssetManager (a
	// Testing::AssetManagerScope must keep that initialized), with one visual script at path (relative to the asset
	// directory) whose file holds the document as the editor writes it (unplaced nodes stay unplaced).
	inline GraphProject MakeGraphProject(TemporaryDirectory const& temporary, VisualScriptDocument const& document,
	                                     std::string_view path = "Scripts/Graph.sgraph")
	{
		GraphProject made;
		Result<Ref<Strada::Project>> project = Strada::Project::Create(temporary.GetPath() / "Game", "Game", Scene("Main"));
		REQUIRE_MESSAGE(project.IsOk(), project.GetError());
		made.Project = project.TakeValue();
		made.File = made.Project->GetAssetDirectory() / FileSystem::PathFromUtf8(path);
		REQUIRE(FileSystem::CreateDirectories(made.File.parent_path()).IsOk());
		Result<void> const saved = VisualScriptSerializer::SaveToFile(document, made.File);
		REQUIRE_MESSAGE(saved.IsOk(), saved.GetError());
		Result<AssetHandle> imported = AssetManager::ImportFile(made.File);
		REQUIRE_MESSAGE(imported.IsOk(), imported.GetError());
		made.Graph = imported.GetValue();
		return made;
	}

	// Whether the tests assert wall-clock times: only with STRADA_PERFORMANCE_TESTS=1, as STRADA_FUZZ_SCALE opts into more fuzz
	// rounds, because shared CI runners make sub-second bounds noise. Counters are asserted always.
	inline bool ArePerformanceTestsEnabled()
	{
		std::optional<std::string> const value = Platform::ReadEnvironmentVariable("STRADA_PERFORMANCE_TESTS");
		return value && *value == "1";
	}

	// The screen point of a pin's anchor (where its wires end), from the visual model and the view: tests never search the
	// screen for items. node is a node ID of the model's graph; the pin must exist.
	inline glm::vec2 CanvasPoint(VisualScriptCanvasModel const& model, VisualScriptView const& view, uint32_t node, std::string_view pin,
	                             VisualScriptPinDirection direction = VisualScriptPinDirection::In)
	{
		std::optional<uint32_t> const index = model.FindNode(node);
		REQUIRE_MESSAGE(index.has_value(), "node ", node, " is not on the canvas");
		std::optional<uint32_t> const pinIndex = model.FindPin(*index, pin, direction);
		REQUIRE_MESSAGE(pinIndex.has_value(), "node ", node, " has no pin ", std::string(pin));
		std::span<VisualScriptNodeVisual const> const nodes = model.GetNodes();
		VisualScriptNodeVisual const& visual = nodes[*index];
		return view.ToScreen(VisualScriptGeometry::GetPinAnchor(visual, model.GetPins()[*pinIndex]));
	}

	// The screen point at a fraction of a node's rectangle ((0.5, 0.5) is its middle).
	inline glm::vec2 CanvasPoint(VisualScriptCanvasModel const& model, VisualScriptView const& view, uint32_t node, glm::vec2 fraction)
	{
		std::optional<uint32_t> const index = model.FindNode(node);
		REQUIRE_MESSAGE(index.has_value(), "node ", node, " is not on the canvas");
		VisualScriptRect const rect = VisualScriptGeometry::GetNodeRect(model.GetNodes()[*index]);
		return view.ToScreen(rect.Min + rect.GetSize() * fraction);
	}

	// A large graph for the canvas's performance guards: columns × rows nodes of the fixture library on a 320 × 180 grid, each
	// with linksPerNode links to the nodes of the next column (the last column links back to the one before), and comment
	// boxes around blocks of 2 × 2 nodes. Five node kinds take turns, with stored literals that make value chips: Branch, Log
	// Info (a string), Add (a number, so it keeps its full shape), Delay (a float) and Add Force (a vector and an enum).
	// Every link names pins of the library's templates. Node IDs start at 1, comment IDs follow them.
	inline VisualScriptDocument MakeGridGraph(uint32_t columns, uint32_t rows, uint32_t linksPerNode, uint32_t comments)
	{
		struct Kind
		{
			std::string_view Type;
			std::array<std::string_view, 4> Inputs;
			size_t InputCount;
			std::array<std::string_view, 2> Outputs;
			size_t OutputCount;
		};
		static constexpr std::array<Kind, 5> Kinds = {{
			{"Flow.Branch", {"Execute", "Condition"}, 2, {"True", "False"}, 2},
			{"Call:Strada.Log.Info(string)", {"Execute", "Message"}, 2, {"Then"}, 1},
			{"Operator.Add", {"A", "B"}, 2, {"Result"}, 1},
			{"Flow.Delay", {"Execute", "Duration"}, 2, {"Completed"}, 1},
			{"Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)",
		     {"Execute", "Target", "Force", "Mode"},
		     4,
		     {"Then"},
		     1},
		}};
		auto const getID = [rows](uint32_t column, uint32_t row)
		{
			return 1 + column * rows + row;
		};
		VisualScriptDocument document;
		document.Graph.Nodes.reserve(static_cast<size_t>(columns) * rows);
		for (uint32_t column = 0; column < columns; column++)
		{
			for (uint32_t row = 0; row < rows; row++)
			{
				uint32_t const id = getID(column, row);
				Kind const& kind = Kinds[id % Kinds.size()];
				VisualScriptNode& node = document.Graph.Nodes.emplace_back();
				node.ID = id;
				node.Type = std::string(kind.Type);
				node.Position = glm::vec2(static_cast<float>(column) * 320.0f, static_cast<float>(row) * 180.0f);
				switch (id % Kinds.size())
				{
					case 1:
						node.Inputs["Message"] = "Hello";
						break;
					case 2:
						node.Inputs["B"] = 1;
						break;
					case 3:
						node.Inputs["Duration"] = 0.5;
						break;
					case 4:
						node.Inputs["Force"] = Json::array({1, 0, 0});
						break;
					default:
						break;
				}
			}
		}
		document.Graph.Links.reserve(static_cast<size_t>(columns) * rows * linksPerNode);
		for (uint32_t column = 0; column < columns && columns > 1; column++)
		{
			uint32_t const next = column + 1 < columns ? column + 1 : column - 1;
			for (uint32_t row = 0; row < rows; row++)
			{
				uint32_t const id = getID(column, row);
				Kind const& kind = Kinds[id % Kinds.size()];
				for (uint32_t link = 0; link < linksPerNode; link++)
				{
					uint32_t const target = getID(next, (row + link) % rows);
					Kind const& targetKind = Kinds[target % Kinds.size()];
					std::string_view const from = kind.Outputs[link % kind.OutputCount];
					std::string_view const to = targetKind.Inputs[(link + id) % targetKind.InputCount];
					document.Graph.Links.push_back({{id, std::string(from)}, {target, std::string(to)}});
				}
			}
		}
		uint32_t const blocksPerRow = std::max(columns / 2, 1u);
		for (uint32_t index = 0; index < comments; index++)
		{
			VisualScriptComment& comment = document.Graph.Comments.emplace_back();
			comment.ID = getID(columns, 0) + index;
			comment.Text = "Block " + std::to_string(index);
			uint32_t const blockColumn = (index % blocksPerRow) * 2;
			uint32_t const blockRow = (index / blocksPerRow) * 2;
			comment.Position = glm::vec2(static_cast<float>(blockColumn) * 320.0f - 32.0f, static_cast<float>(blockRow) * 180.0f - 72.0f);
			comment.Size = glm::vec2(584.0f, 400.0f);
		}
		return document;
	}
}
