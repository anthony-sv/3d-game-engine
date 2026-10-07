#include "stpch.h"
#include "Strada/Core/FileSystem.h"

#include "Strada/Core/Platform.h"
#include "Strada/Core/UUID.h"

#include <fstream>
#include <system_error>
#include <vector>

#if defined(ST_PLATFORM_WINDOWS)
#include <Windows.h>
#elif defined(ST_PLATFORM_LINUX)
#include <unistd.h>
#include <climits>
#elif defined(ST_PLATFORM_MACOS)
#include <mach-o/dyld.h>
#include <climits>
#include <cstdlib>
#endif

namespace Strada
{
	namespace
	{
		Result<void> WriteFileAtomically(std::filesystem::path const& path, void const* data, size_t size)
		{
			std::error_code errorCode;
			std::filesystem::path const parent = path.parent_path();
			if (!parent.empty())
			{
				std::filesystem::create_directories(parent, errorCode);
				if (errorCode)
				{
					return MakeError("Failed to create directory '{}': {}", FileSystem::PathToUtf8(parent), errorCode.message());
				}
			}

			std::filesystem::path temporaryPath = path;
			temporaryPath += ".tmp" + UUID().ToString();

			{
				std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
				if (!stream)
				{
					return MakeError("Failed to open '{}' for writing", FileSystem::PathToUtf8(temporaryPath));
				}
				if (size > 0)
				{
					stream.write(static_cast<char const*>(data), static_cast<std::streamsize>(size));
				}
				stream.flush();
				if (!stream)
				{
					stream.close();
					std::filesystem::remove(temporaryPath, errorCode);
					return MakeError("Failed to write '{}'", FileSystem::PathToUtf8(temporaryPath));
				}
			}

			std::filesystem::rename(temporaryPath, path, errorCode);
			if (errorCode)
			{
				std::error_code removeError;
				std::filesystem::remove(temporaryPath, removeError);
				return MakeError("Failed to replace '{}': {}", FileSystem::PathToUtf8(path), errorCode.message());
			}
			return {};
		}
	}

	Result<std::string> FileSystem::ReadTextFile(std::filesystem::path const& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
		{
			return MakeError("Failed to open '{}' for reading", PathToUtf8(path));
		}

		stream.seekg(0, std::ios::end);
		std::streamoff const size = stream.tellg();
		if (size < 0)
		{
			return MakeError("Failed to determine the size of '{}'", PathToUtf8(path));
		}
		stream.seekg(0, std::ios::beg);

		std::string text(static_cast<size_t>(size), '\0');
		if (size > 0 && !stream.read(text.data(), static_cast<std::streamsize>(size)))
		{
			return MakeError("Failed to read '{}'", PathToUtf8(path));
		}

		// Strip a UTF-8 byte order mark so parsers see clean text.
		if (text.size() >= 3 && static_cast<uint8_t>(text[0]) == 0xEF && static_cast<uint8_t>(text[1]) == 0xBB &&
		    static_cast<uint8_t>(text[2]) == 0xBF)
		{
			text.erase(0, 3);
		}
		return text;
	}

	Result<Buffer> FileSystem::ReadBinaryFile(std::filesystem::path const& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
		{
			return MakeError("Failed to open '{}' for reading", PathToUtf8(path));
		}

		stream.seekg(0, std::ios::end);
		std::streamoff const size = stream.tellg();
		if (size < 0)
		{
			return MakeError("Failed to determine the size of '{}'", PathToUtf8(path));
		}
		stream.seekg(0, std::ios::beg);

		Buffer buffer(static_cast<uint64_t>(size));
		if (size > 0 && !stream.read(reinterpret_cast<char*>(buffer.GetData()), static_cast<std::streamsize>(size)))
		{
			return MakeError("Failed to read '{}'", PathToUtf8(path));
		}
		return buffer;
	}

	Result<void> FileSystem::WriteTextFile(std::filesystem::path const& path, std::string_view text)
	{
		return WriteFileAtomically(path, text.data(), text.size());
	}

