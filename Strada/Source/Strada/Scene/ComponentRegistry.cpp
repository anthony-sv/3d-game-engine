#include "stpch.h"
#include "Strada/Scene/ComponentRegistry.h"

#include "Strada/Scene/ComponentSerialization.h"

#include <vector>

namespace Strada
{
	namespace
	{
		template<RegisteredComponent T>
		ComponentInfo MakeComponentInfo()
		{
			ComponentInfo info;
			info.Name = ComponentTraits<T>::Name;
			info.Flags = ComponentTraits<T>::Flags;
			info.Description = ComponentTraits<T>::Description;
			info.Has = [](entt::registry const& registry, entt::entity entity)
			{
				return registry.all_of<T>(entity);
			};
			info.Add = [](entt::registry& registry, entt::entity entity)
			{
				if (!registry.all_of<T>(entity))
				{
					registry.emplace<T>(entity);
				}
			};
			info.Remove = [](entt::registry& registry, entt::entity entity)
			{
				registry.remove<T>(entity);
			};
			info.Serialize = [](entt::registry const& registry, entt::entity entity) -> Json
			{
				T const* component = registry.try_get<T>(entity);
				return component != nullptr ? SerializeComponent(*component) : Json();
			};
			info.Deserialize = [](entt::registry& registry, entt::entity entity, Json const& json,
			                      DeserializationContext const& context) -> Result<void>
			{
				// Validate against a copy (or defaults) first so a failed update leaves the entity untouched.
				T const* existing = registry.try_get<T>(entity);
				T component = existing != nullptr ? *existing : T{};
				if (Result<void> result = DeserializeComponent(json, component, context); !result)
				{
					return result;
				}
				registry.emplace_or_replace<T>(entity, std::move(component));
				return {};
			};
			info.Copy =
				[](entt::registry const& source, entt::entity sourceEntity, entt::registry& destination, entt::entity destinationEntity)
			{
				if (T const* component = source.try_get<T>(sourceEntity))
				{
					destination.emplace_or_replace<T>(destinationEntity, *component);
				}
			};
			info.Fields = GetFieldDescriptors<ComponentTraits<T>, T>();
			return info;
		}

		template<typename... T>
		std::vector<ComponentInfo> MakeRegistry(ComponentList<T...>)
		{
			return {MakeComponentInfo<T>()...};
		}

		std::vector<ComponentInfo> const& GetRegistry()
		{
			// Built on first use (thread-safe static initialization) and immutable afterwards.
			static std::vector<ComponentInfo> const s_Components = MakeRegistry(AllComponents{});
			return s_Components;
		}
	}

	FieldDescriptor const* ComponentInfo::FindField(std::string_view name) const
	{
		for (FieldDescriptor const& field : Fields)
		{
			if (field.Name == name)
			{
				return &field;
			}
		}
		return nullptr;
	}

	std::span<ComponentInfo const> ComponentRegistry::GetComponents()
	{
		return GetRegistry();
	}

	ComponentInfo const* ComponentRegistry::Find(std::string_view name)
	{
		for (ComponentInfo const& info : GetRegistry())
		{
			if (info.Name == name)
			{
				return &info;
			}
		}
		return nullptr;
	}
}
