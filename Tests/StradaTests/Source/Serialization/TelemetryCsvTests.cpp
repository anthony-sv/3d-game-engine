#include "Core/TelemetryTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Telemetry.h"
#include "Strada/Serialization/JsonTextWriter.h"
#include "Strada/Serialization/TelemetryCsv.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;
using Testing::GetNewestTelemetrySession;
using Testing::MakeTelemetryStep;
using Testing::TelemetryScope;

namespace
{
	constexpr std::string_view Gear = "Gear, \"top\"";

	// Records the session Tests/Data/Telemetry/Session.csv holds: five steps of half a second, two channels, marks.
	TelemetrySession const& RecordSession()
	{
		REQUIRE(Telemetry::Start().IsOk());
		Telemetry::Record("Speed", 1.5f);
		Telemetry::Record(Gear, 3.0f);
		Telemetry::Mark("Lap 1");
		Telemetry::EndStep(MakeTelemetryStep(1, 1, 0.5f));
		Telemetry::Record("Speed", 0.1f);
		Telemetry::EndStep(MakeTelemetryStep(2, 1, 0.5f));
		Telemetry::Record("Speed", -2.0f);
		Telemetry::Record(Gear, 4.0f);
		Telemetry::Mark("Sector 1.1");
		Telemetry::Mark("Say \"hi\"");
		Telemetry::EndStep(MakeTelemetryStep(3, 1, 0.5f));
		Telemetry::Record("Speed", 100000.0f);
		Telemetry::Mark("Lap 2");
		Telemetry::EndStep(MakeTelemetryStep(4, 1, 0.5f));
		Telemetry::EndStep(MakeTelemetryStep(5, 1, 0.5f));
		Telemetry::Stop();
		return GetNewestTelemetrySession();
	}

	std::vector<std::string> SplitLines(std::string_view text)
	{
		std::vector<std::string> lines;
		size_t start = 0;
		for (size_t end = text.find("\r\n"); end != std::string_view::npos; end = text.find("\r\n", start))
		{
			lines.emplace_back(text.substr(start, end - start));
			start = end + 2;
		}
		CHECK(start == text.size());
		return lines;
	}
}

TEST_CASE("TelemetryCsv: a session is written as the expected file, byte for byte")
{
	TelemetryScope const telemetry;
	TelemetrySession const& session = RecordSession();
	Result<std::string> const expected =
		FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "Telemetry" / "Session.csv");
	REQUIRE(expected.IsOk());
	Result<std::string> const written = TelemetryCsvWriter::Write(session);
	REQUIRE(written.IsOk());
	CHECK(written.GetValue() == expected.GetValue());
	CHECK(written.GetValue().starts_with(TelemetryCsvWriter::HeaderStart));
}

TEST_CASE("TelemetryCsv: a lap's rows, some channels, and numbers as the JSON writer writes them")
{
	TelemetryScope const telemetry;
	TelemetrySession const& session = RecordSession();

	std::vector<TelemetryLap> const laps = FindTelemetryLaps(session);
	REQUIRE(laps.size() == 1);
	Result<std::string> const lap = TelemetryCsvWriter::Write(session, TelemetryCsvWriter::ForLap(laps.front()));
	REQUIRE(lap.IsOk());
	std::vector<std::string> const lapLines = SplitLines(lap.GetValue());
	REQUIRE(lapLines.size() == 4);
	CHECK(lapLines[1].starts_with("1,"));
	CHECK(lapLines[3].starts_with("3,"));

	TelemetryCsvOptions options;
	options.Channels = {std::string(Gear)};
	options.FirstRow = 2;
	Result<std::string> const gear = TelemetryCsvWriter::Write(session, options);
	REQUIRE(gear.IsOk());
	CHECK(SplitLines(gear.GetValue()) == std::vector<std::string>{R"("Step","Time","Gear, ""top""","Marks")",
	                                                              "3,1.5,4.0,\"Sector 1.1\nSay \"\"hi\"\"\"", "4,2.0,,\"Lap 2\"",
	                                                              "5,2.5,,"});
	options.Channels = {"Missing"};
	CHECK(TelemetryCsvWriter::Write(session, options).IsError());
	options.Channels = {"Speed", "Speed"};
	CHECK(TelemetryCsvWriter::Write(session, options).IsError());
	// Rows past the session's write the header alone.
	options.Channels.clear();
	options.FirstRow = 10;
	REQUIRE(TelemetryCsvWriter::Write(session, options).IsOk());
	CHECK(SplitLines(TelemetryCsvWriter::Write(session, options).GetValue()).size() == 1);

	// Each value as JsonTextWriter writes the double a float converts to.
	std::vector<float> const values = {0.1f, 1.0e-7f, 3.4e38f, -0.0f, 123456.789f, 16777216.0f, -1.5e-30f};
	REQUIRE(Telemetry::Start().IsOk());
	for (size_t index = 0; index < values.size(); ++index)
	{
		Telemetry::Record("Value", values[index]);
		Telemetry::EndStep(MakeTelemetryStep(index + 1, 2, 1.0f / 60.0f));
	}
	Telemetry::Stop();
	TelemetrySession const& numbers = GetNewestTelemetrySession();
	Result<std::string> const written = TelemetryCsvWriter::Write(numbers);
	REQUIRE(written.IsOk());
	std::vector<std::string> const lines = SplitLines(written.GetValue());
	REQUIRE(lines.size() == values.size() + 1);
	for (size_t index = 0; index < values.size(); ++index)
	{
		JsonTextWriter writer(JsonTextWriter::Style{false});
		writer.BeginArray();
		writer.Unsigned(index + 1);
		writer.Double(GetTelemetryTime(numbers, index));
		writer.Double(static_cast<double>(values[index]));
		writer.EndArray();
		std::string const array = writer.TakeText();
		// "[1,0.016666...,0.1...]" becomes the CSV line "1,0.016666...,0.1...,".
		CHECK(lines[index + 1] == array.substr(1, array.size() - 2) + ",");
	}
}

TEST_CASE("TelemetryCsv: the writer keeps within its bound, and an export is saved as written")
{
	TelemetryScope const telemetry;
	TelemetrySession const& session = RecordSession();
	Result<std::string> const written = TelemetryCsvWriter::Write(session);
	REQUIRE(written.IsOk());
	uint64_t const size = written.GetValue().size();
	CHECK(TelemetryCsvWriter::Write(session, {}, size).IsOk());
	CHECK(TelemetryCsvWriter::Write(session, {}, size - 1).IsError());
	CHECK(TelemetryCsvWriter::Write(session, {}, 10).IsError());

	Testing::TemporaryDirectory const directory;
	std::filesystem::path const path = directory.GetPath() / "Exports" / "Session.csv";
	REQUIRE(TelemetryCsvWriter::Save(session, path).IsOk());
	Result<std::string> const saved = FileSystem::ReadTextFile(path);
	REQUIRE(saved.IsOk());
	CHECK(saved.GetValue() == written.GetValue());
	CHECK(TelemetryCsvWriter::Save(session, directory.GetPath() / "Small.csv", {}, 10).IsError());
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "Small.csv"));
}
