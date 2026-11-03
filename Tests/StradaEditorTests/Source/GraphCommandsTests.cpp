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
#include "Editor/VisualScripting/VisualScriptArrange.h"
#include "Editor/VisualScripting/VisualScriptClipboard.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptEditors.h"
#include "Editor/VisualScripting/VisualScriptHeadlessCanvas.h"
#include "Editor/VisualScripting/VisualScriptPalette.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Hash.h"
#include "Strada/Core/Log.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
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

	// graph.*'s revision of a file's text, and program revision of a document.
	std::string RevisionOf(std::string_view text)
	{
		return fmt::format("{:016x}", Hash::FNV1a(text));
	}

	std::string ProgramRevisionOf(VisualScriptDocument const& document)
	{
		return RevisionOf(VisualScriptSerializer::WriteContentText(VisualScriptEditing::StripVisualScriptLayout(document)));
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
	for (std::string const name :
	     {"graph.create", "graph.get", "graph.set", "graph.check", "graph.edit", "graph.arrange", "graph.copy", "graph.interface",
	      "graph.library", "graph.types", "graph.open", "graph.close", "graph.editors", "graph.palette", "graph.find"})
	{
		CAPTURE(name);
		CommandDefinition const* command = fixture.Registry.Find(name);
		REQUIRE(command != nullptr);
		bool const changes = name == "graph.create" || name == "graph.set" || name == "graph.edit" || name == "graph.arrange" ||
		                     name == "graph.open" || name == "graph.close" || name == "graph.palette";
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
		VisualScriptDocument const events = MakeVisualScriptTemplate(VisualScriptTemplate::Events);
		CHECK(read == Json::object({{"asset", created["asset"]},
		                            {"path", "Scripts/Mover.sgraph"},
		                            {"document", VisualScriptSerializer::SerializeDocument(events)},
		                            {"revision", RevisionOf(fixture.ReadFile("Scripts/Mover.sgraph"))},
		                            {"programRevision", ProgramRevisionOf(events)},
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
	std::string const written = DumpJson(VisualScriptSerializer::Serialize(counter));
	CHECK(set == Json::object({{"asset", graph},
	                           {"path", "Scripts/Counter.sgraph"},
	                           {"changed", true},
	                           {"revision", RevisionOf(written)},
	                           {"programRevision", ProgramRevisionOf(counter)},
	                           {"analyzed", false},
	                           {"warnings", Json::array()},
	                           {"summary", Json::object({{"brokenReferrers", 0}, {"staleSenders", 0}})},
	                           {"referrers", Json::array()}}));
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
	std::string const damaged = "{ \"Strada\": { \"Version\": 4, \"Type\": \"VisualScript\" }, \"VisualScript\": {} }\n";
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
	CHECK(written.find("\"Version\": 3,") != std::string::npos);
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

TEST_CASE("GraphCommands: graph.set keeps a library's kind and its parameters' former names, which graph.get returns")
{
	// Without the scripting runtime nothing is analyzed: this is the format's round trip of the keys of version 3.
	GraphFixture fixture;
	VisualScriptDocument library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	VisualScriptFunction clamp;
	clamp.ID = 1;
	clamp.Name = "Clamp01";
	clamp.Pure = true;
	clamp.Inputs = {{"Value", "float", {"X"}}};
	clamp.Outputs = {{"Result", "float"}};
	VisualScriptNode entry;
	entry.ID = 2;
	entry.Type = "Function.Entry";
	entry.Position = glm::vec2(0.0f, 0.0f);
	VisualScriptNode result;
	result.ID = 3;
	result.Type = "Function.Return";
	result.Position = glm::vec2(320.0f, 0.0f);
	clamp.Graph.Nodes = {entry, result};
	clamp.Graph.Links = {VisualScriptLink{{2, "Value"}, {3, "Result"}}};
	library.Functions = {clamp};
	Json const content = VisualScriptSerializer::SerializeDocument(library);
	REQUIRE(content.begin().key() == "Kind");

	std::string const graph = fixture.CreateGraph("Scripts/Motion.sgraph", library);
	CHECK(fixture.GetDocument(graph) == library);
	Json const document = fixture.Ok("graph.get", Json::object({{"graph", graph}}))["document"];
	CHECK(document == content);
	CHECK(document["Kind"] == "Library");
	CHECK(document["Functions"][0]["Inputs"][0]["FormerNames"] == Json::array({"X"}));
	std::string const written = fixture.ReadFile("Scripts/Motion.sgraph");
	CHECK(written == DumpJson(VisualScriptSerializer::Serialize(library)));
	CHECK(written.find("\"Version\": 3,") != std::string::npos);

	// The same document again changes nothing; a repair of a key of version 3 is refused as any other.
	CHECK(fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", content}}))["changed"] == false);
	Json unknownKind = content;
	unknownKind["Kind"] = "Module";
	Json ownName = content;
	ownName["Functions"][0]["Inputs"][0]["FormerNames"] = Json::array({"Value"});
	for (auto const& [damaged, path] : {std::pair<Json, std::string_view>(unknownKind, "document.Kind"),
	                                    std::pair<Json, std::string_view>(ownName, "document.Functions[0].Inputs[0].FormerNames[0]")})
	{
		CAPTURE(path);
		CommandError const refused = fixture.Fails("graph.set", Json::object({{"graph", graph}, {"document", damaged}}));
		CHECK(refused.Code == AutomationErrorCode::InvalidParams);
		CHECK(refused.Data["path"] == std::string(path));
	}
	CHECK(fixture.GetDocument(graph) == library);
	CHECK(fixture.ReadFile("Scripts/Motion.sgraph") == written);
}

TEST_CASE("GraphCommands: graph.set and graph.check record a library's renamed parameter on the parameter that took its place")
{
	GraphFixture fixture;
	VisualScriptDocument library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	VisualScriptFunction mix;
	mix.ID = 1;
	mix.Name = "Mix";
	mix.Pure = true;
	mix.Inputs = {{"A", "float"}, {"B", "float"}};
	mix.Outputs = {{"Result", "float"}};
	VisualScriptNode entry;
	entry.ID = 2;
	entry.Type = "Function.Entry";
	entry.Position = glm::vec2(0.0f, 0.0f);
	VisualScriptNode result;
	result.ID = 3;
	result.Type = "Function.Return";
	result.Position = glm::vec2(320.0f, 0.0f);
	mix.Graph.Nodes = {entry, result};
	mix.Graph.Links = {VisualScriptLink{{2, "A"}, {3, "Result"}}};
	library.Functions = {mix};
	std::string const graph = fixture.CreateGraph("Scripts/Mixing.sgraph", library);

	// A moved to the end and B renamed C: B is C's former name, not A's.
	VisualScriptDocument edited = library;
	edited.Functions[0].Inputs = {{"C", "float"}, {"A", "float"}};
	Json const content = VisualScriptSerializer::SerializeDocument(edited);
	Json const expectedWarnings = Json::array({"parameter renamed: Mix's B → C (B is kept in FormerNames)"});
	Json const checked = fixture.Ok("graph.check", Json::object({{"graph", graph}, {"document", content}}));
	CHECK(checked["valid"] == true);
	CHECK(checked["warnings"] == expectedWarnings);
	Json const set = fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", content}}));
	CHECK(set["warnings"] == expectedWarnings);
	CHECK(fixture.GetDocument(graph).Functions[0].Inputs == std::vector<VisualScriptParameter>{{"C", "float", {"B"}}, {"A", "float"}});
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

		// With docking, windows can be docked together, so one hides another behind its tab.
		explicit GraphWindowFixture(bool docking = false)
		{
			Gui.UseEditorStyle(docking);
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

		std::function<void()> Drawer()
		{
			return [this]
			{
				Editors.OnImGuiRender();
			};
		}

		void Frames(int count)
		{
			for (int i = 0; i < count; i++)
			{
				Gui.Frame(Drawer());
			}
		}

		static AssetHandle GetHandle(std::string const& graph)
		{
			Result<AssetHandle> const handle = AssetManager::ResolveReference(graph);
			REQUIRE(handle.IsOk());
			return handle.GetValue();
		}

		// A point of the graph window's canvas with nothing on it (its lower left corner, below the window document's nodes).
		static ImVec2 GetEmptyCanvasPoint(std::string const& graph)
		{
			ImGuiWindow const* window = ImGui::FindWindowByName(VisualScriptEditorPanel::GetWindowName(GetHandle(graph), {}).c_str());
			REQUIRE(window != nullptr);
			for (ImGuiWindow const* child : GImGui->Windows)
			{
				if (child->ParentWindow == window && std::strstr(child->Name, "##canvas") != nullptr)
				{
					return ImVec2(child->Pos.x + 30.0f, child->Pos.y + child->Size.y - 30.0f);
				}
			}
			FAIL("the graph window has no canvas");
			return ImVec2();
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
	CHECK(editor["panes"] == Json::object({{"sidebar", "Shown"}, {"details", "Shown"}, {"stacked", false}}));
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

TEST_CASE("GraphCommands: graph.open takes the farthest and the nearest zoom as clients write them (0.1 and 2)")
{
	using Failure = std::pair<AutomationErrorCode, std::string>;
	GraphWindowFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Window.sgraph", MakeWindowDocument());
	for (double const zoom : {0.1, 2.0, 0.1})
	{
		CAPTURE(zoom);
		fixture.Ok("graph.open", Json::object({{"graph", graph}, {"zoom", zoom}}));
		// Past the view's animation.
		fixture.Frames(30);
		CHECK(fixture.Editor(graph)["view"]["zoom"].get<float>() == static_cast<float>(zoom));
	}
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", graph}, {"zoom", 0.0999}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "zoom"});
	CHECK(fixture.Failure("graph.open", Json::object({{"graph", graph}, {"zoom", 2.001}})) ==
	      Failure{AutomationErrorCode::InvalidParams, "zoom"});
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

TEST_CASE("GraphCommands: graph.palette answers for a window graph.open opened in the same frame, and refuses windows that show no "
          "palette")
{
	GraphWindowFixture fixture(true);
	std::string const graph = fixture.CreateGraph("Scripts/Window.sgraph", MakeWindowDocument());
	std::string const other = fixture.CreateGraph("Scripts/Other.sgraph", MakeWindowDocument());

	// graph.open, then graph.palette before any draw: the answer comes once the new window and then its palette were drawn.
	std::optional<CommandResult> opened;
	fixture.Registry.Execute("graph.open", Json::object({{"graph", graph}}),
	                         [&opened](CommandResult result)
	                         {
								 opened.emplace(std::move(result));
							 });
	REQUIRE(opened.has_value());
	REQUIRE(opened->IsOk());
	Json const listed = fixture.Ok("graph.palette", Json::object({{"graph", graph}, {"query", "br"}}));
	CHECK(listed["open"] == true);
	CHECK(listed["query"] == "br");
	CHECK_FALSE(listed["entries"].empty());

	// Closed between the request and the draw that ranks the entries.
	fixture.Frames(1);
	std::optional<CommandResult> palette;
	fixture.Registry.Execute("graph.palette", Json::object({{"graph", graph}, {"query", "lo"}}),
	                         [&palette](CommandResult result)
	                         {
								 palette.emplace(std::move(result));
							 });
	REQUIRE_FALSE(palette.has_value());
	fixture.Ok("graph.close", Json::object({{"graph", graph}}));
	fixture.Frames(1);
	REQUIRE(palette.has_value());
	REQUIRE(palette->IsError());
	CHECK(palette->GetError().Code == AutomationErrorCode::InvalidOperation);

	// Behind another window's tab: still not shown after a draw.
	fixture.Ok("graph.open", Json::object({{"graph", graph}}));
	fixture.Ok("graph.open", Json::object({{"graph", other}}));
	fixture.Frames(2);
	fixture.Gui.Frame(
		[&fixture, &graph, &other]
		{
			ImGuiID const node = ImGui::DockBuilderAddNode(0, ImGuiDockNodeFlags_None);
			ImGui::DockBuilderSetNodePos(node, ImVec2(100.0f, 100.0f));
			ImGui::DockBuilderSetNodeSize(node, ImVec2(1400.0f, 800.0f));
			for (std::string const& docked : {graph, other})
			{
				ImGui::DockBuilderDockWindow(VisualScriptEditorPanel::GetWindowName(GraphWindowFixture::GetHandle(docked), {}).c_str(),
			                                 node);
			}
			ImGui::DockBuilderFinish(node);
			fixture.Editors.OnImGuiRender();
		});
	fixture.Ok("graph.open", Json::object({{"graph", other}}));
	fixture.Frames(2);
	REQUIRE(fixture.Editor(graph)["visible"] == false);
	REQUIRE(fixture.Editor(other)["visible"] == true);
	CommandError const hidden = fixture.Fails("graph.palette", Json::object({{"graph", graph}}));
	CHECK(hidden.Code == AutomationErrorCode::InvalidOperation);
	CHECK(hidden.Message.find("is not shown") != std::string::npos);
	CHECK(fixture.Editor(graph)["palette"]["open"] == false);

	// A file that does not load: its window shows its card, without a canvas.
	REQUIRE(FileSystem::WriteTextFile(AssetManager::GetAssetDirectory() / "Scripts" / "Other.sgraph", "{ \"Strada\": 1 }").IsOk());
	fixture.Ok("asset.refresh");
	fixture.Frames(2);
	REQUIRE(fixture.Editor(other)["card"] == "Unloadable");
	CommandError const unloadable = fixture.Fails("graph.palette", Json::object({{"graph", other}}));
	CHECK(unloadable.Code == AutomationErrorCode::InvalidOperation);
	CHECK(unloadable.Message.find("does not load") != std::string::npos);
}

TEST_CASE("GraphCommands: graph.editors reports what a window shows beyond its defaults")
{
	GraphWindowFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Window.sgraph", MakeWindowDocument());
	AssetHandle const handle = GraphWindowFixture::GetHandle(graph);
	fixture.Ok("graph.open", Json::object({{"graph", graph}, {"frame", "All"}}));
	fixture.Frames(3);
	Json editor = fixture.Editor(graph);
	CHECK(editor["minimap"] == false);
	CHECK(editor["notices"] == Json::array());

	// The minimap shows while part of the graph is out of view.
	fixture.Ok("graph.open", Json::object({{"graph", graph}, {"zoom", 2.0}, {"center", {0.0, 0.0}}}));
	fixture.Frames(3);
	CHECK(fixture.Editor(graph)["minimap"] == true);

	// Without the scripting runtime: the degraded notice, nothing analyzed.
	fixture.Semantics.SetAvailable(false);
	fixture.Frames(2);
	editor = fixture.Editor(graph);
	CHECK(editor["notices"] == Json::array({"Degraded"}));
	CHECK(editor["analyzed"] == false);
	fixture.Semantics.SetAvailable(true);
	fixture.Frames(2);
	CHECK(fixture.Editor(graph)["notices"] == Json::array());

	// Playing and simulating.
	fixture.Context.BeginPlay(Scene::Copy(fixture.Context.GetScene()), EditorPlayState::Play);
	fixture.Frames(2);
	CHECK(fixture.Editor(graph)["live"] == "Live");
	fixture.Context.EndPlay();
	fixture.Context.BeginPlay(Scene::Copy(fixture.Context.GetScene()), EditorPlayState::Simulate);
	fixture.Frames(2);
	CHECK(fixture.Editor(graph)["live"] == "Simulating");
	fixture.Context.EndPlay();
	fixture.Frames(2);
	CHECK(fixture.Editor(graph)["live"] == "None");

	// The shortcut sheet, and a bookmark stored from the canvas.
	fixture.Gui.MouseClick(GraphWindowFixture::GetEmptyCanvasPoint(graph), ImGuiMouseButton_Left, fixture.Drawer());
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_3, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Editor(graph)["bookmarks"] == Json::array({3}));
	fixture.Gui.Press(ImGuiKey_F1, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Editor(graph)["shortcuts"] == true);
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Editor(graph)["shortcuts"] == false);

	// Panes: Details hidden by the user; both auto-hidden in a window too narrow for them beside the canvas.
	VisualScriptPaneSettings panes = fixture.Settings.GetPanes();
	panes.ShowDetails = false;
	fixture.Settings.SetPanes(panes);
	fixture.Frames(2);
	CHECK(fixture.Editor(graph)["panes"] == Json::object({{"sidebar", "Shown"}, {"details", "Hidden"}, {"stacked", false}}));
	panes.ShowDetails = true;
	fixture.Settings.SetPanes(panes);
	std::string const name = VisualScriptEditorPanel::GetWindowName(handle, {});
	auto const resize = [&fixture, &name](float width)
	{
		fixture.Gui.Frame(
			[&fixture, &name, width]
			{
				ImGui::SetWindowSize(name.c_str(), ImVec2(width, 600.0f));
				fixture.Editors.OnImGuiRender();
			});
		fixture.Frames(2);
	};
	// Too narrow for both beside the canvas, wide enough for them stacked in one column.
	resize(1086.0f);
	CHECK(fixture.Editor(graph)["panes"] == Json::object({{"sidebar", "Shown"}, {"details", "Shown"}, {"stacked", true}}));
	resize(400.0f);
	CHECK(fixture.Editor(graph)["panes"] == Json::object({{"sidebar", "AutoHidden"}, {"details", "AutoHidden"}, {"stacked", false}}));

	// A commit whose file cannot be written: not saved, and a toast says why.
	fixture.Ok("graph.open", Json::object({{"graph", graph}, {"select", Json::array({12})}}));
	fixture.Frames(2);
	std::filesystem::path const file = AssetManager::GetAssetDirectory() / "Scripts" / "Window.sgraph";
	{
		Testing::ReadOnlyFileScope const readOnly(file);
		VisualScriptEditorPanel* panel = fixture.Editors.FindPanel(handle);
		REQUIRE(panel != nullptr);
		REQUIRE(panel->RequestAction(VisualScriptEditorAction::Delete));
		fixture.Frames(2);
		editor = fixture.Editor(graph);
		CHECK(editor["save"] == "NotSaved");
		REQUIRE(editor["toasts"].size() >= 1);
		CHECK(editor["toasts"].back().get<std::string>().starts_with("Could not save Window: "));
		// The window now knows the file is read-only.
		CHECK(editor["readOnly"] == true);
		CHECK(Contains(editor["notices"].get<std::vector<std::string>>(), "ReadOnlyFile"));
	}

	// A file that is gone shows its card.
	REQUIRE(FileSystem::Remove(file).IsOk());
	fixture.Ok("asset.refresh");
	fixture.Frames(2);
	editor = fixture.Editor(graph);
	CHECK(editor["card"] == "Missing");
	CHECK(editor["loaded"] == false);
}

TEST_CASE("GraphCommands: graph.editors reports the last working version while the game runs it and the graph has errors")
{
	Testing::ScriptRuntimeScope runtime;
	GraphWindowFixture fixture;
	EditorScripts scripts{fixture.Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
	PlayMode play{fixture.Context, scripts};
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	AssetHandle const handle = GraphWindowFixture::GetHandle(graph);
	fixture.Ok("entity.create",
	           Json::object({{"name", "Counter"}, {"components", Json::object({{"VisualScript", Json::object({{"Graph", graph}})}})}}));
	fixture.Ok("graph.open", Json::object({{"graph", graph}}));
	fixture.Frames(2);

	REQUIRE(play.Start(EditorPlayState::Play, 640, 480).IsOk());
	play.Advance(2, Timestep(FrameTime));
	fixture.Frames(2);
	CHECK(fixture.Editor(graph)["live"] == "Live");

	// An edit that breaks the graph: the game keeps running the last version that compiled, and the window says so.
	VisualScriptDocument broken = MakeCounterDocument();
	broken.Graph.Nodes.push_back(MakeNode(20, "Call:Strada.Nothing.Here()"));
	broken.Graph.Links.push_back(MakeLink(11, "Then", 20, "Execute"));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(handle, broken).IsOk());
	play.Advance(1, Timestep(FrameTime));
	fixture.Frames(2);
	REQUIRE(VisualScriptEngine::HasWorkingProgram(handle));
	Json const editor = fixture.Editor(graph);
	CHECK(editor["errors"].get<uint32_t>() > 0);
	CHECK(editor["live"] == "LastWorking");
	play.Stop();
	fixture.Frames(2);
	CHECK(fixture.Editor(graph)["live"] == "None");
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

namespace
{
	// The counter's nodes all at one place in a comment box, and a function with two nodes of its own, also stacked.
	VisualScriptDocument MakeStackedDocument()
	{
		VisualScriptDocument document = MakeCounterDocument();
		for (VisualScriptNode& node : document.Graph.Nodes)
		{
			node.Position = glm::vec2(32.0f, 48.0f);
		}
		VisualScriptFunction function;
		function.ID = 20;
		function.Name = "Reset";
		function.Graph.Nodes.push_back(MakeNode(21, "Function.Entry"));
		function.Graph.Nodes.push_back(MakeNode(22, "Variable.Set", Json::object({{"Variable", 2}}), Json::object({{"Value", 0}})));
		function.Graph.Nodes[0].Position = glm::vec2(0.0f, 0.0f);
		function.Graph.Nodes[1].Position = glm::vec2(0.0f, 0.0f);
		function.Graph.Links.push_back(MakeLink(21, "Then", 22, "Execute"));
		document.Functions.push_back(std::move(function));
		VisualScriptComment comment;
		comment.ID = 30;
		comment.Text = "Counting";
		comment.Position = glm::vec2(0.0f, 0.0f);
		comment.Size = glm::vec2(400.0f, 200.0f);
		document.Graph.Comments.push_back(std::move(comment));
		return document;
	}

	std::optional<glm::vec2> FindPosition(VisualScriptGraph const& graph, uint32_t id)
	{
		for (VisualScriptNode const& node : graph.Nodes)
		{
			if (node.ID == id)
			{
				return node.Position;
			}
		}
		return std::nullopt;
	}
}

TEST_CASE("GraphCommands: graph.arrange lays out a sub-graph as the Arrange button does, as one undo step")
{
	// Without a project there is nothing to write; copying needs none.
	{
		GraphFixture closed(false);
		CHECK(closed.Fails("graph.arrange", Json::object({{"graph", "123"}})).Code == AutomationErrorCode::InvalidOperation);
		CHECK(closed.Fails("graph.copy", Json::object({{"graph", "123"}, {"elements", Json::array({1})}})).Code ==
		      AutomationErrorCode::AssetNotFound);
	}

	GraphFixture fixture;
	VisualScriptDocument const stacked = MakeStackedDocument();
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", stacked);
	std::string const original = fixture.ReadFile("Scripts/Counter.sgraph");
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();

	// The window's Arrange: ArrangeNodes with node sizes as a window draws them, here without the scripting runtime (the
	// pins the links and literals name).
	VisualScriptDocument expected = stacked;
	VisualScriptHeadlessCanvas canvas;
	canvas.Sync(stacked, 0, nullptr, nullptr, nullptr);
	REQUIRE(VisualScriptEditing::ArrangeNodes(expected, 0, {}, canvas.GetExtents()).IsOk());

	Json const arranged = fixture.Ok("graph.arrange", Json::object({{"graph", graph}}));
	CHECK(arranged["asset"] == graph);
	CHECK(arranged["path"] == "Scripts/Counter.sgraph");
	CHECK(arranged["changed"] == true);
	CHECK(arranged["arranged"] == 5);
	std::string const written = fixture.ReadFile("Scripts/Counter.sgraph");
	CHECK(written == VisualScriptSerializer::WriteText(expected));
	CHECK(fixture.GetDocument(graph) == expected);
	CHECK(arranged["revision"] == RevisionOf(written));
	// Arranging moves nodes only: the program revision is the stored document's.
	CHECK(arranged["programRevision"] == ProgramRevisionOf(stacked));
	// The bounds of the arranged nodes, as a window draws them.
	glm::vec2 low(std::numeric_limits<float>::max());
	glm::vec2 high(std::numeric_limits<float>::lowest());
	for (VisualScriptNode const& node : expected.Graph.Nodes)
	{
		REQUIRE(node.Position.has_value());
		low = glm::min(low, *node.Position);
		high = glm::max(high, *node.Position + canvas.GetExtents()(node.ID));
	}
	CHECK(arranged["bounds"] == Json::object({{"min", Json::array({low.x, low.y})}, {"max", Json::array({high.x, high.y})}}));
	// The comment box whose contents were all arranged is fitted around them again.
	CHECK(fixture.GetDocument(graph).Graph.Comments[0].Size != stacked.Graph.Comments[0].Size);
	// The function's graph is left as it was.
	CHECK(fixture.GetDocument(graph).Functions == stacked.Functions);

	// One step, described as the button describes it; the scene stays unmodified.
	Json const status = fixture.Ok("editor.status");
	CHECK(status["undo"] == "Arrange 5 Nodes in Counter");
	CHECK(status["scene"]["dirty"] == false);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps + 1);

	// Arranging is idempotent: the second call changes nothing and records no step.
	Json const again = fixture.Ok("graph.arrange", Json::object({{"graph", graph}}));
	CHECK(again["changed"] == false);
	CHECK(again["revision"] == arranged["revision"]);
	CHECK(again["bounds"] == arranged["bounds"]);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps + 1);
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == written);

	// Undo gives the file its bytes back, redo writes the arranged ones again.
	CHECK(fixture.Ok("editor.undo")["undone"] == "Arrange 5 Nodes in Counter");
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == original);
	CHECK(fixture.GetDocument(graph) == stacked);
	CHECK(fixture.Ok("editor.redo")["redone"] == "Arrange 5 Nodes in Counter");
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == written);
	fixture.Ok("editor.undo");

	// A function's graph, named by its ID or its name; a single node.
	Json const function = fixture.Ok("graph.arrange", Json::object({{"graph", graph}, {"function", "Reset"}}));
	CHECK(function["arranged"] == 2);
	CHECK(fixture.Ok("editor.status")["undo"] == "Arrange 2 Nodes in Counter");
	CHECK(fixture.GetDocument(graph).Graph == stacked.Graph);
	CHECK(FindPosition(fixture.GetDocument(graph).Functions[0].Graph, 22) != FindPosition(stacked.Functions[0].Graph, 22));
	fixture.Ok("editor.undo");
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == original);
	// One placed node keeps its place: nothing changes.
	Json const one = fixture.Ok("graph.arrange", Json::object({{"graph", graph}, {"function", 20}, {"nodes", Json::array({22})}}));
	CHECK(one["arranged"] == 1);
	CHECK(one["changed"] == false);
	CHECK(one["revision"] == RevisionOf(original));
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
}

TEST_CASE("GraphCommands: graph.arrange arranges the nodes given with either extents, and refuses what it cannot arrange")
{
	GraphFixture fixture;
	VisualScriptDocument const stacked = MakeStackedDocument();
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", stacked);
	std::string const original = fixture.ReadFile("Scripts/Counter.sgraph");
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	std::string const revision = RevisionOf(original);

	// Some nodes: the others stay; Uniform sizes every node 256 x 128, as PlaceUnplacedNodes pitches them.
	std::vector<uint32_t> const some = {12, 13, 14};
	VisualScriptDocument uniform = stacked;
	REQUIRE(VisualScriptEditing::ArrangeNodes(uniform, 0, some,
	                                          [](uint32_t node)
	                                          {
												  return VisualScriptEditing::GetDefaultVisualScriptNodeExtents(node);
											  })
	            .IsOk());
	Json const partial =
		fixture.Ok("graph.arrange", Json::object({{"graph", graph}, {"nodes", Json::array({12, 13, 14, 13})}, {"extents", "Uniform"}}));
	CHECK(partial["arranged"] == 3);
	CHECK(fixture.GetDocument(graph) == uniform);
	CHECK(FindPosition(fixture.GetDocument(graph).Graph, 10) == FindPosition(stacked.Graph, 10));
	CHECK(fixture.Ok("editor.status")["undo"] == "Arrange 3 Nodes in Counter");
	fixture.Ok("editor.undo");

	// A dry run answers what the call then does, and writes nothing.
	Json const dry = fixture.Ok("graph.arrange", Json::object({{"graph", graph}, {"dryRun", true}, {"revision", revision}}));
	CHECK(dry["changed"] == true);
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == original);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
	CHECK(fixture.Context.GetHistory().CanRedo());
	Json const real = fixture.Ok("graph.arrange", Json::object({{"graph", graph}, {"revision", revision}}));
	CHECK(real == dry);
	CHECK(real["revision"] == RevisionOf(fixture.ReadFile("Scripts/Counter.sgraph")));

	// Revisions: the stored document's must be given, as 16 lowercase hex digits.
	size_t const before = fixture.Context.GetHistory().GetUndoCount();
	std::string const written = fixture.ReadFile("Scripts/Counter.sgraph");
	CommandError const stale = fixture.Fails("graph.arrange", Json::object({{"graph", graph}, {"revision", revision}}));
	CHECK(stale.Code == AutomationErrorCode::InvalidOperation);
	CHECK(stale.Data == Json::object({{"path", "revision"}, {"revision", real["revision"]}}));
	for (std::string const malformed : {"ABCDEF0123456789", "0123456789abcdeg"})
	{
		CommandError const refused = fixture.Fails("graph.arrange", Json::object({{"graph", graph}, {"revision", malformed}}));
		CHECK(refused.Code == AutomationErrorCode::InvalidParams);
		CHECK(refused.Data["path"] == "revision");
	}
	CHECK(fixture.Fails("graph.arrange", Json::object({{"graph", graph}, {"revision", "0123"}})).Code ==
	      AutomationErrorCode::InvalidParams);

	// What names nothing of the sub-graph.
	auto const failure = [&fixture, &graph](Json params)
	{
		params["graph"] = graph;
		CommandError const error = fixture.Fails("graph.arrange", std::move(params));
		return std::pair(error.Code, error.Data.value("path", std::string()));
	};
	CHECK(failure(Json::object({{"function", 99}})) == std::pair(AutomationErrorCode::InvalidParams, std::string("function")));
	CHECK(failure(Json::object({{"function", "Nope"}})) == std::pair(AutomationErrorCode::InvalidParams, std::string("function")));
	CHECK(failure(Json::object({{"function", ""}})) == std::pair(AutomationErrorCode::InvalidParams, std::string("function")));
	CHECK(failure(Json::object({{"function", -1}})) == std::pair(AutomationErrorCode::InvalidParams, std::string("function")));
	CHECK(failure(Json::object({{"function", true}})) == std::pair(AutomationErrorCode::InvalidParams, std::string("function")));
	CHECK(failure(Json::object({{"nodes", Json::array({10, 999})}})) ==
	      std::pair(AutomationErrorCode::InvalidParams, std::string("nodes[1]")));
	// A node of another sub-graph, and a comment box, are not nodes of the event graph.
	CHECK(failure(Json::object({{"nodes", Json::array({22})}})) == std::pair(AutomationErrorCode::InvalidParams, std::string("nodes[0]")));
	CHECK(failure(Json::object({{"nodes", Json::array({30})}})) == std::pair(AutomationErrorCode::InvalidParams, std::string("nodes[0]")));
	CHECK(failure(Json::object({{"extents", "Exact"}})) == std::pair(AutomationErrorCode::InvalidParams, std::string("extents")));
	CHECK(fixture.Fails("graph.arrange", Json::object({{"graph", graph}, {"nodes", Json::array({999})}})).Message ==
	      "nodes[0]: 999 is not a node of the event graph");
	CHECK(fixture.Context.GetHistory().GetUndoCount() == before);
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == written);

	// Graphs that are not registered, not visual scripts, without a file of their own, read-only or that do not load.
	CHECK(fixture.Fails("graph.arrange", Json::object({{"graph", "123"}})).Code == AutomationErrorCode::AssetNotFound);
	std::string const material =
		fixture.Ok("material.create", Json::object({{"path", "Materials/Painted.smat"}}))["material"]["id"].get<std::string>();
	CHECK(fixture.Fails("graph.arrange", Json::object({{"graph", material}})).Data["path"] == "graph");
	AssetHandle const memory = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(stacked), "Memory");
	CommandError const withoutFile = fixture.Fails("graph.arrange", Json::object({{"graph", memory.ToString()}}));
	CHECK(withoutFile.Code == AutomationErrorCode::InvalidParams);
	CHECK(withoutFile.Data["path"] == "graph");
	fixture.Ok("editor.undo");
	{
		Testing::ReadOnlyFileScope const readOnly(fixture.GetFile("Scripts/Counter.sgraph"));
		CommandError const refused = fixture.Fails("graph.arrange", Json::object({{"graph", graph}}));
		CHECK(refused.Code == AutomationErrorCode::FileError);
		CHECK(refused.Message == "the file of visual script 'Scripts/Counter.sgraph' is read-only");
	}
	fixture.WriteFile("Broken.sgraph", "{ \"Strada\": ");
	REQUIRE(fixture.Ok("asset.refresh")["added"].size() == 1);
	CommandError const broken = fixture.Fails("graph.arrange", Json::object({{"graph", "asset://Broken.sgraph"}}));
	CHECK(broken.Code == AutomationErrorCode::FileError);
	CHECK(broken.Message.starts_with("the file of visual script 'Broken.sgraph' does not load: "));
	CHECK(broken.Message.ends_with("; graph.set replaces it"));

	// Nodes at the edge of the canvas's range are arranged within it.
	VisualScriptDocument edge = stacked;
	for (VisualScriptNode& node : edge.Graph.Nodes)
	{
		node.Position = glm::vec2(static_cast<float>(VisualScriptSerializer::MaxCoordinate) - 100.0f, 0.0f);
	}
	std::string const edgeGraph = fixture.CreateGraph("Scripts/Far.sgraph", edge);
	CHECK(fixture.Ok("graph.arrange", Json::object({{"graph", edgeGraph}}))["changed"] == true);
	for (VisualScriptNode const& node : fixture.GetDocument(edgeGraph).Graph.Nodes)
	{
		REQUIRE(node.Position.has_value());
		CHECK(VisualScriptSerializer::IsValidPosition(*node.Position));
	}
}

