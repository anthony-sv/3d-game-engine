#include "Core/TelemetryTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Telemetry.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;
using Testing::GetNewestTelemetrySession;
using Testing::GetTelemetryColumn;
using Testing::GetTelemetryMarks;
using Testing::TelemetryScope;

// What scripts record through Strada.Telemetry (Tests/TestScripts' TelemetryProbe) and graphs through its nodes, in running
// scenes: the scripting side of Docs/Architecture.md §4.5. These cases host .NET, so they are named apart from the
// recorder's own Telemetry: cases, which the sanitizer runs.
namespace
{
	constexpr float Step = 1.0f / 60.0f;
	constexpr int LapSteps = 20;

	Entity AddProbe(Scene& scene, UUID id, std::string const& name, ScriptFieldMap fields = {})
	{
		Entity entity = scene.CreateEntityWithUUID(id, name);
		ScriptComponent& script = entity.AddComponent<ScriptComponent>();
		script.ClassName = "Strada.Tests.TelemetryProbe";
		script.Fields = std::move(fields);
		return entity;
	}

	// What one run of the falling probe recorded.
	struct ProbeRun
	{
		uint64_t Rows = 0;
		std::vector<std::optional<float>> Steps;
		std::vector<std::optional<float>> Height;
		std::vector<std::optional<float>> Even;
		std::vector<std::optional<float>> Task;
		// Each row's marks, in order.
		std::vector<std::vector<std::string>> Marks;
		std::vector<TelemetryLap> Laps;
	};

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

	// Create keeps Telemetry.IsRecording in Recording and marks step 3 ("Graph.Step", MarkStep; a ulong's literal is a decimal
	// string); each fixed update records Graph.Value at 2.5 and marks its step "Graph".
	VisualScriptDocument MakeTelemetryGraph()
	{
		VisualScriptDocument document;
		VisualScriptVariable recording;
		recording.ID = 1;
		recording.Name = "Recording";
		recording.Type = "bool";
		recording.Value = false;
		recording.Public = true;
		document.Variables.push_back(std::move(recording));
		std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		nodes.push_back(MakeNode(10, "Event.Create"));
		nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}})));
		nodes.push_back(MakeNode(12, "Get:Strada.Telemetry.IsRecording"));
		nodes.push_back(MakeNode(13, "Call:Strada.Telemetry.MarkStep(string,ulong)", Json::object(),
		                         Json::object({{"Label", "Graph.Step"}, {"Step", "3"}})));
		links.push_back(MakeLink(10, "Then", 11, "Execute"));
		links.push_back(MakeLink(12, "Value", 11, "Value"));
		links.push_back(MakeLink(11, "Then", 13, "Execute"));
		nodes.push_back(MakeNode(20, "Event.FixedUpdate"));
		nodes.push_back(MakeNode(21, "Call:Strada.Telemetry.Record(string,float)", Json::object(),
		                         Json::object({{"Channel", "Graph.Value"}, {"Value", 2.5}})));
		nodes.push_back(MakeNode(22, "Call:Strada.Telemetry.Mark(string)", Json::object(), Json::object({{"Label", "Graph"}})));
		links.push_back(MakeLink(20, "Then", 21, "Execute"));
		links.push_back(MakeLink(21, "Then", 22, "Execute"));
		return document;
	}
}

