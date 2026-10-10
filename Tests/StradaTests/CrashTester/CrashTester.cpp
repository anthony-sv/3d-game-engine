#include "Strada/Core/CrashHandler.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string_view>

// Crashes on purpose, the way its first argument names, with the crash handler writing to the directory its second
// argument names and the log to Tester.log there. The crash handler tests (Source/Core/CrashHandlerTests.cpp) run it.
int main(int argc, char** argv)
{
	if (argc != 3)
	{
		std::fputs("usage: StradaCrashTester <access-violation|abort|exception> <directory>\n", stderr);
		return 2;
	}
	std::string_view const mode = argv[1];
	std::filesystem::path const directory = Strada::FileSystem::PathFromUtf8(argv[2]);
	Strada::LogSpecification log;
	log.FilePath = directory / "Tester.log";
	log.ConsoleOutput = false;
	Strada::Log::Init(log);
	Strada::CrashHandler::Install(directory, "StradaCrashTester");

	if (mode == "access-violation")
	{
		// An address in the never-mapped first page that the compiler cannot see coming: the argument's length.
		auto* const address = reinterpret_cast<int volatile*>(static_cast<std::uintptr_t>(std::strlen(argv[1])));
		*address = 1;
	}
	else if (mode == "abort")
	{
		std::abort();
	}
	else if (mode == "exception")
	{
		throw std::runtime_error("deliberately uncaught");
	}
	return 1;
}
