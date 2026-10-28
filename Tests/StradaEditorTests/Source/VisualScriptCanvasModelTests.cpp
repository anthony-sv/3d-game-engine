#include "Fuzzing.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptHitTest.h"
#include "Editor/VisualScripting/VisualScriptSpatialIndex.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"
#include "Editor/VisualScripting/VisualScriptView.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	namespace Geometry = VisualScriptGeometry;
	using Direction = VisualScriptPinDirection;

	constexpr std::string_view LogInfo = "Call:Strada.Log.Info(string)";
	constexpr std::string_view AddForce = "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)";
	constexpr std::string_view Instantiate = "Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)";
	constexpr std::string_view Raycast = "Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)";
	constexpr std::string_view SetTranslation = "Set:Strada.TransformComponent.Translation";

	VisualScriptNode MakeNode(uint32_t id, std::string_view type, glm::vec2 position = glm::vec2(0.0f), Json settings = Json::object(),
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

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		return variable;
	}

	// A document drawn by a visual model with the fixture library (or none), its analysis made by the fixture's template
	// analyzer when asked for, and the fixed-width measurer (every byte half its size wide).
	struct CanvasFixture
	{
		Testing::FixtureVisualScriptSemantics Semantics;
		VisualScriptDocument Document;
		std::optional<VisualScriptAnalysis> Analysis;
		VisualScriptDocument AnalyzedDocument;
		VisualScriptCanvasModel Model;
		VisualScriptTextMeasurer Measure = Testing::FixedWidthMeasurer(0.5f);
		VisualScriptThemeAnchors Theme;
		uint32_t Function = 0;
		bool WithLibrary = true;
		bool ShowDiagnostics = true;
		uint64_t AssetVersion = 0;

		VisualScriptLibrary const& GetLibrary() { return Semantics.GetFixtureLibrary(); }

		// The analysis of the document as it is now (with the given problems), which the next Sync draws from.
		VisualScriptAnalysis& Analyze(std::span<VisualScriptDiagnostic const> problems = {})
		{
			Result<VisualScriptAnalysis> analysis = Testing::AnalyzeWithFixtureTemplates(GetLibrary(), Document, problems);
			REQUIRE_MESSAGE(analysis.IsOk(), analysis.GetError());
			Analysis = analysis.TakeValue();
			AnalyzedDocument = Document;
			return *Analysis;
		}

		bool Sync() { return Model.Sync(MakeSource(), Measure, Theme); }

		// What a layout-only commit of the document previous makes the canvas do (the document is now Document).
		std::optional<bool> SyncPositions(VisualScriptDocument const& previous)
		{
			return Model.SyncPositions(previous, MakeSource(), Measure, Theme);
		}

		VisualScriptCanvasSource MakeSource()
		{
			VisualScriptCanvasSource source;
			source.Display = &Document;
			source.Function = Function;
			source.Analysis = Analysis ? &*Analysis : nullptr;
			source.AnalyzedDocument = Analysis ? &AnalyzedDocument : nullptr;
			VisualScriptLibrary const* library = WithLibrary ? &GetLibrary() : nullptr;
			source.Library = library;
			if (library != nullptr)
			{
				source.EnumLookup = [library](std::string_view typeId)
				{
					return library->FindEnum(typeId);
				};
			}
			source.ShowDiagnostics = ShowDiagnostics;
			source.AssetNames = [](AssetHandle handle)
			{
				return "Asset " + handle.ToString();
			};
			source.AssetVersion = AssetVersion;
			return source;
		}

		VisualScriptNodeVisual const& Node(uint32_t id) const
		{
			std::optional<uint32_t> const index = Model.FindNode(id);
			REQUIRE_MESSAGE(index.has_value(), "no node ", id);
			return Model.GetNodes()[*index];
		}

		VisualScriptPinVisual const& Pin(uint32_t node, std::string_view name, Direction direction = Direction::In) const
		{
			std::optional<uint32_t> const pin = Model.FindPin(*Model.FindNode(node), name, direction);
			REQUIRE_MESSAGE(pin.has_value(), "node ", node, " has no pin ", std::string(name));
			return Model.GetPins()[*pin];
		}

		std::string Text(VisualScriptTextRef ref) const { return std::string(Model.GetText(ref)); }

		// The node's pins in order: "In:Execute", "Out:Then", ...
		std::vector<std::string> PinNames(uint32_t node) const
		{
			std::vector<std::string> names;
			for (VisualScriptPinVisual const& pin : Model.GetPins(Node(node)))
			{
				names.push_back((pin.Direction == Direction::In ? "In:" : "Out:") + Text(pin.Name));
			}
			return names;
		}
	};

	void CheckPoint(glm::vec2 actual, glm::vec2 expected)
	{
		CHECK(actual.x == doctest::Approx(expected.x));
		CHECK(actual.y == doctest::Approx(expected.y));
	}

	void CheckRect(VisualScriptRect const& actual, VisualScriptRect const& expected)
	{
		CheckPoint(actual.Min, expected.Min);
		CheckPoint(actual.Max, expected.Max);
	}
}

TEST_CASE("VisualScriptEditor: the visual model sizes nodes from their measured title and pin columns, within 120 to 480 units")
{
	CanvasFixture canvas;
	std::string const longName(64, 'A');
	std::string const longParameter(60, 'p');
	Json const parameters = Json::array({Json::object({{"Name", longParameter}, {"Type", "float"}})});
	canvas.Document.Graph.Nodes = {
		MakeNode(1, "Event.Create"), MakeNode(2, "Flow.Branch", glm::vec2(300.0f, 0.0f)),
		MakeNode(3, "Event.Custom", glm::vec2(0.0f, 400.0f), Json::object({{"Name", longName}})),
		MakeNode(4, "Event.Send", glm::vec2(0.0f, 600.0f), Json::object({{"Name", "Hit"}, {"Parameters", parameters}}))};
	canvas.Document.Graph.Links = {MakeLink(1, "Then", 2, "Execute")};
	canvas.Analyze();
	CHECK(canvas.Sync());

	// Create: "Create" is 45 wide (7.5 a byte) after the event's glyph slot; at least 120; one row.
	VisualScriptNodeVisual const& create = canvas.Node(1);
	CheckPoint(create.Size, glm::vec2(120.0f, 66.0f));
	CheckPoint(canvas.Pin(1, "Then", Direction::Out).Anchor, glm::vec2(110.0f, 46.0f));
	CHECK(create.Kind == VisualScriptNodeKind::Event);
	CHECK(create.HasFlag(VisualScriptNodeFlags::Analyzed));

	// Branch: inputs Execute (no label) and Condition (58.5 wide, then a check box), outputs True and False: 104.5 + 24 + 53.5.
	VisualScriptNodeVisual const& branch = canvas.Node(2);
	CheckPoint(branch.Size, glm::vec2(182.0f, 90.0f));
	CheckPoint(branch.ExpandedSize, branch.Size);
	CHECK(canvas.Text(branch.Title) == "Branch");
	CHECK(branch.Title.Width == doctest::Approx(45.0f));
	CHECK(canvas.PinNames(2) == std::vector<std::string>{"In:Execute", "In:Condition", "Out:True", "Out:False"});
	CheckPoint(canvas.Pin(2, "Execute").Anchor, glm::vec2(10.0f, 46.0f));
	CheckPoint(canvas.Pin(2, "Condition").Anchor, glm::vec2(10.0f, 70.0f));
	CheckPoint(canvas.Pin(2, "True", Direction::Out).Anchor, glm::vec2(172.0f, 46.0f));
	CheckPoint(canvas.Pin(2, "False", Direction::Out).Anchor, glm::vec2(172.0f, 70.0f));
	VisualScriptPinVisual const& condition = canvas.Pin(2, "Condition");
	CheckRect(condition.HitRect, {glm::vec2(0.0f, 58.0f), glm::vec2(79.5f, 82.0f)});
	CheckRect(condition.ChipRect, {glm::vec2(85.5f, 60.5f), glm::vec2(104.5f, 79.5f)});
	CHECK(condition.Label.Width == doctest::Approx(58.5f));
	CheckRect(canvas.Pin(2, "True", Direction::Out).HitRect, {glm::vec2(135.0f, 34.0f), glm::vec2(182.0f, 58.0f)});
	CheckRect(branch.VisualBounds, {glm::vec2(288.0f, -12.0f), glm::vec2(494.0f, 105.0f)});
	CHECK(canvas.Pin(2, "Execute").HasFlag(VisualScriptPinFlags::Linked));
	CHECK_FALSE(condition.HasFlag(VisualScriptPinFlags::Linked));

	// A title wider than the widest node is ellipsized: 56 of its 64 bytes and the ellipsis (22.5) fit beside the glyph slot.
	VisualScriptNodeVisual const& custom = canvas.Node(3);
	CHECK(custom.Title.Length == 64);
	CHECK(custom.Title.VisibleLength == 56);
	CHECK(custom.Title.IsEllipsized());
	CHECK(custom.Title.Width == doctest::Approx(442.5f));
	CHECK(custom.Size.x == doctest::Approx(478.5f));
	CHECK(canvas.Model.GetVisibleText(custom.Title) == std::string(56, 'A'));

	// A label too long for the widest node is ellipsized so its row, with its chip, fits beside the outputs.
	VisualScriptNodeVisual const& send = canvas.Node(4);
	VisualScriptPinVisual const& parameter = canvas.Pin(4, longParameter);
	CHECK(parameter.Label.VisibleLength == 50);
	CHECK(parameter.Label.Width == doctest::Approx(344.5f));
	CHECK(send.Size.x == doctest::Approx(474.5f));
	CHECK(send.Size.x <= VisualScriptCanvasMetrics::MaxNodeWidth);
}

