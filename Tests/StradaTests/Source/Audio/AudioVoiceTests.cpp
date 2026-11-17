#include "Audio/AudioTestUtilities.h"
#include "Audio/MixAllocationHook.h"
#include "TestUtilities.h"

#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Audio/AudioTypes.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace Strada;
using Strada::Testing::AudioFrames;
using Strada::Testing::ReadLevels;
using Strada::Testing::ReadSamples;
using Strada::Testing::Skip;

namespace
{
	Scope<AudioScene> CreateAudioScene()
	{
		Result<Scope<AudioScene>> scene = AudioScene::Create();
		REQUIRE(scene.IsOk());
		return scene.TakeValue();
	}

	// A non-spatial source of clip, looping when asked.
	AudioSourceDesc MakeSource(UUID entity, Ref<AudioClipAsset> clip, bool loop = false)
	{
		AudioSourceDesc desc;
		desc.Entity = entity;
		desc.Clip = std::move(clip);
		desc.Spatial = false;
		desc.Loop = loop;
		return desc;
	}

	// A clip of one level for this long.
	Ref<AudioClipAsset> MakeLevelClip(float level, float seconds)
	{
		return Testing::MakeWavClip({{level, seconds}});
	}

	// A sine wave of this frequency, as a mono clip at the test rate.
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

	// Sign changes of the left channel.
	uint32_t CountZeroCrossings(std::span<float const> samples)
	{
		uint32_t crossings = 0;
		for (size_t frame = 1; frame < samples.size() / 2; frame++)
		{
			crossings += (samples[(frame - 1) * 2] < 0.0f) != (samples[frame * 2] < 0.0f) ? 1 : 0;
		}
		return crossings;
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

	UUID SourceId(uint64_t index)
	{
		return UUID(1000 + index);
	}
}

TEST_CASE("AudioEngine: a change made before a read ramps from the read's first frame and reaches its value in the smoothing time")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.5f, 0.5f), true)).IsOk());
	audio->Play(SourceId(0));
	Skip(0.05f);

	uint32_t const rampFrames = AudioFrames(AudioGainSmoothingSeconds);
	REQUIRE(rampFrames == 240);
	// The master volume: from 0.5 down to 0.25 over the ramp, frame by frame, from the read's first frame on.
	AudioEngine::SetMasterVolume(0.5f);
	CHECK(AudioEngine::GetMasterVolume() == 0.5f);
	std::vector<float> samples = ReadSamples(0.02f);
	CHECK(samples[0] < 0.5f);
	CHECK(samples[0] > 0.49f);
	CHECK(samples[(rampFrames / 2) * 2] == doctest::Approx(0.375f).epsilon(0.01));
	CHECK(std::abs(samples[rampFrames * 2] - 0.25f) < 1e-4f);
	CHECK(std::abs(samples.back() - 0.25f) < 1e-4f);

	// A voice's volume: the same time, through miniaudio's own smoothing.
	audio->SetVolume(SourceId(0), 0.5f);
	samples = ReadSamples(0.02f);
	CHECK(samples[0] > 0.24f);
	CHECK(std::abs(samples[rampFrames * 2] - 0.125f) < 1e-4f);
	CHECK(std::abs(samples.back() - 0.125f) < 1e-4f);
}

TEST_CASE("AudioEngine: pitch moves to its target over the smoothing time on a log scale")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	// 400 Hz: 40 zero crossings in 50 ms.
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeSineClip(400.0f, 1.0f, 0.5f), true)).IsOk());
	audio->Play(SourceId(0));
	Skip(0.05f);
	CHECK(CountZeroCrossings(ReadSamples(0.05f)) == doctest::Approx(40.0).epsilon(0.05));

	audio->SetPitch(SourceId(0), 2.0f);
	CHECK(audio->GetPitch(SourceId(0)) == 2.0f);
	// During the 20 ms ramp the frequency lies between the two; once it is over, it is twice the clip's.
	uint32_t const during = CountZeroCrossings(ReadSamples(AudioParameterSmoothingSeconds));
	CHECK(during > 17);
	CHECK(during < 30);
	Skip(0.01f);
	CHECK(CountZeroCrossings(ReadSamples(0.05f)) == doctest::Approx(80.0).epsilon(0.05));
}

