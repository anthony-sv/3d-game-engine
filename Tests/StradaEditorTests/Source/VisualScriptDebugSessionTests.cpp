#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/PlayMode.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/VisualScriptDebugger.h"

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace Strada;

// The editor's visual-script debug session over the scripting runtime, as play mode's observer: stops pause play at the end
// of their frame, controls run graph code between frames, breakpoints follow edits and projects (spec-slice4 sections 4.9,
// 10.3 and 11).
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
	void AddIncrement(VisualScriptDocument& document, uint32_t set, uint32_t variable)
	{
		document.Graph.Nodes.push_back(MakeNode(set, "Variable.Set", Json::object({{"Variable", variable}})));
		document.Graph.Nodes.push_back(MakeNode(set + 1, "Operator.Add", Json::object(), Json::object({{"B", 1}})));
		document.Graph.Nodes.push_back(MakeNode(set + 2, "Variable.Get", Json::object({{"Variable", variable}})));
		document.Graph.Links.push_back(MakeLink(set + 2, "Value", set + 1, "A"));
		document.Graph.Links.push_back(MakeLink(set + 1, "Result", set, "Value"));
	}

	// On every update: Count += 1 at node 11, then Steps += 1 at node 14.
	VisualScriptDocument MakeCounterDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
		document.Variables.push_back(MakeVariable(2, "Steps", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		AddIncrement(document, 11, 1);
		AddIncrement(document, 14, 2);
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(11, "Then", 14, "Execute"));
		return document;
	}

	// When the graph starts: Count = 1 at node 11.
	VisualScriptDocument MakeCreateDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}}), Json::object({{"Value", 1}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		return document;
	}

	// When the graph starts: a Delay of 0.1 s at node 11 whose Completed leads nowhere; on every update: Count += 1 at node 21.
	VisualScriptDocument MakeWaitDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(11, "Flow.Delay", Json::object(), Json::object({{"Duration", 0.1}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Nodes.push_back(MakeNode(20, "Event.Update"));
		AddIncrement(document, 21, 1);
		document.Graph.Links.push_back(MakeLink(20, "Then", 21, "Execute"));
		return document;
	}

	// On every update: Result = 1 / Zero at node 11, an integer division by zero.
	VisualScriptDocument MakeFailingDocument()
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

	// On every update: the call at node 11.
	VisualScriptDocument MakeCallDocument(std::string type, Json inputs = Json::object())
	{
		VisualScriptDocument document;
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		document.Graph.Nodes.push_back(MakeNode(11, std::move(type), Json::object(), std::move(inputs)));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		return document;
	}

	// On every update: Count += 1 at node 11, then destroys the entity named victim at node 14 (its target found by node 15).
	VisualScriptDocument MakeDestroyByNameDocument(std::string victim)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		AddIncrement(document, 11, 1);
		document.Graph.Nodes.push_back(MakeNode(14, "Call:Strada.Entity.Destroy()"));
		document.Graph.Nodes.push_back(
			MakeNode(15, "Call:Strada.Entity.FindByName(string)", Json::object(), Json::object({{"Name", std::move(victim)}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(11, "Then", 14, "Execute"));
		document.Graph.Links.push_back(MakeLink(15, "Result", 14, "Target"));
		return document;
	}

	VisualScriptBreakpoint MakeBreakpoint(uint32_t node, std::string condition = {}, uint32_t hitCount = 0)
	{
		VisualScriptBreakpoint breakpoint;
		breakpoint.Node = node;
		breakpoint.Condition = std::move(condition);
		breakpoint.HitCount = hitCount;
		return breakpoint;
	}

	// A project open in an editor model with the commands of the editor and of play mode, and the debug session as play
	// mode's observer, as the editor layer sets them up (without the UI: breakpoints are kept in memory).
	struct SessionFixture
	{
		Testing::ScriptRuntimeScope Runtime;
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};
		VisualScriptDebugSession Session{Context, Play};
		CommandRegistry Registry;

		SessionFixture()
		{
			REQUIRE(RegisterEditorCommands(Registry, Operations, EditorCommandEnvironment()).IsOk());
			REQUIRE(RegisterPlayCommands(Registry, Play, Context,
			                             []
			                             {
											 return glm::uvec2(640, 480);
										 })
			            .IsOk());
			REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			Play.SetObserver(&Session);
		}

		~SessionFixture()
		{
			Play.Stop();
			Play.SetObserver(nullptr);
		}

		SessionFixture(SessionFixture const&) = delete;
		SessionFixture& operator=(SessionFixture const&) = delete;

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

		AssetHandle CreateGraph(std::string const& path, VisualScriptDocument const& document)
		{
			std::string const graph = Ok("graph.create", Json::object({{"path", path}}))["asset"].get<std::string>();
			Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(document)}}));
			Result<AssetHandle> const handle = AssetManager::ResolveReference(graph);
			REQUIRE(handle.IsOk());
			return handle.GetValue();
		}

		VisualScriptDocument GetDocument(AssetHandle graph) const
		{
			Ref<VisualScriptAsset> const script = AssetManager::GetAsset<VisualScriptAsset>(graph);
			REQUIRE(script != nullptr);
			return script->GetDocument();
		}

		UUID CreateRunner(std::string name, AssetHandle graph)
		{
			Json const created =
				Ok("entity.create",
			       Json::object({{"name", std::move(name)},
			                     {"components", Json::object({{"VisualScript", Json::object({{"Graph", graph.ToString()}})}})}}));
			std::optional<UUID> const id = UUID::FromString(created["id"].get<std::string>());
			REQUIRE(id.has_value());
			return *id;
		}

		// What the running graph holds (a variable it lacks reads as null).
		Json GetVariable(UUID entity, std::string const& name)
		{
			Json const read = Ok("entity.get", Json::object({{"entity", entity.ToString()}}));
			return read["components"]["VisualScript"]["Variables"][name]["Value"];
		}

		void Start(bool paused = false, EditorPlayState state = EditorPlayState::Play, bool testRun = false)
		{
			PlayStartOptions options;
			options.Paused = paused;
			options.TestRun = testRun;
			Result<void> const started = Play.Start(state, 640, 480, nullptr, options);
			REQUIRE_MESSAGE(started.IsOk(), (started ? std::string() : started.GetError()));
		}

		void SetBreakpoint(AssetHandle graph, VisualScriptBreakpoint breakpoint)
		{
			Result<void> const set = Session.SetBreakpoint(graph, std::move(breakpoint));
			REQUIRE_MESSAGE(set.IsOk(), (set ? std::string() : set.GetError()));
		}

		// An editor frame without automation: the queued resumes, play mode's update, the session's end of the frame.
		void EditorFrame()
		{
			Session.BeginFrame();
			Play.Update(Timestep(FrameTime));
			Session.EndFrame();
		}

		uint32_t Advance(uint32_t frames) { return Play.Advance(frames, Timestep(FrameTime)); }

		uint64_t GetFrame() const { return Context.GetScene().GetRuntimeFrame(); }

		VisualScriptStop const& GetCurrentStop() const
		{
			VisualScriptDebugState const& state = Session.GetState();
			REQUIRE(state.Current.has_value());
			auto const found = std::find_if(state.Stops.begin(), state.Stops.end(),
			                                [&state](VisualScriptStop const& stop)
			                                {
												return stop.ID == *state.Current;
											});
			REQUIRE(found != state.Stops.end());
			return *found;
		}

		VisualScriptResumeOutcome Resume(VisualScriptDebugAction action, std::optional<uint64_t> stop = std::nullopt, bool all = false)
		{
			Result<VisualScriptResumeOutcome> resumed = Session.Resume(stop, action, all);
			REQUIRE_MESSAGE(resumed.IsOk(), (resumed ? std::string() : resumed.GetError()));
			return resumed.GetValue();
		}

		VisualScriptResumeOutcome ContinueAll() { return Resume(VisualScriptDebugAction::Continue, std::nullopt, true); }
	};

	std::string GetError(Result<VisualScriptResumeOutcome> const& result)
	{
		REQUIRE(result.IsError());
		return result.GetError();
	}

	// Captures what a completion is given.
	struct CompletionProbe
	{
		std::optional<CommandResult> Result;

		CommandCompletion Make()
		{
			return CommandCompletion(
				[this](CommandResult value)
				{
					Result.emplace(std::move(value));
				});
		}

		Json Value() const
		{
			REQUIRE(Result.has_value());
			REQUIRE_MESSAGE(Result->IsOk(), (Result->IsError() ? Result->GetError().Message : std::string()));
			return Result->GetValue();
		}
	};
}

