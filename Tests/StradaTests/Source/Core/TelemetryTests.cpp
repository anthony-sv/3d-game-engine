#include "Core/TelemetryTestUtilities.h"

#include "Strada/Core/Log.h"
#include "Strada/Core/Telemetry.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Strada;
using Testing::GetNewestTelemetrySession;
using Testing::GetTelemetryColumn;
using Testing::GetTelemetryMarks;
using Testing::MakeTelemetryStep;
using Testing::TelemetryScope;

namespace
{
	constexpr float StepSeconds = 1.0f / 60.0f;

	// The warnings telemetry logged since a log index.
	std::vector<std::string> GetTelemetryWarnings(uint64_t since)
	{
		std::vector<std::string> warnings;
		for (LogEntry const& entry : Log::GetEntries(since))
		{
			if (entry.Severity == LogLevel::Warn && entry.Message.starts_with("Telemetry:"))
			{
				warnings.push_back(entry.Message);
			}
		}
		return warnings;
	}

	// A hand-built session of `rows` rows with the given marks, for the lap rules.
	TelemetrySession MakeSession(uint64_t rows, std::vector<TelemetryMark> marks, float stepSeconds = 0.5f)
	{
		TelemetrySession session;
		session.Id = 1;
		session.Source = 1;
		session.FirstStep = 1;
		session.Rows = rows;
		session.StepSeconds = stepSeconds;
		session.Marks = std::move(marks);
		return session;
	}

	struct LapSpan
	{
		std::string Label;
		uint64_t StartRow = 0;
		uint64_t EndRow = 0;

		bool operator==(LapSpan const&) const = default;
	};

	std::vector<LapSpan> GetLapSpans(TelemetrySession const& session)
	{
		std::vector<LapSpan> spans;
		for (TelemetryLap const& lap : FindTelemetryLaps(session))
		{
			spans.push_back({lap.Label, lap.StartRow, lap.EndRow});
		}
		return spans;
	}

	// Runs a function on a thread of its own and waits for it.
	template<typename TFunction>
	void RunOnThread(TFunction&& function)
	{
		std::thread thread(std::forward<TFunction>(function));
		thread.join();
	}

	// The buffers Start makes (Docs/Architecture.md §4.5): one per hardware thread and four more, at most
	// Telemetry::MaxThreads.
	uint32_t GetStartBuffers()
	{
		return std::min(Telemetry::MaxThreads, std::max(std::thread::hardware_concurrency(), 1u) + 4);
	}
}

TEST_CASE("Telemetry: every function does nothing before Init and after Shutdown")
{
	for (int round = 0; round < 2; ++round)
	{
		CHECK_FALSE(Telemetry::IsInitialized());
		CHECK_FALSE(Telemetry::IsRecording());
		CHECK(Telemetry::Start().IsError());
		Telemetry::Record("Ignored", 1.0f);
		Telemetry::Mark("Ignored");
		Telemetry::MarkStep("Ignored", 3);
		Telemetry::EndStep(MakeTelemetryStep(1));
		Telemetry::EndSession();
		Telemetry::Stop();
		Telemetry::EndThread();
		CHECK(Telemetry::GetSessions().empty());
		CHECK(Telemetry::FindSession(1) == nullptr);
		TelemetryStatus const status = Telemetry::GetStatus();
		CHECK_FALSE(status.Initialized);
		CHECK_FALSE(status.Recording);
		CHECK(status.Buffers == 0);

		Telemetry::Init();
		CHECK(Telemetry::IsInitialized());
		Telemetry::Shutdown();
	}
}

TEST_CASE("Telemetry: Start refuses settings out of bounds and a second recording")
{
	TelemetryScope const telemetry;
	auto refused = [](auto change)
	{
		TelemetrySettings settings;
		change(settings);
		return Telemetry::Start(settings).IsError();
	};
	CHECK(refused(
		[](TelemetrySettings& settings)
		{
			settings.MaxChannels = Telemetry::MinChannels - 1;
		}));
	CHECK(refused(
		[](TelemetrySettings& settings)
		{
			settings.MaxChannels = Telemetry::MaxChannelsLimit + 1;
		}));
	CHECK(refused(
		[](TelemetrySettings& settings)
		{
			settings.MaxSteps = 0;
		}));
	CHECK(refused(
		[](TelemetrySettings& settings)
		{
			settings.MaxSteps = Telemetry::MaxStepsLimit + 1;
		}));
	CHECK(refused(
		[](TelemetrySettings& settings)
		{
			settings.HistorySessions = 0;
		}));
	CHECK(refused(
		[](TelemetrySettings& settings)
		{
			settings.HistorySessions = Telemetry::MaxHistorySessions + 1;
		}));
	CHECK(refused(
		[](TelemetrySettings& settings)
		{
			settings.HistoryBytes = Telemetry::MinHistoryBytes - 1;
		}));
	CHECK(refused(
		[](TelemetrySettings& settings)
		{
			settings.HistoryBytes = Telemetry::MaxHistoryBytes + 1;
		}));
	CHECK(refused(
		[](TelemetrySettings& settings)
		{
			settings.RecordsPerThread = Telemetry::MinRecordsPerThread - 1;
		}));
	CHECK(refused(
		[](TelemetrySettings& settings)
		{
			settings.RecordsPerThread = Telemetry::MaxRecordsPerThread + 1;
		}));
	CHECK_FALSE(Telemetry::IsRecording());

	TelemetrySettings bounds;
	bounds.MaxChannels = Telemetry::MaxChannelsLimit;
	bounds.MaxSteps = Telemetry::MinSteps;
	bounds.HistorySessions = Telemetry::MaxHistorySessions;
	bounds.HistoryBytes = Telemetry::MinHistoryBytes;
	bounds.RecordsPerThread = Telemetry::MinRecordsPerThread;
	REQUIRE(Telemetry::Start(bounds).IsOk());
	CHECK(Telemetry::IsRecording());
	CHECK(Telemetry::Start().IsError());
	CHECK(Telemetry::GetStatus().Settings.MaxChannels == Telemetry::MaxChannelsLimit);
	Telemetry::Stop();
	CHECK_FALSE(Telemetry::IsRecording());
	CHECK(Telemetry::Start().IsOk());
}

