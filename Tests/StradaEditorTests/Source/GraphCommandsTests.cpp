#include "HeadlessImGui.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/Panels/VisualScriptEditorPanel.h"
#include "Editor/PlayMode.h"
#include "Editor/VisualScriptEnumLookupSource.h"
#include "Editor/VisualScripting/VisualScriptEditors.h"
#include "Editor/VisualScripting/VisualScriptPalette.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float FrameTime = 1.0f / 60.0f;

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value = Json(), bool isPublic = true)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		variable.Value = std::move(value);
		variable.Public = isPublic;
		return variable;
	}

	// Placed in a row, so that graph.set lays out nothing.
	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object(), Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = glm::vec2(static_cast<float>(id) * 16.0f, 0.0f);
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	// Adds Speed (public, 1) to Updates (public, 0) on every update.
	VisualScriptDocument MakeCounterDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Speed", "int", 1));
		document.Variables.push_back(MakeVariable(2, "Updates", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 2}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Operator.Add"));
		document.Graph.Nodes.push_back(MakeNode(13, "Variable.Get", Json::object({{"Variable", 2}})));
		document.Graph.Nodes.push_back(MakeNode(14, "Variable.Get", Json::object({{"Variable", 1}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(13, "Value", 12, "A"));
		document.Graph.Links.push_back(MakeLink(14, "Value", 12, "B"));
		document.Graph.Links.push_back(MakeLink(12, "Result", 11, "Value"));
		return document;
	}

	// Sets Done (public) once a Delay of half a second, started when the graph starts, completes.
	VisualScriptDocument MakeDelayDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Done", "bool", false));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(11, "Flow.Delay", Json::object(), Json::object({{"Duration", 0.5}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Variable.Set", Json::object({{"Variable", 1}}), Json::object({{"Value", true}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(11, "Completed", 12, "Execute"));
		return document;
	}

	Json StoredInt(int32_t value)
	{
		return Json::object({{"Type", "Int32"}, {"Value", value}});
	}

	// A project open in an editor model with the commands of the editor and of play mode, as the editor layer sets them up.
	struct GraphFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};
		CommandRegistry Registry;

		explicit GraphFixture(bool withProject = true)
		{
			REQUIRE(RegisterEditorCommands(Registry, Operations, EditorCommandEnvironment()).IsOk());
			REQUIRE(RegisterPlayCommands(Registry, Play, Context,
			                             []
			                             {
											 return glm::uvec2(640, 480);
										 })
			            .IsOk());
			if (withProject)
			{
				REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			}
		}

		CommandResult Run(std::string_view name, Json params = Json::object())
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

		Json Ok(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
			return result.TakeValue();
		}

		CommandError Fails(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE(result.IsError());
			return result.TakeError();
		}

		std::filesystem::path GetFile(std::string_view path) const
		{
			return AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(path);
		}

		std::string ReadFile(std::string_view path) const
		{
			Result<Buffer> bytes = FileSystem::ReadBinaryFile(GetFile(path));
			REQUIRE(bytes.IsOk());
			return std::string(bytes.GetValue().As<char>(), static_cast<size_t>(bytes.GetValue().GetSize()));
		}

		void WriteFile(std::string_view path, std::string_view text) const
		{
			REQUIRE(FileSystem::WriteTextFile(GetFile(path), text).IsOk());
		}

		// Creates a graph with the document; returns its handle.
		std::string CreateGraph(std::string const& path, VisualScriptDocument const& document)
		{
			std::string const graph = Ok("graph.create", Json::object({{"path", path}}))["asset"].get<std::string>();
			Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(document)}}));
			return graph;
		}

		// The document the graph's asset holds.
		VisualScriptDocument GetDocument(std::string const& graph) const
		{
			Result<AssetHandle> const handle = AssetManager::ResolveReference(graph);
			REQUIRE(handle.IsOk());
			Ref<VisualScriptAsset> const script = AssetManager::GetAsset<VisualScriptAsset>(handle.GetValue());
			REQUIRE(script != nullptr);
			return script->GetDocument();
		}

		std::string CreateEntity(std::string name, Json components)
		{
			return Ok("entity.create", Json::object({{"name", std::move(name)}, {"components", std::move(components)}}))["id"]
			    .get<std::string>();
		}
	};

	std::vector<std::string> GetTypes(Json const& nodes)
	{
		std::vector<std::string> types;
		for (Json const& node : nodes)
		{
			types.push_back(node["Type"].get<std::string>());
		}
		return types;
	}

	bool Contains(std::vector<std::string> const& values, std::string_view value)
	{
		return std::find(values.begin(), values.end(), value) != values.end();
	}

	// The node of an analysis' nodes with the ID.
	Json const& FindAnalyzedNode(Json const& nodes, uint32_t id)
	{
		auto const node = std::find_if(nodes.begin(), nodes.end(),
		                               [id](Json const& entry)
		                               {
										   return entry["ID"] == id;
									   });
		REQUIRE(node != nodes.end());
		return *node;
	}
}

TEST_CASE("GraphCommands: every graph command is registered, documented and read-only when it changes nothing")
{
	GraphFixture fixture;
	Result<std::string> const reference = FileSystem::ReadTextFile(STRADA_AUTOMATION_DOC_PATH);
	REQUIRE(reference.IsOk());
	for (std::string const name : {"graph.create", "graph.get", "graph.set", "graph.check", "graph.library", "graph.types", "graph.open",
	                               "graph.close", "graph.editors", "graph.palette", "graph.find"})
	{
		CAPTURE(name);
		CommandDefinition const* command = fixture.Registry.Find(name);
		REQUIRE(command != nullptr);
		bool const changes =
			name == "graph.create" || name == "graph.set" || name == "graph.open" || name == "graph.close" || name == "graph.palette";
		CHECK(command->ReadOnly == !changes);
		// graph.palette answers once the palette was drawn.
		CHECK(command->IsAsync() == (name == "graph.palette"));
		CHECK(reference.GetValue().find("`" + name + "`") != std::string::npos);
	}
}

TEST_CASE("GraphCommands: graphs are created from templates and read back, and what cannot be read is refused")
{
	// Without assets nothing is available.
	{
		EditorContext context;
		EditorOperations operations(context);
		CommandRegistry registry;
		REQUIRE(RegisterEditorCommands(registry, operations, EditorCommandEnvironment()).IsOk());
		auto const code = [&registry](std::string_view name, Json params)
		{
			std::optional<CommandResult> result;
			registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			REQUIRE(result->IsError());
			return result->GetError().Code;
		};
		CHECK(code("graph.create", Json::object({{"path", "Graph.sgraph"}})) == AutomationErrorCode::Unavailable);
		CHECK(code("graph.get", Json::object({{"graph", "1"}})) == AutomationErrorCode::Unavailable);
		CHECK(code("graph.set", Json::object({{"graph", "1"}, {"document", Json::object()}})) == AutomationErrorCode::Unavailable);
		CHECK(code("graph.check", Json::object({{"document", Json::object()}})) == AutomationErrorCode::Unavailable);
		CHECK(code("graph.library", Json::object()) == AutomationErrorCode::Unavailable);
		CHECK(code("graph.types", Json::object()) == AutomationErrorCode::Unavailable);
	}

	// Files need a project; checking a document does not.
	{
		GraphFixture fixture(false);
		CHECK(fixture.Fails("graph.create", Json::object({{"path", "Graph.sgraph"}})).Code == AutomationErrorCode::InvalidOperation);
		CHECK(fixture.Fails("graph.set", Json::object({{"graph", "1"}, {"document", Json::object()}})).Code ==
		      AutomationErrorCode::InvalidOperation);
		CHECK(fixture.Fails("graph.get", Json::object({{"graph", "123"}})).Code == AutomationErrorCode::AssetNotFound);
		Json const checked = fixture.Ok("graph.check", Json::object({{"document", Json::object()}}));
		CHECK(checked["valid"] == true);
	}

	GraphFixture fixture;
	Json const created = fixture.Ok("graph.create", Json::object({{"path", "Scripts/Mover.sgraph"}}));
	CHECK(created ==
	      Json::object({{"asset", AssetManager::FindByPath("Scripts/Mover.sgraph").ToString()}, {"path", "Scripts/Mover.sgraph"}}));
	CHECK(fixture.ReadFile("Scripts/Mover.sgraph") ==
	      DumpJson(VisualScriptSerializer::Serialize(MakeVisualScriptTemplate(VisualScriptTemplate::Events))));
	// File creation is no undo step.
	CHECK_FALSE(fixture.Context.GetHistory().CanUndo());

	// The document, by handle or by reference; without the scripting runtime nothing is analyzed.
	for (Json const& graph : {created["asset"], Json("asset://Scripts/Mover.sgraph")})
	{
		CAPTURE(graph);
		Json const read = fixture.Ok("graph.get", Json::object({{"graph", graph}}));
		CHECK(read ==
		      Json::object({{"asset", created["asset"]},
		                    {"path", "Scripts/Mover.sgraph"},
		                    {"document", VisualScriptSerializer::SerializeDocument(MakeVisualScriptTemplate(VisualScriptTemplate::Events))},
		                    {"analyzed", false}}));
	}
	CHECK(fixture.Ok("graph.get", Json::object({{"graph", created["asset"]}, {"analyze", false}}))["analyzed"] == false);
	Json const empty = fixture.Ok("graph.create", Json::object({{"path", "Empty.sgraph"}, {"template", "Empty"}}));
	CHECK(fixture.Ok("graph.get", Json::object({{"graph", empty["asset"]}}))["document"] ==
	      VisualScriptSerializer::SerializeDocument(MakeVisualScriptTemplate(VisualScriptTemplate::Empty)));

	// Paths that are taken, of other files or outside the asset directory are refused, naming the path.
	for (std::string const path : {"Scripts/Mover.sgraph", "Scripts/Mover.smat", "../Outside.sgraph"})
	{
		CAPTURE(path);
		CommandError const error = fixture.Fails("graph.create", Json::object({{"path", path}}));
		CHECK(error.Code == AutomationErrorCode::InvalidParams);
		CHECK(error.Data["path"] == "path");
	}
	CHECK(fixture.Fails("graph.create", Json::object({{"path", "Scripts/Mover.sgraph"}})).Message.find("already exists") !=
	      std::string::npos);
	CHECK(fixture.Fails("graph.create", Json::object({{"path", "Scripts/Mover.smat"}})).Message.find(".sgraph") != std::string::npos);
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Game" / "Outside.sgraph"));
	CommandError const unknownTemplate = fixture.Fails("graph.create", Json::object({{"path", "Other.sgraph"}, {"template", "Full"}}));
	CHECK(unknownTemplate.Code == AutomationErrorCode::InvalidParams);
	CHECK(unknownTemplate.Data["path"] == "template");

	// Graphs that are not registered, assets of other types and files that do not load.
	CommandError const unregistered = fixture.Fails("graph.get", Json::object({{"graph", "123"}}));
	CHECK(unregistered.Code == AutomationErrorCode::AssetNotFound);
	CHECK(unregistered.Data["path"] == "graph");
	std::string const material =
		fixture.Ok("material.create", Json::object({{"path", "Materials/Painted.smat"}}))["material"]["id"].get<std::string>();
	CommandError const notAGraph = fixture.Fails("graph.get", Json::object({{"graph", material}}));
	CHECK(notAGraph.Code == AutomationErrorCode::InvalidParams);
	CHECK(notAGraph.Data["path"] == "graph");
	CHECK(notAGraph.Message == "graph: asset " + material + " is a Material, not a visual script");
	fixture.WriteFile("Broken.sgraph", "{ \"Strada\": ");
	REQUIRE(fixture.Ok("asset.refresh")["added"].size() == 1);
	CommandError const broken = fixture.Fails("graph.get", Json::object({{"graph", "asset://Broken.sgraph"}}));
	CHECK(broken.Code == AutomationErrorCode::FileError);
	CHECK(broken.Message.find("failed to load 'Broken.sgraph'") != std::string::npos);
	REQUIRE(FileSystem::Remove(fixture.GetFile("Scripts/Mover.sgraph")).IsOk());
	REQUIRE(fixture.Ok("asset.refresh")["missing"].size() == 1);
	CommandError const missing = fixture.Fails("graph.get", Json::object({{"graph", created["asset"]}}));
	CHECK(missing.Code == AutomationErrorCode::FileError);
	CHECK(missing.Message.find("missing") != std::string::npos);
}

