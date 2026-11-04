#include "HeadlessImGui.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/Commands/CommandHistory.h"
#include "Editor/Commands/EntityCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorEditingKeys.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/Panels/VisualScriptEditorPanel.h"
#include "Editor/PlayMode.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptEditors.h"
#include "Editor/VisualScripting/VisualScriptProjectIndex.h"
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
#include <array>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

// The window manager of the graph windows (spec §12): the editor's dock space drawn headless with stand-ins of its scene
// windows, and the graph windows the manager draws beside them.
namespace
{
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

	// Create (1) -> Delay (2), a Log Info (3), and a Get of the variable Speed (4).
	VisualScriptDocument MakeDocument()
	{
		VisualScriptDocument document;
		VisualScriptVariable speed;
		speed.ID = 100;
		speed.Name = "Speed";
		speed.Type = "float";
		document.Variables.push_back(std::move(speed));
		document.Graph.Nodes = {
			MakeNode(1, "Event.Create", glm::vec2(0.0f, 0.0f)),
			MakeNode(2, "Flow.Delay", glm::vec2(400.0f, 0.0f), Json::object(), Json::object({{"Duration", 1.5}})),
			MakeNode(3, "Call:Strada.Log.Info(string)", glm::vec2(800.0f, 0.0f), Json::object(), Json::object({{"Message", "Hi"}})),
			MakeNode(4, "Variable.Get", glm::vec2(400.0f, 320.0f), Json::object({{"Variable", 100}})),
		};
		document.Graph.Links = {{{1, "Then"}, {2, "Execute"}}};
		return document;
	}

	ImGuiID GetWindowID(AssetHandle graph)
	{
		return ImHashStr(VisualScriptEditorPanel::GetWindowName(graph, {}).c_str());
	}

	ImGuiWindow* FindGraphWindow(AssetHandle graph)
	{
		return ImGui::FindWindowByID(GetWindowID(graph));
	}

	// The window that has the keyboard, as its root (a graph window's panes count as the window).
	ImGuiWindow const* GetFocusedRoot()
	{
		return GImGui->NavWindow != nullptr ? GImGui->NavWindow->RootWindow : nullptr;
	}

	// The [Window] and [Table] entries ImGui would write to imgui.ini.
	std::set<std::string> GetSavedEntries()
	{
		std::set<std::string> entries;
		std::istringstream lines(ImGui::SaveIniSettingsToMemory());
		std::string line;
		while (std::getline(lines, line))
		{
			if (line.starts_with("[Window][") || line.starts_with("[Table]["))
			{
				entries.insert(line);
			}
		}
		return entries;
	}

	// What a stand-in of the editor's Edit menu drew, and the window that had the keyboard while it was open.
	struct EditMenuRecord
	{
		std::vector<EditorMenuItemRecord> Items;
		std::string KeyboardWindow;
	};

	std::string GetWindowName(ImGuiWindow const* window)
	{
		return window != nullptr ? std::string(window->Name) : std::string();
	}

	// The editor's dock space laid out as its default layout is: the scene view and the game view in the middle, the
	// hierarchy and the inspector on the right; a console and the real content browser float. The graph windows are drawn
	// after them, as the editor draws them after its fixed panels. Each frame runs what EditorLayer runs around them: the
	// keyboard window first (FindKeyboardWindow), Undo, Redo and the editing keys after the dock space
	// (HandleEditorHistoryKeys, HandleEditorEditingKeys). Three graphs: Scripts/Player, Scripts/Door and Enemies/Player.
	struct EditorsFixture
	{
		Testing::HeadlessImGui Gui;
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Temporary;
		EditorContext Context;
		EditorOperations Operations{Context};
		Testing::FixtureVisualScriptSemantics Semantics;
		std::filesystem::path ViewsFile = Temporary.GetPath() / "UserData" / "Editor" / "VisualScriptViews.json";
		VisualScriptViewSettings Settings{ViewsFile};
		VisualScriptEditors Editors{Operations, Semantics, Settings};
		ContentBrowserPanel ContentBrowser;
		bool ContentBrowserOpen = true;
		// The window that has the keyboard as EditorLayer keeps it (m_KeyboardWindow).
		ImGuiWindow const* KeyboardWindow = nullptr;
		std::filesystem::path ProjectFile;
		AssetHandle Player;
		AssetHandle Door;
		AssetHandle EnemyPlayer;
		ImGuiID Center = 0;
		ImGuiID Right = 0;
		// The dock space's layout is (re)built in the next frame, as View > Reset Layout does.
		bool ResetLayout = true;
		// Drawn after the windows every frame.
		std::function<void()> Probe;

		EditorsFixture()
		{
			VisualScriptEditorServices services;
			services.MonospaceFont = Gui.UseEditorStyle(true);
			Editors.SetServices(std::move(services));
			Settings.SetHintSeen(VisualScriptCanvasHint);
			Settings.SetReduceMotion(true);
			REQUIRE(Operations.CreateProject(Temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			ProjectFile = Context.GetProject()->GetFilePath();
			Player = AddGraph("Scripts/Player.sgraph");
			Door = AddGraph("Scripts/Door.sgraph");
			EnemyPlayer = AddGraph("Enemies/Player.sgraph");
			Editors.SetDefaultDockProvider(
				[]() -> std::optional<ImGuiID>
				{
					ImGuiWindow const* view = ImGui::FindWindowByName("Scene View");
					return view != nullptr && view->DockId != 0 ? std::optional<ImGuiID>(view->DockId) : std::nullopt;
				});
			Frames(3);
		}

		AssetHandle AddGraph(std::string const& path)
		{
			std::filesystem::path const file = AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(path);
			REQUIRE(FileSystem::CreateDirectories(file.parent_path()).IsOk());
			REQUIRE(VisualScriptSerializer::SaveToFile(MakeDocument(), file).IsOk());
			REQUIRE(Operations.RefreshAssets().IsOk());
			AssetHandle const graph = AssetManager::FindByPath(path);
			REQUIRE(graph.IsValid());
			return graph;
		}

		void Draw()
		{
			KeyboardWindow = FindKeyboardWindow(GImGui->NavWindow, KeyboardWindow);
			ImGuiID const dockspace = ImGui::DockSpaceOverViewport(ImGui::GetID("Dockspace"), ImGui::GetMainViewport());
			HandleEditorHistoryKeys(Operations, &Editors);
			HandleEditorEditingKeys(Operations, &Editors, FindClaimedEditingChords(KeyboardWindow));
			if (std::exchange(ResetLayout, false))
			{
				ImGui::DockBuilderRemoveNode(dockspace);
				ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
				ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
				ImGuiID center = dockspace;
				Right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.3f, nullptr, &center);
				Center = center;
				ImGui::DockBuilderDockWindow("Scene View", Center);
				ImGui::DockBuilderDockWindow("Game View", Center);
				ImGui::DockBuilderDockWindow("Scene Hierarchy", Right);
				ImGui::DockBuilderDockWindow("Inspector", Right);
				Editors.ResetLayout(Center);
				ImGui::DockBuilderFinish(dockspace);
			}
			for (char const* name : {"Scene View", "Game View", "Scene Hierarchy", "Inspector"})
			{
				ImGui::Begin(name);
				ImGui::End();
			}
			ImGui::SetNextWindowPos(ImVec2(100.0f, 900.0f), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize(ImVec2(300.0f, 120.0f), ImGuiCond_FirstUseEver);
			ImGui::Begin("Console");
			ImGui::End();
			// Over the right-hand dock node, clear of the graph windows' canvases.
			ImGui::SetNextWindowPos(ImVec2(1360.0f, 620.0f), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize(ImVec2(540.0f, 380.0f), ImGuiCond_FirstUseEver);
			ContentBrowser.OnImGuiRender(Operations, ContentBrowserOpen);
			Editors.OnImGuiRender();
			if (Probe)
			{
				Probe();
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
				Gui.Frame(Drawer());
			}
		}

		// Runs an action inside the next frame, after the windows are drawn.
		void InFrame(std::function<void()> action)
		{
			Probe = std::move(action);
			Frames(1);
			Probe = {};
		}

		void Focus(char const* name)
		{
			InFrame(
				[name]
				{
					ImGui::SetWindowFocus(name);
				});
			Frames(1);
		}

		void OpenGraph(AssetHandle graph, UUID entity = UUID::Invalid())
		{
			VisualScriptOpenRequest request;
			request.Graph = graph;
			request.ContextEntity = entity;
			Result<VisualScriptEditorState> const opened = Editors.Open(request);
			REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
			Frames(2);
		}

		VisualScriptEditorPanel& GetPanel(AssetHandle graph)
		{
			VisualScriptEditorPanel* panel = Editors.FindPanel(graph);
			REQUIRE(panel != nullptr);
			return *panel;
		}

		ImGuiWindow const& GetChild(AssetHandle graph, char const* name)
		{
			ImGuiWindow const* parent = FindGraphWindow(graph);
			REQUIRE(parent != nullptr);
			for (ImGuiWindow const* window : GImGui->Windows)
			{
				if (window->ParentWindow == parent && std::strstr(window->Name, name) != nullptr)
				{
					return *window;
				}
			}
			FAIL("the graph window has no child ", name);
			return *parent;
		}

		void Click(glm::vec2 point, ImGuiMouseButton button = ImGuiMouseButton_Left)
		{
			Gui.MouseClick(ImVec2(point.x, point.y), button, Drawer());
		}

		// A point of a graph window's canvas with nothing on it.
		glm::vec2 EmptyPoint(AssetHandle graph)
		{
			ImGuiWindow const& canvas = GetChild(graph, "##canvas");
			return glm::vec2(canvas.Pos.x + 30.0f, canvas.Pos.y + canvas.Size.y - 30.0f);
		}

		std::vector<AssetHandle> GetOpenGraphs()
		{
			std::vector<AssetHandle> graphs;
			for (VisualScriptEditorState const& state : Editors.Describe())
			{
				graphs.push_back(state.Graph);
			}
			return graphs;
		}

		// What drawing must leave alone of a graph: its file's bytes (none for a file that is gone) and its asset's version (none
		// for a graph that is not registered or does not load).
		struct GraphSnapshot
		{
			std::optional<std::string> Bytes;
			std::optional<uint64_t> Version;

			bool operator==(GraphSnapshot const& other) const = default;
		};

		std::vector<GraphSnapshot> TakeSnapshots() const
		{
			std::vector<GraphSnapshot> snapshots;
			for (AssetHandle const graph : {Player, Door, EnemyPlayer})
			{
				GraphSnapshot snapshot;
				std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(graph);
				if (metadata && !metadata->Path.empty())
				{
					Result<Buffer> const bytes =
						FileSystem::ReadBinaryFile(AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(metadata->Path));
					if (bytes)
					{
						Buffer const& buffer = bytes.GetValue();
						snapshot.Bytes.emplace(buffer.As<char>(), static_cast<size_t>(buffer.GetSize()));
					}
				}
				if (metadata && !AssetManager::IsMissing(graph) && !AssetManager::HasLoadFailure(graph))
				{
					if (Ref<VisualScriptAsset> const script = AssetManager::GetAsset<VisualScriptAsset>(graph))
					{
						snapshot.Version = script->GetVersion();
					}
				}
				snapshots.push_back(std::move(snapshot));
			}
			return snapshots;
		}

		// The panel invariant (spec §15.4), the first assertion of every case: three frames of drawing the editor's windows,
		// the graph windows among them, change no undo count, no file byte and no asset version.
		void CheckDrawingChangesNothing()
		{
			size_t const undo = Context.GetHistory().GetUndoCount();
			std::vector<GraphSnapshot> const before = TakeSnapshots();
			Frames(3);
			CHECK(Context.GetHistory().GetUndoCount() == undo);
			CHECK(TakeSnapshots() == before);
		}

		// Selects the node, opens its node comment in place and types the text, which stays in the field (a pending edit).
		void TypeNodeComment(AssetHandle graph, uint32_t node, char const* text)
		{
			// In front and focused first, so the canvas point is where the window shows the node; a second later, so the click
			// is no double-click with an earlier one there.
			OpenGraph(graph);
			Gui.Frame(Drawer(), 1.0f);
			VisualScriptEditorPanel& panel = GetPanel(graph);
			REQUIRE(panel.GetState().Focused);
			Click(Testing::CanvasPoint(panel.GetModel(), node, glm::vec2(0.5f, 0.06f)));
			Frames(1);
			REQUIRE(panel.GetState().SelectedNodes == std::vector<uint32_t>{node});
			REQUIRE(panel.RequestAction(VisualScriptEditorAction::AddNodeComment));
			Frames(3);
			REQUIRE(ImGui::GetIO().WantTextInput);
			Gui.Type(text);
			Frames(1);
			REQUIRE(panel.HasPendingWidgetEdit());
		}

		// The comment of the graph's node as its asset holds it.
		static std::string GetNodeComment(AssetHandle graph, uint32_t node)
		{
			Ref<VisualScriptAsset> const script = AssetManager::GetAsset<VisualScriptAsset>(graph);
			REQUIRE(script != nullptr);
			VisualScriptDocument const& document = script->GetDocument();
			auto const found = std::find_if(document.Graph.Nodes.begin(), document.Graph.Nodes.end(),
			                                [node](VisualScriptNode const& candidate)
			                                {
												return candidate.ID == node;
											});
			REQUIRE(found != document.Graph.Nodes.end());
			return found->Comment;
		}

		// Moves the mouse along the middle of a graph window's toasts until the toast's button is hovered; the point found.
		std::optional<glm::vec2> FindToastButton(AssetHandle graph, char const* label)
		{
			ImGuiWindow const& canvas = GetChild(graph, "##canvas");
			std::vector<ImGuiWindow const*> toasts;
			for (ImGuiWindow const* window : GImGui->Windows)
			{
				if (window->ParentWindow == &canvas && window->Active && std::strstr(window->Name, "##toast") != nullptr)
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
					ImGui::GetIO().AddMousePosEvent(x, y);
					Frames(1);
					if (GImGui->HoveredId == item)
					{
						return glm::vec2(x, y);
					}
				}
			}
			return std::nullopt;
		}

		// The graphs the views file keeps open for the project.
		std::vector<AssetHandle> ReadSavedOpenGraphs() const
		{
			VisualScriptViewSettings saved(ViewsFile);
			CHECK(saved.Load().empty());
			saved.SetProject(ProjectFile);
			std::span<AssetHandle const> const open = saved.GetOpenGraphs();
			return std::vector<AssetHandle>(open.begin(), open.end());
		}

		// Opens a stand-in of the editor's Edit menu (a popup, as a menu bar's menu is) whose editing items are drawn as
		// EditorLayer draws them, for the chords the keyboard window claims; runs whileOpen (frames of its own) while the menu
		// is open, and returns what the items drew and closes the menu again.
		EditMenuRecord DrawEditMenu(std::function<void()> const& whileOpen = {})
		{
			EditMenuRecord record;
			bool open = true;
			bool close = false;
			Probe = [this, &record, &open, &close]
			{
				if (std::exchange(open, false))
				{
					ImGui::OpenPopup("Edit");
				}
				if (ImGui::BeginPopup("Edit"))
				{
					record.Items = DrawEditorEditingMenuItems(Operations, &Editors, FindClaimedEditingChords(KeyboardWindow));
					if (close)
					{
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
			};
			Frames(2);
			REQUIRE(GImGui->NavWindow != nullptr);
			REQUIRE((GImGui->NavWindow->Flags & ImGuiWindowFlags_Popup) != 0);
			record.KeyboardWindow = GetWindowName(KeyboardWindow);
			if (whileOpen)
			{
				whileOpen();
				REQUIRE(GImGui->OpenPopupStack.Size == 1);
			}
			close = true;
			Frames(1);
			Probe = {};
			Frames(1);
			REQUIRE(GImGui->OpenPopupStack.Size == 0);
			return record;
		}

		// The shortcut text of a drawn item; fails when the item was not drawn.
		static std::string GetShortcutText(EditMenuRecord const& record, std::string_view label)
		{
			auto const found = std::find_if(record.Items.begin(), record.Items.end(),
			                                [label](EditorMenuItemRecord const& item)
			                                {
												return item.Label == label;
											});
			REQUIRE(found != record.Items.end());
			return found->ShortcutText;
		}

		// The number of nodes of a type in the graph's document.
		static size_t CountNodes(AssetHandle graph, std::string_view type)
		{
			Ref<VisualScriptAsset> const script = AssetManager::GetAsset<VisualScriptAsset>(graph);
			REQUIRE(script != nullptr);
			std::vector<VisualScriptNode> const& nodes = script->GetDocument().Graph.Nodes;
			return static_cast<size_t>(std::count_if(nodes.begin(), nodes.end(),
			                                         [type](VisualScriptNode const& node)
			                                         {
														 return node.Type == type;
													 }));
		}

		// Moves the mouse down a popup's items until the item is hovered; the point found.
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
				ImGui::GetIO().AddMousePosEvent(point.x, point.y);
				Frames(1);
				if (GImGui->HoveredId == item)
				{
					return point;
				}
			}
			return std::nullopt;
		}
	};
}

TEST_CASE("VisualScriptEditor: graph windows dock beside the scene view, and a new one joins the graph window focused last")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	ImGuiWindow const* sceneView = ImGui::FindWindowByName("Scene View");
	REQUIRE(sceneView != nullptr);
	ImGuiID const sceneNode = sceneView->DockId;
	REQUIRE(sceneNode != 0);

