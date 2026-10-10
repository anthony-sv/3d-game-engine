#include "stpch.h"
#include "Strada/Script/DotNetHost.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"

#include <coreclr_delegates.h>
#include <hostfxr.h>

#if defined(ST_PLATFORM_WINDOWS)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <charconv>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

namespace Strada::DotNet
{
	namespace
	{
#if defined(_M_ARM64) || defined(__aarch64__)
		constexpr char const* ArchitectureName = "arm64";
		constexpr char const* ArchitectureRootVariable = "DOTNET_ROOT_ARM64";
#else
		constexpr char const* ArchitectureName = "x64";
		constexpr char const* ArchitectureRootVariable = "DOTNET_ROOT_X64";
#endif

#if defined(ST_PLATFORM_WINDOWS)
		constexpr char const* HostFxrFileName = "hostfxr.dll";
#elif defined(ST_PLATFORM_MACOS)
		constexpr char const* HostFxrFileName = "libhostfxr.dylib";
#else
		constexpr char const* HostFxrFileName = "libhostfxr.so";
#endif

		using HostString = std::basic_string<char_t>;

		// hostfxr strings are UTF-16 on Windows and UTF-8 elsewhere, like native paths.
		HostString ToHostString(std::string_view utf8)
		{
			return FileSystem::PathFromUtf8(utf8).native();
		}

		std::string FromHostString(char_t const* text)
		{
			return FileSystem::PathToUtf8(std::filesystem::path(text));
		}

		// Process-wide, because CoreCLR can start once and never unloads: hostfxr stays loaded with it.
		struct HostState
		{
			void* Library = nullptr;
			std::filesystem::path Installation;
			load_assembly_fn LoadAssembly = nullptr;
			get_function_pointer_fn GetFunctionPointer = nullptr;
		};

		HostState s_Host;

		void* LoadLibraryFile(std::filesystem::path const& path)
		{
#if defined(ST_PLATFORM_WINDOWS)
			return reinterpret_cast<void*>(LoadLibraryW(path.c_str()));
#else
			return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
		}

		template<typename TFunction>
		TFunction GetExport(void* library, char const* name)
		{
#if defined(ST_PLATFORM_WINDOWS)
			return reinterpret_cast<TFunction>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
			return reinterpret_cast<TFunction>(dlsym(library, name));
#endif
		}

		void HOSTFXR_CALLTYPE WriteHostError(char_t const* message)
		{
			ST_CORE_ERROR(".NET host: {}", FromHostString(message));
		}

