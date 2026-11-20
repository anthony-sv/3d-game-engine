#include "Core/UserDataTestUtilities.h"
#include "GpuTestUtilities.h"
#include "RuntimeTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/MixerAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Image.h"
#include "Strada/Platform/Process.h"
#include "Strada/Project/GameExporter.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ProjectSettings.h"
#include "Strada/Renderer/QualityPresets.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/PlayerSettings.h"
#include "Strada/Script/SaveSlots.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;
using namespace Strada::RuntimeTesting;

// What a game keeps for its player between runs (Docs/Architecture.md §13.1): the player's settings, read from the folder
// the run keeps its data in, before the window opens and when the game starts, and never by a run whose folder is
// temporary.
namespace
{
	// A game of one scene seen by a camera, without scripts, in a temporary directory: as an exported game (Game.sgame) or
	// a project, of the window given.
	class SettingsGame
	{
	public:
		SettingsGame(ProjectFileKind kind, uint32_t width, uint32_t height)
		{
			std::filesystem::path const scenes = GetDirectory() / "Assets" / "Scenes";
			REQUIRE(FileSystem::CreateDirectories(scenes).IsOk());
			REQUIRE(SceneSerializer::SaveToFile(*MakeScene("Main", std::nullopt), scenes / "Main.sscene").IsOk());
			ProjectSettings settings;
			settings.Name = "Kept Game";
			settings.Window.Width = width;
			settings.Window.Height = height;
			{
				Testing::AssetManagerScope assets;
				REQUIRE(AssetManager::OpenAssetDirectory(GetDirectory() / "Assets").IsOk());
				settings.StartScene = AssetManager::FindByPath("Scenes/Main.sscene");
				REQUIRE(settings.StartScene.IsValid());
			}
			m_File = GetDirectory() / FileSystem::PathFromUtf8(kind == ProjectFileKind::Game ? Project::GameFileName : "Kept Game.sproj");
			REQUIRE(FileSystem::WriteTextFile(m_File, DumpJson(Project::Serialize(settings, kind))).IsOk());
		}

		std::filesystem::path const& GetDirectory() const { return m_Directory.GetPath(); }
		std::string GetFile() const { return Native(m_File); }
		// A development run's game data folder.
		std::filesystem::path GetPlayData() const { return (GetDirectory() / ".strada" / "PlayData").lexically_normal(); }

	private:
		Testing::TemporaryDirectory m_Directory;
		std::filesystem::path m_File;
	};

	void WriteSettings(std::filesystem::path const& folder, PlayerSettings const& settings)
	{
		Result<std::string> const text = WritePlayerSettingsText(settings);
		REQUIRE(text.IsOk());
		REQUIRE(FileSystem::WriteTextFile(folder / "Settings.json", text.GetValue()).IsOk());
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

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		return variable;
	}