	// The first two graphs open as tabs of the scene view's node.
	fixture.OpenGraph(fixture.Player);
	fixture.OpenGraph(fixture.Door);
	fixture.CheckDrawingChangesNothing();
	REQUIRE(FindGraphWindow(fixture.Player) != nullptr);
	REQUIRE(FindGraphWindow(fixture.Door) != nullptr);
	CHECK(FindGraphWindow(fixture.Player)->DockId == sceneNode);
	CHECK(FindGraphWindow(fixture.Door)->DockId == sceneNode);
	CHECK(GetFocusedRoot() == FindGraphWindow(fixture.Door));

	// Once the graph focused last is docked elsewhere, the next one joins it there.
	std::string const doorName = VisualScriptEditorPanel::GetWindowName(fixture.Door, "Door");
	fixture.InFrame(
		[&fixture, &doorName]
		{
			ImGui::DockBuilderDockWindow(doorName.c_str(), fixture.Right);
		});
	fixture.Frames(1);
	fixture.Focus(doorName.c_str());
	REQUIRE(FindGraphWindow(fixture.Door)->DockId == fixture.Right);
	fixture.OpenGraph(fixture.EnemyPlayer);
	CHECK(FindGraphWindow(fixture.EnemyPlayer)->DockId == fixture.Right);
	CHECK(FindGraphWindow(fixture.Player)->DockId == sceneNode);
}

TEST_CASE("VisualScriptEditor: the project index loads the project's graphs once a graph window opens, not before")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	// The invariant's snapshots load the graphs: unloaded again, frames drawn without a graph window load none.
	for (AssetHandle const graph : {fixture.Player, fixture.Door, fixture.EnemyPlayer})
	{
		AssetManager::UnloadAsset(graph);
	}
	fixture.Frames(3);
	VisualScriptProjectIndex const& index = fixture.Editors.GetProjectIndex();
	CHECK(index.GetGraphs().empty());
	for (AssetHandle const graph : {fixture.Player, fixture.Door, fixture.EnemyPlayer})
	{
		CHECK_FALSE(AssetManager::IsLoaded(graph));
	}

	fixture.OpenGraph(fixture.Door);
	CHECK(index.GetGraphs().size() == 3);
	for (AssetHandle const graph : {fixture.Player, fixture.Door, fixture.EnemyPlayer})
	{
		CHECK(AssetManager::IsLoaded(graph));
		VisualScriptProjectGraph const* const found = index.Find(graph);
		REQUIRE(found != nullptr);
		CHECK(found->Document == AssetManager::GetAsset<VisualScriptAsset>(graph)->GetSharedDocument());
	}
}

TEST_CASE("VisualScriptEditor: without a node to dock into, a graph window floats in the middle of the main viewport")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	fixture.Editors.SetDefaultDockProvider({});
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	ImGuiWindow const* window = FindGraphWindow(fixture.Player);
	REQUIRE(window != nullptr);
	CHECK(window->DockId == 0);
	ImGuiViewport const* viewport = ImGui::GetMainViewport();
	CHECK(window->Size.x == doctest::Approx(viewport->WorkSize.x * VisualScriptEditors::FloatingShare));
	CHECK(window->Size.y == doctest::Approx(viewport->WorkSize.y * VisualScriptEditors::FloatingShare));
	CHECK(window->Pos.x + window->Size.x * 0.5f == doctest::Approx(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f));
	CHECK(window->Pos.y + window->Size.y * 0.5f == doctest::Approx(viewport->WorkPos.y + viewport->WorkSize.y * 0.5f));
}

TEST_CASE("VisualScriptEditor: opening a graph that is open keeps its one window and brings it to the front")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	fixture.OpenGraph(fixture.Player);
	fixture.OpenGraph(fixture.Door);
	fixture.CheckDrawingChangesNothing();
	REQUIRE(GetFocusedRoot() == FindGraphWindow(fixture.Door));
	CHECK_FALSE(fixture.GetPanel(fixture.Player).GetState().Visible);

	fixture.OpenGraph(fixture.Player);
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Player, fixture.Door});
	CHECK(GetFocusedRoot() == FindGraphWindow(fixture.Player));
	CHECK(fixture.GetPanel(fixture.Player).GetState().Visible);
	CHECK(fixture.GetPanel(fixture.Player).GetState().Focused);
}

