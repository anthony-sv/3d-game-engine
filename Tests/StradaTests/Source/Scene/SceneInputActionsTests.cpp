#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/CurveAsset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionMap.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Math/Curve.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;

	struct InputScope
	{
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }
		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	InputActionBinding Bound(std::string controls)
	{
		InputActionBinding binding;
		binding.Controls = std::move(controls);
		return binding;
	}

	InputAction MakeAction(std::string name, InputActionType type, std::vector<InputActionBinding> bindings)
	{
		InputAction action;
		action.Name = std::move(name);
		action.Type = type;
		action.Bindings = std::move(bindings);
		return action;
	}

	Ref<TypedDataAsset<InputActionMap>> MakeAsset(InputActionMap const& map)
	{
		TypedDataAssetType<InputActionMap> const* type = DataAssetRegistry::Find<InputActionMap>();
		REQUIRE(type != nullptr);
		Ref<TypedDataAsset<InputActionMap>> const asset = std::static_pointer_cast<TypedDataAsset<InputActionMap>>(type->CreateAsset());
		Result<void> const set = asset->SetData(map);
		REQUIRE_MESSAGE(set.IsOk(), (set ? std::string() : set.GetError()));
		return asset;
	}

	InputActionMap JumpMap()
	{
		InputActionMap map;
		map.Actions.push_back(MakeAction("Jump", InputActionType::Button, {Bound("Key/Space")}));
		map.Actions.push_back(MakeAction("Steer", InputActionType::Axis, {Bound("Key/A Key/D")}));
		return map;
	}

	SceneRuntimeSettings Settings(AssetHandle actions, uint32_t maxSteps = 8)
	{
		SceneRuntimeSettings settings;
		settings.RunScripts = false;
		settings.PlayAudio = false;
		settings.FixedTimestep = Step;
		settings.MaxStepsPerFrame = maxSteps;
		settings.InputActions = actions;
		return settings;
	}

	size_t CountLogged(uint64_t first, std::string_view text)
	{
		std::vector<LogEntry> const entries = Log::GetEntries(first);
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
		                                         [text](LogEntry const& entry)
		                                         {
													 return entry.Message.find(text) != std::string::npos;
												 }));
	}

	// What the steps of one frame saw: for each step it ran, whether that step took a press or a release of the action.
	struct FrameSightings
	{
		uint64_t FirstStep = 0;
		uint32_t Steps = 0;
		uint32_t Presses = 0;
		uint32_t Releases = 0;
		uint64_t PressStep = 0;
		uint64_t ReleaseStep = 0;
	};

	// Runs one frame and asks, for every step it ran, what an OnFixedUpdate of that step read (a step's edges, by its
	// number, are what code inside it reads; InputActionsTests checks that).
	FrameSightings RunFrame(Scene& scene, float frameTime, uint32_t action)
	{
		InputActionState const& state = scene.GetInputActions();
		FrameSightings sightings;
		sightings.FirstStep = state.GetFixedStepNumber() + 1;
		scene.OnUpdateRuntime(Timestep(frameTime));
		Input::EndFrame();
		sightings.Steps = static_cast<uint32_t>(state.GetFixedStepNumber() + 1 - sightings.FirstStep);
		for (uint64_t step = sightings.FirstStep; step < sightings.FirstStep + sightings.Steps; step++)
		{
			if (state.IsPressed(action, InputActionView::FixedStep, step))
			{
				sightings.Presses++;
				sightings.PressStep = step;
			}
			if (state.IsReleased(action, InputActionView::FixedStep, step))
			{
				sightings.Releases++;
				sightings.ReleaseStep = step;
			}
		}
		return sightings;
	}
}

