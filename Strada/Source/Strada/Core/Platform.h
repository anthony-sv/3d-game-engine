#pragma once

#include "Strada/Core/Base.h"

#include <optional>
#include <string>

namespace Strada
{
	// Small OS queries that the core needs before any other subsystem exists. All functions are thread-safe.
	class Platform
	{
	public:
		static bool IsDebuggerAttached();
		static uint32_t GetProcessID();
		// Named to avoid the Win32 GetEnvironmentVariable macro.
		static std::optional<std::string> ReadEnvironmentVariable(std::string const& name);
		// Monotonic time in seconds since an unspecified, process-constant epoch.
		static double GetTime();
	};
}
