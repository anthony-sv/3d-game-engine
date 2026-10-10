#include "stpch.h"
#include "Strada/Core/Application.h"

#include "Strada/Core/CrashHandler.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"

#include <string>
#include <vector>

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

		LogSpecification logSpecification;
		std::filesystem::path const executablePath = FileSystem::GetExecutablePath();
		std::string const logName = executablePath.empty() ? std::string("Strada") : FileSystem::PathToUtf8(executablePath.stem());
		logSpecification.FilePath = FileSystem::GetUserDataDirectory() / "Logs" / (logName + ".log");
#if defined(ST_DIST)
		logSpecification.ConsoleOutput = false;
		logSpecification.FileLevel = LogLevel::Warn;
#endif
		Log::Init(logSpecification);
		CrashHandler::Install(FileSystem::GetUserDataDirectory() / "Crashes", logName);

		// UTF-8 arguments (non-ASCII project paths on Windows), alive until the application is destroyed.
		std::vector<std::string> arguments = Platform::ReadCommandLineArguments(argc, argv);
		std::vector<char*> argumentPointers;
		argumentPointers.reserve(arguments.size() + 1);
		for (std::string& argument : arguments)
		{
			argumentPointers.push_back(argument.data());
		}
		argumentPointers.push_back(nullptr);

		int exitCode = 0;
		{
			Scope<Application> application = CreateApplication({static_cast<int>(arguments.size()), argumentPointers.data()}, exitCode);
			if (application)
			{
				exitCode = application->Run();
			}
		}

		Log::Shutdown();
		return exitCode;
	}
}
