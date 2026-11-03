#include "HeadlessImGui.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"
#include "Editor/VisualScripting/VisualScriptDetails.h"
#include "Editor/VisualScripting/VisualScriptDrawer.h"
#include "Editor/VisualScripting/VisualScriptFindBar.h"
#include "Editor/VisualScripting/VisualScriptLanguageEditing.h"
#include "Editor/VisualScripting/VisualScriptLiteralEditor.h"
#include "Editor/VisualScripting/VisualScriptPalettePopup.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptProjectIndex.h"
#include "Editor/VisualScripting/VisualScriptQuickFixes.h"
#include "Editor/VisualScripting/VisualScriptSidebar.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Script/VisualScriptLiterals.h"
#include "Strada/Script/VisualScriptVariables.h"

#include <doctest/doctest.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr ImVec2 WindowSize(1200.0f, 900.0f);
	constexpr uint64_t SessionKey = 0x50414E4500000000ull;

	VisualScriptNode MakeNode(uint32_t id, std::string type, glm::vec2 position, Json settings = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = position;
		node.Settings = std::move(settings);
		return node;
	}

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		return variable;
	}

	// Update → Log, a Raycast whose flow ends there, and two variables.
	VisualScriptDocument MakeDocument()
	{
		VisualScriptDocument document;
		document.Variables = {MakeVariable(100, "Speed", "float"), MakeVariable(101, "Alive", "bool")};
		document.Graph.Nodes = {MakeNode(1, "Event.Update", glm::vec2(0.0f)),
		                        MakeNode(2, "Call:Strada.Log.Info(string)", glm::vec2(400.0f, 0.0f)),
		                        MakeNode(3, "Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)",
		                                 glm::vec2(0.0f, 300.0f))};
		document.Graph.Links = {{{1, "Then"}, {2, "Execute"}}};
		return document;
	}

	// The panes a PaneFixture draws, each in a window of its own.
	enum class FixturePane : uint8_t
	{
		// The palette popup, in a window as large as the test display (its host, as the canvas's window is).
		Palette = 0,
		Sidebar,
		Details,
		Drawer,
		FindBar
	};

	// A graph window's model of a project's graph and its panes drawn in windows beside it, frame by frame as the panel draws
	// them: the end of widget interactions (spec §11.1's pane-level form of §4.4 rule 2), Sync, Update, then each pane, its
	// commit executed as the panel executes it and its requests applied.
	struct PaneFixture
	{
		explicit PaneFixture(VisualScriptDocument const& document, std::vector<FixturePane> panes = {FixturePane::Palette})
			: Panes(std::move(panes))
		{
			Fonts.Monospace = ImGui.UseEditorStyle();
			REQUIRE(Operations.CreateProject(Temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			Result<AssetHandle> const script = Operations.CreateVisualScript("Scripts/Graph.sgraph", VisualScriptTemplate::Empty);
			REQUIRE(script.IsOk());
			Graph = script.GetValue();
			REQUIRE(Operations.SetVisualScriptDocument(Graph, document).IsOk());
			File = AssetManager::GetAbsolutePath(Graph);
			Model.emplace(Graph, Settings);
		}

		VisualScriptPaneContext MakeContext()
		{
			return {*Model,  *ActiveSemantics, Settings, Session, Services, Shared,          Graph,
			        "Graph", ContextEntity,    Playing,  Time,    Measure,  DrawNodePreview, ReadLiveVariables};
		}

		// What the window's shell does before any window draws: flush the edits of widgets that are no longer active, then
		// end the step when the active item changed.
		void EndInteractions()
		{
			ImGuiID const active = GImGui->ActiveId;
			if (active == 0)
			{
				for (std::optional<VisualScriptCommitRequest> pending :
				     {Details.TakePendingEdit(), Sidebar.TakePendingEdit(), DrawerPane.TakePendingEdit()})
				{
					if (pending)
					{
						ExecuteCommit(std::move(*pending));
					}
				}
			}
			Session.Update(active != 0 && active == PreviousActive, Context.GetHistory());
			PreviousActive = active;
		}

		void ExecuteCommit(VisualScriptCommitRequest request)
		{
			Ref<VisualScriptDocument const> const committed = request.Document;
			REQUIRE(Operations.EditVisualScript(Graph, committed, request.Description, request.MergeKey, std::move(request.Text)).IsOk());
			Model->OnCommitted(committed, request.LayoutOnly);
			Model->Sync(*ActiveSemantics, Measure, Theme, false, Time);
			// The panel selects what the commit asks for, when it asks for something.
			if (!request.Select.empty())
			{
				Model->Select(request.Select);
			}
		}

		// What the panel does with a pane's result, in the order VisualScriptPaneResult gives.
		void Execute(VisualScriptPaneResult result)
		{
			if (result.Commit)
			{
				ExecuteCommit(std::move(*result.Commit));
			}
			if (result.FocusSetting)
			{
				Shared.FocusSetting = result.FocusSetting;
			}
			if (result.ShowFunction)
			{
				Model->ShowFunction(*result.ShowFunction);
			}
			if (result.Open && result.Open->Graph == Graph)
			{
				Model->ApplyOpenRequest(*result.Open);
			}
			if (!result.SelectAndFrame.empty())
			{
				Model->ApplyOpenRequest(MakeVisualScriptJump(Graph, Model->GetFunction(), result.SelectAndFrame));
			}
			if (result.FindReferences)
			{
				DrawerPane.ShowReferences(*result.FindReferences);
			}
			Results.push_back(std::move(result));
		}

		// Draws a pane in a window of its own.
		template<typename TDraw>
		void DrawPane(char const* name, ImVec2 position, ImVec2 size, TDraw&& draw)
		{
			ImGui::SetNextWindowPos(position);
			ImGui::SetNextWindowSize(size);
			ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
			VisualScriptPaneContext context = MakeContext();
			VisualScriptPaneResult result = draw(context);
			ImGui::End();
			Execute(std::move(result));
		}

		void Draw()
		{
			Time += 1.0 / 60.0;
			EndInteractions();
			Model->Sync(*ActiveSemantics, Measure, Theme, false, Time);
			VisualScriptCanvasInput input;
			input.CanvasSize = glm::vec2(800.0f, 600.0f);
			input.Time = Time;
			Model->Update(input);
			for (FixturePane const pane : Panes)
			{
				switch (pane)
				{
					case FixturePane::Palette:
						DrawPane("Graph", ImVec2(0.0f, 0.0f), WindowSize,
						         [this](VisualScriptPaneContext& context)
						         {
									 return Palette.Draw(context);
								 });
						break;
					case FixturePane::Details:
						DrawPane("Details", DetailsPosition, DetailsWindowSize,
						         [this](VisualScriptPaneContext& context)
						         {
									 return Details.Draw(context);
								 });
						break;
					case FixturePane::Sidebar:
						DrawPane("My Script", ImVec2(0.0f, 0.0f), SidebarSize,
						         [this](VisualScriptPaneContext& context)
						         {
									 return Sidebar.Draw(context);
								 });
						break;
					case FixturePane::Drawer:
						DrawPane("Drawer", DrawerPosition, DrawerSize,
						         [this](VisualScriptPaneContext& context)
						         {
									 return DrawerPane.Draw(context);
								 });
						break;
					case FixturePane::FindBar:
						DrawPane("Find", FindBarPosition, FindBarSize,
						         [this](VisualScriptPaneContext& context)
						         {
									 return FindBar.Draw(context);
								 });
						break;
				}
			}
		}

		std::function<void()> Drawer()
		{
			return [this]
			{
				Draw();
			};
		}

		void Frames(int count)
		{
			for (int i = 0; i < count; i++)
			{
				ImGui.Frame(Drawer());
			}
		}

		// Selects nodes and comments of the shown sub-graph as a click on the canvas does (after a frame laid them out).
		void SelectElements(std::vector<uint32_t> const& elements)
		{
			Model->Select(elements);
			Frames(2);
		}

		// The centre of the widget of a Details row, and a point near the left end of it.
		ImVec2 DetailsRow(std::string_view label, float fraction = 0.5f) const
		{
			std::optional<std::pair<glm::vec2, glm::vec2>> const row = Details.FindRow(label);
			REQUIRE_MESSAGE(row, "no Details row " << label);
			return ImVec2(row->first.x + (row->second.x - row->first.x) * fraction, (row->first.y + row->second.y) * 0.5f);
		}

		std::string ReadFile() const
		{
			Result<std::string> text = FileSystem::ReadTextFile(File);
			REQUIRE(text.IsOk());
			return text.TakeValue();
		}

		uint64_t GetVersion() const { return AssetManager::GetAsset<VisualScriptAsset>(Graph)->GetVersion(); }
		VisualScriptDocument const& GetDocument() const { return AssetManager::GetAsset<VisualScriptAsset>(Graph)->GetDocument(); }
		size_t GetUndoCount() { return Context.GetHistory().GetUndoCount(); }

		// The panel invariant: drawing alone changes no undo count, no file byte and no asset version.
		void CheckDrawingChangesNothing(std::function<void()> const& prepare = {})
		{
			size_t const undo = Context.GetHistory().GetUndoCount();
			std::string const bytes = ReadFile();
			uint64_t const version = GetVersion();
			if (prepare)
			{
				prepare();
			}
			Frames(3);
			CHECK(Context.GetHistory().GetUndoCount() == undo);
			CHECK(ReadFile() == bytes);
			CHECK(GetVersion() == version);
		}

		VisualScriptPaletteEntry const& Entry(uint32_t index) const { return Palette.GetIndex().GetEntries()[index]; }
		std::vector<std::string> GetStateTitles() const
		{
			std::vector<std::string> titles;
			for (VisualScriptPaletteMatch const& match : Palette.GetState().Entries)
			{
				titles.push_back(Entry(match.Entry).Title);
			}
			return titles;
		}

		// The centre of a My Script row, and of a section's "+" button.
		ImVec2 SidebarRow(VisualScriptElement element) const
		{
			std::optional<std::pair<glm::vec2, glm::vec2>> const row = Sidebar.FindRow(element);
			REQUIRE_MESSAGE(row, "no My Script row " << static_cast<uint32_t>(element.Kind) << " " << element.ID);
			return ImVec2((row->first.x + row->second.x) * 0.5f, (row->first.y + row->second.y) * 0.5f);
		}
		ImVec2 SidebarAdd(VisualScriptElementKind section) const
		{
			std::optional<std::pair<glm::vec2, glm::vec2>> const button = Sidebar.FindAddButton(section);
			REQUIRE(button);
			return ImVec2((button->first.x + button->second.x) * 0.5f, (button->first.y + button->second.y) * 0.5f);
		}

		static constexpr ImVec2 SidebarSize{340.0f, 900.0f};
		static constexpr ImVec2 DrawerPosition{0.0f, 560.0f};
		static constexpr ImVec2 DrawerSize{1200.0f, 340.0f};
		static constexpr ImVec2 FindBarPosition{360.0f, 0.0f};
		static constexpr ImVec2 FindBarSize{600.0f, 60.0f};
		static constexpr ImVec2 DetailsPosition{760.0f, 0.0f};
		static constexpr ImVec2 DetailsSize{440.0f, 900.0f};
		// The size Details is drawn at (DetailsSize unless a test narrows the pane).
		ImVec2 DetailsWindowSize = DetailsSize;

		struct
		{
			ImFont* Monospace = nullptr;
		} Fonts;
		std::vector<FixturePane> Panes;
		Testing::HeadlessImGui ImGui;
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Temporary;
		EditorContext Context;
		EditorOperations Operations{Context};
		AssetHandle Graph;
		std::filesystem::path File;
		VisualScriptViewSettings Settings{Temporary.GetPath() / "Views.json"};
		Testing::FixtureVisualScriptSemantics Semantics;
		// The semantics the panes and the model use: the fixture library's, or the scripting runtime's.
		VisualScriptSemantics* ActiveSemantics = &Semantics;
		VisualScriptTextMeasurer Measure = Testing::FixedWidthMeasurer(0.5f);
		VisualScriptThemeAnchors Theme;
		VisualScriptEditorServices Services;
		VisualScriptPaneShared Shared;
		EditSession Session{SessionKey};
		std::optional<VisualScriptEditorModel> Model;
		VisualScriptPalettePopup Palette;
		VisualScriptDetails Details;
		VisualScriptSidebar Sidebar;
		VisualScriptDrawer DrawerPane;
		VisualScriptFindBar FindBar;
		std::vector<VisualScriptPaneResult> Results;
		// The window's play state and context entity, and how it reads live values.
		bool Playing = false;
		UUID ContextEntity = UUID::Invalid();
		std::function<Result<ScriptFieldMap>(UUID)> ReadLiveVariables;
		// The canvas's node drawing the palette's preview calls; unset draws none.
		std::function<void(VisualScriptPaletteEntry const&, ImDrawList*, ImVec2, float)> DrawNodePreview;
		ImGuiID PreviousActive = 0;
		double Time = 0.0;
	};

	VisualScriptPaletteIntent MakeIntent(glm::vec2 position, std::string query = {})
	{
		VisualScriptPaletteIntent intent;
		intent.Position = position;
		intent.Query = std::move(query);
		return intent;
	}

	VisualScriptPaletteIntent MakeContextIntent(glm::vec2 position, uint32_t node, std::string pin, VisualScriptPinDirection direction)
	{
		VisualScriptPaletteIntent intent;
		intent.Position = position;
		intent.From = VisualScriptPinReference{node, std::move(pin)};
		intent.Direction = direction;
		return intent;
	}

	VisualScriptNode const* FindNode(VisualScriptDocument const& document, std::string_view type)
	{
		auto const found = std::find_if(document.Graph.Nodes.begin(), document.Graph.Nodes.end(),
		                                [type](VisualScriptNode const& node)
		                                {
											return node.Type == type;
										});
		return found != document.Graph.Nodes.end() ? &*found : nullptr;
	}

	bool HasLink(VisualScriptDocument const& document, VisualScriptLink const& link)
	{
		return std::find(document.Graph.Links.begin(), document.Graph.Links.end(), link) != document.Graph.Links.end();
	}

	// The window a child window's name ends with ("##rows" of the palette).
	ImGuiWindow* FindChildWindow(std::string_view suffix)
	{
		for (ImGuiWindow* const window : GImGui->Windows)
		{
			std::string_view const name = window->Name;
			if (window->Active && name.size() > suffix.size() && name.find(suffix) != std::string_view::npos)
			{
				return window;
			}
		}
		return nullptr;
	}
}

TEST_CASE("VisualScriptEditor: the palette opens with the keyboard in its search field, and typing filters its rows")
{
	PaneFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing(
		[&fixture]
		{
			fixture.Palette.Open(MakeIntent(glm::vec2(96.0f, 64.0f)));
		});
	REQUIRE(fixture.Palette.IsOpen());
	CHECK(fixture.Palette.GetState().Open);
	CHECK(GImGui->OpenPopupStack.Size == 1);
	// The search field has the keyboard: its hint shows over its empty text.
	ImGuiInputTextState const* const state = ImGui::GetInputTextState(GImGui->ActiveId);
	REQUIRE(state != nullptr);
	CHECK(GImGui->InputTextState.ID == GImGui->ActiveId);

	fixture.ImGui.Type("branch");
	fixture.Frames(2);
	CHECK(fixture.Palette.GetState().Query == "branch");
	std::vector<std::string> const titles = fixture.GetStateTitles();
	REQUIRE_FALSE(titles.empty());
	CHECK(titles.front() == "Branch");
	CHECK(fixture.Palette.GetState().Total >= titles.size());
}

TEST_CASE("VisualScriptEditor: what is typed before the palette's field has the keyboard is its query, and the cursor follows it")
{
	PaneFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Frames(1);
	// The query the canvas gives (keys typed while Space was down), then a key typed in the frame the palette opens.
	fixture.Palette.Open(MakeIntent(glm::vec2(0.0f), "br"));
	fixture.ImGui.Type("a");
	fixture.Frames(2);
	CHECK(fixture.Palette.GetState().Query == "bra");
	// Typing goes on after the text, never over it.
	fixture.ImGui.Type("nch");
	fixture.Frames(2);
	CHECK(fixture.Palette.GetState().Query == "branch");
}

TEST_CASE(
	"VisualScriptEditor: Down moves the palette's highlight, which the details strip follows, and Enter adds the first match in one step")
{
	PaneFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Palette.Open(MakeIntent(glm::vec2(100.0f, 205.0f), "b"));
	fixture.Frames(2);
	std::vector<uint32_t> const rows = fixture.Palette.GetRowEntries();
	REQUIRE(rows.size() >= 2);
	CHECK(fixture.Palette.GetHighlightedEntry() == rows[0]);
	fixture.ImGui.Press(ImGuiKey_DownArrow, fixture.Drawer());
	CHECK(fixture.Palette.GetHighlightedEntry() == rows[1]);
	fixture.ImGui.Press(ImGuiKey_UpArrow, fixture.Drawer());
	CHECK(fixture.Palette.GetHighlightedEntry() == rows[0]);
	fixture.ImGui.Type("ranch");
	fixture.Frames(2);
	REQUIRE_FALSE(fixture.GetStateTitles().empty());
	CHECK(fixture.GetStateTitles().front() == "Branch");

	size_t const undo = fixture.Context.GetHistory().GetUndoCount();
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	CHECK_FALSE(fixture.Palette.IsOpen());
	CHECK(fixture.Context.GetHistory().GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Add Branch in Graph");
	VisualScriptNode const* const branch = FindNode(fixture.GetDocument(), "Flow.Branch");
	REQUIRE(branch != nullptr);
	// At the point the palette was opened at, snapped to the grid.
	CHECK(branch->Position == std::optional<glm::vec2>(glm::vec2(96.0f, 208.0f)));
	CHECK(std::vector<uint32_t>(fixture.Model->GetSelection().begin(), fixture.Model->GetSelection().end()) ==
	      std::vector<uint32_t>{branch->ID});
	// Remembered as recent.
	REQUIRE_FALSE(fixture.Settings.GetRecents().empty());
	CHECK(fixture.Settings.GetRecents().front().Type == "Flow.Branch");
}

TEST_CASE("VisualScriptEditor: without a query the palette lists recent entries above its categories, all open while nothing is recent")
{
	PaneFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Palette.Open(MakeIntent(glm::vec2(0.0f)));
	fixture.Frames(2);
	std::vector<uint32_t> const opened = fixture.Palette.GetRowEntries();
	auto const lists = [&fixture](std::vector<uint32_t> const& rows, std::string_view title)
	{
		return std::any_of(rows.begin(), rows.end(),
		                   [&fixture, title](uint32_t entry)
		                   {
							   return fixture.Entry(entry).Title == title;
						   });
	};
	// Nothing is recent: the first level is open, the document's variables with it.
	CHECK(lists(opened, "Branch"));
	CHECK(lists(opened, "Get Speed"));
	CHECK(lists(opened, "Add Comment Box"));
	CHECK(fixture.Palette.GetState().Entries.empty());

	fixture.Palette.Close();
	fixture.Frames(1);
	REQUIRE(fixture.Settings.AddRecent({"Flow.Branch", Json::object()}));
	fixture.Palette.Open(MakeIntent(glm::vec2(0.0f)));
	fixture.Frames(2);
	std::vector<uint32_t> const withRecent = fixture.Palette.GetRowEntries();
	REQUIRE_FALSE(withRecent.empty());
	CHECK(fixture.Entry(withRecent.front()).Title == "Branch");
	CHECK_FALSE(lists(withRecent, "Get Speed"));
	CHECK(fixture.GetStateTitles() == std::vector<std::string>{"Branch"});
}

TEST_CASE("VisualScriptEditor: the palette reads the window's document again after a program change, not after a layout-only commit")
{
	PaneFixture fixture(MakeDocument());
	auto const listed = [&fixture](std::string_view title)
	{
		fixture.Palette.Open(MakeIntent(glm::vec2(0.0f)));
		fixture.Frames(2);
		std::vector<uint32_t> const rows = fixture.Palette.GetRowEntries();
		bool const found = std::any_of(rows.begin(), rows.end(),
		                               [&fixture, title](uint32_t entry)
		                               {
										   return fixture.Entry(entry).Title == title;
									   });
		fixture.Palette.Close();
		fixture.Frames(1);
		return found;
	};
	// A commit as the controller makes it, with its LayoutOnly.
	auto const commit = [&fixture](VisualScriptDocument document, bool layoutOnly)
	{
		Ref<VisualScriptDocument const> const stored = fixture.Model->GetStoredDocument();
		Result<VisualScriptCommitRequest> request =
			MakeVisualScriptCommitRequest(*stored, std::move(document), fixture.Model->GetAnalysis(), "Edit", "Graph", 0);
		REQUIRE(request.IsOk());
		VisualScriptCommitRequest made = request.TakeValue();
		made.LayoutOnly = layoutOnly;
		fixture.ExecuteCommit(std::move(made));
	};
	CHECK(listed("Get Speed"));
	uint64_t const reads = fixture.Palette.GetDocumentReadCount();
	CHECK(reads == 1);

	// A move: the document's entries come from names, signatures and events, never positions, so it is not read again.
	VisualScriptDocument moved = fixture.Model->GetDisplayDocument();
	moved.Graph.Nodes[0].Position = glm::vec2(32.0f, 0.0f);
	commit(moved, true);
	CHECK(listed("Get Speed"));
	CHECK(fixture.Palette.GetDocumentReadCount() == reads);

	// A variable added: read again, and offered.
	VisualScriptDocument added = fixture.Model->GetDisplayDocument();
	added.Variables.push_back(MakeVariable(102, "Score", "int"));
	commit(added, false);
	CHECK(listed("Get Score"));
	CHECK(fixture.Palette.GetDocumentReadCount() == reads + 1);
	// Opened again with nothing changed, it reads nothing.
	CHECK(listed("Get Score"));
	CHECK(fixture.Palette.GetDocumentReadCount() == reads + 1);
}

TEST_CASE("VisualScriptEditor: the context palette opened again from another pin lists what connects to that pin")
{
	PaneFixture fixture(MakeDocument());
	REQUIRE(fixture.Settings.IsContextPalette());
	auto const listed = [&fixture](uint32_t node, std::string const& pin)
	{
		fixture.Palette.Open(MakeContextIntent(glm::vec2(400.0f, 320.0f), node, pin, VisualScriptPinDirection::Out));
		fixture.Frames(2);
		REQUIRE(fixture.Palette.GetState().From.has_value());
		CHECK(*fixture.Palette.GetState().From == VisualScriptPinReference{node, pin});
		return fixture.GetStateTitles();
	};
	std::vector<std::string> const then = listed(1, "Then");
	fixture.Palette.Close();
	fixture.Frames(1);
	uint64_t const reads = fixture.Palette.GetDocumentReadCount();

	// The index is kept while nothing changed, but the ranking follows the pin.
	std::vector<std::string> const result = listed(3, "Result");
	CHECK(result != then);
	fixture.Palette.Close();
	fixture.Frames(1);
	CHECK(listed(1, "Then") == then);
	// Opened again over the open palette, from the other pin.
	CHECK(listed(3, "Result") == result);
	CHECK(fixture.Palette.GetDocumentReadCount() == reads);
}

TEST_CASE("VisualScriptEditor: in context mode the palette lists what connects, and offers every node when nothing does")
{
	PaneFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	VisualScriptLibrary const& library = fixture.Semantics.GetFixtureLibrary();
	fixture.Palette.Open(MakeContextIntent(glm::vec2(600.0f, 300.0f), 3, "Result", VisualScriptPinDirection::Out));
	fixture.Frames(2);
	VisualScriptPin result;
	result.Name = "Result";
	result.Direction = VisualScriptPinDirection::Out;
	result.Type = "bool";
	REQUIRE_FALSE(fixture.Palette.GetState().Entries.empty());
	for (VisualScriptPaletteMatch const& match : fixture.Palette.GetState().Entries)
	{
		VisualScriptPaletteEntry const& entry = fixture.Entry(match.Entry);
		auto const pin = std::find_if(entry.Pins.begin(), entry.Pins.end(),
		                              [&match](VisualScriptPin const& candidate)
		                              {
										  return candidate.Name == match.ConnectPin && candidate.Direction == VisualScriptPinDirection::In;
									  });
		REQUIRE_MESSAGE(pin != entry.Pins.end(), entry.Title);
		CHECK_MESSAGE(library.CanConnect(result, *pin), entry.Title);
	}

	// Nothing matches: "Show all nodes" unchecks Context.
	fixture.ImGui.Type("zzzz");
	fixture.Frames(2);
	CHECK(fixture.Palette.GetRowEntries().empty());
	ImGuiWindow const* const rows = FindChildWindow("##rows");
	REQUIRE(rows != nullptr);
	ImVec2 const button(rows->ContentRegionRect.Min.x + ImGui::GetFontSize(),
	                    rows->ContentRegionRect.Min.y + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() * 0.5f);
	fixture.ImGui.Click(button, fixture.Drawer());
	CHECK_FALSE(fixture.Settings.IsContextPalette());
	CHECK(fixture.Palette.IsOpen());
}

TEST_CASE("VisualScriptEditor: the palette's keys keep working when the pin it was opened from goes")
{
	PaneFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Palette.Open(MakeContextIntent(glm::vec2(400.0f, 320.0f), 3, "Result", VisualScriptPinDirection::Out));
	fixture.Frames(2);
	// The Raycast goes (another window, an undo): without its pin the palette lists every entry, and Down still moves.
	VisualScriptDocument without = fixture.GetDocument();
	std::erase_if(without.Graph.Nodes,
	              [](VisualScriptNode const& node)
	              {
					  return node.ID == 3;
				  });
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, without).IsOk());
	fixture.Frames(2);
	std::vector<uint32_t> const rows = fixture.Palette.GetRowEntries();
	REQUIRE(rows.size() >= 2);
	CHECK(fixture.Palette.GetHighlightedEntry() == rows[0]);
	fixture.ImGui.Press(ImGuiKey_DownArrow, fixture.Drawer());
	CHECK(fixture.Palette.GetHighlightedEntry() == rows[1]);
	CHECK(fixture.Palette.GetRowEntries() == rows);
}

