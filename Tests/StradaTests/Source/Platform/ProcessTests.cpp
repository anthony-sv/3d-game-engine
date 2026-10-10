#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Platform/Process.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace Strada;
using namespace std::chrono_literals;

namespace
{
	// The platform's shell running a command line: the portable way for tests to produce output and exit codes.
	ProcessSpecification Shell(std::string const& command)
	{
		ProcessSpecification specification;
#if defined(ST_PLATFORM_WINDOWS)
		specification.Executable = Process::FindExecutable("cmd").value_or("cmd.exe");
		// No AutoRun commands; /s takes the command line after /c as it is.
		specification.Arguments = {"/d", "/s", "/c", command};
#else
		specification.Executable = "/bin/sh";
		specification.Arguments = {"-c", command};
#endif
		return specification;
	}

	// A program that runs for half a minute unless stopped; no shell, so stopping it leaves no other process behind.
	ProcessSpecification LongRunning()
	{
		ProcessSpecification specification;
#if defined(ST_PLATFORM_WINDOWS)
		specification.Executable = Process::FindExecutable("ping").value_or("ping.exe");
		specification.Arguments = {"-n", "30", "127.0.0.1"};
#else
		specification.Executable = Process::FindExecutable("sleep").value_or("/bin/sleep");
		specification.Arguments = {"30"};
#endif
		return specification;
	}

	size_t CountOccurrences(std::string const& text, std::string const& part)
	{
		size_t count = 0;
		for (size_t position = text.find(part); position != std::string::npos; position = text.find(part, position + part.size()))
		{
			count++;
		}
		return count;
	}
}

TEST_CASE("Process: runs programs and captures their output and exit code")
{
#if defined(ST_PLATFORM_WINDOWS)
	Result<ProcessResult> const failed = Process::Run(Shell("echo out& echo err 1>&2& exit /b 3"));
	Result<ProcessResult> const succeeded = Process::Run(Shell("echo done"));
#else
	Result<ProcessResult> const failed = Process::Run(Shell("echo out; echo err >&2; exit 3"));
	Result<ProcessResult> const succeeded = Process::Run(Shell("echo done"));
#endif
	REQUIRE(failed.IsOk());
	CAPTURE(failed.GetValue().Output);
	CHECK(failed.GetValue().ExitCode == 3);
	CHECK_FALSE(failed.GetValue().Succeeded());
	// Standard error is captured with standard output.
	CHECK(failed.GetValue().Output.find("out") != std::string::npos);
	CHECK(failed.GetValue().Output.find("err") != std::string::npos);
	REQUIRE(succeeded.IsOk());
	CHECK(succeeded.GetValue().Succeeded());
	CHECK(succeeded.GetValue().Output.find("done") != std::string::npos);
}

TEST_CASE("Process: output larger than a pipe buffer is collected whole")
{
	// About 200 KB: a program writing more than the pipe holds blocks until it is read.
#if defined(ST_PLATFORM_WINDOWS)
	Result<ProcessResult> const result = Process::Run(Shell("for /l %i in (1,1,5000) do @echo line %i abcdefghijklmnopqrstuvwxyz"));
#else
	Result<ProcessResult> const result =
		Process::Run(Shell("i=1; while [ $i -le 5000 ]; do echo line $i abcdefghijklmnopqrstuvwxyz; i=$((i+1)); done"));
#endif
	REQUIRE(result.IsOk());
	CHECK(result.GetValue().Succeeded());
	CHECK(CountOccurrences(result.GetValue().Output, "abcdefghijklmnopqrstuvwxyz") == 5000);
	CHECK(result.GetValue().Output.find("line 5000 ") != std::string::npos);
}

TEST_CASE("Process: programs run in the given directory with the given environment")
{
	Testing::TemporaryDirectory directory;
#if defined(ST_PLATFORM_WINDOWS)
	ProcessSpecification specification = Shell("cd& echo %STRADA_PROCESS_TEST%");
#else
	ProcessSpecification specification = Shell("pwd; echo $STRADA_PROCESS_TEST");
#endif
	specification.WorkingDirectory = directory.GetPath();
	specification.Environment = {{"STRADA_PROCESS_TEST", "visible"}};
	Result<ProcessResult> const result = Process::Run(specification);
	REQUIRE(result.IsOk());
	CHECK(result.GetValue().Output.find("visible") != std::string::npos);
	// The temporary directory may be reached through a link (macOS), so only its unique name is compared.
	CHECK(result.GetValue().Output.find(FileSystem::PathToUtf8(directory.GetPath().filename())) != std::string::npos);
}

TEST_CASE("Process: programs are killed when they run out of time or are cancelled")
{
	ProcessSpecification limited = LongRunning();
	limited.Timeout = 300ms;
	auto start = std::chrono::steady_clock::now();
	Result<ProcessResult> const timedOut = Process::Run(limited);
	REQUIRE(timedOut.IsOk());
	CHECK(timedOut.GetValue().TimedOut);
	CHECK_FALSE(timedOut.GetValue().Cancelled);
	CHECK_FALSE(timedOut.GetValue().Succeeded());
	CHECK(std::chrono::steady_clock::now() - start < 10s);

	std::atomic<bool> cancel = false;
	start = std::chrono::steady_clock::now();
	std::thread canceller(
		[&cancel]()
		{
			std::this_thread::sleep_for(200ms);
			cancel = true;
		});
	Result<ProcessResult> const cancelled = Process::Run(LongRunning(), &cancel);
	canceller.join();
	REQUIRE(cancelled.IsOk());
	CHECK(cancelled.GetValue().Cancelled);
	CHECK_FALSE(cancelled.GetValue().TimedOut);
	CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("Process: missing programs fail to start and PATH lookups find programs")
{
	Testing::TemporaryDirectory directory;
	ProcessSpecification missing;
	missing.Executable = directory.GetPath() / "strada-no-such-program";
	Result<ProcessResult> const result = Process::Run(missing);
	REQUIRE(result.IsError());
	CHECK(result.GetError().find("strada-no-such-program") != std::string::npos);

#if defined(ST_PLATFORM_WINDOWS)
	std::optional<std::filesystem::path> const shell = Process::FindExecutable("cmd");
#else
	std::optional<std::filesystem::path> const shell = Process::FindExecutable("sh");
#endif
	REQUIRE(shell.has_value());
	CHECK(shell->is_absolute());
	CHECK(std::filesystem::is_regular_file(*shell));
	CHECK_FALSE(Process::FindExecutable("strada-no-such-program").has_value());
	CHECK_FALSE(Process::FindExecutable("").has_value());
}