TEST_CASE("VisualScriptEditor: an exec pin's label is hidden, and not measured, only when it is its node's one Execute or Then")
{
	CanvasFixture canvas;
	canvas.Document.Graph.Nodes = {MakeNode(1, LogInfo), MakeNode(2, "Flow.Branch", glm::vec2(0.0f, 200.0f)),
	                               MakeNode(3, "Flow.Sequence", glm::vec2(0.0f, 400.0f)),
	                               MakeNode(4, "Flow.Tween", glm::vec2(0.0f, 600.0f))};
	canvas.Analyze();
	canvas.Sync();

	auto const hidden = [&canvas](uint32_t node, std::string_view pin, Direction direction)
	{
		VisualScriptPinVisual const& visual = canvas.Pin(node, pin, direction);
		bool const flag = visual.HasFlag(VisualScriptPinFlags::LabelHidden);
		CHECK(flag == (visual.Label.Length == 0));
		return flag;
	};
	CHECK(hidden(1, "Execute", Direction::In));
	CHECK(hidden(1, "Then", Direction::Out));
	CHECK(hidden(2, "Execute", Direction::In));
	CHECK_FALSE(hidden(2, "True", Direction::Out));
	CHECK_FALSE(hidden(2, "False", Direction::Out));
	CHECK(hidden(3, "Execute", Direction::In));
	CHECK_FALSE(hidden(3, "Then0", Direction::Out));
	CHECK_FALSE(hidden(3, "Then1", Direction::Out));
	// Tween has two exec inputs (Execute and Stop) and two exec outputs: every label shows.
	CHECK_FALSE(hidden(4, "Execute", Direction::In));
	CHECK_FALSE(hidden(4, "Stop", Direction::In));
	CHECK_FALSE(hidden(4, "Update", Direction::Out));
	CHECK_FALSE(hidden(4, "Completed", Direction::Out));
	CHECK(canvas.Text(canvas.Pin(4, "Execute").Label) == "Execute");

	// Log Info's width comes from Message's row alone (15 + 6 + 45.5 + 6 + 96) and Then's glyph: 168.5 + 24 + 15.
	CHECK(canvas.Node(1).Size.x == doctest::Approx(207.5f));
}

TEST_CASE("VisualScriptEditor: a collapsed node is drawn as its header, its wires at the header's edges, its rows still measured")
{
	CanvasFixture canvas;
	VisualScriptNode branch = MakeNode(2, "Flow.Branch");
	branch.Collapsed = true;
	VisualScriptNode disabled = MakeNode(3, "Flow.Branch", glm::vec2(0.0f, 200.0f));
	disabled.Collapsed = true;
	disabled.Enabled = VisualScriptNodeEnabled::Disabled;
	canvas.Document.Graph.Nodes = {branch, disabled};
	canvas.Analyze();
	canvas.Sync();

	// The header with the title, the collapsed marker and padding (81) is at least 120 wide.
	VisualScriptNodeVisual const& node = canvas.Node(2);
	CHECK(node.HasFlag(VisualScriptNodeFlags::Collapsed));
	CheckPoint(node.Size, glm::vec2(120.0f, 28.0f));
	CheckPoint(node.ExpandedSize, glm::vec2(182.0f, 90.0f));
	CheckRect(node.VisualBounds, {glm::vec2(-12.0f), glm::vec2(132.0f, 43.0f)});
	CheckPoint(Geometry::GetPinAnchor(node, canvas.Pin(2, "Condition")), glm::vec2(0.0f, 14.0f));
	CheckPoint(Geometry::GetPinAnchor(node, canvas.Pin(2, "False", Direction::Out)), glm::vec2(120.0f, 14.0f));
	// Drawn expanded under a dragged wire, the rows are where they would be.
	VisualScriptNodeVisual const expanded = Geometry::GetExpandedNode(node);
	CheckPoint(Geometry::GetPinAnchor(expanded, canvas.Pin(2, "Condition")), glm::vec2(10.0f, 70.0f));
	CheckPoint(Geometry::GetPinAnchor(expanded, canvas.Pin(2, "False", Direction::Out)), glm::vec2(172.0f, 70.0f));
	// A disabled collapsed node keeps its strip under the header.
	CheckPoint(canvas.Node(3).Size, glm::vec2(120.0f, 48.0f));
	CheckPoint(canvas.Node(3).ExpandedSize, glm::vec2(182.0f, 110.0f));
}

TEST_CASE("VisualScriptEditor: links and literals that name pins a node lacks become orphan rows after its pins")
{
	CanvasFixture canvas;
	canvas.Document.Graph.Nodes = {
		MakeNode(1, "Flow.Switch", glm::vec2(0.0f), Json::object({{"Type", "int"}, {"Cases", Json::array({1, 2})}})),
		MakeNode(2, LogInfo, glm::vec2(400.0f, 0.0f)),
		MakeNode(3, "Flow.Branch", glm::vec2(0.0f, 300.0f), Json::object(), Json::object({{"Ghost", 5}})),
	};
	canvas.Document.Graph.Links = {MakeLink(1, "Case:3", 2, "Execute"), MakeLink(1, "Case:1", 2, "Execute")};
	canvas.Analyze();
	// A node added since the analysis takes its template's pins; the pins its links name beyond them are rows of no known
	// type, not orphans.
	canvas.Document.Graph.Nodes.push_back(MakeNode(4, "Flow.Sequence", glm::vec2(0.0f, 600.0f), Json::object({{"Count", 3}})));
	canvas.Document.Graph.Links.push_back(MakeLink(4, "Then2", 2, "Execute"));
	canvas.Sync();

	CHECK(canvas.PinNames(1) ==
	      std::vector<std::string>{"In:Execute", "In:Selection", "Out:Case:1", "Out:Case:2", "Out:Default", "Out:Case:3"});
	VisualScriptPinVisual const& removed = canvas.Pin(1, "Case:3", Direction::Out);
	CHECK(removed.HasFlag(VisualScriptPinFlags::Orphan));
	CHECK(removed.Shape == VisualScriptPinShape::Wildcard);
	CHECK(removed.Color == VisualScriptErrorColor);
	CHECK(removed.Type == VisualScriptWildcardType);
	CHECK(removed.HasFlag(VisualScriptPinFlags::Linked));
	CHECK(canvas.Text(canvas.Pin(1, "Case:1", Direction::Out).Label) == "1");
	// The orphan row is the last output row: below Default.
	CHECK(removed.Anchor.y > canvas.Pin(1, "Default", Direction::Out).Anchor.y);
	REQUIRE(canvas.Model.GetLinks().size() == 3);
	CHECK(canvas.Model.GetLinks()[0].HasFlag(VisualScriptLinkFlags::Orphan));
	CHECK_FALSE(canvas.Model.GetLinks()[1].HasFlag(VisualScriptLinkFlags::Orphan));

	CHECK(canvas.PinNames(3) == std::vector<std::string>{"In:Execute", "In:Condition", "In:Ghost", "Out:True", "Out:False"});
	VisualScriptPinVisual const& ghost = canvas.Pin(3, "Ghost");
	CHECK(ghost.HasFlag(VisualScriptPinFlags::Orphan));
	CHECK(ghost.ChipKind == VisualScriptChipKind::None);

	VisualScriptNodeVisual const& sequence = canvas.Node(4);
	CHECK(sequence.HasFlag(VisualScriptNodeFlags::Template));
	CHECK(canvas.PinNames(4) == std::vector<std::string>{"In:Execute", "Out:Then0", "Out:Then1", "Out:Then2"});
	VisualScriptPinVisual const& extra = canvas.Pin(4, "Then2", Direction::Out);
	CHECK_FALSE(extra.HasFlag(VisualScriptPinFlags::Orphan));
	CHECK(extra.Shape == VisualScriptPinShape::Wildcard);
	CHECK_FALSE(canvas.Model.GetLinks()[2].HasFlag(VisualScriptLinkFlags::Orphan));
}

