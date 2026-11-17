#include "stpch.h"
#include "Strada/Audio/AudioVoice.h"

#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Audio/AudioDecoder.h"
#include "Strada/Audio/AudioMixer.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Audio/AudioStream.h"
#include "Strada/Audio/MiniaudioContext.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Strada
{
	namespace
	{
		// Frames a decoding reads at a time.
		constexpr uint64_t DecodeChunkFrames = 4096;
		// A virtual voice comes back in place of one that mixes only when it is this much louder (about 3.5 dB), so that two
		// voices of nearly the same loudness do not trade places at every update.
		constexpr float VirtualReturnFactor = 1.5f;

		Error MakeTooLongError(uint64_t sampleCount)
		{
			constexpr double Mebibyte = 1024.0 * 1024.0;
			return MakeError("the clip takes {:.1f} MiB once decoded, more than the {:.0f} MiB a loaded clip may take: stream it "
			                 "instead (set Stream)",
			                 static_cast<double>(sampleCount * sizeof(float)) / Mebibyte,
			                 static_cast<double>(MaxDecodedClipBytes) / Mebibyte);
		}
	}

	Result<Ref<AudioSampleBuffer>> AudioSampleBuffer::Decode(AudioClipAsset const& clip)
	{
		AudioMemoryFile file(clip.GetData());
		Result<Scope<AudioDecoder>> created = AudioDecoder::Create(clip.GetFormat(), file);
		if (!created)
		{
			return MakeError("the {} clip cannot be decoded: {}", AudioFormatToString(clip.GetFormat()), created.GetError());
		}
		Scope<AudioDecoder> const decoder = created.TakeValue();
		uint32_t const channelCount = decoder->GetChannelCount();
		if (channelCount == 0)
		{
			return MakeError("the {} clip has no channels", AudioFormatToString(clip.GetFormat()));
		}
		uint64_t const maxSamples = MaxDecodedClipBytes / sizeof(float);
		// The length a header states is only used to refuse early: damaged files state any length, so nothing is sized
		// from it. An MP3's length would only show by decoding it whole.
		if (clip.GetFormat() != AudioFormat::Mp3)
		{
			ma_uint64 length = 0;
			if (ma_data_source_get_length_in_pcm_frames(decoder->GetDataSource(), &length) == MA_SUCCESS &&
			    length > maxSamples / channelCount)
			{
				return MakeTooLongError(length * channelCount);
			}
		}

		std::vector<float> samples;
		for (;;)
		{
			size_t const decoded = samples.size();
			samples.resize(decoded + DecodeChunkFrames * channelCount);
			ma_uint64 framesRead = 0;
			ma_result const result =
				ma_data_source_read_pcm_frames(decoder->GetDataSource(), samples.data() + decoded, DecodeChunkFrames, &framesRead);
			samples.resize(decoded + static_cast<size_t>(framesRead) * channelCount);
			if (samples.size() > maxSamples)
			{
				return MakeTooLongError(samples.size());
			}
			// The end, or an error part way: what decoded plays.
			if (result != MA_SUCCESS || framesRead == 0)
			{
				break;
			}
		}
		samples.shrink_to_fit();
		std::span<ma_channel const> const map = decoder->GetChannelMap();
		return CreateRef<AudioSampleBuffer>(PrivateTag{}, std::move(samples), channelCount, decoder->GetSampleRate(),
		                                    std::vector<ma_channel>(map.begin(), map.end()));
	}

	AudioSampleBuffer::AudioSampleBuffer(PrivateTag, std::vector<float> samples, uint32_t channelCount, uint32_t sampleRate,
	                                     std::vector<ma_channel> channelMap)
		: m_Samples(std::move(samples)),
		  m_ChannelCount(channelCount),
		  m_SampleRate(sampleRate),
		  m_ChannelMap(std::move(channelMap))
	{
	}

	Result<Ref<AudioSampleBuffer>> AudioSampleCache::Get(Ref<AudioClipAsset> const& clip)
	{
		ST_CORE_ASSERT(clip, "AudioSampleCache::Get needs a clip");
		// Clips come and go with their assets; their entries go with them, so a long session holds only the live ones.
		if (m_Entries.size() >= m_NextPrune)
		{
			Prune();
		}
		if (auto const found = m_Entries.find(clip.get()); found != m_Entries.end() && found->second.Clip.lock() == clip)
		{
			if (found->second.Samples)
			{
				return found->second.Samples;
			}
			return Error{found->second.Error};
		}

		m_DecodeCount++;
		Result<Ref<AudioSampleBuffer>> decoded = AudioSampleBuffer::Decode(*clip);
		Entry entry;
		entry.Clip = clip;
		if (decoded)
		{
			entry.Samples = decoded.GetValue();
		}
		else
		{
			entry.Error = decoded.GetError();
		}
		m_Entries.insert_or_assign(clip.get(), std::move(entry));
		return decoded;
	}

	void AudioSampleCache::Prune()
	{
		std::erase_if(m_Entries,
		              [](auto const& item)
		              {
						  return item.second.Clip.expired();
					  });
		m_NextPrune = std::max<size_t>(MinPruneSize, m_Entries.size() * 2);
	}

	Result<Scope<AudioVoice>> AudioVoice::Create(AudioVoiceDesc const& desc, AudioSceneGroup& group, AudioVoiceOwner& owner)
	{
		ST_CORE_ASSERT(desc.Samples || desc.Stream, "A voice plays samples or a stream");
		ma_engine& engine = Miniaudio::GetEngine();
		Scope<AudioVoice> voice = CreateScope<AudioVoice>(PrivateTag{});
		ma_data_source* dataSource = nullptr;
		if (desc.Samples)
		{
			static constexpr ma_data_source_vtable SampleReaderVTable = {
				.onRead = OnRead,
				.onSeek = OnSeek,
				.onGetDataFormat = OnGetDataFormat,
				.onGetCursor = OnGetCursor,
				.onGetLength = OnGetLength,
				.onSetLooping = nullptr,
				.flags = 0,
			};
			ma_data_source_config config = ma_data_source_config_init();
			config.vtable = &SampleReaderVTable;
			if (ma_result const result = ma_data_source_init(&config, &voice->m_Reader.Base); result != MA_SUCCESS)
			{
				return MakeError("the voice's samples cannot be read: {}", ma_result_description(result));
			}
			voice->m_HasReader = true;
			voice->m_Samples = desc.Samples;
			voice->m_Reader.Samples = desc.Samples.get();
			dataSource = &voice->m_Reader.Base;
		}
		else
		{
			voice->m_Stream = desc.Stream;
			dataSource = desc.Stream->GetDataSource();
		}

		ma_sound_config config = ma_sound_config_init_2(&engine);
		config.pDataSource = dataSource;
		config.flags = MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
		config.volumeSmoothTimeInPCMFrames =
			static_cast<ma_uint32>(std::lround(AudioGainSmoothingSeconds * static_cast<float>(ma_engine_get_sample_rate(&engine))));
		if (ma_result const result = ma_sound_init_ex(&engine, &config, &voice->m_Sound); result != MA_SUCCESS)
		{
			return MakeError("the sound cannot be created: {}", ma_result_description(result));
		}
		voice->m_HasSound = true;

		// Positions are relative to the scene's listener, which miniaudio's listener stays at.
		ma_sound_set_positioning(&voice->m_Sound, ma_positioning_relative);
		ma_sound_set_attenuation_model(&voice->m_Sound, ma_attenuation_model_inverse);
		// Sources and the listener have no velocities (doppler is the engine's, §9.8).
		ma_sound_set_doppler_factor(&voice->m_Sound, 0.0f);

		voice->m_OneShot = desc.OneShot;
		voice->m_Priority = std::min(desc.Priority, MaxAudioPriority);
		voice->m_Group = &group;
		voice->m_Owner = &owner;
		// The volume reaches miniaudio with the voice's first start, so that its first value is taken at once however often
		// it changed before (miniaudio's gainer smooths every value after the first, miniaudio.h:51010-51016).
		voice->m_Volume = std::max(desc.Volume, 0.0f);
		voice->SetPitch(desc.Pitch);
		voice->SetLooping(desc.Loop && !desc.OneShot);
		voice->SetDistances(desc.MinDistance, desc.MaxDistance);
		voice->SetSpatial(desc.Spatial);
		voice->m_RelativePosition = desc.RelativePosition;
		ma_sound_set_position(&voice->m_Sound, desc.RelativePosition.x, desc.RelativePosition.y, desc.RelativePosition.z);
		Miniaudio::GetVoiceManager().AddVoice(*voice);
		return voice;
	}

	AudioVoice::AudioVoice(PrivateTag)
	{
	}

	AudioVoice::~AudioVoice()
	{
		if (m_TableIndex != NotRegistered)
		{
			Miniaudio::GetVoiceManager().RemoveVoice(*this);
		}
		// The sound reads the samples or the stream until it is uninitialized; then the stream can leave the streaming
		// thread.
		if (m_HasSound)
		{
			ma_sound_uninit(&m_Sound);
		}
		if (m_HasReader)
		{
			ma_data_source_uninit(&m_Reader.Base);
		}
		if (m_Stream)
		{
			if (AudioStreamer* streamer = Miniaudio::GetStreamer())
			{
				streamer->Remove(*m_Stream);
			}
		}
	}

	ma_result AudioVoice::OnRead(ma_data_source* dataSource, void* output, ma_uint64 frameCount, ma_uint64* framesRead)
	{
		SampleReader& reader = *reinterpret_cast<SampleReader*>(dataSource);
		uint64_t const total = reader.Samples->GetFrameCount();
		uint64_t const frames = std::min<uint64_t>(frameCount, total - std::min(reader.Cursor, total));
		uint32_t const channels = reader.Samples->GetChannelCount();
		if (frames > 0)
		{
			std::memcpy(output, reader.Samples->GetSamples().data() + reader.Cursor * channels, frames * channels * sizeof(float));
		}
		reader.Cursor += frames;
		if (framesRead != nullptr)
		{
			*framesRead = frames;
		}
		return frames < frameCount ? MA_AT_END : MA_SUCCESS;
	}

	ma_result AudioVoice::OnSeek(ma_data_source* dataSource, ma_uint64 frame)
	{
		SampleReader& reader = *reinterpret_cast<SampleReader*>(dataSource);
		reader.Cursor = std::min<uint64_t>(frame, reader.Samples->GetFrameCount());
		return MA_SUCCESS;
	}

	ma_result AudioVoice::OnGetDataFormat(ma_data_source* dataSource, ma_format* format, ma_uint32* channels, ma_uint32* sampleRate,
	                                      ma_channel* channelMap, size_t channelMapCapacity)
	{
		AudioSampleBuffer const& samples = *reinterpret_cast<SampleReader*>(dataSource)->Samples;
		*format = ma_format_f32;
		*channels = samples.GetChannelCount();
		*sampleRate = samples.GetSampleRate();
		if (channelMap != nullptr)
		{
			std::span<ma_channel const> const map = samples.GetChannelMap();
			size_t const count = std::min(channelMapCapacity, map.size());
			std::copy_n(map.begin(), count, channelMap);
		}
		return MA_SUCCESS;
	}

	ma_result AudioVoice::OnGetCursor(ma_data_source* dataSource, ma_uint64* cursor)
	{
		*cursor = reinterpret_cast<SampleReader*>(dataSource)->Cursor;
		return MA_SUCCESS;
	}

	ma_result AudioVoice::OnGetLength(ma_data_source* dataSource, ma_uint64* length)
	{
		*length = reinterpret_cast<SampleReader*>(dataSource)->Samples->GetFrameCount();
		return MA_SUCCESS;
	}

	void AudioVoice::Send(AudioCommandType type, std::array<float, 4> const& values) const
	{
		AudioCommand command;
		command.Type = type;
		command.Object = const_cast<AudioVoice*>(this);
		command.Values = values;
		Miniaudio::GetVoiceManager().Submit(command);
	}

	void AudioVoice::SendStart(uint8_t flags)
	{
		if (!m_HeldByMixer)
		{
			m_Group->Attach();
			m_HeldByMixer = true;
		}
		AudioCommand command;
		command.Type = AudioCommandType::StartVoice;
		command.Flags = flags;
		command.Object = this;
		command.Other = m_Group;
		command.Values = {m_Volume, m_Pitch, 0.0f, 0.0f};
		command.Serial = m_PlaySerial;
		Miniaudio::GetVoiceManager().Submit(command);
	}

	void AudioVoice::Play()
	{
		if (IsPlaying())
		{
			return;
		}
		// From a stop or an end the play is new (it starts at the clip's start and takes a new place in the stealing
		// order); from a pause it continues.
		m_NewPlay = m_NewPlay || m_State != State::Paused;
		m_State = State::Playing;
		m_Mixing = false;
		m_Virtual = false;
		m_EndedAtRefresh = false;
		m_PlaySerial = Miniaudio::GetVoiceManager().NextSerial();
	}

	void AudioVoice::Pause()
	{
		if (m_State != State::Playing)
		{
			return;
		}
		m_State = State::Paused;
		m_Mixing = false;
		m_Virtual = false;
		if (m_HeldByMixer)
		{
			Send(AudioCommandType::PauseVoice);
		}
	}

	void AudioVoice::Stop()
	{
		m_State = State::Stopped;
		m_Mixing = false;
		m_Virtual = false;
		m_NewPlay = false;
		if (m_HeldByMixer)
		{
			Send(AudioCommandType::StopVoice);
		}
	}

	void AudioVoice::Hold()
	{
		if (m_Virtual)
		{
			// The mixing thread stops the voice's time where it has taken it; it waits for its scene like any other.
			Send(AudioCommandType::PauseVoice);
			m_Virtual = false;
		}
	}

	void AudioVoice::SetVolume(float volume)
	{
		volume = std::max(volume, 0.0f);
		if (volume == m_Volume)
		{
			return;
		}
		m_Volume = volume;
		if (m_HeldByMixer)
		{
			Send(AudioCommandType::SetVoiceVolume, {volume, 0.0f, 0.0f, 0.0f});
		}
	}

	void AudioVoice::SetPitch(float pitch)
	{
		// Also replaces NaN.
		pitch = pitch >= MinAudioPitch ? pitch : MinAudioPitch;
		if (pitch == m_Pitch && m_HeldByMixer)
		{
			return;
		}
		m_Pitch = pitch;
		if (m_HeldByMixer)
		{
			Send(AudioCommandType::SetVoicePitch, {pitch, 0.0f, 0.0f, 0.0f});
		}
		else
		{
			ma_sound_set_pitch(&m_Sound, pitch);
		}
	}

	void AudioVoice::SetLooping(bool loop)
	{
		loop = loop && !m_OneShot;
		if (loop == m_Loop && m_HeldByMixer)
		{
			return;
		}
		m_Loop = loop;
		if (m_HeldByMixer)
		{
			Send(AudioCommandType::SetVoiceLooping, {loop ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f});
		}
		else
		{
			ma_sound_set_looping(&m_Sound, loop ? MA_TRUE : MA_FALSE);
		}
	}

	void AudioVoice::SetSpatial(bool spatial)
	{
		if (spatial == m_Spatial && m_HeldByMixer)
		{
			return;
		}
		m_Spatial = spatial;
		if (m_HeldByMixer)
		{
			Send(AudioCommandType::SetVoiceSpatial, {spatial ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f});
		}
		else
		{
			ma_sound_set_spatialization_enabled(&m_Sound, spatial ? MA_TRUE : MA_FALSE);
		}
	}

	void AudioVoice::SetDistances(float minDistance, float maxDistance)
	{
		// A zero minimum distance would divide zero by zero next to the listener.
		float const nearest = minDistance >= MinAudioDistance ? minDistance : MinAudioDistance;
		float const farthest = maxDistance >= nearest ? maxDistance : nearest;
		if (nearest == m_MinDistance && farthest == m_MaxDistance && m_HeldByMixer)
		{
			return;
		}
		m_MinDistance = nearest;
		m_MaxDistance = farthest;
		if (m_HeldByMixer)
		{
			Send(AudioCommandType::SetVoiceDistances, {nearest, farthest, 0.0f, 0.0f});
		}
		else
		{
			ma_sound_set_min_distance(&m_Sound, nearest);
			ma_sound_set_max_distance(&m_Sound, farthest);
		}
	}

	void AudioVoice::SetRelativePosition(glm::vec3 const& position)
	{
		if (position == m_RelativePosition)
		{
			return;
		}
		m_RelativePosition = position;
		if (m_HeldByMixer)
		{
			Send(AudioCommandType::SetVoicePosition, {position.x, position.y, position.z, 0.0f});
		}
		else
		{
			ma_sound_set_position(&m_Sound, position.x, position.y, position.z);
		}
	}

	void AudioVoice::MoveTo(AudioSceneGroup& group)
	{
		if (&group == m_Group)
		{
			return;
		}
		m_Group = &group;
		if (m_HeldByMixer)
		{
			group.Attach();
			AudioCommand command;
			command.Type = AudioCommandType::MoveVoice;
			command.Object = this;
			command.Other = &group;
			Miniaudio::GetVoiceManager().Submit(command);
		}
	}

	AudioVoiceManager::AudioVoiceManager(AudioCommandQueue& queue)
		: m_Queue(queue)
	{
	}

	AudioVoiceManager::~AudioVoiceManager()
	{
		ST_CORE_ASSERT(m_Returning.empty(), "AudioVoiceManager destroyed while it waits for objects to come back");
	}

	void AudioVoiceManager::RetireVoice(Scope<AudioVoice> voice)
	{
		if (voice->IsHeldByMixer())
		{
			voice->Send(AudioCommandType::RetireVoice);
		}
		KeepUntilReturned(std::move(voice));
	}

	void AudioVoiceManager::KeepUntilReturned(Scope<AudioVoice> voice)
	{
		RemoveVoice(*voice);
		bool const held = voice->IsHeldByMixer();
		Keep(Scope<AudioMixerObject>(std::move(voice)), held);
	}

	void AudioVoiceManager::StealOneShot(Scope<AudioVoice> voice)
	{
		if (voice->IsHeldByMixer())
		{
			voice->Send(AudioCommandType::StealVoice);
		}
		CountStolen();
		KeepUntilReturned(std::move(voice));
	}

	void AudioVoiceManager::RetireGroup(Scope<AudioSceneGroup> group)
	{
		group->Retire();
		bool const held = group->IsHeldByMixer();
		Keep(Scope<AudioMixerObject>(std::move(group)), held);
	}

	void AudioVoiceManager::Keep(Scope<AudioMixerObject> object, bool held)
	{
		if (held)
		{
			AudioMixerObject* const key = object.get();
			m_Returning.emplace(key, std::move(object));
		}
		// Otherwise the mixing thread never saw it: destroyed now.
	}

	void AudioVoiceManager::TakeReturns(AudioReturnList& returns)
	{
		returns.TakeAll(
			[this](AudioMixerObject& object)
			{
				auto const found = m_Returning.find(&object);
				ST_CORE_ASSERT(found != m_Returning.end(), "The mixing thread returned an object the engine did not lend");
				if (found != m_Returning.end())
				{
					m_Returning.erase(found);
				}
			});
	}

	void AudioVoiceManager::DestroyReturning()
	{
		// In any order: no thread mixes, and each miniaudio node detaches its output and its inputs as it goes
		// (ma_node_detach_full, miniaudio.h:74790-74828), so none waits for a reader.
		m_Returning.clear();
	}

	void AudioVoiceManager::AddVoice(AudioVoice& voice)
	{
		ST_CORE_ASSERT(voice.m_TableIndex == AudioVoice::NotRegistered, "The voice is already in the table");
		voice.m_TableIndex = m_Voices.size();
		m_Voices.push_back(&voice);
	}

	void AudioVoiceManager::RemoveVoice(AudioVoice& voice)
	{
		size_t const index = voice.m_TableIndex;
		if (index == AudioVoice::NotRegistered)
		{
			return;
		}
		AudioVoice* const last = m_Voices.back();
		m_Voices[index] = last;
		last->m_TableIndex = index;
		m_Voices.pop_back();
		voice.m_TableIndex = AudioVoice::NotRegistered;
	}

	void AudioVoiceManager::AddOwner(AudioVoiceOwner& owner)
	{
		m_Owners.push_back(&owner);
	}

	void AudioVoiceManager::RemoveOwner(AudioVoiceOwner& owner)
	{
		std::erase(m_Owners, &owner);
	}

	void AudioVoiceManager::Refresh()
	{
		for (AudioVoiceOwner* owner : m_Owners)
		{
			owner->RetireEndedVoices();
		}
		for (AudioVoice* voice : m_Voices)
		{
			voice->m_EndedAtRefresh = voice->HasEnded();
		}
	}

	bool AudioVoiceManager::IsCandidate(AudioVoice const& voice)
	{
		return voice.m_State == AudioVoice::State::Playing && !voice.HasEnded() && !voice.m_Group->IsPaused();
	}

	float AudioVoiceManager::EstimateLoudness(AudioVoice const& voice)
	{
		float attenuation = 1.0f;
		if (voice.m_Spatial)
		{
			// The inverse law of the voice's attenuation (miniaudio's ma_attenuation_model_inverse between the distances).
			float const distance = std::clamp(glm::length(voice.m_RelativePosition), voice.m_MinDistance, voice.m_MaxDistance);
			attenuation = voice.m_MinDistance / distance;
		}
		return voice.m_Volume * attenuation;
	}

	void AudioVoiceManager::StartMixing(AudioVoice& voice)
	{
		uint8_t const flags = voice.m_Virtual ? AudioStartFlags::Resume : AudioStartFlags::Play;
		if (voice.m_NewPlay)
		{
			voice.m_StartSequence = NextStartSequence();
			voice.m_NewPlay = false;
		}
		voice.m_Virtual = false;
		voice.m_Mixing = true;
		voice.SendStart(flags);
	}

	void AudioVoiceManager::Exclude(AudioVoice& voice)
	{
		if (voice.m_Virtual)
		{
			return;
		}
		CountStolen();
		bool const wasMixing = voice.m_Mixing;
		voice.m_Mixing = false;
		if (voice.m_OneShot)
		{
			// The last command that names the one-shot: it comes back after its fade (or at once, never handed over).
			if (voice.m_HeldByMixer)
			{
				voice.Send(AudioCommandType::StealVoice);
			}
			voice.m_State = AudioVoice::State::Stopped;
			voice.m_Owner->ReleaseVoice(voice);
			return;
		}
		// A main voice becomes virtual: it keeps playing in time, unheard, and comes back when it ranks within the limit.
		voice.m_Virtual = true;
		if (wasMixing)
		{
			voice.Send(AudioCommandType::StealVoice);
		}
		else
		{
			if (voice.m_NewPlay)
			{
				voice.m_StartSequence = NextStartSequence();
				voice.m_NewPlay = false;
			}
			voice.SendStart(AudioStartFlags::Virtual);
		}
	}

	void AudioVoiceManager::Balance()
	{
		m_Candidates.clear();
		for (AudioVoice* voice : m_Voices)
		{
			if (IsCandidate(*voice))
			{
				m_Candidates.push_back({voice, voice->m_Priority, 0.0f, voice->m_NewPlay ? ~uint64_t(0) : voice->m_StartSequence});
			}
		}
		if (m_Candidates.size() <= MaxMixedAudioVoices)
		{
			for (Candidate const& candidate : m_Candidates)
			{
				if (!candidate.Voice->m_Mixing)
				{
					StartMixing(*candidate.Voice);
				}
			}
			return;
		}

		for (Candidate& candidate : m_Candidates)
		{
			candidate.Loudness = EstimateLoudness(*candidate.Voice) / (candidate.Voice->m_Virtual ? VirtualReturnFactor : 1.0f);
		}
		// Higher priority, then louder, then newer ranks first.
		auto const ranksHigher = [](Candidate const& first, Candidate const& second)
		{
			if (first.Priority != second.Priority)
			{
				return first.Priority > second.Priority;
			}
			if (first.Loudness != second.Loudness)
			{
				return first.Loudness > second.Loudness;
			}
			return first.Sequence > second.Sequence;
		};
		auto const kept = m_Candidates.begin() + MaxMixedAudioVoices;
		std::nth_element(m_Candidates.begin(), kept, m_Candidates.end(), ranksHigher);
		for (auto candidate = kept; candidate != m_Candidates.end(); ++candidate)
		{
			Exclude(*candidate->Voice);
		}
		for (auto candidate = m_Candidates.begin(); candidate != kept; ++candidate)
		{
			if (!candidate->Voice->m_Mixing)
			{
				StartMixing(*candidate->Voice);
			}
		}
	}

	bool AudioVoiceManager::HasVirtualVoices() const
	{
		return std::any_of(m_Voices.begin(), m_Voices.end(),
		                   [](AudioVoice const* voice)
		                   {
							   return voice->m_Virtual;
						   });
	}

	uint32_t AudioVoiceManager::GetActiveCount() const
	{
		return static_cast<uint32_t>(std::count_if(m_Voices.begin(), m_Voices.end(),
		                                           [](AudioVoice const* voice)
		                                           {
													   return voice->m_State == AudioVoice::State::Playing && voice->m_Mixing &&
			                                                  !voice->m_EndedAtRefresh && !voice->m_Group->IsPaused();
												   }));
	}
}