TEST_CASE("VisualScriptEditor: graph windows, their modals, sheets and popups and their pane sizes leave imgui.ini as it was")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	std::set<std::string> const before = GetSavedEntries();
	REQUIRE_FALSE(before.empty());

	for (AssetHandle const graph : {fixture.Player, fixture.Door})
	{
		CAPTURE(graph.ToString());
		fixture.OpenGraph(graph);
		fixture.CheckDrawingChangesNothing();
		VisualScriptEditorPanel& panel = fixture.GetPanel(graph);
		REQUIRE(panel.GetState().Visible);
		size_t const undo = fixture.Context.GetHistory().GetUndoCount();

		// The shortcut sheet.
		fixture.Click(fixture.EmptyPoint(graph));
		fixture.Gui.Press(ImGuiKey_F1, fixture.Drawer());
		fixture.Frames(1);
		REQUIRE(panel.GetState().ShortcutsOpen);
		fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
		fixture.Frames(1);
		CHECK_FALSE(panel.GetState().ShortcutsOpen);

		// A node's menu.
		fixture.Click(Testing::CanvasPoint(panel.GetModel(), 2, glm::vec2(0.5f, 0.06f)), ImGuiMouseButton_Right);
		fixture.Frames(1);
		REQUIRE(GImGui->OpenPopupStack.Size == 1);
		fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
		fixture.Frames(1);
		CHECK(GImGui->OpenPopupStack.Size == 0);

		// My Script's delete confirmation, a modal.
		std::optional<std::pair<glm::vec2, glm::vec2>> const row = panel.GetSidebar().FindRow({VisualScriptElementKind::Variable, 100});
		REQUIRE(row.has_value());
		fixture.Click((row->first + row->second) * 0.5f);
		fixture.Frames(1);
		fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
		fixture.Frames(2);
		REQUIRE(panel.GetSidebar().GetPendingDelete().has_value());
		REQUIRE(ImGui::GetTopMostPopupModal() != nullptr);
		fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
		fixture.Frames(2);
		CHECK(ImGui::GetTopMostPopupModal() == nullptr);
		CHECK(fixture.Context.GetHistory().GetUndoCount() == undo);

		// My Script made wider by its splitter.
		float const width = fixture.Settings.GetPanes().Sidebar;
		ImGuiWindow const& sidebar = fixture.GetChild(graph, "##sidebar");
		glm::vec2 const splitter(sidebar.Pos.x + sidebar.Size.x + 2.0f, sidebar.Pos.y + sidebar.Size.y * 0.5f);
		fixture.Gui.DragPath(ImVec2(splitter.x, splitter.y), ImVec2(splitter.x + 60.0f, splitter.y), fixture.Drawer());
		CHECK(fixture.Settings.GetPanes().Sidebar > width);
	}
	CHECK(fixture.Editors.Close() == std::vector<AssetHandle>{fixture.Player, fixture.Door});
	fixture.Frames(2);
	CHECK(GetSavedEntries() == before);
}

TEST_CASE("VisualScriptEditor: a graph deleted while its window is open closes the window, says so, and undo goes on")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	EntityCreateInfo info;
	info.Name = "Crate";
	Result<UUID> const crate = fixture.Operations.CreateEntity(info);
	REQUIRE(crate.IsOk());
	fixture.OpenGraph(fixture.Player);
	fixture.OpenGraph(fixture.Door);
	fixture.CheckDrawingChangesNothing();

	// Drawn: the window closes in the next frame.
	uint64_t const firstLog = Log::GetNextEntryIndex();
	REQUIRE(fixture.Operations.DeleteAsset(fixture.Player).IsOk());
	fixture.Frames(1);
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Door});
	CHECK(Testing::WasLogged(firstLog, "Closed 'Scripts/Player.sgraph': the visual script was deleted"));
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK_FALSE(fixture.Context.GetScene().HasEntity(crate.GetValue()));

	// Between frames: neither reported nor reached, and the views file forgets it, without a draw.
	fixture.Settings.SetShown(fixture.Door, 0);
	REQUIRE(fixture.Settings.FindGraph(fixture.Door) != nullptr);
	REQUIRE(fixture.Operations.DeleteAsset(fixture.Door).IsOk());
	CHECK(fixture.Editors.Describe().empty());
	CHECK(fixture.Editors.FindPanel(fixture.Door) == nullptr);
	CHECK(fixture.Editors.GetEditTarget() == nullptr);
	CHECK(fixture.Settings.FindGraph(fixture.Door) == nullptr);
}

TEST_CASE("VisualScriptEditor: closing a graph window commits its edits and writes the views file, by Close and by its card's Close")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	fixture.OpenGraph(fixture.Player);
	fixture.OpenGraph(fixture.Door);
	fixture.CheckDrawingChangesNothing();
	REQUIRE_FALSE(FileSystem::Exists(fixture.ViewsFile));

	// A node comment still in its field is committed, as one step, before the window goes (its X and graph.close close it as
	// Close does).
	fixture.OpenGraph(fixture.Player);
	size_t const undo = fixture.Context.GetHistory().GetUndoCount();
	fixture.TypeNodeComment(fixture.Player, 3, "Typed");
	CHECK(fixture.GetNodeComment(fixture.Player, 3).empty());
	CHECK(fixture.Editors.Close(fixture.Player) == std::vector<AssetHandle>{fixture.Player});
	CHECK(fixture.GetNodeComment(fixture.Player, 3) == "Typed");
	CHECK(fixture.Context.GetHistory().GetUndoCount() == undo + 1);
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Door});
	REQUIRE(FileSystem::Exists(fixture.ViewsFile));
	CHECK(fixture.ReadSavedOpenGraphs() == std::vector<AssetHandle>{fixture.Door});
	// Closing a graph that is not open is no error.
	CHECK(fixture.Editors.Close(fixture.Player).empty());

	// Flush (the editor's exit, a project action) commits an edit still in a field and keeps the window.
	fixture.TypeNodeComment(fixture.Door, 3, "Flushed");
	fixture.Editors.Flush();
	CHECK(fixture.GetNodeComment(fixture.Door, 3) == "Flushed");
	CHECK(fixture.Context.GetHistory().GetUndoCount() == undo + 2);
	CHECK_FALSE(fixture.GetPanel(fixture.Door).HasPendingWidgetEdit());
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Door});
	// The field closes with its edit committed: the next frames add nothing.
	fixture.Frames(2);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == undo + 2);

	// A missing file shows its card, whose Close closes the window as its X does.
	std::filesystem::path const door = AssetManager::GetAssetDirectory() / "Scripts" / "Door.sgraph";
	REQUIRE(FileSystem::Remove(door).IsOk());
	REQUIRE(fixture.Operations.RefreshAssets().IsOk());
	fixture.Frames(2);
	VisualScriptEditorPanel& panel = fixture.GetPanel(fixture.Door);
	REQUIRE(panel.GetState().Card == VisualScriptCard::Missing);
	// The card sizes itself to its text, then centres itself: a frame each.
	fixture.Frames(2);
	std::optional<VisualScriptRect> const close = panel.GetItemRect(VisualScriptPanelItem::CloseCard);
	REQUIRE(close.has_value());
	fixture.Click(close->GetCenter());
	fixture.Frames(1);
	CHECK(fixture.GetOpenGraphs().empty());
	CHECK(fixture.ReadSavedOpenGraphs().empty());
}

TEST_CASE("VisualScriptEditor: a project change closes the graph windows and opening the project again restores them in order")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	fixture.OpenGraph(fixture.Door);
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	// A view of Player's to come back to.
	VisualScriptOpenRequest view;
	view.Graph = fixture.Player;
	view.Center = glm::vec2(1234.0f, 567.0f);
	view.Zoom = 0.5f;
	REQUIRE(fixture.Editors.Open(view).IsOk());
	fixture.Frames(3);
	REQUIRE(fixture.GetPanel(fixture.Player).GetState().View.Center == glm::vec2(1234.0f, 567.0f));

	REQUIRE(fixture.Operations.CloseProject().IsOk());
	fixture.Frames(1);
	CHECK(fixture.GetOpenGraphs().empty());
	CHECK(fixture.ReadSavedOpenGraphs() == std::vector<AssetHandle>{fixture.Door, fixture.Player});

	// A graph the project no longer has is skipped, with a line in the log.
	AssetHandle const gone(UUID(918273645));
	std::vector<AssetHandle> const kept = {fixture.Door, gone, fixture.Player};
	fixture.Settings.SetProject(fixture.ProjectFile);
	fixture.Settings.SetOpenGraphs(kept);
	fixture.Settings.SetProject({});

	uint64_t const firstLog = Log::GetNextEntryIndex();
	REQUIRE(fixture.Operations.OpenProject(fixture.ProjectFile).IsOk());
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Door, fixture.Player});
	CHECK(Testing::WasLogged(firstLog, gone.ToString()));
	fixture.Frames(3);
	VisualScriptEditorState const player = fixture.GetPanel(fixture.Player).GetState();
	CHECK(player.View.Center == glm::vec2(1234.0f, 567.0f));
	CHECK(player.View.Zoom == doctest::Approx(0.5f));
}

TEST_CASE("VisualScriptEditor: two open graphs with one name show their folders")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	fixture.OpenGraph(fixture.Player);
	CHECK(std::string_view(FindGraphWindow(fixture.Player)->Name).starts_with("Player###"));
	fixture.OpenGraph(fixture.EnemyPlayer);
	fixture.CheckDrawingChangesNothing();
	CHECK(std::string_view(FindGraphWindow(fixture.Player)->Name).starts_with("Player (Scripts)###"));
	CHECK(std::string_view(FindGraphWindow(fixture.EnemyPlayer)->Name).starts_with("Player (Enemies)###"));

	// A rename keeps the window, which follows the new name.
	REQUIRE(fixture.Operations.MoveAsset(fixture.EnemyPlayer, "Enemies/Boss.sgraph").IsOk());
	fixture.Frames(2);
	CHECK(std::string_view(FindGraphWindow(fixture.Player)->Name).starts_with("Player###"));
	CHECK(std::string_view(FindGraphWindow(fixture.EnemyPlayer)->Name).starts_with("Boss###"));
	CHECK(fixture.GetOpenGraphs().size() == 2);
}

TEST_CASE("VisualScriptEditor: View > Reset Layout docks the open graph windows into the middle, and only those")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	fixture.OpenGraph(fixture.Player);
	fixture.OpenGraph(fixture.Door);
	fixture.CheckDrawingChangesNothing();
	REQUIRE(fixture.Editors.Close(fixture.Door).size() == 1);
	std::string const playerName = VisualScriptEditorPanel::GetWindowName(fixture.Player, "Player");
	fixture.InFrame(
		[&fixture, &playerName]
		{
			ImGui::DockBuilderDockWindow(playerName.c_str(), fixture.Right);
		});
	fixture.Frames(1);
	REQUIRE(FindGraphWindow(fixture.Player)->DockId == fixture.Right);
	ImGuiWindowSettings const* doorSettings = ImGui::FindWindowSettingsByID(GetWindowID(fixture.Door));

	fixture.ResetLayout = true;
	fixture.Frames(2);
	CHECK(FindGraphWindow(fixture.Player)->DockId == fixture.Center);
	// No settings entry is made for a window that is not open.
	CHECK(ImGui::FindWindowSettingsByID(GetWindowID(fixture.Door)) == doorSettings);
}

