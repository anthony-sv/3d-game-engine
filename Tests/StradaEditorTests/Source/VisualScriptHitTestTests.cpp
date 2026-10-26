#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptHitTest.h"
#include "Editor/VisualScripting/VisualScriptSpatialIndex.h"
#include "Editor/VisualScripting/VisualScriptView.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	namespace Geometry = VisualScriptGeometry;
	using Direction = VisualScriptPinDirection;

	VisualScriptNode MakeNode(uint32_t id, std::string_view type, glm::vec2 position)
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::string(type);
		node.Position = position;
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	// A document drawn by the visual model with its spatial index, seen through a view whose screen pixels are canvas units at
	// 100 % (the canvas at the screen's origin, its centre on (800, 450)).
	struct HitFixture
	{
		Testing::FixtureVisualScriptSemantics Semantics;
		VisualScriptDocument Document;
		std::optional<VisualScriptAnalysis> Analysis;
		VisualScriptCanvasModel Model;
		VisualScriptSpatialIndex Index;
		VisualScriptView View;
		std::vector<uint8_t> Selected;
		std::vector<uint32_t> Z;
		VisualScriptHitContext Context;

		void Sync(std::span<VisualScriptDiagnostic const> problems = {})
		{
			Result<VisualScriptAnalysis> analysis = Testing::AnalyzeWithFixtureTemplates(Semantics.GetFixtureLibrary(), Document, problems);
			REQUIRE(analysis.IsOk());
			Analysis = analysis.TakeValue();
			VisualScriptCanvasSource source;
			source.Display = &Document;
			source.Analysis = &*Analysis;
			source.AnalyzedDocument = &Document;
			source.Library = &Semantics.GetFixtureLibrary();
			Model.Sync(source, Testing::FixedWidthMeasurer(0.5f), VisualScriptThemeAnchors());
			Index.Build(Model);
			View.SetCanvas(glm::vec2(0.0f), glm::vec2(1600.0f, 900.0f), 1.0f);
			SetZoom(1.0f);
			Selected.assign(Model.GetNodes().size(), 0);
			Z.assign(Model.GetNodes().size(), 0);
			Context.Selected = Selected;
			Context.Z = Z;
		}

		void SetZoom(float zoom) { View.SetState({0, glm::vec2(800.0f, 450.0f), zoom}); }

		VisualScriptHit Hit(glm::vec2 canvasPoint) const { return HitTestCanvas(Model, Index, View, Context, View.ToScreen(canvasPoint)); }

		uint32_t NodeIndex(uint32_t id) const { return *Model.FindNode(id); }

		glm::vec2 Anchor(uint32_t node, std::string_view pin, Direction direction = Direction::In) const
		{
			return View.ToCanvas(Testing::CanvasPoint(Model, View, node, pin, direction));
		}

		uint32_t PinIndex(uint32_t node, std::string_view pin, Direction direction = Direction::In) const
		{
			return *Model.FindPin(NodeIndex(node), pin, direction);
		}
	};

	void CheckHit(VisualScriptHit const& hit, VisualScriptHitKind kind, uint32_t element = 0)
	{
		CHECK(hit.Kind == kind);
		if (kind != VisualScriptHitKind::Link && kind != VisualScriptHitKind::Background && kind != VisualScriptHitKind::Minimap)
		{
			CHECK(hit.Element == element);
		}
	}
}