TEST_CASE("VisualScriptEditor: a node chosen from an exec output's palette goes into its link, in one step")
{
	PaneFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Palette.Open(MakeContextIntent(glm::vec2(240.0f, 40.0f), 1, "Then", VisualScriptPinDirection::Out));
	fixture.Frames(1);
	fixture.ImGui.Type("delay");
	fixture.Frames(2);
	REQUIRE_FALSE(fixture.GetStateTitles().empty());
	CHECK(fixture.GetStateTitles().front() == "Delay");
	size_t const undo = fixture.Context.GetHistory().GetUndoCount();
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	CHECK(fixture.Context.GetHistory().GetUndoCount() == undo + 1);
	VisualScriptDocument const& document = fixture.GetDocument();
	VisualScriptNode const* const delay = FindNode(document, "Flow.Delay");
	REQUIRE(delay != nullptr);
	CHECK(HasLink(document, {{1, "Then"}, {delay->ID, "Execute"}}));
	CHECK(HasLink(document, {{delay->ID, "Completed"}, {2, "Execute"}}));
	CHECK_FALSE(HasLink(document, {{1, "Then"}, {2, "Execute"}}));
	// The pin it connects sits where the wire was released: Execute is the first row, at the node's left edge.
	REQUIRE(delay->Position);
	// 240 - 10 snapped to the grid.
	CHECK(delay->Position->x == 224.0f);
}

TEST_CASE("VisualScriptEditor: a node fed by a value of an impure node whose flow ends there follows it in the flow too")
{
	PaneFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Palette.Open(MakeContextIntent(glm::vec2(400.0f, 320.0f), 3, "Result", VisualScriptPinDirection::Out));
	fixture.Frames(1);
	fixture.ImGui.Type("set alive");
	fixture.Frames(2);
	REQUIRE_FALSE(fixture.GetStateTitles().empty());
	CHECK(fixture.GetStateTitles().front() == "Set Alive");
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Add Set Alive in Graph");
	VisualScriptDocument const& document = fixture.GetDocument();
	VisualScriptNode const* const set = FindNode(document, "Variable.Set");
	REQUIRE(set != nullptr);
	CHECK(HasLink(document, {{3, "Result"}, {set->ID, "Value"}}));
	CHECK(HasLink(document, {{3, "Then"}, {set->ID, "Execute"}}));
}

TEST_CASE("VisualScriptEditor: an event the graph handles already is offered as a jump to it, and a palette needs the node library")
{
	PaneFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Palette.Open(MakeIntent(glm::vec2(0.0f), "update"));
	fixture.Frames(2);
	REQUIRE_FALSE(fixture.GetStateTitles().empty());
	CHECK(fixture.GetStateTitles().front() == "Update (go to)");
	size_t const undo = fixture.Context.GetHistory().GetUndoCount();
	fixture.Results.clear();
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	CHECK(fixture.Context.GetHistory().GetUndoCount() == undo);
	bool const framed = std::any_of(fixture.Results.begin(), fixture.Results.end(),
	                                [](VisualScriptPaneResult const& result)
	                                {
										return result.SelectAndFrame == std::vector<uint32_t>{1};
									});
	CHECK(framed);

	// Without the scripting runtime the palette does not open.
	fixture.Semantics.SetAvailable(false);
	fixture.CheckDrawingChangesNothing(
		[&fixture]
		{
			fixture.Palette.Open(MakeIntent(glm::vec2(0.0f), "branch"));
		});
	CHECK_FALSE(fixture.Palette.IsOpen());
	CHECK_FALSE(fixture.Palette.GetState().Open);
	CHECK(GImGui->OpenPopupStack.Size == 0);
}

TEST_CASE("VisualScriptEditor: the palette's tooltip names a reflected node's type with the injected monospace font and with none")
{
	for (bool const injected : {true, false})
	{
		PaneFixture fixture(MakeDocument());
		fixture.CheckDrawingChangesNothing();
		fixture.Services.MonospaceFont = injected ? fixture.Fonts.Monospace : nullptr;
		fixture.Palette.Open(MakeIntent(glm::vec2(0.0f), "log info"));
		fixture.Frames(2);
		REQUIRE_FALSE(fixture.Palette.GetRowEntries().empty());
		ImGuiWindow const* const rows = FindChildWindow("##rows");
		REQUIRE(rows != nullptr);
		// Hover the first row until the tooltip shows.
		ImVec2 const row(rows->ContentRegionRect.Min.x + 40.0f, rows->ContentRegionRect.Min.y + ImGui::GetTextLineHeight() * 0.5f);
		ImGui::GetIO().AddMousePosEvent(row.x, row.y);
		for (int frame = 0; frame < 40; frame++)
		{
			fixture.ImGui.Frame(fixture.Drawer());
		}
		bool const tooltip = std::any_of(GImGui->Windows.begin(), GImGui->Windows.end(),
		                                 [](ImGuiWindow const* window)
		                                 {
											 return window->Active && (window->Flags & ImGuiWindowFlags_Tooltip) != 0;
										 });
		CHECK(tooltip);
	}
}

// --- The literal editor ---

TEST_CASE("VisualScriptEditor: literals are described for FieldEditor and their edits written canonical, as the codec writes them")
{
	Testing::FixtureVisualScriptSemantics semantics;
	VisualScriptLibrary const& library = semantics.GetFixtureLibrary();

	// A float takes its fallback (the pin's default) without a literal, and is written as the shortest number.
	std::optional<VisualScriptLiteralField> const duration = DescribeVisualScriptLiteral("float", Json(), 0.2, &library);
	REQUIRE(duration);
	CHECK(duration->Descriptor.Kind == FieldKind::Float);
	CHECK(duration->Value.get<float>() == 0.2f);
	FieldChange change;
	change.Value = 1.5f;
	CHECK(EncodeVisualScriptLiteralChange(*duration, change, &library) == Json(1.5));

	// One component of a vector changes; the others stay.
	std::optional<VisualScriptLiteralField> const vector =
		DescribeVisualScriptLiteral("Strada.Vector3", Json::array({1, 2, 3}), Json(), &library);
	REQUIRE(vector);
	change = {};
	change.Value = Json::array({1.0f, 5.0f, 3.0f});
	change.Component = 1;
	CHECK(EncodeVisualScriptLiteralChange(*vector, change, &library) == Json::array({1, 5, 3}));

	// Enums by name; a literal no enumerator has is invalid and shows the fallback.
	std::optional<VisualScriptLiteralField> const mode = DescribeVisualScriptLiteral("Strada.ForceMode", "Bogus", "Impulse", &library);
	REQUIRE(mode);
	CHECK(mode->Invalid);
	CHECK(mode->Descriptor.Kind == FieldKind::Enum);
	CHECK(mode->Value == "Impulse");
	change = {};
	change.Value = "VelocityChange";
	CHECK(EncodeVisualScriptLiteralChange(*mode, change, &library) == Json("VelocityChange"));

	// 64-bit integers are decimal strings in documents and numbers in FieldEditor.
	std::optional<VisualScriptLiteralField> const big = DescribeVisualScriptLiteral("long", "-9000000000", Json(), &library);
	REQUIRE(big);
	CHECK(big->Value == Json(int64_t(-9000000000)));
	change = {};
	change.Value = int64_t(9000000001);
	CHECK(EncodeVisualScriptLiteralChange(*big, change, &library) == Json("9000000001"));

	// Colours stay linear; entities and types without literals have no widget; Matrix4 is a grid of its own.
	std::optional<VisualScriptLiteralField> const color = DescribeVisualScriptLiteral("Strada.Color", Json(), Json(), &library);
	REQUIRE(color);
	CHECK(color->Value == Json::array({1.0, 1.0, 1.0, 1.0}));
	CHECK_FALSE(DescribeVisualScriptLiteral("Strada.Entity", "0", Json(), &library));
	CHECK_FALSE(DescribeVisualScriptLiteral("Strada.RaycastHit", Json(), Json(), &library));
	CHECK_FALSE(VisualScriptLiteralEditor::CanEdit("Strada.Entity", &library));
	CHECK(VisualScriptLiteralEditor::CanEdit("Strada.Matrix4", &library));
	CHECK(VisualScriptLiteralEditor::CanEdit("float", nullptr));
	CHECK_FALSE(VisualScriptLiteralEditor::CanEdit("Strada.ForceMode", nullptr));

	std::array<float, 16> matrix = ReadVisualScriptMatrixLiteral(Json(), Json());
	CHECK(matrix[0] == 1.0f);
	CHECK(matrix[5] == 1.0f);
	CHECK(matrix[1] == 0.0f);
	matrix[12] = 0.2f;
	Json const written = WriteVisualScriptMatrixLiteral(matrix);
	REQUIRE(written.size() == 16);
	CHECK(written[12] == Json(0.2));
	CHECK(ReadVisualScriptMatrixLiteral(written, Json()) == matrix);
}

TEST_CASE("VisualScriptEditor: the literal editor writes a [Flags] pin's value as an array of names and an enum's as its name")
{
	// The node library has no [Flags] enum: a library of one.
	Result<Scope<VisualScriptLibrary>> parsed = VisualScriptLibrary::Parse(R"({
		"Types": [
			{"ID": "Game.Layers", "Title": "Layers", "Kind": "Enum", "FieldType": "Int32", "Literal": true, "Flags": true,
			 "Enumerators": [{"Name": "None", "Value": 0}, {"Name": "Ground", "Value": 1}, {"Name": "Water", "Value": 2}],
			 "ImplicitTo": []},
			{"ID": "Game.Mode", "Title": "Mode", "Kind": "Enum", "FieldType": "Int32", "Literal": true, "Flags": false,
			 "Enumerators": [{"Name": "Walk", "Value": 0}, {"Name": "Swim", "Value": 1}], "ImplicitTo": []}
		],
		"Nodes": []
	})");
	REQUIRE_MESSAGE(parsed.IsOk(), parsed.GetError());
	Scope<VisualScriptLibrary> const library = parsed.TakeValue();
	Testing::HeadlessImGui headless;
	headless.UseEditorStyle();
	VisualScriptLiteralEditor editor;
	std::optional<Json> flags;
	std::optional<Json> mode;
	ImVec2 flagsFrame;
	ImVec2 modeFrame;
	auto const draw = [&]
	{
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
		ImGui::Begin("Literals", nullptr, ImGuiWindowFlags_NoSavedSettings);
		ImGui::PushID("flags");
		flagsFrame = ImGui::GetCursorScreenPos();
		if (std::optional<Json> edited = editor.Draw("Game.Layers", Json::array({"Ground"}), Json(), library.get()))
		{
			flags = std::move(edited);
		}
		ImGui::PopID();
		ImGui::PushID("mode");
		modeFrame = ImGui::GetCursorScreenPos();
		if (std::optional<Json> edited = editor.Draw("Game.Mode", "Walk", Json(), library.get()))
		{
			mode = std::move(edited);
		}
		ImGui::PopID();
		ImGui::End();
	};
	// The panel invariant's form here: drawing alone returns no literal and opens nothing.
	for (int frame = 0; frame < 3; frame++)
	{
		headless.Frame(draw);
	}
	CHECK_FALSE(flags);
	CHECK_FALSE(mode);
	CHECK(GImGui->OpenPopupStack.Size == 0);
	float const frameHeight = ImGui::GetFrameHeight();
	// Open the flags' combo and tick Water: the first check box is Ground (None sets no flag).
	headless.Click(ImVec2(flagsFrame.x + 20.0f, flagsFrame.y + frameHeight * 0.5f), draw);
	// A popup is laid out hidden in its first frame and placed in the next.
	headless.Frame(draw);
	headless.Frame(draw);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	ImGuiWindow const* const popup = GImGui->OpenPopupStack[0].Window;
	REQUIRE(popup != nullptr);
	ImVec2 const water(popup->ContentRegionRect.Min.x + frameHeight * 0.5f,
	                   popup->ContentRegionRect.Min.y + frameHeight + ImGui::GetStyle().ItemSpacing.y + frameHeight * 0.5f);
	headless.Click(water, draw);
	REQUIRE(flags);
	CHECK(*flags == Json::array({"Ground", "Water"}));
	headless.Press(ImGuiKey_Escape, draw);
	headless.Frame(draw);

	// The enum's combo: Swim is its second row.
	headless.Click(ImVec2(modeFrame.x + 20.0f, modeFrame.y + frameHeight * 0.5f), draw);
	headless.Frame(draw);
	headless.Frame(draw);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	ImGuiWindow const* const list = GImGui->OpenPopupStack[0].Window;
	REQUIRE(list != nullptr);
	headless.Click(ImVec2(list->ContentRegionRect.Min.x + 10.0f,
	                      list->ContentRegionRect.Min.y + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetTextLineHeight() * 0.5f),
	               draw);
	REQUIRE(mode);
	CHECK(*mode == Json("Swim"));
}

// --- FieldEditor's inline values ---

namespace
{
	FieldDescriptor MakeField(FieldKind kind, std::string_view name = "Value")
	{
		FieldDescriptor field;
		field.Name = name;
		field.Kind = kind;
		return field;
	}

	// A test window that draws one value with DrawInlineValue, records its changes and requests, and opens the popup a compact
	// widget asks for as a caller does, drawing its body with DrawValuePopup.
	struct InlineFixture
	{
		InlineFixture() { Headless.UseEditorStyle(); }

		void Draw()
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(800.0f, 600.0f));
			ImGui::Begin("Inline", nullptr, ImGuiWindowFlags_NoSavedSettings);
			Table = GImGui->CurrentTable;
			Origin = ImGui::GetCursorScreenPos();
			ImGui::SetNextItemWidth(240.0f);
			FieldChange change;
			bool requested = false;
			if (Editor.DrawInlineValue(Field, Value, false, change, Mode, &requested))
			{
				Changes.push_back(change);
				Value = change.Value;
			}
			if (requested)
			{
				Requests++;
				if (OpenOnRequest)
				{
					ImGui::OpenPopup("##valuePopup");
				}
			}
			if (ImGui::BeginPopup("##valuePopup"))
			{
				Popup = ImGui::GetCurrentWindow();
				FieldChange popupChange;
				if (Editor.DrawValuePopup(Field, Value, popupChange))
				{
					Changes.push_back(popupChange);
					Value = popupChange.Value;
				}
				ImGui::EndPopup();
			}
			else
			{
				Popup = nullptr;
			}
			ImGui::End();
		}

		std::function<void()> Drawer()
		{
			return [this]
			{
				Draw();
			};
		}

		// The panel invariant's form for a lone widget: drawing alone changes no value, asks for no popup and opens none.
		void CheckDrawingChangesNothing()
		{
			for (int frame = 0; frame < 3; frame++)
			{
				Headless.Frame(Drawer());
			}
			CHECK(Changes.empty());
			CHECK(Requests == 0);
			CHECK(GImGui->OpenPopupStack.Size == 0);
		}

		ImVec2 FrameCenter() const { return ImVec2(Origin.x + 60.0f, Origin.y + ImGui::GetFrameHeight() * 0.5f); }

		Testing::HeadlessImGui Headless;
		FieldEditor Editor;
		FieldDescriptor Field;
		Json Value;
		FieldInlineMode Mode = FieldInlineMode::Compact;
		bool OpenOnRequest = true;
		std::vector<FieldChange> Changes;
		int Requests = 0;
		ImGuiTable* Table = nullptr;
		ImGuiWindow* Popup = nullptr;
		ImVec2 Origin;
	};

	std::vector<std::string_view> const& GetKeyNames()
	{
		static std::vector<std::string> const s_Names = []
		{
			Testing::FixtureVisualScriptSemantics semantics;
			std::optional<VisualScriptEnumInfo> const keys = semantics.GetFixtureLibrary().FindEnum("Strada.KeyCode");
			REQUIRE(keys);
			std::vector<std::string> names;
			for (ScriptEnumerator const& enumerator : keys->Enumerators)
			{
				names.push_back(enumerator.Name);
			}
			return names;
		}();
		static std::vector<std::string_view> const s_Views(s_Names.begin(), s_Names.end());
		return s_Views;
	}
}

TEST_CASE("VisualScriptEditor: a colour too narrow for its channels' numbers shows its swatch alone, as in a row of Details")
{
	Testing::FixtureVisualScriptSemantics semantics;
	VisualScriptLibrary const& library = semantics.GetFixtureLibrary();
	Testing::HeadlessImGui headless;
	headless.UseEditorStyle();
	VisualScriptLiteralEditor editor;
	Json const orange = Json::array({1.0, 0.45, 0.15, 1.0});
	for (float const scale : {1.0f, 1.75f})
	{
		CAPTURE(scale);
		ImGui::GetStyle().FontScaleDpi = scale;
		// 120 pixels at a font size of 15 leave each channel about 20, too few for "255" beside its marker; 400 about 90.
		for (float const width : {120.0f, 400.0f})
		{
			CAPTURE(width);
			ImVec2 origin;
			float frameHeight = 0.0f;
			auto const draw = [&]
			{
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(1200.0f, 300.0f));
				ImGui::Begin("Colour", nullptr, ImGuiWindowFlags_NoSavedSettings);
				origin = ImGui::GetCursorScreenPos();
				frameHeight = ImGui::GetFrameHeight();
				ImGui::PushItemWidth(width * scale);
				CHECK_FALSE(editor.Draw("Strada.Color", orange, Json(), &library).has_value());
				ImGui::PopItemWidth();
				ImGui::End();
			};
			headless.Frame(draw);
			// Beside the place of the swatch drawn alone: a channel's field there, or nothing.
			ImGui::GetIO().AddMousePosEvent(origin.x + frameHeight * 1.5f, origin.y + frameHeight * 0.5f);
			headless.Frame(draw);
			headless.Frame(draw);
			CHECK((GImGui->HoveredId != 0) == (width == 400.0f));
			// The swatch is there either way: after the fields, or first when alone.
			float const swatch = width == 400.0f ? width * scale - frameHeight * 0.5f : frameHeight * 0.5f;
			ImGui::GetIO().AddMousePosEvent(origin.x + swatch, origin.y + frameHeight * 0.5f);
			headless.Frame(draw);
			headless.Frame(draw);
			CHECK(GImGui->HoveredId != 0);
		}
	}
	ImGui::GetStyle().FontScaleDpi = 1.0f;
}

TEST_CASE("VisualScriptEditor: FieldEditor draws a value outside a table, and a compact float drags as the canvas shows it")
{
	InlineFixture fixture;
	fixture.Field = MakeField(FieldKind::Float);
	fixture.Field.TypeMin = -1000.0;
	fixture.Field.TypeMax = 1000.0;
	fixture.Value = 2.5;
	fixture.Mode = FieldInlineMode::Normal;
	fixture.CheckDrawingChangesNothing();
	CHECK(fixture.Table == nullptr);
	CHECK(fixture.Changes.empty());
	fixture.Headless.DragPath(fixture.FrameCenter(), ImVec2(fixture.FrameCenter().x + 60.0f, fixture.FrameCenter().y), fixture.Drawer());
	REQUIRE_FALSE(fixture.Changes.empty());
	CHECK(fixture.Changes.back().Value.get<float>() > 2.5f);
	CHECK(fixture.Changes.back().Path.empty());

	InlineFixture compact;
	compact.Field = MakeField(FieldKind::Vec3);
	compact.Value = Json::array({1.0, 2.0, 3.0});
	compact.CheckDrawingChangesNothing();
	// One drag per component, no axis buttons: the middle third is Y.
	ImVec2 const y(compact.Origin.x + 120.0f, compact.Origin.y + ImGui::GetFrameHeight() * 0.5f);
	compact.Headless.DragPath(y, ImVec2(y.x + 40.0f, y.y), compact.Drawer());
	REQUIRE_FALSE(compact.Changes.empty());
	CHECK(compact.Changes.back().Component == std::optional<uint32_t>(1));
	CHECK(compact.Value[0] == Json(1.0f));
	CHECK(compact.Value[2] == Json(3.0f));
}

TEST_CASE("VisualScriptEditor: compact enums, colours and assets ask for a popup instead of opening one, and DrawValuePopup edits each")
{
	SUBCASE("an enum")
	{
		InlineFixture fixture;
		fixture.Field = MakeField(FieldKind::Enum);
		fixture.Field.EnumValues = {"Force", "Impulse", "Acceleration"};
		fixture.Value = "Force";
		fixture.OpenOnRequest = false;
		fixture.CheckDrawingChangesNothing();
		fixture.Headless.Click(fixture.FrameCenter(), fixture.Drawer());
		CHECK(fixture.Requests == 1);
		CHECK(GImGui->OpenPopupStack.Size == 0);

		fixture.OpenOnRequest = true;
		fixture.Headless.Click(fixture.FrameCenter(), fixture.Drawer());
		REQUIRE(fixture.Popup != nullptr);
		ImVec2 const impulse(fixture.Popup->ContentRegionRect.Min.x + 10.0f,
		                     fixture.Popup->ContentRegionRect.Min.y + ImGui::GetTextLineHeightWithSpacing() + 4.0f);
		fixture.Headless.Click(impulse, fixture.Drawer());
		REQUIRE_FALSE(fixture.Changes.empty());
		CHECK(fixture.Changes.back().Value == "Impulse");
		CHECK(GImGui->OpenPopupStack.Size == 0);
	}

	SUBCASE("a colour")
	{
		InlineFixture fixture;
		fixture.Field = MakeField(FieldKind::Vec4);
		fixture.Field.Hints.Display = FieldDisplay::Color;
		fixture.Field.TypeMin = 0.0;
		fixture.Field.TypeMax = 1.0;
		fixture.Value = Json::array({0.5, 0.25, 0.1, 1.0});
		fixture.OpenOnRequest = false;
		fixture.CheckDrawingChangesNothing();
		fixture.Headless.Click(fixture.FrameCenter(), fixture.Drawer());
		CHECK(fixture.Requests == 1);
		CHECK(GImGui->OpenPopupStack.Size == 0);

		fixture.OpenOnRequest = true;
		fixture.Headless.Click(fixture.FrameCenter(), fixture.Drawer());
		REQUIRE(fixture.Popup != nullptr);
		// Drag across the picker's saturation and value square, its first part.
		ImVec2 const square = fixture.Popup->ContentRegionRect.Min;
		fixture.Headless.DragPath(ImVec2(square.x + 20.0f, square.y + 20.0f), ImVec2(square.x + 60.0f, square.y + 60.0f), fixture.Drawer());
		REQUIRE_FALSE(fixture.Changes.empty());
		Json const& color = fixture.Changes.back().Value;
		REQUIRE(color.size() == 4);
		CHECK(color[3] == Json(1.0f));
	}

	SUBCASE("an asset")
	{
		Testing::AssetManagerScope assets;
		Testing::TemporaryDirectory temporary;
		Testing::GraphProject const project = Testing::MakeGraphProject(temporary, VisualScriptDocument());
		InlineFixture fixture;
		fixture.Field = MakeField(FieldKind::Asset);
		fixture.Field.Hints.AssetTypeName = AssetTypeToString(AssetType::VisualScript);
		fixture.Value = AssetHandle().ToString();
		fixture.OpenOnRequest = false;
		fixture.CheckDrawingChangesNothing();
		fixture.Headless.Click(fixture.FrameCenter(), fixture.Drawer());
		CHECK(fixture.Requests == 1);
		CHECK(GImGui->OpenPopupStack.Size == 0);

		fixture.OpenOnRequest = true;
		fixture.Headless.Click(fixture.FrameCenter(), fixture.Drawer());
		REQUIRE(fixture.Popup != nullptr);
		// The picker's filter has the keyboard: type part of the path and take the first match.
		fixture.Headless.Type("graph");
		fixture.Headless.Frame(fixture.Drawer());
		fixture.Headless.Press(ImGuiKey_Enter, fixture.Drawer());
		REQUIRE_FALSE(fixture.Changes.empty());
		CHECK(fixture.Changes.back().Value == project.Graph.ToString());
	}
}

