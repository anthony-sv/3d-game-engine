#include "HeadlessImGui.h"
#include "TestUtilities.h"
#include "VisualScriptPanelTestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/Commands/EntityCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/Panels/VisualScriptEditorPanel.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptShortcuts.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
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
	using Item = VisualScriptPanelItem;
	using Testing::PanelFixture;
	using Testing::ToImVec2;

	VisualScriptNode MakeNode(uint32_t id, std::string type, std::optional<glm::vec2> position, Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = position;
		node.Inputs = std::move(inputs);
		return node;
	}

	// The script "Player": Create (1) -> Delay (2), a Log Info (3) and an Add (6).
	VisualScriptDocument MakeDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes = {
			MakeNode(1, "Event.Create", glm::vec2(0.0f, 0.0f)),
			MakeNode(2, "Flow.Delay", glm::vec2(400.0f, 0.0f), Json::object({{"Duration", 1.5}})),
			MakeNode(3, "Call:Strada.Log.Info(string)", glm::vec2(800.0f, 0.0f), Json::object({{"Message", "Hi"}})),
			MakeNode(6, "Operator.Add", glm::vec2(400.0f, 320.0f)),
		};
		document.Graph.Links = {{{1, "Then"}, {2, "Execute"}}};
		return document;
	}

	bool HasNotice(VisualScriptEditorState const& state, VisualScriptNotice notice)
	{
		return std::find(state.Notices.begin(), state.Notices.end(), notice) != state.Notices.end();
	}

	bool HasToast(VisualScriptEditorState const& state, std::string_view text)
	{
		return std::any_of(state.Toasts.begin(), state.Toasts.end(),
		                   [text](std::string const& toast)
		                   {
							   return toast.find(text) != std::string::npos;
						   });
	}

	// The window's font size, as its items are measured: the style's base size and both scales.
	float GetFontSize()
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		return style.FontSizeBase * style.FontScaleMain * style.FontScaleDpi;
	}

	// An entity of the edited scene whose VisualScriptComponent runs the graph, made as the hierarchy makes one.
	UUID CreateRunner(PanelFixture& fixture, std::string name)
	{
		EntityCreateInfo info;
		info.Name = std::move(name);
		info.Components = Json::object({{"VisualScript", Json::object({{"Graph", fixture.Graph.ToString()}})}});
		Result<UUID> const entity = fixture.Operations.CreateEntity(info);
		REQUIRE_MESSAGE(entity.IsOk(), (entity ? std::string() : entity.GetError()));
		return entity.GetValue();
	}

	// A button of a toast, found by moving the mouse along the toast's middle (toasts come and go, so their buttons are not
	// among the window's located items).
	std::optional<glm::vec2> FindToastButton(PanelFixture& fixture, char const* label)
	{
		std::vector<ImGuiWindow*> toasts;
		ImGuiWindow const* canvas = fixture.GetChild("##canvas");
		for (ImGuiWindow* window : GImGui->Windows)
		{
			if (window->ParentWindow == canvas && window->Active && std::strstr(window->Name, "##toast") != nullptr)
			{
				toasts.push_back(window);
			}
		}
		for (ImGuiWindow const* toast : toasts)
		{
			ImGuiID const item = ImHashStr(label, 0, toast->ID);
			float const y = toast->Pos.y + toast->Size.y * 0.5f;
			for (float x = toast->Pos.x + 2.0f; x < toast->Pos.x + toast->Size.x; x += 3.0f)
			{
				fixture.MoveMouse(glm::vec2(x, y));
				if (GImGui->HoveredId == item)
				{
					return glm::vec2(x, y);
				}
			}
		}
		return std::nullopt;
	}
}

TEST_CASE("VisualScriptEditor: the toolbar fits windows of 784 and 1102 pixels at UI scales 1 and 1.25, moving what does not fit "
          "into the view menu")
{
	for (float const scale : {1.0f, 1.25f})
	{
		for (bool const withInstance : {false, true})
		{
			CAPTURE(scale);
			CAPTURE(withInstance);
			PanelFixture fixture(MakeDocument());
			if (withInstance)
			{
				CreateRunner(fixture, "Door");
			}
			if (scale != 1.0f)
			{
				fixture.SetUIScale(scale);
			}
			for (float const width : {784.0f, 1102.0f})
			{
				CAPTURE(width);
				fixture.WindowSize = ImVec2(width, 700.0f);
				fixture.Frames(3);
				fixture.CheckDrawingChangesNothing();
				VisualScriptEditorState const state = fixture.Panel->GetState();
				ImGuiWindow const* window = fixture.GetWindow();
				REQUIRE(window != nullptr);
				ImRect const inner = window->InnerRect;

				// Every item drawn lies inside the window, and none covers another.
				std::vector<std::pair<float, float>> spans;
				for (size_t index = 0; index < VisualScriptToolbarItemCount; index++)
				{
					std::optional<VisualScriptRect> const rect = fixture.Panel->GetItemRect(static_cast<Item>(index));
					if (!rect)
					{
						continue;
					}
					CAPTURE(index);
					CHECK(rect->Min.x >= inner.Min.x - 0.5f);
					CHECK(rect->Max.x <= inner.Max.x + 0.5f);
					CHECK(rect->Min.y >= inner.Min.y - 0.5f);
					CHECK(rect->Max.y <= inner.Max.y + 0.5f);
					spans.emplace_back(rect->Min.x, rect->Max.x);
				}
				std::sort(spans.begin(), spans.end());
				for (size_t index = 1; index < spans.size(); index++)
				{
					CHECK(spans[index - 1].second <= spans[index].first + 0.5f);
				}

				// What never hides is drawn; what the overflow moves is drawn or in the view menu.
				for (Item const item : {Item::SidebarToggle, Item::Back, Item::Forward, Item::Breadcrumb, Item::Problems, Item::SaveStatus,
				                        Item::Arrange, Item::Find, Item::ViewMenu, Item::Shortcuts, Item::DetailsToggle})
				{
					CAPTURE(static_cast<int>(item));
					CHECK(fixture.Panel->GetItemRect(item).has_value());
				}
				CHECK(fixture.Panel->GetItemRect(Item::Instance).has_value() == withInstance);
				CHECK_FALSE(fixture.Panel->GetItemRect(Item::Live).has_value());
				auto const hidden = [&state](Action action)
				{
					return std::find(state.HiddenToolbarActions.begin(), state.HiddenToolbarActions.end(), action) !=
					       state.HiddenToolbarActions.end();
				};
				CHECK((fixture.Panel->GetItemRect(Item::SnapToGrid).has_value() || hidden(Action::ToggleSnapToGrid)));
				CHECK((fixture.Panel->GetItemRect(Item::Minimap).has_value() || hidden(Action::ToggleMinimap)));
				CHECK((fixture.Panel->GetItemRect(Item::Zoom).has_value() || hidden(Action::ZoomReset)));
				// The Bookmarks menu in the view menu offers the first free slot.
				CHECK((fixture.Panel->GetItemRect(Item::Bookmarks).has_value() || hidden(Action::StoreBookmark1)));

				// The canvas keeps 40 font sizes, or a pane gives way.
				ImGuiWindow const* canvas = fixture.GetChild("##canvas");
				CHECK((canvas->Size.x >= VisualScriptCanvasMinWidth * GetFontSize() - 0.5f ||
				       state.Details == VisualScriptPaneState::AutoHidden || state.Sidebar == VisualScriptPaneState::AutoHidden));

				float const frame = fixture.ItemRect(Item::ViewMenu).GetSize().x;
				if (width == 1102.0f && scale == 1.0f && !withInstance)
				{
					// Wide enough for everything as text.
					CHECK(state.HiddenToolbarActions.empty());
					CHECK(fixture.ItemRect(Item::Arrange).GetSize().x > frame + 1.0f);
				}
				if (width == 784.0f && scale == 1.25f)
				{
					// The text buttons became glyphs, and the view items moved.
					CHECK(fixture.ItemRect(Item::Arrange).GetSize().x == doctest::Approx(frame));
					CHECK_FALSE(state.HiddenToolbarActions.empty());
				}
			}
		}
	}
}

