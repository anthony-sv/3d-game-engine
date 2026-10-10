#include "stpch.h"
#include "Strada/Script/ScriptGlue.h"

#include "Strada/Scene/ComponentRegistry.h"

#include <algorithm>
#include <string>

namespace Strada::ScriptGlue
{
	namespace
	{
		void Log_Write(int32_t level, char const* text, int32_t length)
		{
			spdlog::level::level_enum severity = spdlog::level::info;
			switch (static_cast<LogLevel>(level))
			{
				case LogLevel::Trace:
					severity = spdlog::level::trace;
					break;
				case LogLevel::Info:
					severity = spdlog::level::info;
					break;
				case LogLevel::Warn:
					severity = spdlog::level::warn;
					break;
				case LogLevel::Error:
					severity = spdlog::level::err;
					break;
				case LogLevel::Critical:
					severity = spdlog::level::critical;
					break;
			}
			Log::GetScriptLogger().log(severity, "{}", ToStringView(text, length));
		}

		uint8_t Entity_IsValid(uint64_t id)
		{
			Scene* const scene = ScriptEngine::GetSceneContext();
			return scene != nullptr && scene->HasEntity(UUID(id)) ? 1 : 0;
		}

		// The name stays valid until the entity is renamed or destroyed; the runtime copies it at once.
		char const* Entity_GetName(uint64_t id, int32_t* length)
		{
			*length = 0;
			Entity const entity = FindEntity(id, "Entity.Name");
			if (!entity)
			{
				return nullptr;
			}
			std::string const& name = entity.GetName();
			*length = static_cast<int32_t>(name.size());
			return name.data();
		}

		void Entity_SetName(uint64_t id, char const* name, int32_t length)
		{
			if (Entity entity = FindEntity(id, "Entity.Name"))
			{
				entity.GetComponent<TagComponent>().Tag = ToStringView(name, length);
			}
		}

		ComponentInfo const* FindComponentInfo(char const* name, int32_t length, std::string_view function)
		{
			ComponentInfo const* const component = ComponentRegistry::Find(ToStringView(name, length));
			if (component == nullptr)
			{
				Log::GetScriptLogger().error("{}: there is no component {}", function, ToStringView(name, length));
			}
			return component;
		}

		uint8_t Entity_HasComponent(uint64_t id, char const* name, int32_t length)
		{
			Entity const entity = FindEntity(id, "Entity.HasComponent");
			ComponentInfo const* const component = entity ? FindComponentInfo(name, length, "Entity.HasComponent") : nullptr;
			return component != nullptr && component->Has(entity.GetScene()->GetRegistry(), entity.GetHandle()) ? 1 : 0;
		}

		// Adding a component the entity has keeps it.
		void Entity_AddComponent(uint64_t id, char const* name, int32_t length)
		{
			Entity const entity = FindEntity(id, "Entity.AddComponent");
			ComponentInfo const* const component = entity ? FindComponentInfo(name, length, "Entity.AddComponent") : nullptr;
			if (component == nullptr)
			{
				return;
			}
			if (component->IsInternal())
			{
				Log::GetScriptLogger().error("Entity.AddComponent: {} components are managed by the engine", component->Name);
				return;
			}
			component->Add(entity.GetScene()->GetRegistry(), entity.GetHandle());
		}

		void Entity_RemoveComponent(uint64_t id, char const* name, int32_t length)
		{
			Entity const entity = FindEntity(id, "Entity.RemoveComponent");
			ComponentInfo const* const component = entity ? FindComponentInfo(name, length, "Entity.RemoveComponent") : nullptr;
			if (component == nullptr)
			{
				return;
			}
			if (component->IsCore() || component->IsInternal())
			{
				Log::GetScriptLogger().error("Entity.RemoveComponent: every entity keeps its {} component", component->Name);
				return;
			}
			component->Remove(entity.GetScene()->GetRegistry(), entity.GetHandle());
		}

		uint64_t Entity_Create(char const* name, int32_t length)
		{
			Scene* const scene = GetScene("Entity.Create");
			return scene != nullptr ? scene->CreateEntity(std::string(ToStringView(name, length))).GetUUID().GetValue() : 0;
		}

