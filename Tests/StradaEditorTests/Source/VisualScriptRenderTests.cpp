#include "TestUtilities.h"
#include "VisualScriptPanelTestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptBrushes.h"
#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasRenderer.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;
namespace Elements = VisualScriptDrawElements;

namespace
{
	VisualScriptNode MakeNode(uint32_t id, std::string type, glm::vec2 position, Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = position;
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t fromNode, std::string fromPin, uint32_t toNode, std::string toPin)
	{
		return {{fromNode, std::move(fromPin)}, {toNode, std::move(toPin)}};
	}

	// Create (1) -> Delay (2, a stored duration) -> Branch (3) -> Log Info (4, a stored message) in a row, an Add (5, compact) and
	// a reroute (6) below, and the comment box "Start" (20) around Create and Delay.
	VisualScriptDocument MakeRenderDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes = {
			MakeNode(1, "Event.Create", glm::vec2(0.0f, 0.0f)),
			MakeNode(2, "Flow.Delay", glm::vec2(320.0f, 0.0f), Json::object({{"Duration", 0.5}})),
			MakeNode(3, "Flow.Branch", glm::vec2(640.0f, 0.0f)),
			MakeNode(4, "Call:Strada.Log.Info(string)", glm::vec2(960.0f, 0.0f), Json::object({{"Message", "Hello"}})),
			MakeNode(5, "Operator.Add", glm::vec2(320.0f, 240.0f)),
			MakeNode(6, "Reroute", glm::vec2(800.0f, 260.0f)),
		};
		document.Graph.Links = {MakeLink(1, "Then", 2, "Execute"), MakeLink(2, "Completed", 3, "Execute"),
		                        MakeLink(3, "True", 4, "Execute")};
		VisualScriptComment comment;
		comment.ID = 20;
		comment.Text = "Start\nThe first steps";
		comment.Position = glm::vec2(-48.0f, -80.0f);
		comment.Size = glm::vec2(640.0f, 240.0f);
		document.Graph.Comments = {comment};
		return document;
	}

	std::vector<VisualScriptDrawRecordItem> ItemsOf(VisualScriptDrawRecord const& record, VisualScriptDrawItemKind kind)
	{
		std::vector<VisualScriptDrawRecordItem> items;
		std::copy_if(record.Items.begin(), record.Items.end(), std::back_inserter(items),
		             [kind](VisualScriptDrawRecordItem const& item)
		             {
						 return item.Kind == kind;
					 });
		return items;
	}
}

TEST_CASE("VisualScriptEditor: drawing the canvas changes nothing")
{
	Testing::CanvasRenderFixture fixture(MakeRenderDocument());
	Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(fixture.Project.Graph);
	uint64_t const version = asset->GetVersion();
	Ref<VisualScriptDocument const> const document = asset->GetSharedDocument();
	Result<std::string> const bytes = FileSystem::ReadTextFile(fixture.Project.File);
	REQUIRE(bytes.IsOk());
	VisualScriptViewState const view = fixture.Model.GetViewState();
	uint64_t const revision = fixture.Model.GetCanvasRevision();
	std::vector<uint32_t> const selection(fixture.Model.GetSelection().begin(), fixture.Model.GetSelection().end());

	for (int frame = 0; frame < 3; frame++)
	{
		fixture.Frame();
		CHECK(!fixture.Record.Items.empty());
	}
	CHECK(asset->GetVersion() == version);
	CHECK(asset->GetSharedDocument() == document);
	Result<std::string> const after = FileSystem::ReadTextFile(fixture.Project.File);
	REQUIRE(after.IsOk());
	CHECK(after.GetValue() == bytes.GetValue());
	CHECK(fixture.Model.GetViewState() == view);
	CHECK(fixture.Model.GetCanvasRevision() == revision);
	CHECK(std::vector<uint32_t>(fixture.Model.GetSelection().begin(), fixture.Model.GetSelection().end()) == selection);
	CHECK(fixture.Intents.empty());
}

TEST_CASE("VisualScriptEditor: each level of detail draws its elements of a node")
{
	Testing::CanvasRenderFixture fixture(MakeRenderDocument());
	glm::vec2 const center(480.0f, 60.0f);

	SUBCASE("Detail at 100 %")
	{
		fixture.SetView(center, 1.0f);
		VisualScriptDrawRecordItem const& delay = fixture.Item(VisualScriptDrawItemKind::Node, 2);
		CHECK(delay.Level == VisualScriptDetailLevel::Detail);
		CHECK(delay.Has(Elements::Shadow | Elements::Body | Elements::Header | Elements::Title | Elements::Pins | Elements::PinLabels |
		                Elements::Chips | Elements::KindGlyph));
		CHECK(!delay.Has(Elements::ChipText));
		VisualScriptDrawRecordItem const& add = fixture.Item(VisualScriptDrawItemKind::Node, 5);
		CHECK(add.Has(Elements::Body | Elements::Title | Elements::Pins));
		CHECK(!add.Has(Elements::PinLabels));
		CHECK(fixture.Item(VisualScriptDrawItemKind::Reroute, 6).Has(Elements::Body));
		VisualScriptDrawRecordItem const& comment = fixture.Item(VisualScriptDrawItemKind::Comment, 20);
		CHECK(comment.Has(Elements::Body | Elements::Header | Elements::Title | Elements::BodyText | Elements::Border));
	}
	SUBCASE("Compact at 60 %: labels and values as text")
	{
		fixture.SetView(center, 0.6f);
		VisualScriptDrawRecordItem const& delay = fixture.Item(VisualScriptDrawItemKind::Node, 2);
		CHECK(delay.Level == VisualScriptDetailLevel::Compact);
		CHECK(delay.Has(Elements::Shadow | Elements::Body | Elements::Header | Elements::Title | Elements::Pins | Elements::PinLabels |
		                Elements::ChipText));
		CHECK(!delay.Has(Elements::Chips));
		CHECK(!delay.Has(Elements::Subtitle));
	}
	SUBCASE("Compact at 50 %: labels and values too small to draw")
	{
		fixture.SetView(center, 0.5f);
		VisualScriptDrawRecordItem const& delay = fixture.Item(VisualScriptDrawItemKind::Node, 2);
		CHECK(delay.Level == VisualScriptDetailLevel::Compact);
		CHECK(delay.Has(Elements::Body | Elements::Title | Elements::Pins));
		CHECK(!delay.Has(Elements::PinLabels));
		CHECK(!delay.Has(Elements::ChipText));
		CHECK(!delay.Has(Elements::Chips));
		VisualScriptDrawRecordItem const& comment = fixture.Item(VisualScriptDrawItemKind::Comment, 20);
		CHECK(comment.Has(Elements::Title));
		CHECK(!comment.Has(Elements::BodyText));
	}
	SUBCASE("Overview at 30 %: boxes with titles and pin dots")
	{
		fixture.SetView(center, 0.3f);
		VisualScriptDrawRecordItem const& delay = fixture.Item(VisualScriptDrawItemKind::Node, 2);
		CHECK(delay.Level == VisualScriptDetailLevel::Overview);
		CHECK(delay.Has(Elements::Body | Elements::Title | Elements::Pins));
		CHECK(!delay.Has(Elements::Header));
		CHECK(!delay.Has(Elements::Shadow));
		CHECK(!delay.Has(Elements::PinLabels));
		VisualScriptDrawRecordItem const& comment = fixture.Item(VisualScriptDrawItemKind::Comment, 20);
		CHECK(comment.Has(Elements::Title));
	}
	SUBCASE("Far at 12 %: rectangles")
	{
		fixture.SetView(center, 0.12f);
		VisualScriptDrawRecordItem const& delay = fixture.Item(VisualScriptDrawItemKind::Node, 2);
		CHECK(delay.Level == VisualScriptDetailLevel::Far);
		CHECK(delay.Elements == Elements::Body);
	}
}

