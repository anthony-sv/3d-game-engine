#include "Editor/VisualScripting/VisualScriptArrange.h"

#include "Editor/VisualScripting/VisualScriptEditing.h"

#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Asset/VisualScriptSerializer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		using Format = VisualScriptSerializer;

		// The spacing of the layout: after a column's widest node, between the nodes of a column and between groups.
		constexpr double ColumnGap = 64.0;
		constexpr double RowGap = 32.0;
		constexpr double GroupGap = 112.0;
		// Columns and rows start on this grid.
		constexpr double Grid = 16.0;
		// Down, up, down, up: enough for the orders of the graphs people draw to settle.
		constexpr int SweepCount = 4;
		constexpr glm::vec2 DefaultNodeSize{256.0f, 128.0f};

		// A node being arranged.
		struct ArrangedNode
		{
			uint32_t ID = 0;
			// Into the graph's nodes.
			size_t Index = 0;
			glm::dvec2 Size{0.0};
			uint32_t Component = 0;
			uint32_t Layer = 0;
			// Where the layout puts it, relative to the corner of the arranged bounds.
			glm::dvec2 Offset{0.0};
		};

		// The columns of one group of linked nodes: per layer, the group's nodes (indices into the arranged nodes) in their
		// order from top to bottom.
		using Columns = std::vector<std::vector<size_t>>;

		Result<VisualScriptGraph*> FindGraph(VisualScriptDocument& document, uint32_t function)
		{
			if (function == 0)
			{
				return &document.Graph;
			}
			for (VisualScriptFunction& candidate : document.Functions)
			{
				if (candidate.ID == function)
				{
					return &candidate.Graph;
				}
			}
			return MakeError("the document has no function {}", function);
		}

		Result<glm::dvec2> MeasureNode(VisualScriptNodeExtents const& extents, uint32_t node)
		{
			if (!extents)
			{
				return MakeError("the size of node {} is not known", node);
			}
			glm::vec2 const size = extents(node);
			if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x < 0.0f || size.y < 0.0f)
			{
				return MakeError("the size of node {} is not a size", node);
			}
			return glm::dvec2(size);
		}

		double RoundUpToGrid(double value)
		{
			return std::ceil(value / Grid) * Grid;
		}

		float ToCoordinate(double value)
		{
			return static_cast<float>(std::clamp(value, -Format::MaxCoordinate, Format::MaxCoordinate));
		}

		bool IsInside(glm::dvec2 position, glm::dvec2 size, glm::dvec2 framePosition, glm::dvec2 frameSize)
		{
			return position.x >= framePosition.x && position.y >= framePosition.y && position.x + size.x <= framePosition.x + frameSize.x &&
			       position.y + size.y <= framePosition.y + frameSize.y;
		}

		// The number of pairs of links between two neighbouring columns that cross, the links given as the positions of their
		// ends in the left and the right column. Counts the inversions of the right ends once the links are sorted by their
		// ends, by merging: O(n log n), as graphs may hold thousands of links.
		uint64_t CountCrossings(std::vector<std::pair<size_t, size_t>>& links)
		{
			std::sort(links.begin(), links.end());
			std::vector<size_t> ends(links.size());
			for (size_t i = 0; i < links.size(); i++)
			{
				ends[i] = links[i].second;
			}
			std::vector<size_t> merged(ends.size());
			uint64_t crossings = 0;
			for (size_t width = 1; width < ends.size(); width *= 2)
			{
				for (size_t left = 0; left < ends.size(); left += 2 * width)
				{
					size_t const middle = std::min(left + width, ends.size());
					size_t const right = std::min(left + 2 * width, ends.size());
					size_t first = left;
					size_t second = middle;
					size_t out = left;
					while (first < middle && second < right)
					{
						if (ends[second] < ends[first])
						{
							crossings += middle - first;
							merged[out++] = ends[second++];
						}
						else
						{
							merged[out++] = ends[first++];
						}
					}
					while (first < middle)
					{
						merged[out++] = ends[first++];
					}
					while (second < right)
					{
						merged[out++] = ends[second++];
					}
				}
				std::swap(ends, merged);
			}
			return crossings;
		}

		// Orders the nodes of each column of a group for few crossings between neighbouring columns. Starting from the
		// columns as given (ID order), each sweep sorts the columns one after the other by the barycentre of each node's
		// neighbours in the column it comes from: down sweeps go left to right, from the nodes linked into a node, up sweeps
		// right to left, from the nodes it links to. Only links between neighbouring columns count; a node without such a
		// neighbour keeps its place as its key, and equal keys keep their order. The order with the fewest crossings wins,
		// the earliest of equals, so the result never crosses more than ID order does. With firstColumnFixed the first
		// column (the anchors) keeps its order and the others are ordered against it.
		void OrderColumns(Columns& columns, std::vector<std::vector<size_t>> const& previous, std::vector<std::vector<size_t>> const& next,
		                  size_t nodeCount, bool firstColumnFixed)
		{
			if (columns.size() < 2)
			{
				return;
			}
			std::vector<size_t> places(nodeCount, 0);
			auto const updatePlaces = [&places](std::vector<size_t> const& column)
			{
				for (size_t place = 0; place < column.size(); place++)
				{
					places[column[place]] = place;
				}
			};
			auto const countCrossings = [&]()
			{
				uint64_t crossings = 0;
				std::vector<std::pair<size_t, size_t>> links;
				for (size_t layer = 0; layer + 1 < columns.size(); layer++)
				{
					links.clear();
					for (size_t const node : columns[layer])
					{
						for (size_t const successor : next[node])
						{
							links.emplace_back(places[node], places[successor]);
						}
					}
					crossings += CountCrossings(links);
				}
				return crossings;
			};
			// Sorts one column by the barycentres of its nodes' neighbours, whose places are up to date.
			auto const sortColumn = [&places](std::vector<size_t>& column, std::vector<std::vector<size_t>> const& neighbours)
			{
				std::vector<std::pair<double, size_t>> keyed;
				keyed.reserve(column.size());
				for (size_t place = 0; place < column.size(); place++)
				{
					std::vector<size_t> const& adjacent = neighbours[column[place]];
					double key = static_cast<double>(place);
					if (!adjacent.empty())
					{
						size_t sum = 0;
						for (size_t const neighbour : adjacent)
						{
							sum += places[neighbour];
						}
						key = static_cast<double>(sum) / static_cast<double>(adjacent.size());
					}
					keyed.emplace_back(key, column[place]);
				}
				std::stable_sort(keyed.begin(), keyed.end(),
				                 [](std::pair<double, size_t> const& first, std::pair<double, size_t> const& second)
				                 {
									 return first.first < second.first;
								 });
				for (size_t place = 0; place < column.size(); place++)
				{
					column[place] = keyed[place].second;
				}
			};

			for (std::vector<size_t> const& column : columns)
			{
				updatePlaces(column);
			}
			Columns best = columns;
			uint64_t fewest = countCrossings();
			for (int sweep = 0; sweep < SweepCount && fewest > 0; sweep++)
			{
				if (sweep % 2 == 0)
				{
					for (size_t layer = 1; layer < columns.size(); layer++)
					{
						sortColumn(columns[layer], previous);
						updatePlaces(columns[layer]);
					}
				}
				else
				{
					size_t const lowest = firstColumnFixed ? 1 : 0;
					for (size_t layer = columns.size() - 1; layer-- > lowest;)
					{
						sortColumn(columns[layer], next);
						updatePlaces(columns[layer]);
					}
				}
				uint64_t const crossings = countCrossings();
				if (crossings < fewest)
				{
					fewest = crossings;
					best = columns;
				}
			}
			columns = std::move(best);
		}

		// A comment box to fit around the arranged nodes it holds.
		struct CommentRefit
		{
			size_t Comment = 0;
			// Indices into the arranged nodes.
			std::vector<size_t> Nodes;
		};

		// A placed node the arranged block stands beside.
		struct Anchor
		{
			uint32_t ID = 0;
			glm::dvec2 Position{0.0};
			glm::dvec2 Size{0.0};
		};
	}

	namespace VisualScriptEditing
	{
		Result<void> ArrangeNodes(VisualScriptDocument& document, uint32_t function, std::span<uint32_t const> nodes,
		                          VisualScriptNodeExtents const& extents, std::optional<glm::vec2> corner,
		                          std::span<uint32_t const> anchors)
		{
			if (corner && !anchors.empty())
			{
				return Error{"Arrange takes a corner or anchors, not both"};
			}
			if (corner && (!std::isfinite(corner->x) || !std::isfinite(corner->y)))
			{
				return Error{"the corner of the arranged nodes must be finite"};
			}
			Result<VisualScriptGraph*> found = FindGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			VisualScriptGraph& graph = *found.GetValue();

			// The first node with each ID: the one links lead to.
			std::unordered_map<uint32_t, size_t> indices;
			for (size_t index = 0; index < graph.Nodes.size(); index++)
			{
				indices.emplace(graph.Nodes[index].ID, index);
			}
			std::vector<uint32_t> ids;
			if (nodes.empty())
			{
				for (VisualScriptNode const& node : graph.Nodes)
				{
					ids.push_back(node.ID);
				}
			}
			else
			{
				for (uint32_t const id : nodes)
				{
					if (indices.contains(id))
					{
						ids.push_back(id);
						continue;
					}
					bool const isComment = std::any_of(graph.Comments.begin(), graph.Comments.end(),
					                                   [id](VisualScriptComment const& comment)
					                                   {
														   return comment.ID == id;
													   });
					if (isComment)
					{
						return MakeError("{} is a comment: Arrange lays out nodes", id);
					}
					return MakeError("the graph has no node {}", id);
				}
			}
			std::sort(ids.begin(), ids.end());
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

			// The anchors in ID order, each once, measured.
			std::vector<uint32_t> anchorIDs(anchors.begin(), anchors.end());
			std::sort(anchorIDs.begin(), anchorIDs.end());
			anchorIDs.erase(std::unique(anchorIDs.begin(), anchorIDs.end()), anchorIDs.end());
			std::vector<Anchor> anchorNodes;
			anchorNodes.reserve(anchorIDs.size());
			for (uint32_t const id : anchorIDs)
			{
				auto const index = indices.find(id);
				if (index == indices.end())
				{
					return MakeError("the graph has no node {}: an anchor is a node of the graph", id);
				}
				if (std::binary_search(ids.begin(), ids.end(), id))
				{
					return MakeError("node {} is arranged: an anchor keeps its place", id);
				}
				std::optional<glm::vec2> const& position = graph.Nodes[index->second].Position;
				if (!position)
				{
					return MakeError("node {} has no position: an anchor keeps its place", id);
				}
				Result<glm::dvec2> size = MeasureNode(extents, id);
				if (!size)
				{
					return Error{size.GetError()};
				}
				anchorNodes.push_back({id, glm::dvec2(*position), size.GetValue()});
			}
			if (ids.empty())
			{
				return {};
			}

			// The nodes in ID order, measured, and the corner of the bounds of those that have a position.
			std::vector<ArrangedNode> arranged;
			arranged.reserve(ids.size());
			std::unordered_map<uint32_t, size_t> numbers;
			std::optional<glm::dvec2> placedCorner;
			for (uint32_t const id : ids)
			{
				Result<glm::dvec2> size = MeasureNode(extents, id);
				if (!size)
				{
					return Error{size.GetError()};
				}
				size_t const index = indices.at(id);
				if (std::optional<glm::vec2> const& position = graph.Nodes[index].Position)
				{
					placedCorner = placedCorner ? glm::min(*placedCorner, glm::dvec2(*position)) : glm::dvec2(*position);
				}
				numbers.emplace(id, arranged.size());
				arranged.push_back({id, index, size.GetValue()});
			}

			// The comment boxes whose contents are arranged nodes only, read before anything moves.
			std::vector<CommentRefit> refits;
			for (size_t comment = 0; comment < graph.Comments.size(); comment++)
			{
				glm::dvec2 const framePosition(graph.Comments[comment].Position);
				glm::dvec2 const frameSize(graph.Comments[comment].Size);
				CommentRefit refit{comment, {}};
				bool onlyArranged = true;
				for (size_t index = 0; index < graph.Nodes.size(); index++)
				{
					VisualScriptNode const& node = graph.Nodes[index];
					if (!node.Position)
					{
						continue;
					}
					glm::dvec2 const position(*node.Position);
					if (!IsInside(position, glm::dvec2(0.0), framePosition, frameSize))
					{
						continue;
					}
					// A later node that repeats an ID (in a graph built in memory) is not the one arranged.
					auto const number = numbers.find(node.ID);
					bool const isArranged = number != numbers.end() && arranged[number->second].Index == index;
					glm::dvec2 size(0.0);
					if (isArranged)
					{
						size = arranged[number->second].Size;
					}
					else
					{
						Result<glm::dvec2> measured = MeasureNode(extents, node.ID);
						if (!measured)
						{
							return Error{measured.GetError()};
						}
						size = measured.GetValue();
					}
					if (!IsInside(position, size, framePosition, frameSize))
					{
						continue;
					}
					if (!isArranged)
					{
						onlyArranged = false;
						break;
					}
					refit.Nodes.push_back(number->second);
				}
				for (size_t other = 0; onlyArranged && other < graph.Comments.size(); other++)
				{
					if (other != comment && IsInside(glm::dvec2(graph.Comments[other].Position), glm::dvec2(graph.Comments[other].Size),
					                                 framePosition, frameSize))
					{
						onlyArranged = false;
					}
				}
				if (onlyArranged && !refit.Nodes.empty())
				{
					refits.push_back(std::move(refit));
				}
			}

			// Groups, columns and the links between neighbouring columns (each pair of nodes once).
			std::vector<VisualScriptLayerAssignment> const assignments = AssignVisualScriptLayers(graph, ids);
			uint32_t componentCount = 0;
			for (VisualScriptLayerAssignment const& assignment : assignments)
			{
				ArrangedNode& node = arranged[numbers.at(assignment.Node)];
				node.Component = assignment.Component;
				node.Layer = assignment.Layer;
				componentCount = std::max(componentCount, assignment.Component + 1);
			}
			std::vector<Columns> groups(componentCount);
			// The assignments are in ID order, so each column fills in its ID order: the order of Row.
			for (VisualScriptLayerAssignment const& assignment : assignments)
			{
				Columns& columns = groups[assignment.Component];
				if (columns.size() <= assignment.Layer)
				{
					columns.resize(assignment.Layer + 1);
				}
				columns[assignment.Layer].push_back(numbers.at(assignment.Node));
			}
			// The anchors' column, top to bottom (then left to right, then by ID), numbered after the arranged nodes.
			std::sort(anchorNodes.begin(), anchorNodes.end(),
			          [](Anchor const& a, Anchor const& b)
			          {
						  return std::tie(a.Position.y, a.Position.x, a.ID) < std::tie(b.Position.y, b.Position.x, b.ID);
					  });
			std::vector<size_t> anchorColumn;
			std::unordered_map<uint32_t, size_t> anchorNumbers;
			for (Anchor const& anchor : anchorNodes)
			{
				anchorNumbers.emplace(anchor.ID, arranged.size() + anchorColumn.size());
				anchorColumn.push_back(arranged.size() + anchorColumn.size());
			}
			size_t const nodeCount = arranged.size() + anchorColumn.size();
			std::vector<std::vector<size_t>> previous(nodeCount);
			std::vector<std::vector<size_t>> next(nodeCount);
			for (VisualScriptLink const& link : graph.Links)
			{
				auto const to = numbers.find(link.To.Node);
				if (to == numbers.end())
				{
					continue;
				}
				// A link from an anchor neighbours the block's first column only.
				if (auto const anchor = anchorNumbers.find(link.From.Node); anchor != anchorNumbers.end())
				{
					if (arranged[to->second].Layer == 0)
					{
						previous[to->second].push_back(anchor->second);
						next[anchor->second].push_back(to->second);
					}
					continue;
				}
				auto const from = numbers.find(link.From.Node);
				if (from == numbers.end())
				{
					continue;
				}
				size_t left = from->second;
				size_t right = to->second;
				if (arranged[left].Layer > arranged[right].Layer)
				{
					std::swap(left, right);
				}
				if (arranged[left].Layer + 1 != arranged[right].Layer)
				{
					continue;
				}
				previous[right].push_back(left);
				next[left].push_back(right);
			}
			for (std::vector<size_t>& list : previous)
			{
				std::sort(list.begin(), list.end());
				list.erase(std::unique(list.begin(), list.end()), list.end());
			}
			for (std::vector<size_t>& list : next)
			{
				std::sort(list.begin(), list.end());
				list.erase(std::unique(list.begin(), list.end()), list.end());
			}

			// Coordinates relative to the corner: each group below the one before it, its columns left to right. The anchors
			// stand before each group's first column while its columns are ordered.
			double top = 0.0;
			for (Columns& columns : groups)
			{
				if (anchorColumn.empty())
				{
					OrderColumns(columns, previous, next, nodeCount, false);
				}
				else
				{
					columns.insert(columns.begin(), anchorColumn);
					OrderColumns(columns, previous, next, nodeCount, true);
					columns.erase(columns.begin());
				}
				double left = 0.0;
				double bottom = top;
				for (std::vector<size_t> const& column : columns)
				{
					double width = 0.0;
					double y = top;
					for (size_t const node : column)
					{
						arranged[node].Offset = glm::dvec2(left, y);
						width = std::max(width, arranged[node].Size.x);
						bottom = std::max(bottom, y + arranged[node].Size.y);
						y = RoundUpToGrid(y + arranged[node].Size.y + RowGap);
					}
					left = RoundUpToGrid(left + width + ColumnGap);
				}
				top = RoundUpToGrid(bottom + GroupGap);
			}

			glm::dvec2 origin = placedCorner.value_or(glm::dvec2(0.0));
			if (corner)
			{
				origin = glm::dvec2(*corner);
			}
			else if (!anchorNodes.empty())
			{
				double right = std::numeric_limits<double>::lowest();
				double anchorTop = std::numeric_limits<double>::max();
				for (Anchor const& anchor : anchorNodes)
				{
					right = std::max(right, anchor.Position.x + anchor.Size.x);
					anchorTop = std::min(anchorTop, anchor.Position.y);
				}
				origin = glm::dvec2(RoundUpToGrid(right + ColumnGap), RoundUpToGrid(anchorTop));
			}
			for (ArrangedNode const& node : arranged)
			{
				glm::dvec2 const position = origin + node.Offset;
				graph.Nodes[node.Index].Position = glm::vec2(ToCoordinate(position.x), ToCoordinate(position.y));
			}
			for (CommentRefit const& refit : refits)
			{
				glm::dvec2 minimum(std::numeric_limits<double>::max());
				glm::dvec2 maximum(std::numeric_limits<double>::lowest());
				for (size_t const number : refit.Nodes)
				{
					glm::dvec2 const position(*graph.Nodes[arranged[number].Index].Position);
					minimum = glm::min(minimum, position);
					maximum = glm::max(maximum, position + arranged[number].Size);
				}
				minimum -= glm::dvec2(CommentFrameMargin, CommentFrameMargin + CommentFrameTitleSpace);
				maximum += glm::dvec2(CommentFrameMargin);
				VisualScriptComment& comment = graph.Comments[refit.Comment];
				comment.Position = glm::vec2(ToCoordinate(minimum.x), ToCoordinate(minimum.y));
				glm::dvec2 const size =
					glm::clamp(maximum - minimum, glm::dvec2(Format::MinCommentSize), glm::dvec2(Format::MaxCommentSize));
				comment.Size = glm::vec2(size);
			}
			return {};
		}

		glm::vec2 GetDefaultVisualScriptNodeExtents(uint32_t)
		{
			return DefaultNodeSize;
		}
	}
}
