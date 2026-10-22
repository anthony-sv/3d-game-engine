#include "Fuzzing.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptTheme.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Math/Math.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	constexpr std::array<VisualScriptNodeKind, 11> NodeKinds = {
		VisualScriptNodeKind::Event,  VisualScriptNodeKind::Flow,     VisualScriptNodeKind::Call,     VisualScriptNodeKind::Pure,
		VisualScriptNodeKind::Latent, VisualScriptNodeKind::Variable, VisualScriptNodeKind::Function, VisualScriptNodeKind::Operator,
		VisualScriptNodeKind::Data,   VisualScriptNodeKind::Reroute,  VisualScriptNodeKind::Unknown};

	constexpr std::array<VisualScriptTypeKind, 17> TypeKinds = {
		VisualScriptTypeKind::Exec,   VisualScriptTypeKind::Bool,   VisualScriptTypeKind::Integer,    VisualScriptTypeKind::Float,
		VisualScriptTypeKind::String, VisualScriptTypeKind::Vector, VisualScriptTypeKind::Quaternion, VisualScriptTypeKind::Color,
		VisualScriptTypeKind::Matrix, VisualScriptTypeKind::Entity, VisualScriptTypeKind::Component,  VisualScriptTypeKind::Asset,
		VisualScriptTypeKind::Enum,   VisualScriptTypeKind::Struct, VisualScriptTypeKind::Class,      VisualScriptTypeKind::Array,
		VisualScriptTypeKind::Object};

	// The types graphs use most, which must stay apart in every kind of colour vision; rarer pairs are told apart by the pin
	// tooltip's text.
	constexpr std::array<VisualScriptTypeKind, 9> FrequentTypeKinds = {
		VisualScriptTypeKind::Exec,   VisualScriptTypeKind::Bool,      VisualScriptTypeKind::Integer,
		VisualScriptTypeKind::Float,  VisualScriptTypeKind::String,    VisualScriptTypeKind::Vector,
		VisualScriptTypeKind::Entity, VisualScriptTypeKind::Component, VisualScriptTypeKind::Enum};

	// Categories as node templates have them: none, the quieter ones (by their first segment) and ordinary ones.
	constexpr std::array<std::string_view, 7> Categories = {
		"", "Debug", "Testing", "Testing/Assertions", "Conversions", "Math/Vector3", "Components/Transform"};

	// Colour-vision deficiencies as Machado, Oliveira and Fernandes (2009) simulate them at severity 1, in linear sRGB.
	using VisionMatrix = std::array<std::array<float, 3>, 3>;

	struct Vision
	{
		std::string_view Name;
		VisionMatrix Matrix;
	};

	constexpr std::array<Vision, 3> Deficiencies = {{
		{"deuteranopia", {{{0.367322f, 0.860646f, -0.227968f}, {0.280085f, 0.672501f, 0.047413f}, {-0.011820f, 0.042940f, 0.968881f}}}},
		{"protanopia", {{{0.152286f, 1.052583f, -0.204868f}, {0.114503f, 0.786281f, 0.099216f}, {-0.003882f, -0.048116f, 1.051998f}}}},
		{"tritanopia", {{{1.255528f, -0.076749f, -0.178779f}, {-0.078411f, 0.930809f, 0.147602f}, {0.004733f, 0.691367f, 0.303900f}}}},
	}};

	uint32_t Hex(uint32_t rgb)
	{
		return VisualScriptRgba((rgb >> 16) & 0xFFu, (rgb >> 8) & 0xFFu, rgb & 0xFFu);
	}

	std::string Describe(uint32_t color)
	{
		return fmt::format("#{:02X}{:02X}{:02X} alpha {}", color & 0xFFu, (color >> 8) & 0xFFu, (color >> 16) & 0xFFu, color >> 24);
	}

	uint32_t GetAlpha(uint32_t color)
	{
		return color >> 24;
	}

	// Whether two colours differ by at most one step in every channel: what rounding to bytes at different points gives.
	bool IsWithinRounding(uint32_t a, uint32_t b)
	{
		for (int shift = 0; shift < 32; shift += 8)
		{
			int const difference = static_cast<int>((a >> shift) & 0xFFu) - static_cast<int>((b >> shift) & 0xFFu);
			if (difference < -1 || difference > 1)
			{
				return false;
			}
		}
		return true;
	}

	glm::vec3 ToLinear(uint32_t color)
	{
		return glm::vec3(Math::SrgbToLinear(static_cast<float>(color & 0xFFu) / 255.0f),
		                 Math::SrgbToLinear(static_cast<float>((color >> 8) & 0xFFu) / 255.0f),
		                 Math::SrgbToLinear(static_cast<float>((color >> 16) & 0xFFu) / 255.0f));
	}

	glm::vec3 Simulate(VisionMatrix const& matrix, glm::vec3 linear)
	{
		glm::vec3 result(0.0f);
		for (glm::length_t row = 0; row < 3; row++)
		{
			for (glm::length_t column = 0; column < 3; column++)
			{
				result[row] += matrix[static_cast<size_t>(row)][static_cast<size_t>(column)] * linear[column];
			}
		}
		return result;
	}

	// The byte a channel from 0 to 1 is drawn with.
	uint32_t ToByte(float value)
	{
		return static_cast<uint32_t>(std::lround(static_cast<double>(value) * 255.0));
	}

	// What a disabled node draws behind its text: a colour faded to VisualScriptDisabledAlpha over the canvas.
	uint32_t Faded(uint32_t color, VisualScriptThemeAnchors const& theme)
	{
		return CompositeVisualScriptColors(MultiplyVisualScriptAlpha(color, VisualScriptDisabledAlpha), theme.Canvas);
	}

	// Every header a node can get: each kind with each category.
	std::vector<uint32_t> GetHeaders()
	{
		std::vector<uint32_t> headers;
		for (VisualScriptNodeKind const kind : NodeKinds)
		{
			for (std::string_view const category : Categories)
			{
				headers.push_back(GetVisualScriptHeaderColor(kind, category));
			}
		}
		return headers;
	}

	// Every colour a pin, wire or reroute is drawn in: each type kind's (arrays take their element's) and the wildcard's.
	std::vector<uint32_t> GetTypeColors()
	{
		std::vector<uint32_t> colors;
		for (VisualScriptTypeKind const kind : TypeKinds)
		{
			colors.push_back(GetVisualScriptTypeColor(kind));
		}
		colors.push_back(VisualScriptWildcardColor);
		return colors;
	}
}

