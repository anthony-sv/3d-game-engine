#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Asset/VisualScriptSerializer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	VisualScriptNode MakeNode(uint32_t id, std::optional<glm::vec2> position = std::nullopt)
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = "Flow.Sequence";
		node.Position = position;
		return node;
	}

	VisualScriptGraph MakeGraph(std::initializer_list<uint32_t> unplaced, std::initializer_list<std::pair<uint32_t, uint32_t>> links)
	{
		VisualScriptGraph graph;
		for (uint32_t const id : unplaced)
		{
			graph.Nodes.push_back(MakeNode(id));
		}
		for (auto const& [from, to] : links)
		{
			graph.Links.push_back({{from, "Then"}, {to, "Execute"}});
		}
		return graph;
	}

	// Lays out a document holding only the graph and returns the graph.
	VisualScriptGraph Place(VisualScriptGraph graph)
	{
		VisualScriptDocument document;
		document.Graph = std::move(graph);
		PlaceUnplacedNodes(document);
		return document.Graph;
	}

	glm::vec2 GetPosition(VisualScriptGraph const& graph, uint32_t id)
	{
		auto const node = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
		                               [id](VisualScriptNode const& candidate)
		                               {
										   return candidate.ID == id;
									   });
		REQUIRE(node != graph.Nodes.end());
		REQUIRE(node->Position.has_value());
		return *node->Position;
	}

	// The layer assignment of the given nodes: entries {node, component, layer, row} in ID order.
	std::vector<VisualScriptLayerAssignment> Assign(VisualScriptGraph const& graph, std::vector<uint32_t> const& nodes)
	{
		return AssignVisualScriptLayers(graph, nodes);
	}

	std::vector<uint32_t> GetAllIDs(VisualScriptGraph const& graph)
	{
		std::vector<uint32_t> ids;
		for (VisualScriptNode const& node : graph.Nodes)
		{
			ids.push_back(node.ID);
		}
		return ids;
	}

	bool IsPlacedEverywhere(VisualScriptDocument const& document)
	{
		auto const placed = [](VisualScriptGraph const& graph)
		{
			return std::all_of(graph.Nodes.begin(), graph.Nodes.end(),
			                   [](VisualScriptNode const& node)
			                   {
								   return node.Position.has_value();
							   });
		};
		return placed(document.Graph) && std::all_of(document.Functions.begin(), document.Functions.end(),
		                                             [&placed](VisualScriptFunction const& function)
		                                             {
														 return placed(function.Graph);
													 });
	}
}

TEST_CASE("VisualScript: layout places a chain of links left to right")
{
	VisualScriptGraph const graph = Place(MakeGraph({1, 2, 3, 4}, {{1, 2}, {2, 3}, {3, 4}}));
	CHECK(GetPosition(graph, 1) == glm::vec2(0.0f, 0.0f));
	CHECK(GetPosition(graph, 2) == glm::vec2(320.0f, 0.0f));
	CHECK(GetPosition(graph, 3) == glm::vec2(640.0f, 0.0f));
	CHECK(GetPosition(graph, 4) == glm::vec2(960.0f, 0.0f));
}

