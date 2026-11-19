#include "stpch.h"
#include "Strada/Project/PlayerWindow.h"

#include <algorithm>

namespace Strada
{
	PlayerSettings MakePlayerSettingsDefaults(ProjectWindowSettings const& window)
	{
		PlayerSettings settings;
		settings.Window.Mode = window.Fullscreen ? PlayerWindowMode::Fullscreen : PlayerWindowMode::Windowed;
		// The project's sizes have the player's bounds; a window made in code may not.
		settings.Window.Width = std::clamp(window.Width, MinPlayerWindowSize, MaxPlayerWindowSize);
		settings.Window.Height = std::clamp(window.Height, MinPlayerWindowSize, MaxPlayerWindowSize);
		settings.Window.VSync = window.VSync;
		return settings;
	}

	PlayerWindowState ResolvePlayerWindow(PlayerWindowSettings const& window, PlayerWindowOverrides const& overrides)
	{
		PlayerWindowState state;
		state.Fullscreen = !overrides.KeepSize && window.Mode == PlayerWindowMode::Fullscreen;
		if (!overrides.KeepSize)
		{
			state.Size = glm::uvec2(std::clamp(window.Width, MinPlayerWindowSize, MaxPlayerWindowSize),
			                        std::clamp(window.Height, MinPlayerWindowSize, MaxPlayerWindowSize));
		}
		state.VSync = window.VSync && !overrides.NoVSync;
		return state;
	}
}
