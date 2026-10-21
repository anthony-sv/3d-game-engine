#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <algorithm>
#include <utility>

namespace Strada
{
	namespace
	{
		// The kinds of script instance an entity can have, in the order its events reach them.
		enum class ScriptKind : uint8_t
		{
			CSharp = 0,
			Visual
		};
	}

	void Scene::StartScripts()
	{
		auto const scripts = m_Registry.view<ScriptComponent>();
		bool const hasScripts = scripts.begin() != scripts.end();
		auto const visualScripts = m_Registry.view<VisualScriptComponent>();
		bool const hasVisualScripts = visualScripts.begin() != visualScripts.end();
		if (!ScriptEngine::IsInitialized())
		{
			if (hasScripts || hasVisualScripts)
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
		// Visual scripts need no game assembly. C# scripts need the one loaded now, for the whole run: one loaded later (a
		// build finished while playing) does not start them.
		m_RunsScriptComponents = ScriptEngine::HasGameAssembly();
		if (!m_RunsScriptComponents)
		{
			if (!hasVisualScripts)
			{
				if (hasScripts)
				{
					ST_CORE_WARN("Scene '{}' runs without scripts: no game assembly is loaded", m_Name);
				}
				return;
			}
			if (hasScripts)
			{
				ST_CORE_WARN("Scene '{}' runs without C# scripts: no game assembly is loaded", m_Name);
			}
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
		// OnDestroy runs while the scene, its physics and its audio are intact: first for instances whose component went
		// since the last update, then for every C# script, then for every visual script.
		m_StoppingScripts = true;
		DestroyRemovedScriptInstances();
		std::vector<UUID> const scripts = GetScriptInstanceIDs();
		std::vector<UUID> const visualScripts = GetVisualScriptInstanceIDs();
		for (UUID const id : scripts)
		{
			ScriptEngine::InvokeOnDestroy(id);
		}
		for (UUID const id : visualScripts)
		{
			VisualScriptEngine::InvokeOnDestroy(id);
		}
		ScriptEngine::DestroyAllInstances();
		VisualScriptEngine::DestroyAllInstances();
		m_StoppingScripts = false;
		m_ScriptInstances.clear();
		m_VisualScriptInstances.clear();
		ScriptEngine::SetSceneContext(nullptr);
		m_RunsScripts = false;
		m_RunsScriptComponents = false;
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
		if (m_RunsScriptComponents)
		{
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
		}
		std::vector<entt::entity> replacedVisualScripts;
		std::vector<PendingVisualScriptInstance> pendingVisualScripts;
		CollectVisualScriptChanges(replacedVisualScripts, pendingVisualScripts);

		DestroyRemovedScriptInstances();
		for (entt::entity const handle : replaced)
		{
			DestroyScriptInstance(handle);
		}
		for (entt::entity const handle : replacedVisualScripts)
		{
			DestroyVisualScriptInstance(handle);
		}
		CreateScriptInstances(pending, pendingVisualScripts);
	}

	void Scene::CreateScriptInstances(std::vector<PendingScriptInstance> const& scripts,
	                                  std::vector<PendingVisualScriptInstance> const& visualScripts)
	{
		std::vector<UUID> created;
		for (PendingScriptInstance const& instance : scripts)
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
		std::vector<UUID> const createdVisualScripts = CreateVisualScriptInstances(visualScripts);
		// Every new instance of both kinds exists before the first OnCreate, so scripts can find each other.
		for (UUID const id : created)
		{
			ScriptEngine::InvokeOnCreate(id);
		}
		for (UUID const id : createdVisualScripts)
		{
			VisualScriptEngine::InvokeOnCreate(id);
		}
	}

	void Scene::StartScriptsOf(std::vector<entt::entity> const& handles)
	{
		if (!m_RunsScripts || m_StoppingScripts)
		{
			return;
		}
		std::vector<PendingScriptInstance> scripts;
		std::vector<PendingVisualScriptInstance> visualScripts;
		for (entt::entity const handle : handles)
		{
			UUID const id = m_Registry.get<IDComponent>(handle).ID;
			if (ScriptComponent const* script = m_Registry.try_get<ScriptComponent>(handle); script != nullptr && m_RunsScriptComponents)
			{
				scripts.push_back({handle, id, script->ClassName, script->Fields});
			}
			if (VisualScriptComponent const* visualScript = m_Registry.try_get<VisualScriptComponent>(handle))
			{
				visualScripts.push_back({handle, id, visualScript->Graph, visualScript->Variables});
			}
		}
		CreateScriptInstances(scripts, visualScripts);
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
		// The instances of both kinds as the update starts: the C# scripts run first.
		std::vector<UUID> const scripts = GetScriptInstanceIDs();
		std::vector<UUID> const visualScripts = GetVisualScriptInstanceIDs();
		for (UUID const id : scripts)
		{
			ScriptEngine::InvokeOnUpdate(id, deltaTime);
		}
		for (UUID const id : visualScripts)
		{
			VisualScriptEngine::InvokeOnUpdate(id, deltaTime);
		}
	}

	void Scene::FixedUpdateScripts(float fixedDeltaTime)
	{
		if (!m_RunsScripts)
		{
			return;
		}
		std::vector<UUID> const scripts = GetScriptInstanceIDs();
		std::vector<UUID> const visualScripts = GetVisualScriptInstanceIDs();
		for (UUID const id : scripts)
		{
			ScriptEngine::InvokeOnFixedUpdate(id, fixedDeltaTime);
		}
		for (UUID const id : visualScripts)
		{
			VisualScriptEngine::InvokeOnFixedUpdate(id, fixedDeltaTime);
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
			VisualScriptEngine::InvokeContactEvent(event.First, event.Second, event.Type);
			ScriptEngine::InvokeContactEvent(event.Second, event.First, event.Type);
			VisualScriptEngine::InvokeContactEvent(event.Second, event.First, event.Type);
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
		std::vector<std::pair<entt::entity, ScriptKind>> removed;
		for (auto const& [handle, state] : m_ScriptInstances)
		{
			if (!m_Registry.valid(handle) || !m_Registry.all_of<ScriptComponent>(handle))
			{
				removed.emplace_back(handle, ScriptKind::CSharp);
			}
		}
		for (auto const& [handle, state] : m_VisualScriptInstances)
		{
			if (!m_Registry.valid(handle) || !m_Registry.all_of<VisualScriptComponent>(handle))
			{
				removed.emplace_back(handle, ScriptKind::Visual);
			}
		}
		// The maps' order differs between standard libraries; scripts run in the same order everywhere.
		std::sort(removed.begin(), removed.end());
		for (auto const& [handle, kind] : removed)
		{
			if (kind == ScriptKind::CSharp)
			{
				DestroyScriptInstance(handle);
			}
			else
			{
				DestroyVisualScriptInstance(handle);
			}
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
