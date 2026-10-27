#include "HeadlessImGui.h"

#include "Editor/VisualScripting/VisualScriptTheme.h"
#include "Editor/VisualScripting/VisualScriptThemeStyle.h"

#include "Strada/ImGui/ImGuiLayer.h"

#include <doctest/doctest.h>
#include <imgui.h>

using namespace Strada;

TEST_CASE("VisualScriptEditor: the canvas's theme anchors read from the editor theme equal their defaults")
{
	Testing::HeadlessImGui imgui;
	ImGuiLayer::ApplyTheme();
	ImGuiStyle const& style = ImGui::GetStyle();

	VisualScriptThemeAnchors const anchors = MakeVisualScriptThemeAnchors(style);
	VisualScriptThemeAnchors const defaults;
	CHECK(anchors.Canvas == defaults.Canvas);
	CHECK(anchors.Body == defaults.Body);
	CHECK(anchors.Border == defaults.Border);
	CHECK(anchors.Text == defaults.Text);
	CHECK(anchors.TextDisabled == defaults.TextDisabled);
	CHECK(anchors.Frame == defaults.Frame);
	CHECK(anchors.FrameHovered == defaults.FrameHovered);
	CHECK(anchors.FrameActive == defaults.FrameActive);
	CHECK(anchors.Button == defaults.Button);
	CHECK(anchors.Selection == defaults.Selection);
	CHECK(anchors == defaults);

	// The popup background is slightly translucent in the theme; node bodies are opaque.
	CHECK(style.Colors[ImGuiCol_PopupBg].w < 1.0f);
	CHECK((anchors.Body >> IM_COL32_A_SHIFT) == 255u);
}

TEST_CASE("VisualScriptEditor: the canvas's theme anchors follow the live style")
{
	Testing::HeadlessImGui imgui;
	ImGuiLayer::ApplyTheme();
	ImGuiStyle& style = ImGui::GetStyle();

	style.Colors[ImGuiCol_DockingEmptyBg] = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
	style.Colors[ImGuiCol_CheckMark] = ImVec4(0.0f, 0.0f, 1.0f, 0.5f);
	style.Colors[ImGuiCol_PopupBg] = ImVec4(0.0f, 1.0f, 0.0f, 0.25f);
	VisualScriptThemeAnchors const anchors = MakeVisualScriptThemeAnchors(style);
	CHECK(anchors.Canvas == VisualScriptRgba(255, 0, 0));
	CHECK(anchors.Selection == VisualScriptRgba(0, 0, 255, 128));
	CHECK(anchors.Body == VisualScriptRgba(0, 255, 0));
	CHECK(anchors.Text == VisualScriptThemeAnchors().Text);

	// ImGui's own default style is not the editor's: the anchors are read, never assumed.
	ImGui::StyleColorsDark(&style);
	CHECK(MakeVisualScriptThemeAnchors(style) != VisualScriptThemeAnchors());
}
