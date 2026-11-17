#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Core/Result.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Input actions (Docs/Architecture.md §7.4): named values a game reads instead of keys, buttons and axes, each from the
// bindings of an InputActions asset (.sinput, Strada/Asset/InputActionsAsset.h). This header is the runtime: the controls
// a binding names, the compiled set of actions (InputActionSet, which the Asset module builds from the asset, response
// curves resolved), and the state one consumer evaluates once per frame (InputActionState: a running scene's). It reads
// only Input (Strada/Core/Input.h), so it sits in Core below the asset. Main thread only, except where a function says
// otherwise.
namespace Strada
{
	// What an action's value is. Every type has a magnitude (the absolute value, or a vector's length) that the press and
	// release thresholds turn into "down" with hysteresis, so every action has edges.
	enum class InputActionType : uint8_t
	{
		// The magnitude of its winning binding's reading, from 0 (released) up, with digital bindings at 1; read as a press.
		// An axis bound to a button presses it either way, and Invert changes nothing of it.
		Button = 0,
		// A signed value, typically -1 to 1 (steering, throttle).
		Axis,
		// A two-dimensional value, +Y up, length typically at most 1 (moving, looking).
		Vector2
	};

	// The device a control belongs to and how it reads. Not serialized: files name controls by text (ParseInputControl).
	enum class InputControlKind : uint8_t
	{
		// A keyboard key (Code: a KeyCode): 1 while down.
		Key = 0,
		// A mouse button (Code: a MouseButton): 1 while down.
		Mouse,
		// A gamepad button (Code: a GamepadButton): 1 while down.
		PadButton,
		// A gamepad axis (Code: a GamepadAxis). Sticks read -1 to 1 with up positive (the reverse of
		// Input::GetGamepadAxis, which keeps GLFW's sign); triggers read 0 released to 1 pressed (GLFW gives -1 to 1), and 0
		// while they have read nothing but exactly 0 since their pad connected (Input::HasGamepadAxisMoved: a mapping that
		// lacks the trigger).
		PadAxis,
		// A gamepad stick as one two-dimensional control (Code: 0 the left stick, 1 the right), +Y up.
		PadStick
	};

	// One control of a device, as a binding names it.
	struct InputControl
	{
		InputControlKind Kind = InputControlKind::Key;
		// The KeyCode, MouseButton, GamepadButton or GamepadAxis value, or the stick (0 left, 1 right).
		uint16_t Code = 0;

		bool operator==(InputControl const& other) const = default;
	};

	// A binding names one control, or a composite of two (an axis: negative, positive) or four (a vector: up, down, left,
	// right).
	inline constexpr size_t MaxInputControlsPerBinding = 4;
	// Limits of an InputActions asset, refused beyond by its validation.
	inline constexpr size_t MaxInputActions = 256;
	inline constexpr size_t MaxInputBindingsPerAction = 16;
	inline constexpr size_t MaxInputActionNameLength = 64;
	// A binding's Gamepad: any connected gamepad (the largest reading among them), or one by index 0 to MaxGamepads - 1.
	inline constexpr int32_t AnyGamepad = -1;

	// The controls of one binding in the order the binding's text names them.
	struct InputControlList
	{
		std::array<InputControl, MaxInputControlsPerBinding> Controls = {};
		// 0 (an unbound binding, which reads 0), 1, 2 or 4.
		uint8_t Count = 0;

		std::span<InputControl const> Get() const { return std::span<InputControl const>(Controls.data(), Count); }
		bool operator==(InputControlList const& other) const = default;
	};

	// The control a name gives: "Key/<KeyCode name>" ("Key/Space", "Key/A"), "Mouse/<MouseButton name>" ("Mouse/Left"),
	// "Gamepad/<GamepadButton name>" ("Gamepad/A", "Gamepad/DPadUp"), "Gamepad/<GamepadAxis name>" ("Gamepad/LeftX",
	// "Gamepad/RightTrigger"), "Gamepad/LeftStick" or "Gamepad/RightStick". Letter case is ignored. Nothing for any other
	// text. Any thread.
	[[nodiscard]] std::optional<InputControl> ParseInputControl(std::string_view name);
	// The name ParseInputControl reads back as the control, in the letter case of the enumerators. Any thread.
	[[nodiscard]] std::string FormatInputControl(InputControl control);
	// A binding's text: empty (unbound), or one, two or four control names separated by spaces. Refuses another count and
	// a name ParseInputControl does not read; it does not check the controls against an action type
	// (CheckInputBindingControls does). Any thread.
	[[nodiscard]] Result<InputControlList> ParseInputControls(std::string_view text);
	// A binding's canonical text: each control's name as FormatInputControl writes it, separated by single spaces (empty for
	// an unbound list). ParseInputControls reads it back as the same list. Any thread.
	[[nodiscard]] std::string FormatInputControls(InputControlList const& controls);
	// Whether the controls fit an action of the type: Button and Axis take one control that is not a stick, or two that
	// are not (an axis composite); Vector2 takes one stick, or four controls that are not sticks (a vector composite). An
	// unbound list fits every type. Any thread.
	[[nodiscard]] Result<void> CheckInputBindingControls(InputActionType type, InputControlList const& controls);

