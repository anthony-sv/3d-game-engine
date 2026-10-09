#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace Strada
{
	struct ImageInfo
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		// Channels stored in the file (1-4).
		uint32_t Channels = 0;
		// Radiance .hdr data (decode with HdrImage).
		bool IsHdr = false;
	};

	// CPU image with 8-bit channels, tightly packed rows, top-left origin.
	class Image
	{
	public:
		Image() = default;
		Image(uint32_t width, uint32_t height, uint32_t channels);

		// Decodes PNG, JPEG, TGA, BMP, PSD, GIF (first frame) or PNM. channels = 0 keeps the file's channel count.
		[[nodiscard]] static Result<Image> LoadFromFile(std::filesystem::path const& path, uint32_t channels = 4);
		[[nodiscard]] static Result<Image> LoadFromMemory(std::span<uint8_t const> data, uint32_t channels = 4);
		// Reads the dimensions and format from the file header without decoding the pixels.
		[[nodiscard]] static Result<ImageInfo> ReadInfo(std::span<uint8_t const> data);

		[[nodiscard]] Result<void> WritePNG(std::filesystem::path const& path) const;

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }
		uint32_t GetChannels() const { return m_Channels; }
		bool IsEmpty() const { return m_Pixels.empty(); }

		std::vector<uint8_t>& GetPixels() { return m_Pixels; }
		std::vector<uint8_t> const& GetPixels() const { return m_Pixels; }

		uint8_t* GetPixel(uint32_t x, uint32_t y) { return m_Pixels.data() + (static_cast<size_t>(y) * m_Width + x) * m_Channels; }
		uint8_t const* GetPixel(uint32_t x, uint32_t y) const
		{
			return m_Pixels.data() + (static_cast<size_t>(y) * m_Width + x) * m_Channels;
		}

	private:
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		uint32_t m_Channels = 0;
		std::vector<uint8_t> m_Pixels;
	};

	// CPU image with 32-bit float channels in linear space (Radiance .hdr), tightly packed rows, top-left origin.
	class HdrImage
	{
	public:
		HdrImage() = default;
		HdrImage(uint32_t width, uint32_t height, uint32_t channels);

		// Decodes Radiance .hdr data. channels = 0 keeps the file's channel count (3).
		[[nodiscard]] static Result<HdrImage> LoadFromMemory(std::span<uint8_t const> data, uint32_t channels = 4);
		[[nodiscard]] static Result<HdrImage> LoadFromFile(std::filesystem::path const& path, uint32_t channels = 4);

		// Writes a Radiance .hdr file (RGBE encoding; alpha is dropped).
		[[nodiscard]] Result<void> WriteHDR(std::filesystem::path const& path) const;

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }
		uint32_t GetChannels() const { return m_Channels; }
		bool IsEmpty() const { return m_Pixels.empty(); }

		std::vector<float>& GetPixels() { return m_Pixels; }
		std::vector<float> const& GetPixels() const { return m_Pixels; }

		float* GetPixel(uint32_t x, uint32_t y) { return m_Pixels.data() + (static_cast<size_t>(y) * m_Width + x) * m_Channels; }
		float const* GetPixel(uint32_t x, uint32_t y) const
		{
			return m_Pixels.data() + (static_cast<size_t>(y) * m_Width + x) * m_Channels;
		}

	private:
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		uint32_t m_Channels = 0;
		std::vector<float> m_Pixels;
	};
}
