#pragma once

#include "Editor/UI/CurveEditing.h"
#include "Editor/UI/CurveEditor.h"
#include "Editor/UI/DataAssetEditor.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Math/Curve.h"

#include <cstdint>
#include <optional>
#include <string>

// The inspector's editor of .scurve assets: the drawer of the Curve data asset type (DataAssetEditor). It draws the shared
// curve editor widget (CurveEditor.h) over the asset's curve, its extrapolations shown beyond the keys, above the type's field
// table (the extrapolations and every number of the keys). Every edit of the widget is one undo step of the asset
// (SetDataAssetCommand, through the data asset editor), named as the timeline's are ("Move Key of Curve on Torque"):
// - a drag of keys or of a tangent is one step under a merge key of its own; Escape during it commits the curve it started
//   from under that key, so the step disappears;
// - a run of arrow-key nudges less than CurveEditor::NudgeRunSeconds apart is one step;
// - a header field (Time, Value) is one step while it stays active (the inspector's interaction);
// - every other edit (a key added or deleted, interpolation, tangents) is a step of its own.
// A drag whose curve changes elsewhere meanwhile (an undo, curve.set) ends without an edit: the other change stays. While the
// widget's view has the keyboard it takes Delete and Ctrl+Backspace (delete the selected keys), Ctrl+A, the arrows (Shift: 10
// steps), F (frame) and, during a drag, Escape, so they never reach the editor's editing keys. A curve whose asset is not
// editable is drawn and changes nothing. Main thread only, inside an ImGui frame (but the getters).
namespace Strada
{
	// Instantiated once, in CurveAssetEditor.cpp.
	extern template class CurveEditor<Curve>;

	class CurveAssetEditor
	{
	public:
		// The widget's height, in font sizes.
		static constexpr float Height = 16.0f;
		// The merge keys of its steps: "CRVA" plus a counter for drags and discrete edits, "CRVN" plus a counter for runs of
		// nudges, apart from the inspector's own keys.
		static constexpr uint64_t MergeKeyBase = 0x4352564100000000ull;
		static constexpr uint64_t NudgeMergeKey = 0x4352564E00000000ull;

		CurveAssetEditor();

		// Draws the asset's editor and returns the user's change of this frame (a DataAssetDrawer). A context of another asset
		// than the last forgets the view, the selection and any gesture.
		[[nodiscard]] std::optional<DataAssetEdit> Draw(DataAssetDrawContext const& context);
		// The drawer the data asset editor holds: it draws through the editor given, which keeps its state across frames.
		[[nodiscard]] static DataAssetDrawer MakeDrawer(Ref<CurveAssetEditor> editor);

		// The asset the editor drew last.
		AssetHandle GetAsset() const { return m_Asset; }
		CurveEditor<Curve>& GetEditor() { return m_Editor; }
		CurveEditor<Curve> const& GetEditor() const { return m_Editor; }

	private:
		// Routes the widget's keys while its view has the keyboard (in its child window).
		void HandleKeys();
		uint64_t MakeMergeKey() { return MergeKeyBase + ++m_Steps; }

		CurveEditor<Curve> m_Editor;
		AssetHandle m_Asset;
		// The drag in progress: its merge key, its description, and the curve its last edit set.
		uint64_t m_DragKey = 0;
		std::string m_DragAction;
		std::optional<Curve> m_Expected;
		uint64_t m_Steps = 0;
	};
}
