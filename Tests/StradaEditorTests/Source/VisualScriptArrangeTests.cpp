#include "VisualScriptTestDocuments.h"

#include "Editor/VisualScripting/VisualScriptArrange.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptLayout.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
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
		node.Type = "Flow.Branch";
		node.Position = position;
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, uint32_t to)
	{
		return {{from, "Then"}, {to, "Execute"}};
	}

	// A graph of unplaced nodes whose ID order has no crossings: a chain that branches in two (1 → 2 → 3, 4; 3 → 5;
	// 4 → 6), a pair (7 → 8) and a node on its own (9).
	VisualScriptDocument MakeBranchingDocument()
	{
		VisualScriptDocument document;
		for (uint32_t id = 1; id <= 9; id++)
		{
			document.Graph.Nodes.push_back(MakeNode(id));
		}
		document.Graph.Links = {MakeLink(1, 2), MakeLink(2, 3), MakeLink(2, 4), MakeLink(3, 5), MakeLink(4, 6), MakeLink(7, 8)};
		return document;
	}

	std::map<uint32_t, glm::vec2> GetPositions(VisualScriptGraph const& graph)
	{
		std::map<uint32_t, glm::vec2> positions;
		for (VisualScriptNode const& node : graph.Nodes)
		{
			REQUIRE(node.Position.has_value());
			positions.emplace(node.ID, *node.Position);
		}
		return positions;
	}

	glm::vec2 GetCorner(VisualScriptGraph const& graph)
	{
		glm::vec2 corner(std::numeric_limits<float>::max());
		for (VisualScriptNode const& node : graph.Nodes)
		{
			REQUIRE(node.Position.has_value());
			corner = glm::min(corner, *node.Position);
		}
		return corner;
	}

	// The pairs of links that cross, as the canvas draws them: links between the same two columns whose ends swap their
	// order from one column to the other.
	int CountCrossings(VisualScriptGraph const& graph)
	{
		std::map<uint32_t, glm::vec2> const positions = GetPositions(graph);
		int crossings = 0;
		for (size_t first = 0; first < graph.Links.size(); first++)
		{
			for (size_t second = first + 1; second < graph.Links.size(); second++)
			{
				glm::vec2 const a = positions.at(graph.Links[first].From.Node);
				glm::vec2 const b = positions.at(graph.Links[first].To.Node);
				glm::vec2 const c = positions.at(graph.Links[second].From.Node);
				glm::vec2 const d = positions.at(graph.Links[second].To.Node);
				if (a.x == c.x && b.x == d.x && (a.y - c.y) * (b.y - d.y) < 0.0f)
				{
					crossings++;
				}
			}
		}
		return crossings;
	}

	VisualScriptNodeExtents const DefaultExtents = VisualScriptEditing::GetDefaultVisualScriptNodeExtents;
}

TEST_CASE("VisualScriptArrange: default extents lay out a graph without crossings as placing its unplaced nodes does")
{
	VisualScriptDocument const unplaced = MakeBranchingDocument();
	VisualScriptDocument expected = unplaced;
	PlaceUnplacedNodes(expected);
	std::map<uint32_t, glm::vec2> const placed = GetPositions(expected.Graph);
	CHECK(placed.at(1) == glm::vec2(0.0f, 0.0f));
	CHECK(placed.at(4) == glm::vec2(640.0f, 160.0f));
	CHECK(placed.at(7) == glm::vec2(0.0f, 400.0f));
	CHECK(placed.at(9) == glm::vec2(0.0f, 640.0f));
	CHECK(CountCrossings(expected.Graph) == 0);
	CHECK(glm::vec2(256.0f, 128.0f) == VisualScriptEditing::GetDefaultVisualScriptNodeExtents(1));

	SUBCASE("unplaced nodes, whose corner is (0, 0)")
	{
		VisualScriptDocument document = unplaced;
		REQUIRE(VisualScriptEditing::ArrangeNodes(document, 0, {}, DefaultExtents).IsOk());
		CHECK(GetPositions(document.Graph) == placed);
		Testing::ExpectStrictRoundTrip(document);
	}
	SUBCASE("placed nodes anywhere, with one at (0, 0)")
	{
		VisualScriptDocument document = unplaced;
		for (VisualScriptNode& node : document.Graph.Nodes)
		{
			node.Position = glm::vec2(static_cast<float>((node.ID * 7919) % 1000), static_cast<float>((node.ID * 104729) % 700));
		}
		document.Graph.Nodes[4].Position = glm::vec2(0.0f, 0.0f);
		REQUIRE(VisualScriptEditing::ArrangeNodes(document, 0, {}, DefaultExtents).IsOk());
		CHECK(GetPositions(document.Graph) == placed);
	}
}

