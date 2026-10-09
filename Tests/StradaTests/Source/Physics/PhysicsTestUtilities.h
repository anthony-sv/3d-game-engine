#pragma once

#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsSystem.h"
#include "Strada/Physics/PhysicsTypes.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

namespace Strada::Testing
{
	// Initializes the PhysicsSystem for as long as it lives.
	class PhysicsSystemScope
	{
	public:
		PhysicsSystemScope() { PhysicsSystem::Init(); }
		~PhysicsSystemScope() { PhysicsSystem::Shutdown(); }

		PhysicsSystemScope(PhysicsSystemScope const&) = delete;
		PhysicsSystemScope& operator=(PhysicsSystemScope const&) = delete;
	};

	inline bool HasEvent(std::vector<ContactEvent> const& events, ContactEventType type, UUID a, UUID b)
	{
		return std::any_of(events.begin(), events.end(),
		                   [&](ContactEvent const& event)
		                   {
							   return event.Type == type &&
			                          ((event.First == a && event.Second == b) || (event.First == b && event.Second == a));
						   });
	}
}
