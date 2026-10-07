#include "stpch.h"
#include "Strada/Core/LayerStack.h"

#include <algorithm>

namespace Strada
{
	Layer& LayerStack::PushLayer(Scope<Layer> layer)
	{
		ST_CORE_ASSERT(layer != nullptr);
		Layer& reference = *layer;
		m_Layers.emplace(m_Layers.begin() + static_cast<std::ptrdiff_t>(m_LayerInsertIndex), std::move(layer));
		m_LayerInsertIndex++;
		return reference;
	}

	Layer& LayerStack::PushOverlay(Scope<Layer> overlay)
	{
		ST_CORE_ASSERT(overlay != nullptr);
		Layer& reference = *overlay;
		m_Layers.emplace_back(std::move(overlay));
		return reference;
	}

	Scope<Layer> LayerStack::Remove(Layer* layer)
	{
		auto const it = std::find_if(m_Layers.begin(), m_Layers.end(),
		                             [layer](Scope<Layer> const& entry)
		                             {
										 return entry.get() == layer;
									 });
		if (it == m_Layers.end())
		{
			return nullptr;
		}

		size_t const index = static_cast<size_t>(it - m_Layers.begin());
		Scope<Layer> removed = std::move(*it);
		m_Layers.erase(it);
		if (index < m_LayerInsertIndex)
		{
			m_LayerInsertIndex--;
		}
		return removed;
	}

	std::vector<Scope<Layer>> LayerStack::RemoveAll()
	{
		std::vector<Scope<Layer>> removed;
		removed.reserve(m_Layers.size());
		for (auto it = m_Layers.rbegin(); it != m_Layers.rend(); ++it)
		{
			removed.push_back(std::move(*it));
		}
		m_Layers.clear();
		m_LayerInsertIndex = 0;
		return removed;
	}
}
