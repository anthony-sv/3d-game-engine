#include "Fuzzing.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/GraphDebugCommands.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/PlayMode.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/VisualScriptDebugger.h"

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace Strada;

// The visual-script debug commands over the editor's debug session, play mode and the scripting runtime (spec-slice4
// section 17): every command, parameter and error, the documented agent session (Agents/Debugging.json) and random debug
// sessions (Agents/DebugFuzz.json).
namespace
{
	constexpr float FrameTime = 1.0f / 60.0f;

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value, bool isPublic = true)
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

	// Adds one to the variable at the Set node (its Add and Get nodes take the next two IDs).
	void AddIncrement(VisualScriptGraph& graph, uint32_t set, uint32_t variable)
	{
		graph.Nodes.push_back(MakeNode(set, "Variable.Set", Json::object({{"Variable", variable}})));
		graph.Nodes.push_back(MakeNode(set + 1, "Operator.Add", Json::object(), Json::object({{"B", 1}})));
		graph.Nodes.push_back(MakeNode(set + 2, "Variable.Get", Json::object({{"Variable", variable}})));
		graph.Links.push_back(MakeLink(set + 2, "Value", set + 1, "A"));
		graph.Links.push_back(MakeLink(set + 1, "Result", set, "Value"));
	}

	// On every update: Count += 1 at node 11 (Add 12, Get 13), then Steps += 1 at node 14.
	VisualScriptDocument MakeCounterDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
		document.Variables.push_back(MakeVariable(2, "Steps", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		AddIncrement(document.Graph, 11, 1);
		AddIncrement(document.Graph, 14, 2);
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(11, "Then", 14, "Execute"));
		return document;
	}

	// When the graph starts: A = 1 at node 11, a Delay of 0.1 s at node 12, then B = 1 at node 13.
	VisualScriptDocument MakeDelayDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "A", "int", 0));
		document.Variables.push_back(MakeVariable(2, "B", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}}), Json::object({{"Value", 1}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Flow.Delay", Json::object(), Json::object({{"Duration", 0.1}})));
		document.Graph.Nodes.push_back(MakeNode(13, "Variable.Set", Json::object({{"Variable", 2}}), Json::object({{"Value", 1}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(11, "Then", 12, "Execute"));
		document.Graph.Links.push_back(MakeLink(12, "Completed", 13, "Execute"));
		return document;
	}

	// On every update: Count = value at node 11.
	VisualScriptDocument MakeLiteralDocument(int32_t value)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}}), Json::object({{"Value", value}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		return document;
	}

	// On every update, a chain of increments of Count (each a Set, an Add and a Get), then Done = true: 3 × increments + 2 nodes.
	// The increments' Sets are nodes 3, 6, 9, ... (variables take IDs 1 and 2); the event and Done's Set take the last two IDs.
	VisualScriptDocument MakeChainDocument(uint32_t increments)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
		document.Variables.push_back(MakeVariable(2, "Done", "bool", false));
		uint32_t const event = 3 * increments + 3;
		document.Graph.Nodes.push_back(MakeNode(event, "Event.Update"));
		uint32_t previous = event;
		for (uint32_t index = 0; index < increments; index++)
		{
			uint32_t const set = 3 + index * 3;
			AddIncrement(document.Graph, set, 1);
			document.Graph.Links.push_back(MakeLink(previous, "Then", set, "Execute"));
			previous = set;
		}
		uint32_t const done = event + 1;
		document.Graph.Nodes.push_back(MakeNode(done, "Variable.Set", Json::object({{"Variable", 2}}), Json::object({{"Value", true}})));
		document.Graph.Links.push_back(MakeLink(previous, "Then", done, "Execute"));
		return document;
	}

	// A library whose Twice(Value) answers 2 × Value (pure): node 21 is its Entry, 22 the multiply, 23 its Return.
	VisualScriptDocument MakeLibraryDocument()
	{
		VisualScriptDocument document;
		document.Kind = VisualScriptDocumentKind::Library;
		VisualScriptFunction function;
		function.ID = 20;
		function.Name = "Twice";
		function.Pure = true;
		function.Inputs.push_back({"Value", "int", {}});
		function.Outputs.push_back({"Result", "int", {}});
		function.Graph.Nodes.push_back(MakeNode(21, "Function.Entry"));
		function.Graph.Nodes.push_back(MakeNode(22, "Operator.Multiply", Json::object(), Json::object({{"B", 2}})));
		function.Graph.Nodes.push_back(MakeNode(23, "Function.Return"));
		function.Graph.Links.push_back(MakeLink(21, "Value", 22, "A"));
		function.Graph.Links.push_back(MakeLink(22, "Result", 23, "Result"));
		document.Functions.push_back(std::move(function));
		return document;
	}

	Json LoadAgentFile(std::string_view name)
	{
		Result<std::string> const text = FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" /
		                                                          "Agents" / FileSystem::PathFromUtf8(name));
		REQUIRE_MESSAGE(text.IsOk(), (text ? std::string() : text.GetError()));
		Json parsed = Json::parse(text.GetValue(), nullptr, false);
		REQUIRE_FALSE(parsed.is_discarded());
		return parsed;
	}

	// Where actual departs from expect: objects member by member, arrays whose elements match elements of actual in order,
	// numbers within 1e-4, anything else equal. The path of the first mismatch, or none.
	std::optional<std::string> FindMismatch(Json const& expect, Json const& actual, std::string const& path)
	{
		if (expect.is_object())
		{
			if (!actual.is_object())
			{
				return path + ": expected an object, got " + actual.dump();
			}
			for (auto const& [key, value] : expect.items())
			{
				auto const member = actual.find(key);
				if (member == actual.end())
				{
					return path + "." + key + ": missing";
				}
				if (std::optional<std::string> mismatch = FindMismatch(value, *member, path + "." + key))
				{
					return mismatch;
				}
			}
			return std::nullopt;
		}
		if (expect.is_array())
		{
			if (!actual.is_array())
			{
				return path + ": expected an array, got " + actual.dump();
			}
			size_t next = 0;
			for (size_t index = 0; index < expect.size(); index++)
			{
				while (next < actual.size() && FindMismatch(expect[index], actual[next], path).has_value())
				{
					next++;
				}
				if (next == actual.size())
				{
					return fmt::format("{}[{}]: no later element matches {} in {}", path, index, expect[index].dump(), actual.dump());
				}
				next++;
			}
			return std::nullopt;
		}
		if (expect.is_number() && actual.is_number())
		{
			return std::abs(expect.get<double>() - actual.get<double>()) <= 1.0e-4
			           ? std::nullopt
			           : std::optional<std::string>(path + ": expected " + expect.dump() + ", got " + actual.dump());
		}
		return expect == actual ? std::nullopt
		                        : std::optional<std::string>(path + ": expected " + expect.dump() + ", got " + actual.dump());
	}

	// A project open in an editor model with the commands the editor layer registers for automation (the editor's, play mode's
	// and the debug commands), and the debug session as play mode's observer; headless, so breakpoints are kept in memory.
	// With the scripting runtime unless asked otherwise.
	struct DebugFixture
	{
		std::optional<Testing::ScriptRuntimeScope> Runtime;
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};
		std::optional<VisualScriptDebugSession> Session;
		CommandRegistry Registry;

		explicit DebugFixture(bool withRuntime = true)
		{
			if (withRuntime)
			{
				Runtime.emplace();
			}
			// As the editor layer does: entity fields may name assets by reference.
			Context.SetAssetReferenceResolver(
				[](std::string_view reference)
				{
					return AssetManager::ResolveReference(reference);
				});
			Session.emplace(Context, Play);
			REQUIRE(RegisterEditorCommands(Registry, Operations, EditorCommandEnvironment()).IsOk());
			REQUIRE(RegisterPlayCommands(Registry, Play, Context,
			                             []
			                             {
											 return glm::uvec2(640, 480);
										 })
			            .IsOk());
			REQUIRE(RegisterGraphDebugCommands(Registry, *Session, Play, Context).IsOk());
			REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			Play.SetObserver(&*Session);
		}

		~DebugFixture()
		{
			Play.Stop();
			Play.SetObserver(nullptr);
		}

		DebugFixture(DebugFixture const&) = delete;
		DebugFixture& operator=(DebugFixture const&) = delete;

		CommandResult Run(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE_MESSAGE(result.has_value(), name);
			return std::move(*result);
		}

		Json Ok(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, params);
			REQUIRE_MESSAGE(result.IsOk(), name, ": ", (result.IsError() ? result.GetError().Message : std::string()), " (",
			                params.dump().substr(0, 400), ")");
			return result.TakeValue();
		}

		CommandError Fails(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, params);
			REQUIRE_MESSAGE(result.IsError(), name, " ", params.dump().substr(0, 400), " succeeded");
			return result.TakeError();
		}

		std::string CreateGraph(std::string const& path, VisualScriptDocument const& document)
		{
			std::string const graph = Ok("graph.create", Json::object({{"path", path}}))["asset"].get<std::string>();
			Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(document)}}));
			return graph;
		}

		std::string CreateRunner(std::string name, std::string const& graph)
		{
			Json const created =
				Ok("entity.create", Json::object({{"name", std::move(name)},
			                                      {"components", Json::object({{"VisualScript", Json::object({{"Graph", graph}})}})}}));
			return created["id"].get<std::string>();
		}

		void Start(bool paused = false)
		{
			Json const started = Ok("play.start", Json::object({{"paused", paused}}));
			REQUIRE(started["state"] == "play");
		}

		// An editor frame without automation: the queued resumes, play mode's update, the session's end of the frame.
		void EditorFrame()
		{
			Session->BeginFrame();
			Play.Update(Timestep(FrameTime));
			Session->EndFrame();
		}

		Json Variable(std::string const& entity, std::string const& name)
		{
			Json const read = Ok("entity.get", Json::object({{"entity", entity}}));
			return read["components"]["VisualScript"]["Variables"][name]["Value"];
		}

		Json Breakpoint(std::string const& graph, Json breakpoint)
		{
			Json const set = Ok("graph.breakpoints", Json::object({{"graph", graph}, {"add", Json::array({std::move(breakpoint)})}}));
			return set["breakpoints"];
		}
	};

	// The breakpoint of a node in graph.breakpoints' list.
	Json FindBreakpoint(Json const& breakpoints, uint32_t node)
	{
		for (Json const& breakpoint : breakpoints)
		{
			if (breakpoint["node"] == node)
			{
				return breakpoint;
			}
		}
		FAIL("no breakpoint on node ", node, " in ", breakpoints.dump());
		return Json();
	}
}

TEST_CASE("VisualScriptDebug: every debug command is registered, documented and read-only when it changes nothing")
{
	DebugFixture fixture(false);
	Result<std::string> const reference = FileSystem::ReadTextFile(STRADA_AUTOMATION_DOC_PATH);
	REQUIRE(reference.IsOk());
	for (std::string const name : {"graph.breakpoints", "graph.target", "graph.debug", "graph.continue", "graph.step", "graph.break",
	                               "graph.wait", "graph.trace", "graph.values", "graph.instances", "graph.live"})
	{
		CAPTURE(name);
		CommandDefinition const* command = fixture.Registry.Find(name);
		REQUIRE(command != nullptr);
		bool const readOnly = name == "graph.debug" || name == "graph.values" || name == "graph.instances" || name == "graph.live";
		CHECK(command->ReadOnly == readOnly);
		CHECK(command->IsAsync() == (name == "graph.wait"));
	}
	// Every command the fixture registers (the editor's, play mode's and the debug commands) has its row, so a debug command
	// registered later is held to the reference as soon as it exists.
	for (CommandDefinition const* command : fixture.Registry.GetCommands())
	{
		CAPTURE(command->Name);
		CHECK(reference.GetValue().find("`" + command->Name + "`") != std::string::npos);
	}
	// The play commands' debugging members, and graph.edit's and graph.set's live, are documented with them.
	for (std::string_view const member : {"`stopped`", "`pauseHeld`", "`paused` (default false", "`live` (`{ playState, paused, edits }`"})
	{
		CAPTURE(member);
		CHECK(reference.GetValue().find(member) != std::string::npos);
	}
}

