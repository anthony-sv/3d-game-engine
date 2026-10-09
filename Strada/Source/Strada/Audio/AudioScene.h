#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"

#include <glm/glm.hpp>

namespace Strada
{
	class AudioClipAsset;

	// The sound of an entity.
	struct AudioSourceDesc
	{
		UUID Entity = UUID::Invalid();
		Ref<AudioClipAsset> Clip;
		// Linear gain (at least 0).
		float Volume = 1.0f;
		// Playback speed multiplier, which also shifts the pitch (at least MinAudioPitch).
		float Pitch = 1.0f;
		bool Loop = false;
		// Positioned in 3D relative to the listener: full volume up to MinDistance, then inverse-distance attenuation up
		// to MaxDistance (MinDistance / distance), and no further attenuation beyond. Otherwise played at a constant
		// level on every channel.
		bool Spatial = true;
		float MinDistance = 1.0f;
		float MaxDistance = 100.0f;
		// World-space position.
		glm::vec3 Position = glm::vec3(0.0f);
	};

	// Lowest playback speed; lower values are raised to it.
	inline constexpr float MinAudioPitch = 0.01f;
	// Shortest attenuation distance in meters; shorter minimum distances are raised to it.
	inline constexpr float MinAudioDistance = 0.001f;

	// The sounds of a running scene, mixed by AudioEngine. Each sound decodes its clip while it plays. Knows nothing about
	// the ECS: the Scene creates sources from components and updates their positions and the listener. Positions are
	// heard relative to the scene's own listener, so several AudioScenes do not interfere. Calls with an entity that has
	// no source do nothing. Requires AudioEngine (destroy every AudioScene before AudioEngine::Shutdown). Main thread only.
	class AudioScene
	{
	public:
		[[nodiscard]] static Result<Scope<AudioScene>> Create();
		~AudioScene();

		AudioScene(AudioScene const&) = delete;
		AudioScene& operator=(AudioScene const&) = delete;

		// Creates the stopped sound of an entity, replacing its current one. Fails without a clip or when the clip cannot
		// be decoded (the current sound is then kept).
		[[nodiscard]] Result<void> AddSource(AudioSourceDesc const& desc);
		void RemoveSource(UUID entity);
		bool HasSource(UUID entity) const;
		size_t GetSourceCount() const;

		// Plays from the start, or from where Pause left it; a sound that played to its end starts over.
		void Play(UUID entity);
		// Stops, keeping the position.
		void Pause(UUID entity);
		// Stops and rewinds to the start.
		void Stop(UUID entity);
		// Started and not yet at its end (looping sounds never end). Held sounds of a paused scene count as playing.
		bool IsPlaying(UUID entity) const;

		float GetVolume(UUID entity) const;
		void SetVolume(UUID entity, float volume);
		float GetPitch(UUID entity) const;
		void SetPitch(UUID entity, float pitch);
		bool IsLooping(UUID entity) const;
		void SetLooping(UUID entity, bool loop);
		bool IsSpatial(UUID entity) const;
		void SetSpatial(UUID entity, bool spatial);
		void SetDistances(UUID entity, float minDistance, float maxDistance);
		glm::vec3 GetPosition(UUID entity) const;
		// Non-finite positions are ignored.
		void SetPosition(UUID entity, glm::vec3 const& position);

		// Where the scene is heard from and which way the listener faces; the vectors need not be normalized or
		// orthogonal (a missing forward faces -Z, a missing or parallel up picks one). Non-finite values are ignored.
		void SetListener(glm::vec3 const& position, glm::vec3 const& forward, glm::vec3 const& up);
		glm::vec3 GetListenerPosition() const;

		// Holds every sound where it is (they keep their play state) or lets them continue.
		void SetPaused(bool paused);
		bool IsPaused() const;

	private:
		struct Data;
		struct PrivateTag
		{
		};

	public:
		AudioScene(PrivateTag, Scope<Data> data);

	private:
		Scope<Data> m_Data;
	};
}
