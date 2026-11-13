#pragma once

#include "Editor/StartScreen/NewProjectFormWidget.h"
#include "Editor/StartScreen/StartScreenModel.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Strada
{
	class ExampleCopyTask;
	class StartScreenThumbnails;

	// What the start screen asks of the editor. Each may be unset: its button then does nothing.
	struct StartScreenActions
	{
		// Opens a project file (a recent project; the one Open Project... chose). The editor asks about unsaved changes first.
		std::function<void(std::filesystem::path const& projectFile)> OpenProject;
		// Open Project...: the system's file dialog for a project file, then OpenProject.
		std::function<void()> BrowseProject;
		// Open on an example: the editor asks about unsaved changes, then copies the example into the target (ExampleCopyTask:
		// the screen shows the copy's progress) and opens the copy.
		std::function<void(ExampleEntry const& example, std::filesystem::path const& target)> OpenExample;
		// Continue without a project.
		std::function<void()> Continue;
		// The New Project form's Browse... and Create.
		NewProjectFormActions Form;
	};

	// The editor's start screen, drawn over the whole work area of the main window (under the menu bar) from the start
	// screen's model while it is visible: the header (Strada and its version); the recent projects (name, folder, state;
	// a click opens one, the context menu has Open, Show in Folder and Remove from List; Open Project... below); the New
	// Project form (NewProjectFormWidget); the examples (a card each: Open copies it to <location>/<name>, or "<name> 2"...,
	// with the copy's progress and Cancel, or the hint of an example that is not available); Learn (the guides, opened
	// with the system's handler); the environment strip (.NET, the .NET SDK, the GPU and its Vulkan version) and Continue
	// without a project. What it shows is read again from the disk each time it appears (StartScreenModel::Refresh), not
	// every frame. The editor draws nothing else behind it but the menu bar (EditorLayer). Main thread only.
	class StartScreenPanel
	{
	public:
		static constexpr char const* WindowName = "Start Screen";

		// Draws the screen while the model is visible (nothing otherwise). copies is the editor's one example copy, which
		// project.copy-example shares: its progress shows on the card of the example it copies, and no other Open works
		// while it runs. thumbnails may be null (no images).
		void OnImGuiRender(StartScreenModel& model, ExampleCopyTask& copies, StartScreenThumbnails* thumbnails,
		                   StartScreenActions const& actions);

		// What the screen drew in its last frame (none while hidden): its buttons, cards, rows, links and menu items, named
		// "Open Project...", "Continue without a project", "Recent:<path>", "Recent menu:Open", "Recent menu:Show in Folder",
		// "Recent menu:Remove from List", "Example:<id>" (its Open), "Cancel copy", "Guide:<title>", the error line ("Error",
		// while StartScreenModel::GetError is not empty) and the form's (NewProjectFormWidget).
		std::vector<StartScreenItem> const& GetItems() const { return m_Items; }

	private:
		// Reads what the screen shows again (the model's content, the examples' availability and thumbnails).
		void Refresh(StartScreenModel& model);
		void DrawHeader();
		void DrawRecentProjects(StartScreenModel& model, StartScreenActions const& actions);
		void DrawNewProject(StartScreenModel& model, StartScreenThumbnails* thumbnails, StartScreenActions const& actions);
		void DrawExamples(StartScreenModel& model, ExampleCopyTask& copies, StartScreenThumbnails* thumbnails,
		                  StartScreenActions const& actions);
		void DrawLearn(StartScreenModel& model);
		void DrawFooter(StartScreenModel const& model, StartScreenActions const& actions);
		// Reports a failed action in the log and on the screen's error line (StartScreenModel::ReportError).
		static void Report(StartScreenModel& model, Result<void> const& result, std::string_view doing);

		// An example as the screen last read it.
		struct ExampleView
		{
			bool Available = false;
			std::filesystem::path Thumbnail;
		};

		NewProjectFormWidget m_Form;
		// By example id.
		std::map<std::string, ExampleView> m_Examples;
		std::vector<StartScreenItem> m_Items;
		// The frame the screen was last drawn in (ImGui's frame count); -1 before the first.
		int m_LastFrame = -1;
	};
}
