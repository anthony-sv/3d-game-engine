#include "stpch.h"
#include "Strada/Project/ProjectCopy.h"

#include "Strada/Core/FileSystem.h"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace Strada
{
	namespace
	{
		std::filesystem::path MakeAbsolute(std::filesystem::path const& path)
		{
			std::error_code errorCode;
			std::filesystem::path const absolute = std::filesystem::absolute(path, errorCode);
			return errorCode ? path.lexically_normal() : absolute.lexically_normal();
		}

		// The names of a relative path with forward slashes.
		std::vector<std::string_view> SplitPath(std::string_view path)
		{
			std::vector<std::string_view> names;
			size_t start = 0;
			while (start <= path.size())
			{
				size_t const end = std::min(path.find('/', start), path.size());
				if (end > start)
				{
					names.push_back(path.substr(start, end - start));
				}
				start = end + 1;
			}
			return names;
		}

		bool EndsWithIgnoringCase(std::string_view text, std::string_view suffix)
		{
			if (text.size() < suffix.size())
			{
				return false;
			}
			return std::equal(suffix.begin(), suffix.end(), text.end() - static_cast<std::ptrdiff_t>(suffix.size()),
			                  [](char expected, char actual)
			                  {
								  auto const lower = [](char character)
								  {
									  return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
								  };
								  return lower(expected) == lower(actual);
							  });
		}

		bool IsScriptProject(std::filesystem::path const& file)
		{
			return EndsWithIgnoringCase(FileSystem::PathToUtf8(file.filename()), ".csproj");
		}

		bool IsCancelled(ProjectCopyOptions const& options)
		{
			return options.Cancel != nullptr && options.Cancel->load();
		}

		struct ListedFile
		{
			std::filesystem::path Source;
			std::filesystem::path Relative;
			uint64_t Size = 0;
		};

		struct Listing
		{
			std::vector<std::filesystem::path> Directories;
			std::vector<ListedFile> Files;
		};

		// What a copy takes from the source, without following links.
		Result<Listing> ListSource(std::filesystem::path const& source, ProjectCopyOptions const& options,
		                           std::vector<std::string>& warnings)
		{
			Listing listing;
			std::error_code errorCode;
			auto it = std::filesystem::recursive_directory_iterator(source, std::filesystem::directory_options::none, errorCode);
			for (; !errorCode && it != std::filesystem::recursive_directory_iterator(); it.increment(errorCode))
			{
				if (IsCancelled(options))
				{
					return Error{"the copy was cancelled"};
				}
				std::filesystem::path const& path = it->path();
				std::filesystem::path const relative = path.lexically_relative(source);
				std::string const name = FileSystem::PathToUtf8(relative);
				std::error_code statusError;
				std::filesystem::file_status const status = it->symlink_status(statusError);
				if (statusError)
				{
					return MakeError("'{}' cannot be read: {}", FileSystem::PathToUtf8(path), statusError.message());
				}
				if (std::filesystem::is_symlink(status))
				{
					// A link may lead out of the source: what it names is not copied.
					warnings.push_back(fmt::format("'{}' is a symbolic link and was not copied", name));
					it.disable_recursion_pending();
					continue;
				}
				bool const directory = std::filesystem::is_directory(status);
				// The options name files only: a folder of that name is copied.
				bool const excludedFile = !directory && std::ranges::find(options.ExcludedFiles, name) != options.ExcludedFiles.end();
				if (ProjectCopy::IsExcluded(name) || excludedFile)
				{
					if (directory)
					{
						it.disable_recursion_pending();
					}
					continue;
				}
				if (directory)
				{
					listing.Directories.push_back(relative);
				}
				else if (std::filesystem::is_regular_file(status))
				{
					std::error_code sizeError;
					uintmax_t const size = it->file_size(sizeError);
					listing.Files.push_back({path, relative, sizeError ? 0 : static_cast<uint64_t>(size)});
				}
			}
			if (errorCode)
			{
				return MakeError("'{}' cannot be listed: {}", FileSystem::PathToUtf8(source), errorCode.message());
			}
			return listing;
		}
	}

	bool ProjectCopy::IsExcluded(std::string_view relativePath)
	{
		std::vector<std::string_view> const names = SplitPath(relativePath);
		if (names.empty())
		{
			return false;
		}
		if (std::ranges::find(names, std::string_view("__pycache__")) != names.end())
		{
			return true;
		}
		constexpr std::array<std::string_view, 4> RootFolders = {"Tools", "Build", ".strada", ".git"};
		if (std::ranges::find(RootFolders, names.front()) != RootFolders.end())
		{
			return true;
		}
		constexpr std::array<std::string_view, 4> ScriptOutputs = {"bin", "obj", "Binaries", "Strada.props"};
		return names.size() >= 2 && names[0] == "Scripts" && std::ranges::find(ScriptOutputs, names[1]) != ScriptOutputs.end();
	}

	std::string ProjectCopy::RemoveRepositoryOnlyBlocks(std::string_view text, std::string_view fileName,
	                                                    std::vector<std::string>& warnings)
	{
		constexpr std::string_view ByteOrderMark = "\xEF\xBB\xBF";
		std::string kept;
		kept.reserve(text.size());
		bool inBlock = false;
		bool removed = false;
		size_t start = 0;
		while (start < text.size())
		{
			size_t const newline = text.find('\n', start);
			size_t const end = newline == std::string_view::npos ? text.size() : newline + 1;
			std::string_view const line = text.substr(start, end - start);
			start = end;
			if (inBlock)
			{
				inBlock = line.find(RepositoryOnlyEnd) == std::string_view::npos;
				continue;
			}
			size_t const begin = line.find(RepositoryOnlyBegin);
			if (begin != std::string_view::npos)
			{
				// A block may open and close on one line.
				inBlock = line.find(RepositoryOnlyEnd, begin + RepositoryOnlyBegin.size()) == std::string_view::npos;
				removed = true;
				continue;
			}
			kept += line;
		}
		if (inBlock)
		{
			warnings.push_back(fmt::format("'{}' opens a repository-only block that is not closed ('{}'): it was copied as it is", fileName,
			                               RepositoryOnlyEnd));
			return std::string(text);
		}
		if (removed && text.starts_with(ByteOrderMark) && !kept.starts_with(ByteOrderMark))
		{
			kept.insert(0, ByteOrderMark);
		}
		return kept;
	}

	Result<ProjectCopyResult> ProjectCopy::Copy(std::filesystem::path const& source, std::filesystem::path const& target,
	                                            ProjectCopyOptions const& options)
	{
		std::filesystem::path const from = MakeAbsolute(source);
		std::filesystem::path const to = MakeAbsolute(target);
		if (!FileSystem::IsDirectory(from))
		{
			return MakeError("'{}' is not a directory", FileSystem::PathToUtf8(from));
		}
		// Under any spelling the file system takes: one that ignores letter case finds the source under another case too.
		if (FileSystem::FindInside(to, from).has_value() || FileSystem::FindInside(from, to).has_value())
		{
			return MakeError("'{}' cannot be copied into '{}': one lies inside the other", FileSystem::PathToUtf8(from),
			                 FileSystem::PathToUtf8(to));
		}
		if (Result<void> available = CheckNewProjectDirectory(to); !available)
		{
			return Error{available.GetError()};
		}

		ProjectCopyResult result;
		Result<Listing> listed = ListSource(from, options, result.Warnings);
		if (!listed)
		{
			return Error{listed.GetError()};
		}
		Listing const& listing = listed.GetValue();
		if (options.Progress != nullptr)
		{
			uint64_t total = 0;
			for (ListedFile const& file : listing.Files)
			{
				total += file.Size;
			}
			options.Progress->TotalBytes = total;
			options.Progress->TotalFiles = static_cast<uint32_t>(listing.Files.size());
		}

		NewDirectoryCleanup cleanup(to);
		if (Result<void> created = FileSystem::CreateDirectories(to); !created)
		{
			return Error{created.GetError()};
		}
		// Empty folders are part of a project too (an asset folder made for later).
		for (std::filesystem::path const& directory : listing.Directories)
		{
			if (Result<void> created = FileSystem::CreateDirectories(to / directory); !created)
			{
				return Error{created.GetError()};
			}
		}
		for (ListedFile const& file : listing.Files)
		{
			if (IsCancelled(options))
			{
				return Error{"the copy was cancelled"};
			}
			std::filesystem::path const destination = to / file.Relative;
			uint64_t written = file.Size;
			if (IsScriptProject(file.Source))
			{
				// Read as bytes: ReadTextFile drops a byte order mark, which the copy keeps.
				Result<Buffer> bytes = FileSystem::ReadBinaryFile(file.Source);
				if (!bytes)
				{
					return Error{bytes.GetError()};
				}
				std::string_view const text(bytes.GetValue().As<char>(), static_cast<size_t>(bytes.GetValue().GetSize()));
				std::string const filtered = RemoveRepositoryOnlyBlocks(text, FileSystem::PathToUtf8(file.Relative), result.Warnings);
				if (Result<void> saved = FileSystem::WriteTextFile(destination, filtered); !saved)
				{
					return Error{saved.GetError()};
				}
				written = filtered.size();
			}
			else if (Result<void> copied = FileSystem::Copy(file.Source, destination, false); !copied)
			{
				return Error{copied.GetError()};
			}
			result.Bytes += written;
			result.Files++;
			if (options.Progress != nullptr)
			{
				options.Progress->Bytes += written;
				options.Progress->Files++;
			}
		}
		if (IsCancelled(options))
		{
			return Error{"the copy was cancelled"};
		}
		cleanup.Commit();
		return result;
	}

	NewDirectoryCleanup::NewDirectoryCleanup(std::filesystem::path const& directory)
		: m_Directory(MakeAbsolute(directory)),
		  m_Existed(FileSystem::Exists(m_Directory))
	{
		// Of a new directory, the folders above it that are new too go with it.
		if (!m_Existed)
		{
			m_NewRoot = m_Directory;
			while (m_NewRoot.has_parent_path() && m_NewRoot.parent_path() != m_NewRoot && !FileSystem::Exists(m_NewRoot.parent_path()))
			{
				m_NewRoot = m_NewRoot.parent_path();
			}
		}
	}

	NewDirectoryCleanup::~NewDirectoryCleanup()
	{
		if (m_Committed)
		{
			return;
		}
		if (!m_Existed)
		{
			(void)FileSystem::RemoveAll(m_NewRoot);
			return;
		}
		std::error_code errorCode;
		for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(m_Directory, errorCode))
		{
			(void)FileSystem::RemoveAll(entry.path());
		}
	}

	Result<void> CheckNewProjectDirectory(std::filesystem::path const& directory)
	{
		if (!FileSystem::Exists(directory))
		{
			return {};
		}
		if (!FileSystem::IsDirectory(directory))
		{
			return MakeError("'{}' is not a directory", FileSystem::PathToUtf8(directory));
		}
		std::error_code errorCode;
		if (!std::filesystem::is_empty(directory, errorCode) || errorCode)
		{
			return MakeError("'{}' is not empty; projects are created in empty or new directories", FileSystem::PathToUtf8(directory));
		}
		return {};
	}
}
