#include "Editor/VisualScripting/VisualScriptBreakpoints.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/UUID.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	AssetHandle MakeHandle(uint64_t value)
	{
		return AssetHandle(UUID(value));
	}

	VisualScriptBreakpoint MakeBreakpoint(uint32_t node, std::string type = "Variable.Set", std::string condition = {},
	                                      uint32_t hitCount = 0)
	{
		VisualScriptBreakpoint breakpoint;
		breakpoint.Node = node;
		breakpoint.Type = std::move(type);
		breakpoint.Condition = std::move(condition);
		breakpoint.HitCount = hitCount;
		return breakpoint;
	}

	std::vector<uint32_t> GetNodes(VisualScriptBreakpointStore const& store, AssetHandle graph)
	{
		std::vector<uint32_t> nodes;
		for (VisualScriptBreakpoint const& breakpoint : store.Get(graph))
		{
			nodes.push_back(breakpoint.Node);
		}
		return nodes;
	}

	VisualScriptNode MakeNode(uint32_t id, std::string type)
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		return node;
	}
}

TEST_CASE("VisualScriptDebug: the breakpoint store keeps each graph's breakpoints by node, and changes its revision only on a change")
{
	VisualScriptBreakpointStore store;
	AssetHandle const player = MakeHandle(5000);
	AssetHandle const enemy = MakeHandle(3000);
	CHECK(store.IsEmpty());
	CHECK(store.Get(player).empty());
	CHECK(store.Find(player, 12) == nullptr);
	uint64_t revision = store.GetRevision();

	REQUIRE(store.Set(player, MakeBreakpoint(12, "Flow.Branch", "Health < 0")).IsOk());
	REQUIRE(store.Set(player, MakeBreakpoint(4)).IsOk());
	REQUIRE(store.Set(player, MakeBreakpoint(30, "Event.Update")).IsOk());
	REQUIRE(store.Set(enemy, MakeBreakpoint(7, "Variable.Get")).IsOk());
	CHECK(store.GetRevision() == revision + 4);
	CHECK(GetNodes(store, player) == std::vector<uint32_t>{4, 12, 30});
	// Graphs are listed by their handles' values.
	CHECK(store.GetGraphs() == std::vector<AssetHandle>{enemy, player});
	REQUIRE(store.Find(player, 12) != nullptr);
	CHECK(*store.Find(player, 12) == MakeBreakpoint(12, "Flow.Branch", "Health < 0"));
	CHECK(store.Find(enemy, 12) == nullptr);

	// Set replaces the node's breakpoint; an equal one changes nothing.
	revision = store.GetRevision();
	REQUIRE(store.Set(player, MakeBreakpoint(12, "Flow.Branch", "Health < 0")).IsOk());
	CHECK(store.GetRevision() == revision);
	VisualScriptBreakpoint disabled = MakeBreakpoint(12, "Flow.Branch", "Health < 0", 3);
	disabled.Enabled = false;
	REQUIRE(store.Set(player, disabled).IsOk());
	CHECK(store.GetRevision() == revision + 1);
	CHECK(*store.Find(player, 12) == disabled);
	CHECK(store.Get(player).size() == 3);

	// Remove, and Clear of one graph or of all.
	revision = store.GetRevision();
	CHECK_FALSE(store.Remove(player, 99));
	CHECK_FALSE(store.Remove(MakeHandle(1), 12));
	CHECK(store.GetRevision() == revision);
	CHECK(store.Remove(player, 4));
	CHECK(GetNodes(store, player) == std::vector<uint32_t>{12, 30});
	CHECK(store.GetRevision() == revision + 1);
	store.Clear(MakeHandle(1));
	CHECK(store.GetRevision() == revision + 1);
	store.Clear(enemy);
	CHECK(store.GetGraphs() == std::vector<AssetHandle>{player});
	CHECK(store.GetRevision() == revision + 2);
	CHECK(store.Remove(player, 12));
	CHECK(store.Remove(player, 30));
	// A graph without breakpoints is not listed.
	CHECK(store.IsEmpty());
	CHECK(store.GetGraphs().empty());
	REQUIRE(store.Set(player, MakeBreakpoint(1)).IsOk());
	REQUIRE(store.Set(enemy, MakeBreakpoint(1)).IsOk());
	store.Clear(std::nullopt);
	CHECK(store.IsEmpty());
	revision = store.GetRevision();
	store.Clear(std::nullopt);
	CHECK(store.GetRevision() == revision);
}

