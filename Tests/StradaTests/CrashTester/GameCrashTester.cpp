#include "Strada/Core/EntryPoint.h"

#include <cstdlib>

// A program on the engine's entry point that aborts as its application would be made: the entry point has chosen the
// folders of its log and crash reports from the game file beside the executable by then, as it does for an exported
// game. The crash handler tests (Source/Core/CrashHandlerTests.cpp) copy it beside a game file of their own and run it.
namespace Strada
{
	Scope<Application> CreateApplication(ApplicationCommandLineArgs /*args*/, int& /*exitCode*/)
	{
		std::abort();
	}
}
