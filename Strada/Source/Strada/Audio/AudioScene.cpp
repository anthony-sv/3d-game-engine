#include "stpch.h"
#include "Strada/Audio/AudioScene.h"

#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioMixer.h"
#include "Strada/Audio/AudioStream.h"
#include "Strada/Audio/AudioVoice.h"
#include "Strada/Audio/MiniaudioContext.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace Strada
{
	namespace
	{
		bool IsFinite(glm::vec3 const& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// The key of a bus name: names match ignoring letter case, and the empty name is Master.
		std::string MakeBusKey(std::string_view bus)
		{
			return MakeMixerBusKey(bus);
		}

		std::string const s_EmptyBus;
	}

	struct AudioScene::Data final : AudioVoiceOwner
	{
		struct OneShot
		{
			Scope<AudioVoice> Voice;
			// The one-shot's own gain, times the source's volume.
			float Volume = 1.0f;
		};

		// An entity's sound: its main voice and its one-shots, with the settings they share.
		struct Source
		{
			UUID Entity = UUID::Invalid();
			// The clip one-shots play when they name none (null for a streamed source and for a pool alone).
			Ref<AudioClipAsset> Clip;
			Ref<AudioStream> Stream;
			// Null for a pool alone (AddVoicePool): the source then plays one-shots only.
			Scope<AudioVoice> Main;
			// Oldest first: a full pool steals from the front.
			std::vector<OneShot> OneShots;
			float Volume = 1.0f;
			float Pitch = 1.0f;
			bool Spatial = true;
			float MinDistance = 1.0f;
			float MaxDistance = 100.0f;
			glm::vec3 Position = glm::vec3(0.0f);
			std::string Bus;
			uint32_t Priority = DefaultAudioPriority;
			uint32_t MaxVoices = DefaultAudioSourceVoices;
		};

		struct PointVoice
		{
			Scope<AudioVoice> Voice;
			glm::vec3 Position = glm::vec3(0.0f);
		};

		std::unordered_map<UUID, Scope<Source>> Sources;
		std::vector<PointVoice> PointVoices;
		// By bus key.
		std::unordered_map<std::string, Scope<AudioSceneGroup>> Groups;
		glm::vec3 ListenerPosition = glm::vec3(0.0f);
		// Columns: the listener's right, up and back directions (it faces -Z of its basis, like miniaudio's listener).
		glm::mat3 ListenerBasis = glm::mat3(1.0f);
		bool Paused = false;

		Data() { Miniaudio::GetVoiceManager().AddOwner(*this); }

		~Data() override
		{
			// The scene's voices fade out and come back after the scene; its groups come back after their voices.
			AudioVoiceManager& voices = Miniaudio::GetVoiceManager();
			for (auto& [entity, source] : Sources)
			{
				RetireVoices(*source);
			}
			Sources.clear();
			for (PointVoice& point : PointVoices)
			{
				voices.RetireVoice(std::move(point.Voice));
			}
			PointVoices.clear();
			for (auto& [key, group] : Groups)
			{
				voices.RetireGroup(std::move(group));
			}
			Groups.clear();
			voices.RemoveOwner(*this);
		}

		Data(Data const&) = delete;
		Data& operator=(Data const&) = delete;

		Source* Find(UUID entity) const
		{
			auto const it = Sources.find(entity);
			return it != Sources.end() ? it->second.get() : nullptr;
		}

		glm::vec3 ToRelative(glm::vec3 const& position) const { return glm::transpose(ListenerBasis) * (position - ListenerPosition); }

		Result<AudioSceneGroup*> GetGroup(std::string_view bus)
		{
			std::string key = MakeBusKey(bus);
			if (auto const found = Groups.find(key); found != Groups.end())
			{
				return found->second.get();
			}
			Result<Scope<AudioSceneGroup>> group = AudioSceneGroup::Create(std::string(bus), Paused);
			if (!group)
			{
				return Error{group.GetError()};
			}
			AudioSceneGroup* const created = group.GetValue().get();
			Groups.emplace(std::move(key), group.TakeValue());
			return created;
		}

		// What a source shares with its voices, as a description gives it.
		static void TakeSettings(Source& source, AudioSourceDesc const& desc)
		{
			source.Entity = desc.Entity;
			// Also replaces NaN, which a script can set before the source exists.
			source.Volume = desc.Volume > 0.0f ? desc.Volume : 0.0f;
			source.Pitch = desc.Pitch >= MinAudioPitch ? desc.Pitch : MinAudioPitch;
			source.Spatial = desc.Spatial;
			source.MinDistance = desc.MinDistance;
			source.MaxDistance = desc.MaxDistance;
			source.Position = IsFinite(desc.Position) ? desc.Position : glm::vec3(0.0f);
			source.Bus = desc.Bus;
			source.Priority = std::min(desc.Priority, MaxAudioPriority);
			source.MaxVoices = std::clamp(desc.MaxVoices, 1u, MaxAudioSourceVoices);
		}

		// The entity's sound from now on; the one it replaces retires its voices.
		void Replace(Scope<Source> source)
		{
			UUID const entity = source->Entity;
			if (Source* const existing = Find(entity))
			{
				RetireVoices(*existing);
			}
			Sources.insert_or_assign(entity, std::move(source));
		}

		// Whether the source's main voice takes a place in its pool: while it plays or is paused.
		static bool IsMainVoiceInPool(Source const& source)
		{
			return source.Main && (source.Main->IsPlaying() || source.Main->GetState() == AudioVoice::State::Paused);
		}

		void RetireVoices(Source& source)
		{
			AudioVoiceManager& voices = Miniaudio::GetVoiceManager();
			if (source.Main)
			{
				voices.RetireVoice(std::move(source.Main));
			}
			RetireOneShots(source);
		}

		void RetireOneShots(Source& source)
		{
			AudioVoiceManager& voices = Miniaudio::GetVoiceManager();
			for (OneShot& oneShot : source.OneShots)
			{
				voices.RetireVoice(std::move(oneShot.Voice));
			}
			source.OneShots.clear();
		}

		// The pool's oldest one-shot is stolen (its StealVoice is the last command that names it).
		void StealOldestOneShot(Source& source)
		{
			Scope<AudioVoice> stolen = std::move(source.OneShots.front().Voice);
			source.OneShots.erase(source.OneShots.begin());
			Miniaudio::GetVoiceManager().StealOneShot(std::move(stolen));
		}

		void ReleaseVoice(AudioVoice& voice) override
		{
			AudioVoiceManager& voices = Miniaudio::GetVoiceManager();
			for (auto& [entity, source] : Sources)
			{
				for (auto it = source->OneShots.begin(); it != source->OneShots.end(); ++it)
				{
					if (it->Voice.get() == &voice)
					{
						Scope<AudioVoice> released = std::move(it->Voice);
						source->OneShots.erase(it);
						voices.KeepUntilReturned(std::move(released));
						return;
					}
				}
			}
			for (auto it = PointVoices.begin(); it != PointVoices.end(); ++it)
			{
				if (it->Voice.get() == &voice)
				{
					Scope<AudioVoice> released = std::move(it->Voice);
					PointVoices.erase(it);
					voices.KeepUntilReturned(std::move(released));
					return;
				}
			}
			ST_CORE_ASSERT(false, "A scene was asked to release a voice it does not own");
		}

		void RetireEndedVoices() override
		{
			// Retired first, then the emptied places removed: erase_if's predicate must not change what it tests.
			AudioVoiceManager& voices = Miniaudio::GetVoiceManager();
			for (auto& [entity, source] : Sources)
			{
				for (OneShot& oneShot : source->OneShots)
				{
					if (oneShot.Voice->HasEnded())
					{
						voices.RetireVoice(std::move(oneShot.Voice));
					}
				}
				std::erase_if(source->OneShots,
				              [](OneShot const& oneShot)
				              {
								  return !oneShot.Voice;
							  });
			}
			for (PointVoice& point : PointVoices)
			{
				if (point.Voice->HasEnded())
				{
					voices.RetireVoice(std::move(point.Voice));
				}
			}
			std::erase_if(PointVoices,
			              [](PointVoice const& point)
			              {
							  return !point.Voice;
						  });
		}

		// Every voice of the scene, main voices, one-shots and the scene's own.
		template<typename TFunction>
		void ForEachVoice(TFunction&& function)
		{
			for (auto& [entity, source] : Sources)
			{
				if (source->Main)
				{
					function(*source->Main);
				}
				for (OneShot const& oneShot : source->OneShots)
				{
					function(*oneShot.Voice);
				}
			}
			for (PointVoice const& point : PointVoices)
			{
				function(*point.Voice);
			}
		}

		void UpdatePositions(Source& source) const
		{
			glm::vec3 const relative = ToRelative(source.Position);
			if (source.Main)
			{
				source.Main->SetRelativePosition(relative);
			}
			for (OneShot const& oneShot : source.OneShots)
			{
				oneShot.Voice->SetRelativePosition(relative);
			}
		}

		// The voice of a one-shot or of the scene, started (or refused at the engine's limit). Returns whether it plays.
		bool StartVoice(AudioVoiceDesc const& desc, std::string_view bus, Source* source, float oneShotVolume, glm::vec3 const& position)
		{
			Result<AudioSceneGroup*> group = GetGroup(bus);
			if (!group)
			{
				ST_CORE_WARN("A sound cannot play: {}", group.GetError());
				return false;
			}
			Result<Scope<AudioVoice>> created = AudioVoice::Create(desc, *group.GetValue(), *this);
			if (!created)
			{
				ST_CORE_WARN("A sound cannot play: {}", created.GetError());
				return false;
			}
			Scope<AudioVoice> voice = created.TakeValue();
			AudioVoice* const started = voice.get();
			started->Play();
			if (source != nullptr)
			{
				source->OneShots.push_back({std::move(voice), oneShotVolume});
			}
			else
			{
				PointVoices.push_back({std::move(voice), position});
			}
			Miniaudio::GetVoiceManager().Balance();
			// The limit may have refused it (and released it at once): then the scene no longer holds it.
			if (source != nullptr)
			{
				return std::any_of(source->OneShots.begin(), source->OneShots.end(),
				                   [started](OneShot const& oneShot)
				                   {
									   return oneShot.Voice.get() == started;
								   });
			}
			return std::any_of(PointVoices.begin(), PointVoices.end(),
			                   [started](PointVoice const& point)
			                   {
								   return point.Voice.get() == started;
							   });
		}

		// A clip's samples for a one-shot, logged once per clip when it cannot be decoded.
		static Ref<AudioSampleBuffer> GetSamples(Ref<AudioClipAsset> const& clip)
		{
			AudioSampleCache& cache = Miniaudio::GetSampleCache();
			uint64_t const decodes = cache.GetDecodeCount();
			Result<Ref<AudioSampleBuffer>> samples = cache.Get(clip);
			if (!samples)
			{
				if (cache.GetDecodeCount() != decodes)
				{
					ST_CORE_WARN("A sound cannot play: {}", samples.GetError());
				}
				return nullptr;
			}
			return samples.TakeValue();
		}
	};

	Result<Scope<AudioScene>> AudioScene::Create()
	{
		if (!AudioEngine::IsInitialized())
		{
			return Error{"the audio engine is not initialized"};
		}
		Scope<Data> data = CreateScope<Data>();
		// Master's group now, so that a scene that cannot make one fails here as it always did.
		if (Result<AudioSceneGroup*> group = data->GetGroup(s_EmptyBus); !group)
		{
			return Error{group.GetError()};
		}
		return CreateScope<AudioScene>(PrivateTag{}, std::move(data));
	}

	AudioScene::AudioScene(PrivateTag, Scope<Data> data)
		: m_Data(std::move(data))
	{
	}

	AudioScene::~AudioScene()
	{
		ST_CORE_ASSERT(AudioEngine::IsInitialized(), "An AudioScene outlived the AudioEngine");
	}

	Result<void> AudioScene::AddSource(AudioSourceDesc const& desc)
	{
		if (!desc.Clip && desc.StreamFile.empty())
		{
			return Error{"the audio source has no clip"};
		}

		Scope<Data::Source> source = CreateScope<Data::Source>();
		AudioVoiceDesc voiceDesc;
		AudioStreamer* const streamer = Miniaudio::GetStreamer();
		if (!desc.StreamFile.empty())
		{
			if (streamer == nullptr)
			{
				return Error{"sounds cannot stream: the audio streaming thread is not running"};
			}
			Result<Ref<AudioStream>> stream = AudioStream::Open(desc.StreamFile, Miniaudio::GetStreamFileOpener());
			if (!stream)
			{
				return Error{stream.GetError()};
			}
			source->Stream = stream.TakeValue();
			voiceDesc.Stream = source->Stream;
		}
		else
		{
			Result<Ref<AudioSampleBuffer>> samples = Miniaudio::GetSampleCache().Get(desc.Clip);
			if (!samples)
			{
				return Error{samples.GetError()};
			}
			source->Clip = desc.Clip;
			voiceDesc.Samples = samples.TakeValue();
		}

		Data::TakeSettings(*source, desc);
		Result<AudioSceneGroup*> group = m_Data->GetGroup(source->Bus);
		if (!group)
		{
			return Error{group.GetError()};
		}
		voiceDesc.Volume = source->Volume;
		voiceDesc.Pitch = source->Pitch;
		voiceDesc.Loop = desc.Loop;
		voiceDesc.Spatial = source->Spatial;
		voiceDesc.MinDistance = source->MinDistance;
		voiceDesc.MaxDistance = source->MaxDistance;
		voiceDesc.RelativePosition = m_Data->ToRelative(source->Position);
		voiceDesc.Priority = source->Priority;
		Result<Scope<AudioVoice>> voice = AudioVoice::Create(voiceDesc, *group.GetValue(), *m_Data);
		if (!voice)
		{
			return Error{voice.GetError()};
		}
		source->Main = voice.TakeValue();
		if (source->Stream)
		{
			// Decoding starts now, so a sound played right away is heard within moments.
			streamer->Add(source->Stream);
		}

		m_Data->Replace(std::move(source));
		return {};
	}

	Result<void> AudioScene::AddVoicePool(AudioSourceDesc const& desc)
	{
		Scope<Data::Source> source = CreateScope<Data::Source>();
		Data::TakeSettings(*source, desc);
		// The group is made now, so that a pool that could never play fails here and keeps the current sound.
		if (Result<AudioSceneGroup*> group = m_Data->GetGroup(source->Bus); !group)
		{
			return Error{group.GetError()};
		}
		m_Data->Replace(std::move(source));
		return {};
	}

	void AudioScene::RemoveSource(UUID entity)
	{
		if (Data::Source* const source = m_Data->Find(entity))
		{
			m_Data->RetireVoices(*source);
			m_Data->Sources.erase(entity);
		}
	}

	bool AudioScene::HasSource(UUID entity) const
	{
		return m_Data->Sources.contains(entity);
	}

	bool AudioScene::IsStreaming(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		return source != nullptr && source->Stream != nullptr;
	}

	size_t AudioScene::GetSourceCount() const
	{
		return m_Data->Sources.size();
	}

	void AudioScene::Play(UUID entity)
	{
		Data::Source* const source = m_Data->Find(entity);
		if (source == nullptr || !source->Main || source->Main->IsPlaying())
		{
			return;
		}
		source->Main->Play();
		Miniaudio::GetVoiceManager().Balance();
		if (source->Stream)
		{
			// Starting over at the end seeks the stream: usually to samples it holds, otherwise it reads them now.
			Miniaudio::GetStreamer()->Wake();
		}
	}

	void AudioScene::Pause(UUID entity)
	{
		Data::Source* const source = m_Data->Find(entity);
		if (source != nullptr && source->Main)
		{
			source->Main->Pause();
		}
	}

	void AudioScene::Stop(UUID entity)
	{
		Data::Source* const source = m_Data->Find(entity);
		if (source == nullptr)
		{
			return;
		}
		if (source->Main)
		{
			source->Main->Stop();
		}
		if (source->Stream)
		{
			// The streaming thread reads the start again while the sound is stopped.
			source->Stream->Seek(0);
			Miniaudio::GetStreamer()->Wake();
		}
		m_Data->RetireOneShots(*source);
	}

	bool AudioScene::IsPlaying(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		return source != nullptr && source->Main && source->Main->IsPlaying();
	}

	float AudioScene::GetVolume(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		return source != nullptr ? source->Volume : 0.0f;
	}

	void AudioScene::SetVolume(UUID entity, float volume)
	{
		Data::Source* const source = m_Data->Find(entity);
		if (source == nullptr)
		{
			return;
		}
		source->Volume = volume > 0.0f ? volume : 0.0f;
		if (source->Main)
		{
			source->Main->SetVolume(source->Volume);
		}
		for (Data::OneShot const& oneShot : source->OneShots)
		{
			oneShot.Voice->SetVolume(source->Volume * oneShot.Volume);
		}
	}

	float AudioScene::GetPitch(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		return source != nullptr ? source->Pitch : 0.0f;
	}

	void AudioScene::SetPitch(UUID entity, float pitch)
	{
		Data::Source* const source = m_Data->Find(entity);
		if (source == nullptr)
		{
			return;
		}
		// Also replaces NaN.
		source->Pitch = pitch >= MinAudioPitch ? pitch : MinAudioPitch;
		if (source->Main)
		{
			source->Main->SetPitch(source->Pitch);
		}
		for (Data::OneShot const& oneShot : source->OneShots)
		{
			oneShot.Voice->SetPitch(source->Pitch);
		}
	}

	bool AudioScene::IsLooping(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		return source != nullptr && source->Main && source->Main->IsLooping();
	}

	void AudioScene::SetLooping(UUID entity, bool loop)
	{
		// One-shots never loop.
		Data::Source* const source = m_Data->Find(entity);
		if (source != nullptr && source->Main)
		{
			source->Main->SetLooping(loop);
		}
	}

	bool AudioScene::IsSpatial(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		return source != nullptr && source->Spatial;
	}

	void AudioScene::SetSpatial(UUID entity, bool spatial)
	{
		Data::Source* const source = m_Data->Find(entity);
		if (source == nullptr)
		{
			return;
		}
		source->Spatial = spatial;
		if (source->Main)
		{
			source->Main->SetSpatial(spatial);
		}
		for (Data::OneShot const& oneShot : source->OneShots)
		{
			oneShot.Voice->SetSpatial(spatial);
		}
	}

	void AudioScene::SetDistances(UUID entity, float minDistance, float maxDistance)
	{
		Data::Source* const source = m_Data->Find(entity);
		if (source == nullptr)
		{
			return;
		}
		source->MinDistance = minDistance;
		source->MaxDistance = maxDistance;
		if (source->Main)
		{
			source->Main->SetDistances(minDistance, maxDistance);
		}
		for (Data::OneShot const& oneShot : source->OneShots)
		{
			oneShot.Voice->SetDistances(minDistance, maxDistance);
		}
	}

	glm::vec3 AudioScene::GetPosition(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		return source != nullptr ? source->Position : glm::vec3(0.0f);
	}

	void AudioScene::SetPosition(UUID entity, glm::vec3 const& position)
	{
		Data::Source* const source = m_Data->Find(entity);
		if (source != nullptr && IsFinite(position))
		{
			source->Position = position;
			m_Data->UpdatePositions(*source);
		}
	}

	void AudioScene::SetBus(UUID entity, std::string_view bus)
	{
		Data::Source* const source = m_Data->Find(entity);
		if (source == nullptr || MakeBusKey(bus) == MakeBusKey(source->Bus))
		{
			if (source != nullptr)
			{
				source->Bus = bus;
			}
			return;
		}
		Result<AudioSceneGroup*> group = m_Data->GetGroup(bus);
		if (!group)
		{
			ST_CORE_WARN("A sound cannot move to bus '{}': {}", bus, group.GetError());
			return;
		}
		source->Bus = bus;
		if (source->Main)
		{
			source->Main->MoveTo(*group.GetValue());
		}
		for (Data::OneShot const& oneShot : source->OneShots)
		{
			oneShot.Voice->MoveTo(*group.GetValue());
		}
	}

	std::string const& AudioScene::GetBus(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		return source != nullptr ? source->Bus : s_EmptyBus;
	}

	void AudioScene::SetPriority(UUID entity, uint32_t priority)
	{
		Data::Source* const source = m_Data->Find(entity);
		if (source == nullptr)
		{
			return;
		}
		source->Priority = std::min(priority, MaxAudioPriority);
		if (source->Main)
		{
			source->Main->SetPriority(source->Priority);
		}
		for (Data::OneShot const& oneShot : source->OneShots)
		{
			oneShot.Voice->SetPriority(source->Priority);
		}
	}

	uint32_t AudioScene::GetPriority(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		return source != nullptr ? source->Priority : DefaultAudioPriority;
	}

	void AudioScene::SetMaxVoices(UUID entity, uint32_t maxVoices)
	{
		Data::Source* const source = m_Data->Find(entity);
		if (source == nullptr)
		{
			return;
		}
		source->MaxVoices = std::clamp(maxVoices, 1u, MaxAudioSourceVoices);
		m_Data->RetireEndedVoices();
		uint32_t const mainVoice = Data::IsMainVoiceInPool(*source) ? 1 : 0;
		while (!source->OneShots.empty() && mainVoice + source->OneShots.size() > source->MaxVoices)
		{
			m_Data->StealOldestOneShot(*source);
		}
	}

	uint32_t AudioScene::GetMaxVoices(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		return source != nullptr ? source->MaxVoices : DefaultAudioSourceVoices;
	}

	bool AudioScene::PlayOneShot(UUID entity, Ref<AudioClipAsset> const& clip, float volume)
	{
		Data::Source* const source = m_Data->Find(entity);
		if (source == nullptr)
		{
			return false;
		}
		Ref<AudioClipAsset> const& played = clip ? clip : source->Clip;
		if (!played)
		{
			ST_CORE_WARN("A one-shot of a sound with no loaded clip (a streamed sound, or a pool alone) needs a clip to play");
			return false;
		}
		Ref<AudioSampleBuffer> samples = Data::GetSamples(played);
		if (!samples)
		{
			return false;
		}

		// One-shots that ended free their places at once, without waiting for a frame.
		m_Data->RetireEndedVoices();
		uint32_t const mainVoice = Data::IsMainVoiceInPool(*source) ? 1 : 0;
		if (mainVoice + source->OneShots.size() >= source->MaxVoices)
		{
			if (source->OneShots.empty())
			{
				// The pool holds its main voice alone, which one-shots never steal.
				Miniaudio::GetVoiceManager().CountStolen();
				return false;
			}
			m_Data->StealOldestOneShot(*source);
		}

		float const oneShotVolume = volume > 0.0f ? volume : 0.0f;
		AudioVoiceDesc desc;
		desc.Samples = std::move(samples);
		desc.Volume = source->Volume * oneShotVolume;
		desc.Pitch = source->Pitch;
		desc.Loop = false;
		desc.Spatial = source->Spatial;
		desc.MinDistance = source->MinDistance;
		desc.MaxDistance = source->MaxDistance;
		desc.RelativePosition = m_Data->ToRelative(source->Position);
		desc.Priority = source->Priority;
		desc.OneShot = true;
		return m_Data->StartVoice(desc, source->Bus, source, oneShotVolume, source->Position);
	}

	uint32_t AudioScene::GetVoiceCount(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		if (source == nullptr)
		{
			return 0;
		}
		uint32_t count = source->Main && source->Main->IsPlaying() ? 1 : 0;
		for (Data::OneShot const& oneShot : source->OneShots)
		{
			count += oneShot.Voice->IsPlaying() ? 1 : 0;
		}
		return count;
	}

	bool AudioScene::PlayClipAtPoint(Ref<AudioClipAsset> const& clip, glm::vec3 const& position, float volume, std::string_view bus)
	{
		if (!clip || !IsFinite(position))
		{
			return false;
		}
		Ref<AudioSampleBuffer> samples = Data::GetSamples(clip);
		if (!samples)
		{
			return false;
		}
		m_Data->RetireEndedVoices();
		AudioVoiceDesc desc;
		desc.Samples = std::move(samples);
		desc.Volume = volume > 0.0f ? volume : 0.0f;
		desc.Spatial = true;
		desc.RelativePosition = m_Data->ToRelative(position);
		desc.OneShot = true;
		return m_Data->StartVoice(desc, bus, nullptr, desc.Volume, position);
	}

	void AudioScene::SetListener(glm::vec3 const& position, glm::vec3 const& forward, glm::vec3 const& up)
	{
		if (!IsFinite(position) || !IsFinite(forward) || !IsFinite(up))
		{
			return;
		}

		constexpr float MinLength = 1e-6f;
		float const forwardLength = glm::length(forward);
		glm::vec3 const front = forwardLength > MinLength ? forward / forwardLength : glm::vec3(0.0f, 0.0f, -1.0f);
		float const upLength = glm::length(up);
		glm::vec3 right = glm::cross(front, upLength > MinLength ? up / upLength : glm::vec3(0.0f, 1.0f, 0.0f));
		if (glm::length(right) < 1e-3f)
		{
			// Up is missing or parallel to the forward direction: any perpendicular right will do.
			right = glm::cross(front, std::abs(front.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f));
		}
		right = glm::normalize(right);

		m_Data->ListenerPosition = position;
		m_Data->ListenerBasis = glm::mat3(right, glm::cross(right, front), -front);
		for (auto const& [entity, source] : m_Data->Sources)
		{
			m_Data->UpdatePositions(*source);
		}
		for (Data::PointVoice const& point : m_Data->PointVoices)
		{
			point.Voice->SetRelativePosition(m_Data->ToRelative(point.Position));
		}
	}

	glm::vec3 AudioScene::GetListenerPosition() const
	{
		return m_Data->ListenerPosition;
	}

	void AudioScene::SetPaused(bool paused)
	{
		if (paused == m_Data->Paused)
		{
			return;
		}
		m_Data->Paused = paused;
		// A stopped group does not pull from its voices, so they keep their positions.
		for (auto const& [key, group] : m_Data->Groups)
		{
			group->SetPaused(paused);
		}
		if (paused)
		{
			m_Data->ForEachVoice(
				[](AudioVoice& voice)
				{
					voice.Hold();
				});
		}
		// Pausing makes room at the engine's limit; resuming may need some.
		Miniaudio::GetVoiceManager().Balance();
	}

	bool AudioScene::IsPaused() const
	{
		return m_Data->Paused;
	}
}
