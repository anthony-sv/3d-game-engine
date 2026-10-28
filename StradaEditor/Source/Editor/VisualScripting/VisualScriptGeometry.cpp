#include "Editor/VisualScripting/VisualScriptGeometry.h"

#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Strada::VisualScriptGeometry
{
	namespace
	{
		namespace Metrics = VisualScriptCanvasMetrics;

		bool HasKindGlyph(VisualScriptNodeVisual const& node)
		{
			return node.Kind == VisualScriptNodeKind::Event || node.HasFlag(VisualScriptNodeFlags::FunctionNode);
		}

		float GetHeaderHeight(VisualScriptNodeVisual const& node)
		{
			switch (node.Shape)
			{
				case VisualScriptNodeShape::Standard:
					return node.Subtitle.Length > 0 ? Metrics::HeaderHeightWithSubtitle : Metrics::HeaderHeight;
				case VisualScriptNodeShape::Pill:
					return Metrics::PillHeight;
				case VisualScriptNodeShape::Operator:
					return Metrics::OperatorHeight;
				case VisualScriptNodeShape::Reroute:
					return 0.0f;
			}
			return 0.0f;
		}

		int CountBadges(VisualScriptNodeVisual const& node)
		{
			return (node.ErrorCount > 0 ? 1 : 0) + (node.WarningCount > 0 ? 1 : 0);
		}

		// The centre of the rightmost badge, relative to the node.
		glm::vec2 GetFirstBadgeCenter(VisualScriptNodeVisual const& node)
		{
			if (node.Shape == VisualScriptNodeShape::Standard)
			{
				return glm::vec2(node.Size.x - Metrics::HorizontalPadding - Metrics::BadgeSlot * 0.5f, Metrics::HeaderHeight * 0.5f);
			}
			return glm::vec2(node.Size.x, 0.0f);
		}

		VisualScriptRect MakeRect(glm::vec2 min, glm::vec2 size)
		{
			return {min, min + size};
		}

		VisualScriptRect MakeSquare(glm::vec2 center, float halfSize)
		{
			return {center - glm::vec2(halfSize), center + glm::vec2(halfSize)};
		}

		// The rightmost point of a Standard header's free part: what the badges and the latent clock leave.
		float GetHeaderGlyphsEnd(VisualScriptNodeVisual const& node)
		{
			return node.Size.x - Metrics::HorizontalPadding - Metrics::BadgeSlot * static_cast<float>(CountBadges(node));
		}

		glm::vec2 EvaluateBezier(std::array<glm::vec2, 4> const& points, float t)
		{
			float const u = 1.0f - t;
			float const w1 = u * u * u;
			float const w2 = 3.0f * u * u * t;
			float const w3 = 3.0f * u * t * t;
			float const w4 = t * t * t;
			return points[0] * w1 + points[1] * w2 + points[2] * w3 + points[3] * w4;
		}

		// ImGui's PathBezierCubicCurveToCasteljau (imgui_draw.cpp), so wires flatten as ImGui would flatten them, except that a
		// piece still curved at the deepest level ends at its end point rather than being dropped.
		void Subdivide(std::vector<glm::vec2>& out, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4,
		               float tolerance, int level)
		{
			float const dx = x4 - x1;
			float const dy = y4 - y1;
			float const d2 = std::abs((x2 - x4) * dy - (y2 - y4) * dx);
			float const d3 = std::abs((x3 - x4) * dy - (y3 - y4) * dx);
			if ((d2 + d3) * (d2 + d3) < tolerance * (dx * dx + dy * dy) || level >= MaxDepth)
			{
				out.emplace_back(x4, y4);
				return;
			}
			float const x12 = (x1 + x2) * 0.5f;
			float const y12 = (y1 + y2) * 0.5f;
			float const x23 = (x2 + x3) * 0.5f;
			float const y23 = (y2 + y3) * 0.5f;
			float const x34 = (x3 + x4) * 0.5f;
			float const y34 = (y3 + y4) * 0.5f;
			float const x123 = (x12 + x23) * 0.5f;
			float const y123 = (y12 + y23) * 0.5f;
			float const x234 = (x23 + x34) * 0.5f;
			float const y234 = (y23 + y34) * 0.5f;
			float const x1234 = (x123 + x234) * 0.5f;
			float const y1234 = (y123 + y234) * 0.5f;
			Subdivide(out, x1, y1, x12, y12, x123, y123, x1234, y1234, tolerance, level + 1);
			Subdivide(out, x1234, y1234, x234, y234, x34, y34, x4, y4, tolerance, level + 1);
		}

		// The sign of the turn from a to b to c: positive counter-clockwise, 0 collinear.
		float GetOrientation(glm::vec2 a, glm::vec2 b, glm::vec2 c)
		{
			return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
		}

		// Whether c lies within the box of the segment from a to b (for collinear points).
		bool IsWithinSegmentBox(glm::vec2 a, glm::vec2 b, glm::vec2 c)
		{
			return c.x >= std::min(a.x, b.x) && c.x <= std::max(a.x, b.x) && c.y >= std::min(a.y, b.y) && c.y <= std::max(a.y, b.y);
		}

		bool SegmentsIntersect(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d)
		{
			float const o1 = GetOrientation(a, b, c);
			float const o2 = GetOrientation(a, b, d);
			float const o3 = GetOrientation(c, d, a);
			float const o4 = GetOrientation(c, d, b);
			if (((o1 > 0.0f && o2 < 0.0f) || (o1 < 0.0f && o2 > 0.0f)) && ((o3 > 0.0f && o4 < 0.0f) || (o3 < 0.0f && o4 > 0.0f)))
			{
				return true;
			}
			return (o1 == 0.0f && IsWithinSegmentBox(a, b, c)) || (o2 == 0.0f && IsWithinSegmentBox(a, b, d)) ||
			       (o3 == 0.0f && IsWithinSegmentBox(c, d, a)) || (o4 == 0.0f && IsWithinSegmentBox(c, d, b));
		}

		// Where a pin's wires end this frame: on its node as the preview draws it (moved, or expanded under a dragged wire).
		glm::vec2 GetDrawnAnchor(VisualScriptCanvasModel const& model, uint32_t node, uint32_t pin, VisualScriptNodePreview const& preview)
		{
			// Spans named first: GCC 13 cannot tell that a reference through a temporary span stays valid.
			std::span<VisualScriptNodeVisual const> const nodes = model.GetNodes();
			std::span<VisualScriptPinVisual const> const pins = model.GetPins();
			VisualScriptNodeVisual const& stored = nodes[node];
			VisualScriptPinVisual const& visual = pins[pin];
			glm::vec2 const offset = preview.GetOffset(node);
			if (preview.Expanded == node && stored.HasFlag(VisualScriptNodeFlags::Collapsed))
			{
				return GetPinAnchor(GetExpandedNode(stored), visual, offset);
			}
			return GetPinAnchor(stored, visual, offset);
		}

		// Whether a reroute faces left: its first target lies left of its dot.
		bool FacesLeft(VisualScriptCanvasModel const& model, uint32_t nodeIndex, VisualScriptNodePreview const& preview)
		{
			std::optional<uint32_t> const first = model.GetFirstOutgoingLink(nodeIndex);
			if (!first)
			{
				return false;
			}
			std::span<VisualScriptLinkVisual const> const links = model.GetLinks();
			VisualScriptLinkVisual const& link = links[*first];
			glm::vec2 const targetAnchor = GetDrawnAnchor(model, link.ToNode, link.ToPin, preview);
			glm::vec2 const center = GetRerouteCenter(model.GetNodes()[nodeIndex], preview.GetOffset(nodeIndex));
			return targetAnchor.x < center.x;
		}
	}

	VisualScriptRect GetNodeRect(VisualScriptNodeVisual const& node, glm::vec2 offset)
	{
		return MakeRect(node.Position + offset, node.Size);
	}

	VisualScriptNodeVisual GetExpandedNode(VisualScriptNodeVisual node)
	{
		if (node.HasFlag(VisualScriptNodeFlags::Collapsed))
		{
			node.Size = node.ExpandedSize;
			node.Flags = static_cast<uint16_t>(node.Flags & ~VisualScriptNodeFlags::Collapsed);
		}
		return node;
	}

	VisualScriptRect GetHeaderRect(VisualScriptNodeVisual const& node, glm::vec2 offset)
	{
		float const height = GetHeaderHeight(node);
		if (height <= 0.0f)
		{
			return {};
		}
		return MakeRect(node.Position + offset, glm::vec2(node.Size.x, height));
	}

	VisualScriptRect GetTitleRect(VisualScriptNodeVisual const& node, glm::vec2 offset)
	{
		glm::vec2 const origin = node.Position + offset;
		float const width = node.Title.Width;
		switch (node.Shape)
		{
			case VisualScriptNodeShape::Standard:
			{
				float const left = Metrics::HorizontalPadding + (HasKindGlyph(node) ? Metrics::KindGlyphSlot : 0.0f);
				glm::vec2 const min(left, (Metrics::HeaderHeight - Metrics::TitleTextSize) * 0.5f);
				return MakeRect(origin + min, glm::vec2(width, Metrics::TitleTextSize));
			}
			case VisualScriptNodeShape::Pill:
			{
				// Centred between the left padding and the output's glyph.
				float const right = node.Size.x - Metrics::PinGlyphInset - Metrics::PinGlyphRadius - Metrics::PinLabelGap;
				float const center = (Metrics::HorizontalPadding + right) * 0.5f;
				glm::vec2 const min(center - width * 0.5f, (Metrics::PillHeight - Metrics::TitleTextSize) * 0.5f);
				return MakeRect(origin + min, glm::vec2(width, Metrics::TitleTextSize));
			}
			case VisualScriptNodeShape::Operator:
			{
				glm::vec2 const min((node.Size.x - width) * 0.5f, (Metrics::OperatorHeight - Metrics::OperatorSymbolTextSize) * 0.5f);
				return MakeRect(origin + min, glm::vec2(width, Metrics::OperatorSymbolTextSize));
			}
			case VisualScriptNodeShape::Reroute:
				return {};
		}
		return {};
	}

	VisualScriptRect GetSubtitleRect(VisualScriptNodeVisual const& node, glm::vec2 offset)
	{
		if (node.Shape != VisualScriptNodeShape::Standard || node.Subtitle.Length == 0)
		{
			return {};
		}
		float const left = Metrics::HorizontalPadding + (HasKindGlyph(node) ? Metrics::KindGlyphSlot : 0.0f);
		return MakeRect(node.Position + offset + glm::vec2(left, Metrics::SubtitleTop),
		                glm::vec2(node.Subtitle.Width, Metrics::SubtitleTextSize));
	}

	VisualScriptRect GetKindGlyphRect(VisualScriptNodeVisual const& node, glm::vec2 offset)
	{
		if (node.Shape != VisualScriptNodeShape::Standard || !HasKindGlyph(node))
		{
			return {};
		}
		float const inset = (Metrics::KindGlyphSlot - Metrics::HeaderGlyphSize) * 0.5f;
		glm::vec2 const min(Metrics::HorizontalPadding + inset, (Metrics::HeaderHeight - Metrics::HeaderGlyphSize) * 0.5f);
		return MakeRect(node.Position + offset + min, glm::vec2(Metrics::HeaderGlyphSize));
	}

	VisualScriptRect GetLatentGlyphRect(VisualScriptNodeVisual const& node, glm::vec2 offset)
	{
		if (node.Shape != VisualScriptNodeShape::Standard || !node.HasFlag(VisualScriptNodeFlags::Latent))
		{
			return {};
		}
		float const inset = (Metrics::KindGlyphSlot - Metrics::HeaderGlyphSize) * 0.5f;
		glm::vec2 const min(GetHeaderGlyphsEnd(node) - Metrics::KindGlyphSlot + inset,
		                    (Metrics::HeaderHeight - Metrics::HeaderGlyphSize) * 0.5f);
		return MakeRect(node.Position + offset + min, glm::vec2(Metrics::HeaderGlyphSize));
	}

	VisualScriptRect GetCollapsedMarkerRect(VisualScriptNodeVisual const& node, glm::vec2 offset)
	{
		if (node.Shape != VisualScriptNodeShape::Standard || !node.HasFlag(VisualScriptNodeFlags::Collapsed))
		{
			return {};
		}
		VisualScriptRect const title = GetTitleRect(node, offset);
		float const inset = (Metrics::KindGlyphSlot - Metrics::HeaderGlyphSize) * 0.5f;
		return MakeRect(
			glm::vec2(title.Max.x + inset, node.Position.y + offset.y + (Metrics::HeaderHeight - Metrics::HeaderGlyphSize) * 0.5f),
			glm::vec2(Metrics::HeaderGlyphSize));
	}

	float GetBadgeRadius(float pixelsPerUnit, float uiScale)
	{
		if (!(pixelsPerUnit > 0.0f) || !(uiScale > 0.0f))
		{
			return Metrics::BadgeRadius;
		}
		float const zoom = pixelsPerUnit / uiScale;
		return std::max(Metrics::BadgeRadius, Metrics::BadgeMinLogicalPixels / zoom);
	}

	std::optional<glm::vec2> GetErrorBadgeCenter(VisualScriptNodeVisual const& node, glm::vec2 offset)
	{
		if (node.ErrorCount == 0)
		{
			return std::nullopt;
		}
		return node.Position + offset + GetFirstBadgeCenter(node);
	}

	std::optional<glm::vec2> GetWarningBadgeCenter(VisualScriptNodeVisual const& node, float pixelsPerUnit, float uiScale, glm::vec2 offset)
	{
		if (node.WarningCount == 0)
		{
			return std::nullopt;
		}
		glm::vec2 center = node.Position + offset + GetFirstBadgeCenter(node);
		if (node.ErrorCount > 0)
		{
			center.x -= std::max(Metrics::BadgeSlot, 2.0f * GetBadgeRadius(pixelsPerUnit, uiScale));
		}
		return center;
	}

	VisualScriptRect GetBadgeRect(VisualScriptNodeVisual const& node, float pixelsPerUnit, float uiScale, glm::vec2 offset)
	{
		float const radius = GetBadgeRadius(pixelsPerUnit, uiScale);
		VisualScriptRect rect;
		if (std::optional<glm::vec2> const error = GetErrorBadgeCenter(node, offset))
		{
			rect = MakeSquare(*error, radius);
		}
		if (std::optional<glm::vec2> const warning = GetWarningBadgeCenter(node, pixelsPerUnit, uiScale, offset))
		{
			rect = Unite(rect, MakeSquare(*warning, radius));
		}
		return rect;
	}

	VisualScriptRect GetBubbleRect(VisualScriptNodeVisual const& node, glm::vec2 offset)
	{
		if (IsEmpty(node.BubbleRect))
		{
			return {};
		}
		return Translate(node.BubbleRect, node.Position + offset);
	}

	VisualScriptRect GetEnabledStripRect(VisualScriptNodeVisual const& node, glm::vec2 offset)
	{
		bool const marked = node.HasFlag(VisualScriptNodeFlags::Disabled) || node.HasFlag(VisualScriptNodeFlags::DevelopmentOnly);
		if (!marked || node.Shape == VisualScriptNodeShape::Reroute)
		{
			return {};
		}
		glm::vec2 const origin = node.Position + offset;
		return {origin + glm::vec2(0.0f, node.Size.y - Metrics::EnabledStripHeight), origin + node.Size};
	}

	glm::vec2 GetRerouteCenter(VisualScriptNodeVisual const& node, glm::vec2 offset)
	{
		return node.Position + offset + node.Size * 0.5f;
	}

	glm::vec2 GetPinAnchor(VisualScriptNodeVisual const& node, VisualScriptPinVisual const& pin, glm::vec2 offset)
	{
		glm::vec2 const origin = node.Position + offset;
		if (node.HasFlag(VisualScriptNodeFlags::Collapsed) && node.Shape == VisualScriptNodeShape::Standard)
		{
			float const x = pin.Direction == VisualScriptPinDirection::In ? 0.0f : node.Size.x;
			return origin + glm::vec2(x, GetHeaderHeight(node) * 0.5f);
		}
		return origin + pin.Anchor;
	}

	bool ArePinLabelsDrawn(VisualScriptDetailLevel level, float zoom)
	{
		return level == VisualScriptDetailLevel::Detail ||
		       (level == VisualScriptDetailLevel::Compact && Metrics::IsTextVisible(Metrics::PinLabelTextSize, zoom));
	}

	VisualScriptRect GetPinHitRect(VisualScriptNodeVisual const& node, VisualScriptPinVisual const& pin, float minimumCu, glm::vec2 offset,
	                               bool labelRows)
	{
		glm::vec2 const anchor = GetPinAnchor(node, pin, offset);
		VisualScriptRect const around = MakeSquare(anchor, std::max(minimumCu, 0.0f));
		if (!labelRows || (node.HasFlag(VisualScriptNodeFlags::Collapsed) && node.Shape == VisualScriptNodeShape::Standard))
		{
			return around;
		}
		return Unite(Translate(pin.HitRect, node.Position + offset), around);
	}

	VisualScriptRect GetPinLabelRect(VisualScriptNodeVisual const& node, VisualScriptPinVisual const& pin, glm::vec2 offset)
	{
		if (node.Shape != VisualScriptNodeShape::Standard || pin.HasFlag(VisualScriptPinFlags::LabelHidden) || pin.Label.Length == 0)
		{
			return {};
		}
		glm::vec2 const anchor = node.Position + offset + pin.Anchor;
		float const gap = Metrics::PinGlyphRadius + Metrics::PinLabelGap;
		float const top = anchor.y - Metrics::PinLabelTextSize * 0.5f;
		float const left = pin.Direction == VisualScriptPinDirection::In ? anchor.x + gap : anchor.x - gap - pin.Label.Width;
		return MakeRect(glm::vec2(left, top), glm::vec2(pin.Label.Width, Metrics::PinLabelTextSize));
	}

	VisualScriptRect GetChipRect(VisualScriptNodeVisual const& node, VisualScriptPinVisual const& pin, glm::vec2 offset)
	{
		if (IsEmpty(pin.ChipRect))
		{
			return {};
		}
		return Translate(pin.ChipRect, node.Position + offset);
	}

	VisualScriptRect GetChipCellRect(VisualScriptRect const& chip, uint32_t cell)
	{
		float const left = chip.Min.x + static_cast<float>(cell) * (Metrics::ChipVectorCellWidth + Metrics::ChipCellGap);
		return {glm::vec2(left, chip.Min.y), glm::vec2(left + Metrics::ChipVectorCellWidth, chip.Max.y)};
	}

	std::array<glm::vec2, 4> GetWireControlPoints(glm::vec2 from, glm::vec2 to, VisualScriptWireEnds ends)
	{
		float const dx = to.x - from.x;
		float const dy = to.y - from.y;
		float const fromDirection = ends.FromReversed ? -1.0f : 1.0f;
		float const toDirection = ends.ToReversed ? -1.0f : 1.0f;
		// Forward when the target lies the way the wire leaves its start.
		bool const forward = fromDirection * dx >= 0.0f;
		float const tangent = forward ? std::clamp(0.5f * std::abs(dx) + 0.25f * std::abs(dy), MinTangent, MaxTangent)
		                              : std::clamp(0.5f * std::abs(dx) + 0.5f * std::abs(dy), BackwardMinTangent, MaxTangent);
		bool const facingRight = !ends.FromReversed && !ends.ToReversed;
		if (!forward && facingRight && std::abs(dy) < LoopDrop)
		{
			// A loop on one row passes below both nodes.
			return {from, from + glm::vec2(tangent, LoopDrop), to + glm::vec2(-tangent, LoopDrop), to};
		}
		return {from, from + glm::vec2(fromDirection * tangent, 0.0f), to - glm::vec2(toDirection * tangent, 0.0f), to};
	}

	VisualScriptRect GetWireBounds(std::array<glm::vec2, 4> const& points)
	{
		VisualScriptRect bounds{points[0], points[0]};
		for (glm::vec2 const point : points)
		{
			bounds.Min = glm::min(bounds.Min, point);
			bounds.Max = glm::max(bounds.Max, point);
		}
		return bounds;
	}

	std::array<glm::vec2, 4> GetLinkControlPoints(VisualScriptCanvasModel const& model, uint32_t link,
	                                              VisualScriptNodePreview const& preview)
	{
		std::span<VisualScriptLinkVisual const> const links = model.GetLinks();
		VisualScriptLinkVisual const& visual = links[link];
		glm::vec2 const from = GetDrawnAnchor(model, visual.FromNode, visual.FromPin, preview);
		glm::vec2 const to = GetDrawnAnchor(model, visual.ToNode, visual.ToPin, preview);
		VisualScriptWireEnds ends;
		ends.FromReversed =
			model.GetNodes()[visual.FromNode].Shape == VisualScriptNodeShape::Reroute && FacesLeft(model, visual.FromNode, preview);
		ends.ToReversed =
			model.GetNodes()[visual.ToNode].Shape == VisualScriptNodeShape::Reroute && FacesLeft(model, visual.ToNode, preview);
		return GetWireControlPoints(from, to, ends);
	}

	VisualScriptRect GetLinkBounds(VisualScriptCanvasModel const& model, uint32_t link, VisualScriptNodePreview const& preview)
	{
		return Expand(GetWireBounds(GetLinkControlPoints(model, link, preview)), WireBoundsMargin);
	}

	WireFlattening GetWireFlattening(VisualScriptDetailLevel level, float curveTessellationTolerance)
	{
		switch (level)
		{
			case VisualScriptDetailLevel::Far:
				return {0.0f, FarSegments};
			case VisualScriptDetailLevel::Overview:
				return {curveTessellationTolerance, OverviewMaxSegments};
			case VisualScriptDetailLevel::Compact:
			case VisualScriptDetailLevel::Detail:
				return {curveTessellationTolerance, MaxSegments};
		}
		return {curveTessellationTolerance, MaxSegments};
	}

	void FlattenWire(std::array<glm::vec2, 4> const& screenPoints, float toleranceScreen, int maxSegments, std::vector<glm::vec2>& out)
	{
		int const segments = std::max(maxSegments, 1);
		out.clear();
		out.push_back(screenPoints[0]);
		if (toleranceScreen > 0.0f)
		{
			Subdivide(out, screenPoints[0].x, screenPoints[0].y, screenPoints[1].x, screenPoints[1].y, screenPoints[2].x, screenPoints[2].y,
			          screenPoints[3].x, screenPoints[3].y, toleranceScreen, 0);
			if (out.size() - 1 <= static_cast<size_t>(segments) && out.size() > 1)
			{
				return;
			}
			out.resize(1);
		}
		float const step = 1.0f / static_cast<float>(segments);
		for (int index = 1; index < segments; index++)
		{
			out.push_back(EvaluateBezier(screenPoints, step * static_cast<float>(index)));
		}
		out.push_back(screenPoints[3]);
	}

	WireDistance MeasureDistance(std::span<glm::vec2 const> polyline, glm::vec2 point)
	{
		if (polyline.empty())
		{
			return {std::numeric_limits<float>::infinity(), 0.0f};
		}
		if (polyline.size() == 1)
		{
			return {glm::length(point - polyline[0]), 0.0f};
		}
		float best = std::numeric_limits<float>::infinity();
		float bestArc = 0.0f;
		float arc = 0.0f;
		for (size_t index = 1; index < polyline.size(); index++)
		{
			glm::vec2 const a = polyline[index - 1];
			glm::vec2 const b = polyline[index];
			glm::vec2 const segment = b - a;
			float const lengthSquared = glm::dot(segment, segment);
			float const t = lengthSquared > 0.0f ? std::clamp(glm::dot(point - a, segment) / lengthSquared, 0.0f, 1.0f) : 0.0f;
			float const length = std::sqrt(lengthSquared);
			float const distance = glm::length(point - (a + segment * t));
			if (distance < best)
			{
				best = distance;
				bestArc = arc + length * t;
			}
			arc += length;
		}
		return {best, arc > 0.0f ? bestArc / arc : 0.0f};
	}

	glm::vec2 GetPolylinePoint(std::span<glm::vec2 const> polyline, float arcFraction)
	{
		if (polyline.empty())
		{
			return glm::vec2(0.0f);
		}
		float total = 0.0f;
		for (size_t index = 1; index < polyline.size(); index++)
		{
			total += glm::length(polyline[index] - polyline[index - 1]);
		}
		float remaining = (std::isfinite(arcFraction) ? std::clamp(arcFraction, 0.0f, 1.0f) : 0.0f) * total;
		for (size_t index = 1; index < polyline.size(); index++)
		{
			glm::vec2 const a = polyline[index - 1];
			glm::vec2 const b = polyline[index];
			float const length = glm::length(b - a);
			if (remaining <= length && length > 0.0f)
			{
				return a + (b - a) * (remaining / length);
			}
			remaining -= length;
		}
		return polyline.back();
	}

	bool SegmentCrossesPolyline(glm::vec2 a, glm::vec2 b, std::span<glm::vec2 const> polyline)
	{
		for (size_t index = 1; index < polyline.size(); index++)
		{
			if (SegmentsIntersect(a, b, polyline[index - 1], polyline[index]))
			{
				return true;
			}
		}
		return false;
	}

	float GetCommentTitlePixels(float pixelsPerUnit, float uiScale)
	{
		float const scale = uiScale > 0.0f ? uiScale : 1.0f;
		float const zoom = pixelsPerUnit / scale;
		float const logical = std::max(Metrics::CommentTitleTextSize * zoom, Metrics::CommentTitleMinLogicalPixels);
		return Metrics::LadderFloor(logical * scale);
	}

	VisualScriptRect GetCommentBarRect(VisualScriptCommentVisual const& comment, float pixelsPerUnit, float uiScale)
	{
		if (!(pixelsPerUnit > 0.0f))
		{
			return comment.Rect;
		}
		float const scale = uiScale > 0.0f ? uiScale : 1.0f;
		float const barPixels =
			std::max(Metrics::CommentTitleBarHeight * pixelsPerUnit,
		             GetCommentTitlePixels(pixelsPerUnit, scale) + Metrics::CommentTitleBarPaddingLogicalPixels * scale);
		float const height = std::min(barPixels / pixelsPerUnit, comment.Rect.Max.y - comment.Rect.Min.y);
		return {comment.Rect.Min, glm::vec2(comment.Rect.Max.x, comment.Rect.Min.y + std::max(height, 0.0f))};
	}

	uint32_t CountGlyphs(std::string_view text)
	{
		return static_cast<uint32_t>(std::count_if(text.begin(), text.end(),
		                                           [](char byte)
		                                           {
													   return (static_cast<unsigned char>(byte) & 0xC0u) != 0x80u;
												   }));
	}

	VisualScriptRect GetCommentTitleRect(VisualScriptCommentVisual const& comment, VisualScriptView const& view, float titleWidthCu,
	                                     uint32_t titleGlyphs)
	{
		float const pixelsPerUnit = view.GetPixelsPerUnit();
		float const uiScale = view.GetUIScale();
		if (!(pixelsPerUnit > 0.0f) || !(titleWidthCu > 0.0f) || comment.Title.Length == 0)
		{
			return {};
		}
		float const titlePixels = GetCommentTitlePixels(pixelsPerUnit, uiScale);
		VisualScriptRect const bar = GetCommentBarRect(comment, pixelsPerUnit, uiScale);
		glm::vec2 const lower = view.ToScreen(bar.Min);
		glm::vec2 const upper = view.ToScreen(bar.Max);
		glm::vec2 const canvasMin = view.GetCanvasMin();
		glm::vec2 const canvasMax = canvasMin + view.GetCanvasSize();
		// No vertical stickiness: nested regions would stack their titles at the canvas's edge.
		if (!(titlePixels > 0.0f) || upper.y - lower.y < titlePixels || upper.y <= canvasMin.y || lower.y >= canvasMax.y)
		{
			return {};
		}
		float const inset = std::max(Metrics::HorizontalPadding * pixelsPerUnit, 4.0f * uiScale);
		float const left = std::max(lower.x, canvasMin.x) + inset;
		float const available = std::min(upper.x, canvasMax.x) - left - inset;
		float const width = titleWidthCu * titlePixels / Metrics::CommentTitleTextSize;
		// CommentTitleMinGlyphs glyphs and the ellipsis after them, at the title's average glyph width.
		uint32_t const glyphs = std::max<uint32_t>(titleGlyphs + (comment.Title.IsEllipsized() ? 1u : 0u), 1u);
		float const minimum = std::min(width, static_cast<float>(CommentTitleMinGlyphs + 1u) * width / static_cast<float>(glyphs));
		if (!(available > 0.0f) || available < minimum)
		{
			return {};
		}
		float const top = std::floor((lower.y + upper.y - titlePixels) * 0.5f);
		return {glm::vec2(left, top), glm::vec2(left + std::min(width, available), top + titlePixels)};
	}

	VisualScriptRect GetCommentLabelRect(VisualScriptCommentVisual const& comment, VisualScriptView const& view, float titleWidthCu,
	                                     uint32_t titleGlyphs)
	{
		VisualScriptRect const title = GetCommentTitleRect(comment, view, titleWidthCu, titleGlyphs);
		if (IsEmpty(title))
		{
			return {};
		}
		return Expand(title, CommentLabelPaddingLogicalPixels * view.GetUIScale());
	}

	uint8_t GetCommentEdges(VisualScriptCommentVisual const& comment, glm::vec2 point, float tolerance)
	{
		VisualScriptRect const& rect = comment.Rect;
		if (!Contains(Expand(rect, tolerance), point))
		{
			return 0;
		}
		float const left = std::abs(point.x - rect.Min.x);
		float const right = std::abs(point.x - rect.Max.x);
		float const top = std::abs(point.y - rect.Min.y);
		float const bottom = std::abs(point.y - rect.Max.y);
		uint8_t edges = 0;
		// A box narrower than twice the tolerance has its nearer edge only.
		if (left <= tolerance || right <= tolerance)
		{
			edges |= left <= right ? LeftEdge : RightEdge;
		}
		if (top <= tolerance || bottom <= tolerance)
		{
			edges |= top <= bottom ? TopEdge : BottomEdge;
		}
		return edges;
	}

	VisualScriptRect GetMinimapRect(glm::vec2 canvasMin, glm::vec2 canvasSize, float uiScale)
	{
		float const scale = uiScale > 0.0f && std::isfinite(uiScale) ? uiScale : 1.0f;
		if (!(canvasSize.x > 0.0f) || !(canvasSize.y > 0.0f) || !std::isfinite(canvasSize.x) || !std::isfinite(canvasSize.y))
		{
			return {canvasMin, canvasMin};
		}
		float const width = std::clamp(Metrics::MinimapWidthShare * canvasSize.x, Metrics::MinimapMinWidthLogicalPixels * scale,
		                               Metrics::MinimapMaxWidthLogicalPixels * scale);
		glm::vec2 const size(width, width * Metrics::MinimapAspect);
		glm::vec2 const max = canvasMin + canvasSize - glm::vec2(Metrics::MinimapMarginLogicalPixels * scale);
		return {max - size, max};
	}

	VisualScriptMinimapTransform GetMinimapTransform(VisualScriptRect const& minimapRect,
	                                                 std::optional<VisualScriptRect> const& contentBounds)
	{
		VisualScriptMinimapTransform transform;
		transform.Rect = minimapRect;
		glm::vec2 const rectSize = minimapRect.GetSize();
		if (!contentBounds || !(rectSize.x > 0.0f) || !(rectSize.y > 0.0f))
		{
			return transform;
		}
		glm::vec2 const size = glm::max(contentBounds->GetSize(), glm::vec2(0.0f));
		// A margin of the larger side keeps a single node from filling the minimap edge to edge.
		float const margin = std::max(std::max(size.x, size.y) * Metrics::MinimapContentMargin, 1.0f);
		transform.Bounds = Expand(*contentBounds, margin);
		glm::vec2 const shown = transform.Bounds.GetSize();
		transform.Scale = std::min(rectSize.x / shown.x, rectSize.y / shown.y);
		if (!std::isfinite(transform.Scale) || !(transform.Scale > 0.0f))
		{
			transform.Scale = 0.0f;
		}
		return transform;
	}

	bool Contains(VisualScriptRect const& rect, glm::vec2 point)
	{
		return point.x >= rect.Min.x && point.x <= rect.Max.x && point.y >= rect.Min.y && point.y <= rect.Max.y;
	}

	bool Intersects(VisualScriptRect const& a, VisualScriptRect const& b)
	{
		return a.Min.x <= b.Max.x && b.Min.x <= a.Max.x && a.Min.y <= b.Max.y && b.Min.y <= a.Max.y;
	}

	bool IsEmpty(VisualScriptRect const& rect)
	{
		return !(rect.Max.x > rect.Min.x) || !(rect.Max.y > rect.Min.y);
	}

	VisualScriptRect Unite(VisualScriptRect const& a, VisualScriptRect const& b)
	{
		if (IsEmpty(a))
		{
			return b;
		}
		if (IsEmpty(b))
		{
			return a;
		}
		return {glm::min(a.Min, b.Min), glm::max(a.Max, b.Max)};
	}

	VisualScriptRect Expand(VisualScriptRect const& rect, float amount)
	{
		return {rect.Min - glm::vec2(amount), rect.Max + glm::vec2(amount)};
	}

	VisualScriptRect Translate(VisualScriptRect const& rect, glm::vec2 offset)
	{
		return {rect.Min + offset, rect.Max + offset};
	}
}
