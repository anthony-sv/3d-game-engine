#include "Strada/Core/Input.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Core/Log.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	struct InputActionsFixture
	{
		InputActionsFixture() { Input::Reset(); }
		~InputActionsFixture() { Input::Reset(); }
		InputActionsFixture(InputActionsFixture const&) = delete;
		InputActionsFixture& operator=(InputActionsFixture const&) = delete;
	};

	InputControlList Controls(std::string_view text)
	{
		Result<InputControlList> parsed = ParseInputControls(text);
		REQUIRE_MESSAGE(parsed.IsOk(), (parsed ? std::string() : parsed.GetError()));
		return parsed.GetValue();
	}

	InputBindingSpecification Binding(std::string_view text, int32_t gamepad = AnyGamepad)
	{
		InputBindingSpecification binding;
		binding.Controls = Controls(text);
		binding.Gamepad = gamepad;
		return binding;
	}

	InputActionSpecification Action(std::string name, InputActionType type, std::vector<InputBindingSpecification> bindings)
	{
		InputActionSpecification action;
		action.Name = std::move(name);
		action.Type = type;
		action.Bindings = std::move(bindings);
		return action;
	}

	Ref<InputActionSet const> MakeSet(std::vector<InputActionSpecification> actions)
	{
		Result<Ref<InputActionSet const>> set = InputActionSet::Create(std::move(actions));
		REQUIRE_MESSAGE(set.IsOk(), (set ? std::string() : set.GetError()));
		return set.GetValue();
	}

	std::string CreateError(std::vector<InputActionSpecification> actions)
	{
		Result<Ref<InputActionSet const>> set = InputActionSet::Create(std::move(actions));
		REQUIRE(set.IsError());
		return set.GetError();
	}

	Input::GamepadState Pad()
	{
		return Input::GetRestingGamepadState();
	}

	void SetAxis(Input::GamepadState& state, GamepadAxis axis, float value)
	{
		state.Axes[static_cast<size_t>(axis)] = value;
	}

	void SetButton(Input::GamepadState& state, GamepadButton button, bool down)
	{
		state.Buttons[static_cast<size_t>(button)] = down;
	}

	std::vector<InputActionEdge> Edges(InputActionState const& state)
	{
		return {state.GetEdges().begin(), state.GetEdges().end()};
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

	glm::vec2 Read(std::string_view text, int32_t gamepad = AnyGamepad)
	{
		return InputProcessing::ReadControls(Controls(text), gamepad);
	}
}

TEST_CASE("InputActions: every key, mouse button and gamepad control has a name that reads back, in any letter case")
{
	std::set<std::string> names;
	for (uint16_t code = 1; code < KeyCodeCount; code++)
	{
		std::string_view const key = KeyCodeToString(static_cast<KeyCode>(code));
		if (key == "Unknown")
		{
			continue;
		}
		CAPTURE(key);
		CHECK(key.find(' ') == std::string_view::npos);
		InputControl const control{InputControlKind::Key, code};
		std::string const name = FormatInputControl(control);
		CHECK(name == fmt::format("Key/{}", key));
		CHECK(ParseInputControl(name) == control);
		std::string lower = name;
		std::ranges::transform(lower, lower.begin(),
		                       [](char character)
		                       {
								   return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
							   });
		CHECK(ParseInputControl(lower) == control);
		CHECK(names.insert(lower).second);
	}
	for (uint16_t button = 0; button < MouseButtonCount; button++)
	{
		InputControl const control{InputControlKind::Mouse, button};
		CHECK(ParseInputControl(FormatInputControl(control)) == control);
	}
	CHECK(ParseInputControl("mouse/LEFT") == InputControl{InputControlKind::Mouse, 0});

	// Gamepad buttons, axes and sticks share the device's names: they must not collide.
	std::set<std::string> padNames;
	for (uint16_t button = 0; button < GamepadButtonCount; button++)
	{
		InputControl const control{InputControlKind::PadButton, button};
		std::string const name = FormatInputControl(control);
		CHECK(ParseInputControl(name) == control);
		CHECK(GamepadButtonFromString(GamepadButtonToString(static_cast<GamepadButton>(button))) == static_cast<GamepadButton>(button));
		CHECK(padNames.insert(name).second);
	}
	for (uint16_t axis = 0; axis < GamepadAxisCount; axis++)
	{
		InputControl const control{InputControlKind::PadAxis, axis};
		std::string const name = FormatInputControl(control);
		CHECK(ParseInputControl(name) == control);
		CHECK(GamepadAxisFromString(GamepadAxisToString(static_cast<GamepadAxis>(axis))) == static_cast<GamepadAxis>(axis));
		CHECK(padNames.insert(name).second);
	}
	for (uint16_t stick = 0; stick < 2; stick++)
	{
		InputControl const control{InputControlKind::PadStick, stick};
		std::string const name = FormatInputControl(control);
		CHECK(ParseInputControl(name) == control);
		CHECK(padNames.insert(name).second);
	}
	CHECK(FormatInputControl({InputControlKind::PadStick, 1}) == "Gamepad/RightStick");
	CHECK(FormatInputControl({InputControlKind::PadAxis, static_cast<uint16_t>(GamepadAxis::RightTrigger)}) == "Gamepad/RightTrigger");
	CHECK(ParseInputControl("GAMEPAD/dpadup") == InputControl{InputControlKind::PadButton, static_cast<uint16_t>(GamepadButton::DPadUp)});

	for (std::string_view const bad : {"", "Key", "Key/", "/A", "Keyboard/A", "Key/A/B", "Key/Nope", "Key/Unknown", "Gamepad/Space",
	                                   "Mouse/Button8", "Gamepad/Stick", "Key/ A", "Key/A "})
	{
		CAPTURE(bad);
		CHECK_FALSE(ParseInputControl(bad).has_value());
	}
}

