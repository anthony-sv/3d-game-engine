#include "Editor/StartScreen/StartScreenCards.h"

#include "Strada/ImGui/ImGuiRenderer.h"

#include <imgui_internal.h>
#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace Strada
{
	namespace
	{
		constexpr ImVec4 NoSdkColor = ImVec4(0.45f, 0.8f, 0.55f, 1.0f);
		constexpr ImVec4 SdkColor = ImVec4(0.55f, 0.7f, 1.0f, 1.0f);
		constexpr ImVec4 WarningColor = ImVec4(1.0f, 0.72f, 0.3f, 1.0f);
		// Thumbnails are 16:9.
		constexpr float ImageAspect = 9.0f / 16.0f;

		float GetPadding()
		{
			return std::floor(ImGui::GetStyle().FramePadding.x + 2.0f);
		}

		// The thumbnail fitted into the image area with its aspect kept, or an empty frame.
		void DrawImage(ImDrawList* drawList, nvrhi::ITexture* thumbnail, ImVec2 areaMin, ImVec2 areaMax)
		{
			drawList->AddRectFilled(areaMin, areaMax, ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
			if (thumbnail == nullptr)
			{
				return;
			}
			nvrhi::TextureDesc const& desc = thumbnail->getDesc();
			float const width = areaMax.x - areaMin.x;
			float const height = areaMax.y - areaMin.y;
			float const scale =
				std::min(width / static_cast<float>(std::max(desc.width, 1u)), height / static_cast<float>(std::max(desc.height, 1u)));
			ImVec2 const size(std::floor(static_cast<float>(desc.width) * scale), std::floor(static_cast<float>(desc.height) * scale));
			ImVec2 const offset(std::floor((width - size.x) * 0.5f), std::floor((height - size.y) * 0.5f));
			ImVec2 const imageMin(areaMin.x + offset.x, areaMin.y + offset.y);
			drawList->AddImageRounded(ImTextureRef(ImGuiRenderer::GetTextureID(thumbnail)), imageMin,
			                          ImVec2(imageMin.x + size.x, imageMin.y + size.y), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
			                          IM_COL32_WHITE, ImGui::GetStyle().FrameRounding);
		}

		// One line of text that ends with "..." where it does not fit.
		void DrawLine(ImDrawList* drawList, ImVec2 position, float width, std::string_view text, ImU32 color)
		{
			float const lineHeight = ImGui::GetTextLineHeight();
			std::string const line(text);
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::RenderTextEllipsis(drawList, position, ImVec2(position.x + width, position.y + lineHeight), position.x + width,
			                          line.c_str(), line.c_str() + line.size(), nullptr);
			ImGui::PopStyleColor();
		}
	}

	StartScreenCardLayout StartScreenCardLayout::Vertical(float width, float descriptionLines)
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		float const padding = GetPadding();
		float const lineHeight = ImGui::GetTextLineHeight();
		StartScreenCardLayout layout;
		layout.Width = std::max(width, padding * 4.0f);
		layout.ImageWidth = layout.Width - padding * 2.0f;
		layout.ImageHeight = std::floor(layout.ImageWidth * ImageAspect);
		layout.DescriptionLines = descriptionLines;
		layout.Height =
			std::ceil(padding * 2.0f + layout.ImageHeight + style.ItemSpacing.y * 2.0f + lineHeight * (2.0f + descriptionLines));
		return layout;
	}

	StartScreenCardLayout StartScreenCardLayout::Horizontal(float width, float descriptionLines)
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		float const padding = GetPadding();
		float const lineHeight = ImGui::GetTextLineHeight();
		StartScreenCardLayout layout;
		layout.ImageBeside = true;
		layout.Width = std::max(width, padding * 6.0f);
		layout.ImageWidth = std::floor(std::clamp(layout.Width * 0.38f, padding * 2.0f, ImGui::GetFontSize() * 11.0f));
		layout.ImageHeight = std::floor(layout.ImageWidth * ImageAspect);
		layout.DescriptionLines = descriptionLines;
		float const text = lineHeight * (2.0f + descriptionLines) + style.ItemSpacing.y * 2.0f + ImGui::GetFrameHeight();
		layout.Height = std::ceil(padding * 2.0f + std::max(layout.ImageHeight, text));
		return layout;
	}

	StartScreenCardResult DrawStartScreenCard(char const* id, StartScreenCard const& card, StartScreenCardLayout const& layout,
	                                          bool selected)
	{
		StartScreenCardResult result;
		ImGuiStyle const& style = ImGui::GetStyle();
		ImVec2 const cardMin = ImGui::GetCursorScreenPos();
		ImVec2 const cardMax(cardMin.x + layout.Width, cardMin.y + layout.Height);
		result.Min = cardMin;
		result.Max = cardMax;
		if (layout.ImageBeside)
		{
			ImGui::Dummy(ImVec2(layout.Width, layout.Height));
		}
		else
		{
			result.Clicked = ImGui::Selectable(id, selected, ImGuiSelectableFlags_None, ImVec2(layout.Width, layout.Height));
		}
		bool const hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort);
		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		float const padding = GetPadding();
		float const lineHeight = ImGui::GetTextLineHeight();
		ImU32 const border = ImGui::GetColorU32(selected ? ImGuiCol_CheckMark : ImGuiCol_Border);
		drawList->AddRect(cardMin, cardMax, border, style.FrameRounding, selected ? 2.0f : 1.0f);

		ImVec2 const imageMin(cardMin.x + padding, cardMin.y + padding);
		ImVec2 const imageMax(imageMin.x + layout.ImageWidth, imageMin.y + layout.ImageHeight);
		DrawImage(drawList, card.Thumbnail, imageMin, imageMax);

		ImVec2 text =
			layout.ImageBeside ? ImVec2(imageMax.x + padding, cardMin.y + padding) : ImVec2(imageMin.x, imageMax.y + style.ItemSpacing.y);
		float const textWidth = std::max(cardMax.x - padding - text.x, 1.0f);
		DrawLine(drawList, text, textWidth, card.Name, ImGui::GetColorU32(ImGuiCol_Text));
		text.y += lineHeight;
		DrawLine(drawList, text, textWidth, GetStartScreenRequirementBadge(card.RequiresSdk),
		         ImGui::GetColorU32(card.RequiresSdk ? SdkColor : NoSdkColor));
		text.y += lineHeight + style.ItemSpacing.y;
		ImVec4 const clip(text.x, text.y, text.x + textWidth, text.y + lineHeight * layout.DescriptionLines);
		drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), text, ImGui::GetColorU32(ImGuiCol_TextDisabled), card.Description.data(),
		                  card.Description.data() + card.Description.size(), textWidth, &clip);
		result.ActionPosition = ImVec2(text.x, cardMax.y - padding - ImGui::GetFrameHeight());
		result.ActionWidth = textWidth;
		if (hovered && !card.Description.empty() && ImGui::BeginTooltip())
		{
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
			ImGui::TextUnformatted(card.Description.data(), card.Description.data() + card.Description.size());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
		return result;
	}

	std::string_view GetStartScreenRequirementBadge(bool requiresSdk)
	{
		return requiresSdk ? "C#: needs the .NET 10 SDK" : "Visual scripts: no SDK needed";
	}

	ImVec4 GetStartScreenWarningColor()
	{
		return WarningColor;
	}
}
