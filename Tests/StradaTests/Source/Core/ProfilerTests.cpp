#include "Core/ProfilerTestUtilities.h"
#include "Fuzzing.h"

#include "Strada/Core/Platform.h"
#include "Strada/Core/Profiler.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Strada;
using Testing::ProfilerScope;

TEST_CASE("Profiler: every function does nothing before Init and after Shutdown")
{
	for (int round = 0; round < 2; ++round)
	{
		CHECK_FALSE(Profiler::IsInitialized());
		CHECK_FALSE(Profiler::IsRecording());
		CHECK(Profiler::Start().IsError());
		Profiler::BeginFrame();
		{
			ST_PROFILE_SCOPE("Ignored");
			ST_PROFILE_SCOPE_DYNAMIC(std::string("Ignored"));
		}
		Profiler::EndFrame();
		Profiler::Stop();
		Profiler::RecordGpuPasses(1, "View", {});
		Profiler::SetThreadName("Ignored");
		Profiler::EndThread();
		CHECK(Profiler::InternName("Ignored") == nullptr);
		CHECK(Profiler::GetFrameIndex() == 0);
		CHECK(Profiler::GetRecording().Frames.empty());
		ProfilerStatus const status = Profiler::GetStatus();
		CHECK_FALSE(status.Initialized);
		CHECK_FALSE(status.Recording);

		Profiler::Init();
		Profiler::Shutdown();
	}
}

#ifdef ST_DIST

TEST_CASE("Profiler: Dist builds refuse to record")
{
	ProfilerScope const profiler;
	CHECK_FALSE(Profiler::GetStatus().Available);
	CHECK(Profiler::Start().IsError());
}

#else

namespace
{
	// Settings with the given limits (designated initializers that leave members out draw -Wmissing-field-initializers
	// from some versions of Clang).
	constexpr ProfilerSettings DefaultSettings{};

	ProfilerSettings MakeSettings(uint32_t historyFrames, uint32_t historyZones, uint32_t zonesPerThread)
	{
		ProfilerSettings settings;
		settings.HistoryFrames = historyFrames;
		settings.HistoryZones = historyZones;
		settings.ZonesPerThread = zonesPerThread;
		return settings;
	}

	std::string_view GetName(ProfilerCapture const& capture, uint32_t name)
	{
		REQUIRE(name < capture.Names.size());
		return capture.Names[name];
	}

	std::vector<std::string> GetZoneNames(ProfilerCapture const& capture, ProfilerFrame const& frame)
	{
		std::vector<std::string> names;
		for (ProfilerZone const& zone : frame.Zones)
		{
			names.emplace_back(GetName(capture, zone.Name));
		}
		return names;
	}

	uint64_t CountZones(ProfilerCapture const& capture)
	{
		uint64_t count = 0;
		for (ProfilerFrame const& frame : capture.Frames)
		{
			count += frame.Zones.size();
		}
		return count;
	}

	uint64_t CountDropped(ProfilerCapture const& capture)
	{
		uint64_t count = 0;
		for (ProfilerFrame const& frame : capture.Frames)
		{
			count += frame.DroppedZones;
		}
		return count;
	}

	// Every index and time of a recording within its tables and its frame (checked once per property, since a recording
	// can hold millions of zones).
	void CheckWellFormed(ProfilerCapture const& capture)
	{
		REQUIRE_FALSE(capture.Threads.empty());
		CHECK(capture.Threads[0].Name == "Main");
		bool framesOrdered = true;
		bool zonesWithinTables = true;
		bool zonesGroupedByThread = true;
		bool timesValid = true;
		bool passesWithinTables = true;
		uint64_t previousIndex = 0;
		for (ProfilerFrame const& frame : capture.Frames)
		{
			framesOrdered = framesOrdered && frame.Index > previousIndex;
			previousIndex = frame.Index;
			timesValid = timesValid && frame.StartNanoseconds >= 0 && frame.EndNanoseconds >= frame.StartNanoseconds;
			uint32_t previousThread = 0;
			for (ProfilerZone const& zone : frame.Zones)
			{
				zonesWithinTables = zonesWithinTables && zone.Name < capture.Names.size() && zone.Thread < capture.Threads.size() &&
				                    (zone.ValueLabel == ProfilerNoName || zone.ValueLabel < capture.Names.size());
				zonesGroupedByThread = zonesGroupedByThread && zone.Thread >= previousThread;
				previousThread = zone.Thread;
				timesValid = timesValid && zone.StartNanoseconds >= 0 && zone.DurationNanoseconds >= 0;
			}
			for (ProfilerGpuPass const& pass : frame.GpuPasses)
			{
				passesWithinTables = passesWithinTables && pass.View < capture.Names.size() && pass.Name < capture.Names.size() &&
				                     pass.DurationNanoseconds >= 0;
			}
		}
		CHECK(framesOrdered);
		CHECK(zonesWithinTables);
		CHECK(zonesGroupedByThread);
		CHECK(timesValid);
		CHECK(passesWithinTables);
	}

