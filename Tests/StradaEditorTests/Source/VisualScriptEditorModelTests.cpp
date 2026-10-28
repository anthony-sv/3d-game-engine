#include "TestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptHitTest.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptSpatialIndex.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

using namespace Strada;

namespace
{
	VisualScriptNode MakeNode(uint32_t id, std::string type, std::optional<glm::vec2> position, Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = position;
		node.Inputs = std::move(inputs);
		return node;
	}

	// Create, a Delay with a stored duration, and a Branch that is not placed yet.
	VisualScriptDocument MakeDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes = {MakeNode(1, "Event.Create", glm::vec2(0.0f)),
		                        MakeNode(2, "Flow.Delay", glm::vec2(300.0f, 0.0f), Json::object({{"Duration", 1}})),
		                        MakeNode(3, "Flow.Branch", std::nullopt)};
		document.Graph.Links = {{{1, "Then"}, {2, "Execute"}}, {{2, "Completed"}, {3, "Execute"}}};
		return document;
	}

	// The library without analyses: what the model shows with nodes drawn from their templates (an analysis that fails is
	// not asked for again until the document changes).
	class LibraryOnlySemantics final : public VisualScriptSemantics
	{
	public:
		Ref<VisualScriptLibrary const> GetLibrary() override { return m_Fixture.GetLibrary(); }
		[[nodiscard]] Result<VisualScriptAnalysis> Analyze(VisualScriptDocument const&) override
		{
			return Error{"not analyzed in this test"};
		}

	private:
		Testing::FixtureVisualScriptSemantics m_Fixture;
	};

	// A graph window's model of a project's graph, synced frame by frame as the panel syncs it.
	struct ModelFixture
	{
		explicit ModelFixture(VisualScriptDocument const& document)
			: Project(Testing::MakeGraphProject(Temporary, document)),
			  Settings(Temporary.GetPath() / "Views.json"),
			  Model(Project.Graph, Settings)
		{
		}

		// The next frame's Sync, step seconds after the last one.
		VisualScriptSyncResult Frame(bool widgetActive = false, double step = 1.0 / 60.0)
		{
			Time += step;
			return Model.Sync(Semantics, Measure, Theme, widgetActive, Time);
		}

		Ref<VisualScriptAsset> GetAsset() const { return AssetManager::GetAsset<VisualScriptAsset>(Project.Graph); }

		// A commit as the panel makes it: the asset takes the document object, the model is told (with the request's
		// LayoutOnly), and syncs again in the frame.
		VisualScriptSyncResult Commit(VisualScriptDocument document, bool layoutOnly = false)
		{
			Ref<VisualScriptDocument const> const committed = CreateRef<VisualScriptDocument const>(std::move(document));
			GetAsset()->SetDocument(committed);
			Model.OnCommitted(committed, layoutOnly);
			return Model.Sync(Semantics, Measure, Theme, false, Time);
		}

		std::vector<uint32_t> GetChanged() const
		{
			std::vector<uint32_t> changed(Model.ChangedElements().begin(), Model.ChangedElements().end());
			std::sort(changed.begin(), changed.end());
			return changed;
		}

		VisualScriptNodeVisual const& Node(uint32_t id) const { return Model.GetCanvas().GetNodes()[*Model.GetCanvas().FindNode(id)]; }

		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Temporary;
		Testing::GraphProject Project;
		VisualScriptViewSettings Settings;
		Testing::FixtureVisualScriptSemantics Semantics;
		VisualScriptTextMeasurer Measure = Testing::FixedWidthMeasurer(0.5f);
		VisualScriptThemeAnchors Theme;
		VisualScriptEditorModel Model;
		double Time = 0.0;
	};

	using Intents = std::vector<VisualScriptEditorIntent>;

	void Append(Intents& all, Intents more)
	{
		for (VisualScriptEditorIntent& intent : more)
		{
			all.push_back(std::move(intent));
		}
	}

	// The intents of one kind, in order.
	template<typename T>
	std::vector<T> Of(Intents const& intents)
	{
		std::vector<T> found;
		for (VisualScriptEditorIntent const& intent : intents)
		{
			if (T const* typed = std::get_if<T>(&intent))
			{
				found.push_back(*typed);
			}
		}
		return found;
	}

	// The one intent of a sequence, which must be of this kind.
	template<typename T>
	T Single(Intents const& intents)
	{
		REQUIRE(intents.size() == 1);
		REQUIRE(std::holds_alternative<T>(intents.front()));
		return std::get<T>(intents.front());
	}

	std::set<uint32_t> AsSet(std::span<uint32_t const> elements)
	{
		return std::set<uint32_t>(elements.begin(), elements.end());
	}

	VisualScriptLink MakeLink(uint32_t fromNode, std::string fromPin, uint32_t toNode, std::string toPin)
	{
		return {{fromNode, std::move(fromPin)}, {toNode, std::move(toPin)}};
	}

	// The canvas the interaction tests drive: Create (1) -> Delay (2) -> Branch (3) in a row, Update (4: Then, DeltaTime float),
	// For Each (6: Index int), Log Info (5: Message string) and Raycast (7: LayerMask uint) in the row below, and the comment
	// box "Start" (20) around Create and Delay. Between the rows, and right of the comment, lies empty canvas.
	VisualScriptDocument MakeCanvasDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes = {
			MakeNode(1, "Event.Create", glm::vec2(0.0f, 0.0f)),
			MakeNode(2, "Flow.Delay", glm::vec2(400.0f, 0.0f)),
			MakeNode(3, "Flow.Branch", glm::vec2(800.0f, 0.0f)),
			MakeNode(4, "Event.Update", glm::vec2(0.0f, 320.0f)),
			MakeNode(5, "Call:Strada.Log.Info(string)", glm::vec2(800.0f, 320.0f)),
			MakeNode(6, "Flow.ForEach", glm::vec2(400.0f, 320.0f)),
			MakeNode(7, "Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)",
		             glm::vec2(1200.0f, 320.0f)),
		};
		document.Graph.Links = {MakeLink(1, "Then", 2, "Execute"), MakeLink(2, "Completed", 3, "Execute")};
		VisualScriptComment comment;
		comment.ID = 20;
		comment.Text = "Start";
		comment.Position = glm::vec2(-48.0f, -80.0f);
		comment.Size = glm::vec2(720.0f, 240.0f);
		document.Graph.Comments = {comment};
		return document;
	}

	// A graph window's model driven by synthetic input, frame by frame, as the panel drives it: each frame is a Sync, then an
	// Update with the input, whose one-frame members (presses, releases, wheel, keys) are cleared afterwards. The canvas is 1920
	// x 1080 pixels at the screen's origin, hovered and focused; the first frame frames the whole graph (at 100 %). With
	// memoryAsset the window shows the document as a memory asset (read-only) instead of the project's file.
	struct InteractionFixture
	{
		explicit InteractionFixture(VisualScriptDocument const& document, bool memoryAsset = false)
			: Project(Testing::MakeGraphProject(Temporary, document)),
			  Graph(memoryAsset ? AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(document), "Generated") : Project.Graph),
			  Settings(Temporary.GetPath() / "Views.json"),
			  Model(Graph, Settings)
		{
			Settings.SetProject(Project.Project->GetFilePath());
			Input.CanvasSize = glm::vec2(1920.0f, 1080.0f);
			Input.Hovered = true;
			Input.Focused = true;
			Frame();
		}

		Intents Frame(double step = 1.0 / 60.0)
		{
			Time += step;
			Input.Time = Time;
			Model.Sync(Semantics, Measure, Theme, false, Time);
			Intents intents = Model.Update(Input);
			Input.Pressed = {};
			Input.Released = {};
			Input.DoubleClicked = {};
			Input.ExtraPressed = {};
			Input.Wheel = glm::vec2(0.0f);
			Input.SpacePressed = false;
			Input.SpaceReleased = false;
			Input.Characters.clear();
			Input.Actions.clear();
			Input.Drop.reset();
			return intents;
		}

		Intents MoveTo(glm::vec2 point, double step = 1.0 / 60.0)
		{
			Input.Mouse = point;
			return Frame(step);
		}

		Intents Press(int button = 0)
		{
			Input.Down[static_cast<size_t>(button)] = true;
			Input.Pressed[static_cast<size_t>(button)] = true;
			return Frame();
		}

		Intents Release(int button = 0)
		{
			Input.Down[static_cast<size_t>(button)] = false;
			Input.Released[static_cast<size_t>(button)] = true;
			return Frame();
		}

		Intents Click(glm::vec2 point, int button = 0)
		{
			Intents all = MoveTo(point);
			Append(all, Press(button));
			Append(all, Release(button));
			return all;
		}

		Intents DoubleClick(glm::vec2 point)
		{
			Intents all = Click(point);
			Input.Down[0] = true;
			Input.Pressed[0] = true;
			Input.DoubleClicked[0] = true;
			Append(all, Frame());
			Append(all, Release());
			return all;
		}

		// Presses at from, moves to to in straight steps, and releases there (unless release is false).
		Intents Drag(glm::vec2 from, glm::vec2 to, int button = 0, int steps = 4, bool release = true)
		{
			Intents all = MoveTo(from);
			Append(all, Press(button));
			for (int step = 1; step <= steps; step++)
			{
				Append(all, MoveTo(glm::mix(from, to, static_cast<float>(step) / static_cast<float>(steps))));
			}
			if (release)
			{
				Append(all, Release(button));
			}
			return all;
		}

		Intents Act(VisualScriptEditorAction action)
		{
			Input.Actions.push_back(action);
			return Frame();
		}

		// A commit as the panel makes it, in the current frame: the asset takes the object, the model is told and syncs again.
		void Commit(VisualScriptDocument document)
		{
			Ref<VisualScriptDocument const> const committed = CreateRef<VisualScriptDocument const>(std::move(document));
			AssetManager::GetAsset<VisualScriptAsset>(Graph)->SetDocument(committed);
			Model.OnCommitted(committed);
			Model.Sync(Semantics, Measure, Theme, false, Time);
		}

		// The screen point of a canvas point under the current view.
		glm::vec2 Screen(glm::vec2 canvasPoint) const { return Model.GetView().ToScreen(canvasPoint); }
		glm::vec2 Canvas(glm::vec2 screenPoint) const { return Model.GetView().ToCanvas(screenPoint); }

		// A pin's anchor on a collapsed node drawn expanded (under a wire resting on it).
		glm::vec2 ExpandedPin(uint32_t node, std::string_view pin, VisualScriptPinDirection direction) const
		{
			VisualScriptCanvasModel const& canvas = Model.GetCanvas();
			std::optional<uint32_t> const index = canvas.FindNode(node);
			REQUIRE(index.has_value());
			std::optional<uint32_t> const pinIndex = canvas.FindPin(*index, pin, direction);
			REQUIRE(pinIndex.has_value());
			VisualScriptNodeVisual const expanded = VisualScriptGeometry::GetExpandedNode(canvas.GetNodes()[*index]);
			return Screen(VisualScriptGeometry::GetPinAnchor(expanded, canvas.GetPins()[*pinIndex]));
		}

		// A move of the primary element's top-left from origin by a drag from one screen point to another, as the canvas snaps it.
		glm::vec2 SnappedOffset(glm::vec2 origin, glm::vec2 from, glm::vec2 to) const
		{
			glm::vec2 const moved = origin + Canvas(to) - Canvas(from);
			return glm::vec2(std::round(moved.x / 16.0f) * 16.0f, std::round(moved.y / 16.0f) * 16.0f) - origin;
		}

		std::vector<uint32_t> Selection() const { return {Model.GetSelection().begin(), Model.GetSelection().end()}; }

		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Temporary;
		Testing::GraphProject Project;
		AssetHandle Graph;
		VisualScriptViewSettings Settings;
		Testing::FixtureVisualScriptSemantics Semantics;
		VisualScriptTextMeasurer Measure = Testing::FixedWidthMeasurer(0.5f);
		VisualScriptThemeAnchors Theme;
		VisualScriptEditorModel Model;
		VisualScriptCanvasInput Input;
		double Time = 0.0;
	};

	// Empty canvas between the rows, outside the comment.
	constexpr glm::vec2 EmptyCanvasPoint{600.0f, 250.0f};
	// Inside the comment's body, between Create and Delay, below their wire.
	constexpr glm::vec2 CommentBodyPoint{250.0f, 120.0f};
}

TEST_CASE("VisualScriptEditor: a graph window shows its asset's document with the unplaced nodes placed in memory")
{
	ModelFixture fixture(MakeDocument());
	VisualScriptSyncResult const first = fixture.Frame();
	CHECK(first.State == VisualScriptDocumentState::Editable);
	CHECK(first.DocumentChanged);
	CHECK(first.AnalysisChanged);
	CHECK(first.GeometryChanged);
	CHECK_FALSE(fixture.Model.IsReadOnly());

	// The stored document is the asset's own object, its Branch unplaced; the display copy has it placed.
	CHECK(fixture.Model.GetStoredDocument() == fixture.GetAsset()->GetSharedDocument());
	CHECK_FALSE(fixture.Model.GetStoredDocument()->Graph.Nodes[2].Position.has_value());
	VisualScriptDocument expected = MakeDocument();
	PlaceUnplacedNodes(expected);
	CHECK(fixture.Model.GetDisplayDocument() == expected);
	CHECK(fixture.Model.GetCanvas().GetNodes().size() == 3);
	CHECK(fixture.Node(3).Position == *expected.Graph.Nodes[2].Position);
	CHECK(fixture.Model.GetIndex().GetItemCount(VisualScriptSpatialIndex::ItemKind::Node) == 3);
	CHECK(fixture.Model.GetAnalysis() != nullptr);

	// Nothing changed: nothing to do.
	VisualScriptSyncResult const idle = fixture.Frame();
	CHECK_FALSE(idle.DocumentChanged);
	CHECK_FALSE(idle.AnalysisChanged);
	CHECK_FALSE(idle.GeometryChanged);

	// A placed document is shown as it is, without a copy.
	VisualScriptDocument placed = expected;
	fixture.Commit(placed);
	CHECK(fixture.Model.GetStoredDocument().get() == &fixture.Model.GetDisplayDocument());
}

TEST_CASE(
	"VisualScriptEditor: a window's own commits are known by their object; every other change is an outside change, a redo of its own step included")
{
	ModelFixture fixture(MakeDocument());
	fixture.Frame();
	VisualScriptDocument moved = fixture.Model.GetDisplayDocument();
	moved.Graph.Nodes[1].Position = glm::vec2(320.0f, 16.0f);

	// The window's own commit: nothing flashes.
	Ref<VisualScriptDocument const> const own = CreateRef<VisualScriptDocument const>(moved);
	fixture.GetAsset()->SetDocument(own);
	fixture.Model.OnCommitted(own);
	VisualScriptSyncResult const committed = fixture.Model.Sync(fixture.Semantics, fixture.Measure, fixture.Theme, false, fixture.Time);
	CHECK(committed.DocumentChanged);
	CHECK(fixture.Model.ChangedElements().empty());
	CHECK(fixture.Node(2).Position == glm::vec2(320.0f, 16.0f));
	// Its placements are now stored: no copy.
	CHECK(fixture.Model.GetStoredDocument() == own);

	// An undo (another object, the document as before): the elements it changed flash.
	VisualScriptDocument before = moved;
	before.Graph.Nodes[1].Position = glm::vec2(300.0f, 0.0f);
	before.Graph.Nodes[0].Inputs["Unused"] = 1;
	fixture.GetAsset()->SetDocument(before);
	fixture.Frame();
	CHECK(fixture.GetChanged() == std::vector<uint32_t>{1, 2});
	// Reported for one frame.
	fixture.Frame();
	CHECK(fixture.Model.ChangedElements().empty());

	// A redo of the window's own step brings back the very object it committed: by then it is an outside change.
	fixture.GetAsset()->SetDocument(own);
	fixture.Frame();
	CHECK(fixture.GetChanged() == std::vector<uint32_t>{1, 2});

	// A new comment and a removed node: the comment flashes; the removed node cannot.
	VisualScriptDocument commented = moved;
	VisualScriptComment comment;
	comment.ID = 20;
	commented.Graph.Comments.push_back(comment);
	commented.Graph.Nodes.pop_back();
	fixture.GetAsset()->SetDocument(commented);
	fixture.Frame();
	CHECK(fixture.GetChanged() == std::vector<uint32_t>{20});
	CHECK(fixture.Model.GetCanvas().GetNodes().size() == 2);
}

