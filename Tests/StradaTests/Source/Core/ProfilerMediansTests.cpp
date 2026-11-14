#include "Core/ProfilerTestUtilities.h"

#include "Strada/Core/Profiler.h"
#include "Strada/Serialization/ProfilerCaptureSerializer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	constexpr int64_t Millisecond = 1000000;

	double FindMedian(std::vector<ProfilerMedian> const& medians, std::string_view name)
	{
		for (ProfilerMedian const& median : medians)
		{
			if (median.Name == name)
			{
				return median.Milliseconds;
			}
		}
		FAIL("no median named " << name);
		return 0.0;
	}

	ProfilerZoneMedian FindZoneMedian(ProfilerMedians const& medians, std::string_view name)
	{
		for (ProfilerZoneMedian const& median : medians.Zones)
		{
			if (median.Name == name)
			{
				return median;
			}
		}
		FAIL("no zone median named " << name);
		return {};
	}

	std::vector<std::string> GetNames(std::vector<ProfilerMedian> const& medians)
	{
		std::vector<std::string> names;
		for (ProfilerMedian const& median : medians)
		{
			names.push_back(median.Name);
		}
		return names;
	}

	// Three frames of the same work on the main thread, two Jolt workers and the audio mixer; frames last 16, 30 and 17 ms.
	ProfilerCapture MakeCapture()
	{
		Testing::CaptureBuilder builder;
		uint32_t const workerA = builder.AddThread("Jolt Worker");
		uint32_t const workerB = builder.AddThread("Jolt Worker");
		uint32_t const mixer = builder.AddThread("Audio Mixer");
		int64_t const durations[] = {16, 30, 17};
		int64_t start = 0;
		for (int64_t const duration : durations)
		{
			builder.AddFrame(start, start + duration * Millisecond);
			builder.AddZone(0, "Application.Update", 0, start, 10 * Millisecond);
			builder.AddZone(0, "Scene.Update", 1, start, 8 * Millisecond);
			builder.AddZone(0, "Physics.Step", 2, start, 3 * Millisecond);
			// Unprefixed: in the subsystem of its parent.
			builder.AddZone(0, "Step", 3, start, 1 * Millisecond);
			// Scene again, inside Physics inside Scene: not the Scene subsystem's outermost zone.
			builder.AddZone(0, "Scene.Sync", 3, start + Millisecond, 1 * Millisecond);
			builder.AddZone(0, "Physics.Step", 2, start + 3 * Millisecond, 2 * Millisecond);
			// Other: unprefixed at the root; a zone in a zone of its own name counts once.
			builder.AddZone(0, "Loose", 0, start + 10 * Millisecond, 2 * Millisecond);
			builder.AddZone(0, "Recursive", 0, start + 12 * Millisecond, 4 * Millisecond);
			builder.AddZone(0, "Recursive", 1, start + 12 * Millisecond, 3 * Millisecond);
			// Workers: their outermost zones are their busy time, merged by name.
			builder.AddZone(workerA, "Job", 0, start, 1 * Millisecond);
			builder.AddZone(workerA, "Island", 1, start, Millisecond / 2);
			builder.AddZone(workerB, "Job", 0, start, 1 * Millisecond);
			// A zone deeper than the zones before it allow (a capture built by hand) stands at its thread's root.
			builder.AddZone(mixer, "Audio.Mix", 1, start, Millisecond / 2);
			start += duration * Millisecond;
		}
		return builder.Get();
	}
}

TEST_CASE("Profiler: a zone's subsystem is the text before the first dot of its name")
{
	CHECK(GetProfilerSubsystem("Physics.Step") == "Physics");
	CHECK(GetProfilerSubsystem("Script.Game.Player") == "Script");
	CHECK(GetProfilerSubsystem("Graph.Scripts/Player.sgraph") == "Graph");
	CHECK(GetProfilerSubsystem("Update").empty());
	CHECK(GetProfilerSubsystem(".Step").empty());
	CHECK(GetProfilerSubsystem("").empty());
}