TEST_CASE("SceneInputActions: at 30, 60 and 144 frames per second each press and release reaches exactly one step, the first after it")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	InputScope const input;
	AssetHandle const actions = AssetManager::AddMemoryAsset(MakeAsset(JumpMap()), "Actions.sinput");

	for (float const framesPerSecond : {30.0f, 60.0f, 144.0f})
	{
		CAPTURE(framesPerSecond);
		Input::Reset();
		Scene scene("Rates");
		scene.OnRuntimeStart(Settings(actions));
		REQUIRE(scene.GetInputActions().GetActions() != nullptr);
		uint32_t const jump = scene.GetInputActions().Resolve("Jump").value_or(99);
		REQUIRE(jump == 0);

		float const frameTime = 1.0f / framesPerSecond;
		uint32_t presses = 0;
		uint32_t releases = 0;
		uint32_t pressSightings = 0;
		uint32_t releaseSightings = 0;
		uint32_t zeroStepFrames = 0;
		// Edges waiting for a step: the step number the first step after them will have.
		std::vector<uint64_t> waitingPresses;
		std::vector<uint64_t> waitingReleases;
		bool down = false;
		for (uint32_t frame = 0; frame < static_cast<uint32_t>(framesPerSecond) * 4; frame++)
		{
			// A press or a release every 50 ms or so, so that no two edges of one kind wait for the same step.
			uint32_t const period = std::max(2u, static_cast<uint32_t>(framesPerSecond * 0.05f));
			if (frame % period == 1)
			{
				down = !down;
				Input::SetKeyState(KeyCode::Space, down);
			}
			uint64_t const nextStep = scene.GetInputActions().GetFixedStepNumber() + 1;
			FrameSightings const seen = RunFrame(scene, frameTime, jump);
			InputActionState const& state = scene.GetInputActions();
			// OnUpdate's view: the frame's own edges.
			if (state.IsPressed(jump))
			{
				presses++;
				waitingPresses.push_back(nextStep);
			}
			if (state.IsReleased(jump))
			{
				releases++;
				waitingReleases.push_back(nextStep);
			}
			zeroStepFrames += seen.Steps == 0 ? 1u : 0u;
			CHECK(seen.Presses <= 1);
			CHECK(seen.Releases <= 1);
			if (seen.Presses == 1)
			{
				REQUIRE(waitingPresses.size() == 1);
				CHECK(seen.PressStep == waitingPresses.front());
				waitingPresses.clear();
			}
			if (seen.Releases == 1)
			{
				REQUIRE(waitingReleases.size() == 1);
				CHECK(seen.ReleaseStep == waitingReleases.front());
				waitingReleases.clear();
			}
			if (seen.Steps > 0)
			{
				// A frame that ran a step leaves nothing waiting.
				CHECK(waitingPresses.empty());
				CHECK(waitingReleases.empty());
			}
			pressSightings += seen.Presses;
			releaseSightings += seen.Releases;
		}
		CHECK(presses > 10);
		CHECK(releases > 10);
		CHECK(pressSightings + static_cast<uint32_t>(waitingPresses.size()) == presses);
		CHECK(releaseSightings + static_cast<uint32_t>(waitingReleases.size()) == releases);
		// At 144 frames per second most frames run no step and carry their edges; at 30 every frame runs two.
		if (framesPerSecond > 60.0f)
		{
			CHECK(zeroStepFrames > 0);
		}
		else
		{
			CHECK(zeroStepFrames == 0);
		}
		scene.OnRuntimeStop();
	}
}

TEST_CASE("SceneInputActions: an eight-step frame shows an edge to its first step, and frames that run none carry it")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	InputScope const input;
	AssetHandle const actions = AssetManager::AddMemoryAsset(MakeAsset(JumpMap()), "Actions.sinput");
	Scene scene("Steps");
	scene.OnRuntimeStart(Settings(actions));
	RunFrame(scene, Step, 0);

	Input::SetKeyState(KeyCode::Space, true);
	FrameSightings const eight = RunFrame(scene, Step * 8.0f, 0);
	CHECK(eight.Steps == 8);
	CHECK(eight.Presses == 1);
	CHECK(eight.PressStep == eight.FirstStep);

	// A time scale of 0: frames run no step, and the release waits for the first one that does.
	scene.SetTimeScale(0.0f);
	Input::SetKeyState(KeyCode::Space, false);
	FrameSightings const stopped = RunFrame(scene, Step, 0);
	CHECK(stopped.Steps == 0);
	CHECK(scene.GetInputActions().IsReleased(0));
	for (int frame = 0; frame < 5; frame++)
	{
		CHECK(RunFrame(scene, Step, 0).Steps == 0);
	}
	scene.SetTimeScale(1.0f);
	FrameSightings const resumed = RunFrame(scene, Step, 0);
	CHECK(resumed.Steps == 1);
	CHECK(resumed.Releases == 1);
	CHECK(resumed.Presses == 0);
	CHECK(RunFrame(scene, Step, 0).Releases == 0);

	// A press and release inside one frame reach one step as both edges, with the action up.
	Input::SetKeyState(KeyCode::Space, true);
	Input::SetKeyState(KeyCode::Space, false);
	FrameSightings const tap = RunFrame(scene, Step * 2.0f, 0);
	CHECK(tap.Steps == 2);
	CHECK(tap.Presses == 1);
	CHECK(tap.Releases == 1);
	CHECK(tap.PressStep == tap.FirstStep);
	CHECK(tap.ReleaseStep == tap.FirstStep);
	CHECK_FALSE(scene.GetInputActions().IsDown(0));
	scene.OnRuntimeStop();
}

TEST_CASE(
	"SceneInputActions: a key set before a frame is its action's value in that frame, and a paused scene keeps a tap for its next frame")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	InputScope const input;
	AssetHandle const actions = AssetManager::AddMemoryAsset(MakeAsset(JumpMap()), "Actions.sinput");
	Scene scene("Paused");
	scene.OnRuntimeStart(Settings(actions));
	InputActionState const& state = scene.GetInputActions();

	// No frame of latency: the frame that follows the key reads it.
	Input::SetKeyState(KeyCode::D, true);
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(state.GetValue(1) == 1.0f);
	CHECK(state.IsPressed(1));
	Input::EndFrame();

	// Paused: frames run nothing, and the input frame ends after each (the player's application does so). A tap given
	// meanwhile reaches the frame after the pause, once.
	scene.SetPaused(true);
	Input::SetKeyState(KeyCode::Space, true);
	scene.OnUpdateRuntime(Timestep(Step));
	Input::EndFrame();
	Input::SetKeyState(KeyCode::Space, false);
	for (int frame = 0; frame < 3; frame++)
	{
		scene.OnUpdateRuntime(Timestep(Step));
		Input::EndFrame();
	}
	CHECK_FALSE(state.IsPressed(0));
	// One queued frame of a paused scene (play.step) runs, and evaluates.
	scene.Step();
	scene.OnUpdateRuntime(Timestep(Step));
	Input::EndFrame();
	CHECK(state.IsPressed(0));
	CHECK(state.IsReleased(0));
	CHECK_FALSE(state.IsDown(0));
	scene.Step();
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK_FALSE(state.IsPressed(0));
	CHECK_FALSE(state.IsReleased(0));
	scene.OnRuntimeStop();
	CHECK(state.GetActions() == nullptr);
}