TEST_CASE("Telemetry: a channel holds one value per step, the last recorded for it, and none where nothing was recorded")
{
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	// Before the first step: its row.
	Telemetry::Record("Speed", 1.0f);
	Telemetry::Record("Speed", 2.0f);
	Telemetry::Record("Gear", 3.0f);
	Telemetry::EndStep(MakeTelemetryStep(10));
	Telemetry::EndStep(MakeTelemetryStep(11));
	// Between steps: the next one's row.
	Telemetry::Record("Speed", 5.0f);
	Telemetry::EndStep(MakeTelemetryStep(12));

	TelemetrySession const& session = GetNewestTelemetrySession();
	CHECK(session.Live);
	CHECK(session.FirstStep == 10);
	CHECK(session.Rows == 3);
	CHECK(session.StepSeconds == StepSeconds);
	CHECK(GetTelemetryTime(session, 2) == 12.0 * static_cast<double>(StepSeconds));
	REQUIRE(session.Channels.size() == 2);
	CHECK(session.Channels[0].Name == "Speed");
	CHECK(session.Channels[1].Name == "Gear");
	CHECK(GetTelemetryColumn(session, "Speed") == std::vector<std::optional<float>>{2.0f, std::nullopt, 5.0f});
	CHECK(GetTelemetryColumn(session, "Gear") == std::vector<std::optional<float>>{3.0f, std::nullopt, std::nullopt});

	TelemetryChannel const* const speed = FindTelemetryChannel(session, "Speed");
	REQUIRE(speed != nullptr);
	CHECK(speed->FirstRow == 0);
	CHECK(speed->Cells == 3);
	CHECK(speed->Count == 2);
	CHECK(speed->Minimum == 2.0f);
	CHECK(speed->Maximum == 5.0f);
	CHECK(speed->Last == 5.0f);
	TelemetryChannel const* const gear = FindTelemetryChannel(session, "Gear");
	REQUIRE(gear != nullptr);
	// A column ends at its last value.
	CHECK(gear->Cells == 1);
	CHECK(FindTelemetryChannel(session, "Missing") == nullptr);
	CHECK(session.DroppedValues == 0);
}

TEST_CASE("Telemetry: a pause gives the next row each channel's last value and every mark, with the resuming step's own")
{
	TelemetryScope const telemetry;
	TelemetrySettings settings;
	settings.RecordsPerThread = Telemetry::MinRecordsPerThread;
	REQUIRE(Telemetry::Start(settings).IsOk());
	Telemetry::EndStep(MakeTelemetryStep(1));

	// Ten thousand frames of a pause record on the main thread, which keeps every channel's last value and every mark.
	constexpr int PausedFrames = 10000;
	for (int frame = 0; frame < PausedFrames; ++frame)
	{
		Telemetry::Record("Paused.A", static_cast<float>(frame));
		Telemetry::Record("Paused.B", static_cast<float>(-frame));
		if (frame % 1000 == 0)
		{
			Telemetry::Mark("Pause " + std::to_string(frame / 1000));
		}
	}
	// Another thread's buffer keeps its oldest records once full, and counts the rest.
	RunOnThread(
		[]
		{
			for (int record = 0; record < 1000; ++record)
			{
				Telemetry::Record("Worker", static_cast<float>(record));
			}
		});
	// The resuming step's own values and marks.
	Telemetry::Record("Resumed", 7.0f);
	Telemetry::Mark("Resume");
	Telemetry::EndStep(MakeTelemetryStep(2));

	TelemetrySession const& session = GetNewestTelemetrySession();
	REQUIRE(session.Rows == 2);
	CHECK(GetTelemetryColumn(session, "Paused.A")[1] == static_cast<float>(PausedFrames - 1));
	CHECK(GetTelemetryColumn(session, "Paused.B")[1] == static_cast<float>(-(PausedFrames - 1)));
	CHECK(GetTelemetryColumn(session, "Resumed")[1] == 7.0f);
	CHECK(GetTelemetryColumn(session, "Worker")[1] == static_cast<float>(Telemetry::MinRecordsPerThread - 1));
	std::vector<std::string> const marks = GetTelemetryMarks(session, 1);
	REQUIRE(marks.size() == 11);
	CHECK(marks.front() == "Pause 0");
	CHECK(marks[9] == "Pause 9");
	CHECK(marks.back() == "Resume");
	CHECK(session.DroppedValues == 1000 - Telemetry::MinRecordsPerThread);
	CHECK(session.DroppedMarks == 0);
}