TEST_CASE("VisualScript: layout stacks the nodes of a column in ID order and puts a node after its longest chain")
{
	// Fan-out: the targets share a column, ordered by ID, not by link order.
	VisualScriptGraph const fanOut = Place(MakeGraph({1, 2, 3, 4}, {{1, 4}, {1, 2}, {1, 3}}));
	CHECK(GetPosition(fanOut, 1) == glm::vec2(0.0f, 0.0f));
	CHECK(GetPosition(fanOut, 2) == glm::vec2(320.0f, 0.0f));
	CHECK(GetPosition(fanOut, 3) == glm::vec2(320.0f, 160.0f));
	CHECK(GetPosition(fanOut, 4) == glm::vec2(320.0f, 320.0f));

	// Fan-in: every source starts in the first column.
	VisualScriptGraph const fanIn = Place(MakeGraph({1, 2, 3}, {{2, 3}, {1, 3}}));
	CHECK(GetPosition(fanIn, 1) == glm::vec2(0.0f, 0.0f));
	CHECK(GetPosition(fanIn, 2) == glm::vec2(0.0f, 160.0f));
	CHECK(GetPosition(fanIn, 3) == glm::vec2(320.0f, 0.0f));

	// A shortcut does not pull a node forward: 4 follows the longest chain 1 -> 2 -> 3 -> 4. Repeated links count once.
	VisualScriptGraph const longest = Place(MakeGraph({1, 2, 3, 4}, {{1, 4}, {1, 2}, {2, 3}, {3, 4}, {3, 4}}));
	CHECK(GetPosition(longest, 3) == glm::vec2(640.0f, 0.0f));
	CHECK(GetPosition(longest, 4) == glm::vec2(960.0f, 0.0f));
}

TEST_CASE("VisualScript: layout drops the links that close cycles as a search in ID order finds them")
{
	// 1 -> 2 -> 3 -> 1: the search from 1 finds 3 -> 1 closing the cycle.
	VisualScriptGraph const cycle = Place(MakeGraph({1, 2, 3}, {{1, 2}, {2, 3}, {3, 1}}));
	CHECK(GetPosition(cycle, 1) == glm::vec2(0.0f, 0.0f));
	CHECK(GetPosition(cycle, 2) == glm::vec2(320.0f, 0.0f));
	CHECK(GetPosition(cycle, 3) == glm::vec2(640.0f, 0.0f));

	// The search starts at the smallest ID even inside a cycle: from 1 it follows 1 -> 3 -> 2 and drops 2 -> 1.
	VisualScriptGraph const inner = Place(MakeGraph({1, 2, 3}, {{2, 1}, {1, 3}, {3, 2}}));
	CHECK(GetPosition(inner, 1) == glm::vec2(0.0f, 0.0f));
	CHECK(GetPosition(inner, 3) == glm::vec2(320.0f, 0.0f));
	CHECK(GetPosition(inner, 2) == glm::vec2(640.0f, 0.0f));

	// Successors are followed in ID order: from 1 the search goes to 2 before 3, so 2 -> 3 is kept and 3 -> 2 closes the
	// cycle.
	VisualScriptGraph const order = Place(MakeGraph({1, 2, 3}, {{1, 3}, {1, 2}, {2, 3}, {3, 2}}));
	CHECK(GetPosition(order, 2) == glm::vec2(320.0f, 0.0f));
	CHECK(GetPosition(order, 3) == glm::vec2(640.0f, 0.0f));

	// A node linked to itself and a pair linked both ways.
	VisualScriptGraph const loops = Place(MakeGraph({1, 2, 3}, {{1, 1}, {2, 3}, {3, 2}}));
	CHECK(GetPosition(loops, 1) == glm::vec2(0.0f, 0.0f));
	CHECK(GetPosition(loops, 2) == glm::vec2(0.0f, 240.0f));
	CHECK(GetPosition(loops, 3) == glm::vec2(320.0f, 240.0f));
}

TEST_CASE("VisualScript: layout puts each group of linked nodes below the previous one, in the order of their smallest IDs")
{
	// Groups {1, 5}, {2, 3, 6} and {4}.
	VisualScriptGraph const graph = Place(MakeGraph({6, 5, 4, 3, 2, 1}, {{5, 1}, {2, 3}, {2, 6}}));
	CHECK(GetPosition(graph, 5) == glm::vec2(0.0f, 0.0f));
	CHECK(GetPosition(graph, 1) == glm::vec2(320.0f, 0.0f));
	// The next group starts 240 below the lowest node of the previous one.
	CHECK(GetPosition(graph, 2) == glm::vec2(0.0f, 240.0f));
	CHECK(GetPosition(graph, 3) == glm::vec2(320.0f, 240.0f));
	CHECK(GetPosition(graph, 6) == glm::vec2(320.0f, 400.0f));
	CHECK(GetPosition(graph, 4) == glm::vec2(0.0f, 640.0f));
}

