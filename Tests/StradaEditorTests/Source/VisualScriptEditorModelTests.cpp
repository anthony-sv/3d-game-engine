#include "TestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
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
#include <filesystem>
#include <optional>
#include <string>
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

		// A commit as the panel makes it: the asset takes the document object, the model is told, and syncs again in the frame.
		VisualScriptSyncResult Commit(VisualScriptDocument document)
		{
			Ref<VisualScriptDocument const> const committed = CreateRef<VisualScriptDocument const>(std::move(document));
			GetAsset()->SetDocument(committed);
			Model.OnCommitted(committed);
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

	// The runtime stops: no analysis, none reported; the last one keeps its pins without its problems.
	VisualScriptDiagnostic error;
	error.Code = "InvalidValue";
	error.Node = 2;
	error.Message = "an error";
	fixture.Semantics.InjectDiagnostic(error);
	document.Graph.Nodes[1].Inputs["Duration"] = 3;
	fixture.Commit(document);
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 5);
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
	CHECK(fixture.Model.GetAnalysisCount() == 5);
	CHECK(fixture.Semantics.GetAnalyzeCallCount() == 5);
	// A changed literal keeps the kept analysis' pins: the node's input keys are those analyzed.
	CHECK(fixture.Node(2).HasFlag(VisualScriptNodeFlags::Analyzed));

	// The runtime starts again: analyzed at once.
	fixture.Semantics.SetAvailable(true);
	fixture.Frame();
	CHECK(fixture.Model.GetAnalysisCount() == 6);
	CHECK(fixture.Model.GetAnalysis() != nullptr);
	CHECK(fixture.Node(2).ErrorCount == 1);
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

	size_t const capacity = model.GetCapacityBytes();
	for (int frame = 1; frame <= 10; frame++)
	{
		VisualScriptSyncResult const idle = model.Sync(semantics, measure, VisualScriptThemeAnchors(), false, frame / 60.0);
		CHECK_FALSE(idle.DocumentChanged);
		CHECK_FALSE(idle.AnalysisChanged);
		CHECK_FALSE(idle.GeometryChanged);
		CHECK(model.GetCapacityBytes() == capacity);
	}
	// The failed analysis is not asked for again.
	CHECK(model.GetAnalysisCount() == 1);
}
