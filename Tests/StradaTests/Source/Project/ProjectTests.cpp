#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Strada;

namespace
{
	// Initializes the AssetManager for one test and always shuts it down.
	class AssetManagerScope
	{
	public:
		AssetManagerScope() { AssetManager::Init(); }
		~AssetManagerScope()
		{
			if (AssetManager::IsInitialized())
			{
				AssetManager::Shutdown();
			}
		}

		AssetManagerScope(AssetManagerScope const&) = delete;
		AssetManagerScope& operator=(AssetManagerScope const&) = delete;
	};

	Ref<Scene> MakeStartScene()
	{
		Ref<Scene> scene = CreateRef<Scene>("Main");
		scene->CreateEntity("Player");
		return scene;
	}

	Result<ProjectSettings> Read(Json const& settings)
	{
		Json document = Json::object();
		document["Strada"] = Json::object({{"Version", 1}, {"Type", "Project"}});
		document["Project"] = settings;
		return Project::Deserialize(document, DeserializationContext{});
	}

	std::string ReadError(Json const& settings)
	{
		Result<ProjectSettings> const result = Read(settings);
		return result ? std::string() : result.GetError();
	}
}

TEST_CASE("Project: creating writes the project file, the asset directory and the start scene")
{
	AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const directory = temporary.GetPath() / "Space Game";
	Ref<Scene> const scene = MakeStartScene();

	Result<Ref<Project>> created = Project::Create(directory, "Space Game", *scene);
	REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
	Project const& project = *created.GetValue();
	CHECK(project.GetFilePath() == (directory / "Space Game.sproj").lexically_normal());
	CHECK(FileSystem::IsRegularFile(project.GetFilePath()));
	CHECK(FileSystem::IsRegularFile(directory / "Assets" / "Scenes" / "Main.sscene"));
	CHECK(FileSystem::IsRegularFile(directory / "Assets" / "AssetRegistry.sreg"));
	CHECK(project.GetSettings().Name == "Space Game");

	// The asset directory is open and the start scene is a registered asset.
	CHECK(AssetManager::GetAssetDirectory() == project.GetAssetDirectory());
	AssetHandle const start = project.GetSettings().StartScene;
	CHECK(start == AssetManager::FindByPath("Scenes/Main.sscene"));
	CHECK(AssetManager::GetAssetType(start) == AssetType::Scene);
	Result<Ref<Scene>> loaded = SceneSerializer::LoadFromFile(AssetManager::GetAbsolutePath(start), DeserializationContext{});
	REQUIRE(loaded.IsOk());
	CHECK(loaded.GetValue()->FindEntityByName("Player"));
}

TEST_CASE("Project: creation needs a name and an empty directory and leaves nothing behind on failure")
{
	AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	Ref<Scene> const startScene = MakeStartScene();
	Scene const& scene = *startScene;

	CHECK(Project::Create(temporary.GetPath() / "Unnamed", "", scene).IsError());
	CHECK_FALSE(FileSystem::Exists(temporary.GetPath() / "Unnamed"));

	std::filesystem::path const occupied = temporary.GetPath() / "Occupied";
	REQUIRE(FileSystem::WriteTextFile(occupied / "notes.txt", "keep me").IsOk());
	CHECK(Project::Create(occupied, "Game", scene).IsError());
	CHECK(FileSystem::ReadTextFile(occupied / "notes.txt").GetValue() == "keep me");
	CHECK_FALSE(FileSystem::Exists(occupied / "Assets"));

	REQUIRE(FileSystem::WriteTextFile(temporary.GetPath() / "file", "").IsOk());
	CHECK(Project::Create(temporary.GetPath() / "file", "Game", scene).IsError());

	// An existing empty directory is fine.
	std::filesystem::path const empty = temporary.GetPath() / "Empty";
	REQUIRE(FileSystem::CreateDirectories(empty).IsOk());
	CHECK(Project::Create(empty, "Game: The Sequel", scene).IsOk());
	CHECK(FileSystem::IsRegularFile(empty / "Game_ The Sequel.sproj"));
}

TEST_CASE("Project: settings round trip through the project file")
{
	AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	Result<Ref<Project>> created = Project::Create(temporary.GetPath() / "Game", "Game", *MakeStartScene());
	REQUIRE(created.IsOk());
	Ref<Project> const project = created.GetValue();

	Json const patch = Json::parse(R"({
		"Window": { "Title": "My Game", "Width": 1920, "Fullscreen": true },
		"Physics": { "FixedTimestep": 0.02, "Layers": ["Default", "Player", "Enemy"],
		             "IgnoredCollisions": [{ "First": 1, "Second": 2 }] }
	})");
	REQUIRE(project->ApplySettings(patch).IsOk());
	CHECK(project->GetSettings().Window.Height == 720u);
	REQUIRE(project->Save().IsOk());
	ProjectSettings const expected = project->GetSettings();

	AssetManager::CloseAssetDirectory();
	std::vector<std::string> warnings;
	Result<Ref<Project>> opened = Project::Open(project->GetFilePath(), &warnings);
	REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
	CHECK(opened.GetValue()->GetSettings() == expected);
	CHECK(warnings.empty());
	CHECK(AssetManager::GetAssetDirectory() == project->GetAssetDirectory());
	CHECK(AssetManager::GetAssetType(expected.StartScene) == AssetType::Scene);
}