TEST_CASE("VisualScriptEditor: the canvas draws what its view shows, and wires that cross it")
{
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(1, "Event.Create", glm::vec2(-2400.0f, 0.0f)), MakeNode(2, "Flow.Branch", glm::vec2(2400.0f, 0.0f)),
	                        MakeNode(3, "Flow.Delay", glm::vec2(0.0f, 2000.0f))};
	document.Graph.Links = {MakeLink(1, "Then", 2, "Execute")};
	Testing::CanvasRenderFixture fixture(document);
	fixture.SetView(glm::vec2(0.0f), 1.0f);

	CHECK(fixture.Record.Find(VisualScriptDrawItemKind::Node, 1) == nullptr);
	CHECK(fixture.Record.Find(VisualScriptDrawItemKind::Node, 2) == nullptr);
	CHECK(fixture.Record.Find(VisualScriptDrawItemKind::Node, 3) == nullptr);
	CHECK(fixture.Renderer.GetStats().VisibleNodes == 0);
	REQUIRE(ItemsOf(fixture.Record, VisualScriptDrawItemKind::Wire).size() == 1);
	CHECK(fixture.Renderer.GetStats().DrawnWires == 1);

	fixture.SetView(glm::vec2(0.0f, 2000.0f), 1.0f);
	CHECK(fixture.Record.Find(VisualScriptDrawItemKind::Node, 3) != nullptr);
	CHECK(ItemsOf(fixture.Record, VisualScriptDrawItemKind::Wire).empty());
}

TEST_CASE("VisualScriptEditor: wires are drawn before nodes, and selected nodes after the others")
{
	Testing::CanvasRenderFixture fixture(MakeRenderDocument());
	fixture.SetView(glm::vec2(480.0f, 60.0f), 1.0f);
	std::array<uint32_t, 1> const selected = {2};
	fixture.Model.Select(selected);
	fixture.Frame();

	std::vector<VisualScriptDrawRecordItem> const wires = ItemsOf(fixture.Record, VisualScriptDrawItemKind::Wire);
	std::vector<VisualScriptDrawRecordItem> const nodes = ItemsOf(fixture.Record, VisualScriptDrawItemKind::Node);
	REQUIRE(wires.size() == 3);
	REQUIRE(nodes.size() >= 4);
	uint32_t lastWire = 0;
	for (VisualScriptDrawRecordItem const& wire : wires)
	{
		lastWire = std::max(lastWire, wire.Order);
	}
	uint32_t lastUnselected = 0;
	for (VisualScriptDrawRecordItem const& node : nodes)
	{
		CHECK(node.Order > lastWire);
		if (node.Element != 2)
		{
			lastUnselected = std::max(lastUnselected, node.Order);
		}
	}
	VisualScriptDrawRecordItem const& delay = fixture.Item(VisualScriptDrawItemKind::Node, 2);
	CHECK(delay.Layer == VisualScriptDrawLayer::SelectedNodes);
	CHECK(delay.Order > lastUnselected);
	CHECK(delay.Has(Elements::Outline | Elements::Glow));
	CHECK(delay.Color == fixture.Theme.Selection);
	// The wires of the selected node are highlighted, after the others.
	std::span<VisualScriptLinkVisual const> const links = fixture.Model.GetCanvas().GetLinks();
	std::span<VisualScriptNodeVisual const> const canvasNodes = fixture.Model.GetCanvas().GetNodes();
	for (VisualScriptDrawRecordItem const& wire : wires)
	{
		VisualScriptLinkVisual const& link = links[wire.Element];
		bool const incident = canvasNodes[link.FromNode].ID == 2 || canvasNodes[link.ToNode].ID == 2;
		CHECK((wire.Layer == VisualScriptDrawLayer::HighlightedWires) == incident);
	}
	// The comment and the grid come first.
	CHECK(fixture.Record.Items.front().Kind == VisualScriptDrawItemKind::Grid);
	CHECK(fixture.Item(VisualScriptDrawItemKind::Comment, 20).Order < wires.front().Order);
}

TEST_CASE("VisualScriptEditor: a UI scale of 2 doubles every node's rectangle and draws the same elements")
{
	Testing::CanvasRenderFixture fixture(MakeRenderDocument());
	fixture.SetView(glm::vec2(480.0f, 60.0f), 1.0f);
	std::vector<VisualScriptDrawRecordItem> const before = ItemsOf(fixture.Record, VisualScriptDrawItemKind::Node);
	REQUIRE(!before.empty());

	fixture.SetUIScale(2.0f);
	fixture.Frame();
	// The view shows half as much of the canvas: the nodes still in view are compared.
	std::vector<VisualScriptDrawRecordItem> const after = ItemsOf(fixture.Record, VisualScriptDrawItemKind::Node);
	REQUIRE(after.size() >= 3);
	glm::vec2 const center(800.0f, 450.0f);
	for (VisualScriptDrawRecordItem const& scaled : after)
	{
		CAPTURE(scaled.Element);
		auto const found = std::find_if(before.begin(), before.end(),
		                                [&scaled](VisualScriptDrawRecordItem const& item)
		                                {
											return item.Element == scaled.Element;
										});
		REQUIRE(found != before.end());
		CHECK(scaled.Level == found->Level);
		CHECK(scaled.Elements == found->Elements);
		// Twice as large about the canvas's centre, which shows the same canvas point.
		CHECK(scaled.Screen.Min.x - center.x == doctest::Approx((found->Screen.Min.x - center.x) * 2.0f).epsilon(0.001));
		CHECK(scaled.Screen.Min.y - center.y == doctest::Approx((found->Screen.Min.y - center.y) * 2.0f).epsilon(0.001));
		CHECK(scaled.Screen.Max.x - center.x == doctest::Approx((found->Screen.Max.x - center.x) * 2.0f).epsilon(0.001));
		CHECK(scaled.Screen.Max.y - center.y == doctest::Approx((found->Screen.Max.y - center.y) * 2.0f).epsilon(0.001));
	}
	ImGui::GetStyle().FontScaleDpi = 1.0f;
}

namespace
{
	// The record's item of a link's wire; null when it was not drawn.
	VisualScriptDrawRecordItem const* FindWire(Testing::CanvasRenderFixture const& fixture, uint32_t fromNode, uint32_t toNode)
	{
		VisualScriptCanvasModel const& canvas = fixture.Model.GetCanvas();
		std::span<VisualScriptLinkVisual const> const links = canvas.GetLinks();
		std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
		for (VisualScriptDrawRecordItem const& item : fixture.Record.Items)
		{
			if (item.Kind != VisualScriptDrawItemKind::Wire)
			{
				continue;
			}
			VisualScriptLinkVisual const& link = links[item.Element];
			if (nodes[link.FromNode].ID == fromNode && nodes[link.ToNode].ID == toNode)
			{
				return &item;
			}
		}
		return nullptr;
	}

	// The record's pulse item; null when no pulse was drawn.
	VisualScriptDrawRecordItem const* FindPulse(Testing::CanvasRenderFixture const& fixture)
	{
		auto const found = std::find_if(fixture.Record.Items.begin(), fixture.Record.Items.end(),
		                                [](VisualScriptDrawRecordItem const& item)
		                                {
											return item.Kind == VisualScriptDrawItemKind::Pulse;
										});
		return found != fixture.Record.Items.end() ? &*found : nullptr;
	}

	uint32_t GetRgb(uint32_t color)
	{
		return color & ~IM_COL32_A_MASK;
	}

