#include "Editor/ExampleCopyTask.h"

#include <chrono>
#include <utility>

namespace Strada
{
	ExampleCopyTask::~ExampleCopyTask()
	{
		if (m_Copy.valid())
		{
			m_Cancel = true;
			// A copy that finished before it saw the flag goes too; a failed one removed what it wrote itself.
			Result<ExampleCopyOutcome> const outcome = m_Copy.get();
			if (!outcome && m_Cleanup != nullptr)
			{
				m_Cleanup->Commit();
			}
			m_Cleanup.reset();
		}
	}

	Result<void> ExampleCopyTask::Start(ExampleCatalog catalog, ExampleEntry example, std::filesystem::path directory,
	                                    CompletionCallback callback)
	{
		if (m_Copy.valid())
		{
			return Error{"an example is being copied: wait until it is done"};
		}
		m_Cancel = false;
		m_Progress.Bytes = 0;
		m_Progress.Files = 0;
		m_Progress.TotalBytes = 0;
		m_Progress.TotalFiles = 0;
		m_Callback = std::move(callback);
		m_ExampleId = example.Id;
		// Before the worker writes anything: how the target was (new, or empty) is what a cancelled copy puts back.
		m_Cleanup = CreateScope<NewDirectoryCleanup>(directory);
		// The worker owns its inputs; it shares only the flag and the counters with this thread.
		m_Copy = std::async(std::launch::async,
		                    [this, source = std::move(catalog), entry = std::move(example), target = std::move(directory)]
		                    {
								ProjectCopyOptions options;
								options.Cancel = &m_Cancel;
								options.Progress = &m_Progress;
								return EditorOperations::CopyExample(source, entry, target, options);
							});
		return {};
	}

	void ExampleCopyTask::Cancel()
	{
		if (m_Copy.valid())
		{
			m_Cancel = true;
		}
	}

	void ExampleCopyTask::Update()
	{
		if (!m_Copy.valid() || m_Copy.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			return;
		}
		Result<ExampleCopyOutcome> outcome = m_Copy.get();
		bool const cancelled = m_Cancel.load();
		if (outcome && cancelled)
		{
			// Cancelled after its thread finished copying: the copy goes, as one cancelled while copying does.
			outcome = Result<ExampleCopyOutcome>(Error{"the copy was cancelled"});
		}
		else if (m_Cleanup != nullptr)
		{
			m_Cleanup->Commit();
		}
		m_Cleanup.reset();
		CompletionCallback callback = std::move(m_Callback);
		m_Callback = {};
		if (callback)
		{
			callback(std::move(outcome), cancelled);
		}
	}
}
