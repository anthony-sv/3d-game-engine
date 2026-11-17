#include "Audio/AudioTestUtilities.h"
#include "Core/ProfilerTestUtilities.h"

#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Core/Profiler.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
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

TEST_CASE("AudioEngine: STRADA_AUDIO_OUTPUT chooses a windowed application's output, and headless runs stay on the null device")
{
	CHECK(std::string(AudioOutputVariable) == "STRADA_AUDIO_OUTPUT");
	CHECK(AudioEngine::ResolveOutput(false, std::nullopt) == AudioOutput::Device);
	CHECK(AudioEngine::ResolveOutput(false, std::string()) == AudioOutput::Device);
	CHECK(AudioEngine::ResolveOutput(false, std::string("device")) == AudioOutput::Device);
	CHECK(AudioEngine::ResolveOutput(false, std::string("null")) == AudioOutput::Null);
	CHECK(AudioEngine::ResolveOutput(false, std::string("NULL")) == AudioOutput::Null);
	CHECK(AudioEngine::ResolveOutput(false, std::string("Device")) == AudioOutput::Device);
	// An unknown value keeps the device (and is logged).
	CHECK(AudioEngine::ResolveOutput(false, std::string("speakers")) == AudioOutput::Device);
	// Headless applications never open a device.
	CHECK(AudioEngine::ResolveOutput(true, std::nullopt) == AudioOutput::Null);
	CHECK(AudioEngine::ResolveOutput(true, std::string("device")) == AudioOutput::Null);
	CHECK(AudioEngine::ResolveOutput(true, std::string("null")) == AudioOutput::Null);
}

namespace
{
	// Calls update until the condition holds or the time is up (null output mixes on the device's thread, at its pace).
	template<typename TCondition>
	bool UpdateUntil(TCondition&& condition, std::chrono::seconds timeout)
	{
		auto const deadline = std::chrono::steady_clock::now() + timeout;
		while (!condition())
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			AudioEngine::Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return true;
	}

	void InitNullOutput()
	{
		AudioEngineSettings settings;
		settings.Output = AudioOutput::Null;
		AudioEngine::Init(settings);
		REQUIRE(AudioEngine::IsInitialized());
	}
}

