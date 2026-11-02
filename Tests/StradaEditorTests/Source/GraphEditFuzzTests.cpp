#include "Fuzzing.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"
#include "VisualScriptTestDocuments.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/VisualScripting/VisualScriptBatch.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// A project open in an editor model with the editor's commands, as the editor layer sets them up.
	struct EditFuzzFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		CommandRegistry Registry;

		EditFuzzFixture()
		{
			REQUIRE(RegisterEditorCommands(Registry, Operations, EditorCommandEnvironment()).IsOk());
			REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
		}

		CommandResult Run(std::string_view name, Json params)
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			return std::move(*result);
		}

		Json Ok(std::string_view name, Json params)
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
			return result.TakeValue();
		}

		std::string ReadFile(std::string_view path) const
		{
			Result<Buffer> bytes = FileSystem::ReadBinaryFile(AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(path));
			REQUIRE(bytes.IsOk());
			return std::string(bytes.GetValue().As<char>(), static_cast<size_t>(bytes.GetValue().GetSize()));
		}
	};

	// Builds graph.edit batches from what a document holds: its sub-graphs' nodes, links and comment boxes, the pins its
	// analysis gives each node, its variables, locals and functions, the quick fixes of its diagnostics and a clipboard text,
	// with labels for what the batch creates. Every operation kind is drawn; one operation in twelve is damaged (a field the
	// operation does not take, a label used before it is defined, an ID an earlier operation deleted, an element that does not
	// exist), so failures name their operations too.
	class BatchGenerator
	{
	public:
		BatchGenerator(Testing::FuzzRandom& random, VisualScriptDocument const& document, Json const& nodes, Json const& diagnostics,
		               std::string const& clipboard)
			: m_Random(random),
			  m_Document(document),
			  m_Nodes(nodes),
			  m_Diagnostics(diagnostics),
			  m_Clipboard(clipboard)
		{
			for (VisualScriptNode const& node : document.Graph.Nodes)
			{
				m_Types.insert(node.Type);
			}
			for (VisualScriptFunction const& function : document.Functions)
			{
				for (VisualScriptNode const& node : function.Graph.Nodes)
				{
					m_Types.insert(node.Type);
				}
			}
			m_Types.insert({"Flow.Branch", "Flow.Sequence", "Flow.Switch", "Operator.Add", "Event.Custom", "Variable.Get",
			                "Call:Strada.Log.Info(string)", "Flow.Brnach", "Expression", "Array.Make"});
		}

		// The parameters of one call: graph is the caller's.
		Json Generate(std::map<std::string, size_t>& kinds)
		{
			m_Function = 0;
			if (!m_Document.Functions.empty() && m_Random.Pick(3) == 0)
			{
				m_Function = m_Document.Functions[m_Random.Pick(m_Document.Functions.size())].ID;
			}
			m_ElementLabels.clear();
			m_VariableLabels.clear();
			m_FunctionLabels.clear();
			m_Deleted.clear();
			std::span<std::string_view const> const operations = GetVisualScriptBatchOperations();
			Json batch = Json::array();
			for (size_t count = 1 + m_Random.Pick(6); count > 0; count--)
			{
				std::string_view const kind = operations[m_Random.Pick(operations.size())];
				kinds[std::string(kind)]++;
				Json operation = Operation(kind);
				if (m_Random.Pick(12) == 0)
				{
					Damage(operation);
				}
				batch.push_back(std::move(operation));
			}
			Json params = Json::object({{"operations", std::move(batch)}});
			if (m_Function != 0 || m_Random.Pick(4) == 0)
			{
				params["function"] = m_Function;
			}
			return params;
		}

	private:
		Json Operation(std::string_view kind)
		{
			Json operation = Json::object({{"op", kind}});
			if (kind == "AddNode")
			{
				std::string const type = Pick(std::vector<std::string>(m_Types.begin(), m_Types.end()));
				operation["type"] = type;
				if (m_Random.Pick(2) == 0)
				{
					operation["position"] = Point();
				}
				if (type == "Variable.Get" && !m_Document.Variables.empty())
				{
					operation["settings"] = Json::object({{"Variable", Pick(m_Document.Variables).Name}});
				}
				if (type == "Call:Strada.Log.Info(string)")
				{
					operation["literals"] = Json::object({{"Message", "Fuzz"}});
				}
				if (type == "Expression")
				{
					operation["settings"] = Json::object({{"Formula", Formula()}});
				}
				if (type == "Array.Make" && m_Random.Pick(2) == 0)
				{
					operation["settings"] = Json::object({{"Type", Pick(s_ValueTypes)}, {"Count", m_Random.Pick(4)}});
				}
				Label(operation, "as", m_ElementLabels);
			}
			else if (kind == "Delete")
			{
				operation["elements"] = Elements(1 + m_Random.Pick(3), true);
				for (Json const& element : operation["elements"])
				{
					if (element.is_number_unsigned())
					{
						m_Deleted.push_back(element.get<uint32_t>());
					}
				}
				if (m_Random.Pick(3) == 0)
				{
					operation["bridge"] = false;
				}
			}
			else if (kind == "Duplicate")
			{
				operation["elements"] = Elements(1 + m_Random.Pick(3), true);
				if (m_Random.Pick(2) == 0)
				{
					operation["labels"] = Json::array({NewLabel(m_ElementLabels)});
				}
			}
			else if (kind == "Paste")
			{
				std::string clipboard = m_Clipboard;
				if (!clipboard.empty() && m_Random.Pick(2) == 0)
				{
					clipboard.resize(m_Random.Pick(clipboard.size()));
				}
				operation["clipboard"] = clipboard;
				if (m_Random.Pick(2) == 0)
				{
					operation["at"] = Point();
				}
			}
			else if (kind == "Move")
			{
				operation["elements"] = Elements(1 + m_Random.Pick(3), true);
				operation["offset"] = Point();
			}
			else if (kind == "SetPosition")
			{
				operation["element"] = Element(true);
				operation["position"] = Point();
			}
			else if (kind == "Connect")
			{
				Json const node = Node();
				operation["from"] = Json::array({node, PinOf(node, "Out")});
				Json const other = Node();
				operation["to"] = Json::array({other, PinOf(other, "In")});
			}
			else if (kind == "Disconnect")
			{
				Json const link = Link();
				size_t const form = m_Random.Pick(4);
				if (form == 0 || form == 1)
				{
					operation["from"] = link["from"];
				}
				if (form == 0 || form == 2)
				{
					operation["to"] = link["to"];
				}
				if (form == 3)
				{
					operation["node"] = Node();
				}
			}
			else if (kind == "Relink")
			{
				operation["remove"] = Json::array({Link()});
				Json const node = Node();
				Json const other = Node();
				operation["add"] = Json::array(
					{Json::object({{"from", Json::array({node, PinOf(node, "Out")})}, {"to", Json::array({other, PinOf(other, "In")})}})});
			}
			else if (kind == "Splice")
			{
				operation["node"] = Node();
				operation["link"] = Link();
			}
			else if (kind == "InsertReroute")
			{
				operation["link"] = Link();
				operation["position"] = Point();
				Label(operation, "as", m_ElementLabels);
			}
			else if (kind == "SetInput")
			{
				Json const node = Node();
				operation["node"] = node;
				operation["pin"] = PinOf(node, "In");
				operation["value"] = Value();
			}
			else if (kind == "SetSetting")
			{
				operation["node"] = Node();
				std::string const name =
					Pick(std::vector<std::string>{"Count", "Cases", "Variable", "Function", "Name", "Parameters", "Type", "Formula"});
				operation["name"] = name;
				if (name == "Count")
				{
					operation["value"] = 2 + m_Random.Pick(4);
				}
				else if (name == "Formula")
				{
					operation["value"] = Formula();
				}
				else if (name == "Variable" && !m_Document.Variables.empty())
				{
					operation["value"] = Pick(m_Document.Variables).Name;
				}
				else if (name == "Function" && !m_Document.Functions.empty())
				{
					operation["value"] = Pick(m_Document.Functions).Name;
				}
				else
				{
					operation["value"] = Value();
				}
			}
			else if (kind == "SetSequenceOutputs" || kind == "SetArrayElements")
			{
				operation["node"] = Node();
				operation["count"] = 2 + m_Random.Pick(4);
				if (m_Random.Pick(2) == 0)
				{
					operation[m_Random.Pick(2) == 0 ? "insertAt" : "removeAt"] = m_Random.Pick(3);
				}
			}
			else if (kind == "SetFormula")
			{
				operation["node"] = Node();
				operation["formula"] = Formula();
			}
			else if (kind == "SetSwitchCases")
			{
				operation["node"] = Node();
				operation["cases"] = m_Random.Pick(4) == 0 ? Json() : Json::array({1, 2, 3});
			}
			else if (kind == "SetEventParameters")
			{
				operation["node"] = Node();
				operation["parameters"] = Json::array({Json::object({{"name", "Amount"}, {"type", Pick(s_ValueTypes)}})});
			}
			else if (kind == "RenameCustomEvent")
			{
				operation["node"] = Node();
				operation["name"] = Pick(std::vector<std::string>{"Hit", "Fuzzed", "Hit2"});
			}
			else if (kind == "SetNodeComment")
			{
				operation["nodes"] = Elements(1 + m_Random.Pick(2), false);
				operation["text"] = m_Random.Pick(3) == 0 ? "" : "Fuzzed";
			}
			else if (kind == "SetCollapsed")
			{
				operation["nodes"] = Elements(1 + m_Random.Pick(2), false);
				operation["collapsed"] = m_Random.Pick(2) == 0;
			}
			else if (kind == "SetEnabled")
			{
				operation["nodes"] = Elements(1 + m_Random.Pick(2), false);
				operation["enabled"] = Pick(std::vector<std::string>{"Enabled", "Disabled", "DevelopmentOnly"});
			}
			else if (kind == "SplitPin" || kind == "RecombinePin" || kind == "PromoteToVariable" || kind == "PromoteToFunctionInput")
			{
				Json const node = Node();
				std::string const direction = m_Random.Pick(2) == 0 ? "In" : "Out";
				operation["node"] = node;
				operation["pin"] = PinOf(node, direction);
				if (kind != "PromoteToFunctionInput")
				{
					operation["direction"] = direction;
				}
				if (kind == "PromoteToVariable")
				{
					Label(operation, "as", m_ElementLabels);
				}
				if (kind.starts_with("Promote") && m_Random.Pick(2) == 0)
				{
					operation["name"] = "Promoted";
				}
			}
			else if (kind == "AddComment")
			{
				operation["text"] = "Fuzzed";
				if (m_Random.Pick(2) == 0)
				{
					operation["elements"] = Elements(1 + m_Random.Pick(3), true);
				}
				else
				{
					operation["position"] = Point();
				}
				if (m_Random.Pick(3) == 0)
				{
					operation["size"] = Json::array({320.0, 160.0});
				}
				Label(operation, "as", m_ElementLabels);
			}
			else if (kind == "SetComment")
			{
				operation["comment"] = Comment();
				operation["text"] = "Fuzzed";
				if (m_Random.Pick(2) == 0)
				{
					operation["color"] = Json::array({0.5, 0.25, 0.0, 1.0});
				}
			}
			else if (kind == "SetDescription")
			{
				operation["description"] = m_Random.Pick(2) == 0 ? "" : "Fuzzed";
			}
			else if (kind == "AddVariable")
			{
				operation["name"] = NewName("Fuzz");
				operation["type"] = Pick(s_ValueTypes);
				if (m_Random.Pick(2) == 0)
				{
					operation["public"] = true;
				}
				Label(operation, "as", m_VariableLabels);
			}
			else if (kind == "SetVariable" || kind == "DeleteVariable" || kind == "DuplicateVariable")
			{
				operation["variable"] = Variable();
				if (kind == "SetVariable")
				{
					size_t const change = m_Random.Pick(3);
					if (change == 0)
					{
						operation["value"] = Value();
					}
					else if (change == 1)
					{
						operation["name"] = NewName("Renamed");
					}
					else
					{
						operation["type"] = Pick(s_ValueTypes);
					}
				}
				if (kind == "DeleteVariable" && m_Random.Pick(3) == 0)
				{
					operation["removeNodes"] = false;
				}
				if (kind == "DuplicateVariable" && m_Random.Pick(2) == 0)
				{
					operation["name"] = NewName("Copy");
				}
			}
			else if (kind == "ReorderVariables")
			{
				Json variables = Json::array();
				for (auto it = m_Document.Variables.rbegin(); it != m_Document.Variables.rend(); ++it)
				{
					variables.push_back(it->ID);
				}
				operation["variables"] = std::move(variables);
			}
			else if (kind == "AddLocal" || kind == "SetLocal" || kind == "DeleteLocal")
			{
				VisualScriptFunction const* const function = FindFunction();
				operation["function"] = function != nullptr ? Json(function->Name) : Json("Nope");
				if (kind == "AddLocal")
				{
					operation["name"] = NewName("Local");
					operation["type"] = Pick(s_ValueTypes);
				}
				else
				{
					operation["local"] = function != nullptr && !function->Locals.empty() ? Json(Pick(function->Locals).ID) : Json("Nope");
					if (kind == "SetLocal")
					{
						operation["value"] = Value();
					}
				}
			}
			else if (kind == "AddFunction")
			{
				operation["name"] = NewName("Fuzzed");
				operation["inputs"] = Json::array({Json::object({{"name", "In"}, {"type", Pick(s_ValueTypes)}})});
				if (m_Random.Pick(2) == 0)
				{
					operation["pure"] = true;
					operation["outputs"] = Json::array({Json::object({{"name", "Out"}, {"type", "float"}})});
				}
				Label(operation, "as", m_FunctionLabels);
			}
			else if (kind == "RenameFunction" || kind == "SetFunctionSignature" || kind == "SetFunctionPure" || kind == "DeleteFunction" ||
			         kind == "DuplicateFunction")
			{
				operation["function"] = FunctionReference();
				if (kind == "RenameFunction")
				{
					operation["name"] = NewName("Renamed");
				}
				if (kind == "SetFunctionSignature")
				{
					operation["inputs"] = Json::array({Json::object({{"name", "Amount"}, {"type", "int"}})});
					operation["outputs"] = Json::array();
				}
				if (kind == "SetFunctionPure")
				{
					operation["pure"] = m_Random.Pick(2) == 0;
				}
			}
			else if (kind == "Arrange")
			{
				operation["scope"] = Pick(std::vector<std::string>{"Graph", "Created", "Nodes"});
				if (operation["scope"] == "Nodes")
				{
					operation["nodes"] = Elements(2 + m_Random.Pick(3), false);
				}
				operation["extents"] = m_Random.Pick(2) == 0 ? "Measured" : "Uniform";
			}
			else if (kind == "Align" || kind == "Distribute")
			{
				operation["elements"] = Elements(3 + m_Random.Pick(2), true);
				if (kind == "Align")
				{
					operation["alignment"] = Pick(std::vector<std::string>{"Left", "Right", "Top", "Bottom", "Centers", "Middles"});
				}
				else
				{
					operation["axis"] = m_Random.Pick(2) == 0 ? "Horizontal" : "Vertical";
				}
			}
			else if (kind == "Straighten")
			{
				operation["nodes"] = Elements(2 + m_Random.Pick(2), false);
			}
			else if (kind == "Fix")
			{
				std::vector<Json> fixes;
				for (Json const& diagnostic : m_Diagnostics)
				{
					for (Json const& fix : diagnostic.value("Fixes", Json::array()))
					{
						fixes.push_back(fix["operation"]);
					}
				}
				if (!fixes.empty() && m_Random.Pick(4) != 0)
				{
					return Pick(fixes);
				}
				operation["code"] = Pick(std::vector<std::string>{"UnknownPin", "UnknownNodeType", "TypeMismatch"});
				operation["fix"] = "Use Value";
				operation["node"] = Node();
			}
			else
			{
				// Kinds a later track adds: a node, so that a refusal names the operation.
				operation["node"] = Node();
			}
			return operation;
		}

		// Damages one operation: a field it does not take, a label not yet defined, an ID an earlier operation deleted, or an
		// element the document does not have.
		void Damage(Json& operation)
		{
			switch (m_Random.Pick(4))
			{
				case 0:
					operation["offset"] = Json::array({1.0, 1.0});
					operation["nodeId"] = 1;
					break;
				case 1:
					operation["node"] = "$later";
					break;
				case 2:
					operation["node"] = m_Deleted.empty() ? Json(999999) : Json(Pick(m_Deleted));
					break;
				default:
					operation["elements"] = Json::array({999999});
					break;
			}
		}

		// The sub-graph the batch edits: the event graph, or the function's.
		VisualScriptGraph const& SubGraph() const
		{
			auto const function = std::find_if(m_Document.Functions.begin(), m_Document.Functions.end(),
			                                   [this](VisualScriptFunction const& candidate)
			                                   {
												   return candidate.ID == m_Function;
											   });
			return function != m_Document.Functions.end() ? function->Graph : m_Document.Graph;
		}

		std::vector<VisualScriptNode> const& SubGraphNodes() const { return SubGraph().Nodes; }

		// A node: mostly one of the sub-graph's, else one the batch labelled, now and then one that does not exist.
		Json Node()
		{
			size_t const choice = m_Random.Pick(10);
			if (choice < 2 && !m_ElementLabels.empty())
			{
				return Pick(m_ElementLabels);
			}
			if (choice < 9 && !SubGraphNodes().empty())
			{
				return Pick(SubGraphNodes()).ID;
			}
			return 999999;
		}

		Json Comment()
		{
			std::vector<VisualScriptComment> const& comments = SubGraph().Comments;
			return comments.empty() || m_Random.Pick(10) == 0 ? Json(999999) : Json(Pick(comments).ID);
		}

		Json Element(bool comments) { return comments && m_Random.Pick(5) == 0 ? Comment() : Node(); }

		Json Elements(size_t count, bool comments)
		{
			Json elements = Json::array();
			for (; count > 0; count--)
			{
				elements.push_back(Element(comments));
			}
			return elements;
		}

		// A pin of a node of the given direction, as its analysis names it; a name it may not have now and then.
		std::string PinOf(Json const& node, std::string_view direction)
		{
			std::vector<std::string> names;
			if (node.is_number_unsigned())
			{
				for (Json const& entry : m_Nodes)
				{
					if (entry.value("ID", uint32_t(0)) != node.get<uint32_t>())
					{
						continue;
					}
					for (Json const& pin : entry.value("Pins", Json::array()))
					{
						if (pin.value("Direction", std::string()) == direction)
						{
							names.push_back(pin.value("Name", std::string()));
						}
					}
				}
			}
			if (names.empty() || m_Random.Pick(10) == 0)
			{
				names = direction == "In" ? std::vector<std::string>{"Execute", "Value", "A", "Condition", "Message"}
				                          : std::vector<std::string>{"Then", "Value", "Result", "Completed"};
			}
			return Pick(names);
		}

		Json Link()
		{
			std::vector<VisualScriptLink> const& links = SubGraph().Links;
			if (links.empty() || m_Random.Pick(10) == 0)
			{
				return Json::object({{"from", Json::array({Node(), "Then"})}, {"to", Json::array({Node(), "Execute"})}});
			}
			VisualScriptLink const& link = Pick(links);
			return Json::object({{"from", Json::array({link.From.Node, link.From.Pin})}, {"to", Json::array({link.To.Node, link.To.Pin})}});
		}

		Json Variable()
		{
			if (!m_VariableLabels.empty() && m_Random.Pick(3) == 0)
			{
				return Pick(m_VariableLabels);
			}
			if (m_Document.Variables.empty() || m_Random.Pick(10) == 0)
			{
				return "Nope";
			}
			VisualScriptVariable const& variable = Pick(m_Document.Variables);
			return m_Random.Pick(2) == 0 ? Json(variable.Name) : Json(variable.ID);
		}

		VisualScriptFunction const* FindFunction() { return m_Document.Functions.empty() ? nullptr : &Pick(m_Document.Functions); }

		Json FunctionReference()
		{
			if (!m_FunctionLabels.empty() && m_Random.Pick(3) == 0)
			{
				return Pick(m_FunctionLabels);
			}
			VisualScriptFunction const* const function = FindFunction();
			if (function == nullptr || m_Random.Pick(10) == 0)
			{
				return "Nope";
			}
			return m_Random.Pick(2) == 0 ? Json(function->Name) : Json(function->ID);
		}

		Json Point()
		{
			return Json::array({static_cast<double>(m_Random.Pick(2000)) - 1000.0, static_cast<double>(m_Random.Pick(2000)) - 1000.0});
		}

		Json Value()
		{
			std::array<Json, 7> const values = {
				Json(1), Json(2.5), Json(true), Json("Fuzz"), Json::array({1.0, 2.0, 3.0}), Json(), Json::object({{"Unexpected", 1}})};
			return values[m_Random.Pick(values.size())];
		}

		// A formula: valid, renaming or adding inputs, or broken (a broken formula is stored too; the analysis reports it).
		std::string Formula()
		{
			return Pick(std::vector<std::string>{"a * b + sin(t)", "speed * dt", "velocity * dt", "a * b * c", "2 * pi", "x + (",
			                                     "Self + 1", "v.X + v.Y", ""});
		}

		// Defines a new label for what the operation creates, half the time.
		void Label(Json& operation, char const* field, std::vector<std::string>& labels)
		{
			if (m_Random.Pick(2) == 0)
			{
				operation[field] = NewLabel(labels);
			}
		}

		std::string NewLabel(std::vector<std::string>& labels)
		{
			labels.push_back("$l" + std::to_string(m_NextLabel++));
			return labels.back();
		}

		std::string NewName(std::string_view prefix) { return std::string(prefix) + std::to_string(m_NextLabel++); }

		template<typename T>
		T const& Pick(std::vector<T> const& values)
		{
			REQUIRE_FALSE(values.empty());
			return values[m_Random.Pick(values.size())];
		}

		static inline std::vector<std::string> const s_ValueTypes = {"int", "float", "bool", "string", "Strada.Vector3"};

		Testing::FuzzRandom& m_Random;
		VisualScriptDocument const& m_Document;
		Json const& m_Nodes;
		Json const& m_Diagnostics;
		std::string const& m_Clipboard;
		std::set<std::string> m_Types;
		uint32_t m_Function = 0;
		std::vector<std::string> m_ElementLabels;
		std::vector<std::string> m_VariableLabels;
		std::vector<std::string> m_FunctionLabels;
		std::vector<uint32_t> m_Deleted;
		uint32_t m_NextLabel = 0;
	};
}

