#include "Audio/AudioTestUtilities.h"
#include "Fuzzing.h"
#include "TestUtilities.h"

#include "Strada/Audio/AudioScene.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;
using Strada::Testing::AudioLevels;
using Strada::Testing::ReadLevels;
using Strada::Testing::Skip;

namespace
{
	UUID const SourceId(201);
	UUID const OtherId(202);

	Scope<AudioScene> CreateAudioScene()
	{
		Result<Scope<AudioScene>> scene = AudioScene::Create();
		REQUIRE(scene.IsOk());
		return scene.TakeValue();
	}

	AudioSourceDesc MakeSource(UUID entity, Ref<AudioClipAsset> clip, bool spatial = false)
	{
		AudioSourceDesc desc;
		desc.Entity = entity;
		desc.Clip = std::move(clip);
		desc.Spatial = spatial;
		return desc;
	}

	Ref<AudioClipAsset> MakeClip(std::vector<uint8_t> const& bytes)
	{
		return Testing::MakeClip(Buffer::Copy(bytes.data(), bytes.size()));
	}

	AudioSourceDesc MakeStream(UUID entity, std::filesystem::path file, bool loop = false)
	{
		AudioSourceDesc desc;
		desc.Entity = entity;
		desc.StreamFile = std::move(file);
		desc.Spatial = false;
		desc.Loop = loop;
		return desc;
	}

	std::filesystem::path WriteFile(Testing::TemporaryDirectory const& directory, std::string_view name, std::span<uint8_t const> data)
	{
		std::filesystem::path const path = directory.GetPath() / FileSystem::PathFromUtf8(name);
		REQUIRE(FileSystem::WriteBinaryFile(path, data).IsOk());
		return path;
	}

	// The interleaved stereo samples of the next part of the manual mix.
	std::vector<float> ReadSamples(float seconds)
	{
		uint32_t const frames = Testing::AudioFrames(seconds);
		std::vector<float> samples(static_cast<size_t>(frames) * 2);
		REQUIRE(AudioEngine::ReadFrames(samples) == frames);
		return samples;
	}

	std::vector<uint8_t> ToBytes(std::span<uint8_t const> data)
	{
		return std::vector<uint8_t>(data.begin(), data.end());
	}

	// Files of every format, short enough to fuzz quickly: the Ogg file's first 64 KB hold its headers and first pages.
	std::vector<std::vector<uint8_t>> MakeDamageSeeds()
	{
		std::vector<float> const samples = Testing::MakeSamples({{0.5f, 0.1f}, {-0.25f, 0.1f}});
		Result<Buffer> ogg = FileSystem::ReadBinaryFile(FileSystem::PathFromUtf8(STRADA_TEST_OGG_PATH));
		REQUIRE(ogg.IsOk());
		std::span<uint8_t const> const oggBytes = ogg.GetValue().GetSpan();
		return {ToBytes(Testing::MakeWav(samples).GetSpan()), ToBytes(Testing::MakeFlac(samples).GetSpan()),
		        ToBytes(Testing::MakeSilentMp3(10).GetSpan()), ToBytes(oggBytes.first(std::min<size_t>(oggBytes.size(), 65536)))};
	}

	// Truncates the data or changes some of its bytes, headers and metadata more often than their share.
	void Damage(std::vector<uint8_t>& data, Testing::FuzzRandom& random)
	{
		for (size_t count = 1 + random.Pick(4); count > 0 && data.size() > 1; count--)
		{
			switch (random.Pick(4))
			{
				case 0:
					data.resize(1 + random.Pick(data.size()));
					break;
				case 1:
					data[random.Pick(data.size())] = static_cast<uint8_t>(random.Next());
					break;
				case 2:
					data[random.Pick(std::min<size_t>(data.size(), 256))] = std::array<uint8_t, 4>{0x00, 0x7F, 0x80, 0xFF}[random.Pick(4)];
					break;
				default:
					data[random.Pick(data.size())] ^= static_cast<uint8_t>(1u << random.Pick(8));
					break;
			}
		}
	}
}

