#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

struct GLFWwindow;
struct ImGuiWindow;

namespace Strada
{
	class EditorContext;
	class GameInputRouter;
	class Project;

	// What size the game view renders at.
	enum class GameViewResolution : uint8_t
	{
		// The panel's size.
		Free = 0,
		// The largest rectangle of an aspect ratio that fits the panel.
		Aspect16x9,
		Aspect16x10,
		Aspect4x3,
		Aspect21x9,
		// Fixed sizes, scaled down to fit the panel when it is smaller: the project's window size, Full HD, HD.
		ProjectWindow,
		FullHD,
		HD
	};

	struct GameViewResolutionInfo
	{
		GameViewResolution Resolution = GameViewResolution::Free;
		char const* Name = "";
	};

	// The resolutions in the order the game view lists them.
	std::span<GameViewResolutionInfo const> GetGameViewResolutions();

	// Where the game's image goes inside the panel and what size it is rendered at.
	struct GameViewLayout
	{
		// Pixels the game renders (and its scripts see as the window size).
		glm::uvec2 RenderSize = glm::uvec2(1);
		// The image's rectangle, relative to the panel's content origin; letterboxed when the aspect ratios differ.
		glm::vec2 ImageOffset = glm::vec2(0.0f);
		glm::vec2 ImageSize = glm::vec2(1.0f);
	};

	// Lays the game's image out in an area of the panel. project may be null (the project window size is then 1280x720).
	GameViewLayout ComputeGameViewLayout(GameViewResolution resolution, glm::vec2 available, Project const* project);

	// Shows the scene the way the game sees it: through its primary camera, at the chosen resolution, letterboxed. While
	// playing, the game view shares the window's input with the game through a GameInputRouter: it tells the router where
	// the game is shown each frame, takes the focus when the game takes the input, and frames itself while the game has it.
	// The scene view keeps showing the running scene through the editor camera.
	class GameViewPanel
	{
	public:
		// The router must outlive the panel.
		explicit GameViewPanel(GameInputRouter& input);
		~GameViewPanel();

		GameViewPanel(GameViewPanel const&) = delete;
		GameViewPanel& operator=(GameViewPanel const&) = delete;

		void OnImGuiRender(EditorContext& context, bool& open);

		GameViewResolution GetResolution() const { return m_Resolution; }
		void SetResolution(GameViewResolution resolution) { m_Resolution = resolution; }

		// The size the game renders at, as of the last frame the panel was drawn.
		glm::uvec2 GetRenderSize() const { return m_RenderSize; }
		// The OS window the panel was last drawn in: the main window, or its own when dragged out of it; null before that.
		GLFWwindow* GetPlatformWindow() const;
		// What the game view's renderer drew in its last frame; null without a GPU.
		SceneRendererStatistics const* GetRendererStatistics() const { return m_Renderer ? &m_Renderer->GetStatistics() : nullptr; }

	private:
		void DrawToolbar(EditorContext const& context);
		// Whether something covers the game view at a position (GameInputRouter::CoverageTest), from the windows of ImGui's
		// last frame.
		bool IsCoveredAt(glm::vec2 position) const;

		GameInputRouter& m_Input;
		Scope<SceneRenderer> m_Renderer;
		GameViewResolution m_Resolution = GameViewResolution::Free;
		glm::uvec2 m_RenderSize = glm::uvec2(1280, 720);
		// The panel's ImGui window once drawn (ImGui keeps windows for the context's lifetime).
		ImGuiWindow* m_Window = nullptr;
	};
}
