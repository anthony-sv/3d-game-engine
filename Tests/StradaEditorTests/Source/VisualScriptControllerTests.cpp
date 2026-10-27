#include "TestUtilities.h"
#include "VisualScriptPanelTestUtilities.h"
#include "VisualScriptTestDocuments.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/VisualScripting/VisualScriptClipboard.h"
#include "Editor/VisualScripting/VisualScriptController.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptField.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	using Action = VisualScriptEditorAction;

	VisualScriptNode MakeNode(uint32_t id, std::string type, glm::vec2 position, Json settings = Json::object(),
	                          Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = position;
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t fromNode, std::string fromPin, uint32_t toNode, std::string toPin)
	{
		return {{fromNode, std::move(fromPin)}, {toNode, std::move(toPin)}};
	}

	// The script "Player": Create (1) -> Delay (2) -> Log Info (3) in a row; Get Speed (5) and an Add (6) below; the custom
	// event Hit (7) with a sender (8) and a call of AddScore (9) in a third row; the comment box "Start" (30) around Create and
	// Delay. Speed (10) is a public float; AddScore (20) has an Entry (21) linked to its Return (22).
	VisualScriptDocument MakeDocument()
	{
		VisualScriptDocument document;
		VisualScriptVariable speed;
		speed.ID = 10;
		speed.Name = "Speed";
		speed.Type = "float";
		speed.Public = true;
		document.Variables.push_back(speed);
		document.Graph.Nodes = {
			MakeNode(1, "Event.Create", glm::vec2(0.0f, 0.0f)),
			MakeNode(2, "Flow.Delay", glm::vec2(400.0f, 0.0f), Json::object(), Json::object({{"Duration", 1.5}})),
			MakeNode(3, "Call:Strada.Log.Info(string)", glm::vec2(800.0f, 0.0f), Json::object(), Json::object({{"Message", "Hi"}})),
			MakeNode(5, "Variable.Get", glm::vec2(400.0f, 320.0f), Json::object({{"Variable", 10}})),
			MakeNode(6, "Operator.Add", glm::vec2(640.0f, 320.0f)),
			MakeNode(7, "Event.Custom", glm::vec2(0.0f, 640.0f), Json::object({{"Name", "Hit"}})),
			MakeNode(8, "Event.Send", glm::vec2(400.0f, 640.0f), Json::object({{"Name", "Hit"}})),
			MakeNode(9, "Function.Call", glm::vec2(800.0f, 640.0f), Json::object({{"Function", 20}})),
		};
		document.Graph.Links = {MakeLink(1, "Then", 2, "Execute"), MakeLink(2, "Completed", 3, "Execute"), MakeLink(5, "Value", 6, "A"),
		                        MakeLink(7, "Then", 8, "Execute"), MakeLink(8, "Then", 9, "Execute")};
		VisualScriptComment start;
		start.ID = 30;
		start.Text = "Start\nWhere it begins";
		start.Position = glm::vec2(-48.0f, -80.0f);
		start.Size = glm::vec2(720.0f, 240.0f);
		document.Graph.Comments = {start};
		VisualScriptFunction addScore;
		addScore.ID = 20;
		addScore.Name = "AddScore";
		addScore.Graph.Nodes = {MakeNode(21, "Function.Entry", glm::vec2(0.0f)), MakeNode(22, "Function.Return", glm::vec2(480.0f, 0.0f))};
		addScore.Graph.Links = {MakeLink(21, "Then", 22, "Execute")};
		document.Functions.push_back(addScore);
		return document;
	}

	// A graph window's model of the project's script "Player", with the editor's operations and history: what the panel holds
	// when it hands intents to its controller and executes the requests.
	struct ControllerFixture
	{
		explicit ControllerFixture(VisualScriptDocument const& document = MakeDocument(), bool memoryAsset = false)
			: Settings(Temporary.GetPath() / "Views.json")
		{
			REQUIRE(Operations.CreateProject(Temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			File = AssetManager::GetAssetDirectory() / "Scripts" / "Player.sgraph";
			REQUIRE(FileSystem::CreateDirectories(File.parent_path()).IsOk());
			REQUIRE(VisualScriptSerializer::SaveToFile(document, File).IsOk());
			REQUIRE(Operations.RefreshAssets().IsOk());
			Graph = memoryAsset ? AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(document), "Generated")
			                    : AssetManager::FindByPath("Scripts/Player.sgraph");
			REQUIRE(Graph.IsValid());
			Model = CreateScope<VisualScriptEditorModel>(Graph, Settings);
			Frame();
		}

		// A frame of the window: the model's Sync, then its Update on a canvas nothing hovers.
		void Frame()
		{
			Time += 1.0 / 60.0;
			Model->Sync(SemanticsOverride != nullptr ? *SemanticsOverride : Semantics, Measure, Theme, false, Time);
			VisualScriptCanvasInput input;
			input.CanvasSize = glm::vec2(1600.0f, 900.0f);
			input.Time = Time;
			Model->Update(input);
		}

		void Select(std::vector<uint32_t> const& elements)
		{
			Model->Select(elements);
			Frame();
		}

		VisualScriptEditContext Context() const
		{
			VisualScriptEditContext context(*Model);
			context.Library = Semantics.IsAvailable() ? &Semantics.GetFixtureLibrary() : nullptr;
			context.ScriptName = "Player";
			context.Graph = Graph;
			return context;
		}

		VisualScriptControllerResult Resolve(VisualScriptEditorIntent const& intent, VisualScriptEditContext const& context) const
		{
			Result<VisualScriptControllerResult> result = ResolveVisualScriptIntent(intent, context);
			REQUIRE_MESSAGE(result.IsOk(), (result ? std::string() : result.GetError()));
			return result.TakeValue();
		}

		VisualScriptControllerResult Resolve(VisualScriptEditorIntent const& intent) const { return Resolve(intent, Context()); }

		VisualScriptControllerResult Edit(VisualScriptCanvasEdit const& edit, VisualScriptEditContext const& context) const
		{
			Result<VisualScriptControllerResult> result = ResolveVisualScriptEdit(edit, context);
			REQUIRE_MESSAGE(result.IsOk(), (result ? std::string() : result.GetError()));
			return result.TakeValue();
		}

		VisualScriptControllerResult Edit(VisualScriptCanvasEdit const& edit) const { return Edit(edit, Context()); }

		// An action on a selection (the primary element first), as keys and the node menu send it.
		VisualScriptControllerResult Act(Action action, std::vector<uint32_t> selection, glm::vec2 position = glm::vec2(0.0f)) const
		{
			return Resolve(VisualScriptActionIntent{action, std::move(selection), position});
		}

		// Executes the result's step as the panel does, and syncs the model with it.
		void Execute(VisualScriptControllerResult& result)
		{
			REQUIRE(result.Commit.has_value());
			VisualScriptCommitRequest& request = *result.Commit;
			Result<void> const executed =
				result.ComponentEdits.empty()
					? Operations.EditVisualScript(Graph, request.Document, request.Description, request.MergeKey, std::move(request.Text))
					: Operations.EditVisualScriptAndComponents(Graph, request.Document, request.Description, result.ComponentEdits,
			                                                   std::move(request.Text));
			REQUIRE_MESSAGE(executed.IsOk(), (executed ? std::string() : executed.GetError()));
			Model->OnCommitted(request.Document);
			Frame();
		}

		VisualScriptDocument const& Stored() const { return AssetManager::GetAsset<VisualScriptAsset>(Graph)->GetDocument(); }

		VisualScriptNode const* Node(uint32_t id) const
		{
			for (VisualScriptNode const& node : Stored().Graph.Nodes)
			{
				if (node.ID == id)
				{
					return &node;
				}
			}
			return nullptr;
		}

		bool HasLink(VisualScriptLink const& link) const
		{
			std::vector<VisualScriptLink> const& links = Stored().Graph.Links;
			return std::find(links.begin(), links.end(), link) != links.end();
		}

		CommandHistory const& History() const { return Context_.GetHistory(); }

		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Temporary;
		EditorContext Context_;
		EditorOperations Operations{Context_};
		std::filesystem::path File;
		AssetHandle Graph;
		VisualScriptViewSettings Settings;
		Testing::FixtureVisualScriptSemantics Semantics;
		// Analyzes instead of Semantics when set.
		VisualScriptSemantics* SemanticsOverride = nullptr;
		VisualScriptTextMeasurer Measure = Testing::FixedWidthMeasurer(0.5f);
		VisualScriptThemeAnchors Theme;
		Scope<VisualScriptEditorModel> Model;
		double Time = 0.0;
	};

	std::string Description(VisualScriptControllerResult const& result)
	{
		REQUIRE(result.Commit.has_value());
		return result.Commit->Description;
	}

	bool Contains(std::vector<std::string> const& notes, std::string_view note)
	{
		return std::find(notes.begin(), notes.end(), note) != notes.end();
	}
}

