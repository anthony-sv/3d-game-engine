#pragma once

#include "Editor/Commands/ComponentCommands.h"
#include "Editor/Commands/EntityCommands.h"
#include "Editor/EditorContext.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/ComponentSerialization.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// Every editing operation of the editor, used alike by the UI panels and the automation commands. Scene modifications
	// are undoable commands executed through the context's history; scene files and the selection are handled directly.
	// Failures leave the scene unchanged and return a message for the user. Main thread only.
	class EditorOperations
	{
	public:
		explicit EditorOperations(EditorContext& context);

		EditorContext& GetContext() { return m_Context; }
		// Invalid Entity when the ID does not exist in the edited scene.
		Entity FindEntity(UUID id);

		// --- Projects (not undoable) ---

		// Creates a project in an empty or new directory (see Project::Create) and opens it with its start scene.
		[[nodiscard]] Result<void> CreateProject(std::filesystem::path const& directory, std::string const& name, Scene const& startScene);
		// Opens a project and its start scene (an empty scene when it has none or it cannot be read). Returns warnings
		// (skipped settings, scene problems). When opening fails, the previous project stays open if its assets still are.
		[[nodiscard]] Result<std::vector<std::string>> OpenProject(std::filesystem::path const& file);
		// Closes the project and its asset directory; the scene is replaced with an empty one.
		void CloseProject();
		// Applies a partial patch in the project-file format to the open project's settings, without saving.
		[[nodiscard]] Result<void> ApplyProjectSettings(Json const& patch);
		// Writes the open project's file.
		[[nodiscard]] Result<void> SaveProject();

		// --- Scene files (not undoable: they replace the scene and clear the history) ---

		void NewScene(std::string name = "Untitled");
		// Loads a scene file; unknown components and fields (files from newer versions) are skipped with warnings, which
		// are logged and returned.
		[[nodiscard]] Result<std::vector<std::string>> OpenScene(std::filesystem::path const& path);
		// Saves to path, or to the current scene file when path is empty; the scene file becomes that path. Scenes saved
		// inside the project's asset directory are registered as assets.
		[[nodiscard]] Result<void> SaveScene(std::filesystem::path const& path = {});

		// --- Scene properties ---

		// Renames the scene and/or applies a partial patch to its settings ("Settings" object of scene files).
		[[nodiscard]] Result<void> SetSceneProperties(std::optional<std::string> name, Json const& settingsPatch, uint64_t mergeKey = 0);

		// --- Entities ---

		[[nodiscard]] Result<UUID> CreateEntity(EntityCreateInfo info);
		[[nodiscard]] Result<void> DeleteEntities(std::span<UUID const> entities);
		// Returns the UUIDs of the copies, one per duplicated subtree, in request order.
		[[nodiscard]] Result<std::vector<UUID>> DuplicateEntities(std::span<UUID const> entities);
		[[nodiscard]] Result<void> RenameEntity(UUID entity, std::string name, uint64_t mergeKey = 0);
		// Gives several entities the same name as one undo step.
		[[nodiscard]] Result<void> RenameEntities(std::span<UUID const> entities, std::string name, uint64_t mergeKey = 0);
		// newParent invalid = root entity; siblingIndex empty (or past the end) = last among the new siblings.
		[[nodiscard]] Result<void> ReparentEntity(UUID entity, UUID newParent, std::optional<size_t> siblingIndex = std::nullopt,
		                                          bool keepWorldTransform = true);
		// Moves entities, in the given order, under newParent (invalid = root) right before its child insertBefore (invalid =
		// after the last child), as one undo step; entities whose ancestor is also moved go along with it.
		[[nodiscard]] Result<void> MoveEntities(std::span<UUID const> entities, UUID newParent, UUID insertBefore = UUID::Invalid(),
		                                        bool keepWorldTransform = true);

		// --- Components ---

		[[nodiscard]] Result<void> AddComponent(UUID entity, std::string_view component, Json const& fields = Json::object());
		// Adds the component to every given entity as one undo step (all or nothing).
		[[nodiscard]] Result<void> AddComponent(std::span<UUID const> entities, std::string_view component,
		                                        Json const& fields = Json::object());
		[[nodiscard]] Result<void> RemoveComponent(UUID entity, std::string_view component);
		// Removes the component from every given entity as one undo step (all or nothing).
		[[nodiscard]] Result<void> RemoveComponent(std::span<UUID const> entities, std::string_view component);
		// Applies a partial JSON patch in the scene-file format. Edits with the same non-zero merge key (for example one
		// inspector drag) merge into one undo step until CommandHistory::BreakMerge is called.
		[[nodiscard]] Result<void> SetComponentFields(UUID entity, std::string_view component, Json const& patch, uint64_t mergeKey = 0);

		// Applies several partial patches as one undo step, all or nothing. Edits with the same non-zero merge key and the
		// same entity/component targets merge (for example a gizmo dragging several entities).
		[[nodiscard]] Result<void> SetComponentFields(std::vector<ComponentEdit> edits, uint64_t mergeKey = 0);

		// Replaces every field of a component with the given value (convenient for inspector widgets editing a copy).
		template<RegisteredComponent T>
		[[nodiscard]] Result<void> SetComponent(UUID entity, T const& component, uint64_t mergeKey = 0)
		{
			return SetComponentFields(entity, ComponentTraits<T>::Name, SerializeComponent(component), mergeKey);
		}

		// --- Assets (files of the open project's asset directory; paths are relative to it with forward slashes) ---
		// File operations are not part of the undo history; material edits are (without marking the scene modified).

		// Fails when the folder exists.
		[[nodiscard]] Result<void> CreateAssetFolder(std::string_view folder);
		// Writes a material file (.smat) with default parameters plus the given fields and registers it.
		[[nodiscard]] Result<AssetHandle> CreateMaterial(std::string_view path, Json const& fields = Json::object());
		// Copies files into a folder of the asset directory (numbering names that are taken) and registers them, all or
		// nothing; files already inside the asset directory are registered in place. Returns their handles in order.
		[[nodiscard]] Result<std::vector<AssetHandle>> ImportAssets(std::span<std::filesystem::path const> files, std::string_view folder);
		// Moves or renames a file asset; its handle (and every reference to it) stays valid.
		[[nodiscard]] Result<void> MoveAsset(AssetHandle asset, std::string_view newPath);
		[[nodiscard]] Result<void> MoveAssetFolder(std::string_view folder, std::string_view newFolder);
		[[nodiscard]] Result<void> DeleteAsset(AssetHandle asset);
		// Deletes a folder with every file inside.
		[[nodiscard]] Result<void> DeleteAssetFolder(std::string_view folder);
		// Rescans the asset directory for new, missing and modified files.
		[[nodiscard]] Result<AssetRefreshResult> RefreshAssets();
		// Applies a partial patch to a material file's parameters as an undoable step; edits with the same non-zero merge key
		// (one inspector drag) merge.
		[[nodiscard]] Result<void> SetMaterialFields(AssetHandle material, Json const& patch, uint64_t mergeKey = 0);

		// --- Selection and history ---

		// Fails without changing the selection when an entity does not exist.
		[[nodiscard]] Result<void> Select(std::span<UUID const> entities, SelectionMode mode = SelectionMode::Replace,
		                                  UUID primary = UUID::Invalid());
		[[nodiscard]] Result<void> Undo() { return m_Context.Undo(); }
		[[nodiscard]] Result<void> Redo() { return m_Context.Redo(); }

	private:
		// Makes the project the open one and opens its start scene; returns warnings about the start scene.
		Result<std::vector<std::string>> UseProject(Ref<Project> project);
		// After a failed create or open: keeps the current project when its asset directory is still open, otherwise closes it.
		void KeepProjectIfOpen();

		EditorContext& m_Context;
	};
}