TEST_CASE("AudioEngine: a burst of more commands than the queue holds reaches the mix in order, the last one heard")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.5f, 0.5f), true)).IsOk());
	audio->Play(SourceId(0));
	Skip(0.01f);

	constexpr int Commands = 5000;
	for (int command = 1; command <= Commands; command++)
	{
		audio->SetVolume(SourceId(0), 1.0f - static_cast<float>(command) / (2.0f * Commands));
	}
	CHECK(AudioEngine::GetDeferredCommandCount() > 0);
	CHECK(audio->GetVolume(SourceId(0)) == 0.5f);
	Skip(0.05f);
	CHECK(AudioEngine::GetDeferredCommandCount() == 0);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.25f).epsilon(0.01));
}

TEST_CASE("AudioScene: a volume set twice before the first play plays at the second from its first frame")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.5f, 0.5f))).IsOk());
	audio->SetVolume(SourceId(0), 0.3f);
	audio->SetVolume(SourceId(0), 0.6f);
	audio->Play(SourceId(0));
	std::vector<float> const samples = ReadSamples(0.01f);
	// Past the resampler's first frame (it starts from silence), every frame is at the second value: none ramps from the
	// first.
	bool const atSecond = std::all_of(samples.begin() + 2 * 2, samples.end(),
	                                  [](float sample)
	                                  {
										  return std::abs(sample - 0.3f) < 1e-4f;
									  });
	CHECK(atSecond);
}

TEST_CASE("AudioScene: one-shots play beside the main voice up to the pool, which then steals its oldest one-shot")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	AudioSourceDesc desc = MakeSource(SourceId(0), MakeLevelClip(0.1f, 1.0f), true);
	desc.MaxVoices = 4;
	REQUIRE(audio->AddSource(desc).IsOk());
	CHECK(audio->GetMaxVoices(SourceId(0)) == 4);
	audio->Play(SourceId(0));
	std::vector<Ref<AudioClipAsset>> const clips = {MakeLevelClip(0.01f, 1.0f), MakeLevelClip(0.02f, 1.0f), MakeLevelClip(0.04f, 1.0f),
	                                                MakeLevelClip(0.08f, 1.0f)};
	uint64_t const stolen = AudioEngine::GetStolenVoiceCount();

	// The main voice and three one-shots: the pool is full.
	for (size_t clip = 0; clip < 3; clip++)
	{
		CHECK(audio->PlayOneShot(SourceId(0), clips[clip], 1.0f));
	}
	CHECK(audio->GetVoiceCount(SourceId(0)) == 4);
	CHECK(AudioEngine::GetActiveVoiceCount() == 4);
	Skip(0.02f);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.17f).epsilon(0.01));
	CHECK(AudioEngine::GetStolenVoiceCount() == stolen);

	// A fourth one-shot steals the oldest (0.01), which fades out.
	CHECK(audio->PlayOneShot(SourceId(0), clips[3], 1.0f));
	CHECK(AudioEngine::GetStolenVoiceCount() == stolen + 1);
	CHECK(audio->GetVoiceCount(SourceId(0)) == 4);
	Skip(0.02f);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.24f).epsilon(0.01));

	// One-shots follow their source: half the volume halves them all; a one-shot's own volume scales it.
	audio->SetVolume(SourceId(0), 0.5f);
	Skip(0.02f);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.12f).epsilon(0.01));
	CHECK(audio->PlayOneShot(SourceId(0), nullptr, 0.5f));
	Skip(0.02f);
	// The source's own clip at half of half the volume took the place of the 0.02 one-shot.
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.05f + 0.02f + 0.04f + 0.025f).epsilon(0.01));

	// Stop stops the main voice and fades the one-shots out; nothing is heard after.
	audio->Stop(SourceId(0));
	CHECK(audio->GetVoiceCount(SourceId(0)) == 0);
	Skip(0.02f);
	CHECK(ReadLevels(0.05f).Left == 0.0f);
	CHECK(AudioEngine::GetReturningObjectCount() == 0);
}

