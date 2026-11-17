#pragma once

#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionMap.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructDocument.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <string_view>
#include <tuple>
#include <utility>

// The InputActions data asset type (.sinput, Docs/Architecture.md §7.4 and §7.3): the input action map
// (Strada/Asset/InputActionMap.h) through the data asset registration (§6.4), and its compilation into the runtime's
// InputActionSet (Strada/Core/InputActions.h). Every reader of input action assets includes this header.
namespace Strada
{
	template<>
	struct EnumTraits<InputActionType>
	{
		static constexpr std::array<std::pair<InputActionType, std::string_view>, 3> Values = {{
			{InputActionType::Button, "Button"},
			{InputActionType::Axis, "Axis"},
			{InputActionType::Vector2, "Vector2"},
		}};
	};

	template<>
	struct StructTraits<InputActionBinding>
	{
		static constexpr std::string_view Name = "InputActionBinding";
		static constexpr auto Fields = std::make_tuple(
			Field("Controls", &InputActionBinding::Controls)
				.Doc("One control (Key/Space, Mouse/Left, Gamepad/A, Gamepad/RightTrigger, Gamepad/LeftStick), or two as an axis "
		             "(negative, positive) or four as a vector (up, down, left, right), separated by spaces; empty reads 0."),
			Field("Gamepad", &InputActionBinding::Gamepad)
				.Range(static_cast<double>(AnyGamepad), static_cast<double>(MaxGamepads - 1))
				.Doc("The gamepad its gamepad controls read: -1 for any connected gamepad (the largest reading), or 0 to 15."),
			Field("DeadZone", &InputActionBinding::DeadZone)
				.Range(0.0, 0.99)
				.Doc("Magnitudes up to it read 0, and the rest is stretched to reach 1 again; a stick's is radial."),
			Field("ResponseCurve", &InputActionBinding::ResponseCurve)
				.References("Curve")
				.Doc("A curve applied to the magnitude after the dead zone (0 to 1 in, the magnitude out); none for a straight line."),
			Field("Invert", &InputActionBinding::Invert).Doc("Negates the value (a stick's or a vector composite's X)."),
			Field("InvertY", &InputActionBinding::InvertY).Doc("Negates a stick's or a vector composite's Y; other bindings ignore it."),
			Field("Scale", &InputActionBinding::Scale).AtLeast(0.0).Doc("Multiplies the value, after the curve and the inversion."));
	};

	template<>
	struct StructTraits<InputAction>
	{
		static constexpr std::string_view Name = "InputAction";
		static constexpr auto Fields = std::make_tuple(
			Field("Name", &InputAction::Name)
				.Doc("What scripts, graphs and input.set name the action by; unique in the map in any letter case, at most 64 bytes."),
			Field("Type", &InputAction::Type)
				.Doc("Button (0 up to 1, read as a press), Axis (a signed value, -1 to 1) or Vector2 (two axes, +Y up)."),
			Field("PressThreshold", &InputAction::PressThreshold)
				.Range(0.0, 1.0)
				.Doc("The magnitude at which the action goes down (pressed); above 0."),
			Field("ReleaseThreshold", &InputAction::ReleaseThreshold)
				.Range(0.0, 1.0)
				.Doc("The magnitude below which a down action goes up (released); above 0 and at most PressThreshold."),
			Field("Bindings", &InputAction::Bindings)
				.MaxCount(MaxInputBindingsPerAction)
				.Doc("The controls that give the action its value; the one with the largest magnitude wins (the first on a tie)."));
	};

	template<>
	struct StructTraits<InputActionMap>
	{
		static constexpr std::string_view Name = "InputActions";
		static constexpr auto Fields = std::make_tuple(
			Field("Actions", &InputActionMap::Actions).MaxCount(MaxInputActions).Doc("The game's actions, each with its bindings."));
	};

	// Data assets refuse a field no generic editor can show; an array of structs that hold an array is allowed (only an
	// array whose elements are arrays is refused), so actions hold their bindings.
	static_assert(Detail::IsDescribedFieldType<InputActionMap>(), "An InputActions asset must be editable by the generic editors");

	// Checks what field ranges cannot: names non-empty, at most MaxInputActionNameLength bytes and unique in any letter
	// case; 0 < ReleaseThreshold <= PressThreshold; every binding's Controls read by ParseInputControls and fitting its
	// action's type (CheckInputBindingControls). The error names the action and the binding.
	[[nodiscard]] Result<void> ValidateInputActionMap(InputActionMap const& map);

	inline constexpr std::array<DocumentMigration, 0> InputActionsMigrations = {};

	inline constexpr DataAssetTypeSpecification<InputActionMap> InputActionsAssetSpecification = {
		.Name = "InputActions",
		.Extension = ".sinput",
		.FormatVersion = 1,
		.Description = "A game's input actions (buttons, axes and vectors) and the keys, buttons and sticks bound to them.",
		.Migrations = InputActionsMigrations,
		.Validate = &ValidateInputActionMap,
	};

	// A response curve a compiled set resolved, with the version its asset had then (0: it did not load).
	struct InputResponseCurveUse
	{
		AssetHandle Curve;
		uint64_t Version = 0;

		bool operator==(InputResponseCurveUse const& other) const = default;
	};

	// The runtime's set made from a map: controls parsed, response curves resolved from their Curve assets as they are now
	// (each evaluates a copy of the curve's keys exactly; a curve that does not load is the straight line, logged once per
	// call and curve). With `curves`, also the distinct curves it resolved, in handle order, so that the caller can tell
	// when one changed (GetInputResponseCurveVersion). Refuses what ValidateInputActionMap refuses. Main thread (it reads
	// the asset manager).
	[[nodiscard]] Result<Ref<InputActionSet const>> CompileInputActions(InputActionMap const& map,
	                                                                    std::vector<InputResponseCurveUse>* curves = nullptr);
	// The version of the Curve asset as CompileInputActions would record it now (0: it does not load). Main thread.
	[[nodiscard]] uint64_t GetInputResponseCurveVersion(AssetHandle curve);
}
