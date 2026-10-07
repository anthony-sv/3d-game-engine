#pragma once

#include <array>
#include <cstddef>

namespace Strada
{
	// Frame timing and GPU information.
	class StatisticsPanel
	{
	public:
		void OnImGuiRender(bool& isOpen);

	private:
		std::array<float, 120> m_FrameTimes = {};
		size_t m_FrameTimeOffset = 0;
	};
}
