#include "Editor/AssetDrops.h"

#include "Editor/EditorOperations.h"
#include "Editor/ScriptBrowsing.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"

#include <algorithm>

namespace Strada
{
	namespace AssetDrops
	{
		namespace
		{
			Result<void> RequireAssetType(AssetHandle asset, AssetType type)
			{
				if (!AssetManager::IsInitialized() || AssetManager::GetAssetType(asset) != type)
				{
					return MakeError("asset {} is not a {}", asset, AssetTypeToString(type));
				}
				return {};
			}
		}

		Result<void> AssignMaterial(EditorOperations& operations, UUID entity, AssetHandle material)
		{
			if (Result<void> valid = RequireAssetType(material, AssetType::Material); !valid)
			{
				return valid;
			}
			Scene& scene = operations.GetContext().GetScene();
			Entity const target = scene.HasEntity(entity) ? scene.GetEntityByUUID(entity) : Entity();
			if (!target || !target.HasComponent<MeshComponent>())
			{
				return MakeError("entity {} has no Mesh component", entity);
			}

			// One override per submesh; a mesh that cannot be loaded keeps as many overrides as it has.
			MeshComponent const& mesh = target.GetComponent<MeshComponent>();
			size_t slots = std::max<size_t>(mesh.Materials.size(), 1);
			if (Ref<MeshSource> const source = AssetManager::GetAsset<MeshSource>(mesh.Mesh))
			{
				slots = std::max<size_t>(source->GetSubmeshes().size(), 1);
			}
			Json const materials(slots, Json(material.ToString()));
			return operations.SetComponentFields(entity, "Mesh", Json::object({{"Materials", materials}}));
		}

		Result<UUID> SetSkyEnvironment(EditorOperations& operations, AssetHandle environment)
		{
			if (Result<void> valid = RequireAssetType(environment, AssetType::Environment); !valid)
			{
				return Error{valid.GetError()};
			}
			Json const patch = Json::object({{"Environment", environment.ToString()}});

			UUID skyLight = UUID::Invalid();
			operations.GetContext().GetScene().ForEachEntityInHierarchyOrder(
				[&skyLight](Entity entity)
				{
					if (!skyLight.IsValid() && entity.HasComponent<SkyLightComponent>())
					{
						skyLight = entity.GetUUID();
					}
				});
			if (skyLight.IsValid())
			{
				if (Result<void> changed = operations.SetComponentFields(skyLight, "SkyLight", patch); !changed)
				{
					return Error{changed.GetError()};
				}
				return skyLight;
			}

			EntityCreateInfo info;
			info.Name = "Sky Light";
			info.Components = Json::object({{"SkyLight", patch}});
			return operations.CreateEntity(std::move(info));
		}

		Result<std::string> AddScript(EditorOperations& operations, std::span<UUID const> entities, std::filesystem::path const& file)
		{
			if (entities.empty())
			{
				return Error{"there is no entity to add the script to"};
			}
			Scene& scene = operations.GetContext().GetScene();
			for (UUID const entity : entities)
			{
				if (!scene.HasEntity(entity))
				{
					return MakeError("entity {} does not exist", entity);
				}
			}
			if (!ScriptEngine::IsInitialized())
			{
				return Error{"scripting is unavailable: the .NET 10 runtime was not found"};
			}
			if (!ScriptEngine::HasGameAssembly())
			{
				return MakeError("the scripts are not built yet: build them (Scripts > Build Scripts) and add '{}' again",
				                 FileSystem::PathToUtf8(file.filename()));
			}
			Result<std::string> className = ScriptBrowsing::FindScriptClass(file, ScriptEngine::GetClasses());
			if (!className)
			{
				return className;
			}

			if (Result<void> result = operations.SetScriptClass(entities, className.GetValue()); !result)
			{
				return Error{result.GetError()};
			}
			return className;
		}
	}
}
