#pragma once

#include "Editor/StartScreen/StartScreenModel.h"

#include "Strada/Core/Result.h"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Strada
{
	class StartScreenThumbnails;

	// An item the start screen or the New Project form drew in its last frame: tests click it and check that it shows, and
	// captures can be explained by it.
	struct StartScreenItem
	{
		// What it is: "Create", "Browse...", "Template:<id>" (a template's card), "Set up for coding agents", "Add a .gitignore",
		// and the start screen's own (StartScreenPanel::OnImGuiRender).
		std::string Name;
		// Where it is on the screen.
		ImVec2 Min = ImVec2(0.0f, 0.0f);
		ImVec2 Max = ImVec2(0.0f, 0.0f);
		// Whether all of it shows: inside the area its window draws in (not cut off by a scrolling region or the screen's edge).
		bool Visible = false;
		// Whether it could be clicked.
		bool Enabled = false;

		ImVec2 GetCenter() const { return ImVec2((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f); }
	};

	// What the New Project form asks of the editor. Each may be unset: its button then does nothing.
	struct NewProjectFormActions
	{
		// Browse...: the system's folder dialog, starting at the given folder; none when the user cancelled it.
		std::function<Result<std::optional<std::filesystem::path>>(std::filesystem::path const& start)> PickFolder;
		// Create: the editor asks about unsaved changes, then creates the project (CreateProjectFromRequest).
		std::function<void(NewProjectRequest const& request)> Create;
	};

	// The New Project form, one widget that the start screen and File > New Project... both draw over the start screen's
	// model (its NewProjectForm): a card per template (thumbnail, name, description and what it needs), Name, Location with
	// Browse..., the line "Creates <folder>", "Set up for coding agents" with the files it writes, "Add a .gitignore", and
	// Create, disabled with the reason while the form cannot create. The checks that look at the disk run when the form
	// changes and at most once a second otherwise, never every frame. Main thread only.
	class NewProjectFormWidget
	{
	public:
		// The template cards and the fields, in the current window (the caller's scrolling region). Items go to items when
		// given.
		void DrawFields(StartScreenModel& model, StartScreenThumbnails* thumbnails, NewProjectFormActions const& actions,
		                std::vector<StartScreenItem>* items);
		// Create, then what besideCreate draws on its row (the dialog's Cancel), then the reason Create is disabled. True when
		// it was clicked and the request went to actions.Create.
		bool DrawCreateButton(StartScreenModel& model, NewProjectFormActions const& actions, std::vector<StartScreenItem>* items,
		                      std::function<void()> const& besideCreate = {});
		// How much height DrawCreateButton takes, for a caller that keeps it below a scrolling region.
		static float GetCreateRowHeight();
		// Checks the form and finds the templates' thumbnails again at the next draw (the disk may have changed: the screen
		// showed again, a project was created).
		void Invalidate();

	private:
		NewProjectCheck const& GetCheck(StartScreenModel const& model);
		std::vector<std::string> const& GetAgentFiles(StartScreenModel const& model);
		void DrawTemplateCards(StartScreenModel& model, StartScreenThumbnails* thumbnails, std::vector<StartScreenItem>* items);

		// The form the check was made for, the check, and when (ImGui's time).
		NewProjectForm m_CheckedForm;
		std::optional<NewProjectCheck> m_Check;
		double m_CheckedAt = 0.0;
		// The files a setup writes, for the .gitignore choice they were listed with.
		std::optional<bool> m_AgentFilesGitIgnore;
		std::vector<std::string> m_AgentFiles;
		// Why Browse... failed, shown under the location.
		std::string m_BrowseError;
		// The thumbnail file of each template, by id (empty: none), found once after each Invalidate.
		std::map<std::string, std::filesystem::path> m_TemplateThumbnails;
	};

	// Records the last item drawn (ImGui's last item) into items, when given.
	void RecordStartScreenItem(std::vector<StartScreenItem>* items, std::string name, bool enabled);
	// Records an item of the current window that lies between min and max on the screen.
	void RecordStartScreenItem(std::vector<StartScreenItem>* items, std::string name, bool enabled, ImVec2 itemMin, ImVec2 itemMax);
}
