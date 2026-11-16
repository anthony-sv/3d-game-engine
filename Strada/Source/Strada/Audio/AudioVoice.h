#pragma once

// Internal to the Audio module.

#include "Strada/Audio/AudioQueue.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Audio/AudioTypes.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <glm/glm.hpp>

#include <miniaudio.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

// Voices and the decoded samples they play (Docs/Architecture.md §9.3 and §9.4).
namespace Strada
{
	class AudioClipAsset;
	class AudioSceneGroup;
	class AudioStream;

	// A loaded clip's samples, decoded once on the main thread and shared by every voice that plays the clip, in every
	// scene. Immutable once made, so any number of voices read it at once from cursors of their own. Its memory is
	// GetByteSize(), held while a voice or a source of the clip holds the buffer.
	class AudioSampleBuffer
	{
		struct PrivateTag
		{
		};

	public:
		// Main thread: decodes the whole clip with its format's AudioDecoder. A clip whose length the decoder knows (WAV,
		// FLAC; Ogg Vorbis from its last page) is refused before decoding when it would pass MaxDecodedClipBytes; one whose
		// length shows only while decoding (MP3) is refused when it passes it. A clip that stops decoding part way keeps what
		// decoded, as a damaged clip played from memory always did. Fails as AudioDecoder::Create fails.
		[[nodiscard]] static Result<Ref<AudioSampleBuffer>> Decode(AudioClipAsset const& clip);

		AudioSampleBuffer(PrivateTag, std::vector<float> samples, uint32_t channelCount, uint32_t sampleRate);

		AudioSampleBuffer(AudioSampleBuffer const&) = delete;
		AudioSampleBuffer& operator=(AudioSampleBuffer const&) = delete;

		// Interleaved 32-bit float frames, GetChannelCount() samples each.
		std::span<float const> GetSamples() const { return m_Samples; }
		uint64_t GetFrameCount() const { return m_ChannelCount != 0 ? m_Samples.size() / m_ChannelCount : 0; }
		uint32_t GetChannelCount() const { return m_ChannelCount; }
		uint32_t GetSampleRate() const { return m_SampleRate; }
		uint64_t GetByteSize() const { return m_Samples.size() * sizeof(float); }

	private:
		std::vector<float> m_Samples;
		uint32_t m_ChannelCount = 0;
		uint32_t m_SampleRate = 0;
	};

	// The decoded buffers of the clips that play, so that a clip is decoded once however many sources and voices play it.
	// Keyed by the clip asset object; an entry lives while something holds its buffer and the clip object it was decoded
	// from is still the one at that address (a reloaded clip is a new object and decodes again). Owned by AudioEngine.
	// Main thread only.
	class AudioSampleCache
	{
	public:
		// The clip's buffer, decoding it on first use (AudioSampleBuffer::Decode, which says why it fails).
		[[nodiscard]] Result<Ref<AudioSampleBuffer>> Get(Ref<AudioClipAsset> const& clip);
		// Drops entries whose buffer or clip is gone.
		void Prune();

	private:
		struct Entry
		{
			std::weak_ptr<AudioClipAsset const> Clip;
			std::weak_ptr<AudioSampleBuffer> Samples;
		};

		std::unordered_map<AudioClipAsset const*, Entry> m_Entries;
	};

	// What a voice plays and how it starts out: either Samples (a loaded clip) or Stream (the main voice of a streamed
	// source). The rest are the voice's first values, which it plays with from its first frame (the main thread clamps
	// them as AudioScene's setters do).
	struct AudioVoiceDesc
	{
		Ref<AudioSampleBuffer> Samples;
		Ref<AudioStream> Stream;
		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool Loop = false;
		bool Spatial = true;
		float MinDistance = 1.0f;
		float MaxDistance = 100.0f;
		// Relative to the scene's listener, in its basis (AudioScene computes it).
		glm::vec3 RelativePosition = glm::vec3(0.0f);
		uint32_t Priority = DefaultAudioPriority;
		// A one-shot is returned to the main thread when it ends (or is stolen) and never plays again; a main voice stays
		// attached, ended, until it is played again or retired.
		bool OneShot = false;
	};

