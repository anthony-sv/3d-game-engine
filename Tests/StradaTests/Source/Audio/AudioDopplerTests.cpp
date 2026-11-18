#include "Audio/AudioTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Audio/AudioTypes.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <vector>

// Doppler computed by the engine (Docs/Architecture.md §9.8): the factor and the velocities it reads, and what a running
// scene that is never drawn makes of its sources' and its listener's moves, measured in the mix.

using namespace Strada;

namespace
{
	constexpr float FrameTime = 1.0f / 60.0f;
	constexpr double ToneFrequency = 1000.0;
	constexpr float Speed = 30.0f;

	// c / (c - v) at the default speed of sound.
	double ExpectedFactor(double closingSpeed)
	{
		return static_cast<double>(DefaultSpeedOfSound) / (static_cast<double>(DefaultSpeedOfSound) - closingSpeed);
	}

	// |measured / expected - 1|: what "within 1 %" bounds. doctest::Approx's epsilon is relative to 1 plus the larger value
	// (doctest.h:6556), about twice the bound for factors near 1.
	double RelativeError(double measured, double expected)
	{
		return std::abs(measured / expected - 1.0);
	}

	// One second of a 1 kHz sine at the test rate: a whole number of cycles, so that it loops without a seam.
	AssetHandle AddToneClip()
	{
		std::vector<float> samples(Testing::AudioSampleRate);
		for (size_t index = 0; index < samples.size(); index++)
		{
			double const phase =
				2.0 * glm::pi<double>() * ToneFrequency * static_cast<double>(index) / static_cast<double>(Testing::AudioSampleRate);
			samples[index] = static_cast<float>(0.5 * std::sin(phase));
		}
		return AssetManager::AddMemoryAsset(Testing::MakeClip(Testing::MakeWav(samples)), "Tone");
	}

	// The left channel's frequency in hertz, from its rising zero crossings placed between samples by linear interpolation.
	double MeasureFrequency(std::span<float const> samples)
	{
		std::optional<double> first;
		double last = 0.0;
		uint32_t crossings = 0;
		for (size_t frame = 1; frame < samples.size() / 2; frame++)
		{
			float const before = samples[(frame - 1) * 2];
			float const after = samples[frame * 2];
			if (before < 0.0f && after >= 0.0f)
			{
				double const time = static_cast<double>(frame - 1) + static_cast<double>(before) / static_cast<double>(before - after);
				if (!first)
				{
					first = time;
				}
				last = time;
				crossings++;
			}
		}
		REQUIRE(crossings > 2);
		return static_cast<double>(crossings - 1) * static_cast<double>(Testing::AudioSampleRate) / (last - *first);
	}

	// A running scene heard by a listener at the origin facing -Z, with a looping 1 kHz tone at full volume within 500 m.
	struct DopplerScene
	{
		Testing::AudioEngineScope Engine;
		Testing::AssetManagerScope Assets;
		Scene World{"Doppler"};
		Entity Listener;
		Entity Source;

		explicit DopplerScene(glm::vec3 const& sourcePosition)
		{
			Listener = World.CreateEntity("Listener");
			Listener.AddComponent<AudioListenerComponent>();
			Source = World.CreateEntity("Source");
			Source.GetComponent<TransformComponent>().Translation = sourcePosition;
			AudioSourceComponent& source = Source.AddComponent<AudioSourceComponent>();
			source.Clip = AddToneClip();
			source.Loop = true;
			source.PlayOnStart = true;
			source.MinDistance = 500.0f;
			source.MaxDistance = 1000.0f;
		}

		AudioScene& GetAudio()
		{
			AudioScene* const audio = World.GetAudioScene();
			REQUIRE(audio != nullptr);
			return *audio;
		}

		float GetDoppler() { return GetAudio().GetDoppler(Source.GetUUID()); }

		// Runs frames of FrameTime, calling move(frame) before each update, and reads each frame's audio after it, as a
		// player that records its mix does. Returns the frames of audio read.
		std::vector<float> Run(uint32_t frames, std::function<void(uint32_t)> const& move)
		{
			std::vector<float> samples;
			for (uint32_t frame = 0; frame < frames; frame++)
			{
				move(frame);
				World.OnUpdateRuntime(Timestep(FrameTime));
				std::vector<float> const part = Testing::ReadSamples(FrameTime);
				samples.insert(samples.end(), part.begin(), part.end());
			}
			return samples;
		}
	};

