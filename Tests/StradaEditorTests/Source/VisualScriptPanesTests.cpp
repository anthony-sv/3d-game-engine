#include "HeadlessImGui.h"
#include "TestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"
#include "Editor/VisualScripting/VisualScriptLiteralEditor.h"
#include "Editor/VisualScripting/VisualScriptPalettePopup.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Script/VisualScriptLiterals.h"

#include <doctest/doctest.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
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

	// A graph window's model of a project's graph and one pane drawn in a window beside it, frame by frame as the panel draws
	// them: Sync, Update, the pane, and the pane's commit executed as the panel executes it.
	struct PaneFixture
	{
		explicit PaneFixture(VisualScriptDocument const& document)
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
			return {*Model, Semantics, Settings, Session, Services, Graph, "Graph", UUID::Invalid(), false, Time, Measure, {}};
		}

		void Draw()
		{
			Time += 1.0 / 60.0;
			Model->Sync(Semantics, Measure, Theme, false, Time);
			VisualScriptCanvasInput input;
			input.CanvasSize = glm::vec2(800.0f, 600.0f);
			input.Time = Time;
			Model->Update(input);
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(WindowSize);
			ImGui::Begin("Graph", nullptr, ImGuiWindowFlags_NoSavedSettings);
			VisualScriptPaneContext context = MakeContext();
			VisualScriptPaneResult result = Palette.Draw(context);
			ImGui::End();
			if (result.Commit)
			{
				VisualScriptCommitRequest request = std::move(*result.Commit);
				Ref<VisualScriptDocument const> const committed = request.Document;
				REQUIRE(
					Operations.EditVisualScript(Graph, committed, request.Description, request.MergeKey, std::move(request.Text)).IsOk());
				Model->OnCommitted(committed);
				Model->Sync(Semantics, Measure, Theme, false, Time);
				// The panel selects what the commit asks for.
				Model->Select(request.Select);
			}
			Results.push_back(std::move(result));
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

		std::string ReadFile() const
		{
			Result<std::string> text = FileSystem::ReadTextFile(File);
			REQUIRE(text.IsOk());
			return text.TakeValue();
		}

		uint64_t GetVersion() const { return AssetManager::GetAsset<VisualScriptAsset>(Graph)->GetVersion(); }
		VisualScriptDocument const& GetDocument() const { return AssetManager::GetAsset<VisualScriptAsset>(Graph)->GetDocument(); }

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

		struct
		{
			ImFont* Monospace = nullptr;
		} Fonts;
		Testing::HeadlessImGui ImGui;
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Temporary;
		EditorContext Context;
		EditorOperations Operations{Context};
		AssetHandle Graph;
		std::filesystem::path File;
		VisualScriptViewSettings Settings{Temporary.GetPath() / "Views.json"};
		Testing::FixtureVisualScriptSemantics Semantics;
		VisualScriptTextMeasurer Measure = Testing::FixedWidthMeasurer(0.5f);
		VisualScriptThemeAnchors Theme;
		VisualScriptEditorServices Services;
		EditSession Session{SessionKey};
		std::optional<VisualScriptEditorModel> Model;
		VisualScriptPalettePopup Palette;
		std::vector<VisualScriptPaneResult> Results;
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

TEST_CASE("VisualScriptEditor: a node chosen from an exec output's palette goes into its link, in one step")
{
	PaneFixture fixture(MakeDocument());
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
	headless.Frame(draw);
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
	fixture.Headless.Frame(fixture.Drawer());
	CHECK(fixture.Table == nullptr);
	CHECK(fixture.Changes.empty());
	fixture.Headless.DragPath(fixture.FrameCenter(), ImVec2(fixture.FrameCenter().x + 60.0f, fixture.FrameCenter().y), fixture.Drawer());
	REQUIRE_FALSE(fixture.Changes.empty());
	CHECK(fixture.Changes.back().Value.get<float>() > 2.5f);
	CHECK(fixture.Changes.back().Path.empty());

	InlineFixture compact;
	compact.Field = MakeField(FieldKind::Vec3);
	compact.Value = Json::array({1.0, 2.0, 3.0});
	compact.Headless.Frame(compact.Drawer());
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
		fixture.Headless.Frame(fixture.Drawer());
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
		fixture.Headless.Frame(fixture.Drawer());
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
		fixture.Headless.Frame(fixture.Drawer());
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
		fixture.Headless.Frame(fixture.Drawer());
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
		headless.Frame(draw);
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
	headless.Frame(draw);
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