TEST_CASE("VisualScriptBatch: random batches either fail naming an operation and change nothing, or write one undo step that loads "
          "strictly and undoes byte for byte")
{
	Testing::ScriptRuntimeScope runtime;
	EditFuzzFixture fixture;
	std::string const path = "Scripts/Fuzzed.sgraph";
	std::string const graph = fixture.Ok("graph.create", Json::object({{"path", path}}))["asset"].get<std::string>();
	Result<std::string> const editor =
		FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Documents" / "Editor.json");
	REQUIRE(editor.IsOk());
	Result<Json> editorDocument = ParseJson(editor.GetValue());
	REQUIRE(editorDocument.IsOk());
	std::array<Json, 2> const seeds = {Testing::LoadFeatureTestDocument("Assets/Scripts/FormatSample.sgraph")["VisualScript"],
	                                   editorDocument.TakeValue()};

	Testing::FuzzRandom random(10800);
	std::map<std::string, size_t> kinds;
	size_t successes = 0;
	size_t changes = 0;
	size_t failures = 0;
	int const rounds = Testing::GetFuzzRounds(200);
	for (int round = 0; round < rounds; round++)
	{
		CAPTURE(round);
		// A seed again every twenty rounds, so batches keep finding nodes, links and functions to edit; the history starts
		// empty then, so it never reaches its limit, where every new step drops the oldest.
		if (round % 20 == 0)
		{
			fixture.Context.GetHistory().Clear();
			fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", seeds[static_cast<size_t>(round / 20) % seeds.size()]}}));
		}
		// The document as stored, its analysis' nodes and its diagnostics with their quick fixes: an empty batch answers them.
		Json const state =
			fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", Json::array()}, {"nodes", "All"}, {"document", true}}));
		REQUIRE(state["analyzed"] == true);
		Result<AssetHandle> const handle = AssetManager::ResolveReference(graph);
		REQUIRE(handle.IsOk());
		VisualScriptDocument const document = AssetManager::GetAsset<VisualScriptAsset>(handle.GetValue())->GetDocument();
		std::string clipboard;
		if (!document.Graph.Nodes.empty())
		{
			Json elements = Json::array();
			for (size_t count = 1 + random.Pick(3); count > 0; count--)
			{
				elements.push_back(document.Graph.Nodes[random.Pick(document.Graph.Nodes.size())].ID);
			}
			clipboard = fixture.Ok("graph.copy", Json::object({{"graph", graph}, {"elements", std::move(elements)}}))["text"];
		}

		BatchGenerator generator(random, document, state["nodes"], state["diagnostics"], clipboard);
		Json params = generator.Generate(kinds);
		params["graph"] = graph;
		CAPTURE(params);
		std::string const before = fixture.ReadFile(path);
		size_t const steps = fixture.Context.GetHistory().GetUndoCount();

		Json dryParams = params;
		dryParams["dryRun"] = true;
		CommandResult dry = fixture.Run("graph.edit", dryParams);
		CHECK(fixture.ReadFile(path) == before);
		CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
		CommandResult real = fixture.Run("graph.edit", params);
		REQUIRE(dry.IsOk() == real.IsOk());
		if (real.IsError())
		{
			failures++;
			CommandError const& error = real.GetError();
			CHECK_MESSAGE(error.Code != AutomationErrorCode::InternalError, error.Message);
			// A dry run fails as the call does.
			CHECK(dry.GetError().Code == error.Code);
			CHECK(dry.GetError().Message == error.Message);
			CHECK(dry.GetError().Data == error.Data);
			// An operation's failure names it, and its field.
			if (error.Data.is_object() && error.Data.contains("operation"))
			{
				std::string const operation = fmt::format("operations[{}]", error.Data["operation"].get<size_t>());
				CHECK(error.Message.starts_with(operation));
				CHECK(error.Data["path"].get<std::string>().starts_with(operation));
			}
			CHECK(fixture.ReadFile(path) == before);
			CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
			continue;
		}

		successes++;
		Json answer = dry.TakeValue();
		Json const result = real.TakeValue();
		CHECK(answer["dryRun"] == true);
		// A dry run answers what the call then answers; the call may make fewer analyses, as the dry run kept the stored one.
		answer["dryRun"] = false;
		answer.erase("analyses");
		Json compared = result;
		compared.erase("analyses");
		CHECK(answer == compared);
		VisualScriptDocument const written = AssetManager::GetAsset<VisualScriptAsset>(handle.GetValue())->GetDocument();
		Testing::ExpectStrictRoundTrip(written);
		Testing::ExpectSplitConsistency(written);
		std::string const after = fixture.ReadFile(path);
		CHECK(after == VisualScriptSerializer::WriteText(written));
		if (result["changed"] == false)
		{
			CHECK(after == before);
			CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
			continue;
		}
		changes++;
		// One step, which undoes and redoes byte for byte.
		CHECK(fixture.Context.GetHistory().GetUndoCount() == steps + 1);
		CHECK(fixture.Run("editor.undo", Json::object()).IsOk());
		CHECK(fixture.ReadFile(path) == before);
		CHECK(fixture.Run("editor.redo", Json::object()).IsOk());
		CHECK(fixture.ReadFile(path) == after);
	}

	// The batches reached the operations' work: some wrote, some failed, and every kind was drawn.
	CHECK(successes > 0);
	CHECK(changes > 0);
	CHECK(failures > 0);
	for (std::string_view const kind : GetVisualScriptBatchOperations())
	{
		CAPTURE(kind);
		CHECK(kinds[std::string(kind)] > 0);
	}
}