TEST_CASE("VisualScriptArrange: nodes in each column are ordered for fewer crossings than ID order")
{
	SUBCASE("a fan whose ID order crosses every link")
	{
		// 1 → 2, 3, 4; then 2 → 7, 3 → 6, 4 → 5, which ID order stacks upside down.
		VisualScriptDocument document;
		for (uint32_t id = 1; id <= 7; id++)
		{
			document.Graph.Nodes.push_back(MakeNode(id));
		}
		document.Graph.Links = {MakeLink(1, 2), MakeLink(1, 3), MakeLink(1, 4), MakeLink(2, 7), MakeLink(3, 6), MakeLink(4, 5)};
		VisualScriptDocument placed = document;
		PlaceUnplacedNodes(placed);
		CHECK(CountCrossings(placed.Graph) == 3);

		REQUIRE(VisualScriptEditing::ArrangeNodes(document, 0, {}, DefaultExtents).IsOk());
		CHECK(CountCrossings(document.Graph) == 0);
		std::map<uint32_t, glm::vec2> const positions = GetPositions(document.Graph);
		CHECK(positions.at(1) == glm::vec2(0.0f, 0.0f));
		CHECK(positions.at(2) == glm::vec2(320.0f, 0.0f));
		CHECK(positions.at(4) == glm::vec2(320.0f, 320.0f));
		CHECK(positions.at(7) == glm::vec2(640.0f, 0.0f));
		CHECK(positions.at(6) == glm::vec2(640.0f, 160.0f));
		CHECK(positions.at(5) == glm::vec2(640.0f, 320.0f));
	}
	SUBCASE("four columns, where the order of one column decides the next")
	{
		// The fan above, and 5 → 8, 6 → 9, 7 → 10, which ID order does not cross until column 2 turns over.
		VisualScriptDocument document;
		for (uint32_t id = 1; id <= 10; id++)
		{
			document.Graph.Nodes.push_back(MakeNode(id));
		}
		document.Graph.Links = {MakeLink(1, 2), MakeLink(1, 3), MakeLink(1, 4), MakeLink(2, 7), MakeLink(3, 6),
		                        MakeLink(4, 5), MakeLink(5, 8), MakeLink(6, 9), MakeLink(7, 10)};
		VisualScriptDocument placed = document;
		PlaceUnplacedNodes(placed);
		CHECK(CountCrossings(placed.Graph) == 3);
		REQUIRE(VisualScriptEditing::ArrangeNodes(document, 0, {}, DefaultExtents).IsOk());
		CHECK(CountCrossings(document.Graph) == 0);
		std::map<uint32_t, glm::vec2> const positions = GetPositions(document.Graph);
		CHECK(positions.at(10) == glm::vec2(960.0f, 0.0f));
		CHECK(positions.at(8) == glm::vec2(960.0f, 320.0f));
	}
	SUBCASE("a denser graph never crosses more than ID order")
	{
		VisualScriptDocument document;
		for (uint32_t id = 1; id <= 24; id++)
		{
			document.Graph.Nodes.push_back(MakeNode(id));
		}
		// Each node of a column links to nodes of the next one, in a scrambled order.
		for (uint32_t column = 0; column < 3; column++)
		{
			for (uint32_t row = 0; row < 6; row++)
			{
				uint32_t const from = column * 6 + row + 1;
				document.Graph.Links.push_back({{from, "A"}, {(column + 1) * 6 + (row * 5 + 3) % 6 + 1, "In"}});
				document.Graph.Links.push_back({{from, "B"}, {(column + 1) * 6 + (row * 7 + 1) % 6 + 1, "In"}});
			}
		}
		VisualScriptDocument placed = document;
		PlaceUnplacedNodes(placed);
		int const before = CountCrossings(placed.Graph);
		REQUIRE(VisualScriptEditing::ArrangeNodes(document, 0, {}, DefaultExtents).IsOk());
		int const after = CountCrossings(document.Graph);
		CAPTURE(before);
		CAPTURE(after);
		CHECK(after < before);
	}
}

