#include "stpch.h"
#include "Strada/Asset/VisualScriptLayout.h"

#include "Strada/Asset/VisualScriptSerializer.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
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

		struct LayeredNode
		{
			size_t Component = 0;
			size_t Layer = 0;
			size_t Row = 0;
		};

		float ToCoordinate(double value)
		{
			// Large graphs laid out below placed nodes far out could leave the format's range; its edge keeps them loadable.
			return static_cast<float>(std::clamp(value, -VisualScriptSerializer::MaxCoordinate, VisualScriptSerializer::MaxCoordinate));
		}

		void SortByID(VisualScriptGraph const& graph, std::vector<size_t>& indices)
		{
			std::stable_sort(indices.begin(), indices.end(),
			                 [&graph](size_t first, size_t second)
			                 {
								 return graph.Nodes[first].ID < graph.Nodes[second].ID;
							 });
		}

		// Groups, layers and rows of some nodes of a graph, given as indices into its nodes in ID order: one entry per given
		// node, in the same order, and the nodes are numbered in that order. Only links between the given nodes count. A
		// graph built in memory may repeat an ID among them: links then lead to the first node with it.
		std::vector<LayeredNode> AssignLayers(VisualScriptGraph const& graph, std::vector<size_t> const& numbered)
		{
			size_t const count = numbered.size();
			std::unordered_map<uint32_t, size_t> numbers;
			for (size_t number = 0; number < count; number++)
			{
				numbers.emplace(graph.Nodes[numbered[number]].ID, number);
			}

			// Links between the nodes, as successors in ID order and as undirected neighbors.
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

			// A node's layer is the length of the longest chain of the remaining links that leads to it.
			std::vector<size_t> layers(count, 0);
			for (auto node = finished.rbegin(); node != finished.rend(); ++node)
			{
				for (size_t const successor : acyclicSuccessors[*node])
				{
					layers[successor] = std::max(layers[successor], layers[*node] + 1);
				}
			}

			std::vector<LayeredNode> layered(count);
			for (size_t component = 0; component < groups.size(); component++)
			{
				// The next free row of each layer of the group.
				std::unordered_map<size_t, size_t> rows;
				for (size_t const node : groups[component])
				{
					layered[node] = {component, layers[node], rows[layers[node]]++};
				}
			}
			return layered;
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

			SortByID(graph, unplaced);
			std::vector<LayeredNode> const layered = AssignLayers(graph, unplaced);

			// Groups stand one below the other, the first below the placed nodes: each starts below the last row of the one
			// before it.
			std::vector<size_t> lastRows;
			for (LayeredNode const& node : layered)
			{
				if (node.Component >= lastRows.size())
				{
					lastRows.resize(node.Component + 1, 0);
				}
				lastRows[node.Component] = std::max(lastRows[node.Component], node.Row);
			}
			std::vector<double> tops(lastRows.size());
			double top = hasPlaced ? bottom + GroupSpacing : 0.0;
			for (size_t component = 0; component < lastRows.size(); component++)
			{
				tops[component] = top;
				top += static_cast<double>(lastRows[component]) * RowSpacing + GroupSpacing;
			}

			for (size_t number = 0; number < unplaced.size(); number++)
			{
				LayeredNode const& node = layered[number];
				graph.Nodes[unplaced[number]].Position =
					glm::vec2(ToCoordinate(left + static_cast<double>(node.Layer) * ColumnSpacing),
				              ToCoordinate(tops[node.Component] + static_cast<double>(node.Row) * RowSpacing));
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

	std::vector<VisualScriptLayerAssignment> AssignVisualScriptLayers(VisualScriptGraph const& graph, std::span<uint32_t const> nodes)
	{
		// The first node of the graph with each given ID.
		std::unordered_set<uint32_t> const given(nodes.begin(), nodes.end());
		std::unordered_set<uint32_t> found;
		std::vector<size_t> numbered;
		for (size_t i = 0; i < graph.Nodes.size(); i++)
		{
			uint32_t const id = graph.Nodes[i].ID;
			if (given.contains(id) && found.insert(id).second)
			{
				numbered.push_back(i);
			}
		}
		SortByID(graph, numbered);
		std::vector<LayeredNode> const layered = AssignLayers(graph, numbered);

		std::vector<VisualScriptLayerAssignment> assignments;
		assignments.reserve(numbered.size());
		for (size_t number = 0; number < numbered.size(); number++)
		{
			LayeredNode const& node = layered[number];
			assignments.push_back({graph.Nodes[numbered[number]].ID, static_cast<uint32_t>(node.Component),
			                       static_cast<uint32_t>(node.Layer), static_cast<uint32_t>(node.Row)});
		}
		return assignments;
	}
}
