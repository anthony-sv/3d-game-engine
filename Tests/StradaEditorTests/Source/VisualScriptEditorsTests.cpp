#include "HeadlessImGui.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/Commands/CommandHistory.h"
#include "Editor/Commands/EntityCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/Panels/VisualScriptEditorPanel.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptEditors.h"
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

	// The editor's dock space laid out as its default layout is: the scene view and the game view in the middle, the
	// hierarchy and the inspector on the right; a console floats. The graph windows are drawn after them, as the editor draws
	// them after its fixed panels. Three graphs: Scripts/Player, Scripts/Door and Enemies/Player.
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
			ImGuiID const dockspace = ImGui::DockSpaceOverViewport(ImGui::GetID("Dockspace"), ImGui::GetMainViewport());
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
			drawn = fixture.Editors.DrawEditMenuItems();
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
			drawn = fixture.Editors.DrawEditMenuItems();
		});
	CHECK_FALSE(drawn);

	// The graph focused last, then hidden behind the game view's tab: the scene is the target while it is not shown.
	fixture.Click(fixture.EmptyPoint(fixture.Player));
	REQUIRE(fixture.Editors.GetEditTarget() == &panel);
	fixture.Focus("Game View");
	REQUIRE_FALSE(panel.GetState().Visible);
	CHECK(fixture.Editors.GetEditTarget() == nullptr);
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
