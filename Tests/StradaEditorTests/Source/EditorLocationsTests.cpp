#include "EditorLocationsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/EditorLayer.h"
#include "Editor/EditorLocations.h"
#include "Editor/RecentProjects.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Core/EngineLayout.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	constexpr std::array<EngineContent, 5> AllContent = {EngineContent::Templates, EngineContent::Examples, EngineContent::Docs,
	                                                     EngineContent::AgentKit, EngineContent::MakeGameSkill};

	bool IsEmptyDirectory(std::filesystem::path const& directory)
	{
		std::error_code errorCode;
		return std::filesystem::is_empty(directory, errorCode) && !errorCode;
	}
}

TEST_CASE("EditorLocations: the test locations read the test content and keep the user's folders in the temporary directory")
{
	Testing::TemporaryDirectory temporary;
	EditorLocations const locations = Testing::MakeEditorLocations(temporary.GetPath());
	std::filesystem::path const content = Testing::GetTestContentDirectory();

	EngineLayout const& layout = locations.Layout;
	CHECK_FALSE(layout.IsSourceTree());
	CHECK(layout.GetWarnings().empty());
	CHECK(layout.GetExecutableDirectory() == content);
	CHECK(layout.GetResourcesDirectory() == content);
	CHECK(layout.GetContentPath(EngineContent::Templates) == content / "Templates");
	CHECK(layout.GetContentPath(EngineContent::Examples) == content / "Examples");
	CHECK(layout.GetContentPath(EngineContent::Docs) == content / "Docs");
	CHECK(layout.GetContentPath(EngineContent::AgentKit) == content / "Agents");
	CHECK(layout.GetContentPath(EngineContent::MakeGameSkill) == content / "Agents" / "skills" / "strada-make-game");
	CHECK(layout.GetExampleCatalogPath() == content / "Examples" / "catalog.json");
	CHECK(FileSystem::IsRegularFile(content / "README.md"));
	CHECK(FileSystem::IsRegularFile(layout.GetContentPath(EngineContent::Docs) / "GettingStarted.md"));

	CHECK(locations.UserDataDirectory == temporary.GetPath() / "UserData" / "Editor");
	CHECK(locations.DefaultProjectLocation == temporary.GetPath() / "Projects");
	CHECK(locations.GetUserDataFile("RecentProjects.json") == temporary.GetPath() / "UserData" / "Editor" / "RecentProjects.json");
	// Naming the folders creates none of them.
	CHECK(IsEmptyDirectory(temporary.GetPath()));
}

TEST_CASE("EditorLocations: a default-constructed command environment names no folder")
{
	EditorCommandEnvironment const environment;
	EditorLocations const& locations = environment.Locations;
	CHECK(locations.UserDataDirectory.empty());
	CHECK(locations.DefaultProjectLocation.empty());
	CHECK(locations.GetUserDataFile("RecentProjects.json").empty());

	EngineLayout const& layout = locations.Layout;
	CHECK_FALSE(layout.IsSourceTree());
	CHECK(layout.GetExecutableDirectory().empty());
	CHECK(layout.GetResourcesDirectory().empty());
	CHECK(layout.GetSourceDirectory().empty());
	for (EngineContent const content : AllContent)
	{
		CAPTURE(static_cast<int>(content));
		CHECK(layout.GetContentPath(content).empty());
	}
	CHECK(layout.GetExampleCatalogPath().empty());
	CHECK(layout.GetToolPath().empty());
	CHECK(layout.GetWarnings().empty());
}

TEST_CASE("EditorLocations: the process's are the engine's build tree, the user data's Editor folder and the Documents folder")
{
	// Reads the process's folders and writes nothing.
	EditorLocations const locations = EditorLocations::FromProcess();
	EngineLayout const& process = EngineLayout::Get();
	// The tests run in the build tree, beside the marker CMake writes.
	CHECK(locations.Layout.IsSourceTree());
	CHECK(locations.Layout.GetSourceDirectory() == process.GetSourceDirectory());
	CHECK(locations.Layout.GetResourcesDirectory() == process.GetResourcesDirectory());
	CHECK(locations.UserDataDirectory == FileSystem::GetUserDataDirectory() / "Editor");
	// STRADA_PROJECTS_DIR (read, never set here) or the Documents folder.
	CHECK(locations.DefaultProjectLocation ==
	      EditorLocations::ResolveDefaultProjectLocation(Platform::ReadEnvironmentVariable(EditorLocations::ProjectsDirectoryVariable),
	                                                     Platform::GetDocumentsDirectory()));
}