TEST_CASE(
	"VisualScriptEditor: typing in a long enum's list filters it, and Enter takes the best match, in popups and the inspector's combo")
{
	std::vector<std::string_view> const& keys = GetKeyNames();
	REQUIRE(keys.size() == 121);
	FieldDescriptor field = MakeField(FieldKind::Enum, "Key");
	field.EnumValues = keys;

	SUBCASE("a compact widget's popup")
	{
		InlineFixture fixture;
		fixture.Field = field;
		fixture.Value = "Space";
		fixture.CheckDrawingChangesNothing();
		fixture.Headless.Click(fixture.FrameCenter(), fixture.Drawer());
		REQUIRE(fixture.Popup != nullptr);
		fixture.Headless.Type("w");
		fixture.Headless.Frame(fixture.Drawer());
		fixture.Headless.Press(ImGuiKey_Enter, fixture.Drawer());
		REQUIRE_FALSE(fixture.Changes.empty());
		CHECK(fixture.Changes.back().Value == "W");
		CHECK(GImGui->OpenPopupStack.Size == 0);
	}

	SUBCASE("the inspector's combo")
	{
		Testing::HeadlessImGui headless;
		headless.UseEditorStyle();
		FieldEditor editor;
		std::vector<FieldDescriptor> const fields = {field};
		Json values = Json::object({{"Key", "Space"}});
		std::optional<FieldChange> change;
		ImVec2 combo;
		auto const draw = [&]
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(600.0f, 600.0f));
			ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoSavedSettings);
			combo = ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - 60.0f,
			               ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight() * 0.5f);
			if (std::optional<FieldChange> drawn = editor.Draw(fields, values))
			{
				change = std::move(drawn);
			}
			ImGui::End();
		};
		for (int frame = 0; frame < 3; frame++)
		{
			headless.Frame(draw);
		}
		CHECK_FALSE(change);
		CHECK(GImGui->OpenPopupStack.Size == 0);
		headless.Click(combo, draw);
		REQUIRE(GImGui->OpenPopupStack.Size == 1);
		headless.Type("w");
		headless.Frame(draw);
		headless.Press(ImGuiKey_Enter, draw);
		REQUIRE(change);
		CHECK(change->Value == "W");
		CHECK(change->Path == std::vector<std::string_view>{"Key"});
		CHECK(GImGui->OpenPopupStack.Size == 0);
	}
}

TEST_CASE("VisualScriptEditor: the asset picker opens in a popup of its own and returns the asset chosen")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	Testing::GraphProject const project = Testing::MakeGraphProject(temporary, VisualScriptDocument());
	Testing::HeadlessImGui headless;
	headless.UseEditorStyle();
	FieldEditor editor;
	bool open = false;
	std::optional<AssetHandle> chosen;
	auto const draw = [&]
	{
		ImGui::Begin("Picker", nullptr, ImGuiWindowFlags_NoSavedSettings);
		if (open)
		{
			ImGui::OpenPopup("##graphs");
			open = false;
		}
		if (std::optional<AssetHandle> picked = editor.DrawAssetPickerPopup("##graphs", AssetType::VisualScript, AssetHandle(), true))
		{
			chosen = picked;
		}
		ImGui::End();
	};
	for (int frame = 0; frame < 3; frame++)
	{
		headless.Frame(draw);
	}
	CHECK_FALSE(chosen);
	CHECK(GImGui->OpenPopupStack.Size == 0);
	open = true;
	headless.Frame(draw);
	headless.Frame(draw);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	headless.Press(ImGuiKey_DownArrow, draw);
	headless.Press(ImGuiKey_Enter, draw);
	REQUIRE(chosen);
	CHECK(*chosen == project.Graph);
	CHECK(GImGui->OpenPopupStack.Size == 0);
}

TEST_CASE("VisualScriptEditor: My Script rows drag as one payload of their graph, kind and ID, and pack into the canvas' drop")
{
	VisualScriptElementPayload const payload{42, static_cast<uint32_t>(VisualScriptElementKind::Local), 7};
	CHECK(payload.Graph == 42);
	uint64_t const packed = PackVisualScriptElement(VisualScriptElementKind::CustomEvent, 0xFFFFFFFFu);
	CHECK(UnpackVisualScriptElement(packed) == std::pair(VisualScriptElementKind::CustomEvent, 0xFFFFFFFFu));
	CHECK(UnpackVisualScriptElement(PackVisualScriptElement(VisualScriptElementKind::Variable, 3)) ==
	      std::pair(VisualScriptElementKind::Variable, 3u));
	CHECK_FALSE(UnpackVisualScriptElement(uint64_t(9) << 32));
	CHECK(std::string_view(DragDropPayload::VisualScriptElement) == "STRADA_VISUAL_SCRIPT_ELEMENT");
}

// --- Details ---

namespace
{
	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	VisualScriptGraph const& GetGraph(VisualScriptDocument const& document, uint32_t function)
	{
		VisualScriptGraph const* const graph = FindVisualScriptGraph(document, function);
		REQUIRE(graph != nullptr);
		return *graph;
	}

	bool HasLinkIn(VisualScriptGraph const& graph, VisualScriptLink const& link)
	{
		return std::find(graph.Links.begin(), graph.Links.end(), link) != graph.Links.end();
	}

	// Update → a Sequence of three outputs, each to a Log.
	VisualScriptDocument MakeSequenceDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes = {MakeNode(1, "Event.Update", glm::vec2(0.0f)),
		                        MakeNode(2, "Flow.Sequence", glm::vec2(240.0f, 0.0f), Json::object({{"Count", 3}})),
		                        MakeNode(3, "Call:Strada.Log.Info(string)", glm::vec2(560.0f, 0.0f)),
		                        MakeNode(4, "Call:Strada.Log.Info(string)", glm::vec2(560.0f, 160.0f)),
		                        MakeNode(5, "Call:Strada.Log.Info(string)", glm::vec2(560.0f, 320.0f))};
		document.Graph.Links = {MakeLink(1, "Then", 2, "Execute"), MakeLink(2, "Then0", 3, "Execute"), MakeLink(2, "Then1", 4, "Execute"),
		                        MakeLink(2, "Then2", 5, "Execute")};
		return document;
	}

	// A function AddScore(Amount: int) : Result whose entry passes Amount to its return, and a call of it fed by a variable.
	VisualScriptDocument MakeFunctionDocument()
	{
		VisualScriptDocument document;
		document.Variables = {MakeVariable(100, "Score", "int")};
		VisualScriptFunction function;
		function.ID = 10;
		function.Name = "AddScore";
		function.Inputs = {{"Amount", "int"}};
		function.Outputs = {{"Result", "int"}};
		function.Graph.Nodes = {MakeNode(11, "Function.Entry", glm::vec2(0.0f)), MakeNode(12, "Function.Return", glm::vec2(480.0f, 0.0f))};
		function.Graph.Links = {MakeLink(11, "Then", 12, "Execute"), MakeLink(11, "Amount", 12, "Result")};
		document.Functions.push_back(std::move(function));
		document.Graph.Nodes = {MakeNode(1, "Event.Update", glm::vec2(0.0f)),
		                        MakeNode(2, "Function.Call", glm::vec2(320.0f, 0.0f), Json::object({{"Function", 10}})),
		                        MakeNode(3, "Variable.Get", glm::vec2(0.0f, 200.0f), Json::object({{"Variable", 100}}))};
		document.Graph.Links = {MakeLink(1, "Then", 2, "Execute"), MakeLink(3, "Value", 2, "Amount")};
		return document;
	}

	// A point of the Details window where nothing is drawn: a click there leaves the field that has the keyboard.
	ImVec2 GetDetailsVoid()
	{
		return ImVec2(PaneFixture::DetailsPosition.x + PaneFixture::DetailsSize.x * 0.5f, PaneFixture::DetailsSize.y - 20.0f);
	}
}

TEST_CASE("VisualScriptEditor: the Easing plots follow the curves the scripting runtime eases Tween's Alpha with")
{
	// The values of ScriptCoreTests' TweenIntervalTests (spec v2 §4.2 at t = 0.25, 0.5, 0.75 and 1, computed independently).
	struct Expected
	{
		char const* Easing;
		std::array<float, 4> Alphas;
	};
	constexpr std::array<Expected, 10> Table = {{
		{"Linear", {0.25f, 0.5f, 0.75f, 1.0f}},
		{"EaseIn", {0.0625f, 0.25f, 0.5625f, 1.0f}},
		{"EaseOut", {0.4375f, 0.75f, 0.9375f, 1.0f}},
		{"EaseInOut", {0.125f, 0.5f, 0.875f, 1.0f}},
		{"EaseInCubic", {0.015625f, 0.125f, 0.421875f, 1.0f}},
		{"EaseOutCubic", {0.578125f, 0.875f, 0.984375f, 1.0f}},
		{"EaseInOutCubic", {0.0625f, 0.5f, 0.9375f, 1.0f}},
		{"EaseOutBack", {0.817409694f, 1.08769751f, 1.06413651f, 1.0f}},
		{"EaseOutBounce", {0.47265625f, 0.765625f, 0.97265625f, 1.0f}},
		{"EaseOutElastic", {0.911611676f, 1.015625f, 1.00552428f, 1.0f}},
	}};
	// Every choice of the fixture library's Tween is one of them.
	Testing::FixtureVisualScriptSemantics semantics;
	VisualScriptNodeTemplate const* const tween = semantics.GetFixtureLibrary().FindNode("Flow.Tween");
	REQUIRE(tween != nullptr);
	REQUIRE(tween->Settings.size() == 1);
	CHECK(tween->Settings.front().Choices.size() == Table.size());
	for (Expected const& expected : Table)
	{
		INFO(expected.Easing);
		CHECK(std::find(tween->Settings.front().Choices.begin(), tween->Settings.front().Choices.end(), expected.Easing) !=
		      tween->Settings.front().Choices.end());
		REQUIRE(EvaluateVisualScriptEasing(expected.Easing, 0.0));
		CHECK(std::abs(*EvaluateVisualScriptEasing(expected.Easing, 0.0)) <= 1e-12);
		for (size_t i = 0; i < expected.Alphas.size(); i++)
		{
			double const t = 0.25 * static_cast<double>(i + 1);
			std::optional<double> const alpha = EvaluateVisualScriptEasing(expected.Easing, t);
			REQUIRE(alpha);
			CHECK(std::abs(static_cast<float>(*alpha) - expected.Alphas[i]) <= 1e-6f);
		}
	}
	CHECK_FALSE(EvaluateVisualScriptEasing("Wobble", 0.5));
	// Outside [0, 1] the curve holds its ends.
	CHECK(*EvaluateVisualScriptEasing("EaseOutBack", 2.0) == doctest::Approx(1.0));
	CHECK(*EvaluateVisualScriptEasing("EaseIn", -1.0) == doctest::Approx(0.0));
}

TEST_CASE("VisualScriptEditor: lowering a Sequence's Count in Details removes the link of its last output in the same step")
{
	PaneFixture fixture(MakeSequenceDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({2});
	CHECK(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Node);
	size_t const undo = fixture.GetUndoCount();
	// A double click types the count.
	fixture.ImGui.Click(fixture.DetailsRow("Count"), fixture.Drawer(), 2);
	fixture.ImGui.Type("2");
	fixture.Frames(2);
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Count on Sequence (removes 1 link) in Graph");
	VisualScriptGraph const& graph = GetGraph(fixture.GetDocument(), 0);
	CHECK(FindNode(fixture.GetDocument(), "Flow.Sequence")->Settings.value("Count", Json()) == 2);
	CHECK(HasLinkIn(graph, MakeLink(2, "Then0", 3, "Execute")));
	CHECK(HasLinkIn(graph, MakeLink(2, "Then1", 4, "Execute")));
	CHECK_FALSE(HasLinkIn(graph, MakeLink(2, "Then2", 5, "Execute")));
	CHECK(graph.Links.size() == 3);
}

TEST_CASE("VisualScriptEditor: Remove Output on a Sequence's output in Details renumbers the outputs after it, in one step")
{
	PaneFixture fixture(MakeSequenceDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({2});
	size_t const undo = fixture.GetUndoCount();
	VisualScriptDocument const before = fixture.GetDocument();
	fixture.ImGui.Click(fixture.DetailsRow("Then1"), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Remove Output Then1 of Sequence (removes 1 link) in Graph");
	VisualScriptGraph const& graph = GetGraph(fixture.GetDocument(), 0);
	CHECK(FindNode(fixture.GetDocument(), "Flow.Sequence")->Settings.value("Count", Json()) == 2);
	CHECK(HasLinkIn(graph, MakeLink(2, "Then0", 3, "Execute")));
	// Then2's link is Then1's now; Then1's went.
	CHECK(HasLinkIn(graph, MakeLink(2, "Then1", 5, "Execute")));
	CHECK(std::none_of(graph.Links.begin(), graph.Links.end(),
	                   [](VisualScriptLink const& link)
	                   {
						   return link.To.Node == 4;
					   }));
	// The rows follow the count.
	CHECK(fixture.Details.FindRow("Then1"));
	CHECK_FALSE(fixture.Details.FindRow("Then2"));
	// graph.edit's operation for the same edit writes the same document (one implementation).
	Json const operations = Json::array({Json::object({{"op", "SetSequenceOutputs"}, {"node", 2}, {"count", 2}, {"removeAt", 1}})});
	CHECK(fixture.GetDocument() == Testing::ApplyBatchAsGraphEdit(before, operations, fixture.Semantics, fixture.Graph));
}

TEST_CASE("VisualScriptEditor: renaming a function's input in Details renames the links of its calls and of its entry, in one step")
{
	PaneFixture fixture(MakeFunctionDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	VisualScriptDocument const before = fixture.GetDocument();
	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Function, 10};
	fixture.Frames(2);
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Function);
	size_t const undo = fixture.GetUndoCount();
	// The name field is the left part of the parameter's row.
	fixture.ImGui.Click(fixture.DetailsRow("Input 1", 0.1f), fixture.Drawer());
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type("Points");
	fixture.Frames(2);
	// Nothing is committed while the name is typed.
	CHECK(fixture.GetUndoCount() == undo);
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Rename Input Amount to Points of AddScore in Graph");
	VisualScriptDocument const& document = fixture.GetDocument();
	REQUIRE(document.Functions.size() == 1);
	CHECK(document.Functions.front().Inputs == std::vector<VisualScriptParameter>{{"Points", "int"}});
	CHECK(HasLinkIn(GetGraph(document, 10), MakeLink(11, "Points", 12, "Result")));
	CHECK(HasLinkIn(GetGraph(document, 0), MakeLink(3, "Value", 2, "Points")));
	CHECK(GetGraph(document, 10).Links.size() == 2);
	CHECK(GetGraph(document, 0).Links.size() == 2);
	// graph.edit's operation for the same edit writes the same document (one implementation).
	Json const operations = Json::array({Json::object({{"op", "SetFunctionSignature"},
	                                                   {"function", "AddScore"},
	                                                   {"inputs", Json::array({Json::object({{"name", "Points"}, {"type", "int"}})})},
	                                                   {"outputs", Json::array({Json::object({{"name", "Result"}, {"type", "int"}})})}})});
	CHECK(document == Testing::ApplyBatchAsGraphEdit(before, operations, fixture.Semantics, fixture.Graph));
}

TEST_CASE("VisualScriptEditor: the type picker of a public variable lists only types components can store")
{
	VisualScriptDocument document = MakeDocument();
	document.Variables.front().Public = true;
	PaneFixture fixture(document, {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	auto const pickerTypes = [&fixture](uint32_t variable)
	{
		fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Variable, variable};
		fixture.Frames(2);
		REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Variable);
		fixture.ImGui.Click(fixture.DetailsRow("Type"), fixture.Drawer());
		fixture.Frames(2);
		REQUIRE(fixture.Details.IsPickerOpen("Type"));
		std::span<std::string const> const listed = fixture.Details.GetPickerTypes();
		std::vector<std::string> types(listed.begin(), listed.end());
		fixture.ImGui.Press(ImGuiKey_Escape, fixture.Drawer());
		fixture.Frames(2);
		CHECK(fixture.Details.GetPickerTypes().empty());
		return types;
	};
	auto const lists = [](std::vector<std::string> const& types, std::string_view type)
	{
		return std::find(types.begin(), types.end(), type) != types.end();
	};
	// Speed is public.
	std::vector<std::string> const publicTypes = pickerTypes(100);
	CHECK(lists(publicTypes, "float"));
	CHECK(lists(publicTypes, "Strada.Vector3"));
	CHECK(lists(publicTypes, "Strada.KeyCode"));
	CHECK_FALSE(lists(publicTypes, "Strada.Matrix4"));
	CHECK_FALSE(lists(publicTypes, "Strada.RaycastHit"));
	CHECK_FALSE(lists(publicTypes, "Strada.Entity[]"));
	CHECK_FALSE(lists(publicTypes, "exec"));
	// Alive is private: any type with values, arrays included (spec-slice3 §3.4.3).
	std::vector<std::string> const privateTypes = pickerTypes(101);
	CHECK(lists(privateTypes, "Strada.Matrix4"));
	CHECK(lists(privateTypes, "Strada.RaycastHit"));
	CHECK(lists(privateTypes, "Strada.Entity[]"));
	CHECK_FALSE(lists(privateTypes, "exec"));
	CHECK(fixture.GetVersion() == AssetManager::GetAsset<VisualScriptAsset>(fixture.Graph)->GetVersion());
}

TEST_CASE("VisualScriptEditor: editing the script's description in Details is one step")
{
	PaneFixture fixture(MakeDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Script);
	size_t const undo = fixture.GetUndoCount();
	uint64_t const version = fixture.GetVersion();
	fixture.ImGui.Click(fixture.DetailsRow("Description"), fixture.Drawer());
	fixture.ImGui.Type("Moves");
	fixture.Frames(12);
	fixture.ImGui.Type(" the player");
	fixture.Frames(12);
	// Several commits while it is typed, merged into one step.
	CHECK(fixture.GetVersion() >= version + 2);
	fixture.ImGui.Click(GetDetailsVoid(), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Description in Graph");
	CHECK(fixture.GetDocument().Description == "Moves the player");
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.GetDocument().Description.empty());
}

TEST_CASE("VisualScriptEditor: a click straight from one field of Details into the next, editing both, makes two steps")
{
	PaneFixture fixture(MakeDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({2});
	size_t const undo = fixture.GetUndoCount();
	fixture.ImGui.Click(fixture.DetailsRow("Comment"), fixture.Drawer());
	fixture.ImGui.Type("Greets");
	fixture.Frames(2);
	ImGuiID const comment = GImGui->ActiveId;
	REQUIRE(comment != 0);
	// The Message input's field takes the keyboard in the frame the comment's lets it go.
	fixture.ImGui.Click(fixture.DetailsRow("Message"), fixture.Drawer());
	REQUIRE(GImGui->ActiveId != 0);
	CHECK(GImGui->ActiveId != comment);
	fixture.ImGui.Type("Hello");
	fixture.Frames(2);
	fixture.ImGui.Click(GetDetailsVoid(), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 2);
	VisualScriptNode const* const log = FindNode(fixture.GetDocument(), "Call:Strada.Log.Info(string)");
	REQUIRE(log != nullptr);
	CHECK(log->Comment == "Greets");
	CHECK(log->Inputs.value("Message", Json()) == "Hello");
	CHECK(fixture.Context.GetHistory().GetUndoDescription().starts_with("Set Message on "));
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(FindNode(fixture.GetDocument(), "Call:Strada.Log.Info(string)")->Comment == "Greets");
	CHECK_FALSE(FindNode(fixture.GetDocument(), "Call:Strada.Log.Info(string)")->Inputs.contains("Message"));
}

TEST_CASE(
	"VisualScriptEditor: dragging a value in Details commits at most every 0.1 s while held and once more at the release, in one step")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(4, "Flow.Delay", glm::vec2(400.0f, 300.0f)));
	PaneFixture fixture(document, {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({4});
	size_t const undo = fixture.GetUndoCount();
	uint64_t const version = fixture.GetVersion();
	ImVec2 const start = fixture.DetailsRow("Duration", 0.3f);
	ImGuiIO& io = ImGui::GetIO();
	io.AddMousePosEvent(start.x, start.y);
	fixture.Frames(1);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	fixture.Frames(1);
	CHECK(fixture.GetVersion() == version);
	// 60 frames of 1/60 s, the value changing in each.
	for (int frame = 1; frame <= 60; frame++)
	{
		io.AddMousePosEvent(start.x + 4.0f * static_cast<float>(frame), start.y);
		fixture.Frames(1);
	}
	uint64_t const held = fixture.GetVersion() - version;
	CHECK(held >= 9);
	CHECK(held <= 10);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	fixture.Frames(1);
	CHECK(fixture.GetVersion() - version == held + 1);
	fixture.Frames(3);
	CHECK(fixture.GetVersion() - version == held + 1);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Duration on Delay in Graph");
	VisualScriptNode const* const delay = FindNode(fixture.GetDocument(), "Flow.Delay");
	REQUIRE(delay != nullptr);
	CHECK(delay->Inputs.contains("Duration"));

	// A value dragged away and back to the one committed within 0.1 s: the value in between is not committed at the release.
	size_t const second = fixture.GetUndoCount();
	io.AddMousePosEvent(start.x, start.y);
	fixture.Frames(1);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	fixture.Frames(1);
	io.AddMousePosEvent(start.x + 8.0f, start.y);
	fixture.Frames(1);
	Json const committed = FindNode(fixture.GetDocument(), "Flow.Delay")->Inputs.value("Duration", Json());
	uint64_t const committedVersion = fixture.GetVersion();
	io.AddMousePosEvent(start.x + 12.0f, start.y);
	fixture.Frames(1);
	io.AddMousePosEvent(start.x + 8.0f, start.y);
	fixture.Frames(1);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	fixture.Frames(3);
	CHECK(fixture.GetVersion() == committedVersion);
	CHECK(FindNode(fixture.GetDocument(), "Flow.Delay")->Inputs.value("Duration", Json()) == committed);
	CHECK(fixture.GetUndoCount() == second + 1);
}

TEST_CASE("VisualScriptEditor: Details lists the placeholders the scripting runtime makes inputs of in a Format String's multi-line Format")
{
	Testing::ScriptRuntimeScope runtime;
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(1, "FormatString", glm::vec2(0.0f), Json::object({{"Format", "Score: {Score} of {Total}"}}))};
	PaneFixture fixture(document, {FixturePane::Details});
	EngineVisualScriptSemantics engine;
	fixture.ActiveSemantics = &engine;
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({1});
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Node);
	std::optional<std::pair<glm::vec2, glm::vec2>> const format = fixture.Details.FindRow("Format");
	REQUIRE(format);
	// Multi-line: taller than one line.
	CHECK(format->second.y - format->first.y > ImGui::GetFrameHeight() * 1.5f);
	CHECK(fixture.Details.GetRowText("Placeholders") == std::optional<std::string>("{Score}, {Total}"));
}

TEST_CASE("VisualScriptEditor: the problems popup names codes with the injected monospace font and with none, and its fixes are one step")
{
	for (bool const injected : {true, false})
	{
		VisualScriptDocument document = MakeDocument();
		document.Graph.Nodes[1].Inputs = Json::object({{"Text", "Hello"}});
		PaneFixture fixture(document, {FixturePane::Details});
		fixture.Services.MonospaceFont = injected ? fixture.Fonts.Monospace : nullptr;
		fixture.CheckDrawingChangesNothing();
		VisualScriptDiagnostic diagnostic;
		diagnostic.Code = "UnknownPin";
		diagnostic.Node = 2;
		diagnostic.Pin = "Text";
		diagnostic.Direction = VisualScriptPinDirection::In;
		diagnostic.Message = "Info has no input Text";
		diagnostic.Suggestions = {"Message"};
		fixture.Semantics.InjectDiagnostic(diagnostic);
		fixture.Semantics.ReloadLibrary();
		fixture.Frames(2);
		VisualScriptAnalysis const* const analysis = fixture.Model->GetAnalysis();
		REQUIRE(analysis != nullptr);
		std::vector<VisualScriptDiagnostic> const problems = GetVisualScriptNodeProblems(analysis, 2);
		REQUIRE(problems.size() == 1);

		// The popup a badge opens.
		VisualScriptWidgetCommits commits;
		ImGuiWindow* popup = nullptr;
		std::optional<VisualScriptPaneResult> chosen;
		bool clickFix = false;
		auto const draw = [&]
		{
			fixture.Draw();
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(500.0f, 400.0f));
			ImGui::Begin("Badge", nullptr, ImGuiWindowFlags_NoSavedSettings);
			if (ImGui::IsWindowAppearing())
			{
				ImGui::OpenPopup("##problems");
			}
			if (ImGui::BeginPopup("##problems", ImGuiWindowFlags_NoSavedSettings))
			{
				popup = ImGui::GetCurrentWindow();
				VisualScriptPaneContext context = fixture.MakeContext();
				VisualScriptPaneResult result;
				if (clickFix)
				{
					// The first fix, as if its button was clicked.
					std::vector<VisualScriptQuickFix> const fixes =
						GetVisualScriptQuickFixes(fixture.Model->GetDisplayDocument(), *fixture.Model->GetAnalysis(), problems.front(),
					                              fixture.Semantics.GetFixtureLibrary());
					REQUIRE_FALSE(fixes.empty());
					ApplyVisualScriptQuickFixChoice(context, problems.front(), fixes.front(), commits, result);
					clickFix = false;
				}
				DrawVisualScriptProblems(context, problems, commits, result);
				if (result.Commit)
				{
					chosen = std::move(result);
				}
				ImGui::EndPopup();
			}
			ImGui::End();
		};
		for (int frame = 0; frame < 3; frame++)
		{
			fixture.ImGui.Frame(draw);
		}
		REQUIRE(popup != nullptr);
		CHECK(popup->Active);
		CHECK_FALSE(chosen);
		clickFix = true;
		fixture.ImGui.Frame(draw);
		REQUIRE(chosen);
		CHECK(chosen->Commit->Description == "Fix: Use Message in Graph");
		size_t const undo = fixture.GetUndoCount();
		fixture.Execute(std::move(*chosen));
		CHECK(fixture.GetUndoCount() == undo + 1);
		CHECK(FindNode(fixture.GetDocument(), "Call:Strada.Log.Info(string)")->Inputs == Json::object({{"Message", "Hello"}}));
	}
}

// --- My Script ---