TEST_CASE("VisualScriptEditor: an auto-hidden Details opens over the canvas from its toggle and from a double-click on a node "
          "header, and Escape closes it")
{
	PanelFixture fixture(MakeDocument());
	fixture.WindowSize = ImVec2(784.0f, 700.0f);
	fixture.Frames(3);
	fixture.CheckDrawingChangesNothing();
	REQUIRE(fixture.Panel->GetState().Details == VisualScriptPaneState::AutoHidden);

	fixture.ClickItem(Item::DetailsToggle);
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().Details == VisualScriptPaneState::Overlay);
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().Details == VisualScriptPaneState::AutoHidden);

	glm::vec2 const header = Testing::CanvasPoint(fixture.Model(), 2, glm::vec2(0.5f, 0.06f));
	fixture.Gui.MouseClick(ToImVec2(header), ImGuiMouseButton_Left, fixture.Drawer(), 2);
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().Details == VisualScriptPaneState::Overlay);
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().Details == VisualScriptPaneState::AutoHidden);

	// With the keyboard in the overlay itself (a click on its empty space), one Escape closes it and gives the keyboard back to
	// the canvas.
	fixture.ClickItem(Item::DetailsToggle);
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetState().Details == VisualScriptPaneState::Overlay);
	ImGuiWindow const* overlay = fixture.GetChild("##detailsOverlay");
	fixture.Click(glm::vec2(overlay->Pos.x + overlay->Size.x * 0.5f, overlay->Pos.y + overlay->Size.y - 8.0f));
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetFocusedPart() == VisualScriptWindowPart::Details);
	REQUIRE(fixture.Panel->GetState().Details == VisualScriptPaneState::Overlay);
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().Details == VisualScriptPaneState::AutoHidden);
	CHECK(fixture.Panel->GetFocusedPart() == VisualScriptWindowPart::Canvas);
}

TEST_CASE("VisualScriptEditor: the panes shown over a narrow canvas list and edit what they list beside it")
{
	VisualScriptDocument document = MakeDocument();
	VisualScriptVariable speed;
	speed.ID = 100;
	speed.Name = "Speed";
	speed.Type = "float";
	document.Variables.push_back(std::move(speed));
	PanelFixture fixture(document);
	// Too narrow for either pane beside the canvas.
	fixture.WindowSize = ImVec2(560.0f, 700.0f);
	fixture.Frames(3);
	fixture.CheckDrawingChangesNothing();
	REQUIRE(fixture.Panel->GetState().Sidebar == VisualScriptPaneState::AutoHidden);
	REQUIRE(fixture.Panel->GetState().Details == VisualScriptPaneState::AutoHidden);
	auto const inside = [](std::pair<glm::vec2, glm::vec2> const& row, ImGuiWindow const* window)
	{
		return row.first.x >= window->Pos.x && row.first.y >= window->Pos.y && row.second.x <= window->Pos.x + window->Size.x &&
		       row.second.y <= window->Pos.y + window->Size.y;
	};

	// My Script over the canvas lists the variable; a click on its row selects it (Details, hidden, would show it).
	fixture.ClickItem(Item::SidebarToggle);
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetState().Sidebar == VisualScriptPaneState::Overlay);
	std::optional<std::pair<glm::vec2, glm::vec2>> const row =
		fixture.Panel->GetSidebar().FindRow({VisualScriptElementKind::Variable, 100});
	REQUIRE(row.has_value());
	CHECK(inside(*row, fixture.GetChild("##sidebarOverlay")));
	fixture.Click((row->first + row->second) * 0.5f);
	fixture.Frames(1);
	std::optional<VisualScriptElement> const selected = fixture.Panel->GetPaneShared().Element;
	REQUIRE(selected.has_value());
	CHECK(selected->Kind == VisualScriptElementKind::Variable);
	CHECK(selected->ID == 100);
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(1);

	// Details over the canvas shows a node's rows, and edits it: its Enabled state, in one step.
	glm::vec2 const header = Testing::CanvasPoint(fixture.Model(), 2, glm::vec2(0.5f, 0.06f));
	fixture.Gui.MouseClick(ToImVec2(header), ImGuiMouseButton_Left, fixture.Drawer(), 2);
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetState().Details == VisualScriptPaneState::Overlay);
	REQUIRE(fixture.Panel->GetDetails().GetSubject() == VisualScriptDetailsSubject::Node);
	std::optional<std::pair<glm::vec2, glm::vec2>> const enabled = fixture.Panel->GetDetails().FindRow("Enabled");
	REQUIRE(enabled.has_value());
	CHECK(inside(*enabled, fixture.GetChild("##detailsOverlay")));
	size_t const undo = fixture.GetUndoCount();
	fixture.Click((enabled->first + enabled->second) * 0.5f);
	fixture.Frames(2);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	fixture.Gui.Press(ImGuiKey_DownArrow, fixture.Drawer());
	fixture.Gui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	REQUIRE(fixture.FindNode(2) != nullptr);
	CHECK(fixture.FindNode(2)->Enabled == VisualScriptNodeEnabled::Disabled);
	CHECK(fixture.Panel->GetState().Details == VisualScriptPaneState::Overlay);
}

TEST_CASE("VisualScriptEditor: panes stand beside the canvas, else stack in one column, else auto-hide")
{
	VisualScriptPaneSettings const panes;
	REQUIRE(panes.Sidebar == 16.0f);
	REQUIRE(panes.Details == 20.0f);

	SUBCASE("1366 px at UI scale 1: too narrow for a column, both auto-hidden")
	{
		VisualScriptWindowLayout const layout = ComputeVisualScriptWindowLayout(768.0f, 15.0f, 4.0f, panes);
		CHECK_FALSE(layout.Stacked);
		CHECK(layout.SidebarState == VisualScriptPaneState::AutoHidden);
		CHECK(layout.DetailsState == VisualScriptPaneState::AutoHidden);
		CHECK(layout.Canvas == doctest::Approx(768.0f));
	}
	SUBCASE("1920 px at UI scale 1: stacked in a column as wide as Details")
	{
		VisualScriptWindowLayout const layout = ComputeVisualScriptWindowLayout(1086.0f, 15.0f, 4.0f, panes);
		CHECK(layout.Stacked);
		CHECK(layout.SidebarState == VisualScriptPaneState::Shown);
		CHECK(layout.DetailsState == VisualScriptPaneState::Shown);
		CHECK(layout.Column == doctest::Approx(300.0f));
		CHECK(layout.Canvas == doctest::Approx(782.0f));
		CHECK(layout.Sidebar == 0.0f);
		CHECK(layout.Details == 0.0f);
	}
	SUBCASE("1920 px at UI scale 1.25: stacked in the column the canvas's minimum leaves")
	{
		VisualScriptWindowLayout const layout = ComputeVisualScriptWindowLayout(1082.0f, 18.75f, 5.0f, panes);
		CHECK(layout.Stacked);
		CHECK(layout.Column == doctest::Approx(327.0f));
		CHECK(layout.Canvas == doctest::Approx(750.0f));
		CHECK(layout.Canvas >= VisualScriptCanvasMinWidth * 18.75f - 0.01f);
	}
	SUBCASE("2560 px at UI scale 1: side by side")
	{
		VisualScriptWindowLayout const layout = ComputeVisualScriptWindowLayout(1454.0f, 15.0f, 4.0f, panes);
		CHECK_FALSE(layout.Stacked);
		CHECK(layout.Sidebar == doctest::Approx(240.0f));
		CHECK(layout.Details == doctest::Approx(300.0f));
		CHECK(layout.Canvas == doctest::Approx(906.0f));
	}
	SUBCASE("one pane on: beside the canvas or auto-hidden, never stacked")
	{
		VisualScriptPaneSettings details = panes;
		details.ShowSidebar = false;
		VisualScriptWindowLayout layout = ComputeVisualScriptWindowLayout(1086.0f, 15.0f, 4.0f, details);
		CHECK_FALSE(layout.Stacked);
		CHECK(layout.SidebarState == VisualScriptPaneState::Hidden);
		CHECK(layout.Details == doctest::Approx(300.0f));
		layout = ComputeVisualScriptWindowLayout(768.0f, 15.0f, 4.0f, details);
		CHECK_FALSE(layout.Stacked);
		CHECK(layout.DetailsState == VisualScriptPaneState::AutoHidden);
	}
}