TEST_CASE("Profiler: medians read subsystems on the main thread, busy time per thread name, zones and GPU passes")
{
	Testing::CaptureBuilder empty;
	ProfilerMedians const none = ComputeProfilerMedians(empty.Get());
	CHECK(none.FrameMilliseconds == 0.0);
	CHECK(none.Subsystems.empty());

	ProfilerCapture capture = MakeCapture();
	// The first frame's view rendered twice; the second rendered once; the third measured nothing.
	capture.Frames[0].GpuPasses.push_back(
		{static_cast<uint32_t>(capture.Names.size()), static_cast<uint32_t>(capture.Names.size() + 1), 1 * Millisecond});
	capture.Frames[0].GpuPasses.push_back(capture.Frames[0].GpuPasses.back());
	capture.Frames[1].GpuPasses.push_back(
		{static_cast<uint32_t>(capture.Names.size()), static_cast<uint32_t>(capture.Names.size() + 1), 3 * Millisecond});
	capture.Names.push_back("Viewport");
	capture.Names.push_back("Opaque");

	ProfilerMedians const medians = ComputeProfilerMedians(capture);
	CHECK(medians.FrameMilliseconds == doctest::Approx(17.0));
	CHECK(medians.FramePercentile99Milliseconds == doctest::Approx(30.0));

	CHECK(GetNames(medians.Subsystems) == std::vector<std::string>{"Application", "Other", "Physics", "Scene"});
	CHECK(FindMedian(medians.Subsystems, "Application") == doctest::Approx(10.0));
	CHECK(FindMedian(medians.Subsystems, "Scene") == doctest::Approx(8.0));
	CHECK(FindMedian(medians.Subsystems, "Physics") == doctest::Approx(5.0));
	CHECK(FindMedian(medians.Subsystems, "Other") == doctest::Approx(6.0));

	CHECK(GetNames(medians.Threads) == std::vector<std::string>{"Audio Mixer", "Jolt Worker"});
	CHECK(FindMedian(medians.Threads, "Jolt Worker") == doctest::Approx(2.0));
	CHECK(FindMedian(medians.Threads, "Audio Mixer") == doctest::Approx(0.5));

	ProfilerZoneMedian const step = FindZoneMedian(medians, "Physics.Step");
	CHECK(step.MillisecondsPerFrame == doctest::Approx(5.0));
	CHECK(step.MillisecondsPerCall == doctest::Approx(2.5));
	CHECK(step.CallsPerFrame == doctest::Approx(2.0));
	ProfilerZoneMedian const recursive = FindZoneMedian(medians, "Recursive");
	CHECK(recursive.MillisecondsPerFrame == doctest::Approx(4.0));
	CHECK(recursive.CallsPerFrame == doctest::Approx(1.0));
	CHECK(FindZoneMedian(medians, "Job").MillisecondsPerFrame == doctest::Approx(2.0));

	// Over the frames that measured the pass; a view that rendered twice adds both.
	CHECK(GetNames(medians.GpuPasses) == std::vector<std::string>{"Viewport/Opaque"});
	CHECK(FindMedian(medians.GpuPasses, "Viewport/Opaque") == doctest::Approx(2.5));
}

namespace
{
	// The medians as a dense count works them out: a value per frame for every series, and each zone's enclosing zones
	// searched one by one. The reference the linear computation is checked against.
	double DenseMedian(std::vector<double> values)
	{
		if (values.empty())
		{
			return 0.0;
		}
		std::sort(values.begin(), values.end());
		size_t const middle = values.size() / 2;
		return values.size() % 2 != 0 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
	}

	std::vector<ProfilerMedian> DenseMedians(std::map<std::string, std::vector<double>> const& series)
	{
		std::vector<ProfilerMedian> medians;
		for (auto const& [name, values] : series)
		{
			medians.push_back({name, DenseMedian(values)});
		}
		return medians;
	}

