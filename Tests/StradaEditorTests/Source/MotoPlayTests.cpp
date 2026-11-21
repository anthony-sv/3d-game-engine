#include "Audio/AudioTestUtilities.h"
#include "Core/TelemetryTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/GraphDebugCommands.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/Automation/TelemetryCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/EditorTelemetry.h"
#include "Editor/PlayMode.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/UUID.h"
#include "Strada/Renderer/QualityPresets.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/PlayerSettings.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

// The Moto sample in the editor, played as M1 shows it (Docs/Architecture.md section 15, "The race"): a conditional
// breakpoint on its race director stepped into the HUD across a broadcast, the race's telemetry marks recorded at three
// frame rates, its menus driven by an injected gamepad, the mouse and the keyboard, their settings and bindings kept for
// the next play, the HUD's shift light following a live edit of the bike's definition, and the camera director following
// the race director from the title's attract mode to the race's chase view.

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

	// A copy of the Moto sample (its project file and assets, as Moto's sample case copies it) open in an editor model with
	// the commands the editor layer registers for automation that the cases use: the editor's, play mode's, the graph
	// debugger's (its session play mode's observer; headless, so breakpoints are kept in memory) and telemetry's.
	class MotoPlay
	{
	public:
		MotoPlay()
		{
			std::filesystem::path const sample = FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "Moto";
			REQUIRE(FileSystem::CopyDirectory(sample / "Assets", m_Directory.GetPath() / "Assets").IsOk());
			REQUIRE(FileSystem::Copy(sample / "Moto.sproj", m_Directory.GetPath() / "Moto.sproj", false).IsOk());
			m_Context.SetAssetReferenceResolver(
				[](std::string_view reference)
				{
					return AssetManager::ResolveReference(reference);
				});
			m_Session.emplace(m_Context, m_Play);
			REQUIRE(RegisterEditorCommands(m_Registry, m_Operations, EditorCommandEnvironment()).IsOk());
			REQUIRE(RegisterPlayCommands(m_Registry, m_Play, m_Context,
			                             []
			                             {
											 return glm::uvec2(640, 480);
										 })
			            .IsOk());
			REQUIRE(RegisterGraphDebugCommands(m_Registry, *m_Session, m_Play, m_Context).IsOk());
			REQUIRE(RegisterTelemetryCommands(m_Registry, m_Telemetry, m_Context).IsOk());
			REQUIRE(m_Operations.OpenProject(m_Directory.GetPath() / "Moto.sproj").IsOk());
			m_Play.SetObserver(&*m_Session);
		}

		~MotoPlay()
		{
			m_Play.Stop();
			m_Play.SetObserver(nullptr);
		}

		MotoPlay(MotoPlay const&) = delete;
		MotoPlay& operator=(MotoPlay const&) = delete;

		Json Ok(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			m_Registry.Execute(name, params,
			                   [&result](CommandResult value)
			                   {
								   result.emplace(std::move(value));
							   });
			REQUIRE_MESSAGE(result.has_value(), name);
			REQUIRE_MESSAGE(result->IsOk(), name, ": ", (result->IsError() ? result->GetError().Message : std::string()), " (",
			                params.dump().substr(0, 400), ")");
			return result->TakeValue();
		}

		// Starts playing, paused: the frames run as the case advances them.
		void Start()
		{
			CHECK(Ok("play.start")["state"] == "play");
			Ok("play.pause");
		}

		void Stop() { CHECK(Ok("play.stop")["state"] == "edit"); }

		void Advance(uint32_t frames) { Ok("play.advance", Json::object({{"frames", frames}})); }

		// The input of the next frame: the editor's frame ends between the request and the frame.
		void Give(Json devices)
		{
			Ok("input.set", std::move(devices));
			Input::EndApplicationFrame();
		}

		// A key goes down for a frame, then up.
		void Press(std::string const& key)
		{
			Give(Json::object({{"keys", Json::object({{key, true}})}}));
			Advance(1);
			Give(Json::object({{"keys", Json::object({{key, false}})}}));
			Advance(1);
		}

		// A button of an injected gamepad in slot 0 goes down for a frame, then up.
		void PressPad(std::string const& button)
		{
			Give(Json::object({{"gamepads", Json::object({{"0", Json::object({{"buttons", Json::object({{button, true}})}})}})}}));
			Advance(1);
			Give(Json::object({{"gamepads", Json::object({{"0", Json::object({{"buttons", Json::object({{button, false}})}})}})}}));
			Advance(1);
		}

		// The mouse moves over a UI element of the 640 by 480 game view (where ui.layout puts its center), then its left
		// button goes down for a frame and up.
		void Click(std::string_view element)
		{
			Json const layout = Ok("ui.layout", Json::object({{"entity", std::to_string(Find(element).GetUUID().GetValue())}}));
			REQUIRE(layout["elements"].size() == 1);
			Give(Json::object({{"mousePosition", layout["elements"][0]["center"]}}));
			Advance(1);
			Give(Json::object({{"mouseButtons", Json::object({{"Left", true}})}}));
			Advance(1);
			Give(Json::object({{"mouseButtons", Json::object({{"Left", false}})}}));
			Advance(1);
		}

		Entity Find(std::string_view name)
		{
			Entity const entity = m_Context.GetScene().FindEntityByName(name);
			REQUIRE_MESSAGE(entity, name);
			return entity;
		}

		// A public variable of the graph an entity runs, as its instance holds it.
		ScriptFieldValue ReadOut(std::string_view entity, std::string const& name)
		{
			Result<ScriptFieldMap> const variables = VisualScriptEngine::GetInstanceVariables(Find(entity).GetUUID());
			REQUIRE_MESSAGE(variables.IsOk(), (variables ? std::string() : variables.GetError()));
			auto const variable = variables.GetValue().find(name);
			REQUIRE_MESSAGE(variable != variables.GetValue().end(), name);
			return variable->second;
		}

		// Sets a public variable of the graph an entity runs, in the scene (an edit, as the inspector makes one).
		void SetVariable(std::string_view entity, std::string const& name, ScriptFieldValue value)
		{
			Entity const found = Find(entity);
			VisualScriptComponent script = found.GetComponent<VisualScriptComponent>();
			script.Variables[name] = std::move(value);
			REQUIRE(m_Operations.SetComponent(found.GetUUID(), script).IsOk());
		}

		std::string GetText(std::string_view element) { return Find(element).GetComponent<UITextComponent>().Text; }

		bool IsShown(std::string_view element) { return Find(element).GetComponent<UIRectComponent>().Visible; }

		std::filesystem::path const& GetDirectory() const { return m_Directory.GetPath(); }

	private:
		Testing::AudioEngineScope m_Audio;
		Testing::AssetManagerScope m_Assets;
		Testing::PhysicsSystemScope m_Physics;
		Testing::ScriptRuntimeScope m_Scripting;
		Testing::TelemetryScope m_TelemetryScope;
		InputScope m_Input;
		Testing::TemporaryDirectory m_Directory;
		EditorContext m_Context;
		EditorOperations m_Operations{m_Context};
		EditorScripts m_Scripts{m_Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode m_Play{m_Context, m_Scripts};
		std::optional<VisualScriptDebugSession> m_Session;
		EditorTelemetry m_Telemetry;
		CommandRegistry m_Registry;
	};

	// The flow test scene (Scenes/Flow.sscene): its race checks' graph (Tools/RaceChecks.py) sends the race director two
	// scripted three-lap races and a third that a box's crossing of the line begins, all by about step 480.
	constexpr std::string_view FlowScene = "Scenes/Flow.sscene";
	constexpr std::string_view Director = "asset://Scripts/RaceDirector.sgraph";
	constexpr std::string_view Definition = "asset://Vehicles/Moto.svehicle";

	// The node of the race director's BeginLap function that broadcasts LapStarted.
	int32_t FindLapStartedBroadcast(MotoPlay& moto)
	{
		Json const document = moto.Ok("graph.get", Json::object({{"graph", Director}, {"analyze", false}}))["document"];
		for (Json const& function : document["Functions"])
		{
			if (function["Name"] == "BeginLap")
			{
				for (Json const& node : function["Graph"]["Nodes"])
				{
					if (node["Type"] == "Event.Broadcast")
					{
						return node["ID"].get<int32_t>();
					}
				}
			}
		}
		FAIL("the race director's BeginLap broadcasts nothing");
		return -1;
	}
}

