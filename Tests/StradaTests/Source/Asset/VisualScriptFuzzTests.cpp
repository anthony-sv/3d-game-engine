#include "Fuzzing.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"
#include "VisualScriptRoundTrip.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// One round of a visual-script fuzz case: loads a damaged file (file form) the way the asset manager does, with every
	// asset reference accepted. Nothing when the file is refused; otherwise the document, once it is known to save and
	// load again unchanged and without issues. Every case of this file loads its rounds here.
	std::optional<VisualScriptDocument> LoadDamagedFile(Json const& file)
	{
		std::vector<std::string> warnings;
		DeserializationContext const context = Testing::MakeTolerantContext(warnings);
		Result<VisualScriptDocument> document = VisualScriptSerializer::Deserialize(file, context);
		if (!document)
		{
			return std::nullopt;
		}

		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> reloaded =
			VisualScriptSerializer::Deserialize(VisualScriptSerializer::Serialize(document.GetValue()), context, &issues);
		REQUIRE_MESSAGE(reloaded.IsOk(), reloaded.GetError());
		CHECK_MESSAGE(issues.empty(), (issues.empty() ? std::string() : issues.front().Path + ": " + issues.front().Message));
		CHECK(reloaded.GetValue() == document.GetValue());
		return document.TakeValue();
	}

	// What documents built in memory may hold and no parsed file does: numbers that are not finite or are out of every
	// range, strings and member names that are not UTF-8, values of any shape where the format expects others, and
	// enumerators out of range.
	class InMemoryDamage
	{
	public:
		explicit InMemoryDamage(uint64_t seed)
			: m_Random(seed)
		{
		}

		// Applies 1 to 6 injections, each to an element it adds when the document has none of its kind.
		void Damage(VisualScriptDocument& document)
		{
			for (size_t injection = 0, count = 1 + Pick(6); injection < count; injection++)
			{
				Inject(document);
			}
		}

	private:
		size_t Pick(size_t count) { return m_Random.Pick(count); }

		std::string MakeText()
		{
			constexpr std::array<uint8_t, 16> Interesting = {0x00, 0x09, 0x0A, 0x1F, 0x22, 0x2E, 0x5C, 0x7F,
			                                                 0x80, 0xBF, 0xC0, 0xC3, 0xED, 0xF0, 0xF8, 0xFF};
			std::string text;
			for (size_t length = Pick(9); text.size() < length;)
			{
				switch (Pick(3))
				{
					case 0:
						text.push_back(static_cast<char>(Interesting[Pick(Interesting.size())]));
						break;
					case 1:
						text.push_back(static_cast<char>(m_Random.Next() & 0xFF));
						break;
					default:
						text.append(Pick(2) == 0 ? "A" : "\xC3\xA9");
						break;
				}
			}
			return text;
		}

		double MakeDouble()
		{
			switch (Pick(7))
			{
				case 0:
					return std::numeric_limits<double>::quiet_NaN();
				case 1:
					return Pick(2) == 0 ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
				case 2:
					return std::bit_cast<double>(m_Random.Next());
				case 3:
					return static_cast<double>(static_cast<int64_t>(m_Random.Next() % 2000000000000001) - 1000000000000000);
				case 4:
					return Pick(2) == 0 ? -0.0 : 1.0e16;
				case 5:
					return static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(m_Random.Next())));
				default:
					return static_cast<double>(Pick(1000)) / 8.0;
			}
		}

		glm::vec2 MakeVector()
		{
			// One call after the other: the order of a call's arguments is unspecified.
			float const x = MakeFloat();
			float const y = MakeFloat();
			return glm::vec2(x, y);
		}

		float MakeFloat()
		{
			switch (Pick(4))
			{
				case 0:
					return std::numeric_limits<float>::quiet_NaN();
				case 1:
					return Pick(2) == 0 ? std::numeric_limits<float>::infinity() : 1.0e30f;
				case 2:
					return std::bit_cast<float>(static_cast<uint32_t>(m_Random.Next()));
				default:
					return static_cast<float>(MakeDouble());
			}
		}

		Json MakeValue(int depth = 0)
		{
			switch (Pick(depth < 4 ? 9 : 7))
			{
				case 0:
					return Json();
				case 1:
					return Pick(2) == 0;
				case 2:
					return static_cast<int64_t>(m_Random.Next());
				case 3:
					return m_Random.Next();
				case 4:
				case 5:
					return MakeDouble();
				case 6:
					return MakeText();
				case 7:
				{
					Json array = Json::array();
					for (size_t element = 0, count = Pick(4); element < count; element++)
					{
						array.push_back(MakeValue(depth + 1));
					}
					return array;
				}
				default:
				{
					Json object = Json::object();
					for (size_t member = 0, count = Pick(4); member < count; member++)
					{
						object[MakeText()] = MakeValue(depth + 1);
					}
					return object;
				}
			}
		}

		VisualScriptGraph& PickGraph(VisualScriptDocument& document)
		{
			size_t const graph = Pick(document.Functions.size() + 1);
			return graph == 0 ? document.Graph : document.Functions[graph - 1].Graph;
		}

		VisualScriptNode& PickNode(VisualScriptDocument& document)
		{
			VisualScriptGraph& graph = PickGraph(document);
			if (graph.Nodes.empty())
			{
				graph.Nodes.push_back({});
			}
			return graph.Nodes[Pick(graph.Nodes.size())];
		}

		VisualScriptVariable& PickVariable(VisualScriptDocument& document)
		{
			if (document.Variables.empty())
			{
				document.Variables.push_back({});
			}
			return document.Variables[Pick(document.Variables.size())];
		}

		VisualScriptFunction& PickFunction(VisualScriptDocument& document)
		{
			if (document.Functions.empty())
			{
				document.Functions.push_back({});
			}
			return document.Functions[Pick(document.Functions.size())];
		}

		void Inject(VisualScriptDocument& document)
		{
			switch (Pick(12))
			{
				case 0:
					PickNode(document).Settings = MakeValue();
					break;
				case 1:
					PickNode(document).Inputs = MakeValue();
					break;
				case 2:
					PickVariable(document).Value = MakeValue();
					break;
				case 3:
				{
					VisualScriptFunction& function = PickFunction(document);
					function.Locals.push_back({static_cast<uint32_t>(m_Random.Next()), MakeText(), MakeText(), MakeValue()});
					break;
				}
				case 4:
				{
					VisualScriptNode& node = PickNode(document);
					node.Position = Pick(4) == 0 ? std::nullopt : std::optional<glm::vec2>(MakeVector());
					break;
				}
				case 5:
				{
					VisualScriptGraph& graph = PickGraph(document);
					VisualScriptComment comment;
					comment.ID = static_cast<uint32_t>(m_Random.Next());
					comment.Text = MakeText();
					comment.Position = MakeVector();
					comment.Size = MakeVector();
					glm::vec2 const red = MakeVector();
					glm::vec2 const blue = MakeVector();
					comment.Color = glm::vec4(red, blue);
					graph.Comments.push_back(std::move(comment));
					break;
				}
				case 6:
				{
					VisualScriptVariable& variable = PickVariable(document);
					variable.Range = MakeVector();
					variable.Name = MakeText();
					variable.FormerNames.push_back(MakeText());
					break;
				}
				case 7:
				{
					VisualScriptFunction& function = PickFunction(document);
					function.Name = MakeText();
					std::string name = MakeText();
					std::string type = MakeText();
					function.Inputs.emplace_back(std::move(name), std::move(type), std::vector<std::string>{MakeText()});
					name = MakeText();
					type = MakeText();
					function.Outputs.emplace_back(std::move(name), std::move(type));
					break;
				}
				case 8:
					document.Description = MakeText();
					PickVariable(document).Tooltip = MakeText();
					break;
				case 9:
				{
					VisualScriptNode& node = PickNode(document);
					node.Type = MakeText();
					node.Comment = MakeText();
					node.Split.Inputs.push_back(MakeText());
					node.Split.Outputs.push_back(MakeText());
					break;
				}
				case 10:
				{
					VisualScriptGraph& graph = PickGraph(document);
					graph.Links.push_back({{static_cast<uint32_t>(m_Random.Next()), MakeText()}, {0, MakeText()}});
					break;
				}
				default:
				{
					// Enumerators out of range are written as nlohmann writes their empty names.
					document.Kind = static_cast<VisualScriptDocumentKind>(Pick(3));
					PickNode(document).Enabled = static_cast<VisualScriptNodeEnabled>(Pick(4));
					PickNode(document).Collapsed = Pick(2) == 0;
					break;
				}
			}
		}

		Testing::FuzzRandom m_Random;
	};

	// What documents built in memory may hold that breaks the format's rules across elements, or reaches its limits: repeated
	// IDs, names that clash in their scopes, former names that clash, links to nodes their graph does not have, repeated
	// links, split fields without their parents, one element beyond a limit, invalid UTF-8 in every kind of text, Settings
	// and Inputs that are not objects, and geometry, IDs and enumerators at their bounds and just beyond. Values stay within
	// the format otherwise, so most damage is the only thing wrong with a document.
	class ModelDamage
	{
	public:
		explicit ModelDamage(uint64_t seed)
			: m_Random(seed)
		{
		}

		// Applies 1 to 3 damages.
		void Damage(VisualScriptDocument& document)
		{
			for (size_t count = 1 + Pick(3), damage = 0; damage < count; damage++)
			{
				Inject(document);
			}
		}

	private:
		size_t Pick(size_t count) { return m_Random.Pick(count); }

		uint32_t MakeID(VisualScriptDocument& document)
		{
			uint32_t largest = 0;
			ForEachID(document,
			          [&largest](uint32_t& id)
			          {
						  largest = std::max(largest, id);
					  });
			return largest < VisualScriptSerializer::MaxID ? largest + 1 : 1;
		}

		std::string MakeName() { return fmt::format("Fuzz{}", m_Random.Next() % 100000); }

		// Text that is not UTF-8: truncated, a surrogate, an overlong form or a stray continuation.
		std::string MakeInvalidUtf8()
		{
			constexpr std::array<std::string_view, 4> Invalid = {"\xC3", "\xED\xA0\x80", "\xC0\xAF", "\x80"};
			return std::string("A").append(Invalid[Pick(Invalid.size())]);
		}

		template<typename TFunction>
		static void ForEachGraph(VisualScriptDocument& document, TFunction&& function)
		{
			function(document.Graph);
			for (VisualScriptFunction& owner : document.Functions)
			{
				function(owner.Graph);
			}
		}

		template<typename TFunction>
		static void ForEachID(VisualScriptDocument& document, TFunction&& function)
		{
			for (VisualScriptVariable& variable : document.Variables)
			{
				function(variable.ID);
			}
			for (VisualScriptFunction& owner : document.Functions)
			{
				function(owner.ID);
				for (VisualScriptLocal& local : owner.Locals)
				{
					function(local.ID);
				}
			}
			ForEachGraph(document,
			             [&function](VisualScriptGraph& graph)
			             {
							 for (VisualScriptNode& node : graph.Nodes)
							 {
								 function(node.ID);
							 }
							 for (VisualScriptComment& comment : graph.Comments)
							 {
								 function(comment.ID);
							 }
						 });
		}

		VisualScriptGraph& PickGraph(VisualScriptDocument& document)
		{
			size_t const graph = Pick(document.Functions.size() + 1);
			return graph == 0 ? document.Graph : document.Functions[graph - 1].Graph;
		}

		VisualScriptNode& PickNode(VisualScriptDocument& document)
		{
			VisualScriptGraph& graph = PickGraph(document);
			if (graph.Nodes.empty())
			{
				VisualScriptNode node;
				node.ID = MakeID(document);
				node.Type = "Flow.Branch";
				graph.Nodes.push_back(std::move(node));
			}
			return graph.Nodes[Pick(graph.Nodes.size())];
		}

		VisualScriptVariable& PickVariable(VisualScriptDocument& document)
		{
			if (document.Variables.empty() || Pick(4) == 0)
			{
				VisualScriptVariable variable;
				variable.ID = MakeID(document);
				variable.Name = MakeName();
				variable.Type = "int";
				document.Variables.push_back(std::move(variable));
			}
			return document.Variables[Pick(document.Variables.size())];
		}

		VisualScriptFunction& PickFunction(VisualScriptDocument& document)
		{
			if (document.Functions.empty() || Pick(4) == 0)
			{
				VisualScriptFunction function;
				function.ID = MakeID(document);
				function.Name = MakeName();
				function.Inputs.emplace_back(MakeName(), "float");
				document.Functions.push_back(std::move(function));
			}
			return document.Functions[Pick(document.Functions.size())];
		}

		std::vector<VisualScriptParameter>& PickParameters(VisualScriptDocument& document)
		{
			VisualScriptFunction& function = PickFunction(document);
			std::vector<VisualScriptParameter>& parameters = Pick(2) == 0 ? function.Inputs : function.Outputs;
			if (parameters.empty())
			{
				parameters.emplace_back(MakeName(), "int");
			}
			return parameters;
		}

		VisualScriptLocal& PickLocal(VisualScriptDocument& document)
		{
			VisualScriptFunction& function = PickFunction(document);
			if (function.Locals.empty() || Pick(4) == 0)
			{
				VisualScriptLocal local;
				local.ID = MakeID(document);
				local.Name = MakeName();
				local.Type = "float";
				function.Locals.push_back(std::move(local));
			}
			return function.Locals[Pick(function.Locals.size())];
		}

		VisualScriptComment& PickComment(VisualScriptDocument& document)
		{
			VisualScriptGraph& graph = PickGraph(document);
			if (graph.Comments.empty())
			{
				VisualScriptComment comment;
				comment.ID = MakeID(document);
				graph.Comments.push_back(std::move(comment));
			}
			return graph.Comments[Pick(graph.Comments.size())];
		}

		// A float at a bound, or the float just beyond it.
		float AtBound(float bound, float beyond) { return Pick(2) == 0 ? bound : std::nextafter(bound, beyond); }

		void Inject(VisualScriptDocument& document)
		{
			switch (Pick(12))
			{
				case 0:
				{
					// One element takes another's ID.
					std::vector<uint32_t*> ids;
					ForEachID(document,
					          [&ids](uint32_t& id)
					          {
								  ids.push_back(&id);
							  });
					if (ids.size() >= 2)
					{
						uint32_t const id = *ids[Pick(ids.size())];
						*ids[Pick(ids.size())] = id;
					}
					break;
				}
				case 1:
				{
					// A name another element of its scope has, or a local named as a variable. (Each name is copied before the
					// next pick, which may add an element.)
					switch (Pick(4))
					{
						case 0:
						{
							std::string const name = PickVariable(document).Name;
							PickVariable(document).Name = name;
							break;
						}
						case 1:
						{
							std::string const name = PickVariable(document).Name;
							PickLocal(document).Name = name;
							break;
						}
						case 2:
						{
							std::string const name = PickFunction(document).Name;
							PickFunction(document).Name = name;
							break;
						}
						default:
						{
							std::vector<VisualScriptParameter>& parameters = PickParameters(document);
							std::string name = parameters[Pick(parameters.size())].Name;
							parameters.emplace_back(std::move(name), "int");
							break;
						}
					}
					break;
				}
				case 2:
					// Former names that are current names of their scope, or repeat others of it.
					if (Pick(2) == 0)
					{
						std::string name = Pick(2) == 0 ? PickVariable(document).Name : MakeName();
						PickVariable(document).FormerNames.push_back(std::move(name));
					}
					else
					{
						std::vector<VisualScriptParameter>& parameters = PickParameters(document);
						std::string const name = Pick(2) == 0 ? parameters[Pick(parameters.size())].Name : MakeName();
						parameters[Pick(parameters.size())].FormerNames.push_back(name);
					}
					break;
				case 3:
				{
					// A link to a node of another graph, to no node, or between pins that are no pin names.
					uint32_t const from = PickNode(document).ID;
					uint32_t const to = Pick(4) == 0 ? MakeID(document) : PickNode(document).ID;
					VisualScriptLink link{{from, "Then"}, {to, "Execute"}};
					if (Pick(4) == 0)
					{
						(Pick(2) == 0 ? link.From.Pin : link.To.Pin) = Pick(2) == 0 ? std::string() : MakeInvalidUtf8();
					}
					PickGraph(document).Links.push_back(std::move(link));
					break;
				}
				case 4:
				{
					// A repeated link.
					VisualScriptGraph& graph = PickGraph(document);
					if (!graph.Links.empty())
					{
						graph.Links.push_back(graph.Links[Pick(graph.Links.size())]);
					}
					break;
				}
				case 5:
				{
					// A field without its parent, a repeated path or no pin path.
					VisualScriptNode& node = PickNode(document);
					std::vector<std::string>& paths = Pick(2) == 0 ? node.Split.Inputs : node.Split.Outputs;
					constexpr std::array<std::string_view, 5> Paths = {"Hit", "Hit.Point", "Force.X", "Hit..Point", ".Hit"};
					paths.emplace_back(Paths[Pick(Paths.size())]);
					break;
				}
				case 6:
					// A list at its limit, or one beyond it.
					InjectLimit(document, Pick(2));
					break;
				case 7:
					InjectInvalidText(document);
					break;
				case 8:
				{
					// Settings or Inputs that are no object: written as nothing when empty, else refused by the loader.
					VisualScriptNode& node = PickNode(document);
					Json& members = Pick(2) == 0 ? node.Settings : node.Inputs;
					std::array<Json, 4> const values = {Json(), Json::array(), Json::array({1}), Json("text")};
					members = values[Pick(values.size())];
					break;
				}
				case 9:
				{
					// Geometry at its bounds and just beyond.
					// (One bound after the other: the order of a call's arguments is unspecified.)
					float const coordinate = static_cast<float>(VisualScriptSerializer::MaxCoordinate);
					switch (Pick(4))
					{
						case 0:
						{
							float const x = AtBound(coordinate, 2.0f * coordinate);
							float const y = AtBound(-coordinate, -2.0f * coordinate);
							PickNode(document).Position = glm::vec2(x, y);
							break;
						}
						case 1:
						{
							float const x = AtBound(-coordinate, -2.0f * coordinate);
							float const y = AtBound(coordinate, 2.0f * coordinate);
							PickComment(document).Position = glm::vec2(x, y);
							break;
						}
						case 2:
						{
							float const width = AtBound(static_cast<float>(VisualScriptSerializer::MinCommentSize), 0.0f);
							float const height = AtBound(static_cast<float>(VisualScriptSerializer::MaxCommentSize), 2.0e6f);
							PickComment(document).Size = glm::vec2(width, height);
							break;
						}
						default:
						{
							float const red = AtBound(0.0f, -1.0f);
							float const green = AtBound(1.0f, 2.0f);
							PickComment(document).Color = glm::vec4(red, green, 0.5f, 1.0f);
							break;
						}
					}
					break;
				}
				case 10:
				{
					// An ID at the bounds of its range, or beyond.
					constexpr std::array<uint32_t, 4> IDs = {0, 1, VisualScriptSerializer::MaxID, VisualScriptSerializer::MaxID + 1};
					uint32_t const id = IDs[Pick(IDs.size())];
					switch (Pick(3))
					{
						case 0:
							PickVariable(document).ID = id;
							break;
						case 1:
							PickNode(document).ID = id;
							break;
						default:
							PickComment(document).ID = id;
							break;
					}
					break;
				}
				default:
				{
					// A range at its bounds or reversed, and enumerators in and out of range.
					float const minimum = static_cast<float>(Pick(3));
					float const maximum = static_cast<float>(Pick(3));
					PickVariable(document).Range = glm::vec2(minimum, maximum);
					document.Kind = static_cast<VisualScriptDocumentKind>(Pick(3));
					PickNode(document).Enabled = static_cast<VisualScriptNodeEnabled>(Pick(4));
					break;
				}
			}
		}

		// The limits of lists that small documents reach cheaply: former names, split paths, members, parameters and locals.
		void InjectLimit(VisualScriptDocument& document, size_t beyond)
		{
			switch (Pick(5))
			{
				case 0:
				{
					VisualScriptVariable& variable = PickVariable(document);
					variable.FormerNames.clear();
					for (size_t index = 0; index < VisualScriptSerializer::MaxFormerNames + beyond; index++)
					{
						variable.FormerNames.push_back(fmt::format("Former{}", index));
					}
					break;
				}
				case 1:
				{
					VisualScriptNode& node = PickNode(document);
					node.Split.Outputs.clear();
					for (size_t index = 0; index < VisualScriptSerializer::MaxSplitPins + beyond; index++)
					{
						node.Split.Outputs.push_back(fmt::format("Pin{}", index));
					}
					break;
				}
				case 2:
				{
					VisualScriptNode& node = PickNode(document);
					node.Inputs = Json::object();
					for (size_t index = 0; index < VisualScriptSerializer::MaxSettingsMembers + beyond; index++)
					{
						node.Inputs[fmt::format("Input{}", index)] = static_cast<int64_t>(index);
					}
					break;
				}
				case 3:
				{
					VisualScriptFunction& function = PickFunction(document);
					function.Outputs.clear();
					for (size_t index = 0; index < VisualScriptSerializer::MaxParameters + beyond; index++)
					{
						function.Outputs.emplace_back(fmt::format("Output{}", index), "int");
					}
					break;
				}
				default:
				{
					VisualScriptFunction& function = PickFunction(document);
					function.Locals.clear();
					uint32_t id = MakeID(document);
					for (size_t index = 0; index < VisualScriptSerializer::MaxLocals + beyond && id < VisualScriptSerializer::MaxID;
					     index++)
					{
						VisualScriptLocal local;
						local.ID = id++;
						local.Name = fmt::format("Local{}", index);
						local.Type = "int";
						function.Locals.push_back(std::move(local));
					}
					break;
				}
			}
		}

		// Invalid UTF-8 in one kind of text: names, types, pin names, member names, split paths, long texts and values.
		void InjectInvalidText(VisualScriptDocument& document)
		{
			std::string const text = MakeInvalidUtf8();
			switch (Pick(12))
			{
				case 0:
					PickVariable(document).Name = text;
					break;
				case 1:
					PickVariable(document).Type = text;
					break;
				case 2:
					PickVariable(document).FormerNames.push_back(text);
					break;
				case 3:
					PickFunction(document).Name = text;
					break;
				case 4:
					PickParameters(document).front().Type = text;
					break;
				case 5:
					PickLocal(document).Name = text;
					break;
				case 6:
					PickNode(document).Type = text;
					break;
				case 7:
				{
					VisualScriptNode& node = PickNode(document);
					if (!node.Settings.is_object())
					{
						node.Settings = Json::object();
					}
					node.Settings[text] = 1;
					break;
				}
				case 8:
					PickNode(document).Split.Inputs.push_back(text);
					break;
				case 9:
					PickNode(document).Comment = text;
					break;
				case 10:
					PickComment(document).Text = text;
					break;
				default:
					PickVariable(document).Tooltip = text;
					document.Description = Pick(2) == 0 ? text : std::string();
					break;
			}
		}

		Testing::FuzzRandom m_Random;
	};

	// The text writers' and Validate's seeds, as files: the FeatureTest format sample, the library fixture and the editor's
	// fixture document.
	std::array<Json, 3> LoadWriterSeeds()
	{
		Result<std::string> const libraryText =
			FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Library.sgraph");
		REQUIRE(libraryText.IsOk());
		Result<Json> const library = ParseJson(libraryText.GetValue());
		REQUIRE(library.IsOk());
		Result<std::string> const editorText =
			FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Documents" / "Editor.json");
		REQUIRE(editorText.IsOk());
		Result<Json> const editorContent = ParseJson(editorText.GetValue());
		REQUIRE(editorContent.IsOk());
		std::vector<std::string> warnings;
		Result<VisualScriptDocument> const editor =
			VisualScriptSerializer::DeserializeDocument(editorContent.GetValue(), Testing::MakeTolerantContext(warnings));
		REQUIRE(editor.IsOk());
		return {Testing::LoadFeatureTestDocument("Assets/Scripts/FormatSample.sgraph"), library.GetValue(),
		        VisualScriptSerializer::Serialize(editor.GetValue())};
	}
}

