#include "stpch.h"
#include "Strada/Audio/AudioMixer.h"

#include "Strada/Audio/MiniaudioContext.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		uint32_t SecondsToFrames(float seconds, uint32_t sampleRate)
		{
			return static_cast<uint32_t>(std::lround(seconds * static_cast<float>(sampleRate)));
		}
	}

	void AudioSmoothedGain::SetTarget(float target, uint32_t frames)
	{
		Target = target;
		if (frames == 0)
		{
			Current = target;
			Step = 0.0f;
			FramesLeft = 0;
			return;
		}
		Step = (target - Current) / static_cast<float>(frames);
		FramesLeft = frames;
	}

	namespace
	{
		// A bus volume as an amplitude: MinMixerVolumeDb and below are silence, exactly.
		double ToVolumeGain(float decibels)
		{
			return decibels <= MinMixerVolumeDb ? 0.0 : std::pow(10.0, static_cast<double>(decibels) / 20.0);
		}
	}

	void AudioBusMixGain::SetTarget(float volumeDb, bool muted, uint32_t frames, bool inDecibels)
	{
		// A value whose target does not change keeps its ramp: an edit of a bus's mute during a transition of its volume
		// leaves the transition alone, and the other way round.
		float const muteTarget = muted ? 0.0f : 1.0f;
		if (frames == 0 || muteTarget != Mute.Target)
		{
			Mute.SetTarget(muteTarget, frames);
		}
		double const volumeTarget = ToVolumeGain(volumeDb);
		if (frames != 0 && volumeTarget == VolumeTarget)
		{
			return;
		}
		VolumeTarget = volumeTarget;
		InDecibels = inDecibels;
		if (frames == 0 || Volume == VolumeTarget)
		{
			Volume = VolumeTarget;
			VolumeStep = 0.0;
			FramesLeft = 0;
			return;
		}
		FramesLeft = frames;
		if (!inDecibels)
		{
			VolumeStep = (VolumeTarget - Volume) / static_cast<double>(frames);
			return;
		}
		// From silence, a move in decibels starts at the floor; towards silence it heads for the floor and is silence at its
		// last frame.
		Volume = std::max(Volume, DecibelFloor);
		VolumeStep = std::pow(std::max(VolumeTarget, DecibelFloor) / Volume, 1.0 / static_cast<double>(frames));
	}

	float AudioBusMixGain::Next()
	{
		if (FramesLeft > 0)
		{
			if (--FramesLeft == 0)
			{
				Volume = VolumeTarget;
			}
			else
			{
				Volume = InDecibels ? Volume * VolumeStep : Volume + VolumeStep;
			}
		}
		return static_cast<float>(Volume) * Mute.Next();
	}

	Result<Scope<AudioSceneGroup>> AudioSceneGroup::Create(std::string busName, bool paused, uint64_t scene)
	{
		ma_engine& engine = Miniaudio::GetEngine();
		Scope<AudioSceneGroup> group = CreateScope<AudioSceneGroup>(PrivateTag{}, std::move(busName), scene);
		ma_sound_group_config config = ma_sound_group_config_init_2(&engine);
		// The group only mixes: without pitch and spatialization it adds no resampling delay and no second panning.
		config.flags = MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
		if (ma_result const result = ma_sound_group_init_ex(&engine, &config, &group->m_Group); result != MA_SUCCESS)
		{
			return MakeError("the scene's sound group cannot be created: {}", ma_result_description(result));
		}
		group->m_HasGroup = true;
		if (paused)
		{
			// Not handed over yet: the main thread still sets it up itself.
			ma_sound_group_stop(&group->m_Group);
			group->m_Paused = true;
			group->m_MixPaused = true;
		}
		group->m_BusNode = Miniaudio::FindBus(group->m_BusKey, group->m_BusName);
		Miniaudio::GetVoiceManager().AddGroup(*group);
		group->m_Registered = true;
		return group;
	}

	AudioSceneGroup::AudioSceneGroup(PrivateTag, std::string busName, uint64_t scene)
		: m_BusKey(MakeMixerBusKey(busName)),
		  m_BusName(std::move(busName)),
		  m_Scene(scene)
	{
	}

	AudioSceneGroup::~AudioSceneGroup()
	{
		if (m_Registered)
		{
			Miniaudio::GetVoiceManager().RemoveGroup(*this);
		}
		if (m_HasGroup)
		{
			ma_sound_group_uninit(&m_Group);
		}
	}

	void AudioSceneGroup::Attach()
	{
		if (m_HeldByMixer)
		{
			return;
		}
		m_HeldByMixer = true;
		AudioCommand command;
		command.Type = AudioCommandType::AttachGroup;
		command.Object = this;
		command.Other = m_BusNode;
		Miniaudio::GetVoiceManager().Submit(command);
	}

	void AudioSceneGroup::PointAt(AudioBusNode* bus)
	{
		if (bus == m_BusNode)
		{
			return;
		}
		m_BusNode = bus;
		if (m_HeldByMixer)
		{
			AudioCommand command;
			command.Type = AudioCommandType::MoveGroup;
			command.Object = this;
			command.Other = bus;
			Miniaudio::GetVoiceManager().Submit(command);
		}
	}

	bool AudioSceneGroup::SetPaused(bool paused)
	{
		m_Paused = paused;
		if (m_HeldByMixer)
		{
			return true;
		}
		m_MixPaused = paused;
		if (paused)
		{
			ma_sound_group_stop(&m_Group);
		}
		else
		{
			ma_sound_group_start(&m_Group);
		}
		return false;
	}

	void AudioSceneGroup::Retire()
	{
		if (m_Registered)
		{
			Miniaudio::GetVoiceManager().RemoveGroup(*this);
			m_Registered = false;
		}
		if (m_HeldByMixer)
		{
			AudioCommand command;
			command.Type = AudioCommandType::RetireGroup;
			command.Object = this;
			Miniaudio::GetVoiceManager().Submit(command);
		}
	}

	AudioEffectChain::AudioEffectChain(std::vector<Ref<AudioEffect>> effects)
		: m_Owned(std::move(effects))
	{
		if (m_Owned.size() > MaxBusEffects)
		{
			m_Owned.resize(MaxBusEffects);
		}
		for (Ref<AudioEffect> const& effect : m_Owned)
		{
			m_Effects[m_Count++] = effect.get();
		}
	}

	Result<Scope<AudioBusNode>> AudioBusNode::Create(std::string name, uint32_t channelCount)
	{
		// The bus node's kind for miniaudio: one input, into which miniaudio sums what feeds the bus, and one output. It
		// processes in every block, also without input, so that tails, ramps and meters move on (Docs/Architecture.md
		// §9.5); miniaudio then passes it silence. Not MA_NODE_FLAG_ALLOW_NULL_INPUT: after a block processed without
		// input miniaudio leaves that block's frames counted as cached input (the null-input path skips the counters,
		// miniaudio.h:75361-75392), so the next block reads its input from the cache instead of the graph, and a sound
		// that starts on a silent bus would be heard a block late.
		static ma_node_vtable const s_Vtable = {
			.onProcess = &AudioBusNode::OnProcess,
			.onGetRequiredInputFrameCount = nullptr,
			.inputBusCount = 1,
			.outputBusCount = 1,
			.flags = MA_NODE_FLAG_CONTINUOUS_PROCESSING,
		};
		ma_engine& engine = Miniaudio::GetEngine();
		Scope<AudioBusNode> bus = CreateScope<AudioBusNode>(PrivateTag{}, std::move(name), channelCount);
		ma_uint32 const channels[1] = {channelCount};
		ma_node_config config = ma_node_config_init();
		config.vtable = &s_Vtable;
		config.pInputChannels = channels;
		config.pOutputChannels = channels;
		bus->m_Node.Owner = bus.get();
		if (ma_result const result =
		        ma_node_init(ma_engine_get_node_graph(&engine), &config, &Miniaudio::GetAllocationCallbacks(), &bus->m_Node.Base);
		    result != MA_SUCCESS)
		{
			return MakeError("the mixer's bus '{}' cannot be created: {}", bus->m_Name, ma_result_description(result));
		}
		bus->m_HasNode = true;
		return bus;
	}

	AudioBusNode::AudioBusNode(PrivateTag, std::string name, uint32_t channelCount)
		: m_Name(std::move(name)),
		  m_ChannelCount(channelCount)
	{
	}

	AudioBusNode::~AudioBusNode()
	{
		if (m_HasNode)
		{
			// Detaches its output and whatever still feeds it (ma_node_detach_full), waiting for no reader: no thread mixes.
			ma_node_uninit(&m_Node.Base, &Miniaudio::GetAllocationCallbacks());
		}
	}

	void AudioBusNode::SetInitialValues(float volumeDb, bool muted, float gameGain, AudioEffectChain* chain)
	{
		m_MixGain.SetTarget(volumeDb, muted, 0, false);
		m_GameGain.SetTarget(gameGain, 0);
		m_Chain = chain;
	}

	void AudioBusNode::OnProcess(ma_node* node, float const** framesIn, ma_uint32* frameCountIn, float** framesOut,
	                             ma_uint32* frameCountOut)
	{
		AudioBusNode& bus = *static_cast<Node*>(node)->Owner;
		// miniaudio passes silence without input; a null input is taken for silence too, consuming nothing.
		float const* const input = framesIn != nullptr ? framesIn[0] : nullptr;
		uint32_t const inputFrames = input != nullptr ? std::min(*frameCountIn, *frameCountOut) : 0;
		uint32_t const frames = input != nullptr ? inputFrames : *frameCountOut;
		bus.Process(input, inputFrames, framesOut[0], frames);
		if (input != nullptr)
		{
			*frameCountIn = inputFrames;
		}
		*frameCountOut = frames;
	}

	void AudioBusNode::Process(float const* input, uint32_t inputFrames, float* output, uint32_t frameCount)
	{
		size_t const sampleCount = static_cast<size_t>(frameCount) * m_ChannelCount;
		size_t const inputSamples = static_cast<size_t>(inputFrames) * m_ChannelCount;
		if (input != nullptr && input != output)
		{
			std::copy_n(input, inputSamples, output);
		}
		std::fill(output + (input != nullptr ? inputSamples : 0), output + sampleCount, 0.0f);
		std::span<float> const samples(output, sampleCount);
		if (m_Chain != nullptr)
		{
			for (AudioEffect* const effect : m_Chain->GetEffects())
			{
				effect->Process(samples);
			}
		}
		// Ducking: the duck's gain heads for its target while the other bus's level of the block before is at or above the
		// threshold, and back for 1 below it.
		double const duckTarget =
			m_DuckTrigger != nullptr && m_DuckTrigger->m_PreviousRms >= m_DuckThreshold ? static_cast<double>(m_DuckTarget) : 1.0;
		// The mixer's gain, the game's, the duck's and the dip, ramped per frame. At 0 dB with nothing moving the bus is the
		// identity.
		bool const steady = m_MixGain.IsSteady() && m_GameGain.IsSteady() && m_Dip.IsSteady() && m_DuckGain == duckTarget;
		if (!steady || m_MixGain.GetCurrent() * m_GameGain.Current * m_Dip.Current * static_cast<float>(m_DuckGain) != 1.0f)
		{
			for (uint32_t frame = 0; frame < frameCount; frame++)
			{
				StepDuck(duckTarget);
				float const gain = m_MixGain.Next() * m_GameGain.Next() * m_Dip.Next() * static_cast<float>(m_DuckGain);
				float* const frameSamples = output + static_cast<size_t>(frame) * m_ChannelCount;
				for (uint32_t channel = 0; channel < m_ChannelCount; channel++)
				{
					frameSamples[channel] *= gain;
				}
			}
		}
		float peak = 0.0f;
		double squares = 0.0;
		for (float const sample : samples)
		{
			peak = std::max(peak, std::abs(sample));
			squares += static_cast<double>(sample) * sample;
		}
		m_BlockRms = sampleCount > 0 ? static_cast<float>(std::sqrt(squares / static_cast<double>(sampleCount))) : 0.0f;
		m_Peak.store(peak, std::memory_order_relaxed);
		m_Rms.store(m_BlockRms, std::memory_order_relaxed);
	}

	void AudioBusNode::StepDuck(double target)
	{
		if (m_DuckGain == target)
		{
			return;
		}
		// Linear in decibels: one factor a frame, from the floor when it starts from silence, and the target itself once
		// reached (silence when the amount is MinMixerVolumeDb).
		double const reach = std::max(target, AudioBusMixGain::DecibelFloor);
		if (m_DuckGain > target)
		{
			m_DuckGain *= m_DuckAttackFactor;
			if (m_DuckGain <= reach)
			{
				m_DuckGain = target;
			}
			return;
		}
		m_DuckGain = std::max(m_DuckGain, AudioBusMixGain::DecibelFloor) * m_DuckReleaseFactor;
		if (m_DuckGain >= target)
		{
			m_DuckGain = target;
		}
	}

	void AudioBusNode::SetDucking(AudioBusNode* trigger, float amountDb, float thresholdDb, float attackSeconds, float releaseSeconds,
	                              uint32_t sampleRate)
	{
		m_DuckTrigger = trigger;
		m_DuckTarget = static_cast<float>(ToVolumeGain(amountDb));
		m_DuckThreshold = static_cast<float>(std::pow(10.0, static_cast<double>(thresholdDb) / 20.0));
		// The whole amount (to the floor for silence) in the attack's and the release's frames. A duck already deeper than
		// the new amount (an edit while it is down) comes back up from where it is within the release.
		double const depthDb = -std::max(amountDb, MinMixerVolumeDb);
		double const currentDepthDb = -20.0 * std::log10(std::max(m_DuckGain, AudioBusMixGain::DecibelFloor));
		double const releaseDepthDb = std::max(depthDb, currentDepthDb);
		double const rate = static_cast<double>(sampleRate);
		m_DuckAttackFactor = std::pow(10.0, -depthDb / (std::max(static_cast<double>(attackSeconds), 1.0e-6) * rate) / 20.0);
		m_DuckReleaseFactor = std::pow(10.0, releaseDepthDb / (std::max(static_cast<double>(releaseSeconds), 1.0e-6) * rate) / 20.0);
	}

	Result<Scope<AudioSendNode>> AudioSendNode::Create(uint32_t channelCount, AudioSceneGroup& idle)
	{
		// One input and an output for the dry signal and each send, processed in every period its outputs are read in, with
		// silence when the voice gives nothing (AudioMixer.h). Not MA_NODE_FLAG_ALLOW_NULL_INPUT, for the reason the bus
		// nodes give (AudioBusNode::Create).
		static ma_node_vtable const s_Vtable = {
			.onProcess = &AudioSendNode::OnProcess,
			.onGetRequiredInputFrameCount = nullptr,
			.inputBusCount = 1,
			.outputBusCount = OutputCount,
			.flags = MA_NODE_FLAG_CONTINUOUS_PROCESSING,
		};
		ma_engine& engine = Miniaudio::GetEngine();
		Scope<AudioSendNode> sends = CreateScope<AudioSendNode>(PrivateTag{}, channelCount);
		ma_uint32 const inputChannels[1] = {channelCount};
		std::array<ma_uint32, OutputCount> outputChannels = {};
		outputChannels.fill(channelCount);
		ma_node_config config = ma_node_config_init();
		config.vtable = &s_Vtable;
		config.pInputChannels = inputChannels;
		config.pOutputChannels = outputChannels.data();
		sends->m_Node.Owner = sends.get();
		// More buses than a node keeps inline (MA_MAX_NODE_LOCAL_BUS_COUNT): miniaudio allocates them here, on the main thread.
		if (ma_result const result =
		        ma_node_init(ma_engine_get_node_graph(&engine), &config, &Miniaudio::GetAllocationCallbacks(), &sends->m_Node.Base);
		    result != MA_SUCCESS)
		{
			return MakeError("the sound's sends cannot be created: {}", ma_result_description(result));
		}
		sends->m_HasNode = true;
		for (Send& send : sends->m_Sends)
		{
			send.Group = &idle;
			send.Gain.SetTarget(0.0f, 0);
		}
		return sends;
	}

	AudioSendNode::AudioSendNode(PrivateTag, uint32_t channelCount)
		: m_ChannelCount(channelCount)
	{
	}

	AudioSendNode::~AudioSendNode()
	{
		if (m_HasNode)
		{
			// Detached by the mixing thread when its voice came back (or never attached); no thread reads it.
			ma_node_uninit(&m_Node.Base, &Miniaudio::GetAllocationCallbacks());
		}
	}

	void AudioSendNode::OnProcess(ma_node* node, float const** framesIn, ma_uint32* frameCountIn, float** framesOut,
	                              ma_uint32* frameCountOut)
	{
		AudioSendNode& sends = *static_cast<Node*>(node)->Owner;
		float const* const input = framesIn != nullptr ? framesIn[0] : nullptr;
		uint32_t const inputFrames = input != nullptr ? std::min(*frameCountIn, *frameCountOut) : 0;
		uint32_t const frames = input != nullptr ? inputFrames : *frameCountOut;
		sends.Process(input, inputFrames, framesOut, frames);
		if (input != nullptr)
		{
			*frameCountIn = inputFrames;
		}
		*frameCountOut = frames;
	}

	void AudioSendNode::Process(float const* input, uint32_t inputFrames, float** outputs, uint32_t frameCount)
	{
		size_t const sampleCount = static_cast<size_t>(frameCount) * m_ChannelCount;
		size_t const inputSamples = static_cast<size_t>(input != nullptr ? inputFrames : 0) * m_ChannelCount;
		float* const dry = outputs[0];
		if (input != nullptr && input != dry)
		{
			std::copy_n(input, inputSamples, dry);
		}
		std::fill(dry + inputSamples, dry + sampleCount, 0.0f);
		for (uint32_t slot = 0; slot < MaxAudioVoiceSends; slot++)
		{
			Send& send = m_Sends[slot];
			float* const output = outputs[1 + slot];
			if (send.Gain.IsSteady() && send.Gain.Current == 0.0f)
			{
				std::fill(output, output + sampleCount, 0.0f);
				continue;
			}
			// Ramped per frame, so that a send that changes never steps (the rule for every gain, FR-R30.3).
			for (uint32_t frame = 0; frame < frameCount; frame++)
			{
				float const gain = send.Gain.Next();
				size_t const first = static_cast<size_t>(frame) * m_ChannelCount;
				for (uint32_t channel = 0; channel < m_ChannelCount; channel++)
				{
					output[first + channel] = dry[first + channel] * gain;
				}
			}
		}
	}

	AudioMixer::AudioMixer(ma_engine& engine, AudioCommandQueue& queue, AudioReturnList& returns)
		: m_Engine(engine),
		  m_Queue(queue),
		  m_Returns(returns),
		  m_ChannelCount(ma_engine_get_channels(&engine)),
		  m_SampleRate(ma_engine_get_sample_rate(&engine))
	{
	}

	void AudioMixer::Render(float* output, uint32_t frameCount)
	{
		for (uint32_t done = 0; done < frameCount;)
		{
			uint32_t const frames = std::min(AudioBlockFrames, frameCount - done);
			RenderBlock(output + static_cast<size_t>(done) * m_ChannelCount, frames);
			done += frames;
		}
	}

	void AudioMixer::RenderBlock(float* output, uint32_t frameCount)
	{
		Miniaudio::MixingScope const mixing;
		m_Queue.Consume(
			[this](AudioCommand const& command)
			{
				Apply(command);
			});
		StepPitches(frameCount);

		ma_uint64 framesRead = 0;
		if (ma_engine_read_pcm_frames(&m_Engine, output, frameCount, &framesRead) != MA_SUCCESS)
		{
			// The graph silences what it did not read; nothing attached reads nothing.
			framesRead = 0;
		}
		if (framesRead < frameCount)
		{
			std::fill(output + static_cast<size_t>(framesRead) * m_ChannelCount, output + static_cast<size_t>(frameCount) * m_ChannelCount,
			          0.0f);
		}
		// The master volume after Master: a gain on the block, ramped per frame.
		if (!m_MasterVolume.IsSteady() || m_MasterVolume.Current != 1.0f)
		{
			for (uint32_t frame = 0; frame < frameCount; frame++)
			{
				float const gain = m_MasterVolume.Next();
				for (uint32_t channel = 0; channel < m_ChannelCount; channel++)
				{
					output[static_cast<size_t>(frame) * m_ChannelCount + channel] *= gain;
				}
			}
		}
		m_Time += frameCount;
		FinishBlock();
		m_BlockCount.fetch_add(1, std::memory_order_release);
	}

	void AudioMixer::Drain()
	{
		m_Queue.Consume(
			[this](AudioCommand const& command)
			{
				Apply(command);
			});
		// Nothing renders while a lost device is replaced: every fade, move and dip is over at once.
		while (AudioVoice* voice = m_Fading.GetFirst())
		{
			FinishFade(*voice);
		}
		for (AudioSceneGroup* group = m_Groups.GetFirst(); group != nullptr; group = m_Groups.GetNext(*group))
		{
			group->m_MixMoveEnd = 0;
		}
		for (AudioBusNode* bus = m_Buses.GetFirst(); bus != nullptr; bus = m_Buses.GetNext(*bus))
		{
			bus->m_DipEnd = 0;
		}
		FinishBuses();
		FinishSends(true);
	}

	bool AudioMixer::IsHeard(AudioVoice const& voice) const
	{
		return m_Playing.Contains(voice) && voice.m_MixGroup != nullptr && !voice.m_MixGroup->m_MixPaused;
	}

	void AudioMixer::Apply(AudioCommand const& command)
	{
		switch (command.Type)
		{
			case AudioCommandType::StartVoice:
				ApplyStart(*static_cast<AudioVoice*>(command.Object), command);
				break;
			case AudioCommandType::PauseVoice:
			{
				AudioVoice& voice = *static_cast<AudioVoice*>(command.Object);
				if (m_Virtual.Contains(voice))
				{
					CatchUpVirtual(voice);
					m_Virtual.Remove(voice);
				}
				CancelFade(voice, true);
				m_Playing.Remove(voice);
				ma_sound_stop(&voice.m_Sound);
				break;
			}
			case AudioCommandType::StopVoice:
			{
				AudioVoice& voice = *static_cast<AudioVoice*>(command.Object);
				m_Virtual.Remove(voice);
				CancelFade(voice, true);
				m_Playing.Remove(voice);
				ma_sound_stop(&voice.m_Sound);
				// A loaded clip is rewound here; a stream is rewound by the main thread.
				if (voice.m_Samples)
				{
					voice.m_Reader.Cursor = 0;
					ma_sound_seek_to_pcm_frame(&voice.m_Sound, 0);
				}
				voice.m_MixEnded = false;
				break;
			}
			case AudioCommandType::StealVoice:
			{
				AudioVoice& voice = *static_cast<AudioVoice*>(command.Object);
				if (IsHeard(voice))
				{
					BeginFade(voice, AudioVoice::Fade::Steal, SecondsToFrames(AudioStealFadeSeconds, m_SampleRate));
				}
				else
				{
					voice.m_MixFade = AudioVoice::Fade::Steal;
					FinishFade(voice);
				}
				break;
			}
			case AudioCommandType::RetireVoice:
			{
				AudioVoice& voice = *static_cast<AudioVoice*>(command.Object);
				if (!IsHeard(voice))
				{
					ReturnVoice(voice);
				}
				else if (voice.m_MixFade != AudioVoice::Fade::None)
				{
					// Already fading out (stolen into a virtual voice, or moving): it goes when the fade ends.
					voice.m_MixFade = AudioVoice::Fade::Retire;
					voice.m_MixMoveTarget = nullptr;
				}
				else
				{
					BeginFade(voice, AudioVoice::Fade::Retire, SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate));
				}
				break;
			}
			case AudioCommandType::SetVoiceVolume:
				ma_sound_set_volume(&static_cast<AudioVoice*>(command.Object)->m_Sound, command.Values[0]);
				break;
			case AudioCommandType::SetVoicePitch:
			{
				AudioVoice& voice = *static_cast<AudioVoice*>(command.Object);
				if (m_Virtual.Contains(voice))
				{
					// Virtual time so far passed at the pitch it had: the cursor moves through it (ending the play when it
					// reached its end) before the new pitch counts.
					CatchUpVirtual(voice);
				}
				voice.m_MixPitchTarget = command.Values[0];
				uint32_t const frames = SecondsToFrames(AudioParameterSmoothingSeconds, m_SampleRate);
				if (IsHeard(voice) && frames > 0 && voice.m_MixPitch != voice.m_MixPitchTarget)
				{
					voice.m_MixPitchStep = (std::log2(voice.m_MixPitchTarget) - std::log2(voice.m_MixPitch)) / static_cast<float>(frames);
					voice.m_MixPitchFramesLeft = frames;
				}
				else
				{
					voice.m_MixPitch = voice.m_MixPitchTarget;
					voice.m_MixPitchFramesLeft = 0;
					ma_sound_set_pitch(&voice.m_Sound, voice.m_MixPitch);
				}
				break;
			}
			case AudioCommandType::SetVoiceLooping:
			{
				AudioVoice& voice = *static_cast<AudioVoice*>(command.Object);
				voice.m_MixLoop = command.Values[0] != 0.0f;
				ma_sound_set_looping(&voice.m_Sound, voice.m_MixLoop ? MA_TRUE : MA_FALSE);
				break;
			}
			case AudioCommandType::SetVoiceSpatial:
				ma_sound_set_spatialization_enabled(&static_cast<AudioVoice*>(command.Object)->m_Sound,
				                                    command.Values[0] != 0.0f ? MA_TRUE : MA_FALSE);
				break;
			case AudioCommandType::SetVoiceDistances:
			{
				ma_sound* const sound = &static_cast<AudioVoice*>(command.Object)->m_Sound;
				ma_sound_set_min_distance(sound, command.Values[0]);
				ma_sound_set_max_distance(sound, command.Values[1]);
				break;
			}
			case AudioCommandType::SetVoicePosition:
				ma_sound_set_position(&static_cast<AudioVoice*>(command.Object)->m_Sound, command.Values[0], command.Values[1],
				                      command.Values[2]);
				break;
			case AudioCommandType::MoveVoice:
			{
				AudioVoice& voice = *static_cast<AudioVoice*>(command.Object);
				AudioSceneGroup& target = *static_cast<AudioSceneGroup*>(command.Other);
				if (voice.m_MixFade != AudioVoice::Fade::None)
				{
					// Moved at the end of the fade in progress.
					voice.m_MixMoveTarget = &target;
					if (voice.m_MixFade == AudioVoice::Fade::Move && voice.m_MixGroup == &target)
					{
						// Back to the group it was leaving: it fades back in where it is.
						CancelFade(voice, false);
						ma_sound_set_fade_in_pcm_frames(&voice.m_Sound, -1.0f, 1.0f,
						                                SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate));
					}
				}
				else if (voice.m_MixGroup == &target)
				{
					break;
				}
				else if (IsHeard(voice))
				{
					voice.m_MixMoveTarget = &target;
					BeginFade(voice, AudioVoice::Fade::Move, SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate));
				}
				else
				{
					DetachVoice(voice);
					AttachVoice(voice, target);
				}
				break;
			}
			case AudioCommandType::AttachGroup:
			{
				AudioSceneGroup& group = *static_cast<AudioSceneGroup*>(command.Object);
				m_Groups.Add(group);
				ConnectGroup(group, static_cast<AudioBusNode*>(command.Other));
				break;
			}
			case AudioCommandType::AddVoiceSends:
				InsertSends(*static_cast<AudioVoice*>(command.Object), *static_cast<AudioSendNode*>(command.Other));
				break;
			case AudioCommandType::SetVoiceSend:
				SetVoiceSend(*static_cast<AudioVoice*>(command.Object), command.Slot, *static_cast<AudioSceneGroup*>(command.Other),
				             command.Values[0]);
				break;
			case AudioCommandType::SetScenePaused:
			{
				// Every group of the scene in the same block: a voice's sends play into other groups of its scene.
				bool const paused = command.Values[0] != 0.0f;
				for (AudioSceneGroup* group = m_Groups.GetFirst(); group != nullptr; group = m_Groups.GetNext(*group))
				{
					if (group->m_Scene != command.Serial || group->m_MixPaused == paused)
					{
						continue;
					}
					group->m_MixPaused = paused;
					if (paused)
					{
						ma_sound_group_stop(&group->m_Group);
					}
					else
					{
						ma_sound_group_start(&group->m_Group);
					}
				}
				break;
			}
			case AudioCommandType::RetireGroup:
			{
				AudioSceneGroup& group = *static_cast<AudioSceneGroup*>(command.Object);
				group.m_MixRetiring = true;
				ReturnGroupIfDone(group);
				break;
			}
			case AudioCommandType::SetMasterVolume:
				m_MasterVolume.SetTarget(command.Values[0], SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate));
				break;
			case AudioCommandType::MoveGroup:
			case AudioCommandType::AddBus:
			case AudioCommandType::SetBusParent:
			case AudioCommandType::RemoveBus:
			case AudioCommandType::SetBusMixValues:
			case AudioCommandType::SetBusGameValues:
			case AudioCommandType::SetBusEffects:
			case AudioCommandType::SetEffectParameter:
			case AudioCommandType::SetBusDucking:
				ApplyBusCommand(command);
				break;
			case AudioCommandType::None:
				ST_CORE_ASSERT(false, "An audio command without a type");
				break;
		}
	}

	ma_node* AudioMixer::GetInputNode(AudioBusNode* bus)
	{
		return bus != nullptr ? static_cast<ma_node*>(&bus->m_Node.Base) : ma_engine_get_endpoint(&m_Engine);
	}

	void AudioMixer::ConnectGroup(AudioSceneGroup& group, AudioBusNode* bus)
	{
		DisconnectGroup(group);
		ma_node_attach_output_bus(&group.m_Group, 0, GetInputNode(bus), 0);
		group.m_Bus = bus;
		group.m_MixAttached = true;
		if (bus != nullptr)
		{
			bus->m_MixInputCount++;
		}
	}

	void AudioMixer::DisconnectGroup(AudioSceneGroup& group)
	{
		if (!group.m_MixAttached)
		{
			return;
		}
		ma_node_detach_output_bus(&group.m_Group, 0);
		group.m_MixAttached = false;
		if (group.m_Bus != nullptr)
		{
			group.m_Bus->m_MixInputCount--;
		}
		group.m_Bus = nullptr;
	}

	void AudioMixer::ConnectBus(AudioBusNode& bus, AudioBusNode* parent)
	{
		DisconnectBus(bus);
		ma_node_attach_output_bus(&bus.m_Node.Base, 0, GetInputNode(parent), 0);
		bus.m_Parent = parent;
		if (parent != nullptr)
		{
			parent->m_MixInputCount++;
		}
	}

	void AudioMixer::DisconnectBus(AudioBusNode& bus)
	{
		if (bus.m_Parent != nullptr)
		{
			bus.m_Parent->m_MixInputCount--;
		}
		ma_node_detach_output_bus(&bus.m_Node.Base, 0);
		bus.m_Parent = nullptr;
	}

	void AudioMixer::BeginDip(AudioBusNode& bus)
	{
		if (bus.m_Dipping)
		{
			return;
		}
		uint32_t const frames = SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate);
		bus.m_Dip.SetTarget(0.0f, frames);
		bus.m_Dipping = true;
		bus.m_DipEnd = m_Time + frames;
	}

	void AudioMixer::SetBusParent(AudioBusNode& bus, AudioBusNode* parent)
	{
		if (parent == (bus.m_HasPendingParent ? bus.m_PendingParent : bus.m_Parent))
		{
			return;
		}
		if (parent == bus.m_Parent)
		{
			// Back to where it is: the move is called off (a dip under way still swaps a pending chain).
			bus.m_HasPendingParent = false;
			bus.m_PendingParent = nullptr;
			return;
		}
		bus.m_PendingParent = parent;
		bus.m_HasPendingParent = true;
		BeginDip(bus);
	}

	void AudioMixer::MoveGroup(AudioSceneGroup& group, AudioBusNode* bus)
	{
		uint32_t const frames = SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate);
		if (group.m_MixMoving)
		{
			group.m_MixMoveTarget = bus;
			if (bus == group.m_Bus)
			{
				// Back to the bus it was leaving: it fades back in where it is.
				group.m_MixMoving = false;
				ma_sound_group_set_fade_in_pcm_frames(&group.m_Group, -1.0f, 1.0f, frames);
			}
			return;
		}
		if (bus == group.m_Bus || !group.m_MixAttached)
		{
			return;
		}
		if (group.m_MixPaused)
		{
			// Nothing of it is heard: moved at once.
			ConnectGroup(group, bus);
			return;
		}
		group.m_MixMoveTarget = bus;
		group.m_MixMoving = true;
		group.m_MixMoveEnd = m_Time + frames;
		ma_sound_group_set_fade_in_pcm_frames(&group.m_Group, -1.0f, 0.0f, frames);
	}

	void AudioMixer::ApplyBusCommand(AudioCommand const& command)
	{
		switch (command.Type)
		{
			case AudioCommandType::MoveGroup:
				MoveGroup(*static_cast<AudioSceneGroup*>(command.Object), static_cast<AudioBusNode*>(command.Other));
				break;
			case AudioCommandType::AddBus:
			{
				AudioBusNode& bus = *static_cast<AudioBusNode*>(command.Object);
				AudioBusNode* const parent = static_cast<AudioBusNode*>(command.Other);
				m_Buses.Add(bus);
				if (parent == nullptr && m_Master == nullptr)
				{
					m_Master = &bus;
				}
				ConnectBus(bus, parent);
				break;
			}
			case AudioCommandType::SetBusParent:
				SetBusParent(*static_cast<AudioBusNode*>(command.Object), static_cast<AudioBusNode*>(command.Other));
				break;
			case AudioCommandType::RemoveBus:
			{
				AudioBusNode& removed = *static_cast<AudioBusNode*>(command.Object);
				removed.m_MixRemoving = true;
				// What still plays into it goes to Master: the groups of retired scenes whose voices still fade out (the main
				// thread names them no more), and anything the main thread did not move first.
				for (AudioSceneGroup* group = m_Groups.GetFirst(); group != nullptr; group = m_Groups.GetNext(*group))
				{
					if (group->m_MixMoving && group->m_MixMoveTarget == &removed)
					{
						group->m_MixMoveTarget = m_Master;
					}
					else if (!group->m_MixMoving && group->m_Bus == &removed)
					{
						MoveGroup(*group, m_Master);
					}
				}
				for (AudioBusNode* bus = m_Buses.GetFirst(); bus != nullptr; bus = m_Buses.GetNext(*bus))
				{
					if (bus->m_HasPendingParent && bus->m_PendingParent == &removed)
					{
						bus->m_PendingParent = m_Master;
					}
					else if (!bus->m_HasPendingParent && bus->m_Parent == &removed)
					{
						SetBusParent(*bus, m_Master);
					}
					// The removed bus ducks nothing any more (the main thread names it in no command after this one).
					if (bus->m_DuckTrigger == &removed)
					{
						bus->m_DuckTrigger = nullptr;
					}
				}
				break;
			}
			case AudioCommandType::SetBusMixValues:
			{
				AudioBusNode& bus = *static_cast<AudioBusNode*>(command.Object);
				// A transition longer than an edit's ramp moves in decibels over its own length (a snapshot's, §9.8).
				uint32_t const edit = SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate);
				uint32_t const transition = SecondsToFrames(command.Values[2], m_SampleRate);
				bool const inDecibels = transition > edit;
				bus.m_MixGain.SetTarget(command.Values[0], command.Values[1] != 0.0f, inDecibels ? transition : edit, inDecibels);
				break;
			}
			case AudioCommandType::SetBusGameValues:
			{
				AudioBusNode& bus = *static_cast<AudioBusNode*>(command.Object);
				float const gain = command.Values[1] != 0.0f || command.Values[0] <= MinMixerVolumeDb
				                       ? 0.0f
				                       : static_cast<float>(std::pow(10.0, static_cast<double>(command.Values[0]) / 20.0));
				bus.m_GameGain.SetTarget(gain, SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate));
				break;
			}
			case AudioCommandType::SetBusDucking:
				static_cast<AudioBusNode*>(command.Object)
					->SetDucking(static_cast<AudioBusNode*>(command.Other), command.Values[0], command.Values[1], command.Values[2],
				                 command.Values[3], m_SampleRate);
				break;
			case AudioCommandType::SetBusEffects:
			{
				AudioBusNode& bus = *static_cast<AudioBusNode*>(command.Object);
				if (bus.m_HasPendingChain && bus.m_PendingChain != nullptr)
				{
					// Replaced before it ever ran.
					m_Returns.Push(*bus.m_PendingChain);
				}
				bus.m_PendingChain = static_cast<AudioEffectChain*>(command.Other);
				bus.m_HasPendingChain = true;
				BeginDip(bus);
				break;
			}
			case AudioCommandType::SetEffectParameter:
			{
				AudioBusNode const& bus = *static_cast<AudioBusNode*>(command.Object);
				// The main thread's chain is the newest it sent.
				AudioEffectChain const* const chain = bus.m_HasPendingChain ? bus.m_PendingChain : bus.m_Chain;
				if (chain != nullptr && command.Slot < chain->GetEffects().size())
				{
					uint32_t const frames = std::max(SecondsToFrames(AudioParameterSmoothingSeconds, m_SampleRate),
					                                 SecondsToFrames(command.Values[1], m_SampleRate));
					chain->GetEffects()[command.Slot]->SetParameter(command.Parameter, command.Values[0], frames);
				}
				break;
			}
			default:
				break;
		}
	}

	void AudioMixer::FinishBuses()
	{
		uint32_t const frames = SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate);
		for (AudioSceneGroup* group = m_Groups.GetFirst(); group != nullptr;)
		{
			AudioSceneGroup* const next = m_Groups.GetNext(*group);
			if (group->m_MixMoving && m_Time >= group->m_MixMoveEnd)
			{
				group->m_MixMoving = false;
				ConnectGroup(*group, group->m_MixMoveTarget);
				ma_sound_group_set_fade_in_pcm_frames(&group->m_Group, -1.0f, 1.0f, frames);
			}
			group = next;
		}
		for (AudioBusNode* bus = m_Buses.GetFirst(); bus != nullptr;)
		{
			AudioBusNode* const next = m_Buses.GetNext(*bus);
			if (bus->m_Dipping && m_Time >= bus->m_DipEnd)
			{
				if (bus->m_HasPendingParent)
				{
					ConnectBus(*bus, bus->m_PendingParent);
					bus->m_HasPendingParent = false;
					bus->m_PendingParent = nullptr;
				}
				if (bus->m_HasPendingChain)
				{
					if (bus->m_Chain != nullptr)
					{
						m_Returns.Push(*bus->m_Chain);
					}
					bus->m_Chain = bus->m_PendingChain;
					bus->m_HasPendingChain = false;
					bus->m_PendingChain = nullptr;
				}
				bus->m_Dipping = false;
				bus->m_Dip.SetTarget(1.0f, frames);
			}
			bus = next;
		}
		// Removed buses come back once nothing feeds them (their children reach their new parents only at their dips'
		// bottoms, so a removal can wait a block for them).
		for (AudioBusNode* bus = m_Buses.GetFirst(); bus != nullptr;)
		{
			AudioBusNode* const next = m_Buses.GetNext(*bus);
			if (bus->m_MixRemoving && bus->m_MixInputCount == 0)
			{
				ReturnBus(*bus);
			}
			bus = next;
		}
	}

	void AudioMixer::ReturnBus(AudioBusNode& bus)
	{
		DisconnectBus(bus);
		m_Buses.Remove(bus);
		if (m_Master == &bus)
		{
			m_Master = nullptr;
		}
		// Its chains come back with it: the main thread named them for the last time with the bus.
		if (bus.m_HasPendingChain && bus.m_PendingChain != nullptr)
		{
			m_Returns.Push(*bus.m_PendingChain);
		}
		if (bus.m_Chain != nullptr)
		{
			m_Returns.Push(*bus.m_Chain);
		}
		bus.m_Chain = nullptr;
		bus.m_PendingChain = nullptr;
		bus.m_HasPendingChain = false;
		m_Returns.Push(bus);
	}

	void AudioMixer::ApplyStart(AudioVoice& voice, AudioCommand const& command)
	{
		if (!voice.m_MixAttached)
		{
			AttachVoice(voice, *static_cast<AudioSceneGroup*>(command.Other));
			// The voice's first values: miniaudio takes the first volume set at once and smooths the ones after.
			ma_sound_set_volume(&voice.m_Sound, command.Values[0]);
			voice.m_MixPitch = command.Values[1];
			voice.m_MixPitchTarget = command.Values[1];
			ma_sound_set_pitch(&voice.m_Sound, voice.m_MixPitch);
			voice.m_MixLoop = ma_sound_is_looping(&voice.m_Sound) == MA_TRUE;
		}
		voice.m_MixSerial = command.Serial;
		// Started again while fading out: a move completes now, a steal into a virtual voice is called off.
		bool const wasFading = voice.m_MixFade != AudioVoice::Fade::None;
		if (voice.m_MixFade == AudioVoice::Fade::Move)
		{
			FinishFade(voice);
		}
		else
		{
			CancelFade(voice, false);
		}

		uint8_t flags = command.Flags;
		if (flags == AudioStartFlags::Resume && !m_Virtual.Contains(voice))
		{
			flags = AudioStartFlags::Play;
		}
		if (m_Virtual.Contains(voice))
		{
			bool const playsOn = CatchUpVirtual(voice);
			m_Virtual.Remove(voice);
			if (!playsOn)
			{
				return;
			}
		}

		// A play that ended (at the clip's end, or in virtual time) starts over. miniaudio keeps a sound that reached its end
		// started until its next pass (miniaudio.h:76878), and starting a started sound does nothing: stop it first.
		// (ma_sound_start then rewinds a sound at its end itself, a stream included, :78746.)
		if (ma_sound_at_end(&voice.m_Sound) == MA_TRUE || voice.m_MixEnded)
		{
			ma_sound_stop(&voice.m_Sound);
			if (voice.m_Samples)
			{
				voice.m_Reader.Cursor = 0;
				ma_sound_seek_to_pcm_frame(&voice.m_Sound, 0);
			}
			voice.m_MixEnded = false;
		}

		if (flags == AudioStartFlags::Virtual)
		{
			BecomeVirtual(voice);
			return;
		}
		if (flags == AudioStartFlags::Resume)
		{
			ma_sound_set_fade_in_pcm_frames(&voice.m_Sound, 0.0f, 1.0f, SecondsToFrames(AudioStealFadeSeconds, m_SampleRate));
		}
		else if (wasFading)
		{
			// Back up from where the cancelled fade had taken it.
			ma_sound_set_fade_in_pcm_frames(&voice.m_Sound, -1.0f, 1.0f, SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate));
		}
		ma_sound_start(&voice.m_Sound);
		m_Playing.Add(voice);
	}

	void AudioMixer::StepPitches(uint32_t frameCount)
	{
		for (AudioVoice* voice = m_Playing.GetFirst(); voice != nullptr; voice = m_Playing.GetNext(*voice))
		{
			if (voice->m_MixPitchFramesLeft == 0)
			{
				continue;
			}
			// The pitch at the block's end, on a log2 scale; miniaudio's resampler takes it at the block's start, without
			// breaking the waveform.
			uint32_t const frames = std::min(frameCount, voice->m_MixPitchFramesLeft);
			voice->m_MixPitchFramesLeft -= frames;
			voice->m_MixPitch = voice->m_MixPitchFramesLeft == 0
			                        ? voice->m_MixPitchTarget
			                        : std::exp2(std::log2(voice->m_MixPitch) + voice->m_MixPitchStep * static_cast<float>(frames));
			ma_sound_set_pitch(&voice->m_Sound, voice->m_MixPitch);
		}
	}

	void AudioMixer::FinishBlock()
	{
		// Plays that reached their end in this block: reported now, so that the main thread knows at the end of the read
		// (miniaudio's end callback would come at the sound's next pass).
		for (AudioVoice* voice = m_Playing.GetFirst(); voice != nullptr;)
		{
			AudioVoice* const next = m_Playing.GetNext(*voice);
			if (ma_sound_at_end(&voice->m_Sound) == MA_TRUE)
			{
				EndPlay(*voice);
				if (voice->m_MixFade != AudioVoice::Fade::None)
				{
					FinishFade(*voice);
				}
			}
			voice = next;
		}
		for (AudioVoice* voice = m_Fading.GetFirst(); voice != nullptr;)
		{
			AudioVoice* const next = m_Fading.GetNext(*voice);
			if (m_Time >= voice->m_MixFadeEnd)
			{
				FinishFade(*voice);
			}
			voice = next;
		}
		for (AudioVoice* voice = m_Virtual.GetFirst(); voice != nullptr;)
		{
			AudioVoice* const next = m_Virtual.GetNext(*voice);
			if (voice->m_Samples && !voice->m_MixLoop &&
			    voice->m_Reader.Cursor + GetVirtualFrames(*voice) >= voice->m_Samples->GetFrameCount())
			{
				EndPlay(*voice);
			}
			voice = next;
		}
		FinishSends(false);
		FinishBuses();
		// Every bus has processed this block: its level is what ducking reads in the next, whatever order the graph pulled
		// the buses in.
		for (AudioBusNode* bus = m_Buses.GetFirst(); bus != nullptr; bus = m_Buses.GetNext(*bus))
		{
			bus->m_PreviousRms = bus->m_BlockRms;
		}
		// A paused scene's voices stay playing but are not pulled by its stopped group: they are not mixed.
		uint32_t mixed = 0;
		for (AudioVoice const* voice = m_Playing.GetFirst(); voice != nullptr; voice = m_Playing.GetNext(*voice))
		{
			mixed += IsHeard(*voice) ? 1 : 0;
		}
		m_MixedVoices.store(mixed, std::memory_order_relaxed);
	}

	void AudioMixer::BeginFade(AudioVoice& voice, AudioVoice::Fade fade, uint64_t frames)
	{
		// From the fader's current volume (a fade already under way goes on from where it is).
		ma_sound_set_fade_in_pcm_frames(&voice.m_Sound, -1.0f, 0.0f, frames);
		voice.m_MixFade = fade;
		voice.m_MixFadeEnd = m_Time + frames;
		m_Fading.Add(voice);
	}

	void AudioMixer::CancelFade(AudioVoice& voice, bool resetFader)
	{
		if (voice.m_MixFade == AudioVoice::Fade::None)
		{
			return;
		}
		AudioSceneGroup* const moveTarget = voice.m_MixMoveTarget;
		voice.m_MixFade = AudioVoice::Fade::None;
		voice.m_MixMoveTarget = nullptr;
		m_Fading.Remove(voice);
		// A move that was waiting for the fade happens now.
		if (moveTarget != nullptr && moveTarget != voice.m_MixGroup)
		{
			DetachVoice(voice);
			AttachVoice(voice, *moveTarget);
		}
		if (resetFader)
		{
			ma_sound_set_fade_in_pcm_frames(&voice.m_Sound, 1.0f, 1.0f, 0);
		}
	}

	void AudioMixer::FinishFade(AudioVoice& voice)
	{
		AudioVoice::Fade const fade = voice.m_MixFade;
		AudioSceneGroup* const moveTarget = voice.m_MixMoveTarget;
		voice.m_MixFade = AudioVoice::Fade::None;
		voice.m_MixMoveTarget = nullptr;
		m_Fading.Remove(voice);
		switch (fade)
		{
			case AudioVoice::Fade::Steal:
				if (voice.m_OneShot)
				{
					ReturnVoice(voice);
					return;
				}
				// A main voice goes on in virtual time from where it faded out, unless its play ended meanwhile.
				ma_sound_stop(&voice.m_Sound);
				ma_sound_set_fade_in_pcm_frames(&voice.m_Sound, 1.0f, 1.0f, 0);
				m_Playing.Remove(voice);
				if (!voice.m_MixEnded)
				{
					BecomeVirtual(voice);
				}
				break;
			case AudioVoice::Fade::Retire:
				ReturnVoice(voice);
				return;
			case AudioVoice::Fade::Move:
			case AudioVoice::Fade::None:
				break;
		}
		if (moveTarget != nullptr && moveTarget != voice.m_MixGroup)
		{
			DetachVoice(voice);
			AttachVoice(voice, *moveTarget);
		}
		if (fade == AudioVoice::Fade::Move)
		{
			// In its new group it fades back in (or is at full volume again for its next start).
			if (IsHeard(voice))
			{
				ma_sound_set_fade_in_pcm_frames(&voice.m_Sound, 0.0f, 1.0f, SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate));
			}
			else
			{
				ma_sound_set_fade_in_pcm_frames(&voice.m_Sound, 1.0f, 1.0f, 0);
			}
		}
	}

	ma_node* AudioMixer::GetVoiceOutput(AudioVoice& voice)
	{
		if (voice.m_MixSends != nullptr)
		{
			return &voice.m_MixSends->m_Node.Base;
		}
		return &voice.m_Sound;
	}

	void AudioMixer::AttachVoice(AudioVoice& voice, AudioSceneGroup& group)
	{
		ma_node_attach_output_bus(GetVoiceOutput(voice), 0, &group.m_Group, 0);
		voice.m_MixAttached = true;
		voice.m_MixGroup = &group;
		group.m_MixVoiceCount++;
	}

	void AudioMixer::DetachVoice(AudioVoice& voice)
	{
		if (!voice.m_MixAttached)
		{
			return;
		}
		ma_node_detach_output_bus(GetVoiceOutput(voice), 0);
		AudioSceneGroup& group = *voice.m_MixGroup;
		voice.m_MixAttached = false;
		voice.m_MixGroup = nullptr;
		group.m_MixVoiceCount--;
		ReturnGroupIfDone(group);
	}

	void AudioMixer::ReturnGroupIfDone(AudioSceneGroup& group)
	{
		if (group.m_MixRetiring && group.m_MixVoiceCount == 0 && group.m_MixSendCount == 0)
		{
			ReturnGroup(group);
		}
	}

	void AudioMixer::InsertSends(AudioVoice& voice, AudioSendNode& sends)
	{
		// The sound feeds the node from now on (attaching its output detaches it from its group), and the node feeds the
		// group and the sends' groups, all between two blocks.
		voice.m_MixSends = &sends;
		ma_node_attach_output_bus(&voice.m_Sound, 0, &sends.m_Node.Base, 0);
		if (voice.m_MixAttached)
		{
			ma_node_attach_output_bus(&sends.m_Node.Base, 0, &voice.m_MixGroup->m_Group, 0);
		}
		for (uint32_t slot = 0; slot < MaxAudioVoiceSends; slot++)
		{
			ConnectSend(sends, slot, *sends.m_Sends[slot].Group);
		}
	}

	void AudioMixer::ConnectSend(AudioSendNode& sends, uint32_t slot, AudioSceneGroup& group)
	{
		AudioSendNode::Send& send = sends.m_Sends[slot];
		AudioSceneGroup* const previous = send.Connected ? send.Group : nullptr;
		// Attaching moves the output from the group it fed.
		ma_node_attach_output_bus(&sends.m_Node.Base, 1 + slot, &group.m_Group, 0);
		send.Group = &group;
		send.Connected = true;
		group.m_MixSendCount++;
		if (previous != nullptr)
		{
			previous->m_MixSendCount--;
			ReturnGroupIfDone(*previous);
		}
	}

	void AudioMixer::DisconnectSends(AudioSendNode& sends)
	{
		for (uint32_t slot = 0; slot < MaxAudioVoiceSends; slot++)
		{
			AudioSendNode::Send& send = sends.m_Sends[slot];
			if (!send.Connected)
			{
				continue;
			}
			ma_node_detach_output_bus(&sends.m_Node.Base, 1 + slot);
			send.Connected = false;
			send.Moving = false;
			send.Group->m_MixSendCount--;
			ReturnGroupIfDone(*send.Group);
		}
	}

	void AudioMixer::SetVoiceSend(AudioVoice& voice, uint32_t slot, AudioSceneGroup& group, float level)
	{
		if (voice.m_MixSends == nullptr || slot >= MaxAudioVoiceSends)
		{
			return;
		}
		AudioSendNode::Send& send = voice.m_MixSends->m_Sends[slot];
		uint32_t const ramp = SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate);
		bool const heard = IsHeard(voice);
		if (send.Moving)
		{
			if (&group == send.Group)
			{
				// Back to the group it was leaving: it heads for the new level from where its fade has taken it.
				send.Moving = false;
				send.Gain.SetTarget(level, heard ? ramp : 0);
			}
			else
			{
				send.PendingGroup = &group;
				send.PendingLevel = level;
			}
			return;
		}
		if (&group == send.Group)
		{
			send.Gain.SetTarget(level, heard ? ramp : 0);
			return;
		}
		if (!heard || (send.Gain.IsSteady() && send.Gain.Current == 0.0f))
		{
			// Nothing of the send is heard: it moves now and fades in (at once when the voice is not heard).
			send.Gain.SetTarget(0.0f, 0);
			ConnectSend(*voice.m_MixSends, slot, group);
			send.Gain.SetTarget(level, heard ? ramp : 0);
			return;
		}
		send.Moving = true;
		send.PendingGroup = &group;
		send.PendingLevel = level;
		send.Gain.SetTarget(0.0f, ramp);
	}

	void AudioMixer::FinishSends(bool immediate)
	{
		uint32_t const ramp = SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate);
		for (AudioVoice* voice = m_Playing.GetFirst(); voice != nullptr; voice = m_Playing.GetNext(*voice))
		{
			if (voice->m_MixSends == nullptr)
			{
				continue;
			}
			for (uint32_t slot = 0; slot < MaxAudioVoiceSends; slot++)
			{
				AudioSendNode::Send& send = voice->m_MixSends->m_Sends[slot];
				if (!send.Moving || (!immediate && !send.Gain.IsSteady()))
				{
					continue;
				}
				// Faded out: it moves, and fades in where it plays now.
				send.Moving = false;
				send.Gain.SetTarget(0.0f, 0);
				ConnectSend(*voice->m_MixSends, slot, *send.PendingGroup);
				send.Gain.SetTarget(send.PendingLevel, !immediate && IsHeard(*voice) ? ramp : 0);
			}
		}
	}

	void AudioMixer::ReturnVoice(AudioVoice& voice)
	{
		m_Playing.Remove(voice);
		m_Fading.Remove(voice);
		m_Virtual.Remove(voice);
		ma_sound_stop(&voice.m_Sound);
		DetachVoice(voice);
		if (voice.m_MixSends != nullptr)
		{
			// The node comes back with the voice, nothing attached to it either way.
			DisconnectSends(*voice.m_MixSends);
			ma_node_detach_output_bus(&voice.m_Sound, 0);
		}
		// The last touch: the main thread may destroy the voice as soon as it is on the list.
		m_Returns.Push(voice);
	}

	void AudioMixer::ReturnGroup(AudioSceneGroup& group)
	{
		DisconnectGroup(group);
		group.m_MixMoving = false;
		m_Groups.Remove(group);
		m_Returns.Push(group);
	}

	void AudioMixer::EndPlay(AudioVoice& voice)
	{
		voice.m_MixEnded = true;
		m_Playing.Remove(voice);
		m_Virtual.Remove(voice);
		voice.m_EndedSerial.store(voice.m_MixSerial, std::memory_order_release);
	}

	void AudioMixer::BecomeVirtual(AudioVoice& voice)
	{
		ma_sound_stop(&voice.m_Sound);
		m_Playing.Remove(voice);
		// Pitch ramps step only for playing voices: one under way ends here, so that virtual time runs at the pitch asked.
		if (voice.m_MixPitchFramesLeft > 0)
		{
			voice.m_MixPitch = voice.m_MixPitchTarget;
			voice.m_MixPitchFramesLeft = 0;
			ma_sound_set_pitch(&voice.m_Sound, voice.m_MixPitch);
		}
		voice.m_MixVirtualSince = m_Time;
		m_Virtual.Add(voice);
	}

	uint64_t AudioMixer::GetVirtualFrames(AudioVoice const& voice) const
	{
		if (!voice.m_Samples || m_SampleRate == 0)
		{
			return 0;
		}
		// The frames of the clip that playing at the voice's pitch would have taken it through, at the clip's own rate.
		double const elapsed = static_cast<double>(m_Time - voice.m_MixVirtualSince);
		double const ratio = static_cast<double>(voice.m_MixPitch) * voice.m_Samples->GetSampleRate() / static_cast<double>(m_SampleRate);
		return static_cast<uint64_t>(elapsed * ratio);
	}

	bool AudioMixer::CatchUpVirtual(AudioVoice& voice)
	{
		// A stream holds its place: its file is read ahead from where it stopped.
		if (!voice.m_Samples)
		{
			voice.m_MixVirtualSince = m_Time;
			return true;
		}
		uint64_t const length = voice.m_Samples->GetFrameCount();
		uint64_t const target = voice.m_Reader.Cursor + GetVirtualFrames(voice);
		voice.m_MixVirtualSince = m_Time;
		if (length == 0 || (!voice.m_MixLoop && target >= length))
		{
			EndPlay(voice);
			return false;
		}
		// The sound is stopped, so nothing reads the cursor meanwhile.
		voice.m_Reader.Cursor = voice.m_MixLoop ? target % length : target;
		return true;
	}
}