TEST_CASE("VisualScriptDebug: the session attaches the debugger for Play, never for Simulate or test runs, and detaches at Stop")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Counter", graph);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Detached);
	CHECK(GetError(fixture.Session.Resume(std::nullopt, VisualScriptDebugAction::Continue)) ==
	      "the editor is not playing: play.start starts the game");

	fixture.Start(false, EditorPlayState::Simulate);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Detached);
	CHECK_FALSE(VisualScriptDebugger::IsAttached());
	CHECK(GetError(fixture.Session.Resume(std::nullopt, VisualScriptDebugAction::Continue)) ==
	      "visual scripts do not run while simulating");
	fixture.Play.Stop();

	// A test run ignores breakpoints.
	fixture.SetBreakpoint(graph, MakeBreakpoint(11));
	fixture.Start(false, EditorPlayState::Play, true);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Detached);
	CHECK_FALSE(VisualScriptDebugger::IsAttached());
	CHECK(fixture.Advance(3) == 3);
	CHECK(fixture.Session.GetState().Stops.empty());
	CHECK(fixture.Session.Break().IsError());
	fixture.Play.Stop();

	fixture.Session.ClearBreakpoints(std::nullopt);
	fixture.Start();
	CHECK(fixture.Session.IsAttached());
	CHECK(VisualScriptDebugger::IsAttached());
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	CHECK(fixture.Play.GetDebugSummary().State == "Running");
	CHECK(fixture.Ok("play.state")["debug"] ==
	      Json::object({{"state", "Running"}, {"stops", 0}, {"current", nullptr}, {"pauseHeld", false}}));
	fixture.Play.Stop();
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Detached);
	CHECK_FALSE(VisualScriptDebugger::IsAttached());
}

TEST_CASE("VisualScriptDebug: a breakpoint pauses play at the end of its frame, which runs on; frames are refused until it continues")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	UUID const first = fixture.CreateRunner("First", graph);
	UUID const second = fixture.CreateRunner("Second", graph);
	std::vector<uint64_t> notified;
	// The listener hears of a stop once the scene is paused for it, with the pause held.
	std::vector<std::pair<bool, bool>> pausedWhenNotified;
	int resumes = 0;
	fixture.Session.SetStopListener(
		[&notified, &pausedWhenNotified, &fixture](VisualScriptStop const& stop)
		{
			notified.push_back(stop.ID);
			pausedWhenNotified.emplace_back(fixture.Play.IsPaused(), fixture.Session.HasPausedScene());
		});
	fixture.Session.SetResumeListener(
		[&resumes]
		{
			resumes++;
		});
	fixture.SetBreakpoint(graph, MakeBreakpoint(11));
	fixture.Session.SetTarget(graph, first);
	CHECK(fixture.Session.GetTarget(graph) == first);

	fixture.Start();
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	fixture.EditorFrame();
	CHECK(fixture.GetFrame() == 1);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(fixture.Play.IsPaused());
	CHECK(fixture.Session.HasPausedScene());
	VisualScriptStop const stop = fixture.GetCurrentStop();
	CHECK(fixture.Session.GetState().Stops.size() == 1);
	CHECK(stop.Entity == first);
	CHECK(stop.Graph == graph);
	CHECK(stop.Reason == VisualScriptStopReason::Breakpoint);
	CHECK(stop.Resumable);
	CHECK(stop.Breakpoint == std::make_pair(graph, uint32_t(11)));
	CHECK(notified == std::vector<uint64_t>{stop.ID});
	CHECK(pausedWhenNotified == std::vector<std::pair<bool, bool>>{{true, true}});
	// The rest of the frame ran: the other instance, which is not the target, set both of its variables.
	CHECK(fixture.GetVariable(first, "Count") == 0);
	CHECK(fixture.GetVariable(second, "Count") == 1);
	CHECK(fixture.GetVariable(second, "Steps") == 1);

	// While it is stopped no frame runs, however asked for.
	std::string const reason =
		fmt::format("an execution is stopped at a breakpoint (stop {}): graph.continue or graph.step it first", stop.ID);
	CHECK(fixture.Session.GetFrameStepRefusal() == reason);
	CHECK(fixture.Session.GetFrameStepRefusalForUI() == std::string(VisualScriptDebugSession::RefusalForUI));
	CHECK(fixture.Play.GetFrameStepRefusal() == reason);
	CHECK_FALSE(fixture.Play.SetPaused(false));
	CHECK(fixture.Play.IsPaused());
	CHECK(fixture.Advance(3) == 0);
	fixture.Play.Step(2);
	fixture.EditorFrame();
	fixture.EditorFrame();
	CHECK(fixture.GetFrame() == 1);
	CommandResult const stepped = fixture.Run("play.step", Json::object({{"frames", 2}}));
	REQUIRE(stepped.IsError());
	CHECK(stepped.GetError().Code == AutomationErrorCode::InvalidOperation);
	CHECK(stepped.GetError().Message == reason);
	CHECK(fixture.Ok("play.state")["debug"] ==
	      Json::object({{"state", "Stopped"}, {"stops", 1}, {"current", stop.ID}, {"pauseHeld", true}}));

	// Continue runs the rest of the stopped execution between frames, and resumes play: the debugger paused it.
	VisualScriptResumeOutcome const continued = fixture.ContinueAll();
	CHECK(continued.Result == VisualScriptResumeResult::Ended);
	CHECK(fixture.GetFrame() == 1);
	CHECK(fixture.GetVariable(first, "Count") == 1);
	CHECK(fixture.GetVariable(first, "Steps") == 1);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	CHECK_FALSE(fixture.Play.IsPaused());
	CHECK_FALSE(fixture.Session.HasPausedScene());
	CHECK(resumes == 1);

	// The next frame stops it again; play mode's Resume continues it first, then unpauses.
	fixture.EditorFrame();
	CHECK(fixture.GetFrame() == 2);
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(notified.size() == 2);
	CHECK(fixture.Play.Resume());
	CHECK(fixture.GetVariable(first, "Count") == 2);
	CHECK_FALSE(fixture.Play.IsPaused());
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	CHECK(resumes == 2);

	// Stop ends every stop: nothing more of them runs.
	fixture.EditorFrame();
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	fixture.Play.Stop();
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Detached);
	CHECK(fixture.Session.GetState().Stops.empty());
	CHECK_FALSE(fixture.Session.GetFrameStepRefusal().has_value());
}

