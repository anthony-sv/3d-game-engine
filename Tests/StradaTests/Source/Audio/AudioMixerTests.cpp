#include "Audio/AudioTestUtilities.h"

#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEffects.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Core/Log.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
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

// The buses and effects of the mixer (Docs/Architecture.md §9.5 and §9.6), measured from manual output against their
// closed forms. A non-spatial mono source reaches each stereo channel at its own level, so a bus's gain is the ratio of
// what is read to the clip's level.
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
		return UUID(2000 + index);
	}

	AudioSourceDesc MakeSource(UUID entity, Ref<AudioClipAsset> clip, std::string bus, bool loop = true)
	{
		AudioSourceDesc desc;
		desc.Entity = entity;
		desc.Clip = std::move(clip);
		desc.Spatial = false;
		desc.Loop = loop;
		desc.Bus = std::move(bus);
		return desc;
	}

	Ref<AudioClipAsset> MakeLevelClip(float level, float seconds)
	{
		return Testing::MakeWavClip({{level, seconds}});
	}

	// A whole number of cycles per second loops without a seam.
	Ref<AudioClipAsset> MakeSineClip(float frequency, float seconds, float amplitude)
	{
		std::vector<float> samples(AudioFrames(seconds));
		for (size_t index = 0; index < samples.size(); index++)
		{
			samples[index] = amplitude * std::sin(2.0f * glm::pi<float>() * frequency * static_cast<float>(index) /
			                                      static_cast<float>(Testing::AudioSampleRate));
		}
		return Testing::MakeClip(Testing::MakeWav(samples));
	}

	// One full-scale sample, then silence.
	Ref<AudioClipAsset> MakeImpulseClip(float seconds)
	{
		std::vector<float> samples(AudioFrames(seconds), 0.0f);
		samples[0] = 1.0f;
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

	Mixer MakeMixer(std::vector<MixerBus> buses)
	{
		Mixer mixer;
		mixer.Buses = std::move(buses);
		return mixer;
	}

	MixerEffect MakeFilter(MixerEffectKind kind, float frequency, float q = 0.7071f, float gainDb = 0.0f)
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

	// Root mean square of the left channel.
	double MeasureRms(std::span<float const> samples)
	{
		double squares = 0.0;
		size_t const frames = samples.size() / 2;
		for (size_t frame = 0; frame < frames; frame++)
		{
			squares += static_cast<double>(samples[frame * 2]) * samples[frame * 2];
		}
		return std::sqrt(squares / static_cast<double>(frames));
	}

	float LargestMagnitude(std::span<float const> samples)
	{
		float largest = 0.0f;
		for (float const sample : samples)
		{
			largest = std::max(largest, std::abs(sample));
		}
		return largest;
	}

	// The largest step from one sample of the left channel to the next.
	float LargestStep(std::span<float const> samples)
	{
		float largest = 0.0f;
		for (size_t frame = 1; frame < samples.size() / 2; frame++)
		{
			largest = std::max(largest, std::abs(samples[frame * 2] - samples[(frame - 1) * 2]));
		}
		return largest;
	}

	// The largest second difference of the left channel: how sharply its slope turns.
	float LargestBend(std::span<float const> samples)
	{
		float largest = 0.0f;
		for (size_t frame = 2; frame < samples.size() / 2; frame++)
		{
			largest = std::max(largest, std::abs(samples[frame * 2] - 2.0f * samples[(frame - 1) * 2] + samples[(frame - 2) * 2]));
		}
		return largest;
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

TEST_CASE("AudioMixer: a bus's gain is within 0.1 dB of its volume and its parents' through the chain")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.25f, 1.0f), "Sub")).IsOk());
	audio->Play(SourceId(0));

	// Without a mixer, Master alone: the identity, whatever bus a source names (a voice's first frame comes a frame late).
	CHECK(AudioEngine::GetMixer() == Mixer{});
	Skip(0.01f);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(Quantize(0.25f)).epsilon(1e-6));

	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", "", -3.0f), MakeBus("Music", "Master", -6.0f), MakeBus("Sub", "Music", -2.0f)}));
	CHECK(AudioEngine::GetMixer().Buses.size() == 3);
	Skip(0.05f);
	double const gainDb = ToDecibels(ReadLevels(0.1f).Left / Quantize(0.25f));
	CHECK(std::abs(gainDb - -11.0) < 0.1);

	// A bus that names none feeds Master; parents match names ignoring letter case.
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", "", -3.0f), MakeBus("Music", "", -6.0f), MakeBus("Sub", "MUSIC", 4.0f)}));
	Skip(0.05f);
	CHECK(std::abs(ToDecibels(ReadLevels(0.1f).Left / Quantize(0.25f)) - -5.0) < 0.1);
}