TEST_CASE("VisualScriptEditor: a reload of a file changed outside the editor is an outside change; an equal reload is not")
{
	ModelFixture fixture(MakeDocument());
	fixture.Frame();
	Ref<VisualScriptAsset> const first = fixture.GetAsset();

	// The file read again unchanged: a new asset object at version 0 with an equal document, adopted silently.
	REQUIRE(AssetManager::ReloadAsset(fixture.Project.Graph).IsOk());
	REQUIRE(fixture.GetAsset() != first);
	VisualScriptSyncResult const equal = fixture.Frame();
	CHECK(equal.DocumentChanged);
	CHECK(fixture.Model.ChangedElements().empty());
	CHECK(fixture.Model.GetStoredDocument() == fixture.GetAsset()->GetSharedDocument());

	// Before any commit of the window, the file changes outside the editor and is reloaded: an outside change.
	VisualScriptDocument changed = MakeDocument();
	changed.Graph.Nodes[1].Inputs["Duration"] = 4;
	REQUIRE(VisualScriptSerializer::SaveToFile(changed, fixture.Project.File).IsOk());
	REQUIRE(AssetManager::ReloadAsset(fixture.Project.Graph).IsOk());
	CHECK(fixture.GetAsset()->GetVersion() == 0);
	fixture.Frame();
	CHECK(fixture.GetChanged() == std::vector<uint32_t>{2});
	CHECK(fixture.Model.GetStoredDocument()->Graph.Nodes[1].Inputs.at("Duration") == 4);
}

TEST_CASE(
	"VisualScriptEditor: the analysis runs when the program changes, not for moves, comments or collapsing, and never without the runtime")
{
	ModelFixture fixture(MakeDocument());
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 1);
	CHECK(fixture.Semantics.GetAnalyzeCallCount() == 1);

	// What the scripting runtime never reads costs no analysis.
	VisualScriptDocument document = fixture.Model.GetDisplayDocument();
	document.Graph.Nodes[0].Position = glm::vec2(-64.0f, 32.0f);
	fixture.Commit(document);
	fixture.Frame();
	document.Graph.Nodes[1].Comment = "Waits";
	document.Graph.Nodes[1].Collapsed = true;
	VisualScriptComment box;
	box.ID = 30;
	document.Graph.Comments.push_back(box);
	document.Description = "Changed";
	fixture.Commit(document);
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 1);
	CHECK(fixture.Node(2).HasFlag(VisualScriptNodeFlags::Analyzed));

	// A literal is a program change: one analysis, not one per frame.
	document.Graph.Nodes[1].Inputs["Duration"] = 2;
	fixture.Commit(document);
	fixture.Frame();
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 2);
	// The same value written as a float is a different program: the runtime reads what is written.
	document.Graph.Nodes[1].Inputs["Duration"] = 2.0;
	fixture.Commit(document);
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 3);

	// A library read again (a new game assembly) analyzes anew.
	fixture.Semantics.ReloadLibrary();
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 4);
	// Read again in a frame whose analysis is put off (a widget in use within 0.25 s of the last one): the first frame that
	// may analyze does, with the new library, though no frame sees the library change again.
	fixture.Semantics.ReloadLibrary();
	fixture.Frame(true, 0.05);
	CHECK(fixture.Model.GetAnalysisCount() == 4);
	fixture.Frame(true, 0.5);
	CHECK(fixture.Model.GetAnalysisCount() == 5);
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 5);

	// The runtime stops: no analysis, none reported; the last one keeps its pins without its problems.
	VisualScriptDiagnostic error;
	error.Code = "InvalidValue";
	error.Node = 2;
	error.Message = "an error";
	fixture.Semantics.InjectDiagnostic(error);
	document.Graph.Nodes[1].Inputs["Duration"] = 3;
	fixture.Commit(document);
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 6);
	CHECK(fixture.Node(2).ErrorCount == 1);
	fixture.Semantics.SetAvailable(false);
	VisualScriptSyncResult const stopped = fixture.Frame();
	CHECK(stopped.AnalysisChanged);
	CHECK(fixture.Model.GetAnalysis() == nullptr);
	CHECK(fixture.Node(2).HasFlag(VisualScriptNodeFlags::Analyzed));
	CHECK(fixture.Node(2).ErrorCount == 0);
	document.Graph.Nodes[1].Inputs["Duration"] = 5;
	fixture.Commit(document);
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 6);
	CHECK(fixture.Semantics.GetAnalyzeCallCount() == 6);
	// A changed literal keeps the kept analysis' pins: the node's input keys are those analyzed.
	CHECK(fixture.Node(2).HasFlag(VisualScriptNodeFlags::Analyzed));

	// The runtime starts again: analyzed at once.
	fixture.Semantics.SetAvailable(true);
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 7);
	CHECK(fixture.Model.GetAnalysis() != nullptr);
	CHECK(fixture.Node(2).ErrorCount == 1);
}

TEST_CASE("VisualScriptEditor: an own layout-only commit keeps the analysis that described the document before it, and moves only what "
          "moved")
{
	ModelFixture fixture(MakeDocument());
	fixture.Frame();
	REQUIRE(fixture.Model.GetAnalysisCount() == 1);
	uint64_t const revision = fixture.Model.GetProgramRevision();
	uint64_t const positionSyncs = fixture.Model.GetCanvas().GetPositionSyncCount();

	// A move: the analysis describes the committed document (no analysis, the analysis current, the nodes drawn from it), the
	// canvas moves the node without laying anything out, and what reads the program keeps what it read.
	VisualScriptDocument document = fixture.Model.GetDisplayDocument();
	document.Graph.Nodes[1].Position = glm::vec2(320.0f, 48.0f);
	VisualScriptSyncResult const moved = fixture.Commit(document, true);
	CHECK(moved.DocumentChanged);
	CHECK_FALSE(moved.AnalysisChanged);
	CHECK(moved.GeometryChanged);
	CHECK(fixture.Model.GetAnalysisCount() == 1);
	REQUIRE(fixture.Model.GetAnalysis() != nullptr);
	CHECK(fixture.Node(2).HasFlag(VisualScriptNodeFlags::Analyzed));
	CHECK(fixture.Node(2).Position == glm::vec2(320.0f, 48.0f));
	CHECK(fixture.Model.GetCanvas().GetPositionSyncCount() == positionSyncs + 1);
	CHECK(fixture.Model.GetCanvas().GetMeasuredNodeCount() == 0);
	CHECK(fixture.Model.CommittedLinks().empty());
	CHECK(fixture.Model.GetProgramRevision() == revision);
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 1);

	// Collapsing is layout-only too, but lays its node out again: the canvas syncs fully.
	document.Graph.Nodes[1].Collapsed = true;
	fixture.Commit(document, true);
	CHECK(fixture.Model.GetCanvas().GetPositionSyncCount() == positionSyncs + 1);
	CHECK(fixture.Node(2).HasFlag(VisualScriptNodeFlags::Collapsed));
	CHECK(fixture.Model.GetAnalysisCount() == 1);
	CHECK(fixture.Model.GetProgramRevision() == revision);

	// The same move committed as any other commit: compared with the analyzed document instead, with the same outcome, and
	// read again by what reads the program.
	document.Graph.Nodes[1].Position = glm::vec2(352.0f, 48.0f);
	fixture.Commit(document);
	CHECK(fixture.Model.GetAnalysisCount() == 1);
	CHECK(fixture.Model.GetProgramRevision() == revision + 1);

	// A program change analyzed in its frame, a second one in the same frame (its analysis put off), then a layout-only
	// commit before the analysis caught up: the analysis describes neither, so the next frame analyzes.
	fixture.Frame();
	document.Graph.Nodes[1].Inputs["Duration"] = 2;
	fixture.Commit(document);
	CHECK(fixture.Model.GetAnalysisCount() == 2);
	document.Graph.Nodes[1].Inputs["Duration"] = 3;
	fixture.Commit(document);
	CHECK(fixture.Model.GetAnalysisCount() == 2);
	document.Graph.Nodes[1].Position = glm::vec2(384.0f, 48.0f);
	fixture.Commit(document, true);
	CHECK(fixture.Model.GetAnalysisCount() == 2);
	CHECK(fixture.Model.GetProgramRevision() == revision + 3);
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 3);
	CHECK(fixture.Node(2).HasFlag(VisualScriptNodeFlags::Analyzed));
	CHECK(fixture.Node(2).Position == glm::vec2(384.0f, 48.0f));

	// A document the runtime failed to analyze is not analyzed again for a layout-only commit of it, which has its program;
	// any other commit of it is.
	LibraryOnlySemantics failing;
	auto const frame = [&fixture, &failing]()
	{
		fixture.Time += 1.0 / 60.0;
		fixture.Model.Sync(failing, fixture.Measure, fixture.Theme, false, fixture.Time);
	};
	frame();
	uint64_t const failed = fixture.Model.GetAnalysisCount();
	CHECK(failed == 4);
	CHECK(fixture.Model.GetAnalysis() == nullptr);
	document.Graph.Nodes[1].Position = glm::vec2(416.0f, 48.0f);
	Ref<VisualScriptDocument const> const committed = CreateRef<VisualScriptDocument const>(document);
	fixture.GetAsset()->SetDocument(committed);
	fixture.Model.OnCommitted(committed, true);
	frame();
	frame();
	CHECK(fixture.Model.GetAnalysisCount() == failed);
	document.Graph.Nodes[1].Comment = "Waits";
	Ref<VisualScriptDocument const> const commented = CreateRef<VisualScriptDocument const>(document);
	fixture.GetAsset()->SetDocument(commented);
	fixture.Model.OnCommitted(commented);
	frame();
	CHECK(fixture.Model.GetAnalysisCount() == failed + 1);
}

TEST_CASE("VisualScriptEditor: disabling a node analyzes the graph again, and the canvas draws the bypass that analysis reports")
{
	ModelFixture fixture(MakeDocument());
	// The scripting runtime's answer for the Delay while it is disabled: its flow continues at Completed.
	fixture.Semantics.InjectPassThrough(2, "Completed");
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 1);
	CHECK_FALSE(fixture.Node(2).HasFlag(VisualScriptNodeFlags::PassThroughExit));

	// Disabled: the commit's frame draws the node disabled with the last analysis, made while it was enabled (no bypass); the
	// next frame analyzes the new program, which has one.
	VisualScriptDocument document = fixture.Model.GetDisplayDocument();
	document.Graph.Nodes[1].Enabled = VisualScriptNodeEnabled::Disabled;
	fixture.Commit(document);
	CHECK(fixture.Node(2).HasFlag(VisualScriptNodeFlags::Disabled));
	CHECK_FALSE(fixture.Node(2).HasFlag(VisualScriptNodeFlags::PassThroughExit));
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 2);
	{
		VisualScriptCanvasModel const& canvas = fixture.Model.GetCanvas();
		VisualScriptNodeVisual const& disabled = fixture.Node(2);
		REQUIRE(disabled.HasFlag(VisualScriptNodeFlags::PassThroughExit));
		std::span<VisualScriptPinVisual const> const pins = canvas.GetPins(disabled);
		REQUIRE(disabled.PassThroughPin < pins.size());
		CHECK(canvas.GetText(pins[disabled.PassThroughPin].Name) == "Completed");
	}

	// Development only runs in the editor: analyzed again, without a bypass.
	document.Graph.Nodes[1].Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
	fixture.Commit(document);
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 3);
	CHECK(fixture.Node(2).HasFlag(VisualScriptNodeFlags::DevelopmentOnly));
	CHECK_FALSE(fixture.Node(2).HasFlag(VisualScriptNodeFlags::Disabled));
	CHECK_FALSE(fixture.Node(2).HasFlag(VisualScriptNodeFlags::PassThroughExit));

	// Disabled again, then enabled: the bypass comes back with the analysis and goes with the commit that enables the node,
	// before the analysis that follows.
	document.Graph.Nodes[1].Enabled = VisualScriptNodeEnabled::Disabled;
	fixture.Commit(document);
	fixture.Frame();
	CHECK(fixture.Node(2).HasFlag(VisualScriptNodeFlags::PassThroughExit));
	document.Graph.Nodes[1].Enabled = VisualScriptNodeEnabled::Enabled;
	fixture.Commit(document);
	CHECK_FALSE(fixture.Node(2).HasFlag(VisualScriptNodeFlags::PassThroughExit));
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 5);
	CHECK_FALSE(fixture.Node(2).HasFlag(VisualScriptNodeFlags::PassThroughExit));
}

TEST_CASE("VisualScriptEditor: while a widget is used, its preview draws and the analysis runs at most every 0.25 s")
{
	ModelFixture fixture(MakeDocument());
	fixture.Frame();
	fixture.Frame(false, 1.0);
	CHECK(fixture.Model.GetAnalysisCount() == 1);

	VisualScriptDocument preview = fixture.Model.GetDisplayDocument();
	preview.Graph.Nodes[1].Inputs["Duration"] = 7;
	fixture.Model.SetWidgetPreview(preview);
	VisualScriptSyncResult const firstEdit = fixture.Frame(true, 0.05);
	CHECK(firstEdit.DocumentChanged);
	CHECK(fixture.Model.GetAnalysisCount() == 2);
	auto const chip = [&fixture]()
	{
		VisualScriptCanvasModel const& canvas = fixture.Model.GetCanvas();
		std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
		VisualScriptNodeVisual const& delay = nodes[*canvas.FindNode(2)];
		return std::string(canvas.GetText(
				   canvas.GetPins()[*canvas.FindPin(*canvas.FindNode(2), "Duration", VisualScriptPinDirection::In)].ChipText)) +
		       (delay.HasFlag(VisualScriptNodeFlags::Analyzed) ? "" : "?");
	};
	CHECK(chip() == "7");

	// Further edits within 0.25 s draw at once (the analysis' pins still fit) and analyze once the interval is over.
	for (int edit = 8; edit <= 11; edit++)
	{
		preview.Graph.Nodes[1].Inputs["Duration"] = edit;
		fixture.Model.SetWidgetPreview(preview);
		fixture.Frame(true, 0.05);
		CHECK(chip() == std::to_string(edit));
	}
	CHECK(fixture.Model.GetAnalysisCount() == 2);
	fixture.Frame(true, 0.06);
	CHECK(fixture.Model.GetAnalysisCount() == 3);
	// The edits were drawn, not stored.
	CHECK(fixture.GetAsset()->GetDocument().Graph.Nodes[1].Inputs.at("Duration") == 1);

	// An outside change drops the preview.
	VisualScriptDocument outside = fixture.Model.GetDisplayDocument();
	outside.Graph.Nodes[0].Position = glm::vec2(5.0f, 5.0f);
	fixture.GetAsset()->SetDocument(outside);
	fixture.Frame(true, 0.05);
	CHECK(chip() == "1");
	// Without a widget, the stored document is analyzed again at once.
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 4);

	// An outside change that only writes a literal otherwise (1 as 1.0) is still an outside change, though operator== takes
	// the documents as equal: the runtime reads another program. The preview is dropped and the node flashes.
	preview = fixture.Model.GetDisplayDocument();
	preview.Graph.Nodes[1].Inputs["Duration"] = 7;
	fixture.Model.SetWidgetPreview(preview);
	fixture.Frame(true, 0.05);
	CHECK(chip() == "7");
	VisualScriptDocument rewritten = *fixture.Model.GetStoredDocument();
	rewritten.Graph.Nodes[1].Inputs["Duration"] = 1.0;
	REQUIRE(rewritten == *fixture.Model.GetStoredDocument());
	REQUIRE_FALSE(HaveSameVisualScriptProgram(rewritten, *fixture.Model.GetStoredDocument()));
	fixture.GetAsset()->SetDocument(rewritten);
	fixture.Frame(true, 0.05);
	CHECK(fixture.GetChanged() == std::vector<uint32_t>{2});
	CHECK(chip() == "1");
	CHECK(fixture.Model.GetDisplayDocument().Graph.Nodes[1].Inputs.at("Duration").is_number_float());
}

