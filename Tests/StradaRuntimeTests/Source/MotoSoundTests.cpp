#include "RuntimeTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Audio/AudioScene.h"
#include "Strada/Audio/AudioTypes.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Platform/Process.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;
using namespace Strada::RuntimeTesting;

// The Moto sample heard through the player's --audio-capture at its footbridge camera while its ghost bike passes: the
// doppler of the ghost's engine and the echo under the footbridge. Each is measured as the difference between two
// captures of the same frames that differ in one setting, since the ghost's engine speed changes along the straight and a
// falling pitch alone would prove nothing. The predictions come from the copied project itself: the ghost's racing line
// (the keys of its timeline in Scripts/Ghost.sgraph), the camera's place, the reverb zone and the mixer's speed of sound.

namespace
{
	constexpr double SampleRate = 48000.0;
	constexpr double FrameSeconds = 1.0 / 60.0;
	constexpr uint32_t CaptureFrames = 320;
	// Set out this far into its lap, the ghost leaves a corner onto the back straight, passes the footbridge camera about
	// 2.7 s later and rides under the footbridge at about 3.4 s.
	constexpr double StartAt = 25.0;
	// One sample of a 16-bit capture.
	constexpr double LeastSignificantBit = 1.0 / 32768.0;

	Json* FindEntity(Json& scene, std::string_view tag)
	{
		for (Json& entity : scene["Entities"])
		{
			Json const& components = entity["Components"];
			if (components.contains("Tag") && components["Tag"].value("Tag", std::string()) == tag)
			{
				return &entity;
			}
		}
		return nullptr;
	}

	void SetVariable(Json& entity, char const* name, char const* type, Json value)
	{
		entity["Components"]["VisualScript"]["Variables"][name] = Json{{"Type", type}, {"Value", std::move(value)}};
	}

	glm::dvec3 ReadVector(Json const& value)
	{
		return glm::dvec3(value.at(0).get<double>(), value.at(1).get<double>(), value.at(2).get<double>());
	}

	// One track of a Flow.Timeline: keys of a time, a value and the slopes in and out (cubic), or linear to the next. A
	// float track's values are held in x.
	struct TimelineKey
	{
		double Time = 0.0;
		glm::dvec3 Value = glm::dvec3(0.0);
		glm::dvec3 In = glm::dvec3(0.0);
		glm::dvec3 Out = glm::dvec3(0.0);
		bool Linear = false;
	};

	glm::dvec3 ReadKeyValue(Json const& value)
	{
		return value.is_array() ? ReadVector(value) : glm::dvec3(value.get<double>(), 0.0, 0.0);
	}

	std::vector<TimelineKey> ReadTrack(Json const& timeline, std::string_view name)
	{
		std::vector<TimelineKey> keys;
		for (Json const& track : timeline.at("Tracks"))
		{
			if (track.value("Name", std::string()) != name)
			{
				continue;
			}
			for (Json const& key : track.at("Keys"))
			{
				TimelineKey& read = keys.emplace_back();
				read.Time = key.at("Time").get<double>();
				read.Value = ReadKeyValue(key.at("Value"));
				read.Linear = key.value("Interpolation", std::string()) == "Linear";
				if (!read.Linear)
				{
					read.In = ReadKeyValue(key.at("In"));
					read.Out = ReadKeyValue(key.at("Out"));
				}
			}
		}
		return keys;
	}

	// A track's value at a time as the timeline plays it (Strada-ScriptCore's CurveMath): the last key at or before the time
	// starts the segment (of keys at one time, the later), cubic Hermite from the keys' slopes, or linear.
	glm::dvec3 Evaluate(std::vector<TimelineKey> const& keys, double time)
	{
		if (time < keys.front().Time)
		{
			return keys.front().Value;
		}
		auto const after = std::ranges::upper_bound(keys, time, {}, &TimelineKey::Time);
		size_t const index = static_cast<size_t>(after - keys.begin()) - 1;
		if (index + 1 >= keys.size())
		{
			return keys.back().Value;
		}
		TimelineKey const& first = keys[index];
		TimelineKey const& second = keys[index + 1];
		double const span = second.Time - first.Time;
		double const s = (time - first.Time) / span;
		if (first.Linear)
		{
			return first.Value + (second.Value - first.Value) * s;
		}
		double const s2 = s * s;
		double const s3 = s2 * s;
		return (2.0 * s3 - 3.0 * s2 + 1.0) * first.Value + (s3 - 2.0 * s2 + s) * span * first.Out + (-2.0 * s3 + 3.0 * s2) * second.Value +
		       (s3 - s2) * span * second.In;
	}