TEST_CASE("Project: invalid settings are rejected and change nothing")
{
	CHECK(Read(Json::object()).IsOk());
	CHECK(ReadError(Json::object({{"Name", ""}})) == "Project.Name: must not be empty");
	CHECK(ReadError(Json::object({{"AssetDirectory", "../Shared"}})) ==
	      "Project.AssetDirectory: must stay inside the project directory, got '../Shared'");
	CHECK(ReadError(Json::object({{"ScriptModule", "/usr/lib/Game.dll"}})).starts_with("Project.ScriptModule: must be relative"));
	CHECK(ReadError(Json::object({{"Physics", {{"Layers", Json::array()}}}})) ==
	      "Project.Physics.Layers: between 1 and 16 layers are required, got 0");
	Json tooMany = Json::array();
	for (int i = 0; i < 17; i++)
	{
		tooMany.push_back(fmt::format("Layer{}", i));
	}
	CHECK(ReadError(Json::object({{"Physics", {{"Layers", tooMany}}}})).starts_with("Project.Physics.Layers: between 1 and 16"));
	CHECK(ReadError(Json::object({{"Physics", {{"Layers", {"Default", "Default"}}}}})) ==
	      "Project.Physics.Layers: layer 'Default' is listed twice");
	CHECK(ReadError(Json::object({{"Physics", {{"Layers", {"Default", ""}}}}})) == "Project.Physics.Layers: layer names must not be empty");
	CHECK(ReadError(Json::object({{"Physics", {{"IgnoredCollisions", {{{"First", 0}, {"Second", 1}}}}}}}))
	          .starts_with("Project.Physics.IgnoredCollisions: layers 0 and 1 do not both exist"));
	CHECK(ReadError(Json::object({{"Physics", {{"FixedTimestep", 0.0}}}})) ==
	      "Project.Physics: Physics.FixedTimestep: must be between 0.001 and 0.1");
	CHECK(Read(Json::object({{"Unknown", 1}})).IsError());

	Json wrongType = Json::object();
	wrongType["Strada"] = Json::object({{"Version", 1}, {"Type", "Scene"}});
	wrongType["Project"] = Json::object();
	CHECK(Project::Deserialize(wrongType, DeserializationContext{}).IsError());
	Json newer = wrongType;
	newer["Strada"] = Json::object({{"Version", 99}, {"Type", "Project"}});
	CHECK(Project::Deserialize(newer, DeserializationContext{}).IsError());

	Project project("Game.sproj", ProjectSettings());
	ProjectSettings const before = project.GetSettings();
	CHECK(project.ApplySettings(Json::object({{"Window", {{"Width", 2048}}}, {"Name", ""}})).IsError());
	CHECK(project.ApplySettings(Json::object({{"AssetDirectory", "Content"}})).GetError() ==
	      "Project.AssetDirectory: cannot change while the project is open");
	CHECK(project.GetSettings() == before);
}

TEST_CASE("Project: opening resolves asset references, reports newer settings and fails cleanly")
{
	AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const directory = temporary.GetPath() / "Handwritten";
	REQUIRE(FileSystem::CreateDirectories(directory / "Assets" / "Levels").IsOk());
	REQUIRE(SceneSerializer::SaveToFile(*MakeStartScene(), directory / "Assets" / "Levels" / "One.sscene").IsOk());
	REQUIRE(FileSystem::WriteTextFile(directory / "Game.sproj", R"({
		"Strada": { "Version": 1, "Type": "Project" },
		"Project": { "Name": "Handwritten", "StartScene": "asset://Levels/One.sscene", "Telemetry": true }
	})")
	            .IsOk());

	std::vector<std::string> warnings;
	Result<Ref<Project>> opened = Project::Open(directory / "Game.sproj", &warnings);
	REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
	CHECK(opened.GetValue()->GetSettings().StartScene == AssetManager::FindByPath("Levels/One.sscene"));
	CHECK(warnings == std::vector<std::string>{"ignored unknown field 'Telemetry' in project 'Project'"});

	CHECK(Project::Open(directory / "Missing.sproj").IsError());
	REQUIRE(FileSystem::WriteTextFile(directory / "Broken.sproj", "{ not json").IsOk());
	CHECK(Project::Open(directory / "Broken.sproj").IsError());
	REQUIRE(FileSystem::WriteTextFile(directory / "BadScene.sproj", R"({
		"Strada": { "Version": 1, "Type": "Project" },
		"Project": { "StartScene": "asset://Levels/Missing.sscene" }
	})")
	            .IsOk());
	CHECK(Project::Open(directory / "BadScene.sproj").IsError());
	// A failed open does not leave the asset directory open.
	CHECK_FALSE(AssetManager::HasAssetDirectory());
}