TEST_CASE("VisualScriptEditor: a split pin shows a row per field, grouped under a bracket in the split pin's type colour")
{
	CanvasFixture canvas;
	canvas.Document.Variables = {MakeVariable(9, "Target", "Strada.Vector3")};
	VisualScriptNode set = MakeNode(1, SetTranslation, glm::vec2(0.0f), Json::object(), Json::object({{"Value.X", 1.5}}));
	set.Split.Inputs = {"Value"};
	VisualScriptNode raycast = MakeNode(2, Raycast, glm::vec2(400.0f, 0.0f));
	raycast.Split.Outputs = {"Hit", "Hit.Point"};
	VisualScriptNode variable = MakeNode(3, "Variable.Set", glm::vec2(0.0f, 400.0f), Json::object({{"Variable", 9}}));
	variable.Split.Inputs = {"Value"};
	canvas.Document.Graph.Nodes = {set, raycast, variable};
	canvas.Analyze();
	canvas.Sync();
	VisualScriptLibrary const& library = canvas.GetLibrary();

	CHECK(canvas.PinNames(1) == std::vector<std::string>{"In:Execute", "In:Target", "In:Value.X", "In:Value.Y", "In:Value.Z", "Out:Then"});
	VisualScriptPinVisual const& x = canvas.Pin(1, "Value.X");
	CHECK(x.HasFlag(VisualScriptPinFlags::SplitField));
	CHECK(x.HasFlag(VisualScriptPinFlags::SplitGroupStart));
	CHECK(x.BracketColor == GetVisualScriptTypeColor("Strada.Vector3", &library));
	CHECK(canvas.Text(x.Label) == "Value X");
	CHECK(x.ChipKind == VisualScriptChipKind::Number);
	CHECK(canvas.Text(x.ChipText) == "1.5");
	CHECK_FALSE(x.HasFlag(VisualScriptPinFlags::ChipIsDefault));
	VisualScriptPinVisual const& y = canvas.Pin(1, "Value.Y");
	CHECK(y.HasFlag(VisualScriptPinFlags::SplitField));
	CHECK_FALSE(y.HasFlag(VisualScriptPinFlags::SplitGroupStart));
	CHECK(y.HasFlag(VisualScriptPinFlags::ChipIsDefault));
	CHECK(y.Color == GetVisualScriptTypeColor("float", &library));
	CHECK_FALSE(canvas.Pin(1, "Target").HasFlag(VisualScriptPinFlags::SplitField));

	// Nested: the fields of Hit, Point's in its place; one bracket for all of Hit's rows.
	CHECK(canvas.PinNames(2) == std::vector<std::string>{"In:Execute", "In:Origin", "In:Direction", "In:MaxDistance", "In:LayerMask",
	                                                     "Out:Then", "Out:Hit.Entity", "Out:Hit.Point.X", "Out:Hit.Point.Y",
	                                                     "Out:Hit.Point.Z", "Out:Hit.Normal", "Out:Hit.Distance", "Out:Result"});
	VisualScriptPinVisual const& entity = canvas.Pin(2, "Hit.Entity", Direction::Out);
	CHECK(entity.HasFlag(VisualScriptPinFlags::SplitGroupStart));
	CHECK(entity.BracketColor == GetVisualScriptTypeColor("Strada.RaycastHit", &library));
	for (std::string_view const field : {"Hit.Point.X", "Hit.Point.Y", "Hit.Point.Z", "Hit.Normal", "Hit.Distance"})
	{
		VisualScriptPinVisual const& pin = canvas.Pin(2, field, Direction::Out);
		CHECK(pin.HasFlag(VisualScriptPinFlags::SplitField));
		CHECK_FALSE(pin.HasFlag(VisualScriptPinFlags::SplitGroupStart));
	}
	CHECK(canvas.Text(canvas.Pin(2, "Hit.Point.X", Direction::Out).Label) == "Hit Point X");
	// What can split again shows the split marker; Result, a bool, cannot.
	CHECK(canvas.Pin(2, "Hit.Normal", Direction::Out).HasFlag(VisualScriptPinFlags::Splittable));
	CHECK_FALSE(canvas.Pin(2, "Result", Direction::Out).HasFlag(VisualScriptPinFlags::Splittable));
	CHECK(canvas.Pin(2, "Origin").HasFlag(VisualScriptPinFlags::Splittable));
	CHECK_FALSE(canvas.Pin(2, "Result", Direction::Out).HasFlag(VisualScriptPinFlags::SplitField));

	// A pin whose type its template leaves open (the variable's): the one struct with these fields.
	VisualScriptPinVisual const& variableX = canvas.Pin(3, "Value.X");
	CHECK(variableX.HasFlag(VisualScriptPinFlags::SplitGroupStart));
	CHECK(variableX.BracketColor == GetVisualScriptTypeColor("Strada.Vector3", &library));
}

TEST_CASE("VisualScriptEditor: value chips show stored literals, pin defaults and type defaults in their formats")
{
	CanvasFixture canvas;
	canvas.Document.Graph.Nodes = {
		MakeNode(1, "Flow.Delay"),
		MakeNode(2, "Flow.Delay", glm::vec2(0.0f, 200.0f), Json::object(), Json::object({{"Duration", 1.5}})),
		MakeNode(3, "Flow.Delay", glm::vec2(0.0f, 400.0f), Json::object(), Json::object({{"Duration", "abc"}})),
		MakeNode(4, AddForce, glm::vec2(0.0f, 600.0f), Json::object(), Json::object({{"Force", Json::array({1, 0, 0})}})),
		MakeNode(5, AddForce, glm::vec2(0.0f, 800.0f), Json::object(), Json::object({{"Force", Json::array({0.333333, 2, 0})}})),
		MakeNode(6, Instantiate, glm::vec2(0.0f, 1000.0f)),
		MakeNode(7, Instantiate, glm::vec2(0.0f, 1300.0f), Json::object(), Json::object({{"Prefab", "1234567"}})),
		MakeNode(8, "Call:Strada.Input.IsKeyDown(Strada.KeyCode)", glm::vec2(0.0f, 1600.0f), Json::object(),
	             Json::object({{"Key", "Space"}})),
		MakeNode(9, "Call:Strada.Color.Lerp(Strada.Color,Strada.Color,float)", glm::vec2(0.0f, 1800.0f), Json::object(),
	             Json::object({{"B", Json::array({1, 0, 0, 0.5})}})),
		MakeNode(10, LogInfo, glm::vec2(0.0f, 2000.0f), Json::object(), Json::object({{"Message", "Hello world, this is long\nsecond"}})),
		MakeNode(11, "Flow.Branch", glm::vec2(0.0f, 2200.0f), Json::object(), Json::object({{"Condition", true}})),
		MakeNode(12, Raycast, glm::vec2(0.0f, 2400.0f)),
	};
	canvas.Analyze();
	canvas.Sync();

	auto const check = [&canvas](uint32_t node, std::string_view pin, VisualScriptChipKind kind, std::string_view text)
	{
		CAPTURE(node);
		CAPTURE(pin);
		VisualScriptPinVisual const& visual = canvas.Pin(node, pin);
		CHECK(visual.ChipKind == kind);
		CHECK(canvas.Text(visual.ChipText) == text);
		return visual;
	};
	// A float: the pin's default (0.2), stored values, and a literal that is no float as compact JSON.
	VisualScriptPinVisual const duration = check(1, "Duration", VisualScriptChipKind::Number, "0.2");
	CHECK(duration.HasFlag(VisualScriptPinFlags::ChipIsDefault));
	CHECK(duration.ChipRect.GetSize().x == doctest::Approx(64.0f));
	CHECK_FALSE(check(2, "Duration", VisualScriptChipKind::Number, "1.5").HasFlag(VisualScriptPinFlags::ChipIsDefault));
	VisualScriptPinVisual const invalid = check(3, "Duration", VisualScriptChipKind::Raw, "\"abc\"");
	CHECK(invalid.HasFlag(VisualScriptPinFlags::ChipInvalid));
	CHECK(invalid.ChipRect.GetSize().x == doctest::Approx(96.0f));

	// Vectors: one cell per component, three cells of 44 and two gaps; digits shortened to fit a cell.
	VisualScriptPinVisual const force = check(4, "Force", VisualScriptChipKind::Cells, "1\n0\n0");
	CHECK(force.ChipRect.GetSize().x == doctest::Approx(136.0f));
	CHECK(force.ChipRect.GetSize().y == doctest::Approx(19.0f));
	check(5, "Force", VisualScriptChipKind::Cells, "0.33\n2\n0");
	// Enums by name (the pin's default here); the Target means the running entity.
	CHECK(check(4, "Mode", VisualScriptChipKind::Choice, "Force").HasFlag(VisualScriptPinFlags::ChipIsDefault));
	VisualScriptPinVisual const target = check(4, "Target", VisualScriptChipKind::Placeholder, "Self");
	CHECK(target.HasFlag(VisualScriptPinFlags::DefaultsToSelf));
	check(8, "Key", VisualScriptChipKind::Choice, "Space");

	// Type defaults: no asset, a zero vector, the identity rotation in Euler degrees, no parent entity; and an asset by name.
	check(6, "Prefab", VisualScriptChipKind::Asset, "None");
	check(6, "Position", VisualScriptChipKind::Cells, "0\n0\n0");
	check(6, "Rotation", VisualScriptChipKind::Cells, "0\n0\n0");
	check(6, "Parent", VisualScriptChipKind::Placeholder, "None");
	check(7, "Prefab", VisualScriptChipKind::Asset, "Asset 1234567");

	// Colours: a swatch of the sRGB colour (the default is white), no text.
	VisualScriptPinVisual const a = check(9, "A", VisualScriptChipKind::Color, "");
	CHECK(a.ChipColor == VisualScriptRgba(255, 255, 255, 255));
	CHECK(check(9, "B", VisualScriptChipKind::Color, "").ChipColor == VisualScriptRgba(255, 0, 0, 128));
	CHECK(a.ChipRect.GetSize().x == doctest::Approx(19.0f));

	// Strings: the first line, ellipsized to the chip (86 units of room: 10 bytes and the ellipsis).
	VisualScriptPinVisual const message = check(10, "Message", VisualScriptChipKind::Text, "Hello world, this is long");
	CHECK(message.ChipText.VisibleLength == 10);
	CHECK(message.ChipText.IsEllipsized());
	check(11, "Condition", VisualScriptChipKind::Check, "true");
	// A uint too long for its 64-unit chip is ellipsized.
	VisualScriptPinVisual const mask = check(12, "LayerMask", VisualScriptChipKind::Number, "4294967295");
	CHECK(mask.ChipText.VisibleLength == 5);
	CHECK(mask.ChipRect.GetSize().x == doctest::Approx(64.0f));

	// Linked inputs and exec inputs have no chip.
	CHECK(canvas.Pin(1, "Execute").ChipKind == VisualScriptChipKind::None);
	CHECK(Geometry::IsEmpty(canvas.Pin(1, "Execute").ChipRect));

	// When assets' names may have changed, the nodes with asset chips (the two Instantiates) are laid out again.
	canvas.Sync();
	CHECK(canvas.Model.GetMeasuredNodeCount() == 0);
	canvas.AssetVersion++;
	canvas.Sync();
	CHECK(canvas.Model.GetMeasuredNodeCount() == 2);
	CHECK(canvas.Model.ShowsAssetNames());
}

