#pragma once

#include "Strada/Core/Base.h"

// spdlog (with its bundled fmt) is part of the logging interface so that format strings are checked at compile time.
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/fmt/std.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	class Log
	{
	public:
		enum class Level : uint8_t
		{
			Trace = 0,
			Info,
			Warn,
			Error,
			Critical
		};

		struct Specification
		{
			// Rotating log file; empty disables file output.
			std::filesystem::path FilePath;
			bool ConsoleOutput = true;
			Level ConsoleLevel = Level::Trace;
			Level FileLevel = Level::Trace;
			// Number of entries kept in memory for the editor console and the automation API.
			size_t BufferCapacity = 10000;
		};

		struct Entry
		{
			// Monotonically increasing across the process lifetime; never reused, even after ClearEntries().
			uint64_t Index = 0;
			Level Severity = Level::Info;
			std::string Logger;
			std::string Message;
			// Milliseconds since the Unix epoch.
			int64_t TimestampMs = 0;
		};

		// Init and Shutdown must be called from the main thread while no other thread is logging.
		static void Init(Specification const& specification = {});
		static void Shutdown();
		static bool IsInitialized() { return s_CoreLogger != nullptr; }

		// Before Init (and after Shutdown) these return spdlog's default logger, so logging is always safe.
		static spdlog::logger& GetCoreLogger() { return s_CoreLogger ? *s_CoreLogger : *spdlog::default_logger_raw(); }
		static spdlog::logger& GetClientLogger() { return s_ClientLogger ? *s_ClientLogger : *spdlog::default_logger_raw(); }
		static spdlog::logger& GetScriptLogger() { return s_ScriptLogger ? *s_ScriptLogger : *spdlog::default_logger_raw(); }

		// Thread-safe access to the in-memory buffer. Returns at most maxCount entries with Index >= firstIndex, oldest first.
		static std::vector<Entry> GetEntries(uint64_t firstIndex = 0, size_t maxCount = std::numeric_limits<size_t>::max());
		// Index that the next logged entry will receive.
		static uint64_t GetNextEntryIndex();
		static void ClearEntries();

		static void Flush();

		static char const* LevelToString(Level level);

	private:
		static Ref<spdlog::logger> s_CoreLogger;
		static Ref<spdlog::logger> s_ClientLogger;
		static Ref<spdlog::logger> s_ScriptLogger;
	};
}

template<>
struct fmt::formatter<glm::vec2> : fmt::formatter<std::string_view>
{
	auto format(glm::vec2 const& value, fmt::format_context& context) const
	{
		return fmt::format_to(context.out(), "({}, {})", value.x, value.y);
	}
};

template<>
struct fmt::formatter<glm::vec3> : fmt::formatter<std::string_view>
{
	auto format(glm::vec3 const& value, fmt::format_context& context) const
	{
		return fmt::format_to(context.out(), "({}, {}, {})", value.x, value.y, value.z);
	}
};

template<>
struct fmt::formatter<glm::vec4> : fmt::formatter<std::string_view>
{
	auto format(glm::vec4 const& value, fmt::format_context& context) const
	{
		return fmt::format_to(context.out(), "({}, {}, {}, {})", value.x, value.y, value.z, value.w);
	}
};

template<>
struct fmt::formatter<glm::quat> : fmt::formatter<std::string_view>
{
	auto format(glm::quat const& value, fmt::format_context& context) const
	{
		return fmt::format_to(context.out(), "(x: {}, y: {}, z: {}, w: {})", value.x, value.y, value.z, value.w);
	}
};

// Engine logging.
#define ST_CORE_TRACE(...) ::Strada::Log::GetCoreLogger().trace(__VA_ARGS__)
#define ST_CORE_INFO(...) ::Strada::Log::GetCoreLogger().info(__VA_ARGS__)
#define ST_CORE_WARN(...) ::Strada::Log::GetCoreLogger().warn(__VA_ARGS__)
#define ST_CORE_ERROR(...) ::Strada::Log::GetCoreLogger().error(__VA_ARGS__)
#define ST_CORE_CRITICAL(...) ::Strada::Log::GetCoreLogger().critical(__VA_ARGS__)

// Application (editor/runtime) logging.
#define ST_TRACE(...) ::Strada::Log::GetClientLogger().trace(__VA_ARGS__)
#define ST_INFO(...) ::Strada::Log::GetClientLogger().info(__VA_ARGS__)
#define ST_WARN(...) ::Strada::Log::GetClientLogger().warn(__VA_ARGS__)
#define ST_ERROR(...) ::Strada::Log::GetClientLogger().error(__VA_ARGS__)
#define ST_CRITICAL(...) ::Strada::Log::GetClientLogger().critical(__VA_ARGS__)
