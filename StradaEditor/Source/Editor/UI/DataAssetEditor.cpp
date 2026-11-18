#include "Editor/UI/DataAssetEditor.h"

#include "Editor/Commands/SetDataAssetCommand.h"
#include "Editor/EditorOperations.h"
#include "Editor/UI/CurveAssetEditor.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Serialization/StructSerialization.h"

#include <imgui.h>

#include <span>
#include <utility>
#include <vector>

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

		// A mixer's body: the type's fields with every bus's effects in a fixed order. A snapshot value names an effect by
		// its index on its bus, and only the Mixer panel moves those values with their effects (ApplyMixerBusChange), so a
		// move here would leave each value on the effect that took its effect's place.
		DataAssetDrawer MakeMixerDrawer()
		{
			Ref<std::vector<FieldDescriptor>> const shown = CreateRef<std::vector<FieldDescriptor>>();
			return [shown](DataAssetDrawContext const& context) -> std::optional<DataAssetEdit>
			{
				if (shown->empty())
				{
					std::span<FieldDescriptor const> const table = context.Type.GetFields();
					shown->assign(table.begin(), table.end());
					for (FieldDescriptor& mixerField : *shown)
					{
						if (mixerField.Name != "Buses")
						{
							continue;
						}
						for (FieldDescriptor& busField : mixerField.Fields)
						{
							if (busField.Name == "Effects")
							{
								busField.Hints.FixedOrder = true;
							}
						}
					}
				}
				return DataAssetEditor::DrawFields(context, *shown);
			};
		}
	}

	DataAssetEditor::DataAssetEditor()
	{
		// The drawers of the engine's types that have one, one line per type, sorted by name:
		//   SetDrawer("<Name>", <drawer>);
		SetDrawer("Curve", CurveAssetEditor::MakeDrawer(CreateRef<CurveAssetEditor>()));
		SetDrawer("Mixer", MakeMixerDrawer());
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
		return DrawFields(context, context.Type.GetFields());
	}

	std::optional<DataAssetEdit> DataAssetEditor::DrawFields(DataAssetDrawContext const& context, std::span<FieldDescriptor const> fields)
	{
		ImGui::BeginDisabled(!context.Editable);
		std::optional<FieldChange> const change = context.Editor.Draw(fields, context.Fields);
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
