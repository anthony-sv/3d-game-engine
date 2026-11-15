#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Platform.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

using namespace Strada;

namespace
{
	constexpr UserDataPlatform OnWindows = UserDataPlatform::Windows;
	constexpr UserDataPlatform OnMacOS = UserDataPlatform::MacOS;
	constexpr UserDataPlatform OnLinux = UserDataPlatform::Linux;

	// The game's folder below a root, with forward slashes. Roots are written with forward slashes, which every platform
	// reads as separators, so each platform's names are checked on every platform.
	std::string GameFolder(UserDataPlatform platform, std::string_view company, std::string_view product, std::string_view root = "/root")
	{
		GameIdentity const identity{std::string(company), std::string(product)};
		return FileSystem::PathToUtf8(UserDataLocations::MakeGameDirectory(FileSystem::PathFromUtf8(root), identity, platform));
	}

	// The per-user root an environment gives, with forward slashes; "" for none.
	std::string Root(UserDataPlatform platform, std::map<std::string, std::string> const& environment)
	{
		std::optional<std::filesystem::path> const root =
			UserDataLocations::FindRoot(platform,
		                                [&environment](std::string const& name) -> std::optional<std::string>
		                                {
											auto const found = environment.find(name);
											return found != environment.end() ? std::optional<std::string>(found->second) : std::nullopt;
										});
		return root ? FileSystem::PathToUtf8(*root) : std::string();
	}

	// The names below the root, which must be one (no company) or two (a company, then the product) on Windows and macOS
	// and one on Linux, each a portable name with an ASCII letter or digit that is not the engine's folder, of at most
	// MaxFolderNameLength bytes (twice that and the '-' for Linux's joined name).
	void CheckBelowRoot(UserDataPlatform platform, std::string_view company, std::string_view product)
	{
		CAPTURE(company);
		CAPTURE(product);
		std::filesystem::path const root = FileSystem::PathFromUtf8("/root");
		std::filesystem::path const folder =
			UserDataLocations::MakeGameDirectory(root, GameIdentity{std::string(company), std::string(product)}, platform);
		std::filesystem::path const relative = folder.lexically_relative(root);
		CAPTURE(FileSystem::PathToUtf8(relative));
		size_t names = 0;
		for (std::filesystem::path const& part : relative)
		{
			std::string const name = FileSystem::PathToUtf8(part);
			CHECK(GameIdentity::HasLetterOrDigit(name));
			CHECK(name.size() <=
			      (platform == OnLinux ? UserDataLocations::MaxFolderNameLength * 2 + 1 : UserDataLocations::MaxFolderNameLength));
			CHECK(name == FileSystem::MakePortableFileName(name));
			names++;
		}
		CHECK(names >= 1);
		CHECK(names <= (platform == OnLinux ? 1u : 2u));
		std::string const first = FileSystem::PathToUtf8(*relative.begin());
		CHECK(first != "Strada");
		CHECK(first != "strada");
		CHECK(first != "STRADA");
	}
}

