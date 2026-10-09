#pragma once

#include "Editor/Commands/CommandHistory.h"
#include "Editor/EntitySelection.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string_view>
#include <utility>

namespace Strada
{
	// The editor's document state, shared by the UI panels and the automation commands: the open project, the edited
	// scene and its file, the undo history (which also tracks unsaved changes) and the entity selection. Main thread only.
	//
	// Every modification of the scene goes through ExecuteCommand so it can be undone; EditorOperations builds the
	// commands. Play mode (a later stage) will keep a runtime copy of the scene next to this one; the history and the
	// selection always belong to the edited scene returned by GetScene.
	class EditorContext
	{
	public:
		using AssetReferenceResolver = std::function<Result<AssetHandle>(std::string_view reference)>;

		// Starts with an empty, unsaved scene named "Untitled".
		EditorContext();

		EditorContext(EditorContext const&) = delete;
		EditorContext& operator=(EditorContext const&) = delete;

		Scene& GetScene() { return *m_Scene; }
		Scene const& GetScene() const { return *m_Scene; }
		// Empty for scenes that were never saved.
		std::filesystem::path const& GetScenePath() const { return m_ScenePath; }
		// Replaces the edited scene (new scene or opened file). Clears the history and the selection; the scene counts as
		// saved.
		void SetScene(Ref<Scene> scene, std::filesystem::path path);
		// Records that the scene was written to path: it becomes the scene path and the current state the saved one.
		void MarkSaved(std::filesystem::path path);
		bool IsDirty() const { return m_History.IsDirty(); }
		// Changes whenever SetScene replaces the scene (state tied to one scene, such as viewport picking IDs, resets).
		uint64_t GetSceneVersion() const { return m_SceneVersion; }

		// The open project; null when none is open. Its asset directory is the AssetManager's.
		Project* GetProject() { return m_Project.get(); }
		Project const* GetProject() const { return m_Project.get(); }
		void SetProject(Ref<Project> project) { m_Project = std::move(project); }

		// --- Undoable modifications ---

		// Executes the command through the history (see CommandHistory::Execute), then drops deleted entities from the
		// selection.
		[[nodiscard]] Result<void> ExecuteCommand(Scope<EditorCommand> command);
		[[nodiscard]] Result<void> Undo();
		[[nodiscard]] Result<void> Redo();
		CommandHistory& GetHistory() { return m_History; }
		CommandHistory const& GetHistory() const { return m_History; }

		// --- Selection (not part of the undo history) ---

		EntitySelection const& GetSelection() const { return m_Selection; }
		// Entities that do not exist in the scene are ignored. A valid primary that ends up selected becomes the primary
		// selection.
		void Select(std::span<UUID const> entities, SelectionMode mode = SelectionMode::Replace, UUID primary = UUID::Invalid());
		void ClearSelection() { m_Selection.Clear(); }

		// --- Reading user input and files ---

		// Resolves "asset://<path>" and "builtin://<name>" references in component data. Unset until the asset manager
		// provides one; such references are then reported as errors.
		void SetAssetReferenceResolver(AssetReferenceResolver resolver) { m_AssetReferenceResolver = std::move(resolver); }
		DeserializationContext CreateDeserializationContext(UnknownFieldPolicy unknownFields = UnknownFieldPolicy::Error) const;

	private:
		void PruneSelection();

		Ref<Project> m_Project;
		Ref<Scene> m_Scene;
		std::filesystem::path m_ScenePath;
		CommandHistory m_History;
		EntitySelection m_Selection;
		AssetReferenceResolver m_AssetReferenceResolver;
		uint64_t m_SceneVersion = 0;
	};
}
