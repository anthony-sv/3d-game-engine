#include "Core/ProfilerTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Platform.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/SaveSlots.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;
using Testing::ProfilerScope;

// Measurements, never asserted on timings: run with STRADA_SAVE_BENCHMARK=1 in a Release build (Docs/Architecture.md §13.1,
// "The durable write"). The target is a 1 MB slot written and read back within 50 ms, judged without the durable write's
// waits for the device, whose time is reported beside it.
namespace
{
	constexpr size_t Rounds = 20;
	constexpr size_t SlotBytes = 1000 * 1000;
	constexpr size_t Samples = MaxScriptFieldListElements;

	// A lap's ghost at 10 samples a second, a list of 4,096 elements each (positions, rotations, the engine's speed), more
	// laps' positions until the text holds SlotBytes, and the race's splits and scalars.
	SaveSlotData MakeMegabyteSlot()
	{
		auto const makeLap = [](float offset)
		{
			std::vector<ScriptFieldValue> positions;
			positions.reserve(Samples);
			for (size_t index = 0; index < Samples; index++)
			{
				float const turn = static_cast<float>(index) * 0.0015f + offset;
				positions.push_back(ScriptFieldValue::FromVector3({std::cos(turn) * 412.37f, 0.5f + offset, std::sin(turn) * 297.61f}));
			}
			return ScriptFieldValue::FromList(ScriptFieldType::Vector3, std::move(positions));
		};
		SaveSlotData slot;
		std::vector<ScriptFieldValue> rotations;
		std::vector<ScriptFieldValue> speeds;
		rotations.reserve(Samples);
		speeds.reserve(Samples);
		for (size_t sample = 0; sample < Samples; sample++)
		{
			float const angle = static_cast<float>(sample) * 0.0015f;
			rotations.push_back(ScriptFieldValue::FromQuaternion(glm::angleAxis(angle, glm::vec3(0.0f, 1.0f, 0.0f))));
			speeds.push_back(ScriptFieldValue::FromFloat(40.0f + 30.0f * std::sin(angle * 7.0f)));
		}
		slot.Values["Ghost.Rotations"] = ScriptFieldValue::FromList(ScriptFieldType::Quaternion, std::move(rotations));
		slot.Values["Ghost.Speeds"] = ScriptFieldValue::FromList(ScriptFieldType::Float, std::move(speeds));
		slot.Values["Splits"] =
			ScriptFieldValue::FromList(ScriptFieldType::Float, {ScriptFieldValue::FromFloat(30.2f), ScriptFieldValue::FromFloat(61.7f),
		                                                        ScriptFieldValue::FromFloat(92.4f)});
		slot.Values["BestLap"] = ScriptFieldValue::FromFloat(92.418f);
		slot.Values["Laps"] = ScriptFieldValue::FromInt32(3);
		slot.Values["Rider"] = ScriptFieldValue::FromString("Rider");
		for (int lap = 0;; lap++)
		{
			slot.Values["Ghost.Positions." + std::to_string(lap)] = makeLap(static_cast<float>(lap) * 0.25f);
			Result<std::string> const text = WriteSaveSlotText(slot);
			REQUIRE(text.IsOk());
			if (text.GetValue().size() >= SlotBytes)
			{
				return slot;
			}
		}
	}

	double ToMilliseconds(int64_t nanoseconds)
	{
		return static_cast<double>(nanoseconds) / 1.0e6;
	}

	double GetPercentile(std::vector<int64_t> values, double fraction)
	{
		std::sort(values.begin(), values.end());
		size_t const index = std::min(values.size() - 1, static_cast<size_t>(fraction * static_cast<double>(values.size())));
		return ToMilliseconds(values[index]);
	}

	std::string Describe(std::vector<int64_t> const& durations)
	{
		return "median " + std::to_string(GetPercentile(durations, 0.5)) + " ms, 95th percentile " +
		       std::to_string(GetPercentile(durations, 0.95)) + " ms";
	}

	// The median of a zone per call in milliseconds, 0 when the recording has none.
	double GetZoneMilliseconds(ProfilerMedians const& medians, std::string_view name)
	{
		for (ProfilerZoneMedian const& zone : medians.Zones)
		{
			if (zone.Name == name)
			{
				return zone.MillisecondsPerCall;
			}
		}
		return 0.0;
	}
}

