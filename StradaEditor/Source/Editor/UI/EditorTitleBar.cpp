#include "Editor/UI/EditorTitleBar.h"

#include "Editor/UI/EditorUI.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace Strada
{
	namespace
	{
		// A title bar is this many framed items high, and its buttons and their glyphs have the proportions of Windows' own:
		// 46 by 32 with glyphs of 10, at the editor's font and a display scale of 1.
		constexpr float HeightInFrames = 1.4f;
		constexpr float ButtonAspect = 46.0f / 32.0f;
		constexpr float GlyphShare = 10.0f / 32.0f;
		// The window's icon has the size of Windows' small icons in such a bar: 16 in 32.
		constexpr float IconShare = 16.0f / 32.0f;
		// The title gives way below this many font sizes of room.
		constexpr float MinTitleWidthInFonts = 4.0f;
		// The red of a close button under the cursor, as Windows draws it, and held down.
		constexpr ImU32 CloseHoveredColor = IM_COL32(0xC4, 0x2B, 0x1C, 0xFF);
		constexpr ImU32 ClosePressedColor = IM_COL32(0xC4, 0x2B, 0x1C, 0xCC);
		// How strongly the text color tints the other buttons under the cursor, and held down.
		constexpr float HoveredTint = 0.10f;
		constexpr float PressedTint = 0.06f;

		EditorTitleBar::Rect ToRect(ImRect const& rect, ImVec2 origin)
		{
			return EditorTitleBar::Rect{rect.Min - origin, rect.Max - origin};
		}

		ImRect ToScreen(EditorTitleBar::Rect const& rect, ImVec2 origin)
		{
			return ImRect(rect.Min + origin, rect.Max + origin);
		}

		// The lines of a button's glyph in a square of the given size around a center, on whole pixels.
		void DrawGlyph(ImDrawList& drawList, TitleBarArea button, bool maximized, ImVec2 center, float size, float thickness, ImU32 color)
		{
			// Lines of an odd thickness are sharp through the middle of a pixel.
			float const offset = std::fmod(thickness, 2.0f) >= 1.0f ? 0.5f : 0.0f;
			ImVec2 const min(std::floor(center.x - size * 0.5f) + offset, std::floor(center.y - size * 0.5f) + offset);
			ImVec2 const max(min.x + size, min.y + size);
			switch (button)
			{
				case TitleBarArea::Minimize:
				{
					float const y = std::floor(center.y) + offset;
					drawList.AddLine(ImVec2(min.x, y), ImVec2(max.x, y), color, thickness);
					break;
				}
				case TitleBarArea::Maximize:
					if (!maximized)
					{
						drawList.AddRect(min, max, color, 0.0f, thickness);
						break;
					}
					{
						// Restore: a window in front of another, a fifth of the glyph up and to the right.
						float const step = std::round(size * 0.2f);
						drawList.AddRect(ImVec2(min.x, min.y + step), ImVec2(max.x - step, max.y), color, 0.0f, thickness);
						ImVec2 const back[] = {ImVec2(min.x + step, min.y + step), ImVec2(min.x + step, min.y), ImVec2(max.x, min.y),
						                       ImVec2(max.x, max.y - step), ImVec2(max.x - step, max.y - step)};
						drawList.AddPolyline(back, IM_ARRAYSIZE(back), color, thickness);
					}
					break;
				case TitleBarArea::Close:
					drawList.AddLine(min, max, color, thickness);
					drawList.AddLine(ImVec2(min.x, max.y), ImVec2(max.x, min.y), color, thickness);
					break;
				case TitleBarArea::Client:
				case TitleBarArea::Caption:
					break;
			}
		}
	}

	float EditorTitleBar::GetHeight()
	{
		float const font = ImGui::GetFontSize();
		// The same whole number of pixels above and below the text keeps the text sharp.
		float const padding = std::round((ImGui::GetFrameHeight() * HeightInFrames - font) * 0.5f);
		return font + std::max(padding, ImGui::GetStyle().FramePadding.y) * 2.0f;
	}

	float EditorTitleBar::GetIconSize()
	{
		return std::max(std::round(GetHeight() * IconShare), 1.0f);
	}

	bool EditorTitleBar::Begin(std::optional<EditorTitleBarWindow> const& window)
	{
		m_Window = window;
		m_Layout = {};
		m_Context = ImGui::GetCurrentContext();
		m_Frame = ImGui::GetFrameCount();
		m_BarID = 0;

		// A menu bar is as high as a framed item and lays its line out as that item's text, once, when it begins: with this
		// frame padding it is a title bar's height, with the names of its menus and the status in its middle. The menus open
		// at its lower edge whatever its height, and what they hold has the style's own padding again.
		if (window)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
			                    ImVec2(ImGui::GetStyle().FramePadding.x, (GetHeight() - ImGui::GetFontSize()) * 0.5f));
		}
		bool const open = ImGui::BeginMainMenuBar();
		if (window)
		{
			ImGui::PopStyleVar();
		}
		if (!open)
		{
			return false;
		}
		m_BarID = ImGui::GetCurrentWindow()->ID;
		m_Origin = ImGui::GetMainViewport()->Pos;
		if (window && window->Icon != ImTextureID_Invalid)
		{
			// The icon in the middle of a square at the bar's left end, on whole pixels as its image is made for; the menus
			// start where the square ends.
			ImRect const bar = ImGui::GetCurrentWindow()->Rect();
			float const side = GetIconSize();
			float const margin = std::floor((bar.GetHeight() - side) * 0.5f);
			ImVec2 const min(bar.Min.x + margin, bar.Min.y + margin);
			ImVec2 const max(min.x + side, min.y + side);
			ImDrawList& drawList = *ImGui::GetWindowDrawList();
			// The menu bar's clip rectangle starts after the window's border.
			drawList.PushClipRect(bar.Min, bar.Max, false);
			drawList.AddImage(ImTextureRef(window->Icon), min, max);
			drawList.PopClipRect();
			m_Layout.Icon = ToRect(ImRect(min, max), m_Origin);
			ImGui::SetCursorScreenPos(ImVec2(bar.Min.x + bar.GetHeight(), ImGui::GetCursorScreenPos().y));
		}
		m_MenusStart = ImGui::GetCursorScreenPos().x;
		return true;
	}

	void EditorTitleBar::End(std::string_view title, std::string_view status)
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		ImRect const bar = ImGui::GetCurrentWindow()->Rect();
		float const menusEnd = ImGui::GetCursorScreenPos().x;
		m_Layout.Bar = ToRect(bar, m_Origin);
		m_Layout.Menus = ToRect(ImRect(m_MenusStart, bar.Min.y, std::max(menusEnd, m_MenusStart), bar.Max.y), m_Origin);

		// From the right: the buttons, the status, and the title in what is left.
		float right = bar.Max.x;
		if (m_Window)
		{
			float const width = std::round(bar.GetHeight() * ButtonAspect);
			Rect* const buttons[] = {&m_Layout.Close, &m_Layout.Maximize, &m_Layout.Minimize};
			for (Rect* button : buttons)
			{
				*button = ToRect(ImRect(right - width, bar.Min.y, right, bar.Max.y), m_Origin);
				right -= width;
			}
		}

		float const statusWidth = ImGui::CalcTextSize(status.data(), status.data() + status.size()).x;
		float const statusStart = right - statusWidth - style.ItemSpacing.x * 2.0f;
		// The plain menu bar shows its status wherever it lands, as it always has; a title bar leaves out what does not fit.
		bool const statusShown = !status.empty() && (!m_Window || statusStart >= menusEnd + style.ItemSpacing.x);
		if (statusShown)
		{
			ImGui::SameLine(statusStart - bar.Min.x);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextUnformatted(status.data(), status.data() + status.size());
			ImGui::PopStyleColor();
			m_Layout.Status = ToRect(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), m_Origin);
		}
		if (m_Window)
		{
			DrawTitle(title, menusEnd + style.ItemSpacing.x * 2.0f, (statusShown ? statusStart : right) - style.ItemSpacing.x * 2.0f);
			DrawButtons();
		}

		ImGui::EndMainMenuBar();
	}

	void EditorTitleBar::DrawTitle(std::string_view title, float left, float right)
	{
		float const room = right - left;
		if (title.empty() || room < ImGui::GetFontSize() * MinTitleWidthInFonts)
		{
			return;
		}
		std::string const shown = UI::FitText(title, room);
		ImVec2 const size = ImGui::CalcTextSize(shown.c_str(), shown.c_str() + shown.size());
		if (size.x > room)
		{
			return;
		}
		// In the middle of the bar, or as close to it as the menus and the status leave room.
		ImRect const bar = ToScreen(m_Layout.Bar, m_Origin);
		float const x = std::floor(std::clamp(bar.GetCenter().x - size.x * 0.5f, left, right - size.x));
		float const y = std::floor(bar.Min.y + (bar.GetHeight() - size.y) * 0.5f);
		ImU32 const color = ImGui::GetColorU32(m_Window->Focused ? ImGuiCol_Text : ImGuiCol_TextDisabled);
		ImGui::GetWindowDrawList()->AddText(ImVec2(x, y), color, shown.c_str(), shown.c_str() + shown.size());
		m_Layout.Title = ToRect(ImRect(x, y, x + size.x, y + size.y), m_Origin);
	}

	void EditorTitleBar::DrawButtons() const
	{
		ImRect const bar = ToScreen(m_Layout.Bar, m_Origin);
		ImDrawList& drawList = *ImGui::GetWindowDrawList();
		// The menu bar's clip rectangle ends before the window's border; the buttons reach the corner.
		drawList.PushClipRect(bar.Min, bar.Max, false);
		float const glyph = std::max(std::round(bar.GetHeight() * GlyphShare), 6.0f);
		float const thickness = std::max(std::round(glyph / 10.0f), 1.0f);
		ImVec4 const text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
		struct Button
		{
			TitleBarArea Area;
			Rect const& Bounds;
		};
		Button const buttons[] = {{TitleBarArea::Minimize, m_Layout.Minimize},
		                          {TitleBarArea::Maximize, m_Layout.Maximize},
		                          {TitleBarArea::Close, m_Layout.Close}};
		for (Button const& button : buttons)
		{
			ImRect const bounds = ToScreen(button.Bounds, m_Origin);
			bool const pressed = m_Window->PressedButton == button.Area;
			bool const hovered = m_Window->HoveredButton == button.Area;
			bool const close = button.Area == TitleBarArea::Close;
			if (pressed || hovered)
			{
				ImU32 const tint = ImGui::GetColorU32(ImVec4(text.x, text.y, text.z, pressed ? PressedTint : HoveredTint));
				drawList.AddRectFilled(bounds.Min, bounds.Max, close ? (pressed ? ClosePressedColor : CloseHoveredColor) : tint);
			}
			ImU32 const color = close && (pressed || hovered)
			                        ? IM_COL32_WHITE
			                        : ImGui::GetColorU32(m_Window->Focused || hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			DrawGlyph(drawList, button.Area, m_Window->Maximized, bounds.GetCenter(), glyph, thickness, color);
		}
		drawList.PopClipRect();
	}

	TitleBarArea EditorTitleBar::FindArea(glm::ivec2 position) const
	{
		ImGuiContext* const context = ImGui::GetCurrentContext();
		ImVec2 const point(static_cast<float>(position.x), static_cast<float>(position.y));
		if (!m_Window || context == nullptr || context != m_Context || m_BarID == 0 || !m_Layout.Bar.Contains(point))
		{
			return TitleBarArea::Client;
		}
		// The layout is of the frame drawn last: the one that ended or, while a frame is being drawn, that one (the bar is
		// drawn already) or the one before it.
		int const age = ImGui::GetFrameCount() - m_Frame;
		if (age != 0 && !(age == 1 && context->WithinFrameScope))
		{
			return TitleBarArea::Client;
		}
		// Another window over the bar (a floating panel, a dialog) has what it covers.
		ImGuiWindow* hovered = nullptr;
		ImGui::FindHoveredWindowEx(point + m_Origin, false, &hovered, nullptr);
		if (hovered == nullptr || hovered->ID != m_BarID)
		{
			return TitleBarArea::Client;
		}
		if (m_Layout.Close.Contains(point))
		{
			return TitleBarArea::Close;
		}
		if (m_Layout.Maximize.Contains(point))
		{
			return TitleBarArea::Maximize;
		}
		if (m_Layout.Minimize.Contains(point))
		{
			return TitleBarArea::Minimize;
		}
		if (m_Layout.Menus.Contains(point) || context->OpenPopupStack.Size > 0)
		{
			return TitleBarArea::Client;
		}
		return TitleBarArea::Caption;
	}
}