TEST_CASE("MotoPlay: a conditional breakpoint Lap == 3 on the race director stops at the third lap's LapStarted, and Step Into "
          "goes across the broadcast into the HUD's LapStarted")
{
	MotoPlay moto;
	moto.Ok("scene.open", Json::object({{"path", FlowScene}}));
	int32_t const broadcast = FindLapStartedBroadcast(moto);
	moto.Ok("graph.breakpoints",
	        Json::object({{"graph", Director}, {"add", Json::array({Json::object({{"node", broadcast}, {"condition", "Lap == 3"}})})}}));

	// The first scripted race's third lap begins a few steps after its countdown, by about step 300.
	moto.Start();
	Json stop;
	for (int32_t chunk = 0; chunk < 20 && stop.is_null(); chunk++)
	{
		Json const advanced = moto.Ok("play.advance", Json::object({{"frames", 30}}));
		if (advanced["stopped"].get<bool>())
		{
			stop = advanced["stop"];
		}
	}
	REQUIRE_FALSE(stop.is_null());
	CAPTURE(stop.dump());
	CHECK(stop["reason"] == "Breakpoint");
	CHECK(stop["path"] == "Scripts/RaceDirector.sgraph");
	CHECK(stop["name"] == "Race");
	CHECK(stop["frames"][0]["functionName"] == "BeginLap");
	CHECK(stop["frames"][0]["node"] == broadcast);
	// Reached at each lap's start, it held at the third alone.
	Json const breakpoints = moto.Ok("graph.breakpoints", Json::object({{"graph", Director}}))["breakpoints"];
	REQUIRE(breakpoints.size() == 1);
	CHECK(breakpoints[0]["reached"] == 3);
	CHECK(breakpoints[0]["hits"] == 1);
	Json const values = moto.Ok(
		"graph.values",
		Json::object({{"graph", Director}, {"stop", stop["id"]}, {"nodes", Json::array({broadcast})}, {"evaluate", Json::array({"Lap"})}}));
	CAPTURE(values.dump());
	CHECK(values["nodes"][0]["inputs"][0]["name"] == "Lap");
	CHECK(values["nodes"][0]["inputs"][0]["value"] == 3);
	CHECK(values["evaluated"][0]["value"]["value"] == 3);

	// Step Into goes into each receiver of the broadcast in turn (the race checks' graph counts the laps too), until the
	// HUD's: its LapStarted event, delivered from the director's BeginLap, with the third lap.
	Json hud;
	for (int32_t step = 0; step < 8 && hud.is_null(); step++)
	{
		Json const stepped = moto.Ok("graph.step", Json::object({{"kind", "Into"}}));
		REQUIRE(stepped["outcome"] == "Stopped");
		if (stepped["stop"]["path"] == "Scripts/Hud.sgraph")
		{
			hud = stepped["stop"];
		}
	}
	REQUIRE_FALSE(hud.is_null());
	CAPTURE(hud.dump());
	CHECK(hud["name"] == "Screen");
	CHECK(hud["frames"][0]["title"] == "LapStarted");
	CHECK(hud["frames"][1]["kind"] == "Delivery");
	CHECK(hud["frames"][1]["path"] == "Scripts/RaceDirector.sgraph");
	CHECK(hud["frames"][1]["node"] == broadcast);
	Json const received = moto.Ok("graph.values", Json::object({{"graph", "asset://Scripts/Hud.sgraph"}, {"stop", hud["id"]}}));
	CAPTURE(received.dump());
	REQUIRE(received["event"].size() == 1);
	CHECK(received["event"][0]["name"] == "Lap");
	CHECK(received["event"][0]["value"] == 3);
	moto.Ok("graph.continue");
	moto.Stop();
}

