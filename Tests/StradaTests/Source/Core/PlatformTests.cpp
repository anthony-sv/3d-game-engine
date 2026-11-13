#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

namespace
{
	// The folder user-dirs.dirs text gives XDG_DOCUMENTS_DIR for the home /home/ana, with forward slashes; "" for none.
	std::string ParseDocuments(std::string_view text, std::string_view home = "/home/ana")
	{
		std::optional<std::filesystem::path> const directory =
			Platform::ParseUserDirectory(text, "XDG_DOCUMENTS_DIR", FileSystem::PathFromUtf8(home));
		return directory ? FileSystem::PathToUtf8(*directory) : std::string();
	}
}

TEST_CASE("Platform: user-dirs.dirs gives the Documents folder under the home directory or at an absolute path")
{
	std::string_view const file = "# This file is written by xdg-user-dirs-update\n"
								  "# If you want to change or add directories, just edit the line you're\n"
								  "XDG_DESKTOP_DIR=\"$HOME/Schreibtisch\"\n"
								  "XDG_DOCUMENTS_DIR=\"$HOME/Dokumente\"\n"
								  "XDG_MUSIC_DIR=\"$HOME/Musik\"\n";
	CHECK(ParseDocuments(file) == "/home/ana/Dokumente");
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=\"/data/papers\"") == "/data/papers");
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=\"/data/papers/\"\r\n") == "/data/papers");
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=\"$HOME\"") == "/home/ana");
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=\"$HOME/\"") == "/home/ana");
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=\"$HOME//Docs/\"") == "/home/ana/Docs");
	CHECK(ParseDocuments("  \tXDG_DOCUMENTS_DIR = \"$HOME/My Documents\"  ") == "/home/ana/My Documents");
	// Backslashes escape the next character, as in the shell that sources the file.
	CHECK(ParseDocuments(R"(XDG_DOCUMENTS_DIR="$HOME/The \"Best\" \$Files")") == "/home/ana/The \"Best\" $Files");
	// The last valid line wins; a line that is not valid changes nothing.
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=\"$HOME/One\"\nXDG_DOCUMENTS_DIR=\"/two\"\nXDG_DOCUMENTS_DIR=Three\n") == "/two");
	// An absolute folder needs no home directory; one under it does.
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=\"/data/papers\"", "") == "/data/papers");
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=\"$HOME/Dokumente\"", "").empty());
}

TEST_CASE("Platform: user-dirs.dirs without a valid line of the key gives nothing")
{
	CHECK(ParseDocuments("").empty());
	CHECK(ParseDocuments("XDG_MUSIC_DIR=\"$HOME/Music\"\n").empty());
	CHECK(ParseDocuments("# XDG_DOCUMENTS_DIR=\"$HOME/Documents\"\n").empty());
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIRECTORY=\"$HOME/Documents\"").empty());
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=$HOME/Documents").empty());
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=\"Documents\"").empty());
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=\"$HOMEDocuments\"").empty());
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=\"$HOME/Documents").empty());
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR=\"$HOME/Documents\" extra").empty());
	CHECK(ParseDocuments("XDG_DOCUMENTS_DIR \"$HOME/Documents\"").empty());
	// Control characters and bytes that are not text.
	std::string const control = std::string("XDG_DOCUMENTS_DIR=\"/a") + '\x01' + "b\"";
	CHECK(ParseDocuments(control).empty());
	std::string const garbage = std::string(1, '\0') + "\xff\x7f=\"\"\n\x1b[0m";
	CHECK(ParseDocuments(garbage).empty());
	CHECK_FALSE(Platform::ParseUserDirectory("=\"/data\"", "", "/home/ana").has_value());
}

TEST_CASE("Platform: the Documents folder, when it is known, is an absolute path")
{
	// Whether it exists depends on the machine; the editor creates nothing there until a project is created in it.
	if (std::optional<std::filesystem::path> const documents = Platform::GetDocumentsDirectory())
	{
		CHECK(documents->is_absolute());
		CHECK_FALSE(documents->empty());
	}
}