TEST_CASE("AudioMixer: a muted bus, or a muted parent, reads exactly 0 once its 5 ms ramp ends")
{
	Testing::AudioEngineScope engine;
	MixerBus music = MakeBus("Music", "Master");
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), music}));
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.5f, 1.0f), "Music")).IsOk());
	audio->Play(SourceId(0));
	Skip(0.01f);
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(Quantize(0.5f)).epsilon(1e-6));

	auto const silentAfterRamp = []
	{
		std::vector<float> const samples = ReadSamples(0.02f);
		size_t const rampSamples = static_cast<size_t>(AudioFrames(AudioGainSmoothingSeconds)) * 2;
		return std::all_of(samples.begin() + static_cast<ptrdiff_t>(rampSamples), samples.end(),
		                   [](float sample)
		                   {
							   return sample == 0.0f;
						   });
	};
	music.Muted = true;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), music}));
	CHECK(silentAfterRamp());

	// The bus back on, its parent muted.
	music.Muted = false;
	MixerBus master = MakeBus("Master", "");
	master.Muted = true;
	AudioEngine::SetMixer(MakeMixer({master, music}));
	CHECK(silentAfterRamp());
	// -80 dB is silence too.
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", "", MinMixerVolumeDb), music}));
	CHECK(silentAfterRamp());
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), music}));
	Skip(0.01f);
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(Quantize(0.5f)).epsilon(1e-6));
}

TEST_CASE("AudioMixer: a bus the mixer lacks plays on Master and is logged once per name and mixer")
{
	Testing::AudioEngineScope engine;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", "", -6.0f), MakeBus("Music", "Master")}));
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.25f, 1.0f), "Nowhere")).IsOk());
	REQUIRE(audio->AddSource(MakeSource(SourceId(1), MakeLevelClip(0.25f, 1.0f), "NOWHERE")).IsOk());
	audio->Play(SourceId(0));
	audio->Play(SourceId(1));
	// Master's new volume ramps over the first 5 ms.
	Skip(0.01f);
	CHECK(std::abs(ToDecibels(ReadLevels(0.05f).Left / (2.0f * Quantize(0.25f))) - -6.0) < 0.1);
	CHECK(CountLogged(logStart, "The mixer has no bus named 'Nowhere'") == 1);
	CHECK(CountLogged(logStart, "has no bus named") == 1);

	// Once the mixer gains the bus its sounds' group moves to it: out over 5 ms, moved, back in over 5 ms, never stepping
	// further than its ramps; the empty name is Master's and never missing.
	float const before = 2.0f * Quantize(0.25f) * std::pow(10.0f, -6.0f / 20.0f);
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", "", -6.0f), MakeBus("Nowhere", "Master", -6.0f)}));
	std::vector<float> const moving = ReadSamples(0.05f);
	CHECK(*std::min_element(moving.begin(), moving.end()) < 0.01f);
	CHECK(LargestStep(moving) <= 1.05f * before / static_cast<float>(AudioFrames(AudioGainSmoothingSeconds)));
	CHECK(moving.back() == doctest::Approx(before * std::pow(10.0f, -6.0f / 20.0f)).epsilon(1e-3));
	CHECK(std::abs(ToDecibels(ReadLevels(0.05f).Left / (2.0f * Quantize(0.25f))) - -12.0) < 0.1);
	audio->SetBus(SourceId(0), "");
	Skip(0.05f);
	CHECK(CountLogged(logStart, "has no bus named") == 1);
}

TEST_CASE("AudioMixer: an edit while the mix plays ramps values, dips changed buses without a click, and keeps a reverb's tail")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.5f, 1.0f), "Music")).IsOk());
	audio->Play(SourceId(0));
	MixerBus music = MakeBus("Music", "Master");
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), music}));
	Skip(0.02f);
	float const level = Quantize(0.5f);

	SUBCASE("a volume ramps over 5 ms")
	{
		music.VolumeDb = -12.0f;
		AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), music}));
		std::vector<float> const samples = ReadSamples(0.02f);
		float const target = level * std::pow(10.0f, -12.0f / 20.0f);
		CHECK(LargestStep(samples) <= 1.01f * (level - target) / static_cast<float>(AudioFrames(AudioGainSmoothingSeconds)));
		CHECK(samples.back() == doctest::Approx(target).epsilon(1e-4));
	}
	SUBCASE("an added effect dips the bus for 10 ms, without a click")
	{
		music.Effects = {MakeFilter(MixerEffectKind::HighShelf, 4000.0f, 0.7071f, 0.0f)};
		AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), music}));
		std::vector<float> const samples = ReadSamples(0.03f);
		CHECK(*std::min_element(samples.begin(), samples.end()) < 0.01f);
		CHECK(LargestStep(samples) < 2.0f * level / static_cast<float>(AudioFrames(AudioGainSmoothingSeconds)));
		CHECK(samples.back() == doctest::Approx(level).epsilon(1e-3));
	}
	SUBCASE("a reverb's tail rings on when a filter is inserted before it, and ends when the effects are cleared")
	{
		audio->RemoveSource(SourceId(0));
		MixerEffect reverb;
		reverb.Kind = MixerEffectKind::Reverb;
		reverb.WetDb = 0.0f;
		reverb.DryDb = MinMixerVolumeDb;
		reverb.RoomSize = 0.8f;
		MixerBus room = MakeBus("Room", "Master", 0.0f, {reverb});
		AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), room}));
		REQUIRE(audio->AddSource(MakeSource(SourceId(1), MakeLevelClip(0.5f, 0.05f), "Room", false)).IsOk());
		audio->Play(SourceId(1));
		Skip(0.15f);
		double const before = MeasureRms(ReadSamples(0.03f));
		REQUIRE(before > 1e-3);

		room.Effects = {MakeFilter(MixerEffectKind::LowPass, 20000.0f), reverb};
		AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), room}));
		Skip(0.02f);
		double const after = MeasureRms(ReadSamples(0.03f));
		CHECK(after > 0.3 * before);

		// Fresh effects of the same values: nothing feeds the reverb any more, so nothing rings.
		AudioEngine::ClearEffectState();
		Skip(0.02f);
		CHECK(LargestMagnitude(ReadSamples(0.03f)) == 0.0f);
	}
}

