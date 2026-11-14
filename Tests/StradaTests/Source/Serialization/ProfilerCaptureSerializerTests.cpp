#include "Core/ProfilerTestUtilities.h"
#include "Fuzzing.h"
#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Core/Utf8.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/ProfilerCaptureSerializer.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Strada;

namespace
{
	constexpr int64_t Microsecond = 1000;

	// A capture of names that look like the format's own words: a zone named "Frame" on the main thread, one named like a GPU
	// pass, a thread named like a GPU view, value labels named like the arguments, times that tie, names that must be
	// escaped, and a name longer than a capture stores. Entries no frame uses (a thread, names) are left out by the writer.
	ProfilerCapture MakeTrickyCapture()
	{
		Testing::CaptureBuilder builder;
		builder.AddThread("Unused thread");
		uint32_t const gpuLookalike = builder.AddThread("GPU Viewport");
		builder.GetName("Unused name");
		builder.AddFrame(0, 16 * Microsecond * 1000, 2);
		ProfilerZone& frameZone = builder.AddZone(0, "Frame", 0, 0, 5 * Microsecond);
		builder.SetValue(frameZone, "Frame", 7);
		ProfilerZone& depthZone = builder.AddZone(0, "Frame", 1, 0, 5 * Microsecond);
		builder.SetValue(depthZone, "Depth", 18446744073709551615ull);
		builder.AddZone(0, "GPU.Opaque", 1, 0, 0);
		builder.AddZone(gpuLookalike, "Quote \" backslash \\ newline \n tab \t", 0, 3, 1);
		builder.AddZone(gpuLookalike,
		                "\xC3\x9C"
		                "n"
		                "\xC3\xAF"
		                "c"
		                "\xC3\xB8"
		                "d"
		                "\xC3\xA9 \xE2\x9C\x93 \xF0\x9F\x9A\xB2",
		                0, 3, 1);
		builder.AddZone(gpuLookalike, std::string(Profiler::MaxNameBytes, 'x'), 0, 3, 1);
		builder.AddGpuPass("Viewport", "Frame", 7 * Microsecond);
		builder.AddGpuPass("Viewport", "Opaque", 0);
		builder.AddFrame(16 * Microsecond * 1000, 33 * Microsecond * 1000);
		builder.AddZone(0, "Scene.Update", 0, 17 * Microsecond * 1000, 123456789);
		builder.AddGpuPass("Game", "Opaque", 1);
		return builder.Get();
	}

#ifdef ST_ENABLE_PROFILING
	// A capture the recorder made: zones of the main thread and two named threads, run-time names, counts, and GPU passes
	// of two views.
	ProfilerCapture RecordCapture()
	{
		Testing::ProfilerScope const profiler;
		REQUIRE(Profiler::Start().IsOk());
		std::array<ProfilerGpuPassTiming, 3> const passes{{{"Shadows", 1500}, {"Opaque", 250000}, {"Tonemap", 1}}};
		for (int frame = 0; frame < 4; ++frame)
		{
			Profiler::BeginFrame();
			{
				ST_PROFILE_SCOPE("Scene.Update");
				ST_PROFILE_ZONE(graph, "Graph.Graphs/Counter.sgraph");
				ST_PROFILE_ZONE_VALUE(graph, "Steps", 12 + frame);
				ST_PROFILE_SCOPE_DYNAMIC("Script.Game.Player " + std::to_string(frame % 2));
			}
			for (char const* threadName : {"Jolt Worker", "Audio Streaming"})
			{
				std::thread worker(
					[threadName]()
					{
						ST_PROFILE_THREAD(threadName);
						ST_PROFILE_SCOPE("Job");
						ST_PROFILE_SCOPE("Island");
					});
				worker.join();
			}
			Profiler::RecordGpuPasses(Profiler::GetFrameIndex(), "Viewport", passes);
			Profiler::RecordGpuPasses(Profiler::GetFrameIndex(), "Game", std::span<ProfilerGpuPassTiming const>(passes).first(1));
			Profiler::EndFrame();
		}
		Profiler::Stop();
		ProfilerCapture capture = Profiler::GetRecording();
		return capture;
	}
#endif

