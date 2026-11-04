#pragma once

#include "Editor/EditorContext.h"
#include "Editor/EditorScripts.h"

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Project/SceneRunner.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace Strada
{
	struct VisualScriptStop;

	// What a play run starts with.
	struct PlayStartOptions
	{
		// test.run's runs: no debugger is attached, so breakpoints are ignored.
		bool TestRun = false;
		// The running scene is paused before its first update, in the start's own frame (play.start's paused): its scripts'
		// OnCreate have run, nothing else has.
		bool Paused = false;
	};

	// What play.state reports of the visual-script debugger.
	struct PlayDebugSummary
	{
		// "Detached", "Running", "Stopped", "Stepping" or "StepEnded".
		std::string State = "Detached";
		uint32_t Stops = 0;
		std::optional<uint64_t> Current;
		// The debugger paused the scene for a stop and has not resumed it (it outlives a stop that ends without a resume).
		bool PauseHeld = false;
		// The stops made since play started, also those that ended in the frame or resume that made them before the debugger
		// read them (a scene load, the entity destroyed): a larger count after frames ran means one stopped (play.advance's
		// and graph.trace's stopped).
		uint64_t StopsMade = 0;
		// The frames after which the debugger held play since play started (a new stop, a step that ended its execution, a
		// stop that ended without a resume): a larger count after a frame means play mode paused there (graph.trace ends).
		uint64_t Holds = 0;
	};

	// Is told what play mode does, and may hold it back: the editor's visual-script debug session. Main thread.
	class PlayModeObserver
	{
	public:
		virtual ~PlayModeObserver() = default;

		// Before the running scene starts (its scripts' OnCreate run inside the start).
		virtual void OnPlayStarting(EditorPlayState state, PlayStartOptions const& options) = 0;
		// After the scene started (with a zero time step), and after each frame Update and Advance ran, with that frame's
		// time step before the time scale: true pauses the scene, and Advance stops early.
		virtual bool OnPlayFrame(Timestep timestep) = 0;
		// Before the running scene stops (its scripts' OnDestroy run inside the stop).
		virtual void OnPlayStopping() = 0;
		// Before a paused scene resumes (PlayMode::Resume): the stopped executions continue first; true when one stopped
		// again, and the scene then stays paused.
		virtual bool OnPlayResuming() = 0;
		// Brings what the observer reports up to date: commands that change what is stopped (an entity deleted, an edit)
		// run back to back with no editor frame between them. Runs no graph code and no frame.
		virtual void Refresh() = 0;
		// Why scene frames must not run or be stepped now (an execution is stopped at a breakpoint); none when they may.
		virtual std::optional<std::string> GetFrameStepRefusal() const = 0;
		virtual PlayDebugSummary GetDebugSummary() const = 0;
		// A stop the summary counts, by its ID; null when there is none of the ID.
		virtual VisualScriptStop const* FindStop(uint64_t stop) const = 0;
	};

	// The editor's play mode. It runs a copy of the edited scene (Play: scripts, physics and audio; Simulate: physics only)
	// through a SceneRunner and makes it the scene the panels and automation show and edit (EditorContext::BeginPlay).
	// Stopping discards the copy and every change made to it. Scenes the scripts load replace the copy, and
	// Application.Quit stops playing.
	//
	// While it plays, play mode owns the game's input frame (Input::SetFrameOwned): the frame ends after each game frame that
	// Update or Advance ran, and only then, so the input given while no game frame runs (a paused scene, frames refused at a
	// stop) reaches the next game frame once, whatever editor frames come between. Start and Stop each begin a clean input
	// frame; keys and buttons held stay down. Main thread only.
	class PlayMode
	{
	public:
		using StartCallback = std::function<void(Result<void> const& started)>;

		PlayMode(EditorContext& context, EditorScripts& scripts);
		// Stops playing.
		~PlayMode();

		PlayMode(PlayMode const&) = delete;
		PlayMode& operator=(PlayMode const&) = delete;

		// The observer is told of every start, frame, resume and stop, and may hold frames back; null for none. It must stay
		// alive while set: clear it before destroying it.
		void SetObserver(PlayModeObserver* observer) { m_Observer = observer; }

		// Starts Play or Simulate with a copy of the edited scene, or with the given scene (a test scene loaded from its file)
		// when one is passed, rendering to a view of the given size. Fails while playing. The observer is told before the
		// scene starts, and after it started (OnPlayFrame with a zero time step, which may pause it).
		[[nodiscard]] Result<void> Start(EditorPlayState state, uint32_t viewportWidth, uint32_t viewportHeight, Ref<Scene> scene = nullptr,
		                                 PlayStartOptions const& options = {});
		// Starts like Start once the scripts are built: Play builds changed scripts first and does not start when they fail
		// to build. The callback learns whether playing started (at once, or after the build); requests made while one waits
		// for its build fail.
		void RequestStart(EditorPlayState state, uint32_t viewportWidth, uint32_t viewportHeight, Ref<Scene> scene = nullptr,
		                  StartCallback callback = {}, PlayStartOptions const& options = {});
		bool IsStartPending() const { return m_StartPending; }
		// Stops the running scene (its scripts get OnDestroy) and returns to the edited scene. Game input is released, and the
		// input frame is the application's again. The observer is told before the scene stops.
		void Stop();
		bool IsPlaying() const { return m_Runner != nullptr; }

		// Pausing holds the running scene and its sounds; steps advance it a frame at a time while paused. Unpausing is
		// refused (false) while the observer refuses frames: a scene never resumes past a stopped execution (Resume continues
		// it first). False when not playing; true otherwise.
		bool SetPaused(bool paused);
		bool IsPaused() const;
		// Resumes a paused scene as the play controls do: the observer continues the stopped executions first
		// (OnPlayResuming), and the scene stays paused when one stopped again. True when the scene runs afterwards.
		bool Resume();
		// Queues frames for a paused scene; nothing while the observer refuses frames (brought up to date first).
		void Step(uint32_t frames = 1);
		// Drops the frames Step queued that have not run yet.
		void CancelSteps();
		// Brings what the observer reports up to date (PlayModeObserver::Refresh): automation calls it before it reads the
		// refusal or the summary, and Step and Advance call it first.
		void RefreshObserver();
		// Why scene frames must not run or be stepped now (the observer's reason); none when they may, and without an
		// observer.
		std::optional<std::string> GetFrameStepRefusal() const;
		// The observer's summary; Detached without one.
		PlayDebugSummary GetDebugSummary() const;
		// The observer's stop of the ID; null without an observer, or when it has none of the ID.
		VisualScriptStop const* FindDebugStop(uint64_t stop) const;
		// Acts on what script code that ran between frames asked for, as the end of a frame does: loads the scene it asked
		// for (SceneRunner::ApplyRequests) and shows it, and stops playing when it quit.
		void FollowScripts();

		// Whether the game's UI follows the mouse (Scene::SetUIPointerEnabled), applied to the running scene before each of its
		// updates, scenes the scripts load included. On by default; the editor turns it off while its game view does not have
		// the window's input, whose cursor is then over the editor.
		void SetUIPointerEnabled(bool enabled) { m_UIPointerEnabled = enabled; }
		bool IsUIPointerEnabled() const { return m_UIPointerEnabled; }

		// Advances the running scene by the editor's frame time, then follows what its scripts asked for and, when a frame ran,
		// tells the observer and ends the input frame. Runs no frame while the observer refuses frames, whatever frames are
		// queued, and the input then waits for the next frame that runs. A scene that a frame of a paused scene loads is paused
		// too, with the steps still queued (Step's frames all run, then play stays paused); one that a running scene loads runs.
		void Update(Timestep timestep);
		// Runs frames at once with a fixed time step (paused scenes too), each ending the input frame (the input given before
		// the call, also while paused, is seen by the first frame only): deterministic play for automation and tests. Stops
		// early when the scripts quit and when the observer pauses the scene after a frame; runs none while it refuses frames
		// (brought up to date first). A paused scene stays paused, a scene its frames load included. Returns the number of
		// frames run.
		uint32_t Advance(uint32_t frames, Timestep timestep);

		// The test results and script exceptions of the current run, or of the last one once it stopped (the scripts may end
		// it by quitting); null before the first run.
		TestRunReport const* GetTestReport() const;

	private:
		// Stops when the scripts quit; shows the scene the scripts loaded.
		void FollowRunner();
		// After a frame that ran scene `ran`, paused when it began (by the agent, the debugger's held pause or a step): a scene
		// that frame loaded is paused too, with the frame steps ran still had queued.
		void KeepPaused(Scene const& ran, bool paused);
		// Tells the observer that a frame ran (or the scene started) and pauses the running scene when it asks to; true then.
		bool NotifyFrame(Timestep timestep);

		EditorContext& m_Context;
		EditorScripts& m_Scripts;
		PlayModeObserver* m_Observer = nullptr;
		bool m_StartPending = false;
		Scope<SceneRunner> m_Runner;
		uint64_t m_RunnerSceneVersion = 0;
		std::optional<TestRunReport> m_LastTestReport;
		bool m_UIPointerEnabled = true;
	};
}
