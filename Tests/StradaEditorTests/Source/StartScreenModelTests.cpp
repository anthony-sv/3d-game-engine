#include "EditorLocationsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/StartScreen/StartScreenModel.h"

#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// A start screen over the test content, with its user data and default project location in a temporary directory.
	struct ModelFixture
	{
		Testing::TemporaryDirectory Directory;
		EditorLocations Locations = Testing::MakeEditorLocations(Directory.GetPath());
		std::vector<std::filesystem::path> Opened;
		StartScreenModel Model{Locations, MakeServices()};
		StartScreenEnvironment Environment;

		StartScreenServices MakeServices()
		{
			StartScreenServices services;
			services.DescribeEnvironment = [this]
			{
				return Environment;
			};
			services.OpenWithSystem = [this](std::filesystem::path const& path) -> Result<void>
			{
				Opened.push_back(path);
				return {};
			};
			return services;
		}

		std::filesystem::path Write(std::filesystem::path const& relative, std::string_view text) const
		{
			std::filesystem::path const file = Directory.GetPath() / relative;
			REQUIRE(FileSystem::CreateDirectories(file.parent_path()).IsOk());
			REQUIRE(FileSystem::WriteTextFile(file, text).IsOk());
			return file;
		}
	};

	ExampleEntry FindExample(StartScreenModel const& model, std::string_view id)
	{
		REQUIRE(model.GetExamples().has_value());
		ExampleEntry const* example = model.GetExamples()->Find(id);
		REQUIRE(example != nullptr);
		return *example;
	}
}

TEST_CASE("StartScreenModel: recent projects are Ok, Missing or Unreadable as their files read now")
{
	ModelFixture fixture;
	std::filesystem::path const valid =
		fixture.Write("Orb/Orb Run.sproj", R"({ "Strada": { "Version": 1, "Type": "Project" }, "Project": { "Name": "Orb Run!" } })");
	std::filesystem::path const unnamed = fixture.Write("Unnamed/Plain.sproj", R"({ "Strada": { "Version": 1, "Type": "Project" } })");
	std::filesystem::path const garbage = fixture.Write("Garbage/Garbage.sproj", "\x01\x02 not json {");
	std::filesystem::path const scene =
		fixture.Write("Scene/Main.sproj", R"({ "Strada": { "Version": 1, "Type": "Scene" }, "Scene": { "Name": "Main" } })");
	std::filesystem::path const newer =
		fixture.Write("Newer/Newer.sproj", R"({ "Strada": { "Version": 99, "Type": "Project" }, "Project": { "Name": "Future" } })");
	std::filesystem::path const folder = fixture.Directory.GetPath() / "Folder" / "Folder.sproj";
	REQUIRE(FileSystem::CreateDirectories(folder).IsOk());
	std::filesystem::path const missing = fixture.Directory.GetPath() / "Gone" / "Gone.sproj";

	struct Expected
	{
		std::filesystem::path File;
		RecentProjectState State;
		std::string Name;
	};
	std::vector<Expected> const expected = {
		{valid, RecentProjectState::Ok, "Orb Run!"},          {unnamed, RecentProjectState::Ok, "Plain"},
		{garbage, RecentProjectState::Unreadable, "Garbage"}, {scene, RecentProjectState::Unreadable, "Main"},
		{newer, RecentProjectState::Unreadable, "Newer"},     {folder, RecentProjectState::Unreadable, "Folder"},
		{missing, RecentProjectState::Missing, "Gone"},
	};
	for (Expected const& entry : expected)
	{
		CAPTURE(FileSystem::PathToUtf8(entry.File));
		RecentProjectInfo const info = StartScreenModel::DescribeRecentProject(entry.File);
		CHECK(info.State == entry.State);
		CHECK(info.Name == entry.Name);
		CHECK(info.File == entry.File);
	}
	CHECK(GetRecentProjectStateName(RecentProjectState::Ok) == "Ok");
	CHECK(GetRecentProjectStateName(RecentProjectState::Missing) == "Missing");
	CHECK(GetRecentProjectStateName(RecentProjectState::Unreadable) == "Unreadable");

	// The list in its order, most recent first, as Refresh reads it.
	for (auto it = expected.rbegin(); it != expected.rend(); ++it)
	{
		fixture.Model.GetRecentProjects().Add(it->File);
	}
	fixture.Model.Refresh();
	REQUIRE(fixture.Model.GetRecent().size() == expected.size());
	for (size_t index = 0; index < expected.size(); index++)
	{
		CHECK(fixture.Model.GetRecent()[index].State == expected[index].State);
		CHECK(fixture.Model.GetRecent()[index].Name == expected[index].Name);
	}
	// A file that appears or goes changes its state at the next read.
	REQUIRE(FileSystem::Remove(valid).IsOk());
	CHECK(fixture.Model.DescribeRecentProjects().front().State == RecentProjectState::Missing);
}