TEST_CASE("GraphCommands: graph.set saves the document as one undo step that leaves the scene unchanged")
{
	GraphFixture fixture;
	std::string const graph = fixture.Ok("graph.create", Json::object({{"path", "Scripts/Counter.sgraph"}}))["asset"].get<std::string>();
	std::string const original = fixture.ReadFile("Scripts/Counter.sgraph");
	VisualScriptDocument const before = fixture.GetDocument(graph);

	VisualScriptDocument const counter = MakeCounterDocument();
	Json const params = Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(counter)}});
	Json const set = fixture.Ok("graph.set", params);
	CHECK(set ==
	      Json::object(
			  {{"asset", graph}, {"path", "Scripts/Counter.sgraph"}, {"changed", true}, {"analyzed", false}, {"warnings", Json::array()}}));
	std::string const written = DumpJson(VisualScriptSerializer::Serialize(counter));
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == written);
	CHECK(fixture.GetDocument(graph) == counter);
	Json const status = fixture.Ok("editor.status");
	CHECK(status["scene"]["dirty"] == false);
	CHECK(status["undo"] == "Edit Visual Script");

	// The stored document again changes nothing: no step.
	CHECK(fixture.Ok("graph.set", params)["changed"] == false);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == 1);

	// Undo and redo write the file and give the loaded asset its document; the scene stays as it was.
	CHECK(fixture.Ok("editor.undo")["undone"] == "Edit Visual Script");
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == original);
	CHECK(fixture.GetDocument(graph) == before);
	CHECK(fixture.Ok("editor.status")["scene"]["dirty"] == false);
	CHECK(fixture.Ok("editor.redo")["redone"] == "Edit Visual Script");
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == written);
	CHECK(fixture.GetDocument(graph) == counter);
	CHECK(fixture.Ok("editor.status")["scene"]["dirty"] == false);

	// The step finds the graph by its handle where it moved.
	fixture.Ok("asset.move", Json::object({{"asset", graph}, {"path", "Moved/Counter.sgraph"}}));
	fixture.Ok("editor.undo");
	CHECK(fixture.ReadFile("Moved/Counter.sgraph") == original);
	CHECK(fixture.GetDocument(graph) == before);

	// What graph.set cannot write: nothing changes and no step is recorded.
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	Json const document = VisualScriptSerializer::SerializeDocument(counter);
	CommandError const unregistered = fixture.Fails("graph.set", Json::object({{"graph", "123"}, {"document", document}}));
	CHECK(unregistered.Code == AutomationErrorCode::AssetNotFound);
	CHECK(unregistered.Data["path"] == "graph");
	std::string const material =
		fixture.Ok("material.create", Json::object({{"path", "Materials/Painted.smat"}}))["material"]["id"].get<std::string>();
	CommandError const notAGraph = fixture.Fails("graph.set", Json::object({{"graph", material}, {"document", document}}));
	CHECK(notAGraph.Code == AutomationErrorCode::InvalidParams);
	CHECK(notAGraph.Data["path"] == "graph");
	AssetHandle const memory = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(counter), "Memory");
	CommandError const withoutFile = fixture.Fails("graph.set", Json::object({{"graph", memory.ToString()}, {"document", document}}));
	CHECK(withoutFile.Code == AutomationErrorCode::InvalidParams);
	CHECK(withoutFile.Data["path"] == "graph");
	CHECK(withoutFile.Message.find("no file of its own") != std::string::npos);
	CHECK(fixture.Fails("graph.set", Json::object({{"graph", graph}})).Code == AutomationErrorCode::InvalidParams);
	REQUIRE(FileSystem::Remove(fixture.GetFile("Moved/Counter.sgraph")).IsOk());
	REQUIRE(fixture.Ok("asset.refresh")["missing"].size() == 1);
	CommandError const missing = fixture.Fails("graph.set", Json::object({{"graph", graph}, {"document", document}}));
	CHECK(missing.Code == AutomationErrorCode::FileError);
	CHECK(missing.Message.find("missing") != std::string::npos);
	CHECK_FALSE(FileSystem::Exists(fixture.GetFile("Moved/Counter.sgraph")));
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
}

TEST_CASE("GraphCommands: graph.set refuses what the loader would repair, naming every issue, and graph.check reports them")
{
	GraphFixture fixture;
	std::string const graph = fixture.Ok("graph.create", Json::object({{"path", "Scripts/Strict.sgraph"}}))["asset"].get<std::string>();
	std::string const original = fixture.ReadFile("Scripts/Strict.sgraph");

	// An unknown key, a node with the ID of another and a link from a node that does not exist.
	Json document = VisualScriptSerializer::SerializeDocument(MakeCounterDocument());
	document["Extra"] = true;
	document["Graph"]["Nodes"][1]["ID"] = 10;
	document["Graph"]["Links"].push_back(Json::object({{"From", Json::array({99, "Value"})}, {"To", Json::array({11, "Value"})}}));
	CommandError const error = fixture.Fails("graph.set", Json::object({{"graph", graph}, {"document", document}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	CHECK(error.Message == "Extra: unknown key; it is ignored");
	CHECK(error.Data["path"] == "document.Extra");
	Json const& issues = error.Data["issues"];
	REQUIRE(issues.is_array());
	CHECK(issues.size() >= 3);
	CHECK(issues[0] == Json::object({{"path", "Extra"}, {"message", "unknown key; it is ignored"}}));
	auto const hasIssueAt = [&issues](std::string const& path)
	{
		return std::any_of(issues.begin(), issues.end(),
		                   [&path](Json const& issue)
		                   {
							   return issue["path"] == path;
						   });
	};
	CHECK(hasIssueAt("Graph.Nodes[1].ID"));
	// Nothing was written.
	CHECK(fixture.ReadFile("Scripts/Strict.sgraph") == original);
	CHECK_FALSE(fixture.Context.GetHistory().CanUndo());

	// graph.check lists the same issues instead of failing, and writes nothing either.
	Json const checked = fixture.Ok("graph.check", Json::object({{"document", document}, {"graph", graph}}));
	CHECK(checked == Json::object({{"valid", false}, {"issues", issues}, {"warnings", Json::array()}, {"analyzed", false}}));
	CHECK(fixture.ReadFile("Scripts/Strict.sgraph") == original);

	// The first issue may be deep in the document: data.path leads to it.
	Json duplicate = VisualScriptSerializer::SerializeDocument(MakeCounterDocument());
	duplicate["Graph"]["Nodes"][2]["ID"] = 11;
	CommandError const deep = fixture.Fails("graph.set", Json::object({{"graph", graph}, {"document", duplicate}}));
	CHECK(deep.Data["path"] == "document.Graph.Nodes[2].ID");
	CHECK(deep.Message.starts_with("Graph.Nodes[2].ID: "));

	// Keys left out take their defaults and a null Value is no value: neither is an issue.
	Json const sparse =
		Json::object({{"Variables", Json::array({Json::object({{"ID", 1}, {"Name", "Speed"}, {"Type", "float"}, {"Value", nullptr}})})}});
	CHECK(fixture.Ok("graph.check", Json::object({{"document", sparse}})) ==
	      Json::object({{"valid", true}, {"issues", Json::array()}, {"warnings", Json::array()}, {"analyzed", false}}));
	CHECK(fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", sparse}}))["changed"] == true);
	VisualScriptDocument expected;
	expected.Variables.push_back(MakeVariable(1, "Speed", "float", Json(), false));
	CHECK(fixture.GetDocument(graph) == expected);

	// graph.check reports what is wrong with the graph it names.
	CHECK(fixture.Fails("graph.check", Json::object({{"document", sparse}, {"graph", "123"}})).Code == AutomationErrorCode::AssetNotFound);
	std::string const material =
		fixture.Ok("material.create", Json::object({{"path", "Materials/Painted.smat"}}))["material"]["id"].get<std::string>();
	CommandError const notAGraph = fixture.Fails("graph.check", Json::object({{"document", sparse}, {"graph", material}}));
	CHECK(notAGraph.Code == AutomationErrorCode::InvalidParams);
	CHECK(notAGraph.Data["path"] == "graph");
}

TEST_CASE("GraphCommands: renamed variables keep their stored names as former names, the 16 most recent of them")
{
	GraphFixture fixture;
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "Speed", "float", 4.0));
	document.Variables.push_back(MakeVariable(2, "Score", "int", 0));
	document.Variables.push_back(MakeVariable(3, "Health", "int", 100));
	std::string const graph = fixture.CreateGraph("Scripts/Renamed.sgraph", document);
	auto const stored = [&]
	{
		return fixture.GetDocument(graph);
	};
	auto const set = [&](VisualScriptDocument const& changed)
	{
		return fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(changed)}}));
	};

	// graph.check tells against the graph it names, and writes nothing; without a graph nothing is renamed.
	VisualScriptDocument renamed = stored();
	renamed.Variables[0].Name = "Velocity";
	Json const rename = Json::array({"variable renamed: Speed → Velocity (Speed is kept in FormerNames)"});
	Json const document1 = VisualScriptSerializer::SerializeDocument(renamed);
	CHECK(fixture.Ok("graph.check", Json::object({{"document", document1}, {"graph", graph}}))["warnings"] == rename);
	CHECK(fixture.Ok("graph.check", Json::object({{"document", document1}}))["warnings"] == Json::array());
	CHECK(stored().Variables[0].Name == "Speed");

	CHECK(set(renamed)["warnings"] == rename);
	CHECK(stored().Variables[0].Name == "Velocity");
	CHECK(stored().Variables[0].FormerNames == std::vector<std::string>{"Speed"});

	// Swapped names stay current names: none becomes a former name, which would make the document invalid.
	VisualScriptDocument swapped = stored();
	swapped.Variables[1].Name = "Health";
	swapped.Variables[2].Name = "Score";
	CHECK(set(swapped)["warnings"] == Json::array());
	CHECK(stored().Variables[1].FormerNames.empty());
	CHECK(stored().Variables[2].FormerNames.empty());
	// Renamed back, the variable keeps the other name; a name kept already is not kept twice.
	VisualScriptDocument back = stored();
	back.Variables[0].Name = "Speed";
	back.Variables[0].FormerNames.clear();
	set(back);
	CHECK(stored().Variables[0].FormerNames == std::vector<std::string>{"Velocity"});
	VisualScriptDocument again = stored();
	again.Variables[0].Name = "Velocity";
	again.Variables[0].FormerNames = {"Speed"};
	CHECK(set(again)["warnings"] == Json::array());
	CHECK(stored().Variables[0].FormerNames == std::vector<std::string>{"Speed"});

	// Each rename keeps one more former name, up to 16: then the oldest goes.
	VisualScriptDocument current = stored();
	current.Variables[0].Name = "Name0";
	current.Variables[0].FormerNames.clear();
	set(current);
	for (int renames = 1; renames <= 17; renames++)
	{
		current = stored();
		current.Variables[0].Name = "Name" + std::to_string(renames);
		set(current);
	}
	std::vector<std::string> expected;
	for (int name = 1; name <= 16; name++)
	{
		expected.push_back("Name" + std::to_string(name));
	}
	CHECK(stored().Variables[0].Name == "Name17");
	CHECK(stored().Variables[0].FormerNames == expected);
	// What graph.set wrote loads strictly.
	Json const final = fixture.Ok("graph.get", Json::object({{"graph", graph}}))["document"];
	CHECK(fixture.Ok("graph.check", Json::object({{"document", final}}))["valid"] == true);
}

