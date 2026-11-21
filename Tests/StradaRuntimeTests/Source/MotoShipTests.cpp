#include "Core/UserDataTestUtilities.h"
#include "RuntimeTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Platform/Process.h"
#include "Strada/Project/GameExporter.h"
#include "Strada/Project/Project.h"
#include "Strada/Renderer/QualityPresets.h"
#include "Strada/Script/PlayerSettings.h"
#include "Strada/Script/SaveSlots.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;
using namespace Strada::RuntimeTesting;

// The Moto sample as it ships, played by the player at the frame rates a player's machine runs it at: its test scene's
// pinned checks at 1/30 and 1/144 s a frame (SampleTests.cpp plays them at 1/60), and the physics state of the first
// minute of a ride from the grid, every fixed step of it, the same at 1/30, 1/60 and 1/144 s a frame; and as its players
// get it, exported as "Strada Moto": the same pinned checks, and what the game keeps for its player across two runs in its
// own data folder (Docs/Architecture.md section 15, "Moto shipped"). A ctest entry of its own (StradaRuntimeTests.Ship,
// Shards.cmake): its test runs are on scenes without the props, and only the two runs that keep the player's data start
// the main scene.

namespace
{
	// The exported game's product and no company (Moto.sproj, the identity part of Tools/Author.py): its data folder's name.
	constexpr char const* ShippedProduct = "Strada Moto";
	// Tests.sscene's checks (Tools/Checks.py), as SampleTests.cpp pins them at 1/60 s a frame.
	constexpr std::string_view TestChecks = "Tests passed: 24 checks";
	// A frame of 1/144 s runs a fixed step only now and then, so a run's frames are 2.4 times those at 1/60 s: on a slow
	// runner a Debug run can pass RunTimeout's two minutes, so these runs have a process timeout of their own.
	constexpr std::chrono::seconds ShipRunTimeout = std::chrono::minutes(5);
	// The ride's minute: 3,600 fixed steps of the project's 1/60 s step. The run's timeout gives it a quarter of a second
	// more of the game (15 steps), since 8,640 frames of the player's 1/144 s, a float a frame, may fall short of step 3,600.
	constexpr std::string_view RideStep = "3600";
	constexpr std::string_view RideTimeout = "60.25";

	// The test run's summary line, passed or failed, to its end; empty without one.
	std::string_view SummaryLine(std::string_view output)
	{
		size_t start = output.find("Tests passed: ");
		if (start == std::string_view::npos)
		{
			start = output.find("Tests failed: ");
		}
		if (start == std::string_view::npos)
		{
			return {};
		}
		size_t const end = output.find_first_of("\r\n", start);
		return output.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
	}

	// The summary's physics state, from "physics state" to its end (the fold included); empty without one.
	std::string_view PhysicsState(std::string_view output)
	{
		std::string_view const summary = SummaryLine(output);
		size_t const start = summary.find("; physics state ");
		return start == std::string_view::npos ? std::string_view() : summary.substr(start + 2);
	}

	// A copy of Moto exported as its players get it: the player, renamed after the game, and the folder that holds its
	// Game.sgame and its Scripts (the directory itself, or the bundle's Resources on macOS).
	struct ShippedGame
	{
		std::filesystem::path Player;
		std::filesystem::path GameDirectory;
	};

