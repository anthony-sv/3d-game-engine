#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Timestep.h"

#include <string>
#include <utility>

namespace Strada
{
	class Event;

	// A slice of per-frame application logic (editor, runtime, ImGui). Layers update in push order and receive
	// events in reverse order (overlays first) until an event is handled.
	class Layer
	{
	public:
		explicit Layer(std::string name = "Layer")
			: m_Name(std::move(name))
		{
		}

		virtual ~Layer() = default;

		Layer(Layer const&) = delete;
		Layer& operator=(Layer const&) = delete;

		virtual void OnAttach() {}
		virtual void OnDetach() {}
		virtual void OnUpdate(Timestep timestep) { (void)timestep; }
		virtual void OnImGuiRender() {}
		virtual void OnEvent(Event& event) { (void)event; }

		std::string const& GetName() const { return m_Name; }

	private:
		std::string m_Name;
	};
}
