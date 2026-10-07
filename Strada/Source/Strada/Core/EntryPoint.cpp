#include "stpch.h"
#include "Strada/Core/Application.h"

#include "Strada/Core/FileSystem.h"

#if defined(ST_PLATFORM_WINDOWS)
#include <Windows.h>
#endif

// Kept in its own translation unit: only executables that use EntryPoint.h link it (and must define CreateApplication).

namespace Strada
{
	int ApplicationMain(int argc, char** argv)
	{
#if defined(ST_PLATFORM_WINDOWS) && !defined(ST_DIST)
		::SetConsoleOutputCP(CP_UTF8);
#endif

		Log::Specification logSpecification;
		std::filesystem::path const executablePath = FileSystem::GetExecutablePath();
		std::string const logName = executablePath.empty() ? std::string("Strada") : FileSystem::PathToUtf8(executablePath.stem());
		logSpecification.FilePath = FileSystem::GetUserDataDirectory() / "Logs" / (logName + ".log");
#if defined(ST_DIST)
		logSpecification.ConsoleOutput = false;
		logSpecification.FileLevel = Log::Level::Warn;
#endif
		Log::Init(logSpecification);

		int exitCode = 1;
		{
			Scope<Application> application = CreateApplication({argc, argv});
			if (application)
			{
				exitCode = application->Run();
			}
			else
			{
				ST_CORE_CRITICAL("CreateApplication returned no application");
			}
		}

		Log::Shutdown();
		return exitCode;
	}
}
