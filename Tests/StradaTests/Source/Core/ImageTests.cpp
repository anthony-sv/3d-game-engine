#include "Fuzzing.h"
#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

	// An uncompressed 24-bit BMP of black pixels whose header may claim any size (pixel data is capped at 4 KB).
	std::vector<uint8_t> MakeBmp(int32_t width, int32_t height)
	{
		std::vector<uint8_t> bmp(54, 0);
		auto const put = [&bmp](size_t offset, uint32_t value, size_t bytes)
		{
			for (size_t index = 0; index < bytes; index++)
			{
				bmp[offset + index] = static_cast<uint8_t>(value >> (8 * index));
			}
		};
		uint32_t const rowSize = (static_cast<uint32_t>(std::max(width, 0)) * 3 + 3) & ~3u;
		uint32_t const pixelBytes = std::min<uint32_t>(rowSize * static_cast<uint32_t>(std::max(height, 0)), 4096);
		bmp[0] = 'B';
		bmp[1] = 'M';
		put(2, 54 + pixelBytes, 4);
		put(10, 54, 4);
		put(14, 40, 4);
		put(18, static_cast<uint32_t>(width), 4);
		put(22, static_cast<uint32_t>(height), 4);
		put(26, 1, 2);
		put(28, 24, 2);
		put(34, pixelBytes, 4);
		bmp.resize(54 + pixelBytes, 0);
		return bmp;
	}

	std::vector<uint8_t> ToBytes(std::string_view text)
	{
		return std::vector<uint8_t>(text.begin(), text.end());
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

TEST_CASE("Image: PNG encoding in memory decodes to the same pixels")
{
	Image const original = MakeGradient(19, 7);
	Result<std::vector<uint8_t>> encoded = original.EncodePNG();
	REQUIRE(encoded.IsOk());
	REQUIRE(encoded.GetValue().size() > 8);
	// PNG signature.
	CHECK(encoded.GetValue()[0] == 0x89);
	CHECK(encoded.GetValue()[1] == 'P');

	Result<Image> decoded = Image::LoadFromMemory(encoded.GetValue());
	REQUIRE(decoded.IsOk());
	CHECK(decoded.GetValue().GetWidth() == 19);
	CHECK(decoded.GetValue().GetPixels() == original.GetPixels());

	CHECK(Image().EncodePNG().IsError());
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

TEST_CASE("Image: only the engine's formats decode, with pixels and within GPU limits")
{
	Result<Image> const bmp = Image::LoadFromMemory(MakeBmp(3, 2));
	REQUIRE(bmp.IsOk());
	CHECK(bmp.GetValue().GetWidth() == 3);
	CHECK(bmp.GetValue().GetHeight() == 2);

	// stb_image accepts BMP files without a column or row of pixels, which no texture can use.
	for (auto const& [width, height] : {std::pair{0, 2}, std::pair{3, 0}})
	{
		CAPTURE(width);
		CAPTURE(height);
		CHECK(Image::LoadFromMemory(MakeBmp(width, height)).GetError() == "the image has no pixels");
		CHECK(Image::ReadInfo(MakeBmp(width, height)).IsError());
	}
	// Larger than GPUs sample: refused before any pixel is allocated.
	CHECK(Image::LoadFromMemory(MakeBmp(16385, 1)).IsError());
	CHECK(Image::LoadFromMemory(MakeBmp(1, 16385)).IsError());

	// Formats the engine does not read: stb_image's decoders for them are left out.
	CHECK(Image::LoadFromMemory(ToBytes(std::string_view("P6\n1 1\n255\n\x01\x02\x03", 14))).IsError());

	// A Radiance HDR file without rows.
	CHECK(HdrImage::LoadFromMemory(ToBytes("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 0 +X 4\n")).IsError());
}

TEST_CASE("Image: damaged images are refused or decode to pixels")
{
	// Seeds of the formats the engine reads that it can write or build.
	Testing::TemporaryDirectory directory;
	HdrImage sky(16, 8, 3);
	for (size_t index = 0; index < sky.GetPixels().size(); index++)
	{
		sky.GetPixels()[index] = static_cast<float>(index % 37) * 0.75f;
	}
	REQUIRE(sky.WriteHDR(directory.GetPath() / "Sky.hdr").IsOk());
	Result<Buffer> hdr = FileSystem::ReadBinaryFile(directory.GetPath() / "Sky.hdr");
	Result<std::vector<uint8_t>> png = MakeGradient(23, 17).EncodePNG();
	REQUIRE(hdr.IsOk());
	REQUIRE(png.IsOk());
	std::span<uint8_t const> const hdrBytes = hdr.GetValue().GetSpan();
	std::vector<std::vector<uint8_t>> const seeds = {png.GetValue(), MakeBmp(23, 17), {hdrBytes.begin(), hdrBytes.end()}};

	Testing::FuzzRandom random(7000);
	int decoded = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		std::vector<uint8_t> data = seeds[random.Pick(seeds.size())];
		for (size_t count = 1 + random.Pick(4); count > 0 && data.size() > 1; count--)
		{
			switch (random.Pick(4))
			{
				case 0:
					data.resize(1 + random.Pick(data.size()));
					break;
				case 1:
					data[random.Pick(data.size())] = static_cast<uint8_t>(random.Next());
					break;
				case 2:
					// Header fields: sizes, counts, formats.
					data[random.Pick(std::min<size_t>(data.size(), 64))] = std::array<uint8_t, 4>{0x00, 0x7F, 0x80, 0xFF}[random.Pick(4)];
					break;
				default:
					data[random.Pick(data.size())] ^= static_cast<uint8_t>(1u << random.Pick(8));
					break;
			}
		}
		Result<Image> image = Image::LoadFromMemory(data, 4);
		Result<HdrImage> hdrImage = HdrImage::LoadFromMemory(data, 4);
		static_cast<void>(Image::ReadInfo(data));
		if (image)
		{
			Image const& pixels = image.GetValue();
			CHECK(pixels.GetWidth() > 0);
			CHECK(pixels.GetHeight() > 0);
			CHECK(pixels.GetPixels().size() == size_t(pixels.GetWidth()) * pixels.GetHeight() * 4);
		}
		if (hdrImage)
		{
			CHECK(hdrImage.GetValue().GetWidth() > 0);
			CHECK(hdrImage.GetValue().GetHeight() > 0);
		}
		(image || hdrImage ? decoded : refused)++;
	}
	CHECK(decoded > 0);
	CHECK(refused > 0);
}

TEST_CASE("Image: header information is read without decoding")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "Info.png";
	REQUIRE(MakeGradient(7, 3).WritePNG(path).IsOk());
	Result<Buffer> data = FileSystem::ReadBinaryFile(path);
	REQUIRE(data.IsOk());

	Result<ImageInfo> const info = Image::ReadInfo(data.GetValue().GetSpan());
	REQUIRE(info.IsOk());
	CHECK(info.GetValue().Width == 7);
	CHECK(info.GetValue().Height == 3);
	CHECK(info.GetValue().Channels == 4);
	CHECK_FALSE(info.GetValue().IsHdr);

	std::array<uint8_t, 8> const garbage = {1, 2, 3, 4, 5, 6, 7, 8};
	CHECK(Image::ReadInfo(garbage).IsError());
	CHECK(Image::ReadInfo({}).IsError());
}

