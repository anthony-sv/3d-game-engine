#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <vector>

using namespace Strada;

namespace
{
	namespace Geometry = VisualScriptGeometry;
	namespace Metrics = VisualScriptCanvasMetrics;

	void CheckPoint(glm::vec2 actual, glm::vec2 expected)
	{
		CHECK(actual.x == doctest::Approx(expected.x));
		CHECK(actual.y == doctest::Approx(expected.y));
	}

	glm::vec2 EvaluateBezier(std::array<glm::vec2, 4> const& points, float t)
	{
		float const u = 1.0f - t;
		return points[0] * (u * u * u) + points[1] * (3.0f * u * u * t) + points[2] * (3.0f * u * t * t) + points[3] * (t * t * t);
	}

	// A Standard node at (100, 200), 180 × 90, header 28.
	VisualScriptNodeVisual MakeNode()
	{
		VisualScriptNodeVisual node;
		node.ID = 1;
		node.Position = glm::vec2(100.0f, 200.0f);
		node.Size = glm::vec2(180.0f, 90.0f);
		node.ExpandedSize = node.Size;
		node.Title.Length = 6;
		node.Title.VisibleLength = 6;
		node.Title.Width = 45.0f;
		return node;
	}
}

TEST_CASE("VisualScriptEditor: wires leave outputs to the right and loop below both nodes when they run backward on one row")
{
	glm::vec2 const from(0.0f, 0.0f);

	// Forward: tangents of half the horizontal distance plus a quarter of the vertical one, within 40 and 400.
	std::array<glm::vec2, 4> forward = Geometry::GetWireControlPoints(from, glm::vec2(200.0f, 80.0f), {});
	CheckPoint(forward[1], glm::vec2(120.0f, 0.0f));
	CheckPoint(forward[2], glm::vec2(80.0f, 80.0f));
	CheckPoint(forward[3], glm::vec2(200.0f, 80.0f));
	CheckPoint(Geometry::GetWireControlPoints(from, glm::vec2(10.0f, 0.0f), {})[1], glm::vec2(40.0f, 0.0f));
	CheckPoint(Geometry::GetWireControlPoints(from, glm::vec2(2000.0f, 0.0f), {})[1], glm::vec2(400.0f, 0.0f));

	// Backward on one row (heights closer than 120): both tangents dip 120 below, so the wire passes under both nodes.
	std::array<glm::vec2, 4> const loop = Geometry::GetWireControlPoints(from, glm::vec2(-300.0f, 40.0f), {});
	CheckPoint(loop[1], glm::vec2(170.0f, 120.0f));
	CheckPoint(loop[2], glm::vec2(-470.0f, 160.0f));
	for (float t = 0.1f; t < 0.95f; t += 0.1f)
	{
		CHECK(EvaluateBezier(loop, t).y > 0.0f);
	}
	// At least 100 long, and no loop for ends far apart in height.
	CheckPoint(Geometry::GetWireControlPoints(from, glm::vec2(-20.0f, 0.0f), {})[1], glm::vec2(100.0f, 120.0f));
	std::array<glm::vec2, 4> const steep = Geometry::GetWireControlPoints(from, glm::vec2(-300.0f, 400.0f), {});
	CheckPoint(steep[1], glm::vec2(350.0f, 0.0f));
	CheckPoint(steep[2], glm::vec2(-650.0f, 400.0f));

	// A reroute that faces left turns its end around: the wire leaves it leftward, or reaches it from the right.
	std::array<glm::vec2, 4> const leaving = Geometry::GetWireControlPoints(from, glm::vec2(-200.0f, 0.0f), {true, false});
	CheckPoint(leaving[1], glm::vec2(-100.0f, 0.0f));
	CheckPoint(leaving[2], glm::vec2(-300.0f, 0.0f));
	std::array<glm::vec2, 4> const arriving = Geometry::GetWireControlPoints(from, glm::vec2(-200.0f, 300.0f), {false, true});
	CheckPoint(arriving[1], glm::vec2(250.0f, 0.0f));
	CheckPoint(arriving[2], glm::vec2(50.0f, 300.0f));

	// The bounds of the control points hold the whole curve.
	VisualScriptRect const bounds = Geometry::GetWireBounds(loop);
	for (float t = 0.0f; t <= 1.0f; t += 0.05f)
	{
		CHECK(Geometry::Contains(bounds, EvaluateBezier(loop, t)));
	}
}

