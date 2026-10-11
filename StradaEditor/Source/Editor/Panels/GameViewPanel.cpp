#include "Editor/Panels/GameViewPanel.h"

#include "Editor/EditorContext.h"

#include "Strada/ImGui/ImGuiRenderer.h"
#include "Strada/Project/Project.h"
#include "Strada/Renderer/Renderer.h"
#include "Strada/Scene/SceneRendering.h"

#include <imgui.h>

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

	GameViewPanel::GameViewPanel()
	{
		if (Renderer::IsInitialized())
		{
			m_Renderer = CreateScope<SceneRenderer>();
		}
	}

	void GameViewPanel::OnImGuiRender(EditorContext& context, bool& open)
	{
		if (std::exchange(m_FocusRequested, false))
		{
			ImGui::SetNextWindowFocus();
			m_InputReleased = false;
		}
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		bool const visible = ImGui::Begin("Game View", &open);
		ImGui::PopStyleVar();
		if (!visible)
		{
			m_GameInputActive = false;
			ImGui::End();
			return;
		}
		DrawToolbar(context);
		if (!m_Renderer)
		{
			ImGui::TextDisabled("Rendering is unavailable (no GPU).");
			ImGui::End();
			return;
		}

		ImVec2 const origin = ImGui::GetCursorScreenPos();
		glm::vec2 const available(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(ImGui::GetContentRegionAvail().y, 1.0f));
		GameViewLayout const layout = ComputeGameViewLayout(m_Resolution, available, context.GetProject());
		m_RenderSize = layout.RenderSize;
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(origin, ImVec2(origin.x + available.x, origin.y + available.y), LetterboxColor);

		Scene& scene = context.GetScene();
		m_Renderer->SetViewportSize(layout.RenderSize.x, layout.RenderSize.y);
		// Scripts read the game's view size (Application.WindowWidth/Height, Camera.ScreenToWorldRay).
		scene.OnViewportResize(layout.RenderSize.x, layout.RenderSize.y);
		bool const rendered = RenderSceneFromPrimaryCamera(scene, *m_Renderer);

		ImVec2 const imageMin(origin.x + layout.ImageOffset.x, origin.y + layout.ImageOffset.y);
		ImVec2 const imageMax(imageMin.x + layout.ImageSize.x, imageMin.y + layout.ImageSize.y);
		// The whole area takes clicks (which focus the panel and give the game its input back).
		ImGui::InvisibleButton("##GameImage", ImVec2(available.x, available.y));
		bool const clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
		if (rendered)
		{
			drawList->AddImage(ImGuiRenderer::GetTextureID(m_Renderer->GetFinalImage()), imageMin, imageMax);
		}
		else
		{
			char const* const message = "No camera renders the game: give an entity a Camera component marked Primary.";
			ImVec2 const size = ImGui::CalcTextSize(message);
			drawList->AddText(ImVec2(origin.x + (available.x - size.x) * 0.5f, origin.y + (available.y - size.y) * 0.5f),
			                  ImGui::GetColorU32(ImGuiCol_TextDisabled), message);
		}

		bool const playing = context.GetPlayState() == EditorPlayState::Play;
		bool const focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (!playing || !focused || clicked)
		{
			m_InputReleased = false;
		}
		if (playing && focused && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			m_InputReleased = true;
		}
		m_GameInputActive = playing && focused && !m_InputReleased;
		ImVec2 const mouse = ImGui::GetMousePos();
		m_GameMousePosition = glm::vec2((mouse.x - imageMin.x) * static_cast<float>(layout.RenderSize.x) / layout.ImageSize.x,
		                                (mouse.y - imageMin.y) * static_cast<float>(layout.RenderSize.y) / layout.ImageSize.y);

		if (m_GameInputActive)
		{
			drawList->AddRect(ImVec2(origin.x + 1.0f, origin.y + 1.0f),
			                  ImVec2(origin.x + available.x - 1.0f, origin.y + available.y - 1.0f), ActiveFrameColor, 0.0f, 2.0f);
		}
		ImGui::End();
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
		if (m_GameInputActive)
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
