#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Script/ScriptEngine.h"

#include <algorithm>

namespace Strada
{
	void Scene::StartScripts()
	{
		auto const scripts = m_Registry.view<ScriptComponent>();
		bool const hasScripts = scripts.begin() != scripts.end();
		if (!ScriptEngine::IsInitialized())
		{
			if (hasScripts)
			{
				ST_CORE_WARN("Scene '{}' runs without scripts: the script engine is not initialized", m_Name);
			}
			return;
		}
		if (Scene const* other = ScriptEngine::GetSceneContext(); other != nullptr && other != this)
		{
			ST_CORE_ERROR("Scene '{}' runs without scripts: scene '{}' is running them", m_Name, other->GetName());
			return;
		}
		if (!ScriptEngine::HasGameAssembly())
		{
			if (hasScripts)
			{
				ST_CORE_WARN("Scene '{}' runs without scripts: no game assembly is loaded", m_Name);
			}
			return;
		}
		ScriptEngine::SetSceneContext(this);
		m_RunsScripts = true;
		SyncScriptInstances();
	}

	void Scene::StopScripts()
	{
		if (!m_RunsScripts)
		{
			return;
		}
		// OnDestroy runs while the scene, its physics and its audio are intact: first for scripts whose component went
		// since the last update, then for the others.
		m_StoppingScripts = true;
		DestroyRemovedScriptInstances();
		for (UUID const id : GetScriptInstanceIDs())
		{
			ScriptEngine::InvokeOnDestroy(id);
		}
		ScriptEngine::DestroyAllInstances();
		m_StoppingScripts = false;
		m_ScriptInstances.clear();
		ScriptEngine::SetSceneContext(nullptr);
		m_RunsScripts = false;
	}

	void Scene::SyncScriptInstances()
	{
		if (!m_RunsScripts)
		{
			return;
		}

		// No script code runs while the registry is iterated: constructors and OnDestroy may change it.
		std::vector<entt::entity> replaced;
		std::vector<PendingScriptInstance> pending;
		for (auto const [handle, id, script] : m_Registry.view<IDComponent, ScriptComponent>().each())
		{
			auto const current = m_ScriptInstances.find(handle);
			if (current != m_ScriptInstances.end() && current->second.ClassName == script.ClassName)
			{
				continue;
			}
			if (current != m_ScriptInstances.end())
			{
				replaced.push_back(handle);
			}
			pending.push_back({handle, id.ID, script.ClassName, script.Fields});
		}

		DestroyRemovedScriptInstances();
		for (entt::entity const handle : replaced)
		{
			DestroyScriptInstance(handle);
		}
		CreateScriptInstances(pending);
	}

	void Scene::CreateScriptInstances(std::vector<PendingScriptInstance> const& pending)
	{
		std::vector<UUID> created;
		for (PendingScriptInstance const& instance : pending)
		{
			m_ScriptInstances[instance.Handle] = {instance.Entity, instance.ClassName, false, instance.Fields};
			if (instance.ClassName.empty())
			{
				continue;
			}
			if (Result<void> result = ScriptEngine::CreateInstance(instance.Entity, instance.ClassName, instance.Fields); !result)
			{
				ST_CORE_WARN("Entity '{}' runs no script: {}", Entity(instance.Handle, this).GetName(), result.GetError());
				continue;
			}
			// Looked up again: the constructor may have started other scripts (prefab instances), changing the map.
			if (auto const state = m_ScriptInstances.find(instance.Handle); state != m_ScriptInstances.end())
			{
				state->second.HasInstance = true;
				created.push_back(instance.Entity);
			}
		}
		// Every new instance exists before the first OnCreate, so scripts can find each other.
		for (UUID const id : created)
		{
			ScriptEngine::InvokeOnCreate(id);
		}
	}

	void Scene::StartScriptsOf(std::vector<entt::entity> const& handles)
	{
		if (!m_RunsScripts || m_StoppingScripts)
		{
			return;
		}
		std::vector<PendingScriptInstance> pending;
		for (entt::entity const handle : handles)
		{
			if (ScriptComponent const* script = m_Registry.try_get<ScriptComponent>(handle))
			{
				pending.push_back({handle, m_Registry.get<IDComponent>(handle).ID, script->ClassName, script->Fields});
			}
		}
		CreateScriptInstances(pending);
	}

