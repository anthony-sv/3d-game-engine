#include "Editor/Panels/AgentSetupDialog.h"

#include "Editor/UI/EditorUI.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr ImVec4 SuccessColor = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
		constexpr ImVec4 FailureColor = ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
		constexpr ImVec4 WarningColor = ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
		// The rows the file list shows before it scrolls (every file of the kit) and the lines of a diff, each at most this
		// share of the window's height, so that the whole dialog fits a small window.
		constexpr float VisibleFiles = 14.0f;
		constexpr float VisibleDiffLines = 10.0f;
		constexpr float MaxFilesShare = 0.3f;
		constexpr float MaxDiffShare = 0.2f;

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

		ImGui::TextWrapped("Sets %s up for coding agents such as Claude Code: AGENTS.md tells them how to work, .mcp.json "
		                   "connects them to this editor through the strada MCP server, and the game-making skill and the "
		                   "guides teach them the tools. Text you wrote outside Strada's parts of these files is kept.",
		                   project->Name.c_str());
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", FileSystem::PathToUtf8(project->Directory).c_str());
		ImGui::PopTextWrapPos();
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
		DrawFiles(shown, applied);
		DrawSelection(shown, applied);
		for (std::string const& warning : shown.Warnings)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, WarningColor);
			// The kit's warnings are messages (also in project.setup-agents' result): the dialog shows them as sentences.
			ImGui::TextWrapped("%s", UI::FormatSentence(warning).c_str());
			ImGui::PopStyleColor();
		}
		if (applied)
		{
			DrawResult(*m_Result);
		}
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

	void AgentSetupDialog::DrawFiles(AgentPlan const& plan, bool applied)
	{
		ImGui::SeparatorText(applied ? "What the setup did" : "What the setup does");
		ImGuiStyle const& style = ImGui::GetStyle();
		float const rows = std::min(static_cast<float>(plan.Files.size()), VisibleFiles);
		float const height = std::min(rows * ImGui::GetTextLineHeightWithSpacing() + style.WindowPadding.y * 2.0f,
		                              ImGui::GetMainViewport()->WorkSize.y * MaxFilesShare);
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

	void AgentSetupDialog::DrawSelection(AgentPlan const& plan, bool applied)
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
		float const height = std::min(VisibleDiffLines * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y * 2.0f,
		                              ImGui::GetMainViewport()->WorkSize.y * MaxDiffShare);
		if (ImGui::BeginChild("##diff", ImVec2(0.0f, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
		{
			if (m_MonospaceFont != nullptr)
			{
				ImGui::PushFont(m_MonospaceFont, 0.0f);
			}
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
			if (m_MonospaceFont != nullptr)
			{
				ImGui::PopFont();
			}
		}
		ImGui::EndChild();
	}

	void AgentSetupDialog::DrawResult(AgentApplyResult const& result)
	{
		ImGui::SeparatorText("Result");
		if (result.Succeeded())
		{
			size_t const written = static_cast<size_t>(std::ranges::count_if(result.Plan.Files,
			                                                                 [](AgentFilePlan const& file)
			                                                                 {
																				 return Writes(file.Action);
																			 }));
			ImGui::PushStyleColor(ImGuiCol_Text, SuccessColor);
			ImGui::TextWrapped("The project is set up for coding agents: %zu files written. An agent started in the project's "
			                   "folder reads AGENTS.md and reaches this editor through .mcp.json.",
			                   written);
			ImGui::PopStyleColor();
			return;
		}
		ImGui::PushStyleColor(ImGuiCol_Text, FailureColor);
		if (result.FailedPath.empty())
		{
			ImGui::TextWrapped("The setup wrote nothing: %s", result.Error.c_str());
		}
		else
		{
			// The error names the file.
			ImGui::TextWrapped("The setup stopped: %s. The files listed above stay written.", result.Error.c_str());
		}
		ImGui::PopStyleColor();
	}
}