TEST_CASE("Telemetry: names refused and values not finite are dropped, counted and warned of once per kind and session")
{
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	uint64_t const logStart = Log::GetNextEntryIndex();
	std::string const longest(Telemetry::MaxNameBytes, 'a');
	std::string const tooLong(Telemetry::MaxNameBytes + 1, 'a');
	auto recordRefused = [&tooLong]
	{
		Telemetry::Record("", 1.0f);
		Telemetry::Record(tooLong, 1.0f);
		Telemetry::Record("Bad\xff", 1.0f);
		Telemetry::Record("Tab\tName", 1.0f);
		Telemetry::Record("Delete\x7f", 1.0f);
		Telemetry::Record("Value", std::numeric_limits<float>::quiet_NaN());
		Telemetry::Record("Value", std::numeric_limits<float>::infinity());
		Telemetry::Mark("");
		Telemetry::Mark(tooLong);
	};
	recordRefused();
	Telemetry::Record(longest, 1.0f);
	Telemetry::Record("Gr\xc3\xb6\xc3\x9f"
	                  "e",
	                  2.0f);
	// Another thread's long name is refused too, never cut to the longest name.
	RunOnThread(
		[&tooLong]
		{
			Telemetry::Record(tooLong, 3.0f);
		});
	Telemetry::EndStep(MakeTelemetryStep(1));

	TelemetrySession const& session = GetNewestTelemetrySession();
	CHECK(session.DroppedValues == 8);
	CHECK(session.DroppedMarks == 2);
	REQUIRE(session.Channels.size() == 2);
	CHECK(session.Channels[0].Name == longest);
	CHECK(session.Channels[0].Count == 1);
	CHECK(session.Channels[0].Last == 1.0f);
	CHECK(session.Channels[1].Name == "Gr\xc3\xb6\xc3\x9f"
	                                  "e");
	std::vector<std::string> warnings = GetTelemetryWarnings(logStart);
	REQUIRE(warnings.size() == 2);
	CHECK(warnings[0].find("1 to 64 bytes") != std::string::npos);
	CHECK(warnings[1].find("'Value'") != std::string::npos);

	// The same drops again in the session: counted, not warned of again.
	recordRefused();
	Telemetry::EndStep(MakeTelemetryStep(2));
	CHECK(GetNewestTelemetrySession().DroppedValues == 15);
	CHECK(GetTelemetryWarnings(logStart).size() == 2);

	// A new session warns again.
	Telemetry::EndSession();
	recordRefused();
	Telemetry::EndStep(MakeTelemetryStep(3));
	CHECK(Telemetry::GetSessions().size() == 2);
	CHECK(GetNewestTelemetrySession().DroppedValues == 7);
	CHECK(GetTelemetryWarnings(logStart).size() == 4);
	CHECK(Telemetry::GetStatus().DroppedValues == 22);
	CHECK(Telemetry::GetStatus().DroppedMarks == 6);

	CHECK(IsTelemetryName("Ride.Distance"));
	CHECK(IsTelemetryName(longest));
	CHECK_FALSE(IsTelemetryName(tooLong));
	CHECK_FALSE(IsTelemetryName(""));
	CHECK_FALSE(IsTelemetryName("Line\nFeed"));
	CHECK_FALSE(IsTelemetryName("\xc3"));
}

TEST_CASE("Telemetry: past a session's channels, steps and marks, what is recorded is dropped and counted")
{
	TelemetryScope const telemetry;
	SUBCASE("Channels")
	{
		TelemetrySettings settings;
		settings.MaxChannels = Telemetry::MinChannels;
		REQUIRE(Telemetry::Start(settings).IsOk());
		for (uint64_t step = 1; step <= 2; ++step)
		{
			for (uint32_t channel = 0; channel < Telemetry::MinChannels + 6; ++channel)
			{
				Telemetry::Record("Channel." + std::to_string(channel), 1.0f);
			}
			Telemetry::EndStep(MakeTelemetryStep(step));
		}
		TelemetrySession const& session = GetNewestTelemetrySession();
		CHECK(session.Channels.size() == Telemetry::MinChannels);
		CHECK(session.DroppedValues == 12);
		CHECK(FindTelemetryChannel(session, "Channel.63") != nullptr);
		CHECK(FindTelemetryChannel(session, "Channel.64") == nullptr);
		CHECK(FindTelemetryChannel(session, "Channel.0")->Count == 2);
	}
	SUBCASE("Steps")
	{
		TelemetrySettings settings;
		settings.MaxSteps = 3;
		REQUIRE(Telemetry::Start(settings).IsOk());
		for (uint64_t step = 1; step <= 5; ++step)
		{
			Telemetry::Record("Value", static_cast<float>(step));
			Telemetry::Mark("Step");
			Telemetry::EndStep(MakeTelemetryStep(step));
		}
		TelemetrySession const& session = GetNewestTelemetrySession();
		CHECK(session.Rows == 3);
		CHECK(session.Live);
		CHECK(session.DroppedSteps == 2);
		CHECK(session.DroppedValues == 2);
		CHECK(session.DroppedMarks == 2);
		CHECK(GetTelemetryColumn(session, "Value") == std::vector<std::optional<float>>{1.0f, 2.0f, 3.0f});
		CHECK(Telemetry::GetStatus().DroppedSteps == 2);
	}
	SUBCASE("Marks")
	{
		REQUIRE(Telemetry::Start().IsOk());
		for (uint32_t mark = 0; mark <= Telemetry::MaxMarks; ++mark)
		{
			Telemetry::Mark("M");
		}
		Telemetry::EndStep(MakeTelemetryStep(1));
		TelemetrySession const& session = GetNewestTelemetrySession();
		CHECK(session.Marks.size() == Telemetry::MaxMarks);
		CHECK(session.DroppedMarks == 1);
	}
}

TEST_CASE("Telemetry: the sessions hold their bytes in whole blocks, the oldest dropped first and the live one dropping its own")
{
	TelemetryScope const telemetry;
	constexpr uint64_t BlockBytes = uint64_t{TelemetryChannel::BlockCells} * sizeof(float);
	TelemetrySettings settings;
	settings.MaxChannels = Telemetry::MinChannels;
	settings.HistoryBytes = Telemetry::MinHistoryBytes;
	REQUIRE(Telemetry::MinHistoryBytes == 64 * BlockBytes);
	REQUIRE(Telemetry::Start(settings).IsOk());

	auto recordChannels = [](uint32_t first, uint32_t count)
	{
		for (uint32_t channel = first; channel < first + count; ++channel)
		{
			Telemetry::Record("Channel." + std::to_string(channel), 1.0f);
		}
	};
	recordChannels(0, 40);
	Telemetry::EndStep(MakeTelemetryStep(1));
	uint32_t const first = GetNewestTelemetrySession().Id;
	CHECK(GetNewestTelemetrySession().Bytes == 40 * BlockBytes);
	Telemetry::EndSession();

	// A second session of 40 blocks drops the first, which is not live.
	recordChannels(0, 40);
	Telemetry::EndStep(MakeTelemetryStep(5));
	CHECK(Telemetry::GetSessions().size() == 1);
	CHECK(Telemetry::FindSession(first) == nullptr);
	CHECK(Telemetry::GetStatus().BytesHeld == 40 * BlockBytes);

	// The live session alone fills the bound with 24 more channels; a mark and a value past it are dropped.
	recordChannels(40, 24);
	Telemetry::EndStep(MakeTelemetryStep(6));
	TelemetrySession const& session = GetNewestTelemetrySession();
	CHECK(session.Bytes == 64 * BlockBytes);
	CHECK(Telemetry::GetStatus().BytesHeld == Telemetry::MinHistoryBytes);
	Telemetry::Mark("Past the bound");
	Telemetry::EndStep(MakeTelemetryStep(7));
	CHECK(session.Marks.empty());
	CHECK(session.DroppedMarks == 1);
	// Channel.0's column began at row 0 (step 5) and fills its block at row 4095: row 4096 needs another.
	for (uint64_t step = 8; step <= 4 + TelemetryChannel::BlockCells; ++step)
	{
		Telemetry::Record("Channel.0", 2.0f);
		Telemetry::EndStep(MakeTelemetryStep(step));
	}
	CHECK(session.DroppedValues == 0);
	Telemetry::Record("Channel.0", 3.0f);
	Telemetry::EndStep(MakeTelemetryStep(5 + TelemetryChannel::BlockCells));
	CHECK(session.DroppedValues == 1);
	CHECK(FindTelemetryChannel(session, "Channel.0")->Blocks.size() == 1);
	CHECK(session.Bytes == Telemetry::MinHistoryBytes);
}

