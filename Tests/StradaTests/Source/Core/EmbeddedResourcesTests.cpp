#include "Strada/Asset/TrueTypeSanitizer.h"
#include "Strada/Core/EmbeddedResources.h"
#include "Strada/Core/Image.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

using namespace Strada;

TEST_CASE("Core: the embedded icon decodes and the embedded fonts are valid")
{
	Result<Image> const icon = Image::LoadFromMemory(EmbeddedResources::Get(EmbeddedResources::DefaultIcon));
	REQUIRE(icon);
	CHECK(icon.GetValue().GetWidth() == 256);
	CHECK(icon.GetValue().GetHeight() == 256);
	// Transparent corners around the rounded square, an opaque center.
	CHECK(icon.GetValue().GetPixel(0, 0)[3] == 0);
	CHECK(icon.GetValue().GetPixel(128, 128)[3] == 255);

	for (std::string_view const font : {EmbeddedResources::DefaultFont, EmbeddedResources::MonospaceFont})
	{
		CAPTURE(font);
		std::span<uint8_t const> const data = EmbeddedResources::Get(font);
		std::vector<uint8_t> copy(data.begin(), data.end());
		Result<TrueTypeRepairs> const repairs = SanitizeTrueTypeFont(copy);
		REQUIRE(repairs);
		CHECK(repairs.GetValue().IsEmpty());
	}
}
