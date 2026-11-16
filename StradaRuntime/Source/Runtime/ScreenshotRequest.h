#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace Strada
{
	// The screenshot a windowed run was asked for (--screenshot). The player asks the application for it once, in the first
	// update at or after its frame whose window image can be captured: an application frame may run no update (a minimized
	// window's) or acquire no image, so a request tied to the exact frame could be passed by. A run that wanted a screenshot
	// and saved none ends with an error (RuntimeLayer).
	class ScreenshotRequest
	{
	public:
		ScreenshotRequest() = default;
		ScreenshotRequest(std::filesystem::path path, uint64_t frame)
			: m_Path(std::move(path)),
			  m_Frame(frame)
		{
		}

		bool IsWanted() const { return !m_Path.empty(); }
		std::filesystem::path const& GetPath() const { return m_Path; }
		uint64_t GetFrame() const { return m_Frame; }
		bool IsRequested() const { return m_Requested; }
		bool IsSaved() const { return m_Saved; }

		// Called in each update with the application's frame and whether that frame's window image can be captured: true
		// exactly once, in the first such update at or after the requested frame.
		bool TakeRequest(uint64_t frame, bool capturable)
		{
			if (!IsWanted() || m_Requested || frame < m_Frame || !capturable)
			{
				return false;
			}
			m_Requested = true;
			return true;
		}

		// How the capture ended: saved, or why not.
		void SetSaved()
		{
			m_Saved = true;
			m_Failure.clear();
		}
		void SetFailed(std::string reason)
		{
			m_Saved = false;
			m_Failure = std::move(reason);
		}

		// Why a wanted screenshot was not saved, for the run's error line; empty when it was saved or none was wanted.
		std::string DescribeFailure() const
		{
			if (!IsWanted() || m_Saved)
			{
				return {};
			}
			if (!m_Failure.empty())
			{
				return m_Failure;
			}
			if (m_Requested)
			{
				return "the run ended before the capture was made";
			}
			return "the run ended before a frame at or after frame " + std::to_string(m_Frame) +
			       " was drawn (the window was minimized, or the game quit first)";
		}

	private:
		std::filesystem::path m_Path;
		uint64_t m_Frame = 0;
		bool m_Requested = false;
		bool m_Saved = false;
		std::string m_Failure;
	};
}
