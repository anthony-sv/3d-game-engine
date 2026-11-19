#include "stpch.h"
#include "Strada/Core/FileSystem.h"

#include "Strada/Core/GameData.h"
#include "Strada/Core/Platform.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Core/UUID.h"
#include "Strada/Core/Utf8.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(ST_PLATFORM_WINDOWS)
#include <Windows.h>
#elif defined(ST_PLATFORM_LINUX)
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#elif defined(ST_PLATFORM_MACOS)
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <cstdlib>
#endif

namespace Strada
{
	namespace
	{
#if defined(ST_PLATFORM_WINDOWS)
		// Unlike std::filesystem's conversion, which throws, unpaired surrogates (valid in Windows file names) become
		// U+FFFD.
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

		Result<void> CreateParentDirectories(std::filesystem::path const& path)
		{
			std::filesystem::path const parent = path.parent_path();
			if (parent.empty())
			{
				return {};
			}
			std::error_code errorCode;
			std::filesystem::create_directories(parent, errorCode);
			if (errorCode)
			{
				return MakeError("Failed to create directory '{}': {}", FileSystem::PathToUtf8(parent), errorCode.message());
			}
			return {};
		}

		// The temporary file a write goes into, beside the path: <path>.tmp<UUID>.
		std::filesystem::path MakeTemporaryPath(std::filesystem::path const& path)
		{
			std::filesystem::path temporaryPath = path;
			temporaryPath += ".tmp" + UUID().ToString();
			return temporaryPath;
		}

		void RemoveTemporaryFile(std::filesystem::path const& temporaryPath)
		{
			std::error_code removeError;
			std::filesystem::remove(temporaryPath, removeError);
		}

#if defined(ST_PLATFORM_WINDOWS)
		std::string DescribeWindowsError(DWORD error)
		{
			return std::system_category().message(static_cast<int>(error));
		}

		// Writes the bytes into a new file, syncs them to the device and closes it. The caller removes the file when this
		// fails.
		Result<void> WriteSyncedFile(std::filesystem::path const& path, void const* data, size_t size)
		{
			HANDLE const file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				return MakeError("Failed to open '{}' for writing: {}", FileSystem::PathToUtf8(path),
				                 DescribeWindowsError(::GetLastError()));
			}
			DWORD error = ERROR_SUCCESS;
			char const* bytes = static_cast<char const*>(data);
			size_t remaining = size;
			while (remaining > 0 && error == ERROR_SUCCESS)
			{
				// WriteFile takes a 32-bit count: larger files go in chunks.
				DWORD const chunk = static_cast<DWORD>(std::min<size_t>(remaining, size_t{1} << 30));
				DWORD written = 0;
				if (!::WriteFile(file, bytes, chunk, &written, nullptr))
				{
					error = ::GetLastError();
				}
				else if (written == 0)
				{
					error = ERROR_WRITE_FAULT;
				}
				bytes += written;
				remaining -= written;
			}
			if (error == ERROR_SUCCESS)
			{
				ST_PROFILE_SCOPE("SyncFile");
				if (!::FlushFileBuffers(file))
				{
					error = ::GetLastError();
				}
			}
			if (!::CloseHandle(file) && error == ERROR_SUCCESS)
			{
				error = ::GetLastError();
			}
			if (error != ERROR_SUCCESS)
			{
				return MakeError("Failed to write '{}': {}", FileSystem::PathToUtf8(path), DescribeWindowsError(error));
			}
			return {};
		}

		// Renames the synced file over the path. MOVEFILE_WRITE_THROUGH waits for the move to reach the device where the
		// system makes one wait (a move across volumes); within a volume the rename is a journaled change of the file
		// system's metadata.
		Result<void> ReplaceDurably(std::filesystem::path const& temporaryPath, std::filesystem::path const& path)
		{
			ST_PROFILE_SCOPE("SyncFile");
			if (!::MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				return MakeError("Failed to replace '{}': {}", FileSystem::PathToUtf8(path), DescribeWindowsError(::GetLastError()));
			}
			return {};
		}
#else
		std::string DescribeErrno(int error)
		{
			return std::error_code(error, std::generic_category()).message();
		}