TEST_CASE("VisualScriptDebug: without play, the scripting runtime or a debugger the commands refuse what they cannot do")
{
	DebugFixture fixture(false);
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Counter", graph);

	// What needs no game: the state, breakpoints (unchecked) and targets.
	Json const debug = fixture.Ok("graph.debug");
	CHECK(debug["attached"] == false);
	CHECK(debug["state"] == "Detached");
	CHECK(debug["stops"] == Json::array());
	CHECK(debug["stepping"].is_null());
	Json const set = fixture.Breakpoint(graph, Json::object({{"node", 11}, {"condition", "Count > 2"}}));
	Json const breakpoint = FindBreakpoint(set, 11);
	CHECK(breakpoint["asset"] == graph);
	CHECK(breakpoint["path"] == "Scripts/Counter.sgraph");
	CHECK(breakpoint["type"] == "Variable.Set");
	CHECK(breakpoint["function"] == 0);
	CHECK(breakpoint["checked"] == false);
	CHECK(breakpoint["active"] == false);
	CHECK(breakpoint["inactiveReason"] == "no entity runs it");
	CHECK(breakpoint["error"].is_null());
	// Without the runtime nothing says which nodes no flow enters: the pure Add takes a hit count.
	CHECK(fixture.Breakpoint(graph, Json::object({{"node", 12}, {"hitCount", 2}})).size() == 2);
	CHECK(fixture.Ok("graph.target", Json::object({{"graph", graph}}))["instances"].size() == 1);

	// Parameters: graphs that are not registered or not visual scripts, and members that need a graph.
	CommandError error = fixture.Fails("graph.breakpoints", Json::object({{"graph", "asset://Scripts/Missing.sgraph"}}));
	CHECK(error.Code == AutomationErrorCode::AssetNotFound);
	CHECK(error.Data["path"] == "graph");
	std::string const material =
		fixture.Ok("material.create", Json::object({{"path", "Materials/Paint.smat"}}))["material"]["id"].get<std::string>();
	error = fixture.Fails("graph.breakpoints", Json::object({{"graph", material}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	CHECK(error.Data["path"] == "graph");
	error = fixture.Fails("graph.breakpoints", Json::object({{"remove", Json::array({11})}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	CHECK(error.Data["path"] == "remove");
	error = fixture.Fails("graph.breakpoints", Json::object({{"add", Json::array({Json::object({{"node", 11}})})}}));
	CHECK(error.Data["path"] == "add");
	error = fixture.Fails("graph.trace", Json::object({{"frames", 1}, {"nodes", Json::array({11})}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	CHECK(error.Message == "nodes: node IDs repeat across graphs: pass graphs");
	error = fixture.Fails("graph.trace", Json::object({{"frames", 1}, {"graphs", Json::array({"asset://Nope.sgraph"})}}));
	CHECK(error.Code == AutomationErrorCode::AssetNotFound);
	CHECK(error.Data["path"] == "graphs[0]");

	// Not playing.
	for (auto const& [name, params] : std::vector<std::pair<std::string, Json>>{{"graph.continue", Json::object()},
	                                                                            {"graph.step", Json::object({{"kind", "Over"}})},
	                                                                            {"graph.break", Json::object()},
	                                                                            {"graph.trace", Json::object({{"frames", 1}})},
	                                                                            {"graph.values", Json::object({{"graph", graph}})},
	                                                                            {"graph.instances", Json::object()}})
	{
		CAPTURE(name);
		error = fixture.Fails(name, params);
		CHECK(error.Code == AutomationErrorCode::InvalidOperation);
		CHECK(error.Message == "the editor is not playing (play.start)");
	}
	Json const notPlaying = fixture.Ok("graph.wait", Json::object({{"timeout", 0.1}}));
	CHECK(notPlaying == Json::object({{"stopped", false}, {"stop", nullptr}, {"reason", "NotPlaying"}, {"hint", nullptr}}));

	// Simulating runs no graph.
	fixture.Ok("play.start", Json::object({{"mode", "simulate"}}));
	error = fixture.Fails("graph.continue");
	CHECK(error.Code == AutomationErrorCode::InvalidOperation);
	CHECK(error.Message == "visual scripts do not run while simulating");
	fixture.Ok("play.stop");

	// Playing without the scripting runtime: nothing runs graphs.
	fixture.Start(true);
	for (auto const& [name, params] : std::vector<std::pair<std::string, Json>>{{"graph.continue", Json::object()},
	                                                                            {"graph.break", Json::object()},
	                                                                            {"graph.values", Json::object({{"graph", graph}})},
	                                                                            {"graph.instances", Json::object()}})
	{
		CAPTURE(name);
		CHECK(fixture.Fails(name, params).Code == AutomationErrorCode::Unavailable);
	}
	CHECK(fixture.Ok("graph.debug")["state"] == "Detached");
	// graph.live reads what the engine knows, which without the runtime is nothing.
	Json const live = fixture.Ok("graph.live");
	CHECK(live["playState"] == "play");
	CHECK(live["paused"] == true);
	CHECK(live["serial"] == 0);
	CHECK(live["graphs"] == Json::array());
	CHECK(live["edits"] == Json::array());
	CHECK(live["problems"] == Json::array());
	Json const one = fixture.Ok("graph.live", Json::object({{"graph", graph}}));
	REQUIRE(one["graphs"].size() == 1);
	CHECK(one["graphs"][0]["running"] == "None");
	CHECK(one["graphs"][0]["instances"] == Json::array());
}

TEST_CASE("VisualScriptDebug: graph.breakpoints checks, merges, removes and clears breakpoints and reports what the runtime counts")
{
	DebugFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const idle = fixture.CreateGraph("Scripts/Idle.sgraph", MakeCounterDocument());
	std::string const counter = fixture.CreateRunner("Counter", graph);

	// Refused: a node the stored document does not have, a condition or hit count on a node no flow enters, a condition over
	// the size limit (in bytes) or that does not compile.
	CommandError error =
		fixture.Fails("graph.breakpoints", Json::object({{"graph", graph}, {"add", Json::array({Json::object({{"node", 99}})})}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	CHECK(error.Data["path"] == "add[0].node");
	CHECK(error.Message == "add[0].node: Scripts/Counter.sgraph has no node 99");
	error =
		fixture.Fails("graph.breakpoints",
	                  Json::object({{"graph", graph}, {"add", Json::array({Json::object({{"node", 12}, {"condition", "Count > 1"}})})}}));
	CHECK(error.Data["path"] == "add[0].condition");
	CHECK(error.Message.find("no flow enters") != std::string::npos);
	error =
		fixture.Fails("graph.breakpoints",
	                  Json::object({{"graph", graph},
	                                {"add", Json::array({Json::object({{"node", 11}}), Json::object({{"node", 13}, {"hitCount", 3}})})}}));
	CHECK(error.Data["path"] == "add[1].hitCount");
	// Everything is checked before anything changes: the first addition was not made.
	CHECK(fixture.Ok("graph.breakpoints")["breakpoints"] == Json::array());
	std::string wide;
	for (int index = 0; index < 600; index++)
	{
		wide += "\xC3\xA9";
	}
	error = fixture.Fails("graph.breakpoints",
	                      Json::object({{"graph", graph}, {"add", Json::array({Json::object({{"node", 11}, {"condition", wide}})})}}));
	CHECK(error.Message == "add[0].condition: a condition has at most 1024 bytes");
	error = fixture.Fails(
		"graph.breakpoints",
		Json::object({{"graph", graph}, {"add", Json::array({Json::object({{"node", 11}, {"condition", std::string(1025, 'a')}})})}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);

	// A breakpoint on the pure Add takes neither, but may be set (it stops at the node that reads it: removed for now).
	CHECK(FindBreakpoint(fixture.Breakpoint(graph, Json::object({{"node", 12}})), 12)["title"] == "Add");
	fixture.Ok("graph.breakpoints", Json::object({{"graph", graph}, {"remove", Json::array({12})}}));

	// While nothing runs the graph its conditions are stored unchecked.
	Json breakpoint =
		FindBreakpoint(fixture.Breakpoint(graph, Json::object({{"node", 11}, {"condition", "Count > 100"}, {"hitCount", 2}})), 11);
	CHECK(breakpoint["checked"] == false);
	CHECK(breakpoint["title"] == "Set Count");
	CHECK(breakpoint["enabled"] == true);
	CHECK(breakpoint["hitCount"] == 2);

	fixture.Start(true);
	// The program is loaded: the condition is checked now, and one that does not compile is refused with its suggestion.
	breakpoint = FindBreakpoint(fixture.Ok("graph.breakpoints", Json::object({{"graph", graph}}))["breakpoints"], 11);
	CHECK(breakpoint["checked"] == true);
	CHECK(breakpoint["active"] == true);
	CHECK(breakpoint["inactiveReason"].is_null());
	error =
		fixture.Fails("graph.breakpoints",
	                  Json::object({{"graph", graph}, {"add", Json::array({Json::object({{"node", 14}, {"condition", "count > 1"}})})}}));
	CHECK(error.Data["path"] == "add[0].condition");
	CHECK(error.Message.find("did you mean Count?") != std::string::npos);
	CHECK(error.Message.find("line 1, column 1: ") != std::string::npos);

	// Merging: only the members given change.
	breakpoint = FindBreakpoint(fixture.Breakpoint(graph, Json::object({{"node", 11}, {"enabled", false}})), 11);
	CHECK(breakpoint["enabled"] == false);
	CHECK(breakpoint["condition"] == "Count > 100");
	CHECK(breakpoint["hitCount"] == 2);
	breakpoint = FindBreakpoint(fixture.Breakpoint(graph, Json::object({{"node", 11}, {"enabled", true}, {"hitCount", 0}})), 11);
	CHECK(breakpoint["condition"] == "Count > 100");

	// Reached counts the passes whatever the condition, hits those where it held.
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 5}}))["stopped"] == false);
	breakpoint = FindBreakpoint(fixture.Ok("graph.breakpoints", Json::object({{"graph", graph}}))["breakpoints"], 11);
	CHECK(breakpoint["reached"] == 5);
	CHECK(breakpoint["hits"] == 0);

	// A graph no entity runs, and a target that is gone.
	breakpoint = FindBreakpoint(fixture.Breakpoint(idle, Json::object({{"node", 11}})), 11);
	CHECK(breakpoint["active"] == false);
	CHECK(breakpoint["inactiveReason"] == "no entity runs it");
	fixture.Ok("graph.target", Json::object({{"graph", graph}, {"entity", counter}}));
	fixture.Ok("entity.delete", Json::object({{"entities", Json::array({counter})}}));
	breakpoint = FindBreakpoint(fixture.Ok("graph.breakpoints", Json::object({{"graph", graph}}))["breakpoints"], 11);
	CHECK(breakpoint["active"] == false);
	CHECK(breakpoint["inactiveReason"] == fmt::format("the target {} was destroyed", counter));
	CHECK(FindBreakpoint(fixture.Ok("graph.debug")["breakpoints"], 11)["inactiveReason"] == breakpoint["inactiveReason"]);
	fixture.Ok("editor.undo");

	// Listing every graph's, by path; remove and clear.
	fixture.Breakpoint(graph, Json::object({{"node", 12}, {"enabled", false}}));
	Json const all = fixture.Ok("graph.breakpoints")["breakpoints"];
	REQUIRE(all.size() == 3);
	CHECK(all[0]["path"] == "Scripts/Counter.sgraph");
	CHECK(all[2]["path"] == "Scripts/Idle.sgraph");
	CHECK(fixture.Ok("graph.breakpoints", Json::object({{"graph", graph}, {"remove", Json::array({12, 99})}}))["breakpoints"].size() == 1);
	CHECK(fixture.Ok("graph.breakpoints", Json::object({{"graph", idle}, {"clear", true}}))["breakpoints"] == Json::array());
	CHECK(fixture.Ok("graph.breakpoints")["breakpoints"].size() == 1);
	Json const cleared = fixture.Ok("graph.breakpoints", Json::object({{"clear", true}, {"breakOnErrors", true}}));
	CHECK(cleared["breakpoints"] == Json::array());
	CHECK(cleared["breakOnErrors"] == true);
	CHECK(fixture.Ok("graph.debug")["breakOnErrors"] == true);
	CHECK(fixture.Ok("graph.breakpoints", Json::object({{"breakOnErrors", false}}))["breakOnErrors"] == false);

	// At most 256 per graph, counted with what the call keeps.
	std::string const chain = fixture.CreateGraph("Scripts/Chain.sgraph", MakeChainDocument(100));
	Json many = Json::array();
	for (uint32_t index = 0; index < 100; index++)
	{
		for (uint32_t offset = 0; offset < 3; offset++)
		{
			if (many.size() < 256)
			{
				many.push_back(Json::object({{"node", 3 + index * 3 + offset}}));
			}
		}
	}
	CHECK(fixture.Ok("graph.breakpoints", Json::object({{"graph", chain}, {"add", many}}))["breakpoints"].size() == 256);
	error = fixture.Fails("graph.breakpoints", Json::object({{"graph", chain}, {"add", Json::array({Json::object({{"node", 303}})})}}));
	CHECK(error.Data["path"] == "add");
	CHECK(fixture
	          .Ok("graph.breakpoints",
	              Json::object({{"graph", chain}, {"clear", true}, {"add", Json::array({Json::object({{"node", 303}})})}}))["breakpoints"]
	          .size() == 1);
}

TEST_CASE("VisualScriptDebug: graph.debug lists the stops, the targets and the breakpoints that are inactive or have an error")
{
	DebugFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const library = fixture.CreateGraph("Scripts/Twice.sgraph", MakeLibraryDocument());
	std::string const first = fixture.CreateRunner("First", graph);
	fixture.CreateRunner("Second", graph);
	// Set while nothing runs, so it is unchecked: it fails to compile once the graph runs.
	fixture.Breakpoint(graph, Json::object({{"node", 14}, {"condition", "Nope > 1"}}));
	fixture.Breakpoint(graph, Json::object({{"node", 11}}));
	fixture.Breakpoint(library, Json::object({{"node", 22}}));
	fixture.Ok("graph.target", Json::object({{"graph", graph}, {"entity", first}}));

	fixture.Start(true);
	Json const advanced = fixture.Ok("play.advance", Json::object({{"frames", 3}}));
	CHECK(advanced["framesRun"] == 1);
	CHECK(advanced["stopped"] == true);
	Json const debug = fixture.Ok("graph.debug");
	CHECK(debug["attached"] == true);
	CHECK(debug["state"] == "Stopped");
	CHECK(debug["pauseHeld"] == false);
	REQUIRE(debug["stops"].size() == 1);
	Json const& stop = debug["stops"][0];
	CHECK(debug["current"] == stop["id"]);
	CHECK(stop["reason"] == "Breakpoint");
	CHECK(stop["resumable"] == true);
	CHECK(stop["entity"] == first);
	CHECK(stop["name"] == "First");
	CHECK(stop["graph"] == graph);
	CHECK(stop["path"] == "Scripts/Counter.sgraph");
	CHECK(stop["place"] == "Enter");
	CHECK(stop["breakpoint"] == Json::object({{"asset", graph}, {"path", "Scripts/Counter.sgraph"}, {"node", 11}}));
	REQUIRE(stop["frames"].size() == 1);
	CHECK(stop["frames"][0]["kind"] == "Graph");
	CHECK(stop["frames"][0]["node"] == 11);
	CHECK(stop["frames"][0]["title"] == "Set Count");
	CHECK(stop["frames"][0]["path"] == "Scripts/Counter.sgraph");
	CHECK(stop["start"]["kind"] == "Event");
	CHECK(stop["start"]["node"] == 10);
	CHECK(stop["start"]["origin"].is_null());
	CHECK(debug["targets"] == Json::array({Json::object({{"asset", graph}, {"path", "Scripts/Counter.sgraph"}, {"entity", first}})}));
	CHECK(advanced["stop"] == stop);
	CHECK(fixture.Ok("play.state")["debug"] ==
	      Json::object({{"state", "Stopped"}, {"stops", 1}, {"current", stop["id"]}, {"pauseHeld", false}}));

	// The breakpoints listed: the condition that does not compile, and the library's, which no running graph calls.
	Json const& breakpoints = debug["breakpoints"];
	REQUIRE(breakpoints.size() == 2);
	Json const broken = FindBreakpoint(breakpoints, 14);
	CHECK(broken["active"] == true);
	CHECK(broken["error"].get<std::string>().find("Nope") != std::string::npos);
	Json const unused = FindBreakpoint(breakpoints, 22);
	CHECK(unused["active"] == false);
	CHECK(unused["inactiveReason"] == "no running graph calls this library");
	CHECK(unused["error"].is_null());
	CHECK(FindBreakpoint(fixture.Ok("graph.breakpoints", Json::object({{"graph", graph}}))["breakpoints"], 14)["checked"] == true);

	fixture.Ok("play.stop");
	Json const stopped = fixture.Ok("graph.debug");
	CHECK(stopped["state"] == "Detached");
	CHECK(stopped["stops"] == Json::array());
}

TEST_CASE("VisualScriptDebug: graph.target reads and sets the instance a graph's breakpoints stop")
{
	DebugFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const library = fixture.CreateGraph("Scripts/Twice.sgraph", MakeLibraryDocument());
	std::string const other = fixture.CreateGraph("Scripts/Other.sgraph", MakeLiteralDocument(1));
	std::string const beta = fixture.CreateRunner("Beta", graph);
	std::string const alpha = fixture.CreateRunner("Alpha", graph);
	std::string const stranger = fixture.CreateRunner("Stranger", other);

	Json target = fixture.Ok("graph.target", Json::object({{"graph", graph}}));
	CHECK(target["asset"] == graph);
	CHECK(target["path"] == "Scripts/Counter.sgraph");
	CHECK(target["entity"].is_null());
	CHECK(target["instances"] ==
	      Json::array({Json::object({{"entity", alpha}, {"name", "Alpha"}}), Json::object({{"entity", beta}, {"name", "Beta"}})}));
	CHECK(fixture.Ok("graph.target", Json::object({{"graph", graph}, {"entity", beta}}))["entity"] == beta);
	CHECK(fixture.Ok("graph.target", Json::object({{"graph", graph}}))["entity"] == beta);
	CHECK(fixture.Ok("graph.target", Json::object({{"graph", graph}, {"any", false}}))["entity"] == beta);
	CHECK(fixture.Ok("graph.target", Json::object({{"graph", graph}, {"any", true}}))["entity"].is_null());

	CommandError error = fixture.Fails("graph.target", Json::object({{"graph", graph}, {"entity", beta}, {"any", true}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	error = fixture.Fails("graph.target", Json::object({{"graph", graph}, {"entity", stranger}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	CHECK(error.Data["path"] == "entity");
	CHECK(error.Message == fmt::format("entity: entity {} does not run Scripts/Counter.sgraph", stranger));
	CHECK(fixture.Fails("graph.target", Json::object({{"graph", graph}, {"entity", "123456789"}})).Code ==
	      AutomationErrorCode::EntityNotFound);
	CHECK(fixture.Fails("graph.target", Json::object({{"graph", graph}, {"entity", "x"}})).Code == AutomationErrorCode::InvalidParams);
	error = fixture.Fails("graph.target", Json::object({{"graph", library}, {"any", true}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	CHECK(error.Message.find("a function library runs on no entity") != std::string::npos);

	// While playing the target filters the stops.
	fixture.Ok("graph.target", Json::object({{"graph", graph}, {"entity", alpha}}));
	fixture.Breakpoint(graph, Json::object({{"node", 11}}));
	fixture.Start(true);
	Json const advanced = fixture.Ok("play.advance", Json::object({{"frames", 1}}));
	CHECK(advanced["stop"]["entity"] == alpha);
	CHECK(fixture.Ok("graph.debug")["stops"].size() == 1);
}

TEST_CASE("VisualScriptDebug: graph.break stops the next node of one graph's executions, and cancel disarms it")
{
	DebugFixture fixture;
	std::string const counter = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const literal = fixture.CreateGraph("Scripts/Literal.sgraph", MakeLiteralDocument(3));
	fixture.CreateRunner("Counter", counter);
	fixture.CreateRunner("Literal", literal);
	CHECK(fixture.Fails("graph.break").Code == AutomationErrorCode::InvalidOperation);

	fixture.Start(true);
	Json state = fixture.Ok("graph.break", Json::object({{"graph", literal}}));
	CHECK(state["state"] == "Running");
	Json advanced = fixture.Ok("play.advance", Json::object({{"frames", 3}}));
	CHECK(advanced["framesRun"] == 1);
	CHECK(advanced["stop"]["reason"] == "Break");
	CHECK(advanced["stop"]["graph"] == literal);
	CHECK(fixture.Ok("graph.continue")["outcome"] == "Ended");

	// A cancel takes no graph: it cancels the armed break and the step, whatever graph they are for.
	CommandError const error = fixture.Fails("graph.break", Json::object({{"cancel", true}, {"graph", literal}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	CHECK(error.Data["path"] == "graph");
	CHECK(error.Message == "graph: cancel takes no graph: it cancels the armed break and the step");

	// Cancelled before a frame ran, it stops nothing.
	fixture.Ok("graph.break");
	state = fixture.Ok("graph.break", Json::object({{"cancel", true}}));
	CHECK(state["stops"] == Json::array());
	advanced = fixture.Ok("play.advance", Json::object({{"frames", 3}}));
	CHECK(advanced["framesRun"] == 3);
	CHECK(advanced["stopped"] == false);
	CHECK(advanced["stop"].is_null());
}

TEST_CASE("VisualScriptDebug: graph.continue continues one stop or every stop, runs to a node, arms, and resumes a held pause")
{
	DebugFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const first = fixture.CreateRunner("First", graph);
	fixture.CreateRunner("Second", graph);

	// Nothing is stopped and nothing is held.
	fixture.Start();
	CommandError error = fixture.Fails("graph.continue");
	CHECK(error.Code == AutomationErrorCode::InvalidOperation);
	CHECK(error.Message == "nothing is stopped: no execution to continue (to arms a run to a node)");
	error = fixture.Fails("graph.continue", Json::object({{"to", Json::object({{"graph", graph}, {"node", 99}})}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	CHECK(error.Data["path"] == "to.node");
	error = fixture.Fails("graph.continue", Json::object({{"stop", 1}, {"to", Json::object({{"graph", graph}, {"node", 11}})}}));
	CHECK(error.Data["path"] == "to");

	// With nothing stopped, to only arms; the frame that enters the node stops and pauses the scene: the pause is held.
	Json armed = fixture.Ok("graph.continue", Json::object({{"to", Json::object({{"graph", graph}, {"node", 14}})}}));
	CHECK(armed["outcome"] == "Armed");
	CHECK(armed["stop"].is_null());
	CHECK(armed["paused"] == false);
	fixture.EditorFrame();
	Json debug = fixture.Ok("graph.debug");
	REQUIRE(debug["stops"].size() == 1);
	CHECK(debug["stops"][0]["reason"] == "RunTo");
	CHECK(debug["pauseHeld"] == true);
	CHECK(fixture.Fails("graph.continue", Json::object({{"stop", 999}})).Data["path"] == "stop");

	// Continuing the one stop resumes play, which the debugger had paused.
	uint64_t const runTo = debug["stops"][0]["id"].get<uint64_t>();
	Json continued = fixture.Ok("graph.continue", Json::object({{"stop", runTo}}));
	CHECK(continued["outcome"] == "Ended");
	CHECK(continued["failed"] == false);
	CHECK(continued["paused"] == false);
	CHECK(continued["state"]["state"] == "Running");

	// Two instances stop in one frame; continuing every stop ends both.
	fixture.Breakpoint(graph, Json::object({{"node", 11}}));
	fixture.EditorFrame();
	CHECK(fixture.Ok("graph.debug")["stops"].size() == 2);
	continued = fixture.Ok("graph.continue");
	CHECK(continued["outcome"] == "Ended");
	CHECK(continued["state"]["stops"] == Json::array());
	CHECK(continued["paused"] == false);

	// A stop that ends without a resume (the edit deletes its node) keeps the pause held; graph.continue then resumes play.
	fixture.Ok("graph.target", Json::object({{"graph", graph}, {"entity", first}}));
	fixture.EditorFrame();
	REQUIRE(fixture.Ok("graph.debug")["stops"].size() == 1);
	fixture.Ok(
		"graph.edit",
		Json::object({{"graph", graph}, {"operations", Json::array({Json::object({{"op", "Delete"}, {"elements", Json::array({11})}})})}}));
	debug = fixture.Ok("graph.debug");
	CHECK(debug["stops"] == Json::array());
	CHECK(debug["pauseHeld"] == true);
	CHECK(fixture.Play.IsPaused());
	Json const resumed = fixture.Ok("graph.continue");
	CHECK(resumed["outcome"] == "Resumed");
	CHECK(resumed["paused"] == false);
	CHECK(resumed["state"]["pauseHeld"] == false);
}

TEST_CASE("VisualScriptDebug: input given while an execution is stopped is read by the code the resume runs, then again by the next "
          "frame")
{
	// Input is the application's: what this case presses must not reach the cases after it.
	struct InputGuard
	{
		InputGuard() { Input::Reset(); }
		~InputGuard() { Input::Reset(); }
		InputGuard(InputGuard const&) = delete;
		InputGuard& operator=(InputGuard const&) = delete;
	} const inputGuard;
	DebugFixture fixture;
	// On every update: Ticks += 1 at node 11, then Saw += 1 at node 14 when Space was pressed (the Branch at node 20, its
	// condition read by node 21). Event.KeyPressed (node 30) adds one to Events at node 31.
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "Ticks", "int", 0));
	document.Variables.push_back(MakeVariable(2, "Saw", "int", 0));
	document.Variables.push_back(MakeVariable(3, "Events", "int", 0));
	document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
	AddIncrement(document.Graph, 11, 1);
	document.Graph.Nodes.push_back(MakeNode(20, "Flow.Branch"));
	document.Graph.Nodes.push_back(
		MakeNode(21, "Call:Strada.Input.IsKeyPressed(Strada.KeyCode)", Json::object(), Json::object({{"Key", "Space"}})));
	AddIncrement(document.Graph, 14, 2);
	document.Graph.Nodes.push_back(MakeNode(30, "Event.KeyPressed", Json::object({{"Key", "Space"}})));
	AddIncrement(document.Graph, 31, 3);
	document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	document.Graph.Links.push_back(MakeLink(11, "Then", 20, "Execute"));
	document.Graph.Links.push_back(MakeLink(21, "Result", 20, "Condition"));
	document.Graph.Links.push_back(MakeLink(20, "True", 14, "Execute"));
	document.Graph.Links.push_back(MakeLink(30, "Then", 31, "Execute"));
	std::string const graph = fixture.CreateGraph("Scripts/Keys.sgraph", document);
	std::string const runner = fixture.CreateRunner("Keys", graph);
	auto const counts = [&fixture, &runner]
	{
		return Json::array({fixture.Variable(runner, "Ticks"), fixture.Variable(runner, "Saw"), fixture.Variable(runner, "Events")});
	};

	// The first frame stops before Ticks is counted.
	fixture.Start(true);
	fixture.Breakpoint(graph, Json::object({{"node", 11}}));
	REQUIRE(fixture.Ok("play.advance", Json::object({{"frames", 1}}))["stopped"] == true);

	// Space is pressed while the execution is stopped; the editor's frames run no game frame and keep the press.
	fixture.Ok("input.set", Json::object({{"keys", Json::object({{"Space", true}})}}));
	for (int frame = 0; frame < 3; frame++)
	{
		fixture.EditorFrame();
		Input::EndApplicationFrame();
	}
	CHECK(counts() == Json::array({0, 0, 0}));
	fixture.Ok("graph.breakpoints", Json::object({{"graph", graph}, {"clear", true}}));

	// The resumed code reads the input of that moment (spec-slice4 section 4.12): it sees the press. The frame's key events
	// were dispatched before the press, and the scene stays paused (the debugger did not pause it).
	Json const continued = fixture.Ok("graph.continue");
	CHECK(continued["outcome"] == "Ended");
	CHECK(continued["paused"] == true);
	CHECK(counts() == Json::array({1, 1, 0}));

	// The input frame ends only after a game frame (S4-R15.5): the next frame sees the press again, its key event included.
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 1}}))["framesRun"] == 1);
	CHECK(counts() == Json::array({2, 2, 1}));
	// The frame after it sees the key held, not pressed.
	fixture.EditorFrame();
	Input::EndApplicationFrame();
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 1}}))["framesRun"] == 1);
	CHECK(counts() == Json::array({3, 2, 1}));
	CHECK(Input::IsKeyDown(KeyCode::Space));
}

TEST_CASE("VisualScriptDebug: graph.step steps between frames, waits in a Delay with needsFrames, and arms after the execution ended")
{
	DebugFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Delay.sgraph", MakeDelayDocument());
	std::string const runner = fixture.CreateRunner("Waiter", graph);
	fixture.Breakpoint(graph, Json::object({{"node", 12}}));

	// play.start paused: the Event.Create stops at the Delay before the first frame.
	fixture.Start(true);
	CommandError error = fixture.Fails("graph.step", Json::object({{"kind", "Over"}, {"stop", 99}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	CHECK(error.Data["path"] == "stop");
	Json debug = fixture.Ok("graph.debug");
	REQUIRE(debug["stops"].size() == 1);
	CHECK(debug["stops"][0]["frames"][0]["node"] == 12);
	uint64_t const frame = fixture.Context.GetScene().GetRuntimeFrame();

	// Step Over a Delay waits in it; the scene stays paused, so frames are needed.
	Json stepped = fixture.Ok("graph.step", Json::object({{"kind", "Over"}}));
	CHECK(stepped["outcome"] == "Waiting");
	CHECK(stepped["needsFrames"] == true);
	CHECK(stepped["hint"] == "the scene is paused: play.advance runs frames");
	CHECK(stepped["paused"] == true);
	CHECK(stepped["state"]["state"] == "Stepping");
	CHECK(stepped["state"]["stepping"]["action"] == "Over");
	CHECK(stepped["state"]["stepping"]["needsFrames"] == true);
	CHECK(stepped["state"]["stepping"]["waitsIn"]["node"] == 12);
	CHECK(stepped["state"]["stepping"]["waitsIn"]["entity"] == runner);
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == frame);
	Json const waited = fixture.Ok("graph.wait", Json::object({{"timeout", 5.0}}));
	CHECK(waited ==
	      Json::object(
			  {{"stopped", false}, {"stop", nullptr}, {"reason", "Paused"}, {"hint", "the scene is paused: play.advance runs frames"}}));

	// play.advance stops early at the node after the Delay.
	Json const advanced = fixture.Ok("play.advance", Json::object({{"frames", 60}}));
	CHECK(advanced["framesRun"].get<uint32_t>() < 60);
	CHECK(advanced["stopped"] == true);
	CHECK(advanced["stop"]["reason"] == "Step");
	CHECK(advanced["stop"]["frames"][0]["node"] == 13);
	CHECK(fixture.Variable(runner, "B") == 0);

	// A step that ends the execution: StepEnded; Out has nothing to step out of, Over arms a break at the graph's next node.
	stepped = fixture.Ok("graph.step", Json::object({{"kind", "Over"}}));
	CHECK(stepped["outcome"] == "Ended");
	CHECK(stepped["state"]["state"] == "StepEnded");
	CHECK(stepped["needsFrames"] == false);
	CHECK(stepped["hint"].is_null());
	CHECK(fixture.Variable(runner, "B") == 1);
	error = fixture.Fails("graph.step", Json::object({{"kind", "Out"}}));
	CHECK(error.Code == AutomationErrorCode::InvalidOperation);
	CHECK(error.Message == "the execution ended: nothing to step out of");
	stepped = fixture.Ok("graph.step", Json::object({{"kind", "Into"}}));
	CHECK(stepped["outcome"] == "Armed");
	CHECK(stepped["needsFrames"] == true);
	CHECK(stepped["state"]["state"] == "Running");
	error = fixture.Fails("graph.step", Json::object({{"kind", "Over"}}));
	CHECK(error.Message == "nothing is stopped: no execution to step");

	// A read-only stop is not stepped.
	fixture.Ok("graph.break", Json::object({{"cancel", true}}));
	fixture.Ok("play.stop");
	std::string const failing =
		fixture.CreateGraph("Scripts/Failing.sgraph",
	                        []
	                        {
								VisualScriptDocument document;
								document.Variables.push_back(MakeVariable(1, "Result", "int", 0));
								document.Variables.push_back(MakeVariable(2, "Zero", "int", 0, false));
								document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
								document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}})));
								document.Graph.Nodes.push_back(MakeNode(12, "Operator.Divide", Json::object(), Json::object({{"A", 1}})));
								document.Graph.Nodes.push_back(MakeNode(13, "Variable.Get", Json::object({{"Variable", 2}})));
								document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
								document.Graph.Links.push_back(MakeLink(13, "Value", 12, "B"));
								document.Graph.Links.push_back(MakeLink(12, "Result", 11, "Value"));
								return document;
							}());
	fixture.CreateRunner("Failing", failing);
	fixture.Ok("graph.breakpoints", Json::object({{"clear", true}, {"breakOnErrors", true}}));
	fixture.Start(true);
	Json const failed = fixture.Ok("play.advance", Json::object({{"frames", 1}}));
	REQUIRE(failed["stopped"] == true);
	CHECK(failed["stop"]["reason"] == "Error");
	CHECK(failed["stop"]["resumable"] == false);
	error = fixture.Fails("graph.step", Json::object({{"kind", "Over"}, {"stop", failed["stop"]["id"]}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	CHECK(error.Message.find("is read-only") != std::string::npos);
	CHECK(fixture.Ok("graph.continue")["outcome"] == "Dismissed");
}

TEST_CASE("VisualScriptDebug: graph.wait answers on a stop, at its timeout, when play stops and at once when it can, and Cancelled when "
          "abandoned")
{
	DebugFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Counter", graph);
	auto const wait = [&fixture](double timeout, std::optional<CommandResult>& result)
	{
		fixture.Registry.Execute("graph.wait", Json::object({{"timeout", timeout}}),
		                         [&result](CommandResult value)
		                         {
									 REQUIRE_FALSE(result.has_value());
									 result.emplace(std::move(value));
								 });
	};
	auto const frameUntil = [&fixture](std::optional<CommandResult> const& result)
	{
		auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!result && std::chrono::steady_clock::now() < deadline)
		{
			fixture.EditorFrame();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		REQUIRE(result.has_value());
	};

	fixture.Start();
	// A stop the frames make.
	std::optional<CommandResult> stopped;
	wait(10.0, stopped);
	CHECK_FALSE(stopped.has_value());
	fixture.Breakpoint(graph, Json::object({{"node", 14}}));
	frameUntil(stopped);
	REQUIRE(stopped->IsOk());
	CHECK(stopped->GetValue()["stopped"] == true);
	CHECK(stopped->GetValue()["reason"] == "Stopped");
	CHECK(stopped->GetValue()["stop"]["frames"][0]["node"] == 14);
	CHECK(stopped->GetValue()["hint"].is_null());
	// A stop that exists answers at once.
	CHECK(fixture.Ok("graph.wait")["stop"] == stopped->GetValue()["stop"]);

	// The timeout, while frames run and nothing stops.
	fixture.Ok("graph.breakpoints", Json::object({{"clear", true}}));
	fixture.Ok("graph.continue");
	std::optional<CommandResult> timedOut;
	wait(0.1, timedOut);
	frameUntil(timedOut);
	CHECK(timedOut->GetValue() == Json::object({{"stopped", false}, {"stop", nullptr}, {"reason", "Timeout"}, {"hint", nullptr}}));

	// A paused scene with nothing stopped runs no frame: at once.
	fixture.Ok("play.pause");
	CHECK(fixture.Ok("graph.wait")["reason"] == "Paused");
	fixture.Ok("play.pause", Json::object({{"paused", false}}));

	// Play stops while it waits.
	std::optional<CommandResult> playStopped;
	wait(10.0, playStopped);
	CHECK_FALSE(playStopped.has_value());
	fixture.Ok("play.stop");
	REQUIRE(playStopped.has_value());
	CHECK(playStopped->GetValue()["reason"] == "PlayStopped");

	// The session goes while one waits (the editor closing): the wait is abandoned.
	fixture.Start();
	std::optional<CommandResult> abandoned;
	wait(10.0, abandoned);
	CHECK_FALSE(abandoned.has_value());
	fixture.Play.SetObserver(nullptr);
	fixture.Session.reset();
	REQUIRE(abandoned.has_value());
	REQUIRE(abandoned->IsError());
	CHECK(abandoned->GetError().Code == AutomationErrorCode::Cancelled);
	fixture.Play.Stop();
}

TEST_CASE("VisualScriptDebug: graph.trace records frames as a summary or step by step, narrowed by graph, node and entity")
{
	DebugFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const other = fixture.CreateGraph("Scripts/Literal.sgraph", MakeLiteralDocument(7));
	std::string const first = fixture.CreateRunner("First", graph);
	fixture.CreateRunner("Second", graph);
	fixture.CreateRunner("Literal", other);
	fixture.Start(true);

	CHECK(fixture.Fails("graph.trace", Json::object({{"frames", 1}, {"entity", "123456789"}})).Code == AutomationErrorCode::EntityNotFound);

	// The default summary: per graph, its steps and the values of the pins.
	Json traced = fixture.Ok("graph.trace", Json::object({{"frames", 4}}));
	CHECK(traced["framesRun"] == 4);
	CHECK(traced["stopped"] == false);
	CHECK(traced["stop"].is_null());
	CHECK(traced["truncated"] == Json::object({{"steps", 0}, {"values", 0}, {"hint", nullptr}}));
	Json const& graphs = traced["summary"]["graphs"];
	REQUIRE(graphs.size() == 2);
	Json const* counter = nullptr;
	for (Json const& entry : graphs)
	{
		counter = entry["asset"] == graph ? &entry : counter;
	}
	REQUIRE(counter != nullptr);
	CHECK((*counter)["path"] == "Scripts/Counter.sgraph");
	CHECK((*counter)["function"] == 0);
	CHECK_FALSE(
		FindMismatch(Json::array({Json::object(
						 {{"node", 11}, {"title", "Set Count"}, {"exit", "Then"}, {"count", 8}, {"firstFrame", 0}, {"lastFrame", 3}})}),
	                 (*counter)["steps"], "steps")
			.has_value());
	CHECK_FALSE(FindMismatch(Json::array({Json::object({{"node", 11},
	                                                    {"pin", "Value"},
	                                                    {"direction", "In"},
	                                                    {"type", "int"},
	                                                    {"count", 8},
	                                                    {"first", 1},
	                                                    {"last", 4},
	                                                    {"min", 1},
	                                                    {"max", 4}})}),
	                         (*counter)["values"], "values")
	                .has_value());

	// Narrowed: one graph's node of one entity.
	traced = fixture.Ok("graph.trace",
	                    Json::object({{"frames", 2}, {"graphs", Json::array({graph})}, {"nodes", Json::array({14})}, {"entity", first}}));
	REQUIRE(traced["summary"]["graphs"].size() == 1);
	Json const& narrowed = traced["summary"]["graphs"][0];
	REQUIRE(narrowed["steps"].size() == 1);
	CHECK(narrowed["steps"][0]["node"] == 14);
	CHECK(narrowed["steps"][0]["count"] == 2);
	CHECK_FALSE(FindMismatch(Json::array({Json::object({{"node", 14}, {"pin", "Value"}, {"direction", "In"}, {"first", 5}, {"last", 6}})}),
	                         narrowed["values"], "values")
	                .has_value());

	// Step by step, cut at a cap, with the hint that narrows it.
	traced = fixture.Ok("graph.trace", Json::object({{"frames", 1}, {"summary", false}, {"maxSteps", 2}, {"values", false}}));
	CHECK_FALSE(traced.contains("summary"));
	REQUIRE(traced["steps"].size() == 2);
	Json const& step = traced["steps"][0];
	for (char const* const member : {"i", "frame", "execution", "entity", "asset", "path", "function", "node", "title", "exit"})
	{
		CAPTURE(member);
		CHECK(step.contains(member));
	}
	CHECK(traced["values"] == Json::array());
	CHECK(traced["truncated"]["steps"].get<uint32_t>() > 0);
	CHECK(traced["truncated"]["hint"] == "pass graphs and nodes, or entity");
	traced = fixture.Ok("graph.trace", Json::object({{"frames", 1}, {"summary", false}, {"graphs", Json::array({other})}}));
	REQUIRE(traced["steps"].size() == 2);
	CHECK_FALSE(
		FindMismatch(Json::array({Json::object({{"node", 11}, {"title", "Set Count"}, {"exit", "Then"}})}), traced["steps"], "steps")
			.has_value());
	CHECK_FALSE(
		FindMismatch(Json::array({Json::object({{"node", 11}, {"pin", "Value"}, {"direction", "In"}, {"type", "int"}, {"value", 7}})}),
	                 traced["values"], "values")
			.has_value());

	// A breakpoint that stops ends the trace after its frame; then the trace is refused.
	fixture.Breakpoint(graph, Json::object({{"node", 14}, {"hitCount", 3}}));
	traced = fixture.Ok("graph.trace", Json::object({{"frames", 10}, {"graphs", Json::array({graph})}}));
	CHECK(traced["framesRun"] == 2);
	CHECK(traced["stopped"] == true);
	CHECK(traced["stop"]["frames"][0]["node"] == 14);
	CommandError const refused = fixture.Fails("graph.trace", Json::object({{"frames", 1}}));
	CHECK(refused.Code == AutomationErrorCode::InvalidOperation);
	CHECK(refused.Message.find("graph.continue or graph.step it first") != std::string::npos);
}

TEST_CASE("VisualScriptDebug: graph.trace shows a component member's Target input as the entity it was given")
{
	// On every update: the Mover's translation set at node 11, its Target from the Get at node 12.
	DebugFixture fixture;
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "Mover", "Strada.Entity", Json()));
	document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
	document.Graph.Nodes.push_back(
		MakeNode(11, "Set:Strada.TransformComponent.Translation", Json::object(), Json::object({{"Value", Json::array({1.0, 2.0, 3.0})}})));
	document.Graph.Nodes.push_back(MakeNode(12, "Variable.Get", Json::object({{"Variable", 1}})));
	document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	document.Graph.Links.push_back(MakeLink(12, "Value", 11, "Target"));
	std::string const graph = fixture.CreateGraph("Scripts/Mover.sgraph", document);
	std::string const box = fixture.Ok("entity.create", Json::object({{"name", "Box"}}))["id"].get<std::string>();
	fixture.Ok(
		"entity.create",
		Json::object(
			{{"name", "Runner"},
	         {"components",
	          Json::object(
				  {{"VisualScript",
	                Json::object({{"Graph", graph},
	                              {"Variables", Json::object({{"Mover", Json::object({{"Type", "Entity"}, {"Value", box}})}})}})}})}}));
	fixture.Start(true);

	// The interpreter swaps the input for the component it fetched; the value shown is still the entity linked to it.
	Json const traced = fixture.Ok(
		"graph.trace", Json::object({{"frames", 1}, {"summary", false}, {"graphs", Json::array({graph})}, {"nodes", Json::array({11})}}));
	CHECK_FALSE(FindMismatch(Json::array({Json::object({{"node", 11},
	                                                    {"pin", "Target"},
	                                                    {"direction", "In"},
	                                                    {"type", "Strada.Entity"},
	                                                    {"entity", box},
	                                                    {"entityName", "Box"},
	                                                    {"valid", true}})}),
	                         traced["values"], "values")
	                .has_value());
}

TEST_CASE("VisualScriptDebug: a default 600-frame trace of a 200-node graph stays under 64 KB of compact JSON")
{
	DebugFixture fixture;
	// 66 increments: the event, 198 nodes of increments and Done's Set.
	VisualScriptDocument const document = MakeChainDocument(66);
	REQUIRE(document.Graph.Nodes.size() == 200);
	std::string const graph = fixture.CreateGraph("Scripts/Chain.sgraph", document);
	fixture.CreateRunner("Chain", graph);
	fixture.Start(true);
	auto const started = std::chrono::steady_clock::now();
	Json const traced = fixture.Ok("graph.trace", Json::object({{"frames", 600}}));
	double const milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	CHECK(traced["framesRun"] == 600);
	REQUIRE(traced["summary"]["graphs"].size() == 1);
	CHECK(traced["summary"]["graphs"][0]["steps"].size() >= 67);
	size_t const bytes = traced.dump().size();
	MESSAGE("a default 600-frame trace of a 200-node graph: ", bytes, " bytes in ", milliseconds, " ms");
	CHECK(bytes <= 64u * 1024u);
}

TEST_CASE("VisualScriptDebug: graph.step round trip on 2,000 nodes")
{
	DebugFixture fixture;
	// 666 increments: 2,000 nodes.
	VisualScriptDocument const document = MakeChainDocument(666);
	REQUIRE(document.Graph.Nodes.size() == 2000);
	std::string const graph = fixture.CreateGraph("Scripts/Chain.sgraph", document);
	fixture.CreateRunner("Chain", graph);
	fixture.Breakpoint(graph, Json::object({{"node", 3}}));
	fixture.Start(true);
	REQUIRE(fixture.Ok("play.advance", Json::object({{"frames", 1}}))["stopped"] == true);
	std::vector<double> times;
	for (int index = 0; index < 64; index++)
	{
		auto const started = std::chrono::steady_clock::now();
		Json const stepped = fixture.Ok("graph.step", Json::object({{"kind", "Over"}}));
		times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
		REQUIRE(stepped["outcome"] == "Stopped");
		CHECK(stepped["stop"]["frames"][0]["node"] == 6 + index * 3);
	}
	std::sort(times.begin(), times.end());
	double const median = times[times.size() / 2];
	MESSAGE("graph.step round trip on 2,000 nodes: median ", median, " ms, fastest ", times.front(), " ms, slowest ", times.back(), " ms");
#if !defined(ST_DEBUG)
	// Three times the median measured in Release (0.25 ms; spec-slice4 section 21), only where wall-clock bounds are asserted.
	if (Testing::ArePerformanceTestsEnabled())
	{
		CHECK(median <= 3.0 * 0.25);
	}
#endif
}

TEST_CASE("VisualScriptDebug: graph.values reads live instances and stops' frames, evaluates formulas and shows edited inputs")
{
	DebugFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const literal = fixture.CreateGraph("Scripts/Literal.sgraph", MakeLiteralDocument(5));
	std::string const library = fixture.CreateGraph("Scripts/Twice.sgraph", MakeLibraryDocument());
	std::string const first = fixture.CreateRunner("First", graph);
	std::string const second = fixture.CreateRunner("Second", graph);
	std::string const runner = fixture.CreateRunner("Literal", literal);
	fixture.Start(true);
	fixture.Ok("play.advance", Json::object({{"frames", 3}}));

	// Live: the graph's target, else its only instance; several and neither are refused.
	CommandError error = fixture.Fails("graph.values", Json::object({{"graph", graph}}));
	CHECK(error.Code == AutomationErrorCode::InvalidParams);
	CHECK(error.Data["path"] == "entity");
	CHECK(error.Message.find("2 entities run Scripts/Counter.sgraph") != std::string::npos);
	CHECK(fixture.Fails("graph.values", Json::object({{"graph", graph}, {"entity", runner}})).Data["path"] == "entity");
	CHECK(fixture.Fails("graph.values", Json::object({{"graph", graph}, {"entity", first}, {"function", 7}})).Data["path"] == "function");
	CHECK(fixture.Fails("graph.values", Json::object({{"graph", graph}, {"evaluate", Json::array({"Count"})}})).Data["path"] == "evaluate");
	CHECK(fixture.Fails("graph.values", Json::object({{"graph", library}})).Data["path"] == "graph");
	fixture.Ok("graph.target", Json::object({{"graph", graph}, {"entity", second}}));
	Json live = fixture.Ok("graph.values", Json::object({{"graph", graph}, {"nodes", Json::array({12})}}));
	CHECK(live["entity"] == second);
	CHECK(live["name"] == "Second");
	CHECK(live["source"] == "Frame");
	CHECK(live["function"] == 0);
	CHECK_FALSE(FindMismatch(Json::array({Json::object({{"name", "Count"}, {"type", "int"}, {"value", 3}, {"public", true}}),
	                                      Json::object({{"name", "Steps"}, {"value", 3}})}),
	                         live["variables"], "variables")
	                .has_value());
	REQUIRE(live["nodes"].size() == 1);
	CHECK(live["nodes"][0]["node"] == 12);
	CHECK(live["nodes"][0]["title"] == "Add");
	CHECK(live["nodes"][0]["outputs"][0]["name"] == "Result");
	CHECK(live["timelines"] == Json::array());
	// A target that is gone stands for no other instance.
	fixture.Ok("entity.delete", Json::object({{"entities", Json::array({second})}}));
	error = fixture.Fails("graph.values", Json::object({{"graph", graph}}));
	CHECK(error.Data["path"] == "entity");
	CHECK(error.Message.find("the graph's target") != std::string::npos);
	fixture.Ok("editor.undo");
	live = fixture.Ok("graph.values", Json::object({{"graph", literal}, {"variables", false}}));
	CHECK(live["entity"] == runner);
	CHECK(live["variables"] == Json::array());

	// A stop's frame: its node's inputs (evaluated when it runs, from their sources), the nodes it computed, formulas.
	fixture.Breakpoint(literal, Json::object({{"node", 11}}));
	Json const advanced = fixture.Ok("play.advance", Json::object({{"frames", 1}}));
	REQUIRE(advanced["stopped"] == true);
	uint64_t const stop = advanced["stop"]["id"].get<uint64_t>();
	Json values = fixture.Ok(
		"graph.values", Json::object({{"graph", literal}, {"stop", stop}, {"evaluate", Json::array({"Count * 2", "Count +", "count"})}}));
	CHECK(values["source"] == "Stop");
	CHECK(values["stop"] == stop);
	CHECK(values["frame"] == 0);
	CHECK(values["entity"] == runner);
	REQUIRE(values["nodes"].size() >= 1);
	CHECK(values["nodes"][0]["node"] == 11);
	CHECK(values["nodes"][0]["title"] == "Set Count");
	CHECK_FALSE(
		FindMismatch(Json::array({Json::object({{"name", "Value"}, {"value", 5}})}), values["nodes"][0]["inputs"], "inputs").has_value());
	CHECK_FALSE(FindMismatch(Json::array({Json::object({{"name", "Count"}, {"value", 5}})}), values["variables"], "variables").has_value());
	REQUIRE(values["evaluated"].size() == 3);
	CHECK(values["evaluated"][0]["formula"] == "Count * 2");
	CHECK(values["evaluated"][0]["value"]["value"] == 10);
	CHECK(values["evaluated"][1]["error"].get<std::string>().starts_with("line 1, column "));
	CHECK(values["evaluated"][2]["error"].get<std::string>().find("did you mean") != std::string::npos);

	// A live edit of the stopped node's literal reaches the stopped execution: the input shows the new literal and the old one.
	fixture.Ok(
		"graph.edit",
		Json::object({{"graph", literal},
	                  {"operations", Json::array({Json::object({{"op", "SetInput"}, {"node", 11}, {"pin", "Value"}, {"value", 50}})})}}));
	values = fixture.Ok("graph.values", Json::object({{"graph", literal}, {"stop", stop}, {"nodes", Json::array({11})}}));
	REQUIRE(values["nodes"].size() == 1);
	Json const& input = values["nodes"][0]["inputs"][0];
	CHECK(input["name"] == "Value");
	CHECK(input["value"] == 50);
	CHECK(input["edited"] == true);
	CHECK(input["before"] == 5);
	CHECK(fixture.Ok("graph.debug")["stops"][0]["migrated"] == true);

	// What a stop's frame is refused for.
	CHECK(fixture.Fails("graph.values", Json::object({{"graph", literal}, {"stop", 999}})).Data["path"] == "stop");
	CHECK(fixture.Fails("graph.values", Json::object({{"graph", literal}, {"stop", stop}, {"frame", 4}})).Data["path"] == "frame");
	CHECK(fixture.Fails("graph.values", Json::object({{"graph", graph}, {"stop", stop}})).Data["path"] == "graph");
	CHECK(fixture.Fails("graph.values", Json::object({{"graph", literal}, {"stop", stop}, {"entity", first}})).Data["path"] == "entity");
	CHECK(fixture.Fails("graph.values", Json::object({{"graph", literal}, {"stop", stop}, {"nodes", Json::array({99})}})).Data["path"] ==
	      "nodes[0]");
	fixture.Ok("graph.continue");
	CHECK(fixture.Variable(runner, "Count") == 50);
}

TEST_CASE("VisualScriptDebug: graph.instances lists the instances with their waits and stops, page by page")
{
	DebugFixture fixture;
	std::string const delay = fixture.CreateGraph("Scripts/Delay.sgraph", MakeDelayDocument());
	std::string const counter = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const waiter = fixture.CreateRunner("Waiter", delay);
	std::string const first = fixture.CreateRunner("First", counter);
	fixture.CreateRunner("Second", counter);
	fixture.Breakpoint(counter, Json::object({{"node", 11}}));
	fixture.Ok("graph.target", Json::object({{"graph", counter}, {"entity", first}}));
	fixture.Start(true);
	Json const advanced = fixture.Ok("play.advance", Json::object({{"frames", 1}}));
	REQUIRE(advanced["stopped"] == true);

	Json listed = fixture.Ok("graph.instances");
	CHECK(listed["total"] == 3);
	REQUIRE(listed["instances"].size() == 3);
	listed = fixture.Ok("graph.instances", Json::object({{"graph", delay}}));
	REQUIRE(listed["instances"].size() == 1);
	Json const& waiting = listed["instances"][0];
	CHECK(waiting["entity"] == waiter);
	CHECK(waiting["name"] == "Waiter");
	CHECK(waiting["asset"] == delay);
	CHECK(waiting["path"] == "Scripts/Delay.sgraph");
	CHECK(waiting["state"] == "Waiting");
	REQUIRE(waiting["waits"].size() == 1);
	CHECK(waiting["waits"][0]["node"] == 12);
	CHECK(waiting["waits"][0]["kind"] == "Delay");
	CHECK(waiting["waits"][0]["remaining"].get<double>() > 0.0);
	listed = fixture.Ok("graph.instances", Json::object({{"entity", first}}));
	REQUIRE(listed["instances"].size() == 1);
	CHECK(listed["instances"][0]["state"] == "Stopped");
	CHECK(listed["instances"][0]["stops"] == Json::array({advanced["stop"]["id"]}));
	listed = fixture.Ok("graph.instances", Json::object({{"limit", 1}, {"offset", 1}}));
	CHECK(listed["total"] == 3);
	CHECK(listed["instances"].size() == 1);
	CHECK(fixture.Fails("graph.instances", Json::object({{"entity", "123456789"}})).Code == AutomationErrorCode::EntityNotFound);
}

TEST_CASE("VisualScriptDebug: commands run back to back with no editor frame see each other's configuration and state")
{
	DebugFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const first = fixture.CreateRunner("First", graph);
	std::string const second = fixture.CreateRunner("Second", graph);
	fixture.Start(true);

	// A breakpoint added and play.advance in one batch stops.
	fixture.Breakpoint(graph, Json::object({{"node", 14}}));
	Json advanced = fixture.Ok("play.advance", Json::object({{"frames", 5}}));
	CHECK(advanced["framesRun"] == 1);
	CHECK(advanced["stopped"] == true);
	CHECK(fixture.Fails("play.advance", Json::object({{"frames", 1}})).Code == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("play.step").Code == AutomationErrorCode::InvalidOperation);
	// Continue in the same batch, then cleared breakpoints and play.advance run on.
	fixture.Ok("graph.continue");
	fixture.Ok("graph.breakpoints", Json::object({{"clear", true}}));
	advanced = fixture.Ok("play.advance", Json::object({{"frames", 5}}));
	CHECK(advanced["framesRun"] == 5);
	CHECK(advanced["stopped"] == false);
	// A target set the same way: only that instance stops.
	fixture.Ok("graph.target", Json::object({{"graph", graph}, {"entity", second}}));
	fixture.Breakpoint(graph, Json::object({{"node", 11}}));
	advanced = fixture.Ok("play.advance", Json::object({{"frames", 5}}));
	CHECK(advanced["stop"]["entity"] == second);
	CHECK(fixture.Ok("graph.debug")["stops"].size() == 1);
	// An edit that cancels the stop is seen by the next command, which needs no editor frame.
	fixture.Ok(
		"graph.edit",
		Json::object({{"graph", graph}, {"operations", Json::array({Json::object({{"op", "Delete"}, {"elements", Json::array({11})}})})}}));
	Json const debug = fixture.Ok("graph.debug");
	CHECK(debug["stops"] == Json::array());
	CHECK(fixture.Ok("play.state")["debug"]["stops"] == 0);
	// play.pause false continues stopped executions first; nothing is stopped, so it resumes.
	CHECK(fixture.Ok("play.pause", Json::object({{"paused", false}}))["paused"] == false);
	CHECK(fixture.Variable(first, "Steps").get<int>() >= 6);
}

TEST_CASE("VisualScriptDebug: a breakpoint on a node an edit with errors added is not in the running program")
{
	DebugFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Counter", graph);
	fixture.Start(true);

	// The edit adds node 30 after Steps' Set, setting a variable the graph does not have: the graph has an error, so it runs on
	// with the last version that worked, which has no node 30.
	VisualScriptDocument edited = MakeCounterDocument();
	edited.Graph.Nodes.push_back(MakeNode(30, "Variable.Set", Json::object({{"Variable", 99}}), Json::object({{"Value", 1}})));
	edited.Graph.Links.push_back(MakeLink(14, "Then", 30, "Execute"));
	fixture.Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(edited)}}));
	Json const breakpoint = FindBreakpoint(fixture.Breakpoint(graph, Json::object({{"node", 30}})), 30);
	CHECK(breakpoint["active"] == false);
	CHECK(breakpoint["inactiveReason"] == "not in the running program: the graph has errors");
	Json const listed = FindBreakpoint(fixture.Ok("graph.debug")["breakpoints"], 30);
	CHECK(listed["inactiveReason"] == "not in the running program: the graph has errors");
	// The breakpoint on a node of the program that runs is active, and the frames run past node 30's breakpoint.
	CHECK(FindBreakpoint(fixture.Breakpoint(graph, Json::object({{"node", 14}, {"enabled", false}})), 14)["active"] == true);
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 3}}))["stopped"] == false);
}

TEST_CASE("VisualScriptDebug: the play commands right after a command that ended the stop see it ended")
{
	DebugFixture fixture;
	std::string const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	std::string const spinner = fixture.CreateRunner("Spinner", graph);
	fixture.Breakpoint(graph, Json::object({{"node", 11}}));
	fixture.Start(true);
	REQUIRE(fixture.Ok("play.advance", Json::object({{"frames", 5}}))["stopped"] == true);

	// The stopped execution's entity deleted, then play.state and play.advance with no editor frame between them.
	fixture.Ok("entity.delete", Json::object({{"entities", Json::array({spinner})}}));
	Json const state = fixture.Ok("play.state");
	CHECK(state["debug"] == Json::object({{"state", "Running"}, {"stops", 0}, {"current", nullptr}, {"pauseHeld", false}}));
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 1}}))["framesRun"] == 1);

	// An edit that cancels the stop, then play.step.
	std::string const other = fixture.CreateRunner("Other", graph);
	Json const advanced = fixture.Ok("play.advance", Json::object({{"frames", 5}}));
	REQUIRE(advanced["stopped"] == true);
	CHECK(advanced["stop"]["entity"] == other);
	fixture.Ok(
		"graph.edit",
		Json::object({{"graph", graph}, {"operations", Json::array({Json::object({{"op", "Delete"}, {"elements", Json::array({11})}})})}}));
	CHECK(fixture.Ok("play.step", Json::object({{"frames", 1}}))["debug"]["stops"] == 0);
}