	// An exported game whose start scene runs one graph, with a mixer of a Music bus below Master: Game.sgame, its player and
	// its identity.
	class ExportedGraphGame
	{
	public:
		explicit ExportedGraphGame(VisualScriptDocument const& graph)
		{
			Testing::AssetManagerScope assets;
			Result<Ref<Project>> created =
				Project::Create(m_Directory.GetPath() / "Project", "Kept Progress", *MakeScene("Main", std::nullopt));
			REQUIRE(created.IsOk());
			Ref<Project> const project = created.GetValue();
			m_Identity = GetGameIdentity(project->GetSettings());

			MixerBus music;
			music.Name = "Music";
			Mixer mixer;
			mixer.Buses.push_back(music);
			TypedDataAssetType<Mixer> const* type = DataAssetRegistry::Find<Mixer>();
			REQUIRE(type != nullptr);
			Ref<TypedDataAsset<Mixer>> const asset = std::static_pointer_cast<TypedDataAsset<Mixer>>(type->CreateAsset());
			REQUIRE(asset->SetData(mixer).IsOk());
			std::filesystem::path const mixerFile = project->GetAssetDirectory() / "Audio" / "Game.smixer";
			REQUIRE(FileSystem::CreateDirectories(mixerFile.parent_path()).IsOk());
			REQUIRE(DataAssetSerializer::SaveToFile(*asset, mixerFile).IsOk());
			Result<AssetHandle> const imported = AssetManager::ImportFile(mixerFile);
			REQUIRE(imported.IsOk());

			std::filesystem::path const graphFile = project->GetAssetDirectory() / "Scripts" / "Progress.sgraph";
			REQUIRE(FileSystem::CreateDirectories(graphFile.parent_path()).IsOk());
			REQUIRE(VisualScriptSerializer::SaveToFile(graph, graphFile).IsOk());
			Result<AssetHandle> const graphAsset = AssetManager::ImportFile(graphFile);
			REQUIRE(graphAsset.IsOk());
			REQUIRE(AssetManager::SaveRegistry().IsOk());
			REQUIRE(project->ApplySettings(Json::object({{"Mixer", imported.GetValue().ToString()}})).IsOk());
			REQUIRE(project->Save().IsOk());

			Ref<Scene> const main = MakeScene("Main", std::nullopt);
			main->CreateEntity("Progress").AddComponent<VisualScriptComponent>().Graph = graphAsset.GetValue();
			REQUIRE(SceneSerializer::SaveToFile(*main, project->GetAssetDirectory() / "Scenes" / "Main.sscene").IsOk());

			Result<GameExportSettings> prepared = GameExporter::Prepare(*project, m_Directory.GetPath() / "Export");
			REQUIRE_MESSAGE(prepared.IsOk(), (prepared ? std::string() : prepared.GetError()));
			Result<GameExportResult> exported = GameExporter::Export(prepared.GetValue());
			REQUIRE_MESSAGE(exported.IsOk(), (exported ? std::string() : exported.GetError()));
			REQUIRE(exported.GetValue().Succeeded);
			m_Player = exported.GetValue().Executable;
			if (m_Player.extension() == ".app")
			{
				m_Player = m_Player / "Contents" / "MacOS" / m_Player.stem();
			}
		}

		std::filesystem::path const& GetPlayer() const { return m_Player; }
		GameIdentity const& GetIdentity() const { return m_Identity; }
		std::filesystem::path const& GetDirectory() const { return m_Directory.GetPath(); }

	private:
		Testing::TemporaryDirectory m_Directory;
		std::filesystem::path m_Player;
		GameIdentity m_Identity;
	};