TEST_CASE("VisualScriptEditor: the Edit menu acts on the graph window focused last while it is shown, else on the scene")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	EntityCreateInfo info;
	info.Name = "Crate";
	Result<UUID> const crate = fixture.Operations.CreateEntity(info);
	REQUIRE(crate.IsOk());
	std::array<UUID, 1> const selection = {crate.GetValue()};
	REQUIRE(fixture.Operations.Select(selection).IsOk());
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	VisualScriptEditorPanel& panel = fixture.GetPanel(fixture.Player);
	fixture.Click(Testing::CanvasPoint(panel.GetModel(), 3, glm::vec2(0.5f, 0.06f)));
	fixture.Frames(1);
	REQUIRE(panel.GetState().SelectedNodes == std::vector<uint32_t>{3});
	CHECK(fixture.Editors.GetEditTarget() == &panel);

	// The console takes the focus: the graph stays the target. The Edit menu's Delete deletes its node, not the entity.
	fixture.Focus("Console");
	REQUIRE(std::string_view(GetFocusedRoot()->Name) == "Console");
	REQUIRE(fixture.Editors.GetEditTarget() == &panel);
	size_t const undo = fixture.Context.GetHistory().GetUndoCount();
	bool drawn = false;
	bool openMenu = true;
	fixture.Probe = [&fixture, &drawn, &openMenu]
	{
		if (std::exchange(openMenu, false))
		{
			ImGui::OpenPopup("Edit");
		}
		if (ImGui::BeginPopup("Edit"))
		{
			drawn = fixture.Editors.DrawEditMenuItems({}).has_value();
			ImGui::EndPopup();
		}
	};
	fixture.Frames(2);
	REQUIRE(drawn);
	std::optional<glm::vec2> const item = fixture.FindPopupItem("Delete");
	REQUIRE(item.has_value());
	fixture.Click(*item);
	fixture.Probe = {};
	fixture.Frames(2);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == undo + 1);
	Ref<VisualScriptAsset> const script = AssetManager::GetAsset<VisualScriptAsset>(fixture.Player);
	REQUIRE(script != nullptr);
	VisualScriptDocument const& document = script->GetDocument();
	CHECK(std::none_of(document.Graph.Nodes.begin(), document.Graph.Nodes.end(),
	                   [](VisualScriptNode const& node)
	                   {
						   return node.ID == 3;
					   }));
	CHECK(fixture.Context.GetScene().HasEntity(crate.GetValue()));

	// A scene window focused after it makes the scene the target.
	fixture.Focus("Scene Hierarchy");
	CHECK(fixture.Editors.GetEditTarget() == nullptr);
	fixture.InFrame(
		[&fixture, &drawn]
		{
			drawn = fixture.Editors.DrawEditMenuItems({}).has_value();
		});
	CHECK_FALSE(drawn);

	// The graph focused last, then hidden behind the game view's tab: the scene is the target while it is not shown.
	fixture.Click(fixture.EmptyPoint(fixture.Player));
	REQUIRE(fixture.Editors.GetEditTarget() == &panel);
	fixture.Focus("Game View");
	REQUIRE_FALSE(panel.GetState().Visible);
	CHECK(fixture.Editors.GetEditTarget() == nullptr);
}

TEST_CASE("VisualScriptEditor: the editing keys act on the Edit menu's target while a window that does not change it has the keyboard")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	EntityCreateInfo info;
	info.Name = "Crate";
	Result<UUID> const crate = fixture.Operations.CreateEntity(info);
	REQUIRE(crate.IsOk());
	// A second entity, which Ctrl+A would select were it to reach the entities.
	info.Name = "Barrel";
	REQUIRE(fixture.Operations.CreateEntity(info).IsOk());
	std::array<UUID, 1> const selection = {crate.GetValue()};
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	VisualScriptEditorPanel& panel = fixture.GetPanel(fixture.Player);
	CommandHistory const& history = fixture.Context.GetHistory();
	size_t const entities = fixture.Context.GetScene().GetEntityCount();
	// Selects the node in the graph window, then gives the keyboard to the console with the crate selected: the graph stays
	// the Edit menu's target and its window no longer claims the keys.
	auto const selectNode = [&fixture, &panel, &selection](uint32_t node)
	{
		fixture.Click(Testing::CanvasPoint(panel.GetModel(), node, glm::vec2(0.5f, 0.06f)));
		fixture.Frames(1);
		REQUIRE(panel.GetState().SelectedNodes == std::vector<uint32_t>{node});
		fixture.Focus("Console");
		REQUIRE(GetWindowName(fixture.KeyboardWindow) == "Console");
		REQUIRE(fixture.Editors.GetEditTarget() == &panel);
		REQUIRE(fixture.Operations.Select(selection).IsOk());
	};
	auto const chord = [&fixture](ImGuiKeyChord keys)
	{
		fixture.Gui.Chord(keys, fixture.Drawer());
		fixture.Frames(1);
	};

	// Ctrl+A selects every node, not every entity.
	selectNode(3);
	size_t undo = history.GetUndoCount();
	chord(ImGuiMod_Ctrl | ImGuiKey_A);
	CHECK(panel.GetState().SelectedNodes.size() == 4);
	CHECK(fixture.Context.GetSelection().GetEntities() == std::vector<UUID>{crate.GetValue()});
	CHECK(history.GetUndoCount() == undo);

	// Delete and Ctrl+Backspace delete the selected node and leave the selected entity.
	selectNode(1);
	undo = history.GetUndoCount();
	chord(ImGuiKey_Delete);
	CHECK(history.GetUndoCount() == undo + 1);
	CHECK(EditorsFixture::CountNodes(fixture.Player, "Event.Create") == 0);
	CHECK(fixture.Context.GetScene().HasEntity(crate.GetValue()));
	CHECK(fixture.Context.GetSelection().Contains(crate.GetValue()));
	selectNode(3);
	undo = history.GetUndoCount();
	chord(ImGuiMod_Ctrl | ImGuiKey_Backspace);
	CHECK(history.GetUndoCount() == undo + 1);
	CHECK(EditorsFixture::CountNodes(fixture.Player, "Call:Strada.Log.Info(string)") == 0);
	CHECK(fixture.Context.GetScene().HasEntity(crate.GetValue()));

	// Ctrl+D duplicates the node, Ctrl+X cuts what is then selected in one step.
	selectNode(2);
	undo = history.GetUndoCount();
	chord(ImGuiMod_Ctrl | ImGuiKey_D);
	CHECK(history.GetUndoCount() == undo + 1);
	CHECK(EditorsFixture::CountNodes(fixture.Player, "Flow.Delay") == 2);
	REQUIRE(panel.GetState().SelectedNodes.size() == 1);
	undo = history.GetUndoCount();
	chord(ImGuiMod_Ctrl | ImGuiKey_X);
	CHECK(history.GetUndoCount() == undo + 1);
	CHECK(EditorsFixture::CountNodes(fixture.Player, "Flow.Delay") == 1);
	CHECK_FALSE(std::string_view(ImGui::GetClipboardText()).empty());

	// Ctrl+C copies without a step, Ctrl+V pastes into the graph.
	selectNode(4);
	undo = history.GetUndoCount();
	chord(ImGuiMod_Ctrl | ImGuiKey_C);
	CHECK(history.GetUndoCount() == undo);
	chord(ImGuiMod_Ctrl | ImGuiKey_V);
	CHECK(history.GetUndoCount() == undo + 1);
	CHECK(EditorsFixture::CountNodes(fixture.Player, "Variable.Get") == 2);
	CHECK(fixture.Context.GetScene().GetEntityCount() == entities);
	CHECK(GetWindowName(fixture.KeyboardWindow) == "Console");

	// The Edit menu shows every key's text while the console has the keyboard, and the keyboard window stays the console
	// while the menu has it.
	EditMenuRecord const consoleMenu = fixture.DrawEditMenu();
	CHECK(consoleMenu.KeyboardWindow == "Console");
	std::vector<std::pair<std::string, std::string>> texts;
	for (EditorMenuItemRecord const& item : consoleMenu.Items)
	{
		texts.emplace_back(item.Label, item.ShortcutText);
	}
	std::vector<std::pair<std::string, std::string>> const expected = {
		{"Cut", "Ctrl+X"},    {"Copy", "Ctrl+C"},       {"Paste", "Ctrl+V"},  {"Duplicate", "Ctrl+D"},
		{"Delete", "Delete"}, {"Select All", "Ctrl+A"}, {"Deselect All", ""},
	};
	CHECK(texts == expected);

	// The content browser has the keyboard with an asset selected: Delete is its own (it asks to delete the file) and the
	// graph is left alone; the menu's Delete shows no key then, the other items theirs.
	fixture.ContentBrowser.SetFolder("Scripts");
	fixture.Context.SelectAsset(fixture.Door);
	fixture.Focus(ContentBrowserPanel::WindowName);
	REQUIRE(GetWindowName(fixture.KeyboardWindow) == ContentBrowserPanel::WindowName);
	REQUIRE(fixture.Editors.GetEditTarget() == &panel);
	undo = history.GetUndoCount();
	size_t const nodes = AssetManager::GetAsset<VisualScriptAsset>(fixture.Player)->GetDocument().Graph.Nodes.size();
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	fixture.Frames(1);
	ImGuiWindow const* confirmation = ImGui::FindWindowByName("Delete###ContentBrowserDelete");
	REQUIRE(confirmation != nullptr);
	CHECK(confirmation->Active);
	// The browser's modal has the keyboard: the browser stays the keyboard window.
	CHECK(GetWindowName(fixture.KeyboardWindow) == ContentBrowserPanel::WindowName);
	CHECK(history.GetUndoCount() == undo);
	CHECK(AssetManager::GetAsset<VisualScriptAsset>(fixture.Player)->GetDocument().Graph.Nodes.size() == nodes);
	// The modal takes no key, so the browser's own routes score nothing while it is up (imgui.cpp:8031, :10402-10412): Delete
	// and Ctrl+Backspace, which the browser claims, still run nothing on the graph's selection behind it (the pasted Get).
	REQUIRE(!panel.GetState().SelectedNodes.empty());
	for (ImGuiKeyChord const claimed : {ImGuiKeyChord(ImGuiKey_Delete), ImGuiKeyChord(ImGuiMod_Ctrl | ImGuiKey_Backspace)})
	{
		chord(claimed);
		CHECK(confirmation->Active);
		CHECK(history.GetUndoCount() == undo);
		CHECK(AssetManager::GetAsset<VisualScriptAsset>(fixture.Player)->GetDocument().Graph.Nodes.size() == nodes);
		CHECK(fixture.Context.GetScene().GetEntityCount() == entities);
	}
	// Nor does a chord the browser does not claim reach the Edit menu's target while the modal is open (ruling S-R3.1):
	// Ctrl+A leaves the graph's selection as it was.
	std::vector<uint32_t> const selectedBehind = panel.GetState().SelectedNodes;
	chord(ImGuiMod_Ctrl | ImGuiKey_A);
	CHECK(panel.GetState().SelectedNodes == selectedBehind);
	CHECK(history.GetUndoCount() == undo);
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(2);
	CHECK_FALSE(confirmation->Active);
	CHECK(AssetManager::IsValid(fixture.Door));
	REQUIRE(GetWindowName(fixture.KeyboardWindow) == ContentBrowserPanel::WindowName);
	// The Edit menu has ImGui's keyboard: Delete shows no key there, and pressing it runs nothing either.
	EditMenuRecord const browserMenu = fixture.DrawEditMenu(
		[&]
		{
			chord(ImGuiKey_Delete);
			chord(ImGuiMod_Ctrl | ImGuiKey_Backspace);
		});
	CHECK(history.GetUndoCount() == undo);
	CHECK(AssetManager::GetAsset<VisualScriptAsset>(fixture.Player)->GetDocument().Graph.Nodes.size() == nodes);
	CHECK(fixture.Context.GetScene().GetEntityCount() == entities);
	CHECK(browserMenu.KeyboardWindow == ContentBrowserPanel::WindowName);
	CHECK(EditorsFixture::GetShortcutText(browserMenu, "Delete").empty());
	CHECK(EditorsFixture::GetShortcutText(browserMenu, "Cut") == "Ctrl+X");
	CHECK(EditorsFixture::GetShortcutText(browserMenu, "Duplicate") == "Ctrl+D");
	CHECK(EditorsFixture::GetShortcutText(browserMenu, "Select All") == "Ctrl+A");
	fixture.Focus("Console");
	CHECK(EditorsFixture::GetShortcutText(fixture.DrawEditMenu(), "Delete") == "Delete");
}