TEST_CASE("VisualScriptEditor: literals format as value chips show them")
{
	CanvasFixture canvas;
	VisualScriptLibrary const& library = canvas.GetLibrary();
	VisualScriptEnumLookup const lookup = [&library](std::string_view typeId)
	{
		return library.FindEnum(typeId);
	};
	auto const format = [&](Json const& literal, std::string_view type)
	{
		return FormatVisualScriptLiteral(literal, type, library.FindType(type), lookup, {});
	};
	CHECK(format(42, "int").Text == "42");
	CHECK(format("9007199254740993", "long").Text == "9007199254740993");
	CHECK(format(0.1, "double").Text == "0.1");
	CHECK(format(1e20, "float").Text == "1e+20");
	CHECK(format(Json::array({1.5, 2, 3, 4}), "Strada.Vector4").Text == "1.5\n2\n3\n4");
	VisualScriptChipValue const wrongSize = format(Json::array({1.5, 2}), "Strada.Vector4");
	CHECK(wrongSize.Invalid);
	CHECK(wrongSize.Text == "[1.5,2]");
	// A quarter turn about Y.
	CHECK(format(Json::array({0, 0.70710678, 0, 0.70710678}), "Strada.Quaternion").Text == "0\n90\n0");
	CHECK(format(nullptr, "Strada.ForceMode").Text == "Force");
	CHECK(format(2, "Strada.ForceMode").Text == "Acceleration");
	CHECK(format("Nothing", "Strada.ForceMode").Invalid);
	CHECK(format(nullptr, "Strada.Matrix4").Text == "Matrix");
	CHECK(format(nullptr, "Strada.TransformComponent").Text == "None");
	CHECK(format(5, "Strada.TransformComponent").Invalid);
	CHECK(format(nullptr, "exec").Kind == VisualScriptChipKind::None);
	CHECK(format(nullptr, "*").Kind == VisualScriptChipKind::None);
	CHECK(format(3, "*").Text == "3");
	CHECK_FALSE(format(3, "*").Invalid);
	CHECK(GetVisualScriptOperatorSymbol("Operator.LessOrEqual") == "\xE2\x89\xA4");
	CHECK(GetVisualScriptOperatorSymbol("Operator.Not").empty());
}

TEST_CASE("VisualScriptEditor: a disabled node's bypass exit is the analysis' pass-through, and there is none without an analysis")
{
	CanvasFixture canvas;
	VisualScriptNode delay = MakeNode(1, "Flow.Delay");
	delay.Enabled = VisualScriptNodeEnabled::Disabled;
	VisualScriptNode enabled = MakeNode(2, "Flow.Delay", glm::vec2(0.0f, 200.0f));
	canvas.Document.Graph.Nodes = {delay, enabled};
	canvas.Analyze();
	for (VisualScriptAnalyzedNode& node : canvas.Analysis->Nodes)
	{
		node.PassThrough = "Completed";
	}
	canvas.Sync();
	VisualScriptNodeVisual const& disabled = canvas.Node(1);
	CHECK(disabled.HasFlag(VisualScriptNodeFlags::Disabled));
	REQUIRE(disabled.HasFlag(VisualScriptNodeFlags::PassThroughExit));
	CHECK(disabled.PassThroughPin == 2);
	CHECK(canvas.Text(canvas.Model.GetPins(disabled)[disabled.PassThroughPin].Name) == "Completed");
	// The strip under the body is part of the node: 28 + 6 + 2 × 24 + 8 + 20.
	CHECK(disabled.Size.y == doctest::Approx(110.0f));
	// An enabled node has no bypass, whatever an outdated analysis says.
	CHECK_FALSE(canvas.Node(2).HasFlag(VisualScriptNodeFlags::PassThroughExit));

	// Without an analysis (the node from its template) there is no bypass line.
	canvas.Analysis.reset();
	canvas.Sync();
	CHECK(canvas.Node(1).HasFlag(VisualScriptNodeFlags::Template));
	CHECK_FALSE(canvas.Node(1).HasFlag(VisualScriptNodeFlags::PassThroughExit));
}

TEST_CASE("VisualScriptEditor: a sync lays out only the nodes that changed, and an unchanged document reserves no more memory")
{
	CanvasFixture canvas;
	canvas.Document = Testing::MakeGridGraph(5, 5, 2, 2);
	CHECK(canvas.Sync());
	CHECK(canvas.Model.GetMeasuredNodeCount() == 25);
	CHECK_FALSE(canvas.Model.ShowsAssetNames());
	size_t const capacity = canvas.Model.GetCapacityBytes();

	// Nothing changed: nothing is laid out, nothing for the index to do, no memory reserved.
	for (int frame = 0; frame < 3; frame++)
	{
		CHECK_FALSE(canvas.Sync());
		CHECK(canvas.Model.GetMeasuredNodeCount() == 0);
		CHECK(canvas.Model.GetCapacityBytes() == capacity);
	}

	// A move keeps the layout and moves the bounds.
	VisualScriptNodeVisual const before = canvas.Node(7);
	canvas.Document.Graph.Nodes[6].Position = *canvas.Document.Graph.Nodes[6].Position + glm::vec2(48.0f, 16.0f);
	CHECK(canvas.Sync());
	CHECK(canvas.Model.GetMeasuredNodeCount() == 0);
	VisualScriptNodeVisual const& moved = canvas.Node(7);
	CheckPoint(moved.Position, before.Position + glm::vec2(48.0f, 16.0f));
	CheckPoint(moved.VisualBounds.Min, before.VisualBounds.Min + glm::vec2(48.0f, 16.0f));
	CheckPoint(moved.Size, before.Size);
	CHECK(canvas.Text(moved.Title) == canvas.Text(before.Title));

	// A literal changes one node; a link the two it joins.
	canvas.Document.Graph.Nodes[7].Inputs["Duration"] = 2.5;
	canvas.Sync();
	CHECK(canvas.Model.GetMeasuredNodeCount() == 1);
	canvas.Document.Graph.Links.push_back(MakeLink(1, "True", 25, "Execute"));
	canvas.Sync();
	CHECK(canvas.Model.GetMeasuredNodeCount() == 2);
	// A comment is laid out again; the nodes are not.
	canvas.Document.Graph.Comments[0].Text = "Renamed";
	canvas.Sync();
	CHECK(canvas.Model.GetMeasuredNodeCount() == 0);
	CHECK(canvas.Text(canvas.Model.GetComments()[canvas.Model.FindComment(canvas.Document.Graph.Comments[0].ID).value()].Title) ==
	      "Renamed");

	// The library, the measurer and the theme lay out everything again.
	canvas.Semantics.ReloadLibrary();
	canvas.Sync();
	CHECK(canvas.Model.GetMeasuredNodeCount() == 25);
	canvas.Measure = Testing::FixedWidthMeasurer(0.6f);
	canvas.Sync();
	CHECK(canvas.Model.GetMeasuredNodeCount() == 25);
	canvas.Theme.Text = VisualScriptRgba(250, 250, 250);
	canvas.Sync();
	CHECK(canvas.Model.GetMeasuredNodeCount() == 25);
	// Assets' names: only nodes whose chips show one (none here).
	canvas.AssetVersion++;
	canvas.Sync();
	CHECK(canvas.Model.GetMeasuredNodeCount() == 0);
}

