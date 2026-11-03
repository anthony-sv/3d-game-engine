#include "Editor/VisualScripting/VisualScriptBreakpoints.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Assert.h"
#include "Strada/Core/Utf8.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Strada
{
	namespace
	{
		bool IsBefore(AssetHandle a, AssetHandle b)
		{
			return a.GetUUID().GetValue() < b.GetUUID().GetValue();
		}

		// Where the node's breakpoint is, or would be inserted.
		std::vector<VisualScriptBreakpoint>::iterator FindPosition(std::vector<VisualScriptBreakpoint>& breakpoints, uint32_t node)
		{
			return std::lower_bound(breakpoints.begin(), breakpoints.end(), node,
			                        [](VisualScriptBreakpoint const& breakpoint, uint32_t id)
			                        {
										return breakpoint.Node < id;
									});
		}

		void AddTypes(VisualScriptGraph const& graph, std::unordered_map<uint32_t, std::string_view>& types)
		{
			for (VisualScriptNode const& node : graph.Nodes)
			{
				types.emplace(node.ID, node.Type);
			}
		}
	}

	Result<void> VisualScriptBreakpointStore::Check(VisualScriptBreakpoint const& breakpoint)
	{
		if (breakpoint.Node == 0)
		{
			return Error{"a breakpoint needs a node: node IDs start at 1"};
		}
		if (!VisualScriptSerializer::IsValidText(breakpoint.Type, VisualScriptSerializer::MaxNodeTypeBytes))
		{
			return Error{"a breakpoint needs its node's Type"};
		}
		if (breakpoint.Condition.size() > MaxConditionBytes)
		{
			return MakeError("the condition has {} bytes: a condition has at most {}", breakpoint.Condition.size(), MaxConditionBytes);
		}
		if (!Utf8::IsValid(breakpoint.Condition))
		{
			return Error{"the condition is not valid UTF-8 text"};
		}
		if (breakpoint.HitCount > MaxHitCount)
		{
			return MakeError("the hit count {} is above {}, the largest one", breakpoint.HitCount, MaxHitCount);
		}
		return {};
	}

	std::span<VisualScriptBreakpoint const> VisualScriptBreakpointStore::Get(AssetHandle graph) const
	{
		GraphBreakpoints const* const entry = FindGraph(graph);
		return entry != nullptr ? std::span<VisualScriptBreakpoint const>(entry->Breakpoints) : std::span<VisualScriptBreakpoint const>();
	}

	VisualScriptBreakpoint const* VisualScriptBreakpointStore::Find(AssetHandle graph, uint32_t node) const
	{
		std::span<VisualScriptBreakpoint const> const breakpoints = Get(graph);
		auto const found = std::lower_bound(breakpoints.begin(), breakpoints.end(), node,
		                                    [](VisualScriptBreakpoint const& breakpoint, uint32_t id)
		                                    {
												return breakpoint.Node < id;
											});
		return found != breakpoints.end() && found->Node == node ? &*found : nullptr;
	}

	std::vector<AssetHandle> VisualScriptBreakpointStore::GetGraphs() const
	{
		std::vector<AssetHandle> graphs;
		graphs.reserve(m_Graphs.size());
		for (GraphBreakpoints const& entry : m_Graphs)
		{
			graphs.push_back(entry.Graph);
		}
		return graphs;
	}

	Result<void> VisualScriptBreakpointStore::Set(AssetHandle graph, VisualScriptBreakpoint breakpoint)
	{
		if (!graph.IsValid())
		{
			return Error{"a breakpoint needs a graph"};
		}
		if (Result<void> checked = Check(breakpoint); !checked)
		{
			return checked;
		}
		GraphBreakpoints const* const existing = FindGraph(graph);
		if (existing != nullptr && existing->Breakpoints.size() >= MaxBreakpointsPerGraph && Find(graph, breakpoint.Node) == nullptr)
		{
			return MakeError("the graph has {} breakpoints, the most a graph can have", MaxBreakpointsPerGraph);
		}
		std::vector<VisualScriptBreakpoint>& breakpoints = UseGraph(graph).Breakpoints;
		auto const position = FindPosition(breakpoints, breakpoint.Node);
		if (position != breakpoints.end() && position->Node == breakpoint.Node)
		{
			if (*position == breakpoint)
			{
				return {};
			}
			*position = std::move(breakpoint);
		}
		else
		{
			breakpoints.insert(position, std::move(breakpoint));
		}
		m_Revision++;
		return {};
	}

	bool VisualScriptBreakpointStore::Remove(AssetHandle graph, uint32_t node)
	{
		auto const entry = std::find_if(m_Graphs.begin(), m_Graphs.end(),
		                                [graph](GraphBreakpoints const& candidate)
		                                {
											return candidate.Graph == graph;
										});
		if (entry == m_Graphs.end())
		{
			return false;
		}
		auto const position = FindPosition(entry->Breakpoints, node);
		if (position == entry->Breakpoints.end() || position->Node != node)
		{
			return false;
		}
		entry->Breakpoints.erase(position);
		DropIfEmpty(graph);
		m_Revision++;
		return true;
	}

	void VisualScriptBreakpointStore::Clear(std::optional<AssetHandle> graph)
	{
		size_t const before = m_Graphs.size();
		if (graph)
		{
			std::erase_if(m_Graphs,
			              [graph](GraphBreakpoints const& entry)
			              {
							  return entry.Graph == *graph;
						  });
		}
		else
		{
			m_Graphs.clear();
		}
		if (m_Graphs.size() != before)
		{
			m_Revision++;
		}
	}

	Result<void> VisualScriptBreakpointStore::Toggle(AssetHandle graph, std::span<VisualScriptBreakpoint const> breakpoints)
	{
		if (!graph.IsValid())
		{
			return Error{"a breakpoint needs a graph"};
		}
		bool const everyNodeHasOne = std::all_of(breakpoints.begin(), breakpoints.end(),
		                                         [this, graph](VisualScriptBreakpoint const& breakpoint)
		                                         {
													 return Find(graph, breakpoint.Node) != nullptr;
												 });
		if (everyNodeHasOne)
		{
			for (VisualScriptBreakpoint const& breakpoint : breakpoints)
			{
				Remove(graph, breakpoint.Node);
			}
			return {};
		}

		// Checked as a whole first, so that a refusal changes nothing.
		std::vector<VisualScriptBreakpoint> added;
		for (VisualScriptBreakpoint const& breakpoint : breakpoints)
		{
			bool const repeated = std::any_of(added.begin(), added.end(),
			                                  [&breakpoint](VisualScriptBreakpoint const& earlier)
			                                  {
												  return earlier.Node == breakpoint.Node;
											  });
			if (repeated || Find(graph, breakpoint.Node) != nullptr)
			{
				continue;
			}
			if (Result<void> checked = Check(breakpoint); !checked)
			{
				return checked;
			}
			added.push_back(breakpoint);
		}
		if (Get(graph).size() + added.size() > MaxBreakpointsPerGraph)
		{
			return MakeError("the graph would have {} breakpoints: a graph has at most {}", Get(graph).size() + added.size(),
			                 MaxBreakpointsPerGraph);
		}
		for (VisualScriptBreakpoint& breakpoint : added)
		{
			Result<void> const set = Set(graph, std::move(breakpoint));
			ST_ASSERT(set.IsOk(), "Toggle checked every breakpoint it adds");
			(void)set;
		}
		return {};
	}

	bool VisualScriptBreakpointStore::Follow(AssetHandle graph, VisualScriptDocument const& document)
	{
		auto const entry = std::find_if(m_Graphs.begin(), m_Graphs.end(),
		                                [graph](GraphBreakpoints const& candidate)
		                                {
											return candidate.Graph == graph;
										});
		if (entry == m_Graphs.end())
		{
			return false;
		}
		std::unordered_map<uint32_t, std::string_view> types;
		AddTypes(document.Graph, types);
		for (VisualScriptFunction const& function : document.Functions)
		{
			AddTypes(function.Graph, types);
		}
		size_t const removed = std::erase_if(entry->Breakpoints,
		                                     [&types](VisualScriptBreakpoint const& breakpoint)
		                                     {
												 auto const found = types.find(breakpoint.Node);
												 return found == types.end() || found->second != breakpoint.Type;
											 });
		if (removed == 0)
		{
			return false;
		}
		DropIfEmpty(graph);
		m_Revision++;
		return true;
	}

	VisualScriptBreakpointStore::GraphBreakpoints const* VisualScriptBreakpointStore::FindGraph(AssetHandle graph) const
	{
		auto const found = std::lower_bound(m_Graphs.begin(), m_Graphs.end(), graph,
		                                    [](GraphBreakpoints const& entry, AssetHandle handle)
		                                    {
												return IsBefore(entry.Graph, handle);
											});
		return found != m_Graphs.end() && found->Graph == graph ? &*found : nullptr;
	}

	VisualScriptBreakpointStore::GraphBreakpoints& VisualScriptBreakpointStore::UseGraph(AssetHandle graph)
	{
		auto const found = std::lower_bound(m_Graphs.begin(), m_Graphs.end(), graph,
		                                    [](GraphBreakpoints const& entry, AssetHandle handle)
		                                    {
												return IsBefore(entry.Graph, handle);
											});
		if (found != m_Graphs.end() && found->Graph == graph)
		{
			return *found;
		}
		GraphBreakpoints entry;
		entry.Graph = graph;
		return *m_Graphs.insert(found, std::move(entry));
	}

	void VisualScriptBreakpointStore::DropIfEmpty(AssetHandle graph)
	{
		std::erase_if(m_Graphs,
		              [graph](GraphBreakpoints const& entry)
		              {
						  return entry.Graph == graph && entry.Breakpoints.empty();
					  });
	}
}
