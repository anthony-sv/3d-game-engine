#include "Audio/AudioTestUtilities.h"

#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEffects.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Core/Log.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;
using Strada::Testing::AudioFrames;
using Strada::Testing::ReadLevels;
using Strada::Testing::ReadSamples;
using Strada::Testing::Skip;

// What moves the mix while a game plays (Docs/Architecture.md §9.8): snapshots blended to over time, ducking, and the
// meters, measured from manual output against their closed forms. Every source here is a mono clip of a constant level
// (or a sine of whole cycles) on a bus, not spatial, so each stereo channel reads the clip's level times the gains, and a
// sample tells the gain at its frame.
namespace
{
	Scope<AudioScene> CreateAudioScene()
	{
		Result<Scope<AudioScene>> scene = AudioScene::Create();
		REQUIRE(scene.IsOk());
		return scene.TakeValue();
	}

	UUID SourceId(uint64_t index)
	{
		return UUID(3000 + index);
	}

	void AddSource(AudioScene& audio, uint64_t index, Ref<AudioClipAsset> clip, std::string bus)
	{
		AudioSourceDesc desc;
		desc.Entity = SourceId(index);
		desc.Clip = std::move(clip);
		desc.Spatial = false;
		desc.Loop = true;
		desc.Bus = std::move(bus);
		REQUIRE(audio.AddSource(desc).IsOk());
	}

	Ref<AudioClipAsset> MakeLevelClip(float level)
	{
		return Testing::MakeWavClip({{level, 1.0f}});
	}

	// A whole number of cycles per second loops without a seam.
	Ref<AudioClipAsset> MakeSineClip(float frequency, float amplitude)
	{
		std::vector<float> samples(AudioFrames(1.0f));
		for (size_t index = 0; index < samples.size(); index++)
		{
			samples[index] = amplitude * std::sin(2.0f * glm::pi<float>() * frequency * static_cast<float>(index) /
			                                      static_cast<float>(Testing::AudioSampleRate));
		}
		return Testing::MakeClip(Testing::MakeWav(samples));
	}

	// What a 16-bit clip of this level holds.
	float Quantize(float level)
	{
		return static_cast<float>(std::lround(level * 32768.0f)) / 32768.0f;
	}

	MixerBus MakeBus(std::string name, std::string parent, float volumeDb = 0.0f, std::vector<MixerEffect> effects = {})
	{
		MixerBus bus;
		bus.Name = std::move(name);
		bus.Parent = std::move(parent);
		bus.VolumeDb = volumeDb;
		bus.Effects = std::move(effects);
		return bus;
	}

	Mixer MakeMixer(std::vector<MixerBus> buses, std::vector<MixerSnapshot> snapshots = {})
	{
		Mixer mixer;
		mixer.Buses = std::move(buses);
		mixer.Snapshots = std::move(snapshots);
		return mixer;
	}

	MixerSnapshotValue SetVolume(std::string bus, float decibels)
	{
		return MixerSnapshotValue{.Bus = std::move(bus),
		                          .Target = MixerSnapshotTarget::Volume,
		                          .Effect = 0,
		                          .Parameter = MixerEffectParameter::Frequency,
		                          .Value = decibels};
	}

	MixerSnapshotValue SetMuted(std::string bus)
	{
		return MixerSnapshotValue{.Bus = std::move(bus),
		                          .Target = MixerSnapshotTarget::Muted,
		                          .Effect = 0,
		                          .Parameter = MixerEffectParameter::Frequency,
		                          .Value = 1.0f};
	}

	MixerSnapshotValue SetParameter(std::string bus, MixerEffectParameter parameter, float value)
	{
		return MixerSnapshotValue{
			.Bus = std::move(bus), .Target = MixerSnapshotTarget::Effect, .Effect = 0, .Parameter = parameter, .Value = value};
	}

	MixerEffect MakeFilter(MixerEffectKind kind, float frequency, float q, float gainDb = 0.0f)
	{
		MixerEffect effect;
		effect.Kind = kind;
		effect.Frequency = frequency;
		effect.Q = q;
		effect.GainDb = gainDb;
		return effect;
	}

	double ToDecibels(double ratio)
	{
		return 20.0 * std::log10(ratio);
	}

	// The left channel's sample at a frame.
	float Left(std::vector<float> const& samples, size_t frame)
	{
		return samples[frame * 2];
	}

