#include "Editor/VisualScripting/VisualScriptTheme.h"

#include "Strada/Math/Math.h"

#include <algorithm>
#include <cmath>

namespace Strada
{
	namespace
	{
		// A colour written as the hex code of its sRGB value, opaque.
		constexpr uint32_t Hex(uint32_t rgb)
		{
			return VisualScriptRgba((rgb >> 16) & 0xFFu, (rgb >> 8) & 0xFFu, rgb & 0xFFu);
		}

		// The headers of a kind whose quieter categories keep its hue with less chroma: × 0.45 for debugging and tests, × 0.70
		// for conversions (all at OKLCH lightness 0.47).
		struct HeaderFamily
		{
			uint32_t Base;
			uint32_t DebugAndTesting;
			uint32_t Conversions;
		};

		constexpr HeaderFamily CallHeaders = {Hex(0x1E5BA1), Hex(0x445C7B), Hex(0x355C8D)};
		constexpr HeaderFamily PureHeaders = {Hex(0x1B6C36), Hex(0x46634B), Hex(0x376742)};
		constexpr HeaderFamily LatentHeaders = {Hex(0x554DA0), Hex(0x57567B), Hex(0x56538C)};
		constexpr uint32_t EventHeader = Hex(0x9E2C2C);
		constexpr uint32_t FlowHeader = Hex(0x555B65);
		constexpr uint32_t VariableHeader = Hex(0x00686C);
		constexpr uint32_t FunctionHeader = Hex(0x76408C);
		constexpr uint32_t DataHeader = Hex(0x4D6242);
		constexpr uint32_t UnknownHeader = Hex(0x782A28);

		// The mixes the node colours are made of (VisualScriptNodeColors).
		constexpr float HeaderEndMix = 0.65f;
		constexpr float BoxMix = 0.25f;
		constexpr float SubtitleMix = 0.10f;
		constexpr float PinLabelMix = 0.12f;

		// Comment boxes: the outline's alpha, the least alpha of the title bar, the contrast its title needs, and the steps by
		// which the bar is mixed toward the canvas until the theme's Text reaches it.
		constexpr float CommentBorderAlpha = 0.6f;
		constexpr float CommentBarMinAlpha = 0.6f;
		constexpr float CommentTitleMinContrast = 4.5f;
		constexpr int CommentBarMixSteps = 20;

		// An array type's elements may be arrays in turn; a library whose arrays named each other must not loop.
		constexpr int MaxArrayDepth = 8;

		uint32_t GetChannel(uint32_t color, int shift)
		{
			return (color >> shift) & 0xFFu;
		}

		// A channel value from 0 to 1 as a byte; values outside are clamped, NaN is 0.
		uint32_t ToByte(double value)
		{
			double const clamped = value >= 0.0 ? std::min(value, 1.0) : 0.0;
			return static_cast<uint32_t>(std::lround(clamped * 255.0));
		}

		// 0 to 1, with NaN as 0.
		float ClampUnit(float value)
		{
			return value >= 0.0f ? std::min(value, 1.0f) : 0.0f;
		}

		glm::vec3 ToLinear(uint32_t color)
		{
			return glm::vec3(Math::SrgbToLinear(static_cast<float>(GetChannel(color, 0)) / 255.0f),
			                 Math::SrgbToLinear(static_cast<float>(GetChannel(color, 8)) / 255.0f),
			                 Math::SrgbToLinear(static_cast<float>(GetChannel(color, 16)) / 255.0f));
		}

		// WCAG 2.x relative luminance.
		double GetLuminance(uint32_t color)
		{
			glm::dvec3 const linear(ToLinear(color));
			return 0.2126 * linear.r + 0.7152 * linear.g + 0.0722 * linear.b;
		}

		// Björn Ottosson's OKLab of a linear sRGB colour.
		glm::dvec3 ToOklab(glm::dvec3 linear)
		{
			double const l = std::cbrt(0.4122214708 * linear.r + 0.5363325363 * linear.g + 0.0514459929 * linear.b);
			double const m = std::cbrt(0.2119034982 * linear.r + 0.6806995451 * linear.g + 0.1073969566 * linear.b);
			double const s = std::cbrt(0.0883024619 * linear.r + 0.2817188376 * linear.g + 0.6299787005 * linear.b);
			return glm::dvec3(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
			                  1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
			                  0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s);
		}

		// The first segment of a slash-separated category: "Components" of "Components/Transform".
		std::string_view GetCategoryRoot(std::string_view category)
		{
			return category.substr(0, category.find('/'));
		}

		uint32_t SelectHeader(HeaderFamily const& family, std::string_view category)
		{
			std::string_view const root = GetCategoryRoot(category);
			if (root == "Debug" || root == "Testing")
			{
				return family.DebugAndTesting;
			}
			if (root == "Conversions")
			{
				return family.Conversions;
			}
			return family.Base;
		}
	}

