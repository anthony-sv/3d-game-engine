#include "stpch.h"
#include "Strada/Core/InputActions.h"

#include "Strada/Core/Input.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Profiler.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Strada
{
	namespace
	{
		constexpr std::string_view KeyDevice = "Key";
		constexpr std::string_view MouseDevice = "Mouse";
		constexpr std::string_view GamepadDevice = "Gamepad";
		constexpr std::array<std::string_view, 2> StickNames = {"LeftStick", "RightStick"};

		uint64_t s_NextSetVersion = 1;

		bool EqualsIgnoreCase(std::string_view a, std::string_view b)
		{
			return a.size() == b.size() &&
			       std::equal(a.begin(), a.end(), b.begin(),
			                  [](char left, char right)
			                  {
								  return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
							  });
		}

		std::string ToLowerAscii(std::string_view text)
		{
			std::string lower(text);
			for (char& character : lower)
			{
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			}
			return lower;
		}

		bool IsDigital(InputControlKind kind)
		{
			return kind == InputControlKind::Key || kind == InputControlKind::Mouse || kind == InputControlKind::PadButton;
		}

		float GetMagnitude(InputActionType type, glm::vec2 value)
		{
			return type == InputActionType::Vector2 ? glm::length(value) : std::abs(value.x);
		}

		glm::vec2 LimitLength(glm::vec2 value)
		{
			float const length = glm::length(value);
			return length > 1.0f ? value / length : value;
		}

		// One control of one gamepad slot.
		glm::vec2 ReadPadControl(InputControl control, uint32_t gamepad)
		{
			if (!Input::IsGamepadConnected(gamepad))
			{
				return glm::vec2(0.0f);
			}
			if (control.Kind == InputControlKind::PadButton)
			{
				return glm::vec2(Input::IsGamepadButtonDown(gamepad, static_cast<GamepadButton>(control.Code)) ? 1.0f : 0.0f, 0.0f);
			}
			if (control.Kind == InputControlKind::PadStick)
			{
				GamepadAxis const xAxis = control.Code == 0 ? GamepadAxis::LeftX : GamepadAxis::RightX;
				GamepadAxis const yAxis = control.Code == 0 ? GamepadAxis::LeftY : GamepadAxis::RightY;
				glm::vec2 const stick(std::clamp(Input::GetGamepadAxis(gamepad, xAxis), -1.0f, 1.0f),
				                      -std::clamp(Input::GetGamepadAxis(gamepad, yAxis), -1.0f, 1.0f));
				return LimitLength(stick);
			}
			GamepadAxis const axis = static_cast<GamepadAxis>(control.Code);
			float const raw = std::clamp(Input::GetGamepadAxis(gamepad, axis), -1.0f, 1.0f);
			switch (axis)
			{
				case GamepadAxis::LeftX:
				case GamepadAxis::RightX:
					return glm::vec2(raw, 0.0f);
				case GamepadAxis::LeftY:
				case GamepadAxis::RightY:
					return glm::vec2(-raw, 0.0f);
				case GamepadAxis::LeftTrigger:
				case GamepadAxis::RightTrigger:
					return glm::vec2(Input::HasGamepadAxisMoved(gamepad, axis) ? (raw + 1.0f) * 0.5f : 0.0f, 0.0f);
			}
			return glm::vec2(0.0f);
		}

		bool IsValidSlot(int32_t gamepad)
		{
			return gamepad >= 0 && gamepad < static_cast<int32_t>(MaxGamepads);
		}

		// The counts of one digital control: a key's, a mouse button's, or a gamepad button's on one slot (DigitalControl's
		// Gamepad).
		Input::ButtonCounts ReadCounts(InputControl control, int32_t gamepad)
		{
			switch (control.Kind)
			{
				case InputControlKind::Key:
					return Input::GetKeyCounts(static_cast<KeyCode>(control.Code));
				case InputControlKind::Mouse:
					return Input::GetMouseButtonCounts(static_cast<MouseButton>(control.Code));
				case InputControlKind::PadButton:
					return IsValidSlot(gamepad)
					           ? Input::GetGamepadButtonCounts(static_cast<uint32_t>(gamepad), static_cast<GamepadButton>(control.Code))
					           : Input::ButtonCounts{};
				case InputControlKind::PadAxis:
				case InputControlKind::PadStick:
					break;
			}
			return {};
		}

		// Whether one digital control is down now (a gamepad button on one slot).
		bool IsDigitalDown(InputControl control, int32_t gamepad)
		{
			switch (control.Kind)
			{
				case InputControlKind::Key:
					return Input::IsKeyDown(static_cast<KeyCode>(control.Code));
				case InputControlKind::Mouse:
					return Input::IsMouseButtonDown(static_cast<MouseButton>(control.Code));
				case InputControlKind::PadButton:
					return IsValidSlot(gamepad) &&
					       Input::IsGamepadButtonDown(static_cast<uint32_t>(gamepad), static_cast<GamepadButton>(control.Code));
				case InputControlKind::PadAxis:
				case InputControlKind::PadStick:
					break;
			}
			return false;
		}

		// A binding's part read as if the digital control of that slot were up: what the binding's other gamepads give it
		// when it reads any gamepad, otherwise 0.
		glm::vec2 ReadPartWithout(InputBindingSpecification const& binding, InputControl control, int32_t gamepad)
		{
			if (binding.Gamepad != AnyGamepad || control.Kind != InputControlKind::PadButton)
			{
				return glm::vec2(0.0f);
			}
			glm::vec2 best(0.0f);
			float bestMagnitude = 0.0f;
			for (uint32_t slot = 0; slot < MaxGamepads; slot++)
			{
				if (static_cast<int32_t>(slot) == gamepad)
				{
					continue;
				}
				glm::vec2 const reading = ReadPadControl(control, slot);
				float const magnitude = glm::length(reading);
				if (magnitude > bestMagnitude)
				{
					best = reading;
					bestMagnitude = magnitude;
				}
			}
			return best;
		}

		// The binding's controls read now, one reading per control, except the part given (when below the binding's count),
		// which reads `partReading`.
		glm::vec2 ReadBinding(InputBindingSpecification const& binding, uint32_t part = MaxInputControlsPerBinding,
		                      glm::vec2 partReading = glm::vec2(0.0f))
		{
			std::array<glm::vec2, MaxInputControlsPerBinding> readings = {};
			std::span<InputControl const> const controls = binding.Controls.Get();
			for (size_t index = 0; index < controls.size(); index++)
			{
				readings[index] = index == part ? partReading : InputProcessing::ReadControl(controls[index], binding.Gamepad);
			}
			return InputProcessing::CombineControls(binding.Controls, std::span<glm::vec2 const>(readings.data(), controls.size()));
		}

		std::string DescribeBinding(InputActionSpecification const& action, size_t binding)
		{
			return fmt::format("action '{}', binding {}", action.Name, binding);
		}
	}

	std::optional<InputControl> ParseInputControl(std::string_view name)
	{
		size_t const slash = name.find('/');
		if (slash == std::string_view::npos || name.find('/', slash + 1) != std::string_view::npos)
		{
			return std::nullopt;
		}
		std::string_view const device = name.substr(0, slash);
		std::string_view const control = name.substr(slash + 1);
		if (EqualsIgnoreCase(device, KeyDevice))
		{
			std::optional<KeyCode> const key = KeyCodeFromString(control);
			if (!key || *key == KeyCode{})
			{
				return std::nullopt;
			}
			return InputControl{InputControlKind::Key, static_cast<uint16_t>(*key)};
		}
		if (EqualsIgnoreCase(device, MouseDevice))
		{
			std::optional<MouseButton> const button = MouseButtonFromString(control);
			return button ? std::optional(InputControl{InputControlKind::Mouse, static_cast<uint16_t>(*button)}) : std::nullopt;
		}
		if (!EqualsIgnoreCase(device, GamepadDevice))
		{
			return std::nullopt;
		}
		for (size_t stick = 0; stick < StickNames.size(); stick++)
		{
			if (EqualsIgnoreCase(control, StickNames[stick]))
			{
				return InputControl{InputControlKind::PadStick, static_cast<uint16_t>(stick)};
			}
		}
		if (std::optional<GamepadButton> const button = GamepadButtonFromString(control))
		{
			return InputControl{InputControlKind::PadButton, static_cast<uint16_t>(*button)};
		}
		if (std::optional<GamepadAxis> const axis = GamepadAxisFromString(control))
		{
			return InputControl{InputControlKind::PadAxis, static_cast<uint16_t>(*axis)};
		}
		return std::nullopt;
	}

	std::string FormatInputControl(InputControl control)
	{
		switch (control.Kind)
		{
			case InputControlKind::Key:
				return fmt::format("{}/{}", KeyDevice, KeyCodeToString(static_cast<KeyCode>(control.Code)));
			case InputControlKind::Mouse:
				return fmt::format("{}/{}", MouseDevice, MouseButtonToString(static_cast<MouseButton>(control.Code)));
			case InputControlKind::PadButton:
				return fmt::format("{}/{}", GamepadDevice, GamepadButtonToString(static_cast<GamepadButton>(control.Code)));
			case InputControlKind::PadAxis:
				return fmt::format("{}/{}", GamepadDevice, GamepadAxisToString(static_cast<GamepadAxis>(control.Code)));
			case InputControlKind::PadStick:
				return fmt::format("{}/{}", GamepadDevice, control.Code < StickNames.size() ? StickNames[control.Code] : "Unknown");
		}
		return "Unknown";
	}

	Result<InputControlList> ParseInputControls(std::string_view text)
	{
		InputControlList list;
		size_t count = 0;
		size_t position = 0;
		while (position < text.size())
		{
			if (text[position] == ' ')
			{
				position++;
				continue;
			}
			size_t end = text.find(' ', position);
			if (end == std::string_view::npos)
			{
				end = text.size();
			}
			std::string_view const token = text.substr(position, end - position);
			position = end;
			std::optional<InputControl> const control = ParseInputControl(token);
			if (!control)
			{
				return MakeError("'{}' names no control (Key/<key>, Mouse/<button>, Gamepad/<button, axis or stick>)", token);
			}
			if (count < MaxInputControlsPerBinding)
			{
				list.Controls[count] = *control;
			}
			count++;
		}
		if (count != 0 && count != 1 && count != 2 && count != 4)
		{
			return MakeError("a binding names one control, two (an axis: negative, positive) or four (a vector: up, down, left, "
			                 "right), not {}",
			                 count);
		}
		list.Count = static_cast<uint8_t>(count);
		return list;
	}

	Result<void> CheckInputBindingControls(InputActionType type, InputControlList const& controls)
	{
		std::span<InputControl const> const parts = controls.Get();
		bool const anyStick = std::ranges::any_of(parts,
		                                          [](InputControl const& control)
		                                          {
													  return control.Kind == InputControlKind::PadStick;
												  });
		if (parts.empty())
		{
			return {};
		}
		if (type == InputActionType::Vector2)
		{
			if (parts.size() == 1 && anyStick)
			{
				return {};
			}
			if (parts.size() == 4 && !anyStick)
			{
				return {};
			}
			return Error{"a Vector2 action takes a stick (Gamepad/LeftStick) or four controls that are not sticks (up, down, left, "
			             "right)"};
		}
		if (anyStick)
		{
			return Error{"a stick gives a Vector2 action's value: a Button or Axis action takes one of its axes (Gamepad/LeftX)"};
		}
		if (parts.size() == 1 || parts.size() == 2)
		{
			return {};
		}
		return Error{"a Button or Axis action takes one control, or two as an axis (negative, positive)"};
	}

	namespace InputProcessing
	{
		glm::vec2 ReadControl(InputControl control, int32_t gamepad)
		{
			switch (control.Kind)
			{
				case InputControlKind::Key:
					return glm::vec2(Input::IsKeyDown(static_cast<KeyCode>(control.Code)) ? 1.0f : 0.0f, 0.0f);
				case InputControlKind::Mouse:
					return glm::vec2(Input::IsMouseButtonDown(static_cast<MouseButton>(control.Code)) ? 1.0f : 0.0f, 0.0f);
				case InputControlKind::PadButton:
				case InputControlKind::PadAxis:
				case InputControlKind::PadStick:
					break;
			}
			if (gamepad != AnyGamepad)
			{
				return gamepad >= 0 && gamepad < static_cast<int32_t>(MaxGamepads) ? ReadPadControl(control, static_cast<uint32_t>(gamepad))
				                                                                   : glm::vec2(0.0f);
			}
			glm::vec2 best(0.0f);
			float bestMagnitude = 0.0f;
			for (uint32_t slot = 0; slot < MaxGamepads; slot++)
			{
				glm::vec2 const reading = ReadPadControl(control, slot);
				float const magnitude = glm::length(reading);
				if (magnitude > bestMagnitude)
				{
					best = reading;
					bestMagnitude = magnitude;
				}
			}
			return best;
		}

		glm::vec2 CombineControls(InputControlList const& controls, std::span<glm::vec2 const> readings)
		{
			auto const part = [&readings](size_t index)
			{
				return index < readings.size() ? std::max(readings[index].x, 0.0f) : 0.0f;
			};
			switch (controls.Count)
			{
				case 1:
					return readings.empty() ? glm::vec2(0.0f) : readings[0];
				case 2:
					return glm::vec2(part(1) - part(0), 0.0f);
				case 4:
					return LimitLength(glm::vec2(part(3) - part(2), part(0) - part(1)));
				default:
					return glm::vec2(0.0f);
			}
		}

		glm::vec2 ReadControls(InputControlList const& controls, int32_t gamepad)
		{
			std::array<glm::vec2, MaxInputControlsPerBinding> readings = {};
			std::span<InputControl const> const parts = controls.Get();
			for (size_t index = 0; index < parts.size(); index++)
			{
				readings[index] = ReadControl(parts[index], gamepad);
			}
			return CombineControls(controls, std::span<glm::vec2 const>(readings.data(), parts.size()));
		}

		glm::vec2 ApplyDeadZone(glm::vec2 value, float deadZone)
		{
			float const magnitude = glm::length(value);
			if (magnitude <= deadZone || magnitude <= 0.0f)
			{
				return glm::vec2(0.0f);
			}
			if (deadZone <= 0.0f)
			{
				return value;
			}
			return value / magnitude * ((magnitude - deadZone) / (1.0f - deadZone));
		}

		glm::vec2 ApplyResponse(glm::vec2 value, InputResponse const& response)
		{
			if (!response)
			{
				return value;
			}
			float const magnitude = glm::length(value);
			if (magnitude <= 0.0f)
			{
				return glm::vec2(0.0f);
			}
			float const mapped = response(magnitude);
			if (!std::isfinite(mapped) || mapped <= 0.0f)
			{
				return glm::vec2(0.0f);
			}
			return value / magnitude * mapped;
		}

		glm::vec2 ProcessBinding(InputBindingSpecification const& binding, glm::vec2 raw)
		{
			glm::vec2 value = ApplyResponse(ApplyDeadZone(raw, binding.DeadZone), binding.Response);
			if (binding.Invert)
			{
				value.x = -value.x;
			}
			if (binding.InvertY)
			{
				value.y = -value.y;
			}
			return value * binding.Scale;
		}
	}

	Result<Ref<InputActionSet const>> InputActionSet::Create(std::vector<InputActionSpecification> actions)
	{
		if (actions.size() > MaxInputActions)
		{
			return MakeError("a map holds at most {} actions, not {}", MaxInputActions, actions.size());
		}
		std::vector<std::string> lowerNames;
		lowerNames.reserve(actions.size());
		for (InputActionSpecification const& action : actions)
		{
			if (action.Name.empty())
			{
				return Error{"an action needs a name"};
			}
			if (action.Name.size() > MaxInputActionNameLength)
			{
				return MakeError("action '{}': a name has at most {} bytes, not {}", action.Name, MaxInputActionNameLength,
				                 action.Name.size());
			}
			std::string lower = ToLowerAscii(action.Name);
			if (std::ranges::find(lowerNames, lower) != lowerNames.end())
			{
				return MakeError("action '{}': another action has that name (names are unique in any letter case)", action.Name);
			}
			lowerNames.push_back(std::move(lower));
			if (!std::isfinite(action.PressThreshold) || !std::isfinite(action.ReleaseThreshold) || action.ReleaseThreshold <= 0.0f ||
			    action.ReleaseThreshold > action.PressThreshold || action.PressThreshold > 1.0f)
			{
				return MakeError("action '{}': thresholds must hold 0 < ReleaseThreshold <= PressThreshold <= 1 (Release {}, Press {})",
				                 action.Name, action.ReleaseThreshold, action.PressThreshold);
			}
			if (action.Bindings.size() > MaxInputBindingsPerAction)
			{
				return MakeError("action '{}': an action has at most {} bindings, not {}", action.Name, MaxInputBindingsPerAction,
				                 action.Bindings.size());
			}
			for (size_t index = 0; index < action.Bindings.size(); index++)
			{
				InputBindingSpecification const& binding = action.Bindings[index];
				if (Result<void> const fits = CheckInputBindingControls(action.Type, binding.Controls); !fits)
				{
					return MakeError("{}: {}", DescribeBinding(action, index), fits.GetError());
				}
				if (binding.Gamepad < AnyGamepad || binding.Gamepad >= static_cast<int32_t>(MaxGamepads))
				{
					return MakeError("{}: Gamepad is -1 (any) or 0 to {}, not {}", DescribeBinding(action, index), MaxGamepads - 1,
					                 binding.Gamepad);
				}
				if (!std::isfinite(binding.DeadZone) || binding.DeadZone < 0.0f || binding.DeadZone > 0.99f)
				{
					return MakeError("{}: DeadZone is 0 to 0.99, not {}", DescribeBinding(action, index), binding.DeadZone);
				}
				if (!std::isfinite(binding.Scale) || binding.Scale < 0.0f)
				{
					return MakeError("{}: Scale is at least 0, not {}", DescribeBinding(action, index), binding.Scale);
				}
			}
		}

		Ref<InputActionSet> set = CreateRef<InputActionSet>(PrivateTag{});
		set->m_Actions = std::move(actions);
		set->m_SortedIndices.resize(set->m_Actions.size());
		for (uint32_t index = 0; index < set->m_SortedIndices.size(); index++)
		{
			set->m_SortedIndices[index] = index;
		}
		std::vector<InputActionSpecification> const& specifications = set->m_Actions;
		std::ranges::sort(set->m_SortedIndices,
		                  [&specifications](uint32_t left, uint32_t right)
		                  {
							  return specifications[left].Name < specifications[right].Name;
						  });
		set->m_Version = s_NextSetVersion++;
		return Ref<InputActionSet const>(std::move(set));
	}

	std::optional<uint32_t> InputActionSet::Find(std::string_view name) const
	{
		auto const found = std::ranges::lower_bound(m_SortedIndices, name, std::less<>(),
		                                            [this](uint32_t index)
		                                            {
														return std::string_view(m_Actions[index].Name);
													});
		if (found == m_SortedIndices.end() || m_Actions[*found].Name != name)
		{
			return std::nullopt;
		}
		return *found;
	}

	void InputActionState::SetActions(Ref<InputActionSet const> actions)
	{
		Ref<InputActionSet const> const previous = std::move(m_Actions);
		std::vector<ActionState> previousStates = std::move(m_States);
		m_Actions = std::move(actions);
		m_States.clear();
		m_Edges.clear();
		m_LoggedNames.clear();
		m_LoggedOverrides.clear();
		m_CheckOverrides = true;
		m_ResetGeneration = Input::GetResetGeneration();
		if (!m_Actions)
		{
			return;
		}

		std::span<InputActionSpecification const> const specifications = m_Actions->GetActions();
		m_States.resize(specifications.size());
		m_Edges.reserve(specifications.size() * 2);
		for (uint32_t index = 0; index < specifications.size(); index++)
		{
			InputActionSpecification const& action = specifications[index];
			ActionState& state = m_States[index];
			for (uint32_t binding = 0; binding < action.Bindings.size(); binding++)
			{
				InputBindingSpecification const& specification = action.Bindings[binding];
				std::span<InputControl const> const controls = specification.Controls.Get();
				for (uint32_t part = 0; part < controls.size(); part++)
				{
					if (!IsDigital(controls[part].Kind))
					{
						continue;
					}
					std::array<glm::vec2, MaxInputControlsPerBinding> readings = {};
					readings[part] = glm::vec2(1.0f, 0.0f);
					glm::vec2 const alone = InputProcessing::ProcessBinding(
						specification, InputProcessing::CombineControls(specification.Controls,
					                                                    std::span<glm::vec2 const>(readings.data(), controls.size())));
					DigitalControl control;
					control.Binding = binding;
					control.Part = part;
					control.CanPress = GetMagnitude(action.Type, alone) >= action.PressThreshold;
					bool const anyPad = controls[part].Kind == InputControlKind::PadButton && specification.Gamepad == AnyGamepad;
					if (!anyPad)
					{
						control.Gamepad = controls[part].Kind == InputControlKind::PadButton ? specification.Gamepad : AnyGamepad;
						state.Controls.push_back(control);
						continue;
					}
					for (uint32_t slot = 0; slot < MaxGamepads; slot++)
					{
						control.Gamepad = static_cast<int32_t>(slot);
						state.Controls.push_back(control);
					}
				}
			}
			TakeBaselines(index);

			std::optional<uint32_t> const kept = previous ? previous->Find(action.Name) : std::nullopt;
			if (!kept || *kept >= previousStates.size())
			{
				continue;
			}
			// Kept by name: the level, the latched edges and the counts of the controls both sets read, so a rebuild makes
			// no edge and loses no tap that came since the last evaluation.
			ActionState const& old = previousStates[*kept];
			InputActionSpecification const& oldAction = previous->GetActions()[*kept];
			state.Value = old.Value;
			state.Magnitude = old.Magnitude;
			state.Down = old.Down;
			state.Pressed = old.Pressed;
			state.Released = old.Released;
			state.ReleasedFirst = old.ReleasedFirst;
			state.PendingPresses = old.PendingPresses;
			state.PendingReleases = old.PendingReleases;
			state.StepPressed = old.StepPressed;
			state.StepReleased = old.StepReleased;
			state.OverridePresses = old.OverridePresses;
			state.OverrideReleases = old.OverrideReleases;
			for (DigitalControl& control : state.Controls)
			{
				InputBindingSpecification const& binding = action.Bindings[control.Binding];
				InputControl const identity = binding.Controls.Controls[control.Part];
				for (DigitalControl const& oldControl : old.Controls)
				{
					InputBindingSpecification const& oldBinding = oldAction.Bindings[oldControl.Binding];
					if (oldBinding.Controls.Controls[oldControl.Part] == identity && oldBinding.Gamepad == binding.Gamepad &&
					    oldControl.Gamepad == control.Gamepad)
					{
						control.Presses = oldControl.Presses;
						control.Releases = oldControl.Releases;
						break;
					}
				}
			}
		}

		// New actions take their current reading and level without an edge.
		for (uint32_t index = 0; index < specifications.size(); index++)
		{
			if (previous && previous->Find(specifications[index].Name))
			{
				continue;
			}
			InputActionSpecification const& action = specifications[index];
			ActionState& state = m_States[index];
			glm::vec2 best(0.0f);
			float bestMagnitude = 0.0f;
			for (InputBindingSpecification const& binding : action.Bindings)
			{
				glm::vec2 const value = InputProcessing::ProcessBinding(binding, ReadBinding(binding));
				float const magnitude = GetMagnitude(action.Type, value);
				if (magnitude > bestMagnitude)
				{
					best = value;
					bestMagnitude = magnitude;
				}
			}
			if (Input::ActionOverride const* record = Input::FindActionOverride(action.Name); record != nullptr && record->Held)
			{
				glm::vec2 const value = action.Type == InputActionType::Vector2 ? record->Value : glm::vec2(record->Value.x, 0.0f);
				float const magnitude = GetMagnitude(action.Type, value);
				if (magnitude > bestMagnitude)
				{
					best = value;
					bestMagnitude = magnitude;
				}
			}
			state.Magnitude = bestMagnitude;
			state.Value = action.Type == InputActionType::Button ? glm::vec2(bestMagnitude, 0.0f)
			              : action.Type == InputActionType::Axis ? glm::vec2(best.x, 0.0f)
			                                                     : best;
			state.Down = bestMagnitude >= action.PressThreshold;
		}
	}

	void InputActionState::Clear()
	{
		m_Actions = nullptr;
		m_States.clear();
		m_Edges.clear();
		m_LoggedNames.clear();
		m_LoggedOverrides.clear();
		m_InFixedStep = false;
		m_CheckOverrides = true;
	}

	void InputActionState::TakeBaselines(uint32_t action)
	{
		InputActionSpecification const& specification = m_Actions->GetActions()[action];
		ActionState& state = m_States[action];
		for (DigitalControl& control : state.Controls)
		{
			InputBindingSpecification const& binding = specification.Bindings[control.Binding];
			Input::ButtonCounts const counts = ReadCounts(binding.Controls.Controls[control.Part], control.Gamepad);
			control.Presses = counts.Presses;
			control.Releases = counts.Releases;
		}
		Input::ActionOverride const* record = Input::FindActionOverride(specification.Name);
		state.OverridePresses = record != nullptr ? record->Presses : 0;
		state.OverrideReleases = record != nullptr ? record->Releases : 0;
	}

	void InputActionState::LogUnknownOverrides()
	{
		Input::ForEachActionOverride(
			[this](std::string_view name, Input::ActionOverride const& record)
			{
				if (!record.Held || m_Actions->Find(name) || m_LoggedOverrides.contains(name))
				{
					return;
				}
				m_LoggedOverrides.emplace(name);
				ST_CORE_WARN("Input actions: the override of '{}' names no action of the project's input actions; it is ignored", name);
			});
	}

	void InputActionState::Evaluate()
	{
		m_Edges.clear();
		if (!m_Actions)
		{
			return;
		}
		ST_PROFILE_SCOPE("Input.EvaluateActions");
		uint64_t const generation = Input::GetResetGeneration();
		bool const rebaseline = generation != m_ResetGeneration;
		m_ResetGeneration = generation;
		bool const hasOverrides = Input::HasActionOverrides();
		uint64_t const overridesVersion = Input::GetActionOverridesVersion();
		if (hasOverrides && (m_CheckOverrides || overridesVersion != m_OverridesVersion))
		{
			LogUnknownOverrides();
		}
		m_OverridesVersion = overridesVersion;
		m_CheckOverrides = false;

		std::span<InputActionSpecification const> const specifications = m_Actions->GetActions();
		for (uint32_t index = 0; index < specifications.size(); index++)
		{
			InputActionSpecification const& action = specifications[index];
			ActionState& state = m_States[index];

			std::array<float, MaxInputBindingsPerAction> magnitudes = {};
			glm::vec2 best(0.0f);
			float bestMagnitude = 0.0f;
			for (size_t binding = 0; binding < action.Bindings.size(); binding++)
			{
				InputBindingSpecification const& specification = action.Bindings[binding];
				glm::vec2 const value = InputProcessing::ProcessBinding(specification, ReadBinding(specification));
				magnitudes[binding] = GetMagnitude(action.Type, value);
				if (magnitudes[binding] > bestMagnitude)
				{
					best = value;
					bestMagnitude = magnitudes[binding];
				}
			}
			Input::ActionOverride const* record = hasOverrides ? Input::FindActionOverride(action.Name) : nullptr;
			float overrideMagnitude = 0.0f;
			if (record != nullptr && record->Held)
			{
				glm::vec2 const value = action.Type == InputActionType::Vector2 ? record->Value : glm::vec2(record->Value.x, 0.0f);
				overrideMagnitude = GetMagnitude(action.Type, value);
				if (overrideMagnitude > bestMagnitude)
				{
					best = value;
					bestMagnitude = overrideMagnitude;
				}
			}

			bool const wasDown = state.Down;
			bool const down = wasDown ? bestMagnitude >= action.ReleaseThreshold : bestMagnitude >= action.PressThreshold;
			state.Magnitude = bestMagnitude;
			state.Value = action.Type == InputActionType::Button ? glm::vec2(bestMagnitude, 0.0f)
			              : action.Type == InputActionType::Axis ? glm::vec2(best.x, 0.0f)
			                                                     : best;
			state.Down = down;

			// What the counts say happened between the two evaluations.
			bool tapped = false;
			bool pressedAgain = false;
			if (rebaseline)
			{
				TakeBaselines(index);
			}
			else
			{
				// The action's largest magnitude but for the binding (or the override) given: whether it would have gone up
				// with that one control up.
				auto const othersBelowRelease = [&](size_t exceptBinding, float exceptBindingMagnitude, bool exceptOverride)
				{
					float others = exceptOverride ? 0.0f : overrideMagnitude;
					for (size_t binding = 0; binding < action.Bindings.size(); binding++)
					{
						others = std::max(others, binding == exceptBinding ? exceptBindingMagnitude : magnitudes[binding]);
					}
					return others < action.ReleaseThreshold;
				};
				for (DigitalControl& control : state.Controls)
				{
					InputBindingSpecification const& binding = action.Bindings[control.Binding];
					InputControl const identity = binding.Controls.Controls[control.Part];
					Input::ButtonCounts const counts = ReadCounts(identity, control.Gamepad);
					bool const pressesMoved = counts.Presses != control.Presses;
					bool const releasesMoved = counts.Releases != control.Releases;
					control.Presses = counts.Presses;
					control.Releases = counts.Releases;
					if (!pressesMoved || !releasesMoved)
					{
						continue;
					}
					if (!wasDown && !down)
					{
						tapped = tapped || control.CanPress;
					}
					else if (wasDown && down && !pressedAgain)
					{
						if (IsDigitalDown(identity, control.Gamepad))
						{
							// Released and pressed again: the action went up meanwhile if it is below the release threshold with
							// this control up (on its pad alone, for a binding of any gamepad).
							float const without = GetMagnitude(
								action.Type,
								InputProcessing::ProcessBinding(
									binding, ReadBinding(binding, control.Part, ReadPartWithout(binding, identity, control.Gamepad))));
							pressedAgain = control.CanPress && othersBelowRelease(control.Binding, without, false);
						}
						else
						{
							// Pressed and released: the action went up meanwhile if this control down takes it below the release
							// threshold (a composite's part cancelling the part that holds the action).
							float const with = GetMagnitude(
								action.Type,
								InputProcessing::ProcessBinding(binding, ReadBinding(binding, control.Part, glm::vec2(1.0f, 0.0f))));
							pressedAgain = othersBelowRelease(control.Binding, with, false);
						}
					}
				}
				if (record != nullptr)
				{
					bool const pressesMoved = record->Presses != state.OverridePresses;
					bool const releasesMoved = record->Releases != state.OverrideReleases;
					// The largest magnitude of the presses since the last evaluation, so a strong press followed by a weak one
					// before it is still a press.
					float const peak = action.Type == InputActionType::Vector2 ? record->PeaksLength.GetSince(state.OverridePresses)
					                                                           : record->PeaksX.GetSince(state.OverridePresses);
					state.OverridePresses = record->Presses;
					state.OverrideReleases = record->Releases;
					if (pressesMoved && releasesMoved && peak >= action.PressThreshold)
					{
						if (!wasDown && !down)
						{
							tapped = true;
						}
						else if (wasDown && down && !pressedAgain)
						{
							pressedAgain = othersBelowRelease(MaxInputBindingsPerAction, 0.0f, true);
						}
					}
				}
				else
				{
					state.OverridePresses = 0;
					state.OverrideReleases = 0;
				}
			}

			state.Pressed = false;
			state.Released = false;
			state.ReleasedFirst = false;
			if (!wasDown && down)
			{
				state.Pressed = true;
				m_Edges.push_back({index, true});
			}
			else if (wasDown && !down)
			{
				state.Released = true;
				m_Edges.push_back({index, false});
			}
			else if (tapped)
			{
				state.Pressed = true;
				state.Released = true;
				m_Edges.push_back({index, true});
				m_Edges.push_back({index, false});
			}
			else if (pressedAgain)
			{
				state.Pressed = true;
				state.Released = true;
				state.ReleasedFirst = true;
				m_Edges.push_back({index, false});
				m_Edges.push_back({index, true});
			}
			state.PendingPresses += state.Pressed ? 1u : 0u;
			state.PendingReleases += state.Released ? 1u : 0u;
		}
	}

	void InputActionState::BeginFixedStep()
	{
		ST_CORE_ASSERT(!m_InFixedStep, "InputActionState: fixed steps do not nest");
		m_InFixedStep = true;
		m_StepNumber++;
		for (ActionState& state : m_States)
		{
			if (state.PendingPresses > 0)
			{
				state.StepPressed = m_StepNumber;
			}
			if (state.PendingReleases > 0)
			{
				state.StepReleased = m_StepNumber;
			}
			state.PendingPresses = 0;
			state.PendingReleases = 0;
		}
	}

	void InputActionState::EndFixedStep()
	{
		m_InFixedStep = false;
	}

	InputActionState::FixedStepScope::FixedStepScope(InputActionState& state)
		: m_State(state)
	{
		m_State.BeginFixedStep();
	}

	InputActionState::FixedStepScope::~FixedStepScope()
	{
		m_State.EndFixedStep();
	}

	float InputActionState::GetValue(uint32_t action) const
	{
		if (action >= m_States.size())
		{
			return 0.0f;
		}
		InputActionType const type = m_Actions->GetActions()[action].Type;
		return type == InputActionType::Vector2 ? glm::length(m_States[action].Value) : m_States[action].Value.x;
	}

	glm::vec2 InputActionState::GetVector(uint32_t action) const
	{
		return action < m_States.size() ? m_States[action].Value : glm::vec2(0.0f);
	}

	bool InputActionState::IsDown(uint32_t action) const
	{
		return action < m_States.size() && m_States[action].Down;
	}

	bool InputActionState::HasStepEdge(uint64_t stepOfEdge, InputActionView view, uint64_t step) const
	{
		if (m_InFixedStep)
		{
			return stepOfEdge == m_StepNumber;
		}
		return view == InputActionView::FixedStep && step != 0 && stepOfEdge == step;
	}

	bool InputActionState::IsPressed(uint32_t action, InputActionView view, uint64_t step) const
	{
		if (action >= m_States.size())
		{
			return false;
		}
		ActionState const& state = m_States[action];
		return !m_InFixedStep && view == InputActionView::Frame ? state.Pressed : HasStepEdge(state.StepPressed, view, step);
	}

	bool InputActionState::IsReleased(uint32_t action, InputActionView view, uint64_t step) const
	{
		if (action >= m_States.size())
		{
			return false;
		}
		ActionState const& state = m_States[action];
		return !m_InFixedStep && view == InputActionView::Frame ? state.Released : HasStepEdge(state.StepReleased, view, step);
	}

	std::optional<uint32_t> InputActionState::Resolve(std::string_view name)
	{
		if (m_Actions)
		{
			if (std::optional<uint32_t> const index = m_Actions->Find(name))
			{
				return index;
			}
		}
		if (m_LoggedNames.contains(name))
		{
			return std::nullopt;
		}
		m_LoggedNames.emplace(name);
		if (!m_Actions)
		{
			ST_CORE_WARN("Input actions: no input actions are loaded (the project names none, or they do not load), so action '{}' "
			             "reads zero",
			             name);
		}
		else
		{
			ST_CORE_WARN("Input actions: no action is named '{}' (names match with their letter case); it reads zero", name);
		}
		return std::nullopt;
	}
}
