#include "stpch.h"
#include "Strada/Scene/SceneSerializer.h"

#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/ComponentRegistry.h"

#include <algorithm>
#include <unordered_set>

namespace Strada
{
	static_assert(PrefabSerializer::FormatVersion == PrefabAsset::FormatVersion, "Prefab files are read by PrefabAsset");

	namespace
	{
		constexpr std::string_view IDComponentName = ComponentTraits<IDComponent>::Name;

		// Applies the unknown-field policy to a key that no setting matches.
		Result<void> HandleUnknownSetting(std::string_view key, DeserializationContext const& context)
		{
			switch (context.UnknownFields)
			{
				case UnknownFieldPolicy::Error:
					return MakeError("unknown scene setting '{}'", key);
				case UnknownFieldPolicy::Warn:
					context.Warn(fmt::format("ignored unknown scene setting '{}'", key));
					break;
				case UnknownFieldPolicy::Ignore:
					break;
			}
			return {};
		}

		// Reads the "ID" of every entity object, validating presence and uniqueness.
		Result<std::vector<UUID>> ReadEntityIDs(Json const& entities, DeserializationContext const& context)
		{
			std::vector<UUID> ids;
			ids.reserve(entities.size());
			std::unordered_set<UUID> seen;
			for (size_t i = 0; i < entities.size(); i++)
			{
				Json const& entity = entities[i];
				if (!entity.is_object())
				{
					return MakeError("Entities[{}] must be an object", i);
				}
				auto const idField = entity.find("ID");
				if (idField == entity.end())
				{
					return MakeError("Entities[{}] has no \"ID\"", i);
				}
				UUID id = UUID::Invalid();
				if (Result<void> result = JsonTraits<UUID>::FromJson(*idField, id, context); !result || !id.IsValid())
				{
					return MakeError("Entities[{}].ID must be a non-zero decimal string", i);
				}
				if (!seen.insert(id).second)
				{
					return MakeError("Entities[{}]: duplicate entity ID {}", i, id);
				}
				ids.push_back(id);
			}
			return ids;
		}

		// Rebuilds a consistent hierarchy: Parent links are authoritative; child lists keep their stored order for
		// children that point back, and gain missing children in document order. Dangling parents and cycles turn the
		// affected entities into roots.
		void RepairHierarchy(Scene& scene, std::vector<UUID> const& documentOrder, std::vector<UUID>& rootList,
		                     DeserializationContext const& context)
		{
			entt::registry& registry = scene.GetRegistry();
			auto const handleOf = [&scene](UUID id)
			{
				Entity const entity = scene.GetEntityByUUID(id);
				return entity ? entity.GetHandle() : entt::null;
			};

			for (UUID const id : documentOrder)
			{
				RelationshipComponent& relationship = registry.get<RelationshipComponent>(handleOf(id));
				if (relationship.Parent.IsValid() && handleOf(relationship.Parent) == entt::null)
				{
					context.Warn(fmt::format("entity {} referenced missing parent {}; it is now a root entity", id, relationship.Parent));
					relationship.Parent = UUID::Invalid();
				}
			}

			// Break cycles: walking up from an entity must not return to it.
			for (UUID const id : documentOrder)
			{
				UUID current = registry.get<RelationshipComponent>(handleOf(id)).Parent;
				for (size_t depth = 0; current.IsValid() && depth <= documentOrder.size(); depth++)
				{
					if (current == id)
					{
						context.Warn(fmt::format("entity {} was part of a parent cycle; it is now a root entity", id));
						registry.get<RelationshipComponent>(handleOf(id)).Parent = UUID::Invalid();
						break;
					}
					current = registry.get<RelationshipComponent>(handleOf(current)).Parent;
				}
			}

			// Children of every parent in document order, computed once (linear in the entity count).
			std::unordered_map<UUID, std::vector<UUID>> childrenByParent;
			for (UUID const id : documentOrder)
			{
				UUID const parent = registry.get<RelationshipComponent>(handleOf(id)).Parent;
				if (parent.IsValid())
				{
					childrenByParent[parent].push_back(id);
				}
			}

			for (UUID const id : documentOrder)
			{
				RelationshipComponent& relationship = registry.get<RelationshipComponent>(handleOf(id));
				std::vector<UUID> children;
				std::unordered_set<UUID> added;
				for (UUID const child : relationship.Children)
				{
					entt::entity const childHandle = handleOf(child);
					if (childHandle != entt::null && registry.get<RelationshipComponent>(childHandle).Parent == id &&
					    added.insert(child).second)
					{
						children.push_back(child);
					}
				}
				if (auto const it = childrenByParent.find(id); it != childrenByParent.end())
				{
					for (UUID const child : it->second)
					{
						if (added.insert(child).second)
						{
							children.push_back(child);
						}
					}
				}
				relationship.Children = std::move(children);
			}

			rootList.clear();
			for (UUID const id : documentOrder)
			{
				if (!registry.get<RelationshipComponent>(handleOf(id)).Parent.IsValid())
				{
					rootList.push_back(id);
				}
			}
		}
	}

