#pragma once

#include "Editor/Commands/ComponentCommands.h"
#include "Editor/Commands/EntityCommands.h"
#include "Editor/EditorContext.h"

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

		// --- Scene files (not undoable: they replace the scene and clear the history) ---

		void NewScene(std::string name = "Untitled");
		// Loads a scene file; unknown components and fields (files from newer versions) are skipped with warnings, which
		// are logged and returned.
		[[nodiscard]] Result<std::vector<std::string>> OpenScene(std::filesystem::path const& path);
		// Saves to path, or to the current scene file when path is empty; the scene file becomes that path.
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

		// --- Selection and history ---

		// Fails without changing the selection when an entity does not exist.
		[[nodiscard]] Result<void> Select(std::span<UUID const> entities, SelectionMode mode = SelectionMode::Replace,
		                                  UUID primary = UUID::Invalid());
		[[nodiscard]] Result<void> Undo() { return m_Context.Undo(); }
		[[nodiscard]] Result<void> Redo() { return m_Context.Redo(); }

	private:
		EditorContext& m_Context;
	};
}
