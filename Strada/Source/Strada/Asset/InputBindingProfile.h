#pragma once

#include "Strada/Asset/InputActionMap.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructDocument.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// A player's binding profile (Docs/Architecture.md §7.4, "Rebinding", and §7.3): the bindings of an input action map that a
// player changed while the game ran, kept in the game data folder (<game data>/Input/<the asset's file name>.json) and read
// when the actions load. Only bindings that differ from the asset's are kept, each by action name and binding index with the
// controls it replaced, so that an entry the asset no longer matches is dropped rather than applied to another binding.
// A versioned struct document (Serialization/StructDocument.h), not a data asset. Plain functions, any thread.
namespace Strada
{
	// One binding a player changed.
	struct InputBindingProfileEntry
	{
		// The action's name, with its letter case.
		std::string Action;
		// The binding's index in its action.
		uint32_t Binding = 0;
		// The binding's controls now (a binding's text: one control, or two or four of a composite; empty unbinds it).
		std::string Controls;
		// The asset's controls this entry replaced: the entry applies only while the asset's binding still reads them.
		std::string Replaced;

		bool operator==(InputBindingProfileEntry const& other) const = default;
	};

	struct InputBindingProfile
	{
		std::vector<InputBindingProfileEntry> Bindings;

		bool operator==(InputBindingProfile const& other) const = default;
	};

	// At most one entry per binding of the largest map.
	inline constexpr size_t MaxInputBindingProfileEntries = MaxInputActions * MaxInputBindingsPerAction;

	template<>
	struct StructTraits<InputBindingProfileEntry>
	{
		static constexpr std::string_view Name = "InputBindingProfileEntry";
		static constexpr auto Fields =
			std::make_tuple(Field("Action", &InputBindingProfileEntry::Action).Doc("The action's name, with its letter case."),
		                    Field("Binding", &InputBindingProfileEntry::Binding)
		                        .Range(0.0, static_cast<double>(MaxInputBindingsPerAction - 1))
		                        .Doc("The binding's index in its action."),
		                    Field("Controls", &InputBindingProfileEntry::Controls).Doc("The binding's controls as the player set them."),
		                    Field("Replaced", &InputBindingProfileEntry::Replaced)
		                        .Doc("The asset's controls they replaced: the entry applies only while the asset's binding reads these."));
	};

	template<>
	struct StructTraits<InputBindingProfile>
	{
		static constexpr std::string_view Name = "InputBindingProfile";
		static constexpr auto Fields = std::make_tuple(Field("Bindings", &InputBindingProfile::Bindings)
		                                                   .MaxCount(MaxInputBindingProfileEntries)
		                                                   .Doc("The bindings that differ from the asset's, in the order they were set."));
	};

	inline constexpr std::array<DocumentMigration, 0> InputBindingProfileMigrations = {};

	// { "Strada": { "Version": 1, "Type": "InputBindingProfile" }, "InputBindingProfile": { "Bindings": [ ... ] } }
	inline constexpr StructDocumentFormat InputBindingProfileFormat = {
		.Type = "InputBindingProfile",
		.Version = 1,
		.Migrations = InputBindingProfileMigrations,
	};

	// Reads a profile document: refuses another type, a version newer than this engine's, and fields of the wrong kind or out
	// of their ranges; unknown fields are warned about (into `warnings`) and skipped. What its entries say is not checked
	// against any map here (FitInputBindingProfile does).
	[[nodiscard]] Result<InputBindingProfile> ReadInputBindingProfile(Json const& document, std::vector<std::string>* warnings = nullptr);
	// The document of a profile in the current version.
	[[nodiscard]] Json WriteInputBindingProfile(InputBindingProfile const& profile);

	// A profile's entries split by whether they apply to a map.
	struct InputBindingProfileFit
	{
		// The entries that apply, in their order.
		InputBindingProfile Kept;
		// Each entry that does not, described with why ("Jump binding 3: the action has 2 bindings"), in their order.
		std::vector<std::string> Dropped;
	};

	// Keeps the entries that apply to the map: the action exists (its exact name), the binding index is below its count, the
	// asset's binding still reads the controls the entry replaced (compared as controls, in any letter case), the entry's
	// controls are a binding's text that fits the action's type, they differ from the asset's, and no earlier entry names the
	// same binding. Drops the others.
	[[nodiscard]] InputBindingProfileFit FitInputBindingProfile(InputBindingProfile const& profile, InputActionMap const& map);
	// The map with the entries' controls in place of the asset's, for entries that apply (those FitInputBindingProfile
	// keeps); other entries change nothing.
	[[nodiscard]] InputActionMap ApplyInputBindingProfile(InputActionMap map, InputBindingProfile const& profile);

	// The actions other than the binding's own whose bindings read a control the binding reads, in the order of the map's
	// actions, each once. Two controls meet when they are the same control, or a stick and one of its axes; gamepad controls
	// meet only on a gamepad both bindings read (either reads any gamepad, or both the same slot). Bindings whose controls
	// do not parse meet nothing. Nothing for an action or binding the map lacks.
	[[nodiscard]] std::vector<std::string> FindInputBindingConflicts(InputActionMap const& map, size_t action, size_t binding);
}
