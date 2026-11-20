#include "Strada/Project/PlayerWindow.h"
#include "Strada/Project/ProjectSettings.h"
#include "Strada/Script/PlayerSettings.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>

using namespace Strada;

// The game's side of the player's settings (Docs/Architecture.md §13.1, "The player's settings"): their defaults are the
// game's window, and the window the player opens is the settings under its command line.
TEST_CASE("PlayerWindow: the player's settings default to the game's window, and the command line wins over them")
{
	ProjectWindowSettings game;
	game.Width = 1920;
	game.Height = 1080;
	game.Fullscreen = true;
	game.VSync = false;
	PlayerSettings const defaults = MakePlayerSettingsDefaults(game);
	CHECK(defaults.Window == PlayerWindowSettings{PlayerWindowMode::Fullscreen, 1920, 1080, false});
	CHECK(defaults.Quality == QualityPreset::High);
	CHECK(defaults.MasterVolume == 1.0f);
	CHECK(defaults.Buses.empty());
	CHECK(defaults.Values.empty());
	// Sizes a window made in code may have are brought within the settings' bounds.
	game.Width = 1;
	game.Height = 100000;
	CHECK(MakePlayerSettingsDefaults(game).Window.Width == MinPlayerWindowSize);
	CHECK(MakePlayerSettingsDefaults(game).Window.Height == MaxPlayerWindowSize);

	// Without overrides the settings are the window, today's player's window when nobody chose any.
	PlayerWindowSettings const fullscreen = {PlayerWindowMode::Fullscreen, 1600, 900, true};
	PlayerWindowState state = ResolvePlayerWindow(fullscreen, {});
	CHECK(state.Fullscreen);
	CHECK(state.Size == glm::uvec2(1600, 900));
	CHECK(state.VSync);
	CHECK(ResolvePlayerWindow({PlayerWindowMode::Windowed, 800, 600, false}, {}) == PlayerWindowState{false, glm::uvec2(800, 600), false});

	// --resolution: a window of the size it names, never fullscreen, whatever the settings say; vsync stays theirs.
	PlayerWindowOverrides resolution;
	resolution.KeepSize = true;
	state = ResolvePlayerWindow(fullscreen, resolution);
	CHECK_FALSE(state.Fullscreen);
	CHECK_FALSE(state.Size.has_value());
	CHECK(state.VSync);

	// --no-vsync: off, whatever the settings say.
	PlayerWindowOverrides noVSync;
	noVSync.NoVSync = true;
	state = ResolvePlayerWindow(fullscreen, noVSync);
	CHECK(state.Fullscreen);
	CHECK_FALSE(state.VSync);
}
