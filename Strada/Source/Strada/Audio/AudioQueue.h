#pragma once

// Internal to the Audio module (and its tests: it includes no miniaudio header).

#include "Strada/Asset/Mixer.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <type_traits>

// The audio queue (Docs/Architecture.md §9.2): how the main thread changes what the mixing thread mixes. Every change is
// a command the mixing thread applies at the start of a block; objects travel by pointer in commands and come back on the
// return list, so that the main thread creates and destroys everything and the mixing thread never allocates, locks or
// waits.
//
// The rule of the last command: the mixing thread returns an object only in answer to the last command the main thread
// sends that names it (a RetireVoice, a StealVoice of a one-shot, a RetireGroup, a RemoveBus, which also returns the bus's
// chains, or the SetBusEffects that replaces a chain), and the main thread names an object in no command after that one. So no command in
// the ring or in the deferred commands can name an object that has come back, whatever the order in which the two threads run.
namespace Strada
{
	// Everything the mixing thread can hold: voices, the scenes' groups, buses and effect chains. The main thread creates
	// and destroys it (its owner keeps it in a Scope); between the command that hands it to the mixing thread and its
	// return on AudioReturnList, only the mixing thread touches its miniaudio objects.
	class AudioMixerObject
	{
	public:
		AudioMixerObject() = default;
		virtual ~AudioMixerObject() = default;

		AudioMixerObject(AudioMixerObject const&) = delete;
		AudioMixerObject& operator=(AudioMixerObject const&) = delete;

	private:
		friend class AudioReturnList;

		// The return list's link: written by the mixing thread when it hands the object back, read by the main thread.
		AudioMixerObject* m_NextReturned = nullptr;
	};

	enum class AudioCommandType : uint8_t
	{
		None = 0,

		// Voices (Object: the AudioVoice). A voice's first StartVoice hands it over and attaches it to Other, the
		// AudioSceneGroup of its scene and bus; Serial is the play's serial, which the voice reports when that play ends.
		// Flags says how it starts (AudioStartFlags): a voice that ended starts again from its beginning, a paused one
		// continues, a virtual one continues where its time has taken it, faded in.
		StartVoice,
		// Stops the voice where it is (Pause). A virtual voice stops where its time had taken it.
		PauseVoice,
		// Stops the voice and rewinds a loaded clip to its start (a stream is rewound by the main thread, §9.3).
		StopVoice,
		// Fades the voice out over AudioStealFadeSeconds. A one-shot is then detached and returned (the main thread names it
		// no more); a main voice becomes virtual where the fade ended.
		StealVoice,
		// Fades the voice out over AudioGainSmoothingSeconds when it is heard, then detaches and returns it (at once when
		// it is not heard: stopped, ended, virtual or in a paused group).
		RetireVoice,
		// A new target for the voice's linear volume (Values[0]), reached over AudioGainSmoothingSeconds while it plays and
		// taken at once while it does not.
		SetVoiceVolume,
		// A new target for the voice's pitch (Values[0]), reached over AudioParameterSmoothingSeconds while it plays and
		// taken at once while it does not.
		SetVoicePitch,
		// Values[0] is 1 for looping, 0 for not.
		SetVoiceLooping,
		// Values[0] is 1 for spatial, 0 for not.
		SetVoiceSpatial,
		// The attenuation's minimum and maximum distances (Values[0], Values[1]), already clamped by the main thread.
		SetVoiceDistances,
		// The voice's position relative to its scene's listener (Values[0..2]), in the listener's basis.
		SetVoicePosition,
		// The voice moves to Other, another AudioSceneGroup of its scene (its bus changed): faded out over
		// AudioGainSmoothingSeconds, moved and faded in when it is heard, moved at once when it is not.
		MoveVoice,
		// Hands Other, the voice's AudioSendNode, to the mixing thread (§9.8, reverb zones): it goes between the voice's
		// sound and its group, its dry output to the group and each send to the group of the scene the main thread set it to
		// before this command. It comes back with its voice.
		AddVoiceSends,
		// A new target for send Slot of the voice's send node: Other, an AudioSceneGroup of the voice's scene, at the linear
		// level Values[0], reached over AudioGainSmoothingSeconds while the voice is heard, at once otherwise. A send that
		// changes group while it is heard fades out first, then moves and fades in.
		SetVoiceSend,

