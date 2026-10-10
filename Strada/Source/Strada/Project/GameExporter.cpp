#include "stpch.h"
#include "Strada/Project/GameExporter.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/UUID.h"
#include "Strada/Project/Project.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <array>
#include <initializer_list>
#include <system_error>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::array<std::string_view, 3> ScriptCoreFiles = {"Strada.ScriptCore.dll", "Strada.ScriptCore.runtimeconfig.json",
		                                                             "Strada.ScriptCore.deps.json"};
		constexpr std::string_view NoticesFileName = "ThirdPartyNotices.md";
		constexpr std::string_view RedistDirectoryName = "Redist";
		constexpr std::string_view VulkanDirectoryName = "Vulkan";
		constexpr std::string_view VulkanLoaderFileName = "libvulkan.1.dylib";
		constexpr std::string_view MoltenVkFileName = "libMoltenVK.dylib";
		constexpr std::string_view MoltenVkManifestFileName = "MoltenVK_icd.json";

		// Where an export's files go.
		struct GameLayout
		{
			// The app bundle (macOS); empty elsewhere.
			std::filesystem::path Bundle;
			std::filesystem::path ExecutableDirectory;
			// Game.sgame, Assets/, Scripts/ and the notices.
			std::filesystem::path GameDirectory;
			std::filesystem::path Executable;
		};

		GameLayout MakeLayout(std::filesystem::path const& root, std::string const& executableName, ExportPlatform platform)
		{
			GameLayout layout;
			if (platform == ExportPlatform::MacOS)
			{
				layout.Bundle = root / FileSystem::PathFromUtf8(executableName + ".app");
				layout.ExecutableDirectory = layout.Bundle / "Contents" / "MacOS";
				layout.GameDirectory = layout.Bundle / "Contents" / "Resources";
			}
			else
			{
				layout.ExecutableDirectory = root;
				layout.GameDirectory = root;
			}
			layout.Executable = layout.ExecutableDirectory / FileSystem::PathFromUtf8(executableName);
			return layout;
		}

		bool IsCancelled(std::atomic<bool> const* cancel)
		{
			return cancel != nullptr && cancel->load();
		}

		Error MakeCancelledError()
		{
			return Error{"the export was cancelled"};
		}

		std::filesystem::path MakeAbsolute(std::filesystem::path const& path)
		{
			std::error_code errorCode;
			std::filesystem::path const absolute = std::filesystem::absolute(path, errorCode);
			std::filesystem::path normalized = (errorCode ? path : absolute).lexically_normal();
			// "Games/Out/" names the directory Out.
			if (!normalized.has_filename() && normalized.has_relative_path())
			{
				normalized = normalized.parent_path();
			}
			return normalized;
		}

		// A new directory name next to path, for staging the export and setting the earlier one aside.
		std::filesystem::path MakeSibling(std::filesystem::path const& path, std::string_view purpose)
		{
			return path.parent_path() /
			       FileSystem::PathFromUtf8(fmt::format("{}.{}-{}", FileSystem::PathToUtf8(path.filename()), purpose, UUID().ToString()));
		}

		// An earlier export: Game.sgame at the top, or in an app bundle's resources.
		bool IsExportedGame(std::filesystem::path const& directory)
		{
			std::filesystem::path const gameFile = FileSystem::PathFromUtf8(Project::GameFileName);
			if (FileSystem::IsRegularFile(directory / gameFile))
			{
				return true;
			}
			std::error_code errorCode;
			for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(directory, errorCode))
			{
				if (entry.path().extension() == ".app" && FileSystem::IsRegularFile(entry.path() / "Contents" / "Resources" / gameFile))
				{
					return true;
				}
			}
			return false;
		}

		Result<void> CheckOutputDirectory(std::filesystem::path const& output, std::filesystem::path const& assetDirectory)
		{
			if (!output.has_relative_path())
			{
				return MakeError("'{}' is a root directory: export into a directory of its own", FileSystem::PathToUtf8(output));
			}
			if (FileSystem::IsInside(output, assetDirectory))
			{
				return MakeError("'{}' is inside the asset directory, which the game copies", FileSystem::PathToUtf8(output));
			}
			if (!FileSystem::Exists(output))
			{
				return {};
			}
			if (!FileSystem::IsDirectory(output))
			{
				return MakeError("'{}' is not a directory", FileSystem::PathToUtf8(output));
			}
			std::error_code errorCode;
			bool const empty = std::filesystem::is_empty(output, errorCode);
			if (errorCode)
			{
				return MakeError("'{}' cannot be read: {}", FileSystem::PathToUtf8(output), errorCode.message());
			}
			if (!empty && !IsExportedGame(output))
			{
				return MakeError("'{}' is not empty and holds no exported game: export into a new or empty directory",
				                 FileSystem::PathToUtf8(output));
			}
			return {};
		}

		// Removes a directory when it goes out of scope, unless released.
		class DirectoryCleanup
		{
		public:
			explicit DirectoryCleanup(std::filesystem::path directory)
				: m_Directory(std::move(directory))
			{
			}

			~DirectoryCleanup()
			{
				if (!m_Directory.empty())
				{
					(void)FileSystem::RemoveAll(m_Directory);
				}
			}

			DirectoryCleanup(DirectoryCleanup const&) = delete;
			DirectoryCleanup& operator=(DirectoryCleanup const&) = delete;

			void Release() { m_Directory.clear(); }

		private:
			std::filesystem::path m_Directory;
		};

		// Copies a directory's files, leaving hidden files and folders (.git, editor state) behind like the asset scan does.
		Result<void> CopyVisibleFiles(std::filesystem::path const& from, std::filesystem::path const& to, std::atomic<bool> const* cancel)
		{
			if (Result<void> created = FileSystem::CreateDirectories(to); !created)
			{
				return created;
			}
			std::error_code errorCode;
			std::filesystem::recursive_directory_iterator it(from, std::filesystem::directory_options::none, errorCode);
			if (errorCode)
			{
				return MakeError("'{}' cannot be read: {}", FileSystem::PathToUtf8(from), errorCode.message());
			}
			for (std::filesystem::recursive_directory_iterator const end; it != end; it.increment(errorCode))
			{
				if (errorCode)
				{
					return MakeError("'{}' cannot be read: {}", FileSystem::PathToUtf8(from), errorCode.message());
				}
				if (IsCancelled(cancel))
				{
					return MakeCancelledError();
				}
				std::filesystem::directory_entry const& entry = *it;
				std::error_code entryError;
				if (FileSystem::PathToUtf8(entry.path().filename()).starts_with('.'))
				{
					if (entry.is_directory(entryError))
					{
						it.disable_recursion_pending();
					}
					continue;
				}
				std::filesystem::path const target = to / entry.path().lexically_relative(from);
				if (entry.is_directory(entryError))
				{
					if (Result<void> created = FileSystem::CreateDirectories(target); !created)
					{
						return created;
					}
				}
				else if (entry.is_regular_file(entryError))
				{
					if (Result<void> copied = FileSystem::Copy(entry.path(), target, true); !copied)
					{
						return copied;
					}
				}
			}
			return {};
		}

		// An assembly and its companions: <Name>.pdb, <Name>.deps.json, <Name>.xml.
		Result<void> CopyAssemblyFiles(std::filesystem::path const& assembly, std::filesystem::path const& to)
		{
			if (Result<void> created = FileSystem::CreateDirectories(to); !created)
			{
				return created;
			}
			std::string const prefix = FileSystem::PathToUtf8(assembly.stem()) + ".";
			std::error_code errorCode;
			for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(assembly.parent_path(), errorCode))
			{
				std::error_code entryError;
				if (!entry.is_regular_file(entryError) || !FileSystem::PathToUtf8(entry.path().filename()).starts_with(prefix))
				{
					continue;
				}
				if (Result<void> copied = FileSystem::Copy(entry.path(), to / entry.path().filename(), true); !copied)
				{
					return copied;
				}
			}
			if (errorCode)
			{
				return MakeError("'{}' cannot be read: {}", FileSystem::PathToUtf8(assembly.parent_path()), errorCode.message());
			}
			return {};
		}

		Result<void> CopyEngineFile(std::filesystem::path const& engineDirectory, std::string_view name, std::filesystem::path const& to)
		{
			std::filesystem::path const from = engineDirectory / FileSystem::PathFromUtf8(name);
			if (!FileSystem::IsRegularFile(from))
			{
				return MakeError("the engine's {} is missing from '{}'", name, FileSystem::PathToUtf8(engineDirectory));
			}
			return FileSystem::Copy(from, to, true);
		}

		Result<void> MakeExecutable(std::filesystem::path const& file)
		{
			std::error_code errorCode;
			std::filesystem::permissions(
				file, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
				std::filesystem::perm_options::add, errorCode);
			if (errorCode)
			{
				return MakeError("'{}' cannot be made executable: {}", FileSystem::PathToUtf8(file), errorCode.message());
			}
			return {};
		}

		std::string EscapeXml(std::string_view text)
		{
			std::string escaped;
			for (char const character : text)
			{
				switch (character)
				{
					case '&':
						escaped += "&amp;";
						break;
					case '<':
						escaped += "&lt;";
						break;
					case '>':
						escaped += "&gt;";
						break;
					case '"':
						escaped += "&quot;";
						break;
					case '\'':
						escaped += "&apos;";
						break;
					default:
						escaped += character;
						break;
				}
			}
			return escaped;
		}

		// Reverse-DNS bundle identifiers allow ASCII letters, digits, '-' and '.'.
		std::string MakeBundleIdentifier(std::string_view name)
		{
			std::string identifier = "org.strada.";
			bool any = false;
			for (char const character : name)
			{
				bool const usable = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
				                    (character >= '0' && character <= '9');
				identifier += usable ? character : '-';
				any = any || usable;
			}
			return any ? identifier : identifier + "game";
		}

		std::string MakeInfoPlist(std::string_view name, std::string_view executableName)
		{
			return fmt::format(R"plist(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleDisplayName</key>
	<string>{0}</string>
	<key>CFBundleExecutable</key>
	<string>{1}</string>
	<key>CFBundleIdentifier</key>
	<string>{2}</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>{0}</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>1.0</string>
	<key>CFBundleVersion</key>
	<string>1</string>
	<key>LSMinimumSystemVersion</key>
	<string>13.3</string>
	<key>NSHighResolutionCapable</key>
	<true/>
</dict>
</plist>
)plist",
			                   EscapeXml(name), EscapeXml(executableName), EscapeXml(MakeBundleIdentifier(name)));
		}

		// The Vulkan loader looks for drivers in the bundle's Contents/Resources/vulkan/icd.d; the manifest's library path is
		// relative to it.
		Result<void> WriteMoltenVkManifest(std::filesystem::path const& sdkManifest, std::filesystem::path const& target)
		{
			Result<std::string> text = FileSystem::ReadTextFile(sdkManifest);
			if (!text)
			{
				return Error{text.GetError()};
			}
			Result<Json> manifest = ParseJson(text.GetValue());
			if (!manifest || !manifest.GetValue().contains("ICD") || !manifest.GetValue()["ICD"].is_object())
			{
				return MakeError("'{}' is not a Vulkan driver manifest", FileSystem::PathToUtf8(sdkManifest));
			}
			manifest.GetValue()["ICD"]["library_path"] = fmt::format("../../../Frameworks/{}", MoltenVkFileName);
			return FileSystem::WriteTextFile(target, DumpJson(manifest.GetValue()));
		}

		Result<void> AddPlatformFiles(GameExportSettings const& settings, GameLayout const& layout, std::string const& executableName)
		{
			switch (settings.Platform)
			{
				case ExportPlatform::Windows:
				{
					// The MSVC runtime the player needs, deployed next to it (Release and Dist builds provide it).
					std::filesystem::path const redist = settings.EngineDirectory / FileSystem::PathFromUtf8(RedistDirectoryName);
					if (!FileSystem::IsDirectory(redist))
					{
						return {};
					}
					std::error_code errorCode;
					for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(redist, errorCode))
					{
						std::error_code entryError;
						if (!entry.is_regular_file(entryError))
						{
							continue;
						}
						if (Result<void> copied =
						        FileSystem::Copy(entry.path(), layout.ExecutableDirectory / entry.path().filename(), true);
						    !copied)
						{
							return copied;
						}
					}
					if (errorCode)
					{
						return MakeError("'{}' cannot be read: {}", FileSystem::PathToUtf8(redist), errorCode.message());
					}
					return {};
				}
				case ExportPlatform::Linux:
					// The system's Vulkan loader and drivers serve the player.
					return MakeExecutable(layout.Executable);
				case ExportPlatform::MacOS:
				{
					if (Result<void> executable = MakeExecutable(layout.Executable); !executable)
					{
						return executable;
					}
					std::filesystem::path const vulkan = settings.EngineDirectory / FileSystem::PathFromUtf8(VulkanDirectoryName);
					std::filesystem::path const frameworks = layout.Bundle / "Contents" / "Frameworks";
					for (std::string_view const library : {VulkanLoaderFileName, MoltenVkFileName})
					{
						if (Result<void> copied = CopyEngineFile(vulkan, library, frameworks / FileSystem::PathFromUtf8(library)); !copied)
						{
							return copied;
						}
					}
					std::filesystem::path const manifest = vulkan / FileSystem::PathFromUtf8(MoltenVkManifestFileName);
					if (!FileSystem::IsRegularFile(manifest))
					{
						return MakeError("the engine's {} is missing from '{}'", MoltenVkManifestFileName, FileSystem::PathToUtf8(vulkan));
					}
					std::filesystem::path const drivers = layout.GameDirectory / "vulkan" / "icd.d";
					if (Result<void> created = FileSystem::CreateDirectories(drivers); !created)
					{
						return created;
					}
					if (Result<void> written =
					        WriteMoltenVkManifest(manifest, drivers / FileSystem::PathFromUtf8(MoltenVkManifestFileName));
					    !written)
					{
						return written;
					}
					return FileSystem::WriteTextFile(layout.Bundle / "Contents" / "Info.plist",
					                                 MakeInfoPlist(settings.Project.Name, executableName));
				}
			}
			return {};
		}

		// Replaces the output directory with the staged game; an earlier export is set aside first and restored on failure.
		Result<void> MoveIntoPlace(std::filesystem::path const& staging, std::filesystem::path const& output)
		{
			std::filesystem::path earlier;
			if (FileSystem::Exists(output))
			{
				earlier = MakeSibling(output, "replaced");
				if (Result<void> moved = FileSystem::Move(output, earlier); !moved)
				{
					return MakeError("the earlier export in '{}' cannot be replaced (is the game running?): {}",
					                 FileSystem::PathToUtf8(output), moved.GetError());
				}
			}
			if (Result<void> moved = FileSystem::Move(staging, output); !moved)
			{
				if (!earlier.empty())
				{
					(void)FileSystem::Move(earlier, output);
				}
				return moved;
			}
			if (!earlier.empty())
			{
				if (Result<void> removed = FileSystem::RemoveAll(earlier); !removed)
				{
					ST_CORE_WARN("The earlier export could not be removed: {}", removed.GetError());
				}
			}
			return {};
		}
	}

	ExportPlatform GameExporter::GetHostPlatform()
	{
#if defined(ST_PLATFORM_WINDOWS)
		return ExportPlatform::Windows;
#elif defined(ST_PLATFORM_MACOS)
		return ExportPlatform::MacOS;
#else
		return ExportPlatform::Linux;
#endif
	}

	std::string GameExporter::GetExecutableName(std::string_view gameName, ExportPlatform platform)
	{
		std::string name = FileSystem::MakePortableFileName(gameName);
		if (name.empty())
		{
			name = "Game";
		}
		return platform == ExportPlatform::Windows ? name + ".exe" : name;
	}

	Result<GameExportSettings> GameExporter::Prepare(Project const& project, std::filesystem::path const& outputDirectory)
	{
		if (!AssetManager::IsInitialized() || !AssetManager::HasAssetDirectory() ||
		    AssetManager::GetAssetDirectory() != project.GetAssetDirectory())
		{
			return Error{"the project's asset directory is not open"};
		}
		if (AssetManager::GetAssetType(project.GetSettings().StartScene) != AssetType::Scene)
		{
			return Error{"the project has no start scene: choose one in the project settings"};
		}
		// The game's registry must list every asset: the player would give unregistered ones new handles.
		if (Result<AssetRefreshResult> refreshed = AssetManager::Refresh(); !refreshed)
		{
			return MakeError("the assets cannot be scanned: {}", refreshed.GetError());
		}
		if (Result<void> saved = AssetManager::SaveRegistry(); !saved)
		{
			return MakeError("the asset registry cannot be saved: {}", saved.GetError());
		}

		GameExportSettings settings;
		settings.OutputDirectory = outputDirectory;
		settings.Platform = GetHostPlatform();
		settings.Project = project.GetSettings();
		settings.AssetDirectory = project.GetAssetDirectory();
		if (ScriptProject::Exists(project))
		{
			settings.ScriptProjectFile = ScriptProject::GetProjectFile(project);
		}
		settings.ScriptAssembly = ScriptProject::GetAssemblyPath(project);
		settings.EngineDirectory = FileSystem::GetExecutableDirectory();
		settings.Player = settings.EngineDirectory / FileSystem::PathFromUtf8(settings.Platform == ExportPlatform::Windows
		                                                                          ? std::string_view("StradaRuntime.exe")
		                                                                          : std::string_view("StradaRuntime"));
		if (!FileSystem::IsRegularFile(settings.Player))
		{
			return MakeError("the game player is missing: '{}' was not found", FileSystem::PathToUtf8(settings.Player));
		}
		return settings;
	}

	Result<GameExportResult> GameExporter::Export(GameExportSettings const& settings, ProgressCallback const& progress,
	                                              std::atomic<bool> const* cancel)
	{
		auto const report = [&progress](std::string_view step)
		{
			if (progress)
			{
				progress(step);
			}
		};
		std::filesystem::path const output = MakeAbsolute(settings.OutputDirectory);
		if (Result<void> usable = CheckOutputDirectory(output, MakeAbsolute(settings.AssetDirectory)); !usable)
		{
			return Error{usable.GetError()};
		}
		if (!FileSystem::IsRegularFile(settings.Player))
		{
			return MakeError("the game player is missing: '{}' was not found", FileSystem::PathToUtf8(settings.Player));
		}

		std::filesystem::path const staging = MakeSibling(output, "export");
		if (Result<void> created = FileSystem::CreateDirectories(staging); !created)
		{
			return Error{created.GetError()};
		}
		DirectoryCleanup cleanup(staging);
		std::string const executableName = GetExecutableName(settings.Project.Name, settings.Platform);
		GameLayout const layout = MakeLayout(staging, executableName, settings.Platform);

		GameExportResult result;
		ProjectSettings game = settings.Project;
		game.AssetDirectory = "Assets";
		game.ScriptModule = "Scripts/" + FileSystem::PathToUtf8(FileSystem::PathFromUtf8(settings.Project.ScriptModule).filename());

		std::filesystem::path const scripts = layout.GameDirectory / "Scripts";
		bool hasScripts = false;
		if (!settings.ScriptProjectFile.empty())
		{
			report("Building the scripts");
			ScriptBuildSettings build;
			build.ProjectFile = settings.ScriptProjectFile;
			build.OutputDirectory = scripts;
			build.Configuration = ScriptBuildConfiguration::Release;
			build.DotNet = settings.DotNet;
			Result<ScriptBuildResult> built = ScriptProject::Build(build, cancel);
			if (!built)
			{
				return IsCancelled(cancel) ? MakeCancelledError() : MakeError("the scripts cannot be built: {}", built.GetError());
			}
			result.ScriptBuild = built.TakeValue();
			if (!result.ScriptBuild->Succeeded)
			{
				return result;
			}
			game.ScriptModule = "Scripts/" + FileSystem::PathToUtf8(result.ScriptBuild->Assembly.filename());
			hasScripts = true;
		}
		else if (FileSystem::IsRegularFile(settings.ScriptAssembly))
		{
			// Scripts built elsewhere ship as they are.
			report("Copying the scripts");
			if (Result<void> copied = CopyAssemblyFiles(settings.ScriptAssembly, scripts); !copied)
			{
				return Error{copied.GetError()};
			}
			game.ScriptModule = "Scripts/" + FileSystem::PathToUtf8(settings.ScriptAssembly.filename());
			hasScripts = true;
		}
		if (hasScripts)
		{
			// Exported games carry the scripting API next to their scripts.
			for (std::string_view const file : ScriptCoreFiles)
			{
				if (Result<void> copied = CopyEngineFile(settings.EngineDirectory, file, scripts / FileSystem::PathFromUtf8(file)); !copied)
				{
					return Error{copied.GetError()};
				}
			}
		}
		if (IsCancelled(cancel))
		{
			return MakeCancelledError();
		}

		report("Copying the assets");
		if (Result<void> copied = CopyVisibleFiles(settings.AssetDirectory, layout.GameDirectory / "Assets", cancel); !copied)
		{
			return Error{copied.GetError()};
		}

		report("Writing the game");
		if (Result<void> written = FileSystem::WriteTextFile(layout.GameDirectory / FileSystem::PathFromUtf8(Project::GameFileName),
		                                                     DumpJson(Project::Serialize(game, ProjectFileKind::Game)));
		    !written)
		{
			return Error{written.GetError()};
		}
		if (Result<void> copied =
		        CopyEngineFile(settings.EngineDirectory, NoticesFileName, layout.GameDirectory / FileSystem::PathFromUtf8(NoticesFileName));
		    !copied)
		{
			return Error{copied.GetError()};
		}
		if (Result<void> copied = FileSystem::Copy(settings.Player, layout.Executable, true); !copied)
		{
			return Error{copied.GetError()};
		}
		if (Result<void> added = AddPlatformFiles(settings, layout, executableName); !added)
		{
			return Error{added.GetError()};
		}
		if (IsCancelled(cancel))
		{
			return MakeCancelledError();
		}

		if (Result<void> moved = MoveIntoPlace(staging, output); !moved)
		{
			return Error{moved.GetError()};
		}
		cleanup.Release();
		result.Succeeded = true;
		result.Executable = output / (layout.Bundle.empty() ? layout.Executable.filename() : layout.Bundle.filename());
		return result;
	}
}
