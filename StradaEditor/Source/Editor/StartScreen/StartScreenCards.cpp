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

		// Text wrapped at the width, cut after the given number of lines.
		void DrawWrapped(ImDrawList* drawList, ImVec2 position, float width, float lines, std::string_view text, ImU32 color)
		{
			ImVec4 const clip(position.x, position.y, position.x + width, position.y + ImGui::GetTextLineHeight() * lines);
			drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), position, color, text.data(), text.data() + text.size(), width,
			                  &clip);
		}
	}

	StartScreenCardLayout StartScreenCardLayout::Vertical(float width, float descriptionLines, float badgeLines)
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		float const padding = GetPadding();
		float const lineHeight = ImGui::GetTextLineHeight();
		StartScreenCardLayout layout;
		layout.Width = std::max(width, padding * 4.0f);
		layout.ImageWidth = layout.Width - padding * 2.0f;
		layout.ImageHeight = std::floor(layout.ImageWidth * ImageAspect);
		layout.BadgeLines = badgeLines;
		layout.DescriptionLines = descriptionLines;
		layout.Height = std::ceil(padding * 2.0f + layout.ImageHeight + style.ItemSpacing.y * 2.0f +
		                          lineHeight * (1.0f + badgeLines + descriptionLines));
		return layout;
	}

	StartScreenCardLayout StartScreenCardLayout::Horizontal(float width, float descriptionLines, float badgeLines)
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		float const padding = GetPadding();
		float const lineHeight = ImGui::GetTextLineHeight();
		StartScreenCardLayout layout;
		layout.ImageBeside = true;
		layout.Width = std::max(width, padding * 6.0f);
		layout.ImageWidth = std::floor(std::clamp(layout.Width * 0.38f, padding * 2.0f, ImGui::GetFontSize() * 11.0f));
		layout.ImageHeight = std::floor(layout.ImageWidth * ImageAspect);
		layout.BadgeLines = badgeLines;
		layout.DescriptionLines = descriptionLines;
		float const text = lineHeight * (1.0f + badgeLines + descriptionLines) + style.ItemSpacing.y * 2.0f + ImGui::GetFrameHeight();
		layout.Height = std::ceil(padding * 2.0f + std::max(layout.ImageHeight, text));
		return layout;
	}

	float StartScreenCardLayout::GetTextWidth() const
	{
		float const padding = GetPadding();
		float const width = ImageBeside ? Width - ImageWidth - padding * 3.0f : Width - padding * 2.0f;
		return std::max(width, 1.0f);
	}

	StartScreenCardLayout StartScreenCardLayout::FitText(std::span<StartScreenCard const> cards, float minimumLines,
	                                                     float maximumLines) const
	{
		float const textWidth = GetTextWidth();
		float badgeLines = 1.0f;
		float descriptionLines = minimumLines;
		for (StartScreenCard const& card : cards)
		{
			badgeLines = std::max(badgeLines, CountStartScreenTextLines(GetStartScreenRequirementBadge(card.RequiresSdk), textWidth));
			descriptionLines = std::max(descriptionLines, CountStartScreenTextLines(card.Description, textWidth));
		}
		descriptionLines = std::min(descriptionLines, std::max(minimumLines, maximumLines));
		// The text's width depends on the card's width alone, so the fitted layout wraps the texts as they were measured.
		return ImageBeside ? Horizontal(Width, descriptionLines, badgeLines) : Vertical(Width, descriptionLines, badgeLines);
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
			// Highlighted within its border: without the flag a selectable reaches half the item spacing past its size
			// (imgui_widgets.cpp Selectable), into the gap between the cards.
			result.Clicked =
				ImGui::Selectable(id, selected, ImGuiSelectableFlags_NoPadWithHalfSpacing, ImVec2(layout.Width, layout.Height));
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
		float const textWidth = layout.GetTextWidth();
		DrawLine(drawList, text, textWidth, card.Name, ImGui::GetColorU32(ImGuiCol_Text));
		bool const nameWhole = ImGui::CalcTextSize(card.Name.data(), card.Name.data() + card.Name.size()).x <= textWidth;
		text.y += lineHeight;
		std::string_view const badge = GetStartScreenRequirementBadge(card.RequiresSdk);
		DrawWrapped(drawList, text, textWidth, layout.BadgeLines, badge, ImGui::GetColorU32(card.RequiresSdk ? SdkColor : NoSdkColor));
		text.y += lineHeight * layout.BadgeLines + style.ItemSpacing.y;
		DrawWrapped(drawList, text, textWidth, layout.DescriptionLines, card.Description, ImGui::GetColorU32(ImGuiCol_TextDisabled));
		result.TextWhole = nameWhole && CountStartScreenTextLines(badge, textWidth) <= layout.BadgeLines &&
		                   (card.Description.empty() || CountStartScreenTextLines(card.Description, textWidth) <= layout.DescriptionLines);
		result.ActionPosition = ImVec2(text.x, cardMax.y - padding - ImGui::GetFrameHeight());
		result.ActionWidth = textWidth;
		if (hovered && !result.TextWhole && ImGui::BeginTooltip())
		{
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
			ImGui::TextUnformatted(card.Name.data(), card.Name.data() + card.Name.size());
			ImGui::TextDisabled("%.*s", static_cast<int>(card.Description.size()), card.Description.data());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
		return result;
	}

	float CountStartScreenTextLines(std::string_view text, float width)
	{
		if (text.empty())
		{
			return 1.0f;
		}
		float const height = ImGui::CalcTextSize(text.data(), text.data() + text.size(), false, std::max(width, 1.0f)).y;
		return std::max(1.0f, std::round(height / ImGui::GetTextLineHeight()));
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
