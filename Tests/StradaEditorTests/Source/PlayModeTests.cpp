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
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

TEST_CASE("PlayMode: entities deleted while playing go at once, also while paused")
{
	PlayFixture fixture;
	UUID const crate = fixture.Create("Crate");
	UUID const child = fixture.Create("Child");
	REQUIRE(fixture.Operations.ReparentEntity(child, crate).IsOk());
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());

	// A running scene defers destruction to the end of the frame, which never comes while it is paused; edits do not
	// wait, so undo restores the entities rather than a second copy next to the doomed ones.
	for (bool const paused : {false, true})
	{
		CAPTURE(paused);
		fixture.Play.SetPaused(paused);
		std::array<UUID, 1> const doomed = {crate};
		REQUIRE(fixture.Operations.DeleteEntities(doomed).IsOk());
		CHECK_FALSE(fixture.Context.GetScene().HasEntity(crate));
		CHECK_FALSE(fixture.Context.GetScene().HasEntity(child));
		REQUIRE(fixture.Context.Undo().IsOk());
		CHECK(fixture.Context.GetScene().GetEntityByUUID(child).GetComponent<RelationshipComponent>().Parent == crate);
		REQUIRE(fixture.Context.Redo().IsOk());
		CHECK_FALSE(fixture.Context.GetScene().HasEntity(crate));
		REQUIRE(fixture.Context.Undo().IsOk());
		fixture.Play.Update(Timestep(1.0f / 60.0f));
		CHECK(fixture.Context.GetScene().HasEntity(crate));
		CHECK(fixture.Context.GetScene().HasEntity(child));
	}
	fixture.Play.Stop();
	CHECK(fixture.Context.GetScene().HasEntity(child));
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
	CHECK(fixture.Registry.GetCommandCount() == 10);
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