TEST_CASE("GraphCommands: graph.arrange measures nodes from the runtime's analysis, and an empty sub-graph arranges nothing")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	VisualScriptDocument const stacked = MakeStackedDocument();
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", stacked);
	VisualScriptDocument const stored = fixture.GetDocument(graph);

	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);
	Result<VisualScriptAnalysis> const analysis = VisualScriptEngine::Analyze(stored);
	REQUIRE(analysis.IsOk());
	VisualScriptHeadlessCanvas canvas;
	canvas.Sync(stored, 0, &analysis.GetValue(), &stored, library.get());
	VisualScriptDocument expected = stored;
	REQUIRE(VisualScriptEditing::ArrangeNodes(expected, 0, {}, canvas.GetExtents()).IsOk());
	Json const arranged = fixture.Ok("graph.arrange", Json::object({{"graph", graph}}));
	CHECK(arranged["changed"] == true);
	CHECK(fixture.GetDocument(graph) == expected);

	VisualScriptDocument empty;
	std::string const blank = fixture.CreateGraph("Scripts/Blank.sgraph", empty);
	Json const nothing = fixture.Ok("graph.arrange", Json::object({{"graph", blank}}));
	CHECK(nothing["changed"] == false);
	CHECK(nothing["arranged"] == 0);
	CHECK(nothing["bounds"].is_null());
}