TEST_CASE("GameData: a game's folder is <root>/<Company>/<Product> on Windows and macOS, <root>/<company>-<product> on Linux")
{
	CHECK(GameFolder(OnWindows, "Acme Games", "Moto Racer", "C:/Users/ana/AppData/Roaming") ==
	      "C:/Users/ana/AppData/Roaming/Acme Games/Moto Racer");
	CHECK(GameFolder(OnMacOS, "Acme Games", "Moto Racer", "/Users/ana/Library/Application Support") ==
	      "/Users/ana/Library/Application Support/Acme Games/Moto Racer");
	CHECK(GameFolder(OnLinux, "Acme Games", "Moto Racer", "/home/ana/.local/share") == "/home/ana/.local/share/acme games-moto racer");

	// Without a company there is no company level.
	CHECK(GameFolder(OnWindows, "", "Moto") == "/root/Moto");
	CHECK(GameFolder(OnMacOS, "", "Moto") == "/root/Moto");
	CHECK(GameFolder(OnLinux, "", "Moto") == "/root/moto");
	// A company with no ASCII letter or digit left is none; such a product is "Game".
	CHECK(GameFolder(OnWindows, "   ", "Moto") == "/root/Moto");
	CHECK(GameFolder(OnMacOS, "--", "Moto") == "/root/Moto");
	CHECK(GameFolder(OnLinux, "   ", "Moto") == "/root/moto");
	CHECK(GameFolder(OnWindows, "Acme", "") == "/root/Acme/Game");
	CHECK(GameFolder(OnMacOS, "Acme", "  ") == "/root/Acme/Game");
	CHECK(GameFolder(OnLinux, "Acme", "") == "/root/acme-game");
	CHECK(GameFolder(OnWindows, "", "") == "/root/Game");
	CHECK(GameFolder(OnLinux, "", "") == "/root/game");
	// Text that is not ASCII has nothing portable either, so products of equal byte length do not silently share a folder
	// of underscores.
	CHECK(GameFolder(OnWindows, "\xce\xa9", "\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0") == "/root/Game");
	CHECK(GameFolder(OnMacOS, "Acme", "___") == "/root/Acme/Game");
	CHECK(GameFolder(OnLinux, "\xe6\x97\xa5", "Moto") == "/root/moto");
}

TEST_CASE("GameData: an identity's product without an ASCII letter or digit is the project's name, and such a company is none")
{
	CHECK(GameIdentity::Resolve("", "", "Moto") == GameIdentity{"", "Moto"});
	CHECK(GameIdentity::Resolve("Acme", "", "Moto") == GameIdentity{"Acme", "Moto"});
	CHECK(GameIdentity::Resolve("Acme", "Moto Racer", "Moto") == GameIdentity{"Acme", "Moto Racer"});
	CHECK(GameIdentity::Resolve("", "Moto Racer", "") == GameIdentity{"", "Moto Racer"});
	// Spaces, punctuation and text that is not ASCII name nothing in a folder or a bundle identifier.
	CHECK(GameIdentity::Resolve("Acme", " ", "Moto") == GameIdentity{"Acme", "Moto"});
	CHECK(GameIdentity::Resolve("Acme", "\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0", "Moto") == GameIdentity{"Acme", "Moto"});
	CHECK(GameIdentity::Resolve("  ", "Moto", "Game") == GameIdentity{"", "Moto"});
	CHECK(GameIdentity::Resolve("..", "Moto", "Game") == GameIdentity{"", "Moto"});
	// The name is kept as set, whatever it holds: the folder and the bundle identifier each fall back from it.
	CHECK(GameIdentity::Resolve("", "-", "\xc3\xa9") == GameIdentity{"", "\xc3\xa9"});
	// Text keeps its other characters once it holds one letter or digit.
	CHECK(GameIdentity::Resolve(" Caf\xc3\xa9 ", " 2 ", "Moto") == GameIdentity{" Caf\xc3\xa9 ", " 2 "});

	CHECK(GameIdentity::HasLetterOrDigit("a"));
	CHECK(GameIdentity::HasLetterOrDigit("--7--"));
	CHECK_FALSE(GameIdentity::HasLetterOrDigit(""));
	CHECK_FALSE(GameIdentity::HasLetterOrDigit(" _-.~"));
	CHECK_FALSE(GameIdentity::HasLetterOrDigit("\xc3\xa9\xff"));
}

