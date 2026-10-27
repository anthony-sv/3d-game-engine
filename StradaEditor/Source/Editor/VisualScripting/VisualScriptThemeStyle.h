#pragma once

#include "Editor/VisualScripting/VisualScriptTheme.h"

#include <cstdint>

struct ImGuiStyle;

namespace Strada
{
	// The theme colours the canvas builds on, read from a live ImGui style (cheap: the renderer calls it every frame, so the
	// canvas follows the editor's theme): ImGuiCol_DockingEmptyBg, PopupBg (with its alpha forced to 1, since node bodies are
	// opaque), Border, Text, TextDisabled, FrameBg and its hovered and active forms, Button and CheckMark. Each is converted to
	// the IM_COL32 layout as ImGui converts colours (ImGui::ColorConvertFloat4ToU32). With the editor theme
	// (ImGuiLayer::ApplyTheme) the result equals VisualScriptThemeAnchors' defaults.
	[[nodiscard]] VisualScriptThemeAnchors MakeVisualScriptThemeAnchors(ImGuiStyle const& style);

	// The parts of the live ImGui style the canvas draws with: its theme anchors and a few metrics. The defaults are the editor
	// theme's at a UI scale of 1.
	struct VisualScriptCanvasStyle
	{
		VisualScriptThemeAnchors Theme;
		// ImGuiCol_ResizeGrip and ImGuiCol_ResizeGripHovered: a hovered comment's resize handles.
		uint32_t ResizeGrip = VisualScriptRgba(0x3D, 0x8B, 0xFD, 51);
		uint32_t ResizeGripHovered = VisualScriptRgba(0x3D, 0x8B, 0xFD, 153);
		// ImGuiStyle::CurveTessellationTol: wires are flattened as hit testing flattens them.
		float CurveTessellationTolerance = 1.25f;
		// ImGuiStyle::HoverDelayShort and HoverDelayNormal (seconds): pin tooltips and the wire label wait the short one.
		float HoverDelayShort = 0.15f;
		float HoverDelayNormal = 0.40f;
		// In pixels, as the style has them (the editor scales them by the UI scale): the live widget takes them × zoom.
		float FrameRounding = 4.0f;
		float GrabMinSize = 10.0f;
		// ImGuiStyle::FontScaleMain × FontScaleDpi: what ImGui multiplies a pushed font size by.
		float FontScale = 1.0f;
		// ImGui::GetFontSize(): the text size of pills and tooltips, which keep the window's size at every zoom.
		float FontSize = 15.0f;
	};

	// Reads the canvas's style from a live ImGui style, with fontSize the window's ImGui::GetFontSize().
	[[nodiscard]] VisualScriptCanvasStyle MakeVisualScriptCanvasStyle(ImGuiStyle const& style, float fontSize);
}
