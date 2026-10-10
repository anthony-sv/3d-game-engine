#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/PlayCommands.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/PlayMode.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>

using namespace Strada;

namespace
{
	// Input is global: tests that set it restore it.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	// The editor model with play mode and its commands, as the editor layer sets them up.
	struct PlayFixture
	{
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};
		CommandRegistry Registry;

		PlayFixture()
		{
			REQUIRE(RegisterPlayCommands(Registry, Play, Context,
			                             []
			                             {
											 return glm::uvec2(640, 480);
										 })
			            .IsOk());
		}

		UUID Create(std::string name, Json components = Json::object())
		{
			EntityCreateInfo info;
			info.Name = std::move(name);
			info.Components = std::move(components);
			Result<UUID> entity = Operations.CreateEntity(info);
			REQUIRE(entity.IsOk());
			return entity.GetValue();
		}

		// Commands complete at once here: scripts never need building without a project.
		CommandResult Run(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			return std::move(*result);
		}

		Json Ok(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
			return result.TakeValue();
		}

		AutomationErrorCode Fails(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE(result.IsError());
			return result.GetError().Code;
		}
	};

	// A scene whose SceneSwitcher loads nextScene on L and quits on Q.
	void WriteSwitcherScene(std::filesystem::path const& path, std::string const& name, std::string const& nextScene)
	{
		Scene scene(name);
		Entity switcher = scene.CreateEntity("Switcher");
		ScriptComponent& script = switcher.AddComponent<ScriptComponent>();
		script.ClassName = "Strada.Tests.SceneSwitcher";
		script.Fields["NextScene"] = ScriptFieldValue::FromString(nextScene);
		REQUIRE(SceneSerializer::SaveToFile(scene, path).IsOk());
	}

	void AdvanceWithKey(PlayMode& play, KeyCode key)
	{
		Input::SetKeyState(key, true);
		play.Advance(1, Timestep(1.0f / 60.0f));
		Input::SetKeyState(key, false);
	}
}

TEST_CASE("PlayMode: playing shows and edits a copy that stopping discards")
{
	PlayFixture fixture;
	UUID const crate = fixture.Create("Crate");
	std::array<UUID, 1> const selection = {crate};
	REQUIRE(fixture.Operations.Select(selection).IsOk());
	Scene const* const edited = &fixture.Context.GetScene();
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	uint64_t const version = fixture.Context.GetSceneVersion();

	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	CHECK(fixture.Play.IsPlaying());
	CHECK(fixture.Context.GetPlayState() == EditorPlayState::Play);
	CHECK(&fixture.Context.GetScene() != edited);
	CHECK(fixture.Context.GetScene().IsRunning());
	CHECK(fixture.Context.GetScene().GetViewportWidth() == 640);
	CHECK(fixture.Context.GetSceneVersion() != version);
	// The copy has the same entities, so the selection carries over; its history starts empty.
	CHECK(fixture.Context.GetSelection().Contains(crate));
	CHECK_FALSE(fixture.Context.GetHistory().CanUndo());
	CHECK(fixture.Context.IsDirty());
	CHECK(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsError());

	// Edits while playing reach the copy and can be undone there.
	REQUIRE(fixture.Operations.RenameEntity(crate, "Moved").IsOk());
	UUID const spawned = fixture.Create("Spawned");
	std::array<UUID, 2> const both = {crate, spawned};
	REQUIRE(fixture.Operations.Select(both).IsOk());
	CHECK(fixture.Context.GetHistory().CanUndo());

	// Scene and project files wait until playing stops.
	CHECK(fixture.Operations.SaveScene("Scene.sscene").IsError());
	CHECK(fixture.Operations.NewScene().IsError());
	CHECK(fixture.Operations.OpenScene("Scene.sscene").IsError());
	CHECK(fixture.Operations.CloseProject().IsError());

	fixture.Play.Stop();
	CHECK_FALSE(fixture.Play.IsPlaying());
	CHECK(fixture.Context.GetPlayState() == EditorPlayState::Edit);
	CHECK(&fixture.Context.GetScene() == edited);
	CHECK(fixture.Context.GetScene().GetEntityByUUID(crate).GetName() == "Crate");
	CHECK_FALSE(fixture.Context.GetScene().HasEntity(spawned));
	CHECK_FALSE(fixture.Context.GetScene().IsRunning());
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
	CHECK(fixture.Context.GetSelection().GetEntities() == std::vector<UUID>{crate});
}