	// As the game starts: open the slot Progress, log "Run <Runs>: quality <Quality>, music <Music> dB, slot <Status>" (Runs
	// from the slot, 0 without one; the quality and Music's volume from the player's settings), then save Runs + 1 in the slot,
	// set the quality Low and Music at -12 dB, and save the settings, as a results screen and a settings screen would.
	VisualScriptDocument MakeProgressGraph()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Slot", "Strada.SaveSlot"));
		document.Variables.push_back(MakeVariable(2, "Runs", "int"));
		std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		Json const typedInt = Json::object({{"TypeArguments", Json::array({"int"})}});
		nodes.push_back(MakeNode(10, "Event.Create"));
		nodes.push_back(MakeNode(11, "Call:Strada.SaveSlot.Open(string)", Json::object(), Json::object({{"Name", "Progress"}})));
		nodes.push_back(MakeNode(12, "Variable.Set", Json::object({{"Variable", 1}})));
		nodes.push_back(MakeNode(13, "Variable.Get", Json::object({{"Variable", 1}})));
		nodes.push_back(MakeNode(14, "Call:Strada.SaveSlot.Get<T>(string,T)", typedInt, Json::object({{"Name", "Runs"}, {"Fallback", 0}})));
		nodes.push_back(MakeNode(15, "Variable.Set", Json::object({{"Variable", 2}})));
		nodes.push_back(MakeNode(16, "Variable.Get", Json::object({{"Variable", 2}})));
		nodes.push_back(
			MakeNode(17, "FormatString", Json::object({{"Format", "Run {Runs}: quality {Quality}, music {Music} dB, slot {Status}"}})));
		nodes.push_back(MakeNode(18, "Get:Strada.Settings.Quality"));
		nodes.push_back(MakeNode(19, "Call:Strada.Settings.GetBusVolume(string)", Json::object(), Json::object({{"Bus", "Music"}})));
		nodes.push_back(MakeNode(20, "Get:Strada.SaveSlot.Status"));
		nodes.push_back(MakeNode(21, "Call:Strada.Log.Info(string)"));
		nodes.push_back(MakeNode(22, "Operator.Add", Json::object(), Json::object({{"B", 1}})));
		nodes.push_back(MakeNode(23, "Call:Strada.SaveSlot.Set<T>(string,T)", typedInt, Json::object({{"Name", "Runs"}})));
		nodes.push_back(MakeNode(24, "Call:Strada.SaveSlot.Save()"));
		nodes.push_back(MakeNode(25, "Set:Strada.Settings.Quality", Json::object(), Json::object({{"Value", "Low"}})));
		nodes.push_back(MakeNode(26, "Call:Strada.Settings.SetBusVolume(string,float)", Json::object(),
		                         Json::object({{"Bus", "Music"}, {"Decibels", -12.0}})));
		nodes.push_back(MakeNode(27, "Call:Strada.Settings.Save()"));
		for (auto const& [from, to] : std::vector<std::pair<uint32_t, uint32_t>>{
				 {10, 11}, {11, 12}, {12, 15}, {15, 21}, {21, 23}, {23, 24}, {24, 25}, {25, 26}, {26, 27}})
		{
			links.push_back({{from, "Then"}, {to, "Execute"}});
		}
		links.push_back({{11, "Result"}, {12, "Value"}});
		links.push_back({{13, "Value"}, {14, "Target"}});
		links.push_back({{14, "Result"}, {15, "Value"}});
		links.push_back({{16, "Value"}, {17, "Runs"}});
		links.push_back({{18, "Value"}, {17, "Quality"}});
		links.push_back({{19, "Result"}, {17, "Music"}});
		links.push_back({{13, "Value"}, {20, "Target"}});
		links.push_back({{20, "Value"}, {17, "Status"}});
		links.push_back({{17, "Result"}, {21, "Message"}});
		links.push_back({{16, "Value"}, {22, "A"}});
		links.push_back({{13, "Value"}, {23, "Target"}});
		links.push_back({{22, "Result"}, {23, "Value"}});
		links.push_back({{13, "Value"}, {24, "Target"}});
		return document;
	}

	// As the game starts: unless the player's window is 400 x 100 already, ask for it (windowed, vsync off) and save the
	// settings, as a settings screen would. A game whose window is so asks for vsync alone when it is off, and saves it;
	// with vsync on it goes fullscreen and, five frames later, back into a window, saving nothing.
	VisualScriptDocument MakeWindowGraph()
	{
		VisualScriptDocument document;
		std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		nodes.push_back(MakeNode(10, "Event.Create"));
		nodes.push_back(MakeNode(11, "Get:Strada.Settings.WindowWidth"));
		nodes.push_back(MakeNode(12, "Operator.NotEqual", Json::object(), Json::object({{"B", 400}})));
		nodes.push_back(MakeNode(13, "Flow.Branch"));
		nodes.push_back(
			MakeNode(14, "Call:Strada.Settings.SetWindowSize(int,int)", Json::object(), Json::object({{"Width", 400}, {"Height", 100}})));
		nodes.push_back(MakeNode(15, "Set:Strada.Settings.WindowMode", Json::object(), Json::object({{"Value", "Windowed"}})));
		nodes.push_back(MakeNode(16, "Set:Strada.Settings.VSync", Json::object(), Json::object({{"Value", false}})));
		nodes.push_back(MakeNode(17, "Call:Strada.Settings.Save()"));
		nodes.push_back(MakeNode(18, "Get:Strada.Settings.VSync"));
		nodes.push_back(MakeNode(19, "Flow.Branch"));
		nodes.push_back(MakeNode(20, "Set:Strada.Settings.VSync", Json::object(), Json::object({{"Value", true}})));
		nodes.push_back(MakeNode(21, "Call:Strada.Settings.Save()"));
		nodes.push_back(MakeNode(22, "Set:Strada.Settings.WindowMode", Json::object(), Json::object({{"Value", "Fullscreen"}})));
		nodes.push_back(MakeNode(23, "Flow.WaitFrames", Json::object(), Json::object({{"Frames", 5}})));
		nodes.push_back(MakeNode(24, "Set:Strada.Settings.WindowMode", Json::object(), Json::object({{"Value", "Windowed"}})));
		links.push_back({{10, "Then"}, {13, "Execute"}});
		links.push_back({{11, "Value"}, {12, "A"}});
		links.push_back({{12, "Result"}, {13, "Condition"}});
		links.push_back({{13, "True"}, {14, "Execute"}});
		links.push_back({{14, "Then"}, {15, "Execute"}});
		links.push_back({{15, "Then"}, {16, "Execute"}});
		links.push_back({{16, "Then"}, {17, "Execute"}});
		links.push_back({{13, "False"}, {19, "Execute"}});
		links.push_back({{18, "Value"}, {19, "Condition"}});
		links.push_back({{19, "False"}, {20, "Execute"}});
		links.push_back({{20, "Then"}, {21, "Execute"}});
		links.push_back({{19, "True"}, {22, "Execute"}});
		links.push_back({{22, "Then"}, {23, "Execute"}});
		links.push_back({{23, "Completed"}, {24, "Execute"}});
		return document;
	}

	// The folder the player logs that it gave the game; empty when it logged none.
	std::string GetLoggedDataFolder(std::string_view output)
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