TEST_CASE("VisualScriptEditor: a window 1086 pixels wide shows My Script above Details beside a canvas of 40 font sizes, and "
          "resizing switches the arrangement without changing the user's toggles")
{
	VisualScriptDocument document = MakeDocument();
	VisualScriptVariable speed;
	speed.ID = 100;
	speed.Name = "Speed";
	speed.Type = "float";
	document.Variables.push_back(std::move(speed));
	PanelFixture fixture(document);
	fixture.CheckDrawingChangesNothing();
	VisualScriptPaneSettings const panes = fixture.Settings.GetPanes();
	auto const check = [&](float width, bool stacked, VisualScriptPaneState state)
	{
		CAPTURE(width);
		fixture.WindowSize = ImVec2(width, 700.0f);
		fixture.Frames(2);
		VisualScriptEditorState const editor = fixture.Panel->GetState();
		CHECK(editor.PanesStacked == stacked);
		CHECK(editor.Sidebar == state);
		CHECK(editor.Details == state);
		CHECK(fixture.Settings.GetPanes() == panes);
	};
	check(1600.0f, false, VisualScriptPaneState::Shown);
	check(1086.0f, true, VisualScriptPaneState::Shown);
	check(784.0f, false, VisualScriptPaneState::AutoHidden);
	check(1086.0f, true, VisualScriptPaneState::Shown);
	check(1600.0f, false, VisualScriptPaneState::Shown);
	check(1086.0f, true, VisualScriptPaneState::Shown);
	fixture.CheckDrawingChangesNothing();

	// One column left of the canvas: My Script on top, Details below it, the canvas keeping its 40 font sizes.
	ImGuiWindow const* sidebar = fixture.GetChild("##sidebar");
	ImGuiWindow const* details = fixture.GetChild("##details");
	ImGuiWindow const* canvas = fixture.GetChild("##canvas");
	CHECK(canvas->Size.x >= VisualScriptCanvasMinWidth * GetFontSize() - 0.5f);
	CHECK(sidebar->Pos.x == doctest::Approx(details->Pos.x));
	CHECK(sidebar->Size.x == doctest::Approx(details->Size.x));
	CHECK(sidebar->Pos.y + sidebar->Size.y < details->Pos.y);
	CHECK(details->Pos.x + details->Size.x < canvas->Pos.x);
	CHECK(details->Pos.y + details->Size.y == doctest::Approx(canvas->Pos.y + canvas->Size.y));
	CHECK(sidebar->Size.y / (details->Pos.y + details->Size.y - sidebar->Pos.y) == doctest::Approx(VisualScriptStackShare).epsilon(0.02));

	// The splitter between them moves the share for this window, and stores nothing.
	ImVec2 const splitter(sidebar->Pos.x + sidebar->Size.x * 0.5f, (sidebar->Pos.y + sidebar->Size.y + details->Pos.y) * 0.5f);
	float const top = sidebar->Size.y;
	fixture.Gui.DragPath(splitter, ImVec2(splitter.x, splitter.y + 60.0f), fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.GetChild("##sidebar")->Size.y == doctest::Approx(top + 60.0f).epsilon(0.02));
	CHECK(fixture.Settings.GetPanes() == panes);
	fixture.Gui.DragPath(ImVec2(splitter.x, splitter.y + 60.0f), splitter, fixture.Drawer());
	fixture.Frames(1);

	// The column's edge is no splitter: dragging it either way changes neither the column, the arrangement nor the stored
	// pane widths every graph window reads.
	sidebar = fixture.GetChild("##sidebar");
	canvas = fixture.GetChild("##canvas");
	float const column = sidebar->Size.x;
	float const canvasOffset = canvas->Pos.x - sidebar->Pos.x;
	ImVec2 const edge((sidebar->Pos.x + sidebar->Size.x + canvas->Pos.x) * 0.5f, canvas->Pos.y + canvas->Size.y * 0.5f);
	for (float const distance : {-80.0f, 1.0f, 80.0f})
	{
		CAPTURE(distance);
		fixture.Gui.DragPath(edge, ImVec2(edge.x + distance, edge.y), fixture.Drawer());
		fixture.Frames(1);
		CHECK(fixture.Panel->GetState().PanesStacked);
		CHECK(fixture.GetChild("##sidebar")->Size.x == doctest::Approx(column));
		CHECK(fixture.GetChild("##canvas")->Pos.x - fixture.GetChild("##sidebar")->Pos.x == doctest::Approx(canvasOffset));
		CHECK(fixture.Settings.GetPanes() == panes);
	}

	// Row keys reach both panes of the column: Delete in My Script deletes its variable (no node uses it, so it does not ask),
	// Delete in Details deletes the node it shows.
	size_t const undo = fixture.GetUndoCount();
	std::optional<std::pair<glm::vec2, glm::vec2>> const row =
		fixture.Panel->GetSidebar().FindRow({VisualScriptElementKind::Variable, 100});
	REQUIRE(row.has_value());
	fixture.Click((row->first + row->second) * 0.5f);
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetFocusedPart() == VisualScriptWindowPart::Sidebar);
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.GetDocument().Variables.empty());

	fixture.Click(Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f)));
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetDetails().GetSubject() == VisualScriptDetailsSubject::Node);
	details = fixture.GetChild("##details");
	fixture.Click(glm::vec2(details->Pos.x + details->Size.x * 0.5f, details->Pos.y + details->Size.y - 8.0f));
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetFocusedPart() == VisualScriptWindowPart::Details);
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.FindNode(3) == nullptr);
	CHECK(fixture.FindNode(2) != nullptr);
	CHECK(fixture.Panel->GetState().PanesStacked);
}

TEST_CASE("VisualScriptEditor: a comment's title field opens from the canvas's left edge when the comment's box starts left of it")
{
	VisualScriptDocument document = MakeDocument();
	VisualScriptComment region;
	region.ID = 40;
	region.Text = "Maths";
	region.Position = glm::vec2(336.0f, 256.0f);
	region.Size = glm::vec2(480.0f, 288.0f);
	document.Graph.Comments.push_back(region);
	PanelFixture fixture(document);
	fixture.CheckDrawingChangesNothing();
	VisualScriptView const& view = fixture.Model().GetView();
	glm::vec2 const canvasMin = view.GetCanvasMin();
	glm::vec2 const canvasSize = view.GetCanvasSize();
	auto const findVisual = [&fixture]
	{
		VisualScriptCanvasModel const& canvas = fixture.Model().GetCanvas();
		std::optional<uint32_t> const index = canvas.FindComment(40);
		REQUIRE(index.has_value());
		return canvas.GetComments()[*index];
	};
	auto const boxLeft = [&]
	{
		return view.ToScreen(findVisual().Rect.Min).x;
	};
	// Pans the view left with the middle button, from the bottom middle of the canvas (nothing there), until the box starts
	// 100 pixels left of the canvas.
	for (int drag = 0; drag < 10 && boxLeft() > canvasMin.x - 100.0f; drag++)
	{
		glm::vec2 const from(canvasMin.x + canvasSize.x * 0.5f, canvasMin.y + canvasSize.y - 30.0f);
		float const distance = std::min(boxLeft() - canvasMin.x + 100.0f, canvasSize.x * 0.4f);
		fixture.Gui.DragPath(ToImVec2(from), ToImVec2(from - glm::vec2(distance, 0.0f)), fixture.Drawer(), ImGuiMouseButton_Middle);
		fixture.Frames(1);
	}
	REQUIRE(boxLeft() < canvasMin.x - 50.0f);
	VisualScriptCommentVisual const visual = findVisual();
	REQUIRE(view.ToScreen(visual.Rect.Max).x > canvasMin.x + 200.0f);

	// A double-click on the title where the canvas shows it opens the field there, from the canvas's left edge.
	VisualScriptRect const bar = VisualScriptGeometry::GetCommentBarRect(visual, view.GetPixelsPerUnit(), view.GetUIScale());
	glm::vec2 const title(canvasMin.x + 40.0f, view.ToScreen(bar.GetCenter()).y);
	fixture.Gui.MouseClick(ToImVec2(title), ImGuiMouseButton_Left, fixture.Drawer(), 2);
	fixture.Frames(2);
	REQUIRE(ImGui::GetIO().WantTextInput);
	// The field has ImGui's keyboard: its rectangle is the navigation's, relative to its window's first cursor position
	// (imgui.cpp:14347, imgui_internal.h:3602).
	ImGuiWindow const* window = GImGui->NavWindow;
	REQUIRE(window != nullptr);
	REQUIRE(GImGui->NavId == GImGui->ActiveId);
	ImRect const field = window->NavRectRel[GImGui->NavLayer];
	CHECK(field.Min.x + window->DC.CursorStartPos.x == doctest::Approx(canvasMin.x).epsilon(0.001));
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(1);
	CHECK_FALSE(ImGui::GetIO().WantTextInput);
}