TEST_CASE("AudioMixer: scenes on different buses, paused one at a time, hold only their own sounds")
{
	Testing::AudioEngineScope engine;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Master"), MakeBus("Effects", "Master")}));
	Scope<AudioScene> first = CreateAudioScene();
	Scope<AudioScene> second = CreateAudioScene();
	REQUIRE(first->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.1f, 1.0f), "Music")).IsOk());
	REQUIRE(first->AddSource(MakeSource(SourceId(1), MakeLevelClip(0.05f, 1.0f), "Effects")).IsOk());
	REQUIRE(second->AddSource(MakeSource(SourceId(2), MakeLevelClip(0.2f, 1.0f), "Effects")).IsOk());
	first->Play(SourceId(0));
	first->Play(SourceId(1));
	second->Play(SourceId(2));
	Skip(0.01f);
	float const firstLevel = Quantize(0.1f) + Quantize(0.05f);
	float const secondLevel = Quantize(0.2f);
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(firstLevel + secondLevel).epsilon(1e-5));
	first->SetPaused(true);
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(secondLevel).epsilon(1e-5));
	first->SetPaused(false);
	second->SetPaused(true);
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(firstLevel).epsilon(1e-5));
	second->SetPaused(false);
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(firstLevel + secondLevel).epsilon(1e-5));
}

TEST_CASE("AudioMixer: the game's bus values add to the mixer's, are kept by name, and are reset for each game")
{
	Testing::AudioEngineScope engine;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Master", -3.0f)}));
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.5f, 1.0f), "Music")).IsOk());
	audio->Play(SourceId(0));
	float const level = Quantize(0.5f);
	CHECK(std::abs(ToDecibels(ReadLevels(0.02f).Left / level) - -3.0) < 0.1);

	AudioEngine::SetBusVolume("music", -6.0f);
	CHECK(AudioEngine::GetBusVolume("Music") == -6.0f);
	CHECK_FALSE(AudioEngine::IsBusMuted("Music"));
	Skip(0.01f);
	CHECK(std::abs(ToDecibels(ReadLevels(0.02f).Left / level) - -9.0) < 0.1);
	// The mixer's own value is the asset's: the game's stage does not change it.
	CHECK(AudioEngine::GetMixer().Buses[1].VolumeDb == -3.0f);

	AudioEngine::SetBusMuted("Music", true);
	CHECK(AudioEngine::IsBusMuted("MUSIC"));
	Skip(0.01f);
	CHECK(ReadLevels(0.02f).Left == 0.0f);

	// A bus removed and restored (an undo) gets its game values back.
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", "")}));
	Skip(0.02f);
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(level).epsilon(1e-5));
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Master", -3.0f)}));
	Skip(0.02f);
	CHECK(ReadLevels(0.02f).Left == 0.0f);
	CHECK(AudioEngine::IsBusMuted("Music"));

	AudioEngine::SetMasterVolume(0.5f);
	AudioEngine::ResetGameValues();
	CHECK(AudioEngine::GetMasterVolume() == 1.0f);
	CHECK(AudioEngine::GetBusVolume("Music") == 0.0f);
	CHECK_FALSE(AudioEngine::IsBusMuted("Music"));
	Skip(0.01f);
	CHECK(std::abs(ToDecibels(ReadLevels(0.02f).Left / level) - -3.0) < 0.1);

	// A name the mixer lacks reads 0 dB, sets nothing, and is logged once.
	uint64_t const logStart = Log::GetNextEntryIndex();
	AudioEngine::SetBusVolume("Voices", -20.0f);
	CHECK(AudioEngine::GetBusVolume("Voices") == 0.0f);
	CHECK_FALSE(AudioEngine::IsBusMuted("Voices"));
	CHECK(CountLogged(logStart, "no bus named 'Voices'") == 1);
}