TEST_CASE("VisualScriptDebug: the breakpoint store refuses what it cannot keep, within its limits")
{
	VisualScriptBreakpointStore store;
	AssetHandle const graph = MakeHandle(5000);

	auto const refusal = [&store, graph](VisualScriptBreakpoint const& breakpoint)
	{
		Result<void> const set = store.Set(graph, breakpoint);
		REQUIRE(set.IsError());
		return set.GetError();
	};
	CHECK(store.Set(AssetHandle(), MakeBreakpoint(3)).GetError() == "a breakpoint needs a graph");
	CHECK(refusal(MakeBreakpoint(0)) == "a breakpoint needs a node: node IDs start at 1");
	CHECK(refusal(MakeBreakpoint(3, "")) == "a breakpoint needs its node's Type");
	CHECK(refusal(MakeBreakpoint(3, "Flow.\nBranch")) == "a breakpoint needs its node's Type");
	CHECK(refusal(MakeBreakpoint(3, "Variable.Set", std::string(1025, 'a'))) ==
	      "the condition has 1025 bytes: a condition has at most 1024");
	CHECK(refusal(MakeBreakpoint(3, "Variable.Set", "Health < \xFF")) == "the condition is not valid UTF-8 text");
	CHECK(refusal(MakeBreakpoint(3, "Variable.Set", {}, 1'000'000'001)) == "the hit count 1000000001 is above 1000000000, the largest one");
	CHECK(store.IsEmpty());
	CHECK(VisualScriptBreakpointStore::Check(MakeBreakpoint(3, "Variable.Set", {}, 1'000'000'001)).IsError());

	// The largest condition and hit count are kept, conditions on several lines too.
	CHECK(store.Set(graph, MakeBreakpoint(3, "Variable.Set", std::string(1024, 'a'))).IsOk());
	CHECK(store.Set(graph, MakeBreakpoint(4, "Variable.Set", "Index ==\n3", VisualScriptBreakpointStore::MaxHitCount)).IsOk());
	CHECK(VisualScriptBreakpointStore::Check(MakeBreakpoint(4, "Variable.Set", "Index ==\n3", VisualScriptBreakpointStore::MaxHitCount))
	          .IsOk());

	// A full graph takes no new node, but its breakpoints still change; other graphs are not full.
	store.Clear(std::nullopt);
	for (uint32_t node = 1; node <= VisualScriptBreakpointStore::MaxBreakpointsPerGraph; node++)
	{
		REQUIRE(store.Set(graph, MakeBreakpoint(node)).IsOk());
	}
	CHECK(refusal(MakeBreakpoint(1000)) == "the graph has 256 breakpoints, the most a graph can have");
	CHECK(store.Find(graph, 1000) == nullptr);
	CHECK(store.Set(graph, MakeBreakpoint(7, "Variable.Set", "Count > 2")).IsOk());
	CHECK(store.Find(graph, 7)->Condition == "Count > 2");
	CHECK(store.Set(MakeHandle(6000), MakeBreakpoint(1000)).IsOk());
	CHECK(store.Get(graph).size() == VisualScriptBreakpointStore::MaxBreakpointsPerGraph);
}

TEST_CASE("VisualScriptDebug: F9 on a selection adds a breakpoint to every node unless each has one, and then removes them all")
{
	VisualScriptBreakpointStore store;
	AssetHandle const graph = MakeHandle(5000);
	REQUIRE(store.Set(graph, MakeBreakpoint(5, "Variable.Set", "Count > 2", 4)).IsOk());

	// Some nodes have one: the others get one, and the existing one keeps its condition and hit count.
	std::array<VisualScriptBreakpoint, 4> const selection = {MakeBreakpoint(5), MakeBreakpoint(9, "Flow.Branch"),
	                                                         MakeBreakpoint(2, "Operator.Add"), MakeBreakpoint(9, "Flow.Branch")};
	REQUIRE(store.Toggle(graph, selection).IsOk());
	CHECK(GetNodes(store, graph) == std::vector<uint32_t>{2, 5, 9});
	CHECK(*store.Find(graph, 5) == MakeBreakpoint(5, "Variable.Set", "Count > 2", 4));
	CHECK(store.Find(graph, 9)->Type == "Flow.Branch");
	CHECK(store.Find(graph, 2)->Enabled);

	// Every node has one: they all go, and nodes outside the selection keep theirs.
	REQUIRE(store.Set(graph, MakeBreakpoint(40)).IsOk());
	uint64_t const revision = store.GetRevision();
	REQUIRE(store.Toggle(graph, selection).IsOk());
	CHECK(GetNodes(store, graph) == std::vector<uint32_t>{40});
	CHECK(store.GetRevision() == revision + 3);

	// A toggle that would overfill the graph, or add a breakpoint Set refuses, changes nothing.
	for (uint32_t node = 100; node < 100 + VisualScriptBreakpointStore::MaxBreakpointsPerGraph - 3; node++)
	{
		REQUIRE(store.Set(graph, MakeBreakpoint(node)).IsOk());
	}
	std::array<VisualScriptBreakpoint, 3> const three = {MakeBreakpoint(1), MakeBreakpoint(2), MakeBreakpoint(3)};
	uint64_t const full = store.GetRevision();
	CHECK(store.Toggle(graph, three).GetError() == "the graph would have 257 breakpoints: a graph has at most 256");
	CHECK(store.GetRevision() == full);
	std::array<VisualScriptBreakpoint, 2> const refused = {MakeBreakpoint(1), MakeBreakpoint(0)};
	CHECK(store.Toggle(graph, refused).IsError());
	CHECK(store.Find(graph, 1) == nullptr);
	CHECK(store.Toggle(AssetHandle(), refused).IsError());
	// An empty selection has nothing to toggle.
	CHECK(store.Toggle(graph, std::span<VisualScriptBreakpoint const>()).IsOk());
	CHECK(store.GetRevision() == full);
}

TEST_CASE("VisualScriptDebug: breakpoints follow edits: a deleted node or another Type under its ID loses it, moves keep it")
{
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(10, "Event.Update"), MakeNode(11, "Variable.Set"), MakeNode(12, "Flow.Branch")};
	VisualScriptFunction function;
	function.ID = 20;
	function.Name = "Damage";
	function.Graph.Nodes = {MakeNode(21, "Function.Entry"), MakeNode(22, "Variable.Set")};
	document.Functions.push_back(function);

	VisualScriptBreakpointStore store;
	AssetHandle const graph = MakeHandle(5000);
	AssetHandle const other = MakeHandle(6000);
	REQUIRE(store.Set(graph, MakeBreakpoint(11)).IsOk());
	REQUIRE(store.Set(graph, MakeBreakpoint(12, "Flow.Branch", "Ready")).IsOk());
	REQUIRE(store.Set(graph, MakeBreakpoint(22)).IsOk());
	REQUIRE(store.Set(other, MakeBreakpoint(11)).IsOk());
	uint64_t const revision = store.GetRevision();

	// The document has every node, in whichever sub-graph: nothing goes, also after a move.
	CHECK_FALSE(store.Follow(graph, document));
	document.Graph.Nodes[1].Position = glm::vec2(640.0f, 96.0f);
	document.Graph.Nodes[2].Comment = "Ready?";
	CHECK_FALSE(store.Follow(graph, document));
	CHECK(store.GetRevision() == revision);
	CHECK_FALSE(store.Follow(MakeHandle(1), document));

	// Node 11 deleted, and node 12's ID taken by a node of another Type.
	document.Graph.Nodes.erase(document.Graph.Nodes.begin() + 1);
	document.Graph.Nodes[1] = MakeNode(12, "Flow.Sequence");
	CHECK(store.Follow(graph, document));
	CHECK(GetNodes(store, graph) == std::vector<uint32_t>{22});
	CHECK(store.GetRevision() == revision + 1);
	// Other graphs keep theirs.
	CHECK(GetNodes(store, other) == std::vector<uint32_t>{11});
	// Node 22 deleted with its function: the graph has none left.
	document.Functions.clear();
	CHECK(store.Follow(graph, document));
	CHECK(store.Get(graph).empty());
	CHECK(store.GetGraphs() == std::vector<AssetHandle>{other});
}