		// Syncs what the descriptor wrote, or the folder it names, to the device: 0, or the error. On macOS fsync leaves the
		// data in the drive's cache, which F_FULLFSYNC empties; a file system that refuses F_FULLFSYNC gets fsync.
		int SyncDescriptor(int descriptor)
		{
			ST_PROFILE_SCOPE("SyncFile");
#if defined(ST_PLATFORM_MACOS)
			if (::fcntl(descriptor, F_FULLFSYNC) == 0)
			{
				return 0;
			}
#endif
			while (::fsync(descriptor) != 0)
			{
				if (errno != EINTR)
				{
					return errno;
				}
			}
			return 0;
		}

		// Writes the bytes into a new file, syncs them to the device and closes it. The caller removes the file when this
		// fails.
		Result<void> WriteSyncedFile(std::filesystem::path const& path, void const* data, size_t size)
		{
			// Created as std::ofstream creates files (0666 less the process's umask), and never over an existing one.
			int const descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
			if (descriptor < 0)
			{
				return MakeError("Failed to open '{}' for writing: {}", FileSystem::PathToUtf8(path), DescribeErrno(errno));
			}
			int error = 0;
			char const* bytes = static_cast<char const*>(data);
			size_t remaining = size;
			while (remaining > 0 && error == 0)
			{
				ssize_t const written = ::write(descriptor, bytes, std::min<size_t>(remaining, size_t{1} << 30));
				if (written < 0)
				{
					if (errno != EINTR)
					{
						error = errno;
					}
				}
				else if (written == 0)
				{
					error = EIO;
				}
				else
				{
					bytes += written;
					remaining -= static_cast<size_t>(written);
				}
			}
			if (error == 0)
			{
				error = SyncDescriptor(descriptor);
			}
			// A close that fails may have lost data written to a network file system; it is not retried (the descriptor is
			// gone whatever close answers).
			if (::close(descriptor) != 0 && error == 0)
			{
				error = errno;
			}
			if (error != 0)
			{
				return MakeError("Failed to write '{}': {}", FileSystem::PathToUtf8(path), DescribeErrno(error));
			}
			return {};
		}

		// Renames the synced file over the path, then syncs the folder that holds it, so that the rename lasts. A folder that
		// cannot be synced is logged: the new file is in place and read, and only its lasting through a power loss is in
		// doubt. File systems that cannot sync a folder (EINVAL, ENOTSUP) say nothing.
		Result<void> ReplaceDurably(std::filesystem::path const& temporaryPath, std::filesystem::path const& path)
		{
			if (std::rename(temporaryPath.c_str(), path.c_str()) != 0)
			{
				return MakeError("Failed to replace '{}': {}", FileSystem::PathToUtf8(path), DescribeErrno(errno));
			}
			std::filesystem::path const parent = path.parent_path();
			std::filesystem::path const folder = parent.empty() ? std::filesystem::path(".") : parent;
			int const descriptor = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			int const error = descriptor < 0 ? errno : SyncDescriptor(descriptor);
			if (descriptor >= 0)
			{
				::close(descriptor);
			}
			if (error != 0 && error != EINVAL && error != ENOTSUP)
			{
				ST_CORE_WARN("FileSystem: '{}' is written, but its folder could not be synced to the device: {}",
				             FileSystem::PathToUtf8(path), DescribeErrno(error));
			}
			return {};
		}
#endif

