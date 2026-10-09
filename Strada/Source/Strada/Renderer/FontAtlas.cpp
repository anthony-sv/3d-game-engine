#include "stpch.h"
#include "Strada/Renderer/FontAtlas.h"

#include "Strada/Asset/FontAsset.h"
#include "Strada/Core/Utf8.h"

#include <stb_truetype.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Strada
{
	namespace
	{
		// Empty pixels between glyphs, so filtering never mixes neighbors.
		constexpr uint32_t GlyphGap = 1;
	}

	struct FontAtlas::Data
	{
		// Keeps the font data stb_truetype reads alive.
		Ref<FontAsset> Font;
		stbtt_fontinfo Info{};
		// Font units per line, and atlas pixels per font unit.
		float UnitsPerLine = 1.0f;
		float Scale = 1.0f;
		float Ascent = 0.0f;
		// Node-based: references to glyphs stay valid while more are added.
		std::unordered_map<char32_t, FontGlyph> Glyphs;
		std::unordered_map<uint64_t, float> Kerning;

		std::vector<uint8_t> Pixels;
		uint32_t Width = 0;
		uint32_t Height = 0;
		// Shelf packing: glyphs fill rows (shelves) from left to right.
		uint32_t ShelfY = 0;
		uint32_t ShelfHeight = 0;
		uint32_t CursorX = 0;
		uint64_t Version = 1;
		bool FullReported = false;

		int FindGlyphIndex(char32_t codepoint) const
		{
			return codepoint > 0x10FFFF ? 0 : stbtt_FindGlyphIndex(&Info, static_cast<int>(codepoint));
		}

		// Doubles the image (keeping its content at the same place); false when it is at the maximum size.
		bool Grow()
		{
			if (Width >= MaxSize && Height >= MaxSize)
			{
				return false;
			}
			uint32_t const width = std::min(Width * 2, MaxSize);
			uint32_t const height = std::min(Height * 2, MaxSize);
			std::vector<uint8_t> pixels(static_cast<size_t>(width) * height, 0);
			for (uint32_t row = 0; row < Height; row++)
			{
				std::memcpy(pixels.data() + static_cast<size_t>(row) * width, Pixels.data() + static_cast<size_t>(row) * Width, Width);
			}
			Pixels = std::move(pixels);
			Width = width;
			Height = height;
			Version++;
			return true;
		}

		std::optional<glm::uvec2> Allocate(uint32_t width, uint32_t height)
		{
			while (true)
			{
				if (CursorX + width > Width && CursorX > 0)
				{
					ShelfY += ShelfHeight + GlyphGap;
					ShelfHeight = 0;
					CursorX = 0;
				}
				if (CursorX + width <= Width && ShelfY + height <= Height)
				{
					glm::uvec2 const position(CursorX, ShelfY);
					CursorX += width + GlyphGap;
					ShelfHeight = std::max(ShelfHeight, height);
					return position;
				}
				if (!Grow())
				{
					return std::nullopt;
				}
			}
		}

		FontGlyph Render(int glyphIndex)
		{
			FontGlyph glyph;
			int advance = 0;
			int bearing = 0;
			stbtt_GetGlyphHMetrics(&Info, glyphIndex, &advance, &bearing);
			glyph.Advance = static_cast<float>(advance) / UnitsPerLine;

			int width = 0;
			int height = 0;
			int offsetX = 0;
			int offsetY = 0;
			constexpr float PixelDistanceScale = static_cast<float>(EdgeValue) / static_cast<float>(DistanceRange);
			unsigned char* field = stbtt_GetGlyphSDF(&Info, Scale, glyphIndex, DistanceRange, EdgeValue, PixelDistanceScale, &width,
			                                         &height, &offsetX, &offsetY);
			if (field == nullptr || width <= 0 || height <= 0)
			{
				// No outline (spaces).
				stbtt_FreeSDF(field, nullptr);
				return glyph;
			}
			std::unique_ptr<unsigned char, void (*)(unsigned char*)> const owner(field,
			                                                                     [](unsigned char* pixels)
			                                                                     {
																					 stbtt_FreeSDF(pixels, nullptr);
																				 });

			std::optional<glm::uvec2> const position = Allocate(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
			if (!position)
			{
				if (!FullReported)
				{
					ST_CORE_WARN("A font atlas is full ({0}x{0}); further characters are not drawn", MaxSize);
					FullReported = true;
				}
				return glyph;
			}
			for (int row = 0; row < height; row++)
			{
				std::memcpy(Pixels.data() + static_cast<size_t>(position->y + static_cast<uint32_t>(row)) * Width + position->x,
				            field + static_cast<size_t>(row) * static_cast<size_t>(width), static_cast<size_t>(width));
			}
			Version++;

			glyph.AtlasMin = *position;
			glyph.AtlasMax = *position + glm::uvec2(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
			// The image's offset is from the pen to its top-left corner with y down; plane coordinates have y up.
			glyph.PlaneMin = glm::vec2(static_cast<float>(offsetX), -static_cast<float>(offsetY + height)) / PixelsPerLine;
			glyph.PlaneMax = glm::vec2(static_cast<float>(offsetX + width), -static_cast<float>(offsetY)) / PixelsPerLine;
			return glyph;
		}
	};

	Result<Scope<FontAtlas>> FontAtlas::Create(Ref<FontAsset> font)
	{
		if (!font)
		{
			return Error{"no font"};
		}
		Scope<Data> data = CreateScope<Data>();
		unsigned char const* bytes = font->GetData().data();
		int const offset = stbtt_GetFontOffsetForIndex(bytes, 0);
		if (offset < 0 || stbtt_InitFont(&data->Info, bytes, offset) == 0)
		{
			return Error{"not a TrueType/OpenType font"};
		}
		int ascent = 0;
		int descent = 0;
		int lineGap = 0;
		stbtt_GetFontVMetrics(&data->Info, &ascent, &descent, &lineGap);
		int const unitsPerLine = ascent - descent + lineGap;
		if (ascent <= 0 || unitsPerLine <= 0)
		{
			return Error{"the font has invalid vertical metrics"};
		}
		data->Font = std::move(font);
		data->UnitsPerLine = static_cast<float>(unitsPerLine);
		data->Scale = PixelsPerLine / data->UnitsPerLine;
		data->Ascent = static_cast<float>(ascent) / data->UnitsPerLine;
		data->Width = InitialSize;
		data->Height = InitialSize;
		data->Pixels.assign(static_cast<size_t>(InitialSize) * InitialSize, 0);

		Scope<FontAtlas> atlas = CreateScope<FontAtlas>(PrivateTag{}, std::move(data));
		for (char32_t codepoint = U' '; codepoint <= U'~'; codepoint++)
		{
			(void)atlas->GetGlyph(codepoint);
		}
		return atlas;
	}

	FontAtlas::FontAtlas(PrivateTag, Scope<Data> data)
		: m_Data(std::move(data))
	{
	}

	FontAtlas::~FontAtlas() = default;

	bool FontAtlas::HasGlyph(char32_t codepoint) const
	{
		return m_Data->FindGlyphIndex(codepoint) != 0;
	}

	FontGlyph const& FontAtlas::GetGlyph(char32_t codepoint)
	{
		Data& data = *m_Data;
		if (auto const it = data.Glyphs.find(codepoint); it != data.Glyphs.end())
		{
			return it->second;
		}
		int const glyphIndex = data.FindGlyphIndex(codepoint);
		if (glyphIndex == 0)
		{
			// Missing characters share the replacement glyph; the font's own placeholder (glyph 0) is the last resort.
			if (codepoint != Utf8::ReplacementCharacter && codepoint != U'?')
			{
				char32_t const fallback = HasGlyph(Utf8::ReplacementCharacter) ? Utf8::ReplacementCharacter : U'?';
				FontGlyph const glyph = GetGlyph(fallback);
				return data.Glyphs.emplace(codepoint, glyph).first->second;
			}
			if (codepoint == Utf8::ReplacementCharacter)
			{
				FontGlyph const glyph = GetGlyph(U'?');
				return data.Glyphs.emplace(codepoint, glyph).first->second;
			}
		}
		return data.Glyphs.emplace(codepoint, data.Render(glyphIndex)).first->second;
	}

	float FontAtlas::GetKerning(char32_t left, char32_t right)
	{
		Data& data = *m_Data;
		uint64_t const key = (static_cast<uint64_t>(left) << 32) | static_cast<uint64_t>(right);
		if (auto const it = data.Kerning.find(key); it != data.Kerning.end())
		{
			return it->second;
		}
		int const kerning = stbtt_GetGlyphKernAdvance(&data.Info, data.FindGlyphIndex(left), data.FindGlyphIndex(right));
		float const value = static_cast<float>(kerning) / data.UnitsPerLine;
		data.Kerning.emplace(key, value);
		return value;
	}

	float FontAtlas::GetAscent() const
	{
		return m_Data->Ascent;
	}

	uint32_t FontAtlas::GetWidth() const
	{
		return m_Data->Width;
	}

	uint32_t FontAtlas::GetHeight() const
	{
		return m_Data->Height;
	}

	std::span<uint8_t const> FontAtlas::GetPixels() const
	{
		return m_Data->Pixels;
	}

	uint64_t FontAtlas::GetVersion() const
	{
		return m_Data->Version;
	}
}
