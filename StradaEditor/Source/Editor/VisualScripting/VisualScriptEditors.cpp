#include "Editor/VisualScripting/VisualScriptEditors.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptShortcuts.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/Scene.h"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Strada
{
	namespace
	{
		// The windows whose focus makes the scene the Edit menu's target (spec §8.4).
		constexpr std::array<char const*, 3> SceneWindows = {"Scene View", "Scene Hierarchy", "Inspector"};

		// The ImGui ID of a graph's window, whatever its title (ImHashStr restarts at "###", imgui.cpp:2565).
		ImGuiID GetWindowID(AssetHandle graph)
		{
			return ImHashStr(VisualScriptEditorPanel::GetWindowName(graph, {}).c_str());
		}

		// A path relative to the asset directory (forward slashes) as its file's stem and its folder (empty at the root).
		std::pair<std::string_view, std::string_view> SplitPath(std::string_view path)
		{
			size_t const slash = path.rfind('/');
			std::string_view const file = slash == std::string_view::npos ? path : path.substr(slash + 1);
			std::string_view const folder = slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
			size_t const dot = file.rfind('.');
			return {file.substr(0, dot == std::string_view::npos || dot == 0 ? file.size() : dot), folder};
		}

		bool IsRegisteredVisualScript(AssetHandle graph)
		{
			return AssetManager::IsInitialized() && AssetManager::GetAssetType(graph) == AssetType::VisualScript;
		}

		bool HasElement(VisualScriptGraph const& graph, uint32_t id)
		{
			bool const node = std::any_of(graph.Nodes.begin(), graph.Nodes.end(),
			                              [id](VisualScriptNode const& candidate)
			                              {
											  return candidate.ID == id;
										  });
			return node || std::any_of(graph.Comments.begin(), graph.Comments.end(),
			                           [id](VisualScriptComment const& candidate)
			                           {
										   return candidate.ID == id;
									   });
		}
	}

	VisualScriptEditors::VisualScriptEditors(EditorOperations& operations, VisualScriptSemantics& semantics,
	                                         VisualScriptViewSettings& settings)
		: m_Operations(operations),
		  m_Semantics(semantics),
		  m_Settings(settings)
	{
	}

	VisualScriptEditors::~VisualScriptEditors() = default;

	void VisualScriptEditors::SetServices(VisualScriptEditorServices services)
	{
		m_Services = std::move(services);
	}

	Result<VisualScriptEditorState> VisualScriptEditors::Open(VisualScriptOpenRequest const& request)
	{
		Maintain();
		if (!IsRegisteredVisualScript(request.Graph))
		{
			if (!AssetManager::IsInitialized() || !AssetManager::GetMetadata(request.Graph))
			{
				return MakeError("no asset {}", request.Graph.ToString());
			}
			return MakeError("{} is not a visual script", request.Graph.ToString());
		}
		if (request.ContextEntity.IsValid() && !m_Operations.GetContext().GetScene().HasEntity(request.ContextEntity))
		{
			return MakeError("the scene has no entity {}", request.ContextEntity);
		}

		VisualScriptOpenRequest applied = request;
		// The sub-graph the window shows once the request is applied; unknown for a file that does not load.
		std::optional<uint32_t> shown;
		GraphWindow* window = FindGraphWindow(request.Graph);
		bool const loadable = !AssetManager::IsMissing(request.Graph) && !AssetManager::HasLoadFailure(request.Graph);
		Result<Ref<VisualScriptAsset>> asset =
			loadable ? AssetManager::TryGetAsset<VisualScriptAsset>(request.Graph) : Result<Ref<VisualScriptAsset>>(Error{"not loaded"});
		if (asset)
		{
			Ref<VisualScriptDocument const> const document = asset.GetValue()->GetSharedDocument();
			// The sub-graph the selection belongs to: the request's, else the one the window shows, else the one the views file
			// keeps (a function that is gone shows the event graph).
			uint32_t function = 0;
			if (request.Function)
			{
				function = *request.Function;
				if (function != 0 && FindVisualScriptGraph(*document, function) == nullptr)
				{
					return MakeError("the visual script has no function {}", function);
				}
			}
			else if (window != nullptr)
			{
				function = window->Panel->GetModel().GetFunction();
			}
			else if (VisualScriptGraphViews const* views = m_Settings.FindGraph(request.Graph))
			{
				function = views->Shown;
			}
			VisualScriptGraph const* graph = FindVisualScriptGraph(*document, function);
			if (graph == nullptr)
			{
				graph = &document->Graph;
				function = 0;
			}
			shown = function;
			for (uint32_t const id : request.Select)
			{
				if (!HasElement(*graph, id))
				{
					return MakeError("the graph shown has no node or comment {}", id);
				}
			}
		}
		else
		{
			// The window shows the file's card: nothing of the document can be shown or checked.
			applied.Function.reset();
			applied.Select.clear();
			applied.Frame = VisualScriptFrame::Keep;
			applied.Find.clear();
		}

		if (window == nullptr)
		{
			m_Windows.push_back(GraphWindow{
				CreateScope<VisualScriptEditorPanel>(request.Graph, m_Operations, m_Semantics, m_Settings, m_EditSession, m_Services),
				{},
				false,
				{}});
			window = &m_Windows.back();
			RefreshPath(*window);
		}
		window->Panel->Apply(applied);
		VisualScriptEditorState state = window->Panel->GetState();
		// The draw that applies the request switches to it.
		if (shown)
		{
			state.Function = *shown;
		}
		return state;
	}

	std::vector<AssetHandle> VisualScriptEditors::Close(std::optional<AssetHandle> graph)
	{
		Maintain();
		std::vector<size_t> indices;
		std::vector<AssetHandle> closed;
		for (size_t i = 0; i < m_Windows.size(); i++)
		{
			AssetHandle const handle = m_Windows[i].Panel->GetGraph();
			if (!graph || *graph == handle)
			{
				indices.push_back(i);
				closed.push_back(handle);
			}
		}
		RemoveWindows(indices);
		SaveViews();
		return closed;
	}

	std::vector<VisualScriptEditorState> VisualScriptEditors::Describe()
	{
		Maintain();
		std::vector<VisualScriptEditorState> states;
		states.reserve(m_Windows.size());
		for (GraphWindow const& window : m_Windows)
		{
			states.push_back(window.Panel->GetState());
		}
		return states;
	}

	uint32_t VisualScriptEditors::GetShownFunction(AssetHandle graph)
	{
		Maintain();
		if (GraphWindow const* window = FindGraphWindow(graph))
		{
			return window->Panel->GetModel().GetFunction();
		}
		VisualScriptGraphViews const* views = m_Settings.FindGraph(graph);
		return views != nullptr ? views->Shown : 0;
	}

	bool VisualScriptEditors::HasNodeLibrary()
	{
		return m_Semantics.GetLibrary() != nullptr;
	}

	void VisualScriptEditors::AfterNextDraw(std::function<void()> callback)
	{
		m_AfterDraw.push_back(std::move(callback));
	}

	VisualScriptEditorPanel* VisualScriptEditors::FindPanel(AssetHandle graph)
	{
		Maintain();
		GraphWindow* window = FindGraphWindow(graph);
		return window != nullptr ? window->Panel.get() : nullptr;
	}

	VisualScriptEditorPanel* VisualScriptEditors::GetEditTarget()
	{
		Maintain();
		if (!m_EditTarget)
		{
			return nullptr;
		}
		GraphWindow* window = FindGraphWindow(*m_EditTarget);
		// A window whose tab is not in front takes no actions (R9.2): the scene is the target meanwhile.
		if (window == nullptr || !window->Panel->GetState().Visible)
		{
			return nullptr;
		}
		return window->Panel.get();
	}

	bool VisualScriptEditors::HandleGlobalEditingKey(VisualScriptEditorAction action)
	{
		VisualScriptEditorPanel* target = GetEditTarget();
		if (target == nullptr)
		{
			return false;
		}
		if (!target->RequestAction(action))
		{
			// The edit target is shown, so this is not expected; a window hidden meanwhile takes no actions.
			ST_WARN("The visual script window was hidden: the key was not acted on");
		}
		return true;
	}

	void VisualScriptEditors::NotifyHistoryStep(bool undone, std::string const& description, AssetHandle editedAsset)
	{
		if (VisualScriptEditorPanel* target = GetEditTarget())
		{
			target->NotifyHistoryStep(undone, description, editedAsset);
		}
	}

	void VisualScriptEditors::OnImGuiRender()
	{
		Maintain();
		UpdateFocus();
		EndInteractions();
		OpenOtherGraphRequests();
		UpdateTitles();

		std::vector<size_t> closed;
		for (size_t i = 0; i < m_Windows.size(); i++)
		{
			GraphWindow& window = m_Windows[i];
			std::string const name = VisualScriptEditorPanel::GetWindowName(window.Panel->GetGraph(), window.Title);
			// Placement is computed for a window ImGui does not know yet, and applied once (FirstUseEver, imgui.cpp:7048-7085):
			// windows closed and opened again in a session keep the dock node ImGui remembers.
			std::optional<ImGuiID> dockTarget;
			if (ImGui::FindWindowByName(name.c_str()) == nullptr)
			{
				dockTarget = FindDockTarget();
				if (!dockTarget)
				{
					ImGuiViewport const* viewport = ImGui::GetMainViewport();
					ImVec2 const center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
					                    viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
					ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
					ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * FloatingShare, viewport->WorkSize.y * FloatingShare),
					                         ImGuiCond_FirstUseEver);
				}
			}
			bool open = true;
			window.Panel->OnImGuiRender(open, dockTarget);
			if (!open)
			{
				closed.push_back(i);
			}
		}
		if (!closed.empty())
		{
			RemoveWindows(closed);
			SaveViews();
		}

		// Taken first: a callback may ask for the draw after this one.
		std::vector<std::function<void()>> const callbacks = std::exchange(m_AfterDraw, {});
		for (std::function<void()> const& callback : callbacks)
		{
			callback();
		}
	}

	std::optional<std::vector<EditorMenuItemRecord>> VisualScriptEditors::DrawEditMenuItems(std::span<ImGuiKeyChord const> claimed)
	{
		VisualScriptEditorPanel* target = GetEditTarget();
		if (target == nullptr)
		{
			return std::nullopt;
		}
		VisualScriptEditorState const state = target->GetState();
		bool const selection = !state.SelectedNodes.empty() || !state.SelectedComments.empty();
		// Delete, Duplicate and Rename act on the My Script row Details shows, when it shows one.
		bool const row = target->GetPaneShared().Element.has_value();
		struct Item
		{
			char const* Label;
			VisualScriptEditorAction Action;
			bool Enabled;
		};
		std::array<Item, 7> const items = {{
			{"Cut", VisualScriptEditorAction::Cut, selection},
			{"Copy", VisualScriptEditorAction::Copy, selection},
			{"Paste", VisualScriptEditorAction::Paste, true},
			{"Duplicate", VisualScriptEditorAction::Duplicate, selection || row},
			{"Delete", VisualScriptEditorAction::Delete, selection || row},
			{"Select All", VisualScriptEditorAction::SelectAll, true},
			{"Deselect All", VisualScriptEditorAction::DeselectAll, selection},
		}};
		std::vector<EditorMenuItemRecord> drawn;
		for (Item const& item : items)
		{
			if (item.Action == VisualScriptEditorAction::SelectAll)
			{
				ImGui::Separator();
			}
			// The key the editor routes to the edit target (EditorEditingKeys), the action's first in the windows' table.
			VisualScriptShortcut const* shortcut = FindVisualScriptShortcut(item.Action);
			std::string text = shortcut != nullptr ? GetEditShortcutText(shortcut->Chord, claimed) : std::string();
			if (ImGui::MenuItem(item.Label, text.c_str(), false, item.Enabled) && !target->RequestAction(item.Action))
			{
				// The edit target is shown, so this is not expected; a window hidden meanwhile takes no actions.
				ST_WARN("The visual script window was hidden: {} was not done", item.Label);
			}
			drawn.push_back({item.Label, std::move(text), item.Enabled});
		}
		return drawn;
	}

	void VisualScriptEditors::DrawViewMenuItems()
	{
		if (!ImGui::BeginMenu("Visual Scripts", !m_Windows.empty()))
		{
			return;
		}
		std::optional<AssetHandle> close;
		bool closeAll = false;
		for (GraphWindow const& window : m_Windows)
		{
			AssetHandle const graph = window.Panel->GetGraph();
			// The title its tab shows; the handle tells two windows apart whatever their titles.
			std::string_view const title = !window.Title.empty() ? std::string_view(window.Title)
			                               : window.HasFile      ? SplitPath(window.Path).first
			                                                     : std::string_view(window.Path);
			std::string const label = fmt::format("{}###{}", title, graph.ToString());
			bool checked = true;
			if (ImGui::MenuItem(label.c_str(), nullptr, &checked) && !checked)
			{
				close = graph;
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Close All"))
		{
			closeAll = true;
		}
		ImGui::EndMenu();
		if (closeAll)
		{
			Close();
		}
		else if (close)
		{
			Close(*close);
		}
	}

	void VisualScriptEditors::Flush()
	{
		for (GraphWindow& window : m_Windows)
		{
			if (window.Panel->HasPendingWidgetEdit())
			{
				window.Panel->FlushWidgetEdit();
			}
		}
	}

	void VisualScriptEditors::SaveViews()
	{
		std::vector<AssetHandle> graphs;
		graphs.reserve(m_Windows.size());
		for (GraphWindow const& window : m_Windows)
		{
			graphs.push_back(window.Panel->GetGraph());
		}
		m_Settings.SetOpenGraphs(graphs);
		if (Result<void> saved = m_Settings.Save(); !saved)
		{
			ST_WARN("Could not save the visual script views: {}", saved.GetError());
		}
	}

	void VisualScriptEditors::ResetLayout(ImGuiID centerNode)
	{
		for (GraphWindow const& window : m_Windows)
		{
			std::string const name = VisualScriptEditorPanel::GetWindowName(window.Panel->GetGraph(), window.Title);
			// Only windows that exist: docking a name ImGui does not know would create a settings entry for it.
			if (ImGui::FindWindowByName(name.c_str()) != nullptr)
			{
				ImGui::DockBuilderDockWindow(name.c_str(), centerNode);
			}
		}
	}

	void VisualScriptEditors::Maintain()
	{
		// The project first: closing a project saves its open windows before their handles stop being registered.
		FollowProject();
		CloseDeletedGraphs();
	}

	void VisualScriptEditors::FollowProject()
	{
		Project const* project = m_Operations.GetContext().GetProject();
		std::filesystem::path const file = project != nullptr ? project->GetFilePath() : std::filesystem::path();
		if (project == m_Project && file == m_ProjectFile)
		{
			return;
		}
		bool const hadProject = !m_ProjectFile.empty();
		m_Project = project;
		m_ProjectFile = file;

		// The old project's windows: their edits, then what was open and their views.
		Flush();
		if (hadProject || !m_Windows.empty())
		{
			SaveViews();
		}
		m_Windows.clear();
		m_EditTarget.reset();
		m_FocusOrder.clear();

		// The new project's windows, in their order.
		m_Settings.SetProject(file);
		std::span<AssetHandle const> const open = m_Settings.GetOpenGraphs();
		std::vector<AssetHandle> const graphs(open.begin(), open.end());
		for (AssetHandle const graph : graphs)
		{
			if (!IsRegisteredVisualScript(graph))
			{
				ST_INFO("A visual script window was not opened again: {} is no longer a visual script of the project", graph.ToString());
				continue;
			}
			VisualScriptOpenRequest request;
			request.Graph = graph;
			request.Focus = false;
			if (Result<VisualScriptEditorState> opened = Open(request); !opened)
			{
				ST_INFO("A visual script window was not opened again: {}", opened.GetError());
			}
		}
	}

	void VisualScriptEditors::CloseDeletedGraphs()
	{
		for (size_t i = 0; i < m_Windows.size();)
		{
			GraphWindow& window = m_Windows[i];
			AssetHandle const graph = window.Panel->GetGraph();
			if (IsRegisteredVisualScript(graph))
			{
				RefreshPath(window);
				i++;
				continue;
			}
			// The asset was deleted: its undo steps were discarded or forgotten with it (spec §4.5c), and its pending edits have
			// nowhere to go.
			ST_INFO("Closed '{}': the visual script was deleted", window.Path);
			m_Settings.ForgetGraph(graph);
			m_Windows.erase(m_Windows.begin() + static_cast<std::ptrdiff_t>(i));
			std::erase(m_FocusOrder, graph);
			if (m_EditTarget && *m_EditTarget == graph)
			{
				m_EditTarget.reset();
			}
		}
	}

	void VisualScriptEditors::UpdateFocus()
	{
		ImGuiWindow const* nav = GImGui->NavWindow;
		if (nav == nullptr)
		{
			return;
		}
		// A docked window is its own root (imgui.cpp:7854); the windows of a graph window's panes have it as theirs, its popups
		// are their own and leave the target as it is.
		ImGuiWindow const* root = nav->RootWindow;
		for (GraphWindow const& window : m_Windows)
		{
			AssetHandle const graph = window.Panel->GetGraph();
			if (root->ID == GetWindowID(graph))
			{
				m_EditTarget = graph;
				std::erase(m_FocusOrder, graph);
				m_FocusOrder.insert(m_FocusOrder.begin(), graph);
				return;
			}
		}
		for (char const* name : SceneWindows)
		{
			if (root->ID == ImHashStr(name))
			{
				m_EditTarget.reset();
				return;
			}
		}
	}

	void VisualScriptEditors::UpdateTitles()
	{
		// Graphs with one stem show their folders: "Player (Enemies)"; the others their stems (the window's own title), memory
		// assets (no path) their names.
		std::unordered_map<std::string_view, uint32_t> stems;
		for (GraphWindow const& window : m_Windows)
		{
			if (window.HasFile)
			{
				stems[SplitPath(window.Path).first]++;
			}
		}
		for (GraphWindow& window : m_Windows)
		{
			std::string title;
			if (window.HasFile)
			{
				auto const [stem, folder] = SplitPath(window.Path);
				if (stems[stem] > 1)
				{
					title = fmt::format("{} ({})", stem, folder.empty() ? std::string_view("Assets") : folder);
				}
			}
			if (title != window.Title)
			{
				window.Title = title;
				window.Panel->SetTitle(std::move(title));
			}
		}
	}

	void VisualScriptEditors::EndInteractions()
	{
		ImGuiID const active = ImGui::GetActiveID();
		if (active == 0)
		{
			Flush();
		}
		m_EditSession.Update(active != 0 && active == m_LastActiveItem, m_Operations.GetContext().GetHistory());
		m_LastActiveItem = active;
	}

	void VisualScriptEditors::OpenOtherGraphRequests()
	{
		std::vector<VisualScriptOpenRequest> requests;
		for (GraphWindow& window : m_Windows)
		{
			std::vector<VisualScriptOpenRequest> taken = window.Panel->TakeOtherGraphRequests();
			requests.insert(requests.end(), std::make_move_iterator(taken.begin()), std::make_move_iterator(taken.end()));
		}
		for (VisualScriptOpenRequest const& request : requests)
		{
			if (Result<VisualScriptEditorState> opened = Open(request); !opened)
			{
				ST_WARN("Could not open the visual script: {}", opened.GetError());
			}
		}
	}

	std::optional<ImGuiID> VisualScriptEditors::FindDockTarget() const
	{
		// Beside the graph window focused last that is docked, so graphs gather as tabs.
		for (AssetHandle const graph : m_FocusOrder)
		{
			bool const open = std::any_of(m_Windows.begin(), m_Windows.end(),
			                              [graph](GraphWindow const& window)
			                              {
											  return window.Panel->GetGraph() == graph;
										  });
			ImGuiWindow const* window = open ? ImGui::FindWindowByID(GetWindowID(graph)) : nullptr;
			if (window != nullptr && window->DockId != 0 && ImGui::DockBuilderGetNode(window->DockId) != nullptr)
			{
				return window->DockId;
			}
		}
		if (m_DefaultDock)
		{
			if (std::optional<ImGuiID> const node = m_DefaultDock(); node && ImGui::DockBuilderGetNode(*node) != nullptr)
			{
				return node;
			}
		}
		return std::nullopt;
	}

	void VisualScriptEditors::RefreshPath(GraphWindow& window)
	{
		std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(window.Panel->GetGraph());
		if (!metadata)
		{
			return;
		}
		window.HasFile = !metadata->Path.empty();
		window.Path = window.HasFile ? metadata->Path : metadata->Name;
	}

	VisualScriptEditors::GraphWindow* VisualScriptEditors::FindGraphWindow(AssetHandle graph)
	{
		for (GraphWindow& window : m_Windows)
		{
			if (window.Panel->GetGraph() == graph)
			{
				return &window;
			}
		}
		return nullptr;
	}

	void VisualScriptEditors::RemoveWindows(std::vector<size_t> const& indices)
	{
		// Each window's pending edit is committed before any window goes.
		for (size_t const index : indices)
		{
			VisualScriptEditorPanel& panel = *m_Windows[index].Panel;
			if (panel.HasPendingWidgetEdit())
			{
				panel.FlushWidgetEdit();
			}
		}
		for (auto it = indices.rbegin(); it != indices.rend(); ++it)
		{
			AssetHandle const graph = m_Windows[*it].Panel->GetGraph();
			m_Windows.erase(m_Windows.begin() + static_cast<std::ptrdiff_t>(*it));
			std::erase(m_FocusOrder, graph);
			if (m_EditTarget && *m_EditTarget == graph)
			{
				m_EditTarget.reset();
			}
		}
	}
}
