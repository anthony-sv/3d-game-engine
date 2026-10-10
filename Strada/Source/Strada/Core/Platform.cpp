#include "stpch.h"
#include "Strada/Core/Platform.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>

#if defined(ST_PLATFORM_WINDOWS)
#include <Windows.h>
#elif defined(ST_PLATFORM_LINUX)
#include <signal.h>
#include <unistd.h>
#include <fstream>
#elif defined(ST_PLATFORM_MACOS)
#include <signal.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Strada
{
	bool Platform::IsDebuggerAttached()
	{
#if defined(ST_PLATFORM_WINDOWS)
		return ::IsDebuggerPresent() != FALSE;
#elif defined(ST_PLATFORM_LINUX)
		// A non-zero TracerPid means a debugger (or another tracer) is attached.
		std::ifstream status("/proc/self/status");
		std::string line;
		while (std::getline(status, line))
		{
			constexpr std::string_view Prefix = "TracerPid:";
			if (line.compare(0, Prefix.size(), Prefix) == 0)
			{
				return std::strtol(line.c_str() + Prefix.size(), nullptr, 10) != 0;
			}
		}
		return false;
#elif defined(ST_PLATFORM_MACOS)
		int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(::getpid())};
		struct kinfo_proc info = {};
		size_t size = sizeof(info);
		if (::sysctl(mib, 4, &info, &size, nullptr, 0) != 0)
		{
			return false;
		}
		return (info.kp_proc.p_flag & P_TRACED) != 0;
#endif
	}

	uint32_t Platform::GetProcessID()
	{
#if defined(ST_PLATFORM_WINDOWS)
		return static_cast<uint32_t>(::GetCurrentProcessId());
#else
		return static_cast<uint32_t>(::getpid());
#endif
	}

	bool Platform::IsProcessRunning(uint32_t processID)
	{
		if (processID == 0)
		{
			return false;
		}
#if defined(ST_PLATFORM_WINDOWS)
		HANDLE const process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processID);
		if (process == nullptr)
		{
			// Access denied means the process exists but belongs to another user.
			return ::GetLastError() == ERROR_ACCESS_DENIED;
		}
		DWORD exitCode = 0;
		bool const running = ::GetExitCodeProcess(process, &exitCode) != 0 && exitCode == STILL_ACTIVE;
		::CloseHandle(process);
		return running;
#else
		// Signal 0 only checks existence; EPERM means it exists but belongs to another user.
		return ::kill(static_cast<pid_t>(processID), 0) == 0 || errno == EPERM;
#endif
	}

	std::optional<std::string> Platform::ReadEnvironmentVariable(std::string const& name)
	{
#if defined(ST_PLATFORM_WINDOWS)
		// The wide API is required to read non-ASCII values correctly; convert to UTF-8.
		int const nameLength = ::MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
		if (nameLength <= 0)
		{
			return std::nullopt;
		}
		std::wstring wideName(static_cast<size_t>(nameLength), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wideName.data(), nameLength);

		DWORD const valueLength = ::GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
		if (valueLength == 0)
		{
			return std::nullopt;
		}
		std::wstring wideValue(valueLength, L'\0');
		DWORD const written = ::GetEnvironmentVariableW(wideName.c_str(), wideValue.data(), valueLength);
		wideValue.resize(written);

		int const utf8Length =
			::WideCharToMultiByte(CP_UTF8, 0, wideValue.c_str(), static_cast<int>(wideValue.size()), nullptr, 0, nullptr, nullptr);
		std::string value(static_cast<size_t>(utf8Length), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, wideValue.c_str(), static_cast<int>(wideValue.size()), value.data(), utf8Length, nullptr,
		                      nullptr);
		return value;
#else
		char const* value = std::getenv(name.c_str());
		if (value == nullptr)
		{
			return std::nullopt;
		}
		return std::string(value);
#endif
	}

	double Platform::GetTime()
	{
		static auto const s_Epoch = std::chrono::steady_clock::now();
		std::chrono::duration<double> const elapsed = std::chrono::steady_clock::now() - s_Epoch;
		return elapsed.count();
	}

	void Platform::IgnoreBrokenPipeSignal()
	{
#if !defined(ST_PLATFORM_WINDOWS)
		::signal(SIGPIPE, SIG_IGN);
#endif
	}
}
