#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/InputActionMap.h"
#include "Strada/Asset/InputBindingProfile.h"
#include "Strada/Core/InputCapture.h"
#include "Strada/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// The bindings a player changed while the game runs (Docs/Architecture.md §7.4, "Rebinding"): one table for the process,
// for the project's InputActions asset, read from its profile in the game data folder (Strada/Asset/InputBindingProfile.h)
// when the actions load and written there atomically and durably (FileSystem::WriteTextFileDurably) when a binding
// changes. Scenes compile their actions from the asset
// with the table applied (Apply) and compile again when its version moves, so a rebind reaches every running scene at its
// next frame and outlives scene loads. The asset is never written. Main thread.
namespace Strada
{
	// What a capture of one binding listens for (InputRebinding::PlanCapture).
	struct InputCapturePlan
	{
		InputCaptureTarget Target = InputCaptureTarget::Control;
		// The gamepad the binding reads (AnyGamepad or a slot).
		int32_t Gamepad = AnyGamepad;
		// The binding's controls now.
		std::string Controls;
	};

	class InputRebinding
	{
	public:
		// Makes the table the asset's. When it holds another asset, or was read with another game data folder (or none), it
		// is emptied and the asset's profile is read, if a game data folder exists and the asset is a file: entries that no
		// longer match the asset are dropped with one log line; a profile that cannot be read (damaged, of a newer version)
		// is logged and left as it is until a binding changes, which writes it again. Otherwise the table is kept (a scene
		// loaded after another, a second running scene). Called by a scene when its actions load.
		static void Load(AssetHandle actions);
		// Forgets the table (nothing is written): the editor at the end of play mode and when a project closes or opens, so
		// that the next play reads the profile again; tests. Moves the version.
		static void Clear();
		// Moved by every change of the table (a load, a rebind or reset that changed a binding, Clear); never repeated.
		static uint64_t GetVersion();
		// The asset whose table it is (invalid when none).
		static AssetHandle GetActions();
		// The entries the table holds now, as the profile would be written.
		static InputBindingProfile const& GetProfile();

		// The asset's map with the table's entries in place, when the table is the asset's; the map as it is otherwise.
		// Entries that no longer match the map (the asset was edited while the game runs) are dropped from the table with one
		// log line; dropping moves no version, since the bindings read do not change.
		[[nodiscard]] static InputActionMap Apply(AssetHandle actions, InputActionMap const& map);

		// Sets one binding of the asset's actions (loading the table for the asset first when it holds another): `part` -1
		// replaces the binding's controls with `controls` (one control, or two or four as a composite; empty unbinds it);
		// `part` 0 to 3 replaces that control of a composite with the one control `controls` names (not a stick). The new
		// controls must fit the action's type. Done answers the binding's new controls and the actions it now conflicts
		// with (FindInputBindingConflicts); the binding is applied whatever the conflicts. Controls the binding reads already
		// answer Done and change nothing (no write, no version, no log). Otherwise the profile is written (a binding set back
		// to the asset's controls leaves it; a binding changed again keeps its entry's place), the version moves and one line
		// is logged; a write that fails is logged and said in Message, and the binding holds for this run. Refused, changing
		// nothing: no actions (an invalid handle, an asset that does not load), an unknown action, a binding out of range, a
		// part below -1 or beyond the binding's, controls that do not parse or fit.
		[[nodiscard]] static InputRebindResult Rebind(AssetHandle actions, std::string_view action, int32_t binding,
		                                              std::string_view controls, int32_t part);
		// Every binding back to the asset's: the table emptied and, when it held entries, the profile written without them.
		// Done (Binding -1), or Refused without actions.
		[[nodiscard]] static InputRebindResult ResetBindings(AssetHandle actions);
		// The binding's controls now (with the table applied); an error naming what is missing otherwise. The map it reads is
		// kept until the asset (the data asset registry's epoch) or the table (the version) changes, since a settings
		// screen's graph asks every frame.
		[[nodiscard]] static Result<std::string> GetBindingControls(AssetHandle actions, std::string_view action, int32_t binding);
		// What a capture of the binding listens for, by the action's type and the part (InputCaptureTarget), with the
		// binding's gamepad and controls now; refused as Rebind refuses (a part needs a composite).
		[[nodiscard]] static Result<InputCapturePlan> PlanCapture(AssetHandle actions, std::string_view action, int32_t binding,
		                                                          int32_t part);

		// The profile file of the asset: <game data>/Input/<the asset's file name>.json. Nothing without a game data folder
		// (no game runs) or for an asset that is no file (a memory or built-in asset).
		[[nodiscard]] static std::optional<std::filesystem::path> GetProfilePath(AssetHandle actions);
	};
}