	uint32_t GetAlpha(uint32_t color)
	{
		return color >> IM_COL32_A_SHIFT;
	}

	// The canvas point at the middle of an input's value chip.
	glm::vec2 ChipCenter(VisualScriptEditorModel const& model, uint32_t node, std::string_view pin)
	{
		VisualScriptCanvasModel const& canvas = model.GetCanvas();
		std::optional<uint32_t> const index = canvas.FindNode(node);
		REQUIRE(index.has_value());
		std::optional<uint32_t> const pinIndex = canvas.FindPin(*index, pin, VisualScriptPinDirection::In);
		REQUIRE(pinIndex.has_value());
		VisualScriptRect const chip = VisualScriptGeometry::GetChipRect(canvas.GetNodes()[*index], canvas.GetPins()[*pinIndex]);
		REQUIRE(!VisualScriptGeometry::IsEmpty(chip));
		return chip.GetCenter();
	}

	// Draws each live chip it is asked for with ImGui's own widgets, as the literal editor's compact form draws them (a drag
	// per number or component, a check box, a colour button, a frame for choices), and keeps their frames.
	class RecordingChipHost final : public VisualScriptLiveChipHost
	{
	public:
		void DrawLiveChip(VisualScriptLiveChip const& chip) override
		{
			Chips.push_back(chip);
			Frames.clear();
			VisualScriptLiveChipScope const scope(chip);
			FontSize = ImGui::GetFontSize();
			ImGui::PushID("LiveChip");
			auto const keep = [this]
			{
				Frames.push_back({glm::vec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y),
				                  glm::vec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y)});
			};
			switch (chip.Kind)
			{
				case VisualScriptChipKind::Number:
					ImGui::SetNextItemWidth(chip.CellWidth);
					ImGui::DragFloat("##Value", &m_Value, 0.1f, 0.0f, 0.0f, "%g");
					keep();
					break;
				case VisualScriptChipKind::Cells:
					for (uint32_t cell = 0; cell < chip.CellCount; cell++)
					{
						if (cell > 0)
						{
							ImGui::SameLine();
						}
						ImGui::PushID(static_cast<int>(cell));
						ImGui::SetNextItemWidth(chip.CellWidth);
						ImGui::DragFloat("##Cell", &m_Value, 0.1f, 0.0f, 0.0f, "%g");
						ImGui::PopID();
						keep();
					}
					break;
				case VisualScriptChipKind::Check:
					ImGui::Checkbox("##Check", &m_Checked);
					keep();
					break;
				case VisualScriptChipKind::Color:
					ImGui::ColorButton("##Color", ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
					keep();
					break;
				case VisualScriptChipKind::Choice:
				case VisualScriptChipKind::Asset:
				case VisualScriptChipKind::Text:
					ImGui::Button("##Frame", ImVec2(chip.CellWidth, 0.0f));
					keep();
					break;
				case VisualScriptChipKind::None:
				case VisualScriptChipKind::Matrix:
				case VisualScriptChipKind::Placeholder:
				case VisualScriptChipKind::Raw:
					break;
			}
			ImGui::PopID();
		}

		std::vector<VisualScriptLiveChip> Chips;
		std::vector<VisualScriptRect> Frames;
		float FontSize = 0.0f;

	private:
		float m_Value = 0.5f;
		bool m_Checked = false;
	};

	// Delay (2, a float), Branch (3, a bool), Add Force (7, a vector and an enum) and Color Lerp (8, colours), apart.
	VisualScriptDocument MakeChipDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes = {
			MakeNode(2, "Flow.Delay", glm::vec2(0.0f, 0.0f), Json::object({{"Duration", 0.5}})),
			MakeNode(3, "Flow.Branch", glm::vec2(0.0f, 400.0f)),
			MakeNode(7, "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)", glm::vec2(0.0f, 800.0f),
		             Json::object({{"Force", Json::array({1, 0, 0})}})),
			MakeNode(8, "Call:Strada.Color.Lerp(Strada.Color,Strada.Color,float)", glm::vec2(0.0f, 1200.0f)),
		};
		return document;
	}

	// The fixture's analysis with the reroutes' wildcards resolved to entities, as the scripting runtime resolves a reroute
	// that carries Self (the fixture analyzer resolves no wildcard).
	class EntityRerouteSemantics final : public VisualScriptSemantics
	{
	public:
		explicit EntityRerouteSemantics(Testing::FixtureVisualScriptSemantics& fixture)
			: m_Fixture(fixture)
		{
		}

		Ref<VisualScriptLibrary const> GetLibrary() override { return m_Fixture.GetLibrary(); }

		[[nodiscard]] Result<VisualScriptAnalysis> Analyze(VisualScriptDocument const& document) override
		{
			Result<VisualScriptAnalysis> analysis = m_Fixture.Analyze(document);
			if (!analysis.IsOk())
			{
				return analysis;
			}
			Json source = analysis.GetValue().Source;
			for (Json& node : source["Nodes"])
			{
				if (node["Type"] != "Reroute")
				{
					continue;
				}
				for (Json& pin : node["Pins"])
				{
					pin["Type"] = "Strada.Entity";
				}
			}
			return VisualScriptAnalysis::Parse(source.dump());
		}

	private:
		Testing::FixtureVisualScriptSemantics& m_Fixture;
	};
}

TEST_CASE("VisualScriptEditor: an implicit conversion's wire fades from one type's colour to the other's along its length")
{
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(6, "Flow.ForEach", glm::vec2(0.0f, 0.0f)), MakeNode(2, "Flow.Delay", glm::vec2(400.0f, 120.0f))};
	document.Graph.Links = {MakeLink(6, "Index", 2, "Duration")};
	Testing::CanvasRenderFixture fixture(document);
	fixture.SetView(glm::vec2(300.0f, 80.0f), 1.0f);

	VisualScriptDrawRecordItem const* wire = FindWire(fixture, 6, 2);
	REQUIRE(wire != nullptr);
	std::span<VisualScriptLinkVisual const> const links = fixture.Model.GetCanvas().GetLinks();
	VisualScriptLinkVisual const& link = links[wire->Element];
	REQUIRE(link.HasFlag(VisualScriptLinkFlags::Converted));
	REQUIRE(GetRgb(link.FromColor) != GetRgb(link.ToColor));
	CHECK(wire->Has(Elements::Gradient));
	// AddPolyline's textured path: two vertices per point, in the polyline's order (whole widths, anti-aliased lines).
	REQUIRE(wire->PointCount >= 2);
	CHECK(wire->VertexEnd - wire->VertexBegin == wire->PointCount * 2);
	ImDrawList const* drawList = fixture.Draw.DrawList;
	REQUIRE(static_cast<int>(wire->VertexEnd) <= drawList->VtxBuffer.Size);
	CHECK(GetRgb(drawList->VtxBuffer[static_cast<int>(wire->VertexBegin)].col) == GetRgb(link.FromColor));
	CHECK(GetRgb(drawList->VtxBuffer[static_cast<int>(wire->VertexBegin) + 1].col) == GetRgb(link.FromColor));
	CHECK(GetRgb(drawList->VtxBuffer[static_cast<int>(wire->VertexEnd) - 1].col) == GetRgb(link.ToColor));
	CHECK(GetAlpha(drawList->VtxBuffer[static_cast<int>(wire->VertexBegin)].col) == GetAlpha(wire->Color));

	// Without anti-aliased lines AddPolyline writes four vertices per segment: ImGui's linear gradient between the ends instead.
	ImGui::GetStyle().AntiAliasedLines = false;
	fixture.Frame();
	wire = FindWire(fixture, 6, 2);
	REQUIRE(wire != nullptr);
	CHECK(wire->Has(Elements::GradientFallback));
	CHECK(!wire->Has(Elements::Gradient));
	CHECK(wire->VertexEnd - wire->VertexBegin == (wire->PointCount - 1) * 4);
	ImGui::GetStyle().AntiAliasedLines = true;
}