	ProfilerMedians ComputeDenseMedians(ProfilerCapture const& capture)
	{
		struct OpenZone
		{
			uint32_t Depth = 0;
			std::string Name;
			std::string Subsystem;
		};

		ProfilerMedians result;
		size_t const frameCount = capture.Frames.size();
		if (frameCount == 0)
		{
			return result;
		}
		auto const toMilliseconds = [](int64_t nanoseconds)
		{
			return static_cast<double>(nanoseconds) / 1.0e6;
		};
		auto const getSeries = [frameCount](std::map<std::string, std::vector<double>>& map,
		                                    std::string const& seriesName) -> std::vector<double>&
		{
			return map.try_emplace(seriesName, std::vector<double>(frameCount, 0.0)).first->second;
		};
		std::vector<double> frameTimes;
		std::map<std::string, std::vector<double>> subsystems;
		std::map<std::string, std::vector<double>> threads;
		std::map<std::string, std::vector<double>> zonesPerFrame;
		std::map<std::string, std::vector<double>> zonesPerCall;
		std::map<std::string, std::vector<double>> passes;
		for (size_t frameIndex = 0; frameIndex < frameCount; frameIndex++)
		{
			ProfilerFrame const& frame = capture.Frames[frameIndex];
			frameTimes.push_back(toMilliseconds(frame.EndNanoseconds - frame.StartNanoseconds));
			std::vector<OpenZone> stack;
			uint32_t stackThread = std::numeric_limits<uint32_t>::max();
			for (ProfilerZone const& zone : frame.Zones)
			{
				if (zone.Thread != stackThread)
				{
					stack.clear();
					stackThread = zone.Thread;
				}
				while (!stack.empty() && stack.back().Depth >= zone.Depth)
				{
					stack.pop_back();
				}
				std::string const name = capture.Names[zone.Name];
				std::string const own(GetProfilerSubsystem(name));
				std::string const subsystem = !own.empty() ? own : !stack.empty() ? stack.back().Subsystem : std::string("Other");
				double const milliseconds = toMilliseconds(zone.DurationNanoseconds);
				bool nestedInName = false;
				bool nestedInSubsystem = false;
				for (OpenZone const& open : stack)
				{
					nestedInName = nestedInName || open.Name == name;
					nestedInSubsystem = nestedInSubsystem || open.Subsystem == subsystem;
				}
				if (!nestedInName)
				{
					getSeries(zonesPerFrame, name)[frameIndex] += milliseconds;
					zonesPerCall[name].push_back(milliseconds);
				}
				if (zone.Thread == 0)
				{
					if (!nestedInSubsystem)
					{
						getSeries(subsystems, subsystem)[frameIndex] += milliseconds;
					}
				}
				else if (stack.empty())
				{
					getSeries(threads, capture.Threads[zone.Thread].Name)[frameIndex] += milliseconds;
				}
				stack.push_back({zone.Depth, name, subsystem});
			}
			std::map<std::string, double> framePasses;
			for (ProfilerGpuPass const& pass : frame.GpuPasses)
			{
				framePasses[capture.Names[pass.View] + "/" + capture.Names[pass.Name]] += toMilliseconds(pass.DurationNanoseconds);
			}
			for (auto const& [passName, milliseconds] : framePasses)
			{
				passes[passName].push_back(milliseconds);
			}
		}
		result.FrameMilliseconds = DenseMedian(frameTimes);
		std::vector<double> sorted = frameTimes;
		std::sort(sorted.begin(), sorted.end());
		size_t const rank = static_cast<size_t>(std::ceil(0.99 * static_cast<double>(sorted.size())));
		result.FramePercentile99Milliseconds = sorted[std::clamp<size_t>(rank, 1, sorted.size()) - 1];
		result.Subsystems = DenseMedians(subsystems);
		result.Threads = DenseMedians(threads);
		result.GpuPasses = DenseMedians(passes);
		for (auto const& [zoneName, perFrame] : zonesPerFrame)
		{
			std::vector<double> const& calls = zonesPerCall.at(zoneName);
			result.Zones.push_back(
				{zoneName, DenseMedian(perFrame), DenseMedian(calls), static_cast<double>(calls.size()) / static_cast<double>(frameCount)});
		}
		return result;
	}

	void CheckSameMedians(std::vector<ProfilerMedian> const& actual, std::vector<ProfilerMedian> const& expected)
	{
		REQUIRE(actual.size() == expected.size());
		for (size_t index = 0; index < actual.size(); index++)
		{
			CHECK(actual[index].Name == expected[index].Name);
			CHECK(actual[index].Milliseconds == doctest::Approx(expected[index].Milliseconds));
		}
	}

	void CheckSameMedians(ProfilerMedians const& actual, ProfilerMedians const& expected)
	{
		CHECK(actual.FrameMilliseconds == doctest::Approx(expected.FrameMilliseconds));
		CHECK(actual.FramePercentile99Milliseconds == doctest::Approx(expected.FramePercentile99Milliseconds));
		CheckSameMedians(actual.Subsystems, expected.Subsystems);
		CheckSameMedians(actual.Threads, expected.Threads);
		CheckSameMedians(actual.GpuPasses, expected.GpuPasses);
		REQUIRE(actual.Zones.size() == expected.Zones.size());
		for (size_t index = 0; index < actual.Zones.size(); index++)
		{
			CHECK(actual.Zones[index].Name == expected.Zones[index].Name);
			CHECK(actual.Zones[index].MillisecondsPerFrame == doctest::Approx(expected.Zones[index].MillisecondsPerFrame));
			CHECK(actual.Zones[index].MillisecondsPerCall == doctest::Approx(expected.Zones[index].MillisecondsPerCall));
			CHECK(actual.Zones[index].CallsPerFrame == doctest::Approx(expected.Zones[index].CallsPerFrame));
		}
	}

