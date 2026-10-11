#include "Audio/AudioTestUtilities.h"
#include "Fuzzing.h"

#include "Strada/Audio/AudioWaveform.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <span>
#include <string>
#include <vector>

using namespace Strada;

// Level changes lie well inside parts: a part's boundaries are exact to a sixteenth of the part.
TEST_CASE("Audio: a waveform holds the peaks of equally long parts of the decoded clip")
{
	// Quiet for 1.1 seconds, then loud for 0.9; decoded without the audio engine. Parts are 0.25 seconds long.
	Ref<AudioClipAsset> const clip = Testing::MakeWavClip({{0.1f, 1.1f}, {-0.8f, 0.9f}});
	Result<AudioWaveform> const waveform = ComputeAudioWaveform(*clip, 8);
	REQUIRE_MESSAGE(waveform.IsOk(), (waveform ? std::string() : waveform.GetError()));
	AudioWaveform const& value = waveform.GetValue();
	CHECK(value.SampleRate == Testing::AudioSampleRate);
	CHECK(value.ChannelCount == 1);
	CHECK(value.Duration == doctest::Approx(2.0).epsilon(0.001));
	REQUIRE(value.Peaks.size() == 8);
	for (size_t part = 0; part < 8; part++)
	{
		CAPTURE(part);
		// The part from 1 to 1.25 seconds holds both levels: its peak is the louder one.
		CHECK(value.Peaks[part] == doctest::Approx(part < 4 ? 0.1f : 0.8f).epsilon(0.01));
	}

	// Long clips merge neighboring blocks while they are decoded (memory stays bounded); the parts stay as exact. Three
	// parts of 3 seconds over levels changing at 2.5 and 6.5 seconds.
	Ref<AudioClipAsset> const levels = Testing::MakeWavClip({{0.25f, 2.5f}, {0.5f, 4.0f}, {0.75f, 2.5f}});
	Result<AudioWaveform> const thirds = ComputeAudioWaveform(*levels, 3);
	REQUIRE(thirds.IsOk());
	REQUIRE(thirds.GetValue().Peaks.size() == 3);
	CHECK(thirds.GetValue().Peaks[0] == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(thirds.GetValue().Peaks[1] == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(thirds.GetValue().Peaks[2] == doctest::Approx(0.75f).epsilon(0.01));
	CHECK(thirds.GetValue().Duration == doctest::Approx(9.0).epsilon(0.001));
}

TEST_CASE("Audio: every format decodes into a waveform, and very short clips have fewer parts")
{
	std::vector<float> const samples = Testing::MakeSamples({{0.5f, 0.5f}});
	std::vector<Ref<AudioClipAsset>> const clips = {Testing::MakeClip(Testing::MakeWav(samples)),
	                                                Testing::MakeClip(Testing::MakeFlac(samples)),
	                                                Testing::MakeClip(Testing::MakeSilentMp3(20))};
	for (Ref<AudioClipAsset> const& clip : clips)
	{
		CAPTURE(AudioFormatToString(clip->GetFormat()));
		Result<AudioWaveform> const waveform = ComputeAudioWaveform(*clip, 64);
		REQUIRE_MESSAGE(waveform.IsOk(), (waveform ? std::string() : waveform.GetError()));
		CHECK(waveform.GetValue().Peaks.size() == 64);
		CHECK(waveform.GetValue().Duration > 0.4);
		bool const silent = clip->GetFormat() == AudioFormat::Mp3;
		CHECK(waveform.GetValue().Peaks.front() == doctest::Approx(silent ? 0.0f : 0.5f).epsilon(0.01));
	}

	// 100 frames fill one block of 64 frames and part of another: two parts at most.
	std::vector<float> const tiny(100, 0.25f);
	Result<AudioWaveform> const tinyWaveform = ComputeAudioWaveform(*Testing::MakeClip(Testing::MakeWav(tiny)), 64);
	REQUIRE(tinyWaveform.IsOk());
	CHECK(tinyWaveform.GetValue().Peaks.size() == 2);
	CHECK(tinyWaveform.GetValue().Peaks.back() == doctest::Approx(0.25f).epsilon(0.01));
}

TEST_CASE("Audio: Ogg Vorbis clips decode into waveforms")
{
	Result<Buffer> data = FileSystem::ReadBinaryFile(FileSystem::PathFromUtf8(STRADA_TEST_OGG_PATH));
	REQUIRE(data.IsOk());
	Ref<AudioClipAsset> const ogg = Testing::MakeClip(data.TakeValue());
	REQUIRE(ogg->GetFormat() == AudioFormat::Ogg);
	Result<AudioWaveform> const waveform = ComputeAudioWaveform(*ogg, 32);
	REQUIRE_MESSAGE(waveform.IsOk(), (waveform ? std::string() : waveform.GetError()));
	CHECK(waveform.GetValue().SampleRate == 48000);
	CHECK(waveform.GetValue().ChannelCount == 2);
	CHECK(waveform.GetValue().Duration > 1.0);
	REQUIRE(waveform.GetValue().Peaks.size() == 32);
	CHECK(*std::max_element(waveform.GetValue().Peaks.begin(), waveform.GetValue().Peaks.end()) > 0.05f);
}

TEST_CASE("Audio: damaged clips give a waveform or an error, never more")
{
	std::vector<float> const samples = Testing::MakeSamples({{0.5f, 0.1f}, {-0.25f, 0.1f}});
	Result<Buffer> ogg = FileSystem::ReadBinaryFile(FileSystem::PathFromUtf8(STRADA_TEST_OGG_PATH));
	REQUIRE(ogg.IsOk());
	auto const bytes = [](std::span<uint8_t const> data)
	{
		return std::vector<uint8_t>(data.begin(), data.end());
	};
	// The Ogg file's first 64 KB: its headers and first pages.
	std::span<uint8_t const> const oggBytes = ogg.GetValue().GetSpan();
	std::vector<std::vector<uint8_t>> const seeds = {
		bytes(Testing::MakeWav(samples).GetSpan()), bytes(Testing::MakeFlac(samples).GetSpan()),
		bytes(Testing::MakeSilentMp3(10).GetSpan()), bytes(oggBytes.first(std::min<size_t>(oggBytes.size(), 65536)))};

	Testing::FuzzRandom random(9100);
	int computed = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		std::vector<uint8_t> data = seeds[random.Pick(seeds.size())];
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
					// Headers and metadata.
					data[random.Pick(std::min<size_t>(data.size(), 256))] = std::array<uint8_t, 4>{0x00, 0x7F, 0x80, 0xFF}[random.Pick(4)];
					break;
				default:
					data[random.Pick(data.size())] ^= static_cast<uint8_t>(1u << random.Pick(8));
					break;
			}
		}
		Result<Ref<AudioClipAsset>> clip = AudioClipAsset::Create(Buffer::Copy(data.data(), data.size()));
		if (!clip)
		{
			refused++;
			continue;
		}
		Result<AudioWaveform> const waveform = ComputeAudioWaveform(*clip.GetValue(), 16);
		if (!waveform)
		{
			refused++;
			continue;
		}
		// What decodes is a waveform: up to the parts asked for, peaks between 0 and 1, a real length.
		AudioWaveform const& value = waveform.GetValue();
		CHECK_FALSE(value.Peaks.empty());
		CHECK(value.Peaks.size() <= 16);
		CHECK(std::all_of(value.Peaks.begin(), value.Peaks.end(),
		                  [](float peak)
		                  {
							  return peak >= 0.0f && peak <= 1.0f;
						  }));
		CHECK(std::isfinite(value.Duration));
		CHECK(value.Duration > 0.0);
		CHECK(value.ChannelCount > 0);
		CHECK(value.SampleRate > 0);
		computed++;
	}
	MESSAGE(computed << " damaged clips decoded, " << refused << " refused");
	CHECK(computed > 0);
	CHECK(refused > 0);
}

TEST_CASE("Audio: waveforms fail for clips that cannot be decoded, without parts and when cancelled")
{
	// A WAV signature over garbage.
	std::vector<uint8_t> const broken = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E', 1, 2, 3, 4};
	Result<Ref<AudioClipAsset>> const clip = AudioClipAsset::Create(Buffer::Copy(broken.data(), broken.size()));
	REQUIRE(clip.IsOk());
	CHECK(ComputeAudioWaveform(*clip.GetValue(), 16).IsError());

	Ref<AudioClipAsset> const tone = Testing::MakeWavClip({{0.5f, 0.5f}});
	CHECK(ComputeAudioWaveform(*tone, 0).IsError());
	std::atomic<bool> const cancel{true};
	Result<AudioWaveform> const cancelled = ComputeAudioWaveform(*tone, 16, &cancel);
	REQUIRE(cancelled.IsError());
	CHECK(cancelled.GetError() == "cancelled");
}
