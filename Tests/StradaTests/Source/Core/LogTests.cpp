#include "TestUtilities.h"

#include "Strada/Core/Log.h"

#include <doctest/doctest.h>

#include <string>

using namespace Strada;

TEST_CASE("Log: entries are buffered with monotonic indices")
{
	uint64_t const start = Log::GetNextEntryIndex();
	ST_CORE_INFO("first {}", 1);
	ST_WARN("second {}", 2);
	Log::GetScriptLogger().error("third");

	std::vector<LogEntry> const entries = Log::GetEntries(start);
	REQUIRE(entries.size() == 3);
	CHECK(entries[0].Index == start);
	CHECK(entries[1].Index == start + 1);
	CHECK(entries[2].Index == start + 2);

	CHECK(entries[0].Message == "first 1");
	CHECK(entries[0].Logger == "STRADA");
	CHECK(entries[0].Severity == LogLevel::Info);
	CHECK(entries[1].Logger == "APP");
	CHECK(entries[1].Severity == LogLevel::Warn);
	CHECK(entries[2].Logger == "SCRIPT");
	CHECK(entries[2].Severity == LogLevel::Error);
	CHECK(entries[0].TimestampMs > 0);
	CHECK(Log::GetNextEntryIndex() == start + 3);
}

TEST_CASE("Log: entry queries honor the starting index and maximum count")
{
	uint64_t const start = Log::GetNextEntryIndex();
	for (int i = 0; i < 5; i++)
	{
		ST_CORE_TRACE("entry {}", i);
	}

	std::vector<LogEntry> const limited = Log::GetEntries(start + 1, 2);
	REQUIRE(limited.size() == 2);
	CHECK(limited[0].Message == "entry 1");
	CHECK(limited[1].Message == "entry 2");

	CHECK(Log::GetEntries(start + 5).empty());
	CHECK(Log::GetEntries(start, 0).empty());
}

TEST_CASE("Log: clearing keeps indices increasing")
{
	ST_CORE_INFO("before clear");
	uint64_t const next = Log::GetNextEntryIndex();
	Log::ClearEntries();
	CHECK(Log::GetEntries(0).empty());

	ST_CORE_INFO("after clear");
	std::vector<LogEntry> const entries = Log::GetEntries(0);
	REQUIRE(entries.size() == 1);
	CHECK(entries[0].Index == next);
}

TEST_CASE("Log: the buffer keeps only the most recent entries")
{
	Log::Shutdown();
	LogSpecification specification;
	specification.ConsoleOutput = false;
	specification.BufferCapacity = 3;
	Log::Init(specification);

	for (int i = 0; i < 10; i++)
	{
		ST_CORE_INFO("message {}", i);
	}
	std::vector<LogEntry> const entries = Log::GetEntries(0);
	REQUIRE(entries.size() == 3);
	CHECK(entries[0].Message == "message 7");
	CHECK(entries[2].Message == "message 9");

	// Restore the default test logging configuration.
	Log::Shutdown();
	LogSpecification defaults;
	defaults.ConsoleLevel = LogLevel::Warn;
	Log::Init(defaults);
}

TEST_CASE("Log: file output writes formatted entries")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const logPath = directory.GetPath() / "Logs" / "Test.log";

	Log::Shutdown();
	LogSpecification specification;
	specification.ConsoleOutput = false;
	specification.FilePath = logPath;
	Log::Init(specification);
	ST_CORE_WARN("written to file {}", 123);
	Log::Shutdown();

	Result<std::string> const text = FileSystem::ReadTextFile(logPath);
	REQUIRE(text.IsOk());
	CHECK(text.GetValue().find("written to file 123") != std::string::npos);
	CHECK(text.GetValue().find("[warning]") != std::string::npos);

	LogSpecification defaults;
	defaults.ConsoleLevel = LogLevel::Warn;
	Log::Init(defaults);
}

TEST_CASE("Log: logging is safe while not initialized")
{
	Log::Shutdown();
	CHECK_FALSE(Log::IsInitialized());
	ST_CORE_TRACE("this goes to spdlog's default logger");
	CHECK(Log::GetEntries(0).empty());

	LogSpecification defaults;
	defaults.ConsoleLevel = LogLevel::Warn;
	Log::Init(defaults);
	CHECK(Log::IsInitialized());
}

TEST_CASE("Log: glm types and levels format readably")
{
	uint64_t const start = Log::GetNextEntryIndex();
	ST_CORE_INFO("{} {} {}", glm::vec2(1.0f, 2.0f), glm::vec3(1.0f, 2.0f, 3.0f), glm::vec4(1.0f, 2.0f, 3.0f, 4.0f));
	std::vector<LogEntry> const entries = Log::GetEntries(start);
	REQUIRE(entries.size() == 1);
	CHECK(entries[0].Message == "(1, 2) (1, 2, 3) (1, 2, 3, 4)");
	CHECK(std::string(Log::LevelToString(LogLevel::Critical)) == "Critical");
}