TEST_CASE("GameData: hostile names make portable folders below the root, kept apart from the engine's own folder")
{
	// Paths, separators and parent folders stay one name each.
	CHECK(GameFolder(OnWindows, "../..", "../../Windows/System32") == "/root/______Windows_System32");
	CHECK(GameFolder(OnMacOS, "/etc", "C:\\Games") == "/root/_etc/C__Games");
	CHECK(GameFolder(OnLinux, "..", ".") == "/root/game");
	CHECK(GameFolder(OnLinux, "..a", ".b") == "/root/__a-_b");
	// Device names Windows reserves, and what lower-casing does to them on Linux.
	CHECK(GameFolder(OnWindows, "CON", "nul") == "/root/CON_/nul_");
	CHECK(GameFolder(OnLinux, "", "COM1") == "/root/com1_");
	// The engine's folder in any letter case, as a company or as a game without one; a game named Strada inside a company
	// is no clash.
	CHECK(GameFolder(OnWindows, "", "Strada") == "/root/Strada_");
	CHECK(GameFolder(OnMacOS, "STRADA", "Moto") == "/root/STRADA_/Moto");
	CHECK(GameFolder(OnLinux, "", "Strada") == "/root/strada_");
	CHECK(GameFolder(OnWindows, "Acme", "Strada") == "/root/Acme/Strada");
	CHECK(GameFolder(OnLinux, "strada", "moto") == "/root/strada-moto");
	// Text that is not ASCII, control characters and quotes become '_', surrounding spaces go.
	CHECK(GameFolder(OnWindows, "  Caf\xc3\xa9  ", "\"Moto\"\t\n") == "/root/Caf__/_Moto___");
	// Long names are cut to MaxFolderNameLength bytes, and spaces the cut leaves at the end go too.
	// Built by appending: GCC 13 at -O2 falsely reports -Warray-bounds on operator+ with a temporary string of known length.
	std::string longName(63, 'a');
	longName.append(" b").append(500, 'c');
	std::string const cut = std::string(63, 'a');
	CHECK(GameFolder(OnWindows, longName, longName) == "/root/" + cut + "/" + cut);
	CHECK(GameFolder(OnLinux, longName, std::string(200, 'Z')) == "/root/" + cut + "-" + std::string(64, 'z'));
	// A cut that leaves a reserved name marks it.
	CHECK(GameFolder(OnWindows, "", "AUX" + std::string(70, ' ') + "x") == "/root/AUX_");

	for (UserDataPlatform const platform : {OnWindows, OnMacOS, OnLinux})
	{
		CAPTURE(static_cast<int>(platform));
		for (char const* company : {"", " ", "..", "Strada", "strada ", "a/b", "\\\\server\\share", "C:", "CON"})
		{
			for (char const* product : {"", "  ", ".", "..", "Strada", "~", "*?<>|", "LPT9", "game\x01\x7f"})
			{
				CheckBelowRoot(platform, company, product);
			}
		}
		CheckBelowRoot(platform, std::string(4096, '/'), std::string(4096, '.'));
		CheckBelowRoot(platform, std::string("\0\0", 2), std::string("\xff\xfe", 2));
	}
}