TEST_CASE("PlayCommands: automation finds the UI with ui.layout and clicks its buttons through input.set")
{
	Testing::ScriptEngineScope scripting;
	InputScope input;
	PlayFixture fixture;
	// The game view is 640 x 480 pixels: the 200 x 60 button in the middle of the canvas spans (220, 210) to (420, 270).
	UUID const canvas = fixture.Create("Canvas", Json::object({{"UICanvas", Json::object({{"ScaleMode", "ConstantPixelSize"}})}}));
	EntityCreateInfo info;
	info.Name = "Button";
	info.Parent = canvas;
	info.Components = Json::object(
		{{"UIRect", Json::object({{"Size", Json::array({200.0, 60.0})}})}, {"UIImage", Json::object()}, {"UIButton", Json::object()}});
	Result<UUID> const button = fixture.Operations.CreateEntity(info);
	REQUIRE(button.IsOk());
	std::string const buttonId = std::to_string(button.GetValue().GetValue());
	// The canvas's script hears the clicks of the buttons in it and names its entity after them each update.
	Json const watched = Json::object({{"Type", "Entity"}, {"Value", buttonId}});
	REQUIRE(fixture.Operations
	            .AddComponent(canvas, "Script",
	                          Json::object({{"ClassName", "Strada.Tests.ClickRecorder"}, {"Fields", Json::object({{"Watched", watched}})}}))
	            .IsOk());

	// Where the button is in the game view, before playing at the size the game view would have.
	Json const layout = fixture.Ok("ui.layout");
	CHECK(layout["width"] == 640);
	CHECK(layout["height"] == 480);
	CHECK(layout["pointerOverUI"] == false);
	REQUIRE(layout["elements"].size() == 2);
	Json const& canvasElement = layout["elements"][0];
	CHECK(canvasElement["id"] == std::to_string(canvas.GetValue()));
	CHECK(canvasElement["name"] == "Canvas");
	CHECK(canvasElement["hitTarget"] == false);
	CHECK(canvasElement["button"].is_null());
	CHECK_FALSE(canvasElement.contains("state"));
	Json const& buttonElement = layout["elements"][1];
	CHECK(buttonElement["id"] == buttonId);
	CHECK(buttonElement["canvas"] == std::to_string(canvas.GetValue()));
	CHECK(buttonElement["min"] == Json::array({220.0, 210.0}));
	CHECK(buttonElement["max"] == Json::array({420.0, 270.0}));
	CHECK(buttonElement["center"] == Json::array({320.0, 240.0}));
	CHECK(buttonElement["opacity"] == 1.0);
	CHECK(buttonElement["hitTarget"] == true);
	CHECK(buttonElement["button"] == buttonId);
	CHECK(buttonElement["state"] == "normal");
	CHECK(fixture.Ok("ui.layout", Json::object({{"entity", buttonId}}))["elements"] == Json::array({buttonElement}));
	CHECK(fixture.Fails("ui.layout", Json::object({{"entity", "not an id"}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("ui.layout", Json::object({{"entity", "12345"}})) == AutomationErrorCode::EntityNotFound);
	UUID const outside = fixture.Create("Outside");
	CHECK(fixture.Fails("ui.layout", Json::object({{"entity", std::to_string(outside.GetValue())}})) == AutomationErrorCode::InvalidParams);

	REQUIRE(fixture.Ok("play.start")["state"] == "play");
	fixture.Ok("play.pause");
	auto const advance = [&fixture, canvas](Json const& devices)
	{
		fixture.Ok("input.set", devices);
		// The editor's frame ends between the two requests, and Input's one-frame pressed and released flags with it.
		Input::EndFrame();
		fixture.Ok("play.advance", Json::object({{"frames", 1}}));
		Entity const recorder = fixture.Context.GetScene().GetEntityByUUID(canvas);
		REQUIRE(recorder);
		return recorder.GetName();
	};
	// The mouse at the center ui.layout gave.
	CHECK(advance(Json::object({{"mousePosition", buttonElement["center"]}})) ==
	      "Clicks=0;Last=0;InClick=False;Polled=False;State=Hovered;Over=True");
	Json const hovered = fixture.Ok("ui.layout", Json::object({{"entity", buttonId}}));
	CHECK(hovered["pointerOverUI"] == true);
	CHECK(hovered["elements"][0]["state"] == "hovered");
	CHECK(advance(Json::object({{"mouseButtons", Json::object({{"Left", true}})}})) ==
	      "Clicks=0;Last=0;InClick=False;Polled=False;State=Pressed;Over=True");
	CHECK(fixture.Ok("ui.layout", Json::object({{"entity", buttonId}}))["elements"][0]["state"] == "pressed");
	CHECK(advance(Json::object({{"mouseButtons", Json::object({{"Left", false}})}})) ==
	      "Clicks=1;Last=" + buttonId + ";InClick=True;Polled=True;State=Hovered;Over=True");
	// Outside the button, a press and a release click nothing.
	CHECK(advance(Json::object({{"mousePosition", Json::array({100.0, 100.0})}, {"mouseButtons", Json::object({{"Left", true}})}})) ==
	      "Clicks=1;Last=" + buttonId + ";InClick=True;Polled=False;State=Normal;Over=False");
	CHECK(advance(Json::object({{"mouseButtons", Json::object({{"Left", false}})}})) ==
	      "Clicks=1;Last=" + buttonId + ";InClick=True;Polled=False;State=Normal;Over=False");
	// A press that input.release ends (the window losing the focus) is canceled.
	advance(Json::object({{"mousePosition", buttonElement["center"]}, {"mouseButtons", Json::object({{"Left", true}})}}));
	fixture.Ok("input.release");
	Input::EndFrame();
	fixture.Ok("play.advance", Json::object({{"frames", 1}}));
	CHECK(fixture.Context.GetScene().GetEntityByUUID(canvas).GetName() ==
	      "Clicks=1;Last=" + buttonId + ";InClick=True;Polled=False;State=Hovered;Over=True");
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

TEST_CASE("PlayCommands: input.set plays the Blocks sample: Enter and clicks start games, Escape and P pause, R starts over")
{
	// The sample as a player gets it: its scenes and assets, with the scripts this build made of its sources.
	Testing::AssetManagerScope assets;
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting(FileSystem::GetExecutableDirectory() / "Blocks" / "Blocks.dll");
	InputScope input;
	Testing::TemporaryDirectory directory;
	std::filesystem::path const sample = FileSystem::PathFromUtf8(STRADA_PROJECTS_DIR) / "Blocks";
	REQUIRE(FileSystem::CopyDirectory(sample / "Assets", directory.GetPath() / "Assets").IsOk());
	REQUIRE(FileSystem::Copy(sample / "Blocks.sproj", directory.GetPath() / "Blocks.sproj", false).IsOk());

	PlayFixture fixture;
	REQUIRE(fixture.Operations.OpenProject(directory.GetPath() / "Blocks.sproj").IsOk());
	// The best score stays out of the user's data folder, and the game-over screen soon goes back to the title.
	Entity const game = fixture.Context.GetScene().FindEntityByName("Game");
	REQUIRE(game);
	Json const fields = Json::object({{"SaveBestScore", Json::object({{"Type", "Bool"}, {"Value", false}})},
	                                  {"TitleReturnDelay", Json::object({{"Type", "Float"}, {"Value", 1.0}})}});
	REQUIRE(fixture.Operations.SetComponentFields(game.GetUUID(), "Script", Json::object({{"Fields", fields}})).IsOk());

	CHECK(fixture.Ok("play.start")["state"] == "play");
	fixture.Ok("play.pause");
	auto const advance = [&fixture](uint32_t frames)
	{
		fixture.Ok("play.advance", Json::object({{"frames", frames}}));
	};
	// A key or button goes down for a frame, then up.
	auto const press = [&fixture, &advance](std::string const& device, std::string const& name)
	{
		fixture.Ok("input.set", Json::object({{device, Json::object({{name, true}})}}));
		advance(1);
		fixture.Ok("input.set", Json::object({{device, Json::object({{name, false}})}}));
		advance(1);
	};
	// What a text of the game's screen says.
	auto const text = [&fixture](std::string_view name)
	{
		Entity const entity = fixture.Context.GetScene().FindEntityByName(name);
		REQUIRE(entity);
		return entity.GetComponent<TextComponent>().Text;
	};
	// Hard drops pieces until the stack reaches the top.
	auto const topOut = [&press, &text]
	{
		for (int piece = 0; piece < 40 && text("Headline") != "GAME OVER"; piece++)
		{
			press("keys", "Space");
		}
		REQUIRE(text("Headline") == "GAME OVER");
	};

	// The title screen, where the board plays itself, starts a game with Enter.
	advance(60);
	CHECK(text("Prompt") == "PRESS ENTER OR CLICK TO PLAY");
	press("keys", "Enter");
	CHECK(text("Prompt").empty());
	CHECK(text("Score") == "0");

	// Escape pauses and Enter goes on; P pauses and Escape goes on.
	press("keys", "Escape");
	CHECK(text("Headline") == "PAUSED");
	press("keys", "Enter");
	CHECK(text("Headline").empty());
	press("keys", "P");
	CHECK(text("Headline") == "PAUSED");
	press("keys", "Escape");
	CHECK(text("Headline").empty());

	// R starts the game over: the points of a hard drop go.
	press("keys", "Space");
	CHECK(text("Score") != "0");
	press("keys", "R");
	CHECK(text("Score") == "0");

	// The game-over screen plays again with a click, with Enter and with R.
	std::array<std::pair<std::string, std::string>, 3> const playAgain = {{{"mouseButtons", "Left"}, {"keys", "Enter"}, {"keys", "R"}}};
	for (std::pair<std::string, std::string> const& again : playAgain)
	{
		CAPTURE(again.second);
		topOut();
		press(again.first, again.second);
		CHECK(text("Headline").empty());
		CHECK(text("Prompt").empty());
		CHECK(text("Score") == "0");
	}

	// Left alone, the game-over screen goes back to the title, which starts a game with a click.
	topOut();
	advance(70);
	CHECK(text("Prompt") == "PRESS ENTER OR CLICK TO PLAY");
	press("mouseButtons", "Left");
	CHECK(text("Prompt").empty());
	CHECK(text("Headline").empty());

	CHECK(fixture.Ok("play.stop")["state"] == "edit");
}
