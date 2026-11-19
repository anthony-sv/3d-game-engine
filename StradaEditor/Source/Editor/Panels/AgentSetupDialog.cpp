#include "Editor/Panels/AgentSetupDialog.h"

#include "Editor/UI/EditorUI.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr ImVec4 SuccessColor = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
		constexpr ImVec4 FailureColor = ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
		constexpr ImVec4 WarningColor = ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
		// The rows the file list shows before it scrolls (every file of the kit) and the lines of a diff. The list takes at
		// most MaxFilesShare of the window's height and the messages below the diff (Apply's result, the kit's warnings) at
		// most MaxMessagesShare, scrolling beyond; the diff takes what the rest of the dialog leaves, so that the whole dialog
		// fits a small window whatever the length of the paths and names it shows.
		constexpr float VisibleFiles = 14.0f;
		constexpr float VisibleDiffLines = 16.0f;
		constexpr float MaxFilesShare = 0.3f;
		constexpr float MaxMessagesShare = 0.2f;
		// The share of its line the project's name takes at most; its directory takes the rest.
		constexpr float MaxNameShare = 0.4f;

		// A text of the messages below the diff, in its colour.
		struct DialogMessage
		{
			std::string Text;
			ImVec4 Color;
		};

		// The dialog's width at the current font.
		float GetDialogWidth()
		{
			return ImGui::GetFontSize() * 46.0f;
		}

		// The height of a bordered child of rows of text: as many rows as wanted, or as fit whole in maxHeight (at least one).
		float FitRows(float rows, float maxHeight)
		{
			float const rowHeight = ImGui::GetTextLineHeightWithSpacing();
			float const padding = ImGui::GetStyle().WindowPadding.y * 2.0f;
			float const fitting = std::max(std::floor((maxHeight - padding) / rowHeight), 1.0f);
			return std::min(rows, fitting) * rowHeight + padding;
		}

		bool IsSameProject(AgentProject const& first, AgentProject const& second)
		{
			return first.Directory == second.Directory && first.Name == second.Name && first.ProjectFile == second.ProjectFile;
		}

		bool Writes(AgentFileAction action)
		{
			return action == AgentFileAction::Create || action == AgentFileAction::Update;
		}

		// What the setup does to a file, before Apply and after it.
		char const* DescribeAction(AgentFileAction action, bool applied)
		{
			switch (action)
			{
				case AgentFileAction::Create:
					return applied ? "Created" : "Create";
				case AgentFileAction::Update:
					return applied ? "Updated" : "Update";
				case AgentFileAction::Unchanged:
					return "Up to date";
				case AgentFileAction::SkipUserModified:
					return applied ? "Kept: changed by you" : "Keep: changed by you";
				case AgentFileAction::SkipDamaged:
					return applied ? "Kept: cannot be read" : "Keep: cannot be read";
			}
			return "";
		}

		// Why a file the setup does not write stays as it is.
		char const* ExplainUnwritten(AgentFileAction action)
		{
			switch (action)
			{
				case AgentFileAction::Unchanged:
					return "This file already holds what the setup writes.";
				case AgentFileAction::SkipUserModified:
					return "This file was changed since Strada wrote it, so the setup keeps it. Delete it and set the project up "
						   "again to get the engine's version.";
				case AgentFileAction::SkipDamaged:
					return "Strada cannot read this file as what it should be (AGENTS.md with unbalanced strada-agents markers, a "
						   ".mcp.json that is not a JSON object, a folder or a link), so the setup never rewrites it. Fix or "
						   "remove it, then set the project up again.";
				case AgentFileAction::Create:
				case AgentFileAction::Update:
					break;
			}
			return "";
		}

		AgentFilePlan const* FindFile(AgentPlan const& plan, std::string const& path)
		{
			auto const file = std::ranges::find(plan.Files, path, &AgentFilePlan::Path);
			return file != plan.Files.end() ? &*file : nullptr;
		}

		ImVec4 GetDiffLineColor(std::string_view line)
		{
			if (line.starts_with("+"))
			{
				return SuccessColor;
			}
			if (line.starts_with("-"))
			{
				return FailureColor;
			}
			return ImGui::GetStyleColorVec4(line.starts_with("@@") ? ImGuiCol_TextDisabled : ImGuiCol_Text);
		}

		// The project's name and directory on one line that never wraps, so that the dialog's height depends on neither: the
		// name shortened at its end to MaxNameShare of the line, the directory losing its leading folders first (UI::FitPath)
		// in the rest, and both whole in a tooltip, wrapped at the dialog's width, when either is shortened.
		void DrawProject(AgentProject const& project)
		{
			std::string const directory = FileSystem::PathToUtf8(project.Directory);
			std::string const name = UI::FitText(project.Name, ImGui::GetContentRegionAvail().x * MaxNameShare);
			ImGui::BeginGroup();
			ImGui::TextUnformatted(name.c_str());
			ImGui::SameLine();
			std::string const shown = UI::FitPath(directory, ImGui::GetContentRegionAvail().x);
			ImGui::TextDisabled("%s", shown.c_str());
			ImGui::EndGroup();
			if ((name != project.Name || shown != directory) && ImGui::BeginItemTooltip())
			{
				ImGui::PushTextWrapPos(GetDialogWidth());
				ImGui::TextUnformatted(project.Name.c_str());
				ImGui::TextDisabled("%s", directory.c_str());
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
		}

		// What Apply did, as the dialog says it.
		DialogMessage DescribeResult(AgentApplyResult const& result)
		{
			if (result.Succeeded())
			{
				size_t const written = static_cast<size_t>(std::ranges::count_if(result.Plan.Files,
				                                                                 [](AgentFilePlan const& file)
				                                                                 {
																					 return Writes(file.Action);
																				 }));
				return {fmt::format("The project is set up for coding agents: {} files written. An agent started in the project's "
				                    "folder reads AGENTS.md and reaches this editor through .mcp.json.",
				                    written),
				        SuccessColor};
			}
			if (result.FailedPath.empty())
			{
				return {fmt::format("The setup wrote nothing: {}", result.Error), FailureColor};
			}
			// The error names the file.
			return {fmt::format("The setup stopped: {}. The files listed above stay written.", result.Error), FailureColor};
		}

		// The height of the messages, wrapped at the width, laid out as DrawMessages lays them out.
		float MeasureMessages(std::vector<DialogMessage> const& messages, float width)
		{
			float height = 0.0f;
			for (DialogMessage const& message : messages)
			{
				height += ImGui::CalcTextSize(message.Text.c_str(), nullptr, false, width).y;
			}
			return messages.empty() ? 0.0f : height + ImGui::GetStyle().ItemSpacing.y * static_cast<float>(messages.size() - 1);
		}

		// The messages, wrapped, in a pane of the given height that scrolls when they are taller: their texts name paths of any
		// length (the tool's, the project's in an error) and their number grows with the files a setup keeps.
		void DrawMessages(std::vector<DialogMessage> const& messages, float height)
		{
			if (messages.empty())
			{
				return;
			}
			if (ImGui::BeginChild("##messages", ImVec2(0.0f, height)))
			{
				for (DialogMessage const& message : messages)
				{
					ImGui::PushStyleColor(ImGuiCol_Text, message.Color);
					ImGui::TextWrapped("%s", message.Text.c_str());
					ImGui::PopStyleColor();
				}
			}
			ImGui::EndChild();
		}
	}

	void AgentSetupDialog::Open()
	{
		m_OpenRequested = true;
		m_CloseRequested = false;
	}

	void AgentSetupDialog::Close()
	{
		m_CloseRequested = true;
		m_OpenRequested = false;
	}

	void AgentSetupDialog::MakePlan(AgentProject const& project, EngineLayout const& layout, AgentToolLookup const& findTool)
	{
		m_Project = project;
		m_Plan.reset();
		m_PlanError.clear();
		m_Result.reset();
		m_Selected.clear();
		Result<AgentPlan> plan = AgentKit::Plan(project, layout, findTool);
		if (!plan)
		{
			m_PlanError = plan.GetError();
			return;
		}
		m_Plan = plan.TakeValue();
		// The first file that changes shows its diff at once.
		auto const changed = std::ranges::find_if(m_Plan->Files,
		                                          [](AgentFilePlan const& file)
		                                          {
													  return Writes(file.Action);
												  });
		if (changed != m_Plan->Files.end())
		{
			m_Selected = changed->Path;
		}
		else if (!m_Plan->Files.empty())
		{
			m_Selected = m_Plan->Files.front().Path;
		}
	}

	void AgentSetupDialog::OnImGuiRender(std::optional<AgentProject> const& project, EngineLayout const& layout,
	                                     AgentToolLookup const& findTool)
	{
		if (std::exchange(m_OpenRequested, false))
		{
			m_Project.reset();
			if (project)
			{
				MakePlan(*project, layout, findTool);
			}
			ImGui::OpenPopup(PopupName);
		}
		ImGuiViewport const* viewport = ImGui::GetMainViewport();
		// Centered when it appears, and again when Apply's result makes it taller: the result is drawn in the frame after
		// Apply, and the window takes its height in the frame after that.
		m_RecenterFrames = m_RecenterFrames > 0 ? m_RecenterFrames - 1 : 0;
		ImGui::SetNextWindowPos(viewport->GetCenter(), m_RecenterFrames > 0 ? ImGuiCond_Always : ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		// In the editor window, never in a window of its own (imgui.cpp WindowSelectViewport gives an appearing popup that does
		// not fit its host viewport one), so that captures of the editor window show it.
		ImGui::SetNextWindowViewport(viewport->ID);
		// A fixed width, and the height of what it shows: the result appears below the list after Apply.
		float const width = ImGui::GetFontSize() * 46.0f;
		ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, viewport->WorkSize.y));
		// Opaque: the theme's popups are slightly translucent, and the panels behind would show through its text.
		ImVec4 opaque = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
		opaque.w = 1.0f;
		ImGui::PushStyleColor(ImGuiCol_PopupBg, opaque);
		m_Open = ImGui::BeginPopupModal(PopupName, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
		ImGui::PopStyleColor();
		if (!m_Open)
		{
			m_CloseRequested = false;
			return;
		}
		// Closed as Cancel closes it (editor.dialog): Escape's path below.
		bool const escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || std::exchange(m_CloseRequested, false);
		if (!project)
		{
			ImGui::TextDisabled("No project is open.");
			if (ImGui::Button("Close") || escape)
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
			return;
		}
		// Automation may have closed the project or opened another while the dialog was open.
		if (!m_Project || !IsSameProject(*m_Project, *project))
		{
			MakePlan(*project, layout, findTool);
		}

		// The project is named on a line of its own, which never wraps: this text's height depends on no name.
		ImGui::TextWrapped("Sets the project up for coding agents such as Claude Code: AGENTS.md tells them how to work, .mcp.json "
		                   "connects them to this editor through the strada MCP server, and the game-making skill and the "
		                   "guides teach them the tools. Text you wrote outside Strada's parts of these files is kept.");
		DrawProject(*project);
		if (!m_Plan)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, FailureColor);
			ImGui::TextWrapped("The engine's agent kit is incomplete: %s", m_PlanError.c_str());
			ImGui::PopStyleColor();
			if (ImGui::Button("Close") || escape)
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
			return;
		}

		bool const applied = m_Result.has_value();
		AgentPlan const& shown = applied ? m_Result->Plan : *m_Plan;
		float const workHeight = viewport->WorkSize.y;
		float const filesHeight = FitRows(std::min(static_cast<float>(shown.Files.size()), VisibleFiles), workHeight * MaxFilesShare);
		// Below the diff: Apply's result, then the kit's warnings, which are messages (also in project.setup-agents' result)
		// that the dialog shows as sentences.
		std::vector<DialogMessage> messages;
		if (applied)
		{
			messages.push_back(DescribeResult(*m_Result));
		}
		for (std::string const& warning : shown.Warnings)
		{
			messages.push_back({UI::FormatSentence(warning), WarningColor});
		}
		float const messagesHeight =
			std::min(MeasureMessages(messages, ImGui::GetContentRegionAvail().x), std::floor(workHeight * MaxMessagesShare));
		// What follows the diff, down to the window's bottom edge: the result's heading, the messages, the buttons' row.
		ImGuiStyle const& style = ImGui::GetStyle();
		float footerHeight = style.ItemSpacing.y * 2.0f + ImGui::GetFrameHeight() + style.WindowPadding.y;
		if (applied)
		{
			footerHeight +=
				std::max(ImGui::GetFontSize() + style.SeparatorTextPadding.y * 2.0f, style.SeparatorTextBorderSize) + style.ItemSpacing.y;
		}
		if (!messages.empty())
		{
			footerHeight += messagesHeight + style.ItemSpacing.y;
		}
		// ImGui keeps an auto-resized window DisplaySafeAreaPadding inside the work area.
		float const maxHeight = workHeight - style.DisplaySafeAreaPadding.y * 2.0f;
		DrawFiles(shown, applied, filesHeight);
		DrawSelection(shown, applied, maxHeight - footerHeight);
		if (applied)
		{
			ImGui::SeparatorText("Result");
		}
		DrawMessages(messages, messagesHeight);
		ImGui::Spacing();

		if (applied)
		{
			if (ImGui::Button("Close", ImVec2(ImGui::GetFontSize() * 6.0f, 0.0f)) || escape)
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
			return;
		}
		bool const changes = std::ranges::any_of(m_Plan->Files,
		                                         [](AgentFilePlan const& file)
		                                         {
													 return Writes(file.Action);
												 });
		ImGui::BeginDisabled(!changes);
		if (ImGui::Button("Apply", ImVec2(ImGui::GetFontSize() * 6.0f, 0.0f)))
		{
			m_Result = AgentKit::Apply(*project, layout, findTool);
			m_RecenterFrames = 3;
			if (m_Result->Succeeded())
			{
				ST_INFO("Set '{}' up for coding agents", project->Name);
			}
			else
			{
				ST_ERROR("Setting '{}' up for coding agents stopped: {}", project->Name, m_Result->Error);
			}
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip(changes ? "Write the files marked Create or Update into the project (not an undo step)"
		                              : "Every file is up to date");
		ImGui::SameLine();
		if (ImGui::Button("Cancel") || escape)
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void AgentSetupDialog::DrawFiles(AgentPlan const& plan, bool applied, float height)
	{
		ImGui::SeparatorText(applied ? "What the setup did" : "What the setup does");
		if (ImGui::BeginChild("##files", ImVec2(0.0f, height), ImGuiChildFlags_Borders))
		{
			float const actionColumn = ImGui::GetContentRegionAvail().x * 0.62f;
			for (AgentFilePlan const& file : plan.Files)
			{
				if (ImGui::Selectable(file.Path.c_str(), file.Path == m_Selected))
				{
					m_Selected = file.Path;
				}
				ImGui::SameLine(actionColumn);
				bool const kept = file.Action == AgentFileAction::SkipUserModified || file.Action == AgentFileAction::SkipDamaged;
				ImVec4 const color =
					kept ? WarningColor : ImGui::GetStyleColorVec4(Writes(file.Action) ? ImGuiCol_Text : ImGuiCol_TextDisabled);
				ImGui::TextColored(color, "%s", DescribeAction(file.Action, applied));
			}
		}
		ImGui::EndChild();
	}

	void AgentSetupDialog::DrawSelection(AgentPlan const& plan, bool applied, float diffBottom)
	{
		AgentFilePlan const* const file = FindFile(plan, m_Selected);
		if (file == nullptr)
		{
			ImGui::TextDisabled("Select a file to see what changes in it.");
			return;
		}
		ImGui::SeparatorText(file->Path.c_str());
		if (file->Diff.empty())
		{
			ImGui::TextWrapped("%s", ExplainUnwritten(file->Action));
			return;
		}
		ImGui::TextDisabled(applied ? "What changed in Strada's part of the file:" : "What changes in Strada's part of the file:");
		// The rows that fit whole above diffBottom (one at least), up to VisibleDiffLines.
		float const diffHeight = FitRows(VisibleDiffLines, diffBottom - ImGui::GetCursorPosY());
		if (ImGui::BeginChild("##diff", ImVec2(0.0f, diffHeight), ImGuiChildFlags_Borders))
		{
			if (m_MonospaceFont != nullptr)
			{
				ImGui::PushFont(m_MonospaceFont, 0.0f);
			}
			// Long lines wrap at the pane's right edge (TextUnformatted wraps at the pushed position), each in its line's
			// colour, so that nothing needs a horizontal scroll to be read.
			ImGui::PushTextWrapPos(0.0f);
			std::string_view const diff = file->Diff;
			for (size_t start = 0; start < diff.size();)
			{
				size_t const end = std::min(diff.find('\n', start), diff.size());
				std::string_view const line = diff.substr(start, end - start);
				ImGui::PushStyleColor(ImGuiCol_Text, GetDiffLineColor(line));
				ImGui::TextUnformatted(line.data(), line.data() + line.size());
				ImGui::PopStyleColor();
				start = end + 1;
			}
			ImGui::PopTextWrapPos();
			if (m_MonospaceFont != nullptr)
			{
				ImGui::PopFont();
			}
		}
		ImGui::EndChild();
	}
}
