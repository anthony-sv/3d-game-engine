#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptVariables.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace Strada
{
	namespace
	{
		// Stored values under the current names of the variables of the document a graph's instances run (its working
		// program's, VisualScriptEngine::GetWorkingDocument): a value stored under a former name applies to the renamed
		// variable, and of two values for one variable the one under its current name counts. As stored without a document
		// (the graph has no program that runs, so it has no instances).
		ScriptFieldMap NormalizeVariables(Ref<VisualScriptDocument const> const& document, ScriptFieldMap variables)
		{
			if (document == nullptr)
			{
				return variables;
			}
			return NormalizeVisualScriptOverrides(*document, std::move(variables));
		}

		bool IsPublicVariable(VisualScriptDocument const& document, std::string_view name)
		{
			return std::any_of(document.Variables.begin(), document.Variables.end(),
			                   [name](VisualScriptVariable const& variable)
			                   {
								   return variable.Public && variable.Name == name;
							   });
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
			if (current != m_VisualScriptInstances.end() && current->second.Graph == component.Graph && current->second.HasInstance)
			{
				// A live edit just loaded: values edited before it that only its program can take reach the instance before it
				// runs (setting variables runs no graph code).
				UpdateVisualScriptVariables(handle, current->second, component, false);
				continue;
			}
			if (current != m_VisualScriptInstances.end() && current->second.Graph == component.Graph &&
			    current->second.Generation == VisualScriptEngine::GetProgramGeneration(component.Graph))
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
			// A component without a graph runs nothing, like a Script component without a class.
			if (!instance.Graph.IsValid())
			{
				m_VisualScriptInstances[instance.Handle] = {instance.Entity, instance.Graph, 0, false, instance.Variables};
				continue;
			}
			// The values go under the names of the program the instance will run, so the graph is loaded first (CreateInstance
			// reports a graph that is absent).
			static_cast<void>(VisualScriptEngine::EnsureLoaded(instance.Graph));
			ScriptFieldMap variables = NormalizeVariables(VisualScriptEngine::GetWorkingDocument(instance.Graph), instance.Variables);
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
		// Edits that waited for the program the instance runs now reach it before its values are read over them.
		UpdateVisualScriptVariables(entity.GetHandle(), state->second, *component, false);
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
		// The values just read are under the current names of the program's variables: values stored under former names of
		// the same variables go. Values of variables only a newer document has stay (the inspector shows that document).
		component->Variables =
			NormalizeVariables(VisualScriptEngine::GetWorkingDocument(component->Graph), std::move(component->Variables));
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
			if (state != m_VisualScriptInstances.end() && state->second.HasInstance && state->second.Graph == component.Graph)
			{
				UpdateVisualScriptVariables(handle, state->second, component, true);
			}
		}
	}

	void Scene::UpdateVisualScriptVariables(entt::entity handle, VisualScriptInstanceState& state, VisualScriptComponent const& component,
	                                        bool giveEdits)
	{
		// A live edit gave the instance another program (a new generation), whose variables carry over by ID or name: the
		// baseline follows renames recorded in FormerNames, so stored values that were given before are not given again
		// over the values the variables carried, while values edited meanwhile, including those of variables the edit adds,
		// differ from it and are given now. A generation without a new program (a broken document) changes nothing.
		uint64_t const generation = VisualScriptEngine::GetProgramGeneration(component.Graph);
		bool const followsEdit = generation != state.Generation;
		if (!followsEdit && !giveEdits)
		{
			return;
		}
		Ref<VisualScriptDocument const> const document = VisualScriptEngine::GetWorkingDocument(component.Graph);
		if (followsEdit)
		{
			state.Variables = NormalizeVariables(document, std::move(state.Variables));
			state.Generation = generation;
		}
		ScriptFieldMap const values = NormalizeVariables(document, component.Variables);
		ScriptFieldMap changed;
		for (auto const& [name, value] : values)
		{
			auto const given = state.Variables.find(name);
			if (given != state.Variables.end() && given->second == value)
			{
				continue;
			}
			// The program has no such variable (the runtime would skip the value): the value waits for an edit that adds it.
			if (document != nullptr && !IsPublicVariable(*document, name))
			{
				continue;
			}
			changed.emplace(name, value);
			state.Variables[name] = value;
		}
		if (changed.empty())
		{
			return;
		}
		if (Result<void> const set = VisualScriptEngine::SetInstanceVariables(state.Entity, changed); !set)
		{
			ST_CORE_WARN("Entity '{}': {}", Entity(handle, this).GetName(), set.GetError());
		}
	}
}
