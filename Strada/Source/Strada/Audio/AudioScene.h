#pragma once

#include "Strada/Audio/AudioTypes.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
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
		// The mixer's bus the source's voices play on: empty for Master; names match ignoring letter case; a name the mixer
		// lacks plays on Master and is logged once.
		std::string Bus;
		// 0 to MaxAudioPriority: at the engine's limit (MaxMixedAudioVoices) lower priorities are stolen first.
		uint32_t Priority = DefaultAudioPriority;
		// Voices of the source that play at once, its main voice included: 1 to MaxAudioSourceVoices.
		uint32_t MaxVoices = DefaultAudioSourceVoices;
		// How much of the doppler effect the source's voices take while spatial (Docs/Architecture.md §9.8): 0 none, 1
		// physical, up to MaxAudioDopplerLevel exaggerated.
		float DopplerLevel = DefaultAudioDopplerLevel;
	};

	// Doppler (Docs/Architecture.md §9.8). Positions are heard relative to the listener, so miniaudio's doppler has nothing
	// to read; the scene computes velocities from where things were at its last update and where they are now, over the
	// game time between, and the AudioScene turns them into a factor each voice's pitch is multiplied by.
	//
	// The velocity of something that moved from `from` to `to` in `seconds` of game time, in meters per second: none when
	// the time is not positive and finite, a position is not finite, or the move is faster than half the speed of sound (a
	// teleport, which no speed explains, so it gives no doppler rather than a jump of an octave). Any thread.
	[[nodiscard]] std::optional<glm::vec3> ComputeAudioVelocity(glm::vec3 const& from, glm::vec3 const& to, float seconds,
	                                                            float speedOfSound);
	// The factor c / (c - v * level), v being the speed at which source and listener close in along the line between them
	// (positive as they come closer), c the speed of sound; within MinAudioDopplerFactor and MaxAudioDopplerFactor (also
	// when v * level reaches c). 1 when level is not positive, when source and listener are less than a millimeter apart
	// (no line between them), and when anything is not finite. Any thread.
	[[nodiscard]] float ComputeDopplerFactor(glm::vec3 const& sourcePosition, glm::vec3 const& sourceVelocity,
	                                         glm::vec3 const& listenerPosition, glm::vec3 const& listenerVelocity, float level,
	                                         float speedOfSound);

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
		// within MaxAudioStreamHeaderBytes, and loaded clips past MaxDecodedClipBytes), or for a stream when the streaming
		// thread is not running (the current sound is then kept). A loaded clip is decoded here, once per clip asset object.
		[[nodiscard]] Result<void> AddSource(AudioSourceDesc const& desc);
		// Creates an entity's sound without a main voice, replacing its current one: a pool for the one-shots of clips given
		// to PlayOneShot, with desc's settings (its Clip, StreamFile and Loop are not used). Play, Pause and SetLooping then
		// do nothing, IsPlaying and IsLooping read false, and Stop fades its one-shots out. For an entity whose component
		// names no clip (Scene makes one at its first one-shot). Fails only when the bus's group cannot be made (the current
		// sound is then kept).
		[[nodiscard]] Result<void> AddVoicePool(AudioSourceDesc const& desc);
		// The source's voices, its one-shots included, fade out over AudioGainSmoothingSeconds and are gone (a destroyed
		// entity's sounds end with it; PlayClipAtPoint plays a sound that outlives its entity).
		void RemoveSource(UUID entity);
		bool HasSource(UUID entity) const;
		// Whether the entity's sound streams from its file.
		bool IsStreaming(UUID entity) const;
		size_t GetSourceCount() const;

		// Plays the main voice from the start, or from where Pause left it; a sound that played to its end starts over. A
		// streamed sound is silent until the streaming thread has decoded what it plays (moments after it was created or
		// stopped). At the engine's limit the main voice may become virtual: it plays on in time without being heard and is
		// heard again where its time has taken it once it ranks within the limit (§9.3).
		void Play(UUID entity);
		// Stops the main voice, keeping the position; one-shots play on.
		void Pause(UUID entity);
		// Stops the main voice and rewinds it to the start; the source's one-shots fade out over AudioGainSmoothingSeconds.
		void Stop(UUID entity);
		// The main voice started and not yet at its end (looping sounds never end). Held sounds of a paused scene and
		// virtual voices count as playing.
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
		// Names match ignoring letter case; empty is Master.
		void SetBus(UUID entity, std::string_view bus);
		std::string const& GetBus(UUID entity) const;
		// Lowered to MaxAudioPriority; one-shots playing take it too.
		void SetPriority(UUID entity, uint32_t priority);
		uint32_t GetPriority(UUID entity) const;
		// Raised to 1 and lowered to MaxAudioSourceVoices. A pool made smaller than the voices it plays steals its oldest
		// one-shots down to it.
		void SetMaxVoices(UUID entity, uint32_t maxVoices);
		uint32_t GetMaxVoices(UUID entity) const;
		// Kept within 0 and MaxAudioDopplerLevel (NaN is 0); applies from the next UpdateDoppler.
		void SetDopplerLevel(UUID entity, float level);
		float GetDopplerLevel(UUID entity) const;
		// The source's velocity in meters per second of game time (ComputeAudioVelocity), for doppler from the next
		// UpdateDoppler on; none (its first update, a teleport, no game time) gives its voices no doppler. A source made, or
		// made not spatial, has none until it is set.
		void SetVelocity(UUID entity, std::optional<glm::vec3> const& velocity);
		// The listener's velocity, as SetVelocity's; none gives every voice no doppler.
		void SetListenerVelocity(std::optional<glm::vec3> const& velocity);
		// Once an update, after the listener's and the sources' positions and velocities: gives every voice its doppler
		// factor (ComputeDopplerFactor with the mixer's SpeedOfSound), which multiplies its pitch, ramped as a pitch is
		// (AudioParameterSmoothingSeconds). A source's voices share its factor, one-shots started later included; a source
		// that is not spatial, of level 0, or whose velocity or the listener's is unknown takes 1. The scene's own voices
		// (PlayClipAtPoint) stand still and take the listener's motion alone.
		void UpdateDoppler();
		// The factor the source's voices play at, as UpdateDoppler last gave it (1 for none, or without a source).
		float GetDoppler(UUID entity) const;

		// Plays clip once more on a new voice of the source's pool (the source's own clip when clip is null), beside its
		// main voice and its other one-shots, at this volume (a linear gain times the source's). A one-shot never loops;
		// it follows its source's position, volume, pitch, spatialization, distances, bus and priority as they change, and
		// fades out with the source's Stop, its removal or its scene's end. A full pool (MaxVoices, its main voice counted
		// while it plays or is paused) steals its oldest one-shot; a pool full of its main voice alone refuses the
		// one-shot. At the engine's limit the voice may itself be the one refused. A one-shot that ended frees its place
		// before the next one starts, without a frame of the main thread waiting for it. A streamed source and a pool alone
		// (AddVoicePool) have no loaded clip: their one-shots need one (Scene::PlayAudioOneShot gives a streamed source's
		// one-shot the component's clip, loaded). Returns whether the voice started (or waits for its paused scene); a clip that
		// cannot be decoded, or decodes to more than MaxDecodedClipBytes, is logged once and plays nothing.
		bool PlayOneShot(UUID entity, Ref<AudioClipAsset> const& clip, float volume);
		// Voices of the source playing now (its main voice and its one-shots), as the main thread last knew them.
		uint32_t GetVoiceCount(UUID entity) const;

		// Plays clip once at a world position, on a voice of the scene rather than of an entity: spatial, with the default
		// distances (1 and 100 m), the default priority, at volume (linear), on bus (empty for Master), never looping. It is
		// held while the scene is paused, fades out with the scene's end, and outlives whatever entity played it. Returns
		// whether the voice started (or waits for its paused scene).
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
