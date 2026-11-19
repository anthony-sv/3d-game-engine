#include "Core/ProfilerTestUtilities.h"
#include "Core/TelemetryTestUtilities.h"

#include "Strada/Core/Platform.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Core/Telemetry.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace Strada;
using Testing::GetNewestTelemetrySession;
using Testing::MakeTelemetryStep;
using Testing::ProfilerScope;
using Testing::TelemetryScope;

// Measurements, never asserted on timings: run with STRADA_TELEMETRY_BENCHMARK=1 in a Release build (Docs/Architecture.md
// §4.5, "Measuring"). The target is 64 channels recorded and collected within 0.05 ms a fixed step.
namespace
{
	constexpr size_t Channels = 64;

	std::array<std::string, Channels> MakeChannelNames()
	{
		std::array<std::string, Channels> names;
		for (size_t channel = 0; channel < Channels; ++channel)
		{
			std::string const number = std::to_string(channel);
			names[channel] = "Benchmark.Channel" + std::string(number.size() < 2 ? 1 : 0, '0') + number;
		}
		return names;
	}

	// 64 channels recorded on the main thread, then the step's row collected: what one fixed step costs.
	void RecordStep(std::array<std::string, Channels> const& names, uint64_t step)
	{
		for (size_t channel = 0; channel < Channels; ++channel)
		{
			Telemetry::Record(names[channel], static_cast<float>(step + channel));
		}
		Telemetry::EndStep(MakeTelemetryStep(step));
	}

	double ToMicroseconds(int64_t nanoseconds)
	{
		return static_cast<double>(nanoseconds) / 1000.0;
	}

	double GetPercentile(std::vector<int64_t> values, double fraction)
	{
		std::sort(values.begin(), values.end());
		size_t const index = std::min(values.size() - 1, static_cast<size_t>(fraction * static_cast<double>(values.size())));
		return ToMicroseconds(values[index]);
	}
}

TEST_CASE("TelemetryBenchmark: 64 channels recorded on the main thread and collected, each fixed step" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_TELEMETRY_BENCHMARK")))
{
	constexpr uint64_t Steps = 3600;
	std::array<std::string, Channels> const names = MakeChannelNames();
	TelemetryScope const telemetry;

	// Timed by the clock alone, the profiler off.
	REQUIRE(Telemetry::Start().IsOk());
	std::vector<int64_t> durations;
	durations.reserve(Steps);
	for (uint64_t step = 1; step <= Steps; ++step)
	{
		int64_t const start = Profiler::Now();
		RecordStep(names, step);
		durations.push_back(Profiler::Now() - start);
	}
	Telemetry::Stop();
	TelemetrySession const& session = GetNewestTelemetrySession();
	CHECK(session.Rows == Steps);
	CHECK(session.Channels.size() == Channels);
	CHECK(session.DroppedValues == 0);

	// The collection's own zone, the profiler recording one frame a step.
	ProfilerScope const profiler;
	ProfilerSettings settings;
	settings.HistoryFrames = static_cast<uint32_t>(Steps);
	REQUIRE(Profiler::Start(settings).IsOk());
	REQUIRE(Telemetry::Start().IsOk());
	for (uint64_t step = 1; step <= Steps; ++step)
	{
		Profiler::BeginFrame();
		RecordStep(names, step);
		Profiler::EndFrame();
	}
	Telemetry::Stop();
	Profiler::Stop();
	ProfilerMedians const medians = ComputeProfilerMedians(Profiler::GetRecording());
	double collect = 0.0;
	for (ProfilerZoneMedian const& zone : medians.Zones)
	{
		if (zone.Name == "Telemetry.Collect")
		{
			collect = zone.MillisecondsPerCall * 1000.0;
		}
	}
	MESSAGE("64 channels a step over " << Steps << " steps: median " << GetPercentile(durations, 0.5) << " us, 99th percentile "
	                                   << GetPercentile(durations, 0.99) << " us, worst " << GetPercentile(durations, 1.0)
	                                   << " us; Telemetry.Collect median " << collect << " us");
}

TEST_CASE("TelemetryBenchmark: a long session of 64 channels makes at most one block per channel in any step" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_TELEMETRY_BENCHMARK")))
{
	constexpr uint64_t Steps = 300000;
	constexpr uint64_t BlockBytes = uint64_t{TelemetryChannel::BlockCells} * sizeof(float);
	std::array<std::string, Channels> const names = MakeChannelNames();
	TelemetryScope const telemetry;
	TelemetrySettings settings;
	settings.MaxSteps = static_cast<uint32_t>(Steps);
	settings.HistoryBytes = 128ull * 1024 * 1024;
	REQUIRE(Telemetry::Start(settings).IsOk());

	std::vector<int64_t> durations;
	durations.reserve(Steps);
	uint64_t mostBlocks = 0;
	uint64_t mostBlocksAfterFirst = 0;
	uint64_t bytes = 0;
	for (uint64_t step = 1; step <= Steps; ++step)
	{
		int64_t const start = Profiler::Now();
		RecordStep(names, step);
		durations.push_back(Profiler::Now() - start);
		// Bytes grow by whole blocks: the blocks the step made, a column's next block made ahead of need included.
		uint64_t const held = GetNewestTelemetrySession().Bytes;
		uint64_t const blocks = (held - bytes) / BlockBytes;
		bytes = held;
		mostBlocks = std::max(mostBlocks, blocks);
		if (step > 1)
		{
			mostBlocksAfterFirst = std::max(mostBlocksAfterFirst, blocks);
		}
	}
	Telemetry::Stop();
	TelemetrySession const& session = GetNewestTelemetrySession();
	CHECK(session.Rows == Steps);
	CHECK(session.DroppedValues == 0);
	CHECK(mostBlocks <= Channels);
	MESSAGE("64 channels over " << Steps << " steps: median " << GetPercentile(durations, 0.5) << " us, 99th percentile "
	                            << GetPercentile(durations, 0.99) << " us, worst " << GetPercentile(durations, 1.0)
	                            << " us; blocks made in one step at most " << mostBlocks << " (the first), " << mostBlocksAfterFirst
	                            << " after it; " << session.Bytes / (1024 * 1024) << " MiB held");
}
