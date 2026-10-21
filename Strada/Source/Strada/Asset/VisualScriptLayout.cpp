#include "stpch.h"
#include "Strada/Asset/VisualScriptLayout.h"

#include "Strada/Asset/VisualScriptSerializer.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr double ColumnSpacing = 320.0;
		constexpr double RowSpacing = 160.0;
		// Between the placed nodes and the first group, and between groups.
		constexpr double GroupSpacing = 240.0;

		enum class VisitState : uint8_t
		{
			New = 0,
			Active,
			Done
		};

		float ToCoordinate(double value)
		{
			// Large graphs laid out below placed nodes far out could leave the format's range; its edge keeps them loadable.
			return static_cast<float>(std::clamp(value, -VisualScriptSerializer::MaxCoordinate, VisualScriptSerializer::MaxCoordinate));
		}

		void PlaceGraph(VisualScriptGraph& graph)
		{
			// The unplaced nodes (indices into graph.Nodes) and the left and bottom edges of the placed ones.
			std::vector<size_t> unplaced;
			bool hasPlaced = false;
			double left = 0.0;
			double bottom = 0.0;
			for (size_t i = 0; i < graph.Nodes.size(); i++)
			{
				std::optional<glm::vec2> const& position = graph.Nodes[i].Position;
				if (!position)
				{
					unplaced.push_back(i);
					continue;
				}
				double const x = static_cast<double>(position->x);
				double const y = static_cast<double>(position->y);
				left = hasPlaced ? std::min(left, x) : x;
				bottom = hasPlaced ? std::max(bottom, y) : y;
				hasPlaced = true;
			}
			if (unplaced.empty())
			{
				return;
			}

			// From here on nodes are numbered in ID order. A document built in memory may repeat an ID: links then lead to the
			// first node with it.
			std::stable_sort(unplaced.begin(), unplaced.end(),
			                 [&graph](size_t first, size_t second)
			                 {
								 return graph.Nodes[first].ID < graph.Nodes[second].ID;
							 });
			size_t const count = unplaced.size();
			std::unordered_map<uint32_t, size_t> numbers;
			for (size_t number = 0; number < count; number++)
			{
				numbers.emplace(graph.Nodes[unplaced[number]].ID, number);
			}

			// Links between unplaced nodes, as successors in ID order and as undirected neighbors.
			std::vector<std::vector<size_t>> successors(count);
			std::vector<std::vector<size_t>> neighbors(count);
			for (VisualScriptLink const& link : graph.Links)
			{
				auto const from = numbers.find(link.From.Node);
				auto const to = numbers.find(link.To.Node);
				if (from == numbers.end() || to == numbers.end())
				{
					continue;
				}
				successors[from->second].push_back(to->second);
				neighbors[from->second].push_back(to->second);
				neighbors[to->second].push_back(from->second);
			}
			for (std::vector<size_t>& list : successors)
			{
				std::sort(list.begin(), list.end());
				list.erase(std::unique(list.begin(), list.end()), list.end());
			}

			// Groups of nodes connected by links, in the order of their smallest IDs; each group's nodes in ID order.
			std::vector<std::vector<size_t>> groups;
			std::vector<bool> grouped(count, false);
			std::vector<size_t> pending;
			for (size_t start = 0; start < count; start++)
			{
				if (grouped[start])
				{
					continue;
				}
				std::vector<size_t>& group = groups.emplace_back();
				grouped[start] = true;
				pending.push_back(start);
				while (!pending.empty())
				{
					size_t const node = pending.back();
					pending.pop_back();
					group.push_back(node);
					for (size_t const neighbor : neighbors[node])
					{
						if (!grouped[neighbor])
						{
							grouped[neighbor] = true;
							pending.push_back(neighbor);
						}
					}
				}
				std::sort(group.begin(), group.end());
			}

			// A depth-first search from every node in ID order, following links in ID order, finds the links that close cycles
			// (those to a node still being searched); the others form an acyclic graph, which the reverse of the order in which
			// the search finishes its nodes sorts topologically. The search keeps its own stack: graphs may be chains of
			// thousands of nodes.
			std::vector<VisitState> states(count, VisitState::New);
			std::vector<std::vector<size_t>> acyclicSuccessors(count);
			std::vector<size_t> finished;
			finished.reserve(count);
			// Each entry: a node and the index of the next successor to follow.
			std::vector<std::pair<size_t, size_t>> stack;
			for (size_t start = 0; start < count; start++)
			{
				if (states[start] != VisitState::New)
				{
					continue;
				}
				states[start] = VisitState::Active;
				stack.emplace_back(start, 0);
				while (!stack.empty())
				{
					size_t const node = stack.back().first;
					size_t const next = stack.back().second;
					if (next == successors[node].size())
					{
						states[node] = VisitState::Done;
						finished.push_back(node);
						stack.pop_back();
						continue;
					}
					stack.back().second++;
					size_t const successor = successors[node][next];
					if (states[successor] == VisitState::Active)
					{
						continue;
					}
					acyclicSuccessors[node].push_back(successor);
					if (states[successor] == VisitState::New)
					{
						states[successor] = VisitState::Active;
						stack.emplace_back(successor, 0);
					}
				}
			}

			// A node's column is the length of the longest chain of the remaining links that leads to it.
			std::vector<size_t> columns(count, 0);
			for (auto node = finished.rbegin(); node != finished.rend(); ++node)
			{
				for (size_t const successor : acyclicSuccessors[*node])
				{
					columns[successor] = std::max(columns[successor], columns[*node] + 1);
				}
			}

			double top = hasPlaced ? bottom + GroupSpacing : 0.0;
			for (std::vector<size_t> const& group : groups)
			{
				// The next free row of each column of the group.
				std::unordered_map<size_t, size_t> rows;
				size_t lastRow = 0;
				for (size_t const node : group)
				{
					size_t const column = columns[node];
					size_t const row = rows[column]++;
					lastRow = std::max(lastRow, row);
					graph.Nodes[unplaced[node]].Position = glm::vec2(ToCoordinate(left + static_cast<double>(column) * ColumnSpacing),
					                                                 ToCoordinate(top + static_cast<double>(row) * RowSpacing));
				}
				top += static_cast<double>(lastRow) * RowSpacing + GroupSpacing;
			}
		}
	}

	void PlaceUnplacedNodes(VisualScriptDocument& document)
	{
		PlaceGraph(document.Graph);
		for (VisualScriptFunction& function : document.Functions)
		{
			PlaceGraph(function.Graph);
		}
	}
}