TEST_CASE("MotoPlay: the race's telemetry marks sit on the same rows at 30, 60 and 144 frames a second")
{
	// The race director marks its laps and sectors at the crossings it times in fixed steps (Docs/Architecture.md section
	// 4.5), so each mark sits on the row of its step whatever the frame rate: the flow test scene's races, the box's real
	// crossing of the line among them, recorded at three rates for ten seconds of the game each.
	MotoPlay moto;
	moto.Ok("scene.open", Json::object({{"path", FlowScene}}));
	moto.Ok("telemetry.start");
	// The frames of ten seconds at each rate run a few steps fewer than 600 (the step's accumulator starts empty): the marks
	// are compared up to the last step all three sessions reached.
	constexpr uint64_t Steps = 600;
	std::vector<Json> sessions;
	for (double const timestep : {1.0 / 60.0, 1.0 / 30.0, 1.0 / 144.0})
	{
		CAPTURE(timestep);
		// Play mode keeps the game's data in the copy's .strada/PlayData from one play to the next, where the first play's
		// races leave a best lap that a later play would load: each play starts without one, so the three differ in their
		// frame rate alone.
		REQUIRE(FileSystem::RemoveAll(moto.GetDirectory() / ".strada" / "PlayData").IsOk());
		moto.Start();
		auto const frames = static_cast<uint32_t>(static_cast<double>(Steps) / 60.0 / timestep + 0.5);
		moto.Ok("play.advance", Json::object({{"frames", frames}, {"timestep", timestep}}));
		CHECK(moto.ReadOut("Race", "LoadedLapTime").GetFloat() == 0.0f);
		moto.Stop();
		Json const session = moto.Ok("telemetry.channels")["session"];
		REQUIRE_FALSE(session.is_null());
		CHECK(session["lastStep"].get<uint64_t>() >= Steps - 10);
		sessions.push_back(session);
	}
	moto.Ok("telemetry.stop");
	uint64_t last = Steps;
	for (Json const& session : sessions)
	{
		last = std::min(last, session["lastStep"].get<uint64_t>());
	}
	std::vector<Json> marks;
	for (Json const& session : sessions)
	{
		Json kept = Json::array();
		for (Json const& mark : session["marks"])
		{
			if (mark["step"].get<uint64_t>() <= last)
			{
				kept.push_back(mark);
			}
		}
		marks.push_back(std::move(kept));
	}
	CAPTURE(marks[0].dump());
	// Two scripted races of three laps (each lap and its two sectors, the race's end) and the real crossing's lap.
	CHECK(marks[0].size() >= 20);
	size_t laps = 0;
	size_t sectors = 0;
	for (Json const& mark : marks[0])
	{
		std::string const label = mark["label"].get<std::string>();
		laps += label.starts_with("Lap ") ? 1 : 0;
		sectors += label.starts_with("Sector ") ? 1 : 0;
	}
	CHECK(laps >= 9);
	CHECK(sectors >= 12);
	CHECK(marks[1] == marks[0]);
	CHECK(marks[2] == marks[0]);
}