TEST_CASE("AudioScene: scenes need the audio engine and sources need a playable clip")
{
	CHECK(AudioScene::Create().IsError());

	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	CHECK(audio->AddSource(MakeSource(SourceId, nullptr)).IsError());
	// A WAV signature in front of data that is not a WAV file.
	CHECK(audio->AddSource(MakeSource(SourceId, MakeClip({'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E', 1, 2, 3, 4}))).IsError());
	CHECK(audio->GetSourceCount() == 0);

	Ref<AudioClipAsset> const clip = Testing::MakeWavClip({{0.5f, 0.1f}});
	AudioSourceDesc desc = MakeSource(SourceId, clip, true);
	desc.Volume = 0.25f;
	desc.Pitch = 1.5f;
	desc.Loop = true;
	desc.Position = {1.0f, 2.0f, 3.0f};
	REQUIRE(audio->AddSource(desc).IsOk());
	CHECK(audio->HasSource(SourceId));
	CHECK(audio->GetSourceCount() == 1);
	CHECK_FALSE(audio->IsPlaying(SourceId));
	CHECK(audio->GetVolume(SourceId) == doctest::Approx(0.25f));
	CHECK(audio->GetPitch(SourceId) == doctest::Approx(1.5f));
	CHECK(audio->IsLooping(SourceId));
	CHECK(audio->IsSpatial(SourceId));
	CHECK(audio->GetPosition(SourceId) == glm::vec3(1.0f, 2.0f, 3.0f));

	// Adding again replaces the sound; a failed replacement keeps it.
	REQUIRE(audio->AddSource(MakeSource(SourceId, clip)).IsOk());
	CHECK(audio->GetSourceCount() == 1);
	CHECK(audio->GetVolume(SourceId) == doctest::Approx(1.0f));
	CHECK_FALSE(audio->IsLooping(SourceId));
	CHECK_FALSE(audio->IsSpatial(SourceId));
	CHECK(audio->AddSource(MakeSource(SourceId, nullptr)).IsError());
	CHECK(audio->HasSource(SourceId));

	// Out-of-range settings are clamped and non-finite positions ignored.
	audio->SetVolume(SourceId, -1.0f);
	CHECK(audio->GetVolume(SourceId) == 0.0f);
	audio->SetPitch(SourceId, 0.0f);
	CHECK(audio->GetPitch(SourceId) == doctest::Approx(MinAudioPitch));
	audio->SetPitch(SourceId, std::numeric_limits<float>::quiet_NaN());
	CHECK(audio->GetPitch(SourceId) == doctest::Approx(MinAudioPitch));
	audio->SetPosition(SourceId, {4.0f, 5.0f, 6.0f});
	audio->SetPosition(SourceId, {std::numeric_limits<float>::infinity(), 0.0f, 0.0f});
	CHECK(audio->GetPosition(SourceId) == glm::vec3(4.0f, 5.0f, 6.0f));

	// Entities without a source are ignored.
	audio->Play(OtherId);
	audio->SetVolume(OtherId, 1.0f);
	CHECK_FALSE(audio->IsPlaying(OtherId));
	CHECK(audio->GetVolume(OtherId) == 0.0f);

	audio->RemoveSource(SourceId);
	CHECK_FALSE(audio->HasSource(SourceId));
	CHECK(audio->GetSourceCount() == 0);
}

TEST_CASE("AudioScene: sounds play from the start, end, and start over")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId, Testing::MakeWavClip({{0.5f, 0.25f}}))).IsOk());

	AudioLevels const silent = ReadLevels(0.05f);
	CHECK(silent.Left == 0.0f);
	CHECK(silent.Right == 0.0f);

	audio->Play(SourceId);
	CHECK(audio->IsPlaying(SourceId));
	AudioLevels const playing = ReadLevels(0.1f);
	CHECK(playing.Left == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(playing.Right == doctest::Approx(0.5f).epsilon(0.01));

	audio->SetVolume(SourceId, 0.5f);
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.25f).epsilon(0.01));
	// The clip ends 0.05 s into the next tenth of a second.
	Skip(0.1f);
	CHECK_FALSE(audio->IsPlaying(SourceId));
	CHECK(ReadLevels(0.05f).Left == 0.0f);

	audio->Play(SourceId);
	CHECK(audio->IsPlaying(SourceId));
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.25f).epsilon(0.01));
}

