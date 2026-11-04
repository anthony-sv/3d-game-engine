#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace Strada;

// The documents that name the sample projects (AGENTS.md's repository map, README.md's layout and the scripting guide's
// table of Prism Break's graphs) name exactly what the repository holds.
namespace
{
	std::string ReadDocument(char const* path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(FileSystem::PathFromUtf8(path));
		REQUIRE_MESSAGE(text.IsOk(), path);
		std::string content = text.TakeValue();
		std::erase(content, '\r');
		return content;
	}

	// The names of the entries of a directory that are directories (folders) or regular files with the extension.
	std::set<std::string> ListEntries(std::filesystem::path const& directory, bool folders, std::string_view extension = {})
	{
		std::set<std::string> names;
		std::error_code error;
		for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(directory, error))
		{
			bool const matches = folders ? entry.is_directory(error)
			                             : entry.is_regular_file(error) && FileSystem::PathToUtf8(entry.path().extension()) == extension;
			if (matches)
			{
				names.insert(FileSystem::PathToUtf8(entry.path().filename()));
			}
		}
		REQUIRE_MESSAGE(!error, FileSystem::PathToUtf8(directory), ": ", error.message());
		return names;
	}

	// The lines of the document that start with the prefix, in order.
	std::vector<std::string_view> FindLines(std::string_view document, std::string_view prefix)
	{
		std::vector<std::string_view> lines;
		size_t start = 0;
		while (start < document.size())
		{
			size_t const end = std::min(document.find('\n', start), document.size());
			std::string_view const line = document.substr(start, end - start);
			if (line.starts_with(prefix))
			{
				lines.push_back(line);
			}
			start = end + 1;
		}
		return lines;
	}

	// The first cells of the rows of the first table after the heading whose header starts with the header given, each
	// without its backquotes.
	std::vector<std::string> ReadFirstColumn(std::string_view document, std::string_view heading, std::string_view header)
	{
		size_t const section = document.find("\n" + std::string(heading) + "\n");
		REQUIRE_MESSAGE(section != std::string_view::npos, "no heading ", std::string(heading));
		size_t const table = document.find("\n" + std::string(header), section);
		REQUIRE_MESSAGE(table != std::string_view::npos, "no table ", std::string(header), " after ", std::string(heading));
		std::vector<std::string> cells;
		// The header and its separator row come first.
		size_t start = document.find('\n', document.find('\n', table + 1) + 1) + 1;
		while (start < document.size() && document[start] == '|')
		{
			size_t const end = std::min(document.find('\n', start), document.size());
			std::string_view const row = document.substr(start, end - start);
			size_t const close = row.find('|', 1);
			REQUIRE(close != std::string_view::npos);
			std::string cell(row.substr(1, close - 1));
			std::erase(cell, '`');
			std::erase(cell, ' ');
			cells.push_back(std::move(cell));
			start = end + 1;
		}
		return cells;
	}
}

TEST_CASE("Project: AGENTS.md's repository map and README.md's layout name every folder of Projects")
{
	std::set<std::string> const projects = ListEntries(FileSystem::PathFromUtf8(STRADA_PROJECTS_DIR), true);
	REQUIRE_FALSE(projects.empty());
	std::string const agents = ReadDocument(STRADA_AGENTS_PATH);
	std::string const readme = ReadDocument(STRADA_README_PATH);
	// README.md's layout names the projects in its row of Projects/.
	std::vector<std::string_view> const layout = FindLines(readme, "| `Projects/` |");
	REQUIRE(layout.size() == 1);
	for (std::string const& project : projects)
	{
		CAPTURE(project);
		CHECK_MESSAGE(FindLines(agents, "| `Projects/" + project + "/` |").size() == 1,
		              "AGENTS.md's repository map has no row of the project");
		CHECK_MESSAGE(layout.front().find("`" + project + "`") != std::string_view::npos, "README.md's layout does not name the project");
	}
	// And the map has no row of a project that is not there.
	constexpr std::string_view RowStart = "| `Projects/";
	for (std::string_view const row : FindLines(agents, RowStart))
	{
		CAPTURE(row);
		std::string_view const path = row.substr(RowStart.size());
		size_t const end = path.find('/');
		REQUIRE(end != std::string_view::npos);
		CHECK(projects.contains(std::string(path.substr(0, end))));
	}
}

TEST_CASE("Project: the scripting guide's table of Prism Break's graphs names every graph of the project and nothing else")
{
	std::set<std::string> const graphs =
		ListEntries(FileSystem::PathFromUtf8(STRADA_PROJECTS_DIR) / "PrismBreak" / "Assets" / "Scripts", false, ".sgraph");
	REQUIRE(graphs.size() == 13);
	std::string const guide = ReadDocument(STRADA_SCRIPTING_GUIDE_PATH);
	std::vector<std::string> const named = ReadFirstColumn(guide, "### A complete game: Prism Break", "| Graph |");
	std::set<std::string> const listed(named.begin(), named.end());
	CHECK_MESSAGE(listed.size() == named.size(), "the table names a graph twice");
	CHECK(listed == graphs);
}