TEST_CASE("VisualScriptDebug: graph.trace ends after the frame in which a waiting step ended its execution, as play.advance does")
{
	DebugFixture fixture;
	// When the graph starts: a Delay of 0.1 s at node 11 whose Completed leads nowhere; on every update: Count += 1 at 21.
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
	document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
	document.Graph.Nodes.push_back(MakeNode(11, "Flow.Delay", Json::object(), Json::object({{"Duration", 0.1}})));
	document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	document.Graph.Nodes.push_back(MakeNode(20, "Event.Update"));
	AddIncrement(document.Graph, 21, 1);
	document.Graph.Links.push_back(MakeLink(20, "Then", 21, "Execute"));
	std::string const graph = fixture.CreateGraph("Scripts/Wait.sgraph", document);
	fixture.CreateRunner("Waiter", graph);
	fixture.Breakpoint(graph, Json::object({{"node", 11}}));
	fixture.Start(true);
	REQUIRE(fixture.Ok("graph.debug")["stops"].size() == 1);
	REQUIRE(fixture.Ok("graph.step", Json::object({{"kind", "Over"}}))["outcome"] == "Waiting");

	Json const traced = fixture.Ok("graph.trace", Json::object({{"frames", 30}}));
	CHECK(traced["framesRun"].get<uint32_t>() < 30);
	CHECK(traced["stopped"] == false);
	CHECK(traced["stop"].is_null());
	Json const debug = fixture.Ok("graph.debug");
	CHECK(debug["state"] == "StepEnded");
	CHECK(fixture.Ok("play.state")["paused"] == true);
}