TEST_CASE("GraphCommands: graph.copy writes the clipboard text of elements as the window's Copy does")
{
	GraphFixture fixture;
	VisualScriptDocument const stacked = MakeStackedDocument();
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", stacked);
	AssetHandle const handle = AssetManager::FindByPath("Scripts/Counter.sgraph");
	VisualScriptDocument const document = fixture.GetDocument(graph);
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();

	std::vector<uint32_t> const elements = {11, 12, 13, 30};
	Json const copied = fixture.Ok("graph.copy", Json::object({{"graph", graph}, {"elements", Json::array({11, 12, 13, 30, 12})}}));
	CHECK(copied == Json::object({{"text", WriteVisualScriptClipboard(document, 0, elements, handle, nullptr)}}));
	CHECK(MayHoldVisualScriptClipboard(copied["text"].get<std::string>()));
	// It reads back as the window's Paste reads it.
	Result<VisualScriptClipboardContent> const read = ReadVisualScriptClipboard(copied["text"].get<std::string>());
	REQUIRE_MESSAGE(read.IsOk(), (read.IsError() ? read.GetError() : std::string()));

	// A function's elements, named by the function's ID or name.
	Json const function = fixture.Ok(
		"graph.copy", Json::object({{"graph", "asset://Scripts/Counter.sgraph"}, {"function", "Reset"}, {"elements", Json::array({22})}}));
	std::vector<uint32_t> const set = {22};
	CHECK(function["text"] == WriteVisualScriptClipboard(document, 20, set, handle, nullptr));
	CHECK(fixture.Ok("graph.copy", Json::object({{"graph", graph}, {"function", 20}, {"elements", Json::array({22})}})) == function);

	// Nothing is written: no step.
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);

	auto const failure = [&fixture, &graph](Json params)
	{
		params["graph"] = graph;
		CommandError const error = fixture.Fails("graph.copy", std::move(params));
		return std::pair(error.Code, error.Data.value("path", std::string()));
	};
	CHECK(failure(Json::object({{"elements", Json::array()}})) == std::pair(AutomationErrorCode::InvalidParams, std::string("elements")));
	CHECK(failure(Json::object()) == std::pair(AutomationErrorCode::InvalidParams, std::string("elements")));
	CHECK(failure(Json::object({{"elements", Json::array({11, 22})}})) ==
	      std::pair(AutomationErrorCode::InvalidParams, std::string("elements[1]")));
	CHECK(failure(Json::object({{"function", 20}, {"elements", Json::array({11})}})) ==
	      std::pair(AutomationErrorCode::InvalidParams, std::string("elements[0]")));
	CHECK(failure(Json::object({{"function", 7}, {"elements", Json::array({11})}})) ==
	      std::pair(AutomationErrorCode::InvalidParams, std::string("function")));
	CHECK(fixture.Fails("graph.copy", Json::object({{"graph", graph}, {"elements", Json::array({11, 22})}})).Message ==
	      "elements[1]: 22 is not a node or comment box of the event graph");
	CHECK(fixture.Fails("graph.copy", Json::object({{"graph", "123"}, {"elements", Json::array({1})}})).Code ==
	      AutomationErrorCode::AssetNotFound);
	fixture.WriteFile("Broken.sgraph", "{ \"Strada\": ");
	REQUIRE(fixture.Ok("asset.refresh")["added"].size() == 1);
	CommandError const broken =
		fixture.Fails("graph.copy", Json::object({{"graph", "asset://Broken.sgraph"}, {"elements", Json::array({1})}}));
	CHECK(broken.Code == AutomationErrorCode::FileError);
	CHECK(broken.Message.starts_with("the file of visual script 'Broken.sgraph' does not load: "));

	// A graph without a file of its own copies too.
	AssetHandle const memory = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(stacked), "Memory");
	CHECK(fixture.Ok("graph.copy", Json::object({{"graph", memory.ToString()}, {"elements", Json::array({10})}}))["text"] ==
	      WriteVisualScriptClipboard(stacked, 0, std::vector<uint32_t>{10}, memory, nullptr));
}

