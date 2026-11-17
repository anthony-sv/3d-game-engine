#include "stpch.h"
#include "Strada/Audio/AudioEngine.h"

#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Audio/AudioMixer.h"
#include "Strada/Audio/AudioQueue.h"
#include "Strada/Audio/AudioStream.h"
#include "Strada/Audio/AudioVoice.h"
#include "Strada/Audio/MiniaudioContext.h"
#include "Strada/Core/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <unordered_set>

namespace Strada
{
	namespace
	{
		using SteadyClock = std::chrono::steady_clock;

		struct AudioEngineData
		{
			AudioOutput Output = AudioOutput::Device;
			ma_context Context{};
			bool HasContext = false;
			ma_engine Engine{};
			bool HasDevice = false;
			AudioStreamFileOpener StreamFileOpener;
			// Null when the streaming thread could not be started.
			Scope<AudioStreamer> Streamer;

			// The queue and the return list between the main thread and the mixing thread, the mixer, and the main thread's
			// side of the mix. The queue's ring is 192 KiB, so it lives here on the heap.
			Scope<AudioCommandQueue> Queue;
			AudioReturnList Returns;
			Scope<AudioMixer> Mixer;
			Scope<AudioVoiceManager> Voices;
			AudioSampleCache Samples;
			float MasterVolume = 1.0f;
			std::unordered_set<std::string> MissingBuses;

			// A device that stopped: the main thread holds the mixer until the device runs again.
			bool TakenOver = false;
			SteadyClock::time_point LastRestart;
			uint64_t LastBlockCount = 0;
			SteadyClock::time_point LastBlockTime;
		};

		Scope<AudioEngineData> s_Data;

		// The profiler's state of the device's audio thread, kept here rather than in thread-local storage: on Apple
		// platforms a thread's first access to thread-local storage allocates, and the callback must not. Named by Init
		// before the device starts and ended by Shutdown once it has stopped; only the device's callback uses it between.
		constinit ProfilerThreadState s_MixerThread;
		// The thread that is mixing a block right now (none outside blocks): what IsMixingOnThisThread compares with. Beside
		// the profiler's state, in static storage, so that a hook may read it at any time. Not constinit, since
		// std::thread::id's constructor is not constexpr everywhere: static storage is zeroed before any code runs, and a
		// zero id is no thread's on every platform, so a read before its initialization finds no mixing thread.
		std::atomic<std::thread::id> s_MixingThread;
		// Allocations made through the engine's allocation callbacks on the mixing thread, while it mixed.
		constinit std::atomic<uint64_t> s_MixAllocations{0};
		// The mixer the device's callback renders through: set once everything it reads exists, before the device starts,
		// and cleared once the device has stopped.
		constinit std::atomic<AudioMixer*> s_Mixer{nullptr};
		// Set by the device's stopped notification unless the engine stopped it itself (s_StoppingDevice).
		constinit std::atomic<bool> s_DeviceLost{false};
		constinit std::atomic<bool> s_StoppingDevice{false};