TEST_CASE("MotoPlay: Moto's title opens the settings by the gamepad, clicks step the quality and the window's mode, the "
          "throttle takes the key pressed next with its conflict named, the pad's binding refuses a key, and the next play reads "
          "them back, steps the window back, rebinds the lean's two keys, resets the controls and resumes from the pause screen "
          "by a click")
{
	MotoPlay moto;
	moto.Start();
	moto.Advance(3);
	// The main scene opens on its title over the attract mode, RACE selected.
	REQUIRE(moto.ReadOut("Menus", "Screen").GetString() == "Title");
	CHECK(moto.IsShown("Title Screen"));
	CHECK(moto.ReadOut("Menus", "Selected").GetInt32() == 0);

	// The directional pad moves the selection to SETTINGS and A presses it.
	moto.PressPad("DPadDown");
	CHECK(moto.ReadOut("Menus", "Selected").GetInt32() == 1);
	moto.PressPad("A");
	moto.Advance(1);
	REQUIRE(moto.ReadOut("Menus", "Screen").GetString() == "Settings");
	CHECK(moto.IsShown("Settings Screen"));
	CHECK_FALSE(moto.IsShown("Title Screen"));
	CHECK(moto.GetText("Quality Value") == "HIGH");
	CHECK(moto.GetText("Throttle Key Text") == "W");
	CHECK(moto.GetText("Throttle Pad Text") == "PAD RIGHTTRIGGER");

	// The mouse clicks the quality's "<", and the window's ">": play mode keeps the window the settings ask for without
	// applying it to the editor's (PlayMode.cpp leaves SceneRunnerSettings::ApplyWindow empty).
	moto.Click("Quality Lower");
	CHECK(moto.GetText("Quality Value") == "MEDIUM");
	CHECK(moto.GetText("Window Value") == "WINDOWED");
	moto.Click("Window Upper");
	CHECK(moto.GetText("Window Value") == "FULLSCREEN");

	// The linked brakes' ">" turns them off, and the menus give the setting to their rider, the main scene's bike: its riding
	// graph's LinkedBrakes goes from on, as a new player's bike has it, to off.
	CHECK(moto.GetText("Linked Brakes Value") == "ON");
	CHECK(moto.ReadOut("Bike", "LinkedBrakes").GetBool());
	moto.Click("Linked Brakes Upper");
	CHECK(moto.GetText("Linked Brakes Value") == "OFF");
	CHECK_FALSE(moto.ReadOut("Bike", "LinkedBrakes").GetBool());

	// The throttle's keyboard button listens; E, the key of ShiftUp, answers: the throttle takes it, and the screen names the
	// conflict.
	moto.Click("Throttle Key");
	CHECK(moto.ReadOut("Menus", "Rebinding").GetBool());
	CHECK(moto.GetText("Settings Status") == "PRESS A KEY FOR THROTTLE   ESC CANCELS");
	moto.Press("E");
	moto.Advance(1);
	CHECK_FALSE(moto.ReadOut("Menus", "Rebinding").GetBool());
	CHECK(moto.GetText("Settings Status") == "THROTTLE: E   ALSO BOUND TO SHIFTUP");
	CHECK(moto.GetText("Throttle Key Text") == "E");
	CHECK(moto.ReadOut("Menus", "Conflicts").GetString() == "SHIFTUP");

	// The throttle's gamepad button refuses a key and listens again; Escape ends it with the trigger kept, and pauses nothing.
	moto.Click("Throttle Pad");
	moto.Press("T");
	moto.Advance(1);
	CHECK(moto.ReadOut("Menus", "Rebinding").GetBool());
	CHECK(moto.GetText("Settings Status") == "THE PAD TAKES ITS OWN CONTROLS   PRESS A PAD CONTROL FOR THROTTLE   ESC CANCELS");
	moto.Press("Escape");
	moto.Advance(1);
	CHECK_FALSE(moto.ReadOut("Menus", "Rebinding").GetBool());
	CHECK(moto.GetText("Settings Status") == "THROTTLE KEPT: PAD RIGHTTRIGGER");
	CHECK_FALSE(moto.ReadOut("Race", "Paused").GetBool());

	// B leaves: the settings are saved and the title shows again.
	moto.PressPad("B");
	moto.Advance(1);
	CHECK(moto.ReadOut("Menus", "Screen").GetString() == "Title");
	CHECK(moto.ReadOut("Menus", "Saves").GetInt32() == 1);
	moto.Stop();

	// Both are kept in play mode's game data folder, beside the copy.
	std::filesystem::path const data = moto.GetDirectory() / ".strada" / "PlayData";
	PlayerSettingsRead const settings = ReadPlayerSettingsFile(data / "Settings.json", PlayerSettings());
	CHECK(settings.Status == PlayerSettingsStatus::Present);
	CHECK(settings.Settings.Quality == QualityPreset::Medium);
	CHECK(settings.Settings.Window.Mode == PlayerWindowMode::Fullscreen);
	Result<std::string> const profile = FileSystem::ReadTextFile(data / "Input" / "Moto.sinput.json");
	REQUIRE(profile.IsOk());
	CHECK(profile.GetValue().find("\"Key/E\"") != std::string::npos);

	// Played again, the settings screen reads them back.
	moto.Start();
	moto.Advance(3);
	CHECK(moto.GetText("Quality Value") == "MEDIUM");
	CHECK(moto.GetText("Window Value") == "FULLSCREEN");
	CHECK(moto.GetText("Throttle Key Text") == "E");
	CHECK(moto.GetText("Throttle Pad Text") == "PAD RIGHTTRIGGER");
	// The kept linked brakes' setting reaches the bike as the scene starts: the menus give it to their rider.
	CHECK(moto.GetText("Linked Brakes Value") == "OFF");
	CHECK_FALSE(moto.ReadOut("Bike", "LinkedBrakes").GetBool());

	// The mouse opens the settings, and steps the window back to a window. The lean's keyboard binding is two keys: it listens for the
	// left, then the right; a pad button given for the right is refused, the binding given back with its new left key, and the right asked
	// again.
	moto.Click("Settings Button");
	REQUIRE(moto.ReadOut("Menus", "Screen").GetString() == "Settings");
	moto.Click("Window Lower");
	CHECK(moto.GetText("Window Value") == "WINDOWED");
	moto.Click("Lean Key");
	CHECK(moto.GetText("Settings Status") == "PRESS A KEY FOR LEAN LEFT   ESC CANCELS");
	moto.Press("J");
	moto.Advance(1);
	CHECK(moto.GetText("Settings Status") == "PRESS A KEY FOR LEAN RIGHT   ESC CANCELS");
	moto.PressPad("Y");
	moto.Advance(1);
	CHECK(moto.GetText("Settings Status") == "THE KEYBOARD TAKES KEYS   PRESS A KEY FOR LEAN RIGHT   ESC CANCELS");
	CHECK(moto.ReadOut("Menus", "Rebinding").GetBool());
	moto.Press("L");
	moto.Advance(1);
	CHECK_FALSE(moto.ReadOut("Menus", "Rebinding").GetBool());
	CHECK(moto.GetText("Settings Status") == "LEAN: J L");
	CHECK(moto.GetText("Lean Key Text") == "J L");

	// RESET CONTROLS gives every binding back as the project's actions have it.
	moto.Click("Reset Controls Button");
	CHECK(moto.GetText("Settings Status") == "CONTROLS RESET");
	CHECK(moto.GetText("Throttle Key Text") == "W");
	CHECK(moto.GetText("Lean Key Text") == "A D");
	moto.PressPad("B");
	moto.Advance(1);
	REQUIRE(moto.ReadOut("Menus", "Screen").GetString() == "Title");

	// Escape pauses: the pause screen shows while time stands still, and the mouse clicks its RESUME.
	moto.Press("Escape");
	moto.Advance(1);
	REQUIRE(moto.ReadOut("Menus", "Screen").GetString() == "Pause");
	CHECK(moto.IsShown("Pause Screen"));
	CHECK(moto.ReadOut("Race", "Paused").GetBool());
	moto.Click("Resume Button");
	moto.Advance(1);
	CHECK_FALSE(moto.ReadOut("Race", "Paused").GetBool());
	CHECK(moto.ReadOut("Menus", "Screen").GetString() == "Title");
	CHECK_FALSE(moto.IsShown("Pause Screen"));
	moto.Stop();
	PlayerSettingsRead const stepped = ReadPlayerSettingsFile(data / "Settings.json", PlayerSettings());
	CHECK(stepped.Status == PlayerSettingsStatus::Present);
	CHECK(stepped.Settings.Window.Mode == PlayerWindowMode::Windowed);
}

