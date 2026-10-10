#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Platform/Process.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace Strada;

namespace
{
	// The crash tester's file of the extension in the directory; empty when there is none.
	std::filesystem::path FindCrashFile(std::filesystem::path const& directory, std::string_view extension)
	{
		std::error_code error;
		for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(directory, error))
		{
			if (FileSystem::PathToUtf8(entry.path().filename()).starts_with("StradaCrashTester-") &&
			    FileSystem::PathToUtf8(entry.path().extension()) == extension)
			{
				return entry.path();
			}
		}
		return {};
	}

	std::string ReadText(std::filesystem::path const& path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		return text ? text.GetValue() : std::string();
	}

	ProcessResult Crash(std::string const& mode, std::filesystem::path const& directory)
	{
		ProcessSpecification specification;
		specification.Executable = STRADA_CRASH_TESTER_PATH;
		specification.Arguments = {mode, FileSystem::PathToNativeUtf8(directory)};
		specification.Timeout = std::chrono::seconds(60);
		Result<ProcessResult> result = Process::Run(specification);
		REQUIRE_MESSAGE(result.IsOk(), (result ? std::string() : result.GetError()));
		return result.GetValue();
	}
}

TEST_CASE("CrashHandler: crashes leave a report and end the process")
{
	struct Case
	{
		char const* Mode;
		char const* Cause;
		// The exit codes the system gives the crashed process.
		std::vector<int32_t> ExitCodes;
	};
#if defined(ST_PLATFORM_WINDOWS)
	std::vector<Case> const cases = {
		{"access-violation", "access violation (exception 0xc0000005)", {static_cast<int32_t>(0xC0000005u)}},
		{"abort", "abort()", {3}},
		{"exception", "abort()", {3}},
	};
#else
	// Ended by the signal: 128 plus its number.
	std::vector<Case> const cases = {
		{"access-violation", "SIG", {128 + SIGSEGV, 128 + SIGBUS}},
		{"abort", "SIGABRT", {128 + SIGABRT}},
		{"exception", "SIGABRT", {128 + SIGABRT}},
	};
#endif
	for (Case const& testCase : cases)
	{
		char const* const mode = testCase.Mode;
		char const* const cause = testCase.Cause;
		CAPTURE(mode);
		Testing::TemporaryDirectory directory;
		ProcessResult const result = Crash(mode, directory.GetPath());
		CHECK_FALSE(result.TimedOut);
		CAPTURE(result.ExitCode);
		CHECK(std::find(testCase.ExitCodes.begin(), testCase.ExitCodes.end(), result.ExitCode) != testCase.ExitCodes.end());
		CHECK(result.Output.find("Crash report: ") != std::string::npos);

		std::filesystem::path const report = FindCrashFile(directory.GetPath(), ".txt");
		REQUIRE_FALSE(report.empty());
		std::string const text = ReadText(report);
		CHECK(text.starts_with("StradaCrashTester crashed: "));
		CHECK(text.find(cause) != std::string::npos);
#if defined(ST_PLATFORM_WINDOWS)
		std::filesystem::path const dump = FindCrashFile(directory.GetPath(), ".dmp");
		REQUIRE_FALSE(dump.empty());
		std::error_code error;
		CHECK(std::filesystem::file_size(dump, error) > 0);
		CHECK(text.find("Minidump: " + FileSystem::PathToUtf8(dump)) != std::string::npos);
#else
		CHECK(text.find("Stack:\n") != std::string::npos);
		if (std::string_view(mode) == "access-violation")
		{
			// Linux reports SIGSEGV; macOS may report SIGBUS for the first page.
			CHECK((text.find("SIGSEGV") != std::string::npos || text.find("SIGBUS") != std::string::npos));
		}
#endif
		if (std::string_view(mode) == "exception")
		{
			CHECK(ReadText(directory.GetPath() / "Tester.log").find("Uncaught exception: deliberately uncaught") != std::string::npos);
		}
	}
}