	// A capture's samples, the two channels averaged; checks its header (16-bit PCM, stereo, 48 kHz).
	std::vector<double> ReadCapture(std::filesystem::path const& file)
	{
		Result<Buffer> read = FileSystem::ReadBinaryFile(file);
		REQUIRE(read.IsOk());
		std::span<uint8_t const> const bytes = read.GetValue().GetSpan();
		REQUIRE(bytes.size() >= 44);
		auto const littleEndian = [&bytes](size_t offset, uint32_t byteCount)
		{
			uint32_t value = 0;
			for (uint32_t index = 0; index < byteCount; index++)
			{
				value |= static_cast<uint32_t>(bytes[offset + index]) << (8 * index);
			}
			return value;
		};
		REQUIRE(littleEndian(22, 2) == 2);
		REQUIRE(littleEndian(24, 4) == 48000);
		REQUIRE(littleEndian(34, 2) == 16);
		size_t const frameCount = (bytes.size() - 44) / 4;
		std::vector<double> samples(frameCount);
		for (size_t frame = 0; frame < frameCount; frame++)
		{
			int16_t const left = static_cast<int16_t>(static_cast<uint16_t>(littleEndian(44 + frame * 4, 2)));
			int16_t const right = static_cast<int16_t>(static_cast<uint16_t>(littleEndian(46 + frame * 4, 2)));
			samples[frame] = 0.5 * (static_cast<double>(left) + static_cast<double>(right)) * LeastSignificantBit;
		}
		return samples;
	}

	struct Period
	{
		// In samples, refined between two lags.
		double Samples = 0.0;
		// The normalized autocorrelation at that lag: near 1 for a sound that repeats, low for noise or a pitch that moves.
		double Correlation = 0.0;
	};

	// The period of what repeats in samples [start, start + products + maxLag) between two lags: the lag of the highest
	// normalized autocorrelation, refined by a parabola through it and its neighbours.
	Period MeasurePeriod(std::vector<double> const& samples, size_t start, size_t products, size_t minLag, size_t maxLag)
	{
		REQUIRE(minLag >= 2);
		REQUIRE(start + products + maxLag + 1 <= samples.size());
		std::span<double const> const window(samples.data() + start, products + maxLag + 1);
		double mean = 0.0;
		for (double const value : window)
		{
			mean += value;
		}
		mean /= static_cast<double>(window.size());
		auto const correlation = [&window, mean, products](size_t lag)
		{
			double product = 0.0;
			double head = 0.0;
			double tail = 0.0;
			for (size_t index = 0; index < products; index++)
			{
				double const a = window[index] - mean;
				double const b = window[index + lag] - mean;
				product += a * b;
				head += a * a;
				tail += b * b;
			}
			return head > 0.0 && tail > 0.0 ? product / std::sqrt(head * tail) : 0.0;
		};
		size_t best = minLag;
		double bestCorrelation = correlation(minLag);
		for (size_t lag = minLag + 1; lag <= maxLag; lag++)
		{
			double const value = correlation(lag);
			if (value > bestCorrelation)
			{
				best = lag;
				bestCorrelation = value;
			}
		}
		double const before = correlation(best - 1);
		double const after = correlation(best + 1);
		double const curvature = before - 2.0 * bestCorrelation + after;
		double const shift = curvature < 0.0 ? 0.5 * (before - after) / curvature : 0.0;
		return {static_cast<double>(best) + std::clamp(shift, -0.5, 0.5), bestCorrelation};
	}

	double Rms(std::vector<double> const& a, std::vector<double> const* b, double fromSeconds, double toSeconds)
	{
		size_t const first = static_cast<size_t>(fromSeconds * SampleRate);
		size_t const last = std::min(static_cast<size_t>(toSeconds * SampleRate), a.size());
		REQUIRE(first < last);
		double sum = 0.0;
		for (size_t index = first; index < last; index++)
		{
			double const value = b != nullptr ? a[index] - (*b)[index] : a[index];
			sum += value * value;
		}
		return std::sqrt(sum / static_cast<double>(last - first));
	}
}