TEST_CASE("VisualScript: layout keeps placed nodes where they are and starts below them at their left edge")
{
	VisualScriptGraph graph = MakeGraph({3, 4}, {{3, 4}});
	graph.Nodes.push_back(MakeNode(1, glm::vec2(100.0f, 50.0f)));
	graph.Nodes.push_back(MakeNode(2, glm::vec2(-200.0f, 400.0f)));
	// Links with placed nodes do not count: 4 stays behind 3 only.
	graph.Links.push_back({{1, "Then"}, {4, "Execute"}});
	graph.Links.push_back({{4, "Then"}, {2, "Execute"}});
	VisualScriptGraph const placed = Place(graph);
	CHECK(GetPosition(placed, 1) == glm::vec2(100.0f, 50.0f));
	CHECK(GetPosition(placed, 2) == glm::vec2(-200.0f, 400.0f));
	CHECK(GetPosition(placed, 3) == glm::vec2(-200.0f, 640.0f));
	CHECK(GetPosition(placed, 4) == glm::vec2(120.0f, 640.0f));
	CHECK(placed.Links == graph.Links);

	// Nothing to place: nothing changes.
	VisualScriptGraph const again = Place(placed);
	CHECK(again == placed);
}

TEST_CASE("VisualScript: layout lays out every graph on its own")
{
	VisualScriptDocument document;
	document.Graph = MakeGraph({2}, {});
	document.Graph.Nodes.push_back(MakeNode(1, glm::vec2(5000.0f, 5000.0f)));
	VisualScriptFunction function;
	function.ID = 10;
	function.Name = "F";
	function.Graph = MakeGraph({11, 12}, {{11, 12}});
	document.Functions.push_back(function);
	function.ID = 20;
	function.Name = "G";
	function.Graph = MakeGraph({21}, {});
	function.Graph.Nodes.push_back(MakeNode(22, glm::vec2(-80.0f, -40.0f)));
	document.Functions.push_back(function);

	PlaceUnplacedNodes(document);
	CHECK(IsPlacedEverywhere(document));
	CHECK(GetPosition(document.Graph, 2) == glm::vec2(5000.0f, 5240.0f));
	CHECK(GetPosition(document.Functions[0].Graph, 11) == glm::vec2(0.0f, 0.0f));
	CHECK(GetPosition(document.Functions[0].Graph, 12) == glm::vec2(320.0f, 0.0f));
	CHECK(GetPosition(document.Functions[1].Graph, 21) == glm::vec2(-80.0f, 200.0f));
	CHECK(GetPosition(document.Functions[1].Graph, 22) == glm::vec2(-80.0f, -40.0f));
}

TEST_CASE("VisualScript: layout is deterministic and does not depend on the order of nodes and links")
{
	VisualScriptGraph const graph = MakeGraph({1, 2, 3, 4, 5, 6, 7, 8}, {{1, 2}, {2, 3}, {3, 1}, {1, 4}, {5, 6}, {6, 6}, {7, 5}, {2, 4}});
	VisualScriptGraph reversed = graph;
	std::reverse(reversed.Nodes.begin(), reversed.Nodes.end());
	std::reverse(reversed.Links.begin(), reversed.Links.end());

	VisualScriptGraph const first = Place(graph);
	VisualScriptGraph const second = Place(graph);
	VisualScriptGraph const fromReversed = Place(reversed);
	CHECK(first == second);
	for (VisualScriptNode const& node : first.Nodes)
	{
		CAPTURE(node.ID);
		CHECK(GetPosition(fromReversed, node.ID) == *node.Position);
	}
}

