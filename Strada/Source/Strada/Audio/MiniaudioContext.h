#pragma once

// Internal to the Audio module: miniaudio types stay out of every other header.

#include "Strada/Audio/AudioStreamFile.h"

#include <miniaudio.h>

#include <string_view>

namespace Strada
{
	class AudioBusNode;
	class AudioSampleCache;
	class AudioStreamer;
	class AudioVoiceManager;

	namespace Miniaudio
	{
		// The engine of AudioEngine (valid while it is initialized).
		ma_engine& GetEngine();
		// The audio streaming thread of AudioEngine; null when it could not be started (then nothing can stream).
		AudioStreamer* GetStreamer();
		// How streamed sounds open their files (AudioEngineSettings::StreamFileOpener; empty for the file system).
		AudioStreamFileOpener const& GetStreamFileOpener();
		// The main thread's side of the mix: commands, the table of voices, the objects lent to the mixing thread.
		AudioVoiceManager& GetVoiceManager();
		// The decoded samples of loaded clips.
		AudioSampleCache& GetSampleCache();
		// The bus node a group of the bus named busKey (letters folded to lower case) plays into: null for the engine's
		// endpoint, which is Master until the mixer has buses. A name the mixer lacks plays on Master and is logged once.
		AudioBusNode* FindBus(std::string_view busKey);

		// The mixing thread's identity for the length of one block (AudioEngine::IsMixingOnThisThread). Constructed by the
		// thread that mixes; touches only an atomic in static storage.
		class MixingScope
		{
		public:
			MixingScope();
			~MixingScope();

			MixingScope(MixingScope const&) = delete;
			MixingScope& operator=(MixingScope const&) = delete;
		};
	}
}