namespace
{
	VisualScriptVariable const* FindVariable(VisualScriptDocument const& document, uint32_t id)
	{
		auto const found = std::find_if(document.Variables.begin(), document.Variables.end(),
		                                [id](VisualScriptVariable const& variable)
		                                {
											return variable.ID == id;
										});
		return found != document.Variables.end() ? &*found : nullptr;
	}

	// Update → Set Speed → Log, a Get of Speed, a custom event Hit with two senders, and a function AddScore.
	VisualScriptDocument MakeScriptDocument()
	{
		VisualScriptDocument document = MakeDocument();
		document.Graph.Nodes = {MakeNode(1, "Event.Update", glm::vec2(0.0f)),
		                        MakeNode(2, "Variable.Set", glm::vec2(240.0f, 0.0f), Json::object({{"Variable", 100}})),
		                        MakeNode(3, "Call:Strada.Log.Info(string)", glm::vec2(560.0f, 0.0f)),
		                        MakeNode(4, "Variable.Get", glm::vec2(0.0f, 240.0f), Json::object({{"Variable", 100}})),
		                        MakeNode(5, "Event.Custom", glm::vec2(0.0f, 480.0f), Json::object({{"Name", "Hit"}})),
		                        MakeNode(6, "Event.Send", glm::vec2(320.0f, 480.0f), Json::object({{"Name", "Hit"}})),
		                        MakeNode(7, "Event.Send", glm::vec2(320.0f, 640.0f), Json::object({{"Name", "Hit"}}))};
		document.Graph.Links = {MakeLink(1, "Then", 2, "Execute"), MakeLink(2, "Then", 3, "Execute"), MakeLink(4, "Value", 2, "Value")};
		VisualScriptFunction function;
		function.ID = 10;
		function.Name = "AddScore";
		function.Graph.Nodes = {MakeNode(11, "Function.Entry", glm::vec2(0.0f)), MakeNode(12, "Function.Return", glm::vec2(480.0f, 0.0f))};
		function.Graph.Links = {MakeLink(11, "Then", 12, "Execute")};
		document.Functions.push_back(std::move(function));
		return document;
	}

	// Selects a row of My Script with a click and presses F2 (the window routes it to My Script, whose window has the
	// keyboard), then types a name.
	void RenameRow(PaneFixture& fixture, VisualScriptElement element, char const* name)
	{
		fixture.ImGui.Click(fixture.SidebarRow(element), fixture.Drawer());
		REQUIRE(fixture.Shared.Element == element);
		REQUIRE(fixture.Sidebar.HandleAction(VisualScriptEditorAction::Rename, fixture.Shared));
		fixture.Frames(2);
		REQUIRE(fixture.Sidebar.GetRenaming() == element);
		// The field opens with its name selected: typing replaces it.
		fixture.ImGui.Type(name);
		fixture.Frames(2);
	}
}