TEST_CASE("InputActions: a binding names none, one, two or four controls, and each action type takes the ones that fit it")
{
	CHECK(Controls("").Count == 0);
	CHECK(Controls("   ").Count == 0);
	CHECK(Controls("Key/Space").Count == 1);
	CHECK(Controls("  Key/A   Key/D ").Count == 2);
	InputControlList const vector = Controls("Key/W Key/S Key/A Key/D");
	REQUIRE(vector.Count == 4);
	CHECK(vector.Controls[0] == InputControl{InputControlKind::Key, static_cast<uint16_t>(KeyCode::W)});
	CHECK(vector.Controls[3] == InputControl{InputControlKind::Key, static_cast<uint16_t>(KeyCode::D)});
	CHECK(ParseInputControls("Key/A Key/B Key/C").GetError().find("not 3") != std::string::npos);
	CHECK(ParseInputControls("Key/A Key/B Key/C Key/D Key/E").GetError().find("not 5") != std::string::npos);
	CHECK(ParseInputControls("Key/A Key/Nope").GetError().find("'Key/Nope' names no control") != std::string::npos);

	auto const fits = [](InputActionType type, std::string_view text)
	{
		return CheckInputBindingControls(type, Controls(text)).IsOk();
	};
	for (InputActionType const type : {InputActionType::Button, InputActionType::Axis, InputActionType::Vector2})
	{
		CHECK(fits(type, ""));
	}
	CHECK(fits(InputActionType::Button, "Key/Space"));
	CHECK(fits(InputActionType::Button, "Gamepad/RightTrigger"));
	CHECK(fits(InputActionType::Axis, "Key/A Key/D"));
	CHECK(fits(InputActionType::Axis, "Gamepad/LeftX"));
	CHECK(fits(InputActionType::Axis, "Gamepad/LeftTrigger Gamepad/RightTrigger"));
	CHECK_FALSE(fits(InputActionType::Axis, "Gamepad/LeftStick"));
	CHECK_FALSE(fits(InputActionType::Button, "Key/W Key/S Key/A Key/D"));
	CHECK(fits(InputActionType::Vector2, "Gamepad/LeftStick"));
	CHECK(fits(InputActionType::Vector2, "Key/W Key/S Key/A Key/D"));
	CHECK_FALSE(fits(InputActionType::Vector2, "Key/W"));
	CHECK_FALSE(fits(InputActionType::Vector2, "Key/A Key/D"));
	CHECK_FALSE(fits(InputActionType::Vector2, "Gamepad/LeftStick Gamepad/RightStick"));
	CHECK_FALSE(fits(InputActionType::Vector2, "Gamepad/LeftStick Key/S Key/A Key/D"));
}