TEST_CASE("VisualScriptEditor: canvas colours are packed as ImGui packs them, and the anchors default to the editor theme's")
{
	CHECK(VisualScriptRgba(0x12, 0x34, 0x56, 0x78) == 0x78563412u);
	CHECK(VisualScriptRgba(0x12, 0x34, 0x56) == 0xFF563412u);

	VisualScriptThemeAnchors const theme;
	CHECK(theme.Canvas == 0xFF231E1Cu);
	CHECK(theme.Body == 0xFF38302Cu);
	CHECK(theme.Border == 0xFF1C1816u);
	CHECK(theme.Text == 0xFFECE7E4u);
	CHECK(theme.TextDisabled == 0xFF988C86u);
	CHECK(theme.Frame == 0xFF3F3733u);
	CHECK(theme.FrameHovered == 0xFF4B413Cu);
	CHECK(theme.FrameActive == 0xFF584C46u);
	CHECK(theme.Button == 0xFF493F3Au);
	CHECK(theme.Selection == 0xFFFF9D5Au);

	CHECK(VisualScriptErrorColor == Hex(0xEB5C50));
	CHECK(VisualScriptWarningColor == Hex(0xE8A438));
	CHECK(VisualScriptHoverOutlineColor == Hex(0x6B7385));
	CHECK(VisualScriptWildcardColor == Hex(0x7B8189));
}

