#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Project/ProjectCopy.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace Strada;

namespace
{
	constexpr std::string_view MarkedProject = "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
											   "\t<PropertyGroup />\n"
											   "\t<!-- strada:repository-only -->\n"
											   "\t<!-- The fallback. -->\n"
											   "\t<ItemGroup>\n"
											   "\t\t<ProjectReference Include=\"../../../Strada-ScriptCore/Strada.ScriptCore.csproj\" />\n"
											   "\t</ItemGroup>\n"
											   "\t<!-- /strada:repository-only -->\n"
											   "</Project>\n";

	constexpr std::string_view UnmarkedProject = "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
												 "\t<PropertyGroup />\n"
												 "</Project>\n";

	void Write(std::filesystem::path const& file, std::string_view text = "x")
	{
		REQUIRE(FileSystem::WriteTextFile(file, text).IsOk());
	}

	std::string Read(std::filesystem::path const& file)
	{
		Result<std::string> text = FileSystem::ReadTextFile(file);
		REQUIRE_MESSAGE(text.IsOk(), FileSystem::PathToUtf8(file));
		return text.TakeValue();
	}

	// Every file below a directory, relative with forward slashes.
	std::vector<std::string> ListFiles(std::filesystem::path const& directory)
	{
		std::vector<std::string> files;
		std::error_code error;
		for (auto it = std::filesystem::recursive_directory_iterator(directory, error);
		     !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
		{
			if (it->is_regular_file(error))
			{
				files.push_back(FileSystem::PathToUtf8(it->path().lexically_relative(directory)));
			}
		}
		REQUIRE_FALSE(error);
		std::sort(files.begin(), files.end());
		return files;
	}

	// A project with every kind of file the filter leaves out, and a script project with a repository-only block.
	std::filesystem::path MakeSource(std::filesystem::path const& root)
	{
		std::filesystem::path const source = root / "Source";
		Write(source / "Game.sproj", "{}");
		Write(source / "Assets" / "Scenes" / "Main.sscene", "scene");
		REQUIRE(FileSystem::CreateDirectories(source / "Assets" / "Empty").IsOk());
		Write(source / "Scripts" / "Game.csproj", MarkedProject);
		Write(source / "Scripts" / "Source" / "Game.cs", "class Game {}");
		Write(source / "Scripts" / "Source" / "__pycache__" / "x.pyc");
		Write(source / "Scripts" / "bin" / "Game.dll");
		Write(source / "Scripts" / "obj" / "Game.obj");
		Write(source / "Scripts" / "Binaries" / "Game.dll");
		Write(source / "Scripts" / "Strada.props");
		Write(source / "Tools" / "Author.py");
		Write(source / "Build" / "Game.exe");
		Write(source / ".strada" / "docs" / "README.md");
		Write(source / ".git" / "HEAD");
		Write(source / "__pycache__" / "y.pyc");
		// Only at the project's root are these folders the filter's.
		Write(source / "Assets" / "Tools" / "Hammer.smat");
		Write(source / "Assets" / "Build" / "Site.sscene");
		return source;
	}
}

TEST_CASE("ProjectCopy: the filter leaves out tools, build output, caches and machine-specific files")
{
	for (std::string_view const excluded :
	     {"Tools", "Tools/Author.py", "Build", "Build/Game.exe", ".strada/agents.json", ".git", "Scripts/bin", "Scripts/obj/x",
	      "Scripts/Binaries/Game.dll", "Scripts/Strada.props", "__pycache__", "Scripts/Source/__pycache__/a.pyc", "Assets/__pycache__"})
	{
		CAPTURE(excluded);
		CHECK(ProjectCopy::IsExcluded(excluded));
	}
	for (std::string_view const kept :
	     {"", "Game.sproj", "Assets", "Assets/Tools/Hammer.smat", "Assets/Build", "Scripts", "Scripts/Game.csproj",
	      "Scripts/Source/Game.cs", "Scripts/Strada.props.txt", "Scripts/binaries", "tools", "Assets/.git", "pycache", "Strada.props"})
	{
		CAPTURE(kept);
		CHECK_FALSE(ProjectCopy::IsExcluded(kept));
	}
}

TEST_CASE("ProjectCopy: script projects lose their repository-only blocks, keeping their line endings")
{
	std::vector<std::string> warnings;
	CHECK(ProjectCopy::RemoveRepositoryOnlyBlocks(MarkedProject, "Game.csproj", warnings) == UnmarkedProject);
	CHECK(warnings.empty());

	// Windows line endings, a byte order mark, a block on one line and a last line without an ending.
	std::string const windows = "\xEF\xBB\xBF<Project>\r\n"
								"<!-- strada:repository-only -->\r\n"
								"<A />\r\n"
								"<!-- /strada:repository-only -->\r\n"
								"<B /> <!-- strada:repository-only --> <!-- /strada:repository-only -->\r\n"
								"<C />\r\n"
								"</Project>";
	CHECK(ProjectCopy::RemoveRepositoryOnlyBlocks(windows, "Game.csproj", warnings) == "\xEF\xBB\xBF<Project>\r\n<C />\r\n</Project>");
	CHECK(warnings.empty());
	// The byte order mark stays when the first line goes.
	CHECK(ProjectCopy::RemoveRepositoryOnlyBlocks(
			  "\xEF\xBB\xBF<!-- strada:repository-only -->\n<A />\n<!-- /strada:repository-only -->\n<B />\n", "Game.csproj", warnings) ==
	      "\xEF\xBB\xBF<B />\n");
	// Text without markers is left as it is.
	CHECK(ProjectCopy::RemoveRepositoryOnlyBlocks(UnmarkedProject, "Game.csproj", warnings) == UnmarkedProject);
	CHECK(warnings.empty());

	// A block that is not closed: the whole text, with a warning naming the file.
	std::string const unclosed = "<Project>\n<!-- strada:repository-only -->\n<A />\n</Project>\n";
	CHECK(ProjectCopy::RemoveRepositoryOnlyBlocks(unclosed, "Scripts/Game.csproj", warnings) == unclosed);
	REQUIRE(warnings.size() == 1);
	CHECK(warnings[0].find("Scripts/Game.csproj") != std::string::npos);
	// A closing marker alone closes nothing.
	warnings.clear();
	std::string const closing = "<Project>\n<!-- /strada:repository-only -->\n</Project>\n";
	CHECK(ProjectCopy::RemoveRepositoryOnlyBlocks(closing, "Game.csproj", warnings) == closing);
	CHECK(warnings.empty());
}

TEST_CASE("ProjectCopy: a copy takes the project's files through the filter and reports what it wrote")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const source = MakeSource(directory.GetPath());
	std::filesystem::path const target = directory.GetPath() / "New" / "Copy";

