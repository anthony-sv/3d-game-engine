#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/VersionedDocument.h"

namespace Strada
{
	// A prefab document (.sprefab): the entity array written by PrefabSerializer, instantiated into scenes by the Scene module.
	class PrefabAsset final : public Asset
	{
		struct PrivateTag
		{
		};

	public:
		// The version prefab files are written in; readers read every version up to it (GetFormat).
		static constexpr int FormatVersion = 2;

		// The prefab format: its type, its versions and their migrations (Docs/Architecture.md §7.3, "Versions and
		// migrations"). Version 2 lets script fields and visual script variables hold lists; a version-1 prefab is read as it
		// is. Defined here, below the Scene module that writes prefabs, so that the asset's reader and PrefabSerializer share
		// one history. Any thread.
		[[nodiscard]] static VersionedDocumentFormat const& GetFormat();

		// Validates the file header (a version from 1 to FormatVersion), migrates the document to the current version in
		// place, and checks that "Entities" is a non-empty array: the asset's document is always of the current version.
		[[nodiscard]] static Result<Ref<PrefabAsset>> Create(Json document);

		PrefabAsset(PrivateTag, Json document);

		static AssetType GetStaticType() { return AssetType::Prefab; }
		AssetType GetAssetType() const override { return GetStaticType(); }

		Json const& GetDocument() const { return m_Document; }

	private:
		Json m_Document;
	};
}