TEST_CASE("PlayMode: scenes the scripts load replace the copy, and quitting stops playing")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	InputScope input;
	Testing::TemporaryDirectory directory;
	std::filesystem::path const scenes = directory.GetPath() / "Scenes";
	REQUIRE(FileSystem::CreateDirectories(scenes).IsOk());
	WriteSwitcherScene(scenes / "First.sscene", "First", "Scenes/Second.sscene");
	WriteSwitcherScene(scenes / "Second.sscene", "Second", "");
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());

	PlayFixture fixture;
	REQUIRE(fixture.Operations.OpenScene(scenes / "First.sscene").IsOk());
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	uint64_t const version = fixture.Context.GetSceneVersion();
	AdvanceWithKey(fixture.Play, KeyCode::L);
	CHECK(fixture.Context.GetScene().GetName() == "Second");
	CHECK(fixture.Context.GetSceneVersion() != version);
	CHECK(fixture.Context.GetScene().IsRunning());

	AdvanceWithKey(fixture.Play, KeyCode::Q);
	CHECK_FALSE(fixture.Play.IsPlaying());
	CHECK(fixture.Context.GetScene().GetName() == "First");
	TestRunReport const* const report = fixture.Play.GetTestReport();
	REQUIRE(report != nullptr);
	REQUIRE(report->Results.size() == 4);
	CHECK(report->Results[1].Name == "First stopped");
	CHECK(report->Results[3].Name == "Second stopped");
}

TEST_CASE("PlayMode: simulating runs physics only, and pausing holds the scene")
{
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	PlayFixture fixture;
	UUID const ball = fixture.Create("Ball", Json::object({{"Transform", Json::object({{"Translation", {0.0, 5.0, 0.0}}})},
	                                                       {"RigidBody", Json::object({{"Type", "Dynamic"}})},
	                                                       {"SphereCollider", Json::object()},
	                                                       {"Script", Json::object({{"ClassName", "Strada.Tests.Mover"}})}}));
	REQUIRE(fixture.Play.Start(EditorPlayState::Simulate, 640, 480).IsOk());
	CHECK(fixture.Context.GetPlayState() == EditorPlayState::Simulate);
	CHECK(ScriptEngine::GetInstanceCount() == 0);
	CHECK(fixture.Context.GetScene().GetAudioScene() == nullptr);

	auto const height = [&fixture, ball]
	{
		return fixture.Context.GetScene().GetEntityByUUID(ball).GetComponent<TransformComponent>().Translation.y;
	};
	CHECK(fixture.Play.Advance(30, Timestep(1.0f / 60.0f)) == 30);
	float const fallen = height();
	CHECK(fallen < 5.0f);

	// Paused: the editor's frames hold the scene, steps advance it, and Advance runs frames anyway.
	fixture.Play.SetPaused(true);
	CHECK(fixture.Play.IsPaused());
	uint64_t const frame = fixture.Context.GetScene().GetRuntimeFrame();
	fixture.Play.Update(Timestep(1.0f / 60.0f));
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == frame);
	fixture.Play.Step(2);
	fixture.Play.Update(Timestep(1.0f / 60.0f));
	fixture.Play.Update(Timestep(1.0f / 60.0f));
	fixture.Play.Update(Timestep(1.0f / 60.0f));
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == frame + 2);
	CHECK(fixture.Play.Advance(3, Timestep(1.0f / 60.0f)) == 3);
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == frame + 5);
	CHECK(height() < fallen);

	fixture.Play.Stop();
	CHECK(height() == 5.0f);
}

