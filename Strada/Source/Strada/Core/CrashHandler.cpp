#include "stpch.h"
#include "Strada/Core/CrashHandler.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Platform.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#if defined(ST_PLATFORM_WINDOWS)
#include <Windows.h>

// Needs Windows.h first.
#include <DbgHelp.h>
#include <csignal>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <csignal>
#endif

namespace Strada
{
	namespace
	{
		constexpr size_t MaxPathLength = 1024;

		// Fixed-size and trivially destructible: the handlers read it without allocating, even during static destruction.
		struct CrashHandlerData
		{
			std::array<char, 128> Name{};
			// UTF-8, for the report's text and for the POSIX file functions.
			std::array<char, MaxPathLength> ReportPath{};
#if defined(ST_PLATFORM_WINDOWS)
			std::array<wchar_t, MaxPathLength> ReportPathWide{};
			std::array<wchar_t, MaxPathLength> DumpPathWide{};
			std::array<char, MaxPathLength> DumpPath{};
			// The C runtime's filter, which turns uncaught C++ exceptions into std::terminate.
			LPTOP_LEVEL_EXCEPTION_FILTER RuntimeFilter = nullptr;
#endif
			bool Installed = false;
		};

		CrashHandlerData s_Data;

		template<typename TChar, size_t Size>
		bool CopyTerminated(std::basic_string<TChar> const& text, std::array<TChar, Size>& destination)
		{
			if (text.size() >= Size)
			{
				return false;
			}
			std::copy(text.begin(), text.end(), destination.begin());
			destination[text.size()] = TChar(0);
			return true;
		}

		// Composes the report's text in place, as a C string: no allocation in a crashing process.
		class ReportText
		{
		public:
			void Append(char const* text)
			{
				while (*text != '\0' && HasRoom())
				{
					m_Buffer[m_Size++] = *text++;
				}
			}

			void AppendHex(uint64_t value)
			{
				std::array<char, 17> digits{};
				size_t count = 0;
				do
				{
					digits[count++] = "0123456789abcdef"[value & 0xF];
					value >>= 4;
				} while (value != 0);
				Append("0x");
				while (count > 0 && HasRoom())
				{
					m_Buffer[m_Size++] = digits[--count];
				}
			}

			char const* GetData() const { return m_Buffer.data(); }
			size_t GetSize() const { return m_Size; }

		private:
			// The last character stays the terminating zero.
			bool HasRoom() const { return m_Size + 1 < m_Buffer.size(); }

			std::array<char, 2048> m_Buffer{};
			size_t m_Size = 0;
		};

#if defined(ST_PLATFORM_WINDOWS)
		char const* DescribeException(DWORD code)
		{
			switch (code)
			{
				case EXCEPTION_ACCESS_VIOLATION:
					return "access violation";
				case EXCEPTION_STACK_OVERFLOW:
					return "stack overflow";
				case EXCEPTION_ILLEGAL_INSTRUCTION:
					return "illegal instruction";
				case EXCEPTION_PRIV_INSTRUCTION:
					return "privileged instruction";
				case EXCEPTION_INT_DIVIDE_BY_ZERO:
					return "integer division by zero";
				case EXCEPTION_DATATYPE_MISALIGNMENT:
					return "misaligned data access";
				case EXCEPTION_IN_PAGE_ERROR:
					return "page fault in a mapped file";
				case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
					return "array bounds exceeded";
				default:
					return "unhandled exception";
			}
		}

		void WriteFileText(HANDLE file, char const* text)
		{
			DWORD written = 0;
			::WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
		}

		// The report's first line, also written to standard error with where the report is.
		void WriteReport(ReportText const& cause)
		{
			ReportText report;
			report.Append(s_Data.Name.data());
			report.Append(" crashed: ");
			report.Append(cause.GetData());
			report.Append("\n");
			HANDLE const file =
				::CreateFileW(s_Data.ReportPathWide.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file != INVALID_HANDLE_VALUE)
			{
				WriteFileText(file, report.GetData());
				::CloseHandle(file);
			}
			HANDLE const error = ::GetStdHandle(STD_ERROR_HANDLE);
			if (error != nullptr && error != INVALID_HANDLE_VALUE)
			{
				WriteFileText(error, report.GetData());
				WriteFileText(error, "Crash report: ");
				WriteFileText(error, s_Data.ReportPath.data());
				WriteFileText(error, "\n");
			}
		}