TEST_CASE("AudioEngine: null output mixes while the main thread starts, changes and stops voices")
{
	// The device's thread mixes while this thread plays one-shots on a moving source, changes its voices, steals at the
	// pool and at the limit, and ends a scene: the threads meet only through the queue and the return list.
	InitNullOutput();
	Scope<AudioScene> audio = AudioScene::Create().TakeValue();
	Ref<AudioClipAsset> const shortClip = Testing::MakeWavClip({{0.1f, 0.01f}});
	Ref<AudioClipAsset> const loop = Testing::MakeWavClip({{0.05f, 0.2f}});
	AudioSourceDesc mover;
	mover.Entity = UUID(1);
	mover.Clip = shortClip;
	mover.MaxVoices = 8;
	REQUIRE(audio->AddSource(mover).IsOk());
	for (uint64_t index = 0; index < MaxMixedAudioVoices; index++)
	{
		AudioSourceDesc desc;
		desc.Entity = UUID(100 + index);
		desc.Clip = loop;
		desc.Loop = true;
		desc.Position = {static_cast<float>(index), 0.0f, -1.0f};
		REQUIRE(audio->AddSource(desc).IsOk());
	}

	uint64_t const startBlocks = AudioEngine::GetBlockCount();
	uint32_t frame = 0;
	// About a second of the null device's mix, however slowly the machine runs it.
	bool const mixed = UpdateUntil(
		[&audio, &shortClip, &frame, startBlocks]
		{
			float const phase = static_cast<float>(frame) * 0.05f;
			audio->SetPosition(UUID(1), {std::sin(phase) * 10.0f, 0.0f, std::cos(phase) * 10.0f});
			audio->PlayOneShot(UUID(1), nullptr, 1.0f);
			audio->PlayClipAtPoint(shortClip, {std::cos(phase), 0.0f, -2.0f}, 0.5f, "");
			UUID const looping(100 + frame % MaxMixedAudioVoices);
			if (frame % 3 == 0)
			{
				audio->Stop(looping);
			}
			else
			{
				audio->Play(looping);
			}
			audio->SetVolume(looping, 0.5f + 0.5f * std::sin(phase));
			audio->SetPitch(looping, 0.75f + 0.5f * std::abs(std::cos(phase)));
			audio->SetListener({std::sin(phase), 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
			if (frame % 40 == 20)
			{
				audio->SetPaused(true);
			}
			else if (frame % 40 == 30)
			{
				audio->SetPaused(false);
			}
			AudioEngine::SetMasterVolume(frame % 2 == 0 ? 1.0f : 0.9f);
			frame++;
			return AudioEngine::GetBlockCount() - startBlocks >= 200;
		},
		std::chrono::seconds(30));
	CHECK(mixed);
	CHECK(AudioEngine::GetStolenVoiceCount() > 0);

	// The scene ends: everything it lent comes back once the device's thread has finished with it.
	audio.reset();
	CHECK(UpdateUntil(
		[]
		{
			return AudioEngine::GetReturningObjectCount() == 0;
		},
		std::chrono::seconds(30)));
	AudioEngine::Shutdown();
}

TEST_CASE("AudioEngine: null output mixes through buses and effects while the main thread edits the mixer")
{
	// The device's thread runs the buses' nodes and effects while this thread changes their values, swaps their chains,
	// adds, re-parents and removes buses under playing scenes, sets game values, and ends a scene whose bus goes before
	// its voices have faded: buses, chains and groups cross between the threads only through the queue and the return list.
	InitNullOutput();
	auto const makeMixer = [](uint32_t step)
	{
		MixerEffect filter;
		filter.Kind = step % 2 == 0 ? MixerEffectKind::LowPass : MixerEffectKind::Peaking;
		filter.Frequency = 200.0f + 100.0f * static_cast<float>(step % 10);
		MixerEffect reverb;
		reverb.Kind = MixerEffectKind::Reverb;
		MixerEffect limiter;
		limiter.Kind = MixerEffectKind::Limiter;
		Mixer made;
		made.Buses.resize(3);
		made.Buses[0].Name = "Master";
		made.Buses[0].Effects = {limiter};
		made.Buses[1].Name = "Music";
		made.Buses[1].VolumeDb = -static_cast<float>(step % 12);
		made.Buses[1].Effects = {filter, reverb};
		made.Buses[2].Name = "Effects";
		made.Buses[2].Parent = step % 3 == 0 ? "Music" : "Master";
		made.Buses[2].Muted = step % 5 == 4;
		if (step % 4 != 3)
		{
			MixerBus voices;
			voices.Name = "Voices";
			voices.Parent = "Effects";
			made.Buses.push_back(voices);
		}
		return made;
	};
	AudioEngine::SetMixer(makeMixer(0));
	Ref<AudioClipAsset> const loop = Testing::MakeWavClip({{0.05f, 0.2f}});
	auto const makeScene = [&loop]
	{
		Scope<AudioScene> scene = AudioScene::Create().TakeValue();
		uint64_t index = 0;
		for (char const* bus : {"Music", "Effects", "Voices", ""})
		{
			AudioSourceDesc desc;
			desc.Entity = UUID(300 + index++);
			desc.Clip = loop;
			desc.Loop = true;
			desc.Bus = bus;
			REQUIRE(scene->AddSource(desc).IsOk());
			scene->Play(desc.Entity);
		}
		return scene;
	};
	Scope<AudioScene> audio = makeScene();
	Scope<AudioScene> ending = makeScene();

	uint64_t const startBlocks = AudioEngine::GetBlockCount();
	uint32_t step = 0;
	bool const mixed = UpdateUntil(
		[&]
		{
			AudioEngine::SetMixer(makeMixer(step));
			AudioEngine::SetBusVolume("Music", -static_cast<float>(step % 6));
			AudioEngine::SetBusMuted("Effects", step % 7 == 0);
			audio->SetBus(UUID(300), step % 2 == 0 ? "Music" : "Voices");
			if (step % 30 == 15)
			{
				AudioEngine::ClearEffectState();
			}
			if (step == 40)
			{
				// The scene ends while it is heard, and Voices goes with the next edit.
				ending.reset();
			}
			step++;
			return step > 40 && AudioEngine::GetBlockCount() - startBlocks >= 200;
		},
		std::chrono::seconds(30));
	CHECK(mixed);

	audio.reset();
	CHECK(UpdateUntil(
		[]
		{
			return AudioEngine::GetReturningObjectCount() == 0;
		},
		std::chrono::seconds(30)));
	AudioEngine::Shutdown();
}

TEST_CASE("AudioEngine: a device that stops is taken over by the main thread until it runs again")
{
	InitNullOutput();
	Scope<AudioScene> audio = AudioScene::Create().TakeValue();
	AudioSourceDesc desc;
	desc.Entity = UUID(1);
	desc.Clip = Testing::MakeWavClip({{0.1f, 1.0f}});
	desc.Loop = true;
	REQUIRE(audio->AddSource(desc).IsOk());
	audio->Play(UUID(1));
	CHECK(audio->PlayOneShot(UUID(1), nullptr, 1.0f));
	CHECK_FALSE(AudioEngine::IsMixerTakenOver());

	// The device goes away: no block runs, so nothing lent would ever come back, unless the main thread takes over.
	AudioEngine::SimulateDeviceLoss();
	AudioEngine::Update();
	CHECK(AudioEngine::IsMixerTakenOver());
	audio->Stop(UUID(1));
	audio.reset();
	AudioEngine::Update();
	CHECK(AudioEngine::GetReturningObjectCount() == 0);

	// Once a retry starts the device again, it mixes again.
	CHECK(UpdateUntil(
		[]
		{
			return !AudioEngine::IsMixerTakenOver();
		},
		std::chrono::seconds(10)));
	uint64_t const blocks = AudioEngine::GetBlockCount();
	CHECK(UpdateUntil(
		[blocks]
		{
			return AudioEngine::GetBlockCount() > blocks;
		},
		std::chrono::seconds(10)));
	AudioEngine::Shutdown();
}

TEST_CASE("AudioEngine: the mixing-thread hook may be asked from any thread while the engine starts and stops")
{
	// What a replaced operator new does on every thread: it reads only static storage, so it races with nothing.
	std::atomic<bool> stop{false};
	std::atomic<bool> sawMixing{false};
	std::atomic<uint64_t> asked{0};
	std::thread asker(
		[&stop, &sawMixing, &asked]
		{
			while (!stop.load(std::memory_order_acquire))
			{
				if (AudioEngine::IsMixingOnThisThread())
				{
					sawMixing = true;
				}
				static_cast<void>(AudioEngine::GetMixAllocationCount());
				asked.fetch_add(1, std::memory_order_relaxed);
			}
		});
	for (int round = 0; round < 3; round++)
	{
		InitNullOutput();
		uint64_t const blocks = AudioEngine::GetBlockCount();
		CHECK(UpdateUntil(
			[blocks]
			{
				return AudioEngine::GetBlockCount() > blocks;
			},
			std::chrono::seconds(10)));
		AudioEngine::Shutdown();
	}
	stop.store(true, std::memory_order_release);
	asker.join();
	CHECK(asked > 0);
	CHECK_FALSE(sawMixing);
	CHECK_FALSE(AudioEngine::IsMixingOnThisThread());
}
