#include "Editor/Automation/AutomationInstanceFile.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Platform.h"

#include <doctest/doctest.h>

#if defined(ST_PLATFORM_WINDOWS)
#include <Windows.h>
#endif

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	// The number of log entries since firstIndex of the severity that contain the text.
	size_t CountLogged(uint64_t firstIndex, LogLevel severity, std::string_view text)
	{
		size_t count = 0;
		for (LogEntry const& entry : Log::GetEntries(firstIndex))
		{
			if (entry.Severity == severity && entry.Message.find(text) != std::string::npos)
			{
				++count;
			}
		}
		return count;
	}

	// A file system that fails the next FailuresLeft writes, and a clock the test moves.
	struct FakeInstanceFiles
	{
		std::chrono::steady_clock::time_point Time{std::chrono::hours(1)};
		uint32_t FailuresLeft = 0;
		// Every write asked for, failed or not, in order.
		std::vector<AutomationInstanceInfo> Attempts;
		// What the file holds.
		std::optional<AutomationInstanceInfo> File;
		std::vector<uint32_t> Removed;
		uint32_t ClockReads = 0;

		AutomationInstanceFileServices MakeServices()
		{
			AutomationInstanceFileServices services;
			services.Write = [this](AutomationInstanceInfo const& info) -> Result<std::filesystem::path>
			{
				Attempts.push_back(info);
				if (FailuresLeft > 0)
				{
					--FailuresLeft;
					return MakeError("Failed to replace the instance file: Access is denied.");
				}
				File = info;
				return std::filesystem::path("Instances") / (std::to_string(info.ProcessID) + ".json");
			};
			services.Remove = [this](uint32_t processID)
			{
				Removed.push_back(processID);
				File.reset();
			};
			services.Now = [this]
			{
				++ClockReads;
				return Time;
			};
			return services;
		}

		std::string GetFileProject() const { return File ? File->Project : std::string("<no file>"); }
	};

	AutomationInstanceInfo MakeInfo(std::string project)
	{
		AutomationInstanceInfo info;
		info.ProcessID = Platform::GetProcessID();
		info.Port = 4242;
		info.Token = "0123456789abcdef";
		info.Project = std::move(project);
		info.Version = "0.1.0";
		return info;
	}

	constexpr char const* FailureText = "Could not write the automation instance file";
	constexpr char const* RecoveryText = "Wrote the automation instance file";
}

