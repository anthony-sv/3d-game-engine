#pragma once

#include "Strada/Audio/AudioStreamFile.h"

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

	// How far ahead of their playback streamed sounds are read and decoded: the longest a read of their file can stall
	// before they fall silent.
	inline constexpr float AudioStreamBufferSeconds = 1.0f;
	// Creating a streamed sound reads at most this much of its file (its format and headers) on the main thread: files
	// whose audio starts later, as in damaged files, are refused instead of being read on.
	inline constexpr uint64_t MaxAudioStreamHeaderBytes = 4 * 1024 * 1024;

	struct AudioEngineSettings
	{
		AudioOutput Output = AudioOutput::Device;
		// Mix format of manual output; device and null output mix in the device's format.
		uint32_t SampleRate = 48000;
		uint32_t ChannelCount = 2;
		// Opens the files sounds stream from: the file system (AudioStreamFile::Open) when empty.
		AudioStreamFileOpener StreamFileOpener;
	};

	// The global audio engine (miniaudio): mixes the sounds of every AudioScene into one output, on miniaudio's audio
	// thread unless the output is manual. Streamed sounds are read and decoded ahead on the audio streaming thread, so the
	// mix never waits for a file. Initialized by Application after the PhysicsSystem. Main thread only.
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
		// at the end is left untouched) and returns the number of frames written. Never waits for streamed sounds: one whose
		// file has not been read far enough is silent until it has.
		static uint64_t ReadFrames(std::span<float> samples);
		// Waits until every streamed sound has read and decoded as far ahead as it can (AudioStreamBufferSeconds, or to a
		// file that cannot be read further) and has carried out its seeks. With manual output the next ReadFrames, up to
		// that long, then plays streamed sounds without gaps, as offline capture and tests need.
		static void WaitForStreams();
	};
}
