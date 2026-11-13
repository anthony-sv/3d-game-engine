#include "stpch.h"
#include "Strada/Core/Platform.h"

#include "Strada/Core/FileSystem.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <utility>

#if defined(ST_PLATFORM_WINDOWS)
#include <Windows.h>
#include <objbase.h>
#include <shellapi.h>

// Needs Windows.h first.
#include <ShlObj.h>
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
	namespace
	{
		// "/home/ana/Documents/" and "/home/ana/Documents" name one folder.
		std::filesystem::path WithoutTrailingSeparator(std::filesystem::path const& path)
		{
			std::filesystem::path normal = path.lexically_normal();
			if (!normal.has_filename() && normal.has_relative_path())
			{
				normal = normal.parent_path();
			}
			return normal;
		}

		// One line of user-dirs.dirs: the folder it gives the key, or nothing when it is about another key or not valid.
		std::optional<std::filesystem::path> ParseUserDirectoryLine(std::string_view line, std::string_view key,
		                                                            std::filesystem::path const& home)
		{
			auto const skipBlanks = [&line]
			{
				while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
				{
					line.remove_prefix(1);
				}
			};
			skipBlanks();
			if (!line.starts_with(key))
			{
				return std::nullopt;
			}
			line.remove_prefix(key.size());
			skipBlanks();
			if (!line.starts_with('='))
			{
				return std::nullopt;
			}
			line.remove_prefix(1);
			skipBlanks();
			if (!line.starts_with('"'))
			{
				return std::nullopt;
			}
			line.remove_prefix(1);

			constexpr std::string_view HomeVariable = "$HOME";
			bool const underHome = line.starts_with(HomeVariable);
			if (underHome)
			{
				line.remove_prefix(HomeVariable.size());
				if (!line.starts_with('/') && !line.starts_with('"'))
				{
					return std::nullopt;
				}
			}
			else if (!line.starts_with('/'))
			{
				return std::nullopt;
			}

			std::string value;
			bool closed = false;
			while (!line.empty())
			{
				char character = line.front();
				line.remove_prefix(1);
				if (character == '"')
				{
					closed = true;
					break;
				}
				if (character == '\\' && !line.empty())
				{
					character = line.front();
					line.remove_prefix(1);
				}
				if (static_cast<unsigned char>(character) < 0x20)
				{
					return std::nullopt;
				}
				value.push_back(character);
			}
			skipBlanks();
			if (!closed || !line.empty())
			{
				return std::nullopt;
			}

			if (underHome)
			{
				if (home.empty())
				{
					return std::nullopt;
				}
				// "$HOME", "$HOME/" and "$HOME//Documents/" name folders below home (never the root a leading '/' would).
				size_t const start = value.find_first_not_of('/');
				std::string_view const relative = start == std::string::npos ? std::string_view() : std::string_view(value).substr(start);
				return WithoutTrailingSeparator(relative.empty() ? home : home / FileSystem::PathFromUtf8(relative));
			}
			return WithoutTrailingSeparator(FileSystem::PathFromUtf8(value));
		}

#if defined(ST_PLATFORM_WINDOWS)
		std::string WideToUtf8(std::wstring_view text)
		{
			if (text.empty())
			{
				return {};
			}
			int const wideLength = static_cast<int>(text.size());
			int const length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), wideLength, nullptr, 0, nullptr, nullptr);
			std::string utf8(static_cast<size_t>(std::max(length, 0)), '\0');
			if (length > 0)
			{
				::WideCharToMultiByte(CP_UTF8, 0, text.data(), wideLength, utf8.data(), length, nullptr, nullptr);
			}
			return utf8;
		}
#endif
	}

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
		return WideToUtf8(wideValue);
#else
		char const* value = std::getenv(name.c_str());
		if (value == nullptr)
		{
			return std::nullopt;
		}
		return std::string(value);
