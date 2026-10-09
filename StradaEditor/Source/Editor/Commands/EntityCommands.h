#pragma once

#include "Editor/Commands/EditorCommand.h"
#include "Editor/Commands/EntitySnapshot.h"

#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Strada
{
	struct EntityCreateInfo
	{
		std::string Name = "Entity";
		// Invalid for a root entity.
		UUID Parent = UUID::Invalid();
		// Position among the siblings; empty (or past the end) appends.
		std::optional<size_t> SiblingIndex;
		// Components to add or update, in the scene-file format: { "<Component>": { "<Field>": value } }.
		Json Components = Json::object();
	};

	// Creates an entity with the given UUID (unused in the scene). Redo recreates it from a snapshot of the first
	// execution, so later commands that refer to its UUID keep working.
	class CreateEntityCommand final : public EditorCommand
	{
	public:
		CreateEntityCommand(UUID id, EntityCreateInfo info);

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override { return "Create Entity"; }

	private:
		Result<void> Create(EditorContext& context);

		UUID m_ID;
		EntityCreateInfo m_Info;
		std::optional<EntitySnapshot> m_Snapshot;
	};

	// Deletes entities with their descendants (an entity whose ancestor is also deleted goes with the ancestor). Undo
	// restores the same UUIDs, components, hierarchy and sibling order.
	class DeleteEntitiesCommand final : public EditorCommand
	{
	public:
		explicit DeleteEntitiesCommand(std::vector<UUID> entities);

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override;

	private:
		std::vector<UUID> m_Entities;
		std::vector<EntitySnapshot> m_Snapshots;
	};

	// Copies entities with their descendants; each copy is placed right after its original.
	class DuplicateEntitiesCommand final : public EditorCommand
	{
	public:
		explicit DuplicateEntitiesCommand(std::vector<UUID> entities);

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override;

		// UUIDs of the copies (one per duplicated subtree, in request order); valid after the first execution.
		std::vector<UUID> const& GetCopies() const { return m_Copies; }

	private:
		std::vector<UUID> m_Entities;
		std::vector<UUID> m_Copies;
		std::vector<EntitySnapshot> m_Snapshots;
	};

	// Moves an entity under another parent (or to the root) and/or to another sibling position. Staying under the same
	// parent only reorders: the transform is left untouched.
	class ReparentEntityCommand final : public EditorCommand
	{
	public:
		// newParent invalid = root entity; siblingIndex empty (or past the end) = last.
		ReparentEntityCommand(UUID entity, UUID newParent, std::optional<size_t> siblingIndex, bool keepWorldTransform);

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override;
		bool HasEffect() const override;

	private:
		UUID m_Entity;
		UUID m_NewParent;
		std::optional<size_t> m_SiblingIndex;
		bool m_KeepWorldTransform;

		// Recorded by the first execution: where the entity was and its local transform before and after the move. Redo
		// restores the recorded result instead of recomputing it, so it is bit-exact.
		bool m_Executed = false;
		UUID m_OldParent = UUID::Invalid();
		size_t m_OldSiblingIndex = 0;
		Json m_OldTransform;
		size_t m_NewSiblingIndex = 0;
		Json m_NewTransform;
	};
}
