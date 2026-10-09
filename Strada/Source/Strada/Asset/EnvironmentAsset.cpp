#include "stpch.h"
#include "Strada/Asset/EnvironmentAsset.h"

namespace Strada
{
	namespace
	{
		constexpr uint32_t MaxEnvironmentDimension = 16384;
	}

	Result<Ref<EnvironmentAsset>> EnvironmentAsset::CreateFromEncoded(Buffer data)
	{
		Result<ImageInfo> info = Image::ReadInfo(data.GetSpan());
		if (!info)
		{
			return Error{info.GetError()};
		}
		if (!info.GetValue().IsHdr)
		{
			return Error{"environments must be Radiance HDR images"};
		}
		uint32_t const width = info.GetValue().Width;
		uint32_t const height = info.GetValue().Height;
		if (width == 0 || height == 0 || width > MaxEnvironmentDimension || height > MaxEnvironmentDimension)
		{
			return MakeError("unsupported environment size {}x{} (maximum {})", width, height, MaxEnvironmentDimension);
		}
		return CreateRef<EnvironmentAsset>(PrivateTag{}, std::move(data), HdrImage(), width, height);
	}

	Result<Ref<EnvironmentAsset>> EnvironmentAsset::CreateFromImage(HdrImage image)
	{
		if (image.IsEmpty() || (image.GetChannels() != 3 && image.GetChannels() != 4))
		{
			return Error{"environments need an RGB or RGBA HDR image"};
		}
		if (image.GetWidth() > MaxEnvironmentDimension || image.GetHeight() > MaxEnvironmentDimension)
		{
			return MakeError("unsupported environment size {}x{} (maximum {})", image.GetWidth(), image.GetHeight(),
			                 MaxEnvironmentDimension);
		}
		uint32_t const width = image.GetWidth();
		uint32_t const height = image.GetHeight();
		return CreateRef<EnvironmentAsset>(PrivateTag{}, Buffer(), std::move(image), width, height);
	}

	EnvironmentAsset::EnvironmentAsset(PrivateTag, Buffer encodedData, HdrImage image, uint32_t width, uint32_t height)
		: m_EncodedData(std::move(encodedData)),
		  m_Image(std::move(image)),
		  m_Width(width),
		  m_Height(height)
	{
	}

	Result<HdrImage> EnvironmentAsset::Decode() const
	{
		if (m_EncodedData.GetSize() > 0)
		{
			return HdrImage::LoadFromMemory(m_EncodedData.GetSpan(), 4);
		}
		if (m_Image.GetChannels() == 4)
		{
			return m_Image;
		}

		HdrImage rgba(m_Image.GetWidth(), m_Image.GetHeight(), 4);
		for (uint32_t y = 0; y < m_Image.GetHeight(); y++)
		{
			for (uint32_t x = 0; x < m_Image.GetWidth(); x++)
			{
				float const* source = m_Image.GetPixel(x, y);
				float* destination = rgba.GetPixel(x, y);
				destination[0] = source[0];
				destination[1] = source[1];
				destination[2] = source[2];
				destination[3] = 1.0f;
			}
		}
		return rgba;
	}
}
