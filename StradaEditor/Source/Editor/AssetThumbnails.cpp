#include "Editor/AssetThumbnails.h"

#include "Editor/EntityBounds.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Core/Log.h"
#include "Strada/Math/Math.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/Renderer/Renderer.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneRendering.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr float FieldOfView = glm::radians(30.0f);
		constexpr float EnvironmentFieldOfView = glm::radians(75.0f);
	}

	ThumbnailCamera FrameThumbnailBounds(AABB const& bounds, float verticalFovRadians, float yawDegrees, float pitchDegrees)
	{
		glm::vec3 center(0.0f);
		float radius = 0.5f;
		if (bounds.IsValid())
		{
			center = bounds.GetCenter();
			radius = std::max(glm::length(bounds.Max - bounds.Min) * 0.5f, 1e-3f);
		}
		float const yaw = glm::radians(yawDegrees);
		float const pitch = glm::radians(pitchDegrees);
		// Yaw 0 and pitch 0 look down -Z.
		glm::vec3 const forward(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
		float const distance = radius / std::sin(verticalFovRadians * 0.5f) * 1.08f;

		ThumbnailCamera camera;
		camera.Target = center;
		camera.Position = center - forward * distance;
		camera.NearPlane = std::max(distance - radius * 1.5f, distance * 0.01f);
		return camera;
	}

	AssetThumbnails::AssetThumbnails() = default;
	AssetThumbnails::~AssetThumbnails() = default;

	bool AssetThumbnails::Supports(AssetType type)
	{
		switch (type)
		{
			case AssetType::Texture:
			case AssetType::Material:
			case AssetType::Mesh:
			case AssetType::Prefab:
			case AssetType::Environment:
				return true;
			case AssetType::None:
			case AssetType::Scene:
			case AssetType::AudioClip:
			case AssetType::Font:
				break;
		}
		return false;
	}

	nvrhi::ITexture* AssetThumbnails::Get(AssetHandle asset)
	{
		AssetType const type = AssetManager::GetAssetType(asset);
		if (!Renderer::IsInitialized() || !Supports(type) || AssetManager::IsMissing(asset))
		{
			return nullptr;
		}
		if (type == AssetType::Texture)
		{
			// Sampled without sRGB decoding: the stored, already sRGB-encoded values reach the 8-bit swapchain unchanged.
			return Renderer::GetTexture(asset, false, AssetHandle());
		}

		Entry& entry = m_Entries[asset];
		entry.LastUse = ++m_UseCounter;
		bool const current = (entry.Texture || entry.Failed) && IsCurrent(entry, asset);
		if (!current && !m_Requested.IsValid())
		{
			// The first missing thumbnail drawn this frame renders next.
			m_Requested = asset;
		}
		return entry.Texture;
	}

	bool AssetThumbnails::IsCurrent(Entry const& entry, AssetHandle asset)
	{
		// An asset nothing loaded keeps its thumbnail; showing one is no reason to load it.
		if (!AssetManager::IsLoaded(asset))
		{
			return true;
		}
		Result<Ref<Asset>> const result = AssetManager::LoadAsset(asset);
		Ref<Asset> const loaded = result ? result.GetValue() : nullptr;
		if (loaded != entry.Source.lock())
		{
			return false;
		}
		Ref<MaterialAsset> const material = std::dynamic_pointer_cast<MaterialAsset>(loaded);
		return material == nullptr || material->GetVersion() == entry.SourceVersion;
	}

	void AssetThumbnails::Update()
	{
		AssetHandle const asset = std::exchange(m_Requested, AssetHandle());
		if (!asset.IsValid() || !Renderer::IsInitialized())
		{
			return;
		}
		auto const found = m_Entries.find(asset);
		if (found == m_Entries.end())
		{
			return;
		}
		Entry& entry = found->second;
		entry.Failed = !Render(asset, entry);
		Evict();
	}

	void AssetThumbnails::Clear()
	{
		m_Entries.clear();
		m_Requested = AssetHandle();
	}

	bool AssetThumbnails::Render(AssetHandle asset, Entry& entry)
	{
		Result<Ref<Asset>> const loaded = AssetManager::LoadAsset(asset);
		if (!loaded)
		{
			entry.Source = {};
			return false;
		}
		entry.Source = loaded.GetValue();
		Ref<MaterialAsset> const material = std::dynamic_pointer_cast<MaterialAsset>(loaded.GetValue());
		entry.SourceVersion = material != nullptr ? material->GetVersion() : 0;

		AssetType const type = AssetManager::GetAssetType(asset);
		Scene scene("Thumbnail");
		Entity sun = scene.CreateEntity("Sun");
		sun.GetComponent<TransformComponent>().SetRotationEuler(glm::vec3(-40.0f, 35.0f, 0.0f));
		DirectionalLightComponent& light = sun.AddComponent<DirectionalLightComponent>();
		light.Intensity = 3.0f;
		light.CastShadows = false;
		Entity sky = scene.CreateEntity("Sky");
		SkyLightComponent& skyLight = sky.AddComponent<SkyLightComponent>();
		skyLight.Environment = type == AssetType::Environment ? asset : GetBuiltInHandle(BuiltInAsset::DefaultSky);
		// Other assets stand in front of the blurred default sky.
		skyLight.SkyboxBlur = type == AssetType::Environment ? 0.0f : 1.0f;

		std::vector<UUID> shown;
		switch (type)
		{
			case AssetType::Mesh:
			{
				Entity entity = scene.CreateEntity("Mesh");
				entity.AddComponent<MeshComponent>().Mesh = asset;
				shown.push_back(entity.GetUUID());
				break;
			}
			case AssetType::Material:
			{
				Entity entity = scene.CreateEntity("Sphere");
				MeshComponent& mesh = entity.AddComponent<MeshComponent>();
				mesh.Mesh = GetBuiltInHandle(BuiltInAsset::SphereMesh);
				mesh.Materials = {asset};
				shown.push_back(entity.GetUUID());
				break;
			}
			case AssetType::Prefab:
			{
				Result<Entity> const instance =
					scene.InstantiatePrefab(asset, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), Entity());
				if (!instance)
				{
					return false;
				}
				scene.ForEachEntityInHierarchyOrder(
					[&](Entity entity)
					{
						if (entity != sun && entity != sky)
						{
							shown.push_back(entity.GetUUID());
						}
					});
				break;
			}
			case AssetType::Environment:
				break;
			case AssetType::None:
			case AssetType::Scene:
			case AssetType::Texture:
			case AssetType::AudioClip:
			case AssetType::Font:
				return false;
		}

		if (!m_Renderer)
		{
			m_Renderer = CreateScope<SceneRenderer>();
			m_Renderer->SetViewportSize(Size, Size);
			m_CommandList = GraphicsDevice::GetDevice()->createCommandList();
		}

		SceneRendererCamera camera;
		if (type == AssetType::Environment)
		{
			// The sky towards the horizon.
			glm::vec3 const forward = glm::normalize(glm::vec3(0.0f, 0.15f, -1.0f));
			camera.View = glm::lookAt(glm::vec3(0.0f), forward, glm::vec3(0.0f, 1.0f, 0.0f));
			camera.Projection = Math::PerspectiveReversedZ(EnvironmentFieldOfView, 1.0f, 0.1f);
		}
		else
		{
			ThumbnailCamera const framing = FrameThumbnailBounds(ComputeEntityBounds(scene, shown), FieldOfView);
			camera.View = glm::lookAt(framing.Position, framing.Target, glm::vec3(0.0f, 1.0f, 0.0f));
			camera.Projection = Math::PerspectiveReversedZ(FieldOfView, 1.0f, framing.NearPlane);
			camera.Position = framing.Position;
		}
		RenderScene(scene, *m_Renderer, camera);

		if (!entry.Texture)
		{
			nvrhi::TextureDesc desc;
			desc.width = Size;
			desc.height = Size;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.isShaderResource = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "Asset thumbnail";
			entry.Texture = GraphicsDevice::GetDevice()->createTexture(desc);
			if (!entry.Texture)
			{
				return false;
			}
		}
		m_CommandList->open();
		m_CommandList->copyTexture(entry.Texture, nvrhi::TextureSlice(), m_Renderer->GetFinalImage(), nvrhi::TextureSlice());
		m_CommandList->close();
		GraphicsDevice::GetDevice()->executeCommandList(m_CommandList);
		return true;
	}

	void AssetThumbnails::Evict()
	{
		if (m_Entries.size() <= MaxThumbnails)
		{
			return;
		}
		std::vector<std::pair<uint64_t, AssetHandle>> uses;
		uses.reserve(m_Entries.size());
		for (auto const& [asset, entry] : m_Entries)
		{
			uses.emplace_back(entry.LastUse, asset);
		}
		size_t const excess = m_Entries.size() - MaxThumbnails;
		std::nth_element(uses.begin(), uses.begin() + static_cast<std::ptrdiff_t>(excess), uses.end(),
		                 [](auto const& a, auto const& b)
		                 {
							 return a.first < b.first;
						 });
		for (size_t i = 0; i < excess; i++)
		{
			m_Entries.erase(uses[i].second);
		}
	}
}
