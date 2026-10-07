#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Scene/ComponentTraits.h"

#include <entt/entity/registry.hpp>

#include <span>
#include <string_view>

namespace Strada
{
	// Type-erased operations for one component type.
	struct ComponentInfo
	{
		std::string_view Name;
		uint32_t Flags = ComponentFlagsNone;

		bool (*Has)(entt::registry const& registry, entt::entity entity) = nullptr;
		// Adds a default-constructed component (no-op if present).
		void (*Add)(entt::registry& registry, entt::entity entity) = nullptr;
		void (*Remove)(entt::registry& registry, entt::entity entity) = nullptr;
		Json (*Serialize)(entt::registry const& registry, entt::entity entity) = nullptr;
		// Applies JSON fields, adding the component first when missing. On failure nothing changes (a component added
		// for this call is removed again).
		Result<void> (*Deserialize)(entt::registry& registry, entt::entity entity, Json const& json,
		                            DeserializationContext const& context) = nullptr;
		// Copies the component (if present) from one registry/entity to another, replacing any existing one.
		void (*Copy)(entt::registry const& source, entt::entity sourceEntity, entt::registry& destination,
		             entt::entity destinationEntity) = nullptr;
		// Field schema (names, types, defaults).
		Json (*Describe)() = nullptr;

		bool IsCore() const { return (Flags & ComponentFlagsCore) != 0; }
		bool IsInternal() const { return (Flags & ComponentFlagsInternal) != 0; }
	};

	// The single source of truth about component types: scene serialization, scene copies, automation, the editor and
	// script interop all go through it. Thread-safe (immutable after first use).
	class ComponentRegistry
	{
	public:
		// All components in registration order (core components first).
		static std::span<ComponentInfo const> GetComponents();
		static ComponentInfo const* Find(std::string_view name);

		template<RegisteredComponent T>
		static ComponentInfo const& Get()
		{
			ComponentInfo const* info = Find(ComponentTraits<T>::Name);
			ST_CORE_ASSERT(info != nullptr, "Component '{}' is not registered", ComponentTraits<T>::Name);
			return *info;
		}
	};
}
