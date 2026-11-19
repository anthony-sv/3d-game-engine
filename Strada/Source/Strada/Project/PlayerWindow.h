#pragma once

#include "Strada/Project/ProjectSettings.h"
#include "Strada/Script/PlayerSettings.h"

#include <glm/glm.hpp>

#include <optional>

// The game's side of the player's settings (Docs/Architecture.md §13.1, "The player's settings"): their defaults, which are
// the game's own window settings, and the window the player opens or changes to, which the game's window settings, the
// player's settings and the player's command line decide together. Pure functions, any thread.
namespace Strada
{
	// The player's settings of a game nobody chose any for: the game's window (windowed or fullscreen, its size and vsync),
	// then PlayerSettings' own defaults (High, a master volume of 1, no bus and no value). A settings file's missing fields
	// keep these.
	[[nodiscard]] PlayerSettings MakePlayerSettingsDefaults(ProjectWindowSettings const& window);

	// What the player's command line fixes about its window, whatever the settings say.
	struct PlayerWindowOverrides
	{
		// --resolution: a window of the drawing size the command line names, never fullscreen, whose size no setting changes
		// (the player sets that size itself, and stops a run whose window does not have it).
		bool KeepSize = false;
		// --no-vsync: presents without waiting, whatever the settings say.
		bool NoVSync = false;
	};

	// The window the player opens with, or changes to when a script changes the player's window settings.
	struct PlayerWindowState
	{
		// Borderless fullscreen on the window's monitor.
		bool Fullscreen = false;
		// The windowed size in screen coordinates (the size the window returns to from fullscreen); none leaves the window's
		// size as it is (PlayerWindowOverrides::KeepSize).
		std::optional<glm::uvec2> Size;
		bool VSync = true;

		bool operator==(PlayerWindowState const& other) const = default;
	};

	// The player's window settings under the command line's overrides. A headless run applies none of it: it keeps the
	// settings without a window.
	[[nodiscard]] PlayerWindowState ResolvePlayerWindow(PlayerWindowSettings const& window, PlayerWindowOverrides const& overrides);
}