	// Root mean square of the left channel over the frames [first, first + count).
	double MeasureRms(std::vector<float> const& samples, size_t first, size_t count)
	{
		double squares = 0.0;
		for (size_t frame = first; frame < first + count; frame++)
		{
			squares += static_cast<double>(Left(samples, frame)) * Left(samples, frame);
		}
		return std::sqrt(squares / static_cast<double>(count));
	}

	// Exactly blocks whole blocks in one read, so that the last block the meters measured is a whole one.
	void ReadBlocks(uint32_t blocks)
	{
		std::vector<float> samples(static_cast<size_t>(blocks) * AudioBlockFrames * 2);
		REQUIRE(AudioEngine::ReadFrames(samples) == static_cast<uint64_t>(blocks) * AudioBlockFrames);
	}

	size_t CountLogged(uint64_t first, std::string_view text)
	{
		std::vector<LogEntry> const entries = Log::GetEntries(first);
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
		                                         [text](LogEntry const& entry)
		                                         {
													 return entry.Message.find(text) != std::string::npos;
												 }));
	}
}

TEST_CASE("AudioMixer: a snapshot's transition moves a bus's volume linearly in decibels and its mute linearly in amplitude")
{
	Testing::AudioEngineScope engine;
	Mixer const mixer = MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Master")},
	                              {MixerSnapshot{.Name = "Quiet", .Values = {SetVolume("Music", -24.0f)}},
	                               MixerSnapshot{.Name = "Silent", .Values = {SetMuted("Music")}}});
	AudioEngine::SetMixer(mixer);
	Scope<AudioScene> audio = CreateAudioScene();
	AddSource(*audio, 0, MakeLevelClip(0.5f), "Music");
	audio->Play(SourceId(0));
	Skip(0.02f);
	float const level = Quantize(0.5f);
	CHECK(AudioEngine::GetCurrentSnapshot().empty());

	// Names match ignoring letter case; the current snapshot is named as the mixer names it, and the mixer's own values do
	// not change.
	REQUIRE(AudioEngine::TransitionTo("quiet", 1.0f));
	CHECK(AudioEngine::GetCurrentSnapshot() == "Quiet");
	CHECK(AudioEngine::GetMixer() == mixer);
	// Frame n of the transition is at -24 (n + 1) / 48000 dB.
	std::vector<float> const fading = ReadSamples(1.0f);
	for (size_t const frame : {12000u, 24000u, 36000u})
	{
		CAPTURE(frame);
		double const expected = -24.0 * static_cast<double>(frame + 1) / 48000.0;
		CHECK(std::abs(ToDecibels(Left(fading, frame) / level) - expected) < 0.02);
	}
	CHECK(std::abs(ToDecibels(ReadLevels(0.02f).Left / level) - -24.0) < 0.01);

	// Back to the mixer's own values over an edit's ramp; then a mute over half a second, linear in amplitude, silence
	// exactly at its end.
	REQUIRE(AudioEngine::TransitionTo("", 0.0f));
	CHECK(AudioEngine::GetCurrentSnapshot().empty());
	Skip(0.02f);
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(level).epsilon(1e-5));
	REQUIRE(AudioEngine::TransitionTo("Silent", 0.5f));
	std::vector<float> const muting = ReadSamples(0.5f);
	for (size_t const frame : {6000u, 12000u, 18000u})
	{
		CAPTURE(frame);
		double const expected = level * (1.0 - static_cast<double>(frame + 1) / 24000.0);
		CHECK(std::abs(Left(muting, frame) - expected) < 2.0e-4);
	}
	CHECK(ReadLevels(0.02f).Left == 0.0f);
}

