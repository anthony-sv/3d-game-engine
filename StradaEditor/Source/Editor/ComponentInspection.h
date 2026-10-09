#pragma once

#include "Strada/Core/UUID.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <span>
#include <string>
#include <vector>

namespace Strada
{
	class Scene;

	// Inspector logic independent of the UI: which components several selected entities share or can gain, and which
	// field values differ between them.
	namespace ComponentInspection
	{
		// Components the inspector edits (not internal, not Tag, which is the entity name) present on every given entity,
		// in registry order. Unknown entities share nothing.
		std::vector<ComponentInfo const*> GetCommonComponents(Scene& scene, std::span<UUID const> entities);
		// Components users can add (neither core nor internal) that at least one of the entities lacks, in registry order.
		std::vector<ComponentInfo const*> GetAddableComponents(Scene& scene, std::span<UUID const> entities);
		// The entities among the given ones that lack the component.
		std::vector<UUID> GetEntitiesWithout(Scene& scene, std::span<UUID const> entities, ComponentInfo const& component);
		// Names of the top-level fields whose values differ between the JSON objects.
		std::vector<std::string> FindMixedFields(std::span<Json const> values);
		// Every field of the component at its default value.
		Json GetDefaultValues(ComponentInfo const& component);
	}
}
