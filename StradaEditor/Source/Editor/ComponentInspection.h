#pragma once

#include "Strada/Core/UUID.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	class Scene;

	// Inspector logic independent of the UI: which components several selected entities share or can gain, how the Add
	// Component menu groups and finds them, and which field values differ between the entities.
	namespace ComponentInspection
	{
		// Components of one category, as the Add Component menu lists them.
		struct ComponentGroup
		{
			std::string_view Category;
			// Sorted by display name.
			std::vector<ComponentInfo const*> Components;
		};

		// Components the inspector edits (not internal, not Tag, which is the entity name) present on every given entity,
		// in registry order. Unknown entities share nothing.
		std::vector<ComponentInfo const*> GetCommonComponents(Scene& scene, std::span<UUID const> entities);
		// Components users can add (neither core nor internal) that at least one of the entities lacks, in registry order.
		std::vector<ComponentInfo const*> GetAddableComponents(Scene& scene, std::span<UUID const> entities);
		// Groups components by category: the categories in ComponentCategory::All order (others after them, by name), the
		// components of each by display name. Categories without components are left out.
		std::vector<ComponentGroup> GroupByCategory(std::span<ComponentInfo const* const> components);
		// The components whose display name ("Point Light") or serialized name ("PointLight") contains the text, ignoring
		// case and surrounding spaces: those whose name starts with it first, then those with a word starting with it, then
		// the others, each by display name. Empty text matches every component.
		std::vector<ComponentInfo const*> SearchComponents(std::span<ComponentInfo const* const> components, std::string_view text);
		// The entities among the given ones that lack the component.
		std::vector<UUID> GetEntitiesWithout(Scene& scene, std::span<UUID const> entities, ComponentInfo const& component);
		// Names of the top-level fields whose values differ between the JSON objects.
		std::vector<std::string> FindMixedFields(std::span<Json const> values);
		// Every field of the component at its default value.
		Json GetDefaultValues(ComponentInfo const& component);
	}
}