TEST_CASE("VisualScriptEditor: canvas gestures each make one step named for what they did, never merged with another")
{
	ControllerFixture fixture;
	VisualScriptEditContext context = fixture.Context();
	// A merge key in the context is for the runs that merge: a gesture is a step of its own.
	context.MergeKey = 0x1234;

	SUBCASE("a move")
	{
		VisualScriptControllerResult result = fixture.Resolve(VisualScriptMoveIntent{{2, 3}, glm::vec2(32.0f, 16.0f), true}, context);
		CHECK(Description(result) == "Move 2 Nodes in Player");
		CHECK(result.Commit->MergeKey == 0);
		Testing::ExpectStrictRoundTrip(*result.Commit->Document);
		fixture.Execute(result);
		CHECK(fixture.History().GetUndoCount() == 1);
		CHECK(fixture.Node(2)->Position == glm::vec2(432.0f, 16.0f));
		CHECK(fixture.Resolve(VisualScriptMoveIntent{{30}, glm::vec2(16.0f, 0.0f), true}).Commit->Description == "Move Comment in Player");
		CHECK(fixture.Resolve(VisualScriptMoveIntent{{30, 1, 2}, glm::vec2(16.0f, 0.0f), true}).Commit->Description ==
		      "Move 2 Nodes and 1 Comment in Player");
	}
	SUBCASE("a connection, and one that replaces the input's link")
	{
		VisualScriptControllerResult result = fixture.Resolve(VisualScriptConnectIntent{MakeLink(5, "Value", 6, "B"), false, {}});
		CHECK(Description(result) == "Connect Value to B in Player");
		fixture.Execute(result);
		CHECK(fixture.HasLink(MakeLink(5, "Value", 6, "B")));
		VisualScriptControllerResult replacing = fixture.Resolve(VisualScriptConnectIntent{MakeLink(1, "Then", 3, "Execute"), false, {}});
		CHECK(Description(replacing) == "Connect Then to Execute (replaces 1 link) in Player");
		fixture.Execute(replacing);
		CHECK_FALSE(fixture.HasLink(MakeLink(1, "Then", 2, "Execute")));
	}
	SUBCASE("a connection through a Convert")
	{
		VisualScriptControllerResult result =
			fixture.Resolve(VisualScriptConnectIntent{MakeLink(5, "Value", 3, "Message"), true, "string"});
		CHECK(Description(result) == "Connect Value to Message (Convert) in Player");
		fixture.Execute(result);
		CHECK(fixture.History().GetUndoCount() == 1);
		VisualScriptNode const* convert = fixture.Node(31);
		REQUIRE(convert != nullptr);
		CHECK(convert->Type == "Convert");
		CHECK(convert->Settings["To"] == "string");
		CHECK(fixture.HasLink(MakeLink(5, "Value", 31, "Value")));
		CHECK(fixture.HasLink(MakeLink(31, "Result", 3, "Message")));
	}
	SUBCASE("links moved, broken and cut")
	{
		VisualScriptControllerResult moved =
			fixture.Resolve(VisualScriptRelinkIntent{{MakeLink(1, "Then", 2, "Execute")}, {MakeLink(1, "Then", 3, "Execute")}});
		CHECK(Description(moved) == "Move Link in Player");
		CHECK(fixture.Resolve(VisualScriptDisconnectIntent{{MakeLink(1, "Then", 2, "Execute")}}).Commit->Description ==
		      "Break Link in Player");
		CHECK(fixture.Resolve(VisualScriptDisconnectIntent{{MakeLink(7, "Then", 8, "Execute"), MakeLink(8, "Then", 9, "Execute")}})
		          .Commit->Description == "Cut 2 Links in Player");
		VisualScriptControllerResult cut =
			fixture.Resolve(VisualScriptDisconnectIntent{{MakeLink(1, "Then", 2, "Execute"), MakeLink(2, "Completed", 3, "Execute")}});
		fixture.Execute(cut);
		CHECK(fixture.Stored().Graph.Links.size() == 3);
		CHECK(fixture.History().GetUndoDescription() == "Cut 2 Links in Player");
	}
	SUBCASE("a comment resized, a reroute added, a node spliced into a wire")
	{
		VisualScriptControllerResult resized =
			fixture.Resolve(VisualScriptResizeCommentIntent{30, {glm::vec2(-48.0f, -80.0f), glm::vec2(800.0f, 240.0f)}});
		CHECK(Description(resized) == "Resize Comment in Player");
		VisualScriptControllerResult reroute =
			fixture.Resolve(VisualScriptInsertRerouteIntent{MakeLink(1, "Then", 2, "Execute"), glm::vec2(160.0f, 32.0f)});
		CHECK(Description(reroute) == "Add Reroute in Player");
		fixture.Execute(reroute);
		CHECK(fixture.Node(31)->Type == "Reroute");
		CHECK(fixture.HasLink(MakeLink(31, "Out", 2, "Execute")));

		// Log Info alone (its links broken) dropped on the wire from Get Speed's reroute... the wire Delay -> nothing: unlink it
		// first, then splice it between Create's reroute and Delay.
		VisualScriptControllerResult freed = fixture.Resolve(VisualScriptDisconnectIntent{{MakeLink(2, "Completed", 3, "Execute")}});
		fixture.Execute(freed);
		VisualScriptControllerResult spliced =
			fixture.Resolve(VisualScriptSpliceIntent{3, MakeLink(31, "Out", 2, "Execute"), "Execute", "Then", glm::vec2(-480.0f, 160.0f)});
		CHECK(Description(spliced) == "Insert Info in Player");
		fixture.Execute(spliced);
		CHECK(fixture.HasLink(MakeLink(31, "Out", 3, "Execute")));
		CHECK(fixture.HasLink(MakeLink(3, "Then", 2, "Execute")));
		CHECK(fixture.Node(3)->Position == glm::vec2(320.0f, 160.0f));
		CHECK(fixture.History().GetUndoCount() == 3);
	}
}

TEST_CASE("VisualScriptEditor: menus, the palette and sub-graph switches pass through the controller without a step")
{
	ControllerFixture fixture;
	VisualScriptPaletteIntent palette;
	palette.Position = glm::vec2(64.0f, 32.0f);
	palette.Query = "bra";
	VisualScriptControllerResult opened = fixture.Resolve(palette);
	CHECK_FALSE(opened.Commit.has_value());
	REQUIRE(opened.OpenPalette.has_value());
	CHECK(opened.OpenPalette->Query == "bra");

	VisualScriptContextMenuIntent menu;
	menu.Target.Kind = VisualScriptHitKind::Node;
	menu.Target.Element = 2;
	CHECK(fixture.Resolve(menu).ContextMenu.has_value());
	CHECK(fixture.Resolve(VisualScriptPinMenuIntent{2, VisualScriptPinDirection::In, glm::vec2(0.0f)}).PinMenu.has_value());
	CHECK(fixture.Resolve(VisualScriptOpenFunctionIntent{20}).ShowFunction == std::optional<uint32_t>(20));
	CHECK(fixture.History().GetUndoCount() == 0);
}

TEST_CASE("VisualScriptEditor: Copy and Cut put the selection's clipboard text on the clipboard, and Paste selects what it adds")
{
	ControllerFixture fixture;
	std::string const expected = WriteVisualScriptClipboard(fixture.Model->GetDisplayDocument(), 0, std::vector<uint32_t>{2, 3},
	                                                        fixture.Graph, &fixture.Semantics.GetFixtureLibrary());

	VisualScriptControllerResult copied = fixture.Act(Action::Copy, {2, 3});
	CHECK_FALSE(copied.Commit.has_value());
	CHECK(copied.Clipboard == std::optional<std::string>(expected));
	CHECK_FALSE(fixture.Act(Action::Copy, {}).Clipboard.has_value());

	VisualScriptControllerResult cut = fixture.Act(Action::Cut, {2, 3});
	CHECK(cut.Clipboard == std::optional<std::string>(expected));
	CHECK(Description(cut) == "Cut 2 Nodes in Player");
	fixture.Execute(cut);
	CHECK(fixture.Node(2) == nullptr);
	CHECK(fixture.History().GetUndoCount() == 1);

	VisualScriptEditContext context = fixture.Context();
	context.ClipboardText = expected;
	VisualScriptControllerResult pasted = fixture.Resolve(VisualScriptActionIntent{Action::Paste, {}, glm::vec2(1203.0f, 405.0f)}, context);
	CHECK(Description(pasted) == "Paste 2 Nodes in Player");
	CHECK(pasted.Commit->Select == std::vector<uint32_t>{31, 32});
	CHECK(pasted.Commit->MergeKey == 0);
	fixture.Execute(pasted);
	// At the cursor, on the grid.
	CHECK(fixture.Node(31)->Position == glm::vec2(1200.0f, 400.0f));
	CHECK(fixture.History().GetUndoCount() == 2);

	context.ClipboardText = "{ \"some\": \"other text\" }";
	VisualScriptControllerResult refused = fixture.Resolve(VisualScriptActionIntent{Action::Paste, {}, glm::vec2(0.0f)}, context);
	CHECK_FALSE(refused.Commit.has_value());
	CHECK(Contains(refused.Notes, VisualScriptRefusals::NoClipboard));

	VisualScriptControllerResult duplicated = fixture.Act(Action::Duplicate, {31});
	CHECK(Description(duplicated) == "Duplicate Node in Player");
	CHECK(duplicated.Commit->Select == std::vector<uint32_t>{33});
}

