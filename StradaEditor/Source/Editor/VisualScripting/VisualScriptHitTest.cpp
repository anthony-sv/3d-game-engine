#include "Editor/VisualScripting/VisualScriptHitTest.h"

#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"

#include <algorithm>
#include <limits>

namespace Strada
{
	namespace
	{
		namespace Metrics = VisualScriptCanvasMetrics;
		using Item = VisualScriptSpatialIndex::ItemKind;

		bool IsSelected(VisualScriptHitContext const& context, uint32_t node)
		{
			return node < context.Selected.size() && context.Selected[node] != 0;
		}

		uint32_t GetRaise(VisualScriptHitContext const& context, uint32_t node)
		{
			return node < context.Z.size() ? context.Z[node] : 0u;
		}

		// The node as drawn this frame: expanded under a dragged wire, or not.
		VisualScriptNodeVisual GetDrawnNode(VisualScriptCanvasModel const& model, VisualScriptHitContext const& context, uint32_t node)
		{
			std::span<VisualScriptNodeVisual const> const nodes = model.GetNodes();
			VisualScriptNodeVisual const& visual = nodes[node];
			return context.Preview.Expanded == node ? VisualScriptGeometry::GetExpandedNode(visual) : visual;
		}

		VisualScriptHit MakeNodeHit(VisualScriptHitKind kind, VisualScriptNodeVisual const& node)
		{
			VisualScriptHit hit;
			hit.Kind = kind;
			hit.Element = node.ID;
			return hit;
		}

