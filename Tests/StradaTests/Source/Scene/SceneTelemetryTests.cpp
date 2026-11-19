#include "Core/TelemetryTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"

#include "Strada/Core/Telemetry.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace Strada;
using Testing::GetNewestTelemetrySession;
using Testing::GetTelemetryColumn;
using Testing::GetTelemetryMarks;
using Testing::TelemetryScope;

// The fixed step's named call (Scene::SampleTelemetry) in running scenes without scripts. Scripts' own records (their
// OnFixedUpdate, OnUpdate and contact handlers) are tested through the scripting bindings.
namespace
{
	constexpr float Step = 1.0f / 60.0f;

	SceneRuntimeSettings WithoutScripts()
	{
		SceneRuntimeSettings settings;
		settings.RunScripts = false;
		settings.PlayAudio = false;
		return settings;
	}

	// A ground, a crate falling on it and a ball rolling into it, with fixed UUIDs.
	void BuildBodies(Scene& scene)
	{
		Entity ground = scene.CreateEntityWithUUID(UUID(100), "Ground");
		ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
		ground.AddComponent<BoxColliderComponent>().HalfExtents = {20.0f, 0.5f, 20.0f};

		Entity crate = scene.CreateEntityWithUUID(UUID(200), "Crate");
		crate.GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, 0.0f};
		crate.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		crate.AddComponent<BoxColliderComponent>().HalfExtents = {0.5f, 0.5f, 0.5f};

		Entity ball = scene.CreateEntityWithUUID(UUID(300), "Ball");
		ball.GetComponent<TransformComponent>().Translation = {-4.0f, 0.5f, 0.0f};
		RigidBodyComponent& body = ball.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Dynamic;
		body.InitialLinearVelocity = {6.0f, 0.0f, 0.0f};
		ball.AddComponent<SphereColliderComponent>().Radius = 0.5f;
	}
}

TEST_CASE("SceneTelemetry: a scene records one row per fixed step at 30, 60 and 144 frames a second, what precedes a frame in "
          "its first step's row")
{
	TelemetryScope const telemetry;
	std::vector<std::map<uint64_t, double>> times;
	for (float const frameTime : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f})
	{
		CAPTURE(frameTime);
		REQUIRE(Telemetry::Start().IsOk());
		// No physics system: the steps run without physics, and record.
		Scene scene("Rates");
		scene.OnRuntimeStart(WithoutScripts());
		REQUIRE(scene.GetPhysicsScene() == nullptr);
		int const frames = static_cast<int>(std::lround(2.0f / frameTime));
		// The value each step's row must hold: the frame recorded before the frame that ran it, in its first step only.
		std::map<uint64_t, float> expected;
		for (int frame = 0; frame < frames; ++frame)
		{
			// Between frames, as OnUpdate and events record.
			Telemetry::Record("Frame", static_cast<float>(frame));
			uint64_t const before = scene.GetFixedStepCount();
			scene.OnUpdateRuntime(Timestep(frameTime));
			if (scene.GetFixedStepCount() > before)
			{
				expected[before + 1] = static_cast<float>(frame);
			}
		}
		Telemetry::Stop();

		TelemetrySession const& session = GetNewestTelemetrySession();
		CHECK(session.Source == scene.GetSerial());
		CHECK(session.FirstStep == 1);
		CHECK(session.Rows == scene.GetFixedStepCount());
		CHECK(session.Rows >= 119);
		std::vector<std::optional<float>> const column = GetTelemetryColumn(session, "Frame");
		std::map<uint64_t, double> stepTimes;
		for (uint64_t row = 0; row < session.Rows; ++row)
		{
			uint64_t const step = session.FirstStep + row;
			auto const found = expected.find(step);
			CHECK(column[row] == (found != expected.end() ? std::optional<float>(found->second) : std::nullopt));
			stepTimes[step] = GetTelemetryTime(session, row);
		}
		times.push_back(std::move(stepTimes));
		scene.OnRuntimeStop();
	}
	// A step's time is the simulation's, whatever the frame rate.
	for (auto const& [step, time] : times[0])
	{
		for (size_t rate = 1; rate < times.size(); ++rate)
		{
			auto const found = times[rate].find(step);
			if (found != times[rate].end())
			{
				CHECK(found->second == time);
			}
		}
	}
}