	uint32_t GetVisualScriptHeaderColor(VisualScriptNodeKind kind, std::string_view category)
	{
		switch (kind)
		{
			case VisualScriptNodeKind::Event:
				return EventHeader;
			case VisualScriptNodeKind::Flow:
			case VisualScriptNodeKind::Reroute:
				return FlowHeader;
			case VisualScriptNodeKind::Call:
				return SelectHeader(CallHeaders, category);
			case VisualScriptNodeKind::Pure:
				return SelectHeader(PureHeaders, category);
			case VisualScriptNodeKind::Latent:
				return SelectHeader(LatentHeaders, category);
			case VisualScriptNodeKind::Variable:
				return VariableHeader;
			case VisualScriptNodeKind::Function:
				return FunctionHeader;
			case VisualScriptNodeKind::Operator:
				return PureHeaders.Base;
			case VisualScriptNodeKind::Data:
				return DataHeader;
			case VisualScriptNodeKind::Unknown:
				return UnknownHeader;
		}
		// A value outside the enumeration is a node of a kind nobody knows.
		return UnknownHeader;
	}

	VisualScriptNodeColors ComputeVisualScriptNodeColors(uint32_t header, VisualScriptThemeAnchors const& theme)
	{
		VisualScriptNodeColors colors;
		colors.Header = header;
		colors.HeaderEnd = MixVisualScriptColors(header, theme.Body, HeaderEndMix);
		colors.Box = MixVisualScriptColors(header, theme.Body, BoxMix);
		colors.Subtitle = MixVisualScriptColors(theme.Text, header, SubtitleMix);
		return colors;
	}

	uint32_t GetVisualScriptPinLabelColor(VisualScriptThemeAnchors const& theme)
	{
		return MixVisualScriptColors(theme.Text, theme.Body, PinLabelMix);
	}

	uint32_t GetVisualScriptTypeColor(VisualScriptTypeKind kind)
	{
		switch (kind)
		{
			case VisualScriptTypeKind::Exec:
				return VisualScriptThemeAnchors{}.Text;
			case VisualScriptTypeKind::Bool:
				return Hex(0xF1655A);
			case VisualScriptTypeKind::Integer:
				return Hex(0x57D8B4);
			case VisualScriptTypeKind::Float:
				return Hex(0xBAF06D);
			case VisualScriptTypeKind::String:
				return Hex(0xF779BA);
			case VisualScriptTypeKind::Vector:
				return Hex(0xEFAD32);
			case VisualScriptTypeKind::Quaternion:
				return Hex(0xB2A6EC);
			case VisualScriptTypeKind::Color:
				return Hex(0xF8894B);
			case VisualScriptTypeKind::Matrix:
				return Hex(0xA88D69);
			case VisualScriptTypeKind::Entity:
				return Hex(0x4BA3F7);
			case VisualScriptTypeKind::Component:
				return Hex(0xA4D8FE);
			case VisualScriptTypeKind::Asset:
				return Hex(0xC57EDA);
			case VisualScriptTypeKind::Enum:
				return Hex(0x389C6A);
			case VisualScriptTypeKind::Struct:
				return Hex(0x6482AA);
			case VisualScriptTypeKind::Class:
				return Hex(0xD9A3D3);
			case VisualScriptTypeKind::Array:
				return VisualScriptWildcardColor;
			case VisualScriptTypeKind::Object:
				return Hex(0xA8ABB0);
		}
		// A value outside the enumeration is a type nobody knows.
		return VisualScriptWildcardColor;
	}

	uint32_t GetVisualScriptTypeColor(std::string_view type, VisualScriptLibrary const* library)
	{
		if (type == VisualScriptExecType)
		{
			return GetVisualScriptTypeColor(VisualScriptTypeKind::Exec);
		}
		if (library == nullptr)
		{
			return VisualScriptWildcardColor;
		}
		VisualScriptTypeInfo const* info = library->FindType(type);
		for (int depth = 0; info != nullptr && info->Kind == VisualScriptTypeKind::Array && depth < MaxArrayDepth; depth++)
		{
			info = library->FindType(info->Element);
		}
		return info != nullptr ? GetVisualScriptTypeColor(info->Kind) : VisualScriptWildcardColor;
	}

