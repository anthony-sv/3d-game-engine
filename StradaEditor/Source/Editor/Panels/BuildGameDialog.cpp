#include "Editor/Panels/BuildGameDialog.h"

#include "Editor/FileDialogs.h"
#include "Editor/UI/EditorUI.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Platform/Desktop.h"
#include "Strada/Project/GameExporter.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <cmath>
#include <cstdio>

namespace Strada
{
	namespace
	{
		constexpr char const* PopupName = "Build Game";
		// Errors listed after a failed export; the console has them all.
		constexpr size_t MaxDiagnostics = 8;
		constexpr ImVec4 SuccessColor = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
		constexpr ImVec4 FailureColor = ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
		constexpr ImVec4 WarningColor = ImVec4(1.0f, 0.75f, 0.3f, 1.0f);

		char const* GetPlatformName(ExportPlatform platform)
		{
			switch (platform)
			{
				case ExportPlatform::Windows:
					return "Windows";
				case ExportPlatform::Linux:
					return "Linux";
				case ExportPlatform::MacOS:
					return "macOS";
			}
			return "this platform";
		}

		std::string FormatSize(uint64_t bytes)
		{
			char text[32];
			double const megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
			std::snprintf(text, sizeof(text), megabytes >= 1.0 ? "%.1f MB" : "%.0f KB", megabytes >= 1.0 ? megabytes : megabytes * 1024.0);
			return text;
		}
	}

	BuildGameDialog::BuildGameDialog()
	{
		for (FieldDescriptor field : GetFieldDescriptors<StructTraits<ProjectSettings>, ProjectSettings>())
		{
			if (field.Name == "Name" || field.Name == "StartScene" || field.Name == "Window")
			{
				m_Fields.push_back(std::move(field));
			}
		}
	}

	void BuildGameDialog::Open(Project const& project)
	{
		if (m_Project != project.GetFilePath())
		{
			m_Project = project.GetFilePath();
			m_Directory = FileSystem::PathToUtf8(project.GetDirectory() / "Build");
			m_LastExport.reset();
		}
		m_OpenRequested = true;
	}

	void BuildGameDialog::Flush(EditorOperations& operations)
	{
		if (m_SavePending)
		{
			UI::ReportFailure(operations.SaveProject(), "Saving the project settings");
			m_SavePending = false;
		}
	}

