#pragma once

#include "Strada/Core/Base.h"

#include <optional>
#include <string>
#include <vector>

namespace Strada
{
	// Small OS queries that the core needs before any other subsystem exists. All functions are thread-safe.
	class Platform
	{
	public:
		static bool IsDebuggerAttached();
		static uint32_t GetProcessID();
		// Whether a process with this ID currently exists (false for 0). Used to detect stale instance files.
		static bool IsProcessRunning(uint32_t processID);
		// Named to avoid the Win32 GetEnvironmentVariable macro.
		static std::optional<std::string> ReadEnvironmentVariable(std::string const& name);
		// The process arguments as UTF-8, program name first. On Windows they are read from the wide command line, since
		// main's argv holds them in the ANSI code page (characters outside it become '?'); elsewhere argv is copied.
		static std::vector<std::string> ReadCommandLineArguments(int argc, char const* const* argv);
		// Monotonic time in seconds since an unspecified, process-constant epoch.
		static double GetTime();
		// POSIX systems end a process that writes to a pipe or socket whose reading end is closed (SIGPIPE); afterwards such
		// writes fail with EPIPE instead. Applications do this at startup: an editor that a tool started keeps running when
		// the tool exits and the editor's output pipe closes. Process-wide; nothing to do on Windows, where the writes fail.
		static void IgnoreBrokenPipeSignal();
	};
}