TEST_CASE("AudioMixer: a bus removed while it plays sends its sounds to Master, a retired scene's fading ones included")
{
	Testing::AudioEngineScope engine;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Master", -6.0f), MakeBus("Low", "Music", -6.0f)}));
	Scope<AudioScene> live = CreateAudioScene();
	Scope<AudioScene> ending = CreateAudioScene();
	REQUIRE(live->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.25f, 1.0f), "Low")).IsOk());
	REQUIRE(ending->AddSource(MakeSource(SourceId(1), MakeLevelClip(0.25f, 1.0f), "Music")).IsOk());
	live->Play(SourceId(0));
	ending->Play(SourceId(1));
	Skip(0.02f);

	// The scene ends with its voice heard (it fades out), and before any read its bus goes: the mixing thread moves the
	// retired group to Master itself, since the main thread names it no more.
	ending.reset();
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Low", "Master", -6.0f)}));
	Skip(0.05f);
	AudioEngine::Update();
	CHECK(AudioEngine::GetReturningObjectCount() == 0);
	CHECK(std::abs(ToDecibels(ReadLevels(0.05f).Left / Quantize(0.25f)) - -6.0) < 0.1);

	// Restored, with Low below it again.
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Master", -6.0f), MakeBus("Low", "Music", -6.0f)}));
	Skip(0.05f);
	CHECK(std::abs(ToDecibels(ReadLevels(0.05f).Left / Quantize(0.25f)) - -12.0) < 0.1);
	live.reset();
	Skip(0.02f);
	AudioEngine::Update();
	CHECK(AudioEngine::GetReturningObjectCount() == 0);
}

TEST_CASE("AudioMixer: a mixer the validation refuses is not applied, and numbers out of range are brought within them")
{
	Testing::AudioEngineScope engine;
	Mixer const valid = MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Master", -3.0f)});
	AudioEngine::SetMixer(valid);
	uint64_t const logStart = Log::GetNextEntryIndex();
	AudioEngine::SetMixer(MakeMixer({MakeBus("Music", "")}));
	CHECK(AudioEngine::GetMixer() == valid);
	CHECK(CountLogged(logStart, "The mixer cannot be mixed through") == 1);

	MixerEffect wild = MakeFilter(MixerEffectKind::LowPass, 1.0e9f, 0.0f, 0.0f);
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", "", 1000.0f, {wild})}));
	REQUIRE(AudioEngine::GetMixer().Buses.size() == 1);
	CHECK(AudioEngine::GetMixer().Buses[0].VolumeDb == MaxMixerVolumeDb);
	CHECK(AudioEngine::GetMixer().Buses[0].Effects[0].Frequency == 24000.0f);
	CHECK(AudioEngine::GetMixer().Buses[0].Effects[0].Q == 0.1f);
}

TEST_CASE("AudioMixer: each filter's gain at three frequencies is within 0.1 dB of its closed form")
{
	struct FilterCase
	{
		MixerEffect Effect;
		std::array<float, 3> Frequencies;
	};
	std::vector<FilterCase> const cases = {
		{MakeFilter(MixerEffectKind::LowPass, 1000.0f), {250.0f, 1000.0f, 4000.0f}},
		{MakeFilter(MixerEffectKind::HighPass, 1000.0f, 2.0f), {250.0f, 1000.0f, 4000.0f}},
		{MakeFilter(MixerEffectKind::Peaking, 1000.0f, 2.0f, 9.0f), {125.0f, 1000.0f, 8000.0f}},
		{MakeFilter(MixerEffectKind::LowShelf, 500.0f, 0.7071f, -9.0f), {125.0f, 500.0f, 2000.0f}},
		{MakeFilter(MixerEffectKind::HighShelf, 2000.0f, 0.7071f, 6.0f), {500.0f, 2000.0f, 8000.0f}},
	};
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	constexpr float Amplitude = 0.25f;
	uint64_t source = 0;
	for (FilterCase const& filter : cases)
	{
		AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Filtered", "Master", 0.0f, {filter.Effect})}));
		BiquadCoefficients const coefficients = ComputeBiquadCoefficients(filter.Effect.Kind, filter.Effect.Frequency, filter.Effect.Q,
		                                                                  filter.Effect.GainDb, Testing::AudioSampleRate);
		for (float const frequency : filter.Frequencies)
		{
			CAPTURE(static_cast<int>(filter.Effect.Kind));
			CAPTURE(frequency);
			UUID const entity = SourceId(source++);
			REQUIRE(audio->AddSource(MakeSource(entity, MakeSineClip(frequency, 1.0f, Amplitude), "Filtered")).IsOk());
			audio->Play(entity);
			// The filter settles, then half a second of whole cycles is measured.
			Skip(0.05f);
			double const measured = ToDecibels(MeasureRms(ReadSamples(0.5f)) / (Amplitude / std::sqrt(2.0)));
			double const expected = ToDecibels(EvaluateBiquadGain(coefficients, frequency, Testing::AudioSampleRate));
			CHECK(std::abs(measured - expected) < (expected < -40.0 ? 0.5 : 0.1));
			audio->RemoveSource(entity);
			Skip(0.01f);
		}
	}
}

