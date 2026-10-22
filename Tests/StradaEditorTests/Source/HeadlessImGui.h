#pragma once

#include "Strada/ImGui/ImGuiLayer.h"

#include <imgui.h>
#include <imgui_impl_null.h>
#include <imgui_internal.h>

#include <algorithm>
#include <functional>
#include <string>

namespace Strada::Testing
{
	// A Dear ImGui context with the null platform and renderer backends: panels draw without a window or GPU, and tests
	// feed keyboard input between frames. The clipboard lives in memory, so tests never touch the system's clipboard.
	class HeadlessImGui
	{
	public:
		HeadlessImGui()
		{
			m_Context = ImGui::CreateContext();
			ImGui::GetIO().IniFilename = nullptr;
			ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
			platform.Platform_ClipboardUserData = this;
			platform.Platform_GetClipboardTextFn = [](ImGuiContext* context) -> char const*
			{
				return static_cast<HeadlessImGui*>(context->PlatformIO.Platform_ClipboardUserData)->m_Clipboard.c_str();
			};
			platform.Platform_SetClipboardTextFn = [](ImGuiContext* context, char const* text)
			{
				static_cast<HeadlessImGui*>(context->PlatformIO.Platform_ClipboardUserData)->m_Clipboard = text != nullptr ? text : "";
			};
			ImGui_ImplNull_Init();
		}

		~HeadlessImGui()
		{
			ImGui_ImplNull_Shutdown();
			ImGui::DestroyContext(m_Context);
		}

		HeadlessImGui(HeadlessImGui const&) = delete;
		HeadlessImGui& operator=(HeadlessImGui const&) = delete;

		// Configures the context as the editor's ImGuiLayer::OnAttach does: keyboard navigation (docking too when asked),
		// Strada's theme and the editor's fonts, Roboto as the default font. Returns the fixed-width font (Cousine), which the
		// editor injects into its panels. The UI scale stays 1. With docking, call it before the first frame (ImGui asserts
		// otherwise).
		ImFont* UseEditorStyle(bool docking = false)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			if (docking)
			{
				io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
			}
			ImGuiLayer::ApplyTheme();
			return ImGuiLayer::AddEditorFonts();
		}

		void Frame(std::function<void()> const& draw) { Frame(draw, 1.0f / 60.0f); }

		// Draws a frame that comes the given number of seconds after the previous one (frames are 1/60 s apart otherwise).
		void Frame(std::function<void()> const& draw, float seconds)
		{
			ImGui_ImplNull_NewFrame();
			ImGui::GetIO().DeltaTime = seconds;
			ImGui::NewFrame();
			draw();
			ImGui::Render();
			ImGui_ImplNullRender_RenderDrawData(ImGui::GetDrawData());
		}

		void Type(char const* text) { ImGui::GetIO().AddInputCharactersUTF8(text); }

