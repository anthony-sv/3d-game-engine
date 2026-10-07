#pragma once

#include "Strada/Core/Log.h"

#include <array>
#include <string>
#include <vector>

namespace Strada
{
	// Shows engine, editor and script log entries with level and text filtering.
	class ConsolePanel
	{
	public:
		void OnImGuiRender(bool& isOpen);

	private:
		void FetchNewEntries();

		std::vector<LogEntry> m_Entries;
		uint64_t m_NextIndex = 0;
		std::array<bool, 5> m_ShowLevel = {true, true, true, true, true};
		std::array<char, 256> m_Filter = {};
		bool m_AutoScroll = true;
		bool m_ScrollToBottom = false;

		static constexpr size_t MaxEntries = 5000;
	};
}
