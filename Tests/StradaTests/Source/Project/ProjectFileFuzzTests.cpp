#include "Fuzzing.h"
#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ProjectSettings.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	// Whatever identity a damaged file gives names a game data folder strictly below the per-user root on every platform,
	// in one or two portable names, never the engine's own folder.
	void CheckGameDataFolder(ProjectSettings const& settings)
	{
		GameIdentity const identity = GetGameIdentity(settings);
		std::filesystem::path const root = FileSystem::PathFromUtf8("/root");
		for (UserDataPlatform const platform : {UserDataPlatform::Windows, UserDataPlatform::MacOS, UserDataPlatform::Linux})
		{
			std::filesystem::path const relative = UserDataLocations::MakeGameDirectory(root, identity, platform).lexically_relative(root);
			size_t const names = static_cast<size_t>(std::distance(relative.begin(), relative.end()));
			CHECK(names >= 1);
			CHECK(names <= 2);
			for (std::filesystem::path const& part : relative)
			{
				std::string const name = FileSystem::PathToUtf8(part);
				CHECK_FALSE(name.empty());
				CHECK(name == FileSystem::MakePortableFileName(name));
			}
			CHECK(FileSystem::PathToUtf8(relative) != "Strada");
			CHECK(FileSystem::PathToUtf8(relative) != "strada");
		}
	}
}

TEST_CASE("Project: damaged project files are refused, or load, save again and run scenes")
{
	Testing::PhysicsSystemScope physics;
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);
	Json const original = Testing::LoadFeatureTestDocument("FeatureTest.sproj");
	REQUIRE(Project::Deserialize(original, context).IsOk());

	Testing::DocumentMutator mutator(5000);
	int loaded = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		Result<ProjectSettings> settings = Project::Deserialize(mutator.Damage(original, round), context);
		if (!settings)
		{
			refused++;
			continue;
		}
		loaded++;
		Result<ProjectSettings> reloaded = Project::Deserialize(Project::Serialize(settings.GetValue()), context);
		REQUIRE_MESSAGE(reloaded.IsOk(), (reloaded ? std::string() : reloaded.GetError()));
		CHECK(reloaded.GetValue() == settings.GetValue());
		CheckGameDataFolder(settings.GetValue());

		// Its runtime settings (physics only) simulate a scene: a ball falling onto the ground.
		Scene scene("Project");
		Entity ground = scene.CreateEntity("Ground");
		ground.AddComponent<BoxColliderComponent>().HalfExtents = {5.0f, 0.5f, 5.0f};
		Entity ball = scene.CreateEntity("Ball");
		ball.GetComponent<TransformComponent>().Translation = {0.0f, 3.0f, 0.0f};
		ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		ball.AddComponent<SphereColliderComponent>();
		SceneRuntimeSettings runtime = MakeSceneRuntimeSettings(settings.GetValue());
		runtime.RunScripts = false;
		runtime.PlayAudio = false;
		scene.OnRuntimeStart(runtime);
		for (int frame = 0; frame < 5; frame++)
		{
			scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
		}
		// A long frame runs as many steps as the settings allow, each in their collision steps.
		uint64_t const steps = scene.GetFixedStepCount();
		scene.OnUpdateRuntime(Timestep(0.25f));
		CHECK(scene.GetFixedStepCount() - steps <= settings.GetValue().Physics.MaxStepsPerFrame);
		CHECK(scene.GetFixedStepCount() > steps);
		scene.OnRuntimeStop();
	}
	CHECK(loaded > 0);
	CHECK(refused > 0);
}

TEST_CASE("Project: damaged game files are refused or open, and their window settings are always usable")
{
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);
	Result<ProjectSettings> const project = Project::Deserialize(Testing::LoadFeatureTestDocument("FeatureTest.sproj"), context);
	REQUIRE(project.IsOk());
	Json const original = Project::Serialize(project.GetValue(), ProjectFileKind::Game);

	Testing::TemporaryDirectory directory;
	REQUIRE(FileSystem::CreateDirectories(directory.GetPath() / "Assets").IsOk());
	std::filesystem::path const file = directory.GetPath() / FileSystem::PathFromUtf8(Project::GameFileName);
	Testing::DocumentMutator mutator(5100);
	int opened = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(100); round < rounds; round++)
	{
		CAPTURE(round);
		REQUIRE(FileSystem::WriteTextFile(file, DumpJson(mutator.Damage(original, round))).IsOk());

		// The player reads these before anything else exists: they must make a window that can be created.
		if (Result<ProjectWindowSettings> window = Project::ReadWindowSettings(file, ProjectFileKind::Game))
		{
			CHECK(window.GetValue().Width >= 64);
			CHECK(window.GetValue().Width <= 16384);
			CHECK(window.GetValue().Height >= 64);
			CHECK(window.GetValue().Height <= 16384);
		}

		Testing::AssetManagerScope assets;
		Result<Ref<Project>> game = Project::OpenGame(file);
		(game ? opened : refused)++;
		if (game)
		{
			CheckGameDataFolder(game.GetValue()->GetSettings());
		}
	}
	CHECK(opened > 0);
	CHECK(refused > 0);
}
