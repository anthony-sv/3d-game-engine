#pragma once

#include <cstdint>

// The limits of the audio engine's voices (Docs/Architecture.md §9.3), shared by the audio engine and the components that
// name them.
namespace Strada
{
	// A voice is one playing instance of a clip. A source's voices are its main voice (Play, Pause, Stop) and its
	// one-shots (PlayOneShot); at most its MaxVoices of them play at once.
	inline constexpr uint32_t DefaultAudioSourceVoices = 4;
	inline constexpr uint32_t MaxAudioSourceVoices = 64;
	// The voices the engine mixes at once, across every scene. A voice that would start beyond it steals the voice that
	// ranks lowest: the lowest priority, then the quietest at its listener, then the oldest (the new voice itself when it
	// ranks lowest).
	inline constexpr uint32_t MaxMixedAudioVoices = 64;
	// Priorities run from 0 to MaxAudioPriority; a higher priority is kept over a lower one.
	inline constexpr uint32_t DefaultAudioPriority = 128;
	inline constexpr uint32_t MaxAudioPriority = 255;
	// Seconds a stolen voice takes to fade out; it is still heard, and still mixed, until then.
	inline constexpr float AudioStealFadeSeconds = 0.01f;
}
