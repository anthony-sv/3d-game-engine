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
			// Where the layout puts it, relative to the corner of the arranged block.
			glm::dvec2 Offset{0.0};
		};

		// What the layered layout places: an arranged node, or a region of comment boxes that stands for the nodes it frames.
		struct LayoutItem
		{
			uint32_t ID = 0;
			glm::dvec2 Size{0.0};
			uint32_t Component = 0;
			uint32_t Layer = 0;
			// Where the layout puts it, relative to the corner of the laid out items.
			glm::dvec2 Offset{0.0};
		};

		// The columns of one group of linked items: per layer, the group's items (indices into the laid out items) in their
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

		// Whether two ascending lists share an element.
		bool HaveCommon(std::vector<size_t> const& first, std::vector<size_t> const& second)
		{
			size_t i = 0;
			size_t j = 0;
			while (i < first.size() && j < second.size())
			{
				if (first[i] == second[j])
				{
					return true;
				}
				if (first[i] < second[j])
				{
					i++;
				}
				else
				{
					j++;
				}
			}
			return false;
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

		// A comment box that moves with the arranged nodes: it holds no node that is not arranged. Read before anything moves.
		struct FittedBox
		{
			size_t Comment = 0;
			glm::dvec2 Position{0.0};
			glm::dvec2 Size{0.0};
			// The arranged nodes inside it (indices into the arranged nodes, ascending).
			std::vector<size_t> Nodes;
			// Its frame in the coordinates of its block's items once laid out, then where it ends.
			glm::dvec2 FramePosition{0.0};
			glm::dvec2 FrameSize{0.0};
		};

		constexpr size_t NoBlock = std::numeric_limits<size_t>::max();

		// Fitted boxes whose contents move as one: a box, or boxes that overlap without one holding the other and hold a node
		// or a fitted box in common. Blocks nest as their boxes do: a block inside another is one of its items.
		struct Block
		{
			// Into the fitted boxes, ascending.
			std::vector<size_t> Boxes;
			size_t Parent = NoBlock;
			// The blocks it holds (ascending), the number of blocks it stands in, and the arranged nodes it holds that no block
			// inside it holds (ascending).
			std::vector<size_t> Children;
			size_t Depth = 0;
			std::vector<size_t> Nodes;
			// Laid out under the smallest ID of the nodes it holds, or, when it holds none, of its comment boxes, after the nodes.
			bool HoldsNodes = false;
			uint32_t SmallestID = std::numeric_limits<uint32_t>::max();
			// The bounds of its boxes' frames in its items' coordinates; where its items stand in its item, and the item's size;
			// the item's offset in the level it stands in, and its items' corner relative to the arranged block's.
			glm::dvec2 FrameMinimum{0.0};
			glm::dvec2 FrameMaximum{0.0};
			glm::dvec2 Inset{0.0};
			glm::dvec2 Size{0.0};
			glm::dvec2 Offset{0.0};
			glm::dvec2 Shift{0.0};
		};

		// A placed node the arranged block stands beside.
		struct Anchor
		{
			uint32_t ID = 0;
			glm::dvec2 Position{0.0};
			glm::dvec2 Size{0.0};
		};

		// The frame a comment box gets around nodes of the given bounds: CommentFrameMargin on every side and
		// CommentFrameTitleSpace more above, at least the format's smallest size.
		std::pair<glm::dvec2, glm::dvec2> FrameBounds(glm::dvec2 minimum, glm::dvec2 maximum)
		{
			glm::dvec2 const position =
				minimum - glm::dvec2(VisualScriptEditing::CommentFrameMargin,
			                         VisualScriptEditing::CommentFrameMargin + VisualScriptEditing::CommentFrameTitleSpace);
			glm::dvec2 const size = glm::clamp(maximum + glm::dvec2(VisualScriptEditing::CommentFrameMargin) - position,
			                                   glm::dvec2(Format::MinCommentSize), glm::dvec2(Format::MaxCommentSize));
			return {position, size};
		}

		// The layered layout: sets each item's Offset from the corner of their bounds. The items are nodes of graph with their
		// IDs, each once, in ID order; only the links of graph between them count, and those from the anchors (sorted top to
		// bottom, then left to right, then by ID) into a group's first column, for the order of its rows.
		void LayOutItems(VisualScriptGraph const& graph, std::vector<LayoutItem>& items, std::vector<Anchor> const& anchors)
		{
			std::unordered_map<uint32_t, size_t> numbers;
			std::vector<uint32_t> ids;
			ids.reserve(items.size());
			for (size_t number = 0; number < items.size(); number++)
			{
				numbers.emplace(items[number].ID, number);
				ids.push_back(items[number].ID);
			}

			// Groups, columns and the links between neighbouring columns (each pair of items once).
			std::vector<VisualScriptLayerAssignment> const assignments = AssignVisualScriptLayers(graph, ids);
			uint32_t componentCount = 0;
			for (VisualScriptLayerAssignment const& assignment : assignments)
			{
				LayoutItem& item = items[numbers.at(assignment.Node)];
				item.Component = assignment.Component;
				item.Layer = assignment.Layer;
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
			// The anchors' column, numbered after the items.
			std::vector<size_t> anchorColumn;
			std::unordered_map<uint32_t, size_t> anchorNumbers;
			for (Anchor const& anchor : anchors)
			{
				anchorNumbers.emplace(anchor.ID, items.size() + anchorColumn.size());
				anchorColumn.push_back(items.size() + anchorColumn.size());
			}
			size_t const nodeCount = items.size() + anchorColumn.size();
			std::vector<std::vector<size_t>> previous(nodeCount);
			std::vector<std::vector<size_t>> next(nodeCount);
			// The links from the anchors, (anchor, item) by group: each group is ordered against its own only.
			std::vector<std::vector<std::pair<size_t, size_t>>> anchorLinks(componentCount);
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
					if (items[to->second].Layer == 0)
					{
						previous[to->second].push_back(anchor->second);
						anchorLinks[items[to->second].Component].emplace_back(anchor->second, to->second);
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
				if (items[left].Layer > items[right].Layer)
				{
					std::swap(left, right);
				}
				if (items[left].Layer + 1 != items[right].Layer)
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
			for (size_t group = 0; group < groups.size(); group++)
			{
				Columns& columns = groups[group];
				if (anchorColumn.empty())
				{
					OrderColumns(columns, previous, next, nodeCount, false);
				}
				else
				{
					// The anchors lead into this group's first column only while it is ordered.
					for (auto const& [anchor, item] : anchorLinks[group])
					{
						next[anchor].push_back(item);
					}
					for (size_t const anchor : anchorColumn)
					{
						std::sort(next[anchor].begin(), next[anchor].end());
						next[anchor].erase(std::unique(next[anchor].begin(), next[anchor].end()), next[anchor].end());
					}
					columns.insert(columns.begin(), anchorColumn);
					OrderColumns(columns, previous, next, nodeCount, true);
					columns.erase(columns.begin());
					for (size_t const anchor : anchorColumn)
					{
						next[anchor].clear();
					}
				}
				double left = 0.0;
				double bottom = top;
				for (std::vector<size_t> const& column : columns)
				{
					double width = 0.0;
					double y = top;
					for (size_t const item : column)
					{
						items[item].Offset = glm::dvec2(left, y);
						width = std::max(width, items[item].Size.x);
						bottom = std::max(bottom, y + items[item].Size.y);
						y = RoundUpToGrid(y + items[item].Size.y + RowGap);
					}
					left = RoundUpToGrid(left + width + ColumnGap);
				}
				top = RoundUpToGrid(bottom + GroupGap);
			}
		}
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

			// The comment boxes that move with the arranged nodes, read before anything moves: those that hold no node that is not
			// arranged and hold an arranged node, stand in such a box, or are boxes of a graph whose every node is arranged.
			bool const wholeGraph = arranged.size() == graph.Nodes.size();
			std::vector<FittedBox> candidates;
			for (size_t comment = 0; comment < graph.Comments.size(); comment++)
			{
				FittedBox box;
				box.Comment = comment;
				box.Position = glm::dvec2(graph.Comments[comment].Position);
				box.Size = glm::dvec2(graph.Comments[comment].Size);
				bool onlyArranged = true;
				for (size_t index = 0; index < graph.Nodes.size(); index++)
				{
					VisualScriptNode const& node = graph.Nodes[index];
					if (!node.Position)
					{
						continue;
					}
					glm::dvec2 const position(*node.Position);
					if (!IsInside(position, glm::dvec2(0.0), box.Position, box.Size))
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
					if (!IsInside(position, size, box.Position, box.Size))
					{
						continue;
					}
					if (!isArranged)
					{
						onlyArranged = false;
						break;
					}
					box.Nodes.push_back(number->second);
				}
				if (onlyArranged)
				{
					std::sort(box.Nodes.begin(), box.Nodes.end());
					candidates.push_back(std::move(box));
				}
			}
			std::vector<FittedBox> fitted;
			for (FittedBox const& box : candidates)
			{
				bool moves = wholeGraph || !box.Nodes.empty();
				for (size_t other = 0; !moves && other < candidates.size(); other++)
				{
					moves = !candidates[other].Nodes.empty() &&
					        IsInside(box.Position, box.Size, candidates[other].Position, candidates[other].Size);
				}
				if (moves)
				{
					fitted.push_back(box);
				}
			}

			// Which fitted box stands in which (a box stands in itself, and two boxes of one size in each other).
			size_t const boxCount = fitted.size();
			std::vector<char> inside(boxCount * boxCount, 0);
			for (size_t box = 0; box < boxCount; box++)
			{
				for (size_t other = 0; other < boxCount; other++)
				{
					inside[box * boxCount + other] =
						IsInside(fitted[box].Position, fitted[box].Size, fitted[other].Position, fitted[other].Size) ? 1 : 0;
				}
			}
			auto const isInside = [&inside, boxCount](size_t box, size_t other)
			{
				return inside[box * boxCount + other] != 0;
			};
			auto const isStrictlyInside = [&isInside](size_t box, size_t other)
			{
				return isInside(box, other) && !isInside(other, box);
			};

			// The blocks. Boxes that overlap without one holding the other and hold a node or a box in common are joined, since
			// what both hold must stay in both. Then everything a block holds stands in one box of each block it stands in, so
			// the blocks nest as a forest.
			std::vector<size_t> parents(boxCount);
			for (size_t box = 0; box < boxCount; box++)
			{
				parents[box] = box;
			}
			auto const findRoot = [&parents](size_t box)
			{
				while (parents[box] != box)
				{
					parents[box] = parents[parents[box]];
					box = parents[box];
				}
				return box;
			};
			for (size_t first = 0; first < boxCount; first++)
			{
				for (size_t second = first + 1; second < boxCount; second++)
				{
					// Boxes apart hold nothing in common.
					glm::dvec2 const firstMaximum = fitted[first].Position + fitted[first].Size;
					glm::dvec2 const secondMaximum = fitted[second].Position + fitted[second].Size;
					if (glm::any(glm::greaterThan(fitted[first].Position, secondMaximum)) ||
					    glm::any(glm::greaterThan(fitted[second].Position, firstMaximum)) || isStrictlyInside(first, second) ||
					    isStrictlyInside(second, first))
					{
						continue;
					}
					bool shared = HaveCommon(fitted[first].Nodes, fitted[second].Nodes);
					for (size_t other = 0; !shared && other < boxCount; other++)
					{
						shared = other != first && other != second && isInside(other, first) && isInside(other, second);
					}
					if (shared)
					{
						size_t const firstRoot = findRoot(first);
						size_t const secondRoot = findRoot(second);
						parents[std::max(firstRoot, secondRoot)] = std::min(firstRoot, secondRoot);
					}
				}
			}
			std::vector<Block> blocks;
			std::vector<size_t> blockOf(boxCount, NoBlock);
			{
				std::vector<size_t> blockOfRoot(boxCount, NoBlock);
				for (size_t box = 0; box < boxCount; box++)
				{
					size_t const root = findRoot(box);
					if (blockOfRoot[root] == NoBlock)
					{
						blockOfRoot[root] = blocks.size();
						blocks.emplace_back();
					}
					blockOf[box] = blockOfRoot[root];
					blocks[blockOf[box]].Boxes.push_back(box);
				}
			}
			// A block stands in another when its first box stands strictly in one of the other's. The blocks one stands in form
			// a chain, so the one that holds it directly is the one with most blocks around it.
			auto const standsIn = [&blocks, &isStrictlyInside](size_t block, size_t other)
			{
				size_t const first = blocks[block].Boxes.front();
				return std::any_of(blocks[other].Boxes.begin(), blocks[other].Boxes.end(),
				                   [&isStrictlyInside, first](size_t box)
				                   {
									   return isStrictlyInside(first, box);
								   });
			};
			std::vector<std::vector<size_t>> around(blocks.size());
			for (size_t block = 0; block < blocks.size(); block++)
			{
				for (size_t other = 0; other < blocks.size(); other++)
				{
					if (other != block && standsIn(block, other))
					{
						around[block].push_back(other);
					}
				}
				blocks[block].Depth = around[block].size();
			}
			for (size_t block = 0; block < blocks.size(); block++)
			{
				for (size_t const other : around[block])
				{
					if (blocks[block].Parent == NoBlock || blocks[other].Depth > blocks[blocks[block].Parent].Depth)
					{
						blocks[block].Parent = other;
					}
				}
				if (blocks[block].Parent != NoBlock)
				{
					blocks[blocks[block].Parent].Children.push_back(block);
				}
			}
			// Each arranged node belongs to the innermost block holding it: the blocks holding it form a chain too.
			std::vector<size_t> innermost(arranged.size(), NoBlock);
			for (size_t box = 0; box < boxCount; box++)
			{
				size_t const block = blockOf[box];
				for (size_t const number : fitted[box].Nodes)
				{
					if (innermost[number] == NoBlock || blocks[block].Depth > blocks[innermost[number]].Depth)
					{
						innermost[number] = block;
					}
				}
			}
			for (size_t number = 0; number < arranged.size(); number++)
			{
				if (innermost[number] != NoBlock)
				{
					blocks[innermost[number]].Nodes.push_back(number);
				}
			}
			// Inner blocks first, and the ID each block is laid out under.
			std::vector<size_t> innerFirst(blocks.size());
			for (size_t block = 0; block < blocks.size(); block++)
			{
				innerFirst[block] = block;
			}
			std::stable_sort(innerFirst.begin(), innerFirst.end(),
			                 [&blocks](size_t first, size_t second)
			                 {
								 return blocks[first].Depth > blocks[second].Depth;
							 });
			for (size_t const number : innerFirst)
			{
				Block& block = blocks[number];
				for (size_t const node : block.Nodes)
				{
					block.HoldsNodes = true;
					block.SmallestID = std::min(block.SmallestID, arranged[node].ID);
				}
				for (size_t const child : block.Children)
				{
					if (blocks[child].HoldsNodes)
					{
						block.HoldsNodes = true;
						block.SmallestID = std::min(block.SmallestID, blocks[child].SmallestID);
					}
				}
				if (!block.HoldsNodes)
				{
					for (size_t const box : block.Boxes)
					{
						block.SmallestID = std::min(block.SmallestID, graph.Comments[fitted[box].Comment].ID);
					}
					for (size_t const child : block.Children)
					{
						block.SmallestID = std::min(block.SmallestID, blocks[child].SmallestID);
					}
				}
			}

			// Lays out one level, a block's items or (NoBlock) the arranged block's: its nodes and the blocks standing in it, in
			// ID order, the blocks that hold no node after the nodes. Sets the offsets of the nodes and of the blocks in the
			// level's coordinates.
			std::vector<size_t> nodePlaces(arranged.size(), 0);
			std::vector<size_t> blockPlaces(blocks.size(), 0);
			auto const layOutLevel = [&](size_t level, std::vector<size_t> const& levelNodes, std::vector<size_t> const& levelBlocks,
			                             std::vector<Anchor> const& levelAnchors)
			{
				if (levelBlocks.empty())
				{
					std::vector<LayoutItem> items;
					items.reserve(levelNodes.size());
					for (size_t const number : levelNodes)
					{
						items.push_back({arranged[number].ID, arranged[number].Size});
					}
					LayOutItems(graph, items, levelAnchors);
					for (size_t place = 0; place < levelNodes.size(); place++)
					{
						arranged[levelNodes[place]].Offset = items[place].Offset;
					}
					return;
				}
				// The items as the nodes of a graph of their own, numbered in their order and linked as their nodes are (and from
				// the anchors, numbered after them).
				using Key = std::tuple<bool, uint32_t, bool, size_t>;
				std::vector<Key> keys;
				keys.reserve(levelNodes.size() + levelBlocks.size());
				for (size_t const number : levelNodes)
				{
					keys.emplace_back(false, arranged[number].ID, false, number);
				}
				for (size_t const block : levelBlocks)
				{
					keys.emplace_back(!blocks[block].HoldsNodes, blocks[block].SmallestID, true, block);
				}
				std::sort(keys.begin(), keys.end());
				VisualScriptGraph itemGraph;
				std::vector<LayoutItem> items(keys.size());
				itemGraph.Nodes.resize(keys.size());
				for (size_t place = 0; place < keys.size(); place++)
				{
					bool const isBlock = std::get<2>(keys[place]);
					size_t const index = std::get<3>(keys[place]);
					items[place].ID = static_cast<uint32_t>(place + 1);
					items[place].Size = isBlock ? blocks[index].Size : arranged[index].Size;
					itemGraph.Nodes[place].ID = items[place].ID;
					(isBlock ? blockPlaces : nodePlaces)[index] = place;
				}
				std::vector<Anchor> anchorItems = levelAnchors;
				std::unordered_map<uint32_t, uint32_t> anchorItemIDs;
				for (size_t anchor = 0; anchor < anchorItems.size(); anchor++)
				{
					uint32_t const id = static_cast<uint32_t>(keys.size() + anchor + 1);
					anchorItemIDs.emplace(anchorItems[anchor].ID, id);
					anchorItems[anchor].ID = id;
				}
				// The item of the level an arranged node stands in, if it stands in the level.
				auto const findItem = [&](size_t number) -> std::optional<uint32_t>
				{
					size_t block = innermost[number];
					if (block == level)
					{
						return items[nodePlaces[number]].ID;
					}
					while (block != NoBlock && blocks[block].Parent != level)
					{
						block = blocks[block].Parent;
					}
					if (block == NoBlock)
					{
						return std::nullopt;
					}
					return items[blockPlaces[block]].ID;
				};
				for (VisualScriptLink const& link : graph.Links)
				{
					auto const to = numbers.find(link.To.Node);
					if (to == numbers.end())
					{
						continue;
					}
					std::optional<uint32_t> const target = findItem(to->second);
					if (!target)
					{
						continue;
					}
					std::optional<uint32_t> source;
					if (auto const from = numbers.find(link.From.Node); from != numbers.end())
					{
						source = findItem(from->second);
					}
					else if (auto const anchor = anchorItemIDs.find(link.From.Node); anchor != anchorItemIDs.end())
					{
						source = anchor->second;
					}
					if (source && *source != *target)
					{
						itemGraph.Links.push_back({{*source, {}}, {*target, {}}});
					}
				}
				LayOutItems(itemGraph, items, anchorItems);
				for (size_t place = 0; place < keys.size(); place++)
				{
					size_t const index = std::get<3>(keys[place]);
					(std::get<2>(keys[place]) ? blocks[index].Offset : arranged[index].Offset) = items[place].Offset;
				}
			};

			// Each block laid out on its own, inner blocks first, and its boxes framed as new comment boxes framing what they
			// hold would be: their nodes and the blocks inside them. A box that holds nothing keeps its size.
			for (size_t const number : innerFirst)
			{
				layOutLevel(number, blocks[number].Nodes, blocks[number].Children, {});
				Block& block = blocks[number];
				block.FrameMinimum = glm::dvec2(std::numeric_limits<double>::max());
				block.FrameMaximum = glm::dvec2(std::numeric_limits<double>::lowest());
				for (size_t const box : block.Boxes)
				{
					FittedBox& frame = fitted[box];
					glm::dvec2 minimum(std::numeric_limits<double>::max());
					glm::dvec2 maximum(std::numeric_limits<double>::lowest());
					bool holds = false;
					for (size_t const node : frame.Nodes)
					{
						if (innermost[node] == number)
						{
							minimum = glm::min(minimum, arranged[node].Offset);
							maximum = glm::max(maximum, arranged[node].Offset + arranged[node].Size);
							holds = true;
						}
					}
					for (size_t const child : block.Children)
					{
						Block const& held = blocks[child];
						if (isInside(held.Boxes.front(), box))
						{
							minimum = glm::min(minimum, held.Offset + held.Inset + held.FrameMinimum);
							maximum = glm::max(maximum, held.Offset + held.Inset + held.FrameMaximum);
							holds = true;
						}
					}
					if (holds)
					{
						std::tie(frame.FramePosition, frame.FrameSize) = FrameBounds(minimum, maximum);
					}
					else
					{
						frame.FramePosition = glm::dvec2(0.0);
						frame.FrameSize = frame.Size;
					}
					block.FrameMinimum = glm::min(block.FrameMinimum, frame.FramePosition);
					block.FrameMaximum = glm::max(block.FrameMaximum, frame.FramePosition + frame.FrameSize);
				}
				// The inset keeps the block's nodes on the grid of the items' corners; the frames stand within the item.
				block.Inset = glm::dvec2(RoundUpToGrid(-block.FrameMinimum.x), RoundUpToGrid(-block.FrameMinimum.y));
				block.Size = block.Inset + block.FrameMaximum;
			}

			// The anchors' column, top to bottom (then left to right, then by ID).
			std::sort(anchorNodes.begin(), anchorNodes.end(),
			          [](Anchor const& a, Anchor const& b)
			          {
						  return std::tie(a.Position.y, a.Position.x, a.ID) < std::tie(b.Position.y, b.Position.x, b.ID);
					  });
			// The arranged block: the nodes in no box that moves and the outermost blocks. Then every node's offset from its
			// corner.
			std::vector<size_t> topNodes;
			std::vector<size_t> topBlocks;
			for (size_t number = 0; number < arranged.size(); number++)
			{
				if (innermost[number] == NoBlock)
				{
					topNodes.push_back(number);
				}
			}
			for (size_t block = 0; block < blocks.size(); block++)
			{
				if (blocks[block].Parent == NoBlock)
				{
					topBlocks.push_back(block);
				}
			}
			layOutLevel(NoBlock, topNodes, topBlocks, anchorNodes);
			for (auto number = innerFirst.rbegin(); number != innerFirst.rend(); ++number)
			{
				Block& block = blocks[*number];
				glm::dvec2 const outer = block.Parent == NoBlock ? glm::dvec2(0.0) : blocks[block.Parent].Shift;
				block.Shift = outer + block.Offset + block.Inset;
			}
			for (size_t number = 0; number < arranged.size(); number++)
			{
				if (innermost[number] != NoBlock)
				{
					arranged[number].Offset += blocks[innermost[number]].Shift;
				}
			}

			// By default and with a corner, the arranged nodes' top-left corner is given (the boxes' frames reach above and left
			// of it); with anchors, the block's corner.
			glm::dvec2 nodesCorner(std::numeric_limits<double>::max());
			for (ArrangedNode const& node : arranged)
			{
				nodesCorner = glm::min(nodesCorner, node.Offset);
			}
			glm::dvec2 origin = placedCorner.value_or(glm::dvec2(0.0)) - nodesCorner;
			if (corner)
			{
				origin = glm::dvec2(*corner) - nodesCorner;
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
			// The boxes framed again around where their nodes and inner boxes now stand, inner blocks first. A box that holds
			// nothing moves with its block.
			for (size_t const number : innerFirst)
			{
				Block const& block = blocks[number];
				for (size_t const box : block.Boxes)
				{
					FittedBox& frame = fitted[box];
					glm::dvec2 minimum(std::numeric_limits<double>::max());
					glm::dvec2 maximum(std::numeric_limits<double>::lowest());
					bool holds = false;
					for (size_t const node : frame.Nodes)
					{
						glm::dvec2 const position(*graph.Nodes[arranged[node].Index].Position);
						minimum = glm::min(minimum, position);
						maximum = glm::max(maximum, position + arranged[node].Size);
						holds = true;
					}
					for (size_t const child : block.Children)
					{
						if (!isInside(blocks[child].Boxes.front(), box))
						{
							continue;
						}
						for (size_t const held : blocks[child].Boxes)
						{
							minimum = glm::min(minimum, fitted[held].FramePosition);
							maximum = glm::max(maximum, fitted[held].FramePosition + fitted[held].FrameSize);
							holds = true;
						}
					}
					if (holds)
					{
						std::tie(frame.FramePosition, frame.FrameSize) = FrameBounds(minimum, maximum);
					}
					else
					{
						frame.FramePosition = origin + block.Shift;
					}
					VisualScriptComment& comment = graph.Comments[frame.Comment];
					comment.Position = glm::vec2(ToCoordinate(frame.FramePosition.x), ToCoordinate(frame.FramePosition.y));
					comment.Size = glm::vec2(frame.FrameSize);
					frame.FramePosition = glm::dvec2(comment.Position);
					frame.FrameSize = glm::dvec2(comment.Size);
				}
			}
			return {};
		}

		glm::vec2 GetDefaultVisualScriptNodeExtents(uint32_t)
		{
			return DefaultNodeSize;
		}
	}
}