TEST_CASE("AudioScene: a sound played again the moment it ends starts over")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	REQUIRE(audio->AddSource(MakeSource(SourceId, Testing::MakeWavClip({{0.5f, 0.25f}}))).IsOk());

	// Reading up to the last frame, then a frame past it, finds the end without the mixer stopping the sound yet.
	audio->Play(SourceId);
	for (uint32_t frame = 0; frame < 4 && audio->IsPlaying(SourceId); frame++)
	{
		Skip(frame == 0 ? 0.25f : 1.0f / static_cast<float>(Testing::AudioSampleRate));
	}
	REQUIRE_FALSE(audio->IsPlaying(SourceId));
	audio->Play(SourceId);
	CHECK(audio->IsPlaying(SourceId));
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("AudioScene: pausing keeps the position and stopping rewinds")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	// A quiet first half second, then a loud one: the level tells where playback is.
	REQUIRE(audio->AddSource(MakeSource(SourceId, Testing::MakeWavClip({{0.25f, 0.5f}, {0.75f, 0.5f}}))).IsOk());

	audio->Play(SourceId);
	Skip(0.4f);
	audio->Pause(SourceId);
	CHECK_FALSE(audio->IsPlaying(SourceId));
	CHECK(ReadLevels(0.2f).Left == 0.0f);

	// Resumes at 0.4 s.
	audio->Play(SourceId);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.25f).epsilon(0.02));
	Skip(0.1f);
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.75f).epsilon(0.02));

	// Stopping rewinds to the start.
	audio->Stop(SourceId);
	CHECK_FALSE(audio->IsPlaying(SourceId));
	CHECK(ReadLevels(0.1f).Left == 0.0f);
	audio->Play(SourceId);
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.25f).epsilon(0.02));

	// Playing a playing sound changes nothing: playback carries on into the loud half.
	Skip(0.35f);
	audio->Play(SourceId);
	Skip(0.1f);
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.75f).epsilon(0.02));
}

TEST_CASE("AudioScene: looping sounds repeat and pitch changes the playback speed")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	AudioSourceDesc looping = MakeSource(SourceId, Testing::MakeWavClip({{0.5f, 0.1f}}));
	looping.Loop = true;
	REQUIRE(audio->AddSource(looping).IsOk());
	audio->Play(SourceId);
	CHECK(ReadLevels(0.35f).Left == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(audio->IsPlaying(SourceId));

	// The current repetition is the last.
	audio->SetLooping(SourceId, false);
	CHECK_FALSE(audio->IsLooping(SourceId));
	Skip(0.15f);
	CHECK_FALSE(audio->IsPlaying(SourceId));

	// Twice the speed plays a 0.2 s clip in 0.1 s.
	REQUIRE(audio->AddSource(MakeSource(OtherId, Testing::MakeWavClip({{0.5f, 0.2f}}))).IsOk());
	audio->SetPitch(OtherId, 2.0f);
	audio->Play(OtherId);
	Skip(0.08f);
	CHECK(audio->IsPlaying(OtherId));
	Skip(0.04f);
	CHECK_FALSE(audio->IsPlaying(OtherId));
}

