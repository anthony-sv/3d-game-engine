#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "Strada/Core/Log.h"

int main(int argc, char** argv)
{
	// Keep the console readable: only warnings and errors from the engine are printed while tests run.
	Strada::Log::Specification logSpecification;
	logSpecification.ConsoleLevel = Strada::Log::Level::Warn;
	Strada::Log::Init(logSpecification);

	doctest::Context context;
	context.applyCommandLine(argc, argv);
	int const result = context.run();

	Strada::Log::Shutdown();
	return result;
}