	// A response curve: maps a binding's magnitude after its dead zone (0 to 1) to the magnitude it gives. Empty is the
	// identity. The Asset module makes one from a Curve asset; it must be callable from the main thread for the set's
	// lifetime.
	using InputResponse = std::function<float(float)>;

	// A binding as the runtime evaluates it (what InputActionsAsset's compilation makes of an InputActionBinding).
	struct InputBindingSpecification
	{
		InputControlList Controls;
		// The gamepad its gamepad controls read: AnyGamepad, or 0 to MaxGamepads - 1.
		int32_t Gamepad = AnyGamepad;
		// Magnitudes up to it read 0, and the rest is stretched to reach 1 again: 0 to below 1.
		float DeadZone = 0.0f;
		InputResponse Response;
		// Negates the value (a two-dimensional value's X).
		bool Invert = false;
		// Negates a two-dimensional value's Y; one-dimensional bindings ignore it.
		bool InvertY = false;
		// Multiplies the value, last: at least 0.
		float Scale = 1.0f;
	};

	// An action as the runtime evaluates it.
	struct InputActionSpecification
	{
		std::string Name;
		InputActionType Type = InputActionType::Button;
		// The magnitude at which the action goes down, and the one below which it goes up again: 0 < Release <= Press <= 1.
		float PressThreshold = 0.5f;
		float ReleaseThreshold = 0.4f;
		std::vector<InputBindingSpecification> Bindings;
	};

	// The steps of a binding's processing, in their order, exposed so that tests check each against its closed form.
	// ReadControls (and so ProcessBinding's caller) reads Input on the main thread; the other steps are pure, any thread.
	namespace InputProcessing
	{
		// The reading of one control alone: Kind's reading, 0 for a gamepad that is not connected; with AnyGamepad, the
		// reading of the largest magnitude among the connected gamepads (the first on a tie). A stick's vector is limited to
		// length 1. One-dimensional values are in x, with y 0.
		[[nodiscard]] glm::vec2 ReadControl(InputControl control, int32_t gamepad);
		// The value a binding's controls read before processing: a single control's reading (ReadControl); an axis
		// composite's positive part minus its negative part; a vector composite's (right - left, up - down) with its length
		// limited to 1. A composite's part reads max(x, 0) of its reading. One-dimensional values are in x, with y 0.
		[[nodiscard]] glm::vec2 ReadControls(InputControlList const& controls, int32_t gamepad);
		// What a binding's controls read when the given readings are theirs (ReadControls' arithmetic without the devices):
		// readings[i] is the reading of controls[i]. Pure.
		[[nodiscard]] glm::vec2 CombineControls(InputControlList const& controls, std::span<glm::vec2 const> readings);
		// Radial dead zone: magnitude m <= deadZone gives 0, otherwise the direction times (m - deadZone) / (1 - deadZone).
		[[nodiscard]] glm::vec2 ApplyDeadZone(glm::vec2 value, float deadZone);
		// The response applied to the magnitude, keeping the direction (or the sign).
		[[nodiscard]] glm::vec2 ApplyResponse(glm::vec2 value, InputResponse const& response);
		// Dead zone, then response, then inversion, then scale.
		[[nodiscard]] glm::vec2 ProcessBinding(InputBindingSpecification const& binding, glm::vec2 raw);
	}

	// The actions of one map, compiled and immutable: shared by the states that evaluate it. Main thread.
	class InputActionSet final
	{
		struct PrivateTag
		{
		};

	public:
		// Refuses what an InputActions asset's validation refuses (names empty, too long or repeated in any letter case,
		// thresholds out of order, bindings whose controls do not fit their action, limits). The specifications are taken
		// as they are; responses are not called.
		[[nodiscard]] static Result<Ref<InputActionSet const>> Create(std::vector<InputActionSpecification> actions);

