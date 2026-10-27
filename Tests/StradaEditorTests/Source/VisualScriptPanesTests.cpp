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
#include "Editor/VisualScripting/VisualScriptLiteralEditor.h"
#include "Editor/VisualScripting/VisualScriptPalettePopup.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptQuickFixes.h"
#include "Editor/VisualScripting/VisualScriptSidebar.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
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
			return {*Model,  *ActiveSemantics, Settings, Session, Services, Shared, Graph,
			        "Graph", ContextEntity,    Playing,  Time,    Measure,  {},     ReadLiveVariables};
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
			Model->OnCommitted(committed);
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
			if (!result.FocusSetting.empty())
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
						DrawPane("Details", DetailsPosition, DetailsSize,
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
}

TEST_CASE("VisualScriptEditor: renaming a function's input in Details renames the links of its calls and of its entry, in one step")
{
	PaneFixture fixture(MakeFunctionDocument(), {FixturePane::Details});
	fixture.CheckDrawingChangesNothing();
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
	// Alive is private: any type with values but arrays.
	std::vector<std::string> const privateTypes = pickerTypes(101);
	CHECK(lists(privateTypes, "Strada.Matrix4"));
	CHECK(lists(privateTypes, "Strada.RaycastHit"));
	CHECK_FALSE(lists(privateTypes, "Strada.Entity[]"));
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