	// Exports the open project into the directory, as File > Build Game does from its saved files.
	ShippedGame ExportOpenProject(Project const& project, std::filesystem::path const& directory)
	{
		Result<GameExportSettings> prepared = GameExporter::Prepare(project, directory);
		REQUIRE_MESSAGE(prepared.IsOk(), (prepared ? std::string() : prepared.GetError()));
		// A game of graphs alone: no script project to build, the scripting API to carry.
		CHECK(prepared.GetValue().UsesVisualScripts);
		CHECK(prepared.GetValue().ScriptProjectFile.empty());
		Result<GameExportResult> exported = GameExporter::Export(prepared.GetValue());
		REQUIRE_MESSAGE(exported.IsOk(), (exported ? std::string() : exported.GetError()));
		REQUIRE(exported.GetValue().Succeeded);
		ShippedGame game{exported.GetValue().Executable, directory};
		// Named after the game's Name, which is "Strada Moto" as its product is.
		CHECK(FileSystem::PathToUtf8(game.Player.stem()) == "Strada Moto");
		if (game.Player.extension() == ".app")
		{
			game.GameDirectory = game.Player / "Contents" / "Resources";
			game.Player = game.Player / "Contents" / "MacOS" / game.Player.stem();
		}
		return game;
	}

	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object(), Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	// The names of the game this reads and writes: Moto's input action, its mixer's bus, the race director's save slot and
	// its value (Tools/Race.py, SLOT), and the director's read-out of the best lap.
	constexpr char const* ThrottleAction = "Throttle";
	constexpr char const* EngineBus = "Engine";
	constexpr char const* BestLapSlot = "BestLap";
	constexpr char const* BestLapTime = "BestLapTime";

	// As the game shows its title (five frames after it starts, once the race director has read its slot): log what the
	// player's settings, the throttle's first binding, the race director and the title read, then do what the settings
	// screen and the race's results do: the quality Low, the Engine bus at -6 dB, the throttle on T, the settings saved, and
	// a best lap of 83.25 s saved in the director's slot. Reading comes first, so a run reads only what an earlier run kept.
	VisualScriptDocument MakeKeepingGraph(uint32_t bestLapTime, AssetHandle director)
	{
		VisualScriptDocument document;
		VisualScriptVariable slot;
		slot.ID = 1;
		slot.Name = "Slot";
		slot.Type = "Strada.SaveSlot";
		document.Variables.push_back(slot);
		std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		nodes.push_back(MakeNode(10, "Event.Create"));
		nodes.push_back(MakeNode(11, "Flow.WaitFrames", Json::object(), Json::object({{"Frames", 5}})));
		nodes.push_back(MakeNode(12, "FormatString",
		                         Json::object({{"Format", "Strada Moto reads quality {Quality}, the Engine bus at {Engine} dB, the "
		                                                  "throttle on {Throttle}, a best lap of {Best} s and '{Title}'"}})));
		nodes.push_back(MakeNode(13, "Get:Strada.Settings.Quality"));
		nodes.push_back(MakeNode(14, "Call:Strada.Settings.GetBusVolume(string)", Json::object(), Json::object({{"Bus", EngineBus}})));
		nodes.push_back(MakeNode(15, "Call:Strada.Input.GetBindingControls(string,int)", Json::object(),
		                         Json::object({{"Action", ThrottleAction}, {"Binding", 0}})));
		nodes.push_back(MakeNode(16, "Call:Strada.Entity.FindByName(string)", Json::object(), Json::object({{"Name", "Race"}})));
		nodes.push_back(MakeNode(17, "Variable.Get", Json::object({{"Variable", bestLapTime}, {"Graph", director.ToString()}})));
		nodes.push_back(MakeNode(18, "Call:Strada.Entity.FindByName(string)", Json::object(), Json::object({{"Name", "Title Best"}})));
		nodes.push_back(MakeNode(19, "Get:Strada.UITextComponent.Text"));
		nodes.push_back(MakeNode(20, "Call:Strada.Log.Info(string)"));
		nodes.push_back(MakeNode(21, "Set:Strada.Settings.Quality", Json::object(), Json::object({{"Value", "Low"}})));
		nodes.push_back(MakeNode(22, "Call:Strada.Settings.SetBusVolume(string,float)", Json::object(),
		                         Json::object({{"Bus", EngineBus}, {"Decibels", -6.0}})));
		nodes.push_back(MakeNode(23, "Call:Strada.Input.Rebind(string,int,string,int)", Json::object(),
		                         Json::object({{"Action", ThrottleAction}, {"Binding", 0}, {"Controls", "Key/T"}})));
		nodes.push_back(MakeNode(24, "Call:Strada.Settings.Save()"));
		nodes.push_back(MakeNode(25, "Call:Strada.SaveSlot.Open(string)", Json::object(), Json::object({{"Name", BestLapSlot}})));
		nodes.push_back(MakeNode(26, "Variable.Set", Json::object({{"Variable", 1}})));
		nodes.push_back(MakeNode(27, "Variable.Get", Json::object({{"Variable", 1}})));
		nodes.push_back(MakeNode(28, "Call:Strada.SaveSlot.Set<T>(string,T)", Json::object({{"TypeArguments", Json::array({"float"})}}),
		                         Json::object({{"Name", "LapTime"}, {"Value", 83.25}})));
		nodes.push_back(MakeNode(29, "Call:Strada.SaveSlot.Save()"));
		for (auto const& [from, to] :
		     std::vector<std::pair<uint32_t, uint32_t>>{{21, 22}, {22, 23}, {23, 24}, {24, 25}, {25, 26}, {26, 28}, {28, 29}})
		{
			links.push_back({{from, "Then"}, {to, "Execute"}});
		}
		links.push_back({{10, "Then"}, {11, "Execute"}});
		links.push_back({{11, "Completed"}, {20, "Execute"}});
		links.push_back({{20, "Then"}, {21, "Execute"}});
		links.push_back({{13, "Value"}, {12, "Quality"}});
		links.push_back({{14, "Result"}, {12, "Engine"}});
		links.push_back({{15, "Result"}, {12, "Throttle"}});
		links.push_back({{16, "Result"}, {17, "Target"}});
		links.push_back({{17, "Value"}, {12, "Best"}});
		links.push_back({{18, "Result"}, {19, "Target"}});
		links.push_back({{19, "Value"}, {12, "Title"}});
		links.push_back({{12, "Result"}, {20, "Message"}});
		links.push_back({{25, "Result"}, {26, "Value"}});
		links.push_back({{27, "Value"}, {28, "Target"}});
		links.push_back({{27, "Value"}, {29, "Target"}});
		return document;
	}