	std::string WriteCapture(ProfilerCapture const& capture)
	{
		Result<std::string> text = ProfilerCaptureSerializer::Write(capture);
		REQUIRE_MESSAGE(text.IsOk(), (text ? std::string() : text.GetError()));
		return text.TakeValue();
	}

	ProfilerCapture ReadCapture(std::string_view text)
	{
		Result<ProfilerCapture> capture = ProfilerCaptureSerializer::Read(text);
		REQUIRE_MESSAGE(capture.IsOk(), (capture ? std::string() : capture.GetError()));
		return capture.TakeValue();
	}

	std::vector<std::string> GetThreadNames(ProfilerCapture const& capture)
	{
		std::vector<std::string> names;
		for (ProfilerThread const& thread : capture.Threads)
		{
			names.push_back(thread.Name);
		}
		return names;
	}

	// The capture as written, read back: every table entry it holds is used by its frames.
	void CheckCompacted(ProfilerCapture const& capture)
	{
		std::vector<bool> usedNames(capture.Names.size(), false);
		std::vector<bool> usedThreads(capture.Threads.size(), false);
		usedThreads[0] = true;
		for (ProfilerFrame const& frame : capture.Frames)
		{
			for (ProfilerZone const& zone : frame.Zones)
			{
				usedThreads.at(zone.Thread) = true;
				usedNames.at(zone.Name) = true;
				if (zone.ValueLabel != ProfilerNoName)
				{
					usedNames.at(zone.ValueLabel) = true;
				}
			}
			for (ProfilerGpuPass const& pass : frame.GpuPasses)
			{
				usedNames.at(pass.View) = true;
				usedNames.at(pass.Name) = true;
			}
		}
		CHECK(std::find(usedNames.begin(), usedNames.end(), false) == usedNames.end());
		CHECK(std::find(usedThreads.begin(), usedThreads.end(), false) == usedThreads.end());
		for (std::string const& name : capture.Names)
		{
			CHECK(Utf8::IsValid(name));
			CHECK(name.size() <= Profiler::MaxNameBytes);
		}
	}

	// A written capture's document, damaged by edit, as text.
	std::string EditCapture(std::string const& text, std::function<void(Json&)> const& edit)
	{
		Result<Json> document = ParseJson(text);
		REQUIRE(document.IsOk());
		edit(document.GetValue());
		return document.GetValue().dump();
	}

	Json& FindEvent(Json& document, std::string_view phase, std::string_view name)
	{
		for (Json& event : document["traceEvents"])
		{
			if (event["ph"] == std::string(phase) && event["name"] == std::string(name))
			{
				return event;
			}
		}
		FAIL("no event " << name);
		return document;
	}
}

#ifdef ST_ENABLE_PROFILING
TEST_CASE("ProfilerCaptureSerializer: a recorded capture reads back as it was written")
{
	ProfilerCapture const recorded = RecordCapture();
	REQUIRE(recorded.Frames.size() == 4);
	std::string const text = WriteCapture(recorded);
	CHECK(text.find('\n') == std::string::npos);

	ProfilerCapture const read = ReadCapture(text);
	CHECK(Testing::DescribeCapture(read) == Testing::DescribeCapture(recorded));
	CHECK(GetThreadNames(read) == std::vector<std::string>{"Main", "Jolt Worker", "Audio Streaming"});
	CheckCompacted(read);
	CHECK(Testing::FindZones(read, "Graph.Graphs/Counter.sgraph").at(3).Value == 15);
	// Written again, the text is the same.
	CHECK(WriteCapture(read) == text);
}
#endif

