#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptCanvasPins.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptEditorTypes.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <cstdint>
#include <optional>

// A sub-graph's canvas without a window, for what sizes nodes or reads their pins headless (graph.arrange, graph.edit). Layer
// L3b; no ImGui, no call into the scripting runtime. Main thread only, like the canvas model.
namespace Strada
{
	// A VisualScriptCanvasModel synced with a copy of a document whose unplaced nodes are placed (PlaceUnplacedNodes, as a
	// window shows them), the analysis (else the library's templates, else what the document names: the degraded canvas whose
	// pins are inferred from links and literals), VisualScriptThemeAnchors{} and MakeEstimatingVisualScriptTextMeasurer().
	// Node heights equal a window's; widths are at least a window's for the editor's fonts, and at most about 15 % more.
	class VisualScriptHeadlessCanvas
	{
	public:
		// Lays out the sub-graph (0: the event graph, else a function's ID; an unknown ID shows nothing). analysis is of
		// analyzedDocument (a node takes its analysis' pins only while it still matches what was analyzed, as in a window); both
		// may be null, and library too. analysis, analyzedDocument and library must stay valid until the next Sync; document
		// is copied.
		void Sync(VisualScriptDocument const& document, uint32_t function, VisualScriptAnalysis const* analysis,
		          VisualScriptDocument const* analyzedDocument, VisualScriptLibrary const* library);

		// The size of each node as a window draws it (VisualScriptCanvasModel::GetExtents). Valid until the next Sync.
		[[nodiscard]] VisualScriptNodeExtents GetExtents() const;
		// The nodes' pins as the editing operations take them (MakeVisualScriptCanvasNodePins), anchors included. Valid until
		// the next Sync; the new node's pins must outlive it too.
		[[nodiscard]] VisualScriptNodePins GetPins(std::optional<VisualScriptNewNodePins> newNode = std::nullopt) const;

		// The canvas as the last Sync laid it out, and the document it shows (its unplaced nodes placed).
		[[nodiscard]] VisualScriptCanvasModel const& GetCanvas() const { return m_Canvas; }
		[[nodiscard]] VisualScriptDocument const& GetDisplay() const { return m_Display; }

	private:
		VisualScriptDocument m_Display;
		VisualScriptCanvasModel m_Canvas;
		VisualScriptAnalysis const* m_Analysis = nullptr;
		VisualScriptLibrary const* m_Library = nullptr;
	};
}
