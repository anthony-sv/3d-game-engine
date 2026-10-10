#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Strada
{
	// File helpers. Strings that hold paths are UTF-8; convert with PathFromUtf8/PathToUtf8 so non-ASCII paths work on
	// every platform. All functions are thread-safe.
	class FileSystem
	{
	public:
		[[nodiscard]] static Result<std::string> ReadTextFile(std::filesystem::path const& path);
		[[nodiscard]] static Result<Buffer> ReadBinaryFile(std::filesystem::path const& path);

		// Writes atomically (temporary file + rename) and creates missing parent directories.
		[[nodiscard]] static Result<void> WriteTextFile(std::filesystem::path const& path, std::string_view text);
		[[nodiscard]] static Result<void> WriteBinaryFile(std::filesystem::path const& path, std::span<uint8_t const> data);

		static bool Exists(std::filesystem::path const& path);
		static bool IsDirectory(std::filesystem::path const& path);
		static bool IsRegularFile(std::filesystem::path const& path);

		[[nodiscard]] static Result<void> CreateDirectories(std::filesystem::path const& path);
		// Removes a file or an empty directory. Removing a path that does not exist succeeds.
		[[nodiscard]] static Result<void> Remove(std::filesystem::path const& path);
		// Removes a file or a directory tree. Removing a path that does not exist succeeds.
		[[nodiscard]] static Result<void> RemoveAll(std::filesystem::path const& path);
		[[nodiscard]] static Result<void> Copy(std::filesystem::path const& from, std::filesystem::path const& to, bool overwrite);
		[[nodiscard]] static Result<void> CopyDirectory(std::filesystem::path const& from, std::filesystem::path const& to);
		[[nodiscard]] static Result<void> Move(std::filesystem::path const& from, std::filesystem::path const& to);

		static std::optional<std::filesystem::file_time_type> GetLastWriteTime(std::filesystem::path const& path);

		// Lexically normalized relative path from base to path; empty if no relative path exists.
		static std::filesystem::path GetRelativePath(std::filesystem::path const& path, std::filesystem::path const& base);
		// True if path is base itself or located inside base (lexical check on normalized absolute paths).
		static bool IsInside(std::filesystem::path const& path, std::filesystem::path const& base);
		// A file or directory name (without extension) for a user-chosen name that is valid on every platform: characters
		// other than ASCII letters, digits, spaces, '-' and '_' become '_', surrounding spaces are removed and reserved
		// Windows device names (CON, NUL, COM1, ...) get a trailing '_'. Empty when nothing usable remains.
		static std::string MakePortableFileName(std::string_view name);

		static std::filesystem::path PathFromUtf8(std::string_view utf8);
		// UTF-8 string with forward slashes (the form stored in all Strada files).
		static std::string PathToUtf8(std::filesystem::path const& path);
		// UTF-8 string with the platform's separators (backslashes on Windows): the form for other programs' arguments, which
		// may misread forward slashes as options.
		static std::string PathToNativeUtf8(std::filesystem::path const& path);

		static std::filesystem::path GetExecutablePath();
		static std::filesystem::path GetExecutableDirectory();
		// Per-user writable directory for settings, logs and editor state:
		// Windows %APPDATA%/Strada, macOS ~/Library/Application Support/Strada, Linux $XDG_DATA_HOME/strada.
		static std::filesystem::path GetUserDataDirectory();
	};
}
