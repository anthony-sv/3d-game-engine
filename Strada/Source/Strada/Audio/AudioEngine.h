#pragma once

#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioStreamFile.h"

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Strada
{
	class AudioClipAsset;

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

	// The mix is rendered in blocks of at most this many frames, and the queue is applied at the start of each: 5.3 ms at
	// 48 kHz, the longest a command waits once it is in the ring. Every ReadFrames and every device callback starts a new
	// block, so a change made before a read is heard from that read's first frame.
	inline constexpr uint32_t AudioBlockFrames = 256;
	// Gains ramp linearly in amplitude over this: a voice's volume (miniaudio's volume smoothing,
	// ma_sound_config::volumeSmoothTimeInPCMFrames, miniaudio.h:11222), a bus's volumes and mutes, the master volume, the
	// dips of moves and swaps (and MX2's sends). A value set before a voice's first block is taken at once.
	inline constexpr float AudioGainSmoothingSeconds = 0.005f;
	// Pitch and effect parameters reach a new value over this, on their scales: pitch on a log2 scale, stepped once a block;
	// effect parameters as the effects say (Docs/Architecture.md §9.6).
	inline constexpr float AudioParameterSmoothingSeconds = 0.02f;
	// A playback device (never the null device) that has mixed no block for this long, without telling that it stopped,
	// is taken for lost: the main thread takes the mixer over (§9.1). Long beside any scheduling delay of a busy machine.
	inline constexpr float AudioMixerStallSeconds = 5.0f;
	// While the main thread holds the mixer of a lost device, it tries to start the device again this often.
	inline constexpr float AudioDeviceRetrySeconds = 1.0f;

	// The environment variable that chooses a windowed application's output: "device" (the default) or "null", so that a
	// capture or a session started by a tool plays nothing through the machine's speakers. Headless applications use the
	// null device whatever it says (AudioEngine::ResolveOutput).
	inline constexpr char const* AudioOutputVariable = "STRADA_AUDIO_OUTPUT";

	struct AudioEngineSettings
	{
		AudioOutput Output = AudioOutput::Device;
		// Mix format of manual output; device and null output mix in the device's format.
		uint32_t SampleRate = 48000;
		uint32_t ChannelCount = 2;
		// Opens the files sounds stream from: the file system (AudioStreamFile::Open) when empty.
		AudioStreamFileOpener StreamFileOpener;
	};

	// The global audio engine (miniaudio): mixes the voices of every AudioScene through the mixer's buses into one output,
	// on miniaudio's audio thread unless the output is manual (Docs/Architecture.md §9). The main thread changes the mix
	// only through the audio queue, which the thread that mixes applies at the start of each block; streamed sounds are read
	// and decoded ahead on the audio streaming thread, so the mix never waits for a file. Initialized by Application after
	// the PhysicsSystem. Main thread only, except where a function says otherwise.
	class AudioEngine
	{
	public:
		// The output an application uses (Application calls it with the value of AudioOutputVariable): the null device when
		// headless, whatever the value; otherwise "device" or "null" in any letter case, unset or empty meaning "device". An
		// unknown value is logged and the device kept. Pure: it opens nothing. Any thread.
		static AudioOutput ResolveOutput(bool headless, std::optional<std::string> const& variable);

		// When the output cannot be opened the failure is logged and the engine stays uninitialized (no audio).
		static void Init(AudioEngineSettings const& settings = {});
		// Stops the device first, so that no block runs any more, then destroys every object the mixer held or was
		// returning. Every AudioScene must be destroyed before.
		static void Shutdown();
		static bool IsInitialized();

		// Once a frame (Application calls it; zone Audio.Update): destroys what the mixer handed back, retires the one-shots
		// that ended, starts virtual voices again while there is room, moves deferred commands into the queue, and takes
		// the mixer over from a device that stopped (§9.1): one that said so, or a playback device that has mixed nothing for
		// AudioMixerStallSeconds.
		static void Update();

		// Null while uninitialized.
		static AudioOutput GetOutput();
		// Whether sounds reach a playback device (not the null device or manual output).
		static bool HasDevice();
		static uint32_t GetSampleRate();
		static uint32_t GetChannelCount();

		// Linear gain applied after Master's effects and volume (1 = unchanged); ramped over AudioGainSmoothingSeconds. A
		// value above 1 can take the output past a limiter on Master. The getter answers the value last set.
		static float GetMasterVolume();
		static void SetMasterVolume(float volume);

		// The mixer every scene's voices play through (§9.5); without one set, Master alone, which mixes exactly as the
		// engine did before buses. Numbers outside their fields' ranges are brought within them; a mixer that ValidateMixer
		// refuses is logged and the current one kept. Value edits reach
		// the mix at the next block, ramped; added, removed or re-parented buses and changed effect lists reach it with a
		// dip of twice AudioGainSmoothingSeconds on the buses they change, never a click. Effects whose bus and kind carry
		// on keep their state (a reverb's tail).
		static void SetMixer(Mixer const& mixer);
		static Mixer const& GetMixer();

		// The game's value of a bus (§9.5): a gain stage after the mixer's own (the asset's value, blended by snapshots), so
		// that a settings menu and the mix design never overwrite each other. Volumes in decibels (MinMixerVolumeDb is
		// silence). Names match ignoring letter case, as the mixer's names are unique. The values are kept by bus name on
		// the main thread, so a bus removed and restored (an undo) gets its game value back. An unknown name reads 0 dB,
		// not muted, and setting it does nothing; each unknown name is logged once.
		static float GetBusVolume(std::string_view bus);
		static void SetBusVolume(std::string_view bus, float decibels);
		static bool IsBusMuted(std::string_view bus);
		static void SetBusMuted(std::string_view bus, bool muted);
		// The master volume back to 1 and every bus's game value back to 0 dB, not muted: what a game starts and ends with
		// (SceneRunner calls it).
		static void ResetGameValues();
		// Swaps every bus's effects for new ones of the same values, under the dip of a swap, so that no delay or reverb
		// tail of a game rings on into editing or the next game (SceneRunner calls it when its game stops).
		static void ClearEffectState();

		// The voices the engine mixes, across every scene: voices playing in scenes that are not paused, not virtual (beyond
		// the limit) and not stolen, as the main thread knows them. It is exact right after a start or a steal, and the
		// end of a play counts from the next AudioEngine::Update or ReadFrames that reads it, so the count never depends on
		// when the mixing thread runs. At most MaxMixedAudioVoices.
		static uint32_t GetActiveVoiceCount();
		// The voices the mixing thread mixed at its last block (stolen ones still fading out included; a paused scene's
		// voices left out, since its stopped group does not pull them; MX2's meters). Any thread.
		static uint32_t GetMixedVoiceCount();
		// Voices stolen since Init: by a full pool (a one-shot), by the engine's limit (a one-shot is returned, a main voice
		// made virtual), the one-shots refused because they would have ranked lowest included. Any thread.
		static uint64_t GetStolenVoiceCount();
		// Commands waiting on the main thread's side of the queue (AudioCommandQueue): 0 unless a burst filled the ring.
		static size_t GetDeferredCommandCount();

		// Decodes a loaded clip now, so that its first play later decodes nothing (§9.4): what a scene or a system calls at
		// a scene's start for clips it will play as one-shots. The samples stay while the clip asset object lives. Fails as
		// decoding fails (a clip past MaxDecodedClipBytes must stream).
		[[nodiscard]] static Result<void> PreloadClip(Ref<AudioClipAsset> const& clip);

		// Diagnostics for tests (§9.9).
		// Clips decoded since Init (a clip decodes once however often it plays).
		static uint64_t GetDecodedClipCount();
		// Objects the mixing thread holds that the main thread waits to get back (retired voices and groups).
		static size_t GetReturningObjectCount();
		// Blocks the mixer has rendered since Init (at most AudioBlockFrames frames each), on any output: what a test of
		// device or null output counts instead of time. Any thread while the engine is initialized.
		static uint64_t GetBlockCount();
		// Whether the calling thread is mixing right now (inside a block of a device callback's or ReadFrames' render). It
		// reads only an atomic thread identity in static storage (beside the mixer's profiler state), never the engine's
		// data, so a replaced operator new may call it from any thread at any time, during Init and Shutdown and while
		// uninitialized included; it takes no lock and reaches no thread-local storage (std::this_thread::get_id asks the
		// system). Any thread.
		static bool IsMixingOnThisThread();
		// The allocations miniaudio made through the engine's allocation callbacks on the thread that was mixing, while it
		// mixed: a counter in static storage, reset by Init. Every miniaudio object of the Audio module is made with those
		// callbacks. Any thread.
		static uint64_t GetMixAllocationCount();
		// Device and null output: stops the device the way a lost device stops (the engine did not ask for it), so that the
		// next Update takes the mixer over as it would for a device unplugged.
		static void SimulateDeviceLoss();
		// Whether the main thread holds the mixer because the device stopped; it tries to start the device again every
		// AudioDeviceRetrySeconds and hands the mixer back once it runs.
		static bool IsMixerTakenOver();

		// Manual output only: mixes the next frames into interleaved samples (GetChannelCount() per frame; a partial frame
		// at the end is left untouched) and returns the number of frames written. On the calling thread, which must be the
		// main thread, through the same mixer a device's callback runs, in blocks of at most AudioBlockFrames: deferred
		// commands are flushed before each block, and after the last what the mixer handed back is destroyed and the
		// one-shots that ended are retired. Never waits for streamed sounds: one whose file has not been read far enough is
		// silent until it has.
		static uint64_t ReadFrames(std::span<float> samples);
		// Waits until every streamed sound has read and decoded as far ahead as it can (AudioStreamBufferSeconds, or to a
		// file that cannot be read further) and has carried out its seeks. With manual output the next ReadFrames, up to
		// that long, then plays streamed sounds without gaps, as offline capture and tests need.
		static void WaitForStreams();
	};
}