TEST_CASE("AudioScene: a pool of one refuses one-shots while its main voice plays, and one-shots never loop")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	AudioSourceDesc single = MakeSource(SourceId(0), MakeLevelClip(0.1f, 1.0f), true);
	single.MaxVoices = 1;
	REQUIRE(audio->AddSource(single).IsOk());
	audio->Play(SourceId(0));
	uint64_t const stolen = AudioEngine::GetStolenVoiceCount();
	CHECK_FALSE(audio->PlayOneShot(SourceId(0), nullptr, 1.0f));
	CHECK(AudioEngine::GetStolenVoiceCount() == stolen + 1);
	audio->Stop(SourceId(0));
	CHECK(audio->PlayOneShot(SourceId(0), nullptr, 1.0f));

	// The source loops; its one-shot plays its 0.1 s clip once.
	AudioSourceDesc looping = MakeSource(SourceId(1), MakeLevelClip(0.2f, 0.1f), true);
	REQUIRE(audio->AddSource(looping).IsOk());
	CHECK(audio->IsLooping(SourceId(1)));
	CHECK(audio->PlayOneShot(SourceId(1), nullptr, 1.0f));
	CHECK(audio->GetVoiceCount(SourceId(1)) == 1);
	Skip(0.2f);
	CHECK(audio->GetVoiceCount(SourceId(1)) == 0);
	CHECK(audio->IsLooping(SourceId(1)));

	// The pool's size can shrink under its one-shots: the oldest go.
	audio->SetMaxVoices(SourceId(1), 3);
	for (int shot = 0; shot < 3; shot++)
	{
		CHECK(audio->PlayOneShot(SourceId(1), nullptr, 1.0f));
	}
	audio->SetMaxVoices(SourceId(1), 1);
	CHECK(audio->GetVoiceCount(SourceId(1)) == 1);
	audio->SetMaxVoices(SourceId(1), 0);
	CHECK(audio->GetMaxVoices(SourceId(1)) == 1);
	audio->SetMaxVoices(SourceId(1), 1000);
	CHECK(audio->GetMaxVoices(SourceId(1)) == MaxAudioSourceVoices);
}

TEST_CASE("AudioScene: a one-shot that ended frees its place before the next starts, and comes back after the next read")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	AudioSourceDesc desc = MakeSource(SourceId(0), MakeLevelClip(0.1f, 0.05f));
	desc.MaxVoices = 2;
	REQUIRE(audio->AddSource(desc).IsOk());
	CHECK(audio->PlayOneShot(SourceId(0), nullptr, 1.0f));
	CHECK(audio->PlayOneShot(SourceId(0), nullptr, 1.0f));
	CHECK(AudioEngine::GetActiveVoiceCount() == 2);
	uint64_t const stolen = AudioEngine::GetStolenVoiceCount();

	// Both end in this read: the main thread learns it at the read's end, retires them, and they come back with the next
	// block.
	Skip(0.1f);
	CHECK(audio->GetVoiceCount(SourceId(0)) == 0);
	CHECK(AudioEngine::GetActiveVoiceCount() == 0);
	CHECK(AudioEngine::GetReturningObjectCount() == 2);
	CHECK(audio->PlayOneShot(SourceId(0), nullptr, 1.0f));
	CHECK(audio->PlayOneShot(SourceId(0), nullptr, 1.0f));
	CHECK(AudioEngine::GetStolenVoiceCount() == stolen);
	CHECK(AudioEngine::GetActiveVoiceCount() == 2);
	Skip(0.01f);
	CHECK(AudioEngine::GetReturningObjectCount() == 0);
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(0.2f).epsilon(0.01));
}

TEST_CASE("AudioScene: a one-shot stolen in the block its clip ends comes back once")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	AudioSourceDesc desc = MakeSource(SourceId(0), MakeLevelClip(0.1f, 1.0f));
	desc.MaxVoices = 2;
	REQUIRE(audio->AddSource(desc).IsOk());
	// A clip of 100 frames, shorter than a block, then a long one fill the pool; a third steals the short one before any
	// block. The next block starts both, steals the short one and renders past its end: its fade and its end meet.
	std::vector<float> const shortSamples(100, 0.2f);
	REQUIRE(Testing::AudioFrames(0.1f) > AudioBlockFrames);
	CHECK(audio->PlayOneShot(SourceId(0), Testing::MakeClip(Testing::MakeWav(shortSamples)), 1.0f));
	CHECK(audio->PlayOneShot(SourceId(0), nullptr, 1.0f));
	uint64_t const stolen = AudioEngine::GetStolenVoiceCount();
	CHECK(audio->PlayOneShot(SourceId(0), nullptr, 1.0f));
	CHECK(AudioEngine::GetStolenVoiceCount() == stolen + 1);
	std::vector<float> samples(static_cast<size_t>(AudioBlockFrames) * 2);
	REQUIRE(AudioEngine::ReadFrames(samples) == AudioBlockFrames);
	CHECK(AudioEngine::GetReturningObjectCount() == 0);
	Skip(0.05f);
	CHECK(AudioEngine::GetReturningObjectCount() == 0);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.2f).epsilon(0.01));
}