TEST_CASE("AudioMixer: a snapshot's transition moves effect parameters on their scales")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	// 20 ms windows hold whole cycles of both tones (48 and 12 frames a cycle).
	size_t const window = AudioFrames(0.02f);

	SUBCASE("a peaking filter's gain, in decibels")
	{
		AudioEngine::SetMixer(
			MakeMixer({MakeBus("Master", ""), MakeBus("Tone", "Master", 0.0f, {MakeFilter(MixerEffectKind::Peaking, 1000.0f, 1.0f)})},
		              {MixerSnapshot{.Name = "Cut", .Values = {SetParameter("Tone", MixerEffectParameter::GainDb, -12.0f)}}}));
		AddSource(*audio, 0, MakeSineClip(1000.0f, 0.25f), "Tone");
		audio->Play(SourceId(0));
		Skip(0.1f);
		double const steady = MeasureRms(ReadSamples(0.02f), 0, window);
		REQUIRE(AudioEngine::TransitionTo("Cut", 1.0f));
		std::vector<float> const samples = ReadSamples(1.0f);
		// At its centre frequency a peaking filter's gain is its GainDb: -12 t dB at time t of the transition.
		for (float const time : {0.25f, 0.5f, 0.75f})
		{
			CAPTURE(time);
			size_t const centre = AudioFrames(time);
			double const measured = ToDecibels(MeasureRms(samples, centre - window / 2, window) / steady);
			CHECK(std::abs(measured - -12.0 * static_cast<double>(time)) < 0.2);
		}
	}
	SUBCASE("a low-pass filter's frequency, on a log2 scale")
	{
		AudioEngine::SetMixer(
			MakeMixer({MakeBus("Master", ""), MakeBus("Tone", "Master", 0.0f, {MakeFilter(MixerEffectKind::LowPass, 16000.0f, 0.7071f)})},
		              {MixerSnapshot{.Name = "Muffled", .Values = {SetParameter("Tone", MixerEffectParameter::Frequency, 1000.0f)}}}));
		AddSource(*audio, 0, MakeSineClip(4000.0f, 0.25f), "Tone");
		audio->Play(SourceId(0));
		Skip(0.1f);
		double const steady = MeasureRms(ReadSamples(0.02f), 0, window);
		REQUIRE(AudioEngine::TransitionTo("Muffled", 1.0f));
		std::vector<float> const samples = ReadSamples(1.0f);
		// Halfway from 16 kHz to 1 kHz on a log2 scale is 4 kHz, where the tone is: 3 dB down from the gain at 16 kHz. Halfway
		// in hertz (8.5 kHz) would leave it near 0 dB.
		double const sampleRate = static_cast<double>(Testing::AudioSampleRate);
		double const atStart =
			EvaluateBiquadGain(ComputeBiquadCoefficients(MixerEffectKind::LowPass, 16000.0, 0.7071, 0.0, sampleRate), 4000.0, sampleRate);
		double const halfway =
			EvaluateBiquadGain(ComputeBiquadCoefficients(MixerEffectKind::LowPass, 4000.0, 0.7071, 0.0, sampleRate), 4000.0, sampleRate);
		double const measured = ToDecibels(MeasureRms(samples, AudioFrames(0.5f) - window / 2, window) / steady);
		CHECK(std::abs(measured - ToDecibels(halfway / atStart)) < 0.3);
		CHECK(measured < -2.5);
	}
}

TEST_CASE("AudioMixer: a transition that interrupts another starts where the values are, and a snapshot the mixer lacks changes nothing")
{
	Testing::AudioEngineScope engine;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Master")},
	                                {MixerSnapshot{.Name = "Quiet", .Values = {SetVolume("Music", -24.0f)}},
	                                 MixerSnapshot{.Name = "Half", .Values = {SetVolume("Music", -6.0f)}}}));
	Scope<AudioScene> audio = CreateAudioScene();
	AddSource(*audio, 0, MakeLevelClip(0.5f), "Music");
	audio->Play(SourceId(0));
	Skip(0.02f);
	float const level = Quantize(0.5f);

	REQUIRE(AudioEngine::TransitionTo("Quiet", 1.0f));
	Skip(0.5f);
	// At -12 dB, halfway: the next transition starts there and moves to -6 dB over its own second.
	REQUIRE(AudioEngine::TransitionTo("Half", 1.0f));
	CHECK(AudioEngine::GetCurrentSnapshot() == "Half");
	std::vector<float> const samples = ReadSamples(1.0f);
	CHECK(std::abs(ToDecibels(Left(samples, 0) / level) - -12.0) < 0.02);
	CHECK(std::abs(ToDecibels(Left(samples, 24000) / level) - (-12.0 + 6.0 * 24001.0 / 48000.0)) < 0.02);
	CHECK(std::abs(ToDecibels(ReadLevels(0.02f).Left / level) - -6.0) < 0.01);

	uint64_t const logStart = Log::GetNextEntryIndex();
	CHECK_FALSE(AudioEngine::TransitionTo("Loud", 1.0f));
	CHECK_FALSE(AudioEngine::TransitionTo("LOUD", 0.0f));
	CHECK(CountLogged(logStart, "The mixer has no snapshot named 'Loud'") == 1);
	CHECK(AudioEngine::GetCurrentSnapshot() == "Half");
	CHECK(std::abs(ToDecibels(ReadLevels(0.02f).Left / level) - -6.0) < 0.01);
}

