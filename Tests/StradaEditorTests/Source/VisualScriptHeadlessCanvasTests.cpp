#include "HeadlessImGui.h"
#include "VisualScriptPanelTestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptArrange.h"
#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptCanvasPins.h"
#include "Editor/VisualScripting/VisualScriptHeadlessCanvas.h"
#include "Editor/VisualScripting/VisualScriptPalette.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptTextEstimate.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Hash.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;
using namespace Strada::Testing;

namespace
{
	constexpr std::string_view Raycast = "Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)";
	constexpr std::string_view AddForce = "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)";

	VisualScriptNode MakeNode(uint32_t id, std::string_view type, std::optional<glm::vec2> position, Json settings = Json::object(),
	                          Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::string(type);
		node.Position = position;
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	// An update that raycasts and pushes, a variable read into an addition with a literal, a sequence, an unplaced node, a split
	// pin, and a function whose graph has its own nodes.
	VisualScriptDocument MakePinsDocument()
	{
		VisualScriptDocument document;
		VisualScriptVariable speed;
		speed.ID = 100;
		speed.Name = "Speed";
		speed.Type = "float";
		speed.Value = 2.5;
		document.Variables.push_back(std::move(speed));
		document.Graph.Nodes.push_back(MakeNode(1, "Event.Update", glm::vec2(0.0f, 0.0f)));
		document.Graph.Nodes.push_back(
			MakeNode(2, Raycast, glm::vec2(320.0f, 0.0f), Json::object(), Json::object({{"MaxDistance", 50.0}})));
		document.Graph.Nodes.push_back(MakeNode(3, AddForce, std::nullopt));
		document.Graph.Nodes.back().Split.Inputs.push_back("Force");
		document.Graph.Nodes.push_back(MakeNode(4, "Variable.Get", glm::vec2(0.0f, 320.0f), Json::object({{"Variable", 100}})));
		document.Graph.Nodes.push_back(MakeNode(5, "Operator.Add", glm::vec2(320.0f, 320.0f), Json::object(), Json::object({{"B", 1.5}})));
		document.Graph.Nodes.push_back(MakeNode(6, "Flow.Sequence", glm::vec2(640.0f, 320.0f), Json::object({{"Count", 2}})));
		document.Graph.Links.push_back(MakeLink(1, "Then", 2, "Execute"));
		document.Graph.Links.push_back(MakeLink(2, "Then", 3, "Execute"));
		document.Graph.Links.push_back(MakeLink(4, "Value", 5, "A"));
		document.Graph.Links.push_back(MakeLink(1, "DeltaTime", 3, "Force.X"));
		VisualScriptFunction function;
		function.ID = 200;
		function.Name = "Push";
		function.Graph.Nodes.push_back(MakeNode(201, "Function.Entry", glm::vec2(0.0f, 0.0f)));
		function.Graph.Nodes.push_back(MakeNode(202, AddForce, glm::vec2(320.0f, 0.0f)));
		function.Graph.Links.push_back(MakeLink(201, "Then", 202, "Execute"));
		document.Functions.push_back(std::move(function));
		return document;
	}

	// The IDs of a sub-graph's nodes.
	std::vector<uint32_t> GetNodeIDs(VisualScriptGraph const& graph)
	{
		std::vector<uint32_t> ids;
		for (VisualScriptNode const& node : graph.Nodes)
		{
			ids.push_back(node.ID);
		}
		return ids;
	}

	VisualScriptRect GetRect(VisualScriptDocument const& document, uint32_t id, VisualScriptNodeExtents const& extents)
	{
		auto const node = std::find_if(document.Graph.Nodes.begin(), document.Graph.Nodes.end(),
		                               [id](VisualScriptNode const& candidate)
		                               {
										   return candidate.ID == id;
									   });
		REQUIRE(node != document.Graph.Nodes.end());
		REQUIRE(node->Position.has_value());
		return {*node->Position, *node->Position + extents(id)};
	}

	bool Overlap(VisualScriptRect const& a, VisualScriptRect const& b)
	{
		return a.Min.x < b.Max.x && b.Min.x < a.Max.x && a.Min.y < b.Max.y && b.Min.y < a.Max.y;
	}

	// The window's measurer, as VisualScriptEditorPanel::MakeMeasurer makes it (VisualScriptEditorPanel.cpp, MakeMeasurer): the
	// current font measured at the current font size and scaled linearly. Call it inside a frame.
	VisualScriptTextMeasurer MakeWindowMeasurer()
	{
		ImFont* const font = ImGui::GetFont();
		float const base = ImGui::GetFontSize();
		VisualScriptTextMeasurer measure;
		measure.Measure = [font, base](std::string_view text, float sizeCu)
		{
			if (text.empty() || base <= 0.0f)
			{
				return 0.0f;
			}
			return font->CalcTextSizeA(base, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x * sizeCu / base;
		};
		measure.Signature = Hash::Combine(Hash::FNV1a("WindowMeasurer"), std::bit_cast<uint32_t>(base));
		return measure;
	}
}

TEST_CASE("VisualScriptEditor: a headless canvas gives the operations the pins and sizes a window's panes give them")
{
	VisualScriptDocument const document = MakePinsDocument();
	CanvasRenderFixture fixture(document);
	// The window measures as the headless canvas does, so even the outputs' anchors (at the node's right edge) agree.
	fixture.Measure = MakeEstimatingVisualScriptTextMeasurer();
	fixture.Frame();
	fixture.Frame();
	VisualScriptLibrary const& library = fixture.Semantics.GetFixtureLibrary();
	REQUIRE(fixture.Model.GetAnalysis() != nullptr);
	VisualScriptNodePins const window = MakeVisualScriptNodePins(fixture.Model, &library);
	VisualScriptNodeExtents const windowExtents = [&fixture](uint32_t node)
	{
		return fixture.Model.GetCanvas().GetExtents(node);
	};

	// Synced alike: the window's analysis of its display document; and an analysis of the stored document, which has the
	// same program.
	Result<VisualScriptAnalysis> const own = AnalyzeWithFixtureTemplates(library, document);
	REQUIRE(own.IsOk());
	for (bool const windowAnalysis : {true, false})
	{
		CAPTURE(windowAnalysis);
		VisualScriptHeadlessCanvas canvas;
		canvas.Sync(document, 0, windowAnalysis ? fixture.Model.GetAnalysis() : &own.GetValue(),
		            windowAnalysis ? &fixture.Model.GetDisplayDocument() : &document, &library);
		VisualScriptNodePins const headless = canvas.GetPins();
		VisualScriptNodeExtents const extents = canvas.GetExtents();
		for (uint32_t const id : GetNodeIDs(document.Graph))
		{
			CAPTURE(id);
			std::vector<VisualScriptNodePin> const rows = headless(id);
			CHECK_FALSE(rows.empty());
			CHECK(rows == window(id));
			CHECK(extents(id) == windowExtents(id));
		}
		// The split pin's fields are rows under their stored names.
		std::vector<VisualScriptNodePin> const push = headless(3);
		CHECK(std::any_of(push.begin(), push.end(),
		                  [](VisualScriptNodePin const& pin)
		                  {
							  return pin.Name == "Force.X";
						  }));
		// The unplaced node is placed where the window shows it.
		CHECK(canvas.GetDisplay().Graph.Nodes[2].Position == fixture.Model.GetDisplayDocument().Graph.Nodes[2].Position);
		// No node, no rows; a new node's rows come from the pins given, at no particular anchor.
		CHECK(headless(999).empty());
		VisualScriptNodeTemplate const* const branch = library.FindNode("Flow.Branch");
		REQUIRE(branch != nullptr);
		std::vector<VisualScriptNodePin> const created = canvas.GetPins(VisualScriptNewNodePins{7, branch->Pins})(7);
		CHECK(created == MakeVisualScriptNodePins(fixture.Model, &library, VisualScriptNewNodePins{7, branch->Pins})(7));
		REQUIRE(created.size() == branch->Pins.size());
		CHECK(created.front().Name == branch->Pins.front().Name);
	}

	// A function's graph is laid out as its window shows it.
	VisualScriptHeadlessCanvas function;
	function.Sync(document, 200, &own.GetValue(), &document, &library);
	CHECK(function.GetCanvas().GetFunction() == 200);
	CHECK(function.GetCanvas().GetNodes().size() == 2);
	CHECK(function.GetExtents()(202).y > 0.0f);
	CHECK(function.GetExtents()(2) == glm::vec2(0.0f));
}

TEST_CASE("VisualScriptEditor: without a library a headless canvas measures the pins the document's links and literals name")
{
	VisualScriptDocument const document = MakePinsDocument();
	VisualScriptHeadlessCanvas canvas;
	canvas.Sync(document, 0, nullptr, nullptr, nullptr);
	VisualScriptNodeExtents const extents = canvas.GetExtents();
	VisualScriptNodePins const pins = canvas.GetPins();
	namespace Metrics = VisualScriptCanvasMetrics;
	for (uint32_t const id : GetNodeIDs(document.Graph))
	{
		CAPTURE(id);
		glm::vec2 const size = extents(id);
		CHECK(size.x > 0.0f);
		CHECK(size.x <= Metrics::MaxNodeWidth);
		CHECK(size.y > 0.0f);
	}
	// The raycast has the pins its links and literal name: Execute and MaxDistance in, Then out.
	std::vector<VisualScriptNodePin> const raycast = pins(2);
	std::set<std::string> names;
	for (VisualScriptNodePin const& pin : raycast)
	{
		names.insert(pin.Name);
	}
	CHECK(names == std::set<std::string>({"Execute", "MaxDistance", "Then"}));
	// Two rows (Execute and MaxDistance on the left): a header, the rows' top, two rows and the rows' bottom.
	std::optional<uint32_t> const index = canvas.GetCanvas().FindNode(2);
	REQUIRE(index.has_value());
	float const header =
		canvas.GetCanvas().GetNodes()[*index].Subtitle.Length > 0 ? Metrics::HeaderHeightWithSubtitle : Metrics::HeaderHeight;
	CHECK(extents(2).y == doctest::Approx(header + Metrics::PinRowsTop + 2.0f * Metrics::PinRowHeight + Metrics::PinRowsBottom));

	// Arranging with these sizes works without the runtime, as graph.arrange's Measured extents do in the automation fuzz.
	VisualScriptDocument arranged = document;
	CHECK(VisualScriptEditing::ArrangeNodes(arranged, 0, {}, extents).IsOk());
}

TEST_CASE("VisualScriptEditor: Measured extents arrange tall nodes apart where Uniform extents overlap them")
{
	// A sequence starting two raycasts: they share a column, the second one row below the first. A raycast is 178 cu tall
	// (a subtitle and five rows), more than the 128 of the uniform extents, whose rows stand 160 apart.
	VisualScriptDocument document;
	document.Graph.Nodes.push_back(MakeNode(1, "Event.Update", glm::vec2(0.0f, 0.0f)));
	document.Graph.Nodes.push_back(MakeNode(2, "Flow.Sequence", glm::vec2(0.0f, 0.0f), Json::object({{"Count", 2}})));
	document.Graph.Nodes.push_back(MakeNode(3, Raycast, glm::vec2(0.0f, 0.0f)));
	document.Graph.Nodes.push_back(MakeNode(4, Raycast, glm::vec2(0.0f, 0.0f)));
	document.Graph.Links.push_back(MakeLink(1, "Then", 2, "Execute"));
	document.Graph.Links.push_back(MakeLink(2, "Then0", 3, "Execute"));
	document.Graph.Links.push_back(MakeLink(2, "Then1", 4, "Execute"));

	FixtureVisualScriptSemantics semantics;
	VisualScriptLibrary const& library = semantics.GetFixtureLibrary();
	Result<VisualScriptAnalysis> const analysis = AnalyzeWithFixtureTemplates(library, document);
	REQUIRE(analysis.IsOk());
	VisualScriptHeadlessCanvas canvas;
	canvas.Sync(document, 0, &analysis.GetValue(), &document, &library);
	VisualScriptNodeExtents const measured = canvas.GetExtents();
	CHECK(measured(3).y == doctest::Approx(178.0f));

	VisualScriptNodeExtents const uniform = [](uint32_t node)
	{
		return VisualScriptEditing::GetDefaultVisualScriptNodeExtents(node);
	};
	VisualScriptDocument byUniform = document;
	REQUIRE(VisualScriptEditing::ArrangeNodes(byUniform, 0, {}, uniform).IsOk());
	// The counter-example: as drawn, the uniformly arranged raycasts overlap by 18 cu.
	VisualScriptRect const first = GetRect(byUniform, 3, measured);
	VisualScriptRect const second = GetRect(byUniform, 4, measured);
	CHECK(Overlap(first, second));
	CHECK(std::min(first.Max.y, second.Max.y) - std::max(first.Min.y, second.Min.y) == doctest::Approx(18.0f));

	VisualScriptDocument byMeasure = document;
	REQUIRE(VisualScriptEditing::ArrangeNodes(byMeasure, 0, {}, measured).IsOk());
	std::vector<uint32_t> const ids = {1, 2, 3, 4};
	for (uint32_t const a : ids)
	{
		for (uint32_t const b : ids)
		{
			if (a < b)
			{
				CAPTURE(a);
				CAPTURE(b);
				CHECK_FALSE(Overlap(GetRect(byMeasure, a, measured), GetRect(byMeasure, b, measured)));
			}
		}
	}
	VisualScriptRect const top = GetRect(byMeasure, 3, measured);
	VisualScriptRect const bottom = GetRect(byMeasure, 4, measured);
	CHECK(std::max(top.Min.y, bottom.Min.y) - std::min(top.Max.y, bottom.Max.y) >= 32.0f);
}

TEST_CASE("VisualScriptEditor: the text estimate counts each code point once, ASCII by the table and the rest as the font size")
{
	CHECK(EstimateVisualScriptTextWidth("", 15.0f) == 0.0f);
	float const a = EstimateVisualScriptTextWidth("A", 1.0f);
	CHECK(a == doctest::Approx(static_cast<float>(VisualScriptTextAdvances['A' - 32]) / 64.0f * VisualScriptTextEstimateMargin));
	// Linear in the size and in the text.
	CHECK(EstimateVisualScriptTextWidth("A", 20.0f) == doctest::Approx(20.0f * a));
	CHECK(EstimateVisualScriptTextWidth("AAA", 1.0f) == doctest::Approx(3.0f * a));
	// U+2026 (three bytes), U+00E9 (two) and U+1F600 (four) count the size each; so do a control character, a lone
	// continuation byte, a byte no sequence starts with and a sequence cut short.
	CHECK(EstimateVisualScriptTextWidth("\xE2\x80\xA6", 10.0f) == doctest::Approx(10.0f));
	CHECK(EstimateVisualScriptTextWidth("\xC3\xA9\xF0\x9F\x98\x80", 10.0f) == doctest::Approx(20.0f));
	CHECK(EstimateVisualScriptTextWidth("\t", 10.0f) == doctest::Approx(10.0f));
	CHECK(EstimateVisualScriptTextWidth("\x80", 10.0f) == doctest::Approx(10.0f));
	CHECK(EstimateVisualScriptTextWidth("\xFF", 10.0f) == doctest::Approx(10.0f));
	CHECK(EstimateVisualScriptTextWidth("\xE2\x80", 10.0f) == doctest::Approx(20.0f));
	CHECK(EstimateVisualScriptTextWidth("\xE2\x80"
	                                    "A",
	                                    1.0f) == doctest::Approx(2.0f + a));

	VisualScriptTextMeasurer const measurer = MakeEstimatingVisualScriptTextMeasurer();
	CHECK(measurer.Signature == Hash::FNV1a("VisualScriptEstimatedText v1"));
	CHECK(measurer("Branch", 14.0f) == EstimateVisualScriptTextWidth("Branch", 14.0f));
}

TEST_CASE("VisualScriptEditor: the text estimate's advances are the editor font's")
{
	HeadlessImGui gui;
	gui.UseEditorStyle();
	std::array<uint16_t, 95> generated{};
	gui.Frame(
		[&generated]
		{
			// At size 64 the font's advances are in 1/64 of the size; the table rounds them up.
			ImFontBaked* const baked = ImGui::GetIO().FontDefault->GetFontBaked(64.0f);
			REQUIRE(baked != nullptr);
			for (uint32_t character = 32; character <= 126; character++)
			{
				generated[character - 32] = static_cast<uint16_t>(std::ceil(baked->GetCharAdvance(static_cast<ImWchar>(character))));
			}
		});
	std::string table;
	for (size_t index = 0; index < generated.size(); index++)
	{
		table += fmt::format("{}{}", index == 0 ? "" : ", ", generated[index]);
	}
	CHECK_MESSAGE(generated == VisualScriptTextAdvances, "VisualScriptTextAdvances (VisualScriptTextEstimate.cpp) should be: {", table,
	              "}");
}

TEST_CASE("VisualScriptEditor: headless nodes are as tall as a window's and at least as wide, at most 15 percent wider, at every UI scale")
{
	FixtureVisualScriptSemantics semantics;
	VisualScriptLibrary const& library = semantics.GetFixtureLibrary();

	// Every kind of the fixture library and every typed expansion the palette offers, with the document's own entries (a
	// variable of each kind of type, a function, a custom event with a long name), in one graph.
	VisualScriptDocument document;
	std::array<std::pair<char const*, char const*>, 4> const variables = {
		{{"Speed", "float"}, {"Direction", "Strada.Vector3"}, {"Target", "Strada.Entity"}, {"WhenTheDoorOpensTheLightsTurnOn", "bool"}}};
	uint32_t id = 1;
	for (auto const& [name, type] : variables)
	{
		VisualScriptVariable variable;
		variable.ID = id++;
		variable.Name = name;
		variable.Type = type;
		document.Variables.push_back(std::move(variable));
	}
	VisualScriptFunction function;
	function.ID = id++;
	function.Name = "ComputeTheDamageOfAFallingObject";
	function.Inputs.emplace_back("HeightFallenFrom", "float");
	document.Functions.push_back(std::move(function));
	VisualScriptPaletteIndex index;
	index.Rebuild(library);
	index.SetDocument(document, 0);
	auto const add = [&document, &id](std::string type, Json settings)
	{
		float const column = static_cast<float>((id % 8) * 640);
		float const row = static_cast<float>((id / 8) * 400);
		document.Graph.Nodes.push_back(MakeNode(id++, type, glm::vec2(column, row), std::move(settings)));
	};
	for (VisualScriptNodeTemplate const& kind : library.GetNodes())
	{
		add(kind.Type, Json::object());
	}
	for (VisualScriptPaletteEntry const& entry : index.GetEntries())
	{
		if (entry.Action == VisualScriptPaletteAction::AddNode && !entry.NodeType.empty())
		{
			add(entry.NodeType, entry.Settings);
		}
	}
	add("Event.Custom", Json::object({{"Name", "WhenThePlayerReachesTheTopOfTheTower"}}));
	add("Function.Call", Json::object({{"Function", document.Functions.front().ID}}));
	// Collapsed and disabled nodes are measured too.
	document.Graph.Nodes[3].Collapsed = true;
	document.Graph.Nodes[4].Enabled = VisualScriptNodeEnabled::Disabled;
	REQUIRE(document.Graph.Nodes.size() > library.GetNodes().size());
	Result<VisualScriptAnalysis> const analysis = AnalyzeWithFixtureTemplates(library, document);
	REQUIRE(analysis.IsOk());

	VisualScriptHeadlessCanvas headless;
	headless.Sync(document, 0, &analysis.GetValue(), &document, &library);
	VisualScriptNodeExtents const extents = headless.GetExtents();

	for (float const scale : {1.0f, 1.25f, 1.5f, 2.0f})
	{
		CAPTURE(scale);
		HeadlessImGui gui;
		gui.UseEditorStyle();
		ImGui::GetStyle().FontScaleDpi = scale;
		VisualScriptCanvasModel window;
		gui.Frame(
			[&]
			{
				VisualScriptCanvasSource source;
				source.Display = &document;
				source.Analysis = &analysis.GetValue();
				source.AnalyzedDocument = &document;
				source.Library = &library;
				static_cast<void>(window.Sync(source, MakeWindowMeasurer(), VisualScriptThemeAnchors{}));
			});
		float widest = 0.0f;
		for (VisualScriptNode const& node : document.Graph.Nodes)
		{
			CAPTURE(node.ID);
			CAPTURE(node.Type);
			glm::vec2 const drawn = window.GetExtents(node.ID);
			glm::vec2 const estimated = extents(node.ID);
			REQUIRE(drawn.x > 0.0f);
			CHECK(estimated.y == drawn.y);
			CHECK(estimated.x >= drawn.x);
			CHECK(estimated.x <= 1.15f * drawn.x);
			widest = std::max(widest, estimated.x / drawn.x);
		}
		MESSAGE("UI scale ", scale, ": headless nodes are at most ", widest, " times as wide as a window's");
	}
}