		// The scenes' groups (Object: the AudioSceneGroup). Hands the group over and attaches it to Other, its AudioBusNode
		// (null only when Master's node could not be made: the engine's endpoint).
		AttachGroup,
		// Holds (Values[0] 1, a paused scene) or lets continue (0) the voices of every group the mixing thread holds of the
		// scene Serial names (AudioSceneGroup::GetScene), all in the same block: a voice's sends play into other groups of
		// its scene, which must stop and start together with its own.
		SetScenePaused,
		// The group feeds Other from now on (null: the endpoint), faded out, moved and faded in: a bus added, removed or
		// restored under the name the group's sources ask for (§9.5).
		MoveGroup,
		// Detaches the group and returns it once no voice the mixing thread holds is attached to it (stolen and retired
		// voices that are still fading out come back first).
		RetireGroup,

		// Buses (Object: the AudioBusNode). Hands a new bus over, with the gains and the first chain the main thread gave it
		// (AudioBusNode::SetInitialValues), and attaches it to Other, its parent bus (null for Master, which feeds the
		// engine's endpoint).
		AddBus,
		// The bus feeds Other from now on (faded out, moved, faded in).
		SetBusParent,
		// The bus goes: what still feeds it (the groups of retired scenes, which the main thread names no more, and anything
		// it did not move first) moves to Master with a dip, and once nothing feeds it the bus is detached and returned
		// with its chains.
		RemoveBus,
		// The mixer's value of the bus (the asset's, or a snapshot's in its place): VolumeDb in Values[0] and the mute in
		// Values[1] (1 muted), reached over AudioGainSmoothingSeconds, linearly in amplitude, when Values[2] (seconds) is at
		// most that; over Values[2] seconds otherwise, a snapshot's transition: the volume linearly in decibels, the mute
		// linearly in amplitude (§9.8).
		SetBusMixValues,
		// The game's value of the bus (scripts): VolumeDb and mute as above, reached over AudioGainSmoothingSeconds.
		SetBusGameValues,
		// Replaces the bus's effect chain with Other, an AudioEffectChain (faded out, swapped, faded in); the old chain is
		// returned.
		SetBusEffects,
		// A new target for parameter Parameter of effect Slot of the bus (Values[0]), reached on the parameter's scale (§9.6)
		// over AudioParameterSmoothingSeconds, or over Values[1] seconds when that is longer (a snapshot's transition).
		SetEffectParameter,
		// The bus's ducking (§9.8): Other is the AudioBusNode whose level lowers it (null: not ducked, and a duck under way
		// comes back up over the release); Values[0] the amount and Values[1] the threshold in dB, Values[2] the attack and
		// Values[3] the release in seconds. A RemoveBus of Other ends the ducking by it on the mixing thread, keeping the
		// follower's release.
		SetBusDucking,

		// The engine's master volume (Values[0], linear), applied to the mix after Master, ramped over
		// AudioGainSmoothingSeconds.
		SetMasterVolume,
	};

	// How a StartVoice starts its voice (AudioCommand::Flags).
	namespace AudioStartFlags
	{
		// Plays it: from its start after a stop or an end, from where it is after a pause.
		inline constexpr uint8_t Play = 0;
		// Starts it as a virtual voice (§9.3): silent and not mixed, while its time runs from where it is, as if it played.
		inline constexpr uint8_t Virtual = 1;
		// A virtual voice is heard again: it continues where its time has taken it, faded in over AudioStealFadeSeconds.
		inline constexpr uint8_t Resume = 2;
	}

	// One change for the mixing thread. Plain data: copied into the ring and out of it.
	struct AudioCommand
	{
		AudioCommandType Type = AudioCommandType::None;
		// The effect's index on its bus (SetEffectParameter), or the send's on its voice (SetVoiceSend).
		uint8_t Slot = 0;
		MixerEffectParameter Parameter = MixerEffectParameter::Frequency;
		// What the type reads from it (StartVoice: AudioStartFlags).
		uint8_t Flags = 0;
		// The object the command is about, and a second one where its type names one. The type says which classes they
		// are; the mixing thread holds them from the command that hands them over until it returns them.
		AudioMixerObject* Object = nullptr;
		AudioMixerObject* Other = nullptr;
		std::array<float, 4> Values = {};
		uint64_t Serial = 0;
	};

	static_assert(std::is_trivially_copyable_v<AudioCommand>, "Audio commands are copied through the ring");
	static_assert(sizeof(AudioCommand) == 48, "Audio commands keep the ring at 192 KiB");