TEST_CASE("VisualScriptArrange: columns and rows follow the measured sizes on the 16 grid")
{
	VisualScriptDocument document;
	for (uint32_t id = 1; id <= 4; id++)
	{
		document.Graph.Nodes.push_back(MakeNode(id, glm::vec2(100.0f, 50.0f)));
	}
	document.Graph.Nodes.push_back(MakeNode(5, glm::vec2(900.0f, 900.0f)));
	document.Graph.Links = {MakeLink(1, 3), {{1, "Value"}, {4, "A"}}, MakeLink(2, 4)};
	VisualScriptNodeExtents const extents = [](uint32_t node)
	{
		switch (node)
		{
			case 1:
				return glm::vec2(300.0f, 100.0f);
			case 2:
				return glm::vec2(180.0f, 51.0f);
			case 3:
				return glm::vec2(90.0f, 40.0f);
			default:
				return glm::vec2(200.0f, 70.0f);
		}
	};
	REQUIRE(VisualScriptEditing::ArrangeNodes(document, 0, {}, extents).IsOk());
	std::map<uint32_t, glm::vec2> const positions = GetPositions(document.Graph);
	// Column 1 starts after the widest node of column 0 (300) and 64, rounded up to 16 (368). Node 2 stands below node 1
	// (100) with 32 between them (144, on the grid); node 4 below node 3 (40 + 32 = 72, rounded up to 80).
	CHECK(positions.at(1) == glm::vec2(100.0f, 50.0f));
	CHECK(positions.at(2) == glm::vec2(100.0f, 194.0f));
	CHECK(positions.at(3) == glm::vec2(468.0f, 50.0f));
	CHECK(positions.at(4) == glm::vec2(468.0f, 130.0f));
	// The node on its own is a group of its own, 112 below the bottom of the first (144 + 51 = 195), rounded up to 320.
	CHECK(positions.at(5) == glm::vec2(100.0f, 370.0f));
}

TEST_CASE("VisualScriptArrange: the corner of the arranged nodes' bounds stays where it was")
{
	VisualScriptDocument document = MakeBranchingDocument();
	for (VisualScriptNode& node : document.Graph.Nodes)
	{
		node.Position = glm::vec2(-1234.5f + static_cast<float>(node.ID) * 40.0f, 777.25f - static_cast<float>(node.ID % 3) * 10.0f);
	}
	glm::vec2 const corner = GetCorner(document.Graph);
	CHECK(corner == glm::vec2(-1194.5f, 757.25f));
	REQUIRE(VisualScriptEditing::ArrangeNodes(document, 0, {}, DefaultExtents).IsOk());
	CHECK(GetCorner(document.Graph) == corner);
	std::map<uint32_t, glm::vec2> const positions = GetPositions(document.Graph);
	CHECK(positions.at(1) == corner);
	CHECK(positions.at(5) == corner + glm::vec2(960.0f, 0.0f));

	SUBCASE("nodes without a position take part, and the corner is that of the placed ones")
	{
		VisualScriptDocument mixed = MakeBranchingDocument();
		mixed.Graph.Nodes[3].Position = glm::vec2(50.0f, 60.0f);
		mixed.Graph.Nodes[7].Position = glm::vec2(30.0f, 90.0f);
		REQUIRE(VisualScriptEditing::ArrangeNodes(mixed, 0, {}, DefaultExtents).IsOk());
		CHECK(GetCorner(mixed.Graph) == glm::vec2(30.0f, 60.0f));
		Testing::ExpectStrictRoundTrip(mixed);
	}
}