TEST_CASE("VisualScript: damaged files are refused or load and save again")
{
	// The FeatureTest format sample (every key of a script) and the library fixture (Kind, and parameters' former names).
	Result<std::string> const libraryText =
		FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Library.sgraph");
	REQUIRE(libraryText.IsOk());
	Result<Json> const library = ParseJson(libraryText.GetValue());
	REQUIRE(library.IsOk());
	std::array<Json, 2> const seeds = {Testing::LoadFeatureTestDocument("Assets/Scripts/FormatSample.sgraph"), library.GetValue()};
	std::vector<std::string> warnings;
	for (Json const& seed : seeds)
	{
		std::vector<VisualScriptIssue> issues;
		REQUIRE(VisualScriptSerializer::Deserialize(seed, Testing::MakeTolerantContext(warnings), &issues).IsOk());
		REQUIRE(issues.empty());
	}

	// Each seed is damaged as a file of version 3, 2 and 1 in turn, where the keys of later versions it holds are unknown
	// keys: the sample's node keys of version 2 in a file of version 1, the library's Kind and former names in both.
	auto const withVersion = [](Json file, int version)
	{
		file["Strada"]["Version"] = version;
		return file;
	};
	for (auto const& [seed, version] : {std::pair<size_t, int>(0, 1), std::pair<size_t, int>(1, 2), std::pair<size_t, int>(1, 1)})
	{
		std::vector<VisualScriptIssue> issues;
		REQUIRE(
			VisualScriptSerializer::Deserialize(withVersion(seeds[seed], version), Testing::MakeTolerantContext(warnings), &issues).IsOk());
		CHECK_FALSE(issues.empty());
	}

	Testing::DocumentMutator mutator(10000);
	int loaded = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		int const version = VisualScriptSerializer::FormatVersion - (round / 2) % 3;
		if (LoadDamagedFile(mutator.Damage(withVersion(seeds[static_cast<size_t>(round % 2)], version), round)))
		{
			loaded++;
		}
		else
		{
			refused++;
		}
	}
	CHECK(loaded > 0);
	CHECK(refused > 0);
}

