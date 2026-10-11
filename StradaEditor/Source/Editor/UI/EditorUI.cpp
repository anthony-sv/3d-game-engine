#include "Editor/UI/EditorUI.h"

#include "Editor/Commands/CommandHistory.h"
#include "Editor/EntityPresets.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"

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
		namespace
		{
			float TextWidth(std::string_view text)
			{
				return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
			}

			// Whether an ASCII capital at index starts a word of a PascalCase name: after a lowercase letter ("Showcase|Tests")
			// or as the last capital of an acronym ("XML|Parser").
			bool StartsWord(std::string_view text, size_t index)
			{
				auto const isUpper = [&text](size_t at)
				{
					return at < text.size() && text[at] >= 'A' && text[at] <= 'Z';
				};
				auto const isLower = [&text](size_t at)
				{
					return at < text.size() && text[at] >= 'a' && text[at] <= 'z';
				};
				return index > 0 && isUpper(index) && (isLower(index - 1) || (isUpper(index - 1) && isLower(index + 1)));
			}

			// Bytes of the UTF-8 code point starting at index.
			size_t CodePointLength(std::string_view text, size_t index)
			{
				size_t length = 1;
				while (index + length < text.size() && (static_cast<unsigned char>(text[index + length]) & 0xC0) == 0x80)
				{
					length++;
				}
				return length;
			}

			// The longest start of the text, cut between code points, that is at most the width wide.
			size_t FitCodePoints(std::string_view text, float width)
			{
				size_t end = 0;
				while (end < text.size())
				{
					size_t const next = end + CodePointLength(text, end);
					if (TextWidth(text.substr(0, next)) > width)
					{
						break;
					}
					end = next;
				}
				return end;
			}

			// The text shortened with "..." to fit the width. A given end of the text (a file's extension) stays visible:
			// "LongMaterialName.smat" becomes "LongMat...smat".
			std::string FitTextKeepingEnd(std::string_view text, float width, std::string_view end)
			{
				if (TextWidth(text) <= width)
				{
					return std::string(text);
				}
				constexpr std::string_view Ellipsis = "...";
				if (end.size() > 1 && end.size() < text.size())
				{
					// The ellipsis replaces the extension's dot.
					std::string_view const tail = end.starts_with('.') ? end.substr(1) : end;
					float const tailWidth = TextWidth(Ellipsis) + TextWidth(tail);
					size_t const head = FitCodePoints(text.substr(0, text.size() - end.size()), width - tailWidth);
					if (head > 0)
					{
						return std::string(text.substr(0, head)) + std::string(Ellipsis) + std::string(tail);
					}
				}
				return std::string(text.substr(0, FitCodePoints(text, width - TextWidth(Ellipsis)))) + std::string(Ellipsis);
			}
		}

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

		void ScriptDragSource(std::filesystem::path const& file, std::string_view label)
		{
			if (!ImGui::BeginDragDropSource())
			{
				return;
			}
			std::string const path = FileSystem::PathToUtf8(file);
			ImGui::SetDragDropPayload(DragDropPayload::ScriptFile, path.data(), path.size());
			ImGui::TextUnformatted(label.data(), label.data() + label.size());
			ImGui::EndDragDropSource();
		}

		std::optional<std::filesystem::path> AcceptScriptDrop()
		{
			ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::ScriptFile);
			if (payload == nullptr || payload->DataSize <= 0)
			{
				return std::nullopt;
			}
			return FileSystem::PathFromUtf8(
				std::string_view(static_cast<char const*>(payload->Data), static_cast<size_t>(payload->DataSize)));
		}

		std::array<std::string, 2> SplitTileLabel(std::string_view name, float width, std::string_view extension)
		{
			if (TextWidth(name) <= width)
			{
				return {std::string(name), std::string()};
			}
			// The first line ends at the last place that fits after a space, '_' or '-', before a '.' (the extension then
			// starts the second line) or before a word of a PascalCase name ("Showcase|Tests"); a long word breaks where the
			// line is full. The text before index end always fits.
			size_t end = 0;
			size_t breakAt = 0;
			while (end < name.size())
			{
				char const c = name[end];
				if (end > 0 && (c == '.' || StartsWord(name, end)))
				{
					breakAt = end;
				}
				size_t const next = end + CodePointLength(name, end);
				if (TextWidth(name.substr(0, next)) > width)
				{
					break;
				}
				if (c == ' ' || c == '_' || c == '-')
				{
					breakAt = next;
				}
				end = next;
			}
			if (breakAt > 0)
			{
				end = breakAt;
			}
			// Every line shows at least one character, however narrow the tile.
			end = std::max(end, CodePointLength(name, 0));
			std::string_view first = name.substr(0, end);
			while (first.ends_with(' '))
			{
				first.remove_suffix(1);
			}
			std::string_view const rest = name.substr(end);
			return {std::string(first), FitTextKeepingEnd(rest, width, rest.ends_with(extension) ? extension : std::string_view())};
		}

		std::string FormatDuration(double seconds)
		{
			// Negative and invalid lengths read as none; a billion seconds is beyond any clip.
			int64_t const milliseconds = std::isfinite(seconds) ? std::llround(std::clamp(seconds, 0.0, 1.0e9) * 1000.0) : 0;
			if (milliseconds < 1000)
			{
				return fmt::format("{} ms", milliseconds);
			}
			// From 9.95 seconds on, one decimal would round to "10.0 s".
			if (milliseconds < 9950)
			{
				return fmt::format("{:.1f} s", static_cast<double>(milliseconds) / 1000.0);
			}
			int64_t const total = (milliseconds + 500) / 1000;
			return fmt::format("{}:{:02}", total / 60, total % 60);
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
