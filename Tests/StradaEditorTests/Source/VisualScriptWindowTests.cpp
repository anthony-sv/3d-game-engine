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
#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
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
	REQUIRE(fixture.Panel->GetPaletteRequest().has_value());
	CHECK(fixture.Panel->GetPaletteRequest()->Position == fixture.Model().GetViewState().Center);
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