TEST_CASE("GraphCommands: graph.edit applies a batch as one undo step through the commit pipeline")
{
	{
		GraphFixture closed(false);
		CHECK(closed.Fails("graph.edit", Json::object({{"graph", "123"}, {"operations", Json::array()}})).Code ==
		      AutomationErrorCode::InvalidOperation);
	}

	GraphFixture fixture;
	VisualScriptDocument const stacked = MakeStackedDocument();
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", stacked);
	std::string const original = fixture.ReadFile("Scripts/Counter.sgraph");
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	Json const operations = Json::array({
		Json::object({{"op", "AddVariable"}, {"name", "Lives"}, {"type", "int"}, {"value", 3}, {"as", "$lives"}}),
		Json::object({{"op", "AddNode"},
	                  {"type", "Variable.Get"},
	                  {"settings", Json::object({{"Variable", "Lives"}})},
	                  {"position", Json::array({0, 400})},
	                  {"as", "$get"}}),
		Json::object({{"op", "SetNodeComment"}, {"nodes", Json::array({10})}, {"text", "Start"}}),
	});
	Json const params = Json::object({{"graph", graph}, {"operations", operations}, {"description", "Add Lives"}});

	// A dry run answers what the call then does, and writes nothing.
	Json const dry = fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", operations}, {"dryRun", true}}));
	CHECK(dry["dryRun"] == true);
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == original);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);

	Json const edited = fixture.Ok("graph.edit", params);
	CHECK(edited["asset"] == graph);
	CHECK(edited["changed"] == true);
	CHECK(edited["dryRun"] == false);
	CHECK(edited["analyzed"] == false);
	CHECK(edited["analyses"] == 0);
	CHECK(edited["warnings"] == Json::array());
	CHECK(edited["labels"] == Json::object({{"$lives", 31}, {"$get", 32}}));
	CHECK(edited["results"] == Json::array({Json::object({{"id", 31}}), Json::object({{"id", 32}}), Json::object()}));
	CHECK(dry["revision"] == edited["revision"]);
	CHECK(dry["results"] == edited["results"]);
	std::string const written = fixture.ReadFile("Scripts/Counter.sgraph");
	VisualScriptDocument const result = fixture.GetDocument(graph);
	CHECK(written == VisualScriptSerializer::WriteText(result));
	CHECK(edited["revision"] == RevisionOf(written));
	CHECK(edited["programRevision"] == ProgramRevisionOf(result));
	CHECK(result.Variables.back().Name == "Lives");
	CHECK(result.Graph.Nodes.back().Settings == Json::object({{"Variable", 31}}));
	CHECK(result.Graph.Nodes[0].Comment == "Start");

	// One step with the description given; undo gives the file its bytes back, redo writes them again.
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps + 1);
	CHECK(fixture.Ok("editor.status")["undo"] == "Add Lives in Counter");
	CHECK(fixture.Ok("editor.undo")["undone"] == "Add Lives in Counter");
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == original);
	CHECK(fixture.Ok("editor.redo")["redone"] == "Add Lives in Counter");
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == written);

	// An empty batch changes nothing and records no step; the default description is graph.set's.
	Json const empty = fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", Json::array()}}));
	CHECK(empty["changed"] == false);
	CHECK(empty["revision"] == edited["revision"]);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps + 1);
	Json const describe = Json::array({Json::object({{"op", "SetDescription"}, {"description", "Counts"}})});
	fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", describe}}));
	CHECK(fixture.Ok("editor.status")["undo"] == "Edit Visual Script in Counter");

	// Revisions: a user's move changes the revision but not the program revision; a value changes both.
	Json const current = fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", Json::array()}}));
	std::string const revision = current["revision"].get<std::string>();
	std::string const program = current["programRevision"].get<std::string>();
	Json const move = Json::array({Json::object({{"op", "Move"}, {"elements", Json::array({10})}, {"offset", Json::array({16, 0})}})});
	fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", move}}));
	CommandError const stale =
		fixture.Fails("graph.edit", Json::object({{"graph", graph}, {"operations", Json::array()}, {"revision", revision}}));
	CHECK(stale.Code == AutomationErrorCode::InvalidOperation);
	CHECK(stale.Data["path"] == "revision");
	CHECK(fixture.Ok("graph.edit",
	                 Json::object({{"graph", graph}, {"operations", Json::array()}, {"programRevision", program}}))["changed"] == false);
	Json const value = Json::array({Json::object({{"op", "SetVariable"}, {"variable", "Lives"}, {"value", 4}})});
	fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", value}}));
	CommandError const changedProgram =
		fixture.Fails("graph.edit", Json::object({{"graph", graph}, {"operations", Json::array()}, {"programRevision", program}}));
	CHECK(changedProgram.Data["path"] == "programRevision");
	CHECK(changedProgram.Data["programRevision"] == ProgramRevisionOf(fixture.GetDocument(graph)));

	// Both given: both must match. A user's move since refuses the revision; a stale program revision is refused with a
	// current revision; a current pair passes.
	Json const latest = fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", Json::array()}}));
	auto const both = [&graph](Json revisionGiven, Json programGiven)
	{
		return Json::object({{"graph", graph},
		                     {"operations", Json::array()},
		                     {"revision", std::move(revisionGiven)},
		                     {"programRevision", std::move(programGiven)}});
	};
	CHECK(fixture.Ok("graph.edit", both(latest["revision"], latest["programRevision"]))["changed"] == false);
	fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", move}}));
	CommandError const movedSince = fixture.Fails("graph.edit", both(latest["revision"], latest["programRevision"]));
	CHECK(movedSince.Code == AutomationErrorCode::InvalidOperation);
	CHECK(movedSince.Data["path"] == "revision");
	Json const afterMove = fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", Json::array()}}));
	CHECK(afterMove["programRevision"] == latest["programRevision"]);
	CommandError const staleProgram = fixture.Fails("graph.edit", both(afterMove["revision"], program));
	CHECK(staleProgram.Code == AutomationErrorCode::InvalidOperation);
	CHECK(staleProgram.Data["path"] == "programRevision");
	CHECK(fixture.Ok("graph.edit", both(afterMove["revision"], afterMove["programRevision"]))["changed"] == false);
}

TEST_CASE("GraphCommands: graph.edit refuses what does not apply, naming the operation, and runs degraded without the runtime")
{
	GraphFixture fixture;
	VisualScriptDocument const stacked = MakeStackedDocument();
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", stacked);
	std::string const original = fixture.ReadFile("Scripts/Counter.sgraph");
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	auto const edit = [&graph](Json operations)
	{
		return Json::object({{"graph", graph}, {"operations", std::move(operations)}});
	};

	// An operation that does not apply: InvalidParams naming it and its field; nothing written.
	CommandError const invalid = fixture.Fails(
		"graph.edit",
		edit(Json::array({Json::object({{"op", "SetDescription"}, {"description", "x"}}),
	                      Json::object({{"op", "Move"}, {"elements", Json::array({99})}, {"offset", Json::array({1, 1})}})})));
	CHECK(invalid.Code == AutomationErrorCode::InvalidParams);
	CHECK(invalid.Data["path"] == "operations[1].elements[0]");
	CHECK(invalid.Data["operation"] == 1);
	CHECK(invalid.Data["op"] == "Move");
	CHECK(invalid.Message.starts_with("operations[1] (Move): "));
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == original);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);

	// Connecting needs the pins of nodes, which only the scripting runtime knows.
	Json const link = Json::object({{"op", "Connect"}, {"from", Json::array({14, "Value"})}, {"to", Json::array({12, "A"})}});
	CommandError const connect = fixture.Fails("graph.edit", edit(Json::array({link})));
	CHECK(connect.Code == AutomationErrorCode::Unavailable);
	CHECK(connect.Data["operation"] == 0);
	CHECK(connect.Data["op"] == "Connect");
	CHECK(connect.Data["path"] == "operations[0]");
	CHECK(fixture.Fails("graph.edit", Json::object({{"graph", graph}, {"operations", Json::array()}, {"refuseNewErrors", true}})).Code ==
	      AutomationErrorCode::Unavailable);

	// Adding, deleting (without bridging) and measured arranging run without it.
	Json const degraded = fixture.Ok(
		"graph.edit", edit(Json::array({Json::object({{"op", "AddNode"}, {"type", "Flow.Branch"}}),
	                                    Json::object({{"op", "DeleteVariable"}, {"variable", "Speed"}}),
	                                    Json::object({{"op", "Delete"}, {"elements", Json::array({12})}}),
	                                    Json::object({{"op", "Arrange"}, {"scope", "Created"}}), Json::object({{"op", "Arrange"}})})));
	CHECK(degraded["changed"] == true);
	CHECK(degraded["results"][1] == Json::object({{"removedNodes", 1}}));
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps + 1);
	fixture.Ok("editor.undo");
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == original);

	// A graph without a file of its own.
	AssetHandle const memory = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(stacked), "Memory");
	CHECK(fixture.Fails("graph.edit", Json::object({{"graph", memory.ToString()}, {"operations", Json::array()}})).Data["path"] == "graph");
}

TEST_CASE("GraphCommands: graph.edit with the scripting runtime connects pins and answers the analysis of the result")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	std::string const graph = fixture.Ok("graph.create", Json::object({{"path", "Scripts/Spin.sgraph"}}))["asset"].get<std::string>();
	VisualScriptDocument const created = fixture.GetDocument(graph);
	uint32_t update = 0;
	for (VisualScriptNode const& node : created.Graph.Nodes)
	{
		update = node.Type == "Event.Update" ? node.ID : update;
	}
	REQUIRE(update != 0);
	Json const operations = Json::array({
		Json::object({{"op", "AddNode"},
	                  {"type", "Call:Strada.Log.Info(string)"},
	                  {"literals", Json::object({{"Message", "tick"}})},
	                  {"as", "$log"}}),
		Json::object({{"op", "Connect"}, {"from", Json::array({update, "Then"})}, {"to", Json::array({"$log", "Execute"})}}),
		Json::object({{"op", "Arrange"}, {"scope", "Created"}}),
	});
	Json const edited = fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", operations}, {"document", true}}));
	CHECK(edited["changed"] == true);
	CHECK(edited["analyzed"] == true);
	CHECK(edited["diagnostics"] == Json::array());
	// One analysis at the Connect and one before the measured Arrange, which the result reuses (arranging changed no
	// program), and the stored document's, which New compares with.
	CHECK(edited["analyses"] == 3);
	uint32_t const log = edited["labels"]["$log"].get<uint32_t>();
	// The nodes the batch created or changed: the new Log only.
	REQUIRE(edited["nodes"].size() == 1);
	CHECK(edited["nodes"][0]["ID"] == log);
	VisualScriptDocument const result = fixture.GetDocument(graph);
	CHECK(edited["document"] == VisualScriptSerializer::SerializeDocument(result));
	CHECK(std::find(result.Graph.Links.begin(), result.Graph.Links.end(), MakeLink(update, "Then", log, "Execute")) !=
	      result.Graph.Links.end());
	VisualScriptNode const& added = result.Graph.Nodes.back();
	REQUIRE(added.Position.has_value());
	CHECK(added.Position->x > 0.0f);
	Json const all = fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", Json::array()}, {"nodes", "All"}}));
	CHECK(all["nodes"].size() == result.Graph.Nodes.size());

	// refuseNewErrors: an edit that adds an error writes nothing.
	std::string const before = fixture.ReadFile("Scripts/Spin.sgraph");
	Json const typo = Json::array({Json::object({{"op", "AddNode"}, {"type", "Flow.Brnach"}, {"position", Json::array({0, 600})}})});
	CommandError const refused =
		fixture.Fails("graph.edit", Json::object({{"graph", graph}, {"operations", typo}, {"refuseNewErrors", true}}));
	CHECK(refused.Code == AutomationErrorCode::InvalidOperation);
	CHECK(refused.Data["diagnostics"][0]["Code"] == "UnknownNodeType");
	CHECK(fixture.ReadFile("Scripts/Spin.sgraph") == before);
}

