#include "EditorLocationsTestUtilities.h"
#include "GpuTestUtilities.h"
#include "HeadlessImGui.h"
#include "TestUtilities.h"

#include "Editor/Agents/AgentKit.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorShortcuts.h"
#include "Editor/ExampleCopyTask.h"
#include "Editor/Panels/AgentSetupDialog.h"
#include "Editor/Panels/SceneHierarchyPanel.h"
#include "Editor/StartScreen/NewProjectDialog.h"
#include "Editor/StartScreen/StartScreenModel.h"
#include "Editor/StartScreen/StartScreenPanel.h"
#include "Editor/StartScreen/StartScreenThumbnails.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Project/Project.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <spdlog/fmt/fmt.h>

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
using Strada::Testing::HeadlessImGui;

namespace
{
	// The test content with as much as the engine's: three templates, three examples and the five guides of the screen.
	EditorLocations MakeFullContent(std::filesystem::path const& temporaryRoot)
	{
		std::filesystem::path const content = temporaryRoot / "Content";
		REQUIRE(FileSystem::CopyDirectory(Testing::GetTestContentDirectory(), content).IsOk());
		// A third template: a copy of the project template under another id.
		std::filesystem::path const third = content / "Templates" / "third-project";
		REQUIRE(FileSystem::CopyDirectory(content / "Templates" / "tiny-project", third).IsOk());
		Result<std::string> templateText = FileSystem::ReadTextFile(third / "template.json");
		REQUIRE(templateText.IsOk());
		Result<Json> templateDocument = ParseJson(templateText.GetValue());
		REQUIRE(templateDocument.IsOk());
		Json described = templateDocument.TakeValue();
		// The project template describes itself at the length of the engine's longest description (csharp-starter's), which
		// the cards show whole.
		Json longer = described;
		longer["Template"]["Description"] = "Roll a glowing orb and collect 12 sparks before time runs out, written in C#, with a C# "
											"member that a graph calls as a node.";
		REQUIRE(FileSystem::WriteTextFile(content / "Templates" / "tiny-project" / "template.json", longer.dump(1, '\t')).IsOk());
		described["Template"]["Id"] = "third-project";
		described["Template"]["Name"] = "Third Project With A Name Long Enough To Be Cut";
		described["Template"]["Order"] = 3;
		REQUIRE(FileSystem::WriteTextFile(third / "template.json", described.dump(1, '\t')).IsOk());
		// A third example: Tiny again.
		std::filesystem::path const catalogFile = content / "Examples" / "catalog.json";
		Result<std::string> catalogText = FileSystem::ReadTextFile(catalogFile);
		REQUIRE(catalogText.IsOk());
		Result<Json> catalogDocument = ParseJson(catalogText.GetValue());
		REQUIRE(catalogDocument.IsOk());
		Json catalog = catalogDocument.TakeValue();
		catalog["Examples"].push_back(Json::object({{"Id", "tiny-again"},
		                                            {"Name", "Tiny Again"},
		                                            {"Order", 3},
		                                            {"Directory", "Tiny"},
		                                            {"ProjectFile", "Tiny.sproj"},
		                                            {"Description", "The tiny fixture once more, so that the list holds three, with a "
		                                                            "description as long as the Showcase's."},
		                                            {"Thumbnail", "Thumbnails/tiny.png"},
		                                            {"Requires", Json::object({{"Sdk", false}})}}));
		REQUIRE(FileSystem::WriteTextFile(catalogFile, catalog.dump(1, '\t')).IsOk());
		REQUIRE(FileSystem::WriteTextFile(content / "Docs" / "Editor.md", "# The editor\n").IsOk());

		Result<EngineLayout> layout = EngineLayout::Resolve(content / "StradaEditor");
		REQUIRE(layout.IsOk());
		EditorLocations locations;
		locations.Layout = layout.TakeValue();
		locations.UserDataDirectory = temporaryRoot / "UserData" / "Editor";
		locations.DefaultProjectLocation = temporaryRoot / "Projects";
		return locations;
	}

	// The start screen over the test content, drawn under a menu bar as the editor draws it, with actions that do what the
	// editor's do (without its unsaved-changes prompt): open projects, copy examples with the shared copy task, create
	// projects from the form.
	struct ScreenFixture
	{
		Testing::AssetManagerScope Assets;
		HeadlessImGui Gui;
		Testing::TemporaryDirectory Directory;
		EditorLocations Locations;
		EditorContext Context;
		EditorOperations Operations{Context};
		ExampleCopyTask Copies;
		// What the system's handler was asked to open.
		std::vector<std::filesystem::path> SystemOpened;
		StartScreenModel Model;
		StartScreenPanel Panel;
		StartScreenActions Actions;
		// The actions the screen asked for, by name.
		std::vector<std::string> Calls;
		std::optional<std::filesystem::path> PickedFolder;
		std::optional<NewProjectOutcome> Created;
		std::function<void()> Draw = [this]
		{
			DrawMenuBar();
			Panel.OnImGuiRender(Model, Copies, nullptr, Actions);
		};