TEST_CASE("VisualScriptDebug: play.start paused runs no update before play.advance, which stops early at a stop; queued frame steps "
          "are cancelled at it")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	UUID const counter = fixture.CreateRunner("Counter", graph);

	fixture.Ok("play.start", Json::object({{"paused", true}}));
	for (int frame = 0; frame < 3; frame++)
	{
		fixture.EditorFrame();
	}
	CHECK(fixture.GetFrame() == 0);
	CHECK(fixture.GetVariable(counter, "Count") == 0);

	// The third hit stops: two frames run on, the third holds.
	fixture.SetBreakpoint(graph, MakeBreakpoint(11, {}, 3));
	Json const advanced = fixture.Ok("play.advance", Json::object({{"frames", 10}}));
	CHECK(advanced["framesRun"] == 3);
	CHECK(advanced["debug"]["state"] == "Stopped");
	CHECK(fixture.GetFrame() == 3);
	CHECK(fixture.GetVariable(counter, "Count") == 2);
	// The agent paused the scene, not the debugger: continuing leaves it paused.
	CHECK_FALSE(fixture.Session.HasPausedScene());
	CHECK(fixture.ContinueAll().Result == VisualScriptResumeResult::Ended);
	CHECK(fixture.Play.IsPaused());
	CHECK(fixture.GetVariable(counter, "Count") == 3);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);

	// play.step queues ten frames, which a paused scene runs one per editor frame: a stop in the third cancels the rest.
	fixture.SetBreakpoint(graph, MakeBreakpoint(11, "Count == 5"));
	fixture.Ok("play.step", Json::object({{"frames", 10}}));
	for (int frame = 0; frame < 10; frame++)
	{
		fixture.EditorFrame();
	}
	CHECK(fixture.GetFrame() == 6);
	CHECK(fixture.GetVariable(counter, "Count") == 5);
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(fixture.ContinueAll().Result == VisualScriptResumeResult::Ended);
	for (int frame = 0; frame < 3; frame++)
	{
		fixture.EditorFrame();
	}
	CHECK(fixture.GetFrame() == 6);
	CHECK(fixture.GetVariable(counter, "Count") == 6);
}

TEST_CASE("VisualScriptDebug: an Event.Create stop pauses play before its first frame")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Create.sgraph", MakeCreateDocument());
	UUID const created = fixture.CreateRunner("Created", graph);
	fixture.SetBreakpoint(graph, MakeBreakpoint(11));

	fixture.Start();
	CHECK(fixture.GetFrame() == 0);
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(fixture.Play.IsPaused());
	CHECK(fixture.Session.HasPausedScene());
	CHECK(fixture.GetCurrentStop().Start.Kind == VisualScriptStartKind::Event);
	CHECK(fixture.GetVariable(created, "Count") == 0);
	// The start ran no frame: the flow clock still reads zero.
	CHECK(fixture.Session.GetFlowTime() == 0.0);

	CHECK(fixture.ContinueAll().Result == VisualScriptResumeResult::Ended);
	CHECK(fixture.GetVariable(created, "Count") == 1);
	CHECK_FALSE(fixture.Play.IsPaused());
	fixture.EditorFrame();
	CHECK(fixture.GetFrame() == 1);
}

TEST_CASE("VisualScriptDebug: steps stop at the next node or end the execution, run no frame, and resume play only when the debugger "
          "paused it")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	UUID const counter = fixture.CreateRunner("Counter", graph);
	fixture.SetBreakpoint(graph, MakeBreakpoint(11));
	fixture.Start();
	fixture.EditorFrame();
	uint64_t const first = fixture.GetCurrentStop().ID;

	// Over runs node 11 and stops where its exit leads: a new stop, made by the step.
	VisualScriptResumeOutcome const over = fixture.Resume(VisualScriptDebugAction::StepOver);
	CHECK(over.Result == VisualScriptResumeResult::Stopped);
	REQUIRE(over.Stop.has_value());
	CHECK(*over.Stop != first);
	VisualScriptStop const& stepped = fixture.GetCurrentStop();
	CHECK(stepped.ID == *over.Stop);
	CHECK(stepped.Reason == VisualScriptStopReason::Step);
	REQUIRE(!stepped.Frames.empty());
	CHECK(stepped.Frames.front().Node == 14);
	CHECK(fixture.GetVariable(counter, "Count") == 1);
	CHECK(fixture.GetVariable(counter, "Steps") == 0);
	CHECK(fixture.GetFrame() == 1);

	// Over again ends the execution: StepEnded, and the scene stays paused.
	VisualScriptResumeOutcome const ended = fixture.Resume(VisualScriptDebugAction::StepOver);
	CHECK(ended.Result == VisualScriptResumeResult::Ended);
	CHECK(fixture.GetVariable(counter, "Steps") == 1);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::StepEnded);
	CHECK(fixture.Play.IsPaused());
	CHECK(fixture.Session.HasPausedScene());
	CHECK(fixture.Ok("play.state")["debug"]["state"] == "StepEnded");
	CHECK_FALSE(fixture.Session.GetFrameStepRefusal().has_value());
	CHECK(fixture.GetFrame() == 1);

	// Continue with nothing stopped and the pause held resumes play.
	CHECK(fixture.Resume(VisualScriptDebugAction::Continue).Result == VisualScriptResumeResult::Resumed);
	CHECK_FALSE(fixture.Play.IsPaused());
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	CHECK(GetError(fixture.Session.Resume(std::nullopt, VisualScriptDebugAction::Continue)) ==
	      "nothing is stopped: no execution to continue");
	CHECK(GetError(fixture.Session.Resume(std::nullopt, VisualScriptDebugAction::StepInto)) == "nothing is stopped: no execution to step");

	// A scene the agent paused stays paused through steps and continues.
	fixture.Ok("play.pause", Json::object({{"paused", true}}));
	CHECK(fixture.Advance(5) == 1);
	CHECK_FALSE(fixture.Session.HasPausedScene());
	CHECK(fixture.Resume(VisualScriptDebugAction::StepOver).Result == VisualScriptResumeResult::Stopped);
	CHECK(fixture.ContinueAll().Result == VisualScriptResumeResult::Ended);
	CHECK(fixture.Play.IsPaused());
	CHECK(fixture.GetFrame() == 2);
	CHECK(fixture.GetVariable(counter, "Steps") == 2);
}

TEST_CASE("VisualScriptDebug: Step Over a Delay whose Completed is unlinked waits, and reaches StepEnded at that frame's end")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Wait.sgraph", MakeWaitDocument());
	UUID const waiter = fixture.CreateRunner("Waiter", graph);
	fixture.SetBreakpoint(graph, MakeBreakpoint(11));
	fixture.Start();
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);

	// The step waits in the Delay: play resumes, since the debugger paused it.
	VisualScriptResumeOutcome const waiting = fixture.Resume(VisualScriptDebugAction::StepOver);
	CHECK(waiting.Result == VisualScriptResumeResult::Waiting);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stepping);
	CHECK_FALSE(fixture.Play.IsPaused());
	CHECK_FALSE(fixture.Session.HasPausedScene());
	REQUIRE(fixture.Session.GetState().Stepping.has_value());
	REQUIRE(fixture.Session.GetState().Stepping->WaitsIn.has_value());
	CHECK(fixture.Session.GetState().Stepping->WaitsIn->Node == 11);
	CHECK_FALSE(fixture.Session.GetFrameStepRefusal().has_value());

	// Frames run until the Delay completes; its Completed leads nowhere, so the execution ends and play pauses after that frame.
	uint32_t const run = fixture.Advance(30);
	CHECK(run >= 5);
	CHECK(run <= 8);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::StepEnded);
	CHECK(fixture.Play.IsPaused());
	CHECK(fixture.Session.HasPausedScene());
	CHECK(fixture.GetVariable(waiter, "Count") == static_cast<int>(run));

	// Nothing to step out of; Over arms a Break for the graph and resumes play, which stops at its next stop point.
	CHECK(GetError(fixture.Session.Resume(std::nullopt, VisualScriptDebugAction::StepOut)) ==
	      "the execution ended: nothing to step out of");
	CHECK(fixture.Resume(VisualScriptDebugAction::StepOver).Result == VisualScriptResumeResult::Armed);
	CHECK_FALSE(fixture.Play.IsPaused());
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	CHECK(fixture.Advance(5) == 1);
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(fixture.GetCurrentStop().Reason == VisualScriptStopReason::Break);
	CHECK(fixture.GetCurrentStop().Graph == graph);
	CHECK(fixture.ContinueAll().Result == VisualScriptResumeResult::Ended);
	CHECK_FALSE(fixture.Play.IsPaused());
}

