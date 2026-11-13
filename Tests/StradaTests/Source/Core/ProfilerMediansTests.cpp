#include "Core/ProfilerTestUtilities.h"

#include "Strada/Core/Profiler.h"

#include <doctest/doctest.h>

#include <cstdint>
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
			// An orphan (its parent was dropped) stands at its thread's root.
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