	// The copy's main scene with one more root entity, Kept Progress, that runs the graph: edited as its file's JSON, since
	// loading the scene of 10,000 props to add one entity would take longer than the run.
	void AddKeepingEntity(std::filesystem::path const& scenePath, AssetHandle graph)
	{
		Result<std::string> text = FileSystem::ReadTextFile(scenePath);
		REQUIRE(text.IsOk());
		Result<Json> parsed = ParseJson(text.GetValue());
		REQUIRE(parsed.IsOk());
		Json scene = parsed.TakeValue();
		std::string const keptID = "4242424242424242";
		for (Json const& entity : scene["Entities"])
		{
			REQUIRE(entity.value("ID", std::string()) != keptID);
		}
		scene["Entities"].push_back(
			Json::object({{"ID", keptID},
		                  {"Components",
		                   Json::object({{"Tag", Json::object({{"Tag", "Kept Progress"}})},
		                                 {"Transform", Json::object({{"Translation", Json::array({0.0, 0.0, 0.0})},
		                                                             {"Rotation", Json::array({0.0, 0.0, 0.0, 1.0})},
		                                                             {"Scale", Json::array({1.0, 1.0, 1.0})}})},
		                                 {"Relationship", Json::object({{"Parent", "0"}, {"Children", Json::array()}})},
		                                 {"VisualScript", Json::object({{"Graph", graph.ToString()}, {"Variables", Json::object()}})}})}}));
		REQUIRE(FileSystem::WriteTextFile(scenePath, DumpJson(scene)).IsOk());
	}