TEST_CASE("SaveGame: a game reads the player's settings where it keeps its data, and a --temporary-data run reads none and keeps "
          "none")
{
	PlayerSettings chosen;
	chosen.Quality = QualityPreset::Low;
	chosen.Window = {PlayerWindowMode::Windowed, 640, 360, false};

	// An exported game: its own folder below the per-user root.
	SettingsGame exported(ProjectFileKind::Game, 320, 200);
	Testing::IsolatedUserRoot const root;
	std::filesystem::path const gameData = root.GetGameDirectory(GameIdentity{"", "Kept Game"});
	WriteSettings(gameData, chosen);
	ProcessResult const kept = RunPlayer({"--game", exported.GetFile(), "--headless", "--frames", "3"}, root);
	CAPTURE(kept.Output);
	CHECK(kept.ExitCode == 0);
	CHECK(Contains(kept.Output, "Player settings: quality Low, window 640x360 windowed, vsync off, 0 buses, from Settings.json"));

	// The export check's run: a new folder, gone at exit, so the game reads the game's defaults and writes nothing there.
	ProcessResult const checked = RunPlayer({"--game", exported.GetFile(), "--headless", "--temporary-data", "--frames", "3"}, root);
	CAPTURE(checked.Output);
	CHECK(checked.ExitCode == 0);
	std::string const temporary = GetLoggedDataFolder(checked.Output);
	REQUIRE_FALSE(temporary.empty());
	CHECK(Contains(checked.Output, "The game keeps its data in '" + temporary + "', a temporary folder removed at exit"));
	CHECK_FALSE(FileSystem::Exists(FileSystem::PathFromUtf8(temporary)));
	CHECK_FALSE(FileSystem::IsInside(FileSystem::PathFromUtf8(temporary), gameData));
	CHECK(Contains(checked.Output, "Player settings: quality High, window 320x200 windowed, vsync on, 0 buses, the game's defaults"));
	CHECK(ReadPlayerSettingsFile(gameData / "Settings.json", {}).Settings == chosen);

	// A development run: the project's play data.
	SettingsGame project(ProjectFileKind::Project, 320, 200);
	WriteSettings(project.GetPlayData(), chosen);
	ProcessResult const development = RunPlayer({"--project", project.GetFile(), "--headless", "--frames", "3"}, root);
	CAPTURE(development.Output);
	CHECK(development.ExitCode == 0);
	CHECK(GetLoggedDataFolder(development.Output) == FileSystem::PathToUtf8(project.GetPlayData()));
	CHECK(Contains(development.Output, "Player settings: quality Low"));
}