TEST_CASE("Telemetry: a column grows in blocks that never move, with empty cells where rows have no value")
{
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	constexpr uint64_t BlockCells = TelemetryChannel::BlockCells;
	constexpr uint64_t BlockBytes = BlockCells * sizeof(float);
	float const* firstBlock = nullptr;
	for (uint64_t row = 0; row <= 2 * BlockCells + 10; ++row)
	{
		if (row == 0 || row == BlockCells - 1 || row == BlockCells || row == 2 * BlockCells + 10)
		{
			Telemetry::Record("Sparse", static_cast<float>(row));
		}
		Telemetry::Record("Dense", static_cast<float>(row));
		if (row == 5000)
		{
			Telemetry::Record("Late", 1.0f);
		}
		Telemetry::EndStep(MakeTelemetryStep(row + 1));
		if (row == 0)
		{
			firstBlock = FindTelemetryChannel(GetNewestTelemetrySession(), "Dense")->Blocks.front().data();
		}
	}
	TelemetrySession const& session = GetNewestTelemetrySession();
	TelemetryChannel const* const dense = FindTelemetryChannel(session, "Dense");
	TelemetryChannel const* const sparse = FindTelemetryChannel(session, "Sparse");
	TelemetryChannel const* const late = FindTelemetryChannel(session, "Late");
	REQUIRE(dense != nullptr);
	REQUIRE(sparse != nullptr);
	REQUIRE(late != nullptr);
	CHECK(dense->Blocks.size() == 3);
	CHECK(dense->Blocks.front().data() == firstBlock);
	CHECK(dense->Cells == 2 * BlockCells + 11);
	CHECK(GetTelemetryValue(*dense, BlockCells + 7) == static_cast<float>(BlockCells + 7));
	CHECK(GetTelemetryCells(*dense, BlockCells - 4).size() == 4);
	CHECK(GetTelemetryCells(*dense, BlockCells).size() == BlockCells);
	CHECK(GetTelemetryCells(*dense, 2 * BlockCells).size() == 11);
	CHECK(GetTelemetryCells(*dense, 2 * BlockCells + 11).empty());

	CHECK(sparse->Count == 4);
	CHECK(GetTelemetryValue(*sparse, 1) == std::nullopt);
	CHECK(std::isnan(GetTelemetryCells(*sparse, 1).front()));
	CHECK(GetTelemetryValue(*sparse, BlockCells) == static_cast<float>(BlockCells));
	CHECK(GetTelemetryValue(*sparse, 2 * BlockCells + 10) == static_cast<float>(2 * BlockCells + 10));

	CHECK(late->FirstRow == 5000);
	CHECK(late->Cells == 1);
	CHECK(GetTelemetryCells(*late, 4999).empty());
	CHECK(GetTelemetryValue(*late, 5000) == 1.0f);

	// While live, a session also holds the block each column made ahead of its need; ended, only its columns' blocks.
	uint64_t const columnBlocks = dense->Blocks.size() + sparse->Blocks.size() + late->Blocks.size();
	CHECK(session.Bytes >= columnBlocks * BlockBytes);
	Telemetry::EndSession();
	CHECK(session.Bytes == columnBlocks * BlockBytes);
	CHECK(Telemetry::GetStatus().BytesHeld == session.Bytes);
}