TEST_CASE("Runtime: Moto's ghost passing the footbridge camera falls in pitch by its doppler, and its engine echoes under the "
          "footbridge after it has left, as captures that differ in one setting show")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	std::filesystem::path const assets = project.parent_path() / "Assets";
	std::filesystem::path const scenePath = assets / "Scenes" / "Main.sscene";

	Result<std::string> sceneText = FileSystem::ReadTextFile(scenePath);
	REQUIRE(sceneText.IsOk());
	Result<Json> parsed = ParseJson(sceneText.GetValue());
	REQUIRE(parsed.IsOk());
	Json scene = parsed.TakeValue();
	Json* const director = FindEntity(scene, "Trackside");
	Json* const camera = FindEntity(scene, "Footbridge Camera");
	Json* const ghost = FindEntity(scene, "Ghost");
	Json* const engine = FindEntity(scene, "Engine Sound");
	Json* const echo = FindEntity(scene, "Footbridge Echo");
	REQUIRE(director != nullptr);
	REQUIRE(camera != nullptr);
	REQUIRE(ghost != nullptr);
	REQUIRE(engine != nullptr);
	REQUIRE(echo != nullptr);

	// The game starts on the footbridge camera, the director's child of that name (StartCamera counts them from 1), whose
	// listener hears the scene; the ghost sets out at StartAt.
	Json const& cameras = (*director)["Components"]["Relationship"]["Children"];
	auto const cameraIndex = std::find(cameras.begin(), cameras.end(), (*camera)["ID"]);
	REQUIRE(cameraIndex != cameras.end());
	SetVariable(*director, "StartCamera", "Int32", static_cast<int32_t>(cameraIndex - cameras.begin()) + 1);
	SetVariable(*ghost, "StartAt", "Float", StartAt);
	// The director stands at the origin unturned, so the camera's translation is its place in the world (as its graph
	// assumes when it turns the camera).
	glm::dvec3 const listener =
		ReadVector((*director)["Components"]["Transform"]["Translation"]) + ReadVector((*camera)["Components"]["Transform"]["Translation"]);
	glm::dvec3 const engineOffset = ReadVector((*engine)["Components"]["Transform"]["Translation"]);

	// Three captures of the same frames: with the zone, without it, and without it and without doppler.
	std::filesystem::path const captures = directory.GetPath() / "Captures";
	REQUIRE(FileSystem::CreateDirectories(captures).IsOk());
	auto const capture = [&](std::string const& name, float dopplerLevel, float sendLevel)
	{
		Json variant = scene;
		SetVariable(*FindEntity(variant, "Engine Sound"), "DopplerLevel", "Float", dopplerLevel);
		(*FindEntity(variant, "Footbridge Echo"))["Components"]["ReverbZone"]["SendLevel"] = sendLevel;
		REQUIRE(FileSystem::WriteTextFile(scenePath, DumpJson(variant)).IsOk());
		std::filesystem::path const file = captures / (name + ".wav");
		ProcessResult const run = RunPlayer({"--project", Native(project), "--headless", "--frames", std::to_string(CaptureFrames),
		                                     "--timestep", "0.016666666666666666", "--audio-capture", Native(file)});
		CAPTURE(name);
		CAPTURE(run.Output);
		REQUIRE(run.ExitCode == 0);
		CHECK_FALSE(Contains(run.Output, ".sgraph:"));
		std::vector<double> samples = ReadCapture(file);
		REQUIRE(samples.size() == static_cast<size_t>(CaptureFrames) * 800);
		return samples;
	};
	float const echoLevel = (*echo)["Components"]["ReverbZone"].value("SendLevel", 0.0f);
	REQUIRE(echoLevel > 0.0f);
	std::vector<double> const echoed = capture("Echoed", 1.0f, echoLevel);
	std::vector<double> const dry = capture("Dry", 1.0f, 0.0f);
	std::vector<double> const still = capture("Still", 0.0f, 0.0f);

	// The ghost's racing line, as its timeline plays it: its position and its engine speed.
	Result<std::string> graphText = FileSystem::ReadTextFile(assets / "Scripts" / "Ghost.sgraph");
	REQUIRE(graphText.IsOk());
	Result<Json> graph = ParseJson(graphText.GetValue());
	REQUIRE(graph.IsOk());
	std::optional<Json> timeline;
	for (Json const& node : graph.GetValue()["VisualScript"]["Graph"]["Nodes"])
	{
		if (node.contains("Settings") && node["Settings"].contains("Timeline"))
		{
			timeline = node["Settings"]["Timeline"];
		}
	}
	REQUIRE(timeline.has_value());
	std::vector<TimelineKey> const positions = ReadTrack(*timeline, "Position");
	std::vector<TimelineKey> const engineSpeeds = ReadTrack(*timeline, "EngineSpeed");
	REQUIRE(positions.size() >= 2);
	REQUIRE(engineSpeeds.size() >= 2);
	// Where the ghost's engine sounds at a frame of the capture (its lean turns the sound's small offset by a few degrees at
	// most on the straight, a few centimeters, which is left out).
	auto const source = [&positions, &engineOffset](uint32_t frame)
	{
		return Evaluate(positions, StartAt + frame * FrameSeconds) + engineOffset;
	};

	// Doppler: the pitch before the pass is higher and after it lower by what the ghost's speed predicts. Measured
	// between the two captures without the zone, so that no reverb is in what is measured.
	{
		Result<std::string> mixerText = FileSystem::ReadTextFile(assets / "Audio" / "Moto.smixer");
		REQUIRE(mixerText.IsOk());
		Result<Json> mixer = ParseJson(mixerText.GetValue());
		REQUIRE(mixer.IsOk());
		double const speedOfSound = mixer.GetValue()["Mixer"].value("SpeedOfSound", 343.0);
		// What a still listener hears of a moving source: c / (c - v), v the source's speed towards the listener, from its
		// motion over each frame as the engine takes it (the scene's record of its last update).
		auto const predicted = [&](uint32_t frame)
		{
			glm::dvec3 const at = source(frame);
			glm::dvec3 const velocity = (at - source(frame - 1)) / FrameSeconds;
			double const towards = glm::dot(velocity, glm::normalize(listener - at));
			return speedOfSound / (speedOfSound - towards);
		};
		// A gear change inside a window would move the pitch in it: the engine speed's keys jump there (two keys at one time).
		auto const shifts = [&engineSpeeds](double from, double to)
		{
			for (size_t index = 1; index < engineSpeeds.size(); index++)
			{
				double const time = engineSpeeds[index].Time;
				if (time == engineSpeeds[index - 1].Time && time >= from && time <= to)
				{
					return true;
				}
			}
			return false;
		};

		// Windows of 0.1 s. Before the pass: from 0.6 s, when the ghost is 80 m away, to 1.9 s, at 35 m. After it: 3.0 to
		// 3.4 s, riding away at 20 to 40 m. Between them the factor sweeps through 1 within each window, and later the
		// ghost brakes and shifts down for the next corner.
		struct Window
		{
			double Start;
			bool Approaching;
		};
		std::vector<Window> windows;
		for (int index = 0; index < 13; index++)
		{
			windows.push_back({0.6 + 0.1 * index, true});
		}
		for (int index = 0; index < 4; index++)
		{
			windows.push_back({3.0 + 0.1 * index, false});
		}
		constexpr size_t Products = 3600;
		int approaching = 0;
		int receding = 0;
		for (Window const& window : windows)
		{
			uint32_t const firstFrame = static_cast<uint32_t>(std::lround(window.Start / FrameSeconds));
			uint32_t const frames = static_cast<uint32_t>(std::lround(0.1 / FrameSeconds));
			if (shifts(StartAt + window.Start, StartAt + window.Start + 0.1))
			{
				continue;
			}
			double factor = 0.0;
			double engineSpeed = 0.0;
			for (uint32_t frame = firstFrame; frame < firstFrame + frames; frame++)
			{
				factor += predicted(frame);
				engineSpeed += Evaluate(engineSpeeds, StartAt + frame * FrameSeconds).x;
			}
			factor /= frames;
			engineSpeed /= frames;
			// A four-stroke engine's sound repeats every two turns of the crank: 120 / rpm seconds. The search spans 0.7
			// to 1.4 times that, which holds the doppler's 0.87 to 1.13 and neither half nor twice the period (the
			// twin-pulse firing order is nearly repeated after one turn).
			double const cycle = 120.0 / engineSpeed * SampleRate;
			size_t const minLag = static_cast<size_t>(std::floor(cycle * 0.7));
			size_t const maxLag = static_cast<size_t>(std::ceil(cycle * 1.4));
			size_t const start = static_cast<size_t>(window.Start * SampleRate);
			Period const heard = MeasurePeriod(dry, start, Products, minLag, maxLag);
			Period const unshifted = MeasurePeriod(still, start, Products, minLag, maxLag);
			double const measured = unshifted.Samples / heard.Samples;
			CAPTURE(window.Start);
			CAPTURE(engineSpeed);
			CAPTURE(factor);
			CAPTURE(measured);
			CAPTURE(heard.Correlation);
			CAPTURE(unshifted.Correlation);
			// Both captures repeat clearly in the window: the engine's cycle, not noise.
			CHECK(heard.Correlation > 0.75);
			CHECK(unshifted.Correlation > 0.75);
			// Without doppler the cycle is the engine speed's. The pitch follows the engine speed through a 20 ms ramp, so it
			// trails it by about a frame, where the speed rises by up to 0.4 % (3,000 rpm a second along the straight).
			CHECK(std::abs(unshifted.Samples / cycle - 1.0) < 0.02);
			// With doppler it is the factor the ghost's speed predicts, within 1 %: the doppler moves the pitch by 9 to 13 %
			// here. What the tolerance holds: the period is read to a fraction of a sample of about 400 in each capture
			// (with the parabola, under 0.1 % each), the factor changes by under 0.2 % within a window, and the pitch's
			// ramp moves both captures alike. Nothing here depends on the platform's reverb (both captures are without the
			// zone) or on timing (the captures advance by the given step).
			CHECK(std::abs(measured / factor - 1.0) < 0.01);
			if (window.Approaching)
			{
				CHECK(factor > 1.05);
				CHECK(measured > 1.05);
				approaching++;
			}
			else
			{
				CHECK(factor < 0.95);
				CHECK(measured < 0.95);
				receding++;
			}
		}
		// The windows a shift took out leave enough on each side.
		CHECK(approaching >= 10);
		CHECK(receding >= 3);
	}

	// The echo: the capture with the zone carries energy after the ghost has left the bridge that the other lacks.
	{
		// The zone as the scene places it (the box's axes from its rotation), and the frames in which the ghost's engine is
		// within its reach (inside it, or within its fade).
		Json const& transform = echo->at("Components").at("Transform");
		Json const& rotation = transform.at("Rotation");
		Json const& zoneComponent = echo->at("Components").at("ReverbZone");
		REQUIRE(zoneComponent.value("Shape", std::string()) == "Box");
		AudioReverbZoneDesc zone;
		zone.Shape = ReverbZoneShape::Box;
		zone.Position = glm::vec3(ReadVector(transform.at("Translation")));
		zone.Rotation = glm::mat3_cast(glm::quat::wxyz(rotation.at(3).get<float>(), rotation.at(0).get<float>(),
		                                               rotation.at(1).get<float>(), rotation.at(2).get<float>()));
		zone.HalfExtents = glm::vec3(ReadVector(zoneComponent.at("HalfExtents")));
		zone.FadeDistance = zoneComponent.value("FadeDistance", 0.0f);
		zone.Level = echoLevel;
		std::optional<uint32_t> entered;
		std::optional<uint32_t> left;
		for (uint32_t frame = 0; frame < CaptureFrames; frame++)
		{
			if (ComputeReverbZoneSend(zone, glm::vec3(source(frame))) > 0.0f)
			{
				entered = entered.value_or(frame);
				left = frame + 1;
			}
		}
		REQUIRE(entered.has_value());
		REQUIRE(left.has_value());
		double const enteredSeconds = *entered * FrameSeconds;
		double const leftSeconds = *left * FrameSeconds;
		CAPTURE(enteredSeconds);
		CAPTURE(leftSeconds);
		REQUIRE(leftSeconds + 1.4 <= CaptureFrames * FrameSeconds);

		// Before the ghost comes within the zone's reach, nothing sends to the reverb: the captures are the same samples (up
		// to a tenth of a second before, which leaves room for a frame's difference in when the ghost is where).
		size_t const before = static_cast<size_t>((enteredSeconds - 0.1) * SampleRate);
		double largest = 0.0;
		for (size_t index = 0; index < before; index++)
		{
			largest = std::max(largest, std::abs(echoed[index] - dry[index]));
		}
		CHECK(largest <= LeastSignificantBit);

		// For 0.4 s after it has left, the zone's capture holds the reverb's tail, which the other lacks: their difference
		// carries at least a tenth of the dry mix's level (about a third here). And the tail dies away: a second later
		// it has fallen to under a tenth of that. The reverb is not the same filter on every platform (its delay lines are
		// sized in long double, Docs/Architecture.md section 9.6), so the bounds are an order of magnitude from what is
		// measured, not its samples.
		double const tail = Rms(echoed, &dry, leftSeconds, leftSeconds + 0.4);
		double const level = Rms(dry, nullptr, leftSeconds, leftSeconds + 0.4);
		double const later = Rms(echoed, &dry, leftSeconds + 1.0, leftSeconds + 1.4);
		CAPTURE(tail);
		CAPTURE(level);
		CAPTURE(later);
		CHECK(tail > 0.1 * level);
		CHECK(later < 0.1 * tail);
	}
}
