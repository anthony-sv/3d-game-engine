#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Project/ExampleCatalog.h"
#include "Strada/Project/ProjectCopy.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	std::filesystem::path GetFixtureExamples()
	{
		return FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR "/FrontDoor/Content/Examples").lexically_normal();
	}

	std::string Read(std::filesystem::path const& file)
	{
		Result<std::string> text = FileSystem::ReadTextFile(file);
		REQUIRE_MESSAGE(text.IsOk(), FileSystem::PathToUtf8(file));
		return text.TakeValue();
	}

	Json ReadFixtureCatalog()
	{
		Result<Json> document = ParseJson(Read(GetFixtureExamples() / "catalog.json"));
		REQUIRE(document.IsOk());
		return document.TakeValue();
	}

	Result<ExampleCatalog> LoadDocument(std::filesystem::path const& directory, Json const& document)
	{
		REQUIRE(FileSystem::WriteTextFile(directory / "catalog.json", DumpJson(document)).IsOk());
		return ExampleCatalog::Load(directory / "catalog.json");
	}
}

TEST_CASE("ExampleCatalog: the fixture's catalog loads, sorted, and tells which examples are available")
{
	Result<ExampleCatalog> loaded = ExampleCatalog::Load(GetFixtureExamples() / "catalog.json");
	REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
	ExampleCatalog const& catalog = loaded.GetValue();
	CHECK(catalog.GetWarnings().empty());
	CHECK(catalog.GetRoot() == GetFixtureExamples());
	REQUIRE(catalog.GetExamples().size() == 2);

	ExampleEntry const& tiny = catalog.GetExamples()[0];
	CHECK(tiny.Id == "tiny");
	CHECK(tiny.Name == "Tiny");
	CHECK(tiny.Order == 1);
	CHECK_FALSE(tiny.RequiresSdk);
	CHECK(tiny.NeedsPaths.empty());
	CHECK(catalog.GetDirectory(tiny) == GetFixtureExamples() / "Tiny");
	CHECK(catalog.GetProjectFile(tiny) == GetFixtureExamples() / "Tiny" / "Tiny.sproj");
	CHECK(catalog.GetThumbnailPath(tiny) == GetFixtureExamples() / "Thumbnails" / "tiny.png");
	CHECK(catalog.IsAvailable(tiny));

	ExampleEntry const& fetched = catalog.GetExamples()[1];
	CHECK(fetched.Id == "fetched");
	CHECK(fetched.RequiresSdk);
	CHECK(fetched.NeedsPaths == std::vector<std::string>{"Assets/Downloaded/Big.bin"});
	CHECK_FALSE(fetched.NeedsHint.empty());
	CHECK_FALSE(catalog.IsAvailable(fetched));

	CHECK(catalog.Find("tiny") == &tiny);
	CHECK(catalog.Find("nope") == nullptr);
	CHECK(ExampleCatalog().GetExamples().empty());
}

TEST_CASE("ExampleCatalog: an example is available when its project file and every path it needs exist")
{
	Testing::TemporaryDirectory directory;
	REQUIRE(FileSystem::CopyDirectory(GetFixtureExamples(), directory.GetPath()).IsOk());
	Result<ExampleCatalog> loaded = ExampleCatalog::Load(directory.GetPath() / "catalog.json");
	REQUIRE(loaded.IsOk());
	ExampleEntry const& fetched = *loaded.GetValue().Find("fetched");
	CHECK_FALSE(loaded.GetValue().IsAvailable(fetched));
	REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Fetched" / "Assets" / "Downloaded" / "Big.bin", "big").IsOk());
	CHECK(loaded.GetValue().IsAvailable(fetched));
	REQUIRE(FileSystem::Remove(directory.GetPath() / "Fetched" / "Fetched.sproj").IsOk());
	CHECK_FALSE(loaded.GetValue().IsAvailable(fetched));
	REQUIRE(FileSystem::Remove(directory.GetPath() / "Thumbnails" / "tiny.png").IsOk());
	CHECK(loaded.GetValue().GetThumbnailPath(*loaded.GetValue().Find("tiny")).empty());
}