#endif
	}

	std::vector<std::string> Platform::ReadCommandLineArguments(int argc, char const* const* argv)
	{
#if defined(ST_PLATFORM_WINDOWS)
		int count = 0;
		LPWSTR* wideArguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
		if (wideArguments != nullptr)
		{
			std::vector<std::string> arguments;
			arguments.reserve(static_cast<size_t>(count));
			for (int i = 0; i < count; i++)
			{
				arguments.push_back(WideToUtf8(wideArguments[i]));
			}
			::LocalFree(wideArguments);
			return arguments;
		}
		// Out of memory: the ANSI arguments are the best that is left.
#endif
		std::vector<std::string> arguments;
		arguments.reserve(static_cast<size_t>(std::max(argc, 0)));
		for (int i = 0; i < argc; i++)
		{
			arguments.emplace_back(argv[i] != nullptr ? argv[i] : "");
		}
		return arguments;
	}

	Platform::MainThreadApartment::MainThreadApartment()
	{
#if defined(ST_PLATFORM_WINDOWS)
		m_Entered = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
#endif
	}

	Platform::MainThreadApartment::~MainThreadApartment()
	{
#if defined(ST_PLATFORM_WINDOWS)
		if (m_Entered)
		{
			::CoUninitialize();
		}
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

	std::optional<std::filesystem::path> Platform::GetDocumentsDirectory()
	{
#if defined(ST_PLATFORM_WINDOWS)
		PWSTR folder = nullptr;
		HRESULT const result = ::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &folder);
		std::optional<std::filesystem::path> documents;
		if (SUCCEEDED(result) && folder != nullptr)
		{
			documents = std::filesystem::path(std::wstring(folder));
		}
		// Freed whether or not the call succeeded, as SHGetKnownFolderPath requires.
		::CoTaskMemFree(folder);
		return documents;
#else
		std::optional<std::string> const home = ReadEnvironmentVariable("HOME");
		std::filesystem::path const homeDirectory = home && !home->empty() ? FileSystem::PathFromUtf8(*home) : std::filesystem::path();
#if defined(ST_PLATFORM_LINUX)
		// xdg-user-dirs keeps the folder's name in the user's language there; XDG_CONFIG_HOME counts only when absolute.
		std::filesystem::path configDirectory;
		if (std::optional<std::string> const configHome = ReadEnvironmentVariable("XDG_CONFIG_HOME");
		    configHome && FileSystem::PathFromUtf8(*configHome).is_absolute())
		{
			configDirectory = FileSystem::PathFromUtf8(*configHome);
		}
		else if (!homeDirectory.empty())
		{
			configDirectory = homeDirectory / ".config";
		}
		if (!configDirectory.empty())
		{
			std::filesystem::path const file = configDirectory / "user-dirs.dirs";
			if (FileSystem::IsRegularFile(file))
			{
				Result<std::string> const text = FileSystem::ReadTextFile(file);
				std::optional<std::filesystem::path> documents =
					text ? ParseUserDirectory(text.GetValue(), "XDG_DOCUMENTS_DIR", homeDirectory) : std::nullopt;
				if (documents)
				{
					return documents;
				}
			}
		}
#endif
		if (homeDirectory.empty())
		{
			return std::nullopt;
		}
		return homeDirectory / "Documents";
#endif
	}

	std::optional<std::filesystem::path> Platform::ParseUserDirectory(std::string_view text, std::string_view key,
	                                                                  std::filesystem::path const& home)
	{
		if (key.empty())
		{
			return std::nullopt;
		}
		std::optional<std::filesystem::path> directory;
		while (!text.empty())
		{
			size_t const end = text.find('\n');
			std::string_view line = text.substr(0, end);
			text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
			if (line.ends_with('\r'))
			{
				line.remove_suffix(1);
			}
			// Later lines win, as they do for the shell that sources the file.
			if (std::optional<std::filesystem::path> parsed = ParseUserDirectoryLine(line, key, home))
			{
				directory = std::move(parsed);
			}
		}
		return directory;
	}
}