TEST_CASE("VisualScriptArrange: arranging a selection moves nothing else, and only links between its nodes count")
{
	VisualScriptDocument document = MakeBranchingDocument();
	PlaceUnplacedNodes(document);
	// Scramble the selection: 2, 3 and 4.
	document.Graph.Nodes[1].Position = glm::vec2(2000.0f, 48.0f);
	document.Graph.Nodes[2].Position = glm::vec2(1500.0f, 32.0f);
	document.Graph.Nodes[3].Position = glm::vec2(1600.0f, 16.0f);
	VisualScriptComment comment;
	comment.ID = 20;
	comment.Position = glm::vec2(-100.0f, -100.0f);
	comment.Size = glm::vec2(5000.0f, 5000.0f);
	document.Graph.Comments.push_back(comment);
	VisualScriptDocument const before = document;

	std::vector<uint32_t> const selection = {4, 3, 2, 3};
	REQUIRE(VisualScriptEditing::ArrangeNodes(document, 0, selection, DefaultExtents).IsOk());
	std::map<uint32_t, glm::vec2> const positions = GetPositions(document.Graph);
	std::map<uint32_t, glm::vec2> const old = GetPositions(before.Graph);
	for (uint32_t const id : {1u, 5u, 6u, 7u, 8u, 9u})
	{
		CHECK(positions.at(id) == old.at(id));
	}
	// The selection's corner was (1500, 16): 2 links to 3 and 4, which stand in the next column.
	CHECK(positions.at(2) == glm::vec2(1500.0f, 16.0f));
	CHECK(positions.at(3) == glm::vec2(1820.0f, 16.0f));
	CHECK(positions.at(4) == glm::vec2(1820.0f, 176.0f));
	// The comment holds nodes that were not arranged: it stays.
	CHECK(document.Graph.Comments == before.Graph.Comments);
	CHECK(document.Graph.Links == before.Graph.Links);
	Testing::ExpectStrictRoundTrip(document);
}

TEST_CASE("VisualScriptArrange: arranging twice changes nothing the second time, and the result does not depend on order")
{
	VisualScriptDocument document;
	for (uint32_t id = 1; id <= 12; id++)
	{
		document.Graph.Nodes.push_back(
			MakeNode(id, glm::vec2(static_cast<float>(id * 37 % 11) * 90.0f, static_cast<float>(id * 13 % 7) * 70.0f)));
	}
	document.Graph.Links = {MakeLink(1, 5), MakeLink(2, 4), MakeLink(3, 6),   MakeLink(4, 8),   MakeLink(5, 7),
	                        MakeLink(6, 9), MakeLink(9, 1), MakeLink(10, 11), MakeLink(12, 11), MakeLink(8, 3)};
	VisualScriptNodeExtents const extents = [](uint32_t node)
	{
		return glm::vec2(150.0f + static_cast<float>(node % 4) * 45.0f, 60.0f + static_cast<float>(node % 3) * 33.0f);
	};

	VisualScriptDocument once = document;
	REQUIRE(VisualScriptEditing::ArrangeNodes(once, 0, {}, extents).IsOk());
	VisualScriptDocument twice = once;
	REQUIRE(VisualScriptEditing::ArrangeNodes(twice, 0, {}, extents).IsOk());
	CHECK(twice == once);

	// The same graph with its nodes and links in another order, and the nodes elsewhere around the same corner.
	VisualScriptDocument shuffled = document;
	std::reverse(shuffled.Graph.Nodes.begin(), shuffled.Graph.Nodes.end());
	std::rotate(shuffled.Graph.Links.begin(), shuffled.Graph.Links.begin() + 4, shuffled.Graph.Links.end());
	for (VisualScriptNode& node : shuffled.Graph.Nodes)
	{
		if (node.Position->x > 0.0f && node.Position->y > 0.0f)
		{
			*node.Position += glm::vec2(31.0f, 17.0f);
		}
	}
	REQUIRE(GetCorner(shuffled.Graph) == GetCorner(document.Graph));
	REQUIRE(VisualScriptEditing::ArrangeNodes(shuffled, 0, {}, extents).IsOk());
	CHECK(GetPositions(shuffled.Graph) == GetPositions(once.Graph));
}

