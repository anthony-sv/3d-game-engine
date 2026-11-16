#pragma once

// Internal to the Audio module.

#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEffects.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioQueue.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <miniaudio.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// The mixing thread's side of the engine (Docs/Architecture.md §9.1, §9.2 and §9.5): the blocks, the scenes' groups, the
// buses and the code that applies the queue.
namespace Strada
{
	class AudioBusNode;
	class AudioVoice;

	// A gain on its way to a target, ramped linearly per frame on the mixing thread.
	struct AudioSmoothedGain
	{
		float Current = 1.0f;
		float Target = 1.0f;
		float Step = 0.0f;
		uint32_t FramesLeft = 0;
	};

	// One scene's group of one bus: every voice of that scene on that bus plays through it, and stopping it holds them
	// (a paused scene). An ma_sound_group made with MA_SOUND_FLAG_NO_PITCH and MA_SOUND_FLAG_NO_SPATIALIZATION (it only
	// sums) and MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT (the mixing thread attaches it). Made by its AudioScene on the main
	// thread when the scene's first voice on the bus starts, and kept until the scene ends.
	class AudioSceneGroup final : public AudioMixerObject
	{
		struct PrivateTag
		{
		};

	public:
		// Main thread: the group, unattached. Its node is allocated through the engine's allocation callbacks.
		[[nodiscard]] static Result<Scope<AudioSceneGroup>> Create();

		explicit AudioSceneGroup(PrivateTag);
		// Main thread, only while the mixing thread does not hold the group.
		~AudioSceneGroup() override;

	private:
		friend class AudioMixer;

		ma_sound_group m_Group{};
		bool m_HasGroup = false;
		// Mixing thread only: the bus the group feeds, and the next group feeding it (an intrusive list, so that moving a
		// removed bus's groups to Master allocates nothing).
		AudioBusNode* m_Bus = nullptr;
		AudioSceneGroup* m_NextOfBus = nullptr;
	};

	// A bus's effects, in order, handed to the mixing thread whole (SetBusEffects) and returned whole when replaced. The
	// effects are shared, on the main thread's side, with the chain that replaces this one wherever an effect carries on.
	class AudioEffectChain final : public AudioMixerObject
	{
	public:
		// Main thread: at most MaxBusEffects effects.
		explicit AudioEffectChain(std::vector<Ref<AudioEffect>> effects);

		// Mixing thread: the effects, in the order they run.
		std::span<AudioEffect* const> GetEffects() const { return std::span<AudioEffect* const>(m_Effects.data(), m_Count); }

	private:
		// Main thread: the references that keep the effects alive.
		std::vector<Ref<AudioEffect>> m_Owned;
		// What the mixing thread reads (the same effects, without touching a reference count there).
		std::array<AudioEffect*, MaxBusEffects> m_Effects = {};
		uint32_t m_Count = 0;
	};

	// One bus of the mixer: a custom node of miniaudio's graph with one input bus, into which miniaudio sums the bus's
	// scene groups and child buses, and one output to its parent's node (Master's to the engine's endpoint). Its process
	// function runs the bus's chain in order, then the mixer's gain (the asset's volume and mute, blended by MX2's
	// snapshots), the game's gain (scripts), MX2's ducking and, on Master, the master volume, each ramped; then it measures
	// the block's peak and RMS. One node per bus name: a mixer edit that keeps a bus keeps its node and its groups.
	class AudioBusNode final : public AudioMixerObject
	{
		struct PrivateTag
		{
		};

	public:
		// Main thread: the bus's node, unattached and without effects, for a mix of channelCount channels. Its node is
		// allocated through the engine's allocation callbacks.
		[[nodiscard]] static Result<Scope<AudioBusNode>> Create(std::string name, uint32_t channelCount);

		AudioBusNode(PrivateTag, std::string name);
		// Main thread, only while the mixing thread does not hold the node.
		~AudioBusNode() override;

		std::string const& GetName() const { return m_Name; }

