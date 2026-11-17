#pragma once

// Internal to the Audio module.

#include "Strada/Audio/AudioQueue.h"
#include "Strada/Audio/AudioTypes.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <glm/glm.hpp>

#include <miniaudio.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Voices and the decoded samples they play (Docs/Architecture.md §9.3 and §9.4), and the main thread's side of the mix:
// the engine's table of voices, its limit and the objects it waits to get back.
namespace Strada
{
	class AudioClipAsset;
	class AudioSceneGroup;
	class AudioStream;
	class AudioVoice;

	// A loaded clip's samples, decoded once on the main thread and shared by every voice that plays the clip, in every
	// scene. Immutable once made, so any number of voices read it at once from cursors of their own. Its memory is
	// GetByteSize(), held while the clip asset object it was decoded from lives (AudioSampleCache).
	class AudioSampleBuffer
	{
		struct PrivateTag
		{
		};

	public:
		// Main thread: decodes the whole clip with its format's AudioDecoder. A clip whose length the decoder reports (WAV,
		// FLAC, Ogg Vorbis) is refused before decoding when it would pass MaxDecodedClipBytes; any clip is refused when its
		// decoded samples pass it. A clip that stops decoding part way keeps what decoded, as a damaged clip played from
		// memory always did. Fails as AudioDecoder::Create fails.
		[[nodiscard]] static Result<Ref<AudioSampleBuffer>> Decode(AudioClipAsset const& clip);

		AudioSampleBuffer(PrivateTag, std::vector<float> samples, uint32_t channelCount, uint32_t sampleRate,
		                  std::vector<ma_channel> channelMap);

		AudioSampleBuffer(AudioSampleBuffer const&) = delete;
		AudioSampleBuffer& operator=(AudioSampleBuffer const&) = delete;

		// Interleaved 32-bit float frames, GetChannelCount() samples each.
		std::span<float const> GetSamples() const { return m_Samples; }
		uint64_t GetFrameCount() const { return m_ChannelCount != 0 ? m_Samples.size() / m_ChannelCount : 0; }
		uint32_t GetChannelCount() const { return m_ChannelCount; }
		uint32_t GetSampleRate() const { return m_SampleRate; }
		std::span<ma_channel const> GetChannelMap() const { return m_ChannelMap; }
		uint64_t GetByteSize() const { return m_Samples.size() * sizeof(float); }

	private:
		std::vector<float> m_Samples;
		uint32_t m_ChannelCount = 0;
		uint32_t m_SampleRate = 0;
		std::vector<ma_channel> m_ChannelMap;
	};

	// The decoded buffers of the loaded clips, so that a clip is decoded once per clip asset object however many sources
	// and voices play it, and however often its one-shots end and start again. Keyed by the clip asset object, held
	// weakly: an entry holds its buffer for as long as that object lives and is dropped with it (a reloaded clip is a new
	// object and decodes again), so the memory is what the loaded clips cost once decoded. A clip that cannot be decoded
	// keeps its error, so that it is not decoded again at every play. Owned by AudioEngine. Main thread only.
	class AudioSampleCache
	{
	public:
		// The clip's buffer, decoding it on first use (AudioSampleBuffer::Decode, which says why it fails). Drops the
		// entries of clips that are gone first.
		[[nodiscard]] Result<Ref<AudioSampleBuffer>> Get(Ref<AudioClipAsset> const& clip);
		// Drops entries whose clip is gone.
		void Prune();
		// Clips decoded since the cache was made (a diagnostic: a clip played many times decodes once).
		uint64_t GetDecodeCount() const { return m_DecodeCount; }
		size_t GetEntryCount() const { return m_Entries.size(); }

	private:
		// Entries Get finds before it first prunes.
		static constexpr size_t MinPruneSize = 16;

		struct Entry
		{
			std::weak_ptr<AudioClipAsset const> Clip;
			Ref<AudioSampleBuffer> Samples;
			std::string Error;
		};

		std::unordered_map<AudioClipAsset const*, Entry> m_Entries;
		// Get prunes once the entries reach this many (twice what the last prune kept), so that a game that plays
		// many clips prunes now and then rather than at every play.
		size_t m_NextPrune = MinPruneSize;
		uint64_t m_DecodeCount = 0;
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
		// A one-shot never loops, is retired when it ends and never plays again; a main voice stays attached, ended, until
		// it is played again or retired.
		bool OneShot = false;
	};

	// Who owns voices (an AudioScene): told when the engine's limit steals one of its one-shots, which it then hands to
	// AudioVoiceManager::Retire, and asked to retire the one-shots that ended. Main thread.
	class AudioVoiceOwner
	{
	public:
		virtual ~AudioVoiceOwner() = default;

