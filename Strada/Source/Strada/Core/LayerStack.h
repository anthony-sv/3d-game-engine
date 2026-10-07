#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Layer.h"

#include <vector>

namespace Strada
{
	// Owns layers. Regular layers come first, overlays after them. Attaching/detaching is done by the owner
	// (Application); the stack only stores. Must not be modified while it is being iterated.
	class LayerStack
	{
	public:
		LayerStack() = default;
		~LayerStack() = default;

		LayerStack(LayerStack const&) = delete;
		LayerStack& operator=(LayerStack const&) = delete;

		Layer& PushLayer(Scope<Layer> layer);
		Layer& PushOverlay(Scope<Layer> overlay);
		// Removes the layer and transfers ownership back to the caller; returns null if it is not in the stack.
		Scope<Layer> Remove(Layer* layer);
		// Removes every layer, overlays first then layers in reverse push order, and returns them in removal order.
		std::vector<Scope<Layer>> RemoveAll();

		size_t GetSize() const { return m_Layers.size(); }
		bool IsEmpty() const { return m_Layers.empty(); }

		std::vector<Scope<Layer>>::iterator begin() { return m_Layers.begin(); }
		std::vector<Scope<Layer>>::iterator end() { return m_Layers.end(); }
		std::vector<Scope<Layer>>::reverse_iterator rbegin() { return m_Layers.rbegin(); }
		std::vector<Scope<Layer>>::reverse_iterator rend() { return m_Layers.rend(); }
		std::vector<Scope<Layer>>::const_iterator begin() const { return m_Layers.begin(); }
		std::vector<Scope<Layer>>::const_iterator end() const { return m_Layers.end(); }

	private:
		std::vector<Scope<Layer>> m_Layers;
		size_t m_LayerInsertIndex = 0;
	};
}