TEST_CASE("AudioMixer: the cookbook's coefficients, the closed form of a filter's gain and the compressor's static curve")
{
	// A 1 kHz low-pass at 48 kHz with Q 0.7071, computed by hand from the Audio EQ Cookbook's formulas.
	BiquadCoefficients const lowPass = ComputeBiquadCoefficients(MixerEffectKind::LowPass, 1000.0, 0.7071, 0.0, 48000.0);
	CHECK(lowPass.B0 == doctest::Approx(0.003916123487156441).epsilon(1e-12));
	CHECK(lowPass.B1 == doctest::Approx(0.007832246974312881).epsilon(1e-12));
	CHECK(lowPass.B2 == doctest::Approx(0.003916123487156441).epsilon(1e-12));
	CHECK(lowPass.A1 == doctest::Approx(-1.8153396116625289).epsilon(1e-12));
	CHECK(lowPass.A2 == doctest::Approx(0.8310041056111547).epsilon(1e-12));
	// Unity at DC, -3 dB at the cutoff of the flattest Q.
	CHECK(EvaluateBiquadGain(lowPass, 0.0, 48000.0) == doctest::Approx(1.0).epsilon(1e-9));
	CHECK(ToDecibels(EvaluateBiquadGain(lowPass, 1000.0, 48000.0)) == doctest::Approx(-3.0103).epsilon(1e-3));
	// Other kinds are the identity; frequencies past 0.45 of the rate are lowered to it.
	BiquadCoefficients const identity = ComputeBiquadCoefficients(MixerEffectKind::Delay, 1000.0, 0.7071, 0.0, 48000.0);
	CHECK(EvaluateBiquadGain(identity, 5000.0, 48000.0) == 1.0);
	BiquadCoefficients const highest = ComputeBiquadCoefficients(MixerEffectKind::LowPass, 24000.0, 0.7071, 0.0, 48000.0);
	BiquadCoefficients const lowered = ComputeBiquadCoefficients(MixerEffectKind::LowPass, 0.45 * 48000.0, 0.7071, 0.0, 48000.0);
	CHECK(highest.B0 == lowered.B0);
	CHECK(highest.A1 == lowered.A1);

	// Below the knee unchanged, in it a quadratic, above it the ratio (Giannoulis, Massberg and Reiss, eq. 4).
	CHECK(ComputeCompressorOutputDb(-20.0, -12.0, 4.0, 6.0) == -20.0);
	CHECK(ComputeCompressorOutputDb(-12.0, -12.0, 4.0, 6.0) == doctest::Approx(-12.5625).epsilon(1e-12));
	CHECK(ComputeCompressorOutputDb(0.0, -12.0, 4.0, 6.0) == doctest::Approx(-9.0).epsilon(1e-12));
	CHECK(ComputeCompressorOutputDb(-6.0, -12.0, 2.0, 0.0) == doctest::Approx(-9.0).epsilon(1e-12));
}

TEST_CASE("AudioMixer: a delay's echoes come DelaySeconds apart, each Feedback times the one before")
{
	Testing::AudioEngineScope engine;
	MixerEffect delay;
	delay.Kind = MixerEffectKind::Delay;
	delay.DelaySeconds = 0.1f;
	delay.Feedback = 0.5f;
	delay.WetDb = 0.0f;
	delay.DryDb = MinMixerVolumeDb;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Echo", "Master", 0.0f, {delay})}));
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeImpulseClip(0.01f), "Echo", false)).IsOk());
	audio->Play(SourceId(0));
	std::vector<float> const samples = ReadSamples(0.45f);

	// The dry impulse is silent (DryDb -80); each echo is the largest sample near its time.
	uint32_t const spacing = AudioFrames(delay.DelaySeconds);
	std::vector<std::pair<size_t, float>> echoes;
	for (uint32_t echo = 1; echo <= 4; echo++)
	{
		size_t const begin = static_cast<size_t>(echo) * spacing - spacing / 2;
		size_t const end = std::min(begin + spacing, samples.size() / 2);
		std::pair<size_t, float> largest = {begin, 0.0f};
		for (size_t frame = begin; frame < end; frame++)
		{
			if (std::abs(samples[frame * 2]) > largest.second)
			{
				largest = {frame, std::abs(samples[frame * 2])};
			}
		}
		echoes.push_back(largest);
	}
	CHECK(LargestMagnitude(std::span<float const>(samples).first(static_cast<size_t>(spacing / 2) * 2)) == 0.0f);
	for (size_t index = 1; index < echoes.size(); index++)
	{
		CAPTURE(index);
		CHECK(std::abs(static_cast<int64_t>(echoes[index].first - echoes[index - 1].first) - static_cast<int64_t>(spacing)) <= 1);
		CHECK(echoes[index].second / echoes[index - 1].second == doctest::Approx(delay.Feedback).epsilon(0.01));
	}
	CHECK(echoes[0].second > 0.5f);
}

