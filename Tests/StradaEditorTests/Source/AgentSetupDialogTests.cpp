#include "EditorLocationsTestUtilities.h"
#include "HeadlessImGui.h"
#include "TestUtilities.h"

#include "Editor/Agents/AgentKit.h"
#include "Editor/Panels/AgentSetupDialog.h"

#include "Strada/Core/EngineLayout.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace Strada;
using Strada::Testing::HeadlessImGui;

namespace
{
	std::string ReadFile(std::filesystem::path const& path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE_MESSAGE(text.IsOk(), FileSystem::PathToUtf8(path));
		return text.TakeValue();
	}

	// Every file below a directory with its bytes, by path relative to it.
	std::map<std::string, std::string> Snapshot(std::filesystem::path const& directory)
	{
		std::map<std::string, std::string> files;
		std::error_code error;
		for (std::filesystem::directory_entry const& entry : std::filesystem::recursive_directory_iterator(directory, error))
		{
			if (entry.is_regular_file(error))
			{
				files.emplace(FileSystem::PathToUtf8(entry.path().lexically_relative(directory)), ReadFile(entry.path()));
			}
		}
		return files;
	}

	// The dialog over "Orb Run" in a temporary directory, with the test content's agent kit and no strada on PATH, drawn
	// in a headless ImGui context as the editor draws it.
	struct DialogFixture
	{
		HeadlessImGui Gui;
		Testing::TemporaryDirectory Directory;
		EditorLocations Locations = Testing::MakeEditorLocations(Directory.GetPath());
		EngineLayout Layout = Locations.Layout;
		AgentProject Project{Directory.GetPath() / "Orb Run", "Orb Run", "Orb Run.sproj"};
		// What the editor has open.
		std::optional<AgentProject> OpenProject = Project;
		AgentToolLookup FindTool;
		AgentSetupDialog Dialog;
		std::function<void()> Draw = [this]
		{
			Dialog.OnImGuiRender(OpenProject, Layout, FindTool);
		};

		DialogFixture()
		{
			REQUIRE(FileSystem::CreateDirectories(Project.Directory).IsOk());
			REQUIRE(FileSystem::WriteTextFile(Project.Directory / "Orb Run.sproj", "{}\n").IsOk());
		}

		void Frames(int count = 3)
		{
			for (int frame = 0; frame < count; frame++)
			{
				Gui.Frame(Draw);
			}
		}

		void Open()
		{
			Dialog.Open();
			Frames();
		}

		bool IsOpen() const { return GImGui->OpenPopupStack.Size == 1 && ImGui::GetTopMostPopupModal() != nullptr; }

		ImGuiWindow* GetPopup() const
		{
			ImGuiWindow* const popup = ImGui::FindWindowByName(AgentSetupDialog::PopupName);
			REQUIRE(popup != nullptr);
			REQUIRE(popup->Active);
			return popup;
		}

		// The file list, a child window of the dialog.
		ImGuiWindow* GetFileList() const
		{
			ImGuiWindow const* const popup = GetPopup();
			auto const list = std::ranges::find_if(GImGui->Windows,
			                                       [popup](ImGuiWindow const* window)
			                                       {
													   return window->ParentWindow == popup && window->Active &&
				                                              std::string_view(window->Name).find("##files") != std::string_view::npos;
												   });
			REQUIRE_MESSAGE(list != GImGui->Windows.end(), "the dialog shows no file list");
			return *list;
		}

		// The IDs of the items the mouse hovers along a vertical line through the window, a frame every 3 pixels, and where
		// it first hovered each.
		std::map<ImGuiID, ImVec2> Hover(ImGuiWindow const* window, float x)
		{
			std::map<ImGuiID, ImVec2> hovered;
			for (float y = window->Pos.y + 1.0f; y < window->Pos.y + window->Size.y; y += 3.0f)
			{
				ImGui::GetIO().AddMousePosEvent(x, y);
				Gui.Frame(Draw);
				if (GImGui->HoveredId != 0)
				{
					hovered.emplace(GImGui->HoveredId, ImVec2(x, y));
				}
			}
			return hovered;
		}

