#include "Fuzzing.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Log.h"
#include "Strada/Project/SceneRunner.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptEngineInternal.h"
#include "Strada/Script/VisualScriptDebugger.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object(), Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	// Appends ",<label>" to the name of the graph's Recorder (variable 1) at the node set (Set:Strada.Entity.Name), whose
	// helpers take the three IDs below it; the caller links its Execute and Then.
	void AddRecord(VisualScriptDocument& document, uint32_t set, std::string const& label)
	{
		document.Graph.Nodes.push_back(MakeNode(set - 3, "Variable.Get", Json::object({{"Variable", 1}})));
		document.Graph.Nodes.push_back(MakeNode(set - 2, "Get:Strada.Entity.Name"));
		document.Graph.Nodes.push_back(MakeNode(set - 1, "FormatString", Json::object({{"Format", "{Old}," + label}})));
		document.Graph.Nodes.push_back(MakeNode(set, "Set:Strada.Entity.Name"));
		document.Graph.Links.push_back(MakeLink(set - 3, "Value", set - 2, "Target"));
		document.Graph.Links.push_back(MakeLink(set - 2, "Value", set - 1, "Old"));
		document.Graph.Links.push_back(MakeLink(set - 3, "Value", set, "Target"));
		document.Graph.Links.push_back(MakeLink(set - 1, "Result", set, "Value"));
	}

	// On every update: records "<prefix>1" (node 23), records "<prefix>2" (node 33, where the tests break), then moves its
	// entity to (1, 2, 3) (node 40).
	VisualScriptDocument MakeStepperDocument(std::string const& prefix)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Recorder", "Strada.Entity"));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		AddRecord(document, 23, prefix + "1");
		AddRecord(document, 33, prefix + "2");
		document.Graph.Nodes.push_back(
			MakeNode(40, "Set:Strada.Entity.Translation", Json::object(), Json::object({{"Value", {1.0, 2.0, 3.0}}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 23, "Execute"));
		document.Graph.Links.push_back(MakeLink(23, "Then", 33, "Execute"));
		document.Graph.Links.push_back(MakeLink(33, "Then", 40, "Execute"));
		return document;
	}

	// Records the label on every update.
	VisualScriptDocument MakeUpdateRecorderDocument(std::string const& label)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Recorder", "Strada.Entity"));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		AddRecord(document, 23, label);
		document.Graph.Links.push_back(MakeLink(10, "Then", 23, "Execute"));
		return document;
	}

	// When created, sets Count (public, 0) to 1 / Zero (private, 0) at node 11: an aborting error once it runs.
	VisualScriptDocument MakeFailingDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
		document.Variables.push_back(MakeVariable(2, "Zero", "int", 0, false));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Operator.Divide", Json::object(), Json::object({{"A", 1}})));
		document.Graph.Nodes.push_back(MakeNode(13, "Variable.Get", Json::object({{"Variable", 2}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(13, "Value", 12, "B"));
		document.Graph.Links.push_back(MakeLink(12, "Result", 11, "Value"));
		return document;
	}

	// On every update: a For Loop over 0 … 2 adds each Index to Total (node 12, private), then a Delay of 0.05 s (node 15),
	// then Health (public float) becomes 5 (node 16). Its variables have a value of each kind the debugger encodes (arrays are
	// private: components cannot set them).
	VisualScriptDocument MakeRichDocument()
	{
		Json items = Json::array();
		for (int value = 0; value < 40; value++)
		{
			items.push_back(value);
		}
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Health", "float", 10.0));
		document.Variables.push_back(MakeVariable(2, "Title", "string", std::string(300, 'x')));
		document.Variables.push_back(MakeVariable(3, "Items", "int[]", items, false));
		document.Variables.push_back(MakeVariable(4, "Tint", "Strada.Color", {0.25, 0.5, 1.0, 1.0}));
		document.Variables.push_back(MakeVariable(5, "Partner", "Strada.Entity"));
		document.Variables.push_back(MakeVariable(6, "Total", "int", 0, false));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		document.Graph.Nodes.push_back(MakeNode(11, "Flow.ForLoop", Json::object(), Json::object({{"First", 0}, {"Last", 2}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Variable.Set", Json::object({{"Variable", 6}})));
		document.Graph.Nodes.push_back(MakeNode(13, "Operator.Add"));
		document.Graph.Nodes.push_back(MakeNode(14, "Variable.Get", Json::object({{"Variable", 6}})));
		document.Graph.Nodes.push_back(MakeNode(15, "Flow.Delay", Json::object(), Json::object({{"Duration", 0.05}})));
		document.Graph.Nodes.push_back(MakeNode(16, "Variable.Set", Json::object({{"Variable", 1}}), Json::object({{"Value", 5.0}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(11, "Body", 12, "Execute"));
		document.Graph.Links.push_back(MakeLink(14, "Value", 13, "A"));
		document.Graph.Links.push_back(MakeLink(11, "Index", 13, "B"));
		document.Graph.Links.push_back(MakeLink(13, "Result", 12, "Value"));
		document.Graph.Links.push_back(MakeLink(11, "Completed", 15, "Execute"));
		document.Graph.Links.push_back(MakeLink(15, "Completed", 16, "Execute"));
		return document;
	}

	AssetHandle AddGraph(VisualScriptDocument document, std::string name)
	{
		return AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(std::move(document)), std::move(name));
	}

	Entity AddVisualScript(Entity entity, AssetHandle graph, ScriptFieldMap variables = {})
	{
		VisualScriptComponent& component = entity.AddComponent<VisualScriptComponent>();
		component.Graph = graph;
		component.Variables = std::move(variables);
		return entity;
	}

	// An entity whose name collects what recorder graphs and Lifecycle scripts record.
	Entity AddRecorder(Scene& scene)
	{
		Entity recorder = scene.CreateEntity("Recorder");
		recorder.GetComponent<TagComponent>().Tag.clear();
		return recorder;
	}

	ScriptFieldMap RecorderVariables(Entity recorder)
	{
		return {{"Recorder", ScriptFieldValue::FromEntity(recorder.GetUUID())}};
	}

	// The records in the recorder's name, in order.
	std::vector<std::string> GetRecords(Entity recorder)
	{
		std::vector<std::string> records;
		std::string const& text = recorder.GetName();
		size_t start = 0;
		while (start < text.size())
		{
			size_t const end = std::min(text.find(',', start), text.size());
			if (end > start)
			{
				records.push_back(text.substr(start, end - start));
			}
			start = end + 1;
		}
		return records;
	}

	VisualScriptBreakpointSetting MakeBreakpoint(AssetHandle document, uint32_t node, std::string type, std::string condition = {})
	{
		VisualScriptBreakpointSetting breakpoint;
		breakpoint.Document = document;
		breakpoint.Node = node;
		breakpoint.Type = std::move(type);
		breakpoint.Condition = std::move(condition);
		return breakpoint;
	}

	void Configure(std::vector<VisualScriptBreakpointSetting> breakpoints, std::vector<VisualScriptDebugWatch> watches = {},
	               uint32_t revision = 1)
	{
		VisualScriptDebugConfiguration configuration;
		configuration.Revision = revision;
		configuration.Breakpoints = std::move(breakpoints);
		configuration.Watches = std::move(watches);
		Result<void> const configured = VisualScriptDebugger::Configure(configuration);
		REQUIRE_MESSAGE(configured.IsOk(), configured.GetError());
	}

	VisualScriptDebugState GetState()
	{
		Result<VisualScriptDebugState> state = VisualScriptDebugger::GetState();
		REQUIRE_MESSAGE(state.IsOk(), state.GetError());
		return state.TakeValue();
	}

	// The runtime's own reply to a request, as the facade receives it.
	std::string SendRaw(std::string const& request)
	{
		ScriptEngineInternal::ScriptEngineData const* const data = ScriptEngineInternal::GetData();
		REQUIRE(data != nullptr);
		std::string reply;
		auto const status = static_cast<ScriptEngineInternal::ScriptStatus>(data->Host.VisualScriptDebug(
			request.data(), ScriptEngineInternal::ToLength(request.size()), &reply, &ScriptEngineInternal::ReceiveText));
		REQUIRE(status == ScriptEngineInternal::ScriptStatus::Success);
		return reply;
	}

	// The trace block the runtime answers; none while no debugger is attached.
	std::optional<std::vector<std::byte>> TakeRawTrace()
	{
		ScriptEngineInternal::ScriptEngineData const* const data = ScriptEngineInternal::GetData();
		REQUIRE(data != nullptr);
		std::string block;
		auto const status =
			static_cast<ScriptEngineInternal::ScriptStatus>(data->Host.VisualScriptTakeTrace(&block, &ScriptEngineInternal::ReceiveText));
		if (status == ScriptEngineInternal::ScriptStatus::NotFound)
		{
			return std::nullopt;
		}
		REQUIRE(status == ScriptEngineInternal::ScriptStatus::Success);
		std::span<std::byte const> const bytes = std::as_bytes(std::span<char const>(block.data(), block.size()));
		return std::vector<std::byte>(bytes.begin(), bytes.end());
	}

	// Counts the script exceptions the application is told of.
	struct ExceptionCounter final : ScriptHost
	{
		ExceptionCounter() { ScriptEngine::SetHost(this); }
		~ExceptionCounter() override { ScriptEngine::SetHost(nullptr); }

		ExceptionCounter(ExceptionCounter const&) = delete;
		ExceptionCounter& operator=(ExceptionCounter const&) = delete;

		bool IsEditor() const override { return true; }
		void RequestSceneLoad(AssetHandle) override {}
		void RequestQuit() override {}
		void ReportTestResult(ScriptTestResult const&) override {}
		void FinishTests() override {}
		void OnScriptException() override { Exceptions++; }

		uint32_t Exceptions = 0;
	};

	VisualScriptGraphTrace const* FindGraph(VisualScriptFrameTrace const& trace, AssetHandle document)
	{
		auto const graph = std::find_if(trace.Graphs.begin(), trace.Graphs.end(),
		                                [document](VisualScriptGraphTrace const& candidate)
		                                {
											return candidate.Document == document && candidate.Function == 0;
										});
		return graph != trace.Graphs.end() ? &*graph : nullptr;
	}

	// How often the flow left the node through the exit (empty: without one); through any exit when none is named.
	uint32_t CountSteps(VisualScriptGraphTrace const& graph, uint32_t node, std::optional<std::string_view> exit = std::nullopt)
	{
		uint32_t count = 0;
		for (VisualScriptTraceStep const& step : graph.Steps)
		{
			if (step.Node == node && (!exit || step.Exit == *exit))
			{
				count += step.Count;
			}
		}
		return count;
	}

	VisualScriptDebugValue const& FindValue(std::vector<VisualScriptDebugValue> const& values, std::string_view name)
	{
		auto const value = std::find_if(values.begin(), values.end(),
		                                [name](VisualScriptDebugValue const& candidate)
		                                {
											return candidate.Name == name;
										});
		REQUIRE_MESSAGE(value != values.end(), name);
		return *value;
	}
}

TEST_CASE("VisualScriptDebug: the debugger is attached only when asked, and a scene runner never attaches it")
{
	// Without the scripting runtime there is nothing to attach.
	CHECK_FALSE(VisualScriptDebugger::IsAttached());
	CHECK(VisualScriptDebugger::Attach().IsError());
	CHECK(VisualScriptDebugger::Detach() == 0);
	CHECK(VisualScriptDebugger::GetState().GetError() == "the scripting runtime is not initialized");

	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	CHECK_FALSE(VisualScriptDebugger::IsAttached());
	CHECK(VisualScriptDebugger::GetState().GetError() == "NotAttached: no debugger is attached");
	CHECK(VisualScriptDebugger::TakeTrace().GetError() == "NotAttached: no debugger is attached");

	// A run of the game player's kind: its graphs run untraced.
	AssetHandle const graph = AddGraph(MakeStepperDocument("A"), "Stepper");
	Ref<Scene> const scene = CreateRef<Scene>("Runner");
	Entity const recorder = AddRecorder(*scene);
	Entity const stepper = AddVisualScript(scene->CreateEntity("Stepper"), graph, RecorderVariables(recorder));
	{
		SceneRunner runner({});
		runner.Start(scene);
		runner.Update(Timestep(FrameTime));
		runner.Update(Timestep(FrameTime));
		CHECK_FALSE(VisualScriptDebugger::IsAttached());
		CHECK_FALSE(TakeRawTrace().has_value());
		CHECK(GetRecords(recorder) == std::vector<std::string>{"A1", "A2", "A1", "A2"});

		// Values and instances need no debugger.
		VisualScriptInstanceQuery query;
		query.Graph = graph;
		Result<VisualScriptInstanceList> const instances = VisualScriptDebugger::DescribeInstances(query);
		REQUIRE_MESSAGE(instances.IsOk(), instances.GetError());
		REQUIRE(instances.GetValue().Instances.size() == 1);
		CHECK(instances.GetValue().Total == 1);
		CHECK(instances.GetValue().Instances[0].Entity == stepper.GetUUID());
		CHECK(instances.GetValue().Instances[0].State == VisualScriptInstanceState::Idle);
		VisualScriptValueRequest request;
		request.Entity = stepper.GetUUID();
		request.Variables = true;
		Result<VisualScriptLiveValues> const values = VisualScriptDebugger::ReadValues(request);
		REQUIRE_MESSAGE(values.IsOk(), values.GetError());
		CHECK(values.GetValue().Source == VisualScriptValueSource::Frame);
		REQUIRE(values.GetValue().Variables.size() == 1);
		CHECK(values.GetValue().Variables[0].Entity == recorder.GetUUID());
		request.Entity = UUID(12345);
		CHECK(VisualScriptDebugger::ReadValues(request).GetError() == "NotFound: Entity: no instance runs on the entity");
		runner.Stop();
	}

	REQUIRE(VisualScriptDebugger::Attach().IsOk());
	CHECK(VisualScriptDebugger::IsAttached());
	REQUIRE(VisualScriptDebugger::Attach().IsOk());
	// The runtime's refusals come back with their code.
	VisualScriptDebugConfiguration twice;
	twice.Breakpoints = {MakeBreakpoint(graph, 33, "Set:Strada.Entity.Name"), MakeBreakpoint(graph, 33, "Set:Strada.Entity.Name")};
	CHECK(VisualScriptDebugger::Configure(twice).GetError() == "InvalidRequest: Breakpoints[1]: a second breakpoint of the node");
	CHECK(VisualScriptDebugger::RunTo(graph, 999).GetError().starts_with("InvalidState: Node: no loaded program has the node 999"));
	CHECK(VisualScriptDebugger::DescribeStop(1, 0).GetError() == "NotFound: Stop: no stop of the ID");
	CHECK(VisualScriptDebugger::MarkRecordingFrame(1).GetError() == "InvalidState: no recording runs: Record Start begins one");
	VisualScriptDebugState const state = GetState();
	CHECK(state.Stops.empty());
	CHECK_FALSE(state.Current.has_value());
	CHECK_FALSE(state.Stepping.has_value());
	CHECK(TakeRawTrace().has_value());
	CHECK(VisualScriptDebugger::Detach() == 0);
	CHECK_FALSE(VisualScriptDebugger::IsAttached());
	CHECK(VisualScriptDebugger::Detach() == 0);
}

TEST_CASE("VisualScriptDebug: a stopped execution lets the scene finish its frame, and continuing between frames runs the rest "
          "against the live scene")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	AssetHandle const stepperGraph = AddGraph(MakeStepperDocument("A"), "Stepper");
	AssetHandle const otherGraph = AddGraph(MakeUpdateRecorderDocument("B"), "Other");
	Scene scene("Frame");
	Entity const recorder = AddRecorder(scene);
	// Visual scripts update in the order of their components, the one added last first: the other graph after the stepper.
	AddVisualScript(scene.CreateEntity("Other"), otherGraph, RecorderVariables(recorder));
	Entity const stepper = AddVisualScript(scene.CreateEntity("Stepper"), stepperGraph, RecorderVariables(recorder));
	ScriptComponent& lifecycle = scene.CreateEntity("Lifecycle").AddComponent<ScriptComponent>();
	lifecycle.ClassName = "Strada.Tests.Lifecycle";
	lifecycle.Fields = {{"Label", ScriptFieldValue::FromString("C")}, {"Recorder", ScriptFieldValue::FromEntity(recorder.GetUUID())}};

	REQUIRE(VisualScriptDebugger::Attach().IsOk());
	Configure({MakeBreakpoint(stepperGraph, 33, "Set:Strada.Entity.Name")});
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(FrameTime));

	// The stepper stopped after its first record; the rest of the frame ran: the other graph's update after it, and the C#
	// script's fixed update after the visual scripts' updates.
	CHECK(GetRecords(recorder) == std::vector<std::string>{"C:Create", "C:Update", "A1", "B", "C:Fixed"});
	CHECK(stepper.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
	VisualScriptDebugState const stopped = GetState();
	REQUIRE(stopped.Stops.size() == 1);
	VisualScriptStop const& stop = stopped.Stops[0];
	CHECK(stopped.Current == stop.ID);
	CHECK(stop.Reason == VisualScriptStopReason::Breakpoint);
	CHECK(stop.Resumable);
	CHECK(stop.Entity == stepper.GetUUID());
	CHECK(stop.Graph == stepperGraph);
	CHECK(stop.Place == VisualScriptStopPlace::Enter);
	CHECK(stop.Breakpoint == std::make_pair(stepperGraph, uint32_t{33}));
	REQUIRE(stop.Frames.size() == 1);
	CHECK(stop.Frames[0].Kind == VisualScriptStopFrameKind::Graph);
	CHECK(stop.Frames[0].Node == 33);
	CHECK(stop.Frames[0].Pin == "Execute");
	CHECK(stop.Frames[0].Document == stepperGraph);
	CHECK(stop.Start.Kind == VisualScriptStartKind::Event);
	CHECK(stop.Start.Node == 10);
	CHECK_FALSE(stop.Start.Origin.has_value());
	REQUIRE(stopped.Breakpoints.size() == 1);
	CHECK(stopped.Breakpoints[0].Hits == 1);
	CHECK(stopped.Breakpoints[0].Active);

	// Its instance is stopped, and its stop is listed with it.
	VisualScriptInstanceQuery query;
	query.Entity = stepper.GetUUID();
	Result<VisualScriptInstanceList> const instances = VisualScriptDebugger::DescribeInstances(query);
	REQUIRE_MESSAGE(instances.IsOk(), instances.GetError());
	REQUIRE(instances.GetValue().Instances.size() == 1);
	CHECK(instances.GetValue().Instances[0].State == VisualScriptInstanceState::Stopped);
	CHECK(instances.GetValue().Instances[0].Stops == std::vector<uint64_t>{stop.ID});

	// Between frames, continuing runs the rest of the execution in the scene as it is now.
	Result<VisualScriptResumeOutcome> const resumed = VisualScriptDebugger::Resume(std::nullopt, VisualScriptDebugAction::Continue);
	REQUIRE_MESSAGE(resumed.IsOk(), resumed.GetError());
	CHECK(resumed.GetValue().Result == VisualScriptResumeResult::Ended);
	CHECK_FALSE(resumed.GetValue().Failed);
	CHECK_FALSE(resumed.GetValue().Stop.has_value());
	CHECK(resumed.GetValue().StopRevision != stopped.StopRevision);
	CHECK(GetRecords(recorder) == std::vector<std::string>{"C:Create", "C:Update", "A1", "B", "C:Fixed", "A2"});
	CHECK(stepper.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
	CHECK(GetState().Stops.empty());
	CHECK(VisualScriptDebugger::Resume(std::nullopt, VisualScriptDebugAction::Continue).GetError() == "NotFound: no execution is stopped");

	// Without the breakpoint the next frame runs through.
	Configure({}, {}, 2);
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(GetState().Stops.empty());
	CHECK(GetState().Revision == 2);
	CHECK(GetRecords(recorder).size() == 11);

	// A Break of the stepper's graph stops its next execution at its first stop point; a cancelled one stops nothing.
	REQUIRE(VisualScriptDebugger::Break(false, stepperGraph).IsOk());
	scene.OnUpdateRuntime(Timestep(FrameTime));
	VisualScriptDebugState const broken = GetState();
	REQUIRE(broken.Stops.size() == 1);
	CHECK(broken.Stops[0].Reason == VisualScriptStopReason::Break);
	CHECK(broken.Stops[0].Graph == stepperGraph);
	REQUIRE(VisualScriptDebugger::Resume(std::nullopt, VisualScriptDebugAction::Continue, true).IsOk());
	REQUIRE(VisualScriptDebugger::Break(false).IsOk());
	REQUIRE(VisualScriptDebugger::Break(true).IsOk());
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(GetState().Stops.empty());
	scene.OnRuntimeStop();
	CHECK(VisualScriptDebugger::Detach() == 0);
}

TEST_CASE("VisualScriptDebug: an Event.Create that Entity.Instantiate starts passes its breakpoint with a snapshot naming the "
          "engine code it runs in")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	VisualScriptDocument child;
	child.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
	child.Graph.Nodes.push_back(MakeNode(11, "Set:Strada.Entity.Name", Json::object(), Json::object({{"Value", "Spawned"}})));
	child.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	AssetHandle const childGraph = AddGraph(child, "Child");
	Scene source("Source");
	Entity const member = AddVisualScript(source.CreateEntity("Member"), childGraph);
	Result<Ref<PrefabAsset>> prefabAsset = PrefabAsset::Create(PrefabSerializer::Serialize(source, member));
	REQUIRE(prefabAsset.IsOk());
	AssetHandle const prefab = AssetManager::AddMemoryAsset(prefabAsset.GetValue(), "Member");

	VisualScriptDocument spawner;
	spawner.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
	spawner.Graph.Nodes.push_back(MakeNode(11,
	                                       "Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)",
	                                       Json::object(), Json::object({{"Prefab", prefab.ToString()}})));
	spawner.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	AssetHandle const spawnerGraph = AddGraph(spawner, "Spawner");
	Scene scene("Spawning");
	AddVisualScript(scene.CreateEntity("Spawner"), spawnerGraph);

	REQUIRE(VisualScriptDebugger::Attach().IsOk());
	Configure({MakeBreakpoint(childGraph, 11, "Set:Strada.Entity.Name")});
	scene.OnRuntimeStart();

	// The spawned entity's Event.Create ran inside the spawner's call: it could not stop, so it ran on, and its breakpoint
	// records a read-only stop.
	Entity const spawned = scene.FindEntityByName("Spawned");
	REQUIRE(spawned);
	VisualScriptDebugState const state = GetState();
	REQUIRE(state.Stops.size() == 1);
	VisualScriptStop const& stop = state.Stops[0];
	CHECK(stop.Reason == VisualScriptStopReason::Passed);
	CHECK_FALSE(stop.Resumable);
	CHECK(stop.Entity == spawned.GetUUID());
	CHECK(stop.Graph == childGraph);
	CHECK(stop.Message == "it runs nested in engine code another execution called");
	// Innermost first: the child's frame, the engine code, then the spawner's call.
	REQUIRE(stop.Frames.size() == 3);
	CHECK(stop.Frames[0].Kind == VisualScriptStopFrameKind::Graph);
	CHECK(stop.Frames[0].Graph == childGraph);
	CHECK(stop.Frames[0].Node == 11);
	CHECK(stop.Frames[1].Kind == VisualScriptStopFrameKind::Native);
	CHECK(stop.Frames[1].Title.find("Instantiate") != std::string::npos);
	CHECK(stop.Frames[2].Graph == spawnerGraph);
	CHECK(stop.Frames[2].Node == 11);
	// A Native frame holds no values, and a read-only stop does not step; Continue dismisses it.
	CHECK(VisualScriptDebugger::DescribeStop(stop.ID, 1).GetError().starts_with("InvalidState: Frame: frame 1 is engine code"));
	Result<VisualScriptStopValues> const values = VisualScriptDebugger::DescribeStop(stop.ID, 2);
	REQUIRE_MESSAGE(values.IsOk(), values.GetError());
	CHECK(values.GetValue().Graph == spawnerGraph);
	CHECK(values.GetValue().Node.Node == 11);
	CHECK(VisualScriptDebugger::Resume(stop.ID, VisualScriptDebugAction::StepOver).GetError().starts_with("InvalidState: "));
	Result<VisualScriptResumeOutcome> const dismissed = VisualScriptDebugger::Resume(stop.ID, VisualScriptDebugAction::Continue);
	REQUIRE_MESSAGE(dismissed.IsOk(), dismissed.GetError());
	CHECK(dismissed.GetValue().Result == VisualScriptResumeResult::Dismissed);
	CHECK(GetState().Stops.empty());
	scene.OnRuntimeStop();
}

TEST_CASE("VisualScriptDebug: ApplyVisualScriptEdits while stopped migrates the stopped execution, and cancels it when its node goes")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	Ref<VisualScriptAsset> const asset = CreateRef<VisualScriptAsset>(MakeStepperDocument("A"));
	AssetHandle const graph = AssetManager::AddMemoryAsset(asset, "Stepper");
	Scene scene("Edits");
	Entity const recorder = AddRecorder(scene);
	AddVisualScript(scene.CreateEntity("Stepper"), graph, RecorderVariables(recorder));
	REQUIRE(VisualScriptDebugger::Attach().IsOk());
	Configure({MakeBreakpoint(graph, 33, "Set:Strada.Entity.Name")});
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	REQUIRE(GetState().Stops.size() == 1);
	uint64_t const stop = GetState().Stops[0].ID;

	// An edit that keeps the stopped node: the stopped execution moves to the new program at once, before any update.
	VisualScriptDocument edited = MakeStepperDocument("A");
	auto const format = std::find_if(edited.Graph.Nodes.begin(), edited.Graph.Nodes.end(),
	                                 [](VisualScriptNode const& node)
	                                 {
										 return node.ID == 32;
									 });
	REQUIRE(format != edited.Graph.Nodes.end());
	format->Settings["Format"] = "{Old},E2";
	asset->SetDocument(edited);
	scene.ApplyVisualScriptEdits();
	VisualScriptDebugState const migrated = GetState();
	REQUIRE(migrated.Stops.size() == 1);
	CHECK(migrated.Stops[0].ID == stop);
	CHECK(migrated.Stops[0].Migrated);
	CHECK(migrated.Stops[0].Resumable);
	// Continuing runs the edited program's node.
	REQUIRE(VisualScriptDebugger::Resume(stop, VisualScriptDebugAction::Continue).IsOk());
	CHECK(GetRecords(recorder) == std::vector<std::string>{"A1", "E2"});

	// An edit that removes the stopped node cancels the stopped execution, with the runtime's warning.
	scene.OnUpdateRuntime(Timestep(FrameTime));
	REQUIRE(GetState().Stops.size() == 1);
	uint64_t const logStart = Log::GetNextEntryIndex();
	VisualScriptDocument removed = MakeStepperDocument("A");
	std::erase_if(removed.Graph.Nodes,
	              [](VisualScriptNode const& node)
	              {
					  return node.ID >= 30 && node.ID <= 33;
				  });
	std::erase_if(removed.Graph.Links,
	              [](VisualScriptLink const& link)
	              {
					  return (link.From.Node >= 30 && link.From.Node <= 33) || (link.To.Node >= 30 && link.To.Node <= 33);
				  });
	asset->SetDocument(removed);
	scene.ApplyVisualScriptEdits();
	CHECK(GetState().Stops.empty());
	CHECK(Testing::WasLogged(logStart, "is cancelled"));
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(GetRecords(recorder) == std::vector<std::string>{"A1", "E2", "A1", "A1"});
	scene.OnRuntimeStop();
}

TEST_CASE("VisualScriptDebug: the frame trace of a watched graph counts what ran since the last take")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	AssetHandle const watched = AddGraph(MakeStepperDocument("A"), "Watched");
	AssetHandle const other = AddGraph(MakeUpdateRecorderDocument("B"), "Other");
	Scene scene("Trace");
	Entity const recorder = AddRecorder(scene);
	AddVisualScript(scene.CreateEntity("Watched"), watched, RecorderVariables(recorder));
	AddVisualScript(scene.CreateEntity("Other"), other, RecorderVariables(recorder));
	REQUIRE(VisualScriptDebugger::Attach().IsOk());
	Configure({}, {{watched, 0}});
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	scene.OnUpdateRuntime(Timestep(FrameTime));

	Result<VisualScriptFrameTrace> const taken = VisualScriptDebugger::TakeTrace();
	REQUIRE_MESSAGE(taken.IsOk(), taken.GetError());
	VisualScriptFrameTrace const& trace = taken.GetValue();
	CHECK(trace.Dropped == 0);
	// Only the watched graph is counted, each exit of each instruction twice; pure nodes count without an exit.
	REQUIRE(trace.Graphs.size() == 1);
	VisualScriptGraphTrace const* const graph = FindGraph(trace, watched);
	REQUIRE(graph != nullptr);
	CHECK(CountSteps(*graph, 10, "Then") == 2);
	CHECK(CountSteps(*graph, 23, "Then") == 2);
	CHECK(CountSteps(*graph, 33, "Then") == 2);
	CHECK(CountSteps(*graph, 40) == 2);
	CHECK(CountSteps(*graph, 22, "") == 2);
	CHECK(std::is_sorted(graph->Steps.begin(), graph->Steps.end(),
	                     [](VisualScriptTraceStep const& a, VisualScriptTraceStep const& b)
	                     {
							 return a.FirstOrder < b.FirstOrder;
						 }));

	// A take with nothing run since is empty; the block is what the facade reads.
	Result<VisualScriptFrameTrace> const again = VisualScriptDebugger::TakeTrace();
	REQUIRE(again.IsOk());
	CHECK(again.GetValue().Graphs.empty());
	CHECK(again.GetValue().StopRevision == trace.StopRevision);
	scene.OnUpdateRuntime(Timestep(FrameTime));
	std::optional<std::vector<std::byte>> const block = TakeRawTrace();
	REQUIRE(block.has_value());
	Result<VisualScriptFrameTrace> const read = ScriptEngineInternal::ReadVisualScriptFrameTrace(*block);
	REQUIRE_MESSAGE(read.IsOk(), read.GetError());
	REQUIRE(read.GetValue().Graphs.size() == 1);
	CHECK(CountSteps(read.GetValue().Graphs[0], 10, "Then") == 1);

	// graph.trace's recording, summarized: what ran in the frames it marked, per node and exit.
	VisualScriptRecordingOptions options;
	options.Documents = {watched};
	REQUIRE(VisualScriptDebugger::StartRecording(options).IsOk());
	for (uint32_t frame = 0; frame < 3; frame++)
	{
		REQUIRE(VisualScriptDebugger::MarkRecordingFrame(frame).IsOk());
		scene.OnUpdateRuntime(Timestep(FrameTime));
	}
	Result<VisualScriptRecording> const recorded = VisualScriptDebugger::StopRecording();
	REQUIRE_MESSAGE(recorded.IsOk(), recorded.GetError());
	REQUIRE(recorded.GetValue().Summary.has_value());
	REQUIRE(recorded.GetValue().Summary->size() == 1);
	VisualScriptGraphSummary const& summary = recorded.GetValue().Summary->front();
	CHECK(summary.Document == watched);
	auto const update = std::find_if(summary.Steps.begin(), summary.Steps.end(),
	                                 [](VisualScriptStepSummary const& step)
	                                 {
										 return step.Node == 10;
									 });
	REQUIRE(update != summary.Steps.end());
	CHECK(update->Exit == "Then");
	CHECK(update->Count == 3);
	CHECK(update->FirstFrame == 0);
	CHECK(update->LastFrame == 2);
	CHECK(VisualScriptDebugger::StopRecording().IsError());
	scene.OnRuntimeStop();
}

TEST_CASE("VisualScriptDebug: values come back typed: entities, assets, colours, cut strings and arrays")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	Scene source("Source");
	Entity const member = source.CreateEntity("Member");
	Result<Ref<PrefabAsset>> prefabAsset = PrefabAsset::Create(PrefabSerializer::Serialize(source, member));
	REQUIRE(prefabAsset.IsOk());
	AssetHandle const prefab = AssetManager::AddMemoryAsset(prefabAsset.GetValue(), "Member");
	VisualScriptDocument document = MakeRichDocument();
	document.Variables.push_back(MakeVariable(7, "Ghost", "Strada.Entity"));
	document.Variables.push_back(MakeVariable(8, "Template", "Strada.Prefab", prefab.ToString()));
	AssetHandle const graph = AddGraph(document, "Rich");
	Scene scene("Values");
	Entity const partner = scene.CreateEntity("Partner");
	Entity const holder = AddVisualScript(
		scene.CreateEntity("Holder"), graph,
		{{"Partner", ScriptFieldValue::FromEntity(partner.GetUUID())}, {"Ghost", ScriptFieldValue::FromEntity(UUID(424242))}});
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(FrameTime));

	VisualScriptValueRequest request;
	request.Entity = holder.GetUUID();
	request.Variables = true;
	request.Timelines = true;
	request.Nodes = {13, 999};
	Result<VisualScriptLiveValues> const read = VisualScriptDebugger::ReadValues(request);
	REQUIRE_MESSAGE(read.IsOk(), read.GetError());
	VisualScriptLiveValues const& values = read.GetValue();
	CHECK(values.Entity == holder.GetUUID());
	CHECK(values.Graph == graph);
	CHECK(values.Source == VisualScriptValueSource::Frame);
	CHECK(values.Timelines.empty());
	REQUIRE(values.Variables.size() == 8);
	CHECK(FindValue(values.Variables, "Health").Value == 10.0);
	CHECK(FindValue(values.Variables, "Health").Type == "float");
	CHECK(FindValue(values.Variables, "Health").ID == 1);
	CHECK(FindValue(values.Variables, "Health").Public);

	VisualScriptDebugValue const& title = FindValue(values.Variables, "Title");
	CHECK(title.Truncated);
	CHECK(title.Length == 300u);
	CHECK(title.Value == std::string(256, 'x'));

	VisualScriptDebugValue const& items = FindValue(values.Variables, "Items");
	CHECK(items.Type == "int[]");
	CHECK(items.Truncated);
	CHECK(items.Length == 40u);
	REQUIRE(items.Value.is_array());
	REQUIRE(items.Value.size() == 32);
	CHECK(items.Value[3] == Json::object({{"Value", 3}}));

	VisualScriptDebugValue const& tint = FindValue(values.Variables, "Tint");
	CHECK(tint.Type == "Strada.Color");
	CHECK(tint.Value == Json::array({0.25, 0.5, 1.0, 1.0}));

	VisualScriptDebugValue const& partnerValue = FindValue(values.Variables, "Partner");
	CHECK(partnerValue.Entity == partner.GetUUID());
	CHECK(partnerValue.EntityValid);
	CHECK(partnerValue.Value.is_null());
	VisualScriptDebugValue const& ghost = FindValue(values.Variables, "Ghost");
	CHECK(ghost.Entity == UUID(424242));
	CHECK_FALSE(ghost.EntityValid);

	CHECK(FindValue(values.Variables, "Template").Value == prefab.ToString());
	VisualScriptDebugValue const& total = FindValue(values.Variables, "Total");
	CHECK_FALSE(total.Public);
	CHECK(total.Value == 3);

	// The node's outputs as the frame holds them; node IDs the graph does not have are left out.
	REQUIRE(values.Nodes.size() == 1);
	CHECK(values.Nodes[0].Node == 13);
	REQUIRE(values.Nodes[0].Outputs.size() == 1);
	CHECK(values.Nodes[0].Outputs[0].Name == "Result");
	CHECK(values.Nodes[0].Outputs[0].Value == 3);
	scene.OnRuntimeStop();
}

TEST_CASE("VisualScriptDebug: a resume whose execution fails reports one script exception")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	ExceptionCounter host;
	AssetHandle const graph = AddGraph(MakeFailingDocument(), "Failing");
	Scene scene("Failing");
	AddVisualScript(scene.CreateEntity("Failing"), graph);
	REQUIRE(VisualScriptDebugger::Attach().IsOk());
	Configure({MakeBreakpoint(graph, 11, "Variable.Set")});
	scene.OnRuntimeStart();
	REQUIRE(GetState().Stops.size() == 1);
	CHECK(host.Exceptions == 0);

	Result<VisualScriptResumeOutcome> const resumed = VisualScriptDebugger::Resume(std::nullopt, VisualScriptDebugAction::Continue);
	REQUIRE_MESSAGE(resumed.IsOk(), resumed.GetError());
	CHECK(resumed.GetValue().Failed);
	CHECK(resumed.GetValue().Result == VisualScriptResumeResult::Ended);
	CHECK(host.Exceptions == 1);
	CHECK(GetState().Stops.empty());
	scene.OnRuntimeStop();
	CHECK(host.Exceptions == 1);
}

TEST_CASE("VisualScriptDebug: ScriptEngine::Shutdown with stopped executions leaves nothing of the debugger behind")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
	document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
	document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}}), Json::object({{"Value", 1}})));
	document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	AssetHandle const graph = AddGraph(document, "Counter");
	UUID const entity(77);
	REQUIRE(VisualScriptDebugger::Attach().IsOk());
	Configure({MakeBreakpoint(graph, 11, "Variable.Set")});
	REQUIRE(VisualScriptEngine::CreateInstance(entity, graph, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(entity);
	REQUIRE(GetState().Stops.size() == 1);

	ScriptEngine::Shutdown();
	CHECK_FALSE(VisualScriptDebugger::IsAttached());
	CHECK(VisualScriptDebugger::GetState().GetError() == "the scripting runtime is not initialized");
	REQUIRE(ScriptEngine::Init().IsOk());
	CHECK_FALSE(VisualScriptDebugger::IsAttached());
	CHECK(VisualScriptDebugger::GetState().GetError() == "NotAttached: no debugger is attached");
	CHECK(VisualScriptEngine::GetInstanceCount() == 0);
	Result<VisualScriptInstanceList> const instances = VisualScriptDebugger::DescribeInstances({});
	REQUIRE_MESSAGE(instances.IsOk(), instances.GetError());
	CHECK(instances.GetValue().Total == 0);
	// A fresh attachment starts with no stops.
	REQUIRE(VisualScriptDebugger::Attach().IsOk());
	CHECK(GetState().Stops.empty());
}

TEST_CASE("VisualScriptDebug: the readers refuse replies not of their form, naming the first member that is not")
{
	using namespace ScriptEngineInternal;
	CHECK(ReadVisualScriptDebugState("{ not JSON").GetError().starts_with("the reply is not JSON: "));
	CHECK(ReadVisualScriptDebugState("[]").GetError() == "the reply: not an object");
	CHECK(ReadVisualScriptStopValues(R"({"Error": "Stop: no stop of the ID", "Code": "NotFound"})").GetError() ==
	      "NotFound: Stop: no stop of the ID");
	CHECK(ReadVisualScriptResumeOutcome(R"({"Code": 3})").GetError() == "the reply: an error reply without a string Code and Error");
	CHECK(ReadVisualScriptResumeOutcome(R"({"Outcome": "Armed", "Failed": false, "Stop": null, "StopRevision": 1})").GetError() ==
	      "Outcome: unknown value \"Armed\"");
	CHECK(ReadVisualScriptResumeOutcome(R"({"Outcome": "Stopped", "Failed": false, "Stop": null, "StopRevision": 1})").GetError() ==
	      "Stop: missing for the outcome Stopped");
	Result<VisualScriptResumeOutcome> const outcome =
		ReadVisualScriptResumeOutcome(R"({"Outcome": "Stopped", "Failed": true, "Stop": 4, "StopRevision": 9, "Later": 1})");
	REQUIRE(outcome.IsOk());
	CHECK(outcome.GetValue() == VisualScriptResumeOutcome{VisualScriptResumeResult::Stopped, true, 4, 9});

	constexpr std::string_view State = R"({"Revision": 2, "StopRevision": 5, "Current": 1, "Passed": 0, "Stepping": null,
		"Stops": [{"ID": 1, "Reason": "Breakpoint", "Resumable": true, "Entity": "7", "Graph": "8", "Place": "Enter",
		"Message": "", "Migrated": false, "Changed": [{"Node": 0, "Name": "Health"}], "ChangedMore": 0,
		"Breakpoint": {"Document": "8", "Node": 3},
		"Frames": [{"Kind": "Graph", "Entity": "7", "Graph": "8", "Document": "8", "Function": 0, "FunctionName": "", "Node": 3,
		"Title": "Set Health", "Pin": "Execute", "Note": ""}, {"Kind": "Native", "Title": "Entity.Instantiate"}],
		"Start": {"Kind": "Resume", "Graph": "8", "Document": "8", "Node": 2, "Title": "Delay", "Pin": "Completed",
		"Origin": {"Document": "8", "Node": 1, "Title": "Event Create"}}}],
		"Breakpoints": [{"Document": "8", "Node": 3, "Hits": 1, "Reached": 2, "Active": false, "Reason": "NotRunning", "Error": null}]})";
	Result<VisualScriptDebugState> const state = ReadVisualScriptDebugState(State);
	REQUIRE_MESSAGE(state.IsOk(), state.GetError());
	REQUIRE(state.GetValue().Stops.size() == 1);
	VisualScriptStop const& stop = state.GetValue().Stops[0];
	CHECK(stop.Entity == UUID(7));
	CHECK(stop.Changed == std::vector<VisualScriptChangedValue>{{0, "Health"}});
	REQUIRE(stop.Frames.size() == 2);
	CHECK(stop.Frames[1].Kind == VisualScriptStopFrameKind::Native);
	CHECK(stop.Frames[1].Title == "Entity.Instantiate");
	REQUIRE(stop.Start.Origin.has_value());
	CHECK(stop.Start.Origin->Title == "Event Create");
	CHECK(state.GetValue().Breakpoints[0].Reason == VisualScriptBreakpointInactivity::NotRunning);

	auto const damaged = [&State](std::string_view from, std::string_view to)
	{
		std::string text(State);
		size_t const at = text.find(from);
		REQUIRE(at != std::string::npos);
		text.replace(at, from.size(), to);
		return ReadVisualScriptDebugState(text).GetError();
	};
	CHECK(damaged(R"("Kind": "Native")", R"("Kind": "Lost")") == "Stops[0].Frames[1].Kind: unknown value \"Lost\"");
	CHECK(damaged(R"("Entity": "7", "Graph": "8", "Place")", R"("Entity": "-7", "Graph": "8", "Place")") ==
	      "Stops[0].Entity: not a decimal string of an unsigned 64-bit integer");
	CHECK(damaged(R"("Node": 1, "Title": "Event Create")", R"("Title": "Event Create")") == "Stops[0].Start.Origin.Node: missing");
	CHECK(damaged(R"("Hits": 1)", R"("Hits": 4294967296)") == "Breakpoints[0].Hits: not an unsigned 32-bit integer");
	CHECK(damaged(R"("Active": false)", R"("Active": true)") == "Breakpoints[0].Reason: does not agree with Active");
	CHECK(damaged(R"("Passed": 0, )", "") == "Passed: missing");

	// A value has exactly one of Value, Entity and Text (Text with Entity for components).
	constexpr std::string_view Values =
		R"({"Entity": "7", "Graph": "8", "Source": "Frame", "Variables": [%s], "Timelines": [], "Nodes": [], "Truncated": 0})";
	auto const readValue = [&Values](std::string_view value)
	{
		std::string text(Values);
		text.replace(text.find("%s"), 2, value);
		return ReadVisualScriptLiveValues(text);
	};
	CHECK(readValue(R"({"Name": "A", "Type": "int", "Value": 1, "Text": "1"})").GetError() ==
	      "Variables[0]: not one of Value, Entity and Text");
	CHECK(readValue(R"({"Name": "A", "Type": "int"})").GetError() == "Variables[0]: not one of Value, Entity and Text");
	CHECK(readValue(R"({"Name": "A", "Value": 1})").GetError() == "Variables[0].Type: missing");
	Result<VisualScriptLiveValues> const component = readValue(
		R"json({"Name": "Body", "Type": "Strada.RigidBodyComponent", "Text": "Rigid Body of Player (5)", "Entity": "5", "Valid": false})json");
	REQUIRE(component.IsOk());
	CHECK(component.GetValue().Variables[0].Entity == UUID(5));
	CHECK_FALSE(component.GetValue().Variables[0].EntityValid);
	CHECK(component.GetValue().Variables[0].Text == "Rigid Body of Player (5)");
}

