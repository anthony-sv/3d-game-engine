#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/KeyCodes.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Rebinding while a game runs (Docs/Architecture.md §7.4, "Rebinding"): what a rebind answers, and the capture that listens
// for the control a player presses. The binding table and its profile are the Asset module's (Strada/Asset/InputRebinding.h);
// the capture runs in the scene that asked for it (Scene::StartInputRebind). Main thread.
namespace Strada
{
	// How a rebind ended, or that one is listening.
	enum class InputRebindStatus : uint8_t
	{
		// Nothing was asked yet.
		None = 0,
		// A capture listens for the next control (Scene::StartInputRebind).
		Listening,
		// The binding reads the new controls from the next evaluation on.
		Done,
		// A capture ended without a control: Escape was pressed, its time ran out, another capture or a cancel replaced it,
		// or its scene stopped.
		Cancelled,
		// The request names no binding that can take the controls (an unknown action, a binding or part out of range,
		// controls that do not fit the action), or no actions run; Message says which. Nothing changed.
		Refused
	};

	// The answer of a rebind, a capture or a reset.
	struct InputRebindResult
	{
		InputRebindStatus Status = InputRebindStatus::None;
		// The action and binding asked for (the binding's index in its action, -1 for a reset), and the part: -1 for the whole
		// binding, else the index of one control of a composite.
		std::string Action;
		int32_t Binding = -1;
		int32_t Part = -1;
		// The binding's controls (its text, as a binding names them): the new ones when Done, the current ones while
		// Listening and when Cancelled; empty when Refused and for a reset.
		std::string Controls;
		// When Done: the other actions with a binding that reads a control the new binding reads (on a gamepad both can
		// read), in the order of the actions in the map, each once. The engine applies the binding and reports these; it
		// does not unbind them.
		std::vector<std::string> Conflicts;
		// Why a rebind was refused or cancelled, or that a binding that was done could not be saved; empty otherwise.
		std::string Message;
	};

	// What a capture may take, by what the binding needs.
	enum class InputCaptureTarget : uint8_t
	{
		// A whole Button or Axis binding: a key, a mouse button, a gamepad button, or a stick axis or trigger moved past
		// InputControlCapture::Threshold either way (taken as that axis).
		Control = 0,
		// A whole Vector2 binding: a stick moved past the threshold in any direction (taken as the stick).
		Stick,
		// One part of a composite: a key, a mouse button, a gamepad button, or a gamepad axis moved past the threshold in its
		// positive direction (a trigger pressed, a stick right or up), since a part reads the positive half of its control.
		Part
	};

	// What an update of a capture found.
	enum class InputCaptureOutcome : uint8_t
	{
		// Nothing yet, or the capture is not listening.
		Waiting = 0,
		// A control: GetControl names it. The capture stopped.
		Captured,
		// Escape was pressed. The capture stopped.
		Escaped,
		// Timeout seconds passed without a control. The capture stopped.
		TimedOut
	};

	// Listens for the next control a player gives, from Input alone, for one binding. A control counts only when it is given
	// after Start: a key or button pressed since then (its press count moved, so one held at Start counts once released and
	// pressed again), an axis that reaches the threshold after reading below it (at Start or at an update since). Axes read as
	// actions read them (InputProcessing::ReadControl: up positive, triggers 0 released to 1 pressed, and 0 for a trigger a
	// pad's mapping lacks), so a trigger at rest gives nothing. Main thread.
	class InputControlCapture final
	{
	public:
		// The reading an axis or a stick must reach, after reading below it.
		static constexpr float Threshold = 0.5f;
		// Seconds of updates (the frame's unscaled time) after which a capture stops without a control.
		static constexpr float Timeout = 10.0f;

		// Starts listening for a control that fits the target, on the gamepads a binding of `gamepad` reads (AnyGamepad:
		// every slot; a slot: that one; keys and mouse buttons whatever it is). Takes every count and axis as its baseline.
		void Start(InputCaptureTarget target, int32_t gamepad);
		// Stops listening (nothing is captured).
		void Stop() { m_Listening = false; }
		bool IsListening() const { return m_Listening; }

		// Advances the capture by `seconds` (at least 0; the frame's unscaled time) and looks at what came since the last
		// update, in this order: Escape pressed (Escaped); keys by code, mouse buttons, gamepad buttons by slot then button,
		// gamepad axes (or sticks) by slot then axis (Captured, the first that fits); then the time (TimedOut). Waiting while
		// none of them happened, and whenever the capture is not listening.
		InputCaptureOutcome Update(float seconds);
		// The control of the last Captured outcome.
		InputControl GetControl() const { return m_Control; }

	private:
		// Takes every key's, mouse button's and gamepad button's press count as the baseline presses are counted from.
		void TakeBaselines();
		bool ReadsGamepad(uint32_t slot) const;
		// The reading of an axis (or a stick, by its index) of a slot that the target compares with the threshold.
		float ReadAxis(uint32_t slot, uint32_t axis) const;
		uint32_t GetAxisCount() const;

		InputCaptureTarget m_Target = InputCaptureTarget::Control;
		int32_t m_Gamepad = AnyGamepad;
		bool m_Listening = false;
		float m_Elapsed = 0.0f;
		InputControl m_Control;
		// Input's reset generation the baselines were taken in (Input::Reset clears every count).
		uint64_t m_ResetGeneration = 0;
		std::array<uint32_t, KeyCodeCount> m_KeyPresses = {};
		std::array<uint32_t, MouseButtonCount> m_MousePresses = {};
		std::array<std::array<uint32_t, GamepadButtonCount>, MaxGamepads> m_PadPresses = {};
		// Whether each axis (or stick) of each slot read below the threshold since it last reached it.
		std::array<std::array<bool, GamepadAxisCount>, MaxGamepads> m_Armed = {};
	};
}