TEST_CASE("GraphCommands: graph.set lays out nodes without a position, and placed nodes stay")
{
	GraphFixture fixture;
	VisualScriptDocument document = MakeCounterDocument();
	// The event stays at its place; the others form one group of linked nodes.
	document.Graph.Nodes[0].Position = glm::vec2(64.0f, 32.0f);
	for (size_t index = 1; index < document.Graph.Nodes.size(); index++)
	{
		document.Graph.Nodes[index].Position.reset();
	}
	std::string const graph = fixture.Ok("graph.create", Json::object({{"path", "Scripts/Layout.sgraph"}}))["asset"].get<std::string>();
	fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(document)}}));

	VisualScriptDocument laidOut = document;
	PlaceUnplacedNodes(laidOut);
	VisualScriptDocument const written = fixture.GetDocument(graph);
	CHECK(written == laidOut);
	auto const position = [&written](uint32_t id)
	{
		auto const node = std::find_if(written.Graph.Nodes.begin(), written.Graph.Nodes.end(),
		                               [id](VisualScriptNode const& candidate)
		                               {
										   return candidate.ID == id;
									   });
		REQUIRE(node != written.Graph.Nodes.end());
		REQUIRE(node->Position.has_value());
		return *node->Position;
	};
	CHECK(position(10) == glm::vec2(64.0f, 32.0f));
	// Below the placed node, from its left edge: the getters first, then the operator, then the setter.
	CHECK(position(13) == glm::vec2(64.0f, 272.0f));
	CHECK(position(14) == glm::vec2(64.0f, 432.0f));
	CHECK(position(12) == glm::vec2(384.0f, 272.0f));
	CHECK(position(11) == glm::vec2(704.0f, 272.0f));
}

TEST_CASE("GraphCommands: graph.set replaces a file that does not load, and undo gives the damaged file back")
{
	GraphFixture fixture;
	std::string const graph = fixture.Ok("graph.create", Json::object({{"path", "Scripts/Damaged.sgraph"}}))["asset"].get<std::string>();
	std::string const damaged = "{ \"Strada\": { \"Version\": 3, \"Type\": \"VisualScript\" }, \"VisualScript\": {} }\n";
	fixture.WriteFile("Scripts/Damaged.sgraph", damaged);
	CHECK(fixture.Fails("graph.get", Json::object({{"graph", graph}})).Code == AutomationErrorCode::FileError);

	VisualScriptDocument const counter = MakeCounterDocument();
	Json const set =
		fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(counter)}}));
	CHECK(set["changed"] == true);
	std::string const written = DumpJson(VisualScriptSerializer::Serialize(counter));
	CHECK(fixture.ReadFile("Scripts/Damaged.sgraph") == written);
	CHECK(fixture.Ok("graph.get", Json::object({{"graph", graph}}))["document"] == VisualScriptSerializer::SerializeDocument(counter));

	// Undo writes the damaged file back, which does not load again; redo repairs it again.
	fixture.Ok("editor.undo");
	CHECK(fixture.ReadFile("Scripts/Damaged.sgraph") == damaged);
	CHECK(fixture.Fails("graph.get", Json::object({{"graph", graph}})).Code == AutomationErrorCode::FileError);
	fixture.Ok("editor.redo");
	CHECK(fixture.ReadFile("Scripts/Damaged.sgraph") == written);
	CHECK(fixture.GetDocument(graph) == counter);
	CHECK(fixture.Ok("editor.status")["scene"]["dirty"] == false);
}

TEST_CASE("GraphCommands: graph.set keeps the node keys of version 2, which graph.get returns unchanged")
{
	// Without the scripting runtime nothing is analyzed, so the keys need not make sense to it: this is the format's round trip.
	GraphFixture fixture;
	VisualScriptDocument document = MakeCounterDocument();
	std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
	nodes[0].Comment = "Every frame";
	nodes[1].Split.Inputs = {"Value"};
	nodes[1].Inputs = Json::object({{"Value.X", 1}});
	nodes[2].Collapsed = true;
	nodes[3].Split.Outputs = {"Value", "Value.Position"};
	nodes[3].Enabled = VisualScriptNodeEnabled::Disabled;
	nodes[4].Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
	Json const content = VisualScriptSerializer::SerializeDocument(document);

	std::string const graph = fixture.CreateGraph("Scripts/Notes.sgraph", document);
	CHECK(fixture.GetDocument(graph) == document);
	CHECK(fixture.Ok("graph.get", Json::object({{"graph", graph}}))["document"] == content);
	std::string const written = fixture.ReadFile("Scripts/Notes.sgraph");
	CHECK(written == DumpJson(VisualScriptSerializer::Serialize(document)));
	CHECK(written.find("\"Version\": 2,") != std::string::npos);
	CHECK(fixture.Ok("graph.check", Json::object({{"graph", graph}, {"document", content}}))["valid"] == true);

	// Giving the same document again changes nothing; a repair of a version-2 key is refused as any other.
	CHECK(fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", content}}))["changed"] == false);
	Json damaged = content;
	damaged["Graph"]["Nodes"][3]["Enabled"] = "Off";
	CommandError const refused = fixture.Fails("graph.set", Json::object({{"graph", graph}, {"document", damaged}}));
	CHECK(refused.Code == AutomationErrorCode::InvalidParams);
	CHECK(refused.Data["path"] == "document.Graph.Nodes[3].Enabled");
	CHECK(fixture.GetDocument(graph) == document);
	CHECK(fixture.ReadFile("Scripts/Notes.sgraph") == written);
}

TEST_CASE("GraphCommands: without the scripting runtime there is no node library and nothing is analyzed")
{
	REQUIRE_FALSE(ScriptEngine::IsInitialized());
	GraphFixture fixture;
	std::string const graph = fixture.Ok("graph.create", Json::object({{"path", "Scripts/Plain.sgraph"}}))["asset"].get<std::string>();
	CHECK(fixture.Fails("graph.library").Code == AutomationErrorCode::Unavailable);
	CHECK(fixture.Fails("graph.types").Code == AutomationErrorCode::Unavailable);
	Json const read = fixture.Ok("graph.get", Json::object({{"graph", graph}}));
	CHECK(read["analyzed"] == false);
	CHECK_FALSE(read.contains("nodes"));
	CHECK_FALSE(read.contains("diagnostics"));
	Json const document = VisualScriptSerializer::SerializeDocument(MakeCounterDocument());
	Json const set = fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", document}}));
	CHECK(set["analyzed"] == false);
	CHECK_FALSE(set.contains("diagnostics"));
	Json const checked = fixture.Ok("graph.check", Json::object({{"document", document}}));
	CHECK(checked["analyzed"] == false);
	CHECK_FALSE(checked.contains("nodes"));
	// Node types are stored as given without the runtime to resolve them.
	VisualScriptDocument unresolved = MakeCounterDocument();
	unresolved.Graph.Nodes.push_back(MakeNode(20, "Call:Strada.Input.IsKeyDown", Json::object(), Json::object({{"Key", "Space"}})));
	CHECK(fixture.Ok("graph.set",
	                 Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(unresolved)}}))["warnings"] ==
	      Json::array());
	CHECK(fixture.GetDocument(graph) == unresolved);
}