TEST_CASE("AutomationInstanceFile: a failed write is retried every 100 ms until the file names the newest project")
{
	FakeInstanceFiles files;
	AutomationInstanceFile instanceFile(files.MakeServices());
	uint64_t const firstLog = Log::GetNextEntryIndex();

	instanceFile.Start(MakeInfo("C:/Games/A/A.sproj"));
	REQUIRE(files.Attempts.size() == 1);
	CHECK(files.GetFileProject() == "C:/Games/A/A.sproj");
	REQUIRE(instanceFile.GetWritten().has_value());
	CHECK(instanceFile.GetWritten()->Project == "C:/Games/A/A.sproj");
	CHECK(instanceFile.GetWritten()->Token == "0123456789abcdef");
	CHECK_FALSE(instanceFile.IsPending());
	CHECK(CountLogged(firstLog, LogLevel::Info, "Automation instance file: ") == 1);

	// While the file states the project, updates do nothing: no write, not even a look at the clock.
	for (int frame = 0; frame < 5; ++frame)
	{
		instanceFile.Update("C:/Games/A/A.sproj");
	}
	CHECK(files.Attempts.size() == 1);
	CHECK(files.ClockReads == 0);

	// A change is written at once; the write fails three times.
	files.FailuresLeft = 3;
	instanceFile.Update("C:/Games/B/B.sproj");
	REQUIRE(files.Attempts.size() == 2);
	CHECK(files.Attempts.back().Project == "C:/Games/B/B.sproj");
	CHECK(instanceFile.IsPending());
	// What the file states is what the last successful write wrote.
	CHECK(instanceFile.GetWritten()->Project == "C:/Games/A/A.sproj");
	CHECK(files.GetFileProject() == "C:/Games/A/A.sproj");
	CHECK(instanceFile.GetRetryTime() == files.Time + AutomationInstanceFile::RetryInterval);

	// No attempt before the deadline.
	instanceFile.Update("C:/Games/B/B.sproj");
	files.Time += std::chrono::milliseconds(99);
	instanceFile.Update("C:/Games/B/B.sproj");
	CHECK(files.Attempts.size() == 2);
	files.Time += std::chrono::milliseconds(1);
	instanceFile.Update("C:/Games/B/B.sproj");
	REQUIRE(files.Attempts.size() == 3);
	CHECK(files.Attempts.back().Project == "C:/Games/B/B.sproj");

	// The project changes again while a retry waits: the retry keeps its deadline and writes the newest project.
	files.Time += std::chrono::milliseconds(50);
	instanceFile.Update("C:/Games/C/C.sproj");
	CHECK(files.Attempts.size() == 3);
	files.Time += std::chrono::milliseconds(50);
	instanceFile.Update("C:/Games/C/C.sproj");
	REQUIRE(files.Attempts.size() == 4);
	CHECK(files.Attempts.back().Project == "C:/Games/C/C.sproj");
	CHECK(files.GetFileProject() == "C:/Games/A/A.sproj");
	CHECK(instanceFile.IsPending());

	// The fourth write succeeds.
	files.Time += std::chrono::milliseconds(100);
	instanceFile.Update("C:/Games/C/C.sproj");
	REQUIRE(files.Attempts.size() == 5);
	CHECK(files.GetFileProject() == "C:/Games/C/C.sproj");
	CHECK(files.File->Port == 4242);
	CHECK(instanceFile.GetWritten()->Project == "C:/Games/C/C.sproj");
	CHECK_FALSE(instanceFile.IsPending());

	// The streak was logged once as an error, and its end once at info, naming the project.
	CHECK(CountLogged(firstLog, LogLevel::Error, FailureText) == 1);
	CHECK(CountLogged(firstLog, LogLevel::Info, RecoveryText) == 1);
	CHECK(CountLogged(firstLog, LogLevel::Info, "after 3 failed attempts: it names the project 'C:/Games/C/C.sproj'") == 1);

	// Nothing is pending: no more writes and no clock reads.
	uint32_t const clockReads = files.ClockReads;
	for (int frame = 0; frame < 5; ++frame)
	{
		files.Time += std::chrono::milliseconds(100);
		instanceFile.Update("C:/Games/C/C.sproj");
	}
	CHECK(files.Attempts.size() == 5);
	CHECK(files.ClockReads == clockReads);

	// A new streak is logged again, once.
	files.FailuresLeft = 1;
	instanceFile.Update("");
	files.Time += std::chrono::milliseconds(100);
	instanceFile.Update("");
	CHECK(files.GetFileProject().empty());
	CHECK(CountLogged(firstLog, LogLevel::Error, FailureText) == 2);
	CHECK(CountLogged(firstLog, LogLevel::Info, "after 1 failed attempt: it names no project") == 1);

	instanceFile.Stop();
	CHECK(files.Removed == std::vector<uint32_t>{Platform::GetProcessID()});
}

TEST_CASE("AutomationInstanceFile: the first write is retried, a change back needs no write, and stopping drops the retry")
{
	FakeInstanceFiles files;
	AutomationInstanceFile instanceFile(files.MakeServices());

	// Nothing before Start.
	instanceFile.Update("C:/Games/A/A.sproj");
	instanceFile.Stop();
	CHECK_FALSE(instanceFile.IsStarted());
	CHECK(files.Attempts.empty());
	CHECK(files.Removed.empty());

	// The first write fails and is retried after the interval.
	uint64_t const firstLog = Log::GetNextEntryIndex();
	files.FailuresLeft = 1;
	instanceFile.Start(MakeInfo(""));
	CHECK(instanceFile.IsStarted());
	CHECK(instanceFile.IsPending());
	CHECK_FALSE(instanceFile.GetWritten().has_value());
	CHECK_FALSE(files.File.has_value());
	instanceFile.Update("");
	CHECK(files.Attempts.size() == 1);
	files.Time += AutomationInstanceFile::RetryInterval;
	instanceFile.Update("");
	CHECK(files.Attempts.size() == 2);
	CHECK(files.GetFileProject().empty());
	REQUIRE(instanceFile.GetWritten().has_value());
	CHECK(CountLogged(firstLog, LogLevel::Error, FailureText) == 1);
	CHECK(CountLogged(firstLog, LogLevel::Info, "after 1 failed attempt: it names no project") == 1);

	// A change that fails, then a change back to what the file states: nothing is left to write.
	files.FailuresLeft = 1;
	instanceFile.Update("C:/Games/B/B.sproj");
	CHECK(instanceFile.IsPending());
	instanceFile.Update("");
	CHECK_FALSE(instanceFile.IsPending());
	files.Time += AutomationInstanceFile::RetryInterval;
	instanceFile.Update("");
	CHECK(files.Attempts.size() == 3);
	CHECK(files.GetFileProject().empty());

	// Stopping while a retry waits removes the file and drops the retry.
	files.FailuresLeft = 1;
	instanceFile.Update("C:/Games/D/D.sproj");
	REQUIRE(instanceFile.IsPending());
	instanceFile.Stop();
	CHECK(files.Removed == std::vector<uint32_t>{Platform::GetProcessID()});
	CHECK_FALSE(instanceFile.IsStarted());
	CHECK_FALSE(instanceFile.IsPending());
	CHECK_FALSE(instanceFile.GetWritten().has_value());
	files.Time += AutomationInstanceFile::RetryInterval;
	instanceFile.Update("C:/Games/D/D.sproj");
	CHECK(files.Attempts.size() == 4);
	CHECK_FALSE(files.File.has_value());
}

