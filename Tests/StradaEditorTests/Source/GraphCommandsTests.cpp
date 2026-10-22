#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/PlayMode.h"
#include "Editor/VisualScriptEnumLookupSource.h"

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
#include <optional>
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
	for (std::string const name : {"graph.create", "graph.get", "graph.set", "graph.check", "graph.library", "graph.types"})
	{
		CAPTURE(name);
		CommandDefinition const* command = fixture.Registry.Find(name);
		REQUIRE(command != nullptr);
		CHECK(command->ReadOnly == (name != "graph.create" && name != "graph.set"));
		CHECK_FALSE(command->IsAsync());
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