TEST_CASE("GraphCommands: with the scripting runtime documents are analyzed, and nodes resolved by name are stored exactly")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	std::string const graph = fixture.Ok("graph.create", Json::object({{"path", "Scripts/Keys.sgraph"}}))["asset"].get<std::string>();
	Json const initial = fixture.Ok("graph.get", Json::object({{"graph", graph}}));
	CHECK(initial["analyzed"] == true);
	CHECK(initial["nodes"].size() == 2);
	CHECK(initial["diagnostics"] == Json::array());

	// Is Key Down named without its signature resolves as the only overload with its pins: stored under its exact type.
	VisualScriptDocument document;
	document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
	document.Graph.Nodes.push_back(MakeNode(11, "Call:Strada.Input.IsKeyDown", Json::object(), Json::object({{"Key", "Space"}})));
	document.Graph.Nodes.push_back(MakeNode(12, "Flow.Branch"));
	document.Graph.Links.push_back(MakeLink(10, "Then", 12, "Execute"));
	document.Graph.Links.push_back(MakeLink(11, "Result", 12, "Condition"));
	Json const given = VisualScriptSerializer::SerializeDocument(document);
	Json const update = Json::array({"node updated: Call:Strada.Input.IsKeyDown → Call:Strada.Input.IsKeyDown(Strada.KeyCode)"});

	std::string const before = fixture.ReadFile("Scripts/Keys.sgraph");
	Json const checked = fixture.Ok("graph.check", Json::object({{"document", given}, {"graph", graph}}));
	CHECK(checked["valid"] == true);
	CHECK(checked["warnings"] == update);
	CHECK(checked["analyzed"] == true);
	CHECK(FindAnalyzedNode(checked["nodes"], 11)["Resolved"] == "Exact");
	CHECK(fixture.ReadFile("Scripts/Keys.sgraph") == before);

	Json const set = fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", given}}));
	CHECK(set["warnings"] == update);
	CHECK(set["analyzed"] == true);
	CHECK(set["diagnostics"] == Json::array());
	Json const& node = FindAnalyzedNode(set["nodes"], 11);
	CHECK(node["Type"] == "Call:Strada.Input.IsKeyDown(Strada.KeyCode)");
	CHECK(node["Resolved"] == "Exact");
	VisualScriptDocument expected = document;
	expected.Graph.Nodes[1].Type = "Call:Strada.Input.IsKeyDown(Strada.KeyCode)";
	CHECK(fixture.GetDocument(graph) == expected);

	// Problems of meaning are no issues: the document is stored, with diagnostics that tell what is wrong.
	VisualScriptDocument unknown = expected;
	unknown.Graph.Nodes.push_back(MakeNode(13, "Flow.Brnach"));
	Json const stored =
		fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(unknown)}}));
	CHECK(stored["changed"] == true);
	Json const& diagnostics = stored["diagnostics"];
	auto const unknownType = std::find_if(diagnostics.begin(), diagnostics.end(),
	                                      [](Json const& diagnostic)
	                                      {
											  return diagnostic["Code"] == "UnknownNodeType";
										  });
	REQUIRE(unknownType != diagnostics.end());
	CHECK((*unknownType)["Severity"] == "Error");
	CHECK((*unknownType)["Node"] == 13);
	CHECK(Contains((*unknownType)["Suggestions"].get<std::vector<std::string>>(), "Flow.Branch"));
	CHECK(fixture.GetDocument(graph) == unknown);
}

TEST_CASE("GraphCommands: graph.set writes an integer given as a float, which the analysis reports, and refuses a read-only file")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	VisualScriptDocument document;
	document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
	document.Graph.Nodes.push_back(MakeNode(11, "Flow.Sequence", Json::object({{"Count", 3}})));
	document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	std::string const graph = fixture.CreateGraph("Scripts/Sequence.sgraph", document);
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	auto const hasInvalidCount = [](Json const& diagnostics)
	{
		return std::any_of(diagnostics.begin(), diagnostics.end(),
		                   [](Json const& diagnostic)
		                   {
							   return diagnostic["Code"] == "InvalidSetting" && diagnostic["Node"] == 11;
						   });
	};

	// operator== takes 3.0 for the stored 3, the scripting runtime does not (a count is an integer): graph.set writes it,
	// and its analysis is of what it wrote.
	Json floating = VisualScriptSerializer::SerializeDocument(document);
	floating["Graph"]["Nodes"][1]["Settings"]["Count"] = 3.0;
	Json const set = fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", floating}}));
	CHECK(set["changed"] == true);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps + 1);
	CHECK(fixture.ReadFile("Scripts/Sequence.sgraph").find("\"Count\": 3.0") != std::string::npos);
	CHECK(hasInvalidCount(set["diagnostics"]));
	// The integer again is another change, which the runtime takes.
	Json const restored =
		fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(document)}}));
	CHECK(restored["changed"] == true);
	CHECK_FALSE(hasInvalidCount(restored["diagnostics"]));

	// A file that is read-only on disk is refused as a file error, on every platform, and no step is recorded.
	{
		Testing::ReadOnlyFileScope const readOnly(fixture.GetFile("Scripts/Sequence.sgraph"));
		CommandError const refused = fixture.Fails("graph.set", Json::object({{"graph", graph}, {"document", floating}}));
		CHECK(refused.Code == AutomationErrorCode::FileError);
		CHECK(refused.Message == "the file of visual script 'Scripts/Sequence.sgraph' is read-only");
	}
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps + 2);
	CHECK(fixture.GetDocument(graph) == document);
}

TEST_CASE("GraphCommands: graph.library and graph.types list the runtime's node kinds and types, filtered and paged")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);
	std::span<VisualScriptNodeTemplate const> const nodes = library->GetNodes();
	REQUIRE(nodes.size() > 50);

	// In library order, 50 at a time unless told otherwise; total counts every match.
	Json const first = fixture.Ok("graph.library");
	CHECK(first["total"] == nodes.size());
	REQUIRE(first["nodes"].size() == 50);
	CHECK(first["nodes"][0] == nodes[0].Source);
	CHECK(first["nodes"][49] == nodes[49].Source);
	Json const page = fixture.Ok("graph.library", Json::object({{"limit", 2}, {"offset", 48}}));
	CHECK(page["total"] == nodes.size());
	CHECK(page["nodes"] == Json::array({nodes[48].Source, nodes[49].Source}));
	Json const past = fixture.Ok("graph.library", Json::object({{"offset", nodes.size() + 5}}));
	CHECK(past["nodes"] == Json::array());
	CHECK(past["total"] == nodes.size());

	// Words in titles, keywords, categories and types, ignoring case.
	std::string const addForce = "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)";
	std::vector<std::string> const forces =
		GetTypes(fixture.Ok("graph.library", Json::object({{"query", "ADD FORCE"}, {"limit", 500}}))["nodes"]);
	CHECK(Contains(forces, addForce));
	CHECK_FALSE(Contains(forces, "Flow.Branch"));

	// A category with the ones below it, ignoring case; part of a category's name is none.
	size_t const flowCount = static_cast<size_t>(std::count_if(nodes.begin(), nodes.end(),
	                                                           [](VisualScriptNodeTemplate const& node)
	                                                           {
																   return node.Category == "Flow" || node.Category.starts_with("Flow/");
															   }));
	Json const flow = fixture.Ok("graph.library", Json::object({{"category", "flow"}, {"limit", 500}}));
	CHECK(flow["total"] == flowCount);
	CHECK(Contains(GetTypes(flow["nodes"]), "Flow.Branch"));
	CHECK(Contains(GetTypes(flow["nodes"]), "Flow.Delay"));
	CHECK(fixture.Ok("graph.library", Json::object({{"category", "Flo"}}))["total"] == 0);
	Json const components = fixture.Ok("graph.library", Json::object({{"category", "Components/"}, {"query", "force"}, {"limit", 500}}));
	CHECK(Contains(GetTypes(components["nodes"]), addForce));
	for (Json const& node : components["nodes"])
	{
		CHECK(node["Category"].get<std::string>().starts_with("Components/"));
	}

	// Nodes with a pin that links to a pin of a type: inputs that take it, outputs that give it, or either.
	VisualScriptPin floatOutput;
	floatOutput.Direction = VisualScriptPinDirection::Out;
	floatOutput.Type = "float";
	size_t const floatInputCount =
		static_cast<size_t>(std::count_if(nodes.begin(), nodes.end(),
	                                      [&](VisualScriptNodeTemplate const& node)
	                                      {
											  return std::any_of(node.Pins.begin(), node.Pins.end(),
		                                                         [&](VisualScriptPin const& pin)
		                                                         {
																	 return pin.Direction == VisualScriptPinDirection::In &&
			                                                                library->CanConnect(floatOutput, pin);
																 });
										  }));
	Json const takingFloats = fixture.Ok("graph.library", Json::object({{"pinType", "float"}, {"direction", "In"}, {"limit", 500}}));
	CHECK(takingFloats["total"] == floatInputCount);
	CHECK(Contains(GetTypes(takingFloats["nodes"]), "Flow.Delay"));
	CHECK_FALSE(Contains(GetTypes(takingFloats["nodes"]), "Event.Update"));
	Json const givingFlow = fixture.Ok("graph.library", Json::object({{"pinType", "exec"}, {"direction", "Out"}, {"limit", 500}}));
	CHECK(Contains(GetTypes(givingFlow["nodes"]), "Flow.Branch"));
	CHECK_FALSE(Contains(GetTypes(givingFlow["nodes"]), "Get:Strada.Time.DeltaTime"));
	Json const floats = fixture.Ok("graph.library", Json::object({{"pinType", "float"}, {"query", "delta"}, {"limit", 500}}));
	CHECK(Contains(GetTypes(floats["nodes"]), "Get:Strada.Time.DeltaTime"));
	CHECK(fixture.Ok("graph.library", Json::object({{"pinType", "float"}}))["total"] > takingFloats["total"]);

	// What cannot be listed.
	CommandError const unknownType = fixture.Fails("graph.library", Json::object({{"pinType", "flaot"}}));
	CHECK(unknownType.Code == AutomationErrorCode::InvalidParams);
	CHECK(unknownType.Data["path"] == "pinType");
	CommandError const directionAlone = fixture.Fails("graph.library", Json::object({{"direction", "In"}}));
	CHECK(directionAlone.Code == AutomationErrorCode::InvalidParams);
	CHECK(directionAlone.Data["path"] == "direction");
	CHECK(fixture.Fails("graph.library", Json::object({{"limit", 0}})).Code == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("graph.library", Json::object({{"limit", 501}})).Code == AutomationErrorCode::InvalidParams);

	// Types by ID, by words in their IDs and titles, and by kind.
	std::span<VisualScriptTypeInfo const> const types = library->GetTypes();
	Json const firstTypes = fixture.Ok("graph.types", Json::object({{"limit", 3}, {"offset", 1}}));
	CHECK(firstTypes["total"] == types.size());
	CHECK(firstTypes["types"] == Json::array({types[1].Source, types[2].Source, types[3].Source}));
	Json const vectors = fixture.Ok("graph.types", Json::object({{"query", "VECTOR3"}, {"limit", 500}}));
	std::vector<std::string> vectorIds;
	for (Json const& type : vectors["types"])
	{
		vectorIds.push_back(type["ID"].get<std::string>());
	}
	CHECK(Contains(vectorIds, "Strada.Vector3"));
	CHECK_FALSE(Contains(vectorIds, "float"));
	Json const enums = fixture.Ok("graph.types", Json::object({{"kind", "Enum"}, {"limit", 500}}));
	CHECK(enums["total"] == static_cast<size_t>(std::count_if(types.begin(), types.end(),
	                                                          [](VisualScriptTypeInfo const& type)
	                                                          {
																  return type.Kind == VisualScriptTypeKind::Enum;
															  })));
	bool keyCode = false;
	for (Json const& type : enums["types"])
	{
		CHECK(type["Kind"] == "Enum");
		keyCode = keyCode || type["ID"] == "Strada.KeyCode";
	}
	CHECK(keyCode);
	CHECK(fixture.Fails("graph.types", Json::object({{"kind", "Animal"}})).Code == AutomationErrorCode::InvalidParams);
}