TEST_CASE("VisualScriptEditor: each Sync's measurer lays out until the next Sync, also when it has the last one's Signature")
{
	// Measurers the panel makes each frame, which may capture what lives only for that frame: both measure as the fixture's
	// measurer does, with its Signature, and count their calls.
	uint32_t firstCalls = 0;
	uint32_t secondCalls = 0;
	auto const counting = [](uint32_t& calls)
	{
		VisualScriptTextMeasurer measurer = Testing::FixedWidthMeasurer(0.5f);
		measurer.Measure = [&calls](std::string_view text, float sizeCu)
		{
			calls++;
			return static_cast<float>(text.size()) * 0.5f * sizeCu;
		};
		return measurer;
	};
	InteractionFixture fixture(MakeCanvasDocument());
	fixture.Measure = counting(firstCalls);
	fixture.Frame();
	REQUIRE(fixture.Model.GetCanvas().GetNodes().size() == 7);

	// The next frame's measurer, of the same Signature: nothing is measured again for it, but what is measured from now on
	// is measured with it (the copy the model keeps is this frame's; Update lays out with that copy too).
	fixture.Measure = counting(secondCalls);
	firstCalls = 0;
	fixture.Frame();
	CHECK(secondCalls == 0);
	VisualScriptDocument document = fixture.Model.GetDisplayDocument();
	document.Graph.Nodes[1].Inputs["Duration"] = 4;
	fixture.Commit(document);
	CHECK(secondCalls > 0);
	fixture.Frame();
	CHECK(firstCalls == 0);
}

TEST_CASE("VisualScriptEditor: a new version of the asset registry lays the canvas out again only while a chip shows an asset's name")
{
	ModelFixture fixture(MakeDocument());
	fixture.Frame();
	fixture.Frame();
	uint64_t const revision = fixture.Model.GetCanvasRevision();

	// The registry changes (a thumbnail's model loaded and unloaded, a refresh): no chip names an asset, so nothing is synced.
	AssetHandle const extra = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(VisualScriptDocument()), "Extra");
	AssetManager::RemoveMemoryAsset(extra);
	VisualScriptSyncResult const idle = fixture.Frame();
	CHECK_FALSE(idle.GeometryChanged);
	CHECK(fixture.Model.GetCanvasRevision() == revision);
	CHECK_FALSE(fixture.Model.GetCanvas().ShowsAssetNames());

	// A chip names an asset: a registry change lays its node out again, and the chip shows the name of now.
	AssetHandle const hero = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(VisualScriptDocument()), "Hero");
	VisualScriptDocument document = fixture.Model.GetDisplayDocument();
	document.Graph.Nodes.push_back(MakeNode(4,
	                                        "Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)",
	                                        glm::vec2(0.0f, 400.0f), Json::object({{"Prefab", hero.ToString()}})));
	fixture.Commit(document);
	fixture.Frame();
	auto const chip = [&fixture]()
	{
		VisualScriptCanvasModel const& canvas = fixture.Model.GetCanvas();
		std::optional<uint32_t> const node = canvas.FindNode(4);
		REQUIRE(node.has_value());
		std::optional<uint32_t> const pin = canvas.FindPin(*node, "Prefab", VisualScriptPinDirection::In);
		REQUIRE(pin.has_value());
		return std::string(canvas.GetText(canvas.GetPins()[*pin].ChipText));
	};
	CHECK(chip() == "Hero");
	CHECK(fixture.Model.GetCanvas().ShowsAssetNames());
	uint64_t const named = fixture.Model.GetCanvasRevision();
	AssetManager::RemoveMemoryAsset(hero);
	fixture.Frame();
	CHECK(fixture.Model.GetCanvasRevision() == named + 1);
	CHECK(chip() == "Unknown asset " + hero.ToString());
}

TEST_CASE("VisualScriptEditor: a node created in the frame of its commit takes its template's pins until the next frame's analysis")
{
	ModelFixture fixture(MakeDocument());
	fixture.Frame();
	fixture.Frame();
	// A commit that changes the program: analyzed in its frame, which then has had its analysis.
	VisualScriptDocument document = fixture.Model.GetDisplayDocument();
	document.Graph.Nodes.push_back(MakeNode(9, "Flow.Sequence", glm::vec2(600.0f, 0.0f)));
	document.Graph.Nodes[1].Inputs["Duration"] = 2;
	fixture.Commit(document);
	CHECK(fixture.Model.GetAnalysisCount() == 2);
	CHECK(fixture.Node(9).HasFlag(VisualScriptNodeFlags::Analyzed));
	document.Graph.Nodes.push_back(MakeNode(10, "Flow.Branch", glm::vec2(900.0f, 0.0f)));
	fixture.Commit(document);
	CHECK(fixture.Model.GetAnalysisCount() == 2);
	CHECK(fixture.Node(10).HasFlag(VisualScriptNodeFlags::Template));
	CHECK(fixture.Model.GetCanvas().GetPins(fixture.Node(10)).size() == 4);
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 3);
	CHECK(fixture.Node(10).HasFlag(VisualScriptNodeFlags::Analyzed));
}

TEST_CASE("VisualScriptEditor: the pin type lookup gives the canvas's pins: analyzed, from the template, inferred, or none")
{
	ModelFixture fixture(MakeDocument());
	fixture.Frame();
	VisualScriptPinTypeLookup const lookup = fixture.Model.MakePinTypeLookup();
	CHECK(lookup(2, "Duration", VisualScriptPinDirection::In) == std::optional<std::string>("float"));
	CHECK(lookup(2, "Completed", VisualScriptPinDirection::Out) == std::optional<std::string>("exec"));
	CHECK_FALSE(lookup(2, "Completed", VisualScriptPinDirection::In).has_value());
	CHECK_FALSE(lookup(2, "Missing", VisualScriptPinDirection::In).has_value());
	CHECK_FALSE(lookup(99, "Then", VisualScriptPinDirection::Out).has_value());

	// Without the runtime, pins the document names are of no known type.
	fixture.Semantics.SetAvailable(false);
	VisualScriptDocument document = fixture.Model.GetDisplayDocument();
	document.Graph.Nodes.push_back(MakeNode(11, "Flow.Teleport", glm::vec2(0.0f, 400.0f), Json::object({{"Where", 1}})));
	fixture.Commit(document);
	VisualScriptPinTypeLookup const degraded = fixture.Model.MakePinTypeLookup();
	CHECK(degraded(11, "Where", VisualScriptPinDirection::In) == std::optional<std::string>("*"));
	CHECK_FALSE(degraded(11, "Elsewhere", VisualScriptPinDirection::In).has_value());
}

TEST_CASE(
	"VisualScriptEditor: a window's document is read-only for a memory asset and a file without write permission, and absent when missing, broken or deleted")
{
	SUBCASE("a memory asset")
	{
		Testing::AssetManagerScope assets;
		Testing::TemporaryDirectory temporary;
		VisualScriptViewSettings settings(temporary.GetPath() / "Views.json");
		AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeDocument()), "Generated");
		VisualScriptEditorModel model(graph, settings);
		Testing::FixtureVisualScriptSemantics semantics;
		VisualScriptSyncResult const result =
			model.Sync(semantics, Testing::FixedWidthMeasurer(0.5f), VisualScriptThemeAnchors(), false, 1.0);
		CHECK(result.State == VisualScriptDocumentState::ReadOnly);
		CHECK(model.IsReadOnly());
		// Viewing still works.
		CHECK(model.GetCanvas().GetNodes().size() == 3);
	}
	SUBCASE("a file without write permission, and after it is made writable")
	{
		ModelFixture fixture(MakeDocument());
		using Perms = std::filesystem::perms;
		Perms const write = Perms::owner_write | Perms::group_write | Perms::others_write;
		std::filesystem::permissions(fixture.Project.File, write, std::filesystem::perm_options::remove);
		struct Restore
		{
			std::filesystem::path File;
			~Restore()
			{
				std::error_code error;
				std::filesystem::permissions(File, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, error);
			}
		} const restore{fixture.Project.File};
		CHECK(fixture.Frame().State == VisualScriptDocumentState::ReadOnlyFile);
		CHECK(fixture.Model.IsReadOnly());
		// Checked again only when asked (focus, a failed write, Retry) or when the version changes.
		std::filesystem::permissions(fixture.Project.File, Perms::owner_write, std::filesystem::perm_options::add);
		CHECK(fixture.Frame().State == VisualScriptDocumentState::ReadOnlyFile);
		fixture.Model.CheckWritable();
		CHECK(fixture.Frame().State == VisualScriptDocumentState::Editable);
	}
	SUBCASE("a missing file, a file that does not load, a deleted asset")
	{
		ModelFixture fixture(MakeDocument());
		fixture.Frame();
		REQUIRE(FileSystem::WriteTextFile(fixture.Project.File, "{ not a graph").IsOk());
		REQUIRE(AssetManager::Refresh().IsOk());
		CHECK(fixture.Frame().State == VisualScriptDocumentState::Unloadable);
		// The last document stays for when the file loads again.
		CHECK(fixture.Model.GetCanvas().GetNodes().size() == 3);
		std::filesystem::remove(fixture.Project.File);
		REQUIRE(AssetManager::Refresh().IsOk());
		CHECK(fixture.Frame().State == VisualScriptDocumentState::Missing);
		REQUIRE(VisualScriptSerializer::SaveToFile(MakeDocument(), fixture.Project.File).IsOk());
		REQUIRE(AssetManager::Refresh().IsOk());
		CHECK(fixture.Frame().State == VisualScriptDocumentState::Editable);
		REQUIRE(AssetManager::DeleteAsset(fixture.Project.Graph).IsOk());
		CHECK(fixture.Frame().State == VisualScriptDocumentState::Gone);
	}
}

TEST_CASE("VisualScriptEditor: a window showing a function deleted outside it shows the event graph")
{
	VisualScriptDocument document = MakeDocument();
	VisualScriptFunction function;
	function.ID = 40;
	function.Name = "Helper";
	function.Graph.Nodes = {MakeNode(41, "Function.Entry", glm::vec2(0.0f))};
	document.Functions = {function};
	ModelFixture fixture(document);
	fixture.Settings.SetProject(fixture.Project.Project->GetFilePath());
	fixture.Settings.SetShown(fixture.Project.Graph, 40);
	VisualScriptEditorModel model(fixture.Project.Graph, fixture.Settings);
	model.Sync(fixture.Semantics, fixture.Measure, fixture.Theme, false, 1.0);
	CHECK(model.GetFunction() == 40);
	CHECK(model.GetCanvas().GetNodes().size() == 1);

	VisualScriptDocument without = model.GetDisplayDocument();
	without.Functions.clear();
	fixture.GetAsset()->SetDocument(without);
	model.Sync(fixture.Semantics, fixture.Measure, fixture.Theme, false, 2.0);
	CHECK(model.GetFunction() == 0);
	CHECK(model.GetCanvas().GetNodes().size() == 3);
}

TEST_CASE("VisualScriptEditor: idle frames of a 10,000-node graph change nothing and reserve no memory")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	Testing::GraphProject const project = Testing::MakeGraphProject(temporary, Testing::MakeGridGraph(100, 100, 4, 50));
	VisualScriptViewSettings settings(temporary.GetPath() / "Views.json");
	LibraryOnlySemantics semantics;
	VisualScriptTextMeasurer const measure = Testing::FixedWidthMeasurer(0.5f);
	VisualScriptEditorModel model(project.Graph, settings);
	VisualScriptSyncResult const first = model.Sync(semantics, measure, VisualScriptThemeAnchors(), false, 0.0);
	REQUIRE(first.GeometryChanged);
	REQUIRE(model.GetCanvas().GetNodes().size() == 10000);
	CHECK(model.GetCanvas().GetMeasuredNodeCount() == 10000);
	CHECK(model.GetCanvas().GetNodes()[0].HasFlag(VisualScriptNodeFlags::Template));
	CHECK(model.GetAnalysisCount() == 1);

	// The interaction's frames too: a hovered canvas with the mouse resting over the graph.
	VisualScriptCanvasInput input;
	input.CanvasSize = glm::vec2(1600.0f, 900.0f);
	input.Mouse = glm::vec2(800.0f, 450.0f);
	input.Hovered = true;
	input.Focused = true;
	CHECK(model.Update(input).empty());
	size_t const capacity = model.GetCapacityBytes();
	for (int frame = 1; frame <= 10; frame++)
	{
		VisualScriptSyncResult const idle = model.Sync(semantics, measure, VisualScriptThemeAnchors(), false, frame / 60.0);
		CHECK_FALSE(idle.DocumentChanged);
		CHECK_FALSE(idle.AnalysisChanged);
		CHECK_FALSE(idle.GeometryChanged);
		input.Time = frame / 60.0;
		CHECK(model.Update(input).empty());
		CHECK(model.GetCapacityBytes() == capacity);
	}
	CHECK(model.GetHover().Kind != VisualScriptHitKind::None);
	// The failed analysis is not asked for again.
	CHECK(model.GetAnalysisCount() == 1);
}

TEST_CASE("VisualScriptEditor: a hover resting on a wire with long pin names allocates nothing from frame to frame")
{
	// Pin names longer than a string's small buffer: a custom event's parameter into a Delay.
	VisualScriptDocument document;
	VisualScriptNode received = MakeNode(1, "Event.Custom", glm::vec2(0.0f, 0.0f));
	received.Settings =
		Json::object({{"Name", "Hit"}, {"Parameters", Json::array({Json::object({{"Name", "DamageAmountReceived"}, {"Type", "float"}})})}});
	document.Graph.Nodes = {received, MakeNode(2, "Flow.Delay", glm::vec2(600.0f, 200.0f))};
	VisualScriptLink const link = MakeLink(1, "DamageAmountReceived", 2, "Duration");
	document.Graph.Links = {link};
	InteractionFixture fixture(document);
	fixture.MoveTo(Testing::WirePoint(fixture.Model, link));
	VisualScriptHit const& hover = fixture.Model.GetHover();
	REQUIRE(hover.Kind == VisualScriptHitKind::Link);
	REQUIRE(hover.Link.has_value());
	CHECK(*hover.Link == link);
	char const* const name = hover.Link->From.Pin.data();
	size_t const capacity = fixture.Model.GetCapacityBytes();
	for (int frame = 0; frame < 10; frame++)
	{
		fixture.Frame();
		REQUIRE(hover.Link.has_value());
		CHECK(*hover.Link == link);
		// The same string, its name assigned in place.
		CHECK(hover.Link->From.Pin.data() == name);
		CHECK(fixture.Model.GetCapacityBytes() == capacity);
	}
}

TEST_CASE("VisualScriptEditor: a click selects; Shift adds, Ctrl toggles, and a click on empty canvas clears unless Shift or Ctrl is held")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	CHECK(fixture.Click(Testing::CanvasPoint(model, 1)).empty());
	CHECK(fixture.Selection() == std::vector<uint32_t>{1});
	fixture.Input.Shift = true;
	CHECK(fixture.Click(Testing::CanvasPoint(model, 2)).empty());
	fixture.Input.Shift = false;
	// The last element clicked is the primary one.
	CHECK(fixture.Selection() == std::vector<uint32_t>{2, 1});
	fixture.Input.Ctrl = true;
	fixture.Click(Testing::CanvasPoint(model, 1));
	CHECK(fixture.Selection() == std::vector<uint32_t>{2});
	fixture.Click(Testing::CanvasPoint(model, 3));
	CHECK(fixture.Selection() == std::vector<uint32_t>{3, 2});
	// A comment's title bar selects it as a node is selected.
	fixture.Click(Testing::CommentPoint(model, 20, glm::vec2(0.5f, 0.05f)));
	CHECK(AsSet(model.GetSelection()) == std::set<uint32_t>{2, 3, 20});
	CHECK(model.GetHover().Kind == VisualScriptHitKind::CommentTitle);
	fixture.Click(fixture.Screen(EmptyCanvasPoint));
	CHECK(model.GetSelection().size() == 3);
	fixture.Input.Ctrl = false;
	fixture.Input.Shift = true;
	fixture.Click(fixture.Screen(EmptyCanvasPoint));
	CHECK(model.GetSelection().size() == 3);
	fixture.Input.Shift = false;

	// A press on one of several selected nodes keeps them all (a drag would move them); the click keeps only it.
	fixture.MoveTo(Testing::CanvasPoint(model, 2));
	fixture.Press();
	CHECK(model.GetSelection().size() == 3);
	CHECK(model.GetSelection().front() == 2);
	fixture.Release();
	CHECK(fixture.Selection() == std::vector<uint32_t>{2});
	{
		VisualScriptEditorState const state = model.GetState();
		CHECK(state.SelectedNodes == std::vector<uint32_t>{2});
		CHECK(state.SelectedComments.empty());
		CHECK(state.Graph == fixture.Project.Graph);
		CHECK(state.Path == "Scripts/Graph.sgraph");
		CHECK(state.Analyzed);
	}
	// The selection is drawn and hit by index.
	std::optional<uint32_t> const delay = model.GetCanvas().FindNode(2);
	REQUIRE(delay.has_value());
	CHECK(model.GetNodeSelection()[*delay] != 0);
	CHECK(model.GetNodeRaise()[*delay] > 0);
	CHECK(fixture.Click(fixture.Screen(EmptyCanvasPoint)).empty());
	CHECK(model.GetSelection().empty());
	CHECK(model.GetNodeSelection()[*delay] == 0);
}