TEST_CASE("ProfilerCaptureSerializer: names like the format's own words, tied times and unused entries round-trip")
{
	ProfilerCapture const capture = MakeTrickyCapture();
	std::string const text = WriteCapture(capture);
	ProfilerCapture const read = ReadCapture(text);
	CHECK(Testing::DescribeCapture(read) == Testing::DescribeCapture(capture));
	// The unused thread and name are left out; the GPU-like thread stays a thread.
	CHECK(GetThreadNames(read) == std::vector<std::string>{"Main", "GPU Viewport"});
	CheckCompacted(read);
	CHECK(read.Frames[0].DroppedZones == 2);
	CHECK(Testing::FindZones(read, "Frame").size() == 2);
	CHECK(WriteCapture(read) == text);

	// The format a trace viewer reads: the header, then the events; GPU views on a process of their own.
	Result<Json> const document = ParseJson(text);
	REQUIRE(document.IsOk());
	CHECK(document.GetValue()["Strada"] == Json::parse(R"({ "Version": 1, "Type": "ProfilerCapture" })"));
	CHECK(document.GetValue()["displayTimeUnit"] == "ms");
	int gpuThreadNames = 0;
	for (Json const& event : document.GetValue()["traceEvents"])
	{
		gpuThreadNames += event["ph"] == "M" && event["name"] == "thread_name" && event["pid"] == 2 ? 1 : 0;
		if (event["ph"] == "X" && event["pid"] == 1 && event["name"] == "Scene.Update")
		{
			CHECK(event["cat"] == "Scene");
			CHECK(event["ts"].get<double>() == doctest::Approx(17000.0));
		}
	}
	CHECK(gpuThreadNames == 2);
}

TEST_CASE("ProfilerCaptureSerializer: names are written as valid UTF-8 cut to the longest a capture stores")
{
	Testing::CaptureBuilder builder;
	builder.AddFrame(0, 1000);
	std::string const invalid = "Bad \xFF\xFE name";
	builder.AddZone(0, invalid, 0, 0, 1);
	// Each "é" is two bytes: the cut falls between characters.
	std::string longName(Profiler::MaxNameBytes - 1, 'a');
	longName += "\xC3\xA9\xC3\xA9";
	builder.AddZone(0, longName, 0, 0, 1);
	ProfilerCapture const read = ReadCapture(WriteCapture(builder.Get()));
	CheckCompacted(read);
	CHECK(Testing::FindZones(read, Utf8::Sanitize(invalid)).size() == 1);
	CHECK(Testing::FindZones(read, std::string(Profiler::MaxNameBytes - 1, 'a')).size() == 1);
}

TEST_CASE("ProfilerCaptureSerializer: the writer refuses what a reader would refuse")
{
	Testing::CaptureBuilder frames;
	for (uint32_t frame = 0; frame <= Profiler::MaxHistoryFrames; ++frame)
	{
		frames.AddFrame(frame, frame + 1);
	}
	CHECK(ProfilerCaptureSerializer::Write(frames.Get()).IsError());

	Testing::CaptureBuilder dangling;
	dangling.AddFrame(0, 1);
	dangling.AddZone(5, "Lost", 0, 0, 1);
	CHECK(ProfilerCaptureSerializer::Write(dangling.Get()).IsError());

	Testing::CaptureBuilder late;
	late.AddFrame(0, int64_t{1} << 52);
	CHECK(ProfilerCaptureSerializer::Write(late.Get()).IsError());

	ProfilerCapture threadless;
	CHECK(ProfilerCaptureSerializer::Write(threadless).IsError());

	// A frame's zones come thread by thread: a thread's zones after a later thread's are refused.
	Testing::CaptureBuilder ungrouped;
	uint32_t const worker = ungrouped.AddThread("Worker");
	ungrouped.AddFrame(0, 10);
	ungrouped.AddZone(0, "Scene.Update", 0, 0, 1);
	ungrouped.AddZone(worker, "Job", 0, 0, 1);
	ungrouped.AddZone(0, "Scene.Update", 0, 2, 1);
	Result<std::string> const refused = ProfilerCaptureSerializer::Write(ungrouped.Get());
	REQUIRE(refused.IsError());
	CHECK(refused.GetError().find("grouped by thread") != std::string::npos);
}

