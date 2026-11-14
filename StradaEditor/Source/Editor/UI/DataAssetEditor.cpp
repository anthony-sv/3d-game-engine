#include "Editor/UI/DataAssetEditor.h"

#include "Editor/Commands/SetDataAssetCommand.h"
#include "Editor/EditorOperations.h"
#include "Editor/UI/CurveAssetEditor.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"

#include <imgui.h>

#include <utility>

namespace Strada
{
	namespace
	{
		constexpr ImVec4 ErrorColor = ImVec4(0.95f, 0.40f, 0.35f, 1.0f);

		void TextError(std::string const& message)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ErrorColor);
			ImGui::TextWrapped("%s", message.c_str());
			ImGui::PopStyleColor();
		}
	}

	DataAssetEditor::DataAssetEditor()
	{
		// The drawers of the engine's types that have one, one line per type, sorted by name:
		//   SetDrawer("<Name>", <drawer>);
		SetDrawer("Curve", CurveAssetEditor::MakeDrawer(CreateRef<CurveAssetEditor>()));
	}

	void DataAssetEditor::SetDrawer(std::string typeName, DataAssetDrawer drawer)
	{
		m_Drawers[std::move(typeName)] = std::move(drawer);
	}

	bool DataAssetEditor::HasDrawer(std::string_view typeName) const
	{
		return m_Drawers.contains(std::string(typeName));
	}

	void DataAssetEditor::Draw(EditorOperations& operations, FieldEditor& editor, AssetHandle asset, EditSession& session)
	{
		Result<Ref<Asset>> const loaded = AssetManager::LoadAsset(asset);
		if (!loaded)
		{
			TextError(loaded.GetError());
			return;
		}
		Ref<DataAsset> const dataAsset = std::dynamic_pointer_cast<DataAsset>(loaded.GetValue());
		if (dataAsset == nullptr)
		{
			TextError(fmt::format("Asset {} is not a data asset.", asset));
			return;
		}
		DataAssetType const& type = dataAsset->GetDataAssetType();
		Result<void> const editable = SetDataAssetCommand::CheckEditable(asset);
		if (!editable)
		{
			// Data assets made in memory, and read-only files: shown as they are, disabled.
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("%s", UI::FormatSentence(editable.GetError()).c_str());
			ImGui::PopStyleColor();
			ImGui::Spacing();
		}

		Json const fields = dataAsset->SerializeFields();
		DataAssetDrawContext const context{asset, type, fields, editable.IsOk(), editor};
		auto const drawer = m_Drawers.find(type.GetName());
		std::optional<DataAssetEdit> edit = drawer != m_Drawers.end() ? drawer->second(context) : DrawFields(context);
		if (!edit || !editable)
		{
			return;
		}
		uint64_t const mergeKey = edit->MergeKey != 0 ? edit->MergeKey : session.GetMergeKey();
		UI::ReportFailure(operations.SetDataAssetFields(asset, edit->Patch, std::move(edit->Description), mergeKey),
		                  fmt::format("Editing the {}", UI::FormatDisplayName(type.GetName())));
	}

	std::optional<DataAssetEdit> DataAssetEditor::DrawFields(DataAssetDrawContext const& context)
	{
		ImGui::BeginDisabled(!context.Editable);
		std::optional<FieldChange> const change = context.Editor.Draw(context.Type.GetFields(), context.Fields);
		ImGui::EndDisabled();
		if (!change || !context.Editable)
		{
			return std::nullopt;
		}
		// A struct drawer's description and key come along; empty and 0 are the type's step and the inspector's interaction.
		return DataAssetEdit{change->MakePatch(context.Fields), change->Description, change->MergeKey};
	}

	bool DataAssetEditor::IsEditable(AssetHandle asset)
	{
		return SetDataAssetCommand::CheckEditable(asset).IsOk();
	}
}