		// Where a button of the dialog is: the buttons sit in one row at the bottom, Apply (or Close) first.
		std::optional<ImVec2> FindButton(char const* label)
		{
			ImGuiWindow const* const popup = GetPopup();
			ImGuiID const id = ImHashStr(label, 0, popup->ID);
			std::map<ImGuiID, ImVec2> const first = Hover(popup, popup->Pos.x + popup->WindowPadding.x + 4.0f);
			if (auto const found = first.find(id); found != first.end())
			{
				return found->second;
			}
			// Further right on the row of the first button.
			auto const row = first.find(ImHashStr("Apply", 0, popup->ID));
			if (row == first.end())
			{
				return std::nullopt;
			}
			for (float x = row->second.x; x < popup->Pos.x + popup->Size.x; x += 3.0f)
			{
				ImGui::GetIO().AddMousePosEvent(x, row->second.y);
				Gui.Frame(Draw);
				if (GImGui->HoveredId == id)
				{
					return ImVec2(x, row->second.y);
				}
			}
			return std::nullopt;
		}

		void Click(char const* label)
		{
			std::optional<ImVec2> const position = FindButton(label);
			REQUIRE_MESSAGE(position, label);
			Gui.Click(*position, Draw);
			Frames();
		}

		// The rows of the file list, by path, with where each is.
		std::map<std::string, ImVec2> FindRows(std::vector<std::string> const& paths)
		{
			ImGuiWindow const* const list = GetFileList();
			std::map<ImGuiID, ImVec2> const hovered = Hover(list, list->Pos.x + list->WindowPadding.x + 4.0f);
			std::map<std::string, ImVec2> rows;
			for (std::string const& path : paths)
			{
				if (auto const found = hovered.find(ImHashStr(path.c_str(), 0, list->ID)); found != hovered.end())
				{
					rows.emplace(path, found->second);
				}
			}
			return rows;
		}

		std::map<std::string, AgentFileState> States() const
		{
			Result<AgentStatus> status = AgentKit::GetStatus(Project, Layout, FindTool);
			REQUIRE(status.IsOk());
			std::map<std::string, AgentFileState> states;
			for (AgentFileStatus const& file : status.GetValue().Files)
			{
				states.emplace(file.Path, file.State);
			}
			return states;
		}
	};
}

TEST_CASE("AgentSetupDialog: opening it plans the setup without writing anything, and Cancel closes it writing nothing")
{
	DialogFixture fixture;
	REQUIRE(FileSystem::WriteTextFile(fixture.Project.Directory / "AGENTS.md", "# My notes\n").IsOk());
	std::map<std::string, std::string> const before = Snapshot(fixture.Project.Directory);
	fixture.Frames();
	CHECK_FALSE(fixture.IsOpen());
	fixture.Open();
	REQUIRE(fixture.IsOpen());
	CHECK(Snapshot(fixture.Project.Directory) == before);

	// Every file a setup writes has its row, the one that changes first is selected, and selecting another shows it.
	std::vector<std::string> const paths = AgentKit::ListFiles(fixture.Layout);
	REQUIRE(paths.size() > 5);
	CHECK(paths.front() == "AGENTS.md");
	CHECK(fixture.Dialog.GetSelectedFile() == "AGENTS.md");
	std::map<std::string, ImVec2> const rows = fixture.FindRows(paths);
	for (std::string const& path : paths)
	{
		CAPTURE(path);
		CHECK(rows.contains(path));
	}
	REQUIRE(rows.contains(".mcp.json"));
	fixture.Gui.Click(rows.at(".mcp.json"), fixture.Draw);
	CHECK(fixture.Dialog.GetSelectedFile() == ".mcp.json");
	CHECK(fixture.IsOpen());

	fixture.Click("Cancel");
	CHECK_FALSE(fixture.IsOpen());
	CHECK_FALSE(fixture.Dialog.GetLastResult().has_value());
	CHECK(Snapshot(fixture.Project.Directory) == before);

	// Escape cancels too.
	fixture.Open();
	REQUIRE(fixture.IsOpen());
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Draw);
	CHECK_FALSE(fixture.IsOpen());
	CHECK(Snapshot(fixture.Project.Directory) == before);
}