TEST_CASE("StartScreenModel: opening a project lists it first, saves the list and hides the screen")
{
	ModelFixture fixture;
	std::filesystem::path const file = fixture.Locations.GetUserDataFile(StartScreenModel::RecentProjectsFileName);
	CHECK(fixture.Model.GetRecentProjects().GetFile() == file);
	std::filesystem::path const first = fixture.Directory.GetPath() / "A" / "A.sproj";
	std::filesystem::path const second = fixture.Directory.GetPath() / "B" / "B.sproj";

	fixture.Model.SetVisible(true);
	REQUIRE(fixture.Model.OnProjectOpened(first).IsOk());
	CHECK_FALSE(fixture.Model.IsVisible());
	REQUIRE(fixture.Model.OnProjectOpened(second).IsOk());
	REQUIRE(fixture.Model.OnProjectOpened(first).IsOk());
	CHECK(FileSystem::Exists(file));

	// Another model reads the same list from the user data.
	StartScreenModel reloaded(fixture.Locations);
	REQUIRE(reloaded.GetRecentProjects().Load().IsOk());
	REQUIRE(reloaded.GetRecentProjects().GetProjects().size() == 2);
	CHECK(reloaded.GetRecentProjects().GetProjects()[0] == RecentProjects::NormalizePath(first));
	CHECK(reloaded.GetRecentProjects().GetProjects()[1] == RecentProjects::NormalizePath(second));

	fixture.Model.Refresh();
	REQUIRE(fixture.Model.GetRecent().size() == 2);
	// Removed under another spelling of its path.
	REQUIRE(fixture.Model.RemoveRecentProject(fixture.Directory.GetPath() / "A" / "." / "A.sproj").IsOk());
	REQUIRE(fixture.Model.GetRecentProjects().GetProjects().size() == 1);
	REQUIRE(fixture.Model.GetRecent().size() == 1);
	CHECK(fixture.Model.GetRecent().front().Name == "B");
	REQUIRE(reloaded.GetRecentProjects().Load().IsOk());
	CHECK(reloaded.GetRecentProjects().GetProjects().size() == 1);

	REQUIRE(fixture.Model.ClearRecentProjects().IsOk());
	CHECK(fixture.Model.GetRecentProjects().GetProjects().empty());
	CHECK(fixture.Model.GetRecent().empty());
	REQUIRE(reloaded.GetRecentProjects().Load().IsOk());
	CHECK(reloaded.GetRecentProjects().GetProjects().empty());

	// Without user data nothing is written, and the list still works in memory.
	StartScreenModel memoryOnly{EditorLocations{}};
	REQUIRE(memoryOnly.OnProjectOpened(first).IsOk());
	CHECK(memoryOnly.GetRecentProjects().GetProjects().size() == 1);
	CHECK(memoryOnly.GetRecentProjects().GetFile().empty());
}

