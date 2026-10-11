#include "Editor/UI/EditorUI.h"

#include "Editor/Commands/CommandHistory.h"
#include "Editor/EntityPresets.h"

#include "Strada/Asset/AssetManager.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <string>

namespace Strada
{
	namespace UI
	{
		ImVec2 GetToolbarButtonSize()
		{
			float const height = ImGui::GetFrameHeight();
			return ImVec2(std::floor(height * 1.35f), height);
		}

		bool ToolbarButton(char const* id, ToolbarIcon icon, bool active)
		{
			ImVec2 const size = GetToolbarButtonSize();
			bool const pressed = ImGui::InvisibleButton(id, size);
			bool const hovered = ImGui::IsItemHovered();
			bool const held = ImGui::IsItemActive();
			ImVec2 const min = ImGui::GetItemRectMin();
			ImVec2 const max = ImGui::GetItemRectMax();
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			ImGuiCol const background = active || held ? ImGuiCol_ButtonActive : (hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
			drawList->AddRectFilled(min, max, ImGui::GetColorU32(background), ImGui::GetStyle().FrameRounding);

			// The icon fills the middle of the button; Alpha follows BeginDisabled through GetColorU32.
			ImU32 const color = ImGui::GetColorU32(ImGuiCol_Text);
			float const extent = size.y * 0.42f;
			ImVec2 const center(std::floor((min.x + max.x) * 0.5f), std::floor((min.y + max.y) * 0.5f));
			float const half = extent * 0.5f;
			switch (icon)
			{
				case ToolbarIcon::Play:
					drawList->AddTriangleFilled(ImVec2(center.x - half * 0.8f, center.y - half),
					                            ImVec2(center.x - half * 0.8f, center.y + half), ImVec2(center.x + half, center.y), color);
					break;
				case ToolbarIcon::Stop:
					drawList->AddRectFilled(ImVec2(center.x - half * 0.85f, center.y - half * 0.85f),
					                        ImVec2(center.x + half * 0.85f, center.y + half * 0.85f), color, 1.5f);
					break;
				case ToolbarIcon::Pause:
				{
					float const bar = half * 0.6f;
					drawList->AddRectFilled(ImVec2(center.x - half * 0.85f, center.y - half),
					                        ImVec2(center.x - half * 0.85f + bar, center.y + half), color, 1.0f);
					drawList->AddRectFilled(ImVec2(center.x + half * 0.85f - bar, center.y - half),
					                        ImVec2(center.x + half * 0.85f, center.y + half), color, 1.0f);
					break;
				}
				case ToolbarIcon::Step:
				{
					float const bar = half * 0.45f;
					drawList->AddTriangleFilled(ImVec2(center.x - half, center.y - half), ImVec2(center.x - half, center.y + half),
					                            ImVec2(center.x + half * 0.55f, center.y), color);
					drawList->AddRectFilled(ImVec2(center.x + half * 0.55f, center.y - half),
					                        ImVec2(center.x + half * 0.55f + bar, center.y + half), color, 1.0f);
					break;
				}
			}
			return pressed;
		}

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

			// Private C# fields of scripts follow the m_/s_ prefix convention; the prefix is not part of the name shown.
			if (name.size() > 2 && (name.starts_with("m_") || name.starts_with("s_")) && isUpper(name[2]))
			{
				name.remove_prefix(2);
			}

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

		void AssetDragSource(AssetHandle asset, std::string_view label)
		{
			if (!ImGui::BeginDragDropSource())
			{
				return;
			}
			uint64_t const value = asset.GetUUID().GetValue();
			ImGui::SetDragDropPayload(DragDropPayload::Asset, &value, sizeof(value));
			ImGui::TextUnformatted(label.data(), label.data() + label.size());
			ImGui::EndDragDropSource();
		}

		AssetHandle AcceptAssetDrop(std::function<bool(AssetMetadata const&)> const& accepts)
		{
			// Peeking first keeps the target from highlighting for assets it would refuse.
			ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::Asset, ImGuiDragDropFlags_AcceptPeekOnly);
			if (payload == nullptr || payload->DataSize != static_cast<int>(sizeof(uint64_t)) || !AssetManager::IsInitialized())
			{
				return AssetHandle();
			}
			uint64_t value = 0;
			std::memcpy(&value, payload->Data, sizeof(value));
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(AssetHandle(UUID(value)));
			if (!metadata || !accepts(*metadata))
			{
				return AssetHandle();
			}
			return ImGui::AcceptDragDropPayload(DragDropPayload::Asset) != nullptr ? metadata->Handle : AssetHandle();
		}

		AssetHandle AcceptAssetDrop(std::span<AssetType const> types)
		{
			return AcceptAssetDrop(
				[types](AssetMetadata const& metadata)
				{
					return types.empty() || std::find(types.begin(), types.end(), metadata.Type) != types.end();
				});
		}

		std::vector<UUID> ReadEntityPayload(ImGuiPayload const& payload)
		{
			size_t const count = static_cast<size_t>(payload.DataSize) / sizeof(uint64_t);
			std::vector<UUID> entities;
			entities.reserve(count);
			for (size_t i = 0; i < count; i++)
			{
				uint64_t value = 0;
				std::memcpy(&value, static_cast<char const*>(payload.Data) + i * sizeof(uint64_t), sizeof(value));
				entities.emplace_back(value);
			}
			return entities;
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