TEST_CASE_FIXTURE(InputActionsFixture, "InputActions: each control reads its device: keys, buttons, sticks up positive, triggers 0 to 1")
{
	CHECK(Read("Key/Space") == glm::vec2(0.0f));
	Input::SetKeyState(KeyCode::Space, true);
	CHECK(Read("Key/Space") == glm::vec2(1.0f, 0.0f));
	Input::SetMouseButtonState(MouseButton::Right, true);
	CHECK(Read("Mouse/Right") == glm::vec2(1.0f, 0.0f));
	CHECK(Read("Mouse/Left") == glm::vec2(0.0f));

	// No pad connected: every gamepad control reads 0.
	CHECK(Read("Gamepad/A") == glm::vec2(0.0f));
	CHECK(Read("Gamepad/LeftStick") == glm::vec2(0.0f));

	Input::GamepadState pad = Pad();
	Input::SetGamepadState(2, pad);
	SetButton(pad, GamepadButton::A, true);
	SetAxis(pad, GamepadAxis::LeftX, 0.5f);
	// GLFW gives a stick pushed up as -1.
	SetAxis(pad, GamepadAxis::LeftY, -0.25f);
	SetAxis(pad, GamepadAxis::RightTrigger, 0.0f);
	Input::SetGamepadState(2, pad);
	CHECK(Read("Gamepad/A") == glm::vec2(1.0f, 0.0f));
	CHECK(Read("Gamepad/A", 2) == glm::vec2(1.0f, 0.0f));
	CHECK(Read("Gamepad/A", 1) == glm::vec2(0.0f));
	CHECK(Read("Gamepad/A", 16) == glm::vec2(0.0f));
	CHECK(Read("Gamepad/LeftX") == glm::vec2(0.5f, 0.0f));
	CHECK(Read("Gamepad/LeftY") == glm::vec2(0.25f, 0.0f));
	CHECK(Read("Gamepad/LeftStick") == glm::vec2(0.5f, 0.25f));
	// A trigger reads (raw + 1) / 2 once it has read anything but exactly 0: half way, here (it rested at -1 first).
	CHECK(Read("Gamepad/RightTrigger") == glm::vec2(0.5f, 0.0f));
	CHECK(Read("Gamepad/LeftTrigger") == glm::vec2(0.0f));

	// A stick's diagonal is limited to length 1 (GLFW clamps each axis on its own).
	SetAxis(pad, GamepadAxis::RightX, 1.0f);
	SetAxis(pad, GamepadAxis::RightY, -1.0f);
	Input::SetGamepadState(2, pad);
	glm::vec2 const diagonal = Read("Gamepad/RightStick");
	CHECK(diagonal.x == doctest::Approx(std::sqrt(0.5f)));
	CHECK(diagonal.y == doctest::Approx(std::sqrt(0.5f)));

	// A pad whose mapping lacks a trigger reads 0 there forever: at rest, not half pressed.
	Input::GamepadState unmapped;
	unmapped.Connected = true;
	Input::SetGamepadState(5, unmapped);
	CHECK(Read("Gamepad/LeftTrigger", 5) == glm::vec2(0.0f));
	CHECK_FALSE(Input::HasGamepadAxisMoved(5, GamepadAxis::LeftTrigger));
	SetAxis(unmapped, GamepadAxis::LeftTrigger, 1.0f);
	Input::SetGamepadState(5, unmapped);
	CHECK(Read("Gamepad/LeftTrigger", 5) == glm::vec2(1.0f, 0.0f));
	SetAxis(unmapped, GamepadAxis::LeftTrigger, 0.0f);
	Input::SetGamepadState(5, unmapped);
	CHECK(Read("Gamepad/LeftTrigger", 5) == glm::vec2(0.5f, 0.0f));

	// Any gamepad reads the largest magnitude among the connected ones; the first wins a tie.
	Input::GamepadState other = Pad();
	SetAxis(other, GamepadAxis::LeftX, -0.75f);
	Input::SetGamepadState(7, other);
	CHECK(Read("Gamepad/LeftX") == glm::vec2(-0.75f, 0.0f));
	CHECK(Read("Gamepad/LeftX", 2) == glm::vec2(0.5f, 0.0f));
	SetAxis(other, GamepadAxis::LeftX, 0.5f);
	SetAxis(other, GamepadAxis::LeftY, 0.25f);
	Input::SetGamepadState(7, other);
	CHECK(Read("Gamepad/LeftY") == glm::vec2(0.25f, 0.0f));
	CHECK(Read("Gamepad/LeftY", 7) == glm::vec2(-0.25f, 0.0f));
}

TEST_CASE_FIXTURE(InputActionsFixture, "InputActions: composites take each part's positive half, and a vector of keys reaches length 1")
{
	CHECK(Read("Key/A Key/D") == glm::vec2(0.0f));
	Input::SetKeyState(KeyCode::D, true);
	CHECK(Read("Key/A Key/D") == glm::vec2(1.0f, 0.0f));
	Input::SetKeyState(KeyCode::A, true);
	CHECK(Read("Key/A Key/D") == glm::vec2(0.0f));
	Input::SetKeyState(KeyCode::D, false);
	CHECK(Read("Key/A Key/D") == glm::vec2(-1.0f, 0.0f));

	// up, down, left, right
	CHECK(Read("Key/W Key/S Key/A Key/D") == glm::vec2(-1.0f, 0.0f));
	Input::SetKeyState(KeyCode::W, true);
	glm::vec2 const diagonal = Read("Key/W Key/S Key/A Key/D");
	CHECK(diagonal.x == doctest::Approx(-0.70710678f));
	CHECK(diagonal.y == doctest::Approx(0.70710678f));
	CHECK(glm::length(diagonal) == doctest::Approx(1.0f));

	// Triggers as an axis's parts; a stick axis as a part reads only its positive half.
	Input::GamepadState pad = Pad();
	Input::SetGamepadState(0, pad);
	SetAxis(pad, GamepadAxis::LeftTrigger, 0.0f);
	SetAxis(pad, GamepadAxis::RightTrigger, 1.0f);
	SetAxis(pad, GamepadAxis::LeftX, -0.6f);
	Input::SetGamepadState(0, pad);
	CHECK(Read("Gamepad/LeftTrigger Gamepad/RightTrigger") == glm::vec2(0.5f, 0.0f));
	CHECK(Read("Gamepad/LeftX Key/Space") == glm::vec2(0.0f));
	CHECK(InputProcessing::CombineControls(Controls("Key/A Key/D"), std::array{glm::vec2(0.25f, 0.0f), glm::vec2(1.0f, 0.0f)}) ==
	      glm::vec2(0.75f, 0.0f));
}