	void RecordFunctionZone()
	{
		ST_PROFILE_FUNCTION();
	}

	// Records nested zones of random shape, each carrying its begin order as its value and remembering its depth.
	void RecordNestedZones(Testing::FuzzRandom& random, uint32_t depth, uint64_t limit, std::vector<uint32_t>& depths)
	{
		while (depths.size() < limit && random.Pick(3) != 0)
		{
			ST_PROFILE_ZONE(zone, "Tie");
			ST_PROFILE_ZONE_VALUE(zone, "Order", depths.size());
			depths.push_back(depth);
			if (depth < 12)
			{
				RecordNestedZones(random, depth + 1, limit, depths);
			}
		}
	}

	std::string CountedName(int& evaluations)
	{
		++evaluations;
		return "Counted";
	}
}

TEST_CASE("Profiler: initializing again records again from scratch")
{
	for (int round = 0; round < 3; ++round)
	{
		ProfilerScope const profiler;
		CHECK(Profiler::IsInitialized());
		CHECK(Profiler::GetStatus().Available);
		CHECK(Profiler::GetFrameIndex() == 0);
		REQUIRE(Profiler::Start().IsOk());
		Profiler::BeginFrame();
		{
			ST_PROFILE_SCOPE("Round.Zone");
			ST_PROFILE_SCOPE_DYNAMIC("Round " + std::to_string(round));
		}
		Profiler::EndFrame();
		ProfilerCapture const& capture = Profiler::GetRecording();
		REQUIRE(capture.Frames.size() == 1);
		CHECK(capture.Frames[0].Index == 1);
		CHECK(GetZoneNames(capture, capture.Frames[0]) == std::vector<std::string>{"Round.Zone", "Round " + std::to_string(round)});
		CHECK(capture.Threads.size() == 1);
	}
	CHECK_FALSE(Profiler::IsRecording());
}

TEST_CASE("Profiler: Start refuses settings out of bounds and a second recording")
{
	ProfilerScope const profiler;
	auto const refuses = [](ProfilerSettings const& settings)
	{
		Result<void> const started = Profiler::Start(settings);
		bool const refused = started.IsError() && !Profiler::IsRecording();
		if (!refused)
		{
			Profiler::Stop();
		}
		return refused;
	};
	CHECK(refuses(MakeSettings(0, DefaultSettings.HistoryZones, DefaultSettings.ZonesPerThread)));
	CHECK(refuses(MakeSettings(Profiler::MaxHistoryFrames + 1, DefaultSettings.HistoryZones, DefaultSettings.ZonesPerThread)));
	CHECK(refuses(MakeSettings(DefaultSettings.HistoryFrames, Profiler::MinHistoryZones - 1, DefaultSettings.ZonesPerThread)));
	CHECK(refuses(MakeSettings(DefaultSettings.HistoryFrames, Profiler::MaxHistoryZones + 1, DefaultSettings.ZonesPerThread)));
	CHECK(refuses(MakeSettings(DefaultSettings.HistoryFrames, DefaultSettings.HistoryZones, Profiler::MinZonesPerThread - 1)));
	CHECK(refuses(MakeSettings(DefaultSettings.HistoryFrames, DefaultSettings.HistoryZones, Profiler::MaxZonesPerThread + 1)));

	REQUIRE(Profiler::Start(MakeSettings(Profiler::MaxHistoryFrames, Profiler::MinHistoryZones, Profiler::MaxZonesPerThread)).IsOk());
	CHECK(Profiler::IsRecording());
	Result<void> const second = Profiler::Start();
	CHECK(second.IsError());
	CHECK(Profiler::IsRecording());
	ProfilerStatus const status = Profiler::GetStatus();
	CHECK(status.Recording);
	CHECK(status.HistoryFrames == Profiler::MaxHistoryFrames);
	CHECK(status.HistoryZones == Profiler::MinHistoryZones);
	Profiler::Stop();
	CHECK_FALSE(Profiler::IsRecording());
	Profiler::Stop();
}