TEST_CASE("VisualScriptEditor: wires flatten as ImGui flattens curves, within their level's segment cap")
{
	std::array<glm::vec2, 4> const points = {glm::vec2(0.0f, 0.0f), glm::vec2(300.0f, 0.0f), glm::vec2(100.0f, 400.0f),
	                                         glm::vec2(400.0f, 400.0f)};
	std::vector<glm::vec2> polyline;
	Geometry::FlattenWire(points, 1.25f, Geometry::MaxSegments, polyline);
	REQUIRE(polyline.size() >= 3);
	CHECK(polyline.size() - 1 <= static_cast<size_t>(Geometry::MaxSegments));
	CheckPoint(polyline.front(), points[0]);
	CheckPoint(polyline.back(), points[3]);
	// ImGui's flatness test: every segment's chord stays within about the tolerance of the curve.
	for (size_t index = 1; index < polyline.size(); index++)
	{
		glm::vec2 const middle = (polyline[index - 1] + polyline[index]) * 0.5f;
		float nearest = 1e9f;
		for (float t = 0.0f; t <= 1.0f; t += 0.0005f)
		{
			nearest = std::min(nearest, glm::length(EvaluateBezier(points, t) - middle));
		}
		CHECK(nearest < 2.0f);
	}
	// A finer tolerance gives more points, a coarser one fewer.
	std::vector<glm::vec2> fine;
	Geometry::FlattenWire(points, 0.1f, Geometry::MaxSegments, fine);
	std::vector<glm::vec2> coarse;
	Geometry::FlattenWire(points, 10.0f, Geometry::MaxSegments, coarse);
	CHECK(fine.size() > polyline.size());
	CHECK(coarse.size() < polyline.size());

	// More segments than the cap: equal steps of the curve instead.
	std::vector<glm::vec2> capped;
	Geometry::FlattenWire(points, 0.01f, Geometry::OverviewMaxSegments, capped);
	REQUIRE(capped.size() == static_cast<size_t>(Geometry::OverviewMaxSegments) + 1);
	for (size_t index = 0; index < capped.size(); index++)
	{
		CheckPoint(capped[index], EvaluateBezier(points, static_cast<float>(index) / static_cast<float>(Geometry::OverviewMaxSegments)));
	}

	// The levels' settings: Far is three equal steps whatever the tolerance.
	Geometry::WireFlattening const farLevel = Geometry::GetWireFlattening(VisualScriptDetailLevel::Far, 1.25f);
	CHECK(farLevel.Tolerance == 0.0f);
	CHECK(farLevel.SegmentCap == 3);
	std::vector<glm::vec2> fixed;
	Geometry::FlattenWire(points, farLevel.Tolerance, farLevel.SegmentCap, fixed);
	CHECK(fixed.size() == 4);
	CHECK(Geometry::GetWireFlattening(VisualScriptDetailLevel::Overview, 1.25f).SegmentCap == 12);
	CHECK(Geometry::GetWireFlattening(VisualScriptDetailLevel::Compact, 1.25f).SegmentCap == 64);
	CHECK(Geometry::GetWireFlattening(VisualScriptDetailLevel::Detail, 2.0f).Tolerance == 2.0f);

	// A straight wire is one segment.
	std::vector<glm::vec2> straight;
	Geometry::FlattenWire({glm::vec2(0.0f), glm::vec2(10.0f, 0.0f), glm::vec2(20.0f, 0.0f), glm::vec2(30.0f, 0.0f)}, 1.25f, 64, straight);
	CHECK(straight.size() == 2);
}

