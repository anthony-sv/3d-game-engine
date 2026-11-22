#include "RuntimeTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Platform/Process.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ProjectSettings.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;
using namespace Strada::RuntimeTesting;

// The player's --hash-every-step on a generated game: the test summary's fold of every fixed step's physics state hash, which
// Moto's runs at three frame rates compare (MotoShipTests.cpp). A ctest entry with them (StradaRuntimeTests.Ship,
// Shards.cmake).

namespace
{
	// A project of one scene without scripts: a stack of crates a ball knocks over on a ground, seen by a camera. A test run
	// of it never finishes testing, so it ends at its timeout and its summary says so.
	class StackGame
	{
	public:
		StackGame()
		{
			Ref<Scene> const scene = MakeScene("Stack", std::nullopt);
			Entity ground = scene->CreateEntity("Ground");
			ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
			ground.AddComponent<BoxColliderComponent>().HalfExtents = {20.0f, 0.5f, 20.0f};
			for (int level = 0; level < 4; level++)
			{
				Entity crate = scene->CreateEntity("Crate");
				crate.GetComponent<TransformComponent>().Translation = {0.03f * static_cast<float>(level % 2),
				                                                        0.5f + static_cast<float>(level), 0.0f};
				crate.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
				crate.AddComponent<BoxColliderComponent>();
			}
			Entity ball = scene->CreateEntity("Ball");
			ball.GetComponent<TransformComponent>().Translation = {-5.0f, 0.5f, 0.1f};
			RigidBodyComponent& body = ball.AddComponent<RigidBodyComponent>();
			body.Type = RigidBodyType::Dynamic;
			body.Mass = 5.0f;
			body.InitialLinearVelocity = {8.0f, 0.0f, 0.0f};
			ball.AddComponent<SphereColliderComponent>();

			std::filesystem::path const scenes = m_Directory.GetPath() / "Assets" / "Scenes";
			REQUIRE(FileSystem::CreateDirectories(scenes).IsOk());
			REQUIRE(SceneSerializer::SaveToFile(*scene, scenes / "Stack.sscene").IsOk());
			ProjectSettings settings;
			settings.Name = "Stack";
			{
				Testing::AssetManagerScope assets;
				REQUIRE(AssetManager::OpenAssetDirectory(m_Directory.GetPath() / "Assets").IsOk());
				settings.StartScene = AssetManager::FindByPath("Scenes/Stack.sscene");
				REQUIRE(settings.StartScene.IsValid());
			}
			m_File = m_Directory.GetPath() / "Stack.sproj";
			REQUIRE(FileSystem::WriteTextFile(m_File, DumpJson(Project::Serialize(settings, ProjectFileKind::Project))).IsOk());
		}

		// A test run of four seconds of the game (240 fixed steps), with the arguments given after it.
		std::vector<std::string> TestRun(std::vector<std::string> const& more) const
		{
			std::vector<std::string> arguments = {"--project", GetFile(), "--test", "--timeout", "4"};
			arguments.insert(arguments.end(), more.begin(), more.end());
			return arguments;
		}

		std::string GetFile() const { return Native(m_File); }

	private:
		Testing::TemporaryDirectory m_Directory;
		std::filesystem::path m_File;
	};

	// The test summary's physics state, from "physics state" (or "no physics state") to the line's end; empty without one.
	std::string PhysicsState(std::string_view output)
	{
		size_t const summary = output.find("Tests failed: ");
		if (summary == std::string_view::npos)
		{
			return {};
		}
		size_t const end = output.find_first_of("\r\n", summary);
		std::string_view const line = output.substr(summary, end == std::string_view::npos ? std::string_view::npos : end - summary);
		size_t const start = line.find("; ", line.find(" frames"));
		return start == std::string_view::npos ? std::string() : std::string(line.substr(start + 2));
	}

	// The hash a physics state's fold ends with ("... folded <hash>"); empty without one.
	std::string FoldedHash(std::string_view state)
	{
		size_t const start = state.find(" folded ");
		return start == std::string_view::npos ? std::string() : std::string(state.substr(start + 8));
	}
}

TEST_CASE("Runtime: --hash-every-step ends a test summary with every step's physics state hash folded, the same at 30, 60 and "
          "144 frames per second")
{
	StackGame const game;

	// The fold of steps 1 to 180 comes after the hash of step 180, which is what it is without the option, and is the same
	// whichever way the frames divide the steps. The runs never finish testing: each ends at its timeout and says so.
	ProcessResult const plain = RunPlayer(game.TestRun({"--hash-step", "180"}));
	CAPTURE(plain.Output);
	CHECK(plain.ExitCode == 1);
	std::string const hashOnly = PhysicsState(plain.Output);
	REQUIRE(hashOnly.starts_with("physics state "));
	REQUIRE(hashOnly.ends_with(" at step 180"));
	std::string expected;
	for (char const* timestep : {"0.0166666667", "0.0333333333", "0.0069444444"})
	{
		CAPTURE(timestep);
		ProcessResult const run = RunPlayer(game.TestRun({"--timestep", timestep, "--hash-step", "180", "--hash-every-step"}));
		CAPTURE(run.Output);
		CHECK(run.ExitCode == 1);
		CHECK(Contains(run.Output, "; the scripts did not finish testing in "));
		std::string const state = PhysicsState(run.Output);
		CHECK(state.starts_with(hashOnly + "; steps 1 to 180 folded "));
		std::string const folded = FoldedHash(state);
		REQUIRE(folded.size() == 16);
		if (expected.empty())
		{
			expected = folded;
		}
		CHECK(folded == expected);
	}

	// The fold tells the steps it covers apart: one step fewer folds another hash.
	std::string const shorter = PhysicsState(RunPlayer(game.TestRun({"--hash-step", "179", "--hash-every-step"})).Output);
	CAPTURE(shorter);
	CHECK(Contains(shorter, " at step 179; steps 1 to 179 folded "));
	CHECK(FoldedHash(shorter) != expected);

	// Without --hash-step it folds every step to the run's last, where the hash is taken (about step 240).
	std::string const last = PhysicsState(RunPlayer(game.TestRun({"--hash-every-step"})).Output);
	CAPTURE(last);
	size_t const stepAt = last.find(" at step ");
	REQUIRE(stepAt != std::string::npos);
	std::string const step = last.substr(stepAt + 9, last.find(';', stepAt) - stepAt - 9);
	CHECK(step.starts_with("24"));
	CHECK(Contains(last, " at step " + step + "; steps 1 to " + step + " folded "));
	CHECK(FoldedHash(last).size() == 16);
	CHECK(FoldedHash(last) != expected);

	// A step the run never reaches has no hash and no fold.
	std::string const unreached = PhysicsState(RunPlayer(game.TestRun({"--hash-step", "5000", "--hash-every-step"})).Output);
	CHECK(unreached == "no physics state at step 5000: the run ended at step " + step);

	// The option belongs to test runs.
	CHECK(RunPlayer({"--project", game.GetFile(), "--headless", "--frames", "1", "--hash-every-step"}).ExitCode == 2);
}