TEST_CASE("VisualScriptEditor: a paste that adds a variable says so in one toast")
{
	ControllerFixture fixture;
	VisualScriptDocument other;
	VisualScriptVariable score;
	score.ID = 1;
	score.Name = "Score";
	score.Type = "int";
	other.Variables.push_back(score);
	other.Graph.Nodes = {MakeNode(2, "Variable.Get", glm::vec2(0.0f), Json::object({{"Variable", 1}}))};
	std::string const text =
		WriteVisualScriptClipboard(other, 0, std::vector<uint32_t>{2}, AssetHandle(UUID(42)), &fixture.Semantics.GetFixtureLibrary());
	VisualScriptEditContext context = fixture.Context();
	context.ClipboardText = text;
	VisualScriptControllerResult pasted = fixture.Resolve(VisualScriptActionIntent{Action::Paste, {}, glm::vec2(0.0f)}, context);
	CHECK(Description(pasted) == "Paste Node in Player");
	CHECK(pasted.Commit->Notes == std::vector<std::string>{"Pasted 1 node; added variable Score (int)"});
}

TEST_CASE("VisualScriptEditor: delete bridges the flow, and deleting without relinking does not")
{
	ControllerFixture fixture;
	VisualScriptControllerResult bridged = fixture.Act(Action::Delete, {2});
	CHECK(Description(bridged) == "Delete Node in Player");
	VisualScriptControllerResult unbridged = fixture.Act(Action::DeleteWithoutRelink, {2});
	CHECK(Description(unbridged) == "Delete Node in Player");
	fixture.Execute(bridged);
	CHECK(fixture.HasLink(MakeLink(1, "Then", 3, "Execute")));

	CHECK(fixture.Act(Action::Delete, {30}).Commit->Description == "Delete Comment in Player");
	CHECK(fixture.Act(Action::Delete, {1, 3, 30}).Commit->Description == "Delete 2 Nodes and 1 Comment in Player");

	// A function's entry stays, and says why.
	fixture.Model->ShowFunction(20);
	fixture.Frame();
	VisualScriptControllerResult entry = fixture.Act(Action::Delete, {21});
	CHECK_FALSE(entry.Commit.has_value());
	REQUIRE(entry.Notes.size() == 1);
	CHECK(entry.Notes.front().find("AddScore") != std::string::npos);
}

TEST_CASE("VisualScriptEditor: nudges merge into the run's step, and a comment box opens its title in the step that adds it")
{
	ControllerFixture fixture;
	VisualScriptEditContext context = fixture.Context();
	context.MergeKey = VisualScriptNudgeMergeKey + 1;
	for (int press = 0; press < 3; press++)
	{
		VisualScriptControllerResult nudged = fixture.Resolve(
			VisualScriptActionIntent{press == 2 ? Action::NudgeDownFine : Action::NudgeRight, {2}, glm::vec2(0.0f)}, context);
		CHECK(Description(nudged) == "Nudge Node in Player");
		CHECK(nudged.Commit->MergeKey == VisualScriptNudgeMergeKey + 1);
		fixture.Execute(nudged);
	}
	CHECK(fixture.History().GetUndoCount() == 1);
	CHECK(fixture.Node(2)->Position == glm::vec2(432.0f, 1.0f));

	VisualScriptEditContext naming = fixture.Context();
	naming.MergeKey = VisualScriptNameMergeKey + 1;
	naming.CommentColor = glm::vec4(0.2f, 0.4f, 0.6f, 0.3f);
	VisualScriptControllerResult added = fixture.Resolve(VisualScriptActionIntent{Action::AddCommentBox, {5, 6}, glm::vec2(0.0f)}, naming);
	CHECK(Description(added) == "Add Comment Box in Player");
	CHECK(added.Commit->MergeKey == VisualScriptNameMergeKey + 1);
	CHECK(added.Commit->Select == std::vector<uint32_t>{31});
	REQUIRE(added.EditText.has_value());
	CHECK(added.EditText->Kind == VisualScriptTextEditKind::CommentTitle);
	CHECK(added.EditText->Element == 31);
	CHECK(added.EditText->Text == "Comment");
	Ref<VisualScriptDocument const> const before = fixture.Model->GetStoredDocument();
	fixture.Execute(added);

	// The title typed into that field joins the same step.
	naming.Stored = before.get();
	VisualScriptControllerResult titled =
		fixture.Edit(VisualScriptCommentEdit{VisualScriptCommentAction::Rename, 31, "Speed math", glm::vec4(0.0f)}, naming);
	CHECK(Description(titled) == "Rename Comment in Player");
	fixture.Execute(titled);
	CHECK(fixture.History().GetUndoCount() == 2);
	CHECK(fixture.History().GetUndoDescription() == "Add Comment Box in Player");
	VisualScriptComment const& comment = fixture.Stored().Graph.Comments.back();
	CHECK(comment.Text == "Speed math");
	CHECK(comment.Color == glm::vec4(0.2f, 0.4f, 0.6f, 0.3f));
}

TEST_CASE("VisualScriptEditor: align, distribute, straighten and arrange name what they moved, and arrange gives the glide its start")
{
	ControllerFixture fixture;
	CHECK(fixture.Act(Action::AlignTop, {5, 6, 3}).Commit->Description == "Align 3 Nodes Top in Player");
	CHECK(fixture.Act(Action::AlignLeft, {1, 30}).Commit->Description == "Align 1 Node and 1 Comment Left in Player");
	VisualScriptControllerResult single = fixture.Act(Action::AlignLeft, {2});
	CHECK_FALSE(single.Commit.has_value());
	CHECK(Contains(single.Notes, "Select two or more nodes to align them"));
	VisualScriptControllerResult pair = fixture.Act(Action::DistributeHorizontally, {1, 2});
	CHECK_FALSE(pair.Commit.has_value());
	CHECK(Contains(pair.Notes, "Select three or more nodes to distribute them"));
	CHECK(fixture.Act(Action::DistributeVertically, {2, 5, 8}).Commit->Description == "Distribute 3 Nodes in Player");

	VisualScriptControllerResult arranged = fixture.Act(Action::Arrange, {});
	CHECK(Description(arranged) == "Arrange 8 Nodes in Player");
	CHECK(arranged.GlideFrom.size() == 8);
	CHECK(std::find(arranged.GlideFrom.begin(), arranged.GlideFrom.end(), std::pair<uint32_t, glm::vec2>(2, glm::vec2(400.0f, 0.0f))) !=
	      arranged.GlideFrom.end());
	VisualScriptControllerResult selection = fixture.Act(Action::Arrange, {5, 6});
	CHECK(Description(selection) == "Arrange 2 Nodes in Player");
	CHECK(selection.GlideFrom.size() == 2);
}

TEST_CASE("VisualScriptEditor: collapsing, disabling and node comments name their nodes, and an empty node comment commits nothing")
{
	ControllerFixture fixture;
	VisualScriptControllerResult collapsed = fixture.Act(Action::ToggleCollapsed, {2});
	CHECK(Description(collapsed) == "Collapse Delay in Player");
	fixture.Execute(collapsed);
	CHECK(fixture.Act(Action::ToggleCollapsed, {2}).Commit->Description == "Expand Delay in Player");
	CHECK(fixture.Act(Action::ToggleCollapsed, {2, 3, 8}).Commit->Description == "Collapse 3 Nodes in Player");

	VisualScriptControllerResult disabled = fixture.Act(Action::ToggleDisabled, {2});
	CHECK(Description(disabled) == "Disable Delay in Player");
	fixture.Execute(disabled);
	CHECK(fixture.Node(2)->Enabled == VisualScriptNodeEnabled::Disabled);
	CHECK(fixture.Act(Action::ToggleDisabled, {2}).Commit->Description == "Enable Delay in Player");
	CHECK(fixture.Act(Action::SetDevelopmentOnly, {2, 3}).Commit->Description == "Set 2 Nodes to Development Only in Player");
	// Pure nodes and a function's ends are left out, and alone they say why.
	CHECK(fixture.Act(Action::SetDisabled, {3, 6}).Commit->Description == "Disable Info in Player");
	VisualScriptControllerResult pure = fixture.Act(Action::SetDisabled, {6});
	CHECK_FALSE(pure.Commit.has_value());
	CHECK(Contains(pure.Notes, VisualScriptPureNodesRefusal));

	VisualScriptControllerResult opened = fixture.Act(Action::AddNodeComment, {2});
	CHECK_FALSE(opened.Commit.has_value());
	REQUIRE(opened.EditText.has_value());
	CHECK(opened.EditText->Kind == VisualScriptTextEditKind::NodeComment);
	CHECK(opened.EditText->Element == 2);
	CHECK(opened.EditText->Text.empty());
	VisualScriptControllerResult empty = fixture.Edit(VisualScriptNodeCommentEdit{2, ""});
	CHECK_FALSE(empty.Commit.has_value());
	CHECK(empty.Notes.empty());

	VisualScriptControllerResult commented = fixture.Edit(VisualScriptNodeCommentEdit{2, "Waits for the door"});
	CHECK(Description(commented) == "Comment Delay in Player");
	fixture.Execute(commented);
	CHECK(fixture.Act(Action::EditNodeComment, {2}).EditText->Text == "Waits for the door");
	CHECK(fixture.Edit(VisualScriptNodeCommentEdit{2, "Waits"}).Commit->Description == "Edit Comment of Delay in Player");
	CHECK(fixture.Edit(VisualScriptNodeCommentEdit{2, ""}).Commit->Description == "Remove Comment of Delay in Player");
	VisualScriptControllerResult removed = fixture.Act(Action::RemoveNodeComment, {2, 3});
	CHECK(Description(removed) == "Remove Comment of Delay in Player");
	fixture.Execute(removed);
	CHECK(fixture.Node(2)->Comment.empty());
	CHECK(fixture.History().GetUndoCount() == 4);
}

