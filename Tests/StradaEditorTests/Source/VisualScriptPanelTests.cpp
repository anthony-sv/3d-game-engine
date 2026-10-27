#include "HeadlessImGui.h"
#include "TestUtilities.h"
#include "VisualScriptPanelTestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/Panels/VisualScriptEditorPanel.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	using Action = VisualScriptEditorAction;

	// The edit session prefix every graph window shares ("VSED").
	constexpr uint64_t SessionKey = 0x5653454400000000ull;

	VisualScriptNode MakeNode(uint32_t id, std::string type, std::optional<glm::vec2> position, Json settings = Json::object(),
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

	// The script "Player": Create (1) -> Delay (2), a Log Info (3) not linked, and an Add (6), in a row.
	VisualScriptDocument MakeDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes = {
			MakeNode(1, "Event.Create", glm::vec2(0.0f, 0.0f)),
			MakeNode(2, "Flow.Delay", glm::vec2(400.0f, 0.0f), Json::object(), Json::object({{"Duration", 1.5}})),
			MakeNode(3, "Call:Strada.Log.Info(string)", glm::vec2(800.0f, 0.0f), Json::object(), Json::object({{"Message", "Hi"}})),
			MakeNode(6, "Operator.Add", glm::vec2(400.0f, 320.0f)),
		};
		document.Graph.Links = {MakeLink(1, "Then", 2, "Execute")};
		return document;
	}

	ImVec2 ToImVec2(glm::vec2 point)
	{
		return ImVec2(point.x, point.y);
	}

	// A graph window of the project's script "Player", drawn headless at a fixed place frame by frame as the editor draws it:
	// the window manager's end of widget interactions (spec §4.4 rule 2), then the window. The fixture library stands in for
	// the scripting runtime; Reduce Motion makes views move at once.
	struct PanelFixture
	{
		explicit PanelFixture(VisualScriptDocument const& document)
		{
			Services.MonospaceFont = Gui.UseEditorStyle();
			REQUIRE(Operations.CreateProject(Temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			File = AssetManager::GetAssetDirectory() / "Scripts" / "Player.sgraph";
			REQUIRE(FileSystem::CreateDirectories(File.parent_path()).IsOk());
			REQUIRE(VisualScriptSerializer::SaveToFile(document, File).IsOk());
			REQUIRE(Operations.RefreshAssets().IsOk());
			Graph = AssetManager::FindByPath("Scripts/Player.sgraph");
			REQUIRE(Graph.IsValid());
			Settings.SetProject(Context.GetProject()->GetFilePath());
			Settings.SetReduceMotion(true);
			Panel.emplace(Graph, Operations, Semantics, Settings, Session, Services);
			Frames(3);
		}

		void Draw()
		{
			VisualScriptEditorPanel* panels[] = {&*Panel};
			Testing::EndInteractions(panels, Session, Context.GetHistory(), PreviousActive);
			if (Probe)
			{
				Probe();
			}
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(WindowSize);
			Panel->OnImGuiRender(Open, std::nullopt);
		}

		std::function<void()> Drawer()
		{
			return [this]
			{
				Draw();
			};
		}

		void Frames(int count, float seconds = 1.0f / 60.0f)
		{
			for (int i = 0; i < count; i++)
			{
				Gui.Frame(Drawer(), seconds);
			}
		}

		// Presses and releases a key (with modifiers) in a frame that comes seconds after the previous one.
		void PressAfter(ImGuiKeyChord chord, float seconds)
		{
			ImGuiIO& io = ImGui::GetIO();
			ImGuiKey const key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
			if ((chord & ImGuiMod_Alt) != 0)
			{
				io.AddKeyEvent(ImGuiMod_Alt, true);
			}
			io.AddKeyEvent(key, true);
			Gui.Frame(Drawer(), seconds);
			io.AddKeyEvent(key, false);
			if ((chord & ImGuiMod_Alt) != 0)
			{
				io.AddKeyEvent(ImGuiMod_Alt, false);
			}
			Gui.Frame(Drawer(), 1.0f / 60.0f);
		}

		void Click(glm::vec2 point, ImGuiMouseButton button = ImGuiMouseButton_Left) { Gui.MouseClick(ToImVec2(point), button, Drawer()); }

		void MoveMouse(glm::vec2 point)
		{
			ImGui::GetIO().AddMousePosEvent(point.x, point.y);
			Gui.Frame(Drawer());
		}

		void SetButton(ImGuiMouseButton button, bool down)
		{
			ImGui::GetIO().AddMouseButtonEvent(button, down);
			Gui.Frame(Drawer());
		}

		VisualScriptEditorModel const& Model() const { return Panel->GetModel(); }

		std::string ReadFile() const
		{
			Result<std::string> text = FileSystem::ReadTextFile(File);
			REQUIRE(text.IsOk());
			return text.TakeValue();
		}

		uint64_t GetVersion() const { return AssetManager::GetAsset<VisualScriptAsset>(Graph)->GetVersion(); }
		VisualScriptDocument const& GetDocument() const { return AssetManager::GetAsset<VisualScriptAsset>(Graph)->GetDocument(); }
		size_t GetUndoCount() const { return Context.GetHistory().GetUndoCount(); }

		VisualScriptNode const* FindNode(uint32_t id) const
		{
			for (VisualScriptNode const& node : GetDocument().Graph.Nodes)
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
			std::vector<VisualScriptLink> const& links = GetDocument().Graph.Links;
			return std::find(links.begin(), links.end(), link) != links.end();
		}

		// The panel invariant (spec §15.4): three frames of drawing change no undo count, no file byte and no asset version.
		void CheckDrawingChangesNothing()
		{
			size_t const undo = GetUndoCount();
			std::string const bytes = ReadFile();
			uint64_t const version = GetVersion();
			Frames(3);
			CHECK(GetUndoCount() == undo);
			CHECK(ReadFile() == bytes);
			CHECK(GetVersion() == version);
			CHECK(Open);
		}

		ImGuiWindow* GetWindow() const { return ImGui::FindWindowByName(VisualScriptEditorPanel::GetWindowName(Graph, "Player").c_str()); }

		ImGuiWindow* GetChild(char const* name) const
		{
			ImGuiWindow const* parent = GetWindow();
			for (ImGuiWindow* window : GImGui->Windows)
			{
				if (window->ParentWindow == parent && std::strstr(window->Name, name) != nullptr)
				{
					return window;
				}
			}
			FAIL("the graph window has no child ", name);
			return nullptr;
		}

		ImGuiID GetSurface() const
		{
			ImGuiWindow const* canvas = GetChild("##canvas");
			return ImHashStr("##surface", 0, canvas->ID);
		}

		// A point of the canvas with nothing on it: near its bottom-left corner (the minimap is at the bottom-right).
		glm::vec2 EmptyPoint() const
		{
			ImGuiWindow const* canvas = GetChild("##canvas");
			return glm::vec2(canvas->Pos.x + 30.0f, canvas->Pos.y + canvas->Size.y - 30.0f);
		}

		// Moves the mouse down a popup's items a frame at a time until the item is hovered; the point found.
		std::optional<glm::vec2> FindPopupItem(char const* label)
		{
			if (GImGui->OpenPopupStack.Size == 0)
			{
				return std::nullopt;
			}
			ImGuiWindow const* popup = GImGui->OpenPopupStack.back().Window;
			REQUIRE(popup != nullptr);
			ImGuiID const item = ImHashStr(label, 0, popup->ID);
			for (float y = popup->Pos.y + 2.0f; y < popup->Pos.y + popup->Size.y; y += 3.0f)
			{
				glm::vec2 const point(popup->Pos.x + popup->Size.x * 0.5f, y);
				MoveMouse(point);
				if (GImGui->HoveredId == item)
				{
					return point;
				}
			}
			return std::nullopt;
		}

		Testing::HeadlessImGui Gui;
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Temporary;
		EditorContext Context;
		EditorOperations Operations{Context};
		AssetHandle Graph;
		std::filesystem::path File;
		VisualScriptViewSettings Settings{Temporary.GetPath() / "Views.json"};
		Testing::FixtureVisualScriptSemantics Semantics;
		VisualScriptEditorServices Services;
		EditSession Session{SessionKey};
		std::optional<VisualScriptEditorPanel> Panel;
		// Drawn before the window every frame (the editor's global shortcuts in the probe tests).
		std::function<void()> Probe;
		ImGuiID PreviousActive = 0;
		bool Open = true;
		ImVec2 WindowSize{1600.0f, 900.0f};
	};
}

TEST_CASE("VisualScriptEditor: a node drag is one step, drawn moved in its release frame, and undo gives the file its bytes back")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	std::string const bytes = fixture.ReadFile();
	size_t const undo = fixture.GetUndoCount();
	glm::vec2 const before = *fixture.FindNode(2)->Position;

	// The header of Delay, dragged by a press, four moves and a release.
	glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), 2, glm::vec2(0.5f, 0.06f));
	fixture.MoveMouse(from);
	fixture.SetButton(ImGuiMouseButton_Left, true);
	for (int step = 1; step <= 4; step++)
	{
		fixture.MoveMouse(from + glm::vec2(40.0f, 24.0f) * static_cast<float>(step));
	}
	CHECK(fixture.GetUndoCount() == undo);
	fixture.SetButton(ImGuiMouseButton_Left, false);

	REQUIRE(fixture.GetUndoCount() == undo + 1);
	// Spec §4.6 (which owns descriptions) leaves the count out for one: "Move Node".
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Move Node in Player");
	glm::vec2 const after = *fixture.FindNode(2)->Position;
	CHECK(after != before);
	CHECK(std::fmod(after.x, VisualScriptCanvasMetrics::GridSize) == doctest::Approx(0.0f));
	// The frame of the release drew the committed position: the canvas the renderer drew has it, nothing previews a move.
	std::optional<uint32_t> const index = fixture.Model().GetCanvas().FindNode(2);
	REQUIRE(index.has_value());
	CHECK(fixture.Model().GetCanvas().GetNodes()[*index].Position == after);
	CHECK(fixture.Model().GetGesture().Kind == VisualScriptGesture::Idle);

	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Frames(2);
	CHECK(fixture.ReadFile() == bytes);
	CHECK(*fixture.FindNode(2)->Position == before);
}