		explicit ScreenFixture(
			std::function<EditorLocations(std::filesystem::path const&)> const& makeLocations = Testing::MakeEditorLocations)
			: Locations(makeLocations(Directory.GetPath())),
			  Model(Locations, MakeServices())
		{
			Gui.UseEditorStyle();
			Model.SetVisible(true);
			Actions.OpenProject = [this](std::filesystem::path const& file)
			{
				Calls.emplace_back("OpenProject");
				Open(file);
			};
			Actions.BrowseProject = [this]
			{
				Calls.emplace_back("BrowseProject");
			};
			Actions.OpenExample = [this](ExampleEntry const& example, std::filesystem::path const& target)
			{
				Calls.emplace_back("OpenExample");
				REQUIRE(Model.GetExamples().has_value());
				Result<void> const started =
					Copies.Start(*Model.GetExamples(), example, target,
				                 [this](Result<ExampleCopyOutcome> outcome, bool cancelled)
				                 {
									 Calls.emplace_back(cancelled ? "CopyCancelled" : (outcome ? "Copied" : "CopyFailed"));
									 if (outcome)
									 {
										 Open(outcome.GetValue().ProjectFile);
									 }
								 });
				REQUIRE(started.IsOk());
			};
			Actions.Continue = [this]
			{
				Calls.emplace_back("Continue");
				Model.SetVisible(false);
			};
			Actions.Form.PickFolder = [this](std::filesystem::path const&) -> Result<std::optional<std::filesystem::path>>
			{
				Calls.emplace_back("PickFolder");
				return PickedFolder;
			};
			Actions.Form.Create = [this](NewProjectRequest const& request)
			{
				Calls.emplace_back("Create");
				Result<NewProjectOutcome> created =
					CreateProjectFromRequest(Operations, request, Model.GetTemplates(), Locations.Layout, {});
				REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
				Created = created.TakeValue();
				CHECK(Model.OnProjectOpened(Created->ProjectFile).IsOk());
			};
		}

		StartScreenServices MakeServices()
		{
			StartScreenServices services;
			services.OpenWithSystem = [this](std::filesystem::path const& path) -> Result<void>
			{
				SystemOpened.push_back(path);
				return {};
			};
			return services;
		}

		static void DrawMenuBar()
		{
			if (ImGui::BeginMainMenuBar())
			{
				if (ImGui::BeginMenu("File"))
				{
					ImGui::EndMenu();
				}
				ImGui::EndMainMenuBar();
			}
		}

		void Open(std::filesystem::path const& file)
		{
			Result<std::vector<std::string>> opened = Operations.OpenProject(file);
			REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
			CHECK(Model.OnProjectOpened(Context.GetProject()->GetFilePath()).IsOk());
		}

		void Frames(int count = 3)
		{
			for (int frame = 0; frame < count; frame++)
			{
				Gui.Frame(Draw);
			}
		}

		StartScreenItem const* Find(std::string_view name) const
		{
			std::vector<StartScreenItem> const& items = Panel.GetItems();
			auto const found = std::ranges::find(items, name, &StartScreenItem::Name);
			return found != items.end() ? &*found : nullptr;
		}

		StartScreenItem Require(std::string_view name) const
		{
			StartScreenItem const* const item = Find(name);
			REQUIRE_MESSAGE(item != nullptr, name);
			return *item;
		}

		void Click(std::string_view name, ImGuiMouseButton button = ImGuiMouseButton_Left)
		{
			StartScreenItem const item = Require(name);
			REQUIRE_MESSAGE(item.Visible, name);
			Gui.MouseClick(item.GetCenter(), button, Draw);
			Frames();
		}

		// Draws frames and completes the copy until it ends.
		void FinishCopy()
		{
			auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
			while (Copies.IsRunning() && std::chrono::steady_clock::now() < deadline)
			{
				Gui.Frame(Draw);
				Copies.Update();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			REQUIRE_FALSE(Copies.IsRunning());
			Frames();
		}

		std::filesystem::path WriteProject(std::string const& name)
		{
			REQUIRE(Operations.CreateProject(Directory.GetPath() / "Existing" / name, name, Scene("Main")).IsOk());
			std::filesystem::path const file = Context.GetProject()->GetFilePath();
			REQUIRE(Operations.CloseProject().IsOk());
			return file;
		}
	};

	std::string RecentItem(std::filesystem::path const& file)
	{
		return "Recent:" + FileSystem::PathToUtf8(RecentProjects::NormalizePath(file));
	}