	VisualScriptCommentColors ComputeVisualScriptCommentColors(glm::vec4 linearColor, VisualScriptThemeAnchors const& theme)
	{
		// Documents store linear colours, while ImGui draws the sRGB-encoded values it is given.
		double const red = Math::LinearToSrgb(ClampUnit(linearColor.r));
		double const green = Math::LinearToSrgb(ClampUnit(linearColor.g));
		double const blue = Math::LinearToSrgb(ClampUnit(linearColor.b));
		float const alpha = ClampUnit(linearColor.a);

		VisualScriptCommentColors colors;
		colors.Body = VisualScriptRgba(ToByte(red), ToByte(green), ToByte(blue), ToByte(alpha));
		colors.Border = VisualScriptRgba(ToByte(red), ToByte(green), ToByte(blue), ToByte(CommentBorderAlpha));

		// Composited before the colour is rounded to bytes, so the bar is the colour drawn over the canvas as closely as
		// bytes hold it. The contrast is then measured on the bytes the bar is drawn with.
		double const barAlpha = std::max(alpha, CommentBarMinAlpha);
		auto const overCanvas = [barAlpha, &theme](double value, int shift)
		{
			return ToByte(value * barAlpha + static_cast<double>(GetChannel(theme.Canvas, shift)) / 255.0 * (1.0 - barAlpha));
		};
		uint32_t const bar = VisualScriptRgba(overCanvas(red, 0), overCanvas(green, 8), overCanvas(blue, 16));
		float const textContrast = GetVisualScriptContrast(theme.Text, bar);
		float const borderContrast = GetVisualScriptContrast(theme.Border, bar);
		if (std::max(textContrast, borderContrast) >= CommentTitleMinContrast)
		{
			colors.Bar = bar;
			colors.BarText = textContrast >= borderContrast ? theme.Text : theme.Border;
			return colors;
		}
		// The theme's Text on the canvas itself is far above the minimum, so the last step (the canvas) always ends this.
		colors.BarText = theme.Text;
		for (int step = 1; step <= CommentBarMixSteps; step++)
		{
			colors.Bar = MixVisualScriptColors(bar, theme.Canvas, static_cast<float>(step) / static_cast<float>(CommentBarMixSteps));
			if (GetVisualScriptContrast(theme.Text, colors.Bar) >= CommentTitleMinContrast)
			{
				break;
			}
		}
		return colors;
	}

	uint32_t GetVisualScriptRingColor(uint32_t stateColor, uint32_t typeColor, VisualScriptThemeAnchors const& theme)
	{
		return GetVisualScriptColorDistance(stateColor, typeColor) >= VisualScriptRingMinDistance ? stateColor : theme.Text;
	}

	float GetVisualScriptContrast(uint32_t a, uint32_t b)
	{
		double const luminanceA = GetLuminance(a);
		double const luminanceB = GetLuminance(b);
		return static_cast<float>((std::max(luminanceA, luminanceB) + 0.05) / (std::min(luminanceA, luminanceB) + 0.05));
	}

	float GetVisualScriptColorDistance(uint32_t a, uint32_t b)
	{
		return GetVisualScriptColorDistance(ToLinear(a), ToLinear(b));
	}

	float GetVisualScriptColorDistance(glm::vec3 linearA, glm::vec3 linearB)
	{
		glm::dvec3 const clampedA = glm::clamp(glm::dvec3(linearA), glm::dvec3(0.0), glm::dvec3(1.0));
		glm::dvec3 const clampedB = glm::clamp(glm::dvec3(linearB), glm::dvec3(0.0), glm::dvec3(1.0));
		return static_cast<float>(glm::distance(ToOklab(clampedA), ToOklab(clampedB)));
	}

	uint32_t MixVisualScriptColors(uint32_t from, uint32_t to, float amount)
	{
		double const t = static_cast<double>(ClampUnit(amount));
		auto const mixChannel = [from, to, t](int shift)
		{
			double const start = static_cast<double>(GetChannel(from, shift));
			double const end = static_cast<double>(GetChannel(to, shift));
			return static_cast<uint32_t>(std::lround(start + (end - start) * t));
		};
		return VisualScriptRgba(mixChannel(0), mixChannel(8), mixChannel(16), mixChannel(24));
	}

	uint32_t MultiplyVisualScriptAlpha(uint32_t color, float factor)
	{
		double const alpha = static_cast<double>(GetChannel(color, 24)) / 255.0 * static_cast<double>(ClampUnit(factor));
		return (color & 0x00FFFFFFu) | (ToByte(alpha) << 24);
	}

	uint32_t CompositeVisualScriptColors(uint32_t top, uint32_t bottom)
	{
		double const topAlpha = static_cast<double>(GetChannel(top, 24)) / 255.0;
		double const bottomAlpha = static_cast<double>(GetChannel(bottom, 24)) / 255.0;
		double const alpha = topAlpha + bottomAlpha * (1.0 - topAlpha);
		if (alpha <= 0.0)
		{
			return 0;
		}
		auto const blendChannel = [top, bottom, topAlpha, bottomAlpha, alpha](int shift)
		{
			double const topValue = static_cast<double>(GetChannel(top, shift)) / 255.0;
			double const bottomValue = static_cast<double>(GetChannel(bottom, shift)) / 255.0;
			return ToByte((topValue * topAlpha + bottomValue * bottomAlpha * (1.0 - topAlpha)) / alpha);
		};
		return VisualScriptRgba(blendChannel(0), blendChannel(8), blendChannel(16), ToByte(alpha));
	}
}