TEST_CASE("VisualScriptEditor: values, pins and comment boxes edited from the window's menus and fields")
{
	ControllerFixture fixture;
	VisualScriptEditContext context = fixture.Context();
	context.MergeKey = 0x5653454400000007ull;
	VisualScriptControllerResult value = fixture.Edit(VisualScriptLiteralEdit{{2, "Duration"}, 2.5}, context);
	CHECK(Description(value) == "Set Duration on Delay in Player");
	CHECK(value.Commit->MergeKey == 0x5653454400000007ull);
	fixture.Execute(value);
	CHECK(fixture.Node(2)->Inputs["Duration"] == 2.5);
	VisualScriptControllerResult reset =
		fixture.Edit(VisualScriptPinEdit{VisualScriptPinAction::ResetToDefault, {2, "Duration"}, VisualScriptPinDirection::In, false, {}});
	CHECK(Description(reset) == "Reset Duration on Delay in Player");
	fixture.Execute(reset);
	CHECK_FALSE(fixture.Node(2)->Inputs.contains("Duration"));

	CHECK(fixture.Edit(VisualScriptPinEdit{VisualScriptPinAction::BreakAllLinks, {2, "Execute"}, VisualScriptPinDirection::In, false, {}})
	          .Commit->Description == "Break Link in Player");
	CHECK(fixture.Edit(VisualScriptPinEdit{VisualScriptPinAction::BreakAllLinks, {2, "Execute"}, VisualScriptPinDirection::In, true, {}})
	          .Commit->Description == "Break Link in Player");
	VisualScriptControllerResult promoted = fixture.Edit(
		VisualScriptPinEdit{VisualScriptPinAction::PromoteToVariable, {3, "Message"}, VisualScriptPinDirection::In, false, {}});
	CHECK(Description(promoted) == "Promote Message to Variable in Player");
	REQUIRE(promoted.Reveal.has_value());
	CHECK(promoted.Reveal->Kind == VisualScriptRevealKind::Variable);
	CHECK(promoted.Reveal->Rename);

	VisualScriptControllerResult recoloured =
		fixture.Edit(VisualScriptCommentEdit{VisualScriptCommentAction::Recolour, 30, {}, glm::vec4(0.1f, 0.2f, 0.3f, 0.4f)});
	CHECK(Description(recoloured) == "Recolour Comment in Player");
	VisualScriptControllerResult renamed = fixture.Edit(VisualScriptCommentEdit{VisualScriptCommentAction::Rename, 30, "Begin", {}});
	fixture.Execute(renamed);
	CHECK(fixture.Stored().Graph.Comments.front().Text == "Begin\nWhere it begins");
	CHECK(fixture.Edit(VisualScriptCommentEdit{VisualScriptCommentAction::FitToContents, 30, {}, {}}).Commit->Description ==
	      "Fit Comment to Contents in Player");
	CHECK(fixture.Edit(VisualScriptCommentEdit{VisualScriptCommentAction::Delete, 30, {}, {}}).Commit->Description ==
	      "Delete Comment in Player");
	VisualScriptControllerResult contents =
		fixture.Edit(VisualScriptCommentEdit{VisualScriptCommentAction::DeleteWithContents, 30, {}, {}});
	CHECK(Description(contents) == "Delete Comment and 2 Nodes in Player");
	fixture.Execute(contents);
	CHECK(fixture.Stored().Graph.Comments.empty());
	CHECK(fixture.Node(1) == nullptr);
	CHECK(fixture.Node(2) == nullptr);

	VisualScriptControllerResult update = fixture.Edit(VisualScriptAddNodeEdit{"Event.Update", Json::object(), glm::vec2(0.0f, 960.0f)});
	CHECK(Description(update) == "Add Update in Player");
	// The comment box and its contents are gone: the largest ID left is AddScore's return (22).
	CHECK(update.Commit->Select == std::vector<uint32_t>{23});
}

TEST_CASE("VisualScriptEditor: opening a node, finding its references and renaming go where the node points")
{
	ControllerFixture fixture;
	CHECK(fixture.Act(Action::OpenNode, {9}).ShowFunction == std::optional<uint32_t>(20));
	VisualScriptControllerResult sender = fixture.Act(Action::OpenNode, {8});
	REQUIRE(sender.Open.has_value());
	CHECK(sender.Open->Select == std::vector<uint32_t>{7});
	CHECK(sender.Open->Frame == VisualScriptFrame::Selection);
	VisualScriptControllerResult variable = fixture.Act(Action::OpenNode, {5});
	REQUIRE(variable.Reveal.has_value());
	CHECK(variable.Reveal->Kind == VisualScriptRevealKind::Variable);
	CHECK(variable.Reveal->ID == 10);
	VisualScriptControllerResult details = fixture.Act(Action::OpenNode, {2});
	REQUIRE(details.Reveal.has_value());
	CHECK(details.Reveal->Kind == VisualScriptRevealKind::Node);

	CHECK(fixture.Act(Action::FindReferences, {5}).FindReferences ==
	      std::optional<VisualScriptReference>(VisualScriptReference{VisualScriptReference::Kind::Variable, 10, {}}));
	CHECK(fixture.Act(Action::FindReferences, {8}).FindReferences ==
	      std::optional<VisualScriptReference>(VisualScriptReference{VisualScriptReference::Kind::CustomEvent, 0, "Hit"}));
	CHECK_FALSE(fixture.Act(Action::FindReferences, {2}).FindReferences.has_value());

	VisualScriptControllerResult comment = fixture.Act(Action::Rename, {30});
	REQUIRE(comment.EditText.has_value());
	CHECK(comment.EditText->Text == "Start");
	VisualScriptControllerResult event = fixture.Act(Action::Rename, {7});
	REQUIRE(event.Reveal.has_value());
	CHECK(event.Reveal->Kind == VisualScriptRevealKind::CustomEvent);
	CHECK(event.Reveal->Rename);
	CHECK(fixture.History().GetUndoCount() == 0);
}

TEST_CASE("VisualScriptEditor: without the scripting runtime, edits that need node details are refused and the others commit")
{
	ControllerFixture fixture;
	fixture.Semantics.SetAvailable(false);
	fixture.Frame();
	REQUIRE(fixture.Model->GetAnalysis() == nullptr);

	VisualScriptControllerResult connect = fixture.Resolve(VisualScriptConnectIntent{MakeLink(5, "Value", 6, "B"), false, {}});
	CHECK_FALSE(connect.Commit.has_value());
	CHECK(connect.NeedsRuntime);
	CHECK(Contains(connect.Notes, VisualScriptRefusals::Degraded));
	CHECK(fixture.Act(Action::ToggleDisabled, {2}).NeedsRuntime);
	CHECK(fixture.Act(Action::Straighten, {1, 2}).NeedsRuntime);
	CHECK(fixture.Edit(VisualScriptPinEdit{VisualScriptPinAction::Split, {6, "A"}, VisualScriptPinDirection::In, false, {}}).NeedsRuntime);

	CHECK(fixture.Resolve(VisualScriptMoveIntent{{2}, glm::vec2(16.0f), true}).Commit.has_value());
	CHECK(fixture.Act(Action::ToggleCollapsed, {2}).Commit.has_value());
	VisualScriptControllerResult deleted = fixture.Act(Action::Delete, {2});
	CHECK(Description(deleted) == "Delete Node in Player");
	CHECK(Contains(deleted.Commit->Notes, VisualScriptRefusals::NotBridged));
	fixture.Execute(deleted);
	CHECK_FALSE(fixture.HasLink(MakeLink(1, "Then", 3, "Execute")));
}