TEST_CASE("SaveGame: the window opens as the player's settings left it, under the player's command line")
{
	{
		// The player needs a Vulkan device (this probe's is released at the end of the block) and a display.
		ST_REQUIRE_GPU();
	}
	ST_REQUIRE_DISPLAY();
	// A game of a 16:10 window whose player chose a 4:1 one: the drawing area has the player's shape, whatever the display's
	// scale.
	SettingsGame project(ProjectFileKind::Project, 320, 200);
	PlayerSettings chosen;
	chosen.Window = {PlayerWindowMode::Windowed, 400, 100, true};
	WriteSettings(project.GetPlayData(), chosen);
	Testing::IsolatedUserRoot const root;
	std::filesystem::path const shot = project.GetDirectory() / "Shot.png";
	ProcessResult const opened = RunPlayer({"--project", project.GetFile(), "--frames", "10", "--screenshot", Native(shot)}, root);
	CAPTURE(opened.Output);
	REQUIRE(opened.ExitCode == 0);
	Result<Image> const image = Image::LoadFromFile(shot);
	REQUIRE(image.IsOk());
	CHECK(image.GetValue().GetWidth() == 4 * image.GetValue().GetHeight());

	// --resolution wins over the settings: a window of the drawing size it names.
	std::filesystem::path const sized = project.GetDirectory() / "Sized.png";
	ProcessResult const resolution =
		RunPlayer({"--project", project.GetFile(), "--frames", "10", "--resolution", "320x184", "--screenshot", Native(sized)}, root);
	CAPTURE(resolution.Output);
	REQUIRE(resolution.ExitCode == 0);
	Result<Image> const sizedImage = Image::LoadFromFile(sized);
	REQUIRE(sizedImage.IsOk());
	CHECK(sizedImage.GetValue().GetWidth() == 320);
	CHECK(sizedImage.GetValue().GetHeight() == 184);
}

TEST_CASE("SaveGame: an exported game's quality, a bus volume and a slot written in one run are read back by the next")
{
	ExportedGraphGame const game(MakeProgressGraph());
	// Both runs share one per-user root, as two runs on a player's machine do; neither is a test run, whose folder is removed.
	Testing::IsolatedUserRoot const root;
	std::filesystem::path const gameData = root.GetGameDirectory(game.GetIdentity());
	ProcessResult const first = RunProgram(game.GetPlayer(), {"--headless", "--frames", "3"}, root);
	CAPTURE(first.Output);
	CHECK(first.ExitCode == 0);
	CHECK(Contains(first.Output, "Player settings: quality High, window 1280x720 windowed, vsync on, 0 buses, the game's defaults"));
	CHECK(Contains(first.Output, "Run 0: quality High, music 0 dB, slot Missing"));
	CHECK(FileSystem::IsRegularFile(gameData / "Saves" / "progress.ssave"));
	CHECK(FileSystem::IsRegularFile(gameData / "Settings.json"));

	ProcessResult const second = RunProgram(game.GetPlayer(), {"--headless", "--frames", "3"}, root);
	CAPTURE(second.Output);
	CHECK(second.ExitCode == 0);
	CHECK(Contains(second.Output, "Player settings: quality Low, window 1280x720 windowed, vsync on, 1 bus, from Settings.json"));
	CHECK(Contains(second.Output, "Run 1: quality Low, music -12 dB, slot Present"));
	// Neither the slot nor the settings had anything to say.
	CHECK_FALSE(Contains(second.Output, "SaveSlot."));
	CHECK_FALSE(Contains(second.Output, "Settings.Save:"));

	// What the second run left: Runs 2, and the settings as the first run saved them.
	Result<std::string> const slot = FileSystem::ReadTextFile(gameData / "Saves" / "progress.ssave");
	REQUIRE(slot.IsOk());
	SaveSlotRead const read = ReadSaveSlotText(slot.GetValue());
	REQUIRE(read.Status == SaveSlotStatus::Present);
	CHECK(read.Data.Values == ScriptFieldMap{{"Runs", ScriptFieldValue::FromInt32(2)}});
	PlayerSettingsRead const settings = ReadPlayerSettingsFile(gameData / "Settings.json", {});
	REQUIRE(settings.Status == PlayerSettingsStatus::Present);
	CHECK(settings.Settings.Quality == QualityPreset::Low);
	CHECK(settings.Settings.Buses == std::vector<PlayerBusSettings>{{"Music", -12.0f, false}});
}