TEST_CASE("VisualScriptEditor: the first edit writes the positions of the nodes that had none, in the same step")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(7, "Flow.Delay", std::nullopt));
	PanelFixture fixture(document);
	fixture.CheckDrawingChangesNothing();
	std::string const bytes = fixture.ReadFile();
	REQUIRE_FALSE(fixture.FindNode(7)->Position.has_value());
	size_t const undo = fixture.GetUndoCount();

	glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f));
	fixture.Gui.DragPath(ToImVec2(from), ToImVec2(from + glm::vec2(64.0f, 48.0f)), fixture.Drawer());

	REQUIRE(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.FindNode(7)->Position.has_value());
	// The asset holds the document the step wrote to the file.
	for (VisualScriptNode const& node : fixture.GetDocument().Graph.Nodes)
	{
		CHECK_MESSAGE(node.Position.has_value(), "node ", node.ID, " was written without a position");
	}
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile() == bytes);
}

TEST_CASE(
	"VisualScriptEditor: a wire is one step, a wire to a pin it cannot connect to adds none, and one released on empty canvas asks for "
	"the palette with its pin")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();

	glm::vec2 const delayOut = Testing::CanvasPoint(fixture.Model(), 2, "Completed", VisualScriptPinDirection::Out);
	glm::vec2 const logIn = Testing::CanvasPoint(fixture.Model(), 3, "Execute");
	fixture.Gui.DragPath(ToImVec2(delayOut), ToImVec2(logIn), fixture.Drawer());
	REQUIRE(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.HasLink(MakeLink(2, "Completed", 3, "Execute")));

	// A string input onto the flow's output: no Convert bridges them, so nothing happens.
	glm::vec2 const message = Testing::CanvasPoint(fixture.Model(), 3, "Message");
	glm::vec2 const createThen = Testing::CanvasPoint(fixture.Model(), 1, "Then", VisualScriptPinDirection::Out);
	fixture.Gui.DragPath(ToImVec2(message), ToImVec2(createThen), fixture.Drawer());
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK_FALSE(fixture.Panel->GetState().PaletteOpen);

	// Released on empty canvas: the palette, in context mode for the pin, and still nothing written.
	glm::vec2 const logThen = Testing::CanvasPoint(fixture.Model(), 3, "Then", VisualScriptPinDirection::Out);
	fixture.Gui.DragPath(ToImVec2(logThen), ToImVec2(fixture.EmptyPoint()), fixture.Drawer());
	CHECK(fixture.GetUndoCount() == undo + 1);
	std::optional<VisualScriptPaletteIntent> const& palette = fixture.Panel->GetPaletteRequest();
	REQUIRE(palette.has_value());
	REQUIRE(palette->From.has_value());
	CHECK(palette->From->Node == 3);
	CHECK(palette->From->Pin == "Then");
	CHECK(palette->Direction == VisualScriptPinDirection::Out);
	CHECK(fixture.Panel->GetState().PaletteOpen);
	CHECK(fixture.Model().GetView().ToScreen(palette->Position).x == doctest::Approx(fixture.EmptyPoint().x).epsilon(0.01));
}