TEST_CASE("GameData: the per-user root of each platform comes from its environment, ignoring empty and relative values")
{
	CHECK(Root(OnWindows, {{"APPDATA", "C:/Users/ana/AppData/Roaming"}, {"USERPROFILE", "C:/Users/bo"}}) == "C:/Users/ana/AppData/Roaming");
	CHECK(Root(OnWindows, {{"APPDATA", "C:\\Users\\ana\\AppData\\Roaming"}}) != "");
	CHECK(Root(OnWindows, {{"APPDATA", "\\\\server\\profiles\\ana"}}) != "");
	CHECK(Root(OnWindows, {{"USERPROFILE", "C:/Users/ana"}}) == "C:/Users/ana/AppData/Roaming");
	CHECK(Root(OnWindows, {{"APPDATA", ""}, {"USERPROFILE", "C:/Users/ana"}}) == "C:/Users/ana/AppData/Roaming");
	CHECK(Root(OnWindows, {{"APPDATA", "Roaming"}, {"USERPROFILE", "C:/Users/ana"}}) == "C:/Users/ana/AppData/Roaming");
	CHECK(Root(OnWindows, {{"APPDATA", "C:Roaming"}}) == "");
	CHECK(Root(OnWindows, {{"APPDATA", "/home/ana"}}) == "");
	CHECK(Root(OnWindows, {{"HOME", "C:/Users/ana"}}) == "");

	CHECK(Root(OnMacOS, {{"HOME", "/Users/ana"}}) == "/Users/ana/Library/Application Support");
	CHECK(Root(OnMacOS, {{"HOME", ""}}) == "");
	CHECK(Root(OnMacOS, {{"HOME", "Users/ana"}}) == "");
	CHECK(Root(OnMacOS, {{"XDG_DATA_HOME", "/data"}, {"APPDATA", "C:/Roaming"}}) == "");

	CHECK(Root(OnLinux, {{"XDG_DATA_HOME", "/data/ana"}, {"HOME", "/home/ana"}}) == "/data/ana");
	CHECK(Root(OnLinux, {{"HOME", "/home/ana"}}) == "/home/ana/.local/share");
	CHECK(Root(OnLinux, {{"XDG_DATA_HOME", ""}, {"HOME", "/home/ana"}}) == "/home/ana/.local/share");
	// The XDG base directory specification ignores relative paths.
	CHECK(Root(OnLinux, {{"XDG_DATA_HOME", "data"}, {"HOME", "/home/ana"}}) == "/home/ana/.local/share");
	CHECK(Root(OnLinux, {{"XDG_DATA_HOME", "C:/data"}}) == "");
	CHECK(Root(OnLinux, {}) == "");

	// The engine's folder below a root.
	CHECK(FileSystem::PathToUtf8(UserDataLocations::MakeEngineDirectory("/root", OnWindows)) == "/root/Strada");
	CHECK(FileSystem::PathToUtf8(UserDataLocations::MakeEngineDirectory("/root", OnMacOS)) == "/root/Strada");
	CHECK(FileSystem::PathToUtf8(UserDataLocations::MakeEngineDirectory("/root", OnLinux)) == "/root/strada");
}

TEST_CASE("GameData: without a per-user root the engine's folder and the games' are apart beside the executable")
{
	std::filesystem::path const executableDirectory = FileSystem::PathFromUtf8("/opt/Game");
	std::filesystem::path const engine = UserDataLocations::MakeFallbackEngineDirectory(executableDirectory);
	std::filesystem::path const games = UserDataLocations::MakeFallbackGameRoot(executableDirectory);
	CHECK(FileSystem::PathToUtf8(engine) == "/opt/Game/UserData");
	CHECK(FileSystem::PathToUtf8(games) == "/opt/Game/GameData");
	// The engine's own folders, and the fallback folders' names, are names a game may have.
	for (UserDataPlatform const platform : {OnWindows, OnMacOS, OnLinux})
	{
		for (char const* company : {"", "Logs", "UserData", "GameData", "Strada"})
		{
			for (char const* product : {"Logs", "Crashes", "Editor", "UserData", "GameData", "Strada", ".."})
			{
				CAPTURE(static_cast<int>(platform));
				CAPTURE(company);
				CAPTURE(product);
				std::filesystem::path const game =
					UserDataLocations::MakeGameDirectory(games, GameIdentity{std::string(company), std::string(product)}, platform);
				CHECK(FileSystem::IsInside(game, games));
				CHECK(game != engine);
				CHECK_FALSE(FileSystem::IsInside(game, engine));
				CHECK_FALSE(FileSystem::IsInside(engine, game));
			}
		}
	}
}