		uint64_t Entity_Instantiate(uint64_t prefab, Vector3 const* position, Quaternion const* rotation, uint64_t parentId)
		{
			if (!CheckFinite("Entity.Instantiate", *position, *rotation))
			{
				return 0;
			}
			Scene* const scene = GetScene("Entity.Instantiate");
			if (scene == nullptr)
			{
				return 0;
			}
			Entity parent;
			if (parentId != 0)
			{
				parent = FindEntity(parentId, "Entity.Instantiate");
				if (!parent)
				{
					return 0;
				}
			}
			Result<Entity> instance =
				scene->InstantiatePrefab(AssetHandle(UUID(prefab)), FromScript(*position), ToRotation(*rotation), parent);
			if (!instance)
			{
				Log::GetScriptLogger().error("Entity.Instantiate: {}", instance.GetError());
				return 0;
			}
			return instance.GetValue().GetUUID().GetValue();
		}

		void Entity_Destroy(uint64_t id)
		{
			if (Entity const entity = FindEntity(id, "Entity.Destroy"))
			{
				entity.GetScene()->DestroyEntity(entity);
			}
		}

		uint64_t Entity_FindByName(char const* name, int32_t length)
		{
			Scene* const scene = GetScene("Entity.FindByName");
			if (scene == nullptr)
			{
				return 0;
			}
			Entity const entity = scene->FindEntityByName(ToStringView(name, length));
			return entity ? entity.GetUUID().GetValue() : 0;
		}

		uint64_t Entity_GetParent(uint64_t id)
		{
			Entity const entity = FindEntity(id, "Entity.Parent");
			return entity ? entity.GetComponent<RelationshipComponent>().Parent.GetValue() : 0;
		}

		// A parent of 0 moves the entity to the root. The world transform is kept.
		void Entity_SetParent(uint64_t id, uint64_t parentId)
		{
			Entity const entity = FindEntity(id, "Entity.Parent");
			if (!entity)
			{
				return;
			}
			Entity parent;
			if (parentId != 0)
			{
				parent = FindEntity(parentId, "Entity.Parent");
				if (!parent)
				{
					return;
				}
			}
			if (Result<void> result = entity.GetScene()->SetParent(entity, parent); !result)
			{
				Log::GetScriptLogger().error("Entity.Parent: {}", result.GetError());
			}
		}

		// Copies up to capacity child IDs (in order) and returns how many children there are.
		int32_t Entity_GetChildren(uint64_t id, uint64_t* children, int32_t capacity)
		{
			Entity const entity = FindEntity(id, "Entity.Children");
			if (!entity)
			{
				return 0;
			}
			std::vector<UUID> const& ids = entity.GetComponent<RelationshipComponent>().Children;
			size_t const count = std::min(ids.size(), static_cast<size_t>(std::max(capacity, 0)));
			for (size_t index = 0; index < count; index++)
			{
				children[index] = ids[index].GetValue();
			}
			return static_cast<int32_t>(ids.size());
		}
	}

	void RegisterEntityBindings(BindingTable& table)
	{
		table.Add("Log_Write", &Log_Write);
		table.Add("Entity_IsValid", &Entity_IsValid);
		table.Add("Entity_GetName", &Entity_GetName);
		table.Add("Entity_SetName", &Entity_SetName);
		table.Add("Entity_HasComponent", &Entity_HasComponent);
		table.Add("Entity_AddComponent", &Entity_AddComponent);
		table.Add("Entity_RemoveComponent", &Entity_RemoveComponent);
		table.Add("Entity_Create", &Entity_Create);
		table.Add("Entity_Instantiate", &Entity_Instantiate);
		table.Add("Entity_Destroy", &Entity_Destroy);
		table.Add("Entity_FindByName", &Entity_FindByName);
		table.Add("Entity_GetParent", &Entity_GetParent);
		table.Add("Entity_SetParent", &Entity_SetParent);
		table.Add("Entity_GetChildren", &Entity_GetChildren);
	}
}
