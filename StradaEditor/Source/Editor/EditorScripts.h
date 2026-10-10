#pragma once

#include "Editor/EditorContext.h"

#include "Strada/Core/Result.h"
#include "Strada/Project/ScriptProject.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// The outcome of a script build in the editor.
	struct ScriptBuildReport
	{
		bool Succeeded = false;
		// Why the build could not run (no project, no scripts, no .NET SDK, cancelled); empty when it ran.
		std::string Error;
		std::vector<ScriptDiagnostic> Diagnostics;
		// The built assembly is loaded into the script engine. One built while scripts run (play mode) loads when they
		// stop.
		bool Loaded = false;
		double Seconds = 0.0;

		uint32_t CountDiagnostics(ScriptDiagnosticSeverity severity) const;
	};

	// The open project's C# scripts in the editor. It keeps the C# project pointing at this engine, builds it with the .NET
	// SDK on a worker thread (when asked, and by itself when the sources changed) and loads the built game assembly into
	// the script engine (again after every build: hot reload). It follows the project the context opens or closes. Main
	// thread only, apart from the build thread it owns.
	class EditorScripts
	{
	public:
		using BuildCallback = std::function<void(ScriptBuildReport const&)>;

		// How often Update looks for changed sources.
		static constexpr std::chrono::milliseconds SourceCheckInterval{1000};

		// scriptCoreAssembly: the engine's Strada.ScriptCore.dll, which script projects reference.
		EditorScripts(EditorContext& context, std::filesystem::path scriptCoreAssembly);
		// Stops a running build; its callbacks are dropped.
		~EditorScripts();

		EditorScripts(EditorScripts const&) = delete;
		EditorScripts& operator=(EditorScripts const&) = delete;

		// Every frame: follows project changes, finishes builds, loads assemblies that waited for scripts to stop and
		// builds when sources changed.
		void Update();

		// Starts building the open project's scripts, or joins the build that is running. The callback runs on the main
		// thread (from Update, or at once when the build cannot start).
		void Build(BuildCallback callback = {});
		bool IsBuilding() const { return m_Build.valid(); }
		// The last finished build; null before the first one of the open project.
		ScriptBuildReport const* GetLastBuild() const { return m_LastBuild ? &*m_LastBuild : nullptr; }

		// A script class from the template, creating the C# project first when the project has none. The next Update builds it.
		[[nodiscard]] Result<std::filesystem::path> CreateScript(std::string_view className);

		bool HasScriptProject() const;
		// A C# source or project file changed after the game assembly was built.
		bool IsOutOfDate() const;

	private:
		void FollowProject();
		void FinishBuild();
		void BuildChangedSources();
		void Complete(ScriptBuildReport report);
		// Loads the project's built assembly unless scripts are running (then it waits); whether it was loaded.
		bool LoadAssembly();
		// Waits for a running build to stop; true when one was running.
		bool StopBuild();

		EditorContext& m_Context;
		std::filesystem::path m_ScriptCoreAssembly;
		// The project file of the project the scripts belong to; empty without one.
		std::filesystem::path m_ProjectFile;

		std::future<Result<ScriptBuildResult>> m_Build;
		std::atomic<bool> m_CancelBuild = false;
		std::chrono::steady_clock::time_point m_BuildStart;
		std::vector<BuildCallback> m_Callbacks;
		std::optional<ScriptBuildReport> m_LastBuild;
		// The sources the last build started with, so changed sources are built once.
		std::optional<std::filesystem::file_time_type> m_BuiltSources;
		std::chrono::steady_clock::time_point m_LastSourceCheck;
		bool m_LoadPending = false;
	};
}