TEST_CASE("VisualScriptDebug: a stop that ends without a resume keeps the scene paused with the pause held, until Continue")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	UUID const counter = fixture.CreateRunner("Counter", graph);
	std::vector<std::pair<AssetHandle, std::string>> ended;
	fixture.Session.SetStopEndedListener(
		[&ended](AssetHandle stopped, std::string const& reason)
		{
			ended.emplace_back(stopped, reason);
		});
	fixture.SetBreakpoint(graph, MakeBreakpoint(11));
	fixture.Start();
	fixture.EditorFrame();
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);

	// An edit removes the stopped node: the edit cancels the execution in its call, the session sees it at the frame's end.
	VisualScriptDocument edited = MakeCounterDocument();
	std::erase_if(edited.Graph.Nodes,
	              [](VisualScriptNode const& node)
	              {
					  return node.ID >= 11 && node.ID <= 13;
				  });
	std::erase_if(edited.Graph.Links,
	              [](VisualScriptLink const& link)
	              {
					  return link.From.Node <= 13 || link.To.Node <= 13;
				  });
	edited.Graph.Links.push_back(MakeLink(10, "Then", 14, "Execute"));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(graph, edited).IsOk());
	fixture.Session.EndFrame();
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	CHECK(fixture.Play.IsPaused());
	CHECK(fixture.Session.HasPausedScene());
	CHECK(ended == std::vector<std::pair<AssetHandle, std::string>>{{graph, std::string(VisualScriptDebugSession::StopEndedByEdit)}});
	// Its breakpoint went with the node.
	CHECK(fixture.Session.GetBreakpoints().Find(graph, 11) == nullptr);
	CHECK(fixture.Ok("play.state")["debug"]["pauseHeld"] == true);
	CHECK(fixture.Resume(VisualScriptDebugAction::Continue).Result == VisualScriptResumeResult::Resumed);
	CHECK_FALSE(fixture.Play.IsPaused());

	// Undo brings the node back, not its breakpoint.
	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Session.EndFrame();
	CHECK(fixture.Session.GetBreakpoints().Find(graph, 11) == nullptr);
	fixture.SetBreakpoint(graph, MakeBreakpoint(11));
	fixture.EditorFrame();
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);

	// The stopped execution's entity is deleted.
	fixture.Ok("entity.delete", Json::object({{"entities", Json::array({counter.ToString()})}}));
	fixture.Session.EndFrame();
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	CHECK(fixture.Session.HasPausedScene());
	REQUIRE(ended.size() == 2);
	CHECK(ended[1] == std::make_pair(graph, std::string(VisualScriptDebugSession::StopEndedByDestroy)));
	CHECK(fixture.Resume(VisualScriptDebugAction::Continue).Result == VisualScriptResumeResult::Resumed);
	CHECK_FALSE(fixture.Play.IsPaused());
}

TEST_CASE("VisualScriptDebug: a stop that another stop's resume ends keeps the scene paused with the pause held, and the listener is "
          "told why")
{
	SessionFixture fixture;
	std::vector<std::pair<AssetHandle, std::string>> ended;
	fixture.Session.SetStopEndedListener(
		[&ended, &fixture](AssetHandle stopped, std::string const& reason)
		{
			// Told once the scene is kept paused.
			CHECK(fixture.Play.IsPaused());
			ended.emplace_back(stopped, reason);
		});
	// The stop of the first entity, of the two stopped.
	auto const findStop = [&fixture](UUID entity)
	{
		std::vector<VisualScriptStop> const& stops = fixture.Session.GetState().Stops;
		auto const found = std::find_if(stops.begin(), stops.end(),
		                                [entity](VisualScriptStop const& candidate)
		                                {
											return candidate.Entity == entity;
										});
		REQUIRE(found != stops.end());
		return found->ID;
	};

	SUBCASE("the resumed execution destroys the other stop's entity")
	{
		AssetHandle const graph = fixture.CreateGraph("Scripts/Pair.sgraph", MakeDestroyByNameDocument("Victim"));
		UUID const victim = fixture.CreateRunner("Victim", graph);
		UUID const killer = fixture.CreateRunner("Killer", graph);
		fixture.SetBreakpoint(graph, MakeBreakpoint(11));
		fixture.Start();
		fixture.EditorFrame();
		REQUIRE(fixture.Session.GetState().Stops.size() == 2);
		CHECK(fixture.Session.HasPausedScene());

		CHECK(fixture.Resume(VisualScriptDebugAction::Continue, findStop(killer)).Result == VisualScriptResumeResult::Ended);
		CHECK_FALSE(fixture.Context.GetScene().HasEntity(victim));
		CHECK(fixture.Session.GetState().Stops.empty());
		CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
		CHECK(fixture.Play.IsPaused());
		CHECK(fixture.Session.HasPausedScene());
		CHECK(ended ==
		      std::vector<std::pair<AssetHandle, std::string>>{{graph, std::string(VisualScriptDebugSession::StopEndedByDestroy)}});
		CHECK(fixture.Ok("play.state")["debug"]["pauseHeld"] == true);
	}
	SUBCASE("the resumed execution loads another scene")
	{
		std::filesystem::path const second = AssetManager::GetAssetDirectory() / "Scenes" / "Second.sscene";
		REQUIRE(FileSystem::CreateDirectories(second.parent_path()).IsOk());
		REQUIRE(SceneSerializer::SaveToFile(Scene("Second"), second).IsOk());
		REQUIRE(fixture.Operations.RefreshAssets().IsOk());
		AssetHandle const graph =
			fixture.CreateGraph("Scripts/Loader.sgraph", MakeCallDocument("Call:Strada.SceneManager.LoadScene(string)",
		                                                                  Json::object({{"Path", "Scenes/Second.sscene"}})));
		UUID const first = fixture.CreateRunner("First", graph);
		fixture.CreateRunner("Second", graph);
		fixture.SetBreakpoint(graph, MakeBreakpoint(11));
		fixture.Start();
		fixture.EditorFrame();
		REQUIRE(fixture.Session.GetState().Stops.size() == 2);
		uint64_t const stop = findStop(first);

		SUBCASE("Continue")
		{
			CHECK(fixture.Resume(VisualScriptDebugAction::Continue, stop).Result == VisualScriptResumeResult::Ended);
			CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
		}
		SUBCASE("Step Over")
		{
			CHECK(fixture.Resume(VisualScriptDebugAction::StepOver, stop).Result == VisualScriptResumeResult::Ended);
			CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::StepEnded);
		}
		CHECK(fixture.Context.GetScene().GetName() == "Second");
		CHECK(fixture.Session.GetState().Stops.empty());
		// The loaded scene stays paused, as the one it replaced was.
		CHECK(fixture.Play.IsPaused());
		CHECK(fixture.Session.HasPausedScene());
		CHECK(ended ==
		      std::vector<std::pair<AssetHandle, std::string>>{{graph, std::string(VisualScriptDebugSession::StopEndedBySceneLoad)}});
	}

	// No frame runs while the pause is held; Continue resumes play.
	uint64_t const frame = fixture.GetFrame();
	fixture.EditorFrame();
	CHECK(fixture.GetFrame() == frame);
	CHECK(fixture.Session.HasPausedScene());
	CHECK(fixture.Resume(VisualScriptDebugAction::Continue).Result == VisualScriptResumeResult::Resumed);
	CHECK_FALSE(fixture.Play.IsPaused());
	CHECK(ended.size() == 1);
}