TEST_CASE("StartScreenModel: the New Project form says what it creates and why it cannot")
{
	ModelFixture fixture;
	fixture.Model.Refresh();
	NewProjectForm& form = fixture.Model.GetForm();
	CHECK(form.Name == "New Game");
	CHECK(form.Location == FileSystem::PathToUtf8(fixture.Locations.DefaultProjectLocation));
	CHECK(form.TemplateId == "empty-3d");
	CHECK(form.SetUpAgents);
	CHECK(form.GitIgnore);

	// The default location need not exist yet: it is made with the project.
	NewProjectCheck check = fixture.Model.CheckForm();
	CHECK(check.CanCreate());
	CHECK(check.Reason.empty());
	CHECK(check.Warning.empty());
	CHECK(check.DirectoryName == "New Game");
	CHECK(check.Directory == fixture.Locations.DefaultProjectLocation / "New Game");
	CHECK_FALSE(FileSystem::Exists(fixture.Locations.DefaultProjectLocation));

	// Names that need a portable form: the folder is named in it.
	for (auto const& [name, portable] : std::vector<std::pair<std::string, std::string>>{
			 {"Orb: Run?", "Orb_ Run_"}, {"  Spaced  ", "Spaced"}, {"CON", "CON_"}, {"a/b\\c", "a_b_c"}, {"名前", "______"}})
	{
		CAPTURE(name);
		form.Name = name;
		check = fixture.Model.CheckForm();
		CHECK(check.CanCreate());
		CHECK(check.DirectoryName == portable);
		CHECK(check.Directory == fixture.Locations.DefaultProjectLocation / FileSystem::PathFromUtf8(portable));
	}

	form.Name = "   ";
	check = fixture.Model.CheckForm();
	CHECK(check.Problem == NewProjectProblem::NoName);
	CHECK_FALSE(check.Reason.empty());
	CHECK(check.Directory.empty());

	form.Name = "Game";
	form.Location.clear();
	check = fixture.Model.CheckForm();
	CHECK(check.Problem == NewProjectProblem::NoLocation);
	CHECK_FALSE(check.Reason.empty());
	CHECK(check.Directory.empty());

	form.Location = "Relative/Folder";
	CHECK(fixture.Model.CheckForm().Problem == NewProjectProblem::RelativeLocation);

	form.Location = FileSystem::PathToUtf8(fixture.Directory.GetPath());
	form.TemplateId = "nope";
	CHECK(fixture.Model.CheckForm().Problem == NewProjectProblem::UnknownTemplate);
	form.TemplateId = "empty-3d";

	// A target that holds something is taken; an empty folder is not; a file is.
	fixture.Write("Game/Keep.txt", "keep");
	check = fixture.Model.CheckForm();
	CHECK(check.Problem == NewProjectProblem::TargetTaken);
	CHECK(check.Reason.find(FileSystem::PathToUtf8(fixture.Directory.GetPath() / "Game")) != std::string::npos);
	REQUIRE(FileSystem::Remove(fixture.Directory.GetPath() / "Game" / "Keep.txt").IsOk());
	CHECK(fixture.Model.CheckForm().CanCreate());
	REQUIRE(FileSystem::Remove(fixture.Directory.GetPath() / "Game").IsOk());
	fixture.Write("Game", "a file");
	CHECK(fixture.Model.CheckForm().Problem == NewProjectProblem::TargetTaken);

	// A template that needs the .NET SDK warns on a machine without one, and can still be created.
	form.Name = "Other";
	form.TemplateId = "tiny-project";
	check = fixture.Model.CheckForm();
	CHECK(check.CanCreate());
	CHECK(check.Warning.find(".NET 10 SDK") != std::string::npos);
	fixture.Environment.DotNetSdk = fixture.Directory.GetPath() / "dotnet";
	fixture.Model.Refresh();
	CHECK(fixture.Model.CheckForm().Warning.empty());

	fixture.Model.ResetForm();
	CHECK(fixture.Model.GetForm().Name == "New Game");
	CHECK(fixture.Model.GetForm().TemplateId == "empty-3d");
	CHECK(fixture.Model.GetForm().Location == FileSystem::PathToUtf8(fixture.Locations.DefaultProjectLocation));

	// Without a default location (no Documents folder) the form starts without one.
	StartScreenModel noDefault{EditorLocations{}};
	noDefault.Refresh();
	CHECK(noDefault.GetForm().Location.empty());
	CHECK(noDefault.CheckForm().Problem == NewProjectProblem::NoLocation);
}