TEST_CASE("VisualScript: layer assignment puts chains in successive layers and fans in one layer, rows in ID order")
{
	VisualScriptGraph const chain = MakeGraph({3, 1, 2}, {{1, 2}, {2, 3}});
	CHECK(Assign(chain, {1, 2, 3}) == std::vector<VisualScriptLayerAssignment>{{1, 0, 0, 0}, {2, 0, 1, 0}, {3, 0, 2, 0}});

	// Fan-out: the targets share a layer, ordered by ID, not by link order.
	VisualScriptGraph const fanOut = MakeGraph({1, 2, 3, 4}, {{1, 4}, {1, 2}, {1, 3}});
	CHECK(Assign(fanOut, {1, 2, 3, 4}) == std::vector<VisualScriptLayerAssignment>{{1, 0, 0, 0}, {2, 0, 1, 0}, {3, 0, 1, 1}, {4, 0, 1, 2}});

	// Fan-in: every source is in the first layer; a node follows its longest chain, whatever shortcuts lead to it.
	VisualScriptGraph const fanIn = MakeGraph({1, 2, 3, 4}, {{2, 4}, {1, 3}, {3, 4}, {1, 4}});
	CHECK(Assign(fanIn, {1, 2, 3, 4}) == std::vector<VisualScriptLayerAssignment>{{1, 0, 0, 0}, {2, 0, 0, 1}, {3, 0, 1, 0}, {4, 0, 2, 0}});
}

TEST_CASE("VisualScript: layer assignment drops the links that close cycles and orders groups by their smallest IDs")
{
	// 1 -> 2 -> 3 -> 1: the search from 1 finds 3 -> 1 closing the cycle. A node linked to itself stays in the first layer.
	VisualScriptGraph const cycle = MakeGraph({1, 2, 3, 4}, {{1, 2}, {2, 3}, {3, 1}, {4, 4}});
	CHECK(Assign(cycle, {1, 2, 3, 4}) == std::vector<VisualScriptLayerAssignment>{{1, 0, 0, 0}, {2, 0, 1, 0}, {3, 0, 2, 0}, {4, 1, 0, 0}});

	// Groups {1, 5}, {2, 3, 6} and {4}; rows count within a group's layer.
	VisualScriptGraph const groups = MakeGraph({6, 5, 4, 3, 2, 1}, {{5, 1}, {2, 3}, {2, 6}});
	CHECK(Assign(groups, {1, 2, 3, 4, 5, 6}) ==
	      std::vector<VisualScriptLayerAssignment>{{1, 0, 1, 0}, {2, 1, 0, 0}, {3, 1, 1, 0}, {4, 2, 0, 0}, {5, 0, 0, 0}, {6, 1, 1, 1}});
}

TEST_CASE("VisualScript: layer assignment counts only the links between the given nodes, placed or not")
{
	// 1 -> 2 -> 3 -> 4 with 1 and 4 placed: without 2, the given nodes 1, 3 and 4 form two groups.
	VisualScriptGraph graph = MakeGraph({2, 3}, {{1, 2}, {2, 3}, {3, 4}});
	graph.Nodes.push_back(MakeNode(1, glm::vec2(500.0f, 500.0f)));
	graph.Nodes.push_back(MakeNode(4, glm::vec2(-500.0f, 0.0f)));
	CHECK(Assign(graph, {4, 3, 1}) == std::vector<VisualScriptLayerAssignment>{{1, 0, 0, 0}, {3, 1, 0, 0}, {4, 1, 1, 0}});

	// IDs of no node and repeated IDs are left out; no node, no entry.
	CHECK(Assign(graph, {3, 9, 3, 0, 2}) == std::vector<VisualScriptLayerAssignment>{{2, 0, 0, 0}, {3, 0, 1, 0}});
	CHECK(Assign(graph, {}).empty());
	CHECK(Assign(VisualScriptGraph{}, {1, 2}).empty());
}