TEST_CASE("Telemetry: a session holds one scene's consecutive steps, and the sessions are kept across recordings")
{
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	Telemetry::EndStep(MakeTelemetryStep(1, 7));
	Telemetry::EndStep(MakeTelemetryStep(2, 7));
	// Another scene, then a step that is not the next one (a scene started again, a snapshot restored), then another
	// step length.
	Telemetry::EndStep(MakeTelemetryStep(3, 8));
	Telemetry::EndStep(MakeTelemetryStep(1, 8));
	Telemetry::EndStep(MakeTelemetryStep(2, 8, 1.0f / 30.0f));
	// The play mode's game stops: the next step begins another.
	Telemetry::EndSession();
	Telemetry::EndSession();
	Telemetry::EndStep(MakeTelemetryStep(3, 8, 1.0f / 30.0f));

	std::deque<TelemetrySession> const& sessions = Telemetry::GetSessions();
	REQUIRE(sessions.size() == 5);
	std::vector<uint64_t> firstSteps;
	std::vector<uint64_t> rows;
	for (uint32_t index = 0; index < sessions.size(); ++index)
	{
		CHECK(sessions[index].Id == index + 1);
		CHECK(sessions[index].Live == (index == 4));
		firstSteps.push_back(sessions[index].FirstStep);
		rows.push_back(sessions[index].Rows);
	}
	CHECK(firstSteps == std::vector<uint64_t>{1, 3, 1, 2, 3});
	CHECK(rows == std::vector<uint64_t>{2, 1, 1, 1, 1});
	CHECK(sessions[0].Source == 7);
	CHECK(sessions[3].StepSeconds == 1.0f / 30.0f);
	CHECK(Telemetry::GetStatus().LiveSession == 5);

	// Stopped, the sessions stay; a recording with room for two keeps the two newest, then drops the oldest, never the live
	// one.
	Telemetry::Stop();
	CHECK(Telemetry::GetStatus().LiveSession == 0);
	CHECK(Telemetry::GetSessions().size() == 5);
	TelemetrySettings settings;
	settings.HistorySessions = 2;
	REQUIRE(Telemetry::Start(settings).IsOk());
	CHECK(Telemetry::GetSessions().size() == 2);
	CHECK(Telemetry::GetSessions().front().Id == 4);
	Telemetry::EndStep(MakeTelemetryStep(1));
	REQUIRE(Telemetry::GetSessions().size() == 2);
	CHECK(Telemetry::GetSessions().front().Id == 5);
	CHECK(Telemetry::GetSessions().back().Id == 6);
	CHECK(Telemetry::FindSession(6)->Live);
	Telemetry::Stop();

	settings.HistorySessions = 1;
	REQUIRE(Telemetry::Start(settings).IsOk());
	Telemetry::EndStep(MakeTelemetryStep(1));
	Telemetry::EndStep(MakeTelemetryStep(5));
	REQUIRE(Telemetry::GetSessions().size() == 1);
	CHECK(Telemetry::GetSessions().front().Id == 8);
	CHECK(Telemetry::GetSessions().front().Live);

	// Numbers count from 1 again only after Shutdown.
	Telemetry::Shutdown();
	Telemetry::Init();
	REQUIRE(Telemetry::Start().IsOk());
	Telemetry::EndStep(MakeTelemetryStep(1));
	CHECK(GetNewestTelemetrySession().Id == 1);
}

TEST_CASE("Telemetry: laps run from a mark Lap n to the next boundary when it is Lap n + 1, on a later row")
{
	SUBCASE("A race of three laps, its sectors, its finish and its end")
	{
		TelemetrySession const session = MakeSession(
			100, {{0, "Lap 1"}, {10, "Sector 1.1"}, {20, "Lap 2"}, {50, "Lap 3"}, {60, "Lap 4"}, {60, std::string(TelemetryLapEndLabel)}});
		std::vector<TelemetryLap> const laps = FindTelemetryLaps(session);
		REQUIRE(laps.size() == 3);
		CHECK(laps[0].Label == "Lap 1");
		CHECK(laps[0].Number == 1);
		CHECK(laps[0].StartRow == 0);
		CHECK(laps[0].EndRow == 20);
		CHECK(laps[0].Seconds == 10.0);
		CHECK(laps[2].Label == "Lap 3");
		CHECK(laps[2].Seconds == 5.0);
		CHECK(FindBestTelemetryLap(laps) == size_t{2});
		CHECK_FALSE(FindTelemetryLapInProgress(session).has_value());
	}
	SUBCASE("A race abandoned and started again, and a finished race followed by another")
	{
		TelemetrySession const restarted =
			MakeSession(100, {{0, "Lap 1"}, {30, "Lap 2"}, {40, std::string(TelemetryLapEndLabel)}, {45, "Lap 1"}, {75, "Lap 2"}});
		CHECK(GetLapSpans(restarted) == std::vector<LapSpan>{{"Lap 1", 0, 30}, {"Lap 1", 45, 75}});
		// Equal laps: the first is the best, and no span across the restart is a lap.
		std::vector<TelemetryLap> const laps = FindTelemetryLaps(restarted);
		CHECK(FindBestTelemetryLap(laps) == size_t{0});

		TelemetrySession const again = MakeSession(100, {{0, "Lap 1"}, {10, "Lap 2"}, {20, "Lap 3"}, {30, "Lap 1"}, {45, "Lap 2"}});
		CHECK(GetLapSpans(again) == std::vector<LapSpan>{{"Lap 1", 0, 10}, {"Lap 2", 10, 20}, {"Lap 1", 30, 45}});
		std::optional<TelemetryLap> const inProgress = FindTelemetryLapInProgress(again);
		REQUIRE(inProgress.has_value());
		CHECK(inProgress->Label == "Lap 2");
		CHECK(inProgress->StartRow == 45);
		CHECK(inProgress->EndRow == 99);
	}
	SUBCASE("Two boundaries on one row, a gap in the numbers, and labels that only look like laps")
	{
		CHECK(GetLapSpans(MakeSession(50, {{5, "Lap 1"}, {5, "Lap 2"}, {15, "Lap 3"}})) == std::vector<LapSpan>{{"Lap 2", 5, 15}});
		CHECK(GetLapSpans(MakeSession(50, {{0, "Lap 1"}, {10, "Lap 3"}, {20, "Lap 4"}})) == std::vector<LapSpan>{{"Lap 3", 10, 20}});
		CHECK(GetLapSpans(MakeSession(50, {{0, "Lap 1"}, {5, "Lapse"}, {7, "Lap 02"}, {9, "Lap"}, {12, "Lap 2"}})) ==
		      std::vector<LapSpan>{{"Lap 1", 0, 12}});
		CHECK(FindTelemetryLaps(MakeSession(10, {})).empty());
		CHECK_FALSE(FindTelemetryLapInProgress(MakeSession(10, {{3, "Sector 1.1"}})).has_value());
		CHECK_FALSE(FindBestTelemetryLap({}).has_value());

		CHECK(ParseTelemetryLapNumber("Lap 1") == 1u);
		CHECK(ParseTelemetryLapNumber("Lap 4294967295") == 4294967295u);
		CHECK_FALSE(ParseTelemetryLapNumber("Lap 4294967296").has_value());
		CHECK_FALSE(ParseTelemetryLapNumber("Lap 0").has_value());
		CHECK_FALSE(ParseTelemetryLapNumber("Lap 01").has_value());
		CHECK_FALSE(ParseTelemetryLapNumber("Lap -1").has_value());
		CHECK_FALSE(ParseTelemetryLapNumber("Lap +1").has_value());
		CHECK_FALSE(ParseTelemetryLapNumber("Lap 2x").has_value());
		CHECK_FALSE(ParseTelemetryLapNumber("Lap  2").has_value());
		CHECK_FALSE(ParseTelemetryLapNumber("lap 2").has_value());
		CHECK_FALSE(ParseTelemetryLapNumber(TelemetryLapEndLabel).has_value());
	}
}

