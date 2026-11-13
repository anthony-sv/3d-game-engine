#include "EditorLocationsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/EditorOperations.h"

#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace Strada;

// project.create replaces the open project, so the automation fuzz leaves it out (AutomationFuzzTests.cpp): its template,
// gitIgnore and agents parameters get their damaged values here.
namespace
{
	// Every file and folder below a directory with its size, to tell what a command wrote.
	std::map<std::filesystem::path, uintmax_t> Snapshot(std::filesystem::path const& directory)
	{
		std::map<std::filesystem::path, uintmax_t> entries;
		std::error_code error;
		for (auto it = std::filesystem::recursive_directory_iterator(directory, error);
		     !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
		{
			std::error_code sizeError;
			entries[it->path()] = it->is_regular_file(sizeError) ? it->file_size(sizeError) : 0;
		}
		REQUIRE_FALSE(error);
		return entries;
	}

	struct CreateFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		CommandRegistry Registry;

		CreateFixture()
		{
			EditorCommandEnvironment environment;
			environment.Locations = Testing::MakeEditorLocations(Directory.GetPath());
			REQUIRE(RegisterEditorCommands(Registry, Operations, std::move(environment)).IsOk());
		}

		CommandResult Run(Json const& params)
		{
			std::optional<CommandResult> result;
			Registry.Execute("project.create", params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			return std::move(*result);
		}
	};
}

TEST_CASE("ProjectCreateParameters: damaged template, gitIgnore and agents values are refused without writing anything")
{
	CreateFixture fixture;
	std::string const directory = FileSystem::PathToUtf8(fixture.Directory.GetPath() / "Game");
	auto const content = Snapshot(Testing::GetTestContentDirectory());
	auto const before = Snapshot(fixture.Directory.GetPath());

	std::vector<std::pair<std::string, Json>> const damaged = {
		{"template", 7},
		{"template", true},
		{"template", Json()},
		{"template", Json::array({"empty-3d"})},
		{"template", Json::object({{"id", "empty-3d"}})},
		{"template", ""},
		{"template", "nope"},
		{"template", "Empty-3D"},
		{"template", " empty-3d"},
		{"template", std::string(300, 'a')},
		{"template", std::string(100000, 'b')},
		{"template", "../Templates/empty-3d"},
		{"template", "empty-3d/.."},
		{"template", "tiny-project/../empty-3d"},
		{"template", "Templates\\empty-3d"},
		{"template", "C:/empty-3d"},
		{"template", "/empty-3d"},
		{"template", "CON"},
		{"template", "nul"},
		{"template", "Tools"},
		{"template", std::string("empty-3d\0x", 10)},
		{"template", "名前"},
		{"gitIgnore", "yes"},
		{"gitIgnore", 1},
		{"gitIgnore", Json()},
		{"gitIgnore", Json::array()},
		{"agents", "yes"},
		{"agents", 0},
		{"agents", Json()},
		{"agents", Json::object()},
		{"agents", Json::array({true})},
	};
	for (auto const& [parameter, value] : damaged)
	{
		CAPTURE(parameter);
		CAPTURE(value.dump(-1, ' ', true).substr(0, 80));
		Json params = Json::object({{"directory", directory}, {"name", "Game"}});
		params[parameter] = value;
		CommandResult result = fixture.Run(params);
		REQUIRE(result.IsError());
		CHECK(result.GetError().Code == AutomationErrorCode::InvalidParams);
		CHECK(Snapshot(fixture.Directory.GetPath()) == before);
		CHECK(fixture.Context.GetProject() == nullptr);
	}
	CHECK(Snapshot(Testing::GetTestContentDirectory()) == content);
}

TEST_CASE("ProjectCreateParameters: names and directories with separators and reserved names never fail inside the editor")
{
	CreateFixture fixture;
	auto const content = Snapshot(Testing::GetTestContentDirectory());
	std::vector<std::string> const names = {"CON", "nul", "a/b", "..", "Game\\..\\x", "名前", std::string(300, 'n'), "  ", "Line\nBreak"};
	int index = 0;
	for (std::string const& name : names)
	{
		for (char const* const templateId : {"empty-3d", "tiny-project"})
		{
			CAPTURE(name);
			CAPTURE(templateId);
			std::filesystem::path const target = fixture.Directory.GetPath() / ("Project" + std::to_string(index++));
			CommandResult result = fixture.Run(Json::object(
				{{"directory", FileSystem::PathToUtf8(target)}, {"name", name}, {"template", templateId}, {"discardChanges", true}}));
			if (result.IsError())
			{
				CHECK((result.GetError().Code == AutomationErrorCode::InvalidParams ||
				       result.GetError().Code == AutomationErrorCode::FileError));
				// A refused project leaves nothing behind.
				CHECK_FALSE(FileSystem::Exists(target));
				continue;
			}
			// The project file lies in the directory, under a name every platform can store.
			std::filesystem::path const file = FileSystem::PathFromUtf8(result.GetValue()["project"]["file"].get<std::string>());
			CHECK(FileSystem::IsInside(file, target));
			CHECK(file.parent_path().lexically_normal() == target.lexically_normal());
			CHECK(result.GetValue()["project"]["name"] == name);
			// Set up for coding agents (the default) under any name: the kit wrote its files in the directory.
			Json const& created = result.GetValue();
			Json const& agents = created["agents"];
			REQUIRE(agents.is_object());
			CHECK_FALSE(agents["files"].empty());
			for (Json const& written : agents["files"])
			{
				CAPTURE(written.dump());
				CHECK(FileSystem::IsRegularFile(target / FileSystem::PathFromUtf8(written["path"].get<std::string>())));
			}
		}
	}

	// Directories that are files, or hold something, or climb out to one that does.
	REQUIRE(FileSystem::WriteTextFile(fixture.Directory.GetPath() / "Taken" / "Keep.txt", "keep").IsOk());
	auto const before = Snapshot(fixture.Directory.GetPath());
	for (std::filesystem::path const& directory :
	     {fixture.Directory.GetPath() / "Taken", fixture.Directory.GetPath() / "Taken" / "Keep.txt",
	      fixture.Directory.GetPath() / "Taken" / ".."})
	{
		CAPTURE(FileSystem::PathToUtf8(directory));
		for (char const* const templateId : {"empty-3d", "tiny-project"})
		{
			CommandResult result = fixture.Run(Json::object(
				{{"directory", FileSystem::PathToUtf8(directory)}, {"name", "Game"}, {"template", templateId}, {"discardChanges", true}}));
			REQUIRE(result.IsError());
			CHECK(result.GetError().Code == AutomationErrorCode::FileError);
		}
	}
	CHECK(Snapshot(fixture.Directory.GetPath()) == before);
	CHECK(Snapshot(Testing::GetTestContentDirectory()) == content);
}

TEST_CASE("ProjectCreateParameters: names that are the template's project file name in another letter case create the project")
{
	CreateFixture fixture;
	int index = 0;
	// Where the file system ignores letter case, these name the template's Template.sproj itself.
	for (std::string const name : {"template", "TEMPLATE", "tEmPlAtE", "Template"})
	{
		for (char const* const templateId : {"empty-3d", "tiny-project"})
		{
			CAPTURE(name);
			CAPTURE(templateId);
			std::filesystem::path const target = fixture.Directory.GetPath() / ("Cased" + std::to_string(index++));
			CommandResult result = fixture.Run(Json::object(
				{{"directory", FileSystem::PathToUtf8(target)}, {"name", name}, {"template", templateId}, {"discardChanges", true}}));
			REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
			std::string const fileName = name + ".sproj";
			CHECK(result.GetValue()["project"]["file"] == FileSystem::PathToUtf8(target / FileSystem::PathFromUtf8(fileName)));
			CHECK(result.GetValue()["project"]["name"] == name);
			// The one project file is stored under the name asked for.
			std::vector<std::string> projectFiles;
			std::error_code error;
			for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(target, error))
			{
				if (entry.path().extension() == ".sproj")
				{
					projectFiles.push_back(FileSystem::PathToUtf8(entry.path().filename()));
				}
			}
			REQUIRE_FALSE(error);
			CHECK(projectFiles == std::vector<std::string>{fileName});
		}
	}
}