TEST_CASE("VisualScriptEditor: the brushes draw shadows and glows from the font atlas, and follow it when it grows")
{
	Testing::CanvasRenderFixture fixture(MakeRenderDocument());
	fixture.SetView(glm::vec2(480.0f, 60.0f), 1.0f);
	std::array<uint32_t, 1> const selected = {2};
	fixture.Model.Select(selected);
	fixture.Frame();

	VisualScriptDrawRecordItem const* delay = &fixture.Item(VisualScriptDrawItemKind::Node, 2);
	CHECK(delay->Has(Elements::Shadow | Elements::Glow));
	CHECK(fixture.Renderer.GetStats().BrushDraws > 0);
	CHECK(fixture.Brushes.IsReady());
	CHECK(fixture.Brushes.GetAddCount() == 1);
	auto const checkShadowUVs = [&fixture](VisualScriptDrawRecordItem const& item)
	{
		// The node's first 16 vertices are its shadow's 9-slice, inside the soft rectangle's current UVs.
		ImDrawList const* drawList = fixture.Draw.DrawList;
		ImVec2 const uv0 = fixture.Brushes.GetSoftRectUV0();
		ImVec2 const uv1 = fixture.Brushes.GetSoftRectUV1();
		for (uint32_t vertex = item.VertexBegin; vertex < item.VertexBegin + 16; vertex++)
		{
			ImVec2 const uv = drawList->VtxBuffer[static_cast<int>(vertex)].uv;
			CHECK(uv.x >= uv0.x - 1e-6f);
			CHECK(uv.x <= uv1.x + 1e-6f);
			CHECK(uv.y >= uv0.y - 1e-6f);
			CHECK(uv.y <= uv1.y + 1e-6f);
		}
	};
	checkShadowUVs(*delay);

	// A growth recreates the texture and moves every rectangle: the next frame reads the UVs again.
	ImFontAtlas* const atlas = ImGui::GetIO().Fonts;
	ImTextureData const* const texture = atlas->TexData;
	ImVec2 const uvBefore = fixture.Brushes.GetSoftRectUV1();
	ImFontAtlasTextureGrow(atlas);
	REQUIRE(atlas->TexData != texture);
	fixture.Frame();
	delay = &fixture.Item(VisualScriptDrawItemKind::Node, 2);
	CHECK(delay->Has(Elements::Shadow));
	CHECK((fixture.Brushes.GetSoftRectUV1().x != uvBefore.x || fixture.Brushes.GetSoftRectUV1().y != uvBefore.y));
	CHECK(fixture.Brushes.GetAddCount() == 1);
	checkShadowUVs(*delay);

	// Without brushes the canvas leaves its soft shapes out and draws everything else.
	fixture.UseBrushes = false;
	fixture.Frame();
	delay = &fixture.Item(VisualScriptDrawItemKind::Node, 2);
	CHECK(!delay->Has(Elements::Shadow));
	CHECK(!delay->Has(Elements::Glow));
	CHECK(delay->Has(Elements::Body | Elements::Title | Elements::Outline));
	CHECK(fixture.Renderer.GetStats().BrushDraws == 0);
}

TEST_CASE("VisualScriptEditor: the brushes are added to the current context's atlas once, and again in another context")
{
	VisualScriptBrushes brushes;
	CHECK(!brushes.Prepare());
	{
		Testing::HeadlessImGui imgui;
		imgui.Frame(
			[&brushes]
			{
				CHECK(brushes.Prepare());
				CHECK(brushes.Prepare());
			});
		CHECK(brushes.GetAddCount() == 1);
	}
	CHECK(!brushes.Prepare());
	Testing::HeadlessImGui other;
	other.Frame(
		[&brushes]
		{
			CHECK(brushes.Prepare());
			ImVec2 const uv0 = brushes.GetSoftDotUV0();
			ImVec2 const uv1 = brushes.GetSoftDotUV1();
			CHECK(uv1.x > uv0.x);
			CHECK(uv1.y > uv0.y);
		});
	CHECK(brushes.GetAddCount() == 2);
}

TEST_CASE("VisualScriptEditor: a new link pulses for a moment, and only highlights with Reduce Motion")
{
	Testing::CanvasRenderFixture fixture(MakeRenderDocument());
	fixture.SetView(glm::vec2(480.0f, 60.0f), 1.0f);
	VisualScriptDocument linked = fixture.Model.GetDisplayDocument();
	linked.Graph.Links.push_back(MakeLink(3, "False", 4, "Execute"));

	SUBCASE("moving")
	{
		fixture.Settings.SetReduceMotion(false);
		fixture.Commit(linked);
		REQUIRE(FindPulse(fixture) != nullptr);
		CHECK(FindPulse(fixture)->Has(Elements::PulseDot | Elements::PulseHighlight));
		for (int frame = 0; frame < 3; frame++)
		{
			fixture.Frame(0.1);
			REQUIRE(FindPulse(fixture) != nullptr);
			CHECK(FindPulse(fixture)->Has(Elements::PulseDot));
		}
		// Past 0.35 s the dot has arrived; the wire's highlight decays until 0.6 s.
		fixture.Frame(0.1);
		REQUIRE(FindPulse(fixture) != nullptr);
		CHECK(!FindPulse(fixture)->Has(Elements::PulseDot));
		CHECK(FindPulse(fixture)->Has(Elements::PulseHighlight));
		fixture.Frame(0.5);
		CHECK(FindPulse(fixture) == nullptr);
		VisualScriptDrawRecordItem const* wire = FindWire(fixture, 3, 4);
		REQUIRE(wire != nullptr);
		CHECK(!wire->Has(Elements::PulseHighlight));
	}
	SUBCASE("with Reduce Motion")
	{
		fixture.Commit(linked);
		REQUIRE(FindPulse(fixture) != nullptr);
		CHECK(!FindPulse(fixture)->Has(Elements::PulseDot));
		CHECK(FindPulse(fixture)->Has(Elements::PulseHighlight));
		fixture.Frame(0.3);
		REQUIRE(FindPulse(fixture) != nullptr);
		CHECK(!FindPulse(fixture)->Has(Elements::PulseDot));
		fixture.Frame(0.6);
		CHECK(FindPulse(fixture) == nullptr);
	}
}

TEST_CASE("VisualScriptEditor: elements changed outside the window flash, and an Arrange glides")
{
	Testing::CanvasRenderFixture fixture(MakeRenderDocument());
	fixture.SetView(glm::vec2(480.0f, 60.0f), 1.0f);
	fixture.Settings.SetReduceMotion(false);
	VisualScriptDocument moved = fixture.Model.GetDisplayDocument();
	moved.Graph.Nodes[1].Position = glm::vec2(336.0f, 16.0f);
	fixture.ChangeOutside(moved);
	fixture.Frame();
	CHECK(fixture.Item(VisualScriptDrawItemKind::Node, 2).Has(Elements::Flash));
	CHECK(!fixture.Item(VisualScriptDrawItemKind::Node, 3).Has(Elements::Flash));
	fixture.Frame(0.5);
	CHECK(fixture.Item(VisualScriptDrawItemKind::Node, 2).Has(Elements::Flash));
	fixture.Frame(0.5);
	CHECK(!fixture.Item(VisualScriptDrawItemKind::Node, 2).Has(Elements::Flash));

	// An Arrange's glide: drawn from the old place toward the new one, landing there after 0.25 s.
	VisualScriptRect const settled = fixture.Item(VisualScriptDrawItemKind::Node, 3).Screen;
	std::array<std::pair<uint32_t, glm::vec2>, 1> const from = {{{3, glm::vec2(640.0f, 200.0f)}}};
	fixture.Renderer.StartGlide(from, fixture.Time);
	fixture.Frame(0.05);
	VisualScriptRect const gliding = fixture.Item(VisualScriptDrawItemKind::Node, 3).Screen;
	CHECK(gliding.Min.y > settled.Min.y + 1.0f);
	CHECK(gliding.Min.y < settled.Min.y + 200.0f);
	fixture.Frame(0.25);
	CHECK(fixture.Item(VisualScriptDrawItemKind::Node, 3).Screen == settled);
}

