#include "stpch.h"
#include "Strada/Audio/AudioScene.h"

#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioMixer.h"
#include "Strada/Audio/AudioQueue.h"
#include "Strada/Audio/AudioStream.h"
#include "Strada/Audio/AudioVoice.h"
#include "Strada/Audio/MiniaudioContext.h"
#include "Strada/Core/Profiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>
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
		// One send of a voice: a group of the scene (of a zone's bus) and its level.
		struct VoiceSend
		{
			AudioSceneGroup* Group = nullptr;
			float Level = 0.0f;
		};
		// The sends of a voice, the largest first (at most MaxAudioVoiceSends; the first Count hold one).
		struct VoiceSends
		{
			std::array<VoiceSend, MaxAudioVoiceSends> Sends = {};
			uint32_t Count = 0;
		};
		// A zone of the last update whose bus the mixer has, with the scene's group of that bus.
		struct ResolvedZone
		{
			AudioReverbZoneDesc Desc;
			AudioSceneGroup* Group = nullptr;
		};

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
			float DopplerLevel = DefaultAudioDopplerLevel;
			// Doppler (§9.8): the velocity the scene last gave (none until it gives one) and the factor its voices play at.
			std::optional<glm::vec3> Velocity;
			float Doppler = 1.0f;
			// Reverb zones (§9.8): what its voices send, as the last update found it.
			VoiceSends Sends;
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
		std::optional<glm::vec3> ListenerVelocity;
		bool Paused = false;
		// The scene's number, which its groups carry (one SetScenePaused names them all).
		uint64_t Serial = 0;
		// The zones of the last update that send, and the keys of the buses zones named that the mixer lacks (logged once).
		std::vector<ResolvedZone> Zones;
		std::unordered_set<std::string> MissingZoneBuses;
		bool SendsFailed = false;

		Data()
			: Serial(Miniaudio::GetVoiceManager().NextSceneSerial())
		{
			Miniaudio::GetVoiceManager().AddOwner(*this);
		}

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
			Result<Scope<AudioSceneGroup>> group = AudioSceneGroup::Create(std::string(bus), Paused, Serial);
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
			source.DopplerLevel = ClampDopplerLevel(desc.DopplerLevel);
		}

		static float ClampDopplerLevel(float level) { return std::isnan(level) ? 0.0f : std::clamp(level, 0.0f, MaxAudioDopplerLevel); }

		// The factor of a voice of the scene, which stands still at its position.
		float ComputePointDoppler(glm::vec3 const& position) const
		{
			if (!ListenerVelocity)
			{
				return 1.0f;
			}
			return ComputeDopplerFactor(position, glm::vec3(0.0f), ListenerPosition, *ListenerVelocity, 1.0f,
			                            AudioEngine::GetMixer().SpeedOfSound);
		}

		// The source's voices, its one-shots included, play at its factor.
		static void ApplyDoppler(Source& source)
		{
			if (source.Main)
			{
				source.Main->SetDoppler(source.Doppler);
			}
			for (OneShot const& oneShot : source.OneShots)
			{
				oneShot.Voice->SetDoppler(source.Doppler);
			}
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

		// The larger send first; of two equal ones, the bus whose key sorts first.
		static bool IsSendBefore(VoiceSend const& left, VoiceSend const& right)
		{
			if (left.Level != right.Level)
			{
				return left.Level > right.Level;
			}
			return left.Group->GetBusKey() < right.Group->GetBusKey();
		}

		// What a spatial sound at position sends to the zones of the last update: per bus the largest send, and the
		// MaxAudioVoiceSends largest of those (ties to the bus whose key sorts first, so that it never depends on the zones'
		// order).
		VoiceSends ComputeSends(glm::vec3 const& position) const
		{
			VoiceSends result;
			for (ResolvedZone const& zone : Zones)
			{
				float const level = ComputeReverbZoneSend(zone.Desc, position);
				if (!(level > 0.0f))
				{
					continue;
				}
				auto const first = result.Sends.begin();
				auto const last = first + result.Count;
				auto const same = std::find_if(first, last,
				                               [&zone](VoiceSend const& send)
				                               {
												   return send.Group == zone.Group;
											   });
				VoiceSend const candidate = {zone.Group, level};
				if (same != last)
				{
					same->Level = std::max(same->Level, level);
				}
				else if (result.Count < MaxAudioVoiceSends)
				{
					result.Sends[result.Count++] = candidate;
				}
				else
				{
					// Full: the new bus takes the place of the smallest send when it ranks before it.
					auto const smallest = std::max_element(first, last, &IsSendBefore);
					if (IsSendBefore(candidate, *smallest))
					{
						*smallest = candidate;
					}
				}
			}
			// Ordered by an insertion over the sends in place, not std::sort: GCC 13 at -O2 cannot see that Count stays within
			// the array, and reports the branch of libstdc++'s sort for more than 16 elements as out of its bounds
			// (-Warray-bounds). The loops are bounded by the array's size in plain sight.
			uint32_t const count = std::min(result.Count, MaxAudioVoiceSends);
			for (uint32_t index = 1; index < count; index++)
			{
				VoiceSend const send = result.Sends[index];
				uint32_t place = index;
				while (place > 0 && IsSendBefore(send, result.Sends[place - 1]))
				{
					result.Sends[place] = result.Sends[place - 1];
					place--;
				}
				result.Sends[place] = send;
			}
			return result;
		}

		// The scene's group of Master, made with the scene: where the sends of a voice without a zone play silence.
		AudioSceneGroup& GetMasterGroup() const { return *Groups.at(MakeBusKey(s_EmptyBus)); }

		// Gives the voice its sends: a bus it already sends to keeps its slot (no fade out and in), a new bus takes a slot the
		// voice no longer needs, the quietest first, and slots left over go silent where they are.
		void ApplySends(AudioVoice& voice, VoiceSends const& sends)
		{
			if (!voice.HasSends())
			{
				if (sends.Count == 0)
				{
					return;
				}
				if (Result<void> enabled = voice.EnableSends(GetMasterGroup()); !enabled)
				{
					if (!SendsFailed)
					{
						ST_CORE_WARN("A sound cannot send to reverb zones: {}", enabled.GetError());
						SendsFailed = true;
					}
					return;
				}
			}
			std::array<bool, MaxAudioVoiceSends> slotTaken = {};
			std::array<bool, MaxAudioVoiceSends> sendPlaced = {};
			for (uint32_t index = 0; index < sends.Count; index++)
			{
				for (uint32_t slot = 0; slot < MaxAudioVoiceSends; slot++)
				{
					if (!slotTaken[slot] && voice.GetSendGroup(slot) == sends.Sends[index].Group)
					{
						voice.SetSend(slot, *sends.Sends[index].Group, sends.Sends[index].Level);
						slotTaken[slot] = true;
						sendPlaced[index] = true;
						break;
					}
				}
			}
			for (uint32_t index = 0; index < sends.Count; index++)
			{
				if (sendPlaced[index])
				{
					continue;
				}
				uint32_t chosen = MaxAudioVoiceSends;
				for (uint32_t slot = 0; slot < MaxAudioVoiceSends; slot++)
				{
					if (!slotTaken[slot] && (chosen == MaxAudioVoiceSends || voice.GetSendLevel(slot) < voice.GetSendLevel(chosen)))
					{
						chosen = slot;
					}
				}
				voice.SetSend(chosen, *sends.Sends[index].Group, sends.Sends[index].Level);
				slotTaken[chosen] = true;
			}
			for (uint32_t slot = 0; slot < MaxAudioVoiceSends; slot++)
			{
				if (!slotTaken[slot])
				{
					voice.SetSend(slot, *voice.GetSendGroup(slot), 0.0f);
				}
			}
		}

		// The voice of a one-shot or of the scene, started (or refused at the engine's limit), with the sends of its sound
		// set before it is handed over. Returns whether it plays.
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
			if (desc.Spatial)
			{
				ApplySends(*started, source != nullptr ? source->Sends : ComputeSends(position));
			}
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

	std::optional<glm::vec3> ComputeAudioVelocity(glm::vec3 const& from, glm::vec3 const& to, float seconds, float speedOfSound)
	{
		if (!(seconds > 0.0f) || !std::isfinite(seconds) || !IsFinite(from) || !IsFinite(to))
		{
			return std::nullopt;
		}
		glm::vec3 const velocity = (to - from) / seconds;
		float const speed = glm::length(velocity);
		// A move faster than half the speed of sound is a teleport (and NaN or infinite speeds are no speed at all).
		if (!std::isfinite(speed) || !(speed <= 0.5f * speedOfSound))
		{
			return std::nullopt;
		}
		return velocity;
	}

	float ComputeDopplerFactor(glm::vec3 const& sourcePosition, glm::vec3 const& sourceVelocity, glm::vec3 const& listenerPosition,
	                           glm::vec3 const& listenerVelocity, float level, float speedOfSound)
	{
		constexpr float MinSeparation = 0.001f;
		if (!(level > 0.0f) || !std::isfinite(level) || !(speedOfSound > 0.0f) || !std::isfinite(speedOfSound) ||
		    !IsFinite(sourcePosition) || !IsFinite(sourceVelocity) || !IsFinite(listenerPosition) || !IsFinite(listenerVelocity))
		{
			return 1.0f;
		}
		glm::vec3 const towardListener = listenerPosition - sourcePosition;
		float const distance = glm::length(towardListener);
		if (!(distance >= MinSeparation) || !std::isfinite(distance))
		{
			return 1.0f;
		}
		// Positive as they come closer: the source moving toward the listener, or the listener toward the source.
		float const closing = glm::dot(sourceVelocity - listenerVelocity, towardListener / distance) * level;
		float const remaining = speedOfSound - closing;
		// c / (c - v) reaches the highest factor at c - v = c / MaxAudioDopplerFactor, and never divides by zero beyond it.
		if (!(remaining > speedOfSound / MaxAudioDopplerFactor))
		{
			return std::isfinite(remaining) ? MaxAudioDopplerFactor : 1.0f;
		}
		return std::clamp(speedOfSound / remaining, MinAudioDopplerFactor, MaxAudioDopplerFactor);
	}

	float ComputeReverbZoneSend(AudioReverbZoneDesc const& zone, glm::vec3 const& position)
	{
		if (!(zone.Level > 0.0f) || !std::isfinite(zone.Level) || !IsFinite(position) || !IsFinite(zone.Position) ||
		    std::isnan(zone.FadeDistance))
		{
			return 0.0f;
		}
		// How far the position lies outside the shape (0 inside it).
		float distance = 0.0f;
		if (zone.Shape == ReverbZoneShape::Box)
		{
			// In the box's own axes: the columns of Rotation are its axes in world space.
			glm::vec3 const local = glm::transpose(zone.Rotation) * (position - zone.Position);
			glm::vec3 const outside = glm::max(glm::abs(local) - glm::max(zone.HalfExtents, glm::vec3(0.0f)), glm::vec3(0.0f));
			distance = glm::length(outside);
		}
		else
		{
			distance = std::max(glm::length(position - zone.Position) - std::max(zone.Radius, 0.0f), 0.0f);
		}
		if (std::isnan(distance))
		{
			return 0.0f;
		}
		if (distance <= 0.0f)
		{
			return zone.Level;
		}
		if (!(zone.FadeDistance > 0.0f) || distance >= zone.FadeDistance)
		{
			return 0.0f;
		}
		return zone.Level * (1.0f - distance / zone.FadeDistance);
	}

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
		if (!spatial)
		{
			// A sound that is not positioned has no motion to hear, and is in no zone.
			source->Velocity.reset();
			source->Doppler = 1.0f;
			Data::ApplyDoppler(*source);
			source->Sends = {};
			if (source->Main)
			{
				m_Data->ApplySends(*source->Main, source->Sends);
			}
			for (Data::OneShot const& oneShot : source->OneShots)
			{
				m_Data->ApplySends(*oneShot.Voice, source->Sends);
			}
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

	void AudioScene::SetDopplerLevel(UUID entity, float level)
	{
		if (Data::Source* const source = m_Data->Find(entity))
		{
			source->DopplerLevel = Data::ClampDopplerLevel(level);
		}
	}

	float AudioScene::GetDopplerLevel(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		return source != nullptr ? source->DopplerLevel : DefaultAudioDopplerLevel;
	}

	void AudioScene::SetVelocity(UUID entity, std::optional<glm::vec3> const& velocity)
	{
		if (Data::Source* const source = m_Data->Find(entity))
		{
			source->Velocity = velocity && IsFinite(*velocity) ? velocity : std::nullopt;
		}
	}

	void AudioScene::SetListenerVelocity(std::optional<glm::vec3> const& velocity)
	{
		m_Data->ListenerVelocity = velocity && IsFinite(*velocity) ? velocity : std::nullopt;
	}

	void AudioScene::UpdateDoppler()
	{
		float const speedOfSound = AudioEngine::GetMixer().SpeedOfSound;
		std::optional<glm::vec3> const& listenerVelocity = m_Data->ListenerVelocity;
		for (auto const& [entity, source] : m_Data->Sources)
		{
			bool const moving = source->Spatial && source->DopplerLevel > 0.0f && source->Velocity && listenerVelocity;
			source->Doppler = moving ? ComputeDopplerFactor(source->Position, *source->Velocity, m_Data->ListenerPosition,
			                                                *listenerVelocity, source->DopplerLevel, speedOfSound)
			                         : 1.0f;
			Data::ApplyDoppler(*source);
		}
		for (Data::PointVoice const& point : m_Data->PointVoices)
		{
			point.Voice->SetDoppler(m_Data->ComputePointDoppler(point.Position));
		}
	}

	float AudioScene::GetDoppler(UUID entity) const
	{
		Data::Source const* source = m_Data->Find(entity);
		return source != nullptr ? source->Doppler : 1.0f;
	}

	void AudioScene::UpdateReverbZones(std::span<AudioReverbZoneDesc const> zones)
	{
		ST_PROFILE_SCOPE("Audio.ReverbZones");
		// The zones that send, each with the scene's group of its bus. Assigned in place, so that the vector and each
		// element's bus name keep their memory from one update to the next.
		size_t count = 0;
		Mixer const& mixer = AudioEngine::GetMixer();
		for (AudioReverbZoneDesc const& zone : zones)
		{
			if (zone.Bus.empty() || !(zone.Level > 0.0f))
			{
				continue;
			}
			// Names compared without making keys, which this runs for every zone each frame.
			bool const hasBus = std::any_of(mixer.Buses.begin(), mixer.Buses.end(),
			                                [&zone](MixerBus const& bus)
			                                {
												return IsSameMixerBus(bus.Name, zone.Bus);
											});
			if (!hasBus)
			{
				// A group of a bus the mixer lacks plays on Master, where a send would only double the sound.
				if (m_Data->MissingZoneBuses.insert(MakeBusKey(zone.Bus)).second)
				{
					ST_CORE_WARN("A reverb zone sends to bus '{}', which the project's mixer lacks: it sends nothing", zone.Bus);
				}
				continue;
			}
			Result<AudioSceneGroup*> group = m_Data->GetGroup(zone.Bus);
			if (!group)
			{
				if (m_Data->MissingZoneBuses.insert(MakeBusKey(zone.Bus)).second)
				{
					ST_CORE_WARN("A reverb zone cannot send to bus '{}': {}", zone.Bus, group.GetError());
				}
				continue;
			}
			if (count == m_Data->Zones.size())
			{
				m_Data->Zones.emplace_back();
			}
			Data::ResolvedZone& resolved = m_Data->Zones[count++];
			resolved.Desc = zone;
			resolved.Group = group.GetValue();
		}
		m_Data->Zones.resize(count);

		for (auto const& [entity, source] : m_Data->Sources)
		{
			source->Sends = source->Spatial ? m_Data->ComputeSends(source->Position) : Data::VoiceSends{};
			if (source->Main)
			{
				m_Data->ApplySends(*source->Main, source->Sends);
			}
			for (Data::OneShot const& oneShot : source->OneShots)
			{
				m_Data->ApplySends(*oneShot.Voice, source->Sends);
			}
		}
		for (Data::PointVoice const& point : m_Data->PointVoices)
		{
			m_Data->ApplySends(*point.Voice, m_Data->ComputeSends(point.Position));
		}
	}

	float AudioScene::GetReverbSend(UUID entity, std::string_view bus) const
	{
		Data::Source const* source = m_Data->Find(entity);
		if (source == nullptr)
		{
			return 0.0f;
		}
		std::string const key = MakeBusKey(bus);
		for (uint32_t index = 0; index < source->Sends.Count; index++)
		{
			if (source->Sends.Sends[index].Group->GetBusKey() == key)
			{
				return source->Sends.Sends[index].Level;
			}
		}
		return 0.0f;
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
		desc.Doppler = source->Doppler;
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
		desc.Doppler = m_Data->ComputePointDoppler(position);
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
		// A stopped group does not pull from its voices, so they keep their positions. The groups the mixing thread holds
		// stop and start in one command, in the same block.
		bool held = false;
		for (auto const& [key, group] : m_Data->Groups)
		{
			held = group->SetPaused(paused) || held;
		}
		if (held)
		{
			AudioCommand command;
			command.Type = AudioCommandType::SetScenePaused;
			command.Serial = m_Data->Serial;
			command.Values[0] = paused ? 1.0f : 0.0f;
			Miniaudio::GetVoiceManager().Submit(command);
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
