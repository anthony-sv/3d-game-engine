#include "Editor/ProjectImport.h"

#include "Editor/AssetBrowsing.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/ScriptBrowsing.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"

#include <algorithm>
#include <system_error>

namespace Strada
{
	namespace ProjectImport
	{
		namespace
		{
			// Dropped folders are copied while the editor waits: larger ones belong in the file manager.
			constexpr size_t MaxFolderFiles = 10000;
			// Kept files listed by name in the log.
			constexpr size_t ListedKeptFiles = 5;

			bool IsAssetFile(std::filesystem::path const& path)
			{
				return GetAssetTypeForExtension(FileSystem::PathToUtf8(path.extension())) != AssetType::None;
			}

			std::string CountOf(size_t count, std::string_view noun)
			{
				return fmt::format("{} {}{}", count, noun, count == 1 ? "" : "s");
			}

			std::string JoinNames(std::span<std::string const> names)
			{
				std::string joined;
				for (std::string const& name : names)
				{
					joined += joined.empty() ? name : ", " + name;
				}
				return joined;
			}

			// Imports the files as one batch; when the batch fails (all or nothing), one by one, so that a broken file does
			// not hold up the others.
			void ImportAssetFiles(EditorOperations& operations, std::span<std::filesystem::path const> files, std::string_view folder,
			                      Report& report)
			{
				if (files.empty())
				{
					return;
				}
				Result<std::vector<AssetHandle>> imported = operations.ImportAssets(files, folder);
				if (imported)
				{
					report.Assets.insert(report.Assets.end(), imported.GetValue().begin(), imported.GetValue().end());
					return;
				}
				if (files.size() == 1)
				{
					report.Errors.push_back(fmt::format("'{}': {}", FileSystem::PathToUtf8(files.front().filename()), imported.GetError()));
					return;
				}
				for (std::filesystem::path const& file : files)
				{
					ImportAssetFiles(operations, std::span<std::filesystem::path const>(&file, 1), folder, report);
				}
			}

			// Visits the regular files and folders inside a directory, skipping hidden ones (names starting with '.') and links
			// like the asset scan does. The visitor returns false to stop; so does an unreadable directory (an error).
			template<typename Visitor>
			Result<void> VisitFolder(std::filesystem::path const& directory, Visitor const& visit)
			{
				std::error_code iterationError;
				std::filesystem::recursive_directory_iterator iterator(
					directory, std::filesystem::directory_options::skip_permission_denied, iterationError);
				for (std::filesystem::recursive_directory_iterator const end; !iterationError && iterator != end;
				     iterator.increment(iterationError))
				{
					std::filesystem::directory_entry const& entry = *iterator;
					std::error_code entryError;
					bool const link = entry.is_symlink(entryError);
					bool const isDirectory = !link && entry.is_directory(entryError);
					if (link || FileSystem::PathToUtf8(entry.path().filename()).starts_with('.'))
					{
						if (isDirectory)
						{
							iterator.disable_recursion_pending();
						}
						continue;
					}
					bool const isFile = !isDirectory && entry.is_regular_file(entryError);
					if ((isDirectory || isFile) && !visit(entry.path(), isDirectory))
					{
						return {};
					}
				}
				if (iterationError)
				{
					return MakeError("'{}' cannot be read: {}", FileSystem::PathToUtf8(directory), iterationError.message());
				}
				return {};
			}

			void ImportFolder(EditorOperations& operations, std::filesystem::path const& source, std::string_view assetFolder,
			                  Report& report)
			{
				std::filesystem::path const& assets = AssetManager::GetAssetDirectory();
				std::string const name = FileSystem::PathToUtf8(source.filename());
				if (FileSystem::IsInside(assets, source))
				{
					report.Errors.push_back(fmt::format("'{}' holds the project's assets and cannot be imported into them", name));
					return;
				}

				size_t fileCount = 0;
				std::vector<std::filesystem::path> assetFiles;
				Result<void> const listed = VisitFolder(source,
				                                        [&](std::filesystem::path const& path, bool isDirectory)
				                                        {
															if (!isDirectory)
															{
																fileCount++;
																if (IsAssetFile(path))
																{
																	assetFiles.push_back(path);
																}
															}
															return fileCount <= MaxFolderFiles;
														});
				if (!listed)
				{
					report.Errors.push_back(listed.GetError());
					return;
				}
				if (fileCount > MaxFolderFiles)
				{
					report.Errors.push_back(fmt::format("'{}' holds more than {} files: copy it into the project's Assets folder with the "
					                                    "file manager instead (the editor finds the files when it gets the focus back)",
					                                    name, MaxFolderFiles));
					return;
				}
				// A folder of the asset directory is registered where it is.
				if (FileSystem::IsInside(source, assets))
				{
					ImportAssetFiles(operations, assetFiles, assetFolder, report);
					return;
				}

				std::string const folder = AssetBrowsing::JoinPath(assetFolder, AssetBrowsing::MakeUniqueName(assetFolder, name, ""));
				std::filesystem::path const target = assets / FileSystem::PathFromUtf8(folder);
				if (Result<void> const created = FileSystem::CreateDirectories(target); !created)
				{
					report.Errors.push_back(fmt::format("'{}': {}", name, created.GetError()));
					return;
				}
				std::vector<std::filesystem::path> copied;
				bool failed = false;
				Result<void> const visited =
					VisitFolder(source,
				                [&](std::filesystem::path const& path, bool isDirectory)
				                {
									std::filesystem::path const destination = target / path.lexically_relative(source);
									Result<void> const done = isDirectory ? FileSystem::CreateDirectories(destination)
					                                                      : FileSystem::Copy(path, destination, false);
									if (!done)
									{
										report.Errors.push_back(fmt::format("'{}': {}", name, done.GetError()));
										failed = true;
										return false;
									}
									if (!isDirectory)
									{
										copied.push_back(destination);
									}
									return true;
								});
				if (!visited)
				{
					report.Errors.push_back(visited.GetError());
					failed = true;
				}
				if (failed)
				{
					// A folder comes in whole or not at all.
					(void)FileSystem::RemoveAll(target);
					return;
				}
				report.Folders.push_back(folder);
				std::vector<std::filesystem::path> copiedAssets;
				for (std::filesystem::path const& file : copied)
				{
					if (IsAssetFile(file))
					{
						copiedAssets.push_back(file);
					}
					else
					{
						report.KeptFiles.push_back(FileSystem::PathToUtf8(file.lexically_relative(assets)));
					}
				}
				ImportAssetFiles(operations, copiedAssets, folder, report);
			}
		}