	Result<void> FileSystem::WriteBinaryFile(std::filesystem::path const& path, std::span<uint8_t const> data)
	{
		return WriteFileAtomically(path, data.data(), data.size());
	}

	bool FileSystem::Exists(std::filesystem::path const& path)
	{
		std::error_code errorCode;
		return std::filesystem::exists(path, errorCode);
	}

	bool FileSystem::IsDirectory(std::filesystem::path const& path)
	{
		std::error_code errorCode;
		return std::filesystem::is_directory(path, errorCode);
	}

	bool FileSystem::IsRegularFile(std::filesystem::path const& path)
	{
		std::error_code errorCode;
		return std::filesystem::is_regular_file(path, errorCode);
	}

	Result<void> FileSystem::CreateDirectories(std::filesystem::path const& path)
	{
		std::error_code errorCode;
		std::filesystem::create_directories(path, errorCode);
		if (errorCode)
		{
			return MakeError("Failed to create directory '{}': {}", PathToUtf8(path), errorCode.message());
		}
		return {};
	}

	Result<void> FileSystem::Remove(std::filesystem::path const& path)
	{
		std::error_code errorCode;
		std::filesystem::remove(path, errorCode);
		if (errorCode)
		{
			return MakeError("Failed to remove '{}': {}", PathToUtf8(path), errorCode.message());
		}
		return {};
	}

	Result<void> FileSystem::RemoveAll(std::filesystem::path const& path)
	{
		std::error_code errorCode;
		std::filesystem::remove_all(path, errorCode);
		if (errorCode)
		{
			return MakeError("Failed to remove '{}': {}", PathToUtf8(path), errorCode.message());
		}
		return {};
	}

	Result<void> FileSystem::Copy(std::filesystem::path const& from, std::filesystem::path const& to, bool overwrite)
	{
		std::error_code errorCode;
		std::filesystem::path const parent = to.parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, errorCode);
			if (errorCode)
			{
				return MakeError("Failed to create directory '{}': {}", PathToUtf8(parent), errorCode.message());
			}
		}

		std::filesystem::copy_options const options =
			overwrite ? std::filesystem::copy_options::overwrite_existing : std::filesystem::copy_options::none;
		std::filesystem::copy_file(from, to, options, errorCode);
		if (errorCode)
		{
			return MakeError("Failed to copy '{}' to '{}': {}", PathToUtf8(from), PathToUtf8(to), errorCode.message());
		}
		return {};
	}

	Result<void> FileSystem::CopyDirectory(std::filesystem::path const& from, std::filesystem::path const& to)
	{
		std::error_code errorCode;
		std::filesystem::create_directories(to, errorCode);
		if (errorCode)
		{
			return MakeError("Failed to create directory '{}': {}", PathToUtf8(to), errorCode.message());
		}

		std::filesystem::copy(from, to, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
		                      errorCode);
		if (errorCode)
		{
			return MakeError("Failed to copy directory '{}' to '{}': {}", PathToUtf8(from), PathToUtf8(to), errorCode.message());
		}
		return {};
	}

	Result<void> FileSystem::Move(std::filesystem::path const& from, std::filesystem::path const& to)
	{
		std::error_code errorCode;
		std::filesystem::path const parent = to.parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, errorCode);
			if (errorCode)
			{
				return MakeError("Failed to create directory '{}': {}", PathToUtf8(parent), errorCode.message());
			}
		}

		std::filesystem::rename(from, to, errorCode);
		if (errorCode)
		{
			return MakeError("Failed to move '{}' to '{}': {}", PathToUtf8(from), PathToUtf8(to), errorCode.message());
		}
		return {};
	}

	std::optional<std::filesystem::file_time_type> FileSystem::GetLastWriteTime(std::filesystem::path const& path)
	{
		std::error_code errorCode;
		std::filesystem::file_time_type const time = std::filesystem::last_write_time(path, errorCode);
		if (errorCode)
		{
			return std::nullopt;
		}
		return time;
	}

	std::filesystem::path FileSystem::GetRelativePath(std::filesystem::path const& path, std::filesystem::path const& base)
	{
		std::error_code errorCode;
		std::filesystem::path const absolutePath = std::filesystem::absolute(path, errorCode).lexically_normal();
		if (errorCode)
		{
			return {};
		}
		std::filesystem::path const absoluteBase = std::filesystem::absolute(base, errorCode).lexically_normal();
		if (errorCode)
		{
			return {};
		}
		return absolutePath.lexically_relative(absoluteBase);
	}

	bool FileSystem::IsInside(std::filesystem::path const& path, std::filesystem::path const& base)
	{
		std::filesystem::path const relative = GetRelativePath(path, base);
		if (relative.empty())
		{
			return false;
		}
		auto const first = relative.begin();
		return first == relative.end() || *first != "..";
	}

	std::filesystem::path FileSystem::PathFromUtf8(std::string_view utf8)
	{
		return std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(utf8.data()), utf8.size()));
	}

	std::string FileSystem::PathToUtf8(std::filesystem::path const& path)
	{
		std::u8string const text = path.generic_u8string();
		return std::string(reinterpret_cast<char const*>(text.data()), text.size());
	}

	std::filesystem::path FileSystem::GetExecutablePath()
	{
#if defined(ST_PLATFORM_WINDOWS)
		std::vector<wchar_t> buffer(MAX_PATH);
		while (true)
		{
			DWORD const length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length == 0)
			{
				return {};
			}
			if (length < buffer.size())
			{
				return std::filesystem::path(std::wstring(buffer.data(), length));
			}
			buffer.resize(buffer.size() * 2);
		}