		void CountMixAllocation()
		{
			if (AudioEngine::IsMixingOnThisThread())
			{
				s_MixAllocations.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// The engine's allocation callbacks: every object miniaudio makes for the module (the engine, its device, every sound
		// and group, which inherit them from the engine, miniaudio.h:78433) allocates through them.
		void* OnAllocate(size_t size, void* userData)
		{
			static_cast<void>(userData);
			CountMixAllocation();
			return std::malloc(size);
		}

		void* OnReallocate(void* memory, size_t size, void* userData)
		{
			static_cast<void>(userData);
			CountMixAllocation();
			return std::realloc(memory, size);
		}

		void OnFree(void* memory, void* userData)
		{
			static_cast<void>(userData);
			std::free(memory);
		}

		constexpr ma_allocation_callbacks AllocationCallbacks = {
			.pUserData = nullptr,
			.onMalloc = OnAllocate,
			.onRealloc = OnReallocate,
			.onFree = OnFree,
		};

		// The device's data callback, on miniaudio's audio thread: the mixer's render, inside the profiler's zone of the mix.
		// Neither allocates nor locks, and reaches no thread-local storage: no zone inside the render uses the thread's own
		// profiler state.
		void MixIntoDevice(ma_device* device, void* output, void const* input, ma_uint32 frameCount)
		{
			static_cast<void>(input);
			ST_PROFILE_SCOPE_ON(s_MixerThread, "Audio.Mix");
			if (AudioMixer* const mixer = s_Mixer.load(std::memory_order_acquire))
			{
				mixer->Render(static_cast<float*>(output), frameCount);
			}
			else
			{
				std::fill_n(static_cast<float*>(output), static_cast<size_t>(frameCount) * device->playback.channels, 0.0f);
			}
		}

		// On the device's thread: a stop the engine did not ask for is a lost device.
		void OnDeviceNotification(ma_device_notification const* notification)
		{
			if (notification->type == ma_device_notification_type_stopped && !s_StoppingDevice.load(std::memory_order_acquire))
			{
				s_DeviceLost.store(true, std::memory_order_release);
			}
		}

		void StopDevice(ma_device& device)
		{
			s_StoppingDevice.store(true, std::memory_order_release);
			ma_device_stop(&device);
			s_StoppingDevice.store(false, std::memory_order_release);
		}

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

		std::string ToLowerAscii(std::string_view text)
		{
			std::string lower(text);
			std::transform(lower.begin(), lower.end(), lower.begin(),
			               [](char character)
			               {
							   return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
						   });
			return lower;
		}

		// The engine's half of a frame on the main thread: what came back, what ended, and the limit.
		void SyncVoices(AudioEngineData& data)
		{
			data.Voices->TakeReturns(data.Returns);
			data.Voices->Refresh();
		}

		// Device and null output: the device stopped (it said so, or a playback device mixed nothing for
		// AudioMixerStallSeconds). Until it runs again, the main thread applies the queue and takes back what the mixer lets
		// go, so that nothing waits on a device that no longer calls.
		void UpdateDevice(AudioEngineData& data)
		{
			ma_device* const device = ma_engine_get_device(&data.Engine);
			if (device == nullptr)
			{
				return;
			}
			SteadyClock::time_point const now = SteadyClock::now();
			if (!data.TakenOver)
			{
				uint64_t const blocks = data.Mixer->GetBlockCount();
				if (blocks != data.LastBlockCount)
				{
					data.LastBlockCount = blocks;
					data.LastBlockTime = now;
				}
				bool const stalled =
					data.HasDevice && std::chrono::duration<float>(now - data.LastBlockTime).count() >= AudioMixerStallSeconds;
				if (!s_DeviceLost.load(std::memory_order_acquire) && !stalled)
				{
					return;
				}
				// The device's thread has left its callback once stop returns (it waits for it).
				StopDevice(*device);
				s_DeviceLost.store(false, std::memory_order_release);
				data.TakenOver = true;
				data.LastRestart = now;
				ST_CORE_WARN("The audio device stopped; sounds go on silently until it runs again");
			}
			data.Mixer->Drain();
			data.Voices->TakeReturns(data.Returns);
			if (std::chrono::duration<float>(now - data.LastRestart).count() < AudioDeviceRetrySeconds)
			{
				return;
			}
			data.LastRestart = now;
			// Nothing of the main thread touches the mixer once the device runs again.
			if (ma_device_start(device) == MA_SUCCESS)
			{
				data.TakenOver = false;
				data.LastBlockCount = data.Mixer->GetBlockCount();
				data.LastBlockTime = now;
				ST_CORE_INFO("The audio device runs again");
			}
		}
	}

	AudioOutput AudioEngine::ResolveOutput(bool headless, std::optional<std::string> const& variable)
	{
		if (headless || !variable || variable->empty())
		{
			return headless ? AudioOutput::Null : AudioOutput::Device;
		}
		std::string const value = ToLowerAscii(*variable);
		if (value == "device")
		{
			return AudioOutput::Device;
		}
		if (value == "null")
		{
			return AudioOutput::Null;
		}
		ST_CORE_WARN("Ignoring unknown {} value '{}' (expected 'device' or 'null')", AudioOutputVariable, *variable);
		return AudioOutput::Device;
	}

	void AudioEngine::Init(AudioEngineSettings const& settings)
	{
		ST_CORE_ASSERT(!s_Data, "AudioEngine is already initialized");

		// miniaudio keeps pointers into the engine and the context: they must not move once initialized.
		Scope<AudioEngineData> data = CreateScope<AudioEngineData>();
		data->Output = settings.Output;
		s_MixAllocations.store(0, std::memory_order_relaxed);
		s_DeviceLost.store(false, std::memory_order_relaxed);
		// Before the device starts, whose thread then reads it.
		Profiler::SetThreadName(s_MixerThread, "Audio Mixer");
		ma_engine_config config = ma_engine_config_init();
		config.allocationCallbacks = AllocationCallbacks;
		switch (settings.Output)
		{
			case AudioOutput::Device:
				// miniaudio tries every backend of the platform and ends with the null backend.
				config.dataCallback = MixIntoDevice;
				config.notificationCallback = OnDeviceNotification;
				config.noAutoStart = MA_TRUE;
				break;
			case AudioOutput::Null:
			{
				ma_backend const backend = ma_backend_null;
				ma_context_config contextConfig = ma_context_config_init();
				contextConfig.allocationCallbacks = AllocationCallbacks;
				if (ma_result const result = ma_context_init(&backend, 1, &contextConfig, &data->Context); result != MA_SUCCESS)
				{
					ST_CORE_ERROR("The audio engine cannot be initialized: {}", ma_result_description(result));
					return;
				}
				data->HasContext = true;
				config.pContext = &data->Context;
				config.dataCallback = MixIntoDevice;
				config.notificationCallback = OnDeviceNotification;
				config.noAutoStart = MA_TRUE;
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
		data->StreamFileOpener = settings.StreamFileOpener;
		if (Result<Scope<AudioStreamer>> streamer = AudioStreamer::Create(); streamer)
		{
			data->Streamer = streamer.TakeValue();
		}
		else
		{
			ST_CORE_ERROR("Sounds cannot stream from their files: {}", streamer.GetError());
		}

		data->Queue = CreateScope<AudioCommandQueue>();
		data->Mixer = CreateScope<AudioMixer>(data->Engine, *data->Queue, data->Returns);
		data->Voices = CreateScope<AudioVoiceManager>(*data->Queue);
		data->LastBlockTime = SteadyClock::now();
		s_Data = std::move(data);
		if (device != nullptr)
		{
			s_Mixer.store(s_Data->Mixer.get(), std::memory_order_release);
			if (ma_result const result = ma_device_start(device); result != MA_SUCCESS)
			{
				// Taken for a lost device: the next Update holds the mixer and tries again.
				ST_CORE_ERROR("The audio device cannot be started: {}", ma_result_description(result));
				s_DeviceLost.store(true, std::memory_order_release);
			}
		}
		ST_CORE_INFO("Audio initialized (miniaudio {}, {} output, {} backend, {} Hz, {} channels)", MA_VERSION_STRING,
		             AudioOutputToString(settings.Output), backend, GetSampleRate(), GetChannelCount());
	}

	void AudioEngine::Shutdown()
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		// No block runs once the device has stopped (ma_device_stop waits for a callback in progress), so the main thread
		// owns every object again.
		if (ma_device* const device = ma_engine_get_device(&s_Data->Engine))
		{
			StopDevice(*device);
		}
		s_Mixer.store(nullptr, std::memory_order_release);
		s_Data->Voices->TakeReturns(s_Data->Returns);
		s_Data->Voices->DestroyReturning();
		s_Data->Voices.reset();
		s_Data->Mixer.reset();
		s_Data->Queue.reset();
		// The streams are gone with the voices; the thread ends before the engine it streams for.
		s_Data->Streamer.reset();
		s_StoppingDevice.store(true, std::memory_order_release);
		ma_engine_uninit(&s_Data->Engine);
		s_StoppingDevice.store(false, std::memory_order_release);
		// The device has stopped: its thread's buffer is free again at the profiler's next collection.
		Profiler::EndThread(s_MixerThread);
		if (s_Data->HasContext)
		{
			ma_context_uninit(&s_Data->Context);
		}
		s_Data.reset();
		s_DeviceLost.store(false, std::memory_order_relaxed);
	}

	bool AudioEngine::IsInitialized()
	{
		return s_Data != nullptr;
	}

	void AudioEngine::Update()
	{
		if (s_Data == nullptr)
		{
			return;
		}
		ST_PROFILE_SCOPE("Audio.Update");
		AudioEngineData& data = *s_Data;
		SyncVoices(data);
		if (data.Voices->HasVirtualVoices())
		{
			data.Voices->Balance();
		}
		data.Samples.Prune();
		if (data.Output != AudioOutput::Manual)
		{
			UpdateDevice(data);
		}
		data.Queue->Flush();
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
		return s_Data != nullptr ? s_Data->MasterVolume : 0.0f;
	}

	void AudioEngine::SetMasterVolume(float volume)
	{
		if (s_Data == nullptr)
		{
			return;
		}
		// Also replaces NaN.
		volume = volume > 0.0f ? volume : 0.0f;
		if (volume == s_Data->MasterVolume)
		{
			return;
		}
		s_Data->MasterVolume = volume;
		AudioCommand command;
		command.Type = AudioCommandType::SetMasterVolume;
		command.Values[0] = volume;
		s_Data->Queue->Push(command);
	}

	uint32_t AudioEngine::GetActiveVoiceCount()
	{
		return s_Data != nullptr ? s_Data->Voices->GetActiveCount() : 0;
	}

	uint32_t AudioEngine::GetMixedVoiceCount()
	{
		return s_Data != nullptr ? s_Data->Mixer->GetMixedVoiceCount() : 0;
	}

	uint64_t AudioEngine::GetStolenVoiceCount()
	{
		return s_Data != nullptr ? s_Data->Voices->GetStolenCount() : 0;
	}

	size_t AudioEngine::GetDeferredCommandCount()
	{
		return s_Data != nullptr ? s_Data->Queue->GetDeferredCount() : 0;
	}

	Result<void> AudioEngine::PreloadClip(Ref<AudioClipAsset> const& clip)
	{
		if (s_Data == nullptr)
		{
			return Error{"the audio engine is not initialized"};
		}
		if (!clip)
		{
			return Error{"no clip to preload"};
		}
		if (Result<Ref<AudioSampleBuffer>> samples = s_Data->Samples.Get(clip); !samples)
		{
			return Error{samples.GetError()};
		}
		return {};
	}

	uint64_t AudioEngine::GetDecodedClipCount()
	{
		return s_Data != nullptr ? s_Data->Samples.GetDecodeCount() : 0;
	}

	size_t AudioEngine::GetReturningObjectCount()
	{
		return s_Data != nullptr ? s_Data->Voices->GetReturningCount() : 0;
	}

	uint64_t AudioEngine::GetBlockCount()
	{
		return s_Data != nullptr ? s_Data->Mixer->GetBlockCount() : 0;
	}

	bool AudioEngine::IsMixingOnThisThread()
	{
		return s_MixingThread.load(std::memory_order_relaxed) == std::this_thread::get_id();
	}

	uint64_t AudioEngine::GetMixAllocationCount()
	{
		return s_MixAllocations.load(std::memory_order_relaxed);
	}

	void AudioEngine::SimulateDeviceLoss()
	{
		if (s_Data == nullptr)
		{
			return;
		}
		if (ma_device* const device = ma_engine_get_device(&s_Data->Engine))
		{
			// Not through StopDevice: the stopped notification takes it for a device that went away.
			ma_device_stop(device);
		}
	}

	bool AudioEngine::IsMixerTakenOver()
	{
		return s_Data != nullptr && s_Data->TakenOver;
	}

	uint64_t AudioEngine::ReadFrames(std::span<float> samples)
	{
		ST_CORE_ASSERT(s_Data && s_Data->Output == AudioOutput::Manual, "AudioEngine::ReadFrames needs manual output");
		// Device output is read by the audio thread; a second reader would race it.
		if (s_Data == nullptr || s_Data->Output != AudioOutput::Manual)
		{
			return 0;
		}
		uint32_t const channelCount = ma_engine_get_channels(&s_Data->Engine);
		uint64_t const frameCount = samples.size() / channelCount;
		if (frameCount == 0)
		{
			return 0;
		}
		ST_PROFILE_SCOPE("Audio.Mix");
		AudioEngineData& data = *s_Data;
		for (uint64_t done = 0; done < frameCount;)
		{
			uint32_t const frames = static_cast<uint32_t>(std::min<uint64_t>(AudioBlockFrames, frameCount - done));
			// Outside the block: the deferred commands' memory is the main thread's.
			data.Queue->Flush();
			data.Mixer->Render(samples.data() + done * channelCount, frames);
			done += frames;
		}
		SyncVoices(data);
		data.Queue->Flush();
		return frameCount;
	}

	void AudioEngine::WaitForStreams()
	{
		if (s_Data != nullptr && s_Data->Streamer)
		{
			s_Data->Streamer->WaitUntilIdle();
		}
	}

	ma_engine& Miniaudio::GetEngine()
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		return s_Data->Engine;
	}

	AudioStreamer* Miniaudio::GetStreamer()
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		return s_Data->Streamer.get();
	}

	AudioStreamFileOpener const& Miniaudio::GetStreamFileOpener()
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		return s_Data->StreamFileOpener;
	}

	AudioVoiceManager& Miniaudio::GetVoiceManager()
	{
		ST_CORE_ASSERT(s_Data && s_Data->Voices, "AudioEngine is not initialized");
		return *s_Data->Voices;
	}

	AudioSampleCache& Miniaudio::GetSampleCache()
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		return s_Data->Samples;
	}

	AudioBusNode* Miniaudio::FindBus(std::string_view busKey)
	{
		ST_CORE_ASSERT(s_Data, "AudioEngine is not initialized");
		// Until the mixer has buses there is Master alone, the engine's endpoint.
		if (busKey != "master" && s_Data->MissingBuses.emplace(busKey).second)
		{
			ST_CORE_WARN("The mixer has no bus named '{}': its sounds play on {}", busKey, MasterBusName);
		}
		return nullptr;
	}

	Miniaudio::MixingScope::MixingScope()
	{
		s_MixingThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
	}

	Miniaudio::MixingScope::~MixingScope()
	{
		s_MixingThread.store(std::thread::id(), std::memory_order_relaxed);
	}
}