TEST_CASE("StartScreenModel: the agents expander lists the kit's files, with the .gitignore only when the form adds one")
{
	ModelFixture fixture;
	std::vector<std::string> const withGitIgnore = fixture.Model.ListAgentFiles();
	CHECK(withGitIgnore == AgentKit::ListFiles(fixture.Locations.Layout, AgentSetupOptions{}));
	CHECK(std::ranges::find(withGitIgnore, ".gitignore") != withGitIgnore.end());
	CHECK(std::ranges::find(withGitIgnore, "AGENTS.md") != withGitIgnore.end());
	CHECK(std::ranges::find(withGitIgnore, ".mcp.json") != withGitIgnore.end());

	fixture.Model.GetForm().GitIgnore = false;
	std::vector<std::string> const without = fixture.Model.ListAgentFiles();
	CHECK(std::ranges::find(without, ".gitignore") == without.end());
	CHECK(without.size() + 1 == withGitIgnore.size());
}

TEST_CASE("StartScreenModel: an example's copy goes to a free folder named after it, numbered when taken")
{
	ModelFixture fixture;
	fixture.Model.Refresh();
	ExampleEntry const tiny = FindExample(fixture.Model, "tiny");
	std::filesystem::path const location = fixture.Locations.DefaultProjectLocation;

	CHECK(fixture.Model.GetExampleCopyTarget(tiny) == location / "Tiny");
	// An empty folder is free.
	REQUIRE(FileSystem::CreateDirectories(location / "Tiny").IsOk());
	CHECK(fixture.Model.GetExampleCopyTarget(tiny) == location / "Tiny");
	fixture.Write("Projects/Tiny/Tiny.sproj", "{}");
	CHECK(fixture.Model.GetExampleCopyTarget(tiny) == location / "Tiny 2");
	fixture.Write("Projects/Tiny 2", "a file");
	fixture.Write("Projects/Tiny 3/Keep.txt", "keep");
	CHECK(fixture.Model.GetExampleCopyTarget(tiny) == location / "Tiny 4");

	// A name that needs a portable form, a relative or missing location: none or the portable folder.
	ExampleEntry odd = tiny;
	odd.Name = "Odd: Name";
	CHECK(fixture.Model.GetExampleCopyTarget(odd) == location / "Odd_ Name");
	odd.Name = "  ";
	CHECK_FALSE(fixture.Model.GetExampleCopyTarget(odd).has_value());
	fixture.Model.GetForm().Location = "Relative";
	CHECK_FALSE(fixture.Model.GetExampleCopyTarget(tiny).has_value());
	fixture.Model.GetForm().Location.clear();
	CHECK_FALSE(fixture.Model.GetExampleCopyTarget(tiny).has_value());
}

TEST_CASE("StartScreenModel: the environment is what the editor was told, and nothing without a provider")
{
	ModelFixture fixture;
	fixture.Model.Refresh();
	StartScreenEnvironment const& none = fixture.Model.GetEnvironment();
	CHECK_FALSE(none.DotNetRuntime.has_value());
	CHECK_FALSE(none.DotNetSdk.has_value());
	CHECK_FALSE(none.Gpu.has_value());
	CHECK_FALSE(none.Vulkan.has_value());

	fixture.Environment.DotNetRuntime = ".NET 10";
	fixture.Environment.DotNetSdk = fixture.Directory.GetPath() / "dotnet" / "dotnet.exe";
	fixture.Environment.Gpu = "Test Adapter";
	fixture.Environment.Vulkan = "1.4.0";
	// Read at Refresh only.
	CHECK_FALSE(fixture.Model.GetEnvironment().DotNetRuntime.has_value());
	fixture.Model.Refresh();
	CHECK(fixture.Model.GetEnvironment().DotNetRuntime == ".NET 10");
	CHECK(fixture.Model.GetEnvironment().DotNetSdk == fixture.Directory.GetPath() / "dotnet" / "dotnet.exe");
	CHECK(fixture.Model.GetEnvironment().Gpu == "Test Adapter");
	CHECK(fixture.Model.GetEnvironment().Vulkan == "1.4.0");

	// A runtime without an SDK, and the other way round.
	fixture.Environment.DotNetSdk.reset();
	fixture.Model.Refresh();
	CHECK(fixture.Model.GetEnvironment().DotNetRuntime.has_value());
	CHECK_FALSE(fixture.Model.GetEnvironment().DotNetSdk.has_value());
	fixture.Environment.DotNetRuntime.reset();
	fixture.Environment.DotNetSdk = fixture.Directory.GetPath() / "dotnet";
	fixture.Model.Refresh();
	CHECK_FALSE(fixture.Model.GetEnvironment().DotNetRuntime.has_value());
	CHECK(fixture.Model.GetEnvironment().DotNetSdk.has_value());

	// A model without services knows nothing and opens nothing.
	StartScreenModel bare{fixture.Locations};
	bare.Refresh();
	CHECK_FALSE(bare.GetEnvironment().Gpu.has_value());
	CHECK(bare.OpenWithSystem(fixture.Directory.GetPath()).IsError());
}

