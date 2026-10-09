#include "stpch.h"
#include "Strada/Renderer/TextureMips.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace Strada
{
	namespace
	{
		float SrgbToLinear(float value)
		{
			return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
		}

		float LinearToSrgb(float value)
		{
			return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
		}

		std::array<float, 256> MakeDecodeTable()
		{
			std::array<float, 256> table{};
			for (size_t i = 0; i < table.size(); i++)
			{
				table[i] = SrgbToLinear(static_cast<float>(i) / 255.0f);
			}
			return table;
		}

		uint8_t ToByte(float value)
		{
			return static_cast<uint8_t>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
		}
	}

	uint32_t GetMipLevelCount(uint32_t width, uint32_t height)
	{
		uint32_t const largest = std::max(std::max(width, height), 1u);
		return static_cast<uint32_t>(std::bit_width(largest));
	}

	std::vector<Image> GenerateMipChain(Image const& image, bool srgb)
	{
		ST_CORE_ASSERT(image.GetChannels() == 4, "Mip generation expects RGBA8 images");
		static std::array<float, 256> const s_SrgbDecode = MakeDecodeTable();

		std::vector<Image> levels;
		levels.reserve(GetMipLevelCount(image.GetWidth(), image.GetHeight()));
		levels.push_back(image);

		while (levels.back().GetWidth() > 1 || levels.back().GetHeight() > 1)
		{
			Image const& source = levels.back();
			uint32_t const width = std::max(source.GetWidth() / 2, 1u);
			uint32_t const height = std::max(source.GetHeight() / 2, 1u);
			Image destination(width, height, 4);
			for (uint32_t y = 0; y < height; y++)
			{
				for (uint32_t x = 0; x < width; x++)
				{
					uint32_t const x0 = std::min(x * 2, source.GetWidth() - 1);
					uint32_t const x1 = std::min(x * 2 + 1, source.GetWidth() - 1);
					uint32_t const y0 = std::min(y * 2, source.GetHeight() - 1);
					uint32_t const y1 = std::min(y * 2 + 1, source.GetHeight() - 1);
					uint8_t const* samples[4] = {source.GetPixel(x0, y0), source.GetPixel(x1, y0), source.GetPixel(x0, y1),
					                             source.GetPixel(x1, y1)};
					uint8_t* output = destination.GetPixel(x, y);
					for (int channel = 0; channel < 4; channel++)
					{
						bool const decode = srgb && channel < 3;
						float sum = 0.0f;
						for (uint8_t const* sample : samples)
						{
							sum += decode ? s_SrgbDecode[sample[channel]] : static_cast<float>(sample[channel]) / 255.0f;
						}
						float const average = sum * 0.25f;
						output[channel] = ToByte(decode ? LinearToSrgb(average) : average);
					}
				}
			}
			levels.push_back(std::move(destination));
		}
		return levels;
	}
}