TEST_CASE("VisualScriptEditor: read-only graphs refuse every edit with their notice's text and still copy")
{
	SUBCASE("a memory asset")
	{
		ControllerFixture fixture(MakeDocument(), true);
		REQUIRE(fixture.Model->GetDocumentState() == VisualScriptDocumentState::ReadOnly);
		VisualScriptControllerResult moved = fixture.Resolve(VisualScriptMoveIntent{{2}, glm::vec2(16.0f), true});
		CHECK_FALSE(moved.Commit.has_value());
		CHECK(moved.Notes == std::vector<std::string>{std::string(VisualScriptRefusals::ReadOnlyMemory)});
		CHECK_FALSE(fixture.Edit(VisualScriptNodeCommentEdit{2, "Note"}).Commit.has_value());
		CHECK_FALSE(fixture.Act(Action::Delete, {2}).Commit.has_value());
		CHECK(fixture.Act(Action::Copy, {2}).Clipboard.has_value());
		VisualScriptControllerResult cut = fixture.Act(Action::Cut, {2});
		CHECK(cut.Clipboard.has_value());
		CHECK_FALSE(cut.Commit.has_value());
	}
	SUBCASE("a file without write permission")
	{
		ControllerFixture fixture;
		Testing::ReadOnlyFileScope const readOnly(fixture.File);
		fixture.Model->CheckWritable();
		fixture.Frame();
		REQUIRE(fixture.Model->GetDocumentState() == VisualScriptDocumentState::ReadOnlyFile);
		VisualScriptControllerResult moved = fixture.Resolve(VisualScriptMoveIntent{{2}, glm::vec2(16.0f), true});
		CHECK_FALSE(moved.Commit.has_value());
		CHECK(moved.Notes == std::vector<std::string>{"Player.sgraph is read-only on disk: edits cannot be saved."});
		// The node menu still opens, copies and finds; its edits say why they are off.
		fixture.Select({5});
		std::vector<VisualScriptMenuItem> const menu = BuildVisualScriptNodeMenu(fixture.Context());
		REQUIRE_FALSE(menu.empty());
		for (VisualScriptMenuItem const& item : menu)
		{
			CAPTURE(item.Label);
			bool const views = item.Action == Action::OpenNode || item.Action == Action::Copy || item.Action == Action::FindReferences;
			CHECK(item.Enabled == views);
			if (!views)
			{
				CHECK(item.Reason == "Player.sgraph is read-only on disk: edits cannot be saved.");
			}
		}
	}
}

TEST_CASE(
	"VisualScriptEditor: an entity dropped on the canvas becomes a public variable and its Get, set on the context entity in the same step")
{
	ControllerFixture fixture;
	EntityCreateInfo info;
	info.Name = "Door";
	Result<UUID> const door = fixture.Operations.CreateEntity(info);
	REQUIRE(door.IsOk());
	info.Name = "Player";
	Result<UUID> const player = fixture.Operations.CreateEntity(info);
	REQUIRE(player.IsOk());
	REQUIRE(fixture.Operations.AddComponent(player.GetValue(), "VisualScript", Json::object({{"Graph", fixture.Graph.ToString()}})).IsOk());
	size_t const steps = fixture.History().GetUndoCount();

	VisualScriptSceneContext scene;
	scene.EntityName = [&fixture](UUID entity) -> std::optional<std::string>
	{
		Scene& edited = fixture.Context_.GetScene();
		if (!edited.HasEntity(entity))
		{
			return std::nullopt;
		}
		Entity const found = edited.GetEntityByUUID(entity);
		return found.GetName();
	};
	scene.ContextEntity = player.GetValue();
	scene.ContextEntityName = "Player";
	VisualScriptEditContext context = fixture.Context();
	context.Scene = &scene;

	VisualScriptDropIntent drop;
	drop.Kind = VisualScriptDropKind::Entities;
	drop.Entities = {static_cast<uint64_t>(door.GetValue())};
	drop.Position = glm::vec2(1000.0f, 1000.0f);
	VisualScriptControllerResult dropped = fixture.Resolve(drop, context);
	CHECK(Description(dropped) == "Reference Door in Player");
	CHECK(dropped.Commit->Select == std::vector<uint32_t>{32});
	REQUIRE(dropped.ComponentEdits.size() == 1);
	fixture.Execute(dropped);
	CHECK(fixture.History().GetUndoCount() == steps + 1);
	VisualScriptVariable const& variable = fixture.Stored().Variables.back();
	CHECK(variable.Name == "Door");
	CHECK(variable.Type == "Strada.Entity");
	CHECK(variable.Public);
	CHECK(fixture.Node(32)->Type == "Variable.Get");
	auto const variables = [&fixture, &player]
	{
		Scene& edited = fixture.Context_.GetScene();
		Entity entity = edited.GetEntityByUUID(player.GetValue());
		return entity.GetComponent<VisualScriptComponent>().Variables;
	};
	CHECK(variables() == ScriptFieldMap{{"Door", ScriptFieldValue::FromEntity(door.GetValue())}});

	// One undo takes the variable, the node and the component's value back.
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.Stored().Variables.size() == 1);
	CHECK(fixture.Node(32) == nullptr);
	CHECK(variables().empty());

	SUBCASE("onto an Entity input, without a context entity")
	{
		VisualScriptDocument document = MakeDocument();
		document.Graph.Nodes.push_back(
			MakeNode(40, "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)", glm::vec2(1200.0f, 0.0f)));
		REQUIRE(fixture.Operations.EditVisualScript(fixture.Graph, CreateRef<VisualScriptDocument const>(document), "Add Force").IsOk());
		fixture.Frame();
		scene.ContextEntity = UUID::Invalid();
		drop.Pin = VisualScriptPinReference{40, "Target"};
		drop.Direction = VisualScriptPinDirection::In;
		VisualScriptControllerResult linked = fixture.Resolve(drop, context);
		CHECK(Description(linked) == "Reference Door in Player");
		CHECK(linked.ComponentEdits.empty());
		CHECK(linked.Commit->Notes == std::vector<std::string>{"Set Door on the entities that run Player: Inspector > Variables"});
		fixture.Execute(linked);
		CHECK(fixture.HasLink(MakeLink(42, "Value", 40, "Target")));

		drop.Pin = VisualScriptPinReference{2, "Duration"};
		VisualScriptControllerResult wrong = fixture.Resolve(drop, context);
		CHECK_FALSE(wrong.Commit.has_value());
		CHECK(wrong.Notes.size() == 1);
	}
	SUBCASE("while playing")
	{
		scene.Playing = true;
		VisualScriptControllerResult refused = fixture.Resolve(drop, context);
		CHECK_FALSE(refused.Commit.has_value());
		CHECK(refused.Notes == std::vector<std::string>{std::string(VisualScriptRefusals::EntitiesWhilePlaying)});
	}
}

