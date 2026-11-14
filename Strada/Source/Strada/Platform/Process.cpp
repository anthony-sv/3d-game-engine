#include "stpch.h"
#include "Strada/Platform/Process.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"

#include <reproc++/reproc.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwchar>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(ST_PLATFORM_WINDOWS)
#include <Windows.h>
#else
// The process's environment; not every platform's headers declare it.
extern char** environ;
#endif

namespace Strada
{
	namespace
	{
		// How often a running program is checked for cancellation and its time limit.
		constexpr reproc::milliseconds PollInterval(50);
		// Output still arriving this soon after the program exited is collected; its own children may keep the pipe open
		// far longer (build servers, for one).
		constexpr reproc::milliseconds TrailingOutputTime(200);

		using Clock = std::chrono::steady_clock;

		class ProcessRun
		{
		public:
			ProcessRun(ProcessSpecification const& specification, std::atomic<bool> const* cancel)
				: m_Specification(specification),
				  m_Cancel(cancel),
				  m_Start(Clock::now())
			{
			}

			// Whether the program must be stopped now; records why.
			bool ShouldStop(ProcessResult& result) const
			{
				if (m_Cancel != nullptr && m_Cancel->load(std::memory_order_relaxed))
				{
					result.Cancelled = true;
					return true;
				}
				if (m_Specification.Timeout.count() > 0 && Clock::now() - m_Start >= m_Specification.Timeout)
				{
					result.TimedOut = true;
					return true;
				}
				return false;
			}

		private:
			ProcessSpecification const& m_Specification;
			std::atomic<bool> const* m_Cancel;
			Clock::time_point m_Start;
		};

#if defined(ST_PLATFORM_WINDOWS)
		// reproc returns exit codes as int and takes negative ones for errors, but a program ended by an exception exits with
		// its NTSTATUS code (0xC0000005 for an access violation), negative as an int. Reads the code from the system while
		// reproc still holds the process open, so the ID cannot have been reused; empty while the program runs.
		std::optional<int32_t> ReadExitCode(int processID)
		{
			HANDLE const process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processID));
			if (process == nullptr)
			{
				return std::nullopt;
			}
			DWORD code = 0;
			BOOL const read = ::GetExitCodeProcess(process, &code);
			::CloseHandle(process);
			if (read == FALSE || code == STILL_ACTIVE)
			{
				return std::nullopt;
			}
			return static_cast<int32_t>(code);
		}