		// After the report, as the riskier step; the report names the minidump once it is written.
		void WriteMinidump(EXCEPTION_POINTERS* exception)
		{
			HANDLE const dump =
				::CreateFileW(s_Data.DumpPathWide.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (dump == INVALID_HANDLE_VALUE)
			{
				return;
			}
			MINIDUMP_EXCEPTION_INFORMATION information{};
			information.ThreadId = ::GetCurrentThreadId();
			information.ExceptionPointers = exception;
			information.ClientPointers = FALSE;
			auto const type =
				static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
			BOOL const written = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), dump, type,
			                                         exception != nullptr ? &information : nullptr, nullptr, nullptr);
			::CloseHandle(dump);
			if (written == FALSE)
			{
				::DeleteFileW(s_Data.DumpPathWide.data());
				return;
			}
			HANDLE const report =
				::CreateFileW(s_Data.ReportPathWide.data(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (report != INVALID_HANDLE_VALUE)
			{
				WriteFileText(report, "Minidump: ");
				WriteFileText(report, s_Data.DumpPath.data());
				WriteFileText(report, "\n");
				::CloseHandle(report);
			}
		}

		LONG WINAPI HandleException(EXCEPTION_POINTERS* exception)
		{
			EXCEPTION_RECORD const& record = *exception->ExceptionRecord;
			// An uncaught C++ exception: std::terminate logs its message and aborts, which reports.
			constexpr DWORD CppExceptionCode = 0xE06D7363;
			if (record.ExceptionCode == CppExceptionCode && s_Data.RuntimeFilter != nullptr)
			{
				return s_Data.RuntimeFilter(exception);
			}
			ReportText cause;
			cause.Append(DescribeException(record.ExceptionCode));
			cause.Append(" (exception ");
			cause.AppendHex(record.ExceptionCode);
			cause.Append(") at ");
			cause.AppendHex(reinterpret_cast<uintptr_t>(record.ExceptionAddress));
			if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
			{
				cause.Append(record.ExceptionInformation[0] == 1 ? ", writing " : ", reading ");
				cause.AppendHex(record.ExceptionInformation[1]);
			}
			WriteReport(cause);
			WriteMinidump(exception);
			// Ends the process with the exception code, without the system's crash dialog: the report is written.
			return EXCEPTION_EXECUTE_HANDLER;
		}

		void ReportAndExit(char const* what)
		{
			ReportText cause;
			cause.Append(what);
			WriteReport(cause);
			WriteMinidump(nullptr);
			// abort()'s exit code.
			::_exit(3);
		}

		void HandleAbort(int)
		{
			ReportAndExit("abort() (a failed assert or an uncaught exception, see the log)");
		}

		void HandleInvalidParameter(wchar_t const*, wchar_t const*, wchar_t const*, unsigned int, uintptr_t)
		{
			ReportAndExit("invalid argument to a C runtime function");
		}

		void HandlePureCall()
		{
			ReportAndExit("pure virtual function call");
		}

		void InstallPlatformHandlers()
		{
			LPTOP_LEVEL_EXCEPTION_FILTER const previous = ::SetUnhandledExceptionFilter(HandleException);
			// Installing again must not make this handler its own previous filter.
			if (previous != HandleException)
			{
				s_Data.RuntimeFilter = previous;
			}
			// Leaves the installing thread enough stack to write the report after a stack overflow.
			ULONG guarantee = 64 * 1024;
			::SetThreadStackGuarantee(&guarantee);
			// abort() reaches the SIGABRT handler without its message box or the system's error reporting.
			_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
			std::signal(SIGABRT, HandleAbort);
			_set_invalid_parameter_handler(HandleInvalidParameter);
			_set_purecall_handler(HandlePureCall);
		}
#else
		void WriteAll(int file, char const* data, size_t size)
		{
			while (size > 0)
			{
				ssize_t const written = ::write(file, data, size);
				if (written <= 0)
				{
					return;
				}
				data += written;
				size -= static_cast<size_t>(written);
			}
		}

		char const* DescribeSignal(int signal)
		{
			switch (signal)
			{
				case SIGSEGV:
					return "SIGSEGV (invalid memory access)";
				case SIGBUS:
					return "SIGBUS (invalid memory access)";
				case SIGILL:
					return "SIGILL (illegal instruction)";
				case SIGFPE:
					return "SIGFPE (arithmetic error)";
				case SIGABRT:
					return "SIGABRT (abort(): a failed assert or an uncaught exception, see the log)";
				default:
					return "fatal signal";
			}
		}

		// Only async-signal-safe calls, apart from backtrace(), which Install already called once (its first call loads code).
		void HandleSignal(int signal, siginfo_t* information, void*)
		{
			ReportText report;
			report.Append(s_Data.Name.data());
			report.Append(" crashed: ");
			report.Append(DescribeSignal(signal));
			if (signal != SIGABRT && information != nullptr)
			{
				report.Append(" at ");
				report.AppendHex(reinterpret_cast<uintptr_t>(information->si_addr));
			}
			report.Append("\n");

			int const file = ::open(s_Data.ReportPath.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
			if (file >= 0)
			{
				WriteAll(file, report.GetData(), report.GetSize());
				char const* const stack = "Stack:\n";
				WriteAll(file, stack, std::strlen(stack));
				std::array<void*, 64> frames{};
				int const count = ::backtrace(frames.data(), static_cast<int>(frames.size()));
				::backtrace_symbols_fd(frames.data(), count, file);
				::close(file);
			}
			WriteAll(STDERR_FILENO, report.GetData(), report.GetSize());
			char const* const location = "Crash report: ";
			WriteAll(STDERR_FILENO, location, std::strlen(location));
			WriteAll(STDERR_FILENO, s_Data.ReportPath.data(), std::strlen(s_Data.ReportPath.data()));
			WriteAll(STDERR_FILENO, "\n", 1);
			// The default action ends the process once the handler returns. SA_RESETHAND is not enough: .NET installs its
			// handlers over this one and calls it directly for crashes outside managed code.
			struct sigaction fallback{};
			fallback.sa_handler = SIG_DFL;
			sigemptyset(&fallback.sa_mask);
			::sigaction(signal, &fallback, nullptr);
			::raise(signal);
		}

		// A stack overflow leaves no stack for the handler: it runs on this one (the installing thread's).
		alignas(16) std::array<std::byte, 64 * 1024> s_SignalStack;

		void InstallPlatformHandlers()
		{
			stack_t stack{};
			stack.ss_sp = s_SignalStack.data();
			stack.ss_size = s_SignalStack.size();
			::sigaltstack(&stack, nullptr);

			std::array<void*, 4> frames{};
			::backtrace(frames.data(), static_cast<int>(frames.size()));

			struct sigaction action{};
			action.sa_sigaction = HandleSignal;
			sigemptyset(&action.sa_mask);
			action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
			for (int const signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
			{
				::sigaction(signal, &action, nullptr);
			}
		}
#endif

		[[noreturn]] void HandleTerminate()
		{
			// Not a signal context: the exception's message tells more than the stack of std::terminate.
			if (Log::IsInitialized())
			{
				if (std::exception_ptr const current = std::current_exception())
				{
					try
					{
						std::rethrow_exception(current);
					}
					catch (std::exception const& exception)
					{
						ST_CORE_CRITICAL("Uncaught exception: {}", exception.what());
					}
					catch (...)
					{
						ST_CORE_CRITICAL("Uncaught exception of an unknown type");
					}
				}
				else
				{
					ST_CORE_CRITICAL("std::terminate was called");
				}
				Log::Flush();
			}
			std::abort();
		}
	}

	void CrashHandler::Install(std::filesystem::path const& directory, std::string_view name)
	{
		std::string const fileName = fmt::format("{}-{}", name, Platform::GetProcessID());
		std::filesystem::path const report = directory / FileSystem::PathFromUtf8(fileName + ".txt");
		CrashHandlerData data;
		bool fits = CopyTerminated(std::string(name.substr(0, data.Name.size() - 1)), data.Name) &&
		            CopyTerminated(FileSystem::PathToUtf8(report), data.ReportPath);
#if defined(ST_PLATFORM_WINDOWS)
		std::filesystem::path const dump = directory / FileSystem::PathFromUtf8(fileName + ".dmp");
		fits = fits && CopyTerminated(report.wstring(), data.ReportPathWide) && CopyTerminated(dump.wstring(), data.DumpPathWide) &&
		       CopyTerminated(FileSystem::PathToUtf8(dump), data.DumpPath);
#endif
		if (!fits)
		{
			ST_CORE_WARN("Crash reports are off: the path '{}' is too long", FileSystem::PathToUtf8(report));
			return;
		}
		if (Result<void> created = FileSystem::CreateDirectories(directory); !created)
		{
			ST_CORE_WARN("Crash reports are off: {}", created.GetError());
			return;
		}

#if defined(ST_PLATFORM_WINDOWS)
		data.RuntimeFilter = s_Data.RuntimeFilter;
#endif
		data.Installed = true;
		s_Data = data;
		InstallPlatformHandlers();
		std::set_terminate(HandleTerminate);
	}

	std::filesystem::path CrashHandler::GetReportPath()
	{
		return s_Data.Installed ? FileSystem::PathFromUtf8(s_Data.ReportPath.data()) : std::filesystem::path();
	}
}