TEST_CASE("VisualScriptEditor: Delete in a focused graph window deletes its nodes, never the selected entities")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	EntityCreateInfo info;
	info.Name = "Hero";
	Result<UUID> const hero = fixture.Operations.CreateEntity(info);
	REQUIRE(hero.IsOk());
	std::vector<UUID> const heroSelection = {hero.GetValue()};
	REQUIRE(fixture.Operations.Select(heroSelection).IsOk());
	// The editor's global Delete, which deletes the selected entities: it must not fire while the graph window is focused.
	bool globalDelete = false;
	fixture.Probe = [&globalDelete]
	{
		globalDelete = globalDelete || ImGui::Shortcut(ImGuiKey_Delete, ImGuiInputFlags_RouteGlobal);
	};

	fixture.Click(Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f)));
	REQUIRE(std::vector<uint32_t>(fixture.Model().GetSelection().begin(), fixture.Model().GetSelection().end()) ==
	        std::vector<uint32_t>{3});
	CHECK(fixture.Panel->GetFocusedPart() == VisualScriptWindowPart::Canvas);
	size_t const undo = fixture.GetUndoCount();
	globalDelete = false;
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	fixture.Frames(1);

	CHECK_FALSE(globalDelete);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.FindNode(3) == nullptr);
	CHECK(fixture.Context.GetScene().HasEntity(hero.GetValue()));
	CHECK(fixture.Context.GetSelection().Contains(hero.GetValue()));

	// With nothing selected on the canvas the keys stay the window's too.
	globalDelete = false;
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	fixture.Frames(1);
	CHECK_FALSE(globalDelete);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetScene().HasEntity(hero.GetValue()));
}

