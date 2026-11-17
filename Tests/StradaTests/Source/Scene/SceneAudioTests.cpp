#include "Audio/AudioTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Audio/AudioTypes.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

using namespace Strada;

namespace
{
	constexpr float FrameTime = 1.0f / 60.0f;

	Entity AddSource(Scene& scene, std::string const& name, AssetHandle clip, bool playOnStart)
	{
		Entity entity = scene.CreateEntity(name);
		AudioSourceComponent& source = entity.AddComponent<AudioSourceComponent>();
		source.Clip = clip;
		source.PlayOnStart = playOnStart;
		source.Spatial = false;
		return entity;
	}
}

TEST_CASE("Scene: the runtime plays audio sources and follows their changes")
{
	Testing::AudioEngineScope engine;
	Testing::AssetManagerScope assets;
	AssetHandle const clip = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.5f, 1.0f}}), "Tone");
	AssetHandle const otherClip = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.25f, 1.0f}}), "Other tone");

	Scene scene("Audio");
	Entity music = AddSource(scene, "Music", clip, true);
	Entity idle = AddSource(scene, "Idle", clip, false);
	Entity silent = AddSource(scene, "Silent", AssetHandle(), true);

	scene.OnRuntimeStart();
	AudioScene* audio = scene.GetAudioScene();
	REQUIRE(audio != nullptr);
	CHECK(audio->GetSourceCount() == 2);
	CHECK(audio->IsPlaying(music.GetUUID()));
	CHECK_FALSE(audio->IsPlaying(idle.GetUUID()));
	CHECK_FALSE(audio->HasSource(silent.GetUUID()));

	// Field changes reach the playing sound.
	AudioSourceComponent& source = music.GetComponent<AudioSourceComponent>();
	source.Volume = 0.5f;
	source.Pitch = 1.25f;
	source.Loop = true;
	source.Spatial = true;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(audio->GetVolume(music.GetUUID()) == doctest::Approx(0.5f));
	CHECK(audio->GetPitch(music.GetUUID()) == doctest::Approx(1.25f));
	CHECK(audio->IsLooping(music.GetUUID()));
	CHECK(audio->IsSpatial(music.GetUUID()));
	CHECK(audio->IsPlaying(music.GetUUID()));

	// A new clip makes a new sound, stopped until played, with the current settings.
	source.Clip = otherClip;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	REQUIRE(audio->HasSource(music.GetUUID()));
	CHECK_FALSE(audio->IsPlaying(music.GetUUID()));
	CHECK(audio->GetVolume(music.GetUUID()) == doctest::Approx(0.5f));

	// Sources added while running start like the others; removed components and destroyed entities lose their sound.
	Entity spawned = AddSource(scene, "Spawned", clip, true);
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(audio->IsPlaying(spawned.GetUUID()));
	UUID const spawnedId = spawned.GetUUID();
	scene.DestroyEntity(spawned);
	idle.RemoveComponent<AudioSourceComponent>();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK_FALSE(audio->HasSource(spawnedId));
	CHECK_FALSE(audio->HasSource(idle.GetUUID()));
	CHECK(audio->GetSourceCount() == 1);

	// An asset that is not an audio clip plays nothing.
	silent.GetComponent<AudioSourceComponent>().Clip = GetBuiltInHandle(BuiltInAsset::CubeMesh);
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK_FALSE(audio->HasSource(silent.GetUUID()));

	// Pausing the scene pauses its sounds.
	scene.SetPaused(true);
	CHECK(audio->IsPaused());
	scene.SetPaused(false);
	CHECK_FALSE(audio->IsPaused());

	scene.OnRuntimeStop();
	CHECK(scene.GetAudioScene() == nullptr);
}

