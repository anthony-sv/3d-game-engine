#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/Platform.h"

#include <doctest/doctest.h>

#if defined(ST_PLATFORM_WINDOWS)
#include <Windows.h>
#include <objbase.h>

#include <optional>
#include <thread>
#else
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

TEST_CASE("Platform: the main thread stays a single-threaded COM apartment when audio starts")
{
#if defined(ST_PLATFORM_WINDOWS)
	// On fresh threads, so the test process's own apartment does not matter.
	auto const apartmentAfterAudioStarts = [](bool enterApartment)
	{
		APTTYPE type = APTTYPE_CURRENT;
		std::thread thread(
			[&type, enterApartment]
			{
				std::optional<Platform::MainThreadApartment> apartment;
				if (enterApartment)
				{
					apartment.emplace();
				}
				AudioEngineSettings settings;
				settings.Output = AudioOutput::Null;
				AudioEngine::Init(settings);
				APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
				if (FAILED(::CoGetApartmentType(&type, &qualifier)))
				{
					type = APTTYPE_CURRENT;
				}
				AudioEngine::Shutdown();
			});
		thread.join();
		return type;
	};

	// miniaudio makes a thread without an apartment a multithreaded one, where shell dialogs can hang.
	CHECK(apartmentAfterAudioStarts(false) == APTTYPE_MTA);
	// The first single-threaded apartment of a process is its main one.
	APTTYPE const entered = apartmentAfterAudioStarts(true);
	CHECK((entered == APTTYPE_STA || entered == APTTYPE_MAINSTA));
#else
	// Nothing to do elsewhere.
	Platform::MainThreadApartment const apartment;
	CHECK_FALSE(apartment.IsEntered());
#endif
}