TEST_CASE("VisualScriptEditor: the canvas owns Escape: it cancels a marquee without losing the press, and clears the selection without "
          "losing the focus")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	ImGuiWindow const* canvas = fixture.GetChild("##canvas");

	// A marquee from the empty canvas, still held.
	glm::vec2 const start = fixture.EmptyPoint();
	fixture.MoveMouse(start);
	fixture.SetButton(ImGuiMouseButton_Left, true);
	fixture.MoveMouse(start + glm::vec2(60.0f, -60.0f));
	fixture.MoveMouse(start + glm::vec2(120.0f, -120.0f));
	REQUIRE(fixture.Model().GetGesture().Kind == VisualScriptGesture::Marquee);
	REQUIRE(GImGui->ActiveId == fixture.GetSurface());
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	CHECK(fixture.Model().GetGesture().Kind == VisualScriptGesture::Idle);
	CHECK(GImGui->ActiveId == fixture.GetSurface());
	CHECK(GImGui->NavWindow == canvas);
	fixture.MoveMouse(start + glm::vec2(140.0f, -140.0f));
	CHECK(GImGui->ActiveId == fixture.GetSurface());
	fixture.SetButton(ImGuiMouseButton_Left, false);
	CHECK(fixture.Model().GetSelection().empty());

	// A selection and no gesture: Escape clears it and the canvas keeps the keyboard.
	fixture.Click(Testing::CanvasPoint(fixture.Model(), 2, glm::vec2(0.5f, 0.06f)));
	REQUIRE_FALSE(fixture.Model().GetSelection().empty());
	REQUIRE(GImGui->NavWindow == canvas);
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Model().GetSelection().empty());
	CHECK(GImGui->NavWindow == canvas);
	CHECK(fixture.Panel->GetFocusedPart() == VisualScriptWindowPart::Canvas);
}

