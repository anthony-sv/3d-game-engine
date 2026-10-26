#include "Fuzzing.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptSpatialIndex.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace Strada;

namespace
{
	using Index = VisualScriptSpatialIndex;

	VisualScriptNode MakeNode(uint32_t id, std::string type, glm::vec2 position)
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = position;
		return node;
	}

	// The visual model of a document's event graph with the fixture library, laid out with the fixed-width measurer.
	struct IndexedGraph
	{
		explicit IndexedGraph(VisualScriptDocument document)
			: Document(std::move(document))
		{
			VisualScriptCanvasSource source;
			source.Display = &Document;
			source.Library = &Semantics.GetFixtureLibrary();
			Model.Sync(source, Testing::FixedWidthMeasurer(0.5f), VisualScriptThemeAnchors());
			Grid.Build(Model);
		}

		// Every item of the kinds whose bounds (as the index holds them) intersect the rectangle, dynamic ones left out.
		std::vector<uint32_t> Scan(VisualScriptRect const& rect, uint8_t mask, std::vector<uint8_t> const& dynamicNodes = {},
		                           std::vector<uint8_t> const& dynamicLinks = {}, std::vector<uint8_t> const& dynamicComments = {}) const
		{
			std::vector<uint32_t> found;
			auto const scan = [&](Index::ItemKind kind, size_t count, uint8_t bit, std::vector<uint8_t> const& dynamic)
			{
				if ((mask & bit) == 0)
				{
					return;
				}
				for (uint32_t index = 0; index < count; index++)
				{
					uint32_t const item = Index::MakeItem(kind, index);
					bool const excluded = index < dynamic.size() && dynamic[index] != 0;
					if (!excluded && VisualScriptGeometry::Intersects(Index::GetItemBounds(Model, item), rect))
					{
						found.push_back(item);
					}
				}
			};
			scan(Index::ItemKind::Node, Model.GetNodes().size(), Index::NodeMask, dynamicNodes);
			scan(Index::ItemKind::Comment, Model.GetComments().size(), Index::CommentMask, dynamicComments);
			scan(Index::ItemKind::Link, Model.GetLinks().size(), Index::LinkMask, dynamicLinks);
			std::sort(found.begin(), found.end());
			return found;
		}

		std::vector<uint32_t> Query(VisualScriptRect const& rect, uint8_t mask, VisualScriptIndexStats* stats = nullptr) const
		{
			std::vector<uint32_t> found;
			Grid.Query(rect, mask, found, stats);
			std::sort(found.begin(), found.end());
			return found;
		}

		Testing::FixtureVisualScriptSemantics Semantics;
		VisualScriptDocument Document;
		VisualScriptCanvasModel Model;
		Index Grid;
	};

	// A random graph: nodes anywhere up to ±1e7 or clustered, comments up to 1e6 units, links between any two nodes (long
	// wires among them).
	VisualScriptDocument MakeRandomGraph(Testing::FuzzRandom& random)
	{
		static constexpr std::array<std::string_view, 5> Types = {"Flow.Branch", "Call:Strada.Log.Info(string)", "Operator.Add", "Reroute",
		                                                          "Event.Update"};
		VisualScriptDocument document;
		uint32_t const nodes = 1 + static_cast<uint32_t>(random.Pick(60));
		bool const spread = random.Pick(3) == 0;
		auto const coordinate = [&]()
		{
			if (spread)
			{
				return static_cast<float>(static_cast<double>(random.Pick(20000001)) - 1.0e7);
			}
			return static_cast<float>(random.Pick(8000)) - 4000.0f;
		};
		for (uint32_t id = 1; id <= nodes; id++)
		{
			document.Graph.Nodes.push_back(
				MakeNode(id, std::string(Types[random.Pick(Types.size())]), glm::vec2(coordinate(), coordinate())));
		}
		uint32_t const links = static_cast<uint32_t>(random.Pick(nodes * 3 + 1));
		for (uint32_t link = 0; link < links; link++)
		{
			uint32_t const from = 1 + static_cast<uint32_t>(random.Pick(nodes));
			uint32_t const to = 1 + static_cast<uint32_t>(random.Pick(nodes));
			document.Graph.Links.push_back({{from, "Out"}, {to, "In"}});
		}
		uint32_t const comments = static_cast<uint32_t>(random.Pick(6));
		for (uint32_t index = 0; index < comments; index++)
		{
			VisualScriptComment comment;
			comment.ID = nodes + 1 + index;
			comment.Position = glm::vec2(coordinate(), coordinate());
			float const size = random.Pick(2) == 0 ? 1.0e6f : static_cast<float>(16 + random.Pick(3000));
			comment.Size = glm::vec2(size, static_cast<float>(16 + random.Pick(3000)));
			document.Graph.Comments.push_back(comment);
		}
		return document;
	}
}