TEST_CASE("VisualScriptArrange: a comment box holding only arranged nodes is fitted around them again")
{
	VisualScriptDocument document = MakeBranchingDocument();
	PlaceUnplacedNodes(document);
	VisualScriptComment around;
	around.ID = 20;
	around.Text = "Chain";
	around.Position = glm::vec2(-50.0f, -50.0f);
	around.Size = glm::vec2(1300.0f, 400.0f);
	VisualScriptComment empty;
	empty.ID = 21;
	empty.Position = glm::vec2(5000.0f, 5000.0f);
	VisualScriptComment outer;
	outer.ID = 22;
	outer.Position = glm::vec2(-60.0f, -60.0f);
	outer.Size = glm::vec2(1400.0f, 420.0f);
	VisualScriptComment mixed;
	mixed.ID = 23;
	mixed.Position = glm::vec2(-50.0f, 350.0f);
	mixed.Size = glm::vec2(1000.0f, 450.0f);
	document.Graph.Comments = {around, empty, outer, mixed};

	std::vector<uint32_t> const chain = {1, 2, 3, 4, 5, 6};
	REQUIRE(VisualScriptEditing::ArrangeNodes(document, 0, chain, DefaultExtents).IsOk());
	// The chain's bounds: (0, 0) to (960 + 256, 160 + 128); the frame adds 32 on each side and 40 more above.
	VisualScriptComment const& refit = document.Graph.Comments[0];
	CHECK(refit.Position == glm::vec2(-32.0f, -72.0f));
	CHECK(refit.Size == glm::vec2(1280.0f, 392.0f));
	CHECK(refit.Text == "Chain");
	// Empty, holding another comment box, or holding a node not arranged (7 and 9): they stay.
	CHECK(document.Graph.Comments[1] == empty);
	CHECK(document.Graph.Comments[2] == outer);
	CHECK(document.Graph.Comments[3] == mixed);
	Testing::ExpectStrictRoundTrip(document);
}

TEST_CASE("VisualScriptArrange: a function's graph is arranged, and bad arguments change nothing")
{
	VisualScriptDocument document;
	VisualScriptFunction function;
	function.ID = 30;
	function.Name = "Work";
	function.Graph.Nodes = {MakeNode(31, glm::vec2(500.0f, 500.0f)), MakeNode(32, glm::vec2(100.0f, 100.0f))};
	function.Graph.Links = {MakeLink(31, 32)};
	VisualScriptComment comment;
	comment.ID = 33;
	function.Graph.Comments.push_back(comment);
	document.Functions.push_back(function);
	document.Graph.Nodes = {MakeNode(1, glm::vec2(0.0f, 0.0f))};
	VisualScriptDocument const before = document;

	std::vector<uint32_t> const unknown = {31, 99};
	Result<void> result = VisualScriptEditing::ArrangeNodes(document, 30, unknown, DefaultExtents);
	CHECK(result.GetError() == "the graph has no node 99");
	std::vector<uint32_t> const commentID = {33};
	result = VisualScriptEditing::ArrangeNodes(document, 30, commentID, DefaultExtents);
	CHECK(result.GetError() == "33 is a comment: Arrange lays out nodes");
	std::vector<uint32_t> const otherGraph = {1};
	CHECK(VisualScriptEditing::ArrangeNodes(document, 30, otherGraph, DefaultExtents).IsError());
	result = VisualScriptEditing::ArrangeNodes(document, 7, {}, DefaultExtents);
	CHECK(result.GetError() == "the document has no function 7");
	result = VisualScriptEditing::ArrangeNodes(document, 30, {}, VisualScriptNodeExtents());
	CHECK(result.GetError() == "the size of node 31 is not known");
	result = VisualScriptEditing::ArrangeNodes(document, 30, {},
	                                           [](uint32_t node)
	                                           {
												   return node == 32 ? glm::vec2(std::nanf(""), 10.0f) : glm::vec2(100.0f);
											   });
	CHECK(result.GetError() == "the size of node 32 is not a size");
	result = VisualScriptEditing::ArrangeNodes(document, 30, {},
	                                           [](uint32_t)
	                                           {
												   return glm::vec2(-1.0f, 10.0f);
											   });
	CHECK(result.IsError());
	CHECK(document == before);

	REQUIRE(VisualScriptEditing::ArrangeNodes(document, 30, {}, DefaultExtents).IsOk());
	CHECK(document.Functions[0].Graph.Nodes[0].Position == glm::vec2(100.0f, 100.0f));
	CHECK(document.Functions[0].Graph.Nodes[1].Position == glm::vec2(420.0f, 100.0f));
	CHECK(document.Graph == before.Graph);

	// An empty graph has nothing to arrange.
	VisualScriptDocument empty;
	CHECK(VisualScriptEditing::ArrangeNodes(empty, 0, {}, DefaultExtents).IsOk());
	CHECK(empty == VisualScriptDocument());
}