TEST_CASE("GameData: on this machine the engine's folder and a game's share the per-user root")
{
	constexpr UserDataPlatform host = UserDataLocations::GetHostPlatform();
	std::optional<std::filesystem::path> const root = UserDataLocations::FindRoot(host, &Platform::ReadEnvironmentVariable);
	std::filesystem::path const executableDirectory = FileSystem::GetExecutableDirectory();
	std::filesystem::path const base = root ? *root : UserDataLocations::MakeFallbackGameRoot(executableDirectory);
	CHECK(FileSystem::GetUserDataDirectory() == (root ? UserDataLocations::MakeEngineDirectory(*root, host)
	                                                  : UserDataLocations::MakeFallbackEngineDirectory(executableDirectory)));
	for (GameIdentity const& identity : {GameIdentity{"Acme", "Moto"}, GameIdentity{"", "Logs"}, GameIdentity{"", "Editor"}})
	{
		CAPTURE(identity.Product);
		std::filesystem::path const game = GameData::GetUserDirectory(identity);
		CHECK(game == UserDataLocations::MakeGameDirectory(base, identity, host));
		CHECK(game.is_absolute());
		CHECK(FileSystem::IsInside(game, base));
		// Beside the engine's folder, never in it.
		CHECK_FALSE(FileSystem::IsInside(game, FileSystem::GetUserDataDirectory()));
	}
}

TEST_CASE("GameData: the service is empty until a host fills it, and each scope gives back what it held")
{
	REQUIRE_FALSE(GameData::GetDirectory().has_value());
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const kept = temporary.GetPath() / "Kept";
	{
		GameDataScope const scope(kept, GameDataLifetime::Kept);
		CHECK(GameData::GetDirectory() == kept);
		CHECK_FALSE(GameData::IsTemporary());
		{
			// A scope inside another, as a test that plays inside a filled service does.
			GameDataScope const inner(temporary.GetPath() / "Inner", GameDataLifetime::Temporary);
			CHECK(GameData::GetDirectory() == temporary.GetPath() / "Inner");
			CHECK(GameData::IsTemporary());
		}
		CHECK(GameData::GetDirectory() == kept);
		CHECK_FALSE(GameData::IsTemporary());
		{
			GameDataScope const empty({}, GameDataLifetime::Temporary);
			CHECK_FALSE(GameData::GetDirectory().has_value());
			CHECK_FALSE(GameData::IsTemporary());
		}
		CHECK(GameData::GetDirectory() == kept);
	}
	CHECK_FALSE(GameData::GetDirectory().has_value());
	// The service names the folder; the host creates nothing.
	CHECK_FALSE(FileSystem::Exists(kept));
	CHECK_FALSE(FileSystem::Exists(temporary.GetPath() / "Inner"));
}

TEST_CASE("GameData: a test run's temporary folder is new each time and goes with everything written into it")
{
	// Named below the system's temporary folder, and not created.
	std::filesystem::path const first = GameData::MakeTemporaryDirectory();
	std::filesystem::path const second = GameData::MakeTemporaryDirectory();
	REQUIRE_FALSE(first.empty());
	CHECK(first != second);
	CHECK(first.is_absolute());
	CHECK(first.parent_path() == second.parent_path());
	CHECK_FALSE(FileSystem::Exists(first));
	CHECK_FALSE(FileSystem::Exists(second));

	// What a run writes goes with a temporary folder and stays in a kept one.
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const run = temporary.GetPath() / "Run";
	std::filesystem::path const kept = temporary.GetPath() / "Kept";
	{
		GameDataScope const scope(run, GameDataLifetime::Temporary);
		REQUIRE(FileSystem::WriteTextFile(run / "Saves" / "Slot1.json", "{}").IsOk());
		GameDataScope const keptScope(kept, GameDataLifetime::Kept);
		REQUIRE(FileSystem::WriteTextFile(kept / "Input" / "Player.json", "{}").IsOk());
	}
	CHECK_FALSE(FileSystem::Exists(run));
	CHECK(FileSystem::IsRegularFile(kept / "Input" / "Player.json"));
	CHECK_FALSE(GameData::GetDirectory().has_value());
	// A temporary folder the run never wrote is no failure.
	{
		GameDataScope const scope(temporary.GetPath() / "Unused", GameDataLifetime::Temporary);
	}
	CHECK_FALSE(GameData::GetDirectory().has_value());
}
