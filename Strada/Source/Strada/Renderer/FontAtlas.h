#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

namespace Strada
{
	class FontAsset;

	// A glyph of a FontAtlas.
	struct FontGlyph
	{
		// Pen advance to the next glyph, in lines.
		float Advance = 0.0f;
		// The glyph's quad relative to the pen on the baseline (x right, y up), in lines, including the distance-field
		// margin. Empty for glyphs without an outline (spaces).
		glm::vec2 PlaneMin = glm::vec2(0.0f);
		glm::vec2 PlaneMax = glm::vec2(0.0f);
		// The quad's pixels in the atlas image (minimum inclusive, maximum exclusive). Stable while the atlas lives.
		glm::uvec2 AtlasMin = glm::uvec2(0);
		glm::uvec2 AtlasMax = glm::uvec2(0);

		bool HasQuad() const { return AtlasMax.x > AtlasMin.x && AtlasMax.y > AtlasMin.y; }
	};

	// Signed distance fields of a font's glyphs packed into one single-channel image, rendered on first use of each
	// character, so text stays sharp at any size and any character the font has can be drawn. Metrics are in lines: 1 is
	// the font's line height (ascent - descent + line gap). The image grows when it is full, keeping glyph positions.
	// CPU only: the renderer uploads the image whenever GetVersion changes. Not thread-safe.
	class FontAtlas
	{
		struct Data;
		struct PrivateTag
		{
		};

	public:
		// Glyphs are rendered for a line height of this many atlas pixels.
		static constexpr float PixelsPerLine = 64.0f;
		// The distance field extends this many atlas pixels inside and outside each outline.
		static constexpr int32_t DistanceRange = 6;
		// The stored value (0-255) on outlines; values above are inside.
		static constexpr uint8_t EdgeValue = 128;
		static constexpr uint32_t InitialSize = 512;
		static constexpr uint32_t MaxSize = 4096;

		// Fails when the font data cannot be read. Renders the printable ASCII characters right away.
		[[nodiscard]] static Result<Scope<FontAtlas>> Create(Ref<FontAsset> font);

		FontAtlas(PrivateTag, Scope<Data> data);
		~FontAtlas();

		FontAtlas(FontAtlas const&) = delete;
		FontAtlas& operator=(FontAtlas const&) = delete;

		// Whether the font has a glyph for the code point.
		bool HasGlyph(char32_t codepoint) const;
		// The glyph of a code point, rendered into the atlas on first use. Code points the font lacks use U+FFFD, or '?'
		// when it lacks that too. When the atlas is full, new glyphs keep their advance but have no quad (logged once).
		FontGlyph const& GetGlyph(char32_t codepoint);
		// Adjustment of the advance between two consecutive code points (usually negative), in lines.
		float GetKerning(char32_t left, char32_t right);
		// The baseline's distance below the top of a line, in lines.
		float GetAscent() const;

		uint32_t GetWidth() const;
		uint32_t GetHeight() const;
		// GetWidth() * GetHeight() bytes, rows from the top.
		std::span<uint8_t const> GetPixels() const;
		// Changes whenever the image changes.
		uint64_t GetVersion() const;

		// Atlas pixels per unit of a stored value divided by 255 (for shaders sampling the image as UNORM).
		static constexpr float GetDistanceScale() { return 255.0f * static_cast<float>(DistanceRange) / static_cast<float>(EdgeValue); }
		// The stored outline value as a UNORM sample.
		static constexpr float GetEdgeSample() { return static_cast<float>(EdgeValue) / 255.0f; }

	private:
		Scope<Data> m_Data;
	};
}