TEST_CASE("VisualScriptEditor: the distance to a wire is measured on its drawn polyline, with where on it the nearest point is")
{
	std::array<glm::vec2, 4> const points = {glm::vec2(0.0f, 0.0f), glm::vec2(200.0f, 0.0f), glm::vec2(200.0f, 300.0f),
	                                         glm::vec2(400.0f, 300.0f)};
	std::vector<glm::vec2> polyline;
	Geometry::FlattenWire(points, 1.25f, Geometry::MaxSegments, polyline);

	// On the curve: within a pixel. Seven pixels off it (along its normal): more than the 6 px hit tolerance.
	for (float t : {0.1f, 0.35f, 0.5f, 0.8f})
	{
		glm::vec2 const onCurve = EvaluateBezier(points, t);
		CHECK(Geometry::MeasureDistance(polyline, onCurve).Distance < 1.0f);
		glm::vec2 const tangent = glm::normalize(EvaluateBezier(points, t + 0.001f) - EvaluateBezier(points, t - 0.001f));
		glm::vec2 const normal(-tangent.y, tangent.x);
		float const off = Geometry::MeasureDistance(polyline, onCurve + normal * 7.0f).Distance;
		CHECK(off > Metrics::WireHitLogicalPixels);
		CHECK(off < 8.0f);
	}
	CHECK(Geometry::MeasureDistance(polyline, points[0]).ArcFraction == doctest::Approx(0.0f));
	CHECK(Geometry::MeasureDistance(polyline, points[3]).ArcFraction == doctest::Approx(1.0f));

	// Along a loop the nearest point's place grows steadily, though the loop first moves away from its end.
	std::array<glm::vec2, 4> const loop = Geometry::GetWireControlPoints(glm::vec2(0.0f), glm::vec2(-300.0f, 0.0f), {});
	std::vector<glm::vec2> loopLine;
	Geometry::FlattenWire(loop, 0.25f, Geometry::MaxSegments, loopLine);
	float previous = -1.0f;
	for (float t = 0.02f; t < 1.0f; t += 0.04f)
	{
		float const fraction = Geometry::MeasureDistance(loopLine, EvaluateBezier(loop, t)).ArcFraction;
		CHECK(fraction > previous);
		previous = fraction;
	}

	// Degenerate polylines.
	CHECK(std::isinf(Geometry::MeasureDistance({}, glm::vec2(0.0f)).Distance));
	std::array<glm::vec2, 1> const single = {glm::vec2(3.0f, 4.0f)};
	CHECK(Geometry::MeasureDistance(single, glm::vec2(0.0f)).Distance == doctest::Approx(5.0f));
}

TEST_CASE("VisualScriptEditor: the cut line crosses wires it touches and no others")
{
	std::array<glm::vec2, 3> const polyline = {glm::vec2(0.0f, 0.0f), glm::vec2(100.0f, 0.0f), glm::vec2(100.0f, 100.0f)};
	CHECK(Geometry::SegmentCrossesPolyline(glm::vec2(50.0f, -10.0f), glm::vec2(50.0f, 10.0f), polyline));
	CHECK(Geometry::SegmentCrossesPolyline(glm::vec2(90.0f, 50.0f), glm::vec2(110.0f, 50.0f), polyline));
	// Ending exactly on the wire counts; passing beside or beyond it does not.
	CHECK(Geometry::SegmentCrossesPolyline(glm::vec2(50.0f, -10.0f), glm::vec2(50.0f, 0.0f), polyline));
	CHECK_FALSE(Geometry::SegmentCrossesPolyline(glm::vec2(50.0f, 5.0f), glm::vec2(50.0f, 50.0f), polyline));
	CHECK_FALSE(Geometry::SegmentCrossesPolyline(glm::vec2(120.0f, -10.0f), glm::vec2(130.0f, 200.0f), polyline));
	// Collinear and overlapping.
	CHECK(Geometry::SegmentCrossesPolyline(glm::vec2(-10.0f, 0.0f), glm::vec2(10.0f, 0.0f), polyline));
	CHECK_FALSE(Geometry::SegmentCrossesPolyline(glm::vec2(-30.0f, 0.0f), glm::vec2(-10.0f, 0.0f), polyline));
}

