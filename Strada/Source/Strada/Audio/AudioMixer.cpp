#include "stpch.h"
#include "Strada/Audio/AudioMixer.h"

#include "Strada/Audio/MiniaudioContext.h"

#include <algorithm>
#include <cmath>

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

	Result<Scope<AudioSceneGroup>> AudioSceneGroup::Create(std::string busKey, bool paused)
	{
		ma_engine& engine = Miniaudio::GetEngine();
		Scope<AudioSceneGroup> group = CreateScope<AudioSceneGroup>(PrivateTag{}, std::move(busKey));
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
		return group;
	}

	AudioSceneGroup::AudioSceneGroup(PrivateTag, std::string busKey)
		: m_BusKey(std::move(busKey))
	{
	}

	AudioSceneGroup::~AudioSceneGroup()
	{
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
		command.Other = Miniaudio::FindBus(m_BusKey);
		Miniaudio::GetVoiceManager().Submit(command);
	}

	void AudioSceneGroup::SetPaused(bool paused)
	{
		if (paused == m_Paused)
		{
			return;
		}
		m_Paused = paused;
		if (m_HeldByMixer)
		{
			AudioCommand command;
			command.Type = AudioCommandType::SetGroupPaused;
			command.Object = this;
			command.Values[0] = paused ? 1.0f : 0.0f;
			Miniaudio::GetVoiceManager().Submit(command);
		}
		else
		{
			m_MixPaused = paused;
			if (paused)
			{
				ma_sound_group_stop(&m_Group);
			}
			else
			{
				ma_sound_group_start(&m_Group);
			}
		}
	}

	void AudioSceneGroup::Retire()
	{
		if (m_HeldByMixer)
		{
			AudioCommand command;
			command.Type = AudioCommandType::RetireGroup;
			command.Object = this;
			Miniaudio::GetVoiceManager().Submit(command);
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
		// Nothing renders while a lost device is replaced: every fade is over at once.
		while (AudioVoice* voice = m_Fading.GetFirst())
		{
			FinishFade(*voice);
		}
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
				group.m_Bus = static_cast<AudioBusNode*>(command.Other);
				ma_node* const output =
					group.m_Bus != nullptr ? static_cast<ma_node*>(&group.m_Bus->m_Node) : ma_engine_get_endpoint(&m_Engine);
				ma_node_attach_output_bus(&group.m_Group, 0, output, 0);
				group.m_MixAttached = true;
				break;
			}
			case AudioCommandType::SetGroupPaused:
			{
				AudioSceneGroup& group = *static_cast<AudioSceneGroup*>(command.Object);
				group.m_MixPaused = command.Values[0] != 0.0f;
				if (group.m_MixPaused)
				{
					ma_sound_group_stop(&group.m_Group);
				}
				else
				{
					ma_sound_group_start(&group.m_Group);
				}
				break;
			}
			case AudioCommandType::RetireGroup:
			{
				AudioSceneGroup& group = *static_cast<AudioSceneGroup*>(command.Object);
				group.m_MixRetiring = true;
				if (group.m_MixVoiceCount == 0)
				{
					ReturnGroup(group);
				}
				break;
			}
			case AudioCommandType::SetMasterVolume:
				m_MasterVolume.SetTarget(command.Values[0], SecondsToFrames(AudioGainSmoothingSeconds, m_SampleRate));
				break;
			default:
				// The buses' commands (MoveGroup, AddBus, SetBusParent, RemoveBus, the bus and effect values) are sent only
				// once the mixer has buses; nothing sends them before.
				ST_CORE_ASSERT(false, "An audio command the mixer does not apply");
				break;
		}
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
		m_MixedVoices.store(m_Playing.GetCount(), std::memory_order_relaxed);
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

	void AudioMixer::AttachVoice(AudioVoice& voice, AudioSceneGroup& group)
	{
		ma_node_attach_output_bus(&voice.m_Sound, 0, &group.m_Group, 0);
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
		ma_node_detach_output_bus(&voice.m_Sound, 0);
		AudioSceneGroup& group = *voice.m_MixGroup;
		voice.m_MixAttached = false;
		voice.m_MixGroup = nullptr;
		group.m_MixVoiceCount--;
		if (group.m_MixRetiring && group.m_MixVoiceCount == 0)
		{
			ReturnGroup(group);
		}
	}

	void AudioMixer::ReturnVoice(AudioVoice& voice)
	{
		m_Playing.Remove(voice);
		m_Fading.Remove(voice);
		m_Virtual.Remove(voice);
		ma_sound_stop(&voice.m_Sound);
		DetachVoice(voice);
		// The last touch: the main thread may destroy the voice as soon as it is on the list.
		m_Returns.Push(voice);
	}

	void AudioMixer::ReturnGroup(AudioSceneGroup& group)
	{
		if (group.m_MixAttached)
		{
			ma_node_detach_output_bus(&group.m_Group, 0);
			group.m_MixAttached = false;
		}
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