	void BuildGameDialog::OnImGuiRender(EditorOperations& operations, EditorExport& exporter)
	{
		if (std::exchange(m_OpenRequested, false))
		{
			ImGui::OpenPopup(PopupName);
		}
		ImGuiViewport const* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40.0f, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal(PopupName, nullptr, ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}
		if (m_SavePending && !ImGui::IsAnyItemActive())
		{
			Flush(operations);
		}

		EditorContext const& context = operations.GetContext();
		Project const* const project = context.GetProject();
		if (project == nullptr)
		{
			ImGui::TextDisabled("No project is open.");
			if (ImGui::Button("Close"))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
			return;
		}
		bool const running = exporter.IsRunning();
		ImGui::TextDisabled("Builds %s for %s from its saved files.", project->GetSettings().Name.c_str(),
		                    GetPlatformName(GameExporter::GetHostPlatform()));

		ImGui::BeginDisabled(running);
		DrawSettings(operations, *project);
		DrawOutput(running);
		ImGui::EndDisabled();
		if (context.IsDirty() && !running)
		{
			ImGui::TextColored(WarningColor, "The scene has unsaved changes: the game is built from the saved files.");
		}
		ImGui::Spacing();

		if (running)
		{
			// Indeterminate progress: a bar that sweeps while the step is shown.
			float const sweep = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.6f, 1.0f);
			std::string const step = exporter.GetProgress() + "...";
			ImGui::ProgressBar(-sweep, ImVec2(-1.0f, 0.0f), step.c_str());
			if (ImGui::Button("Cancel Export"))
			{
				exporter.Cancel();
			}
			ImGui::EndPopup();
			return;
		}
		if (m_LastExport)
		{
			DrawReport(exporter, *m_LastExport);
			ImGui::Spacing();
		}
		ImGui::BeginDisabled(m_Directory.empty() || context.IsPlaying());
		if (ImGui::Button("Build", ImVec2(ImGui::GetFontSize() * 6.0f, 0.0f)))
		{
			StartExport(operations, exporter);
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip(context.IsPlaying() ? "Stop playing first" : "Export the game for this platform");
		ImGui::SameLine();
		if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			Flush(operations);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void BuildGameDialog::DrawSettings(EditorOperations& operations, Project const& project)
	{
		ImGui::SeparatorText("Game");
		Json const values = SerializeFields<StructTraits<ProjectSettings>>(project.GetSettings());
		if (std::optional<FieldChange> const change = m_FieldEditor.Draw(m_Fields, values))
		{
			Result<void> const applied = operations.ApplyProjectSettings(change->MakePatch(values));
			UI::ReportFailure(applied, "Changing the project settings");
			m_SavePending = m_SavePending || applied.IsOk();
		}
	}

	void BuildGameDialog::DrawOutput(bool running)
	{
		ImGui::SeparatorText("Output");
		float const browseWidth = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::SetNextItemWidth(-(browseWidth + ImGui::GetStyle().ItemSpacing.x));
		ImGui::InputText("##directory", &m_Directory);
		ImGui::SameLine();
		if (ImGui::Button("Browse...") && !running)
		{
			Result<std::optional<std::filesystem::path>> chosen = FileDialogs::PickFolder(FileSystem::PathFromUtf8(m_Directory));
			if (!chosen)
			{
				ST_ERROR("{}", chosen.GetError());
			}
			else if (chosen.GetValue())
			{
				m_Directory = FileSystem::PathToUtf8(*chosen.GetValue());
			}
		}
		ImGui::TextDisabled("A new or empty directory, or an earlier export, which the game replaces.");
		ImGui::Checkbox("Run a smoke test", &m_SmokeTest);
		ImGui::SetItemTooltip("Runs the exported game headless for a few frames and checks that it exits cleanly");
	}

	void BuildGameDialog::StartExport(EditorOperations& operations, EditorExport& exporter)
	{
		Flush(operations);
		Project const* const project = operations.GetContext().GetProject();
		// Relative directories are relative to the project, as for project.export.
		std::filesystem::path const directory = (project->GetDirectory() / FileSystem::PathFromUtf8(m_Directory)).lexically_normal();
		m_LastExport.reset();
		EditorExportOptions options;
		options.SmokeTest = m_SmokeTest;
		Result<void> const started = exporter.Start(
			directory,
			[this](GameExportReport const& report)
			{
				m_LastExport = report;
			},
			options);
		if (!started)
		{
			GameExportReport refused;
			refused.Directory = directory;
			refused.Error = started.GetError();
			m_LastExport = std::move(refused);
		}
	}

	void BuildGameDialog::DrawReport(EditorExport& exporter, GameExportReport const& report)
	{
		ImGui::SeparatorText("Result");
		if (!report.Succeeded)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, FailureColor);
			ImGui::TextWrapped("The game was not exported: %s", report.Error.c_str());
			ImGui::PopStyleColor();
			size_t shown = 0;
			for (ScriptDiagnostic const& diagnostic : report.Diagnostics)
			{
				if (diagnostic.Severity != ScriptDiagnosticSeverity::Error)
				{
					continue;
				}
				if (shown++ == MaxDiagnostics)
				{
					ImGui::TextDisabled("More errors are in the console.");
					break;
				}
				ImGui::TextWrapped("%s(%u,%u): %s %s", FileSystem::PathToUtf8(diagnostic.File.filename()).c_str(), diagnostic.Line,
				                   diagnostic.Column, diagnostic.Code.c_str(), diagnostic.Message.c_str());
			}
			return;
		}

		ImGui::TextColored(SuccessColor, "Built in %.1f s: %u files, %s.", report.Seconds, report.Files, FormatSize(report.Bytes).c_str());
		ImGui::TextWrapped("%s", FileSystem::PathToUtf8(report.Executable).c_str());
		if (report.SmokeTestPassed == std::optional<bool>(true))
		{
			ImGui::TextColored(SuccessColor, "Smoke test passed: the game started and exited cleanly.");
		}
		else if (report.SmokeTestPassed)
		{
			ImGui::TextColored(FailureColor, "Smoke test failed:");
			ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.9f);
			ImGui::TextWrapped("%s", report.SmokeTestOutput.c_str());
			ImGui::PopFont();
		}
		if (ImGui::Button("Open Folder"))
		{
			UI::ReportFailure(Desktop::Open(report.Directory), "Opening the folder");
		}
		ImGui::SameLine();
		bool const gameRunning = exporter.IsGameRunning();
		if (ImGui::Button(gameRunning ? "Stop Game" : "Run Game"))
		{
			if (gameRunning)
			{
				exporter.StopGame();
			}
			else
			{
				UI::ReportFailure(exporter.RunGame(report.Executable), "Running the game");
			}
		}
	}
}