TEST_CASE("SaveSlotBenchmark: a 1 MB slot written and read back, with and without the durable write's waits" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_SAVE_BENCHMARK")))
{
	SaveSlotData const slot = MakeMegabyteSlot();
	Testing::TemporaryDirectory directory;
	GameDataScope const gameData(directory.GetPath() / "Data", GameDataLifetime::Kept);
	std::filesystem::path const plain = directory.GetPath() / "Plain" / "best.ssave";
	std::filesystem::path const durable = directory.GetPath() / "Durable" / "best.ssave";
	size_t bytes = 0;

	// Each piece timed by the clock alone, the profiler off: the text, the atomic write (no wait for the device), the
	// durable write, and the read back.
	std::vector<int64_t> serialize;
	std::vector<int64_t> write;
	std::vector<int64_t> writeDurably;
	std::vector<int64_t> readBack;
	std::vector<int64_t> withoutFlush;
	std::vector<int64_t> store;
	for (size_t round = 0; round < Rounds; round++)
	{
		int64_t const start = Profiler::Now();
		Result<std::string> const text = WriteSaveSlotText(slot);
		int64_t const serialized = Profiler::Now();
		REQUIRE(text.IsOk());
		bytes = text.GetValue().size();
		REQUIRE(FileSystem::WriteTextFile(plain, text.GetValue()).IsOk());
		int64_t const written = Profiler::Now();
		Result<std::string> const file = FileSystem::ReadTextFile(plain);
		REQUIRE(file.IsOk());
		SaveSlotRead const read = ReadSaveSlotText(file.GetValue());
		int64_t const readEnd = Profiler::Now();
		REQUIRE(read.Status == SaveSlotStatus::Present);
		CHECK(read.Data == slot);
		REQUIRE(FileSystem::WriteTextFileDurably(durable, text.GetValue()).IsOk());
		int64_t const durableEnd = Profiler::Now();

		serialize.push_back(serialized - start);
		write.push_back(written - serialized);
		readBack.push_back(readEnd - written);
		withoutFlush.push_back(readEnd - start);
		writeDurably.push_back(durableEnd - readEnd);

		// The store as a game reaches it: a durable write that checks the slot on disk first, then a read.
		int64_t const storeStart = Profiler::Now();
		REQUIRE(SaveSlotStore::Write("Best", slot).IsOk());
		Result<SaveSlotRead> const stored = SaveSlotStore::Read("Best");
		int64_t const storeEnd = Profiler::Now();
		REQUIRE(stored.IsOk());
		REQUIRE(stored.GetValue().Status == SaveSlotStatus::Present);
		store.push_back(storeEnd - storeStart);
	}

	// Of the read, the JSON parse alone.
	Result<std::string> const written = FileSystem::ReadTextFile(plain);
	REQUIRE(written.IsOk());
	std::vector<int64_t> parse;
	for (size_t round = 0; round < Rounds; round++)
	{
		int64_t const start = Profiler::Now();
		Result<Json> const document = ParseJson(written.GetValue());
		parse.push_back(Profiler::Now() - start);
		REQUIRE(document.IsOk());
	}

	// The store's zones, the profiler recording one frame a round.
	ProfilerScope const profiler;
	ProfilerSettings settings;
	settings.HistoryFrames = static_cast<uint32_t>(Rounds);
	REQUIRE(Profiler::Start(settings).IsOk());
	for (size_t round = 0; round < Rounds; round++)
	{
		Profiler::BeginFrame();
		REQUIRE(SaveSlotStore::Write("Best", slot).IsOk());
		REQUIRE(SaveSlotStore::Read("Best").IsOk());
		Profiler::EndFrame();
	}
	Profiler::Stop();
	ProfilerMedians const medians = ComputeProfilerMedians(Profiler::GetRecording());

	CHECK(bytes >= SlotBytes);
	MESSAGE("a slot of " << bytes << " bytes over " << Rounds << " rounds: written and read back without the flush (text, atomic "
	                     << "write, read) " << Describe(withoutFlush) << "; text " << Describe(serialize) << ", atomic write "
	                     << Describe(write) << ", read and parse " << Describe(readBack) << " (the JSON parse alone " << Describe(parse)
	                     << "); durable write " << Describe(writeDurably) << ", so the flush adds "
	                     << (GetPercentile(writeDurably, 0.5) - GetPercentile(write, 0.5))
	                     << " ms at the median; the store's durable write and read " << Describe(store) << "; zones per call: "
	                     << "Saves.WriteSlot " << GetZoneMilliseconds(medians, "Saves.WriteSlot") << " ms, Saves.ReadSlot "
	                     << GetZoneMilliseconds(medians, "Saves.ReadSlot") << " ms, SyncFile " << GetZoneMilliseconds(medians, "SyncFile")
	                     << " ms");
}

TEST_CASE("SaveSlotBenchmark: the 1 MB slot set, saved, opened and read back by a C# script, through the scripting API" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_SAVE_BENCHMARK")))
{
	// The probe builds the same slot as MakeMegabyteSlot: as many laps of positions.
	SaveSlotData const slot = MakeMegabyteSlot();
	int32_t const laps = static_cast<int32_t>(std::count_if(slot.Values.begin(), slot.Values.end(),
	                                                        [](auto const& value)
	                                                        {
																return value.first.starts_with("Ghost.Positions.");
															}));
	Testing::ScriptEngineScope scripting;
	Testing::TemporaryDirectory directory;
	GameDataScope const gameData(directory.GetPath(), GameDataLifetime::Kept);
	Scene scene("Benchmark");
	Entity probe = scene.CreateEntity("Probe");
	ScriptComponent& script = probe.AddComponent<ScriptComponent>();
	script.ClassName = "Strada.Tests.SaveSlotProbe";
	script.Fields["Slot"] = ScriptFieldValue::FromString("Benchmark");
	script.Fields["BenchmarkRounds"] = ScriptFieldValue::FromInt32(static_cast<int32_t>(Rounds));
	script.Fields["BenchmarkLaps"] = ScriptFieldValue::FromInt32(laps);
	scene.OnRuntimeStart();
	std::string const result = probe.GetName();
	scene.OnRuntimeStop();
	Result<std::string> const file = FileSystem::ReadTextFile(directory.GetPath() / "Saves" / "benchmark.ssave");
	REQUIRE(file.IsOk());
	CHECK(file.GetValue().size() >= SlotBytes);
	CHECK(result.starts_with("Benchmark: set "));
	MESSAGE("a slot of " << file.GetValue().size() << " bytes over " << Rounds << " rounds, through the scripting API (each round a save "
	                     << "of a new object's values and an open with every value read back; the store's durable write is in the save): "
	                     << result);
}
