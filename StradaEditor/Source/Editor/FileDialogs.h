#pragma once

#include "Strada/Core/Result.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace Strada
{
	// A file type choice of a native file dialog.
	struct FileDialogFilter
	{
		// Shown to the user, e.g. "Strada Scene".
		std::string Name;
		// Comma-separated extensions without dots, e.g. "sscene" or "png,jpg".
		std::string Extensions;
	};

	// Native open, save and folder dialogs (Win32, Cocoa, GTK 3), parented to the main window. They are modal and block
	// until closed. An empty optional means the user cancelled. Editor-only: exported games do not depend on GTK. Main
	// thread only.
	namespace FileDialogs
	{
		[[nodiscard]] Result<std::optional<std::filesystem::path>> OpenFile(std::span<FileDialogFilter const> filters,
		                                                                    std::filesystem::path const& defaultDirectory = {});
		// Platforms differ in whether they append the filter's extension; callers add it when it is missing.
		[[nodiscard]] Result<std::optional<std::filesystem::path>> SaveFile(std::span<FileDialogFilter const> filters,
		                                                                    std::filesystem::path const& defaultDirectory = {},
		                                                                    std::string const& defaultName = {});
		[[nodiscard]] Result<std::optional<std::filesystem::path>> PickFolder(std::filesystem::path const& defaultDirectory = {});
	}
}