TEST_CASE("GraphCommands: graph.set while playing reaches the running graph, and a waiting Delay survives the edit and a refresh")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Delay.sgraph", MakeDelayDocument());
	std::string const waiter = fixture.CreateEntity("Waiter", Json::object({{"VisualScript", Json::object({{"Graph", graph}})}}));
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	bool const dirty = fixture.Context.IsDirty();
	std::string const bytes = fixture.ReadFile("Scripts/Delay.sgraph");
	auto const done = [&]
	{
		// What the running graph holds (members it lacks read as null).
		Json entity = fixture.Ok("entity.get", Json::object({{"entity", waiter}}));
		return entity["components"]["VisualScript"]["Variables"]["Done"]["Value"];
	};

	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(12, Timestep(FrameTime));
	CHECK(done() == false);

	// An edit that leaves the Delay alone: the running graph takes it at its next update, and the Delay keeps waiting.
	VisualScriptDocument edited = MakeDelayDocument();
	edited.Description = "Waits half a second";
	uint64_t const logStart = Log::GetNextEntryIndex();
	Json const set =
		fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(edited)}}));
	CHECK(set["changed"] == true);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Edit Visual Script");
	fixture.Play.Advance(1, Timestep(FrameTime));
	// The file changed after it was loaded: a refresh reloads it, an equal document the running graph does not take again. Its
	// modification time moves past every earlier one, so the change is noticed even on file systems with coarse timestamps.
	std::filesystem::last_write_time(fixture.GetFile("Scripts/Delay.sgraph"),
	                                 std::filesystem::file_time_type::clock::now() + std::chrono::hours(1));
	Json const refreshed = fixture.Ok("asset.refresh");
	CHECK(refreshed["modified"].size() == 1);
	fixture.Play.Advance(6, Timestep(FrameTime));
	CHECK(done() == false);
	fixture.Play.Advance(15, Timestep(FrameTime));
	CHECK(done() == true);
	CHECK_FALSE(Testing::WasLogged(logStart, "cancelled"));

	// The step stays undoable after Stop, and the scene has the unsaved changes it had before playing. Undo gives the file
	// its bytes from before the edit (the asset was reloaded by the refresh meanwhile), redo the edit.
	std::string const edit = fixture.ReadFile("Scripts/Delay.sgraph");
	fixture.Play.Stop();
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Edit Visual Script");
	CHECK(fixture.Context.IsDirty() == dirty);
	CHECK(fixture.Ok("graph.get", Json::object({{"graph", graph}}))["document"]["Description"] == "Waits half a second");
	CHECK(fixture.Ok("editor.undo")["undone"] == "Edit Visual Script");
	CHECK(fixture.ReadFile("Scripts/Delay.sgraph") == bytes);
	CHECK(fixture.Ok("graph.get", Json::object({{"graph", graph}}))["document"]["Description"] == "");
	CHECK(fixture.Context.IsDirty() == dirty);
	fixture.Ok("editor.redo");
	CHECK(fixture.ReadFile("Scripts/Delay.sgraph") == edit);
	CHECK(fixture.Context.IsDirty() == dirty);
}

TEST_CASE("GraphCommands: while playing, visual script variables are the running graph's values, and edits reach it at once")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const counter = fixture.CreateEntity(
		"Counter",
		Json::object({{"VisualScript", Json::object({{"Graph", graph}, {"Variables", Json::object({{"Speed", StoredInt(2)}})}})}}));
	std::optional<UUID> const id = UUID::FromString(counter);
	REQUIRE(id.has_value());
	auto const variable = [&](std::string const& name)
	{
		Result<ScriptFieldMap> const values = VisualScriptEngine::GetInstanceVariables(*id);
		REQUIRE_MESSAGE(values.IsOk(), (values ? std::string() : values.GetError()));
		return values.GetValue().at(name);
	};
	auto const setVariables = [&](Json variables)
	{
		fixture.Ok("component.set", Json::object({{"entity", counter},
		                                          {"component", "VisualScript"},
		                                          {"fields", Json::object({{"Variables", std::move(variables)}})}}));
	};

	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(3, Timestep(FrameTime));
	// entity.get and component.get show the values the graph holds.
	CHECK(fixture.Ok("entity.get", Json::object({{"entity", counter}}))["components"]["VisualScript"]["Variables"]["Updates"] ==
	      StoredInt(6));
	fixture.Play.Advance(1, Timestep(FrameTime));
	Json const component = fixture.Ok("component.get", Json::object({{"entity", counter}, {"component", "VisualScript"}}));
	CHECK(component["fields"]["Variables"] == Json::object({{"Speed", StoredInt(2)}, {"Updates", StoredInt(8)}}));

	// An edit reaches the graph at once, also while paused; variables the patch leaves out keep the graph's values, and
	// undoing gives back what the graph had.
	fixture.Play.SetPaused(true);
	setVariables(Json::object({{"Speed", StoredInt(5)}}));
	CHECK(variable("Speed") == ScriptFieldValue::FromInt32(5));
	CHECK(variable("Updates") == ScriptFieldValue::FromInt32(8));
	fixture.Play.Advance(1, Timestep(FrameTime));
	CHECK(variable("Updates") == ScriptFieldValue::FromInt32(13));
	fixture.Ok("editor.undo");
	CHECK(variable("Speed") == ScriptFieldValue::FromInt32(2));
	fixture.Ok("editor.redo");
	CHECK(variable("Speed") == ScriptFieldValue::FromInt32(5));

	// Edits start from what the graph holds: setting a variable it changed itself reaches it, and undoing gives back the value
	// it had.
	fixture.Play.SetPaused(false);
	fixture.Play.Advance(2, Timestep(FrameTime));
	CHECK(variable("Updates") == ScriptFieldValue::FromInt32(23));
	setVariables(Json::object({{"Updates", StoredInt(0)}}));
	CHECK(variable("Updates") == ScriptFieldValue::FromInt32(0));
	CHECK(variable("Speed") == ScriptFieldValue::FromInt32(5));
	fixture.Ok("editor.undo");
	CHECK(variable("Updates") == ScriptFieldValue::FromInt32(23));

	// Stopping discards what was edited.
	fixture.Play.Stop();
	CHECK(fixture.Ok("component.get", Json::object({{"entity", counter}, {"component", "VisualScript"}}))["fields"]["Variables"] ==
	      Json::object({{"Speed", StoredInt(2)}}));
}

TEST_CASE("GraphCommands: while paused, a graph.set that renames a variable and an edit of it before the next update both reach the graph")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const counter = fixture.CreateEntity(
		"Counter",
		Json::object({{"VisualScript", Json::object({{"Graph", graph}, {"Variables", Json::object({{"Speed", StoredInt(2)}})}})}}));
	std::optional<UUID> const id = UUID::FromString(counter);
	REQUIRE(id.has_value());
	auto const variable = [&](std::string const& name)
	{
		Result<ScriptFieldMap> const values = VisualScriptEngine::GetInstanceVariables(*id);
		REQUIRE_MESSAGE(values.IsOk(), (values ? std::string() : values.GetError()));
		return values.GetValue().at(name);
	};
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(2, Timestep(FrameTime));
	fixture.Play.SetPaused(true);
	CHECK(variable("Updates") == ScriptFieldValue::FromInt32(4));

	// The rename keeps Speed as a former name; the running graph takes it at its next update. The renamed variable edited
	// before then takes the edit with it.
	VisualScriptDocument renamed = MakeCounterDocument();
	renamed.Variables[0].Name = "Velocity";
	Json const set =
		fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(renamed)}}));
	CHECK(set["warnings"] == Json::array({"variable renamed: Speed → Velocity (Speed is kept in FormerNames)"}));
	fixture.Ok("component.set", Json::object({{"entity", counter},
	                                          {"component", "VisualScript"},
	                                          {"fields", Json::object({{"Variables", Json::object({{"Velocity", StoredInt(50)}})}})}}));
	CHECK(variable("Speed") == ScriptFieldValue::FromInt32(2));
	fixture.Play.Advance(1, Timestep(FrameTime));
	CHECK(variable("Velocity") == ScriptFieldValue::FromInt32(50));
	CHECK(variable("Updates") == ScriptFieldValue::FromInt32(54));
	CHECK(fixture.Ok("entity.get", Json::object({{"entity", counter}}))["components"]["VisualScript"]["Variables"] ==
	      Json::object({{"Updates", StoredInt(54)}, {"Velocity", StoredInt(50)}}));
	fixture.Play.Stop();
}

TEST_CASE(
	"VisualScriptEnumLookupSource: the inspector's enum lookup reads the node library only when asked, and is empty without the runtime")
{
	// Without the runtime: an empty lookup (enum variables need the runtime), given once.
	VisualScriptEnumLookupSource source;
	std::optional<VisualScriptEnumLookup> const none = source.Update();
	REQUIRE(none.has_value());
	CHECK_FALSE(*none);
	CHECK_FALSE(source.Update().has_value());
	{
		Testing::ScriptRuntimeScope runtime;
		std::optional<VisualScriptEnumLookup> const lookup = source.Update();
		REQUIRE(lookup.has_value());
		REQUIRE(*lookup);
		CHECK_FALSE(source.Update().has_value());
		// Giving the lookup reads nothing; asking it about a type reads the library.
		CHECK(VisualScriptEngine::GetLibraryState() == VisualScriptLibraryState::NotBuilt);
		std::optional<VisualScriptEnumInfo> const modes = (*lookup)("Strada.ForceMode");
		REQUIRE(modes.has_value());
		CHECK(modes->UnderlyingType == ScriptFieldType::Int32);
		CHECK_FALSE((*lookup)("float").has_value());
		CHECK(VisualScriptEngine::GetLibraryState() == VisualScriptLibraryState::Built);

		// A game-assembly change drops the library: the same lookup reads it again when next asked.
		REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
		CHECK(VisualScriptEngine::GetLibraryState() == VisualScriptLibraryState::NotBuilt);
		CHECK_FALSE(source.Update().has_value());
		CHECK((*lookup)("Strada.KeyCode").has_value());
		CHECK(VisualScriptEngine::GetLibraryState() == VisualScriptLibraryState::Built);
	}

	// The runtime stopped: an empty lookup again.
	std::optional<VisualScriptEnumLookup> const stopped = source.Update();
	REQUIRE(stopped.has_value());
	CHECK_FALSE(*stopped);
	CHECK_FALSE(source.Update().has_value());
}

// The commands of the graph windows (spec §13): an editor with windows, drawn headless between requests.
namespace
{
	VisualScriptNode MakePlacedNode(uint32_t id, std::string type, glm::vec2 position, Json settings = Json::object(),
	                                Json inputs = Json::object())
	{
		VisualScriptNode node = MakeNode(id, std::move(type), std::move(settings), std::move(inputs));
		node.Position = position;
		return node;
	}

