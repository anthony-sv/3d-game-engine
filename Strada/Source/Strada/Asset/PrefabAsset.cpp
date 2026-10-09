#include "stpch.h"
#include "Strada/Asset/PrefabAsset.h"

namespace Strada
{
	Result<Ref<PrefabAsset>> PrefabAsset::Create(Json document)
	{
		if (Result<int> header = ReadFileHeader(document, "Prefab", FormatVersion); !header)
		{
			return Error{header.GetError()};
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