		// The one-shot (never a main voice) is stolen or refused: the owner gives up its Scope to AudioVoiceManager::Retire.
		virtual void ReleaseVoice(AudioVoice& voice) = 0;
		// Retires the owner's one-shots whose play the mixing thread reported ended.
		virtual void RetireEndedVoices() = 0;

	protected:
		AudioVoiceOwner() = default;
	};

	// The intrusive links of a voice in one of the mixing thread's lists (playing, fading, virtual), so that a voice joins
	// and leaves a list without an allocation.
	struct AudioVoiceLink
	{
		AudioVoice* Previous = nullptr;
		AudioVoice* Next = nullptr;
		bool Linked = false;
	};

	// One playing instance of a clip: an ma_sound reading its clip's shared samples, or a streamed source's stream, with
	// miniaudio's resampling (pitch), attenuation and panning.
	//
	// The main thread creates the voice, unattached (MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT), and sets it up directly until
	// its first StartVoice command hands it to the mixing thread, which attaches it to its scene's group. From then on only
	// the mixing thread calls miniaudio on it, through commands; the main thread keeps a shadow of every value it set (what
	// the getters answer at once, with no block mixed in between) and reads what the mixing thread reports through an
	// atomic. When the voice comes back on the return list the main thread destroys it.
	//
	// Play states (main thread): stopped, paused or playing. A playing voice is mixing (the mixing thread was told to play
	// it), virtual (beyond the engine's limit: not mixed, its time running, §9.3) or waiting (its scene is paused); it stays
	// playing until it is paused or stopped, or the mixing thread reports that its play reached its end.
	class AudioVoice final : public AudioMixerObject
	{
		struct PrivateTag
		{
		};

	public:
		enum class State : uint8_t
		{
			Stopped,
			Paused,
			Playing
		};

		// Main thread: the voice, stopped and unattached, in group, with the description's values. Fails when miniaudio
		// cannot make the sound (it allocates the sound's node through the engine's allocation callbacks).
		[[nodiscard]] static Result<Scope<AudioVoice>> Create(AudioVoiceDesc const& desc, AudioSceneGroup& group, AudioVoiceOwner& owner);

		explicit AudioVoice(PrivateTag);
		// Main thread, only while the mixing thread does not hold the voice (never handed over, or returned).
		~AudioVoice() override;

		// Main thread: the shadow. These are the values last set, whether or not a block has applied them yet.
		float GetVolume() const { return m_Volume; }
		float GetPitch() const { return m_Pitch; }
		bool IsLooping() const { return m_Loop; }
		bool IsSpatial() const { return m_Spatial; }
		float GetMinDistance() const { return m_MinDistance; }
		float GetMaxDistance() const { return m_MaxDistance; }
		glm::vec3 GetRelativePosition() const { return m_RelativePosition; }
		uint32_t GetPriority() const { return m_Priority; }
		bool IsOneShot() const { return m_OneShot; }
		bool IsStreamed() const { return m_Stream != nullptr; }
		AudioSceneGroup& GetGroup() const { return *m_Group; }
		AudioVoiceOwner& GetOwner() const { return *m_Owner; }
		State GetState() const { return m_State; }
		// The engine's count of plays when this voice's current play began (Play from a stop or an end): smaller is older,
		// the stealing order's last key.
		uint64_t GetStartSequence() const { return m_StartSequence; }
		// Whether a StartVoice has handed the voice to the mixing thread (so that it comes back only through the queue).
		bool IsHeldByMixer() const { return m_HeldByMixer; }
		bool IsMixing() const { return m_Mixing; }
		bool IsVirtual() const { return m_Virtual; }

		// Main thread: playing and not reported ended (a looping voice never ends). A virtual voice is playing.
		bool IsPlaying() const { return m_State == State::Playing && !HasEnded(); }
		// Main thread: playing, but the mixing thread reported that this play reached its end.
		bool HasEnded() const { return m_State == State::Playing && GetEndedSerial() == m_PlaySerial; }
		// Main thread: what AudioVoiceManager::Refresh last found (GetActiveVoiceCount counts from it, so the count stays the
		// same between two refreshes whatever the mixing thread does meanwhile).
		bool WasEndedAtRefresh() const { return m_EndedAtRefresh; }
		// Any thread: the serial of the last play the mixing thread saw reach its end (0 before any).
		uint64_t GetEndedSerial() const { return m_EndedSerial.load(std::memory_order_acquire); }

