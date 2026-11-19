#include "Core/UserDataTestUtilities.h"
#include "GpuTestUtilities.h"
#include "RuntimeTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Image.h"
#include "Strada/Platform/Process.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ProjectSettings.h"
#include "Strada/Renderer/QualityPresets.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/PlayerSettings.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

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