		// Drags with the left mouse button from one point to another (screen coordinates), a frame per step, and releases.
		void Drag(ImVec2 from, ImVec2 to, std::function<void()> const& draw)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddMousePosEvent(from.x, from.y);
			Frame(draw);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			Frame(draw);
			// Past the drag threshold, then onto the target long enough for it to accept the payload.
			io.AddMousePosEvent(from.x + 20.0f, from.y + 20.0f);
			Frame(draw);
			io.AddMousePosEvent(to.x, to.y);
			Frame(draw);
			Frame(draw);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			Frame(draw);
			Frame(draw);
		}

		// Drags with a mouse button along the straight line from one point to another, a frame at each of the given number
		// of evenly spaced steps (at least one; the last lands on the target), rests on the target for a frame (so that it
		// can accept a payload) and releases, then draws two frames. ImGui starts dragging only once the mouse has moved
		// io.MouseDragThreshold pixels from where it was pressed (6 by default), so shorter paths only click.
		void DragPath(ImVec2 from, ImVec2 to, std::function<void()> const& draw, ImGuiMouseButton button = ImGuiMouseButton_Left,
		              int steps = 4)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddMousePosEvent(from.x, from.y);
			Frame(draw);
			io.AddMouseButtonEvent(button, true);
			Frame(draw);
			int const count = std::max(steps, 1);
			for (int step = 1; step <= count; step++)
			{
				float const amount = static_cast<float>(step) / static_cast<float>(count);
				io.AddMousePosEvent(from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount);
				Frame(draw);
			}
			Frame(draw);
			io.AddMouseButtonEvent(button, false);
			Frame(draw);
			Frame(draw);
		}

		// Moves the mouse to a screen position for a frame, then turns the wheel by the given amount (x horizontal, y
		// vertical, in notches; positive y scrolls up) for another.
		void Wheel(ImVec2 position, ImVec2 delta, std::function<void()> const& draw)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddMousePosEvent(position.x, position.y);
			Frame(draw);
			io.AddMouseWheelEvent(delta.x, delta.y);
			Frame(draw);
		}

		void Press(ImGuiKey key)
		{
			ImGui::GetIO().AddKeyEvent(key, true);
			Frame([] {});
			ImGui::GetIO().AddKeyEvent(key, false);
		}

		// Presses and releases a key while the panels are drawn.
		void Press(ImGuiKey key, std::function<void()> const& draw)
		{
			ImGui::GetIO().AddKeyEvent(key, true);
			Frame(draw);
			ImGui::GetIO().AddKeyEvent(key, false);
			Frame(draw);
		}

		// Presses a key with modifiers (ImGuiMod_Ctrl | ImGuiKey_C) while the panels are drawn: the modifiers go down before
		// the key and come up after it, a frame with the chord held and a frame after. The chord ImGui sees is the one given,
		// also with io.ConfigMacOSXBehaviors (the default on macOS), under which ImGui swaps Ctrl and Super as the platform
		// reports them (Cmd is Ctrl there): ImGuiMod_Ctrl means the shortcut key of the platform, as in ImGui::Shortcut.
		void Chord(ImGuiKeyChord chord, std::function<void()> const& draw)
		{
			ImGuiIO& io = ImGui::GetIO();
			ImGuiKey const key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
			SetModifiers(chord, true);
			io.AddKeyEvent(key, true);
			Frame(draw);
			io.AddKeyEvent(key, false);
			SetModifiers(chord, false);
			Frame(draw);
		}

		// Moves the mouse to a screen position for a frame, then clicks a button there (count times: 2 is a double click), a
		// frame for each press and each release.
		void MouseClick(ImVec2 position, ImGuiMouseButton button, std::function<void()> const& draw, int count = 1)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddMousePosEvent(position.x, position.y);
			Frame(draw);
			for (int i = 0; i < count; i++)
			{
				io.AddMouseButtonEvent(button, true);
				Frame(draw);
				io.AddMouseButtonEvent(button, false);
				Frame(draw);
			}
		}

		// Left clicks (one or two times) at a screen position.
		void Click(ImVec2 position, std::function<void()> const& draw, int count = 1)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddMousePosEvent(position.x, position.y);
			Frame(draw);
			for (int i = 0; i < count; i++)
			{
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				Frame(draw);
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				Frame(draw);
			}
		}

	private:
		// Sends the modifiers of a chord as the platform backend reports them: with macOS behaviors, the key ImGui turns into
		// Ctrl is Super (Cmd), and the other way around (imgui.cpp, ImGuiIO::AddKeyEvent).
		static void SetModifiers(ImGuiKeyChord chord, bool down)
		{
			ImGuiIO& io = ImGui::GetIO();
			bool const swapped = io.ConfigMacOSXBehaviors;
			if ((chord & ImGuiMod_Ctrl) != 0)
			{
				io.AddKeyEvent(swapped ? ImGuiMod_Super : ImGuiMod_Ctrl, down);
			}
			if ((chord & ImGuiMod_Shift) != 0)
			{
				io.AddKeyEvent(ImGuiMod_Shift, down);
			}
			if ((chord & ImGuiMod_Alt) != 0)
			{
				io.AddKeyEvent(ImGuiMod_Alt, down);
			}
			if ((chord & ImGuiMod_Super) != 0)
			{
				io.AddKeyEvent(swapped ? ImGuiMod_Ctrl : ImGuiMod_Super, down);
			}
		}

		ImGuiContext* m_Context = nullptr;
		// What the context's clipboard holds (its Platform_ClipboardUserData is this object, which never moves).
		std::string m_Clipboard;
	};
}