TEST_CASE(
	"VisualScriptEditor: hit testing tries the minimap, pins, badges, headers, bodies, reroutes, wires, comment edges and title bars, then the background")
{
	HitFixture fixture;
	VisualScriptComment comment;
	comment.ID = 10;
	comment.Position = glm::vec2(50.0f);
	comment.Size = glm::vec2(900.0f, 600.0f);
	fixture.Document.Graph.Comments = {comment};
	fixture.Document.Graph.Nodes = {MakeNode(1, "Flow.Branch", glm::vec2(100.0f)), MakeNode(2, "Flow.Branch", glm::vec2(400.0f, 100.0f)),
	                                MakeNode(3, "Reroute", glm::vec2(700.0f, 300.0f))};
	fixture.Document.Graph.Links = {MakeLink(1, "True", 2, "Execute")};
	VisualScriptDiagnostic error;
	error.Code = "InvalidValue";
	error.Node = 1;
	error.Message = "an error";
	fixture.Sync(std::span<VisualScriptDiagnostic const>(&error, 1));

	// The minimap takes hits before the canvas.
	fixture.Context.Minimap = VisualScriptRect{glm::vec2(1400.0f, 700.0f), glm::vec2(1590.0f, 890.0f)};
	CheckHit(HitTestCanvas(fixture.Model, fixture.Index, fixture.View, fixture.Context, glm::vec2(1500.0f, 800.0f)),
	         VisualScriptHitKind::Minimap);
	fixture.Context.Minimap.reset();

	// A pin: on its glyph, on its label, and a few pixels off its glyph.
	VisualScriptHit pin = fixture.Hit(fixture.Anchor(1, "Condition"));
	CheckHit(pin, VisualScriptHitKind::Pin, 1);
	CHECK(pin.Pin == fixture.PinIndex(1, "Condition"));
	CHECK(fixture.Hit(fixture.Anchor(1, "Condition") + glm::vec2(40.0f, 0.0f)).Pin == fixture.PinIndex(1, "Condition"));
	CHECK(fixture.Hit(fixture.Anchor(1, "False", Direction::Out) + glm::vec2(5.0f, 4.0f)).Pin ==
	      fixture.PinIndex(1, "False", Direction::Out));

	// The badge, the header, the body.
	std::span<VisualScriptNodeVisual const> const nodes = fixture.Model.GetNodes();
	VisualScriptNodeVisual const& node = nodes[fixture.NodeIndex(1)];
	CheckHit(fixture.Hit(*Geometry::GetErrorBadgeCenter(node)), VisualScriptHitKind::Badge, 1);
	CheckHit(fixture.Hit(glm::vec2(100.0f + 60.0f, 110.0f)), VisualScriptHitKind::NodeHeader, 1);
	CheckHit(fixture.Hit(glm::vec2(100.0f + 100.0f, 100.0f + 81.0f)), VisualScriptHitKind::Node, 1);

	// The reroute's dot, within 10 pixels of its centre.
	CheckHit(fixture.Hit(glm::vec2(706.0f, 306.0f)), VisualScriptHitKind::Reroute, 3);
	CheckHit(fixture.Hit(glm::vec2(706.0f + 9.0f, 306.0f)), VisualScriptHitKind::Reroute, 3);
	CheckHit(fixture.Hit(glm::vec2(706.0f + 11.0f, 306.0f)), VisualScriptHitKind::Background);

	// The wire: on its curve, with where on it; 7 pixels off it is the background.
	std::array<glm::vec2, 4> const points = Geometry::GetLinkControlPoints(fixture.Model, 0);
	glm::vec2 const middle = (points[0] + points[1] * 3.0f + points[2] * 3.0f + points[3]) / 8.0f;
	VisualScriptHit const wire = fixture.Hit(middle);
	CheckHit(wire, VisualScriptHitKind::Link);
	REQUIRE(wire.Link.has_value());
	CHECK(*wire.Link == fixture.Document.Graph.Links[0]);
	CHECK(wire.ArcFraction == doctest::Approx(0.5f).epsilon(0.05));
	CheckHit(fixture.Hit(middle + glm::vec2(0.0f, 7.0f)), VisualScriptHitKind::Background);

	// The comment: its edges (a corner sets two), its title bar; its body is background.
	VisualScriptHit const edge = fixture.Hit(glm::vec2(53.0f, 400.0f));
	CheckHit(edge, VisualScriptHitKind::CommentEdge, 10);
	CHECK(edge.Edges == Geometry::LeftEdge);
	CHECK(fixture.Hit(glm::vec2(948.0f, 648.0f)).Edges == (Geometry::RightEdge | Geometry::BottomEdge));
	CheckHit(fixture.Hit(glm::vec2(600.0f, 70.0f)), VisualScriptHitKind::CommentTitle, 10);
	CheckHit(fixture.Hit(glm::vec2(600.0f, 500.0f)), VisualScriptHitKind::Background);
	// Outside everything.
	CheckHit(fixture.Hit(glm::vec2(1200.0f, 800.0f)), VisualScriptHitKind::Background);
}