TEST_CASE("VisualScriptDebug: a stop that ends in the frame that made it pauses play with the pause held, and play.advance says it "
          "stopped")
{
	SessionFixture fixture;
	std::vector<std::pair<AssetHandle, std::string>> ended;
	std::vector<uint64_t> notified;
	fixture.Session.SetStopEndedListener(
		[&ended](AssetHandle stopped, std::string const& reason)
		{
			ended.emplace_back(stopped, reason);
		});
	fixture.Session.SetStopListener(
		[&notified](VisualScriptStop const& stop)
		{
			notified.push_back(stop.ID);
		});
	AssetHandle const counter = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	// Made first, so its update runs (and stops) before the other entity's later in the frame.
	fixture.CreateRunner("Victim", counter);
	fixture.SetBreakpoint(counter, MakeBreakpoint(11));
	std::string reason;

	SUBCASE("a scene load the frame asked for")
	{
		std::filesystem::path const second = AssetManager::GetAssetDirectory() / "Scenes" / "Second.sscene";
		REQUIRE(FileSystem::CreateDirectories(second.parent_path()).IsOk());
		REQUIRE(SceneSerializer::SaveToFile(Scene("Second"), second).IsOk());
		REQUIRE(fixture.Operations.RefreshAssets().IsOk());
		AssetHandle const loader =
			fixture.CreateGraph("Scripts/Loader.sgraph", MakeCallDocument("Call:Strada.SceneManager.LoadScene(string)",
		                                                                  Json::object({{"Path", "Scenes/Second.sscene"}})));
		fixture.CreateRunner("Loader", loader);
		reason = VisualScriptDebugSession::StopEndedBySceneLoad;
	}
	SUBCASE("the stopped entity destroyed later in the frame")
	{
		AssetHandle const killer = fixture.CreateGraph("Scripts/Killer.sgraph", MakeDestroyByNameDocument("Victim"));
		fixture.CreateRunner("Killer", killer);
		reason = VisualScriptDebugSession::StopEndedByDestroy;
	}

	// Started paused, as an agent does: the frame play.advance runs stops, and play stays paused after it.
	fixture.Ok("play.start", Json::object({{"paused", true}}));
	Json const advanced = fixture.Ok("play.advance", Json::object({{"frames", 3}}));
	CHECK(advanced["framesRun"] == 1);
	CHECK(advanced["stopped"] == true);
	CHECK(advanced["stop"].is_null());
	CHECK(advanced["debug"]["state"] == "Running");
	CHECK(advanced["debug"]["stops"] == 0);
	CHECK(advanced["paused"] == true);
	CHECK(fixture.Play.GetDebugSummary().StopsMade == 1);
	CHECK(fixture.Play.GetDebugSummary().Holds == 1);
	// Nothing the session could show was made, so no stop was announced; the stop's graph is unknown.
	CHECK(notified.empty());
	CHECK(ended == std::vector<std::pair<AssetHandle, std::string>>{{AssetHandle(), reason}});
	CHECK(fixture.Session.GetBreakpoints().Find(counter, 11) != nullptr);
}

TEST_CASE("VisualScriptDebug: with break on errors an error stops read-only at the frame's end, and Resume and play.advance dismiss it")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Failing.sgraph", MakeFailingDocument());
	fixture.CreateRunner("Failing", graph);
	fixture.Session.SetBreakOnErrors(true);
	CHECK(fixture.Session.IsBreakOnErrors());
	fixture.Start();
	fixture.EditorFrame();
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	VisualScriptStop const error = fixture.GetCurrentStop();
	CHECK(error.Reason == VisualScriptStopReason::Error);
	CHECK_FALSE(error.Resumable);
	CHECK(error.Message.find("division by zero") != std::string::npos);
	CHECK(fixture.Play.IsPaused());
	CHECK(fixture.Session.HasPausedScene());
	// A read-only stop blocks nothing.
	CHECK_FALSE(fixture.Session.GetFrameStepRefusal().has_value());

	// Resuming play dismisses it.
	CHECK(fixture.Play.Resume());
	CHECK(fixture.Session.GetState().Stops.empty());
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	CHECK_FALSE(fixture.Play.IsPaused());

	// The next frame fails again; play.advance's first frame dismisses that stop and makes a new one, after which it stops.
	fixture.EditorFrame();
	REQUIRE(fixture.Session.GetState().Stops.size() == 1);
	uint64_t const second = fixture.GetCurrentStop().ID;
	CHECK(second > error.ID);
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 3}}))["framesRun"] == 1);
	REQUIRE(fixture.Session.GetState().Stops.size() == 1);
	CHECK(fixture.GetCurrentStop().ID > second);

	// Off, errors only fail: frames run on, the stop of the last frame dismissed.
	fixture.Session.SetBreakOnErrors(false);
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 3}}))["framesRun"] == 3);
	CHECK(fixture.Session.GetState().Stops.empty());
}

TEST_CASE("VisualScriptDebug: an edit while stopped reaches the stopped execution, which continues with the edited literal")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Literal.sgraph", MakeLiteralDocument(5));
	UUID const runner = fixture.CreateRunner("Runner", graph);
	fixture.SetBreakpoint(graph, MakeBreakpoint(11));
	fixture.Start();
	fixture.EditorFrame();
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	uint64_t const stop = fixture.GetCurrentStop().ID;
	CHECK_FALSE(fixture.GetCurrentStop().Migrated);
	VisualScriptStopValues const* before = fixture.Session.GetStopValues(stop, 0);
	REQUIRE(before != nullptr);
	// Read once per stop revision and frame.
	CHECK(fixture.Session.GetStopValues(stop, 0) == before);

	REQUIRE(fixture.Operations.SetVisualScriptDocument(graph, MakeLiteralDocument(50)).IsOk());
	fixture.Session.EndFrame();
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(fixture.GetCurrentStop().ID == stop);
	CHECK(fixture.GetCurrentStop().Migrated);
	VisualScriptStopValues const* values = fixture.Session.GetStopValues(stop, 0);
	REQUIRE(values != nullptr);
	auto const input = std::find_if(values->Node.Inputs.begin(), values->Node.Inputs.end(),
	                                [](VisualScriptDebugValue const& value)
	                                {
										return value.Name == "Value";
									});
	REQUIRE(input != values->Node.Inputs.end());
	CHECK(input->Edited);
	CHECK(input->Value == 50);
	CHECK(fixture.Session.GetStopValues(stop, 7) == nullptr);
	CHECK(fixture.Session.GetStopValues(stop + 100, 0) == nullptr);

	CHECK(fixture.ContinueAll().Result == VisualScriptResumeResult::Ended);
	CHECK(fixture.GetVariable(runner, "Count") == 50);
}

TEST_CASE("VisualScriptDebug: after a step, what the frame's end would do happens: destroyed entities go, a quit stops play, a "
          "loaded scene shows")
{
	SUBCASE("destroy")
	{
		SessionFixture fixture;
		AssetHandle const graph = fixture.CreateGraph("Scripts/Destroyer.sgraph", MakeCallDocument("Call:Strada.Entity.Destroy()"));
		UUID const destroyer = fixture.CreateRunner("Destroyer", graph);
		fixture.SetBreakpoint(graph, MakeBreakpoint(11));
		fixture.Start();
		fixture.EditorFrame();
		REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
		CHECK(fixture.Context.GetScene().HasEntity(destroyer));
		fixture.Resume(VisualScriptDebugAction::StepOver);
		CHECK_FALSE(fixture.Context.GetScene().HasEntity(destroyer));
		CHECK(fixture.GetFrame() == 1);
	}
	SUBCASE("quit")
	{
		SessionFixture fixture;
		AssetHandle const graph = fixture.CreateGraph("Scripts/Quitter.sgraph", MakeCallDocument("Call:Strada.Application.Quit()"));
		fixture.CreateRunner("Quitter", graph);
		fixture.SetBreakpoint(graph, MakeBreakpoint(11));
		fixture.Start();
		fixture.EditorFrame();
		REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
		fixture.Resume(VisualScriptDebugAction::StepOver);
		CHECK_FALSE(fixture.Play.IsPlaying());
		CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Detached);
		CHECK_FALSE(VisualScriptDebugger::IsAttached());
	}
	SUBCASE("scene load")
	{
		SessionFixture fixture;
		std::filesystem::path const second = AssetManager::GetAssetDirectory() / "Scenes" / "Second.sscene";
		REQUIRE(FileSystem::CreateDirectories(second.parent_path()).IsOk());
		REQUIRE(SceneSerializer::SaveToFile(Scene("Second"), second).IsOk());
		REQUIRE(fixture.Operations.RefreshAssets().IsOk());
		AssetHandle const graph =
			fixture.CreateGraph("Scripts/Loader.sgraph", MakeCallDocument("Call:Strada.SceneManager.LoadScene(string)",
		                                                                  Json::object({{"Path", "Scenes/Second.sscene"}})));
		fixture.CreateRunner("Loader", graph);
		fixture.SetBreakpoint(graph, MakeBreakpoint(11));
		fixture.Start();
		fixture.EditorFrame();
		REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
		uint64_t const version = fixture.Context.GetSceneVersion();
		CHECK(fixture.Resume(VisualScriptDebugAction::StepOver).Result == VisualScriptResumeResult::Ended);
		CHECK(fixture.Play.IsPlaying());
		CHECK(fixture.Context.GetSceneVersion() != version);
		CHECK(fixture.Context.GetScene().GetName() == "Second");
		CHECK(fixture.Session.GetState().Stops.empty());
		// The loaded scene stays paused as the step left the scene before it: the step ended its execution.
		CHECK(fixture.Play.IsPaused());
		CHECK(fixture.Session.HasPausedScene());
		CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::StepEnded);
		uint64_t const frame = fixture.GetFrame();
		fixture.EditorFrame();
		CHECK(fixture.GetFrame() == frame);
		CHECK(fixture.Resume(VisualScriptDebugAction::Continue).Result == VisualScriptResumeResult::Resumed);
		CHECK_FALSE(fixture.Play.IsPaused());
	}
}