TEST_CASE("AudioScene: spatial sounds are attenuated with distance and panned around the listener")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	AudioSourceDesc desc = MakeSource(SourceId, Testing::MakeWavClip({{0.5f, 0.1f}}), true);
	desc.Loop = true;
	desc.MinDistance = 1.0f;
	desc.MaxDistance = 10.0f;
	REQUIRE(audio->AddSource(desc).IsOk());
	audio->Play(SourceId);

	// Measured once the gains settled (miniaudio smooths gain changes over a few milliseconds).
	auto const measure = [&audio](glm::vec3 const& position)
	{
		audio->SetPosition(SourceId, position);
		Skip(0.03f);
		return ReadLevels(0.05f);
	};

	// In front: both speakers alike, unattenuated within the minimum distance.
	float const front = 0.5f * Testing::SpeakerGain({0.0f, 0.0f, -1.0f}, false);
	AudioLevels levels = measure({0.0f, 0.0f, -1.0f});
	CHECK(levels.Left == doctest::Approx(front).epsilon(0.01));
	CHECK(levels.Right == doctest::Approx(front).epsilon(0.01));
	CHECK(measure({0.0f, 0.0f, -0.5f}).Left == doctest::Approx(front).epsilon(0.01));
	// Inverse distance beyond it, and no quieter beyond the maximum distance.
	CHECK(measure({0.0f, 0.0f, -4.0f}).Left == doctest::Approx(front / 4.0f).epsilon(0.01));
	CHECK(measure({0.0f, 0.0f, -40.0f}).Left == doctest::Approx(front / 10.0f).epsilon(0.01));

	// On the listener's right.
	levels = measure({2.0f, 0.0f, 0.0f});
	CHECK(levels.Left == doctest::Approx(0.5f * Testing::SpeakerGain({1.0f, 0.0f, 0.0f}, false) / 2.0f).epsilon(0.01));
	CHECK(levels.Right == doctest::Approx(0.5f * Testing::SpeakerGain({1.0f, 0.0f, 0.0f}, true) / 2.0f).epsilon(0.01));

	// Turned around, the listener has the sound on its left.
	audio->SetListener(glm::vec3(0.0f), {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f});
	levels = measure({2.0f, 0.0f, 0.0f});
	CHECK(levels.Left > 3.0f * levels.Right);
	// Next to the sound and facing it: in front, within the minimum distance.
	audio->SetListener({2.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
	CHECK(audio->GetListenerPosition() == glm::vec3(2.0f, 0.0f, 1.0f));
	levels = measure({2.0f, 0.0f, 0.0f});
	CHECK(levels.Left == doctest::Approx(front).epsilon(0.01));
	CHECK(levels.Right == doctest::Approx(front).epsilon(0.01));

	// Scaled and non-orthogonal vectors, a missing forward and a forward parallel to up still give a valid basis.
	audio->SetListener(glm::vec3(0.0f), {0.0f, 0.0f, -5.0f}, {0.0f, 3.0f, -1.0f});
	levels = measure({2.0f, 0.0f, 0.0f});
	CHECK(levels.Right > 3.0f * levels.Left);
	audio->SetListener(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f));
	levels = measure({2.0f, 0.0f, 0.0f});
	CHECK(levels.Right > 3.0f * levels.Left);
	audio->SetListener(glm::vec3(0.0f), {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
	levels = measure({2.0f, 0.0f, 0.0f});
	CHECK(std::isfinite(levels.Left));
	CHECK(std::isfinite(levels.Right));
	CHECK(levels.Left > 0.0f);
	audio->SetListener({std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
	CHECK(audio->GetListenerPosition() == glm::vec3(0.0f));

	// Non-spatial sounds play at their own level wherever they are.
	audio->SetSpatial(SourceId, false);
	CHECK_FALSE(audio->IsSpatial(SourceId));
	levels = measure({0.0f, 0.0f, -40.0f});
	CHECK(levels.Left == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(levels.Right == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("AudioScene: pausing a scene holds its sounds without affecting other scenes")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> first = CreateAudioScene();
	Scope<AudioScene> second = CreateAudioScene();
	REQUIRE(first->AddSource(MakeSource(SourceId, Testing::MakeWavClip({{0.25f, 0.5f}, {0.75f, 0.5f}}))).IsOk());
	AudioSourceDesc spatial = MakeSource(OtherId, Testing::MakeWavClip({{0.5f, 1.0f}}), true);
	spatial.Position = {2.0f, 0.0f, 0.0f};
	REQUIRE(second->AddSource(spatial).IsOk());
	first->Play(SourceId);
	Skip(0.4f);

	first->SetPaused(true);
	CHECK(first->IsPaused());
	CHECK(first->IsPlaying(SourceId));
	CHECK(ReadLevels(0.2f).Left == 0.0f);

	// The other scene plays on and is heard from its own listener: on the right, although the first scene's listener
	// turned around.
	first->SetListener(glm::vec3(0.0f), {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f});
	second->Play(OtherId);
	Skip(0.03f);
	AudioLevels const levels = ReadLevels(0.1f);
	CHECK(levels.Right > 3.0f * levels.Left);
	second->Stop(OtherId);

	// Resuming continues where the scene paused (0.4 s).
	first->SetPaused(false);
	CHECK_FALSE(first->IsPaused());
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.25f).epsilon(0.02));
	Skip(0.1f);
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.75f).epsilon(0.02));
}

TEST_CASE("AudioScene: FLAC, MP3 and WAV clips play and broken Ogg streams fail cleanly")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();

	std::vector<float> const samples = Testing::MakeSamples({{0.5f, 0.2f}});
	Ref<AudioClipAsset> const flac = Testing::MakeClip(Testing::MakeFlac(samples));
	REQUIRE(flac->GetFormat() == AudioFormat::Flac);
	REQUIRE(audio->AddSource(MakeSource(SourceId, flac)).IsOk());
	audio->Play(SourceId);
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.5f).epsilon(0.01));
	Skip(0.15f);
	CHECK_FALSE(audio->IsPlaying(SourceId));

	// Ten silent frames play for 0.24 s.
	Ref<AudioClipAsset> const mp3 = Testing::MakeClip(Testing::MakeSilentMp3(10));
	REQUIRE(mp3->GetFormat() == AudioFormat::Mp3);
	REQUIRE(audio->AddSource(MakeSource(OtherId, mp3)).IsOk());
	audio->Play(OtherId);
	Skip(0.15f);
	CHECK(audio->IsPlaying(OtherId));
	Skip(0.15f);
	CHECK_FALSE(audio->IsPlaying(OtherId));

	// An Ogg page header without a Vorbis stream.
	Ref<AudioClipAsset> const ogg = MakeClip({'O', 'g', 'g', 'S', 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
	REQUIRE(ogg->GetFormat() == AudioFormat::Ogg);
	CHECK(audio->AddSource(MakeSource(UUID(203), ogg)).IsError());
}

TEST_CASE("AudioScene: Ogg Vorbis clips play")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	Result<Buffer> data = FileSystem::ReadBinaryFile(FileSystem::PathFromUtf8(STRADA_TEST_OGG_PATH));
	REQUIRE(data.IsOk());
	Ref<AudioClipAsset> const ogg = Testing::MakeClip(data.TakeValue());
	REQUIRE(ogg->GetFormat() == AudioFormat::Ogg);
	REQUIRE(audio->AddSource(MakeSource(SourceId, ogg)).IsOk());
	audio->Play(SourceId);
	AudioLevels const levels = ReadLevels(0.5f);
	CHECK(levels.Left > 0.001f);
	CHECK(levels.Right > 0.001f);
	CHECK(audio->IsPlaying(SourceId));
}