		// Main thread: what the voice's owner asks for. Play marks the voice playing and waiting: AudioVoiceManager::Balance
		// then starts it (mixing or virtual) or, for a one-shot beyond the limit, refuses it. Playing a playing voice does
		// nothing; an ended one starts over.
		void Play();
		void Pause();
		void Stop();
		// Its scene pauses: a virtual voice stops its time where it has taken it and waits for the scene, as the scene's
		// other voices do (they wait in their paused group).
		void Hold();
		void SetVolume(float volume);
		void SetPitch(float pitch);
		void SetLooping(bool loop);
		void SetSpatial(bool spatial);
		void SetDistances(float minDistance, float maxDistance);
		void SetRelativePosition(glm::vec3 const& position);
		void SetPriority(uint32_t priority) { m_Priority = priority; }
		// The voice plays in group from now on (moved with a dip while it is heard).
		void MoveTo(AudioSceneGroup& group);

	private:
		friend class AudioMixer;
		friend class AudioVoiceManager;

		// A data source that reads the shared samples from the voice's own cursor and reports the clip's own sample rate.
		// (ma_audio_buffer_ref reports a rate of 0, miniaudio.h:60065, which the engine would take for its own,
		// :77111, so clips at other rates would play at the wrong speed.) Copies only; read by the thread that mixes.
		struct SampleReader
		{
			ma_data_source_base Base;
			AudioSampleBuffer const* Samples = nullptr;
			uint64_t Cursor = 0;
		};

		static ma_result OnRead(ma_data_source* dataSource, void* output, ma_uint64 frameCount, ma_uint64* framesRead);
		static ma_result OnSeek(ma_data_source* dataSource, ma_uint64 frame);
		static ma_result OnGetDataFormat(ma_data_source* dataSource, ma_format* format, ma_uint32* channels, ma_uint32* sampleRate,
		                                 ma_channel* channelMap, size_t channelMapCapacity);
		static ma_result OnGetCursor(ma_data_source* dataSource, ma_uint64* cursor);
		static ma_result OnGetLength(ma_data_source* dataSource, ma_uint64* length);

		// Main thread: a command about this voice.
		void Send(AudioCommandType type, std::array<float, 4> const& values = {}) const;
		// Main thread (AudioVoiceManager): hands the voice to the mixing thread or starts it again, as flags
		// (AudioStartFlags) says, with the serial of the current play.
		void SendStart(uint8_t flags);

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
		float m_MinDistance = 1.0f;
		float m_MaxDistance = 100.0f;
		glm::vec3 m_RelativePosition = glm::vec3(0.0f);
		uint32_t m_Priority = DefaultAudioPriority;
		bool m_OneShot = false;
		State m_State = State::Stopped;
		bool m_Mixing = false;
		bool m_Virtual = false;
		bool m_HeldByMixer = false;
		bool m_EndedAtRefresh = false;
		// A new play waits for its start sequence (from a stop or an end, not a resume).
		bool m_NewPlay = false;
		uint64_t m_StartSequence = 0;
		// The serial the last StartVoice carried.
		uint64_t m_PlaySerial = 0;
		AudioSceneGroup* m_Group = nullptr;
		AudioVoiceOwner* m_Owner = nullptr;
		// The voice's place in AudioVoiceManager's table (main thread), or NotRegistered.
		static constexpr size_t NotRegistered = ~size_t(0);
		size_t m_TableIndex = NotRegistered;

		// Written by the mixing thread when the play of that serial reaches its end.
		std::atomic<uint64_t> m_EndedSerial{0};

		// Mixing thread only (AudioMixer).
		enum class Fade : uint8_t
		{
			None,
			// Stolen: a one-shot is returned at the fade's end, a main voice becomes virtual.
			Steal,
			// Retired: returned at the fade's end.
			Retire,
			// Moving to m_MixMoveTarget: moved at the fade's end, then faded in.
			Move
		};
		uint64_t m_MixSerial = 0;
		AudioSceneGroup* m_MixGroup = nullptr;
		AudioSceneGroup* m_MixMoveTarget = nullptr;
		bool m_MixAttached = false;
		// The play reached its end (or a virtual voice's time passed it): the next start begins at the clip's start.
		bool m_MixEnded = false;
		bool m_MixLoop = false;
		Fade m_MixFade = Fade::None;
		uint64_t m_MixFadeEnd = 0;
		// The engine's time when the voice became virtual.
		uint64_t m_MixVirtualSince = 0;
		// Pitch on its way to its target, stepped once a block on a log2 scale.
		float m_MixPitch = 1.0f;
		float m_MixPitchTarget = 1.0f;
		float m_MixPitchStep = 0.0f;
		uint32_t m_MixPitchFramesLeft = 0;
		AudioVoiceLink m_PlayingLink;
		AudioVoiceLink m_FadingLink;
		AudioVoiceLink m_VirtualLink;
	};