TEST_CASE("ExampleCatalog: damaged entries are left out with a warning; a damaged catalog is an error")
{
	Testing::TemporaryDirectory directory;
	Json const fixture = ReadFixtureCatalog();
	Json const valid = fixture["Examples"][0];

	std::vector<std::function<void(Json&)>> const damages = {
		[](Json& entry)
		{
			entry = 7;
		},
		[](Json& entry)
		{
			entry["Id"] = "Not An Id";
		},
		[](Json& entry)
		{
			entry["Id"] = std::string(41, 'a');
		},
		[](Json& entry)
		{
			entry.erase("Name");
		},
		[](Json& entry)
		{
			entry["Name"] = "Line\nBreak";
		},
		[](Json& entry)
		{
			entry["Description"] = std::string(401, 'd');
		},
		[](Json& entry)
		{
			entry["Order"] = 2000;
		},
		[](Json& entry)
		{
			entry["Order"] = "1";
		},
		[](Json& entry)
		{
			entry["Directory"] = "../Tiny";
		},
		[](Json& entry)
		{
			entry["Directory"] = "/Tiny";
		},
		[](Json& entry)
		{
			entry.erase("Directory");
		},
		[](Json& entry)
		{
			entry["ProjectFile"] = "Tiny.txt";
		},
		[](Json& entry)
		{
			entry["ProjectFile"] = "../Tiny.sproj";
		},
		[](Json& entry)
		{
			entry["Thumbnail"] = "C:/Thumbnails/tiny.png";
		},
		[](Json& entry)
		{
			entry["Requires"] = true;
		},
		[](Json& entry)
		{
			entry["Needs"] = Json::object({{"Paths", "Assets"}});
		},
		[](Json& entry)
		{
			entry["Needs"] = Json::object({{"Paths", Json::array({"../Outside"})}});
		},
		[](Json& entry)
		{
			Json paths = Json::array();
			for (int index = 0; index < 17; index++)
			{
				paths.push_back("Assets/File" + std::to_string(index));
			}
			entry["Needs"] = Json::object({{"Paths", paths}});
		},
		[](Json& entry)
		{
			entry["Needs"] = Json::object({{"Hint", std::string(401, 'h')}});
		},
	};
	for (size_t index = 0; index < damages.size(); index++)
	{
		CAPTURE(index);
		Json document = fixture;
		Json damaged = valid;
		damages[index](damaged);
		document["Examples"].push_back(damaged);
		Result<ExampleCatalog> loaded = LoadDocument(directory.GetPath(), document);
		REQUIRE(loaded.IsOk());
		CHECK(loaded.GetValue().GetExamples().size() == 2);
		REQUIRE(loaded.GetValue().GetWarnings().size() == 1);
		CHECK(loaded.GetValue().GetWarnings()[0].find("example 2") != std::string::npos);
	}

	// An id taken by an earlier entry; members a later version adds.
	Json document = fixture;
	document["Examples"].push_back(valid);
	Json later = valid;
	later["Id"] = "later";
	later["Order"] = 0;
	later["Video"] = Json::object({{"Path", "intro.mp4"}});
	later.erase("Description");
	later.erase("Thumbnail");
	later.erase("Requires");
	document["Examples"].push_back(later);
	document["Featured"] = "tiny";
	Result<ExampleCatalog> loaded = LoadDocument(directory.GetPath(), document);
	REQUIRE(loaded.IsOk());
	REQUIRE(loaded.GetValue().GetExamples().size() == 3);
	CHECK(loaded.GetValue().GetExamples()[0].Id == "later");
	REQUIRE(loaded.GetValue().GetWarnings().size() == 1);
	CHECK(loaded.GetValue().GetWarnings()[0].find("'tiny'") != std::string::npos);

	// Damage to the whole file.
	Json newer = fixture;
	newer["Strada"]["Version"] = ExampleCatalog::FormatVersion + 1;
	CHECK(LoadDocument(directory.GetPath(), newer).IsError());
	Json foreign = fixture;
	foreign["Strada"] = MakeFileHeader("ProjectTemplate", 1);
	CHECK(LoadDocument(directory.GetPath(), foreign).IsError());
	Json noExamples = fixture;
	noExamples["Examples"] = Json::object();
	CHECK(LoadDocument(directory.GetPath(), noExamples).IsError());
	REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "catalog.json", "[1,").IsOk());
	CHECK(ExampleCatalog::Load(directory.GetPath() / "catalog.json").IsError());
	CHECK(ExampleCatalog::Load(directory.GetPath() / "Missing.json").IsError());
	CHECK(ExampleCatalog::Load({}).IsError());
}