	// The items the screen must show whole at any editor size.
	std::vector<std::string> const& GetPrimaryItems()
	{
		static std::vector<std::string> const s_Items = {
			"Open Project...",
			"Continue without a project",
			"Create",
			"Browse...",
			"Name",
			"Location",
			"Set up for coding agents",
			"Add a .gitignore",
			"Template:empty-3d",
			"Template:tiny-project",
			"Template:third-project",
			"Example:tiny",
			"Example:fetched",
			"Guide:Getting started",
			"Guide:Agents",
		};
		return s_Items;
	}
}

TEST_CASE("StartScreenPanel: every area draws at 1600x900 and at 1280x720 with its primary buttons whole")
{
	for (ImVec2 const& size : {ImVec2(1600.0f, 900.0f), ImVec2(1280.0f, 720.0f)})
	{
		CAPTURE(size.x);
		ScreenFixture fixture(MakeFullContent);
		fixture.Gui.SetDisplaySize(size);
		// A full list: projects that open, one that is gone and one that does not read as a project.
		std::vector<std::filesystem::path> projects;
		for (int index = 0; index < 8; index++)
		{
			projects.push_back(fixture.WriteProject(fmt::format("Game {}", index)));
		}
		std::filesystem::path const gone = fixture.Directory.GetPath() / "Gone" / "Gone.sproj";
		std::filesystem::path const garbage = fixture.Directory.GetPath() / "Garbage" / "Garbage.sproj";
		REQUIRE(FileSystem::WriteTextFile(garbage, "not a project").IsOk());
		fixture.Model.GetRecentProjects().Add(gone);
		fixture.Model.GetRecentProjects().Add(garbage);
		for (std::filesystem::path const& project : projects)
		{
			fixture.Model.GetRecentProjects().Add(project);
		}
		size_t const steps = fixture.Context.GetHistory().GetUndoCount();
		fixture.Frames(6);

		for (std::string const& name : GetPrimaryItems())
		{
			StartScreenItem const item = fixture.Require(name);
			CHECK_MESSAGE(item.Visible, name);
			CHECK_MESSAGE(item.Max.x <= size.x, name);
			CHECK_MESSAGE(item.Max.y <= size.y, name);
		}
		// Under the menu bar.
		CHECK(fixture.Require("Template:empty-3d").Min.y >= ImGui::GetMainViewport()->WorkPos.y);
		// Every area is there: the recent projects (the list scrolls), the third example, the five guides.
		CHECK(fixture.Find(RecentItem(projects.back())) != nullptr);
		CHECK(fixture.Find(RecentItem(gone)) != nullptr);
		CHECK(fixture.Find("Example:tiny-again") != nullptr);
		for (char const* const guide :
		     {"Guide:Getting started", "Guide:The editor", "Guide:Scripting", "Guide:Visual scripting", "Guide:Agents"})
		{
			CHECK_MESSAGE(fixture.Find(guide) != nullptr, guide);
		}
		// Every card shows its badge and description whole, and its name unless it is wider than the card (the third
		// template's, which its tooltip shows).
		for (std::string_view const card :
		     {"Template:empty-3d", "Template:tiny-project", "Example card:tiny", "Example card:fetched", "Example card:tiny-again"})
		{
			CHECK_MESSAGE(fixture.Require(card).TextWhole, card);
		}
		CHECK_FALSE(fixture.Require("Template:third-project").TextWhole);
		// Create follows the form's last field, and Learn the last example, rather than the bottom of their columns.
		float const rowHeight = ImGui::GetFrameHeightWithSpacing();
		CHECK(fixture.Require("Create").Min.y - fixture.Require("Add a .gitignore").Max.y < rowHeight * 2.0f);
		CHECK(fixture.Require("Guide:Getting started").Min.y - fixture.Require("Example card:tiny-again").Max.y < rowHeight * 3.0f);
		// The unavailable example's Open is disabled; Create can create the default form.
		CHECK_FALSE(fixture.Require("Example:fetched").Enabled);
		CHECK(fixture.Require("Example:tiny").Enabled);
		CHECK(fixture.Require("Create").Enabled);
		CHECK_FALSE(fixture.Require(RecentItem(gone)).Enabled);
		CHECK_FALSE(fixture.Require(RecentItem(garbage)).Enabled);
		CHECK(fixture.Require(RecentItem(projects.front())).Enabled);
		// Drawing changes nothing.
		CHECK(fixture.Calls.empty());
		CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
		CHECK_FALSE(FileSystem::Exists(fixture.Locations.DefaultProjectLocation));
	}
}

TEST_CASE("StartScreenPanel: an empty screen says what there is to do, and a hidden one draws nothing")
{
	ScreenFixture fixture(
		[](std::filesystem::path const& temporaryRoot)
		{
			EditorLocations locations;
			locations.UserDataDirectory = temporaryRoot / "UserData" / "Editor";
			return locations;
		});
	fixture.Frames();
	// No content: the default template alone, no examples, no guides, no location.
	CHECK(fixture.Find("Template:empty-3d") != nullptr);
	CHECK(fixture.Find("Example:tiny") == nullptr);
	CHECK(fixture.Find("Guide:Getting started") == nullptr);
	CHECK_FALSE(fixture.Require("Create").Enabled);
	CHECK(fixture.Require("Open Project...").Visible);
	CHECK(fixture.Require("Continue without a project").Visible);

	fixture.Model.SetVisible(false);
	fixture.Frames();
	CHECK(fixture.Panel.GetItems().empty());
	CHECK(ImGui::FindWindowByName(StartScreenPanel::WindowName)->Active == false);
}

TEST_CASE("StartScreenPanel: with the files the agent setup writes listed, Create stays on the screen and in the dialog at 1280x720")
{
	ScreenFixture fixture(MakeFullContent);
	fixture.Gui.SetDisplaySize(ImVec2(1280.0f, 720.0f));
	fixture.Frames();
	fixture.Click("Files it writes");
	fixture.Frames();
	// Expanded, the form can be taller than its column: the fields scroll above Create, which stays whole.
	CHECK(fixture.Require("Create").Visible);
	CHECK(fixture.Require("Create").Max.y <= 720.0f);
	CHECK(fixture.Require("Continue without a project").Visible);

	REQUIRE(fixture.Operations.CreateProject(fixture.Directory.GetPath() / "Open", "Open", Scene("Main")).IsOk());
	fixture.Model.SetVisible(false);
	NewProjectDialog dialog;
	fixture.Draw = [&]
	{
		ScreenFixture::DrawMenuBar();
		dialog.OnImGuiRender(fixture.Model, nullptr, fixture.Actions.Form);
	};
	dialog.Open();
	fixture.Frames();
	REQUIRE(dialog.IsOpen());
	auto const findItem = [&dialog](std::string_view itemName)
	{
		std::vector<StartScreenItem> const& dialogItems = dialog.GetItems();
		auto const found = std::ranges::find(dialogItems, itemName, &StartScreenItem::Name);
		REQUIRE_MESSAGE(found != dialogItems.end(), itemName);
		return *found;
	};
	fixture.Gui.Click(findItem("Files it writes").GetCenter(), fixture.Draw);
	fixture.Frames();
	for (char const* const name : {"Create", "Cancel"})
	{
		StartScreenItem const item = findItem(name);
		CHECK_MESSAGE(item.Visible, name);
		CHECK_MESSAGE(item.Max.y <= 720.0f, name);
	}
	for (std::string_view const card : {"Template:empty-3d", "Template:tiny-project"})
	{
		CHECK_MESSAGE(findItem(card).TextWhole, card);
	}
}

TEST_CASE("StartScreenPanel: clicking a recent project opens it; the context menu shows its folder and removes it from the list")
{
	ScreenFixture fixture;
	std::filesystem::path const orb = fixture.WriteProject("Orb");
	std::filesystem::path const gone = fixture.Directory.GetPath() / "Gone" / "Gone.sproj";
	fixture.Model.GetRecentProjects().Add(gone);
	fixture.Model.GetRecentProjects().Add(orb);
	fixture.Frames();

	// A project that is gone does not open.
	fixture.Click(RecentItem(gone));
	CHECK(fixture.Calls.empty());
	CHECK(fixture.Context.GetProject() == nullptr);

	// Show in Folder on the one that opens, through the system's handler.
	fixture.Click(RecentItem(orb), ImGuiMouseButton_Right);
	fixture.Click("Recent menu:Show in Folder");
	REQUIRE(fixture.SystemOpened.size() == 1);
	CHECK(fixture.SystemOpened[0] == RecentProjects::NormalizePath(orb).parent_path());

	// Remove from List takes the one that is gone off the list, and the list is saved.
	fixture.Click(RecentItem(gone), ImGuiMouseButton_Right);
	CHECK_FALSE(fixture.Require("Recent menu:Open").Enabled);
	CHECK_FALSE(fixture.Require("Recent menu:Show in Folder").Enabled);
	fixture.Click("Recent menu:Remove from List");
	CHECK(fixture.Find(RecentItem(gone)) == nullptr);
	RecentProjects saved(fixture.Model.GetRecentProjects().GetFile());
	REQUIRE(saved.Load().IsOk());
	REQUIRE(saved.GetProjects().size() == 1);
	CHECK(saved.GetProjects()[0] == RecentProjects::NormalizePath(orb));

	// A click opens the project, which hides the screen.
	fixture.Click(RecentItem(orb));
	CHECK(fixture.Calls == std::vector<std::string>{"OpenProject"});
	REQUIRE(fixture.Context.GetProject() != nullptr);
	CHECK(RecentProjects::NormalizePath(fixture.Context.GetProject()->GetFilePath()) == RecentProjects::NormalizePath(orb));
	CHECK_FALSE(fixture.Model.IsVisible());
	CHECK(fixture.Panel.GetItems().empty());

	// Open Project... asks the editor's file dialog.
	fixture.Model.SetVisible(true);
	fixture.Frames();
	fixture.Click("Open Project...");
	CHECK(fixture.Calls.back() == "BrowseProject");
}

TEST_CASE("StartScreenPanel: Create makes the form's project from each template, with and without agents and the .gitignore")
{
	ScreenFixture fixture;
	fixture.Frames();
	int index = 0;
	for (char const* const templateId : {"empty-3d", "tiny-project"})
	{
		for (bool const agents : {true, false})
		{
			for (bool const gitIgnore : {true, false})
			{
				CAPTURE(templateId);
				CAPTURE(agents);
				CAPTURE(gitIgnore);
				fixture.Model.SetVisible(true);
				fixture.Frames();
				fixture.Click(std::string("Template:") + templateId);
				CHECK(fixture.Model.GetForm().TemplateId == templateId);
				// The checkboxes start on each time the screen shows the form as the user left it; click what differs.
				if (fixture.Model.GetForm().SetUpAgents != agents)
				{
					fixture.Click("Set up for coding agents");
				}
				if (fixture.Model.GetForm().GitIgnore != gitIgnore)
				{
					fixture.Click("Add a .gitignore");
				}
				CHECK(fixture.Model.GetForm().SetUpAgents == agents);
				CHECK(fixture.Model.GetForm().GitIgnore == gitIgnore);
				std::string const name = fmt::format("Game {}", ++index);
				fixture.Model.GetForm().Name = name;
				fixture.Frames();
				fixture.Click("Create");

				REQUIRE(fixture.Created.has_value());
				std::filesystem::path const directory = fixture.Locations.DefaultProjectLocation / name;
				CHECK(RecentProjects::NormalizePath(fixture.Created->ProjectFile) ==
				      RecentProjects::NormalizePath(directory / (name + ".sproj")));
				CHECK(FileSystem::IsRegularFile(directory / (name + ".sproj")));
				CHECK(FileSystem::IsRegularFile(directory / "AGENTS.md") == agents);
				CHECK(FileSystem::IsRegularFile(directory / ".mcp.json") == agents);
				CHECK(FileSystem::IsRegularFile(directory / ".gitignore") == gitIgnore);
				CHECK(fixture.Created->Agents.has_value() == agents);
				CHECK_FALSE(fixture.Model.IsVisible());
				CHECK(fixture.Model.GetRecentProjects().GetProjects().front() ==
				      RecentProjects::NormalizePath(fixture.Created->ProjectFile));
				fixture.Created.reset();
			}
		}
	}
	CHECK(std::ranges::count(fixture.Calls, "Create") == 8);
}

TEST_CASE("StartScreenPanel: Create is disabled with the reason while the form cannot create, and Browse... sets the location")
{
	ScreenFixture fixture;
	fixture.Frames();
	fixture.Model.GetForm().Name = "";
	fixture.Frames();
	CHECK_FALSE(fixture.Require("Create").Enabled);
	fixture.Click("Create");
	CHECK(fixture.Calls.empty());

	// A location the folder dialog chose; a cancelled dialog leaves it.
	fixture.Model.GetForm().Name = "Game";
	std::filesystem::path const chosen = fixture.Directory.GetPath() / "Chosen";
	fixture.PickedFolder = chosen;
	fixture.Click("Browse...");
	CHECK(fixture.Model.GetForm().Location == FileSystem::PathToUtf8(chosen));
	fixture.PickedFolder.reset();
	fixture.Click("Browse...");
	CHECK(fixture.Model.GetForm().Location == FileSystem::PathToUtf8(chosen));
	CHECK(std::ranges::count(fixture.Calls, "PickFolder") == 2);

	// A target that is taken.
	REQUIRE(FileSystem::WriteTextFile(chosen / "Game" / "notes.txt", "mine").IsOk());
	fixture.Frames(70);
	CHECK_FALSE(fixture.Require("Create").Enabled);
	fixture.Model.GetForm().Name = "Game 2";
	fixture.Frames();
	CHECK(fixture.Require("Create").Enabled);
}

TEST_CASE("StartScreenPanel: Open copies an example next to the form's location and opens the copy; an unavailable one cannot be opened")
{
	ScreenFixture fixture;
	fixture.Frames();

	// Fetched's files are missing: its Open does nothing.
	CHECK_FALSE(fixture.Require("Example:fetched").Enabled);
	fixture.Click("Example:fetched");
	CHECK(fixture.Calls.empty());

	fixture.Click("Example:tiny");
	REQUIRE(fixture.Calls == std::vector<std::string>{"OpenExample"});
	fixture.FinishCopy();
	CHECK(fixture.Calls.back() == "Copied");
	REQUIRE(fixture.Context.GetProject() != nullptr);
	std::filesystem::path const copy = fixture.Locations.DefaultProjectLocation / "Tiny";
	CHECK(RecentProjects::NormalizePath(fixture.Context.GetProject()->GetFilePath()) == RecentProjects::NormalizePath(copy / "Tiny.sproj"));
	CHECK_FALSE(FileSystem::Exists(copy / "Tools"));
	CHECK_FALSE(fixture.Model.IsVisible());

	// Once more: the copy goes to "Tiny 2". While it copies, the card shows its progress and no other Open works.
	fixture.Model.SetVisible(true);
	fixture.Frames();
	fixture.Click("Example:tiny");
	if (fixture.Copies.IsRunning())
	{
		fixture.Gui.Frame(fixture.Draw);
		if (fixture.Copies.IsRunning())
		{
			CHECK(fixture.Find("Cancel copy") != nullptr);
			CHECK_FALSE(fixture.Require("Example:fetched").Enabled);
		}
	}
	fixture.FinishCopy();
	REQUIRE(fixture.Context.GetProject() != nullptr);
	CHECK(RecentProjects::NormalizePath(fixture.Context.GetProject()->GetFilePath()) ==
	      RecentProjects::NormalizePath(fixture.Locations.DefaultProjectLocation / "Tiny 2" / "Tiny.sproj"));
}

TEST_CASE("StartScreenPanel: Continue without a project hides the screen; the guides open with the system's handler")
{
	ScreenFixture fixture;
	fixture.Frames();
	fixture.Click("Guide:Getting started");
	REQUIRE(fixture.SystemOpened.size() == 1);
	CHECK(fixture.SystemOpened[0] == Testing::GetTestContentDirectory() / "Docs" / "GettingStarted.md");

	fixture.Click("Continue without a project");
	CHECK(fixture.Calls == std::vector<std::string>{"Continue"});
	CHECK_FALSE(fixture.Model.IsVisible());
	CHECK(fixture.Panel.GetItems().empty());
	CHECK(fixture.Context.GetProject() == nullptr);
}

namespace
{
	// The editor's global shortcuts with actions that record what ran, a scene with an entity and an undo step, and the
	// chords to press.
	struct ShortcutFixture
	{
		std::vector<std::string> Ran;
		EditorShortcutActions Actions;

