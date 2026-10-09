#include "Editor/Commands/ComponentCommands.h"

#include "Editor/EditorContext.h"

#include "Strada/Scene/Entity.h"

#include <utility>

namespace Strada
{
	namespace
	{
		// The entity and the component's type-erased operations a command works on.
		struct ComponentTarget
		{
			entt::registry* Registry = nullptr;
			entt::entity Handle = entt::null;
			ComponentInfo const* Component = nullptr;

			bool HasComponent() const { return Component->Has(*Registry, Handle); }
		};

		Result<ComponentTarget> FindTarget(EditorContext& context, UUID entityID, std::string_view component, ComponentAccess access)
		{
			Scene& scene = context.GetScene();
			Entity const entity = scene.GetEntityByUUID(entityID);
			if (!entity)
			{
				return MakeError("entity {} does not exist", entityID);
			}
			Result<ComponentInfo const*> info = FindEditableComponent(component, access);
			if (!info)
			{
				return Error{info.GetError()};
			}
			return ComponentTarget{&scene.GetRegistry(), entity.GetHandle(), info.GetValue()};
		}
	}

	Result<ComponentInfo const*> FindEditableComponent(std::string_view name, ComponentAccess access)
	{
		ComponentInfo const* component = ComponentRegistry::Find(name);
		if (component == nullptr)
		{
			return MakeError("unknown component '{}'", name);
		}
		if (component->IsInternal())
		{
			return MakeError("the {} component is managed by the engine and cannot be edited", name);
		}
		if (access == ComponentAccess::AddRemove && component->IsCore())
		{
			return MakeError("every entity has a {} component; it cannot be added or removed", name);
		}
		return component;
	}

	AddComponentCommand::AddComponentCommand(UUID entity, std::string component, Json fields)
		: m_Entity(entity),
		  m_Component(std::move(component)),
		  m_Fields(fields.is_null() ? Json::object() : std::move(fields))
	{
	}

	Result<void> AddComponentCommand::Execute(EditorContext& context)
	{
		Result<ComponentTarget> target = FindTarget(context, m_Entity, m_Component, ComponentAccess::AddRemove);
		if (!target)
		{
			return Error{target.GetError()};
		}
		ComponentTarget const& component = target.GetValue();
		if (component.HasComponent())
		{
			return MakeError("entity {} already has a {} component", m_Entity, m_Component);
		}

		bool const firstExecution = m_Snapshot.is_null();
		Json const& fields = firstExecution ? m_Fields : m_Snapshot;
		if (Result<void> result =
		        component.Component->Deserialize(*component.Registry, component.Handle, fields, context.CreateDeserializationContext());
		    !result)
		{
			return result;
		}
		if (firstExecution)
		{
			m_Snapshot = component.Component->Serialize(*component.Registry, component.Handle);
		}
		return {};
	}

	Result<void> AddComponentCommand::Undo(EditorContext& context)
	{
		Result<ComponentTarget> target = FindTarget(context, m_Entity, m_Component, ComponentAccess::AddRemove);
		if (!target)
		{
			return Error{target.GetError()};
		}
		ComponentTarget const& component = target.GetValue();
		if (!component.HasComponent())
		{
			return MakeError("entity {} has no {} component", m_Entity, m_Component);
		}
		component.Component->Remove(*component.Registry, component.Handle);
		return {};
	}

	std::string AddComponentCommand::GetDescription() const
	{
		return fmt::format("Add {} Component", m_Component);
	}

	RemoveComponentCommand::RemoveComponentCommand(UUID entity, std::string component)
		: m_Entity(entity),
		  m_Component(std::move(component))
	{
	}

	Result<void> RemoveComponentCommand::Execute(EditorContext& context)
	{
		Result<ComponentTarget> target = FindTarget(context, m_Entity, m_Component, ComponentAccess::AddRemove);
		if (!target)
		{
			return Error{target.GetError()};
		}
		ComponentTarget const& component = target.GetValue();
		if (!component.HasComponent())
		{
			return MakeError("entity {} has no {} component", m_Entity, m_Component);
		}
		m_Snapshot = component.Component->Serialize(*component.Registry, component.Handle);
		component.Component->Remove(*component.Registry, component.Handle);
		return {};
	}

	Result<void> RemoveComponentCommand::Undo(EditorContext& context)
	{
		Result<ComponentTarget> target = FindTarget(context, m_Entity, m_Component, ComponentAccess::AddRemove);
		if (!target)
		{
			return Error{target.GetError()};
		}
		ComponentTarget const& component = target.GetValue();
		if (component.HasComponent())
		{
			return MakeError("entity {} already has a {} component", m_Entity, m_Component);
		}
		return component.Component->Deserialize(*component.Registry, component.Handle, m_Snapshot, context.CreateDeserializationContext());
	}

	std::string RemoveComponentCommand::GetDescription() const
	{
		return fmt::format("Remove {} Component", m_Component);
	}

	SetComponentCommand::SetComponentCommand(UUID entity, std::string component, Json patch, uint64_t mergeKey, std::string description)
		: m_Entity(entity),
		  m_Component(std::move(component)),
		  m_Patch(std::move(patch)),
		  m_MergeKey(mergeKey),
		  m_Description(std::move(description))
	{
	}

	Result<void> SetComponentCommand::Execute(EditorContext& context)
	{
		Result<ComponentTarget> target = FindTarget(context, m_Entity, m_Component, ComponentAccess::Modify);
		if (!target)
		{
			return Error{target.GetError()};
		}
		ComponentTarget const& component = target.GetValue();
		if (!component.HasComponent())
		{
			return MakeError("entity {} has no {} component", m_Entity, m_Component);
		}

		DeserializationContext const deserialization = context.CreateDeserializationContext();
		if (!m_After.is_null())
		{
			return component.Component->Deserialize(*component.Registry, component.Handle, m_After, deserialization);
		}

		Json before = component.Component->Serialize(*component.Registry, component.Handle);
		if (Result<void> result = component.Component->Deserialize(*component.Registry, component.Handle, m_Patch, deserialization);
		    !result)
		{
			return result;
		}
		m_Before = std::move(before);
		m_After = component.Component->Serialize(*component.Registry, component.Handle);
		return {};
	}

	Result<void> SetComponentCommand::Undo(EditorContext& context)
	{
		Result<ComponentTarget> target = FindTarget(context, m_Entity, m_Component, ComponentAccess::Modify);
		if (!target)
		{
			return Error{target.GetError()};
		}
		ComponentTarget const& component = target.GetValue();
		if (!component.HasComponent())
		{
			return MakeError("entity {} has no {} component", m_Entity, m_Component);
		}
		return component.Component->Deserialize(*component.Registry, component.Handle, m_Before, context.CreateDeserializationContext());
	}

	std::string SetComponentCommand::GetDescription() const
	{
		return m_Description.empty() ? fmt::format("Edit {}", m_Component) : m_Description;
	}

	bool SetComponentCommand::CanMergeWith(EditorCommand const& next) const
	{
		if (m_MergeKey == 0)
		{
			return false;
		}
		auto const* other = dynamic_cast<SetComponentCommand const*>(&next);
		return other != nullptr && other->m_MergeKey == m_MergeKey && other->m_Entity == m_Entity && other->m_Component == m_Component;
	}

	void SetComponentCommand::MergeWith(EditorCommand& next)
	{
		auto& other = static_cast<SetComponentCommand&>(next);
		m_After = std::move(other.m_After);
	}
}