TEST_CASE("VisualScriptEditor: My Script's + adds a variable whose name is typed next, and Enter makes one step of both")
{
	PaneFixture fixture(MakeDocument(), {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	fixture.ImGui.Click(fixture.SidebarAdd(VisualScriptElementKind::Variable), fixture.Drawer());
	fixture.Frames(2);
	// Added at once, its name open for typing.
	CHECK(fixture.GetUndoCount() == undo + 1);
	VisualScriptVariable const* const added = FindVariable(fixture.GetDocument(), 102);
	REQUIRE(added != nullptr);
	CHECK(added->Name == "NewVar");
	CHECK(added->Type == "float");
	CHECK_FALSE(added->Public);
	CHECK(fixture.Sidebar.GetRenaming() == VisualScriptElement{VisualScriptElementKind::Variable, 102});
	CHECK(fixture.Shared.Element == VisualScriptElement{VisualScriptElementKind::Variable, 102});
	fixture.ImGui.Type("Health");
	fixture.Frames(2);
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK_FALSE(fixture.Sidebar.GetRenaming());
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Add Variable in Graph");
	VisualScriptVariable const* const named = FindVariable(fixture.GetDocument(), 102);
	REQUIRE(named != nullptr);
	CHECK(named->Name == "Health");
	// Named in the step that created it: no former name.
	CHECK(named->FormerNames.empty());
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.GetDocument().Variables.size() == 2);
}

TEST_CASE("VisualScriptEditor: an invalid name typed in My Script keeps the old one and says why")
{
	PaneFixture fixture(MakeDocument(), {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	std::string const bytes = fixture.ReadFile();
	VisualScriptElement const speed{VisualScriptElementKind::Variable, 100};
	RenameRow(fixture, speed, "Alive");
	CHECK(fixture.Sidebar.GetRenameError() == "a variable is named Alive already");
	// Enter is refused: the name stays open for typing.
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.Sidebar.GetRenaming() == speed);
	CHECK(fixture.GetUndoCount() == undo);
	fixture.ImGui.Type(".");
	fixture.Frames(2);
	CHECK(fixture.Sidebar.GetRenameError().starts_with("expected a name of 1 to 64 bytes"));
	// Leaving the field keeps the old name, with a note saying why.
	fixture.ImGui.Click(ImVec2(PaneFixture::SidebarSize.x * 0.5f, PaneFixture::SidebarSize.y - 20.0f), fixture.Drawer());
	fixture.Frames(2);
	CHECK_FALSE(fixture.Sidebar.GetRenaming());
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);
	CHECK(std::any_of(fixture.Results.begin(), fixture.Results.end(),
	                  [](VisualScriptPaneResult const& result)
	                  {
						  return std::find(result.Notes.begin(), result.Notes.end(),
		                                   "Speed keeps its name: expected a name of 1 to 64 bytes without control characters, '.' or a "
		                                   "leading or trailing space") != result.Notes.end();
					  }));
}

TEST_CASE("VisualScriptEditor: renaming a variable in My Script keeps its old name, so a component value set under it still applies")
{
	VisualScriptDocument document = MakeDocument();
	document.Variables.front().Public = true;
	PaneFixture fixture(document, {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	RenameRow(fixture, {VisualScriptElementKind::Variable, 100}, "MaxSpeed");
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Rename Speed to MaxSpeed in Graph");
	VisualScriptVariable const* const renamed = FindVariable(fixture.GetDocument(), 100);
	REQUIRE(renamed != nullptr);
	CHECK(renamed->Name == "MaxSpeed");
	CHECK(renamed->FormerNames == std::vector<std::string>{"Speed"});
	// The value a component stored under the old name is the inspector's value of the renamed variable
	// (InspectorPanel::DrawVisualScriptVariables reads the stored values through NormalizeVisualScriptOverrides).
	ScriptFieldMap const stored{{"Speed", ScriptFieldValue::FromFloat(7.5f)}};
	ScriptFieldMap const shown = NormalizeVisualScriptOverrides(fixture.GetDocument(), stored);
	REQUIRE(shown.contains("MaxSpeed"));
	CHECK(shown.at("MaxSpeed") == ScriptFieldValue::FromFloat(7.5f));
	CHECK_FALSE(shown.contains("Speed"));
}

TEST_CASE("VisualScriptEditor: a variable of a type components cannot store cannot be made public from My Script")
{
	VisualScriptDocument document = MakeDocument();
	document.Variables.push_back(MakeVariable(102, "Pose", "Strada.Matrix4"));
	PaneFixture fixture(document, {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	std::string const bytes = fixture.ReadFile();
	std::optional<std::pair<glm::vec2, glm::vec2>> const eye = fixture.Sidebar.FindEye(102);
	REQUIRE(eye);
	fixture.ImGui.Click(ImVec2((eye->first.x + eye->second.x) * 0.5f, (eye->first.y + eye->second.y) * 0.5f), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);
	CHECK_FALSE(FindVariable(fixture.GetDocument(), 102)->Public);
	CHECK(std::any_of(fixture.Results.begin(), fixture.Results.end(),
	                  [](VisualScriptPaneResult const& result)
	                  {
						  return std::find(result.Notes.begin(), result.Notes.end(),
		                                   "Strada.Matrix4 cannot be public: components cannot store it") != result.Notes.end();
					  }));
	// A float can.
	std::optional<std::pair<glm::vec2, glm::vec2>> const speedEye = fixture.Sidebar.FindEye(100);
	REQUIRE(speedEye);
	fixture.ImGui.Click(ImVec2((speedEye->first.x + speedEye->second.x) * 0.5f, (speedEye->first.y + speedEye->second.y) * 0.5f),
	                    fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Make Speed Public in Graph");
	CHECK(FindVariable(fixture.GetDocument(), 100)->Public);
}

TEST_CASE(
	"VisualScriptEditor: deleting a used variable in My Script asks first, then deletes it and its nodes in one step, bridging the flow")
{
	PaneFixture fixture(MakeScriptDocument(), {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Variable, 100};
	fixture.Frames(1);
	REQUIRE(fixture.Sidebar.HandleAction(VisualScriptEditorAction::Delete, fixture.Shared));
	fixture.Frames(2);
	CHECK(fixture.Sidebar.GetPendingDelete() == VisualScriptElement{VisualScriptElementKind::Variable, 100});
	CHECK(fixture.Sidebar.GetDeleteMessage() == "2 nodes use it; they are deleted too.");
	CHECK(GImGui->OpenPopupStack.Size == 1);
	CHECK(fixture.GetUndoCount() == undo);
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(GImGui->OpenPopupStack.Size == 0);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Delete Variable Speed (and 2 nodes) in Graph");
	VisualScriptDocument const& document = fixture.GetDocument();
	CHECK(FindVariable(document, 100) == nullptr);
	CHECK(FindVisualScriptNode(document, 2) == nullptr);
	CHECK(FindVisualScriptNode(document, 4) == nullptr);
	CHECK(HasLinkIn(document.Graph, MakeLink(1, "Then", 3, "Execute")));
	CHECK_FALSE(fixture.Shared.Element);

	// An unused variable goes at once; Cancel keeps a used one.
	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Frames(2);
	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Variable, 101};
	fixture.Frames(1);
	REQUIRE(fixture.Sidebar.HandleAction(VisualScriptEditorAction::Delete, fixture.Shared));
	fixture.Frames(2);
	CHECK(GImGui->OpenPopupStack.Size == 0);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Delete Variable Alive in Graph");
	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Variable, 100};
	fixture.Frames(1);
	REQUIRE(fixture.Sidebar.HandleAction(VisualScriptEditorAction::Delete, fixture.Shared));
	fixture.Frames(2);
	REQUIRE(fixture.Sidebar.GetPendingDelete());
	size_t const before = fixture.GetUndoCount();
	fixture.ImGui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(2);
	CHECK_FALSE(fixture.Sidebar.GetPendingDelete());
	CHECK(fixture.GetUndoCount() == before);
	CHECK(FindVariable(fixture.GetDocument(), 100) != nullptr);
}

TEST_CASE("VisualScriptEditor: My Script's rows drag their graph, kind and ID, and a variable dropped on another goes before it")
{
	PaneFixture fixture(MakeScriptDocument(), {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	ImVec2 const alive = fixture.SidebarRow({VisualScriptElementKind::Variable, 101});
	ImVec2 const speed = fixture.SidebarRow({VisualScriptElementKind::Variable, 100});
	ImGuiIO& io = ImGui::GetIO();
	io.AddMousePosEvent(alive.x, alive.y);
	fixture.Frames(1);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	fixture.Frames(1);
	io.AddMousePosEvent(alive.x, alive.y - 8.0f);
	fixture.Frames(1);
	io.AddMousePosEvent(speed.x, speed.y);
	fixture.Frames(2);
	REQUIRE(GImGui->DragDropActive);
	REQUIRE(GImGui->DragDropPayload.IsDataType(DragDropPayload::VisualScriptElement));
	REQUIRE(GImGui->DragDropPayload.DataSize == static_cast<int>(sizeof(VisualScriptElementPayload)));
	VisualScriptElementPayload payload;
	std::memcpy(&payload, GImGui->DragDropPayload.Data, sizeof(payload));
	CHECK(payload.Graph == fixture.Graph.GetUUID().GetValue());
	CHECK(payload.Kind == static_cast<uint32_t>(VisualScriptElementKind::Variable));
	CHECK(payload.ID == 101);
	size_t const undo = fixture.GetUndoCount();
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Reorder Variables in Graph");
	REQUIRE(fixture.GetDocument().Variables.size() == 2);
	CHECK(fixture.GetDocument().Variables[0].ID == 101);
	CHECK(fixture.GetDocument().Variables[1].ID == 100);
}

TEST_CASE("VisualScriptEditor: renaming a custom event in My Script renames the senders of this script in the same step")
{
	PaneFixture fixture(MakeScriptDocument(), {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	RenameRow(fixture, {VisualScriptElementKind::CustomEvent, 5}, "Damaged");
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Rename Event Hit to Damaged (and 2 senders) in Graph");
	for (uint32_t const id : {5u, 6u, 7u})
	{
		VisualScriptNode const* const node = FindVisualScriptNode(fixture.GetDocument(), id);
		REQUIRE(node != nullptr);
		CHECK(node->Settings.value("Name", Json()) == "Damaged");
	}
}

TEST_CASE("VisualScriptEditor: opening My Script's rows shows a function, frames a custom event and selects a variable's nodes")
{
	PaneFixture fixture(MakeScriptDocument(), {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	// A double click on a function's row shows its graph.
	fixture.ImGui.Click(fixture.SidebarRow({VisualScriptElementKind::Function, 10}), fixture.Drawer(), 2);
	fixture.Frames(2);
	CHECK(fixture.Model->GetFunction() == 10);
	CHECK(fixture.Shared.Element == VisualScriptElement{VisualScriptElementKind::Function, 10});
	// Enter on a custom event's row shows the event graph and selects its Event.Custom.
	fixture.ImGui.Click(fixture.SidebarRow({VisualScriptElementKind::CustomEvent, 5}), fixture.Drawer());
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(3);
	CHECK(fixture.Model->GetFunction() == 0);
	CHECK(std::vector<uint32_t>(fixture.Model->GetSelection().begin(), fixture.Model->GetSelection().end()) == std::vector<uint32_t>{5});
	// A double click on a variable's row selects its Get and Set nodes in the shown graph.
	fixture.ImGui.Click(fixture.SidebarRow({VisualScriptElementKind::Variable, 100}), fixture.Drawer(), 2);
	fixture.Frames(3);
	std::vector<uint32_t> selection(fixture.Model->GetSelection().begin(), fixture.Model->GetSelection().end());
	std::sort(selection.begin(), selection.end());
	CHECK(selection == std::vector<uint32_t>{2, 4});
	// The canvas's selection replaces the row's.
	CHECK_FALSE(fixture.Shared.Element);
}

TEST_CASE("VisualScriptEditor: My Script's rows draw their text inside the pane that clips at its left edge as the graph window's does")
{
	PaneFixture fixture(MakeScriptDocument(), {FixturePane::Sidebar});
	// The graph window draws My Script in a child without borders, which has no padding: its clip rectangle starts where its
	// contents do. The fixture's window gets the same.
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowPadding = ImVec2(0.0f, 0.0f);
	style.WindowBorderSize = 0.0f;
	fixture.CheckDrawingChangesNothing();
	ImGuiWindow* const pane = ImGui::FindWindowByName("My Script");
	REQUIRE(pane != nullptr);
	// The text's triangles: those whose texture coordinates vary vertically. Filled shapes use the white pixel and textured
	// lines one row of the atlas (imgui_draw.cpp:3694), so their v is constant.
	uint32_t glyphTriangles = 0;
	for (ImGuiWindow const* const window : GImGui->Windows)
	{
		if (window->RootWindow != pane || !window->WasActive)
		{
			continue;
		}
		ImDrawList const& drawList = *window->DrawList;
		for (ImDrawCmd const& command : drawList.CmdBuffer)
		{
			for (uint32_t index = 0; index + 2 < command.ElemCount; index += 3)
			{
				std::array<ImDrawVert const*, 3> corners{};
				for (uint32_t corner = 0; corner < 3; corner++)
				{
					corners[corner] = &drawList.VtxBuffer[static_cast<int>(
						command.VtxOffset + drawList.IdxBuffer[static_cast<int>(command.IdxOffset + index + corner)])];
				}
				if (corners[0]->uv.y == corners[1]->uv.y && corners[1]->uv.y == corners[2]->uv.y)
				{
					continue;
				}
				glyphTriangles++;
				float const left = std::min({corners[0]->pos.x, corners[1]->pos.x, corners[2]->pos.x});
				std::string const name = window->Name;
				CAPTURE(name);
				CHECK(left >= command.ClipRect.x);
			}
		}
	}
	// The rows' names were drawn: Event Graph, AddScore, the variable and the custom event.
	CHECK(glyphTriangles > 0);
}

TEST_CASE("VisualScriptEditor: My Script's + adds a function shown at once and a custom event whose name Details takes")
{
	PaneFixture fixture(MakeScriptDocument(), {FixturePane::Sidebar, FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	fixture.ImGui.Click(fixture.SidebarAdd(VisualScriptElementKind::Function), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Add Function in Graph");
	REQUIRE(fixture.GetDocument().Functions.size() == 2);
	uint32_t const added = fixture.GetDocument().Functions.back().ID;
	CHECK(fixture.GetDocument().Functions.back().Name == "NewFunction");
	CHECK(fixture.Model->GetFunction() == added);
	CHECK(fixture.Sidebar.GetRenaming() == VisualScriptElement{VisualScriptElementKind::Function, added});
	fixture.ImGui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(2);
	CHECK_FALSE(fixture.Sidebar.GetRenaming());

	fixture.ImGui.Click(fixture.SidebarAdd(VisualScriptElementKind::CustomEvent), fixture.Drawer());
	fixture.Frames(3);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Add Custom Event in Graph");
	CHECK(fixture.Model->GetFunction() == 0);
	REQUIRE(fixture.Model->GetSelection().size() == 1);
	VisualScriptNode const* const event = FindVisualScriptNode(fixture.GetDocument(), fixture.Model->GetSelection().front());
	REQUIRE(event != nullptr);
	CHECK(event->Type == "Event.Custom");
	CHECK(event->Settings.value("Name", Json()) == "NewEvent");
	// Details has its name, selected for typing.
	fixture.ImGui.Type("Landed");
	fixture.Frames(2);
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 3);
	CHECK(FindVisualScriptNode(fixture.GetDocument(), event->ID)->Settings.value("Name", Json()) == "Landed");
}

TEST_CASE("VisualScriptEditor: while playing, My Script shows the context entity's public values, read at most 4 times a second")
{
	VisualScriptDocument document = MakeDocument();
	document.Variables.front().Public = true;
	PaneFixture fixture(document, {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	int reads = 0;
	fixture.ReadLiveVariables = [&reads](UUID)
	{
		reads++;
		return Result<ScriptFieldMap>(
			ScriptFieldMap{{"Speed", ScriptFieldValue::FromFloat(2.5f)}, {"Alive", ScriptFieldValue::FromBool(true)}});
	};
	fixture.Frames(30);
	CHECK(reads == 0);
	CHECK(fixture.Sidebar.GetLiveValues().empty());
	fixture.Playing = true;
	fixture.ContextEntity = UUID(42);
	fixture.Frames(60);
	CHECK(reads >= 4);
	CHECK(reads <= 5);
	// Public variables only.
	CHECK(fixture.Sidebar.GetLiveValues() == std::map<std::string, Json>{{"Speed", Json(2.5)}});
}

// --- The problems drawer and find ---

namespace
{
	ImVec2 GetCenter(std::optional<std::pair<glm::vec2, glm::vec2>> const& rect)
	{
		REQUIRE(rect);
		return ImVec2((rect->first.x + rect->second.x) * 0.5f, (rect->first.y + rect->second.y) * 0.5f);
	}

	VisualScriptDiagnostic MakeDiagnostic(std::string code, uint32_t function, std::optional<uint32_t> node, std::string message,
	                                      VisualScriptSeverity severity = VisualScriptSeverity::Error)
	{
		VisualScriptDiagnostic diagnostic;
		diagnostic.Severity = severity;
		diagnostic.Code = std::move(code);
		diagnostic.Function = function;
		diagnostic.Node = node;
		diagnostic.Message = std::move(message);
		return diagnostic;
	}
}

TEST_CASE("VisualScriptEditor: a quick fix in the problems drawer is one step")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes[1].Inputs = Json::object({{"Text", "Hello"}});
	PaneFixture fixture(document, {FixturePane::Drawer});
	VisualScriptDiagnostic diagnostic = MakeDiagnostic("UnknownPin", 0, 2, "Info has no input Text");
	diagnostic.Pin = "Text";
	diagnostic.Direction = VisualScriptPinDirection::In;
	diagnostic.Suggestions = {"Message"};
	fixture.Semantics.InjectDiagnostic(diagnostic);
	fixture.CheckDrawingChangesNothing();
	REQUIRE(fixture.DrawerPane.GetMode() == VisualScriptDrawerMode::Problems);
	REQUIRE(fixture.DrawerPane.GetProblems().size() == 1);
	size_t const undo = fixture.GetUndoCount();
	fixture.ImGui.Click(GetCenter(fixture.DrawerPane.FindFixButton(0)), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Fix: Use Message in Graph");
	CHECK(FindNode(fixture.GetDocument(), "Call:Strada.Log.Info(string)")->Inputs == Json::object({{"Message", "Hello"}}));
}

TEST_CASE(
	"VisualScriptEditor: the problems drawer lists errors first by graph, and a click on one in a function shows it and selects the node")
{
	VisualScriptDocument document = MakeScriptDocument();
	PaneFixture fixture(document, {FixturePane::Drawer});
	fixture.Semantics.InjectDiagnostic(
		MakeDiagnostic("UnusedVariable", 0, std::nullopt, "Alive is never read", VisualScriptSeverity::Warning));
	fixture.Semantics.InjectDiagnostic(MakeDiagnostic("Unreachable", 10, 12, "Return is never reached"));
	fixture.Semantics.InjectDiagnostic(MakeDiagnostic("TypeMismatch", 0, 3, "Info needs a string"));
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	std::span<VisualScriptDiagnostic const> const problems = fixture.DrawerPane.GetProblems();
	REQUIRE(problems.size() == 3);
	// Errors first, the event graph's before the functions', then the warning.
	CHECK(problems[0].Code == "TypeMismatch");
	CHECK(problems[1].Code == "Unreachable");
	CHECK(problems[2].Code == "UnusedVariable");
	fixture.ImGui.Click(GetCenter(fixture.DrawerPane.FindProblemRow(1)), fixture.Drawer());
	fixture.Frames(3);
	CHECK(fixture.Model->GetFunction() == 10);
	CHECK(std::vector<uint32_t>(fixture.Model->GetSelection().begin(), fixture.Model->GetSelection().end()) == std::vector<uint32_t>{12});
	// Navigating changes nothing.
	CHECK(fixture.GetUndoCount() == undo);
}

TEST_CASE("VisualScriptEditor: Enter in the find bar goes from match to match across sub-graphs and selects each")
{
	VisualScriptDocument document = MakeScriptDocument();
	document.Graph.Nodes[2].Comment = "loud";
	document.Functions.front().Graph.Nodes[1].Comment = "loud too";
	PaneFixture fixture(document, {FixturePane::FindBar});
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	fixture.FindBar.Open("loud");
	// The field has the keyboard, and takes Enter once its key route is in place.
	fixture.Frames(3);
	REQUIRE(GImGui->ActiveId != 0);
	REQUIRE(fixture.FindBar.GetCount() == 2);
	CHECK(fixture.FindBar.GetIndex() == 0);
	CHECK(fixture.FindBar.GetOutlined().size() == 1);
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(3);
	CHECK(fixture.FindBar.GetIndex() == 1);
	CHECK(fixture.Model->GetFunction() == 0);
	CHECK(std::vector<uint32_t>(fixture.Model->GetSelection().begin(), fixture.Model->GetSelection().end()) == std::vector<uint32_t>{3});
	// The field keeps the keyboard.
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(3);
	CHECK(fixture.FindBar.GetIndex() == 2);
	CHECK(fixture.Model->GetFunction() == 10);
	CHECK(std::vector<uint32_t>(fixture.Model->GetSelection().begin(), fixture.Model->GetSelection().end()) == std::vector<uint32_t>{12});
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(3);
	CHECK(fixture.FindBar.GetIndex() == 1);
	CHECK(fixture.Model->GetFunction() == 0);
	CHECK(std::vector<uint32_t>(fixture.Model->GetSelection().begin(), fixture.Model->GetSelection().end()) == std::vector<uint32_t>{3});
	// Shift+Enter goes back.
	fixture.ImGui.Chord(ImGuiMod_Shift | ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(3);
	CHECK(fixture.FindBar.GetIndex() == 2);
	CHECK(fixture.Model->GetFunction() == 10);
	// Only the shown graph's matches without All graphs; Escape closes and keeps the selection.
	fixture.FindBar.SetAllGraphs(false);
	fixture.Frames(3);
	CHECK(fixture.FindBar.GetCount() == 1);
	fixture.ImGui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(3);
	CHECK_FALSE(fixture.FindBar.IsOpen());
	CHECK(std::vector<uint32_t>(fixture.Model->GetSelection().begin(), fixture.Model->GetSelection().end()) == std::vector<uint32_t>{12});
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(VisualScriptFindBar::GetWidth(1000.0f, 16.0f) == 24.0f * 16.0f);
	CHECK(VisualScriptFindBar::GetWidth(200.0f, 16.0f) == 200.0f - 32.0f);
}

TEST_CASE("VisualScriptEditor: the references of a custom event list its senders in every script of the project")
{
	PaneFixture fixture(MakeScriptDocument(), {FixturePane::Drawer});
	// Another script sends Hit too.
	Result<AssetHandle> const other = fixture.Operations.CreateVisualScript("Scripts/Other.sgraph", VisualScriptTemplate::Empty);
	REQUIRE(other.IsOk());
	VisualScriptDocument sender;
	sender.Graph.Nodes = {MakeNode(1, "Event.Update", glm::vec2(0.0f)),
	                      MakeNode(2, "Event.Send", glm::vec2(240.0f, 0.0f), Json::object({{"Name", "Hit"}}))};
	sender.Graph.Links = {MakeLink(1, "Then", 2, "Execute")};
	REQUIRE(fixture.Operations.SetVisualScriptDocument(other.GetValue(), sender).IsOk());
	fixture.CheckDrawingChangesNothing();
	fixture.DrawerPane.ShowReferences({VisualScriptReference::Kind::CustomEvent, 0, "Hit"});
	fixture.Frames(2);
	CHECK(fixture.DrawerPane.GetMode() == VisualScriptDrawerMode::Find);
	std::span<VisualScriptDrawerFindRow const> const rows = fixture.DrawerPane.GetFindRows();
	auto const has = [&rows](AssetHandle graph, uint32_t node)
	{
		return std::any_of(rows.begin(), rows.end(),
		                   [graph, node](VisualScriptDrawerFindRow const& row)
		                   {
							   return row.Graph == graph && row.Match.Node == node && row.Error.empty();
						   });
	};
	CHECK(has(fixture.Graph, 5));
	CHECK(has(fixture.Graph, 6));
	CHECK(has(fixture.Graph, 7));
	CHECK(has(other.GetValue(), 2));
	CHECK(rows.size() == 4);
	// A click on the other script's sender opens it there.
	auto const found = std::find_if(rows.begin(), rows.end(),
	                                [&other](VisualScriptDrawerFindRow const& row)
	                                {
										return row.Graph == other.GetValue();
									});
	REQUIRE(found != rows.end());
	CHECK(found->Script == "Scripts/Other.sgraph");
	fixture.ImGui.Click(GetCenter(fixture.DrawerPane.FindFindRow(static_cast<size_t>(found - rows.begin()))), fixture.Drawer());
	auto const opened = std::find_if(fixture.Results.rbegin(), fixture.Results.rend(),
	                                 [](VisualScriptPaneResult const& result)
	                                 {
										 return result.Open.has_value();
									 });
	REQUIRE(opened != fixture.Results.rend());
	CHECK(opened->Open->Graph == other.GetValue());
	CHECK(opened->Open->Select == std::vector<uint32_t>{2});
	CHECK(opened->Open->Frame == VisualScriptFrame::Selection);

	// A text found in every script.
	fixture.DrawerPane.ShowFind("Send");
	fixture.Frames(2);
	CHECK(fixture.DrawerPane.GetFindRows().size() >= 2);
	std::span<VisualScriptDrawerFindRow const> const scripts = fixture.DrawerPane.GetFindRows();
	CHECK(std::all_of(scripts.begin(), scripts.end(),
	                  [&fixture](VisualScriptDrawerFindRow const& row)
	                  {
						  return row.Graph == fixture.Graph;
					  }));
}

// --- What the track's review found ---

namespace
{
	// MakeScriptDocument with a local Total of AddScore and two inputs of AddScore.
	VisualScriptDocument MakeLocalDocument()
	{
		VisualScriptDocument document = MakeScriptDocument();
		VisualScriptFunction& function = document.Functions.front();
		VisualScriptLocal local;
		local.ID = 20;
		local.Name = "Total";
		local.Type = "float";
		function.Locals.push_back(local);
		function.Inputs = {{"Points", "int"}, {"Bonus", "int"}};
		return document;
	}

	ImVec2 GetRowPoint(std::optional<std::pair<glm::vec2, glm::vec2>> const& rect, float fraction = 0.5f)
	{
		REQUIRE(rect);
		return ImVec2(rect->first.x + (rect->second.x - rect->first.x) * fraction, (rect->first.y + rect->second.y) * 0.5f);
	}
}

TEST_CASE("VisualScriptEditor: a name field left for a widget drawn before it commits the name typed")
{
	SUBCASE("a row of My Script above the row renamed")
	{
		PaneFixture fixture(MakeDocument(), {FixturePane::Sidebar});
		fixture.CheckDrawingChangesNothing();
		size_t const undo = fixture.GetUndoCount();
		RenameRow(fixture, {VisualScriptElementKind::Variable, 101}, "Living");
		fixture.ImGui.Click(fixture.SidebarRow({VisualScriptElementKind::Variable, 100}), fixture.Drawer());
		fixture.Frames(2);
		CHECK_FALSE(fixture.Sidebar.GetRenaming());
		CHECK(fixture.GetUndoCount() == undo + 1);
		CHECK(FindVariable(fixture.GetDocument(), 101)->Name == "Living");
	}
	SUBCASE("My Script, drawn before Details, while Details' name field has the keyboard")
	{
		PaneFixture fixture(MakeScriptDocument(), {FixturePane::Sidebar, FixturePane::Details});
		fixture.CheckDrawingChangesNothing();
		fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Variable, 100};
		fixture.Frames(2);
		REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Variable);
		size_t const undo = fixture.GetUndoCount();
		fixture.ImGui.Click(fixture.DetailsRow("Name"), fixture.Drawer());
		fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
		fixture.ImGui.Type("MaxSpeed");
		fixture.Frames(2);
		fixture.ImGui.Click(fixture.SidebarRow({VisualScriptElementKind::Function, 0}), fixture.Drawer());
		fixture.Frames(2);
		CHECK(fixture.GetUndoCount() == undo + 1);
		CHECK(FindVariable(fixture.GetDocument(), 100)->Name == "MaxSpeed");
	}
	SUBCASE("another name field of Details above it")
	{
		PaneFixture fixture(MakeLocalDocument(), {FixturePane::Details});
		fixture.CheckDrawingChangesNothing();
		fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Function, 10};
		fixture.Frames(2);
		REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Function);
		size_t const undo = fixture.GetUndoCount();
		fixture.ImGui.Click(fixture.DetailsRow("Input 2", 0.2f), fixture.Drawer());
		fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
		fixture.ImGui.Type("Extra");
		fixture.Frames(2);
		fixture.ImGui.Click(fixture.DetailsRow("Input 1", 0.2f), fixture.Drawer());
		fixture.Frames(2);
		CHECK(fixture.GetUndoCount() == undo + 1);
		REQUIRE(fixture.GetDocument().Functions.front().Inputs.size() == 2);
		CHECK(fixture.GetDocument().Functions.front().Inputs[0].Name == "Points");
		CHECK(fixture.GetDocument().Functions.front().Inputs[1].Name == "Extra");
		// The field clicked has the keyboard now; leaving it unchanged changes nothing.
		fixture.ImGui.Click(GetDetailsVoid(), fixture.Drawer());
		fixture.Frames(2);
		CHECK(fixture.GetUndoCount() == undo + 1);
	}
}

TEST_CASE("VisualScriptEditor: after a refused Enter the name field keeps what was typed, and typing goes on after it")
{
	PaneFixture fixture(MakeDocument(), {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	VisualScriptElement const speed{VisualScriptElementKind::Variable, 100};
	RenameRow(fixture, speed, "Alive");
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Sidebar.GetRenaming() == speed);
	CHECK(fixture.Sidebar.GetRenameError() == "a variable is named Alive already");
	fixture.Frames(3);
	fixture.ImGui.Type("2");
	fixture.Frames(2);
	CHECK(fixture.Sidebar.GetRenameError().empty());
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK_FALSE(fixture.Sidebar.GetRenaming());
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(FindVariable(fixture.GetDocument(), 100)->Name == "Alive2");
}

TEST_CASE("VisualScriptEditor: a setting and an input of the same name are separate fields in Details")
{
	// An Event.Send of an event with a parameter called Name: its Name setting and its Name input.
	VisualScriptDocument document;
	VisualScriptParameter parameter{"Name", "string"};
	document.Graph.Nodes = {
		MakeNode(1, "Event.Custom", glm::vec2(0.0f),
	             Json::object({{"Name", "Joined"}, {"Parameters", Json::array({{{"Name", "Name"}, {"Type", "string"}}})}})),
		MakeNode(2, "Event.Send", glm::vec2(320.0f, 0.0f),
	             Json::object({{"Name", "Joined"}, {"Parameters", Json::array({{{"Name", "Name"}, {"Type", "string"}}})}}))};
	PaneFixture fixture(document, {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({2});
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Node);
	REQUIRE(fixture.Details.FindRow("Name", 0));
	REQUIRE(fixture.Details.FindRow("Name", 1));
	size_t const undo = fixture.GetUndoCount();
	// The setting.
	fixture.ImGui.Click(GetRowPoint(fixture.Details.FindRow("Name", 0)), fixture.Drawer());
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type("Left");
	fixture.Frames(2);
	fixture.ImGui.Click(GetDetailsVoid(), fixture.Drawer());
	fixture.Frames(2);
	VisualScriptNode const* send = FindVisualScriptNode(fixture.GetDocument(), 2);
	REQUIRE(send != nullptr);
	CHECK(send->Settings.value("Name", Json()) == "Left");
	CHECK_FALSE(send->Inputs.contains("Name"));
	CHECK(fixture.GetUndoCount() == undo + 1);
	// The input.
	fixture.ImGui.Click(GetRowPoint(fixture.Details.FindRow("Name", 1), 0.3f), fixture.Drawer());
	fixture.ImGui.Type("Bob");
	fixture.Frames(2);
	fixture.ImGui.Click(GetDetailsVoid(), fixture.Drawer());
	fixture.Frames(2);
	send = FindVisualScriptNode(fixture.GetDocument(), 2);
	CHECK(send->Settings.value("Name", Json()) == "Left");
	CHECK(send->Inputs.value("Name", Json()) == "Bob");
	CHECK(fixture.GetUndoCount() == undo + 2);
}

TEST_CASE("VisualScriptEditor: Details offers the Enabled states for a node of a type the library does not know")
{
	VisualScriptDocument document = MakeScriptDocument();
	document.Graph.Nodes.push_back(MakeNode(8, "Flow.NoSuchNode", glm::vec2(0.0f, 800.0f)));
	PaneFixture fixture(document, {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({8});
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Node);
	REQUIRE(fixture.Details.FindRow("Enabled"));
	CHECK(GetVisualScriptEnabledOffer(*fixture.Model, &fixture.Semantics.GetFixtureLibrary(), *FindVisualScriptNode(document, 8)) ==
	      VisualScriptEnabledOffer::Offered);
	// The states are offered: the row opens its list, whose second entry disables the node in one step.
	size_t const undo = fixture.GetUndoCount();
	fixture.ImGui.Click(fixture.DetailsRow("Enabled"), fixture.Drawer());
	fixture.Frames(2);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	fixture.ImGui.Press(ImGuiKey_DownArrow, fixture.Drawer());
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(FindVisualScriptNode(fixture.GetDocument(), 8)->Enabled == VisualScriptNodeEnabled::Disabled);
}

TEST_CASE("VisualScriptEditor: Details offers the Enabled states only where they act")
{
	VisualScriptDocument document = MakeScriptDocument();
	document.Graph.Nodes.push_back(MakeNode(8, "Get:Strada.Time.DeltaTime", glm::vec2(0.0f, 800.0f)));
	PaneFixture fixture(document, {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	// A node the flow passes through, and an event.
	fixture.SelectElements({3});
	CHECK(fixture.Details.FindRow("Enabled"));
	fixture.SelectElements({5});
	CHECK(fixture.Details.FindRow("Enabled"));
	CHECK(GetVisualScriptEnabledOffer(*fixture.Model, &fixture.Semantics.GetFixtureLibrary(), *FindVisualScriptNode(document, 5)) ==
	      VisualScriptEnabledOffer::Offered);
	// A pure node shows them disabled.
	fixture.SelectElements({8});
	REQUIRE(fixture.Details.FindRow("Enabled"));
	CHECK(GetVisualScriptEnabledOffer(*fixture.Model, &fixture.Semantics.GetFixtureLibrary(), *FindVisualScriptNode(document, 8)) ==
	      VisualScriptEnabledOffer::PureNode);
	size_t const undo = fixture.GetUndoCount();
	fixture.ImGui.Click(fixture.DetailsRow("Enabled"), fixture.Drawer());
	fixture.Frames(2);
	CHECK(GImGui->OpenPopupStack.Size == 0);
	CHECK(fixture.GetUndoCount() == undo);
	// Several: the states act on the nodes that offer them.
	fixture.SelectElements({3, 8});
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Several);
	fixture.ImGui.Click(fixture.DetailsRow("Enabled"), fixture.Drawer());
	fixture.Frames(2);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	fixture.ImGui.Press(ImGuiKey_DownArrow, fixture.Drawer());
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(FindVisualScriptNode(fixture.GetDocument(), 3)->Enabled == VisualScriptNodeEnabled::Disabled);
	CHECK(FindVisualScriptNode(fixture.GetDocument(), 8)->Enabled == VisualScriptNodeEnabled::Enabled);
	// A function's Entry and Return do not show them.
	fixture.Model->ShowFunction(10);
	fixture.Frames(2);
	fixture.SelectElements({11});
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Node);
	CHECK_FALSE(fixture.Details.FindRow("Enabled"));
	fixture.SelectElements({11, 12});
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Several);
	CHECK_FALSE(fixture.Details.FindRow("Enabled"));
}

TEST_CASE("VisualScriptEditor: a new custom event's name takes the keyboard in Details while another node was selected")
{
	PaneFixture fixture(MakeScriptDocument(), {FixturePane::Sidebar, FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({3});
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Node);
	size_t const undo = fixture.GetUndoCount();
	fixture.ImGui.Click(fixture.SidebarAdd(VisualScriptElementKind::CustomEvent), fixture.Drawer());
	fixture.Frames(3);
	REQUIRE(fixture.Model->GetSelection().size() == 1);
	uint32_t const event = fixture.Model->GetSelection().front();
	CHECK(event != 3);
	fixture.ImGui.Type("Landed");
	fixture.Frames(2);
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(FindVisualScriptNode(fixture.GetDocument(), event)->Settings.value("Name", Json()) == "Landed");
	CHECK_FALSE(fixture.Shared.FocusSetting);
}

TEST_CASE("VisualScriptEditor: a comment's position dragged slowly in Details moves it by whole grid steps")
{
	VisualScriptDocument document = MakeDocument();
	VisualScriptComment comment;
	comment.ID = 50;
	comment.Text = "Area";
	comment.Position = glm::vec2(96.0f, 400.0f);
	comment.Size = glm::vec2(320.0f, 160.0f);
	document.Graph.Comments.push_back(comment);
	PaneFixture fixture(document, {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({50});
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Comment);
	size_t const undo = fixture.GetUndoCount();
	// The X cell, 2 pixels a frame for 60 frames: 120 units at 1 unit a pixel.
	ImVec2 const start = fixture.DetailsRow("Position", 0.25f);
	ImGuiIO& io = ImGui::GetIO();
	io.AddMousePosEvent(start.x, start.y);
	fixture.Frames(1);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	fixture.Frames(1);
	for (int frame = 1; frame <= 60; frame++)
	{
		io.AddMousePosEvent(start.x + 2.0f * static_cast<float>(frame), start.y);
		fixture.Frames(1);
	}
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	fixture.Frames(3);
	VisualScriptComment const& moved = fixture.GetDocument().Graph.Comments.front();
	// Dragged about 120 units (less the drag threshold), snapped to 16.
	CHECK(moved.Position.x >= 96.0f + 96.0f);
	CHECK(moved.Position.x <= 96.0f + 128.0f);
	CHECK(std::fmod(moved.Position.x, 16.0f) == 0.0f);
	CHECK(moved.Position.y == 400.0f);
	CHECK(fixture.GetUndoCount() == undo + 1);
}

TEST_CASE("VisualScriptEditor: a comment box added from the palette or Details and its first title are one step")
{
	SUBCASE("the palette's Add Comment Box")
	{
		PaneFixture fixture(MakeDocument());
		fixture.CheckDrawingChangesNothing();
		size_t const undo = fixture.GetUndoCount();
		fixture.Palette.Open(MakeIntent(glm::vec2(400.0f, 400.0f), "add comment box"));
		fixture.Frames(2);
		REQUIRE_FALSE(fixture.GetStateTitles().empty());
		REQUIRE(fixture.GetStateTitles().front() == "Add Comment Box");
		fixture.Results.clear();
		fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
		auto const request = std::find_if(fixture.Results.begin(), fixture.Results.end(),
		                                  [](VisualScriptPaneResult const& result)
		                                  {
											  return result.EditCommentTitle.has_value();
										  });
		REQUIRE(request != fixture.Results.end());
		VisualScriptCommentTitleRequest const title = *request->EditCommentTitle;
		CHECK(title.MergeKey != 0);
		CHECK(fixture.GetUndoCount() == undo + 1);
		// The window's title editor commits the title under the key.
		VisualScriptDocument titled = fixture.GetDocument();
		REQUIRE(VisualScriptEditing::SetCommentText(titled, title.Comment, "Movement"));
		Result<VisualScriptCommitRequest> commit =
			MakeVisualScriptCommitRequest(*fixture.Model->GetStoredDocument(), std::move(titled), fixture.Model->GetAnalysis(),
		                                  "Rename Comment", "Graph", title.MergeKey);
		REQUIRE(commit.IsOk());
		fixture.ExecuteCommit(commit.TakeValue());
		CHECK(fixture.GetUndoCount() == undo + 1);
		REQUIRE(fixture.Operations.Undo().IsOk());
		CHECK(fixture.GetDocument().Graph.Comments.empty());
	}
	SUBCASE("Details' Comment Selection")
	{
		PaneFixture fixture(MakeDocument(), {FixturePane::Details});
		fixture.CheckDrawingChangesNothing();
		fixture.SelectElements({1, 2});
		size_t const undo = fixture.GetUndoCount();
		fixture.Results.clear();
		fixture.ImGui.Click(fixture.DetailsRow("Comment Selection"), fixture.Drawer());
		fixture.Frames(1);
		auto const request = std::find_if(fixture.Results.begin(), fixture.Results.end(),
		                                  [](VisualScriptPaneResult const& result)
		                                  {
											  return result.EditCommentTitle.has_value();
										  });
		REQUIRE(request != fixture.Results.end());
		VisualScriptCommentTitleRequest const title = *request->EditCommentTitle;
		CHECK(fixture.GetUndoCount() == undo + 1);
		VisualScriptDocument titled = fixture.GetDocument();
		REQUIRE(VisualScriptEditing::SetCommentText(titled, title.Comment, "Greeting"));
		Result<VisualScriptCommitRequest> commit =
			MakeVisualScriptCommitRequest(*fixture.Model->GetStoredDocument(), std::move(titled), fixture.Model->GetAnalysis(),
		                                  "Rename Comment", "Graph", title.MergeKey);
		REQUIRE(commit.IsOk());
		fixture.ExecuteCommit(commit.TakeValue());
		CHECK(fixture.GetUndoCount() == undo + 1);
	}
}

TEST_CASE("VisualScriptEditor: a dragged value held still in Details is committed 0.1 s after the last commit")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(4, "Flow.Delay", glm::vec2(400.0f, 300.0f)));
	PaneFixture fixture(document, {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({4});
	ImVec2 const start = fixture.DetailsRow("Duration", 0.3f);
	ImGuiIO& io = ImGui::GetIO();
	io.AddMousePosEvent(start.x, start.y);
	fixture.Frames(1);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	fixture.Frames(1);
	uint64_t const version = fixture.GetVersion();
	// Three frames of movement: the first change commits at once, the others wait.
	for (int frame = 1; frame <= 3; frame++)
	{
		io.AddMousePosEvent(start.x + 8.0f * static_cast<float>(frame), start.y);
		fixture.Frames(1);
	}
	CHECK(fixture.GetVersion() == version + 1);
	CHECK(fixture.Details.HasPendingEdit());
	// Held still: the value moved to reaches the document 0.1 s after the first commit, while the button is still down.
	fixture.Frames(6);
	CHECK(fixture.GetVersion() == version + 2);
	CHECK_FALSE(fixture.Details.HasPendingEdit());
	Json const held = FindNode(fixture.GetDocument(), "Flow.Delay")->Inputs.value("Duration", Json());
	fixture.Frames(20);
	CHECK(fixture.GetVersion() == version + 2);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	fixture.Frames(3);
	CHECK(FindNode(fixture.GetDocument(), "Flow.Delay")->Inputs.value("Duration", Json()) == held);
}

TEST_CASE("VisualScriptEditor: a function's Details lists each local's name, type and default value")
{
	PaneFixture fixture(MakeLocalDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Function, 10};
	fixture.Frames(2);
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Function);
	REQUIRE(fixture.Details.FindRow("Local 1"));
	REQUIRE(fixture.Details.FindRow("Local 1 Default"));
	size_t const undo = fixture.GetUndoCount();
	// Its name.
	fixture.ImGui.Click(fixture.DetailsRow("Local 1", 0.2f), fixture.Drawer());
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type("Sum");
	fixture.Frames(2);
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Rename Total to Sum in Graph");
	// Its default value.
	fixture.ImGui.Click(fixture.DetailsRow("Local 1 Default"), fixture.Drawer(), 2);
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type("5");
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(3);
	CHECK(fixture.GetUndoCount() == undo + 2);
	VisualScriptLocal const& local = fixture.GetDocument().Functions.front().Locals.front();
	CHECK(local.Name == "Sum");
	CHECK(local.Value == Json(5.0));
}

TEST_CASE("VisualScriptEditor: My Script does a routed key only on the row it was routed for, and never renames read-only scripts")
{
	PaneFixture fixture(MakeDocument(), {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	// Routed for Alive, then another row is selected before My Script draws: nothing is deleted.
	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Variable, 101};
	fixture.Frames(1);
	REQUIRE(fixture.Sidebar.HandleAction(VisualScriptEditorAction::Delete, fixture.Shared));
	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Variable, 100};
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.GetDocument().Variables.size() == 2);
	// Routed while My Script is hidden: dropped when it shows again.
	REQUIRE(fixture.Sidebar.HandleAction(VisualScriptEditorAction::Delete, fixture.Shared));
	fixture.Panes.clear();
	fixture.Frames(3);
	fixture.Panes = {FixturePane::Sidebar};
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.GetDocument().Variables.size() == 2);

	// A read-only file: F2 is not taken, and a quick fix's rename opens no field.
	Testing::ReadOnlyFileScope readOnly(fixture.File);
	fixture.Model->CheckWritable();
	fixture.Frames(2);
	REQUIRE(fixture.Model->IsReadOnly());
	CHECK_FALSE(fixture.Sidebar.HandleAction(VisualScriptEditorAction::Rename, fixture.Shared));
	CHECK_FALSE(fixture.Sidebar.HandleAction(VisualScriptEditorAction::Delete, fixture.Shared));
	fixture.Shared.Rename = VisualScriptElement{VisualScriptElementKind::Variable, 100};
	fixture.Frames(2);
	CHECK_FALSE(fixture.Sidebar.GetRenaming());
	CHECK_FALSE(fixture.Shared.Rename);
	readOnly.Restore();
}

TEST_CASE("VisualScriptEditor: going to a local of another sub-graph selects its row, whatever that sub-graph had selected")
{
	SUBCASE("from the find bar")
	{
		PaneFixture fixture(MakeLocalDocument(), {FixturePane::FindBar, FixturePane::Details});
		fixture.CheckDrawingChangesNothing();
		fixture.Model->ShowFunction(10);
		fixture.Frames(2);
		fixture.SelectElements({11});
		fixture.Model->ShowFunction(0);
		fixture.Frames(2);
		fixture.FindBar.Open("Total");
		fixture.Frames(3);
		REQUIRE(fixture.FindBar.GetCount() >= 1);
		fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
		fixture.Frames(2);
		CHECK(fixture.Model->GetFunction() == 10);
		CHECK(fixture.Model->GetSelection().empty());
		CHECK(fixture.Shared.Element == VisualScriptElement{VisualScriptElementKind::Local, 20});
		CHECK(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Local);
	}
	SUBCASE("from the problems drawer")
	{
		PaneFixture fixture(MakeLocalDocument(), {FixturePane::Drawer, FixturePane::Details});
		VisualScriptDiagnostic diagnostic =
			MakeDiagnostic("UnusedVariable", 10, std::nullopt, "Total is never read", VisualScriptSeverity::Warning);
		diagnostic.Variable = 20;
		fixture.Semantics.InjectDiagnostic(diagnostic);
		fixture.CheckDrawingChangesNothing();
		fixture.Model->ShowFunction(10);
		fixture.Frames(2);
		fixture.SelectElements({11});
		fixture.Model->ShowFunction(0);
		fixture.Frames(2);
		REQUIRE(fixture.DrawerPane.GetProblems().size() == 1);
		fixture.ImGui.Click(GetCenter(fixture.DrawerPane.FindProblemRow(0)), fixture.Drawer());
		fixture.Frames(2);
		CHECK(fixture.Model->GetFunction() == 10);
		CHECK(fixture.Model->GetSelection().empty());
		CHECK(fixture.Shared.Element == VisualScriptElement{VisualScriptElementKind::Local, 20});
		CHECK(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Local);
	}
}

TEST_CASE("VisualScriptEditor: the characters of the palette's opening query are not typed twice")
{
	PaneFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	// Space + F: the canvas makes the intent's query of the frame's characters, which ImGui still queues in that frame.
	fixture.ImGui.Type("f");
	fixture.ImGui.Frame(
		[&fixture]
		{
			fixture.Palette.Open(MakeIntent(glm::vec2(0.0f), "f"));
			fixture.Draw();
		});
	fixture.Frames(2);
	CHECK(fixture.Palette.GetState().Query == "f");
	fixture.ImGui.Type("or");
	fixture.Frames(2);
	CHECK(fixture.Palette.GetState().Query == "for");
}

TEST_CASE("VisualScriptEditor: the palette's preview reserves the node's laid-out size, and New: entries show their type")
{
	CHECK(IsVisualScriptReflectedType("New:Strada.Vector3(float,float,float)"));
	CHECK(IsVisualScriptReflectedType("Call:Strada.Log.Info(string)"));
	CHECK_FALSE(IsVisualScriptReflectedType("Flow.Branch"));

	PaneFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	int previews = 0;
	fixture.DrawNodePreview = [&previews](VisualScriptPaletteEntry const&, ImDrawList*, ImVec2, float)
	{
		previews++;
	};
	fixture.Palette.Open(MakeIntent(glm::vec2(0.0f), "raycast"));
	fixture.Frames(2);
	REQUIRE_FALSE(fixture.Palette.GetRowEntries().empty());
	ImGuiWindow const* const rows = FindChildWindow("##rows");
	REQUIRE(rows != nullptr);
	ImVec2 const row(rows->ContentRegionRect.Min.x + 40.0f, rows->ContentRegionRect.Min.y + ImGui::GetTextLineHeight() * 0.5f);
	ImGui::GetIO().AddMousePosEvent(row.x, row.y);
	fixture.Frames(40);
	REQUIRE(previews > 0);
	// The canvas's layout of the node: taller than the four rows reserved before (a header with its subtitle, five inputs).
	VisualScriptCanvasModel const& canvas = fixture.Model->GetCanvas();
	std::optional<uint32_t> const raycast = canvas.FindNode(3);
	REQUIRE(raycast);
	std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
	CHECK(fixture.Palette.GetPreviewSize().y >
	      VisualScriptCanvasMetrics::HeaderHeightWithSubtitle + VisualScriptCanvasMetrics::PinRowHeight * 4.0f);
	CHECK(fixture.Palette.GetPreviewSize().y == doctest::Approx(nodes[*raycast].Size.y));
}

TEST_CASE("VisualScriptEditor: a read-only script's variables, locals and functions with problems draw in Details")
{
	PaneFixture fixture(MakeLocalDocument(), {FixturePane::Details});
	VisualScriptDiagnostic variable =
		MakeDiagnostic("UnusedVariable", 0, std::nullopt, "Alive is never read", VisualScriptSeverity::Warning);
	variable.Variable = 101;
	fixture.Semantics.InjectDiagnostic(variable);
	VisualScriptDiagnostic local = MakeDiagnostic("UnusedVariable", 10, std::nullopt, "Total is never read", VisualScriptSeverity::Warning);
	local.Variable = 20;
	fixture.Semantics.InjectDiagnostic(local);
	fixture.Semantics.InjectDiagnostic(MakeDiagnostic("MissingReturn", 10, std::nullopt, "AddScore does not return"));
	fixture.CheckDrawingChangesNothing();
	Testing::ReadOnlyFileScope readOnly(fixture.File);
	fixture.Model->CheckWritable();
	fixture.Frames(2);
	REQUIRE(fixture.Model->IsReadOnly());
	for (VisualScriptElement const element :
	     {VisualScriptElement{VisualScriptElementKind::Variable, 101}, VisualScriptElement{VisualScriptElementKind::Local, 20},
	      VisualScriptElement{VisualScriptElementKind::Function, 10}})
	{
		CAPTURE(static_cast<uint32_t>(element.Kind));
		fixture.Shared.Element = element;
		fixture.Frames(2);
		CHECK(fixture.Shared.Element == element);
		CHECK(GImGui->DisabledStackSize == 0);
	}
	// Nothing selected in the function's graph: Details shows the script and the function.
	fixture.Shared.Element.reset();
	fixture.Model->ShowFunction(10);
	fixture.Frames(2);
	CHECK(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Script);
	CHECK(GImGui->DisabledStackSize == 0);
	readOnly.Restore();
}

TEST_CASE("VisualScriptEditor: find lists a script it cannot read, and All Scripts finds a text in every script")
{
	PaneFixture fixture(MakeScriptDocument(), {FixturePane::Drawer});
	Result<AssetHandle> const other = fixture.Operations.CreateVisualScript("Scripts/Other.sgraph", VisualScriptTemplate::Empty);
	REQUIRE(other.IsOk());
	VisualScriptDocument sender;
	sender.Graph.Nodes = {MakeNode(1, "Event.Update", glm::vec2(0.0f)),
	                      MakeNode(2, "Event.Send", glm::vec2(240.0f, 0.0f), Json::object({{"Name", "Hit"}}))};
	sender.Graph.Links = {MakeLink(1, "Then", 2, "Execute")};
	REQUIRE(fixture.Operations.SetVisualScriptDocument(other.GetValue(), sender).IsOk());
	Result<AssetHandle> const broken = fixture.Operations.CreateVisualScript("Scripts/Broken.sgraph", VisualScriptTemplate::Empty);
	REQUIRE(broken.IsOk());
	fixture.CheckDrawingChangesNothing();
	// The broken script's file is damaged and unloaded: it cannot be read.
	REQUIRE(FileSystem::WriteTextFile(AssetManager::GetAbsolutePath(broken.GetValue()), "{ not a graph").IsOk());
	AssetManager::UnloadAsset(broken.GetValue());

	fixture.DrawerPane.ShowFind("Send");
	fixture.DrawerPane.SetScope(VisualScriptFindScope::AllScripts);
	fixture.Frames(2);
	std::span<VisualScriptDrawerFindRow const> const rows = fixture.DrawerPane.GetFindRows();
	auto const error = std::find_if(rows.begin(), rows.end(),
	                                [&broken](VisualScriptDrawerFindRow const& row)
	                                {
										return row.Graph == broken.GetValue();
									});
	REQUIRE(error != rows.end());
	CHECK(error->Error.starts_with("could not be read: "));
	auto const found = std::find_if(rows.begin(), rows.end(),
	                                [&other](VisualScriptDrawerFindRow const& row)
	                                {
										return row.Graph == other.GetValue() && row.Error.empty();
									});
	REQUIRE(found != rows.end());
	CHECK(std::any_of(rows.begin(), rows.end(),
	                  [&fixture](VisualScriptDrawerFindRow const& row)
	                  {
						  return row.Graph == fixture.Graph;
					  }));
	fixture.Results.clear();
	fixture.ImGui.Click(GetCenter(fixture.DrawerPane.FindFindRow(static_cast<size_t>(found - rows.begin()))), fixture.Drawer());
	auto const opened = std::find_if(fixture.Results.rbegin(), fixture.Results.rend(),
	                                 [](VisualScriptPaneResult const& result)
	                                 {
										 return result.Open.has_value();
									 });
	REQUIRE(opened != fixture.Results.rend());
	CHECK(opened->Open->Graph == other.GetValue());
	CHECK(opened->Open->Select == std::vector<uint32_t>{2});
	// Problems mode lets the results go.
	fixture.DrawerPane.ShowProblems();
	CHECK(fixture.DrawerPane.GetFindRows().empty());
}

TEST_CASE("VisualScriptEditor: compact enum and asset frames show the text of the chips they replace")
{
	PaneFixture fixture(MakeDocument());
	VisualScriptLibrary const& library = fixture.Semantics.GetFixtureLibrary();
	// An enum: the stored name, as the canvas's chip shows it.
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(4, "Call:Strada.Input.IsKeyDown(Strada.KeyCode)", glm::vec2(0.0f, 600.0f)));
	document.Graph.Nodes.back().Inputs = Json::object({{"Key", "LeftShift"}});
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, document).IsOk());
	fixture.Frames(2);
	VisualScriptCanvasModel const& canvas = fixture.Model->GetCanvas();
	std::optional<uint32_t> const node = canvas.FindNode(4);
	REQUIRE(node);
	std::optional<uint32_t> const pin = canvas.FindPin(*node, "Key", VisualScriptPinDirection::In);
	REQUIRE(pin);
	std::span<VisualScriptPinVisual const> const pins = canvas.GetPins();
	std::string const chip(canvas.GetText(pins[*pin].ChipText));
	std::optional<VisualScriptLiteralField> const key = DescribeVisualScriptLiteral("Strada.KeyCode", "LeftShift", Json(), &library);
	REQUIRE(key);
	CHECK(FieldEditor::DescribeCompactText(key->Descriptor, key->Value) == chip);
	CHECK(chip == "LeftShift");
	// [Flags]: the stored names joined with " | " (the chips' GetEnumText).
	FieldDescriptor flags;
	flags.Kind = FieldKind::Enum;
	flags.IsFlags = true;
	std::vector<std::string> const names{"Read", "Write"};
	flags.EnumValues = {names[0], names[1]};
	CHECK(FieldEditor::DescribeCompactText(flags, Json::array({"Read", "Write"})) == "Read | Write");
	CHECK(FieldEditor::DescribeCompactText(flags, Json::array()) == "None");
	// An asset: its display name, the file's name.
	FieldDescriptor asset;
	asset.Kind = FieldKind::Asset;
	CHECK(FieldEditor::DescribeCompactText(asset, Json(fixture.Graph.ToString())) == "Graph.sgraph");
	CHECK(FieldEditor::DescribeCompactText(asset, Json()) == "None");
}

namespace
{
	// An Expression (2) of speed * dt with Speed's Get (1) linked into speed, and dt stored.
	VisualScriptDocument MakeFormulaDocument(std::string formula = "speed * dt")
	{
		VisualScriptDocument document;
		document.Variables = {MakeVariable(100, "Speed", "float")};
		VisualScriptNode expression = MakeNode(2, "Expression", glm::vec2(400.0f, 0.0f), Json::object({{"Formula", std::move(formula)}}));
		expression.Inputs = Json::object({{"dt", 0.5}});
		document.Graph.Nodes = {MakeNode(1, "Variable.Get", glm::vec2(0.0f), Json::object({{"Variable", 100}})), expression};
		document.Graph.Links = {{{1, "Value"}, {2, "speed"}}};
		return document;
	}

	std::string GetFormula(VisualScriptDocument const& document)
	{
		VisualScriptNode const* const node = FindNode(document, "Expression");
		REQUIRE(node != nullptr);
		return node->Settings.value("Formula", std::string());
	}
}

TEST_CASE("VisualScriptEditor: a formula retyped in Details commits once, as one rename that keeps the link into the input")
{
	PaneFixture fixture(MakeFormulaDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({2});
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Node);
	// The field is several lines high.
	std::optional<std::pair<glm::vec2, glm::vec2>> const row = fixture.Details.FindRow("Formula");
	REQUIRE(row);
	CHECK(row->second.y - row->first.y > ImGui::GetFrameHeight() * 2.0f);
	size_t const undo = fixture.GetUndoCount();
	uint64_t const version = fixture.GetVersion();
	fixture.ImGui.Click(fixture.DetailsRow("Formula"), fixture.Drawer());
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type("velocity * dt");
	fixture.Frames(12);
	// While the inputs differ from the start's, nothing reaches the document: no intermediate formula is committed.
	CHECK(fixture.GetVersion() == version);
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Formula on Expression (renames 1 input) in Graph");
	CHECK(GetFormula(fixture.GetDocument()) == "velocity * dt");
	CHECK(HasLink(fixture.GetDocument(), {{1, "Value"}, {2, "velocity"}}));
	CHECK(FindNode(fixture.GetDocument(), "Expression")->Inputs == Json::object({{"dt", 0.5}}));
	// graph.edit's operation for the same edit writes the same document (one implementation).
	VisualScriptDocument const after = fixture.GetDocument();
	REQUIRE(fixture.Operations.Undo().IsOk());
	Json const operations = Json::array({Json::object({{"op", "SetFormula"}, {"node", 2}, {"formula", "velocity * dt"}})});
	CHECK(after == Testing::ApplyBatchAsGraphEdit(fixture.GetDocument(), operations, fixture.Semantics, fixture.Graph));
}

TEST_CASE("VisualScriptEditor: a formula's field commits when it is left, and Escape cancels it")
{
	PaneFixture fixture(MakeFormulaDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({2});
	size_t const undo = fixture.GetUndoCount();
	std::string const bytes = fixture.ReadFile();
	// Escape: nothing changes.
	fixture.ImGui.Click(fixture.DetailsRow("Formula"), fixture.Drawer());
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type("x");
	fixture.Frames(2);
	fixture.ImGui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);
	// Left with a click elsewhere: committed, the input that went with its link (the list's length changed).
	fixture.ImGui.Click(fixture.DetailsRow("Formula"), fixture.Drawer());
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type("2 * dt");
	fixture.Frames(2);
	fixture.ImGui.Click(GetDetailsVoid(), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Formula on Expression (removes 1 link) in Graph");
	CHECK(GetFormula(fixture.GetDocument()) == "2 * dt");
	CHECK(fixture.GetDocument().Graph.Links.empty());
}

TEST_CASE("VisualScriptEditor: changing a formula's numbers commits at the widget cadence, in one step")
{
	PaneFixture fixture(MakeFormulaDocument("speed * 2"), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({2});
	size_t const undo = fixture.GetUndoCount();
	uint64_t const version = fixture.GetVersion();
	// The cursor at the end: the 2 becomes a 3, and the inputs stay those of the start.
	fixture.ImGui.Click(fixture.DetailsRow("Formula"), fixture.Drawer());
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_End, fixture.Drawer());
	fixture.ImGui.Press(ImGuiKey_Backspace, fixture.Drawer());
	fixture.ImGui.Type("3");
	fixture.Frames(8);
	// Committed while the field still has the keyboard, so a running game gets it within 0.1 s.
	CHECK(fixture.GetVersion() > version);
	CHECK(GetFormula(fixture.GetDocument()) == "speed * 3");
	fixture.ImGui.Type("0");
	fixture.Frames(12);
	CHECK(GetFormula(fixture.GetDocument()) == "speed * 30");
	fixture.ImGui.Click(GetDetailsVoid(), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Formula on Expression in Graph");
	CHECK(HasLink(fixture.GetDocument(), {{1, "Value"}, {2, "speed"}}));
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(GetFormula(fixture.GetDocument()) == "speed * 2");
}

TEST_CASE("VisualScriptEditor: a formula's problems show under it as caret blocks, and a click selects the part in the field")
{
	PaneFixture fixture(MakeFormulaDocument("a * b + sni(t)"), {FixturePane::Details});
	VisualScriptDiagnostic unknown = MakeDiagnostic("InvalidExpression", 0, 2, "unknown function sni: did you mean sin?");
	unknown.Setting = "Formula";
	unknown.Span = std::pair<uint32_t, uint32_t>(8, 11);
	fixture.Semantics.InjectDiagnostic(unknown);
	// A span past the formula's end has no line to mark.
	VisualScriptDiagnostic outside = MakeDiagnostic("InvalidExpression", 0, 2, "expected ')'", VisualScriptSeverity::Warning);
	outside.Setting = "Formula";
	outside.Span = std::pair<uint32_t, uint32_t>(15, 15);
	fixture.Semantics.InjectDiagnostic(outside);
	fixture.Semantics.InjectDiagnostic(MakeDiagnostic("Unused", 0, 2, "Result is read by nothing", VisualScriptSeverity::Warning));
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({2});
	CHECK(fixture.Details.GetRowText("Formula Problem 0") ==
	      std::optional<std::string>("a * b + sni(t)\n        ^^^ unknown function sni: did you mean sin?"));
	CHECK_FALSE(fixture.Details.GetRowText("Formula Problem 1").has_value());
	// Without the formula's inputs in the analysis (the fixture derives none), the inputs are listed untyped.
	CHECK(fixture.Details.GetRowText("Inputs") == std::optional<std::string>("a, b, t"));

	fixture.ImGui.Click(fixture.DetailsRow("Formula Problem 0"), fixture.Drawer());
	fixture.Frames(2);
	ImGuiInputTextState const* const state = ImGui::GetInputTextState(GImGui->ActiveId);
	REQUIRE(state != nullptr);
	CHECK(state->GetSelectionStart() == 8);
	CHECK(state->GetSelectionEnd() == 11);
	// Typing replaces the part. The typing gets a frame of its own, as keystrokes do: queued with the chord, ImGui reads the
	// characters with Ctrl held, and ignores them, unless the field asked for text input in the frame before (it trickles them
	// apart only then, imgui.cpp:11286), which this word-wrapped field does from the frame after it took the selection.
	size_t const undo = fixture.GetUndoCount();
	fixture.ImGui.Type("sin");
	fixture.Frames(1);
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(GetFormula(fixture.GetDocument()) == "a * b + sin(t)");
}

TEST_CASE("VisualScriptEditor: the Formula field wraps long lines at its width and grows with the lines it shows, up to eight")
{
	// One line of 36 terms, too wide for the field, and one too long for eight wrapped lines.
	std::string longFormula = "a";
	for (int term = 0; term < 35; term++)
	{
		longFormula += " + a";
	}
	std::string longestFormula = longFormula;
	for (int round = 0; round < 3; round++)
	{
		longestFormula += " + " + longestFormula;
	}
	PaneFixture fixture(MakeFormulaDocument(longFormula), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({2});
	float const lineHeight = ImGui::GetTextLineHeight();
	float const padding = ImGui::GetStyle().FramePadding.y * 2.0f;
	auto const shownLines = [&]
	{
		std::optional<std::pair<glm::vec2, glm::vec2>> const row = fixture.Details.FindRow("Formula");
		REQUIRE(row);
		return (row->second.y - row->first.y - padding) / lineHeight;
	};
	float const lines = shownLines();
	CHECK(lines > 3.5f);
	CHECK(lines < 8.5f);

	// The field wraps where the line count assumed it does, so no part of a line is out of view to the right.
	std::optional<std::pair<glm::vec2, glm::vec2>> const row = fixture.Details.FindRow("Formula");
	REQUIRE(row);
	fixture.ImGui.Click(fixture.DetailsRow("Formula"), fixture.Drawer());
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_End, fixture.Drawer());
	fixture.Frames(2);
	ImGuiInputTextState const* const state = ImGui::GetInputTextState(GImGui->ActiveId);
	REQUIRE(state != nullptr);
	CHECK((state->Flags & ImGuiInputTextFlags_WordWrap) != 0);
	ImGuiStyle const& style = ImGui::GetStyle();
	// Within the pixel the field's frame rounds its width to.
	CHECK(std::abs(state->WrapWidth - (row->second.x - row->first.x - style.FramePadding.x - style.ScrollbarSize)) <= 1.0f);
	CHECK(state->Scroll.x == 0.0f);
	// Wrapping changes no offset: the cursor at the end is at the text's last byte.
	CHECK(state->GetCursorPos() == static_cast<int>(longFormula.size()));
	fixture.ImGui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(2);

	// A narrower pane wraps into more lines; a formula of more lines than eight shows eight and scrolls.
	fixture.DetailsWindowSize = ImVec2(PaneFixture::DetailsSize.x * 0.75f, PaneFixture::DetailsSize.y);
	fixture.Frames(2);
	CHECK(shownLines() > lines + 0.5f);
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, MakeFormulaDocument(longestFormula)).IsOk());
	fixture.Frames(2);
	CHECK(shownLines() == doctest::Approx(8.0f).epsilon(0.01));
	// Short formulas keep three lines.
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, MakeFormulaDocument("a + b")).IsOk());
	fixture.Frames(2);
	CHECK(shownLines() == doctest::Approx(3.0f).epsilon(0.01));
}

TEST_CASE("VisualScriptEditor: Details' node header ellipsizes a subtitle too long for the pane, and its tooltip shows it whole")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(
		MakeNode(4, "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)", glm::vec2(0.0f, 600.0f)));
	PaneFixture fixture(document, {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({4});
	std::string const subtitle = "Target is Rigid Body";
	CHECK(fixture.Details.GetRowText("Subtitle") == std::optional<std::string>(subtitle));

	// Narrowed until the subtitle has no room for itself on the title's line: it ends at the pane's edge, ellipsized.
	fixture.DetailsWindowSize = ImVec2(180.0f, PaneFixture::DetailsSize.y);
	fixture.Frames(3);
	std::optional<std::string> const shown = fixture.Details.GetRowText("Subtitle");
	REQUIRE(shown);
	CAPTURE(*shown);
	CHECK(*shown != subtitle);
	CHECK(shown->ends_with(UI::GetEllipsis()));
	CHECK(subtitle.starts_with(shown->substr(0, shown->size() - UI::GetEllipsis().size())));
	std::optional<std::pair<glm::vec2, glm::vec2>> const rect = fixture.Details.FindRow("Subtitle");
	REQUIRE(rect);
	ImGuiWindow const* const details = ImGui::FindWindowByName("Details");
	REQUIRE(details != nullptr);
	CHECK(rect->second.x <= details->ContentRegionRect.Max.x + 0.5f);
	// So is the node's type on the line below, before its Copy button.
	std::optional<std::string> const identity = fixture.Details.GetRowText("Node Type");
	REQUIRE(identity);
	CHECK(identity->starts_with("Node 4"));
	CHECK(identity->ends_with(UI::GetEllipsis()));
	std::optional<std::pair<glm::vec2, glm::vec2>> const identityRect = fixture.Details.FindRow("Node Type");
	REQUIRE(identityRect);
	CHECK(identityRect->second.x <= details->ContentRegionRect.Max.x + 0.5f);

	// Rested on, it shows whole in the tooltip.
	ImGui::GetIO().AddMousePosEvent((rect->first.x + rect->second.x) * 0.5f, (rect->first.y + rect->second.y) * 0.5f);
	fixture.Frames(30);
	ImGuiWindow const* const tooltip = ImGui::FindWindowByName("##Tooltip_00");
	REQUIRE(tooltip != nullptr);
	CHECK(tooltip->Active);
	CHECK(tooltip->ContentSize.x >= ImGui::CalcTextSize(subtitle.c_str()).x - 1.0f);
}

TEST_CASE("VisualScriptEditor: a caret block marks the line of its span with tabs as four columns, and Edit Formula selects the span")
{
	PaneFixture fixture(MakeFormulaDocument("a +\n\tb * #"), {FixturePane::Details});
	VisualScriptDiagnostic character = MakeDiagnostic("InvalidExpression", 0, 2, "unexpected '#'");
	character.Setting = "Formula";
	character.Span = std::pair<uint32_t, uint32_t>(9, 10);
	fixture.Semantics.InjectDiagnostic(character);
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({2});
	CHECK(fixture.Details.GetRowText("Formula Problem 0") == std::optional<std::string>("    b * #\n        ^ unexpected '#'"));
	// What the problems' Edit Formula asks: the field with the keyboard and the span selected.
	fixture.Shared.FocusSetting = VisualScriptSettingFocus{2, "Formula", std::pair<uint32_t, uint32_t>(9, 10)};
	fixture.Frames(3);
	ImGuiInputTextState const* const state = ImGui::GetInputTextState(GImGui->ActiveId);
	REQUIRE(state != nullptr);
	CHECK(state->GetSelectionStart() == 9);
	CHECK(state->GetSelectionEnd() == 10);
	CHECK_FALSE(fixture.Shared.FocusSetting.has_value());
}

TEST_CASE("VisualScriptEditor: an array's elements are added, removed and moved in Details, one step each, and edited in one")
{
	VisualScriptDocument document = MakeDocument();
	VisualScriptVariable speeds = MakeVariable(102, "Speeds", "float[]");
	speeds.Value = Json::array({1.5, 2});
	document.Variables.push_back(speeds);
	PaneFixture fixture(document, {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Variable, 102};
	fixture.Frames(2);
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Variable);
	size_t const undo = fixture.GetUndoCount();
	auto const value = [&fixture]
	{
		return FindVariable(fixture.GetDocument(), 102)->Value;
	};

	// + Add Element appends the element type's default.
	fixture.ImGui.Click(fixture.DetailsRow("Default Value Add"), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Add Element to Speeds in Graph");
	CHECK(value() == Json::array({1.5, 2, 0}));

	fixture.ImGui.Click(fixture.DetailsRow("Default Value 2 Remove"), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Remove Element 2 of Speeds in Graph");
	CHECK(value() == Json::array({1.5, 2}));
	CHECK_FALSE(fixture.Details.FindRow("Default Value 2"));

	// A handle dropped on another element's handle moves the element there.
	fixture.ImGui.Drag(fixture.DetailsRow("Default Value 0 Handle"), fixture.DetailsRow("Default Value 1 Handle"), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 3);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Move Element of Speeds in Graph");
	CHECK(value() == Json::array({2, 1.5}));

	// An element's value typed into its widget: one step however many commits.
	fixture.ImGui.Click(fixture.DetailsRow("Default Value 0"), fixture.Drawer(), 2);
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type("7");
	fixture.Frames(2);
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 4);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Default Value of Speeds in Graph");
	CHECK(value() == Json::array({7, 1.5}));
}

namespace
{
	// A graph with a Timeline (node 4) of 2 s: a float track Lift with keys at 0, 1 and 2 s linked into the log's Message, and
	// an event track Step with a key at 1.5 s.
	VisualScriptDocument MakeTimelineDocument()
	{
		VisualScriptDocument document = MakeDocument();
		Result<Json> timeline = ParseJson(R"({"Length": 2, "Loop": false, "Tracks": [
			{"Name": "Lift", "Type": "float", "Keys": [{"Time": 0, "Value": 0}, {"Time": 1, "Value": 1}, {"Time": 2, "Value": 0}]},
			{"Name": "Step", "Type": "exec", "Keys": [{"Time": 1.5}]}]})");
		REQUIRE(timeline.IsOk());
		document.Graph.Nodes.push_back(
			MakeNode(4, "Flow.Timeline", glm::vec2(400.0f, 300.0f), Json::object({{"Timeline", timeline.GetValue()}})));
		document.Graph.Links.push_back({{4, "Lift"}, {2, "Message"}});
		return document;
	}

	VisualScriptTimeline ReadTimeline(VisualScriptDocument const& document)
	{
		VisualScriptNode const* node = FindNode(document, "Flow.Timeline");
		REQUIRE(node != nullptr);
		Result<VisualScriptTimeline> timeline = ReadVisualScriptTimeline(*node);
		REQUIRE_MESSAGE(timeline.IsOk(), timeline.GetError());
		return timeline.TakeValue();
	}

	std::vector<float> GetKeyTimes(VisualScriptTimeline const& timeline, size_t track)
	{
		std::vector<float> times;
		for (VisualScriptTimelineKey const& key : timeline.Tracks[track].Keys)
		{
			times.push_back(key.Time);
		}
		return times;
	}
}

TEST_CASE("VisualScriptEditor: Details edits a Timeline's length, loop and tracks, one step each")
{
	PaneFixture fixture(MakeTimelineDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({4});
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Node);
	REQUIRE(fixture.Details.FindRow("Track 0 Name"));
	REQUIRE(fixture.Details.FindRow("Track 1 Preview"));
	CHECK_FALSE(fixture.Details.FindRow("Track 2 Name"));
	size_t const undo = fixture.GetUndoCount();

	// A length typed below the last keys moves them onto it, in the same step.
	fixture.ImGui.Click(fixture.DetailsRow("Length"), fixture.Drawer(), 2);
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type("0.5");
	fixture.Frames(2);
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Length on Timeline (moves 3 keys) in Graph");
	VisualScriptTimeline shorter = ReadTimeline(fixture.GetDocument());
	CHECK(shorter.Length == 0.5f);
	CHECK(GetKeyTimes(shorter, 0) == std::vector<float>{0.0f, 0.5f, 0.5f});
	CHECK(GetKeyTimes(shorter, 1) == std::vector<float>{0.5f});

	// Loop.
	fixture.ImGui.Click(fixture.DetailsRow("Loop", 0.1f), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Loop Timeline in Graph");
	CHECK(ReadTimeline(fixture.GetDocument()).Loop);

	// A track renamed in its name field keeps its link.
	fixture.ImGui.Click(fixture.DetailsRow("Track 0 Name"), fixture.Drawer(), 2);
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type("Height");
	fixture.Frames(1);
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 3);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Rename Track Lift to Height on Timeline (and 1 link) in Graph");
	CHECK(HasLinkIn(GetGraph(fixture.GetDocument(), 0), MakeLink(4, "Height", 2, "Message")));

	// Remove.
	fixture.ImGui.Click(fixture.DetailsRow("Track 1 Remove"), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 4);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Remove Track Step from Timeline in Graph");
	CHECK(ReadTimeline(fixture.GetDocument()).Tracks.size() == 1);

	// + Add Track lists the six kinds; Float is the first.
	fixture.ImGui.Click(fixture.DetailsRow("Add Track"), fixture.Drawer());
	fixture.Frames(1);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	ImGuiWindow const* list = GImGui->OpenPopupStack.back().Window;
	REQUIRE(list != nullptr);
	fixture.ImGui.Click(ImVec2(list->DC.CursorStartPos.x + 20.0f, list->DC.CursorStartPos.y + ImGui::GetTextLineHeight() * 0.5f),
	                    fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 5);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Add Track Curve to Timeline in Graph");
	VisualScriptTimeline const added = ReadTimeline(fixture.GetDocument());
	REQUIRE(added.Tracks.size() == 2);
	CHECK(added.Tracks[1].Type == "float");

	// Edit opens the drawer on the track, which the window does.
	fixture.ImGui.Click(fixture.DetailsRow("Track 1 Edit"), fixture.Drawer());
	fixture.Frames(1);
	bool const asked = std::any_of(fixture.Results.begin(), fixture.Results.end(),
	                               [](VisualScriptPaneResult const& result)
	                               {
									   return result.ShowTimeline == VisualScriptTimelineFocus{4, 1};
								   });
	CHECK(asked);
	CHECK(fixture.GetUndoCount() == undo + 5);
}

TEST_CASE("VisualScriptEditor: a Timeline's length dragged down and back up in Details leaves every key where it was")
{
	PaneFixture fixture(MakeTimelineDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({4});
	VisualScriptTimeline const original = ReadTimeline(fixture.GetDocument());
	size_t const undo = fixture.GetUndoCount();
	ImVec2 const start = fixture.DetailsRow("Length");
	ImGuiIO& io = ImGui::GetIO();
	io.AddMousePosEvent(start.x, start.y);
	fixture.Frames(1);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	fixture.Frames(1);
	// 0.01 s a pixel, to the left: the length goes below the last keys, which follow it.
	float x = start.x;
	for (int step = 1; step <= 15; step++)
	{
		x -= 10.0f;
		io.AddMousePosEvent(x, start.y);
		fixture.Frames(1);
	}
	fixture.Frames(8);
	VisualScriptTimeline const shorter = ReadTimeline(fixture.GetDocument());
	REQUIRE(shorter.Length < 1.0f);
	CHECK(GetKeyTimes(shorter, 0) == std::vector<float>{0.0f, shorter.Length, shorter.Length});
	CHECK(GetKeyTimes(shorter, 1) == std::vector<float>{shorter.Length});
	CHECK(fixture.GetUndoCount() == undo + 1);
	// Back up to 2 s in the same drag (as the window shows it): every frame is computed from the timeline at the drag's start.
	for (int step = 0; step < 40 && ReadTimeline(fixture.Model->GetDisplayDocument()).Length < original.Length; step++)
	{
		x += 10.0f;
		io.AddMousePosEvent(x, start.y);
		fixture.Frames(1);
	}
	REQUIRE(ReadTimeline(fixture.Model->GetDisplayDocument()).Length == original.Length);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	fixture.Frames(3);
	VisualScriptTimeline const restored = ReadTimeline(fixture.GetDocument());
	CAPTURE(WriteVisualScriptTimeline(restored).dump());
	CHECK(restored == original);
	// One step at most: the drag's commits merge.
	CHECK(fixture.GetUndoCount() <= undo + 1);
}

TEST_CASE("VisualScriptEditor: an array variable cannot be made public from My Script, which says why")
{
	VisualScriptDocument document = MakeDocument();
	document.Variables.push_back(MakeVariable(102, "Targets", "Strada.Entity[]"));
	PaneFixture fixture(document, {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	fixture.ImGui.Click(GetCenter(fixture.Sidebar.FindEye(102)), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK_FALSE(FindVariable(fixture.GetDocument(), 102)->Public);
	CHECK(std::any_of(fixture.Results.begin(), fixture.Results.end(),
	                  [](VisualScriptPaneResult const& result)
	                  {
						  return std::find(result.Notes.begin(), result.Notes.end(), VisualScriptArrayPublicRefusal) != result.Notes.end();
					  }));
	CHECK(std::string_view(VisualScriptArrayPublicRefusal) == "Arrays cannot be public: components cannot store them");
}

TEST_CASE("VisualScriptEditor: a formula Details refuses says why, and Ctrl+Enter keeps it in the field to be fixed")
{
	PaneFixture fixture(MakeFormulaDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({2});
	size_t const undo = fixture.GetUndoCount();
	std::string const bytes = fixture.ReadFile();
	auto const noted = [&fixture]
	{
		return std::any_of(fixture.Results.begin(), fixture.Results.end(),
		                   [](VisualScriptPaneResult const& result)
		                   {
							   return std::find(result.Notes.begin(), result.Notes.end(),
			                                    "The formula stays: a formula has at most 4096 bytes") != result.Notes.end();
						   });
	};
	// One name of 4100 bytes: more than a formula may have.
	std::string const typed(VisualScriptEditing::MaxFormulaBytes + 4, 'x');
	fixture.ImGui.Click(fixture.DetailsRow("Formula"), fixture.Drawer());
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type(typed.c_str());
	fixture.Frames(2);
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(3);
	CHECK(noted());
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);
	// The field has the keyboard again, with what was typed.
	REQUIRE(ImGui::GetIO().WantTextInput);
	ImGuiInputTextState const* const state = ImGui::GetInputTextState(GImGui->ActiveId);
	REQUIRE(state != nullptr);
	CHECK(state->TextLen == static_cast<int>(typed.size()));

	// Fixed and committed.
	fixture.Results.clear();
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type("speed * 3");
	fixture.Frames(2);
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK_FALSE(noted());
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(GetFormula(fixture.GetDocument()) == "speed * 3");

	// Left with a click elsewhere, a refused formula goes with the note.
	fixture.ImGui.Click(fixture.DetailsRow("Formula"), fixture.Drawer());
	fixture.ImGui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.ImGui.Type(typed.c_str());
	fixture.Frames(2);
	fixture.ImGui.Click(GetDetailsVoid(), fixture.Drawer());
	fixture.Frames(2);
	CHECK(noted());
	CHECK_FALSE(ImGui::GetIO().WantTextInput);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(GetFormula(fixture.GetDocument()) == "speed * 3");
}

TEST_CASE("VisualScriptEditor: an array element's handle dropped on another array's handle moves nothing")
{
	Testing::HeadlessImGui headless;
	headless.UseEditorStyle();
	VisualScriptLiteralEditor first;
	VisualScriptLiteralEditor second;
	Json const left = Json::array({1, 2, 3});
	Json const right = Json::array({10, 20, 30});
	VisualScriptArrayLiteralOutcome firstOutcome;
	VisualScriptArrayLiteralOutcome secondOutcome;
	std::vector<VisualScriptArrayLiteralEdit> edits;
	auto const draw = [&]
	{
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(600.0f, 600.0f));
		ImGui::Begin("Arrays", nullptr, ImGuiWindowFlags_NoSavedSettings);
		ImGui::PushID("first");
		firstOutcome = DrawVisualScriptArrayLiteral(first, "float", left, nullptr);
		ImGui::PopID();
		ImGui::PushID("second");
		secondOutcome = DrawVisualScriptArrayLiteral(second, "float", right, nullptr);
		ImGui::PopID();
		ImGui::End();
		for (VisualScriptArrayLiteralOutcome const* outcome : {&firstOutcome, &secondOutcome})
		{
			if (outcome->Edit)
			{
				edits.push_back(*outcome->Edit);
			}
		}
	};
	auto const center = [](VisualScriptArrayLiteralOutcome const& outcome, std::string_view part)
	{
		auto const found = std::find_if(outcome.Parts.begin(), outcome.Parts.end(),
		                                [part](auto const& candidate)
		                                {
											return candidate.first == part;
										});
		REQUIRE(found != outcome.Parts.end());
		return ImVec2((found->second[0] + found->second[2]) * 0.5f, (found->second[1] + found->second[3]) * 0.5f);
	};
	headless.Frame(draw);
	headless.Frame(draw);
	REQUIRE(edits.empty());

	// Onto the other array's handle: neither array changes.
	headless.Drag(center(firstOutcome, "2 Handle"), center(secondOutcome, "0 Handle"), draw);
	CHECK(edits.empty());
	// Onto a handle of its own array: the element moves there.
	headless.Drag(center(firstOutcome, "2 Handle"), center(firstOutcome, "0 Handle"), draw);
	REQUIRE(edits.size() == 1);
	CHECK(edits[0].Action == VisualScriptArrayLiteralAction::Move);
	CHECK(edits[0].Literal == Json::array({3, 1, 2}));
}

namespace
{
	AssetHandle const PaneDoor(UUID(7600001ull));
	AssetHandle const PaneMotion(UUID(7600002ull));
	AssetHandle const PaneEnemy(UUID(7600003ull));

	VisualScriptProjectGraph MakeProjectGraph(AssetHandle graph, std::string name, VisualScriptDocument const& document)
	{
		VisualScriptProjectGraph made;
		made.Graph = graph;
		made.Path = "Scripts/" + name + ".sgraph";
		made.DisplayName = std::move(name);
		made.Kind = document.Kind;
		made.Document = CreateRef<VisualScriptDocument const>(document);
		return made;
	}

	VisualScriptVariable MakePaneVariable(uint32_t id, std::string name, std::string type, bool isPublic)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		variable.Public = isPublic;
		return variable;
	}

	// Door (the public Health 5 and Armor 9, the private Secret 6, the events Hit (Damage: float) and Open), Motion (a library:
	// Clamp01 (7, Value: float), Push (8)) and Enemy (a script), as the window manager's index gives them.
	std::vector<VisualScriptProjectGraph> MakePaneGraphs()
	{
		VisualScriptDocument door;
		door.Variables = {MakePaneVariable(5, "Health", "float", true), MakePaneVariable(9, "Armor", "int", true),
		                  MakePaneVariable(6, "Secret", "float", false)};
		door.Graph.Nodes = {
			MakeNode(1, "Event.Custom", glm::vec2(0.0f),
		             Json::object({{"Name", "Hit"}, {"Parameters", Json::array({{{"Name", "Damage"}, {"Type", "float"}}})}})),
			MakeNode(2, "Event.Custom", glm::vec2(0.0f, 240.0f), Json::object({{"Name", "Open"}}))};
		VisualScriptDocument motion = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
		VisualScriptFunction clamp;
		clamp.ID = 7;
		clamp.Name = "Clamp01";
		clamp.Pure = true;
		clamp.Inputs = {{"Value", "float"}};
		VisualScriptFunction push;
		push.ID = 8;
		push.Name = "Push";
		motion.Functions = {clamp, push};
		return {MakeProjectGraph(PaneDoor, "Door", door), MakeProjectGraph(PaneMotion, "Motion", motion),
		        MakeProjectGraph(PaneEnemy, "Enemy", VisualScriptDocument())};
	}

	// The centre of a Details row's widget, or of an item a Details combo lists ("<row>/<item>").
	ImVec2 GetRowCenter(VisualScriptDetails const& details, std::string_view label)
	{
		std::optional<std::pair<glm::vec2, glm::vec2>> const row = details.FindRow(label);
		REQUIRE_MESSAGE(row, "no Details row " << label);
		return ImVec2((row->first.x + row->second.x) * 0.5f, (row->first.y + row->second.y) * 0.5f);
	}

	// Opens a combo of Details and picks one of its items.
	void PickDetailsItem(PaneFixture& fixture, std::string_view row, std::string_view item)
	{
		fixture.ImGui.Click(fixture.DetailsRow(row), fixture.Drawer());
		fixture.Frames(2);
		fixture.ImGui.Click(GetRowCenter(fixture.Details, fmt::format("{}/{}", row, item)), fixture.Drawer());
		fixture.Frames(2);
	}
}

TEST_CASE("VisualScriptEditor: Details' Graph picker lists the graphs its setting allows and retargets the node in one step")
{
	std::string const door = PaneDoor.ToString();
	VisualScriptDocument document = MakeScriptDocument();
	Json const damage = Json::array({{{"Name", "Damage"}, {"Type", "float"}}});
	document.Graph.Nodes.push_back(
		MakeNode(20, "Event.Send", glm::vec2(0.0f, 800.0f), Json::object({{"Name", "Hit"}, {"Parameters", damage}})));
	document.Graph.Nodes.push_back(MakeNode(21, "Get:Strada.Time.DeltaTime", glm::vec2(-320.0f, 800.0f)));
	document.Graph.Nodes.push_back(MakeNode(22, "Function.Call", glm::vec2(0.0f, 1040.0f), Json::object({{"Function", 7}})));
	document.Graph.Links.push_back(MakeLink(21, "Value", 20, "Damage"));
	PaneFixture fixture(document, {FixturePane::Details});
	std::vector<VisualScriptProjectGraph> const graphs = MakePaneGraphs();
	fixture.Model->SetProjectGraphs(graphs, 1);
	fixture.CheckDrawingChangesNothing();

	// A Send names scripts: Door and Enemy, after None; no library.
	fixture.SelectElements({20});
	fixture.ImGui.Click(fixture.DetailsRow("Graph"), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.Details.FindRow("Graph/None (any receiver, by name)"));
	CHECK(fixture.Details.FindRow("Graph/Door \xC2\xB7 Scripts/Door.sgraph"));
	CHECK(fixture.Details.FindRow("Graph/Enemy \xC2\xB7 Scripts/Enemy.sgraph"));
	CHECK_FALSE(fixture.Details.FindRow("Graph/Motion \xC2\xB7 Scripts/Motion.sgraph"));
	fixture.ImGui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(2);

	// Typing it keeps its parameters and their links: one step.
	size_t const undo = fixture.GetUndoCount();
	PickDetailsItem(fixture, "Graph", "Door \xC2\xB7 Scripts/Door.sgraph");
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Graph on Send Hit in Graph");
	CHECK(FindVisualScriptNode(fixture.GetDocument(), 20)->Settings ==
	      Json::object({{"Name", "Hit"}, {"Parameters", damage}, {"Graph", door}}));
	// The target's events are offered beside Name: Open has no parameters, so the Damage link goes.
	CHECK(fixture.Details.FindRow("Event"));
	PickDetailsItem(fixture, "Event", "Open");
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Event on Send Hit (removes 1 link) in Graph");
	CHECK(FindVisualScriptNode(fixture.GetDocument(), 20)->Settings ==
	      Json::object({{"Name", "Open"}, {"Parameters", Json::array()}, {"Graph", door}}));
	CHECK(fixture.GetDocument().Graph.Links.size() == document.Graph.Links.size() - 1);
	// None untypes it and keeps its parameters.
	PickDetailsItem(fixture, "Graph", "None (any receiver, by name)");
	CHECK(fixture.GetUndoCount() == undo + 3);
	CHECK(FindVisualScriptNode(fixture.GetDocument(), 20)->Settings == Json::object({{"Name", "Open"}, {"Parameters", Json::array()}}));

	// A call names libraries only, "None (this script)" first; with one, its functions.
	fixture.SelectElements({22});
	fixture.ImGui.Click(fixture.DetailsRow("Graph"), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.Details.FindRow("Graph/None (this script)"));
	CHECK(fixture.Details.FindRow("Graph/Motion \xC2\xB7 Scripts/Motion.sgraph"));
	CHECK_FALSE(fixture.Details.FindRow("Graph/Door \xC2\xB7 Scripts/Door.sgraph"));
	fixture.ImGui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(2);
	PickDetailsItem(fixture, "Graph", "Motion \xC2\xB7 Scripts/Motion.sgraph");
	VisualScriptLibrary const& library = fixture.Semantics.GetFixtureLibrary();
	std::string const clamp = fmt::format("Clamp01 ({})", DescribeVisualScriptType(&library, "float"));
	fixture.ImGui.Click(fixture.DetailsRow("Function"), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.Details.FindRow("Function/" + clamp));
	CHECK(fixture.Details.FindRow("Function/Push ()"));
	// The document's own AddScore is not Motion's.
	CHECK_FALSE(fixture.Details.FindRow("Function/AddScore ()"));
	fixture.ImGui.Click(GetRowCenter(fixture.Details, "Function/" + clamp), fixture.Drawer());
	fixture.Frames(2);
	CHECK(FindVisualScriptNode(fixture.GetDocument(), 22)->Settings == Json::object({{"Function", 7}, {"Graph", PaneMotion.ToString()}}));

	// A graph the project does not have shows as missing.
	VisualScriptDocument missing = fixture.GetDocument();
	missing.Graph.Nodes.push_back(MakeNode(23, "RunsGraph", glm::vec2(0.0f, 1280.0f), Json::object({{"Graph", "81460000"}})));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, missing).IsOk());
	fixture.Frames(2);
	fixture.SelectElements({23});
	fixture.ImGui.Click(fixture.DetailsRow("Graph"), fixture.Drawer());
	fixture.Frames(2);
	// RunsGraph needs one: no None.
	CHECK_FALSE(fixture.Details.FindRow("Graph/None (this script)"));
	CHECK(fixture.Details.FindRow("Graph/Door \xC2\xB7 Scripts/Door.sgraph"));
	// The picker shows the handle it holds, in the Error colour.
	CHECK(fixture.Details.GetRowText("Graph") == std::optional<std::string>("Missing (8146\xE2\x80\xA6)"));
	fixture.ImGui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(2);
	// A graph the project knows shows by its name.
	fixture.SelectElements({22});
	fixture.Frames(1);
	CHECK(fixture.Details.GetRowText("Graph") == std::optional<std::string>("Motion \xC2\xB7 Scripts/Motion.sgraph"));
}

TEST_CASE("VisualScriptEditor: Details' Variable and Function of a node naming another graph list that graph's elements")
{
	std::string const door = PaneDoor.ToString();
	VisualScriptDocument document = MakeScriptDocument();
	// The document's own Speed must not be listed for the remote Get.
	document.Graph.Nodes.push_back(MakeNode(20, "Variable.Get", glm::vec2(0.0f, 800.0f), Json::object({{"Variable", 5}, {"Graph", door}})));
	PaneFixture fixture(document, {FixturePane::Details});
	std::vector<VisualScriptProjectGraph> const graphs = MakePaneGraphs();
	fixture.Model->SetProjectGraphs(graphs, 1);
	fixture.Semantics.SetGraphs({{PaneDoor, graphs[0].Document}});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({20});
	VisualScriptLibrary const& library = fixture.Semantics.GetFixtureLibrary();
	std::string const health = fmt::format("Health \xC2\xB7 {}", DescribeVisualScriptType(&library, "float"));
	std::string const armor = fmt::format("Armor \xC2\xB7 {}", DescribeVisualScriptType(&library, "int"));
	fixture.ImGui.Click(fixture.DetailsRow("Variable"), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.Details.FindRow("Variable/" + health));
	CHECK(fixture.Details.FindRow("Variable/" + armor));
	// Private variables of another graph, and the document's own, are not offered.
	CHECK_FALSE(fixture.Details.FindRow(fmt::format("Variable/Secret \xC2\xB7 {}", DescribeVisualScriptType(&library, "float"))));
	CHECK_FALSE(fixture.Details.FindRow(fmt::format("Variable/Speed \xC2\xB7 {}", DescribeVisualScriptType(&library, "float"))));
	size_t const undo = fixture.GetUndoCount();
	fixture.ImGui.Click(GetRowCenter(fixture.Details, "Variable/" + armor), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(FindVisualScriptNode(fixture.GetDocument(), 20)->Settings == Json::object({{"Variable", 9}, {"Graph", door}}));
	// The node's title follows the other graph: "Get Armor".
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Variable on Get Health in Graph");
}

TEST_CASE("VisualScriptEditor: switching a script with variables or events to a function library asks first, in one step")
{
	PaneFixture fixture(MakeScriptDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Script);
	size_t const undo = fixture.GetUndoCount();
	PickDetailsItem(fixture, "Kind", "Function Library");
	// The modal asks; nothing changed yet.
	REQUIRE(fixture.Details.GetKindQuestion().has_value());
	CHECK(*fixture.Details.GetKindQuestion() ==
	      "Make Graph a function library? Its 2 variables and 7 event nodes cannot stay in a library: they will show as errors "
	      "until you remove them.");
	CHECK(fixture.GetUndoCount() == undo);
	SUBCASE("Cancel keeps the kind")
	{
		fixture.ImGui.Press(ImGuiKey_Escape, fixture.Drawer());
		fixture.Frames(2);
		CHECK_FALSE(fixture.Details.GetKindQuestion().has_value());
		CHECK(fixture.GetUndoCount() == undo);
		CHECK(fixture.GetDocument().Kind == VisualScriptDocumentKind::Script);
	}
	SUBCASE("Enter makes it a library, and back to a script needs no question")
	{
		fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
		fixture.Frames(2);
		CHECK(fixture.GetUndoCount() == undo + 1);
		CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Make Graph a Function Library in Graph");
		CHECK(fixture.GetDocument().Kind == VisualScriptDocumentKind::Library);
		// Nothing else changed: its variables and event graph show as errors until removed.
		CHECK(fixture.GetDocument().Variables.size() == 2);
		PickDetailsItem(fixture, "Kind", "Script");
		CHECK_FALSE(fixture.Details.GetKindQuestion().has_value());
		CHECK(fixture.GetUndoCount() == undo + 2);
		CHECK(fixture.GetDocument().Kind == VisualScriptDocumentKind::Script);
	}
}

TEST_CASE("VisualScriptEditor: Details counts the uses of a public variable and a library's function in other scripts")
{
	PaneFixture fixture(MakeScriptDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	std::string const self = fixture.Graph.ToString();
	// Door reads variable 100 of this graph twice, Enemy once; Enemy's own Get names its own variable.
	VisualScriptDocument door;
	door.Graph.Nodes = {MakeNode(1, "Variable.Get", glm::vec2(0.0f), Json::object({{"Variable", 100}, {"Graph", self}})),
	                    MakeNode(2, "Variable.Set", glm::vec2(0.0f, 240.0f), Json::object({{"Variable", 100}, {"Graph", self}}))};
	VisualScriptDocument enemy;
	enemy.Graph.Nodes = {MakeNode(1, "Variable.Get", glm::vec2(0.0f), Json::object({{"Variable", 100}, {"Graph", self}})),
	                     MakeNode(2, "Variable.Get", glm::vec2(0.0f, 240.0f), Json::object({{"Variable", 100}}))};
	std::vector<VisualScriptProjectGraph> const graphs = {MakeProjectGraph(PaneDoor, "Door", door),
	                                                      MakeProjectGraph(PaneEnemy, "Enemy", enemy)};
	CHECK(CountVisualScriptOtherUses(graphs, fixture.Graph, VisualScriptElementKind::Variable, 100) == VisualScriptOtherUses{3, 2});
	CHECK(CountVisualScriptOtherUses(graphs, fixture.Graph, VisualScriptElementKind::Function, 100) == VisualScriptOtherUses{});
	fixture.Model->SetProjectGraphs(graphs, 1);
	// A private variable shows no such row.
	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Variable, 100};
	fixture.Frames(2);
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Variable);
	CHECK_FALSE(fixture.Details.GetRowText("Used by").has_value());
	VisualScriptDocument publicSpeed = fixture.GetDocument();
	publicSpeed.Variables[0].Public = true;
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, publicSpeed).IsOk());
	fixture.Frames(2);
	CHECK(fixture.Details.GetRowText("Used by") == std::optional<std::string>("Used by 3 nodes in 2 other scripts"));

	// A library's function: called by the scripts that name it.
	VisualScriptDocument library = fixture.GetDocument();
	library.Kind = VisualScriptDocumentKind::Library;
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, library).IsOk());
	VisualScriptDocument caller;
	caller.Graph.Nodes = {MakeNode(1, "Function.Call", glm::vec2(0.0f), Json::object({{"Function", 10}, {"Graph", self}}))};
	std::vector<VisualScriptProjectGraph> const callers = {MakeProjectGraph(PaneDoor, "Door", caller)};
	fixture.Model->SetProjectGraphs(callers, 2);
	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Function, 10};
	fixture.Frames(2);
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Function);
	CHECK(fixture.Details.GetRowText("Called by") == std::optional<std::string>("Called by 1 node in 1 script"));
}

namespace
{
	// A project graph whose references are its document's, as the project index lists them.
	VisualScriptProjectGraph MakeReferringGraph(AssetHandle graph, std::string name, VisualScriptDocument const& document)
	{
		VisualScriptProjectGraph made = MakeProjectGraph(graph, std::move(name), document);
		made.References = CollectVisualScriptReferences(document);
		return made;
	}

	VisualScriptNode MakeRemoteNode(uint32_t id, char const* type, AssetHandle graph, char const* key, Json value)
	{
		return MakeNode(id, type, glm::vec2(0.0f, static_cast<float>(id) * 240.0f),
		                Json::object({{key, std::move(value)}, {"Graph", graph.ToString()}}));
	}

	bool HasNote(std::vector<VisualScriptPaneResult> const& results, std::string_view note)
	{
		return std::any_of(results.begin(), results.end(),
		                   [note](VisualScriptPaneResult const& result)
		                   {
							   return std::find(result.Notes.begin(), result.Notes.end(), note) != result.Notes.end();
						   });
	}

	bool HasFindReferences(std::vector<VisualScriptPaneResult> const& results, VisualScriptReference const& reference)
	{
		return std::any_of(results.begin(), results.end(),
		                   [&reference](VisualScriptPaneResult const& result)
		                   {
							   return result.FindReferences == std::optional<VisualScriptReference>(reference);
						   });
	}
}

TEST_CASE("VisualScriptEditor: My Script's delete modal counts the nodes of other scripts that use what goes, and lists them")
{
	VisualScriptDocument document = MakeScriptDocument();
	document.Variables[0].Public = true;
	PaneFixture fixture(document, {FixturePane::Sidebar});
	// Door reads Speed (100) twice, Enemy once.
	VisualScriptDocument door;
	door.Graph.Nodes = {MakeRemoteNode(1, "Variable.Get", fixture.Graph, "Variable", 100),
	                    MakeRemoteNode(2, "Variable.Set", fixture.Graph, "Variable", 100)};
	VisualScriptDocument enemy;
	enemy.Graph.Nodes = {MakeRemoteNode(1, "Variable.Get", fixture.Graph, "Variable", 100)};
	std::vector<VisualScriptProjectGraph> const graphs = {MakeReferringGraph(PaneDoor, "Door", door),
	                                                      MakeReferringGraph(PaneEnemy, "Enemy", enemy)};
	fixture.Model->SetProjectGraphs(graphs, 1);
	fixture.CheckDrawingChangesNothing();

	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Variable, 100};
	fixture.Frames(1);
	REQUIRE(fixture.Sidebar.HandleAction(VisualScriptEditorAction::Delete, fixture.Shared));
	fixture.Frames(2);
	CHECK(fixture.Sidebar.GetDeleteMessage() ==
	      "2 nodes here use it (they are deleted too). 3 nodes in 2 other scripts use it: they will show errors until fixed.");
	// [Find References] lists them in the drawer and deletes nothing.
	size_t const undo = fixture.GetUndoCount();
	std::optional<std::pair<glm::vec2, glm::vec2>> const find = fixture.Sidebar.FindDeleteButton("Find References");
	REQUIRE(find);
	fixture.ImGui.Click(ImVec2((find->first.x + find->second.x) * 0.5f, (find->first.y + find->second.y) * 0.5f), fixture.Drawer());
	fixture.Frames(2);
	CHECK_FALSE(fixture.Sidebar.GetPendingDelete());
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(HasFindReferences(fixture.Results, VisualScriptReference{VisualScriptReference::Kind::Variable, 100, {}, fixture.Graph}));

	// A variable this script reads only on another entity (Graph naming itself): counted with the other scripts, and kept.
	VisualScriptDocument own = fixture.GetDocument();
	own.Graph.Nodes.push_back(MakeRemoteNode(30, "Variable.Get", fixture.Graph, "Variable", 101));
	own.Variables[1].Public = true;
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, own).IsOk());
	fixture.Frames(2);
	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Variable, 101};
	fixture.Frames(1);
	REQUIRE(fixture.Sidebar.HandleAction(VisualScriptEditorAction::Delete, fixture.Shared));
	fixture.Frames(2);
	CHECK(fixture.Sidebar.GetDeleteMessage() == "1 node in 1 script uses it: they will show errors until fixed.");
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(FindVisualScriptNode(fixture.GetDocument(), 30) != nullptr);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Delete Variable Alive in Graph");
}