		Report Import(EditorOperations& operations, EditorScripts* scripts, std::span<std::filesystem::path const> paths,
		              Destination const& destination)
		{
			Report report;
			if (!AssetManager::IsInitialized() || !AssetManager::HasAssetDirectory())
			{
				report.Errors.push_back("no project is open");
				return report;
			}
			std::vector<std::filesystem::path> assetFiles;
			std::vector<std::filesystem::path> scriptFiles;
			std::vector<std::filesystem::path> folders;
			for (std::filesystem::path const& dropped : paths)
			{
				// "C:/Pack/" names the folder Pack; a drive or file system root is never copied.
				std::filesystem::path const normal = dropped.lexically_normal();
				std::filesystem::path const path = normal.has_filename() ? normal : normal.parent_path();
				if (!path.has_relative_path())
				{
					report.Errors.push_back(fmt::format("'{}' is a drive and cannot be imported", FileSystem::PathToUtf8(dropped)));
					continue;
				}
				if (FileSystem::IsDirectory(path))
				{
					folders.push_back(path);
				}
				else if (!FileSystem::IsRegularFile(path))
				{
					report.Errors.push_back(fmt::format("'{}' is not a file or a folder", FileSystem::PathToUtf8(path)));
				}
				else if (ScriptBrowsing::IsScriptFile(path))
				{
					scriptFiles.push_back(path);
				}
				else if (IsAssetFile(path))
				{
					assetFiles.push_back(path);
				}
				else
				{
					report.Skipped.push_back(FileSystem::PathToUtf8(path.filename()));
				}
			}
			ImportAssetFiles(operations, assetFiles, destination.AssetFolder, report);
			for (std::filesystem::path const& folder : folders)
			{
				ImportFolder(operations, folder, destination.AssetFolder, report);
			}
			if (scriptFiles.empty())
			{
				return report;
			}
			if (scripts == nullptr)
			{
				report.Errors.push_back(fmt::format("{} cannot be added here: the project's scripts are not available",
				                                    CountOf(scriptFiles.size(), "C# source")));
				return report;
			}
			Result<std::vector<std::filesystem::path>> added = scripts->AddScripts(scriptFiles, destination.ScriptFolder);
			if (added)
			{
				report.Scripts = added.TakeValue();
			}
			else
			{
				report.Errors.push_back(added.GetError());
			}
			return report;
		}

		void Log(Report const& report)
		{
			for (std::string const& error : report.Errors)
			{
				ST_ERROR("Importing failed: {}", error);
			}
			if (!report.Skipped.empty())
			{
				std::string supported;
				for (std::string_view const extension : GetSupportedAssetExtensions())
				{
					supported += fmt::format("{} ", extension);
				}
				ST_WARN("Not imported, neither assets nor C# scripts: {} (assets are {}files; scripts .cs files)",
				        JoinNames(report.Skipped), supported);
			}
			if (!report.KeptFiles.empty())
			{
				size_t const listed = std::min(report.KeptFiles.size(), ListedKeptFiles);
				std::string names = JoinNames(std::span<std::string const>(report.KeptFiles.data(), listed));
				if (report.KeptFiles.size() > listed)
				{
					names += fmt::format(" and {} more", report.KeptFiles.size() - listed);
				}
				ST_INFO("Copied with the imported folders, without being assets: {}", names);
			}
			// "Imported 3 assets", "Imported 1 asset, 2 C# scripts and 1 folder".
			std::vector<std::string> parts;
			if (!report.Assets.empty())
			{
				parts.push_back(CountOf(report.Assets.size(), "asset"));
			}
			if (!report.Scripts.empty())
			{
				parts.push_back(CountOf(report.Scripts.size(), "C# script"));
			}
			if (!report.Folders.empty())
			{
				parts.push_back(CountOf(report.Folders.size(), "folder"));
			}
			if (parts.empty())
			{
				return;
			}
			std::string summary = parts.front();
			for (size_t index = 1; index < parts.size(); index++)
			{
				summary += (index + 1 == parts.size() ? " and " : ", ") + parts[index];
			}
			ST_INFO("Imported {}", summary);
		}
	}
}