TEST_CASE("VisualScriptDebug: every mutator pushes the configuration in its own call: frames run right after it see it")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	UUID const first = fixture.CreateRunner("First", graph);
	UUID const second = fixture.CreateRunner("Second", graph);
	std::vector<std::pair<AssetHandle, UUID>> targets;
	fixture.Session.SetTargetListener(
		[&targets](AssetHandle targeted, UUID entity)
		{
			targets.emplace_back(targeted, entity);
		});
	fixture.Ok("play.start", Json::object({{"paused", true}}));

	// A breakpoint added, then play.advance, with no editor frame between them: it stops both instances (any instance).
	fixture.SetBreakpoint(graph, MakeBreakpoint(11));
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 5}}))["framesRun"] == 1);
	CHECK(fixture.Session.GetState().Stops.size() == 2);
	CHECK(fixture.ContinueAll().Result == VisualScriptResumeResult::Ended);

	// Cleared, then play.advance: it runs on.
	fixture.Session.ClearBreakpoints(std::nullopt);
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 5}}))["framesRun"] == 5);

	// A target, the same way: only that instance stops.
	fixture.SetBreakpoint(graph, MakeBreakpoint(11));
	fixture.Session.SetTarget(graph, second);
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 5}}))["framesRun"] == 1);
	REQUIRE(fixture.Session.GetState().Stops.size() == 1);
	CHECK(fixture.GetCurrentStop().Entity == second);
	CHECK(fixture.GetVariable(first, "Count") == 7);
	CHECK(fixture.ContinueAll().Result == VisualScriptResumeResult::Ended);

	// Disabled breakpoints and break on errors reach it alike.
	std::array<uint32_t, 1> const nodes = {11};
	fixture.Session.SetBreakpointsEnabled(graph, nodes, false);
	CHECK_FALSE(fixture.Session.GetBreakpoints().Find(graph, 11)->Enabled);
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 2}}))["framesRun"] == 2);
	fixture.Session.SetBreakpointsEnabled(graph, nodes, true);
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 2}}))["framesRun"] == 1);
	CHECK(fixture.ContinueAll().Result == VisualScriptResumeResult::Ended);

	// Any instance again; an unchanged target tells nothing.
	fixture.Session.SetTarget(graph, UUID::Invalid());
	fixture.Session.SetTarget(graph, UUID::Invalid());
	CHECK_FALSE(fixture.Session.GetTarget(graph).IsValid());
	CHECK(targets == std::vector<std::pair<AssetHandle, UUID>>{{graph, second}, {graph, UUID::Invalid()}});
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 2}}))["framesRun"] == 1);
	CHECK(fixture.Session.GetState().Stops.size() == 2);
}

TEST_CASE("VisualScriptDebug: breakpoints are checked against the stored document and follow its edits")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	uint64_t const revision = fixture.Session.GetRevision();

	// The node's Type comes from the document.
	VisualScriptBreakpoint typed = MakeBreakpoint(11, "Count > 2", 4);
	typed.Type = "Flow.Branch";
	fixture.SetBreakpoint(graph, typed);
	REQUIRE(fixture.Session.GetBreakpoints().Find(graph, 11) != nullptr);
	CHECK(fixture.Session.GetBreakpoints().Find(graph, 11)->Type == "Variable.Set");
	CHECK(fixture.Session.GetBreakpoints().Find(graph, 11)->Condition == "Count > 2");
	CHECK(fixture.Session.GetRevision() > revision);
	Result<void> const missing = fixture.Session.SetBreakpoint(graph, MakeBreakpoint(99));
	REQUIRE(missing.IsError());
	CHECK(missing.GetError() == "Scripts/Counter.sgraph has no node 99");
	CHECK(fixture.Session.SetBreakpoint(AssetHandle(), MakeBreakpoint(11)).IsError());
	CHECK(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(14, std::string(2000, 'x'))).IsError());

	// F9 on a selection: nodes the document lacks are left out.
	std::array<uint32_t, 3> const selection = {11, 14, 99};
	fixture.Session.ToggleBreakpoints(graph, selection);
	CHECK(fixture.Session.GetBreakpoints().Get(graph).size() == 2);
	CHECK(fixture.Session.GetBreakpoints().Find(graph, 11)->Condition == "Count > 2");
	CHECK(fixture.Session.GetBreakpoints().Find(graph, 14)->Type == "Variable.Set");
	fixture.Session.ToggleBreakpoints(graph, selection);
	CHECK(fixture.Session.GetBreakpoints().IsEmpty());
	fixture.Session.ToggleBreakpoints(graph, selection);
	std::array<uint32_t, 1> const one = {14};
	fixture.Session.RemoveBreakpoints(graph, one);
	CHECK(fixture.Session.GetBreakpoints().Find(graph, 14) == nullptr);
	fixture.SetBreakpoint(graph, MakeBreakpoint(14));

	// A move keeps them.
	VisualScriptDocument moved = fixture.GetDocument(graph);
	moved.Graph.Nodes[1].Position = glm::vec2(900.0f, 300.0f);
	REQUIRE(fixture.Operations.SetVisualScriptDocument(graph, moved).IsOk());
	fixture.Session.EndFrame();
	CHECK(fixture.Session.GetBreakpoints().Get(graph).size() == 2);

	// Node 14 replaced by a node of another Type under its ID: its breakpoint goes, and the runtime is told while playing.
	fixture.Start();
	VisualScriptDocument retyped = moved;
	for (VisualScriptNode& node : retyped.Graph.Nodes)
	{
		if (node.ID == 14)
		{
			node = MakeNode(14, "Call:Strada.Log.Info(string)", Json::object(), Json::object({{"Message", "Hello"}}));
		}
	}
	std::erase_if(retyped.Graph.Links,
	              [](VisualScriptLink const& link)
	              {
					  return link.To.Node == 14 && link.To.Pin == "Value";
				  });
	REQUIRE(fixture.Operations.SetVisualScriptDocument(graph, retyped).IsOk());
	fixture.Session.EndFrame();
	CHECK(fixture.Session.GetBreakpoints().Find(graph, 14) == nullptr);
	CHECK(fixture.Session.GetBreakpoints().Find(graph, 11) != nullptr);
	Result<VisualScriptDebugState> const state = VisualScriptDebugger::GetState();
	REQUIRE(state.IsOk());
	CHECK(std::none_of(state.GetValue().Breakpoints.begin(), state.GetValue().Breakpoints.end(),
	                   [](VisualScriptBreakpointState const& breakpoint)
	                   {
						   return breakpoint.Node == 14;
					   }));
	fixture.Play.Stop();

	// A deleted graph loses them.
	fixture.Ok("asset.delete", Json::object({{"asset", graph.ToString()}}));
	fixture.Session.EndFrame();
	CHECK(fixture.Session.GetBreakpoints().IsEmpty());
}

