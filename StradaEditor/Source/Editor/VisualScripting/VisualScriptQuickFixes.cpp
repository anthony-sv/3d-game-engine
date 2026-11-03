#include "Editor/VisualScripting/VisualScriptQuickFixes.h"

#include "Editor/VisualScripting/VisualScriptCrossGraph.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptLanguageEditing.h"
#include "Editor/VisualScripting/VisualScriptNodeTitles.h"
#include "Editor/VisualScripting/VisualScriptNormalization.h"
#include "Editor/VisualScripting/VisualScriptTimeline.h"

#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <functional>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Strada
{
	namespace
	{
		using Format = VisualScriptSerializer;

		constexpr std::string_view EntryType = "Function.Entry";
		constexpr std::string_view ReturnType = "Function.Return";
		constexpr std::string_view ConvertType = "Convert";
		constexpr std::string_view GetVariableType = "Variable.Get";
		constexpr std::string_view EntityType = "Strada.Entity";
		constexpr std::string_view StringType = "string";
		constexpr std::string_view TypeArgumentsSetting = "TypeArguments";
		constexpr char const* FormulaSetting = "Formula";
		// The settings that name types: Type and To (a type ID), TypeArguments (a list of them) and Parameters (each a Name
		// and a Type), the setting kinds Type, TypeList and Parameters of the node library.
		constexpr std::array<std::string_view, 4> TypeSettings = {"Type", "To", "TypeArguments", "Parameters"};
		// What tells two events of one type apart: a custom event's name, an input event's key or button.
		constexpr std::array<char const*, 3> EventKeySettings = {"Name", "Key", "Button"};
		// Where new nodes stand from the nodes they serve.
		constexpr float NewEndOffset = 320.0f;
		constexpr glm::vec2 EmptyGraphReturn{480.0f, 0.0f};
		constexpr float GetVariableOffset = 240.0f;

		template<typename TDocument>
		struct NodePlace
		{
			using Graph = std::conditional_t<std::is_const_v<TDocument>, VisualScriptGraph const, VisualScriptGraph>;
			using Node = std::conditional_t<std::is_const_v<TDocument>, VisualScriptNode const, VisualScriptNode>;

			Graph* GraphOf = nullptr;
			Node* NodeOf = nullptr;
			// The graph's function: 0 for the event graph.
			uint32_t Function = 0;
		};

		// Node IDs are unique in a document: the node and the graph that holds it.
		template<typename TDocument>
		NodePlace<TDocument> FindNode(TDocument& document, std::optional<uint32_t> id)
		{
			NodePlace<TDocument> place;
			if (!id.has_value())
			{
				return place;
			}
			auto const search = [&](auto& graph, uint32_t function)
			{
				auto const node = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
				                               [id](VisualScriptNode const& candidate)
				                               {
												   return candidate.ID == *id;
											   });
				if (node == graph.Nodes.end())
				{
					return false;
				}
				place.GraphOf = &graph;
				place.NodeOf = &*node;
				place.Function = function;
				return true;
			};
			if (search(document.Graph, 0))
			{
				return place;
			}
			for (auto& function : document.Functions)
			{
				if (search(function.Graph, function.ID))
				{
					break;
				}
			}
			return place;
		}

		template<typename TDocument>
		auto* FindFunction(TDocument& document, uint32_t id)
		{
			auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
			                                [id](VisualScriptFunction const& function)
			                                {
												return function.ID == id;
											});
			return found != document.Functions.end() ? &*found : nullptr;
		}

		template<typename TDocument>
		auto* FindGraph(TDocument& document, uint32_t function)
		{
			using Graph = std::conditional_t<std::is_const_v<TDocument>, VisualScriptGraph const, VisualScriptGraph>;
			if (function == 0)
			{
				return static_cast<Graph*>(&document.Graph);
			}
			auto* owner = FindFunction(document, function);
			return owner != nullptr ? static_cast<Graph*>(&owner->Graph) : static_cast<Graph*>(nullptr);
		}

		VisualScriptVariable* FindVariable(VisualScriptDocument& document, uint32_t id)
		{
			auto const found = std::find_if(document.Variables.begin(), document.Variables.end(),
			                                [id](VisualScriptVariable const& variable)
			                                {
												return variable.ID == id;
											});
			return found != document.Variables.end() ? &*found : nullptr;
		}

		// A local of any function, with its function.
		std::pair<VisualScriptFunction*, VisualScriptLocal*> FindLocal(VisualScriptDocument& document, uint32_t id)
		{
			for (VisualScriptFunction& function : document.Functions)
			{
				auto const found = std::find_if(function.Locals.begin(), function.Locals.end(),
				                                [id](VisualScriptLocal const& local)
				                                {
													return local.ID == id;
												});
				if (found != function.Locals.end())
				{
					return {&function, &*found};
				}
			}
			return {nullptr, nullptr};
		}

		// The name of a variable, local or function of the document; empty when it has none of that ID.
		std::string FindElementName(VisualScriptDocument const& document, uint32_t id, bool function)
		{
			if (function)
			{
				VisualScriptFunction const* found = FindFunction(document, id);
				return found != nullptr ? found->Name : std::string();
			}
			for (VisualScriptVariable const& variable : document.Variables)
			{
				if (variable.ID == id)
				{
					return variable.Name;
				}
			}
			for (VisualScriptFunction const& owner : document.Functions)
			{
				for (VisualScriptLocal const& local : owner.Locals)
				{
					if (local.ID == id)
					{
						return local.Name;
					}
				}
			}
			return {};
		}

		// The name the analysis gives a stored pin, or a field of one, the inverse of VisualScriptEditing::ToStoredVisualScriptPin:
		// "Amount.X" of a pin renamed to Value is "Value.X"; an exact name wins, then the longest renamed pin.
		std::string ToAnalyzedPin(VisualScriptAnalyzedNode const* analyzed, VisualScriptPinDirection direction, std::string const& name)
		{
			if (analyzed == nullptr)
			{
				return name;
			}
			VisualScriptPinRename const* field = nullptr;
			for (VisualScriptPinRename const& rename : analyzed->PinRenames)
			{
				if (rename.Direction != direction)
				{
					continue;
				}
				if (rename.From == name)
				{
					return rename.To;
				}
				bool const isField = name.size() > rename.From.size() && name.starts_with(rename.From) && name[rename.From.size()] == '.';
				if (isField && (field == nullptr || rename.From.size() > field->From.size()))
				{
					field = &rename;
				}
			}
			return field != nullptr ? field->To + name.substr(field->From.size()) : name;
		}

		// The type the analysis gives a stored pin; empty when it has none.
		std::string FindPinType(VisualScriptAnalysis const& analysis, uint32_t node, VisualScriptPinDirection direction,
		                        std::string const& stored)
		{
			VisualScriptAnalyzedNode const* analyzed = analysis.FindNode(node);
			if (analyzed == nullptr)
			{
				return {};
			}
			VisualScriptPin const* pin = analyzed->FindPin(ToAnalyzedPin(analyzed, direction, stored), direction);
			return pin != nullptr ? pin->Type : std::string();
		}

		std::string TitleOfNode(VisualScriptDocument const& document, VisualScriptAnalysis const& analysis, VisualScriptNode const& node,
		                        VisualScriptGraphResolver const& resolver)
		{
			VisualScriptAnalyzedNode const* analyzed = analysis.FindNode(node.ID);
			return analyzed != nullptr && !analyzed->Title.empty() ? analyzed->Title
			                                                       : DeriveVisualScriptNodeTitle(node, document, resolver);
		}

		// The document of the graph a node names, from the graphs; null for a node that names none or when none is at hand.
		VisualScriptDocument const* ResolveTarget(VisualScriptNode const& node, VisualScriptQuickFixGraphs const* graphs)
		{
			std::optional<AssetHandle> const graph = ReadVisualScriptGraphSetting(node);
			return graph && graphs != nullptr && graphs->Resolve ? graphs->Resolve(*graph) : nullptr;
		}

		// A value as compact JSON text, as fixes carry settings in their Argument.
		std::string DumpCompact(Json const& value)
		{
			return value.dump(-1, ' ', false, Json::error_handler_t::replace);
		}

		// An ID a node's setting holds (an integer from 1 to the format's largest ID); none otherwise.
		std::optional<uint32_t> ReadSettingID(VisualScriptNode const& node, char const* key)
		{
			auto const found = node.Settings.is_object() ? node.Settings.find(key) : Json::const_iterator();
			if (!node.Settings.is_object() || found == node.Settings.end() || !found->is_number_integer())
			{
				return std::nullopt;
			}
			int64_t const value = found->is_number_unsigned()
			                          ? static_cast<int64_t>(std::min<uint64_t>(found->get<uint64_t>(), Format::MaxID + 1ull))
			                          : found->get<int64_t>();
			return value >= 1 && value <= static_cast<int64_t>(Format::MaxID) ? std::optional<uint32_t>(static_cast<uint32_t>(value))
			                                                                  : std::nullopt;
		}

		// The name labels give the graph a diagnostic names: its file's stem, else its handle.
		std::string NameOfGraph(AssetHandle graph, VisualScriptQuickFixGraphs const* graphs)
		{
			std::string name = graphs != nullptr && graphs->GetName ? graphs->GetName(graph) : std::string();
			return !name.empty() ? name : graph.ToString();
		}

		// The name of a variable or function of another graph's document; empty when it has none of that ID.
		std::string FindRemoteElementName(VisualScriptDocument const& target, uint32_t id, bool function)
		{
			if (function)
			{
				auto const found = std::find_if(target.Functions.begin(), target.Functions.end(),
				                                [id](VisualScriptFunction const& candidate)
				                                {
													return candidate.ID == id;
												});
				return found != target.Functions.end() ? found->Name : std::string();
			}
			auto const found = std::find_if(target.Variables.begin(), target.Variables.end(),
			                                [id](VisualScriptVariable const& candidate)
			                                {
												return candidate.ID == id;
											});
			return found != target.Variables.end() ? found->Name : std::string();
		}

		// The receiving custom event of a name in a document's event graph: the enabled one, else the first; null when none.
		VisualScriptNode const* FindReceivingEvent(VisualScriptDocument const& target, std::string_view name)
		{
			VisualScriptNode const* receiver = nullptr;
			for (VisualScriptNode const& candidate : target.Graph.Nodes)
			{
				if (candidate.Type != "Event.Custom" || !candidate.Settings.is_object())
				{
					continue;
				}
				auto const found = candidate.Settings.find("Name");
				if (found == candidate.Settings.end() || !found->is_string() || found->get_ref<std::string const&>() != name)
				{
					continue;
				}
				if (receiver == nullptr ||
				    (receiver->Enabled == VisualScriptNodeEnabled::Disabled && candidate.Enabled != VisualScriptNodeEnabled::Disabled))
				{
					receiver = &candidate;
				}
			}
			return receiver;
		}

		std::string TitleOfNodeType(VisualScriptDocument const& document, VisualScriptLibrary const& library, std::string const& type)
		{
			if (VisualScriptNodeTemplate const* node = library.FindNode(type))
			{
				return node->Title;
			}
			VisualScriptNode probe;
			probe.Type = type;
			return DeriveVisualScriptNodeTitle(probe, document);
		}

		std::string TitleOfType(VisualScriptLibrary const& library, std::string const& type)
		{
			VisualScriptTypeInfo const* info = library.FindType(type);
			return info != nullptr ? info->Title : type;
		}

		std::string_view LastSegment(std::string_view text)
		{
			size_t const dot = text.rfind('.');
			return dot == std::string_view::npos ? text : text.substr(dot + 1);
		}

		// A reflected node's type string as its overload reads: "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,
		// Strada.ForceMode)" is "AddForce(Vector3, ForceMode)", "New:Strada.Vector3(float,float,float)" "Vector3(float, float,
		// float)". Types without a signature read as themselves.
		std::string DescribeOverload(std::string_view type)
		{
			size_t const colon = type.find(':');
			size_t const open = type.find('(');
			if (colon == std::string_view::npos || open == std::string_view::npos || open < colon || type.back() != ')')
			{
				return std::string(type);
			}
			std::string_view const member = type.substr(colon + 1, open - colon - 1);
			size_t const angle = member.find('<');
			std::string text(LastSegment(angle == std::string_view::npos ? member : member.substr(0, angle)));
			if (angle != std::string_view::npos)
			{
				text += member.substr(angle);
			}
			text += '(';
			std::string_view parameters = type.substr(open + 1, type.size() - open - 2);
			bool first = true;
			while (!parameters.empty())
			{
				size_t const comma = parameters.find(',');
				std::string_view parameter = parameters.substr(0, comma);
				parameters = comma == std::string_view::npos ? std::string_view() : parameters.substr(comma + 1);
				text += first ? "" : ", ";
				first = false;
				for (std::string_view const modifier : {std::string_view("out "), std::string_view("ref ")})
				{
					if (parameter.starts_with(modifier))
					{
						text += modifier;
						parameter.remove_prefix(modifier.size());
					}
				}
				text += LastSegment(parameter);
			}
			text += ')';
			return text;
		}

		std::optional<uint32_t> ParseID(std::string_view text)
		{
			uint32_t value = 0;
			auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (error != std::errc() || end != text.data() + text.size() || value == 0 || value > Format::MaxID)
			{
				return std::nullopt;
			}
			return value;
		}

		// The template of a node's kind as the analysis resolved it; null when the library has none.
		VisualScriptNodeTemplate const* FindTemplate(VisualScriptLibrary const& library, VisualScriptAnalysis const& analysis,
		                                             VisualScriptNode const& node)
		{
			VisualScriptAnalyzedNode const* analyzed = analysis.FindNode(node.ID);
			if (analyzed != nullptr && !analyzed->CanonicalType.empty())
			{
				if (VisualScriptNodeTemplate const* canonical = library.FindNode(analyzed->CanonicalType))
				{
					return canonical;
				}
			}
			return library.FindNode(node.Type);
		}

		// Of several candidates, the one the message names in quotes ("unknown type \"Strada.Vectr3\""), or alone; none when
		// it cannot tell.
		std::optional<std::string> PickNamed(std::vector<std::string> const& candidates, std::string const& message, bool quoted)
		{
			if (candidates.size() == 1)
			{
				return candidates.front();
			}
			std::optional<std::string> picked;
			for (std::string const& candidate : candidates)
			{
				std::string const needle = quoted ? fmt::format("\"{}\"", candidate) : candidate;
				if (message.find(needle) == std::string::npos)
				{
					continue;
				}
				if (picked.has_value())
				{
					return std::nullopt;
				}
				picked = candidate;
			}
			return picked;
		}

		void AddUnique(std::vector<std::string>& values, std::string value)
		{
			if (std::find(values.begin(), values.end(), value) == values.end())
			{
				values.push_back(std::move(value));
			}
		}

		// The types a node's settings name (Type, To, TypeArguments and the Parameters' types) that the predicate rejects.
		std::vector<std::string> FindRejectedTypes(Json const& settings, std::function<bool(std::string const&)> const& accepts)
		{
			std::vector<std::string> rejected;
			if (!settings.is_object())
			{
				return rejected;
			}
			for (std::string_view const name : TypeSettings)
			{
				auto const value = settings.find(std::string(name));
				if (value == settings.end())
				{
					continue;
				}
				if (value->is_string())
				{
					if (!accepts(value->get<std::string>()))
					{
						AddUnique(rejected, value->get<std::string>());
					}
					continue;
				}
				if (!value->is_array())
				{
					continue;
				}
				for (Json const& element : *value)
				{
					Json const* type = &element;
					if (element.is_object())
					{
						auto const member = element.find("Type");
						type = member != element.end() ? &*member : nullptr;
					}
					if (type != nullptr && type->is_string() && !accepts(type->get<std::string>()))
					{
						AddUnique(rejected, type->get<std::string>());
					}
				}
			}
			return rejected;
		}

		// How many type parameters a reflected generic node's type string has: GetComponent<T>() has 1.
		size_t CountTypeParameters(std::string_view type)
		{
			size_t const open = type.find('<');
			size_t const close = type.find('>', open == std::string_view::npos ? 0 : open);
			if (open == std::string_view::npos || close == std::string_view::npos)
			{
				return 0;
			}
			std::string_view const list = type.substr(open + 1, close - open - 1);
			return static_cast<size_t>(std::count(list.begin(), list.end(), ',')) + 1;
		}

		// The first node of a graph after which an event node of the same type and key is a duplicate.
		VisualScriptNode const* FindFirstEvent(VisualScriptGraph const& graph, VisualScriptNode const& duplicate)
		{
			for (VisualScriptNode const& node : graph.Nodes)
			{
				if (node.ID == duplicate.ID)
				{
					return nullptr;
				}
				if (node.Type != duplicate.Type || !node.Settings.is_object() || !duplicate.Settings.is_object())
				{
					continue;
				}
				bool const sameKey = std::all_of(EventKeySettings.begin(), EventKeySettings.end(),
				                                 [&](char const* key)
				                                 {
													 auto const a = node.Settings.find(key);
													 auto const b = duplicate.Settings.find(key);
													 bool const hasA = a != node.Settings.end();
													 bool const hasB = b != duplicate.Settings.end();
													 return hasA == hasB && (!hasA || *a == *b);
												 });
				if (sameKey)
				{
					return &node;
				}
			}
			return nullptr;
		}

		VisualScriptNode const* FindFirstOfType(VisualScriptGraph const& graph, std::string_view type)
		{
			auto const found = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
			                                [type](VisualScriptNode const& node)
			                                {
												return node.Type == type;
											});
			return found != graph.Nodes.end() ? &*found : nullptr;
		}

		bool IsNumeric(VisualScriptLibrary const& library, std::string_view type)
		{
			VisualScriptTypeInfo const* info = library.FindType(type);
			return info != nullptr && (info->Kind == VisualScriptTypeKind::Integer || info->Kind == VisualScriptTypeKind::Float);
		}

		// Where a value that a diagnostic names is stored.
		enum class ValuePlace : uint8_t
		{
			None = 0,
			// An input's literal (Inputs[stored pin]).
			Input,
			// A setting of the node (a Literal's Value).
			Setting,
			Variable
		};

		ValuePlace FindValuePlace(VisualScriptDocument const& document, VisualScriptAnalysis const& analysis,
		                          VisualScriptDiagnostic const& diagnostic, std::string& key)
		{
			if (diagnostic.Variable.has_value())
			{
				return FindElementName(document, *diagnostic.Variable, false).empty() ? ValuePlace::None : ValuePlace::Variable;
			}
			NodePlace<VisualScriptDocument const> const place = FindNode(document, diagnostic.Node);
			if (place.NodeOf == nullptr)
			{
				return ValuePlace::None;
			}
			if (!diagnostic.Pin.empty())
			{
				key = VisualScriptEditing::ToStoredVisualScriptPin(analysis.FindNode(place.NodeOf->ID), VisualScriptPinDirection::In,
				                                                   diagnostic.Pin);
				return place.NodeOf->Inputs.is_object() && place.NodeOf->Inputs.contains(key) ? ValuePlace::Input : ValuePlace::None;
			}
			// A node's own values: a Literal's Value, the only value setting analysis decodes.
			key = "Value";
			return place.NodeOf->Settings.is_object() && place.NodeOf->Settings.contains(key) ? ValuePlace::Setting : ValuePlace::None;
		}

		// The formula of an Expression with the bytes a diagnostic's Span covers replaced by the fix's argument; nothing when the
		// node stores no formula or the span no longer covers what the fix replaces.
		std::optional<std::string> ReplaceFormulaSpan(VisualScriptNode const& node, VisualScriptDiagnostic const& diagnostic,
		                                              VisualScriptQuickFix const& fix)
		{
			if (!node.Settings.is_object() || !diagnostic.Span)
			{
				return std::nullopt;
			}
			auto const stored = node.Settings.find(FormulaSetting);
			if (stored == node.Settings.end() || !stored->is_string())
			{
				return std::nullopt;
			}
			std::string const& formula = stored->get_ref<std::string const&>();
			auto const [start, end] = *diagnostic.Span;
			if (start > end || end > formula.size() || formula.compare(start, end - start, fix.Replaces) != 0)
			{
				return std::nullopt;
			}
			return formula.substr(0, start) + fix.Argument + formula.substr(end);
		}

		VisualScriptQuickFix MakeFix(std::string label, VisualScriptFixKind kind, uint32_t target, std::string argument = {},
		                             std::string replaces = {})
		{
			VisualScriptQuickFix fix;
			fix.Label = std::move(label);
			fix.Kind = kind;
			fix.Target = target;
			fix.Argument = std::move(argument);
			fix.Replaces = std::move(replaces);
			fix.Navigates = kind == VisualScriptFixKind::GoTo || kind == VisualScriptFixKind::EditSetting ||
			                kind == VisualScriptFixKind::Rename || kind == VisualScriptFixKind::OpenGraph ||
			                kind == VisualScriptFixKind::OpenTimeline;
			return fix;
		}

		// The setting a node diagnostic is about, from its message ("Count: expected ...", "... needs the setting Type: ..."),
		// when the node's kind has it.
		std::string FindNamedSetting(VisualScriptNodeTemplate const* node, std::string const& message)
		{
			if (node == nullptr)
			{
				return {};
			}
			std::string candidate;
			constexpr std::string_view Needs = "needs the setting ";
			if (size_t const needs = message.find(Needs); needs != std::string::npos)
			{
				size_t const start = needs + Needs.size();
				candidate = message.substr(start, message.find(':', start) - start);
			}
			else
			{
				size_t const end = message.find_first_of(":.[ ");
				candidate = message.substr(0, end);
			}
			bool const known = std::any_of(node->Settings.begin(), node->Settings.end(),
			                               [&candidate](VisualScriptSetting const& setting)
			                               {
											   return setting.Name == candidate;
										   });
			return known ? candidate : std::string();
		}

		// Removes the links that equal one, in their graph; false when there is none.
		bool RemoveLink(VisualScriptGraph& graph, VisualScriptLink const& link)
		{
			auto const end = std::remove(graph.Links.begin(), graph.Links.end(), link);
			bool const removed = end != graph.Links.end();
			graph.Links.erase(end, graph.Links.end());
			return removed;
		}

		glm::vec2 PositionOf(VisualScriptNode const& node)
		{
			return node.Position.value_or(glm::vec2(0.0f));
		}

		Result<void> ReplaceValue(VisualScriptDocument& document, VisualScriptDiagnostic const& diagnostic, std::string const& key,
		                          Json value)
		{
			if (diagnostic.Variable.has_value())
			{
				if (VisualScriptVariable* variable = FindVariable(document, *diagnostic.Variable))
				{
					variable->Value = std::move(value);
					return {};
				}
				auto const [owner, local] = FindLocal(document, *diagnostic.Variable);
				if (local == nullptr)
				{
					return MakeError("there is no variable {}", *diagnostic.Variable);
				}
				local->Value = std::move(value);
				return {};
			}
			NodePlace<VisualScriptDocument> const place = FindNode(document, diagnostic.Node);
			if (place.NodeOf == nullptr)
			{
				return MakeError("the document has no node {}", diagnostic.Node.value_or(0));
			}
			Json& values = diagnostic.Pin.empty() ? place.NodeOf->Settings : place.NodeOf->Inputs;
			if (!values.is_object() || !values.contains(key))
			{
				return MakeError("node {} stores no value {}", place.NodeOf->ID, key);
			}
			if (value.is_null())
			{
				values.erase(key);
			}
			else
			{
				values[key] = std::move(value);
			}
			return {};
		}

		Result<void> Apply(VisualScriptDocument& document, VisualScriptAnalysis const& analysis, VisualScriptDiagnostic const& diagnostic,
		                   VisualScriptQuickFix const& fix, VisualScriptQuickFixGraphs const* graphs)
		{
			VisualScriptGraphResolver const resolver = graphs != nullptr ? graphs->Resolve : VisualScriptGraphResolver();
			NodePlace<VisualScriptDocument> const place = FindNode(document, diagnostic.Node);
			auto const requireNode = [&]() -> Result<void>
			{
				if (place.NodeOf == nullptr)
				{
					return MakeError("the document has no node {}", diagnostic.Node.value_or(0));
				}
				return {};
			};
			switch (fix.Kind)
			{
				case VisualScriptFixKind::GoTo:
				case VisualScriptFixKind::EditSetting:
				case VisualScriptFixKind::Rename:
				case VisualScriptFixKind::OpenGraph:
				case VisualScriptFixKind::OpenTimeline:
					return {};
				case VisualScriptFixKind::Retarget:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					Result<Json> settings = ParseJson(fix.Argument);
					if (!settings || !settings.GetValue().is_object())
					{
						return MakeError("the fix's settings are not an object: {}", fix.Argument);
					}
					Result<uint32_t> const retargeted =
						VisualScriptEditing::RetargetVisualScriptNode(document, place.NodeOf->ID, std::move(settings.GetValue()), resolver);
					return retargeted ? Result<void>() : Result<void>(Error{retargeted.GetError()});
				}
				case VisualScriptFixKind::UpdateParameters:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					Result<Json> const list = ParseJson(fix.Argument);
					if (!list || !list.GetValue().is_array())
					{
						return MakeError("the fix's parameters are not a list: {}", fix.Argument);
					}
					std::vector<VisualScriptParameter> parameters;
					for (Json const& entry : list.GetValue())
					{
						auto const name = entry.is_object() ? entry.find("Name") : Json::const_iterator();
						auto const type = entry.is_object() ? entry.find("Type") : Json::const_iterator();
						if (!entry.is_object() || name == entry.end() || type == entry.end() || !name->is_string() || !type->is_string())
						{
							return MakeError("the fix's parameters are not a list of {{ Name, Type }}: {}", fix.Argument);
						}
						parameters.emplace_back(name->get<std::string>(), type->get<std::string>());
					}
					Result<uint32_t> const set = VisualScriptEditing::SetEventParameters(document, place.NodeOf->ID, std::move(parameters));
					return set ? Result<void>() : Result<void>(Error{set.GetError()});
				}
				case VisualScriptFixKind::DeleteVariable:
				{
					if (!diagnostic.Variable.has_value())
					{
						return Error{std::string("the diagnostic names no variable")};
					}
					Result<uint32_t> const removed = VisualScriptEditing::RemoveVariable(document, *diagnostic.Variable, true, nullptr);
					return removed ? Result<void>() : Result<void>(Error{removed.GetError()});
				}
				case VisualScriptFixKind::ChangeNodeType:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					if (!VisualScriptSerializer::IsValidText(fix.Argument, Format::MaxNodeTypeBytes))
					{
						return MakeError("a node type is 1 to {} bytes of text", Format::MaxNodeTypeBytes);
					}
					place.NodeOf->Type = fix.Argument;
					return {};
				}
				case VisualScriptFixKind::UpdateNode:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					VisualScriptAnalysis single;
					if (VisualScriptAnalyzedNode const* analyzed = analysis.FindNode(place.NodeOf->ID))
					{
						single.Nodes.push_back(*analyzed);
					}
					if (ApplyCanonicalNodeTypes(document, single).empty())
					{
						return MakeError("node {} is stored under its type already", place.NodeOf->ID);
					}
					return {};
				}
				case VisualScriptFixKind::UsePin:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					if (!VisualScriptSerializer::IsValidPinPath(fix.Argument))
					{
						return MakeError("a pin name is 1 to {} bytes of text", Format::MaxPinNameBytes);
					}
					if (diagnostic.Link.has_value())
					{
						auto const link = std::find(place.GraphOf->Links.begin(), place.GraphOf->Links.end(), *diagnostic.Link);
						if (link == place.GraphOf->Links.end())
						{
							return Error{"the link is gone"};
						}
						VisualScriptLink renamed = *link;
						(diagnostic.Direction == VisualScriptPinDirection::Out ? renamed.From.Pin : renamed.To.Pin) = fix.Argument;
						place.GraphOf->Links.erase(link);
						if (std::find(place.GraphOf->Links.begin(), place.GraphOf->Links.end(), renamed) == place.GraphOf->Links.end())
						{
							place.GraphOf->Links.push_back(std::move(renamed));
						}
						return {};
					}
					Json& inputs = place.NodeOf->Inputs;
					if (!inputs.is_object() || !inputs.contains(fix.Replaces))
					{
						return MakeError("node {} stores no value {}", place.NodeOf->ID, fix.Replaces);
					}
					if (inputs.contains(fix.Argument))
					{
						return MakeError("node {} stores a value of {} already", place.NodeOf->ID, fix.Argument);
					}
					Json value = std::move(inputs[fix.Replaces]);
					inputs.erase(fix.Replaces);
					inputs[fix.Argument] = std::move(value);
					return {};
				}
				case VisualScriptFixKind::RemoveInput:
				case VisualScriptFixKind::RemoveSetting:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					Json& values = fix.Kind == VisualScriptFixKind::RemoveInput ? place.NodeOf->Inputs : place.NodeOf->Settings;
					if (!values.is_object() || values.erase(fix.Replaces) == 0)
					{
						return MakeError("node {} stores no {} {}", place.NodeOf->ID,
						                 fix.Kind == VisualScriptFixKind::RemoveInput ? "value" : "setting", fix.Replaces);
					}
					return {};
				}
				case VisualScriptFixKind::UseSetting:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					Json& settings = place.NodeOf->Settings;
					if (!VisualScriptSerializer::IsValidText(fix.Argument, Format::MaxPinNameBytes))
					{
						return MakeError("a setting name is 1 to {} bytes of text", Format::MaxPinNameBytes);
					}
					if (!settings.is_object() || !settings.contains(fix.Replaces))
					{
						return MakeError("node {} stores no setting {}", place.NodeOf->ID, fix.Replaces);
					}
					if (settings.contains(fix.Argument))
					{
						return MakeError("node {} stores the setting {} already", place.NodeOf->ID, fix.Argument);
					}
					Json value = std::move(settings[fix.Replaces]);
					settings.erase(fix.Replaces);
					settings[fix.Argument] = std::move(value);
					return {};
				}
				case VisualScriptFixKind::KeepFirstLink:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					bool const exec = diagnostic.Code == "MultipleExecLinks";
					VisualScriptPinReference endpoint;
					if (diagnostic.Link.has_value())
					{
						endpoint = exec ? diagnostic.Link->From : diagnostic.Link->To;
					}
					else
					{
						endpoint = {place.NodeOf->ID, VisualScriptEditing::ToStoredVisualScriptPin(analysis.FindNode(place.NodeOf->ID),
						                                                                           exec ? VisualScriptPinDirection::Out
						                                                                                : VisualScriptPinDirection::In,
						                                                                           diagnostic.Pin)};
					}
					bool first = true;
					size_t const before = place.GraphOf->Links.size();
					std::erase_if(place.GraphOf->Links,
					              [&](VisualScriptLink const& link)
					              {
									  if ((exec ? link.From : link.To) != endpoint)
									  {
										  return false;
									  }
									  return !std::exchange(first, false);
								  });
					if (place.GraphOf->Links.size() == before)
					{
						return MakeError("{} of node {} has one link", endpoint.Pin, endpoint.Node);
					}
					return {};
				}
				case VisualScriptFixKind::BreakLink:
				{
					VisualScriptGraph* graph = FindGraph(document, diagnostic.Function);
					if (!diagnostic.Link.has_value() || graph == nullptr || !RemoveLink(*graph, *diagnostic.Link))
					{
						return Error{"the link is gone"};
					}
					return {};
				}
				case VisualScriptFixKind::InsertConvert:
				{
					VisualScriptGraph* graph = FindGraph(document, diagnostic.Function);
					if (!diagnostic.Link.has_value() || graph == nullptr ||
					    std::find(graph->Links.begin(), graph->Links.end(), *diagnostic.Link) == graph->Links.end())
					{
						return Error{"the link is gone"};
					}
					if (!VisualScriptSerializer::IsValidText(fix.Argument, Format::MaxTypeBytes))
					{
						return MakeError("a type is 1 to {} bytes of text", Format::MaxTypeBytes);
					}
					VisualScriptLink const link = *diagnostic.Link;
					NodePlace<VisualScriptDocument> const from = FindNode(document, link.From.Node);
					NodePlace<VisualScriptDocument> const to = FindNode(document, link.To.Node);
					if (from.NodeOf == nullptr || to.NodeOf == nullptr)
					{
						return Error{"the link is gone"};
					}
					glm::vec2 const position = (PositionOf(*from.NodeOf) + PositionOf(*to.NodeOf)) * 0.5f;
					Result<uint32_t> convert = VisualScriptEditing::AddNode(document, diagnostic.Function, std::string(ConvertType),
					                                                        Json::object({{"To", fix.Argument}}), position);
					if (!convert)
					{
						return Error{convert.GetError()};
					}
					if (graph->Links.size() + 1 > Format::MaxLinksPerGraph)
					{
						return MakeError("a graph holds at most {} links", Format::MaxLinksPerGraph);
					}
					RemoveLink(*graph, link);
					graph->Links.push_back({link.From, {convert.GetValue(), "Value"}});
					graph->Links.push_back({{convert.GetValue(), "Result"}, link.To});
					return {};
				}
				case VisualScriptFixKind::ResetValue:
					return ReplaceValue(document, diagnostic, fix.Replaces, Json());
				case VisualScriptFixKind::UseIdentity:
					return ReplaceValue(document, diagnostic, fix.Replaces, Json::array({0, 0, 0, 1}));
				case VisualScriptFixKind::CreatePublicVariable:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					uint32_t const node = place.NodeOf->ID;
					uint32_t const function = place.Function;
					glm::vec2 const position = PositionOf(*place.NodeOf) - glm::vec2(GetVariableOffset, 0.0f);
					if (!place.NodeOf->Inputs.is_object() || !place.NodeOf->Inputs.contains(fix.Replaces))
					{
						return MakeError("node {} stores no value {}", node, fix.Replaces);
					}
					place.NodeOf->Inputs.erase(fix.Replaces);
					std::string const name = VisualScriptEditing::MakeUniqueVisualScriptName(
						fix.Replaces,
						[&document](std::string_view candidate)
						{
							return VisualScriptEditing::IsVariableNameTaken(document, candidate);
						});
					Result<uint32_t> variable = VisualScriptEditing::AddVariable(document, name, std::string(EntityType));
					if (!variable)
					{
						return Error{variable.GetError()};
					}
					VisualScriptVariable exposed = *FindVariable(document, variable.GetValue());
					exposed.Public = true;
					if (Result<bool> exposedSet = VisualScriptEditing::SetVariable(document, variable.GetValue(), std::move(exposed));
					    !exposedSet)
					{
						return Error{exposedSet.GetError()};
					}
					Result<uint32_t> get = VisualScriptEditing::AddNode(document, function, std::string(GetVariableType),
					                                                    Json::object({{"Variable", variable.GetValue()}}), position);
					if (!get)
					{
						return Error{get.GetError()};
					}
					VisualScriptGraph* graph = FindGraph(document, function);
					std::erase_if(graph->Links,
					              [&](VisualScriptLink const& link)
					              {
									  return link.To.Node == node && link.To.Pin == fix.Replaces;
								  });
					if (graph->Links.size() + 1 > Format::MaxLinksPerGraph)
					{
						return MakeError("a graph holds at most {} links", Format::MaxLinksPerGraph);
					}
					graph->Links.push_back({{get.GetValue(), "Value"}, {node, fix.Replaces}});
					return {};
				}
				case VisualScriptFixKind::UseElement:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					bool const function = diagnostic.Code == "UnknownFunction";
					std::optional<uint32_t> const id = ParseID(fix.Argument);
					if (!IsLocalVisualScriptReference(*place.NodeOf))
					{
						// The element is the other graph's: the node's pins follow it.
						VisualScriptDocument const* const target = ResolveTarget(*place.NodeOf, graphs);
						if (!id.has_value() || target == nullptr || FindRemoteElementName(*target, *id, function).empty())
						{
							return MakeError("the graph the node names has no {} {}", function ? "function" : "variable", fix.Argument);
						}
						Result<uint32_t> const retargeted = VisualScriptEditing::RetargetVisualScriptNode(
							document, place.NodeOf->ID, Json::object({{function ? "Function" : "Variable", *id}}), resolver);
						return retargeted ? Result<void>() : Result<void>(Error{retargeted.GetError()});
					}
					if (!id.has_value() || FindElementName(document, *id, function).empty())
					{
						return MakeError("the document has no {} {}", function ? "function" : "variable", fix.Argument);
					}
					if (!place.NodeOf->Settings.is_object())
					{
						place.NodeOf->Settings = Json::object();
					}
					place.NodeOf->Settings[function ? "Function" : "Variable"] = *id;
					return {};
				}
				case VisualScriptFixKind::UseType:
				{
					if (!VisualScriptSerializer::IsValidText(fix.Argument, Format::MaxTypeBytes))
					{
						return MakeError("a type is 1 to {} bytes of text", Format::MaxTypeBytes);
					}
					if (diagnostic.Variable.has_value())
					{
						if (VisualScriptVariable* variable = FindVariable(document, *diagnostic.Variable))
						{
							VisualScriptVariable changed = *variable;
							changed.Type = fix.Argument;
							Result<bool> set = VisualScriptEditing::SetVariable(document, changed.ID, std::move(changed));
							return set ? Result<void>() : Result<void>(Error{set.GetError()});
						}
						auto const [owner, local] = FindLocal(document, *diagnostic.Variable);
						if (local == nullptr)
						{
							return MakeError("there is no variable {}", *diagnostic.Variable);
						}
						VisualScriptLocal changed = *local;
						changed.Type = fix.Argument;
						Result<bool> set = VisualScriptEditing::SetLocal(document, owner->ID, changed.ID, std::move(changed));
						return set ? Result<void>() : Result<void>(Error{set.GetError()});
					}
					if (place.NodeOf != nullptr)
					{
						Json& settings = place.NodeOf->Settings;
						if (!settings.is_object())
						{
							settings = Json::object();
						}
						if (fix.Replaces.empty())
						{
							settings[std::string(TypeArgumentsSetting)] = Json::array({fix.Argument});
							return {};
						}
						bool replaced = false;
						auto const replace = [&](Json& value)
						{
							if (value.is_string() && value.get_ref<std::string const&>() == fix.Replaces)
							{
								value = fix.Argument;
								replaced = true;
							}
						};
						for (std::string_view const name : TypeSettings)
						{
							auto const value = settings.find(std::string(name));
							if (value == settings.end())
							{
								continue;
							}
							replace(*value);
							if (!value->is_array())
							{
								continue;
							}
							for (Json& element : *value)
							{
								replace(element);
								if (element.is_object())
								{
									if (auto const type = element.find("Type"); type != element.end())
									{
										replace(*type);
									}
								}
							}
						}
						if (!replaced)
						{
							return MakeError("node {} names no type {}", place.NodeOf->ID, fix.Replaces);
						}
						return {};
					}
					VisualScriptFunction* function = FindFunction(document, diagnostic.Function);
					if (function == nullptr)
					{
						return MakeError("the document has no function {}", diagnostic.Function);
					}
					bool replaced = false;
					for (std::vector<VisualScriptParameter>* parameters : {&function->Inputs, &function->Outputs})
					{
						for (VisualScriptParameter& parameter : *parameters)
						{
							if (parameter.Type == fix.Replaces)
							{
								parameter.Type = fix.Argument;
								replaced = true;
							}
						}
					}
					if (!replaced)
					{
						return MakeError("{} has no parameter of the type {}", function->Name, fix.Replaces);
					}
					return {};
				}
				case VisualScriptFixKind::MakePrivate:
				{
					VisualScriptVariable* variable = FindVariable(document, diagnostic.Variable.value_or(0));
					if (variable == nullptr)
					{
						return MakeError("there is no variable {}", diagnostic.Variable.value_or(0));
					}
					variable->Public = false;
					return {};
				}
				case VisualScriptFixKind::DeleteNode:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					uint32_t const node = place.NodeOf->ID;
					Result<VisualScriptRemoval> removal =
						VisualScriptEditing::RemoveElements(document, place.Function, std::span<uint32_t const>(&node, 1), nullptr, false);
					if (!removal)
					{
						return Error{removal.GetError()};
					}
					if (removal.GetValue().Nodes == 0)
					{
						return Error{removal.GetValue().Notes.empty() ? fmt::format("node {} stays", node)
						                                              : removal.GetValue().Notes.front()};
					}
					return {};
				}
				case VisualScriptFixKind::AddEntry:
				case VisualScriptFixKind::AddReturn:
				{
					bool const entry = fix.Kind == VisualScriptFixKind::AddEntry;
					VisualScriptFunction* function = FindFunction(document, fix.Target);
					if (function == nullptr)
					{
						return MakeError("the document has no function {}", fix.Target);
					}
					glm::vec2 position = entry ? glm::vec2(0.0f) : EmptyGraphReturn;
					bool placed = false;
					glm::vec2 minimum(0.0f);
					glm::vec2 maximum(0.0f);
					for (VisualScriptNode const& node : function->Graph.Nodes)
					{
						if (!node.Position.has_value())
						{
							continue;
						}
						minimum = placed ? glm::min(minimum, *node.Position) : *node.Position;
						maximum = placed ? glm::max(maximum, *node.Position) : *node.Position;
						placed = true;
					}
					if (placed)
					{
						position = entry ? glm::vec2(minimum.x - NewEndOffset, minimum.y) : glm::vec2(maximum.x + NewEndOffset, minimum.y);
					}
					Result<uint32_t> added =
						VisualScriptEditing::AddNode(document, fix.Target, std::string(entry ? EntryType : ReturnType), Json(), position);
					return added ? Result<void>() : Result<void>(Error{added.GetError()});
				}
				case VisualScriptFixKind::MakeImpure:
				{
					Result<uint32_t> impure = VisualScriptEditing::SetFunctionPure(document, fix.Target, false);
					return impure ? Result<void>() : Result<void>(Error{impure.GetError()});
				}
				case VisualScriptFixKind::RemoveSplit:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					bool const output = diagnostic.Direction == VisualScriptPinDirection::Out;
					std::vector<std::string>& split = output ? place.NodeOf->Split.Outputs : place.NodeOf->Split.Inputs;
					if (std::find(split.begin(), split.end(), fix.Argument) == split.end())
					{
						return MakeError("node {} does not split {}", place.NodeOf->ID, fix.Argument);
					}
					std::string const fields = fix.Argument + ".";
					auto const underPath = [&](std::string const& pin)
					{
						return pin == fix.Argument || pin.starts_with(fields);
					};
					std::erase_if(split, underPath);
					uint32_t const node = place.NodeOf->ID;
					std::erase_if(place.GraphOf->Links,
					              [&](VisualScriptLink const& link)
					              {
									  VisualScriptPinReference const& end = output ? link.From : link.To;
									  return end.Node == node && end.Pin.starts_with(fields);
								  });
					if (!output && place.NodeOf->Inputs.is_object())
					{
						std::vector<std::string> keys;
						for (auto const& input : place.NodeOf->Inputs.items())
						{
							if (input.key().starts_with(fields))
							{
								keys.push_back(input.key());
							}
						}
						for (std::string const& key : keys)
						{
							place.NodeOf->Inputs.erase(key);
						}
					}
					return {};
				}
				case VisualScriptFixKind::EnableNode:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					uint32_t const node = place.NodeOf->ID;
					return VisualScriptEditing::SetNodeEnabled(document, std::span<uint32_t const>(&node, 1),
					                                           VisualScriptNodeEnabled::Enabled);
				}
				case VisualScriptFixKind::ReplaceInFormula:
				{
					if (Result<void> found = requireNode(); !found)
					{
						return found;
					}
					std::optional<std::string> formula = ReplaceFormulaSpan(*place.NodeOf, diagnostic, fix);
					if (!formula)
					{
						return Error{"the formula no longer holds what the fix replaces"};
					}
					Result<VisualScriptEditing::VisualScriptFormulaChange> set =
						VisualScriptEditing::SetFormula(document, place.NodeOf->ID, std::move(*formula));
					return set ? Result<void>() : Result<void>(Error{set.GetError()});
				}
			}
			return MakeError("unknown fix kind {}", static_cast<int>(fix.Kind));
		}
	}

	std::vector<VisualScriptQuickFix> GetVisualScriptQuickFixes(VisualScriptDocument const& document, VisualScriptAnalysis const& analysis,
	                                                            VisualScriptDiagnostic const& diagnostic,
	                                                            VisualScriptLibrary const& library,
	                                                            VisualScriptQuickFixGraphs const* graphs)
	{
		std::vector<VisualScriptQuickFix> fixes;
		VisualScriptGraphResolver const resolver = graphs != nullptr ? graphs->Resolve : VisualScriptGraphResolver();
		std::string const& code = diagnostic.Code;
		NodePlace<VisualScriptDocument const> const place = FindNode(document, diagnostic.Node);
		VisualScriptNode const* node = place.NodeOf;
		uint32_t const nodeID = node != nullptr ? node->ID : 0;
		VisualScriptAnalyzedNode const* analyzed = node != nullptr ? analysis.FindNode(node->ID) : nullptr;
		VisualScriptFunction const* function = diagnostic.Function != 0 ? FindFunction(document, diagnostic.Function) : nullptr;
		VisualScriptGraph const* graph = FindGraph(document, diagnostic.Function);
		bool const linkExists = diagnostic.Link.has_value() && graph != nullptr &&
		                        std::find(graph->Links.begin(), graph->Links.end(), *diagnostic.Link) != graph->Links.end();

		// A finding about a Flow.Timeline's Timeline setting, of any code (spec-slice4 §16.4): its message names the path in the
		// setting ("Timeline.Tracks[1].Keys[3].Time: ..."), which Details and the Timeline drawer edit.
		if (diagnostic.Setting == VisualScriptTimelineSetting)
		{
			if (node != nullptr && node->Type == VisualScriptTimelineNodeType)
			{
				fixes.push_back(
					MakeFix("Edit in Details", VisualScriptFixKind::EditSetting, nodeID, std::string(VisualScriptTimelineSetting)));
				fixes.push_back(MakeFix("Open Timeline", VisualScriptFixKind::OpenTimeline, nodeID));
			}
			return fixes;
		}

		if (code == "UnknownNodeType" || code == "AmbiguousNode")
		{
			if (node == nullptr)
			{
				return fixes;
			}
			for (std::string const& suggestion : diagnostic.Suggestions)
			{
				std::string const label = code == "UnknownNodeType"
				                              ? fmt::format("Change to {}", TitleOfNodeType(document, library, suggestion))
				                              : fmt::format("Use {}", DescribeOverload(suggestion));
				fixes.push_back(MakeFix(label, VisualScriptFixKind::ChangeNodeType, nodeID, suggestion));
			}
		}
		else if (code == "NodeUpdated")
		{
			if (node != nullptr && analyzed != nullptr && !analyzed->CanonicalType.empty() && node->Type == analyzed->Type &&
			    node->Type != analyzed->CanonicalType)
			{
				fixes.push_back(MakeFix("Update Node", VisualScriptFixKind::UpdateNode, nodeID));
			}
		}
		else if (code == "UnknownPin")
		{
			if (node == nullptr || !diagnostic.Direction.has_value())
			{
				return fixes;
			}
			std::string replaces;
			if (diagnostic.Link.has_value())
			{
				if (!linkExists)
				{
					return fixes;
				}
				replaces = *diagnostic.Direction == VisualScriptPinDirection::Out ? diagnostic.Link->From.Pin : diagnostic.Link->To.Pin;
			}
			else
			{
				replaces = VisualScriptEditing::ToStoredVisualScriptPin(analyzed, VisualScriptPinDirection::In, diagnostic.Pin);
				if (*diagnostic.Direction != VisualScriptPinDirection::In || !node->Inputs.is_object() || !node->Inputs.contains(replaces))
				{
					return fixes;
				}
			}
			for (std::string const& suggestion : diagnostic.Suggestions)
			{
				if (!diagnostic.Link.has_value() && node->Inputs.contains(suggestion))
				{
					continue;
				}
				fixes.push_back(MakeFix(fmt::format("Use {}", suggestion), VisualScriptFixKind::UsePin, nodeID, suggestion, replaces));
			}
		}
		else if (code == "UnusedInput")
		{
			if (node == nullptr || !node->Inputs.is_object())
			{
				return fixes;
			}
			std::string const stored = VisualScriptEditing::ToStoredVisualScriptPin(analyzed, VisualScriptPinDirection::In, diagnostic.Pin);
			if (!node->Inputs.contains(stored))
			{
				return fixes;
			}
			fixes.push_back(MakeFix("Remove Value", VisualScriptFixKind::RemoveInput, nodeID, {}, stored));
			for (std::string const& suggestion : diagnostic.Suggestions)
			{
				if (!node->Inputs.contains(suggestion))
				{
					fixes.push_back(MakeFix(fmt::format("Use {}", suggestion), VisualScriptFixKind::UsePin, nodeID, suggestion, stored));
				}
			}
		}
		else if (code == "UnknownSetting")
		{
			VisualScriptNodeTemplate const* kind = node != nullptr ? FindTemplate(library, analysis, *node) : nullptr;
			if (kind == nullptr || !node->Settings.is_object())
			{
				return fixes;
			}
			std::vector<std::string> unknown;
			for (auto const& setting : node->Settings.items())
			{
				bool const known = std::any_of(kind->Settings.begin(), kind->Settings.end(),
				                               [&setting](VisualScriptSetting const& candidate)
				                               {
												   return candidate.Name == setting.key();
											   });
				if (!known)
				{
					unknown.push_back(setting.key());
				}
			}
			std::optional<std::string> const key = PickNamed(unknown, diagnostic.Message, true);
			if (!key.has_value())
			{
				return fixes;
			}
			fixes.push_back(MakeFix("Remove Setting", VisualScriptFixKind::RemoveSetting, nodeID, {}, *key));
			for (std::string const& suggestion : diagnostic.Suggestions)
			{
				if (!node->Settings.contains(suggestion))
				{
					fixes.push_back(MakeFix(fmt::format("Use {}", suggestion), VisualScriptFixKind::UseSetting, nodeID, suggestion, *key));
				}
			}
		}
		else if (code == "MultipleExecLinks" || code == "MultipleDataLinks")
		{
			if (node != nullptr)
			{
				fixes.push_back(MakeFix("Keep the First Link", VisualScriptFixKind::KeepFirstLink, nodeID));
			}
		}
		else if (code == "TypeMismatch" || code == "EventInputLinked")
		{
			if (!linkExists)
			{
				return fixes;
			}
			fixes.push_back(MakeFix("Break Link", VisualScriptFixKind::BreakLink, diagnostic.Link->To.Node));
			if (code == "TypeMismatch")
			{
				std::string const from =
					FindPinType(analysis, diagnostic.Link->From.Node, VisualScriptPinDirection::Out, diagnostic.Link->From.Pin);
				std::string const to =
					FindPinType(analysis, diagnostic.Link->To.Node, VisualScriptPinDirection::In, diagnostic.Link->To.Pin);
				bool const typed = !from.empty() && !to.empty() && from != VisualScriptExecType && to != VisualScriptExecType &&
				                   from != VisualScriptWildcardType && to != VisualScriptWildcardType && from != to;
				if (typed && (to == StringType || (IsNumeric(library, from) && IsNumeric(library, to))))
				{
					fixes.push_back(MakeFix("Insert Convert", VisualScriptFixKind::InsertConvert, diagnostic.Link->To.Node, to));
				}
			}
		}
		else if (code == "InvalidValue" || code == "ZeroQuaternion" || code == "EntityValue")
		{
			std::string key;
			ValuePlace const where = FindValuePlace(document, analysis, diagnostic, key);
			uint32_t const target = where == ValuePlace::Variable ? *diagnostic.Variable : nodeID;
			if (code == "InvalidValue" && where != ValuePlace::None)
			{
				fixes.push_back(MakeFix("Reset to Default", VisualScriptFixKind::ResetValue, target, {}, key));
			}
			else if (code == "ZeroQuaternion" && where != ValuePlace::None)
			{
				fixes.push_back(MakeFix("Use Identity", VisualScriptFixKind::UseIdentity, target, {}, key));
			}
			else if (code == "EntityValue" && where == ValuePlace::Input)
			{
				fixes.push_back(MakeFix("Create Public Variable", VisualScriptFixKind::CreatePublicVariable, nodeID, {}, key));
			}
		}
		else if (code == "InvalidExpression" || (code == "ReservedName" && diagnostic.Setting == FormulaSetting))
		{
			if (node == nullptr)
			{
				return fixes;
			}
			// Edit Formula names the setting; the diagnostic's span tells the field what to select.
			fixes.push_back(MakeFix("Edit Formula", VisualScriptFixKind::EditSetting, nodeID, std::string(FormulaSetting)));
			if (code != "InvalidExpression" || !diagnostic.Span || !node->Settings.is_object())
			{
				return fixes;
			}
			auto const formula = node->Settings.find(FormulaSetting);
			if (formula == node->Settings.end() || !formula->is_string())
			{
				return fixes;
			}
			std::string const& text = formula->get_ref<std::string const&>();
			auto const [start, end] = *diagnostic.Span;
			if (start >= end || end > text.size())
			{
				return fixes;
			}
			std::string const replaces = text.substr(start, end - start);
			for (std::string const& suggestion : diagnostic.Suggestions)
			{
				fixes.push_back(
					MakeFix(fmt::format("Use {}", suggestion), VisualScriptFixKind::ReplaceInFormula, nodeID, suggestion, replaces));
			}
		}
		else if (code == "InvalidSetting" || code == "MissingSetting" || code == "InvalidCase" || code == "MissingTypeArgument")
		{
			if (node == nullptr)
			{
				return fixes;
			}
			VisualScriptNodeTemplate const* kind = FindTemplate(library, analysis, *node);
			std::string const setting = code == "MissingTypeArgument" ? std::string(TypeArgumentsSetting)
			                            : code == "InvalidCase"       ? std::string("Cases")
			                            : !diagnostic.Setting.empty() ? diagnostic.Setting
			                                                          : FindNamedSetting(kind, diagnostic.Message);
			fixes.push_back(MakeFix("Edit in Details", VisualScriptFixKind::EditSetting, nodeID, setting));
			// A library call naming its own graph: the graph's own functions are called without the setting.
			if (code == "InvalidSetting" && setting == "Graph" && node->Type == "Function.Call" && node->Settings.is_object() &&
			    node->Settings.contains("Graph"))
			{
				fixes.push_back(MakeFix("Remove Setting", VisualScriptFixKind::Retarget, nodeID, R"({"Graph":null})", "Graph"));
			}
			if (code == "MissingTypeArgument" &&
			    CountTypeParameters(analyzed != nullptr && !analyzed->CanonicalType.empty() ? analyzed->CanonicalType : node->Type) == 1)
			{
				for (std::string const& suggestion : diagnostic.Suggestions)
				{
					fixes.push_back(
						MakeFix(fmt::format("Use {}", TitleOfType(library, suggestion)), VisualScriptFixKind::UseType, nodeID, suggestion));
				}
			}
		}
		else if (code == "UnknownVariable" || code == "UnknownFunction")
		{
			if (node == nullptr)
			{
				return fixes;
			}
			// The suggestions of a node naming another graph are that graph's IDs: named from its document, never the local
			// elements of those IDs.
			bool const remote = !IsLocalVisualScriptReference(*node);
			VisualScriptDocument const* const target = remote ? ResolveTarget(*node, graphs) : &document;
			for (std::string const& suggestion : diagnostic.Suggestions)
			{
				std::optional<uint32_t> const id = ParseID(suggestion);
				bool const isFunction = code == "UnknownFunction";
				std::string const name = !id.has_value() || target == nullptr ? std::string()
				                         : remote                             ? FindRemoteElementName(*target, *id, isFunction)
				                                                              : FindElementName(document, *id, isFunction);
				if (!name.empty())
				{
					fixes.push_back(MakeFix(fmt::format("Use {}", name), VisualScriptFixKind::UseElement, nodeID, std::to_string(*id)));
				}
			}
		}
		else if (code == "UnknownGraph" || code == "NotALibrary")
		{
			if (node != nullptr)
			{
				fixes.push_back(MakeFix(code == "NotALibrary" ? "Choose a Library" : "Choose a Graph", VisualScriptFixKind::EditSetting,
				                        nodeID, "Graph"));
			}
		}
		else if (code == "UncheckedGraph")
		{
			if (node != nullptr)
			{
				fixes.push_back(MakeFix("Choose a Graph", VisualScriptFixKind::EditSetting, nodeID, "Graph"));
				fixes.push_back(MakeFix("Send by Name", VisualScriptFixKind::Retarget, nodeID, R"({"Graph":null})", "Graph"));
			}
		}
		else if (code == "UnknownEvent")
		{
			if (node == nullptr)
			{
				return fixes;
			}
			// Each suggestion names an event of the target: "Use" it sets the Name and that event's Parameters.
			if (VisualScriptDocument const* const target = ResolveTarget(*node, graphs))
			{
				for (std::string const& suggestion : diagnostic.Suggestions)
				{
					if (FindReceivingEvent(*target, suggestion) == nullptr)
					{
						continue;
					}
					Result<Json> const parameters = VisualScriptEditing::FindVisualScriptEventParameters(*target, suggestion);
					if (!parameters)
					{
						continue;
					}
					Json const settings = Json::object({{"Name", suggestion}, {"Parameters", parameters.GetValue()}});
					fixes.push_back(
						MakeFix(fmt::format("Use {}", suggestion), VisualScriptFixKind::Retarget, nodeID, DumpCompact(settings), "Name"));
				}
			}
			fixes.push_back(MakeFix("Choose a Graph", VisualScriptFixKind::EditSetting, nodeID, "Graph"));
		}
		else if (code == "EventSignatureMismatch")
		{
			VisualScriptDocument const* const target = node != nullptr ? ResolveTarget(*node, graphs) : nullptr;
			std::optional<AssetHandle> const named = node != nullptr ? ReadVisualScriptGraphSetting(*node) : std::nullopt;
			auto const name = node != nullptr && node->Settings.is_object() ? node->Settings.find("Name") : Json::const_iterator();
			if (target != nullptr && named && name != node->Settings.end() && name->is_string())
			{
				if (Result<Json> const parameters = VisualScriptEditing::FindVisualScriptEventParameters(*target, name->get<std::string>()))
				{
					fixes.push_back(MakeFix(fmt::format("Update Parameters from {}", NameOfGraph(*named, graphs)),
					                        VisualScriptFixKind::UpdateParameters, nodeID, DumpCompact(parameters.GetValue()),
					                        "Parameters"));
				}
			}
		}
		else if (code == "DisabledTarget")
		{
			VisualScriptDocument const* const target = node != nullptr ? ResolveTarget(*node, graphs) : nullptr;
			auto const name = node != nullptr && node->Settings.is_object() ? node->Settings.find("Name") : Json::const_iterator();
			if (target != nullptr && name != node->Settings.end() && name->is_string())
			{
				if (VisualScriptNode const* const event = FindReceivingEvent(*target, name->get<std::string>()))
				{
					fixes.push_back(MakeFix(fmt::format("Open {} in {}", name->get<std::string>(), NameOfGraph(diagnostic.Graph, graphs)),
					                        VisualScriptFixKind::OpenGraph, event->ID, "Node"));
				}
			}
		}
		else if (code == "PrivateVariable")
		{
			VisualScriptDocument const* const target = node != nullptr ? ResolveTarget(*node, graphs) : nullptr;
			std::optional<uint32_t> const id = node != nullptr ? ReadSettingID(*node, "Variable") : std::nullopt;
			std::string const name = target != nullptr && id ? FindRemoteElementName(*target, *id, false) : std::string();
			if (!name.empty())
			{
				fixes.push_back(MakeFix(fmt::format("Open {} in {}", name, NameOfGraph(diagnostic.Graph, graphs)),
				                        VisualScriptFixKind::OpenGraph, *id, "Variable"));
			}
		}
		else if (code == "ReferenceError" || code == "LibraryCycle")
		{
			if (diagnostic.Graph.IsValid())
			{
				fixes.push_back(MakeFix(fmt::format("Open {}", NameOfGraph(diagnostic.Graph, graphs)), VisualScriptFixKind::OpenGraph, 0,
				                        code == "ReferenceError" ? "Problems" : ""));
			}
		}
		else if (code == "LibraryVariable")
		{
			if (diagnostic.Variable.has_value() && !FindElementName(document, *diagnostic.Variable, false).empty())
			{
				fixes.push_back(MakeFix("Delete Variable", VisualScriptFixKind::DeleteVariable, *diagnostic.Variable));
			}
		}
		else if (code == "LibraryEventGraph")
		{
			if (node != nullptr)
			{
				fixes.push_back(MakeFix("Delete This Node", VisualScriptFixKind::DeleteNode, nodeID));
			}
		}
		else if (code == "UnknownType" || code == "InvalidType")
		{
			bool const unknown = code == "UnknownType";
			// Arrays are types of private variables, locals and parameters, but not of public variables, which components set.
			bool const publicVariable =
				diagnostic.Variable.has_value() && std::any_of(document.Variables.begin(), document.Variables.end(),
			                                                   [&diagnostic](VisualScriptVariable const& variable)
			                                                   {
																   return variable.ID == *diagnostic.Variable && variable.Public;
															   });
			auto const isArray = [&library](std::string const& type)
			{
				VisualScriptTypeInfo const* info = library.FindType(type);
				return info != nullptr && info->Kind == VisualScriptTypeKind::Array;
			};
			auto const rejects = [&](std::string const& type)
			{
				return unknown ? library.FindType(type) == nullptr
				               : type == VisualScriptExecType || type == VisualScriptWildcardType || (publicVariable && isArray(type));
			};
			std::vector<std::string> rejected;
			uint32_t target = 0;
			if (diagnostic.Variable.has_value())
			{
				target = *diagnostic.Variable;
				for (VisualScriptVariable const& variable : document.Variables)
				{
					if (variable.ID == target && rejects(variable.Type))
					{
						rejected.push_back(variable.Type);
					}
				}
				for (VisualScriptFunction const& owner : document.Functions)
				{
					for (VisualScriptLocal const& local : owner.Locals)
					{
						if (local.ID == target && rejects(local.Type))
						{
							rejected.push_back(local.Type);
						}
					}
				}
			}
			else if (node != nullptr)
			{
				target = nodeID;
				rejected = FindRejectedTypes(node->Settings,
				                             [&](std::string const& type)
				                             {
												 return !rejects(type);
											 });
			}
			else if (function != nullptr)
			{
				target = function->ID;
				for (std::vector<VisualScriptParameter> const* parameters : {&function->Inputs, &function->Outputs})
				{
					for (VisualScriptParameter const& parameter : *parameters)
					{
						if (rejects(parameter.Type))
						{
							AddUnique(rejected, parameter.Type);
						}
					}
				}
			}
			std::optional<std::string> const replaces = PickNamed(rejected, diagnostic.Message, unknown);
			if (!replaces.has_value())
			{
				return fixes;
			}
			for (std::string const& suggestion : diagnostic.Suggestions)
			{
				if (publicVariable && isArray(suggestion))
				{
					continue;
				}
				fixes.push_back(MakeFix(fmt::format("Use {}", TitleOfType(library, suggestion)), VisualScriptFixKind::UseType, target,
				                        suggestion, *replaces));
			}
		}
		else if (code == "NotExposable")
		{
			bool const isPublic = std::any_of(document.Variables.begin(), document.Variables.end(),
			                                  [&diagnostic](VisualScriptVariable const& variable)
			                                  {
												  return diagnostic.Variable == variable.ID && variable.Public;
											  });
			if (isPublic)
			{
				fixes.push_back(MakeFix("Make Private", VisualScriptFixKind::MakePrivate, *diagnostic.Variable));
			}
		}
		else if (code == "InvalidName" || code == "ReservedName")
		{
			uint32_t const target = node != nullptr                   ? nodeID
			                        : diagnostic.Variable.has_value() ? *diagnostic.Variable
			                        : function != nullptr             ? function->ID
			                                                          : 0;
			if (target != 0)
			{
				fixes.push_back(MakeFix("Rename", VisualScriptFixKind::Rename, target));
			}
		}
		else if (code == "DuplicateEvent")
		{
			if (node == nullptr)
			{
				return fixes;
			}
			if (VisualScriptNode const* first = FindFirstEvent(*place.GraphOf, *node))
			{
				fixes.push_back(MakeFix(fmt::format("Go to the First {}", TitleOfNode(document, analysis, *first, resolver)),
				                        VisualScriptFixKind::GoTo, first->ID));
			}
			fixes.push_back(MakeFix("Delete This Node", VisualScriptFixKind::DeleteNode, nodeID));
		}
		else if (code == "EventInFunction" || code == "TickInFunction" || code == "NotInFunction")
		{
			if (node != nullptr)
			{
				fixes.push_back(MakeFix("Delete This Node", VisualScriptFixKind::DeleteNode, nodeID));
			}
		}
		else if (code == "MissingEntry" || code == "DuplicateEntry" || code == "MissingReturn" || code == "DuplicateReturn")
		{
			if (function == nullptr)
			{
				return fixes;
			}
			bool const entry = code == "MissingEntry" || code == "DuplicateEntry";
			VisualScriptNode const* first = FindFirstOfType(function->Graph, entry ? EntryType : ReturnType);
			if (first == nullptr)
			{
				fixes.push_back(MakeFix(entry ? "Add Entry" : "Add Return",
				                        entry ? VisualScriptFixKind::AddEntry : VisualScriptFixKind::AddReturn, function->ID));
			}
			else if (code == "DuplicateEntry" || code == "DuplicateReturn")
			{
				fixes.push_back(MakeFix(entry ? "Go to the First Entry" : "Go to the First Return", VisualScriptFixKind::GoTo, first->ID));
			}
		}
		else if (code == "LatentInPureContext" || code == "ImpureInPureFunction")
		{
			if (function != nullptr && function->Pure)
			{
				fixes.push_back(MakeFix(fmt::format("Make {} Impure", function->Name), VisualScriptFixKind::MakeImpure, function->ID));
			}
		}
		else if (code == "UnusedSplit")
		{
			if (node == nullptr || !diagnostic.Direction.has_value())
			{
				return fixes;
			}
			bool const output = *diagnostic.Direction == VisualScriptPinDirection::Out;
			std::vector<std::string> const& split = output ? node->Split.Outputs : node->Split.Inputs;
			// The analysis names the entry exactly as the Split list stores it, also when a redirect renamed the pin it starts
			// with: mapping it through the renames would pick another entry when one equals a renamed pin's new name.
			if (std::find(split.begin(), split.end(), diagnostic.Pin) != split.end())
			{
				fixes.push_back(MakeFix("Remove Split", VisualScriptFixKind::RemoveSplit, nodeID, diagnostic.Pin));
			}
		}
		else if (code == "DisabledIgnored")
		{
			if (node != nullptr && node->Enabled != VisualScriptNodeEnabled::Enabled)
			{
				fixes.push_back(MakeFix("Enable Node", VisualScriptFixKind::EnableNode, nodeID));
			}
		}
		return fixes;
	}

	Result<void> ApplyVisualScriptQuickFix(VisualScriptDocument& document, VisualScriptAnalysis const& analysis,
	                                       VisualScriptDiagnostic const& diagnostic, VisualScriptQuickFix const& fix,
	                                       VisualScriptQuickFixGraphs const* graphs)
	{
		VisualScriptDocument edited = document;
		Result<void> applied = Apply(edited, analysis, diagnostic, fix, graphs);
		if (applied)
		{
			document = std::move(edited);
		}
		return applied;
	}
}