TEST_CASE("VisualScriptEditor: no editing key acts behind a modal, the content browser's delete confirmation or My Script's")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	// A step to undo (Ctrl+Z would remove the crate) and a selected entity besides the graph's selected node.
	EntityCreateInfo info;
	info.Name = "Crate";
	Result<UUID> const crate = fixture.Operations.CreateEntity(info);
	REQUIRE(crate.IsOk());
	std::array<UUID, 1> const selection = {crate.GetValue()};
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	VisualScriptEditorPanel& panel = fixture.GetPanel(fixture.Player);
	CommandHistory const& history = fixture.Context.GetHistory();
	fixture.Click(Testing::CanvasPoint(panel.GetModel(), 2, glm::vec2(0.5f, 0.06f)));
	fixture.Frames(1);
	REQUIRE(panel.GetState().SelectedNodes == std::vector<uint32_t>{2});
	REQUIRE(fixture.Operations.Select(selection).IsOk());

	// Each key behind the open modal changes nothing: no step is done or undone, the graph keeps its document and its
	// selection, the scene its entities and their selection, and the modal stays open.
	auto const pressEach = [&](std::function<bool()> const& isModalOpen)
	{
		REQUIRE(history.CanUndo());
		size_t const undo = history.GetUndoCount();
		Ref<VisualScriptDocument const> const document = AssetManager::GetAsset<VisualScriptAsset>(fixture.Player)->GetSharedDocument();
		std::vector<uint32_t> const nodes = panel.GetState().SelectedNodes;
		size_t const entities = fixture.Context.GetScene().GetEntityCount();
		std::vector<UUID> const entitySelection = fixture.Context.GetSelection().GetEntities();
		for (ImGuiKeyChord const keys : {ImGuiKeyChord(ImGuiMod_Ctrl | ImGuiKey_A), ImGuiKeyChord(ImGuiMod_Ctrl | ImGuiKey_X),
		                                 ImGuiKeyChord(ImGuiMod_Ctrl | ImGuiKey_D), ImGuiKeyChord(ImGuiMod_Ctrl | ImGuiKey_Z),
		                                 ImGuiKeyChord(ImGuiKey_Delete), ImGuiKeyChord(ImGuiMod_Ctrl | ImGuiKey_Backspace)})
		{
			CAPTURE(keys);
			fixture.Gui.Chord(keys, fixture.Drawer());
			fixture.Frames(1);
			CHECK(isModalOpen());
			CHECK(ImGui::GetTopMostPopupModal() != nullptr);
			CHECK(history.GetUndoCount() == undo);
			CHECK(history.CanUndo());
			CHECK(AssetManager::GetAsset<VisualScriptAsset>(fixture.Player)->GetSharedDocument() == document);
			CHECK(panel.GetState().SelectedNodes == nodes);
			CHECK(fixture.Context.GetScene().GetEntityCount() == entities);
			CHECK(fixture.Context.GetScene().HasEntity(crate.GetValue()));
			CHECK(fixture.Context.GetSelection().GetEntities() == entitySelection);
		}
	};

	// The content browser's delete confirmation of a graph, the Player graph the Edit menu's target behind it.
	fixture.ContentBrowser.SetFolder("Scripts");
	fixture.Context.SelectAsset(fixture.Door);
	fixture.Focus(ContentBrowserPanel::WindowName);
	REQUIRE(fixture.Editors.GetEditTarget() == &panel);
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	fixture.Frames(1);
	ImGuiWindow const* confirmation = ImGui::FindWindowByName("Delete###ContentBrowserDelete");
	REQUIRE(confirmation != nullptr);
	REQUIRE(confirmation->Active);
	pressEach(
		[confirmation]
		{
			return confirmation->Active;
		});
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(2);
	REQUIRE_FALSE(confirmation->Active);
	CHECK(AssetManager::IsValid(fixture.Door));
	// Closed, the keys act again: Ctrl+A selects every node of the graph.
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	fixture.Frames(1);
	CHECK(panel.GetState().SelectedNodes.size() == 4);

	// My Script's delete confirmation of the variable Speed, in the graph window that has the keyboard.
	std::optional<std::pair<glm::vec2, glm::vec2>> const row = panel.GetSidebar().FindRow({VisualScriptElementKind::Variable, 100});
	REQUIRE(row.has_value());
	fixture.Click((row->first + row->second) * 0.5f);
	fixture.Frames(1);
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	fixture.Frames(2);
	REQUIRE(panel.GetSidebar().GetPendingDelete().has_value());
	REQUIRE(ImGui::GetTopMostPopupModal() != nullptr);
	REQUIRE(fixture.Editors.GetEditTarget() == &panel);
	REQUIRE(fixture.Operations.Select(selection).IsOk());
	pressEach(
		[&panel]
		{
			return panel.GetSidebar().GetPendingDelete().has_value();
		});
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(2);
	CHECK(ImGui::GetTopMostPopupModal() == nullptr);
	CHECK_FALSE(panel.GetSidebar().GetPendingDelete().has_value());
	// Closed, Ctrl+Z undoes again: the crate goes.
	size_t const undo = history.GetUndoCount();
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_Z, fixture.Drawer());
	fixture.Frames(1);
	CHECK(history.GetUndoCount() == undo - 1);
	CHECK_FALSE(fixture.Context.GetScene().HasEntity(crate.GetValue()));
}

TEST_CASE("VisualScriptEditor: with the scene as the Edit menu's target the editing keys act on the selected entities")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	EntityCreateInfo info;
	info.Name = "Crate";
	Result<UUID> const crate = fixture.Operations.CreateEntity(info);
	REQUIRE(crate.IsOk());
	std::array<UUID, 1> const selection = {crate.GetValue()};
	info.Name = "Barrel";
	Result<UUID> const barrel = fixture.Operations.CreateEntity(info);
	REQUIRE(barrel.IsOk());
	fixture.OpenGraph(fixture.Player);
	fixture.Click(fixture.EmptyPoint(fixture.Player));
	REQUIRE(fixture.Editors.GetEditTarget() != nullptr);
	CommandHistory const& history = fixture.Context.GetHistory();

	// The hierarchy has the keyboard: the scene is the target, and its items show their keys.
	fixture.Focus("Scene Hierarchy");
	REQUIRE(fixture.Editors.GetEditTarget() == nullptr);
	REQUIRE(fixture.Operations.Select(selection).IsOk());
	EditMenuRecord const hierarchyMenu = fixture.DrawEditMenu();
	CHECK(hierarchyMenu.KeyboardWindow == "Scene Hierarchy");
	std::vector<EditorMenuItemRecord> const expected = {
		{"Duplicate", "Ctrl+D", true},
		{"Delete", "Delete", true},
		{"Select All", "Ctrl+A", true},
		{"Deselect All", "", true},
	};
	CHECK(hierarchyMenu.Items == expected);

	// The content browser takes the keyboard and leaves the scene the target: the scene's Delete shows no key then.
	fixture.Focus(ContentBrowserPanel::WindowName);
	REQUIRE(fixture.Editors.GetEditTarget() == nullptr);
	EditMenuRecord const browserMenu = fixture.DrawEditMenu();
	CHECK(browserMenu.KeyboardWindow == ContentBrowserPanel::WindowName);
	CHECK(EditorsFixture::GetShortcutText(browserMenu, "Delete").empty());
	CHECK(EditorsFixture::GetShortcutText(browserMenu, "Duplicate") == "Ctrl+D");

	// Ctrl+A selects every entity, Ctrl+D duplicates the selection, Delete deletes it; the graph is left alone.
	fixture.Focus("Scene Hierarchy");
	size_t const nodes = AssetManager::GetAsset<VisualScriptAsset>(fixture.Player)->GetDocument().Graph.Nodes.size();
	size_t const entities = fixture.Context.GetScene().GetEntityCount();
	REQUIRE(entities == 2);
	REQUIRE(fixture.Operations.Select(selection).IsOk());
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	CHECK(fixture.Context.GetSelection().GetEntities().size() == entities);
	CHECK(fixture.Context.GetSelection().Contains(crate.GetValue()));
	CHECK(fixture.Context.GetSelection().Contains(barrel.GetValue()));
	REQUIRE(fixture.Operations.Select(selection).IsOk());
	size_t const undo = history.GetUndoCount();
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_D, fixture.Drawer());
	CHECK(history.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetScene().GetEntityCount() == entities + 1);
	REQUIRE(fixture.Operations.Select(selection).IsOk());
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	CHECK(history.GetUndoCount() == undo + 2);
	CHECK_FALSE(fixture.Context.GetScene().HasEntity(crate.GetValue()));
	CHECK(AssetManager::GetAsset<VisualScriptAsset>(fixture.Player)->GetDocument().Graph.Nodes.size() == nodes);
}

