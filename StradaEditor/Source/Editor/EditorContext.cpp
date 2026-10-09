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
		m_Scene = std::move(scene);
		m_ScenePath = std::move(path);
		m_History.Clear();
		m_Selection.Clear();
	}

	void EditorContext::MarkSaved(std::filesystem::path path)
	{
		m_ScenePath = std::move(path);
		m_History.MarkSaved();
	}

	Result<void> EditorContext::ExecuteCommand(Scope<EditorCommand> command)
	{
		Result<void> result = m_History.Execute(std::move(command), *this);
		PruneSelection();
		return result;
	}

	Result<void> EditorContext::Undo()
	{
		Result<void> result = m_History.Undo(*this);
		PruneSelection();
		return result;
	}

	Result<void> EditorContext::Redo()
	{
		Result<void> result = m_History.Redo(*this);
		PruneSelection();
		return result;
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
