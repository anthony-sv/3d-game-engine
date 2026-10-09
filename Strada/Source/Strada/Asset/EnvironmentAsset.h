#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Result.h"

namespace Strada
{
	// An equirectangular HDR environment (Radiance .hdr, e.g. Poly Haven HDRIs) used for the sky and image-based lighting.
	// Keeps the encoded data and decodes on demand.
	class EnvironmentAsset final : public Asset
	{
		struct PrivateTag
		{
		};

	public:
		// Encoded Radiance .hdr data. Validates the header.
		[[nodiscard]] static Result<Ref<EnvironmentAsset>> CreateFromEncoded(Buffer data);
		// Decoded linear RGB(A) pixels.
		[[nodiscard]] static Result<Ref<EnvironmentAsset>> CreateFromImage(HdrImage image);

		EnvironmentAsset(PrivateTag, Buffer encodedData, HdrImage image, uint32_t width, uint32_t height);

		static AssetType GetStaticType() { return AssetType::Environment; }
		AssetType GetAssetType() const override { return GetStaticType(); }

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }

		// Linear RGBA, 32-bit float per channel.
		[[nodiscard]] Result<HdrImage> Decode() const;

	private:
		Buffer m_EncodedData;
		HdrImage m_Image;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
	};
}