TEST_CASE("VisualScriptEditor: the live widget replaces the hovered chip at Detail, with frames equal to the chip's cells")
{
	Testing::CanvasRenderFixture fixture(MakeChipDocument(), glm::vec2(1600.0f, 900.0f), true);
	RecordingChipHost host;
	fixture.Draw.LiveChips = &host;

	struct Target
	{
		uint32_t Node;
		std::string_view Pin;
		VisualScriptChipKind Kind;
	};
	std::array<Target, 5> const targets = {{{2, "Duration", VisualScriptChipKind::Number},
	                                        {7, "Force", VisualScriptChipKind::Cells},
	                                        {3, "Condition", VisualScriptChipKind::Check},
	                                        {7, "Mode", VisualScriptChipKind::Choice},
	                                        {8, "A", VisualScriptChipKind::Color}}};
	for (float const scale : {1.0f, 2.0f})
	{
		fixture.SetUIScale(scale);
		for (float const zoom : {0.7f, 1.0f, 2.0f})
		{
			for (Target const& target : targets)
			{
				CAPTURE(scale);
				CAPTURE(zoom);
				CAPTURE(target.Node);
				CAPTURE(target.Pin);
				glm::vec2 const center = ChipCenter(fixture.Model, target.Node, target.Pin);
				fixture.SetView(center, zoom);
				host.Chips.clear();
				fixture.MoveTo(fixture.Model.GetView().ToScreen(center));
				fixture.Frame();
				CHECK(fixture.Renderer.GetStats().LiveWidgets == 1);
				REQUIRE(!host.Chips.empty());
				VisualScriptLiveChip const& chip = host.Chips.back();
				CHECK(chip.Node == target.Node);
				CHECK(chip.Kind == target.Kind);
				float const k = fixture.Model.GetView().GetPixelsPerUnit();
				CHECK(host.FontSize == VisualScriptCanvasMetrics::LadderFloor(VisualScriptCanvasMetrics::ChipTextSize * k));
				CHECK(fixture.Item(VisualScriptDrawItemKind::Node, target.Node).Has(Elements::LiveWidget));

				// The widget's frames lie on the chip's cells, which lie on the visual model's chip (whole-pixel widths).
				VisualScriptCanvasModel const& canvas = fixture.Model.GetCanvas();
				std::optional<uint32_t> const index = canvas.FindNode(target.Node);
				REQUIRE(index.has_value());
				std::optional<uint32_t> const pin = canvas.FindPin(*index, target.Pin, VisualScriptPinDirection::In);
				REQUIRE(pin.has_value());
				VisualScriptRect const chipRect = VisualScriptGeometry::GetChipRect(canvas.GetNodes()[*index], canvas.GetPins()[*pin]);
				REQUIRE(host.Frames.size() == chip.CellCount);
				for (uint32_t cell = 0; cell < chip.CellCount; cell++)
				{
					CAPTURE(cell);
					VisualScriptRect const expected = chip.GetCell(cell);
					CHECK(std::abs(host.Frames[cell].Min.x - expected.Min.x) <= 0.5f);
					CHECK(std::abs(host.Frames[cell].Min.y - expected.Min.y) <= 0.5f);
					CHECK(std::abs(host.Frames[cell].Max.x - expected.Max.x) <= 0.5f);
					CHECK(std::abs(host.Frames[cell].Max.y - expected.Max.y) <= 0.5f);
					VisualScriptRect const layout =
						target.Kind == VisualScriptChipKind::Cells ? VisualScriptGeometry::GetChipCellRect(chipRect, cell) : chipRect;
					glm::vec2 const lower = fixture.Model.GetView().ToScreen(layout.Min);
					glm::vec2 const upper = fixture.Model.GetView().ToScreen(layout.Max);
					CHECK(std::abs(expected.Min.y - lower.y) <= 0.01f);
					CHECK(std::abs(expected.Max.y - upper.y) <= 0.01f);
					// Each cell loses less than a pixel to the whole-pixel width.
					CHECK(expected.Max.x <= upper.x + 0.01f);
					CHECK(expected.Max.x > upper.x - 1.0f - static_cast<float>(cell));
				}
				CHECK(std::abs(chip.GetCell(0).Min.x - fixture.Model.GetView().ToScreen(chipRect.Min).x) <= 0.01f);
				// One draw command per 64K vertices, and at most two clip changes for the widget.
				VisualScriptRenderStats const& stats = fixture.Renderer.GetStats();
				CHECK(stats.DrawCommands <= (stats.Vertices + 65535u) / 65536u + 2u);
			}
		}
	}

	// Only at Detail, and only for the chip under the mouse.
	glm::vec2 const center = ChipCenter(fixture.Model, 2, "Duration");
	fixture.SetView(center, 0.6f);
	fixture.MoveTo(fixture.Model.GetView().ToScreen(center));
	CHECK(fixture.Renderer.GetStats().LiveWidgets == 0);
	fixture.SetView(center, 1.0f);
	fixture.MoveTo(fixture.Model.GetView().ToScreen(center) + glm::vec2(0.0f, -200.0f));
	CHECK(fixture.Renderer.GetStats().LiveWidgets == 0);
	// The chip whose widget is in use stays live while the mouse is elsewhere.
	VisualScriptPinReference const active{2, "Duration"};
	fixture.Draw.ActiveChip = &active;
	fixture.Frame();
	CHECK(fixture.Renderer.GetStats().LiveWidgets == 1);
	fixture.Draw.ActiveChip = nullptr;
	ImGui::GetStyle().FontScaleDpi = 1.0f;
}

