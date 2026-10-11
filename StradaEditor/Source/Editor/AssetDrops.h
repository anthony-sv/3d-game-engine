#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"

#include <filesystem>
#include <span>
#include <string>

namespace Strada
{
	class EditorOperations;

	// What dropping an asset or a C# source onto the scene does (viewport, hierarchy and inspector), as undoable operations.
	// Mesh assets create entities through EntityPresets::CreateFromMesh.
	namespace AssetDrops
	{
		// Makes every submesh of the entity's mesh use the material (one override per submesh), as one undo step. Fails when
		// the entity has no Mesh component or the asset is not a material.
		[[nodiscard]] Result<void> AssignMaterial(EditorOperations& operations, UUID entity, AssetHandle material);
		// Shows the environment through the scene's sky light: the first one in hierarchy order (the one the renderer uses),
		// or a new "Sky Light" root entity when the scene has none. One undo step; returns the sky light entity.
		[[nodiscard]] Result<UUID> SetSkyEnvironment(EditorOperations& operations, AssetHandle environment);
		// Makes the entities run the script class a C# source defines among the loaded classes (ScriptBrowsing::FindScriptClass)
		// as one undo step (EditorOperations::SetScriptClass): entities without a Script component get one, the class of the
		// others is replaced. Fails without changes when scripting is unavailable, the source defines no loaded class (its
		// scripts are not built yet, which the error says) or an entity does not exist. Returns the class.
		[[nodiscard]] Result<std::string> AddScript(EditorOperations& operations, std::span<UUID const> entities,
		                                            std::filesystem::path const& file);
	}
}