TEST_CASE("VisualScriptEditor: the spatial index finds exactly what a scan of every item finds, at any coordinates and sizes")
{
	Testing::FuzzRandom random(10500);
	int const rounds = Testing::GetFuzzRounds(200);
	size_t found = 0;
	size_t large = 0;
	for (int round = 0; round < rounds; round++)
	{
		CAPTURE(round);
		IndexedGraph const graph(MakeRandomGraph(random));
		large += graph.Grid.GetLargeItemCount();
		for (int query = 0; query < 10; query++)
		{
			glm::vec2 const center(static_cast<float>(static_cast<double>(random.Pick(20000001)) - 1.0e7),
			                       static_cast<float>(static_cast<double>(random.Pick(20000001)) - 1.0e7));
			glm::vec2 const anchor = query % 2 == 0 || graph.Model.GetNodes().empty()
			                             ? center
			                             : graph.Model.GetNodes()[random.Pick(graph.Model.GetNodes().size())].Position;
			float const extent = query % 3 == 0 ? 1.0e7f : static_cast<float>(random.Pick(5000));
			VisualScriptRect const rect{anchor - glm::vec2(extent), anchor + glm::vec2(static_cast<float>(random.Pick(5000)))};
			uint8_t const mask = static_cast<uint8_t>(1 + random.Pick(7));
			std::vector<uint32_t> const expected = graph.Scan(rect, mask);
			REQUIRE(graph.Query(rect, mask) == expected);
			found += expected.size();
		}
	}
	// The rounds found items and had items too large for the grid.
	CHECK(found > 0);
	CHECK(large > 0);
}

TEST_CASE("VisualScriptEditor: the spatial index leaves dynamic items out until they are cleared or the index is rebuilt")
{
	IndexedGraph graph(Testing::MakeGridGraph(4, 4, 2, 2));
	VisualScriptRect const everything{glm::vec2(-1.0e6f), glm::vec2(1.0e6f)};
	std::vector<uint32_t> const all = graph.Query(everything, Index::AllMask);
	REQUIRE(all.size() == graph.Model.GetNodes().size() + graph.Model.GetLinks().size() + graph.Model.GetComments().size());

	// A moved node and its wires, and a moved comment.
	std::vector<uint32_t> const nodes = {5};
	std::span<uint32_t const> const incident = graph.Model.GetIncidentLinks(5);
	std::vector<uint32_t> const links(incident.begin(), incident.end());
	REQUIRE_FALSE(links.empty());
	REQUIRE_FALSE(graph.Model.GetComments().empty());
	std::vector<uint32_t> const comments = {0};
	graph.Grid.SetDynamic(nodes, comments, links);
	std::vector<uint8_t> dynamicNodes(graph.Model.GetNodes().size(), 0);
	dynamicNodes[5] = 1;
	std::vector<uint8_t> dynamicLinks(graph.Model.GetLinks().size(), 0);
	for (uint32_t const link : links)
	{
		dynamicLinks[link] = 1;
	}
	std::vector<uint8_t> dynamicComments(graph.Model.GetComments().size(), 0);
	dynamicComments[0] = 1;
	std::vector<uint32_t> const moving = graph.Query(everything, Index::AllMask);
	CHECK(moving == graph.Scan(everything, Index::AllMask, dynamicNodes, dynamicLinks, dynamicComments));
	CHECK(moving.size() == all.size() - 2 - links.size());
	CHECK(std::find(moving.begin(), moving.end(), Index::MakeItem(Index::ItemKind::Node, 5)) == moving.end());
	CHECK(std::find(moving.begin(), moving.end(), Index::MakeItem(Index::ItemKind::Comment, 0)) == moving.end());

	// Cleared, and rebuilt.
	graph.Grid.SetDynamic({}, {}, {});
	CHECK(graph.Query(everything, Index::AllMask) == all);
	graph.Grid.SetDynamic(nodes, comments, links);
	graph.Grid.Build(graph.Model);
	CHECK(graph.Query(everything, Index::AllMask) == all);
	// Out-of-range indices are ignored.
	std::vector<uint32_t> const wrong = {100000};
	graph.Grid.SetDynamic(wrong, wrong, wrong);
	CHECK(graph.Query(everything, Index::AllMask) == all);
}

