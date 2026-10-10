#include "Strada/Core/Platform.h"

#include <doctest/doctest.h>

#if !defined(ST_PLATFORM_WINDOWS)
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#endif

using namespace Strada;

TEST_CASE("Platform: the current process runs, process 0 does not")
{
	CHECK(Platform::GetProcessID() != 0);
	CHECK(Platform::IsProcessRunning(Platform::GetProcessID()));
	CHECK_FALSE(Platform::IsProcessRunning(0));
}

TEST_CASE("Platform: writes to closed pipes fail instead of ending the process")
{
#if defined(ST_PLATFORM_WINDOWS)
	// Windows has no SIGPIPE: there is nothing to change.
	Platform::IgnoreBrokenPipeSignal();
#else
	struct sigaction previous = {};
	REQUIRE(::sigaction(SIGPIPE, nullptr, &previous) == 0);
	Platform::IgnoreBrokenPipeSignal();

	int pipeEnds[2] = {-1, -1};
	REQUIRE(::pipe(pipeEnds) == 0);
	::close(pipeEnds[0]);
	char const byte = 0;
	errno = 0;
	CHECK(::write(pipeEnds[1], &byte, 1) == -1);
	CHECK(errno == EPIPE);
	::close(pipeEnds[1]);

	::sigaction(SIGPIPE, &previous, nullptr);
#endif
}