TEST_CASE("Telemetry: marks land on their step's row, and marks of a named step on that step's row")
{
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	for (uint64_t step = 1; step <= 5; ++step)
	{
		Telemetry::EndStep(MakeTelemetryStep(step));
	}
	TelemetrySession const& session = GetNewestTelemetrySession();

	// A step the session holds is marked at once on the main thread, in row order.
	Telemetry::MarkStep("Lap 2", 4);
	Telemetry::MarkStep("Lap 1", 2);
	REQUIRE(session.Marks.size() == 2);
	CHECK(session.Marks[0].Row == 1);
	CHECK(session.Marks[1].Row == 3);

	// Another thread's marks are placed when its records are collected, after the main thread's; the current step, one
	// still to come, and step 0 (refused).
	RunOnThread(
		[]
		{
			Telemetry::MarkStep("Worker", 3);
			Telemetry::Mark("Worker now");
		});
	Telemetry::Mark("Now");
	Telemetry::MarkStep("Named now", 6);
	Telemetry::MarkStep("Later", 8);
	Telemetry::MarkStep("Refused", 0);
	Telemetry::EndStep(MakeTelemetryStep(6));
	CHECK(GetTelemetryMarks(session, 2) == std::vector<std::string>{"Worker"});
	CHECK(GetTelemetryMarks(session, 5) == std::vector<std::string>{"Now", "Named now", "Worker now"});
	CHECK(session.DroppedMarks == 1);

	// A mark held for its step comes before the marks collected on its row.
	Telemetry::EndStep(MakeTelemetryStep(7));
	Telemetry::Mark("Then");
	Telemetry::EndStep(MakeTelemetryStep(8));
	CHECK(GetTelemetryMarks(session, 7) == std::vector<std::string>{"Later", "Then"});

	// A step the session never reaches drops its mark when the session ends; one before a session began, too.
	Telemetry::MarkStep("Never", 20);
	Telemetry::EndSession();
	CHECK(session.DroppedMarks == 2);
	Telemetry::EndStep(MakeTelemetryStep(9));
	Telemetry::MarkStep("Before", 4);
	Telemetry::EndStep(MakeTelemetryStep(10));
	CHECK(GetNewestTelemetrySession().Marks.empty());
	CHECK(GetNewestTelemetrySession().DroppedMarks == 1);
	CHECK(Telemetry::GetStatus().DroppedMarks == 3);
	// A mark made right before the session ends stays.
	Telemetry::MarkStep("Lap 1", 9);
	Telemetry::EndSession();
	CHECK(GetTelemetryMarks(GetNewestTelemetrySession(), 0) == std::vector<std::string>{"Lap 1"});
}

TEST_CASE("Telemetry: threads record at once, and each value lands once, in its step's row or the next")
{
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	Telemetry::EndStep(MakeTelemetryStep(1));

	constexpr int Threads = 4;
	constexpr int ChannelsPerThread = 50;
	constexpr int RecordsPerThread = 2000;
	std::atomic<int> finished{0};
	std::vector<std::thread> threads;
	for (int thread = 0; thread < Threads; ++thread)
	{
		threads.emplace_back(
			[thread, &finished]
			{
				for (int channel = 0; channel < ChannelsPerThread; ++channel)
				{
					Telemetry::Record("Thread" + std::to_string(thread) + "." + std::to_string(channel),
				                      static_cast<float>(thread * 1000 + channel));
				}
				std::string const counter = "Counter" + std::to_string(thread);
				for (int record = 1; record <= RecordsPerThread; ++record)
				{
					Telemetry::Record(counter, static_cast<float>(record));
				}
				finished.fetch_add(1, std::memory_order_release);
			});
	}
	// Steps end while the threads record.
	uint64_t step = 2;
	while (finished.load(std::memory_order_acquire) < Threads && step < 100000)
	{
		Telemetry::EndStep(MakeTelemetryStep(step++));
	}
	for (std::thread& thread : threads)
	{
		thread.join();
	}
	Telemetry::EndStep(MakeTelemetryStep(step));

	TelemetrySession const& session = GetNewestTelemetrySession();
	CHECK(session.DroppedValues == 0);
	CHECK(session.Rows == step);
	for (int thread = 0; thread < Threads; ++thread)
	{
		for (int channel = 0; channel < ChannelsPerThread; ++channel)
		{
			TelemetryChannel const* const recorded =
				FindTelemetryChannel(session, "Thread" + std::to_string(thread) + "." + std::to_string(channel));
			REQUIRE(recorded != nullptr);
			CHECK(recorded->Count == 1);
			CHECK(recorded->Last == static_cast<float>(thread * 1000 + channel));
		}
		// A thread's later values land on the same row or later ones, and its last value is the column's last.
		std::vector<std::optional<float>> const column = GetTelemetryColumn(session, "Counter" + std::to_string(thread));
		float previous = 0.0f;
		for (std::optional<float> const& value : column)
		{
			if (value)
			{
				CHECK(*value > previous);
				previous = *value;
			}
		}
		CHECK(previous == static_cast<float>(RecordsPerThread));
	}
}

