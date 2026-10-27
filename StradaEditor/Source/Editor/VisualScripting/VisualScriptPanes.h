#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCommit.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptEditorTypes.h"
#include "Editor/VisualScripting/VisualScriptPalette.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/UUID.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ImDrawList;
struct ImVec2;

// The panes of a graph window (the palette popup, My Script, Details, the drawer and the find bar): what the window gives
// them each frame and what they hand back. Panes are drawn with ImGui; what they edit, they return as commit requests, which
// the window executes as it executes its canvas's (EditorOperations::EditVisualScript). Main thread only.
namespace Strada
{
	class EditSession;

	// What a pane works with in a frame.
	struct VisualScriptPaneContext
	{
		VisualScriptEditorModel& Model;
		VisualScriptSemantics& Semantics;
		VisualScriptViewSettings& Settings;
		EditSession& Session;
		VisualScriptEditorServices const& Services;
		AssetHandle Graph;
		// The script's name, which commit descriptions end with ("Add Branch in Player").
		std::string_view ScriptName;
		UUID ContextEntity = UUID::Invalid();
		bool Playing = false;
		// Seconds: the clock of the model's Sync.
		double Time = 0.0;
		// The canvas's text measurer of the frame: the palette lays a new node out with it, to put the pin it connects where
		// the wire was released. An empty measurer places that pin by the node's top-left corner instead.
		VisualScriptTextMeasurer Measure;
		// Draws a node as the canvas does (the palette's hover preview), its top-left at the position, at the scale; unset in
		// the panes' own tests and until the window sets it.
		std::function<void(VisualScriptPaletteEntry const&, ImDrawList*, ImVec2 position, float scale)> DrawNodePreview;
	};

	// What a pane asks of the window after drawing.
	struct VisualScriptPaneResult
	{
		std::optional<VisualScriptCommitRequest> Commit;
		// Another graph, or a jump in this one (find results, problems).
		std::optional<VisualScriptOpenRequest> Open;
		// Nodes and comments of the shown sub-graph to select and frame.
		std::vector<uint32_t> SelectAndFrame;
		std::optional<uint32_t> ShowFunction;
		// Toasts.
		std::vector<std::string> Notes;
		// A setting of the committed node that needs a value (an Event.Custom's Name, a Convert's To): Details gives it the
		// keyboard once it shows the node. Empty when none.
		std::string FocusSetting;
		// A widget interaction is in progress: its edits merge into one step until it ends.
		bool WidgetActive = false;
	};

	// What a row of My Script stands for, as VisualScriptElementPayload::Kind holds it.
	enum class VisualScriptElementKind : uint32_t
	{
		Variable = 0,
		Local,
		Function,
		// An Event.Custom node of the event graph, by its node ID.
		CustomEvent
	};

	// A dragged My Script row as the canvas' drop carries it (VisualScriptCanvasDrop::Payload, one uint64_t): the kind in the
	// high 32 bits, the ID in the low 32. The window accepts rows of its own graph only (VisualScriptElementPayload::Graph).
	[[nodiscard]] constexpr uint64_t PackVisualScriptElement(VisualScriptElementKind kind, uint32_t id)
	{
		return (static_cast<uint64_t>(kind) << 32) | id;
	}
	// The kind and ID of a packed row; nothing for a kind this editor does not know.
	[[nodiscard]] std::optional<std::pair<VisualScriptElementKind, uint32_t>> UnpackVisualScriptElement(uint64_t packed);

	// The pins of a node created in the same edit (a palette node, a dropped variable), which the canvas does not show yet:
	// the ID and the pins of its palette entry or library template, in their order.
	struct VisualScriptNewNodePins
	{
		uint32_t Node = 0;
		std::span<VisualScriptPin const> Pins;
	};

	// The pins of the shown sub-graph's nodes as VisualScriptEditing's operations take them: the canvas's rows of each node
	// (orphans left out) under the names the document stores (VisualScriptEditing::ToStoredVisualScriptPin), with the
	// defaults of the analysis' pin, else the library template's; and the new node's rows, in its pins' order at no
	// particular anchor. Valid until the model's next Sync; the new node's pins must outlive it too.
	[[nodiscard]] VisualScriptNodePins MakeVisualScriptNodePins(VisualScriptEditorModel const& model, VisualScriptLibrary const* library,
	                                                            std::optional<VisualScriptNewNodePins> newNode = std::nullopt);

	// The library pin of a pin of the shown sub-graph as the canvas names it: the analysis' (with its resolved type) while the
	// analysis describes the node, else the library template's with the type the canvas knows; nothing for a pin the node
	// does not have.
	[[nodiscard]] std::optional<VisualScriptPin> FindVisualScriptCanvasPin(VisualScriptEditorModel const& model,
	                                                                       VisualScriptLibrary const* library, uint32_t node,
	                                                                       std::string_view pin, VisualScriptPinDirection direction);
}