TEST_CASE("VisualScriptEditor: the top drawn node is the one hit: selected nodes, then raised ones, then later ones in the document")
{
	HitFixture fixture;
	fixture.Document.Graph.Nodes = {MakeNode(1, "Flow.Branch", glm::vec2(100.0f)), MakeNode(2, "Flow.Branch", glm::vec2(150.0f, 120.0f))};
	fixture.Sync();
	glm::vec2 const overlap(220.0f, 180.0f);
	CHECK(fixture.Hit(overlap).Element == 2);
	fixture.Z[fixture.NodeIndex(1)] = 5;
	CHECK(fixture.Hit(overlap).Element == 1);
	fixture.Selected[fixture.NodeIndex(2)] = 1;
	CHECK(fixture.Hit(overlap).Element == 2);
	fixture.Selected[fixture.NodeIndex(1)] = 1;
	CHECK(fixture.Hit(overlap).Element == 1);

	// The renderer's order: unselected before selected, then raised, then by index.
	CHECK(GetVisualScriptNodeDrawKey(false, 0, 7) < GetVisualScriptNodeDrawKey(false, 0, 8));
	CHECK(GetVisualScriptNodeDrawKey(false, 1, 0) > GetVisualScriptNodeDrawKey(false, 0, 9000));
	CHECK(GetVisualScriptNodeDrawKey(true, 0, 0) > GetVisualScriptNodeDrawKey(false, 0xFFFFFFFFu, 9000));
}

TEST_CASE("VisualScriptEditor: a pin is hit only on the topmost node under the mouse, and not at all at Far")
{
	HitFixture fixture;
	// The second node covers the first's True output.
	fixture.Document.Graph.Nodes = {MakeNode(1, "Flow.Branch", glm::vec2(0.0f, 100.0f)),
	                                MakeNode(2, "Flow.Branch", glm::vec2(100.0f, 100.0f))};
	fixture.Sync();
	glm::vec2 const covered = fixture.Anchor(1, "True", Direction::Out);
	CheckHit(fixture.Hit(covered), VisualScriptHitKind::Node, 2);
	fixture.Z[fixture.NodeIndex(1)] = 1;
	CheckHit(fixture.Hit(covered), VisualScriptHitKind::Pin, 1);
	fixture.Z[fixture.NodeIndex(1)] = 0;

	// Where pin labels are drawn (Detail, and Compact from 13 z >= 7), a pin is hit over its label's row; elsewhere pins are
	// dots, hit only around their glyphs, and the rest of the row is the node's body (a press there selects and moves it).
	glm::vec2 const condition = fixture.Anchor(2, "Condition");
	glm::vec2 const conditionRow = condition + glm::vec2(40.0f, 0.0f);
	CheckHit(fixture.Hit(conditionRow), VisualScriptHitKind::Pin, 2);
	fixture.SetZoom(0.6f);
	REQUIRE(fixture.View.GetDetailLevel() == VisualScriptDetailLevel::Compact);
	CHECK(Geometry::ArePinLabelsDrawn(VisualScriptDetailLevel::Compact, 0.6f));
	CheckHit(fixture.Hit(conditionRow), VisualScriptHitKind::Pin, 2);
	fixture.SetZoom(0.5f);
	REQUIRE(fixture.View.GetDetailLevel() == VisualScriptDetailLevel::Compact);
	CHECK_FALSE(Geometry::ArePinLabelsDrawn(VisualScriptDetailLevel::Compact, 0.5f));
	CheckHit(fixture.Hit(conditionRow), VisualScriptHitKind::Node, 2);
	CheckHit(fixture.Hit(condition), VisualScriptHitKind::Pin, 2);

	// At Overview pins are dots and still hit around their glyphs; at Far only nodes are.
	fixture.SetZoom(0.3f);
	CHECK_FALSE(Geometry::ArePinLabelsDrawn(VisualScriptDetailLevel::Overview, 0.3f));
	CheckHit(fixture.Hit(condition), VisualScriptHitKind::Pin, 2);
	CheckHit(fixture.Hit(conditionRow), VisualScriptHitKind::Node, 2);
	fixture.SetZoom(0.1f);
	CHECK(fixture.View.GetDetailLevel() == VisualScriptDetailLevel::Far);
	CHECK(fixture.Hit(condition).Kind != VisualScriptHitKind::Pin);
	CHECK(fixture.Hit(condition).Element == 2);
}