TEST_CASE("VisualScriptArrange: links that close a cycle and links that skip columns leave the layout well formed")
{
	// 1 → 2 → 3 → 1 is a cycle; 1 → 3 skips a column; 4 feeds 3 from the first column.
	VisualScriptDocument document;
	for (uint32_t id = 1; id <= 4; id++)
	{
		document.Graph.Nodes.push_back(MakeNode(id));
	}
	document.Graph.Links = {MakeLink(1, 2), MakeLink(2, 3), MakeLink(3, 1), {{1, "Value"}, {3, "A"}}, {{4, "Value"}, {3, "B"}}};
	VisualScriptDocument expected = document;
	PlaceUnplacedNodes(expected);
	REQUIRE(VisualScriptEditing::ArrangeNodes(document, 0, {}, DefaultExtents).IsOk());
	CHECK(GetPositions(document.Graph) == GetPositions(expected.Graph));
	Testing::ExpectStrictRoundTrip(document);
}

TEST_CASE("VisualScriptArrange: a corner places the arranged block, whatever the corner of the nodes that had a position")
{
	VisualScriptDocument document = MakeBranchingDocument();
	for (VisualScriptNode& node : document.Graph.Nodes)
	{
		node.Position = glm::vec2(500.0f + static_cast<float>(node.ID) * 40.0f, 300.0f);
	}
	VisualScriptDocument expected = MakeBranchingDocument();
	REQUIRE(VisualScriptEditing::ArrangeNodes(expected, 0, {}, DefaultExtents).IsOk());

	// The corner is kept as given, on the grid or not.
	glm::vec2 const corner(-100.5f, 37.0f);
	REQUIRE(VisualScriptEditing::ArrangeNodes(document, 0, {}, DefaultExtents, corner).IsOk());
	CHECK(GetCorner(document.Graph) == corner);
	std::map<uint32_t, glm::vec2> const positions = GetPositions(document.Graph);
	for (auto const& [id, position] : GetPositions(expected.Graph))
	{
		CAPTURE(id);
		CHECK(positions.at(id) == corner + position);
	}
	Testing::ExpectStrictRoundTrip(document);
}

TEST_CASE("VisualScriptArrange: anchors stand before the arranged block, which starts right of them and orders its rows against theirs")
{
	// Anchors 1 (top) and 2 (below it) keep their places; 1 links into 11 and 2 into 10, which both lead to 12; 13 is a
	// group of its own.
	VisualScriptDocument document;
	document.Graph.Nodes = {
		MakeNode(1, glm::vec2(0.0f, 0.0f)), MakeNode(2, glm::vec2(0.0f, 400.0f)), MakeNode(10), MakeNode(11), MakeNode(12), MakeNode(13)};
	document.Graph.Links = {MakeLink(1, 11), MakeLink(2, 10), MakeLink(10, 12), MakeLink(11, 12)};
	std::vector<uint32_t> const arranged = {10, 11, 12, 13};
	std::vector<uint32_t> const anchors = {2, 1, 2};

	// Without anchors, ID order stands: 10 above 11.
	VisualScriptDocument alone = document;
	REQUIRE(VisualScriptEditing::ArrangeNodes(alone, 0, arranged, DefaultExtents).IsOk());
	CHECK(GetPositions(alone.Graph).at(10).y < GetPositions(alone.Graph).at(11).y);

	VisualScriptDocument const before = document;
	REQUIRE(VisualScriptEditing::ArrangeNodes(document, 0, arranged, DefaultExtents, std::nullopt, anchors).IsOk());
	std::map<uint32_t, glm::vec2> const positions = GetPositions(document.Graph);
	// The anchors do not move; the block's first column starts at their right edge + 64, its first row at their top.
	CHECK(positions.at(1) == glm::vec2(0.0f, 0.0f));
	CHECK(positions.at(2) == glm::vec2(0.0f, 400.0f));
	// 11, linked from the upper anchor, stands above 10, linked from the lower one.
	CHECK(positions.at(11) == glm::vec2(320.0f, 0.0f));
	CHECK(positions.at(10) == glm::vec2(320.0f, 160.0f));
	CHECK(positions.at(12) == glm::vec2(640.0f, 0.0f));
	// The next group stands below the first, in the same columns.
	CHECK(positions.at(13) == glm::vec2(320.0f, 400.0f));
	CHECK(CountCrossings(document.Graph) == 0);
	CHECK(document.Graph.Links == before.Graph.Links);
	Testing::ExpectStrictRoundTrip(document);

	SUBCASE("arranging again changes nothing, and the order of the anchors does not matter")
	{
		VisualScriptDocument again = document;
		std::vector<uint32_t> const reordered = {1, 2};
		REQUIRE(VisualScriptEditing::ArrangeNodes(again, 0, arranged, DefaultExtents, std::nullopt, reordered).IsOk());
		CHECK(again == document);
	}

	SUBCASE("the block's corner is rounded up to the 16 grid from the anchors' right edge and top, with their sizes")
	{
		VisualScriptDocument offGrid = before;
		offGrid.Graph.Nodes[0].Position = glm::vec2(37.0f, 13.0f);
		REQUIRE(VisualScriptEditing::ArrangeNodes(offGrid, 0, arranged, DefaultExtents, std::nullopt, anchors).IsOk());
		// The right edge is 37 + 256 = 293: the block starts at 293 + 64 = 357, on the grid 368; the top 13 gives 16.
		CHECK(GetPositions(offGrid.Graph).at(11) == glm::vec2(368.0f, 16.0f));

		VisualScriptDocument wide = before;
		VisualScriptNodeExtents const extents = [](uint32_t node)
		{
			return node == 2 ? glm::vec2(500.0f, 100.0f) : glm::vec2(256.0f, 128.0f);
		};
		REQUIRE(VisualScriptEditing::ArrangeNodes(wide, 0, arranged, extents, std::nullopt, anchors).IsOk());
		CHECK(GetPositions(wide.Graph).at(11) == glm::vec2(576.0f, 0.0f));
	}
}

