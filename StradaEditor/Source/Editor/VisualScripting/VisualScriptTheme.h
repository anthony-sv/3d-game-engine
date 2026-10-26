#pragma once

#include "Strada/Script/VisualScriptLibrary.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string_view>

// The canvas's colours: the theme colours it builds on, the colours of node kinds and of types, and the rules that keep
// text readable and states visible on every colour. Colours are uint32_t in ImGui's IM_COL32 layout (red in the low byte),
// sRGB-encoded with straight alpha, so they pass straight to ImDrawList while this stays free of ImGui. Pure functions,
// usable on any thread.
namespace Strada
{
	// Packs a colour in the IM_COL32 layout; channels from 0 to 255.
	constexpr uint32_t VisualScriptRgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255)
	{
		return (a << 24) | (b << 16) | (g << 8) | r;
	}

	// The theme colours the canvas builds on. The renderer reads them from the live ImGui style every frame; the defaults
	// equal the editor theme's (ImGuiLayer::ApplyTheme).
	struct VisualScriptThemeAnchors
	{
		// ImGuiCol_DockingEmptyBg.
		uint32_t Canvas = VisualScriptRgba(0x1C, 0x1E, 0x23);
		// Node bodies: ImGuiCol_PopupBg with its alpha forced to 1.
		uint32_t Body = VisualScriptRgba(0x2C, 0x30, 0x38);
		uint32_t Border = VisualScriptRgba(0x16, 0x18, 0x1C);
		uint32_t Text = VisualScriptRgba(0xE4, 0xE7, 0xEC);
		uint32_t TextDisabled = VisualScriptRgba(0x86, 0x8C, 0x98);
		// Value chips: ImGuiCol_FrameBg and its hovered and active forms.
		uint32_t Frame = VisualScriptRgba(0x33, 0x37, 0x3F);
		uint32_t FrameHovered = VisualScriptRgba(0x3C, 0x41, 0x4B);
		uint32_t FrameActive = VisualScriptRgba(0x46, 0x4C, 0x58);
		// Node comment bubbles: ImGuiCol_Button.
		uint32_t Button = VisualScriptRgba(0x3A, 0x3F, 0x49);
		// ImGuiCol_CheckMark, the content browser's selection colour: selections, and the canvas's accent (the ring of
		// compatible pins while wiring, the focus frame).
		uint32_t Selection = VisualScriptRgba(0x5A, 0x9D, 0xFF);

		bool operator==(VisualScriptThemeAnchors const& other) const = default;
	};

	// The content browser's badge and hover colours, which the theme does not define.
	inline constexpr uint32_t VisualScriptErrorColor = VisualScriptRgba(235, 92, 80);
	inline constexpr uint32_t VisualScriptWarningColor = VisualScriptRgba(232, 164, 56);
	inline constexpr uint32_t VisualScriptHoverOutlineColor = VisualScriptRgba(107, 115, 133);
	// Pins and wires whose type is not known: unresolved wildcards ("*"), types the library does not have, and every data pin
	// without a library.
	inline constexpr uint32_t VisualScriptWildcardColor = VisualScriptRgba(0x7B, 0x81, 0x89);

	// A disabled node draws every primitive of itself and of its wires at this alpha, except text, which stays readable.
	inline constexpr float VisualScriptDisabledAlpha = 0.55f;
	// A value chip shows a value that is not stored (the pin's or the type's default) in Text at this alpha.
	inline constexpr float VisualScriptDefaultValueAlpha = 0.6f;
	// A state ring keeps its colour when that colour is at least this far (ΔE_OK) from the type colour it surrounds.
	inline constexpr float VisualScriptRingMinDistance = 0.1f;

	// The colour of a node kind's header. Headers share one OKLCH lightness, so their white titles keep a nearly constant
	// contrast and the kind is carried by hue. Within the Call, Pure and Latent kinds, nodes of a quieter category (the first
	// segment of the template's slash-separated Category: Debug and Testing, less so Conversions) keep the hue with less
	// chroma. Operator nodes have no header; their box at Overview and Far takes Pure's colour, as operators are pure.
	// Reroutes have no header or box; they get Flow's neutral colour.
	uint32_t GetVisualScriptHeaderColor(VisualScriptNodeKind kind, std::string_view category);

	// The colours a node is drawn with, derived from its header colour and the theme.
	struct VisualScriptNodeColors
	{
		// The header's gradient runs from Header (left) to HeaderEnd (right): mix(Header, Body, 0.65).
		uint32_t Header = 0;
		uint32_t HeaderEnd = 0;
		// The whole node at Overview and Far: mix(Header, Body, 0.25).
		uint32_t Box = 0;
		// The subtitle under the title: mix(Text, Header, 0.10).
		uint32_t Subtitle = 0;
	};
	VisualScriptNodeColors ComputeVisualScriptNodeColors(uint32_t header, VisualScriptThemeAnchors const& theme);
	// Pin labels on node bodies: mix(Text, Body, 0.12).
	uint32_t GetVisualScriptPinLabelColor(VisualScriptThemeAnchors const& theme);

	// The colour of the pins and wires of a type kind. Exec is the default theme's Text. Arrays take their element's colour,
	// which only the type knows: the overload below resolves it, and Array alone (an array of an unknown element) is the
	// wildcard colour.
	uint32_t GetVisualScriptTypeColor(VisualScriptTypeKind kind);
	// The colour of a pin's type as the canvas knows it: "exec", a type ID of the library, or "*". Arrays take their
	// element's colour; "*", types the library does not have and every type but exec without a library get the wildcard
	// colour.
	uint32_t GetVisualScriptTypeColor(std::string_view type, VisualScriptLibrary const* library);

	// The colours of a comment box of a linear RGBA colour (a document's comment Color; channels outside 0 to 1 are clamped).
	struct VisualScriptCommentColors
	{
		// The box: the colour (converted to sRGB) at its alpha.
		uint32_t Body = 0;
		// The title bar, opaque: the colour composited over the canvas at an alpha of at least 0.6, mixed toward the canvas
		// when neither text colour reaches 4.5:1 on it.
		uint32_t Bar = 0;
		// The title: whichever of the theme's Text and Border contrasts more with the bar (Text once the bar was mixed).
		uint32_t BarText = 0;
		// The outline: the colour at 60 %.
		uint32_t Border = 0;
	};
	VisualScriptCommentColors ComputeVisualScriptCommentColors(glm::vec4 linearColor, VisualScriptThemeAnchors const& theme);

	// A colour the editor offers comment boxes (the comment menu, Details), in linear RGBA as documents store it.
	struct VisualScriptCommentSwatch
	{
		std::string_view Name;
		glm::vec4 Color{0.0f};
	};
	// The first is the format's default comment colour.
	inline constexpr std::array<VisualScriptCommentSwatch, 8> VisualScriptCommentSwatches = {{
		{"Grey", glm::vec4(0.50f, 0.50f, 0.50f, 0.25f)},
		{"Blue", glm::vec4(0.20f, 0.45f, 0.85f, 0.25f)},
		{"Green", glm::vec4(0.25f, 0.65f, 0.30f, 0.25f)},
		{"Yellow", glm::vec4(0.85f, 0.70f, 0.15f, 0.25f)},
		{"Orange", glm::vec4(0.90f, 0.45f, 0.15f, 0.25f)},
		{"Red", glm::vec4(0.80f, 0.20f, 0.20f, 0.25f)},
		{"Purple", glm::vec4(0.55f, 0.30f, 0.80f, 0.25f)},
		{"Teal", glm::vec4(0.15f, 0.60f, 0.60f, 0.25f)},
	}};

	// The colour of a state ring or outline drawn on or around a type-coloured glyph, dot or wire: the state colour when it is
	// at least VisualScriptRingMinDistance from the type colour, else the theme's Text, so no state vanishes on any type
	// (Selection on Entity, Error on Bool and Warning on Vector are nearly the same colours). Callers also separate the ring
	// from the glyph by a gap of 1 pixel × UI scale in the colour behind it.
	uint32_t GetVisualScriptRingColor(uint32_t stateColor, uint32_t typeColor, VisualScriptThemeAnchors const& theme);

	// The WCAG 2.x contrast ratio of two colours, from 1 to 21; alpha is ignored.
	float GetVisualScriptContrast(uint32_t a, uint32_t b);
	// ΔE_OK: the Euclidean distance of two colours in OKLab; alpha is ignored. About 0.02 is a just-noticeable difference.
	float GetVisualScriptColorDistance(uint32_t a, uint32_t b);
	// ΔE_OK of two colours in linear sRGB, each channel clamped to 0 to 1: for colours that were transformed, such as
	// simulations of colour-vision deficiencies.
	float GetVisualScriptColorDistance(glm::vec3 linearA, glm::vec3 linearB);

	// Blends from (amount 0) to to (amount 1) per channel, alpha included, as the canvas's mix(a, b, t) means: in the
	// sRGB-encoded values ImGui draws.
	uint32_t MixVisualScriptColors(uint32_t from, uint32_t to, float amount);
	// The colour with its alpha multiplied by factor (0 to 1): "a colour at 60 %".
	uint32_t MultiplyVisualScriptAlpha(uint32_t color, float factor);
	// top drawn over bottom, as the GPU blends ImGui's vertices (straight alpha, in the encoded values).
	uint32_t CompositeVisualScriptColors(uint32_t top, uint32_t bottom);
}