TEST_CASE("VisualScriptEditor: contrast is the WCAG ratio and colour distance is the distance in OKLab")
{
	uint32_t const black = Hex(0x000000);
	uint32_t const white = Hex(0xFFFFFF);
	VisualScriptThemeAnchors const theme;

	CHECK(GetVisualScriptContrast(black, white) == doctest::Approx(21.0f));
	CHECK(GetVisualScriptContrast(white, black) == doctest::Approx(21.0f));
	CHECK(GetVisualScriptContrast(theme.Body, theme.Body) == doctest::Approx(1.0f));
	CHECK(GetVisualScriptContrast(theme.Text, theme.Canvas) == doctest::Approx(13.45f).epsilon(0.001));
	CHECK(GetVisualScriptContrast(theme.Selection, theme.Body) == doctest::Approx(4.85f).epsilon(0.002));
	// Alpha is ignored.
	CHECK(GetVisualScriptContrast(theme.Text & 0x00FFFFFFu, theme.Canvas) == GetVisualScriptContrast(theme.Text, theme.Canvas));

	CHECK(GetVisualScriptColorDistance(theme.Text, theme.Text) == 0.0f);
	CHECK(GetVisualScriptColorDistance(black, white) == doctest::Approx(1.0f).epsilon(0.001));
	CHECK(GetVisualScriptColorDistance(white, black) == GetVisualScriptColorDistance(black, white));
	CHECK(GetVisualScriptColorDistance(theme.Text & 0x00FFFFFFu, white) == GetVisualScriptColorDistance(theme.Text, white));
	// The near-collisions between state and type colours that the ring rule exists for.
	CHECK(GetVisualScriptColorDistance(theme.Selection, GetVisualScriptTypeColor(VisualScriptTypeKind::Entity)) ==
	      doctest::Approx(0.0241f).epsilon(0.01));
	CHECK(GetVisualScriptColorDistance(VisualScriptErrorColor, GetVisualScriptTypeColor(VisualScriptTypeKind::Bool)) ==
	      doctest::Approx(0.0226f).epsilon(0.01));
	CHECK(GetVisualScriptColorDistance(VisualScriptWarningColor, GetVisualScriptTypeColor(VisualScriptTypeKind::Vector)) ==
	      doctest::Approx(0.0269f).epsilon(0.01));

	// The linear form measures the same, and clamps channels outside 0 to 1 (a simulated colour can leave the gamut).
	CHECK(GetVisualScriptColorDistance(ToLinear(theme.Selection), ToLinear(theme.Text)) ==
	      doctest::Approx(GetVisualScriptColorDistance(theme.Selection, theme.Text)));
	CHECK(GetVisualScriptColorDistance(glm::vec3(-1.0f, 2.0f, 0.5f), glm::vec3(0.0f, 1.0f, 0.5f)) == 0.0f);
}