	Json SceneSerializer::SerializeSettings(SceneSettings const& settings)
	{
		Json physics = Json::object();
		physics["Gravity"] = JsonTraits<glm::vec3>::ToJson(settings.Gravity);
		Json json = Json::object();
		json["Physics"] = std::move(physics);
		return json;
	}

	Result<void> SceneSerializer::DeserializeSettings(Json const& json, SceneSettings& settings, DeserializationContext const& context)
	{
		if (!json.is_object())
		{
			return Error{"Scene.Settings must be an object"};
		}

		SceneSettings updated = settings;
		for (auto const& item : json.items())
		{
			if (item.key() != "Physics")
			{
				if (Result<void> result = HandleUnknownSetting(item.key(), context); !result)
				{
					return result;
				}
				continue;
			}

			Json const& physics = item.value();
			if (!physics.is_object())
			{
				return Error{"Scene.Settings.Physics must be an object"};
			}
			for (auto const& physicsItem : physics.items())
			{
				if (physicsItem.key() == "Gravity")
				{
					if (Result<void> result = JsonTraits<glm::vec3>::FromJson(physicsItem.value(), updated.Gravity, context); !result)
					{
						return MakeError("Scene.Settings.Physics.Gravity: {}", result.GetError());
					}
				}
				else if (Result<void> result = HandleUnknownSetting("Physics." + physicsItem.key(), context); !result)
				{
					return result;
				}
			}
		}

		settings = updated;
		return {};
	}

	Json SceneSerializer::SerializeEntity(Scene const& scene, entt::entity handle)
	{
		entt::registry const& registry = scene.GetRegistry();
		Json components = Json::object();
		for (ComponentInfo const& component : ComponentRegistry::GetComponents())
		{
			if (component.Name != IDComponentName && component.Has(registry, handle))
			{
				components[std::string(component.Name)] = component.Serialize(registry, handle);
			}
		}

		Json entity = Json::object();
		entity["ID"] = JsonTraits<UUID>::ToJson(registry.get<IDComponent>(handle).ID);
		entity["Components"] = std::move(components);
		return entity;
	}

	std::vector<entt::entity> SceneSerializer::CollectHierarchy(Scene const& scene, std::vector<UUID> const& roots)
	{
		entt::registry const& registry = scene.GetRegistry();
		std::vector<entt::entity> result;
		std::vector<UUID> stack(roots.rbegin(), roots.rend());
		std::unordered_set<UUID> visited;
		while (!stack.empty())
		{
			UUID const id = stack.back();
			stack.pop_back();
			entt::entity const handle = scene.FindHandle(id);
			if (handle == entt::null || !visited.insert(id).second)
			{
				continue;
			}
			result.push_back(handle);
			std::vector<UUID> const& children = registry.get<RelationshipComponent>(handle).Children;
			stack.insert(stack.end(), children.rbegin(), children.rend());
		}
		return result;
	}

