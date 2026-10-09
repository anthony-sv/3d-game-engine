#include "stpch.h"
#include "Strada/Asset/TextureAsset.h"

namespace Strada
{
	namespace
	{
		// Guards against decompression bombs: textures larger than this are rejected before decoding.
		constexpr uint32_t MaxTextureDimension = 16384;
	}

	Result<Ref<TextureAsset>> TextureAsset::CreateFromEncoded(Buffer data)
	{
		Result<ImageInfo> info = Image::ReadInfo(data.GetSpan());
		if (!info)
		{
			return Error{info.GetError()};
		}
		if (info.GetValue().IsHdr)
		{
			return Error{"HDR images are environments, not textures"};
		}
		if (info.GetValue().Width == 0 || info.GetValue().Height == 0 || info.GetValue().Width > MaxTextureDimension ||
		    info.GetValue().Height > MaxTextureDimension)
		{
			return MakeError("unsupported texture size {}x{} (maximum {})", info.GetValue().Width, info.GetValue().Height,
			                 MaxTextureDimension);
		}
		return CreateRef<TextureAsset>(PrivateTag{}, std::move(data), Image(), info.GetValue());
	}

	Result<Ref<TextureAsset>> TextureAsset::CreateFromImage(Image image)
	{
		if (image.IsEmpty() || image.GetChannels() == 0 || image.GetChannels() > 4)
		{
			return Error{"cannot create a texture from an empty image"};
		}
		if (image.GetWidth() > MaxTextureDimension || image.GetHeight() > MaxTextureDimension)
		{
			return MakeError("unsupported texture size {}x{} (maximum {})", image.GetWidth(), image.GetHeight(), MaxTextureDimension);
		}

		ImageInfo info;
		info.Width = image.GetWidth();
		info.Height = image.GetHeight();
		info.Channels = image.GetChannels();
		return CreateRef<TextureAsset>(PrivateTag{}, Buffer(), std::move(image), info);
	}

	TextureAsset::TextureAsset(PrivateTag, Buffer encodedData, Image image, ImageInfo info)
		: m_EncodedData(std::move(encodedData)),
		  m_Image(std::move(image)),
		  m_Info(info)
	{
	}

	Result<Image> TextureAsset::Decode() const
	{
		if (IsEncoded())
		{
			return Image::LoadFromMemory(m_EncodedData.GetSpan(), 4);
		}
		if (m_Image.GetChannels() == 4)
		{
			return m_Image;
		}

		// Expand gray, gray+alpha and RGB to RGBA.
		Image rgba(m_Image.GetWidth(), m_Image.GetHeight(), 4);
		uint32_t const channels = m_Image.GetChannels();
		for (uint32_t y = 0; y < m_Image.GetHeight(); y++)
		{
			for (uint32_t x = 0; x < m_Image.GetWidth(); x++)
			{
				uint8_t const* source = m_Image.GetPixel(x, y);
				uint8_t* destination = rgba.GetPixel(x, y);
				switch (channels)
				{
					case 1:
						destination[0] = destination[1] = destination[2] = source[0];
						destination[3] = 255;
						break;
					case 2:
						destination[0] = destination[1] = destination[2] = source[0];
						destination[3] = source[1];
						break;
					default:
						destination[0] = source[0];
						destination[1] = source[1];
						destination[2] = source[2];
						destination[3] = 255;
						break;
				}
			}
		}
		return rgba;
	}
}
