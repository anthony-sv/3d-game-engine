#include "Editor/Viewport/PickingIdMap.h"

#include "Strada/Renderer/SceneRenderer.h"

namespace Strada
{
	uint32_t PickingIdMap::GetId(UUID entity)
	{
		if (!entity.IsValid())
		{
			return 0;
		}
		if (auto const it = m_Ids.find(entity); it != m_Ids.end())
		{
			return it->second;
		}
		if (m_Entities.size() >= SceneRenderer::MaxPickingId)
		{
			return 0;
		}
		m_Entities.push_back(entity);
		uint32_t const id = static_cast<uint32_t>(m_Entities.size());
		m_Ids.emplace(entity, id);
		return id;
	}

	UUID PickingIdMap::GetEntity(uint32_t id) const
	{
		return id != 0 && id <= m_Entities.size() ? m_Entities[id - 1] : UUID::Invalid();
	}

	void PickingIdMap::Clear()
	{
		m_Ids.clear();
		m_Entities.clear();
	}
}