	ProjectCopyProgress progress;
	ProjectCopyOptions options;
	options.Progress = &progress;
	// A folder of an excluded file's name is copied all the same.
	options.ExcludedFiles = {"Assets/Scenes/Main.sscene", "Assets/Tools", "Scripts"};
	Result<ProjectCopyResult> copied = ProjectCopy::Copy(source, target, options);
	REQUIRE_MESSAGE(copied.IsOk(), (copied ? std::string() : copied.GetError()));
	std::vector<std::string> const expected = {"Assets/Build/Site.sscene", "Assets/Tools/Hammer.smat", "Game.sproj", "Scripts/Game.csproj",
	                                           "Scripts/Source/Game.cs"};
	CHECK(ListFiles(target) == expected);
	CHECK(FileSystem::IsDirectory(target / "Assets" / "Empty"));
	CHECK(Read(target / "Scripts" / "Game.csproj") == UnmarkedProject);
	CHECK(copied.GetValue().Warnings.empty());
	CHECK(copied.GetValue().Files == expected.size());
	uint64_t bytes = 0;
	for (std::string const& file : expected)
	{
		bytes += *FileSystem::GetFileSize(target / FileSystem::PathFromUtf8(file));
	}
	CHECK(copied.GetValue().Bytes == bytes);
	CHECK(progress.Files.load() == expected.size());
	CHECK(progress.TotalFiles.load() == expected.size());
	CHECK(progress.Bytes.load() == bytes);
	// The totals are the source's sizes, before the script project lost its block.
	CHECK(progress.TotalBytes.load() == bytes + MarkedProject.size() - UnmarkedProject.size());
	// The source is unchanged.
	CHECK(Read(source / "Scripts" / "Game.csproj") == MarkedProject);

	// An empty directory takes a copy too.
	std::filesystem::path const empty = directory.GetPath() / "Empty";
	REQUIRE(FileSystem::CreateDirectories(empty).IsOk());
	CHECK(ProjectCopy::Copy(source, empty).IsOk());
	CHECK(FileSystem::Exists(empty / "Assets" / "Scenes" / "Main.sscene"));

	// An unclosed block is copied as it is, with a warning.
	std::filesystem::path const other = directory.GetPath() / "Unclosed";
	Write(other / "Scripts" / "Game.csproj", "<Project>\n<!-- strada:repository-only -->\n</Project>\n");
	Result<ProjectCopyResult> unclosed = ProjectCopy::Copy(other, directory.GetPath() / "UnclosedCopy");
	REQUIRE(unclosed.IsOk());
	CHECK(unclosed.GetValue().Warnings.size() == 1);
	CHECK(Read(directory.GetPath() / "UnclosedCopy" / "Scripts" / "Game.csproj") == Read(other / "Scripts" / "Game.csproj"));
}