TEST_CASE("Telemetry: threads beyond the buffers drop and count their records, and EndThread gives a buffer back")
{
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	Telemetry::EndStep(MakeTelemetryStep(1));
	uint64_t const logStart = Log::GetNextEntryIndex();

	// Threads that record once and hold their buffers until released; each step keeps four buffers free.
	std::mutex mutex;
	std::condition_variable released;
	bool release = false;
	std::atomic<uint32_t> recorded{0};
	std::vector<std::thread> holders;
	auto startHolder = [&](uint32_t index, bool endThread)
	{
		holders.emplace_back(
			[&, index, endThread]
			{
				Telemetry::Record("Holder." + std::to_string(index), 1.0f);
				recorded.fetch_add(1, std::memory_order_release);
				std::unique_lock lock(mutex);
				released.wait(lock,
			                  [&release]
			                  {
								  return release;
							  });
				if (endThread)
				{
					Telemetry::EndThread();
				}
			});
	};
	uint64_t step = 2;
	for (uint32_t index = 0; index < Telemetry::MaxThreads; ++index)
	{
		startHolder(index, index == 0);
		while (recorded.load(std::memory_order_acquire) < index + 1)
		{
			std::this_thread::yield();
		}
		if (index % 4 == 3)
		{
			Telemetry::EndStep(MakeTelemetryStep(step++));
		}
	}
	CHECK(Telemetry::GetStatus().Buffers == Telemetry::MaxThreads);
	CHECK(GetNewestTelemetrySession().DroppedValues == 0);

	// Every buffer is held: one more thread drops its record.
	RunOnThread(
		[]
		{
			Telemetry::Record("Beyond", 1.0f);
		});
	Telemetry::EndStep(MakeTelemetryStep(step++));
	CHECK(GetNewestTelemetrySession().DroppedValues == 1);
	CHECK(FindTelemetryChannel(GetNewestTelemetrySession(), "Beyond") == nullptr);
	std::vector<std::string> const warnings = GetTelemetryWarnings(logStart);
	REQUIRE(warnings.size() == 1);
	CHECK(warnings[0].find("no free buffer") != std::string::npos);

	{
		std::scoped_lock const lock(mutex);
		release = true;
	}
	released.notify_all();
	holders[0].join();
	// The first holder gave its buffer back: the next step frees it, and a new thread takes it.
	Telemetry::EndStep(MakeTelemetryStep(step++));
	RunOnThread(
		[]
		{
			Telemetry::Record("Returned", 2.0f);
		});
	Telemetry::EndStep(MakeTelemetryStep(step++));
	TelemetrySession const& session = GetNewestTelemetrySession();
	CHECK(session.DroppedValues == 1);
	REQUIRE(FindTelemetryChannel(session, "Returned") != nullptr);
	CHECK(FindTelemetryChannel(session, "Returned")->Last == 2.0f);
	for (size_t index = 1; index < holders.size(); ++index)
	{
		holders[index].join();
	}
	for (uint32_t index = 0; index < Telemetry::MaxThreads; ++index)
	{
		CHECK(FindTelemetryChannel(session, "Holder." + std::to_string(index)) != nullptr);
	}
}

TEST_CASE("Telemetry: every session frees the buffers of the threads that recorded in the last one, ended or not")
{
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	constexpr int Sessions = 10;
	constexpr int ThreadsPerSession = 20;
	uint64_t step = 1;
	for (int session = 0; session < Sessions; ++session)
	{
		Telemetry::EndStep(MakeTelemetryStep(step++));
		for (int thread = 0; thread < ThreadsPerSession; ++thread)
		{
			// None of them calls EndThread.
			RunOnThread(
				[thread]
				{
					Telemetry::Record("Thread", static_cast<float>(thread));
				});
			Telemetry::EndStep(MakeTelemetryStep(step++));
		}
		Telemetry::EndSession();
	}
	std::deque<TelemetrySession> const& sessions = Telemetry::GetSessions();
	REQUIRE(sessions.size() == TelemetrySettings{}.HistorySessions);
	for (TelemetrySession const& session : sessions)
	{
		CHECK(session.DroppedValues == 0);
		TelemetryChannel const* const channel = FindTelemetryChannel(session, "Thread");
		REQUIRE(channel != nullptr);
		CHECK(channel->Count == ThreadsPerSession);
	}
	CHECK(Telemetry::GetStatus().DroppedValues == 0);
	CHECK(Telemetry::GetStatus().Buffers <= std::max(GetStartBuffers(), uint32_t{ThreadsPerSession} + 4));
}

TEST_CASE("Telemetry: as many threads as Start made buffers record in the first step, and one more drops for want of one ready")
{
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	uint32_t const ready = Telemetry::GetStatus().Buffers;
	CHECK(ready == GetStartBuffers());
	// More than the four a step's collection makes free: the threads of a parallel loop in a session's first step.
	REQUIRE(ready > 4);
	uint64_t const logStart = Log::GetNextEntryIndex();
	// The threads end without EndThread, so each holds its buffer until the session ends.
	for (uint32_t index = 0; index < ready; ++index)
	{
		RunOnThread(
			[index]
			{
				Telemetry::Record("Thread." + std::to_string(index), 1.0f);
			});
	}
	bool const allMade = ready == Telemetry::MaxThreads;
	RunOnThread(
		[]
		{
			Telemetry::Record("One more", 1.0f);
		});
	Telemetry::EndStep(MakeTelemetryStep(1));
	TelemetrySession const& session = GetNewestTelemetrySession();
	for (uint32_t index = 0; index < ready; ++index)
	{
		CHECK(FindTelemetryChannel(session, "Thread." + std::to_string(index)) != nullptr);
	}
	CHECK(FindTelemetryChannel(session, "One more") == nullptr);
	CHECK(session.DroppedValues == 1);
	std::vector<std::string> const warnings = GetTelemetryWarnings(logStart);
	REQUIRE(warnings.size() == 1);
	// Only a machine of 60 hardware threads or more has every buffer made at Start.
	CHECK(warnings[0].find(allMade ? "all 64 are held" : "no free buffer ready") != std::string::npos);

	// The collection made buffers free again: a thread that begins now records.
	if (!allMade)
	{
		RunOnThread(
			[]
			{
				Telemetry::Record("After the step", 2.0f);
			});
		Telemetry::EndStep(MakeTelemetryStep(2));
		CHECK(FindTelemetryChannel(GetNewestTelemetrySession(), "After the step") != nullptr);
		CHECK(GetNewestTelemetrySession().DroppedValues == 1);
	}
}

