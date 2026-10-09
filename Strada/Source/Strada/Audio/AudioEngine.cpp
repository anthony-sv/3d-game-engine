#include "stpch.h"
#include "Strada/Audio/AudioEngine.h"

#include "Strada/Audio/MiniaudioContext.h"

#include <algorithm>

namespace Strada
{
	namespace
	{
		struct AudioEngineData
		{
			AudioOutput Output = AudioOutput::Device;
			ma_context Context{};
			bool HasContext = false;
			ma_engine Engine{};
			bool HasDevice = false;
		};

		Scope<AudioEngineData> s_Data;

		char const* AudioOutputToString(AudioOutput output)
		{
			switch (output)
			{
				case AudioOutput::Device:
					return "device";
				case AudioOutput::Null:
					return "null device";
				case AudioOutput::Manual:
					return "manual";
			}
			return "unknown";
		}
	}

	void AudioEngine::Init(AudioEngineSettings const& settings)
	{
		ST_CORE_ASSERT(!s_Data, "AudioEngine is already initialized");

		// miniaudio keeps pointers into the engine and the context: they must not move once initialized.
		Scope<AudioEngineData> data = CreateScope<AudioEngineData>();
		data->Output = settings.Output;
		ma_engine_config config = ma_engine_config_init();
		switch (settings.Output)
		{
			case AudioOutput::Device:
				// miniaudio tries every backend of the platform and ends with the null backend.
				break;
			case AudioOutput::Null:
			{
				ma_backend const backend = ma_backend_null;
				if (ma_result const result = ma_context_init(&backend, 1, nullptr, &data->Context); result != MA_SUCCESS)
				{
					ST_CORE_ERROR("The audio engine cannot be initialized: {}", ma_result_description(result));
					return;
				}
				data->HasContext = true;
				config.pContext = &data->Context;
				break;
			}
			case AudioOutput::Manual:
				if (settings.ChannelCount < static_cast<uint32_t>(MA_MIN_CHANNELS) ||
				    settings.ChannelCount > static_cast<uint32_t>(MA_MAX_CHANNELS) ||
				    settings.SampleRate < static_cast<uint32_t>(ma_standard_sample_rate_min) ||
				    settings.SampleRate > static_cast<uint32_t>(ma_standard_sample_rate_max))
				{
					ST_CORE_ERROR("The audio engine cannot be initialized: {} channels at {} Hz is not a supported format",
					              settings.ChannelCount, settings.SampleRate);
					return;
				}
				config.noDevice = MA_TRUE;
				config.channels = settings.ChannelCount;
				config.sampleRate = settings.SampleRate;
				break;
		}

		if (ma_result const result = ma_engine_init(&config, &data->Engine); result != MA_SUCCESS)
		{
			ST_CORE_ERROR("The audio engine cannot be initialized: {}", ma_result_description(result));
			if (data->HasContext)
			{
				ma_context_uninit(&data->Context);
			}
			return;
		}

		ma_device* const device = ma_engine_get_device(&data->Engine);
		char const* backend = "none";
		if (device != nullptr)
		{
			ma_backend const deviceBackend = ma_device_get_context(device)->backend;
			backend = ma_get_backend_name(deviceBackend);
			data->HasDevice = deviceBackend != ma_backend_null;
		}
		if (settings.Output == AudioOutput::Device && !data->HasDevice)
		{
			ST_CORE_WARN("No audio device is available; sounds play silently");
		}

		s_Data = std::move(data);
		ST_CORE_INFO("Audio initialized (miniaudio {}, {} output, {} backend, {} Hz, {} channels)", MA_VERSION_STRING,
		             AudioOutputToString(settings.Output), backend, GetSampleRate(), GetChannelCount());
	}

	void AudioEngine::Shutdown()
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		ma_engine_uninit(&s_Data->Engine);
		if (s_Data->HasContext)
		{
			ma_context_uninit(&s_Data->Context);
		}
		s_Data.reset();
	}

	bool AudioEngine::IsInitialized()
	{
		return s_Data != nullptr;
	}

	AudioOutput AudioEngine::GetOutput()
	{
		return s_Data != nullptr ? s_Data->Output : AudioOutput::Null;
	}

	bool AudioEngine::HasDevice()
	{
		return s_Data != nullptr && s_Data->HasDevice;
	}

	uint32_t AudioEngine::GetSampleRate()
	{
		return s_Data != nullptr ? ma_engine_get_sample_rate(&s_Data->Engine) : 0;
	}

	uint32_t AudioEngine::GetChannelCount()
	{
		return s_Data != nullptr ? ma_engine_get_channels(&s_Data->Engine) : 0;
	}

	float AudioEngine::GetMasterVolume()
	{
		return s_Data != nullptr ? ma_engine_get_volume(&s_Data->Engine) : 0.0f;
	}

	void AudioEngine::SetMasterVolume(float volume)
	{
		if (s_Data != nullptr)
		{
			ma_engine_set_volume(&s_Data->Engine, std::max(volume, 0.0f));
		}
	}

	uint64_t AudioEngine::ReadFrames(std::span<float> samples)
	{
		ST_CORE_ASSERT(s_Data && s_Data->Output == AudioOutput::Manual, "AudioEngine::ReadFrames needs manual output");
		// Device output is read by the audio thread; a second reader would race it.
		if (s_Data == nullptr || s_Data->Output != AudioOutput::Manual)
		{
			return 0;
		}
		uint64_t const frameCount = samples.size() / ma_engine_get_channels(&s_Data->Engine);
		if (frameCount == 0)
		{
			return 0;
		}
		// The node graph fills every requested frame, with silence where nothing plays; with no sound at all it reports
		// its end instead of success.
		ma_result const result = ma_engine_read_pcm_frames(&s_Data->Engine, samples.data(), frameCount, nullptr);
		return result == MA_SUCCESS || result == MA_AT_END ? frameCount : 0;
	}

	ma_engine& Miniaudio::GetEngine()
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		return s_Data->Engine;
	}
}