TEST_CASE("Profiler: zones come out in pre-order with their depths, each inside its parent")
{
	ProfilerScope const profiler;
	REQUIRE(Profiler::Start().IsOk());
	Profiler::BeginFrame();
	{
		ST_PROFILE_SCOPE("Outer.First");
		{
			ST_PROFILE_SCOPE("Inner");
			{
				ST_PROFILE_SCOPE("Innermost");
			}
		}
		{
			ST_PROFILE_SCOPE("Sibling");
		}
	}
	{
		ST_PROFILE_SCOPE("Outer.Second");
	}
	RecordFunctionZone();
	Profiler::EndFrame();

	ProfilerCapture const& capture = Profiler::GetRecording();
	REQUIRE(capture.Frames.size() == 1);
	ProfilerFrame const& frame = capture.Frames[0];
	CHECK(GetZoneNames(capture, frame) ==
	      std::vector<std::string>{"Outer.First", "Inner", "Innermost", "Sibling", "Outer.Second", "RecordFunctionZone"});
	std::vector<uint32_t> depths;
	for (ProfilerZone const& zone : frame.Zones)
	{
		depths.push_back(zone.Depth);
		CHECK(zone.Thread == 0);
		CHECK(zone.StartNanoseconds >= frame.StartNanoseconds);
		CHECK(zone.StartNanoseconds + zone.DurationNanoseconds <= frame.EndNanoseconds);
	}
	CHECK(depths == std::vector<uint32_t>{0, 1, 2, 1, 0, 0});
	auto const contains = [&frame](size_t parent, size_t child)
	{
		ProfilerZone const& outer = frame.Zones[parent];
		ProfilerZone const& inner = frame.Zones[child];
		return inner.StartNanoseconds >= outer.StartNanoseconds &&
		       inner.StartNanoseconds + inner.DurationNanoseconds <= outer.StartNanoseconds + outer.DurationNanoseconds;
	};
	CHECK(contains(0, 1));
	CHECK(contains(1, 2));
	CHECK(contains(0, 3));
	CHECK(frame.Zones[4].StartNanoseconds >= frame.Zones[0].StartNanoseconds + frame.Zones[0].DurationNanoseconds);
	CHECK(frame.DroppedZones == 0);
	CheckWellFormed(capture);
}

TEST_CASE("Profiler: the pre-order comes from the order zones end in, not from their times")
{
	// Thousands of zones a few nanoseconds long: many begin on the same clock tick, so only the order in which they
	// ended and their depths can give the order in which they began.
	ProfilerScope const profiler;
	REQUIRE(Profiler::Start(MakeSettings(DefaultSettings.HistoryFrames, DefaultSettings.HistoryZones, Profiler::MaxZonesPerThread)).IsOk());
	Testing::FuzzRandom random(0x5EED0001u);
	std::vector<uint32_t> depths;
	Profiler::BeginFrame();
	while (depths.size() < 20000)
	{
		RecordNestedZones(random, 0, 20000, depths);
	}
	Profiler::EndFrame();

	ProfilerCapture const& capture = Profiler::GetRecording();
	REQUIRE(capture.Frames.size() == 1);
	ProfilerFrame const& frame = capture.Frames[0];
	CHECK(frame.DroppedZones == 0);
	REQUIRE(frame.Zones.size() == depths.size());
	bool inBeginOrder = true;
	for (size_t index = 0; index < frame.Zones.size() && inBeginOrder; ++index)
	{
		inBeginOrder = frame.Zones[index].Value == index && frame.Zones[index].Depth == depths[index] &&
		               GetName(capture, frame.Zones[index].ValueLabel) == "Order";
	}
	CHECK(inBeginOrder);
}

TEST_CASE("Profiler: run-time names are stored once, only while recording, and cut at a character")
{
	ProfilerScope const profiler;
	std::string const first = "Script.Player";
	std::string const second = "Script.Player";
	char const* const stored = Profiler::InternName(first);
	REQUIRE(stored != nullptr);
	CHECK(Profiler::InternName(second) == stored);
	CHECK(std::string_view(stored) == "Script.Player");

	int evaluations = 0;
	Profiler::BeginFrame();
	{
		ST_PROFILE_SCOPE_DYNAMIC(CountedName(evaluations));
	}
	Profiler::EndFrame();
	CHECK(evaluations == 0);

	REQUIRE(Profiler::Start().IsOk());
	Profiler::BeginFrame();
	{
		ST_PROFILE_SCOPE_DYNAMIC(CountedName(evaluations));
		{
			// The name's text dies before the frame is collected.
			std::string temporary = "Graph " + std::to_string(42);
			ST_PROFILE_ZONE_DYNAMIC(graphZone, temporary);
			ST_PROFILE_ZONE_VALUE(graphZone, "Steps", 7u);
			temporary.assign(temporary.size(), 'x');
		}
		std::string longName;
		for (int index = 0; index < 200; ++index)
		{
			longName += "\xC3\xA9"; // U+00E9, two bytes
		}
		ST_PROFILE_SCOPE_DYNAMIC(longName);
	}
	Profiler::EndFrame();
	CHECK(evaluations == 1);

	ProfilerCapture const& capture = Profiler::GetRecording();
	REQUIRE(capture.Frames.size() == 1);
	ProfilerFrame const& frame = capture.Frames[0];
	REQUIRE(frame.Zones.size() == 3);
	CHECK(GetName(capture, frame.Zones[0].Name) == "Counted");
	CHECK(GetName(capture, frame.Zones[1].Name) == "Graph 42");
	CHECK(GetName(capture, frame.Zones[1].ValueLabel) == "Steps");
	CHECK(frame.Zones[1].Value == 7);
	std::string_view const cut = GetName(capture, frame.Zones[2].Name);
	CHECK(cut.size() == Profiler::MaxNameBytes);
	CHECK(cut.substr(0, 2) == "\xC3\xA9");
	CHECK(cut.substr(cut.size() - 2) == "\xC3\xA9");
	CHECK(frame.Zones[0].ValueLabel == ProfilerNoName);
}

