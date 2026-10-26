#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptSpatialIndex.h"
#include "Editor/VisualScripting/VisualScriptView.h"

#include "Strada/Asset/VisualScriptAsset.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Strada
{
	// What is under the mouse on the canvas, in the order hit testing tries them.
	enum class VisualScriptHitKind : uint8_t
	{
		None = 0,
		Minimap,
		Pin,
		// A node's problem badge.
		Badge,
		NodeHeader,
		Node,
		Reroute,
		Link,
		// An edge or a corner of a comment box (its resize handles).
		CommentEdge,
		CommentTitle,
		// The empty canvas, comment bodies included (a marquee can start inside a comment).
		Background
	};

	struct VisualScriptHit
	{
		VisualScriptHitKind Kind = VisualScriptHitKind::None;
		// Pin, Badge, NodeHeader, Node and Reroute: the node's ID; CommentEdge and CommentTitle: the comment's.
		uint32_t Element = 0;
		// Pin: the pin's index in the visual model's pins. A collapsed node's pins are hit at its two anchors, each standing for
		// every pin of its side: the hit names the side's first pin.
		uint32_t Pin = 0;
		// Link: the link as the document stores it.
		std::optional<VisualScriptLink> Link;
		// CommentEdge: VisualScriptGeometry::LeftEdge, TopEdge, RightEdge and BottomEdge (a corner sets two).
		uint8_t Edges = 0;
		// Link: where on the wire, as a fraction of its length from its output.
		float ArcFraction = 0.0f;

		bool operator==(VisualScriptHit const& other) const = default;
	};

	// The order nodes are drawn in, which hit testing follows backwards so the top drawn node is the one hit: unselected
	// nodes before selected ones, then by their raise counter (a node is raised when pressed or dragged), then in document
	// order. The renderer sorts the nodes it draws by this key, ascending.
	constexpr uint64_t GetVisualScriptNodeDrawKey(bool selected, uint32_t raise, uint32_t nodeIndex)
	{
		uint64_t const raised = raise < 0x7FFFFFFFu ? raise : 0x7FFFFFFFu;
		return (selected ? (1ull << 63) : 0ull) | (raised << 32) | nodeIndex;
	}

	// Buffers a hit test works in; kept by the caller, a hit test allocates nothing.
	struct VisualScriptHitScratch
	{
		std::vector<uint32_t> Candidates;
		std::vector<uint64_t> Nodes;
		std::vector<glm::vec2> Polyline;
	};

	// What a hit test needs besides the model, the index and the view.
	struct VisualScriptHitContext
	{
		// Screen pixels: the minimap's rectangle while it is shown (it takes hits before the canvas).
		std::optional<VisualScriptRect> Minimap;
		// Per node index of the visual model: non-zero for the selected nodes. Empty: none is selected.
		std::span<uint8_t const> Selected;
		// Per node index: the node's raise counter Z (the view's, never saved). Empty: all 0.
		std::span<uint32_t const> Z;
		// Nodes drawn moved (a move preview, whose nodes and wires the index has as dynamic) and a collapsed node drawn
		// expanded under a dragged wire.
		VisualScriptNodePreview Preview;
		// The moved nodes and their wires are not hit: what lies under a dragged node (the wire it may be dropped on).
		bool SkipMoved = false;
		// ImGuiStyle::CurveTessellationTol: wires are hit where they are drawn.
		float CurveTessellationTolerance = 1.25f;
		// The index's cost of the hit test is added here when given.
		VisualScriptIndexStats* Stats = nullptr;
		VisualScriptHitScratch* Scratch = nullptr;
	};

	// What the mouse (screen pixels) is over. The first match wins: the minimap, a pin, a node's badge, a node's header, a
	// node's body (its comment's bubble included at Compact and Detail, where the bubble is drawn with its text), a reroute's
	// dot, a wire (within WireHitLogicalPixels of its drawn polyline, the nearest), a comment's edge (within
	// CommentHandleLogicalPixels), a comment's title bar, the background. Nodes are tried in reverse draw order
	// (GetVisualScriptNodeDrawKey), and a pin only on the topmost node whose rectangle (grown by PinSlopLogicalPixels) holds the
	// mouse; comments smallest first, as they are drawn largest first. At Far no pins and no wires are hit (only selecting and
	// moving work there). One index query at most 4 cells wide.
	[[nodiscard]] VisualScriptHit HitTestCanvas(VisualScriptCanvasModel const& model, VisualScriptSpatialIndex const& index,
	                                            VisualScriptView const& view, VisualScriptHitContext const& context, glm::vec2 mouse);
}
