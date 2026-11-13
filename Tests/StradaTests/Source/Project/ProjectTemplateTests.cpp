#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ProjectTemplate.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

using namespace Strada;

namespace
{
	std::filesystem::path GetFixtureTemplates()
	{
		return FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR "/FrontDoor/Content/Templates").lexically_normal();
	}

	std::string Read(std::filesystem::path const& file)
	{
		Result<std::string> text = FileSystem::ReadTextFile(file);
		REQUIRE_MESSAGE(text.IsOk(), FileSystem::PathToUtf8(file));
		return text.TakeValue();
	}

	Json ReadJson(std::filesystem::path const& file)
	{
		Result<Json> document = ParseJson(Read(file));
		REQUIRE(document.IsOk());
		return document.TakeValue();
	}

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

	// A copy of the fixture's project template in the directory, under its id, whose template.json the test then changes.
	std::filesystem::path CopyFixtureTemplate(std::filesystem::path const& root, std::string const& id = "tiny-project")
	{
		std::filesystem::path const copy = root / FileSystem::PathFromUtf8(id);
		REQUIRE(FileSystem::CopyDirectory(GetFixtureTemplates() / "tiny-project", copy).IsOk());
		return copy;
	}

	// Whether the file system finds the file under its name in upper case (Windows, macOS by default).
	bool IgnoresLetterCase(std::filesystem::path const& file)
	{
		std::string const name = FileSystem::PathToUtf8(file.filename());
		std::string upper = name;
		std::ranges::transform(upper, upper.begin(),
		                       [](unsigned char character)
		                       {
								   return static_cast<char>(std::toupper(character));
							   });
		return upper != name && FileSystem::Exists(file.parent_path() / FileSystem::PathFromUtf8(upper));
	}

	// The fixture's template.json with a change made to its "Template" object.
	void WriteTemplate(std::filesystem::path const& folder, std::function<void(Json&)> const& change)
	{
		Json document = ReadJson(GetFixtureTemplates() / "tiny-project" / "template.json");
		change(document["Template"]);
		REQUIRE(FileSystem::WriteTextFile(folder / "template.json", DumpJson(document)).IsOk());
	}
}

TEST_CASE("ProjectTemplate: the fixture's templates load, sorted, with their thumbnails")
{
	std::vector<std::string> warnings;
	std::vector<ProjectTemplate> const templates = ProjectTemplate::List(GetFixtureTemplates(), warnings);
	CHECK(warnings.empty());
	REQUIRE(templates.size() == 2);

	ProjectTemplate const& empty = templates[0];
	CHECK(empty.Id == "empty-3d");
	CHECK(empty.Name == "Empty 3D");
	CHECK(empty.Order == 1);
	CHECK(empty.IsDefaultScene());
	CHECK_FALSE(empty.RequiresSdk);
	CHECK(empty.Tags == std::vector<std::string>{"3D"});
	CHECK(empty.GetThumbnailPath() == (GetFixtureTemplates() / "empty-3d" / "thumbnail.png").lexically_normal());

	ProjectTemplate const& tiny = templates[1];
	CHECK(tiny.Id == "tiny-project");
	CHECK(tiny.Order == 2);
	CHECK_FALSE(tiny.IsDefaultScene());
	CHECK(tiny.ProjectFile == "Template.sproj");
	CHECK(tiny.RequiresSdk);
	CHECK(tiny.Tags == std::vector<std::string>{"3D", "Visual scripts", "C#"});
	CHECK_FALSE(tiny.Description.empty());
	CHECK(FileSystem::IsInside(tiny.Directory, GetFixtureTemplates()));

	// The editor's stand-in for a missing empty-3d says the same as the fixture's.
	ProjectTemplate const made = ProjectTemplate::MakeDefault();
	CHECK(made.Id == empty.Id);
	CHECK(made.Name == empty.Name);
	CHECK(made.Description == empty.Description);
	CHECK(made.IsDefaultScene());
	CHECK(made.GetThumbnailPath().empty());
}

