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
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Strada
{
	struct VisualScriptDocument;

	// What the editor does with the scene.
	enum class EditorPlayState : uint8_t
	{
		Edit = 0,
		// A copy of the scene runs with scripts, physics and audio, seen through the game's camera.
		Play,
		// A copy of the scene runs physics only, seen through the editor camera.
		Simulate
	};

	// The editor's document state, shared by the UI panels and the automation commands: the open project, the edited
	// scene and its file, the undo history (which also tracks unsaved changes) and the entity selection. Main thread only.
	//
	// Every modification of the scene goes through ExecuteCommand so it can be undone; EditorOperations builds the
	// commands. During play mode GetScene returns the running copy of the scene, which panels and automation show and
	// edit like the scene itself, with a history of its own; stopping discards the copy and every change made to it, and
	// the edited scene comes back with its history. Steps that edit asset files (visual scripts, materials) leave the scene
	// alone and their files keep the change, so they stay undoable when the history they are in goes: playing stopping,
	// the running scene being replaced, another scene of the project being opened (CommandHistory::TakeAssetSteps).
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
		// Replaces the edited scene (new scene or opened file); the scene counts as saved and the selection is cleared. The
		// history keeps only its steps of asset files, which stay undoable, or none when keepAssetSteps is false (another
		// project is opening: the steps name the assets of the one before). Not while playing.
		void SetScene(Ref<Scene> scene, std::filesystem::path path, bool keepAssetSteps = true);
		// Records that the scene was written to path: it becomes the scene path and the current state the saved one.
		void MarkSaved(std::filesystem::path path);
		// The edited scene has unsaved changes (changes to the running copy during play mode do not count).
		bool IsDirty() const { return IsPlaying() ? m_EditedHistory.IsDirty() : m_History.IsDirty(); }
		// Changes whenever the scene GetScene returns is replaced (SetScene, play mode starting and stopping, the running
		// scene loading another), so state tied to one scene, such as viewport picking IDs, resets.
		uint64_t GetSceneVersion() const { return m_SceneVersion; }
		// Changes whenever SetScene replaces the edited scene with a new or opened one (not when play mode starts or stops),
		// so views show it from a fitting place.
		uint64_t GetOpenedSceneVersion() const { return m_OpenedSceneVersion; }

		// --- Play mode (driven by PlayMode) ---

		EditorPlayState GetPlayState() const { return m_PlayState; }
		bool IsPlaying() const { return m_PlayState != EditorPlayState::Edit; }
		// Makes the running copy the scene GetScene returns, with an empty history; the selection carries over (the copy
		// has the same entity IDs).
		void BeginPlay(Ref<Scene> runningScene, EditorPlayState state);
		// The running scene was replaced (a script loaded another scene): its history starts over with the steps of asset
		// files it had, and the selection keeps the entities it has.
		void SetRunningScene(Ref<Scene> runningScene);
		// Returns to the edited scene and its history, which takes the steps of asset files made while playing as its newest
		// undo steps (steps undone while playing are not taken): they stay undoable, and the edited scene's unsaved changes
		// are the ones it had. The selection keeps the entities the edited scene has.
		void EndPlay();

		// The open project; null when none is open. Its asset directory is the AssetManager's.
		Project* GetProject() { return m_Project.get(); }
		Project const* GetProject() const { return m_Project.get(); }
		void SetProject(Ref<Project> project);

		// --- Undoable modifications ---

		// Executes the command through the history (see CommandHistory::Execute), then drops deleted entities from the
		// selection. Edits run between frames: entities they destroy in a running scene go at once, as when editing (the
		// scene would wait for the end of the next frame, which never comes while it is paused), so later edits, undo
		// included, find them gone, and the script fields and visual script variables they change reach the running scripts
		// at once (Scene::ApplyScriptFieldChanges, Scene::ApplyVisualScriptVariableChanges). The same holds for Undo and Redo.
		[[nodiscard]] Result<void> ExecuteCommand(Scope<EditorCommand> command);
		[[nodiscard]] Result<void> Undo();
		[[nodiscard]] Result<void> Redo();
		CommandHistory& GetHistory() { return m_History; }
		CommandHistory const& GetHistory() const { return m_History; }
		// The asset was deleted: its steps leave the history and the edited scene's history put aside while playing, but for
		// steps that also changed the scene, which stay and forget it (CommandHistory::DiscardStepsOf). Undo then goes on
		// with the steps before them instead of failing at a step whose file is gone.
		void DiscardAssetSteps(AssetHandle asset);

		// --- Selection (not part of the undo history) ---

		EntitySelection const& GetSelection() const { return m_Selection; }
		// Entities that do not exist in the scene are ignored. A valid primary that ends up selected becomes the primary
		// selection.
		void Select(std::span<UUID const> entities, SelectionMode mode = SelectionMode::Replace, UUID primary = UUID::Invalid());
		// Clears the entity and the asset selection.
		void ClearSelection();

		// The asset the inspector shows (selected in the content browser); invalid when none. The inspector shows what was
		// selected last: selecting an asset clears the entity selection, and selecting entities (or replacing the selection
		// with nothing) clears the asset.
		AssetHandle GetSelectedAsset() const { return m_SelectedAsset; }
		void SelectAsset(AssetHandle asset);

		// --- Reading user input and files ---

		// Resolves "asset://<path>" and "builtin://<name>" references in component data. Unset until the asset manager
		// provides one; such references are then reported as errors.
		void SetAssetReferenceResolver(AssetReferenceResolver resolver) { m_AssetReferenceResolver = std::move(resolver); }
		DeserializationContext CreateDeserializationContext(UnknownFieldPolicy unknownFields = UnknownFieldPolicy::Error) const;

		// --- Files the editor writes ---

		// The canonical text the editor last wrote to a visual script's file, as the text of the document object the asset
		// took with it. Documents never change, so while the asset holds that very object, a file with these bytes holds its
		// canonical text: edits compare the file with it instead of serializing the document again (SetVisualScriptCommand).
		void RememberVisualScriptText(AssetHandle graph, Ref<VisualScriptDocument const> const& document, std::string text);
		void ForgetVisualScriptText(AssetHandle graph);
		// The text remembered for the graph when it is the text of this document object; null otherwise.
		std::string const* FindVisualScriptText(AssetHandle graph, VisualScriptDocument const* document) const;

	private:
		struct VisualScriptText
		{
			std::weak_ptr<VisualScriptDocument const> Document;
			std::string Text;
		};

		void FinishEdit();
		void PruneSelection();

		Ref<Project> m_Project;
		Ref<Scene> m_Scene;
		std::filesystem::path m_ScenePath;
		CommandHistory m_History;
		EditorPlayState m_PlayState = EditorPlayState::Edit;
		// While playing: the edited scene and its history, put aside.
		Ref<Scene> m_EditedScene;
		CommandHistory m_EditedHistory;
		EntitySelection m_Selection;
		AssetHandle m_SelectedAsset;
		AssetReferenceResolver m_AssetReferenceResolver;
		std::unordered_map<AssetHandle, VisualScriptText> m_VisualScriptTexts;
		uint64_t m_SceneVersion = 0;
		uint64_t m_OpenedSceneVersion = 0;
	};
}
