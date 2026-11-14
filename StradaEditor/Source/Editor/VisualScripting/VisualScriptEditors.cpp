#include "Editor/VisualScripting/VisualScriptEditors.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"
#include "Editor/VisualScripting/VisualScriptDebugValues.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptShortcuts.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptDebugger.h"
#include "Strada/Script/VisualScriptEngine.h"

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

		// Live wire values are read at most this often (seconds): 10 times a second (spec-slice4 §13.4).
		constexpr double LiveWireValueInterval = 0.1;
		// A runtime problem's mark names at most this many instances in its tooltip.
		constexpr size_t LiveMarkInstances = 5;

		// The first code points of a text (a value pill's), with an ellipsis when it was longer.
		std::string CutToCodePoints(std::string const& text, size_t codePoints)
		{
			size_t count = 0;
			for (size_t index = 0; index < text.size(); index++)
			{
				if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80)
				{
					if (count == codePoints)
					{
						return text.substr(0, index) + std::string(VisualScriptDebugValueTexts::Ellipsis);
					}
					count++;
				}
			}
			return text;
		}

		namespace Texts = VisualScriptPanelTexts;

		// "Enemy 3 (1234)": an entity of the running scene by name, with its ID.
		std::string DescribeDebugEntity(Scene& scene, UUID entity)
		{
			if (!entity.IsValid())
			{
				return std::string(Texts::StripNoEntity);
			}
			if (!scene.HasEntity(entity))
			{
				return fmt::format("{}{})", Texts::StripMissing, entity);
			}
			return fmt::format("{} ({})", scene.GetEntityByUUID(entity).GetName(), entity);
		}

		// "Apply Damage (node 12) on Player (123)": where a frame is.
		std::string DescribeDebugPlace(Scene& scene, std::string_view title, uint32_t node, UUID entity)
		{
			return fmt::format("{}{}{}{}{}", title, Texts::StripNode, node, Texts::StripOn, DescribeDebugEntity(scene, entity));
		}

		// The words a stop's reason ends its strip's line with.
		std::string_view GetStopReasonWords(VisualScriptStopReason reason)
		{
			switch (reason)
			{
				case VisualScriptStopReason::Breakpoint:
					return Texts::ReasonBreakpoint;
				case VisualScriptStopReason::Step:
					return Texts::ReasonStep;
				case VisualScriptStopReason::Break:
					return Texts::ReasonBreak;
				case VisualScriptStopReason::RunTo:
					return Texts::ReasonRunTo;
				case VisualScriptStopReason::Error:
					return Texts::ReasonError;
				case VisualScriptStopReason::Passed:
					return Texts::ReasonPassed;
			}
			return Texts::ReasonBreakpoint;
		}

		// What a stop-ended reason says in the strip's parenthesis.
		std::string_view GetHeldCause(std::string_view reason)
		{
			if (reason == VisualScriptDebugSession::StopEndedByEdit)
			{
				return Texts::StripHeldByEdit;
			}
			if (reason == VisualScriptDebugSession::StopEndedByDestroy)
			{
				return Texts::StripHeldByDestroy;
			}
			if (reason == VisualScriptDebugSession::StopEndedBySceneLoad)
			{
				return Texts::StripHeldBySceneLoad;
			}
			return Texts::StripHeldOther;
		}

		// Why a breakpoint does not stop now (spec-slice4 §2.6), as graph.breakpoints says it; empty while it is active.
		std::string GetInactiveReason(VisualScriptBreakpointState const* counted, bool library, UUID target, bool targetGone)
		{
			if (counted != nullptr && counted->Reason == VisualScriptBreakpointInactivity::NotInProgram)
			{
				return std::string(Texts::InactiveNotInProgram);
			}
			if (!library && target.IsValid() && targetGone)
			{
				return fmt::format("{}{}{}", Texts::InactiveTarget, target, Texts::InactiveTargetGone);
			}
			if (counted == nullptr || !counted->Active)
			{
				return std::string(library ? Texts::InactiveNoLibraryCaller : Texts::InactiveNoRunner);
			}
			return {};
		}

		// Whether a node of a document's sub-graph is disabled (it passes the flow through without running).
		bool IsNodeDisabled(VisualScriptDocument const* document, uint32_t function, uint32_t node)
		{
			if (document == nullptr)
			{
				return false;
			}
			VisualScriptGraph const* graph = &document->Graph;
			if (function != 0)
			{
				auto const found = std::find_if(document->Functions.begin(), document->Functions.end(),
				                                [function](VisualScriptFunction const& candidate)
				                                {
													return candidate.ID == function;
												});
				graph = found != document->Functions.end() ? &found->Graph : nullptr;
			}
			if (graph == nullptr)
			{
				return false;
			}
			auto const found = std::find_if(graph->Nodes.begin(), graph->Nodes.end(),
			                                [node](VisualScriptNode const& candidate)
			                                {
												return candidate.ID == node;
											});
			return found != graph->Nodes.end() && found->Enabled == VisualScriptNodeEnabled::Disabled;
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
					// Back and Forward to a view of a function deleted since show the event graph, framed whole.
					bool const history = request.Origin && request.Origin->Step != VisualScriptHistoryStep::Jump;
					if (!history)
					{
						return MakeError("the visual script has no function {}", function);
					}
					function = 0;
					applied.Function = 0;
					applied.Center.reset();
					applied.Zoom.reset();
					applied.Frame = VisualScriptFrame::All;
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
			// A function library opens on its first function: it has no event graph to show (spec-slice3 §10.9).
			if (!request.Function && function == 0 && !OffersVisualScriptEventGraph(*document) && !document->Functions.empty())
			{
				function = document->Functions.front().ID;
				graph = &document->Functions.front().Graph;
				applied.Function = function;
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
			applied.Reveal.reset();
		}

		if (window == nullptr)
		{
			m_Windows.push_back(GraphWindow{
				CreateScope<VisualScriptEditorPanel>(request.Graph, m_Operations, m_Semantics, m_Settings, m_EditSession, m_Services),
				{},
				false,
				{},
				CreateScope<VisualScriptWindowDebugInput>(),
				-1.0e30,
				std::nullopt,
				{}});
			window = &m_Windows.back();
			window->Panel->SetProjectIndex(&m_ProjectIndex);
			window->Panel->SetDebugInput(m_DebugSession != nullptr ? window->Debug.get() : nullptr);
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

	void VisualScriptEditors::DropAfterDraw()
	{
		// Taken out of the list before they go: releasing a callback's captures answers a command, which may wait again.
		std::vector<std::function<void()>> dropped;
		dropped.swap(m_AfterDraw);
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
		// The first refresh loads every graph of the project (spec-slice3 §21): not before a graph window opens.
		if (!m_Windows.empty())
		{
			m_ProjectIndex.Refresh();
		}
		UpdateFocus();
		EndInteractions();
		OpenOtherGraphRequests();
		UpdateTitles();
		// The live log's revision every window draws from (spec-slice4 §8.5): one take of the runtime's events per frame, before
		// the windows draw; a window that commits while it draws takes a newer one itself.
		if (!m_Windows.empty())
		{
			m_LiveLog = VisualScriptEngine::GetLiveLog();
		}
		for (GraphWindow const& window : m_Windows)
		{
			window.Panel->SetLiveLog(m_LiveLog);
		}
		UpdateDebugging();

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
		ApplyDebugRequests();

		// Taken first: a callback may ask for the draw after this one.
		std::vector<std::function<void()>> const callbacks = std::exchange(m_AfterDraw, {});
		for (std::function<void()> const& callback : callbacks)
		{
			callback();
		}
	}

	bool VisualScriptEditors::WantsGamePreview() const
	{
		return std::any_of(m_Windows.begin(), m_Windows.end(),
		                   [](GraphWindow const& window)
		                   {
							   return window.Panel->IsGamePreviewShown();
						   });
	}

	void VisualScriptEditors::RequestGameViewBeside(AssetHandle graph)
	{
		m_BesideRequest = BesideRequest{graph, 0};
		m_GameBeside = VisualScriptGameBesideState{graph, VisualScriptGameBesideOutcome::Queued, {}};
	}

	std::optional<AssetHandle> VisualScriptEditors::TakeGameViewBesideRequest()
	{
		if (!m_BesideRequest)
		{
			return std::nullopt;
		}
		// graph.open {besideGameView} opens the window and asks in one call, before the frame whose dock space runs the request
		// and draws the window only after it: the request waits for the window's first draw (WasActive: drawn in the last
		// frame, imgui.cpp:5979).
		if (GraphWindow const* const window = FindGraphWindow(m_BesideRequest->Graph);
		    window != nullptr && m_BesideRequest->Waited < BesideWaitFrames)
		{
			std::string const name = VisualScriptEditorPanel::GetWindowName(m_BesideRequest->Graph, window->Title);
			ImGuiWindow const* const drawn = ImGui::FindWindowByName(name.c_str());
			if (drawn == nullptr || !drawn->WasActive)
			{
				m_BesideRequest->Waited++;
				return std::nullopt;
			}
		}
		AssetHandle const graph = m_BesideRequest->Graph;
		m_BesideRequest.reset();
		return graph;
	}

	void VisualScriptEditors::OnGameViewBesidePlaced(VisualScriptGameBesideState state)
	{
		if (VisualScriptEditorPanel* const panel = FindPanel(state.Graph))
		{
			switch (state.Outcome)
			{
				case VisualScriptGameBesideOutcome::Applied:
					// The game view took the other side of the node: the graph stays in front of its own.
					panel->RequestFocus();
					break;
				case VisualScriptGameBesideOutcome::AlreadyVisible:
					panel->ShowToast(
						fmt::format("{}{}", VisualScriptPanelTexts::BesideNotApplied, VisualScriptPanelTexts::BesideAlreadyVisible));
					break;
				case VisualScriptGameBesideOutcome::Failed:
					panel->ShowToast(fmt::format("{}{}", VisualScriptPanelTexts::BesideNotApplied, state.Reason));
					break;
				case VisualScriptGameBesideOutcome::Queued:
					break;
			}
		}
		m_GameBeside = std::move(state);
	}

	std::optional<AssetHandle> VisualScriptEditors::FindWindowDockedWith(char const* windowName) const
	{
		ImGuiWindow const* const other = ImGui::FindWindowByName(windowName);
		if (other == nullptr || other->DockNode == nullptr)
		{
			return std::nullopt;
		}
		std::optional<AssetHandle> found;
		for (GraphWindow const& window : m_Windows)
		{
			AssetHandle const graph = window.Panel->GetGraph();
			ImGuiWindow const* const candidate = ImGui::FindWindowByID(GetWindowID(graph));
			if (candidate == nullptr || candidate->DockNode != other->DockNode)
			{
				continue;
			}
			if (m_EditTarget && *m_EditTarget == graph)
			{
				return graph;
			}
			// The one focused last among them.
			auto const rank = [this](AssetHandle handle)
			{
				return std::find(m_FocusOrder.begin(), m_FocusOrder.end(), handle) - m_FocusOrder.begin();
			};
			if (!found || rank(graph) < rank(*found))
			{
				found = graph;
			}
		}
		return found;
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
		// Each request with the window that asked, which learns of a failure (spec-slice4 §13.10).
		std::vector<std::pair<AssetHandle, VisualScriptOpenRequest>> requests;
		for (GraphWindow& window : m_Windows)
		{
			for (VisualScriptOpenRequest& request : window.Panel->TakeOtherGraphRequests())
			{
				requests.emplace_back(window.Panel->GetGraph(), std::move(request));
			}
		}
		for (auto const& [from, request] : requests)
		{
			Result<VisualScriptEditorState> const opened = Open(request);
			if (opened)
			{
				continue;
			}
			ST_WARN("Could not open the visual script: {}", opened.GetError());
			if (GraphWindow* asking = FindGraphWindow(from))
			{
				asking->Panel->OnOtherGraphOpenFailed(request, opened.GetError());
			}
		}
	}

	void VisualScriptEditors::SetDebugSession(VisualScriptDebugSession* session)
	{
		m_DebugSession = session;
		for (GraphWindow& window : m_Windows)
		{
			*window.Debug = VisualScriptWindowDebugInput();
			window.Panel->SetDebugInput(session != nullptr ? window.Debug.get() : nullptr);
		}
	}

	void VisualScriptEditors::FollowStop(VisualScriptStop const& stop)
	{
		// The innermost frame that is a graph's (a passed stop's innermost frames can be engine code).
		for (uint32_t frame = 0; frame < stop.Frames.size(); frame++)
		{
			if (stop.Frames[frame].Kind != VisualScriptStopFrameKind::Native)
			{
				FollowFrame(stop, frame);
				return;
			}
		}
	}

	void VisualScriptEditors::FollowFrame(VisualScriptStop const& stop, uint32_t frame)
	{
		if (frame >= stop.Frames.size() || stop.Frames[frame].Kind == VisualScriptStopFrameKind::Native)
		{
			return;
		}
		VisualScriptStopFrame const& shown = stop.Frames[frame];
		VisualScriptOpenRequest request;
		request.Graph = shown.Document;
		request.Function = shown.Function;
		request.ShowNode = shown.Node;
		request.FromDebugger = true;
		request.Focus = true;
		if (Result<VisualScriptEditorState> const opened = Open(request); !opened)
		{
			ST_WARN("The debugger could not show where the execution stopped: {}", opened.GetError());
		}
	}

	void VisualScriptEditors::OnTargetChanged(AssetHandle graph, UUID entity)
	{
		if (GraphWindow* window = FindGraphWindow(graph); window != nullptr && window->Panel->GetDebugTarget() != entity)
		{
			window->Panel->ChooseContextEntity(entity);
		}
	}

	void VisualScriptEditors::OnStopEnded(AssetHandle graph, std::string const& reason)
	{
		m_HeldGraph = graph;
		m_HeldReason = reason;
		VisualScriptEditorPanel* panel = graph.IsValid() ? FindPanel(graph) : nullptr;
		if (panel == nullptr)
		{
			panel = GetEditTarget();
		}
		if (panel != nullptr)
		{
			panel->ShowToast(reason);
		}
		else
		{
			ST_INFO("{}", reason);
		}
	}

	VisualScriptWindowDebugInput const* VisualScriptEditors::FindDebugInput(AssetHandle graph)
	{
		GraphWindow* window = FindGraphWindow(graph);
		return window != nullptr && m_DebugSession != nullptr ? window->Debug.get() : nullptr;
	}

	void VisualScriptEditors::UpdateDebugging()
	{
		if (m_DebugSession == nullptr)
		{
			return;
		}
		VisualScriptDebugSession& session = *m_DebugSession;
		bool const attached = session.IsAttached();
		if (attached && !m_WasAttached)
		{
			m_Attachment++;
		}
		if (!attached && m_WasAttached)
		{
			// The debugger's opens leave the history's stop session with it.
			for (GraphWindow const& window : m_Windows)
			{
				window.Panel->EndDebuggerNavigation();
			}
			m_LastStopGraph = AssetHandle();
		}
		m_WasAttached = attached;
		VisualScriptDebugState const& state = session.GetState();
		if (state.Current)
		{
			if (VisualScriptStop const* current = session.FindStop(*state.Current); current != nullptr && !current->Frames.empty())
			{
				m_LastStopGraph = current->Frames.front().Document;
			}
		}
		if (!attached || !session.HasPausedScene() || !state.Stops.empty())
		{
			m_HeldGraph = AssetHandle();
			m_HeldReason.clear();
		}

		std::vector<VisualScriptDebugWatch> watches;
		for (GraphWindow& window : m_Windows)
		{
			BuildDebugInput(window, watches);
		}
		session.SetWatches(std::move(watches));
		// The flow of the watched sub-graphs, taken once the watches are what the windows show.
		for (GraphWindow& window : m_Windows)
		{
			VisualScriptWindowDebugInput& input = *window.Debug;
			input.Flow = VisualScriptFlowTake();
			if (input.ShowFlow && window.Panel->IsVisible())
			{
				input.Flow = session.TakeFlow(window.Panel->GetGraph(), input.FlowFunction);
			}
		}
	}

	void VisualScriptEditors::BuildDebugInput(GraphWindow& window, std::vector<VisualScriptDebugWatch>& watches)
	{
		VisualScriptDebugSession& session = *m_DebugSession;
		VisualScriptEditorPanel& panel = *window.Panel;
		VisualScriptWindowDebugInput& input = *window.Debug;
		panel.SetDebugInput(&input);
		AssetHandle const graph = panel.GetGraph();
		VisualScriptEditorModel const& model = panel.GetModel();
		bool const library = model.GetStoredDocument() != nullptr && model.GetDisplayDocument().Kind == VisualScriptDocumentKind::Library;
		uint32_t const function = model.GetFunction();
		bool const attached = session.IsAttached();
		bool const mac = ImGui::GetIO().ConfigMacOSXBehaviors;
		Scene& scene = m_Operations.GetContext().GetScene();
		VisualScriptDebugState const& state = session.GetState();

		VisualScriptWindowDebugInput previous = std::move(input);
		input = VisualScriptWindowDebugInput();
		input.Attached = attached;
		input.Attachment = m_Attachment;
		input.BreakpointChord = FormatVisualScriptChord(ImGuiKey_F9, mac);
		VisualScriptWireValues const wireValues = m_Settings.GetWireValues();
		input.WireValues = wireValues != VisualScriptWireValues::Off;
		input.LiveWireValues = wireValues == VisualScriptWireValues::Always;
		input.ShowFlow = attached && m_Settings.IsShowFlow();
		input.FlowFunction = function;
		input.FlowTime = session.GetFlowTime();
		if (panel.IsVisible() && model.GetStoredDocument() != nullptr)
		{
			watches.push_back(VisualScriptDebugWatch{graph, function});
		}
		// The window's target: what its Instance combo shows (spec-slice4 §13.6). A function library runs on no entity (the
		// session keeps no target for one). A window that has not chosen its entity yet (just opened, a stop's window among them,
		// before its first draw resolves it) adopts the target the session has, which an agent's graph.target may have set while
		// it was closed, instead of replacing it.
		if (UUID const kept = session.GetTarget(graph); kept.IsValid() && panel.IsContextUndecided())
		{
			panel.ChooseContextEntity(kept);
		}
		else if (!library && model.GetStoredDocument() != nullptr)
		{
			session.SetTarget(graph, panel.GetDebugTarget());
		}
		UUID const target = library ? UUID::Invalid() : session.GetTarget(graph);

		// The stop selected, and where it stands in this document.
		std::optional<uint64_t> const selected = session.GetSelectedStop();
		VisualScriptStop const* stop = selected ? session.FindStop(*selected) : nullptr;
		uint32_t const frameIndex = session.GetSelectedFrame();

		// The breakpoints of the document.
		for (VisualScriptBreakpoint const& breakpoint : session.GetBreakpoints().Get(graph))
		{
			VisualScriptWindowBreakpoint& shown = input.Breakpoints.emplace_back();
			shown.Breakpoint = breakpoint;
			auto const counted = std::find_if(state.Breakpoints.begin(), state.Breakpoints.end(),
			                                  [graph, &breakpoint](VisualScriptBreakpointState const& candidate)
			                                  {
												  return candidate.Document == graph && candidate.Node == breakpoint.Node;
											  });
			VisualScriptBreakpointState const* report = counted != state.Breakpoints.end() ? &*counted : nullptr;
			if (report != nullptr)
			{
				shown.Reached = report->Reached;
				shown.Hits = report->Hits;
				shown.Error = report->Error;
			}
			shown.State = !breakpoint.Enabled                                        ? VisualScriptBreakpointMarkState::Disabled
			              : !shown.Error.empty()                                     ? VisualScriptBreakpointMarkState::Invalid
			              : !breakpoint.Condition.empty() || breakpoint.HitCount > 1 ? VisualScriptBreakpointMarkState::Conditional
			                                                                         : VisualScriptBreakpointMarkState::Enabled;
			if (attached)
			{
				shown.InactiveReason = GetInactiveReason(report, library, target, target.IsValid() && !scene.HasEntity(target));
			}
			shown.CausedStop =
				stop != nullptr && stop->Breakpoint && *stop->Breakpoint == std::pair<AssetHandle, uint32_t>(graph, breakpoint.Node);
		}

		auto const chord = [mac](ImGuiKeyChord key)
		{
			return FormatVisualScriptChord(key, mac);
		};
		auto const button = [&chord](VisualScriptDebugStripAction action, std::string_view label, ImGuiKeyChord key)
		{
			return VisualScriptDebugStripButton{
				action, std::string(label), key == ImGuiKey_None ? std::string() : chord(key), std::nullopt, {}};
		};
		auto const stoppedButtons = [&button, &chord]
		{
			std::vector<VisualScriptDebugStripButton> buttons{
				button(VisualScriptDebugStripAction::Continue, Texts::StripContinue, ImGuiKey_F5),
				button(VisualScriptDebugStripAction::StepOver, Texts::StripStepOver, ImGuiKey_F10),
				button(VisualScriptDebugStripAction::StepInto, Texts::StripStepInto, ImGuiKey_F11),
				button(VisualScriptDebugStripAction::StepOut, Texts::StripStepOut, ImGuiMod_Shift | ImGuiKey_F11),
			};
			// Step Into's second key, for macOS, which keeps F11 for Show Desktop (spec-slice4 §14.4).
			buttons[2].SecondChord = chord(ImGuiMod_Ctrl | ImGuiKey_F11);
			return buttons;
		};
		// The other frames of the stop in this document (the calls and sends that led to the selected frame): outlined as callers.
		auto const addCallers = [&input, graph](VisualScriptStop const& shown, std::optional<uint32_t> selectedFrame)
		{
			for (uint32_t index = 0; index < shown.Frames.size(); index++)
			{
				VisualScriptStopFrame const& other = shown.Frames[index];
				if (index != selectedFrame && other.Kind != VisualScriptStopFrameKind::Native && other.Document == graph)
				{
					input.Callers.emplace_back(other.Function, other.Node);
				}
			}
		};
		// A member of the stop's chain waiting in this document, inside its Send or Broadcast (its frame is a delivery).
		std::optional<uint32_t> waiting;
		if (stop != nullptr)
		{
			for (uint32_t index = 0; index < stop->Frames.size() && !waiting; index++)
			{
				if (stop->Frames[index].Kind == VisualScriptStopFrameKind::Delivery && stop->Frames[index].Document == graph)
				{
					waiting = index;
				}
			}
		}

		if (stop != nullptr && frameIndex < stop->Frames.size() && stop->Frames[frameIndex].Kind != VisualScriptStopFrameKind::Native &&
		    stop->Frames[frameIndex].Document == graph)
		{
			// The selected stop frame is this document's.
			VisualScriptStopFrame const& frame = stop->Frames[frameIndex];
			input.Stop = stop->ID;
			input.Frame = frameIndex;
			input.Function = frame.Function;
			VisualScriptInstructionMark instruction;
			instruction.Node = frame.Node;
			instruction.Place = frameIndex == 0 ? stop->Place : VisualScriptStopPlace::Enter;
			instruction.Pin = frame.Pin;
			instruction.Note = frame.Note;
			instruction.ReadOnly = !stop->Resumable;
			input.Instruction = std::move(instruction);
			input.State = stop->Reason == VisualScriptStopReason::Passed  ? VisualScriptWindowDebugState::Passed
			              : stop->Reason == VisualScriptStopReason::Error ? VisualScriptWindowDebugState::Error
			                                                              : VisualScriptWindowDebugState::Stopped;
			input.StoppedHere = true;
			std::string const place = DescribeDebugPlace(scene, frame.Title, frame.Node, frame.Entity);
			std::string where = place;
			input.StoppedTooltip = fmt::format("{}{}{}", Texts::StripStoppedAt, place, Texts::StoppedTooltipEnd);
			// A disabled node stops too: the flow enters it and passes through (spec-slice4 §2.2).
			if (IsNodeDisabled(model.GetStoredDocument().get(), frame.Function, frame.Node))
			{
				where += Texts::StripDisabled;
			}
			switch (input.State)
			{
				case VisualScriptWindowDebugState::Passed:
					input.Strip.push_back({fmt::format("{}{}: {}{}", Texts::StripPassedAt, where,
					                                   stop->Message.empty() ? std::string(Texts::StripPassedUnknown) : stop->Message,
					                                   Texts::StripPassedValues),
					                       {button(VisualScriptDebugStripAction::Continue, Texts::StripContinue, ImGuiKey_F5)}});
					break;
				case VisualScriptWindowDebugState::Error:
				{
					// The runtime's message names the place too ("Scripts/Failing.sgraph: Divide (node 12) on Failer (1): integer
					// division by zero"): the line, which starts with the place, keeps what follows it.
					std::string_view message = stop->Message;
					std::string const named = place + ": ";
					if (size_t const at = message.find(named); at != std::string_view::npos)
					{
						message = message.substr(at + named.size());
					}
					input.Strip.push_back({fmt::format("{}{}: {}", Texts::StripFailedAt, where, message),
					                       {button(VisualScriptDebugStripAction::Continue, Texts::StripContinue, ImGuiKey_F5)}});
					break;
				}
				default:
					input.Strip.push_back(
						{fmt::format("{}{}: {}", Texts::StripStoppedAt, where, GetStopReasonWords(stop->Reason)), stoppedButtons()});
					break;
			}
			// One frame's breakpoint stopped several instances of this graph under any instance (spec-slice4 §4.7).
			if (stop->Breakpoint && !target.IsValid() && !library)
			{
				size_t const others = static_cast<size_t>(std::count_if(state.Stops.begin(), state.Stops.end(),
				                                                        [stop](VisualScriptStop const& other)
				                                                        {
																			return other.ID != stop->ID && other.Resumable &&
					                                                               other.Graph == stop->Graph &&
					                                                               other.Breakpoint == stop->Breakpoint;
																		}));
				if (others > 0)
				{
					std::string const entity =
						scene.HasEntity(stop->Entity) ? scene.GetEntityByUUID(stop->Entity).GetName() : stop->Entity.ToString();
					input.Strip.push_back({fmt::format("+{}{}", others, others == 1 ? Texts::StripMoreInstance : Texts::StripMoreInstances),
					                       {{VisualScriptDebugStripAction::OnlyInstance,
					                         fmt::format("{}{}", Texts::StripOnly, entity),
					                         {},
					                         std::nullopt,
					                         {}}}});
				}
			}
			addCallers(*stop, frameIndex);
			// The values of the frame: the path it computed, and the wires' values.
			if (VisualScriptStopValues const* values = session.GetStopValues(stop->ID, frameIndex))
			{
				for (VisualScriptNodeValues const& computed : values->Computed)
				{
					input.Computed.push_back(computed.Node);
				}
				if (input.WireValues)
				{
					VisualScriptLibrary const* nodeLibrary = m_Semantics.GetLibrary().get();
					VisualScriptEnumLookup const enums = MakeVisualScriptEnumLookup(nodeLibrary);
					auto const describe = [&scene](UUID entity)
					{
						return scene.HasEntity(entity) ? scene.GetEntityByUUID(entity).GetName()
						                               : fmt::format("{}{})", Texts::StripMissing, entity);
					};
					VisualScriptAssetNameLookup const assetName = [](AssetHandle handle)
					{
						std::optional<AssetMetadata> const metadata =
							AssetManager::IsInitialized() ? AssetManager::GetMetadata(handle) : std::nullopt;
						return metadata ? (metadata->Path.empty() ? metadata->Name : metadata->Path) : handle.ToString();
					};
					auto const add = [&](uint32_t node, VisualScriptDebugValue const& value, bool output)
					{
						VisualScriptValueCell const cell = FormatVisualScriptDebugValue(value, nodeLibrary, enums, describe, assetName);
						VisualScriptDebugPinValue& pin = input.Values.emplace_back();
						pin.Node = node;
						pin.Pin = value.Name;
						pin.Output = output;
						pin.Text = CutToCodePoints(cell.Text, VisualScriptValuePillCodePoints);
						pin.Swatch = cell.Kind == VisualScriptValueCellKind::Swatch ? cell.Color : std::nullopt;
						pin.Currency = value.Currency;
						pin.Tooltip = cell.Tooltip.empty() ? cell.Text : cell.Tooltip;
					};
					for (VisualScriptNodeValues const& computed : values->Computed)
					{
						for (VisualScriptDebugValue const& output : computed.Outputs)
						{
							add(computed.Node, output, true);
						}
					}
					for (VisualScriptDebugValue const& output : values->Node.Outputs)
					{
						add(values->Node.Node, output, true);
					}
					for (VisualScriptDebugValue const& inputValue : values->Node.Inputs)
					{
						add(values->Node.Node, inputValue, false);
					}
					input.ValuesFunction = values->Function;
				}
			}
		}
		else if (stop != nullptr && waiting)
		{
			// A member of the stop's chain waits in this document, inside its Send or Broadcast; its receiver is the member just
			// inside it (the frame before its delivery, past any engine code between them).
			VisualScriptStopFrame const& member = stop->Frames[*waiting];
			std::optional<uint32_t> receiver;
			for (uint32_t index = *waiting; index > 0 && !receiver; index--)
			{
				if (stop->Frames[index - 1].Kind != VisualScriptStopFrameKind::Native)
				{
					receiver = index - 1;
				}
			}
			input.State = VisualScriptWindowDebugState::Waiting;
			input.Stop = stop->ID;
			input.Frame = *waiting;
			input.Function = member.Function;
			input.StoppedHere = true;
			VisualScriptStopFrame const& leaf = stop->Frames.front();
			input.StoppedTooltip = fmt::format("{}{}{}", Texts::StripStoppedAt,
			                                   DescribeDebugPlace(scene, leaf.Title, leaf.Node, leaf.Entity), Texts::StoppedTooltipEnd);
			addCallers(*stop, std::nullopt);
			VisualScriptStopFrame const& inside = stop->Frames[receiver.value_or(0)];
			VisualScriptDebugStripButton show{VisualScriptDebugStripAction::Show, std::string(Texts::StripShow), {}, receiver, {}};
			input.Strip.push_back({fmt::format("{}{}{}{}{}{}{}", Texts::StripWaitingIn, member.Title, Texts::StripNode, member.Node,
			                                   Texts::StripReceiverOn, DescribeDebugEntity(scene, inside.Entity), Texts::StripStoppedIn) +
			                           model.GetGraphName(inside.Graph),
			                       {std::move(show)}});
		}
		else if (state.Stepping && state.Stepping->WaitsIn && state.Stepping->WaitsIn->Document == graph && state.Stops.empty())
		{
			VisualScriptStepWait const& wait = *state.Stepping->WaitsIn;
			input.State = VisualScriptWindowDebugState::Stepping;
			input.Function = wait.Function;
			std::string text =
				fmt::format("{}{}{}{}{}", Texts::StripStepping, wait.Title, Texts::StripNode, wait.Node, Texts::StripSteppingEnd);
			if (m_Operations.GetContext().IsPlaying() && m_Operations.GetContext().GetScene().IsPaused() && !session.HasPausedScene())
			{
				text += Texts::StripSteppingPaused;
			}
			input.Strip.push_back({std::move(text), {button(VisualScriptDebugStripAction::Break, Texts::StripBreak, ImGuiKey_None)}});
		}
		else if (session.GetSessionState() == VisualScriptDebugSessionState::StepEnded && m_LastStopGraph == graph)
		{
			input.State = VisualScriptWindowDebugState::StepEnded;
			input.Strip.push_back({fmt::format("{}{}{}{}{}", Texts::StripStepEnded, chord(ImGuiKey_F5), Texts::StripStepEndedStep,
			                                   chord(ImGuiKey_F10), Texts::StripStepEndedEnd),
			                       {button(VisualScriptDebugStripAction::Continue, Texts::StripContinue, ImGuiKey_F5),
			                        button(VisualScriptDebugStripAction::StepOver, Texts::StripStepOver, ImGuiKey_F10)}});
		}
		else if (m_HeldGraph == graph && graph.IsValid() && session.HasPausedScene() && state.Stops.empty())
		{
			input.Strip.push_back({fmt::format("{}{}{}{}{}", Texts::StripHeld, GetHeldCause(m_HeldReason), Texts::StripHeldPaused,
			                                   chord(ImGuiKey_F5), Texts::StripHeldEnd),
			                       {button(VisualScriptDebugStripAction::Continue, Texts::StripContinue, ImGuiKey_F5)}});
		}
		else if (stop != nullptr)
		{
			// The stop's other frames in this document (a call that led into a library, a sender in another graph): outlined, with
			// no strip.
			addCallers(*stop, std::nullopt);
		}
		// My Script while stopped (spec-slice4 §13.6): the variables of the selected frame's instance, when it runs this graph.
		if (stop != nullptr && !library && frameIndex < stop->Frames.size() &&
		    stop->Frames[frameIndex].Kind != VisualScriptStopFrameKind::Native && stop->Frames[frameIndex].Graph == graph)
		{
			if (VisualScriptStopValues const* values = session.GetStopValues(stop->ID, frameIndex))
			{
				input.StopVariables = values->Variables;
				input.StopEntity = stop->Frames[frameIndex].Entity;
			}
		}
		BuildLiveMarks(window, input);

		// Values of the running game on the wires (Wire Values: Always), while no stop of this graph shows its own and the window
		// shows its canvas (a tab behind another draws none).
		if (input.LiveWireValues && attached && !input.Stop && !library && panel.IsVisible())
		{
			input.Values = std::move(previous.Values);
			input.ValuesFunction = previous.ValuesFunction;
			ReadLiveWireValues(window, input);
		}
	}

	void VisualScriptEditors::BuildLiveMarks(GraphWindow& window, VisualScriptWindowDebugInput& input)
	{
		if (m_LiveLog == nullptr)
		{
			return;
		}
		AssetHandle const graph = window.Panel->GetGraph();
		Scene& scene = m_Operations.GetContext().GetScene();
		// The runtime problems at the document's nodes (whichever graph's instance ran a library's): one mark per node and
		// severity, the instances' counts summed, with the latest message and the instances in its tooltip.
		struct ProblemMark
		{
			uint32_t Function = 0;
			uint32_t Node = 0;
			VisualScriptLiveMarkKind Kind = VisualScriptLiveMarkKind::RuntimeError;
			uint32_t Count = 0;
			std::string Message;
			std::vector<std::string> Instances;
		};
		std::vector<ProblemMark> problems;
		for (Ref<VisualScriptRuntimeProblem const> const& problem : m_LiveLog->Problems)
		{
			if (problem->Document != graph)
			{
				continue;
			}
			VisualScriptLiveMarkKind const kind = problem->Severity == VisualScriptSeverity::Error
			                                          ? VisualScriptLiveMarkKind::RuntimeError
			                                          : VisualScriptLiveMarkKind::RuntimeWarning;
			auto found = std::find_if(problems.begin(), problems.end(),
			                          [&problem, kind](ProblemMark const& candidate)
			                          {
										  return candidate.Function == problem->Function && candidate.Node == problem->Node &&
				                                 candidate.Kind == kind;
									  });
			if (found == problems.end())
			{
				problems.push_back({problem->Function, problem->Node, kind, 0, {}, {}});
				found = std::prev(problems.end());
			}
			// The log keeps its problems by serial: the last one's message is the latest.
			found->Count += problem->Count;
			found->Message = problem->Message;
			std::string const entity =
				scene.HasEntity(problem->Entity) ? scene.GetEntityByUUID(problem->Entity).GetName() : problem->Entity.ToString();
			found->Instances.push_back(fmt::format("{} {}", entity, FormatVisualScriptRunCount(problem->Count)));
		}
		for (ProblemMark& problem : problems)
		{
			VisualScriptLiveMark mark;
			mark.Node = problem.Node;
			mark.Kind = problem.Kind;
			mark.Count = problem.Count;
			std::string instances;
			for (size_t index = 0; index < problem.Instances.size() && index < LiveMarkInstances; index++)
			{
				instances += (index == 0 ? "" : ", ") + problem.Instances[index];
			}
			if (problem.Instances.size() > LiveMarkInstances)
			{
				instances += fmt::format("{}{}{}", Texts::LiveMarkAnd, problem.Instances.size() - LiveMarkInstances, Texts::LiveMarkMore);
			}
			mark.Tooltip = fmt::format("{}\n{}{}", problem.Message, Texts::LiveMarkOn, instances);
			input.LiveMarks.emplace_back(problem.Function, std::move(mark));
		}

		// The receipts since the last draw: where they cancelled a wait or a stopped execution, or reset a state, at this
		// document's nodes (any instance), for ReceiptMarkSeconds.
		double const now = ImGui::GetTime();
		if (!window.MarkSerial)
		{
			window.MarkSerial = m_LiveLog->Serial;
		}
		auto const add = [&window, graph, now](VisualScriptLiveNode const& node, VisualScriptLiveMarkKind kind, std::string tooltip)
		{
			if (node.Document != graph)
			{
				return;
			}
			std::erase_if(window.TimedMarks,
			              [&node, kind](TimedLiveMark const& timed)
			              {
							  return timed.Function == node.Function && timed.Mark.Node == node.Node && timed.Mark.Kind == kind;
						  });
			TimedLiveMark timed;
			timed.Function = node.Function;
			timed.Mark.Node = node.Node;
			timed.Mark.Kind = kind;
			timed.Mark.Tooltip = std::move(tooltip);
			timed.Expires = now + ReceiptMarkSeconds;
			window.TimedMarks.push_back(std::move(timed));
		};
		for (Ref<VisualScriptLiveEdit const> const& edit : m_LiveLog->Edits)
		{
			if (edit->Serial <= *window.MarkSerial)
			{
				continue;
			}
			for (VisualScriptLiveInstance const& note : edit->InstanceNotes)
			{
				for (VisualScriptLiveNode const& cancelled : note.Cancelled)
				{
					add(cancelled, VisualScriptLiveMarkKind::Cancelled,
					    fmt::format("{}{}", cancelled.Stopped ? Texts::LiveMarkStopCancelled : Texts::LiveMarkWaitCancelled,
					                cancelled.Reason));
				}
			}
			for (VisualScriptLiveNode const& reset : edit->ResetStates)
			{
				// A removed node has no place to mark.
				if (reset.Reason != VisualScriptRemovedStateReason)
				{
					add(reset, VisualScriptLiveMarkKind::Reset, fmt::format("{}{}", reset.Title, Texts::LiveMarkReset));
				}
			}
		}
		window.MarkSerial = std::max(*window.MarkSerial, m_LiveLog->Serial);
		std::erase_if(window.TimedMarks,
		              [now](TimedLiveMark const& timed)
		              {
						  return timed.Expires <= now;
					  });
		for (TimedLiveMark const& timed : window.TimedMarks)
		{
			input.LiveMarks.emplace_back(timed.Function, timed.Mark);
		}
	}

	void VisualScriptEditors::ReadLiveWireValues(GraphWindow& window, VisualScriptWindowDebugInput& input)
	{
		double const now = ImGui::GetTime();
		VisualScriptEditorPanel const& panel = *window.Panel;
		uint32_t const function = panel.GetModel().GetFunction();
		if (now - window.LiveValuesRead < LiveWireValueInterval && input.ValuesFunction == function)
		{
			return;
		}
		window.LiveValuesRead = now;
		input.ValuesFunction = function;
		input.Values = ReadLiveWireValues(panel.GetModel(), panel.GetDebugTarget(), m_Semantics.GetLibrary().get(),
		                                  m_Operations.GetContext().GetScene());
	}

	std::vector<VisualScriptDebugPinValue> VisualScriptEditors::ReadLiveWireValues(VisualScriptEditorModel const& model, UUID entity,
	                                                                               VisualScriptLibrary const* library, Scene& scene)
	{
		ST_PROFILE_SCOPE("GraphDebug.ReadValues");
		std::vector<VisualScriptDebugPinValue> values;
		if (!entity.IsValid() || !ScriptEngine::IsInitialized())
		{
			return values;
		}
		// Only the sources of the data wires in view, at most as many wires as pills are drawn: the request's cost follows the
		// view, not the graph.
		VisualScriptCanvasModel const& canvas = model.GetCanvas();
		std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
		VisualScriptRect const visible = model.GetView().GetVisibleRect();
		auto const shown = [&visible, &nodes](uint32_t index)
		{
			VisualScriptNodeVisual const& node = nodes[index];
			return node.Position.x <= visible.Max.x && node.Position.x + node.Size.x >= visible.Min.x && node.Position.y <= visible.Max.y &&
			       node.Position.y + node.Size.y >= visible.Min.y;
		};
		VisualScriptValueRequest request;
		request.Entity = entity;
		request.Function = model.GetFunction();
		size_t wires = 0;
		for (VisualScriptLinkVisual const& link : canvas.GetLinks())
		{
			if (wires == VisualScriptValuePillLimit)
			{
				break;
			}
			if (link.HasFlag(VisualScriptLinkFlags::Exec) || link.FromNode >= nodes.size() || link.ToNode >= nodes.size() ||
			    (!shown(link.FromNode) && !shown(link.ToNode)))
			{
				continue;
			}
			wires++;
			uint32_t const source = nodes[link.FromNode].ID;
			if (std::find(request.Nodes.begin(), request.Nodes.end(), source) == request.Nodes.end())
			{
				request.Nodes.push_back(source);
			}
		}
		if (request.Nodes.empty())
		{
			return values;
		}
		Result<VisualScriptLiveValues> const read = VisualScriptDebugger::ReadValues(request);
		if (!read)
		{
			return values;
		}
		VisualScriptEnumLookup const enums = MakeVisualScriptEnumLookup(library);
		auto const describe = [&scene](UUID value)
		{
			return scene.HasEntity(value) ? scene.GetEntityByUUID(value).GetName() : fmt::format("Missing ({})", value);
		};
		VisualScriptAssetNameLookup const assetName = [](AssetHandle handle)
		{
			std::optional<AssetMetadata> const metadata = AssetManager::IsInitialized() ? AssetManager::GetMetadata(handle) : std::nullopt;
			return metadata ? (metadata->Path.empty() ? metadata->Name : metadata->Path) : handle.ToString();
		};
		for (VisualScriptNodeValues const& node : read.GetValue().Nodes)
		{
			for (VisualScriptDebugValue const& output : node.Outputs)
			{
				VisualScriptValueCell const cell = FormatVisualScriptDebugValue(output, library, enums, describe, assetName);
				VisualScriptDebugPinValue& pin = values.emplace_back();
				pin.Node = node.Node;
				pin.Pin = output.Name;
				pin.Output = true;
				pin.Text = CutToCodePoints(cell.Text, VisualScriptValuePillCodePoints);
				pin.Swatch = cell.Kind == VisualScriptValueCellKind::Swatch ? cell.Color : std::nullopt;
				pin.Currency = output.Currency;
				pin.Tooltip = cell.Tooltip.empty() ? cell.Text : cell.Tooltip;
			}
		}
		return values;
	}

	void VisualScriptEditors::ApplyDebugRequests()
	{
		if (m_DebugSession == nullptr)
		{
			return;
		}
		VisualScriptDebugSession& session = *m_DebugSession;
		// Taken from every window first: a request may open a window (the strip's Show).
		std::vector<std::pair<AssetHandle, VisualScriptDebugRequest>> requests;
		for (GraphWindow& window : m_Windows)
		{
			for (VisualScriptDebugRequest& request : window.Panel->TakeDebugRequests())
			{
				requests.emplace_back(window.Panel->GetGraph(), std::move(request));
			}
		}
		for (auto const& [graph, request] : requests)
		{
			switch (request.RequestKind)
			{
				case VisualScriptDebugRequest::Kind::ToggleBreakpoints:
					session.ToggleBreakpoints(graph, request.Nodes);
					break;
				case VisualScriptDebugRequest::Kind::SetBreakpointsEnabled:
					session.SetBreakpointsEnabled(graph, request.Nodes, request.Enabled);
					break;
				case VisualScriptDebugRequest::Kind::SetBreakpoint:
					if (Result<void> const set = session.SetBreakpoint(graph, request.Breakpoint); !set)
					{
						if (VisualScriptEditorPanel* panel = FindPanel(graph))
						{
							panel->ShowToast(fmt::format("The breakpoint was not changed: {}", set.GetError()));
						}
					}
					break;
				case VisualScriptDebugRequest::Kind::RemoveBreakpoints:
					session.RemoveBreakpoints(graph, request.Nodes);
					break;
				case VisualScriptDebugRequest::Kind::RunTo:
					if (!request.Nodes.empty())
					{
						session.RequestRunTo(graph, request.Nodes.front());
					}
					break;
				case VisualScriptDebugRequest::Kind::Resume:
					// Continue without a stop is every stop's (F5); steps step the stop shown.
					session.RequestResume(request.Stop, request.Action,
					                      request.Action == VisualScriptDebugAction::Continue && !request.Stop);
					break;
				case VisualScriptDebugRequest::Kind::OnlyInstance:
					if (request.Stop)
					{
						session.RequestOnlyInstance(*request.Stop);
					}
					break;
				case VisualScriptDebugRequest::Kind::CancelStep:
					if (Result<void> const cancelled = session.Break(true); !cancelled)
					{
						ST_WARN("The step was not cancelled: {}", cancelled.GetError());
					}
					break;
				case VisualScriptDebugRequest::Kind::BreakOnErrors:
					session.SetBreakOnErrors(true);
					break;
				case VisualScriptDebugRequest::Kind::ShowFrame:
					if (request.Stop)
					{
						if (VisualScriptStop const* stop = session.FindStop(*request.Stop);
						    stop != nullptr && request.Frame < stop->Frames.size())
						{
							// The receiver a waiting member's strip names: the member just inside it.
							VisualScriptStop const shown = *stop;
							session.SelectStop(shown.ID);
							session.SelectFrame(request.Frame);
							FollowFrame(shown, request.Frame);
						}
					}
					break;
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
