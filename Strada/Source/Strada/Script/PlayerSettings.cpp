#include "stpch.h"
#include "Strada/Script/PlayerSettings.h"

namespace Strada
{
	// The settings' named values are script fields: a change to the script field form takes the next version of the settings
	// (and of slots, scenes and prefabs; Docs/Architecture.md §7.3, "Adding a version", step 3), so that an older engine reads
	// a later game's settings as Newer. These pin the form version 1 holds: raise PlayerSettingsFormat.Version, with its
	// migration, and these checks with it.
	static_assert(PlayerSettingsFormat.Version == 1 && LastScriptFieldElementType == ScriptFieldType::Asset &&
	                  static_cast<uint8_t>(LastScriptFieldElementType) == 16 && MaxScriptFieldListElements == 4096,
	              "the script field form changed: the player's settings need their next version");
	static_assert(PlayerSettingsMigrations.size() + 1 == PlayerSettingsFormat.Version, "one migration per older version");

	namespace
	{
		// The settings' field tables read and write as a struct document.
		[[maybe_unused]] constexpr auto ReadSettingsDocument = &ReadStructDocument<PlayerSettings>;
		[[maybe_unused]] constexpr auto WriteSettingsDocument = &WriteStructDocument<PlayerSettings>;
	}
}