TEST_CASE("GraphCommands: graph.get and graph.set return revisions, which graph.set takes to refuse a graph changed since")
{
	GraphFixture fixture;
	VisualScriptDocument const counter = MakeCounterDocument();
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", counter);
	std::string const written = fixture.ReadFile("Scripts/Counter.sgraph");
	Json const read = fixture.Ok("graph.get", Json::object({{"graph", graph}}));
	CHECK(read["revision"] == RevisionOf(written));
	CHECK(read["programRevision"] == ProgramRevisionOf(counter));
	std::string const revision = read["revision"].get<std::string>();
	std::string const program = read["programRevision"].get<std::string>();

	// The same document with the revisions read: written as given.
	Json const content = VisualScriptSerializer::SerializeDocument(counter);
	Json const same = fixture.Ok(
		"graph.set", Json::object({{"graph", graph}, {"document", content}, {"revision", revision}, {"programRevision", program}}));
	CHECK(same["changed"] == false);
	CHECK(same["revision"] == revision);
	CHECK(same["programRevision"] == program);

	// A user's move changes the revision, not the program revision: graph.set refuses the first and takes the second.
	Json const move = Json::array({Json::object({{"op", "Move"}, {"elements", Json::array({10})}, {"offset", Json::array({32, 0})}})});
	fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", move}}));
	std::string const moved = fixture.ReadFile("Scripts/Counter.sgraph");
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	VisualScriptDocument renamed = counter;
	renamed.Variables[0].Name = "Pace";
	Json const renamedContent = VisualScriptSerializer::SerializeDocument(renamed);
	CommandError const stale =
		fixture.Fails("graph.set", Json::object({{"graph", graph}, {"document", renamedContent}, {"revision", revision}}));
	CHECK(stale.Code == AutomationErrorCode::InvalidOperation);
	CHECK(stale.Data == Json::object({{"path", "revision"}, {"revision", RevisionOf(moved)}}));
	CHECK(stale.Message.find("read it again before setting it") != std::string::npos);
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == moved);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
	// Both given must both match.
	CHECK(fixture
	          .Fails("graph.set",
	                 Json::object({{"graph", graph}, {"document", renamedContent}, {"revision", revision}, {"programRevision", program}}))
	          .Data["path"] == "revision");
	Json const set = fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", renamedContent}, {"programRevision", program}}));
	CHECK(set["changed"] == true);
	CHECK(set["revision"] == RevisionOf(fixture.ReadFile("Scripts/Counter.sgraph")));
	CHECK(set["programRevision"] == ProgramRevisionOf(fixture.GetDocument(graph)));
	CHECK(set["programRevision"] != program);
	// Now the program changed: the old program revision is refused, naming the current one.
	CommandError const changed =
		fixture.Fails("graph.set", Json::object({{"graph", graph}, {"document", content}, {"programRevision", program}}));
	CHECK(changed.Data == Json::object({{"path", "programRevision"}, {"programRevision", set["programRevision"]}}));
	// A revision that is no revision is refused as a parameter.
	CHECK(fixture.Fails("graph.set", Json::object({{"graph", graph}, {"document", content}, {"revision", "ABCDEF0123456789"}})).Code ==
	      AutomationErrorCode::InvalidParams);

	// A file of an older version has the revision of its document as it loads, which is the revision its next write has.
	VisualScriptDocument const current = fixture.GetDocument(graph);
	std::string older = VisualScriptSerializer::WriteText(current);
	size_t const version = older.find("\"Version\": 3,");
	REQUIRE(version != std::string::npos);
	older.replace(version, 13, "\"Version\": 2,");
	fixture.WriteFile("Scripts/Counter.sgraph", older);
	std::filesystem::last_write_time(fixture.GetFile("Scripts/Counter.sgraph"),
	                                 std::filesystem::file_time_type::clock::now() + std::chrono::hours(1));
	fixture.Ok("asset.refresh");
	Json const reread = fixture.Ok("graph.get", Json::object({{"graph", graph}}));
	CHECK(reread["revision"] == RevisionOf(VisualScriptSerializer::WriteText(current)));
	CHECK(reread["revision"] != RevisionOf(older));
	CHECK(fixture.Ok("graph.edit",
	                 Json::object({{"graph", graph}, {"operations", Json::array()}, {"revision", reread["revision"]}}))["changed"] ==
	      false);

	// A file that does not load has no revision.
	fixture.WriteFile("Scripts/Counter.sgraph", "{ \"Strada\": 1 }");
	std::filesystem::last_write_time(fixture.GetFile("Scripts/Counter.sgraph"),
	                                 std::filesystem::file_time_type::clock::now() + std::chrono::hours(2));
	fixture.Ok("asset.refresh");
	CommandError const unloadable =
		fixture.Fails("graph.set", Json::object({{"graph", graph}, {"document", content}, {"revision", revision}}));
	CHECK(unloadable.Code == AutomationErrorCode::InvalidOperation);
	CHECK(unloadable.Data == Json::object({{"path", "revision"}, {"revision", Json()}}));
	CHECK(fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", content}}))["changed"] == true);
}

TEST_CASE("GraphCommands: graph.edit refuses files that do not load, are read-only or are missing, and writes nothing for no change")
{
	GraphFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const original = fixture.ReadFile("Scripts/Counter.sgraph");
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	Json const describe = Json::array({Json::object({{"op", "SetDescription"}, {"description", "Counts"}})});
	auto const edit = [&graph](Json operations)
	{
		return Json::object({{"graph", graph}, {"operations", std::move(operations)}});
	};

	// Operations that leave the document as it is change nothing and record no step.
	Json const unchanged = fixture.Ok("graph.edit", edit(Json::array({Json::object({{"op", "SetDescription"}, {"description", "Counts"}}),
	                                                                  Json::object({{"op", "SetDescription"}, {"description", ""}})})));
	CHECK(unchanged["changed"] == false);
	CHECK(unchanged["revision"] == RevisionOf(original));
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == original);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);

	// A dry run answers what the call then answers, but for dryRun itself.
	Json dry = fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", describe}, {"dryRun", true}, {"document", true}}));
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == original);
	Json const real = fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", describe}, {"document", true}}));
	CHECK(dry["dryRun"] == true);
	dry["dryRun"] = false;
	CHECK(dry == real);
	fixture.Ok("editor.undo");
	CHECK(fixture.ReadFile("Scripts/Counter.sgraph") == original);

	// Read-only: a file error, nothing recorded.
	{
		Testing::ReadOnlyFileScope const readOnly(fixture.GetFile("Scripts/Counter.sgraph"));
		CommandError const refused = fixture.Fails("graph.edit", edit(describe));
		CHECK(refused.Code == AutomationErrorCode::FileError);
		CHECK(refused.Message == "the file of visual script 'Scripts/Counter.sgraph' is read-only");
	}
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);

	// A file that does not load: graph.set replaces it.
	fixture.WriteFile("Scripts/Counter.sgraph", "{ \"Strada\": 1 }");
	std::filesystem::last_write_time(fixture.GetFile("Scripts/Counter.sgraph"),
	                                 std::filesystem::file_time_type::clock::now() + std::chrono::hours(1));
	fixture.Ok("asset.refresh");
	CommandError const unloadable = fixture.Fails("graph.edit", edit(describe));
	CHECK(unloadable.Code == AutomationErrorCode::FileError);
	CHECK(unloadable.Message.starts_with("the file of visual script 'Scripts/Counter.sgraph' does not load: "));
	CHECK(unloadable.Message.ends_with("; graph.set replaces it"));

	// A missing file.
	fixture.WriteFile("Scripts/Counter.sgraph", original);
	std::filesystem::last_write_time(fixture.GetFile("Scripts/Counter.sgraph"),
	                                 std::filesystem::file_time_type::clock::now() + std::chrono::hours(2));
	fixture.Ok("asset.refresh");
	REQUIRE(fixture.Ok("graph.edit", edit(Json::array()))["changed"] == false);
	REQUIRE(FileSystem::Remove(fixture.GetFile("Scripts/Counter.sgraph")).IsOk());
	CommandError const missing = fixture.Fails("graph.edit", edit(describe));
	CHECK(missing.Code == AutomationErrorCode::FileError);
	CHECK(missing.Message.find("missing") != std::string::npos);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
}

TEST_CASE("GraphCommands: graph.edit while playing reaches the running graph, and its step is carried over Stop")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	VisualScriptDocument document = MakeDelayDocument();
	document.Graph.Nodes[1].Inputs = Json::object({{"Duration", 5.0}});
	std::string const graph = fixture.CreateGraph("Scripts/Delay.sgraph", document);
	std::string const waiter = fixture.CreateEntity("Waiter", Json::object({{"VisualScript", Json::object({{"Graph", graph}})}}));
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	auto const done = [&]
	{
		Json entity = fixture.Ok("entity.get", Json::object({{"entity", waiter}}));
		return entity["components"]["VisualScript"]["Variables"]["Done"]["Value"];
	};

	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(2, Timestep(FrameTime));
	CHECK(done() == false);
	// Allowed while playing, as graph.set is: the running graph takes the edit.
	Json const operations = Json::array({Json::object({{"op", "Delete"}, {"elements", Json::array({11})}})});
	Json const edited =
		fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", operations}, {"description", "Skip the wait"}}));
	CHECK(edited["changed"] == true);
	CHECK(edited["analyzed"] == true);
	CHECK(edited["diagnostics"] == Json::array());
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Skip the wait in Delay");
	fixture.Play.Advance(2, Timestep(FrameTime));

	// The step stays undoable after Stop.
	fixture.Play.Stop();
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Skip the wait in Delay");
	VisualScriptDocument const result = fixture.GetDocument(graph);
	CHECK(std::find(result.Graph.Links.begin(), result.Graph.Links.end(), MakeLink(10, "Then", 12, "Execute")) != result.Graph.Links.end());
	CHECK(fixture.Ok("editor.undo")["undone"] == "Skip the wait in Delay");
	CHECK(fixture.GetDocument(graph) == document);
}

TEST_CASE("GraphCommands: an open graph window takes graph.edit's change as an outside change")
{
	GraphWindowFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Window.sgraph", MakeWindowDocument());
	fixture.Ok("graph.open", Json::object({{"graph", graph}}));
	fixture.Frames(3);
	VisualScriptEditorPanel* const panel = fixture.Editors.FindPanel(GraphWindowFixture::GetHandle(graph));
	REQUIRE(panel != nullptr);

	Json const operations =
		Json::array({Json::object({{"op", "SetInput"}, {"node", 11}, {"pin", "Message"}, {"value", "Bye"}}),
	                 Json::object({{"op", "Move"}, {"elements", Json::array({12})}, {"offset", Json::array({0, 64})}})});
	fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", operations}}));
	fixture.Frames(1);
	VisualScriptEditorModel const& model = panel->GetModel();
	REQUIRE(model.GetStoredDocument() != nullptr);
	CHECK(*model.GetStoredDocument() == fixture.GetDocument(graph));
	// The window flashes what changed.
	std::vector<uint32_t> changed(model.ChangedElements().begin(), model.ChangedElements().end());
	std::sort(changed.begin(), changed.end());
	CHECK(changed == std::vector<uint32_t>{11, 12});
}

TEST_CASE("GraphCommands: graph.copy's text pasted by graph.edit binds a variable of the same name and adds a missing one")
{
	GraphFixture fixture;
	std::string const source = fixture.CreateGraph("Scripts/Window.sgraph", MakeWindowDocument());
	VisualScriptDocument target;
	target.Variables.push_back(MakeVariable(5, "Speed", "float", 1.0));
	target.Graph.Nodes.push_back(MakeNode(6, "Event.Update"));
	std::string const graph = fixture.CreateGraph("Scripts/Target.sgraph", target);

	// Get Speed from the event graph, Set Score from AddScore's.
	std::string const speed = fixture.Ok("graph.copy", Json::object({{"graph", source}, {"elements", Json::array({12})}}))["text"];
	std::string const score =
		fixture.Ok("graph.copy", Json::object({{"graph", source}, {"function", "AddScore"}, {"elements", Json::array({33})}}))["text"];
	Json const operations = Json::array({
		Json::object({{"op", "Paste"}, {"clipboard", speed}, {"at", Json::array({0, 200})}, {"labels", Json::array({"$speed"})}}),
		Json::object({{"op", "Paste"}, {"clipboard", score}, {"labels", Json::array({"$score"})}}),
	});
	Json const pasted = fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", operations}}));
	VisualScriptDocument const result = fixture.GetDocument(graph);
	REQUIRE(result.Variables.size() == 2);
	CHECK(result.Variables[1].Name == "Score");
	CHECK(result.Variables[1].Type == "int");
	uint32_t const getter = pasted["labels"]["$speed"].get<uint32_t>();
	uint32_t const setter = pasted["labels"]["$score"].get<uint32_t>();
	CHECK(pasted["results"][0]["ids"] == Json::array({getter}));
	auto const settings = [&result](uint32_t id)
	{
		auto const node = std::find_if(result.Graph.Nodes.begin(), result.Graph.Nodes.end(),
		                               [id](VisualScriptNode const& candidate)
		                               {
										   return candidate.ID == id;
									   });
		REQUIRE(node != result.Graph.Nodes.end());
		return node->Settings;
	};
	CHECK(settings(getter) == Json::object({{"Variable", 5}}));
	CHECK(settings(setter) == Json::object({{"Variable", result.Variables[1].ID}}));
}