TEST_CASE("VisualScriptEditor: a collapsed node is hit at its two anchors, and with its rows while it is drawn expanded")
{
	HitFixture fixture;
	VisualScriptNode branch = MakeNode(1, "Flow.Branch", glm::vec2(100.0f));
	branch.Collapsed = true;
	fixture.Document.Graph.Nodes = {branch};
	fixture.Sync();

	// The input anchor stands for every input: the hit names the first.
	VisualScriptHit const input = fixture.Hit(glm::vec2(100.0f, 114.0f));
	CheckHit(input, VisualScriptHitKind::Pin, 1);
	CHECK(input.Pin == fixture.PinIndex(1, "Execute"));
	CHECK(fixture.Hit(glm::vec2(220.0f, 114.0f)).Pin == fixture.PinIndex(1, "True", Direction::Out));
	// Where the Condition row would be is outside the collapsed node.
	glm::vec2 const conditionRow(110.0f, 170.0f);
	CheckHit(fixture.Hit(conditionRow), VisualScriptHitKind::Background);

	// Expanded under a dragged wire, the rows are hit.
	fixture.Context.Preview.Expanded = fixture.NodeIndex(1);
	VisualScriptHit const row = fixture.Hit(conditionRow);
	CheckHit(row, VisualScriptHitKind::Pin, 1);
	CHECK(row.Pin == fixture.PinIndex(1, "Condition"));
}

TEST_CASE("VisualScriptEditor: moved nodes and their wires are hit where the move preview draws them")
{
	HitFixture fixture;
	fixture.Document.Graph.Nodes = {MakeNode(1, "Event.Create", glm::vec2(100.0f)), MakeNode(2, "Flow.Branch", glm::vec2(400.0f, 100.0f))};
	fixture.Document.Graph.Links = {MakeLink(1, "Then", 2, "Execute")};
	fixture.Sync();
	uint32_t const branch = fixture.NodeIndex(2);
	std::vector<uint32_t> const nodes = {branch};
	std::span<uint32_t const> const incident = fixture.Model.GetIncidentLinks(branch);
	std::vector<uint32_t> const links(incident.begin(), incident.end());
	fixture.Index.SetDynamic(nodes, {}, links);
	std::vector<uint8_t> moved(fixture.Model.GetNodes().size(), 0);
	moved[branch] = 1;
	fixture.Context.Preview.Moved = moved;
	fixture.Context.Preview.Offset = glm::vec2(0.0f, 300.0f);
	fixture.Context.DynamicLinks = links;

	glm::vec2 const body(400.0f + 90.0f, 100.0f + 81.0f);
	CheckHit(fixture.Hit(body), VisualScriptHitKind::Background);
	CheckHit(fixture.Hit(body + glm::vec2(0.0f, 300.0f)), VisualScriptHitKind::Node, 2);
	// The wire ends at the moved Execute.
	std::array<glm::vec2, 4> const points = Geometry::GetLinkControlPoints(fixture.Model, 0, fixture.Context.Preview);
	glm::vec2 const middle = (points[0] + points[1] * 3.0f + points[2] * 3.0f + points[3]) / 8.0f;
	CheckHit(fixture.Hit(middle), VisualScriptHitKind::Link);
	// The wire runs nearly straight down there: 30 to its side is off it.
	CheckHit(fixture.Hit(middle - glm::vec2(30.0f, 0.0f)), VisualScriptHitKind::Background);
}