TEST_CASE("VisualScript: layer assignment is deterministic and does not depend on the order of nodes, links and given IDs")
{
	VisualScriptGraph const graph = MakeGraph({1, 2, 3, 4, 5, 6, 7, 8}, {{1, 2}, {2, 3}, {3, 1}, {1, 4}, {5, 6}, {6, 6}, {7, 5}, {2, 4}});
	VisualScriptGraph reversed = graph;
	std::reverse(reversed.Nodes.begin(), reversed.Nodes.end());
	std::reverse(reversed.Links.begin(), reversed.Links.end());
	std::vector<uint32_t> ids = GetAllIDs(graph);
	std::vector<VisualScriptLayerAssignment> const first = Assign(graph, ids);
	CHECK(first.size() == 8);
	CHECK(Assign(graph, ids) == first);
	std::reverse(ids.begin(), ids.end());
	CHECK(Assign(reversed, ids) == first);
}

TEST_CASE("VisualScript: layout places the unplaced nodes at their layer assignment")
{
	// Groups {1, 2, 3, 4} (a fan into a chain) and {5, 6}, below a placed node at (-100, 40).
	VisualScriptGraph graph = MakeGraph({1, 2, 3, 4, 5, 6}, {{1, 3}, {2, 3}, {3, 4}, {1, 4}, {6, 5}});
	graph.Nodes.push_back(MakeNode(7, glm::vec2(-100.0f, 40.0f)));
	std::vector<uint32_t> unplaced = GetAllIDs(graph);
	unplaced.pop_back();
	std::vector<VisualScriptLayerAssignment> const assignments = Assign(graph, unplaced);
	VisualScriptGraph const placed = Place(graph);

	// Each group starts 240 below the lowest row of the one before it, the first 240 below the placed node.
	std::vector<float> tops = {280.0f};
	for (VisualScriptLayerAssignment const& assignment : assignments)
	{
		CAPTURE(assignment.Node);
		if (assignment.Component == tops.size())
		{
			float lastRow = 0.0f;
			for (VisualScriptLayerAssignment const& other : assignments)
			{
				lastRow = other.Component + 1 == tops.size() ? std::max(lastRow, static_cast<float>(other.Row)) : lastRow;
			}
			tops.push_back(tops.back() + lastRow * 160.0f + 240.0f);
		}
		CHECK(GetPosition(placed, assignment.Node) ==
		      glm::vec2(-100.0f + static_cast<float>(assignment.Layer) * 320.0f,
		                tops.at(assignment.Component) + static_cast<float>(assignment.Row) * 160.0f));
	}
	CHECK(tops.size() == 2);
}

TEST_CASE("VisualScript: laid out documents load without issues, also when the layout reaches the edge of the canvas")
{
	// A long chain (the search keeps its own stack) below a node placed near the canvas's bottom edge.
	VisualScriptGraph graph;
	for (uint32_t id = 1; id <= 5000; id++)
	{
		graph.Nodes.push_back(MakeNode(id));
		if (id > 1)
		{
			graph.Links.push_back({{id - 1, "Then"}, {id, "Execute"}});
		}
	}
	graph.Nodes.push_back(MakeNode(6000, glm::vec2(0.0f, 9999900.0f)));
	graph.Nodes.push_back(MakeNode(6001));
	VisualScriptDocument document;
	document.Graph = graph;
	PlaceUnplacedNodes(document);
	CHECK(IsPlacedEverywhere(document));
	CHECK(GetPosition(document.Graph, 5000) == glm::vec2(4999.0f * 320.0f, static_cast<float>(VisualScriptSerializer::MaxCoordinate)));
	CHECK(GetPosition(document.Graph, 6001) == glm::vec2(0.0f, static_cast<float>(VisualScriptSerializer::MaxCoordinate)));

	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> loaded =
		VisualScriptSerializer::DeserializeDocument(VisualScriptSerializer::SerializeDocument(document), DeserializationContext{}, &issues);
	REQUIRE(loaded.IsOk());
	CHECK(issues.empty());
	CHECK(loaded.GetValue() == document);
}