TEST_CASE("VisualScriptEditor: an edit of what a graph offers tells what it does to the other scripts that use it")
{
	AssetHandle const library(UUID(7700001ull));
	AssetHandle const script(UUID(7700002ull));
	VisualScriptDocument motion = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	VisualScriptFunction clamp;
	clamp.ID = 7;
	clamp.Name = "Clamp01";
	clamp.Inputs = {{"Value", "float"}, {"Speed", "float"}};
	motion.Functions = {clamp};
	// Door calls Clamp01 twice and links Speed in one; Enemy calls it once.
	VisualScriptDocument door;
	door.Graph.Nodes = {MakeRemoteNode(1, "Function.Call", library, "Function", 7),
	                    MakeRemoteNode(2, "Function.Call", library, "Function", 7),
	                    MakeNode(3, "Get:Strada.Time.DeltaTime", glm::vec2(-320.0f, 0.0f))};
	door.Graph.Links = {MakeLink(3, "Value", 1, "Speed")};
	VisualScriptDocument enemy;
	enemy.Graph.Nodes = {MakeRemoteNode(1, "Function.Call", library, "Function", 7)};
	std::vector<VisualScriptProjectGraph> const graphs = {MakeReferringGraph(PaneDoor, "Door", door),
	                                                      MakeReferringGraph(PaneEnemy, "Enemy", enemy)};
	auto const describe = [&graphs](VisualScriptDocument const& before, VisualScriptDocument const& after, AssetHandle graph)
	{
		std::vector<std::string> texts;
		for (VisualScriptOfferToast const& toast : DescribeVisualScriptOfferChange(graph, before, after, graphs))
		{
			texts.push_back(toast.Text + (toast.Find ? " [" + toast.FindLabel + "]" : std::string()));
		}
		return texts;
	};

	// A rename recorded in the parameter's former names: every caller follows it.
	VisualScriptDocument renamed = motion;
	renamed.Functions[0].Inputs[0] = VisualScriptParameter("Amount", "float", {"Value"});
	CHECK(describe(motion, renamed, library) == std::vector<std::string>{"Renamed Value to Amount: 3 calls in 2 other scripts follow it"});
	// Removed and retyped: the calls that link it.
	VisualScriptDocument removed = motion;
	removed.Functions[0].Inputs.pop_back();
	CHECK(describe(motion, removed, library) ==
	      std::vector<std::string>{"Removed Speed: 1 call in 1 other script lose links [Find Callers]"});
	VisualScriptDocument retyped = motion;
	retyped.Functions[0].Inputs[1].Type = "int";
	CHECK(describe(motion, retyped, library) ==
	      std::vector<std::string>{"Changed Speed to int: 1 call in 1 other script may not fit [Find Callers]"});
	VisualScriptDocument deleted = motion;
	deleted.Functions.clear();
	std::vector<VisualScriptOfferToast> const gone = DescribeVisualScriptOfferChange(library, motion, deleted, graphs);
	REQUIRE(gone.size() == 1);
	CHECK(gone[0].Text == "Deleted Clamp01: 3 calls in 2 other scripts show errors");
	CHECK(gone[0].Find ==
	      std::optional<VisualScriptReference>(VisualScriptReference{VisualScriptReference::Kind::Function, 7, {}, library}));
	CHECK(gone[0].FindLabel == "Find Callers");
	VisualScriptDocument pure = motion;
	pure.Functions[0].Pure = true;
	CHECK(describe(motion, pure, library) ==
	      std::vector<std::string>{"Made Clamp01 pure: 3 calls in 2 other scripts lose links [Find Callers]"});
	// What changes nothing other scripts use tells nothing.
	VisualScriptDocument described = motion;
	described.Description = "Clamps";
	CHECK(describe(motion, described, library).empty());
	CHECK(DescribeVisualScriptOfferChange(library, motion, removed, {}).empty());

	// A script's public variable made private, and a custom event renamed while senders elsewhere still send its old name.
	VisualScriptDocument player = MakeScriptDocument();
	player.Variables[0].Public = true;
	VisualScriptDocument reader;
	reader.Graph.Nodes = {MakeRemoteNode(1, "Variable.Get", script, "Variable", 100),
	                      MakeNode(2, "Event.Send", glm::vec2(0.0f, 480.0f), Json::object({{"Name", "Hit"}})),
	                      MakeNode(3, "Event.Broadcast", glm::vec2(0.0f, 720.0f), Json::object({{"Name", "Hit"}}))};
	std::vector<VisualScriptProjectGraph> const readers = {MakeReferringGraph(PaneDoor, "Door", reader)};
	VisualScriptDocument hidden = player;
	hidden.Variables[0].Public = false;
	std::vector<VisualScriptOfferToast> const privacy = DescribeVisualScriptOfferChange(script, player, hidden, readers);
	REQUIRE(privacy.size() == 1);
	CHECK(privacy[0].Text == "Made Speed private: 1 node in 1 other script show errors");
	CHECK(privacy[0].FindLabel == "Find References");
	VisualScriptDocument damaged = player;
	Result<uint32_t> const renamedEvent = VisualScriptEditing::RenameCustomEvent(damaged, 5, "Damaged", script);
	REQUIRE_MESSAGE(renamedEvent.IsOk(), renamedEvent.GetError());
	std::vector<VisualScriptOfferToast> const events = DescribeVisualScriptOfferChange(script, player, damaged, readers);
	REQUIRE(events.size() == 1);
	CHECK(events[0].Text == "Renamed Hit to Damaged (and 2 senders). 2 senders in 1 other script still send Hit");
	CHECK(events[0].Find ==
	      std::optional<VisualScriptReference>(VisualScriptReference{VisualScriptReference::Kind::CustomEvent, 0, "Hit", script}));
	CHECK(events[0].FindLabel == "Find Senders");
}