TEST_CASE("VisualScriptEditor: with keyboard navigation, Escape moves the focus of a child window that does not own it to its parent")
{
	// The control for the canvas's ownership of Escape: ImGui's navigation cancel acts on every Escape nobody owns.
	Testing::HeadlessImGui imgui;
	imgui.UseEditorStyle();
	auto const draw = []
	{
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
		ImGui::Begin("Control", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavInputs);
		ImGui::BeginChild("##child", ImVec2(300.0f, 200.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs);
		ImGui::Button("Inside", ImVec2(200.0f, 100.0f));
		ImGui::EndChild();
		ImGui::End();
	};
	imgui.Frame(draw);
	imgui.Frame(draw);
	ImGuiWindow const* parent = ImGui::FindWindowByName("Control");
	REQUIRE(parent != nullptr);
	imgui.Click(ImVec2(parent->Pos.x + 60.0f, parent->Pos.y + 80.0f), draw);
	REQUIRE(GImGui->NavWindow != nullptr);
	REQUIRE(GImGui->NavWindow->ParentWindow == parent);
	imgui.Press(ImGuiKey_Escape, draw);
	imgui.Frame(draw);
	CHECK(GImGui->NavWindow == parent);
}

TEST_CASE("VisualScriptEditor: a right drag pans and opens no menu, and a right click on a node selects it and opens its menu")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	glm::vec2 const center = fixture.Model().GetViewState().Center;
	glm::vec2 const start = fixture.EmptyPoint() + glm::vec2(200.0f, -100.0f);
	fixture.Gui.DragPath(ToImVec2(start), ToImVec2(start + glm::vec2(120.0f, 60.0f)), fixture.Drawer(), ImGuiMouseButton_Right);
	CHECK(fixture.Model().GetViewState().Center != center);
	CHECK(GImGui->OpenPopupStack.Size == 0);

	fixture.Click(Testing::CanvasPoint(fixture.Model(), 6, glm::vec2(0.5f, 0.5f)), ImGuiMouseButton_Right);
	fixture.Frames(1);
	CHECK(std::vector<uint32_t>(fixture.Model().GetSelection().begin(), fixture.Model().GetSelection().end()) == std::vector<uint32_t>{6});
	CHECK(GImGui->OpenPopupStack.Size == 1);

	// A click on the canvas outside the menu closes it, and that click is the menu's: it selects and starts nothing.
	size_t const undo = fixture.GetUndoCount();
	fixture.Click(fixture.EmptyPoint());
	fixture.Frames(1);
	CHECK(GImGui->OpenPopupStack.Size == 0);
	CHECK(fixture.Model().GetGesture().Kind == VisualScriptGesture::Idle);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(std::vector<uint32_t>(fixture.Model().GetSelection().begin(), fixture.Model().GetSelection().end()) == std::vector<uint32_t>{6});
}

TEST_CASE("VisualScriptEditor: with three nodes selected, the menu of one acts on the three in one step")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Click(Testing::CanvasPoint(fixture.Model(), 1, glm::vec2(0.5f, 0.06f)));
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
	fixture.Click(Testing::CanvasPoint(fixture.Model(), 2, glm::vec2(0.5f, 0.06f)));
	fixture.Click(Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f)));
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
	fixture.Frames(1);
	REQUIRE(fixture.Model().GetSelection().size() == 3);
	size_t const undo = fixture.GetUndoCount();

	fixture.Click(Testing::CanvasPoint(fixture.Model(), 2, glm::vec2(0.5f, 0.06f)), ImGuiMouseButton_Right);
	fixture.Frames(1);
	CHECK(fixture.Model().GetSelection().size() == 3);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	std::optional<glm::vec2> const item = fixture.FindPopupItem("Collapse 3 Nodes");
	REQUIRE(item.has_value());
	fixture.Click(*item);
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Collapse 3 Nodes in Player");
	for (uint32_t const id : {1u, 2u, 3u})
	{
		CHECK(fixture.FindNode(id)->Collapsed);
	}
	CHECK_FALSE(fixture.FindNode(6)->Collapsed);
}

TEST_CASE("VisualScriptEditor: Ctrl+C and Ctrl+V copy and paste through the clipboard, and Ctrl+F opens find without moving the view")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Click(Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f)));
	size_t const undo = fixture.GetUndoCount();
	size_t const comments = fixture.GetDocument().Graph.Comments.size();
	size_t const nodes = fixture.GetDocument().Graph.Nodes.size();

	// Ctrl+C copies; C alone would add a comment box, which a chord never triggers.
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_C, fixture.Drawer());
	CHECK(std::string_view(ImGui::GetClipboardText()).find("Strada.Log.Info") != std::string_view::npos);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.GetDocument().Graph.Comments.size() == comments);

	fixture.MoveMouse(fixture.EmptyPoint() + glm::vec2(300.0f, -200.0f));
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_V, fixture.Drawer());
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.GetDocument().Graph.Nodes.size() == nodes + 1);

	// Ctrl+F: find, and F (frame the selection) does not run.
	VisualScriptViewState const view = fixture.Model().GetViewState();
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_F, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().FindOpen);
	CHECK(fixture.Model().GetViewState() == view);
}

