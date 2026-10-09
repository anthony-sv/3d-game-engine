#pragma once

#include "Strada/Core/UUID.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Strada
{
	// Picking IDs of the entities a viewport renders. Each entity gets the next free ID the first time it is asked for, and
	// IDs are never reused: a pick result that arrives frames later cannot resolve to a different entity.
	class PickingIdMap
	{
	public:
		// Assigns an ID on first use. Returns 0 (not pickable) for invalid UUIDs or once every ID is taken.
		uint32_t GetId(UUID entity);
		// Invalid when the ID was never assigned.
		UUID GetEntity(uint32_t id) const;
		size_t GetCount() const { return m_Entities.size(); }
		// Forgets every assignment (for example when another scene is opened).
		void Clear();

	private:
		std::unordered_map<UUID, uint32_t> m_Ids;
		// Entity of ID i + 1.
		std::vector<UUID> m_Entities;
	};
}
