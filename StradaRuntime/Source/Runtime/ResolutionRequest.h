#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace Strada
{
	// The drawing area a windowed run was asked for (--resolution). The player sizes the window before the first frame, but
	// the system may give it less (Windows keeps a window within about the desktop's size), and the new size arrives during
	// the resize on Windows and with later events elsewhere. The run judges the size once, in the first frame drawn at or
	// after SettleFrame, and a run whose drawing area is not the one asked for ends with an error, so that no capture or
	// profile is taken at a size nobody asked for (RuntimeLayer).
	class ResolutionRequest
	{
	public:
		// The frames a resize may take to arrive where it is asynchronous.
		static constexpr uint64_t SettleFrame = 2;

		ResolutionRequest() = default;
		explicit ResolutionRequest(std::optional<glm::uvec2> size)
			: m_Size(size)
		{
		}

		bool IsWanted() const { return m_Size.has_value(); }
		bool IsJudged() const { return m_Judged; }

		// Called in each update that draws, with the application's frame and the drawing area's size in pixels: the reason
		// the run must end, in the frame that judges the size and finds it wrong; empty otherwise.
		[[nodiscard]] std::string Judge(uint64_t frame, glm::uvec2 drawn)
		{
			if (!IsWanted() || m_Judged)
			{
				return {};
			}
			m_LastDrawn = drawn;
			if (frame < SettleFrame)
			{
				return {};
			}
			m_Judged = true;
			return drawn == *m_Size ? std::string() : Describe(drawn);
		}

		// At the end of a run that ended before judging the size: the reason it failed when the last frame drawn was not of
		// the size asked for; empty when it was, when nothing was drawn, when the size was judged or none was wanted.
		[[nodiscard]] std::string DescribeUnjudged() const
		{
			if (!IsWanted() || m_Judged || !m_LastDrawn || *m_LastDrawn == *m_Size)
			{
				return {};
			}
			return Describe(*m_LastDrawn);
		}

	private:
		std::string Describe(glm::uvec2 drawn) const
		{
			return "--resolution asked for " + std::to_string(m_Size->x) + "x" + std::to_string(m_Size->y) +
			       " pixels, but the window's drawing area is " + std::to_string(drawn.x) + "x" + std::to_string(drawn.y) +
			       " (the system keeps a window within the size it allows, on Windows about the desktop's)";
		}

		std::optional<glm::uvec2> m_Size;
		std::optional<glm::uvec2> m_LastDrawn;
		bool m_Judged = false;
	};
}