TEST_CASE("ProfilerCaptureSerializer: damaged captures are refused")
{
	std::string const valid = WriteCapture(MakeTrickyCapture());
	REQUIRE(ProfilerCaptureSerializer::Read(valid).IsOk());

	auto const refused = [](std::string const& text, std::string_view expected)
	{
		Result<ProfilerCapture> const read = ProfilerCaptureSerializer::Read(text);
		REQUIRE_MESSAGE(read.IsError(), text);
		CHECK_MESSAGE(read.GetError().find(expected) != std::string::npos, read.GetError());
	};
	refused("", "not a JSON object");
	refused("[1, 2]", "not a JSON object");
	refused(R"({ "traceEvents": [] })", "missing \"Strada\"");
	refused(R"({ "Strada": { "Version": 1, "Type": "Scene" }, "traceEvents": [] })", "expected a ProfilerCapture");
	refused(R"({ "Strada": { "Version": 2, "Type": "ProfilerCapture" }, "traceEvents": [] })", "newer");
	refused(R"({ "Strada": { "Version": 1, "Type": "ProfilerCapture" }, "traceEvents": {} })", "not an array");
	refused(R"({ "Strada": { "Version": 1, "Type": "ProfilerCapture" }, "traceEvents": [] })", "no main thread");
	refused(R"({ "Strada": { "Version": 1, "Type": "ProfilerCapture" }, "traceEvents": [ { "ph": "M", "name": ")" +
	            std::string(70000, 'n') + R"(" } ] })",
	        "longer than 65536 bytes");
	refused(R"({ "Strada": { "Version": 1, "Type": "ProfilerCapture", "Note": ")" + std::string(70000, 'n') + R"(" }, "traceEvents": [] })",
	        "header is too long");
	refused(
		R"({ "Strada": { "Version": 1, "Type": "ProfilerCapture" }, "Strada": { "Version": 1, "Type": "ProfilerCapture" }, "traceEvents": [] })",
		"twice");
	refused(EditCapture(valid,
	                    [](Json& document)
	                    {
							FindEvent(document, "X", "Scene.Update")["ph"] = "B";
						}),
	        "phase");
	refused(EditCapture(valid,
	                    [](Json& document)
	                    {
							FindEvent(document, "X", "Scene.Update")["dur"] = -1.0;
						}),
	        "\"dur\"");
	refused(EditCapture(valid,
	                    [](Json& document)
	                    {
							FindEvent(document, "X", "Scene.Update")["ts"] = 1.0e30;
						}),
	        "\"ts\"");
	refused(EditCapture(valid,
	                    [](Json& document)
	                    {
							FindEvent(document, "X", "Scene.Update")["args"]["Frame"] = 99;
						}),
	        "frame 99");
	refused(EditCapture(valid,
	                    [](Json& document)
	                    {
							FindEvent(document, "X", "Scene.Update")["tid"] = 9;
						}),
	        "thread 9");
	refused(EditCapture(valid,
	                    [](Json& document)
	                    {
							FindEvent(document, "X", "Scene.Update")["pid"] = 0;
						}),
	        "process 0");
	refused(EditCapture(valid,
	                    [](Json& document)
	                    {
							FindEvent(document, "X", "Opaque")["tid"] = 9;
						}),
	        "GPU view 9");
	refused(EditCapture(valid,
	                    [](Json& document)
	                    {
							for (Json& event : document["traceEvents"])
							{
								if (event["ph"] == "X" && event["pid"] == 1 && !event["args"].contains("Depth") &&
			                        event["args"]["Frame"] == 2)
								{
									event["args"]["Frame"] = 3;
								}
							}
						}),
	        "does not follow");
	refused(EditCapture(valid,
	                    [](Json& document)
	                    {
							FindEvent(document, "X", "Scene.Update")["name"] = std::string(Profiler::MaxNameBytes + 1, 'n');
						}),
	        "longer than");
	refused(EditCapture(valid,
	                    [](Json& document)
	                    {
							FindEvent(document, "X", "Scene.Update")["args"]["Value"] = Json::object({{"A", 1}, {"B", 2}});
						}),
	        "\"Value\"");
	refused(EditCapture(valid,
	                    [](Json& document)
	                    {
							FindEvent(document, "M", "thread_name")["tid"] = 2;
						}),
	        "out of order");
	// The first frame's last zone, on the second thread, moved to the main thread after that thread's zones.
	refused(EditCapture(valid,
	                    [](Json& document)
	                    {
							FindEvent(document, "X", std::string(Profiler::MaxNameBytes, 'x'))["tid"] = 1;
						}),
	        "grouped by thread");
}

