#include "Editor/ProjectImport.h"

#include "Editor/AssetBrowsing.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/FileCopies.h"
#include "Editor/ScriptBrowsing.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MeshImporter.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ScriptProject.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <system_error>

namespace Strada
{
	namespace ProjectImport
	{
		namespace
		{
			// Kept files listed by name in the log.
			constexpr size_t ListedKeptFiles = 5;

			AssetType GetFileAssetType(std::filesystem::path const& path)
			{
				return GetAssetTypeForExtension(FileSystem::PathToUtf8(path.extension()));
			}

			bool IsAssetFile(std::filesystem::path const& path)
			{
				return GetFileAssetType(path) != AssetType::None;
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

			// A path inside a directory, relative to it with forward slashes.
			std::string GetRelativePath(std::filesystem::path const& path, std::filesystem::path const& base)
			{
				return FileSystem::PathToUtf8(path.lexically_relative(base));
			}

			bool Contains(std::span<std::filesystem::path const> paths, std::filesystem::path const& path)
			{
				return std::find(paths.begin(), paths.end(), path) != paths.end();
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

			// Registers the assets among files copied into a folder of the asset directory; the others are kept files.
			void RegisterCopies(EditorOperations& operations, std::span<std::filesystem::path const> copies, std::string_view folder,
			                    Report& report)
			{
				std::vector<std::filesystem::path> assets;
				for (std::filesystem::path const& copy : copies)
				{
					if (IsAssetFile(copy))
					{
						assets.push_back(copy);
					}
					else
					{
						report.KeptFiles.push_back(GetRelativePath(copy, AssetManager::GetAssetDirectory()));
					}
				}
				ImportAssetFiles(operations, assets, folder, report);
			}

			// The build output of a C# project (bin and obj beside its .csproj file): never sources, and its generated files
			// would declare the scripts' assembly attributes twice.
			bool IsBuildOutputDirectory(std::filesystem::path const& directory)
			{
				std::string const name = FileSystem::PathToUtf8(directory.filename());
				if (name != "bin" && name != "obj")
				{
					return false;
				}
				std::error_code error;
				for (std::filesystem::directory_iterator iterator(directory.parent_path(), error), end; !error && iterator != end;
				     iterator.increment(error))
				{
					std::error_code entryError;
					if (FileSystem::PathToUtf8(iterator->path().extension()) == ".csproj" && iterator->is_regular_file(entryError))
					{
						return true;
					}
				}
				return false;
			}

			// Visits the regular files and folders inside a directory, skipping hidden ones (names starting with '.') and links
			// like the asset scan does, and the build output of C# projects. The visitor returns false to stop; so does an
			// unreadable directory (an error).
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
					if (link || FileSystem::PathToUtf8(entry.path().filename()).starts_with('.') ||
					    (isDirectory && IsBuildOutputDirectory(entry.path())))
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

			// The files of a dropped folder, relative to it with forward slashes, by what they are.
			struct FolderFiles
			{
				std::vector<std::string> Assets;
				std::vector<std::string> Scripts;
				std::vector<std::string> Others;
			};

			Result<FolderFiles> ListFolder(std::filesystem::path const& folder, size_t maxFiles)
			{
				FolderFiles files;
				size_t count = 0;
				Result<void> const listed = VisitFolder(folder,
				                                        [&](std::filesystem::path const& path, bool isDirectory)
				                                        {
															if (isDirectory)
															{
																return true;
															}
															if (++count > maxFiles)
															{
																return false;
															}
															std::string relative = GetRelativePath(path, folder);
															if (ScriptBrowsing::IsScriptFile(path))
															{
																files.Scripts.push_back(std::move(relative));
															}
															else if (IsAssetFile(path))
															{
																files.Assets.push_back(std::move(relative));
															}
															else
															{
																files.Others.push_back(std::move(relative));
															}
															return true;
														});
				if (!listed)
				{
					return Error{listed.GetError()};
				}
				if (count > maxFiles)
				{
					return MakeError("'{}' holds more than {} files: copy it into the project with the file manager instead (the editor "
					                 "finds the files when it gets the focus back)",
					                 FileSystem::PathToUtf8(folder.filename()), maxFiles);
				}
				return files;
			}

			// Copies files of a folder, given relative to it, to the same paths in another one.
			Result<void> CopyFiles(FileCopies& copies, std::filesystem::path const& from, std::filesystem::path const& to,
			                       std::span<std::string const> files)
			{
				for (std::string const& file : files)
				{
					std::filesystem::path const relative = FileSystem::PathFromUtf8(file);
					if (Result<void> copied = copies.Copy(from / relative, to / relative); !copied)
					{
						return copied;
					}
				}
				return {};
			}

			// The directory of the script folder C# sources go to, or why there is none.
			Result<std::filesystem::path> GetScriptFolder(EditorOperations& operations, EditorScripts const* scripts,
			                                              Destination const& destination)
			{
				Project const* const project = operations.GetContext().GetProject();
				if (scripts == nullptr || project == nullptr)
				{
					return Error{"the project's scripts are not available"};
				}
				return ScriptProject::GetSourceFolder(*project, destination.ScriptFolder);
			}

			// Copies files of a dropped folder (relative to it) into a new folder of the scripts named after it, in the script
			// folder of the destination. Returns the new folder, relative to the scripts directory.
			Result<std::string> AddScriptFolder(EditorScripts& scripts, std::filesystem::path const& scriptFolder,
			                                    Destination const& destination, std::filesystem::path const& source,
			                                    std::span<std::string const> files, Report& report)
			{
				std::string const name = AssetBrowsing::MakeUniqueFileName(scriptFolder, FileSystem::PathToUtf8(source.filename()), "");
				std::vector<ScriptFileCopy> copies;
				copies.reserve(files.size());
				for (std::string const& file : files)
				{
					copies.push_back({source / FileSystem::PathFromUtf8(file), AssetBrowsing::JoinPath(name, file)});
				}
				Result<std::vector<std::filesystem::path>> added = scripts.AddScriptFiles(copies, destination.ScriptFolder);
				if (!added)
				{
					return Error{added.GetError()};
				}
				for (std::filesystem::path const& copy : added.GetValue())
				{
					if (ScriptBrowsing::IsScriptFile(copy))
					{
						report.Scripts.push_back(copy);
					}
				}
				std::string folder = AssetBrowsing::JoinPath(destination.ScriptFolder, name);
				report.ScriptFolders.push_back(folder);
				return folder;
			}

			void ImportFolder(EditorOperations& operations, EditorScripts* scripts, std::filesystem::path const& source,
			                  Destination const& destination, size_t maxFiles, Report& report)
			{
				std::filesystem::path const& assets = AssetManager::GetAssetDirectory();
				std::string const name = FileSystem::PathToUtf8(source.filename());
				if (FileSystem::IsInside(assets, source))
				{
					report.Errors.push_back(fmt::format("'{}' holds the project's assets and cannot be imported into them", name));
					return;
				}
				Project const* const project = operations.GetContext().GetProject();
				std::filesystem::path const scriptsDirectory =
					project != nullptr ? project->GetDirectory() / ScriptProject::DirectoryName : std::filesystem::path();
				if (!scriptsDirectory.empty() && FileSystem::IsInside(scriptsDirectory, source))
				{
					report.Errors.push_back(fmt::format("'{}' holds the project's scripts and cannot be imported into them", name));
					return;
				}
				Result<FolderFiles> const listed = ListFolder(source, maxFiles);
				if (!listed)
				{
					report.Errors.push_back(listed.GetError());
					return;
				}
				FolderFiles const& files = listed.GetValue();

				// What is in the project already stays where it is.
				if (FileSystem::IsInside(source, assets))
				{
					std::vector<std::filesystem::path> registered;
					for (std::string const& file : files.Assets)
					{
						registered.push_back(source / FileSystem::PathFromUtf8(file));
					}
					ImportAssetFiles(operations, registered, destination.AssetFolder, report);
					return;
				}
				if (!scriptsDirectory.empty() && FileSystem::IsInside(source, scriptsDirectory))
				{
					for (std::string const& file : files.Scripts)
					{
						report.Scripts.push_back(source / FileSystem::PathFromUtf8(file));
					}
					return;
				}

				std::optional<std::filesystem::path> scriptFolder;
				if (!files.Scripts.empty())
				{
					Result<std::filesystem::path> folder = GetScriptFolder(operations, scripts, destination);
					if (!folder)
					{
						report.Errors.push_back(fmt::format("'{}' holds C# sources, which cannot be added: {}", name, folder.GetError()));
						return;
					}
					scriptFolder = folder.TakeValue();
				}

				// A folder of C# sources (an AI folder, a library) belongs to the scripts with the other files it holds.
				if (scriptFolder && files.Assets.empty())
				{
					std::vector<std::string> all = files.Scripts;
					all.insert(all.end(), files.Others.begin(), files.Others.end());
					if (Result<std::string> const added = AddScriptFolder(*scripts, *scriptFolder, destination, source, all, report);
					    !added)
					{
						report.Errors.push_back(fmt::format("'{}': {}", name, added.GetError()));
					}
					return;
				}

				// Else assets and other files go into a new folder of the asset directory, and C# sources into one of the scripts.
				std::string const folder =
					AssetBrowsing::JoinPath(destination.AssetFolder, AssetBrowsing::MakeUniqueName(destination.AssetFolder, name, ""));
				std::filesystem::path const target = assets / FileSystem::PathFromUtf8(folder);
				FileCopies copies;
				Result<void> copied = copies.CreateFolder(target);
				if (copied)
				{
					copied = CopyFiles(copies, source, target, files.Assets);
				}
				if (copied)
				{
					copied = CopyFiles(copies, source, target, files.Others);
				}
				if (copied && scriptFolder)
				{
					Result<std::string> const added = AddScriptFolder(*scripts, *scriptFolder, destination, source, files.Scripts, report);
					if (added)
					{
						report.Notes.push_back(fmt::format(
							"The C# sources of '{}' went to the scripts, {}/{}", name,
							GetRelativePath(ScriptProject::GetSourceDirectory(*project), project->GetDirectory()), added.GetValue()));
					}
					else
					{
						copied = Error{added.GetError()};
					}
				}
				if (!copied)
				{
					// A folder comes in whole or not at all.
					copies.Rollback();
					report.Errors.push_back(fmt::format("'{}': {}", name, copied.GetError()));
					return;
				}
				report.Folders.push_back(folder);
				RegisterCopies(operations, copies.GetCopies(), folder, report);
			}

			// Models dropped as files with the files they read; dropped folders holding some of those files come whole.
			struct ModelGroup
			{
				std::vector<std::filesystem::path> Models;
				// The models, the files they read and those folders, absolute and lexically normal.
				std::vector<std::filesystem::path> Members;
			};

			// The innermost directory holding every model.
			std::filesystem::path GetCommonDirectory(std::span<std::filesystem::path const> models)
			{
				std::filesystem::path common = models.front().parent_path();
				for (std::filesystem::path const& model : models.subspan(1))
				{
					while (!FileSystem::IsInside(model, common) && common.has_relative_path())
					{
						common = common.parent_path();
					}
				}
				return common;
			}

			// Finds the files the dropped models outside the project read and groups them with the models (models reading
			// the same files are one group); the files and folders the groups hold are taken out of the other lists.
			std::vector<ModelGroup> GroupModels(std::vector<std::filesystem::path>& assetFiles,
			                                    std::vector<std::filesystem::path>& otherFiles, std::vector<std::filesystem::path>& folders)
			{
				std::vector<ModelGroup> groups;
				for (std::filesystem::path const& file : assetFiles)
				{
					if (GetFileAssetType(file) != AssetType::Mesh || FileSystem::IsInside(file, AssetManager::GetAssetDirectory()))
					{
						continue;
					}
					// A model that cannot be read comes alone; its asset says why it does not load.
					Result<std::vector<std::filesystem::path>> const referenced = MeshImporter::FindExternalFiles(file);
					if (!referenced || referenced.GetValue().empty())
					{
						continue;
					}
					ModelGroup group;
					group.Models.push_back(file);
					group.Members.push_back(file);
					for (std::filesystem::path const& reference : referenced.GetValue())
					{
						// C# sources never go to the assets.
						if (ScriptBrowsing::IsScriptFile(reference))
						{
							continue;
						}
						auto const holder = std::find_if(folders.begin(), folders.end(),
						                                 [&reference](std::filesystem::path const& folder)
						                                 {
															 return FileSystem::IsInside(reference, folder);
														 });
						std::filesystem::path const member = holder != folders.end() ? *holder : reference;
						if (!Contains(group.Members, member))
						{
							group.Members.push_back(member);
						}
					}
					groups.push_back(std::move(group));
				}

				for (size_t first = 0; first < groups.size(); first++)
				{
					for (size_t second = first + 1; second < groups.size();)
					{
						ModelGroup& into = groups[first];
						ModelGroup& from = groups[second];
						bool const shared = std::any_of(from.Members.begin(), from.Members.end(),
						                                [&into](std::filesystem::path const& member)
						                                {
															return Contains(into.Members, member);
														});
						if (!shared)
						{
							second++;
							continue;
						}
						into.Models.insert(into.Models.end(), from.Models.begin(), from.Models.end());
						for (std::filesystem::path const& member : from.Members)
						{
							if (!Contains(into.Members, member))
							{
								into.Members.push_back(member);
							}
						}
						groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(second));
						// The grown group may share files with groups it was compared with before.
						second = first + 1;
					}
				}

				auto const grouped = [&groups](std::filesystem::path const& path)
				{
					return std::any_of(groups.begin(), groups.end(),
					                   [&path](ModelGroup const& group)
					                   {
										   return Contains(group.Members, path);
									   });
				};
				std::erase_if(assetFiles, grouped);
				std::erase_if(otherFiles, grouped);
				std::erase_if(folders, grouped);
				return groups;
			}

			void ImportModelGroup(EditorOperations& operations, EditorScripts* scripts, ModelGroup const& group,
			                      Destination const& destination, size_t maxFiles, Report& report)
			{
				std::filesystem::path const& assets = AssetManager::GetAssetDirectory();
				std::filesystem::path const base = GetCommonDirectory(group.Models);
				std::string const modelName = FileSystem::PathToUtf8(group.Models.front().filename());
				// The files keep their paths relative to the models, so the models find them. When one of those paths is taken,
				// everything goes into a new folder named after the (first) model.
				bool const taken = std::any_of(
					group.Members.begin(), group.Members.end(),
					[&](std::filesystem::path const& member)
					{
						std::string const path = AssetBrowsing::JoinPath(destination.AssetFolder, GetRelativePath(member, base));
						return FileSystem::Exists(assets / FileSystem::PathFromUtf8(path)) || AssetManager::FindByPath(path).IsValid();
					});
				std::string const folder =
					taken ? AssetBrowsing::JoinPath(destination.AssetFolder,
				                                    AssetBrowsing::MakeUniqueName(destination.AssetFolder,
				                                                                  FileSystem::PathToUtf8(group.Models.front().stem()), ""))
						  : destination.AssetFolder;
				std::filesystem::path const target = assets / FileSystem::PathFromUtf8(folder);

				FileCopies copies;
				std::vector<ScriptFileCopy> scriptCopies;
				Result<void> copied = copies.CreateFolder(target);
				for (std::filesystem::path const& member : group.Members)
				{
					if (!copied)
					{
						break;
					}
					std::string const relative = GetRelativePath(member, base);
					std::filesystem::path const memberTarget = target / FileSystem::PathFromUtf8(relative);
					if (!FileSystem::IsDirectory(member))
					{
						copied = copies.Copy(member, memberTarget);
						continue;
					}
					// A dropped folder holding files the models read comes whole.
					Result<FolderFiles> const listed = ListFolder(member, maxFiles);
					if (!listed)
					{
						copied = Error{listed.GetError()};
						break;
					}
					copied = copies.CreateFolder(memberTarget);
					if (copied)
					{
						copied = CopyFiles(copies, member, memberTarget, listed.GetValue().Assets);
					}
					if (copied)
					{
						copied = CopyFiles(copies, member, memberTarget, listed.GetValue().Others);
					}
					for (std::string const& script : listed.GetValue().Scripts)
					{
						scriptCopies.push_back({member / FileSystem::PathFromUtf8(script), AssetBrowsing::JoinPath(relative, script)});
					}
				}
				if (copied && !scriptCopies.empty())
				{
					Result<std::filesystem::path> const scriptFolder = GetScriptFolder(operations, scripts, destination);
					Result<std::vector<std::filesystem::path>> added =
						scriptFolder ? scripts->AddScriptFiles(scriptCopies, destination.ScriptFolder)
									 : Result<std::vector<std::filesystem::path>>(Error{scriptFolder.GetError()});
					if (added)
					{
						report.Scripts.insert(report.Scripts.end(), added.GetValue().begin(), added.GetValue().end());
					}
					else
					{
						copied = Error{added.GetError()};
					}
				}
				if (!copied)
				{
					copies.Rollback();
					report.Errors.push_back(fmt::format("'{}': {}", modelName, copied.GetError()));
					return;
				}
				if (taken)
				{
					report.Folders.push_back(folder);
					report.Notes.push_back(
						fmt::format("'{}' went into the new folder '{}' with the files it reads, as files of their names "
					                "exist where it was dropped",
					                modelName, folder));
				}
				RegisterCopies(operations, copies.GetCopies(), folder, report);
			}

			// Files that are neither assets nor C# sources, dropped with models: copied beside them, numbered when taken.
			void KeepFiles(std::span<std::filesystem::path const> files, std::string_view folder, Report& report)
			{
				std::filesystem::path const& assets = AssetManager::GetAssetDirectory();
				for (std::filesystem::path const& file : files)
				{
					std::string const path =
						AssetBrowsing::JoinPath(folder, AssetBrowsing::MakeUniqueName(folder, FileSystem::PathToUtf8(file.stem()),
					                                                                  FileSystem::PathToUtf8(file.extension())));
					FileCopies copies;
					if (Result<void> copied = copies.Copy(file, assets / FileSystem::PathFromUtf8(path)); !copied)
					{
						copies.Rollback();
						report.Errors.push_back(fmt::format("'{}': {}", FileSystem::PathToUtf8(file.filename()), copied.GetError()));
						continue;
					}
					report.KeptFiles.push_back(path);
				}
			}
		}