namespace
{
	// Two visual models that should be one: every node's rectangles, texts and pin anchors, every wire's ends and control
	// points, every comment box, and which nodes each box frames. Exact: both made the same float operations.
	void CheckSameCanvas(VisualScriptCanvasModel const& moved, VisualScriptCanvasModel const& synced)
	{
		REQUIRE(moved.GetNodes().size() == synced.GetNodes().size());
		for (size_t index = 0; index < moved.GetNodes().size(); index++)
		{
			VisualScriptNodeVisual const& a = moved.GetNodes()[index];
			VisualScriptNodeVisual const& b = synced.GetNodes()[index];
			CAPTURE(a.ID);
			CHECK(a.ID == b.ID);
			CHECK(a.Position == b.Position);
			CHECK(a.Size == b.Size);
			CHECK(a.ExpandedSize == b.ExpandedSize);
			CHECK(a.VisualBounds == b.VisualBounds);
			CHECK(a.BubbleRect == b.BubbleRect);
			CHECK(a.Flags == b.Flags);
			CHECK(moved.GetText(a.Title) == synced.GetText(b.Title));
			CHECK(moved.GetText(a.Comment) == synced.GetText(b.Comment));
			std::span<VisualScriptPinVisual const> const pinsA = moved.GetPins(a);
			std::span<VisualScriptPinVisual const> const pinsB = synced.GetPins(b);
			REQUIRE(pinsA.size() == pinsB.size());
			for (size_t pin = 0; pin < pinsA.size(); pin++)
			{
				CHECK(moved.GetText(pinsA[pin].Name) == synced.GetText(pinsB[pin].Name));
				CHECK(Geometry::GetPinAnchor(a, pinsA[pin]) == Geometry::GetPinAnchor(b, pinsB[pin]));
				CHECK(Geometry::GetPinHitRect(a, pinsA[pin], 0.0f) == Geometry::GetPinHitRect(b, pinsB[pin], 0.0f));
			}
		}
		REQUIRE(moved.GetLinks().size() == synced.GetLinks().size());
		for (uint32_t link = 0; link < moved.GetLinks().size(); link++)
		{
			CHECK(moved.GetDocumentLink(link) == synced.GetDocumentLink(link));
			CHECK(Geometry::GetLinkControlPoints(moved, link) == Geometry::GetLinkControlPoints(synced, link));
			CHECK(Geometry::GetLinkBounds(moved, link) == Geometry::GetLinkBounds(synced, link));
		}
		REQUIRE(moved.GetComments().size() == synced.GetComments().size());
		for (size_t index = 0; index < moved.GetComments().size(); index++)
		{
			VisualScriptCommentVisual const& a = moved.GetComments()[index];
			VisualScriptCommentVisual const& b = synced.GetComments()[index];
			CAPTURE(a.ID);
			CHECK(a.ID == b.ID);
			CHECK(a.Rect == b.Rect);
			CHECK(moved.GetText(a.Title) == synced.GetText(b.Title));
			CHECK(moved.GetText(a.Body) == synced.GetText(b.Body));
			CHECK(a.BodyColor == b.BodyColor);
			// The nodes the box frames whole, as moving a comment carries them.
			auto const framed = [](VisualScriptCanvasModel const& model, VisualScriptRect const& box)
			{
				std::vector<uint32_t> nodes;
				for (VisualScriptNodeVisual const& node : model.GetNodes())
				{
					VisualScriptRect const rect = Geometry::GetNodeRect(node);
					if (Geometry::Contains(box, rect.Min) && Geometry::Contains(box, rect.Max))
					{
						nodes.push_back(node.ID);
					}
				}
				return nodes;
			};
			CHECK(framed(moved, a.Rect) == framed(synced, b.Rect));
		}
		CHECK(moved.GetBounds() == synced.GetBounds());
	}

	// What two spatial indices built from the models return over a grid of rectangles of several sizes covering the graph.
	void CheckSameIndex(VisualScriptCanvasModel const& moved, VisualScriptCanvasModel const& synced)
	{
		VisualScriptSpatialIndex movedIndex;
		VisualScriptSpatialIndex syncedIndex;
		movedIndex.Build(moved);
		syncedIndex.Build(synced);
		std::optional<VisualScriptRect> const bounds = synced.GetBounds();
		REQUIRE(bounds.has_value());
		std::vector<uint32_t> movedItems;
		std::vector<uint32_t> syncedItems;
		for (float const size : {96.0f, 400.0f, 1500.0f})
		{
			for (float x = bounds->Min.x - size; x < bounds->Max.x + size; x += size * 0.75f)
			{
				for (float y = bounds->Min.y - size; y < bounds->Max.y + size; y += size * 0.75f)
				{
					VisualScriptRect const rect{glm::vec2(x, y), glm::vec2(x + size, y + size)};
					movedIndex.Query(rect, VisualScriptSpatialIndex::AllMask, movedItems);
					syncedIndex.Query(rect, VisualScriptSpatialIndex::AllMask, syncedItems);
					std::sort(movedItems.begin(), movedItems.end());
					std::sort(syncedItems.begin(), syncedItems.end());
					CHECK(movedItems == syncedItems);
				}
			}
		}
	}
}

TEST_CASE("VisualScriptEditor: after random layout-only commits, moving what moved gives the visual model and index a full sync gives")
{
	// One model moves what moved (SyncPositions), the other syncs every document fully; a third lays the last one out anew.
	VisualScriptDocument document = Testing::MakeGridGraph(8, 6, 2, 6);
	document.Graph.Nodes[3].Comment = "A note\non two lines";
	document.Graph.Nodes[10].Collapsed = true;
	document.Graph.Nodes[11].Position.reset();
	CanvasFixture moved;
	CanvasFixture synced;
	moved.Document = document;
	synced.Document = document;
	moved.Analyze();
	synced.Analyze();
	REQUIRE(moved.Sync());
	REQUIRE(synced.Sync());
	CheckSameCanvas(moved.Model, synced.Model);

	Testing::FuzzRandom random(11500);
	auto const offset = [&random]()
	{
		// Quarters of units, and now and then a fraction no float holds exactly.
		float const x = static_cast<float>(static_cast<int64_t>(random.Pick(4001)) - 2000) * 0.25f;
		float const y = random.Pick(8) == 0 ? 0.1f : static_cast<float>(static_cast<int64_t>(random.Pick(4001)) - 2000) * 0.25f;
		return glm::vec2(x, y);
	};
	for (int round = 0; round < 40; round++)
	{
		CAPTURE(round);
		VisualScriptDocument const previous = moved.Document;
		VisualScriptDocument next = previous;
		// A random subset of nodes and comments moves; now and then nothing does.
		for (VisualScriptNode& node : next.Graph.Nodes)
		{
			if (random.Pick(4) == 0)
			{
				node.Position = node.Position.value_or(glm::vec2(0.0f)) + offset();
			}
		}
		for (VisualScriptComment& comment : next.Graph.Comments)
		{
			if (random.Pick(3) == 0)
			{
				comment.Position += offset();
			}
		}
		moved.Document = next;
		synced.Document = next;
		std::optional<bool> const geometry = moved.SyncPositions(previous);
		REQUIRE(geometry.has_value());
		CHECK(*geometry == synced.Sync());
		CHECK(moved.Model.GetMeasuredNodeCount() == 0);
		CheckSameCanvas(moved.Model, synced.Model);
		if (round % 8 == 0)
		{
			CheckSameIndex(moved.Model, synced.Model);
		}
	}
	CHECK(moved.Model.GetPositionSyncCount() == 40);
	CheckSameIndex(moved.Model, synced.Model);
	// A full Sync after the moves keeps what they gave.
	CHECK_FALSE(moved.Sync());
	CHECK(moved.Model.GetMeasuredNodeCount() == 0);
	CheckSameCanvas(moved.Model, synced.Model);

	// Laid out anew, the last document gives the same model, up to the rounding of positions moved by differences.
	CanvasFixture fresh;
	fresh.Document = moved.Document;
	fresh.Analyze();
	fresh.Sync();
	REQUIRE(fresh.Model.GetNodes().size() == moved.Model.GetNodes().size());
	for (size_t index = 0; index < fresh.Model.GetNodes().size(); index++)
	{
		CheckRect(moved.Model.GetNodes()[index].VisualBounds, fresh.Model.GetNodes()[index].VisualBounds);
	}

	// Anything else lays out, so moving declines and the caller syncs: a resized, recoloured or renamed box, a collapsed
	// node, a node comment, a node removed, another sub-graph, another library.
	VisualScriptDocument const base = moved.Document;
	auto const declines = [&moved, &base](auto&& change)
	{
		VisualScriptDocument next = base;
		change(next);
		moved.Document = next;
		bool const declined = !moved.SyncPositions(base).has_value();
		moved.Document = base;
		return declined;
	};
	CHECK(declines(
		[](VisualScriptDocument& next)
		{
			next.Graph.Comments[0].Size.x += 16.0f;
		}));
	CHECK(declines(
		[](VisualScriptDocument& next)
		{
			next.Graph.Comments[0].Color.a = 0.5f;
		}));
	CHECK(declines(
		[](VisualScriptDocument& next)
		{
			next.Graph.Comments[0].Text = "Renamed";
		}));
	CHECK(declines(
		[](VisualScriptDocument& next)
		{
			next.Graph.Nodes[0].Collapsed = true;
		}));
	CHECK(declines(
		[](VisualScriptDocument& next)
		{
			next.Graph.Nodes[0].Comment = "Note";
		}));
	CHECK(declines(
		[](VisualScriptDocument& next)
		{
			next.Graph.Nodes.pop_back();
		}));
	CHECK(declines(
		[](VisualScriptDocument& next)
		{
			std::swap(next.Graph.Nodes[0], next.Graph.Nodes[1]);
		}));
	CHECK(declines(
		[](VisualScriptDocument& next)
		{
			next.Graph.Comments.pop_back();
		}));
	CHECK_FALSE(declines(
		[](VisualScriptDocument& next)
		{
			next.Graph.Nodes[0].Position = glm::vec2(-640.0f, 0.0f);
		}));
	moved.Function = 7;
	CHECK(declines([](VisualScriptDocument&) {}));
	moved.Function = 0;
	moved.WithLibrary = false;
	CHECK(declines([](VisualScriptDocument&) {}));
}