TEST_CASE("InputActions: the dead zone, the response, inversion and scale follow their closed forms, in that order")
{
	using InputProcessing::ApplyDeadZone;
	using InputProcessing::ApplyResponse;
	CHECK(ApplyDeadZone(glm::vec2(0.3f, 0.0f), 0.0f) == glm::vec2(0.3f, 0.0f));
	CHECK(ApplyDeadZone(glm::vec2(0.2f, 0.0f), 0.2f) == glm::vec2(0.0f));
	CHECK(ApplyDeadZone(glm::vec2(-0.6f, 0.0f), 0.2f).x == doctest::Approx(-0.5f));
	CHECK(ApplyDeadZone(glm::vec2(1.0f, 0.0f), 0.99f).x == doctest::Approx(1.0f));
	CHECK(ApplyDeadZone(glm::vec2(0.995f, 0.0f), 0.99f).x == doctest::Approx(0.5f));
	// Radial: the direction kept, the length stretched.
	glm::vec2 const radial = ApplyDeadZone(glm::vec2(0.36f, 0.48f), 0.2f);
	CHECK(glm::length(radial) == doctest::Approx(0.5f));
	CHECK(radial.x / radial.y == doctest::Approx(0.75f));
	CHECK(ApplyDeadZone(glm::vec2(0.1f, 0.1f), 0.2f) == glm::vec2(0.0f));

	InputResponse const square = [](float value)
	{
		return value * value;
	};
	CHECK(ApplyResponse(glm::vec2(-0.5f, 0.0f), square).x == doctest::Approx(-0.25f));
	glm::vec2 const bent = ApplyResponse(glm::vec2(0.0f, -0.5f), square);
	CHECK(bent.x == doctest::Approx(0.0f));
	CHECK(bent.y == doctest::Approx(-0.25f));
	CHECK(ApplyResponse(glm::vec2(0.3f, 0.4f), square) == glm::vec2(0.3f, 0.4f) * 0.5f);
	CHECK(ApplyResponse(glm::vec2(0.0f),
	                    [](float)
	                    {
							return 0.5f;
						}) == glm::vec2(0.0f));
	CHECK(ApplyResponse(glm::vec2(0.5f, 0.0f),
	                    [](float)
	                    {
							return -1.0f;
						}) == glm::vec2(0.0f));
	CHECK(ApplyResponse(glm::vec2(0.5f, 0.0f),
	                    [](float)
	                    {
							return std::nanf("");
						}) == glm::vec2(0.0f));
	CHECK(ApplyResponse(glm::vec2(0.5f, 0.0f), InputResponse()) == glm::vec2(0.5f, 0.0f));

	// Dead zone 0.2, then the square, then inversion, then a scale of 2: 0.6 gives -2 * ((0.6 - 0.2) / 0.8)^2 = -0.5. In
	// any other order the result would differ (scale before the square: 2; inversion before the dead zone: the same
	// magnitude, but the square of a scaled value: 4 times as much).
	InputBindingSpecification binding = Binding("Gamepad/LeftX");
	binding.DeadZone = 0.2f;
	binding.Response = square;
	binding.Invert = true;
	binding.Scale = 2.0f;
	CHECK(InputProcessing::ProcessBinding(binding, glm::vec2(0.6f, 0.0f)).x == doctest::Approx(-0.5f));
	binding.InvertY = true;
	binding.Invert = false;
	// A vector: Y negated alone.
	glm::vec2 const stick = InputProcessing::ProcessBinding(binding, glm::vec2(0.0f, 0.6f));
	CHECK(stick.x == doctest::Approx(0.0f));
	CHECK(stick.y == doctest::Approx(-0.5f));
}

TEST_CASE("InputActions: a set refuses what an asset's validation refuses, naming the action and the binding")
{
	CHECK(CreateError({Action("", InputActionType::Button, {})}) == "an action needs a name");
	CHECK(CreateError({Action(std::string(65, 'a'), InputActionType::Button, {})}).find("at most 64 bytes") != std::string::npos);
	CHECK(MakeSet({Action(std::string(64, 'a'), InputActionType::Button, {})})->GetActions().size() == 1);
	CHECK(CreateError({Action("Jump", InputActionType::Button, {}), Action("JUMP", InputActionType::Button, {})}).find("action 'JUMP'") !=
	      std::string::npos);

	InputActionSpecification thresholds = Action("Fire", InputActionType::Button, {});
	thresholds.ReleaseThreshold = 0.6f;
	CHECK(CreateError({thresholds}).find("action 'Fire': thresholds") != std::string::npos);
	thresholds.ReleaseThreshold = 0.0f;
	CHECK(CreateError({thresholds}).find("thresholds") != std::string::npos);
	thresholds.ReleaseThreshold = 0.5f;
	thresholds.PressThreshold = 0.5f;
	CHECK(MakeSet({thresholds})->GetActions()[0].PressThreshold == 0.5f);

	CHECK(CreateError({Action("Move", InputActionType::Vector2, {Binding(""), Binding("Key/A Key/D")})}).find("action 'Move', binding 1") !=
	      std::string::npos);
	InputBindingSpecification badPad = Binding("Gamepad/A");
	badPad.Gamepad = 16;
	CHECK(CreateError({Action("A", InputActionType::Button, {badPad})}).find("Gamepad") != std::string::npos);
	InputBindingSpecification badZone = Binding("Gamepad/A");
	badZone.DeadZone = 1.0f;
	CHECK(CreateError({Action("A", InputActionType::Button, {badZone})}).find("DeadZone") != std::string::npos);
	InputBindingSpecification badScale = Binding("Gamepad/A");
	badScale.Scale = -1.0f;
	CHECK(CreateError({Action("A", InputActionType::Button, {badScale})}).find("Scale") != std::string::npos);
	std::vector<InputBindingSpecification> const many(17, Binding("Key/A"));
	CHECK(CreateError({Action("A", InputActionType::Button, many)}).find("at most 16 bindings") != std::string::npos);
	std::vector<InputActionSpecification> tooMany;
	for (int index = 0; index < 257; index++)
	{
		tooMany.push_back(Action(fmt::format("A{}", index), InputActionType::Button, {}));
	}
	CHECK(CreateError(std::move(tooMany)).find("at most 256 actions") != std::string::npos);

	// Names are found with their letter case, and sets never share a version.
	Ref<InputActionSet const> const set = MakeSet({Action("Zoom", InputActionType::Axis, {}), Action("Aim", InputActionType::Vector2, {}),
	                                               Action("Jump", InputActionType::Button, {})});
	CHECK(set->Find("Jump") == 2u);
	CHECK(set->Find("Aim") == 1u);
	CHECK(set->Find("Zoom") == 0u);
	CHECK_FALSE(set->Find("jump").has_value());
	CHECK_FALSE(set->Find("").has_value());
	CHECK(MakeSet({})->GetVersion() != set->GetVersion());
}