TEST_CASE("VisualScriptEditor: without the scripting runtime the degraded notice shows, offers the console and blinks when a "
          "gesture needs the runtime")
{
	PanelFixture fixture(MakeDocument());
	fixture.Semantics.SetAvailable(false);
	fixture.Frames(2);
	fixture.CheckDrawingChangesNothing();
	VisualScriptEditorState state = fixture.Panel->GetState();
	REQUIRE(HasNotice(state, VisualScriptNotice::Degraded));
	CHECK(state.Notices.front() == VisualScriptNotice::Degraded);
	// [Show Console] needs the editor's callback.
	CHECK_FALSE(fixture.Panel->GetItemRect(Item::ShowConsole).has_value());
	int shown = 0;
	fixture.Services.ShowConsole = [&shown]
	{
		shown++;
	};
	fixture.Frames(2);
	fixture.ClickItem(Item::ShowConsole);
	CHECK(shown == 1);

	uint64_t const blinks = fixture.Panel->GetActivity().DegradedBlinks;
	fixture.Click(fixture.EmptyPoint());
	fixture.Gui.Press(ImGuiKey_Tab, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetActivity().DegradedBlinks == blinks + 1);

	// The runtime comes back: the notice goes.
	fixture.Semantics.SetAvailable(true);
	fixture.Frames(2);
	CHECK_FALSE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::Degraded));
}

TEST_CASE("VisualScriptEditor: nodes without positions are announced, and Update Nodes writes them in one step")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(7, "Flow.Delay", std::nullopt));
	document.Graph.Nodes.push_back(MakeNode(8, "Flow.Delay", std::nullopt));
	PanelFixture fixture(document);
	fixture.CheckDrawingChangesNothing();
	REQUIRE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::Normalization));
	size_t const undo = fixture.GetUndoCount();

	fixture.ClickItem(Item::UpdateNodes);
	REQUIRE(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Update Nodes in Player");
	CHECK(fixture.FindNode(7)->Position.has_value());
	CHECK(fixture.FindNode(8)->Position.has_value());
	fixture.Frames(1);
	CHECK_FALSE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::Normalization));
}

TEST_CASE("VisualScriptEditor: a file repaired while loading says so until dismissed, and memory assets are shown read only")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	CHECK_FALSE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::Repaired));

	// A second node with the first one's ID and a key the format does not have.
	Json damaged = VisualScriptSerializer::Serialize(MakeDocument());
	damaged["VisualScript"]["Graph"]["Nodes"].push_back(damaged["VisualScript"]["Graph"]["Nodes"][0]);
	damaged["Comment"] = "written by hand";
	std::filesystem::path const file = AssetManager::GetAssetDirectory() / "Scripts" / "Damaged.sgraph";
	REQUIRE(FileSystem::WriteTextFile(file, DumpJson(damaged)).IsOk());
	REQUIRE(fixture.Operations.RefreshAssets().IsOk());
	AssetHandle const graph = AssetManager::FindByPath("Scripts/Damaged.sgraph");
	REQUIRE(graph.IsValid());
	fixture.Reopen(graph);
	VisualScriptEditorState state = fixture.Panel->GetState();
	REQUIRE(HasNotice(state, VisualScriptNotice::Repaired));
	CHECK(AssetManager::GetAsset<VisualScriptAsset>(graph)->GetLoadRepairCount() >= 2);
	fixture.ClickItem(Item::DismissRepaired);
	fixture.Frames(1);
	CHECK_FALSE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::Repaired));

	AssetHandle const memory = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeDocument()), "Generated");
	fixture.Reopen(memory);
	state = fixture.Panel->GetState();
	CHECK(state.Document == VisualScriptDocumentState::ReadOnly);
	CHECK(HasNotice(state, VisualScriptNotice::ReadOnlyMemory));
	CHECK_FALSE(HasNotice(state, VisualScriptNotice::ReadOnlyFile));
}

TEST_CASE("VisualScriptEditor: a file without write permission opens read only, and Retry makes it editable once it can be "
          "written")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	Testing::ReadOnlyFileScope readOnly(fixture.File);
	fixture.Reopen(fixture.Graph);
	VisualScriptEditorState state = fixture.Panel->GetState();
	REQUIRE(state.Document == VisualScriptDocumentState::ReadOnlyFile);
	CHECK(HasNotice(state, VisualScriptNotice::ReadOnlyFile));
	size_t const undo = fixture.GetUndoCount();

	// Nothing edits: a drag selects and moves nothing.
	glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f));
	fixture.Gui.DragPath(ToImVec2(from), ToImVec2(from + glm::vec2(64.0f, 64.0f)), fixture.Drawer());
	CHECK(fixture.GetUndoCount() == undo);

	// Still read only: Retry finds it so.
	fixture.ClickItem(Item::Retry);
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().Document == VisualScriptDocumentState::ReadOnlyFile);
	readOnly.Restore();
	fixture.ClickItem(Item::Retry);
	fixture.Frames(1);
	state = fixture.Panel->GetState();
	CHECK(state.Document == VisualScriptDocumentState::Editable);
	CHECK_FALSE(HasNotice(state, VisualScriptNotice::ReadOnlyFile));
	glm::vec2 const again = Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f));
	fixture.Gui.DragPath(ToImVec2(again), ToImVec2(again + glm::vec2(64.0f, 64.0f)), fixture.Drawer());
	CHECK(fixture.GetUndoCount() == undo + 1);
}

TEST_CASE("VisualScriptEditor: a commit whose file cannot be written shows the error until dismissed and Not saved, and "
          "changes nothing")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	CHECK(fixture.Panel->GetState().Save == VisualScriptSaveState::Saved);
	// Made read-only after the window opened, so the window still takes it for writable.
	Testing::ReadOnlyFileScope readOnly(fixture.File);
	std::string const bytes = fixture.ReadFile();
	VisualScriptDocument const document = fixture.GetDocument();
	size_t const undo = fixture.GetUndoCount();

	glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f));
	fixture.Gui.DragPath(ToImVec2(from), ToImVec2(from + glm::vec2(64.0f, 64.0f)), fixture.Drawer());
	fixture.Frames(1);
	VisualScriptEditorState const state = fixture.Panel->GetState();
	CHECK(state.Save == VisualScriptSaveState::NotSaved);
	CHECK(HasToast(state, "Could not save Player: "));
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);
	CHECK(fixture.GetDocument() == document);
	// The failure stays; the window now knows the file is read-only.
	fixture.Frames(60, 1.0f / 10.0f);
	CHECK(HasToast(fixture.Panel->GetState(), "Could not save Player: "));
	CHECK(fixture.Panel->GetState().Document == VisualScriptDocumentState::ReadOnlyFile);

	// Once it can be written, the next edit saves and the status says so.
	readOnly.Restore();
	fixture.ClickItem(Item::Retry);
	fixture.Frames(1);
	glm::vec2 const again = Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f));
	fixture.Gui.DragPath(ToImVec2(again), ToImVec2(again + glm::vec2(64.0f, 64.0f)), fixture.Drawer());
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Panel->GetState().Save == VisualScriptSaveState::Saved);
}

