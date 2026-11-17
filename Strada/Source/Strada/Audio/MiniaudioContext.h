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
		// The engine's allocation callbacks, which every miniaudio object of the module is made and destroyed with (the
		// bus nodes pass them to ma_node_init and ma_node_uninit; sounds and groups take the engine's). Any time.
		ma_allocation_callbacks const& GetAllocationCallbacks();
		// The audio streaming thread of AudioEngine; null when it could not be started (then nothing can stream).
		AudioStreamer* GetStreamer();
		// How streamed sounds open their files (AudioEngineSettings::StreamFileOpener; empty for the file system).
		AudioStreamFileOpener const& GetStreamFileOpener();
		// The main thread's side of the mix: commands, the table of voices, the objects lent to the mixing thread.
		AudioVoiceManager& GetVoiceManager();
		// The decoded samples of loaded clips.
		AudioSampleCache& GetSampleCache();
		// The bus node a group of the bus keyed busKey (MakeMixerBusKey) plays into: the mixer's bus of that key, or Master's
		// when the mixer lacks it, which is logged once per key and mixer with busName, the name as a source gave it (null
		// only when Master's node could not be made: the engine's endpoint).
		AudioBusNode* FindBus(std::string_view busKey, std::string_view busName);

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
