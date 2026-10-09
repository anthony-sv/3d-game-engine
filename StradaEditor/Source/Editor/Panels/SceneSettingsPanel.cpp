#include "Editor/Panels/SceneSettingsPanel.h"

#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <optional>
#include <string>

namespace Strada
{
	namespace
	{
		// Merge keys of this panel ("SCNS" in the high bytes, a running count below).
		constexpr uint64_t MergeKeyPrefix = 0x53434E5300000000ull;
	}

	SceneSettingsPanel::SceneSettingsPanel()
		: m_EditSession(MergeKeyPrefix),
		  m_Fields(GetFieldDescriptors<StructTraits<SceneSettings>, SceneSettings>())
	{
	}

	void SceneSettingsPanel::OnImGuiRender(EditorOperations& operations, bool& open)
	{
		if (!ImGui::Begin("Scene Settings", &open))
		{
			ImGui::End();
			return;
		}

		EditorContext& context = operations.GetContext();
		m_EditSession.Update(ImGui::IsAnyItemActive(), context.GetHistory());
		Scene& scene = context.GetScene();

		std::string name = scene.GetName();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Name");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::InputText("##sceneName", &name) && !name.empty())
		{
			UI::ReportFailure(operations.SetSceneProperties(name, Json::object(), m_EditSession.GetMergeKey()), "Renaming the scene");
		}
		ImGui::Spacing();

		Json const values = SceneSerializer::SerializeSettings(scene.GetSettings());
		if (std::optional<FieldChange> const change = m_FieldEditor.Draw(m_Fields, values))
		{
			UI::ReportFailure(operations.SetSceneProperties(std::nullopt, change->MakePatch(values), m_EditSession.GetMergeKey()),
			                  "Changing the scene settings");
		}

		ImGui::Spacing();
		if (ImGui::Button("Reset Renderer Settings"))
		{
			Json const defaults =
				Json::object({{"Renderer", SerializeFields<StructTraits<SceneRendererSettings>>(SceneRendererSettings())}});
			UI::ReportFailure(operations.SetSceneProperties(std::nullopt, defaults), "Resetting the renderer settings");
		}
		ImGui::End();
	}
}
