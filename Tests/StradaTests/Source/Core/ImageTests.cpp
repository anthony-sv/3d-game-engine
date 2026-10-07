#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"

#include <doctest/doctest.h>

#include <array>
#include <string>

using namespace Strada;

namespace
{
	Image MakeGradient(uint32_t width, uint32_t height)
	{
		Image image(width, height, 4);
		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				uint8_t* pixel = image.GetPixel(x, y);
				pixel[0] = static_cast<uint8_t>(x * 255 / std::max(1u, width - 1));
				pixel[1] = static_cast<uint8_t>(y * 255 / std::max(1u, height - 1));
				pixel[2] = static_cast<uint8_t>((x + y) % 256);
				pixel[3] = 255;
			}
		}
		return image;
	}
}

TEST_CASE("Image: construction allocates zeroed, tightly packed pixels")
{
	Image const image(5, 3, 4);
	CHECK(image.GetWidth() == 5);
	CHECK(image.GetHeight() == 3);
	CHECK(image.GetChannels() == 4);
	REQUIRE(image.GetPixels().size() == 5 * 3 * 4);
	for (uint8_t const value : image.GetPixels())
	{
		CHECK(value == 0);
	}
	CHECK(image.GetPixel(1, 2) == image.GetPixels().data() + (2 * 5 + 1) * 4);
	CHECK(Image().IsEmpty());
}

TEST_CASE("Image: PNG round trip preserves every pixel")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "gradient.png";

	Image const original = MakeGradient(37, 21);
	REQUIRE(original.WritePNG(path).IsOk());

	Result<Image> loaded = Image::LoadFromFile(path);
	REQUIRE(loaded.IsOk());
	CHECK(loaded.GetValue().GetWidth() == 37);
	CHECK(loaded.GetValue().GetHeight() == 21);
	CHECK(loaded.GetValue().GetChannels() == 4);
	CHECK(loaded.GetValue().GetPixels() == original.GetPixels());
}

TEST_CASE("Image: channel conversion on load")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "rgba.png";
	REQUIRE(MakeGradient(4, 4).WritePNG(path).IsOk());

	Result<Image> rgb = Image::LoadFromFile(path, 3);
	REQUIRE(rgb.IsOk());
	CHECK(rgb.GetValue().GetChannels() == 3);
	CHECK(rgb.GetValue().GetPixels().size() == 4 * 4 * 3);

	Result<Image> native = Image::LoadFromFile(path, 0);
	REQUIRE(native.IsOk());
	CHECK(native.GetValue().GetChannels() == 4);

	CHECK(Image::LoadFromFile(path, 5).IsError());
}

TEST_CASE("Image: non-ASCII paths are supported")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / FileSystem::PathFromUtf8("Bild \xC3\xBC\xE6\x97\xA5.png");
	REQUIRE(MakeGradient(8, 8).WritePNG(path).IsOk());
	CHECK(Image::LoadFromFile(path).IsOk());
}

TEST_CASE("Image: invalid input reports errors instead of crashing")
{
	std::array<uint8_t, 16> const garbage = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
	Result<Image> const decoded = Image::LoadFromMemory(garbage);
	CHECK(decoded.IsError());
	CHECK_FALSE(decoded.GetError().empty());

	CHECK(Image::LoadFromMemory({}).IsError());

	Testing::TemporaryDirectory directory;
	CHECK(Image::LoadFromFile(directory.GetPath() / "missing.png").IsError());
	CHECK(Image().WritePNG(directory.GetPath() / "empty.png").IsError());
}
