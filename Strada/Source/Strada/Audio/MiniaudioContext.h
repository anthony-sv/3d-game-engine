#pragma once

// Internal to the Audio module: miniaudio types stay out of every other header.

#include "Strada/Audio/AudioStreamFile.h"

#include <miniaudio.h>

namespace Strada
{
	class AudioStreamer;

	namespace Miniaudio
	{
		// The engine of AudioEngine (valid while it is initialized).
		ma_engine& GetEngine();
		// The audio streaming thread of AudioEngine; null when it could not be started (then nothing can stream).
		AudioStreamer* GetStreamer();
		// How streamed sounds open their files (AudioEngineSettings::StreamFileOpener; empty for the file system).
		AudioStreamFileOpener const& GetStreamFileOpener();
	}
}