		std::span<InputActionSpecification const> GetActions() const { return m_Actions; }
		// The index of the action of exactly that name (names are matched with their letter case). Allocates nothing.
		[[nodiscard]] std::optional<uint32_t> Find(std::string_view name) const;
		// A number no other set of this process has: states tell a rebuilt set from the one they evaluated.
		uint64_t GetVersion() const { return m_Version; }

		// For Create alone (the tag is private).
		explicit InputActionSet(PrivateTag) {}

	private:
		std::vector<InputActionSpecification> m_Actions;
		// The action indices in ordinal order of their names, for Find.
		std::vector<uint32_t> m_SortedIndices;
		uint64_t m_Version = 0;
	};

	// One edge of the latest evaluation, in the order the events of the frame dispatch them: by action index, and for a
	// press and release within one evaluation the press first.
	struct InputActionEdge
	{
		uint32_t Action = 0;
		// A press (the action went down), otherwise a release.
		bool Pressed = true;

		bool operator==(InputActionEdge const& other) const = default;
	};

	// Which edges a query made outside every fixed step reads (Docs/Architecture.md §7.4, "The fixed-step rule"). Inside a
	// step every query reads that step's edges whatever it passes: the state knows (InputActionState::IsInFixedStep). The
	// caller chooses from where the code that asks came from, recorded when that code started.
	enum class InputActionView : uint8_t
	{
		// The latest evaluation's edges: OnUpdate, events, clicks, contacts, OnDestroy, and whatever a frame started.
		Frame = 0,
		// The edges of one fixed step, by its number (InputActionState::GetFixedStepNumber when the code started): code a
		// step started that a debugger resume runs between frames.
		FixedStep,
		// None: code a step started that runs later outside a step for another reason (a latent node's continuation, a
		// WaitUntil recheck, a Timeline's tick), where either the frame's or a step's edges would show an edge twice.
		NoEdges
	};

	// The actions of one consumer, evaluated once per frame from Input and its action overrides: a running scene owns one
	// (Scene::UpdateInputActions evaluates it, scripts' queries read it). Between two evaluations it does not change
	// except at fixed steps, whose edges it latches (Docs/Architecture.md §7.4, "The fixed-step rule"). Main thread.
	class InputActionState final
	{
	public:
		// Evaluates this set from the next Evaluate on. Actions of the same name in the old set keep their state (their
		// value, whether they are down, the edges latched for the next fixed step and those steps took), so a set rebuilt
		// while a game runs (an edited asset, a rebind) makes no edge. A kept action whose bindings read other controls now
		// goes down without a press when its current reading holds it down (the control pressed to bind it does not press
		// it), and one that was down stays down until an evaluation finds nothing holding it (its release is reported
		// then). New actions start at their current reading and level, without
		// an edge (a control held as the set arrives holds the action down, as a key held when play starts is down but not
		// pressed), with the controls' and the override's current press and release counts as their baseline. Calls each
		// binding's response once per digital control (which controls can press their action). Forgets the names logged
		// as unknown. Null evaluates nothing, and every query reads zero.
		void SetActions(Ref<InputActionSet const> actions);
		Ref<InputActionSet const> const& GetActions() const { return m_Actions; }
		// Drops the set and every action's state, and the unknown names already logged.
		void Clear();
		// Makes what the action's digital controls did since the last evaluation no edge (their counts become the baseline)
		// and, when its reading now holds it down, puts it down without a press: what a capture calls once it bound the
		// control a player pressed, also when the binding already read that control (no new set comes then). An action that
		// is down stays down. Its override's counts are left alone. Nothing for an index the set lacks.
		void Settle(uint32_t action);

		// The frame's evaluation (Docs/Architecture.md §7.4): every action's value is the reading of the binding with the
		// largest magnitude, then of its override (Input::FindActionOverride) under the same rule (the first in that order
		// wins a tie; a Button's value is the magnitude); its level follows the thresholds with hysteresis; its edges come
		// from the level and from the press and release counts of its digital controls and its override since the last
		// evaluation (the table of "Edges"); each edge is latched for the next fixed step. Replaces the previous evaluation's
		// edges (GetEdges). Counts are taken again as baselines, without an edge, when Input::Reset ran since. An override
		// naming no action of the set is logged once per name, looked for only when the overrides or the set changed.
		// Allocates nothing but those log lines. Profiler zone Input.EvaluateActions.
		void Evaluate();
		// The edges of the latest evaluation, in dispatch order: by action index, each action's in the order they happened
		// (a tap's press first, a release and press again's release first).
		std::span<InputActionEdge const> GetEdges() const { return m_Edges; }

