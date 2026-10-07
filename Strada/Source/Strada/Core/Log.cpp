#include "stpch.h"
#include "Strada/Core/Log.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <system_error>

namespace Strada
{
	Ref<spdlog::logger> Log::s_CoreLogger;
	Ref<spdlog::logger> Log::s_ClientLogger;
	Ref<spdlog::logger> Log::s_ScriptLogger;

	namespace
	{
		constexpr size_t MaxLogFileSize = 5 * 1024 * 1024;
		constexpr size_t MaxLogFiles = 3;

		spdlog::level::level_enum ToSpdlogLevel(Log::Level level)
		{
			switch (level)
			{
				case Log::Level::Trace:
					return spdlog::level::trace;
				case Log::Level::Info:
					return spdlog::level::info;
				case Log::Level::Warn:
					return spdlog::level::warn;
				case Log::Level::Error:
					return spdlog::level::err;
				case Log::Level::Critical:
					return spdlog::level::critical;
			}
			return spdlog::level::trace;
		}

		Log::Level FromSpdlogLevel(spdlog::level::level_enum level)
		{
			switch (level)
			{
				case spdlog::level::trace:
				case spdlog::level::debug:
					return Log::Level::Trace;
				case spdlog::level::info:
					return Log::Level::Info;
				case spdlog::level::warn:
					return Log::Level::Warn;
				case spdlog::level::err:
					return Log::Level::Error;
				case spdlog::level::critical:
				case spdlog::level::off:
				case spdlog::level::n_levels:
					return Log::Level::Critical;
			}
			return Log::Level::Critical;
		}

		// Process-wide entry counter so indices are never reused, even across Init/Shutdown cycles.
		// Only touched under the buffer sink's mutex (or while no logger exists).
		uint64_t s_NextEntryIndex = 0;

		// Keeps the most recent log entries in memory for the editor console and the automation API.
		class LogBufferSink final : public spdlog::sinks::base_sink<std::mutex>
		{
		public:
			explicit LogBufferSink(size_t capacity)
				: m_Capacity(capacity > 0 ? capacity : 1)
			{
			}

			std::vector<Log::Entry> GetEntries(uint64_t firstIndex, size_t maxCount)
			{
				std::lock_guard<std::mutex> lock(mutex_);

				std::vector<Log::Entry> result;
				if (m_Entries.empty() || maxCount == 0)
				{
					return result;
				}

				uint64_t const oldestIndex = m_Entries.front().Index;
				size_t const start = firstIndex <= oldestIndex ? 0 : static_cast<size_t>(firstIndex - oldestIndex);
				if (start >= m_Entries.size())
				{
					return result;
				}

				size_t const count = std::min(maxCount, m_Entries.size() - start);
				result.reserve(count);
				for (size_t i = start; i < start + count; i++)
				{
					result.push_back(m_Entries[i]);
				}
				return result;
			}

			uint64_t GetNextIndex()
			{
				std::lock_guard<std::mutex> lock(mutex_);
				return s_NextEntryIndex;
			}

			void Clear()
			{
				std::lock_guard<std::mutex> lock(mutex_);
				m_Entries.clear();
			}

		protected:
			void sink_it_(spdlog::details::log_msg const& message) override
			{
				Log::Entry entry;
				entry.Index = s_NextEntryIndex++;
				entry.Severity = FromSpdlogLevel(message.level);
				entry.Logger.assign(message.logger_name.data(), message.logger_name.size());
				entry.Message.assign(message.payload.data(), message.payload.size());
				entry.TimestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(message.time.time_since_epoch()).count();

				m_Entries.push_back(std::move(entry));
				while (m_Entries.size() > m_Capacity)
				{
					m_Entries.pop_front();
				}
			}

			void flush_() override {}

		private:
			std::deque<Log::Entry> m_Entries;
			size_t m_Capacity;
		};

		Ref<LogBufferSink> s_BufferSink;
	}

	void Log::Init(Specification const& specification)
	{
		if (IsInitialized())
		{
			GetCoreLogger().warn("Log::Init called while the log is already initialized; ignoring.");
			return;
		}

		std::vector<spdlog::sink_ptr> sinks;

		if (specification.ConsoleOutput)
		{
			auto consoleSink = CreateRef<spdlog::sinks::stdout_color_sink_mt>();
			consoleSink->set_pattern("%^[%T] %n: %v%$");
			consoleSink->set_level(ToSpdlogLevel(specification.ConsoleLevel));
			sinks.push_back(consoleSink);
		}

		std::string fileSinkError;
		if (!specification.FilePath.empty())
		{
			std::error_code errorCode;
			std::filesystem::create_directories(specification.FilePath.parent_path(), errorCode);

			try
			{
#if defined(SPDLOG_WCHAR_FILENAMES)
				spdlog::filename_t const fileName = specification.FilePath.wstring();
#else
				spdlog::filename_t const fileName = specification.FilePath.string();
#endif
				auto fileSink = CreateRef<spdlog::sinks::rotating_file_sink_mt>(fileName, MaxLogFileSize, MaxLogFiles);
				fileSink->set_pattern("[%Y-%m-%d %T.%e] [%l] %n: %v");
				fileSink->set_level(ToSpdlogLevel(specification.FileLevel));
				sinks.push_back(fileSink);
			}
			catch (spdlog::spdlog_ex const& exception)
			{
				fileSinkError = exception.what();
			}
		}

		s_BufferSink = CreateRef<LogBufferSink>(specification.BufferCapacity);
		s_BufferSink->set_level(spdlog::level::trace);
		sinks.push_back(s_BufferSink);

		auto const createLogger = [&sinks](char const* name)
		{
			auto logger = CreateRef<spdlog::logger>(name, sinks.begin(), sinks.end());
			logger->set_level(spdlog::level::trace);
			logger->flush_on(spdlog::level::warn);
			return logger;
		};

		s_CoreLogger = createLogger("STRADA");
		s_ClientLogger = createLogger("APP");
		s_ScriptLogger = createLogger("SCRIPT");

		if (!fileSinkError.empty())
		{
			s_CoreLogger->error("Failed to open log file '{}': {}", specification.FilePath, fileSinkError);
		}
	}

	void Log::Shutdown()
	{
		if (!IsInitialized())
		{
			return;
		}

		Flush();
		s_ScriptLogger.reset();
		s_ClientLogger.reset();
		s_CoreLogger.reset();
		s_BufferSink.reset();
	}

	std::vector<Log::Entry> Log::GetEntries(uint64_t firstIndex, size_t maxCount)
	{
		if (!s_BufferSink)
		{
			return {};
		}
		return s_BufferSink->GetEntries(firstIndex, maxCount);
	}

	uint64_t Log::GetNextEntryIndex()
	{
		if (!s_BufferSink)
		{
			return s_NextEntryIndex;
		}
		return s_BufferSink->GetNextIndex();
	}

	void Log::ClearEntries()
	{
		if (s_BufferSink)
		{
			s_BufferSink->Clear();
		}
	}

	void Log::Flush()
	{
		if (s_CoreLogger)
		{
			// All loggers share the same sinks, so flushing one flushes every sink.
			s_CoreLogger->flush();
		}
	}

	char const* Log::LevelToString(Level level)
	{
		switch (level)
		{
			case Level::Trace:
				return "Trace";
			case Level::Info:
				return "Info";
			case Level::Warn:
				return "Warn";
			case Level::Error:
				return "Error";
			case Level::Critical:
				return "Critical";
		}
		return "Unknown";
	}
}