TEST_CASE("VisualScriptEditor: arrow keys nudge in runs of one step, and Alt nudges one unit")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Click(Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f)));
	glm::vec2 const start = *fixture.FindNode(3)->Position;
	size_t const undo = fixture.GetUndoCount();

	// Three presses 0.2 s apart: one step.
	for (int press = 0; press < 3; press++)
	{
		fixture.PressAfter(ImGuiKey_LeftArrow, 0.2f);
	}
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.FindNode(3)->Position->x == doctest::Approx(start.x - 48.0f));
	// 0.6 s later: a step of its own.
	fixture.PressAfter(ImGuiKey_LeftArrow, 0.6f);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.FindNode(3)->Position->x == doctest::Approx(start.x - 64.0f));
	fixture.PressAfter(ImGuiMod_Alt | ImGuiKey_LeftArrow, 0.6f);
	CHECK(fixture.GetUndoCount() == undo + 3);
	CHECK(fixture.FindNode(3)->Position->x == doctest::Approx(start.x - 65.0f));
}

TEST_CASE("VisualScriptEditor: H and M collapse and disable the selected nodes, and keys do nothing while a text field has the keyboard")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Click(Testing::CanvasPoint(fixture.Model(), 2, glm::vec2(0.5f, 0.06f)));
	size_t const undo = fixture.GetUndoCount();
	fixture.Gui.Press(ImGuiKey_H, fixture.Drawer());
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.FindNode(2)->Collapsed);
	fixture.Gui.Press(ImGuiKey_M, fixture.Drawer());
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.FindNode(2)->Enabled == VisualScriptNodeEnabled::Disabled);

	// "Add Node Comment..." opens the in-place editor, which has the keyboard: H types, and Escape adds nothing.
	std::string const bytes = fixture.ReadFile();
	fixture.Panel->RequestAction(Action::AddNodeComment);
	fixture.Frames(3);
	REQUIRE(ImGui::GetIO().WantTextInput);
	CHECK(fixture.Panel->HasPendingWidgetEdit() == false);
	fixture.Gui.Press(ImGuiKey_H, fixture.Drawer());
	fixture.Gui.Type("h");
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.FindNode(2)->Collapsed);
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(2);
	CHECK_FALSE(ImGui::GetIO().WantTextInput);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.ReadFile() == bytes);
	CHECK(fixture.FindNode(2)->Comment.empty());
}

TEST_CASE("VisualScriptEditor: a node comment typed in place is one step when it is done")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Click(Testing::CanvasPoint(fixture.Model(), 2, glm::vec2(0.5f, 0.06f)));
	size_t const undo = fixture.GetUndoCount();
	fixture.Panel->RequestAction(Action::AddNodeComment);
	fixture.Frames(3);
	fixture.Gui.Type("Wait first");
	fixture.Frames(1);
	CHECK(fixture.Panel->HasPendingWidgetEdit());
	CHECK(fixture.GetUndoCount() == undo);
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.FindNode(2)->Comment == "Wait first");
	CHECK_FALSE(fixture.Panel->HasPendingWidgetEdit());
}

TEST_CASE("VisualScriptEditor: C adds a comment box whose title, typed at once, is part of the same step")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Click(Testing::CanvasPoint(fixture.Model(), 2, glm::vec2(0.5f, 0.06f)));
	size_t const undo = fixture.GetUndoCount();
	size_t const comments = fixture.GetDocument().Graph.Comments.size();
	fixture.Gui.Press(ImGuiKey_C, fixture.Drawer());
	fixture.Frames(2);
	REQUIRE(fixture.GetDocument().Graph.Comments.size() == comments + 1);
	CHECK(fixture.GetUndoCount() == undo + 1);
	REQUIRE(ImGui::GetIO().WantTextInput);
	// "Comment" is selected: typing replaces it.
	fixture.Gui.Type("Waiting");
	fixture.Frames(1);
	fixture.Gui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.GetDocument().Graph.Comments.back().Text.starts_with("Waiting"));
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.GetDocument().Graph.Comments.size() == comments);
}

