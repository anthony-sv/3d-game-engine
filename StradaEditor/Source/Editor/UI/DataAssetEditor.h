#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Strada
{
	class DataAssetType;
	class EditorOperations;
	class FieldEditor;

	// What the body of a data asset's editor draws from (a drawer's argument).
	struct DataAssetDrawContext
	{
		AssetHandle Asset;
		DataAssetType const& Type;
		// Every field as it is now (DataAsset::SerializeFields).
		Json const& Fields;
		// The asset has a file of its own that is not read-only (DataAssetEditor::IsEditable): drawers show the fields
		// disabled otherwise and report no change.
		bool Editable = false;
		// The inspector's field editor, with its asset previews.
		FieldEditor& Editor;
	};

	// The body a type's editor draws in the inspector instead of the default one: returns, when the user changed something
	// this frame, a partial patch of the fields (the content object's form: { "<Field>": <whole new value> }), which the
	// data asset editor applies as one undo step of the inspector's interaction (EditorOperations::SetDataAssetFields with
	// the inspector's merge key), the step <domain>.set makes. A drawer never writes files or assets itself. It may draw the
	// default body for the fields it does not draw (DataAssetEditor::DrawFields), the others hidden in editors in the type's
	// field table (FieldHints::HiddenInEditors). Main thread only, inside an ImGui frame.
	using DataAssetDrawer = std::function<std::optional<Json>(DataAssetDrawContext const& context)>;

	// The editor of data assets, drawn by the inspector below the asset's name and reference: the default body is the type's
	// field table drawn by the FieldEditor (asset fields, nested structs, arrays of structs), and a type may have a drawer of
	// its own. Edits of assets with a file of their own become undo steps (SetDataAssetCommand); other data assets (made in
	// memory) are shown disabled. Owned by the inspector panel.
	class DataAssetEditor
	{
	public:
		// With the drawers of the engine's types that have one (a list in DataAssetEditor.cpp, where a type's track adds its
		// line).
		DataAssetEditor();

		// Gives the registered type of that name a drawer of its own, replacing an earlier one.
		void SetDrawer(std::string typeName, DataAssetDrawer drawer);
		bool HasDrawer(std::string_view typeName) const;

		// Draws the body of the data asset's editor and applies the user's change through operations (failures are reported
		// with UI::ReportFailure). Shows the load error of an asset that does not load. Main thread only, inside an ImGui frame.
		void Draw(EditorOperations& operations, FieldEditor& editor, AssetHandle asset, uint64_t mergeKey);

		// The default body: the type's field table (fields hidden in editors left out), disabled unless context.Editable.
		// Returns the user's change as a partial patch.
		static std::optional<Json> DrawFields(DataAssetDrawContext const& context);
		// Whether edits of the asset can be kept: a data asset of a registered type with a file of its own, present and not
		// read-only. Main thread only (reads the asset manager).
		[[nodiscard]] static bool IsEditable(AssetHandle asset);

	private:
		std::unordered_map<std::string, DataAssetDrawer> m_Drawers;
	};
}
