#include "Editor/EntitySelection.h"

#include <algorithm>

namespace Strada
{
	bool EntitySelection::Contains(UUID entity) const
	{
		return std::find(m_Entities.begin(), m_Entities.end(), entity) != m_Entities.end();
	}

	void EntitySelection::Apply(std::span<UUID const> entities, SelectionMode mode)
	{
		if (mode == SelectionMode::Replace)
		{
			m_Entities.clear();
			m_Primary = UUID::Invalid();
		}

		for (UUID const entity : entities)
		{
			if (!entity.IsValid())
			{
				continue;
			}
			switch (mode)
			{
				case SelectionMode::Replace:
				case SelectionMode::Add:
					Add(entity);
					break;
				case SelectionMode::Remove:
					Remove(entity);
					break;
				case SelectionMode::Toggle:
					if (Contains(entity))
					{
						Remove(entity);
					}
					else
					{
						Add(entity);
					}
					break;
			}
		}
	}

	bool EntitySelection::SetPrimary(UUID entity)
	{
		if (!Contains(entity))
		{
			return false;
		}
		m_Primary = entity;
		return true;
	}

	void EntitySelection::Clear()
	{
		m_Entities.clear();
		m_Primary = UUID::Invalid();
	}

	void EntitySelection::Retain(std::function<bool(UUID)> const& keep)
	{
		std::erase_if(m_Entities,
		              [&keep](UUID entity)
		              {
						  return !keep(entity);
					  });
		if (!Contains(m_Primary))
		{
			m_Primary = m_Entities.empty() ? UUID::Invalid() : m_Entities.back();
		}
	}

	void EntitySelection::Add(UUID entity)
	{
		if (!Contains(entity))
		{
			m_Entities.push_back(entity);
		}
		m_Primary = entity;
	}

	void EntitySelection::Remove(UUID entity)
	{
		std::erase(m_Entities, entity);
		if (m_Primary == entity)
		{
			m_Primary = m_Entities.empty() ? UUID::Invalid() : m_Entities.back();
		}
	}
}