TEST_CASE_FIXTURE(InputActionsFixture, "InputActions: the binding of the largest magnitude gives the value, a button its magnitude")
{
	InputBindingSpecification half = Binding("Key/Q");
	half.Scale = 0.5f;
	InputActionState state;
	state.SetActions(MakeSet({
		Action("Steer", InputActionType::Axis, {half, Binding("Key/A Key/D"), Binding("Gamepad/LeftX")}),
		Action("Brake", InputActionType::Button, {Binding("Key/A Key/D")}),
		Action("Move", InputActionType::Vector2, {Binding("Key/W Key/S Key/A Key/D"), Binding("Gamepad/LeftStick")}),
	}));
	state.Evaluate();
	CHECK(state.GetValue(0) == 0.0f);

	Input::SetKeyState(KeyCode::Q, true);
	state.Evaluate();
	CHECK(state.GetValue(0) == 0.5f);
	Input::SetKeyState(KeyCode::A, true);
	state.Evaluate();
	CHECK(state.GetValue(0) == -1.0f);
	// A button reads the magnitude: an axis composite presses it either way.
	CHECK(state.GetValue(1) == 1.0f);
	CHECK(state.IsDown(1));
	CHECK(state.GetVector(2) == glm::vec2(-1.0f, 0.0f));
	CHECK(state.GetValue(2) == 1.0f);

	// Ties go to the first binding: the keys' -1 against the stick's +1.
	Input::GamepadState pad = Pad();
	SetAxis(pad, GamepadAxis::LeftX, 1.0f);
	Input::SetGamepadState(0, pad);
	state.Evaluate();
	CHECK(state.GetValue(0) == -1.0f);
	CHECK(state.GetVector(2) == glm::vec2(-1.0f, 0.0f));
	Input::SetKeyState(KeyCode::A, false);
	state.Evaluate();
	CHECK(state.GetValue(0) == 1.0f);
	CHECK(state.GetVector(2) == glm::vec2(1.0f, 0.0f));
	CHECK(state.GetVector(0) == glm::vec2(1.0f, 0.0f));

	// The override comes after the bindings: it wins only by a larger magnitude.
	REQUIRE(Input::SetActionOverride("Steer", glm::vec2(-1.0f, 0.0f)));
	state.Evaluate();
	CHECK(state.GetValue(0) == 1.0f);
	REQUIRE(Input::SetActionOverride("Steer", glm::vec2(-0.5f, 7.0f)));
	SetAxis(pad, GamepadAxis::LeftX, 0.25f);
	Input::SetGamepadState(0, pad);
	state.Evaluate();
	// Q's 0.5 ties with the override's x (its y is no axis's): the first, Q's, wins.
	CHECK(state.GetValue(0) == 0.5f);
	REQUIRE(Input::SetActionOverride("Move", glm::vec2(0.0f, -0.75f)));
	state.Evaluate();
	CHECK(state.GetVector(2) == glm::vec2(0.0f, -0.75f));
	// Indices the set lacks read zero.
	CHECK(state.GetValue(3) == 0.0f);
	CHECK_FALSE(state.IsDown(3));
	CHECK_FALSE(state.IsPressed(3));
}

TEST_CASE_FIXTURE(InputActionsFixture,
                  "InputActions: a button read from an axis goes down at the press threshold and up below the release one")
{
	InputActionState state;
	state.SetActions(MakeSet({Action("Throttle", InputActionType::Button, {Binding("Gamepad/RightTrigger")})}));
	Input::GamepadState pad = Pad();
	auto const pull = [&pad, &state](float amount)
	{
		SetAxis(pad, GamepadAxis::RightTrigger, amount * 2.0f - 1.0f);
		Input::SetGamepadState(0, pad);
		state.Evaluate();
	};
	pull(0.45f);
	CHECK(state.GetValue(0) == doctest::Approx(0.45f));
	CHECK_FALSE(state.IsDown(0));
	CHECK(state.GetEdges().empty());
	pull(0.5f);
	CHECK(state.IsDown(0));
	CHECK(Edges(state) == std::vector<InputActionEdge>{{0, true}});
	pull(0.45f);
	CHECK(state.IsDown(0));
	CHECK(state.GetEdges().empty());
	pull(0.41f);
	CHECK(state.IsDown(0));
	pull(0.39f);
	CHECK_FALSE(state.IsDown(0));
	CHECK(Edges(state) == std::vector<InputActionEdge>{{0, false}});
	pull(0.45f);
	CHECK_FALSE(state.IsDown(0));
	CHECK(state.GetEdges().empty());
}

