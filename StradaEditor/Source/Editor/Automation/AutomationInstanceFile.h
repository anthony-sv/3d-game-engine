#pragma once

#include "Editor/Automation/AutomationInstance.h"

#include "Strada/Core/Result.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace Strada
{
	// What AutomationInstanceFile writes, removes and times with; tests replace them. One left empty is the default:
	// AutomationInstance::Write, AutomationInstance::Remove and the steady clock.
	struct AutomationInstanceFileServices
	{
		std::function<Result<std::filesystem::path>(AutomationInstanceInfo const&)> Write;
		std::function<void(uint32_t processID)> Remove;
		std::function<std::chrono::steady_clock::time_point()> Now;
	};

	// Keeps the editor's automation instance file stating the editor's current project, whatever other processes do with
	// the file. A write can fail: on Windows the atomic replace fails while another process has the file open (as the strada
	// tool's scans do). A failed write is retried on later updates, at most every RetryInterval, until one succeeds, and an
	// update always writes the newest project. What the file states (GetWritten) changes only when a write succeeds. A
	// streak of failures is logged once, as an error, and what ends it once, at info: the success, or a change back to the
	// project the file states. Main thread.
	class AutomationInstanceFile
	{
	public:
		static constexpr std::chrono::milliseconds RetryInterval{100};

		AutomationInstanceFile();
		explicit AutomationInstanceFile(AutomationInstanceFileServices services);

		// Writes the file for info at once (retried by Update when that fails).
		void Start(AutomationInstanceInfo info);
		// Asks for the file to name project, writing it at once when no failed write waits for its retry, and retries a
		// failed write once its deadline has passed. Does no file-system work while the file states what it should. Does
		// nothing before Start.
		void Update(std::string const& project);
		// Removes the file and drops a pending retry; Update does nothing until the next Start.
		void Stop();

		bool IsStarted() const { return m_IsStarted; }
		// Whether a write waits: the file does not state the info and project asked for.
		bool IsPending() const { return m_IsPending; }
		// What the file states: the info of the last write that succeeded since Start; empty before one has.
		std::optional<AutomationInstanceInfo> const& GetWritten() const { return m_Written; }
		// When the next retry may run; meaningful while a failed write waits (IsPending after a failure).
		std::chrono::steady_clock::time_point GetRetryTime() const { return m_RetryTime; }

	private:
		void Write();

		AutomationInstanceFileServices m_Services;
		bool m_IsStarted = false;
		bool m_IsPending = false;
		// What the file is to state.
		AutomationInstanceInfo m_Wanted;
		std::optional<AutomationInstanceInfo> m_Written;
		std::chrono::steady_clock::time_point m_RetryTime;
		// Writes that failed in the current streak, which a success or a change back ends; the first one is logged.
		uint32_t m_FailedWrites = 0;
	};
}
