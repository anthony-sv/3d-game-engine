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

	SetComponentsCommand::SetComponentsCommand(std::vector<ComponentEdit> edits, uint64_t mergeKey, std::string description)
		: m_Edits(std::move(edits)),
		  m_MergeKey(mergeKey),
		  m_Description(std::move(description))
	{
	}

	Result<void> SetComponentsCommand::Execute(EditorContext& context)
	{
		if (m_Edits.empty())
		{
			return Error{"no component edits"};
		}
		std::vector<ComponentTarget> targets;
		targets.reserve(m_Edits.size());
		for (ComponentEdit const& edit : m_Edits)
		{
			Result<ComponentTarget> target = FindTarget(context, edit.Entity, edit.Component, ComponentAccess::Modify);
			if (!target)
			{
				return Error{target.GetError()};
			}
			if (!target.GetValue().HasComponent())
			{
				return MakeError("entity {} has no {} component", edit.Entity, edit.Component);
			}
			targets.push_back(target.GetValue());
		}

		DeserializationContext const deserialization = context.CreateDeserializationContext();
		bool const redo = !m_After.empty();
		std::vector<Json> before;
		before.reserve(targets.size());
		for (ComponentTarget const& target : targets)
		{
			before.push_back(target.Component->Serialize(*target.Registry, target.Handle));
		}

		for (size_t i = 0; i < targets.size(); i++)
		{
			ComponentTarget const& target = targets[i];
			Json const& state = redo ? m_After[i] : m_Edits[i].Patch;
			if (Result<void> result = target.Component->Deserialize(*target.Registry, target.Handle, state, deserialization); !result)
			{
				// Snapshots taken from the components themselves always deserialize: restore what was applied.
				for (size_t j = i; j-- > 0;)
				{
					(void)targets[j].Component->Deserialize(*targets[j].Registry, targets[j].Handle, before[j], deserialization);
				}
				return MakeError("{} of entity {}: {}", m_Edits[i].Component, m_Edits[i].Entity, result.GetError());
			}
		}

		if (!redo)
		{
			m_Before = std::move(before);
			m_After.reserve(targets.size());
			for (ComponentTarget const& target : targets)
			{
				m_After.push_back(target.Component->Serialize(*target.Registry, target.Handle));
			}
		}
		return {};
	}

	Result<void> SetComponentsCommand::Undo(EditorContext& context)
	{
		std::vector<ComponentTarget> targets;
		targets.reserve(m_Edits.size());
		for (ComponentEdit const& edit : m_Edits)
		{
			Result<ComponentTarget> target = FindTarget(context, edit.Entity, edit.Component, ComponentAccess::Modify);
			if (!target)
			{
				return Error{target.GetError()};
			}
			if (!target.GetValue().HasComponent())
			{
				return MakeError("entity {} has no {} component", edit.Entity, edit.Component);
			}
			targets.push_back(target.GetValue());
		}

		// Reverse order, so a component edited twice ends in its original state.
		DeserializationContext const deserialization = context.CreateDeserializationContext();
		for (size_t i = targets.size(); i-- > 0;)
		{
			ComponentTarget const& target = targets[i];
			if (Result<void> result = target.Component->Deserialize(*target.Registry, target.Handle, m_Before[i], deserialization); !result)
			{
				for (size_t j = i + 1; j < targets.size(); j++)
				{
					(void)targets[j].Component->Deserialize(*targets[j].Registry, targets[j].Handle, m_After[j], deserialization);
				}
				return result;
			}
		}
		return {};
	}

	std::string SetComponentsCommand::GetDescription() const
	{
		if (!m_Description.empty())
		{
			return m_Description;
		}
		return m_Edits.size() == 1 ? fmt::format("Edit {}", m_Edits.front().Component) : fmt::format("Edit {} Components", m_Edits.size());
	}

	bool SetComponentsCommand::CanMergeWith(EditorCommand const& next) const
	{
		if (m_MergeKey == 0)
		{
			return false;
		}
		auto const* other = dynamic_cast<SetComponentsCommand const*>(&next);
		if (other == nullptr || other->m_MergeKey != m_MergeKey || other->m_Edits.size() != m_Edits.size())
		{
			return false;
		}
		for (size_t i = 0; i < m_Edits.size(); i++)
		{
			if (other->m_Edits[i].Entity != m_Edits[i].Entity || other->m_Edits[i].Component != m_Edits[i].Component)
			{
				return false;
			}
		}
		return true;
	}

	void SetComponentsCommand::MergeWith(EditorCommand& next)
	{
		auto& other = static_cast<SetComponentsCommand&>(next);
		m_After = std::move(other.m_After);
	}
}