TEST_CASE("AudioScene: a volume that is not a number plays as silence")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	AudioSourceDesc desc = MakeSource(SourceId(0), MakeLevelClip(0.5f, 1.0f), true);
	desc.Volume = std::nanf("");
	REQUIRE(audio->AddSource(desc).IsOk());
	CHECK(audio->GetVolume(SourceId(0)) == 0.0f);
	audio->Play(SourceId(0));
	audio->SetVolume(SourceId(0), std::nanf(""));
	CHECK(audio->GetVolume(SourceId(0)) == 0.0f);
	// A one-shot at such a volume plays, silent.
	CHECK(audio->PlayOneShot(SourceId(0), nullptr, std::nanf("")));
	Skip(0.02f);
	CHECK(ReadLevels(0.05f).Left == 0.0f);
}

TEST_CASE("AudioScene: a scene that ends while its stolen one-shot fades out gives every object back, other scenes play on")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> other = CreateAudioScene();
	REQUIRE(other->AddSource(MakeSource(SourceId(9), MakeLevelClip(0.1f, 1.0f), true)).IsOk());
	other->Play(SourceId(9));

	Scope<AudioScene> audio = CreateAudioScene();
	AudioSourceDesc desc = MakeSource(SourceId(0), MakeLevelClip(0.2f, 1.0f));
	desc.MaxVoices = 2;
	REQUIRE(audio->AddSource(desc).IsOk());
	for (int shot = 0; shot < 3; shot++)
	{
		CHECK(audio->PlayOneShot(SourceId(0), nullptr, 1.0f));
	}
	// 2 ms into the steal's 10 ms fade the scene ends: its voices fade out, its groups come back after them.
	Skip(0.002f);
	audio.reset();
	CHECK(AudioEngine::GetReturningObjectCount() > 0);
	Skip(0.02f);
	CHECK(AudioEngine::GetReturningObjectCount() == 0);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.1f).epsilon(0.01));
	CHECK(other->IsPlaying(SourceId(9)));
}

TEST_CASE("AudioScene: a removed source and an ended scene fade out without a click")
{
	Testing::AudioEngineScope engine;
	// A step of the fade is the level over the fade's frames; anything far above it would be heard as a click.
	float const fadeStep = 0.5f / static_cast<float>(AudioFrames(AudioGainSmoothingSeconds));
	{
		Scope<AudioScene> audio = CreateAudioScene();
		REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.5f, 1.0f), true)).IsOk());
		audio->Play(SourceId(0));
		Skip(0.05f);
		audio->RemoveSource(SourceId(0));
		std::vector<float> const samples = ReadSamples(0.02f);
		CHECK(samples[0] > 0.45f);
		CHECK(LargestStep(samples) < 2.0f * fadeStep);
		CHECK(samples.back() == 0.0f);
	}
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.5f, 1.0f), true)).IsOk());
	audio->Play(SourceId(0));
	Skip(0.05f);
	audio.reset();
	std::vector<float> const samples = ReadSamples(0.02f);
	CHECK(samples[0] > 0.45f);
	CHECK(LargestStep(samples) < 2.0f * fadeStep);
	CHECK(samples.back() == 0.0f);
	CHECK(AudioEngine::GetReturningObjectCount() == 0);
}

TEST_CASE("AudioScene: a source moved to another bus dips without a click, its one-shots with it")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.5f, 1.0f), true)).IsOk());
	audio->Play(SourceId(0));
	CHECK(audio->PlayOneShot(SourceId(0), MakeLevelClip(0.1f, 1.0f), 1.0f));
	Skip(0.05f);
	CHECK(audio->GetBus(SourceId(0)).empty());

	// Until the mixer has buses every name plays on Master: the move is heard only as its 10 ms dip, out and back in.
	audio->SetBus(SourceId(0), "Music");
	CHECK(audio->GetBus(SourceId(0)) == "Music");
	std::vector<float> const samples = ReadSamples(0.03f);
	float const lowest = *std::min_element(samples.begin(), samples.end());
	CHECK(lowest < 0.05f);
	CHECK(LargestStep(samples) < 2.0f * 0.6f / static_cast<float>(AudioFrames(AudioGainSmoothingSeconds)));
	CHECK(std::abs(samples.back() - 0.6f) < 1e-3f);
	// Names match ignoring letter case: the same bus again moves nothing.
	audio->SetBus(SourceId(0), "MUSIC");
	CHECK(ReadLevels(0.02f).Left == doctest::Approx(0.6f).epsilon(0.01));
	CHECK(audio->GetVoiceCount(SourceId(0)) == 2);
}

