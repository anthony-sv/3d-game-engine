#pragma once

#include <array>
#include <cstddef>

namespace Strada
{
	struct SceneRendererStatistics;

	// Frame timing, what the viewport's renderer drew and GPU information.
	class StatisticsPanel
	{
	public:
		// viewport: the statistics of the viewport's last rendered frame; null when it has no renderer.
		void OnImGuiRender(bool& isOpen, SceneRendererStatistics const* viewport);

	private:
		std::array<float, 120> m_FrameTimes = {};
		size_t m_FrameTimeOffset = 0;
	};
}