TEST_CASE("VisualScriptDebug: breakpoints are per project: another project's are neither listed nor pushed, and come back with it")
{
	SessionFixture fixture;
	VisualScriptViewSettings views(fixture.Directory.GetPath() / "UserData" / "Editor" / "VisualScriptViews.json");
	fixture.Session.SetViewSettings(&views);
	std::filesystem::path const game = fixture.Context.GetProject()->GetFilePath();
	AssetHandle const counter = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.SetBreakpoint(counter, MakeBreakpoint(11, "Count > 2"));
	fixture.Session.SetBreakOnErrors(true);
	// Written into the views settings at once, under the project's file.
	CHECK(views.GetBreakpoints(game, counter).size() == 1);
	CHECK(views.GetBreakpoints(game, counter)[0].Condition == "Count > 2");
	CHECK(views.IsBreakOnErrors());

	// Another project: the first one's breakpoints are not listed, nor pushed when it plays.
	REQUIRE(fixture.Operations.CreateProject(fixture.Directory.GetPath() / "Other", "Other", Scene("Main")).IsOk());
	std::filesystem::path const other = fixture.Context.GetProject()->GetFilePath();
	CHECK(fixture.Session.GetBreakpoints().IsEmpty());
	AssetHandle const spinner = fixture.CreateGraph("Scripts/Spinner.sgraph", MakeCounterDocument());
	fixture.SetBreakpoint(spinner, MakeBreakpoint(14));
	fixture.Start();
	Result<VisualScriptDebugState> const state = VisualScriptDebugger::GetState();
	REQUIRE(state.IsOk());
	REQUIRE(state.GetValue().Breakpoints.size() == 1);
	CHECK(state.GetValue().Breakpoints[0].Document == spinner);
	fixture.Play.Stop();
	CHECK(views.GetBreakpointGraphs(game) == std::vector<AssetHandle>{counter});
	CHECK(views.GetBreakpointGraphs(other) == std::vector<AssetHandle>{spinner});

	// Back: its breakpoints are back, the other project's are gone.
	REQUIRE(fixture.Operations.OpenProject(game).IsOk());
	CHECK(fixture.Session.GetBreakpoints().GetGraphs() == std::vector<AssetHandle>{counter});
	CHECK(fixture.Session.GetBreakpoints().Find(counter, 11)->Condition == "Count > 2");
	fixture.Start();
	Result<VisualScriptDebugState> const back = VisualScriptDebugger::GetState();
	REQUIRE(back.IsOk());
	REQUIRE(back.GetValue().Breakpoints.size() == 1);
	CHECK(back.GetValue().Breakpoints[0].Document == counter);
	fixture.Play.Stop();

	// A graph the views settings drop loses its breakpoints in the session too.
	views.SetProject(game);
	views.ForgetGraph(counter);
	fixture.Session.EndFrame();
	CHECK(fixture.Session.GetBreakpoints().IsEmpty());

	// Headless (no views settings), breakpoints live in memory only: a project switch and back starts empty.
	fixture.Session.SetViewSettings(nullptr);
	fixture.SetBreakpoint(counter, MakeBreakpoint(11));
	REQUIRE(fixture.Operations.OpenProject(other).IsOk());
	CHECK(fixture.Session.GetBreakpoints().IsEmpty());
	REQUIRE(fixture.Operations.OpenProject(game).IsOk());
	CHECK(fixture.Session.GetBreakpoints().IsEmpty());
	CHECK(views.GetBreakpoints(game, counter).empty());
}

TEST_CASE("VisualScriptDebug: the flow clock advances only with scene frames, and each take drains the watched sub-graph's steps")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Counter", graph);
	fixture.Session.SetWatches({{graph, 0}});
	fixture.Ok("play.start", Json::object({{"paused", true}}));
	CHECK(fixture.Session.GetFlowTime() == 0.0);

	CHECK(fixture.Advance(5) == 5);
	double const time = 5.0 * static_cast<double>(FrameTime);
	CHECK(fixture.Session.GetFlowTime() == doctest::Approx(time));
	// Paused editor frames hold the clock.
	for (int frame = 0; frame < 3; frame++)
	{
		fixture.EditorFrame();
	}
	CHECK(fixture.Session.GetFlowTime() == doctest::Approx(time));

	VisualScriptFlowTake const take = fixture.Session.TakeFlow(graph, 0);
	CHECK(take.Frames == 5);
	CHECK_FALSE(take.Held);
	CHECK(take.Time == doctest::Approx(time));
	auto const count = [&take](uint32_t node)
	{
		uint32_t total = 0;
		for (VisualScriptFlowStep const& step : take.Steps)
		{
			total += step.Node == node ? step.Count : 0;
		}
		return total;
	};
	CHECK(count(10) == 5);
	CHECK(count(11) == 5);
	CHECK(count(14) == 5);
	REQUIRE(!take.Steps.empty());
	for (size_t index = 0; index < take.Steps.size(); index++)
	{
		CHECK(take.Steps[index].FirstOrder == index);
		CHECK(take.Steps[index].LastTime == doctest::Approx(time));
	}

	// Drained: nothing ran since, and the clock held.
	VisualScriptFlowTake const again = fixture.Session.TakeFlow(graph, 0);
	CHECK(again.Steps.empty());
	CHECK(again.Frames == 0);
	CHECK(again.Held);
	CHECK(fixture.Advance(1) == 1);
	VisualScriptFlowTake const next = fixture.Session.TakeFlow(graph, 0);
	CHECK(next.Frames == 1);
	REQUIRE(!next.Steps.empty());
	CHECK(next.Steps.front().LastTime == doctest::Approx(6.0 * static_cast<double>(FrameTime)));
	// A sub-graph no window watches has no flow.
	VisualScriptFlowTake const unwatched = fixture.Session.TakeFlow(graph, 7);
	CHECK(unwatched.Steps.empty());
	CHECK(unwatched.Held);

	// Playing again starts the clock over.
	fixture.Play.Stop();
	fixture.Start(true);
	CHECK(fixture.Session.GetFlowTime() == 0.0);
}

