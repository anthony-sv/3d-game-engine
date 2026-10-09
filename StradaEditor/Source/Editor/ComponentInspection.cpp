#include "Editor/ComponentInspection.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

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
