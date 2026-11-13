#pragma once

#include <imgui.h>

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
	// click) or beside it with a row for a button under the text (Horizontal: the example cards).
	struct StartScreenCardLayout
	{
		bool ImageBeside = false;
		float Width = 0.0f;
		float Height = 0.0f;
		float ImageWidth = 0.0f;
		float ImageHeight = 0.0f;
		float DescriptionLines = 0.0f;

		static StartScreenCardLayout Vertical(float width, float descriptionLines);
		static StartScreenCardLayout Horizontal(float width, float descriptionLines);
	};

	// What drawing a card gave.
	struct StartScreenCardResult
	{
		// A Vertical card was clicked.
		bool Clicked = false;
		// Where the card is on the screen.
		ImVec2 Min = ImVec2(0.0f, 0.0f);
		ImVec2 Max = ImVec2(0.0f, 0.0f);
		// A Horizontal card's button row: where a button goes and how wide the row is.
		ImVec2 ActionPosition = ImVec2(0.0f, 0.0f);
		float ActionWidth = 0.0f;
	};

	// Draws a card at the cursor, as one item: a selectable one (Vertical, highlighted when selected) or a frame (Horizontal,
	// whose button the caller draws at ActionPosition). The whole description shows in the card's tooltip.
	StartScreenCardResult DrawStartScreenCard(char const* id, StartScreenCard const& card, StartScreenCardLayout const& layout,
	                                          bool selected);

	// "Visual scripts: no SDK needed", or "C#: needs the .NET 10 SDK".
	[[nodiscard]] std::string_view GetStartScreenRequirementBadge(bool requiresSdk);
	// The colour of warnings and reasons on the start screen and in the New Project form.
	[[nodiscard]] ImVec4 GetStartScreenWarningColor();
}