TEST_CASE("SceneTelemetry: C# scripts' fixed updates record the same rows at 30, 60 and 144 frames a second, their updates the "
          "next step's row, another thread its step's row, and a contact handler the contact's step")
{
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	TelemetryScope const telemetry;
	std::vector<ProbeRun> runs;
	for (float const frameTime : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f})
	{
		CAPTURE(frameTime);
		Scene scene("Probe");
		Entity ground = scene.CreateEntityWithUUID(UUID(100), "Ground");
		ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
		ground.AddComponent<BoxColliderComponent>().HalfExtents = {10.0f, 0.5f, 10.0f};
		// A ball that falls 2.5 m onto the ground, at about the 43rd step.
		Entity probe = AddProbe(scene, UUID(200), "Probe", {{"LapSteps", ScriptFieldValue::FromInt32(LapSteps)}});
		probe.GetComponent<TransformComponent>().Translation = {0.0f, 3.0f, 0.0f};
		probe.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		probe.AddComponent<SphereColliderComponent>().Radius = 0.5f;

		REQUIRE(Telemetry::Start().IsOk());
		scene.OnRuntimeStart();
		CHECK(probe.GetName() == "Recording=True;NullThrows=True");
		std::map<uint64_t, float> frames;
		std::optional<uint64_t> landed;
		int const frameCount = static_cast<int>(std::lround(2.0f / frameTime));
		for (int frame = 1; frame <= frameCount; ++frame)
		{
			uint64_t const before = scene.GetFixedStepCount();
			scene.OnUpdateRuntime(Timestep(frameTime));
			// The probe's update runs before the frame's steps: its count lands in the frame's first step's row.
			if (scene.GetFixedStepCount() > before)
			{
				frames[before + 1] = static_cast<float>(frame);
			}
			for (ContactEvent const& event : scene.GetContactEvents())
			{
				if (event.Type == ContactEventType::CollisionEnter && !landed &&
				    (event.First == probe.GetUUID() || event.Second == probe.GetUUID()))
				{
					landed = event.Step;
				}
			}
		}
		Telemetry::Stop();
		REQUIRE(landed.has_value());

		TelemetrySession const& session = GetNewestTelemetrySession();
		CHECK(session.Source == scene.GetSerial());
		CHECK(session.FirstStep == 1);
		REQUIRE(session.Rows == scene.GetFixedStepCount());
		REQUIRE(session.Rows >= 119);
		CHECK(session.DroppedValues == 0);
		CHECK(session.DroppedMarks == 0);
		ProbeRun run;
		run.Rows = session.Rows;
		run.Steps = GetTelemetryColumn(session, "Probe.Steps");
		run.Height = GetTelemetryColumn(session, "Probe.Height");
		run.Even = GetTelemetryColumn(session, "Probe.Even");
		run.Task = GetTelemetryColumn(session, "Probe.Task");
		std::vector<std::optional<float>> const updates = GetTelemetryColumn(session, "Probe.Frame");
		for (uint64_t row = 0; row < session.Rows; ++row)
		{
			CAPTURE(row);
			float const step = static_cast<float>(row + 1);
			CHECK(run.Steps[row] == step);
			CHECK(run.Height[row].has_value());
			CHECK(run.Even[row] == (row % 2 == 1 ? std::optional<float>(step) : std::nullopt));
			CHECK(run.Task[row] == (row == 0 ? std::optional<float>(7.0f) : std::nullopt));
			auto const frame = frames.find(row + 1);
			CHECK(updates[row] == (frame != frames.end() ? std::optional<float>(frame->second) : std::nullopt));
			run.Marks.push_back(GetTelemetryMarks(session, row));
			std::vector<std::string> expected;
			if (row % LapSteps == 0)
			{
				expected.push_back(fmt::format("Lap {}", row / LapSteps + 1));
			}
			if (row + 1 == *landed)
			{
				expected.push_back("Contact");
			}
			CHECK(run.Marks.back() == expected);
		}
		// The ball fell, then rested on the ground.
		CHECK(*run.Height.front() > 2.9f);
		CHECK(*run.Height.back() < 1.0f);
		run.Laps = FindTelemetryLaps(session);
		REQUIRE(run.Laps.size() == (session.Rows - 1) / LapSteps);
		for (TelemetryLap const& lap : run.Laps)
		{
			CHECK(lap.EndRow - lap.StartRow == LapSteps);
			CHECK(lap.Seconds == doctest::Approx(LapSteps * Step));
		}
		runs.push_back(std::move(run));
		scene.OnRuntimeStop();
	}
	// The simulation's rows are the same at every frame rate.
	for (size_t rate = 0; rate < runs.size(); ++rate)
	{
		CAPTURE(rate);
		uint64_t const rows = std::min(runs[rate].Rows, runs[1].Rows);
		for (uint64_t row = 0; row < rows; ++row)
		{
			CAPTURE(row);
			CHECK(runs[rate].Steps[row] == runs[1].Steps[row]);
			CHECK(runs[rate].Height[row] == runs[1].Height[row]);
			CHECK(runs[rate].Even[row] == runs[1].Even[row]);
			CHECK(runs[rate].Marks[row] == runs[1].Marks[row]);
		}
	}
}

