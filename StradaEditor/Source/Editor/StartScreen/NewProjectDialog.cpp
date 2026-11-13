#include "Editor/StartScreen/NewProjectDialog.h"

#include "Editor/StartScreen/StartScreenModel.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <utility>

namespace Strada
{
	namespace
	{
		// The share of the window's height the form's fields may take before they scroll, so the dialog and its buttons fit.
		constexpr float MaxFieldsShare = 0.72f;
	}

	void NewProjectDialog::Open()
	{
		m_OpenRequested = true;
		m_CloseRequested = false;
	}

	void NewProjectDialog::Close()
	{
		m_CloseRequested = true;
		m_OpenRequested = false;
	}

	void NewProjectDialog::OnImGuiRender(StartScreenModel& model, StartScreenThumbnails* thumbnails, NewProjectFormActions const& actions)
	{
		m_Items.clear();
		if (std::exchange(m_OpenRequested, false))
		{
			model.ResetForm();
			model.Refresh();
			m_Form.Invalidate();
			ImGui::OpenPopup(PopupName);
		}
		ImGuiViewport const* const viewport = ImGui::GetMainViewport();
		// Centered in every frame: its height follows the form (the agent files' list, a warning), and a window that grows
		// past the editor window would get a window of its own. It stays in the editor window, as captures read it
		// (imgui.cpp WindowSelectViewport gives an appearing popup that does not fit its host viewport one of its own).
		ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowViewport(viewport->ID);
		float const width = std::min(ImGui::GetFontSize() * 46.0f, viewport->WorkSize.x * 0.9f);
		ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, viewport->WorkSize.y * 0.95f));
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
		if (std::exchange(m_CloseRequested, false))
		{
			ImGui::CloseCurrentPopup();
		}
		bool const escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

		ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, viewport->WorkSize.y * MaxFieldsShare));
		if (ImGui::BeginChild("##fields", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY))
		{
			m_Form.DrawFields(model, thumbnails, actions, &m_Items);
		}
		ImGui::EndChild();
		ImGui::Separator();
		bool cancelled = escape;
		bool const created =
			m_Form.DrawCreateButton(model, actions, &m_Items,
		                            [this, &cancelled]
		                            {
										cancelled = ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f)) || cancelled;
										RecordStartScreenItem(&m_Items, "Cancel", true);
									});
		if (created || cancelled)
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}
