#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Project/GameExporter.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	std::string Read(std::filesystem::path const& path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE_MESSAGE(text.IsOk(), (text ? std::string() : text.GetError()));
		return text.GetValue();
	}

	void Write(std::filesystem::path const& path, std::string_view text)
	{
		REQUIRE(FileSystem::WriteTextFile(path, text).IsOk());
	}

	std::vector<std::string> ListNames(std::filesystem::path const& directory)
	{
		std::vector<std::string> names;
		std::error_code errorCode;
		for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(directory, errorCode))
		{
			names.push_back(FileSystem::PathToUtf8(entry.path().filename()));
		}
		std::sort(names.begin(), names.end());
		return names;
	}

	// A project with a scene, hidden files and prebuilt scripts, and the engine files an export takes, all made up: the
	// layout is what is checked, for every platform.
	class ExportFixture
	{
	public:
		ExportFixture()
		{
			std::filesystem::path const assets = GetProjectDirectory() / "Assets";
			REQUIRE(FileSystem::CreateDirectories(assets / "Scenes").IsOk());
			REQUIRE(SceneSerializer::SaveToFile(Scene("Main"), assets / "Scenes" / "Main.sscene").IsOk());
			REQUIRE(FileSystem::CreateDirectories(assets / ".cache").IsOk());
			Write(assets / ".cache" / "Thumbnail.png", "editor state");
			Write(assets / ".gitignore", "*.tmp");
			std::filesystem::path const binaries = GetProjectDirectory() / "Scripts" / "Binaries";
			REQUIRE(FileSystem::CreateDirectories(binaries).IsOk());
			for (char const* file : {"Game.dll", "Game.pdb", "Game.deps.json", "Unrelated.dll"})
			{
				Write(binaries / file, file);
			}

			std::filesystem::path const engine = GetEngineDirectory();
			REQUIRE(FileSystem::CreateDirectories(engine / "Redist").IsOk());
			REQUIRE(FileSystem::CreateDirectories(engine / "Vulkan").IsOk());
			for (char const* file :
			     {"StradaRuntime", "Strada.ScriptCore.dll", "Strada.ScriptCore.runtimeconfig.json", "Strada.ScriptCore.deps.json",
			      "ThirdPartyNotices.md", "Redist/vcruntime140.dll", "Vulkan/libvulkan.1.dylib", "Vulkan/libMoltenVK.dylib"})
			{
				Write(engine / file, file);
			}
			Write(
				engine / "Vulkan" / "MoltenVK_icd.json",
				R"({ "file_format_version": "1.0.0", "ICD": { "library_path": "../../../lib/libMoltenVK.dylib", "api_version": "1.2.0" } })");
		}

		std::filesystem::path GetProjectDirectory() const { return m_Directory.GetPath() / "Project"; }
		std::filesystem::path GetEngineDirectory() const { return m_Directory.GetPath() / "Engine"; }
		std::filesystem::path GetOutputDirectory() const { return m_Directory.GetPath() / "Out"; }
		std::filesystem::path const& GetRoot() const { return m_Directory.GetPath(); }

		GameExportSettings MakeSettings(ExportPlatform platform) const
		{
			GameExportSettings settings;
			settings.OutputDirectory = GetOutputDirectory();
			settings.Platform = platform;
			settings.Project.Name = "Space & Time";
			settings.AssetDirectory = GetProjectDirectory() / "Assets";
			settings.ScriptAssembly = GetProjectDirectory() / "Scripts" / "Binaries" / "Game.dll";
			settings.Player = GetEngineDirectory() / "StradaRuntime";
			settings.EngineDirectory = GetEngineDirectory();
			return settings;
		}

		GameExportResult Export(GameExportSettings const& settings) const
		{
			std::vector<std::string> steps;
			Result<GameExportResult> exported = GameExporter::Export(settings,
			                                                         [&steps](std::string_view step)
			                                                         {
																		 steps.emplace_back(step);
																	 });
			REQUIRE_MESSAGE(exported.IsOk(), (exported ? std::string() : exported.GetError()));
			CHECK_FALSE(steps.empty());
			// The staging directory is gone: the output directory sits alone next to the project and the engine.
			CHECK(ListNames(GetRoot()) == std::vector<std::string>{"Engine", "Out", "Project"});
			return exported.TakeValue();
		}

	private:
		Testing::TemporaryDirectory m_Directory;
	};

	// The game's configuration points at the exported layout.
	void CheckGameFile(std::filesystem::path const& file, std::string const& scriptModule)
	{
		Json const document = Json::parse(Read(file));
		CHECK(document["Strada"]["Type"] == "Game");
		CHECK(document["Game"]["Name"] == "Space & Time");
		CHECK(document["Game"]["AssetDirectory"] == "Assets");
		CHECK(document["Game"]["ScriptModule"] == scriptModule);
	}

	// Assets without hidden files, and the scripts with the scripting API but without unrelated assemblies.
	void CheckGameDirectory(std::filesystem::path const& game)
	{
		CHECK(FileSystem::IsRegularFile(game / "Assets" / "Scenes" / "Main.sscene"));
		CHECK_FALSE(FileSystem::Exists(game / "Assets" / ".cache"));
		CHECK_FALSE(FileSystem::Exists(game / "Assets" / ".gitignore"));
		CHECK(ListNames(game / "Scripts") == std::vector<std::string>{"Game.deps.json", "Game.dll", "Game.pdb",
		                                                              "Strada.ScriptCore.deps.json", "Strada.ScriptCore.dll",
		                                                              "Strada.ScriptCore.runtimeconfig.json"});
		CHECK(Read(game / "ThirdPartyNotices.md") == "ThirdPartyNotices.md");
		CheckGameFile(game / "Game.sgame", "Scripts/Game.dll");
	}
}