TEST_CASE("VisualScriptEditor: a comment's title bar is 40 units high, never less than its title on the screen, nor more than the comment")
{
	VisualScriptCommentVisual comment;
	comment.Rect = {glm::vec2(0.0f), glm::vec2(400.0f, 200.0f)};

	// At 100 %: 40 units. Zoomed out to 10 %, the title keeps 12 pixels and the bar 22: 220 units at a scale of 1.
	CHECK(Geometry::GetCommentBarRect(comment, 1.0f, 1.0f).Max.y == doctest::Approx(40.0f));
	CHECK(Geometry::GetCommentTitlePixels(0.1f, 1.0f) == 12.0f);
	CHECK(Geometry::GetCommentBarRect(comment, 0.1f, 1.0f).Max.y == doctest::Approx(200.0f));
	comment.Rect.Max.y = 1000.0f;
	CHECK(Geometry::GetCommentBarRect(comment, 0.1f, 1.0f).Max.y == doctest::Approx(220.0f));
	// The UI scale doubles the pixels, and the zoom with it.
	CHECK(Geometry::GetCommentBarRect(comment, 0.2f, 2.0f).Max.y == doctest::Approx(220.0f));
	// At 200 % the title is 36 pixels (18 units), within the bar's 80.
	CHECK(Geometry::GetCommentTitlePixels(2.0f, 1.0f) == 35.0f);
	CHECK(Geometry::GetCommentBarRect(comment, 2.0f, 1.0f).Max.y == doctest::Approx(40.0f));

	// Edges and corners within the tolerance; a corner sets two edges.
	CHECK(Geometry::GetCommentEdges(comment, glm::vec2(2.0f, 500.0f), 8.0f) == Geometry::LeftEdge);
	CHECK(Geometry::GetCommentEdges(comment, glm::vec2(405.0f, 500.0f), 8.0f) == Geometry::RightEdge);
	CHECK(Geometry::GetCommentEdges(comment, glm::vec2(398.0f, 997.0f), 8.0f) == (Geometry::RightEdge | Geometry::BottomEdge));
	CHECK(Geometry::GetCommentEdges(comment, glm::vec2(-3.0f, -3.0f), 8.0f) == (Geometry::LeftEdge | Geometry::TopEdge));
	CHECK(Geometry::GetCommentEdges(comment, glm::vec2(200.0f, 500.0f), 8.0f) == 0);
	CHECK(Geometry::GetCommentEdges(comment, glm::vec2(-20.0f, 500.0f), 8.0f) == 0);
}