TEST_CASE("AudioScene: damaged clips are refused or play")
{
	Testing::AudioEngineScope engine;
	Scope<AudioScene> audio = CreateAudioScene();
	std::vector<std::vector<uint8_t>> const seeds = MakeDamageSeeds();

	Testing::FuzzRandom random(8000);
	int played = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		std::vector<uint8_t> data = seeds[random.Pick(seeds.size())];
		Damage(data, random);
		Result<Ref<AudioClipAsset>> clip = AudioClipAsset::Create(Buffer::Copy(data.data(), data.size()));
		if (!clip)
		{
			refused++;
			continue;
		}
		// Looping sources seek back to the start when they end.
		AudioSourceDesc desc = MakeSource(SourceId, clip.TakeValue());
		desc.Loop = round % 2 == 0;
		if (!audio->AddSource(desc))
		{
			refused++;
			continue;
		}
		audio->Play(SourceId);
		Skip(0.2f);
		audio->RemoveSource(SourceId);
		played++;
	}
	CHECK(played > 0);
	CHECK(refused > 0);
}

TEST_CASE("AudioScene: streamed sources play their file, pause, stop, end and start over")
{
	Testing::AudioEngineScope engine;
	Testing::TemporaryDirectory directory;
	Scope<AudioScene> audio = CreateAudioScene();
	// A quiet first half second, then a loud one: the level tells where playback is. The file name is not ASCII.
	std::vector<float> const samples = Testing::MakeSamples({{0.25f, 0.5f}, {0.75f, 0.5f}});
	std::filesystem::path const file = WriteFile(directory, "M\xC3\xBCsik \xE2\x99\xAA.wav", Testing::MakeWav(samples).GetSpan());
	REQUIRE(audio->AddSource(MakeStream(SourceId, file)).IsOk());
	CHECK(audio->IsStreaming(SourceId));
	CHECK_FALSE(audio->IsPlaying(SourceId));
	CHECK(ReadLevels(0.05f).Left == 0.0f);

	audio->Play(SourceId);
	CHECK(audio->IsPlaying(SourceId));
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.25f).epsilon(0.02));
	Skip(0.3f);
	audio->Pause(SourceId);
	CHECK_FALSE(audio->IsPlaying(SourceId));
	CHECK(ReadLevels(0.2f).Left == 0.0f);
	// Resumes at 0.4 s.
	audio->Play(SourceId);
	CHECK(ReadLevels(0.05f).Left == doctest::Approx(0.25f).epsilon(0.02));
	Skip(0.1f);
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.75f).epsilon(0.02));

	// Stopping rewinds to the start.
	audio->Stop(SourceId);
	CHECK(ReadLevels(0.1f).Left == 0.0f);
	audio->Play(SourceId);
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.25f).epsilon(0.02));

	// Played to its end it stops; played again it starts over.
	Skip(1.0f);
	CHECK_FALSE(audio->IsPlaying(SourceId));
	CHECK(ReadLevels(0.05f).Left == 0.0f);
	audio->Play(SourceId);
	CHECK(audio->IsPlaying(SourceId));
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.25f).epsilon(0.02));

	// Settings apply as to loaded clips; replacing the source with a loaded clip stops streaming.
	audio->SetVolume(SourceId, 0.5f);
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.125f).epsilon(0.02));
	REQUIRE(audio->AddSource(MakeSource(SourceId, Testing::MakeWavClip({{0.5f, 0.1f}}))).IsOk());
	CHECK_FALSE(audio->IsStreaming(SourceId));
	CHECK_FALSE(audio->IsStreaming(OtherId));
}