TEST_CASE("VisualScriptEditor: canvas colours mix, fade and composite in the values ImGui draws")
{
	uint32_t const black = Hex(0x000000);
	uint32_t const white = Hex(0xFFFFFF);
	uint32_t const red = Hex(0xFF0000);

	CHECK(MixVisualScriptColors(red, white, 0.0f) == red);
	CHECK(MixVisualScriptColors(red, white, 1.0f) == white);
	CHECK(MixVisualScriptColors(black, white, 0.5f) == Hex(0x808080));
	CHECK(MixVisualScriptColors(black, white, 0.25f) == Hex(0x404040));
	CHECK(MixVisualScriptColors(red, white, -1.0f) == red);
	CHECK(MixVisualScriptColors(red, white, 2.0f) == white);
	CHECK(MixVisualScriptColors(red, white, std::numeric_limits<float>::quiet_NaN()) == red);
	// Alpha is mixed too.
	CHECK(MixVisualScriptColors(VisualScriptRgba(0, 0, 0, 0), white, 0.5f) == VisualScriptRgba(128, 128, 128, 128));

	CHECK(MultiplyVisualScriptAlpha(red, 0.6f) == VisualScriptRgba(255, 0, 0, 153));
	CHECK(MultiplyVisualScriptAlpha(VisualScriptRgba(255, 0, 0, 128), 0.5f) == VisualScriptRgba(255, 0, 0, 64));
	CHECK(MultiplyVisualScriptAlpha(red, 1.5f) == red);
	CHECK(MultiplyVisualScriptAlpha(red, -1.0f) == VisualScriptRgba(255, 0, 0, 0));

	CHECK(CompositeVisualScriptColors(red, white) == red);
	CHECK(CompositeVisualScriptColors(VisualScriptRgba(255, 0, 0, 0), white) == white);
	CHECK(CompositeVisualScriptColors(VisualScriptRgba(255, 255, 255, 128), black) == Hex(0x808080));
	CHECK(CompositeVisualScriptColors(MultiplyVisualScriptAlpha(white, 0.25f), black) == Hex(0x404040));
	// Over nothing, the top colour stays as it is.
	CHECK(CompositeVisualScriptColors(VisualScriptRgba(10, 20, 30, 40), VisualScriptRgba(200, 200, 200, 0)) ==
	      VisualScriptRgba(10, 20, 30, 40));
	CHECK(CompositeVisualScriptColors(VisualScriptRgba(10, 20, 30, 0), VisualScriptRgba(200, 200, 200, 0)) == 0u);
}

TEST_CASE("VisualScriptEditor: node kinds have the header colours of the design, quieter in debugging, test and conversion "
          "categories")
{
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Event, "") == Hex(0x9E2C2C));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Flow, "") == Hex(0x555B65));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Call, "") == Hex(0x1E5BA1));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Latent, "") == Hex(0x554DA0));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Pure, "") == Hex(0x1B6C36));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Variable, "") == Hex(0x00686C));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Function, "") == Hex(0x76408C));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Data, "") == Hex(0x4D6242));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Unknown, "") == Hex(0x782A28));
	// Kinds without a header: an operator's box is Pure's, a reroute gets Flow's neutral colour.
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Operator, "Math/Operators") == Hex(0x1B6C36));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Reroute, "Utilities") == Hex(0x555B65));
	// A kind outside the enumeration is drawn as an unknown node.
	CHECK(GetVisualScriptHeaderColor(static_cast<VisualScriptNodeKind>(200), "") == Hex(0x782A28));

	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Call, "Debug") == Hex(0x445C7B));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Call, "Testing") == Hex(0x445C7B));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Call, "Conversions") == Hex(0x355C8D));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Pure, "Testing/Assertions") == Hex(0x46634B));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Pure, "Conversions") == Hex(0x376742));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Latent, "Debug") == Hex(0x57567B));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Latent, "Conversions/Numbers") == Hex(0x56538C));
	// Only the first segment counts, and only whole words; other kinds keep their colour in every category.
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Call, "Debugging") == Hex(0x1E5BA1));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Call, "Components/Debug") == Hex(0x1E5BA1));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Flow, "Debug") == Hex(0x555B65));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Data, "Conversions") == Hex(0x4D6242));
	CHECK(GetVisualScriptHeaderColor(VisualScriptNodeKind::Event, "Testing") == Hex(0x9E2C2C));
}