TEST_CASE("ProjectTemplate: the repository's templates load without warnings")
{
	std::vector<std::string> warnings;
	std::vector<ProjectTemplate> const templates = ProjectTemplate::List(FileSystem::PathFromUtf8(STRADA_TEMPLATES_DIR), warnings);
	CHECK_MESSAGE(warnings.empty(), (warnings.empty() ? std::string() : warnings.front()));
	// Spec section 4.2: the editor's default scene, Orb Run made of graphs, and Orb Run in C#, which needs the .NET SDK.
	REQUIRE(templates.size() == 3);
	CHECK(templates[0].Id == ProjectTemplate::DefaultId);
	CHECK(templates[0].IsDefaultScene());
	ProjectTemplate const made = ProjectTemplate::MakeDefault();
	CHECK(templates[0].Name == made.Name);
	CHECK(templates[0].Description == made.Description);
	CHECK(templates[1].Id == "graph-starter");
	CHECK_FALSE(templates[1].RequiresSdk);
	CHECK(templates[2].Id == "csharp-starter");
	CHECK(templates[2].RequiresSdk);
	for (ProjectTemplate const& projectTemplate : templates)
	{
		CAPTURE(projectTemplate.Id);
		CHECK(FileSystem::IsRegularFile(projectTemplate.GetThumbnailPath()));
		if (!projectTemplate.IsDefaultScene())
		{
			CHECK(projectTemplate.ProjectFile == "Template.sproj");
			CHECK(FileSystem::IsRegularFile(projectTemplate.Directory / FileSystem::PathFromUtf8(projectTemplate.ProjectFile)));
		}
	}
}

TEST_CASE("ProjectTemplate: damaged, foreign and newer template files are refused, unknown members ignored")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const folder = CopyFixtureTemplate(directory.GetPath());
	REQUIRE(ProjectTemplate::Load(folder).IsOk());

	std::vector<std::function<void(Json&)>> const damages = {
		[](Json& value)
		{
			value["Id"] = "other";
		},
		[](Json& value)
		{
			value["Id"] = "Tiny-Project";
		},
		[](Json& value)
		{
			value.erase("Id");
		},
		[](Json& value)
		{
			value["Name"] = "";
		},
		[](Json& value)
		{
			value["Name"] = std::string(65, 'n');
		},
		[](Json& value)
		{
			value["Name"] = "Tab\tName";
		},
		[](Json& value)
		{
			value["Description"] = std::string(401, 'd');
		},
		[](Json& value)
		{
			value["Order"] = 1001;
		},
		[](Json& value)
		{
			value["Order"] = -1;
		},
		[](Json& value)
		{
			value["Order"] = 2.5;
		},
		[](Json& value)
		{
			value.erase("Order");
		},
		[](Json& value)
		{
			value["Tags"] = Json::array({"1", "2", "3", "4", "5", "6", "7", "8", "9"});
		},
		[](Json& value)
		{
			value["Tags"] = Json::array({std::string(25, 't')});
		},
		[](Json& value)
		{
			value["Tags"] = Json::array({1});
		},
		[](Json& value)
		{
			value["Requires"] = Json::object({{"Sdk", "yes"}});
		},
		[](Json& value)
		{
			value["Thumbnail"] = "../thumbnail.png";
		},
		[](Json& value)
		{
			value["Thumbnail"] = "C:/thumbnail.png";
		},
		[](Json& value)
		{
			value["Thumbnail"] = "Assets";
		},
		[](Json& value)
		{
			value["Thumbnail"] = "Assets/AssetRegistry.sreg";
		},
		[](Json& value)
		{
			value["Thumbnail"] = "Template.sproj";
		},
		[](Json& value)
		{
			value["ProjectFile"] = "/Template.sproj";
		},
		[](Json& value)
		{
			value["ProjectFile"] = "Assets/Scenes/Main.sscene";
		},
		[](Json& value)
		{
			value["ProjectFile"] = "Missing.sproj";
		},
		[](Json& value)
		{
			value["ProjectFile"] = "Scripts\\Template.sproj";
		},
		[](Json& value)
		{
			value["ProjectFile"] = "./Template.sproj";
		},
	};
	for (size_t index = 0; index < damages.size(); index++)
	{
		CAPTURE(index);
		WriteTemplate(folder, damages[index]);
		CHECK(ProjectTemplate::Load(folder).IsError());
	}

	// A thumbnail in a folder of the project, which a copy would leave out of it, even a real PNG the project uses.
	REQUIRE(FileSystem::Copy(folder / "thumbnail.png", folder / "Assets" / "Textures" / "Grid.png", false).IsOk());
	for (char const* const thumbnail : {"Assets/Textures/Grid.png", "Media/thumbnail.png"})
	{
		CAPTURE(thumbnail);
		WriteTemplate(folder,
		              [thumbnail](Json& value)
		              {
						  value["Thumbnail"] = thumbnail;
					  });
		Result<ProjectTemplate> refused = ProjectTemplate::Load(folder);
		REQUIRE(refused.IsError());
		CHECK(refused.GetError().find("Template.Thumbnail") != std::string::npos);
	}
	// A project file where the copy filter leaves files out, which no copy would bring along (in any letter case the file
	// system takes).
	for (char const* const projectFile :
	     {"Tools/Template.sproj", "Build/Template.sproj", "Scripts/bin/Template.sproj", ".strada/Template.sproj"})
	{
		CAPTURE(projectFile);
		REQUIRE(FileSystem::Copy(folder / "Template.sproj", folder / FileSystem::PathFromUtf8(projectFile), false).IsOk());
		WriteTemplate(folder,
		              [projectFile](Json& value)
		              {
						  value["ProjectFile"] = projectFile;
					  });
		Result<ProjectTemplate> refused = ProjectTemplate::Load(folder);
		REQUIRE(refused.IsError());
		CHECK(refused.GetError().find("leaves files out") != std::string::npos);
	}
	WriteTemplate(folder,
	              [](Json& value)
	              {
					  value["ProjectFile"] = "tools/Template.sproj";
				  });
	CHECK(ProjectTemplate::Load(folder).IsError());

	WriteTemplate(folder,
	              [](Json& value)
	              {
					  value["Later"] = Json::object({{"Member", 1}});
					  value.erase("Description");
					  value.erase("Tags");
					  value.erase("Requires");
					  value.erase("Thumbnail");
				  });
	Result<ProjectTemplate> lenient = ProjectTemplate::Load(folder);
	REQUIRE(lenient.IsOk());
	CHECK(lenient.GetValue().Description.empty());
	CHECK(lenient.GetValue().GetThumbnailPath().empty());

	Json document = ReadJson(folder / "template.json");
	document["Strada"]["Version"] = ProjectTemplate::FormatVersion + 1;
	REQUIRE(FileSystem::WriteTextFile(folder / "template.json", DumpJson(document)).IsOk());
	CHECK(ProjectTemplate::Load(folder).IsError());
	document["Strada"] = MakeFileHeader("Project", 1);
	REQUIRE(FileSystem::WriteTextFile(folder / "template.json", DumpJson(document)).IsOk());
	CHECK(ProjectTemplate::Load(folder).IsError());
	REQUIRE(FileSystem::WriteTextFile(folder / "template.json", "{ not json").IsOk());
	CHECK(ProjectTemplate::Load(folder).IsError());
	REQUIRE(FileSystem::Remove(folder / "template.json").IsOk());
	CHECK(ProjectTemplate::Load(folder).IsError());
}