TEST_CASE(
	"AudioMixer: an edit of the mixer keeps the current snapshot's values, and one that removes the snapshot returns to the mixer's own")
{
	Testing::AudioEngineScope engine;
	Mixer mixer = MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Master"), MakeBus("Effects", "Master", -6.0f)},
	                        {MixerSnapshot{.Name = "Quiet", .Values = {SetVolume("Music", -20.0f)}}});
	AudioEngine::SetMixer(mixer);
	Scope<AudioScene> audio = CreateAudioScene();
	AddSource(*audio, 0, MakeLevelClip(0.5f), "Music");
	audio->Play(SourceId(0));
	Skip(0.02f);
	float const level = Quantize(0.5f);

	// An edit of another bus in the middle of a transition leaves the transition alone.
	REQUIRE(AudioEngine::TransitionTo("Quiet", 1.0f));
	Skip(0.25f);
	mixer.Buses[2].VolumeDb = -12.0f;
	AudioEngine::SetMixer(mixer);
	std::vector<float> const samples = ReadSamples(0.25f);
	CHECK(std::abs(ToDecibels(Left(samples, samples.size() / 2 - 1) / level) - -10.0) < 0.05);
	Skip(0.6f);
	CHECK(std::abs(ToDecibels(ReadLevels(0.02f).Left / level) - -20.0) < 0.01);

	// The bus's own volume changes under the snapshot, which still sets it.
	mixer.Buses[1].VolumeDb = -3.0f;
	AudioEngine::SetMixer(mixer);
	Skip(0.02f);
	CHECK(AudioEngine::GetMixer() == mixer);
	CHECK(std::abs(ToDecibels(ReadLevels(0.02f).Left / level) - -20.0) < 0.01);

	// The snapshot goes from the mixer: the mix returns to the mixer's own values, which is said once.
	uint64_t const logStart = Log::GetNextEntryIndex();
	mixer.Snapshots.clear();
	AudioEngine::SetMixer(mixer);
	CHECK(AudioEngine::GetCurrentSnapshot().empty());
	CHECK(CountLogged(logStart, "The mixer has no snapshot 'Quiet' any more") == 1);
	Skip(0.02f);
	CHECK(std::abs(ToDecibels(ReadLevels(0.02f).Left / level) - -3.0) < 0.01);

	// An edit of the bus's own volume in the middle of a transition of its mute leaves the mute's ramp alone: halfway
	// through the second, the mute's gain is 0.5 and the volume the edit's.
	mixer.Snapshots = {MixerSnapshot{.Name = "Silent", .Values = {SetMuted("Music")}}};
	AudioEngine::SetMixer(mixer);
	REQUIRE(AudioEngine::TransitionTo("Silent", 1.0f));
	Skip(0.25f);
	mixer.Buses[1].VolumeDb = -9.0f;
	AudioEngine::SetMixer(mixer);
	std::vector<float> const muting = ReadSamples(0.25f);
	double const expected = level * std::pow(10.0, -9.0 / 20.0) * 0.5;
	CHECK(std::abs(Left(muting, muting.size() / 2 - 1) - expected) < 2.0e-4);
}