TEST_CASE("VisualScriptEditor: node colours are mixed from the header and the theme")
{
	VisualScriptThemeAnchors const theme;
	// The gradient ends of the design's table.
	struct Expected
	{
		VisualScriptNodeKind Kind;
		uint32_t HeaderEnd;
	};
	constexpr std::array<Expected, 9> gradientEnds = {{
		{VisualScriptNodeKind::Event, 0x542F34},
		{VisualScriptNodeKind::Flow, 0x3A3F48},
		{VisualScriptNodeKind::Call, 0x273F5D},
		{VisualScriptNodeKind::Latent, 0x3A3A5C},
		{VisualScriptNodeKind::Pure, 0x264537},
		{VisualScriptNodeKind::Variable, 0x1D444A},
		{VisualScriptNodeKind::Function, 0x463655},
		{VisualScriptNodeKind::Data, 0x38423C},
		{VisualScriptNodeKind::Unknown, 0x472E32},
	}};
	for (Expected const& expected : gradientEnds)
	{
		uint32_t const header = GetVisualScriptHeaderColor(expected.Kind, "");
		VisualScriptNodeColors const colors = ComputeVisualScriptNodeColors(header, theme);
		CAPTURE(Describe(header));
		CHECK(colors.Header == header);
		CHECK(colors.HeaderEnd == Hex(expected.HeaderEnd));
		CHECK(colors.HeaderEnd == MixVisualScriptColors(header, theme.Body, 0.65f));
		CHECK(colors.Box == MixVisualScriptColors(header, theme.Body, 0.25f));
		CHECK(colors.Subtitle == MixVisualScriptColors(theme.Text, header, 0.10f));
	}
	CHECK(GetVisualScriptPinLabelColor(theme) == Hex(0xCED1D6));
}

TEST_CASE("VisualScriptEditor: every header keeps its title and subtitle readable")
{
	VisualScriptThemeAnchors const theme;
	for (uint32_t const header : GetHeaders())
	{
		VisualScriptNodeColors const colors = ComputeVisualScriptNodeColors(header, theme);
		CAPTURE(Describe(header));
		CHECK(GetVisualScriptContrast(theme.Text, colors.Header) >= 4.5f);
		CHECK(GetVisualScriptContrast(colors.Subtitle, colors.Header) >= 4.5f);
		CHECK(GetVisualScriptContrast(theme.Text, colors.HeaderEnd) >= 4.5f);
		CHECK(GetVisualScriptContrast(colors.Subtitle, colors.HeaderEnd) >= 4.5f);
		// Titles on the boxes nodes become at Overview.
		CHECK(GetVisualScriptContrast(theme.Text, colors.Box) >= 4.5f);
	}
	CHECK(GetVisualScriptContrast(GetVisualScriptPinLabelColor(theme), theme.Body) >= 4.5f);
}

TEST_CASE("VisualScriptEditor: type kinds have the pin colours of the design, which stand out on bodies and on the canvas")
{
	VisualScriptThemeAnchors const theme;
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Exec) == theme.Text);
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Bool) == Hex(0xF1655A));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Integer) == Hex(0x57D8B4));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Float) == Hex(0xBAF06D));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::String) == Hex(0xF779BA));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Vector) == Hex(0xEFAD32));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Quaternion) == Hex(0xB2A6EC));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Color) == Hex(0xF8894B));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Matrix) == Hex(0xA88D69));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Entity) == Hex(0x4BA3F7));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Component) == Hex(0xA4D8FE));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Asset) == Hex(0xC57EDA));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Enum) == Hex(0x389C6A));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Struct) == Hex(0x6482AA));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Class) == Hex(0xD9A3D3));
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Object) == Hex(0xA8ABB0));
	// An array's colour is its element's, which only the type knows.
	CHECK(GetVisualScriptTypeColor(VisualScriptTypeKind::Array) == VisualScriptWildcardColor);
	CHECK(GetVisualScriptTypeColor(static_cast<VisualScriptTypeKind>(200)) == VisualScriptWildcardColor);

	for (uint32_t const color : GetTypeColors())
	{
		CAPTURE(Describe(color));
		CHECK(GetVisualScriptContrast(color, theme.Body) >= 3.0f);
		CHECK(GetVisualScriptContrast(color, theme.Canvas) >= 3.0f);
	}
}