TEST_CASE("VisualScriptEditor: a marquee selects the nodes it touches and the comments it holds whole, replacing, adding or removing")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	fixture.Click(Testing::CanvasPoint(model, 7));
	// Over part of Delay and Branch and of the comment: the nodes, not the comment. Previewed, applied at the release.
	fixture.Drag(fixture.Screen({350.0f, 250.0f}), fixture.Screen({900.0f, 50.0f}), 0, 4, false);
	{
		VisualScriptGestureView const gesture = model.GetGesture();
		CHECK(gesture.Kind == VisualScriptGesture::Marquee);
		REQUIRE(gesture.Marquee.has_value());
		CHECK(std::vector<uint32_t>(gesture.MarqueeElements.begin(), gesture.MarqueeElements.end()) == std::vector<uint32_t>{2, 3});
		CHECK(fixture.Selection() == std::vector<uint32_t>{7});
	}
	CHECK(fixture.Release().empty());
	CHECK(AsSet(model.GetSelection()) == std::set<uint32_t>{2, 3});
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Idle);

	// Started in a comment's body, it never selects that comment.
	fixture.Drag(fixture.Screen(CommentBodyPoint), fixture.Screen({-150.0f, -150.0f}));
	CHECK(fixture.Selection() == std::vector<uint32_t>{1});
	// Shift adds; holding a comment whole selects it.
	fixture.Input.Shift = true;
	fixture.Drag(fixture.Screen({-150.0f, -150.0f}), fixture.Screen({700.0f, 200.0f}));
	fixture.Input.Shift = false;
	CHECK(AsSet(model.GetSelection()) == std::set<uint32_t>{1, 2, 20});
	// Ctrl removes.
	fixture.Input.Ctrl = true;
	fixture.Drag(fixture.Screen({350.0f, 250.0f}), fixture.Screen({900.0f, 50.0f}));
	fixture.Input.Ctrl = false;
	CHECK(AsSet(model.GetSelection()) == std::set<uint32_t>{1, 20});
}

TEST_CASE("VisualScriptEditor: a drag past 4 px gives one move intent at its release and none before; a smaller one is a click")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	glm::vec2 const start = Testing::CanvasPoint(model, 2);
	// 3 pixels: a click.
	fixture.MoveTo(start);
	fixture.Press();
	CHECK(fixture.MoveTo(start + glm::vec2(3.0f, 0.0f)).empty());
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Idle);
	CHECK(fixture.Release().empty());
	CHECK(fixture.Selection() == std::vector<uint32_t>{2});

	// Past 4 pixels the selection moves: previewed, and one intent at the release.
	glm::vec2 const end = start + glm::vec2(60.0f, 37.0f);
	fixture.MoveTo(start);
	fixture.Press();
	CHECK(fixture.MoveTo(start + glm::vec2(5.0f, 0.0f)).empty());
	CHECK(model.GetGesture().Kind == VisualScriptGesture::MovingElements);
	CHECK(fixture.MoveTo(end).empty());
	glm::vec2 const expected = fixture.SnappedOffset(glm::vec2(400.0f, 0.0f), start, end);
	{
		VisualScriptGestureView const gesture = model.GetGesture();
		CHECK(std::vector<uint32_t>(gesture.MovedElements.begin(), gesture.MovedElements.end()) == std::vector<uint32_t>{2});
		CHECK(gesture.MoveOffset == expected);
		VisualScriptNodePreview const preview = model.GetNodePreview();
		CHECK(preview.IsMoved(*model.GetCanvas().FindNode(2)));
		CHECK_FALSE(preview.IsMoved(*model.GetCanvas().FindNode(1)));
	}
	VisualScriptMoveIntent const move = Single<VisualScriptMoveIntent>(fixture.Release());
	CHECK(move.Elements == std::vector<uint32_t>{2});
	CHECK(move.Offset == expected);
	CHECK(move.Snap);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Idle);
	CHECK(model.GetNodePreview().Moved.empty());

	// A drag that comes back to where it started moves nothing.
	fixture.MoveTo(start);
	fixture.Press();
	fixture.MoveTo(start + glm::vec2(40.0f, 0.0f));
	fixture.MoveTo(start);
	CHECK(fixture.Release().empty());

	// Dragging one of several selected nodes moves them all, the one dragged primary.
	fixture.Input.Shift = true;
	fixture.Click(Testing::CanvasPoint(model, 3));
	fixture.Input.Shift = false;
	CHECK(fixture.Selection() == std::vector<uint32_t>{3, 2});
	VisualScriptMoveIntent const both = Single<VisualScriptMoveIntent>(fixture.Drag(start, end));
	CHECK(both.Elements == std::vector<uint32_t>{2, 3});
	CHECK(both.Offset == expected);
	CHECK(fixture.Selection() == std::vector<uint32_t>{2, 3});
}

TEST_CASE(
	"VisualScriptEditor: dragging a comment's title bar moves what it holds; alone with Move Comment Contents off; Alt inverts the toggle")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	auto const dragComment = [&fixture, &model]()
	{
		fixture.Click(fixture.Screen(EmptyCanvasPoint));
		glm::vec2 const title = Testing::CommentPoint(model, 20, glm::vec2(0.5f, 0.05f));
		return Single<VisualScriptMoveIntent>(fixture.Drag(title, title + glm::vec2(100.0f, 50.0f)));
	};
	CHECK(fixture.Settings.IsMoveCommentContents());
	CHECK(AsSet(dragComment().Elements) == std::set<uint32_t>{1, 2, 20});
	fixture.Input.Alt = true;
	VisualScriptMoveIntent const alone = dragComment();
	CHECK(alone.Elements == std::vector<uint32_t>{20});
	// Alt also turns snapping off for the drag.
	CHECK_FALSE(alone.Snap);
	fixture.Input.Alt = false;

	fixture.Settings.SetMoveCommentContents(false);
	CHECK(dragComment().Elements == std::vector<uint32_t>{20});
	fixture.Input.Alt = true;
	CHECK(AsSet(dragComment().Elements) == std::set<uint32_t>{1, 2, 20});
}

TEST_CASE("VisualScriptEditor: moves snap the primary node to the grid while Snap to Grid is on, and Alt inverts it during the drag")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	glm::vec2 const start = Testing::CanvasPoint(model, 2);
	glm::vec2 const end = start + glm::vec2(37.0f, 21.0f);
	glm::vec2 const snapped = fixture.SnappedOffset(glm::vec2(400.0f, 0.0f), start, end);
	glm::vec2 const exact = fixture.Canvas(end) - fixture.Canvas(start);
	REQUIRE(snapped != exact);

	VisualScriptMoveIntent move = Single<VisualScriptMoveIntent>(fixture.Drag(start, end));
	CHECK(move.Offset == snapped);
	CHECK(move.Snap);
	glm::vec2 const placed = glm::vec2(400.0f, 0.0f) + move.Offset;
	CHECK(std::fmod(placed.x, 16.0f) == 0.0f);
	CHECK(std::fmod(placed.y, 16.0f) == 0.0f);
	// Alt pressed during the drag: no snapping from then on.
	fixture.MoveTo(start);
	fixture.Press();
	fixture.MoveTo(end);
	CHECK(model.GetGesture().MoveOffset == snapped);
	fixture.Input.Alt = true;
	fixture.MoveTo(end);
	CHECK(model.GetGesture().MoveOffset == exact);
	move = Single<VisualScriptMoveIntent>(fixture.Release());
	CHECK(move.Offset == exact);
	CHECK_FALSE(move.Snap);
	fixture.Input.Alt = false;

	fixture.Settings.SetSnapToGrid(false);
	move = Single<VisualScriptMoveIntent>(fixture.Drag(start, end));
	CHECK(move.Offset == exact);
	CHECK_FALSE(move.Snap);
	fixture.Input.Alt = true;
	move = Single<VisualScriptMoveIntent>(fixture.Drag(start, end));
	CHECK(move.Offset == snapped);
	CHECK(move.Snap);
}

TEST_CASE(
	"VisualScriptEditor: a wire dropped on a compatible input connects, on an incompatible one nothing, and on empty canvas opens the palette with its pin")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	VisualScriptCanvasModel const& canvas = model.GetCanvas();
	auto const pinIndex = [&canvas](uint32_t node, std::string_view pin, VisualScriptPinDirection direction)
	{
		std::optional<uint32_t> const index = canvas.FindPin(*canvas.FindNode(node), pin, direction);
		REQUIRE(index.has_value());
		return *index;
	};
	auto const pinClass = [&model](uint32_t pin)
	{
		std::span<uint8_t const> const classes = model.GetGesture().PinClasses;
		REQUIRE(pin < classes.size());
		return static_cast<VisualScriptPinClass>(classes[pin]);
	};
	glm::vec2 const deltaTime = Testing::CanvasPoint(model, 4, "DeltaTime", VisualScriptPinDirection::Out);
	glm::vec2 const duration = Testing::CanvasPoint(model, 2, "Duration");

	VisualScriptConnectIntent connect = Single<VisualScriptConnectIntent>(fixture.Drag(deltaTime, duration));
	CHECK(connect.Link == MakeLink(4, "DeltaTime", 2, "Duration"));
	CHECK_FALSE(connect.InsertConvert);

	// Over a pin it cannot connect to, the ghost is invalid, and the release does nothing.
	fixture.Drag(deltaTime, Testing::CanvasPoint(model, 3, "Condition"), 0, 4, false);
	{
		VisualScriptGestureView const gesture = model.GetGesture();
		CHECK(gesture.Kind == VisualScriptGesture::Wiring);
		CHECK(gesture.GhostInvalid);
		REQUIRE(gesture.GhostWire.has_value());
		// The not-allowed tooltip names both pins: the one under the mouse and the wire's own.
		CHECK(model.GetHover().Kind == VisualScriptHitKind::Pin);
		CHECK(model.GetHover().Element == 3);
		REQUIRE(gesture.WireSource != nullptr);
		CHECK(*gesture.WireSource == VisualScriptPinReference{4, "DeltaTime"});
		CHECK(gesture.WireDirection == VisualScriptPinDirection::Out);
		CHECK(pinClass(pinIndex(3, "Condition", VisualScriptPinDirection::In)) == VisualScriptPinClass::Incompatible);
		CHECK(pinClass(pinIndex(2, "Duration", VisualScriptPinDirection::In)) == VisualScriptPinClass::Compatible);
		// Pins of the wire's own node and of its direction never connect.
		CHECK(pinClass(pinIndex(4, "Then", VisualScriptPinDirection::Out)) == VisualScriptPinClass::Incompatible);
		CHECK(pinClass(pinIndex(6, "Index", VisualScriptPinDirection::Out)) == VisualScriptPinClass::Incompatible);
	}
	CHECK(fixture.Release().empty());

	// Released on empty canvas: the palette in context mode, at the release.
	VisualScriptPaletteIntent const palette = Single<VisualScriptPaletteIntent>(fixture.Drag(deltaTime, fixture.Screen(EmptyCanvasPoint)));
	REQUIRE(palette.From.has_value());
	CHECK(*palette.From == VisualScriptPinReference{4, "DeltaTime"});
	CHECK(palette.Direction == VisualScriptPinDirection::Out);
	CHECK(glm::length(palette.Position - EmptyCanvasPoint) < 0.01f);
	CHECK(palette.Query.empty());

	// From an input to an output: the link still runs from the output.
	fixture.Drag(duration, deltaTime, 0, 4, false);
	REQUIRE(model.GetGesture().WireSource != nullptr);
	CHECK(*model.GetGesture().WireSource == VisualScriptPinReference{2, "Duration"});
	CHECK(model.GetGesture().WireDirection == VisualScriptPinDirection::In);
	CHECK(model.GetGesture().PinClasses.size() == canvas.GetPins().size());
	fixture.Release();
	CHECK(model.GetGesture().WireSource == nullptr);
	CHECK(model.GetGesture().PinClasses.empty());
	connect = Single<VisualScriptConnectIntent>(fixture.Drag(duration, deltaTime));
	CHECK(connect.Link == MakeLink(4, "DeltaTime", 2, "Duration"));
	// Within 16 px of a compatible pin the wire snaps to it.
	fixture.Drag(deltaTime, duration + glm::vec2(-10.0f, 9.0f), 0, 4, false);
	CHECK(pinClass(pinIndex(2, "Duration", VisualScriptPinDirection::In)) == VisualScriptPinClass::Snapped);
	CHECK_FALSE(model.GetGesture().GhostInvalid);
	connect = Single<VisualScriptConnectIntent>(fixture.Release());
	CHECK(connect.Link == MakeLink(4, "DeltaTime", 2, "Duration"));

	// An int reaches a float by an implicit conversion; a float reaches a uint through a Convert node.
	glm::vec2 const index = Testing::CanvasPoint(model, 6, "Index", VisualScriptPinDirection::Out);
	fixture.Drag(index, duration + glm::vec2(30.0f, 30.0f), 0, 4, false);
	CHECK(pinClass(pinIndex(2, "Duration", VisualScriptPinDirection::In)) == VisualScriptPinClass::Converting);
	fixture.Release();
	connect = Single<VisualScriptConnectIntent>(fixture.Drag(index, duration));
	CHECK(connect.Link == MakeLink(6, "Index", 2, "Duration"));
	CHECK_FALSE(connect.InsertConvert);
	glm::vec2 const layerMask = Testing::CanvasPoint(model, 7, "LayerMask");
	fixture.Drag(deltaTime, layerMask, 0, 4, false);
	CHECK(pinClass(pinIndex(7, "LayerMask", VisualScriptPinDirection::In)) == VisualScriptPinClass::Incompatible);
	CHECK_FALSE(model.GetGesture().GhostInvalid);
	connect = Single<VisualScriptConnectIntent>(fixture.Release());
	CHECK(connect.Link == MakeLink(4, "DeltaTime", 7, "LayerMask"));
	CHECK(connect.InsertConvert);
	CHECK(connect.ConvertTo == "uint");

	// Over a node's body: its first pin that connects (a flow's: Execute).
	connect = Single<VisualScriptConnectIntent>(fixture.Drag(Testing::CanvasPoint(model, 4, "Then", VisualScriptPinDirection::Out),
	                                                         Testing::CanvasPoint(model, 5, glm::vec2(0.9f, 0.85f))));
	CHECK(connect.Link == MakeLink(4, "Then", 5, "Execute"));
	// A wire released on its own node does nothing.
	CHECK(fixture.Drag(deltaTime, Testing::CanvasPoint(model, 4, glm::vec2(0.5f, 0.2f))).empty());
}