TEST_CASE("AudioScene: a clip played at a point is attenuated by its distance, and its clip decodes once")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	Ref<AudioClipAsset> const clip = MakeLevelClip(0.5f, 0.2f);
	uint64_t const decoded = AudioEngine::GetDecodedClipCount();
	float const front = 0.5f * Testing::SpeakerGain({0.0f, 0.0f, -1.0f}, false);
	for (float const distance : {1.0f, 2.0f, 4.0f})
	{
		CAPTURE(distance);
		CHECK(audio->PlayClipAtPoint(clip, {0.0f, 0.0f, -distance}, 1.0f, ""));
		Skip(0.03f);
		Testing::AudioLevels const levels = ReadLevels(0.1f);
		CHECK(levels.Left == doctest::Approx(front / distance).epsilon(0.01));
		CHECK(levels.Right == doctest::Approx(front / distance).epsilon(0.01));
		// Past its end: the next play finds the clip decoded.
		Skip(0.1f);
	}
	CHECK(AudioEngine::GetDecodedClipCount() == decoded + 1);
	CHECK(AudioEngine::GetActiveVoiceCount() == 0);
	CHECK_FALSE(audio->PlayClipAtPoint(nullptr, glm::vec3(0.0f), 1.0f, ""));
	CHECK_FALSE(audio->PlayClipAtPoint(clip, {std::nanf(""), 0.0f, 0.0f}, 1.0f, ""));
}

TEST_CASE("AudioScene: clips play at their own sample rate, are decoded once for every source, and must stream past the limit")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	// 0.2 s at 44.1 kHz ends after 0.2 s of a 48 kHz mix.
	std::vector<float> const samples(8820, 0.5f);
	Ref<AudioClipAsset> const clip = Testing::MakeClip(Testing::MakeWav(samples, 44100));
	uint64_t const decoded = AudioEngine::GetDecodedClipCount();
	REQUIRE(audio->AddSource(MakeSource(SourceId(0), clip)).IsOk());
	REQUIRE(audio->AddSource(MakeSource(SourceId(1), clip)).IsOk());
	CHECK(AudioEngine::GetDecodedClipCount() == decoded + 1);
	CHECK(AudioEngine::PreloadClip(clip).IsOk());
	CHECK(AudioEngine::GetDecodedClipCount() == decoded + 1);
	audio->Play(SourceId(0));
	Skip(0.19f);
	CHECK(audio->IsPlaying(SourceId(0)));
	Skip(0.02f);
	CHECK_FALSE(audio->IsPlaying(SourceId(0)));

	// 16.8 MB of 16-bit mono decodes to more than 32 MiB: refused before decoding, with what to do instead.
	std::vector<float> const longSamples(8400000, 0.25f);
	Ref<AudioClipAsset> const longClip = Testing::MakeClip(Testing::MakeWav(longSamples));
	Result<void> const refused = audio->AddSource(MakeSource(SourceId(2), longClip));
	REQUIRE(refused.IsError());
	CHECK(refused.GetError().find("stream") != std::string::npos);
	CHECK_FALSE(audio->HasSource(SourceId(2)));
	CHECK(AudioEngine::PreloadClip(longClip).IsError());
	CHECK_FALSE(audio->PlayOneShot(SourceId(1), longClip, 1.0f));
	CHECK(AudioEngine::GetDecodedClipCount() == decoded + 2);
}

