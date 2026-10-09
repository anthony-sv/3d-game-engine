#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"

namespace Strada
{
	class EditorOperations;

	// What dropping an asset onto the scene does (viewport and hierarchy), as undoable operations. Mesh assets create
	// entities through EntityPresets::CreateFromMesh.
	namespace AssetDrops
	{
		// Makes every submesh of the entity's mesh use the material (one override per submesh), as one undo step. Fails when
		// the entity has no Mesh component or the asset is not a material.
		[[nodiscard]] Result<void> AssignMaterial(EditorOperations& operations, UUID entity, AssetHandle material);
		// Shows the environment through the scene's sky light: the first one in hierarchy order (the one the renderer uses),
		// or a new "Sky Light" root entity when the scene has none. One undo step; returns the sky light entity.
		[[nodiscard]] Result<UUID> SetSkyEnvironment(EditorOperations& operations, AssetHandle environment);
	}
}
