#pragma once

#include <cstdint>

// The limits of the audio engine's voices (Docs/Architecture.md §9.3) and of its doppler (§9.8), shared by the audio engine
// and the components that name them.
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
	// How much of the doppler effect a spatial source takes (AudioSourceComponent::DopplerLevel): 0 none, 1 physical, up to
	// MaxAudioDopplerLevel exaggerated.
	inline constexpr float DefaultAudioDopplerLevel = 1.0f;
	inline constexpr float MaxAudioDopplerLevel = 5.0f;
	// The doppler factor a voice's pitch is multiplied by stays within these (Docs/Architecture.md §9.8): an octave down
	// and up. At level 1 the octave up is reached approaching at half the speed of sound, the octave down receding at the
	// speed of sound (each body at the teleport threshold of half of it) or at a higher level.
	inline constexpr float MinAudioDopplerFactor = 0.5f;
	inline constexpr float MaxAudioDopplerFactor = 2.0f;

	// The shape of a reverb zone (ReverbZoneComponent, Docs/Architecture.md §9.8): a sphere of a radius, or a box of half
	// extents turned with its entity.
	enum class ReverbZoneShape : uint8_t
	{
		Sphere = 0,
		Box
	};
	// The buses a spatial voice sends to at once: of the reverb zones its sound is in, those of the buses with the two
	// largest sends.
	inline constexpr uint32_t MaxAudioVoiceSends = 2;
	// A zone's sizes and fade distance stay within this many meters.
	inline constexpr float MaxReverbZoneSize = 100000.0f;
}
