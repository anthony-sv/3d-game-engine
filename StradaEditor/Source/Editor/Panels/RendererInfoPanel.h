#pragma once

#include <array>
#include <cstddef>

namespace Strada
{
	struct SceneRendererStatistics;

	// Renderer Info: the GPU and its driver, validation, frame timing (with a graph), the back buffer, and what the scene
	// view's and the game view's renderers drew in their last frames.
	class RendererInfoPanel
	{
	public:
		// sceneView, gameView: statistics of the views' last rendered frames; null when they have no renderer.
		void OnImGuiRender(bool& isOpen, SceneRendererStatistics const* sceneView, SceneRendererStatistics const* gameView);
		// The panel's Views section, in the current window: one row per statistic with the value of each view (a dash for a
		// view without a renderer). Reads nothing but its arguments, so it draws without an application or a GPU.
		static void DrawViews(SceneRendererStatistics const* sceneView, SceneRendererStatistics const* gameView);

	private:
		std::array<float, 240> m_FrameTimes = {};
		size_t m_FrameTimeOffset = 0;
	};
}
