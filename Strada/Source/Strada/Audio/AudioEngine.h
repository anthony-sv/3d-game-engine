#pragma once

#include <cstdint>
#include <span>

namespace Strada
{
	enum class AudioOutput : uint8_t
	{
		// The default playback device; the null device when none is available.
		Device,
		// The null device: sounds advance in real time without being heard (headless runs).
		Null,
		// No device: the mix advances only when read with AudioEngine::ReadFrames (tests, offline capture).
		Manual
	};

	struct AudioEngineSettings
	{
		AudioOutput Output = AudioOutput::Device;
		// Mix format of manual output; device and null output mix in the device's format.
		uint32_t SampleRate = 48000;
		uint32_t ChannelCount = 2;
	};

	// The global audio engine (miniaudio): mixes the sounds of every AudioScene into one output, on miniaudio's audio
	// thread unless the output is manual. Initialized by Application after the PhysicsSystem. Main thread only.
	class AudioEngine
	{
	public:
		// When the output cannot be opened the failure is logged and the engine stays uninitialized (no audio).
		static void Init(AudioEngineSettings const& settings = {});
		static void Shutdown();
		static bool IsInitialized();

		// Null while uninitialized.
		static AudioOutput GetOutput();
		// Whether sounds reach a playback device (not the null device or manual output).
		static bool HasDevice();
		static uint32_t GetSampleRate();
		static uint32_t GetChannelCount();

		// Linear gain applied to everything (1 = unchanged).
		static float GetMasterVolume();
		static void SetMasterVolume(float volume);

		// Manual output only: mixes the next frames into interleaved samples (GetChannelCount() per frame; a partial frame
		// at the end is left untouched) and returns the number of frames written.
		static uint64_t ReadFrames(std::span<float> samples);
	};
}