	// The capture as the writer writes it and the reader reads it back.
	ProfilerCapture ReadWritten(ProfilerCapture const& capture)
	{
		Result<std::string> text = ProfilerCaptureSerializer::Write(capture);
		REQUIRE_MESSAGE(text.IsOk(), (text ? std::string() : text.GetError()));
		Result<ProfilerCapture> read = ProfilerCaptureSerializer::Read(text.GetValue());
		REQUIRE_MESSAGE(read.IsOk(), (read ? std::string() : read.GetError()));
		return read.TakeValue();
	}
}

TEST_CASE("ProfilerMedians: frames without a series' zones count zero, as a dense count of every frame gives")
{
	// Random captures of few frames (odd and even counts), names that appear in some frames only (in half of them too),
	// nesting, threads of one name, negative times (a capture built by hand) and GPU passes.
	std::mt19937 random(20261113u);
	auto const pick = [&random](int low, int high)
	{
		return std::uniform_int_distribution<int>(low, high)(random);
	};
	std::array<char const*, 6> const names = {"Scene.Update", "Scene.Sync", "Physics.Step", "Loose", "Step", "Script.Player"};
	std::array<char const*, 3> const views = {"Viewport", "Game", "Thumbnails"};
	std::array<char const*, 2> const passes = {"Opaque", "Shadows"};
	for (int round = 0; round < 300; round++)
	{
		CAPTURE(round);
		Testing::CaptureBuilder builder;
		builder.AddThread("Jolt Worker");
		builder.AddThread("Jolt Worker");
		builder.AddThread("Audio Mixer");
		int const frames = pick(1, 10);
		for (int frame = 0; frame < frames; frame++)
		{
			int64_t const start = frame * 20 * Millisecond;
			builder.AddFrame(start, start + pick(-1, 20) * Millisecond);
			for (uint32_t thread = 0; thread < 4; thread++)
			{
				int const zones = pick(0, 6);
				uint32_t depth = 0;
				for (int zone = 0; zone < zones; zone++)
				{
					depth = zone == 0 ? 0 : static_cast<uint32_t>(pick(0, static_cast<int>(depth) + 1));
					builder.AddZone(thread, names[static_cast<size_t>(pick(0, static_cast<int>(names.size()) - 1))], depth, start,
					                pick(-1, 8) * Millisecond / 2);
				}
			}
			int const gpuPasses = pick(0, 3);
			for (int pass = 0; pass < gpuPasses; pass++)
			{
				builder.AddGpuPass(views[static_cast<size_t>(pick(0, 2))], passes[static_cast<size_t>(pick(0, 1))],
				                   pick(0, 4) * Millisecond);
			}
		}
		CheckSameMedians(ComputeProfilerMedians(builder.Get()), ComputeDenseMedians(builder.Get()));
	}
}
namespace
{
	// frames frames of 2 ms with a zone of 1 ms named Every.Frame each, the first also holding names zones of 1 us, each
	// of a name no other frame has.
	ProfilerCapture MakeOnceNamesCapture(uint32_t frames, uint32_t names)
	{
		ProfilerCapture capture;
		capture.Threads.push_back({"Main"});
		capture.Names.push_back("Every.Frame");
		for (uint32_t name = 0; name < names; name++)
		{
			capture.Names.push_back("Once.Name" + std::to_string(name));
		}
		for (uint32_t frame = 0; frame < frames; frame++)
		{
			ProfilerFrame& added = capture.Frames.emplace_back();
			added.Index = frame + 1;
			added.StartNanoseconds = int64_t{frame} * 2 * Millisecond;
			added.EndNanoseconds = added.StartNanoseconds + 2 * Millisecond;
			added.Zones.push_back({0, 0, 0, ProfilerNoName, 0, added.StartNanoseconds, Millisecond});
		}
		for (uint32_t name = 0; name < names; name++)
		{
			capture.Frames.front().Zones.push_back({name + 1, 0, 0, ProfilerNoName, 0, 0, 1000});
		}
		return capture;
	}

