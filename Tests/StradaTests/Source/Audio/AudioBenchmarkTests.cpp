#include "Audio/AudioTestUtilities.h"
#include "Audio/MixAllocationHook.h"
#include "Core/ProfilerTestUtilities.h"

#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Audio/AudioTypes.h"
#include "Strada/Audio/AudioWaveform.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"
#include "Strada/Core/Profiler.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

// The mixer's budget (Docs/Architecture.md §9.9): 32 voices on 6 buses with a filter each, one reverb and one compressor in at
// most 1 ms of mixing per 10 ms of audio, the same with a snapshot blend, a ducked bus, a reverb zone and doppler in play,
// and the same at the engine's limit of 64 voices with one voice stolen and fading every frame; and what decoding a loaded clip costs per
// second of audio, for each format (§9.4). Measurements, never asserted on timings: run them with STRADA_AUDIO_BENCHMARK=1 in a Release
// build. They mix through manual output at 48 kHz in stereo, on the calling thread, which runs the same code a device's thread runs.
namespace
{
	constexpr uint32_t ReadFrames = 480;
	constexpr uint32_t FramesPerUpdate = 800;
	constexpr float AudioSeconds = 20.0f;
	constexpr int Rounds = 3;

	// A whole number of cycles per second loops without a seam; mono, 16-bit, at the given rate.
	Ref<AudioClipAsset> MakeSineClip(float frequency, float amplitude, uint32_t sampleRate)
	{
		std::vector<float> samples(sampleRate);
		for (size_t index = 0; index < samples.size(); index++)
		{
			samples[index] =
				amplitude * std::sin(2.0f * glm::pi<float>() * frequency * static_cast<float>(index) / static_cast<float>(sampleRate));
		}
		return Testing::MakeClip(Testing::MakeWav(samples, sampleRate));
	}

	MixerEffect MakeEffect(MixerEffectKind kind, float frequency = 1000.0f)
	{
		MixerEffect effect;
		effect.Kind = kind;
		effect.Frequency = frequency;
		effect.GainDb =
			kind == MixerEffectKind::Peaking || kind == MixerEffectKind::LowShelf || kind == MixerEffectKind::HighShelf ? 3.0f : 0.0f;
		return effect;
	}

	MixerBus MakeBus(std::string name, std::string parent, std::vector<MixerEffect> effects)
	{
		MixerBus bus;
		bus.Name = std::move(name);
		bus.Parent = std::move(parent);
		bus.Effects = std::move(effects);
		return bus;
	}

	// Six buses with a filter each; Master compresses after its filter, Reverb runs a reverb (its dry signal off) after its
	// filter.
	Mixer MakeBenchmarkMixer()
	{
		MixerEffect reverb = MakeEffect(MixerEffectKind::Reverb);
		reverb.DryDb = MinMixerVolumeDb;
		reverb.WetDb = 0.0f;
		Mixer mixer;
		mixer.Buses = {MakeBus("Master", "", {MakeEffect(MixerEffectKind::LowShelf, 120.0f), MakeEffect(MixerEffectKind::Compressor)}),
		               MakeBus("Music", "Master", {MakeEffect(MixerEffectKind::LowPass, 8000.0f)}),
		               MakeBus("Effects", "Master", {MakeEffect(MixerEffectKind::HighPass, 80.0f)}),
		               MakeBus("Engine", "Master", {MakeEffect(MixerEffectKind::Peaking, 2500.0f)}),
		               MakeBus("Ambience", "Master", {MakeEffect(MixerEffectKind::HighShelf, 6000.0f)}),
		               MakeBus("Reverb", "Master", {MakeEffect(MixerEffectKind::LowPass, 5000.0f), reverb})};
		return mixer;
	}

