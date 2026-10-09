#pragma once

#include "Strada/Core/UUID.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace Strada
{
	enum class SelectionMode : uint8_t
	{
		// The given entities become the selection.
		Replace = 0,
		Add,
		Remove,
		// Selected entities are deselected, the others selected.
		Toggle
	};

	// Ordered set of selected entities (in selection order) with a primary entity: the one the inspector shows, by
	// default the most recently selected.
	class EntitySelection
	{
	public:
		std::vector<UUID> const& GetEntities() const { return m_Entities; }
		// Invalid when nothing is selected.
		UUID GetPrimary() const { return m_Primary; }
		bool Contains(UUID entity) const;
		bool IsEmpty() const { return m_Entities.empty(); }
		size_t GetCount() const { return m_Entities.size(); }

		// Invalid IDs and duplicates are ignored.
		void Apply(std::span<UUID const> entities, SelectionMode mode);
		// Returns false (and changes nothing) when the entity is not selected.
		bool SetPrimary(UUID entity);
		void Clear();
		// Removes the entities for which keep returns false; the primary falls back to the last remaining entity.
		void Retain(std::function<bool(UUID)> const& keep);

	private:
		void Add(UUID entity);
		void Remove(UUID entity);

		std::vector<UUID> m_Entities;
		UUID m_Primary = UUID::Invalid();
	};
}