TEST_CASE("VisualScriptEditor: a missing file and a file that cannot be read show cards until a refresh or graph.set brings the "
          "graph back")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	CHECK(fixture.Panel->GetState().Card == VisualScriptCard::None);

	// Missing: the card says where the file was; it goes when a refresh finds the file again.
	Result<std::string> const text = FileSystem::ReadTextFile(fixture.File);
	REQUIRE(text.IsOk());
	std::filesystem::remove(fixture.File);
	REQUIRE(fixture.Operations.RefreshAssets().IsOk());
	fixture.Frames(2);
	CHECK(fixture.Panel->GetState().Card == VisualScriptCard::Missing);
	CHECK(fixture.Panel->GetItemRect(Item::CloseCard).has_value());
	REQUIRE(FileSystem::WriteTextFile(fixture.File, text.GetValue()).IsOk());
	REQUIRE(fixture.Operations.RefreshAssets().IsOk());
	fixture.Frames(2);
	CHECK(fixture.Panel->GetState().Card == VisualScriptCard::None);
	CHECK(fixture.Panel->GetState().Document == VisualScriptDocumentState::Editable);

	// Refused: the card names the error; graph.set writes a document the window then shows.
	REQUIRE(FileSystem::WriteTextFile(fixture.File, "not a visual script").IsOk());
	std::filesystem::last_write_time(fixture.File, std::filesystem::file_time_type::clock::now() + std::chrono::hours(1));
	REQUIRE(fixture.Operations.RefreshAssets().IsOk());
	fixture.Frames(2);
	CHECK(fixture.Panel->GetState().Card == VisualScriptCard::Unloadable);
	CHECK(fixture.Panel->GetState().Document == VisualScriptDocumentState::Unloadable);
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, MakeDocument()).IsOk());
	fixture.Frames(2);
	CHECK(fixture.Panel->GetState().Card == VisualScriptCard::None);
	CHECK(fixture.Panel->GetState().Document == VisualScriptDocumentState::Editable);
	CHECK(fixture.Model().GetCanvas().FindNode(3).has_value());

	// [Close] closes the window.
	std::filesystem::remove(fixture.File);
	REQUIRE(fixture.Operations.RefreshAssets().IsOk());
	fixture.Frames(2);
	// The card sizes itself to its text, then centres itself: a frame each.
	fixture.Frames(2);
	fixture.ClickItem(Item::CloseCard);
	CHECK_FALSE(fixture.Open);
}

TEST_CASE("VisualScriptEditor: an empty event graph shows a card whose buttons add an event in one step each")
{
	PanelFixture fixture(VisualScriptDocument{});
	fixture.CheckDrawingChangesNothing();
	REQUIRE(fixture.Panel->GetState().Card == VisualScriptCard::EmptyGraph);
	size_t const undo = fixture.GetUndoCount();

	fixture.ClickItem(Item::AddCreateEvent);
	REQUIRE(fixture.GetUndoCount() == undo + 1);
	REQUIRE(fixture.GetDocument().Graph.Nodes.size() == 1);
	CHECK(fixture.GetDocument().Graph.Nodes[0].Type == "Event.Create");
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().Card == VisualScriptCard::None);
	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Frames(2);
	REQUIRE(fixture.Panel->GetState().Card == VisualScriptCard::EmptyGraph);

	fixture.ClickItem(Item::AddUpdateEvent);
	REQUIRE(fixture.GetUndoCount() == undo + 1);
	REQUIRE(fixture.GetDocument().Graph.Nodes.size() == 1);
	CHECK(fixture.GetDocument().Graph.Nodes[0].Type == "Event.Update");
	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Frames(2);

	// "Add Node..." opens the palette at the view's centre.
	fixture.ClickItem(Item::AddNode);
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().PaletteOpen);
	// The node chosen there goes to the view's centre.
	glm::vec2 const center = fixture.Model().GetViewState().Center;
	ImGui::GetIO().AddInputCharactersUTF8("Branch");
	fixture.Frames(2);
	fixture.Gui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(1);
	REQUIRE(fixture.GetUndoCount() == undo + 1);
	REQUIRE(fixture.GetDocument().Graph.Nodes.size() == 1);
	VisualScriptNode const& branch = fixture.GetDocument().Graph.Nodes[0];
	CHECK(branch.Type == "Flow.Branch");
	REQUIRE(branch.Position.has_value());
	CHECK(*branch.Position == glm::round(center / 16.0f) * 16.0f);
}

TEST_CASE("VisualScriptEditor: the first canvas's hint shows once per user, until Got it")
{
	PanelFixture fixture(MakeDocument(), nullptr, true);
	fixture.CheckDrawingChangesNothing();
	REQUIRE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::FirstCanvas));
	fixture.ClickItem(Item::GotIt);
	fixture.Frames(1);
	CHECK_FALSE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::FirstCanvas));
	CHECK(fixture.Settings.IsHintSeen(VisualScriptCanvasHint));
	fixture.Reopen(fixture.Graph);
	CHECK_FALSE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::FirstCanvas));
}

TEST_CASE("VisualScriptEditor: a new function's hint shows once, and is gone after it is dismissed or the window shows another "
          "sub-graph")
{
	VisualScriptDocument document = MakeDocument();
	VisualScriptFunction function;
	function.ID = 40;
	function.Name = "Reset";
	function.Graph.Nodes = {MakeNode(41, "Function.Entry", glm::vec2(0.0f, 0.0f))};
	document.Functions.push_back(function);
	function.ID = 50;
	function.Name = "Jump";
	function.Graph.Nodes = {MakeNode(51, "Function.Entry", glm::vec2(0.0f, 0.0f))};
	document.Functions.push_back(function);
	PanelFixture fixture(document);
	fixture.CheckDrawingChangesNothing();
	CHECK_FALSE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::NewFunction));

	VisualScriptOpenRequest request;
	request.Graph = fixture.Graph;
	request.Function = 40;
	fixture.Panel->Apply(request);
	fixture.Frames(2);
	REQUIRE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::NewFunction));
	fixture.ClickItem(Item::DismissNewFunction);
	fixture.Frames(1);
	CHECK_FALSE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::NewFunction));

	// Another function: its hint shows, and once left it is not shown again.
	request.Function = 50;
	fixture.Panel->Apply(request);
	fixture.Frames(2);
	CHECK(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::NewFunction));
	request.Function = 0;
	fixture.Panel->Apply(request);
	fixture.Frames(2);
	CHECK_FALSE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::NewFunction));
	request.Function = 50;
	fixture.Panel->Apply(request);
	fixture.Frames(2);
	CHECK_FALSE(HasNotice(fixture.Panel->GetState(), VisualScriptNotice::NewFunction));
}