TEST_CASE("Profiler: idle zones and zones that span recordings are not recorded")
{
	ProfilerScope const profiler;
	{
		ProfilerZoneToken const idle = Profiler::BeginZone("Idle");
		CHECK(idle.Name == nullptr);
		Profiler::EndZone(idle);
	}

	ProfilerZoneToken const beforeStart = Profiler::BeginZone("BeforeStart");
	REQUIRE(Profiler::Start().IsOk());
	ProfilerZoneToken const spanning = Profiler::BeginZone("Spanning");
	CHECK(spanning.Name != nullptr);
	Profiler::BeginFrame();
	Profiler::EndZone(beforeStart);
	{
		ST_PROFILE_SCOPE("First");
	}
	Profiler::EndFrame();
	Profiler::Stop();
	REQUIRE(Profiler::Start().IsOk());
	Profiler::BeginFrame();
	Profiler::EndZone(spanning);
	{
		ST_PROFILE_SCOPE("Second");
	}
	Profiler::EndFrame();

	ProfilerCapture const& capture = Profiler::GetRecording();
	REQUIRE(capture.Frames.size() == 1);
	CHECK(GetZoneNames(capture, capture.Frames[0]) == std::vector<std::string>{"Second"});
	CHECK(capture.Frames[0].Zones[0].Depth == 0);
	CHECK(capture.Frames[0].DroppedZones == 0);
}

TEST_CASE("Profiler: frames are counted from 1, recorded only while recording, and never nest")
{
	ProfilerScope const profiler;
	CHECK(Profiler::GetFrameIndex() == 0);
	Profiler::EndFrame();
	CHECK(Profiler::GetFrameIndex() == 0);
	Profiler::BeginFrame();
	Profiler::EndFrame();
	Profiler::BeginFrame();
	CHECK(Profiler::GetFrameIndex() == 2);

	// Started inside frame 2: that frame is not recorded, and what ends before frame 3 begins is discarded.
	REQUIRE(Profiler::Start().IsOk());
	{
		ST_PROFILE_SCOPE("Discarded");
	}
	Profiler::EndFrame();
	{
		ST_PROFILE_SCOPE("AlsoDiscarded");
	}
	CHECK(Profiler::GetRecording().Frames.empty());

	Profiler::BeginFrame();
	{
		ST_PROFILE_SCOPE("Third");
	}
	// A BeginFrame inside a frame ends it first.
	Profiler::BeginFrame();
	{
		ST_PROFILE_SCOPE("Fourth");
	}
	Profiler::EndFrame();
	Profiler::EndFrame();

	ProfilerCapture const& capture = Profiler::GetRecording();
	REQUIRE(capture.Frames.size() == 2);
	CHECK(capture.Frames[0].Index == 3);
	CHECK(capture.Frames[1].Index == 4);
	CHECK(GetZoneNames(capture, capture.Frames[0]) == std::vector<std::string>{"Third"});
	CHECK(GetZoneNames(capture, capture.Frames[1]) == std::vector<std::string>{"Fourth"});
	CHECK(capture.Frames[0].EndNanoseconds <= capture.Frames[1].StartNanoseconds);
	CheckWellFormed(capture);

	// Stop discards the frame in progress and what it collected; the history stays until the next Start.
	Profiler::BeginFrame();
	{
		ST_PROFILE_SCOPE("Fifth");
	}
	Profiler::Stop();
	Profiler::EndFrame();
	CHECK(Profiler::GetRecording().Frames.size() == 2);
	CHECK(Profiler::GetStatus().FramesHeld == 2);
	CHECK(Profiler::GetFrameIndex() == 5);
	REQUIRE(Profiler::Start().IsOk());
	CHECK(Profiler::GetRecording().Frames.empty());
	CHECK(Profiler::GetRecording().Names.empty());
}