TEST_CASE("ProfilerCaptureSerializer: damaged captures are refused or read, and what is read writes and reads again")
{
	std::vector<std::string> seeds = {WriteCapture(MakeTrickyCapture())};
#ifdef ST_ENABLE_PROFILING
	seeds.push_back(WriteCapture(RecordCapture()));
#endif
	Testing::DocumentMutator mutator(0xCA97u);
	int const rounds = Testing::GetFuzzRounds(300);
	int accepted = 0;
	for (int round = 0; round < rounds; ++round)
	{
		std::string const& seed = seeds[static_cast<size_t>(round) % seeds.size()];
		Result<Json> const original = ParseJson(seed);
		REQUIRE(original.IsOk());
		Json const damaged = mutator.Damage(original.GetValue(), round);
		std::string const text = DumpJson(damaged);
		Result<ProfilerCapture> const read = ProfilerCaptureSerializer::Read(text);
		if (!read)
		{
			continue;
		}
		accepted++;
		INFO("round " << round << ": " << text);
		std::string const written = WriteCapture(read.GetValue());
		ProfilerCapture const again = ReadCapture(written);
		// Written, a capture loses the threads and names no frame uses: the frames stay, and the text is then a fixed point.
		CHECK(Testing::DescribeCapture(again) == Testing::DescribeCapture(read.GetValue()));
		CHECK(WriteCapture(again) == written);
	}
	// Mutations of members a reader ignores (categories, process names) leave captures readable.
	CHECK(accepted > 0);
}

TEST_CASE("ProfilerCaptureSerializer: the writer, the reader and files keep within a capture's size")
{
	ProfilerCapture const capture = MakeTrickyCapture();
	std::string const text = WriteCapture(capture);
	uint64_t const size = text.size();
	CHECK(ProfilerCaptureSerializer::Write(capture, size).IsOk());
	Result<std::string> const larger = ProfilerCaptureSerializer::Write(capture, size - 1);
	REQUIRE(larger.IsError());
	CHECK(larger.GetError().find("HistoryFrames") != std::string::npos);
	CHECK(ProfilerCaptureSerializer::Read(text, size).IsOk());
	CHECK(ProfilerCaptureSerializer::Read(text, size - 1).IsError());

	Testing::TemporaryDirectory const directory;
	std::filesystem::path const path = directory.GetPath() / "Capture.json";
	CHECK(ProfilerCaptureSerializer::Save(capture, path, size - 1).IsError());
	CHECK_FALSE(FileSystem::Exists(path));
	REQUIRE(ProfilerCaptureSerializer::Save(capture, path).IsOk());
	CHECK(ProfilerCaptureSerializer::Load(path, size).IsOk());
	Result<ProfilerCapture> const refused = ProfilerCaptureSerializer::Load(path, size - 1);
	REQUIRE(refused.IsError());
	CHECK(refused.GetError().find("is larger than") != std::string::npos);
}

TEST_CASE("ProfilerCaptureSerializer: captures are saved to files and loaded from them")
{
	Testing::TemporaryDirectory const directory;
	std::filesystem::path const path = directory.GetPath() / "Captures" / "Run.json";
	CHECK(ProfilerCaptureSerializer::Load(path).IsError());
	ProfilerCapture const capture = MakeTrickyCapture();
	REQUIRE(ProfilerCaptureSerializer::Save(capture, path).IsOk());
	Result<ProfilerCapture> const loaded = ProfilerCaptureSerializer::Load(path);
	REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
	CHECK(Testing::DescribeCapture(loaded.GetValue()) == Testing::DescribeCapture(capture));
	CHECK(ProfilerCaptureSerializer::Load(directory.GetPath()).IsError());
}
