#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

namespace Strada
{
	// A prefab document (.sprefab): the entity array written by PrefabSerializer, instantiated into scenes by the Scene module.
	class PrefabAsset final : public Asset
	{
		struct PrivateTag
		{
		};

	public:
		static constexpr int FormatVersion = 1;

		// Validates the file header and that "Entities" is a non-empty array.
		[[nodiscard]] static Result<Ref<PrefabAsset>> Create(Json document);

		PrefabAsset(PrivateTag, Json document);

		static AssetType GetStaticType() { return AssetType::Prefab; }
		AssetType GetAssetType() const override { return GetStaticType(); }

		Json const& GetDocument() const { return m_Document; }

	private:
		Json m_Document;
	};
}