TEST_CASE("Profiler: the history keeps its last frames and at most its zones")
{
	ProfilerScope const profiler;
	auto const recordFrame = [](int zones)
	{
		Profiler::BeginFrame();
		for (int index = 0; index < zones; ++index)
		{
			ST_PROFILE_SCOPE("Zone");
		}
		Profiler::EndFrame();
	};

	REQUIRE(Profiler::Start(MakeSettings(3, DefaultSettings.HistoryZones, DefaultSettings.ZonesPerThread)).IsOk());
	for (int frame = 0; frame < 5; ++frame)
	{
		recordFrame(1);
	}
	ProfilerCapture const& capture = Profiler::GetRecording();
	REQUIRE(capture.Frames.size() == 3);
	CHECK(capture.Frames.front().Index == 3);
	CHECK(capture.Frames.back().Index == 5);
	Profiler::Stop();

	REQUIRE(Profiler::Start(MakeSettings(DefaultSettings.HistoryFrames, 256, 512)).IsOk());
	recordFrame(100);
	recordFrame(100);
	CHECK(Profiler::GetRecording().Frames.size() == 2);
	recordFrame(100);
	CHECK(Profiler::GetRecording().Frames.size() == 2);
	CHECK(Profiler::GetStatus().ZonesHeld == 200);
	// A frame that alone collects more than the history holds keeps the first ones and counts the rest.
	recordFrame(300);
	REQUIRE(Profiler::GetRecording().Frames.size() == 1);
	CHECK(Profiler::GetRecording().Frames[0].Zones.size() == 256);
	CHECK(Profiler::GetRecording().Frames[0].DroppedZones == 44);
	ProfilerStatus const status = Profiler::GetStatus();
	CHECK(status.ZonesHeld == 256);
	CHECK(status.DroppedZones == 44);
}

TEST_CASE("Profiler: a full buffer drops and counts zones, and their children stay at the thread's root")
{
	ProfilerScope const profiler;
	REQUIRE(Profiler::Start(MakeSettings(DefaultSettings.HistoryFrames, DefaultSettings.HistoryZones, 256)).IsOk());
	Profiler::BeginFrame();
	{
		// The 300 children end first: 256 fill the buffer, the other 44 and their parent are dropped.
		ST_PROFILE_SCOPE("Parent");
		for (int index = 0; index < 300; ++index)
		{
			ST_PROFILE_SCOPE("Child");
		}
	}
	Profiler::EndFrame();
	Profiler::BeginFrame();
	{
		ST_PROFILE_SCOPE("Next");
	}
	Profiler::EndFrame();

	ProfilerCapture const& capture = Profiler::GetRecording();
	REQUIRE(capture.Frames.size() == 2);
	ProfilerFrame const& full = capture.Frames[0];
	CHECK(full.DroppedZones == 45);
	REQUIRE(full.Zones.size() == 256);
	CHECK(std::all_of(full.Zones.begin(), full.Zones.end(),
	                  [&capture](ProfilerZone const& zone)
	                  {
						  return zone.Depth == 1 && GetName(capture, zone.Name) == "Child";
					  }));
	CHECK(capture.Frames[1].DroppedZones == 0);
	CHECK(GetZoneNames(capture, capture.Frames[1]) == std::vector<std::string>{"Next"});
	CHECK(Profiler::GetStatus().DroppedZones == 45);
}

