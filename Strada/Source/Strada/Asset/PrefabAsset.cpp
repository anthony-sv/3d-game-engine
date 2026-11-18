#include "stpch.h"
#include "Strada/Asset/PrefabAsset.h"

#include <array>

namespace Strada
{
	namespace
	{
		// Each version after the first. A version that changes what an entity holds changes the scene format too: both
		// formats then get a version, and their migrations share the function that upgrades an entity array, kept here.
		constexpr std::array<DocumentVersion, 1> PrefabVersions = {{
			// Script fields and visual script variables may be lists ({ "Type": "Float[]", "Value": [...] }), which readers of
			// version 1 refuse as an unknown type. A version-1 prefab holds none and is a version-2 prefab as it is.
			{"script fields and visual script variables may hold lists", nullptr},
		}};

		constexpr VersionedDocumentFormat PrefabFormat{"Prefab", PrefabVersions};
		static_assert(PrefabFormat.GetVersion() == PrefabAsset::FormatVersion, "PrefabAsset::FormatVersion is the newest version");
	}

	VersionedDocumentFormat const& PrefabAsset::GetFormat()
	{
		return PrefabFormat;
	}

	Result<Ref<PrefabAsset>> PrefabAsset::Create(Json document)
	{
		if (Result<int> migrated = MigrateDocument(document, PrefabFormat); !migrated)
		{
			return Error{migrated.GetError()};
		}
		auto const entities = document.find("Entities");
		if (entities == document.end() || !entities->is_array() || entities->empty())
		{
			return Error{"prefab has no entities"};
		}
		return CreateRef<PrefabAsset>(PrivateTag{}, std::move(document));
	}

	PrefabAsset::PrefabAsset(PrivateTag, Json document)
		: m_Document(std::move(document))
	{
	}
}
