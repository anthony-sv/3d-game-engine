#include "Editor/UI/EditorWindowIcon.h"

#include "Strada/Core/EmbeddedResources.h"
#include "Strada/Core/Image.h"
#include "Strada/RHI/GraphicsDevice.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace Strada;

namespace
{
	using Color = std::array<uint8_t, 4>;

	// An RGBA image of the given pixels, row by row.
	Image MakeImage(uint32_t width, uint32_t height, std::vector<Color> const& pixels)
	{
		REQUIRE(pixels.size() == static_cast<size_t>(width) * height);
		Image image(width, height, 4);
		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				Color const& color = pixels[static_cast<size_t>(y) * width + x];
				std::copy(color.begin(), color.end(), image.GetPixel(x, y));
			}
		}
		return image;
	}

	Color PixelOf(Image const& image, uint32_t x, uint32_t y)
	{
		uint8_t const* const pixel = image.GetPixel(x, y);
		return Color{pixel[0], pixel[1], pixel[2], pixel[3]};
	}

	constexpr Color Red = {255, 0, 0, 255};
	constexpr Color Green = {0, 255, 0, 255};
	constexpr Color Blue = {0, 0, 255, 255};
	constexpr Color White = {255, 255, 255, 255};
}

TEST_CASE("EditorWindowIcon: a scaled image's pixels are the averages of the pixels they cover")
{
	SUBCASE("whole pixels")
	{
		// Four blocks of two by two.
		Image const image =
			MakeImage(4, 4, {Red, Red, Green, Green, Red, Red, Green, Green, Blue, Blue, White, White, Blue, Blue, White, White});
		Result<Image> const scaled = EditorWindowIcon::Scale(image, 2);
		REQUIRE(scaled);
		CHECK(scaled.GetValue().GetWidth() == 2);
		CHECK(scaled.GetValue().GetHeight() == 2);
		CHECK(scaled.GetValue().GetChannels() == 4);
		CHECK(PixelOf(scaled.GetValue(), 0, 0) == Red);
		CHECK(PixelOf(scaled.GetValue(), 1, 0) == Green);
		CHECK(PixelOf(scaled.GetValue(), 0, 1) == Blue);
		CHECK(PixelOf(scaled.GetValue(), 1, 1) == White);

		Image const four =
			MakeImage(2, 2, {Color{0, 10, 20, 255}, Color{100, 10, 40, 255}, Color{200, 10, 60, 255}, Color{60, 10, 80, 255}});
		Result<Image> const one = EditorWindowIcon::Scale(four, 1);
		REQUIRE(one);
		CHECK(PixelOf(one.GetValue(), 0, 0) == Color{90, 10, 50, 255});
	}

	SUBCASE("pixels covered in part count by that part")
	{
		// Three pixels in two: each has one whole pixel and half of the middle one.
		Image const three = MakeImage(3, 1, {Color{30, 0, 0, 255}, Color{90, 0, 0, 255}, Color{150, 0, 0, 255}});
		Result<Image> const scaled = EditorWindowIcon::Scale(three, 2);
		REQUIRE(scaled);
		CHECK(PixelOf(scaled.GetValue(), 0, 0) == Color{50, 0, 0, 255});
		CHECK(PixelOf(scaled.GetValue(), 1, 0) == Color{130, 0, 0, 255});
		// The one row fills both.
		CHECK(PixelOf(scaled.GetValue(), 0, 1) == Color{50, 0, 0, 255});
		CHECK(PixelOf(scaled.GetValue(), 1, 1) == Color{130, 0, 0, 255});
	}

	SUBCASE("what is transparent lends no color")
	{
		// An opaque blue pixel beside a transparent red one: blue, half opaque.
		Result<Image> const edge = EditorWindowIcon::Scale(MakeImage(2, 1, {Blue, Color{255, 0, 0, 0}}), 1);
		REQUIRE(edge);
		CHECK(PixelOf(edge.GetValue(), 0, 0) == Color{0, 0, 255, 128});
		// Colors weigh as much as they are opaque.
		Result<Image> const mixed = EditorWindowIcon::Scale(MakeImage(2, 1, {Color{200, 0, 0, 255}, Color{0, 0, 0, 85}}), 1);
		REQUIRE(mixed);
		CHECK(PixelOf(mixed.GetValue(), 0, 0) == Color{150, 0, 0, 170});
		Image const clear = MakeImage(2, 2, {Color{255, 0, 0, 0}, Color{0, 255, 0, 0}, Color{0, 0, 255, 0}, Color{9, 9, 9, 0}});
		Result<Image> const nothing = EditorWindowIcon::Scale(clear, 1);
		REQUIRE(nothing);
		CHECK(PixelOf(nothing.GetValue(), 0, 0) == Color{0, 0, 0, 0});
	}

	SUBCASE("the same size and larger sizes keep the pixels")
	{
		Image const image = MakeImage(2, 2, {Red, Green, Blue, Color{10, 20, 30, 40}});
		Result<Image> const same = EditorWindowIcon::Scale(image, 2);
		REQUIRE(same);
		CHECK(same.GetValue().GetPixels() == image.GetPixels());
		Result<Image> const larger = EditorWindowIcon::Scale(image, 4);
		REQUIRE(larger);
		CHECK(PixelOf(larger.GetValue(), 0, 0) == Red);
		CHECK(PixelOf(larger.GetValue(), 1, 1) == Red);
		CHECK(PixelOf(larger.GetValue(), 2, 1) == Green);
		CHECK(PixelOf(larger.GetValue(), 1, 2) == Blue);
		CHECK(PixelOf(larger.GetValue(), 3, 3) == Color{10, 20, 30, 40});
	}
}

TEST_CASE("EditorWindowIcon: it refuses what is no image to scale and sizes it does not make")
{
	Image const image = MakeImage(1, 1, {White});
	CHECK_FALSE(EditorWindowIcon::Scale(Image(), 16));
	CHECK_FALSE(EditorWindowIcon::Scale(Image(4, 4, 3), 16));
	CHECK_FALSE(EditorWindowIcon::Scale(image, 0));
	CHECK(EditorWindowIcon::Scale(image, EditorWindowIcon::MaxSide));
	CHECK_FALSE(EditorWindowIcon::Scale(image, EditorWindowIcon::MaxSide + 1));
}

TEST_CASE("EditorWindowIcon: the Strada icon scales to the sizes of a title bar")
{
	Result<Image> const icon = Image::LoadFromMemory(EmbeddedResources::Get(EmbeddedResources::DefaultIcon), 4);
	REQUIRE(icon);
	// 100 %, 125 %, 175 % and 200 % of 16 pixels.
	for (uint32_t const side : {16u, 20u, 28u, 32u})
	{
		CAPTURE(side);
		Result<Image> const scaled = EditorWindowIcon::Scale(icon.GetValue(), side);
		REQUIRE(scaled);
		CHECK(scaled.GetValue().GetWidth() == side);
		CHECK(scaled.GetValue().GetHeight() == side);
		// The picture fills the icon's middle.
		CHECK(PixelOf(scaled.GetValue(), side / 2, side / 2)[3] == 255);
	}
}

TEST_CASE("EditorWindowIcon: without a graphics device there is no image")
{
	REQUIRE_FALSE(GraphicsDevice::IsInitialized());
	EditorWindowIcon icon;
	CHECK(icon.Get(16) == nullptr);
	CHECK(icon.Get(0) == nullptr);
	icon.Release();
	CHECK(icon.Get(28) == nullptr);
}