TEST_CASE("AudioScene: looping streamed sources start over without a gap")
{
	Testing::AudioEngineScope engine;
	Testing::TemporaryDirectory directory;
	Scope<AudioScene> audio = CreateAudioScene();
	std::filesystem::path const file = WriteFile(directory, "Loop.wav", Testing::MakeWav(Testing::MakeSamples({{0.5f, 0.1f}})).GetSpan());
	REQUIRE(audio->AddSource(MakeStream(SourceId, file, true)).IsOk());
	CHECK(audio->IsLooping(SourceId));
	audio->Play(SourceId);
	// Past the resampler's first samples, every sample of three passes through the file is heard.
	Skip(0.02f);
	std::vector<float> const samples = ReadSamples(0.3f);
	CHECK(std::all_of(samples.begin(), samples.end(),
	                  [](float sample)
	                  {
						  return std::abs(sample - 0.5f) < 0.01f;
					  }));
	CHECK(audio->IsPlaying(SourceId));

	// The current repetition is the last.
	audio->SetLooping(SourceId, false);
	Skip(0.15f);
	CHECK_FALSE(audio->IsPlaying(SourceId));
}

TEST_CASE("AudioScene: Ogg Vorbis, FLAC and MP3 files stream")
{
	Testing::AudioEngineScope engine;
	Testing::TemporaryDirectory directory;
	Scope<AudioScene> audio = CreateAudioScene();

	REQUIRE(audio->AddSource(MakeStream(SourceId, FileSystem::PathFromUtf8(STRADA_TEST_OGG_PATH))).IsOk());
	audio->Play(SourceId);
	AudioLevels const ogg = ReadLevels(0.5f);
	CHECK(ogg.Left > 0.001f);
	CHECK(ogg.Right > 0.001f);
	CHECK(audio->IsPlaying(SourceId));
	audio->RemoveSource(SourceId);

	std::filesystem::path const flac = WriteFile(directory, "Tone.flac", Testing::MakeFlac(Testing::MakeSamples({{0.5f, 0.2f}})).GetSpan());
	REQUIRE(audio->AddSource(MakeStream(SourceId, flac)).IsOk());
	audio->Play(SourceId);
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.5f).epsilon(0.01));
	Skip(0.15f);
	CHECK_FALSE(audio->IsPlaying(SourceId));

	// Ten silent frames play for 0.24 s.
	std::filesystem::path const mp3 = WriteFile(directory, "Silence.mp3", Testing::MakeSilentMp3(10).GetSpan());
	REQUIRE(audio->AddSource(MakeStream(OtherId, mp3)).IsOk());
	audio->Play(OtherId);
	Skip(0.15f);
	CHECK(audio->IsPlaying(OtherId));
	Skip(0.15f);
	CHECK_FALSE(audio->IsPlaying(OtherId));
}