		ShortcutFixture()
		{
			auto const record = [this](char const* name)
			{
				return [this, name]
				{
					Ran.emplace_back(name);
				};
			};
			Actions.NewScene = record("NewScene");
			Actions.OpenScene = record("OpenScene");
			Actions.SaveScene = record("SaveScene");
			Actions.SaveSceneAs = record("SaveSceneAs");
			Actions.BuildScripts = record("BuildScripts");
			Actions.TogglePlay = record("TogglePlay");
			Actions.TogglePause = record("TogglePause");
			Actions.StepFrame = record("StepFrame");
		}

		// Every global chord: the actions', then Undo, Redo and the editing keys.
		static std::vector<ImGuiKeyChord> const& GetChords()
		{
			static std::vector<ImGuiKeyChord> const s_Chords = {
				ImGuiMod_Ctrl | ImGuiKey_N,
				ImGuiMod_Ctrl | ImGuiKey_O,
				ImGuiMod_Ctrl | ImGuiKey_S,
				ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S,
				ImGuiMod_Ctrl | ImGuiKey_B,
				ImGuiMod_Ctrl | ImGuiKey_P,
				ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P,
				ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_P,
				ImGuiMod_Ctrl | ImGuiKey_Z,
				ImGuiMod_Ctrl | ImGuiKey_Y,
				ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z,
				ImGuiMod_Ctrl | ImGuiKey_D,
				ImGuiKey_Delete,
				ImGuiMod_Ctrl | ImGuiKey_Backspace,
				ImGuiMod_Ctrl | ImGuiKey_A,
			};
			return s_Chords;
		}
	};