TEST_CASE("VisualScriptEditor: toasts tell paste notes, changes outside the view with a Show that frames them, and history steps "
          "of the scene")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();

	// Paste notes: the Create event pasted where one is.
	fixture.Click(Testing::CanvasPoint(fixture.Model(), 1, glm::vec2(0.5f, 0.06f)));
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_C, fixture.Drawer());
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_V, fixture.Drawer());
	fixture.Frames(1);
	CHECK(HasToast(fixture.Panel->GetState(), "left out 1 event"));

	// A change made outside the window, far beyond the view: a toast, whose Show frames it and keeps the selection.
	std::vector<uint32_t> const selection(fixture.Model().GetSelection().begin(), fixture.Model().GetSelection().end());
	VisualScriptDocument changed = fixture.GetDocument();
	for (VisualScriptNode& node : changed.Graph.Nodes)
	{
		if (node.ID == 3)
		{
			node.Position = glm::vec2(60000.0f, 40000.0f);
		}
	}
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, changed).IsOk());
	fixture.Frames(1);
	REQUIRE(HasToast(fixture.Panel->GetState(), "1 node changed outside the view"));
	std::optional<glm::vec2> const show = FindToastButton(fixture, "Show");
	REQUIRE(show.has_value());
	fixture.Click(*show);
	fixture.Frames(2);
	VisualScriptRect const visible = fixture.Model().GetView().GetVisibleRect();
	CHECK(VisualScriptGeometry::Contains(visible, glm::vec2(60000.0f, 40000.0f)));
	CHECK(std::vector<uint32_t>(fixture.Model().GetSelection().begin(), fixture.Model().GetSelection().end()) == selection);

	// A change in view flashes where it is, without a toast.
	size_t const toasts = fixture.Panel->GetState().Toasts.size();
	changed = fixture.GetDocument();
	for (VisualScriptNode& node : changed.Graph.Nodes)
	{
		if (node.ID == 3)
		{
			node.Position = glm::vec2(60040.0f, 40000.0f);
		}
	}
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Graph, changed).IsOk());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().Toasts.size() <= toasts);

	// Undoing a step of the scene from the focused window: the toast's Show focuses Scene View.
	EntityCreateInfo info;
	info.Name = "Lamp";
	REQUIRE(fixture.Operations.CreateEntity(info).IsOk());
	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Panel->NotifyHistoryStep(true, "Create Lamp", AssetHandle());
	fixture.Probe = []
	{
		ImGui::SetNextWindowPos(ImVec2(1700.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(200.0f, 200.0f));
		ImGui::Begin("Scene View", nullptr, ImGuiWindowFlags_NoSavedSettings);
		ImGui::End();
	};
	fixture.Frames(2);
	REQUIRE(HasToast(fixture.Panel->GetState(), "Undone: Create Lamp"));
	std::optional<glm::vec2> const scene = FindToastButton(fixture, "Show");
	REQUIRE(scene.has_value());
	fixture.Click(*scene);
	fixture.Frames(1);
	REQUIRE(GImGui->NavWindow != nullptr);
	CHECK(std::string_view(GImGui->NavWindow->Name) == "Scene View");
}

TEST_CASE("VisualScriptEditor: the toolbar shows LIVE while playing and SIMULATING while simulating")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	CHECK(fixture.Panel->GetState().Live == VisualScriptLiveState::None);

	fixture.Context.BeginPlay(Scene::Copy(fixture.Context.GetScene()), EditorPlayState::Play);
	fixture.Frames(2);
	CHECK(fixture.Panel->GetState().Live == VisualScriptLiveState::Live);
	CHECK(fixture.Panel->GetItemRect(Item::Live).has_value());
	// Without the scripting runtime no program runs, so an error does not make it the last working version.
	fixture.Context.EndPlay();
	fixture.Frames(2);
	CHECK(fixture.Panel->GetState().Live == VisualScriptLiveState::None);
	CHECK_FALSE(fixture.Panel->GetItemRect(Item::Live).has_value());

	fixture.Context.BeginPlay(Scene::Copy(fixture.Context.GetScene()), EditorPlayState::Simulate);
	fixture.Frames(2);
	CHECK(fixture.Panel->GetState().Live == VisualScriptLiveState::Simulating);
	CHECK(fixture.Panel->GetItemRect(Item::Live).has_value());
	fixture.Context.EndPlay();
	fixture.Frames(1);
}

TEST_CASE("VisualScriptEditor: the context entity is the request's, else the views file's, else the only entity that runs the "
          "graph, and entity drops set values on it")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	CHECK_FALSE(fixture.Panel->GetContextEntity().IsValid());
	CHECK_FALSE(fixture.Panel->GetItemRect(Item::Instance).has_value());

	// The only entity that runs the graph.
	UUID const door = CreateRunner(fixture, "Door");
	fixture.Frames(1);
	CHECK(fixture.Panel->GetContextEntity() == door);
	CHECK(fixture.Panel->GetState().ContextEntity == door);
	CHECK(fixture.Panel->GetItemRect(Item::Instance).has_value());
	// Two of them, none chosen: none.
	UUID const gate = CreateRunner(fixture, "Gate");
	fixture.Frames(1);
	CHECK_FALSE(fixture.Panel->GetContextEntity().IsValid());

	// The request's, kept in the views file for the next window of the graph.
	VisualScriptOpenRequest request;
	request.Graph = fixture.Graph;
	request.ContextEntity = gate;
	fixture.Panel->Apply(request);
	fixture.Frames(1);
	CHECK(fixture.Panel->GetContextEntity() == gate);
	REQUIRE(fixture.Settings.FindGraph(fixture.Graph) != nullptr);
	CHECK(fixture.Settings.FindGraph(fixture.Graph)->ContextEntity == gate);
	fixture.Reopen(fixture.Graph);
	CHECK(fixture.Panel->GetContextEntity() == gate);

	// An entity dropped on the canvas: the new variable, its Get, and its value on the context entity, in one step.
	EntityCreateInfo info;
	info.Name = "Lamp";
	Result<UUID> const lamp = fixture.Operations.CreateEntity(info);
	REQUIRE(lamp.IsOk());
	glm::vec2 const source(1500.0f, 860.0f);
	fixture.Probe = [&]
	{
		ImGui::SetNextWindowPos(ImVec2(source.x - 40.0f, source.y - 20.0f));
		ImGui::SetNextWindowSize(ImVec2(80.0f, 40.0f));
		ImGui::Begin("Source", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
		ImGui::Button("Lamp", ImVec2(60.0f, 20.0f));
		if (ImGui::BeginDragDropSource())
		{
			uint64_t const value = lamp.GetValue().GetValue();
			ImGui::SetDragDropPayload(DragDropPayload::Entities, &value, sizeof(value));
			ImGui::EndDragDropSource();
		}
		ImGui::End();
	};
	fixture.Frames(2);
	size_t const undo = fixture.GetUndoCount();
	fixture.Gui.DragPath(ImVec2(source.x - 10.0f, source.y - 5.0f), ToImVec2(fixture.EmptyPoint() + glm::vec2(400.0f, -300.0f)),
	                     fixture.Drawer(), ImGuiMouseButton_Left, 8);
	REQUIRE(fixture.GetUndoCount() == undo + 1);
	REQUIRE_FALSE(fixture.GetDocument().Variables.empty());
	CHECK(fixture.GetDocument().Variables.back().Name == "Lamp");
	Entity const gateEntity = fixture.Context.GetScene().GetEntityByUUID(gate);
	CHECK(gateEntity.GetComponent<VisualScriptComponent>().Variables.contains("Lamp"));
	Entity const doorEntity = fixture.Context.GetScene().GetEntityByUUID(door);
	CHECK_FALSE(doorEntity.GetComponent<VisualScriptComponent>().Variables.contains("Lamp"));
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK_FALSE(fixture.Context.GetScene().GetEntityByUUID(gate).GetComponent<VisualScriptComponent>().Variables.contains("Lamp"));
}

