#pragma once

#include "Strada/Asset/VisualScriptAsset.h"

namespace Strada
{
	// Gives every unplaced node of every graph a position; placed nodes never move. In each graph the unplaced nodes form
	// groups connected by their links, laid out one below the other under the placed nodes and starting at their left
	// edge; within a group, links run left to right through columns 320 apart (a node's column is the longest chain of
	// links leading to it, ignoring links that close a cycle), and the nodes of a column stand 160 apart in ID order.
	// Deterministic and pure: the result depends only on the IDs, positions and links of the nodes, not on their order.
	void PlaceUnplacedNodes(VisualScriptDocument& document);
}