TEST_CASE("Profiler: zones of several threads arrive once each, under their threads, and buffers return")
{
	ProfilerScope const profiler;
	REQUIRE(Profiler::Start().IsOk());
	constexpr int ThreadCount = 8;
	constexpr int ZonesPerWorker = 1000;
	constexpr int Waves = 10;
	for (int wave = 0; wave < Waves; ++wave)
	{
		std::atomic<bool> go = false;
		std::vector<std::thread> workers;
		for (int worker = 0; worker < ThreadCount; ++worker)
		{
			workers.emplace_back(
				[&go, worker]()
				{
					ST_PROFILE_THREAD("Worker " + std::to_string(worker));
					while (!go.load())
					{
						std::this_thread::yield();
					}
					for (int zoneIndex = 0; zoneIndex < ZonesPerWorker; ++zoneIndex)
					{
						ST_PROFILE_SCOPE("Work.Outer");
						ST_PROFILE_SCOPE("Inner");
					}
				});
		}
		Profiler::BeginFrame();
		go = true;
		// Frames end while the workers record: their zones spread over several collections.
		for (int frame = 0; frame < 3; ++frame)
		{
			Profiler::EndFrame();
			Profiler::BeginFrame();
		}
		for (std::thread& worker : workers)
		{
			worker.join();
		}
		Profiler::EndFrame();
	}

	ProfilerCapture const& capture = Profiler::GetRecording();
	CheckWellFormed(capture);
	CHECK(CountDropped(capture) == 0);
	// Each wave's threads take the entries of the previous wave's threads of the same name.
	REQUIRE(capture.Threads.size() == ThreadCount + 1);
	std::vector<uint64_t> zonesPerThread(capture.Threads.size(), 0);
	for (ProfilerFrame const& frame : capture.Frames)
	{
		for (size_t index = 0; index < frame.Zones.size(); ++index)
		{
			ProfilerZone const& zone = frame.Zones[index];
			++zonesPerThread[zone.Thread];
			if (GetName(capture, zone.Name) == "Inner")
			{
				// Pre-order: a child follows its parent, unless its parent ended after the collection that took the child,
				// which leaves the child last of its thread in the frame.
				CHECK(zone.Depth == 1);
				bool const lastOfThread = index + 1 == frame.Zones.size() || frame.Zones[index + 1].Thread != zone.Thread;
				bool const afterParent = index > 0 && frame.Zones[index - 1].Thread == zone.Thread &&
				                         GetName(capture, frame.Zones[index - 1].Name) == "Work.Outer";
				CHECK((afterParent || lastOfThread));
			}
		}
	}
	CHECK(zonesPerThread[0] == 0);
	for (int worker = 0; worker < ThreadCount; ++worker)
	{
		std::string const name = "Worker " + std::to_string(worker);
		auto const found = std::find_if(capture.Threads.begin(), capture.Threads.end(),
		                                [&name](ProfilerThread const& thread)
		                                {
											return thread.Name == name;
										});
		REQUIRE(found != capture.Threads.end());
		CHECK(zonesPerThread[static_cast<size_t>(found - capture.Threads.begin())] == uint64_t{2} * ZonesPerWorker * Waves);
	}
}

TEST_CASE("Profiler: tasks of std::async share entries by name and return their buffers")
{
	ProfilerScope const profiler;
	REQUIRE(Profiler::Start().IsOk());
	constexpr int TasksPerFrame = 4;
	constexpr int Frames = 50;
	for (int frame = 0; frame < Frames; ++frame)
	{
		Profiler::BeginFrame();
		std::vector<std::future<void>> tasks;
		for (int task = 0; task < TasksPerFrame; ++task)
		{
			tasks.push_back(std::async(std::launch::async,
			                           []()
			                           {
										   ST_PROFILE_THREAD("Task");
										   ST_PROFILE_SCOPE("Task.Run");
									   }));
		}
		for (std::future<void>& task : tasks)
		{
			task.get();
		}
		Profiler::EndFrame();
	}

	ProfilerCapture const& capture = Profiler::GetRecording();
	CheckWellFormed(capture);
	CHECK(CountZones(capture) == uint64_t{TasksPerFrame} * Frames);
	CHECK(CountDropped(capture) == 0);
	CHECK(capture.Threads.size() <= TasksPerFrame + 1);
	for (size_t index = 1; index < capture.Threads.size(); ++index)
	{
		CHECK(capture.Threads[index].Name == "Task");
	}
}

TEST_CASE("Profiler: a thread that ends in the profiler takes a new buffer when it records again")
{
	ProfilerScope const profiler;
	REQUIRE(Profiler::Start().IsOk());
	Profiler::BeginFrame();
	std::thread pooled(
		[]()
		{
			{
				ST_PROFILE_THREAD("Pooled");
				ST_PROFILE_SCOPE("Pooled.First");
			}
			{
				ST_PROFILE_THREAD("Pooled");
				ST_PROFILE_SCOPE("Pooled.Second");
			}
			// Unnamed after its last task.
			ST_PROFILE_SCOPE("Pooled.Unnamed");
		});
	pooled.join();
	Profiler::EndFrame();

	ProfilerCapture const& capture = Profiler::GetRecording();
	REQUIRE(capture.Frames.size() == 1);
	ProfilerFrame const& frame = capture.Frames[0];
	REQUIRE(frame.Zones.size() == 3);
	CHECK(frame.DroppedZones == 0);
	auto const findZone = [&capture, &frame](std::string_view name)
	{
		auto const found = std::find_if(frame.Zones.begin(), frame.Zones.end(),
		                                [&capture, name](ProfilerZone const& zone)
		                                {
											return GetName(capture, zone.Name) == name;
										});
		REQUIRE(found != frame.Zones.end());
		return *found;
	};
	ProfilerZone const first = findZone("Pooled.First");
	ProfilerZone const second = findZone("Pooled.Second");
	ProfilerZone const unnamed = findZone("Pooled.Unnamed");
	// Two occupants of one name in one frame never share an entry.
	CHECK(first.Thread != second.Thread);
	CHECK(capture.Threads[first.Thread].Name == "Pooled");
	CHECK(capture.Threads[second.Thread].Name == "Pooled");
	CHECK(capture.Threads[unnamed.Thread].Name == "Thread " + std::to_string(unnamed.Thread));
}