TEST_CASE("GraphCommands: graph.library's notes are the library's members without a node, filtered by category and query")
{
	Result<Scope<VisualScriptLibrary>> const parsed = VisualScriptLibrary::Parse(R"({ "Types": [], "Nodes": [], "Notes": [
		{ "Member": "TestGame.Probe.Swap", "Category": "Game/Probe", "Message": "TestGame.Probe.Swap: has a ref parameter" },
		{ "Member": "TestGame.Probe.Span", "Category": "Game/Probe/Deep", "Message": "TestGame.Probe.Span: takes a ref struct" },
		{ "Member": "TestGame.Mover.Wait", "Category": "Game/Mover", "Message": "TestGame.Mover.Wait: is latent" } ] })");
	REQUIRE_MESSAGE(parsed.IsOk(), (parsed ? std::string() : parsed.GetError()));
	VisualScriptLibrary const& library = *parsed.GetValue();
	auto const members = [&library](std::string_view query, std::string_view category)
	{
		std::vector<std::string> listed;
		for (Json const& note : ListVisualScriptLibraryNotes(library, query, category))
		{
			listed.push_back(note["member"].get<std::string>());
		}
		return listed;
	};
	CHECK(ListVisualScriptLibraryNotes(library, "", "")[0] ==
	      Json::object(
			  {{"member", "TestGame.Probe.Swap"}, {"category", "Game/Probe"}, {"message", "TestGame.Probe.Swap: has a ref parameter"}}));
	CHECK(members("", "") == std::vector<std::string>{"TestGame.Probe.Swap", "TestGame.Probe.Span", "TestGame.Mover.Wait"});
	// A category holds the ones below it, ignoring case; a prefix of a category's name is no category.
	CHECK(members("", "game/probe") == std::vector<std::string>{"TestGame.Probe.Swap", "TestGame.Probe.Span"});
	CHECK(members("", "Game/Probe/Deep") == std::vector<std::string>{"TestGame.Probe.Span"});
	CHECK(members("", "Game/Pro").empty());
	// The query is in the member or the message, ignoring case.
	CHECK(members("LATENT", "") == std::vector<std::string>{"TestGame.Mover.Wait"});
	CHECK(members("probe.s", "Game") == std::vector<std::string>{"TestGame.Probe.Swap", "TestGame.Probe.Span"});
	CHECK(members("ref struct", "Game/Mover").empty());
	CHECK(ListVisualScriptLibraryNotes(*VisualScriptLibrary::Parse(R"({ "Types": [], "Nodes": [] })").GetValue(), "", "") == Json::array());
}

namespace
{
	// The diagnostics of a result with the code.
	std::vector<Json> FindDiagnostics(Json const& result, std::string_view code)
	{
		std::vector<Json> found;
		for (Json const& diagnostic : result["diagnostics"])
		{
			if (diagnostic["Code"] == std::string(code))
			{
				found.push_back(diagnostic);
			}
		}
		return found;
	}

	// The hints of a result as "<node>:<kind>".
	std::vector<std::string> ListHints(Json const& result)
	{
		std::vector<std::string> hints;
		for (Json const& hint : result["hints"])
		{
			hints.push_back(fmt::format("{}:{}", hint["node"].get<uint32_t>(), hint["kind"].get<std::string>()));
		}
		return hints;
	}

	// Update (10) sets Score (11) to Speed (12), whose output the link names "Valu".
	VisualScriptDocument MakeMisspelledDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Speed", "int", 1));
		document.Variables.push_back(MakeVariable(2, "Score", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 2}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Variable.Get", Json::object({{"Variable", 1}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(12, "Valu", 11, "Value"));
		return document;
	}

	// Agents/Spinner.json: graph.edit's parameters as the documentation teaches them.
	Json LoadSpinnerBatch()
	{
		Result<std::string> const text =
			FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Agents" / "Spinner.json");
		REQUIRE(text.IsOk());
		Json batch = Json::parse(text.GetValue(), nullptr, false);
		REQUIRE(batch.is_object());
		return batch;
	}
}

TEST_CASE("GraphCommands: graph.edit's diagnostics carry their quick fixes as Fix operations, New and the label of their node")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Score.sgraph", MakeMisspelledDocument());
	auto const edit = [&graph](Json operations)
	{
		return Json::object({{"graph", graph}, {"operations", std::move(operations)}});
	};

	// An empty batch answers the stored document's diagnostics, none of them new, with the fixes that repair them.
	Json const current = fixture.Ok("graph.edit", edit(Json::array()));
	CHECK(current["changed"] == false);
	std::vector<Json> const unknown = FindDiagnostics(current, "UnknownPin");
	REQUIRE(unknown.size() == 1);
	CHECK(unknown[0]["New"] == false);
	CHECK_FALSE(unknown[0].contains("Label"));
	Json useValue;
	for (Json const& fix : unknown[0]["Fixes"])
	{
		CHECK(fix["operation"]["op"] == "Fix");
		CHECK(fix["operation"]["code"] == "UnknownPin");
		CHECK(fix["operation"]["fix"] == fix["label"]);
		useValue = fix["label"] == "Use Value" ? fix["operation"] : useValue;
	}
	REQUIRE(useValue.is_object());
	CHECK(useValue["node"] == 12);
	CHECK(current["summary"]["errors"].get<uint32_t>() >= 1);
	CHECK(current["summary"]["newErrors"] == 0);
	CHECK(current["summary"]["newWarnings"] == 0);

	// The fix's operation, appended unchanged to a batch, repairs the link.
	Json const fixed = fixture.Ok("graph.edit", edit(Json::array({useValue})));
	CHECK(fixed["changed"] == true);
	CHECK(fixed["results"][0] == Json::object({{"fix", "Use Value"}}));
	CHECK(FindDiagnostics(fixed, "UnknownPin").empty());
	VisualScriptDocument const repaired = fixture.GetDocument(graph);
	CHECK(std::find(repaired.Graph.Links.begin(), repaired.Graph.Links.end(), MakeLink(12, "Value", 11, "Value")) !=
	      repaired.Graph.Links.end());

	// A node the batch adds with a type that does not exist: its diagnostic is new and names the node's label, and every fix
	// it offers applies as it is given.
	Json const typo = fixture.Ok(
		"graph.edit",
		Json::object(
			{{"graph", graph},
	         {"operations", Json::array({Json::object(
								{{"op", "AddNode"}, {"type", "Flow.Brnach"}, {"position", Json::array({0, 400})}, {"as", "$branch"}})})},
	         {"dryRun", true}}));
	std::vector<Json> const types = FindDiagnostics(typo, "UnknownNodeType");
	REQUIRE(types.size() == 1);
	CHECK(types[0]["New"] == true);
	CHECK(types[0]["Label"] == "$branch");
	CHECK(typo["summary"]["newErrors"] == 1);
	REQUIRE_FALSE(types[0]["Fixes"].empty());
	fixture.Ok("graph.edit", edit(Json::array({Json::object(
								 {{"op", "AddNode"}, {"type", "Flow.Brnach"}, {"position", Json::array({0, 400})}, {"as", "$branch"}})})));
	for (Json const& fix : types[0]["Fixes"])
	{
		CAPTURE(fix);
		Json const applied =
			fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", Json::array({fix["operation"]})}, {"dryRun", true}}));
		CHECK(FindDiagnostics(applied, "UnknownNodeType").empty());
	}
}

TEST_CASE("GraphCommands: graph.edit's hints name nodes left without flow or readers, and its warnings the canonical types it stored")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	VisualScriptDocument document;
	document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
	document.Graph.Nodes.push_back(MakeNode(11, "Call:Strada.Log.Info(string)", Json::object(), Json::object({{"Message", "A"}})));
	document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	std::string const graph = fixture.CreateGraph("Scripts/Hints.sgraph", document);

	// A Connect that takes Update's flow from Log A leaves A without flow; an Add read by nothing is unused.
	Json const operations = Json::array({
		Json::object(
			{{"op", "AddNode"}, {"type", "Call:Strada.Log.Info(string)"}, {"literals", Json::object({{"Message", "B"}})}, {"as", "$b"}}),
		Json::object({{"op", "Connect"}, {"from", Json::array({10, "Then"})}, {"to", Json::array({"$b", "Execute"})}}),
		Json::object({{"op", "AddNode"}, {"type", "Operator.Add"}, {"as", "$add"}}),
		Json::object({{"op", "Arrange"}, {"scope", "Created"}}),
	});
	Json const edited = fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", operations}}));
	uint32_t const b = edited["labels"]["$b"].get<uint32_t>();
	uint32_t const add = edited["labels"]["$add"].get<uint32_t>();
	CHECK(edited["results"][1] ==
	      Json::object(
			  {{"replaced", Json::array({Json::object({{"from", Json::array({10, "Then"})}, {"to", Json::array({11, "Execute"})}})})}}));
	std::vector<std::string> const hints = ListHints(edited);
	CHECK(Contains(hints, fmt::format("{}:Unused", add)));
	CHECK(Contains(hints, "11:NeverRuns"));
	CHECK_FALSE(Contains(hints, fmt::format("{}:NeverRuns", b)));
	for (Json const& hint : edited["hints"])
	{
		CHECK_FALSE(hint["message"].get<std::string>().empty());
	}

	// A type named without its signature is stored under its exact one, which the warnings say.
	Json const keys =
		fixture.Ok("graph.edit", Json::object({{"graph", graph},
	                                           {"operations", Json::array({Json::object({{"op", "AddNode"},
	                                                                                     {"type", "Call:Strada.Input.IsKeyDown"},
	                                                                                     {"literals", Json::object({{"Key", "Space"}})},
	                                                                                     {"position", Json::array({0, 600})}})})}}));
	CHECK(keys["warnings"] == Json::array({"node updated: Call:Strada.Input.IsKeyDown → Call:Strada.Input.IsKeyDown(Strada.KeyCode)"}));
	CHECK(fixture.GetDocument(graph).Graph.Nodes.back().Type == "Call:Strada.Input.IsKeyDown(Strada.KeyCode)");
}

TEST_CASE("GraphCommands: graph.edit keeps the analysis of the document it wrote for the next call, until the library changes")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	auto const setSpeed = [&](int32_t speed)
	{
		Json const operations = Json::array({Json::object({{"op", "SetVariable"}, {"variable", "Speed"}, {"value", speed}})});
		return fixture.Ok("graph.edit", Json::object({{"graph", graph}, {"operations", operations}}))["analyses"].get<uint32_t>();
	};
	// The result's analysis, and the stored document's, which New compares with.
	CHECK(setSpeed(2) == 2);
	// The document written is the next call's stored one, whose analysis the first call kept.
	CHECK(setSpeed(3) == 1);
	CHECK(setSpeed(4) == 1);
	// An undo puts another document in the asset: analyzed again.
	fixture.Ok("editor.undo");
	CHECK(setSpeed(5) == 2);
	// A new node library (the game's scripts loaded again) drops what was kept.
	REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
	CHECK(setSpeed(6) == 2);
	CHECK(setSpeed(7) == 1);
}

TEST_CASE("GraphCommands: graph.library lists no notes for scripts whose marked members all have nodes")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	Json const listed = fixture.Ok("graph.library", Json::object({{"limit", 1}}));
	CHECK(listed["notes"] == Json::array());
	CHECK(fixture.Ok("graph.library", Json::object({{"category", "Game"}, {"query", "x"}}))["notes"] == Json::array());
}

TEST_CASE("GraphCommands: the documentation's spinner batch makes one undo step without an error in three analyses")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);
	REQUIRE(library->FindNode("Expression") != nullptr);
	Json const batch = LoadSpinnerBatch();
	REQUIRE(fixture.Ok("graph.create", Json::object({{"path", "Scripts/Spinner.sgraph"}}))["asset"].is_string());
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();

	Json const spun = fixture.Ok("graph.edit", batch);
	CHECK(spun["changed"] == true);
	CHECK(spun["summary"]["errors"] == 0);
	CHECK(spun["analyses"] == 3);
	CHECK(spun["results"].size() == batch["operations"].size());
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Spin at Speed degrees per second in Spinner");
	CHECK(spun["hints"] == Json::array());

	// Without the link of DeltaTime, the formula reads dt as 0: a hint says so.
	Json broken = batch;
	Json& operations = broken["operations"];
	operations.erase(std::remove_if(operations.begin(), operations.end(),
	                                [](Json const& operation)
	                                {
										return operation["op"] == "Connect" && operation["from"] == Json::array({2, "DeltaTime"});
									}),
	                 operations.end());
	broken["dryRun"] = true;
	REQUIRE(fixture.Ok("editor.undo")["undone"] == "Spin at Speed degrees per second in Spinner");
	Json const unlinked = fixture.Ok("graph.edit", broken);
	uint32_t const next = unlinked["labels"]["$next"].get<uint32_t>();
	CHECK(Contains(ListHints(unlinked), fmt::format("{}:UnlinkedInput", next)));

	// A broken spinner: the setter's type misspelled, and the links into it left out. Its diagnostic is new and names the
	// node's label, and every fix it offers applies unchanged in a follow-up batch.
	Json misspelled = batch;
	Json& misspelledOperations = misspelled["operations"];
	for (Json& operation : misspelledOperations)
	{
		if (operation.value("as", "") == "$turn")
		{
			operation["type"] = "Set:Strada.TransformComponent.EulerAngels";
		}
	}
	misspelledOperations.erase(std::remove_if(misspelledOperations.begin(), misspelledOperations.end(),
	                                          [](Json const& operation)
	                                          {
												  return operation["op"] == "Connect" && operation["to"][0] == "$turn";
											  }),
	                           misspelledOperations.end());
	Json const typo = fixture.Ok("graph.edit", misspelled);
	CHECK(typo["changed"] == true);
	std::vector<Json> const unknown = FindDiagnostics(typo, "UnknownNodeType");
	REQUIRE(unknown.size() == 1);
	CHECK(unknown[0]["New"] == true);
	CHECK(unknown[0]["Label"] == "$turn");
	REQUIRE_FALSE(unknown[0]["Fixes"].empty());
	for (Json const& fix : unknown[0]["Fixes"])
	{
		CAPTURE(fix);
		Json const applied = fixture.Ok(
			"graph.edit", Json::object({{"graph", batch["graph"]}, {"operations", Json::array({fix["operation"]})}, {"dryRun", true}}));
		CHECK(applied["changed"] == true);
		CHECK(FindDiagnostics(applied, "UnknownNodeType").empty());
	}
}

