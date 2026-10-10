#include "stpch.h"
#include "Strada/Script/ScriptGlue.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MaterialAsset.h"

#include <array>
#include <cstddef>
#include <string>

namespace Strada::ScriptGlue
{
	namespace
	{
		// Strada.Interop.NativeMaterialValues: MaterialData in a layout both languages declare alike (no implicit padding).
		struct MaterialValues
		{
			Vector4 BaseColor;
			Vector3 EmissiveColor;
			float Metallic;
			float Roughness;
			float EmissiveIntensity;
			float NormalStrength;
			float OcclusionStrength;
			Vector2 UVTiling;
			Vector2 UVOffset;
			uint64_t BaseColorTexture;
			uint64_t NormalTexture;
			uint64_t MetallicRoughnessTexture;
			uint64_t OcclusionTexture;
			uint64_t EmissiveTexture;
			int32_t AlphaMode;
			float AlphaCutoff;
			uint32_t DoubleSided;
			uint32_t Reserved;
		};

		static_assert(sizeof(MaterialValues) == 120 && offsetof(MaterialValues, BaseColorTexture) == 64 &&
		              offsetof(MaterialValues, AlphaMode) == 104);

		uint64_t ToScript(AssetHandle handle)
		{
			return handle.GetUUID().GetValue();
		}

		AssetHandle ToHandle(uint64_t value)
		{
			return AssetHandle(UUID(value));
		}

		MaterialValues ToScript(MaterialData const& data)
		{
			return {ScriptGlue::ToScript(data.BaseColor),
			        ScriptGlue::ToScript(data.EmissiveColor),
			        data.Metallic,
			        data.Roughness,
			        data.EmissiveIntensity,
			        data.NormalStrength,
			        data.OcclusionStrength,
			        ScriptGlue::ToScript(data.UVTiling),
			        ScriptGlue::ToScript(data.UVOffset),
			        ToScript(data.BaseColorTexture),
			        ToScript(data.NormalTexture),
			        ToScript(data.MetallicRoughnessTexture),
			        ToScript(data.OcclusionTexture),
			        ToScript(data.EmissiveTexture),
			        static_cast<int32_t>(data.AlphaMode),
			        data.AlphaCutoff,
			        data.DoubleSided ? 1u : 0u,
			        0u};
		}

		// The material parameters scripts asked for; false (logged, naming the property set) when a value is invalid.
		bool FromScript(MaterialValues const& values, std::string_view property, MaterialData& data)
		{
			if (!IsFinite(values.BaseColor) || !IsFinite(values.EmissiveColor) || !IsFinite(values.Metallic) ||
			    !IsFinite(values.Roughness) || !IsFinite(values.EmissiveIntensity) || !IsFinite(values.NormalStrength) ||
			    !IsFinite(values.OcclusionStrength) || !IsFinite(values.UVTiling) || !IsFinite(values.UVOffset) ||
			    !IsFinite(values.AlphaCutoff))
			{
				Log::GetScriptLogger().error("Material.{}: the value is not a finite number", property);
				return false;
			}
			if (!FieldValue<MaterialAlphaMode>::IsValid(values.AlphaMode))
			{
				Log::GetScriptLogger().error("Material.{}: {} is not a MaterialAlphaMode", property, values.AlphaMode);
				return false;
			}
			std::array<uint64_t, 5> const textures = {values.BaseColorTexture, values.NormalTexture, values.MetallicRoughnessTexture,
			                                          values.OcclusionTexture, values.EmissiveTexture};
			for (uint64_t const texture : textures)
			{
				if (texture != 0 && AssetManager::GetAssetType(ToHandle(texture)) != AssetType::Texture)
				{
					Log::GetScriptLogger().error("Material.{}: asset {} is not a texture", property, texture);
					return false;
				}
			}

			data.BaseColor = ScriptGlue::FromScript(values.BaseColor);
			data.EmissiveColor = ScriptGlue::FromScript(values.EmissiveColor);
			data.Metallic = values.Metallic;
			data.Roughness = values.Roughness;
			data.EmissiveIntensity = values.EmissiveIntensity;
			data.NormalStrength = values.NormalStrength;
			data.OcclusionStrength = values.OcclusionStrength;
			data.UVTiling = ScriptGlue::FromScript(values.UVTiling);
			data.UVOffset = ScriptGlue::FromScript(values.UVOffset);
			data.BaseColorTexture = ToHandle(values.BaseColorTexture);
			data.NormalTexture = ToHandle(values.NormalTexture);
			data.MetallicRoughnessTexture = ToHandle(values.MetallicRoughnessTexture);
			data.OcclusionTexture = ToHandle(values.OcclusionTexture);
			data.EmissiveTexture = ToHandle(values.EmissiveTexture);
			data.AlphaMode = static_cast<MaterialAlphaMode>(values.AlphaMode);
			data.AlphaCutoff = values.AlphaCutoff;
			data.DoubleSided = values.DoubleSided != 0;
			return true;
		}

		// Asset types with a C# class (scenes are loaded through SceneManager).
		bool IsLoadableType(int32_t type)
		{
			switch (static_cast<AssetType>(type))
			{
				case AssetType::Prefab:
				case AssetType::Mesh:
				case AssetType::Material:
				case AssetType::Texture:
				case AssetType::Environment:
				case AssetType::AudioClip:
				case AssetType::Font:
					return true;
				case AssetType::None:
				case AssetType::Scene:
					break;
			}
			return false;
		}