	// The folder the player logs that it gave the game; empty when it logged none.
	std::string LoggedDataFolder(std::string_view output)
	{
		std::string_view const prefix = "The game keeps its data in '";
		size_t const start = output.find(prefix);
		if (start == std::string_view::npos)
		{
			return {};
		}
		size_t const end = output.find('\'', start + prefix.size());
		return end == std::string_view::npos ? std::string()
		                                     : std::string(output.substr(start + prefix.size(), end - start - prefix.size()));
	}
}

TEST_CASE("Runtime: the Moto sample's test scene passes its pinned checks at 1/30 and at 1/144 s a frame")
{
	// Its checks wait in seconds of the game, so they pass at other frame rates too.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	Testing::IsolatedUserRoot const userRoot;
	for (char const* timestep : {"0.0333333", "0.00694444"})
	{
		CAPTURE(timestep);
		ProcessResult const run =
			RunPlayer({"--project", Native(project), "--scene", "Scenes/Tests.sscene", "--test", "--timeout", "90", "--timestep", timestep},
		              userRoot, ShipRunTimeout);
		CAPTURE(run.Output);
		CHECK(run.ExitCode == 0);
		CHECK(Contains(run.Output, TestChecks));
	}
}

TEST_CASE("Runtime: the first minute of Moto's ride from the grid has the same physics state at every fixed step at 1/30, 1/60 "
          "and 1/144 s a frame")
{
	// The lap test scene (Scenes/Lap.sscene: the main scene without its props, Tools/LapChecks.py) rides the bike on the
	// autopilot from the grid as the scene starts, its inputs set in its fixed steps from what the bike reads, never from a
	// frame. Its checks report after two whole laps, so a minute of it is a test run cut by its timeout: it ends unfinished
	// (exit code 1) with no check failed, and its summary gives the state hash of step 3,600 and the fold of every step's
	// hash from the first to it (--hash-every-step), compared exactly between the rates. A test run's game data folder is
	// temporary, so every run starts from no kept best lap and no settings.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	Testing::IsolatedUserRoot const userRoot;
	std::string expected;
	for (char const* timestep : {"0.0166666667", "0.0333333333", "0.0069444444"})
	{
		CAPTURE(timestep);
		ProcessResult const run =
			RunPlayer({"--project", Native(project), "--scene", "Scenes/Lap.sscene", "--test", "--timeout", std::string(RideTimeout),
		               "--timestep", timestep, "--hash-step", std::string(RideStep), "--hash-every-step"},
		              userRoot, ShipRunTimeout);
		CAPTURE(run.Output);
		CHECK(run.ExitCode == 1);
		std::string_view const summary = SummaryLine(run.Output);
		CHECK(summary.starts_with("Tests failed: 0 of "));
		CHECK(Contains(summary, " failed, 0 script exceptions; the scripts did not finish testing in "));
		std::string const state(PhysicsState(run.Output));
		REQUIRE(Contains(state, std::string(" at step ") + std::string(RideStep) + "; steps 1 to " + std::string(RideStep) + " folded "));
		if (expected.empty())
		{
			expected = state;
		}
		CHECK(state == expected);
	}
}

