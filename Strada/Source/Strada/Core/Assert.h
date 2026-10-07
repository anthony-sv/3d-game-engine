#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Log.h"

#include <string>

#if defined(ST_PLATFORM_WINDOWS)
#include <intrin.h>
#define ST_DEBUGBREAK() __debugbreak()
#elif defined(ST_COMPILER_CLANG)
#define ST_DEBUGBREAK() __builtin_debugtrap()
#else
#include <csignal>
#define ST_DEBUGBREAK() std::raise(SIGTRAP)
#endif

namespace Strada
{
	class Assert
	{
	public:
		enum class Source : uint8_t
		{
			Core = 0,
			Client
		};

		// Logs the failure. Returns true when a debugger is attached (the caller then breaks at the call site);
		// otherwise flushes the log and aborts the process.
		static bool ReportFailure(Source source, char const* file, int line, char const* condition, std::string const& message);

		static std::string ComposeMessage() { return {}; }

		template<typename... Args>
		static std::string ComposeMessage(fmt::format_string<Args...> format, Args&&... args)
		{
			return fmt::format(format, std::forward<Args>(args)...);
		}
	};
}

#define ST_INTERNAL_ASSERT_IMPL(source, condition, ...)                                                                                 \
	do                                                                                                                                  \
	{                                                                                                                                   \
		if (!(condition)) [[unlikely]]                                                                                                  \
		{                                                                                                                               \
			if (::Strada::Assert::ReportFailure(source, __FILE__, __LINE__, #condition, ::Strada::Assert::ComposeMessage(__VA_ARGS__))) \
			{                                                                                                                           \
				ST_DEBUGBREAK();                                                                                                        \
			}                                                                                                                           \
		}                                                                                                                               \
	} while (false)

#if defined(ST_ENABLE_ASSERTS)
// Checked in Debug and Release; compiled out (not evaluated) in Dist.
#define ST_CORE_ASSERT(condition, ...) ST_INTERNAL_ASSERT_IMPL(::Strada::Assert::Source::Core, condition, __VA_ARGS__)
#define ST_ASSERT(condition, ...) ST_INTERNAL_ASSERT_IMPL(::Strada::Assert::Source::Client, condition, __VA_ARGS__)
// The condition is always evaluated; it is only checked in Debug and Release.
#define ST_CORE_VERIFY(condition, ...) ST_INTERNAL_ASSERT_IMPL(::Strada::Assert::Source::Core, condition, __VA_ARGS__)
#define ST_VERIFY(condition, ...) ST_INTERNAL_ASSERT_IMPL(::Strada::Assert::Source::Client, condition, __VA_ARGS__)
#else
#define ST_CORE_ASSERT(condition, ...) \
	do                                 \
	{                                  \
		(void)sizeof(condition);       \
	} while (false)
#define ST_ASSERT(condition, ...) ST_CORE_ASSERT(condition)
#define ST_CORE_VERIFY(condition, ...) \
	do                                 \
	{                                  \
		(void)(condition);             \
	} while (false)
#define ST_VERIFY(condition, ...) ST_CORE_VERIFY(condition)
#endif