	// What a scene and its history are, to compare before and after.
	struct SceneState
	{
		Json Scene;
		size_t Undo = 0;
		size_t Redo = 0;
		std::vector<UUID> Selection;

		bool operator==(SceneState const& other) const = default;
	};

	SceneState Capture(EditorContext const& context)
	{
		return SceneState{SceneSerializer::Serialize(context.GetScene()), context.GetHistory().GetUndoCount(),
		                  context.GetHistory().GetRedoCount(), context.GetSelection().GetEntities()};
	}
}

TEST_CASE("StartScreenPanel: while it shows, a click on a panel behind it and every global shortcut change nothing")
{
	ScreenFixture fixture;
	ShortcutFixture shortcuts;
	EntityCreateInfo info;
	info.Name = "Crate";
	Result<UUID> const crate = fixture.Operations.CreateEntity(info);
	REQUIRE(crate.IsOk());
	SceneHierarchyPanel hierarchy;
	bool hierarchyOpen = true;
	// As the editor draws a frame: the shortcuts, then the panels unless the screen shows, then the screen.
	fixture.Draw = [&]
	{
		bool const startScreen = fixture.Model.IsVisible();
		ScreenFixture::DrawMenuBar();
		HandleEditorShortcuts(shortcuts.Actions, fixture.Operations, nullptr, {}, startScreen);
		if (!startScreen)
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 40.0f));
			ImGui::SetNextWindowSize(ImVec2(400.0f, 400.0f));
			hierarchy.OnImGuiRender(fixture.Operations, hierarchyOpen);
		}
		fixture.Panel.OnImGuiRender(fixture.Model, fixture.Copies, nullptr, fixture.Actions);
	};

	// Without the screen: where a click selects the crate, and that every shortcut runs.
	fixture.Model.SetVisible(false);
	fixture.Frames();
	std::optional<ImVec2> row;
	for (float y = 60.0f; y < 300.0f && !row; y += 4.0f)
	{
		fixture.Gui.Click(ImVec2(60.0f, y), fixture.Draw);
		if (!fixture.Context.GetSelection().IsEmpty())
		{
			row = ImVec2(60.0f, y);
		}
	}
	REQUIRE(row.has_value());
	REQUIRE(fixture.Operations.Select({}).IsOk());

	fixture.Model.SetVisible(true);
	fixture.Frames();
	SceneState const before = Capture(fixture.Context);
	fixture.Gui.Click(*row, fixture.Draw);
	fixture.Gui.MouseClick(*row, ImGuiMouseButton_Right, fixture.Draw);
	for (ImGuiKeyChord const chord : ShortcutFixture::GetChords())
	{
		CAPTURE(chord);
		fixture.Gui.Chord(chord, fixture.Draw);
	}
	CHECK(shortcuts.Ran.empty());
	CHECK(Capture(fixture.Context) == before);
	CHECK(fixture.Model.IsVisible());

	// Once it hides, the same keys act.
	fixture.Click("Continue without a project");
	for (size_t index = 0; index < 8; index++)
	{
		fixture.Gui.Chord(ShortcutFixture::GetChords()[index], fixture.Draw);
	}
	CHECK(shortcuts.Ran == std::vector<std::string>{"NewScene", "OpenScene", "SaveScene", "SaveSceneAs", "BuildScripts", "TogglePlay",
	                                                "TogglePause", "StepFrame"});
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_Z, fixture.Draw);
	CHECK(fixture.Context.GetHistory().GetUndoCount() + 1 == before.Undo);
}