	// The benchmark's mixer with MX2's features in play (Docs/Architecture.md §9.8): Music ducked by Effects below a
	// threshold the effects pass, and a snapshot that sets a bus volume and two effect parameters, which the run keeps
	// blending to and back from.
	Mixer MakeFeatureMixer()
	{
		Mixer mixer = MakeBenchmarkMixer();
		MixerBus& music = mixer.Buses[1];
		REQUIRE(music.Name == "Music");
		music.DuckedBy = "Effects";
		music.DuckAmountDb = -9.0f;
		music.DuckThresholdDb = -70.0f;
		MixerSnapshot blend;
		blend.Name = "Blend";
		blend.Values = {MixerSnapshotValue{.Bus = "Music",
		                                   .Target = MixerSnapshotTarget::Volume,
		                                   .Effect = 0,
		                                   .Parameter = MixerEffectParameter::Frequency,
		                                   .Value = -12.0f},
		                MixerSnapshotValue{.Bus = "Reverb",
		                                   .Target = MixerSnapshotTarget::Effect,
		                                   .Effect = 0,
		                                   .Parameter = MixerEffectParameter::Frequency,
		                                   .Value = 1500.0f},
		                MixerSnapshotValue{.Bus = "Master",
		                                   .Target = MixerSnapshotTarget::Effect,
		                                   .Effect = 1,
		                                   .Parameter = MixerEffectParameter::ThresholdDb,
		                                   .Value = -20.0f}};
		mixer.Snapshots.push_back(std::move(blend));
		return mixer;
	}

	// A sphere that sends the spatial voices round it to the Reverb bus, most of them inside it and some across its fade.
	AudioReverbZoneDesc MakeBenchmarkZone()
	{
		AudioReverbZoneDesc zone;
		zone.Shape = ReverbZoneShape::Sphere;
		zone.Position = glm::vec3(0.0f, 1.0f, -7.0f);
		zone.Radius = 5.0f;
		zone.FadeDistance = 3.0f;
		zone.Bus = "Reverb";
		zone.Level = 0.7f;
		return zone;
	}

	// One kind of voice of the benchmark: how many, on which bus, whether spatial and at which clip rate, and whether its pitch
	// moves every simulated frame (an engine's layers) or stays where it was set (0.8 to 1.25).
	struct VoiceGroup
	{
		uint32_t Count = 0;
		std::string_view Bus;
		bool Spatial = false;
		uint32_t SampleRate = Testing::AudioSampleRate;
		bool PitchMoves = false;
	};

	// 32 voices: 4 of music (not spatial), 10 effects at 44.1 kHz, 10 engine layers whose pitch moves, 4 of ambience and 4 on
	// the reverb's bus; each source plays its own sine.
	constexpr std::array<VoiceGroup, 5> BenchmarkVoices = {{{4, "Music", false, Testing::AudioSampleRate, false},
	                                                        {10, "Effects", true, 44100, false},
	                                                        {10, "Engine", true, Testing::AudioSampleRate, true},
	                                                        {4, "Ambience", true, Testing::AudioSampleRate, false},
	                                                        {4, "Reverb", true, Testing::AudioSampleRate, false}}};

	struct BenchmarkSource
	{
		UUID Entity;
		glm::vec3 Center = glm::vec3(0.0f);
		bool PitchMoves = false;
	};

	// The sources of `copies` times the 32 voices of BenchmarkVoices, each playing.
	std::vector<BenchmarkSource> AddSources(AudioScene& scene, uint32_t copies, uint32_t maxVoices)
	{
		std::vector<BenchmarkSource> sources;
		uint64_t next = 5000;
		for (uint32_t copy = 0; copy < copies; copy++)
		{
			for (VoiceGroup const& group : BenchmarkVoices)
			{
				for (uint32_t index = 0; index < group.Count; index++)
				{
					float const frequency = 110.0f + 37.0f * static_cast<float>(sources.size());
					BenchmarkSource source;
					source.Entity = UUID(next++);
					source.Center = glm::vec3(std::sin(static_cast<float>(sources.size())) * 8.0f, 1.0f, -4.0f - static_cast<float>(index));
					source.PitchMoves = group.PitchMoves;
					AudioSourceDesc desc;
					desc.Entity = source.Entity;
					desc.Clip = MakeSineClip(frequency, 0.05f, group.SampleRate);
					desc.Loop = true;
					desc.Spatial = group.Spatial;
					desc.Position = source.Center;
					desc.Bus = std::string(group.Bus);
					desc.MaxVoices = maxVoices;
					desc.Pitch = group.PitchMoves ? 1.0f : 0.8f + 0.45f * static_cast<float>(index) / static_cast<float>(group.Count);
					REQUIRE(scene.AddSource(desc).IsOk());
					scene.Play(source.Entity);
					sources.push_back(source);
				}
			}
		}
		return sources;
	}