		// Any thread: the linear peak and RMS of the bus's output over the last block the mixing thread finished (MX2's
		// meters; MX2's ducking reads them a block late, since miniaudio's graph pulls buses in an order of its own).
		float GetPeak() const { return m_Peak.load(std::memory_order_relaxed); }
		float GetRms() const { return m_Rms.load(std::memory_order_relaxed); }

	private:
		friend class AudioMixer;

		// miniaudio's custom node: its base first, as miniaudio requires.
		struct Node
		{
			ma_node_base Base;
			AudioBusNode* Owner = nullptr;
		};

		// ma_node_vtable::onProcess (miniaudio.h:10680), on the mixing thread.
		static void OnProcess(ma_node* node, float const** framesIn, ma_uint32* frameCountIn, float** framesOut, ma_uint32* frameCountOut);

		Node m_Node{};
		bool m_HasNode = false;
		std::string m_Name;

		// Mixing thread only.
		AudioEffectChain* m_Chain = nullptr;
		// A chain waiting for the bottom of the dip to be swapped in.
		AudioEffectChain* m_PendingChain = nullptr;
		AudioBusNode* m_Parent = nullptr;
		// The head of the groups feeding the bus.
		AudioSceneGroup* m_Groups = nullptr;
		AudioSmoothedGain m_MixGain;
		AudioSmoothedGain m_GameGain;
		// The dip of a move or a swap: down to 0, the change, back to 1.
		AudioSmoothedGain m_Dip;

		// Written by the mixing thread at the end of each block.
		std::atomic<float> m_Peak{0.0f};
		std::atomic<float> m_Rms{0.0f};
	};

	// The mixing thread's side of the engine. Render is called by the device's data callback (MixIntoDevice) on miniaudio's
	// device thread, or by AudioEngine::ReadFrames on the calling thread with manual output, so tests mix exactly as a
	// device does; one thread at a time. Each block it applies every command the ring holds, renders the block through
	// ma_engine_read_pcm_frames, then finishes the voices that ended or faded and pushes what it lets go onto the return
	// list. It never allocates, locks, waits or logs: miniaudio's graph reads without locking (miniaudio.h:74198-74202), and
	// the graph's attach and detach, which take spinlocks (:74017, :74086) and wait for readers (:74051-74059), are only ever
	// called by this thread between blocks, so no lock is contended and no reader is waited for. Render records its thread
	// in the engine's static mixing-thread identity for its length (AudioEngine::IsMixingOnThisThread).
	class AudioMixer
	{
	public:
		AudioMixer(ma_engine& engine, AudioCommandQueue& queue, AudioReturnList& returns);

		AudioMixer(AudioMixer const&) = delete;
		AudioMixer& operator=(AudioMixer const&) = delete;

		// The thread that mixes: frameCount frames of interleaved floats (the engine's channels) into output, in blocks of
		// at most AudioBlockFrames.
		void Render(float* output, uint32_t frameCount);

		// Any thread: blocks rendered since the mixer was made (the main thread's stall check reads it).
		uint64_t GetBlockCount() const { return m_BlockCount.load(std::memory_order_acquire); }

	private:
		void Apply(AudioCommand const& command);
		void FinishBlock(uint32_t frameCount);
		// ma_sound_end_proc (miniaudio.h:11209): a voice's play reached its end, on the mixing thread inside the graph's
		// read; the voice joins m_Ended and is finished after the block.
		static void OnVoiceEnded(void* userData, ma_sound* sound);

		ma_engine& m_Engine;
		AudioCommandQueue& m_Queue;
		AudioReturnList& m_Returns;
		// Voices whose play ended in this block, and voices fading out (stolen or moving), as intrusive lists of the voices.
		AudioVoice* m_Ended = nullptr;
		AudioVoice* m_Fading = nullptr;
		AudioBusNode* m_Master = nullptr;
		AudioSmoothedGain m_MasterVolume;
		std::atomic<uint64_t> m_BlockCount{0};
	};
}
