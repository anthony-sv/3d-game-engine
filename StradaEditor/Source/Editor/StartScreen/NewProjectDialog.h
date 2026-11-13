#pragma once

#include "Editor/StartScreen/NewProjectFormWidget.h"

#include <vector>

namespace Strada
{
	class StartScreenModel;
	class StartScreenThumbnails;

	// File > New Project...: a modal dialog holding the New Project form (NewProjectFormWidget, the start screen's form), so
	// that a new project made with a project open offers the templates, the setup for coding agents and the .gitignore too.
	// Opening it starts the form afresh (StartScreenModel::ResetForm) and reads the templates again; Create hands the
	// request on and closes it; Cancel and Escape close it. Main thread only.
	class NewProjectDialog
	{
	public:
		// The File menu's item.
		static constexpr char const* MenuItem = "New Project...";
		// The modal popup's name.
		static constexpr char const* PopupName = "New Project";

		// Opens the dialog in the next frame (and drops a pending Close).
		void Open();
		// Closes the dialog in the next frame (and drops a pending Open).
		void Close();
		bool IsOpenRequested() const { return m_OpenRequested; }
		// Whether the dialog was drawn open in the last frame.
		bool IsOpen() const { return m_Open; }

		void OnImGuiRender(StartScreenModel& model, StartScreenThumbnails* thumbnails, NewProjectFormActions const& actions);

		// What the dialog drew in its last frame.
		std::vector<StartScreenItem> const& GetItems() const { return m_Items; }

	private:
		NewProjectFormWidget m_Form;
		bool m_OpenRequested = false;
		bool m_CloseRequested = false;
		bool m_Open = false;
		std::vector<StartScreenItem> m_Items;
	};
}