TEST_CASE("VisualScriptEditor: badges keep a minimum size on the screen and sit in the header's badge slots")
{
	VisualScriptNodeVisual node = MakeNode();
	CHECK(Geometry::IsEmpty(Geometry::GetBadgeRect(node, 1.0f, 1.0f)));
	node.ErrorCount = 1;
	// 8 units at 100 %; never below 6 pixels: 60 units at 10 %, 30 at 20 %.
	CHECK(Geometry::GetBadgeRadius(1.0f, 1.0f) == doctest::Approx(8.0f));
	CHECK(Geometry::GetBadgeRadius(0.1f, 1.0f) == doctest::Approx(60.0f));
	CHECK(Geometry::GetBadgeRadius(0.4f, 2.0f) == doctest::Approx(30.0f));
	CHECK(Geometry::GetBadgeRadius(2.0f, 1.0f) == doctest::Approx(8.0f));

	// The error badge in the rightmost slot (its centre 18 inside the right edge, on the title's line); a warning left of it.
	std::optional<glm::vec2> const error = Geometry::GetErrorBadgeCenter(node);
	REQUIRE(error.has_value());
	CheckPoint(*error, glm::vec2(100.0f + 180.0f - 18.0f, 214.0f));
	node.WarningCount = 2;
	std::optional<glm::vec2> const warning = Geometry::GetWarningBadgeCenter(node, 1.0f, 1.0f);
	REQUIRE(warning.has_value());
	CheckPoint(*warning, glm::vec2(error->x - 16.0f, 214.0f));
	// Zoomed out, the badges grow apart rather than overlap.
	std::optional<glm::vec2> const farWarning = Geometry::GetWarningBadgeCenter(node, 0.1f, 1.0f);
	CHECK(farWarning->x == doctest::Approx(error->x - 120.0f));
	VisualScriptRect const rect = Geometry::GetBadgeRect(node, 1.0f, 1.0f);
	CheckPoint(rect.Min, glm::vec2(error->x - 24.0f, 206.0f));
	CheckPoint(rect.Max, glm::vec2(error->x + 8.0f, 222.0f));

	// Compact shapes have no header slots: the badge sits on the top-right corner.
	node.Shape = VisualScriptNodeShape::Operator;
	node.WarningCount = 0;
	CheckPoint(*Geometry::GetErrorBadgeCenter(node), glm::vec2(280.0f, 200.0f));
}

TEST_CASE("VisualScriptEditor: node rectangles, headers, pins and chips are placed from the visual model, with a move preview's offset")
{
	VisualScriptNodeVisual node = MakeNode();
	glm::vec2 const offset(5.0f, -10.0f);
	VisualScriptRect const rect = Geometry::GetNodeRect(node, offset);
	CheckPoint(rect.Min, glm::vec2(105.0f, 190.0f));
	CheckPoint(rect.Max, glm::vec2(285.0f, 280.0f));
	CheckPoint(Geometry::GetHeaderRect(node).Max, glm::vec2(280.0f, 228.0f));
	node.Subtitle.Length = 4;
	CheckPoint(Geometry::GetHeaderRect(node).Max, glm::vec2(280.0f, 244.0f));
	CheckPoint(Geometry::GetSubtitleRect(node).Min, glm::vec2(110.0f, 226.0f));
	CheckPoint(Geometry::GetTitleRect(node).Min, glm::vec2(110.0f, 206.5f));
	node.Kind = VisualScriptNodeKind::Event;
	CheckPoint(Geometry::GetTitleRect(node).Min, glm::vec2(126.0f, 206.5f));
	CheckPoint(Geometry::GetKindGlyphRect(node).Min, glm::vec2(112.0f, 208.0f));

	VisualScriptPinVisual input;
	input.Direction = VisualScriptPinDirection::In;
	input.Anchor = glm::vec2(10.0f, 46.0f);
	input.HitRect = {glm::vec2(0.0f, 34.0f), glm::vec2(60.0f, 58.0f)};
	input.ChipRect = {glm::vec2(66.0f, 36.5f), glm::vec2(130.0f, 55.5f)};
	input.Label.Length = 5;
	input.Label.Width = 39.0f;
	CheckPoint(Geometry::GetPinAnchor(node, input, offset), glm::vec2(115.0f, 236.0f));
	CheckPoint(Geometry::GetPinLabelRect(node, input).Min, glm::vec2(121.0f, 239.5f));
	CheckPoint(Geometry::GetChipRect(node, input, offset).Min, glm::vec2(171.0f, 226.5f));
	VisualScriptRect const cell = Geometry::GetChipCellRect(Geometry::GetChipRect(node, input), 2);
	CheckPoint(cell.Min, glm::vec2(166.0f + 92.0f, 236.5f));
	CHECK(cell.GetSize().x == doctest::Approx(44.0f));
	// The hit area is the row, grown around the glyph to the minimum.
	VisualScriptRect const hit = Geometry::GetPinHitRect(node, input, 20.0f);
	CheckPoint(hit.Min, glm::vec2(90.0f, 226.0f));
	CheckPoint(hit.Max, glm::vec2(160.0f, 266.0f));

	// A collapsed node anchors its pins at its header's edges; drawn expanded, at their rows.
	node.Flags |= VisualScriptNodeFlags::Collapsed;
	node.Size = glm::vec2(120.0f, 44.0f);
	CheckPoint(Geometry::GetPinAnchor(node, input), glm::vec2(100.0f, 222.0f));
	VisualScriptPinVisual output = input;
	output.Direction = VisualScriptPinDirection::Out;
	CheckPoint(Geometry::GetPinAnchor(node, output), glm::vec2(220.0f, 222.0f));
	VisualScriptNodeVisual const expanded = Geometry::GetExpandedNode(node);
	CHECK_FALSE(expanded.HasFlag(VisualScriptNodeFlags::Collapsed));
	CheckPoint(expanded.Size, node.ExpandedSize);
	CheckPoint(Geometry::GetPinAnchor(expanded, input), glm::vec2(110.0f, 246.0f));
	CheckPoint(Geometry::GetCollapsedMarkerRect(node).Min, glm::vec2(126.0f + 45.0f + 2.0f, 208.0f));

	// Strips belong to disabled and development-only nodes.
	CHECK(Geometry::IsEmpty(Geometry::GetEnabledStripRect(node)));
	node.Flags |= VisualScriptNodeFlags::Disabled;
	CheckPoint(Geometry::GetEnabledStripRect(node).Min, glm::vec2(100.0f, 224.0f));
}