TEST_CASE("GameExporter: executables are named after the game")
{
	CHECK(GameExporter::GetExecutableName("Space Game", ExportPlatform::Windows) == "Space Game.exe");
	CHECK(GameExporter::GetExecutableName("Space Game", ExportPlatform::Linux) == "Space Game");
	CHECK(GameExporter::GetExecutableName("Space Game", ExportPlatform::MacOS) == "Space Game");
	CHECK(GameExporter::GetExecutableName("A/B: C", ExportPlatform::Windows) == "A_B_ C.exe");
	CHECK(GameExporter::GetExecutableName("", ExportPlatform::Linux) == "Game");
#if defined(ST_PLATFORM_WINDOWS)
	CHECK(GameExporter::GetHostPlatform() == ExportPlatform::Windows);
#elif defined(ST_PLATFORM_MACOS)
	CHECK(GameExporter::GetHostPlatform() == ExportPlatform::MacOS);
#else
	CHECK(GameExporter::GetHostPlatform() == ExportPlatform::Linux);
#endif
}

TEST_CASE("GameExporter: Windows and Linux games are a directory with the player, the assets and the scripts")
{
	ExportFixture fixture;
	GameExportResult const windows = fixture.Export(fixture.MakeSettings(ExportPlatform::Windows));
	std::filesystem::path const output = fixture.GetOutputDirectory();
	CHECK(windows.Succeeded);
	CHECK_FALSE(windows.ScriptBuild.has_value());
	CHECK(windows.Executable == output / "Space _ Time.exe");
	CHECK(Read(windows.Executable) == "StradaRuntime");
	// The MSVC runtime goes next to the executable.
	CHECK(Read(output / "vcruntime140.dll") == "Redist/vcruntime140.dll");
	CheckGameDirectory(output);

	// Exporting again replaces the earlier game, also for another platform.
	Write(output / "Stale.txt", "left from before");
	GameExportResult const linux = fixture.Export(fixture.MakeSettings(ExportPlatform::Linux));
	CHECK(linux.Executable == output / "Space _ Time");
	CHECK(Read(linux.Executable) == "StradaRuntime");
	CHECK_FALSE(FileSystem::Exists(output / "Stale.txt"));
	CHECK_FALSE(FileSystem::Exists(output / "Space _ Time.exe"));
	CHECK_FALSE(FileSystem::Exists(output / "vcruntime140.dll"));
	CheckGameDirectory(output);
}

