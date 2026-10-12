#include "Editor/Panels/GameViewPanel.h"

#include "Editor/EditorContext.h"
#include "Editor/GameInputRouter.h"

#include "Strada/Core/Window.h"
#include "Strada/ImGui/ImGuiRenderer.h"
#include "Strada/Project/Project.h"
#include "Strada/Renderer/Renderer.h"
#include "Strada/Scene/SceneRendering.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::array<GameViewResolutionInfo, 8> Resolutions = {{
			{GameViewResolution::Free, "Free Aspect"},
			{GameViewResolution::Aspect16x9, "16:9"},
			{GameViewResolution::Aspect16x10, "16:10"},
			{GameViewResolution::Aspect4x3, "4:3"},
			{GameViewResolution::Aspect21x9, "21:9"},
			{GameViewResolution::ProjectWindow, "Project Window"},
			{GameViewResolution::FullHD, "1920 x 1080"},
			{GameViewResolution::HD, "1280 x 720"},
		}};

		constexpr ImU32 LetterboxColor = IM_COL32(0, 0, 0, 255);
		constexpr ImU32 ActiveFrameColor = IM_COL32(61, 139, 253, 255);

		char const* GetResolutionName(GameViewResolution resolution)
		{
			for (GameViewResolutionInfo const& info : Resolutions)
			{
				if (info.Resolution == resolution)
				{
					return info.Name;
				}
			}
			return "";
		}

		glm::uvec2 ToPixels(glm::vec2 size)
		{
			return glm::uvec2(static_cast<uint32_t>(std::max(std::floor(size.x), 1.0f)),
			                  static_cast<uint32_t>(std::max(std::floor(size.y), 1.0f)));
		}

		// The largest rectangle with the aspect ratio of size that fits into available, centered.
		GameViewLayout Fit(glm::vec2 size, glm::vec2 available)
		{
			float const scale = std::min(available.x / size.x, available.y / size.y);
			GameViewLayout layout;
			layout.ImageSize = glm::max(glm::floor(size * scale), glm::vec2(1.0f));
			layout.ImageOffset = glm::floor((available - layout.ImageSize) * 0.5f);
			return layout;
		}
	}

	std::span<GameViewResolutionInfo const> GetGameViewResolutions()
	{
		return Resolutions;
	}

	GameViewLayout ComputeGameViewLayout(GameViewResolution resolution, glm::vec2 available, Project const* project)
	{
		available = glm::max(available, glm::vec2(1.0f));
		auto const aspect = [available](float width, float height)
		{
			GameViewLayout layout = Fit(glm::vec2(width, height), available);
			layout.RenderSize = ToPixels(layout.ImageSize);
			return layout;
		};
		auto const fixed = [available](glm::uvec2 size)
		{
			glm::vec2 const pixels(static_cast<float>(size.x), static_cast<float>(size.y));
			GameViewLayout layout;
			// Never enlarged: a fixed size that fits stays pixel-exact.
			if (pixels.x <= available.x && pixels.y <= available.y)
			{
				layout.ImageSize = pixels;
				layout.ImageOffset = glm::floor((available - pixels) * 0.5f);
			}
			else
			{
				layout = Fit(pixels, available);
			}
			layout.RenderSize = size;
			return layout;
		};

		switch (resolution)
		{
			case GameViewResolution::Free:
				break;
			case GameViewResolution::Aspect16x9:
				return aspect(16.0f, 9.0f);
			case GameViewResolution::Aspect16x10:
				return aspect(16.0f, 10.0f);
			case GameViewResolution::Aspect4x3:
				return aspect(4.0f, 3.0f);
			case GameViewResolution::Aspect21x9:
				return aspect(21.0f, 9.0f);
			case GameViewResolution::ProjectWindow:
			{
				glm::uvec2 size(1280, 720);
				if (project != nullptr)
				{
					size =
						glm::uvec2(std::max(project->GetSettings().Window.Width, 1u), std::max(project->GetSettings().Window.Height, 1u));
				}
				return fixed(size);
			}
			case GameViewResolution::FullHD:
				return fixed(glm::uvec2(1920, 1080));
			case GameViewResolution::HD:
				return fixed(glm::uvec2(1280, 720));
		}
		GameViewLayout layout;
		layout.RenderSize = ToPixels(available);
		layout.ImageSize = glm::vec2(static_cast<float>(layout.RenderSize.x), static_cast<float>(layout.RenderSize.y));
		return layout;
	}

	GameViewPanel::GameViewPanel(GameInputRouter& input)
		: m_Input(input)
	{
		if (Renderer::IsInitialized())
		{
			m_Renderer = CreateScope<SceneRenderer>();
		}
		m_Input.SetCoverageTest(
			[this](glm::vec2 position)
			{
				return IsCoveredAt(position);
			});
	}

	GameViewPanel::~GameViewPanel()
	{
		m_Input.SetCoverageTest({});
	}

	void GameViewPanel::OnImGuiRender(EditorContext& context, bool& open)
	{
		// The game took the input (it started playing, or a click on the view gave it): the view comes forward with it.
		if (m_Input.TakeFocusRequest())
		{
			ImGui::SetNextWindowFocus();
		}
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		// The view never scrolls: the wheel over it is the game's.
		bool const visible = ImGui::Begin("Game View", &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleVar();
		m_Window = ImGui::GetCurrentWindow();
		if (!visible)
		{
			// A hidden view (another tab in front, collapsed) gives the input back.
			m_Input.SetView({});
			ImGui::End();
			return;
		}
		DrawToolbar(context);

		ImVec2 const origin = ImGui::GetCursorScreenPos();
		glm::vec2 const available(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(ImGui::GetContentRegionAvail().y, 1.0f));
		GameViewLayout const layout = ComputeGameViewLayout(m_Resolution, available, context.GetProject());
		m_RenderSize = layout.RenderSize;
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(origin, ImVec2(origin.x + available.x, origin.y + available.y), LetterboxColor);

		Scene& scene = context.GetScene();
		// Scripts read the game's view size (Application.WindowWidth/Height, Camera.ScreenToWorldRay).
		scene.OnViewportResize(layout.RenderSize.x, layout.RenderSize.y);
		bool rendered = false;
		if (m_Renderer)
		{
			m_Renderer->SetViewportSize(layout.RenderSize.x, layout.RenderSize.y);
			rendered = RenderSceneFromPrimaryCamera(scene, *m_Renderer);
		}

		ImVec2 const imageMin(origin.x + layout.ImageOffset.x, origin.y + layout.ImageOffset.y);
		ImVec2 const imageMax(imageMin.x + layout.ImageSize.x, imageMin.y + layout.ImageSize.y);
		// The whole area is an item, so that ImGui focuses the view with the click that gives the game the input.
		ImGui::InvisibleButton("##GameImage", ImVec2(available.x, available.y));
		bool const hovered = ImGui::IsItemHovered();
		if (rendered)
		{
			drawList->AddImage(ImGuiRenderer::GetTextureID(m_Renderer->GetFinalImage()), imageMin, imageMax);
		}
		else
		{
			char const* const message = m_Renderer ? "No camera renders the game: give an entity a Camera component marked Primary."
			                                       : "Rendering is unavailable (no GPU).";
			ImVec2 const size = ImGui::CalcTextSize(message);
			drawList->AddText(ImVec2(origin.x + (available.x - size.x) * 0.5f, origin.y + (available.y - size.y) * 0.5f),
			                  ImGui::GetColorU32(ImGuiCol_TextDisabled), message);
		}

		GameInputRouter::View view;
		view.Playing = context.GetPlayState() == EditorPlayState::Play;
		view.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		view.Viewport = ImGui::GetWindowViewport()->ID;
		view.AreaMin = glm::vec2(origin.x, origin.y);
		view.AreaMax = view.AreaMin + available;
		view.ImageMin = glm::vec2(imageMin.x, imageMin.y);
		view.ImageSize = layout.ImageSize;
		view.RenderSize = layout.RenderSize;
		m_Input.SetView(view);

		// A cursor the game hides is hidden over its view alone: the editor's cursors show over the toolbar and every other
		// panel. (A locked cursor takes the windows instead: UpdateWindowCursors.)
		if (hovered && m_Input.GetCursorMode() == CursorMode::Hidden)
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);
		}
		if (m_Input.HasInput())
		{
			drawList->AddRect(ImVec2(origin.x + 1.0f, origin.y + 1.0f),
			                  ImVec2(origin.x + available.x - 1.0f, origin.y + available.y - 1.0f), ActiveFrameColor, 0.0f, 2.0f);
		}
		ImGui::End();
	}

	void GameViewPanel::UpdateWindowCursors()
	{
		bool const locked = m_Input.GetCursorMode() == CursorMode::Locked;
		if (!locked && !m_WindowCursorsLocked)
		{
			return;
		}
		// Applied every frame while locked (modes that did not change cost nothing): the windows of panels dragged out of the
		// editor come and go, and the game view can move to another window. Unlocking gives every window the normal cursor
		// back for ImGui to manage again.
		for (ImGuiViewport const* viewport : ImGui::GetPlatformIO().Viewports)
		{
			if (GLFWwindow* const window = static_cast<GLFWwindow*>(viewport->PlatformHandle))
			{
				Window::ApplyCursorMode(window, m_Input.GetWindowCursorMode(viewport->ID));
			}
		}
		m_WindowCursorsLocked = locked;
		ImGuiIO& io = ImGui::GetIO();
		if (locked)
		{
			io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		}
		else
		{
			io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
		}
	}

	bool GameViewPanel::IsCoveredAt(glm::vec2 position) const
	{
		// Clicks outside a modal dialog do nothing.
		if (m_Window == nullptr || m_Window->Viewport == nullptr || ImGui::GetTopMostPopupModal() != nullptr)
		{
			return true;
		}
		// The front-most window there among the windows of the view's viewport, as ImGui finds the hovered window; window
		// events arrive between frames, so the windows are as the last frame showed them. Anything but the view itself (a
		// popup, a menu, a floating panel; tooltips take no clicks) covers it.
		ImGuiContext const& imgui = *ImGui::GetCurrentContext();
		ImVec2 const point(position.x, position.y);
		for (int i = imgui.Windows.Size - 1; i >= 0; i--)
		{
			ImGuiWindow const* const window = imgui.Windows[i];
			bool const shown = window->Active && !window->Hidden && (window->Flags & ImGuiWindowFlags_NoMouseInputs) == 0;
			if (shown && window->Viewport == m_Window->Viewport && window->OuterRectClipped.Contains(point))
			{
				return window->RootWindow != m_Window;
			}
		}
		return true;
	}

	void GameViewPanel::DrawToolbar(EditorContext const& context)
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		ImGui::SetCursorPos(ImVec2(style.WindowPadding.x + 6.0f, ImGui::GetCursorPosY() + 4.0f));
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
		if (ImGui::BeginCombo("##Resolution", GetResolutionName(m_Resolution)))
		{
			for (GameViewResolutionInfo const& info : Resolutions)
			{
				std::string name = info.Name;
				if (info.Resolution == GameViewResolution::ProjectWindow && context.GetProject() != nullptr)
				{
					ProjectWindowSettings const& window = context.GetProject()->GetSettings().Window;
					name += " (" + std::to_string(window.Width) + " x " + std::to_string(window.Height) + ")";
				}
				if (ImGui::Selectable(name.c_str(), info.Resolution == m_Resolution))
				{
					m_Resolution = info.Resolution;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SetItemTooltip("The size the game renders at");
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		// The state of the previous frame: the image is laid out after the toolbar.
		ImGui::TextDisabled("%u x %u", m_RenderSize.x, m_RenderSize.y);
		ImGui::SameLine();
		if (m_Input.HasInput())
		{
			ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "The game has the input - Shift+Esc releases it");
		}
		else if (context.GetPlayState() == EditorPlayState::Play)
		{
			ImGui::TextDisabled("Click the view to give the game the input");
		}
		else
		{
			ImGui::TextDisabled("Preview through the primary camera");
		}
		ImGui::Dummy(ImVec2(0.0f, 2.0f));
	}
}
