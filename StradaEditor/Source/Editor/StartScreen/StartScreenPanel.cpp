#include "Editor/StartScreen/StartScreenPanel.h"

#include "Editor/ExampleCopyTask.h"
#include "Editor/StartScreen/StartScreenCards.h"
#include "Editor/StartScreen/StartScreenThumbnails.h"
#include "Editor/UI/EditorUI.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Version.h"

#include <spdlog/fmt/fmt.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace Strada
{
	namespace
	{
		// The columns' shares of the screen's width: recent projects, then the examples and Learn; the New Project form
		// takes the rest.
		constexpr float RecentShare = 0.26f;
		constexpr float ExamplesShare = 0.30f;
		// Lines of an example card's description: as many as the longest takes, within these (a longer one is cut, and the
		// card's tooltip shows it).
		constexpr float MinimumExampleLines = 2.0f;
		constexpr float MaximumExampleLines = 4.0f;

		void DrawSectionTitle(char const* title)
		{
			ImGui::PushFont(nullptr, std::round(ImGui::GetStyle().FontSizeBase * 1.3f));
			ImGui::TextUnformatted(title);
			ImGui::PopFont();
		}

		// The height a section title takes: its line, at the size the window's scale gives it, and the spacing after it.
		float GetSectionTitleHeight()
		{
			return std::round(ImGui::GetFontSize() * 1.3f) + ImGui::GetStyle().ItemSpacing.y;
		}

		// One line of text at the cursor, cut with "..." where it does not fit the width; the item is the line.
		void TextEllipsis(std::string const& text, float width)
		{
			ImVec2 const position = ImGui::GetCursorScreenPos();
			float const lineHeight = ImGui::GetTextLineHeight();
			ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), position, ImVec2(position.x + width, position.y + lineHeight),
			                          position.x + width, text.c_str(), text.c_str() + text.size(), nullptr);
			ImGui::Dummy(ImVec2(width, lineHeight));
		}

		std::string DescribeState(RecentProjectState state)
		{
			switch (state)
			{
				case RecentProjectState::Ok:
					return {};
				case RecentProjectState::Missing:
					return "Missing";
				case RecentProjectState::Unreadable:
					return "Unreadable";
			}
			return {};
		}

		char const* ExplainState(RecentProjectState state)
		{
			switch (state)
			{
				case RecentProjectState::Ok:
					return "Click to open it.";
				case RecentProjectState::Missing:
					return "The project file is gone. Remove it from the list with the context menu.";
				case RecentProjectState::Unreadable:
					return "The file does not read as a Strada project. Remove it from the list with the context menu.";
			}
			return "";
		}
	}

	void StartScreenPanel::Refresh(StartScreenModel& model)
	{
		model.Refresh();
		m_Form.Invalidate();
		m_Examples.clear();
		// The error line stays: a failure reported as the screen came up (the project the command line named, which did not
		// open) is what it shows first. Hiding the screen cleared any older one.
		if (std::optional<ExampleCatalog> const& catalog = model.GetExamples())
		{
			for (ExampleEntry const& example : catalog->GetExamples())
			{
				m_Examples[example.Id] = ExampleView{catalog->IsAvailable(example), catalog->GetThumbnailPath(example)};
			}
		}
	}

	void StartScreenPanel::Report(StartScreenModel& model, Result<void> const& result, std::string_view doing)
	{
		if (result)
		{
			return;
		}
		std::string message = DescribeStartScreenFailure(doing, result.GetError());
		ST_ERROR("{}", message);
		model.ReportError(std::move(message));
	}

	void StartScreenPanel::OnImGuiRender(StartScreenModel& model, ExampleCopyTask& copies, StartScreenThumbnails* thumbnails,
	                                     StartScreenActions const& actions)
	{
		m_Items.clear();
		if (!model.IsVisible())
		{
			return;
		}
		// Shown again (it was hidden or not drawn in the last frame): what it shows is read again.
		int const frame = ImGui::GetFrameCount();
		bool const appearing = m_LastFrame < 0 || frame > m_LastFrame + 1;
		m_LastFrame = frame;
		if (appearing)
		{
			Refresh(model);
		}

		ImGuiViewport const* const viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGuiWindowFlags const flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
		                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse;
		float const margin = std::round(ImGui::GetFontSize() * 1.25f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(margin, margin * 0.75f));
		bool const open = ImGui::Begin(WindowName, nullptr, flags);
		ImGui::PopStyleVar(3);
		if (appearing)
		{
			// The screen takes the keyboard, unless a modal dialog is open over it (focusing would close it: imgui.cpp
			// FocusWindow closes the popups above the window it focuses).
			ImGui::FocusWindow(ImGui::GetCurrentWindow(), ImGuiFocusRequestFlags_UnlessBelowModal);
		}
		if (!open)
		{
			ImGui::End();
			return;
		}

		ImGuiStyle const& style = ImGui::GetStyle();
		DrawHeader();
		if (std::string const& error = model.GetError(); !error.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GetStartScreenWarningColor());
			TextEllipsis(error, std::max(ImGui::GetContentRegionAvail().x, 1.0f));
			ImGui::PopStyleColor();
			RecordStartScreenItem(&m_Items, "Error", false);
			ImGui::SetItemTooltip("%s", error.c_str());
		}
		ImGui::Spacing();

		float const footerHeight = ImGui::GetFrameHeight() + style.ItemSpacing.y * 2.0f + 1.0f;
		float const bodyHeight = std::max(ImGui::GetContentRegionAvail().y - footerHeight, ImGui::GetFrameHeight() * 4.0f);
		float const gap = std::round(margin * 1.2f);
		float const width = ImGui::GetContentRegionAvail().x;
		float const recentWidth = std::floor(width * RecentShare);
		float const examplesWidth = std::floor(width * ExamplesShare);
		float const formWidth = std::max(width - recentWidth - examplesWidth - gap * 2.0f, 1.0f);

		// The columns do not scroll themselves: their lists do.
		ImGuiWindowFlags const columnFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		if (ImGui::BeginChild("##RecentColumn", ImVec2(recentWidth, bodyHeight), ImGuiChildFlags_None, columnFlags))
		{
			DrawRecentProjects(model, actions);
		}
		ImGui::EndChild();
		ImGui::SameLine(0.0f, gap);
		if (ImGui::BeginChild("##NewProjectColumn", ImVec2(formWidth, bodyHeight), ImGuiChildFlags_None, columnFlags))
		{
			DrawNewProject(model, thumbnails, actions);
		}
		ImGui::EndChild();
		ImGui::SameLine(0.0f, gap);
		if (ImGui::BeginChild("##ExamplesColumn", ImVec2(examplesWidth, bodyHeight), ImGuiChildFlags_None, columnFlags))
		{
			DrawExamples(model, copies, thumbnails, actions);
			DrawLearn(model);
		}
		ImGui::EndChild();
		DrawFooter(model, actions);
		ImGui::End();
	}

	void StartScreenPanel::DrawHeader()
	{
		ImGui::PushFont(nullptr, std::round(ImGui::GetStyle().FontSizeBase * 2.2f));
		// As drawn, with the window's scale.
		float const titleSize = ImGui::GetFontSize();
		ImGui::TextUnformatted("Strada");
		ImGui::PopFont();
		ImGui::SameLine();
		// About on the title's baseline (the fonts' ascent is about four fifths of their size).
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::round((titleSize - ImGui::GetFontSize()) * 0.8f));
		ImGui::TextDisabled("%s", EngineVersion::String);
	}

	void StartScreenPanel::DrawRecentProjects(StartScreenModel& model, StartScreenActions const& actions)
	{
		DrawSectionTitle("Recent projects");
		ImGuiStyle const& style = ImGui::GetStyle();
		float const listHeight = std::max(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight() - style.ItemSpacing.y, 1.0f);
		// What the rows asked for, done once the list is drawn: opening a project that fails takes it off the list the loop
		// reads (EditorLayer), and removing one does.
		std::optional<std::filesystem::path> opened;
		std::optional<std::filesystem::path> removed;
		if (ImGui::BeginChild("##RecentList", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders))
		{
			std::vector<RecentProjectInfo> const& recent = model.GetRecent();
			if (recent.empty())
			{
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextDisabled("No recent projects yet. Create one, open an example, or open a project from disk.");
				ImGui::PopTextWrapPos();
			}
			float const lineHeight = ImGui::GetTextLineHeight();
			// The name above its folder, with a little room around them.
			float const rowPadding = std::round(style.FramePadding.y * 0.5f);
			float const rowHeight = lineHeight * 2.0f + rowPadding * 2.0f;
			for (size_t index = 0; index < recent.size(); index++)
			{
				RecentProjectInfo const& project = recent[index];
				std::string const path = FileSystem::PathToUtf8(project.File);
				bool const canOpen = project.State == RecentProjectState::Ok;
				ImGui::PushID(static_cast<int>(index));
				ImVec2 const rowMin = ImGui::GetCursorScreenPos();
				float const rowWidth = ImGui::GetContentRegionAvail().x;
				bool const clicked = ImGui::Selectable("##row", false, ImGuiSelectableFlags_None, ImVec2(0.0f, rowHeight));
				bool const hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort);
				// The row as drawn (a selectable's item reaches half the item spacing past it).
				RecordStartScreenItem(&m_Items, "Recent:" + path, canOpen, rowMin, ImVec2(rowMin.x + rowWidth, rowMin.y + rowHeight));
				if (clicked && canOpen && actions.OpenProject)
				{
					opened = project.File;
				}
				// Right after the row, whose item it opens over.
				if (ImGui::BeginPopupContextItem("##RecentMenu"))
				{
					if (ImGui::MenuItem("Open", nullptr, false, canOpen) && actions.OpenProject)
					{
						opened = project.File;
					}
					RecordStartScreenItem(&m_Items, "Recent menu:Open", canOpen);
					bool const canShow = project.State != RecentProjectState::Missing;
					if (ImGui::MenuItem("Show in Folder", nullptr, false, canShow))
					{
						model.ClearError();
						Report(model, model.ShowInFolder(project.File), "Showing the project's folder");
					}
					RecordStartScreenItem(&m_Items, "Recent menu:Show in Folder", canShow);
					if (ImGui::MenuItem("Remove from List"))
					{
						removed = project.File;
					}
					RecordStartScreenItem(&m_Items, "Recent menu:Remove from List", true);
					ImGui::EndPopup();
				}
				else if (hovered && ImGui::BeginTooltip())
				{
					ImGui::TextUnformatted(path.c_str());
					ImGui::TextDisabled("%s", ExplainState(project.State));
					ImGui::EndTooltip();
				}

				// The name above its folder; a state that keeps it from opening on the right.
				ImDrawList* const drawList = ImGui::GetWindowDrawList();
				std::string const state = DescribeState(project.State);
				float const stateWidth = state.empty() ? 0.0f : ImGui::CalcTextSize(state.c_str()).x + style.ItemSpacing.x;
				float const textWidth = std::max(rowWidth - stateWidth - style.FramePadding.x, 1.0f);
				ImVec2 const namePosition(rowMin.x + style.FramePadding.x * 0.5f, rowMin.y + rowPadding);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(canOpen ? ImGuiCol_Text : ImGuiCol_TextDisabled));
				ImGui::RenderTextEllipsis(drawList, namePosition, ImVec2(namePosition.x + textWidth, namePosition.y + lineHeight),
				                          namePosition.x + textWidth, project.Name.c_str(), project.Name.c_str() + project.Name.size(),
				                          nullptr);
				ImGui::PopStyleColor();
				// The folder keeps its last folders, which tell the projects apart, and gives way at its start.
				std::string const folder = UI::FitPath(FileSystem::PathToUtf8(project.File.parent_path()), textWidth);
				ImVec2 const folderPosition(namePosition.x, namePosition.y + lineHeight);
				drawList->AddText(folderPosition, ImGui::GetColorU32(ImGuiCol_TextDisabled), folder.c_str(),
				                  folder.c_str() + folder.size());
				if (!state.empty())
				{
					drawList->AddText(ImVec2(rowMin.x + rowWidth - stateWidth + style.ItemSpacing.x * 0.5f, namePosition.y),
					                  ImGui::GetColorU32(GetStartScreenWarningColor()), state.c_str());
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
		if (removed)
		{
			model.ClearError();
			Report(model, model.RemoveRecentProject(*removed), "Removing the project from the list");
		}
		if (opened)
		{
			model.ClearError();
			actions.OpenProject(*opened);
		}
		bool const canBrowse = static_cast<bool>(actions.BrowseProject);
		ImGui::BeginDisabled(!canBrowse);
		if (ImGui::Button("Open Project...", ImVec2(-FLT_MIN, 0.0f)) && canBrowse)
		{
			model.ClearError();
			actions.BrowseProject();
		}
		ImGui::EndDisabled();
		RecordStartScreenItem(&m_Items, "Open Project...", canBrowse);
		ImGui::SetItemTooltip("Open a project file (.sproj) from disk");
	}

	void StartScreenPanel::DrawNewProject(StartScreenModel& model, StartScreenThumbnails* thumbnails, StartScreenActions const& actions)
	{
		DrawSectionTitle("New project");
		// Create follows the fields; the fields scroll when they need more height than the column leaves above Create.
		float const fieldsHeight = std::max(ImGui::GetContentRegionAvail().y - NewProjectFormWidget::GetCreateRowHeight(), 1.0f);
		ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, fieldsHeight));
		if (ImGui::BeginChild("##NewProjectFields", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY))
		{
			m_Form.DrawFields(model, thumbnails, actions.Form, &m_Items);
		}
		ImGui::EndChild();
		m_Form.DrawCreateButton(model, actions.Form, &m_Items);
	}

	void StartScreenPanel::DrawExamples(StartScreenModel& model, ExampleCopyTask& copies, StartScreenThumbnails* thumbnails,
	                                    StartScreenActions const& actions)
	{
		DrawSectionTitle("Examples");
		ImGuiStyle const& style = ImGui::GetStyle();
		size_t const guides = std::max<size_t>(model.GetGuides().size(), 1);
		float const learnHeight =
			GetSectionTitleHeight() + static_cast<float>(guides) * ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y * 3.0f;
		float const listHeight = std::max(ImGui::GetContentRegionAvail().y - learnHeight, ImGui::GetFrameHeight() * 2.0f);
		// Learn follows the examples; the list scrolls when it needs more height than the column leaves above Learn.
		ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, listHeight));
		if (ImGui::BeginChild("##ExampleList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY))
		{
			std::optional<ExampleCatalog> const& catalog = model.GetExamples();
			ImGui::PushTextWrapPos(0.0f);
			if (!catalog)
			{
				ImGui::TextDisabled("No examples: %s", model.GetExamplesError().c_str());
			}
			else if (catalog->GetExamples().empty())
			{
				ImGui::TextDisabled("This engine comes with no examples.");
			}
			ImGui::PopTextWrapPos();
			bool const copying = copies.IsRunning();
			std::vector<ExampleEntry> const* const examples = catalog ? &catalog->GetExamples() : nullptr;
			// Asked for by an Open, done once the list is drawn (as the recent projects' are).
			std::optional<std::pair<ExampleEntry, std::filesystem::path>> opened;
			std::vector<StartScreenCard> cards;
			for (size_t index = 0; examples != nullptr && index < examples->size(); index++)
			{
				ExampleEntry const& example = (*examples)[index];
				StartScreenCard card;
				card.Name = example.Name;
				card.Description = example.Description;
				card.RequiresSdk = example.RequiresSdk;
				card.Thumbnail = thumbnails != nullptr ? thumbnails->Get(m_Examples[example.Id].Thumbnail) : nullptr;
				cards.push_back(card);
			}
			// The cards are as tall as the longest text needs, so none is cut and the list stays even.
			StartScreenCardLayout const layout = StartScreenCardLayout::Horizontal(ImGui::GetContentRegionAvail().x, MinimumExampleLines)
			                                         .FitText(cards, MinimumExampleLines, MaximumExampleLines);
			for (size_t index = 0; examples != nullptr && index < examples->size(); index++)
			{
				ExampleEntry const& example = (*examples)[index];
				ExampleView const& view = m_Examples[example.Id];
				ImGui::PushID(example.Id.c_str());
				StartScreenCardResult const drawn = DrawStartScreenCard("##card", cards[index], layout, false);
				RecordStartScreenItem(&m_Items, "Example card:" + example.Id, true, drawn.Min, drawn.Max);
				m_Items.back().TextWhole = drawn.TextWhole;
				ImVec2 const below = ImGui::GetCursorScreenPos();

				ImGui::SetCursorScreenPos(drawn.ActionPosition);
				float const buttonWidth = std::min(ImGui::GetFontSize() * 6.0f, drawn.ActionWidth);
				if (copying && copies.GetExampleId() == example.Id)
				{
					ProjectCopyProgress const& progress = copies.GetProgress();
					uint64_t const total = progress.TotalBytes.load();
					float const fraction =
						total > 0 ? static_cast<float>(static_cast<double>(progress.Bytes.load()) / static_cast<double>(total)) : 0.0f;
					std::string const label = fmt::format("Copying {}%", static_cast<int>(fraction * 100.0f));
					float const cancelWidth = ImGui::CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f;
					ImGui::ProgressBar(fraction, ImVec2(std::max(drawn.ActionWidth - cancelWidth - style.ItemSpacing.x, 1.0f), 0.0f),
					                   label.c_str());
					ImGui::SameLine();
					if (ImGui::Button("Cancel"))
					{
						copies.Cancel();
					}
					RecordStartScreenItem(&m_Items, "Cancel copy", true);
				}
				else
				{
					bool const enabled = view.Available && !copying && static_cast<bool>(actions.OpenExample);
					ImGui::BeginDisabled(!enabled);
					if (ImGui::Button("Open", ImVec2(buttonWidth, 0.0f)) && enabled)
					{
						model.ClearError();
						if (std::optional<std::filesystem::path> target = model.GetExampleCopyTarget(example))
						{
							opened.emplace(example, std::move(*target));
						}
						else
						{
							std::string message =
								fmt::format("{} cannot be copied: choose a location (a full path) in the New Project form, which the "
							                "example is copied into",
							                example.Name);
							ST_WARN("{}", message);
							model.ReportError(std::move(message));
						}
					}
					ImGui::EndDisabled();
					RecordStartScreenItem(&m_Items, "Example:" + example.Id, enabled);
					if (!view.Available)
					{
						ImGui::SetItemTooltip("%s", example.NeedsHint.empty() ? "Its files are missing" : example.NeedsHint.c_str());
					}
					else if (copying)
					{
						ImGui::SetItemTooltip("Another example is being copied");
					}
					else
					{
						ImGui::SetItemTooltip("Copy the example into the New Project form's location and open the copy");
					}
				}
				ImGui::SetCursorScreenPos(below);
				if (!view.Available)
				{
					// What restores its files.
					ImGui::PushStyleColor(ImGuiCol_Text, GetStartScreenWarningColor());
					ImGui::PushTextWrapPos(0.0f);
					ImGui::TextUnformatted(example.NeedsHint.empty() ? "Not available: its files are missing." : example.NeedsHint.c_str());
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
				}
				if (index + 1 < examples->size())
				{
					ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y));
				}
				else
				{
					// The list ends where the last card (or its hint) ends: the spacing after it would make the list scroll for
					// nothing. An item must follow SetCursorScreenPos (imgui.cpp
					// ErrorCheckUsingSetCursorPosToExtendParentBoundaries): a zero-sized one at the end adds no height.
					float const end = view.Available ? drawn.Max.y : ImGui::GetCursorScreenPos().y - style.ItemSpacing.y;
					ImGui::SetCursorScreenPos(ImVec2(below.x, end));
					ImGui::Dummy(ImVec2(0.0f, 0.0f));
				}
				ImGui::PopID();
			}
			if (opened)
			{
				actions.OpenExample(opened->first, opened->second);
			}
		}
		ImGui::EndChild();
	}

	void StartScreenPanel::DrawLearn(StartScreenModel& model)
	{
		ImGui::Spacing();
		DrawSectionTitle("Learn");
		std::vector<StartScreenGuide> const& guides = model.GetGuides();
		if (guides.empty())
		{
			ImGui::TextDisabled("The guides are not installed with this engine.");
		}
		for (StartScreenGuide const& guide : guides)
		{
			if (ImGui::TextLink(guide.Title.c_str()))
			{
				model.ClearError();
				Report(model, model.OpenWithSystem(guide.File), "Opening the guide");
			}
			RecordStartScreenItem(&m_Items, "Guide:" + guide.Title, true);
			ImGui::SetItemTooltip("%s", FileSystem::PathToUtf8(guide.File).c_str());
		}
	}

	void StartScreenPanel::DrawFooter(StartScreenModel const& model, StartScreenActions const& actions)
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		ImGui::Separator();
		StartScreenEnvironment const& environment = model.GetEnvironment();
		struct Part
		{
			std::string Text;
			bool Missing = false;
		};
		std::vector<Part> const parts = {
			environment.DotNetRuntime ? Part{*environment.DotNetRuntime, false} : Part{".NET runtime not found: scripts are off", true},
			environment.DotNetSdk ? Part{".NET SDK found", false} : Part{".NET SDK not found: needed to build C# scripts", true},
			environment.Gpu ? Part{*environment.Gpu, false} : Part{"No GPU", true},
			environment.Vulkan ? Part{"Vulkan " + *environment.Vulkan, false} : Part{"Vulkan unknown", true},
		};
		char const* const continueLabel = "Continue without a project";
		float const continueWidth = ImGui::CalcTextSize(continueLabel).x + style.FramePadding.x * 4.0f;
		float const stripWidth = std::max(ImGui::GetContentRegionAvail().x - continueWidth - style.ItemSpacing.x * 2.0f, 1.0f);

		// The strip, cut at its width; the SDK's folder in its tooltip.
		ImVec2 const start = ImGui::GetCursorScreenPos();
		float const textY = start.y + style.FramePadding.y;
		ImGui::PushClipRect(start, ImVec2(start.x + stripWidth, start.y + ImGui::GetFrameHeight()), true);
		float x = start.x;
		for (size_t index = 0; index < parts.size(); index++)
		{
			if (index > 0)
			{
				ImGui::GetWindowDrawList()->AddText(ImVec2(x, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), "  |  ");
				x += ImGui::CalcTextSize("  |  ").x;
			}
			ImU32 const color =
				parts[index].Missing ? ImGui::GetColorU32(GetStartScreenWarningColor()) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
			ImGui::GetWindowDrawList()->AddText(ImVec2(x, textY), color, parts[index].Text.c_str());
			x += ImGui::CalcTextSize(parts[index].Text.c_str()).x;
		}
		ImGui::PopClipRect();
		ImGui::Dummy(ImVec2(stripWidth, ImGui::GetFrameHeight()));
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::BeginTooltip())
		{
			for (Part const& part : parts)
			{
				ImGui::TextUnformatted(part.Text.c_str());
			}
			if (environment.DotNetSdk)
			{
				ImGui::TextDisabled("%s", FileSystem::PathToUtf8(*environment.DotNetSdk).c_str());
			}
			ImGui::EndTooltip();
		}

		ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
		bool const canContinue = static_cast<bool>(actions.Continue);
		ImGui::BeginDisabled(!canContinue);
		if (ImGui::Button(continueLabel, ImVec2(continueWidth, 0.0f)) && canContinue)
		{
			actions.Continue();
		}
		ImGui::EndDisabled();
		RecordStartScreenItem(&m_Items, continueLabel, canContinue);
		ImGui::SetItemTooltip("Work on the scene behind the start screen; File > Start Screen shows it again");
	}
}