	Result<void> SceneSerializer::DeserializeComponents(Scene& scene, entt::entity handle, Json const& components,
	                                                    DeserializationContext const& context)
	{
		if (!components.is_object())
		{
			return Error{"\"Components\" must be an object"};
		}

		entt::registry& registry = scene.GetRegistry();
		for (auto const& item : components.items())
		{
			if (item.key() == IDComponentName)
			{
				continue;
			}

			ComponentInfo const* component = ComponentRegistry::Find(item.key());
			if (component == nullptr)
			{
				switch (context.UnknownFields)
				{
					case UnknownFieldPolicy::Error:
						return MakeError("unknown component '{}'", item.key());
					case UnknownFieldPolicy::Warn:
						context.Warn(fmt::format("ignored unknown component '{}'", item.key()));
						break;
					case UnknownFieldPolicy::Ignore:
						break;
				}
				continue;
			}

			if (Result<void> result = component->Deserialize(registry, handle, item.value(), context); !result)
			{
				return result;
			}
		}
		return {};
	}

	Result<Entity> SceneSerializer::DeserializeEntityHierarchy(Scene& scene, Json const& entities, Entity parent, size_t siblingIndex,
	                                                           DeserializationContext const& context)
	{
		if (parent && parent.GetScene() != &scene)
		{
			return Error{"the parent belongs to another scene"};
		}
		if (!entities.is_array() || entities.empty())
		{
			return Error{"expected a non-empty array of entities"};
		}

		Result<std::vector<UUID>> ids = ReadEntityIDs(entities, context);
		if (!ids)
		{
			return Error{ids.GetError()};
		}
		for (UUID const id : ids.GetValue())
		{
			if (scene.HasEntity(id))
			{
				return MakeError("an entity with ID {} already exists", id);
			}
		}

		entt::registry& registry = scene.GetRegistry();
		std::vector<entt::entity> created;
		created.reserve(ids.GetValue().size());
		for (size_t i = 0; i < entities.size(); i++)
		{
			entt::entity const handle = scene.CreateHandle(ids.GetValue()[i]);
			registry.emplace<TagComponent>(handle);
			registry.emplace<TransformComponent>(handle);
			registry.emplace<RelationshipComponent>(handle);
			created.push_back(handle);

			Json const& entity = entities[i];
			if (auto const components = entity.find("Components"); components != entity.end())
			{
				if (Result<void> result = DeserializeComponents(scene, handle, *components, context); !result)
				{
					for (entt::entity const createdHandle : created)
					{
						scene.m_EntityMap.erase(registry.get<IDComponent>(createdHandle).ID);
						registry.destroy(createdHandle);
					}
					return MakeError("entity {}: {}", ids.GetValue()[i], result.GetError());
				}
			}
		}

		// Links inside the subtree are kept; the root is attached below and links leaving the subtree are dropped.
		std::unordered_set<UUID> const members(ids.GetValue().begin(), ids.GetValue().end());
		for (size_t i = 0; i < created.size(); i++)
		{
			RelationshipComponent& relationship = registry.get<RelationshipComponent>(created[i]);
			if (i == 0 || !members.contains(relationship.Parent))
			{
				relationship.Parent = UUID::Invalid();
			}
		}

		std::vector<UUID> subtreeRoots;
		RepairHierarchy(scene, ids.GetValue(), subtreeRoots, context);
		Entity root(created.front(), &scene);
		for (UUID const orphan : subtreeRoots)
		{
			if (orphan != root.GetUUID())
			{
				registry.get<RelationshipComponent>(scene.FindHandle(orphan)).Parent = root.GetUUID();
				root.GetComponent<RelationshipComponent>().Children.push_back(orphan);
			}
		}

		std::vector<UUID>& siblings = parent ? parent.GetComponent<RelationshipComponent>().Children : scene.m_RootEntities;
		root.GetComponent<RelationshipComponent>().Parent = parent ? parent.GetUUID() : UUID::Invalid();
		siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(std::min(siblingIndex, siblings.size())), root.GetUUID());
		return root;
	}

	Json SceneSerializer::Serialize(Scene const& scene)
	{
		Json sceneInfo = Json::object();
		sceneInfo["Name"] = scene.GetName();
		sceneInfo["Settings"] = SerializeSettings(scene.GetSettings());

		Json entities = Json::array();
		for (entt::entity const handle : CollectHierarchy(scene, scene.GetRootEntityIDs()))
		{
			entities.push_back(SerializeEntity(scene, handle));
		}

		Json document = Json::object();
		document["Strada"] = MakeFileHeader("Scene", FormatVersion);
		document["Scene"] = std::move(sceneInfo);
		document["Entities"] = std::move(entities);
		return document;
	}

	Result<Ref<Scene>> SceneSerializer::Deserialize(Json const& document, DeserializationContext const& context)
	{
		if (Result<int> version = ReadFileHeader(document, "Scene", FormatVersion); !version)
		{
			return Error{version.GetError()};
		}

		Ref<Scene> scene = CreateRef<Scene>();
		if (auto const info = document.find("Scene"); info != document.end())
		{
			if (!info->is_object())
			{
				return Error{"\"Scene\" must be an object"};
			}
			if (auto const name = info->find("Name"); name != info->end())
			{
				if (!name->is_string())
				{
					return Error{"Scene.Name must be a string"};
				}
				scene->SetName(name->get<std::string>());
			}
			if (auto const settings = info->find("Settings"); settings != info->end())
			{
				if (Result<void> result = DeserializeSettings(*settings, scene->GetSettings(), context); !result)
				{
					return Error{result.GetError()};
				}
			}
		}

		auto const entitiesField = document.find("Entities");
		if (entitiesField == document.end())
		{
			return scene;
		}
		if (!entitiesField->is_array())
		{
			return Error{"\"Entities\" must be an array"};
		}

		Result<std::vector<UUID>> ids = ReadEntityIDs(*entitiesField, context);
		if (!ids)
		{
			return Error{ids.GetError()};
		}

		for (UUID const id : ids.GetValue())
		{
			scene->CreateEntityWithUUID(id);
		}

		for (size_t i = 0; i < entitiesField->size(); i++)
		{
			Json const& entity = (*entitiesField)[i];
			auto const components = entity.find("Components");
			if (components == entity.end())
			{
				continue;
			}
			entt::entity const handle = scene->FindHandle(ids.GetValue()[i]);
			if (Result<void> result = DeserializeComponents(*scene, handle, *components, context); !result)
			{
				return MakeError("entity {}: {}", ids.GetValue()[i], result.GetError());
			}
		}

		RepairHierarchy(*scene, ids.GetValue(), scene->m_RootEntities, context);
		return scene;
	}

	Result<void> SceneSerializer::SaveToFile(Scene const& scene, std::filesystem::path const& path)
	{
		return FileSystem::WriteTextFile(path, DumpJson(Serialize(scene)));
	}

	Result<Ref<Scene>> SceneSerializer::LoadFromFile(std::filesystem::path const& path, DeserializationContext const& context)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		if (!text)
		{
			return Error{text.GetError()};
		}
		Result<Json> document = ParseJson(text.GetValue());
		if (!document)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(path), document.GetError());
		}
		Result<Ref<Scene>> scene = Deserialize(document.GetValue(), context);
		if (!scene)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(path), scene.GetError());
		}
		return scene;
	}

	Json PrefabSerializer::Serialize(Scene const& scene, Entity root)
	{
		ST_CORE_ASSERT(root && root.GetScene() == &scene, "The prefab root must belong to the scene");

		Json entities = Json::array();
		for (entt::entity const handle : SceneSerializer::CollectHierarchy(scene, {root.GetUUID()}))
		{
			Json entity = SceneSerializer::SerializeEntity(scene, handle);
			if (handle == root.GetHandle())
			{
				entity["Components"][std::string(ComponentTraits<RelationshipComponent>::Name)]["Parent"] =
					JsonTraits<UUID>::ToJson(UUID::Invalid());
			}
			entities.push_back(std::move(entity));
		}

		Json document = Json::object();
		document["Strada"] = MakeFileHeader("Prefab", FormatVersion);
		document["Entities"] = std::move(entities);
		return document;
	}

	Result<Entity> PrefabSerializer::Instantiate(Scene& scene, Json const& document, AssetHandle prefab, Entity parent,
	                                             DeserializationContext const& context)
	{
		if (Result<int> version = ReadFileHeader(document, "Prefab", FormatVersion); !version)
		{
			return Error{version.GetError()};
		}
		if (parent && parent.GetScene() != &scene)
		{
			return Error{"the parent belongs to another scene"};
		}

		auto const entitiesField = document.find("Entities");
		if (entitiesField == document.end() || !entitiesField->is_array() || entitiesField->empty())
		{
			return Error{"a prefab needs a non-empty \"Entities\" array"};
		}

		Result<std::vector<UUID>> ids = ReadEntityIDs(*entitiesField, context);
		if (!ids)
		{
			return Error{ids.GetError()};
		}

		std::unordered_map<UUID, UUID> remap;
		std::unordered_set<UUID> assigned;
		for (UUID const id : ids.GetValue())
		{
			UUID newId = scene.GenerateUniqueID();
			while (!assigned.insert(newId).second)
			{
				newId = scene.GenerateUniqueID();
			}
			remap[id] = newId;
		}

		entt::registry& registry = scene.GetRegistry();
		std::vector<entt::entity> created;
		auto const rollback = [&scene, &registry, &created]()
		{
			for (entt::entity const handle : created)
			{
				scene.m_EntityMap.erase(registry.get<IDComponent>(handle).ID);
				registry.destroy(handle);
			}
		};

		for (size_t i = 0; i < entitiesField->size(); i++)
		{
			entt::entity const handle = scene.CreateHandle(remap.at(ids.GetValue()[i]));
			registry.emplace<TagComponent>(handle);
			registry.emplace<TransformComponent>(handle);
			registry.emplace<RelationshipComponent>(handle);
			created.push_back(handle);

			Json const& entity = (*entitiesField)[i];
			if (auto const components = entity.find("Components"); components != entity.end())
			{
				if (Result<void> result = SceneSerializer::DeserializeComponents(scene, handle, *components, context); !result)
				{
					rollback();
					return MakeError("prefab entity {}: {}", ids.GetValue()[i], result.GetError());
				}
			}
		}

		// Rewrite links to the new UUIDs; references leaving the prefab are dropped.
		std::vector<UUID> newIds;
		newIds.reserve(created.size());
		for (size_t i = 0; i < created.size(); i++)
		{
			RelationshipComponent& relationship = registry.get<RelationshipComponent>(created[i]);
			auto const parentIt = remap.find(relationship.Parent);
			relationship.Parent = i == 0 || parentIt == remap.end() ? UUID::Invalid() : parentIt->second;

			std::vector<UUID> children;
			for (UUID const child : relationship.Children)
			{
				if (auto const it = remap.find(child); it != remap.end())
				{
					children.push_back(it->second);
				}
			}
			relationship.Children = std::move(children);

			scene.RemapEntityReferences(created[i], remap);
			registry.emplace_or_replace<PrefabComponent>(created[i], PrefabComponent{prefab, ids.GetValue()[i]});
			newIds.push_back(registry.get<IDComponent>(created[i]).ID);
		}

		// Make the links consistent, then hang entities left without a parent under the prefab root.
		std::vector<UUID> prefabRoots;
		RepairHierarchy(scene, newIds, prefabRoots, context);
		Entity root(created.front(), &scene);
		for (UUID const orphan : prefabRoots)
		{
			if (orphan != root.GetUUID())
			{
				registry.get<RelationshipComponent>(scene.FindHandle(orphan)).Parent = root.GetUUID();
				root.GetComponent<RelationshipComponent>().Children.push_back(orphan);
			}
		}

		if (parent)
		{
			registry.get<RelationshipComponent>(root.GetHandle()).Parent = parent.GetUUID();
			parent.GetComponent<RelationshipComponent>().Children.push_back(root.GetUUID());
		}
		else
		{
			scene.m_RootEntities.push_back(root.GetUUID());
		}
		return root;
	}

	Result<void> PrefabSerializer::SaveToFile(Scene const& scene, Entity root, std::filesystem::path const& path)
	{
		return FileSystem::WriteTextFile(path, DumpJson(Serialize(scene, root)));
	}
}