	// Moves an entity along +Z at Speed from its translation at frame 0.
	std::function<void(uint32_t)> MoveAlongZ(Entity entity, glm::vec3 const& start)
	{
		return [entity, start](uint32_t frame) mutable
		{
			entity.GetComponent<TransformComponent>().Translation =
				start + glm::vec3(0.0f, 0.0f, Speed * FrameTime * static_cast<float>(frame + 1));
		};
	}

	// The samples of frames [first, first + count) of what Run returned.
	std::span<float const> Frames(std::vector<float> const& samples, uint32_t first, uint32_t count)
	{
		uint32_t const framesPerUpdate = Testing::AudioFrames(FrameTime);
		return std::span<float const>(samples).subspan(static_cast<size_t>(first) * framesPerUpdate * 2,
		                                               static_cast<size_t>(count) * framesPerUpdate * 2);
	}
}

TEST_CASE("AudioDoppler: the factor is c / (c - v) of the closing speed, kept within an octave, and none without a line or a level")
{
	glm::vec3 const listener(0.0f);
	glm::vec3 const still(0.0f);
	float const c = DefaultSpeedOfSound;

	// A source in front, coming closer, going away; the listener coming closer; a source moving across the line.
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, 30.0f}, listener, still, 1.0f, c) ==
	      doctest::Approx(ExpectedFactor(30.0)).epsilon(1e-5));
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, -30.0f}, listener, still, 1.0f, c) ==
	      doctest::Approx(ExpectedFactor(-30.0)).epsilon(1e-5));
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, still, listener, {0.0f, 0.0f, -30.0f}, 1.0f, c) ==
	      doctest::Approx(ExpectedFactor(30.0)).epsilon(1e-5));
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, {30.0f, 0.0f, 0.0f}, listener, still, 1.0f, c) == doctest::Approx(1.0f));
	// The same motion of both: nothing closes in.
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, 30.0f}, listener, {0.0f, 0.0f, 30.0f}, 1.0f, c) == doctest::Approx(1.0f));
	// The level scales the closing speed; 0 turns it off.
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, 30.0f}, listener, still, 2.0f, c) ==
	      doctest::Approx(ExpectedFactor(60.0)).epsilon(1e-5));
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, 30.0f}, listener, still, 0.0f, c) == 1.0f);
	// Within an octave either way, also at and past the speed of sound.
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, 200.0f}, listener, still, 1.0f, c) == MaxAudioDopplerFactor);
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, 343.0f}, listener, still, 5.0f, c) == MaxAudioDopplerFactor);
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, -400.0f}, listener, still, 1.0f, c) == MinAudioDopplerFactor);
	// No line between source and listener, and values that are not numbers, give none.
	CHECK(ComputeDopplerFactor(listener, {0.0f, 0.0f, 30.0f}, listener, still, 1.0f, c) == 1.0f);
	float const nan = std::numeric_limits<float>::quiet_NaN();
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, nan}, listener, still, 1.0f, c) == 1.0f);
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, 30.0f}, listener, still, nan, c) == 1.0f);
	CHECK(ComputeDopplerFactor({0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, 30.0f}, listener, still, 1.0f, 0.0f) == 1.0f);

	// Velocities: a move over game time; none without time, and none for a move faster than half the speed of sound.
	std::optional<glm::vec3> const velocity = ComputeAudioVelocity({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.5f}, FrameTime, c);
	REQUIRE(velocity);
	CHECK(velocity->z == doctest::Approx(30.0f));
	CHECK_FALSE(ComputeAudioVelocity({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.5f}, 0.0f, c));
	CHECK_FALSE(ComputeAudioVelocity({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.5f}, -FrameTime, c));
	CHECK_FALSE(ComputeAudioVelocity({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.5f}, nan, c));
	CHECK(ComputeAudioVelocity({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.5f * c * FrameTime * 0.999f}, FrameTime, c));
	CHECK_FALSE(ComputeAudioVelocity({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.5f * c * FrameTime * 1.001f}, FrameTime, c));
	CHECK_FALSE(ComputeAudioVelocity({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, nan}, FrameTime, c));
}

