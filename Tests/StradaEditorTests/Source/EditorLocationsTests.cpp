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
	std::optional<std::filesystem::path> const documents = Platform::GetDocumentsDirectory();
	CHECK(locations.DefaultProjectLocation == (documents ? *documents / "Strada Projects" : std::filesystem::path()));
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
