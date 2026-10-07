#pragma once

#include "Strada/Core/Assert.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Scene.h"

#include <entt/entity/registry.hpp>

#include <string>
#include <type_traits>

namespace Strada
{
	// Lightweight, copyable handle to an entity in a Scene. It becomes invalid when the entity is destroyed.
	class Entity
	{
	public:
		Entity() = default;
		Entity(entt::entity handle, Scene* scene)
			: m_Handle(handle),
			  m_Scene(scene)
		{
		}

		template<typename T, typename... Args>
		T& AddComponent(Args&&... args)
		{
			ST_CORE_ASSERT(IsValid(), "Entity is invalid");
			ST_CORE_ASSERT(!HasComponent<T>(), "Entity already has this component");
			return m_Scene->m_Registry.emplace<T>(m_Handle, std::forward<Args>(args)...);
		}

		template<typename T, typename... Args>
		T& AddOrReplaceComponent(Args&&... args)
		{
			ST_CORE_ASSERT(IsValid(), "Entity is invalid");
			return m_Scene->m_Registry.emplace_or_replace<T>(m_Handle, std::forward<Args>(args)...);
		}

		template<typename T>
		T& GetComponent()
		{
			ST_CORE_ASSERT(HasComponent<T>(), "Entity does not have this component");
			return m_Scene->m_Registry.get<T>(m_Handle);
		}

		template<typename T>
		T const& GetComponent() const
		{
			ST_CORE_ASSERT(HasComponent<T>(), "Entity does not have this component");
			return m_Scene->m_Registry.get<T>(m_Handle);
		}

		template<typename T>
		T* TryGetComponent()
		{
			return IsValid() ? m_Scene->m_Registry.try_get<T>(m_Handle) : nullptr;
		}

		template<typename... T>
		bool HasComponent() const
		{
			return IsValid() && m_Scene->m_Registry.all_of<T...>(m_Handle);
		}

		template<typename T>
		void RemoveComponent()
		{
			static_assert(!std::is_same_v<T, IDComponent> && !std::is_same_v<T, TagComponent> && !std::is_same_v<T, TransformComponent> &&
			                  !std::is_same_v<T, RelationshipComponent>,
			              "Core components cannot be removed");
			ST_CORE_ASSERT(HasComponent<T>(), "Entity does not have this component");
			m_Scene->m_Registry.remove<T>(m_Handle);
		}

		UUID GetUUID() const { return GetComponent<IDComponent>().ID; }
		std::string const& GetName() const { return GetComponent<TagComponent>().Tag; }
		void SetName(std::string name) { GetComponent<TagComponent>().Tag = std::move(name); }

		bool IsValid() const { return m_Scene != nullptr && m_Scene->m_Registry.valid(m_Handle); }
		explicit operator bool() const { return IsValid(); }

		entt::entity GetHandle() const { return m_Handle; }
		Scene* GetScene() const { return m_Scene; }

		bool operator==(Entity const& other) const { return m_Handle == other.m_Handle && m_Scene == other.m_Scene; }
		bool operator!=(Entity const& other) const { return !(*this == other); }

	private:
		entt::entity m_Handle = entt::null;
		Scene* m_Scene = nullptr;
	};
}
