#include "Audio/AudioTestUtilities.h"
#include "Core/ProfilerTestUtilities.h"

#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/Profiler.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <thread>
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

TEST_CASE("AudioEngine: the device's mix is recorded on the Audio Mixer thread, whose buffer returns at shutdown")
{
	Testing::ProfilerScope const profiler;
	if (!Profiler::GetStatus().Available)
	{
		return;
	}
	REQUIRE(Profiler::Start().IsOk());
	AudioEngineSettings settings;
	settings.Output = AudioOutput::Null;
	AudioEngine::Init(settings);
	REQUIRE(AudioEngine::IsInitialized());
	// The null device mixes on a thread of its own at the device's rate: frames run until one holds its mix.
	auto const countMixes = []
	{
		ProfilerCapture const& capture = Profiler::GetRecording();
		size_t mixes = 0;
		for (ProfilerFrame const& frame : capture.Frames)
		{
			for (ProfilerZone const& zone : frame.Zones)
			{
				mixes += capture.Names[zone.Name] == "Audio.Mix" && capture.Threads[zone.Thread].Name == "Audio Mixer" && zone.Depth == 0
				             ? 1
				             : 0;
			}
		}
		return mixes;
	};
	auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (countMixes() == 0 && std::chrono::steady_clock::now() < deadline)
	{
		Profiler::BeginFrame();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		Profiler::EndFrame();
	}
	CHECK(countMixes() > 0);
	// The streaming thread records on a schedule of its own, so only the mixer's entries are counted.
	auto const countMixerEntries = []
	{
		ProfilerCapture const& capture = Profiler::GetRecording();
		return std::count_if(capture.Threads.begin(), capture.Threads.end(),
		                     [](ProfilerThread const& thread)
		                     {
								 return thread.Name == "Audio Mixer";
							 });
	};
	CHECK(countMixerEntries() == 1);
	AudioEngine::Shutdown();
	// The stopped device's buffer is free again after the next collection; a device started again records under the same
	// thread entry.
	Profiler::BeginFrame();
	Profiler::EndFrame();
	size_t const before = countMixes();
	AudioEngine::Init(settings);
	REQUIRE(AudioEngine::IsInitialized());
	auto const again = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (countMixes() == before && std::chrono::steady_clock::now() < again)
	{
		Profiler::BeginFrame();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		Profiler::EndFrame();
	}
	CHECK(countMixes() > before);
	CHECK(countMixerEntries() == 1);
	AudioEngine::Shutdown();
}