	// Something of everything the window commands reach. The event graph: Create -> Log "Hello", a Get of Speed, a custom
	// event Hit with its sender, a call of AddScore and a comment box. The function AddScore (30): Entry -> Set Score, a local
	// Bonus and a comment box.
	VisualScriptDocument MakeWindowDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Speed", "float", 2.5));
		document.Variables.push_back(MakeVariable(2, "Score", "int", 0));
		document.Graph.Nodes = {
			MakePlacedNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
			MakePlacedNode(11, "Call:Strada.Log.Info(string)", glm::vec2(400.0f, 0.0f), Json::object(),
		                   Json::object({{"Message", "Hello"}})),
			MakePlacedNode(12, "Variable.Get", glm::vec2(100.0f, 300.0f), Json::object({{"Variable", 1}})),
			MakePlacedNode(13, "Event.Custom", glm::vec2(0.0f, 600.0f), Json::object({{"Name", "Hit"}})),
			MakePlacedNode(14, "Event.Send", glm::vec2(400.0f, 600.0f), Json::object({{"Name", "Hit"}})),
			MakePlacedNode(15, "Function.Call", glm::vec2(1600.0f, 1200.0f), Json::object({{"Function", 30}})),
		};
		document.Graph.Links = {MakeLink(10, "Then", 11, "Execute"), MakeLink(13, "Then", 15, "Execute")};
		VisualScriptComment comment;
		comment.ID = 20;
		comment.Text = "Start here";
		comment.Position = glm::vec2(-40.0f, -80.0f);
		comment.Size = glm::vec2(800.0f, 240.0f);
		document.Graph.Comments.push_back(comment);

		VisualScriptFunction function;
		function.ID = 30;
		function.Name = "AddScore";
		function.Locals.push_back(VisualScriptLocal{31, "Bonus", "int", Json()});
		function.Graph.Nodes = {MakePlacedNode(32, "Function.Entry", glm::vec2(0.0f, 0.0f)),
		                        MakePlacedNode(33, "Variable.Set", glm::vec2(400.0f, 0.0f), Json::object({{"Variable", 2}}))};
		function.Graph.Links = {MakeLink(32, "Then", 33, "Execute")};
		comment.ID = 34;
		comment.Text = "Adds";
		function.Graph.Comments.push_back(comment);
		document.Functions.push_back(std::move(function));
		return document;
	}

	// The commands of an editor that has graph windows, as the editor layer sets them up, and the windows drawn headless
	// between requests as the editor draws them every frame. The fixture library stands in for the scripting runtime; views
	// move at once (Reduce Motion) and the first canvas's hint counts as seen.
	struct GraphWindowFixture
	{
		Testing::HeadlessImGui Gui;
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		Testing::FixtureVisualScriptSemantics Semantics;
		std::filesystem::path ViewsFile = Directory.GetPath() / "UserData" / "Editor" / "VisualScriptViews.json";
		VisualScriptViewSettings Settings{ViewsFile};
		VisualScriptEditors Editors{Operations, Semantics, Settings};
		CommandRegistry Registry;
		bool DialogOpen = false;

		GraphWindowFixture()
		{
			Gui.UseEditorStyle();
			Settings.SetHintSeen(VisualScriptCanvasHint);
			Settings.SetReduceMotion(true);
			EditorCommandEnvironment environment;
			environment.GraphEditors = [this]() -> VisualScriptEditors&
			{
				return Editors;
			};
			environment.IsDialogOpen = [this]
			{
				return DialogOpen;
			};
			REQUIRE(RegisterEditorCommands(Registry, Operations, std::move(environment)).IsOk());
			REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
		}

		void Frames(int count)
		{
			for (int i = 0; i < count; i++)
			{
				Gui.Frame(
					[this]
					{
						Editors.OnImGuiRender();
					});
			}
		}

		// Runs a command; an asynchronous one gets frames drawn until it answers, at most frames of them.
		CommandResult Run(std::string_view name, Json params = Json::object(), int frames = 4)
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			for (int frame = 0; frame < frames && !result; frame++)
			{
				Frames(1);
			}
			REQUIRE(result.has_value());
			return std::move(*result);
		}

		Json Ok(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
			return result.TakeValue();
		}

		CommandError Fails(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE(result.IsError());
			return result.TakeError();
		}

		// The code and data.path of a failure.
		std::pair<AutomationErrorCode, std::string> Failure(std::string_view name, Json params)
		{
			CommandError const error = Fails(name, std::move(params));
			std::string path;
			if (error.Data.is_object() && error.Data.contains("path"))
			{
				path = error.Data["path"].get<std::string>();
			}
			return {error.Code, path};
		}

		std::string CreateGraph(std::string const& path, VisualScriptDocument const& document)
		{
			std::string const graph = Ok("graph.create", Json::object({{"path", path}}))["asset"].get<std::string>();
			Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(document)}}));
			return graph;
		}

		// graph.editors' entry of the graph's window.
		Json Editor(std::string const& graph)
		{
			Json const editors = Ok("graph.editors")["editors"];
			for (Json const& editor : editors)
			{
				if (editor["asset"] == graph)
				{
					return editor;
				}
			}
			FAIL("the graph has no window");
			return Json();
		}

		VisualScriptDocument GetDocument(std::string const& graph) const
		{
			Result<AssetHandle> const handle = AssetManager::ResolveReference(graph);
			REQUIRE(handle.IsOk());
			Ref<VisualScriptAsset> const script = AssetManager::GetAsset<VisualScriptAsset>(handle.GetValue());
			REQUIRE(script != nullptr);
			return script->GetDocument();
		}

		std::vector<std::string> ListOpen()
		{
			std::vector<std::string> open;
			Json const editors = Ok("graph.editors")["editors"];
			for (Json const& editor : editors)
			{
				open.push_back(editor["asset"].get<std::string>());
			}
			return open;
		}
	};

	glm::vec2 ReadPoint(Json const& point)
	{
		return glm::vec2(point[0].get<float>(), point[1].get<float>());
	}

	// graph.find's results as "<node, comment, variable or else function ID>:<field>".
	std::vector<std::string> ListFound(Json const& found)
	{
		std::vector<std::string> results;
		for (Json const& result : found["results"])
		{
			uint32_t const id = result.contains("node")       ? result["node"].get<uint32_t>()
			                    : result.contains("comment")  ? result["comment"].get<uint32_t>()
			                    : result.contains("variable") ? result["variable"].get<uint32_t>()
			                                                  : result["function"].get<uint32_t>();
			results.push_back(fmt::format("{}:{}", id, result["field"].get<std::string>()));
		}
		return results;
	}
}

TEST_CASE("GraphCommands: the graph window commands need the editor's UI, and graph.find works headless")
{
	GraphFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Window.sgraph", MakeWindowDocument());
	for (std::string_view const name : {"graph.open", "graph.palette"})
	{
		CAPTURE(name);
		CHECK(fixture.Fails(name, Json::object({{"graph", graph}})).Code == AutomationErrorCode::Unavailable);
	}
	CHECK(fixture.Fails("graph.close").Code == AutomationErrorCode::Unavailable);
	CHECK(fixture.Fails("graph.editors").Code == AutomationErrorCode::Unavailable);

	// Without the runtime, titles are derived and nothing is analyzed.
	Json const found = fixture.Ok("graph.find", Json::object({{"query", "hello"}}));
	CHECK(found["analyzed"] == false);
	CHECK(found["total"] == 1);
	CHECK(ListFound(found) == std::vector<std::string>{"11:Value"});
	CHECK(found["results"][0]["asset"] == graph);
	CHECK(found["results"][0]["path"] == "Scripts/Window.sgraph");
	CHECK(found["results"][0]["function"] == 0);
	CHECK(found["results"][0]["text"] == "\"Hello\"");
}

TEST_CASE("GraphCommands: graph.open opens a window that its next draw applies, and graph.editors reports what it shows")
{
	GraphWindowFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Window.sgraph", MakeWindowDocument());
	std::string const entity =
		fixture
			.Ok("entity.create", Json::object({{"name", "Runner"},
	                                           {"components", Json::object({{"VisualScript", Json::object({{"Graph", graph}})}})}}))["id"]
			.get<std::string>();

	// The window exists at once, in its state before its first draw.
	Json opened = fixture.Ok("graph.open", Json::object({{"graph", graph}, {"entity", entity}}));
	CHECK(opened["asset"] == graph);
	CHECK(opened["path"] == "Scripts/Window.sgraph");
	CHECK(opened["function"] == 0);
	CHECK(opened["selection"] == Json::array());
	CHECK(opened["loaded"] == true);
	CHECK(opened["warnings"] == Json::array());
	Json editor = fixture.Editor(graph);
	CHECK(editor["visible"] == false);
	CHECK(editor["focused"] == false);

	// Drawn, it shows the event graph, focused, with its context entity.
	fixture.Frames(3);
	editor = fixture.Editor(graph);
	CHECK(editor["visible"] == true);
	CHECK(editor["focused"] == true);
	CHECK(editor["loaded"] == true);
	CHECK(editor["readOnly"] == false);
	CHECK(editor["analyzed"] == true);
	CHECK(editor["errors"] == 0);
	CHECK(editor["contextEntity"] == entity);
	CHECK(Contains({"Detail", "Compact", "Overview", "Far"}, editor["view"]["level"].get<std::string>()));
	CHECK(editor["selection"] == Json::object({{"nodes", Json::array()}, {"comments", Json::array()}}));
	CHECK(editor["palette"] == Json::object({{"open", false}, {"query", ""}}));
	CHECK(editor["notices"] == Json::array());
	CHECK(editor["card"] == "None");
	CHECK(editor["toasts"] == Json::array());
	CHECK(editor["live"] == "None");
	CHECK(editor["save"] == "Saved");
	CHECK(editor["drawer"] == "Closed");
	CHECK(editor["find"] == Json::object({{"open", false}, {"text", ""}, {"index", 0}, {"count", 0}}));
	CHECK(editor["bookmarks"] == Json::array());
	CHECK(editor["panes"] == Json::object({{"sidebar", "Shown"}, {"details", "Shown"}}));
	CHECK(editor["shortcuts"] == false);

	// A function's graph with a node and its comment selected, the node the primary selection; the problems drawer.
	opened = fixture.Ok("graph.open", Json::object({{"graph", graph}, {"function", 30}, {"select", {33, 34}}, {"problems", true}}));
	CHECK(opened["function"] == 30);
	CHECK(opened["selection"] == Json::array({33, 34}));
	fixture.Frames(2);
	editor = fixture.Editor(graph);
	CHECK(editor["function"] == 30);
	CHECK(editor["selection"] == Json::object({{"nodes", Json::array({33})}, {"comments", Json::array({34})}}));
	CHECK(editor["drawer"] == "Problems");
	// Without a function, the sub-graph shown stays and select names its elements.
	CHECK(fixture.Ok("graph.open", Json::object({{"graph", graph}, {"select", Json::array({32})}}))["function"] == 30);
	fixture.Frames(2);
	CHECK(fixture.Editor(graph)["selection"]["nodes"] == Json::array({32}));

	// find opens the find bar and goes to the first match, which becomes the selection.
	fixture.Ok("graph.open", Json::object({{"graph", graph}, {"function", 0}, {"find", "Hello"}}));
	fixture.Frames(3);
	editor = fixture.Editor(graph);
	CHECK(editor["function"] == 0);
	CHECK(editor["find"]["open"] == true);
	CHECK(editor["find"]["text"] == "Hello");
	CHECK(editor["find"]["index"] == 1);
	CHECK(editor["find"]["count"] == 1);
	CHECK(editor["selection"]["nodes"] == Json::array({11}));

	// Closed and opened again without a function, it shows the sub-graph the views file kept, which select names.
	fixture.Ok("graph.open", Json::object({{"graph", graph}, {"function", 30}}));
	fixture.Frames(2);
	fixture.Ok("graph.close", Json::object({{"graph", graph}}));
	opened = fixture.Ok("graph.open", Json::object({{"graph", graph}, {"select", Json::array({32})}}));
	CHECK(opened["function"] == 30);
	fixture.Frames(2);
	CHECK(fixture.Editor(graph)["function"] == 30);
	CHECK(fixture.Editor(graph)["selection"]["nodes"] == Json::array({32}));

	// Opening it again keeps one window; a second graph opens a second, in tab order.
	std::string const other = fixture.CreateGraph("Scripts/Other.sgraph", MakeWindowDocument());
	fixture.Ok("graph.open", Json::object({{"graph", graph}}));
	fixture.Ok("graph.open", Json::object({{"graph", other}}));
	fixture.Frames(2);
	CHECK(fixture.ListOpen() == std::vector<std::string>{graph, other});
	CHECK(fixture.Editor(other)["focused"] == true);
	CHECK(fixture.Editor(graph)["focused"] == false);
}

