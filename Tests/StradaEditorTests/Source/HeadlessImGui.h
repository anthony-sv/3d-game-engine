#pragma once

#include <imgui.h>
#include <imgui_impl_null.h>

#include <functional>

namespace Strada::Testing
{
	// A Dear ImGui context with the null platform and renderer backends: panels draw without a window or GPU, and tests
	// feed keyboard input between frames.
	class HeadlessImGui
	{
	public:
		HeadlessImGui()
		{
			m_Context = ImGui::CreateContext();
			ImGui::GetIO().IniFilename = nullptr;
			ImGui_ImplNull_Init();
		}

		~HeadlessImGui()
		{
			ImGui_ImplNull_Shutdown();
			ImGui::DestroyContext(m_Context);
		}

		HeadlessImGui(HeadlessImGui const&) = delete;
		HeadlessImGui& operator=(HeadlessImGui const&) = delete;

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
		ImGuiContext* m_Context = nullptr;
	};
}