TEST_CASE("VisualScriptDebug: damaged trace blocks are refused or read with every size checked")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	AssetHandle const stepper = AddGraph(MakeStepperDocument("A"), "Stepper");
	AssetHandle const rich = AddGraph(MakeRichDocument(), "Rich");
	Scene scene("Blocks");
	Entity const recorder = AddRecorder(scene);
	AddVisualScript(scene.CreateEntity("Stepper"), stepper, RecorderVariables(recorder));
	AddVisualScript(scene.CreateEntity("Rich"), rich);
	REQUIRE(VisualScriptDebugger::Attach().IsOk());
	Configure({}, {{stepper, 0}, {rich, 0}});
	scene.OnRuntimeStart();

	// The blocks the runtime's writer produces: with flow in both graphs, in one, and none.
	std::vector<std::vector<std::byte>> blocks;
	for (int frame = 0; frame < 4; frame++)
	{
		scene.OnUpdateRuntime(Timestep(FrameTime));
		std::optional<std::vector<std::byte>> block = TakeRawTrace();
		REQUIRE(block.has_value());
		blocks.push_back(std::move(*block));
	}
	std::optional<std::vector<std::byte>> empty = TakeRawTrace();
	REQUIRE(empty.has_value());
	blocks.push_back(std::move(*empty));
	scene.OnRuntimeStop();
	for (std::vector<std::byte> const& block : blocks)
	{
		Result<VisualScriptFrameTrace> const read = ScriptEngineInternal::ReadVisualScriptFrameTrace(block);
		REQUIRE_MESSAGE(read.IsOk(), read.GetError());
	}
	REQUIRE(ScriptEngineInternal::ReadVisualScriptFrameTrace(blocks[0]).GetValue().Graphs.size() == 2);
	CHECK(ScriptEngineInternal::ReadVisualScriptFrameTrace(blocks.back()).GetValue().Graphs.empty());

	Testing::FuzzRandom random(11400);
	int const rounds = Testing::GetFuzzRounds(300);
	int refused = 0;
	for (int round = 0; round < rounds; round++)
	{
		CAPTURE(round);
		std::vector<std::byte> block = blocks[random.Pick(blocks.size())];
		for (int mutation = 0; mutation <= round % 3; mutation++)
		{
			switch (random.Pick(5))
			{
				case 0:
					// A byte anywhere, the header's counts and sizes most of all.
					block[random.Pick(std::min<size_t>(block.size(), random.Pick(2) == 0 ? 32 : block.size()))] =
						static_cast<std::byte>(random.Pick(256));
					break;
				case 1:
					block.resize(random.Pick(block.size() + 1));
					break;
				case 2:
					block.push_back(static_cast<std::byte>(random.Pick(256)));
					break;
				case 3:
					if (block.size() >= 24)
					{
						// A count or a length set to an extreme.
						size_t const at = 16 + 4 * random.Pick(2);
						std::byte const extreme = random.Pick(2) == 0 ? std::byte{0xFF} : std::byte{0x7F};
						for (size_t index = 0; index < 4; index++)
						{
							block[at + index] = extreme;
						}
					}
					break;
				default:
					if (!block.empty())
					{
						size_t const at = random.Pick(block.size());
						block.insert(block.begin() + static_cast<std::ptrdiff_t>(at), block.begin(),
						             block.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(block.size() - at, 16)));
					}
					break;
			}
			if (block.empty())
			{
				break;
			}
		}
		Result<VisualScriptFrameTrace> const read = ScriptEngineInternal::ReadVisualScriptFrameTrace(block);
		if (!read)
		{
			refused++;
			CHECK_FALSE(read.GetError().empty());
			continue;
		}
		// What reads fits the block it came from: every graph and step took its bytes.
		size_t steps = 0;
		for (VisualScriptGraphTrace const& graph : read.GetValue().Graphs)
		{
			steps += graph.Steps.size();
		}
		CHECK(32 + 16 * (read.GetValue().Graphs.size() + steps) <= block.size());
	}
	CHECK(refused > 0);
}