	void CheckOnceNames(ProfilerMedians const& medians, uint32_t frames, uint32_t names)
	{
		CHECK(medians.FrameMilliseconds == doctest::Approx(2.0));
		REQUIRE(medians.Zones.size() == names + 1);
		ProfilerZoneMedian const every = FindZoneMedian(medians, "Every.Frame");
		CHECK(every.MillisecondsPerFrame == doctest::Approx(1.0));
		CHECK(every.CallsPerFrame == doctest::Approx(1.0));
		ProfilerZoneMedian const once = FindZoneMedian(medians, "Once.Name" + std::to_string(names - 1));
		CHECK(once.MillisecondsPerFrame == 0.0);
		CHECK(once.MillisecondsPerCall == doctest::Approx(0.001));
		CHECK(once.CallsPerFrame == doctest::Approx(1.0 / frames));
		CHECK(FindMedian(medians.Subsystems, "Once") == 0.0);
		CHECK(FindMedian(medians.Subsystems, "Every") == doctest::Approx(1.0));
	}

	// One frame holding a chain of depth nested zones that names names in turn, the outer ones longer.
	ProfilerCapture MakeChainCapture(uint32_t depth, uint32_t names)
	{
		ProfilerCapture capture;
		capture.Threads.push_back({"Main"});
		for (uint32_t name = 0; name < names; name++)
		{
			capture.Names.push_back("Chain.Zone" + std::to_string(name));
		}
		ProfilerFrame& frame = capture.Frames.emplace_back();
		frame.Index = 1;
		frame.EndNanoseconds = int64_t{2} * depth + 1;
		for (uint32_t level = 0; level < depth; level++)
		{
			frame.Zones.push_back({level % names, 0, level, ProfilerNoName, 0, int64_t{level}, int64_t{2} * (depth - level)});
		}
		return capture;
	}

	void CheckChain(ProfilerMedians const& medians, uint32_t depth, uint32_t names)
	{
		REQUIRE(medians.Zones.size() == names);
		// A zone inside a zone of its own name counts once: the outer one.
		ProfilerZoneMedian const first = FindZoneMedian(medians, "Chain.Zone0");
		CHECK(first.MillisecondsPerFrame == doctest::Approx(2.0 * depth / 1.0e6));
		CHECK(first.CallsPerFrame == doctest::Approx(1.0));
		ProfilerZoneMedian const last = FindZoneMedian(medians, "Chain.Zone" + std::to_string(names - 1));
		CHECK(last.MillisecondsPerCall == doctest::Approx(2.0 * (depth - (names - 1)) / 1.0e6));
		// The subsystem's outermost zone alone.
		REQUIRE(medians.Subsystems.size() == 1);
		CHECK(FindMedian(medians.Subsystems, "Chain") == doctest::Approx(2.0 * depth / 1.0e6));
	}
}

// The full sizes are measured as built: the reader reads them too (they are within its bounds), but parsing a few
// hundred thousand events takes a minute in Debug builds, so smaller captures of the same shapes go through it.
TEST_CASE("ProfilerMedians: a capture of as many frames as the reader takes, with names in one frame only, is measured")
{
	// A value per frame and name would take frames times names (here 5.8 GB); the names' medians are 0.
	constexpr uint32_t Names = 20000;
	CheckOnceNames(ComputeProfilerMedians(MakeOnceNamesCapture(Profiler::MaxHistoryFrames, Names)), Profiler::MaxHistoryFrames, Names);
	CheckOnceNames(ComputeProfilerMedians(ReadWritten(MakeOnceNamesCapture(3000, 1000))), 3000, 1000);
}

TEST_CASE("ProfilerMedians: a chain of nested zones deeper than a capture holds names is measured")
{
	// Every name a capture holds, then the first ones again inside the last: searching each zone's enclosing zones would
	// take the chain's length squared.
	constexpr uint32_t Depth = Profiler::MaxCaptureNames + 4000;
	CheckChain(ComputeProfilerMedians(MakeChainCapture(Depth, Profiler::MaxCaptureNames)), Depth, Profiler::MaxCaptureNames);
	CheckChain(ComputeProfilerMedians(ReadWritten(MakeChainCapture(3000, 2000))), 3000, 2000);
}