#elif defined(ST_PLATFORM_LINUX)
		std::error_code errorCode;
		std::filesystem::path const path = std::filesystem::read_symlink("/proc/self/exe", errorCode);
		return errorCode ? std::filesystem::path() : path;
#elif defined(ST_PLATFORM_MACOS)
		uint32_t size = 0;
		::_NSGetExecutablePath(nullptr, &size);
		std::vector<char> buffer(size + 1, '\0');
		if (::_NSGetExecutablePath(buffer.data(), &size) != 0)
		{
			return {};
		}
		char resolved[PATH_MAX] = {};
		if (::realpath(buffer.data(), resolved) == nullptr)
		{
			return std::filesystem::path(buffer.data());
		}
		return std::filesystem::path(resolved);
#endif
	}

	std::filesystem::path FileSystem::GetExecutableDirectory()
	{
		return GetExecutablePath().parent_path();
	}

	std::filesystem::path FileSystem::GetUserDataDirectory()
	{
#if defined(ST_PLATFORM_WINDOWS)
		if (std::optional<std::string> const appData = Platform::ReadEnvironmentVariable("APPDATA"))
		{
			return PathFromUtf8(*appData) / "Strada";
		}
		if (std::optional<std::string> const userProfile = Platform::ReadEnvironmentVariable("USERPROFILE"))
		{
			return PathFromUtf8(*userProfile) / "AppData" / "Roaming" / "Strada";
		}
		return GetExecutableDirectory() / "UserData";
#elif defined(ST_PLATFORM_MACOS)
		if (std::optional<std::string> const home = Platform::ReadEnvironmentVariable("HOME"))
		{
			return PathFromUtf8(*home) / "Library" / "Application Support" / "Strada";
		}
		return GetExecutableDirectory() / "UserData";
#elif defined(ST_PLATFORM_LINUX)
		if (std::optional<std::string> const dataHome = Platform::ReadEnvironmentVariable("XDG_DATA_HOME"); dataHome && !dataHome->empty())
		{
			return PathFromUtf8(*dataHome) / "strada";
		}
		if (std::optional<std::string> const home = Platform::ReadEnvironmentVariable("HOME"))
		{
			return PathFromUtf8(*home) / ".local" / "share" / "strada";
		}
		return GetExecutableDirectory() / "UserData";
#endif
	}
}