TEST_CASE("VisualScriptEditor: wires the cut line crosses are dashed and faded, and the splice target has a halo")
{
	Testing::CanvasRenderFixture fixture(MakeRenderDocument());
	fixture.SetView(glm::vec2(480.0f, 60.0f), 1.0f);

	SUBCASE("cut")
	{
		glm::vec2 const crossing = Testing::WirePoint(fixture.Model, MakeLink(1, "Then", 2, "Execute"));
		fixture.Input.Ctrl = true;
		fixture.MoveTo(crossing + glm::vec2(0.0f, -60.0f));
		fixture.Input.Down[1] = true;
		fixture.Input.Pressed[1] = true;
		fixture.Frame();
		for (int step = 1; step <= 4; step++)
		{
			fixture.MoveTo(crossing + glm::vec2(0.0f, -60.0f + 30.0f * static_cast<float>(step)));
		}
		REQUIRE(fixture.Model.GetGesture().Kind == VisualScriptGesture::Cutting);
		VisualScriptDrawRecordItem const* wire = FindWire(fixture, 1, 2);
		REQUIRE(wire != nullptr);
		CHECK(wire->Has(Elements::Dashed | Elements::Faded));
		CHECK(GetAlpha(wire->Color) <= static_cast<uint32_t>(0.42f * 255.0f));
		CHECK(GetAlpha(wire->Color) >= static_cast<uint32_t>(0.35f * 255.0f));
		VisualScriptDrawRecordItem const* other = FindWire(fixture, 2, 3);
		REQUIRE(other != nullptr);
		CHECK(!other->Has(Elements::Dashed));
		CHECK(fixture.Record.Count(VisualScriptDrawItemKind::CutLine) == 1);
		fixture.Input.Down[1] = false;
		fixture.Input.Released[1] = true;
		fixture.Input.Ctrl = false;
		fixture.Frame();
	}
	SUBCASE("splice")
	{
		// A second Delay, unlinked, dragged onto the wire from Delay to Branch and held there.
		VisualScriptDocument document = fixture.Model.GetDisplayDocument();
		document.Graph.Nodes.push_back(MakeNode(9, "Flow.Delay", glm::vec2(480.0f, 300.0f)));
		fixture.Commit(document);
		glm::vec2 const grab = Testing::CanvasPoint(fixture.Model, 9, glm::vec2(0.5f, 0.1f));
		glm::vec2 const target = Testing::WirePoint(fixture.Model, MakeLink(2, "Completed", 3, "Execute"));
		fixture.MoveTo(grab);
		fixture.Input.Down[0] = true;
		fixture.Input.Pressed[0] = true;
		fixture.Frame();
		for (int step = 1; step <= 4; step++)
		{
			fixture.MoveTo(glm::mix(grab, target, static_cast<float>(step) / 4.0f));
		}
		for (int frame = 0; frame < 30; frame++)
		{
			fixture.Frame();
		}
		REQUIRE(fixture.Model.GetGesture().SpliceTarget != nullptr);
		VisualScriptDrawRecordItem const* wire = FindWire(fixture, 2, 3);
		REQUIRE(wire != nullptr);
		CHECK(wire->Has(Elements::Halo));
		CHECK(wire->Layer == VisualScriptDrawLayer::HighlightedWires);
		VisualScriptDrawRecordItem const* other = FindWire(fixture, 1, 2);
		REQUIRE(other != nullptr);
		CHECK(!other->Has(Elements::Halo));
		fixture.Model.CancelGesture();
		fixture.Input.Down[0] = false;
		fixture.Frame();
	}
}

TEST_CASE("VisualScriptEditor: rings around type colours keep their gap and turn to the text colour where the state's is too close")
{
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(1, "Self", glm::vec2(0.0f, 0.0f)), MakeNode(6, "Reroute", glm::vec2(240.0f, 8.0f)),
	                        MakeNode(3, "Flow.Branch", glm::vec2(0.0f, 200.0f))};
	Testing::CanvasRenderFixture fixture(document);
	// The link is committed after the semantics are replaced: a change of the program is what analyzes again.
	document.Graph.Links = {MakeLink(1, "Self", 6, "In")};
	EntityRerouteSemantics semantics(fixture.Semantics);
	fixture.SemanticsOverride = &semantics;
	VisualScriptDiagnostic problem;
	problem.Severity = VisualScriptSeverity::Error;
	problem.Code = "TypeMismatch";
	problem.Node = 3;
	problem.Pin = "Condition";
	problem.Direction = VisualScriptPinDirection::In;
	problem.Message = "the condition is wrong";
	fixture.Semantics.InjectDiagnostic(problem);
	fixture.Commit(document);
	fixture.SetView(glm::vec2(160.0f, 120.0f), 1.0f);
	std::array<uint32_t, 1> const selected = {6};
	fixture.Model.Select(selected);
	fixture.Frame();

	// A selected reroute carrying entities: the selection's colour is nearly the Entity colour.
	VisualScriptDrawRecordItem const& reroute = fixture.Item(VisualScriptDrawItemKind::Reroute, 6);
	uint32_t const entity = GetVisualScriptTypeColor("Strada.Entity", &fixture.Semantics.GetFixtureLibrary());
	REQUIRE(reroute.Color == entity);
	CHECK(GetVisualScriptColorDistance(fixture.Theme.Selection, entity) < VisualScriptRingMinDistance);
	CHECK(reroute.RingColor == GetVisualScriptRingColor(fixture.Theme.Selection, entity, fixture.Theme));
	CHECK(reroute.RingColor == fixture.Theme.Text);
	CHECK(reroute.GapColor == fixture.Theme.Canvas);

	// An error on a Bool pin: the error colour is nearly the Bool colour.
	VisualScriptCanvasModel const& canvas = fixture.Model.GetCanvas();
	std::optional<uint32_t> const branch = canvas.FindNode(3);
	REQUIRE(branch.has_value());
	std::optional<uint32_t> const condition = canvas.FindPin(*branch, "Condition", VisualScriptPinDirection::In);
	REQUIRE(condition.has_value());
	auto const record = std::find_if(fixture.Record.Pins.begin(), fixture.Record.Pins.end(),
	                                 [&condition](VisualScriptDrawRecordPin const& pin)
	                                 {
										 return pin.Pin == *condition;
									 });
	REQUIRE(record != fixture.Record.Pins.end());
	CHECK((record->States & VisualScriptPinDrawStates::Problem) != 0);
	uint32_t const boolean = canvas.GetPins()[*condition].Color;
	CHECK(GetVisualScriptColorDistance(VisualScriptErrorColor, boolean) < VisualScriptRingMinDistance);
	CHECK(record->RingColor == fixture.Theme.Text);
	CHECK(record->GapColor == fixture.Theme.Body);
}

TEST_CASE("VisualScriptEditor: disabled and development-only nodes draw their labels on pills, and a disabled node its bypass")
{
	Testing::CanvasRenderFixture fixture(MakeRenderDocument());
	// The states are committed after the pass-through is set up: a change of the program is what analyzes again.
	fixture.Semantics.InjectPassThrough(2, "Completed");
	VisualScriptDocument document = fixture.Model.GetDisplayDocument();
	document.Graph.Nodes[1].Enabled = VisualScriptNodeEnabled::Disabled;
	document.Graph.Nodes[3].Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
	fixture.Commit(document);
	fixture.SetView(glm::vec2(640.0f, 60.0f), 1.0f);

	VisualScriptDrawRecordItem const& disabled = fixture.Item(VisualScriptDrawItemKind::Node, 2);
	CHECK(disabled.Has(Elements::Strip | Elements::StripLabel | Elements::Bypass));
	CHECK(!disabled.Has(Elements::Hatching));
	VisualScriptDrawRecordItem const& development = fixture.Item(VisualScriptDrawItemKind::Node, 4);
	CHECK(development.Has(Elements::Strip | Elements::StripLabel | Elements::Hatching));
	CHECK(!development.Has(Elements::Bypass));
	// The wires of a disabled node are faded with it.
	VisualScriptDrawRecordItem const* wire = FindWire(fixture, 1, 2);
	REQUIRE(wire != nullptr);
	CHECK(wire->Has(Elements::Faded));
	VisualScriptDrawRecordItem const* other = FindWire(fixture, 3, 4);
	REQUIRE(other != nullptr);
	CHECK(!other->Has(Elements::Faded));
}