	// The main thread's side of the mix (Docs/Architecture.md §9.1 to §9.3): the commands it sends, the table of every
	// voice of every scene (from which it decides every start and every steal at the engine's limit), and the objects it
	// waits to get back from the mixing thread. Owned by AudioEngine. Main thread only, except where a function says
	// otherwise.
	class AudioVoiceManager
	{
	public:
		explicit AudioVoiceManager(AudioCommandQueue& queue);
		~AudioVoiceManager();

		AudioVoiceManager(AudioVoiceManager const&) = delete;
		AudioVoiceManager& operator=(AudioVoiceManager const&) = delete;

		void Submit(AudioCommand const& command) { m_Queue.Push(command); }
		// A new serial for a StartVoice (every start has one).
		uint64_t NextSerial() { return ++m_Serials; }
		// A new start sequence (every new play has one).
		uint64_t NextStartSequence() { return ++m_StartSequences; }

		// The voice is done (its source or scene went, or its one-shot ended): RetireVoice is the last command that names
		// it, then KeepUntilReturned.
		void RetireVoice(Scope<AudioVoice> voice);
		// The voice leaves the table and is the engine's to destroy: at once when the mixing thread never held it,
		// otherwise once it comes back. The caller has sent the last command that names it (a RetireVoice, or the
		// StealVoice of a one-shot).
		void KeepUntilReturned(Scope<AudioVoice> voice);
		// A pool steals its oldest one-shot: StealVoice is the last command that names it (it fades out and comes back), and
		// it counts as stolen.
		void StealOneShot(Scope<AudioVoice> voice);
		// The group's scene ended: RetireGroup is its last command, and it comes back once its voices have.
		void RetireGroup(Scope<AudioSceneGroup> group);
		// Destroys what came back on the return list.
		void TakeReturns(AudioReturnList& returns);
		// Objects lent to the mixing thread and not yet back.
		size_t GetReturningCount() const { return m_Returning.size(); }
		// AudioEngine::Shutdown, once no thread mixes: destroys every object waiting to come back.
		void DestroyReturning();

		void AddVoice(AudioVoice& voice);
		void RemoveVoice(AudioVoice& voice);
		void AddOwner(AudioVoiceOwner& owner);
		void RemoveOwner(AudioVoiceOwner& owner);

		// Reads what the mixing thread reported: owners retire their ended one-shots, and every voice records whether it
		// ended (AudioVoice::WasEndedAtRefresh).
		void Refresh();
		// The engine's limit (§9.3): starts the voices that wait, devirtualizes virtual ones while there is room, and when
		// more would mix than MaxMixedAudioVoices keeps the ones that rank highest (priority, then loudness at the
		// listener, then the newest), steals the mixing ones that do not (a one-shot is returned, a main voice becomes
		// virtual), and refuses a waiting one-shot that does not.
		void Balance();
		// Whether virtual voices wait for room (AudioEngine::Update balances again while there are).
		bool HasVirtualVoices() const;

		// Voices mixing in scenes that are not paused, as of the last Refresh and the starts and steals since. A virtual
		// voice does not count; a stolen one stops counting at once.
		uint32_t GetActiveCount() const;
		void CountStolen(uint64_t count = 1) { m_Stolen.fetch_add(count, std::memory_order_relaxed); }
		// Any thread.
		uint64_t GetStolenCount() const { return m_Stolen.load(std::memory_order_relaxed); }

	private:
		// Held objects wait in m_Returning; others are destroyed now.
		void Keep(Scope<AudioMixerObject> object, bool held);
		// Whether the voice wants to mix now: playing, not ended, its scene not paused.
		static bool IsCandidate(AudioVoice const& voice);
		// The main thread's estimate of how loud the voice is at its listener, in amplitude (§9.3).
		static float EstimateLoudness(AudioVoice const& voice);
		// Starts a waiting or virtual voice in the mix.
		void StartMixing(AudioVoice& voice);
		// A voice that ranks beyond the limit: stolen while it mixes, refused (a one-shot) or made virtual (a main voice)
		// while it waits.
		void Exclude(AudioVoice& voice);

		AudioCommandQueue& m_Queue;
		std::vector<AudioVoice*> m_Voices;
		std::vector<AudioVoiceOwner*> m_Owners;
		std::unordered_map<AudioMixerObject*, Scope<AudioMixerObject>> m_Returning;
		uint64_t m_Serials = 0;
		uint64_t m_StartSequences = 0;
		std::atomic<uint64_t> m_Stolen{0};

		struct Candidate
		{
			AudioVoice* Voice = nullptr;
			uint32_t Priority = 0;
			float Loudness = 0.0f;
			uint64_t Sequence = 0;
		};
		// Reused by Balance, so that a balance allocates only when the voices outgrow it.
		std::vector<Candidate> m_Candidates;
	};
}
