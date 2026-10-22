#pragma once

#include "Strada/Core/PlatformDetection.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Strada
{
	// What a graph window shows of one sub-graph: which one, and where its view is.
	struct VisualScriptViewState
	{
		// 0 is the event graph, else a function's ID.
		uint32_t Function = 0;
		// The canvas point at the centre of the view, in canvas units (the units of positions in documents).
		glm::vec2 Center{0.0f};
		// Screen pixels per canvas unit at a UI scale of 1.
		float Zoom = 1.0f;

		bool operator==(VisualScriptViewState const& other) const = default;
	};

	// The zoom levels a wheel notch steps between; every view's zoom lies within the first and the last.
	inline constexpr std::array<float, 16> VisualScriptZoomLevels = {0.10f, 0.15f, 0.20f, 0.25f, 0.33f, 0.40f, 0.50f, 0.60f,
	                                                                 0.70f, 0.80f, 0.90f, 1.00f, 1.25f, 1.50f, 1.75f, 2.00f};
	inline constexpr float VisualScriptMinZoom = VisualScriptZoomLevels.front();
	inline constexpr float VisualScriptMaxZoom = VisualScriptZoomLevels.back();

	// What the mouse wheel and trackpad scrolling do on the canvas (a per-user choice, so a trackpad navigates as well as a
	// wheel mouse).
	enum class VisualScriptScrollMode : uint8_t
	{
		// The wheel zooms to the cursor; Ctrl+wheel pans vertically, Shift+wheel and the horizontal wheel horizontally, and
		// a diagonal trackpad swipe pans.
		Zoom = 0,
		// Wheel and trackpad deltas pan in both axes; Ctrl+wheel (Cmd on macOS) zooms to the cursor.
		Pan
	};

	// The mode of users who never chose one: Pan on macOS, where trackpads are the usual device, else Zoom.
	constexpr VisualScriptScrollMode GetDefaultVisualScriptScrollMode()
	{
#if defined(ST_PLATFORM_MACOS)
		return VisualScriptScrollMode::Pan;
#else
		return VisualScriptScrollMode::Zoom;
#endif
	}
}
