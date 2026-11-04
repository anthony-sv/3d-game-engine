#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/PlayMode.h"
#include "Editor/VisualScripting/VisualScriptBreakpoints.h"
#include "Editor/VisualScripting/VisualScriptFlowTake.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Core/UUID.h"
#include "Strada/Script/VisualScriptDebugger.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Strada
{
	class EditorContext;
	class VisualScriptViewSettings;
	struct VisualScriptDocument;

	// What the debug session is doing (graph.debug's state).
	enum class VisualScriptDebugSessionState : uint8_t
	{
		// Not playing, simulating, or a test run: no debugger is attached.
		Detached = 0,
		// Attached and nothing stopped (the scene runs, or is paused by whoever paused it).
		Running,
		// A stop exists: a stopped execution, or a read-only stop (a passed breakpoint, an error).
		Stopped,
		// A step's execution waits in a latent node; the step ends at its next stop point.
		Stepping,
		// A step ended its execution; the scene stays paused.
		StepEnded
	};

	// "Detached", "Running", "Stopped", "Stepping" or "StepEnded".
	[[nodiscard]] std::string_view VisualScriptDebugSessionStateToString(VisualScriptDebugSessionState state);

	// The visual-script debugger of the editor: breakpoints (per user and project), targets and watches, the engine's debugger
	// attached while the editor plays (never for test runs or Simulate), stops and steps, the flow each window draws, and the
	// waits automation makes. Exists with and without the UI: automation debugs headless editors. Main thread only.
	//
	// It is play mode's observer: it attaches before the running scene starts and detaches before it stops, and after each
	// scene frame it reads what the frame traced and stopped, pausing the scene at the end of a frame that stopped an
	// execution (the held pause: the session paused it, so a resume unpauses it). Steps and continues are synchronous calls
	// between frames that run graph code and never a frame; frames are refused while an execution is stopped. Every mutator
	// that changes what the runtime is told pushes the configuration in its own call while attached, so no frame or resume
	// ever runs with a stale one. The editor calls BeginFrame before automation runs and EndFrame after play mode's update.
	class VisualScriptDebugSession final : public PlayModeObserver
	{
	public:
		// GetFrameStepRefusalForUI's text, which names the keys (the toolbar's and the Play menu's Step show it).
		static constexpr std::string_view RefusalForUI = "An execution is stopped at a breakpoint: continue it (F5) or step it (F10) first";
		// The reasons the stop-ended listener is told.
		static constexpr std::string_view StopEndedBySceneLoad =
			"A script loaded another scene, which ended the stopped execution; the game stays paused: Continue (F5) resumes it";
		static constexpr std::string_view StopEndedByDestroy =
			"The stopped execution's entity was destroyed; the game stays paused: Continue (F5) resumes it";
		static constexpr std::string_view StopEndedByEdit =
			"The stopped execution was cancelled by your edit; the game stays paused: Continue (F5) resumes it";
		// Every text the session gives the UI to show (the glyph tests read them).
		static constexpr std::array<std::string_view, 4> Texts = {RefusalForUI, StopEndedBySceneLoad, StopEndedByDestroy, StopEndedByEdit};

		VisualScriptDebugSession(EditorContext& context, PlayMode& playMode);
		~VisualScriptDebugSession() override;

		VisualScriptDebugSession(VisualScriptDebugSession const&) = delete;
		VisualScriptDebugSession& operator=(VisualScriptDebugSession const&) = delete;

		// The views settings that persist breakpoints and break on errors (they must outlive the session or be unset first);
		// none (headless) keeps them in memory only. The store is loaded again from them for the project followed.
		void SetViewSettings(VisualScriptViewSettings* settings);

		// --- Breakpoints, checked against the graph's stored document: refused for nodes it does not have. Every mutator here
		// and below that changes what the runtime is told pushes the configuration in the same call while attached. Before
		// any use the store follows the project the editor has open: another project's breakpoints are loaded (from the
		// views settings, or none) ---

		VisualScriptBreakpointStore const& GetBreakpoints() const;
		// Replaces or adds the node's breakpoint; its Type is the node's in the stored document, whatever breakpoint.Type
		// says. Refused for a graph that is not a loaded visual script, a node its document does not have, and what the
		// store refuses.
		[[nodiscard]] Result<void> SetBreakpoint(AssetHandle graph, VisualScriptBreakpoint breakpoint);
		void RemoveBreakpoints(AssetHandle graph, std::span<uint32_t const> nodes);
		// F9 on a selection: adds one to every node unless each has one, else removes them all (nodes no flow enters too).
		// Nodes the stored document does not have are left out; a toggle the store refuses (a full graph) changes nothing
		// and is logged.
		void ToggleBreakpoints(AssetHandle graph, std::span<uint32_t const> nodes);
		void SetBreakpointsEnabled(AssetHandle graph, std::span<uint32_t const> nodes, bool enabled);
		// The graph's breakpoints, or every graph's.
		void ClearBreakpoints(std::optional<AssetHandle> graph);
		bool IsBreakOnErrors() const { return m_BreakOnErrors; }
		void SetBreakOnErrors(bool breakOnErrors);

		// --- Targets: an entity whose executions stop at a script graph's breakpoints, or any instance (invalid) ---

		void SetTarget(AssetHandle graph, UUID entity);
		// Invalid: any instance.
		UUID GetTarget(AssetHandle graph) const;
		// The graphs with an entity as their target (the others target any instance), in the order they were set.
		std::vector<VisualScriptDebugTarget> const& GetTargets() const;
		// Told whenever a graph's target changes, so the graph's window shows that entity (an unchanged call tells nothing).
		void SetTargetListener(std::function<void(AssetHandle graph, UUID entity)> listener);
		// The sub-graphs the visible windows show (the window manager, every frame; applied when they change, which costs a
		// comparison otherwise).
		void SetWatches(std::vector<VisualScriptDebugWatch> watches);

		// --- Control, between frames; each answers an error when nothing plays, while simulating, and in a test run ---

		// Continues (Continue) or steps a stop (none: the current one), or with all continues every stop. Continue with no
		// resumable stop dismisses the read-only ones and, while the debugger holds its pause, resumes play: Resumed. In
		// StepEnded, Over and Into arm a Break for the ended execution's graph and resume play if the debugger paused it:
		// Armed. A continue or a step resumes play only if the debugger paused it, once no execution is stopped; a step that
		// ends its execution leaves the scene paused (StepEnded).
		[[nodiscard]] Result<VisualScriptResumeOutcome> Resume(std::optional<uint64_t> stop, VisualScriptDebugAction action,
		                                                       bool all = false);
		// graph: only that graph's stop points (a script graph's executions, or a library's nodes); none: any graph. cancel
		// cancels the armed Break and a waiting step.
		[[nodiscard]] Result<void> Break(bool cancel = false, std::optional<AssetHandle> graph = std::nullopt);
		// Continue with a one-shot breakpoint on the node. With nothing stopped it only arms (Armed), resuming play while the
		// debugger holds its pause.
		[[nodiscard]] Result<VisualScriptResumeOutcome> RunTo(AssetHandle document, uint32_t node);
		// The UI's resumes, queued and run at the next BeginFrame, never while a panel draws. thenResumePlay: the UI's unpause
		// while stopped, run as PlayMode::Resume (Continue every stop, then unpause unless one stopped again).
		void RequestResume(std::optional<uint64_t> stop, VisualScriptDebugAction action, bool all = false, bool thenResumePlay = false);
		void RequestRunTo(AssetHandle document, uint32_t node);
		// The UI's remedy for a breakpoint that stopped many instances: makes the stop's entity its graph's target at once,
		// and queues Continue of the graph's other stops.
		void RequestOnlyInstance(uint64_t stop);
		// The stop and frame the windows and the Debugger panel show.
		void SelectStop(uint64_t stop);
		void SelectFrame(uint32_t frame);
		// Why frames must not be stepped now (an execution is stopped); none when they may. This form names commands; the
		// UI's names keys.
		std::optional<std::string> GetFrameStepRefusal() const override;
		std::optional<std::string> GetFrameStepRefusalForUI() const;

		// --- What is shown ---

		VisualScriptDebugSessionState GetSessionState() const;
		bool IsAttached() const { return m_Attached; }
		// The runtime's, as last read.
		VisualScriptDebugState const& GetState() const { return m_State; }
		// A stop of the state as last read; null when it has none of the ID.
		VisualScriptStop const* FindStop(uint64_t stop) const override;
		// The stop selected while it exists, else the current one.
		std::optional<uint64_t> GetSelectedStop() const;
		uint32_t GetSelectedFrame() const { return m_SelectedFrame; }
		// The values of a stop's frame, read once per stop revision and frame; null when they cannot be read.
		VisualScriptStopValues const* GetStopValues(uint64_t stop, uint32_t frame);
		// The steps counted for a watched (document, function) since the last call, each with the flow time of the last frame
		// that ran it; the window drawing it drains them (one window per graph).
		VisualScriptFlowTake TakeFlow(AssetHandle document, uint32_t function);
		// The flow clock: the sum of the time steps of the scene frames run since play started. It holds while none runs.
		double GetFlowTime() const { return m_FlowTime; }
		// Changes whenever anything windows draw of debugging changed (state, breakpoints, targets, a new stop): views
		// rebuild then.
		uint64_t GetRevision() const;

		// --- PlayModeObserver ---

		// Attaches the runtime's debugger for Play, unless a test run (and pushes the configuration before the scene starts).
		void OnPlayStarting(EditorPlayState state, PlayStartOptions const& options) override;
		// Advances the flow clock, takes the frame's trace and reads the state when it changed; true on a new stop (or a step
		// that ended), which pauses the scene. Read-only stops of earlier frames are dismissed by the frame.
		bool OnPlayFrame(Timestep timestep) override;
		// Detaches: every stop ends, and waits answer PlayStopped.
		void OnPlayStopping() override;
		// Continues every stop; true when one stopped again, or when a stop ended without a resume (the scene stays paused).
		bool OnPlayResuming() override;
		PlayDebugSummary GetDebugSummary() const override;

		// --- The editor frame ---

		// Runs the UI's queued resumes.
		void BeginFrame();
		// Brings what the session shows up to date between editor frames, as EndFrame does without ending one: follows the
		// project and edits, and reads the runtime's state when its stop revision changed (an edit, an inspector change or a
		// resume while paused), pausing the scene at a stop that made. Automation and play mode call it before they read or act
		// on the state: commands run back to back with no editor frame between them.
		void Refresh() override;
		// Follows the project and edits (breakpoints whose node went), polls the stop revision when no frame ran, and
		// completes waits.
		void EndFrame();
		// graph.wait: completes with {stopped, stop, reason (Stopped, Timeout, NotPlaying, PlayStopped, Paused), hint} once a
		// stop exists, the timeout (0.1 to 60 real seconds) passed or play stopped, and at once when nothing plays or no frame
		// can run (the scene is paused and nothing is stopped).
		void WaitForStop(double timeoutSeconds, CommandCompletion completion);
		// The held pause: the debugger paused the scene and has not resumed it (so a resume unpauses it). It outlives a stop
		// that ends without a resume.
		bool HasPausedScene() const;
		// Told of a new current stop (the UI brings its window forward and takes the game's input back), after the scene was
		// paused for it and queued frame steps were cancelled.
		void SetStopListener(std::function<void(VisualScriptStop const& stop)> listener);
		// Told when the session unpauses the scene it paused.
		void SetResumeListener(std::function<void()> listener);
		// Told when the last resumable stop ended without a resume (an edit cancelled it, its entity was destroyed, a scene
		// load; also by what another stop's resume ran, and in the frame or resume that made it), with its graph and the reason
		// (one of the StopEndedBy texts), after the scene was kept paused. The graph is invalid when the stop ended before the
		// session read it: in the frame that made it.
		void SetStopEndedListener(std::function<void(AssetHandle graph, std::string const& reason)> listener);

	private:
		// Where a state read comes from: a frame's end, an editor frame's poll, a resume, or another control call.
		enum class StateOrigin : uint8_t
		{
			Frame = 0,
			Poll,
			Resume,
			Control
		};

		struct QueuedRequest
		{
			enum class Kind : uint8_t
			{
				Resume = 0,
				RunTo,
				OnlyInstance
			};

			Kind RequestKind = Kind::Resume;
			std::optional<uint64_t> Stop;
			VisualScriptDebugAction Action = VisualScriptDebugAction::Continue;
			bool All = false;
			bool ThenResumePlay = false;
			// RunTo: the node's document; OnlyInstance: the graph whose other stops continue.
			AssetHandle Document;
			uint32_t Node = 0;
		};

		// The steps of a watch accumulated since its last take.
		struct WatchFlow
		{
			VisualScriptDebugWatch Watch;
			std::vector<VisualScriptFlowStep> Steps;
			// (node, exit) to its index in Steps.
			std::map<std::pair<uint32_t, std::string>, size_t> Index;
			// The scene frame count at the last take.
			uint64_t TakenAtFrame = 0;
		};

		struct PendingWait
		{
			std::chrono::steady_clock::time_point Deadline;
			CommandCompletion Completion;
		};

		// What a resume continues, read before it runs.
		struct ResumeScope
		{
			// The stops it continues; every stop when All.
			std::vector<uint64_t> Resumed;
			bool All = false;
			// The graph of the one stop it continues (invalid for All).
			AssetHandle Graph;
			// The scene was paused before it: a scene the resume loads stays paused.
			bool WasPaused = false;
			uint64_t SceneVersion = 0;
		};

		// What a state read knows of how it came about, beyond its origin.
		struct StateChange
		{
			// A resume's scope and the stop it reached (stops it did not continue that are gone ended without a resume).
			ResumeScope const* Resume = nullptr;
			std::optional<uint64_t> Reached;
			// A frame that started with no stop and no step: the stop revision then. A change with no stop left means that a
			// stop was made and ended within the frame.
			std::optional<uint64_t> QuietRevision;
		};

		// What FinishResume did. Held: a stop ended without a resume, so the scene stays paused.
		struct FinishedResume
		{
			VisualScriptResumeOutcome Outcome;
			bool Held = false;
		};

		// Loads another project's breakpoints when the editor's project is not the one followed; true then. Queries follow
		// too, so the store and what it follows are mutable.
		bool FollowProject() const;
		// FollowProject, then the configuration push when something changed.
		void Follow();
		// Pushes the configuration while attached when it changed since the last push.
		void PushConfiguration();
		VisualScriptDebugConfiguration BuildConfiguration() const;
		// After a store change: the views settings get the graphs' breakpoints, the runtime the configuration.
		void OnBreakpointsChanged(std::span<AssetHandle const> graphs);
		// Removes the breakpoints of graphs that were deleted or that the views settings dropped, and those whose node an
		// edit removed or retyped.
		void FollowGraphs();
		void RememberDocument(AssetHandle graph, Ref<VisualScriptDocument const> const& document);

		// Why control calls are refused now; none while attached.
		std::optional<std::string> GetControlRefusal() const;
		bool HasResumableStop() const;
		// Takes the trace and reads the state when the stop revision changed since the last read (no frame ran to read it),
		// holding play at a new stop.
		void Poll();
		// The flags that hold only while the scene stays paused.
		bool IsPauseHeld() const;
		bool IsStepEnded() const;
		void SyncPause();
		// Unpauses the scene the debugger paused; tells the resume listener.
		void ResumePlay();
		// Pauses the scene at a new stop (remembering whether the debugger paused it) and cancels queued frame steps.
		void HoldPlay();

		// Reads the runtime's state and applies it: true when play must be held (a new stop or a step that ended, after a
		// frame or a poll; the last resumable stop ended without a resume, after anything); none when it cannot be read.
		std::optional<bool> RefreshState(StateOrigin origin);
		std::optional<bool> RefreshState(StateOrigin origin, StateChange const& change);
		bool ApplyState(VisualScriptDebugState state, StateOrigin origin, StateChange const& change);
		// Tells the listeners what the last state reads found, once play was held for it.
		void Notify();
		// Merges a trace into the watches' accumulations (frame: a scene frame ran).
		void MergeTrace(VisualScriptFrameTrace const& trace);
		// Takes the runtime's trace, merging it; nothing (having logged once per attachment) when it cannot be read.
		std::optional<uint64_t> TakeTrace();
		// What a resume of the stop (none: the current one), or of every stop, continues, as the state last read says.
		ResumeScope MakeResumeScope(std::optional<uint64_t> stop, bool all) const;
		// After a resume ran graph code: what the frame's end would have flushed and followed, then the state, then play
		// (managePlay: resumes it, or leaves it paused when the step ended its execution or a stop ended without a resume).
		FinishedResume FinishResume(VisualScriptResumeOutcome outcome, VisualScriptDebugAction action, ResumeScope const& scope,
		                            bool managePlay);
		void RunRequest(QueuedRequest const& request);
		void CompleteWaits(bool playStopped);
		Json MakeWaitResult(std::string_view reason) const;
		// Every per-attachment member back to its state before attaching.
		void ResetAttachment();

		EditorContext& m_Context;
		PlayMode& m_PlayMode;
		VisualScriptViewSettings* m_Views = nullptr;

		// The breakpoints of the project followed, and that project's file (empty: none).
		mutable VisualScriptBreakpointStore m_Breakpoints;
		mutable std::filesystem::path m_FollowedProject;
		// The project's file as the views settings key it.
		mutable std::filesystem::path m_FollowedViewsProject;
		mutable bool m_ProjectFollowed = false;
		// The document each graph with breakpoints had when its breakpoints last followed it.
		mutable std::unordered_map<AssetHandle, std::weak_ptr<VisualScriptDocument const>> m_FollowedDocuments;
		mutable std::vector<VisualScriptDebugTarget> m_Targets;
		mutable bool m_ConfigurationDirty = true;
		mutable uint64_t m_Revision = 1;
		bool m_BreakOnErrors = false;
		std::vector<VisualScriptDebugWatch> m_Watches;

		bool m_Attached = false;
		std::optional<VisualScriptDebugConfiguration> m_Pushed;
		uint32_t m_ConfigurationRevision = 0;
		VisualScriptDebugState m_State;
		// The largest stop ID seen this attachment: larger ones are new.
		uint64_t m_LastStopID = 0;
		// The stops made, and the frames after which play was held, this attachment (PlayDebugSummary).
		uint64_t m_StopsMade = 0;
		uint64_t m_Holds = 0;
		// The context's scene version after the last frame or resume (a scene load since then ends stops).
		uint64_t m_SceneVersionSeen = 0;
		std::optional<uint64_t> m_NotifiedCurrent;
		// A stop that ended without a resume, for the stop-ended listener: its graph and the reason.
		std::optional<std::pair<AssetHandle, std::string>> m_PendingEnded;
		bool m_PauseHeld = false;
		bool m_StepEnded = false;
		// The graph of the execution the last step was made in, which a Break after StepEnded is armed for.
		AssetHandle m_StepGraph;
		std::optional<uint64_t> m_SelectedStop;
		uint32_t m_SelectedFrame = 0;
		std::map<std::pair<uint64_t, uint32_t>, std::optional<VisualScriptStopValues>> m_StopValues;
		bool m_TraceFailureLogged = false;

		double m_FlowTime = 0.0;
		uint64_t m_FrameCount = 0;
		// A scene frame ran since the last EndFrame (or the start was reported).
		bool m_FrameRan = false;
		// Play mode reported the start (its first OnPlayFrame, which ran no scene frame).
		bool m_StartReported = false;
		std::vector<WatchFlow> m_Flows;

		std::vector<QueuedRequest> m_Requests;
		std::vector<PendingWait> m_Waits;

		std::function<void(AssetHandle, UUID)> m_TargetListener;
		std::function<void(VisualScriptStop const&)> m_StopListener;
		std::function<void()> m_ResumeListener;
		std::function<void(AssetHandle, std::string const&)> m_StopEndedListener;
	};
}
