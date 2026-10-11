#pragma once

#include "Editor/EditorExport.h"
#include "Editor/EditorOperations.h"
#include "Editor/UI/FieldEditor.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strada
{
	// File > Build Game: the game's settings (name, start scene, window: title, size, fullscreen, vsync, resizing, icon),
	// the output directory (the project's Build directory at first), a smoke test that runs the exported game headless,
	// the export's progress, and the outcome: the executable, the size of the export, the smoke test's result and buttons
	// that open its folder and run the game. Settings edits go to the project file like the Project Settings panel's.
	class BuildGameDialog
	{
	public:
		BuildGameDialog();

		// Opens the dialog for the open project (the output directory starts at its Build directory).
		void Open(Project const& project);
		void OnImGuiRender(EditorOperations& operations, EditorExport& exporter);
		// Writes settings changes that wait for the end of an interaction (call before the editor closes).
		void Flush(EditorOperations& operations);

	private:
		void DrawSettings(EditorOperations& operations, Project const& project);
		void DrawOutput(bool running);
		void DrawReport(EditorExport& exporter, GameExportReport const& report);
		void StartExport(EditorOperations& operations, EditorExport& exporter);

		bool m_OpenRequested = false;
		FieldEditor m_FieldEditor;
		// The project's name, start scene and window fields.
		std::vector<FieldDescriptor> m_Fields;
		bool m_SavePending = false;
		std::string m_Directory;
		// The project the directory was chosen for.
		std::filesystem::path m_Project;
		bool m_SmokeTest = true;
		std::optional<GameExportReport> m_LastExport;
	};
}