TEST_CASE("VisualScriptEditor: a pin's type finds its colour through the node library")
{
	Testing::FixtureVisualScriptSemantics semantics;
	VisualScriptLibrary const& library = semantics.GetFixtureLibrary();

	CHECK(GetVisualScriptTypeColor(VisualScriptExecType, &library) == GetVisualScriptTypeColor(VisualScriptTypeKind::Exec));
	CHECK(GetVisualScriptTypeColor(VisualScriptExecType, nullptr) == GetVisualScriptTypeColor(VisualScriptTypeKind::Exec));
	CHECK(GetVisualScriptTypeColor("float", &library) == GetVisualScriptTypeColor(VisualScriptTypeKind::Float));
	CHECK(GetVisualScriptTypeColor("ulong", &library) == GetVisualScriptTypeColor(VisualScriptTypeKind::Integer));
	CHECK(GetVisualScriptTypeColor("Strada.Vector3", &library) == GetVisualScriptTypeColor(VisualScriptTypeKind::Vector));
	CHECK(GetVisualScriptTypeColor("Strada.TransformComponent", &library) == GetVisualScriptTypeColor(VisualScriptTypeKind::Component));
	CHECK(GetVisualScriptTypeColor("Strada.KeyCode", &library) == GetVisualScriptTypeColor(VisualScriptTypeKind::Enum));
	CHECK(GetVisualScriptTypeColor("Strada.Prefab", &library) == GetVisualScriptTypeColor(VisualScriptTypeKind::Asset));
	// Arrays take their element's colour.
	REQUIRE(library.FindType("Strada.Entity[]") != nullptr);
	CHECK(GetVisualScriptTypeColor("Strada.Entity[]", &library) == GetVisualScriptTypeColor(VisualScriptTypeKind::Entity));
	// Unresolved wildcards, types the library lacks, and everything but exec without a library.
	CHECK(GetVisualScriptTypeColor(VisualScriptWildcardType, &library) == VisualScriptWildcardColor);
	CHECK(GetVisualScriptTypeColor("Game.Missing", &library) == VisualScriptWildcardColor);
	CHECK(GetVisualScriptTypeColor("", &library) == VisualScriptWildcardColor);
	CHECK(GetVisualScriptTypeColor("float", nullptr) == VisualScriptWildcardColor);
}

TEST_CASE("VisualScriptEditor: the frequent types stay apart in normal vision and under colour-vision deficiencies")
{
	for (size_t first = 0; first < FrequentTypeKinds.size(); first++)
	{
		for (size_t second = first + 1; second < FrequentTypeKinds.size(); second++)
		{
			uint32_t const a = GetVisualScriptTypeColor(FrequentTypeKinds[first]);
			uint32_t const b = GetVisualScriptTypeColor(FrequentTypeKinds[second]);
			CAPTURE(Describe(a));
			CAPTURE(Describe(b));
			CHECK(GetVisualScriptColorDistance(a, b) >= 0.05f);
			for (Vision const& vision : Deficiencies)
			{
				CAPTURE(vision.Name);
				CHECK(GetVisualScriptColorDistance(Simulate(vision.Matrix, ToLinear(a)), Simulate(vision.Matrix, ToLinear(b))) >= 0.05f);
			}
		}
	}
}

TEST_CASE("VisualScriptEditor: state rings stand apart from every type colour they surround")
{
	VisualScriptThemeAnchors const theme;
	std::array<uint32_t, 3> const states = {theme.Selection, VisualScriptErrorColor, VisualScriptWarningColor};
	for (uint32_t const state : states)
	{
		for (uint32_t const type : GetTypeColors())
		{
			uint32_t const ring = GetVisualScriptRingColor(state, type, theme);
			CAPTURE(Describe(state));
			CAPTURE(Describe(type));
			CHECK(GetVisualScriptColorDistance(ring, type) >= VisualScriptRingMinDistance);
			CHECK(ring == (GetVisualScriptColorDistance(state, type) >= VisualScriptRingMinDistance ? state : theme.Text));
		}
	}
	// The pairs that are nearly the same colour take the theme's Text; others keep their state's colour.
	CHECK(GetVisualScriptRingColor(theme.Selection, GetVisualScriptTypeColor(VisualScriptTypeKind::Entity), theme) == theme.Text);
	CHECK(GetVisualScriptRingColor(VisualScriptErrorColor, GetVisualScriptTypeColor(VisualScriptTypeKind::Bool), theme) == theme.Text);
	CHECK(GetVisualScriptRingColor(VisualScriptWarningColor, GetVisualScriptTypeColor(VisualScriptTypeKind::Vector), theme) == theme.Text);
	CHECK(GetVisualScriptRingColor(theme.Selection, GetVisualScriptTypeColor(VisualScriptTypeKind::Float), theme) == theme.Selection);
}

