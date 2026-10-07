#include "Editor/Panels/ConsolePanel.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace Strada
{
	namespace
	{
		ImVec4 GetLevelColor(LogLevel level)
		{
			switch (level)
			{
				case LogLevel::Trace:
					return {0.60f, 0.60f, 0.60f, 1.0f};
				case LogLevel::Info:
					return {0.85f, 0.85f, 0.85f, 1.0f};
				case LogLevel::Warn:
					return {0.95f, 0.80f, 0.30f, 1.0f};
				case LogLevel::Error:
					return {0.95f, 0.40f, 0.40f, 1.0f};
				case LogLevel::Critical:
					return {1.00f, 0.25f, 0.55f, 1.0f};
			}
			return {1.0f, 1.0f, 1.0f, 1.0f};
		}

		bool ContainsIgnoreCase(std::string_view text, std::string_view pattern)
		{
			if (pattern.empty())
			{
				return true;
			}
			auto const it =
				std::search(text.begin(), text.end(), pattern.begin(), pattern.end(),
			                [](char a, char b)
			                {
								return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
							});
			return it != text.end();
		}
	}

	void ConsolePanel::FetchNewEntries()
	{
		std::vector<LogEntry> newEntries = Log::GetEntries(m_NextIndex);
		if (newEntries.empty())
		{
			return;
		}

		m_NextIndex = newEntries.back().Index + 1;
		m_Entries.insert(m_Entries.end(), std::make_move_iterator(newEntries.begin()), std::make_move_iterator(newEntries.end()));
		if (m_Entries.size() > MaxEntries)
		{
			m_Entries.erase(m_Entries.begin(), m_Entries.begin() + static_cast<std::ptrdiff_t>(m_Entries.size() - MaxEntries));
		}
		m_ScrollToBottom = m_AutoScroll;
	}

	void ConsolePanel::OnImGuiRender(bool& isOpen)
	{
		FetchNewEntries();

		if (!ImGui::Begin("Console", &isOpen))
		{
			ImGui::End();
			return;
		}

		if (ImGui::Button("Clear"))
		{
			m_Entries.clear();
		}
		ImGui::SameLine();
		ImGui::Checkbox("Auto-scroll", &m_AutoScroll);

		constexpr std::array<LogLevel, 5> Levels = {LogLevel::Trace, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Critical};
		for (LogLevel const level : Levels)
		{
			ImGui::SameLine();
			ImGui::Checkbox(Log::LevelToString(level), &m_ShowLevel[static_cast<size_t>(level)]);
		}

		ImGui::SameLine();
		ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x));
		ImGui::InputTextWithHint("##Filter", "Filter", m_Filter.data(), m_Filter.size());
		ImGui::Separator();

		std::string_view const filter(m_Filter.data());
		if (ImGui::BeginChild("Entries", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
		{
			for (LogEntry const& entry : m_Entries)
			{
				if (!m_ShowLevel[static_cast<size_t>(entry.Severity)] || !ContainsIgnoreCase(entry.Message, filter))
				{
					continue;
				}
				ImGui::PushStyleColor(ImGuiCol_Text, GetLevelColor(entry.Severity));
				ImGui::TextUnformatted(entry.Logger.c_str());
				ImGui::SameLine();
				ImGui::TextUnformatted(entry.Message.c_str());
				ImGui::PopStyleColor();
			}

			if (m_ScrollToBottom)
			{
				ImGui::SetScrollHereY(1.0f);
				m_ScrollToBottom = false;
			}
		}
		ImGui::EndChild();
		ImGui::End();
	}
}