TEST_CASE("GraphCommands: graph.open frames, then zooms around the framed centre, then centres, in every combination")
{
	GraphWindowFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Window.sgraph", MakeWindowDocument());
	fixture.Ok("graph.open", Json::object({{"graph", graph}}));
	fixture.Frames(2);

	// What each framing shows on its own: the view it had (Keep: the first one frames the whole graph, never shown before),
	// node 11 (Selection) or the whole event graph (All).
	auto const view = [&fixture, &graph](Json params)
	{
		params["graph"] = graph;
		params["select"] = Json::array({11});
		fixture.Ok("graph.open", params);
		fixture.Frames(2);
		Json const shown = fixture.Editor(graph)["view"];
		return std::pair<glm::vec2, float>(ReadPoint(shown["center"]), shown["zoom"].get<float>());
	};
	auto const kept = view(Json::object({{"frame", "Keep"}}));
	auto const selection = view(Json::object({{"frame", "Selection"}}));
	auto const all = view(Json::object({{"frame", "All"}}));
	CHECK(kept == all);
	CHECK(selection.first != all.first);
	CHECK(selection.second != all.second);
	CHECK(view(Json::object()) == selection);
	CHECK(view(Json::object({{"frame", "Keep"}})) == selection);

	glm::vec2 const center(123.0f, -456.0f);
	for (char const* const frame : {"Keep", "Selection", "All"})
	{
		for (bool const zoom : {false, true})
		{
			for (bool const centered : {false, true})
			{
				CAPTURE(frame);
				CAPTURE(zoom);
				CAPTURE(centered);
				// Back to the view kept first, so Keep keeps it.
				view(Json::object({{"frame", "Keep"}, {"zoom", kept.second}, {"center", {kept.first.x, kept.first.y}}}));
				Json params = Json::object({{"frame", frame}});
				if (zoom)
				{
					params["zoom"] = 1.25;
				}
				if (centered)
				{
					params["center"] = Json::array({center.x, center.y});
				}
				auto const framed = std::string_view(frame) == "Keep" ? kept : std::string_view(frame) == "Selection" ? selection : all;
				auto const [shownCenter, shownZoom] = view(params);
				CHECK(shownZoom == doctest::Approx(zoom ? 1.25f : framed.second));
				glm::vec2 const expected = centered ? center : framed.first;
				CHECK(shownCenter.x == doctest::Approx(expected.x).epsilon(0.001));
				CHECK(shownCenter.y == doctest::Approx(expected.y).epsilon(0.001));
			}
		}
	}
}

TEST_CASE("GraphCommands: graph.open refuses what the graph or the scene does not have, and opens a file that does not load on "
          "its card")
{
	GraphWindowFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Window.sgraph", MakeWindowDocument());
	std::string const material =
		fixture.Ok("material.create", Json::object({{"path", "Materials/Red.smat"}}))["material"]["id"].get<std::string>();
	using Failure = std::pair<AutomationErrorCode, std::string>;
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", "asset://Scripts/Missing.sgraph"}})) ==
	      Failure{AutomationErrorCode::AssetNotFound, "graph"});
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", material}})) == Failure{AutomationErrorCode::InvalidParams, "graph"});
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", graph}, {"function", 99}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "function"});
	// select is checked against the sub-graph shown: 33 is a node of the function, not of the event graph.
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", graph}, {"select", {11, 33}}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "select[1]"});
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", graph}, {"function", 30}, {"select", Json::array({11})}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "select[0]"});
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", graph}, {"entity", "not-an-id"}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "entity"});
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", graph}, {"entity", "12345"}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "entity"});
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", graph}, {"zoom", std::numeric_limits<double>::quiet_NaN()}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "zoom"});
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", graph}, {"center", {0.0, std::numeric_limits<double>::quiet_NaN()}}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "center"});
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", graph}, {"center", {0.0, 2.0e7}}})).first ==
	      AutomationErrorCode::InvalidParams);
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", graph}, {"zoom", 3.0}})).first == AutomationErrorCode::InvalidParams);
	fixture.DialogOpen = true;
	CHECK(fixture.Fails("graph.open", Json::object({{"graph", graph}})).Code == AutomationErrorCode::InvalidOperation);
	fixture.DialogOpen = false;
	CHECK(fixture.ListOpen().empty());

	// A file that does not load opens on its card; what needs its document is ignored, which the warnings say.
	std::string const damaged = fixture.Ok("graph.create", Json::object({{"path", "Scripts/Damaged.sgraph"}}))["asset"].get<std::string>();
	REQUIRE(FileSystem::WriteTextFile(AssetManager::GetAssetDirectory() / "Scripts" / "Damaged.sgraph", "{ \"Strada\": 1 }").IsOk());
	Json const opened =
		fixture.Ok("graph.open", Json::object({{"graph", damaged}, {"function", 30}, {"select", Json::array({11})}, {"find", "x"}}));
	CHECK(opened["loaded"] == false);
	CHECK(opened["selection"] == Json::array());
	REQUIRE(opened["warnings"].size() == 3);
	CHECK(opened["warnings"][0].get<std::string>().starts_with("function is ignored: the visual script does not load"));
	CHECK(opened["warnings"][1].get<std::string>().starts_with("select is ignored"));
	CHECK(opened["warnings"][2].get<std::string>().starts_with("find is ignored"));
	fixture.Frames(2);
	Json const editor = fixture.Editor(damaged);
	CHECK(editor["loaded"] == false);
	CHECK(editor["card"] == "Unloadable");
	CHECK(editor["readOnly"] == false);
}

TEST_CASE("GraphCommands: graph.close closes one window or all and writes the views file, and a deleted graph's window is gone")
{
	GraphWindowFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Window.sgraph", MakeWindowDocument());
	std::string const other = fixture.CreateGraph("Scripts/Other.sgraph", MakeWindowDocument());
	std::string const third = fixture.CreateGraph("Scripts/Third.sgraph", MakeWindowDocument());
	for (std::string const& open : {graph, other, third})
	{
		fixture.Ok("graph.open", Json::object({{"graph", open}}));
	}
	fixture.Frames(2);
	CHECK_FALSE(FileSystem::Exists(fixture.ViewsFile));

	CHECK(fixture.Ok("graph.close", Json::object({{"graph", other}}))["closed"] == Json::array({other}));
	CHECK(fixture.ListOpen() == std::vector<std::string>{graph, third});
	CHECK(FileSystem::Exists(fixture.ViewsFile));
	// Not open: nothing to close, and no error.
	CHECK(fixture.Ok("graph.close", Json::object({{"graph", other}}))["closed"] == Json::array());
	CHECK(fixture.Failure("graph.close", Json::object({{"graph", "asset://Scripts/Missing.sgraph"}})) ==
	      std::pair<AutomationErrorCode, std::string>{AutomationErrorCode::AssetNotFound, "graph"});

	// A graph deleted after its window opened: graph.editors no longer lists it and closing it closes nothing (spec §13.8).
	fixture.Ok("asset.delete", Json::object({{"asset", third}}));
	CHECK(fixture.ListOpen() == std::vector<std::string>{graph});
	CHECK(fixture.Ok("graph.close", Json::object({{"graph", third}}))["closed"] == Json::array());

	// Every window.
	fixture.Ok("graph.open", Json::object({{"graph", other}}));
	CHECK(fixture.Ok("graph.close")["closed"] == Json::array({graph, other}));
	CHECK(fixture.ListOpen().empty());
	fixture.Frames(1);
	VisualScriptViewSettings saved(fixture.ViewsFile);
	CHECK(saved.Load().empty());
}