TEST_CASE("EditorLocations: STRADA_PROJECTS_DIR names the default project location when it is an absolute path")
{
	// Absolute on every platform: the working directory's root ("C:\" or "/").
	std::filesystem::path const root = std::filesystem::current_path().root_path();
	std::filesystem::path const documents = root / "Users" / "Someone" / "Documents";
	std::filesystem::path const games = root / "Games" / "Strada";
	std::string const gamesText = FileSystem::PathToUtf8(games);

	// Unset or empty: the Documents folder's "Strada Projects", or nothing without one.
	std::vector<std::string> warnings;
	CHECK(EditorLocations::ResolveDefaultProjectLocation(std::nullopt, documents, &warnings) == documents / "Strada Projects");
	CHECK(EditorLocations::ResolveDefaultProjectLocation(std::string(), documents, &warnings) == documents / "Strada Projects");
	CHECK(EditorLocations::ResolveDefaultProjectLocation(std::nullopt, std::nullopt, &warnings).empty());
	CHECK(EditorLocations::ResolveDefaultProjectLocation(std::string(), std::nullopt, &warnings).empty());
	CHECK(warnings.empty());

	// An absolute path is the location itself, with or without a Documents folder, and without a trailing separator.
	CHECK(EditorLocations::ResolveDefaultProjectLocation(gamesText, documents, &warnings) == games);
	CHECK(EditorLocations::ResolveDefaultProjectLocation(gamesText, std::nullopt, &warnings) == games);
	std::filesystem::path const trailing = EditorLocations::ResolveDefaultProjectLocation(gamesText + "/", documents, &warnings);
	CHECK(trailing == games);
	CHECK(trailing.has_filename());
	CHECK(EditorLocations::ResolveDefaultProjectLocation(FileSystem::PathToUtf8(root / "Games" / "Old" / ".." / "Strada"), documents,
	                                                     &warnings) == games);
	// A root stays a root.
	CHECK(EditorLocations::ResolveDefaultProjectLocation(FileSystem::PathToUtf8(root), documents, &warnings) == root);
	CHECK(warnings.empty());

	// A relative value is ignored, with one warning that names the variable and the value.
	for (std::string const relative : {"Games", "Games/Strada", "./Strada", "../Strada"})
	{
		CAPTURE(relative);
		std::vector<std::string> ignored;
		CHECK(EditorLocations::ResolveDefaultProjectLocation(relative, documents, &ignored) == documents / "Strada Projects");
		REQUIRE(ignored.size() == 1);
		CHECK(ignored.front().find(EditorLocations::ProjectsDirectoryVariable) != std::string::npos);
		CHECK(ignored.front().find("'" + relative + "'") != std::string::npos);
		CHECK(EditorLocations::ResolveDefaultProjectLocation(relative, std::nullopt, &ignored).empty());
		CHECK(ignored.size() == 2);
		// Without a list the value is still ignored.
		CHECK(EditorLocations::ResolveDefaultProjectLocation(relative, documents) == documents / "Strada Projects");
	}
#ifdef ST_PLATFORM_WINDOWS
	// Rooted without a drive, or on a drive without a root: relative on Windows.
	for (std::string const relative : {"/Games", "C:Games"})
	{
		CAPTURE(relative);
		std::vector<std::string> ignored;
		CHECK(EditorLocations::ResolveDefaultProjectLocation(relative, documents, &ignored) == documents / "Strada Projects");
		CHECK(ignored.size() == 1);
	}
#endif
}

TEST_CASE("EditorLocations: the editor layer keeps its recent projects and graph views in the user data of its locations")
{
	Testing::TemporaryDirectory temporary;
	EditorLayerSpecification specification;
	specification.EnableAutomation = false;
	specification.Locations = Testing::MakeEditorLocations(temporary.GetPath());
	EditorLayer const layer(specification);
	std::filesystem::path const userData = temporary.GetPath() / "UserData" / "Editor";
	CHECK(layer.GetLocations().UserDataDirectory == userData);
	CHECK(layer.GetLocations().DefaultProjectLocation == temporary.GetPath() / "Projects");
	CHECK(layer.GetRecentProjects().GetFile() == userData / "RecentProjects.json");
	CHECK(layer.GetVisualScriptViews().GetFile() == userData / "VisualScriptViews.json");
	CHECK(IsEmptyDirectory(temporary.GetPath()));

	// Without a user data directory nothing is persisted: the lists have no file, and saving them writes nothing.
	EditorLayerSpecification inMemory;
	inMemory.EnableAutomation = false;
	inMemory.Locations = EditorLocations();
	EditorLayer const unsaved(inMemory);
	CHECK(unsaved.GetRecentProjects().GetFile().empty());
	CHECK(unsaved.GetVisualScriptViews().GetFile().empty());

	RecentProjects recent({});
	recent.Add(temporary.GetPath() / "Game" / "Game.sproj");
	CHECK(recent.Save().IsOk());
	CHECK(recent.Load().IsOk());
	CHECK(recent.GetProjects().empty());
	VisualScriptViewSettings views({});
	CHECK(views.Save().IsOk());
	CHECK(views.Load().empty());
	CHECK(IsEmptyDirectory(temporary.GetPath()));
}