TEST_CASE("MotoPlay: a live edit of the bike's shift point moves the HUD's shift light from the next frame")
{
	// The race director without its title, as the test scenes start it, so that the HUD shows its riding group: the bike
	// waits on the grid in neutral at its idle speed, far below the definition's shift point, and the light is off.
	MotoPlay moto;
	moto.SetVariable("Race", "TitleOnStart", ScriptFieldValue::FromBool(false));
	moto.Start();
	moto.Advance(3);
	REQUIRE(moto.ReadOut("Race", "State").GetString() == "Idle");
	REQUIRE(moto.IsShown("Riding Plates"));
	auto const lit = [&moto]
	{
		glm::vec4 const color = moto.Find("Shift Light").GetComponent<UIImageComponent>().Color;
		CAPTURE(color.r);
		CHECK((color.r > 0.9f || color.r < 0.3f));
		return color.r > 0.9f;
	};
	Json const vehicles = moto.Ok("vehicle.state")["vehicles"];
	REQUIRE(vehicles.size() == 1);
	float const idle = vehicles[0]["engineSpeed"].get<float>();
	CAPTURE(idle);
	Json const transmission = moto.Ok("vehicle.get", Json::object({{"vehicle", Definition}}))["fields"]["Transmission"];
	REQUIRE(idle > 1200.0f);
	REQUIRE(transmission["ShiftUpRPM"].get<float>() > idle);
	CHECK_FALSE(lit());

	// vehicle.set moves the shift point below the idle speed while the game plays (an agent's "shift later" the other way):
	// the HUD reads the definition every frame, so the light is on from the next frame, and off again once it is back.
	Json lowered = transmission;
	lowered["ShiftDownRPM"] = 1000.0;
	lowered["ShiftUpRPM"] = 1200.0;
	moto.Ok("vehicle.set", Json::object({{"vehicle", Definition}, {"fields", Json::object({{"Transmission", lowered}})}}));
	moto.Advance(2);
	CHECK(lit());
	moto.Ok("vehicle.set", Json::object({{"vehicle", Definition}, {"fields", Json::object({{"Transmission", transmission}})}}));
	moto.Advance(2);
	CHECK_FALSE(lit());
	moto.Stop();
}