	// What a frame of a game does to the sounds: moves every spatial source and turns the engine layers' pitch.
	void SimulateFrame(AudioScene& scene, std::span<BenchmarkSource const> sources, uint64_t frame)
	{
		float const time = static_cast<float>(frame) / 60.0f;
		for (size_t index = 0; index < sources.size(); index++)
		{
			BenchmarkSource const& source = sources[index];
			scene.SetPosition(source.Entity, source.Center + glm::vec3(std::sin(time + static_cast<float>(index)) * 3.0f, 0.0f, 0.0f));
			if (source.PitchMoves)
			{
				scene.SetPitch(source.Entity, 1.0f + 0.4f * std::sin(time * 0.7f + static_cast<float>(index)));
			}
		}
	}

	struct MixFigures
	{
		double Median = 0.0;
		double Percentile99 = 0.0;
		double ProfiledMedian = 0.0;
	};

	// Mixes AudioSeconds of audio in reads of ReadFrames, sending a frame's commands every FramesPerUpdate frames (with what
	// onFrame adds, and, when steal is set, a one-shot that steals the oldest of its source's pool), and times each read: milliseconds of
	// mixing per 10 ms of audio. The profiler's median of Audio.Mix over the last reads (its history) is the same figure as the profiler
	// measures it.
	MixFigures MixAndTime(AudioScene& scene, std::span<BenchmarkSource const> sources, uint64_t& frame, UUID const* steal,
	                      Ref<AudioClipAsset> const& oneShot, std::function<void(uint64_t)> const& onFrame = {})
	{
		uint32_t const reads = static_cast<uint32_t>(std::lround(AudioSeconds * static_cast<float>(Testing::AudioSampleRate))) / ReadFrames;
		std::vector<float> samples(ReadFrames * 2);
		std::vector<double> milliseconds;
		milliseconds.reserve(reads);
		uint64_t mixed = 0;
		uint64_t nextUpdate = 0;
		REQUIRE(Profiler::Start().IsOk());
		for (uint32_t read = 0; read < reads; read++)
		{
			Profiler::BeginFrame();
			while (mixed >= nextUpdate)
			{
				SimulateFrame(scene, sources, frame);
				if (onFrame)
				{
					onFrame(frame);
				}
				frame++;
				if (steal != nullptr)
				{
					scene.PlayOneShot(*steal, oneShot, 0.5f);
				}
				AudioEngine::Update();
				nextUpdate += FramesPerUpdate;
			}
			auto const start = std::chrono::steady_clock::now();
			uint64_t const written = AudioEngine::ReadFrames(samples);
			auto const end = std::chrono::steady_clock::now();
			Profiler::EndFrame();
			REQUIRE(written == ReadFrames);
			mixed += written;
			milliseconds.push_back(std::chrono::duration<double, std::milli>(end - start).count());
		}
		Profiler::Stop();
		ProfilerMedians const medians = ComputeProfilerMedians(Profiler::GetRecording());
		MixFigures figures;
		for (ProfilerZoneMedian const& zone : medians.Zones)
		{
			if (zone.Name == "Audio.Mix")
			{
				figures.ProfiledMedian = zone.MillisecondsPerFrame;
			}
		}
		std::sort(milliseconds.begin(), milliseconds.end());
		figures.Median = milliseconds[milliseconds.size() / 2];
		figures.Percentile99 = milliseconds[std::min(milliseconds.size() - 1, milliseconds.size() * 99 / 100)];
		return figures;
	}