#endif

		// Collects the program's output until its pipe closes, it exits (plus TrailingOutputTime) or it must stop.
		Result<void> CollectOutput(reproc::process& process, ProcessRun const& run, ProcessResult& result)
		{
			std::array<uint8_t, 16384> buffer{};
			std::optional<Clock::time_point> trailingDeadline;
			while (true)
			{
				if (run.ShouldStop(result))
				{
					return {};
				}
				reproc::milliseconds timeout = PollInterval;
				if (trailingDeadline)
				{
					Clock::duration const remaining = *trailingDeadline - Clock::now();
					if (remaining <= Clock::duration::zero())
					{
						return {};
					}
					timeout = std::min(timeout, std::chrono::ceil<reproc::milliseconds>(remaining));
				}
				int const interests = trailingDeadline ? reproc::event::out : (reproc::event::out | reproc::event::exit);
				auto const [events, pollError] = process.poll(interests, timeout);
				if (pollError == std::errc::broken_pipe)
				{
					return {};
				}
				if (pollError)
				{
					return MakeError("waiting for the program failed: {}", pollError.message());
				}
				if ((events & reproc::event::exit) != 0 && !trailingDeadline)
				{
					trailingDeadline = Clock::now() + TrailingOutputTime;
				}
				if ((events & reproc::event::out) != 0)
				{
					auto const [bytes, readError] = process.read(reproc::stream::out, buffer.data(), buffer.size());
					if (readError == std::errc::broken_pipe)
					{
						return {};
					}
					if (readError)
					{
						return MakeError("reading the program's output failed: {}", readError.message());
					}
					result.Output.append(reinterpret_cast<char const*>(buffer.data()), bytes);
				}
			}
		}

		bool IsExecutableFile(std::filesystem::path const& path)
		{
			std::error_code error;
			std::filesystem::file_status const status = std::filesystem::status(path, error);
			if (error || !std::filesystem::is_regular_file(status))
			{
				return false;
			}
#if defined(ST_PLATFORM_WINDOWS)
			return true;
#else
			using std::filesystem::perms;
			return (status.permissions() & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none;
#endif
		}

		std::vector<std::string> SplitList(std::string const& text, char separator)
		{
			std::vector<std::string> parts;
			size_t start = 0;
			while (start <= text.size())
			{
				size_t const end = std::min(text.find(separator, start), text.size());
				if (end > start)
				{
					parts.push_back(text.substr(start, end - start));
				}
				start = end + 1;
			}
			return parts;
		}

		using EnvironmentVariables = std::vector<std::pair<std::string, std::string>>;

		// Whether two environment variable names name one variable: Windows ignores the letter case of names.
		bool IsSameVariable(std::string_view first, std::string_view second)
		{
#if defined(ST_PLATFORM_WINDOWS)
			return std::ranges::equal(first, second,
			                          [](char a, char b)
			                          {
										  return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
									  });
#else
			return first == second;
#endif
		}

		// This process's environment, as name and value pairs in its own order. A name is what comes before the first '='
		// after its first character, which keeps the hidden "=C:=C:\..." entries of Windows whole.
		EnvironmentVariables ReadEnvironment()
		{
			EnvironmentVariables variables;
			auto const add = [&variables](std::string_view text)
			{
				size_t const separator = text.find('=', 1);
				if (separator != std::string_view::npos)
				{
					variables.emplace_back(std::string(text.substr(0, separator)), std::string(text.substr(separator + 1)));
				}
			};
#if defined(ST_PLATFORM_WINDOWS)
			wchar_t* const block = ::GetEnvironmentStringsW();
			if (block == nullptr)
			{
				return variables;
			}
			for (wchar_t const* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1)
			{
				int const wideLength = static_cast<int>(std::wcslen(entry));
				int const length = ::WideCharToMultiByte(CP_UTF8, 0, entry, wideLength, nullptr, 0, nullptr, nullptr);
				std::string utf8(static_cast<size_t>(std::max(length, 0)), '\0');
				if (length > 0)
				{
					::WideCharToMultiByte(CP_UTF8, 0, entry, wideLength, utf8.data(), length, nullptr, nullptr);
				}
				add(utf8);
			}
			::FreeEnvironmentStringsW(block);
#else
			for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
			{
				add(*entry);
			}
#endif
			return variables;
		}

		// The environment a program starts with: this process's, with the specification's variables replacing inherited ones
		// of the same name in place and the others added at the end. reproc only appends to the inherited environment, and
		// the platforms' lookups find the first of two entries of one name, which would be the inherited one.
		EnvironmentVariables MakeEnvironment(EnvironmentVariables const& changes)
		{
			EnvironmentVariables variables = ReadEnvironment();
			for (std::pair<std::string, std::string> const& change : changes)
			{
				auto const inherited = std::ranges::find_if(variables,
				                                            [&change](std::pair<std::string, std::string> const& variable)
				                                            {
																return IsSameVariable(variable.first, change.first);
															});
				if (inherited != variables.end())
				{
					inherited->second = change.second;
				}
				else
				{
					variables.push_back(change);
				}
			}
			return variables;
		}
	}

	Result<ProcessResult> Process::Run(ProcessSpecification const& specification, std::atomic<bool> const* cancel)
	{
		std::vector<std::string> arguments;
		arguments.reserve(specification.Arguments.size() + 1);
		arguments.push_back(FileSystem::PathToNativeUtf8(specification.Executable));
		arguments.insert(arguments.end(), specification.Arguments.begin(), specification.Arguments.end());
		std::string const workingDirectory = FileSystem::PathToNativeUtf8(specification.WorkingDirectory);

		reproc::options options;
		options.redirect.in.type = reproc::redirect::discard;
		options.redirect.out.type = reproc::redirect::pipe;
		options.redirect.err.type = reproc::redirect::stdout_;
		options.working_directory = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
		if (specification.Environment.empty())
		{
			options.env.behavior = reproc::env::extend;
		}
		else
		{
			options.env.behavior = reproc::env::empty;
			options.env.extra = reproc::env(MakeEnvironment(specification.Environment));
		}
		// Whatever happens below, the program does not outlive this call.
		options.stop = {{reproc::stop::kill, reproc::infinite},
		                {reproc::stop::noop, reproc::milliseconds(0)},
		                {reproc::stop::noop, reproc::milliseconds(0)}};

		reproc::process process;
		if (std::error_code const error = process.start(arguments, options))
		{
			if (error == std::errc::no_such_file_or_directory)
			{
				return MakeError("'{}' was not found", arguments.front());
			}
			return MakeError("'{}' cannot be started: {}", arguments.front(), error.message());
		}

		ProcessRun const run(specification, cancel);
		ProcessResult result;
		if (Result<void> collected = CollectOutput(process, run, result); !collected)
		{
			return MakeError("'{}': {}", arguments.front(), collected.GetError());
		}

		// The program may have closed its output and still run: wait for it, still honoring the time limit.
		while (true)
		{
			if (result.Cancelled || result.TimedOut || run.ShouldStop(result))
			{
				process.kill();
				process.wait(reproc::infinite);
				return result;
			}
			auto const [status, waitError] = process.wait(PollInterval);
			if (!waitError)
			{
				result.ExitCode = status;
				return result;
			}
			if (waitError != std::errc::timed_out)
			{
#if defined(ST_PLATFORM_WINDOWS)
				if (std::optional<int32_t> const exitCode = ReadExitCode(process.pid().first))
				{
					result.ExitCode = *exitCode;
					return result;
				}
#endif
				return MakeError("'{}': waiting for it failed: {}", arguments.front(), waitError.message());
			}
		}
	}

	std::optional<std::filesystem::path> Process::FindExecutable(std::string_view name)
	{
		if (name.empty())
		{
			return std::nullopt;
		}
		std::optional<std::string> const path = Platform::ReadEnvironmentVariable("PATH");
		if (!path)
		{
			return std::nullopt;
		}
#if defined(ST_PLATFORM_WINDOWS)
		char const separator = ';';
		std::string const extensionList = Platform::ReadEnvironmentVariable("PATHEXT").value_or(".COM;.EXE;.BAT;.CMD");
		std::vector<std::string> extensions = SplitList(extensionList, ';');
		// A name with its extension is tried as it is.
		extensions.insert(extensions.begin(), std::string());
#else
		char const separator = ':';
		std::vector<std::string> const extensions = {std::string()};
#endif
		for (std::string const& directory : SplitList(*path, separator))
		{
			for (std::string const& extension : extensions)
			{
				std::filesystem::path const candidate =
					FileSystem::PathFromUtf8(directory) / FileSystem::PathFromUtf8(std::string(name) + extension);
				if (IsExecutableFile(candidate))
				{
					std::error_code error;
					std::filesystem::path const absolute = std::filesystem::absolute(candidate, error);
					return error ? candidate : absolute;
				}
			}
		}
		return std::nullopt;
	}
}