TEST_CASE("VisualScriptEditor: the keyboard window stays the one before a menu bar's menu, the side bars and no window took the keyboard")
{
	Testing::HeadlessImGui gui;
	ImGuiWindow const* keyboard = nullptr;
	bool openMenu = false;
	bool closeMenu = false;
	bool menuOpen = false;
	char const* focus = nullptr;
	bool clearFocus = false;
	bool drawConsole = true;
	// As EditorLayer draws a frame: the keyboard window first, then the main menu bar with an Edit menu (opened as
	// editor.entity-menu opens its menu), the toolbar and a window.
	auto const draw = [&]
	{
		keyboard = FindKeyboardWindow(GImGui->NavWindow, keyboard);
		if (ImGui::BeginMainMenuBar())
		{
			if (std::exchange(openMenu, false))
			{
				ImGui::OpenPopup("Edit");
			}
			menuOpen = ImGui::BeginMenu("Edit");
			if (menuOpen)
			{
				ImGui::MenuItem("Delete", "Delete");
				if (std::exchange(closeMenu, false))
				{
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}
		if (ImGui::BeginViewportSideBar(EditorToolbarWindowName, ImGui::GetMainViewport(), ImGuiDir_Up, 30.0f,
		                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::TextUnformatted("Play");
		}
		ImGui::End();
		ImGui::SetNextWindowPos(ImVec2(100.0f, 200.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f), ImGuiCond_FirstUseEver);
		if (drawConsole)
		{
			ImGui::Begin("Console", nullptr, ImGuiWindowFlags_NoSavedSettings);
			ImGui::End();
		}
		if (char const* const name = std::exchange(focus, nullptr))
		{
			ImGui::SetWindowFocus(name);
		}
		if (std::exchange(clearFocus, false))
		{
			ImGui::FocusWindow(nullptr);
		}
	};
	auto const frames = [&gui, &draw](int count)
	{
		for (int i = 0; i < count; i++)
		{
			gui.Frame(draw);
		}
	};
	frames(2);
	focus = "Console";
	frames(2);
	REQUIRE(GetWindowName(keyboard) == "Console");

	// The menu bar's menu has ImGui's keyboard while it is open and after it closed: the console stays the keyboard window
	// throughout.
	openMenu = true;
	frames(2);
	REQUIRE(menuOpen);
	REQUIRE(GImGui->NavWindow != nullptr);
	CHECK(std::string_view(GImGui->NavWindow->RootWindow->Name) != "Console");
	CHECK(GetWindowName(keyboard) == "Console");
	closeMenu = true;
	frames(2);
	REQUIRE_FALSE(menuOpen);
	REQUIRE(GImGui->NavWindow != nullptr);
	CHECK(std::string_view(GImGui->NavWindow->RootWindow->Name) != "Console");
	CHECK(GetWindowName(keyboard) == "Console");

	// The toolbar (its buttons take the focus when clicked) and no window at all leave it too.
	focus = EditorToolbarWindowName;
	frames(2);
	REQUIRE(GImGui->NavWindow != nullptr);
	REQUIRE(std::string_view(GImGui->NavWindow->Name) == EditorToolbarWindowName);
	CHECK(GetWindowName(keyboard) == "Console");
	clearFocus = true;
	frames(2);
	REQUIRE(GImGui->NavWindow == nullptr);
	CHECK(GetWindowName(keyboard) == "Console");

	// A window closed while the menu has ImGui's keyboard has the keyboard no more, nor after the menu closed.
	focus = "Console";
	frames(2);
	REQUIRE(GetWindowName(keyboard) == "Console");
	openMenu = true;
	frames(2);
	REQUIRE(menuOpen);
	drawConsole = false;
	frames(2);
	CHECK(keyboard == nullptr);
	closeMenu = true;
	frames(2);
	REQUIRE_FALSE(menuOpen);
	CHECK(keyboard == nullptr);
}

TEST_CASE("VisualScriptEditor: an Edit item's shortcut text is its key's unless the window that has the keyboard claims the key")
{
	std::span<ImGuiKeyChord const> const none;
	CHECK(GetEditShortcutText(ImGuiMod_Ctrl | ImGuiKey_X, none) == "Ctrl+X");
	CHECK(GetEditShortcutText(ImGuiMod_Ctrl | ImGuiKey_C, none) == "Ctrl+C");
	CHECK(GetEditShortcutText(ImGuiMod_Ctrl | ImGuiKey_V, none) == "Ctrl+V");
	CHECK(GetEditShortcutText(ImGuiMod_Ctrl | ImGuiKey_D, none) == "Ctrl+D");
	CHECK(GetEditShortcutText(ImGuiKey_Delete, none) == "Delete");
	CHECK(GetEditShortcutText(ImGuiMod_Ctrl | ImGuiKey_A, none) == "Ctrl+A");

	std::span<ImGuiKeyChord const> const browser = ContentBrowserPanel::FocusedChords;
	CHECK(GetEditShortcutText(ImGuiMod_Ctrl | ImGuiKey_X, browser) == "Ctrl+X");
	CHECK(GetEditShortcutText(ImGuiMod_Ctrl | ImGuiKey_C, browser) == "Ctrl+C");
	CHECK(GetEditShortcutText(ImGuiMod_Ctrl | ImGuiKey_V, browser) == "Ctrl+V");
	CHECK(GetEditShortcutText(ImGuiMod_Ctrl | ImGuiKey_D, browser) == "Ctrl+D");
	CHECK(GetEditShortcutText(ImGuiKey_Delete, browser).empty());
	CHECK(GetEditShortcutText(ImGuiMod_Ctrl | ImGuiKey_A, browser) == "Ctrl+A");
	CHECK(GetEditShortcutText(ImGuiMod_Ctrl | ImGuiKey_Backspace, browser).empty());
	CHECK(GetEditShortcutText(ImGuiKey_F2, browser).empty());
	// A claimed key with other modifiers is another chord.
	CHECK(GetEditShortcutText(ImGuiMod_Shift | ImGuiKey_Delete, browser) == "Shift+Delete");

	// Only the content browser claims editing chords; without a keyboard window, none are.
	CHECK(FindClaimedEditingChords(nullptr).empty());
}

TEST_CASE("VisualScriptEditor: undo and redo say what they did in the graph window that is the Edit menu's target")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	EntityCreateInfo info;
	info.Name = "Crate";
	REQUIRE(fixture.Operations.CreateEntity(info).IsOk());
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	fixture.Click(fixture.EmptyPoint(fixture.Player));
	REQUIRE(fixture.Editors.GetEditTarget() != nullptr);

	CommandHistory const& history = fixture.Context.GetHistory();
	std::string const description = history.GetUndoDescription();
	AssetHandle const edited = history.GetUndoEditedAsset();
	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Editors.NotifyHistoryStep(true, description, edited);
	fixture.Frames(1);
	std::vector<std::string> const toasts = fixture.GetPanel(fixture.Player).GetState().Toasts;
	REQUIRE_FALSE(toasts.empty());
	CHECK(toasts.back() == "Undone: " + description);
}

TEST_CASE("VisualScriptEditor: a history toast's Show for a step of another script opens that script's window, or brings it to the "
          "front")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	// A step of Door's, whose window is not open.
	VisualScriptDocument door = MakeDocument();
	door.Description = "Opens";
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Door, door).IsOk());
	CommandHistory const& history = fixture.Context.GetHistory();
	REQUIRE(history.GetUndoEditedAsset() == fixture.Door);
	std::string const description = history.GetUndoDescription();
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	fixture.Click(fixture.EmptyPoint(fixture.Player));
	REQUIRE(fixture.Editors.GetEditTarget() == &fixture.GetPanel(fixture.Player));

	// Undone from Player's window: Show opens Door's window, in front.
	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Editors.NotifyHistoryStep(true, description, fixture.Door);
	fixture.Frames(2);
	std::optional<glm::vec2> show = fixture.FindToastButton(fixture.Player, "Show");
	REQUIRE(show.has_value());
	fixture.Click(*show);
	fixture.Frames(2);
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Player, fixture.Door});
	CHECK(GetFocusedRoot() == FindGraphWindow(fixture.Door));
	CHECK(fixture.GetPanel(fixture.Door).GetState().Visible);

	// Redone from Player's window again, with Door's window behind it: Show brings Door's to the front.
	fixture.OpenGraph(fixture.Player);
	REQUIRE_FALSE(fixture.GetPanel(fixture.Door).GetState().Visible);
	fixture.Click(fixture.EmptyPoint(fixture.Player));
	REQUIRE(fixture.Editors.GetEditTarget() == &fixture.GetPanel(fixture.Player));
	REQUIRE(fixture.Operations.Redo().IsOk());
	fixture.Editors.NotifyHistoryStep(false, description, fixture.Door);
	fixture.Frames(2);
	show = fixture.FindToastButton(fixture.Player, "Show");
	REQUIRE(show.has_value());
	fixture.Click(*show);
	fixture.Frames(2);
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Player, fixture.Door});
	CHECK(GetFocusedRoot() == FindGraphWindow(fixture.Door));
	CHECK(fixture.GetPanel(fixture.Door).GetState().Visible);
}

TEST_CASE("VisualScriptEditor: a graph window's requests for another script open that script's window")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	// Player handles the custom event Hit, which Door sends.
	VisualScriptDocument player = MakeDocument();
	player.Graph.Nodes.push_back(MakeNode(5, "Event.Custom", glm::vec2(0.0f, 480.0f), Json::object({{"Name", "Hit"}})));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Player, player).IsOk());
	VisualScriptDocument door = MakeDocument();
	door.Graph.Nodes.push_back(MakeNode(6, "Event.Send", glm::vec2(400.0f, 480.0f), Json::object({{"Name", "Hit"}})));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Door, door).IsOk());
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	VisualScriptEditorPanel& panel = fixture.GetPanel(fixture.Player);

	// The node menu's Find References lists the senders in every script; a click on Door's opens Door's window on it.
	fixture.Click(Testing::CanvasPoint(panel.GetModel(), 5, glm::vec2(0.5f, 0.06f)), ImGuiMouseButton_Right);
	fixture.Frames(1);
	std::optional<glm::vec2> const find = fixture.FindPopupItem("Find References");
	REQUIRE(find.has_value());
	fixture.Click(*find);
	fixture.Frames(2);
	REQUIRE(panel.GetState().Drawer == VisualScriptDrawerMode::Find);
	std::span<VisualScriptDrawerFindRow const> const rows = panel.GetDrawer().GetFindRows();
	auto const sender = std::find_if(rows.begin(), rows.end(),
	                                 [&fixture](VisualScriptDrawerFindRow const& row)
	                                 {
										 return row.Graph == fixture.Door && row.Error.empty();
									 });
	REQUIRE(sender != rows.end());
	std::optional<std::pair<glm::vec2, glm::vec2>> const row = panel.GetDrawer().FindFindRow(static_cast<size_t>(sender - rows.begin()));
	REQUIRE(row.has_value());
	fixture.Click((row->first + row->second) * 0.5f);
	fixture.Frames(3);
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Player, fixture.Door});
	VisualScriptEditorState const opened = fixture.GetPanel(fixture.Door).GetState();
	CHECK(opened.Visible);
	CHECK(opened.SelectedNodes == std::vector<uint32_t>{6});
}

TEST_CASE("VisualScriptEditor: the services given to the window manager reach its windows, also those open already")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	fixture.Semantics.SetAvailable(false);
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	VisualScriptEditorPanel& panel = fixture.GetPanel(fixture.Player);
	fixture.Frames(1);
	REQUIRE(panel.GetState().Notices.front() == VisualScriptNotice::Degraded);
	// The fixture's services have no console callback: the degraded notice offers none.
	CHECK_FALSE(panel.GetItemRect(VisualScriptPanelItem::ShowConsole).has_value());

	int shown = 0;
	VisualScriptEditorServices services = fixture.Editors.GetServices();
	services.ShowConsole = [&shown]
	{
		shown++;
	};
	fixture.Editors.SetServices(std::move(services));
	fixture.Frames(2);
	std::optional<VisualScriptRect> const button = panel.GetItemRect(VisualScriptPanelItem::ShowConsole);
	REQUIRE(button.has_value());
	fixture.Click(button->GetCenter());
	CHECK(shown == 1);
}

TEST_CASE("VisualScriptEditor: the window manager checks what it opens, and opens a file it cannot read on its card")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	VisualScriptOpenRequest request;
	request.Graph = AssetHandle(UUID(55555));
	CHECK_FALSE(fixture.Editors.Open(request).IsOk());
	request.Graph = fixture.Context.GetProject()->GetSettings().StartScene;
	CHECK_FALSE(fixture.Editors.Open(request).IsOk());

	request.Graph = fixture.Player;
	request.Function = 7;
	CHECK_FALSE(fixture.Editors.Open(request).IsOk());
	request.Function.reset();
	request.Select = {2, 99};
	CHECK_FALSE(fixture.Editors.Open(request).IsOk());
	request.Select = {2};
	request.ContextEntity = UUID(424242);
	CHECK_FALSE(fixture.Editors.Open(request).IsOk());
	CHECK(fixture.Editors.Describe().empty());
	request.ContextEntity = UUID::Invalid();
	Result<VisualScriptEditorState> const opened = fixture.Editors.Open(request);
	REQUIRE(opened.IsOk());
	CHECK(opened.GetValue().Graph == fixture.Player);
	CHECK_FALSE(opened.GetValue().Visible);
	fixture.Frames(2);
	CHECK(fixture.GetPanel(fixture.Player).GetState().SelectedNodes == std::vector<uint32_t>{2});
	fixture.CheckDrawingChangesNothing();

	// A file the loader refuses opens on its card; what the request names in it is left out.
	std::filesystem::path const file = AssetManager::GetAssetDirectory() / "Scripts" / "Door.sgraph";
	REQUIRE(FileSystem::WriteTextFile(file, "not a visual script").IsOk());
	REQUIRE(fixture.Operations.RefreshAssets().IsOk());
	request.Graph = fixture.Door;
	request.Select = {99};
	request.Function = 7;
	REQUIRE(fixture.Editors.Open(request).IsOk());
	fixture.Frames(2);
	CHECK(fixture.GetPanel(fixture.Door).GetState().Card == VisualScriptCard::Unloadable);
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Player, fixture.Door});
}