TEST_CASE_FIXTURE(InputActionsFixture, "InputActions: edges follow the table: taps between evaluations, and a release and press again")
{
	InputBindingSpecification weak = Binding("Key/E");
	weak.Scale = 0.3f;
	InputActionState state;
	state.SetActions(MakeSet({
		Action("Jump", InputActionType::Button, {Binding("Key/Space"), Binding("Gamepad/A")}),
		Action("Steer", InputActionType::Axis, {Binding("Key/A Key/D")}),
		Action("Weak", InputActionType::Button, {weak}),
	}));
	state.Evaluate();
	CHECK(state.GetEdges().empty());

	SUBCASE("up to down, down to up")
	{
		Input::SetKeyState(KeyCode::Space, true);
		state.Evaluate();
		CHECK(Edges(state) == std::vector<InputActionEdge>{{0, true}});
		CHECK(state.IsPressed(0));
		CHECK_FALSE(state.IsReleased(0));
		state.Evaluate();
		CHECK(state.GetEdges().empty());
		CHECK_FALSE(state.IsPressed(0));
		Input::SetKeyState(KeyCode::Space, false);
		state.Evaluate();
		CHECK(Edges(state) == std::vector<InputActionEdge>{{0, false}});
		CHECK(state.IsReleased(0));
	}
	SUBCASE("a tap between two evaluations reports a press, then a release")
	{
		Input::SetKeyState(KeyCode::Space, true);
		Input::SetKeyState(KeyCode::Space, false);
		state.Evaluate();
		CHECK(Edges(state) == std::vector<InputActionEdge>{{0, true}, {0, false}});
		CHECK(state.IsPressed(0));
		CHECK(state.IsReleased(0));
		CHECK_FALSE(state.IsDown(0));
		// Through the end of an input frame between them: the counts outlive the frame's flags.
		Input::SetKeyState(KeyCode::Space, true);
		Input::EndFrame();
		Input::SetKeyState(KeyCode::Space, false);
		Input::EndFrame();
		state.Evaluate();
		CHECK(Edges(state) == std::vector<InputActionEdge>{{0, true}, {0, false}});
	}
	SUBCASE("a release and press again of the only control holding the action reports a release, then a press")
	{
		Input::SetKeyState(KeyCode::Space, true);
		state.Evaluate();
		Input::SetKeyState(KeyCode::Space, false);
		Input::SetKeyState(KeyCode::Space, true);
		state.Evaluate();
		CHECK(Edges(state) == std::vector<InputActionEdge>{{0, false}, {0, true}});
		CHECK(state.IsDown(0));
	}
	SUBCASE("while another binding holds the action, a release and press again reports nothing")
	{
		Input::GamepadState pad = Pad();
		SetButton(pad, GamepadButton::A, true);
		Input::SetGamepadState(0, pad);
		Input::SetKeyState(KeyCode::Space, true);
		state.Evaluate();
		CHECK(Edges(state) == std::vector<InputActionEdge>{{0, true}});
		Input::SetKeyState(KeyCode::Space, false);
		Input::SetKeyState(KeyCode::Space, true);
		state.Evaluate();
		CHECK(state.GetEdges().empty());
	}
	SUBCASE("a control that cannot press its action alone makes no tap, and a part cancelled by its other half makes none")
	{
		Input::SetKeyState(KeyCode::E, true);
		Input::SetKeyState(KeyCode::E, false);
		state.Evaluate();
		CHECK(state.GetEdges().empty());

		Input::SetKeyState(KeyCode::A, true);
		state.Evaluate();
		CHECK(Edges(state) == std::vector<InputActionEdge>{{1, true}});
		// D pressed while A is held: the axis returns to 0 and the action goes up, a release by its level.
		Input::SetKeyState(KeyCode::D, true);
		state.Evaluate();
		CHECK(Edges(state) == std::vector<InputActionEdge>{{1, false}});
		// D let go and pressed again while A is held: D alone can press the action, so its release and press is a tap (the
		// axis was -1 between, the action down).
		Input::SetKeyState(KeyCode::D, false);
		Input::SetKeyState(KeyCode::D, true);
		state.Evaluate();
		CHECK(Edges(state) == std::vector<InputActionEdge>{{1, true}, {1, false}});
		// With A let go, D alone presses it; then D let go and A pressed between two evaluations keep it down (no tap: no
		// control was both pressed and released), and D pressed again cancels A.
		Input::SetKeyState(KeyCode::A, false);
		state.Evaluate();
		CHECK(Edges(state) == std::vector<InputActionEdge>{{1, true}});
		Input::SetKeyState(KeyCode::D, false);
		Input::SetKeyState(KeyCode::A, true);
		state.Evaluate();
		CHECK(state.GetEdges().empty());
		Input::SetKeyState(KeyCode::D, true);
		state.Evaluate();
		CHECK(Edges(state) == std::vector<InputActionEdge>{{1, false}});
	}
	SUBCASE("edges are listed by action, each action's in the order they happened")
	{
		Input::SetKeyState(KeyCode::D, true);
		Input::SetKeyState(KeyCode::Space, true);
		Input::SetKeyState(KeyCode::Space, false);
		state.Evaluate();
		CHECK(Edges(state) == std::vector<InputActionEdge>{{0, true}, {0, false}, {1, true}});
	}
}