TEST_CASE("VisualScriptEditor: My Script shows a function library's functions and says how many nodes of other scripts call them")
{
	VisualScriptDocument library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	VisualScriptFunction clamp;
	clamp.ID = 10;
	clamp.Name = "Clamp01";
	library.Functions = {clamp};
	PaneFixture fixture(library, {FixturePane::Sidebar});
	VisualScriptDocument door;
	door.Graph.Nodes = {MakeRemoteNode(1, "Function.Call", fixture.Graph, "Function", 10),
	                    MakeRemoteNode(2, "Function.Call", fixture.Graph, "Function", 10)};
	VisualScriptDocument enemy;
	enemy.Graph.Nodes = {MakeRemoteNode(1, "Function.Call", fixture.Graph, "Function", 10)};
	std::vector<VisualScriptProjectGraph> const graphs = {MakeReferringGraph(PaneDoor, "Door", door),
	                                                      MakeReferringGraph(PaneEnemy, "Enemy", enemy)};
	fixture.Model->SetProjectGraphs(graphs, 1);
	fixture.CheckDrawingChangesNothing();
	CHECK(fixture.Sidebar.GetLibraryLine() == "Function library \xC2\xB7 used by 3 nodes in 2 scripts");
	// Its functions, no event graph, no variables or custom events, and nothing to add but functions.
	CHECK(fixture.Sidebar.FindRow({VisualScriptElementKind::Function, 10}));
	CHECK_FALSE(fixture.Sidebar.FindRow({VisualScriptElementKind::Function, 0}));
	CHECK(fixture.Sidebar.FindAddButton(VisualScriptElementKind::Function));
	CHECK_FALSE(fixture.Sidebar.FindAddButton(VisualScriptElementKind::Variable));
	CHECK_FALSE(fixture.Sidebar.FindAddButton(VisualScriptElementKind::CustomEvent));

	// What cannot stay in a library shows until it is deleted, and nothing new of it can be added.
	VisualScriptDocument leftovers = fixture.GetDocument();
	leftovers.Variables = {MakeVariable(100, "Speed", "float")};
	leftovers.Graph.Nodes = {MakeNode(1, "Event.Custom", glm::vec2(0.0f), Json::object({{"Name", "Hit"}}))};
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, leftovers).IsOk());
	fixture.Frames(2);
	CHECK(fixture.Sidebar.FindRow({VisualScriptElementKind::Variable, 100}));
	CHECK(fixture.Sidebar.FindRow({VisualScriptElementKind::CustomEvent, 1}));
	CHECK(fixture.Sidebar.FindRow({VisualScriptElementKind::Function, 0}));
	CHECK_FALSE(fixture.Sidebar.FindAddButton(VisualScriptElementKind::Variable));
	CHECK_FALSE(fixture.Sidebar.FindAddButton(VisualScriptElementKind::CustomEvent));

	// Unused, it says so; a script's My Script has no such line.
	fixture.Model->SetProjectGraphs({}, 2);
	fixture.Frames(1);
	CHECK(fixture.Sidebar.GetLibraryLine() == "Function library \xC2\xB7 used by no script yet");
	VisualScriptDocument script = fixture.GetDocument();
	script.Kind = VisualScriptDocumentKind::Script;
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, script).IsOk());
	fixture.Frames(2);
	CHECK(fixture.Sidebar.GetLibraryLine().empty());
	CHECK(fixture.Sidebar.FindAddButton(VisualScriptElementKind::Variable));
}