TEST_CASE("VisualScriptEditor: the point at a fraction of a wire's drawn length lies on it, the fraction clamped")
{
	std::array<glm::vec2, 4> const polyline = {glm::vec2(0.0f, 0.0f), glm::vec2(10.0f, 0.0f), glm::vec2(10.0f, 0.0f),
	                                           glm::vec2(10.0f, 10.0f)};
	CheckPoint(Geometry::GetPolylinePoint(polyline, 0.0f), glm::vec2(0.0f, 0.0f));
	CheckPoint(Geometry::GetPolylinePoint(polyline, 0.25f), glm::vec2(5.0f, 0.0f));
	CheckPoint(Geometry::GetPolylinePoint(polyline, 0.5f), glm::vec2(10.0f, 0.0f));
	// The repeated point adds no length.
	CheckPoint(Geometry::GetPolylinePoint(polyline, 0.75f), glm::vec2(10.0f, 5.0f));
	CheckPoint(Geometry::GetPolylinePoint(polyline, 1.0f), glm::vec2(10.0f, 10.0f));
	CheckPoint(Geometry::GetPolylinePoint(polyline, -1.0f), glm::vec2(0.0f, 0.0f));
	CheckPoint(Geometry::GetPolylinePoint(polyline, 2.0f), glm::vec2(10.0f, 10.0f));
	CheckPoint(Geometry::GetPolylinePoint(polyline, std::numeric_limits<float>::quiet_NaN()), glm::vec2(0.0f, 0.0f));

	// It finds again the point MeasureDistance placed on a drawn wire.
	std::array<glm::vec2, 4> const points = {glm::vec2(0.0f, 0.0f), glm::vec2(200.0f, 0.0f), glm::vec2(200.0f, 300.0f),
	                                         glm::vec2(400.0f, 300.0f)};
	std::vector<glm::vec2> wire;
	Geometry::FlattenWire(points, 1.25f, Geometry::MaxSegments, wire);
	for (float t : {0.1f, 0.5f, 0.8f})
	{
		glm::vec2 const onCurve = EvaluateBezier(points, t);
		glm::vec2 const found = Geometry::GetPolylinePoint(wire, Geometry::MeasureDistance(wire, onCurve).ArcFraction);
		CHECK(glm::length(found - onCurve) < 1.0f);
	}

	// Degenerate polylines.
	CheckPoint(Geometry::GetPolylinePoint({}, 0.5f), glm::vec2(0.0f));
	std::array<glm::vec2, 1> const single = {glm::vec2(3.0f, 4.0f)};
	CheckPoint(Geometry::GetPolylinePoint(single, 0.5f), glm::vec2(3.0f, 4.0f));
}