TEST_CASE("ProjectCopy: targets that are not new or empty, and sources that are not directories, are refused")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const source = MakeSource(directory.GetPath());

	std::filesystem::path const taken = directory.GetPath() / "Taken";
	Write(taken / "Keep.txt", "keep");
	Result<ProjectCopyResult> notEmpty = ProjectCopy::Copy(source, taken);
	REQUIRE(notEmpty.IsError());
	CHECK(notEmpty.GetError().find("not empty") != std::string::npos);
	CHECK(ListFiles(taken) == std::vector<std::string>{"Keep.txt"});

	CHECK(ProjectCopy::Copy(source, taken / "Keep.txt").IsError());
	CHECK(Read(taken / "Keep.txt") == "keep");
	CHECK(ProjectCopy::Copy(directory.GetPath() / "Missing", directory.GetPath() / "Target").IsError());
	CHECK(ProjectCopy::Copy(taken / "Keep.txt", directory.GetPath() / "Target").IsError());
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "Target"));

	// Into itself, or around itself.
	CHECK(ProjectCopy::Copy(source, source / "Inside").IsError());
	CHECK_FALSE(FileSystem::Exists(source / "Inside"));
	std::filesystem::path const empty = directory.GetPath() / "Empty";
	REQUIRE(FileSystem::CreateDirectories(empty).IsOk());
	CHECK(ProjectCopy::Copy(empty, empty).IsError());
}

TEST_CASE("ProjectCopy: a cancelled copy leaves nothing behind")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const source = MakeSource(directory.GetPath());
	std::atomic<bool> cancel = true;
	ProjectCopyOptions options;
	options.Cancel = &cancel;

	std::filesystem::path const target = directory.GetPath() / "New" / "Deeper" / "Copy";
	Result<ProjectCopyResult> cancelled = ProjectCopy::Copy(source, target, options);
	REQUIRE(cancelled.IsError());
	CHECK(cancelled.GetError().find("cancelled") != std::string::npos);
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "New"));

	std::filesystem::path const empty = directory.GetPath() / "Empty";
	REQUIRE(FileSystem::CreateDirectories(empty).IsOk());
	CHECK(ProjectCopy::Copy(source, empty, options).IsError());
	CHECK(FileSystem::IsDirectory(empty));
	CHECK(ListFiles(empty).empty());
}

TEST_CASE("ProjectCopy: what was written into a new or empty directory goes unless it is committed")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const fresh = directory.GetPath() / "Fresh" / "Project";
	{
		NewDirectoryCleanup cleanup(fresh);
		Write(fresh / "Assets" / "File.txt");
	}
	// The folders above it that were new go too.
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "Fresh"));

	std::filesystem::path const empty = directory.GetPath() / "Empty";
	REQUIRE(FileSystem::CreateDirectories(empty).IsOk());
	{
		NewDirectoryCleanup cleanup(empty);
		Write(empty / "Assets" / "File.txt");
		Write(empty / "Game.sproj");
	}
	CHECK(FileSystem::IsDirectory(empty));
	CHECK(ListFiles(empty).empty());

	{
		NewDirectoryCleanup cleanup(fresh);
		Write(fresh / "Game.sproj");
		cleanup.Commit();
	}
	CHECK(FileSystem::Exists(fresh / "Game.sproj"));
}

TEST_CASE("ProjectCopy: symbolic links are left out, so nothing outside the source is copied")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const source = directory.GetPath() / "Source";
	std::filesystem::path const outside = directory.GetPath() / "Outside";
	Write(source / "Game.sproj", "{}");
	Write(outside / "Secret.txt", "secret");
	std::error_code folderError;
	std::filesystem::create_directory_symlink(outside, source / "Linked", folderError);
	std::error_code fileError;
	std::filesystem::create_symlink(outside / "Secret.txt", source / "Secret.txt", fileError);
	if (folderError && fileError)
	{
		// Windows makes links only for administrators or in developer mode.
		MESSAGE("symbolic links cannot be made here: " << folderError.message());
		return;
	}

	std::filesystem::path const target = directory.GetPath() / "Copy";
	Result<ProjectCopyResult> copied = ProjectCopy::Copy(source, target);
	REQUIRE_MESSAGE(copied.IsOk(), (copied ? std::string() : copied.GetError()));
	CHECK(ListFiles(target) == std::vector<std::string>{"Game.sproj"});
	CHECK_FALSE(FileSystem::Exists(target / "Linked"));
	CHECK_FALSE(FileSystem::Exists(target / "Secret.txt"));
	CHECK(copied.GetValue().Warnings.size() == static_cast<size_t>((folderError ? 0 : 1) + (fileError ? 0 : 1)));
	CHECK(ListFiles(outside) == std::vector<std::string>{"Secret.txt"});
}