TEST_CASE("Image: HDR images round trip through Radiance files")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "Sky.hdr";

	HdrImage image(4, 2, 3);
	for (uint32_t y = 0; y < 2; y++)
	{
		for (uint32_t x = 0; x < 4; x++)
		{
			float* pixel = image.GetPixel(x, y);
			pixel[0] = 0.5f * static_cast<float>(x + 1);
			pixel[1] = 16.0f;
			pixel[2] = 0.25f * static_cast<float>(y + 1);
		}
	}
	REQUIRE(image.WriteHDR(path).IsOk());

	Result<Buffer> data = FileSystem::ReadBinaryFile(path);
	REQUIRE(data.IsOk());
	Result<ImageInfo> const info = Image::ReadInfo(data.GetValue().GetSpan());
	REQUIRE(info.IsOk());
	CHECK(info.GetValue().IsHdr);

	Result<HdrImage> const loaded = HdrImage::LoadFromFile(path);
	REQUIRE(loaded.IsOk());
	HdrImage const& decoded = loaded.GetValue();
	REQUIRE(decoded.GetWidth() == 4);
	REQUIRE(decoded.GetHeight() == 2);
	REQUIRE(decoded.GetChannels() == 4);
	// RGBE keeps 8 bits of mantissa per channel relative to the largest component.
	CHECK(decoded.GetPixel(3, 1)[0] == doctest::Approx(2.0f).epsilon(0.01));
	CHECK(decoded.GetPixel(3, 1)[1] == doctest::Approx(16.0f).epsilon(0.01));
	CHECK(decoded.GetPixel(0, 0)[3] == doctest::Approx(1.0f));

	// 8-bit images are not HDR data.
	std::filesystem::path const pngPath = directory.GetPath() / "Ldr.png";
	REQUIRE(MakeGradient(2, 2).WritePNG(pngPath).IsOk());
	CHECK(HdrImage::LoadFromFile(pngPath).IsError());
	CHECK(HdrImage().WriteHDR(directory.GetPath() / "Empty.hdr").IsError());
}