TEST_CASE("VisualScriptEditor: the View menu lists the open graphs, closes one when it is unchecked, and closes them all")
{
	EditorsFixture fixture;
	fixture.CheckDrawingChangesNothing();
	fixture.OpenGraph(fixture.Player);
	fixture.OpenGraph(fixture.Door);
	fixture.CheckDrawingChangesNothing();
	bool openMenu = true;
	fixture.Probe = [&fixture, &openMenu]
	{
		if (std::exchange(openMenu, false))
		{
			ImGui::OpenPopup("View");
		}
		if (ImGui::BeginPopup("View"))
		{
			fixture.Editors.DrawViewMenuItems();
			ImGui::EndPopup();
		}
	};
	fixture.Frames(2);
	std::optional<glm::vec2> const submenu = fixture.FindPopupItem("Visual Scripts");
	REQUIRE(submenu.has_value());
	fixture.Frames(10);
	REQUIRE(GImGui->OpenPopupStack.Size == 2);
	std::string const doorItem = "Door###" + fixture.Door.ToString();
	std::optional<glm::vec2> const door = fixture.FindPopupItem(doorItem.c_str());
	REQUIRE(door.has_value());
	fixture.Click(*door);
	fixture.Frames(1);
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Player});

	openMenu = true;
	fixture.Frames(2);
	REQUIRE(fixture.FindPopupItem("Visual Scripts").has_value());
	fixture.Frames(10);
	std::optional<glm::vec2> const closeAll = fixture.FindPopupItem("Close All");
	REQUIRE(closeAll.has_value());
	fixture.Click(*closeAll);
	fixture.Frames(1);
	CHECK(fixture.GetOpenGraphs().empty());
	fixture.Probe = {};
}

TEST_CASE("VisualScriptEditor: opening a node that names another graph opens its window on the element, and Back and Forward cross "
          "between the two windows")
{
	EditorsFixture fixture;
	// Door has the public variable Health (50); Player reads it on another entity (node 60).
	VisualScriptDocument door = MakeDocument();
	VisualScriptVariable health;
	health.ID = 50;
	health.Name = "Health";
	health.Type = "float";
	health.Public = true;
	door.Variables.push_back(health);
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Door, door).IsOk());
	VisualScriptDocument player = MakeDocument();
	player.Graph.Nodes.push_back(
		MakeNode(60, "Variable.Get", glm::vec2(400.0f, 640.0f), Json::object({{"Variable", 50}, {"Graph", fixture.Door.ToString()}})));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Player, player).IsOk());
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	fixture.Gui.Frame(fixture.Drawer(), 1.0f);
	VisualScriptEditorPanel& callerWindow = fixture.GetPanel(fixture.Player);
	CHECK_FALSE(callerWindow.GetState().CanGoBack);
	VisualScriptViewState const callerView = callerWindow.GetState().View;

	// A double click on the remote Get's header: Door's window, in front, with Health selected in My Script.
	glm::vec2 const header = Testing::CanvasPoint(callerWindow.GetModel(), 60, glm::vec2(0.5f, 0.06f));
	fixture.Gui.MouseClick(ImVec2(header.x, header.y), ImGuiMouseButton_Left, fixture.Drawer(), 2);
	fixture.Frames(3);
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Player, fixture.Door});
	CHECK(GetFocusedRoot() == FindGraphWindow(fixture.Door));
	VisualScriptEditorPanel& doorWindow = fixture.GetPanel(fixture.Door);
	CHECK(doorWindow.GetPaneShared().Element ==
	      std::optional<VisualScriptElement>(VisualScriptElement{VisualScriptElementKind::Variable, 50}));
	CHECK(doorWindow.GetState().CanGoBack);
	CHECK_FALSE(doorWindow.GetState().CanGoForward);

	// Back in Door's window: Player's window in front again, at the view it had.
	REQUIRE(doorWindow.RequestAction(VisualScriptEditorAction::Back));
	fixture.Frames(3);
	CHECK(GetFocusedRoot() == FindGraphWindow(fixture.Player));
	CHECK(callerWindow.GetState().View.Center == callerView.Center);
	CHECK(callerWindow.GetState().View.Zoom == callerView.Zoom);
	CHECK(callerWindow.GetState().CanGoForward);
	CHECK_FALSE(doorWindow.GetState().CanGoBack);

	// Forward in Player's window: Door's window again, whose Back returns once more.
	REQUIRE(callerWindow.RequestAction(VisualScriptEditorAction::Forward));
	fixture.Frames(3);
	CHECK(GetFocusedRoot() == FindGraphWindow(fixture.Door));
	CHECK(doorWindow.GetState().CanGoBack);
	CHECK_FALSE(callerWindow.GetState().CanGoForward);

	// Back opens the caller's window when it was closed meanwhile.
	fixture.Editors.Close(fixture.Player);
	fixture.Frames(2);
	REQUIRE(doorWindow.RequestAction(VisualScriptEditorAction::Back));
	fixture.Frames(3);
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Door, fixture.Player});
	CHECK(GetFocusedRoot() == FindGraphWindow(fixture.Player));
	CHECK(fixture.GetPanel(fixture.Player).GetState().View.Center == callerView.Center);
}

TEST_CASE("VisualScriptEditor: a window analyzes again when a graph its analysis read changes what it read, and only then")
{
	EditorsFixture fixture;
	// Motion, a library whose function Clamp01 Player calls.
	Result<AssetHandle> const created = fixture.Operations.CreateVisualScript("Scripts/Motion.sgraph", VisualScriptTemplate::Library);
	REQUIRE(created.IsOk());
	AssetHandle const motion = created.GetValue();
	VisualScriptDocument library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	VisualScriptFunction clamp;
	clamp.ID = 7;
	clamp.Name = "Clamp01";
	clamp.Pure = true;
	clamp.Graph.Nodes = {MakeNode(8, "Function.Entry", glm::vec2(0.0f)), MakeNode(9, "Function.Return", glm::vec2(400.0f, 0.0f))};
	library.Functions = {clamp};
	REQUIRE(fixture.Operations.SetVisualScriptDocument(motion, library).IsOk());
	VisualScriptDocument player = MakeDocument();
	player.Graph.Nodes.push_back(
		MakeNode(60, "Function.Call", glm::vec2(400.0f, 640.0f), Json::object({{"Function", 7}, {"Graph", motion.ToString()}})));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Player, player).IsOk());
	// The analyses read what the engine would: Motion's document as its asset holds it.
	auto const readMotion = [&fixture, motion]
	{
		fixture.Semantics.SetGraphs({{motion, AssetManager::GetAsset<VisualScriptAsset>(motion)->GetSharedDocument()}});
	};
	readMotion();
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	uint64_t const analyzed = fixture.Semantics.GetAnalyzeCallCount();

	// A layout-only commit in Motion, and a reload that gives an equal document: no analysis.
	VisualScriptDocument moved = AssetManager::GetAsset<VisualScriptAsset>(motion)->GetDocument();
	moved.Functions[0].Graph.Nodes[1].Position = glm::vec2(640.0f, 80.0f);
	REQUIRE(fixture.Operations.SetVisualScriptDocument(motion, moved).IsOk());
	readMotion();
	fixture.Frames(3);
	CHECK(fixture.Semantics.GetAnalyzeCallCount() == analyzed);
	REQUIRE(AssetManager::ReloadAsset(motion).IsOk());
	readMotion();
	fixture.Frames(3);
	CHECK(fixture.Semantics.GetAnalyzeCallCount() == analyzed);

	// A commit that changes Clamp01's signature: Player's window analyzes again within a frame, once.
	VisualScriptDocument changed = AssetManager::GetAsset<VisualScriptAsset>(motion)->GetDocument();
	changed.Functions[0].Inputs = {{"Value", "float"}};
	REQUIRE(fixture.Operations.SetVisualScriptDocument(motion, changed).IsOk());
	readMotion();
	fixture.Frames(1);
	CHECK(fixture.Semantics.GetAnalyzeCallCount() == analyzed + 1);
	fixture.Frames(3);
	CHECK(fixture.Semantics.GetAnalyzeCallCount() == analyzed + 1);
}

TEST_CASE("VisualScriptEditor: a function library's window opens on its first function, and an empty one offers to add one")
{
	EditorsFixture fixture;
	Result<AssetHandle> const created = fixture.Operations.CreateVisualScript("Scripts/Motion.sgraph", VisualScriptTemplate::Library);
	REQUIRE(created.IsOk());
	AssetHandle const motion = created.GetValue();
	fixture.Frames(2);

	// Empty: the card over its empty event graph, whose button adds a function in one step and shows it.
	fixture.OpenGraph(motion);
	fixture.CheckDrawingChangesNothing();
	VisualScriptEditorPanel& window = fixture.GetPanel(motion);
	REQUIRE(window.GetState().Card == VisualScriptCard::EmptyLibrary);
	std::optional<VisualScriptRect> const add = window.GetItemRect(VisualScriptPanelItem::AddFunction);
	REQUIRE(add.has_value());
	size_t const undo = fixture.Context.GetHistory().GetUndoCount();
	fixture.Click(add->GetCenter());
	fixture.Frames(2);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == undo + 1);
	Ref<VisualScriptDocument const> const added = AssetManager::GetAsset<VisualScriptAsset>(motion)->GetSharedDocument();
	VisualScriptDocument const& document = *added;
	REQUIRE(document.Functions.size() == 1);
	CHECK(window.GetState().Function == document.Functions[0].ID);
	CHECK(window.GetState().Card == VisualScriptCard::None);

	// Opened again, it shows its first function.
	fixture.Editors.Close(motion);
	fixture.Settings.ForgetGraph(motion);
	fixture.Frames(1);
	VisualScriptOpenRequest request;
	request.Graph = motion;
	Result<VisualScriptEditorState> const opened = fixture.Editors.Open(request);
	REQUIRE(opened.IsOk());
	CHECK(opened.GetValue().Function == document.Functions[0].ID);
}

TEST_CASE("VisualScriptEditor: a library window shown on its empty event graph offers its first function")
{
	EditorsFixture fixture;
	Result<AssetHandle> const created = fixture.Operations.CreateVisualScript("Scripts/Motion.sgraph", VisualScriptTemplate::Library);
	REQUIRE(created.IsOk());
	AssetHandle const motion = created.GetValue();
	VisualScriptDocument library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	VisualScriptFunction clamp;
	clamp.ID = 7;
	clamp.Name = "Clamp01";
	clamp.Graph.Nodes = {MakeNode(8, "Function.Entry", glm::vec2(0.0f))};
	library.Functions = {clamp};
	REQUIRE(fixture.Operations.SetVisualScriptDocument(motion, library).IsOk());
	VisualScriptOpenRequest request;
	request.Graph = motion;
	request.Function = 0;
	REQUIRE(fixture.Editors.Open(request).IsOk());
	fixture.CheckDrawingChangesNothing();
	VisualScriptEditorPanel& window = fixture.GetPanel(motion);
	CHECK(window.GetState().Function == 0);
	REQUIRE(window.GetState().Card == VisualScriptCard::EmptyLibrary);
	CHECK_FALSE(window.GetItemRect(VisualScriptPanelItem::AddFunction).has_value());
	std::optional<VisualScriptRect> const open = window.GetItemRect(VisualScriptPanelItem::OpenFunction);
	REQUIRE(open.has_value());
	size_t const undo = fixture.Context.GetHistory().GetUndoCount();
	fixture.Click(open->GetCenter());
	fixture.Frames(2);
	CHECK(window.GetState().Function == 7);
	CHECK(window.GetState().Card == VisualScriptCard::None);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == undo);
}