TEST_CASE("VisualScriptEditor: pins and wires name their types in a pill after the short hover delay")
{
	Testing::CanvasRenderFixture fixture(MakeRenderDocument(), glm::vec2(1600.0f, 900.0f), true);
	fixture.SetView(glm::vec2(480.0f, 60.0f), 1.0f);

	SUBCASE("a pin with a stored value")
	{
		fixture.MoveTo(Testing::CanvasPoint(fixture.Model, 2, "Duration"));
		CHECK(fixture.Record.Count(VisualScriptDrawItemKind::Pill) == 0);
		for (int frame = 0; frame < 30; frame++)
		{
			fixture.Frame();
		}
		VisualScriptDrawRecordItem const* pill = fixture.Record.Find(VisualScriptDrawItemKind::Pill, 2);
		REQUIRE(pill != nullptr);
		CHECK(pill->Text == "Duration \xC2\xB7 Float (float)\nDefault: 0.5");
		CHECK(pill->Layer == VisualScriptDrawLayer::Overlay);
		CHECK(VisualScriptGeometry::Contains({glm::vec2(0.0f), glm::vec2(1600.0f, 900.0f)}, pill->Screen.Max));
	}
	SUBCASE("an exec pin whose label is hidden")
	{
		fixture.MoveTo(Testing::CanvasPoint(fixture.Model, 2, "Execute"));
		for (int frame = 0; frame < 30; frame++)
		{
			fixture.Frame();
		}
		VisualScriptDrawRecordItem const* pill = fixture.Record.Find(VisualScriptDrawItemKind::Pill, 2);
		REQUIRE(pill != nullptr);
		CHECK(pill->Text == "Execute \xC2\xB7 exec");
	}
	SUBCASE("a wire")
	{
		fixture.MoveTo(Testing::WirePoint(fixture.Model, MakeLink(3, "True", 4, "Execute")));
		for (int frame = 0; frame < 30; frame++)
		{
			fixture.Frame();
		}
		std::vector<VisualScriptDrawRecordItem> const pills = ItemsOf(fixture.Record, VisualScriptDrawItemKind::Pill);
		REQUIRE(pills.size() == 1);
		CHECK(pills.front().Text == "True \xE2\x86\x92 Execute");
		VisualScriptDrawRecordItem const* wire = FindWire(fixture, 3, 4);
		REQUIRE(wire != nullptr);
		CHECK(wire->Layer == VisualScriptDrawLayer::HighlightedWires);
	}
}

namespace
{
	// The fixture's analysis with Add's wildcards resolved to float, as the scripting runtime resolves an Add of two floats (the
	// fixture analyzer resolves no wildcard).
	class FloatAddSemantics final : public VisualScriptSemantics
	{
	public:
		explicit FloatAddSemantics(Testing::FixtureVisualScriptSemantics& fixture)
			: m_Fixture(fixture)
		{
		}

		Ref<VisualScriptLibrary const> GetLibrary() override { return m_Fixture.GetLibrary(); }

		[[nodiscard]] Result<VisualScriptAnalysis> Analyze(VisualScriptDocument const& document) override
		{
			Result<VisualScriptAnalysis> analysis = m_Fixture.Analyze(document);
			if (!analysis.IsOk())
			{
				return analysis;
			}
			Json source = analysis.GetValue().Source;
			for (Json& node : source["Nodes"])
			{
				if (node["Type"] != "Operator.Add")
				{
					continue;
				}
				for (Json& pin : node["Pins"])
				{
					pin["Type"] = "float";
				}
			}
			return VisualScriptAnalysis::Parse(source.dump());
		}

	private:
		Testing::FixtureVisualScriptSemantics& m_Fixture;
	};
}

TEST_CASE("VisualScriptEditor: a wildcard pin's pill names the type it resolved to, or says it is unresolved")
{
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(6, "Operator.Add", glm::vec2(0.0f, 0.0f))};
	Testing::CanvasRenderFixture fixture(document, glm::vec2(1600.0f, 900.0f), true);
	fixture.SetView(glm::vec2(60.0f, 40.0f), 1.0f);
	auto const restOn = [&fixture](std::string_view pin, VisualScriptPinDirection direction = VisualScriptPinDirection::In)
	{
		fixture.MoveTo(glm::vec2(-1000.0f));
		fixture.MoveTo(Testing::CanvasPoint(fixture.Model, 6, pin, direction));
		for (int frame = 0; frame < 30; frame++)
		{
			fixture.Frame();
		}
		VisualScriptDrawRecordItem const* pill = fixture.Record.Find(VisualScriptDrawItemKind::Pill, 6);
		REQUIRE(pill != nullptr);
		return pill->Text;
	};
	std::string const wildcard(VisualScriptWildcardType);

	// The fixture analyzer resolves none: unresolved.
	std::string const unresolved = restOn("A");
	CHECK(unresolved.find(std::string(VisualScriptCanvasTexts::Unresolved)) != std::string::npos);
	CHECK(unresolved.find(std::string(VisualScriptCanvasTexts::ResolvedTo)) == std::string::npos);

	// Resolved by the analysis: "* resolved to" the type.
	FloatAddSemantics resolving(fixture.Semantics);
	fixture.SemanticsOverride = &resolving;
	fixture.Semantics.ReloadLibrary();
	fixture.Frame();
	fixture.Frame();
	std::string const resolved = restOn("B");
	CHECK(resolved.find(wildcard + std::string(VisualScriptCanvasTexts::ResolvedTo)) != std::string::npos);
	CHECK(resolved.find(std::string(VisualScriptCanvasTexts::Unresolved)) == std::string::npos);
	CHECK(restOn("Result", VisualScriptPinDirection::Out).find(wildcard + std::string(VisualScriptCanvasTexts::ResolvedTo)) !=
	      std::string::npos);
}

TEST_CASE("VisualScriptEditor: the minimap shows the graph while some of it is out of view")
{
	Testing::CanvasRenderFixture fixture(MakeRenderDocument());
	fixture.SetView(glm::vec2(480.0f, 60.0f), 2.0f);
	for (int frame = 0; frame < 20; frame++)
	{
		fixture.Frame();
	}
	VisualScriptMinimapState const& minimap = fixture.Model.GetMinimap();
	REQUIRE(minimap.Alpha > 0.0f);
	VisualScriptDrawRecordItem const* item = fixture.Record.Find(VisualScriptDrawItemKind::Minimap, 0);
	REQUIRE(item != nullptr);
	CHECK(item->Layer == VisualScriptDrawLayer::Minimap);
	CHECK(item->Screen == minimap.Transform.Rect);
	CHECK(item->Order == fixture.Record.Items.back().Order);
}

TEST_CASE("VisualScriptEditor: idle frames of the canvas reserve nothing more")
{
	Testing::CanvasRenderFixture fixture(Testing::MakeGridGraph(20, 10, 3, 10));
	fixture.SetView(glm::vec2(3200.0f, 900.0f), 0.7f);
	std::array<uint32_t, 2> const selected = {1, 2};
	fixture.Model.Select(selected);
	fixture.MoveTo(Testing::CanvasPoint(fixture.Model, 1, glm::vec2(0.5f)));
	for (int frame = 0; frame < 5; frame++)
	{
		fixture.Frame();
	}
	size_t const capacity = fixture.Renderer.GetCapacityBytes();
	CHECK(capacity > 0);
	for (int frame = 0; frame < 10; frame++)
	{
		fixture.Frame();
		CHECK(fixture.Renderer.GetCapacityBytes() == capacity);
	}
}