TEST_CASE("AgentSetupDialog: Apply sets the project up and shows what it did; opened again, everything is up to date")
{
	DialogFixture fixture;
	REQUIRE(FileSystem::WriteTextFile(fixture.Project.Directory / "AGENTS.md", "# My notes\n").IsOk());
	// The dialog fits a small window before and after Apply.
	ImVec2 const display(1280.0f, 720.0f);
	fixture.Gui.SetDisplaySize(display);
	auto const fits = [&fixture, display]
	{
		ImGuiWindow const* const popup = fixture.GetPopup();
		return popup->Pos.x >= 0.0f && popup->Pos.y >= 0.0f && popup->Pos.x + popup->Size.x <= display.x &&
		       popup->Pos.y + popup->Size.y <= display.y;
	};
	fixture.Open();
	CHECK(fits());
	fixture.Click("Apply");
	CHECK(fits());
	REQUIRE(fixture.Dialog.GetLastResult().has_value());
	AgentApplyResult const applied = *fixture.Dialog.GetLastResult();
	CHECK(applied.Succeeded());
	CHECK(applied.Plan.Files.size() == AgentKit::ListFiles(fixture.Layout).size());
	for (auto const& file : fixture.States())
	{
		CAPTURE(file.first);
		CHECK(file.second == AgentFileState::Current);
	}
	CHECK(ReadFile(fixture.Project.Directory / "AGENTS.md").starts_with("# My notes\n\n<!-- BEGIN strada-agents -->\n"));
	// The dialog stays open on its result until it is closed; Apply is gone.
	CHECK(fixture.IsOpen());
	CHECK_FALSE(fixture.FindButton("Apply").has_value());
	fixture.Click("Close");
	CHECK_FALSE(fixture.IsOpen());

	// Nothing is left to do: Apply is disabled and clicking it writes nothing.
	std::map<std::string, std::string> const before = Snapshot(fixture.Project.Directory);
	fixture.Open();
	REQUIRE(fixture.IsOpen());
	CHECK_FALSE(fixture.Dialog.GetLastResult().has_value());
	CHECK(fixture.Dialog.GetSelectedFile() == "AGENTS.md");
	fixture.Click("Apply");
	CHECK_FALSE(fixture.Dialog.GetLastResult().has_value());
	CHECK(fixture.IsOpen());
	CHECK(Snapshot(fixture.Project.Directory) == before);
	fixture.Click("Cancel");
	CHECK_FALSE(fixture.IsOpen());
}

TEST_CASE("AgentSetupDialog: a file that cannot be written stops Apply, and the dialog shows which")
{
	DialogFixture fixture;
	REQUIRE(FileSystem::WriteTextFile(fixture.Project.Directory / "CLAUDE.md", "Mine.\n").IsOk());
	Testing::ReadOnlyFileScope const readOnly(fixture.Project.Directory / "CLAUDE.md");
	fixture.Open();
	fixture.Click("Apply");
	REQUIRE(fixture.Dialog.GetLastResult().has_value());
	AgentApplyResult const applied = *fixture.Dialog.GetLastResult();
	CHECK_FALSE(applied.Succeeded());
	CHECK(applied.FailedPath == "CLAUDE.md");
	// The list shows the files written before it.
	REQUIRE(applied.Plan.Files.size() == 1);
	CHECK(applied.Plan.Files[0].Path == "AGENTS.md");
	CHECK(ReadFile(fixture.Project.Directory / "CLAUDE.md") == "Mine.\n");
	CHECK(fixture.IsOpen());
	fixture.Click("Close");
	CHECK_FALSE(fixture.IsOpen());
}

