#include "Editor/AssetThumbnails.h"

#include "Editor/EntityBounds.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/FontAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Core/Log.h"
#include "Strada/Math/Math.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/Renderer/FontAtlas.h"
#include "Strada/Renderer/Renderer.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Renderer/TextLayout.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneRendering.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <span>
#include <system_error>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr float FieldOfView = glm::radians(30.0f);
		constexpr float EnvironmentFieldOfView = glm::radians(75.0f);
		// What font thumbnails write, and how much of the image its ink fills.
		constexpr std::string_view FontSampleText = "Aa";
		constexpr float FontSampleExtent = 0.74f;

		// Bilinear sample of a single-channel image (pixel centers at +0.5), clamped at the edges; 0 to 255.
		float SampleBilinear(std::span<uint8_t const> pixels, uint32_t width, uint32_t height, float x, float y)
		{
			float const fx = std::clamp(x - 0.5f, 0.0f, static_cast<float>(width - 1));
			float const fy = std::clamp(y - 0.5f, 0.0f, static_cast<float>(height - 1));
			uint32_t const x0 = static_cast<uint32_t>(fx);
			uint32_t const y0 = static_cast<uint32_t>(fy);
			uint32_t const x1 = std::min(x0 + 1, width - 1);
			uint32_t const y1 = std::min(y0 + 1, height - 1);
			float const tx = fx - static_cast<float>(x0);
			float const ty = fy - static_cast<float>(y0);
			auto const at = [&](uint32_t px, uint32_t py)
			{
				return static_cast<float>(pixels[static_cast<size_t>(py) * width + px]);
			};
			float const top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
			float const bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
			return top + (bottom - top) * ty;
		}
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

	AssetThumbnails::AssetThumbnails(size_t maxThumbnails, size_t maxWaveforms)
		: m_MaxThumbnails(maxThumbnails),
		  m_MaxWaveforms(maxWaveforms)
	{
	}

	AssetThumbnails::~AssetThumbnails()
	{
		StopWaveformJob();
	}

	bool AssetThumbnails::Supports(AssetType type)
	{
		switch (type)
		{
			case AssetType::Texture:
			case AssetType::Material:
			case AssetType::Mesh:
			case AssetType::Prefab:
			case AssetType::Environment:
			case AssetType::Font:
				return true;
			case AssetType::None:
			case AssetType::Scene:
			case AssetType::AudioClip:
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
		Entry& entry = m_Entries[asset];
		entry.LastUse = ++m_UseCounter;
		if (type == AssetType::Texture)
		{
			// Textures show themselves (the renderer keeps them), sampled without sRGB decoding: the stored, already
			// sRGB-encoded values reach the 8-bit swapchain unchanged. Only why a texture cannot be shown is kept here; the
			// asset manager remembers failed loads, so asking again is cheap.
			nvrhi::ITexture* const texture = Renderer::GetTexture(asset, false, AssetHandle());
			entry.Failed = texture == nullptr;
			entry.LoadError.clear();
			if (texture == nullptr)
			{
				if (Result<Ref<Asset>> const loaded = AssetManager::LoadAsset(asset); !loaded)
				{
					entry.LoadError = loaded.GetError();
				}
			}
			return texture;
		}

		bool const current =
			(entry.Texture || entry.Failed) && IsCurrent(entry.Source, entry.Loaded, entry.SourceVersion, entry.Failed, asset);
		if (!current && !m_Requested.IsValid())
		{
			// The first missing thumbnail drawn this frame renders next.
			m_Requested = asset;
		}
		return entry.Texture;
	}

	AudioWaveform const* AssetThumbnails::GetWaveform(AssetHandle clip)
	{
		if (AssetManager::GetAssetType(clip) != AssetType::AudioClip || AssetManager::IsMissing(clip))
		{
			return nullptr;
		}
		WaveformEntry& entry = m_Waveforms[clip];
		entry.LastUse = ++m_UseCounter;
		bool const current = (entry.Waveform || entry.Failed) && IsCurrent(entry.Source, entry.Loaded, 0, entry.Failed, clip);
		if (!current && !m_RequestedWaveform.IsValid() && !(m_WaveformJob && m_WaveformJob->Clip == clip))
		{
			m_RequestedWaveform = clip;
		}
		return entry.Waveform ? &*entry.Waveform : nullptr;
	}

	std::string const* AssetThumbnails::GetLoadError(AssetHandle asset) const
	{
		if (auto const found = m_Entries.find(asset); found != m_Entries.end() && found->second.Failed && !found->second.LoadError.empty())
		{
			return &found->second.LoadError;
		}
		if (auto const found = m_Waveforms.find(asset);
		    found != m_Waveforms.end() && found->second.Failed && !found->second.LoadError.empty())
		{
			return &found->second.LoadError;
		}
		return nullptr;
	}

	bool AssetThumbnails::IsCurrent(std::weak_ptr<Asset> const& source, bool sourceLoaded, uint64_t sourceVersion, bool failed,
	                                AssetHandle asset)
	{
		if (failed && !sourceLoaded)
		{
			// Until a refresh (which may find the file repaired) or a reload makes the asset manager forget the failed load.
			return AssetManager::HasLoadFailure(asset);
		}
		// Previews are made from loaded assets, which stay loaded: an asset that is no longer loaded was unloaded because
		// its file changed (or it was reloaded).
		if (!AssetManager::IsLoaded(asset))
		{
			return false;
		}
		Result<Ref<Asset>> const result = AssetManager::LoadAsset(asset);
		Ref<Asset> const loaded = result ? result.GetValue() : nullptr;
		if (loaded != source.lock())
		{
			return false;
		}
		Ref<MaterialAsset> const material = std::dynamic_pointer_cast<MaterialAsset>(loaded);
		return material == nullptr || material->GetVersion() == sourceVersion;
	}

	void AssetThumbnails::Update()
	{
		UpdateWaveforms();
		AssetHandle const asset = std::exchange(m_Requested, AssetHandle());
		if (asset.IsValid() && Renderer::IsInitialized())
		{
			if (auto const found = m_Entries.find(asset); found != m_Entries.end())
			{
				Entry& entry = found->second;
				bool const rendered =
					AssetManager::GetAssetType(asset) == AssetType::Font ? RenderFont(asset, entry) : Render(asset, entry);
				entry.Failed = !rendered;
			}
		}
		// Every frame: browsing only textures or audio clips adds entries too.
		Evict();
	}

	void AssetThumbnails::Clear()
	{
		StopWaveformJob();
		m_Entries.clear();
		m_Requested = AssetHandle();
		m_Waveforms.clear();
		m_RequestedWaveform = AssetHandle();
	}

	bool AssetThumbnails::Render(AssetHandle asset, Entry& entry)
	{
		Result<Ref<Asset>> const loaded = AssetManager::LoadAsset(asset);
		if (!loaded)
		{
			entry.Source = {};
			entry.Loaded = false;
			entry.LoadError = loaded.GetError();
			return false;
		}
		entry.LoadError.clear();
		entry.Source = loaded.GetValue();
		entry.Loaded = true;
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
		}
		if (!m_CommandList)
		{
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

	bool AssetThumbnails::RenderFont(AssetHandle asset, Entry& entry)
	{
		Result<Ref<FontAsset>> const font = AssetManager::TryGetAsset<FontAsset>(asset);
		if (!font)
		{
			entry.Source = {};
			entry.Loaded = false;
			entry.LoadError = font.GetError();
			return false;
		}
		entry.LoadError.clear();
		entry.Source = font.GetValue();
		entry.Loaded = true;
		entry.SourceVersion = 0;
		GpuFont* const glyphs = Renderer::GetFont(font.GetValue());
		if (glyphs == nullptr || !glyphs->Atlas)
		{
			return false;
		}
		Result<Image> const sample = RenderFontSample(*glyphs->Atlas, FontSampleText, Size);
		return sample && Upload(sample.GetValue(), entry);
	}

	bool AssetThumbnails::Upload(Image const& image, Entry& entry)
	{
		if (!entry.Texture)
		{
			nvrhi::TextureDesc desc;
			desc.width = image.GetWidth();
			desc.height = image.GetHeight();
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
		if (!m_CommandList)
		{
			m_CommandList = GraphicsDevice::GetDevice()->createCommandList();
		}
		m_CommandList->open();
		m_CommandList->writeTexture(entry.Texture, 0, 0, image.GetPixels().data(), static_cast<size_t>(image.GetWidth()) * 4);
		m_CommandList->close();
		GraphicsDevice::GetDevice()->executeCommandList(m_CommandList);
		return true;
	}

	Result<Image> AssetThumbnails::RenderFontSample(FontAtlas& atlas, std::string_view text, uint32_t size)
	{
		if (size == 0)
		{
			return Error{"a font sample needs pixels"};
		}
		TextLayout const layout = LayoutText(atlas, text);
		// The ink of each glyph: its quad without the distance field's margin.
		float const margin = static_cast<float>(FontAtlas::DistanceRange) / FontAtlas::PixelsPerLine;
		glm::vec2 inkMin(std::numeric_limits<float>::max());
		glm::vec2 inkMax(std::numeric_limits<float>::lowest());
		for (TextGlyphQuad const& glyph : layout.Glyphs)
		{
			inkMin = glm::min(inkMin, glyph.Min + margin);
			inkMax = glm::max(inkMax, glyph.Max - margin);
		}
		glm::vec2 const inkSize = inkMax - inkMin;
		if (layout.Glyphs.empty() || inkSize.x <= 0.0f || inkSize.y <= 0.0f)
		{
			return MakeError("the font has no outlines for '{}'", text);
		}

		// Pixels per line, and where the layout's origin lands so that the ink is centered.
		float const scale = FontSampleExtent * static_cast<float>(size) / std::max(inkSize.x, inkSize.y);
		glm::vec2 const origin = glm::vec2(static_cast<float>(size) * 0.5f) - (inkMin + inkMax) * 0.5f * scale;
		// Distances in atlas pixels become distances in image pixels.
		float const distanceToPixels = scale / FontAtlas::PixelsPerLine;

		Image image(size, size, 4);
		std::vector<uint8_t>& pixels = image.GetPixels();
		for (size_t index = 0; index < pixels.size(); index += 4)
		{
			pixels[index + 0] = 255;
			pixels[index + 1] = 255;
			pixels[index + 2] = 255;
			pixels[index + 3] = 0;
		}
		std::span<uint8_t const> const field = atlas.GetPixels();
		uint32_t const atlasWidth = atlas.GetWidth();
		uint32_t const atlasHeight = atlas.GetHeight();
		float const limit = static_cast<float>(size);
		for (TextGlyphQuad const& glyph : layout.Glyphs)
		{
			glm::vec2 const quadMin = origin + glyph.Min * scale;
			glm::vec2 const quadMax = origin + glyph.Max * scale;
			glm::vec2 const atlasMin(glyph.AtlasMin);
			glm::vec2 const atlasSize = glm::vec2(glyph.AtlasMax) - atlasMin;
			uint32_t const left = static_cast<uint32_t>(std::clamp(std::floor(quadMin.x), 0.0f, limit));
			uint32_t const right = static_cast<uint32_t>(std::clamp(std::ceil(quadMax.x), 0.0f, limit));
			uint32_t const top = static_cast<uint32_t>(std::clamp(std::floor(quadMin.y), 0.0f, limit));
			uint32_t const bottom = static_cast<uint32_t>(std::clamp(std::ceil(quadMax.y), 0.0f, limit));
			for (uint32_t y = top; y < bottom; y++)
			{
				for (uint32_t x = left; x < right; x++)
				{
					glm::vec2 const uv = (glm::vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f - quadMin) / (quadMax - quadMin);
					if (uv.x < 0.0f || uv.y < 0.0f || uv.x > 1.0f || uv.y > 1.0f)
					{
						continue;
					}
					glm::vec2 const sample = atlasMin + uv * atlasSize;
					float const value = SampleBilinear(field, atlasWidth, atlasHeight, sample.x, sample.y) / 255.0f;
					float const distance = (value - FontAtlas::GetEdgeSample()) * FontAtlas::GetDistanceScale() * distanceToPixels;
					float const coverage = std::clamp(distance + 0.5f, 0.0f, 1.0f);
					uint8_t& alpha = *(image.GetPixel(x, y) + 3);
					alpha = std::max(alpha, static_cast<uint8_t>(std::lround(coverage * 255.0f)));
				}
			}
		}
		return image;
	}

	void AssetThumbnails::UpdateWaveforms()
	{
		if (m_WaveformJob && m_WaveformJob->Future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			WaveformJob job = std::move(*m_WaveformJob);
			m_WaveformJob.reset();
			Result<AudioWaveform> result = job.Future.get();
			auto const found = m_Waveforms.find(job.Clip);
			if (found != m_Waveforms.end())
			{
				WaveformEntry& entry = found->second;
				entry.Source = job.Source;
				entry.Loaded = true;
				entry.Failed = !result;
				if (result)
				{
					entry.Waveform = result.TakeValue();
					entry.LoadError.clear();
				}
				else
				{
					entry.Waveform.reset();
					entry.LoadError = result.GetError();
					// Said once per clip shown: it is tried again when its file changes (a refresh reloads it).
					if (!std::exchange(entry.Reported, true))
					{
						ST_WARN("The waveform of {} cannot be shown: {}", AssetManager::GetReference(job.Clip), result.GetError());
					}
				}
			}
		}
		if (m_WaveformJob || !m_RequestedWaveform.IsValid())
		{
			return;
		}

		AssetHandle const clip = std::exchange(m_RequestedWaveform, AssetHandle());
		auto const found = m_Waveforms.find(clip);
		if (found == m_Waveforms.end())
		{
			return;
		}
		WaveformEntry& entry = found->second;
		Result<Ref<AudioClipAsset>> loaded = AssetManager::TryGetAsset<AudioClipAsset>(clip);
		if (!loaded)
		{
			entry.Source = {};
			entry.Loaded = false;
			entry.Waveform.reset();
			entry.Failed = true;
			entry.LoadError = loaded.GetError();
			return;
		}
		m_CancelWaveform = false;
		WaveformJob job;
		job.Clip = clip;
		job.Source = loaded.TakeValue();
		try
		{
			// The worker reads only the clip's encoded data, which the job keeps alive.
			job.Future = std::async(std::launch::async,
			                        [source = job.Source, &cancel = m_CancelWaveform]()
			                        {
										return ComputeAudioWaveform(*source, WaveformParts, &cancel);
									});
		}
		catch (std::system_error const& error)
		{
			// Tried again when the clip is loaded again.
			entry.Source = job.Source;
			entry.Loaded = true;
			entry.Failed = true;
			ST_WARN("Waveforms cannot be computed: {}", error.what());
			return;
		}
		m_WaveformJob = std::move(job);
	}

	void AssetThumbnails::StopWaveformJob()
	{
		if (!m_WaveformJob)
		{
			return;
		}
		m_CancelWaveform = true;
		m_WaveformJob->Future.wait();
		m_WaveformJob.reset();
	}

	void AssetThumbnails::Evict()
	{
		auto const evict = [](auto& entries, size_t limit)
		{
			if (entries.size() <= limit)
			{
				return;
			}
			std::vector<std::pair<uint64_t, AssetHandle>> uses;
			uses.reserve(entries.size());
			for (auto const& [asset, entry] : entries)
			{
				uses.emplace_back(entry.LastUse, asset);
			}
			size_t const excess = entries.size() - limit;
			std::nth_element(uses.begin(), uses.begin() + static_cast<std::ptrdiff_t>(excess), uses.end(),
			                 [](auto const& a, auto const& b)
			                 {
								 return a.first < b.first;
							 });
			for (size_t i = 0; i < excess; i++)
			{
				entries.erase(uses[i].second);
			}
		};
		evict(m_Entries, m_MaxThumbnails);
		evict(m_Waveforms, m_MaxWaveforms);
	}
}