TEST_CASE("VisualScriptDebug: the documented agent session (Agents/Debugging.json) runs headless, every expectation met")
{
	Json const session = LoadAgentFile("Debugging.json");
	REQUIRE(session.is_array());
	REQUIRE(session.size() == 22);

	// It starts from the Spinner batch the documentation teaches, with the planted mistake.
	Json spinner = LoadAgentFile("Spinner.json");
	for (Json& operation : spinner["operations"])
	{
		if (operation.value("as", "") == "$next")
		{
			operation["settings"]["Formula"] = "(angle + speed * dt) % 36";
		}
	}
	CHECK(session[1]["command"] == "graph.edit");
	CHECK(session[1]["arguments"] == spinner);

	DebugFixture fixture;
	for (size_t index = 0; index < session.size(); index++)
	{
		Json const& step = session[index];
		std::string const command = step["command"].get<std::string>();
		CAPTURE(index + 1);
		CAPTURE(command);
		Json const result = fixture.Ok(command, step["arguments"]);
		std::optional<std::string> const mismatch = FindMismatch(step["expect"], result, "result");
		CHECK_MESSAGE(!mismatch.has_value(), "step ", index + 1, " ", command, ": ", mismatch.value_or(""), "\nresult: ", result.dump());
	}
}

TEST_CASE("VisualScriptDebug: the docs and the game-making skill show every step of the documented agent session verbatim")
{
	auto const read = [](char const* path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(FileSystem::PathFromUtf8(path));
		REQUIRE_MESSAGE(text.IsOk(), path);
		std::string content = text.TakeValue();
		std::erase(content, '\r');
		return content;
	};
	std::string const automation = read(STRADA_AUTOMATION_DOC_PATH);
	std::string const scripting = read(STRADA_VISUAL_SCRIPTING_DOC_PATH);
	std::string const skill = read(STRADA_MAKE_GAME_SKILL_PATH);
	Json const session = LoadAgentFile("Debugging.json");
	REQUIRE(session.size() == 22);
	// Each document shows the whole session as one block, so a step missing, repeated, moved or answered otherwise fails. The
	// guide and the skill show each step as the MCP tool (named as strada's catalog names it) and its compact JSON arguments,
	// then what it answers; the command reference shows the JSON-RPC requests and replies, numbered from 3 (after
	// authenticate and project.create).
	std::string tools;
	std::string rpc;
	for (size_t index = 0; index < session.size(); index++)
	{
		std::string const command = session[index]["command"].get<std::string>();
		std::string const arguments = session[index]["arguments"].dump();
		std::string const expect = session[index]["expect"].dump();
		std::string tool = command;
		std::replace(tool.begin(), tool.end(), '.', '_');
		std::replace(tool.begin(), tool.end(), '-', '_');
		// The arrows are U+2192 and U+2190, spelled as UTF-8.
		tools += fmt::format("{} {}\n  \xE2\x86\x92 {}\n", tool, arguments, expect);
		size_t const id = index + 3;
		rpc += fmt::format("\xE2\x86\x92 {{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"{}\",\"params\":{}}}\n", id, command, arguments);
		rpc += fmt::format("\xE2\x86\x90 {{\"jsonrpc\":\"2.0\",\"id\":{},\"result\":{}}}\n", id, expect);
	}
	std::string const toolBlock = "```\n" + tools + "```\n";
	std::string const rpcBlock = "```\n" + rpc + "```\n";
	CHECK_MESSAGE(scripting.find(toolBlock) != std::string::npos, "Docs/VisualScripting.md does not show the session as:\n", toolBlock);
	CHECK_MESSAGE(skill.find(toolBlock) != std::string::npos, "the strada-make-game skill does not show the session as:\n", toolBlock);
	CHECK_MESSAGE(automation.find(rpcBlock) != std::string::npos, "Docs/Automation.md does not show the session as:\n", rpcBlock);
}