TEST_CASE("VisualScriptEditor: nodes take the analysis' pins while they are what it analyzed, else their template's, else the document's")
{
	CanvasFixture canvas;
	canvas.Document.Graph.Nodes = {MakeNode(1, "Flow.Delay", glm::vec2(0.0f), Json::object(), Json::object({{"Duration", 1}})),
	                               MakeNode(2, "Flow.Sequence", glm::vec2(300.0f, 0.0f), Json::object({{"Count", 4}})),
	                               MakeNode(3, "Flow.Teleport", glm::vec2(0.0f, 300.0f))};
	canvas.Document.Graph.Links = {MakeLink(1, "Completed", 2, "Execute"), MakeLink(1, "Completed", 3, "Execute")};
	canvas.Analyze();
	canvas.Sync();
	CHECK(canvas.Node(2).HasFlag(VisualScriptNodeFlags::Analyzed));
	CHECK(canvas.PinNames(2).size() == 5);
	// The analysis says it does not know Teleport: the Unknown header and a hatched body.
	VisualScriptNodeVisual const& unknown = canvas.Node(3);
	CHECK(unknown.HasFlag(VisualScriptNodeFlags::UnknownType));
	CHECK(unknown.Kind == VisualScriptNodeKind::Unknown);
	CHECK(unknown.HeaderColor == GetVisualScriptHeaderColor(VisualScriptNodeKind::Unknown, {}));

	// Changed settings: the template's pins (the default count) until the next analysis. A changed literal keeps the
	// analysis' pins.
	canvas.Document.Graph.Nodes[1].Settings["Count"] = 3;
	canvas.Document.Graph.Nodes[0].Inputs["Duration"] = 2;
	canvas.Sync();
	CHECK(canvas.Node(2).HasFlag(VisualScriptNodeFlags::Template));
	CHECK(canvas.PinNames(2) == std::vector<std::string>{"In:Execute", "Out:Then0", "Out:Then1"});
	CHECK(canvas.Node(1).HasFlag(VisualScriptNodeFlags::Analyzed));
	CHECK(canvas.Text(canvas.Pin(1, "Duration").ChipText) == "2");
	// A literal stored under a new key changes the pins the node may have: its template's until the next analysis.
	canvas.Document.Graph.Nodes[0].Inputs["Execute"] = 1;
	canvas.Sync();
	CHECK(canvas.Node(1).HasFlag(VisualScriptNodeFlags::Template));

	// Without a library: pins from the document, of no known type; titles from the types; neutral headers.
	CanvasFixture degraded;
	degraded.WithLibrary = false;
	degraded.Document.Graph.Nodes = {MakeNode(1, "Event.Create"), MakeNode(2, "Flow.Branch", glm::vec2(300.0f, 0.0f), Json::object(),
	                                                                       Json::object({{"Condition", true}}))};
	degraded.Document.Graph.Links = {MakeLink(1, "Then", 2, "Execute")};
	degraded.Sync();
	VisualScriptNodeVisual const& branch = degraded.Node(2);
	CHECK(branch.HasFlag(VisualScriptNodeFlags::Inferred));
	CHECK_FALSE(branch.HasFlag(VisualScriptNodeFlags::UnknownType));
	CHECK(degraded.Text(branch.Title) == "Branch");
	CHECK(branch.HeaderColor == GetVisualScriptHeaderColor(VisualScriptNodeKind::Flow, {}));
	CHECK(degraded.PinNames(2) == std::vector<std::string>{"In:Condition", "In:Execute"});
	VisualScriptPinVisual const& inferred = degraded.Pin(2, "Condition");
	CHECK(inferred.Type == VisualScriptWildcardType);
	CHECK(inferred.Shape == VisualScriptPinShape::Wildcard);
	CHECK(inferred.ChipKind == VisualScriptChipKind::Raw);
	CHECK(degraded.Text(inferred.ChipText) == "true");
	CHECK(degraded.PinNames(1) == std::vector<std::string>{"Out:Then"});
}

TEST_CASE("VisualScriptEditor: pins the analysis renamed keep the document's names on their links and literals until the next commit")
{
	CanvasFixture canvas;
	canvas.Document.Graph.Nodes = {MakeNode(1, LogInfo, glm::vec2(400.0f, 0.0f), Json::object(), Json::object({{"Text", "Hi"}})),
	                               MakeNode(2, "Get:Strada.Time.DeltaTime"), MakeNode(3, LogInfo, glm::vec2(400.0f, 300.0f))};
	canvas.Document.Graph.Links = {MakeLink(2, "Value", 3, "Text")};
	VisualScriptAnalysis& analysis = canvas.Analyze();
	analysis.Diagnostics.clear();
	for (VisualScriptAnalyzedNode& node : analysis.Nodes)
	{
		if (node.ID == 1 || node.ID == 3)
		{
			node.Resolved = VisualScriptResolution::Redirect;
			node.CanonicalType = std::string(LogInfo);
			node.PinRenames = {{VisualScriptPinDirection::In, "Text", "Message"}};
		}
	}
	canvas.Sync();

	// The stored literal shows on the renamed pin; no orphan row.
	CHECK(canvas.PinNames(1) == std::vector<std::string>{"In:Execute", "In:Message", "Out:Then"});
	CHECK(canvas.Text(canvas.Pin(1, "Message").ChipText) == "Hi");
	// The link ends at the renamed pin and still names the document's.
	REQUIRE(canvas.Model.GetLinks().size() == 1);
	std::span<VisualScriptLinkVisual const> const links = canvas.Model.GetLinks();
	VisualScriptLinkVisual const& link = links[0];
	CHECK(canvas.Text(canvas.Model.GetPins()[link.ToPin].Name) == "Message");
	CHECK(canvas.Text(link.ToName) == "Text");
	CHECK(canvas.Model.GetDocumentLink(0) == canvas.Document.Graph.Links[0]);
	CHECK(canvas.Pin(3, "Message").HasFlag(VisualScriptPinFlags::Linked));
	CHECK(canvas.Pin(3, "Message").ChipKind == VisualScriptChipKind::None);
	// A field of a renamed pin follows it.
	std::string buffer;
	CHECK(MapVisualScriptPinName(&analysis.Nodes[0], "Text.X", Direction::In, buffer) == "Message.X");
	CHECK(MapVisualScriptPinName(&analysis.Nodes[0], "Texts", Direction::In, buffer) == "Texts");
	CHECK(MapVisualScriptPinName(&analysis.Nodes[0], "Text", Direction::Out, buffer) == "Text");
}

TEST_CASE("VisualScriptEditor: the fields of a split pin the analysis renamed stay a split group, with their literals and problems")
{
	CanvasFixture canvas;
	VisualScriptNode set = MakeNode(1, SetTranslation, glm::vec2(0.0f), Json::object(), Json::object({{"Value.X", 2}}));
	set.Split.Inputs = {"Value"};
	canvas.Document.Graph.Nodes = {set};
	VisualScriptAnalysis& analysis = canvas.Analyze();
	// The analysis knows the pin as Offset now; the document still names Value, its split and its field's literal.
	VisualScriptAnalyzedNode& node = analysis.Nodes[0];
	node.Resolved = VisualScriptResolution::Redirect;
	node.CanonicalType = std::string(SetTranslation);
	node.PinRenames = {{VisualScriptPinDirection::In, "Value", "Offset"}};
	for (VisualScriptPin& pin : node.Pins)
	{
		if (pin.Name.starts_with("Value."))
		{
			pin.Name = "Offset" + pin.Name.substr(5);
		}
	}
	VisualScriptDiagnostic problem;
	problem.Code = "InvalidValue";
	problem.Node = 1;
	problem.Pin = "Value.Y";
	problem.Direction = VisualScriptPinDirection::In;
	problem.Message = "a problem on a field";
	analysis.Diagnostics = {problem};
	canvas.Sync();

	CHECK(canvas.PinNames(1) ==
	      std::vector<std::string>{"In:Execute", "In:Target", "In:Offset.X", "In:Offset.Y", "In:Offset.Z", "Out:Then"});
	VisualScriptPinVisual const& x = canvas.Pin(1, "Offset.X");
	CHECK(x.HasFlag(VisualScriptPinFlags::SplitField));
	CHECK(x.HasFlag(VisualScriptPinFlags::SplitGroupStart));
	CHECK(canvas.Text(x.ChipText) == "2");
	CHECK(canvas.Pin(1, "Offset.Y").HasFlag(VisualScriptPinFlags::HasError));
	CHECK_FALSE(canvas.Pin(1, "Offset.Z").HasFlag(VisualScriptPinFlags::HasError));
}

TEST_CASE("VisualScriptEditor: wires carry their ends' colours, conversions, errors and disabled ends")
{
	CanvasFixture canvas;
	VisualScriptNode delay = MakeNode(4, "Flow.Delay", glm::vec2(900.0f, 0.0f));
	delay.Enabled = VisualScriptNodeEnabled::Disabled;
	canvas.Document.Graph.Nodes = {MakeNode(1, "Event.Update"), MakeNode(2, LogInfo, glm::vec2(600.0f, 200.0f)),
	                               MakeNode(3, "Flow.Branch", glm::vec2(300.0f, 0.0f)), delay};
	canvas.Document.Graph.Links = {MakeLink(1, "Then", 3, "Execute"), MakeLink(1, "DeltaTime", 2, "Message"),
	                               MakeLink(3, "True", 4, "Execute"), MakeLink(3, "False", 2, "Execute")};
	VisualScriptDiagnostic error;
	error.Code = "TypeMismatch";
	error.Link = canvas.Document.Graph.Links[3];
	error.Message = "a test error";
	canvas.Analyze(std::span<VisualScriptDiagnostic const>(&error, 1));
	canvas.Sync();
	VisualScriptLibrary const& library = canvas.GetLibrary();

	std::span<VisualScriptLinkVisual const> const links = canvas.Model.GetLinks();
	REQUIRE(links.size() == 4);
	CHECK(links[0].Flags == VisualScriptLinkFlags::Exec);
	CHECK(links[0].FromColor == canvas.Theme.Text);
	CHECK(links[0].ToColor == canvas.Theme.Text);
	// A float into a string converts: the wire fades from the float's colour to the string's.
	CHECK(links[1].Flags == VisualScriptLinkFlags::Converted);
	CHECK(links[1].FromColor == GetVisualScriptTypeColor("float", &library));
	CHECK(links[1].ToColor == GetVisualScriptTypeColor("string", &library));
	CHECK(links[2].Flags == (VisualScriptLinkFlags::Exec | VisualScriptLinkFlags::Disabled));
	CHECK(links[3].Flags == (VisualScriptLinkFlags::Exec | VisualScriptLinkFlags::Error));
	for (uint32_t index = 0; index < links.size(); index++)
	{
		CHECK(links[index].Source == index);
	}

	// Without the problems shown, no error.
	canvas.ShowDiagnostics = false;
	canvas.Sync();
	CHECK_FALSE(canvas.Model.GetLinks()[3].HasFlag(VisualScriptLinkFlags::Error));
}