TEST_CASE("VisualScriptEditor: Forward to a function another window's Back left, deleted since, shows that graph's event graph")
{
	EditorsFixture fixture;
	Result<AssetHandle> const created = fixture.Operations.CreateVisualScript("Scripts/Motion.sgraph", VisualScriptTemplate::Library);
	REQUIRE(created.IsOk());
	AssetHandle const motion = created.GetValue();
	VisualScriptDocument library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	VisualScriptFunction clamp;
	clamp.ID = 7;
	clamp.Name = "Clamp01";
	clamp.Graph.Nodes = {MakeNode(8, "Function.Entry", glm::vec2(0.0f))};
	VisualScriptFunction push = clamp;
	push.ID = 9;
	push.Name = "Push";
	push.Graph.Nodes = {MakeNode(10, "Function.Entry", glm::vec2(0.0f))};
	library.Functions = {clamp, push};
	REQUIRE(fixture.Operations.SetVisualScriptDocument(motion, library).IsOk());
	VisualScriptDocument player = MakeDocument();
	player.Graph.Nodes.push_back(
		MakeNode(60, "Function.Call", glm::vec2(400.0f, 640.0f), Json::object({{"Function", 9}, {"Graph", motion.ToString()}})));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Player, player).IsOk());
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	fixture.Gui.Frame(fixture.Drawer(), 1.0f);
	VisualScriptEditorPanel& callerWindow = fixture.GetPanel(fixture.Player);

	// The call's header opens Push in Motion's window; Back returns to Player's.
	glm::vec2 const header = Testing::CanvasPoint(callerWindow.GetModel(), 60, glm::vec2(0.5f, 0.06f));
	fixture.Gui.MouseClick(ImVec2(header.x, header.y), ImGuiMouseButton_Left, fixture.Drawer(), 2);
	fixture.Frames(3);
	VisualScriptEditorPanel& motionWindow = fixture.GetPanel(motion);
	REQUIRE(motionWindow.GetState().Function == 9);
	REQUIRE(motionWindow.RequestAction(VisualScriptEditorAction::Back));
	fixture.Frames(3);
	REQUIRE(GetFocusedRoot() == FindGraphWindow(fixture.Player));
	REQUIRE(callerWindow.GetState().CanGoForward);

	// Push goes (an agent's edit); Forward still reaches Motion's window, on its event graph.
	VisualScriptDocument without = library;
	without.Functions.pop_back();
	REQUIRE(fixture.Operations.SetVisualScriptDocument(motion, without).IsOk());
	fixture.Frames(2);
	REQUIRE(callerWindow.RequestAction(VisualScriptEditorAction::Forward));
	fixture.Frames(3);
	CHECK(GetFocusedRoot() == FindGraphWindow(motion));
	CHECK(motionWindow.GetState().Function == 0);
	CHECK(motionWindow.GetState().CanGoBack);
}

TEST_CASE("VisualScriptEditor: a commit lays a window's canvas out again only when it changed a graph the window's nodes name")
{
	EditorsFixture fixture;
	// Without the runtime the Get is a pill named from Door's document.
	fixture.Semantics.SetAvailable(false);
	// Player reads Door's Speed on another entity, and names no other graph.
	VisualScriptDocument player = MakeDocument();
	player.Graph.Nodes.push_back(
		MakeNode(60, "Variable.Get", glm::vec2(400.0f, 640.0f), Json::object({{"Variable", 100}, {"Graph", fixture.Door.ToString()}})));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Player, player).IsOk());
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();
	VisualScriptEditorPanel& window = fixture.GetPanel(fixture.Player);
	REQUIRE(window.GetState().Visible);
	uint64_t const layouts = window.GetModel().GetCanvasRevision();

	// A move in another graph Player does not name: the project index changes, and the canvas stays as it is.
	VisualScriptDocument moved = AssetManager::GetAsset<VisualScriptAsset>(fixture.EnemyPlayer)->GetDocument();
	moved.Graph.Nodes[1].Position = glm::vec2(480.0f, 80.0f);
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.EnemyPlayer, moved).IsOk());
	fixture.Frames(3);
	CHECK(window.GetModel().GetCanvasRevision() == layouts);

	// A rename of Speed in Door: the pill follows it.
	VisualScriptDocument renamed = AssetManager::GetAsset<VisualScriptAsset>(fixture.Door)->GetDocument();
	renamed.Variables[0].Name = "Pace";
	renamed.Variables[0].Public = true;
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Door, renamed).IsOk());
	fixture.Frames(3);
	uint64_t const followed = window.GetModel().GetCanvasRevision();
	CHECK(followed > layouts);
	VisualScriptCanvasModel const& canvas = window.GetModel().GetCanvas();
	std::optional<uint32_t> const pill = canvas.FindNode(60);
	REQUIRE(pill.has_value());
	CHECK(canvas.GetText(canvas.GetNodes()[*pill].Title) == "Get Pace");

	// Player's own change lays its canvas out, and the index's revision that follows it lays out nothing more.
	VisualScriptDocument own = AssetManager::GetAsset<VisualScriptAsset>(fixture.Player)->GetDocument();
	own.Graph.Nodes[1].Comment = "Waits";
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Player, own).IsOk());
	fixture.Frames(3);
	uint64_t const settled = window.GetModel().GetCanvasRevision();
	CHECK(settled > followed);
	fixture.Frames(3);
	CHECK(window.GetModel().GetCanvasRevision() == settled);
}

TEST_CASE("VisualScriptEditor: following a stop opens the window of its innermost frame's document on its node, a library's own")
{
	EditorsFixture fixture;
	// A library whose function 30 holds node 31, which a frame of a stop names.
	VisualScriptDocument library;
	library.Kind = VisualScriptDocumentKind::Library;
	VisualScriptFunction function;
	function.ID = 30;
	function.Name = "Damage";
	function.Graph.Nodes = {MakeNode(31, "Function.Entry", glm::vec2(0.0f, 0.0f)),
	                        MakeNode(32, "Function.Return", glm::vec2(400.0f, 0.0f))};
	library.Functions.push_back(std::move(function));
	AssetHandle const combat = fixture.AddGraph("Scripts/Combat.sgraph");
	REQUIRE(fixture.Operations.SetVisualScriptDocument(combat, library).IsOk());
	fixture.OpenGraph(fixture.Player);
	fixture.CheckDrawingChangesNothing();

	VisualScriptStop stop;
	stop.ID = 1;
	stop.Graph = fixture.Player;
	VisualScriptStopFrame inner;
	inner.Kind = VisualScriptStopFrameKind::Graph;
	inner.Graph = fixture.Player;
	inner.Document = combat;
	inner.Function = 30;
	inner.Node = 32;
	VisualScriptStopFrame outer;
	outer.Kind = VisualScriptStopFrameKind::Call;
	outer.Graph = fixture.Player;
	outer.Document = fixture.Player;
	outer.Node = 1;
	VisualScriptStopFrame native;
	native.Kind = VisualScriptStopFrameKind::Native;
	native.Title = "Entity.Instantiate";
	stop.Frames = {native, inner, outer};
	fixture.Editors.FollowStop(stop);
	fixture.Frames(3);
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Player, combat});
	CHECK(GetFocusedRoot() == FindGraphWindow(combat));
	CHECK(fixture.GetPanel(combat).GetState().Function == 30);

	// A call-stack row's frame opens its own window; a native frame opens nothing.
	fixture.Editors.FollowFrame(stop, 2);
	fixture.Frames(3);
	CHECK(GetFocusedRoot() == FindGraphWindow(fixture.Player));
	fixture.Editors.FollowFrame(stop, 0);
	fixture.Frames(1);
	CHECK(fixture.GetOpenGraphs().size() == 2);
}

TEST_CASE("VisualScriptEditor: the debug session watches the sub-graphs the visible graph windows show")
{
	EditorsFixture fixture;
	EditorScripts scripts{fixture.Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
	PlayMode play{fixture.Context, scripts};
	VisualScriptDebugSession session{fixture.Context, play};
	fixture.Editors.SetDebugSession(&session);
	fixture.OpenGraph(fixture.Player);
	fixture.OpenGraph(fixture.Door);
	fixture.CheckDrawingChangesNothing();
	fixture.Frames(2);
	// Both docked in the middle: the one in front is watched.
	CHECK(std::vector<VisualScriptDebugWatch>(session.GetWatches().begin(), session.GetWatches().end()) ==
	      std::vector<VisualScriptDebugWatch>{{fixture.Door, 0}});
	fixture.Focus(VisualScriptEditorPanel::GetWindowName(fixture.Player, {}).c_str());
	fixture.Frames(2);
	CHECK(std::vector<VisualScriptDebugWatch>(session.GetWatches().begin(), session.GetWatches().end()) ==
	      std::vector<VisualScriptDebugWatch>{{fixture.Player, 0}});
	fixture.Editors.Close();
	fixture.Frames(2);
	CHECK(session.GetWatches().empty());
	fixture.Editors.SetDebugSession(nullptr);
}

TEST_CASE("VisualScriptEditor: Back to a graph deleted since goes on to the window's own view")
{
	EditorsFixture fixture;
	VisualScriptDocument door = MakeDocument();
	VisualScriptVariable health;
	health.ID = 50;
	health.Name = "Health";
	health.Type = "float";
	health.Public = true;
	door.Variables.push_back(health);
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Door, door).IsOk());
	VisualScriptDocument player = MakeDocument();
	player.Graph.Nodes.push_back(
		MakeNode(60, "Variable.Get", glm::vec2(400.0f, 640.0f), Json::object({{"Variable", 50}, {"Graph", fixture.Door.ToString()}})));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(fixture.Player, player).IsOk());
	// Door's window has a view of its own on its history, then Player's (a jump from Player's node).
	fixture.OpenGraph(fixture.Door);
	fixture.CheckDrawingChangesNothing();
	VisualScriptEditorPanel& doorWindow = fixture.GetPanel(fixture.Door);
	VisualScriptViewState const own = doorWindow.GetState().View;
	VisualScriptOpenRequest moved;
	moved.Graph = fixture.Door;
	moved.Center = own.Center + glm::vec2(900.0f, 0.0f);
	REQUIRE(fixture.Editors.Open(moved).IsOk());
	fixture.Frames(2);
	fixture.OpenGraph(fixture.Player);
	fixture.Gui.Frame(fixture.Drawer(), 1.0f);
	VisualScriptEditorPanel& callerWindow = fixture.GetPanel(fixture.Player);
	glm::vec2 const header = Testing::CanvasPoint(callerWindow.GetModel(), 60, glm::vec2(0.5f, 0.06f));
	fixture.Gui.MouseClick(ImVec2(header.x, header.y), ImGuiMouseButton_Left, fixture.Drawer(), 2);
	fixture.Frames(3);
	REQUIRE(GetFocusedRoot() == FindGraphWindow(fixture.Door));

	// Player is deleted: Back in Door's window skips its entry and returns to Door's own view.
	REQUIRE(fixture.Operations.DeleteAsset(fixture.Player).IsOk());
	fixture.Frames(2);
	REQUIRE(doorWindow.RequestAction(VisualScriptEditorAction::Back));
	fixture.Frames(3);
	CHECK(fixture.GetOpenGraphs() == std::vector<AssetHandle>{fixture.Door});
	CHECK(doorWindow.GetState().View.Center == own.Center);
}