	// The output is not silent: the voices reach it through the buses.
	void CheckAudible()
	{
		std::vector<float> samples(ReadFrames * 2);
		REQUIRE(AudioEngine::ReadFrames(samples) == ReadFrames);
		float largest = 0.0f;
		for (float const sample : samples)
		{
			largest = std::max(largest, std::abs(sample));
		}
		CHECK(largest > 0.01f);
	}

	// The mixing thread called no global allocation function since the count read before; a build that leaves those
	// functions to a sanitizer's runtime counts nothing, and says the check is skipped.
	void CheckNoGlobalAllocations(uint64_t before)
	{
		if (Testing::IsMixingThreadAllocationCounted())
		{
			CHECK(Testing::GetMixingThreadAllocationCount() == before);
		}
		else
		{
			MESSAGE("Skipping the count of the global allocation functions: a sanitizer's runtime defines them in this build");
		}
	}
}

TEST_CASE("AudioBenchmark: 32 voices on 6 filtered buses with a reverb and a compressor, and 64 with a voice stolen every frame" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_AUDIO_BENCHMARK")))
{
	Testing::AudioEngineScope engine;
	Testing::ProfilerScope const profiler;
	AudioEngine::SetMixer(MakeBenchmarkMixer());
	REQUIRE(AudioEngine::GetMixer().Buses.size() == 6);
	Result<Scope<AudioScene>> created = AudioScene::Create();
	REQUIRE(created.IsOk());
	Scope<AudioScene> scene = created.TakeValue();
	Ref<AudioClipAsset> const oneShot = MakeSineClip(880.0f, 0.05f, Testing::AudioSampleRate);
	REQUIRE(AudioEngine::PreloadClip(oneShot).IsOk());
	uint64_t frame = 0;

	SUBCASE("32 voices")
	{
		std::vector<BenchmarkSource> const sources = AddSources(*scene, 1, DefaultAudioSourceVoices);
		REQUIRE(sources.size() == 32);
		CHECK(AudioEngine::GetActiveVoiceCount() == 32);
		CheckAudible();
		uint64_t const allocations = Testing::GetMixingThreadAllocationCount();
		uint64_t const miniaudioAllocations = AudioEngine::GetMixAllocationCount();
		for (int round = 1; round <= Rounds; round++)
		{
			MixFigures const figures = MixAndTime(*scene, sources, frame, nullptr, oneShot);
			MESSAGE("Round " << round << ", 32 voices on 6 buses: median " << figures.Median << " ms, 99th percentile "
			                 << figures.Percentile99 << " ms of mixing per 10 ms of audio (target 1 ms); Audio.Mix median "
			                 << figures.ProfiledMedian << " ms");
		}
		CHECK(AudioEngine::GetActiveVoiceCount() == 32);
		CHECK(AudioEngine::GetMixedVoiceCount() == 32);
		CheckNoGlobalAllocations(allocations);
		CHECK(AudioEngine::GetMixAllocationCount() == miniaudioAllocations);
	}

	SUBCASE("64 voices, one stolen and fading every frame")
	{
		// The shape twice over, one source stopped, and the last source's pool of 2 holding its main voice and its latest
		// one-shot: each frame's one-shot steals the one before, which fades out beside the 64.
		std::vector<BenchmarkSource> sources = AddSources(*scene, 2, DefaultAudioSourceVoices);
		REQUIRE(sources.size() == 64);
		UUID const stealer = sources.back().Entity;
		scene->SetMaxVoices(stealer, 2);
		scene->Stop(sources[sources.size() - 2].Entity);
		REQUIRE(scene->PlayOneShot(stealer, oneShot, 0.5f));
		CHECK(AudioEngine::GetActiveVoiceCount() == 64);
		uint64_t const stolenBefore = AudioEngine::GetStolenVoiceCount();
		uint64_t const allocations = Testing::GetMixingThreadAllocationCount();
		uint64_t const miniaudioAllocations = AudioEngine::GetMixAllocationCount();
		for (int round = 1; round <= Rounds; round++)
		{
			MixFigures const figures = MixAndTime(*scene, sources, frame, &stealer, oneShot);
			MESSAGE("Round " << round << ", 64 voices with one stolen every frame: median " << figures.Median << " ms, 99th percentile "
			                 << figures.Percentile99 << " ms of mixing per 10 ms of audio; Audio.Mix median " << figures.ProfiledMedian
			                 << " ms");
		}
		CHECK(AudioEngine::GetStolenVoiceCount() - stolenBefore >= static_cast<uint64_t>(Rounds) * 1000);
		CHECK(AudioEngine::GetActiveVoiceCount() == 64);
		CheckNoGlobalAllocations(allocations);
		CHECK(AudioEngine::GetMixAllocationCount() == miniaudioAllocations);
	}
}

TEST_CASE("AudioBenchmark: the 32 voices with a snapshot blend, a ducked bus, a reverb zone and doppler in play" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_AUDIO_BENCHMARK")))
{
	Testing::AudioEngineScope engine;
	Testing::ProfilerScope const profiler;
	AudioEngine::SetMixer(MakeFeatureMixer());
	REQUIRE(AudioEngine::GetMixer().Snapshots.size() == 1);
	Result<Scope<AudioScene>> created = AudioScene::Create();
	REQUIRE(created.IsOk());
	Scope<AudioScene> scene = created.TakeValue();
	Ref<AudioClipAsset> const oneShot = MakeSineClip(880.0f, 0.05f, Testing::AudioSampleRate);
	std::vector<BenchmarkSource> const sources = AddSources(*scene, 1, DefaultAudioSourceVoices);
	REQUIRE(sources.size() == 32);
	std::array<AudioReverbZoneDesc, 1> const zones = {MakeBenchmarkZone()};
	uint64_t frame = 0;

	// What a frame of a game adds with MX2: every source's velocity (the derivative of SimulateFrame's motion) and the
	// listener's, doppler, the zone's sends; and every half second a new blend of a second, to the snapshot and back, so
	// that one is always under way.
	auto const onFrame = [&scene, &sources, &zones](uint64_t simulated)
	{
		float const time = static_cast<float>(simulated) / 60.0f;
		for (size_t index = 0; index < sources.size(); index++)
		{
			scene->SetVelocity(sources[index].Entity, glm::vec3(std::cos(time + static_cast<float>(index)) * 3.0f, 0.0f, 0.0f));
		}
		scene->SetListenerVelocity(glm::vec3(0.0f));
		scene->UpdateDoppler();
		scene->UpdateReverbZones(zones);
		if (simulated % 30 == 0)
		{
			REQUIRE(AudioEngine::TransitionTo((simulated / 30) % 2 == 0 ? "Blend" : "", 1.0f));
		}
	};
	SimulateFrame(*scene, sources, frame);
	onFrame(frame++);
	CheckAudible();
	uint64_t const allocations = Testing::GetMixingThreadAllocationCount();
	uint64_t const miniaudioAllocations = AudioEngine::GetMixAllocationCount();
	for (int round = 1; round <= Rounds; round++)
	{
		MixFigures const figures = MixAndTime(*scene, sources, frame, nullptr, oneShot, onFrame);
		MESSAGE("Round " << round << ", 32 voices with a blend, a ducked bus, a zone and doppler: median " << figures.Median
		                 << " ms, 99th percentile " << figures.Percentile99
		                 << " ms of mixing per 10 ms of audio (target 1 ms); Audio.Mix median " << figures.ProfiledMedian << " ms");
	}

	// Each feature was in play: the ducking bus above its threshold, voices sending to the zone's bus, voices pitched by
	// doppler, and a blend under way.
	CHECK(AudioEngine::GetBusRms("Effects") > std::pow(10.0f, -70.0f / 20.0f));
	size_t sending = 0;
	size_t shifted = 0;
	for (BenchmarkSource const& source : sources)
	{
		sending += scene->GetReverbSend(source.Entity, "Reverb") > 0.0f ? 1 : 0;
		shifted += std::abs(scene->GetDoppler(source.Entity) - 1.0f) > 1e-4f ? 1 : 0;
	}
	MESSAGE(sending << " of 32 voices send to the zone's bus; " << shifted << " are pitched by doppler; the mix blends to '"
	                << AudioEngine::GetCurrentSnapshot() << "'");
	CHECK(sending > 0);
	CHECK(shifted > 0);
	CHECK(AudioEngine::GetMixedVoiceCount() == 32);
	CheckNoGlobalAllocations(allocations);
	CHECK(AudioEngine::GetMixAllocationCount() == miniaudioAllocations);
}

TEST_CASE("AudioBenchmark: decoding a loaded clip costs this much per second of audio, by format" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_AUDIO_BENCHMARK")))
{
	Testing::AudioEngineScope engine;
	constexpr int Repeats = 5;
	std::vector<float> samples(Testing::AudioFrames(20.0f));
	for (size_t index = 0; index < samples.size(); index++)
	{
		samples[index] =
			0.5f * std::sin(2.0f * glm::pi<float>() * 441.0f * static_cast<float>(index) / static_cast<float>(Testing::AudioSampleRate));
	}
	Result<Buffer> ogg = FileSystem::ReadBinaryFile(FileSystem::PathFromUtf8(STRADA_TEST_OGG_PATH));
	REQUIRE(ogg.IsOk());

	struct Format
	{
		std::string_view Name;
		Buffer Data;
	};
	std::vector<Format> formats;
	formats.push_back({"WAV, 16-bit mono, 48 kHz, a 20 s sine", Testing::MakeWav(samples)});
	// The tests' FLAC encoder writes at most 127 blocks: 10 s.
	std::span<float const> const flacSamples = std::span<float const>(samples).first(Testing::AudioFrames(10.0f));
	formats.push_back({"FLAC, mono, 48 kHz, a 10 s sine (the tests' encoder: verbatim subframes)", Testing::MakeFlac(flacSamples)});
	formats.push_back({"MP3, 128 kbit/s, a silent stream of 20 s (a lower bound)", Testing::MakeSilentMp3(834)});
	formats.push_back({"Ogg Vorbis, stereo, 48 kHz (miniaudio's test file)", ogg.TakeValue()});
	for (Format const& format : formats)
	{
		Ref<AudioClipAsset> const probe = Testing::MakeClip(Buffer::Copy(format.Data.GetSpan().data(), format.Data.GetSpan().size()));
		Result<AudioWaveform> const waveform = ComputeAudioWaveform(*probe, 16);
		REQUIRE_MESSAGE(waveform.IsOk(), format.Name);
		double const seconds = waveform.GetValue().Duration;
		REQUIRE(seconds > 1.0);
		std::vector<double> milliseconds;
		for (int repeat = 0; repeat < Repeats; repeat++)
		{
			// A new clip object decodes again (the cache keys on the object).
			Ref<AudioClipAsset> const clip = Testing::MakeClip(Buffer::Copy(format.Data.GetSpan().data(), format.Data.GetSpan().size()));
			auto const start = std::chrono::steady_clock::now();
			REQUIRE(AudioEngine::PreloadClip(clip).IsOk());
			auto const end = std::chrono::steady_clock::now();
			milliseconds.push_back(std::chrono::duration<double, std::milli>(end - start).count());
		}
		std::sort(milliseconds.begin(), milliseconds.end());
		MESSAGE(std::string(format.Name) << ": " << seconds << " s decoded in a median of " << milliseconds[Repeats / 2] << " ms, "
		                                 << milliseconds[Repeats / 2] / seconds << " ms per second of audio");
	}
	CHECK(AudioEngine::GetDecodedClipCount() == formats.size() * Repeats);
}
