#include "EditorLocationsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/ProjectContentCommands.h"
#include "Editor/Automation/StartCommands.h"
#include "Editor/EditorOperations.h"
#include "Editor/ExampleCopyTask.h"
#include "Editor/StartScreen/StartScreenModel.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Project/Project.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// The editor model with the editor's, the project content and the start commands, as the editor layer sets them up: an
	// opened project goes to the start screen's recent projects. With a window, start.show shows the screen.
	struct StartFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		ExampleCopyTask Copies;
		EditorLocations Locations;
		std::optional<StartScreenEnvironment> Environment;
		StartScreenModel StartScreen;
		CommandRegistry Registry;
		std::optional<std::filesystem::path> ToolOnPath;
		// With a window: what editor.dialog asked of the editor, and why the editor refuses the next request (none: it
		// accepts).
		std::vector<std::pair<EditorDialog, bool>> Dialogs;
		std::optional<std::string> DialogRefusal;

		// The locations come from the temporary directory: the test content's by default.
		explicit StartFixture(bool withWindow = false, std::function<EditorLocations(std::filesystem::path const&)> const& makeLocations =
		                                                   Testing::MakeEditorLocations)
			: Locations(makeLocations(Directory.GetPath())),
			  StartScreen(Locations, MakeServices())
		{
			EditorCommandEnvironment environment;
			environment.Locations = Locations;
			environment.ProjectOpened = [this]
			{
				OnProjectOpened();
			};
			environment.FindToolOnPath = [this]
			{
				return ToolOnPath;
			};
			REQUIRE(RegisterEditorCommands(Registry, Operations, std::move(environment)).IsOk());
			REQUIRE(RegisterProjectContentCommands(Registry, Operations, Copies, Locations,
			                                       [this]
			                                       {
													   OnProjectOpened();
												   })
			            .IsOk());
			StartCommandsHost host;
			if (withWindow)
			{
				host.ShowStartScreen = [this](bool visible)
				{
					StartScreen.SetVisible(visible);
				};
				host.ShowDialog = [this](EditorDialog dialog, bool open) -> Result<void>
				{
					if (DialogRefusal)
					{
						return Error{*DialogRefusal};
					}
					Dialogs.emplace_back(dialog, open);
					return {};
				};
			}
			REQUIRE(RegisterStartCommands(Registry, StartScreen, std::move(host)).IsOk());
		}

		StartScreenServices MakeServices()
		{
			StartScreenServices services;
			services.DescribeEnvironment = [this]
			{
				return Environment.value_or(StartScreenEnvironment{});
			};
			return services;
		}

		void OnProjectOpened()
		{
			REQUIRE(Context.GetProject() != nullptr);
			CHECK(StartScreen.OnProjectOpened(Context.GetProject()->GetFilePath()).IsOk());
		}

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
			CHECK_FALSE(result.GetError().Message.empty());
			return result.GetError().Code;
		}

		std::string Path(std::filesystem::path const& relative) const { return FileSystem::PathToUtf8(Directory.GetPath() / relative); }

		std::filesystem::path WriteProjectFile(std::filesystem::path const& relative, std::string const& name) const
		{
			std::filesystem::path const file = Directory.GetPath() / relative;
			REQUIRE(FileSystem::CreateDirectories(file.parent_path()).IsOk());
			REQUIRE(FileSystem::WriteTextFile(file, R"({ "Strada": { "Version": 1, "Type": "Project" }, "Project": { "Name": ")" + name +
			                                            R"(" } })")
			            .IsOk());
			return file;
		}
	};

	std::vector<std::string> Strings(Json const& array, char const* key)
	{
		std::vector<std::string> values;
		for (Json const& entry : array)
		{
			values.push_back(entry[key].get<std::string>());
		}
		return values;
	}

	EditorLocations NoContent(std::filesystem::path const&)
	{
		return EditorLocations{};
	}

	// The test content's agent kit and guides in a folder of the temporary directory, with a strada beside its editor.
	EditorLocations ContentWithTool(std::filesystem::path const& temporaryRoot)
	{
		std::filesystem::path const content = temporaryRoot / "Content";
		for (char const* const folder : {"Agents", "Docs", "Templates"})
		{
			REQUIRE(FileSystem::CopyDirectory(Testing::GetTestContentDirectory() / folder, content / folder).IsOk());
		}
		Result<EngineLayout> layout = EngineLayout::Resolve(content / "StradaEditor");
		REQUIRE(layout.IsOk());
		REQUIRE(FileSystem::WriteTextFile(layout.GetValue().GetToolPath(), "a tool").IsOk());
		EditorLocations locations;
		locations.Layout = layout.TakeValue();
		return locations;
	}

	// The command of the strada entry of a project's .mcp.json.
	std::string ReadStradaCommand(std::filesystem::path const& project)
	{
		Result<std::string> const text = FileSystem::ReadTextFile(project / ".mcp.json");
		REQUIRE(text.IsOk());
		Result<Json> const document = ParseJson(text.GetValue());
		REQUIRE(document.IsOk());
		return document.GetValue().at("mcpServers").at("strada").at("command").get<std::string>();
	}

	bool HasFile(Json const& files, std::string_view path)
	{
		return std::ranges::any_of(files,
		                           [path](Json const& file)
		                           {
									   return file["path"].get<std::string>() == path;
								   });
	}
}

