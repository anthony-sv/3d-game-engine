#include "Editor/UI/DataAssetEditor.h"

#include "Editor/Commands/SetDataAssetCommand.h"
#include "Editor/EditorOperations.h"
#include "Editor/MixerEditing.h"
#include "Editor/UI/CurveAssetEditor.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/Mixer.h"
#include "Strada/Asset/MixerAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructSerialization.h"

#include <imgui.h>

#include <optional>
#include <string>
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

		// What a refused edit of an asset of the type is reported as.
		std::string DescribeEditing(DataAssetType const& type)
		{
			return fmt::format("Editing the {}", UI::FormatDisplayName(type.GetName()));
		}

		// The type's field table drawn by the inspector's field editor, disabled unless the asset is editable: the user's
		// change of this frame, none when the asset is not editable.
		std::optional<FieldChange> DrawFieldTable(DataAssetDrawContext const& context)
		{
			ImGui::BeginDisabled(!context.Editable);
			std::optional<FieldChange> change = context.Editor.Draw(context.Type.GetFields(), context.Fields);
			ImGui::EndDisabled();
			if (!context.Editable)
			{
				return std::nullopt;
			}
			return change;
		}

		// A mixer's body: the type's field table, whose edits are applied as the Mixer panel applies them (ApplyMixerChange).
		// A snapshot value names an effect by its index on its bus, so a list edit of a bus's effects (an effect added,
		// removed, moved or duplicated) moves the values with their effects instead of leaving each on the index it had.
		std::optional<DataAssetEdit> DrawMixer(DataAssetDrawContext const& context)
		{
			std::optional<FieldChange> const change = DrawFieldTable(context);
			if (!change || change->Path.empty())
			{
				return std::nullopt;
			}
			Mixer mixer;
			Result<void> applied = DeserializeFields<StructTraits<Mixer>>(context.Fields, mixer, DeserializationContext{}, "mixer");
			if (applied)
			{
				applied = ApplyMixerChange(mixer, *change);
			}
			if (!applied)
			{
				UI::ReportFailure(applied, DescribeEditing(context.Type));
				return std::nullopt;
			}
			// The field the change is in, as the field editor's own patch has it, and every other field the change moved (the
			// snapshots, when their values followed their effects).
			Json const values = SerializeFields<StructTraits<Mixer>>(mixer);
			Json patch = Json::object();
			for (auto const& [name, value] : values.items())
			{
				auto const old = context.Fields.find(name);
				if (name == change->Path.front().Field || old == context.Fields.end() || *old != value)
				{
					patch[name] = value;
				}
			}
			return DataAssetEdit{std::move(patch), change->Description, change->MergeKey};
		}
	}

	DataAssetEditor::DataAssetEditor()
	{
		// The drawers of the engine's types that have one, one line per type, sorted by name:
		//   SetDrawer("<Name>", <drawer>);
		SetDrawer("Curve", CurveAssetEditor::MakeDrawer(CreateRef<CurveAssetEditor>()));
		SetDrawer("Mixer", &DrawMixer);
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
		UI::ReportFailure(operations.SetDataAssetFields(asset, edit->Patch, std::move(edit->Description), mergeKey), DescribeEditing(type));
	}

	std::optional<DataAssetEdit> DataAssetEditor::DrawFields(DataAssetDrawContext const& context)
	{
		std::optional<FieldChange> const change = DrawFieldTable(context);
		if (!change)
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
