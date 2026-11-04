#include "Editor/Automation/AutomationInstanceFile.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"

#include <limits>
#include <utility>

namespace Strada
{
	namespace
	{
		std::string DescribeProject(std::string const& project)
		{
			return project.empty() ? std::string("no project") : fmt::format("the project '{}'", project);
		}
	}

	AutomationInstanceFile::AutomationInstanceFile()
		: AutomationInstanceFile(AutomationInstanceFileServices())
	{
	}

	AutomationInstanceFile::AutomationInstanceFile(AutomationInstanceFileServices services)
		: m_Services(std::move(services))
	{
		if (!m_Services.Write)
		{
			m_Services.Write = [](AutomationInstanceInfo const& info)
			{
				return AutomationInstance::Write(info);
			};
		}
		if (!m_Services.Remove)
		{
			m_Services.Remove = [](uint32_t processID)
			{
				AutomationInstance::Remove(processID);
			};
		}
		if (!m_Services.Now)
		{
			m_Services.Now = []
			{
				return std::chrono::steady_clock::now();
			};
		}
	}

	void AutomationInstanceFile::Start(AutomationInstanceInfo info)
	{
		m_IsStarted = true;
		m_IsPending = true;
		m_Wanted = std::move(info);
		m_Written.reset();
		m_FailedWrites = 0;
		Write();
	}

	void AutomationInstanceFile::Update(std::string const& project)
	{
		if (!m_IsStarted)
		{
			return;
		}
		if (project != m_Wanted.Project)
		{
			m_Wanted.Project = project;
			// A change back to what the file states needs no write; otherwise a retry that waits keeps its deadline and
			// writes the newest project.
			m_IsPending = !m_Written || m_Written->Project != project;
		}
		if (!m_IsPending)
		{
			return;
		}
		if (m_FailedWrites > 0 && m_Services.Now() < m_RetryTime)
		{
			return;
		}
		Write();
	}

	void AutomationInstanceFile::Stop()
	{
		if (!m_IsStarted)
		{
			return;
		}
		// Also when no write succeeded: a file with this process ID can only be a stale one of an earlier process.
		m_Services.Remove(m_Wanted.ProcessID);
		m_IsStarted = false;
		m_IsPending = false;
		m_Written.reset();
		m_FailedWrites = 0;
	}

	void AutomationInstanceFile::Write()
	{
		Result<std::filesystem::path> written = m_Services.Write(m_Wanted);
		if (!written)
		{
			m_RetryTime = m_Services.Now() + RetryInterval;
			if (m_FailedWrites == 0)
			{
				ST_ERROR("Could not write the automation instance file (retried every {} ms until it succeeds): {}", RetryInterval.count(),
				         written.GetError());
			}
			if (m_FailedWrites < std::numeric_limits<uint32_t>::max())
			{
				++m_FailedWrites;
			}
			return;
		}
		if (m_FailedWrites > 0)
		{
			ST_INFO("Wrote the automation instance file '{}' after {} failed {}: it names {}", FileSystem::PathToUtf8(written.GetValue()),
			        m_FailedWrites, m_FailedWrites == 1 ? "attempt" : "attempts", DescribeProject(m_Wanted.Project));
		}
		else if (!m_Written)
		{
			ST_INFO("Automation instance file: {}", FileSystem::PathToUtf8(written.GetValue()));
		}
		m_Written = m_Wanted;
		m_IsPending = false;
		m_FailedWrites = 0;
	}
}