TEST_CASE("VisualScriptEditor: a hit test queries the index once, within at most 4 cells, at every zoom and UI scale")
{
	HitFixture fixture;
	fixture.Document = Testing::MakeGridGraph(30, 30, 3, 10);
	fixture.Sync();
	VisualScriptHitScratch scratch;
	fixture.Context.Scratch = &scratch;
	for (float const scale : {1.0f, 2.0f})
	{
		fixture.View.SetCanvas(glm::vec2(0.0f), glm::vec2(1600.0f, 900.0f), scale);
		for (float const zoom : VisualScriptZoomLevels)
		{
			CAPTURE(zoom);
			fixture.View.SetState({0, glm::vec2(4800.0f, 2700.0f), zoom});
			for (glm::vec2 const mouse : {glm::vec2(800.0f, 450.0f), glm::vec2(13.0f, 17.0f), glm::vec2(1590.0f, 880.0f)})
			{
				VisualScriptIndexStats stats;
				fixture.Context.Stats = &stats;
				static_cast<void>(HitTestCanvas(fixture.Model, fixture.Index, fixture.View, fixture.Context, mouse));
				CHECK(stats.CellsVisited <= 4);
			}
		}
	}
}

TEST_CASE("VisualScriptEditor: a node comment's bubble is hit as its node where its text is drawn, and moved nodes can be looked through")
{
	HitFixture fixture;
	VisualScriptNode branch = MakeNode(1, "Flow.Branch", glm::vec2(100.0f, 300.0f));
	branch.Comment = "Checks the condition";
	fixture.Document.Graph.Nodes = {branch, MakeNode(2, "Event.Create", glm::vec2(500.0f, 100.0f)),
	                                MakeNode(3, "Flow.Branch", glm::vec2(900.0f, 100.0f))};
	fixture.Document.Graph.Links = {MakeLink(2, "Then", 3, "Execute")};
	fixture.Sync();
	VisualScriptRect const bubble = Geometry::GetBubbleRect(fixture.Model.GetNodes()[fixture.NodeIndex(1)]);
	REQUIRE_FALSE(Geometry::IsEmpty(bubble));

	// At Detail and Compact the bubble shows its text, and a press on it is on its node.
	CheckHit(fixture.Hit(bubble.GetCenter()), VisualScriptHitKind::Node, 1);
	fixture.SetZoom(0.5f);
	REQUIRE(fixture.View.GetDetailLevel() == VisualScriptDetailLevel::Compact);
	CheckHit(fixture.Hit(bubble.GetCenter()), VisualScriptHitKind::Node, 1);
	// At Overview it shows no text: what lies under it is hit.
	fixture.SetZoom(0.3f);
	REQUIRE(fixture.View.GetDetailLevel() == VisualScriptDetailLevel::Overview);
	CheckHit(fixture.Hit(bubble.GetCenter()), VisualScriptHitKind::Background);
	fixture.SetZoom(1.0f);

	// The first Branch dragged onto the middle of the wire: hit where the preview draws it, unless moved nodes are looked
	// through, which finds the wire a dragged node may be dropped on.
	std::array<glm::vec2, 4> const points = Geometry::GetLinkControlPoints(fixture.Model, 0);
	glm::vec2 const middle = (points[0] + points[1] * 3.0f + points[2] * 3.0f + points[3]) / 8.0f;
	uint32_t const moved = fixture.NodeIndex(1);
	VisualScriptRect const rect = Geometry::GetNodeRect(fixture.Model.GetNodes()[moved]);
	std::vector<uint32_t> const dynamic = {moved};
	fixture.Index.SetDynamic(dynamic, {}, {});
	std::vector<uint8_t> movedMask(fixture.Model.GetNodes().size(), 0);
	movedMask[moved] = 1;
	fixture.Context.Preview.Moved = movedMask;
	fixture.Context.Preview.Offset = middle - rect.GetCenter();
	CheckHit(fixture.Hit(middle), VisualScriptHitKind::Node, 1);
	fixture.Context.SkipMoved = true;
	VisualScriptHit const under = fixture.Hit(middle);
	CheckHit(under, VisualScriptHitKind::Link);
	REQUIRE(under.Link.has_value());
	CHECK(*under.Link == fixture.Document.Graph.Links[0]);
	// Where the node was before the move is empty either way.
	CheckHit(fixture.Hit(rect.GetCenter()), VisualScriptHitKind::Background);
	fixture.Context.SkipMoved = false;
	CheckHit(fixture.Hit(rect.GetCenter()), VisualScriptHitKind::Background);
}
