#pragma once

#include "Editor/EditorOperations.h"

#include "Strada/Core/Result.h"
#include "Strada/Project/ExampleCatalog.h"
#include "Strada/Project/ProjectCopy.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <future>

namespace Strada
{
	// Copies an example (EditorOperations::CopyExample) on a worker thread, one at a time, for project.copy-example and the
	// start screen; opening the copy is the caller's, on the main thread. Main thread only, apart from the copy thread it owns.
	class ExampleCopyTask
	{
	public:
		// The copy's outcome on the main thread, from Update; cancelled tells a cancelled copy (whose outcome is an error) from
		// a failed one.
		using CompletionCallback = std::function<void(Result<ExampleCopyOutcome> outcome, bool cancelled)>;

		ExampleCopyTask() = default;
		// Cancels a running copy and waits for it (it removes what it wrote); its callback is dropped.
		~ExampleCopyTask();

		ExampleCopyTask(ExampleCopyTask const&) = delete;
		ExampleCopyTask& operator=(ExampleCopyTask const&) = delete;

		// Starts copying the example into a new or empty directory. Fails at once (without calling back) while a copy runs.
		[[nodiscard]] Result<void> Start(ExampleCatalog catalog, ExampleEntry example, std::filesystem::path directory,
		                                 CompletionCallback callback);
		bool IsRunning() const { return m_Copy.valid(); }
		// How far the running (or last) copy got; any thread may read it.
		ProjectCopyProgress const& GetProgress() const { return m_Progress; }
		// Stops the running copy: it completes as cancelled, leaving nothing behind.
		void Cancel();

		// Every frame: completes a finished copy.
		void Update();

	private:
		std::future<Result<ExampleCopyOutcome>> m_Copy;
		std::atomic<bool> m_Cancel = false;
		ProjectCopyProgress m_Progress;
		CompletionCallback m_Callback;
	};
}