TEST_CASE("VisualScriptDebug: a wait answers at once when it can, else on a stop, a timeout or the end of play, and Cancelled when "
          "abandoned")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Counter", graph);

	CompletionProbe idle;
	fixture.Session.WaitForStop(1.0, idle.Make());
	CHECK(idle.Value() == Json::object({{"stopped", false}, {"stop", nullptr}, {"reason", "NotPlaying"}, {"hint", nullptr}}));

	// A paused scene with nothing stopped runs no frame.
	fixture.Start(true);
	CompletionProbe paused;
	fixture.Session.WaitForStop(10.0, paused.Make());
	CHECK(paused.Value()["reason"] == "Paused");
	CHECK(paused.Value()["hint"] == "the scene is paused: play.advance runs frames");

	// Running: the wait ends with the stop.
	REQUIRE(fixture.Play.SetPaused(false));
	fixture.SetBreakpoint(graph, MakeBreakpoint(11));
	CompletionProbe stopped;
	fixture.Session.WaitForStop(10.0, stopped.Make());
	CHECK_FALSE(stopped.Result.has_value());
	fixture.EditorFrame();
	CHECK(stopped.Value() ==
	      Json::object({{"stopped", true}, {"stop", fixture.GetCurrentStop().ID}, {"reason", "Stopped"}, {"hint", nullptr}}));
	CompletionProbe atOnce;
	fixture.Session.WaitForStop(10.0, atOnce.Make());
	CHECK(atOnce.Value()["reason"] == "Stopped");

	// Nothing stops: a real-time timeout.
	fixture.Session.ClearBreakpoints(std::nullopt);
	CHECK(fixture.ContinueAll().Result == VisualScriptResumeResult::Ended);
	CompletionProbe timeout;
	fixture.Session.WaitForStop(0.1, timeout.Make());
	auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!timeout.Result && std::chrono::steady_clock::now() < deadline)
	{
		fixture.EditorFrame();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(timeout.Value()["reason"] == "Timeout");

	// Play stops.
	CompletionProbe ended;
	fixture.Session.WaitForStop(10.0, ended.Make());
	fixture.Play.Stop();
	CHECK(ended.Value()["reason"] == "PlayStopped");

	// Abandoned: a session that goes with its wait waiting.
	{
		Scope<VisualScriptDebugSession> abandoned = CreateScope<VisualScriptDebugSession>(fixture.Context, fixture.Play);
		fixture.Play.SetObserver(abandoned.get());
		fixture.Start();
		CompletionProbe cancelled;
		abandoned->WaitForStop(10.0, cancelled.Make());
		CHECK_FALSE(cancelled.Result.has_value());
		fixture.Play.SetObserver(&fixture.Session);
		abandoned.reset();
		REQUIRE(cancelled.Result.has_value());
		REQUIRE(cancelled.Result->IsError());
		CHECK(cancelled.Result->GetError().Code == AutomationErrorCode::Cancelled);
		CHECK_FALSE(VisualScriptDebugger::IsAttached());
	}
}

TEST_CASE("VisualScriptDebug: the UI's resumes wait for the next BeginFrame, and Only This Instance targets the stop's entity")
{
	SessionFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	UUID const first = fixture.CreateRunner("First", graph);
	UUID const second = fixture.CreateRunner("Second", graph);
	fixture.SetBreakpoint(graph, MakeBreakpoint(11));
	fixture.Start();
	fixture.EditorFrame();
	REQUIRE(fixture.Session.GetState().Stops.size() == 2);
	auto const stopOf = [&fixture](UUID entity)
	{
		for (VisualScriptStop const& stop : fixture.Session.GetState().Stops)
		{
			if (stop.Entity == entity)
			{
				return stop.ID;
			}
		}
		FAIL("no stop of the entity");
		return uint64_t(0);
	};
	uint64_t const firstStop = stopOf(first);
	uint64_t const secondStop = stopOf(second);

	// The stop and frame shown.
	fixture.Session.SelectStop(secondStop);
	fixture.Session.SelectFrame(0);
	CHECK(fixture.Session.GetSelectedStop() == secondStop);
	fixture.Session.SelectStop(12345);
	CHECK(fixture.Session.GetSelectedStop() == secondStop);

	// The remedy for a breakpoint that stopped many instances: the target at once, the other stops at the next BeginFrame.
	fixture.Session.RequestOnlyInstance(firstStop);
	CHECK(fixture.Session.GetTarget(graph) == first);
	CHECK(fixture.Session.GetState().Stops.size() == 2);
	CHECK(fixture.GetVariable(second, "Count") == 0);
	fixture.Session.BeginFrame();
	REQUIRE(fixture.Session.GetState().Stops.size() == 1);
	CHECK(fixture.GetCurrentStop().ID == firstStop);
	CHECK(fixture.GetVariable(second, "Count") == 1);
	CHECK(fixture.GetVariable(first, "Count") == 0);
	// The selected stop ended: the current one is shown.
	CHECK(fixture.Session.GetSelectedStop() == firstStop);

	// The toolbar's unpause while stopped: Continue of every stop, then the unpause, at the next BeginFrame.
	fixture.Session.RequestResume(std::nullopt, VisualScriptDebugAction::Continue, true, true);
	CHECK(fixture.Session.GetState().Stops.size() == 1);
	CHECK(fixture.Play.IsPaused());
	fixture.Session.BeginFrame();
	CHECK(fixture.Session.GetState().Stops.empty());
	CHECK_FALSE(fixture.Play.IsPaused());
	CHECK(fixture.GetVariable(first, "Count") == 1);

	// Now only the target stops; F10's queued step runs at the next BeginFrame.
	fixture.EditorFrame();
	REQUIRE(fixture.Session.GetState().Stops.size() == 1);
	CHECK(fixture.GetCurrentStop().Entity == first);
	CHECK(fixture.GetVariable(second, "Count") == 2);
	fixture.Session.RequestResume(std::nullopt, VisualScriptDebugAction::StepOver);
	CHECK(fixture.GetCurrentStop().Frames.front().Node == 11);
	fixture.Session.BeginFrame();
	CHECK(fixture.GetCurrentStop().Frames.front().Node == 14);
	// A request the state already answered is dropped.
	fixture.Session.RequestResume(std::nullopt, VisualScriptDebugAction::Continue, true);
	fixture.Session.RequestResume(std::nullopt, VisualScriptDebugAction::Continue, true);
	fixture.Session.BeginFrame();
	CHECK(fixture.Session.GetState().Stops.empty());
	CHECK_FALSE(fixture.Play.IsPaused());
}

TEST_CASE("VisualScriptDebug: Break stops the next stop point of one graph, and Run to Node arms a one-shot breakpoint")
{
	SessionFixture fixture;
	AssetHandle const counter = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	AssetHandle const other = fixture.CreateGraph("Scripts/Other.sgraph", MakeCounterDocument());
	UUID const first = fixture.CreateRunner("First", counter);
	UUID const second = fixture.CreateRunner("Second", other);
	CHECK(fixture.Session.Break().IsError());
	fixture.Start(true);

	// Break for one graph: the other graph's update runs on.
	REQUIRE(fixture.Session.Break(false, other).IsOk());
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	CHECK(fixture.Advance(3) == 1);
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(fixture.GetCurrentStop().Reason == VisualScriptStopReason::Break);
	CHECK(fixture.GetCurrentStop().Graph == other);
	CHECK(fixture.GetCurrentStop().Entity == second);
	CHECK(fixture.GetVariable(first, "Count") == 1);
	CHECK(fixture.ContinueAll().Result == VisualScriptResumeResult::Ended);

	// Run to Node with nothing stopped only arms it; it stops once.
	CHECK(GetError(fixture.Session.RunTo(counter, 99)) == "Scripts/Counter.sgraph has no node 99");
	Result<VisualScriptResumeOutcome> const armed = fixture.Session.RunTo(counter, 14);
	REQUIRE(armed.IsOk());
	CHECK(armed.GetValue().Result == VisualScriptResumeResult::Armed);
	CHECK(fixture.Advance(3) == 1);
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(fixture.GetCurrentStop().Reason == VisualScriptStopReason::RunTo);
	CHECK(fixture.GetCurrentStop().Frames.front().Node == 14);
	CHECK(fixture.GetCurrentStop().Entity == first);
	// While stopped, Run to Node continues every stop.
	Result<VisualScriptResumeOutcome> const ran = fixture.Session.RunTo(counter, 11);
	REQUIRE(ran.IsOk());
	CHECK(ran.GetValue().Result == VisualScriptResumeResult::Ended);
	CHECK(fixture.Advance(3) == 1);
	CHECK(fixture.GetCurrentStop().Frames.front().Node == 11);
	CHECK(fixture.ContinueAll().Result == VisualScriptResumeResult::Ended);
	CHECK(fixture.Advance(3) == 3);

	// Break, cancelled before a frame, stops nothing.
	REQUIRE(fixture.Session.Break().IsOk());
	REQUIRE(fixture.Session.Break(true).IsOk());
	CHECK(fixture.Advance(3) == 3);
	CHECK(fixture.Session.GetState().Stops.empty());
}
