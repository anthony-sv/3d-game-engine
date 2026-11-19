#include "EditorLocationsTestUtilities.h"
#include "HeadlessImGui.h"
#include "TestUtilities.h"

#include "Editor/Agents/AgentKit.h"
#include "Editor/Panels/AgentSetupDialog.h"
#include "Editor/UI/EditorUI.h"

#include "Strada/Core/EngineLayout.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Result.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
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

	// The length of a deep project's directory. Windows allows a path 259 characters, and the longest one a setup writes,
	// ".claude/skills/strada-make-game/SKILL.md" under its temporary name (".tmp" and up to 20 digits), takes 65 more.
	constexpr size_t DeepProjectLength = 190;
	// The length of a deep engine's folder: its tool's path and the test content's deepest file (51 characters) fit below it.
	constexpr size_t DeepEngineLength = 180;

	// A directory below base whose path (FileSystem::PathToUtf8) has exactly the given length and ends in the named folder:
	// folders with long names are added until it does, whatever the length of base (the temporary folder's differs from one
	// machine to another). Creates nothing.
	std::filesystem::path MakeDeepDirectory(std::filesystem::path directory, std::string const& last, size_t length)
	{
		std::string const folder = "Folder of a studio that keeps its projects deep";
		auto const measure = [](std::filesystem::path const& path)
		{
			return FileSystem::PathToUtf8(path).size();
		};
		// Room for a separator, a folder of one character at least, a separator and the last folder.
		size_t const end = last.size() + 3;
		REQUIRE(measure(directory) + end <= length);
		while (measure(directory) + folder.size() + 1 + end <= length)
		{
			directory /= folder;
		}
		// The rest in a folder of letters alone: Windows drops spaces at the end of a name.
		std::string filler;
		while (filler.size() < length - measure(directory) - end + 1)
		{
			filler += "Deeper";
		}
		filler.resize(length - measure(directory) - end + 1);
		directory /= filler;
		directory /= last;
		REQUIRE(measure(directory) == length);
		return directory;
	}

	bool IsTooltipShown()
	{
		ImGuiWindow const* const tooltip = ImGui::FindWindowByName("##Tooltip_00");
		return tooltip != nullptr && tooltip->Active;
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
			// The editor's fonts, theme and keyboard navigation, and its fixed-width font in the diffs.
			Dialog.SetMonospaceFont(Gui.UseEditorStyle());
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

		// Moves the project, and what the editor has open, to a directory of DeepProjectLength characters.
		void UseDeepProject()
		{
			Project.Directory = MakeDeepDirectory(Directory.GetPath(), Project.Name, DeepProjectLength);
			OpenProject = Project;
			REQUIRE(FileSystem::CreateDirectories(Project.Directory).IsOk());
			REQUIRE(FileSystem::WriteTextFile(Project.Directory / Project.ProjectFile, "{}\n").IsOk());
		}

		// Checks that the dialog lies inside the display with nothing to scroll to (its buttons show).
		void CheckFits(ImVec2 display) const
		{
			ImGuiWindow const* const popup = GetPopup();
			CAPTURE(popup->Pos.y);
			CAPTURE(popup->Size.y);
			CAPTURE(popup->ContentSize.y);
			CAPTURE(popup->ScrollMax.y);
			CHECK(popup->ScrollMax.y == 0.0f);
			CHECK(popup->Pos.x >= 0.0f);
			CHECK(popup->Pos.y >= 0.0f);
			CHECK(popup->Pos.x + popup->Size.x <= display.x);
			CHECK(popup->Pos.y + popup->Size.y <= display.y);
		}

		// The text the dialog draws in a frame, as ImGui's log of rendered text has it: a line per line drawn, the items of
		// one line separated by a space.
		std::string DrawnText()
		{
			std::string text;
			Gui.Frame(
				[this, &text]
				{
					ImGui::LogToBuffer();
					Draw();
					text = GImGui->LogBuffer.c_str();
					ImGui::LogFinish();
				});
			return text;
		}

		// Rests the mouse, 30 frames at a time (tooltips wait for a mouse at rest), at points down the dialog's left edge from
		// its title bar to its file list, until a tooltip shows: the text drawn in a frame that shows it, or none.
		std::optional<std::string> RestForTooltip()
		{
			ImGuiWindow const* const popup = GetPopup();
			float const x = popup->Pos.x + popup->WindowPadding.x + 4.0f;
			float const bottom = GetFileList()->Pos.y;
			for (float y = popup->Pos.y + popup->TitleBarHeight + 2.0f; y < bottom; y += GImGui->FontSize * 0.5f)
			{
				ImGui::GetIO().AddMousePosEvent(x, y);
				for (int frame = 0; frame < 30; frame++)
				{
					Gui.Frame(Draw);
				}
				if (IsTooltipShown())
				{
					std::string const text = DrawnText();
					REQUIRE(IsTooltipShown());
					return text;
				}
			}
			return std::nullopt;
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
	// The dialog fits a small window before and after Apply, with nothing to scroll to (its buttons show).
	ImVec2 const display(1280.0f, 720.0f);
	fixture.Gui.SetDisplaySize(display);
	auto const fits = [&fixture, display]
	{
		ImGuiWindow const* const popup = fixture.GetPopup();
		return popup->ScrollMax.y == 0.0f && popup->Pos.x >= 0.0f && popup->Pos.y >= 0.0f && popup->Pos.x + popup->Size.x <= display.x &&
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

TEST_CASE("AgentSetupDialog: a project in a deep folder keeps its path to one line, whole in its tooltip, inside a small window")
{
	DialogFixture fixture;
	fixture.UseDeepProject();
	std::string const directory = FileSystem::PathToUtf8(fixture.Project.Directory);
	std::string const folder = "/" + fixture.Project.Name;
	ImVec2 const display(1280.0f, 720.0f);
	fixture.Gui.SetDisplaySize(display);
	fixture.Open();
	REQUIRE(fixture.IsOpen());
	fixture.CheckFits(display);

	// The project's line names it and ends with the end of its path, the project's folder; the rest of the path gives way.
	std::string ellipsis;
	fixture.Gui.Frame(
		[&fixture, &ellipsis]
		{
			ellipsis = UI::GetEllipsis();
			fixture.Draw();
		});
	auto const showsDirectory = [&](std::string_view text)
	{
		for (size_t start = 0; start < text.size();)
		{
			size_t const end = std::min(text.find('\n', start), text.size());
			std::string_view line = text.substr(start, end - start);
			start = end + 1;
			if (line.ends_with('\r'))
			{
				line.remove_suffix(1);
			}
			size_t const cut = line.find(ellipsis);
			if (line.starts_with(fixture.Project.Name + " ") && cut != std::string_view::npos)
			{
				std::string_view const rest = line.substr(cut + ellipsis.size());
				if (rest.size() > folder.size() && rest.ends_with(folder) && std::string_view(directory).ends_with(rest))
				{
					return true;
				}
			}
		}
		return false;
	};
	std::string const drawn = fixture.DrawnText();
	CAPTURE(drawn);
	CHECK(showsDirectory(drawn));
	CHECK(drawn.find(directory) == std::string::npos);

	// Resting the mouse on that line shows the whole path, wrapped inside the display.
	std::optional<std::string> const tooltip = fixture.RestForTooltip();
	REQUIRE_MESSAGE(tooltip.has_value(), "no tooltip shows the project's path");
	CHECK(tooltip->find(directory) != std::string::npos);
	ImGuiWindow const* const window = ImGui::FindWindowByName("##Tooltip_00");
	REQUIRE(window != nullptr);
	CHECK(window->Pos.x >= 0.0f);
	CHECK(window->Pos.y >= 0.0f);
	CHECK(window->Pos.x + window->Size.x <= display.x);
	CHECK(window->Pos.y + window->Size.y <= display.y);

	// Apply writes every file below the deep folder, and the dialog with its result still fits.
	fixture.Click("Apply");
	fixture.CheckFits(display);
	REQUIRE(fixture.Dialog.GetLastResult().has_value());
	CHECK(fixture.Dialog.GetLastResult()->Succeeded());
	CHECK(showsDirectory(fixture.DrawnText()));
	for (auto const& file : fixture.States())
	{
		CAPTURE(file.first);
		CHECK(file.second == AgentFileState::Current);
	}
	fixture.Click("Close");
	CHECK_FALSE(fixture.IsOpen());
}

TEST_CASE("AgentSetupDialog: many warnings, one naming a long path, and a failed Apply leave the dialog inside a small window")
{
	DialogFixture fixture;
	fixture.UseDeepProject();
	// An engine in a deep folder with strada beside its editor: as none is on PATH, a warning names that tool's path.
	std::filesystem::path const engine = MakeDeepDirectory(fixture.Directory.GetPath(), "Engine", DeepEngineLength);
	REQUIRE(FileSystem::CopyDirectory(Testing::GetTestContentDirectory(), engine).IsOk());
	Result<EngineLayout> layout = EngineLayout::Resolve(engine / "StradaEditor");
	REQUIRE(layout.IsOk());
	fixture.Layout = layout.TakeValue();
	REQUIRE(FileSystem::WriteTextFile(fixture.Layout.GetToolPath(), "a tool").IsOk());
	// The engine lacks its guides, a warning each.
	for (std::string_view const guide : AgentKit::Guides)
	{
		std::error_code error;
		CHECK(std::filesystem::remove(engine / "Docs" / FileSystem::PathFromUtf8(guide), error));
		CHECK_FALSE(error);
	}
	// The project's skill was changed by the user and its .mcp.json is not a JSON object: the setup keeps both, a warning each.
	std::filesystem::path const skill = fixture.Project.Directory / ".claude" / "skills" / "strada-make-game" / "SKILL.md";
	REQUIRE(FileSystem::WriteTextFile(skill, "Mine.\n").IsOk());
	REQUIRE(FileSystem::WriteTextFile(fixture.Project.Directory / ".mcp.json", "[]\n").IsOk());
	// And a file that cannot be written stops Apply.
	REQUIRE(FileSystem::WriteTextFile(fixture.Project.Directory / "CLAUDE.md", "Mine.\n").IsOk());
	Testing::ReadOnlyFileScope const readOnly(fixture.Project.Directory / "CLAUDE.md");
	Result<AgentPlan> plan = AgentKit::Plan(fixture.Project, fixture.Layout, fixture.FindTool);
	REQUIRE(plan.IsOk());
	std::vector<std::string> const& warnings = plan.GetValue().Warnings;
	CHECK(warnings.size() == AgentKit::Guides.size() + 3);
	CHECK(std::ranges::any_of(warnings,
	                          [&engine](std::string const& warning)
	                          {
								  return warning.find(FileSystem::PathToUtf8(engine)) != std::string::npos;
							  }));

	ImVec2 const display(1280.0f, 720.0f);
	fixture.Gui.SetDisplaySize(display);
	fixture.Open();
	REQUIRE(fixture.IsOpen());
	fixture.CheckFits(display);
	fixture.Click("Apply");
	fixture.CheckFits(display);
	REQUIRE(fixture.Dialog.GetLastResult().has_value());
	CHECK_FALSE(fixture.Dialog.GetLastResult()->Succeeded());
	CHECK(fixture.Dialog.GetLastResult()->FailedPath == "CLAUDE.md");
	fixture.Click("Close");
	CHECK_FALSE(fixture.IsOpen());
}

TEST_CASE("AgentSetupDialog: a diff's long lines wrap in a pane that leaves the dialog inside a small window")
{
	DialogFixture fixture;
	// An engine whose AGENTS.md block has a line far wider than the dialog.
	std::filesystem::path const content = fixture.Directory.GetPath() / "WideEngine";
	REQUIRE(FileSystem::CopyDirectory(Testing::GetTestContentDirectory(), content).IsOk());
	std::string block = ReadFile(content / "Agents" / "AGENTS.block.md");
	for (int word = 0; word < 60; word++)
	{
		block += "wide ";
	}
	block += "end.\n";
	REQUIRE(FileSystem::WriteTextFile(content / "Agents" / "AGENTS.block.md", block).IsOk());
	Result<EngineLayout> layout = EngineLayout::Resolve(content / "StradaEditor");
	REQUIRE(layout.IsOk());
	fixture.Layout = layout.TakeValue();

	ImVec2 const display(1280.0f, 720.0f);
	fixture.Gui.SetDisplaySize(display);
	fixture.Open();
	REQUIRE(fixture.IsOpen());
	REQUIRE(fixture.Dialog.GetSelectedFile() == "AGENTS.md");
	ImGuiWindow const* const popup = fixture.GetPopup();
	auto const diff = std::ranges::find_if(GImGui->Windows,
	                                       [popup](ImGuiWindow const* window)
	                                       {
											   return window->ParentWindow == popup && window->Active &&
		                                              std::string_view(window->Name).find("##diff") != std::string_view::npos;
										   });
	REQUIRE_MESSAGE(diff != GImGui->Windows.end(), "the dialog shows no diff");
	// The file list shows whole rows (it scrolls to the others).
	ImGuiWindow const* const list = fixture.GetFileList();
	float const rowPitch = GImGui->FontSize + GImGui->Style.ItemSpacing.y;
	float const shownRows = (list->Size.y - list->WindowPadding.y * 2.0f) / rowPitch;
	CHECK(shownRows == doctest::Approx(std::round(shownRows)));
	// Nothing to scroll to sideways: the long line wrapped at the pane's edge.
	CHECK((*diff)->ScrollMax.x == 0.0f);
	CHECK((*diff)->ContentSize.x <= (*diff)->InnerRect.GetWidth());
	CHECK(popup->ScrollMax.y == 0.0f);
	CHECK(popup->Pos.y >= 0.0f);
	CHECK(popup->Pos.y + popup->Size.y <= display.y);
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
	// The engine's own block, in the repository's Agents/ folder.
	std::filesystem::path const repository = FileSystem::PathFromUtf8(STRADA_AUTOMATION_DOC_PATH).parent_path().parent_path();
	CHECK(ReadFile(repository / "Agents" / FileSystem::PathFromUtf8(AgentKit::BlockFileName)).find(item) != std::string::npos);
	// The README of the guides a setup writes.
	DialogFixture fixture;
	AgentApplyResult const applied = AgentKit::Apply(fixture.Project, fixture.Layout, fixture.FindTool);
	REQUIRE(applied.Succeeded());
	CHECK(ReadFile(fixture.Project.Directory / ".strada" / "docs" / "README.md").find(item) != std::string::npos);
}
