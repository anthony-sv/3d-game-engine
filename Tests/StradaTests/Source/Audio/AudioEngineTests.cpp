#include "Audio/AudioTestUtilities.h"

#include "Strada/Audio/AudioEngine.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

using namespace Strada;

TEST_CASE("AudioEngine: manual output mixes silence until sounds play")
{
	{
		Testing::AudioEngineScope engine;
		CHECK(AudioEngine::GetOutput() == AudioOutput::Manual);
		CHECK_FALSE(AudioEngine::HasDevice());
		CHECK(AudioEngine::GetSampleRate() == Testing::AudioSampleRate);
		CHECK(AudioEngine::GetChannelCount() == 2);

		// Whole frames only: the fifth sample is left alone.
		std::vector<float> samples(5, 7.0f);
		CHECK(AudioEngine::ReadFrames(samples) == 2);
		CHECK(std::all_of(samples.begin(), samples.begin() + 4,
		                  [](float sample)
		                  {
							  return sample == 0.0f;
						  }));
		CHECK(samples[4] == 7.0f);
		CHECK(AudioEngine::ReadFrames({}) == 0);

		CHECK(AudioEngine::GetMasterVolume() == doctest::Approx(1.0f));
		AudioEngine::SetMasterVolume(-2.0f);
		CHECK(AudioEngine::GetMasterVolume() == 0.0f);
		AudioEngine::SetMasterVolume(0.5f);
		CHECK(AudioEngine::GetMasterVolume() == doctest::Approx(0.5f));
	}
	CHECK_FALSE(AudioEngine::IsInitialized());
	CHECK(AudioEngine::GetOutput() == AudioOutput::Null);
	CHECK_FALSE(AudioEngine::HasDevice());
	CHECK(AudioEngine::GetSampleRate() == 0);
	CHECK(AudioEngine::GetChannelCount() == 0);
	CHECK(AudioEngine::GetMasterVolume() == 0.0f);
}

TEST_CASE("AudioEngine: unsupported manual formats leave the engine uninitialized")
{
	AudioEngineSettings settings;
	settings.Output = AudioOutput::Manual;
	settings.ChannelCount = 0;
	AudioEngine::Init(settings);
	CHECK_FALSE(AudioEngine::IsInitialized());

	settings.ChannelCount = 2;
	settings.SampleRate = 1000;
	AudioEngine::Init(settings);
	CHECK_FALSE(AudioEngine::IsInitialized());
}

TEST_CASE("AudioEngine: the null output runs without a device")
{
	AudioEngineSettings settings;
	settings.Output = AudioOutput::Null;
	AudioEngine::Init(settings);
	REQUIRE(AudioEngine::IsInitialized());
	CHECK(AudioEngine::GetOutput() == AudioOutput::Null);
	CHECK_FALSE(AudioEngine::HasDevice());
	CHECK(AudioEngine::GetSampleRate() > 0);
	CHECK(AudioEngine::GetChannelCount() > 0);
	AudioEngine::Shutdown();
	CHECK_FALSE(AudioEngine::IsInitialized());
}
