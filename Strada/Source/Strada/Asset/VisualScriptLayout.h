#pragma once

#include "Strada/Asset/VisualScriptAsset.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Strada
{
	// Where the layered layout puts a node before it computes coordinates.
	struct VisualScriptLayerAssignment
	{
		uint32_t Node = 0;
		// The node's group of nodes connected by links (whatever their direction): its index in the order of the groups'
		// smallest IDs.
		uint32_t Component = 0;
		// The length of the longest chain of links leading to the node, without the links that close a cycle.
		uint32_t Layer = 0;
		// The node's index among the nodes of its layer and group, in ID order.
		uint32_t Row = 0;

		bool operator==(VisualScriptLayerAssignment const& other) const = default;
	};

	// Gives every unplaced node of every graph a position; placed nodes never move. In each graph the unplaced nodes form
	// groups connected by their links, laid out one below the other under the placed nodes and starting at their left
	// edge; within a group, links run left to right through columns 320 apart (a node's column is the longest chain of
	// links leading to it, ignoring links that close a cycle), and the nodes of a column stand 160 apart in ID order.
	// Deterministic and pure: the result depends only on the IDs, positions and links of the nodes, not on their order.
	void PlaceUnplacedNodes(VisualScriptDocument& document);

	// Assigns the given nodes of a graph to groups, layers and rows as PlaceUnplacedNodes assigns the unplaced nodes it
	// places (Layer is the column, Row the index in it). Only the links between given nodes count; the nodes' positions do
	// not matter. A link that closes a cycle is one a depth-first search finds back to a node it is still searching, the
	// search starting at each node in ID order and following links to nodes in ID order. Returns one entry per given ID of
	// a node of the graph (the first node with it, should a graph built in memory repeat an ID), in ID order; repeated IDs
	// and IDs of no node are left out. Deterministic and pure: the order of the given IDs, of the links and of nodes with
	// different IDs does not matter.
	[[nodiscard]] std::vector<VisualScriptLayerAssignment> AssignVisualScriptLayers(VisualScriptGraph const& graph,
	                                                                                std::span<uint32_t const> nodes);
}