TEST_CASE("VisualScript: damaged graphs that load also run")
{
	Json const features = Testing::LoadFeatureTestDocument("Assets/Scripts/Features.sgraph");
	// The editor's fixture document in a file of version 3: disabled, development-only and split nodes.
	Result<std::string> const text =
		FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Documents" / "Editor.json");
	REQUIRE(text.IsOk());
	Result<Json> const content = ParseJson(text.GetValue());
	REQUIRE(content.IsOk());
	std::vector<std::string> warnings;
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> editor =
		VisualScriptSerializer::DeserializeDocument(content.GetValue(), Testing::MakeTolerantContext(warnings), &issues);
	REQUIRE_MESSAGE(editor.IsOk(), (editor ? std::string() : editor.GetError()));
	REQUIRE(issues.empty());
	Json const seeds[] = {features, VisualScriptSerializer::Serialize(editor.GetValue())};
	REQUIRE(seeds[1]["Strada"]["Version"] == 3);
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;

	// Each document that loads runs on an entity of a scene that starts, updates and stops: whatever the runtime makes of
	// it, including failures, nothing crashes or hangs. The seeds take turns.
	Testing::DocumentMutator mutator(10100);
	std::array<int, 2> ran{};
	for (int round = 0, rounds = Testing::GetFuzzRounds(100); round < rounds; round++)
	{
		CAPTURE(round);
		size_t const seed = static_cast<size_t>(round % 2);
		std::optional<VisualScriptDocument> document = LoadDamagedFile(mutator.Damage(seeds[seed], round));
		if (!document)
		{
			continue;
		}
		AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(std::move(*document)), "Damaged");
		{
			Scene scene("Fuzz");
			Entity entity = scene.CreateEntity("Graph");
			entity.AddComponent<VisualScriptComponent>().Graph = graph;
			scene.OnRuntimeStart();
			if (VisualScriptEngine::HasInstance(entity.GetUUID()))
			{
				ran[seed]++;
			}
			for (int frame = 0; frame < 5; frame++)
			{
				scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
			}
			scene.OnRuntimeStop();
		}
		AssetManager::RemoveMemoryAsset(graph);
	}
	CHECK(ran[0] > 0);
	CHECK(ran[1] > 0);
}

