#include "Editor/EntityPresets.h"

#include "Editor/EditorOperations.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Core/FileSystem.h"

#include <array>
#include <string>

namespace Strada
{
	namespace
	{
		Json MeshComponents(BuiltInAsset mesh)
		{
			return Json::object({{"Mesh", Json::object({{"Mesh", GetBuiltInHandle(mesh).ToString()}})}});
		}

		// std::to_array deduces the count, so every entry is initialized.
		constexpr auto s_Presets = std::to_array<EntityPreset>({
			{"", "Empty Entity", "Entity",
		     []
		     {
				 return Json::object();
			 }},
			{"", "Camera", "Camera",
		     []
		     {
				 return Json::object({{"Camera", Json::object()}});
			 }},
			{"3D Object", "Cube", "Cube",
		     []
		     {
				 return MeshComponents(BuiltInAsset::CubeMesh);
			 }},
			{"3D Object", "Sphere", "Sphere",
		     []
		     {
				 return MeshComponents(BuiltInAsset::SphereMesh);
			 }},
			{"3D Object", "Plane", "Plane",
		     []
		     {
				 return MeshComponents(BuiltInAsset::PlaneMesh);
			 }},
			{"3D Object", "Cylinder", "Cylinder",
		     []
		     {
				 return MeshComponents(BuiltInAsset::CylinderMesh);
			 }},
			{"3D Object", "Capsule", "Capsule",
		     []
		     {
				 return MeshComponents(BuiltInAsset::CapsuleMesh);
			 }},
			{"3D Object", "Cone", "Cone",
		     []
		     {
				 return MeshComponents(BuiltInAsset::ConeMesh);
			 }},
			{"3D Object", "Quad", "Quad",
		     []
		     {
				 return MeshComponents(BuiltInAsset::QuadMesh);
			 }},
			// Directional and spot lights shine along -Z: tilt them towards the ground.
			{"Light", "Directional Light", "Directional Light",
		     []
		     {
				 return Json::object({{"DirectionalLight", Json::object()},
			                          {"Transform", Json::object({{"RotationEuler", Json::array({-50.0, 30.0, 0.0})}})}});
			 }},
			{"Light", "Point Light", "Point Light",
		     []
		     {
				 return Json::object({{"PointLight", Json::object()}});
			 }},
			{"Light", "Spot Light", "Spot Light",
		     []
		     {
				 return Json::object(
					 {{"SpotLight", Json::object()}, {"Transform", Json::object({{"RotationEuler", Json::array({-90.0, 0.0, 0.0})}})}});
			 }},
			{"Light", "Sky Light", "Sky Light",
		     []
		     {
				 return Json::object(
					 {{"SkyLight", Json::object({{"Environment", GetBuiltInHandle(BuiltInAsset::DefaultSky).ToString()}})}});
			 }},
			{"Audio", "Audio Source", "Audio Source",
		     []
		     {
				 return Json::object({{"AudioSource", Json::object()}});
			 }},
			{"2D", "Text", "Text",
		     []
		     {
				 return Json::object({{"Text", Json::object({{"Text", "Text"}})}});
			 }},
			{"2D", "Sprite", "Sprite",
		     []
		     {
				 return Json::object({{"SpriteRenderer", Json::object()}});
			 }},
		});
	}

	namespace EntityPresets
	{
		std::span<EntityPreset const> GetAll()
		{
			return s_Presets;
		}

		namespace
		{
			Result<UUID> CreatePlaced(EditorOperations& operations, EntityCreateInfo info, glm::vec3 const& position)
			{
				if (!info.Parent.IsValid())
				{
					Json& transform = info.Components["Transform"];
					if (!transform.is_object())
					{
						transform = Json::object();
					}
					transform["Translation"] = Json::array({position.x, position.y, position.z});
				}
				return operations.CreateEntity(std::move(info));
			}
		}

		Result<UUID> CreateFromMesh(EditorOperations& operations, AssetHandle mesh, UUID parent, glm::vec3 const& position)
		{
			std::optional<AssetMetadata> const metadata = AssetManager::IsInitialized() ? AssetManager::GetMetadata(mesh) : std::nullopt;
			if (!metadata || metadata->Type != AssetType::Mesh)
			{
				return MakeError("asset {} is not a mesh", mesh);
			}
			EntityCreateInfo info;
			info.Name = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(metadata->GetDisplayName()).stem());
			info.Parent = parent;
			info.Components = Json::object({{"Mesh", Json::object({{"Mesh", mesh.ToString()}})}});
			return CreatePlaced(operations, std::move(info), position);
		}

		Result<UUID> Create(EditorOperations& operations, EntityPreset const& preset, UUID parent, glm::vec3 const& position)
		{
			EntityCreateInfo info;
			info.Name = std::string(preset.EntityName);
			info.Parent = parent;
			info.Components = preset.BuildComponents();
			return CreatePlaced(operations, std::move(info), position);
		}
	}
}