TEST_CASE_FIXTURE(InputActionsFixture, "InputActions: a new set makes no edge for what is held, and a rebuilt one keeps state by name")
{
	Input::SetKeyState(KeyCode::Space, true);
	InputActionState state;
	state.SetActions(MakeSet({Action("Jump", InputActionType::Button, {Binding("Key/Space")})}));
	CHECK(state.IsDown(0));
	state.Evaluate();
	CHECK(state.IsDown(0));
	CHECK(state.GetEdges().empty());

	// A rebuilt set (an edited asset): the held action stays down without an edge, a tap since the last evaluation is
	// kept, a new action takes its level without an edge, and the latch carries over.
	Input::SetKeyState(KeyCode::F, true);
	Input::SetKeyState(KeyCode::G, true);
	Input::SetKeyState(KeyCode::G, false);
	state.SetActions(MakeSet({Action("Fire", InputActionType::Button, {Binding("Key/F")}),
	                          Action("Jump", InputActionType::Button, {Binding("Key/G"), Binding("Key/Space")})}));
	CHECK(state.IsDown(0));
	CHECK(state.IsDown(1));
	state.Evaluate();
	// Fire was held as it arrived: no edge. G's tap came before the rebuild, on a control the old set did not read: not seen.
	CHECK(state.GetEdges().empty());
	Input::SetKeyState(KeyCode::Space, false);
	state.Evaluate();
	CHECK(Edges(state) == std::vector<InputActionEdge>{{1, false}});

	// A tap on a control both sets read, made before the rebuild, is reported at the next evaluation.
	Input::SetKeyState(KeyCode::Space, true);
	Input::SetKeyState(KeyCode::Space, false);
	state.SetActions(MakeSet({Action("Jump", InputActionType::Button, {Binding("Key/Space")})}));
	state.Evaluate();
	CHECK(Edges(state) == std::vector<InputActionEdge>{{0, true}, {0, false}});

	state.SetActions(nullptr);
	state.Evaluate();
	CHECK(state.GetEdges().empty());
	CHECK(state.GetValue(0) == 0.0f);
}

TEST_CASE_FIXTURE(InputActionsFixture, "InputActions: unknown names read zero and are logged once per name until the set changes")
{
	InputActionState state;
	uint64_t const first = Log::GetNextEntryIndex();
	CHECK_FALSE(state.Resolve("Jump").has_value());
	CHECK_FALSE(state.Resolve("Jump").has_value());
	CHECK(CountLogged(first, "no input actions are loaded") == 1);

	state.SetActions(MakeSet({Action("Jump", InputActionType::Button, {Binding("Key/Space")})}));
	CHECK(state.Resolve("Jump") == 0u);
	uint64_t const second = Log::GetNextEntryIndex();
	CHECK_FALSE(state.Resolve("jump").has_value());
	CHECK_FALSE(state.Resolve("jump").has_value());
	CHECK_FALSE(state.Resolve("Dash").has_value());
	CHECK(CountLogged(second, "no action is named 'jump'") == 1);
	CHECK(CountLogged(second, "no action is named 'Dash'") == 1);
	state.SetActions(MakeSet({Action("Jump", InputActionType::Button, {})}));
	CHECK_FALSE(state.Resolve("Dash").has_value());
	CHECK(CountLogged(second, "no action is named 'Dash'") == 2);
}

TEST_CASE_FIXTURE(InputActionsFixture, "InputActions: overrides are one more button of their action, with its taps and presses again")
{
	InputActionState state;
	state.SetActions(MakeSet({Action("Jump", InputActionType::Button, {Binding("Key/Space")}),
	                          Action("Move", InputActionType::Vector2, {Binding("Gamepad/LeftStick")})}));
	state.Evaluate();

	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(1.0f, 0.0f)));
	CHECK(state.GetEdges().empty());
	state.Evaluate();
	CHECK(Edges(state) == std::vector<InputActionEdge>{{0, true}});
	Input::ClearActionOverride("Jump");
	state.Evaluate();
	CHECK(Edges(state) == std::vector<InputActionEdge>{{0, false}});

	// Set and cleared between two evaluations: a tap, as keys given to a paused game.
	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(1.0f, 0.0f)));
	Input::ClearActionOverride("Jump");
	state.Evaluate();
	CHECK(Edges(state) == std::vector<InputActionEdge>{{0, true}, {0, false}});
	// Below the press threshold, it is no tap.
	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(0.3f, 0.0f)));
	Input::ClearActionOverride("Jump");
	state.Evaluate();
	CHECK(state.GetEdges().empty());
	// Set to 0 and back while held: a release and press again.
	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(1.0f, 0.0f)));
	state.Evaluate();
	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(0.0f)));
	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(1.0f, 0.0f)));
	state.Evaluate();
	CHECK(Edges(state) == std::vector<InputActionEdge>{{0, false}, {0, true}});
	// ...but not while a key holds the action too.
	Input::SetKeyState(KeyCode::Space, true);
	Input::ClearActionOverride("Jump");
	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(1.0f, 0.0f)));
	state.Evaluate();
	CHECK(state.GetEdges().empty());
	Input::SetKeyState(KeyCode::Space, false);

	// A vector override.
	REQUIRE(Input::SetActionOverride("Move", glm::vec2(0.0f, 1.0f)));
	state.Evaluate();
	CHECK(state.GetVector(1) == glm::vec2(0.0f, 1.0f));
	CHECK(Edges(state) == std::vector<InputActionEdge>{{1, true}});

	// The end of what automation put in place of devices: every override released, with its edges.
	Input::ReleaseInjectedInput();
	state.Evaluate();
	CHECK(Edges(state) == std::vector<InputActionEdge>{{0, false}, {1, false}});
	CHECK(Input::GetActionOverrides().empty());

	// An override naming no action of the set is logged once.
	uint64_t const first = Log::GetNextEntryIndex();
	REQUIRE(Input::SetActionOverride("Dash", glm::vec2(1.0f, 0.0f)));
	state.Evaluate();
	REQUIRE(Input::SetActionOverride("Dash", glm::vec2(0.5f, 0.0f)));
	state.Evaluate();
	CHECK(CountLogged(first, "the override of 'Dash' names no action") == 1);
	CHECK(state.GetEdges().empty());
}

