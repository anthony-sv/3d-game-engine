#pragma once

#include "Strada/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Strada
{
	// How clients (the strada CLI and MCP bridge) find and authenticate with a running editor.
	struct AutomationInstanceInfo
	{
		uint32_t ProcessID = 0;
		uint16_t Port = 0;
		std::string Token;
		// Project file of the editor, empty when none is open.
		std::string Project;
		std::string Version;
	};

	// Instance files: <user data>/Editor/Instances/<pid>.json. They contain the access token, so the directory is
	// user-only: POSIX permissions 0700 (directory) and 0600 (files); on Windows the per-user application data folder is
	// already restricted to the user (and administrators) by its default ACL.
	class AutomationInstance
	{
	public:
		static std::filesystem::path GetDirectory();
		static std::filesystem::path GetPath(uint32_t processID);

		// Writes the file for info.ProcessID and returns its path.
		[[nodiscard]] static Result<std::filesystem::path> Write(AutomationInstanceInfo const& info);
		[[nodiscard]] static Result<AutomationInstanceInfo> Read(std::filesystem::path const& path);
		static void Remove(uint32_t processID);
		// Instances whose process is still running, newest process ID last; files of exited processes are deleted.
		static std::vector<AutomationInstanceInfo> FindRunningInstances();
	};
}
