#pragma once

// Internal to the Audio module.

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
		// A voice that ended starts again from its beginning; a paused one continues.
		StartVoice,
		// Stops the voice where it is (Pause).
		PauseVoice,
		// Stops the voice and rewinds a loaded clip to its start (a stream is rewound by the main thread, §9.4).
		StopVoice,
		// Fades the voice out over AudioStealFadeSeconds, then stops it; a one-shot is then detached and returned.
		StealVoice,
		// Stops and detaches the voice and returns it.
		RetireVoice,
		// A new target for the voice's linear volume (Values[0]), reached over AudioGainSmoothingSeconds while it plays.
		SetVoiceVolume,
		// A new target for the voice's pitch (Values[0]), reached over AudioParameterSmoothingSeconds while it plays.
		SetVoicePitch,
		// Values[0] is 1 for looping, 0 for not.
		SetVoiceLooping,
		// Values[0] is 1 for spatial, 0 for not.
		SetVoiceSpatial,
		// The attenuation's minimum and maximum distances (Values[0], Values[1]), already clamped by the main thread.
		SetVoiceDistances,
		// The voice's position relative to its scene's listener (Values[0..2]), in the listener's basis.
		SetVoicePosition,
		// The voice moves to Other, another AudioSceneGroup of its scene (its bus changed): faded out, moved, faded in.
		MoveVoice,

		// The scenes' groups (Object: the AudioSceneGroup). Hands the group over and attaches it to Other, its AudioBusNode.
		AttachGroup,
		// Values[0] is 1 to hold the group's voices (a paused scene), 0 to let them continue.
		SetGroupPaused,
		// Detaches the group and returns it; its voices were retired before it.
		RetireGroup,

		// Buses (Object: the AudioBusNode). Hands a new bus over and attaches it to Other, its parent bus (null for Master,
		// which feeds the engine's endpoint).
		AddBus,
		// The bus feeds Other from now on (faded out, moved, faded in).
		SetBusParent,
		// Moves the bus's groups and child buses to Other (Master), then detaches the bus and returns it.
		RemoveBus,
		// The mixer's own value of the bus (the asset's, later blended by snapshots): VolumeDb in Values[0] and the mute in
		// Values[1] (1 muted), reached over AudioGainSmoothingSeconds.
		SetBusMixValues,
		// The game's value of the bus (scripts), the same way.
		SetBusGameValues,
		// Replaces the bus's effect chain with Other, an AudioEffectChain (faded out, swapped, faded in); the old chain is
		// returned.
		SetBusEffects,
		// A new target for parameter Parameter of effect Slot of the bus (Values[0]), reached over
		// AudioParameterSmoothingSeconds on the parameter's scale (§9.6).
		SetEffectParameter,

		// The engine's master volume (Values[0], linear), applied after Master's chain and volume.
		SetMasterVolume,
	};

	// One change for the mixing thread. Plain data: copied into the ring and out of it.
	struct AudioCommand
	{
		AudioCommandType Type = AudioCommandType::None;
		// The effect's index on its bus (SetEffectParameter).
		uint8_t Slot = 0;
		MixerEffectParameter Parameter = MixerEffectParameter::Frequency;
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
		size_t GetDeferredCount() const;

		// Mixing thread: calls apply(AudioCommand const&) for every command the ring held when it was called, in order,
		// and frees their places. Returns how many it applied.
		template<typename TApply>
		uint32_t Consume(TApply&& apply);

	private:
		std::array<AudioCommand, Capacity> m_Commands = {};
		// Commands written and read since the queue began (the ring's index is the count modulo Capacity). Written is
		// published by the main thread with release, Read by the mixing thread with release.
		std::atomic<uint64_t> m_Written{0};
		std::atomic<uint64_t> m_Read{0};
		// Main thread only.
		std::deque<AudioCommand> m_Deferred;
	};

	// The objects the mixing thread hands back to the main thread: an intrusive, lock-free list (a stack the main thread
	// takes whole), so that handing back never allocates and never fills up.
	class AudioReturnList
	{
	public:
		AudioReturnList() = default;

		AudioReturnList(AudioReturnList const&) = delete;
		AudioReturnList& operator=(AudioReturnList const&) = delete;

		// Mixing thread: the object is the main thread's again once taken.
		void Push(AudioMixerObject& object);
		// Main thread: calls take(AudioMixerObject&) for every object pushed since the last call, oldest first.
		template<typename TTake>
		void TakeAll(TTake&& take);

	private:
		std::atomic<AudioMixerObject*> m_Head{nullptr};
	};
}