namespace
{
	// The tail's decay time from 20 to 50 dB below its loudest 10 ms, extrapolated to 60 dB, in frames.
	double MeasureDecayFrames(std::span<float const> samples)
	{
		size_t const window = AudioFrames(0.01f);
		std::vector<double> levels;
		for (size_t begin = 0; begin + window <= samples.size() / 2; begin += window)
		{
			double squares = 0.0;
			for (size_t frame = begin; frame < begin + window; frame++)
			{
				squares += static_cast<double>(samples[frame * 2]) * samples[frame * 2] +
				           static_cast<double>(samples[frame * 2 + 1]) * samples[frame * 2 + 1];
			}
			levels.push_back(10.0 * std::log10(std::max(squares / static_cast<double>(window), 1e-30)));
		}
		size_t const loudest = static_cast<size_t>(std::max_element(levels.begin(), levels.end()) - levels.begin());
		auto const firstBelow = [&levels, loudest](double drop)
		{
			for (size_t index = loudest; index < levels.size(); index++)
			{
				if (levels[index] <= levels[loudest] - drop)
				{
					return static_cast<double>(index);
				}
			}
			return static_cast<double>(levels.size());
		};
		return (firstBelow(50.0) - firstBelow(20.0)) * static_cast<double>(window) * 2.0;
	}

	// Freeverb's closed form: its longest comb (1640 frames at 44.1 kHz, scaled to the rate; verblib.h:170) feeds back
	// RoomSize * 0.28 + 0.7 of itself per pass without damping (verblib.h:139-140), and sets the tail's late decay.
	double FreeverbDecayFrames(float roomSize, uint32_t sampleRate)
	{
		double const feedback = roomSize * 0.28 + 0.7;
		double const longestComb = std::floor(1640.0 * sampleRate / 44100.0);
		return 60.0 / (-20.0 * std::log10(feedback)) * longestComb;
	}
}

TEST_CASE("AudioMixer: a reverb's tail falls 60 dB in its closed form's time within 25 %, longer in a larger room")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	std::vector<double> measured;
	uint64_t source = 0;
	for (float const roomSize : {0.2f, 0.5f})
	{
		CAPTURE(roomSize);
		MixerEffect reverb;
		reverb.Kind = MixerEffectKind::Reverb;
		reverb.RoomSize = roomSize;
		reverb.Damping = 0.0f;
		reverb.WetDb = 0.0f;
		reverb.DryDb = MinMixerVolumeDb;
		AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Room", "Master", 0.0f, {reverb})}));
		AudioEngine::ClearEffectState();
		Skip(0.05f);
		UUID const entity = SourceId(source++);
		REQUIRE(audio->AddSource(MakeSource(entity, MakeLevelClip(0.5f, 0.02f), "Room", false)).IsOk());
		audio->Play(entity);
		double const frames = MeasureDecayFrames(ReadSamples(3.0f));
		double const expected = FreeverbDecayFrames(roomSize, Testing::AudioSampleRate);
		CAPTURE(frames);
		CAPTURE(expected);
		CHECK(frames == doctest::Approx(expected).epsilon(0.25));
		measured.push_back(frames);
		audio->RemoveSource(entity);
	}
	CHECK(measured[1] > measured[0]);
}

TEST_CASE("AudioMixer: a compressor's steady output matches its static curve plus makeup within 0.25 dB")
{
	Testing::AudioEngineScope engine;
	MixerEffect compressor;
	compressor.Kind = MixerEffectKind::Compressor;
	compressor.ThresholdDb = -12.0f;
	compressor.Ratio = 4.0f;
	compressor.KneeDb = 6.0f;
	compressor.AttackSeconds = 0.005f;
	compressor.ReleaseSeconds = 0.1f;
	compressor.MakeupDb = 3.0f;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), MakeBus("Dynamics", "Master", 0.0f, {compressor})}));
	Scope<AudioScene> audio = CreateAudioScene();
	uint64_t source = 0;
	for (float const level : {0.02f, 0.1f, 0.25f, 0.5f, 0.8f})
	{
		CAPTURE(level);
		UUID const entity = SourceId(source++);
		REQUIRE(audio->AddSource(MakeSource(entity, MakeLevelClip(level, 1.0f), "Dynamics")).IsOk());
		audio->Play(entity);
		Skip(0.6f);
		double const inputDb = ToDecibels(Quantize(level));
		double const expected =
			ComputeCompressorOutputDb(inputDb, compressor.ThresholdDb, compressor.Ratio, compressor.KneeDb) + compressor.MakeupDb;
		CHECK(std::abs(ToDecibels(ReadLevels(0.1f).Left) - expected) < 0.25);
		audio->RemoveSource(entity);
		Skip(0.6f);
	}
}

