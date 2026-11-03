#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strada
{
	struct VisualScriptDocument;

	// A breakpoint on a node of a graph's document (a script graph or a function library). Breakpoints are view state, kept
	// per user and per project (the views file), never in the document and never in the undo history.
	struct VisualScriptBreakpoint
	{
		// A node of the graph's document (node IDs are unique across its sub-graphs).
		uint32_t Node = 0;
		// The node's Type when the breakpoint was set: a node that later has the ID with another Type is not it.
		std::string Type;
		bool Enabled = true;
		// A formula that must hold for a hit to count; empty: every hit counts. At most MaxConditionBytes.
		std::string Condition;
		// 0: every hit stops; n: hits 1 to n - 1 run on, every hit from the n-th on stops. At most MaxHitCount.
		uint32_t HitCount = 0;

		bool operator==(VisualScriptBreakpoint const& other) const = default;
	};

	// The editor's breakpoints, per graph and ordered by node ID within each graph. It knows nothing of runtimes, projects or
	// files: the debug session keeps one, the views file another of each project. Pure; main thread only.
	class VisualScriptBreakpointStore
	{
	public:
		static constexpr size_t MaxBreakpointsPerGraph = 256;
		static constexpr size_t MaxConditionBytes = 1024;
		static constexpr uint32_t MaxHitCount = 1'000'000'000;

		// Whether a breakpoint can be kept, as Set checks it (the views file reads its entries with it): refused for node 0, a
		// Type that is no node type (empty, too long, with control characters or not UTF-8), a condition longer than
		// MaxConditionBytes or not UTF-8, and a hit count above MaxHitCount. The graph's size is Set's to check.
		[[nodiscard]] static Result<void> Check(VisualScriptBreakpoint const& breakpoint);

		// The graph's breakpoints by node ID; empty when it has none.
		std::span<VisualScriptBreakpoint const> Get(AssetHandle graph) const;
		// Null when the node has no breakpoint.
		VisualScriptBreakpoint const* Find(AssetHandle graph, uint32_t node) const;
		// The graphs that have breakpoints, in the order of their handles' values.
		std::vector<AssetHandle> GetGraphs() const;
		bool IsEmpty() const { return m_Graphs.empty(); }

		// Replaces the node's breakpoint, or adds it. Refused: an invalid graph, what Check refuses, a new node of a graph that
		// has MaxBreakpointsPerGraph breakpoints.
		[[nodiscard]] Result<void> Set(AssetHandle graph, VisualScriptBreakpoint breakpoint);
		// True when the node had a breakpoint.
		bool Remove(AssetHandle graph, uint32_t node);
		// The graph's breakpoints, or every graph's.
		void Clear(std::optional<AssetHandle> graph);
		// F9 on a selection: when every node of the breakpoints given (their nodes and types; other members are those of a
		// new breakpoint) has a breakpoint, removes them all; otherwise adds the breakpoints given to the nodes that have none,
		// keeping those that have one. Refused, changing nothing, when an added one would be refused by Set. Repeated nodes
		// count once.
		[[nodiscard]] Result<void> Toggle(AssetHandle graph, std::span<VisualScriptBreakpoint const> breakpoints);
		// Breakpoints follow edits: removes the graph's breakpoints whose node the document has not, or has with another Type
		// (a node that took the ID of a deleted one is not it); moves, renames and other edits keep them. True when one went.
		bool Follow(AssetHandle graph, VisualScriptDocument const& document);

		// Changes with every change.
		uint64_t GetRevision() const { return m_Revision; }

	private:
		struct GraphBreakpoints
		{
			AssetHandle Graph;
			// By node ID; never empty.
			std::vector<VisualScriptBreakpoint> Breakpoints;
		};

		GraphBreakpoints const* FindGraph(AssetHandle graph) const;
		// The graph's entry, made when missing.
		GraphBreakpoints& UseGraph(AssetHandle graph);
		// Drops the graph's entry when it has no breakpoint left.
		void DropIfEmpty(AssetHandle graph);

		// By the graphs' handle values.
		std::vector<GraphBreakpoints> m_Graphs;
		uint64_t m_Revision = 0;
	};
}
