#pragma once

#include <filesystem>
#include <string_view>

namespace Strada
{
	// Leaves a report when the process crashes: a fatal exception such as an access violation (Windows), a fatal signal
	// (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT on Linux and macOS), abort() and failed asserts, or an uncaught C++
	// exception. The report, <directory>/<name>-<process ID>.txt, tells what happened, with the stack on Linux and macOS;
	// on Windows a minidump for a debugger (<name>-<process ID>.dmp) goes next to it. The process then ends as it would
	// have (with the signal on Linux and macOS, with the exception code or 3 for abort() on Windows). Crash reports are
	// written without allocating or logging, as the crash may have happened inside either; uncaught exceptions are also
	// logged with their message first. Install once, early, from the main thread (the entry point does).
	class CrashHandler
	{
	public:
		static void Install(std::filesystem::path const& directory, std::string_view name);
		// Where a crash report would go; empty before Install.
		static std::filesystem::path GetReportPath();
	};
}