TEST_CASE("AudioMixer: a limiter at -1 dBFS holds 32 voices at full scale under its ceiling")
{
	Testing::AudioEngineScope engine;
	MixerEffect limiter;
	limiter.Kind = MixerEffectKind::Limiter;
	limiter.CeilingDb = -1.0f;
	limiter.ReleaseSeconds = 0.05f;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", "", 0.0f, {limiter})}));
	// Master's new chain is swapped in at the bottom of its dip, before any voice plays.
	Skip(0.02f);
	Scope<AudioScene> audio = CreateAudioScene();
	Ref<AudioClipAsset> const loud = MakeSineClip(440.0f, 1.0f, 1.0f);
	for (uint64_t index = 0; index < 32; index++)
	{
		REQUIRE(audio->AddSource(MakeSource(SourceId(index), loud, "")).IsOk());
		audio->Play(SourceId(index));
	}
	std::vector<float> const samples = ReadSamples(0.5f);
	float const ceiling = std::pow(10.0f, -1.0f / 20.0f);
	size_t const lookahead = static_cast<size_t>(AudioFrames(AudioLimiterLookaheadSeconds)) * 2;
	float const largest = LargestMagnitude(std::span<float const>(samples).subspan(lookahead));
	CHECK(largest <= 0.8913f);
	CHECK(largest > 0.95f * ceiling);
	// Released, a single quiet voice passes at its own level.
	for (uint64_t index = 1; index < 32; index++)
	{
		audio->RemoveSource(SourceId(index));
	}
	audio->SetVolume(SourceId(0), 0.5f);
	Skip(0.5f);
	CHECK(LargestMagnitude(ReadSamples(0.05f)) == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("AudioMixer: moving an effect's parameters makes no discontinuity")
{
	Testing::AudioEngineScope engine;
	constexpr float Amplitude = 0.25f;
	MixerBus filtered = MakeBus("Filtered", "Master", 0.0f, {MakeFilter(MixerEffectKind::LowPass, 8000.0f)});
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), filtered}));
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeSineClip(300.0f, 1.0f, Amplitude), "Filtered")).IsOk());
	audio->Play(SourceId(0));
	Skip(0.05f);
	// A sine's largest step from sample to sample, and its largest second difference: a click is a step in the waveform or
	// in its slope, so both stay near the sine's.
	float const omega = 2.0f * glm::pi<float>() * 300.0f / static_cast<float>(Testing::AudioSampleRate);
	float const sineStep = Amplitude * omega;
	float const sineBend = Amplitude * omega * omega;
	std::vector<float> const steady = ReadSamples(0.02f);
	CHECK(LargestStep(steady) < 1.1f * sineStep);
	CHECK(LargestBend(steady) < 1.1f * sineBend);

	// Down past the sine's frequency over 20 ms: the flattest low-pass never boosts, so the steps stay a sine's.
	filtered.Effects[0].Frequency = 400.0f;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", ""), filtered}));
	std::vector<float> const moving = ReadSamples(0.05f);
	CHECK(LargestStep(moving) < 1.1f * sineStep);
	CHECK(LargestBend(moving) < 4.0f * sineBend);
	// And it got there: 300 Hz through a 400 Hz low-pass.
	BiquadCoefficients const settled = ComputeBiquadCoefficients(MixerEffectKind::LowPass, 400.0, 0.7071, 0.0, Testing::AudioSampleRate);
	double const expected = ToDecibels(EvaluateBiquadGain(settled, 300.0, Testing::AudioSampleRate));
	CHECK(std::abs(ToDecibels(MeasureRms(ReadSamples(0.1f)) / (Amplitude / std::sqrt(2.0))) - expected) < 0.1);
}

TEST_CASE("AudioMixer: a reverb passes what its library cannot take unchanged")
{
	MixerEffect reverb;
	reverb.Kind = MixerEffectKind::Reverb;
	reverb.WetDb = 0.0f;
	reverb.DryDb = 0.0f;
	// Past Freeverb's shortest comb and allpasses, so that its tail has begun.
	std::vector<float> input(4 * 4096);
	for (size_t index = 0; index < input.size(); index++)
	{
		input[index] = std::sin(static_cast<float>(index) * 0.1f) * 0.5f;
	}
	// verblib runs at 22,050 to 176,400 Hz (verblib.h:422-430).
	Ref<AudioEffect> const slow = AudioEffect::Create(reverb, 16000, 2);
	std::vector<float> samples = input;
	slow->Process(samples);
	CHECK(samples == input);

	// On four channels it runs on the first two: the others pass unchanged.
	Ref<AudioEffect> const quad = AudioEffect::Create(reverb, 48000, 4);
	samples = input;
	quad->Process(samples);
	bool othersUnchanged = true;
	bool firstChanged = false;
	for (size_t frame = 0; frame < samples.size() / 4; frame++)
	{
		othersUnchanged =
			othersUnchanged && samples[frame * 4 + 2] == input[frame * 4 + 2] && samples[frame * 4 + 3] == input[frame * 4 + 3];
		firstChanged = firstChanged || samples[frame * 4] != input[frame * 4];
	}
	CHECK(othersUnchanged);
	CHECK(firstChanged);
}

TEST_CASE("AudioMixer: a reverb at a rate its library refuses is logged once, however often its effects are made again")
{
	Testing::AudioEngineScope engine({}, 16000);
	uint64_t const logStart = Log::GetNextEntryIndex();
	MixerEffect reverb;
	reverb.Kind = MixerEffectKind::Reverb;
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", "", 0.0f, {reverb})}));
	CHECK(CountLogged(logStart, "Reverb passes its bus through unchanged") == 1);
	// Each game's end makes every effect again, and so does a filter put in front of the reverb.
	AudioEngine::ClearEffectState();
	AudioEngine::ClearEffectState();
	AudioEngine::SetMixer(MakeMixer({MakeBus("Master", "", 0.0f, {MakeFilter(MixerEffectKind::LowPass, 1000.0f), reverb})}));
	AudioEngine::ClearEffectState();
	CHECK(CountLogged(logStart, "Reverb passes its bus through unchanged") == 1);
}

