#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/VersionedDocument.h"

#include <filesystem>
#include <vector>

namespace Strada
{
	// Scene files (.sscene): { "Strada": header, "Scene": { "Name", "Settings" }, "Entities": [ ... ] }.
	// Entities are written in hierarchy order as { "ID": "<uuid>", "Components": { "<Name>": { fields } } }.
	class SceneSerializer
	{
	public:
		// The version scene files are written in; readers read every version up to it (GetFormat).
		static constexpr int FormatVersion = 2;

		// The scene format: its type, its versions and their migrations (Docs/Architecture.md §7.3, "Versions and
		// migrations"). Version 2 lets script fields and visual script variables hold lists; a version-1 scene is read as it
		// is. Any thread.
		[[nodiscard]] static VersionedDocumentFormat const& GetFormat();

		// A document of the current version.
		static Json Serialize(Scene const& scene);
		// Reads a document of any version up to FormatVersion, migrated to the current one first (a version that needs no
		// migration is read in place; ReadMigratedDocument). Unknown components and fields follow context.UnknownFields;
		// structural problems (bad header, a newer version, missing or duplicate IDs) and values a component refuses are
		// errors. Hierarchy inconsistencies are repaired and reported as warnings.
		[[nodiscard]] static Result<Ref<Scene>> Deserialize(Json const& document, DeserializationContext const& context);

		[[nodiscard]] static Result<void> SaveToFile(Scene const& scene, std::filesystem::path const& path);
		// Reads the file, migrates its document in place (no copy, whatever the migrations) and deserializes it.
		[[nodiscard]] static Result<Ref<Scene>> LoadFromFile(std::filesystem::path const& path, DeserializationContext const& context);

		// --- Shared with prefabs, the editor and the automation API ---

		// The "Settings" object of a scene file.
		static Json SerializeSettings(SceneSettings const& settings);
		// Applies the settings present in the object (missing ones keep their values, so this is also a partial update);
		// unknown keys follow context.UnknownFields. Nothing changes when the object is invalid.
		[[nodiscard]] static Result<void> DeserializeSettings(Json const& json, SceneSettings& settings,
		                                                      DeserializationContext const& context);

		static Json SerializeEntity(Scene const& scene, entt::entity handle);
		// Handles of the given roots and all their descendants, in hierarchy order.
		static std::vector<entt::entity> CollectHierarchy(Scene const& scene, std::vector<UUID> const& roots);
		// Applies a "Components" object ("ID" entries are ignored). Like every entity function here, it reads the current
		// version's form: entity JSON without a file header (undo snapshots, automation input) is never migrated.
		[[nodiscard]] static Result<void> DeserializeComponents(Scene& scene, entt::entity handle, Json const& components,
		                                                        DeserializationContext const& context);
		// Recreates an entity subtree written with SerializeEntity (root first, descendants in CollectHierarchy order) with
		// its original UUIDs, none of which may exist in the scene. The root is attached to parent (or becomes a root
		// entity when parent is invalid) at siblingIndex (clamped to the end); descendants keep their stored links, and
		// descendants whose parent is not part of the subtree are attached to the root. Nothing is created on failure.
		[[nodiscard]] static Result<Entity> DeserializeEntityHierarchy(Scene& scene, Json const& entities, Entity parent,
		                                                               size_t siblingIndex, DeserializationContext const& context);
	};

	// Prefab files (.sprefab): { "Strada": header, "Entities": [ root, descendants... ] }. The format and its versions are
	// PrefabAsset's (PrefabAsset::GetFormat), which reads the files.
	class PrefabSerializer
	{
	public:
		static constexpr int FormatVersion = PrefabAsset::FormatVersion;

		// The entity and its descendants, in a document of the current version; the root is written without a parent.
		static Json Serialize(Scene const& scene, Entity root);
		// Creates a new instance with fresh UUIDs under parent (a root entity when parent is invalid), from a document of any
		// version up to FormatVersion (migrated as SceneSerializer::Deserialize migrates scenes; an asset's document is
		// already current). Every created entity gets a PrefabComponent linking it to the prefab and to its source entity,
		// and references between the prefab's entities (hierarchy links, entity fields and the elements of entity lists)
		// are remapped to the new UUIDs. Nothing is created on failure.
		[[nodiscard]] static Result<Entity> Instantiate(Scene& scene, Json const& document, AssetHandle prefab, Entity parent,
		                                                DeserializationContext const& context);

		[[nodiscard]] static Result<void> SaveToFile(Scene const& scene, Entity root, std::filesystem::path const& path);
	};
}
