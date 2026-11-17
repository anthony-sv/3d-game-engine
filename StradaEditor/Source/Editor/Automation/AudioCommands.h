#pragma once

#include "Editor/Automation/CommandRegistry.h"

#include "Strada/Core/Result.h"

namespace Strada
{
	// The meters' reading a decibel value of silence is given as: the mixer's silence, MinMixerVolumeDb, so that the answer
	// holds no infinity.
	[[nodiscard]] float ToMeterDecibels(float level);

	// Registers audio.meters (read-only, not an undo step): every bus's peak and RMS over the last block the audio engine
	// mixed, the snapshot the mix is at, and the active, mixed and stolen voice counts (Docs/Architecture.md §9.8). The
	// registry must outlive every registered command. Documented in Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterAudioCommands(CommandRegistry& registry);
}