TEST_CASE("SceneInputActions: an edited asset or curve reaches the running scene at its next frame without an edge")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	InputScope const input;

	TypedDataAssetType<Curve> const* curveType = DataAssetRegistry::Find<Curve>();
	REQUIRE(curveType != nullptr);
	Ref<TypedDataAsset<Curve>> const curve = std::static_pointer_cast<TypedDataAsset<Curve>>(curveType->CreateAsset());
	Curve halfway;
	halfway.Keys = {CurveKey{0.0f, 0.0f, CurveInterpolation::Linear}, CurveKey{1.0f, 0.5f, CurveInterpolation::Linear}};
	REQUIRE(curve->SetData(halfway).IsOk());
	AssetHandle const curveHandle = AssetManager::AddMemoryAsset(curve, "Half.scurve");

	Ref<TypedDataAsset<InputActionMap>> const asset = MakeAsset(JumpMap());
	AssetHandle const actions = AssetManager::AddMemoryAsset(asset, "Actions.sinput");
	Scene scene("Edits");
	Input::SetKeyState(KeyCode::Space, true);
	uint64_t const logStart = Log::GetNextEntryIndex();
	scene.OnRuntimeStart(Settings(actions));
	CHECK(CountLogged(logStart, "Input actions: 2 actions from") == 1);
	InputActionState const& state = scene.GetInputActions();
	// Held as the scene starts: down, no edge.
	CHECK(state.IsDown(0));
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(state.IsDown(0));
	CHECK(state.GetEdges().empty());
	uint64_t const version = state.GetActions()->GetVersion();

	// An unrelated data asset edit makes no new set.
	REQUIRE(curve->SetData(halfway).IsOk());
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(state.GetActions()->GetVersion() == version);

	// The asset edited: Jump now also reads E through the curve; Space still holds it, and nothing is reported.
	InputActionMap edited = JumpMap();
	InputActionBinding throughCurve = Bound("Key/E");
	throughCurve.ResponseCurve = curveHandle;
	edited.Actions[0].Bindings.push_back(throughCurve);
	edited.Actions.push_back(MakeAction("Fire", InputActionType::Button, {Bound("Key/F")}));
	REQUIRE(asset->SetData(edited).IsOk());
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(state.GetActions()->GetVersion() != version);
	CHECK(state.GetActions()->GetActions().size() == 3);
	CHECK(state.IsDown(0));
	CHECK(state.GetEdges().empty());
	Input::SetKeyState(KeyCode::Space, false);
	Input::SetKeyState(KeyCode::E, true);
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(state.GetValue(0) == doctest::Approx(0.5f));
	CHECK(state.IsDown(0));

	// The curve edited: the next frame reads through the new keys.
	Curve full = halfway;
	full.Keys[1].Value = 0.25f;
	REQUIRE(curve->SetData(full).IsOk());
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(state.GetValue(0) == doctest::Approx(0.25f));
	CHECK(state.GetEdges().size() == 1);
	scene.OnRuntimeStop();
}

TEST_CASE("SceneInputActions: a scene without actions evaluates nothing, and one whose asset does not load says so once")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	InputScope const input;
	{
		Scene scene("None");
		scene.OnRuntimeStart(Settings(AssetHandle()));
		Input::SetKeyState(KeyCode::Space, true);
		for (int frame = 0; frame < 3; frame++)
		{
			scene.OnUpdateRuntime(Timestep(Step * 2.0f));
		}
		CHECK(scene.GetInputActions().GetActions() == nullptr);
		CHECK(scene.GetInputActions().GetFixedStepNumber() == 6);
		CHECK_FALSE(scene.GetInputActions().Resolve("Jump").has_value());
		scene.OnRuntimeStop();
	}
	{
		uint64_t const logStart = Log::GetNextEntryIndex();
		Scene scene("Missing");
		scene.OnRuntimeStart(Settings(AssetHandle(UUID(123456789))));
		for (int frame = 0; frame < 3; frame++)
		{
			scene.OnUpdateRuntime(Timestep(Step));
		}
		CHECK(CountLogged(logStart, "does not load as an InputActions asset") == 1);
		CHECK(scene.GetInputActions().GetActions() == nullptr);
		scene.OnRuntimeStop();
	}
}
