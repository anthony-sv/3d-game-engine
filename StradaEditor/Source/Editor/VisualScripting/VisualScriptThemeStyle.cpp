#include "Editor/VisualScripting/VisualScriptThemeStyle.h"

#include <imgui.h>

namespace Strada
{
	// The theme's colours are packed without imgui.h (VisualScriptRgba): they must use ImGui's own layout, red in the low byte,
	// which imconfig.h could change with IMGUI_USE_BGRA_PACKED_COLOR (imgui.h, IM_COL32_R_SHIFT).
	static_assert(VisualScriptRgba(1, 2, 3, 4) == IM_COL32(1, 2, 3, 4));

	VisualScriptThemeAnchors MakeVisualScriptThemeAnchors(ImGuiStyle const& style)
	{
		auto const read = [&style](ImGuiCol color)
		{
			return static_cast<uint32_t>(ImGui::ColorConvertFloat4ToU32(style.Colors[color]));
		};
		VisualScriptThemeAnchors anchors;
		anchors.Canvas = read(ImGuiCol_DockingEmptyBg);
		anchors.Body = read(ImGuiCol_PopupBg) | IM_COL32_A_MASK;
		anchors.Border = read(ImGuiCol_Border);
		anchors.Text = read(ImGuiCol_Text);
		anchors.TextDisabled = read(ImGuiCol_TextDisabled);
		anchors.Frame = read(ImGuiCol_FrameBg);
		anchors.FrameHovered = read(ImGuiCol_FrameBgHovered);
		anchors.FrameActive = read(ImGuiCol_FrameBgActive);
		anchors.Button = read(ImGuiCol_Button);
		anchors.Selection = read(ImGuiCol_CheckMark);
		return anchors;
	}

	VisualScriptCanvasStyle MakeVisualScriptCanvasStyle(ImGuiStyle const& style, float fontSize)
	{
		VisualScriptCanvasStyle canvas;
		canvas.Theme = MakeVisualScriptThemeAnchors(style);
		canvas.ResizeGrip = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_ResizeGrip]);
		canvas.ResizeGripHovered = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_ResizeGripHovered]);
		canvas.CurveTessellationTolerance = style.CurveTessellationTol;
		canvas.HoverDelayShort = style.HoverDelayShort;
		canvas.HoverDelayNormal = style.HoverDelayNormal;
		canvas.FrameRounding = style.FrameRounding;
		canvas.GrabMinSize = style.GrabMinSize;
		canvas.FontScale = style.FontScaleMain * style.FontScaleDpi;
		canvas.FontSize = fontSize;
		return canvas;
	}
}