TEST_CASE("StartCommands: start.info gives what the start screen shows, as the content and the editor have it")
{
	StartFixture fixture;
	Json info = fixture.Ok("start.info");
	CHECK(info["visible"] == false);
	CHECK(info["defaultLocation"] == FileSystem::PathToUtf8(fixture.Locations.DefaultProjectLocation));
	CHECK(info["recent"] == Json::array());
	CHECK(Strings(info["templates"], "id") == std::vector<std::string>{"empty-3d", "tiny-project"});
	// As project.templates and project.examples give them.
	CHECK(info["templates"] == fixture.Ok("project.templates")["templates"]);
	CHECK(info["examples"] == fixture.Ok("project.examples")["examples"]);
	CHECK(Strings(info["examples"], "id") == std::vector<std::string>{"tiny", "fetched"});
	CHECK(info["examples"][1]["available"] == false);
	CHECK(info["examples"][1]["hint"].get<std::string>().starts_with("Run: "));
	CHECK(Strings(info["docs"], "title") == std::vector<std::string>{"Getting started", "Scripting", "Visual scripting", "Agents"});
	CHECK(info["docs"][0]["path"] == FileSystem::PathToUtf8(Testing::GetTestContentDirectory() / "Docs" / "GettingStarted.md"));
	// Nothing known of the machine: every value is null.
	CHECK(info["environment"] == Json::object({{"dotnetRuntime", Json()}, {"dotnetSdk", Json()}, {"gpu", Json()}, {"vulkan", Json()}}));

	// What the editor knows, read at each call; the recent projects with their states.
	std::filesystem::path const sdk = fixture.Directory.GetPath() / "dotnet" / "dotnet.exe";
	fixture.Environment = StartScreenEnvironment{".NET 10", sdk, "Test Adapter", "1.4.0"};
	std::filesystem::path const project = fixture.WriteProjectFile("Orb/Orb Run.sproj", "Orb Run");
	std::filesystem::path const missing = fixture.Directory.GetPath() / "Gone" / "Gone.sproj";
	fixture.StartScreen.GetRecentProjects().Add(missing);
	fixture.StartScreen.GetRecentProjects().Add(project);
	info = fixture.Ok("start.info");
	CHECK(info["environment"] ==
	      Json::object(
			  {{"dotnetRuntime", ".NET 10"}, {"dotnetSdk", FileSystem::PathToUtf8(sdk)}, {"gpu", "Test Adapter"}, {"vulkan", "1.4.0"}}));
	REQUIRE(info["recent"].size() == 2);
	CHECK(info["recent"][0] ==
	      Json::object({{"name", "Orb Run"}, {"path", FileSystem::PathToUtf8(RecentProjects::NormalizePath(project))}, {"state", "Ok"}}));
	CHECK(info["recent"][1]["name"] == "Gone");
	CHECK(info["recent"][1]["state"] == "Missing");

	// start.info is read-only and takes no parameters.
	CHECK(fixture.Registry.Find("start.info")->ReadOnly);
	CHECK(fixture.Fails("start.info", Json::object({{"unexpected", 1}})) == AutomationErrorCode::InvalidParams);
}

