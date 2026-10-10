#pragma once

// Internal to the Script module: hosts the .NET runtime through hostfxr.

#include "Strada/Core/Result.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace Strada::DotNet
{
	// The .NET installation scripts run on, searched in the .NET host's order: a runtime shipped with the application
	// (<applicationDirectory>/dotnet, as exported games may do), the DOTNET_ROOT_<ARCH> and DOTNET_ROOT variables, the
	// registered install location, then the default one. Only installations that contain hostfxr count.
	std::optional<std::filesystem::path> FindInstallation(std::filesystem::path const& applicationDirectory);

	// The newest hostfxr of an installation (host/fxr/<version>).
	std::optional<std::filesystem::path> FindHostFxr(std::filesystem::path const& installation);

	// Starts the .NET runtime described by the assembly's runtimeconfig.json and loads the assembly into the default load
	// context. The runtime cannot be unloaded: it stays for the rest of the process and later calls only load their
	// assembly. Main thread only.
	[[nodiscard]] Result<void> LoadAssembly(std::filesystem::path const& assemblyPath, std::filesystem::path const& applicationDirectory);

	// A static [UnmanagedCallersOnly] method of a type ("Namespace.Type, Assembly") in an assembly LoadAssembly loaded.
	[[nodiscard]] Result<void*> GetFunction(std::string_view typeName, std::string_view methodName);
}