TEST_CASE("GraphCommands: graph.palette checks its parameters in order, then lists the palette's entries as it ranks them")
{
	GraphWindowFixture fixture;
	VisualScriptDocument const document = MakeWindowDocument();
	std::string const graph = fixture.CreateGraph("Scripts/Window.sgraph", document);
	using Failure = std::pair<AutomationErrorCode, std::string>;
	auto const params = [&graph](Json extra)
	{
		extra["graph"] = graph;
		return extra;
	};
	Json const fromCreate = Json::object({{"node", 10}, {"pin", "Then"}, {"direction", "Out"}});

	// The window must be open.
	CHECK(fixture.Failure("graph.palette", Json::object({{"graph", "asset://Scripts/Missing.sgraph"}})) ==
	      Failure{AutomationErrorCode::AssetNotFound, "graph"});
	CHECK(fixture.Fails("graph.palette", params(Json::object())).Code == AutomationErrorCode::InvalidOperation);
	fixture.Ok("graph.open", Json::object({{"graph", graph}}));
	fixture.DialogOpen = true;
	CHECK(fixture.Fails("graph.palette", params(Json::object())).Code == AutomationErrorCode::InvalidOperation);
	fixture.DialogOpen = false;

	// The parameters are checked before the library is asked for: without it too.
	fixture.Semantics.SetAvailable(false);
	for (Json const& from : {Json::object({{"node", 99}, {"pin", "Then"}, {"direction", "Out"}}),
	                         Json::object({{"node", 33}, {"pin", "Then"}, {"direction", "Out"}})})
	{
		CHECK(fixture.Failure("graph.palette", params(Json::object({{"from", from}}))) ==
		      Failure{AutomationErrorCode::InvalidParams, "from.node"});
	}
	for (char const* const pin : {"", "Hit.", ".Point", "A..B"})
	{
		CAPTURE(pin);
		CHECK(fixture.Failure("graph.palette", params(Json::object({{"from", {{"node", 10}, {"pin", pin}, {"direction", "Out"}}}}))) ==
		      Failure{AutomationErrorCode::InvalidParams, "from.pin"});
	}
	for (Json const& at : {Json::array({0.0, 2.0e7}), Json::array({std::numeric_limits<double>::quiet_NaN(), 0.0}),
	                       Json::array({-std::numeric_limits<double>::infinity(), 0.0})})
	{
		CHECK(fixture.Failure("graph.palette", params(Json::object({{"at", at}}))) == Failure{AutomationErrorCode::InvalidParams, "at"});
	}
	CommandError const degraded = fixture.Fails("graph.palette", params(Json::object({{"from", fromCreate}})));
	CHECK(degraded.Code == AutomationErrorCode::Unavailable);
	CHECK(degraded.Message == "the node palette needs the scripting runtime");
	fixture.Semantics.SetAvailable(true);
	fixture.Frames(2);
	// A pin the node does not have is known once the window has the node's pins.
	CHECK(fixture.Failure("graph.palette", params(Json::object({{"from", {{"node", 10}, {"pin", "Nope"}, {"direction", "Out"}}}}))) ==
	      Failure{AutomationErrorCode::InvalidParams, "from.pin"});

	// The entries of a query are the palette's ranking of it, a frame later, and the window says its palette is open.
	VisualScriptLibrary const& library = fixture.Semantics.GetFixtureLibrary();
	VisualScriptPaletteIndex index;
	index.Rebuild(library);
	index.SetDocument(document, 0);
	std::span<VisualScriptPaletteEntry const> const entries = index.GetEntries();
	auto const expectTitles = [&entries](Json const& listed, std::vector<VisualScriptPaletteMatch> const& matches, size_t limit)
	{
		CHECK(listed["total"] == matches.size());
		REQUIRE(listed["entries"].size() == std::min(matches.size(), limit));
		for (size_t i = 0; i < listed["entries"].size(); i++)
		{
			CAPTURE(i);
			VisualScriptPaletteEntry const& entry = entries[matches[i].Entry];
			Json const& row = listed["entries"][i];
			CHECK(row["title"] == entry.Title);
			CHECK(row["subtitle"] == entry.Subtitle);
			CHECK(row["category"] == entry.Category);
			CHECK(row["type"] == entry.NodeType);
			CHECK(row["settings"] == entry.Settings);
		}
	};
	std::vector<VisualScriptPaletteMatch> const branch = RankVisualScriptPalette(index, "br", {}, nullptr, library);
	REQUIRE(branch.size() > 2);
	Json listed = fixture.Ok("graph.palette", params(Json::object({{"query", "br"}, {"limit", 2}, {"at", {100.0, 200.0}}})));
	CHECK(listed["open"] == true);
	CHECK(listed["query"] == "br");
	CHECK(listed["context"] == false);
	expectTitles(listed, branch, 2);
	CHECK_FALSE(listed["entries"][0].contains("connects"));
	CHECK(fixture.Editor(graph)["palette"] == Json::object({{"open", true}, {"query", "br"}}));

	// Context mode: what connects to the pin a wire is dragged from, with the pin each entry links to.
	VisualScriptNodeTemplate const* create = library.FindNode("Event.Create");
	REQUIRE(create != nullptr);
	VisualScriptPaletteContext context;
	context.Pin = create->Pins.front();
	std::vector<VisualScriptPaletteMatch> const connected = RankVisualScriptPalette(index, "", {}, &context, library);
	REQUIRE_FALSE(connected.empty());
	listed = fixture.Ok("graph.palette", params(Json::object({{"from", fromCreate}})));
	CHECK(listed["context"] == true);
	expectTitles(listed, connected, 50);
	for (size_t i = 0; i < listed["entries"].size(); i++)
	{
		CHECK(listed["entries"][i]["connects"] == connected[i].ConnectPin);
		CHECK(listed["entries"][i]["converts"] == connected[i].Converts);
	}

	// Closed; nothing in the document changed.
	listed = fixture.Ok("graph.palette", params(Json::object({{"open", false}})));
	CHECK(listed["open"] == false);
	CHECK(listed["entries"] == Json::array());
	fixture.Frames(1);
	CHECK(fixture.Editor(graph)["palette"]["open"] == false);
	CHECK(fixture.GetDocument(graph) == document);
}

TEST_CASE("GraphCommands: graph.find finds text in every script or one, and the references of variables, functions and events")
{
	GraphFixture fixture;
	VisualScriptDocument const document = MakeWindowDocument();
	std::string const window = fixture.CreateGraph("Scripts/Window.sgraph", document);
	std::string const another = fixture.CreateGraph("Another.sgraph", document);
	using Failure = std::pair<AutomationErrorCode, std::string>;
	auto const failure = [&fixture](Json params)
	{
		CommandError const error = fixture.Fails("graph.find", std::move(params));
		return Failure{error.Code, error.Data.is_object() ? error.Data.value("path", std::string()) : std::string()};
	};

	// Text: every script by path, in document order (variables, nodes, comments, then each function).
	Json found = fixture.Ok("graph.find", Json::object({{"query", "s"}}));
	CAPTURE(found.dump());
	CHECK(found["total"] == found["results"].size());
	CHECK(found["results"][0]["asset"] == another);
	CHECK(found["results"].back()["asset"] == window);
	CHECK(found["results"].back()["function"] == 30);
	Json const one = fixture.Ok("graph.find", Json::object({{"query", "s"}, {"graph", window}}));
	CHECK(ListFound(one) == std::vector<std::string>{"1:VariableName", "2:VariableName", "11:Type", "12:Title", "13:Type", "14:Title",
	                                                 "14:Type", "15:Title", "20:CommentText", "30:FunctionName", "31:VariableName",
	                                                 "33:Title", "33:Type", "34:CommentText"});
	CHECK(found["total"] == 2 * one["total"].get<size_t>());
	// Paged.
	Json const page = fixture.Ok("graph.find", Json::object({{"query", "s"}, {"graph", window}, {"offset", 1}, {"limit", 2}}));
	CHECK(page["total"] == one["total"]);
	REQUIRE(page["results"].size() == 2);
	CHECK(page["results"][0] == one["results"][1]);
	CHECK(page["results"][1] == one["results"][2]);
	CHECK(fixture.Ok("graph.find", Json::object({{"query", "hello"}, {"graph", window}}))["results"][0]["field"] == "Value");
	CHECK(fixture.Ok("graph.find", Json::object({{"query", "nothing like it"}}))["total"] == 0);

	// References: a variable's Get and Set nodes, a function's calls, in their own script; a custom event's nodes everywhere.
	auto const references = [&fixture](Json reference)
	{
		return ListFound(fixture.Ok("graph.find", Json::object({{"references", std::move(reference)}})));
	};
	CHECK(references(Json::object({{"kind", "Variable"}, {"graph", window}, {"id", 1}})) == std::vector<std::string>{"12:Title"});
	CHECK(references(Json::object({{"kind", "Variable"}, {"graph", window}, {"id", 2}})) == std::vector<std::string>{"33:Title"});
	CHECK(references(Json::object({{"kind", "Variable"}, {"graph", window}, {"id", 31}})).empty());
	CHECK(references(Json::object({{"kind", "Function"}, {"graph", window}, {"id", 30}})) == std::vector<std::string>{"15:Title"});
	Json const events = fixture.Ok("graph.find", Json::object({{"references", {{"kind", "CustomEvent"}, {"name", "Hit"}}}}));
	CHECK(ListFound(events) == std::vector<std::string>{"13:Title", "14:Title", "13:Title", "14:Title"});
	CHECK(events["results"][0]["asset"] == another);
	CHECK(events["results"][2]["asset"] == window);
	CHECK(ListFound(fixture.Ok("graph.find", Json::object({{"references", {{"kind", "CustomEvent"}, {"name", "Hit"}}},
	                                                       {"graph", window}}))) == std::vector<std::string>{"13:Title", "14:Title"});
	// graph restricts a variable's references to its own script, which another is not.
	CHECK(fixture.Ok("graph.find", Json::object({{"references", {{"kind", "Variable"}, {"graph", window}, {"id", 1}}},
	                                             {"graph", another}}))["total"] == 0);

	// Errors.
	CHECK(failure(Json::object()) == Failure{AutomationErrorCode::InvalidParams, "query"});
	CHECK(failure(Json::object({{"query", "s"}, {"references", {{"kind", "CustomEvent"}, {"name", "Hit"}}}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "references"});
	CHECK(failure(Json::object({{"query", "s"}, {"graph", "asset://Missing.sgraph"}})) ==
	      Failure{AutomationErrorCode::AssetNotFound, "graph"});
	std::string const material =
		fixture.Ok("material.create", Json::object({{"path", "Materials/Red.smat"}}))["material"]["id"].get<std::string>();
	CHECK(failure(Json::object({{"query", "s"}, {"graph", material}})) == Failure{AutomationErrorCode::InvalidParams, "graph"});
	CHECK(failure(Json::object({{"references", {{"kind", "Variable"}, {"graph", "asset://Missing.sgraph"}, {"id", 1}}}})) ==
	      Failure{AutomationErrorCode::AssetNotFound, "references.graph"});
	CHECK(failure(Json::object({{"references", {{"kind", "Variable"}, {"graph", material}, {"id", 1}}}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "references.graph"});
	CHECK(failure(Json::object({{"references", {{"kind", "Variable"}, {"graph", window}, {"id", 30}}}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "references.id"});
	CHECK(failure(Json::object({{"references", {{"kind", "Function"}, {"graph", window}, {"id", 1}}}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "references.id"});
	CHECK(failure(Json::object({{"references", {{"kind", "Function"}, {"id", 30}}}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "references.graph"});
	CHECK(failure(Json::object({{"references", {{"kind", "Function"}, {"graph", window}}}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "references.id"});
	CHECK(failure(Json::object({{"references", {{"kind", "CustomEvent"}}}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "references.name"});

	// A script that does not load is skipped among every script, and a FileError when named.
	std::string const damaged = fixture.Ok("graph.create", Json::object({{"path", "Scripts/Damaged.sgraph"}}))["asset"].get<std::string>();
	fixture.WriteFile("Scripts/Damaged.sgraph", "{ \"Strada\": 1 }");
	found = fixture.Ok("graph.find", Json::object({{"query", "s"}}));
	CHECK(found["total"] == 2 * one["total"].get<size_t>());
	REQUIRE(found["skipped"].size() == 1);
	CHECK(found["skipped"][0]["asset"] == damaged);
	CHECK(found["skipped"][0]["path"] == "Scripts/Damaged.sgraph");
	CHECK(fixture.Fails("graph.find", Json::object({{"query", "s"}, {"graph", damaged}})).Code == AutomationErrorCode::FileError);
}