#if defined(ST_PLATFORM_WINDOWS)
TEST_CASE("AutomationInstanceFile: while a reader holds the file open the new project waits for the next retry after it closes")
{
	struct RemovedAtEnd
	{
		~RemovedAtEnd() { AutomationInstance::Remove(Platform::GetProcessID()); }
	} removedAtEnd;
	// A reader as the strada tool's scans are: read access, sharing reads and writes but not deletion.
	struct Reader
	{
		HANDLE Handle = INVALID_HANDLE_VALUE;

		explicit Reader(std::filesystem::path const& path)
			: Handle(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		                           FILE_ATTRIBUTE_NORMAL, nullptr))
		{
		}
		~Reader() { Close(); }
		Reader(Reader const&) = delete;
		Reader& operator=(Reader const&) = delete;

		void Close()
		{
			if (Handle != INVALID_HANDLE_VALUE)
			{
				::CloseHandle(Handle);
				Handle = INVALID_HANDLE_VALUE;
			}
		}
	};

	std::chrono::steady_clock::time_point clockTime{std::chrono::hours(1)};
	// The real file, timed by the test.
	AutomationInstanceFileServices services;
	services.Now = [&clockTime]
	{
		return clockTime;
	};
	AutomationInstanceFile instanceFile(std::move(services));
	std::filesystem::path const path = AutomationInstance::GetPath(Platform::GetProcessID());
	auto const readProject = [&path]() -> std::string
	{
		Result<AutomationInstanceInfo> read = AutomationInstance::Read(path);
		return read ? read.GetValue().Project : std::string("<unreadable>");
	};

	instanceFile.Start(MakeInfo("C:/Games/A/A.sproj"));
	REQUIRE(instanceFile.GetWritten().has_value());
	CHECK(readProject() == "C:/Games/A/A.sproj");

	Reader reader(path);
	REQUIRE(reader.Handle != INVALID_HANDLE_VALUE);
	uint64_t const firstLog = Log::GetNextEntryIndex();
	instanceFile.Update("C:/Games/B/B.sproj");
	// The replace fails while the reader has the file open; the file still names the old project.
	REQUIRE(instanceFile.IsPending());
	CHECK(instanceFile.GetWritten()->Project == "C:/Games/A/A.sproj");
	CHECK(readProject() == "C:/Games/A/A.sproj");
	clockTime += AutomationInstanceFile::RetryInterval;
	instanceFile.Update("C:/Games/B/B.sproj");
	CHECK(instanceFile.IsPending());
	CHECK(CountLogged(firstLog, LogLevel::Error, FailureText) == 1);

	// Once the reader closed, the next retry writes the new project, not an update before its deadline.
	reader.Close();
	instanceFile.Update("C:/Games/B/B.sproj");
	CHECK(instanceFile.IsPending());
	CHECK(readProject() == "C:/Games/A/A.sproj");
	clockTime += AutomationInstanceFile::RetryInterval;
	instanceFile.Update("C:/Games/B/B.sproj");
	CHECK_FALSE(instanceFile.IsPending());
	CHECK(readProject() == "C:/Games/B/B.sproj");
	CHECK(CountLogged(firstLog, LogLevel::Info, "after 2 failed attempts: it names the project 'C:/Games/B/B.sproj'") == 1);

	// The failed writes left no temporary file beside it.
	std::string const prefix = FileSystem::PathToUtf8(path.filename()) + ".tmp";
	std::error_code errorCode;
	for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(path.parent_path(), errorCode))
	{
		CHECK_FALSE(FileSystem::PathToUtf8(entry.path().filename()).starts_with(prefix));
	}

	instanceFile.Stop();
	CHECK_FALSE(FileSystem::Exists(path));
}
#endif