TEST_CASE("SaveGame: a script's window request sizes the player's window as one opened at that size, a request of vsync or "
          "the mode alone keeps its size, and --resolution holds against a request")
{
	{
		// The player needs a Vulkan device (this probe's is released at the end of the block) and a display.
		ST_REQUIRE_GPU();
	}
	ST_REQUIRE_DISPLAY();
	ExportedGraphGame const game(MakeWindowGraph());
	Testing::IsolatedUserRoot const root;
	std::filesystem::path const settingsFile = root.GetGameDirectory(game.GetIdentity()) / "Settings.json";
	// A resize takes a frame or two to reach the swapchain, so each run draws 20 frames before its screenshot.
	struct Run
	{
		Image Shot;
		std::string Output;
	};
	auto const play = [&](std::string const& name, std::vector<std::string> arguments)
	{
		std::filesystem::path const shot = game.GetDirectory() / FileSystem::PathFromUtf8(name);
		arguments.insert(arguments.end(), {"--frames", "20", "--screenshot", Native(shot)});
		ProcessResult const run = RunProgram(game.GetPlayer(), std::move(arguments), root);
		CAPTURE(run.Output);
		REQUIRE(run.ExitCode == 0);
		Result<Image> image = Image::LoadFromFile(shot);
		REQUIRE(image.IsOk());
		return Run{image.TakeValue(), run.Output};
	};

	// The game opens at its own 1280 x 720 (16:9), and its script asks for 400 x 100 (4:1) in its first frame.
	Run const requested = play("Requested.png", {});
	uint32_t const width = requested.Shot.GetWidth();
	uint32_t const height = requested.Shot.GetHeight();
	CHECK(width == 4 * height);
	CHECK(Contains(requested.Output, "The window takes the player's settings: windowed at 400x100, vsync off (the swapchain's: off)"));
	REQUIRE(FileSystem::IsRegularFile(settingsFile));
	CHECK(ReadPlayerSettingsFile(settingsFile, {}).Settings.Window == PlayerWindowSettings{PlayerWindowMode::Windowed, 400, 100, false});

	// The next run opens at the saved size with the drawing area the request gave, whatever the display's scale, and its
	// request of vsync alone leaves it so.
	std::string drawingArea = "(";
	drawingArea += std::to_string(width);
	drawingArea += "x";
	drawingArea += std::to_string(height);
	drawingArea += " framebuffer)";
	Run const vsync = play("VSync.png", {});
	CHECK(Contains(vsync.Output, drawingArea));
	CHECK(Contains(vsync.Output, "The window takes the player's settings: windowed at 400x100, vsync on (the swapchain's: on)"));
	CHECK(vsync.Shot.GetWidth() == width);
	CHECK(vsync.Shot.GetHeight() == height);
	CHECK(ReadPlayerSettingsFile(settingsFile, {}).Settings.Window.VSync);

	// Fullscreen, then back into a window: the size it opened at.
	Run const returned = play("Returned.png", {});
	CHECK(Contains(returned.Output, "The window takes the player's settings: fullscreen, vsync on (the swapchain's: on)"));
	CHECK(Contains(returned.Output, "The window takes the player's settings: windowed at 400x100, vsync on (the swapchain's: on)"));
	CHECK(returned.Shot.GetWidth() == width);
	CHECK(returned.Shot.GetHeight() == height);

	// Without the saved settings the script asks again, and --resolution keeps the drawing size it names.
	REQUIRE(FileSystem::Remove(settingsFile).IsOk());
	Run const sized = play("Sized.png", {"--resolution", "320x184"});
	CHECK(sized.Shot.GetWidth() == 320);
	CHECK(sized.Shot.GetHeight() == 184);
}
