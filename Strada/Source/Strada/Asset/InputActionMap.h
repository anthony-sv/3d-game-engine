#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/InputActions.h"

#include <cstdint>
#include <string>
#include <vector>

// The input action map (Docs/Architecture.md §7.4): a game's actions and the bindings that give them values. The data of
// an InputActions asset (.sinput; Strada/Asset/InputActionsAsset.h has its field table, validation, registration and the
// compilation into the runtime's InputActionSet). A project names one (ProjectSettings::InputActions). Plain data, any
// thread.
namespace Strada
{
	// One way to give an action a value. Rows of the editor's list are titled by Controls (the first text field).
	struct InputActionBinding
	{
		// One control name, or a composite of two (an axis: negative, then positive) or four (a vector: up, down, left,
		// right), separated by spaces: "Key/Space", "Gamepad/RightTrigger", "Gamepad/LeftStick", "Key/A Key/D",
		// "Key/W Key/S Key/A Key/D" (ParseInputControls). Empty leaves the binding unbound: it reads 0.
		std::string Controls;
		// The gamepad its gamepad controls read: AnyGamepad (-1) or 0 to 15.
		int32_t Gamepad = AnyGamepad;
		// Magnitudes up to it read 0, and the rest is stretched to reach 1 again (0 to 0.99).
		float DeadZone = 0.0f;
		// A Curve asset applied to the magnitude after the dead zone; none for the identity.
		AssetHandle ResponseCurve;
		// Negates the value (a two-dimensional value's X).
		bool Invert = false;
		// Negates a two-dimensional value's Y; one-dimensional bindings ignore it.
		bool InvertY = false;
		// Multiplies the value, last (at least 0).
		float Scale = 1.0f;

		bool operator==(InputActionBinding const& other) const = default;
	};

	// A named value of the game. Rows of the editor's list are titled by Name.
	struct InputAction
	{
		// Unique in the map in any letter case; what scripts, graphs and input.set name it by (with its letter case).
		std::string Name;
		InputActionType Type = InputActionType::Button;
		// The magnitude at which the action goes down, and the one below which it goes up again: 0 < Release <= Press <= 1.
		float PressThreshold = 0.5f;
		float ReleaseThreshold = 0.4f;
		// In the order ties between equal magnitudes are decided (the first wins).
		std::vector<InputActionBinding> Bindings;

		bool operator==(InputAction const& other) const = default;
	};

	struct InputActionMap
	{
		std::vector<InputAction> Actions;

		bool operator==(InputActionMap const& other) const = default;
	};
}
