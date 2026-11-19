#pragma once

#include "Editor/Automation/CommandRegistry.h"

#include "Strada/Core/Result.h"

#include <cstdint>
#include <functional>

namespace Strada
{
	class EditorContext;
	class EditorTelemetry;

	// What telemetry.read answers at most: channels per request, and samples per channel (512 unless asked).
	inline constexpr uint32_t MaxTelemetryReadChannels = 16;
	inline constexpr uint32_t MaxTelemetryReadSamples = 4096;
	inline constexpr uint32_t DefaultTelemetryReadSamples = 512;
	// The marks telemetry.channels lists of a session; it counts the rest.
	inline constexpr uint32_t MaxTelemetryListedMarks = 256;

	// Registers telemetry.start, telemetry.stop, telemetry.channels, telemetry.read and telemetry.export
	// (Docs/Automation.md, Docs/Architecture.md §4.5), none of them an undo step; telemetry.channels and telemetry.read
	// change nothing. They answer summaries and write bulk data to CSV files (telemetry.export). The registry, the telemetry
	// owner and the context must outlive every registered command. showPanel opens the Telemetry panel and brings it forward
	// (telemetry.start's panel); empty without a panel to show (headless).
	[[nodiscard]] Result<void> RegisterTelemetryCommands(CommandRegistry& registry, EditorTelemetry& telemetry, EditorContext& context,
	                                                     std::function<void()> showPanel = {});
}