TEST_CASE("AudioDoppler: a source passing at 30 m/s is heard within 1 % of c / (c - v), approaching and receding")
{
	// The scene is never drawn, so the transform cache is never refreshed: doppler reads the scene's own updates.
	glm::vec3 const start(0.0f, 0.0f, -60.0f);
	DopplerScene scene(start);
	scene.World.OnRuntimeStart();
	CHECK(scene.GetDoppler() == 1.0f);

	// 3 s along +Z through the listener: in front and closing for 2 s, behind and parting after.
	std::vector<float> const samples = scene.Run(180, MoveAlongZ(scene.Source, start));
	double const approaching = MeasureFrequency(Frames(samples, 15, 45)) / ToneFrequency;
	double const receding = MeasureFrequency(Frames(samples, 150, 30)) / ToneFrequency;
	CHECK(RelativeError(approaching, ExpectedFactor(Speed)) < 0.01);
	CHECK(RelativeError(receding, ExpectedFactor(-Speed)) < 0.01);
	// And not within 1 % of the tone itself: the difference is what is measured.
	CHECK(std::abs(approaching - 1.0) > 0.05);
	CHECK(std::abs(receding - 1.0) > 0.05);
	CHECK(scene.GetDoppler() == doctest::Approx(ExpectedFactor(-Speed)).epsilon(1e-4));
	scene.World.OnRuntimeStop();
}

TEST_CASE("AudioDoppler: a listener moving at 30 m/s hears c / (c - v), and so do the sounds a scene plays at a point")
{
	SUBCASE("a source")
	{
		DopplerScene scene(glm::vec3(0.0f, 0.0f, -200.0f));
		scene.World.OnRuntimeStart();
		// The listener walks along +Z, away from the source in front of it.
		std::vector<float> const away = scene.Run(60, MoveAlongZ(scene.Listener, glm::vec3(0.0f, 0.0f, 60.0f)));
		CHECK(scene.GetDoppler() == doctest::Approx(ExpectedFactor(-Speed)).epsilon(1e-4));
		CHECK(RelativeError(MeasureFrequency(Frames(away, 15, 45)) / ToneFrequency, ExpectedFactor(-Speed)) < 0.01);
	}
	SUBCASE("a sound played at a point stands still and takes the listener's motion")
	{
		DopplerScene scene(glm::vec3(0.0f, 0.0f, 0.0f));
		scene.Source.GetComponent<AudioSourceComponent>().PlayOnStart = false;
		scene.World.OnRuntimeStart();
		glm::vec3 const start(0.0f, 0.0f, 100.0f);
		std::function<void(uint32_t)> const walk = [&scene, start](uint32_t frame)
		{
			// Toward a point at z = -100 (along -Z).
			scene.Listener.GetComponent<TransformComponent>().Translation =
				start - glm::vec3(0.0f, 0.0f, Speed * FrameTime * static_cast<float>(frame + 1));
		};
		scene.Run(2, walk);
		AssetHandle const clip = scene.Source.GetComponent<AudioSourceComponent>().Clip;
		REQUIRE(scene.World.PlayAudioClipAtPoint(clip, glm::vec3(0.0f, 0.0f, -100.0f), 1.0f, ""));
		std::vector<float> const heard = scene.Run(40,
		                                           [&walk](uint32_t frame)
		                                           {
													   walk(frame + 2);
												   });
		// The point's 1 s clip at the default 1 m: quiet, but its frequency is what is read.
		CHECK(RelativeError(MeasureFrequency(Frames(heard, 10, 30)) / ToneFrequency, ExpectedFactor(Speed)) < 0.01);
	}
}

