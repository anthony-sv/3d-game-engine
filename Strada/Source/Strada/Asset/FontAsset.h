#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/Result.h"

#include <span>

namespace Strada
{
	// A TrueType font file (TrueType outlines, in .ttf or .otf files) used by text rendering.
	class FontAsset final : public Asset
	{
		struct PrivateTag
		{
		};

	public:
		// Validates that the data contains a usable font (the first font of a collection is used), repairing damage that
		// leaves the rest usable (see SanitizeTrueTypeFont).
		[[nodiscard]] static Result<Ref<FontAsset>> Create(Buffer data);

		FontAsset(PrivateTag, Buffer data);

		static AssetType GetStaticType() { return AssetType::Font; }
		AssetType GetAssetType() const override { return GetStaticType(); }

		std::span<uint8_t const> GetData() const { return m_Data.GetSpan(); }

	private:
		Buffer m_Data;
	};
}