		Report Import(EditorOperations& operations, EditorScripts* scripts, std::span<std::filesystem::path const> paths,
		              Destination const& destination, size_t maxFolderFiles)
		{
			Report report;
			if (!AssetManager::IsInitialized() || !AssetManager::HasAssetDirectory())
			{
				report.Errors.push_back("no project is open");
				return report;
			}
			std::vector<std::filesystem::path> dropped;
			for (std::filesystem::path const& path : paths)
			{
				// "C:/Pack/" names the folder Pack; a drive or file system root is never copied.
				std::filesystem::path const normal = path.lexically_normal();
				std::filesystem::path const file = normal.has_filename() ? normal : normal.parent_path();
				if (!file.has_relative_path())
				{
					report.Errors.push_back(fmt::format("'{}' is a drive and cannot be imported", FileSystem::PathToUtf8(path)));
					continue;
				}
				if (!Contains(dropped, file))
				{
					dropped.push_back(file);
				}
			}

			std::vector<std::filesystem::path> folders;
			std::vector<std::filesystem::path> scriptFiles;
			std::vector<std::filesystem::path> assetFiles;
			std::vector<std::filesystem::path> otherFiles;
			for (std::filesystem::path const& path : dropped)
			{
				// What lies inside a dropped folder comes with it.
				bool const inFolder =
					std::any_of(dropped.begin(), dropped.end(),
				                [&path](std::filesystem::path const& other)
				                {
									return other != path && FileSystem::IsInside(path, other) && FileSystem::IsDirectory(other);
								});
				if (inFolder)
				{
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
				// Other files of the project are in it already.
				else if (!FileSystem::IsInside(path, AssetManager::GetAssetDirectory()))
				{
					otherFiles.push_back(path);
				}
			}

			bool const withModels = std::any_of(assetFiles.begin(), assetFiles.end(),
			                                    [](std::filesystem::path const& file)
			                                    {
													return GetFileAssetType(file) == AssetType::Mesh;
												});
			for (ModelGroup const& group : GroupModels(assetFiles, otherFiles, folders))
			{
				ImportModelGroup(operations, scripts, group, destination, maxFolderFiles, report);
			}
			ImportAssetFiles(operations, assetFiles, destination.AssetFolder, report);
			if (withModels)
			{
				KeepFiles(otherFiles, destination.AssetFolder, report);
			}
			else
			{
				for (std::filesystem::path const& file : otherFiles)
				{
					report.Skipped.push_back(FileSystem::PathToUtf8(file.filename()));
				}
			}
			for (std::filesystem::path const& folder : folders)
			{
				ImportFolder(operations, scripts, folder, destination, maxFolderFiles, report);
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
				report.Scripts.insert(report.Scripts.end(), added.GetValue().begin(), added.GetValue().end());
			}
			else
			{
				report.Errors.push_back(added.GetError());
			}
			return report;
		}

		std::string DescribeImportableFiles()
		{
			// Upper-case names without their dots, which word wrapping would break after (".tt" | "f").
			std::vector<std::string> names;
			for (std::string_view const extension : GetSupportedAssetExtensions())
			{
				std::string name(extension.starts_with('.') ? extension.substr(1) : extension);
				std::transform(name.begin(), name.end(), name.begin(),
				               [](unsigned char character)
				               {
								   return static_cast<char>(std::toupper(character));
							   });
				names.push_back(std::move(name));
			}
			std::string list;
			for (size_t index = 0; index < names.size(); index++)
			{
				list += index == 0 ? "" : (index + 1 == names.size() ? " and " : ", ");
				list += names[index];
			}
			return fmt::format("Strada imports {} files as assets, and CS files as C# scripts.", list);
		}

		std::string DescribeSkipped(Report const& report)
		{
			if (report.Skipped.empty())
			{
				return {};
			}
			return fmt::format("Not imported, neither assets nor C# scripts: {}. {}", JoinNames(report.Skipped), DescribeImportableFiles());
		}

		void Log(Report const& report)
		{
			for (std::string const& error : report.Errors)
			{
				ST_ERROR("Importing failed: {}", error);
			}
			if (!report.Skipped.empty())
			{
				ST_WARN("{}", DescribeSkipped(report));
			}
			if (!report.KeptFiles.empty())
			{
				size_t const listed = std::min(report.KeptFiles.size(), ListedKeptFiles);
				std::string names = JoinNames(std::span<std::string const>(report.KeptFiles.data(), listed));
				if (report.KeptFiles.size() > listed)
				{
					names += fmt::format(" and {} more", report.KeptFiles.size() - listed);
				}
				ST_INFO("Copied beside the assets, without being assets: {}", names);
			}
			for (std::string const& note : report.Notes)
			{
				ST_INFO("{}", note);
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
			if (size_t const folders = report.Folders.size() + report.ScriptFolders.size(); folders > 0)
			{
				parts.push_back(CountOf(folders, "folder"));
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