	// A single-producer, single-consumer ring of commands, from the main thread to the mixing thread, with no lock and no
	// allocation on the mixing thread's side.
	//
	// Nothing pushed is ever dropped. A command that finds the ring full, or finds commands already deferred, waits in
	// order on the main thread's side (deferred, an allocation of the main thread) until Flush moves it into the ring:
	// every Push flushes first, AudioEngine::Update flushes once a frame, and with manual output AudioEngine::ReadFrames
	// flushes before each block, so a burst reaches the mix in order over the blocks that follow.
	class AudioCommandQueue
	{
	public:
		// Commands the ring holds (a power of two).
		static constexpr uint32_t Capacity = 4096;

		AudioCommandQueue() = default;

		AudioCommandQueue(AudioCommandQueue const&) = delete;
		AudioCommandQueue& operator=(AudioCommandQueue const&) = delete;

		// Main thread: appends the command, after every deferred one.
		void Push(AudioCommand const& command);
		// Main thread: moves deferred commands into the ring while it has room. Returns how many still wait.
		size_t Flush();
		// Main thread: commands waiting on the main thread's side.
		size_t GetDeferredCount() const { return m_Deferred.size(); }
		// Main thread: commands pushed since the queue began, the deferred ones included.
		uint64_t GetPushedCount() const { return m_Pushed; }

		// Mixing thread: calls apply(AudioCommand const&) for every command the ring held when it was called, in order,
		// and frees their places. Returns how many it applied.
		template<typename TApply>
		uint32_t Consume(TApply&& apply);

	private:
		static constexpr uint64_t IndexMask = Capacity - 1;
		static_assert((Capacity & IndexMask) == 0, "The ring's capacity is a power of two");

		// Main thread: whether the ring has room for one more command.
		bool HasRoom() const;

		std::array<AudioCommand, Capacity> m_Commands = {};
		// Commands written and read since the queue began (the ring's index is the count modulo Capacity). Written is
		// published by the main thread with release, Read by the mixing thread with release.
		std::atomic<uint64_t> m_Written{0};
		std::atomic<uint64_t> m_Read{0};
		// Main thread only.
		std::deque<AudioCommand> m_Deferred;
		uint64_t m_Pushed = 0;
	};

	template<typename TApply>
	uint32_t AudioCommandQueue::Consume(TApply&& apply)
	{
		// The main thread publishes Written after writing the commands below it, and reads Read before reusing a place.
		uint64_t const read = m_Read.load(std::memory_order_relaxed);
		uint64_t const written = m_Written.load(std::memory_order_acquire);
		for (uint64_t index = read; index != written; index++)
		{
			apply(m_Commands[index & IndexMask]);
		}
		m_Read.store(written, std::memory_order_release);
		return static_cast<uint32_t>(written - read);
	}

	// The objects the mixing thread hands back to the main thread: an intrusive, lock-free list (a stack the main thread
	// takes whole), so that handing back never allocates and never fills up.
	class AudioReturnList
	{
	public:
		AudioReturnList() = default;

		AudioReturnList(AudioReturnList const&) = delete;
		AudioReturnList& operator=(AudioReturnList const&) = delete;

		// The thread that mixes (one at a time): the object is the main thread's again once taken.
		void Push(AudioMixerObject& object);
		// Main thread: calls take(AudioMixerObject&) for every object pushed since the last call, oldest first.
		template<typename TTake>
		void TakeAll(TTake&& take);

	private:
		std::atomic<AudioMixerObject*> m_Head{nullptr};
	};

	template<typename TTake>
	void AudioReturnList::TakeAll(TTake&& take)
	{
		// The whole list at once: the pusher only ever adds at the head, so nothing taken here can be pushed again until
		// the main thread hands it over once more.
		AudioMixerObject* newest = m_Head.exchange(nullptr, std::memory_order_acquire);
		AudioMixerObject* oldest = nullptr;
		while (newest != nullptr)
		{
			AudioMixerObject* const next = newest->m_NextReturned;
			newest->m_NextReturned = oldest;
			oldest = newest;
			newest = next;
		}
		while (oldest != nullptr)
		{
			AudioMixerObject* const next = oldest->m_NextReturned;
			oldest->m_NextReturned = nullptr;
			take(*oldest);
			oldest = next;
		}
	}
}