	Result<ScriptFieldMap> Scene::ReadScriptFields(Entity entity)
	{
		auto const state = entity && entity.GetScene() == this ? m_ScriptInstances.find(entity.GetHandle()) : m_ScriptInstances.end();
		ScriptComponent* const script = entity && entity.GetScene() == this ? entity.TryGetComponent<ScriptComponent>() : nullptr;
		if (!m_RunsScripts || state == m_ScriptInstances.end() || !state->second.HasInstance || script == nullptr ||
		    state->second.Entity != entity.GetUUID() || state->second.ClassName != script->ClassName)
		{
			return Error{"the entity runs no script"};
		}
		Result<ScriptFieldMap> fields = ScriptEngine::GetInstanceFields(state->second.Entity);
		if (!fields)
		{
			return fields;
		}
		for (auto const& [name, value] : fields.GetValue())
		{
			script->Fields[name] = value;
			state->second.Fields[name] = value;
		}
		return fields;
	}

	void Scene::ApplyScriptFieldChanges()
	{
		if (!m_RunsScripts)
		{
			return;
		}
		// Setting fields runs no script code, so the registry can be walked meanwhile.
		for (auto const [handle, script] : m_Registry.view<ScriptComponent>().each())
		{
			auto const state = m_ScriptInstances.find(handle);
			if (state == m_ScriptInstances.end() || !state->second.HasInstance || state->second.ClassName != script.ClassName)
			{
				continue;
			}
			ScriptFieldMap changed;
			for (auto const& [name, value] : script.Fields)
			{
				auto const given = state->second.Fields.find(name);
				if (given == state->second.Fields.end() || !(given->second == value))
				{
					changed.emplace(name, value);
					state->second.Fields[name] = value;
				}
			}
			if (changed.empty())
			{
				continue;
			}
			if (Result<void> const set = ScriptEngine::SetInstanceFields(state->second.Entity, changed); !set)
			{
				ST_CORE_WARN("Entity '{}': {}", Entity(handle, this).GetName(), set.GetError());
			}
		}
	}

	void Scene::UpdateScripts(float deltaTime)
	{
		if (!m_RunsScripts)
		{
			return;
		}
		SyncScriptInstances();
		for (UUID const id : GetScriptInstanceIDs())
		{
			ScriptEngine::InvokeOnUpdate(id, deltaTime);
		}
	}

	void Scene::FixedUpdateScripts(float fixedDeltaTime)
	{
		if (!m_RunsScripts)
		{
			return;
		}
		for (UUID const id : GetScriptInstanceIDs())
		{
			ScriptEngine::InvokeOnFixedUpdate(id, fixedDeltaTime);
		}
	}

	void Scene::DispatchContactEventsToScripts()
	{
		if (!m_RunsScripts || m_ContactEvents.empty())
		{
			return;
		}
		// A copy: scripts may destroy entities and so change what physics reports.
		std::vector<ContactEvent> const events = m_ContactEvents;
		for (ContactEvent const& event : events)
		{
			ScriptEngine::InvokeContactEvent(event.First, event.Second, event.Type);
			ScriptEngine::InvokeContactEvent(event.Second, event.First, event.Type);
		}
	}

	void Scene::DestroyScriptInstance(entt::entity handle)
	{
		auto const instance = m_ScriptInstances.find(handle);
		if (instance == m_ScriptInstances.end())
		{
			return;
		}
		ScriptInstanceState const state = instance->second;
		m_ScriptInstances.erase(instance);
		if (state.HasInstance)
		{
			ScriptEngine::InvokeOnDestroy(state.Entity);
		}
	}

	void Scene::DestroyRemovedScriptInstances()
	{
		std::vector<entt::entity> removed;
		for (auto const& [handle, state] : m_ScriptInstances)
		{
			if (!m_Registry.valid(handle) || !m_Registry.all_of<ScriptComponent>(handle))
			{
				removed.push_back(handle);
			}
		}
		// The map's order differs between standard libraries; scripts run in the same order everywhere.
		std::sort(removed.begin(), removed.end());
		for (entt::entity const handle : removed)
		{
			DestroyScriptInstance(handle);
		}
	}

	std::vector<UUID> Scene::GetScriptInstanceIDs() const
	{
		// A snapshot (scripts create and destroy instances while it is walked) in the order of the Script components,
		// which is the same on every platform.
		std::vector<UUID> ids;
		ids.reserve(m_ScriptInstances.size());
		for (entt::entity const handle : m_Registry.view<ScriptComponent>())
		{
			auto const instance = m_ScriptInstances.find(handle);
			if (instance != m_ScriptInstances.end() && instance->second.HasInstance)
			{
				ids.push_back(instance->second.Entity);
			}
		}
		return ids;
	}
}
