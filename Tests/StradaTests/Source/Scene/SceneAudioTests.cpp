#include "Audio/AudioTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

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