TEST_CASE("SceneTelemetry: a frame of eight steps adds eight rows, the first with what preceded it, and a slower one only the "
          "steps it runs")
{
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	Scene scene("Long frames");
	scene.OnRuntimeStart(WithoutScripts());

	Telemetry::Record("Before", 1.0f);
	scene.OnUpdateRuntime(Timestep(8.0f * Step));
	REQUIRE(scene.GetFixedStepCount() == 8);
	TelemetrySession const& session = GetNewestTelemetrySession();
	CHECK(session.Rows == 8);
	std::vector<std::optional<float>> column = GetTelemetryColumn(session, "Before");
	CHECK(column[0] == 1.0f);
	for (size_t row = 1; row < column.size(); ++row)
	{
		CHECK_FALSE(column[row].has_value());
	}

	// Twenty steps' time in one frame: the scene runs MaxStepsPerFrame and drops the backlog, and records what it runs.
	Telemetry::Record("Before", 2.0f);
	scene.OnUpdateRuntime(Timestep(20.0f * Step));
	CHECK(scene.GetFixedStepCount() == 16);
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(Telemetry::GetSessions().size() == 1);
	CHECK(session.Rows == scene.GetFixedStepCount());
	column = GetTelemetryColumn(session, "Before");
	CHECK(column[8] == 2.0f);
	CHECK(FindTelemetryChannel(session, "Before")->Count == 2);
	scene.OnRuntimeStop();
}

TEST_CASE("SceneTelemetry: a paused scene records nothing, a frame it steps records as usual, and at time scale 0 the next "
          "row takes each channel's last value and every mark")
{
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	Scene scene("Pauses");
	scene.OnRuntimeStart(WithoutScripts());

	// The editor's pause: frames run nothing until one is stepped.
	scene.SetPaused(true);
	Telemetry::Record("Value", 1.0f);
	for (int frame = 0; frame < 3; ++frame)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	CHECK(Telemetry::GetSessions().empty());
	scene.Step(1);
	scene.OnUpdateRuntime(Timestep(Step));
	REQUIRE(scene.GetFixedStepCount() == 1);
	TelemetrySession const& session = GetNewestTelemetrySession();
	CHECK(session.Rows == 1);
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(session.Rows == 1);
	scene.SetPaused(false);

	// A game's pause: time scale 0 runs frames, scripts and events, and no step.
	scene.SetTimeScale(0.0f);
	constexpr int PausedFrames = 600;
	for (int frame = 0; frame < PausedFrames; ++frame)
	{
		Telemetry::Record("Throttle", static_cast<float>(frame));
		Telemetry::Record("Lean", static_cast<float>(-frame));
		if (frame % 100 == 0)
		{
			Telemetry::Mark("Paused " + std::to_string(frame / 100));
		}
		scene.OnUpdateRuntime(Timestep(Step));
	}
	CHECK(scene.GetFixedStepCount() == 1);
	CHECK(session.Rows == 1);
	scene.SetTimeScale(1.0f);
	scene.OnUpdateRuntime(Timestep(Step));
	REQUIRE(session.Rows == 2);
	CHECK(GetTelemetryColumn(session, "Value") == std::vector<std::optional<float>>{1.0f, std::nullopt});
	CHECK(GetTelemetryColumn(session, "Throttle")[1] == static_cast<float>(PausedFrames - 1));
	CHECK(GetTelemetryColumn(session, "Lean")[1] == static_cast<float>(-(PausedFrames - 1)));
	CHECK(GetTelemetryMarks(session, 1).size() == 6);
	CHECK(session.DroppedValues == 0);
	CHECK(session.DroppedMarks == 0);
	scene.OnRuntimeStop();
}

TEST_CASE("SceneTelemetry: recording leaves every physics state hash as it is")
{
	Testing::PhysicsSystemScope const physics;
	TelemetryScope const telemetry;
	struct Played
	{
		std::vector<uint64_t> Hashes;
		uint64_t Steps = 0;
	};
	auto play = [](bool record)
	{
		if (record)
		{
			REQUIRE(Telemetry::Start().IsOk());
		}
		Scene scene("Hashes");
		BuildBodies(scene);
		scene.OnRuntimeStart(WithoutScripts());
		Played played;
		for (int frame = 0; frame < 120; ++frame)
		{
			Entity ball = scene.GetEntityByUUID(UUID(300));
			Telemetry::Record("Ball.X", ball.GetComponent<TransformComponent>().Translation.x);
			Telemetry::Mark("Frame");
			scene.OnUpdateRuntime(Timestep(1.0f / 144.0f));
			played.Hashes.push_back(scene.ComputePhysicsStateHash());
		}
		played.Steps = scene.GetFixedStepCount();
		scene.OnRuntimeStop();
		Telemetry::Stop();
		return played;
	};
	Played const unrecorded = play(false);
	Played const recorded = play(true);
	CHECK(recorded.Hashes == unrecorded.Hashes);
	REQUIRE(Telemetry::GetSessions().size() == 1);
	CHECK(GetNewestTelemetrySession().Rows == recorded.Steps);
	CHECK(FindTelemetryChannel(GetNewestTelemetrySession(), "Ball.X")->Count == recorded.Steps);
}