		// Durable form of WriteFileAtomically (Docs/Architecture.md §13.1, "The durable write").
		Result<void> WriteFileDurably(std::filesystem::path const& path, void const* data, size_t size)
		{
			if (Result<void> created = CreateParentDirectories(path); !created)
			{
				return created;
			}
			std::filesystem::path const temporaryPath = MakeTemporaryPath(path);
			if (Result<void> written = WriteSyncedFile(temporaryPath, data, size); !written)
			{
				RemoveTemporaryFile(temporaryPath);
				return written;
			}
			if (Result<void> replaced = ReplaceDurably(temporaryPath, path); !replaced)
			{
				RemoveTemporaryFile(temporaryPath);
				return replaced;
			}
			return {};
		}

		Result<void> WriteFileAtomically(std::filesystem::path const& path, void const* data, size_t size)
		{
			if (Result<void> created = CreateParentDirectories(path); !created)
			{
				return created;
			}

			std::error_code errorCode;
			std::filesystem::path const temporaryPath = MakeTemporaryPath(path);

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

		// The directory followed by the names of relative, each one that names an existing file or folder spelled as stored.
		// canonical() reads the stored name of the last element but also replaces links by their targets: links, and what
		// follows a name that does not exist, keep the spelling given.
		std::filesystem::path AppendStoredNames(std::filesystem::path directory, std::filesystem::path const& relative)
		{
			bool exists = true;
			for (std::filesystem::path const& name : relative)
			{
				std::filesystem::path next = directory / name;
				if (exists && !name.empty())
				{
					std::error_code error;
					std::filesystem::file_status const status = std::filesystem::symlink_status(next, error);
					exists = !error && std::filesystem::exists(status);
					if (exists && (std::filesystem::is_directory(status) || std::filesystem::is_regular_file(status)))
					{
						std::filesystem::path const stored = std::filesystem::canonical(next, error);
						if (!error && stored.has_filename())
						{
							next = directory / stored.filename();
						}
					}
				}
				directory = std::move(next);
			}
			return directory;
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

	Result<void> FileSystem::WriteTextFileDurably(std::filesystem::path const& path, std::string_view text)
	{
		return WriteFileDurably(path, text.data(), text.size());
	}

	Result<void> FileSystem::WriteBinaryFileDurably(std::filesystem::path const& path, std::span<uint8_t const> data)
	{
		return WriteFileDurably(path, data.data(), data.size());
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

	std::optional<uint64_t> FileSystem::GetFileSize(std::filesystem::path const& path)
	{
		// file_size of a directory reports an error on some standard libraries and a size on others (MSVC's).
		std::error_code errorCode;
		if (!std::filesystem::is_regular_file(path, errorCode) || errorCode)
		{
			return std::nullopt;
		}
		std::uintmax_t const size = std::filesystem::file_size(path, errorCode);
		if (errorCode)
		{
			return std::nullopt;
		}
		return static_cast<uint64_t>(size);
	}

	bool FileSystem::IsReadOnly(std::filesystem::path const& path)
	{
		std::error_code errorCode;
		std::filesystem::file_status const status = std::filesystem::status(path, errorCode);
		if (errorCode || !std::filesystem::exists(status))
		{
			return false;
		}
		constexpr std::filesystem::perms Writable =
			std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write;
		return (status.permissions() & Writable) == std::filesystem::perms::none;
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

	std::filesystem::path FileSystem::GetStoredSpelling(std::filesystem::path const& path)
	{
		std::error_code error;
		std::filesystem::path const absolutePath = std::filesystem::absolute(path, error).lexically_normal();
		if (error)
		{
			return path.lexically_normal();
		}
		// A drive letter names its drive in either case.
		std::filesystem::path::string_type rootName = absolutePath.root_name().native();
		if (rootName.size() == 2 && rootName[1] == ':' && rootName[0] >= 'a' && rootName[0] <= 'z')
		{
			rootName[0] = static_cast<std::filesystem::path::value_type>(rootName[0] - 'a' + 'A');
		}
		return AppendStoredNames(std::filesystem::path(rootName) / absolutePath.root_directory(), absolutePath.relative_path());
	}

	std::optional<std::filesystem::path> FileSystem::FindInside(std::filesystem::path const& path, std::filesystem::path const& base)
	{
		std::error_code error;
		std::filesystem::path absoluteBase = std::filesystem::absolute(base, error).lexically_normal();
		if (error)
		{
			return std::nullopt;
		}
		if (!absoluteBase.has_filename() && absoluteBase.has_relative_path())
		{
			absoluteBase = absoluteBase.parent_path();
		}
		auto const isBelow = [](std::filesystem::path const& relative)
		{
			return !relative.empty() && *relative.begin() != "..";
		};
		// Spelled like base where it passes through it, only the names below base need reading.
		if (std::filesystem::path const relative = GetRelativePath(path, absoluteBase); isBelow(relative))
		{
			return relative == "." ? absoluteBase : AppendStoredNames(absoluteBase, relative).lexically_normal();
		}
		std::filesystem::path const relative = GetStoredSpelling(path).lexically_relative(GetStoredSpelling(absoluteBase));
		if (!isBelow(relative))
		{
			return std::nullopt;
		}
		return relative == "." ? absoluteBase : (absoluteBase / relative).lexically_normal();
	}

	std::string FileSystem::MakePortableFileName(std::string_view name)
	{
		std::string fileName;
		fileName.reserve(name.size());
		for (char const c : name)
		{
			unsigned char const byte = static_cast<unsigned char>(c);
			bool const portable = (byte < 0x80 && std::isalnum(byte) != 0) || c == ' ' || c == '-' || c == '_';
			fileName += portable ? c : '_';
		}
		size_t const first = fileName.find_first_not_of(' ');
		if (first == std::string::npos)
		{
			return {};
		}
		fileName = fileName.substr(first, fileName.find_last_not_of(' ') - first + 1);

		std::string upper = fileName;
		std::transform(upper.begin(), upper.end(), upper.begin(),
		               [](unsigned char c)
		               {
						   return static_cast<char>(std::toupper(c));
					   });
		static constexpr std::array<std::string_view, 4> s_ReservedNames = {"CON", "PRN", "AUX", "NUL"};
		bool const reserved =
			std::find(s_ReservedNames.begin(), s_ReservedNames.end(), upper) != s_ReservedNames.end() ||
			(upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '0' && upper[3] <= '9');
		return reserved ? fileName + "_" : fileName;
	}

	std::filesystem::path FileSystem::PathFromUtf8(std::string_view utf8)
	{
		// The conversion to the native encoding throws for invalid UTF-8 on Windows, and paths come from user data.
		if (!Utf8::IsValid(utf8))
		{
			std::string const sanitized = Utf8::Sanitize(utf8);
			return std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(sanitized.data()), sanitized.size()));
		}
		return std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(utf8.data()), utf8.size()));
	}

	std::string FileSystem::PathToUtf8(std::filesystem::path const& path)
	{
#if defined(ST_PLATFORM_WINDOWS)
		std::wstring text = path.native();
		std::replace(text.begin(), text.end(), L'\\', L'/');
		return WideToUtf8(text);
#else
		std::u8string const text = path.generic_u8string();
		return std::string(reinterpret_cast<char const*>(text.data()), text.size());
#endif
	}

	std::string FileSystem::PathToNativeUtf8(std::filesystem::path const& path)
	{
#if defined(ST_PLATFORM_WINDOWS)
		std::wstring text = path.native();
		std::replace(text.begin(), text.end(), L'/', L'\\');
		return WideToUtf8(text);
#else
		std::u8string const text = path.u8string();
		return std::string(reinterpret_cast<char const*>(text.data()), text.size());
#endif
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
		constexpr UserDataPlatform platform = UserDataLocations::GetHostPlatform();
		if (std::optional<std::filesystem::path> const root = UserDataLocations::FindRoot(platform, &Platform::ReadEnvironmentVariable))
		{
			return UserDataLocations::MakeEngineDirectory(*root, platform);
		}
		return UserDataLocations::MakeFallbackEngineDirectory(GetExecutableDirectory());
	}
}