TEST_CASE(
	"AudioMixer: a transition runs in mixed time while its scene is paused, takes its seconds within their limits, and ends with the game's values")
{
	Testing::AudioEngineScope engine;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Master")},
	                                {MixerSnapshot{.Name = "Quiet", .Values = {SetVolume("Music", -20.0f)}}}));
	Scope<AudioScene> audio = CreateAudioScene();
	AddSource(*audio, 0, MakeLevelClip(0.5f), "Music");
	audio->Play(SourceId(0));
	Skip(0.02f);
	float const level = Quantize(0.5f);

	// The pause snapshot's case: the scene's voices are held, the buses go on mixing, and the transition with them.
	audio->SetPaused(true);
	REQUIRE(AudioEngine::TransitionTo("Quiet", 0.2f));
	Skip(0.3f);
	CHECK(ReadLevels(0.02f).Left == 0.0f);
	audio->SetPaused(false);
	Skip(0.01f);
	CHECK(std::abs(ToDecibels(ReadLevels(0.02f).Left / level) - -20.0) < 0.01);

	// What a game ends with: no snapshot, the mixer's own values over an edit's ramp.
	AudioEngine::ResetGameValues();
	CHECK(AudioEngine::GetCurrentSnapshot().empty());
	Skip(0.01f);
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(level).epsilon(1e-5));

	// Seconds that are NaN or not positive are an edit's 5 ms ramp.
	REQUIRE(AudioEngine::TransitionTo("Quiet", std::numeric_limits<float>::quiet_NaN()));
	Skip(0.01f);
	CHECK(std::abs(ToDecibels(ReadLevels(0.02f).Left / level) - -20.0) < 0.01);
	REQUIRE(AudioEngine::TransitionTo("", -1.0f));
	Skip(0.01f);
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(level).epsilon(1e-5));
	// Longer than MaxSnapshotTransitionSeconds is that long: a second of it moves a three-hundredth of the way.
	REQUIRE(AudioEngine::TransitionTo("Quiet", 1.0e9f));
	Skip(1.0f);
	double const moved = ToDecibels(ReadLevels(0.001f).Left / level);
	CHECK(std::abs(moved - -20.0 * 1.0 / static_cast<double>(MaxSnapshotTransitionSeconds)) < 0.01);
}

TEST_CASE("AudioMixer: a ducked bus goes down by its amount over its attack while the other bus sounds, and back over its release")
{
	Testing::AudioEngineScope engine;
	MixerBus music = MakeBus("Music", "Master");
	music.DuckedBy = "voices";
	music.DuckAmountDb = -12.0f;
	music.DuckThresholdDb = -40.0f;
	music.DuckAttackSeconds = 0.2f;
	music.DuckReleaseSeconds = 0.4f;
	Mixer mixer = MakeMixer({MakeBus("Master", ""), music, MakeBus("Voices", "Master")});
	AudioEngine::SetMixer(mixer);
	Scope<AudioScene> audio = CreateAudioScene();
	AddSource(*audio, 0, MakeLevelClip(0.25f), "Music");
	AddSource(*audio, 1, MakeLevelClip(0.125f), "Voices");
	audio->Play(SourceId(0));
	Skip(0.05f);
	float const musicLevel = Quantize(0.25f);
	float const voiceLevel = Quantize(0.125f);
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(musicLevel).epsilon(1e-6));

	// The voices sound from the read's first block; ducking reads that block's level in the next, 256 frames on, and goes
	// down 12 dB per 0.2 s from there: -12 (n - 255) / 9600 dB at frame n.
	audio->Play(SourceId(1));
	std::vector<float> const attack = ReadSamples(0.5f);
	auto const duckDb = [&](std::vector<float> const& samples, size_t frame, float voice)
	{
		return ToDecibels((Left(samples, frame) - voice) / musicLevel);
	};
	for (size_t const frame : {2655u, 5055u, 7455u})
	{
		CAPTURE(frame);
		double const expected = -12.0 * static_cast<double>(frame - 255) / 9600.0;
		CHECK(std::abs(duckDb(attack, frame, voiceLevel) - expected) < 0.05);
	}
	CHECK(std::abs(duckDb(attack, AudioFrames(0.5f) - 1, voiceLevel) - -12.0) < 0.02);

	// The voices stop at a block's edge: back up 12 dB per 0.4 s from the next block on.
	audio->Stop(SourceId(1));
	std::vector<float> const release = ReadSamples(0.6f);
	for (size_t const frame : {5055u, 9855u, 14655u})
	{
		CAPTURE(frame);
		double const expected = -12.0 + 12.0 * static_cast<double>(frame - 255) / 19200.0;
		CHECK(std::abs(duckDb(release, frame, 0.0f) - expected) < 0.05);
	}
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(musicLevel).epsilon(1e-6));

	SUBCASE("a muted bus ducks nothing: its level is what it lets through")
	{
		mixer.Buses[2].Muted = true;
		AudioEngine::SetMixer(mixer);
		audio->Play(SourceId(1));
		Skip(0.3f);
		CHECK(ReadLevels(0.02f).Left == doctest::Approx(musicLevel).epsilon(1e-6));
	}
	// A duck that ends because the bus is ducked no more comes back over its release from the first frame after the edit,
	// never at once: halfway up after half the release.
	SUBCASE("a bus no longer ducked comes back over its release")
	{
		audio->Play(SourceId(1));
		Skip(0.3f);
		CHECK(ReadLevels(0.02f).Left < musicLevel * 0.3f + voiceLevel);
		mixer.Buses[1].DuckedBy.clear();
		AudioEngine::SetMixer(mixer);
		std::vector<float> const releasing = ReadSamples(0.4f);
		CHECK(std::abs(duckDb(releasing, 9600, voiceLevel) - -6.0) < 0.05);
		Skip(0.1f);
		CHECK(ReadLevels(0.02f).Left == doctest::Approx(musicLevel + voiceLevel).epsilon(1e-5));
	}
	SUBCASE("a bus ducked by one that goes is not ducked any more")
	{
		audio->Play(SourceId(1));
		Skip(0.3f);
		mixer.Buses[1].DuckedBy.clear();
		mixer.Buses.pop_back();
		AudioEngine::SetMixer(mixer);
		// The voices move to Master with a 10 ms dip, at their level again long before the half of the release.
		std::vector<float> const releasing = ReadSamples(0.4f);
		CHECK(std::abs(duckDb(releasing, 9600, voiceLevel) - -6.0) < 0.05);
		Skip(0.1f);
		CHECK(ReadLevels(0.02f).Left == doctest::Approx(musicLevel + voiceLevel).epsilon(1e-5));
	}
}