TEST_CASE("VisualScript: the text writer writes what the Json writer writes")
{
	std::array<Json, 3> const seeds = LoadWriterSeeds();
	std::vector<std::string> warnings;

	// Both writers, in the file form (pretty) and the content form (compact), against nlohmann's text of the Json ones: for
	// each document that loads, repaired or not, then for the same document with values injected in memory.
	auto const checkWriters = [](VisualScriptDocument const& document)
	{
		CHECK(VisualScriptSerializer::WriteText(document) == DumpJson(VisualScriptSerializer::Serialize(document)));
		CHECK(VisualScriptSerializer::WriteContentText(document) ==
		      VisualScriptSerializer::SerializeDocument(document).dump(-1, ' ', false, Json::error_handler_t::replace));
	};
	Testing::DocumentMutator mutator(10500);
	InMemoryDamage damage(10501);
	int loaded = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		Result<VisualScriptDocument> document = VisualScriptSerializer::Deserialize(
			mutator.Damage(seeds[static_cast<size_t>(round % 3)], round), Testing::MakeTolerantContext(warnings));
		warnings.clear();
		if (!document)
		{
			continue;
		}
		loaded++;
		checkWriters(document.GetValue());
		VisualScriptDocument damaged = document.TakeValue();
		damage.Damage(damaged);
		checkWriters(damaged);
	}
	CHECK(loaded > 0);
}