namespace
{
	// The random debug sessions' editor: the Spinner, the three graphs of Agents/DebugFuzz.json and their entities, the
	// commands, and what the generator draws parameters from.
	struct SequenceFixture : DebugFixture
	{
		struct GraphInfo
		{
			std::string Graph;
			Json Document;
			std::vector<uint32_t> Nodes;
			// The node of an Event.Send (renamed by some edits), 0 when it has none.
			uint32_t Send = 0;
		};

		std::vector<GraphInfo> Graphs;
		std::vector<std::string> Entities;

		SequenceFixture()
		{
			Json spinner = LoadAgentFile("Spinner.json");
			Ok("graph.create", Json::object({{"path", "Scripts/Spinner.sgraph"}}));
			Ok("graph.edit", spinner);
			Entities.push_back(CreateRunner("Spinner", "asset://Scripts/Spinner.sgraph"));
			Json const batches = LoadAgentFile("DebugFuzz.json");
			REQUIRE(batches.is_array());
			REQUIRE(batches.size() == 3);
			for (Json const& batch : batches)
			{
				std::string const path = batch["graph"].get<std::string>().substr(std::string_view("asset://").size());
				Ok("graph.create", Json::object({{"path", path}}));
				Json const edited = Ok("graph.edit", batch);
				CHECK_MESSAGE(edited["summary"]["errors"] == 0, path, ": ", edited.dump());
			}
			for (char const* const path :
			     {"Scripts/Spinner.sgraph", "Scripts/Ponger.sgraph", "Scripts/Waiter.sgraph", "Scripts/Pinger.sgraph"})
			{
				GraphInfo info;
				info.Graph = std::string("asset://") + path;
				Graphs.push_back(std::move(info));
			}
			for (char const* const name : {"Ponger A", "Ponger B"})
			{
				Entities.push_back(CreateRunner(name, Graphs[1].Graph));
			}
			Entities.push_back(CreateRunner("Waiter", Graphs[2].Graph));
			for (char const* const name : {"Pinger A", "Pinger B"})
			{
				Entities.push_back(CreateRunner(name, Graphs[3].Graph));
			}
			for (GraphInfo& info : Graphs)
			{
				info.Document = Ok("graph.get", Json::object({{"graph", info.Graph}, {"analyze", false}}))["document"];
				ReadNodes(info);
			}
			Ok("scene.save");
		}