TEST_CASE("VisualScriptEditor: Tab and a Space tap open the palette without moving the focus, and Space with a key types its query")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Click(fixture.EmptyPoint());
	ImGuiWindow const* canvas = fixture.GetChild("##canvas");
	REQUIRE(GImGui->NavWindow == canvas);

	fixture.Gui.Press(ImGuiKey_Tab, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().PaletteOpen);
	CHECK(GImGui->NavWindow == canvas);

	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	CHECK_FALSE(fixture.Panel->GetState().PaletteOpen);
	fixture.Gui.Press(ImGuiKey_Space, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().PaletteOpen);
	CHECK(fixture.Panel->GetState().PaletteQuery.empty());

	// Space held, F typed: the query "f", and F's framing never runs.
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	VisualScriptViewState const view = fixture.Model().GetViewState();
	ImGuiIO& io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiKey_Space, true);
	fixture.Frames(1);
	io.AddKeyEvent(ImGuiKey_F, true);
	io.AddInputCharacter('f');
	fixture.Frames(1);
	io.AddKeyEvent(ImGuiKey_F, false);
	fixture.Frames(1);
	io.AddKeyEvent(ImGuiKey_Space, false);
	fixture.Frames(2);
	CHECK(fixture.Panel->GetState().PaletteOpen);
	CHECK(fixture.Panel->GetState().PaletteQuery == "f");
	CHECK(fixture.Model().GetViewState() == view);
}

TEST_CASE("VisualScriptEditor: the wheel zooms one level about the point under the cursor")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	glm::vec2 const point = Testing::CanvasPoint(fixture.Model(), 2, glm::vec2(0.3f, 0.7f));
	float const zoom = fixture.Model().GetViewState().Zoom;
	fixture.MoveMouse(point);
	glm::vec2 const under = fixture.Model().GetView().ToCanvas(point);
	fixture.Gui.Wheel(ToImVec2(point), ImVec2(0.0f, 1.0f), fixture.Drawer());
	fixture.Frames(1);
	float const zoomed = fixture.Model().GetViewState().Zoom;
	CHECK(zoomed > zoom);
	auto const next = std::upper_bound(VisualScriptZoomLevels.begin(), VisualScriptZoomLevels.end(), zoom + 0.001f);
	REQUIRE(next != VisualScriptZoomLevels.end());
	CHECK(zoomed == doctest::Approx(*next));
	glm::vec2 const still = fixture.Model().GetView().ToCanvas(point);
	CHECK(still.x == doctest::Approx(under.x).epsilon(0.001));
	CHECK(still.y == doctest::Approx(under.y).epsilon(0.001));
}

TEST_CASE("VisualScriptEditor: an asset dropped on a pin sets its literal and entities dropped on the canvas add their references, one "
          "step each")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(
		40, "Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)", glm::vec2(400.0f, 600.0f)));
	PanelFixture fixture(document);
	fixture.CheckDrawingChangesNothing();
	EntityCreateInfo info;
	info.Name = "Door";
	Result<UUID> const door = fixture.Operations.CreateEntity(info);
	REQUIRE(door.IsOk());
	Result<AssetHandle> const prefab = fixture.Operations.CreatePrefab(door.GetValue(), "Door.sprefab");
	REQUIRE(prefab.IsOk());
	size_t const undo = fixture.GetUndoCount();
	size_t const variables = fixture.GetDocument().Variables.size();

	// A source window dragging what the hierarchy (entities) or the content browser (an asset) drags.
	glm::vec2 const source(1500.0f, 860.0f);
	bool dragAsset = true;
	fixture.Probe = [&]
	{
		ImGui::SetNextWindowPos(ImVec2(source.x - 40.0f, source.y - 20.0f));
		ImGui::SetNextWindowSize(ImVec2(80.0f, 40.0f));
		ImGui::Begin("Source", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
		ImGui::Button("Door", ImVec2(60.0f, 20.0f));
		if (dragAsset)
		{
			UI::AssetDragSource(prefab.GetValue(), "Door.sprefab");
		}
		else if (ImGui::BeginDragDropSource())
		{
			uint64_t const value = door.GetValue().GetValue();
			ImGui::SetDragDropPayload(DragDropPayload::Entities, &value, sizeof(value));
			ImGui::EndDragDropSource();
		}
		ImGui::End();
	};
	fixture.Frames(2);
	ImVec2 const grab(source.x - 10.0f, source.y - 5.0f);

	// The prefab onto Instantiate's Prefab input: its literal, one step.
	fixture.Gui.DragPath(grab, ToImVec2(Testing::CanvasPoint(fixture.Model(), 40, "Prefab")), fixture.Drawer(), ImGuiMouseButton_Left, 8);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.FindNode(40)->Inputs.value("Prefab", Json()) == Json(prefab.GetValue().ToString()));

	// The entity onto the empty canvas: a public Entity variable named after it and its Get, one step.
	dragAsset = false;
	fixture.Frames(1);
	fixture.Gui.DragPath(grab, ToImVec2(fixture.EmptyPoint() + glm::vec2(400.0f, -300.0f)), fixture.Drawer(), ImGuiMouseButton_Left, 8);
	CHECK(fixture.GetUndoCount() == undo + 2);
	REQUIRE(fixture.GetDocument().Variables.size() == variables + 1);
	CHECK(fixture.GetDocument().Variables.back().Type == "Strada.Entity");
	CHECK(fixture.GetDocument().Variables.back().Name == "Door");
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.GetDocument().Variables.size() == variables);
}