TEST_CASE(
	"VisualScriptEditor: Ctrl+drag picks up a pin's links and moves them to the pin it is dropped on, or removes them on empty canvas")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	glm::vec2 const execute = Testing::CanvasPoint(model, 2, "Execute");
	fixture.Input.Ctrl = true;
	fixture.Drag(execute, Testing::CanvasPoint(model, 3, "Execute"), 0, 4, false);
	{
		VisualScriptGestureView const gesture = model.GetGesture();
		CHECK(gesture.Kind == VisualScriptGesture::Relinking);
		CHECK(std::vector<VisualScriptLink>(gesture.PickedLinks.begin(), gesture.PickedLinks.end()) ==
		      std::vector<VisualScriptLink>{MakeLink(1, "Then", 2, "Execute")});
		CHECK(gesture.PickedWires.size() == 1);
	}
	VisualScriptRelinkIntent relink = Single<VisualScriptRelinkIntent>(fixture.Release());
	CHECK(relink.Remove == std::vector<VisualScriptLink>{MakeLink(1, "Then", 2, "Execute")});
	CHECK(relink.Add == std::vector<VisualScriptLink>{MakeLink(1, "Then", 3, "Execute")});

	relink = Single<VisualScriptRelinkIntent>(fixture.Drag(execute, fixture.Screen(EmptyCanvasPoint)));
	CHECK(relink.Remove == std::vector<VisualScriptLink>{MakeLink(1, "Then", 2, "Execute")});
	CHECK(relink.Add.empty());

	// An output's links move to another output.
	relink = Single<VisualScriptRelinkIntent>(fixture.Drag(Testing::CanvasPoint(model, 2, "Completed", VisualScriptPinDirection::Out),
	                                                       Testing::CanvasPoint(model, 4, "Then", VisualScriptPinDirection::Out)));
	CHECK(relink.Remove == std::vector<VisualScriptLink>{MakeLink(2, "Completed", 3, "Execute")});
	CHECK(relink.Add == std::vector<VisualScriptLink>{MakeLink(4, "Then", 3, "Execute")});
	// Dropped back on the pin they came from: nothing.
	fixture.Drag(execute, execute + glm::vec2(0.0f, 40.0f), 0, 4, false);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Relinking);
	fixture.MoveTo(execute);
	CHECK(fixture.Release().empty());
	fixture.Input.Ctrl = false;

	// Without Ctrl, a drag from a linked pin draws a new wire.
	fixture.Drag(execute, fixture.Screen(EmptyCanvasPoint), 0, 4, false);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Wiring);
	fixture.Release();
}

TEST_CASE("VisualScriptEditor: Ctrl+right drag cuts the wires its line crosses; a right drag without Ctrl pans")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	glm::vec2 const top = fixture.Screen({275.0f, -20.0f});
	glm::vec2 const bottom = fixture.Screen({275.0f, 110.0f});
	fixture.Input.Ctrl = true;
	fixture.Drag(top, bottom, 1, 4, false);
	{
		VisualScriptGestureView const gesture = model.GetGesture();
		CHECK(gesture.Kind == VisualScriptGesture::Cutting);
		REQUIRE(gesture.CutLine.has_value());
		CHECK(std::vector<VisualScriptLink>(gesture.CutLinks.begin(), gesture.CutLinks.end()) ==
		      std::vector<VisualScriptLink>{MakeLink(1, "Then", 2, "Execute")});
	}
	VisualScriptDisconnectIntent const cut = Single<VisualScriptDisconnectIntent>(fixture.Release(1));
	CHECK(cut.Links == std::vector<VisualScriptLink>{MakeLink(1, "Then", 2, "Execute")});
	// Crossing nothing does nothing.
	CHECK(fixture.Drag(fixture.Screen(EmptyCanvasPoint), fixture.Screen(EmptyCanvasPoint + glm::vec2(60.0f, 30.0f)), 1).empty());
	fixture.Input.Ctrl = false;

	VisualScriptViewState const before = model.GetViewState();
	CHECK(fixture.Drag(top, bottom, 1).empty());
	glm::vec2 const moved = before.Center - model.GetViewState().Center;
	CHECK(glm::length(moved - (bottom - top) / model.GetView().GetPixelsPerUnit()) < 0.01f);
}

TEST_CASE("VisualScriptEditor: nothing beyond the canvas's edges is hit: a wire or picked-up links released there change nothing")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel& model = fixture.Model;
	// Centred right of the graph: Delay's Execute lies beyond the canvas's left edge, Log Info and Branch on the canvas.
	fixture.Settings.SetReduceMotion(true);
	VisualScriptOpenRequest request;
	request.Graph = fixture.Graph;
	request.Center = glm::vec2(1400.0f, 200.0f);
	request.Zoom = 1.0f;
	model.ApplyOpenRequest(request);
	fixture.Frame();
	glm::vec2 const hidden = Testing::CanvasPoint(model, 2, "Execute");
	REQUIRE(hidden.x < 0.0f);
	glm::vec2 const then = Testing::CanvasPoint(model, 5, "Then", VisualScriptPinDirection::Out);
	REQUIRE(then.x > 0.0f);

	// A wire dragged onto that pin, which is not drawn: no hover, no snap, and the release adds nothing.
	fixture.Drag(then, hidden, 0, 4, false);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Wiring);
	CHECK(model.GetHover().Kind == VisualScriptHitKind::None);
	std::span<uint8_t const> const classes = model.GetGesture().PinClasses;
	CHECK(std::count(classes.begin(), classes.end(), static_cast<uint8_t>(VisualScriptPinClass::Snapped)) == 0);
	CHECK(fixture.Release().empty());

	// Branch's links picked up and dropped beyond the edge stay where they are.
	fixture.Input.Ctrl = true;
	glm::vec2 const execute = Testing::CanvasPoint(model, 3, "Execute");
	fixture.Drag(execute, glm::vec2(-200.0f, execute.y), 0, 4, false);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Relinking);
	CHECK(fixture.Release().empty());
	fixture.Input.Ctrl = false;

	// A move that ends beyond the edge still moves.
	glm::vec2 const branch = Testing::CanvasPoint(model, 3, glm::vec2(0.5f, 0.15f));
	CHECK(Single<VisualScriptMoveIntent>(fixture.Drag(branch, glm::vec2(-50.0f, branch.y))).Elements == std::vector<uint32_t>{3});
}

TEST_CASE("VisualScriptEditor: Alt+click breaks a pin's links, or the wire clicked")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	fixture.Input.Alt = true;
	VisualScriptDisconnectIntent disconnect =
		Single<VisualScriptDisconnectIntent>(fixture.Click(Testing::CanvasPoint(model, 2, "Execute")));
	CHECK(disconnect.Links == std::vector<VisualScriptLink>{MakeLink(1, "Then", 2, "Execute")});
	disconnect = Single<VisualScriptDisconnectIntent>(fixture.Click(Testing::WirePoint(model, MakeLink(2, "Completed", 3, "Execute"))));
	CHECK(disconnect.Links == std::vector<VisualScriptLink>{MakeLink(2, "Completed", 3, "Execute")});
	// A pin without links, and an Alt drag, break nothing.
	CHECK(fixture.Click(Testing::CanvasPoint(model, 4, "Then", VisualScriptPinDirection::Out)).empty());
	CHECK(fixture.Drag(Testing::CanvasPoint(model, 2, "Execute"), fixture.Screen(EmptyCanvasPoint)).empty());
}

TEST_CASE("VisualScriptEditor: a double-click on a wire inserts a reroute at the wire's point nearest the click, snapped")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	VisualScriptLink const link = MakeLink(1, "Then", 2, "Execute");
	glm::vec2 const point = Testing::WirePoint(model, link);
	std::vector<VisualScriptInsertRerouteIntent> const reroutes = Of<VisualScriptInsertRerouteIntent>(fixture.DoubleClick(point));
	REQUIRE(reroutes.size() == 1);
	CHECK(reroutes.front().Link == link);
	glm::vec2 const position = reroutes.front().Position;
	CHECK(std::fmod(position.x, 16.0f) == 0.0f);
	CHECK(std::fmod(position.y, 16.0f) == 0.0f);
	// Its dot's centre is on the wire, up to the snap.
	glm::vec2 const center = position + glm::vec2(VisualScriptCanvasMetrics::RerouteRadius);
	CHECK(glm::length(center - fixture.Canvas(point)) <= 12.0f);
}

TEST_CASE(
	"VisualScriptEditor: a wire released on a reroute's dot links an output into it and an input out of it; a drag from the dot moves it")
{
	VisualScriptDocument document = MakeCanvasDocument();
	document.Graph.Nodes.push_back(MakeNode(8, "Reroute", glm::vec2(592.0f, 240.0f)));
	InteractionFixture fixture(document);
	VisualScriptEditorModel const& model = fixture.Model;
	glm::vec2 const dot = Testing::CanvasPoint(model, 8);
	fixture.MoveTo(dot);
	CHECK(model.GetHover().Kind == VisualScriptHitKind::Reroute);

	VisualScriptConnectIntent connect =
		Single<VisualScriptConnectIntent>(fixture.Drag(Testing::CanvasPoint(model, 4, "DeltaTime", VisualScriptPinDirection::Out), dot));
	CHECK(connect.Link == MakeLink(4, "DeltaTime", 8, "In"));
	connect = Single<VisualScriptConnectIntent>(fixture.Drag(Testing::CanvasPoint(model, 2, "Duration"), dot));
	CHECK(connect.Link == MakeLink(8, "Out", 2, "Duration"));
	// Its pins are reached through the dot only: a drag that starts there moves the reroute, as a node moves.
	CHECK(Single<VisualScriptMoveIntent>(fixture.Drag(dot, dot + glm::vec2(64.0f, 32.0f))).Elements == std::vector<uint32_t>{8});
}

TEST_CASE(
	"VisualScriptEditor: a wire resting 0.3 s on a collapsed node expands it and snaps to a pin of its rows; a press on an anchor of several pins asks which")
{
	VisualScriptDocument document = MakeCanvasDocument();
	document.Graph.Nodes[2].Collapsed = true;
	InteractionFixture fixture(document);
	VisualScriptEditorModel& model = fixture.Model;
	glm::vec2 const then = Testing::CanvasPoint(model, 4, "Then", VisualScriptPinDirection::Out);
	glm::vec2 const branch = Testing::CanvasPoint(model, 3, glm::vec2(0.5f, 0.5f));
	fixture.Drag(then, branch, 0, 4, false);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Wiring);
	CHECK_FALSE(model.GetGesture().ExpandedNode.has_value());
	fixture.MoveTo(branch, 0.35);
	CHECK(model.GetGesture().ExpandedNode == std::optional<uint32_t>(3));
	CHECK(model.GetNodePreview().Expanded == model.GetCanvas().FindNode(3));
	// Its rows are hit and snapped to as drawn expanded.
	fixture.MoveTo(fixture.ExpandedPin(3, "Execute", VisualScriptPinDirection::In));
	CHECK(model.GetHover().Kind == VisualScriptHitKind::Pin);
	CHECK(model.GetGesture().ExpandedNode == std::optional<uint32_t>(3));
	VisualScriptConnectIntent const connect = Single<VisualScriptConnectIntent>(fixture.Release());
	CHECK(connect.Link == MakeLink(4, "Then", 3, "Execute"));
	CHECK_FALSE(model.GetNodePreview().Expanded.has_value());

	// The wire leaving it collapses it again.
	fixture.Drag(then, branch, 0, 4, false);
	fixture.MoveTo(branch, 0.35);
	CHECK(model.GetGesture().ExpandedNode.has_value());
	fixture.MoveTo(fixture.Screen(EmptyCanvasPoint));
	CHECK_FALSE(model.GetGesture().ExpandedNode.has_value());
	fixture.Release();

	// A press on its input anchor, which stands for Execute and Condition, asks which pin the wire starts from.
	std::vector<VisualScriptPinMenuIntent> const menus =
		Of<VisualScriptPinMenuIntent>(fixture.Click(Testing::CanvasPoint(model, 3, "Execute", VisualScriptPinDirection::In)));
	REQUIRE(menus.size() == 1);
	CHECK(menus.front().Node == 3);
	CHECK(menus.front().Direction == VisualScriptPinDirection::In);
	// The wire from the pin chosen (here Execute) follows the mouse with the button up, and the next press ends it.
	model.StartWire(3, "Execute", VisualScriptPinDirection::In);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Wiring);
	fixture.MoveTo(Testing::CanvasPoint(model, 5, "Then", VisualScriptPinDirection::Out));
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Wiring);
	CHECK(Single<VisualScriptConnectIntent>(fixture.Press()).Link == MakeLink(5, "Then", 3, "Execute"));
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Idle);
	CHECK(fixture.Release().empty());
	// A right press drops it.
	model.StartWire(3, "Execute", VisualScriptPinDirection::In);
	CHECK(fixture.Press(1).empty());
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Idle);
	fixture.Release(1);
}

TEST_CASE(
	"VisualScriptEditor: a right click opens the menu of what it was pressed on, or the palette on empty canvas; a right drag pans and opens nothing")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	VisualScriptContextMenuIntent menu =
		Single<VisualScriptContextMenuIntent>(fixture.Click(Testing::CanvasPoint(model, 2, glm::vec2(0.5f, 0.15f)), 1));
	CHECK(menu.Target.Kind == VisualScriptHitKind::NodeHeader);
	CHECK(menu.Target.Element == 2);
	// An unselected node becomes the selection.
	CHECK(fixture.Selection() == std::vector<uint32_t>{2});
	fixture.Input.Shift = true;
	fixture.Click(Testing::CanvasPoint(model, 3));
	fixture.Input.Shift = false;
	// On a selected node the selection stays: the menu acts on all of it.
	menu = Single<VisualScriptContextMenuIntent>(fixture.Click(Testing::CanvasPoint(model, 2), 1));
	CHECK(menu.Target.Element == 2);
	CHECK(AsSet(model.GetSelection()) == std::set<uint32_t>{2, 3});
	// Pins and wires leave the selection alone.
	menu = Single<VisualScriptContextMenuIntent>(fixture.Click(Testing::CanvasPoint(model, 2, "Duration"), 1));
	CHECK(menu.Target.Kind == VisualScriptHitKind::Pin);
	auto const durationIndex = [&model]()
	{
		VisualScriptCanvasModel const& canvas = model.GetCanvas();
		std::optional<uint32_t> const pin = canvas.FindPin(*canvas.FindNode(2), "Duration", VisualScriptPinDirection::In);
		REQUIRE(pin.has_value());
		return *pin;
	};
	CHECK(menu.Target.Pin == durationIndex());
	REQUIRE(menu.Pin.has_value());
	CHECK(*menu.Pin == VisualScriptPinReference{2, "Duration"});
	CHECK(menu.PinDirection == VisualScriptPinDirection::In);
	CHECK_FALSE(menu.WholeSide);
	// A layout between the press and the release moves the pins' indices (a commit puts a node before the Delay): the menu
	// names the pin pressed, at its index of now.
	fixture.MoveTo(Testing::CanvasPoint(model, 2, "Duration"));
	fixture.Press(1);
	uint32_t const pressedIndex = durationIndex();
	VisualScriptDocument document = model.GetDisplayDocument();
	document.Graph.Nodes.insert(document.Graph.Nodes.begin(), MakeNode(9, "Flow.Sequence", glm::vec2(0.0f, 2000.0f)));
	fixture.Commit(document);
	REQUIRE(durationIndex() != pressedIndex);
	menu = Single<VisualScriptContextMenuIntent>(fixture.Release(1));
	CHECK(menu.Target.Kind == VisualScriptHitKind::Pin);
	CHECK(menu.Target.Pin == durationIndex());
	REQUIRE(menu.Pin.has_value());
	CHECK(*menu.Pin == VisualScriptPinReference{2, "Duration"});
	// A collapsed node's anchor stands for its whole side.
	document.Graph.Nodes[2].Collapsed = true;
	REQUIRE(document.Graph.Nodes[2].ID == 2);
	fixture.Commit(document);
	fixture.Frame();
	menu = Single<VisualScriptContextMenuIntent>(fixture.Click(Testing::CanvasPoint(model, 2, "Duration"), 1));
	CHECK(menu.Target.Kind == VisualScriptHitKind::Pin);
	CHECK(menu.WholeSide);
	REQUIRE(menu.Pin.has_value());
	CHECK(menu.Pin->Node == 2);
	document.Graph.Nodes[2].Collapsed = false;
	fixture.Commit(document);
	fixture.Frame();
	menu = Single<VisualScriptContextMenuIntent>(fixture.Click(Testing::WirePoint(model, MakeLink(1, "Then", 2, "Execute")), 1));
	CHECK(menu.Target.Kind == VisualScriptHitKind::Link);
	CHECK(menu.Target.Link == std::optional<VisualScriptLink>(MakeLink(1, "Then", 2, "Execute")));
	CHECK(AsSet(model.GetSelection()) == std::set<uint32_t>{2, 3});
	// The menu is of what was pressed, though the release is a few pixels away.
	fixture.MoveTo(Testing::CanvasPoint(model, 1));
	fixture.Press(1);
	fixture.MoveTo(Testing::CanvasPoint(model, 1) + glm::vec2(3.0f, 0.0f));
	CHECK(Single<VisualScriptContextMenuIntent>(fixture.Release(1)).Target.Element == 1);

	VisualScriptPaletteIntent const palette = Single<VisualScriptPaletteIntent>(fixture.Click(fixture.Screen(EmptyCanvasPoint), 1));
	CHECK_FALSE(palette.From.has_value());
	CHECK(glm::length(palette.Position - EmptyCanvasPoint) < 0.01f);

	VisualScriptViewState const before = model.GetViewState();
	CHECK(fixture.Drag(fixture.Screen(EmptyCanvasPoint), fixture.Screen(EmptyCanvasPoint) + glm::vec2(100.0f, 40.0f), 1).empty());
	glm::vec2 const moved = before.Center - model.GetViewState().Center;
	CHECK(glm::length(moved - glm::vec2(100.0f, 40.0f) / model.GetView().GetPixelsPerUnit()) < 0.01f);
	// The middle button pans at once, without a threshold.
	VisualScriptViewState const panned = model.GetViewState();
	CHECK(fixture.Drag(fixture.Screen(EmptyCanvasPoint), fixture.Screen(EmptyCanvasPoint) + glm::vec2(2.0f, 0.0f), 2).empty());
	CHECK(model.GetViewState().Center.x < panned.Center.x);
}