		static void ReadNodes(GraphInfo& info)
		{
			info.Nodes.clear();
			info.Send = 0;
			auto const read = [&info](Json const& graph)
			{
				for (Json const& node : graph.value("Nodes", Json::array()))
				{
					info.Nodes.push_back(node["ID"].get<uint32_t>());
					if (node["Type"] == "Event.Send")
					{
						info.Send = node["ID"].get<uint32_t>();
					}
				}
			};
			read(info.Document["Graph"]);
			for (Json const& function : info.Document.value("Functions", Json::array()))
			{
				read(function["Graph"]);
			}
		}

		// Undoing every step, then redoing them all, gives the scene and the graphs back.
		void CheckHistory()
		{
			while (Run("editor.redo").IsOk())
			{
			}
			Json const scene = Ok("scene.dump");
			std::vector<Json> documents;
			for (GraphInfo const& info : Graphs)
			{
				documents.push_back(Ok("graph.get", Json::object({{"graph", info.Graph}, {"analyze", false}}))["document"]);
			}
			int undone = 0;
			while (Run("editor.undo").IsOk())
			{
				undone++;
			}
			int redone = 0;
			while (Run("editor.redo").IsOk())
			{
				redone++;
			}
			CHECK(undone == redone);
			CHECK(Ok("scene.dump") == scene);
			for (size_t index = 0; index < Graphs.size(); index++)
			{
				CHECK(Ok("graph.get", Json::object({{"graph", Graphs[index].Graph}, {"analyze", false}}))["document"] == documents[index]);
			}
		}
	};
}