TEST_CASE("VisualScriptEditor: a change made elsewhere during a drag cancels the drag")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();
	glm::vec2 const before = *fixture.FindNode(2)->Position;
	glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), 2, glm::vec2(0.5f, 0.06f));
	fixture.MoveMouse(from);
	fixture.SetButton(ImGuiMouseButton_Left, true);
	fixture.MoveMouse(from + glm::vec2(40.0f, 30.0f));
	fixture.MoveMouse(from + glm::vec2(80.0f, 60.0f));
	REQUIRE(fixture.Model().GetGesture().Kind == VisualScriptGesture::MovingElements);

	// An edit of another node made between frames (an agent's graph.set).
	VisualScriptDocument changed = fixture.GetDocument();
	changed.Graph.Nodes[2].Position = glm::vec2(800.0f, 160.0f);
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, changed).IsOk());
	fixture.MoveMouse(from + glm::vec2(120.0f, 90.0f));
	CHECK(fixture.Model().GetGesture().Kind == VisualScriptGesture::Idle);
	fixture.SetButton(ImGuiMouseButton_Left, false);
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(*fixture.FindNode(2)->Position == before);
	CHECK(*fixture.FindNode(3)->Position == glm::vec2(800.0f, 160.0f));
}

TEST_CASE("VisualScriptEditor: without the scripting runtime nodes still move and delete, and wires and the palette are refused")
{
	PanelFixture fixture(MakeDocument());
	fixture.Semantics.SetAvailable(false);
	fixture.CheckDrawingChangesNothing();
	size_t const undo = fixture.GetUndoCount();

	glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f));
	fixture.Gui.DragPath(ToImVec2(from), ToImVec2(from + glm::vec2(64.0f, 64.0f)), fixture.Drawer());
	CHECK(fixture.GetUndoCount() == undo + 1);

	uint64_t const refused = fixture.Model().GetRefusedCount();
	fixture.Gui.Press(ImGuiKey_Tab, fixture.Drawer());
	fixture.Frames(1);
	CHECK_FALSE(fixture.Panel->GetState().PaletteOpen);
	CHECK(fixture.Model().GetRefusedCount() > refused);

	fixture.Click(Testing::CanvasPoint(fixture.Model(), 6, glm::vec2(0.5f, 0.5f)));
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.FindNode(6) == nullptr);
}

TEST_CASE("VisualScriptEditor: a visual script without a file is shown read only: views and selections change, nothing is written")
{
	PanelFixture fixture(MakeDocument());
	AssetHandle const memory = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeDocument()), "Generated");
	fixture.Panel.reset();
	fixture.Panel.emplace(memory, fixture.Operations, fixture.Semantics, fixture.Settings, fixture.Session, fixture.Services);
	fixture.Frames(3);
	REQUIRE(fixture.Model().IsReadOnly());
	size_t const undo = fixture.GetUndoCount();

	glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f));
	fixture.Gui.DragPath(ToImVec2(from), ToImVec2(from + glm::vec2(64.0f, 64.0f)), fixture.Drawer());
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(std::vector<uint32_t>(fixture.Model().GetSelection().begin(), fixture.Model().GetSelection().end()) == std::vector<uint32_t>{3});
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	fixture.Gui.Press(ImGuiKey_H, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(AssetManager::GetAsset<VisualScriptAsset>(memory)->GetVersion() == 0);
	CHECK(fixture.Open);
}