TEST_CASE("Profiler: threads beyond the buffers drop and count their zones")
{
	ProfilerScope const profiler;
	REQUIRE(Profiler::Start().IsOk());
	Profiler::BeginFrame();
	constexpr int ThreadCount = Profiler::MaxThreads + 8;
	std::atomic<int> recorded = 0;
	std::atomic<bool> release = false;
	std::vector<std::thread> threads;
	for (int index = 0; index < ThreadCount; ++index)
	{
		// None of them ends in the profiler, so each keeps its buffer.
		threads.emplace_back(
			[&recorded, &release]()
			{
				{
					ST_PROFILE_SCOPE("Crowd.Zone");
				}
				++recorded;
				while (!release.load())
				{
					std::this_thread::yield();
				}
			});
	}
	while (recorded.load() < ThreadCount)
	{
		std::this_thread::yield();
	}
	Profiler::EndFrame();
	release = true;
	for (std::thread& thread : threads)
	{
		thread.join();
	}

	ProfilerCapture const& capture = Profiler::GetRecording();
	REQUIRE(capture.Frames.size() == 1);
	ProfilerFrame const& frame = capture.Frames[0];
	CHECK(frame.Zones.size() + frame.DroppedZones == ThreadCount);
	CHECK(frame.DroppedZones >= ThreadCount - Profiler::MaxThreads);
	CheckWellFormed(capture);

	// The buffers of threads that never ended stay taken until the recording stops; the next one has them all.
	Profiler::Stop();
	REQUIRE(Profiler::Start().IsOk());
	Profiler::BeginFrame();
	std::thread later(
		[]()
		{
			ST_PROFILE_SCOPE("Later.Zone");
		});
	later.join();
	Profiler::EndFrame();
	REQUIRE(Profiler::GetRecording().Frames.size() == 1);
	CHECK(Profiler::GetRecording().Frames[0].Zones.size() == 1);
	CHECK(Profiler::GetRecording().Frames[0].DroppedZones == 0);
}

TEST_CASE("Profiler: Start, Stop, Init and Shutdown race threads that record")
{
	std::atomic<bool> running = true;
	std::vector<std::thread> workers;
	for (int worker = 0; worker < 4; ++worker)
	{
		workers.emplace_back(
			[&running, worker]()
			{
				uint64_t iteration = 0;
				while (running.load(std::memory_order_relaxed))
				{
					Profiler::SetThreadName("Racer");
					ST_PROFILE_SCOPE("Race.Outer");
					{
						ST_PROFILE_SCOPE_DYNAMIC("Race " + std::to_string(worker));
						ST_PROFILE_ZONE(counted, "Race.Counted");
						ST_PROFILE_ZONE_VALUE(counted, "Iteration", iteration);
					}
					if (++iteration % 64 == 0)
					{
						Profiler::EndThread();
					}
				}
				Profiler::EndThread();
			});
	}

	for (int round = 0; round < 50; ++round)
	{
		ProfilerScope const profiler;
		REQUIRE(
			Profiler::Start(MakeSettings(DefaultSettings.HistoryFrames, DefaultSettings.HistoryZones, Profiler::MinZonesPerThread)).IsOk());
		for (int frame = 0; frame < 4; ++frame)
		{
			Profiler::BeginFrame();
			// A short frame (a sleep would last a whole timer tick on Windows).
			auto const frameEnd = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
			while (std::chrono::steady_clock::now() < frameEnd)
			{
				std::this_thread::yield();
			}
			Profiler::EndFrame();
		}
		Profiler::BeginFrame();
		if (round % 2 == 0)
		{
			Profiler::Stop();
			CheckWellFormed(Profiler::GetRecording());
		}
		else
		{
			CheckWellFormed(Profiler::GetRecording());
		}
	}
	running = false;
	for (std::thread& worker : workers)
	{
		worker.join();
	}
	CHECK_FALSE(Profiler::IsInitialized());
}

