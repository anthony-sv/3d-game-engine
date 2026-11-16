#pragma once

#include "Strada/Audio/AudioTypes.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Strada
{
	class AudioClipAsset;

	// The sound of an entity.
	struct AudioSourceDesc
	{
		UUID Entity = UUID::Invalid();
		Ref<AudioClipAsset> Clip;
		// When set, the sound streams from this file instead of playing Clip (which is then not needed): the audio streaming
		// thread reads and decodes it about a second ahead of the playback (AudioStreamBufferSeconds), so only that much of
		// it is in memory (music, long ambience). The file is opened, and its format detected as a clip's is, when the source
		// is created, which reads only its headers (at most MaxAudioStreamHeaderBytes); it stays open while the source exists.
		std::filesystem::path StreamFile;
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
		// The mixer's bus the source's voices play on: empty for Master; a name the mixer lacks plays on Master and is
		// logged once.
		std::string Bus;
		// 0 to MaxAudioPriority: at the engine's limit (MaxMixedAudioVoices) lower priorities are stolen first.
		uint32_t Priority = DefaultAudioPriority;
		// Voices of the source that play at once, its main voice included: 1 to MaxAudioSourceVoices.
		uint32_t MaxVoices = DefaultAudioSourceVoices;
	};

	// A loaded clip whose samples would take more than this once decoded (4 bytes a sample, in the clip's own channels and
	// sample rate) is refused: it must stream (AudioSourceDesc::StreamFile, AudioSourceComponent::Stream). 32 MiB is about
	// 87 s of 48 kHz stereo and 175 s of mono.
	inline constexpr uint64_t MaxDecodedClipBytes = 32ull * 1024 * 1024;

	// Lowest playback speed; lower values are raised to it.
	inline constexpr float MinAudioPitch = 0.01f;
	// Shortest attenuation distance in meters; shorter minimum distances are raised to it.
	inline constexpr float MinAudioDistance = 0.001f;

	// The sounds of a running scene, mixed by AudioEngine (Docs/Architecture.md §9.3). Each source owns a pool of voices: its
	// main voice, which Play, Pause and Stop drive, and its one-shots; the scene also plays voices of its own
	// (PlayClipAtPoint). A loaded clip is decoded once into samples every voice of it shares; a streamed source's main voice
	// plays what the audio streaming thread decoded ahead from its file, so the mixer never reads a file. Each scene has a
	// group per bus its voices use, below the mixer's global buses, so that pausing the scene holds its voices and no other
	// scene's. Every change reaches the mixing thread through the audio queue; the getters answer the values last set, at
	// once. Knows nothing about the ECS: the Scene creates sources from components and updates their positions and the
	// listener. Positions are heard relative to the scene's own listener, so several AudioScenes do not interfere. Calls with
	// an entity that has no source do nothing. Destroying the scene retires its voices and groups; the engine destroys them
	// when the mixer hands them back. Requires AudioEngine (destroy every AudioScene before AudioEngine::Shutdown). Main
	// thread only.
	class AudioScene
	{
	public:
		[[nodiscard]] static Result<Scope<AudioScene>> Create();
		~AudioScene();

		AudioScene(AudioScene const&) = delete;
		AudioScene& operator=(AudioScene const&) = delete;

		// Creates the stopped sound of an entity, replacing its current one. Fails without a clip or stream file, when a
		// stream's file cannot be opened, when the clip or file cannot be decoded (including files whose audio does not start
		// within MaxAudioStreamHeaderBytes), or for a stream when the streaming thread is not running (the current sound is
		// then kept).
		[[nodiscard]] Result<void> AddSource(AudioSourceDesc const& desc);
		void RemoveSource(UUID entity);
		bool HasSource(UUID entity) const;
		// Whether the entity's sound streams from its file.
		bool IsStreaming(UUID entity) const;
		size_t GetSourceCount() const;

		// Plays from the start, or from where Pause left it; a sound that played to its end starts over. A streamed sound is
		// silent until the streaming thread has decoded what it plays (moments after it was created or stopped).
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
		// The bus the source's voices play on from now on, playing ones included (each moved with a dip, never a click).
		void SetBus(UUID entity, std::string_view bus);
		std::string const& GetBus(UUID entity) const;
		void SetPriority(UUID entity, uint32_t priority);
		uint32_t GetPriority(UUID entity) const;
		// Raised to 1 and lowered to MaxAudioSourceVoices. A pool made smaller than the voices it plays steals its oldest
		// one-shots down to it.
		void SetMaxVoices(UUID entity, uint32_t maxVoices);
		uint32_t GetMaxVoices(UUID entity) const;

		// Plays clip once more on a new voice of the source's pool (the source's own clip when clip is null), with the
		// source's position, bus, priority and settings and this volume (a linear gain times the source's), beside its main
		// voice and its other one-shots. A full pool steals its oldest one-shot; a pool full of its main voice alone refuses
		// the one-shot. At the engine's limit the voice may itself be the one stolen. The voice ends when the clip does and
		// is freed without the main thread waiting for it. A streamed source's one-shots play its clip loaded. Returns
		// whether the voice started; a clip that cannot be decoded, or decodes to more than MaxDecodedClipBytes, is logged.
		bool PlayOneShot(UUID entity, Ref<AudioClipAsset> const& clip, float volume);
		// Voices of the source playing now (its main voice and its one-shots), as the main thread last knew them.
		uint32_t GetVoiceCount(UUID entity) const;

		// Plays clip once at a world position, on a voice of the scene rather than of an entity: spatial, with the default
		// distances (1 and 100 m), the default priority, at volume (linear), on bus (empty for Master). It is held while the
		// scene is paused and retired with the scene. Returns whether the voice started.
		bool PlayClipAtPoint(Ref<AudioClipAsset> const& clip, glm::vec3 const& position, float volume, std::string_view bus);

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
