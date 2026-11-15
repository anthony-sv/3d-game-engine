#pragma once

#include "Strada/Core/Window.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <optional>
#include <string_view>

struct ImGuiContext;

namespace Strada
{
	// What a title bar shows of the window it is the title bar of (Window's state on the platforms with custom title bars).
	struct EditorTitleBarWindow
	{
		bool Maximized = false;
		bool Focused = true;
		// The button under the cursor and the one held down, which the window tracks (Window::GetHoveredTitleBarButton).
		TitleBarArea HoveredButton = TitleBarArea::Client;
		TitleBarArea PressedButton = TitleBarArea::Client;
		// The window's icon as an image of EditorTitleBar::GetIconSize pixels a side (EditorWindowIcon); without one the
		// bar starts with its menus.
		ImTextureID Icon = ImTextureID_Invalid;
	};

	// The editor's main menu bar (ImGui's, the main viewport's "##MainMenuBar"), which is also the title bar of a window
	// whose title bar the application draws (Window::HasCustomTitleBar): then it is as high as a title bar and shows the
	// window's icon before the menus and, after them, the window's title, the status text and the buttons that minimize,
	// maximize or restore, and close the window. The window presses the buttons and drags itself by the bar's other parts,
	// the icon among them: the bar only draws, and tells the window what is where (FindArea). Without such a window it is
	// the plain menu bar with the status text at its right.
	//
	//     if (titleBar.Begin(window))
	//     {
	//         if (ImGui::BeginMenu("File")) { ...; ImGui::EndMenu(); }
	//         titleBar.End(title, status);
	//     }
	//
	// Main thread only.
	class EditorTitleBar
	{
	public:
		// A rectangle of the bar, in pixels from the main viewport's top-left corner (the window's client area).
		struct Rect
		{
			ImVec2 Min = ImVec2(0.0f, 0.0f);
			ImVec2 Max = ImVec2(0.0f, 0.0f);

			bool Contains(ImVec2 point) const { return point.x >= Min.x && point.y >= Min.y && point.x < Max.x && point.y < Max.y; }
			bool IsEmpty() const { return Max.x <= Min.x || Max.y <= Min.y; }
		};

		// Where the parts of the bar were in the frame drawn last. A part that was not drawn is empty: the icon and the
		// buttons without a window, the icon without an image of it, the title and the status when they did not fit.
		struct Layout
		{
			Rect Bar;
			Rect Icon;
			// From the first menu's left to the last one's right.
			Rect Menus;
			Rect Title;
			Rect Status;
			Rect Minimize;
			Rect Maximize;
			Rect Close;
		};

		// The bar's height as a title bar, at the current font and style. Inside an ImGui frame.
		static float GetHeight();
		// The side of the icon a title bar shows, in whole pixels, at the current font and style. Inside an ImGui frame.
		static float GetIconSize();

		// Begins the bar: as the title bar of the given window, or as the plain menu bar without one. Its menus follow, as
		// in any menu bar (ImGui::BeginMenu). False when the bar is not drawn; then nothing follows.
		bool Begin(std::optional<EditorTitleBarWindow> const& window);
		// Ends the bar after its menus. The title is shown only by a title bar.
		void End(std::string_view title, std::string_view status);

		// What the bar of the frame drawn last has at a position of the window's client area: the buttons, Caption for the
		// parts that drag the window (the icon too), Client for the menus, for everything outside the bar, where another ImGui window lies
		// over the bar, and while a popup is open (the click closes it, as a click on a title bar closes an open menu).
		// Always Client without a title bar window. Reads the ImGui context the bar was drawn in; also outside a frame.
		TitleBarArea FindArea(glm::ivec2 position) const;

		Layout const& GetLayout() const { return m_Layout; }

	private:
		void DrawTitle(std::string_view title, float left, float right);
		void DrawButtons() const;

		std::optional<EditorTitleBarWindow> m_Window;
		float m_MenusStart = 0.0f;
		Layout m_Layout;

		// The frame the layout is of: its context, its number, the bar's ImGui window and the main viewport's position.
		ImGuiContext* m_Context = nullptr;
		int m_Frame = -1;
		ImGuiID m_BarID = 0;
		ImVec2 m_Origin = ImVec2(0.0f, 0.0f);
	};
}