TEST_CASE("SceneTelemetry: a restored snapshot begins a session at the step it went back to, a scene started again or another "
          "scene a session of its own")
{
	Testing::PhysicsSystemScope const physics;
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	Scene scene("Snapshots");
	BuildBodies(scene);
	scene.OnRuntimeStart(WithoutScripts());
	for (int frame = 0; frame < 20; ++frame)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	Result<ScenePhysicsSnapshot> const saved = scene.SavePhysicsState();
	REQUIRE(saved.IsOk());
	for (int frame = 0; frame < 10; ++frame)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	REQUIRE(scene.RestorePhysicsState(saved.GetValue()).IsOk());
	for (int frame = 0; frame < 5; ++frame)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	scene.OnRuntimeStop();
	scene.OnRuntimeStart(WithoutScripts());
	scene.OnUpdateRuntime(Timestep(Step));
	scene.OnRuntimeStop();

	Scene other("Another scene");
	other.OnRuntimeStart(WithoutScripts());
	other.OnUpdateRuntime(Timestep(Step));
	other.OnRuntimeStop();

	std::deque<TelemetrySession> const& sessions = Telemetry::GetSessions();
	REQUIRE(sessions.size() == 4);
	CHECK(sessions[0].FirstStep == 1);
	CHECK(sessions[0].Rows == 30);
	CHECK(sessions[1].FirstStep == 21);
	CHECK(sessions[1].Rows == 5);
	CHECK(sessions[2].FirstStep == 1);
	CHECK(sessions[2].Rows == 1);
	CHECK(sessions[2].Source == scene.GetSerial());
	CHECK(sessions[3].Source == other.GetSerial());
	CHECK(sessions[3].Live);
}

TEST_CASE("SceneTelemetry: marks at the steps of trigger crossings give laps of the same rows at 30, 60 and 144 frames a second")
{
	Testing::PhysicsSystemScope const physics;
	TelemetryScope const telemetry;
	struct Crossings
	{
		std::vector<uint64_t> MarkRows;
		std::vector<uint64_t> LapRows;
		uint64_t FirstStep = 0;
	};
	auto ride = [](float frameTime)
	{
		REQUIRE(Telemetry::Start().IsOk());
		Scene scene("Gates");
		// A ball falling through three gates, which a handler learns of after the frame's steps.
		Entity ball = scene.CreateEntityWithUUID(UUID(10), "Ball");
		ball.GetComponent<TransformComponent>().Translation = {0.0f, 10.0f, 0.0f};
		ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		ball.AddComponent<SphereColliderComponent>().Radius = 0.25f;
		uint64_t gateID = 20;
		for (float const height : {8.0f, 5.0f, 1.0f})
		{
			Entity gate = scene.CreateEntityWithUUID(UUID(gateID++), "Gate");
			gate.GetComponent<TransformComponent>().Translation = {0.0f, height, 0.0f};
			BoxColliderComponent& trigger = gate.AddComponent<BoxColliderComponent>();
			trigger.HalfExtents = {2.0f, 0.1f, 2.0f};
			trigger.IsTrigger = true;
		}
		scene.OnRuntimeStart(WithoutScripts());
		uint32_t lap = 0;
		for (int frame = 0; frame < 2000 && lap < 3; ++frame)
		{
			scene.OnUpdateRuntime(Timestep(frameTime));
			for (ContactEvent const& event : scene.GetContactEvents())
			{
				if (event.Type == ContactEventType::TriggerEnter)
				{
					Telemetry::MarkStep("Lap " + std::to_string(++lap), event.Step);
				}
			}
		}
		REQUIRE(lap == 3);
		scene.OnUpdateRuntime(Timestep(frameTime));
		scene.OnRuntimeStop();
		Telemetry::Stop();

		TelemetrySession const& session = GetNewestTelemetrySession();
		Crossings crossings;
		crossings.FirstStep = session.FirstStep;
		for (TelemetryMark const& mark : session.Marks)
		{
			crossings.MarkRows.push_back(mark.Row);
		}
		for (TelemetryLap const& found : FindTelemetryLaps(session))
		{
			crossings.LapRows.push_back(found.StartRow);
			crossings.LapRows.push_back(found.EndRow);
		}
		return crossings;
	};
	Crossings const at60 = ride(1.0f / 60.0f);
	REQUIRE(at60.MarkRows.size() == 3);
	REQUIRE(at60.LapRows.size() == 4);
	CHECK(at60.LapRows[0] < at60.LapRows[1]);
	for (float const frameTime : {1.0f / 30.0f, 1.0f / 144.0f})
	{
		CAPTURE(frameTime);
		Crossings const crossings = ride(frameTime);
		CHECK(crossings.FirstStep == at60.FirstStep);
		CHECK(crossings.MarkRows == at60.MarkRows);
		CHECK(crossings.LapRows == at60.LapRows);
	}
}
