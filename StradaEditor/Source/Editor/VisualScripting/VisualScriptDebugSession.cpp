#include "Editor/VisualScripting/VisualScriptDebugSession.h"

#include "Editor/EditorContext.h"
#include "Editor/RecentProjects.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace Strada
{
	namespace
	{
		constexpr double MinWaitSeconds = 0.1;
		constexpr double MaxWaitSeconds = 60.0;

		// A graph as messages name it: its path, else its handle.
		std::string DescribeGraph(AssetHandle graph)
		{
			if (AssetManager::IsInitialized())
			{
				if (std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(graph); metadata && !metadata->Path.empty())
				{
					return metadata->Path;
				}
			}
			return graph.ToString();
		}

		// The graph's document as stored (the asset as loaded); an error for what is not a visual script.
		Result<Ref<VisualScriptDocument const>> LoadDocument(AssetHandle graph)
		{
			if (!AssetManager::IsInitialized() || !graph.IsValid())
			{
				return MakeError("no visual script has the handle {}", graph.ToString());
			}
			Result<Ref<VisualScriptAsset>> asset = AssetManager::TryGetAsset<VisualScriptAsset>(graph);
			if (!asset)
			{
				return Error{asset.GetError()};
			}
			return asset.GetValue()->GetSharedDocument();
		}

		VisualScriptNode const* FindNode(VisualScriptGraph const& graph, uint32_t node)
		{
			auto const found = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
			                                [node](VisualScriptNode const& candidate)
			                                {
												return candidate.ID == node;
											});
			return found != graph.Nodes.end() ? &*found : nullptr;
		}

		// The node of an ID in whichever sub-graph holds it (node IDs are unique across them); null when none does.
		VisualScriptNode const* FindNode(VisualScriptDocument const& document, uint32_t node)
		{
			if (VisualScriptNode const* found = FindNode(document.Graph, node))
			{
				return found;
			}
			for (VisualScriptFunction const& function : document.Functions)
			{
				if (VisualScriptNode const* found = FindNode(function.Graph, node))
				{
					return found;
				}
			}
			return nullptr;
		}
	}

	std::string_view VisualScriptDebugSessionStateToString(VisualScriptDebugSessionState state)
	{
		switch (state)
		{
			case VisualScriptDebugSessionState::Detached:
				return "Detached";
			case VisualScriptDebugSessionState::Running:
				return "Running";
			case VisualScriptDebugSessionState::Stopped:
				return "Stopped";
			case VisualScriptDebugSessionState::Stepping:
				return "Stepping";
			case VisualScriptDebugSessionState::StepEnded:
				return "StepEnded";
		}
		return "";
	}

	VisualScriptDebugSession::VisualScriptDebugSession(EditorContext& context, PlayMode& playMode)
		: m_Context(context),
		  m_PlayMode(playMode)
	{
	}

	VisualScriptDebugSession::~VisualScriptDebugSession()
	{
		// The runtime keeps no stopped execution of a session that is gone (play mode clears its observer first).
		if (m_Attached)
		{
			VisualScriptDebugger::Detach();
		}
	}

	void VisualScriptDebugSession::SetViewSettings(VisualScriptViewSettings* settings)
	{
		m_Views = settings;
		if (m_Views != nullptr)
		{
			m_BreakOnErrors = m_Views->IsBreakOnErrors();
		}
		// The store is read again from the views settings (or emptied) for the project followed.
		m_ProjectFollowed = false;
		m_ConfigurationDirty = true;
		Follow();
	}

	// --- Breakpoints ---

	VisualScriptBreakpointStore const& VisualScriptDebugSession::GetBreakpoints() const
	{
		FollowProject();
		return m_Breakpoints;
	}

	Result<void> VisualScriptDebugSession::SetBreakpoint(AssetHandle graph, VisualScriptBreakpoint breakpoint)
	{
		Follow();
		Result<Ref<VisualScriptDocument const>> document = LoadDocument(graph);
		if (!document)
		{
			return Error{document.GetError()};
		}
		VisualScriptNode const* node = FindNode(*document.GetValue(), breakpoint.Node);
		if (node == nullptr)
		{
			return MakeError("{} has no node {}", DescribeGraph(graph), breakpoint.Node);
		}
		breakpoint.Type = node->Type;
		uint64_t const revision = m_Breakpoints.GetRevision();
		if (Result<void> set = m_Breakpoints.Set(graph, std::move(breakpoint)); !set)
		{
			return set;
		}
		RememberDocument(graph, document.GetValue());
		if (m_Breakpoints.GetRevision() != revision)
		{
			std::array<AssetHandle, 1> const graphs = {graph};
			OnBreakpointsChanged(graphs);
		}
		return {};
	}

	void VisualScriptDebugSession::RemoveBreakpoints(AssetHandle graph, std::span<uint32_t const> nodes)
	{
		Follow();
		bool removed = false;
		for (uint32_t const node : nodes)
		{
			removed = m_Breakpoints.Remove(graph, node) || removed;
		}
		if (removed)
		{
			std::array<AssetHandle, 1> const graphs = {graph};
			OnBreakpointsChanged(graphs);
		}
	}

	void VisualScriptDebugSession::ToggleBreakpoints(AssetHandle graph, std::span<uint32_t const> nodes)
	{
		Follow();
		Result<Ref<VisualScriptDocument const>> document = LoadDocument(graph);
		if (!document)
		{
			ST_WARN("Visual script breakpoints: {}", document.GetError());
			return;
		}
		std::vector<VisualScriptBreakpoint> breakpoints;
		for (uint32_t const id : nodes)
		{
			if (VisualScriptNode const* node = FindNode(*document.GetValue(), id))
			{
				VisualScriptBreakpoint breakpoint;
				breakpoint.Node = id;
				breakpoint.Type = node->Type;
				breakpoints.push_back(std::move(breakpoint));
			}
		}
		if (breakpoints.empty())
		{
			return;
		}
		uint64_t const revision = m_Breakpoints.GetRevision();
		if (Result<void> toggled = m_Breakpoints.Toggle(graph, breakpoints); !toggled)
		{
			ST_WARN("Visual script breakpoints of {}: {}", DescribeGraph(graph), toggled.GetError());
			return;
		}
		RememberDocument(graph, document.GetValue());
		if (m_Breakpoints.GetRevision() != revision)
		{
			std::array<AssetHandle, 1> const graphs = {graph};
			OnBreakpointsChanged(graphs);
		}
	}

	void VisualScriptDebugSession::SetBreakpointsEnabled(AssetHandle graph, std::span<uint32_t const> nodes, bool enabled)
	{
		Follow();
		uint64_t const revision = m_Breakpoints.GetRevision();
		for (uint32_t const node : nodes)
		{
			VisualScriptBreakpoint const* existing = m_Breakpoints.Find(graph, node);
			if (existing == nullptr || existing->Enabled == enabled)
			{
				continue;
			}
			VisualScriptBreakpoint changed = *existing;
			changed.Enabled = enabled;
			Result<void> const set = m_Breakpoints.Set(graph, std::move(changed));
			ST_ASSERT(set.IsOk(), "A breakpoint the store keeps is kept again with another Enabled");
			(void)set;
		}
		if (m_Breakpoints.GetRevision() != revision)
		{
			std::array<AssetHandle, 1> const graphs = {graph};
			OnBreakpointsChanged(graphs);
		}
	}

	void VisualScriptDebugSession::ClearBreakpoints(std::optional<AssetHandle> graph)
	{
		Follow();
		std::vector<AssetHandle> const graphs = graph ? std::vector<AssetHandle>{*graph} : m_Breakpoints.GetGraphs();
		uint64_t const revision = m_Breakpoints.GetRevision();
		m_Breakpoints.Clear(graph);
		if (m_Breakpoints.GetRevision() != revision)
		{
			OnBreakpointsChanged(graphs);
		}
	}

	void VisualScriptDebugSession::SetBreakOnErrors(bool breakOnErrors)
	{
		Follow();
		if (m_Views != nullptr)
		{
			m_Views->SetBreakOnErrors(breakOnErrors);
		}
		if (m_BreakOnErrors == breakOnErrors)
		{
			return;
		}
		m_BreakOnErrors = breakOnErrors;
		m_ConfigurationDirty = true;
		m_Revision++;
		PushConfiguration();
	}

	// --- Targets and watches ---

	void VisualScriptDebugSession::SetTarget(AssetHandle graph, UUID entity)
	{
		Follow();
		if (!graph.IsValid())
		{
			return;
		}
		auto const found = std::find_if(m_Targets.begin(), m_Targets.end(),
		                                [graph](VisualScriptDebugTarget const& target)
		                                {
											return target.Graph == graph;
										});
		if (found != m_Targets.end() ? found->Entity == entity : !entity.IsValid())
		{
			return;
		}
		if (!entity.IsValid())
		{
			m_Targets.erase(found);
		}
		else if (found != m_Targets.end())
		{
			found->Entity = entity;
		}
		else
		{
			m_Targets.push_back({graph, entity});
		}
		m_ConfigurationDirty = true;
		m_Revision++;
		PushConfiguration();
		if (m_TargetListener)
		{
			m_TargetListener(graph, entity);
		}
	}

	std::vector<VisualScriptDebugTarget> const& VisualScriptDebugSession::GetTargets() const
	{
		FollowProject();
		return m_Targets;
	}

	UUID VisualScriptDebugSession::GetTarget(AssetHandle graph) const
	{
		FollowProject();
		auto const found = std::find_if(m_Targets.begin(), m_Targets.end(),
		                                [graph](VisualScriptDebugTarget const& target)
		                                {
											return target.Graph == graph;
										});
		return found != m_Targets.end() ? found->Entity : UUID::Invalid();
	}

	void VisualScriptDebugSession::SetTargetListener(std::function<void(AssetHandle graph, UUID entity)> listener)
	{
		m_TargetListener = std::move(listener);
	}

	void VisualScriptDebugSession::SetWatches(std::vector<VisualScriptDebugWatch> watches)
	{
		if (watches == m_Watches)
		{
			return;
		}
		m_Watches = std::move(watches);
		// Accumulations of watches that went are dropped; new ones start empty, as of now.
		std::erase_if(m_Flows,
		              [this](WatchFlow const& flow)
		              {
						  return std::find(m_Watches.begin(), m_Watches.end(), flow.Watch) == m_Watches.end();
					  });
		for (VisualScriptDebugWatch const& watch : m_Watches)
		{
			bool const known = std::any_of(m_Flows.begin(), m_Flows.end(),
			                               [&watch](WatchFlow const& flow)
			                               {
											   return flow.Watch == watch;
										   });
			if (!known)
			{
				WatchFlow flow;
				flow.Watch = watch;
				flow.TakenAtFrame = m_FrameCount;
				m_Flows.push_back(std::move(flow));
			}
		}
		m_ConfigurationDirty = true;
		Follow();
	}

	// --- Control ---

	Result<VisualScriptResumeOutcome> VisualScriptDebugSession::Resume(std::optional<uint64_t> stop, VisualScriptDebugAction action,
	                                                                   bool all)
	{
		Follow();
		if (std::optional<std::string> refusal = GetControlRefusal())
		{
			return Error{std::move(*refusal)};
		}
		SyncPause();
		if (!HasResumableStop())
		{
			if (action == VisualScriptDebugAction::Continue)
			{
				if (stop && FindStop(*stop) == nullptr)
				{
					return MakeError("no stop has the ID {}", *stop);
				}
				VisualScriptResumeOutcome outcome;
				outcome.StopRevision = m_State.StopRevision;
				bool const dismissing = !m_State.Stops.empty();
				if (dismissing)
				{
					Result<VisualScriptResumeOutcome> dismissed = VisualScriptDebugger::Resume(stop, action, !stop.has_value());
					if (!dismissed)
					{
						return Error{dismissed.GetError()};
					}
					outcome = dismissed.GetValue();
					RefreshState(StateOrigin::Control);
				}
				bool const held = IsPauseHeld();
				bool const stepEnded = IsStepEnded();
				if (!dismissing && !held && !stepEnded)
				{
					return Error{"nothing is stopped: no execution to continue"};
				}
				m_StepEnded = false;
				m_Revision++;
				if (held)
				{
					ResumePlay();
					outcome.Result = VisualScriptResumeResult::Resumed;
				}
				else if (!dismissing)
				{
					outcome.Result = VisualScriptResumeResult::Ended;
				}
				Notify();
				return outcome;
			}
			if (IsStepEnded() && !m_State.Stepping)
			{
				if (action == VisualScriptDebugAction::StepOut)
				{
					return Error{"the execution ended: nothing to step out of"};
				}
				// The next stop point of the ended execution's graph, once play runs on.
				std::optional<AssetHandle> const graph = m_StepGraph.IsValid() ? std::optional<AssetHandle>(m_StepGraph) : std::nullopt;
				if (Result<void> armed = VisualScriptDebugger::Break(false, graph); !armed)
				{
					return Error{armed.GetError()};
				}
				m_StepEnded = false;
				m_Revision++;
				if (IsPauseHeld())
				{
					ResumePlay();
				}
				VisualScriptResumeOutcome outcome;
				outcome.Result = VisualScriptResumeResult::Armed;
				outcome.StopRevision = m_State.StopRevision;
				return outcome;
			}
			return Error{"nothing is stopped: no execution to step"};
		}

		ResumeScope const scope = MakeResumeScope(stop, all);
		Result<VisualScriptResumeOutcome> resumed = VisualScriptDebugger::Resume(stop, action, all);
		if (!resumed)
		{
			return Error{resumed.GetError()};
		}
		return FinishResume(resumed.GetValue(), action, scope, true).Outcome;
	}

	Result<void> VisualScriptDebugSession::Break(bool cancel, std::optional<AssetHandle> graph)
	{
		Follow();
		if (std::optional<std::string> refusal = GetControlRefusal())
		{
			return Error{std::move(*refusal)};
		}
		if (cancel && graph)
		{
			return Error{"cancel takes no graph: it cancels the armed break and the step"};
		}
		if (Result<void> armed = VisualScriptDebugger::Break(cancel, graph); !armed)
		{
			return armed;
		}
		if (cancel)
		{
			// A cancelled step ends in Running, not StepEnded.
			m_StepEnded = false;
		}
		RefreshState(StateOrigin::Control);
		m_Revision++;
		Notify();
		return {};
	}

	Result<VisualScriptResumeOutcome> VisualScriptDebugSession::RunTo(AssetHandle document, uint32_t node)
	{
		Follow();
		if (std::optional<std::string> refusal = GetControlRefusal())
		{
			return Error{std::move(*refusal)};
		}
		Result<Ref<VisualScriptDocument const>> stored = LoadDocument(document);
		if (!stored)
		{
			return Error{stored.GetError()};
		}
		if (FindNode(*stored.GetValue(), node) == nullptr)
		{
			return MakeError("{} has no node {}", DescribeGraph(document), node);
		}
		if (Result<void> armed = VisualScriptDebugger::RunTo(document, node); !armed)
		{
			return Error{armed.GetError()};
		}
		SyncPause();
		if (!m_State.Stops.empty())
		{
			ResumeScope const scope = MakeResumeScope(std::nullopt, true);
			Result<VisualScriptResumeOutcome> resumed = VisualScriptDebugger::Resume(std::nullopt, VisualScriptDebugAction::Continue, true);
			if (!resumed)
			{
				return Error{resumed.GetError()};
			}
			return FinishResume(resumed.GetValue(), VisualScriptDebugAction::Continue, scope, true).Outcome;
		}
		m_StepEnded = false;
		m_Revision++;
		if (IsPauseHeld())
		{
			ResumePlay();
		}
		VisualScriptResumeOutcome outcome;
		outcome.Result = VisualScriptResumeResult::Armed;
		outcome.StopRevision = m_State.StopRevision;
		return outcome;
	}

	void VisualScriptDebugSession::RequestResume(std::optional<uint64_t> stop, VisualScriptDebugAction action, bool all,
	                                             bool thenResumePlay)
	{
		QueuedRequest request;
		request.RequestKind = QueuedRequest::Kind::Resume;
		request.Stop = stop;
		request.Action = action;
		request.All = all;
		request.ThenResumePlay = thenResumePlay;
		m_Requests.push_back(std::move(request));
	}

	void VisualScriptDebugSession::RequestRunTo(AssetHandle document, uint32_t node)
	{
		QueuedRequest request;
		request.RequestKind = QueuedRequest::Kind::RunTo;
		request.Document = document;
		request.Node = node;
		m_Requests.push_back(std::move(request));
	}

	void VisualScriptDebugSession::RequestOnlyInstance(uint64_t stop)
	{
		VisualScriptStop const* const record = FindStop(stop);
		if (record == nullptr || !record->Resumable)
		{
			return;
		}
		SetTarget(record->Graph, record->Entity);
		QueuedRequest request;
		request.RequestKind = QueuedRequest::Kind::OnlyInstance;
		request.Stop = stop;
		request.Document = record->Graph;
		m_Requests.push_back(std::move(request));
	}

	void VisualScriptDebugSession::SelectStop(uint64_t stop)
	{
		if (FindStop(stop) == nullptr || m_SelectedStop == stop)
		{
			return;
		}
		m_SelectedStop = stop;
		m_SelectedFrame = 0;
		m_Revision++;
	}

	void VisualScriptDebugSession::SelectFrame(uint32_t frame)
	{
		if (m_SelectedFrame == frame)
		{
			return;
		}
		m_SelectedFrame = frame;
		m_Revision++;
	}

	std::optional<std::string> VisualScriptDebugSession::GetFrameStepRefusal() const
	{
		if (!HasResumableStop())
		{
			return std::nullopt;
		}
		VisualScriptStop const* current = m_State.Current ? FindStop(*m_State.Current) : nullptr;
		if (current == nullptr || !current->Resumable)
		{
			auto const resumable = std::find_if(m_State.Stops.begin(), m_State.Stops.end(),
			                                    [](VisualScriptStop const& candidate)
			                                    {
													return candidate.Resumable;
												});
			current = &*resumable;
		}
		return fmt::format("an execution is stopped at a breakpoint (stop {}): graph.continue or graph.step it first", current->ID);
	}

	std::optional<std::string> VisualScriptDebugSession::GetFrameStepRefusalForUI() const
	{
		return HasResumableStop() ? std::optional<std::string>(std::string(RefusalForUI)) : std::nullopt;
	}

	// --- What is shown ---

	VisualScriptDebugSessionState VisualScriptDebugSession::GetSessionState() const
	{
		if (!m_Attached)
		{
			return VisualScriptDebugSessionState::Detached;
		}
		if (!m_State.Stops.empty())
		{
			return VisualScriptDebugSessionState::Stopped;
		}
		if (m_State.Stepping)
		{
			return VisualScriptDebugSessionState::Stepping;
		}
		return IsStepEnded() ? VisualScriptDebugSessionState::StepEnded : VisualScriptDebugSessionState::Running;
	}

	std::optional<uint64_t> VisualScriptDebugSession::GetSelectedStop() const
	{
		if (m_SelectedStop && FindStop(*m_SelectedStop) != nullptr)
		{
			return m_SelectedStop;
		}
		return m_State.Current;
	}

	VisualScriptStopValues const* VisualScriptDebugSession::GetStopValues(uint64_t stop, uint32_t frame)
	{
		if (!m_Attached || FindStop(stop) == nullptr)
		{
			return nullptr;
		}
		auto [entry, added] = m_StopValues.try_emplace(std::make_pair(stop, frame));
		if (added)
		{
			Result<VisualScriptStopValues> values = VisualScriptDebugger::DescribeStop(stop, frame);
			if (values)
			{
				entry->second = values.TakeValue();
			}
		}
		return entry->second ? &*entry->second : nullptr;
	}

	VisualScriptFlowTake VisualScriptDebugSession::TakeFlow(AssetHandle document, uint32_t function)
	{
		ST_PROFILE_SCOPE("GraphDebug.TakeFlow");
		VisualScriptFlowTake take;
		take.Time = m_FlowTime;
		auto const found = std::find_if(m_Flows.begin(), m_Flows.end(),
		                                [document, function](WatchFlow const& flow)
		                                {
											return flow.Watch.Document == document && flow.Watch.Function == function;
										});
		if (found == m_Flows.end())
		{
			take.Held = true;
			return take;
		}
		take.Frames = static_cast<uint32_t>(std::min<uint64_t>(m_FrameCount - found->TakenAtFrame, std::numeric_limits<uint32_t>::max()));
		take.Held = take.Frames == 0;
		take.Steps = std::move(found->Steps);
		found->Steps.clear();
		found->Index.clear();
		found->TakenAtFrame = m_FrameCount;
		return take;
	}

	uint64_t VisualScriptDebugSession::GetRevision() const
	{
		FollowProject();
		return m_Revision;
	}

	// --- PlayModeObserver ---

	void VisualScriptDebugSession::OnPlayStarting(EditorPlayState state, PlayStartOptions const& options)
	{
		Follow();
		if (m_Attached)
		{
			VisualScriptDebugger::Detach();
			m_Attached = false;
		}
		ResetAttachment();
		m_FlowTime = 0.0;
		m_FrameCount = 0;
		for (WatchFlow& flow : m_Flows)
		{
			flow.Steps.clear();
			flow.Index.clear();
			flow.TakenAtFrame = 0;
		}
		m_Revision++;
		// Simulate runs no graph, and test runs ignore breakpoints; without the runtime there is nothing to attach.
		if (state != EditorPlayState::Play || options.TestRun || !ScriptEngine::IsInitialized())
		{
			return;
		}
		if (Result<void> attached = VisualScriptDebugger::Attach(); !attached)
		{
			ST_WARN("The visual script debugger cannot attach: {}", attached.GetError());
			return;
		}
		m_Attached = true;
		m_ConfigurationDirty = true;
		PushConfiguration();
	}

	bool VisualScriptDebugSession::OnPlayFrame(Timestep timestep)
	{
		ST_PROFILE_SCOPE("GraphDebug.Frame");
		if (!m_Attached)
		{
			return false;
		}
		// The start's call (the scene's OnCreate ran) is no scene frame: the flow clock and the frame count stay.
		if (m_StartReported)
		{
			m_FlowTime += static_cast<double>(timestep.GetSeconds());
			m_FrameCount++;
		}
		else
		{
			// The running scene the start made is the one stops were made in.
			m_SceneVersionSeen = m_Context.GetSceneVersion();
		}
		m_StartReported = true;
		m_FrameRan = true;

		// Frames run only without a resumable stop. Without a step either, only a stop made in the frame changes the stop
		// revision (after the dismissals below), which tells a stop that already ended in its frame.
		bool const quiet = !m_State.Stepping && !HasResumableStop();
		uint64_t quietRevision = m_State.StopRevision;

		// A frame dismisses the read-only stops of the frames before it.
		std::vector<uint64_t> readOnly;
		for (VisualScriptStop const& stop : m_State.Stops)
		{
			if (!stop.Resumable)
			{
				readOnly.push_back(stop.ID);
			}
		}
		bool dismissed = false;
		for (uint64_t const stop : readOnly)
		{
			Result<VisualScriptResumeOutcome> const resumed = VisualScriptDebugger::Resume(stop, VisualScriptDebugAction::Continue, false);
			if (resumed)
			{
				dismissed = true;
				quietRevision = resumed.GetValue().StopRevision;
			}
		}

		std::optional<uint64_t> const stopRevision = TakeTrace();
		bool hold = false;
		if (dismissed || !stopRevision || *stopRevision != m_State.StopRevision)
		{
			StateChange change;
			if (quiet)
			{
				change.QuietRevision = quietRevision;
			}
			hold = RefreshState(StateOrigin::Frame, change).value_or(false);
		}
		m_SceneVersionSeen = m_Context.GetSceneVersion();
		if (hold)
		{
			// Paused before the listeners hear of it (play mode pauses the scene too when this answers true); the pause is the
			// debugger's unless it was paused already.
			HoldPlay();
			m_Holds++;
		}
		Notify();
		return hold;
	}

	void VisualScriptDebugSession::OnPlayStopping()
	{
		if (m_Attached)
		{
			VisualScriptDebugger::Detach();
			m_Attached = false;
		}
		ResetAttachment();
		m_Revision++;
		CompleteWaits(true);
	}

	bool VisualScriptDebugSession::OnPlayResuming()
	{
		if (!m_Attached)
		{
			return false;
		}
		Refresh();
		bool held = false;
		if (!m_State.Stops.empty())
		{
			ResumeScope const scope = MakeResumeScope(std::nullopt, true);
			Result<VisualScriptResumeOutcome> resumed = VisualScriptDebugger::Resume(std::nullopt, VisualScriptDebugAction::Continue, true);
			if (resumed)
			{
				held = FinishResume(resumed.GetValue(), VisualScriptDebugAction::Continue, scope, false).Held;
			}
			else
			{
				ST_WARN("The stopped visual script executions cannot continue: {}", resumed.GetError());
			}
		}
		if (!m_Attached)
		{
			return false;
		}
		// A continued chain that stopped again, or whose new stop ended at once, keeps the scene paused.
		bool const stoppedAgain = HasResumableStop() || held;
		if (!stoppedAgain)
		{
			// Play mode unpauses the scene next: the debugger's pause and an ended step end with it.
			m_StepEnded = false;
			if (IsPauseHeld())
			{
				m_PauseHeld = false;
				if (m_ResumeListener)
				{
					m_ResumeListener();
				}
			}
			m_Revision++;
		}
		return stoppedAgain;
	}

	PlayDebugSummary VisualScriptDebugSession::GetDebugSummary() const
	{
		PlayDebugSummary summary;
		summary.State = std::string(VisualScriptDebugSessionStateToString(GetSessionState()));
		summary.Stops = static_cast<uint32_t>(m_State.Stops.size());
		summary.Current = m_State.Current;
		summary.PauseHeld = IsPauseHeld();
		summary.StopsMade = m_StopsMade;
		summary.Holds = m_Holds;
		return summary;
	}

	// --- The editor frame ---

	void VisualScriptDebugSession::BeginFrame()
	{
		Follow();
		// Requests a resume queues (none should) run at the next frame.
		std::vector<QueuedRequest> const requests = std::move(m_Requests);
		m_Requests.clear();
		for (QueuedRequest const& request : requests)
		{
			RunRequest(request);
		}
	}

	void VisualScriptDebugSession::Refresh()
	{
		Follow();
		FollowGraphs();
		if (m_Attached)
		{
			SyncPause();
			Poll();
		}
	}

	void VisualScriptDebugSession::EndFrame()
	{
		ST_PROFILE_SCOPE("GraphDebug.EndFrame");
		Follow();
		FollowGraphs();
		if (m_Attached)
		{
			SyncPause();
			// Edits, inspector changes and resumes change what is stopped while no frame runs; a frame's end read it already.
			if (!m_FrameRan)
			{
				Poll();
			}
		}
		m_FrameRan = false;
		CompleteWaits(false);
	}

	void VisualScriptDebugSession::WaitForStop(double timeoutSeconds, CommandCompletion completion)
	{
		double const seconds = std::isfinite(timeoutSeconds) ? std::clamp(timeoutSeconds, MinWaitSeconds, MaxWaitSeconds) : MaxWaitSeconds;
		PendingWait wait{std::chrono::steady_clock::now() +
		                     std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds)),
		                 std::move(completion)};
		m_Waits.push_back(std::move(wait));
		// What can be answered at once is: a stop, nothing playing, no frame that can run.
		CompleteWaits(false);
	}

	bool VisualScriptDebugSession::HasPausedScene() const
	{
		return IsPauseHeld();
	}

	void VisualScriptDebugSession::SetStopListener(std::function<void(VisualScriptStop const& stop)> listener)
	{
		m_StopListener = std::move(listener);
	}

	void VisualScriptDebugSession::SetResumeListener(std::function<void()> listener)
	{
		m_ResumeListener = std::move(listener);
	}

	void VisualScriptDebugSession::SetStopEndedListener(std::function<void(AssetHandle graph, std::string const& reason)> listener)
	{
		m_StopEndedListener = std::move(listener);
	}

	// --- Private ---

	bool VisualScriptDebugSession::FollowProject() const
	{
		Project const* const project = m_Context.GetProject();
		std::filesystem::path const file = project != nullptr ? project->GetFilePath() : std::filesystem::path();
		if (m_ProjectFollowed && file == m_FollowedProject)
		{
			return false;
		}
		m_ProjectFollowed = true;
		m_FollowedProject = file;
		m_FollowedViewsProject = file.empty() ? std::filesystem::path() : RecentProjects::NormalizePath(file);
		m_Breakpoints.Clear(std::nullopt);
		m_FollowedDocuments.clear();
		// Targets name the entities of the project's scenes.
		m_Targets.clear();
		if (m_Views != nullptr && !file.empty())
		{
			for (AssetHandle const graph : m_Views->GetBreakpointGraphs(file))
			{
				for (VisualScriptBreakpoint const& breakpoint : m_Views->GetBreakpoints(file, graph))
				{
					// The views settings keep only what the store accepts.
					Result<void> const set = m_Breakpoints.Set(graph, breakpoint);
					ST_ASSERT(set.IsOk(), "The views settings keep breakpoints the store accepts");
					(void)set;
				}
			}
		}
		m_ConfigurationDirty = true;
		m_Revision++;
		return true;
	}

	void VisualScriptDebugSession::Follow()
	{
		FollowProject();
		PushConfiguration();
	}

	void VisualScriptDebugSession::PushConfiguration()
	{
		if (!m_Attached || !m_ConfigurationDirty)
		{
			return;
		}
		m_ConfigurationDirty = false;
		VisualScriptDebugConfiguration configuration = BuildConfiguration();
		if (m_Pushed)
		{
			configuration.Revision = m_Pushed->Revision;
			if (configuration == *m_Pushed)
			{
				return;
			}
		}
		configuration.Revision = ++m_ConfigurationRevision;
		if (Result<void> configured = VisualScriptDebugger::Configure(configuration); !configured)
		{
			ST_WARN("The visual script debugger refused its configuration: {}", configured.GetError());
			return;
		}
		m_Pushed = std::move(configuration);
	}

	VisualScriptDebugConfiguration VisualScriptDebugSession::BuildConfiguration() const
	{
		VisualScriptDebugConfiguration configuration;
		for (AssetHandle const graph : m_Breakpoints.GetGraphs())
		{
			for (VisualScriptBreakpoint const& breakpoint : m_Breakpoints.Get(graph))
			{
				VisualScriptBreakpointSetting setting;
				setting.Document = graph;
				setting.Node = breakpoint.Node;
				setting.Type = breakpoint.Type;
				setting.Enabled = breakpoint.Enabled;
				setting.Condition = breakpoint.Condition;
				setting.HitCount = breakpoint.HitCount;
				configuration.Breakpoints.push_back(std::move(setting));
			}
		}
		configuration.Targets = m_Targets;
		configuration.Watches = m_Watches;
		configuration.BreakOnErrors = m_BreakOnErrors;
		return configuration;
	}

	void VisualScriptDebugSession::OnBreakpointsChanged(std::span<AssetHandle const> graphs)
	{
		if (m_Views != nullptr)
		{
			for (AssetHandle const graph : graphs)
			{
				m_Views->SetBreakpoints(m_FollowedProject, graph, m_Breakpoints.Get(graph));
			}
		}
		m_ConfigurationDirty = true;
		m_Revision++;
		PushConfiguration();
	}

	void VisualScriptDebugSession::FollowGraphs()
	{
		if (m_Breakpoints.IsEmpty())
		{
			return;
		}
		// The views settings' entry of the project followed: a graph it no longer keeps breakpoints for was dropped there.
		VisualScriptProjectViews const* views = nullptr;
		if (m_Views != nullptr)
		{
			std::vector<VisualScriptProjectViews> const& projects = m_Views->GetViews().Projects;
			auto const found = std::find_if(projects.begin(), projects.end(),
			                                [this](VisualScriptProjectViews const& project)
			                                {
												return project.Project == m_FollowedViewsProject;
											});
			views = found != projects.end() ? &*found : nullptr;
		}

		std::vector<AssetHandle> changed;
		bool dropped = false;
		for (AssetHandle const graph : m_Breakpoints.GetGraphs())
		{
			bool const deleted = !AssetManager::IsInitialized() || !AssetManager::IsValid(graph);
			VisualScriptGraphViews const* const graphViews = views != nullptr ? views->FindGraph(graph) : nullptr;
			bool const forgotten = m_Views != nullptr && (graphViews == nullptr || graphViews->Breakpoints.empty());
			if (deleted || forgotten)
			{
				m_Breakpoints.Clear(graph);
				m_FollowedDocuments.erase(graph);
				dropped = true;
				continue;
			}
			if (!AssetManager::IsLoaded(graph))
			{
				continue;
			}
			Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(graph);
			if (asset == nullptr)
			{
				continue;
			}
			Ref<VisualScriptDocument const> const& document = asset->GetSharedDocument();
			std::weak_ptr<VisualScriptDocument const>& followed = m_FollowedDocuments[graph];
			if (followed.lock() == document)
			{
				continue;
			}
			followed = document;
			if (m_Breakpoints.Follow(graph, *document))
			{
				changed.push_back(graph);
			}
		}
		if (!changed.empty())
		{
			OnBreakpointsChanged(changed);
		}
		else if (dropped)
		{
			m_ConfigurationDirty = true;
			m_Revision++;
			PushConfiguration();
		}
	}

	void VisualScriptDebugSession::RememberDocument(AssetHandle graph, Ref<VisualScriptDocument const> const& document)
	{
		m_FollowedDocuments[graph] = document;
	}

	std::optional<std::string> VisualScriptDebugSession::GetControlRefusal() const
	{
		if (!m_Context.IsPlaying())
		{
			return std::string("the editor is not playing: play.start starts the game");
		}
		if (m_Context.GetPlayState() == EditorPlayState::Simulate)
		{
			return std::string("visual scripts do not run while simulating");
		}
		if (!m_Attached)
		{
			return std::string("the visual script debugger is not attached: test runs and games without the scripting runtime run "
			                   "undebugged");
		}
		return std::nullopt;
	}

	void VisualScriptDebugSession::Poll()
	{
		std::optional<uint64_t> const stopRevision = TakeTrace();
		if (!stopRevision || *stopRevision != m_State.StopRevision)
		{
			if (RefreshState(StateOrigin::Poll).value_or(false))
			{
				HoldPlay();
			}
		}
		Notify();
	}

	bool VisualScriptDebugSession::HasResumableStop() const
	{
		return std::any_of(m_State.Stops.begin(), m_State.Stops.end(),
		                   [](VisualScriptStop const& stop)
		                   {
							   return stop.Resumable;
						   });
	}

	VisualScriptStop const* VisualScriptDebugSession::FindStop(uint64_t stop) const
	{
		auto const found = std::find_if(m_State.Stops.begin(), m_State.Stops.end(),
		                                [stop](VisualScriptStop const& candidate)
		                                {
											return candidate.ID == stop;
										});
		return found != m_State.Stops.end() ? &*found : nullptr;
	}

	bool VisualScriptDebugSession::IsPauseHeld() const
	{
		return m_Attached && m_PauseHeld && m_PlayMode.IsPaused();
	}

	bool VisualScriptDebugSession::IsStepEnded() const
	{
		return m_Attached && m_StepEnded && m_PlayMode.IsPaused();
	}

	void VisualScriptDebugSession::SyncPause()
	{
		// Whoever unpaused the scene (the toolbar's Pause with nothing stopped, play.pause) ended the hold and the step.
		if (!m_PlayMode.IsPaused())
		{
			m_PauseHeld = false;
			m_StepEnded = false;
		}
	}

	void VisualScriptDebugSession::ResumePlay()
	{
		if (!m_PlayMode.SetPaused(false))
		{
			return;
		}
		m_PauseHeld = false;
		m_Revision++;
		if (m_ResumeListener)
		{
			m_ResumeListener();
		}
	}

	void VisualScriptDebugSession::HoldPlay()
	{
		if (!m_PlayMode.IsPaused())
		{
			m_PlayMode.SetPaused(true);
			m_PauseHeld = true;
		}
		m_PlayMode.CancelSteps();
	}

	std::optional<bool> VisualScriptDebugSession::RefreshState(StateOrigin origin)
	{
		return RefreshState(origin, StateChange());
	}

	std::optional<bool> VisualScriptDebugSession::RefreshState(StateOrigin origin, StateChange const& change)
	{
		Result<VisualScriptDebugState> state = VisualScriptDebugger::GetState();
		if (!state)
		{
			ST_WARN("The visual script debugger's state cannot be read: {}", state.GetError());
			return std::nullopt;
		}
		return ApplyState(state.TakeValue(), origin, change);
	}

	bool VisualScriptDebugSession::ApplyState(VisualScriptDebugState state, StateOrigin origin, StateChange const& change)
	{
		bool const wasStepping = m_State.Stepping.has_value();
		uint64_t const lastSeen = m_LastStopID;
		auto const kept = [&state](uint64_t id)
		{
			return std::any_of(state.Stops.begin(), state.Stops.end(),
			                   [id](VisualScriptStop const& stop)
			                   {
								   return stop.ID == id;
							   });
		};
		bool newStop = false;
		for (VisualScriptStop const& stop : state.Stops)
		{
			if (stop.ID > lastSeen)
			{
				newStop = true;
				m_StopsMade++;
				m_LastStopID = std::max(m_LastStopID, stop.ID);
			}
		}
		// A stop made and ended before this read: the stop a resume reached that a scene load or a destruction its code asked
		// for ended at once, or one a quiet frame made and ended (nothing else changes the stop revision then).
		bool madeAndEnded = false;
		if (change.Reached && *change.Reached > lastSeen && !kept(*change.Reached))
		{
			madeAndEnded = true;
			m_StopsMade++;
			m_LastStopID = std::max(m_LastStopID, *change.Reached);
		}
		else if (change.QuietRevision && !newStop && !state.Stepping && state.StopRevision != *change.QuietRevision)
		{
			madeAndEnded = true;
			m_StopsMade++;
		}
		// The resumable stop that ended last without a resume, for the stop-ended listener: a resume ends the stops it
		// continued.
		std::optional<VisualScriptStop> ended;
		for (VisualScriptStop const& stop : m_State.Stops)
		{
			bool const resumed = change.Resume != nullptr &&
			                     (change.Resume->All || std::find(change.Resume->Resumed.begin(), change.Resume->Resumed.end(), stop.ID) !=
			                                                change.Resume->Resumed.end());
			if (stop.Resumable && !resumed && !kept(stop.ID))
			{
				ended = stop;
			}
		}
		if (state.StopRevision != m_State.StopRevision)
		{
			m_StopValues.clear();
		}
		if (state != m_State)
		{
			m_Revision++;
		}
		m_State = std::move(state);
		if (newStop)
		{
			m_StepEnded = false;
		}
		if (m_SelectedStop && FindStop(*m_SelectedStop) == nullptr)
		{
			m_SelectedStop.reset();
			m_SelectedFrame = 0;
		}

		bool hold = false;
		bool const resumable = HasResumableStop();
		if (origin == StateOrigin::Frame || origin == StateOrigin::Poll)
		{
			if (newStop || madeAndEnded)
			{
				hold = true;
			}
			else if (wasStepping && !m_State.Stepping && !resumable)
			{
				// The waiting step's execution ended without another stop point, or its fiber was discarded.
				m_StepEnded = true;
				hold = true;
			}
		}
		if ((ended || madeAndEnded) && !resumable && !newStop)
		{
			// The last resumable stop ended without a resume: play stays paused, with the pause held when the debugger held it.
			hold = true;
			uint64_t const sceneVersion = change.Resume != nullptr ? change.Resume->SceneVersion : m_SceneVersionSeen;
			std::string_view reason = StopEndedByEdit;
			if (m_Context.GetSceneVersion() != sceneVersion)
			{
				reason = StopEndedBySceneLoad;
			}
			else if (change.Resume != nullptr || !ended || !m_Context.GetScene().HasEntity(ended->Entity))
			{
				// No edit runs inside a resume or a frame: what one ends (and a stop the session never saw) went with its
				// instance. Between them, an ended stop whose entity is still there was cancelled by an edit.
				reason = StopEndedByDestroy;
			}
			AssetHandle const graph = ended ? ended->Graph : change.Resume != nullptr ? change.Resume->Graph : AssetHandle();
			m_PendingEnded.emplace(graph, std::string(reason));
		}
		return hold;
	}

	void VisualScriptDebugSession::Notify()
	{
		if (m_PendingEnded)
		{
			std::pair<AssetHandle, std::string> const ended = std::move(*m_PendingEnded);
			m_PendingEnded.reset();
			if (m_StopEndedListener)
			{
				m_StopEndedListener(ended.first, ended.second);
			}
		}
		if (m_State.Current != m_NotifiedCurrent)
		{
			m_NotifiedCurrent = m_State.Current;
			if (VisualScriptStop const* const current = m_State.Current ? FindStop(*m_State.Current) : nullptr; current && m_StopListener)
			{
				m_StopListener(*current);
			}
		}
	}

	void VisualScriptDebugSession::MergeTrace(VisualScriptFrameTrace const& trace)
	{
		for (VisualScriptGraphTrace const& graph : trace.Graphs)
		{
			auto const flow =
				std::find_if(m_Flows.begin(), m_Flows.end(),
			                 [&graph](WatchFlow const& candidate)
			                 {
								 return candidate.Watch.Document == graph.Document && candidate.Watch.Function == graph.Function;
							 });
			if (flow == m_Flows.end())
			{
				continue;
			}
			// In the order the frame first saw them, so that new steps keep the frame's order after the earlier frames'.
			std::vector<size_t> order(graph.Steps.size());
			std::iota(order.begin(), order.end(), size_t(0));
			std::stable_sort(order.begin(), order.end(),
			                 [&graph](size_t a, size_t b)
			                 {
								 return graph.Steps[a].FirstOrder < graph.Steps[b].FirstOrder;
							 });
			for (size_t const index : order)
			{
				VisualScriptTraceStep const& step = graph.Steps[index];
				auto [entry, added] = flow->Index.try_emplace(std::make_pair(step.Node, step.Exit), flow->Steps.size());
				if (added)
				{
					VisualScriptFlowStep merged;
					merged.Node = step.Node;
					merged.Exit = step.Exit;
					merged.FirstOrder = static_cast<uint32_t>(flow->Steps.size());
					flow->Steps.push_back(std::move(merged));
				}
				VisualScriptFlowStep& merged = flow->Steps[entry->second];
				constexpr uint32_t MaxCount = std::numeric_limits<uint32_t>::max();
				merged.Count = merged.Count > MaxCount - step.Count ? MaxCount : merged.Count + step.Count;
				merged.LastTime = m_FlowTime;
			}
		}
	}

	std::optional<uint64_t> VisualScriptDebugSession::TakeTrace()
	{
		Result<VisualScriptFrameTrace> trace = VisualScriptDebugger::TakeTrace();
		if (!trace)
		{
			if (!m_TraceFailureLogged)
			{
				m_TraceFailureLogged = true;
				ST_WARN("The visual script debugger's flow trace cannot be read, so the graph windows show no flow: {}", trace.GetError());
			}
			return std::nullopt;
		}
		MergeTrace(trace.GetValue());
		return trace.GetValue().StopRevision;
	}

	VisualScriptDebugSession::ResumeScope VisualScriptDebugSession::MakeResumeScope(std::optional<uint64_t> stop, bool all) const
	{
		ResumeScope scope;
		scope.All = all;
		scope.WasPaused = m_PlayMode.IsPaused();
		scope.SceneVersion = m_Context.GetSceneVersion();
		std::optional<uint64_t> const resumed = stop ? stop : m_State.Current;
		if (!all && resumed)
		{
			scope.Resumed.push_back(*resumed);
			if (VisualScriptStop const* const record = FindStop(*resumed))
			{
				scope.Graph = record->Graph;
			}
		}
		return scope;
	}

	VisualScriptDebugSession::FinishedResume VisualScriptDebugSession::FinishResume(VisualScriptResumeOutcome outcome,
	                                                                                VisualScriptDebugAction action,
	                                                                                ResumeScope const& scope, bool managePlay)
	{
		FinishedResume finished;
		finished.Outcome = outcome;
		m_StepEnded = false;
		m_Revision++;
		if (action != VisualScriptDebugAction::Continue)
		{
			m_StepGraph = scope.Graph;
		}
		// What the frame's end would have done after the code the resume ran: destroyed entities go, a requested scene load
		// happens and a quit stops playing (which detaches).
		m_Context.GetScene().FlushPendingDestruction();
		m_PlayMode.FollowScripts();
		if (!m_Attached)
		{
			return finished;
		}
		// A scene the resume loaded starts running: it stays paused as the scene it replaced was, so that the pause the
		// debugger holds, an ended step's and the agent's own survive the load.
		if (scope.WasPaused && m_Context.GetSceneVersion() != scope.SceneVersion && !m_PlayMode.IsPaused())
		{
			m_PlayMode.SetPaused(true);
		}
		// What the resumed code ran lights the flow too.
		TakeTrace();
		StateChange change;
		change.Resume = &scope;
		if (outcome.Result == VisualScriptResumeResult::Stopped)
		{
			change.Reached = outcome.Stop;
		}
		std::optional<bool> const read = RefreshState(StateOrigin::Resume, change);
		m_SceneVersionSeen = m_Context.GetSceneVersion();
		finished.Held = read.value_or(false);
		// A stop the resume reached that ended at once (a scene load or a destruction its code asked for) leaves the execution
		// ended.
		if (read && outcome.Result == VisualScriptResumeResult::Stopped && outcome.Stop && FindStop(*outcome.Stop) == nullptr)
		{
			finished.Outcome.Result = VisualScriptResumeResult::Ended;
			finished.Outcome.Stop.reset();
			finished.Outcome.StopRevision = m_State.StopRevision;
		}
		if (managePlay)
		{
			if (action != VisualScriptDebugAction::Continue && finished.Outcome.Result == VisualScriptResumeResult::Ended)
			{
				// The step ended its execution: the scene stays paused.
				m_StepEnded = !HasResumableStop();
			}
			else if (!finished.Held && finished.Outcome.Result != VisualScriptResumeResult::Stopped && !HasResumableStop() && IsPauseHeld())
			{
				ResumePlay();
			}
		}
		Notify();
		return finished;
	}

	void VisualScriptDebugSession::RunRequest(QueuedRequest const& request)
	{
		if (!m_Attached)
		{
			return;
		}
		SyncPause();
		switch (request.RequestKind)
		{
			case QueuedRequest::Kind::Resume:
			{
				if (request.ThenResumePlay)
				{
					m_PlayMode.Resume();
					return;
				}
				// A request the state already answered (another resume ran first) is dropped.
				bool const applies = HasResumableStop() || (request.Action == VisualScriptDebugAction::Continue
				                                                ? !m_State.Stops.empty() || IsPauseHeld() || IsStepEnded()
				                                                : IsStepEnded() && request.Action != VisualScriptDebugAction::StepOut);
				if (!applies || (request.Stop && FindStop(*request.Stop) == nullptr))
				{
					return;
				}
				if (Result<VisualScriptResumeOutcome> resumed = Resume(request.Stop, request.Action, request.All); !resumed)
				{
					ST_WARN("The visual script debugger cannot resume: {}", resumed.GetError());
				}
				return;
			}
			case QueuedRequest::Kind::RunTo:
				if (Result<VisualScriptResumeOutcome> resumed = RunTo(request.Document, request.Node); !resumed)
				{
					ST_WARN("The visual script debugger cannot run to the node: {}", resumed.GetError());
				}
				return;
			case QueuedRequest::Kind::OnlyInstance:
			{
				std::vector<uint64_t> others;
				for (VisualScriptStop const& stop : m_State.Stops)
				{
					if (stop.Graph == request.Document && stop.Resumable && request.Stop && stop.ID != *request.Stop)
					{
						others.push_back(stop.ID);
					}
				}
				for (uint64_t const other : others)
				{
					if (FindStop(other) == nullptr)
					{
						continue;
					}
					if (Result<VisualScriptResumeOutcome> resumed = Resume(other, VisualScriptDebugAction::Continue); !resumed)
					{
						ST_WARN("The visual script debugger cannot continue stop {}: {}", other, resumed.GetError());
					}
				}
				return;
			}
		}
	}

	void VisualScriptDebugSession::CompleteWaits(bool playStopped)
	{
		if (m_Waits.empty())
		{
			return;
		}
		std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
		std::vector<PendingWait> waits = std::move(m_Waits);
		m_Waits.clear();
		for (PendingWait& wait : waits)
		{
			std::optional<std::string_view> reason;
			if (playStopped)
			{
				reason = "PlayStopped";
			}
			else if (!m_Attached)
			{
				reason = "NotPlaying";
			}
			else if (!m_State.Stops.empty())
			{
				reason = "Stopped";
			}
			else if (m_PlayMode.IsPaused())
			{
				reason = "Paused";
			}
			else if (now >= wait.Deadline)
			{
				reason = "Timeout";
			}
			if (reason)
			{
				wait.Completion.Complete(CommandResult(MakeWaitResult(*reason)));
			}
			else
			{
				m_Waits.push_back(std::move(wait));
			}
		}
	}

	Json VisualScriptDebugSession::MakeWaitResult(std::string_view reason) const
	{
		Json result = Json::object();
		bool const stopped = reason == "Stopped";
		result["stopped"] = stopped;
		result["stop"] = stopped && m_State.Current ? Json(*m_State.Current) : Json();
		result["reason"] = std::string(reason);
		if (reason == "Paused")
		{
			result["hint"] = IsPauseHeld() ? "graph.continue resumes the game" : "the scene is paused: play.advance runs frames";
		}
		else
		{
			result["hint"] = Json();
		}
		return result;
	}

	void VisualScriptDebugSession::ResetAttachment()
	{
		m_Pushed.reset();
		m_ConfigurationDirty = true;
		m_State = VisualScriptDebugState();
		m_LastStopID = 0;
		m_StopsMade = 0;
		m_Holds = 0;
		m_SceneVersionSeen = 0;
		m_PendingEnded.reset();
		m_NotifiedCurrent.reset();
		m_PauseHeld = false;
		m_StepEnded = false;
		m_StepGraph = AssetHandle();
		m_SelectedStop.reset();
		m_SelectedFrame = 0;
		m_StopValues.clear();
		m_TraceFailureLogged = false;
		m_FrameRan = false;
		m_StartReported = false;
		m_Requests.clear();
	}
}