TEST_CASE("VisualScriptEditor: the text of disabled and development-only nodes stays readable on what is drawn behind it")
{
	VisualScriptThemeAnchors const theme;
	uint32_t const pinLabel = GetVisualScriptPinLabelColor(theme);
	for (uint32_t const header : GetHeaders())
	{
		VisualScriptNodeColors const colors = ComputeVisualScriptNodeColors(header, theme);
		CAPTURE(Describe(header));
		// A disabled node fades its header (across its whole gradient) and its body, but not its text.
		for (float const along : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
		{
			uint32_t const background = Faded(MixVisualScriptColors(colors.Header, colors.HeaderEnd, along), theme);
			CAPTURE(along);
			CHECK(GetVisualScriptContrast(theme.Text, background) >= 4.5f);
			CHECK(GetVisualScriptContrast(colors.Subtitle, background) >= 4.5f);
		}
		CHECK(GetVisualScriptContrast(pinLabel, Faded(theme.Body, theme)) >= 4.5f);
	}
	// The strip labels ("Disabled", "Development Only") sit on solid pills of the body's colour; a development-only node is
	// otherwise drawn at full opacity.
	CHECK(GetVisualScriptContrast(theme.Text, theme.Body) >= 4.5f);
	// Why text keeps its opacity: faded with the rest of the node, a title falls below 4.5:1.
	uint32_t const fadedHeader = Faded(GetVisualScriptHeaderColor(VisualScriptNodeKind::Pure, ""), theme);
	CHECK(
		GetVisualScriptContrast(CompositeVisualScriptColors(MultiplyVisualScriptAlpha(theme.Text, VisualScriptDisabledAlpha), fadedHeader),
	                            fadedHeader) < 4.5f);
}

TEST_CASE("VisualScriptEditor: a value chip's default value stays readable on the chip")
{
	VisualScriptThemeAnchors const theme;
	uint32_t const text = CompositeVisualScriptColors(MultiplyVisualScriptAlpha(theme.Text, VisualScriptDefaultValueAlpha), theme.Frame);
	CHECK(GetVisualScriptContrast(text, theme.Frame) >= 4.5f);
}

TEST_CASE("VisualScriptEditor: the comment swatches are the design's, the first being a new comment's colour")
{
	struct Expected
	{
		std::string_view Name;
		glm::vec3 Color;
	};
	constexpr std::array<Expected, 8> swatches = {{
		{"Grey", {0.50f, 0.50f, 0.50f}},
		{"Blue", {0.20f, 0.45f, 0.85f}},
		{"Green", {0.25f, 0.65f, 0.30f}},
		{"Yellow", {0.85f, 0.70f, 0.15f}},
		{"Orange", {0.90f, 0.45f, 0.15f}},
		{"Red", {0.80f, 0.20f, 0.20f}},
		{"Purple", {0.55f, 0.30f, 0.80f}},
		{"Teal", {0.15f, 0.60f, 0.60f}},
	}};
	REQUIRE(VisualScriptCommentSwatches.size() == swatches.size());
	for (size_t index = 0; index < swatches.size(); index++)
	{
		CAPTURE(swatches[index].Name);
		CHECK(VisualScriptCommentSwatches[index].Name == swatches[index].Name);
		CHECK(VisualScriptCommentSwatches[index].Color == glm::vec4(swatches[index].Color, 0.25f));
	}
	CHECK(VisualScriptCommentSwatches.front().Color == VisualScriptComment().Color);
}

TEST_CASE("VisualScriptEditor: comment boxes keep their titles readable on every colour")
{
	VisualScriptThemeAnchors const theme;
	auto const checkColors = [&theme](glm::vec4 const& linear)
	{
		CAPTURE(linear.r);
		CAPTURE(linear.g);
		CAPTURE(linear.b);
		CAPTURE(linear.a);
		VisualScriptCommentColors const colors = ComputeVisualScriptCommentColors(linear, theme);
		// Documents store linear colours; ImGui draws sRGB values.
		uint32_t const red = ToByte(Math::LinearToSrgb(linear.r));
		uint32_t const green = ToByte(Math::LinearToSrgb(linear.g));
		uint32_t const blue = ToByte(Math::LinearToSrgb(linear.b));
		CHECK(colors.Body == VisualScriptRgba(red, green, blue, ToByte(linear.a)));
		CHECK(colors.Border == VisualScriptRgba(red, green, blue, 153));
		CHECK(GetAlpha(colors.Bar) == 255u);
		CHECK((colors.BarText == theme.Text || colors.BarText == theme.Border));
		CHECK(GetVisualScriptContrast(colors.BarText, colors.Bar) >= 4.5f);
		return colors;
	};

	// The swatches the editor offers.
	for (VisualScriptCommentSwatch const& swatch : VisualScriptCommentSwatches)
	{
		CAPTURE(swatch.Name);
		checkColors(swatch.Color);
	}

	// The bar is the colour over the canvas at 60 % (the rounding of the body's bytes aside). Yellow's is light enough for the
	// dark text as it is; grey's suits neither text, so it is mixed toward the canvas for the light one.
	VisualScriptCommentColors const yellow = checkColors(glm::vec4(0.85f, 0.70f, 0.15f, 0.25f));
	uint32_t const yellowOverCanvas = CompositeVisualScriptColors(MultiplyVisualScriptAlpha(yellow.Body | 0xFF000000u, 0.6f), theme.Canvas);
	CHECK(IsWithinRounding(yellow.Bar, yellowOverCanvas));
	CHECK(yellow.BarText == theme.Border);
	VisualScriptCommentColors const grey = checkColors(glm::vec4(0.5f, 0.5f, 0.5f, 0.25f));
	uint32_t const greyOverCanvas = CompositeVisualScriptColors(MultiplyVisualScriptAlpha(grey.Body | 0xFF000000u, 0.6f), theme.Canvas);
	CHECK(GetVisualScriptContrast(theme.Text, greyOverCanvas) < 4.4f);
	CHECK(GetVisualScriptContrast(theme.Border, greyOverCanvas) < 4.4f);
	CHECK(grey.BarText == theme.Text);
	CHECK(GetVisualScriptContrast(grey.Bar, theme.Canvas) < GetVisualScriptContrast(greyOverCanvas, theme.Canvas));
	// An opaque colour keeps its alpha for the bar.
	VisualScriptCommentColors const opaque = checkColors(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
	CHECK(opaque.Bar == Hex(0x000000));
	CHECK(opaque.BarText == theme.Text);

	Testing::FuzzRandom random(10500);
	auto const unit = [&random]()
	{
		return static_cast<float>(random.Next() >> 40) / static_cast<float>(1u << 24);
	};
	for (int index = 0; index < 1000; index++)
	{
		glm::vec4 const color(unit(), unit(), unit(), unit());
		checkColors(color);
	}

	// Channels outside 0 to 1, NaN included, are clamped.
	float const nan = std::numeric_limits<float>::quiet_NaN();
	VisualScriptCommentColors const damaged = ComputeVisualScriptCommentColors(glm::vec4(2.0f, -1.0f, nan, 5.0f), theme);
	VisualScriptCommentColors const clamped = ComputeVisualScriptCommentColors(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), theme);
	CHECK(damaged.Body == clamped.Body);
	CHECK(damaged.Bar == clamped.Bar);
	CHECK(damaged.BarText == clamped.BarText);
	CHECK(damaged.Border == clamped.Border);
}
