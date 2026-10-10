#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"

#include <cstdint>
#include <vector>

namespace Strada
{
	// What the scripts of a run reported through Strada.Testing, and the exceptions they did not catch.
	struct TestRunReport
	{
		std::vector<ScriptTestResult> Results;
		// TestReporter.Finish was called.
		bool Finished = false;
		uint32_t ScriptExceptions = 0;

		uint32_t GetFailedCount() const;
		// Failed checks plus script exceptions: what fails a test run.
		uint32_t GetFailureCount() const { return GetFailedCount() + ScriptExceptions; }
	};

	// Reads a scene file of the asset directory (settings from newer versions are skipped with a warning).
	[[nodiscard]] Result<Ref<Scene>> LoadSceneAsset(AssetHandle scene);

	struct SceneRunnerSettings
	{
		// From the project (MakeSceneRuntimeSettings); RunScripts and PlayAudio false simulate physics only.
		SceneRuntimeSettings Runtime;
		// Application.IsEditor for the scripts.
		bool IsEditor = false;
	};

	// Runs scenes for the editor's play mode and the game player. It starts a scene's runtime and acts on what its scripts
	// ask of the application (the ScriptHost while running): SceneManager.LoadScene replaces the scene after the frame,
	// Application.Quit is recorded for the owner to act on, and test results and unhandled script exceptions are
	// collected across scene loads. Main thread only; one runner runs at a time.
	class SceneRunner final : public ScriptHost
	{
	public:
		explicit SceneRunner(SceneRunnerSettings settings);
		// Stops the running scene.
		~SceneRunner() override;

		SceneRunner(SceneRunner const&) = delete;
		SceneRunner& operator=(SceneRunner const&) = delete;

		// Starts running the scene (which must not be running), replacing a running one.
		void Start(Ref<Scene> scene);
		// Stops the running scene (its scripts get OnDestroy) and forgets it. Requests not acted on yet are dropped.
		void Stop();
		bool IsRunning() const { return m_Scene != nullptr; }
		// The running scene; null when stopped.
		Ref<Scene> const& GetScene() const { return m_Scene; }
		// Changes whenever the running scene is replaced (a scene load), so owners can drop state tied to it.
		uint64_t GetSceneVersion() const { return m_SceneVersion; }

		// Advances the running scene by the time step, then loads the scene the scripts asked for, if any. A scene that
		// cannot be loaded is logged and the current one keeps running.
		void Update(Timestep timestep);

		// Application.Quit was called; the owner decides what quitting means (the editor stops playing).
		bool IsQuitRequested() const { return m_QuitRequested; }
		TestRunReport const& GetTestReport() const { return m_TestReport; }

		// --- ScriptHost ---

		bool IsEditor() const override { return m_Settings.IsEditor; }
		void RequestSceneLoad(AssetHandle scene) override;
		void RequestQuit() override;
		void ReportTestResult(ScriptTestResult const& result) override;
		void FinishTests() override;
		void OnScriptException() override;

	private:
		void LoadRequestedScene();

		SceneRunnerSettings m_Settings;
		Ref<Scene> m_Scene;
		uint64_t m_SceneVersion = 0;
		AssetHandle m_RequestedScene;
		bool m_QuitRequested = false;
		TestRunReport m_TestReport;
	};
}