TEST_CASE("Runtime: Moto exported as Strada Moto carries the scripting API and no game assembly, passes its pinned checks and "
          "logs below its own data folder")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	ShippedGame game;
	{
		Testing::AssetManagerScope assets;
		Result<Ref<Project>> opened = Project::Open(project);
		REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
		game = ExportOpenProject(*opened.GetValue(), directory.GetPath() / "Export");
	}

	// A game of graphs alone names the game assembly it would have (Scripts/Game.dll) and ships none: the player takes the
	// scripting API from that path's folder and loads no assembly where the file is absent.
	Result<std::string> const text = FileSystem::ReadTextFile(game.GameDirectory / FileSystem::PathFromUtf8(Project::GameFileName));
	REQUIRE(text.IsOk());
	Result<Json> const parsed = ParseJson(text.GetValue());
	REQUIRE(parsed.IsOk());
	Json const& settings = parsed.GetValue()["Game"];
	CHECK(settings["Name"] == "Strada Moto");
	CHECK(settings["Identity"]["Product"] == "Strada Moto");
	CHECK(settings["Identity"]["Company"] == "");
	CHECK(settings["ScriptModule"] == "Scripts/Game.dll");
	CHECK(FileSystem::IsRegularFile(game.GameDirectory / "Scripts" / "Strada.ScriptCore.dll"));
	CHECK_FALSE(FileSystem::Exists(game.GameDirectory / "Scripts" / "Game.dll"));

	// The exported game's test run passes the test scene's pinned checks, logging into its own folder below the per-user
	// root; its game data folder is a test run's, temporary, so it keeps nothing there.
	Testing::IsolatedUserRoot const userRoot;
	ProcessResult const tests =
		RunProgram(game.Player, {"--test", "--scene", "Scenes/Tests.sscene", "--timeout", "90"}, userRoot, ShipRunTimeout);
	CAPTURE(tests.Output);
	CHECK(tests.ExitCode == 0);
	CHECK(Contains(tests.Output, TestChecks));
	CHECK_FALSE(Contains(tests.Output, "The game has no scripts"));
	std::filesystem::path const gameData = userRoot.GetGameDirectory(GameIdentity{"", ShippedProduct});
	std::filesystem::path const log = gameData / "Logs" / FileSystem::PathFromUtf8(FileSystem::PathToUtf8(game.Player.stem()) + ".log");
	REQUIRE(FileSystem::IsRegularFile(log));
	Result<std::string> const logged = FileSystem::ReadTextFile(log);
	REQUIRE(logged.IsOk());
	CHECK(Contains(logged.GetValue(), TestChecks));
	CHECK_FALSE(FileSystem::Exists(gameData / "Settings.json"));
	CHECK_FALSE(FileSystem::Exists(gameData / "Saves"));
}