TEST_CASE("VisualScriptEditor: Details' Find Callers of a library function lists its calls in every script")
{
	VisualScriptDocument library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	VisualScriptFunction clamp;
	clamp.ID = 10;
	clamp.Name = "Clamp01";
	library.Functions = {clamp};
	PaneFixture fixture(library, {FixturePane::Details});
	VisualScriptDocument door;
	door.Graph.Nodes = {MakeRemoteNode(1, "Function.Call", fixture.Graph, "Function", 10)};
	std::vector<VisualScriptProjectGraph> const graphs = {MakeReferringGraph(PaneDoor, "Door", door)};
	fixture.Model->SetProjectGraphs(graphs, 1);
	fixture.Shared.Element = VisualScriptElement{VisualScriptElementKind::Function, 10};
	fixture.CheckDrawingChangesNothing();
	REQUIRE(fixture.Details.FindRow("Find Callers"));
	fixture.ImGui.Click(fixture.DetailsRow("Find Callers"), fixture.Drawer());
	fixture.Frames(2);
	CHECK(HasFindReferences(fixture.Results, VisualScriptReference{VisualScriptReference::Kind::Function, 10, {}, fixture.Graph}));
}

TEST_CASE("VisualScriptEditor: a value dragged in Details survives an outside change of another node, and is dropped with a toast "
          "over one of its own")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(4, "Flow.Delay", glm::vec2(400.0f, 300.0f)));
	PaneFixture fixture(document, {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
	fixture.SelectElements({4});
	size_t const undo = fixture.GetUndoCount();
	ImVec2 const start = fixture.DetailsRow("Duration", 0.3f);
	ImGuiIO& io = ImGui::GetIO();
	io.AddMousePosEvent(start.x, start.y);
	fixture.Frames(1);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	fixture.Frames(1);
	// The first change commits at once, the next waits for the cadence: a value is pending when the agent's edit lands.
	io.AddMousePosEvent(start.x + 8.0f, start.y);
	fixture.Frames(1);
	io.AddMousePosEvent(start.x + 16.0f, start.y);
	fixture.Frames(1);
	REQUIRE(fixture.Details.HasPendingEdit());
	REQUIRE(fixture.GetUndoCount() == undo + 1);

	auto const agentEdit = [&fixture](uint32_t node, std::string comment)
	{
		VisualScriptDocument edited = fixture.GetDocument();
		for (VisualScriptNode& candidate : edited.Graph.Nodes)
		{
			if (candidate.ID == node)
			{
				candidate.Comment = comment;
			}
		}
		REQUIRE(
			fixture.Operations.EditVisualScript(fixture.Graph, CreateRef<VisualScriptDocument const>(std::move(edited)), "Agent").IsOk());
	};

	SUBCASE("another node: the value is kept, and the drag goes on in a step after the agent's")
	{
		agentEdit(2, "Logs");
		fixture.Frames(1);
		CHECK(fixture.Details.HasPendingEdit());
		io.AddMousePosEvent(start.x + 24.0f, start.y);
		fixture.Frames(1);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		fixture.Frames(3);
		VisualScriptDocument const& after = fixture.GetDocument();
		CHECK(FindVisualScriptNode(after, 2)->Comment == "Logs");
		CHECK(FindVisualScriptNode(after, 4)->Inputs.value("Duration", Json()).is_number());
		// The drag before the agent's edit, the agent's, and the drag after it.
		CHECK(fixture.GetUndoCount() == undo + 3);
		CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Duration on Delay in Graph");
		REQUIRE(fixture.Operations.Undo().IsOk());
		CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Agent");
		CHECK_FALSE(HasNote(fixture.Results, "Your edit of Duration was replaced by a change made elsewhere"));
	}
	SUBCASE("the dragged node: the value is dropped, and the toast says so")
	{
		agentEdit(4, "Waits");
		fixture.Frames(1);
		CHECK(HasNote(fixture.Results, "Your edit of Duration was replaced by a change made elsewhere"));
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		fixture.Frames(3);
		CHECK(FindVisualScriptNode(fixture.GetDocument(), 4)->Comment == "Waits");
	}
	SUBCASE("the dragged node, flushed between draws: nothing is committed, and the next draw says so")
	{
		agentEdit(4, "Waits");
		// The window synced its model with the agent's edit, and the window manager flushes before the panes draw again.
		fixture.Model->Sync(*fixture.ActiveSemantics, fixture.Measure, fixture.Theme, false, fixture.Time);
		CHECK_FALSE(fixture.Details.TakePendingEdit().has_value());
		CHECK_FALSE(fixture.Details.HasPendingEdit());
		CHECK_FALSE(HasNote(fixture.Results, "Your edit of Duration was replaced by a change made elsewhere"));
		fixture.Frames(1);
		CHECK(HasNote(fixture.Results, "Your edit of Duration was replaced by a change made elsewhere"));
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		fixture.Frames(3);
		CHECK(FindVisualScriptNode(fixture.GetDocument(), 4)->Comment == "Waits");
		// The drag's first step and the agent's.
		CHECK(fixture.GetUndoCount() == undo + 2);
	}
}

TEST_CASE("VisualScriptEditor: a Kind switch that breaks other scripts' nodes asks first, in both directions, and tells after it")
{
	// An empty script: nothing of its own stays behind in a library. Door runs it on another entity.
	PaneFixture fixture(VisualScriptDocument(), {FixturePane::Details});
	VisualScriptDocument door;
	door.Graph.Nodes = {MakeNode(1, "RunsGraph", glm::vec2(0.0f), Json::object({{"Graph", fixture.Graph.ToString()}}))};
	std::vector<VisualScriptProjectGraph> const runners = {MakeReferringGraph(PaneDoor, "Door", door)};
	fixture.Model->SetProjectGraphs(runners, 1);
	fixture.CheckDrawingChangesNothing();
	REQUIRE(fixture.Details.GetSubject() == VisualScriptDetailsSubject::Script);
	size_t const undo = fixture.GetUndoCount();

	PickDetailsItem(fixture, "Kind", "Function Library");
	REQUIRE(fixture.Details.GetKindQuestion().has_value());
	CHECK(*fixture.Details.GetKindQuestion() ==
	      "Make Graph a function library? 1 node in 1 other script uses it as a script: they will show errors until fixed.");
	CHECK(fixture.GetUndoCount() == undo);
	fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	REQUIRE(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.GetDocument().Kind == VisualScriptDocumentKind::Library);

	// Back to a script while Door and Enemy call its function: asked again, the other way.
	VisualScriptDocument library = fixture.GetDocument();
	VisualScriptFunction clamp;
	clamp.ID = 10;
	clamp.Name = "Clamp01";
	library.Functions = {clamp};
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, library).IsOk());
	VisualScriptDocument caller;
	caller.Graph.Nodes = {MakeRemoteNode(1, "Function.Call", fixture.Graph, "Function", 10),
	                      MakeRemoteNode(2, "Function.Call", fixture.Graph, "Function", 10)};
	VisualScriptDocument enemy;
	enemy.Graph.Nodes = {MakeRemoteNode(1, "Function.Call", fixture.Graph, "Function", 10)};
	std::vector<VisualScriptProjectGraph> const callers = {MakeReferringGraph(PaneDoor, "Door", caller),
	                                                       MakeReferringGraph(PaneEnemy, "Enemy", enemy)};
	fixture.Model->SetProjectGraphs(callers, 2);
	fixture.Frames(2);
	size_t const before = fixture.GetUndoCount();
	PickDetailsItem(fixture, "Kind", "Script");
	REQUIRE(fixture.Details.GetKindQuestion().has_value());
	CHECK(*fixture.Details.GetKindQuestion() ==
	      "Make Graph a script? 3 nodes in 2 other scripts call its functions: they will show errors until fixed.");
	SUBCASE("Escape keeps it a library")
	{
		fixture.ImGui.Press(ImGuiKey_Escape, fixture.Drawer());
		fixture.Frames(2);
		CHECK(fixture.GetUndoCount() == before);
		CHECK(fixture.GetDocument().Kind == VisualScriptDocumentKind::Library);
	}
	SUBCASE("Enter makes it a script in one step")
	{
		fixture.ImGui.Press(ImGuiKey_Enter, fixture.Drawer());
		fixture.Frames(2);
		CHECK(fixture.GetUndoCount() == before + 1);
		CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Make Graph a Script in Graph");
		CHECK(fixture.GetDocument().Kind == VisualScriptDocumentKind::Script);
	}

	// After the switch, the window's toast says the same (its index lists this graph too, by name).
	std::vector<VisualScriptProjectGraph> named = callers;
	named.push_back(MakeReferringGraph(fixture.Graph, "Motion", library));
	VisualScriptDocument script = library;
	script.Kind = VisualScriptDocumentKind::Script;
	std::vector<VisualScriptOfferToast> const toasts = DescribeVisualScriptOfferChange(fixture.Graph, library, script, named);
	REQUIRE(toasts.size() == 1);
	CHECK(toasts[0].Text == "Made Motion a script: 3 nodes in 2 other scripts show errors");
	CHECK(CountVisualScriptRemovedReferrers(fixture.Graph, library, script, named) == VisualScriptOtherUses{3, 2});
	// A switch no other script notices tells nothing.
	CHECK(DescribeVisualScriptOfferChange(fixture.Graph, library, script, {}).empty());
}

TEST_CASE("VisualScriptEditor: a library whose event graph holds only comment boxes offers it in My Script, to delete them")
{
	VisualScriptDocument library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	VisualScriptFunction clamp;
	clamp.ID = 10;
	clamp.Name = "Clamp01";
	library.Functions = {clamp};
	CHECK_FALSE(OffersVisualScriptEventGraph(library));
	PaneFixture fixture(library, {FixturePane::Sidebar});
	fixture.CheckDrawingChangesNothing();
	CHECK_FALSE(fixture.Sidebar.FindRow({VisualScriptElementKind::Function, 0}));
	VisualScriptDocument boxed = fixture.GetDocument();
	VisualScriptComment box;
	box.ID = 1;
	box.Text = "Old notes";
	box.Position = glm::vec2(0.0f);
	box.Size = glm::vec2(400.0f, 200.0f);
	boxed.Graph.Comments = {box};
	CHECK(OffersVisualScriptEventGraph(boxed));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, boxed).IsOk());
	fixture.Frames(2);
	CHECK(fixture.Sidebar.FindRow({VisualScriptElementKind::Function, 0}));
	// A script always offers its event graph.
	CHECK(OffersVisualScriptEventGraph(VisualScriptDocument()));
}