TEST_CASE("AudioScene: a streamed source's one-shots play a loaded clip")
{
	Testing::AudioEngineScope engine;
	Testing::TemporaryDirectory directory;
	std::filesystem::path const file = directory.GetPath() / "Music.wav";
	REQUIRE(FileSystem::WriteBinaryFile(file, Testing::MakeWav(Testing::MakeSamples({{0.25f, 1.0f}})).GetSpan()).IsOk());
	Scope<AudioScene> audio = CreateAudioScene();
	AudioSourceDesc desc;
	desc.Entity = SourceId(0);
	desc.StreamFile = file;
	desc.Spatial = false;
	REQUIRE(audio->AddSource(desc).IsOk());
	REQUIRE(audio->IsStreaming(SourceId(0)));
	CHECK_FALSE(audio->PlayOneShot(SourceId(0), nullptr, 1.0f));
	audio->Play(SourceId(0));
	CHECK(audio->PlayOneShot(SourceId(0), MakeLevelClip(0.5f, 1.0f), 1.0f));
	Skip(0.05f);
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.75f).epsilon(0.01));
}

TEST_CASE("AudioEngine: at the limit the lowest priority, then the quietest, then the oldest voice is stolen")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> first = CreateAudioScene();
	Scope<AudioScene> second = CreateAudioScene();
	Ref<AudioClipAsset> const quiet = MakeLevelClip(0.001f, 1.0f);
	uint64_t const stolen = AudioEngine::GetStolenVoiceCount();

	// The first voice, which the others outlast, is loud; 62 quiet ones fill the limit with it and the next.
	REQUIRE(first->AddSource(MakeSource(SourceId(0), MakeLevelClip(0.1f, 1.0f), true)).IsOk());
	first->Play(SourceId(0));
	for (uint64_t index = 1; index < MaxMixedAudioVoices - 1; index++)
	{
		REQUIRE(first->AddSource(MakeSource(SourceId(index), quiet, true)).IsOk());
		first->Play(SourceId(index));
	}
	// The voice of the second scene with a low priority is the 64th.
	AudioSourceDesc low = MakeSource(SourceId(100), MakeLevelClip(0.2f, 1.0f), true);
	low.Priority = 10;
	REQUIRE(second->AddSource(low).IsOk());
	second->Play(SourceId(100));
	CHECK(AudioEngine::GetActiveVoiceCount() == MaxMixedAudioVoices);
	Skip(0.02f);
	float const full = 0.1f + 0.062f + 0.2f;
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(full).epsilon(0.01));

	// One more: the low priority goes, in another scene, and becomes virtual (still playing for scripts).
	REQUIRE(first->AddSource(MakeSource(SourceId(63), quiet, true)).IsOk());
	first->Play(SourceId(63));
	CHECK(AudioEngine::GetStolenVoiceCount() == stolen + 1);
	CHECK(AudioEngine::GetActiveVoiceCount() == MaxMixedAudioVoices);
	CHECK(second->IsPlaying(SourceId(100)));
	Skip(0.02f);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(full - 0.2f + 0.001f).epsilon(0.01));

	// A new voice that ranks lowest starts virtual at once: nothing it does is heard.
	AudioSourceDesc lowest = MakeSource(SourceId(101), MakeLevelClip(0.3f, 1.0f), true);
	lowest.Priority = 0;
	REQUIRE(second->AddSource(lowest).IsOk());
	second->Play(SourceId(101));
	CHECK(second->IsPlaying(SourceId(101)));
	CHECK(AudioEngine::GetStolenVoiceCount() == stolen + 2);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(full - 0.2f + 0.001f).epsilon(0.01));
	// A one-shot that would rank lowest is refused.
	CHECK_FALSE(second->PlayOneShot(SourceId(101), nullptr, 1.0f));
	CHECK(AudioEngine::GetStolenVoiceCount() == stolen + 3);

	// At one priority the quietest goes: a voice made quieter by its volume, then by its distance.
	first->SetVolume(SourceId(10), 0.5f);
	REQUIRE(first->AddSource(MakeSource(SourceId(64), quiet, true)).IsOk());
	first->Play(SourceId(64));
	CHECK(AudioEngine::GetStolenVoiceCount() == stolen + 4);
	AudioSourceDesc far = MakeSource(SourceId(65), quiet, true);
	far.Spatial = true;
	far.Position = {0.0f, 0.0f, -50.0f};
	REQUIRE(first->AddSource(far).IsOk());
	first->Play(SourceId(65));
	CHECK(AudioEngine::GetStolenVoiceCount() == stolen + 5);
	Skip(0.02f);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(full - 0.2f + 0.001f).epsilon(0.01));

	// At one priority and loudness the oldest goes: the first, loud voice.
	REQUIRE(first->AddSource(MakeSource(SourceId(66), quiet, true)).IsOk());
	first->Play(SourceId(66));
	CHECK(AudioEngine::GetStolenVoiceCount() == stolen + 6);
	Skip(0.02f);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.064f).epsilon(0.02));
	CHECK(first->IsPlaying(SourceId(0)));

	// Room again: the highest-ranking virtual voice is heard again at the next update, where its time has taken it.
	first->Stop(SourceId(66));
	AudioEngine::Update();
	CHECK(AudioEngine::GetActiveVoiceCount() == MaxMixedAudioVoices);
	Skip(0.02f);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.063f + 0.1f).epsilon(0.02));
}