TEST_CASE("MotoPlay: the race director's AttractStarted puts the camera director in its attract mode on a trackside camera, and "
          "its RaceStarted cuts it to the chase view behind the bike")
{
	// The main scene as the game opens it: the race director on its title, which broadcasts AttractStarted, and the camera
	// director (Tools/Trackside.py) cutting by itself (its Directs on): its Mode 1, a trackside camera (1 to 7) the one
	// that shows and hears the scene.
	MotoPlay moto;
	moto.Start();
	moto.Advance(3);
	REQUIRE(moto.ReadOut("Race", "State").GetString() == "Title");
	auto const shows = [&moto](std::string_view name)
	{
		Entity const camera = moto.Find(name);
		return camera.GetComponent<CameraComponent>().Primary && camera.GetComponent<AudioListenerComponent>().Active;
	};
	CHECK(moto.ReadOut("Trackside", "Mode").GetInt32() == 1);
	int32_t const attract = moto.ReadOut("Trackside", "Active").GetInt32();
	REQUIRE(attract >= 1);
	REQUIRE(attract <= 7);
	// Active is the camera's place among the director's children (the trackside cameras in the lap's order, then the chase
	// camera): that camera alone shows and hears the scene.
	Entity const director = moto.Find("Trackside");
	std::vector<UUID> const cameras = director.GetComponent<RelationshipComponent>().Children;
	REQUIRE(cameras.size() >= static_cast<size_t>(attract));
	Entity const attractCamera = director.GetScene()->GetEntityByUUID(cameras[static_cast<size_t>(attract) - 1]);
	REQUIRE(attractCamera);
	CHECK(shows(attractCamera.GetName()));
	CHECK_FALSE(shows("Main Camera"));
	CHECK_FALSE(shows("Chase Camera"));

	// Enter presses the title's RACE: the countdown, in which the attract mode goes on; at its end the director broadcasts
	// RaceStarted, and the camera director cuts to the chase camera (its Active 8), which alone shows and hears the scene.
	moto.Press("Enter");
	moto.Advance(1);
	REQUIRE(moto.ReadOut("Race", "State").GetString() == "Countdown");
	CHECK(moto.ReadOut("Trackside", "Mode").GetInt32() == 1);
	// The countdown is 3 s of fixed steps: 180 frames at 1/60 s, with room.
	for (int32_t frame = 0; frame < 240 && moto.ReadOut("Race", "State").GetString() == "Countdown"; frame++)
	{
		moto.Advance(1);
	}
	REQUIRE(moto.ReadOut("Race", "State").GetString() == "Racing");
	moto.Advance(2);
	CHECK(moto.ReadOut("Trackside", "Mode").GetInt32() == 2);
	CHECK(moto.ReadOut("Trackside", "Active").GetInt32() == 8);
	CHECK(shows("Chase Camera"));
	CHECK_FALSE(shows("Main Camera"));
	moto.Stop();
}