TEST_CASE("VisualScriptDebug: damaged debug replies are refused or read")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	AssetHandle const graph = AddGraph(MakeRichDocument(), "Rich");
	Scene scene("Replies");
	Entity const partner = scene.CreateEntity("Partner");
	Entity const holder =
		AddVisualScript(scene.CreateEntity("Holder"), graph, {{"Partner", ScriptFieldValue::FromEntity(partner.GetUUID())}});
	std::string const document = graph.ToString();
	std::string const entity = holder.GetUUID().ToString();

	// The replies of every command the runtime's protocol answers, in the states a session goes through, collected by the
	// reader that reads them.
	enum class Form : uint8_t
	{
		State = 0,
		Resume,
		Describe,
		Values,
		Instances,
		Condition,
		Recording
	};
	std::vector<std::pair<Form, std::string>> replies;
	auto const collect = [&replies](Form form, std::string const& request)
	{
		replies.emplace_back(form, SendRaw(request));
	};
	REQUIRE(VisualScriptDebugger::Attach().IsOk());
	Configure({MakeBreakpoint(graph, 12, "Variable.Set", "Index == 1")}, {{graph, 0}});
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	REQUIRE(GetState().Stops.size() == 1);
	uint64_t const stop = GetState().Stops[0].ID;
	std::string const stopText = std::to_string(stop);
	collect(Form::State, R"({"Command": "State"})");
	collect(Form::Describe, R"({"Command": "Describe", "Stop": )" + stopText + R"(, "Frame": 0, "Evaluate": ["Total + Index", "nope +"]})");
	collect(Form::Describe, R"({"Command": "Describe", "Stop": 999, "Frame": 0})");
	collect(Form::Values,
	        R"({"Command": "Values", "Entity": ")" + entity + R"(", "Nodes": [12, 13], "Variables": true, "Timelines": true})");
	collect(Form::Instances, R"({"Command": "Instances"})");
	collect(Form::Condition, R"({"Command": "CheckCondition", "Document": ")" + document + R"(", "Node": 12, "Condition": "Index == 1"})");
	collect(Form::Condition, R"({"Command": "CheckCondition", "Document": ")" + document + R"(", "Node": 12, "Condition": "angle"})");
	collect(Form::Condition, R"({"Command": "CheckCondition", "Document": ")" + document + R"(", "Node": 12, "Condition": ""})");

	// Steps, a run to the Delay and a step over it, which waits for its fiber, with a recording of what the steps run.
	SendRaw(
		R"({"Command": "Record", "Start": {"Entity": null, "Documents": null, "Nodes": null, "Values": true, "Summary": false, "MaxSteps": 50, "MaxValues": 50}})");
	collect(Form::Resume, R"({"Command": "Resume", "Action": "StepOver"})");
	collect(Form::Resume, R"({"Command": "Resume", "Action": "StepInto"})");
	Configure({}, {{graph, 0}}, 2);
	REQUIRE(VisualScriptDebugger::RunTo(graph, 15).IsOk());
	collect(Form::Resume, R"({"Command": "Resume", "Action": "Continue"})");
	collect(Form::State, R"({"Command": "State"})");
	collect(Form::Resume, R"({"Command": "Resume", "Action": "StepOver"})");
	collect(Form::State, R"({"Command": "State"})");
	collect(Form::Instances, R"({"Command": "Instances", "Graph": ")" + document + R"(", "Limit": 10})");
	for (int frame = 1; frame <= 5; frame++)
	{
		SendRaw(R"({"Command": "Record", "Frame": )" + std::to_string(frame) + "}");
		scene.OnUpdateRuntime(Timestep(FrameTime));
		if (!GetState().Stops.empty())
		{
			collect(Form::State, R"({"Command": "State"})");
			collect(Form::Describe, R"({"Command": "Describe", "Stop": )" + std::to_string(GetState().Stops[0].ID) + R"(, "Frame": 0})");
			collect(Form::Resume, R"({"Command": "Resume", "All": true, "Action": "Continue"})");
		}
	}
	collect(Form::Recording, R"({"Command": "Record", "Stop": true})");
	SendRaw(R"({"Command": "Record", "Start": {"Entity": ")" + entity + R"(", "Documents": [")" + document +
	        R"("], "Nodes": [12, 13, 16], "Values": true, "Summary": true, "MaxSteps": 50, "MaxValues": 50}})");
	for (int frame = 0; frame < 3; frame++)
	{
		scene.OnUpdateRuntime(Timestep(FrameTime));
	}
	collect(Form::Recording, R"({"Command": "Record", "Stop": true})");
	collect(Form::Values, R"({"Command": "Values", "Entity": ")" + entity + R"(", "Function": 0})");
	collect(Form::Recording, R"({"Command": "Record", "Stop": true})");
	scene.OnRuntimeStop();

	auto const read = [](Form form, std::string_view text) -> std::string
	{
		switch (form)
		{
			case Form::State:
				return ScriptEngineInternal::ReadVisualScriptDebugState(text).GetError();
			case Form::Resume:
			{
				Result<VisualScriptResumeOutcome> const outcome = ScriptEngineInternal::ReadVisualScriptResumeOutcome(text);
				if (outcome)
				{
					CHECK((outcome.GetValue().Result == VisualScriptResumeResult::Stopped) == outcome.GetValue().Stop.has_value());
				}
				return outcome.GetError();
			}
			case Form::Describe:
				return ScriptEngineInternal::ReadVisualScriptStopValues(text).GetError();
			case Form::Values:
				return ScriptEngineInternal::ReadVisualScriptLiveValues(text).GetError();
			case Form::Instances:
			{
				Result<VisualScriptInstanceList> const list = ScriptEngineInternal::ReadVisualScriptInstanceList(text);
				if (list)
				{
					CHECK(list.GetValue().Instances.size() <= list.GetValue().Total);
				}
				return list.GetError();
			}
			case Form::Condition:
				return ScriptEngineInternal::ReadVisualScriptConditionCheck(text).GetError();
			case Form::Recording:
				return ScriptEngineInternal::ReadVisualScriptRecording(text).GetError();
		}
		return "unknown form";
	};

	// Every reply the runtime gave reads, but its error replies, which are refused with their code.
	std::array<int, 7> forms = {};
	for (auto const& [form, text] : replies)
	{
		CAPTURE(text);
		forms[static_cast<size_t>(form)]++;
		std::string const error = read(form, text);
		bool const errorReply = text.find("\"Code\"") != std::string::npos;
		CHECK(error.empty() != errorReply);
	}
	for (int const count : forms)
	{
		CHECK(count > 0);
	}
	// The states include a step waiting in the Delay.
	bool waitingStep = false;
	for (auto const& [form, text] : replies)
	{
		if (form == Form::State)
		{
			Result<VisualScriptDebugState> const state = ScriptEngineInternal::ReadVisualScriptDebugState(text);
			waitingStep |= state && state.GetValue().Stepping && state.GetValue().Stepping->WaitsIn;
		}
	}
	CHECK(waitingStep);

	Testing::DocumentMutator mutator(11450);
	Testing::FuzzRandom pick(11450);
	int const rounds = Testing::GetFuzzRounds(200);
	int readCount = 0;
	for (int round = 0; round < rounds; round++)
	{
		CAPTURE(round);
		auto const& [form, text] = replies[pick.Pick(replies.size())];
		Result<Json> const parsed = ParseJson(text);
		REQUIRE(parsed.IsOk());
		std::string const damaged = mutator.Damage(parsed.GetValue(), round).dump();
		CAPTURE(damaged);
		std::string const error = read(form, damaged);
		if (error.empty())
		{
			readCount++;
			continue;
		}
		// A refusal names the member, the reply as a whole, or the runtime's error reply.
		CHECK((error.find(": ") != std::string::npos || error.starts_with("the reply")));
		// Read by another form's reader, a reply is refused or read too, never more.
		static_cast<void>(read(static_cast<Form>(pick.Pick(forms.size())), damaged));
	}
	CHECK(readCount > 0);
}
