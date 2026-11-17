#pragma once

// Internal to the Audio module.

#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEffects.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioQueue.h"
#include "Strada/Audio/AudioVoice.h"
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

	// The link of an object in one of the mixing thread's intrusive lists (AudioMixerList).
	template<typename TObject>
	struct AudioMixerLink
	{
		TObject* Previous = nullptr;
		TObject* Next = nullptr;
		bool Linked = false;
	};

	// An intrusive list of the mixing thread's objects through one of their links, so that it keeps its lists of groups
	// and buses without allocating.
	template<typename TObject, AudioMixerLink<TObject> TObject::* TLink>
	class AudioMixerList
	{
	public:
		TObject* GetFirst() const { return m_First; }
		static TObject* GetNext(TObject const& object) { return (object.*TLink).Next; }

		void Add(TObject& object)
		{
			AudioMixerLink<TObject>& link = object.*TLink;
			if (link.Linked)
			{
				return;
			}
			link.Previous = nullptr;
			link.Next = m_First;
			if (m_First != nullptr)
			{
				(m_First->*TLink).Previous = &object;
			}
			m_First = &object;
			link.Linked = true;
		}

		void Remove(TObject& object)
		{
			AudioMixerLink<TObject>& link = object.*TLink;
			if (!link.Linked)
			{
				return;
			}
			if (link.Previous != nullptr)
			{
				(link.Previous->*TLink).Next = link.Next;
			}
			else
			{
				m_First = link.Next;
			}
			if (link.Next != nullptr)
			{
				(link.Next->*TLink).Previous = link.Previous;
			}
			link = {};
		}

	private:
		TObject* m_First = nullptr;
	};

	// A gain on its way to a target, ramped linearly per frame on the mixing thread.
	struct AudioSmoothedGain
	{
		float Current = 1.0f;
		float Target = 1.0f;
		float Step = 0.0f;
		uint32_t FramesLeft = 0;

		// A new target, reached over frames (at once with 0).
		void SetTarget(float target, uint32_t frames);
		// The gain of the next frame.
		float Next()
		{
			if (FramesLeft == 0)
			{
				return Current;
			}
			Current = --FramesLeft == 0 ? Target : Current + Step;
			return Current;
		}
		bool IsSteady() const { return FramesLeft == 0; }
	};

	// One scene's group of one bus: every voice of that scene on that bus plays through it, and stopping it holds them
	// (a paused scene). An ma_sound_group made with MA_SOUND_FLAG_NO_PITCH and MA_SOUND_FLAG_NO_SPATIALIZATION (it only
	// sums) and MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT (the mixing thread attaches it). Made by its AudioScene on the main
	// thread for the bus name its sources ask for (the key, letters folded to lower case), handed over with its first
	// voice's start, and kept until the scene ends. While it lives (until its scene retires it) the engine knows it
	// (AudioVoiceManager::GetGroups), so that a mixer edit points it at the bus its key names (PointAt).
	class AudioSceneGroup final : public AudioMixerObject
	{
		struct PrivateTag
		{
		};

	public:
		// Main thread: the group, unattached, holding its voices when paused, for the bus of that name (matched by its key,
		// MakeMixerBusKey; Master when the mixer lacks it, which is logged once with the name as given). Its node is
		// allocated through the engine's allocation callbacks.
		[[nodiscard]] static Result<Scope<AudioSceneGroup>> Create(std::string busName, bool paused);

		AudioSceneGroup(PrivateTag, std::string busName);
		// Main thread, only while the mixing thread does not hold the group.
		~AudioSceneGroup() override;

		// Main thread.
		std::string const& GetBusKey() const { return m_BusKey; }
		// The bus's name as the source that made the group gave it (another spelling of the key may share the group).
		std::string const& GetBusName() const { return m_BusName; }
		bool IsPaused() const { return m_Paused; }
		bool IsHeldByMixer() const { return m_HeldByMixer; }
		// The bus it plays into, as the main thread last pointed it (null: the engine's endpoint).
		AudioBusNode* GetBusNode() const { return m_BusNode; }
		// Hands the group to the mixing thread (before the first start of a voice in it); nothing when it already holds it.
		void Attach();
		void SetPaused(bool paused);
		// The group plays into bus from now on: moved with a dip (MoveGroup) once the mixing thread holds it.
		void PointAt(AudioBusNode* bus);
		// The last command that names the group: it comes back once its voices have. The engine forgets it.
		void Retire();

	private:
		friend class AudioMixer;

		ma_sound_group m_Group{};
		bool m_HasGroup = false;
		std::string m_BusKey;
		std::string m_BusName;
		// Main thread.
		bool m_Paused = false;
		bool m_HeldByMixer = false;
		bool m_Registered = false;
		AudioBusNode* m_BusNode = nullptr;
		// Mixing thread only: whether it is attached, held, retiring, and how many voices it holds attached.
		bool m_MixAttached = false;
		bool m_MixPaused = false;
		bool m_MixRetiring = false;
		uint32_t m_MixVoiceCount = 0;
		// The bus it is attached to (null: the endpoint), and a move under way: faded out until m_MixMoveEnd on the mixer's
		// clock, then attached to m_MixMoveTarget and faded back in.
		AudioBusNode* m_Bus = nullptr;
		AudioBusNode* m_MixMoveTarget = nullptr;
		bool m_MixMoving = false;
		uint64_t m_MixMoveEnd = 0;
		AudioMixerLink<AudioSceneGroup> m_HeldLink;
	};

	// A bus's effects, in order, handed to the mixing thread whole (SetBusEffects) and returned whole when replaced. The
	// effects are shared, on the main thread's side, with the chain that replaces this one wherever an effect carries on.
	class AudioEffectChain final : public AudioMixerObject
	{
	public:
		// Main thread: at most MaxBusEffects effects (more are left out).
		explicit AudioEffectChain(std::vector<Ref<AudioEffect>> effects);

		// Mixing thread: the effects, in the order they run.
		std::span<AudioEffect* const> GetEffects() const { return std::span<AudioEffect* const>(m_Effects.data(), m_Count); }
		// Main thread: the same effects, by reference (what a new chain carries on).
		std::vector<Ref<AudioEffect>> const& GetOwnedEffects() const { return m_Owned; }

	private:
		// Main thread: the references that keep the effects alive.
		std::vector<Ref<AudioEffect>> m_Owned;
		// What the mixing thread reads (the same effects, without touching a reference count there).
		std::array<AudioEffect*, MaxBusEffects> m_Effects = {};
		uint32_t m_Count = 0;
	};

	// One bus of the mixer: a custom node of miniaudio's graph with one input bus, into which miniaudio sums the bus's
	// scene groups and child buses, and one output to its parent's node (Master's to the engine's endpoint). Made with
	// MA_NODE_FLAG_CONTINUOUS_PROCESSING (miniaudio.h:75293-75325 skip a node whose input is silent otherwise; without
	// MA_NODE_FLAG_ALLOW_NULL_INPUT it is given silence, which keeps new input from coming a block late, AudioMixer.cpp),
	// so that every bus runs in every block: a reverb's or a delay's tail rings on after the bus's
	// last voice ends or its scene pauses, the dynamics' state and the ramps move on, and the meters fall to silence. Its
	// process function runs the bus's chain in order, then the mixer's gain (the asset's volume and mute, blended by MX2's
	// snapshots), the game's gain (scripts), MX2's ducking and the dip of a move or a swap, each ramped; then it measures
	// the block's peak and RMS, silence included. One node per bus name: a mixer edit that keeps a bus keeps its node.
	class AudioBusNode final : public AudioMixerObject
	{
		struct PrivateTag
		{
		};

	public:
		// Main thread: the bus's node, unattached and without effects, for a mix of channelCount channels. Its node is
		// allocated through the engine's allocation callbacks.
		[[nodiscard]] static Result<Scope<AudioBusNode>> Create(std::string name, uint32_t channelCount);

		AudioBusNode(PrivateTag, std::string name, uint32_t channelCount);
		// Main thread, only while the mixing thread does not hold the node.
		~AudioBusNode() override;

		std::string const& GetName() const { return m_Name; }

		// Main thread, before AddBus hands the node over: its first gains (linear) and effects, taken at once.
		void SetInitialValues(float mixGain, float gameGain, AudioEffectChain* chain);
		// Main thread: the product of the mixer's and the game's gains of this bus and every bus above it, in amplitude, as
		// the main thread last set them (AudioVoiceManager's estimate of a voice's loudness, §9.3).
		float GetChainGain() const { return m_ChainGain; }
		void SetChainGain(float gain) { m_ChainGain = gain; }

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

		// ma_node_vtable::onProcess (miniaudio.h:10680), on the mixing thread: the input (silence when nothing feeds the
		// bus), the effects in order, the gains, then the meters.
		static void OnProcess(ma_node* node, float const** framesIn, ma_uint32* frameCountIn, float** framesOut, ma_uint32* frameCountOut);
		void Process(float const* input, uint32_t inputFrames, float* output, uint32_t frameCount);

		Node m_Node{};
		bool m_HasNode = false;
		std::string m_Name;
		uint32_t m_ChannelCount = 0;
		// Main thread.
		float m_ChainGain = 1.0f;

		// Mixing thread only.
		AudioEffectChain* m_Chain = nullptr;
		// The parent and the chain waiting for the bottom of the dip.
		AudioEffectChain* m_PendingChain = nullptr;
		bool m_HasPendingChain = false;
		AudioBusNode* m_PendingParent = nullptr;
		bool m_HasPendingParent = false;
		// The bus it feeds (null: the engine's endpoint, Master's place).
		AudioBusNode* m_Parent = nullptr;
		AudioSmoothedGain m_MixGain;
		AudioSmoothedGain m_GameGain;
		// The dip of a move or a swap: down to 0 until m_DipEnd on the mixer's clock, the change, back to 1.
		AudioSmoothedGain m_Dip;
		bool m_Dipping = false;
		uint64_t m_DipEnd = 0;
		// Groups and buses attached to its input; a removed bus comes back once none is.
		uint32_t m_MixInputCount = 0;
		bool m_MixRemoving = false;
		AudioMixerLink<AudioBusNode> m_HeldLink;

		// Written by the mixing thread at the end of each block.
		std::atomic<float> m_Peak{0.0f};
		std::atomic<float> m_Rms{0.0f};
	};

	// An intrusive list of voices through one of their links (AudioVoice::m_PlayingLink, m_FadingLink, m_VirtualLink), so
	// that the mixing thread keeps its lists without allocating.
	template<AudioVoiceLink AudioVoice::* TLink>
	class AudioVoiceList
	{
	public:
		AudioVoice* GetFirst() const { return m_First; }
		static AudioVoice* GetNext(AudioVoice const& voice) { return (voice.*TLink).Next; }
		static bool Contains(AudioVoice const& voice) { return (voice.*TLink).Linked; }
		uint32_t GetCount() const { return m_Count; }

		void Add(AudioVoice& voice)
		{
			AudioVoiceLink& link = voice.*TLink;
			if (link.Linked)
			{
				return;
			}
			link.Previous = nullptr;
			link.Next = m_First;
			if (m_First != nullptr)
			{
				(m_First->*TLink).Previous = &voice;
			}
			m_First = &voice;
			link.Linked = true;
			m_Count++;
		}

		void Remove(AudioVoice& voice)
		{
			AudioVoiceLink& link = voice.*TLink;
			if (!link.Linked)
			{
				return;
			}
			if (link.Previous != nullptr)
			{
				(link.Previous->*TLink).Next = link.Next;
			}
			else
			{
				m_First = link.Next;
			}
			if (link.Next != nullptr)
			{
				(link.Next->*TLink).Previous = link.Previous;
			}
			link = {};
			m_Count--;
		}

	private:
		AudioVoice* m_First = nullptr;
		uint32_t m_Count = 0;
	};

	// The mixing thread's side of the engine. Render is called by the device's data callback (MixIntoDevice) on miniaudio's
	// device thread, or by AudioEngine::ReadFrames on the calling thread with manual output, so tests mix exactly as a
	// device does; one thread at a time. Each block it applies every command the ring holds, steps the pitches that move,
	// renders the block through ma_engine_read_pcm_frames, applies the master volume, then finishes the voices that ended,
	// faded or passed their end while virtual, and pushes what it lets go onto the return list. It never allocates, locks,
	// waits or logs: miniaudio's graph reads without locking (miniaudio.h:74198-74202), and the graph's attach and detach,
	// which take spinlocks (:74017, :74086) and wait for readers (:74051-74059), are only ever called by this thread
	// between blocks, so no lock is contended and no reader is waited for. Each block records its thread in the engine's
	// static mixing-thread identity for its length (AudioEngine::IsMixingOnThisThread).
	class AudioMixer
	{
	public:
		AudioMixer(ma_engine& engine, AudioCommandQueue& queue, AudioReturnList& returns);

		AudioMixer(AudioMixer const&) = delete;
		AudioMixer& operator=(AudioMixer const&) = delete;

		// The thread that mixes: frameCount frames of interleaved floats (the engine's channels) into output, in blocks of
		// at most AudioBlockFrames.
		void Render(float* output, uint32_t frameCount);
		// The main thread, while it has taken the mixer over from a device that stopped (§9.1) and no thread mixes: applies
		// the queue and finishes every fade at once, rendering nothing.
		void Drain();

		// Any thread: blocks rendered since the mixer was made (the main thread's stall check reads it).
		uint64_t GetBlockCount() const { return m_BlockCount.load(std::memory_order_acquire); }
		// Any thread: voices playing in the mix at the end of the last block (stolen ones still fading out included; a paused
		// scene's voices, which its stopped group does not pull, and virtual voices left out).
		uint32_t GetMixedVoiceCount() const { return m_MixedVoices.load(std::memory_order_relaxed); }

	private:
		void RenderBlock(float* output, uint32_t frameCount);
		void Apply(AudioCommand const& command);
		void ApplyStart(AudioVoice& voice, AudioCommand const& command);
		void StepPitches(uint32_t frameCount);
		void FinishBlock();
		// The voice's fade ended (or is cut short): what its kind says happens now, and a move waiting for it.
		void FinishFade(AudioVoice& voice);

		// The buses' and the groups' commands (MoveGroup, AddBus, SetBusParent, RemoveBus, the values, the effects).
		void ApplyBusCommand(AudioCommand const& command);
		// A dip of the bus begins now, unless one is under way: at its bottom the bus moves to its pending parent and swaps
		// in its pending chain.
		void BeginDip(AudioBusNode& bus);
		void SetBusParent(AudioBusNode& bus, AudioBusNode* parent);
		void MoveGroup(AudioSceneGroup& group, AudioBusNode* bus);
		// Attaches the group's or the bus's output to bus (null: the engine's endpoint), and detaches it.
		void ConnectGroup(AudioSceneGroup& group, AudioBusNode* bus);
		void DisconnectGroup(AudioSceneGroup& group);
		void ConnectBus(AudioBusNode& bus, AudioBusNode* parent);
		void DisconnectBus(AudioBusNode& bus);
		// What the dips and moves that ended in this block do, and the removed buses nothing feeds any more.
		void FinishBuses();
		void ReturnBus(AudioBusNode& bus);
		ma_node* GetInputNode(AudioBusNode* bus);

		void AttachVoice(AudioVoice& voice, AudioSceneGroup& group);
		void DetachVoice(AudioVoice& voice);
		// The last touch of a voice or a group: the main thread may destroy it as soon as it is on the return list.
		void ReturnVoice(AudioVoice& voice);
		void ReturnGroup(AudioSceneGroup& group);
		// Fades the voice out over frames from the fader's current volume; FinishBlock finishes the fade at its end.
		void BeginFade(AudioVoice& voice, AudioVoice::Fade fade, uint64_t frames);
		// Calls a fade off; resetFader puts the voice's fader back at full volume at once (a voice that stops).
		void CancelFade(AudioVoice& voice, bool resetFader);
		// The play reached its end: reported to the main thread through the voice's ended serial.
		void EndPlay(AudioVoice& voice);
		void BecomeVirtual(AudioVoice& voice);
		// The clip frames the voice's virtual time has played since it became virtual (or last caught up).
		uint64_t GetVirtualFrames(AudioVoice const& voice) const;
		// Moves a virtual voice's cursor to where its time has taken it; reports its end when that passed the end of a clip
		// that does not loop. Returns whether the voice can still play.
		bool CatchUpVirtual(AudioVoice& voice);
		// Playing in the mix, in a group that is not paused.
		bool IsHeard(AudioVoice const& voice) const;

		ma_engine& m_Engine;
		AudioCommandQueue& m_Queue;
		AudioReturnList& m_Returns;
		uint32_t m_ChannelCount = 0;
		uint32_t m_SampleRate = 0;
		AudioVoiceList<&AudioVoice::m_PlayingLink> m_Playing;
		AudioVoiceList<&AudioVoice::m_FadingLink> m_Fading;
		AudioVoiceList<&AudioVoice::m_VirtualLink> m_Virtual;
		// The groups and the buses the mixing thread holds, and Master's bus (the one added without a parent), where groups
		// and buses go when the bus they play into is removed.
		AudioMixerList<AudioSceneGroup, &AudioSceneGroup::m_HeldLink> m_Groups;
		AudioMixerList<AudioBusNode, &AudioBusNode::m_HeldLink> m_Buses;
		AudioBusNode* m_Master = nullptr;
		AudioSmoothedGain m_MasterVolume;
		// Frames rendered since the mixer was made: the clock of fades and virtual voices (miniaudio's own stops while
		// nothing is attached to its graph).
		uint64_t m_Time = 0;
		std::atomic<uint64_t> m_BlockCount{0};
		std::atomic<uint32_t> m_MixedVoices{0};
	};
}