TEST_CASE_FIXTURE(InputActionsFixture, "InputActions: each edge reaches exactly one fixed step, the first at or after its evaluation")
{
	InputActionState state;
	state.SetActions(MakeSet({Action("Jump", InputActionType::Button, {Binding("Key/Space")})}));
	state.Evaluate();
	CHECK(state.GetFixedStepNumber() == 0);

	// An evaluation that no step follows carries its edge to the next evaluation's first step.
	Input::SetKeyState(KeyCode::Space, true);
	state.Evaluate();
	CHECK(state.IsPressed(0));
	state.Evaluate();
	CHECK_FALSE(state.IsPressed(0));
	{
		InputActionState::FixedStepScope const step(state);
		CHECK(state.IsInFixedStep());
		CHECK(state.GetFixedStepNumber() == 1);
		// Inside a step, every query reads the step's edges, whatever view it asks for.
		CHECK(state.IsPressed(0));
		CHECK(state.IsPressed(0, InputActionView::Frame));
		CHECK(state.IsPressed(0, InputActionView::NoEdges));
		CHECK(state.IsPressed(0, InputActionView::FixedStep, 99));
	}
	{
		InputActionState::FixedStepScope const step(state);
		CHECK_FALSE(state.IsPressed(0));
	}
	CHECK_FALSE(state.IsInFixedStep());
	// Outside the steps: the frame's edges (none now), step 1's by its number, none in the NoEdges view.
	CHECK_FALSE(state.IsPressed(0, InputActionView::Frame));
	CHECK(state.IsPressed(0, InputActionView::FixedStep, 1));
	CHECK_FALSE(state.IsPressed(0, InputActionView::FixedStep, 2));
	CHECK_FALSE(state.IsPressed(0, InputActionView::FixedStep, 0));
	CHECK_FALSE(state.IsPressed(0, InputActionView::NoEdges));

	// A press and release within one evaluation reach one step as both edges, the action up.
	Input::SetKeyState(KeyCode::Space, false);
	state.Evaluate();
	Input::SetKeyState(KeyCode::Space, true);
	Input::SetKeyState(KeyCode::Space, false);
	state.Evaluate();
	{
		InputActionState::FixedStepScope const step(state);
		CHECK(state.IsPressed(0));
		CHECK(state.IsReleased(0));
		CHECK_FALSE(state.IsDown(0));
	}
	// Two presses before one step reach it as one.
	Input::SetKeyState(KeyCode::Space, true);
	state.Evaluate();
	Input::SetKeyState(KeyCode::Space, false);
	state.Evaluate();
	Input::SetKeyState(KeyCode::Space, true);
	state.Evaluate();
	uint64_t taken = 0;
	for (int index = 0; index < 3; index++)
	{
		InputActionState::FixedStepScope const step(state);
		if (state.IsPressed(0))
		{
			CHECK(taken == 0);
			taken = state.GetFixedStepNumber();
		}
	}
	CHECK(taken == 4);
	// A debugger resume of code that step 4 started reads exactly step 4's edges, though steps 5 and 6 came after it.
	CHECK(state.IsPressed(0, InputActionView::FixedStep, 4));
	CHECK(state.IsReleased(0, InputActionView::FixedStep, 4));
	CHECK_FALSE(state.IsPressed(0, InputActionView::FixedStep, 6));
}

TEST_CASE_FIXTURE(InputActionsFixture, "InputActions: Input::Reset while a state evaluates makes no edge of the counts it clears")
{
	InputActionState state;
	state.SetActions(MakeSet({Action("Jump", InputActionType::Button, {Binding("Key/Space"), Binding("Gamepad/A")}),
	                          Action("Fire", InputActionType::Button, {Binding("Key/F")})}));
	for (int tap = 0; tap < 3; tap++)
	{
		Input::SetKeyState(KeyCode::Space, true);
		Input::SetKeyState(KeyCode::Space, false);
		state.Evaluate();
	}
	REQUIRE(Input::SetActionOverride("Fire", glm::vec2(1.0f, 0.0f)));
	Input::SetKeyState(KeyCode::F, true);
	state.Evaluate();
	CHECK(state.IsDown(1));
	uint64_t const version = Input::GetActionOverridesVersion();

	Input::Reset();
	CHECK(Input::GetActionOverridesVersion() > version);
	state.Evaluate();
	// The held action falls as its controls did; nothing else reports a thing.
	CHECK(Edges(state) == std::vector<InputActionEdge>{{1, false}});
	state.Evaluate();
	CHECK(state.GetEdges().empty());
	// Counts read after the reset are the baseline: the next tap is one.
	Input::SetKeyState(KeyCode::Space, true);
	Input::SetKeyState(KeyCode::Space, false);
	state.Evaluate();
	CHECK(Edges(state) == std::vector<InputActionEdge>{{0, true}, {0, false}});
}