		// Brackets one fixed step (the scene's FixedUpdateScripts, whether or not it runs scripts): Begin gives the step the
		// next step number and makes it take every action's latch (the edges since the previous step began), which empties
		// it. Until End, every query reads that step's edges. Nested calls are a programmer error.
		void BeginFixedStep();
		void EndFixedStep();
		bool IsInFixedStep() const { return m_InFixedStep; }
		// The number of the step running, or of the latest one: 1 for the state's first step, 0 before any. What code a step
		// starts records, so that a debugger resume reads that step's edges (InputActionView::FixedStep).
		uint64_t GetFixedStepNumber() const { return m_StepNumber; }

		// RAII for BeginFixedStep and EndFixedStep.
		class FixedStepScope final
		{
		public:
			explicit FixedStepScope(InputActionState& state);
			~FixedStepScope();
			FixedStepScope(FixedStepScope const&) = delete;
			FixedStepScope& operator=(FixedStepScope const&) = delete;

		private:
			InputActionState& m_State;
		};

		// Queries by index into the set (an index the set lacks reads zero). The value and level are the latest
		// evaluation's in every view. GetValue is a Button's or Axis's value and a Vector2's length; GetVector is a Vector2's
		// vector and (value, 0) for the others.
		[[nodiscard]] float GetValue(uint32_t action) const;
		[[nodiscard]] glm::vec2 GetVector(uint32_t action) const;
		[[nodiscard]] bool IsDown(uint32_t action) const;
		// The edges: inside a fixed step, the running step's whatever view and step are given; outside, the latest
		// evaluation's (Frame), those step `step` took (FixedStep: what its number was when the asking code started), or
		// none (NoEdges).
		[[nodiscard]] bool IsPressed(uint32_t action, InputActionView view = InputActionView::Frame, uint64_t step = 0) const;
		[[nodiscard]] bool IsReleased(uint32_t action, InputActionView view = InputActionView::Frame, uint64_t step = 0) const;

		// The index of the named action. A name the set lacks (or every name, without a set) is logged once per name until
		// the set changes or Clear, and reads as zero through the queries: callers pass the result on as it is. Allocates
		// only to remember a name it logs.
		[[nodiscard]] std::optional<uint32_t> Resolve(std::string_view name);

	private:
		// A digital control of an action's binding (a key, a mouse button, a gamepad button) with its press and release
		// counts as the last evaluation read them. A gamepad button of a binding that reads any gamepad is one control per
		// slot, so that one pad's press or tap is judged apart from another pad holding the same button.
		struct DigitalControl
		{
			uint32_t Binding = 0;
			uint32_t Part = 0;
			// The gamepad slot of a gamepad button (the binding's own, or this control's for a binding of any gamepad);
			// AnyGamepad for a key or a mouse button.
			int32_t Gamepad = AnyGamepad;
			// Whether the binding, read with this control down and its other controls up, reaches the press threshold.
			bool CanPress = false;
			uint32_t Presses = 0;
			uint32_t Releases = 0;
		};

		struct ActionState
		{
			// The latest evaluation's value (Button and Axis in x, y 0) and its magnitude.
			glm::vec2 Value = glm::vec2(0.0f);
			float Magnitude = 0.0f;
			bool Down = false;
			// The latest evaluation's edges, and which came first when it has both.
			bool Pressed = false;
			bool Released = false;
			bool ReleasedFirst = false;
			// Edges since the last fixed step began, for the next one.
			uint32_t PendingPresses = 0;
			uint32_t PendingReleases = 0;
			// The number of the latest step that took a press, and a release, of the action (0: none).
			uint64_t StepPressed = 0;
			uint64_t StepReleased = 0;
			// In binding and control order.
			std::vector<DigitalControl> Controls;
			// The action's override's counts as the last evaluation read them.
			uint32_t OverridePresses = 0;
			uint32_t OverrideReleases = 0;
		};

		// Takes every count as the action's baseline (no edge).
		void TakeBaselines(uint32_t action);
		void LogUnknownOverrides();
		bool HasStepEdge(uint64_t stepOfEdge, InputActionView view, uint64_t step) const;

		Ref<InputActionSet const> m_Actions;
		std::vector<ActionState> m_States;
		std::vector<InputActionEdge> m_Edges;
		std::set<std::string, std::less<>> m_LoggedNames;
		std::set<std::string, std::less<>> m_LoggedOverrides;
		bool m_InFixedStep = false;
		uint64_t m_StepNumber = 0;
		// Input's reset generation and overrides' version as the last evaluation read them.
		uint64_t m_ResetGeneration = 0;
		uint64_t m_OverridesVersion = 0;
		bool m_CheckOverrides = true;
	};
}
