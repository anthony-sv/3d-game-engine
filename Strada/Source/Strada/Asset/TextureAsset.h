#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Result.h"

namespace Strada
{
	// A 2D image used as a texture. Keeps the encoded file data (compact) and decodes on demand; the color space (sRGB or
	// linear) is chosen by the material slot that samples it.
	class TextureAsset final : public Asset
	{
		struct PrivateTag
		{
		};

	public:
		// Encoded image file data (PNG, JPEG, TGA, BMP, PSD, GIF, PNM). Validates the header.
		[[nodiscard]] static Result<Ref<TextureAsset>> CreateFromEncoded(Buffer data);
		// Decoded pixels (1-4 channels).
		[[nodiscard]] static Result<Ref<TextureAsset>> CreateFromImage(Image image);

		TextureAsset(PrivateTag, Buffer encodedData, Image image, ImageInfo info);

		static AssetType GetStaticType() { return AssetType::Texture; }
		AssetType GetAssetType() const override { return GetStaticType(); }

		uint32_t GetWidth() const { return m_Info.Width; }
		uint32_t GetHeight() const { return m_Info.Height; }
		bool IsEncoded() const { return m_EncodedData.GetSize() > 0; }

		// RGBA, 8 bits per channel.
		[[nodiscard]] Result<Image> Decode() const;

	private:
		Buffer m_EncodedData;
		Image m_Image;
		ImageInfo m_Info;
	};
}