TEST_CASE("VisualScriptEditor: the graph window's live widget, the literal editor's compact form, has the chip's cells at every zoom "
          "and UI scale, and its popups the window's size")
{
	Testing::PanelFixture fixture(MakeChipDocument());
	fixture.CheckDrawingChangesNothing();
	struct Target
	{
		uint32_t Node;
		std::string_view Pin;
	};
	std::array<Target, 5> const targets = {{{2, "Duration"}, {7, "Force"}, {3, "Condition"}, {7, "Mode"}, {8, "A"}}};
	auto const show = [&fixture](glm::vec2 center, float zoom)
	{
		VisualScriptOpenRequest request;
		request.Graph = fixture.Graph;
		request.Center = center;
		request.Zoom = zoom;
		request.Focus = false;
		fixture.Panel->Apply(request);
		fixture.Frames(2);
	};
	for (float const scale : {1.0f, 2.0f})
	{
		if (scale != 1.0f)
		{
			fixture.SetUIScale(scale);
		}
		for (float const zoom : {0.7f, 1.0f, 2.0f})
		{
			for (Target const& target : targets)
			{
				CAPTURE(scale);
				CAPTURE(zoom);
				CAPTURE(target.Node);
				CAPTURE(target.Pin);
				glm::vec2 const center = ChipCenter(fixture.Model(), target.Node, target.Pin);
				show(center, zoom);
				fixture.MoveMouse(fixture.Model().GetView().ToScreen(center));
				fixture.Frames(1);
				std::optional<VisualScriptLiveWidgetFrames> const& live = fixture.Panel->GetLiveWidget();
				REQUIRE(live.has_value());
				CHECK(live->Chip.Node == target.Node);
				// The widget's frame spans the chip's cells, and its last cell is the chip's last.
				VisualScriptRect const first = live->Chip.GetCell(0);
				VisualScriptRect const last = live->Chip.GetCell(std::max(live->Chip.CellCount, 1u) - 1u);
				CHECK(std::abs(live->Frame.Min.x - first.Min.x) <= 0.5f);
				CHECK(std::abs(live->Frame.Min.y - first.Min.y) <= 0.5f);
				CHECK(std::abs(live->Frame.Max.x - last.Max.x) <= 0.5f);
				CHECK(std::abs(live->Frame.Max.y - last.Max.y) <= 0.5f);
				CHECK(std::abs(live->LastCell.Min.x - last.Min.x) <= 0.5f);
				CHECK(std::abs(live->LastCell.Max.x - last.Max.x) <= 0.5f);
				VisualScriptRenderStats const& stats = fixture.Panel->GetRenderer().GetStats();
				CHECK(stats.LiveWidgets == 1);
				CHECK(stats.DrawCommands <= (stats.Vertices + 65535u) / 65536u + 2u);
			}
		}
	}

	// An enum chip's list opens at the window's size and padding, however far the canvas is zoomed in.
	glm::vec2 const mode = ChipCenter(fixture.Model(), 7, "Mode");
	show(mode, 2.0f);
	glm::vec2 const point = fixture.Model().GetView().ToScreen(mode);
	fixture.MoveMouse(point);
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetLiveWidget().has_value());
	CHECK(GImGui->OpenPopupStack.Size == 0);
	fixture.Click(point);
	fixture.Frames(2);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	ImGuiWindow const* popup = GImGui->OpenPopupStack.back().Window;
	REQUIRE(popup != nullptr);
	ImGuiStyle const& style = ImGui::GetStyle();
	CHECK(popup->FontRefSize == doctest::Approx(style.FontSizeBase * style.FontScaleMain * style.FontScaleDpi));
	CHECK(popup->WindowPadding.x == doctest::Approx(style.WindowPadding.x));
	CHECK(popup->WindowPadding.y == doctest::Approx(style.WindowPadding.y));
	ImGui::GetStyle().FontScaleDpi = 1.0f;
}

TEST_CASE("VisualScriptEditor: the live widget's colour and asset frames show no tooltip, which would take the canvas's zoom")
{
	VisualScriptDocument document = MakeChipDocument();
	document.Graph.Nodes.push_back(MakeNode(
		40, "Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)", glm::vec2(0.0f, 1600.0f)));
	Testing::PanelFixture fixture(document);
	fixture.CheckDrawingChangesNothing();
	struct Target
	{
		uint32_t Node;
		std::string_view Pin;
	};
	for (Target const& target : {Target{8, "A"}, Target{40, "Prefab"}})
	{
		CAPTURE(target.Pin);
		glm::vec2 const center = ChipCenter(fixture.Model(), target.Node, target.Pin);
		VisualScriptOpenRequest request;
		request.Graph = fixture.Graph;
		request.Center = center;
		request.Zoom = 2.0f;
		request.Focus = false;
		fixture.Panel->Apply(request);
		fixture.Frames(2);
		fixture.MoveMouse(fixture.Model().GetView().ToScreen(center));
		// Well past ImGui's tooltip delays, the mouse held still.
		fixture.Frames(90);
		REQUIRE(fixture.Panel->GetLiveWidget().has_value());
		CHECK(fixture.Panel->GetLiveWidget()->Chip.Node == target.Node);
		CHECK(std::none_of(GImGui->Windows.begin(), GImGui->Windows.end(),
		                   [](ImGuiWindow const* window)
		                   {
							   return window->Active && (window->Flags & ImGuiWindowFlags_Tooltip) != 0;
						   }));
	}
}

TEST_CASE("VisualScriptEditor: the find bar's matches are outlined in amber, and the palette's preview draws a node alone at its "
          "place and scale")
{
	Testing::CanvasRenderFixture fixture(MakeChipDocument());
	fixture.Frame();
	CHECK_FALSE(fixture.Item(VisualScriptDrawItemKind::Node, 2).Color == VisualScriptWarningColor);
	std::vector<uint32_t> const outlined = {2};
	fixture.Draw.Outlined = outlined;
	fixture.Frame();
	VisualScriptDrawRecordItem const& match = fixture.Item(VisualScriptDrawItemKind::Node, 2);
	CHECK(match.Has(Elements::Outline));
	CHECK(match.Color == VisualScriptWarningColor);
	CHECK_FALSE(fixture.Item(VisualScriptDrawItemKind::Node, 3).Color == VisualScriptWarningColor);
	fixture.Draw.Outlined = {};

	// A node alone: the canvas's Delay, drawn with its top-left at a point, 0.75 screen pixels per canvas unit.
	VisualScriptCanvasModel const& canvas = fixture.Model.GetCanvas();
	std::optional<uint32_t> const index = canvas.FindNode(2);
	REQUIRE(index.has_value());
	glm::vec2 const size = canvas.GetNodes()[*index].Size;
	glm::vec2 const position = canvas.GetNodes()[*index].Position;
	VisualScriptCanvasRenderer preview;
	VisualScriptDrawRecord record;
	size_t const canvasItems = fixture.Record.Items.size();
	fixture.Extra = [&]
	{
		VisualScriptViewState state;
		state.Center = position + size * 0.5f;
		state.Zoom = 0.75f;
		VisualScriptView view(state);
		view.SetCanvas(glm::vec2(100.0f, 50.0f), size * 0.75f, 1.0f);
		VisualScriptCanvasDrawInput input = fixture.Draw;
		input.Record = &record;
		preview.DrawNodePreview(canvas, *index, view, input);
	};
	fixture.Frame();
	fixture.Extra = nullptr;
	REQUIRE(record.Items.size() == 1);
	VisualScriptDrawRecordItem const& item = record.Items.front();
	CHECK(item.Kind == VisualScriptDrawItemKind::Node);
	CHECK(item.Element == 2);
	CHECK(item.Level == VisualScriptDetailLevel::Detail);
	CHECK(item.Has(Elements::Body | Elements::Title | Elements::Pins));
	CHECK(item.Screen.Min.x == doctest::Approx(100.0f));
	CHECK(item.Screen.Min.y == doctest::Approx(50.0f));
	CHECK(item.Screen.Max.x == doctest::Approx(100.0f + size.x * 0.75f));
	CHECK(preview.GetStats().VisibleNodes == 1);
	// The canvas's own record and counts are untouched by it.
	CHECK(fixture.Record.Items.size() == canvasItems);
}