TEST_CASE("VisualScriptEditor: Ctrl+Alt+3 stores a bookmark and Ctrl+3 goes back to it on the history; the Bookmarks menu renames "
          "and deletes")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	fixture.Click(fixture.EmptyPoint());
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_3, fixture.Drawer());
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetState().Bookmarks == std::vector<uint8_t>{3});
	VisualScriptViewState const stored = fixture.Model().GetViewState();

	VisualScriptOpenRequest away;
	away.Graph = fixture.Graph;
	away.Center = glm::vec2(5000.0f, 4000.0f);
	away.Focus = false;
	fixture.Panel->Apply(away);
	fixture.Frames(2);
	REQUIRE(fixture.Model().GetViewState().Center != stored.Center);
	fixture.Click(fixture.EmptyPoint());
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_3, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.Model().GetViewState().Center.x == doctest::Approx(stored.Center.x));
	CHECK(fixture.Model().GetViewState().Center.y == doctest::Approx(stored.Center.y));
	CHECK(fixture.Model().CanGoBack());

	// Rename: the menu's Rename submenu, typed and confirmed with Enter.
	fixture.ClickItem(Item::Bookmarks);
	std::optional<glm::vec2> const rename = fixture.FindPopupItem("Rename");
	REQUIRE(rename.has_value());
	fixture.Frames(2);
	REQUIRE(GImGui->OpenPopupStack.Size >= 2);
	ImGuiWindow const* submenu = GImGui->OpenPopupStack.back().Window;
	REQUIRE(submenu != nullptr);
	int const slot = 3;
	ImGuiID const field = ImHashStr("##name", 0, ImHashData(&slot, sizeof(slot), submenu->ID));
	std::optional<glm::vec2> found;
	for (float x = submenu->Pos.x + 4.0f; x < submenu->Pos.x + submenu->Size.x && !found; x += 6.0f)
	{
		for (float y = submenu->Pos.y + 2.0f; y < submenu->Pos.y + submenu->Size.y && !found; y += 4.0f)
		{
			fixture.MoveMouse(glm::vec2(x, y));
			if (GImGui->HoveredId == field)
			{
				found = glm::vec2(x, y);
			}
		}
	}
	REQUIRE(found.has_value());
	// The click selects the name, which typing replaces.
	fixture.Click(*found);
	fixture.Gui.Type("Start");
	fixture.Frames(1);
	fixture.Gui.Press(ImGuiKey_Enter, fixture.Drawer());
	fixture.Frames(1);
	VisualScriptGraphViews const* views = fixture.Settings.FindGraph(fixture.Graph);
	REQUIRE(views != nullptr);
	REQUIRE(views->FindBookmark(3) != nullptr);
	CHECK(views->FindBookmark(3)->Name == "Start");

	// Delete: the menu's Delete submenu.
	fixture.Click(fixture.EmptyPoint());
	fixture.ClickItem(Item::Bookmarks);
	REQUIRE(fixture.FindPopupItem("Delete").has_value());
	fixture.Frames(2);
	std::optional<glm::vec2> const remove = fixture.FindPopupItem("Start##delete3");
	REQUIRE(remove.has_value());
	fixture.Click(*remove);
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().Bookmarks.empty());
}

TEST_CASE("VisualScriptEditor: F1 and the toolbar's ? open the shortcut sheet listing every key and mouse gesture, with the "
          "injected monospace font and with none")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	size_t const rows = VisualScriptShortcuts.size() + VisualScriptMouseGestures.size();
	for (bool const monospace : {true, false})
	{
		CAPTURE(monospace);
		ImFont* const font = fixture.Services.MonospaceFont;
		if (!monospace)
		{
			fixture.Services.MonospaceFont = nullptr;
		}
		fixture.Click(fixture.EmptyPoint());
		if (monospace)
		{
			fixture.Gui.Press(ImGuiKey_F1, fixture.Drawer());
		}
		else
		{
			fixture.ClickItem(Item::Shortcuts);
		}
		fixture.Frames(2);
		REQUIRE(fixture.Panel->GetState().ShortcutsOpen);
		REQUIRE(GImGui->OpenPopupStack.Size == 1);
		ImGuiWindow const* sheet = GImGui->OpenPopupStack.back().Window;
		REQUIRE(sheet != nullptr);
		CHECK((sheet->Flags & ImGuiWindowFlags_NoSavedSettings) != 0);
		CHECK(sheet->Size.x == doctest::Approx(34.0f * GetFontSize()).epsilon(0.01));
		// A row per key and per mouse gesture, besides the group headings.
		CHECK(sheet->ContentSize.y >= static_cast<float>(rows) * GetFontSize());

		if (monospace)
		{
			fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
		}
		else
		{
			fixture.Click(fixture.EmptyPoint());
		}
		fixture.Frames(1);
		CHECK_FALSE(fixture.Panel->GetState().ShortcutsOpen);
		fixture.Services.MonospaceFont = font;
	}
}

TEST_CASE("VisualScriptEditor: Ctrl+S in a focused graph window flashes the save status and still saves the scene")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	int saves = 0;
	fixture.Probe = [&saves]
	{
		// As EditorLayer::HandleShortcuts reads it.
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal))
		{
			saves++;
		}
	};
	fixture.Frames(2);
	fixture.Click(fixture.EmptyPoint());
	uint64_t const flashes = fixture.Panel->GetActivity().SaveFlashes;
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_S, fixture.Drawer());
	fixture.Frames(1);
	CHECK(saves == 1);
	CHECK(fixture.Panel->GetActivity().SaveFlashes == flashes + 1);
}

namespace
{
	// A window of the graph window's canvas, by a part of its name.
	ImGuiWindow const* FindCanvasChild(PanelFixture const& fixture, char const* name)
	{
		ImGuiWindow const* canvas = fixture.GetChild("##canvas");
		for (ImGuiWindow const* window : GImGui->Windows)
		{
			if (window->ParentWindow == canvas && window->Active && std::strstr(window->Name, name) != nullptr)
			{
				return window;
			}
		}
		return nullptr;
	}
}

TEST_CASE("VisualScriptEditor: the find bar sits at the canvas's top-right and fits a narrow canvas")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	for (float const width : {1600.0f, 420.0f})
	{
		CAPTURE(width);
		fixture.WindowSize = ImVec2(width, 600.0f);
		fixture.Frames(2);
		fixture.Click(fixture.EmptyPoint());
		fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_F, fixture.Drawer());
		fixture.Frames(2);
		REQUIRE(fixture.Panel->GetState().FindOpen);
		ImGuiWindow const* canvas = fixture.GetChild("##canvas");
		ImGuiWindow const* bar = FindCanvasChild(fixture, "##findBar");
		REQUIRE(bar != nullptr);
		float const fontSize = GetFontSize();
		CHECK(bar->Size.x == doctest::Approx(std::min(24.0f * fontSize, canvas->Size.x - 2.0f * fontSize)));
		CHECK(bar->Pos.x >= canvas->Pos.x);
		CHECK(bar->Pos.y >= canvas->Pos.y);
		CHECK(bar->Pos.x + bar->Size.x <= canvas->Pos.x + canvas->Size.x);
		CHECK(bar->Pos.y + bar->Size.y <= canvas->Pos.y + canvas->Size.y);
		// The bar has the keyboard; Escape there closes it.
		CHECK(ImGui::GetIO().WantTextInput);
		fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
		fixture.Frames(1);
		CHECK_FALSE(fixture.Panel->GetState().FindOpen);
	}
}

TEST_CASE("VisualScriptEditor: Ctrl+F opens the find bar and Ctrl+Shift+F the drawer's find, from the canvas and from My Script")
{
	VisualScriptDocument document = MakeDocument();
	VisualScriptVariable speed;
	speed.ID = 100;
	speed.Name = "Speed";
	speed.Type = "float";
	document.Variables.push_back(speed);
	PanelFixture fixture(document);
	fixture.CheckDrawingChangesNothing();
	fixture.Click(fixture.EmptyPoint());
	VisualScriptViewState const view = fixture.Model().GetViewState();
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_F, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().FindOpen);
	CHECK(fixture.Model().GetViewState() == view);
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(1);
	REQUIRE_FALSE(fixture.Panel->GetState().FindOpen);
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().Drawer == VisualScriptDrawerMode::Find);

	// From My Script.
	fixture.ClickItem(Item::Problems);
	fixture.ClickItem(Item::Problems);
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetState().Drawer == VisualScriptDrawerMode::Closed);
	std::optional<std::pair<glm::vec2, glm::vec2>> const row =
		fixture.Panel->GetSidebar().FindRow({VisualScriptElementKind::Variable, speed.ID});
	REQUIRE(row.has_value());
	fixture.Click((row->first + row->second) * 0.5f);
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetFocusedPart() == VisualScriptWindowPart::Sidebar);
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().Drawer == VisualScriptDrawerMode::Find);
	fixture.Click((row->first + row->second) * 0.5f);
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetFocusedPart() == VisualScriptWindowPart::Sidebar);
	REQUIRE_FALSE(ImGui::GetIO().WantTextInput);
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_F, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().FindOpen);
}

