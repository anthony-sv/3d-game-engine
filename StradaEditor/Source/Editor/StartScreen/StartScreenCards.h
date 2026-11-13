#pragma once

#include <imgui.h>

#include <span>
#include <string_view>

namespace nvrhi
{
	class ITexture;
}

namespace Strada
{
	// What a card of the start screen shows: a template's (the New Project form) or an example's.
	struct StartScreenCard
	{
		std::string_view Name;
		std::string_view Description;
		// The badge says what playing it needs (GetStartScreenRequirementBadge).
		bool RequiresSdk = false;
		// Its thumbnail; none draws an empty frame in its place.
		nvrhi::ITexture* Thumbnail = nullptr;
	};

	// Where the parts of a card go, in pixels: the image above the text (Vertical: the template cards, which are chosen with a
	// click) or beside it with a row for a button under the text (Horizontal: the example cards). The name takes one line;
	// the badge and the description take the lines the layout gives them, wrapped at GetTextWidth().
	struct StartScreenCardLayout
	{
		bool ImageBeside = false;
		float Width = 0.0f;
		float Height = 0.0f;
		float ImageWidth = 0.0f;
		float ImageHeight = 0.0f;
		float BadgeLines = 1.0f;
		float DescriptionLines = 0.0f;

		static StartScreenCardLayout Vertical(float width, float descriptionLines, float badgeLines = 1.0f);
		static StartScreenCardLayout Horizontal(float width, float descriptionLines, float badgeLines = 1.0f);

		// How wide the text of a card is: the name, the badge and the description wrap or are cut at this width.
		[[nodiscard]] float GetTextWidth() const;
		// This layout at the same width with as many badge lines and description lines as the cards' texts take, so cards
		// drawn with it side by side or one under another have the same height and show their texts whole. The description
		// takes between minimumLines and maximumLines lines: a longer one is cut, and the card's tooltip shows it.
		[[nodiscard]] StartScreenCardLayout FitText(std::span<StartScreenCard const> cards, float minimumLines, float maximumLines) const;
	};

	// What drawing a card gave.
	struct StartScreenCardResult
	{
		// A Vertical card was clicked.
		bool Clicked = false;
		// Whether its name, badge and description show whole. A card whose text is cut shows it whole in its tooltip.
		bool TextWhole = true;
		// Where the card is on the screen.
		ImVec2 Min = ImVec2(0.0f, 0.0f);
		ImVec2 Max = ImVec2(0.0f, 0.0f);
		// A Horizontal card's button row: where a button goes and how wide the row is.
		ImVec2 ActionPosition = ImVec2(0.0f, 0.0f);
		float ActionWidth = 0.0f;
	};

	// Draws a card at the cursor, as one item: a selectable one (Vertical, highlighted when selected) or a frame (Horizontal,
	// whose button the caller draws at ActionPosition). A card whose text the layout cuts shows its name and description in
	// its tooltip.
	StartScreenCardResult DrawStartScreenCard(char const* id, StartScreenCard const& card, StartScreenCardLayout const& layout,
	                                          bool selected);

	// The lines the text takes wrapped at the width with the current font, at least one. Inside an ImGui frame.
	[[nodiscard]] float CountStartScreenTextLines(std::string_view text, float width);

	// "Visual scripts: no SDK needed", or "C#: needs the .NET 10 SDK".
	[[nodiscard]] std::string_view GetStartScreenRequirementBadge(bool requiresSdk);
	// The colour of warnings and reasons on the start screen and in the New Project form.
	[[nodiscard]] ImVec4 GetStartScreenWarningColor();
}