TEST_CASE("AudioEngine: a virtual voice made loud is heard again, and a scene resumed at the limit takes its places back")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	Ref<AudioClipAsset> const quiet = MakeLevelClip(0.001f, 1.0f);
	for (uint64_t index = 0; index < MaxMixedAudioVoices; index++)
	{
		REQUIRE(audio->AddSource(MakeSource(SourceId(index), quiet, true)).IsOk());
		audio->Play(SourceId(index));
	}
	// Sixteen engine layers at volume 0, as a crossfade leaves them: virtual from the start.
	Ref<AudioClipAsset> const layer = MakeLevelClip(0.1f, 1.0f);
	uint64_t const stolen = AudioEngine::GetStolenVoiceCount();
	for (uint64_t index = 100; index < 116; index++)
	{
		AudioSourceDesc desc = MakeSource(SourceId(index), layer, true);
		desc.Volume = 0.0f;
		REQUIRE(audio->AddSource(desc).IsOk());
		audio->Play(SourceId(index));
		CHECK(audio->IsPlaying(SourceId(index)));
	}
	CHECK(AudioEngine::GetStolenVoiceCount() == stolen + 16);
	CHECK(AudioEngine::GetActiveVoiceCount() == MaxMixedAudioVoices);

	// A burst of one-shots steals the oldest voices; once they end, the virtual ones that were heard come back.
	AudioSourceDesc bursts = MakeSource(SourceId(200), MakeLevelClip(0.001f, 0.05f));
	bursts.MaxVoices = MaxAudioSourceVoices;
	REQUIRE(audio->AddSource(bursts).IsOk());
	for (int shot = 0; shot < 20; shot++)
	{
		CHECK(audio->PlayOneShot(SourceId(200), nullptr, 1.0f));
	}
	CHECK(AudioEngine::GetActiveVoiceCount() == MaxMixedAudioVoices);
	Skip(0.1f);
	AudioEngine::Update();
	CHECK(AudioEngine::GetActiveVoiceCount() == MaxMixedAudioVoices);
	Skip(0.02f);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.064f).epsilon(0.02));

	// The engine raises one layer's volume: at the next update it ranks above the quietest that mixes, and is heard.
	audio->SetVolume(SourceId(105), 2.0f);
	AudioEngine::Update();
	Skip(0.02f);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.063f + 0.2f).epsilon(0.02));

	// A second scene paused with voices of its own: they do not count until it resumes, and then take places from the
	// oldest voices of the first.
	Scope<AudioScene> paused = CreateAudioScene();
	paused->SetPaused(true);
	for (uint64_t index = 300; index < 310; index++)
	{
		REQUIRE(paused->AddSource(MakeSource(SourceId(index), MakeLevelClip(0.01f, 1.0f), true)).IsOk());
		paused->Play(SourceId(index));
		CHECK(paused->IsPlaying(SourceId(index)));
	}
	CHECK(AudioEngine::GetActiveVoiceCount() == MaxMixedAudioVoices);
	uint64_t const beforeResume = AudioEngine::GetStolenVoiceCount();
	paused->SetPaused(false);
	CHECK(AudioEngine::GetStolenVoiceCount() == beforeResume + 10);
	CHECK(AudioEngine::GetActiveVoiceCount() == MaxMixedAudioVoices);
	Skip(0.02f);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.053f + 0.2f + 0.1f).epsilon(0.02));
}