namespace
{
	// Runs a 300 Hz sine of amplitude 0.25 through the effect for this long, in blocks as a bus would, from frame start on,
	// and returns the stereo output.
	std::vector<float> ProcessSine(AudioEffect& effect, uint64_t& start, float seconds)
	{
		uint32_t const frames = AudioFrames(seconds);
		std::vector<float> samples(static_cast<size_t>(frames) * 2);
		for (uint32_t frame = 0; frame < frames; frame++)
		{
			float const value = 0.25f * std::sin(2.0f * glm::pi<float>() * 300.0f * static_cast<float>(start + frame) /
			                                     static_cast<float>(Testing::AudioSampleRate));
			samples[frame * 2] = value;
			samples[frame * 2 + 1] = value;
		}
		start += frames;
		for (size_t offset = 0; offset < samples.size(); offset += static_cast<size_t>(AudioBlockFrames) * 2)
		{
			effect.Process(std::span<float>(samples).subspan(offset, std::min<size_t>(AudioBlockFrames * 2, samples.size() - offset)));
		}
		return samples;
	}
}

TEST_CASE("AudioMixer: every kind of effect takes a new parameter without a click")
{
	struct MoveCase
	{
		char const* Name;
		MixerEffect Effect;
		MixerEffectParameter Parameter;
		float Value;
	};
	MixerEffect delay;
	delay.Kind = MixerEffectKind::Delay;
	MixerEffect reverb;
	reverb.Kind = MixerEffectKind::Reverb;
	MixerEffect compressor;
	compressor.Kind = MixerEffectKind::Compressor;
	compressor.ThresholdDb = -6.0f;
	MixerEffect limiter;
	limiter.Kind = MixerEffectKind::Limiter;
	std::vector<MoveCase> const cases = {
		{"a delay's time (its read crossfades)", delay, MixerEffectParameter::DelaySeconds, 0.07f},
		{"a delay's feedback", delay, MixerEffectParameter::Feedback, 0.9f},
		{"a delay's wet level", delay, MixerEffectParameter::WetDb, 0.0f},
		{"a delay's dry level", delay, MixerEffectParameter::DryDb, -20.0f},
		{"a reverb's room (verblib's values set once a step)", reverb, MixerEffectParameter::RoomSize, 0.95f},
		{"a reverb's damping", reverb, MixerEffectParameter::Damping, 0.0f},
		{"a reverb's width", reverb, MixerEffectParameter::Width, 0.0f},
		{"a reverb's wet level", reverb, MixerEffectParameter::WetDb, 0.0f},
		{"a compressor's threshold", compressor, MixerEffectParameter::ThresholdDb, -30.0f},
		{"a compressor's ratio", compressor, MixerEffectParameter::Ratio, 20.0f},
		{"a compressor's makeup", compressor, MixerEffectParameter::MakeupDb, 12.0f},
		{"a limiter's ceiling", limiter, MixerEffectParameter::CeilingDb, -24.0f},
		{"a peaking filter's gain", MakeFilter(MixerEffectKind::Peaking, 300.0f, 1.0f, 0.0f), MixerEffectParameter::GainDb, 12.0f},
		{"a high-pass filter's Q", MakeFilter(MixerEffectKind::HighPass, 250.0f, 0.7071f), MixerEffectParameter::Q, 8.0f},
	};
	for (MoveCase const& move : cases)
	{
		std::string const name = move.Name;
		CAPTURE(name);
		Ref<AudioEffect> const effect = AudioEffect::Create(move.Effect, Testing::AudioSampleRate, 2);
		uint64_t frame = 0;
		// Long enough for echoes, tails and envelopes to settle; then the steady output's sharpest turn is the measure.
		ProcessSine(*effect, frame, 2.0f);
		std::vector<float> const before = ProcessSine(*effect, frame, 0.1f);
		effect->SetParameter(move.Parameter, move.Value);
		std::vector<float> const after = ProcessSine(*effect, frame, 0.1f);
		// A sine of amplitude A turns its slope by at most A (2 pi 300 / 48000)^2 from one sample to the next; a click turns
		// it at once by many times that. A value on its way to its target only scales and shifts the waveform, so the
		// sharpest turn after the change stays within twice what the output's own level makes of the steady turn (or of a
		// sine's, for outputs smoother than the effect's steady state).
		float const omega = 2.0f * glm::pi<float>() * 300.0f / static_cast<float>(Testing::AudioSampleRate);
		float const steadyLevel = LargestMagnitude(before);
		float const movingLevel = LargestMagnitude(after);
		REQUIRE(steadyLevel > 0.0f);
		float const steady = LargestBend(before) * movingLevel / steadyLevel;
		float const moving = LargestBend(after);
		CAPTURE(steady);
		CAPTURE(moving);
		CHECK(moving <= 2.0f * std::max(steady, movingLevel * omega * omega));
	}
}