		uint64_t Assets_Load(char const* path, int32_t length, int32_t type)
		{
			if (!IsLoadableType(type))
			{
				Log::GetScriptLogger().error("Assets.Load: {} is not a loadable asset type", type);
				return 0;
			}
			std::string_view const reference = ToStringView(path, length);
			AssetHandle const handle = FindAsset(reference, static_cast<AssetType>(type), "Assets.Load");
			if (!handle.IsValid())
			{
				return 0;
			}
			if (Result<Ref<Asset>> asset = AssetManager::LoadAsset(handle); !asset)
			{
				Log::GetScriptLogger().error("Assets.Load: '{}' cannot be loaded: {}", reference, asset.GetError());
				return 0;
			}
			return ToScript(handle);
		}

		Ref<MaterialAsset> FindMaterial(uint64_t material, std::string_view function)
		{
			if (!AssetManager::IsInitialized())
			{
				Log::GetScriptLogger().error("{}: assets are unavailable", function);
				return nullptr;
			}
			Result<Ref<MaterialAsset>> asset = AssetManager::TryGetAsset<MaterialAsset>(ToHandle(material));
			if (!asset)
			{
				Log::GetScriptLogger().error("{}: {}", function, asset.GetError());
				return nullptr;
			}
			return asset.GetValue();
		}

		// A material owned by the running scene: scripts may change it, and it goes when the scene stops.
		uint64_t AddRuntimeMaterial(MaterialData data, std::string name, std::string_view function)
		{
			Scene* const scene = GetScene(function);
			if (scene == nullptr)
			{
				return 0;
			}
			if (!AssetManager::IsInitialized())
			{
				Log::GetScriptLogger().error("{}: assets are unavailable", function);
				return 0;
			}
			AssetHandle const handle = AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(std::move(data)), std::move(name));
			scene->AddRuntimeAsset(handle);
			return ToScript(handle);
		}

		uint64_t Material_Create()
		{
			return AddRuntimeMaterial(MaterialData{}, "Material", "Material.Create");
		}

		uint64_t Material_Clone(uint64_t material)
		{
			Ref<MaterialAsset> const source = FindMaterial(material, "Material.Clone");
			if (source == nullptr)
			{
				return 0;
			}
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(ToHandle(material));
			std::string name = metadata ? metadata->GetDisplayName() : std::string("Material");
			return AddRuntimeMaterial(source->GetData(), name + " (Clone)", "Material.Clone");
		}

		void Material_GetValues(uint64_t material, MaterialValues* values)
		{
			Ref<MaterialAsset> const asset = FindMaterial(material, "Material");
			*values = ToScript(asset != nullptr ? asset->GetData() : MaterialData{});
		}

		uint8_t Material_IsRuntime(uint64_t material)
		{
			Scene const* const scene = ScriptEngine::GetSceneContext();
			return scene != nullptr && scene->IsRuntimeAsset(ToHandle(material)) ? 1 : 0;
		}

		// Materials of the project (files, built-ins, mesh materials) are shared with every scene and the editor, so scripts
		// change only the copies they make.
		void Material_SetValues(uint64_t material, MaterialValues const* values, char const* property, int32_t propertyLength)
		{
			std::string_view const name = ToStringView(property, propertyLength);
			std::string const function = "Material." + std::string(name);
			Scene const* const scene = GetScene(function);
			if (scene == nullptr)
			{
				return;
			}
			if (!scene->IsRuntimeAsset(ToHandle(material)))
			{
				Log::GetScriptLogger().error("{}: material {} belongs to the project; change a copy made with Clone() or Create()",
				                             function, material);
				return;
			}
			Ref<MaterialAsset> const asset = FindMaterial(material, function);
			MaterialData data;
			if (asset != nullptr && FromScript(*values, name, data))
			{
				asset->SetData(std::move(data));
			}
		}
	}

	AssetHandle FindAsset(std::string_view path, AssetType type, std::string_view function)
	{
		if (!AssetManager::IsInitialized())
		{
			Log::GetScriptLogger().error("{}: assets are unavailable", function);
			return {};
		}
		AssetHandle handle;
		if (path.find("://") != std::string_view::npos)
		{
			Result<AssetHandle> resolved = AssetManager::ResolveReference(path);
			if (!resolved)
			{
				Log::GetScriptLogger().error("{}: {}", function, resolved.GetError());
				return {};
			}
			handle = resolved.GetValue();
		}
		else
		{
			handle = AssetManager::FindByPath(path);
			if (!handle.IsValid())
			{
				Log::GetScriptLogger().error("{}: there is no asset at '{}'", function, path);
				return {};
			}
		}
		if (AssetType const actual = AssetManager::GetAssetType(handle); actual != type)
		{
			Log::GetScriptLogger().error("{}: '{}' is a {}, not a {}", function, path, AssetTypeToString(actual), AssetTypeToString(type));
			return {};
		}
		return handle;
	}

	void RegisterAssetBindings(BindingTable& table)
	{
		table.Add("Assets_Load", &Assets_Load);
		table.Add("Material_Create", &Material_Create);
		table.Add("Material_Clone", &Material_Clone);
		table.Add("Material_GetValues", &Material_GetValues);
		table.Add("Material_SetValues", &Material_SetValues);
		table.Add("Material_IsRuntime", &Material_IsRuntime);
	}
}
