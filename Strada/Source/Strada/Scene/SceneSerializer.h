#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <filesystem>
#include <vector>

namespace Strada
{
	// Scene files (.sscene): { "Strada": header, "Scene": { "Name", "Settings" }, "Entities": [ ... ] }.
	// Entities are written in hierarchy order as { "ID": "<uuid>", "Components": { "<Name>": { fields } } }.
	class SceneSerializer
	{
	public:
		static constexpr int FormatVersion = 1;

		static Json Serialize(Scene const& scene);
		// Unknown components and fields follow context.UnknownFields; structural problems (bad header, missing or
		// duplicate IDs) are errors. Hierarchy inconsistencies are repaired and reported as warnings.
		[[nodiscard]] static Result<Ref<Scene>> Deserialize(Json const& document, DeserializationContext const& context);

		[[nodiscard]] static Result<void> SaveToFile(Scene const& scene, std::filesystem::path const& path);
		[[nodiscard]] static Result<Ref<Scene>> LoadFromFile(std::filesystem::path const& path, DeserializationContext const& context);

		// --- Shared with prefabs and the automation API ---

		static Json SerializeEntity(Scene const& scene, entt::entity handle);
		// Handles of the given roots and all their descendants, in hierarchy order.
		static std::vector<entt::entity> CollectHierarchy(Scene const& scene, std::vector<UUID> const& roots);
		// Applies a "Components" object ("ID" entries are ignored).
		[[nodiscard]] static Result<void> DeserializeComponents(Scene& scene, entt::entity handle, Json const& components,
		                                                        DeserializationContext const& context);
	};

	// Prefab files (.sprefab): { "Strada": header, "Entities": [ root, descendants... ] }.
	class PrefabSerializer
	{
	public:
		static constexpr int FormatVersion = 1;

		// The entity and its descendants; the root is written without a parent.
		static Json Serialize(Scene const& scene, Entity root);
		// Creates a new instance with fresh UUIDs under parent (a root entity when parent is invalid). Every created
		// entity gets a PrefabComponent linking it to the prefab and to its source entity. Nothing is created on failure.
		[[nodiscard]] static Result<Entity> Instantiate(Scene& scene, Json const& document, AssetHandle prefab, Entity parent,
		                                                DeserializationContext const& context);

		[[nodiscard]] static Result<void> SaveToFile(Scene const& scene, Entity root, std::filesystem::path const& path);
	};
}
