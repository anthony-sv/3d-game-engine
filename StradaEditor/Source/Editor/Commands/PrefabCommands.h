#pragma once

#include "Editor/Commands/EditorCommand.h"
#include "Editor/Commands/EntitySnapshot.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>

namespace Strada
{
	// Creates an instance of a prefab asset with fresh UUIDs (PrefabSerializer::Instantiate) under a parent, or as a root
	// entity. With a position the root moves there in world space, keeping its rotation and scale. Redo recreates the same
	// entities from a snapshot of the first execution, so later commands that refer to them keep working.
	class InstantiatePrefabCommand final : public EditorCommand
	{
	public:
		InstantiatePrefabCommand(AssetHandle prefab, UUID parent, std::optional<glm::vec3> position);

		Result<void> Execute(EditorContext& context) override;
		Result<void> Undo(EditorContext& context) override;
		std::string GetDescription() const override { return "Instantiate Prefab"; }

		// The instance's root; valid after the first execution.
		UUID GetRoot() const { return m_Root; }

	private:
		AssetHandle m_Prefab;
		UUID m_Parent;
		std::optional<glm::vec3> m_Position;
		UUID m_Root = UUID::Invalid();
		std::optional<EntitySnapshot> m_Snapshot;
	};
}
