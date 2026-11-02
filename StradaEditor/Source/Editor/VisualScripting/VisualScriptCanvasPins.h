#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// The pins of a canvas's nodes as the editing operations take them (VisualScriptNodePins), shared by a graph window's panes
// and graph.edit's headless canvas, so both hand the operations the same rows. Layer L3b: it reads the canvas model and the
// editing operations' types; no ImGui. Main thread only, like the canvas model.
namespace Strada
{
	// The pins of a node created in the same edit (a palette node, a dropped variable), which the canvas does not show yet:
	// the ID and the pins of its palette entry or library template, in their order.
	struct VisualScriptNewNodePins
	{
		uint32_t Node = 0;
		std::span<VisualScriptPin const> Pins;
	};

	// The library pin behind a pin of a node the canvas shows (by the name the canvas gives it): the analysis' while the
	// canvas draws the node from that analysis (VisualScriptNodeFlags::Analyzed), else the library template's of the node's
	// type in the shown document; null when neither has it. analysis, shownDocument and library may be null.
	[[nodiscard]] VisualScriptPin const*
	FindVisualScriptCanvasLibraryPin(VisualScriptCanvasModel const& canvas, VisualScriptAnalysis const* analysis,
	                                 VisualScriptDocument const* shownDocument, VisualScriptLibrary const* library,
	                                 VisualScriptNodeVisual const& visual, std::string_view pin, VisualScriptPinDirection direction);

	// The pins of the canvas's nodes as VisualScriptEditing's operations take them: each node's rows (orphans left out) under
	// the names the document stores (VisualScriptEditing::ToStoredVisualScriptPin), with the defaults of the analysis' pin,
	// else the library template's (FindVisualScriptCanvasLibraryPin), and the canvas's anchors; and the new node's rows, in
	// its pins' order at no particular anchor. analysis is the one the canvas was synced with, shownDocument the document it
	// shows. Valid until the canvas's next Sync; the analysis, the document, the library and the new node's pins must outlive
	// it too.
	[[nodiscard]] VisualScriptNodePins MakeVisualScriptCanvasNodePins(VisualScriptCanvasModel const& canvas,
	                                                                  VisualScriptAnalysis const* analysis,
	                                                                  VisualScriptDocument const* shownDocument,
	                                                                  VisualScriptLibrary const* library,
	                                                                  std::optional<VisualScriptNewNodePins> newNode = std::nullopt);
}