TEST_CASE("Runtime: exported Strada Moto keeps its player's quality, a bus volume, the throttle's binding and a best lap in "
          "its own data folder, and its next run reads them back")
{
	// A graph added to a copy's main scene (Kept Progress) changes the three settings and saves a lap, as the settings screen
	// and the race's results do; the next run reads them through the player (its settings and bindings), the race director
	// (the slot) and the title. Both runs share one per-user root, as two runs on a player's machine do; neither is a test
	// run, whose data folder is removed.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	ShippedGame game;
	{
		Testing::AssetManagerScope assets;
		Result<Ref<Project>> opened = Project::Open(project);
		REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
		AssetHandle const director = AssetManager::FindByPath("Scripts/RaceDirector.sgraph");
		REQUIRE(director.IsValid());
		Result<VisualScriptDocument> const directorGraph = VisualScriptSerializer::LoadFromFile(
			AssetManager::GetAssetDirectory() / "Scripts" / "RaceDirector.sgraph", AssetManager::CreateDeserializationContext());
		REQUIRE(directorGraph.IsOk());
		uint32_t bestLapTime = 0;
		for (VisualScriptVariable const& variable : directorGraph.GetValue().Variables)
		{
			if (variable.Name == BestLapTime)
			{
				CHECK(variable.Public);
				bestLapTime = variable.ID;
			}
		}
		REQUIRE(bestLapTime != 0);
		std::filesystem::path const graphFile = AssetManager::GetAssetDirectory() / "Scripts" / "KeptProgress.sgraph";
		REQUIRE(VisualScriptSerializer::SaveToFile(MakeKeepingGraph(bestLapTime, director), graphFile).IsOk());
		Result<AssetHandle> const graph = AssetManager::ImportFile(graphFile);
		REQUIRE(graph.IsOk());
		REQUIRE(AssetManager::SaveRegistry().IsOk());
		AddKeepingEntity(AssetManager::GetAssetDirectory() / "Scenes" / "Main.sscene", graph.GetValue());
		game = ExportOpenProject(*opened.GetValue(), directory.GetPath() / "Export");
	}

	Testing::IsolatedUserRoot const userRoot;
	std::filesystem::path const gameData = userRoot.GetGameDirectory(GameIdentity{"", ShippedProduct});
	// Thirty frames: the graph reads at the fifth and writes at once.
	std::vector<std::string> const play = {"--headless", "--frames", "30"};
	ProcessResult const first = RunProgram(game.Player, play, userRoot);
	CAPTURE(first.Output);
	CHECK(first.ExitCode == 0);
	CHECK(FileSystem::PathFromUtf8(LoggedDataFolder(first.Output)).lexically_normal() == gameData.lexically_normal());
	CHECK(Contains(first.Output, "the game's defaults"));
	CHECK(Contains(first.Output, "Strada Moto reads quality High, the Engine bus at 0 dB, the throttle on Key/W, a best lap of 0 s "
	                             "and 'NO BEST LAP YET'"));
	CHECK(Contains(first.Output, "Input bindings: binding 0 of 'Throttle' reads 'Key/T' (it read 'Key/W')"));
	std::filesystem::path const settingsFile = gameData / "Settings.json";
	std::filesystem::path const profile = gameData / "Input" / "Moto.sinput.json";
	Result<std::string> const stem = SaveSlotStore::MakeFileStem(BestLapSlot);
	REQUIRE(stem.IsOk());
	std::filesystem::path const slotFile = gameData / "Saves" / FileSystem::PathFromUtf8(stem.GetValue() + ".ssave");
	CHECK(FileSystem::IsRegularFile(settingsFile));
	CHECK(FileSystem::IsRegularFile(profile));
	CHECK(FileSystem::IsRegularFile(slotFile));

	ProcessResult const second = RunProgram(game.Player, play, userRoot);
	CAPTURE(second.Output);
	CHECK(second.ExitCode == 0);
	CHECK(Contains(second.Output, "Player settings: quality Low"));
	CHECK(Contains(second.Output, ", 1 bus, from Settings.json"));
	CHECK(Contains(second.Output, "Input bindings: 1 changed binding from"));
	CHECK(Contains(second.Output, "Strada Moto reads quality Low, the Engine bus at -6 dB, the throttle on Key/T, a best lap of "
	                              "83.25 s and 'BEST LAP 1:23.250'"));
	// Nothing had anything to say about the slot, the settings or the binding written again.
	CHECK_FALSE(Contains(second.Output, "SaveSlot."));
	CHECK_FALSE(Contains(second.Output, "Settings.Save:"));
	CHECK_FALSE(Contains(second.Output, "reads 'Key/T' (it read"));
	CHECK_FALSE(Contains(second.Output, ".sgraph:"));

	// What the second run left in the game's folder.
	PlayerSettingsRead const settings = ReadPlayerSettingsFile(settingsFile, {});
	REQUIRE(settings.Status == PlayerSettingsStatus::Present);
	CHECK(settings.Settings.Quality == QualityPreset::Low);
	CHECK(settings.Settings.Buses == std::vector<PlayerBusSettings>{{EngineBus, -6.0f, false}});
	Result<std::string> const bindings = FileSystem::ReadTextFile(profile);
	REQUIRE(bindings.IsOk());
	CHECK(Contains(bindings.GetValue(), R"("Controls": "Key/T")"));
	CHECK(Contains(bindings.GetValue(), R"("Replaced": "Key/W")"));
	Result<std::string> const slot = FileSystem::ReadTextFile(slotFile);
	REQUIRE(slot.IsOk());
	SaveSlotRead const read = ReadSaveSlotText(slot.GetValue());
	REQUIRE(read.Status == SaveSlotStatus::Present);
	CHECK(read.Data.Values == ScriptFieldMap{{"LapTime", ScriptFieldValue::FromFloat(83.25f)}});
}