TEST_CASE("GraphCommands: an idle graph.edit leaves a file with unplaced nodes and signature-less types as it is, in one analysis")
{
	// A file written by hand (graph.set would place the node and store the type canonically): a Branch without a position, and
	// a call named without its signature, which the runtime resolves inexactly.
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	VisualScriptDocument document;
	VisualScriptNode update;
	update.ID = 1;
	update.Type = "Event.Update";
	update.Position = glm::vec2(0.0f);
	VisualScriptNode branch;
	branch.ID = 3;
	branch.Type = "Flow.Branch";
	VisualScriptNode key;
	key.ID = 4;
	key.Type = "Call:Strada.Input.IsKeyDown";
	key.Position = glm::vec2(0.0f, 240.0f);
	key.Inputs = Json::object({{"Key", "Space"}});
	document.Graph.Nodes = {update, branch, key};
	document.Graph.Links.push_back(MakeLink(1, "Then", 3, "Execute"));
	document.Graph.Links.push_back(MakeLink(4, "Result", 3, "Condition"));
	fixture.WriteFile("Scripts/Probe.sgraph", VisualScriptSerializer::WriteText(document));
	fixture.Ok("asset.refresh");
	std::string const graph = "asset://Scripts/Probe.sgraph";
	std::string const original = fixture.ReadFile("Scripts/Probe.sgraph");
	std::string const revision = fixture.Ok("graph.get", Json::object({{"graph", graph}}))["revision"].get<std::string>();
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	auto const edit = [&graph](Json operations, bool dryRun)
	{
		return Json::object({{"graph", graph}, {"operations", std::move(operations)}, {"dryRun", dryRun}});
	};
	auto const expectUnchanged = [&](Json const& answered)
	{
		CHECK(answered["changed"] == false);
		CHECK(answered["revision"] == revision);
		CHECK(answered["warnings"] == Json::array());
		CHECK(answered["analyzed"] == true);
		CHECK(fixture.ReadFile("Scripts/Probe.sgraph") == original);
		CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
	};

	// The first call analyzes the stored document once, for the result and as the stored one; later calls reuse it.
	Json const dry = fixture.Ok("graph.edit", edit(Json::array(), true));
	expectUnchanged(dry);
	CHECK(dry["analyses"] == 1);
	Json const empty = fixture.Ok("graph.edit", edit(Json::array(), false));
	expectUnchanged(empty);
	CHECK(empty["analyses"] == 0);
	// The dry run answered what the call answers, but for the analysis the call found cached.
	Json expected = dry;
	expected["dryRun"] = false;
	expected["analyses"] = 0;
	CHECK(empty == expected);
	// Operations that undo each other leave the file as it is too.
	Json const idle = fixture.Ok("graph.edit", edit(Json::array({Json::object({{"op", "SetDescription"}, {"description", "x"}}),
	                                                             Json::object({{"op", "SetDescription"}, {"description", ""}})}),
	                                                false));
	expectUnchanged(idle);

	// A change of the program places the node and stores the type canonically, as every such commit does, and says so.
	Json const described =
		fixture.Ok("graph.edit", edit(Json::array({Json::object({{"op", "AddVariable"}, {"name", "Probes"}, {"type", "int"}})}), false));
	CHECK(described["changed"] == true);
	CHECK(described["warnings"].size() == 1);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps + 1);
	VisualScriptDocument const written = fixture.GetDocument(graph);
	CHECK(written.Graph.Nodes[1].Position.has_value());
	CHECK(written.Graph.Nodes[2].Type == "Call:Strada.Input.IsKeyDown(Strada.KeyCode)");
}

namespace
{
	// Tests/Data/VisualScripting/Library.sgraph's document: Clamp01 (1, pure, Value with the former name X), Report (2) and
	// Pause (4, latent).
	VisualScriptDocument LoadLibraryDocument()
	{
		DeserializationContext const context;
		Result<VisualScriptDocument> loaded = VisualScriptSerializer::LoadFromFile(
			FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Library.sgraph", context);
		REQUIRE_MESSAGE(loaded.IsOk(), loaded.GetError());
		return loaded.TakeValue();
	}

	// A script whose event graph calls the library's Clamp01 (2, its input given by the former name X) and Report (3), gets
	// the public Health (variable 1) of another entity running door (4), sends door's Hit typed (5) and Opened untyped (6),
	// and has the custom event Opened itself (7).
	VisualScriptDocument MakeCallerDocument(std::string const& library, std::string const& door)
	{
		VisualScriptDocument document;
		document.Graph.Nodes.push_back(MakeNode(1, "Event.Update"));
		document.Graph.Nodes.push_back(
			MakeNode(2, "Function.Call", Json::object({{"Function", 1}, {"Graph", library}}), Json::object({{"X", 2.0}})));
		document.Graph.Nodes.push_back(MakeNode(3, "Function.Call", Json::object({{"Function", 2}, {"Graph", library}})));
		document.Graph.Nodes.push_back(MakeNode(4, "Variable.Get", Json::object({{"Variable", 1}, {"Graph", door}})));
		document.Graph.Nodes.push_back(MakeNode(5, "Event.Send", Json::object({{"Name", "Hit"}, {"Graph", door}})));
		document.Graph.Nodes.push_back(MakeNode(6, "Event.Send", Json::object({{"Name", "Opened"}})));
		document.Graph.Nodes.push_back(MakeNode(7, "Event.Custom", Json::object({{"Name", "Opened"}})));
		document.Graph.Links.push_back(MakeLink(1, "Then", 3, "Execute"));
		return document;
	}

	// A door: the public float Health (1), the private Code (2), and the custom events Hit (3, Damage: float) and Opened (4).
	VisualScriptDocument MakeDoorDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Health", "float", 10.0));
		document.Variables.push_back(MakeVariable(2, "Code", "int", 0, false));
		document.Graph.Nodes.push_back(MakeNode(
			3, "Event.Custom",
			Json::object({{"Name", "Hit"}, {"Parameters", Json::array({Json::object({{"Name", "Damage"}, {"Type", "float"}})})}})));
		document.Graph.Nodes.push_back(MakeNode(4, "Event.Custom", Json::object({{"Name", "Opened"}})));
		return document;
	}
}

TEST_CASE("GraphCommands: graph.create makes function libraries, which graph.get returns")
{
	GraphFixture fixture;
	Json const created = fixture.Ok("graph.create", Json::object({{"path", "Libraries/Motion.sgraph"}, {"template", "Library"}}));
	CHECK(created["path"] == "Libraries/Motion.sgraph");
	VisualScriptDocument const library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	CHECK(fixture.ReadFile("Libraries/Motion.sgraph") == VisualScriptSerializer::WriteText(library));
	Json const read = fixture.Ok("graph.get", Json::object({{"graph", created["asset"]}}));
	CHECK(read["document"] == VisualScriptSerializer::SerializeDocument(library));
	CHECK(read["document"]["Kind"] == "Library");
	CHECK_FALSE(fixture.Context.GetHistory().CanUndo());
}

TEST_CASE("GraphCommands: graph.interface describes what a graph offers others, what it uses and who uses it")
{
	GraphFixture fixture;
	std::string const library = fixture.CreateGraph("Libraries/Motion.sgraph", LoadLibraryDocument());
	std::string const door = fixture.CreateGraph("Scripts/Door.sgraph", MakeDoorDocument());
	std::string const caller = fixture.CreateGraph("Scripts/Caller.sgraph", MakeCallerDocument(library, door));
	fixture.WriteFile("Scripts/Broken.sgraph", "{ \"Strada\": ");
	fixture.Ok("asset.refresh");

	Json const motion = fixture.Ok("graph.interface", Json::object({{"graph", "asset://Libraries/Motion.sgraph"}}));
	CHECK(motion["asset"] == library);
	CHECK(motion["path"] == "Libraries/Motion.sgraph");
	CHECK(motion["kind"] == "Library");
	CHECK(motion["variables"] == Json::array());
	CHECK(motion["events"] == Json::array());
	REQUIRE(motion["functions"].size() == 3);
	Json const valueInput = Json::object({{"name", "Value"}, {"type", "float"}, {"formerNames", Json::array({"X"})}});
	CHECK(motion["functions"][0] == Json::object({{"id", 1},
	                                              {"name", "Clamp01"},
	                                              {"pure", true},
	                                              {"inputs", Json::array({valueInput})},
	                                              {"outputs", Json::array({Json::object({{"name", "Result"}, {"type", "float"}})})}}));
	CHECK(motion["functions"][2]["name"] == "Pause");
	CHECK(motion["functions"][2]["inputs"] == Json::array({Json::object({{"name", "Seconds"}, {"type", "float"}})}));
	// latent needs the runtime.
	CHECK_FALSE(motion["functions"][2].contains("latent"));
	CHECK(motion["analyzed"] == false);
	// A library calling its own functions uses no other graph; the caller uses it (two nodes); the broken file is skipped.
	CHECK(motion["uses"] == Json::array());
	CHECK(motion["usedBy"] == Json::array({Json::object({{"asset", caller}, {"path", "Scripts/Caller.sgraph"}, {"nodes", 2}})}));
	REQUIRE(motion["skipped"].size() == 1);
	CHECK(motion["skipped"][0]["path"] == "Scripts/Broken.sgraph");
	CHECK(motion["skipped"][0]["error"].get<std::string>().find("failed to load 'Scripts/Broken.sgraph'") != std::string::npos);

	Json const doorInterface = fixture.Ok("graph.interface", Json::object({{"graph", door}}));
	CHECK(doorInterface["kind"] == "Script");
	CHECK(doorInterface["variables"] == Json::array({Json::object({{"id", 1}, {"name", "Health"}, {"type", "float"}, {"public", true}}),
	                                                 Json::object({{"id", 2}, {"name", "Code"}, {"type", "int"}, {"public", false}})}));
	Json const hit = Json::object({{"node", 3},
	                               {"name", "Hit"},
	                               {"parameters", Json::array({Json::object({{"name", "Damage"}, {"type", "float"}})})},
	                               {"enabled", "Enabled"}});
	Json const opened = Json::object({{"node", 4}, {"name", "Opened"}, {"parameters", Json::array()}, {"enabled", "Enabled"}});
	CHECK(doorInterface["events"] == Json::array({hit, opened}));
	CHECK(doorInterface["functions"] == Json::array());
	CHECK(doorInterface["usedBy"] == Json::array({Json::object({{"asset", caller}, {"path", "Scripts/Caller.sgraph"}, {"nodes", 2}})}));

	// What the caller uses, ascending by handle, each with its strongest use.
	Json const callerInterface = fixture.Ok("graph.interface", Json::object({{"graph", caller}}));
	REQUIRE(callerInterface["uses"].size() == 2);
	for (Json const& use : callerInterface["uses"])
	{
		CAPTURE(use);
		CHECK(use["use"] == (use["asset"] == library ? "Library" : "Interface"));
		CHECK(use["path"] == (use["asset"] == library ? "Libraries/Motion.sgraph" : "Scripts/Door.sgraph"));
		CHECK(use["nodes"] == 2);
	}
	CHECK(callerInterface["usedBy"] == Json::array());
	CHECK(callerInterface["events"].size() == 1);

	// Errors: not registered, not a visual script, a file that does not load.
	CHECK(fixture.Fails("graph.interface", Json::object({{"graph", "123"}})).Code == AutomationErrorCode::AssetNotFound);
	std::string const material =
		fixture.Ok("material.create", Json::object({{"path", "Materials/Painted.smat"}}))["material"]["id"].get<std::string>();
	CommandError const notAGraph = fixture.Fails("graph.interface", Json::object({{"graph", material}}));
	CHECK(notAGraph.Code == AutomationErrorCode::InvalidParams);
	CHECK(notAGraph.Data["path"] == "graph");
	CHECK(fixture.Fails("graph.interface", Json::object({{"graph", "asset://Scripts/Broken.sgraph"}})).Code ==
	      AutomationErrorCode::FileError);
}

TEST_CASE("GraphCommands: graph.interface with the scripting runtime says which library functions wait")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	std::string const library = fixture.CreateGraph("Libraries/Motion.sgraph", LoadLibraryDocument());
	Json const motion = fixture.Ok("graph.interface", Json::object({{"graph", library}}));
	CHECK(motion["analyzed"] == true);
	CHECK(motion["functions"][0]["latent"] == false);
	CHECK(motion["functions"][1]["latent"] == false);
	CHECK(motion["functions"][2]["latent"] == true);
}

