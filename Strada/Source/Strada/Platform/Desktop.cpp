#include "stpch.h"
#include "Strada/Platform/Desktop.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Platform/Process.h"

#include <chrono>
#include <system_error>

#if defined(ST_PLATFORM_WINDOWS)
// shellapi.h defines macros such as FindExecutable: it stays in this file, apart from Process.
#include <Windows.h>
#include <shellapi.h>
#endif

namespace Strada
{
	Result<void> Desktop::Open(std::filesystem::path const& path)
	{
		std::error_code error;
		if (!std::filesystem::exists(path, error))
		{
			return MakeError("'{}' does not exist", FileSystem::PathToUtf8(path));
		}
#if defined(ST_PLATFORM_WINDOWS)
		// Values above 32 mean success.
		HINSTANCE const result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		if (reinterpret_cast<INT_PTR>(result) <= 32)
		{
			return MakeError("Windows cannot open '{}'", FileSystem::PathToUtf8(path));
		}
		return {};
#else
#if defined(ST_PLATFORM_MACOS)
		char const* const opener = "open";
#else
		char const* const opener = "xdg-open";
#endif
		ProcessSpecification specification;
		specification.Executable = opener;
		specification.Arguments = {FileSystem::PathToNativeUtf8(path)};
		specification.Timeout = std::chrono::seconds(10);
		Result<ProcessResult> const opened = Process::Run(specification);
		if (!opened)
		{
			return Error{opened.GetError()};
		}
		if (!opened.GetValue().Succeeded())
		{
			return MakeError("{} could not open '{}'", opener, FileSystem::PathToUtf8(path));
		}
		return {};
#endif
	}
}