TEST_CASE("VisualScriptEditor: Get and Self are pills, binary operators boxes with their symbol, reroutes dots facing their first target")
{
	CanvasFixture canvas;
	canvas.Document.Variables = {MakeVariable(20, "Speed", "float")};
	canvas.Document.Graph.Nodes = {
		MakeNode(1, "Variable.Get", glm::vec2(0.0f), Json::object({{"Variable", 20}})),
		MakeNode(2, "Self", glm::vec2(0.0f, 100.0f)),
		MakeNode(3, "Operator.Add", glm::vec2(300.0f, 0.0f)),
		MakeNode(4, "Operator.Add", glm::vec2(300.0f, 200.0f), Json::object(), Json::object({{"B", 1}})),
		MakeNode(5, "Reroute", glm::vec2(500.0f, 100.0f)),
		MakeNode(6, "Event.Update", glm::vec2(0.0f, 300.0f)),
		MakeNode(7, "Flow.Branch", glm::vec2(100.0f, 500.0f)),
	};
	canvas.Document.Graph.Links = {MakeLink(1, "Value", 3, "A"), MakeLink(6, "DeltaTime", 3, "B"), MakeLink(6, "DeltaTime", 4, "A"),
	                               MakeLink(6, "Then", 5, "In"), MakeLink(5, "Out", 7, "Execute")};
	canvas.Analyze();
	canvas.Sync();

	// The pill shows the variable's name: 10 + 37.5 + 6 + 5 + 10.
	VisualScriptNodeVisual const& get = canvas.Node(1);
	CHECK(get.Shape == VisualScriptNodeShape::Pill);
	CHECK(canvas.Text(get.Title) == "Speed");
	CheckPoint(get.Size, glm::vec2(68.5f, 28.0f));
	CheckPoint(canvas.Pin(1, "Value", Direction::Out).Anchor, glm::vec2(58.5f, 14.0f));
	CHECK(canvas.Node(2).Shape == VisualScriptNodeShape::Pill);
	CHECK(canvas.Text(canvas.Node(2).Title) == "Self");

	VisualScriptNodeVisual const& add = canvas.Node(3);
	CHECK(add.Shape == VisualScriptNodeShape::Operator);
	CHECK(canvas.Text(add.Title) == "+");
	CheckPoint(add.Size, glm::vec2(64.0f, 48.0f));
	CheckPoint(canvas.Pin(3, "A").Anchor, glm::vec2(10.0f, 12.0f));
	CheckPoint(canvas.Pin(3, "B").Anchor, glm::vec2(10.0f, 36.0f));
	CheckPoint(canvas.Pin(3, "Result", Direction::Out).Anchor, glm::vec2(54.0f, 24.0f));
	CHECK(canvas.Pin(3, "A").Label.Length == 0);
	// An unlinked operand with a value of its own keeps the full shape, so the value shows.
	CHECK(canvas.Node(4).Shape == VisualScriptNodeShape::Standard);
	CHECK(canvas.Pin(4, "B").ChipKind == VisualScriptChipKind::Raw);

	VisualScriptNodeVisual const& reroute = canvas.Node(5);
	CHECK(reroute.Shape == VisualScriptNodeShape::Reroute);
	CheckPoint(reroute.Size, glm::vec2(12.0f));
	CheckPoint(Geometry::GetPinAnchor(reroute, canvas.Pin(5, "In")), glm::vec2(506.0f, 106.0f));
	CheckPoint(Geometry::GetPinAnchor(reroute, canvas.Pin(5, "Out", Direction::Out)), glm::vec2(506.0f, 106.0f));
	// Its first target lies to its left: the wire leaves it leftward and the wire into it arrives from the right.
	std::optional<uint32_t> const outgoing = canvas.Model.FindExecLink(*canvas.Model.FindNode(5), "Out");
	REQUIRE(outgoing.has_value());
	std::array<glm::vec2, 4> const leaving = Geometry::GetLinkControlPoints(canvas.Model, *outgoing);
	CHECK(leaving[1].x < leaving[0].x);
	std::array<glm::vec2, 4> const arriving =
		Geometry::GetLinkControlPoints(canvas.Model, *canvas.Model.FindExecLink(*canvas.Model.FindNode(6), "Then"));
	CHECK(arriving[2].x > arriving[3].x);
	// Moving the target to its right turns it around.
	VisualScriptNodePreview preview;
	std::vector<uint8_t> moved(canvas.Model.GetNodes().size(), 0);
	moved[*canvas.Model.FindNode(7)] = 1;
	preview.Moved = moved;
	preview.Offset = glm::vec2(800.0f, 0.0f);
	CHECK(Geometry::GetLinkControlPoints(canvas.Model, *outgoing, preview)[1].x >
	      Geometry::GetLinkControlPoints(canvas.Model, *outgoing, preview)[0].x);
}

TEST_CASE("VisualScriptEditor: comment boxes draw largest first, with their title line and wrapped body; node comments are bubbles")
{
	CanvasFixture canvas;
	VisualScriptComment small;
	small.ID = 10;
	small.Text = "Small\nBody line one two three";
	small.Position = glm::vec2(0.0f);
	small.Size = glm::vec2(400.0f, 200.0f);
	VisualScriptComment big;
	big.ID = 11;
	big.Text = "Big";
	big.Position = glm::vec2(-50.0f);
	big.Size = glm::vec2(1000.0f, 600.0f);
	big.Color = glm::vec4(0.2f, 0.45f, 0.85f, 0.25f);
	canvas.Document.Graph.Comments = {small, big};
	VisualScriptNode noted = MakeNode(1, "Flow.Branch", glm::vec2(100.0f, 300.0f));
	noted.Comment = "A note on this node";
	VisualScriptNode chatty = MakeNode(2, "Flow.Branch", glm::vec2(600.0f, 300.0f));
	for (int word = 0; word < 400; word++)
	{
		chatty.Comment += "word ";
	}
	canvas.Document.Graph.Nodes = {noted, chatty};
	canvas.Analyze();
	canvas.Sync();

	std::span<VisualScriptCommentVisual const> const comments = canvas.Model.GetComments();
	REQUIRE(comments.size() == 2);
	CHECK(comments[0].ID == 11);
	CHECK(comments[1].ID == 10);
	CHECK(canvas.Model.FindComment(10) == 1u);
	VisualScriptCommentVisual const& inner = comments[1];
	CheckRect(inner.Rect, {glm::vec2(0.0f), glm::vec2(400.0f, 200.0f)});
	CHECK(canvas.Text(inner.Title) == "Small");
	CHECK(inner.Title.Width == doctest::Approx(45.0f));
	CHECK(canvas.Text(inner.Body) == "Body line one two three");
	VisualScriptCommentColors const colors = ComputeVisualScriptCommentColors(big.Color, canvas.Theme);
	CHECK(comments[0].BarColor == colors.Bar);
	CHECK(comments[0].BarTextColor == colors.BarText);
	CHECK(comments[0].BodyColor == colors.Body);
	CHECK(comments[0].BorderColor == colors.Border);

	// The bubble above the node, its tail down to it: one line of 123.5 units with 6 of padding around it.
	VisualScriptNodeVisual const& node = canvas.Node(1);
	CHECK(canvas.Text(node.Comment) == "A note on this node");
	CheckRect(node.BubbleRect, {glm::vec2(0.0f, -31.0f), glm::vec2(135.5f, -6.0f)});
	CHECK(node.VisualBounds.Min.y == doctest::Approx(300.0f - 31.0f));
	// A long note keeps its first 16 lines, wrapped at 240, the last ending with an ellipsis.
	std::string const chattyText = canvas.Text(canvas.Node(2).Comment);
	CHECK(std::count(chattyText.begin(), chattyText.end(), '\n') == 15);
	CHECK(chattyText.ends_with(VisualScriptEllipsis));
	CHECK(canvas.Node(2).Comment.Width <= VisualScriptCanvasMetrics::NodeCommentWrapWidth);
}

