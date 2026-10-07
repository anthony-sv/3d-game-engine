#include "stpch.h"
#include "Strada/Core/Assert.h"

#include "Strada/Core/Platform.h"

#include <cstdlib>

namespace Strada
{
	bool Assert::ReportFailure(Source source, char const* file, int line, char const* condition, std::string const& message)
	{
		spdlog::logger& logger = source == Source::Core ? Log::GetCoreLogger() : Log::GetClientLogger();
		if (message.empty())
		{
			logger.critical("Assertion failed: {} ({}:{})", condition, file, line);
		}
		else
		{
			logger.critical("Assertion failed: {} — {} ({}:{})", condition, message, file, line);
		}
		Log::Flush();

		if (Platform::IsDebuggerAttached())
		{
			return true;
		}

		std::abort();
	}
}