TEST_CASE("ProjectTemplate: thumbnails are PNG images of at most 1 MiB")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const folder = CopyFixtureTemplate(directory.GetPath());
	Result<ProjectTemplate> loaded = ProjectTemplate::Load(folder);
	REQUIRE(loaded.IsOk());
	CHECK(loaded.GetValue().GetThumbnailPath() == (folder / "thumbnail.png").lexically_normal());

	REQUIRE(FileSystem::WriteTextFile(folder / "thumbnail.png", "not a picture").IsOk());
	CHECK(loaded.GetValue().GetThumbnailPath().empty());
	std::string large = Read(GetFixtureTemplates() / "tiny-project" / "thumbnail.png");
	large.resize(1024 * 1024 + 1, '\0');
	REQUIRE(FileSystem::WriteTextFile(folder / "thumbnail.png", large).IsOk());
	CHECK(loaded.GetValue().GetThumbnailPath().empty());
	REQUIRE(FileSystem::Remove(folder / "thumbnail.png").IsOk());
	CHECK(loaded.GetValue().GetThumbnailPath().empty());
	// A missing thumbnail does not make the template damaged.
	CHECK(ProjectTemplate::Load(folder).IsOk());
}

TEST_CASE("ProjectTemplate: listing leaves out damaged templates with a warning and folders that hold none")
{
	Testing::TemporaryDirectory directory;
	std::vector<std::string> warnings;
	CHECK(ProjectTemplate::List(directory.GetPath() / "Missing", warnings).empty());
	CHECK(ProjectTemplate::List({}, warnings).empty());
	CHECK(warnings.empty());

	std::filesystem::path const first = CopyFixtureTemplate(directory.GetPath(), "zeta");
	WriteTemplate(first,
	              [](Json& value)
	              {
					  value["Id"] = "zeta";
					  value["Order"] = 5;
				  });
	std::filesystem::path const second = CopyFixtureTemplate(directory.GetPath(), "alpha");
	WriteTemplate(second,
	              [](Json& value)
	              {
					  value["Id"] = "alpha";
					  value["Order"] = 5;
				  });
	std::filesystem::path const early = CopyFixtureTemplate(directory.GetPath(), "omega");
	WriteTemplate(early,
	              [](Json& value)
	              {
					  value["Id"] = "omega";
					  value["Order"] = 0;
				  });
	std::filesystem::path const broken = CopyFixtureTemplate(directory.GetPath(), "broken");
	REQUIRE(FileSystem::WriteTextFile(broken / "template.json", "[]").IsOk());
	REQUIRE(FileSystem::CreateDirectories(directory.GetPath() / "Tools").IsOk());
	REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Tools" / "Author.py", "").IsOk());
	REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "template.json", "{}").IsOk());

	std::vector<ProjectTemplate> const templates = ProjectTemplate::List(directory.GetPath(), warnings);
	REQUIRE(templates.size() == 3);
	CHECK(templates[0].Id == "omega");
	CHECK(templates[1].Id == "alpha");
	CHECK(templates[2].Id == "zeta");
	REQUIRE(warnings.size() == 1);
	CHECK(warnings[0].find("broken") != std::string::npos);
}