TEST_CASE("ExampleCatalog: the repository's catalog lists its samples without warnings, each with a thumbnail")
{
	std::filesystem::path const projects = FileSystem::PathFromUtf8(STRADA_PROJECTS_DIR);
	Result<ExampleCatalog> loaded = ExampleCatalog::Load(projects / "catalog.json");
	REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
	ExampleCatalog const& catalog = loaded.GetValue();
	CHECK_MESSAGE(catalog.GetWarnings().empty(), (catalog.GetWarnings().empty() ? std::string() : catalog.GetWarnings().front()));
	std::vector<std::string> ids;
	for (ExampleEntry const& example : catalog.GetExamples())
	{
		CAPTURE(example.Id);
		ids.push_back(example.Id);
		CHECK(FileSystem::IsRegularFile(catalog.GetProjectFile(example)));
		std::filesystem::path const thumbnail = catalog.GetThumbnailPath(example);
		CHECK_FALSE(thumbnail.empty());
		CHECK(FileSystem::IsInside(thumbnail, projects / "Thumbnails"));
		// The examples with C# scripts say they need the SDK.
		CHECK(example.RequiresSdk == FileSystem::IsDirectory(catalog.GetDirectory(example) / "Scripts"));
	}
	// FeatureTest tests the engine: it is not an example.
	CHECK(ids == std::vector<std::string>{"prism-break", "blocks", "showcase"});

	// The Showcase needs its downloaded assets, one file of each kind its .gitignore leaves out, and says how to fetch them.
	ExampleEntry const& showcase = *catalog.Find("showcase");
	std::string const ignored = Read(projects / "Showcase" / ".gitignore");
	for (char const* const kind : {"/Assets/Environments/*.hdr", "/Assets/Models/", "/Assets/Textures/**/*.jpg"})
	{
		CHECK(ignored.find(kind) != std::string::npos);
	}
	REQUIRE(showcase.NeedsPaths.size() == 3);
	CHECK(showcase.NeedsPaths[0].starts_with("Assets/Environments/"));
	CHECK(showcase.NeedsPaths[0].ends_with(".hdr"));
	CHECK(showcase.NeedsPaths[1].starts_with("Assets/Models/"));
	CHECK(showcase.NeedsPaths[2].starts_with("Assets/Textures/"));
	CHECK(showcase.NeedsPaths[2].ends_with(".jpg"));
	CHECK(showcase.NeedsHint.find("python Projects/Showcase/Tools/FetchAssets.py") != std::string::npos);
	CHECK(FileSystem::IsRegularFile(projects / "Showcase" / "Tools" / "FetchAssets.py"));
	CHECK(catalog.IsAvailable(*catalog.Find("prism-break")));
	CHECK(catalog.IsAvailable(*catalog.Find("blocks")));
}

TEST_CASE("ExampleCatalog: the repository's examples copy without their tools and their repository-only blocks")
{
	Result<ExampleCatalog> loaded = ExampleCatalog::Load(FileSystem::PathFromUtf8(STRADA_PROJECTS_DIR) / "catalog.json");
	REQUIRE(loaded.IsOk());
	for (char const* const id : {"prism-break", "blocks"})
	{
		CAPTURE(id);
		Testing::TemporaryDirectory directory;
		ExampleEntry const& example = *loaded.GetValue().Find(id);
		std::filesystem::path const target = directory.GetPath() / "Copy";
		Result<ProjectCopyResult> copied = ProjectCopy::Copy(loaded.GetValue().GetDirectory(example), target);
		REQUIRE_MESSAGE(copied.IsOk(), (copied ? std::string() : copied.GetError()));
		CHECK(copied.GetValue().Warnings.empty());
		CHECK(FileSystem::IsRegularFile(target / FileSystem::PathFromUtf8(example.ProjectFile)));
		CHECK_FALSE(FileSystem::Exists(target / "Tools"));
		CHECK(FileSystem::IsDirectory(target / "Scripts") == example.RequiresSdk);
	}
	// The C# sample's script project keeps its engine reference and loses the fallback to the repository's sources.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const target = directory.GetPath() / "Blocks";
	REQUIRE(ProjectCopy::Copy(loaded.GetValue().GetDirectory(*loaded.GetValue().Find("blocks")), target).IsOk());
	std::string const project = Read(target / "Scripts" / "Blocks.csproj");
	CHECK(project.find("Strada-ScriptCore") == std::string::npos);
	CHECK(project.find("repository-only") == std::string::npos);
	CHECK(project.find("$(StradaScriptCoreAssembly)") != std::string::npos);
	CHECK(project.ends_with("</Project>\n"));
}