TEST_CASE("AgentSetupDialog: without a project or without the engine's agent kit it writes nothing and only closes")
{
	SUBCASE("No project is open")
	{
		DialogFixture fixture;
		fixture.OpenProject.reset();
		std::map<std::string, std::string> const before = Snapshot(fixture.Directory.GetPath());
		fixture.Open();
		REQUIRE(fixture.IsOpen());
		CHECK_FALSE(fixture.FindButton("Apply").has_value());
		fixture.Click("Close");
		CHECK_FALSE(fixture.IsOpen());
		CHECK(Snapshot(fixture.Directory.GetPath()) == before);
	}
	SUBCASE("The engine has no agent kit")
	{
		DialogFixture fixture;
		fixture.Layout = EngineLayout();
		std::map<std::string, std::string> const before = Snapshot(fixture.Project.Directory);
		fixture.Open();
		REQUIRE(fixture.IsOpen());
		CHECK_FALSE(fixture.FindButton("Apply").has_value());
		CHECK(fixture.Dialog.GetSelectedFile().empty());
		fixture.Click("Close");
		CHECK_FALSE(fixture.IsOpen());
		CHECK_FALSE(fixture.Dialog.GetLastResult().has_value());
		CHECK(Snapshot(fixture.Project.Directory) == before);
	}
}

TEST_CASE("AgentSetupDialog: a project opened while the dialog shows gets a plan of its own, and Apply writes only into it")
{
	DialogFixture fixture;
	fixture.Open();
	REQUIRE(fixture.IsOpen());
	AgentProject const other{fixture.Directory.GetPath() / "Spark Hunt", "Spark Hunt", "Spark Hunt.sproj"};
	REQUIRE(FileSystem::CreateDirectories(other.Directory).IsOk());
	REQUIRE(FileSystem::WriteTextFile(other.Directory / "AGENTS.md", "Spark notes.\n").IsOk());
	std::map<std::string, std::string> const first = Snapshot(fixture.Project.Directory);
	fixture.OpenProject = other;
	fixture.Frames();
	fixture.Click("Apply");
	REQUIRE(fixture.Dialog.GetLastResult().has_value());
	CHECK(fixture.Dialog.GetLastResult()->Succeeded());
	CHECK(Snapshot(fixture.Project.Directory) == first);
	std::string const agents = ReadFile(other.Directory / "AGENTS.md");
	CHECK(agents.starts_with("Spark notes.\n\n<!-- BEGIN strada-agents -->\n"));
	CHECK(agents.find("\"Spark Hunt\"") != std::string::npos);
	CHECK(agents.find("`Spark Hunt.sproj`") != std::string::npos);

	// Closing the project under the dialog leaves it with Close alone.
	fixture.OpenProject.reset();
	fixture.Frames();
	CHECK_FALSE(fixture.FindButton("Apply").has_value());
	fixture.Click("Close");
	CHECK_FALSE(fixture.IsOpen());
}

TEST_CASE("AgentSetupDialog: the texts the agent kit writes name the File menu's item")
{
	std::string const item = std::string("File > ") + AgentSetupDialog::MenuItem;
	// The engine's own block.
	Result<EngineLayout> const layout = EngineLayout::Resolve(FileSystem::GetExecutablePath());
	REQUIRE(layout.IsOk());
	REQUIRE(layout.GetValue().IsSourceTree());
	CHECK(ReadFile(layout.GetValue().GetContentPath(EngineContent::AgentKit) / AgentKit::BlockFileName).find(item) != std::string::npos);
	// The README of the guides a setup writes.
	DialogFixture fixture;
	AgentApplyResult const applied = AgentKit::Apply(fixture.Project, fixture.Layout, fixture.FindTool);
	REQUIRE(applied.Succeeded());
	CHECK(ReadFile(fixture.Project.Directory / ".strada" / "docs" / "README.md").find(item) != std::string::npos);
}