TEST_CASE("AudioScene: streamed sources refuse missing, unsupported and broken files and play damaged ones")
{
	Testing::AudioEngineScope engine;
	Testing::TemporaryDirectory directory;
	Scope<AudioScene> audio = CreateAudioScene();
	std::filesystem::path const tone = WriteFile(directory, "Tone.wav", Testing::MakeWav(Testing::MakeSamples({{0.5f, 0.2f}})).GetSpan());
	REQUIRE(audio->AddSource(MakeStream(SourceId, tone)).IsOk());

	// Each failure keeps the current sound.
	CHECK(audio->AddSource(MakeStream(SourceId, directory.GetPath() / "Missing.wav")).IsError());
	CHECK(audio->AddSource(MakeStream(SourceId, directory.GetPath())).IsError());
	std::string_view const text = "This is not a sound.";
	std::vector<uint8_t> const notes(text.begin(), text.end());
	CHECK(audio->AddSource(MakeStream(SourceId, WriteFile(directory, "Notes.wav", notes))).IsError());
	std::vector<uint8_t> const notWav = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E', 1, 2, 3, 4};
	CHECK(audio->AddSource(MakeStream(SourceId, WriteFile(directory, "Broken.wav", notWav))).IsError());
	CHECK(audio->AddSource(MakeStream(SourceId, WriteFile(directory, "Empty.wav", {}))).IsError());
	CHECK(audio->HasSource(SourceId));
	CHECK(audio->IsStreaming(SourceId));
	audio->Play(SourceId);
	CHECK(ReadLevels(0.1f).Left == doctest::Approx(0.5f).epsilon(0.01));

	// A file cut short plays what it has, then ends.
	std::vector<uint8_t> cut = ToBytes(Testing::MakeWav(Testing::MakeSamples({{0.5f, 1.0f}})).GetSpan());
	cut.resize(cut.size() / 4);
	REQUIRE(audio->AddSource(MakeStream(OtherId, WriteFile(directory, "Cut.wav", cut))).IsOk());
	audio->Play(OtherId);
	Skip(0.3f);
	CHECK_FALSE(audio->IsPlaying(OtherId));

	// Damaged files are refused or play, looping or not, and never take the mixer down.
	std::vector<std::vector<uint8_t>> const seeds = MakeDamageSeeds();
	Testing::FuzzRandom random(8100);
	int played = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(150); round < rounds; round++)
	{
		CAPTURE(round);
		std::vector<uint8_t> data = seeds[random.Pick(seeds.size())];
		Damage(data, random);
		AudioSourceDesc desc = MakeStream(UUID(300), WriteFile(directory, "Damaged" + std::to_string(round), data), round % 2 == 0);
		if (!audio->AddSource(desc))
		{
			refused++;
			continue;
		}
		audio->Play(UUID(300));
		Skip(0.2f);
		audio->RemoveSource(UUID(300));
		played++;
	}
	CHECK(played > 0);
	CHECK(refused > 0);
}
