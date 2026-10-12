#include "Editor/MaterialSlots.h"

#include "Editor/AssetBrowsing.h"
#include "Editor/EditorOperations.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	namespace MaterialSlots
	{
		namespace
		{
			// An entity's Mesh component and its loaded mesh; null pointers without a component or a loadable mesh.
			std::pair<MeshComponent const*, Ref<MeshSource>> FindMesh(Scene& scene, UUID entity)
			{
				Entity const target = scene.HasEntity(entity) ? scene.GetEntityByUUID(entity) : Entity();
				if (!target || !target.HasComponent<MeshComponent>() || !AssetManager::IsInitialized())
				{
					return {nullptr, nullptr};
				}
				MeshComponent const& component = target.GetComponent<MeshComponent>();
				return {&component, AssetManager::GetAsset<MeshSource>(component.Mesh)};
			}

			size_t CountSlots(Scene& scene, UUID entity)
			{
				Ref<MeshSource> const mesh = FindMesh(scene, entity).second;
				return mesh != nullptr ? mesh->GetMaterials().size() : 0;
			}

			// The part of a sub-asset's name after its last '/' ("Helmet.glb/Materials/Visor" -> "Visor").
			std::string_view GetLastSegment(std::string_view name)
			{
				size_t const separator = name.find_last_of('/');
				return separator == std::string_view::npos ? name : name.substr(separator + 1);
			}
		}

		AssetHandle GetMaterial(Scene& scene, UUID entity, size_t slot)
		{
			auto const [component, mesh] = FindMesh(scene, entity);
			if (mesh == nullptr || slot >= mesh->GetMaterials().size())
			{
				return AssetHandle();
			}
			if (slot < component->Materials.size() && component->Materials[slot].IsValid())
			{
				return component->Materials[slot];
			}
			AssetHandle const own = mesh->GetMaterials()[slot];
			return own.IsValid() ? own : GetBuiltInHandle(BuiltInAsset::DefaultMaterial);
		}

		std::vector<AssetHandle> GetMaterials(Scene& scene, std::span<UUID const> entities)
		{
			std::vector<AssetHandle> materials;
			if (entities.empty())
			{
				return materials;
			}
			size_t const slots = CountSlots(scene, entities.front());
			materials.reserve(slots);
			for (size_t slot = 0; slot < slots; slot++)
			{
				AssetHandle const material = GetMaterial(scene, entities.front(), slot);
				bool shared = true;
				for (size_t i = 1; i < entities.size() && shared; i++)
				{
					shared = GetMaterial(scene, entities[i], slot) == material;
				}
				materials.push_back(shared ? material : AssetHandle());
			}
			return materials;
		}

		bool IsEditable(AssetHandle material)
		{
			if (!AssetManager::IsInitialized())
			{
				return false;
			}
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(material);
			return metadata && metadata->Type == AssetType::Material && metadata->IsFileAsset();
		}

		std::string GetMaterialName(AssetHandle material)
		{
			std::optional<AssetMetadata> const metadata =
				AssetManager::IsInitialized() ? AssetManager::GetMetadata(material) : std::nullopt;
			if (!metadata)
			{
				return {};
			}
			if (metadata->IsFileAsset())
			{
				return FileSystem::PathToUtf8(FileSystem::PathFromUtf8(metadata->Path).stem());
			}
			return std::string(GetLastSegment(metadata->GetDisplayName()));
		}

		Result<void> Assign(EditorOperations& operations, std::span<UUID const> entities, size_t slot, AssetHandle material)
		{
			if (entities.empty())
			{
				return Error{"no entities to assign the material to"};
			}
			if (!AssetManager::IsInitialized() || AssetManager::GetAssetType(material) != AssetType::Material)
			{
				return MakeError("asset {} is not a material", material);
			}
			Scene& scene = operations.GetContext().GetScene();
			std::vector<ComponentEdit> edits;
			edits.reserve(entities.size());
			for (UUID const entity : entities)
			{
				if (slot >= CountSlots(scene, entity))
				{
					return MakeError("entity {} has no mesh with a material slot {}", entity, slot);
				}
				std::vector<AssetHandle> materials = scene.GetEntityByUUID(entity).GetComponent<MeshComponent>().Materials;
				if (materials.size() <= slot)
				{
					materials.resize(slot + 1);
				}
				materials[slot] = material;
				Json overrides = Json::array();
				for (AssetHandle const handle : materials)
				{
					overrides.push_back(handle.ToString());
				}
				edits.push_back({entity, "Mesh", Json::object({{"Materials", std::move(overrides)}})});
			}
			return operations.SetComponentFields(std::move(edits));
		}

		Result<AssetHandle> CreateCopy(EditorOperations& operations, AssetHandle material, std::string_view name)
		{
			if (!AssetManager::IsInitialized())
			{
				return Error{"no assets are available"};
			}
			Result<Ref<MaterialAsset>> source = AssetManager::TryGetAsset<MaterialAsset>(material);
			if (!source)
			{
				return Error{source.GetError()};
			}
			std::string fileName = FileSystem::MakePortableFileName(name);
			if (fileName.empty())
			{
				fileName = "Material";
			}
			std::string const path =
				AssetBrowsing::JoinPath(CopyFolder, AssetBrowsing::MakeUniqueName(CopyFolder, fileName, std::string_view(".smat")));
			// A material file only holds parameters inside the material ranges; imports bring theirs into them, other
			// materials may still lie outside (made at run time). Emitted light keeps its brightness.
			MaterialData data = source.GetValue()->GetData();
			if (std::vector<std::string_view> const adjusted = FitMaterialToFieldRanges(data); !adjusted.empty())
			{
				std::string fields;
				for (std::string_view const field : adjusted)
				{
					fields += fields.empty() ? "" : ", ";
					fields += field;
				}
				ST_WARN("The copy of material '{}' has {} adjusted to the ranges materials accept", name, fields);
			}
			return operations.CreateMaterial(path, SerializeFields<StructTraits<MaterialData>>(data));
		}

		Result<AssetHandle> CreateEditableCopy(EditorOperations& operations, std::span<UUID const> entities, size_t slot)
		{
			Scene& scene = operations.GetContext().GetScene();
			std::vector<AssetHandle> const materials = GetMaterials(scene, entities);
			if (slot >= materials.size())
			{
				return MakeError("the entities have no mesh with a material slot {}", slot);
			}
			AssetHandle const material = materials[slot];
			if (!material.IsValid())
			{
				return MakeError("the entities render material slot {} with different materials", slot);
			}

			// Copies are named after the material; copies of built-in materials (such as the engine's default one, which many
			// entities use) after the entity they are made for.
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(material);
			std::string const name =
				metadata && metadata->IsBuiltIn() ? scene.GetEntityByUUID(entities.front()).GetName() : GetMaterialName(material);
			Result<AssetHandle> copy = CreateCopy(operations, material, name);
			if (!copy)
			{
				return copy;
			}
			if (Result<void> assigned = Assign(operations, entities, slot, copy.GetValue()); !assigned)
			{
				(void)operations.DeleteAsset(copy.GetValue());
				return Error{assigned.GetError()};
			}
			return copy;
		}
	}
}