TEST_CASE("StartCommands: an editor without engine content still describes its start screen")
{
	StartFixture fixture(false, NoContent);
	Json const info = fixture.Ok("start.info");
	CHECK(info["defaultLocation"].is_null());
	CHECK(Strings(info["templates"], "id") == std::vector<std::string>{"empty-3d"});
	CHECK(info["examples"] == Json::array());
	CHECK(info["docs"] == Json::array());
}

TEST_CASE("StartCommands: start.show shows and hides the start screen, which opening a project hides; headless it is unavailable")
{
	{
		StartFixture headless;
		CHECK(headless.Fails("start.show", Json::object({{"visible", true}})) == AutomationErrorCode::Unavailable);
		CHECK_FALSE(headless.StartScreen.IsVisible());
		CHECK(headless.Ok("start.info")["visible"] == false);
	}

	StartFixture fixture(true);
	CHECK(fixture.Fails("start.show") == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("start.show", Json::object({{"visible", "yes"}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Ok("start.show", Json::object({{"visible", true}}))["visible"] == true);
	CHECK(fixture.StartScreen.IsVisible());
	CHECK(fixture.Ok("start.info")["visible"] == true);
	CHECK(fixture.Ok("start.show", Json::object({{"visible", false}}))["visible"] == false);
	CHECK_FALSE(fixture.StartScreen.IsVisible());

	// Creating a project, by any path, hides it and lists the project first.
	fixture.Ok("start.show", Json::object({{"visible", true}}));
	Json const created = fixture.Ok("project.create", Json::object({{"directory", fixture.Path("Game")}, {"name", "Game"}}));
	CHECK_FALSE(fixture.StartScreen.IsVisible());
	Json const recent = fixture.Ok("editor.recent")["projects"];
	REQUIRE(recent.size() == 1);
	CHECK(recent[0]["name"] == "Game");
	CHECK(recent[0]["state"] == "Ok");
	CHECK(FileSystem::PathFromUtf8(recent[0]["path"].get<std::string>()) ==
	      RecentProjects::NormalizePath(FileSystem::PathFromUtf8(created["project"]["file"].get<std::string>())));
	// Shown again with a project open (File > Start Screen), then a copied example opens and hides it.
	fixture.Ok("start.show", Json::object({{"visible", true}}));
	std::optional<CommandResult> copied;
	fixture.Registry.Execute("project.copy-example", Json::object({{"example", "tiny"}, {"directory", fixture.Path("Tiny")}}),
	                         [&copied](CommandResult value)
	                         {
								 copied.emplace(std::move(value));
							 });
	auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
	while (!copied && std::chrono::steady_clock::now() < deadline)
	{
		fixture.Copies.Update();
		if (!copied)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}
	REQUIRE(copied.has_value());
	REQUIRE(copied->IsOk());
	CHECK_FALSE(fixture.StartScreen.IsVisible());
	CHECK(fixture.Ok("editor.recent")["projects"].size() == 2);
}

TEST_CASE("StartCommands: editor.recent lists the recent projects, removes one or clears them, and saves the list")
{
	StartFixture fixture;
	std::filesystem::path const first = fixture.WriteProjectFile("A/A.sproj", "Alpha");
	std::filesystem::path const second = fixture.WriteProjectFile("B/B.sproj", "Beta");
	std::filesystem::path const garbage = fixture.Directory.GetPath() / "C" / "C.sproj";
	REQUIRE(FileSystem::WriteTextFile(garbage, "garbage").IsOk());
	for (std::filesystem::path const& project : {garbage, second, first})
	{
		REQUIRE(fixture.StartScreen.OnProjectOpened(project).IsOk());
	}
	std::filesystem::path const file = fixture.Locations.GetUserDataFile(StartScreenModel::RecentProjectsFileName);
	auto const saved = [&file]
	{
		RecentProjects reloaded(file);
		REQUIRE(reloaded.Load().IsOk());
		return reloaded.GetProjects().size();
	};

	Json projects = fixture.Ok("editor.recent")["projects"];
	CHECK(Strings(projects, "name") == std::vector<std::string>{"Alpha", "Beta", "C"});
	CHECK(Strings(projects, "state") == std::vector<std::string>{"Ok", "Ok", "Unreadable"});

	// Under another spelling of its path; one that is not listed changes nothing.
	projects = fixture.Ok("editor.recent", Json::object({{"remove", fixture.Path("B/../B/B.sproj")}}))["projects"];
	CHECK(Strings(projects, "name") == std::vector<std::string>{"Alpha", "C"});
	CHECK(saved() == 2);
	projects = fixture.Ok("editor.recent", Json::object({{"remove", fixture.Path("Nowhere/Nowhere.sproj")}}))["projects"];
	CHECK(projects.size() == 2);

	// Both at once are refused, and change nothing.
	CHECK(fixture.Fails("editor.recent", Json::object({{"remove", FileSystem::PathToUtf8(first)}, {"clear", true}})) ==
	      AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("editor.recent", Json::object({{"remove", ""}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("editor.recent", Json::object({{"clear", "yes"}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Ok("editor.recent")["projects"].size() == 2);

	// clear false changes nothing; clear true empties the list.
	CHECK(fixture.Ok("editor.recent", Json::object({{"clear", false}}))["projects"].size() == 2);
	CHECK(fixture.Ok("editor.recent", Json::object({{"clear", true}}))["projects"] == Json::array());
	CHECK(saved() == 0);
	CHECK_FALSE(fixture.Registry.Find("editor.recent")->ReadOnly);
}

TEST_CASE("StartCommands: project.create sets a new project up for coding agents unless asked not to")
{
	StartFixture fixture;
	Json const created = fixture.Ok("project.create", Json::object({{"directory", fixture.Path("Orb")}, {"name", "Orb Run"}}));
	Json const& agents = created["agents"];
	REQUIRE(agents.is_object());
	std::filesystem::path const orb = fixture.Directory.GetPath() / "Orb";
	// Every file the kit lists, created; the .gitignore the template wrote already ignores .strada/.
	std::vector<std::string> const listed = AgentKit::ListFiles(fixture.Locations.Layout);
	CHECK(Strings(agents["files"], "path") == listed);
	for (Json const& file : agents["files"])
	{
		CAPTURE(file.dump());
		CHECK(file["action"] == (file["path"] == ".gitignore" ? "Unchanged" : "Create"));
		CHECK(FileSystem::IsRegularFile(orb / FileSystem::PathFromUtf8(file["path"].get<std::string>())));
	}
	CHECK(agents["files"][0].size() == 2);
	// No strada on PATH and none beside the fixture's editor: .mcp.json names strada and says to put it on PATH.
	CHECK_FALSE(agents["warnings"].empty());
	CHECK(ReadStradaCommand(orb) == "strada");
	// As project.setup-agents would leave it: a setup now changes nothing.
	Json const again = fixture.Ok("project.setup-agents", Json::object({{"dryRun", true}}));
	for (Json const& file : again["files"])
	{
		CHECK(file["action"] == "Unchanged");
	}

	// agents false: none of the kit's files.
	Json const plain =
		fixture.Ok("project.create",
	               Json::object({{"directory", fixture.Path("Plain")}, {"name", "Plain"}, {"agents", false}, {"discardChanges", true}}));
	CHECK(plain["agents"].is_null());
	for (std::string const& path : listed)
	{
		CAPTURE(path);
		CHECK(FileSystem::Exists(fixture.Directory.GetPath() / "Plain" / FileSystem::PathFromUtf8(path)) == (path == ".gitignore"));
	}

	// Without a .gitignore the kit writes none either; from a template with a project of its own too.
	Json const bare = fixture.Ok("project.create", Json::object({{"directory", fixture.Path("Bare")},
	                                                             {"name", "Bare"},
	                                                             {"template", "tiny-project"},
	                                                             {"gitIgnore", false},
	                                                             {"discardChanges", true}}));
	CHECK_FALSE(HasFile(bare["agents"]["files"], ".gitignore"));
	CHECK(HasFile(bare["agents"]["files"], "AGENTS.md"));
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Bare" / ".gitignore"));
	CHECK(FileSystem::IsRegularFile(fixture.Directory.GetPath() / "Bare" / "AGENTS.md"));
}

TEST_CASE("StartCommands: project.create names the strada the lookup finds, and a setup that fails leaves the project created")
{
	{
		// A strada on PATH that is not the one beside the editor: .mcp.json names the editor's by its path, with a warning.
		StartFixture fixture(false, ContentWithTool);
		fixture.ToolOnPath = fixture.Directory.GetPath() / "Elsewhere" / "strada";
		Json const created = fixture.Ok("project.create", Json::object({{"directory", fixture.Path("Game")}, {"name", "Game"}}));
		CHECK(ReadStradaCommand(fixture.Directory.GetPath() / "Game") == FileSystem::PathToUtf8(fixture.Locations.Layout.GetToolPath()));
		CHECK_FALSE(created["agents"]["warnings"].empty());

		// The one on PATH is the one beside the editor: .mcp.json names strada.
		fixture.ToolOnPath = fixture.Locations.Layout.GetToolPath();
		fixture.Ok("project.create", Json::object({{"directory", fixture.Path("Other")}, {"name", "Other"}}));
		CHECK(ReadStradaCommand(fixture.Directory.GetPath() / "Other") == "strada");
	}

	// An editor without the engine's agent kit creates the project and says why it was not set up.
	StartFixture bare(false, NoContent);
	Json const plain = bare.Ok("project.create", Json::object({{"directory", bare.Path("Plain")}, {"name", "Plain"}}));
	CHECK(plain["project"]["name"] == "Plain");
	CHECK(plain["agents"]["files"] == Json::array());
	REQUIRE(plain["agents"]["warnings"].size() >= 1);
	CHECK(plain["agents"]["warnings"].back().get<std::string>().starts_with("the project was not set up for coding agents: "));
	CHECK_FALSE(FileSystem::Exists(bare.Directory.GetPath() / "Plain" / "AGENTS.md"));
	CHECK(bare.Context.GetProject() != nullptr);
}

TEST_CASE("StartCommands: the commands are documented, and none is an undo step")
{
	StartFixture fixture;
	for (char const* const name : {"start.info", "start.show", "editor.recent", "editor.dialog"})
	{
		CAPTURE(name);
		CommandDefinition const* command = fixture.Registry.Find(name);
		REQUIRE(command != nullptr);
		CHECK_FALSE(command->Description.empty());
	}
	// Nothing they do enters the history.
	fixture.Ok("start.info");
	fixture.Ok("editor.recent", Json::object({{"clear", true}}));
	CHECK_FALSE(fixture.Context.GetHistory().CanUndo());
}

TEST_CASE("StartCommands: editor.dialog opens and closes the File menu's dialogs; headless it is unavailable")
{
	for (EditorDialog const dialog : {EditorDialog::NewProject, EditorDialog::AgentSetup})
	{
		CHECK(ParseEditorDialog(GetEditorDialogName(dialog)) == dialog);
	}
	CHECK_FALSE(ParseEditorDialog("about").has_value());
	{
		StartFixture headless;
		CHECK(headless.Fails("editor.dialog", Json::object({{"name", "new-project"}})) == AutomationErrorCode::Unavailable);
	}

	StartFixture fixture(true);
	CHECK(fixture.Fails("editor.dialog") == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("editor.dialog", Json::object({{"name", "about"}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("editor.dialog", Json::object({{"name", "new-project"}, {"open", "yes"}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Dialogs.empty());

	// Opened by default, closed with open false.
	CHECK(fixture.Ok("editor.dialog", Json::object({{"name", "new-project"}})) == Json::object({{"name", "new-project"}, {"open", true}}));
	CHECK(fixture.Ok("editor.dialog", Json::object({{"name", "agent-setup"}, {"open", false}})) ==
	      Json::object({{"name", "agent-setup"}, {"open", false}}));
	CHECK(fixture.Dialogs ==
	      std::vector<std::pair<EditorDialog, bool>>{{EditorDialog::NewProject, true}, {EditorDialog::AgentSetup, false}});

	// What the editor refuses (another dialog is open, no project for the agent setup) is an InvalidOperation saying why.
	fixture.DialogRefusal = "another dialog is open (Unsaved Changes): close it first";
	CommandResult const refused = fixture.Run("editor.dialog", Json::object({{"name", "agent-setup"}}));
	REQUIRE(refused.IsError());
	CHECK(refused.GetError().Code == AutomationErrorCode::InvalidOperation);
	CHECK(refused.GetError().Message == *fixture.DialogRefusal);
	CHECK(fixture.Dialogs.size() == 2);
	CHECK_FALSE(fixture.Registry.Find("editor.dialog")->ReadOnly);
	CHECK_FALSE(fixture.Context.GetHistory().CanUndo());
}