TEST_CASE("VisualScript: Validate accepts exactly what loads back as written")
{
	// The text writers' seeds; each document that loads, repaired or not, then the same document damaged in its model and,
	// separately, in its values.
	std::array<Json, 3> const seeds = LoadWriterSeeds();
	std::vector<std::string> warnings;
	int accepted = 0;
	int refused = 0;
	auto const check = [&accepted, &refused](VisualScriptDocument const& document)
	{
		std::vector<VisualScriptIssue> issues;
		bool const loadsBack = Testing::LoadsBackAsWritten(document, issues);
		std::optional<VisualScriptIssue> const issue = VisualScriptSerializer::Validate(document);
		CHECK_MESSAGE(issue.has_value() == !loadsBack, (issue ? issue->Path + ": " + issue->Message : std::string("Validate accepts"))
		                                                   << "; the loader: "
		                                                   << (issues.empty() ? std::string("no repair") : issues.front().Path));
		if (issue && !issues.empty())
		{
			// The path is one of the loader's repairs.
			CHECK_MESSAGE(std::any_of(issues.begin(), issues.end(),
			                          [&issue](VisualScriptIssue const& repair)
			                          {
										  return repair.Path == issue->Path;
									  }),
			              issue->Path);
		}
		(issue ? refused : accepted)++;
	};
	Testing::DocumentMutator mutator(10600);
	ModelDamage model(10601);
	InMemoryDamage values(10602);
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		Result<VisualScriptDocument> document = VisualScriptSerializer::Deserialize(
			mutator.Damage(seeds[static_cast<size_t>(round % 3)], round), Testing::MakeTolerantContext(warnings));
		warnings.clear();
		if (!document)
		{
			continue;
		}
		// What loads saves and loads again: always accepted.
		CHECK_FALSE(VisualScriptSerializer::Validate(document.GetValue()).has_value());
		check(document.GetValue());
		VisualScriptDocument damaged = document.GetValue();
		model.Damage(damaged);
		check(damaged);
		VisualScriptDocument injected = document.TakeValue();
		values.Damage(injected);
		check(injected);
	}
	CHECK(accepted > 0);
	CHECK(refused > 0);
}