TEST_CASE("VisualScriptArrange: anchors must be placed nodes of the graph that are not arranged, and exclude a corner")
{
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(1, glm::vec2(0.0f, 0.0f)), MakeNode(2), MakeNode(3), MakeNode(4)};
	document.Graph.Links = {MakeLink(1, 2), MakeLink(2, 3)};
	VisualScriptComment comment;
	comment.ID = 9;
	document.Graph.Comments.push_back(comment);
	VisualScriptDocument const before = document;
	std::vector<uint32_t> const arranged = {2, 3};
	auto const arrange = [&document, &arranged](std::vector<uint32_t> const& anchors, std::optional<glm::vec2> corner = std::nullopt)
	{
		return VisualScriptEditing::ArrangeNodes(document, 0, arranged, DefaultExtents, corner, anchors);
	};

	CHECK(arrange({1}, glm::vec2(0.0f)).GetError() == "Arrange takes a corner or anchors, not both");
	CHECK(arrange({99}).GetError() == "the graph has no node 99: an anchor is a node of the graph");
	CHECK(arrange({9}).GetError() == "the graph has no node 9: an anchor is a node of the graph");
	CHECK(arrange({1, 3}).GetError() == "node 3 is arranged: an anchor keeps its place");
	CHECK(arrange({4}).GetError() == "node 4 has no position: an anchor keeps its place");
	CHECK(arrange({}, glm::vec2(std::numeric_limits<float>::infinity(), 0.0f)).GetError() ==
	      "the corner of the arranged nodes must be finite");
	std::vector<uint32_t> const anchor = {1};
	CHECK(VisualScriptEditing::ArrangeNodes(document, 0, {}, DefaultExtents, std::nullopt, anchor).GetError() ==
	      "node 1 is arranged: an anchor keeps its place");
	Result<void> const unmeasured = VisualScriptEditing::ArrangeNodes(
		document, 0, arranged,
		[](uint32_t node)
		{
			return node == 1 ? glm::vec2(std::nanf(""), 10.0f) : glm::vec2(100.0f);
		},
		std::nullopt, anchor);
	CHECK(unmeasured.GetError() == "the size of node 1 is not a size");
	CHECK(document == before);

	REQUIRE(arrange({1}).IsOk());
	CHECK(document.Graph.Nodes[0].Position == glm::vec2(0.0f, 0.0f));
	CHECK(document.Graph.Nodes[1].Position == glm::vec2(320.0f, 0.0f));
	CHECK(document.Graph.Nodes[2].Position == glm::vec2(640.0f, 0.0f));
	CHECK_FALSE(document.Graph.Nodes[3].Position.has_value());
}