TEST_CASE("VisualScriptEditor: the node menu lists its items in order, enabled as the nodes' state says, and each is one step")
{
	ControllerFixture fixture;
	fixture.Select({2});
	std::vector<VisualScriptMenuItem> const menu = BuildVisualScriptNodeMenu(fixture.Context());
	std::vector<std::string> labels;
	for (VisualScriptMenuItem const& item : menu)
	{
		labels.push_back(item.Submenu.empty() ? item.Label : std::string(item.Submenu) + "/" + item.Label);
	}
	CHECK(labels == std::vector<std::string>{"Cut",
	                                         "Copy",
	                                         "Duplicate",
	                                         "Delete",
	                                         "Delete Without Relinking",
	                                         "Break All Links of Delay",
	                                         "Find References",
	                                         "Add Node Comment...",
	                                         "Remove Node Comment",
	                                         "Collapse",
	                                         "Enabled State/Enabled",
	                                         "Enabled State/Disabled",
	                                         "Enabled State/Development Only",
	                                         "Comment Selection",
	                                         "Align/Left",
	                                         "Align/Right",
	                                         "Align/Top",
	                                         "Align/Bottom",
	                                         "Align/Centers",
	                                         "Align/Middles",
	                                         "Distribute/Horizontally",
	                                         "Distribute/Vertically",
	                                         "Straighten",
	                                         "Arrange Selection"});
	auto const find = [&menu](Action action) -> VisualScriptMenuItem const&
	{
		auto const found = std::find_if(menu.begin(), menu.end(),
		                                [action](VisualScriptMenuItem const& item)
		                                {
											return item.Action == action;
										});
		REQUIRE(found != menu.end());
		return *found;
	};
	CHECK(find(Action::BreakNodeLinks).SeparatorBefore);
	CHECK_FALSE(find(Action::Copy).SeparatorBefore);
	CHECK_FALSE(find(Action::FindReferences).Enabled);
	CHECK(find(Action::FindReferences).Reason == "Delay refers to no variable, function or event");
	CHECK_FALSE(find(Action::RemoveNodeComment).Enabled);
	CHECK(find(Action::SetEnabled).Checked);
	CHECK_FALSE(find(Action::SetDisabled).Checked);
	CHECK_FALSE(find(Action::AlignLeft).Enabled);
	CHECK_FALSE(find(Action::Arrange).Enabled);

	// Every enabled item that edits makes exactly one step.
	for (VisualScriptMenuItem const& item : menu)
	{
		if (!item.Enabled || item.Action == Action::Copy || item.Action == Action::AddNodeComment)
		{
			continue;
		}
		CAPTURE(item.Label);
		VisualScriptControllerResult result = fixture.Act(item.Action, {2}, glm::vec2(0.0f, 960.0f));
		if (item.Action == Action::SetEnabled)
		{
			// Already its state: nothing to write.
			CHECK_FALSE(result.Commit.has_value());
			continue;
		}
		REQUIRE(result.Commit.has_value());
		Testing::ExpectStrictRoundTrip(*result.Commit->Document);
		size_t const steps = fixture.History().GetUndoCount();
		fixture.Execute(result);
		CHECK(fixture.History().GetUndoCount() == steps + 1);
		REQUIRE(fixture.Operations.Undo().IsOk());
		fixture.Frame();
	}

	// Several nodes: the menu acts on all of them, and says how many.
	fixture.Select({2, 3, 8});
	std::vector<VisualScriptMenuItem> const several = BuildVisualScriptNodeMenu(fixture.Context());
	auto const label = [&several](Action action)
	{
		auto const found = std::find_if(several.begin(), several.end(),
		                                [action](VisualScriptMenuItem const& item)
		                                {
											return item.Action == action;
										});
		REQUIRE(found != several.end());
		return found->Label;
	};
	CHECK(label(Action::ToggleCollapsed) == "Collapse 3 Nodes");
	CHECK(label(Action::Delete) == "Delete 3 Nodes");
	CHECK(label(Action::BreakNodeLinks) == "Break All Links of 3 Nodes");
	VisualScriptControllerResult collapsed = fixture.Act(Action::ToggleCollapsed, {2, 3, 8});
	CHECK(Description(collapsed) == "Collapse 3 Nodes in Player");
	size_t const steps = fixture.History().GetUndoCount();
	fixture.Execute(collapsed);
	CHECK(fixture.History().GetUndoCount() == steps + 1);

	// The first item opens what the node points to.
	fixture.Select({9});
	CHECK(BuildVisualScriptNodeMenu(fixture.Context()).front().Label == "Open AddScore");
	fixture.Select({8});
	CHECK(BuildVisualScriptNodeMenu(fixture.Context()).front().Label == "Go to Event Hit");
	fixture.Select({5});
	std::vector<VisualScriptMenuItem> const get = BuildVisualScriptNodeMenu(fixture.Context());
	CHECK(get.front().Label == "Select Variable Speed");
	// A pure node offers the enabled states disabled, saying why.
	fixture.Select({6});
	for (VisualScriptMenuItem const& item : BuildVisualScriptNodeMenu(fixture.Context()))
	{
		if (item.Submenu == "Enabled State")
		{
			CHECK_FALSE(item.Enabled);
			CHECK(item.Reason == VisualScriptPureNodesRefusal);
		}
	}
	// A function's ends do not offer them; its entry finds its callers.
	fixture.Model->ShowFunction(20);
	fixture.Frame();
	fixture.Select({21});
	std::vector<VisualScriptMenuItem> const entry = BuildVisualScriptNodeMenu(fixture.Context());
	CHECK(std::none_of(entry.begin(), entry.end(),
	                   [](VisualScriptMenuItem const& item)
	                   {
						   return item.Submenu == "Enabled State";
					   }));
	CHECK(std::any_of(entry.begin(), entry.end(),
	                  [](VisualScriptMenuItem const& item)
	                  {
						  return item.Label == "Find Callers" && item.Enabled;
					  }));
}

TEST_CASE("VisualScriptEditor: pins named as the canvas shows a renamed member's pins are edited under the names the document stores")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes[2].Type = std::string(Testing::RenamedLogSemantics::FormerType);
	document.Graph.Nodes[2].Inputs = Json::object({{"Text", "Hi"}});
	ControllerFixture fixture(document);
	Testing::RenamedLogSemantics renamed(fixture.Semantics);
	fixture.SemanticsOverride = &renamed;
	fixture.Semantics.ReloadLibrary();
	fixture.Frame();
	fixture.Frame();
	VisualScriptAnalyzedNode const* analyzed = fixture.Model->GetAnalysis()->FindNode(3);
	REQUIRE(analyzed != nullptr);
	REQUIRE(analyzed->Resolved == VisualScriptResolution::Redirect);

	// The value typed into the chip the canvas calls Message is the stored Text, which the commit stores as Message.
	VisualScriptControllerResult value = fixture.Edit(VisualScriptLiteralEdit{{3, "Message"}, "Bye"});
	CHECK(Description(value) == "Set Message on Info in Player");
	CHECK(value.Commit->Notes == std::vector<std::string>{"node updated: Call:Strada.Log.Write(string) → Call:Strada.Log.Info(string)"});
	CHECK(value.Commit->Document->Graph.Nodes[2].Type == Testing::RenamedLogSemantics::Type);
	CHECK(value.Commit->Document->Graph.Nodes[2].Inputs == Json::object({{"Message", "Bye"}}));

	VisualScriptControllerResult linked = fixture.Resolve(VisualScriptConnectIntent{MakeLink(5, "Value", 3, "Message"), false, {}});
	CHECK(Description(linked) == "Connect Value to Message in Player");
	fixture.Execute(linked);
	CHECK(fixture.HasLink(MakeLink(5, "Value", 3, "Message")));
}

TEST_CASE("VisualScriptEditor: a struct pin splits from its menu and recombines from any of its field pins")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(40, "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)",
	                                        glm::vec2(1200.0f, 0.0f), Json::object(), Json::object({{"Force", Json::array({1, 2, 3})}})));
	ControllerFixture fixture(document);

	VisualScriptControllerResult split =
		fixture.Edit(VisualScriptPinEdit{VisualScriptPinAction::Split, {40, "Force"}, VisualScriptPinDirection::In, false, {}});
	CHECK(Description(split) == "Split Force in Player");
	Testing::ExpectSplitConsistency(*split.Commit->Document);
	fixture.Execute(split);
	CHECK(fixture.Node(40)->Split.Inputs == std::vector<std::string>{"Force"});
	CHECK(fixture.Node(40)->Inputs["Force.Y"] == 2);
	VisualScriptCanvasModel const& canvas = fixture.Model->GetCanvas();
	REQUIRE(canvas.FindNode(40).has_value());
	CHECK(canvas.FindPin(*canvas.FindNode(40), "Force.X", VisualScriptPinDirection::In).has_value());

	VisualScriptControllerResult recombined =
		fixture.Edit(VisualScriptPinEdit{VisualScriptPinAction::Recombine, {40, "Force.Y"}, VisualScriptPinDirection::In, false, {}});
	CHECK(Description(recombined) == "Recombine Force in Player");
	Testing::ExpectSplitConsistency(*recombined.Commit->Document);
	fixture.Execute(recombined);
	CHECK(fixture.Node(40)->Split.Inputs.empty());
	CHECK(fixture.Node(40)->Inputs["Force"] == Json::array({1, 2, 3}));
	CHECK(fixture.History().GetUndoCount() == 2);

	// A pin that is no field of a split pin has nothing to recombine: the window never offers it.
	CHECK_FALSE(ResolveVisualScriptEdit(
					VisualScriptPinEdit{VisualScriptPinAction::Recombine, {2, "Duration"}, VisualScriptPinDirection::In, false, {}},
					fixture.Context())
	                .IsOk());
}

TEST_CASE("VisualScriptEditor: an asset dropped on a pin of its type sets the pin's value, and on the canvas adds a literal of it")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(
		40, "Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)", glm::vec2(1200.0f, 0.0f)));
	ControllerFixture fixture(document);
	EntityCreateInfo info;
	info.Name = "Door";
	Result<UUID> const door = fixture.Operations.CreateEntity(info);
	REQUIRE(door.IsOk());
	Result<AssetHandle> const prefab = fixture.Operations.CreatePrefab(door.GetValue(), "Door.sprefab");
	REQUIRE(prefab.IsOk());
	Result<AssetHandle> const material = fixture.Operations.CreateMaterial("Materials/Red.smat");
	REQUIRE(material.IsOk());
	fixture.Frame();
	std::string const handle = prefab.GetValue().ToString();

	auto const drop = [&fixture](AssetHandle asset, std::optional<VisualScriptPinReference> pin)
	{
		VisualScriptDropIntent intent;
		intent.Kind = VisualScriptDropKind::Asset;
		intent.Payload = asset.GetUUID().GetValue();
		intent.Position = glm::vec2(1203.0f, 405.0f);
		intent.Pin = std::move(pin);
		intent.Direction = VisualScriptPinDirection::In;
		return fixture.Resolve(intent);
	};

	VisualScriptControllerResult set = drop(prefab.GetValue(), VisualScriptPinReference{40, "Prefab"});
	CHECK(Description(set) == "Set Prefab on Instantiate in Player");
	fixture.Execute(set);
	CHECK(fixture.Node(40)->Inputs["Prefab"] == handle);

	VisualScriptControllerResult literal = drop(prefab.GetValue(), std::nullopt);
	CHECK(Description(literal) == "Add Prefab in Player");
	REQUIRE(literal.Commit->Select.size() == 1);
	fixture.Execute(literal);
	VisualScriptNode const* added = fixture.Node(literal.Commit->Select.front());
	REQUIRE(added != nullptr);
	CHECK(added->Type == "Literal");
	CHECK(added->Settings == Json::object({{"Type", "Strada.Prefab"}, {"Value", handle}}));
	CHECK(added->Position == glm::vec2(1200.0f, 400.0f));

	// Refused, with a toast and no step: a pin of another type, an asset type graphs have no values of, an unknown asset.
	size_t const steps = fixture.History().GetUndoCount();
	VisualScriptControllerResult wrongPin = drop(prefab.GetValue(), VisualScriptPinReference{2, "Duration"});
	CHECK_FALSE(wrongPin.Commit.has_value());
	CHECK(wrongPin.Notes.size() == 1);
	VisualScriptControllerResult wrongType = drop(material.GetValue(), std::nullopt);
	CHECK_FALSE(wrongType.Commit.has_value());
	CHECK(wrongType.Notes.size() == 1);
	VisualScriptControllerResult unknown = drop(AssetHandle(UUID(0x5EED5EED5EEDull)), std::nullopt);
	CHECK_FALSE(unknown.Commit.has_value());
	CHECK(unknown.Notes == std::vector<std::string>{"The dropped asset is not in the project"});
	CHECK(fixture.History().GetUndoCount() == steps);
}