TEST_CASE("SceneTelemetry: Strada.Telemetry refuses null, says whether a recording runs, records nothing while none does, and "
          "its names reach the recorder whole")
{
	Testing::ScriptEngineScope scripting;
	TelemetryScope const telemetry;
	{
		Scene scene("Idle");
		Entity probe = AddProbe(scene, UUID(1), "Probe");
		scene.OnRuntimeStart();
		CHECK(probe.GetName() == "Recording=False;NullThrows=True");
		for (int frame = 0; frame < 10; ++frame)
		{
			scene.OnUpdateRuntime(Timestep(Step));
		}
		CHECK(Telemetry::GetSessions().empty());
		CHECK(Telemetry::GetStatus().Buffers == 0);
		scene.OnRuntimeStop();
	}

	// A name of 70 bytes is refused, not cut to 64; one of UTF-8 beyond ASCII is kept as written.
	uint64_t const logStart = Log::GetNextEntryIndex();
	REQUIRE(Telemetry::Start().IsOk());
	Scene scene("Names");
	AddProbe(scene, UUID(1), "Long", {{"ExtraChannel", ScriptFieldValue::FromString(std::string(70, 'x'))}});
	AddProbe(scene, UUID(2), "Wide", {{"ExtraChannel", ScriptFieldValue::FromString("Größe.Ü")}});
	scene.OnRuntimeStart();
	for (int frame = 0; frame < 10; ++frame)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	Telemetry::Stop();
	TelemetrySession const& session = GetNewestTelemetrySession();
	REQUIRE(session.Rows == 10);
	CHECK(GetTelemetryColumn(session, "Größe.Ü") == std::vector<std::optional<float>>(10, 1.0f));
	for (TelemetryChannel const& channel : session.Channels)
	{
		CHECK_FALSE(channel.Name.starts_with("xxxx"));
	}
	CHECK(session.DroppedValues == 10);
	CHECK(Testing::WasLogged(logStart, "Telemetry: dropped what was recorded under a name longer than 64 bytes"));
	scene.OnRuntimeStop();
}

TEST_CASE("SceneTelemetry: graphs' Record, Mark and MarkStep nodes record, and IsRecording says a recording runs")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	TelemetryScope const telemetry;
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeTelemetryGraph()), "Telemetry");
	Scene scene("Graph");
	Entity entity = scene.CreateEntityWithUUID(UUID(1), "Recorder");
	entity.AddComponent<VisualScriptComponent>().Graph = graph;

	REQUIRE(Telemetry::Start().IsOk());
	scene.OnRuntimeStart();
	for (int frame = 0; frame < 6; ++frame)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	Telemetry::Stop();
	Result<ScriptFieldMap> const variables = VisualScriptEngine::GetInstanceVariables(entity.GetUUID());
	REQUIRE(variables.IsOk());
	CHECK(variables.GetValue().at("Recording").GetBool());
	TelemetrySession const& session = GetNewestTelemetrySession();
	REQUIRE(session.Rows == 6);
	CHECK(GetTelemetryColumn(session, "Graph.Value") == std::vector<std::optional<float>>(6, 2.5f));
	for (uint64_t row = 0; row < session.Rows; ++row)
	{
		CAPTURE(row);
		// The mark of step 3 was made before the first step: it waited for its row, and comes before the marks made then.
		std::vector<std::string> const expected =
			row == 2 ? std::vector<std::string>{"Graph.Step", "Graph"} : std::vector<std::string>{"Graph"};
		CHECK(GetTelemetryMarks(session, row) == expected);
	}
	scene.OnRuntimeStop();
}
