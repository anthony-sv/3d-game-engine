#include "Core/ProfilerTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/CurveAsset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionMap.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Core/Platform.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Math/Curve.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr size_t ButtonAndAxisActions = 48;
	constexpr size_t VectorActions = 16;

	struct BenchmarkInputScope
	{
		BenchmarkInputScope() { Input::Reset(); }
		~BenchmarkInputScope() { Input::Reset(); }
		BenchmarkInputScope(BenchmarkInputScope const&) = delete;
		BenchmarkInputScope& operator=(BenchmarkInputScope const&) = delete;
	};

	// The keys a binding can name, in KeyCode order.
	std::vector<KeyCode> NamedKeys()
	{
		std::vector<KeyCode> keys;
		for (uint16_t code = 1; code < KeyCodeCount; code++)
		{
			KeyCode const key = static_cast<KeyCode>(code);
			std::string const name = KeyCodeToString(key);
			if (KeyCodeFromString(name) == key && ParseInputControl("Key/" + name).has_value())
			{
				keys.push_back(key);
			}
		}
		return keys;
	}

	std::string KeyName(std::vector<KeyCode> const& keys, size_t index)
	{
		return std::string("Key/") + KeyCodeToString(keys[index % keys.size()]);
	}

	InputActionBinding Bound(std::string controls, float deadZone = 0.0f, AssetHandle curve = {}, int32_t gamepad = AnyGamepad)
	{
		InputActionBinding binding;
		binding.Controls = std::move(controls);
		binding.DeadZone = deadZone;
		binding.ResponseCurve = curve;
		binding.Gamepad = gamepad;
		return binding;
	}

	// A cubic response: 0 at 0 with a flat start, 1 at 1 arriving at a slope of 3, as a game's stick curve would.
	AssetHandle AddResponseCurve()
	{
		TypedDataAssetType<Curve> const* curveType = DataAssetRegistry::Find<Curve>();
		REQUIRE(curveType != nullptr);
		Ref<TypedDataAsset<Curve>> const curve = std::static_pointer_cast<TypedDataAsset<Curve>>(curveType->CreateAsset());
		Curve response;
		response.Keys = {CurveKey{0.0f, 0.0f}, CurveKey{1.0f, 1.0f}};
		response.Keys[0].HasOut = true;
		response.Keys[0].Out = 0.0f;
		response.Keys[1].HasIn = true;
		response.Keys[1].In = 3.0f;
		REQUIRE(curve->SetData(response).IsOk());
		return AssetManager::AddMemoryAsset(curve, "Response.scurve");
	}

	// 64 actions of three bindings each, every kind of binding among them: 48 Button and Axis actions (a key; a gamepad
	// button or trigger of any pad with a dead zone and the response curve; an axis of two keys) and 16 Vector2 actions (a
	// stick of any pad with a radial dead zone and the curve; a vector of four keys; a stick of pad 0).
	InputActionMap MakeMap(std::vector<KeyCode> const& keys, AssetHandle curve)
	{
		std::array<char const*, 6> const padButtons = {"Gamepad/A", "Gamepad/B",          "Gamepad/X",
		                                               "Gamepad/Y", "Gamepad/LeftBumper", "Gamepad/RightBumper"};
		InputActionMap map;
		for (size_t index = 0; index < ButtonAndAxisActions; index++)
		{
			InputAction action;
			action.Name = "Action" + std::to_string(index);
			action.Type = index % 2 == 0 ? InputActionType::Button : InputActionType::Axis;
			std::string const pad =
				index % 4 == 1 ? std::string("Gamepad/RightTrigger")
							   : (index % 4 == 3 ? std::string("Gamepad/LeftTrigger") : std::string(padButtons[index % padButtons.size()]));
			action.Bindings = {Bound(KeyName(keys, index)), Bound(pad, 0.1f, curve),
			                   Bound(KeyName(keys, 48 + 2 * index) + " " + KeyName(keys, 49 + 2 * index))};
			map.Actions.push_back(std::move(action));
		}
		for (size_t index = 0; index < VectorActions; index++)
		{
			InputAction action;
			action.Name = "Vector" + std::to_string(index);
			action.Type = InputActionType::Vector2;
			std::string composite;
			for (size_t part = 0; part < 4; part++)
			{
				composite += (part == 0 ? "" : " ") + KeyName(keys, 7 * index + part);
			}
			action.Bindings = {Bound(index % 2 == 0 ? "Gamepad/LeftStick" : "Gamepad/RightStick", 0.15f, curve), Bound(composite),
			                   Bound("Gamepad/RightStick", 0.0f, {}, 0)};
			map.Actions.push_back(std::move(action));
		}
		return map;
	}

	// Changes the input as a hard frame of play would, so that actions go down and up again every frame and the edge paths
	// run: eight keys change, and every pad goes between rest and its sticks, triggers and one button moved, each pad to
	// its own values (the any-pad bindings read and compare all sixteen either way).
	void ChangeInput(std::vector<KeyCode> const& keys, uint32_t frame)
	{
		for (uint32_t index = 0; index < 8; index++)
		{
			Input::SetKeyState(keys[((frame / 2) * 8 + index) % keys.size()], frame % 2 == 0);
		}
		for (uint32_t pad = 0; pad < MaxGamepads; pad++)
		{
			Input::GamepadState state = Input::GetRestingGamepadState();
			if (frame % 2 == 0)
			{
				float const phase = static_cast<float>(frame) * 0.05f + static_cast<float>(pad);
				float const reach = 0.6f + 0.4f * static_cast<float>(pad) / static_cast<float>(MaxGamepads);
				state.Axes[static_cast<size_t>(GamepadAxis::LeftX)] = reach * std::sin(phase);
				state.Axes[static_cast<size_t>(GamepadAxis::LeftY)] = reach * std::cos(phase);
				state.Axes[static_cast<size_t>(GamepadAxis::RightX)] = reach * std::cos(phase * 1.3f);
				state.Axes[static_cast<size_t>(GamepadAxis::RightY)] = reach * std::sin(phase * 1.3f);
				state.Axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = reach;
				state.Axes[static_cast<size_t>(GamepadAxis::LeftTrigger)] = reach;
				state.Buttons[(frame / 2 + pad) % GamepadButtonCount] = true;
			}
			Input::InjectGamepadState(pad, state);
		}
	}

	double Percentile(std::vector<double> values, double fraction)
	{
		std::sort(values.begin(), values.end());
		size_t const index = std::min(values.size() - 1, static_cast<size_t>(fraction * static_cast<double>(values.size())));
		return values[index];
	}

	double FindZone(ProfilerMedians const& medians, std::string_view name)
	{
		for (ProfilerZoneMedian const& zone : medians.Zones)
		{
			if (zone.Name == name)
			{
				return zone.MillisecondsPerFrame;
			}
		}
		return 0.0;
	}
}

