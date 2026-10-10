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

namespace Strada
{
	// The editor's play mode. It runs a copy of the edited scene (Play: scripts, physics and audio; Simulate: physics only)
	// through a SceneRunner and makes it the scene the panels and automation show and edit (EditorContext::BeginPlay).
	// Stopping discards the copy and every change made to it. Scenes the scripts load replace the copy, and
	// Application.Quit stops playing. Main thread only.
	class PlayMode
	{
	public:
		using StartCallback = std::function<void(Result<void> const& started)>;

		PlayMode(EditorContext& context, EditorScripts& scripts);
		// Stops playing.
		~PlayMode();

		PlayMode(PlayMode const&) = delete;
		PlayMode& operator=(PlayMode const&) = delete;

		// Starts Play or Simulate with a copy of the edited scene, or with the given scene (a test scene loaded from its file)
		// when one is passed, rendering to a view of the given size. Fails while playing.
		[[nodiscard]] Result<void> Start(EditorPlayState state, uint32_t viewportWidth, uint32_t viewportHeight,
		                                 Ref<Scene> scene = nullptr);
		// Stops the running scene (its scripts get OnDestroy) and returns to the edited scene. Game input is released.
		// Starts like Start once the scripts are built: Play builds changed scripts first and does not start when they fail
		// to build. The callback learns whether playing started (at once, or after the build); requests made while one waits
		// for its build fail.
		void RequestStart(EditorPlayState state, uint32_t viewportWidth, uint32_t viewportHeight, Ref<Scene> scene = nullptr,
		                  StartCallback callback = {});
		bool IsStartPending() const { return m_StartPending; }
		void Stop();
		bool IsPlaying() const { return m_Runner != nullptr; }

		// Pausing holds the running scene and its sounds; steps advance it a frame at a time while paused.
		void SetPaused(bool paused);
		bool IsPaused() const;
		void Step(uint32_t frames = 1);

		// Advances the running scene by the editor's frame time, then follows what its scripts asked for.
		void Update(Timestep timestep);
		// Runs frames at once with a fixed time step (paused scenes too), each ending like an editor frame ends (input
		// pressed in a frame is seen by that frame only): deterministic play for automation and tests. Stops early when the
		// scripts quit. Returns the number of frames run.
		uint32_t Advance(uint32_t frames, Timestep timestep);

		// The test results and script exceptions of the current run, or of the last one once it stopped (the scripts may end
		// it by quitting); null before the first run.
		TestRunReport const* GetTestReport() const;

	private:
		// Stops when the scripts quit; shows the scene the scripts loaded.
		void FollowRunner();

		EditorContext& m_Context;
		EditorScripts& m_Scripts;
		bool m_StartPending = false;
		Scope<SceneRunner> m_Runner;
		uint64_t m_RunnerSceneVersion = 0;
		std::optional<TestRunReport> m_LastTestReport;
	};
}