TEST_CASE("ProjectTemplate: a template with a project is copied, renamed and named after the new project")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory directory;
	Result<ProjectTemplate> loaded = ProjectTemplate::Load(GetFixtureTemplates() / "tiny-project");
	REQUIRE(loaded.IsOk());
	std::filesystem::path const source = loaded.GetValue().Directory;

	std::filesystem::path const target = directory.GetPath() / "Games" / "Orb";
	Result<std::filesystem::path> created = ProjectTemplate::Instantiate(loaded.GetValue(), target, "Orb Run: Deluxe", TemplateOptions{});
	REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
	CHECK(created.GetValue() == (target / "Orb Run_ Deluxe.sproj").lexically_normal());
	std::vector<std::string> const expected = {".gitignore",
	                                           "Assets/AssetRegistry.sreg",
	                                           "Assets/Scenes/Main.sscene",
	                                           "Assets/Scripts/Rule.sgraph",
	                                           "Orb Run_ Deluxe.sproj",
	                                           "Scripts/Game.csproj",
	                                           "Scripts/Source/Game.cs"};
	CHECK(ListFiles(target) == expected);
	CHECK(Read(target / ".gitignore") == ProjectTemplate::GitIgnoreText);
	// Nothing but the project file is rewritten; the script project loses its repository-only block.
	for (char const* const file :
	     {"Assets/AssetRegistry.sreg", "Assets/Scenes/Main.sscene", "Assets/Scripts/Rule.sgraph", "Scripts/Source/Game.cs"})
	{
		CAPTURE(file);
		CHECK(Read(target / file) == Read(source / file));
	}
	std::string const script = Read(target / "Scripts" / "Game.csproj");
	CHECK(script.find("Strada-ScriptCore") == std::string::npos);
	CHECK(script.find("repository-only") == std::string::npos);
	CHECK(script.find("<RootNamespace>Game</RootNamespace>") != std::string::npos);

	Json const project = ReadJson(created.GetValue());
	Json expectedProject = ReadJson(source / "Template.sproj");
	expectedProject["Project"]["Name"] = "Orb Run: Deluxe";
	expectedProject["Project"]["Window"]["Title"] = "Orb Run: Deluxe";
	CHECK(project == expectedProject);
	CHECK(Read(created.GetValue()) == DumpJson(expectedProject));

	// It opens, with its start scene.
	Result<Ref<Project>> opened = Project::Open(created.GetValue());
	REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
	CHECK(opened.GetValue()->GetSettings().Name == "Orb Run: Deluxe");
	CHECK(AssetManager::GetMetadata(opened.GetValue()->GetSettings().StartScene)->Path == "Scenes/Main.sscene");
	AssetManager::CloseAssetDirectory();

	// Without a .gitignore; a title of its own stays.
	Testing::TemporaryDirectory other;
	std::filesystem::path const folder = CopyFixtureTemplate(other.GetPath());
	Json titled = ReadJson(folder / "Template.sproj");
	titled["Project"]["Window"]["Title"] = "A Title of Its Own";
	REQUIRE(FileSystem::WriteTextFile(folder / "Template.sproj", DumpJson(titled)).IsOk());
	Result<ProjectTemplate> titledTemplate = ProjectTemplate::Load(folder);
	REQUIRE(titledTemplate.IsOk());
	TemplateOptions options;
	options.GitIgnore = false;
	Result<std::filesystem::path> plain = ProjectTemplate::Instantiate(titledTemplate.GetValue(), other.GetPath() / "Plain", "  ", options);
	REQUIRE(plain.IsOk());
	CHECK(plain.GetValue().filename() == "Project.sproj");
	CHECK_FALSE(FileSystem::Exists(other.GetPath() / "Plain" / ".gitignore"));
	CHECK(ReadJson(plain.GetValue())["Project"]["Window"]["Title"] == "A Title of Its Own");
	CHECK(ReadJson(plain.GetValue())["Project"]["Name"] == "  ");

	// A .gitignore the template brings stays as it is.
	REQUIRE(FileSystem::WriteTextFile(folder / ".gitignore", "Mine/\n").IsOk());
	Result<std::filesystem::path> own =
		ProjectTemplate::Instantiate(titledTemplate.GetValue(), other.GetPath() / "Own", "Own", TemplateOptions{});
	REQUIRE(own.IsOk());
	CHECK(Read(other.GetPath() / "Own" / ".gitignore") == "Mine/\n");
}