TEST_CASE("VisualScriptEditor: the minimap sits in the canvas's bottom-right corner and maps the content's bounds, grown, at one scale")
{
	// 20 % of the canvas's width within 160 and 280 logical pixels, 0.65 times as high, 12 logical pixels inside the corner.
	VisualScriptRect rect = Geometry::GetMinimapRect(glm::vec2(100.0f, 50.0f), glm::vec2(1920.0f, 1080.0f), 1.0f);
	CheckPoint(rect.Max, glm::vec2(100.0f + 1920.0f - 12.0f, 50.0f + 1080.0f - 12.0f));
	CheckPoint(rect.GetSize(), glm::vec2(280.0f, 182.0f));
	CHECK(Geometry::GetMinimapRect(glm::vec2(0.0f), glm::vec2(1000.0f, 600.0f), 1.0f).GetSize().x == doctest::Approx(200.0f));
	CHECK(Geometry::GetMinimapRect(glm::vec2(0.0f), glm::vec2(500.0f, 600.0f), 1.0f).GetSize().x == doctest::Approx(160.0f));
	rect = Geometry::GetMinimapRect(glm::vec2(0.0f), glm::vec2(1000.0f, 600.0f), 2.0f);
	CHECK(rect.GetSize().x == doctest::Approx(320.0f));
	CheckPoint(rect.Max, glm::vec2(1000.0f - 24.0f, 600.0f - 24.0f));
	CHECK(Geometry::IsEmpty(Geometry::GetMinimapRect(glm::vec2(5.0f), glm::vec2(0.0f), 1.0f)));

	// Content of 1000 x 500 units, grown by 5 % of its larger side on every side, centred in the rectangle.
	VisualScriptRect const minimap{glm::vec2(0.0f), glm::vec2(280.0f, 182.0f)};
	VisualScriptMinimapTransform const transform =
		Geometry::GetMinimapTransform(minimap, VisualScriptRect{glm::vec2(0.0f), glm::vec2(1000.0f, 500.0f)});
	CheckPoint(transform.Bounds.Min, glm::vec2(-50.0f));
	CheckPoint(transform.Bounds.Max, glm::vec2(1050.0f, 550.0f));
	CHECK(transform.Scale == doctest::Approx(280.0f / 1100.0f));
	CheckPoint(transform.ToScreen(transform.Bounds.GetCenter()), minimap.GetCenter());
	for (glm::vec2 const point : {glm::vec2(0.0f), glm::vec2(1000.0f, 500.0f), glm::vec2(250.0f, 125.0f)})
	{
		CheckPoint(transform.ToCanvas(transform.ToScreen(point)), point);
		CHECK(Geometry::Contains(minimap, transform.ToScreen(point)));
	}
	// A single point of content still maps at a finite scale.
	VisualScriptMinimapTransform const point = Geometry::GetMinimapTransform(minimap, VisualScriptRect{glm::vec2(7.0f), glm::vec2(7.0f)});
	CHECK(std::isfinite(point.Scale));
	CHECK(point.Scale > 0.0f);
	CheckPoint(point.ToScreen(glm::vec2(7.0f)), minimap.GetCenter());
	// Nothing to show: no content, or no rectangle.
	CHECK(Geometry::GetMinimapTransform(minimap, std::nullopt).Scale == 0.0f);
	CHECK(Geometry::GetMinimapTransform(VisualScriptRect{}, VisualScriptRect{glm::vec2(0.0f), glm::vec2(10.0f)}).Scale == 0.0f);
}