TEST_CASE("Telemetry: a session that begins frees the buffers its threads held, and makes none while four were free")
{
	TelemetryScope const telemetry;
	REQUIRE(Telemetry::Start().IsOk());
	uint32_t const ready = Telemetry::GetStatus().Buffers;
	uint64_t const logStart = Log::GetNextEntryIndex();
	// Threads that hold every buffer but four through the first session, then record again in the next.
	uint32_t const holders = ready - 4;
	std::mutex mutex;
	std::condition_variable changed;
	int phase = 0;
	std::atomic<uint32_t> recorded{0};
	std::vector<std::thread> threads;
	for (uint32_t index = 0; index < holders; ++index)
	{
		threads.emplace_back(
			[&, index]
			{
				std::string const name = "Holder." + std::to_string(index);
				Telemetry::Record(name, 1.0f);
				recorded.fetch_add(1, std::memory_order_release);
				std::unique_lock guard(mutex);
				changed.wait(guard,
			                 [&phase]
			                 {
								 return phase == 1;
							 });
				guard.unlock();
				Telemetry::Record(name, 2.0f);
			});
	}
	while (recorded.load(std::memory_order_acquire) < holders)
	{
		std::this_thread::yield();
	}
	Telemetry::EndStep(MakeTelemetryStep(1));
	CHECK(Telemetry::GetStatus().Buffers == ready);
	// Another scene's step begins a session: four buffers were free and pass on, the held ones are freed.
	Telemetry::EndStep(MakeTelemetryStep(1, 2));
	CHECK(Telemetry::GetStatus().Buffers == ready);
	{
		std::scoped_lock const lock(mutex);
		phase = 1;
	}
	changed.notify_all();
	for (std::thread& thread : threads)
	{
		thread.join();
	}
	Telemetry::EndStep(MakeTelemetryStep(2, 2));
	TelemetrySession const& session = GetNewestTelemetrySession();
	CHECK(session.Source == 2);
	CHECK(session.DroppedValues == 0);
	for (uint32_t index = 0; index < holders; ++index)
	{
		TelemetryChannel const* const channel = FindTelemetryChannel(session, "Holder." + std::to_string(index));
		REQUIRE(channel != nullptr);
		CHECK(channel->Last == 2.0f);
	}
	CHECK(GetTelemetryWarnings(logStart).empty());
}

TEST_CASE("Telemetry: Start, Stop, EndSession, Init and Shutdown race threads that record")
{
	TelemetryScope const telemetry;
	constexpr int Threads = 4;
	std::atomic<bool> stop{false};
	std::vector<std::thread> threads;
	for (int thread = 0; thread < Threads; ++thread)
	{
		threads.emplace_back(
			[thread, &stop]
			{
				std::string const channel = "Racer" + std::to_string(thread);
				for (uint64_t record = 0; !stop.load(std::memory_order_relaxed); ++record)
				{
					Telemetry::Record(channel, static_cast<float>(record % 1000));
					if (record % 7 == 0)
					{
						Telemetry::Mark("Racer");
					}
					if (record % 101 == 0)
					{
						Telemetry::EndThread();
					}
				}
			});
	}
	uint64_t step = 1;
	for (int round = 0; round < 60; ++round)
	{
		TelemetrySettings settings;
		settings.RecordsPerThread = Telemetry::MinRecordsPerThread;
		REQUIRE(Telemetry::Start(settings).IsOk());
		for (int stepInRound = 0; stepInRound < 5; ++stepInRound)
		{
			Telemetry::Record("Main", static_cast<float>(round));
			Telemetry::EndStep(MakeTelemetryStep(step++));
		}
		if (round % 3 == 0)
		{
			Telemetry::EndSession();
			Telemetry::Record("Main", static_cast<float>(round));
			Telemetry::EndStep(MakeTelemetryStep(step++));
		}
		Telemetry::Stop();
		if (round % 20 == 19)
		{
			Telemetry::Shutdown();
			Telemetry::Init();
		}
	}
	stop.store(true, std::memory_order_relaxed);
	for (std::thread& thread : threads)
	{
		thread.join();
	}
	CHECK_FALSE(Telemetry::IsRecording());
	CHECK(Telemetry::GetStatus().Buffers == 0);
	for (TelemetrySession const& session : Telemetry::GetSessions())
	{
		CHECK_FALSE(session.Live);
		CHECK(FindTelemetryChannel(session, "Main") != nullptr);
	}
}

TEST_CASE("Telemetry: an idle recorder holds no buffers, and what is recorded while idle changes nothing")
{
	TelemetryScope const telemetry;
	CHECK(Telemetry::GetStatus().Buffers == 0);
	Telemetry::Record("Idle", 1.0f);
	Telemetry::Mark("Idle");
	RunOnThread(
		[]
		{
			Telemetry::Record("Idle", 1.0f);
			Telemetry::MarkStep("Idle", 1);
		});
	Telemetry::EndStep(MakeTelemetryStep(1));
	CHECK(Telemetry::GetSessions().empty());
	CHECK(Telemetry::GetStatus().Buffers == 0);

	REQUIRE(Telemetry::Start().IsOk());
	CHECK(Telemetry::GetStatus().Buffers == GetStartBuffers());
	Telemetry::EndStep(MakeTelemetryStep(1));
	Telemetry::Stop();
	CHECK(Telemetry::GetStatus().Buffers == 0);
	Telemetry::Record("Idle", 2.0f);
	Telemetry::EndStep(MakeTelemetryStep(2));
	REQUIRE(Telemetry::GetSessions().size() == 1);
	CHECK(Telemetry::GetSessions().front().Rows == 1);
	CHECK(Telemetry::GetSessions().front().Channels.empty());
}