TEST_CASE("ProjectTemplate: a default-scene template writes what creating a project writes, without opening it")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory directory;
	std::vector<std::string> warnings;
	std::vector<ProjectTemplate> const templates = ProjectTemplate::List(GetFixtureTemplates(), warnings);
	REQUIRE_FALSE(templates.empty());
	ProjectTemplate const& empty = templates.front();
	REQUIRE(empty.IsDefaultScene());

	Scene scene("Main");
	scene.CreateEntity("Player");
	CHECK(ProjectTemplate::Instantiate(empty, directory.GetPath() / "None", "Game", TemplateOptions{}).IsError());
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "None"));

	std::filesystem::path const target = directory.GetPath() / "Game";
	Result<std::filesystem::path> created = ProjectTemplate::Instantiate(empty, target, "My Game", TemplateOptions{}, &scene);
	REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
	CHECK_FALSE(AssetManager::HasAssetDirectory());
	CHECK(ListFiles(target) ==
	      std::vector<std::string>{".gitignore", "Assets/AssetRegistry.sreg", "Assets/Scenes/Main.sscene", "My Game.sproj"});

	// The same files as Project::Create's.
	Result<Ref<Project>> reference = Project::Create(directory.GetPath() / "Reference", "My Game", scene);
	REQUIRE(reference.IsOk());
	AssetManager::CloseAssetDirectory();
	CHECK(Read(target / "Assets" / "Scenes" / "Main.sscene") ==
	      Read(directory.GetPath() / "Reference" / "Assets" / "Scenes" / "Main.sscene"));
	Json made = ReadJson(created.GetValue());
	Json expected = ReadJson(directory.GetPath() / "Reference" / "My Game.sproj");
	made["Project"].erase("StartScene");
	expected["Project"].erase("StartScene");
	CHECK(made == expected);

	Result<Ref<Project>> opened = Project::Open(created.GetValue());
	REQUIRE(opened.IsOk());
	CHECK(AssetManager::GetMetadata(opened.GetValue()->GetSettings().StartScene)->Path == "Scenes/Main.sscene");
}