		// The hit test, a wire's hit named by its index in the visual model (foundLink) instead of its document link: building
		// that is the caller's.
		VisualScriptHit HitTestWithoutLink(VisualScriptCanvasModel const& model, VisualScriptSpatialIndex const& index,
		                                   VisualScriptView const& view, VisualScriptHitContext const& context, glm::vec2 mouse,
		                                   std::optional<uint32_t>& foundLink)
		{
			if (context.Minimap && VisualScriptGeometry::Contains(*context.Minimap, mouse))
			{
				VisualScriptHit hit;
				hit.Kind = VisualScriptHitKind::Minimap;
				return hit;
			}
			VisualScriptHit background;
			background.Kind = VisualScriptHitKind::Background;
			float const pixelsPerUnit = view.GetPixelsPerUnit();
			float const uiScale = view.GetUIScale();
			if (!(pixelsPerUnit > 0.0f))
			{
				return background;
			}
			VisualScriptDetailLevel const level = view.GetDetailLevel();
			glm::vec2 const point = view.ToCanvas(mouse);
			float const unitsPerLogicalPixel = uiScale / pixelsPerUnit;
			float const pinSlop = Metrics::PinSlopLogicalPixels * unitsPerLogicalPixel;
			float const rerouteRadius = Metrics::RerouteHitRadiusLogicalPixels * unitsPerLogicalPixel;
			float const commentTolerance = Metrics::CommentHandleLogicalPixels * unitsPerLogicalPixel;
			float const wireTolerance = Metrics::WireHitLogicalPixels * uiScale;
			float const radius = std::max({pinSlop, rerouteRadius, commentTolerance, wireTolerance / pixelsPerUnit,
			                               VisualScriptGeometry::GetBadgeRadius(pixelsPerUnit, uiScale)});

			VisualScriptHitScratch local;
			VisualScriptHitScratch& scratch = context.Scratch != nullptr ? *context.Scratch : local;
			std::vector<uint32_t>& candidates = scratch.Candidates;
			index.QueryPoint(point, radius, VisualScriptSpatialIndex::AllMask, candidates, context.Stats);
			std::span<VisualScriptCommentVisual const> const comments = model.GetComments();

			// At Far and Overview comment titles are drawn on plates above the nodes, so they are hit first: the smallest comment's,
			// as the plates are drawn largest comment first. A plate lies inside its comment's box, which the query found.
			if (level == VisualScriptDetailLevel::Far || level == VisualScriptDetailLevel::Overview)
			{
				std::optional<uint32_t> labelComment;
				for (uint32_t const item : candidates)
				{
					if (VisualScriptSpatialIndex::GetKind(item) != Item::Comment)
					{
						continue;
					}
					uint32_t const comment = VisualScriptSpatialIndex::GetIndex(item);
					VisualScriptCommentVisual const& visual = comments[comment];
					if ((!labelComment || comment > *labelComment) &&
					    VisualScriptGeometry::Contains(VisualScriptGeometry::GetCommentLabelRect(visual, view, visual.Title.Width), mouse))
					{
						labelComment = comment;
					}
				}
				if (labelComment)
				{
					// A plate reaches into its comment's resize handles at the top-left corner: there the handles keep the mouse.
					VisualScriptHit hit;
					hit.Element = comments[*labelComment].ID;
					hit.Edges = VisualScriptGeometry::GetCommentEdges(comments[*labelComment], point, commentTolerance);
					hit.Kind = hit.Edges != 0 ? VisualScriptHitKind::CommentEdge : VisualScriptHitKind::CommentTitle;
					return hit;
				}
			}

			// Nodes near the mouse, top drawn first: the index's, the moved ones (dynamic: at their preview places) and the one
			// drawn expanded (larger than the bounds the index has).
			std::span<VisualScriptNodeVisual const> const nodes = model.GetNodes();
			std::vector<uint64_t>& order = scratch.Nodes;
			order.clear();
			auto const addNode = [&](uint32_t node)
			{
				if (context.SkipMoved && context.Preview.IsMoved(node))
				{
					return;
				}
				order.push_back(GetVisualScriptNodeDrawKey(IsSelected(context, node), GetRaise(context, node), node));
			};
			for (uint32_t const item : candidates)
			{
				if (VisualScriptSpatialIndex::GetKind(item) == Item::Node)
				{
					addNode(VisualScriptSpatialIndex::GetIndex(item));
				}
			}
			for (uint32_t node = 0; node < context.Preview.Moved.size() && node < nodes.size() && !context.SkipMoved; node++)
			{
				if (context.Preview.IsMoved(node))
				{
					addNode(node);
				}
			}
			if (context.Preview.Expanded && *context.Preview.Expanded < nodes.size())
			{
				addNode(*context.Preview.Expanded);
			}
			std::sort(order.begin(), order.end(), std::greater<uint64_t>());
			order.erase(std::unique(order.begin(), order.end()), order.end());
			auto const getNodeIndex = [](uint64_t key)
			{
				return static_cast<uint32_t>(key & 0xFFFFFFFFull);
			};

			// A pin of the topmost node whose rectangle, grown by the pin slop, holds the mouse: the nearest one whose row (where
			// labels are drawn) or anchor area holds it.
			if (level != VisualScriptDetailLevel::Far)
			{
				bool const labelRows = VisualScriptGeometry::ArePinLabelsDrawn(level, view.GetState().Zoom);
				for (uint64_t const key : order)
				{
					uint32_t const nodeIndex = getNodeIndex(key);
					VisualScriptNodeVisual const node = GetDrawnNode(model, context, nodeIndex);
					if (node.Shape == VisualScriptNodeShape::Reroute)
					{
						continue;
					}
					glm::vec2 const offset = context.Preview.GetOffset(nodeIndex);
					if (!VisualScriptGeometry::Contains(
							VisualScriptGeometry::Expand(VisualScriptGeometry::GetNodeRect(node, offset), pinSlop), point))
					{
						continue;
					}
					std::span<VisualScriptPinVisual const> const pins = model.GetPins(node);
					std::optional<uint32_t> nearest;
					float nearestDistance = std::numeric_limits<float>::infinity();
					for (uint32_t pin = 0; pin < pins.size(); pin++)
					{
						if (!VisualScriptGeometry::Contains(
								VisualScriptGeometry::GetPinHitRect(node, pins[pin], pinSlop, offset, labelRows), point))
						{
							continue;
						}
						float const distance = glm::length(VisualScriptGeometry::GetPinAnchor(node, pins[pin], offset) - point);
						if (distance < nearestDistance)
						{
							nearestDistance = distance;
							nearest = node.FirstPin + pin;
						}
					}
					if (nearest)
					{
						VisualScriptHit hit = MakeNodeHit(VisualScriptHitKind::Pin, node);
						hit.Pin = *nearest;
						return hit;
					}
					break;
				}
			}

			// The topmost node under the mouse: its badge, its header or its body, and the bubble of its comment where it is drawn
			// with its text.
			bool const bubbles = level == VisualScriptDetailLevel::Compact || level == VisualScriptDetailLevel::Detail;
			for (uint64_t const key : order)
			{
				uint32_t const nodeIndex = getNodeIndex(key);
				VisualScriptNodeVisual const node = GetDrawnNode(model, context, nodeIndex);
				if (node.Shape == VisualScriptNodeShape::Reroute)
				{
					continue;
				}
				glm::vec2 const offset = context.Preview.GetOffset(nodeIndex);
				if (node.ErrorCount > 0 || node.WarningCount > 0)
				{
					float const badgeRadius = VisualScriptGeometry::GetBadgeRadius(pixelsPerUnit, uiScale);
					for (std::optional<glm::vec2> const center :
					     {VisualScriptGeometry::GetErrorBadgeCenter(node, offset),
					      VisualScriptGeometry::GetWarningBadgeCenter(node, pixelsPerUnit, uiScale, offset)})
					{
						if (center && glm::length(point - *center) <= badgeRadius)
						{
							return MakeNodeHit(VisualScriptHitKind::Badge, node);
						}
					}
				}
				if (VisualScriptGeometry::Contains(VisualScriptGeometry::GetNodeRect(node, offset), point))
				{
					bool const header = VisualScriptGeometry::Contains(VisualScriptGeometry::GetHeaderRect(node, offset), point);
					return MakeNodeHit(header ? VisualScriptHitKind::NodeHeader : VisualScriptHitKind::Node, node);
				}
				VisualScriptRect const bubble = VisualScriptGeometry::GetBubbleRect(node, offset);
				if (bubbles && !VisualScriptGeometry::IsEmpty(bubble) && VisualScriptGeometry::Contains(bubble, point))
				{
					return MakeNodeHit(VisualScriptHitKind::Node, node);
				}
			}

			// The nearest reroute dot within its hit radius.
			std::optional<uint32_t> reroute;
			float rerouteDistance = std::numeric_limits<float>::infinity();
			for (uint64_t const key : order)
			{
				uint32_t const nodeIndex = getNodeIndex(key);
				VisualScriptNodeVisual const& node = nodes[nodeIndex];
				if (node.Shape != VisualScriptNodeShape::Reroute)
				{
					continue;
				}
				float const distance =
					glm::length(VisualScriptGeometry::GetRerouteCenter(node, context.Preview.GetOffset(nodeIndex)) - point);
				if (distance <= rerouteRadius && distance < rerouteDistance)
				{
					rerouteDistance = distance;
					reroute = nodeIndex;
				}
			}
			if (reroute)
			{
				return MakeNodeHit(VisualScriptHitKind::Reroute, nodes[*reroute]);
			}

			// The nearest wire within its tolerance of the polyline drawn at this level. Dynamic wires are tested here at their
			// preview places.
			if (level != VisualScriptDetailLevel::Far)
			{
				VisualScriptGeometry::WireFlattening const flattening =
					VisualScriptGeometry::GetWireFlattening(level, context.CurveTessellationTolerance);
				std::optional<uint32_t> nearest;
				VisualScriptGeometry::WireDistance nearestDistance{std::numeric_limits<float>::infinity(), 0.0f};
				auto const testLink = [&](uint32_t link)
				{
					std::array<glm::vec2, 4> const points = VisualScriptGeometry::GetLinkControlPoints(model, link, context.Preview);
					if (!VisualScriptGeometry::Contains(
							VisualScriptGeometry::Expand(VisualScriptGeometry::GetWireBounds(points), wireTolerance / pixelsPerUnit),
							point))
					{
						return;
					}
					std::array<glm::vec2, 4> const screen = {view.ToScreen(points[0]), view.ToScreen(points[1]), view.ToScreen(points[2]),
					                                         view.ToScreen(points[3])};
					VisualScriptGeometry::FlattenWire(screen, flattening.Tolerance, flattening.SegmentCap, scratch.Polyline);
					VisualScriptGeometry::WireDistance const distance = VisualScriptGeometry::MeasureDistance(scratch.Polyline, mouse);
					if (distance.Distance <= wireTolerance && distance.Distance < nearestDistance.Distance)
					{
						nearestDistance = distance;
						nearest = link;
					}
				};
				for (uint32_t const item : candidates)
				{
					if (VisualScriptSpatialIndex::GetKind(item) == Item::Link)
					{
						testLink(VisualScriptSpatialIndex::GetIndex(item));
					}
				}
				std::span<VisualScriptLinkVisual const> const links = model.GetLinks();
				for (uint32_t const link : context.DynamicLinks)
				{
					if (link >= links.size() || (context.SkipMoved && (context.Preview.IsMoved(links[link].FromNode) ||
					                                                   context.Preview.IsMoved(links[link].ToNode))))
					{
						continue;
					}
					testLink(link);
				}
				if (nearest)
				{
					VisualScriptHit hit;
					hit.Kind = VisualScriptHitKind::Link;
					foundLink = nearest;
					hit.ArcFraction = nearestDistance.ArcFraction;
					return hit;
				}
			}

			// Comments, smallest first (they are drawn largest first): an edge, then the title bar.
			std::optional<uint32_t> edgeComment;
			uint8_t edges = 0;
			std::optional<uint32_t> barComment;
			for (uint32_t const item : candidates)
			{
				if (VisualScriptSpatialIndex::GetKind(item) != Item::Comment)
				{
					continue;
				}
				uint32_t const comment = VisualScriptSpatialIndex::GetIndex(item);
				uint8_t const found = VisualScriptGeometry::GetCommentEdges(comments[comment], point, commentTolerance);
				if (found != 0 && (!edgeComment || comment > *edgeComment))
				{
					edgeComment = comment;
					edges = found;
				}
				bool const onBar = VisualScriptGeometry::Contains(
					VisualScriptGeometry::GetCommentBarRect(comments[comment], pixelsPerUnit, uiScale), point);
				if (onBar && (!barComment || comment > *barComment))
				{
					barComment = comment;
				}
			}
			if (edgeComment)
			{
				VisualScriptHit hit;
				hit.Kind = VisualScriptHitKind::CommentEdge;
				hit.Element = comments[*edgeComment].ID;
				hit.Edges = edges;
				return hit;
			}
			if (barComment)
			{
				VisualScriptHit hit;
				hit.Kind = VisualScriptHitKind::CommentTitle;
				hit.Element = comments[*barComment].ID;
				return hit;
			}
			return background;
		}
	}