namespace
{
	// What the sequence fuzz checks of the live channel: an edit's receipts keep no more stopped executions than there were
	// before it (the stops graph.debug listed), and graph.live answers in its form with only newer serials.
	void CheckLive(std::string const& name, Json const& params, Json const& result, size_t stopsBefore)
	{
		if (name == "graph.edit" || name == "graph.set")
		{
			REQUIRE(result.contains("live"));
			Json const& live = result["live"];
			CHECK(live["playState"].is_string());
			CHECK(live["paused"].is_boolean());
			REQUIRE(live["edits"].is_array());
			if (result.value("dryRun", false) || result["changed"] == false)
			{
				CHECK(live["edits"] == Json::array());
			}
			for (Json const& edit : live["edits"])
			{
				uint64_t kept = 0;
				for (Json const& note : edit["instanceNotes"])
				{
					kept += note["stopsKept"].get<uint64_t>();
				}
				CHECK_MESSAGE(kept <= stopsBefore, "a receipt keeps ", kept, " stops of ", stopsBefore, ": ", edit.dump());
				CHECK(edit["outcome"].is_string());
			}
		}
		if (name == "graph.live")
		{
			REQUIRE(result["graphs"].is_array());
			uint64_t const after = params.value("after", uint64_t(0));
			for (char const* const list : {"edits", "problems"})
			{
				REQUIRE(result[list].is_array());
				for (Json const& entry : result[list])
				{
					CHECK(entry["serial"].get<uint64_t>() > after);
					CHECK(entry["serial"].get<uint64_t>() <= result["serial"].get<uint64_t>());
				}
			}
			for (Json const& graph : result["graphs"])
			{
				CHECK((graph["running"] == "Current" || graph["running"] == "LastWorking" || graph["running"] == "None"));
				CHECK(graph["notRunning"].is_null() == (graph["running"] == "None" || graph["kind"] == "Library"));
			}
		}
	}
}

