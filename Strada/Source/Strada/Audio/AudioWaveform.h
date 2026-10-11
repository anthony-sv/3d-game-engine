#pragma once

#include "Strada/Core/Result.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace Strada
{
	class AudioClipAsset;

	// The outline of an audio clip's loudness over time, for previews (the editor's content browser draws it).
	struct AudioWaveform
	{
		// The largest absolute sample value over every channel (0 to 1) of each of equally long parts of the clip, in order.
		std::vector<float> Peaks;
		double Duration = 0.0;
		uint32_t SampleRate = 0;
		uint32_t ChannelCount = 0;
	};

	// Decodes the clip and measures its peaks in partCount equally long parts (fewer for very short clips). Touches no
	// engine state, so it may run on any thread; setting *cancel from another thread stops it. Fails when the clip cannot
	// be decoded or holds no audio, and when cancelled.
	[[nodiscard]] Result<AudioWaveform> ComputeAudioWaveform(AudioClipAsset const& clip, uint32_t partCount,
	                                                         std::atomic<bool> const* cancel = nullptr);
}