TEST_CASE("PlayCommands: every play, input and test command is documented in Docs/Automation.md")
{
	PlayFixture fixture;
	Result<std::string> reference = FileSystem::ReadTextFile(STRADA_AUTOMATION_DOC_PATH);
	REQUIRE(reference.IsOk());
	CHECK(fixture.Registry.GetCommandCount() == 9);
	for (CommandDefinition const* command : fixture.Registry.GetCommands())
	{
		CAPTURE(command->Name);
		CHECK(reference.GetValue().find("`" + command->Name + "`") != std::string::npos);
	}
}

TEST_CASE("PlayCommands: automation plays, steps, injects input and stops")
{
	Testing::ScriptEngineScope scripting;
	InputScope input;
	PlayFixture fixture;
	fixture.Create("Crate");

	CHECK(fixture.Ok("play.state")["state"] == "edit");
	CHECK(fixture.Fails("play.stop") == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("play.advance", Json::object({{"frames", 1}})) == AutomationErrorCode::InvalidOperation);

	Json const started = fixture.Ok("play.start");
	CHECK(started["state"] == "play");
	CHECK(started["frame"] == 0);
	CHECK(fixture.Fails("play.start") == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("play.step") == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Ok("play.pause")["paused"] == true);
	CHECK(fixture.Ok("play.step", Json::object({{"frames", 2}}))["paused"] == true);
	Json const advanced = fixture.Ok("play.advance", Json::object({{"frames", 5}, {"timestep", 0.5}}));
	CHECK(advanced["framesRun"] == 5);
	CHECK(advanced["frame"] == 5);
	CHECK(advanced["time"] == doctest::Approx(2.5));

	Json const keys = fixture.Ok("input.set", Json::object({{"keys", Json::object({{"A", true}, {"Space", true}})},
	                                                        {"mouseButtons", Json::object({{"Left", true}})},
	                                                        {"mousePosition", {12.0, 34.0}}}));
	CHECK(keys["keys"] == Json::array({"Space", "A"}));
	CHECK(keys["mouseButtons"] == Json::array({"Left"}));
	CHECK(keys["mousePosition"] == Json::array({12.0, 34.0}));
	CHECK(Input::IsKeyDown(KeyCode::A));
	CHECK(fixture.Fails("input.set", Json::object({{"keys", Json::object({{"Nope", true}})}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Ok("input.release")["keys"].empty());

	Json const stopped = fixture.Ok("play.stop");
	CHECK(stopped["state"] == "edit");
	CHECK(fixture.Ok("play.start", Json::object({{"mode", "simulate"}}))["state"] == "simulate");
	CHECK(fixture.Ok("play.stop")["state"] == "edit");
}

TEST_CASE("PlayCommands: test runs report the scripts' checks and time out")
{
	Testing::ScriptEngineScope scripting;
	PlayFixture fixture;
	UUID const tester = fixture.Create("Tester", Json::object({{"Script", Json::object({{"ClassName", "Strada.Tests.SelfTest"}})}}));

	Json const passed = fixture.Ok("test.run");
	CAPTURE(passed.dump());
	CHECK(passed["finished"] == true);
	CHECK(passed["timedOut"] == false);
	CHECK(passed["frames"] == 1);
	CHECK(passed["passed"] == 2);
	CHECK(passed["failures"] == 0);
	CHECK_FALSE(fixture.Play.IsPlaying());

	REQUIRE(fixture.Operations
	            .SetComponentFields(tester, "Script",
	                                Json::object({{"Fields", Json::object({{"Fail", Json::object({{"Type", "Bool"}, {"Value", true}})}})}}))
	            .IsOk());
	Json const failed = fixture.Ok("test.run");
	CHECK(failed["failed"] == 1);
	CHECK(failed["results"][1]["message"] == "expected false: Fail is set");

	REQUIRE(fixture.Operations.DeleteEntities(std::array<UUID, 1>{tester}).IsOk());
	Json const timedOut = fixture.Ok("test.run", Json::object({{"timeout", 0.5}, {"timestep", 0.1}}));
	CHECK(timedOut["finished"] == false);
	CHECK(timedOut["timedOut"] == true);
	CHECK(timedOut["frames"] == 5);
	CHECK(fixture.Fails("test.run", Json::object({{"scene", "Scenes/Missing.sscene"}})) == AutomationErrorCode::AssetNotFound);
}