TEST_CASE("GameExporter: macOS games are app bundles with the Vulkan loader and MoltenVK")
{
	ExportFixture fixture;
	GameExportResult const result = fixture.Export(fixture.MakeSettings(ExportPlatform::MacOS));
	std::filesystem::path const bundle = fixture.GetOutputDirectory() / "Space _ Time.app";
	CHECK(result.Executable == bundle);
	CHECK(ListNames(fixture.GetOutputDirectory()) == std::vector<std::string>{"Space _ Time.app"});
	CHECK(Read(bundle / "Contents" / "MacOS" / "Space _ Time") == "StradaRuntime");
	CHECK(Read(bundle / "Contents" / "Frameworks" / "libvulkan.1.dylib") == "Vulkan/libvulkan.1.dylib");
	CHECK(Read(bundle / "Contents" / "Frameworks" / "libMoltenVK.dylib") == "Vulkan/libMoltenVK.dylib");
	CheckGameDirectory(bundle / "Contents" / "Resources");

	// The driver manifest finds MoltenVK relative to itself.
	Json const manifest = Json::parse(Read(bundle / "Contents" / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json"));
	CHECK(manifest["ICD"]["library_path"] == "../../../Frameworks/libMoltenVK.dylib");
	CHECK(manifest["ICD"]["api_version"] == "1.2.0");

	std::string const plist = Read(bundle / "Contents" / "Info.plist");
	CHECK(plist.find("<key>CFBundleExecutable</key>\n\t<string>Space _ Time</string>") != std::string::npos);
	CHECK(plist.find("<string>Space &amp; Time</string>") != std::string::npos);
	CHECK(plist.find("<string>org.strada.Space---Time</string>") != std::string::npos);
	CHECK(plist.find("<string>APPL</string>") != std::string::npos);
}

TEST_CASE("GameExporter: games without scripts ship none")
{
	ExportFixture fixture;
	GameExportSettings settings = fixture.MakeSettings(ExportPlatform::Linux);
	settings.ScriptAssembly = fixture.GetProjectDirectory() / "Scripts" / "Binaries" / "Missing.dll";
	settings.Project.ScriptModule = "Scripts/Binaries/Missing.dll";
	fixture.Export(settings);
	CHECK_FALSE(FileSystem::Exists(fixture.GetOutputDirectory() / "Scripts"));
	CheckGameFile(fixture.GetOutputDirectory() / "Game.sgame", "Scripts/Missing.dll");
}

TEST_CASE("GameExporter: exports that cannot finish leave the earlier game and no partial one")
{
	ExportFixture fixture;
	std::filesystem::path const output = fixture.GetOutputDirectory();

	// Only empty directories and earlier exports are written to.
	REQUIRE(FileSystem::CreateDirectories(output).IsOk());
	Write(output / "Notes.txt", "mine");
	Result<GameExportResult> refused = GameExporter::Export(fixture.MakeSettings(ExportPlatform::Windows));
	REQUIRE(refused.IsError());
	CHECK(refused.GetError().find("is not empty and holds no exported game") != std::string::npos);
	CHECK(ListNames(output) == std::vector<std::string>{"Notes.txt"});
	REQUIRE(FileSystem::Remove(output / "Notes.txt").IsOk());

	GameExportSettings inside = fixture.MakeSettings(ExportPlatform::Windows);
	inside.OutputDirectory = inside.AssetDirectory / "Build";
	CHECK(GameExporter::Export(inside).IsError());

	fixture.Export(fixture.MakeSettings(ExportPlatform::Windows));
	std::string const game = Read(output / "Game.sgame");

	// A missing engine file stops the export before anything is replaced.
	REQUIRE(FileSystem::Remove(fixture.GetEngineDirectory() / "ThirdPartyNotices.md").IsOk());
	Result<GameExportResult> failed = GameExporter::Export(fixture.MakeSettings(ExportPlatform::Linux));
	REQUIRE(failed.IsError());
	CHECK(failed.GetError().find("ThirdPartyNotices.md") != std::string::npos);
	CHECK(Read(output / "Game.sgame") == game);
	CHECK(FileSystem::IsRegularFile(output / "Space _ Time.exe"));
	CHECK(ListNames(fixture.GetRoot()) == std::vector<std::string>{"Engine", "Out", "Project"});

	std::atomic<bool> const cancel = true;
	Result<GameExportResult> cancelled = GameExporter::Export(fixture.MakeSettings(ExportPlatform::Windows), {}, &cancel);
	REQUIRE(cancelled.IsError());
	CHECK(cancelled.GetError() == "the export was cancelled");
	CHECK(ListNames(fixture.GetRoot()) == std::vector<std::string>{"Engine", "Out", "Project"});

	GameExportSettings noPlayer = fixture.MakeSettings(ExportPlatform::Windows);
	noPlayer.Player = fixture.GetEngineDirectory() / "Missing";
	CHECK(GameExporter::Export(noPlayer).IsError());
}

TEST_CASE("GameExporter: only open projects with a start scene are prepared for export")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory directory;
	Result<Ref<Project>> created = Project::Create(directory.GetPath() / "Game", "Game", Scene("Main"));
	REQUIRE(created.IsOk());
	Ref<Project> const project = created.GetValue();

	// Projects start with their start scene; without one there is nothing to run.
	REQUIRE(project->ApplySettings(Json::object({{"StartScene", "0"}})).IsOk());
	Result<GameExportSettings> noStartScene = GameExporter::Prepare(*project, directory.GetPath() / "Out");
	REQUIRE(noStartScene.IsError());
	CHECK(noStartScene.GetError() == "the project has no start scene: choose one in the project settings");

	AssetManager::CloseAssetDirectory();
	Result<GameExportSettings> closed = GameExporter::Prepare(*project, directory.GetPath() / "Out");
	REQUIRE(closed.IsError());
	CHECK(closed.GetError() == "the project's asset directory is not open");
}
