#include "TestUtilities.h"

#include "Editor/EditorOperations.h"
#include "Editor/RecentProjects.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Entity.h"

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
		scene->CreateEntity("Hero");
		return scene;
	}
}

TEST_CASE("RecentProjects: most recent first, without duplicates, bounded and persisted")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const file = temporary.GetPath() / "Editor" / "RecentProjects.json";
	RecentProjects recent(file);
	REQUIRE(recent.Load().IsOk());
	CHECK(recent.GetProjects().empty());

	auto const project = [&](int index)
	{
		return temporary.GetPath() / fmt::format("Project{}", index) / "Game.sproj";
	};
	for (int i = 0; i < 12; i++)
	{
		recent.Add(project(i));
	}
	REQUIRE(recent.GetProjects().size() == RecentProjects::MaxCount);
	CHECK(recent.GetProjects().front().filename() == "Game.sproj");
	CHECK(recent.GetProjects().front().parent_path().filename() == "Project11");
	CHECK(recent.GetProjects().back().parent_path().filename() == "Project2");

	// Re-adding moves a project to the front; differently spelled paths to it are the same project.
	recent.Add(temporary.GetPath() / "Project5" / "Sub" / ".." / "Game.sproj");
	CHECK(recent.GetProjects().size() == RecentProjects::MaxCount);
	CHECK(recent.GetProjects().front().parent_path().filename() == "Project5");
	recent.Remove(project(5));
	CHECK(recent.GetProjects().size() == RecentProjects::MaxCount - 1);
	CHECK(recent.GetProjects().front().parent_path().filename() == "Project11");

	REQUIRE(recent.Save().IsOk());
	RecentProjects reloaded(file);
	REQUIRE(reloaded.Load().IsOk());
	CHECK(reloaded.GetProjects() == recent.GetProjects());

	recent.Clear();
	CHECK(recent.GetProjects().empty());
	REQUIRE(FileSystem::WriteTextFile(file, "{ not json").IsOk());
	CHECK(reloaded.Load().IsError());
	CHECK(reloaded.GetProjects().empty());
}

TEST_CASE("EditorOperations: projects are created, opened with their start scene, configured and closed")
{
	AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	CHECK(operations.ApplyProjectSettings(Json::object()).IsError());
	CHECK(operations.SaveProject().IsError());

	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", *MakeStartScene()).IsOk());
	Project* project = context.GetProject();
	REQUIRE(project != nullptr);
	CHECK(context.GetScenePath() == project->GetAssetDirectory() / "Scenes" / "Main.sscene");
	CHECK(context.GetScene().FindEntityByName("Hero"));
	CHECK_FALSE(context.IsDirty());

	REQUIRE(operations.ApplyProjectSettings(Json::object({{"Window", {{"Width", 1600}}}})).IsOk());
	REQUIRE(operations.SaveProject().IsOk());
	CHECK(operations.ApplyProjectSettings(Json::object({{"Window", {{"Width", 1}}}})).IsError());
	CHECK(project->GetSettings().Window.Width == 1600u);

	// Scenes saved into the project are registered as assets.
	std::filesystem::path const second = project->GetAssetDirectory() / "Scenes" / "Second.sscene";
	REQUIRE(operations.SaveScene(second).IsOk());
	CHECK(AssetManager::GetAssetType(AssetManager::FindByPath("Scenes/Second.sscene")) == AssetType::Scene);

	std::filesystem::path const file = project->GetFilePath();
	REQUIRE(operations.CloseProject().IsOk());
	CHECK(context.GetProject() == nullptr);
	CHECK_FALSE(AssetManager::HasAssetDirectory());
	CHECK(context.GetScenePath().empty());
	CHECK(context.GetScene().GetEntityCount() == 0);

	Result<std::vector<std::string>> opened = operations.OpenProject(file);
	REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
	CHECK(opened.GetValue().empty());
	REQUIRE(context.GetProject() != nullptr);
	CHECK(context.GetProject()->GetSettings().Window.Width == 1600u);
	CHECK(context.GetScene().FindEntityByName("Hero"));

	// A project that cannot be opened leaves the open one alone.
	CHECK(operations.OpenProject(temporary.GetPath() / "Missing.sproj").IsError());
	CHECK(operations.CreateProject(temporary.GetPath() / "Game", "Again", *MakeStartScene()).IsError());
	REQUIRE(context.GetProject() != nullptr);
	CHECK(context.GetProject()->GetFilePath() == file);
	CHECK(AssetManager::GetAssetDirectory() == context.GetProject()->GetAssetDirectory());
}

TEST_CASE("EditorOperations: a project whose start scene cannot be read opens with an empty scene and a warning")
{
	AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", *MakeStartScene()).IsOk());
	std::filesystem::path const file = context.GetProject()->GetFilePath();
	REQUIRE(FileSystem::WriteTextFile(context.GetProject()->GetAssetDirectory() / "Scenes" / "Main.sscene", "{ broken").IsOk());
	REQUIRE(operations.CloseProject().IsOk());

	Result<std::vector<std::string>> opened = operations.OpenProject(file);
	REQUIRE(opened.IsOk());
	REQUIRE(opened.GetValue().size() == 1);
	CHECK(opened.GetValue().front().starts_with("the start scene could not be opened"));
	CHECK(context.GetProject() != nullptr);
	CHECK(context.GetScene().GetEntityCount() == 0);
	CHECK(context.GetScenePath().empty());
}