		std::optional<std::filesystem::path> ReadRegisteredInstallation()
		{
#if defined(ST_PLATFORM_WINDOWS)
			// The installers register the location in the 32-bit view of the registry.
			std::wstring const key = L"SOFTWARE\\dotnet\\Setup\\InstalledVersions\\" + ToHostString(ArchitectureName);
			wchar_t location[MAX_PATH] = {};
			DWORD size = sizeof(location);
			if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"InstallLocation", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, nullptr, location,
			                 &size) != ERROR_SUCCESS)
			{
				return std::nullopt;
			}
			return std::filesystem::path(location);
#else
			for (std::string const& file :
			     {std::string("/etc/dotnet/install_location_") + ArchitectureName, std::string("/etc/dotnet/install_location")})
			{
				std::ifstream stream(file);
				std::string location;
				if (stream && std::getline(stream, location))
				{
					while (!location.empty() && (location.back() == '\r' || location.back() == ' '))
					{
						location.pop_back();
					}
					if (!location.empty())
					{
						return std::filesystem::path(location);
					}
				}
			}
			return std::nullopt;
#endif
		}

		std::filesystem::path GetDefaultInstallation()
		{
#if defined(ST_PLATFORM_WINDOWS)
			std::optional<std::string> const programFiles = Platform::ReadEnvironmentVariable("ProgramFiles");
			return (programFiles ? FileSystem::PathFromUtf8(*programFiles) : std::filesystem::path(L"C:\\Program Files")) / "dotnet";
#elif defined(ST_PLATFORM_MACOS)
			return "/usr/local/share/dotnet";
#else
			return "/usr/share/dotnet";
#endif
		}

		// Version directories order by major.minor.patch, releases after their previews.
		struct HostVersion
		{
			uint32_t Major = 0;
			uint32_t Minor = 0;
			uint32_t Patch = 0;
			std::string Prerelease;

			bool operator<(HostVersion const& other) const
			{
				auto const key = [](HostVersion const& version)
				{
					return std::tuple(version.Major, version.Minor, version.Patch, version.Prerelease.empty());
				};
				if (key(*this) != key(other))
				{
					return key(*this) < key(other);
				}
				return Prerelease < other.Prerelease;
			}
		};

		std::optional<HostVersion> ParseVersion(std::string const& text)
		{
			HostVersion version;
			char const* const end = text.data() + text.size();
			char const* position = text.data();
			for (uint32_t* part : {&version.Major, &version.Minor, &version.Patch})
			{
				auto const [next, error] = std::from_chars(position, end, *part);
				if (error != std::errc())
				{
					return std::nullopt;
				}
				position = next;
				if (part != &version.Patch)
				{
					if (position == end || *position != '.')
					{
						return std::nullopt;
					}
					position++;
				}
			}
			if (position != end)
			{
				if (*position != '-')
				{
					return std::nullopt;
				}
				version.Prerelease.assign(position + 1, end);
			}
			return version;
		}

		Result<void> StartRuntime(std::filesystem::path const& runtimeConfig, std::filesystem::path const& applicationDirectory)
		{
			std::optional<std::filesystem::path> const installation = FindInstallation(applicationDirectory);
			std::optional<std::filesystem::path> const hostFxr = installation ? FindHostFxr(*installation) : std::nullopt;
			if (!hostFxr)
			{
				return Error{"no .NET installation was found; install the .NET 10 runtime (https://dot.net) or set DOTNET_ROOT"};
			}
			void* const library = LoadLibraryFile(*hostFxr);
			if (library == nullptr)
			{
				return MakeError("{} cannot be loaded", FileSystem::PathToUtf8(*hostFxr));
			}

			auto const setErrorWriter = GetExport<hostfxr_set_error_writer_fn>(library, "hostfxr_set_error_writer");
			auto const initialize = GetExport<hostfxr_initialize_for_runtime_config_fn>(library, "hostfxr_initialize_for_runtime_config");
			auto const getDelegate = GetExport<hostfxr_get_runtime_delegate_fn>(library, "hostfxr_get_runtime_delegate");
			auto const close = GetExport<hostfxr_close_fn>(library, "hostfxr_close");
			if (setErrorWriter == nullptr || initialize == nullptr || getDelegate == nullptr || close == nullptr)
			{
				return MakeError("{} is not a usable hostfxr", FileSystem::PathToUtf8(*hostFxr));
			}
			// Without a writer, hostfxr prints its errors to stderr only.
			setErrorWriter(WriteHostError);

			HostString const hostPath = FileSystem::GetExecutablePath().native();
			HostString const root = installation->native();
			hostfxr_initialize_parameters const parameters{sizeof(hostfxr_initialize_parameters), hostPath.c_str(), root.c_str()};
			hostfxr_handle context = nullptr;
			int32_t const status = initialize(runtimeConfig.c_str(), &parameters, &context);
			// Failures are negative HRESULT-style codes; 1 and 2 report an already running runtime.
			if (status < 0 || context == nullptr)
			{
				return MakeError("the .NET runtime cannot start for {} (hostfxr status 0x{:08X}; see the log)",
				                 FileSystem::PathToUtf8(runtimeConfig), static_cast<uint32_t>(status));
			}

			load_assembly_fn loadAssembly = nullptr;
			get_function_pointer_fn getFunctionPointer = nullptr;
			int32_t const loadStatus = getDelegate(context, hdt_load_assembly, reinterpret_cast<void**>(&loadAssembly));
			int32_t const functionStatus = getDelegate(context, hdt_get_function_pointer, reinterpret_cast<void**>(&getFunctionPointer));
			close(context);
			if (loadStatus < 0 || functionStatus < 0 || loadAssembly == nullptr || getFunctionPointer == nullptr)
			{
				return MakeError("the .NET runtime does not provide its hosting functions (hostfxr status 0x{:08X})",
				                 static_cast<uint32_t>(loadStatus < 0 ? loadStatus : functionStatus));
			}

			s_Host.Library = library;
			s_Host.Installation = *installation;
			s_Host.LoadAssembly = loadAssembly;
			s_Host.GetFunctionPointer = getFunctionPointer;
			ST_CORE_INFO(".NET runtime started from {}", FileSystem::PathToUtf8(*installation));
			return {};
		}
	}

	std::optional<std::filesystem::path> FindInstallation(std::filesystem::path const& applicationDirectory)
	{
		std::vector<std::filesystem::path> candidates;
		candidates.push_back(applicationDirectory / "dotnet");
		for (char const* variable : {ArchitectureRootVariable, "DOTNET_ROOT"})
		{
			if (std::optional<std::string> const root = Platform::ReadEnvironmentVariable(variable); root && !root->empty())
			{
				candidates.push_back(FileSystem::PathFromUtf8(*root));
			}
		}
		if (std::optional<std::filesystem::path> registered = ReadRegisteredInstallation())
		{
			candidates.push_back(std::move(*registered));
		}
		candidates.push_back(GetDefaultInstallation());

		for (std::filesystem::path const& candidate : candidates)
		{
			if (FindHostFxr(candidate))
			{
				return candidate;
			}
		}
		return std::nullopt;
	}

	std::optional<std::filesystem::path> FindHostFxr(std::filesystem::path const& installation)
	{
		std::error_code error;
		std::filesystem::directory_iterator versions(installation / "host" / "fxr", error);
		if (error)
		{
			return std::nullopt;
		}
		std::optional<HostVersion> newest;
		std::filesystem::path newestPath;
		for (std::filesystem::directory_entry const& entry : versions)
		{
			std::optional<HostVersion> const version = ParseVersion(FileSystem::PathToUtf8(entry.path().filename()));
			std::filesystem::path const library = entry.path() / HostFxrFileName;
			if (version && (!newest || *newest < *version) && std::filesystem::is_regular_file(library, error))
			{
				newest = version;
				newestPath = library;
			}
		}
		return newest ? std::optional(newestPath) : std::nullopt;
	}

	Result<void> LoadAssembly(std::filesystem::path const& assemblyPath, std::filesystem::path const& applicationDirectory)
	{
		if (s_Host.Library == nullptr)
		{
			std::filesystem::path runtimeConfig = assemblyPath;
			runtimeConfig.replace_extension(".runtimeconfig.json");
			if (Result<void> started = StartRuntime(runtimeConfig, applicationDirectory); !started)
			{
				return started;
			}
		}
		if (int32_t const status = s_Host.LoadAssembly(assemblyPath.c_str(), nullptr, nullptr); status < 0)
		{
			return MakeError("{} cannot be loaded into the .NET runtime (status 0x{:08X})", FileSystem::PathToUtf8(assemblyPath),
			                 static_cast<uint32_t>(status));
		}
		return {};
	}

	Result<void*> GetFunction(std::string_view typeName, std::string_view methodName)
	{
		ST_CORE_ASSERT(s_Host.Library != nullptr, "The .NET runtime is not started");
		void* function = nullptr;
		int32_t const status = s_Host.GetFunctionPointer(ToHostString(typeName).c_str(), ToHostString(methodName).c_str(),
		                                                 UNMANAGEDCALLERSONLY_METHOD, nullptr, nullptr, &function);
		if (status < 0 || function == nullptr)
		{
			return MakeError("{}.{} cannot be found in the scripting runtime (status 0x{:08X})", typeName, methodName,
			                 static_cast<uint32_t>(status));
		}
		return function;
	}
}