TEST_CASE("VisualScriptEditor: a Space tap opens the palette at the cursor; a long press or a Space drag does not")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	// Space as ImGui reports it: the key, and at its press the space character it types.
	auto const space = [&fixture](bool down, double step = 1.0 / 60.0)
	{
		fixture.Input.SpaceDown = down;
		(down ? fixture.Input.SpacePressed : fixture.Input.SpaceReleased) = true;
		fixture.Input.Characters = down ? " " : "";
		return fixture.Frame(step);
	};
	fixture.MoveTo(fixture.Screen(EmptyCanvasPoint));
	CHECK(space(true).empty());
	CHECK(model.GetGesture().SpaceHeld);
	VisualScriptPaletteIntent const palette = Single<VisualScriptPaletteIntent>(space(false, 0.1));
	CHECK(palette.Query.empty());
	CHECK(glm::length(palette.Position - EmptyCanvasPoint) < 0.01f);
	CHECK_FALSE(model.GetGesture().SpaceHeld);

	space(true);
	CHECK(space(false, 0.3).empty());

	// Held, a left drag pans: the press neither selects nor starts a marquee, and releasing Space opens nothing.
	space(true);
	VisualScriptViewState const before = model.GetViewState();
	glm::vec2 const delay = Testing::CanvasPoint(model, 2);
	CHECK(fixture.Drag(delay, delay + glm::vec2(50.0f, 0.0f)).empty());
	CHECK(model.GetSelection().empty());
	CHECK(model.GetViewState().Center.x < before.Center.x);
	CHECK(space(false).empty());
	// Without focus Space is not the canvas's.
	fixture.Input.Focused = false;
	space(true);
	CHECK(space(false).empty());
}

TEST_CASE(
	"VisualScriptEditor: a character typed while Space is down opens the palette with it as the query, and its key's action does not run")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	fixture.Click(Testing::CanvasPoint(model, 7));
	VisualScriptViewState const before = model.GetView().GetTargetState();
	// Space as ImGui reports it: the key with the space it types, then the spaces its key repeat types while it is held.
	fixture.Input.SpaceDown = true;
	fixture.Input.SpacePressed = true;
	fixture.Input.Characters = " ";
	CHECK(fixture.Frame().empty());
	fixture.Input.Characters = "  ";
	CHECK(fixture.Frame().empty());
	// Space, F: the palette with "f", and the view is not framed.
	fixture.Input.Characters = "f";
	fixture.Input.Actions = {VisualScriptEditorAction::FrameSelection};
	VisualScriptPaletteIntent const palette = Single<VisualScriptPaletteIntent>(fixture.Frame());
	CHECK(palette.Query == "f");
	CHECK(model.GetView().GetTargetState() == before);
	// The next characters are the palette's own.
	fixture.Input.Characters = "o";
	CHECK(fixture.Frame().empty());
	// A chord with Ctrl still acts.
	fixture.Input.Ctrl = true;
	CHECK(fixture.Act(VisualScriptEditorAction::SelectAll).empty());
	fixture.Input.Ctrl = false;
	CHECK(model.GetSelection().size() == 8);
	fixture.Input.SpaceDown = false;
	fixture.Input.SpaceReleased = true;
	CHECK(fixture.Frame().empty());
	CHECK(model.GetView().GetTargetState() == before);
	// Without Space the same key frames the selection.
	fixture.Click(Testing::CanvasPoint(model, 7));
	fixture.Act(VisualScriptEditorAction::FrameSelection);
	CHECK_FALSE(model.GetView().GetTargetState() == before);
}

TEST_CASE("VisualScriptEditor: keys pressed during a gesture or a press do nothing but cancel it, and never analyze")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	fixture.Click(Testing::CanvasPoint(model, 2));
	VisualScriptViewState const view = model.GetView().GetTargetState();
	glm::vec2 const deltaTime = Testing::CanvasPoint(model, 4, "DeltaTime", VisualScriptPinDirection::Out);
	glm::vec2 const duration = Testing::CanvasPoint(model, 2, "Duration");
	fixture.Drag(deltaTime, fixture.Screen(EmptyCanvasPoint), 0, 4, false);
	REQUIRE(model.GetGesture().Kind == VisualScriptGesture::Wiring);
	// The window commits a literal meanwhile: its own change, which the wire outlives and which is analyzed after it.
	VisualScriptDocument document = model.GetDisplayDocument();
	document.Graph.Nodes[1].Inputs["Duration"] = 2;
	fixture.Commit(document);
	uint64_t const analyses = model.GetAnalysisCount();
	for (VisualScriptEditorAction const action :
	     {VisualScriptEditorAction::Delete, VisualScriptEditorAction::Paste, VisualScriptEditorAction::OpenPalette,
	      VisualScriptEditorAction::FrameAll, VisualScriptEditorAction::ZoomIn, VisualScriptEditorAction::SelectAll})
	{
		CHECK(fixture.Act(action).empty());
	}
	CHECK(model.GetAnalysisCount() == analyses);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Wiring);
	CHECK(fixture.Selection() == std::vector<uint32_t>{2});
	CHECK(model.GetView().GetTargetState() == view);
	fixture.MoveTo(duration);
	CHECK(Single<VisualScriptConnectIntent>(fixture.Release()).Link == MakeLink(4, "DeltaTime", 2, "Duration"));
	fixture.Frame();
	CHECK(model.GetAnalysisCount() == analyses + 1);

	// A press that is no drag yet takes no keys either; Escape ends it, so its release is no click (which would clear the
	// selection).
	fixture.MoveTo(fixture.Screen(EmptyCanvasPoint));
	fixture.Press();
	CHECK(fixture.Act(VisualScriptEditorAction::Delete).empty());
	CHECK(fixture.Act(VisualScriptEditorAction::Cancel).empty());
	CHECK(fixture.Release().empty());
	CHECK(fixture.Selection() == std::vector<uint32_t>{2});
	// Idle again, keys act.
	CHECK(Single<VisualScriptActionIntent>(fixture.Act(VisualScriptEditorAction::Delete)).Selection == std::vector<uint32_t>{2});
}

TEST_CASE("VisualScriptEditor: a change from outside during a drag cancels it, and the selection keeps the elements that still exist")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	fixture.Click(Testing::CanvasPoint(model, 1));
	fixture.Input.Shift = true;
	fixture.Click(Testing::CanvasPoint(model, 2));
	fixture.Input.Shift = false;
	glm::vec2 const delay = Testing::CanvasPoint(model, 2);
	fixture.Drag(delay, delay + glm::vec2(80.0f, 0.0f), 0, 4, false);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::MovingElements);

	// Create is deleted by automation meanwhile.
	VisualScriptDocument changed = model.GetDisplayDocument();
	changed.Graph.Nodes.erase(changed.Graph.Nodes.begin());
	changed.Graph.Links.erase(changed.Graph.Links.begin());
	AssetManager::GetAsset<VisualScriptAsset>(fixture.Graph)->SetDocument(changed);
	CHECK(fixture.MoveTo(delay + glm::vec2(90.0f, 0.0f)).empty());
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Idle);
	CHECK(fixture.Selection() == std::vector<uint32_t>{2});
	CHECK(model.GetNodePreview().Moved.empty());
	CHECK(fixture.Release().empty());
}

TEST_CASE("VisualScriptEditor: read-only graphs give no edit intents, while selecting, viewing, menus and copying still work")
{
	auto const check = [](InteractionFixture& fixture)
	{
		VisualScriptEditorModel const& model = fixture.Model;
		REQUIRE(model.IsReadOnly());
		glm::vec2 const delay = Testing::CanvasPoint(model, 2);
		CHECK(fixture.Drag(delay, delay + glm::vec2(80.0f, 30.0f)).empty());
		CHECK(fixture.Selection() == std::vector<uint32_t>{2});
		fixture.Drag(Testing::CanvasPoint(model, 4, "DeltaTime", VisualScriptPinDirection::Out), Testing::CanvasPoint(model, 2, "Duration"),
		             0, 4, false);
		CHECK(model.GetGesture().Kind == VisualScriptGesture::Idle);
		CHECK(fixture.Release().empty());
		glm::vec2 const wire = Testing::WirePoint(model, MakeLink(1, "Then", 2, "Execute"));
		fixture.Input.Alt = true;
		CHECK(fixture.Click(wire).empty());
		fixture.Input.Alt = false;
		CHECK(Of<VisualScriptInsertRerouteIntent>(fixture.DoubleClick(wire)).empty());
		fixture.Input.Ctrl = true;
		fixture.Drag(fixture.Screen({275.0f, -20.0f}), fixture.Screen({275.0f, 110.0f}), 1, 4, false);
		CHECK(model.GetGesture().Kind == VisualScriptGesture::Panning);
		CHECK(fixture.Release(1).empty());
		fixture.Input.Ctrl = false;
		fixture.Click(Testing::CanvasPoint(model, 2));
		for (VisualScriptEditorAction const action :
		     {VisualScriptEditorAction::Delete, VisualScriptEditorAction::Paste, VisualScriptEditorAction::OpenPalette,
		      VisualScriptEditorAction::NudgeLeft, VisualScriptEditorAction::AddCommentBox, VisualScriptEditorAction::ToggleCollapsed})
		{
			CHECK(fixture.Act(action).empty());
		}
		CHECK(Single<VisualScriptActionIntent>(fixture.Act(VisualScriptEditorAction::Copy)).Action == VisualScriptEditorAction::Copy);
		CHECK(Single<VisualScriptActionIntent>(fixture.Act(VisualScriptEditorAction::FindReferences)).Selection ==
		      std::vector<uint32_t>{2});
		CHECK(Single<VisualScriptContextMenuIntent>(fixture.Click(Testing::CanvasPoint(model, 3), 1)).Target.Element == 3);
		CHECK(fixture.Click(fixture.Screen(EmptyCanvasPoint), 1).empty());
		fixture.Input.Drop = VisualScriptCanvasDrop{VisualScriptDropKind::Asset, 77, {}};
		CHECK(fixture.Frame().empty());
		CHECK(fixture.Act(VisualScriptEditorAction::SelectAll).empty());
		CHECK(model.GetSelection().size() == 8);
	};
	SUBCASE("a file without write permission")
	{
		InteractionFixture fixture(MakeCanvasDocument());
		using Perms = std::filesystem::perms;
		std::filesystem::permissions(fixture.Project.File, Perms::owner_write | Perms::group_write | Perms::others_write,
		                             std::filesystem::perm_options::remove);
		struct Restore
		{
			std::filesystem::path File;
			~Restore()
			{
				std::error_code error;
				std::filesystem::permissions(File, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, error);
			}
		} const restore{fixture.Project.File};
		fixture.Model.CheckWritable();
		fixture.Frame();
		CHECK(fixture.Model.GetDocumentState() == VisualScriptDocumentState::ReadOnlyFile);
		check(fixture);
	}
	SUBCASE("a memory asset")
	{
		InteractionFixture fixture(MakeCanvasDocument(), true);
		CHECK(fixture.Model.GetDocumentState() == VisualScriptDocumentState::ReadOnly);
		CHECK(fixture.Model.GetState().Path == "Generated");
		check(fixture);
	}
}

TEST_CASE(
	"VisualScriptEditor: a wire start analyzes a document changed since the last analysis at once, and no analysis runs during a gesture")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel& model = fixture.Model;
	uint64_t const first = model.GetAnalysisCount();
	fixture.MoveTo(Testing::CanvasPoint(model, 4, "DeltaTime", VisualScriptPinDirection::Out));
	fixture.Press();
	// One frame: a commit is analyzed, then a second one, after the frame's analysis is spent.
	fixture.Time += 1.0 / 60.0;
	fixture.Input.Time = fixture.Time;
	VisualScriptDocument document = model.GetDisplayDocument();
	document.Graph.Nodes[1].Inputs["Duration"] = 2;
	fixture.Commit(document);
	CHECK(model.GetAnalysisCount() == first + 1);
	document.Graph.Nodes[1].Inputs["Duration"] = 3;
	fixture.Commit(document);
	CHECK(model.GetAnalysisCount() == first + 1);
	// The wire starts in that frame's Update: its pins are analyzed first.
	fixture.Input.Mouse += glm::vec2(30.0f, 0.0f);
	CHECK(model.Update(fixture.Input).empty());
	CHECK(model.GetAnalysisCount() == first + 2);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Wiring);
	fixture.Release();

	// During a move, not even a new library is analyzed; after it, it is.
	glm::vec2 const delay = Testing::CanvasPoint(model, 2);
	fixture.Drag(delay, delay + glm::vec2(60.0f, 0.0f), 0, 4, false);
	fixture.Semantics.ReloadLibrary();
	fixture.Frame();
	fixture.Frame();
	CHECK(model.GetAnalysisCount() == first + 2);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::MovingElements);
	fixture.Release();
	fixture.Frame();
	CHECK(model.GetAnalysisCount() == first + 3);
}