TEST_CASE("StartScreenModel: the guides are those of the engine's docs that exist, and none without docs")
{
	ModelFixture fixture;
	fixture.Model.Refresh();
	std::vector<std::string> titles;
	for (StartScreenGuide const& guide : fixture.Model.GetGuides())
	{
		CHECK(FileSystem::IsRegularFile(guide.File));
		titles.push_back(guide.Title);
	}
	// The test content has no Editor.md: "The editor" is not listed.
	CHECK(titles == std::vector<std::string>{"Getting started", "Scripting", "Visual scripting", "Agents"});
	CHECK(fixture.Model.GetGuides().front().File == Testing::GetTestContentDirectory() / "Docs" / "GettingStarted.md");

	// The links open through the editor's handler; Show in Folder opens the project's folder.
	REQUIRE(fixture.Model.OpenWithSystem(fixture.Model.GetGuides().front().File).IsOk());
	std::filesystem::path const project = fixture.Directory.GetPath() / "Game" / "Game.sproj";
	REQUIRE(fixture.Model.ShowInFolder(project).IsOk());
	CHECK(fixture.Opened == std::vector<std::filesystem::path>{fixture.Model.GetGuides().front().File, project.parent_path()});

	// An engine content without a Docs folder, and no engine content at all.
	std::filesystem::path const content = fixture.Directory.GetPath() / "Content";
	REQUIRE(FileSystem::CreateDirectories(content).IsOk());
	Result<EngineLayout> layout = EngineLayout::Resolve(content / "StradaEditor");
	REQUIRE(layout.IsOk());
	CHECK(StartScreenModel::ListGuides(layout.GetValue()).empty());
	CHECK(StartScreenModel::ListGuides(EngineLayout{}).empty());

	// No engine content: the default scene still stands in for empty-3d, and there are no examples.
	StartScreenModel bare{EditorLocations{}};
	bare.Refresh();
	REQUIRE(bare.GetTemplates().Templates.size() == 1);
	CHECK(bare.GetTemplates().Templates.front().Id == "empty-3d");
	CHECK_FALSE(bare.GetExamples().has_value());
	CHECK_FALSE(bare.GetExamplesError().empty());
	CHECK(bare.GetGuides().empty());
}

TEST_CASE("StartScreenModel: the templates and examples are the engine content's")
{
	ModelFixture fixture;
	CHECK(fixture.Model.GetTemplates().Templates.empty());
	fixture.Model.Refresh();
	std::vector<std::string> ids;
	for (ProjectTemplate const& projectTemplate : fixture.Model.GetTemplates().Templates)
	{
		ids.push_back(projectTemplate.Id);
	}
	CHECK(ids == std::vector<std::string>{"empty-3d", "tiny-project"});
	REQUIRE(fixture.Model.GetExamples().has_value());
	CHECK(fixture.Model.GetExamples()->GetExamples().size() == 2);
	CHECK(fixture.Model.GetExamplesError().empty());
	ExampleEntry const fetched = FindExample(fixture.Model, "fetched");
	CHECK_FALSE(fixture.Model.GetExamples()->IsAvailable(fetched));
}