TEST_CASE("VisualScriptEditor: a click on a node's problem badge opens its problems, and the toolbar's pill toggles the problems "
          "drawer")
{
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(7, "Flow.NoSuchNode", glm::vec2(800.0f, 320.0f)));
	PanelFixture fixture(document);
	fixture.CheckDrawingChangesNothing();
	VisualScriptEditorState const state = fixture.Panel->GetState();
	REQUIRE(state.Errors >= 1);

	// The pill opens the drawer on the problems, and closes it.
	fixture.ClickItem(Item::Problems);
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetState().Drawer == VisualScriptDrawerMode::Problems);
	CHECK(fixture.Panel->GetDrawer().GetProblems().size() == state.Errors + state.Warnings);
	fixture.ClickItem(Item::Problems);
	fixture.Frames(1);
	CHECK(fixture.Panel->GetState().Drawer == VisualScriptDrawerMode::Closed);

	// The badge: its node's problems in a popup.
	VisualScriptCanvasModel const& canvas = fixture.Model().GetCanvas();
	std::optional<uint32_t> const index = canvas.FindNode(7);
	REQUIRE(index.has_value());
	std::optional<glm::vec2> const badge = VisualScriptGeometry::GetErrorBadgeCenter(canvas.GetNodes()[*index]);
	REQUIRE(badge.has_value());
	CHECK(GImGui->OpenPopupStack.Size == 0);
	fixture.Click(fixture.Model().GetView().ToScreen(*badge));
	fixture.Frames(1);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	ImGuiWindow const* canvasWindow = fixture.GetChild("##canvas");
	CHECK(GImGui->OpenPopupStack.back().PopupId == ImHashStr("##problemsPopup", 0, canvasWindow->ID));
	// A click elsewhere closes it.
	fixture.Click(fixture.EmptyPoint());
	fixture.Frames(1);
	CHECK(GImGui->OpenPopupStack.Size == 0);
}

TEST_CASE("VisualScriptEditor: a chosen context entity that was deleted is no context entity until the deletion is undone")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	CreateRunner(fixture, "Door");
	UUID const gate = CreateRunner(fixture, "Gate");
	VisualScriptOpenRequest request;
	request.Graph = fixture.Graph;
	request.ContextEntity = gate;
	fixture.Panel->Apply(request);
	fixture.Frames(1);
	REQUIRE(fixture.Panel->GetContextEntity() == gate);

	UUID const deleted[] = {gate};
	REQUIRE(fixture.Operations.DeleteEntities(deleted).IsOk());
	fixture.Frames(1);
	CHECK_FALSE(fixture.Panel->GetContextEntity().IsValid());
	CHECK_FALSE(fixture.Panel->GetState().ContextEntity.IsValid());

	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Frames(1);
	CHECK(fixture.Panel->GetContextEntity() == gate);
	CHECK(fixture.Panel->GetState().ContextEntity == gate);
}

TEST_CASE("VisualScriptEditor: a click on a link's problem badge at half the wire's length opens that link's problems and no other's")
{
	PanelFixture fixture(MakeDocument());
	VisualScriptLink const flow{{1, "Then"}, {2, "Execute"}};
	VisualScriptDiagnostic linkError;
	linkError.Code = "TypeMismatch";
	linkError.Severity = VisualScriptSeverity::Error;
	linkError.Link = flow;
	linkError.Message = "a test error of the link";
	fixture.Semantics.InjectDiagnostic(linkError);
	VisualScriptDiagnostic nodeError;
	nodeError.Code = "InvalidValue";
	nodeError.Severity = VisualScriptSeverity::Error;
	nodeError.Node = 3;
	nodeError.Message = "a test error of a node";
	fixture.Semantics.InjectDiagnostic(nodeError);
	fixture.Semantics.ReloadLibrary();
	fixture.Frames(3);
	fixture.CheckDrawingChangesNothing();

	VisualScriptCanvasModel const& canvas = fixture.Model().GetCanvas();
	std::span<VisualScriptLinkVisual const> const links = canvas.GetLinks();
	std::optional<uint32_t> index;
	for (uint32_t link = 0; link < links.size(); link++)
	{
		if (canvas.GetDocumentLink(link) == flow)
		{
			index = link;
		}
	}
	REQUIRE(index.has_value());
	REQUIRE(links[*index].HasFlag(VisualScriptLinkFlags::Error));
	// The badge's place, as the renderer draws it: half the flattened wire's length.
	VisualScriptView const& view = fixture.Model().GetView();
	std::array<glm::vec2, 4> points = VisualScriptGeometry::GetLinkControlPoints(canvas, *index, fixture.Model().GetNodePreview());
	for (glm::vec2& point : points)
	{
		point = view.ToScreen(point);
	}
	VisualScriptGeometry::WireFlattening const flattening =
		VisualScriptGeometry::GetWireFlattening(view.GetDetailLevel(), ImGui::GetStyle().CurveTessellationTol);
	std::vector<glm::vec2> polyline;
	VisualScriptGeometry::FlattenWire(points, flattening.Tolerance, flattening.SegmentCap, polyline);
	REQUIRE_FALSE(polyline.empty());
	glm::vec2 const badge = VisualScriptGeometry::GetPolylinePoint(polyline, 0.5f);

	// A click on the wire away from the badge opens nothing.
	fixture.Click(VisualScriptGeometry::GetPolylinePoint(polyline, 0.15f));
	fixture.Frames(1);
	CHECK(GImGui->OpenPopupStack.Size == 0);
	CHECK(fixture.Panel->GetPopupProblems().empty());

	fixture.Click(badge);
	fixture.Frames(1);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	ImGuiWindow const* canvasWindow = fixture.GetChild("##canvas");
	CHECK(GImGui->OpenPopupStack.back().PopupId == ImHashStr("##problemsPopup", 0, canvasWindow->ID));
	std::span<VisualScriptDiagnostic const> const problems = fixture.Panel->GetPopupProblems();
	REQUIRE(problems.size() == 1);
	CHECK(problems.front().Message == linkError.Message);
	fixture.Click(fixture.EmptyPoint());
	fixture.Frames(1);
	CHECK(GImGui->OpenPopupStack.Size == 0);
	CHECK(fixture.Panel->GetPopupProblems().empty());
}

TEST_CASE("VisualScriptEditor: the toolbar's problems pill says No problems without any, and Not analyzed without the scripting runtime")
{
	PanelFixture fixture(MakeDocument());
	fixture.CheckDrawingChangesNothing();
	VisualScriptEditorState const state = fixture.Panel->GetState();
	REQUIRE(state.Errors + state.Warnings == 0);
	ImGuiStyle const& style = ImGui::GetStyle();
	auto const width = [&style](std::string_view text)
	{
		return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x + style.FramePadding.x * 2.0f;
	};
	CHECK(fixture.ItemRect(Item::Problems).GetSize().x == doctest::Approx(width(VisualScriptPanelTexts::NoProblems)));

	fixture.Semantics.SetAvailable(false);
	fixture.Frames(2);
	REQUIRE_FALSE(fixture.Panel->GetState().Analyzed);
	CHECK(fixture.ItemRect(Item::Problems).GetSize().x == doctest::Approx(width(VisualScriptPanelTexts::NotAnalyzed)));

	// With problems: the counts.
	fixture.Semantics.SetAvailable(true);
	VisualScriptDiagnostic problem;
	problem.Code = "InvalidValue";
	problem.Severity = VisualScriptSeverity::Warning;
	problem.Node = 3;
	problem.Message = "a test warning";
	fixture.Semantics.InjectDiagnostic(problem);
	fixture.Semantics.ReloadLibrary();
	fixture.Frames(3);
	REQUIRE(fixture.Panel->GetState().Warnings == 1);
	CHECK(fixture.ItemRect(Item::Problems).GetSize().x != doctest::Approx(width(VisualScriptPanelTexts::NoProblems)));
}