	VisualScriptHit HitTestCanvas(VisualScriptCanvasModel const& model, VisualScriptSpatialIndex const& index, VisualScriptView const& view,
	                              VisualScriptHitContext const& context, glm::vec2 mouse)
	{
		VisualScriptHit hit;
		HitTestCanvas(model, index, view, context, mouse, hit);
		return hit;
	}

	void HitTestCanvas(VisualScriptCanvasModel const& model, VisualScriptSpatialIndex const& index, VisualScriptView const& view,
	                   VisualScriptHitContext const& context, glm::vec2 mouse, VisualScriptHit& hit)
	{
		std::optional<uint32_t> link;
		VisualScriptHit found = HitTestWithoutLink(model, index, view, context, mouse, link);
		std::optional<VisualScriptLink> names = std::move(hit.Link);
		hit = std::move(found);
		if (!link)
		{
			return;
		}
		std::span<VisualScriptNodeVisual const> const nodes = model.GetNodes();
		std::span<VisualScriptLinkVisual const> const links = model.GetLinks();
		VisualScriptLinkVisual const& visual = links[*link];
		if (!names)
		{
			names.emplace();
		}
		names->From.Node = nodes[visual.FromNode].ID;
		names->From.Pin.assign(model.GetText(visual.FromName));
		names->To.Node = nodes[visual.ToNode].ID;
		names->To.Pin.assign(model.GetText(visual.ToName));
		hit.Link = std::move(names);
	}
}
