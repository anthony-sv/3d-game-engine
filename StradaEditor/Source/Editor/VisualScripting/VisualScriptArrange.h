#pragma once

#include "Editor/VisualScripting/VisualScriptEditorTypes.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Result.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>

// The Arrange operation of the visual script editor: a layered layout of nodes that keeps links running left to right with
// few crossings. Pure, like the other edits (VisualScriptEditing.h); no ImGui, any thread.
namespace Strada::VisualScriptEditing
{
	// Lays the given nodes of a graph (0 for the event graph, else a function's ID) out on their own; no nodes means every
	// node of the graph. Only the links between the given nodes count: they run left to right through columns (a node's
	// column is the longest chain of links leading to it, ignoring links that close a cycle, as PlaceUnplacedNodes assigns
	// them), and the order of the nodes in each column is chosen for few crossings between neighbouring columns. Groups of
	// nodes connected by links stand one below the other. Columns are as wide as their widest node plus 64, nodes stand 32
	// apart in a column and groups 112 apart, with every column and row starting on the 16 grid; with
	// GetDefaultVisualScriptNodeExtents this gives PlaceUnplacedNodes' 320, 160 and 240. Nodes without a position take part
	// and get one.
	//
	// A comment box that holds no node but arranged ones moves with them when it holds one, stands in such a box, or when every
	// node of the graph is arranged. What it holds (the nodes and boxes entirely inside it) moves as one block: it is laid out
	// on its own, and the block, with the box fitted around it as a new comment box framing it would be, takes part in the
	// layout as one node whose links are its nodes' links to the others. The boxes inside it are blocks of that layout in
	// turn, so boxes nest as they nested. A box that holds nothing keeps its size and stands in the layout as a node without
	// links, after the nodes. Boxes that overlap without one holding the other and hold a node or a box in common form one
	// block, laid out together. So of the boxes that move, those that did not overlap before arranging do not overlap after
	// it, a box that held another holds it still, and each frames the nodes it framed; one that overlapped no box but those it
	// held or stood in holds no other node.
	//
	// Where the arranged block stands:
	//  - by default, the top-left corner of its nodes is that of the nodes that had a position, (0, 0) when none had (a
	//    box's frame reaches above and left of its nodes);
	//  - with corner, the top-left corner of its nodes is the corner;
	//  - with anchors (placed nodes of the graph that are not arranged, which keep their places), the anchors stand as a
	//    column before each group's first column: the links from them into that column count for the order of its nodes
	//    (against the anchors' rows: top to bottom, then left to right, then by ID), those into other groups do not, and the
	//    block's first column (frames included) starts at the anchors' right edge plus 64, its first row at the anchors'
	//    top, both rounded up to the 16 grid.
	// corner and anchors are alternatives.
	//
	// Deterministic: where the nodes go depends on their IDs, links and sizes, on the comment boxes they stand in and on the
	// corner or the anchors, not on where else they stood or the order of anything. Fails, changing nothing, for an ID that
	// names no node of the graph, for an anchor that is arranged too or has no position, for a corner that is not finite or
	// given with anchors, and for a size that is not one (extents measures the arranged nodes, the anchors and the other
	// nodes that stand in comment boxes).
	[[nodiscard]] Result<void> ArrangeNodes(VisualScriptDocument& document, uint32_t function, std::span<uint32_t const> nodes,
	                                        VisualScriptNodeExtents const& extents, std::optional<glm::vec2> corner = std::nullopt,
	                                        std::span<uint32_t const> anchors = {});

	// The size of every node for callers without a canvas to measure it (automation, tests): 256 × 128, which makes
	// ArrangeNodes reproduce the pitches of PlaceUnplacedNodes.
	[[nodiscard]] glm::vec2 GetDefaultVisualScriptNodeExtents(uint32_t node);
}
