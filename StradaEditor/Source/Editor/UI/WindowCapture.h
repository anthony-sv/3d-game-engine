#pragma once

#include "Strada/Core/Result.h"

#include <cstdint>
#include <string_view>

struct ImGuiViewport;

namespace Strada
{
	// A rectangle of the editor window's back buffer, in pixels from its top-left corner.
	struct ScreenshotRect
	{
		uint32_t X = 0;
		uint32_t Y = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;

		bool operator==(ScreenshotRect const& other) const = default;
	};

	// Where an editor window (an ImGui window, by its name: "Game View", "Inspector", a graph window's "<title>###VisualScript<n>")
	// lies in the main viewport's back buffer: its rectangle (ImGuiWindow::Pos and Size) relative to the viewport, times the
	// viewport's framebuffer scale, clamped to the viewport. Fails, naming the window by its title, when it was not drawn this
	// frame ("the Game View window is not open"), is docked behind another tab, is in a viewport other than mainViewport
	// (captures read the editor window alone) or has no area. Call between the end of a frame's drawing and the next frame,
	// as the screenshot callback runs. Main thread only.
	[[nodiscard]] Result<ScreenshotRect> FindEditorWindowRect(std::string_view windowName, ImGuiViewport const* mainViewport);
}
