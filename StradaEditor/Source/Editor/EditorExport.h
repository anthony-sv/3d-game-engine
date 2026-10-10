#pragma once

#include "Editor/EditorContext.h"
#include "Editor/EditorScripts.h"

#include "Strada/Core/Result.h"
#include "Strada/Project/GameExporter.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace Strada
{
	// The outcome of exporting the game from the editor.
	struct GameExportReport
	{
		bool Succeeded = false;
		// Why the export failed (cancelled, a file could not be written, the scripts did not build); empty on success.
		std::string Error;
		std::filesystem::path Directory;
		// The game's executable (its app bundle on macOS); empty when the export failed.
		std::filesystem::path Executable;
		// The scripts were built in Release for the game, or failed to compile (nothing was exported then); the errors and
		// warnings of the build.
		bool ScriptsBuilt = false;
		bool ScriptBuildFailed = false;
		std::vector<ScriptDiagnostic> Diagnostics;
		// The export was cancelled.
		bool Cancelled = false;
		// The edited scene had unsaved changes, which the game does not have.
		bool UnsavedChanges = false;
		double Seconds = 0.0;
	};

	// Exports the open project as a game (GameExporter) on a worker thread. The editor's own script builds wait meanwhile,
	// as both build the same C# project. Main thread only, apart from the export thread it owns.
	class EditorExport
	{
	public:
		using CompletionCallback = std::function<void(GameExportReport const&)>;

		EditorExport(EditorContext& context, EditorScripts& scripts);
		// Cancels a running export and waits for it; its callback is dropped.
		~EditorExport();

		EditorExport(EditorExport const&) = delete;
		EditorExport& operator=(EditorExport const&) = delete;

		// Starts exporting to a directory: a new or empty one, or an earlier export, which is replaced. Fails at once (without
		// calling back) when no project is open, the editor is playing, scripts or a game are being built, or the project
		// cannot be exported. The callback runs on the main thread, from Update.
		[[nodiscard]] Result<void> Start(std::filesystem::path const& directory, CompletionCallback callback = {});
		bool IsRunning() const { return m_Export.valid(); }
		// The step the running export is at.
		std::string GetProgress() const;
		// Stops the running export; it completes as cancelled.
		void Cancel();

		// Every frame: completes a finished export.
		void Update();

	private:
		EditorContext& m_Context;
		EditorScripts& m_Scripts;
		std::future<Result<GameExportResult>> m_Export;
		std::atomic<bool> m_Cancel = false;
		mutable std::mutex m_ProgressMutex;
		std::string m_Progress;
		CompletionCallback m_Callback;
		GameExportReport m_Pending;
		// Of the exported project: diagnostics name its files relative to it.
		std::filesystem::path m_ProjectDirectory;
		std::chrono::steady_clock::time_point m_Start;
	};
}
