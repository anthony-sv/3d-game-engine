#include "stpch.h"
#include "Strada/Audio/AudioScene.h"

#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/MiniaudioContext.h"

#include <extras/decoders/libvorbis/miniaudio_libvorbis.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace Strada
{
	namespace
	{
		ma_encoding_format ToEncodingFormat(AudioFormat format)
		{
			switch (format)
			{
				case AudioFormat::Wav:
					return ma_encoding_format_wav;
				case AudioFormat::Flac:
					return ma_encoding_format_flac;
				case AudioFormat::Mp3:
					return ma_encoding_format_mp3;
				case AudioFormat::Ogg:
					// libvorbis decodes it: miniaudio offers custom backends the data whose encoding it is not told.
				case AudioFormat::Unknown:
					break;
			}
			return ma_encoding_format_unknown;
		}

		bool IsFinite(glm::vec3 const& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// A clip played through its own decoder, so every sound has its own read position.
		struct Sound
		{
			Ref<AudioClipAsset> Clip;
			ma_decoder Decoder{};
			ma_sound Handle{};
			bool HasDecoder = false;
			bool HasHandle = false;
			bool Spatial = true;
			glm::vec3 Position = glm::vec3(0.0f);

			Sound() = default;
			~Sound()
			{
				// The sound reads from the decoder, which reads from the clip's data.
				if (HasHandle)
				{
					ma_sound_uninit(&Handle);
				}
				if (HasDecoder)
				{
					ma_decoder_uninit(&Decoder);
				}
			}

			Sound(Sound const&) = delete;
			Sound& operator=(Sound const&) = delete;
		};
	}

	struct AudioScene::Data
	{
		// Every sound of the scene plays through this group, which pauses them together.
		ma_sound_group Group{};
		bool HasGroup = false;
		std::unordered_map<UUID, Scope<Sound>> Sounds;
		glm::vec3 ListenerPosition = glm::vec3(0.0f);
		// Columns: the listener's right, up and back directions (it faces -Z of its basis, like miniaudio's listener).
		glm::mat3 ListenerBasis = glm::mat3(1.0f);
		bool Paused = false;

		Data() = default;
		~Data()
		{
			Sounds.clear();
			if (HasGroup)
			{
				ma_sound_group_uninit(&Group);
			}
		}

		Data(Data const&) = delete;
		Data& operator=(Data const&) = delete;

		Sound* Find(UUID entity) const
		{
			auto const it = Sounds.find(entity);
			return it != Sounds.end() ? it->second.get() : nullptr;
		}

		// Sounds are positioned relative to the scene's listener, so scenes never move each other's listener.
		void UpdatePosition(Sound& sound) const
		{
			glm::vec3 const relative = glm::transpose(ListenerBasis) * (sound.Position - ListenerPosition);
			ma_sound_set_position(&sound.Handle, relative.x, relative.y, relative.z);
		}
	};

	Result<Scope<AudioScene>> AudioScene::Create()
	{
		if (!AudioEngine::IsInitialized())
		{
			return Error{"the audio engine is not initialized"};
		}
		Scope<Data> data = CreateScope<Data>();
		// The group only mixes: without pitch and spatialization it adds no resampling delay and no second panning.
		ma_uint32 const groupFlags = MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION;
		if (ma_result const result = ma_sound_group_init(&Miniaudio::GetEngine(), groupFlags, nullptr, &data->Group); result != MA_SUCCESS)
		{
			return MakeError("the scene's sound group cannot be created: {}", ma_result_description(result));
		}
		data->HasGroup = true;
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
		if (!desc.Clip)
		{
			return Error{"the audio source has no clip"};
		}

		Scope<Sound> sound = CreateScope<Sound>();
		sound->Clip = desc.Clip;
		std::span<uint8_t const> const data = sound->Clip->GetData();
		ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, 0);
		decoderConfig.encodingFormat = ToEncodingFormat(sound->Clip->GetFormat());
		// Read only while the decoder initializes.
		std::array<ma_decoding_backend_vtable*, 1> customBackends = {ma_decoding_backend_libvorbis};
		decoderConfig.ppCustomBackendVTables = customBackends.data();
		decoderConfig.customBackendCount = static_cast<ma_uint32>(customBackends.size());
		if (ma_result const result = ma_decoder_init_memory(data.data(), data.size(), &decoderConfig, &sound->Decoder);
		    result != MA_SUCCESS)
		{
			return MakeError("the {} clip cannot be decoded: {}", AudioFormatToString(sound->Clip->GetFormat()),
			                 ma_result_description(result));
		}
		sound->HasDecoder = true;
		if (ma_result const result =
		        ma_sound_init_from_data_source(&Miniaudio::GetEngine(), &sound->Decoder, 0, &m_Data->Group, &sound->Handle);
		    result != MA_SUCCESS)
		{
			return MakeError("the sound cannot be created: {}", ma_result_description(result));
		}
		sound->HasHandle = true;

		ma_sound_set_positioning(&sound->Handle, ma_positioning_relative);
		ma_sound_set_attenuation_model(&sound->Handle, ma_attenuation_model_inverse);
		// Sources and the listener have no velocities.
		ma_sound_set_doppler_factor(&sound->Handle, 0.0f);

		UUID const entity = desc.Entity;
		m_Data->Sounds.insert_or_assign(entity, std::move(sound));
		SetVolume(entity, desc.Volume);
		SetPitch(entity, desc.Pitch);
		SetLooping(entity, desc.Loop);
		SetDistances(entity, desc.MinDistance, desc.MaxDistance);
		SetSpatial(entity, desc.Spatial);
		SetPosition(entity, desc.Position);
		return {};
	}

	void AudioScene::RemoveSource(UUID entity)
	{
		m_Data->Sounds.erase(entity);
	}

	bool AudioScene::HasSource(UUID entity) const
	{
		return m_Data->Sounds.contains(entity);
	}

	size_t AudioScene::GetSourceCount() const
	{
		return m_Data->Sounds.size();
	}

	void AudioScene::Play(UUID entity)
	{
		if (Sound* sound = m_Data->Find(entity))
		{
			// A sound that reached its end stays started until the mixer next visits it, and starting a started sound
			// does nothing: stop it first so it starts over now instead of being stopped by the mixer.
			if (ma_sound_at_end(&sound->Handle))
			{
				ma_sound_stop(&sound->Handle);
			}
			ma_sound_start(&sound->Handle);
		}
	}

	void AudioScene::Pause(UUID entity)
	{
		if (Sound* sound = m_Data->Find(entity))
		{
			ma_sound_stop(&sound->Handle);
		}
	}

	void AudioScene::Stop(UUID entity)
	{
		if (Sound* sound = m_Data->Find(entity))
		{
			ma_sound_stop(&sound->Handle);
			// Applied by the mixer when the sound plays again.
			ma_sound_seek_to_pcm_frame(&sound->Handle, 0);
		}
	}

	bool AudioScene::IsPlaying(UUID entity) const
	{
		Sound const* sound = m_Data->Find(entity);
		return sound != nullptr && ma_sound_is_playing(&sound->Handle) && !ma_sound_at_end(&sound->Handle);
	}

	float AudioScene::GetVolume(UUID entity) const
	{
		Sound const* sound = m_Data->Find(entity);
		return sound != nullptr ? ma_sound_get_volume(&sound->Handle) : 0.0f;
	}

	void AudioScene::SetVolume(UUID entity, float volume)
	{
		if (Sound* sound = m_Data->Find(entity))
		{
			ma_sound_set_volume(&sound->Handle, std::max(volume, 0.0f));
		}
	}

	float AudioScene::GetPitch(UUID entity) const
	{
		Sound const* sound = m_Data->Find(entity);
		return sound != nullptr ? ma_sound_get_pitch(&sound->Handle) : 0.0f;
	}

	void AudioScene::SetPitch(UUID entity, float pitch)
	{
		if (Sound* sound = m_Data->Find(entity))
		{
			// Also replaces NaN.
			ma_sound_set_pitch(&sound->Handle, pitch >= MinAudioPitch ? pitch : MinAudioPitch);
		}
	}

	bool AudioScene::IsLooping(UUID entity) const
	{
		Sound const* sound = m_Data->Find(entity);
		return sound != nullptr && ma_sound_is_looping(&sound->Handle);
	}

	void AudioScene::SetLooping(UUID entity, bool loop)
	{
		if (Sound* sound = m_Data->Find(entity))
		{
			ma_sound_set_looping(&sound->Handle, loop ? MA_TRUE : MA_FALSE);
		}
	}

	bool AudioScene::IsSpatial(UUID entity) const
	{
		Sound const* sound = m_Data->Find(entity);
		return sound != nullptr && sound->Spatial;
	}

	void AudioScene::SetSpatial(UUID entity, bool spatial)
	{
		if (Sound* sound = m_Data->Find(entity))
		{
			sound->Spatial = spatial;
			ma_sound_set_spatialization_enabled(&sound->Handle, spatial ? MA_TRUE : MA_FALSE);
		}
	}

	void AudioScene::SetDistances(UUID entity, float minDistance, float maxDistance)
	{
		if (Sound* sound = m_Data->Find(entity))
		{
			// A zero minimum distance would divide zero by zero next to the listener.
			float const nearest = minDistance >= MinAudioDistance ? minDistance : MinAudioDistance;
			float const farthest = maxDistance >= nearest ? maxDistance : nearest;
			ma_sound_set_min_distance(&sound->Handle, nearest);
			ma_sound_set_max_distance(&sound->Handle, farthest);
		}
	}

	glm::vec3 AudioScene::GetPosition(UUID entity) const
	{
		Sound const* sound = m_Data->Find(entity);
		return sound != nullptr ? sound->Position : glm::vec3(0.0f);
	}

	void AudioScene::SetPosition(UUID entity, glm::vec3 const& position)
	{
		Sound* sound = m_Data->Find(entity);
		if (sound != nullptr && IsFinite(position))
		{
			sound->Position = position;
			m_Data->UpdatePosition(*sound);
		}
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
		for (auto const& [entity, sound] : m_Data->Sounds)
		{
			m_Data->UpdatePosition(*sound);
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
		// A stopped group does not pull from its sounds, so they keep their positions.
		if (paused)
		{
			ma_sound_group_stop(&m_Data->Group);
		}
		else
		{
			ma_sound_group_start(&m_Data->Group);
		}
	}

	bool AudioScene::IsPaused() const
	{
		return m_Data->Paused;
	}
}