	// One playing instance of a clip: an ma_sound reading its clip's shared samples, or a streamed source's stream, with
	// miniaudio's resampling (pitch), attenuation and panning.
	//
	// The main thread creates the voice, unattached (MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT), and may set it up directly until
	// its first StartVoice command hands it to the mixing thread, which attaches it to its scene's group. From then on only
	// the mixing thread calls miniaudio on it, through commands; the main thread keeps a shadow of every value it set (what
	// the getters answer at once, with no block mixed in between) and reads what the mixing thread reports through atomics.
	// When the voice comes back on the return list (a one-shot that ended or was stolen, or a voice retired) the main thread
	// owns it again and destroys it.
	class AudioVoice final : public AudioMixerObject
	{
		struct PrivateTag
		{
		};

	public:
		// Main thread: the voice, stopped and unattached, with the description's values. Fails when miniaudio cannot make
		// the sound (it allocates the sound's node through the engine's allocation callbacks).
		[[nodiscard]] static Result<Scope<AudioVoice>> Create(AudioVoiceDesc const& desc);

		explicit AudioVoice(PrivateTag);
		// Main thread, only while the mixing thread does not hold the voice (never handed over, or returned).
		~AudioVoice() override;

		// Main thread: the shadow. These are the values last set, whether or not a block has applied them yet.
		float GetVolume() const { return m_Volume; }
		float GetPitch() const { return m_Pitch; }
		bool IsLooping() const { return m_Loop; }
		bool IsSpatial() const { return m_Spatial; }
		uint32_t GetPriority() const { return m_Priority; }
		bool IsOneShot() const { return m_OneShot; }
		bool IsStreamed() const { return m_Stream != nullptr; }
		// The engine's count of started voices when this one last started: smaller is older (the stealing order).
		uint64_t GetStartSequence() const { return m_StartSequence; }
		// Whether a StartVoice has handed the voice to the mixing thread and it has not come back.
		bool IsHeldByMixer() const { return m_HeldByMixer; }

		// Main thread: whether the voice was last started and has neither been paused, stopped or stolen since, nor been
		// reported ended by the mixing thread (a looping voice never ends).
		bool IsPlaying() const;

		// Any thread: the serial of the last play the mixing thread saw reach its end (0 before any).
		uint64_t GetEndedSerial() const { return m_EndedSerial.load(std::memory_order_acquire); }

	private:
		friend class AudioMixer;
		friend class AudioScene;

		// A data source that reads the shared samples from the voice's own cursor and reports the clip's own sample rate.
		// (ma_audio_buffer_ref reports a rate of 0, miniaudio.h:60065, which the engine would take for its own,
		// :77111, so clips at other rates would play at the wrong speed.) Copies only; read by the thread that mixes.
		struct SampleReader
		{
			ma_data_source_base Base;
			AudioSampleBuffer const* Samples = nullptr;
			uint64_t Cursor = 0;
		};

		// What it plays. Held here, on the main thread's side, so that the last reference is never dropped on the mixing
		// thread.
		Ref<AudioSampleBuffer> m_Samples;
		Ref<AudioStream> m_Stream;
		SampleReader m_Reader{};
		bool m_HasReader = false;
		ma_sound m_Sound{};
		bool m_HasSound = false;

		// The shadow (main thread).
		float m_Volume = 1.0f;
		float m_Pitch = 1.0f;
		bool m_Loop = false;
		bool m_Spatial = true;
		uint32_t m_Priority = DefaultAudioPriority;
		bool m_OneShot = false;
		bool m_Playing = false;
		bool m_HeldByMixer = false;
		uint64_t m_StartSequence = 0;
		// The serial the last StartVoice carried.
		uint64_t m_PlaySerial = 0;
		// The scene group the voice was last sent to.
		AudioSceneGroup* m_Group = nullptr;

		// Written by the mixing thread when the play of that serial reaches its end.
		std::atomic<uint64_t> m_EndedSerial{0};

		// Mixing thread only: the play in progress, the pitch on its way to its target (stepped once a block), a fade
		// (stealing, or a move between groups) with the frames it has left and the group a move ends in, and the links of
		// the mixer's lists of ended and fading voices (intrusive, so that ending a voice allocates nothing).
		uint64_t m_MixSerial = 0;
		float m_MixPitch = 1.0f;
		float m_MixPitchTarget = 1.0f;
		uint32_t m_FadeFramesLeft = 0;
		AudioSceneGroup* m_MoveTarget = nullptr;
		AudioVoice* m_NextEnded = nullptr;
		AudioVoice* m_NextFading = nullptr;
	};
}
