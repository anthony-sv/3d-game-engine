#include "Editor/ComponentInspection.h"

#include "Editor/UI/EditorUI.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <utility>

namespace Strada
{
	namespace ComponentInspection
	{
		namespace
		{
			bool AllHave(Scene& scene, std::span<UUID const> entities, ComponentInfo const& component)
			{
				for (UUID const id : entities)
				{
					Entity const entity = scene.GetEntityByUUID(id);
					if (!entity || !component.Has(scene.GetRegistry(), entity.GetHandle()))
					{
						return false;
					}
				}
				return true;
			}

			std::string ToLower(std::string_view text)
			{
				std::string lower(text);
				std::transform(lower.begin(), lower.end(), lower.begin(),
				               [](unsigned char c)
				               {
								   return static_cast<char>(std::tolower(c));
							   });
				return lower;
			}

			// Components with their display names, sorted by them (names repeat only for distinct serialized names).
			std::vector<std::pair<std::string, ComponentInfo const*>> SortByDisplayName(std::span<ComponentInfo const* const> components)
			{
				std::vector<std::pair<std::string, ComponentInfo const*>> named;
				named.reserve(components.size());
				for (ComponentInfo const* component : components)
				{
					named.emplace_back(UI::FormatDisplayName(component->Name), component);
				}
				std::sort(named.begin(), named.end(),
				          [](auto const& first, auto const& second)
				          {
							  return first.first != second.first ? first.first < second.first : first.second->Name < second.second->Name;
						  });
				return named;
			}

			// How well a component's names match lowercase search text: 0 = a name starts with it, 1 = a word of the display
			// name starts with it, 2 = a name contains it, 3 = no match.
			int RankMatch(std::string const& displayName, std::string_view name, std::string const& text)
			{
				std::string const display = ToLower(displayName);
				std::string const serialized = ToLower(name);
				if (display.starts_with(text) || serialized.starts_with(text))
				{
					return 0;
				}
				for (size_t position = display.find(text); position != std::string::npos; position = display.find(text, position + 1))
				{
					if (position > 0 && display[position - 1] == ' ')
					{
						return 1;
					}
				}
				return display.find(text) != std::string::npos || serialized.find(text) != std::string::npos ? 2 : 3;
			}
		}

		std::vector<ComponentInfo const*> GetCommonComponents(Scene& scene, std::span<UUID const> entities)
		{
			std::vector<ComponentInfo const*> components;
			if (entities.empty())
			{
				return components;
			}
			for (ComponentInfo const& component : ComponentRegistry::GetComponents())
			{
				if (!component.IsInternal() && component.Name != ComponentTraits<TagComponent>::Name && AllHave(scene, entities, component))
				{
					components.push_back(&component);
				}
			}
			return components;
		}

		std::vector<ComponentInfo const*> GetAddableComponents(Scene& scene, std::span<UUID const> entities)
		{
			std::vector<ComponentInfo const*> components;
			if (entities.empty())
			{
				return components;
			}
			for (ComponentInfo const& component : ComponentRegistry::GetComponents())
			{
				if (!component.IsCore() && !component.IsInternal() && !GetEntitiesWithout(scene, entities, component).empty())
				{
					components.push_back(&component);
				}
			}
			return components;
		}

		std::vector<ComponentGroup> GroupByCategory(std::span<ComponentInfo const* const> components)
		{
			std::vector<std::string_view> categories(ComponentCategory::All.begin(), ComponentCategory::All.end());
			std::vector<std::string_view> others;
			for (ComponentInfo const* component : components)
			{
				if (std::find(categories.begin(), categories.end(), component->Category) == categories.end() &&
				    std::find(others.begin(), others.end(), component->Category) == others.end())
				{
					others.push_back(component->Category);
				}
			}
			std::sort(others.begin(), others.end());
			categories.insert(categories.end(), others.begin(), others.end());

			std::vector<std::pair<std::string, ComponentInfo const*>> const sorted = SortByDisplayName(components);
			std::vector<ComponentGroup> groups;
			for (std::string_view const category : categories)
			{
				ComponentGroup group{category, {}};
				for (auto const& [displayName, component] : sorted)
				{
					if (component->Category == category)
					{
						group.Components.push_back(component);
					}
				}
				if (!group.Components.empty())
				{
					groups.push_back(std::move(group));
				}
			}
			return groups;
		}

		std::vector<ComponentInfo const*> SearchComponents(std::span<ComponentInfo const* const> components, std::string_view text)
		{
			size_t const first = text.find_first_not_of(' ');
			std::string const search =
				first == std::string_view::npos ? std::string() : ToLower(text.substr(first, text.find_last_not_of(' ') - first + 1));

			// Stable within each rank, so components stay sorted by display name.
			std::vector<std::pair<int, ComponentInfo const*>> ranked;
			for (auto const& [displayName, component] : SortByDisplayName(components))
			{
				int const rank = RankMatch(displayName, component->Name, search);
				if (rank < 3)
				{
					ranked.emplace_back(rank, component);
				}
			}
			std::stable_sort(ranked.begin(), ranked.end(),
			                 [](auto const& firstMatch, auto const& secondMatch)
			                 {
								 return firstMatch.first < secondMatch.first;
							 });
			std::vector<ComponentInfo const*> matches;
			matches.reserve(ranked.size());
			for (auto const& [rank, component] : ranked)
			{
				matches.push_back(component);
			}
			return matches;
		}

		std::vector<UUID> GetEntitiesWithout(Scene& scene, std::span<UUID const> entities, ComponentInfo const& component)
		{
			std::vector<UUID> without;
			for (UUID const id : entities)
			{
				Entity const entity = scene.GetEntityByUUID(id);
				if (entity && !component.Has(scene.GetRegistry(), entity.GetHandle()))
				{
					without.push_back(id);
				}
			}
			return without;
		}

		std::vector<std::string> FindMixedFields(std::span<Json const> values)
		{
			std::vector<std::string> mixed;
			if (values.size() < 2 || !values.front().is_object())
			{
				return mixed;
			}
			for (auto const& [name, value] : values.front().items())
			{
				for (size_t i = 1; i < values.size(); i++)
				{
					if (!values[i].is_object() || !values[i].contains(name) || values[i][name] != value)
					{
						mixed.push_back(name);
						break;
					}
				}
			}
			return mixed;
		}

		Json GetDefaultValues(ComponentInfo const& component)
		{
			Json defaults = Json::object();
			for (FieldDescriptor const& field : component.Fields)
			{
				defaults[std::string(field.Name)] = field.Default;
			}
			return defaults;
		}
	}
}
