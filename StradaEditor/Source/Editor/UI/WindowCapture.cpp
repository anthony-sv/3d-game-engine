#include "Editor/UI/WindowCapture.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace Strada
{
	namespace
	{
		// The title an ImGui window name shows: the part before "##" ("Player###VisualScript42" is "Player").
		std::string_view GetTitle(std::string_view name)
		{
			return name.substr(0, name.find("##"));
		}

		// A coordinate of the viewport's framebuffer, clamped to it.
		uint32_t ToPixel(float value, float limit)
		{
			return static_cast<uint32_t>(std::clamp(value, 0.0f, std::max(limit, 0.0f)));
		}
	}

	Result<ScreenshotRect> FindEditorWindowRect(std::string_view windowName, ImGuiViewport const* mainViewport)
	{
		std::string const name(windowName);
		ImGuiWindow const* const window = ImGui::FindWindowByName(name.c_str());
		// Active: Begin ran for it this frame (ImGui clears it at the next NewFrame, imgui.cpp:5979).
		if (window == nullptr || !window->Active || window->Viewport == nullptr || mainViewport == nullptr)
		{
			return MakeError("the {} window is not open", GetTitle(windowName));
		}
		std::string_view const title = GetTitle(window->Name);
		if (window->DockIsActive && !window->DockTabIsVisible)
		{
			return MakeError("the {} window is behind another tab", title);
		}
		if (window->Viewport->ID != mainViewport->ID)
		{
			return MakeError("the {} window is in a window of its own: captures read the editor window", title);
		}
		// As ImGui gives the draw data its scale (imgui.cpp:6180): the viewport's, else the display's.
		ImVec2 const scale =
			mainViewport->FramebufferScale.x != 0.0f ? mainViewport->FramebufferScale : ImGui::GetIO().DisplayFramebufferScale;
		float const limitX = mainViewport->Size.x * scale.x;
		float const limitY = mainViewport->Size.y * scale.y;
		uint32_t const minX = ToPixel(std::floor((window->Pos.x - mainViewport->Pos.x) * scale.x), limitX);
		uint32_t const minY = ToPixel(std::floor((window->Pos.y - mainViewport->Pos.y) * scale.y), limitY);
		uint32_t const maxX = ToPixel(std::ceil((window->Pos.x + window->Size.x - mainViewport->Pos.x) * scale.x), limitX);
		uint32_t const maxY = ToPixel(std::ceil((window->Pos.y + window->Size.y - mainViewport->Pos.y) * scale.y), limitY);
		if (maxX <= minX || maxY <= minY)
		{
			return MakeError("the {} window has no area", title);
		}
		return ScreenshotRect{minX, minY, maxX - minX, maxY - minY};
	}
}