TEST_CASE("VisualScriptEditor: problems show as badges and pin rings, and an analysis kept without the runtime shows none")
{
	CanvasFixture canvas;
	canvas.Document.Graph.Nodes = {MakeNode(1, "Flow.Branch"), MakeNode(2, "Event.Create", glm::vec2(0.0f, 300.0f))};
	VisualScriptDiagnostic error;
	error.Code = "InvalidValue";
	error.Node = 1;
	error.Pin = "Condition";
	error.Direction = VisualScriptPinDirection::In;
	error.Message = "an error";
	VisualScriptDiagnostic warning;
	warning.Severity = VisualScriptSeverity::Warning;
	warning.Code = "UnusedSplit";
	warning.Node = 1;
	warning.Message = "a warning";
	VisualScriptDiagnostic createWarning = warning;
	createWarning.Node = 2;
	std::array<VisualScriptDiagnostic, 3> const problems = {error, warning, createWarning};
	canvas.Analyze(problems);
	canvas.Sync();

	VisualScriptNodeVisual const& branch = canvas.Node(1);
	CHECK(branch.ErrorCount == 1);
	CHECK(branch.WarningCount == 1);
	CHECK(canvas.Pin(1, "Condition").HasFlag(VisualScriptPinFlags::HasError));
	CHECK_FALSE(canvas.Pin(1, "Execute").HasFlag(VisualScriptPinFlags::HasError));
	// Branch is as wide as its pins; Create's header makes room for its badge: 20 + 16 + 45 + 16 is still below 120.
	CHECK(branch.Size.x == doctest::Approx(182.0f));
	CHECK(canvas.Node(2).WarningCount == 1);
	CHECK(canvas.Node(2).ErrorCount == 0);

	canvas.ShowDiagnostics = false;
	canvas.Sync();
	CHECK(canvas.Node(1).ErrorCount == 0);
	CHECK(canvas.Node(1).WarningCount == 0);
	CHECK_FALSE(canvas.Pin(1, "Condition").HasFlag(VisualScriptPinFlags::HasError));
	CHECK(canvas.Node(1).HasFlag(VisualScriptNodeFlags::Analyzed));
}

TEST_CASE("VisualScriptEditor: the visual model finds nodes, comments, pins and links by their document names, and their bounds")
{
	CanvasFixture canvas;
	canvas.Document.Graph.Nodes = {MakeNode(5, "Event.Create"), MakeNode(9, "Flow.Branch", glm::vec2(300.0f, 0.0f))};
	canvas.Document.Graph.Links = {MakeLink(5, "Then", 9, "Execute")};
	VisualScriptComment comment;
	comment.ID = 12;
	comment.Position = glm::vec2(-100.0f, -100.0f);
	comment.Size = glm::vec2(50.0f, 50.0f);
	canvas.Document.Graph.Comments = {comment};
	canvas.Analyze();
	canvas.Sync();

	CHECK(canvas.Model.FindNode(5) == 0u);
	CHECK(canvas.Model.FindNode(9) == 1u);
	CHECK_FALSE(canvas.Model.FindNode(12).has_value());
	CHECK(canvas.Model.FindComment(12) == 0u);
	CHECK(canvas.Model.FindPin(1, "Condition", Direction::In).has_value());
	CHECK_FALSE(canvas.Model.FindPin(1, "Condition", Direction::Out).has_value());
	CHECK(canvas.Model.FindExecLink(0, "Then") == 0u);
	CHECK_FALSE(canvas.Model.FindExecLink(1, "True").has_value());
	CHECK(canvas.Model.GetIncidentLinks(0).size() == 1);
	CHECK(canvas.Model.GetIncidentLinks(1).size() == 1);
	CHECK(canvas.Model.GetFirstOutgoingLink(0) == 0u);
	CHECK_FALSE(canvas.Model.GetFirstOutgoingLink(1).has_value());
	CHECK(canvas.Model.GetDocumentLink(0) == canvas.Document.Graph.Links[0]);
	CheckPoint(canvas.Model.GetExtents(9), glm::vec2(182.0f, 90.0f));
	CheckPoint(canvas.Model.GetExtents(77), glm::vec2(0.0f));

	std::array<uint32_t, 2> const selection = {9, 12};
	std::optional<VisualScriptRect> const bounds = canvas.Model.GetBounds(selection);
	REQUIRE(bounds.has_value());
	CheckRect(*bounds, {glm::vec2(-100.0f), glm::vec2(482.0f, 90.0f)});
	CHECK_FALSE(canvas.Model.GetBounds(std::span<uint32_t const>()).has_value());
	CheckRect(*canvas.Model.GetBounds(), {glm::vec2(-100.0f), glm::vec2(482.0f, 90.0f)});
	CHECK(canvas.Model.GetFunction() == 0);
}

TEST_CASE("VisualScriptEditor: in a function's graph, pills show locals and the function nodes have the function glyph")
{
	CanvasFixture canvas;
	canvas.Document.Variables = {MakeVariable(1, "Score", "int")};
	VisualScriptFunction function;
	function.ID = 2;
	function.Name = "Count";
	function.Locals = {{3, "Tally", "int", Json()}};
	function.Graph.Nodes = {MakeNode(4, "Function.Entry"),
	                        MakeNode(5, "Variable.Get", glm::vec2(0.0f, 100.0f), Json::object({{"Variable", 3}})),
	                        MakeNode(6, "Function.Return", glm::vec2(300.0f, 0.0f))};
	function.Graph.Links = {MakeLink(4, "Then", 6, "Execute")};
	canvas.Document.Functions = {function};
	canvas.Document.Graph.Nodes = {MakeNode(7, "Variable.Get", glm::vec2(0.0f), Json::object({{"Variable", 1}}))};
	canvas.Analyze();
	canvas.Function = 2;
	canvas.Sync();

	CHECK(canvas.Model.GetNodes().size() == 3);
	CHECK(canvas.Model.GetFunction() == 2);
	CHECK(canvas.Text(canvas.Node(5).Title) == "Tally");
	CHECK(canvas.Node(4).HasFlag(VisualScriptNodeFlags::FunctionNode));
	CHECK(canvas.Node(6).HasFlag(VisualScriptNodeFlags::FunctionNode));
	CHECK(canvas.Node(4).HasFlag(VisualScriptNodeFlags::Analyzed));
	CHECK(canvas.Pin(6, "Execute").HasFlag(VisualScriptPinFlags::LabelHidden));

	// A function that does not exist shows nothing.
	canvas.Function = 99;
	canvas.Sync();
	CHECK(canvas.Model.GetNodes().empty());
	CHECK(canvas.Model.GetLinks().empty());
	canvas.Function = 0;
	canvas.Sync();
	CHECK(canvas.Text(canvas.Node(7).Title) == "Score");
}

TEST_CASE(
	"VisualScriptEditor: a full sync of 10,000 nodes and 40,000 links lays out each node once; idle syncs and index rebuilds reserve nothing more")
{
	CanvasFixture canvas;
	canvas.Document = Testing::MakeGridGraph(100, 100, 4, 50);
	REQUIRE(canvas.Document.Graph.Nodes.size() == 10000);
	REQUIRE(canvas.Document.Graph.Links.size() == 40000);

	using Clock = std::chrono::steady_clock;
	Clock::time_point const syncStart = Clock::now();
	CHECK(canvas.Sync());
	double const syncSeconds = std::chrono::duration<double>(Clock::now() - syncStart).count();
	CHECK(canvas.Model.GetMeasuredNodeCount() == 10000);
	CHECK(canvas.Model.GetNodes().size() == 10000);
	CHECK(canvas.Model.GetLinks().size() == 40000);
	CHECK(canvas.Model.GetComments().size() == 50);

	VisualScriptSpatialIndex index;
	Clock::time_point const buildStart = Clock::now();
	index.Build(canvas.Model);
	double const buildSeconds = std::chrono::duration<double>(Clock::now() - buildStart).count();
	CHECK(index.GetItemCount(VisualScriptSpatialIndex::ItemKind::Node) == 10000);
	CHECK(index.GetItemCount(VisualScriptSpatialIndex::ItemKind::Link) == 40000);

	// Idle: the same document lays out nothing again, and neither the model nor a rebuilt index reserves more memory.
	size_t const modelCapacity = canvas.Model.GetCapacityBytes();
	size_t const indexCapacity = index.GetCapacityBytes();
	Clock::time_point const idleStart = Clock::now();
	for (int frame = 0; frame < 10; frame++)
	{
		CHECK_FALSE(canvas.Sync());
		CHECK(canvas.Model.GetMeasuredNodeCount() == 0);
		index.Build(canvas.Model);
		CHECK(canvas.Model.GetCapacityBytes() == modelCapacity);
		CHECK(index.GetCapacityBytes() == indexCapacity);
	}
	double const idleSeconds = std::chrono::duration<double>(Clock::now() - idleStart).count() / 10.0;

	// A hit test visits at most 4 cells at every zoom.
	VisualScriptView view;
	view.SetCanvas(glm::vec2(0.0f), glm::vec2(1600.0f, 900.0f), 1.0f);
	VisualScriptHitScratch scratch;
	for (float const zoom : VisualScriptZoomLevels)
	{
		view.SetState({0, glm::vec2(16000.0f, 9000.0f), zoom});
		VisualScriptIndexStats stats;
		VisualScriptHitContext context;
		context.Stats = &stats;
		context.Scratch = &scratch;
		static_cast<void>(HitTestCanvas(canvas.Model, index, view, context, glm::vec2(800.0f, 450.0f)));
		CHECK(stats.CellsVisited <= 4);
	}

	if (Testing::ArePerformanceTestsEnabled())
	{
		MESSAGE("full sync of 10,000 nodes and 40,000 links: ", syncSeconds * 1000.0, " ms; index build: ", buildSeconds * 1000.0,
		        " ms; idle sync and index rebuild: ", idleSeconds * 1000.0, " ms");
		// 3 × the medians of 5 runs measured on the track's machine (track M's final report): 438 ms and 41 ms in Debug,
		// 24.4 ms and 5.4 ms in Release.
#if defined(ST_DEBUG)
		constexpr double SyncBound = 3.0 * 0.438;
		constexpr double BuildBound = 3.0 * 0.041;
#else
		constexpr double SyncBound = 3.0 * 0.0244;
		constexpr double BuildBound = 3.0 * 0.0054;
#endif
		CHECK(syncSeconds <= SyncBound);
		CHECK(buildSeconds <= BuildBound);
	}
}
