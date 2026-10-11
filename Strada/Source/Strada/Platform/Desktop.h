#pragma once

#include "Strada/Core/Result.h"

#include <filesystem>

namespace Strada
{
	// The desktop environment. Thread-safe.
	namespace Desktop
	{
		// Opens a file or folder the way the desktop does (a folder in the file manager); returns once it is handed over
		// (ShellExecute on Windows, open on macOS, xdg-open on Linux). Fails when it does not exist or nothing can open it.
		[[nodiscard]] Result<void> Open(std::filesystem::path const& path);
	}
}
