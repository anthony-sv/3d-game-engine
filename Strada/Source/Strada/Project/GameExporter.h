#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Project/ProjectSettings.h"
#include "Strada/Project/ScriptProject.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Strada
{
	class Project;

	// The platform whose game layout an export writes (the player executable must be built for it).
	enum class ExportPlatform : uint8_t
	{
		Windows = 0,
		Linux,
		MacOS,
	};

	// What an export needs, gathered on the main thread (GameExporter::Prepare) so the export can run on any thread.
	struct GameExportSettings
	{
		// Where the game is written: a new or empty directory, or an earlier export, which the new one replaces.
		std::filesystem::path OutputDirectory;
		ExportPlatform Platform = ExportPlatform::Windows;
		// The project's settings; the game's configuration points them at the exported layout.
		ProjectSettings Project;
		std::filesystem::path AssetDirectory;
		// The C# project, built in Release into the game; empty without one (a built ScriptAssembly then ships as it is).
		std::filesystem::path ScriptProjectFile;
		std::filesystem::path ScriptAssembly;
		// The player shipped as the game's executable: StradaRuntime next to the editor.
		std::filesystem::path Player;
		// The engine's files games ship, as the build puts them next to the editor: Strada.ScriptCore.dll with its
		// .runtimeconfig.json and .deps.json, ThirdPartyNotices.md, Redist/ (the MSVC runtime on Windows; absent in Debug
		// builds, whose runtime is not redistributable) and Vulkan/ (the Vulkan loader, MoltenVK and its driver manifest, for
		// macOS app bundles).
		std::filesystem::path EngineDirectory;
		// The .NET SDK driver; empty finds it like ScriptProject::FindDotNet.
		std::filesystem::path DotNet;
	};

	struct GameExportResult
	{
		// False when the scripts did not build (ScriptBuild says why); nothing was exported then.
		bool Succeeded = false;
		// The game's executable, or its app bundle on macOS.
		std::filesystem::path Executable;
		// The Release build of the game's scripts, when the game has a C# project.
		std::optional<ScriptBuildResult> ScriptBuild;
	};

	// Exports projects as distributable games:
	//   <Out>/<Name>[.exe]        the player, renamed after the game
	//   <Out>/Game.sgame          the game's configuration (Project::OpenGame)
	//   <Out>/Assets/             the asset directory with its registry (hidden files stay behind)
	//   <Out>/Scripts/            the scripts built in Release, with Strada.ScriptCore.dll (only for games with scripts)
	//   <Out>/ThirdPartyNotices.md
	// plus the MSVC runtime DLLs next to the executable on Windows. On macOS <Out> holds <Name>.app: the player in
	// Contents/MacOS, the files above in Contents/Resources, the Vulkan loader and MoltenVK in Contents/Frameworks with
	// MoltenVK's driver manifest in Contents/Resources/vulkan/icd.d. Players of other machines need no installed Vulkan SDK;
	// games with scripts need the .NET 10 runtime.
	class GameExporter
	{
	public:
		using ProgressCallback = std::function<void(std::string_view step)>;

		static ExportPlatform GetHostPlatform();
		// The executable's file name: the game's name made portable ("Game" when nothing usable remains), with .exe on Windows.
		static std::string GetExecutableName(std::string_view gameName, ExportPlatform platform);

		// Gathers the settings for exporting the open project to a directory for this platform, with the player and the
		// engine's files next to this executable. Refreshes the asset directory first so its registry lists every asset.
		// Fails when the project has no start scene or the player is missing. Main thread only.
		[[nodiscard]] static Result<GameExportSettings> Prepare(Project const& project, std::filesystem::path const& outputDirectory);

		// Writes the game. It is assembled next to the output directory and moved into place at the end, so a failed or
		// cancelled export leaves an earlier one untouched. Blocks the calling thread (any thread); progress names each step
		// on that thread; setting *cancel from another thread stops the export.
		[[nodiscard]] static Result<GameExportResult> Export(GameExportSettings const& settings, ProgressCallback const& progress = {},
		                                                     std::atomic<bool> const* cancel = nullptr);
	};
}