TEST_CASE("Profiler: GPU passes attach to the frame that measured them")
{
	ProfilerScope const profiler;
	std::array<ProfilerGpuPassTiming, 2> const passes{{{"Shadows", 1000}, {"Opaque", 2000}}};

	// Not recording: dropped.
	Profiler::BeginFrame();
	Profiler::RecordGpuPasses(Profiler::GetFrameIndex(), "Viewport", passes);
	Profiler::EndFrame();

	REQUIRE(Profiler::Start(MakeSettings(3, DefaultSettings.HistoryZones, DefaultSettings.ZonesPerThread)).IsOk());
	Profiler::BeginFrame();
	uint64_t const first = Profiler::GetFrameIndex();
	// The frame in progress: waits for its end.
	Profiler::RecordGpuPasses(first, "Viewport", passes);
	Profiler::RecordGpuPasses(first + 1, "Viewport", passes);
	Profiler::EndFrame();
	REQUIRE(Profiler::GetRecording().Frames.size() == 1);
	CHECK(Profiler::GetRecording().Frames[0].GpuPasses.size() == 2);

	Profiler::BeginFrame();
	// A past frame the history holds: attached after what it has, a view that rendered twice adds its passes again.
	Profiler::RecordGpuPasses(first, "Game", std::span<ProfilerGpuPassTiming const>(passes).first(1));
	Profiler::RecordGpuPasses(first, "Viewport", passes);
	Profiler::EndFrame();
	ProfilerCapture const& capture = Profiler::GetRecording();
	ProfilerFrame const& measured = capture.Frames[0];
	REQUIRE(measured.GpuPasses.size() == 5);
	std::vector<std::string> names;
	for (ProfilerGpuPass const& pass : measured.GpuPasses)
	{
		names.push_back(std::string(GetName(capture, pass.View)) + "/" + std::string(GetName(capture, pass.Name)));
	}
	CHECK(names == std::vector<std::string>{"Viewport/Shadows", "Viewport/Opaque", "Game/Shadows", "Viewport/Shadows", "Viewport/Opaque"});
	CHECK(measured.GpuPasses[1].DurationNanoseconds == 2000);
	CHECK(capture.Frames[1].GpuPasses.empty());

	// A frame the history no longer holds, and views past the limit, are dropped.
	for (int frame = 0; frame < 3; ++frame)
	{
		Profiler::BeginFrame();
		Profiler::EndFrame();
	}
	Profiler::RecordGpuPasses(first, "Viewport", passes);
	for (uint32_t view = 0; view < Profiler::MaxGpuViews + 1; ++view)
	{
		Profiler::RecordGpuPasses(Profiler::GetFrameIndex(), "View " + std::to_string(view), passes);
	}
	ProfilerCapture const& later = Profiler::GetRecording();
	REQUIRE(later.Frames.size() == 3);
	CHECK(later.Frames.front().Index == first + 2);
	// Viewport and Game are two of the views: 14 more fit.
	CHECK(later.Frames.back().GpuPasses.size() == 2 * (Profiler::MaxGpuViews - 2));
	CheckWellFormed(later);
}

TEST_CASE("Profiler: the cost of a zone, idle and recording" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_PROFILER_BENCHMARK")))
{
	// A measurement, never asserted on timings: run with STRADA_PROFILER_BENCHMARK=1 in a Release build.
	using Clock = std::chrono::steady_clock;
	constexpr int IdleZones = 50000000;
	constexpr int ZonesPerFrame = 50000;
	constexpr int RecordedFrames = 200;
	ProfilerScope const profiler;

	auto const idleStart = Clock::now();
	for (int index = 0; index < IdleZones; ++index)
	{
		ST_PROFILE_SCOPE("Benchmark.Idle");
	}
	double const idleNanoseconds = std::chrono::duration<double, std::nano>(Clock::now() - idleStart).count() / IdleZones;

	auto const emptyStart = Clock::now();
	for (int index = 0; index < IdleZones; ++index)
	{
		std::atomic_signal_fence(std::memory_order_seq_cst);
	}
	double const loopNanoseconds = std::chrono::duration<double, std::nano>(Clock::now() - emptyStart).count() / IdleZones;

	REQUIRE(Profiler::Start(MakeSettings(2, DefaultSettings.HistoryZones, Profiler::MaxZonesPerThread)).IsOk());
	double recordNanoseconds = 0.0;
	double collectNanoseconds = 0.0;
	for (int frame = 0; frame < RecordedFrames; ++frame)
	{
		Profiler::BeginFrame();
		auto const recordStart = Clock::now();
		for (int index = 0; index < ZonesPerFrame; ++index)
		{
			ST_PROFILE_SCOPE("Benchmark.Recorded");
		}
		auto const collectStart = Clock::now();
		Profiler::EndFrame();
		auto const collectEnd = Clock::now();
		recordNanoseconds += std::chrono::duration<double, std::nano>(collectStart - recordStart).count();
		collectNanoseconds += std::chrono::duration<double, std::nano>(collectEnd - collectStart).count();
	}
	CHECK(Profiler::GetStatus().DroppedZones == 0);
	double const zones = static_cast<double>(ZonesPerFrame) * RecordedFrames;
	MESSAGE("Idle zone: " << idleNanoseconds << " ns (empty loop " << loopNanoseconds << " ns); recorded zone: "
	                      << recordNanoseconds / zones << " ns, collected at EndFrame: " << collectNanoseconds / zones << " ns");
}

#endif