TEST_CASE("VisualScriptEditor: each sub-graph keeps its view and selection, and Back and Forward walk the views left")
{
	VisualScriptDocument document = MakeCanvasDocument();
	VisualScriptFunction function;
	function.ID = 40;
	function.Name = "Helper";
	function.Graph.Nodes = {MakeNode(41, "Function.Entry", glm::vec2(0.0f)), MakeNode(42, "Function.Return", glm::vec2(480.0f, 0.0f))};
	function.Graph.Links = {MakeLink(41, "Then", 42, "Execute")};
	document.Functions = {function};
	InteractionFixture fixture(document);
	VisualScriptEditorModel& model = fixture.Model;
	fixture.Click(Testing::CanvasPoint(model, 1));
	VisualScriptViewState const eventView = model.GetViewState();
	CHECK_FALSE(model.CanGoBack());

	model.ShowFunction(40);
	fixture.Frame();
	CHECK(model.GetFunction() == 40);
	CHECK(model.GetViewState().Function == 40);
	CHECK(model.GetSelection().empty());
	CHECK(model.GetCanvas().GetNodes().size() == 2);
	CHECK(model.CanGoBack());
	CHECK(fixture.Settings.FindGraph(fixture.Graph)->Shown == 40);
	fixture.Click(Testing::CanvasPoint(model, 41));
	VisualScriptViewState const functionView = model.GetViewState();

	fixture.Act(VisualScriptEditorAction::Back);
	CHECK(model.GetFunction() == 0);
	CHECK(fixture.Selection() == std::vector<uint32_t>{1});
	CHECK(model.GetViewState() == eventView);
	CHECK(model.CanGoForward());
	fixture.Act(VisualScriptEditorAction::Forward);
	CHECK(model.GetFunction() == 40);
	CHECK(fixture.Selection() == std::vector<uint32_t>{41});
	CHECK(model.GetViewState() == functionView);
	// The mouse's back and forward buttons.
	fixture.Input.ExtraPressed[0] = true;
	fixture.Frame();
	CHECK(model.GetFunction() == 0);
	fixture.Input.ExtraPressed[1] = true;
	fixture.Frame();
	CHECK(model.GetFunction() == 40);
	// Each sub-graph's view is kept in the per-user settings.
	REQUIRE(fixture.Settings.FindGraph(fixture.Graph) != nullptr);
	CHECK(fixture.Settings.FindGraph(fixture.Graph)->FindView(0) != nullptr);
	CHECK(*fixture.Settings.FindGraph(fixture.Graph)->FindView(40) == functionView);

	// A function deleted while shown: the event graph shows, and the history skips its views.
	VisualScriptDocument without = model.GetDisplayDocument();
	without.Functions.clear();
	AssetManager::GetAsset<VisualScriptAsset>(fixture.Graph)->SetDocument(without);
	fixture.Frame();
	CHECK(model.GetFunction() == 0);
	while (model.CanGoBack())
	{
		fixture.Act(VisualScriptEditorAction::Back);
	}
	CHECK(model.GetFunction() == 0);

	// The history keeps the last 64 views.
	for (int step = 0; step < 70; step++)
	{
		fixture.Act(VisualScriptEditorAction::ZoomIn);
		VisualScriptOpenRequest request;
		request.Graph = fixture.Graph;
		request.Center = glm::vec2(static_cast<float>(step) * 50.0f, 0.0f);
		model.ApplyOpenRequest(request);
		fixture.Frame(0.25);
	}
	int backs = 0;
	while (model.CanGoBack())
	{
		fixture.Act(VisualScriptEditorAction::Back);
		backs++;
	}
	CHECK(backs == static_cast<int>(VisualScriptEditorModel::MaxHistory));
}

TEST_CASE("VisualScriptEditor: bookmarks keep views named after the comment at the view's centre, and going to one pushes the history")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel& model = fixture.Model;
	fixture.Settings.SetReduceMotion(true);
	auto const centerOn = [&fixture, &model](glm::vec2 center)
	{
		VisualScriptOpenRequest request;
		request.Graph = fixture.Graph;
		request.Center = center;
		request.Zoom = 1.0f;
		model.ApplyOpenRequest(request);
		fixture.Frame();
	};
	centerOn(glm::vec2(312.0f, 40.0f));
	fixture.Act(VisualScriptEditorAction::StoreBookmark3);
	VisualScriptBookmark const* start = fixture.Settings.FindGraph(fixture.Graph)->FindBookmark(3);
	REQUIRE(start != nullptr);
	CHECK(start->Name == "Start");
	CHECK(start->View == model.GetViewState());
	VisualScriptViewState const startView = start->View;
	centerOn(glm::vec2(3000.0f, 2000.0f));
	VisualScriptViewState const away = model.GetViewState();
	fixture.Act(VisualScriptEditorAction::StoreBookmark5);
	CHECK(fixture.Settings.FindGraph(fixture.Graph)->FindBookmark(5)->Name == "Bookmark 5");
	CHECK(model.GetState().Bookmarks == std::vector<uint8_t>{3, 5});

	fixture.Act(VisualScriptEditorAction::GoToBookmark3);
	CHECK(model.GetViewState() == startView);
	fixture.Act(VisualScriptEditorAction::Back);
	CHECK(model.GetViewState() == away);
	// A free slot goes nowhere.
	fixture.Act(VisualScriptEditorAction::GoToBookmark9);
	CHECK(model.GetViewState() == away);
}

TEST_CASE("VisualScriptEditor: an open request shows its sub-graph, then frames, then zooms about the framed centre, then centres")
{
	VisualScriptDocument document = MakeCanvasDocument();
	VisualScriptFunction function;
	function.ID = 40;
	function.Name = "Helper";
	function.Graph.Nodes = {MakeNode(41, "Function.Entry", glm::vec2(0.0f)), MakeNode(42, "Function.Return", glm::vec2(480.0f, 0.0f))};
	document.Functions = {function};
	InteractionFixture fixture(document);
	VisualScriptEditorModel& model = fixture.Model;

	VisualScriptOpenRequest request;
	request.Graph = fixture.Graph;
	request.Function = 40;
	request.Select = {42};
	request.Frame = VisualScriptFrame::Selection;
	request.Zoom = 1.25f;
	model.ApplyOpenRequest(request);
	CHECK(model.GetFunction() == 40);
	fixture.Frame();
	CHECK(fixture.Selection() == std::vector<uint32_t>{42});
	std::optional<VisualScriptRect> const bounds = model.GetCanvas().GetBounds(std::vector<uint32_t>{42});
	REQUIRE(bounds.has_value());
	VisualScriptViewState const target = model.GetView().GetTargetState();
	CHECK(target.Function == 40);
	CHECK(target.Zoom == 1.25f);
	CHECK(glm::length(target.Center - bounds->GetCenter()) < 0.01f);
	// Animated: there after the move's time.
	CHECK(model.GetView().IsAnimating());
	fixture.Frame(0.25);
	CHECK(model.GetViewState() == target);

	// An explicit centre is applied last.
	request = VisualScriptOpenRequest();
	request.Graph = fixture.Graph;
	request.Frame = VisualScriptFrame::All;
	request.Center = glm::vec2(100.0f, 50.0f);
	model.ApplyOpenRequest(request);
	fixture.Frame(0.25);
	fixture.Frame(0.25);
	CHECK(model.GetViewState().Center == glm::vec2(100.0f, 50.0f));
	CHECK(model.GetViewState().Zoom <= 1.0f);

	// A window's first view is not animated.
	VisualScriptEditorModel second(fixture.Graph, fixture.Settings);
	VisualScriptOpenRequest first;
	first.Graph = fixture.Graph;
	first.Function = 0;
	first.Center = glm::vec2(10.0f, 20.0f);
	first.Zoom = 0.5f;
	second.ApplyOpenRequest(first);
	second.Sync(fixture.Semantics, fixture.Measure, fixture.Theme, false, fixture.Time);
	second.Update(fixture.Input);
	CHECK(second.GetViewState().Center == glm::vec2(10.0f, 20.0f));
	CHECK(second.GetViewState().Zoom == 0.5f);
	CHECK_FALSE(second.GetView().IsAnimating());
}

TEST_CASE(
	"VisualScriptEditor: edit actions become intents with the selection and the cursor, and view and selection actions apply in the model")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	fixture.Click(Testing::CanvasPoint(model, 2));
	fixture.Input.Shift = true;
	fixture.Click(Testing::CanvasPoint(model, 3));
	fixture.Input.Shift = false;
	fixture.MoveTo(fixture.Screen(EmptyCanvasPoint));
	VisualScriptActionIntent action = Single<VisualScriptActionIntent>(fixture.Act(VisualScriptEditorAction::Delete));
	CHECK(action.Action == VisualScriptEditorAction::Delete);
	CHECK(action.Selection == std::vector<uint32_t>{3, 2});
	CHECK(glm::length(action.Position - EmptyCanvasPoint) < 0.01f);
	// Without the mouse over the canvas, things go to the view's centre.
	fixture.Input.Hovered = false;
	action = Single<VisualScriptActionIntent>(fixture.Act(VisualScriptEditorAction::Paste));
	CHECK(action.Position == model.GetViewState().Center);
	fixture.Input.Hovered = true;
	CHECK(Single<VisualScriptActionIntent>(fixture.Act(VisualScriptEditorAction::NudgeLeft)).Action == VisualScriptEditorAction::NudgeLeft);
	CHECK(Single<VisualScriptActionIntent>(fixture.Act(VisualScriptEditorAction::AlignTop)).Selection.size() == 2);
	// The palette from Tab takes what was typed in its frame.
	fixture.Input.Characters = "br";
	CHECK(Single<VisualScriptPaletteIntent>(fixture.Act(VisualScriptEditorAction::OpenPalette)).Query == "br");

	CHECK(fixture.Act(VisualScriptEditorAction::SelectAll).empty());
	CHECK(AsSet(model.GetSelection()) == std::set<uint32_t>{1, 2, 3, 4, 5, 6, 7, 20});
	// The primary element stays primary.
	CHECK(model.GetSelection().front() == 3);
	CHECK(fixture.Act(VisualScriptEditorAction::Cancel).empty());
	CHECK(model.GetSelection().empty());
	fixture.Click(Testing::CanvasPoint(model, 1));
	CHECK(fixture.Act(VisualScriptEditorAction::DeselectAll).empty());
	CHECK(model.GetSelection().empty());

	CHECK(fixture.Act(VisualScriptEditorAction::ToggleSnapToGrid).empty());
	CHECK_FALSE(fixture.Settings.IsSnapToGrid());
	fixture.Act(VisualScriptEditorAction::ToggleSnapToGrid);
	CHECK(fixture.Settings.IsSnapToGrid());
	fixture.Act(VisualScriptEditorAction::ToggleMinimap);
	CHECK_FALSE(fixture.Settings.GetPanes().ShowMinimap);

	float const zoom = model.GetViewState().Zoom;
	fixture.Act(VisualScriptEditorAction::ZoomOut);
	CHECK(model.GetViewState().Zoom < zoom);
	fixture.Act(VisualScriptEditorAction::ZoomIn);
	CHECK(model.GetViewState().Zoom == zoom);
	// 100 % about the mouse: the canvas point under it stays.
	fixture.Act(VisualScriptEditorAction::ZoomOut);
	fixture.MoveTo(Testing::CanvasPoint(model, 7));
	glm::vec2 const under = fixture.Canvas(fixture.Input.Mouse);
	fixture.Act(VisualScriptEditorAction::ZoomReset);
	CHECK(model.GetViewState().Zoom == 1.0f);
	CHECK(glm::length(fixture.Canvas(fixture.Input.Mouse) - under) < 0.01f);
	// Framing everything.
	fixture.Act(VisualScriptEditorAction::FrameAll);
	VisualScriptViewState const framed = model.GetView().GetTargetState();
	CHECK(glm::length(framed.Center - model.GetCanvas().GetBounds()->GetCenter()) < 0.01f);

	// Escape cancels a gesture: a move ends without moving anything.
	glm::vec2 const delay = Testing::CanvasPoint(model, 2);
	fixture.Drag(delay, delay + glm::vec2(80.0f, 0.0f), 0, 4, false);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::MovingElements);
	CHECK(fixture.Act(VisualScriptEditorAction::Cancel).empty());
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Idle);
	CHECK(fixture.Release().empty());
	CHECK(fixture.Selection() == std::vector<uint32_t>{2});
}

TEST_CASE(
	"VisualScriptEditor: without the scripting runtime, wires, the palette and reroutes on wires are refused, while moving, cutting and breaking links work")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	fixture.Semantics.SetAvailable(false);
	fixture.Frame();
	REQUIRE(model.GetAnalysis() == nullptr);
	uint64_t const refused = model.GetRefusedCount();
	CHECK(
		fixture.Drag(Testing::CanvasPoint(model, 4, "DeltaTime", VisualScriptPinDirection::Out), Testing::CanvasPoint(model, 2, "Duration"))
			.empty());
	CHECK(model.GetRefusedCount() == refused + 1);
	CHECK(fixture.Act(VisualScriptEditorAction::OpenPalette).empty());
	CHECK(fixture.Click(fixture.Screen(EmptyCanvasPoint), 1).empty());
	CHECK(fixture.DoubleClick(Testing::WirePoint(model, MakeLink(1, "Then", 2, "Execute"))).empty());
	CHECK(model.GetRefusedCount() == refused + 4);

	glm::vec2 const delay = Testing::CanvasPoint(model, 2);
	CHECK(Single<VisualScriptMoveIntent>(fixture.Drag(delay, delay + glm::vec2(64.0f, 0.0f))).Elements == std::vector<uint32_t>{2});
	fixture.Input.Alt = true;
	CHECK(Single<VisualScriptDisconnectIntent>(fixture.Click(Testing::WirePoint(model, MakeLink(2, "Completed", 3, "Execute"))))
	          .Links.size() == 1);
	fixture.Input.Alt = false;
	fixture.Input.Ctrl = true;
	CHECK(Single<VisualScriptDisconnectIntent>(fixture.Drag(fixture.Screen({275.0f, -20.0f}), fixture.Screen({275.0f, 110.0f}), 1))
	          .Links.size() == 1);
	fixture.Input.Ctrl = false;
	// Edits the controller can do without pins pass; it refuses those that need them.
	CHECK(Single<VisualScriptActionIntent>(fixture.Act(VisualScriptEditorAction::DeleteWithoutRelink)).Selection ==
	      std::vector<uint32_t>{2});
}

TEST_CASE("VisualScriptEditor: a single unlinked node resting 0.3 s on a wire it fits is spliced into it at the release; Alt prevents it")
{
	VisualScriptDocument document = MakeCanvasDocument();
	document.Graph.Nodes.push_back(MakeNode(8, "Flow.Delay", glm::vec2(400.0f, 600.0f)));
	InteractionFixture fixture(document);
	VisualScriptEditorModel const& model = fixture.Model;
	VisualScriptLink const link = MakeLink(1, "Then", 2, "Execute");
	glm::vec2 const header = Testing::CanvasPoint(model, 8, glm::vec2(0.5f, 0.15f));
	glm::vec2 const wire = Testing::WirePoint(model, link);

	fixture.Drag(header, wire, 0, 4, false);
	CHECK(model.GetGesture().SpliceTarget == nullptr);
	fixture.MoveTo(wire, 0.35);
	REQUIRE(model.GetGesture().SpliceTarget != nullptr);
	CHECK(*model.GetGesture().SpliceTarget == link);
	VisualScriptSpliceIntent const splice = Single<VisualScriptSpliceIntent>(fixture.Release());
	CHECK(splice.Node == 8);
	CHECK(splice.Link == link);
	CHECK(splice.InputPin == "Execute");
	CHECK(splice.OutputPin == "Completed");
	CHECK(splice.Offset == fixture.SnappedOffset(glm::vec2(400.0f, 600.0f), header, wire));

	// Released before it rested: a move.
	CHECK(Single<VisualScriptMoveIntent>(fixture.Drag(header, wire)).Elements == std::vector<uint32_t>{8});
	// With Alt: a move.
	fixture.Input.Alt = true;
	fixture.Drag(header, wire, 0, 4, false);
	fixture.MoveTo(wire, 0.35);
	CHECK(model.GetGesture().SpliceTarget == nullptr);
	CHECK(Single<VisualScriptMoveIntent>(fixture.Release()).Elements == std::vector<uint32_t>{8});
	fixture.Input.Alt = false;
	// A node with links is never spliced.
	glm::vec2 const branch = Testing::CanvasPoint(model, 3, glm::vec2(0.5f, 0.15f));
	fixture.Drag(branch, wire, 0, 4, false);
	fixture.MoveTo(wire, 0.35);
	CHECK(model.GetGesture().SpliceTarget == nullptr);
	fixture.Release();
}