TEST_CASE("SceneInputActions: 64 actions of three bindings each, benchmark" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_INPUT_ACTIONS_BENCHMARK")))
{
	// A measurement, never asserted on timings: run with STRADA_INPUT_ACTIONS_BENCHMARK=1 in a Release build.
	using Clock = std::chrono::steady_clock;
	constexpr uint32_t Evaluations = 10000;
	constexpr uint32_t ProfiledFrames = 600;
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ProfilerScope const profiler;
	BenchmarkInputScope const input;

	std::vector<KeyCode> const keys = NamedKeys();
	REQUIRE(keys.size() >= 64);
	InputActionMap const map = MakeMap(keys, AddResponseCurve());
	REQUIRE(map.Actions.size() == ButtonAndAxisActions + VectorActions);
	TypedDataAssetType<InputActionMap> const* type = DataAssetRegistry::Find<InputActionMap>();
	REQUIRE(type != nullptr);
	Ref<TypedDataAsset<InputActionMap>> const asset = std::static_pointer_cast<TypedDataAsset<InputActionMap>>(type->CreateAsset());
	Result<void> const set = asset->SetData(map);
	REQUIRE_MESSAGE(set.IsOk(), (set ? std::string() : set.GetError()));
	AssetHandle const handle = AssetManager::AddMemoryAsset(asset, "Benchmark.sinput");

	SceneRuntimeSettings settings;
	settings.RunScripts = false;
	settings.PlayAudio = false;
	settings.InputActions = handle;
	Scene scene("Input actions benchmark");
	scene.OnRuntimeStart(settings);
	InputActionState& state = scene.GetInputActions();
	REQUIRE(state.GetActions() != nullptr);
	REQUIRE(state.GetActions()->GetActions().size() == 64);
	for (InputActionSpecification const& action : state.GetActions()->GetActions())
	{
		CHECK(action.Bindings.size() == 3);
	}

	// No frame of latency: a key set before a frame is the action's value in that frame.
	Input::SetKeyState(keys[0], true);
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	Input::EndFrame();
	CHECK(state.GetValue(0) == 1.0f);
	CHECK(state.IsPressed(0));

	// The evaluation alone, timed per call, each after a change of the input.
	std::vector<double> evaluations;
	evaluations.reserve(Evaluations);
	size_t edges = 0;
	for (uint32_t frame = 0; frame < Evaluations; frame++)
	{
		ChangeInput(keys, frame);
		auto const start = Clock::now();
		state.Evaluate();
		auto const end = Clock::now();
		evaluations.push_back(std::chrono::duration<double, std::milli>(end - start).count());
		edges += state.GetEdges().size();
		Input::EndFrame();
	}
	CHECK(edges > Evaluations);

	// The frame's call, as the profiler times it in the scene's frames: Scene.InputActions (the epoch's check, the
	// evaluation and the events, which a scene that runs no scripts skips) around Input.EvaluateActions.
	REQUIRE(Profiler::Start().IsOk());
	for (uint32_t frame = 0; frame < ProfiledFrames; frame++)
	{
		ChangeInput(keys, Evaluations + frame);
		Profiler::BeginFrame();
		scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
		Profiler::EndFrame();
		Input::EndFrame();
	}
	Profiler::Stop();
	ProfilerMedians const medians = ComputeProfilerMedians(Profiler::GetRecording());
	CHECK(Profiler::GetStatus().DroppedZones == 0);
	scene.OnRuntimeStop();

	MESSAGE("64 actions x 3 bindings, 16 pads injected, the input changed before each evaluation: Evaluate median "
	        << Percentile(evaluations, 0.5) << " ms, 99th percentile " << Percentile(evaluations, 0.99) << " ms, maximum "
	        << Percentile(evaluations, 1.0) << " ms over " << Evaluations << " evaluations (" << edges << " edges)");
	MESSAGE("In " << ProfiledFrames << " profiled frames: Scene.InputActions " << FindZone(medians, "Scene.InputActions")
	              << " ms per frame, Input.EvaluateActions " << FindZone(medians, "Input.EvaluateActions") << " ms per frame");
}
