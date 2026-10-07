#include "stpch.h"
#include "Strada/Core/Image.h"

#include "Strada/Core/FileSystem.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <limits>

namespace Strada
{
	namespace
	{
		void AppendToVector(void* context, void* data, int size)
		{
			auto* output = static_cast<std::vector<uint8_t>*>(context);
			auto const* bytes = static_cast<uint8_t const*>(data);
			output->insert(output->end(), bytes, bytes + size);
		}
	}

	Image::Image(uint32_t width, uint32_t height, uint32_t channels)
		: m_Width(width),
		  m_Height(height),
		  m_Channels(channels),
		  m_Pixels(static_cast<size_t>(width) * height * channels, 0)
	{
	}

	Result<Image> Image::LoadFromFile(std::filesystem::path const& path, uint32_t channels)
	{
		// The file is read through FileSystem so non-ASCII paths work on every platform.
		Result<Buffer> data = FileSystem::ReadBinaryFile(path);
		if (!data)
		{
			return Error{data.GetError()};
		}

		Result<Image> image = LoadFromMemory(data.GetValue().GetSpan(), channels);
		if (!image)
		{
			return MakeError("Failed to decode '{}': {}", FileSystem::PathToUtf8(path), image.GetError());
		}
		return image;
	}

	Result<Image> Image::LoadFromMemory(std::span<uint8_t const> data, uint32_t channels)
	{
		if (channels > 4)
		{
			return MakeError("Invalid channel count {}", channels);
		}
		if (data.empty() || data.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
		{
			return Error{"Image data is empty or too large"};
		}

		int width = 0;
		int height = 0;
		int fileChannels = 0;
		stbi_uc* pixels =
			stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &width, &height, &fileChannels, static_cast<int>(channels));
		if (pixels == nullptr)
		{
			return Error{stbi_failure_reason() != nullptr ? stbi_failure_reason() : "unknown image format"};
		}

		uint32_t const resultChannels = channels != 0 ? channels : static_cast<uint32_t>(fileChannels);
		Image image(static_cast<uint32_t>(width), static_cast<uint32_t>(height), resultChannels);
		std::memcpy(image.m_Pixels.data(), pixels, image.m_Pixels.size());
		stbi_image_free(pixels);
		return image;
	}

	Result<void> Image::WritePNG(std::filesystem::path const& path) const
	{
		if (IsEmpty() || m_Channels == 0 || m_Channels > 4)
		{
			return Error{"Cannot write an empty image"};
		}

		std::vector<uint8_t> encoded;
		int const result = stbi_write_png_to_func(AppendToVector, &encoded, static_cast<int>(m_Width), static_cast<int>(m_Height),
		                                          static_cast<int>(m_Channels), m_Pixels.data(), static_cast<int>(m_Width * m_Channels));
		if (result == 0)
		{
			return MakeError("Failed to encode PNG '{}'", FileSystem::PathToUtf8(path));
		}
		return FileSystem::WriteBinaryFile(path, encoded);
	}
}