TEST_CASE("VisualScriptEditor: Update Node is offered on an updated node and stores every node under its current type in one step")
{
	VisualScriptDiagnostic updated;
	updated.Severity = VisualScriptSeverity::Warning;
	updated.Code = "NodeUpdated";
	updated.Node = 3;
	updated.Message = "node updated";

	SUBCASE("nothing stored under a former type")
	{
		ControllerFixture fixture;
		fixture.Semantics.InjectDiagnostic(updated);
		fixture.Semantics.ReloadLibrary();
		fixture.Frame();
		fixture.Frame();
		fixture.Select({3});
		std::vector<VisualScriptMenuItem> const menu = BuildVisualScriptNodeMenu(fixture.Context());
		REQUIRE_FALSE(menu.empty());
		CHECK(menu.back().Action == Action::UpdateNode);
		CHECK(menu.back().Label == "Update Node");
		CHECK(menu.back().Enabled);
		CHECK(menu.back().SeparatorBefore);
		VisualScriptControllerResult result = fixture.Act(Action::UpdateNode, {3});
		CHECK_FALSE(result.Commit.has_value());
		CHECK(result.Notes == std::vector<std::string>{"Every node is up to date"});
	}
	SUBCASE("a node stored under its member's former name")
	{
		VisualScriptDocument document = MakeDocument();
		document.Graph.Nodes[2].Type = std::string(Testing::RenamedLogSemantics::FormerType);
		document.Graph.Nodes[2].Inputs = Json::object({{"Text", "Hi"}});
		ControllerFixture fixture(document);
		Testing::RenamedLogSemantics renamed(fixture.Semantics);
		fixture.SemanticsOverride = &renamed;
		fixture.Semantics.ReloadLibrary();
		fixture.Frame();
		fixture.Frame();
		VisualScriptControllerResult result = fixture.Act(Action::UpdateNode, {3});
		CHECK(Description(result) == "Update Nodes in Player");
		CHECK(result.Commit->Notes ==
		      std::vector<std::string>{"node updated: Call:Strada.Log.Write(string) → Call:Strada.Log.Info(string)"});
		fixture.Execute(result);
		CHECK(fixture.Node(3)->Type == Testing::RenamedLogSemantics::Type);
		CHECK(fixture.Node(3)->Inputs == Json::object({{"Message", "Hi"}}));
		CHECK(fixture.History().GetUndoCount() == 1);
	}
}

TEST_CASE("VisualScriptEditor: My Script's rows dropped on the canvas make their nodes: Get, Set, a call and a sender")
{
	ControllerFixture fixture;
	size_t const steps = fixture.History().GetUndoCount();
	auto const drop = [](VisualScriptElementKind kind, uint32_t id, uint8_t modifiers = 0)
	{
		VisualScriptDropIntent intent;
		intent.Kind = VisualScriptDropKind::Element;
		intent.Payload = PackVisualScriptElement(kind, id);
		intent.Position = glm::vec2(1203.0f, 1001.0f);
		intent.Modifiers = modifiers;
		return intent;
	};

	// A variable on empty canvas: the window asks which node; Ctrl makes a Get and Alt a Set without asking.
	VisualScriptControllerResult const asked = fixture.Resolve(drop(VisualScriptElementKind::Variable, 10));
	CHECK_FALSE(asked.Commit.has_value());
	REQUIRE(asked.DropMenu.has_value());
	CHECK(asked.DropMenu->Name == "Speed");
	VisualScriptControllerResult get = fixture.Resolve(drop(VisualScriptElementKind::Variable, 10, VisualScriptDropModifiers::Ctrl));
	CHECK(Description(get) == "Add Get Speed in Player");
	fixture.Execute(get);
	VisualScriptNode const& made = fixture.Stored().Graph.Nodes.back();
	CHECK(made.Type == "Variable.Get");
	CHECK(made.Settings["Variable"] == 10);
	CHECK(*made.Position == glm::vec2(1200.0f, 1008.0f));
	VisualScriptControllerResult set = fixture.Resolve(drop(VisualScriptElementKind::Variable, 10, VisualScriptDropModifiers::Alt));
	CHECK(Description(set) == "Add Set Speed in Player");

	// On a data output: a Set fed from it, and run after its node when that node's flow goes nowhere yet.
	VisualScriptDropIntent onOutput = drop(VisualScriptElementKind::Variable, 10);
	onOutput.Pin = VisualScriptPinReference{6, "Result"};
	onOutput.Direction = VisualScriptPinDirection::Out;
	VisualScriptControllerResult fed = fixture.Resolve(onOutput);
	CHECK(Description(fed) == "Add Set Speed in Player");
	fixture.Execute(fed);
	uint32_t const setter = fixture.Stored().Graph.Nodes.back().ID;
	CHECK(fixture.Node(setter)->Type == "Variable.Set");
	CHECK(fixture.HasLink(MakeLink(6, "Result", setter, "Value")));
	// On an input: a Get linked into it.
	VisualScriptDropIntent onInput = drop(VisualScriptElementKind::Variable, 10);
	onInput.Pin = VisualScriptPinReference{3, "Message"};
	onInput.Direction = VisualScriptPinDirection::In;
	VisualScriptControllerResult linked = fixture.Resolve(onInput);
	CHECK(Description(linked) == "Add Get Speed in Player");
	fixture.Execute(linked);
	CHECK(fixture.HasLink(MakeLink(fixture.Stored().Graph.Nodes.back().ID, "Value", 3, "Message")));

	// A function: its call; a custom event: a sender of its name.
	VisualScriptControllerResult call = fixture.Resolve(drop(VisualScriptElementKind::Function, 20));
	CHECK(Description(call) == "Add AddScore in Player");
	fixture.Execute(call);
	CHECK(fixture.Stored().Graph.Nodes.back().Type == "Function.Call");
	CHECK(fixture.Stored().Graph.Nodes.back().Settings["Function"] == 20);
	VisualScriptControllerResult send = fixture.Resolve(drop(VisualScriptElementKind::CustomEvent, 7));
	CHECK(Description(send) == "Add Send Hit in Player");
	fixture.Execute(send);
	CHECK(fixture.Stored().Graph.Nodes.back().Type == "Event.Send");
	CHECK(fixture.Stored().Graph.Nodes.back().Settings["Name"] == "Hit");
	CHECK(fixture.History().GetUndoCount() == steps + 5);

	// What is gone, and a local of a function not shown, are refused with a note.
	VisualScriptControllerResult const gone = fixture.Resolve(drop(VisualScriptElementKind::Variable, 99, VisualScriptDropModifiers::Ctrl));
	CHECK_FALSE(gone.Commit.has_value());
	CHECK_FALSE(gone.Notes.empty());
	VisualScriptControllerResult const unknown = fixture.Resolve(drop(VisualScriptElementKind::CustomEvent, 2));
	CHECK_FALSE(unknown.Commit.has_value());
	CHECK_FALSE(unknown.Notes.empty());
}

TEST_CASE("VisualScriptEditor: the node menu's Enabled State follows the panes' rule: offered, disabled with its reason, or not shown")
{
	ControllerFixture fixture;
	auto const states = [&fixture](uint32_t node)
	{
		fixture.Select({node});
		std::vector<VisualScriptMenuItem> items;
		for (VisualScriptMenuItem const& item : BuildVisualScriptNodeMenu(fixture.Context()))
		{
			if (item.Submenu == "Enabled State")
			{
				items.push_back(item);
			}
		}
		return items;
	};
	auto const offer = [&fixture](uint32_t node)
	{
		VisualScriptNode const* found = FindVisualScriptNode(fixture.Stored(), node);
		REQUIRE(found != nullptr);
		return GetVisualScriptEnabledOffer(*fixture.Model, &fixture.Semantics.GetFixtureLibrary(), *found);
	};

	REQUIRE(offer(2) == VisualScriptEnabledOffer::Offered);
	std::vector<VisualScriptMenuItem> const delay = states(2);
	REQUIRE(delay.size() == 3);
	CHECK(std::all_of(delay.begin(), delay.end(),
	                  [](VisualScriptMenuItem const& item)
	                  {
						  return item.Enabled;
					  }));
	REQUIRE(offer(6) == VisualScriptEnabledOffer::PureNode);
	std::vector<VisualScriptMenuItem> const pure = states(6);
	REQUIRE(pure.size() == 3);
	for (VisualScriptMenuItem const& item : pure)
	{
		CHECK_FALSE(item.Enabled);
		CHECK(item.Reason == VisualScriptPureNodesRefusal);
	}

	fixture.Model->ShowFunction(20);
	fixture.Frame();
	REQUIRE(offer(21) == VisualScriptEnabledOffer::FunctionEnd);
	CHECK(states(21).empty());
}

