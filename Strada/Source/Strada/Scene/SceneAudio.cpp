#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/Mixer.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Asset/PhysicsMaterialAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/ComponentTraits.h"
#include "Strada/Scene/Entity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace Strada
{
	namespace
	{
		glm::vec3 GetTranslation(glm::mat4 const& transform)
		{
			return glm::vec3(transform[3]);
		}

		// The settings of an entity's sound, as its component gives them (not its clip).
		AudioSourceDesc DescribeAudioSource(UUID entity, AudioSourceComponent const& component, glm::vec3 const& position)
		{
			AudioSourceDesc desc;
			desc.Entity = entity;
			desc.Volume = component.Volume;
			desc.Pitch = component.Pitch;
			desc.Loop = component.Loop;
			desc.Spatial = component.Spatial;
			desc.MinDistance = component.MinDistance;
			desc.MaxDistance = component.MaxDistance;
			desc.Bus = component.Bus;
			desc.Priority = component.Priority;
			desc.MaxVoices = component.MaxVoices;
			desc.DopplerLevel = component.DopplerLevel;
			desc.Position = position;
			return desc;
		}

		// The impact sound of a physics material: its ImpactSound when the handle names a registered physics material that
		// loads, else none (a collider without a material, or with one that does not load, makes no sound).
		AssetHandle FindImpactSound(AssetHandle material)
		{
			AssetType const type = TypedDataAsset<PhysicsMaterial>::GetStaticType();
			if (!material.IsValid() || !AssetManager::IsInitialized() || type == AssetType::None ||
			    AssetManager::GetAssetType(material) != type)
			{
				return AssetHandle();
			}
			Ref<TypedDataAsset<PhysicsMaterial>> const asset = AssetManager::GetAsset<TypedDataAsset<PhysicsMaterial>>(material);
			return asset ? asset->GetData().ImpactSound : AssetHandle();
		}
	}

	void Scene::StartAudio()
	{
		if (!AudioEngine::IsInitialized())
		{
			ST_CORE_WARN("Scene '{}' runs without audio: the audio engine is not initialized", m_Name);
			return;
		}
		Result<Scope<AudioScene>> audio = AudioScene::Create();
		if (!audio)
		{
			ST_CORE_ERROR("Scene '{}' runs without audio: {}", m_Name, audio.GetError());
			return;
		}
		m_Audio = audio.TakeValue();
		PreloadImpactSounds();
		// No game time has passed: the first update places the sounds and gives them no doppler.
		UpdateAudio(0.0f);
	}

	void Scene::StopAudio()
	{
		m_AudioSources.clear();
		ForgetAudioMotion();
		m_AudioZones.clear();
		m_ImpactClips.clear();
		m_FailedImpactClips.clear();
		m_Audio.reset();
	}

	void Scene::ForgetAudioMotion()
	{
		m_AudioListenerLastPosition.reset();
		m_AudioListenerLastEntity = UUID::Invalid();
		for (auto& [handle, state] : m_AudioSources)
		{
			state.LastPosition.reset();
		}
	}

	void Scene::UpdateAudio(float deltaTime)
	{
		if (!m_Audio)
		{
			return;
		}

		// Sounds of destroyed entities and removed components go first: a new entity may have taken their UUID.
		std::erase_if(m_AudioSources,
		              [this](auto const& entry)
		              {
						  auto const& [handle, state] = entry;
						  if (m_Registry.valid(handle) && m_Registry.all_of<AudioSourceComponent>(handle))
						  {
							  return false;
						  }
						  m_Audio->RemoveSource(state.Entity);
						  return true;
					  });

		// Components are compared with the settings last applied, so every way of changing them (inspector, scripts,
		// automation) reaches the sound.
		for (auto const [handle, source] : m_Registry.view<AudioSourceComponent>().each())
		{
			SyncAudioSource(handle, source);
		}

		// Doppler's velocities are the moves since the last update over the game time between (Docs/Architecture.md §9.8),
		// from the world transforms this update reads: the transform cache's previous matrices are the last drawn frame's,
		// and a scene that is not drawn (headless runs, tests, the player's audio capture) refreshes none.
		float const speedOfSound = AudioEngine::GetMixer().SpeedOfSound;
		Entity const listener = FindAudioListener();
		UUID const listenerId = listener ? listener.GetUUID() : UUID::Invalid();
		glm::mat4 const listenerTransform = listener ? GetWorldTransform(listener) : glm::mat4(1.0f);
		glm::vec3 const listenerPosition = GetTranslation(listenerTransform);
		// A listener that moved to another entity (a camera cut) did not travel the distance between them.
		std::optional<glm::vec3> listenerVelocity;
		if (m_AudioListenerLastPosition && listenerId == m_AudioListenerLastEntity)
		{
			listenerVelocity = ComputeAudioVelocity(*m_AudioListenerLastPosition, listenerPosition, deltaTime, speedOfSound);
		}
		m_AudioListenerLastPosition = listenerPosition;
		m_AudioListenerLastEntity = listenerId;
		m_Audio->SetListener(listenerPosition, -glm::vec3(listenerTransform[2]), glm::vec3(listenerTransform[1]));
		m_Audio->SetListenerVelocity(listenerVelocity);
		for (auto& [handle, state] : m_AudioSources)
		{
			if (!state.HasSound || !state.Applied.Spatial)
			{
				state.LastPosition.reset();
				continue;
			}
			glm::vec3 const position = GetTranslation(GetWorldTransform(Entity(handle, this)));
			m_Audio->SetPosition(state.Entity, position);
			m_Audio->SetVelocity(state.Entity, state.LastPosition
			                                       ? ComputeAudioVelocity(*state.LastPosition, position, deltaTime, speedOfSound)
			                                       : std::nullopt);
			state.LastPosition = position;
		}
		m_Audio->UpdateDoppler();
		UpdateReverbZones();
		// After the zones, so that an impact inside one sends to it from its first frame.
		PlayImpactSounds();
	}

	void Scene::UpdateReverbZones()
	{
		// Each zone placed and turned by its entity's world transform, sized times its scale. The vector keeps its memory,
		// and each element its bus name's, from one update to the next.
		size_t count = 0;
		for (auto const [handle, zone] : m_Registry.view<ReverbZoneComponent>().each())
		{
			if (count == m_AudioZones.size())
			{
				m_AudioZones.emplace_back();
			}
			AudioReverbZoneDesc& desc = m_AudioZones[count++];
			glm::mat4 const transform = GetWorldTransform(Entity(handle, this));
			glm::vec3 scale(0.0f);
			for (glm::length_t axis = 0; axis < 3; axis++)
			{
				glm::vec3 const column(transform[axis]);
				float const length = glm::length(column);
				scale[axis] = length;
				glm::vec3 unit(0.0f);
				unit[axis] = 1.0f;
				desc.Rotation[axis] = length > 1e-6f ? column / length : unit;
			}
			// Scripts set fields without their ranges: the zone takes them within. The bus name is assigned below rather than
			// copied with the rest, so that the element's string keeps its memory (every field is named, so a new one cannot
			// be left out: -Wmissing-designated-field-initializers).
			ReverbZoneComponent fitted{.Shape = zone.Shape,
			                           .Radius = zone.Radius,
			                           .HalfExtents = zone.HalfExtents,
			                           .FadeDistance = zone.FadeDistance,
			                           .Bus = {},
			                           .SendLevel = zone.SendLevel};
			ClampToFieldRanges<ComponentTraits<ReverbZoneComponent>>(fitted);
			desc.Shape = fitted.Shape;
			desc.Position = GetTranslation(transform);
			desc.Radius = fitted.Radius * std::max({scale.x, scale.y, scale.z});
			desc.HalfExtents = fitted.HalfExtents * scale;
			desc.FadeDistance = fitted.FadeDistance;
			desc.Bus = zone.Bus;
			desc.Level = fitted.SendLevel;
		}
		m_AudioZones.resize(count);
		m_Audio->UpdateReverbZones(m_AudioZones);
	}

	void Scene::PlayImpactSounds()
	{
		MixerImpacts const& impacts = AudioEngine::GetMixer().Impacts;
		if (impacts.Bus.empty() || m_ContactEvents.empty())
		{
			return;
		}
		ST_PROFILE_SCOPE("Scene.ImpactSounds");
		// The collisions that began in the frame's steps with contact points, the strongest first (ties in the order the
		// events came: by step, then by their bodies' UUIDs), so that the limit keeps the same ones on every run.
		std::vector<ContactEvent const*> collisions;
		for (ContactEvent const& event : m_ContactEvents)
		{
			if (event.Type == ContactEventType::CollisionEnter && event.PointCount > 0 && event.Points[0].Impulse >= impacts.MinImpulse)
			{
				collisions.push_back(&event);
			}
		}
		std::stable_sort(collisions.begin(), collisions.end(),
		                 [](ContactEvent const* left, ContactEvent const* right)
		                 {
							 return left->Points[0].Impulse > right->Points[0].Impulse;
						 });
		uint32_t played = 0;
		for (ContactEvent const* collision : collisions)
		{
			// The strongest point, where each side's material sounds once; the same sound on both sides plays once.
			ContactPoint const& point = collision->Points[0];
			float const volume = std::min(point.Impulse / impacts.FullImpulse, 1.0f);
			std::array<AssetHandle, 2> const sounds = {FindImpactSound(point.Material), FindImpactSound(point.OtherMaterial)};
			for (size_t side = 0; side < sounds.size(); side++)
			{
				AssetHandle const sound = sounds[side];
				if (!sound.IsValid() || (side == 1 && sound == sounds[0]))
				{
					continue;
				}
				if (played >= impacts.MaxPerFrame)
				{
					return;
				}
				if (Ref<AudioClipAsset> const clip = GetImpactClip(sound))
				{
					m_Audio->PlayClipAtPoint(clip, point.Point, volume, impacts.Bus);
					played++;
				}
			}
		}
	}

	Ref<AudioClipAsset> Scene::GetImpactClip(AssetHandle clip)
	{
		if (auto const found = m_ImpactClips.find(clip); found != m_ImpactClips.end())
		{
			return found->second;
		}
		if (m_FailedImpactClips.contains(clip) || !AssetManager::IsInitialized())
		{
			return nullptr;
		}
		Result<Ref<AudioClipAsset>> loaded = AssetManager::TryGetAsset<AudioClipAsset>(clip);
		if (!loaded)
		{
			m_FailedImpactClips.insert(clip);
			ST_CORE_WARN("Scene '{}' plays no impact sound {}: {}", m_Name, AssetManager::GetReference(clip), loaded.GetError());
			return nullptr;
		}
		// Decoded now, once, rather than at its first play.
		if (Result<void> decoded = AudioEngine::PreloadClip(loaded.GetValue()); !decoded)
		{
			m_FailedImpactClips.insert(clip);
			ST_CORE_WARN("Scene '{}' plays no impact sound {}: {}", m_Name, AssetManager::GetReference(clip), decoded.GetError());
			return nullptr;
		}
		Ref<AudioClipAsset> const asset = loaded.TakeValue();
		m_ImpactClips.emplace(clip, asset);
		return asset;
	}

	void Scene::PreloadImpactSounds()
	{
		if (AudioEngine::GetMixer().Impacts.Bus.empty() || !AssetManager::IsInitialized())
		{
			return;
		}
		auto const preload = [this](AssetHandle materialHandle)
		{
			if (AssetHandle const sound = FindImpactSound(materialHandle); sound.IsValid())
			{
				GetImpactClip(sound);
			}
		};
		for (auto const [handle, collider] : m_Registry.view<BoxColliderComponent>().each())
		{
			preload(collider.Material);
		}
		for (auto const [handle, collider] : m_Registry.view<SphereColliderComponent>().each())
		{
			preload(collider.Material);
		}
		for (auto const [handle, collider] : m_Registry.view<CapsuleColliderComponent>().each())
		{
			preload(collider.Material);
		}
		for (auto const [handle, collider] : m_Registry.view<MeshColliderComponent>().each())
		{
			preload(collider.Material);
			for (AssetHandle const material : collider.Materials)
			{
				preload(material);
			}
		}
	}

	bool Scene::SyncAudioSource(entt::entity handle, AudioSourceComponent const& component)
	{
		auto const [entry, created] = m_AudioSources.try_emplace(handle);
		AudioSourceState& state = entry->second;
		if (created || component.Clip != state.Applied.Clip || component.Stream != state.Applied.Stream)
		{
			// New sources start when PlayOnStart is set; a new clip (or a change between streaming and loading it) on an
			// existing source waits for Play.
			state.Entity = m_Registry.get<IDComponent>(handle).ID;
			CreateAudioSource(handle, state, component, created);
		}
		else if (state.HasSound)
		{
			UpdateAudioSource(state, component);
		}
		state.Applied = component;
		return state.HasSound;
	}

	std::optional<UUID> Scene::PrepareAudioSource(Entity entity)
	{
		if (!m_Audio || !entity || entity.GetScene() != this)
		{
			return std::nullopt;
		}
		AudioSourceComponent const* const source = m_Registry.try_get<AudioSourceComponent>(entity.GetHandle());
		if (source == nullptr || !SyncAudioSource(entity.GetHandle(), *source))
		{
			return std::nullopt;
		}
		return entity.GetUUID();
	}

	void Scene::PlayAudioSource(Entity entity)
	{
		if (std::optional<UUID> const sound = PrepareAudioSource(entity))
		{
			m_Audio->Play(*sound);
		}
	}

	void Scene::PauseAudioSource(Entity entity)
	{
		if (std::optional<UUID> const sound = PrepareAudioSource(entity))
		{
			m_Audio->Pause(*sound);
		}
	}

	void Scene::StopAudioSource(Entity entity)
	{
		if (std::optional<UUID> const sound = PrepareAudioSource(entity))
		{
			m_Audio->Stop(*sound);
		}
	}

	bool Scene::IsAudioSourcePlaying(Entity entity)
	{
		std::optional<UUID> const sound = PrepareAudioSource(entity);
		return sound && m_Audio->IsPlaying(*sound);
	}

	bool Scene::PlayAudioOneShot(Entity entity, AssetHandle clip, float volume)
	{
		if (!m_Audio || !entity || entity.GetScene() != this)
		{
			return false;
		}
		AudioSourceComponent const* const component = m_Registry.try_get<AudioSourceComponent>(entity.GetHandle());
		if (component == nullptr)
		{
			return false;
		}
		bool const hasSound = SyncAudioSource(entity.GetHandle(), *component);
		// A component that names a clip plays one-shots only with a sound of its own; one that names none, only of clips
		// given (asked for its own, it has none to play: nothing to log).
		if ((!hasSound && component->Clip.IsValid()) || (!clip.IsValid() && !component->Clip.IsValid()))
		{
			return false;
		}
		// A streamed source's sound reads its file and has no loaded clip: its own one-shots load the clip.
		AssetHandle const played = !clip.IsValid() && component->Stream ? component->Clip : clip;
		Ref<AudioClipAsset> clipAsset;
		if (played.IsValid())
		{
			if (!AssetManager::IsInitialized())
			{
				ST_CORE_WARN("Entity '{}' plays no one-shot: the asset manager is not initialized", entity.GetName());
				return false;
			}
			Result<Ref<AudioClipAsset>> loaded = AssetManager::TryGetAsset<AudioClipAsset>(played);
			if (!loaded)
			{
				ST_CORE_WARN("Entity '{}' plays no one-shot: {}", entity.GetName(), loaded.GetError());
				return false;
			}
			clipAsset = loaded.TakeValue();
		}
		AudioSourceState& state = m_AudioSources.at(entity.GetHandle());
		if (!hasSound)
		{
			AudioSourceDesc const desc = DescribeAudioSource(state.Entity, *component, GetTranslation(GetWorldTransform(entity)));
			if (Result<void> added = m_Audio->AddVoicePool(desc); !added)
			{
				ST_CORE_WARN("Entity '{}' plays no one-shot: {}", entity.GetName(), added.GetError());
				return false;
			}
			state.HasSound = true;
			state.LastPosition.reset();
		}
		return m_Audio->PlayOneShot(state.Entity, clipAsset, volume);
	}

	bool Scene::PlayAudioClipAtPoint(AssetHandle clip, glm::vec3 const& position, float volume, std::string_view bus)
	{
		if (!m_Audio)
		{
			return false;
		}
		if (!AssetManager::IsInitialized())
		{
			ST_CORE_WARN("Scene '{}' plays no clip at a point: the asset manager is not initialized", m_Name);
			return false;
		}
		Result<Ref<AudioClipAsset>> loaded = AssetManager::TryGetAsset<AudioClipAsset>(clip);
		if (!loaded)
		{
			ST_CORE_WARN("Scene '{}' plays no clip at a point: {}", m_Name, loaded.GetError());
			return false;
		}
		return m_Audio->PlayClipAtPoint(loaded.GetValue(), position, volume, bus);
	}

	void Scene::CreateAudioSource(entt::entity handle, AudioSourceState& state, AudioSourceComponent const& component, bool playOnStart)
	{
		m_Audio->RemoveSource(state.Entity);
		state.HasSound = false;
		state.LastPosition.reset();
		if (!component.Clip.IsValid())
		{
			return;
		}

		Entity const entity(handle, this);
		if (!AssetManager::IsInitialized())
		{
			ST_CORE_WARN("Entity '{}' plays no sound: the asset manager is not initialized", entity.GetName());
			return;
		}

		AudioSourceDesc desc = DescribeAudioSource(state.Entity, component, GetTranslation(GetWorldTransform(entity)));
		if (component.Stream)
		{
			// The sound reads the clip's file itself: the clip is not loaded.
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(component.Clip);
			if (!metadata || metadata->Type != AssetType::AudioClip || !metadata->IsFileAsset())
			{
				ST_CORE_WARN("Entity '{}' plays no sound: asset {} is not an audio clip file to stream", entity.GetName(), component.Clip);
				return;
			}
			desc.StreamFile = AssetManager::GetAbsolutePath(component.Clip);
		}
		else
		{
			Result<Ref<AudioClipAsset>> clip = AssetManager::TryGetAsset<AudioClipAsset>(component.Clip);
			if (!clip)
			{
				ST_CORE_WARN("Entity '{}' plays no sound: {}", entity.GetName(), clip.GetError());
				return;
			}
			desc.Clip = clip.TakeValue();
		}
		if (Result<void> added = m_Audio->AddSource(desc); !added)
		{
			ST_CORE_WARN("Entity '{}' plays no sound: {}", entity.GetName(), added.GetError());
			return;
		}
		state.HasSound = true;
		if (playOnStart && component.PlayOnStart)
		{
			m_Audio->Play(state.Entity);
		}
	}

	void Scene::UpdateAudioSource(AudioSourceState& state, AudioSourceComponent const& component)
	{
		AudioSourceComponent const& applied = state.Applied;
		if (component.Volume != applied.Volume)
		{
			m_Audio->SetVolume(state.Entity, component.Volume);
		}
		if (component.Pitch != applied.Pitch)
		{
			m_Audio->SetPitch(state.Entity, component.Pitch);
		}
		if (component.Loop != applied.Loop)
		{
			m_Audio->SetLooping(state.Entity, component.Loop);
		}
		if (component.Spatial != applied.Spatial)
		{
			m_Audio->SetSpatial(state.Entity, component.Spatial);
		}
		if (component.MinDistance != applied.MinDistance || component.MaxDistance != applied.MaxDistance)
		{
			m_Audio->SetDistances(state.Entity, component.MinDistance, component.MaxDistance);
		}
		if (component.Bus != applied.Bus)
		{
			m_Audio->SetBus(state.Entity, component.Bus);
		}
		if (component.Priority != applied.Priority)
		{
			m_Audio->SetPriority(state.Entity, component.Priority);
		}
		if (component.MaxVoices != applied.MaxVoices)
		{
			m_Audio->SetMaxVoices(state.Entity, component.MaxVoices);
		}
		if (component.DopplerLevel != applied.DopplerLevel)
		{
			m_Audio->SetDopplerLevel(state.Entity, component.DopplerLevel);
		}
	}

	Entity Scene::FindAudioListener()
	{
		// Scenes usually have at most one active listener; only several need the hierarchy walk to pick the first.
		entt::entity active = entt::null;
		size_t activeCount = 0;
		for (auto const [handle, listener] : m_Registry.view<AudioListenerComponent>().each())
		{
			if (listener.Active)
			{
				active = handle;
				activeCount++;
			}
		}
		if (activeCount == 0)
		{
			return GetPrimaryCameraEntity();
		}
		if (activeCount == 1)
		{
			return Entity(active, this);
		}

		Entity result;
		ForEachEntityInHierarchyOrder(
			[&result](Entity entity)
			{
				if (!result)
				{
					if (AudioListenerComponent const* listener = entity.TryGetComponent<AudioListenerComponent>();
				        listener != nullptr && listener->Active)
					{
						result = entity;
					}
				}
			});
		return result;
	}
}