TEST_CASE(
	"VisualScriptEditor: during a drag, moved comments and the wires of a reroute that faces a moved node are drawn and hit where they are drawn")
{
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(1, "Event.Update", glm::vec2(0.0f, 0.0f)), MakeNode(2, "Reroute", glm::vec2(600.0f, 300.0f)),
	                        MakeNode(3, "Flow.Branch", glm::vec2(900.0f, 300.0f))};
	document.Graph.Links = {MakeLink(1, "Then", 2, "In"), MakeLink(2, "Out", 3, "Execute")};
	VisualScriptComment box;
	box.ID = 20;
	box.Text = "Box";
	box.Position = glm::vec2(0.0f, 600.0f);
	box.Size = glm::vec2(300.0f, 160.0f);
	document.Graph.Comments = {box};
	InteractionFixture fixture(document);
	VisualScriptEditorModel const& model = fixture.Model;
	VisualScriptCanvasModel const& canvas = model.GetCanvas();
	uint32_t const update = *canvas.FindNode(1);
	uint32_t const reroute = *canvas.FindNode(2);
	std::span<VisualScriptLinkVisual const> const links = canvas.GetLinks();
	auto const findLink = [&links](uint32_t from, uint32_t to)
	{
		for (uint32_t link = 0; link < links.size(); link++)
		{
			if (links[link].FromNode == from && links[link].ToNode == to)
			{
				return link;
			}
		}
		FAIL("no such link");
		return 0u;
	};
	uint32_t const intoReroute = findLink(update, reroute);

	// The Branch dragged left of the reroute: the reroute turns to face it, so its incoming wire bends out to the right, past
	// the bounds the index holds for it. That wire is dynamic, drawn and hit where it is drawn.
	glm::vec2 const branch = Testing::CanvasPoint(model, 3, glm::vec2(0.5f, 0.15f));
	fixture.Drag(branch, branch - glm::vec2(500.0f, 0.0f), 0, 4, false);
	REQUIRE(model.GetGesture().Kind == VisualScriptGesture::MovingElements);
	std::span<uint32_t const> const dynamic = model.GetDynamicLinks();
	CHECK(std::find(dynamic.begin(), dynamic.end(), intoReroute) != dynamic.end());
	VisualScriptNodePreview const preview = model.GetNodePreview();
	std::array<glm::vec2, 4> const points = VisualScriptGeometry::GetLinkControlPoints(canvas, intoReroute, preview);
	glm::vec2 rightmost = points[0];
	for (int step = 0; step <= 40; step++)
	{
		float const t = static_cast<float>(step) / 40.0f;
		float const u = 1.0f - t;
		glm::vec2 const point = u * u * u * points[0] + 3.0f * u * u * t * points[1] + 3.0f * u * t * t * points[2] + t * t * t * points[3];
		if (point.x > rightmost.x)
		{
			rightmost = point;
		}
	}
	VisualScriptRect const stored =
		model.GetIndex().GetBounds(VisualScriptSpatialIndex::MakeItem(VisualScriptSpatialIndex::ItemKind::Link, intoReroute));
	REQUIRE(rightmost.x > stored.Max.x + 20.0f);
	VisualScriptHitContext context;
	context.Selected = model.GetNodeSelection();
	context.Z = model.GetNodeRaise();
	context.Preview = preview;
	context.DynamicLinks = model.GetDynamicLinks();
	VisualScriptHit const hit = HitTestCanvas(canvas, model.GetIndex(), model.GetView(), context, fixture.Screen(rightmost));
	CHECK(hit.Kind == VisualScriptHitKind::Link);
	REQUIRE(hit.Link.has_value());
	CHECK(*hit.Link == MakeLink(1, "Then", 2, "In"));
	VisualScriptRect const around{rightmost - glm::vec2(2.0f), rightmost + glm::vec2(2.0f)};
	std::vector<uint32_t> drawn;
	model.QueryDrawn(around, VisualScriptSpatialIndex::LinkMask, drawn);
	CHECK(std::find(drawn.begin(), drawn.end(),
	                VisualScriptSpatialIndex::MakeItem(VisualScriptSpatialIndex::ItemKind::Link, intoReroute)) != drawn.end());
	fixture.Release();
	CHECK(model.GetDynamicLinks().empty());

	// The comment dragged down, out of its stored rectangle: left out of the index's queries, but drawn where the preview has
	// it.
	glm::vec2 const title = Testing::CommentPoint(model, 20, glm::vec2(0.5f, 0.1f));
	fixture.Drag(title, title + glm::vec2(0.0f, 300.0f), 0, 4, false);
	REQUIRE(model.GetGesture().Kind == VisualScriptGesture::MovingElements);
	uint32_t const comment = *canvas.FindComment(20);
	uint32_t const commentItem = VisualScriptSpatialIndex::MakeItem(VisualScriptSpatialIndex::ItemKind::Comment, comment);
	CHECK(std::vector<uint32_t>(model.GetDynamicComments().begin(), model.GetDynamicComments().end()) == std::vector<uint32_t>{comment});
	VisualScriptRect const moved = VisualScriptGeometry::Translate(canvas.GetComments()[comment].Rect, model.GetGesture().MoveOffset);
	VisualScriptRect const below{glm::vec2(moved.Min.x, canvas.GetComments()[comment].Rect.Max.y + 10.0f), moved.Max};
	REQUIRE(below.Min.y < below.Max.y);
	model.GetIndex().Query(below, VisualScriptSpatialIndex::CommentMask, drawn);
	CHECK(drawn.empty());
	model.QueryDrawn(below, VisualScriptSpatialIndex::CommentMask, drawn);
	CHECK(drawn == std::vector<uint32_t>{commentItem});
	fixture.Release();
	CHECK(model.GetDynamicComments().empty());
	model.QueryDrawn(below, VisualScriptSpatialIndex::CommentMask, drawn);
	CHECK(drawn.empty());
}

TEST_CASE("VisualScriptEditor: dragging a comment's edge resizes it, snapped, and never below 96 x 64")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	glm::vec2 const right = Testing::CommentPoint(model, 20, glm::vec2(1.0f, 0.9f));
	fixture.Drag(right, right + glm::vec2(100.0f, 0.0f), 0, 4, false);
	{
		VisualScriptGestureView const gesture = model.GetGesture();
		CHECK(gesture.Kind == VisualScriptGesture::ResizingComment);
		CHECK(gesture.ResizedComment == std::optional<uint32_t>(20));
		CHECK(gesture.ResizeEdges == VisualScriptGeometry::RightEdge);
	}
	VisualScriptResizeCommentIntent resize = Single<VisualScriptResizeCommentIntent>(fixture.Release());
	CHECK(resize.Comment == 20);
	float const expected = std::round((672.0f + 100.0f / model.GetView().GetPixelsPerUnit()) / 16.0f) * 16.0f;
	CHECK(resize.Rect.Min == glm::vec2(-48.0f, -80.0f));
	CHECK(resize.Rect.Max == glm::vec2(expected, 160.0f));
	CHECK(fixture.Selection() == std::vector<uint32_t>{20});

	glm::vec2 const left = Testing::CommentPoint(model, 20, glm::vec2(0.0f, 0.9f));
	resize = Single<VisualScriptResizeCommentIntent>(fixture.Drag(left, left + glm::vec2(900.0f, 0.0f)));
	CHECK(resize.Rect.Max.x == 672.0f);
	CHECK(resize.Rect.Max.x - resize.Rect.Min.x == VisualScriptCanvasMetrics::CommentMinWidth);
}

TEST_CASE(
	"VisualScriptEditor: a double-click opens a function call's function, takes a sender to its event, renames a comment and edits a node comment")
{
	VisualScriptDocument document = MakeCanvasDocument();
	VisualScriptFunction function;
	function.ID = 40;
	function.Name = "Helper";
	function.Graph.Nodes = {MakeNode(41, "Function.Entry", glm::vec2(0.0f))};
	document.Functions = {function};
	VisualScriptNode call = MakeNode(9, "Function.Call", glm::vec2(0.0f, 640.0f));
	call.Settings["Function"] = 40;
	VisualScriptNode sender = MakeNode(10, "Event.Send", glm::vec2(400.0f, 640.0f));
	sender.Settings["Name"] = "Hit";
	VisualScriptNode receiver = MakeNode(11, "Event.Custom", glm::vec2(1200.0f, 0.0f));
	receiver.Settings["Name"] = "Hit";
	document.Graph.Nodes.push_back(call);
	document.Graph.Nodes.push_back(sender);
	document.Graph.Nodes.push_back(receiver);
	document.Graph.Nodes[1].Comment = "Waits a second";
	InteractionFixture fixture(document);
	VisualScriptEditorModel const& model = fixture.Model;

	std::vector<VisualScriptOpenFunctionIntent> const opened =
		Of<VisualScriptOpenFunctionIntent>(fixture.DoubleClick(Testing::CanvasPoint(model, 9, glm::vec2(0.5f, 0.1f))));
	REQUIRE(opened.size() == 1);
	CHECK(opened.front().Function == 40);

	CHECK(Of<VisualScriptActionIntent>(fixture.DoubleClick(Testing::CanvasPoint(model, 10, glm::vec2(0.5f, 0.1f)))).empty());
	fixture.Frame();
	CHECK(fixture.Selection() == std::vector<uint32_t>{11});
	CHECK(model.CanGoBack());
	std::optional<VisualScriptRect> const event = model.GetCanvas().GetBounds(std::vector<uint32_t>{11});
	REQUIRE(event.has_value());
	CHECK(glm::length(model.GetView().GetTargetState().Center - event->GetCenter()) < 0.01f);
	// The jump animates; the next points are taken where it ends.
	fixture.Frame(0.25);
	CHECK_FALSE(model.GetView().IsAnimating());

	std::vector<VisualScriptActionIntent> open =
		Of<VisualScriptActionIntent>(fixture.DoubleClick(Testing::CanvasPoint(model, 5, glm::vec2(0.5f, 0.1f))));
	REQUIRE(open.size() == 1);
	CHECK(open.front().Action == VisualScriptEditorAction::OpenNode);
	CHECK(open.front().Selection.front() == 5);

	// Back to the view before the jump, where the comment and the bubble are on the canvas (beyond its edges nothing is hit).
	CHECK(Testing::CommentPoint(model, 20, glm::vec2(0.3f, 0.05f)).x < 0.0f);
	fixture.Act(VisualScriptEditorAction::Back);
	fixture.Frame(0.25);
	open = Of<VisualScriptActionIntent>(fixture.DoubleClick(Testing::CommentPoint(model, 20, glm::vec2(0.3f, 0.05f))));
	REQUIRE(open.size() == 1);
	CHECK(open.front().Action == VisualScriptEditorAction::Rename);
	CHECK(open.front().Selection.front() == 20);

	std::optional<uint32_t> const delay = model.GetCanvas().FindNode(2);
	REQUIRE(delay.has_value());
	VisualScriptRect const bubble = VisualScriptGeometry::GetBubbleRect(model.GetCanvas().GetNodes()[*delay]);
	REQUIRE_FALSE(VisualScriptGeometry::IsEmpty(bubble));
	open = Of<VisualScriptActionIntent>(fixture.DoubleClick(fixture.Screen(bubble.GetCenter())));
	REQUIRE(open.size() == 1);
	CHECK(open.front().Action == VisualScriptEditorAction::EditNodeComment);
	CHECK(open.front().Selection.front() == 2);
}

TEST_CASE("VisualScriptEditor: a drop names the pin it lands on, or the canvas point, with the keys held")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	fixture.MoveTo(Testing::CanvasPoint(model, 2, "Duration"));
	fixture.Input.Drop = VisualScriptCanvasDrop{VisualScriptDropKind::Asset, 77, {}};
	fixture.Input.Ctrl = true;
	VisualScriptDropIntent drop = Single<VisualScriptDropIntent>(fixture.Frame());
	fixture.Input.Ctrl = false;
	CHECK(drop.Kind == VisualScriptDropKind::Asset);
	CHECK(drop.Payload == 77);
	REQUIRE(drop.Pin.has_value());
	CHECK(*drop.Pin == VisualScriptPinReference{2, "Duration"});
	CHECK(drop.Direction == VisualScriptPinDirection::In);
	CHECK(drop.Modifiers == VisualScriptDropModifiers::Ctrl);

	fixture.MoveTo(fixture.Screen(EmptyCanvasPoint));
	fixture.Input.Drop = VisualScriptCanvasDrop{VisualScriptDropKind::Entities, 0, {5, 6}};
	fixture.Input.Alt = true;
	drop = Single<VisualScriptDropIntent>(fixture.Frame());
	fixture.Input.Alt = false;
	CHECK(drop.Kind == VisualScriptDropKind::Entities);
	CHECK(drop.Entities == std::vector<uint64_t>{5, 6});
	CHECK_FALSE(drop.Pin.has_value());
	CHECK(glm::length(drop.Position - EmptyCanvasPoint) < 0.01f);
	CHECK(drop.Modifiers == VisualScriptDropModifiers::Alt);
}

TEST_CASE("VisualScriptEditor: the minimap shows while content is out of view, and a press or a drag on it centres the view there")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel& model = fixture.Model;
	// Everything is in view: hidden.
	CHECK(model.GetMinimap().Alpha == 0.0f);
	CHECK_FALSE(model.GetState().MinimapVisible);
	fixture.Settings.SetReduceMotion(true);
	VisualScriptOpenRequest request;
	request.Graph = fixture.Graph;
	request.Center = glm::vec2(0.0f);
	request.Zoom = 2.0f;
	model.ApplyOpenRequest(request);
	fixture.Frame();
	fixture.Settings.SetReduceMotion(false);
	CHECK(model.GetMinimap().Alpha == 1.0f);
	VisualScriptMinimapState const minimap = model.GetMinimap();
	glm::vec2 const canvasMax = fixture.Input.CanvasMin + fixture.Input.CanvasSize;
	CHECK(minimap.Transform.Rect.Max.x == canvasMax.x - 12.0f);
	CHECK(minimap.Transform.Rect.GetSize().x == 280.0f);
	CHECK(model.GetState().MinimapVisible);

	glm::vec2 const middle = minimap.Transform.Rect.GetCenter();
	fixture.MoveTo(middle);
	CHECK(model.GetHover().Kind == VisualScriptHitKind::Minimap);
	CHECK(fixture.Press().empty());
	CHECK(model.GetGesture().Kind == VisualScriptGesture::MinimapDrag);
	CHECK(glm::length(model.GetViewState().Center - minimap.Transform.ToCanvas(middle)) < 0.01f);
	glm::vec2 const lower = middle + glm::vec2(20.0f, 10.0f);
	fixture.MoveTo(lower);
	CHECK(glm::length(model.GetViewState().Center - model.GetMinimap().Transform.ToCanvas(lower)) < 0.01f);
	CHECK(fixture.Release().empty());
	CHECK(model.GetSelection().empty());
	// The wheel over it zooms about the view's centre.
	VisualScriptViewState const before = model.GetViewState();
	fixture.Input.Wheel = glm::vec2(0.0f, -1.0f);
	fixture.Frame();
	CHECK(model.GetViewState().Zoom < before.Zoom);
	CHECK(glm::length(model.GetViewState().Center - before.Center) < 0.01f);
	// Turned off, it fades out, and the canvas under it takes the mouse.
	fixture.Act(VisualScriptEditorAction::ToggleMinimap);
	fixture.Frame(0.1);
	CHECK(model.GetMinimap().Alpha > 0.0f);
	fixture.Frame(0.15);
	CHECK(model.GetMinimap().Alpha == 0.0f);
	CHECK(model.GetHover().Kind != VisualScriptHitKind::Minimap);
}

TEST_CASE("VisualScriptEditor: the wheel zooms to the cursor or pans as the scroll mode says, and a drag near an edge pans the view")
{
	InteractionFixture fixture(MakeCanvasDocument());
	VisualScriptEditorModel const& model = fixture.Model;
	fixture.Settings.SetScrollMode(VisualScriptScrollMode::Zoom);
	glm::vec2 const mouse = fixture.Screen(EmptyCanvasPoint);
	fixture.MoveTo(mouse);
	float const zoom = model.GetViewState().Zoom;
	fixture.Input.Wheel = glm::vec2(0.0f, -1.0f);
	fixture.Frame();
	CHECK(model.GetViewState().Zoom < zoom);
	CHECK(glm::length(fixture.Canvas(mouse) - EmptyCanvasPoint) < 0.01f);

	fixture.Settings.SetScrollMode(VisualScriptScrollMode::Pan);
	VisualScriptViewState const before = model.GetViewState();
	fixture.Input.Wheel = glm::vec2(0.0f, -1.0f);
	fixture.Frame();
	CHECK(model.GetViewState().Zoom == before.Zoom);
	CHECK(model.GetViewState().Center.y > before.Center.y);

	// A marquee held at the canvas's right edge pans the view toward it.
	fixture.Drag(mouse, glm::vec2(fixture.Input.CanvasSize.x - 1.0f, mouse.y), 0, 4, false);
	CHECK(model.GetGesture().Kind == VisualScriptGesture::Marquee);
	float const x = model.GetViewState().Center.x;
	fixture.Frame();
	fixture.Frame();
	CHECK(model.GetViewState().Center.x > x);
	fixture.Release();
}