TEST_CASE("VisualScriptEditor: a node of a type the library does not know offers the Enabled states, so disabling it ends its "
          "chain")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(30, "Flow.NoSuchNode", glm::vec2(0.0f, 960.0f)));
	ControllerFixture fixture(document);
	VisualScriptCanvasModel const& canvas = fixture.Model->GetCanvas();
	std::optional<uint32_t> const index = canvas.FindNode(30);
	REQUIRE(index.has_value());
	std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
	REQUIRE(nodes[*index].HasFlag(VisualScriptNodeFlags::UnknownType));
	VisualScriptNode const* node = FindVisualScriptNode(fixture.Stored(), 30);
	REQUIRE(node != nullptr);
	CHECK(GetVisualScriptEnabledOffer(*fixture.Model, &fixture.Semantics.GetFixtureLibrary(), *node) == VisualScriptEnabledOffer::Offered);

	fixture.Select({30});
	std::vector<VisualScriptMenuItem> states;
	for (VisualScriptMenuItem const& item : BuildVisualScriptNodeMenu(fixture.Context()))
	{
		if (item.Submenu == "Enabled State")
		{
			states.push_back(item);
		}
	}
	REQUIRE(states.size() == 3);
	CHECK(std::all_of(states.begin(), states.end(),
	                  [](VisualScriptMenuItem const& item)
	                  {
						  return item.Enabled;
					  }));
}

TEST_CASE("VisualScriptEditor: a variable dropped on a data output runs after that node only when its first exec output is free")
{
	VisualScriptDocument document = MakeDocument();
	// A Tween whose Update goes on to a Delay, and a free Tween.
	document.Graph.Nodes.push_back(MakeNode(40, "Flow.Tween", glm::vec2(0.0f, 960.0f)));
	document.Graph.Nodes.push_back(MakeNode(41, "Flow.Delay", glm::vec2(400.0f, 960.0f)));
	document.Graph.Nodes.push_back(MakeNode(42, "Flow.Tween", glm::vec2(0.0f, 1280.0f)));
	document.Graph.Links.push_back(MakeLink(40, "Update", 41, "Execute"));
	ControllerFixture fixture(document);
	auto const dropOnAlpha = [&fixture](uint32_t tween)
	{
		VisualScriptDropIntent intent;
		intent.Kind = VisualScriptDropKind::Element;
		intent.Payload = PackVisualScriptElement(VisualScriptElementKind::Variable, 10);
		intent.Position = glm::vec2(400.0f, 1200.0f);
		intent.Pin = VisualScriptPinReference{tween, "Alpha"};
		intent.Direction = VisualScriptPinDirection::Out;
		VisualScriptControllerResult result = fixture.Resolve(intent);
		REQUIRE(result.Commit.has_value());
		fixture.Execute(result);
		return fixture.Stored().Graph.Nodes.back().ID;
	};

	// Update, the first exec output, is linked: the Set is fed but never run after Completed, which comes once at the end.
	uint32_t const fed = dropOnAlpha(40);
	CHECK(fixture.HasLink(MakeLink(40, "Alpha", fed, "Value")));
	CHECK_FALSE(fixture.HasLink(MakeLink(40, "Completed", fed, "Execute")));
	CHECK_FALSE(fixture.HasLink(MakeLink(40, "Update", fed, "Execute")));
	// Free: the Set runs on every Update.
	uint32_t const run = dropOnAlpha(42);
	CHECK(fixture.HasLink(MakeLink(42, "Alpha", run, "Value")));
	CHECK(fixture.HasLink(MakeLink(42, "Update", run, "Execute")));
}

namespace
{
	// The fixture's analysis with the Get of Targets (node 40) resolved to its array type, as the scripting runtime resolves it
	// (the fixture analyzer resolves no wildcard).
	class ArrayGetSemantics final : public VisualScriptSemantics
	{
	public:
		explicit ArrayGetSemantics(Testing::FixtureVisualScriptSemantics& fixture)
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
				if (node["ID"] != 40)
				{
					continue;
				}
				for (Json& pin : node["Pins"])
				{
					pin["Type"] = "Strada.Entity[]";
				}
			}
			return VisualScriptAnalysis::Parse(source.dump());
		}

	private:
		Testing::FixtureVisualScriptSemantics& m_Fixture;
	};
}

TEST_CASE("VisualScriptEditor: an array pin is never promoted to a variable or a function input, and says why")
{
	VisualScriptDocument document = MakeDocument();
	VisualScriptVariable targets;
	targets.ID = 11;
	targets.Name = "Targets";
	targets.Type = "Strada.Entity[]";
	targets.Public = true;
	document.Variables.push_back(targets);
	document.Graph.Nodes.push_back(MakeNode(40, "Variable.Get", glm::vec2(0.0f, 960.0f), Json::object({{"Variable", 11}})));
	ControllerFixture fixture(document);
	ArrayGetSemantics arrays(fixture.Semantics);
	fixture.SemanticsOverride = &arrays;
	fixture.Semantics.ReloadLibrary();
	fixture.Frame();
	fixture.Frame();
	VisualScriptCanvasModel const& canvas = fixture.Model->GetCanvas();
	std::optional<uint32_t> const node = canvas.FindNode(40);
	REQUIRE(node.has_value());
	std::optional<uint32_t> const pin = canvas.FindPin(*node, "Value", VisualScriptPinDirection::Out);
	REQUIRE(pin.has_value());
	std::span<VisualScriptPinVisual const> const pins = canvas.GetPins();
	REQUIRE(pins[*pin].Shape == VisualScriptPinShape::Array);
	size_t const steps = fixture.History().GetUndoCount();

	VisualScriptPinEdit promote;
	promote.Action = VisualScriptPinAction::PromoteToVariable;
	promote.Pin = VisualScriptPinReference{40, "Value"};
	promote.Direction = VisualScriptPinDirection::Out;
	VisualScriptControllerResult const refused = fixture.Edit(promote);
	CHECK_FALSE(refused.Commit.has_value());
	CHECK(refused.Notes == std::vector<std::string>{std::string(VisualScriptRefusals::PromoteArray)});
	CHECK(fixture.History().GetUndoCount() == steps);
	CHECK(fixture.Stored().Variables.size() == 2);
}

TEST_CASE("VisualScriptEditor: what a drop on a pin can change: assets of the input's type, entities on Entity inputs, variables on "
          "data pins")
{
	ControllerFixture fixture;
	VisualScriptCanvasModel const& canvas = fixture.Model->GetCanvas();
	std::span<VisualScriptPinVisual const> const pins = canvas.GetPins();
	auto const pinOf = [&](uint32_t id, std::string_view name, VisualScriptPinDirection direction)
	{
		std::optional<uint32_t> const node = canvas.FindNode(id);
		REQUIRE(node.has_value());
		std::optional<uint32_t> const pin = canvas.FindPin(*node, name, direction);
		REQUIRE(pin.has_value());
		return VisualScriptPinVisual(pins[*pin]);
	};
	VisualScriptPinVisual const message = pinOf(3, "Message", VisualScriptPinDirection::In);
	VisualScriptPinVisual const execute = pinOf(3, "Execute", VisualScriptPinDirection::In);
	VisualScriptPinVisual const value = pinOf(5, "Value", VisualScriptPinDirection::Out);

	uint64_t const speed = PackVisualScriptElement(VisualScriptElementKind::Variable, 10);
	CHECK(CanDropOnVisualScriptPin(VisualScriptDropKind::Element, speed, message));
	CHECK(CanDropOnVisualScriptPin(VisualScriptDropKind::Element, speed, value));
	CHECK_FALSE(CanDropOnVisualScriptPin(VisualScriptDropKind::Element, speed, execute));
	// A function's call is placed beside any pin.
	CHECK(CanDropOnVisualScriptPin(VisualScriptDropKind::Element, PackVisualScriptElement(VisualScriptElementKind::Function, 20), execute));
	// Entities only on Entity inputs; an asset that is not in the project nowhere.
	CHECK_FALSE(CanDropOnVisualScriptPin(VisualScriptDropKind::Entities, 0, message));
	CHECK_FALSE(CanDropOnVisualScriptPin(VisualScriptDropKind::Asset, 12345, message));
}