TEST_CASE("AudioDoppler: a source's first update, a teleport, a listener that changes entity, level 0 and a sound that is not "
          "spatial give none")
{
	glm::vec3 const start(0.0f, 0.0f, -100.0f);
	DopplerScene scene(start);
	scene.World.OnRuntimeStart();
	AudioScene& audio = scene.GetAudio();
	TransformComponent& transform = scene.Source.GetComponent<TransformComponent>();
	auto const step = [&scene, &transform](glm::vec3 const& delta)
	{
		transform.Translation += delta;
		scene.World.OnUpdateRuntime(Timestep(FrameTime));
	};
	glm::vec3 const closer(0.0f, 0.0f, Speed * FrameTime);
	float const coming = static_cast<float>(ExpectedFactor(Speed));

	step(closer);
	CHECK(audio.GetDoppler(scene.Source.GetUUID()) == doctest::Approx(coming).epsilon(1e-4));

	SUBCASE("a teleport, then the motion again")
	{
		step(glm::vec3(0.0f, 0.0f, 40.0f));
		CHECK(scene.GetDoppler() == 1.0f);
		step(closer);
		CHECK(scene.GetDoppler() == doctest::Approx(coming).epsilon(1e-4));
	}
	SUBCASE("a scene paused, moved and resumed")
	{
		scene.World.SetPaused(true);
		transform.Translation += glm::vec3(0.0f, 0.0f, 2.0f);
		scene.World.OnUpdateRuntime(Timestep(FrameTime));
		scene.World.SetPaused(false);
		step(closer);
		CHECK(scene.GetDoppler() == 1.0f);
		step(closer);
		CHECK(scene.GetDoppler() == doctest::Approx(coming).epsilon(1e-4));
	}
	SUBCASE("a game whose time stands still")
	{
		scene.World.SetTimeScale(0.0f);
		step(closer);
		CHECK(scene.GetDoppler() == 1.0f);
	}
	SUBCASE("level 0, heard at the clip's own pitch")
	{
		scene.Source.GetComponent<AudioSourceComponent>().DopplerLevel = 0.0f;
		step(closer);
		CHECK(audio.GetDopplerLevel(scene.Source.GetUUID()) == 0.0f);
		CHECK(scene.GetDoppler() == 1.0f);
		std::vector<float> const samples = scene.Run(30,
		                                             [&transform, closer](uint32_t)
		                                             {
														 transform.Translation += closer;
													 });
		CHECK(RelativeError(MeasureFrequency(Frames(samples, 6, 24)) / ToneFrequency, 1.0) < 0.002);
	}
	SUBCASE("twice the level, twice the closing speed")
	{
		scene.Source.GetComponent<AudioSourceComponent>().DopplerLevel = 2.0f;
		step(closer);
		CHECK(scene.GetDoppler() == doctest::Approx(ExpectedFactor(2.0 * Speed)).epsilon(1e-4));
	}
	SUBCASE("a sound that is not spatial, then spatial again from its first update")
	{
		scene.Source.GetComponent<AudioSourceComponent>().Spatial = false;
		step(closer);
		CHECK(scene.GetDoppler() == 1.0f);
		scene.Source.GetComponent<AudioSourceComponent>().Spatial = true;
		step(closer);
		CHECK(scene.GetDoppler() == 1.0f);
		step(closer);
		CHECK(scene.GetDoppler() == doctest::Approx(coming).epsilon(1e-4));
	}
	SUBCASE("a listener that changes entity, a few meters away")
	{
		Entity second = scene.World.CreateEntity("Second listener");
		second.GetComponent<TransformComponent>().Translation = glm::vec3(2.0f, 0.0f, 0.0f);
		second.AddComponent<AudioListenerComponent>();
		scene.Listener.GetComponent<AudioListenerComponent>().Active = false;
		step(closer);
		CHECK(scene.GetDoppler() == 1.0f);
		step(closer);
		CHECK(scene.GetDoppler() == doctest::Approx(coming).epsilon(1e-3));
	}
	SUBCASE("a speed of sound the mixer sets")
	{
		Mixer mixer;
		mixer.SpeedOfSound = 100.0f;
		AudioEngine::SetMixer(mixer);
		step(closer);
		CHECK(scene.GetDoppler() == doctest::Approx(100.0f / (100.0f - Speed)).epsilon(1e-4));
	}
	scene.World.OnRuntimeStop();
}

TEST_CASE("AudioDoppler: a source's one-shots play at its factor, and its pitch reads as it was set")
{
	glm::vec3 const start(0.0f, 0.0f, -100.0f);
	DopplerScene scene(start);
	AudioSourceComponent& component = scene.Source.GetComponent<AudioSourceComponent>();
	component.PlayOnStart = false;
	component.Pitch = 1.25f;
	scene.World.OnRuntimeStart();
	scene.Run(2, MoveAlongZ(scene.Source, start));
	REQUIRE(scene.World.PlayAudioOneShot(scene.Source, AssetHandle(), 1.0f));
	std::vector<float> const samples = scene.Run(40,
	                                             [&scene, start](uint32_t frame)
	                                             {
													 MoveAlongZ(scene.Source, start)(frame + 2);
												 });
	CHECK(scene.GetAudio().GetPitch(scene.Source.GetUUID()) == 1.25f);
	CHECK(RelativeError(MeasureFrequency(Frames(samples, 10, 30)) / ToneFrequency, 1.25 * ExpectedFactor(Speed)) < 0.01);
	scene.World.OnRuntimeStop();
}