TEST_CASE("AudioEngine: the active voice count is the main thread's, exact right after a start")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	for (uint64_t index = 0; index < 3; index++)
	{
		REQUIRE(audio->AddSource(MakeSource(SourceId(index), MakeLevelClip(0.1f, 0.05f))).IsOk());
		audio->Play(SourceId(index));
	}
	CHECK(AudioEngine::GetActiveVoiceCount() == 3);
	CHECK(audio->PlayOneShot(SourceId(0), nullptr, 1.0f));
	CHECK(AudioEngine::GetActiveVoiceCount() == 4);
	audio->Pause(SourceId(1));
	CHECK(AudioEngine::GetActiveVoiceCount() == 3);
	audio->SetPaused(true);
	CHECK(AudioEngine::GetActiveVoiceCount() == 0);
	audio->SetPaused(false);
	CHECK(AudioEngine::GetActiveVoiceCount() == 3);
	CHECK(AudioEngine::GetMixedVoiceCount() == 0);
	// The mixer counts what it mixed in its last block; the plays end within the read, and the count with them.
	Skip(0.01f);
	CHECK(AudioEngine::GetMixedVoiceCount() == 3);
	Skip(0.1f);
	CHECK(AudioEngine::GetActiveVoiceCount() == 0);
	CHECK(AudioEngine::GetMixedVoiceCount() == 0);
}

TEST_CASE("AudioEngine: mixing starts, steals, one-shots and changes allocates nothing on the mixing thread")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	Scope<AudioScene> other = CreateAudioScene();
	std::vector<Ref<AudioClipAsset>> const clips = {MakeLevelClip(0.001f, 0.5f), MakeLevelClip(0.002f, 0.03f),
	                                                MakeSineClip(300.0f, 0.2f, 0.01f)};
	std::vector<float> const slowSamples(4410, 0.003f);
	Ref<AudioClipAsset> const slowClip = Testing::MakeClip(Testing::MakeWav(slowSamples, 44100));
	constexpr uint64_t SourceCount = 70;
	for (uint64_t index = 0; index < SourceCount; index++)
	{
		AudioSourceDesc desc = MakeSource(SourceId(index), index % 2 == 0 ? clips[0] : slowClip, index % 3 != 0);
		desc.Spatial = index % 4 == 0;
		desc.Position = {static_cast<float>(index % 7), 0.0f, -2.0f};
		desc.Priority = static_cast<uint32_t>(index * 3 % 256);
		desc.MaxVoices = 1 + static_cast<uint32_t>(index % 5);
		REQUIRE((index < 40 ? audio : other)->AddSource(desc).IsOk());
	}
	uint64_t const miniaudioBefore = AudioEngine::GetMixAllocationCount();
	uint64_t const globalBefore = Testing::GetMixingThreadAllocationCount();

	std::vector<float> block(static_cast<size_t>(AudioFrames(0.01f)) * 2);
	for (uint32_t frame = 0; frame < 200; frame++)
	{
		for (uint64_t index = frame % 5; index < SourceCount; index += 5)
		{
			AudioScene* const scene = index < 40 ? audio.get() : other.get();
			if (scene == nullptr)
			{
				continue;
			}
			UUID const entity = SourceId(index);
			if (frame % 37 == index % 37)
			{
				scene->Stop(entity);
			}
			else
			{
				scene->Play(entity);
			}
			scene->SetVolume(entity, 0.5f + 0.5f * std::sin(static_cast<float>(frame + index)));
			scene->SetPitch(entity, 0.8f + 0.4f * static_cast<float>((frame + index) % 10) / 10.0f);
			scene->SetPosition(entity, {std::sin(static_cast<float>(frame) * 0.1f) * 5.0f, 0.0f, -2.0f});
			if (frame % 3 == 0)
			{
				scene->PlayOneShot(entity, clips[1 + index % 2], 1.0f);
			}
		}
		if (frame % 10 == 0)
		{
			audio->PlayClipAtPoint(clips[2], {1.0f, 0.0f, -3.0f}, 0.5f, "");
			AudioEngine::SetMasterVolume(frame % 20 == 0 ? 0.8f : 1.0f);
		}
		if (other && frame % 50 == 25)
		{
			other->SetPaused(true);
		}
		else if (other && frame % 50 == 40)
		{
			other->SetPaused(false);
		}
		if (frame == 150)
		{
			other.reset();
		}
		REQUIRE(AudioEngine::ReadFrames(block) == block.size() / 2);
		AudioEngine::Update();
	}
	CHECK(AudioEngine::GetStolenVoiceCount() > 0);
	CHECK(AudioEngine::GetMixAllocationCount() == miniaudioBefore);
	CHECK(Testing::GetMixingThreadAllocationCount() == globalBefore);
}