TEST_CASE("StartScreenPanel: no global shortcut acts while a modal dialog is open: New Project and Set Up for Coding Agents")
{
	ScreenFixture fixture;
	ShortcutFixture shortcuts;
	REQUIRE(fixture.Operations.CreateProject(fixture.Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	EntityCreateInfo info;
	info.Name = "Crate";
	REQUIRE(fixture.Operations.CreateEntity(info).IsOk());
	fixture.Model.SetVisible(false);
	NewProjectDialog newProject;
	AgentSetupDialog agentSetup;
	std::optional<AgentProject> const project = AgentProject{
		fixture.Directory.GetPath() / "Game", "Game", FileSystem::PathToUtf8(fixture.Context.GetProject()->GetFilePath().filename())};
	fixture.Draw = [&]
	{
		HandleEditorShortcuts(shortcuts.Actions, fixture.Operations, nullptr, {}, fixture.Model.IsVisible());
		newProject.OnImGuiRender(fixture.Model, nullptr, fixture.Actions.Form);
		agentSetup.OnImGuiRender(project, fixture.Locations.Layout, {});
	};

	for (std::function<void()> const& open : std::vector<std::function<void()>>{[&newProject]
	                                                                            {
																					newProject.Open();
																				},
	                                                                            [&agentSetup]
	                                                                            {
																					agentSetup.Open();
																				}})
	{
		open();
		fixture.Frames();
		REQUIRE(ImGui::GetTopMostPopupModal() != nullptr);
		SceneState const before = Capture(fixture.Context);
		for (ImGuiKeyChord const chord : ShortcutFixture::GetChords())
		{
			CAPTURE(chord);
			fixture.Gui.Chord(chord, fixture.Draw);
		}
		CHECK(shortcuts.Ran.empty());
		CHECK(Capture(fixture.Context) == before);
		REQUIRE(ImGui::GetTopMostPopupModal() != nullptr);
		fixture.Gui.Press(ImGuiKey_Escape, fixture.Draw);
		fixture.Frames();
		REQUIRE(ImGui::GetTopMostPopupModal() == nullptr);
	}
	// Without a dialog the shortcuts act again.
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_N, fixture.Draw);
	CHECK(shortcuts.Ran == std::vector<std::string>{"NewScene"});
}

TEST_CASE("StartScreenPanel: File > New Project... shows the same form with a project open, and creates from it")
{
	ScreenFixture fixture;
	REQUIRE(fixture.Operations.CreateProject(fixture.Directory.GetPath() / "Open", "Open", Scene("Main")).IsOk());
	fixture.Model.SetVisible(false);
	fixture.Model.GetForm().Name = "Left over";
	NewProjectDialog dialog;
	fixture.Draw = [&]
	{
		ScreenFixture::DrawMenuBar();
		dialog.OnImGuiRender(fixture.Model, nullptr, fixture.Actions.Form);
	};
	dialog.Open();
	fixture.Frames();
	REQUIRE(dialog.IsOpen());
	// The form starts afresh, with the templates, the agents and the .gitignore.
	CHECK(fixture.Model.GetForm() == NewProjectForm{"New Game", FileSystem::PathToUtf8(fixture.Locations.DefaultProjectLocation)});
	std::vector<StartScreenItem> const& items = dialog.GetItems();
	for (char const* const name : {"Template:empty-3d", "Template:tiny-project", "Name", "Location", "Browse...",
	                               "Set up for coding agents", "Add a .gitignore", "Create", "Cancel"})
	{
		auto const item = std::ranges::find(items, std::string_view(name), &StartScreenItem::Name);
		REQUIRE_MESSAGE(item != items.end(), name);
		CHECK_MESSAGE(item->Visible, name);
	}
	auto const card = std::ranges::find(items, std::string_view("Template:tiny-project"), &StartScreenItem::Name);
	fixture.Gui.Click(card->GetCenter(), fixture.Draw);
	CHECK(fixture.Model.GetForm().TemplateId == "tiny-project");
	// The template needs the SDK, which the form now says: the rows below move down in the frames after.
	fixture.Frames();
	auto const create = std::ranges::find(items, std::string_view("Create"), &StartScreenItem::Name);
	REQUIRE(create->Enabled);
	fixture.Gui.Click(create->GetCenter(), fixture.Draw);
	fixture.Frames();
	REQUIRE(fixture.Created.has_value());
	std::filesystem::path const directory = fixture.Locations.DefaultProjectLocation / "New Game";
	CHECK(RecentProjects::NormalizePath(fixture.Created->ProjectFile) == RecentProjects::NormalizePath(directory / "New Game.sproj"));
	CHECK(FileSystem::IsRegularFile(directory / "AGENTS.md"));
	CHECK(FileSystem::IsRegularFile(directory / ".gitignore"));
	CHECK_FALSE(dialog.IsOpen());
	CHECK(ImGui::GetTopMostPopupModal() == nullptr);

	// Close() closes it as Cancel does.
	dialog.Open();
	fixture.Frames();
	REQUIRE(dialog.IsOpen());
	dialog.Close();
	fixture.Frames();
	CHECK_FALSE(dialog.IsOpen());
}

TEST_CASE("StartScreenPanel: the cards' thumbnails become images once a graphics device exists, and are released")
{
	{
		// Without a device: no image, and the cards draw all the same.
		StartScreenThumbnails thumbnails;
		HeadlessImGui gui;
		if (!GraphicsDevice::IsInitialized())
		{
			gui.Frame(
				[&thumbnails]
				{
					CHECK(thumbnails.Get(Testing::GetTestContentDirectory() / "Examples" / "Thumbnails" / "tiny.png") == nullptr);
				});
		}
	}

	ST_REQUIRE_GPU();
	ScreenFixture fixture;
	StartScreenThumbnails thumbnails;
	fixture.Draw = [&]
	{
		fixture.Panel.OnImGuiRender(fixture.Model, fixture.Copies, &thumbnails, fixture.Actions);
	};
	std::filesystem::path const tiny = Testing::GetTestContentDirectory() / "Examples" / "Thumbnails" / "tiny.png";
	std::filesystem::path const fetched = Testing::GetTestContentDirectory() / "Examples" / "Thumbnails" / "fetched.png";
	fixture.Gui.Frame(
		[&]
		{
			// One file a frame.
			CHECK(thumbnails.Get(tiny) != nullptr);
			CHECK(thumbnails.Get(fetched) == nullptr);
			CHECK(thumbnails.Get(tiny) != nullptr);
		});
	fixture.Gui.Frame(
		[&]
		{
			CHECK(thumbnails.Get(fetched) != nullptr);
			CHECK(thumbnails.Get(fixture.Directory.GetPath() / "missing.png") == nullptr);
		});
	// The screen's cards: two templates and two examples.
	fixture.Frames(10);
	CHECK(thumbnails.GetCount() == 4);
	thumbnails.Release();
	CHECK(thumbnails.GetCount() == 0);
}
