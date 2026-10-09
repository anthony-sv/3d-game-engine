#include "Editor/UI/EditorUI.h"

#include "Editor/Commands/CommandHistory.h"
#include "Editor/EntityPresets.h"

#include <imgui.h>

#include <cctype>
#include <span>
#include <string>

namespace Strada
{
	namespace UI
	{
		std::string FormatDisplayName(std::string_view name)
		{
			auto const isUpper = [](char c)
			{
				return std::isupper(static_cast<unsigned char>(c)) != 0;
			};
			auto const isLower = [](char c)
			{
				return std::islower(static_cast<unsigned char>(c)) != 0;
			};
			auto const isDigit = [](char c)
			{
				return std::isdigit(static_cast<unsigned char>(c)) != 0;
			};

			std::string result;
			result.reserve(name.size() + 4);
			for (size_t i = 0; i < name.size(); i++)
			{
				char const c = name[i];
				if (i > 0 && isUpper(c))
				{
					char const previous = name[i - 1];
					bool const startsWord = isLower(previous) || isDigit(previous);
					// The last capital of an acronym of two or more letters starts the next word: "UVTiling" -> "UV Tiling",
					// but "VSync" stays one word.
					bool const endsAcronym =
						isUpper(previous) && i >= 2 && isUpper(name[i - 2]) && i + 1 < name.size() && isLower(name[i + 1]);
					if (startsWord || endsAcronym)
					{
						result += ' ';
					}
				}
				result += c;
			}
			return result;
		}

		EntityPreset const* DrawEntityPresetMenuItems()
		{
			EntityPreset const* chosen = nullptr;
			std::span<EntityPreset const> const presets = EntityPresets::GetAll();
			for (size_t i = 0; i < presets.size();)
			{
				std::string_view const category = presets[i].Category;
				size_t end = i;
				while (end < presets.size() && presets[end].Category == category)
				{
					end++;
				}
				bool const open = category.empty() || ImGui::BeginMenu(std::string(category).c_str());
				if (open)
				{
					for (size_t j = i; j < end; j++)
					{
						if (ImGui::MenuItem(std::string(presets[j].Label).c_str()))
						{
							chosen = &presets[j];
						}
					}
					if (!category.empty())
					{
						ImGui::EndMenu();
					}
				}
				i = end;
			}
			return chosen;
		}
	}

	EditSession::EditSession(uint64_t keyPrefix)
		: m_KeyPrefix(keyPrefix)
	{
	}

	void EditSession::Update(bool interactionActive, CommandHistory& history)
	{
		if (m_CurrentKey != 0 && !interactionActive)
		{
			history.BreakMerge();
			m_CurrentKey = 0;
		}
	}

	uint64_t EditSession::GetMergeKey()
	{
		if (m_CurrentKey == 0)
		{
			m_CurrentKey = m_KeyPrefix + ++m_SessionCount;
		}
		return m_CurrentKey;
	}
}
