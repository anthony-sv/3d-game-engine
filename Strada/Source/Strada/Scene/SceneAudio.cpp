#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Scene/Entity.h"

namespace Strada
{
	namespace
	{
		glm::vec3 GetTranslation(glm::mat4 const& transform)
		{
			return glm::vec3(transform[3]);
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
		UpdateAudio();
	}

	void Scene::StopAudio()
	{
		m_AudioSources.clear();
		m_Audio.reset();
	}

	void Scene::UpdateAudio()
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
		for (auto const [handle, id, source] : m_Registry.view<IDComponent, AudioSourceComponent>().each())
		{
			auto const [entry, created] = m_AudioSources.try_emplace(handle);
			AudioSourceState& state = entry->second;
			if (created || source.Clip != state.Applied.Clip)
			{
				// New sources start when PlayOnStart is set; a new clip on an existing source waits for Play.
				state.Entity = id.ID;
				CreateAudioSource(handle, state, source, created);
			}
			else if (state.HasSound)
			{
				UpdateAudioSource(state, source);
			}
			state.Applied = source;
		}

		Entity const listener = FindAudioListener();
		glm::mat4 const listenerTransform = listener ? GetWorldTransform(listener) : glm::mat4(1.0f);
		m_Audio->SetListener(GetTranslation(listenerTransform), -glm::vec3(listenerTransform[2]), glm::vec3(listenerTransform[1]));
		for (auto const& [handle, state] : m_AudioSources)
		{
			if (state.HasSound && state.Applied.Spatial)
			{
				m_Audio->SetPosition(state.Entity, GetTranslation(GetWorldTransform(Entity(handle, this))));
			}
		}
	}

	void Scene::CreateAudioSource(entt::entity handle, AudioSourceState& state, AudioSourceComponent const& component, bool playOnStart)
	{
		m_Audio->RemoveSource(state.Entity);
		state.HasSound = false;
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
		Result<Ref<AudioClipAsset>> clip = AssetManager::TryGetAsset<AudioClipAsset>(component.Clip);
		if (!clip)
		{
			ST_CORE_WARN("Entity '{}' plays no sound: {}", entity.GetName(), clip.GetError());
			return;
		}

		AudioSourceDesc desc;
		desc.Entity = state.Entity;
		desc.Clip = clip.TakeValue();
		desc.Volume = component.Volume;
		desc.Pitch = component.Pitch;
		desc.Loop = component.Loop;
		desc.Spatial = component.Spatial;
		desc.MinDistance = component.MinDistance;
		desc.MaxDistance = component.MaxDistance;
		desc.Position = GetTranslation(GetWorldTransform(entity));
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
