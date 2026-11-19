#pragma once

#include "Strada/Core/Result.h"
#include "Strada/Core/Telemetry.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// What a CSV export of a session holds.
	struct TelemetryCsvOptions
	{
		// The rows FirstRow to LastRow, both included, of those the session holds; by default every row.
		uint64_t FirstRow = 0;
		uint64_t LastRow = std::numeric_limits<uint64_t>::max();
		// The channels exported, in this order; empty: every channel of the session, in its order.
		std::vector<std::string> Channels;
	};

	// Writes a telemetry session as CSV (Docs/Architecture.md §7.3, "Telemetry CSV"): RFC 4180 text in UTF-8, a header
	// line of quoted names ("Step", "Time", each channel's name, "Marks") and a line per row (its step, its time in
	// seconds, each channel's value or an empty field, its marks joined by a line feed in one quoted field), every line
	// ended by CR LF, numbers as JsonTextWriter writes them. An export for analysis tools: nothing in the engine reads it
	// back, so it carries no Strada header and no version. Pure; any thread.
	class TelemetryCsvWriter
	{
	public:
		static constexpr uint64_t MaxBytes = 512ull * 1024 * 1024;
		// How an export's first line begins (telemetry.export replaces an existing file only when it begins so).
		static constexpr std::string_view HeaderStart = "\"Step\",\"Time\"";

		// The rows of one lap: from the mark that begins it to the row before the mark that ends it.
		[[nodiscard]] static TelemetryCsvOptions ForLap(TelemetryLap const& lap);

		// Refuses a channel the session does not hold, a channel named twice, and text longer than maxBytes (checked
		// after each line).
		[[nodiscard]] static Result<std::string> Write(TelemetrySession const& session, TelemetryCsvOptions const& options = {},
		                                               uint64_t maxBytes = MaxBytes);
		// Write, then FileSystem::WriteTextFile (atomic; creates missing parent directories; CR LF stays CR LF).
		[[nodiscard]] static Result<void> Save(TelemetrySession const& session, std::filesystem::path const& path,
		                                       TelemetryCsvOptions const& options = {}, uint64_t maxBytes = MaxBytes);
	};
}
