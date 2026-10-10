#include "Editor/EditorContext.h"

#include "Strada/Core/Assert.h"

#include <utility>
#include <vector>

namespace Strada
{
	EditorContext::EditorContext()
		: m_Scene(CreateRef<Scene>("Untitled"))
	{
	}

	void EditorContext::SetScene(Ref<Scene> scene, std::filesystem::path path)
	{
		ST_CORE_ASSERT(scene != nullptr, "The editor needs a scene");
		ST_CORE_ASSERT(!IsPlaying(), "The edited scene cannot be replaced while playing");
		m_Scene = std::move(scene);
		m_ScenePath = std::move(path);
		m_History.Clear();
		m_Selection.Clear();
		m_SceneVersion++;
	}

	void EditorContext::BeginPlay(Ref<Scene> runningScene, EditorPlayState state)
	{
		ST_CORE_ASSERT(runningScene != nullptr && state != EditorPlayState::Edit && !IsPlaying(), "BeginPlay needs a running scene");
		m_EditedScene = std::exchange(m_Scene, std::move(runningScene));
		m_EditedHistory = std::exchange(m_History, CommandHistory());
		m_PlayState = state;
		m_SceneVersion++;
		PruneSelection();
	}

	void EditorContext::SetRunningScene(Ref<Scene> runningScene)
	{
		ST_CORE_ASSERT(runningScene != nullptr && IsPlaying(), "SetRunningScene is for play mode");
		m_Scene = std::move(runningScene);
		m_History.Clear();
		m_SceneVersion++;
		PruneSelection();
	}

	void EditorContext::EndPlay()
	{
		if (!IsPlaying())
		{
			return;
		}
		m_Scene = std::move(m_EditedScene);
		m_History = std::move(m_EditedHistory);
		m_EditedHistory = CommandHistory();
		m_PlayState = EditorPlayState::Edit;
		m_SceneVersion++;
		PruneSelection();
	}

	void EditorContext::MarkSaved(std::filesystem::path path)
	{
		m_ScenePath = std::move(path);
		m_History.MarkSaved();
	}

	Result<void> EditorContext::ExecuteCommand(Scope<EditorCommand> command)
	{
		Result<void> result = m_History.Execute(std::move(command), *this);
		FinishEdit();
		return result;
	}

	Result<void> EditorContext::Undo()
	{
		Result<void> result = m_History.Undo(*this);
		FinishEdit();
		return result;
	}

	Result<void> EditorContext::Redo()
	{
		Result<void> result = m_History.Redo(*this);
		FinishEdit();
		return result;
	}

	void EditorContext::FinishEdit()
	{
		m_Scene->FlushPendingDestruction();
		PruneSelection();
	}

	void EditorContext::Select(std::span<UUID const> entities, SelectionMode mode, UUID primary)
	{
		std::vector<UUID> existing;
		existing.reserve(entities.size());
		for (UUID const entity : entities)
		{
			if (m_Scene->HasEntity(entity))
			{
				existing.push_back(entity);
			}
		}
		m_Selection.Apply(existing, mode);
		if (primary.IsValid())
		{
			m_Selection.SetPrimary(primary);
		}
		// Replacing the selection (even with nothing) or ending up with selected entities stops showing the asset.
		if (mode == SelectionMode::Replace || !m_Selection.IsEmpty())
		{
			m_SelectedAsset = AssetHandle();
		}
	}

	void EditorContext::ClearSelection()
	{
		m_Selection.Clear();
		m_SelectedAsset = AssetHandle();
	}

	void EditorContext::SelectAsset(AssetHandle asset)
	{
		m_SelectedAsset = asset;
		if (asset.IsValid())
		{
			m_Selection.Clear();
		}
	}

	DeserializationContext EditorContext::CreateDeserializationContext(UnknownFieldPolicy unknownFields) const
	{
		DeserializationContext context;
		context.UnknownFields = unknownFields;
		context.ResolveAssetReference = m_AssetReferenceResolver;
		return context;
	}

	void EditorContext::PruneSelection()
	{
		m_Selection.Retain(
			[this](UUID entity)
			{
				return m_Scene->HasEntity(entity);
			});
	}
}