TEST_CASE("GraphCommands: graph.get, graph.set and graph.check analyze a document that names its own graph against itself")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	std::string const graph = fixture.Ok("graph.create", Json::object({{"path", "Scripts/Door.sgraph"}}))["asset"].get<std::string>();
	// Health of another entity running this script: the stored document (the Events template) has no such variable.
	VisualScriptDocument document = MakeDoorDocument();
	document.Graph.Nodes.push_back(MakeNode(10, "Variable.Get", Json::object({{"Variable", 1}, {"Graph", graph}})));
	Json const content = VisualScriptSerializer::SerializeDocument(document);
	auto const remoteGet = [](Json const& result) -> Json
	{
		REQUIRE(result["analyzed"] == true);
		return FindAnalyzedNode(result["nodes"], 10);
	};

	// graph.check: without graph, the stored document is read (it has no Health); with it, the document itself.
	Json const unnamed = fixture.Ok("graph.check", Json::object({{"document", content}}));
	CHECK(remoteGet(unnamed)["Title"] == "Get Variable");
	Json const named = fixture.Ok("graph.check", Json::object({{"document", content}, {"graph", graph}}));
	CHECK(remoteGet(named)["Title"] == "Get Health");
	CHECK(FindDiagnostics(named, "UnknownVariable").empty());

	// graph.set analyzes what it writes against itself; graph.get likewise: an entity running this script.
	Json const set = fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", content}}));
	CHECK(remoteGet(set)["Title"] == "Get Health");
	Json const read = fixture.Ok("graph.get", Json::object({{"graph", graph}}));
	CHECK(remoteGet(read)["Title"] == "Get Health");
	CHECK(remoteGet(read)["Subtitle"].get<std::string>().starts_with("of another"));
}

TEST_CASE("GraphCommands: graph.edit and graph.set report the nodes of other scripts an edit breaks, and can refuse them")
{
	GraphFixture fixture;
	std::string const library = fixture.CreateGraph("Libraries/Motion.sgraph", LoadLibraryDocument());
	std::string const door = fixture.CreateGraph("Scripts/Door.sgraph", MakeDoorDocument());
	std::string const caller = fixture.CreateGraph("Scripts/Caller.sgraph", MakeCallerDocument(library, door));
	std::string const libraryFile = fixture.ReadFile("Libraries/Motion.sgraph");
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();

	// Deleting a function a script calls: its node is Removed, and refuseBrokenReferrers writes nothing.
	Json const deleteClamp = Json::array({Json::object({{"op", "DeleteFunction"}, {"function", "Clamp01"}})});
	CommandError const refused =
		fixture.Fails("graph.edit", Json::object({{"graph", library}, {"operations", deleteClamp}, {"refuseBrokenReferrers", true}}));
	CHECK(refused.Code == AutomationErrorCode::InvalidOperation);
	CHECK(refused.Message.find("Clamp01 (node 2) calls Clamp01, which is gone") != std::string::npos);
	Json const removedNode = Json::object(
		{{"id", 2}, {"type", "Function.Call"}, {"problem", "Removed"}, {"message", "Clamp01 (node 2) calls Clamp01, which is gone"}});
	Json const expected =
		Json::array({Json::object({{"asset", caller}, {"path", "Scripts/Caller.sgraph"}, {"nodes", Json::array({removedNode})}})});
	CHECK(refused.Data["referrers"] == expected);
	CHECK(fixture.ReadFile("Libraries/Motion.sgraph") == libraryFile);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
	// Without it the edit is made, saying what it broke.
	Json const deleted = fixture.Ok("graph.edit", Json::object({{"graph", library}, {"operations", deleteClamp}}));
	CHECK(deleted["changed"] == true);
	CHECK(deleted["referrers"] == expected);
	CHECK(deleted["summary"] == Json::object({{"brokenReferrers", 1}, {"staleSenders", 0}}));
	fixture.Ok("editor.undo");

	// Renaming a custom event: the other script's untyped sender is stale, which refuseBrokenReferrers never refuses.
	Json const rename = Json::array({Json::object({{"op", "RenameCustomEvent"}, {"node", 4}, {"name", "Shut"}})});
	Json const renamed = fixture.Ok("graph.edit", Json::object({{"graph", door}, {"operations", rename}, {"dryRun", true}}));
	REQUIRE(renamed["referrers"].size() == 1);
	Json const staleNode = Json::object({{"id", 6},
	                                     {"type", "Event.Send"},
	                                     {"problem", "Stale"},
	                                     {"message", "Send Opened (node 6) sends Opened by name, which the edited graph no longer has: "
	                                                 "it may have reached that graph and now never does"}});
	CHECK(renamed["referrers"][0]["nodes"] == Json::array({staleNode}));
	CHECK(renamed["summary"] == Json::object({{"brokenReferrers", 0}, {"staleSenders", 1}}));
	CHECK(fixture.Ok("graph.edit", Json::object({{"graph", door}, {"operations", rename}, {"refuseBrokenReferrers", true}}))["changed"] ==
	      true);
	fixture.Ok("editor.undo");

	// graph.set alike: the door's Health made private.
	VisualScriptDocument hidden = MakeDoorDocument();
	hidden.Variables[0].Public = false;
	Json const content = VisualScriptSerializer::SerializeDocument(hidden);
	CommandError const setRefused =
		fixture.Fails("graph.set", Json::object({{"graph", door}, {"document", content}, {"refuseBrokenReferrers", true}}));
	CHECK(setRefused.Code == AutomationErrorCode::InvalidOperation);
	REQUIRE(setRefused.Data["referrers"].size() == 1);
	CHECK(setRefused.Data["referrers"][0]["nodes"][0]["problem"] == "Private");
	CHECK(fixture.GetDocument(door) == MakeDoorDocument());
	Json const set = fixture.Ok("graph.set", Json::object({{"graph", door}, {"document", content}}));
	CHECK(set["summary"] == Json::object({{"brokenReferrers", 1}, {"staleSenders", 0}}));
	CHECK(set["referrers"][0]["nodes"][0]["message"] == "Get Health (node 4) reads Health, which is private now");

	// An edit that keeps what a graph offers reads no other script: the caller stays unloaded.
	Result<AssetHandle> const callerHandle = AssetManager::ResolveReference(caller);
	REQUIRE(callerHandle.IsOk());
	AssetManager::UnloadAsset(callerHandle.GetValue());
	REQUIRE_FALSE(AssetManager::IsLoaded(callerHandle.GetValue()));
	Json const description = Json::array({Json::object({{"op", "SetDescription"}, {"description", "Moves things"}})});
	Json const described = fixture.Ok("graph.edit", Json::object({{"graph", library}, {"operations", description}}));
	CHECK(described["changed"] == true);
	CHECK(described["referrers"] == Json::array());
	CHECK(described["summary"] == Json::object({{"brokenReferrers", 0}, {"staleSenders", 0}}));
	Json const kept = fixture.Ok("graph.set", Json::object({{"graph", door}, {"document", content}}));
	CHECK(kept["referrers"] == Json::array());
	CHECK_FALSE(AssetManager::IsLoaded(callerHandle.GetValue()));
}

TEST_CASE("GraphCommands: graph.edit's kept analysis is dropped when a library the graph calls changes its program")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	std::string const library = fixture.CreateGraph("Libraries/Motion.sgraph", LoadLibraryDocument());
	VisualScriptDocument callerDocument;
	callerDocument.Variables.push_back(MakeVariable(1, "Speed", "int", 1));
	callerDocument.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
	callerDocument.Graph.Nodes.push_back(MakeNode(11, "Function.Call", Json::object({{"Function", 2}, {"Graph", library}})));
	callerDocument.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	std::string const caller = fixture.CreateGraph("Scripts/Caller.sgraph", callerDocument);
	auto const setSpeed = [&](int32_t speed)
	{
		Json const operations = Json::array({Json::object({{"op", "SetVariable"}, {"variable", "Speed"}, {"value", speed}})});
		return fixture.Ok("graph.edit", Json::object({{"graph", caller}, {"operations", operations}}))["analyses"].get<uint32_t>();
	};
	CHECK(setSpeed(2) == 2);
	CHECK(setSpeed(3) == 1);
	// A change of the library's description: what the caller read of it is the same program, the kept analysis stays.
	Json const description = Json::array({Json::object({{"op", "SetDescription"}, {"description", "x"}})});
	fixture.Ok("graph.edit", Json::object({{"graph", library}, {"operations", description}}));
	CHECK(setSpeed(4) == 1);
	// A change of its program: analyzed again, so New is against the library as it is now.
	VisualScriptDocument changed = LoadLibraryDocument();
	changed.Functions[1].Inputs.push_back({"Level", "int"});
	fixture.Ok("graph.set", Json::object({{"graph", library}, {"document", VisualScriptSerializer::SerializeDocument(changed)}}));
	CHECK(setSpeed(5) == 2);
	CHECK(setSpeed(6) == 1);
}

TEST_CASE("GraphCommands: graph.edit's kept analysis is dropped when another graph's events change, never for a typed sender's material")
{
	Testing::ScriptRuntimeScope runtime;
	GraphFixture fixture;
	std::string const door = fixture.CreateGraph("Scripts/Door.sgraph", MakeDoorDocument());
	// A material named by a typed Send by mistake: the engine reads it as absent, and nothing loads it to find that out.
	Result<AssetHandle> const material = fixture.Operations.CreateMaterial("Materials/Paint.smat");
	REQUIRE(material.IsOk());
	AssetManager::UnloadAsset(material.GetValue());
	REQUIRE_FALSE(AssetManager::IsLoaded(material.GetValue()));
	VisualScriptDocument callerDocument;
	callerDocument.Variables.push_back(MakeVariable(1, "Speed", "int", 1));
	callerDocument.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
	callerDocument.Graph.Nodes.push_back(MakeNode(11, "Event.Send",
	                                              Json::object({{"Name", "Hit"}, {"Parameters", Json::array()}, {"Graph", door}}),
	                                              Json::object({{"Damage", 2.0}})));
	callerDocument.Graph.Nodes.push_back(MakeNode(
		12, "Event.Send", Json::object({{"Name", "Hit"}, {"Parameters", Json::array()}, {"Graph", material.GetValue().ToString()}})));
	callerDocument.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	std::string const caller = fixture.CreateGraph("Scripts/Caller.sgraph", callerDocument);
	auto const setSpeed = [&](int32_t speed)
	{
		Json const operations = Json::array({Json::object({{"op", "SetVariable"}, {"variable", "Speed"}, {"value", speed}})});
		return fixture.Ok("graph.edit", Json::object({{"graph", caller}, {"operations", operations}}))["analyses"].get<uint32_t>();
	};
	CHECK(setSpeed(2) == 2);
	CHECK(setSpeed(3) == 1);
	CHECK_FALSE(AssetManager::IsLoaded(material.GetValue()));

	// The door's layout changes: what the caller read of it (its interface) is the same, the kept analysis stays.
	Json const description = Json::array({Json::object({{"op", "SetDescription"}, {"description", "A door"}})});
	fixture.Ok("graph.edit", Json::object({{"graph", door}, {"operations", description}}));
	CHECK(setSpeed(4) == 1);
	// The parameters of its event Hit change: analyzed again, against the door as it is now.
	VisualScriptDocument changed = MakeDoorDocument();
	changed.Graph.Nodes[0].Settings["Parameters"] = Json::array({Json::object({{"Name", "Damage"}, {"Type", "int"}})});
	fixture.Ok("graph.set", Json::object({{"graph", door}, {"document", VisualScriptSerializer::SerializeDocument(changed)}}));
	CHECK(setSpeed(5) == 2);
	CHECK(setSpeed(6) == 1);
	CHECK_FALSE(AssetManager::IsLoaded(material.GetValue()));
}

TEST_CASE("GraphCommands: graph.set replacing a file that does not load reports no referrers, so refuseBrokenReferrers repairs it")
{
	GraphFixture fixture;
	std::string const broken = fixture.CreateGraph("Scripts/Motion.sgraph", MakeDoorDocument());
	VisualScriptDocument runner;
	runner.Graph.Nodes.push_back(MakeNode(1, "RunsGraph", Json::object({{"Graph", broken}})));
	std::string const caller = fixture.CreateGraph("Scripts/Caller.sgraph", runner);
	fixture.WriteFile("Scripts/Motion.sgraph", "{ \"Strada\": ");
	fixture.Ok("asset.refresh");
	Result<AssetHandle> const handle = AssetManager::ResolveReference(broken);
	REQUIRE(handle.IsOk());
	REQUIRE(AssetManager::TryGetAsset<VisualScriptAsset>(handle.GetValue()).IsError());

	// Replaced by a library: the caller's RunsGraph was broken while the file did not load, and the replacement is no change
	// of what the graph offered (it offered nothing), so nothing is reported and nothing refused.
	Json const document = VisualScriptSerializer::SerializeDocument(LoadLibraryDocument());
	Json const set = fixture.Ok("graph.set", Json::object({{"graph", broken}, {"document", document}, {"refuseBrokenReferrers", true}}));
	CHECK(set["changed"] == true);
	CHECK(set["referrers"] == Json::array());
	CHECK(set["summary"] == Json::object({{"brokenReferrers", 0}, {"staleSenders", 0}}));
	CHECK(fixture.GetDocument(broken).Kind == VisualScriptDocumentKind::Library);
	CHECK(fixture.GetDocument(caller) == runner);
}