TEST_CASE("Scene: streamed audio sources read their clip's file without loading the clip")
{
	Testing::AudioEngineScope engine;
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory directory;
	Buffer const tone = Testing::MakeWav(Testing::MakeSamples({{0.5f, 1.0f}}));
	REQUIRE(FileSystem::WriteBinaryFile(directory.GetPath() / "Music.wav", tone.GetSpan()).IsOk());
	REQUIRE(FileSystem::WriteBinaryFile(directory.GetPath() / "Gone.wav", tone.GetSpan()).IsOk());
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());
	AssetHandle const music = AssetManager::FindByPath("Music.wav");
	AssetHandle const gone = AssetManager::FindByPath("Gone.wav");
	REQUIRE(music.IsValid());
	REQUIRE(gone.IsValid());
	REQUIRE(FileSystem::Remove(directory.GetPath() / "Gone.wav").IsOk());
	AssetHandle const memoryClip = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.5f, 1.0f}}), "Tone");

	Scene scene("Streams");
	Entity entity = AddSource(scene, "Music", music, true);
	AudioSourceComponent& source = entity.GetComponent<AudioSourceComponent>();
	source.Stream = true;
	scene.OnRuntimeStart();
	AudioScene* audio = scene.GetAudioScene();
	REQUIRE(audio != nullptr);
	CHECK(audio->IsStreaming(entity.GetUUID()));
	CHECK(audio->IsPlaying(entity.GetUUID()));
	CHECK(Testing::ReadLevels(0.1f).Left == doctest::Approx(0.5f).epsilon(0.01));
	CHECK_FALSE(AssetManager::IsLoaded(music));

	// Loading the clip instead (or streaming it again) makes a new sound, stopped until played.
	source.Stream = false;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK_FALSE(audio->IsStreaming(entity.GetUUID()));
	CHECK_FALSE(audio->IsPlaying(entity.GetUUID()));
	CHECK(AssetManager::IsLoaded(music));
	source.Stream = true;
	CHECK_FALSE(scene.IsAudioSourcePlaying(entity));
	CHECK(audio->IsStreaming(entity.GetUUID()));
	scene.PlayAudioSource(entity);
	CHECK(scene.IsAudioSourcePlaying(entity));

	// Only file assets stream: missing files and memory assets play nothing.
	source.Clip = gone;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK_FALSE(audio->HasSource(entity.GetUUID()));
	source.Clip = memoryClip;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK_FALSE(audio->HasSource(entity.GetUUID()));
	source.Stream = false;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(audio->HasSource(entity.GetUUID()));
	CHECK_FALSE(audio->IsStreaming(entity.GetUUID()));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: a streamed source's one-shots play its clip loaded, and a source without a clip plays the clips it is given")
{
	Testing::AudioEngineScope engine;
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory directory;
	REQUIRE(FileSystem::WriteBinaryFile(directory.GetPath() / "Music.wav", Testing::MakeWav(Testing::MakeSamples({{0.5f, 1.0f}})).GetSpan())
	            .IsOk());
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());
	AssetHandle const music = AssetManager::FindByPath("Music.wav");
	REQUIRE(music.IsValid());
	AssetHandle const step = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.25f, 1.0f}}), "Step");
	AssetHandle const tone = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.5f, 1.0f}}), "Tone");

	Scene scene("One-shots");
	Entity streamed = AddSource(scene, "Music", music, false);
	streamed.GetComponent<AudioSourceComponent>().Stream = true;
	Entity feet = AddSource(scene, "Feet", AssetHandle(), false);
	scene.OnRuntimeStart();
	AudioScene* audio = scene.GetAudioScene();
	REQUIRE(audio != nullptr);
	REQUIRE(audio->IsStreaming(streamed.GetUUID()));
	CHECK_FALSE(AssetManager::IsLoaded(music));

	// The streamed source's own one-shot plays its clip, loaded for it.
	CHECK(scene.PlayAudioOneShot(streamed, AssetHandle(), 1.0f));
	CHECK(AssetManager::IsLoaded(music));
	CHECK_FALSE(scene.IsAudioSourcePlaying(streamed));
	Testing::Skip(0.01f);
	CHECK(Testing::ReadLevels(0.05f).Left == doctest::Approx(0.5f).epsilon(0.01));
	scene.StopAudioSource(streamed);
	Testing::Skip(0.02f);

	// A source without a clip has no sound of its own until a one-shot names a clip: it is then a pool of one-shots
	// alone, which follows its component.
	CHECK_FALSE(scene.PlayAudioOneShot(feet, AssetHandle(), 1.0f));
	CHECK_FALSE(audio->HasSource(feet.GetUUID()));
	CHECK(scene.PlayAudioOneShot(feet, step, 1.0f));
	REQUIRE(audio->HasSource(feet.GetUUID()));
	CHECK_FALSE(scene.IsAudioSourcePlaying(feet));
	CHECK(audio->GetVoiceCount(feet.GetUUID()) == 1);
	CHECK_FALSE(scene.PlayAudioOneShot(feet, AssetHandle(), 1.0f));
	CHECK(audio->GetVoiceCount(feet.GetUUID()) == 1);
	Testing::Skip(0.01f);
	CHECK(Testing::ReadLevels(0.05f).Left == doctest::Approx(0.25f).epsilon(0.01));
	AudioSourceComponent& source = feet.GetComponent<AudioSourceComponent>();
	source.Volume = 0.5f;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(audio->GetVolume(feet.GetUUID()) == doctest::Approx(0.5f));
	Testing::Skip(0.01f);
	CHECK(Testing::ReadLevels(0.05f).Left == doctest::Approx(0.125f).epsilon(0.01));
	scene.PlayAudioSource(feet);
	CHECK_FALSE(scene.IsAudioSourcePlaying(feet));
	scene.StopAudioSource(feet);
	Testing::Skip(0.02f);
	CHECK(Testing::ReadLevels(0.02f).Left == 0.0f);

	// Given a clip, it is a source like any other, stopped until played; without the component it has no sound.
	source.Clip = tone;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	REQUIRE(audio->HasSource(feet.GetUUID()));
	CHECK_FALSE(scene.IsAudioSourcePlaying(feet));
	CHECK(scene.PlayAudioOneShot(feet, AssetHandle(), 1.0f));
	feet.RemoveComponent<AudioSourceComponent>();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK_FALSE(audio->HasSource(feet.GetUUID()));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: audio is heard from the active listener or the primary camera")
{
	Testing::AudioEngineScope engine;
	Testing::AssetManagerScope assets;
	AssetHandle const clip = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.5f, 1.0f}}), "Tone");

	Scene scene("Listener");
	Entity holder = scene.CreateEntity("Holder");
	holder.GetComponent<TransformComponent>().Translation = {10.0f, 0.0f, 0.0f};
	Entity speaker = scene.CreateEntity("Speaker", holder);
	speaker.GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, 0.0f};
	AudioSourceComponent& source = speaker.AddComponent<AudioSourceComponent>();
	source.Clip = clip;
	source.Loop = true;
	// Unattenuated anywhere in the test, so the levels only show the panning.
	source.MinDistance = 100.0f;
	source.MaxDistance = 200.0f;
	Entity camera = scene.CreateEntity("Camera");
	camera.AddComponent<CameraComponent>().Primary = true;
	camera.GetComponent<TransformComponent>().Translation = {0.0f, 0.0f, 5.0f};

	// Sources take their world positions; without a listener the primary camera hears the scene.
	scene.OnRuntimeStart();
	AudioScene* audio = scene.GetAudioScene();
	REQUIRE(audio != nullptr);
	CHECK(audio->GetPosition(speaker.GetUUID()) == glm::vec3(10.0f, 2.0f, 0.0f));
	CHECK(audio->GetListenerPosition() == glm::vec3(0.0f, 0.0f, 5.0f));

	// An active listener takes over; with several, the first in hierarchy order.
	Entity ears = scene.CreateEntity("Ears");
	ears.GetComponent<TransformComponent>().Translation = {1.0f, 2.0f, 3.0f};
	ears.AddComponent<AudioListenerComponent>();
	Entity laterEars = scene.CreateEntity("Later ears");
	laterEars.GetComponent<TransformComponent>().Translation = {-1.0f, 0.0f, 0.0f};
	laterEars.AddComponent<AudioListenerComponent>();
	holder.GetComponent<TransformComponent>().Translation = {20.0f, 0.0f, 0.0f};
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(audio->GetListenerPosition() == glm::vec3(1.0f, 2.0f, 3.0f));
	CHECK(audio->GetPosition(speaker.GetUUID()) == glm::vec3(20.0f, 2.0f, 0.0f));
	ears.GetComponent<AudioListenerComponent>().Active = false;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(audio->GetListenerPosition() == glm::vec3(-1.0f, 0.0f, 0.0f));

	// The listener faces its entity's -Z: the sound (towards +X) is on its right, and on its left once it turns around.
	audio->Play(speaker.GetUUID());
	Testing::Skip(0.03f);
	Testing::AudioLevels levels = Testing::ReadLevels(0.05f);
	CHECK(levels.Right > 3.0f * levels.Left);
	laterEars.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
	scene.OnUpdateRuntime(Timestep(FrameTime));
	Testing::Skip(0.03f);
	levels = Testing::ReadLevels(0.05f);
	CHECK(levels.Left > 3.0f * levels.Right);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: audio sources follow their priority and their number of voices")
{
	Testing::AudioEngineScope engine;
	Testing::AssetManagerScope assets;
	AssetHandle const clip = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.5f, 1.0f}}), "Tone");

	Scene scene("Voices");
	Entity speaker = AddSource(scene, "Speaker", clip, true);
	AudioSourceComponent& source = speaker.GetComponent<AudioSourceComponent>();
	CHECK(source.Priority == DefaultAudioPriority);
	CHECK(source.MaxVoices == DefaultAudioSourceVoices);
	source.Priority = 200;
	source.MaxVoices = 2;
	scene.OnRuntimeStart();
	AudioScene* audio = scene.GetAudioScene();
	REQUIRE(audio != nullptr);
	CHECK(audio->GetPriority(speaker.GetUUID()) == 200);
	CHECK(audio->GetMaxVoices(speaker.GetUUID()) == 2);

	// The main voice and one one-shot fill a pool of two; a smaller pool steals the one-shot.
	CHECK(audio->PlayOneShot(speaker.GetUUID(), nullptr, 1.0f));
	CHECK(audio->GetVoiceCount(speaker.GetUUID()) == 2);
	source.MaxVoices = 1;
	source.Priority = 5;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(audio->GetMaxVoices(speaker.GetUUID()) == 1);
	CHECK(audio->GetPriority(speaker.GetUUID()) == 5);
	CHECK(audio->GetVoiceCount(speaker.GetUUID()) == 1);

	// Values set from code past the fields' ranges are brought within them.
	source.MaxVoices = 1000;
	source.Priority = 1000;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(audio->GetMaxVoices(speaker.GetUUID()) == MaxAudioSourceVoices);
	CHECK(audio->GetPriority(speaker.GetUUID()) == MaxAudioPriority);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: audio sources play on the bus they name, and follow it as it changes")
{
	Testing::AudioEngineScope engine;
	Testing::AssetManagerScope assets;
	AssetHandle const clip = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.5f, 1.0f}}), "Tone");
	Mixer mixer;
	MixerBus quiet;
	quiet.Name = "Quiet";
	quiet.VolumeDb = -12.0f;
	MixerBus silent;
	silent.Name = "Silent";
	silent.Muted = true;
	mixer.Buses.push_back(quiet);
	mixer.Buses.push_back(silent);
	AudioEngine::SetMixer(mixer);

	Scene scene("Buses");
	Entity speaker = AddSource(scene, "Speaker", clip, true);
	AudioSourceComponent& source = speaker.GetComponent<AudioSourceComponent>();
	CHECK(source.Bus.empty());
	source.Bus = "quiet";
	scene.OnRuntimeStart();
	AudioScene* audio = scene.GetAudioScene();
	REQUIRE(audio != nullptr);
	CHECK(audio->GetBus(speaker.GetUUID()) == "quiet");
	float const level = Testing::ReadLevels(0.05f).Left;
	CHECK(20.0f * std::log10(level / 0.5f) == doctest::Approx(-12.0f).epsilon(0.01));

	source.Bus = "Silent";
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(audio->GetBus(speaker.GetUUID()) == "Silent");
	Testing::Skip(0.02f);
	CHECK(Testing::ReadLevels(0.05f).Left == 0.0f);
	source.Bus = "";
	scene.OnUpdateRuntime(Timestep(FrameTime));
	Testing::Skip(0.02f);
	CHECK(Testing::ReadLevels(0.05f).Left == doctest::Approx(0.5f).epsilon(0.001));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: without the audio engine the runtime runs without audio")
{
	REQUIRE_FALSE(AudioEngine::IsInitialized());
	Scene scene("Silent");
	scene.CreateEntity("Speaker").AddComponent<AudioSourceComponent>().PlayOnStart = true;
	scene.OnRuntimeStart();
	CHECK(scene.GetAudioScene() == nullptr);
	scene.OnUpdateRuntime(Timestep(FrameTime));
	scene.SetPaused(true);
	scene.OnRuntimeStop();
}