TEST_CASE("VisualScriptEditor: a viewport query visits the cells it covers and tests about the items visible, whatever the graph's size")
{
	// T5's view on growing graphs: the same 1600 × 900 pixels at a zoom of 0.224 cover the same part of each graph.
	VisualScriptRect const view{glm::vec2(0.0f), glm::vec2(1600.0f / 0.224f, 900.0f / 0.224f)};
	std::vector<uint32_t> candidates;
	for (uint32_t const columns : {20u, 50u, 100u})
	{
		CAPTURE(columns);
		IndexedGraph const graph(Testing::MakeGridGraph(columns, 50, 3, 10));
		VisualScriptIndexStats stats;
		std::vector<uint32_t> const visible = graph.Query(view, Index::AllMask, &stats);
		CHECK(visible == graph.Scan(view, Index::AllMask));
		// Candidates stay within twice the visible items plus a margin for items on the view's border; cells are the view's.
		CHECK(stats.Candidates <= 2 * visible.size() + 256);
		CHECK(stats.CellsVisited <= 16 * 9);
		candidates.push_back(stats.Candidates);
	}
	// The view's cost does not grow with the graph.
	CHECK(candidates[2] <= candidates[1] + candidates[1] / 10);

	// A query over more cells than are occupied walks the occupied cells.
	IndexedGraph const small(Testing::MakeGridGraph(2, 2, 1, 0));
	VisualScriptIndexStats stats;
	std::vector<uint32_t> const everything = small.Query({glm::vec2(-1.0e7f), glm::vec2(1.0e7f)}, Index::AllMask, &stats);
	CHECK(everything.size() == small.Model.GetNodes().size() + small.Model.GetLinks().size());
	CHECK(stats.CellsVisited == small.Grid.GetOccupiedCellCount());

	// A point query of a radius under half a cell visits at most 4 cells.
	VisualScriptIndexStats point;
	std::vector<uint32_t> nearby;
	small.Grid.QueryPoint(glm::vec2(511.0f, 511.0f), 255.0f, Index::AllMask, nearby, &point);
	CHECK(point.CellsVisited <= 4);
}

TEST_CASE("VisualScriptEditor: rebuilding the spatial index of an unchanged graph reserves no more memory")
{
	IndexedGraph graph(Testing::MakeGridGraph(30, 30, 4, 20));
	size_t const capacity = graph.Grid.GetCapacityBytes();
	CHECK(capacity > 0);
	std::vector<uint32_t> found;
	for (int frame = 0; frame < 10; frame++)
	{
		graph.Grid.Build(graph.Model);
		graph.Grid.Query({glm::vec2(0.0f), glm::vec2(3000.0f)}, Index::AllMask, found);
		CHECK(graph.Grid.GetCapacityBytes() == capacity);
	}
	CHECK(graph.Grid.GetItemCount(Index::ItemKind::Node) == 900);
	CHECK(graph.Grid.GetItemCount(Index::ItemKind::Link) == 900 * 4);
	CHECK(graph.Grid.GetItemCount(Index::ItemKind::Comment) == 20);
}