TEST_CASE("ProjectTemplate: instantiating refuses targets that are not new or empty and leaves nothing behind when it fails")
{
	Testing::TemporaryDirectory directory;
	Result<ProjectTemplate> loaded = ProjectTemplate::Load(GetFixtureTemplates() / "tiny-project");
	REQUIRE(loaded.IsOk());

	std::filesystem::path const taken = directory.GetPath() / "Taken";
	REQUIRE(FileSystem::WriteTextFile(taken / "Keep.txt", "keep").IsOk());
	CHECK(ProjectTemplate::Instantiate(loaded.GetValue(), taken, "Game", TemplateOptions{}).IsError());
	CHECK(ListFiles(taken) == std::vector<std::string>{"Keep.txt"});
	CHECK(ProjectTemplate::Instantiate(loaded.GetValue(), directory.GetPath() / "Empty", "", TemplateOptions{}).IsError());
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "Empty"));

	// A template whose project file is damaged fails after its files were copied: they go again.
	std::filesystem::path const folder = CopyFixtureTemplate(directory.GetPath() / "Templates");
	REQUIRE(FileSystem::WriteTextFile(folder / "Template.sproj", "{ damaged").IsOk());
	Result<ProjectTemplate> damaged = ProjectTemplate::Load(folder);
	REQUIRE(damaged.IsOk());
	CHECK(ProjectTemplate::Instantiate(damaged.GetValue(), directory.GetPath() / "New" / "Game", "Game", TemplateOptions{}).IsError());
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "New"));
	std::filesystem::path const empty = directory.GetPath() / "EmptyTarget";
	REQUIRE(FileSystem::CreateDirectories(empty).IsOk());
	CHECK(ProjectTemplate::Instantiate(damaged.GetValue(), empty, "Game", TemplateOptions{}).IsError());
	CHECK(FileSystem::IsDirectory(empty));
	CHECK(ListFiles(empty).empty());

	// A file of the template that the project file would replace.
	std::filesystem::path const clash = CopyFixtureTemplate(directory.GetPath() / "Clash");
	REQUIRE(FileSystem::WriteTextFile(clash / "Game.sproj", "{}").IsOk());
	Result<ProjectTemplate> clashing = ProjectTemplate::Load(clash);
	REQUIRE(clashing.IsOk());
	CHECK(ProjectTemplate::Instantiate(clashing.GetValue(), directory.GetPath() / "ClashGame", "Game", TemplateOptions{}).IsError());
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "ClashGame"));
}

TEST_CASE("ProjectTemplate: a name that differs from the template's project file only in letter case renames it")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory directory;
	Result<ProjectTemplate> loaded = ProjectTemplate::Load(GetFixtureTemplates() / "tiny-project");
	REQUIRE(loaded.IsOk());
	// Where the file system ignores letter case, these name Template.sproj itself.
	int index = 0;
	for (std::string const name : {"template", "TEMPLATE", "tEmPlAtE", "Template"})
	{
		CAPTURE(name);
		std::filesystem::path const target = directory.GetPath() / ("Game" + std::to_string(index++));
		Result<std::filesystem::path> created = ProjectTemplate::Instantiate(loaded.GetValue(), target, name, TemplateOptions{});
		REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
		std::string const fileName = name + ".sproj";
		CHECK(created.GetValue() == (target / FileSystem::PathFromUtf8(fileName)).lexically_normal());
		// Stored under the new spelling, once.
		std::vector<std::string> const files = ListFiles(target);
		CHECK(std::ranges::count_if(files,
		                            [](std::string const& file)
		                            {
										return file.ends_with(".sproj");
									}) == 1);
		CHECK(std::ranges::find(files, fileName) != files.end());
		CHECK(ReadJson(created.GetValue())["Project"]["Name"] == name);
		Result<Ref<Project>> opened = Project::Open(created.GetValue());
		REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
		CHECK(opened.GetValue()->GetSettings().Name == name);
		AssetManager::CloseAssetDirectory();
	}

	// The files a copy leaves out are found under the spelling template.json gives, too.
	std::filesystem::path const folder = CopyFixtureTemplate(directory.GetPath() / "Templates");
	if (IgnoresLetterCase(folder / "thumbnail.png"))
	{
		WriteTemplate(folder,
		              [](Json& value)
		              {
						  value["Thumbnail"] = "THUMBNAIL.png";
					  });
		Result<ProjectTemplate> spelled = ProjectTemplate::Load(folder);
		REQUIRE(spelled.IsOk());
		CHECK_FALSE(spelled.GetValue().GetThumbnailPath().empty());
		Result<std::filesystem::path> created =
			ProjectTemplate::Instantiate(spelled.GetValue(), directory.GetPath() / "Spelled", "Spelled", TemplateOptions{});
		REQUIRE(created.IsOk());
		std::vector<std::string> const files = ListFiles(directory.GetPath() / "Spelled");
		CHECK(std::ranges::find(files, "thumbnail.png") == files.end());
		CHECK(std::ranges::find(files, "template.json") == files.end());
	}
}
