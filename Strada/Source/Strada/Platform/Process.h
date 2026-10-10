#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	struct ProcessSpecification
	{
		// The program: a path, or a name found on PATH. Arguments reach it as they are (no shell is involved); give paths among
		// them in the native form (FileSystem::PathToNativeUtf8), as some programs read forward slashes as options.
		std::filesystem::path Executable;
		std::vector<std::string> Arguments;
		// Empty runs it in the current working directory.
		std::filesystem::path WorkingDirectory;
		// Set in its environment, which otherwise is a copy of this process's.
		std::vector<std::pair<std::string, std::string>> Environment;
		// It is killed when it runs longer; zero lets it run as long as it takes.
		std::chrono::milliseconds Timeout{0};
	};

	struct ProcessResult
	{
		// Meaningless when it timed out or was cancelled.
		int32_t ExitCode = 0;
		// Standard output and standard error, interleaved as written, in the bytes the program wrote.
		std::string Output;
		bool TimedOut = false;
		bool Cancelled = false;

		bool Succeeded() const { return ExitCode == 0 && !TimedOut && !Cancelled; }
	};

	// Child processes (builds, exports, tools). Thread-safe: every call is independent.
	class Process
	{
	public:
		// Runs a program to completion with its standard input empty, capturing its output; blocks the calling thread (use a
		// worker thread for long programs). Setting *cancel from another thread kills the program. Output a program's own
		// children write after it exited is not waited for. Fails when the program cannot be started.
		[[nodiscard]] static Result<ProcessResult> Run(ProcessSpecification const& specification,
		                                               std::atomic<bool> const* cancel = nullptr);

		// The absolute path of a program on PATH (with the executable extensions of PATHEXT on Windows).
		static std::optional<std::filesystem::path> FindExecutable(std::string_view name);
	};
}