TEST_CASE("Project: an exported game's configuration opens like a project and keeps its format")
{
	AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const directory = temporary.GetPath() / "Export";
	REQUIRE(FileSystem::CreateDirectories(directory / "Assets" / "Scenes").IsOk());
	REQUIRE(SceneSerializer::SaveToFile(*MakeStartScene(), directory / "Assets" / "Scenes" / "Main.sscene").IsOk());
	REQUIRE(AssetManager::OpenAssetDirectory(directory / "Assets").IsOk());
	ProjectSettings settings;
	settings.Name = "Exported";
	settings.ScriptModule = "Scripts/Game.dll";
	settings.StartScene = AssetManager::FindByPath("Scenes/Main.sscene");
	settings.Window.Width = 800;
	AssetManager::CloseAssetDirectory();

	std::filesystem::path const file = directory / FileSystem::PathFromUtf8(Project::GameFileName);
	REQUIRE(FileSystem::WriteTextFile(file, DumpJson(Project::Serialize(settings, ProjectFileKind::Game))).IsOk());
	Json const written = Json::parse(FileSystem::ReadTextFile(file).GetValue());
	CHECK(written["Strada"]["Type"] == "Game");
	CHECK(written.contains("Game"));
	CHECK_FALSE(written.contains("Project"));

	Result<Ref<Project>> opened = Project::OpenGame(file);
	REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
	Ref<Project> const game = opened.GetValue();
	CHECK(game->IsGame());
	CHECK(game->GetFileKind() == ProjectFileKind::Game);
	CHECK(game->GetSettings() == settings);
	CHECK(AssetManager::GetAssetDirectory() == game->GetAssetDirectory());
	CHECK(AssetManager::GetAssetType(game->GetSettings().StartScene) == AssetType::Scene);
	REQUIRE(game->Save().IsOk());
	CHECK(Json::parse(FileSystem::ReadTextFile(file).GetValue())["Strada"]["Type"] == "Game");

	// Projects and games do not open as each other.
	AssetManager::CloseAssetDirectory();
	CHECK(Project::Open(file).IsError());
	CHECK_FALSE(AssetManager::HasAssetDirectory());
	Result<Ref<Project>> created = Project::Create(temporary.GetPath() / "Project", "Project", *MakeStartScene());
	REQUIRE(created.IsOk());
	CHECK_FALSE(created.GetValue()->IsGame());
	AssetManager::CloseAssetDirectory();
	CHECK(Project::OpenGame(created.GetValue()->GetFilePath()).IsError());
	CHECK_FALSE(AssetManager::HasAssetDirectory());
}

TEST_CASE("Project: the window settings are read without opening the file")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const file = temporary.GetPath() / "Game.sgame";
	REQUIRE(FileSystem::WriteTextFile(file, R"({
		"Strada": { "Version": 1, "Type": "Game" },
		"Game": { "Name": "Windowed", "StartScene": "asset://Scenes/Main.sscene", "Future": 1,
		          "Window": { "Width": 1024, "Fullscreen": true, "Future": 2 } }
	})")
	            .IsOk());
	// Neither the asset reference nor fields of newer versions matter here.
	Result<ProjectWindowSettings> window = Project::ReadWindowSettings(file, ProjectFileKind::Game);
	REQUIRE_MESSAGE(window.IsOk(), (window ? std::string() : window.GetError()));
	CHECK(window.GetValue().Title == "Windowed");
	CHECK(window.GetValue().Width == 1024u);
	CHECK(window.GetValue().Height == 720u);
	CHECK(window.GetValue().Fullscreen);
	CHECK(window.GetValue().VSync);

	REQUIRE(FileSystem::WriteTextFile(file, R"({
		"Strada": { "Version": 1, "Type": "Game" },
		"Game": { "Name": "Windowed", "Window": { "Title": "Shown Title", "Resizable": false } }
	})")
	            .IsOk());
	window = Project::ReadWindowSettings(file, ProjectFileKind::Game);
	REQUIRE(window.IsOk());
	CHECK(window.GetValue().Title == "Shown Title");
	CHECK_FALSE(window.GetValue().Resizable);

	CHECK(Project::ReadWindowSettings(file, ProjectFileKind::Project).IsError());
	CHECK(Project::ReadWindowSettings(temporary.GetPath() / "Missing.sgame", ProjectFileKind::Game).IsError());
	REQUIRE(FileSystem::WriteTextFile(file, R"({
		"Strada": { "Version": 1, "Type": "Game" },
		"Game": { "Window": { "Width": 10 } }
	})")
	            .IsOk());
	Result<ProjectWindowSettings> const tooSmall = Project::ReadWindowSettings(file, ProjectFileKind::Game);
	REQUIRE(tooSmall.IsError());
	CHECK(tooSmall.GetError().find("Game.Window") != std::string::npos);
	REQUIRE(FileSystem::WriteTextFile(file, R"({ "Strada": { "Version": 1, "Type": "Game" } })").IsOk());
	CHECK(Project::ReadWindowSettings(file, ProjectFileKind::Game).IsError());
}
