#pragma once

#include "Strada/Core/Base.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// Small OS queries that the core needs before any other subsystem exists. All functions are thread-safe.
	class Platform
	{
	public:
		static bool IsDebuggerAttached();
		static uint32_t GetProcessID();
		// Whether a process with this ID currently exists (false for 0). Used to detect stale instance files.
		static bool IsProcessRunning(uint32_t processID);
		// Named to avoid the Win32 GetEnvironmentVariable macro.
		static std::optional<std::string> ReadEnvironmentVariable(std::string const& name);
		// The process arguments as UTF-8, program name first. On Windows they are read from the wide command line, since
		// main's argv holds them in the ANSI code page (characters outside it become '?'); elsewhere argv is copied.
		static std::vector<std::string> ReadCommandLineArguments(int argc, char const* const* argv);
		// Monotonic time in seconds since an unspecified, process-constant epoch.
		static double GetTime();
		// POSIX systems end a process that writes to a pipe or socket whose reading end is closed (SIGPIPE); afterwards such
		// writes fail with EPIPE instead. Applications do this at startup: an editor that a tool started keeps running when
		// the tool exits and the editor's output pipe closes. Process-wide; nothing to do on Windows, where the writes fail.
		static void IgnoreBrokenPipeSignal();
		// The user's Documents folder (it need not exist): Windows' known folder (FOLDERID_Documents); macOS
		// $HOME/Documents; Linux XDG_DOCUMENTS_DIR from $XDG_CONFIG_HOME/user-dirs.dirs (else ~/.config/user-dirs.dirs),
		// else $HOME/Documents. Nothing when it cannot be told (no home directory).
		[[nodiscard]] static std::optional<std::filesystem::path> GetDocumentsDirectory();
		// The folder a user-dirs.dirs text (xdg-user-dirs' file) gives a key such as "XDG_DOCUMENTS_DIR": the last line
		// `KEY="$HOME/<path>"`, `KEY="$HOME"` or `KEY="/<absolute path>"` (backslashes escape the next character; blank lines
		// and lines starting with '#' are skipped), with $HOME replaced by home. Nothing when no line of the key is valid,
		// or when the value is under $HOME and home is empty. Pure: compiled and tested on every platform.
		[[nodiscard]] static std::optional<std::filesystem::path> ParseUserDirectory(std::string_view text, std::string_view key,
		                                                                             std::filesystem::path const& home);

		// Windows: makes the calling thread a single-threaded COM apartment while it exists (nothing elsewhere). Applications
		// hold one on the main thread for their lifetime: shell dialogs (the editor's file dialogs) require such an
		// apartment and can hang in a multithreaded one, which miniaudio would otherwise make the main thread when the audio
		// engine starts (it keeps an apartment that already exists).
		class MainThreadApartment
		{
		public:
			MainThreadApartment();
			~MainThreadApartment();

			MainThreadApartment(MainThreadApartment const&) = delete;
			MainThreadApartment& operator=(MainThreadApartment const&) = delete;

			// Whether this object entered the apartment (false when the thread already had another one).
			bool IsEntered() const { return m_Entered; }

		private:
			bool m_Entered = false;
		};
	};
}