TEST_CASE("VisualScriptDebug: random debug sessions answer once and leave a consistent state")
{
	SequenceFixture fixture;
	Testing::FuzzRandom random(11700);
	std::array<char const*, 9> const conditions = {"",           "Pings > 3", "Loops == 1", "Waits >= 1",    "Angle > 10",
	                                               "Ticks != 2", "count > 1", "((",         "Echoes / 0 > 1"};
	std::map<std::string, std::pair<int, int>> outcomes;
	int unanswered = 0;

	auto const pickNode = [&random](SequenceFixture::GraphInfo const& info) -> uint32_t
	{
		return info.Nodes.empty() || random.Pick(8) == 0 ? 999999u : info.Nodes[random.Pick(info.Nodes.size())];
	};

	for (int round = 0, rounds = Testing::GetFuzzRounds(50); round < rounds; round++)
	{
		CAPTURE(round);
		// Each round starts from the fixture's graphs (an earlier round's edits may have damaged them).
		for (SequenceFixture::GraphInfo& info : fixture.Graphs)
		{
			fixture.Ok("graph.set", Json::object({{"graph", info.Graph}, {"document", info.Document}}));
		}
		fixture.Start(true);
		std::vector<std::string> requests;
		for (int call = 0; call < 40; call++)
		{
			Json const debug = fixture.Ok("graph.debug");
			std::vector<Json> stops(debug["stops"].begin(), debug["stops"].end());
			auto const pickStop = [&random, &stops]() -> Json
			{
				return stops.empty() || random.Pick(6) == 0 ? Json(uint64_t(987)) : stops[random.Pick(stops.size())]["id"];
			};
			SequenceFixture::GraphInfo const& info = fixture.Graphs[random.Pick(fixture.Graphs.size())];
			std::string name;
			Json params = Json::object();
			switch (random.Pick(18))
			{
				case 0:
				{
					name = "graph.breakpoints";
					params["graph"] = info.Graph;
					if (random.Pick(5) == 0)
					{
						params["clear"] = true;
					}
					if (random.Pick(4) == 0)
					{
						params["remove"] = Json::array({pickNode(info)});
					}
					Json add = Json::array();
					for (size_t count = random.Pick(3); count > 0; count--)
					{
						Json breakpoint = Json::object({{"node", pickNode(info)}});
						if (random.Pick(2) == 0)
						{
							breakpoint["condition"] = conditions[random.Pick(conditions.size())];
						}
						if (random.Pick(4) == 0)
						{
							breakpoint["hitCount"] = random.Pick(4);
						}
						if (random.Pick(4) == 0)
						{
							breakpoint["enabled"] = random.Pick(2) == 0;
						}
						add.push_back(std::move(breakpoint));
					}
					params["add"] = std::move(add);
					if (random.Pick(6) == 0)
					{
						params["breakOnErrors"] = random.Pick(2) == 0;
					}
					break;
				}
				case 1:
					name = "play.advance";
					params["frames"] = 1 + random.Pick(30);
					break;
				case 2:
					name = "play.pause";
					params["paused"] = random.Pick(2) == 0;
					break;
				case 3:
					name = "play.step";
					params["frames"] = 1 + random.Pick(3);
					break;
				case 4:
				{
					std::array<char const*, 3> const kinds = {"Into", "Over", "Out"};
					name = "graph.step";
					params["kind"] = kinds[random.Pick(kinds.size())];
					if (random.Pick(3) == 0)
					{
						params["stop"] = pickStop();
					}
					break;
				}
				case 5:
					name = "graph.continue";
					if (random.Pick(4) == 0)
					{
						params["to"] = Json::object({{"graph", info.Graph}, {"node", pickNode(info)}});
					}
					else if (random.Pick(3) == 0)
					{
						params["stop"] = pickStop();
					}
					break;
				case 6:
					name = "graph.break";
					params["cancel"] = random.Pick(4) == 0;
					if (random.Pick(2) == 0)
					{
						params["graph"] = info.Graph;
					}
					break;
				case 7:
					name = "graph.wait";
					params["timeout"] = 0.1;
					break;
				case 8:
					name = "graph.trace";
					params["frames"] = 1 + random.Pick(10);
					if (random.Pick(2) == 0)
					{
						params["graphs"] = Json::array({info.Graph});
						if (random.Pick(2) == 0)
						{
							params["nodes"] = Json::array({pickNode(info)});
						}
					}
					params["summary"] = random.Pick(3) != 0;
					break;
				case 9:
					name = "graph.values";
					params["graph"] = info.Graph;
					if (!stops.empty() && random.Pick(2) == 0)
					{
						Json const& stop = stops[random.Pick(stops.size())];
						params["graph"] = stop["graph"];
						params["stop"] = stop["id"];
						params["evaluate"] = Json::array({"Pings + 1", "nope", "Angle * 2"});
					}
					else if (random.Pick(2) == 0)
					{
						params["entity"] = fixture.Entities[random.Pick(fixture.Entities.size())];
					}
					break;
				case 10:
					name = "graph.instances";
					if (random.Pick(2) == 0)
					{
						params["graph"] = info.Graph;
					}
					break;
				case 11:
					name = "graph.target";
					params["graph"] = info.Graph;
					if (random.Pick(3) == 0)
					{
						params["any"] = true;
					}
					else if (random.Pick(2) == 0)
					{
						params["entity"] = fixture.Entities[random.Pick(fixture.Entities.size())];
					}
					break;
				case 12:
				{
					// Edits that reach stopped executions: deleting or retyping the stopped node, renaming a Send's event, a formula.
					name = "graph.edit";
					Json stopped = stops.empty() ? Json() : stops[random.Pick(stops.size())];
					Json operations = Json::array();
					std::string target = info.Graph;
					uint32_t const choice = static_cast<uint32_t>(random.Pick(4));
					if (choice == 0 && stopped.is_object() && !stopped["frames"].empty() && stopped["frames"][0].contains("node"))
					{
						target = stopped["frames"][0]["document"].get<std::string>();
						operations.push_back(Json::object({{"op", "Delete"}, {"elements", Json::array({stopped["frames"][0]["node"]})}}));
					}
					else if (choice == 1 && stopped.is_object() && !stopped["frames"].empty() && stopped["frames"][0].contains("node"))
					{
						// Retyped: graph.set gives the stopped node's ID another Type.
						name = "graph.set";
						std::string const document = stopped["frames"][0]["document"].get<std::string>();
						Json edited = fixture.Ok("graph.get", Json::object({{"graph", document}, {"analyze", false}}))["document"];
						for (Json& node : edited["Graph"]["Nodes"])
						{
							if (node["ID"] == stopped["frames"][0]["node"])
							{
								node["Type"] = "Flow.Sequence";
								node.erase("Settings");
								node.erase("Inputs");
							}
						}
						params = Json::object({{"graph", document}, {"document", std::move(edited)}});
						break;
					}
					else if (choice == 2 && fixture.Graphs[3].Send != 0)
					{
						target = fixture.Graphs[3].Graph;
						operations.push_back(Json::object({{"op", "SetSetting"},
						                                   {"node", fixture.Graphs[3].Send},
						                                   {"name", "Name"},
						                                   {"value", random.Pick(2) == 0 ? "Pong" : "Ping"}}));
					}
					else
					{
						target = fixture.Graphs[0].Graph;
						operations.push_back(
							Json::object({{"op", "SetFormula"},
						                  {"node", 7},
						                  {"formula", random.Pick(2) == 0 ? "(angle + speed * dt) % 36" : "(angle + speed * dt) % 360"}}));
					}
					params = Json::object({{"graph", target}, {"operations", std::move(operations)}});
					break;
				}
				case 13:
					name = "editor.undo";
					break;
				case 14:
					name = "editor.redo";
					break;
				case 15:
				{
					name = "entity.delete";
					std::string entity = fixture.Entities[random.Pick(fixture.Entities.size())];
					if (!stops.empty() && random.Pick(2) == 0 && stops[0]["entity"].is_string())
					{
						entity = stops[0]["entity"].get<std::string>();
					}
					params["entities"] = Json::array({entity});
					break;
				}
				case 16:
					name = "graph.live";
					if (random.Pick(2) == 0)
					{
						params["graph"] = info.Graph;
					}
					if (random.Pick(3) == 0)
					{
						params["after"] = random.Pick(4) == 0 ? uint64_t(1000000) : static_cast<uint64_t>(random.Pick(200));
					}
					break;
				default:
					// Rarely, so that most rounds make their forty calls.
					name = random.Pick(5) == 0 ? "play.stop" : "play.advance";
					if (name == "play.advance")
					{
						params["frames"] = 1 + random.Pick(30);
					}
					break;
			}

			std::string const request = name + " " + params.dump();
			CAPTURE(request);
			requests.push_back(request);
			// A wait may answer after this iteration: its count outlives it.
			Ref<int> const answers = CreateRef<int>(0);
			unanswered++;
			size_t const stopsBefore = stops.size();
			fixture.Registry.Execute(name, params,
			                         [answers, &unanswered, &outcomes, name, params, request, stopsBefore](CommandResult result)
			                         {
										 (*answers)++;
										 unanswered--;
										 CHECK_MESSAGE(*answers == 1, "answered twice: ", request);
										 if (result.IsOk())
										 {
											 outcomes[name].first++;
											 CAPTURE(request);
											 CheckLive(name, params, result.GetValue(), stopsBefore);
											 return;
										 }
										 outcomes[name].second++;
										 CHECK_MESSAGE(result.GetError().Code != AutomationErrorCode::InternalError, request, ": ",
				                                       result.GetError().Message);
									 });
			// Every command but graph.wait answers before Execute returns (a wait answers at once, or at a later frame's end).
			if (name != "graph.wait")
			{
				CHECK_MESSAGE(*answers == 1, "unanswered: ", request);
			}
			// The state agrees with itself: graph.debug with play.state, and every stopped execution with its instance. play.state
			// is read first, right after the call with no editor frame between them, as a batch runs commands.
			Json const play = fixture.Ok("play.state");
			Json const after = fixture.Ok("graph.debug");
			CHECK(after["state"] == play["debug"]["state"]);
			CHECK(after["stops"].size() == play["debug"]["stops"]);
			CHECK(after["current"] == play["debug"]["current"]);
			CHECK(after["pauseHeld"] == play["debug"]["pauseHeld"]);
			if (!fixture.Play.IsPlaying())
			{
				CHECK(after["state"] == "Detached");
				CHECK(after["stops"] == Json::array());
				fixture.Session->EndFrame();
				break;
			}
			// Every stop, read-only ones too, is in its instance's stops while its entity exists.
			Json const instances = fixture.Ok("graph.instances", Json::object({{"limit", 500}}));
			for (Json const& stop : after["stops"])
			{
				if (!stop["entity"].is_string() ||
				    !fixture.Context.GetScene().HasEntity(*UUID::FromString(stop["entity"].get<std::string>())))
				{
					continue;
				}
				bool const listed = std::any_of(instances["instances"].begin(), instances["instances"].end(),
				                                [&stop](Json const& instance)
				                                {
													Json const& ids = instance["stops"];
													return std::find(ids.begin(), ids.end(), stop["id"]) != ids.end();
												});
				CHECK_MESSAGE(listed, "stop ", stop.dump(), " is in no instance's stops: ", instances.dump());
			}
			fixture.Session->EndFrame();
		}
		if (fixture.Play.IsPlaying())
		{
			fixture.Ok("play.stop");
		}
		// Waits answer once play stops.
		CHECK(unanswered == 0);
		Json const ended = fixture.Ok("graph.debug");
		CHECK(ended["state"] == "Detached");
		CHECK(ended["stops"] == Json::array());
		Json const dump = fixture.Ok("scene.dump");
		Result<Ref<Scene>> const loaded = SceneSerializer::Deserialize(dump, AssetManager::CreateDeserializationContext());
		CHECK_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
		if (round % 10 == 9)
		{
			fixture.CheckHistory();
		}
	}
	fixture.CheckHistory();

	// The sessions reached the commands' work.
	for (char const* const command : {"graph.breakpoints", "graph.target", "graph.continue", "graph.step", "graph.values",
	                                  "graph.instances", "graph.trace", "play.advance", "graph.live", "graph.edit"})
	{
		CAPTURE(command);
		CHECK(outcomes[command].first > 0);
	}
}
