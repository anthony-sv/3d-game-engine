#pragma once

#include "Strada/Core/Profiler.h"
#include "Strada/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Strada
{
	// Profiler captures (Docs/Architecture.md §7.3): a ProfilerCapture as one JSON document in the Chrome trace event format,
	// with the Strada file header, which chrome://tracing and Perfetto display and the editor opens again. Pure functions
	// of their arguments; any thread.
	//
	// What the reader accepts the writer writes back unchanged: reading the text a writer wrote gives the capture it was
	// given, compacted (only the threads, names and views its frames use, renumbered in their order) and with its names made
	// valid UTF-8 of at most Profiler::MaxNameBytes (thread names Profiler::MaxThreadNameBytes).
	class ProfilerCaptureSerializer
	{
	public:
		static constexpr int Version = 1;
		static constexpr std::string_view FileType = "ProfilerCapture";
		// GPU passes a capture holds at most over all its frames (the recorder bounds zones, not passes).
		static constexpr uint32_t MaxGpuPasses = Profiler::MaxHistoryZones;

		// The capture's text. Refuses a capture holding more than a reader accepts (frames, zones, GPU passes, names,
		// threads, views; see Profiler's bounds) or whose text would exceed Profiler::MaxCaptureBytes, which the error names
		// with what makes a capture smaller (fewer frames or zones: ProfilerSettings::HistoryFrames, HistoryZones).
		[[nodiscard]] static Result<std::string> Write(ProfilerCapture const& capture);
		// Reads a capture's text. Refuses a damaged capture, saying what is wrong and with which event.
		[[nodiscard]] static Result<ProfilerCapture> Read(std::string_view text);

		// Write, then FileSystem::WriteTextFile (atomic; creates missing parent directories).
		[[nodiscard]] static Result<void> Save(ProfilerCapture const& capture, std::filesystem::path const& path);
		// Reads a capture file, refusing one larger than Profiler::MaxCaptureBytes before reading it.
		[[nodiscard]] static Result<ProfilerCapture> Load(std::filesystem::path const& path);
	};
}