TEST_CASE("AudioMixer: each bus's meter reads the peak and RMS of its output over the last block")
{
	Testing::AudioEngineScope engine;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Master", -6.0f), MakeBus("Tone", "Master")}));
	Scope<AudioScene> audio = CreateAudioScene();
	AddSource(*audio, 0, MakeLevelClip(0.25f), "Music");
	// 128 frames a cycle: a block holds two whole cycles, and every cycle holds the crest.
	AddSource(*audio, 1, MakeSineClip(375.0f, 0.5f), "Tone");
	ReadBlocks(2);
	std::vector<AudioBusMeter> silent = AudioEngine::GetBusMeters();
	REQUIRE(silent.size() == 3);
	CHECK(silent[0].Name == "Master");
	CHECK(silent[1].Name == "Music");
	CHECK(silent[2].Name == "Tone");
	for (AudioBusMeter const& meter : silent)
	{
		CHECK(meter.Peak == 0.0f);
		CHECK(meter.Rms == 0.0f);
	}

	audio->Play(SourceId(0));
	ReadBlocks(4);
	float const music = Quantize(0.25f) * std::pow(10.0f, -6.0f / 20.0f);
	std::vector<AudioBusMeter> const constant = AudioEngine::GetBusMeters();
	CHECK(constant[1].Peak == doctest::Approx(music).epsilon(1e-5));
	CHECK(constant[1].Rms == doctest::Approx(music).epsilon(1e-5));
	CHECK(constant[0].Peak == doctest::Approx(music).epsilon(1e-5));
	CHECK(constant[2].Rms == 0.0f);
	CHECK(AudioEngine::GetBusPeak("music") == constant[1].Peak);
	CHECK(AudioEngine::GetBusRms("MUSIC") == constant[1].Rms);

	audio->Stop(SourceId(0));
	audio->Play(SourceId(1));
	ReadBlocks(4);
	float const crest = Quantize(0.5f);
	CHECK(AudioEngine::GetBusPeak("Tone") == doctest::Approx(crest).epsilon(1e-4));
	CHECK(AudioEngine::GetBusRms("Tone") == doctest::Approx(crest / std::sqrt(2.0f)).epsilon(1e-3));
	CHECK(AudioEngine::GetBusRms("Music") == 0.0f);
	CHECK(AudioEngine::GetBusRms("Master") == doctest::Approx(crest / std::sqrt(2.0f)).epsilon(1e-3));

	// A bus the mixer lacks reads silence and is logged once.
	uint64_t const logStart = Log::GetNextEntryIndex();
	CHECK(AudioEngine::GetBusPeak("Nowhere") == 0.0f);
	CHECK(AudioEngine::GetBusRms("NOWHERE") == 0.0f);
	CHECK(CountLogged(logStart, "The mixer has no bus named 'Nowhere': its meter reads silence") == 1);

	audio->Stop(SourceId(1));
	ReadBlocks(1);
	CHECK(AudioEngine::GetBusPeak("Master") == 0.0f);
}
