#pragma once

#include "Editor/VisualScripting/VisualScriptEditorTypes.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Result.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

// The Arrange operation of the visual script editor: a layered layout of nodes that keeps links running left to right with
// few crossings. Pure, like the other edits (VisualScriptEditing.h); no ImGui, any thread.
namespace Strada::VisualScriptEditing
{
	// Lays the given nodes of a graph (0 for the event graph, else a function's ID) out on their own, keeping the top-left
	// corner of their bounds; no nodes means every node of the graph. Only the links between the given nodes count: they
	// run left to right through columns (a node's column is the longest chain of links leading to it, ignoring links that
	// close a cycle, as PlaceUnplacedNodes assigns them), and the order of the nodes in each column is chosen for few
	// crossings between neighbouring columns. Groups of nodes connected by links stand one below the other. Columns are as
	// wide as their widest node plus 64, nodes stand 32 apart in a column and groups 112 apart, with every column and row
	// starting on the 16 grid; with GetDefaultVisualScriptNodeExtents this gives PlaceUnplacedNodes' 320, 160 and 240.
	// Nodes without a position take part and get one; the corner kept is that of the nodes that had one, (0, 0) when none
	// had. A comment box whose contents (the nodes and comments entirely inside it) are all arranged nodes is fitted around
	// them again, as a new comment box framing them would be. Deterministic: where the nodes go depends on their IDs,
	// links and sizes and on the corner, not on where they stood or the order of anything. Fails, changing nothing, for an
	// ID that names no node of the graph and for a size that is not one (extents measures the arranged nodes, and the
	// other nodes that stand in comment boxes).
	[[nodiscard]] Result<void> ArrangeNodes(VisualScriptDocument& document, uint32_t function, std::span<uint32_t const> nodes,
	                                        VisualScriptNodeExtents const& extents);

	// The size of every node for callers without a canvas to measure it (automation, tests): 256 × 128, which makes
	// ArrangeNodes reproduce the pitches of PlaceUnplacedNodes.
	[[nodiscard]] glm::vec2 GetDefaultVisualScriptNodeExtents(uint32_t node);
}
