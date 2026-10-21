#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptVariables.h"

#include <algorithm>
#include <utility>

namespace Strada
{
	namespace
	{
		// A VisualScript component's stored values under the current names of its graph's variables, as the inspector shows
		// them: a value stored under a former name applies to the renamed variable, and of two values for one variable the
		// one under its current name counts. As stored while the graph does not load.
		ScriptFieldMap NormalizeVariables(AssetHandle graph, ScriptFieldMap variables)
		{
			if (!graph.IsValid() || !AssetManager::IsInitialized() || AssetManager::GetAssetType(graph) != AssetType::VisualScript)
			{
				return variables;
			}
			Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(graph);
			if (asset == nullptr)
			{
				return variables;
			}
			return NormalizeVisualScriptOverrides(asset->GetDocument(), std::move(variables));
		}
	}

	void Scene::CollectVisualScriptChanges(std::vector<entt::entity>& replaced, std::vector<PendingVisualScriptInstance>& pending)
	{
		// Each graph once, in the same order everywhere. Loading runs managed code, so the graphs are collected first.
		std::vector<AssetHandle> graphs;
		for (entt::entity const handle : m_Registry.view<VisualScriptComponent>())
		{
			if (AssetHandle const graph = m_Registry.get<VisualScriptComponent>(handle).Graph; graph.IsValid())
			{
				graphs.push_back(graph);
			}
		}
		std::sort(graphs.begin(), graphs.end(),
		          [](AssetHandle a, AssetHandle b)
		          {
					  return a.GetUUID().GetValue() < b.GetUUID().GetValue();
				  });
		graphs.erase(std::unique(graphs.begin(), graphs.end()), graphs.end());
		for (AssetHandle const graph : graphs)
		{
			// A graph that is absent is reported by the entities it keeps from starting; its running instances run on.
			static_cast<void>(VisualScriptEngine::EnsureLoaded(graph));
		}

		for (auto const [handle, id, component] : m_Registry.view<IDComponent, VisualScriptComponent>().each())
		{
			auto const current = m_VisualScriptInstances.find(handle);
			if (current != m_VisualScriptInstances.end() && current->second.Graph == component.Graph &&
			    (current->second.HasInstance || current->second.Generation == VisualScriptEngine::GetProgramGeneration(component.Graph)))
			{
				continue;
			}
			if (current != m_VisualScriptInstances.end())
			{
				replaced.push_back(handle);
			}
			pending.push_back({handle, id.ID, component.Graph, component.Variables});
		}
	}

	std::vector<UUID> Scene::CreateVisualScriptInstances(std::vector<PendingVisualScriptInstance> const& pending)
	{
		// Making a graph instance runs no graph code (OnCreate comes later), so the map changes only here.
		std::vector<UUID> created;
		for (PendingVisualScriptInstance const& instance : pending)
		{
			ScriptFieldMap variables = NormalizeVariables(instance.Graph, instance.Variables);
			// A component without a graph runs nothing, like a Script component without a class.
			if (!instance.Graph.IsValid())
			{
				m_VisualScriptInstances[instance.Handle] = {instance.Entity, instance.Graph, 0, false, std::move(variables)};
				continue;
			}
			Result<void> const result = VisualScriptEngine::CreateInstance(instance.Entity, instance.Graph, variables);
			// The generation as the graph was loaded for the instance: one that could not start tries again when it changes.
			m_VisualScriptInstances[instance.Handle] = {instance.Entity, instance.Graph,
			                                            VisualScriptEngine::GetProgramGeneration(instance.Graph), result.IsOk(),
			                                            std::move(variables)};
			if (!result)
			{
				// A graph that does not run was reported by the runtime (a script exception); an absent one only here.
				ST_CORE_WARN("Entity '{}' runs no visual script: {}", Entity(instance.Handle, this).GetName(), result.GetError());
				continue;
			}
			created.push_back(instance.Entity);
		}
		return created;
	}

	void Scene::DestroyVisualScriptInstance(entt::entity handle)
	{
		auto const instance = m_VisualScriptInstances.find(handle);
		if (instance == m_VisualScriptInstances.end())
		{
			return;
		}
		UUID const entity = instance->second.Entity;
		bool const hasInstance = instance->second.HasInstance;
		m_VisualScriptInstances.erase(instance);
		if (hasInstance)
		{
			VisualScriptEngine::InvokeOnDestroy(entity);
		}
	}

	std::vector<UUID> Scene::GetVisualScriptInstanceIDs() const
	{
		// A snapshot in the order of the VisualScript components, which is the same on every platform.
		std::vector<UUID> ids;
		ids.reserve(m_VisualScriptInstances.size());
		for (entt::entity const handle : m_Registry.view<VisualScriptComponent>())
		{
			auto const instance = m_VisualScriptInstances.find(handle);
			if (instance != m_VisualScriptInstances.end() && instance->second.HasInstance)
			{
				ids.push_back(instance->second.Entity);
			}
		}
		return ids;
	}

	Result<ScriptFieldMap> Scene::ReadVisualScriptVariables(Entity entity)
	{
		bool const ofThisScene = entity && entity.GetScene() == this;
		auto const state = ofThisScene ? m_VisualScriptInstances.find(entity.GetHandle()) : m_VisualScriptInstances.end();
		VisualScriptComponent* const component = ofThisScene ? entity.TryGetComponent<VisualScriptComponent>() : nullptr;
		if (!m_RunsScripts || state == m_VisualScriptInstances.end() || !state->second.HasInstance || component == nullptr ||
		    state->second.Entity != entity.GetUUID() || state->second.Graph != component->Graph)
		{
			return Error{"the entity runs no visual script"};
		}
		Result<ScriptFieldMap> variables = VisualScriptEngine::GetInstanceVariables(state->second.Entity);
		if (!variables)
		{
			return variables;
		}
		for (auto const& [name, value] : variables.GetValue())
		{
			component->Variables[name] = value;
			state->second.Variables[name] = value;
		}
		// The values just read are under the current names: values stored under former names of the same variables go.
		component->Variables = NormalizeVariables(component->Graph, std::move(component->Variables));
		return variables;
	}

	void Scene::ApplyVisualScriptVariableChanges()
	{
		if (!m_RunsScripts)
		{
			return;
		}
		// Setting variables runs no graph code, so the registry can be walked meanwhile.
		for (auto const [handle, component] : m_Registry.view<VisualScriptComponent>().each())
		{
			auto const state = m_VisualScriptInstances.find(handle);
			if (state == m_VisualScriptInstances.end() || !state->second.HasInstance || state->second.Graph != component.Graph)
			{
				continue;
			}
			ScriptFieldMap const values = NormalizeVariables(component.Graph, component.Variables);
			ScriptFieldMap changed;
			for (auto const& [name, value] : values)
			{
				auto const given = state->second.Variables.find(name);
				if (given == state->second.Variables.end() || !(given->second == value))
				{
					changed.emplace(name, value);
					state->second.Variables[name] = value;
				}
			}
			if (changed.empty())
			{
				continue;
			}
			if (Result<void> const set = VisualScriptEngine::SetInstanceVariables(state->second.Entity, changed); !set)
			{
				ST_CORE_WARN("Entity '{}': {}", Entity(handle, this).GetName(), set.GetError());
			}
		}
	}
}
