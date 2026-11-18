#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/InputCapture.h"
#include "Strada/Core/Result.h"
#include "Strada/ImGui/ImGuiLayer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The Input Actions panel's capture of a binding (Docs/Editor.md, "Input Actions"): the editor listens for the next control the
// user gives and the panel writes it into the asset. Keys and mouse buttons come from the editor's windows, as the game
// input router sees them before ImGui does (GameInputRouter::SetCaptureFilter): the physical key, as games read keys, and a
// press the capture takes reaches neither the editor's interface nor a playing game. Gamepads, and keys and buttons that
// automation injects into Input, come from Input through the engine's InputControlCapture. It makes no ImGui call (only the layer's
// InputContext type comes from the ImGui layer's header), so tests drive it without a context; main thread.
namespace Strada
{
	class Event;

	// The binding a capture writes.
	struct InputBindingCaptureTarget
	{
		// The InputActions asset, the action (its name with its letter case, as the asset holds it) and the binding's index.
		AssetHandle Asset;
		std::string Action;
		uint32_t Binding = 0;
		// -1: the whole binding; 0 to 3: one control of a composite.
		int32_t Part = -1;
		// The binding's controls when the capture started: a binding that reads others since (moved, edited elsewhere) is
		// no longer the one asked for.
		std::string Controls;
		// What fits: a control, a stick (a whole Vector2 binding) or one part (InputCaptureTarget).
		InputCaptureTarget Kind = InputCaptureTarget::Control;
		// The gamepad the binding reads (AnyGamepad or a slot): the capture listens to those gamepads only.
		int32_t Gamepad = AnyGamepad;
	};

	// How a capture ended, as Update reports it once.
	enum class InputBindingCaptureOutcome : uint8_t
	{
		// Listening, or not capturing.
		Waiting = 0,
		// A control fits: GetControl names it.
		Captured,
		// Escape, a click outside the prompt, or Cancel.
		Cancelled,
		// InputControlCapture::Timeout seconds of updates passed without a control.
		TimedOut
	};

	// Listens for one control. A key pressed in an editor window (TakeEvent) is the key it names physically; Escape cancels.
	// A mouse button pressed over the prompt (the area SetPrompt gives) is that button, and one pressed anywhere else cancels
	// and goes on to the editor as usual. A Vector2 binding taken whole fits only a stick: keys and mouse buttons are taken
	// (so they do nothing else) but bind nothing, and GetNote says why. Gamepads are read through InputControlCapture with the
	// binding's gamepad.
	class InputBindingCapture final
	{
	public:
		// Starts listening for the target, ending a capture in progress without a control.
		void Start(InputBindingCaptureTarget target);
		// Stops listening without a control; Update reports Cancelled once when it was listening.
		void Cancel();
		bool IsListening() const { return m_Listening; }
		// Listening, or the character a captured key types is still to come (TakeEvent takes it, until the next Update).
		bool IsTakingInput() const { return m_Listening || m_TakesTyped; }
		InputBindingCaptureTarget const& GetTarget() const { return m_Target; }

		// Where a mouse press binds a mouse button: a rectangle in ImGui's coordinates, in the ImGui viewport given
		// (ImGuiLayer::InputContext). The panel gives it every frame it draws the prompt.
		void SetPrompt(glm::vec2 topLeft, glm::vec2 bottomRight, uint32_t viewport);

		// A window's input event before ImGui and the game see it. While listening, takes every key press (repeats included),
		// typed character and mouse button press over the prompt, and returns true; a mouse press elsewhere cancels and
		// returns false. After a key ended the capture, also takes the characters that key types until the next Update.
		// Returns false for everything else.
		bool TakeEvent(Event const& event, ImGuiLayer::InputContext const& context);
		// Advances the capture by the frame's seconds (unscaled, at least 0): reports what TakeEvent decided since the last
		// update, else looks at Input (Escape, keys and gamepads given since Start; mouse buttons are the windows' alone), else
		// counts the time. The outcome is reported once; Waiting afterwards.
		InputBindingCaptureOutcome Update(float seconds);
		// The control of the last Captured outcome.
		InputControl GetControl() const { return m_Control; }
		// Why the last input bound nothing while listening (a key for a stick); empty otherwise.
		std::string const& GetNote() const { return m_Note; }
		// Whether the capture last ended through a mouse press outside the prompt (that click goes on to what it is over).
		bool WasClickedAway() const { return m_ClickedAway; }

	private:
		void Finish(InputBindingCaptureOutcome outcome);

		InputBindingCaptureTarget m_Target;
		bool m_Listening = false;
		InputControlCapture m_Devices;
		// What TakeEvent decided, for the next Update.
		InputBindingCaptureOutcome m_Pending = InputBindingCaptureOutcome::Waiting;
		InputControl m_Control;
		std::string m_Note;
		bool m_ClickedAway = false;
		bool m_TakesTyped = false;
		glm::vec2 m_PromptMin = glm::vec2(0.0f);
		glm::vec2 m_PromptMax = glm::vec2(0.0f);
		uint32_t m_PromptViewport = 0;
		bool m_HasPrompt = false;
	};

	// The text of a binding's controls with one control in place of what the target binds: the whole text for a whole
	// binding, else that part of the composite the controls hold. Refuses a part the controls do not have, and a stick as a
	// part. The result is not checked against the action's type (the asset's validation does that). Any thread.
	[[nodiscard]] Result<std::string> ReplaceInputBindingControl(std::string_view controls, int32_t part, InputControl control);
	// What a capture of a binding of the type listens for: a part of a composite, else a stick for Vector2 and a control for
	// the other types. Any thread.
	[[nodiscard]] InputCaptureTarget GetInputBindingCaptureKind(InputActionType type, int32_t part);
}
