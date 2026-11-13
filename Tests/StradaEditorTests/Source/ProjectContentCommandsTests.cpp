#include "EditorLocationsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/ProjectContentCommands.h"
#include "Editor/EditorOperations.h"
#include "Editor/ExampleCopyTask.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ProjectTemplate.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>

using namespace Strada;

namespace
{
	// The editor model with the editor's and the project content commands, as the editor layer sets them up, with the test
	// content as the engine's (or no engine content at all).
	struct ContentFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		Scope<ExampleCopyTask> Copies = CreateScope<ExampleCopyTask>();
		CommandRegistry Registry;
		EditorLocations Locations;
		int ProjectsOpened = 0;

		explicit ContentFixture(bool withContent = true)
		{
			if (withContent)
			{
				Locations = Testing::MakeEditorLocations(Directory.GetPath());
			}
			EditorCommandEnvironment environment;
			environment.Locations = Locations;
			environment.ProjectOpened = [this]
			{
				ProjectsOpened++;
			};
			REQUIRE(RegisterEditorCommands(Registry, Operations, std::move(environment)).IsOk());
			REQUIRE(RegisterProjectContentCommands(Registry, Operations, *Copies, Locations,
			                                       [this]
			                                       {
													   ProjectsOpened++;
												   })
			            .IsOk());
		}

		// Runs a command, updating the copies as the editor does every frame until it answers.
		CommandResult Run(std::string_view name, Json params = Json::object()) { return Run(Registry, name, std::move(params)); }

		CommandResult Run(CommandRegistry& registry, std::string_view name, Json params)
		{
			std::optional<CommandResult> result;
			registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			Await(result);
			return std::move(*result);
		}

		// Updates the copies as the editor does every frame until the command answered.
		void Await(std::optional<CommandResult> const& result)
		{
			auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
			while (!result && std::chrono::steady_clock::now() < deadline)
			{
				Copies->Update();
				if (!result)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
			}
			REQUIRE(result.has_value());
		}

		// The test content's examples, copied into the temporary directory where a test may change them, as the engine content
		// of another registry of the project content commands. Returns the copied examples' folder.
		std::filesystem::path RegisterCopiedContent(CommandRegistry& registry)
		{
			std::filesystem::path const content = Directory.GetPath() / "Content";
			REQUIRE(FileSystem::CopyDirectory(Testing::GetTestContentDirectory() / "Examples", content / "Examples").IsOk());
			Result<EngineLayout> layout = EngineLayout::Resolve(content / "StradaEditor");
			REQUIRE(layout.IsOk());
			EditorLocations locations;
			locations.Layout = layout.TakeValue();
			REQUIRE(RegisterProjectContentCommands(registry, Operations, *Copies, locations, {}).IsOk());
			return content / "Examples";
		}

		Json Ok(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
			return result.TakeValue();
		}

		CommandError Fails(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE(result.IsError());
			return result.GetError();
		}

		std::string Path(std::filesystem::path const& relative) const { return FileSystem::PathToUtf8(Directory.GetPath() / relative); }
	};

	std::filesystem::path GetContent()
	{
		return Testing::GetTestContentDirectory();
	}

	std::string Read(std::filesystem::path const& file)
	{
		Result<std::string> text = FileSystem::ReadTextFile(file);
		REQUIRE_MESSAGE(text.IsOk(), FileSystem::PathToUtf8(file));
		return text.TakeValue();
	}

	// Whether the file system finds the file or folder under its name in upper case (Windows, macOS by default).
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
}

TEST_CASE("ProjectContentCommands: project.templates lists the engine's templates, or the default scene standing in for them")
{
	ContentFixture fixture;
	Json const listed = fixture.Ok("project.templates");
	CHECK(listed["warnings"] == Json::array());
	REQUIRE(listed["templates"].size() == 2);
	Json const& empty = listed["templates"][0];
	CHECK(empty["id"] == "empty-3d");
	CHECK(empty["name"] == "Empty 3D");
	CHECK(empty["order"] == 1);
	CHECK(empty["requires"] == Json::object({{"sdk", false}}));
	CHECK(empty["tags"] == Json::array({"3D"}));
	CHECK(empty["thumbnail"] == FileSystem::PathToUtf8(GetContent() / "Templates" / "empty-3d" / "thumbnail.png"));
	Json const& tiny = listed["templates"][1];
	CHECK(tiny["id"] == "tiny-project");
	CHECK(tiny["requires"]["sdk"] == true);
	CHECK_FALSE(tiny["description"].get<std::string>().empty());
	CHECK(fixture.Registry.Find("project.templates")->ReadOnly);
}

TEST_CASE("ProjectContentCommands: without engine content, project.templates offers the default scene with a warning")
{
	ContentFixture bare(false);
	Json const synthesized = bare.Ok("project.templates");
	REQUIRE(synthesized["templates"].size() == 1);
	CHECK(synthesized["templates"][0]["id"] == "empty-3d");
	CHECK(synthesized["templates"][0]["thumbnail"].is_null());
	REQUIRE(synthesized["warnings"].size() == 1);
	CHECK(synthesized["warnings"][0].get<std::string>().find("empty-3d") != std::string::npos);
}

TEST_CASE("ProjectContentCommands: project.examples lists the catalog, with what is not available and why")
{
	ContentFixture fixture;
	Json const listed = fixture.Ok("project.examples");
	CHECK(listed["warnings"] == Json::array());
	REQUIRE(listed["examples"].size() == 2);
	Json const& tiny = listed["examples"][0];
	CHECK(tiny["id"] == "tiny");
	CHECK(tiny["name"] == "Tiny");
	CHECK(tiny["available"] == true);
	CHECK_FALSE(tiny.contains("hint"));
	CHECK(tiny["requires"]["sdk"] == false);
	CHECK(tiny["thumbnail"] == FileSystem::PathToUtf8(GetContent() / "Examples" / "Thumbnails" / "tiny.png"));
	Json const& fetched = listed["examples"][1];
	CHECK(fetched["id"] == "fetched");
	CHECK(fetched["available"] == false);
	CHECK(fetched["hint"].get<std::string>().starts_with("Run: "));
	CHECK(fixture.Registry.Find("project.examples")->ReadOnly);
}

TEST_CASE("ProjectContentCommands: without engine content, project.examples lists none and says why")
{
	ContentFixture bare(false);
	Json const none = bare.Ok("project.examples");
	CHECK(none["examples"] == Json::array());
	CHECK(none["warnings"].size() == 1);
}

TEST_CASE("ProjectContentCommands: project.copy-example copies an example without opening it")
{
	ContentFixture fixture;
	CHECK(fixture.Registry.Find("project.copy-example")->IsAsync());
	std::string const directory = fixture.Path("Copies/Tiny");
	Json const copied = fixture.Ok("project.copy-example", Json::object({{"example", "tiny"}, {"directory", directory}, {"open", false}}));
	CHECK(copied["directory"] == directory);
	CHECK(copied["files"] == 5);
	CHECK(copied["bytes"].get<uint64_t>() > 0);
	CHECK(copied["warnings"] == Json::array());
	CHECK_FALSE(copied.contains("project"));
	CHECK(fixture.ProjectsOpened == 0);
	CHECK(fixture.Context.GetProject() == nullptr);

	std::filesystem::path const target = FileSystem::PathFromUtf8(directory);
	CHECK(FileSystem::IsRegularFile(target / "Tiny.sproj"));
	CHECK(FileSystem::IsRegularFile(target / "Assets" / "Scenes" / "Main.sscene"));
	CHECK_FALSE(FileSystem::Exists(target / "Tools"));
	std::string const project = Read(target / "Scripts" / "Tiny.csproj");
	CHECK(project.find("Strada-ScriptCore") == std::string::npos);
	CHECK(project.find("RootNamespace") != std::string::npos);
	// The example itself is untouched.
	CHECK(Read(GetContent() / "Examples" / "Tiny" / "Scripts" / "Tiny.csproj").find("Strada-ScriptCore") != std::string::npos);

	// The same directory again is not new or empty.
	CommandError const taken =
		fixture.Fails("project.copy-example", Json::object({{"example", "tiny"}, {"directory", directory}, {"open", false}}));
	CHECK(taken.Code == AutomationErrorCode::InvalidParams);
	CHECK(taken.Data["path"] == "directory");
}

TEST_CASE("ProjectContentCommands: project.copy-example opens the copy, and relative directories are the open project's")
{
	ContentFixture fixture;
	Json const copied = fixture.Ok("project.copy-example", Json::object({{"example", "tiny"}, {"directory", fixture.Path("First")}}));
	CHECK(fixture.ProjectsOpened == 1);
	REQUIRE(fixture.Context.GetProject() != nullptr);
	CHECK(copied["project"]["name"] == "Tiny");
	CHECK(copied["project"]["file"] == fixture.Path("First/Tiny.sproj"));
	CHECK(copied["scene"]["path"] == fixture.Path("First/Assets/Scenes/Main.sscene"));
	CHECK(copied["warnings"] == Json::array());
	CHECK(fixture.Context.GetProject()->GetFilePath() == FileSystem::PathFromUtf8(fixture.Path("First/Tiny.sproj")));

	// Relative to the open project's directory.
	Json const relative =
		fixture.Ok("project.copy-example", Json::object({{"example", "tiny"}, {"directory", "../Second"}, {"open", false}}));
	CHECK(relative["directory"] == fixture.Path("Second"));
	CHECK(FileSystem::IsRegularFile(fixture.Directory.GetPath() / "Second" / "Tiny.sproj"));
	Json const inside =
		fixture.Ok("project.copy-example", Json::object({{"example", "tiny"}, {"directory", "Copies/Third"}, {"open", false}}));
	CHECK(inside["directory"] == fixture.Path("First/Copies/Third"));
	// But not into its asset directory.
	CommandError const assets =
		fixture.Fails("project.copy-example", Json::object({{"example", "tiny"}, {"directory", "Assets/Copy"}, {"open", false}}));
	CHECK(assets.Code == AutomationErrorCode::InvalidParams);
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "First" / "Assets" / "Copy"));
	// Nor into it spelled in another letter case, where the file system takes that spelling for it.
	if (IgnoresLetterCase(fixture.Directory.GetPath() / "First" / "Assets"))
	{
		for (std::string const& spelled : {std::string("assets/Copy"), fixture.Path("FIRST/ASSETS/Copy")})
		{
			CAPTURE(spelled);
			CommandError const refused =
				fixture.Fails("project.copy-example", Json::object({{"example", "tiny"}, {"directory", spelled}, {"open", false}}));
			CHECK(refused.Code == AutomationErrorCode::InvalidParams);
			CHECK(refused.Data["path"] == "directory");
		}
		CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "First" / "Assets" / "Copy"));
	}
	CHECK(fixture.ProjectsOpened == 1);
}

TEST_CASE("ProjectContentCommands: project.copy-example refuses what it cannot copy before it writes anything")
{
	ContentFixture fixture;
	auto const fails = [&fixture](Json params)
	{
		return fixture.Fails("project.copy-example", std::move(params));
	};
	std::string const directory = fixture.Path("Copy");

	CommandError const unknown = fails(Json::object({{"example", "nope"}, {"directory", directory}}));
	CHECK(unknown.Code == AutomationErrorCode::InvalidParams);
	CHECK(unknown.Data["path"] == "example");
	CHECK(fails(Json::object({{"directory", directory}})).Code == AutomationErrorCode::InvalidParams);
	CHECK(fails(Json::object({{"example", "tiny"}})).Code == AutomationErrorCode::InvalidParams);
	CHECK(fails(Json::object({{"example", "tiny"}, {"directory", ""}})).Code == AutomationErrorCode::InvalidParams);

	// Relative without a project, control characters, a file, a folder that holds something, the example's own folder.
	CHECK(fails(Json::object({{"example", "tiny"}, {"directory", "Relative"}})).Code == AutomationErrorCode::InvalidParams);
	CHECK(fails(Json::object({{"example", "tiny"}, {"directory", directory + "\nName"}})).Code == AutomationErrorCode::InvalidParams);
	REQUIRE(FileSystem::WriteTextFile(fixture.Directory.GetPath() / "File.txt", "file").IsOk());
	CHECK(fails(Json::object({{"example", "tiny"}, {"directory", fixture.Path("File.txt")}})).Code == AutomationErrorCode::InvalidParams);
	CHECK(fails(Json::object({{"example", "tiny"}, {"directory", fixture.Path("")}})).Code == AutomationErrorCode::InvalidParams);
	CHECK(fails(Json::object({{"example", "tiny"}, {"directory", FileSystem::PathToUtf8(GetContent() / "Examples" / "Tiny" / "Inside")}}))
	          .Code == AutomationErrorCode::InvalidParams);
	CHECK_FALSE(FileSystem::Exists(GetContent() / "Examples" / "Tiny" / "Inside"));

	// Not available: the message carries the hint.
	CommandError const unavailable = fails(Json::object({{"example", "fetched"}, {"directory", directory}}));
	CHECK(unavailable.Code == AutomationErrorCode::InvalidOperation);
	CHECK(unavailable.Message.find("Run: python Tools/Fetch.py") != std::string::npos);
	CHECK(unavailable.Data["hint"].get<std::string>().starts_with("Run: "));

	// Unsaved changes stop an open before anything is copied; without open they do not matter.
	fixture.Ok("entity.create", Json::object({{"name", "Unsaved"}}));
	REQUIRE(fixture.Context.IsDirty());
	CommandError const unsaved = fails(Json::object({{"example", "tiny"}, {"directory", directory}}));
	CHECK(unsaved.Code == AutomationErrorCode::UnsavedChanges);
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Copy"));
	CHECK(fixture.Ok("project.copy-example", Json::object({{"example", "tiny"}, {"directory", directory}, {"open", false}}))["files"] == 5);
	CHECK(fixture.Ok("project.copy-example", Json::object({{"example", "tiny"},
	                                                       {"directory", fixture.Path("Discarded")},
	                                                       {"discardChanges", true}}))["project"]["name"] == "Tiny");
	CHECK(fixture.ProjectsOpened == 1);
}

TEST_CASE("ProjectContentCommands: one example is copied at a time, and a copy the editor abandons is cancelled")
{
	ContentFixture fixture;
	std::optional<CommandResult> first;
	fixture.Registry.Execute("project.copy-example",
	                         Json::object({{"example", "tiny"}, {"directory", fixture.Path("First")}, {"open", false}}),
	                         [&first](CommandResult value)
	                         {
								 first.emplace(std::move(value));
							 });
	CHECK_FALSE(first.has_value());
	CHECK(fixture.Copies->IsRunning());
	CommandError const busy =
		fixture.Fails("project.copy-example", Json::object({{"example", "tiny"}, {"directory", fixture.Path("Second")}}));
	CHECK(busy.Code == AutomationErrorCode::InvalidOperation);
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Second"));

	// The editor closes while it copies: the task goes, and the command answers once, as cancelled.
	fixture.Copies.reset();
	REQUIRE(first.has_value());
	REQUIRE(first->IsError());
	CHECK(first->GetError().Code == AutomationErrorCode::Cancelled);
}

TEST_CASE("ProjectContentCommands: a scene changed while the example was copied is not discarded to open the copy")
{
	ContentFixture fixture;
	std::optional<CommandResult> result;
	fixture.Registry.Execute("project.copy-example", Json::object({{"example", "tiny"}, {"directory", fixture.Path("Copy")}}),
	                         [&result](CommandResult value)
	                         {
								 result.emplace(std::move(value));
							 });
	// Commands run between frames while the copy runs.
	fixture.Ok("entity.create", Json::object({{"name", "Edited Meanwhile"}}));
	while (!result)
	{
		fixture.Copies->Update();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	REQUIRE(result->IsError());
	CHECK(result->GetError().Code == AutomationErrorCode::UnsavedChanges);
	CHECK(result->GetError().Message.find(fixture.Path("Copy/Tiny.sproj")) != std::string::npos);
	// The copy stays for project.open; the scene is the one being edited.
	CHECK(FileSystem::IsRegularFile(fixture.Directory.GetPath() / "Copy" / "Tiny.sproj"));
	CHECK(fixture.Context.GetProject() == nullptr);
	CHECK(fixture.Context.IsDirty());
	CHECK(fixture.ProjectsOpened == 0);
}

TEST_CASE("ProjectContentCommands: a copy that does not open stays, and the error says where it is")
{
	// An engine content whose one example has a project file that does not load.
	ContentFixture fixture(false);
	std::filesystem::path const content = fixture.Directory.GetPath() / "Content";
	REQUIRE(FileSystem::CopyDirectory(GetContent() / "Examples", content / "Examples").IsOk());
	REQUIRE(FileSystem::WriteTextFile(content / "Examples" / "Tiny" / "Tiny.sproj", "{ damaged").IsOk());
	Result<EngineLayout> layout = EngineLayout::Resolve(content / "StradaEditor");
	REQUIRE(layout.IsOk());
	EditorLocations locations;
	locations.Layout = layout.TakeValue();
	CommandRegistry registry;
	REQUIRE(RegisterProjectContentCommands(registry, fixture.Operations, *fixture.Copies, locations, {}).IsOk());

	std::optional<CommandResult> result;
	registry.Execute("project.copy-example", Json::object({{"example", "tiny"}, {"directory", fixture.Path("Broken")}}),
	                 [&result](CommandResult value)
	                 {
						 result.emplace(std::move(value));
					 });
	while (!result)
	{
		fixture.Copies->Update();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	REQUIRE(result->IsError());
	CHECK(result->GetError().Code == AutomationErrorCode::FileError);
	CHECK(result->GetError().Message.find(fixture.Path("Broken/Tiny.sproj")) != std::string::npos);
	CHECK(FileSystem::IsRegularFile(fixture.Directory.GetPath() / "Broken" / "Tiny.sproj"));
	CHECK(fixture.Context.GetProject() == nullptr);
}

TEST_CASE("ProjectContentCommands: project.create starts from a template, with a .gitignore unless asked not to")
{
	ContentFixture fixture;
	Json const created =
		fixture.Ok("project.create", Json::object({{"directory", fixture.Path("Orb")}, {"name", "Orb Run"}, {"template", "tiny-project"}}));
	CHECK(created["template"] == "tiny-project");
	CHECK(created["project"]["name"] == "Orb Run");
	CHECK(created["project"]["file"] == fixture.Path("Orb/Orb Run.sproj"));
	CHECK(created["scene"]["path"] == fixture.Path("Orb/Assets/Scenes/Main.sscene"));
	CHECK(fixture.ProjectsOpened == 1);
	CHECK(Read(fixture.Directory.GetPath() / "Orb" / ".gitignore") == ProjectTemplate::GitIgnoreText);
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Orb" / "Tools"));
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Orb" / "template.json"));
	CHECK(Read(fixture.Directory.GetPath() / "Orb" / "Scripts" / "Game.csproj").find("Strada-ScriptCore") == std::string::npos);

	// The default: empty-3d, the editor's default scene.
	Json const plain =
		fixture.Ok("project.create",
	               Json::object({{"directory", fixture.Path("Plain")}, {"name", "Plain"}, {"gitIgnore", false}, {"discardChanges", true}}));
	CHECK(plain["template"] == "empty-3d");
	CHECK(plain["scene"]["path"] == fixture.Path("Plain/Assets/Scenes/Main.sscene"));
	CHECK(plain["scene"]["entityCount"].get<int>() > 0);
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Plain" / ".gitignore"));
	CHECK(fixture.ProjectsOpened == 2);
	CHECK(AssetManager::GetMetadata(fixture.Context.GetProject()->GetSettings().StartScene)->Path == "Scenes/Main.sscene");
}

TEST_CASE("ProjectContentCommands: an editor without engine content still creates projects from the default scene")
{
	ContentFixture bare(false);
	Json const synthesized = bare.Ok("project.create", Json::object({{"directory", bare.Path("Game")}, {"name", "Game"}}));
	CHECK(synthesized["template"] == "empty-3d");
	CHECK(FileSystem::Exists(bare.Directory.GetPath() / "Game" / ".gitignore"));
}

TEST_CASE("ProjectContentCommands: project.copy-example opens no copy while the editor plays")
{
	ContentFixture fixture;
	fixture.Context.BeginPlay(CreateRef<Scene>("Running"), EditorPlayState::Play);
	CommandError const playing =
		fixture.Fails("project.copy-example", Json::object({{"example", "tiny"}, {"directory", fixture.Path("Copy")}}));
	CHECK(playing.Code == AutomationErrorCode::InvalidOperation);
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Copy"));
	// A copy that is not opened is made while playing.
	CHECK(fixture.Ok("project.copy-example",
	                 Json::object({{"example", "tiny"}, {"directory", fixture.Path("Copy")}, {"open", false}}))["files"] == 5);
	fixture.Context.EndPlay();

	// Play starts while the example is copied: the copy stays, and the error says where it is.
	std::optional<CommandResult> result;
	fixture.Registry.Execute("project.copy-example", Json::object({{"example", "tiny"}, {"directory", fixture.Path("Meanwhile")}}),
	                         [&result](CommandResult value)
	                         {
								 result.emplace(std::move(value));
							 });
	fixture.Context.BeginPlay(CreateRef<Scene>("Running"), EditorPlayState::Play);
	fixture.Await(result);
	REQUIRE(result->IsError());
	CHECK(result->GetError().Code == AutomationErrorCode::InvalidOperation);
	CHECK(result->GetError().Message.find(fixture.Path("Meanwhile/Tiny.sproj")) != std::string::npos);
	CHECK(FileSystem::IsRegularFile(fixture.Directory.GetPath() / "Meanwhile" / "Tiny.sproj"));
	CHECK(fixture.Context.GetProject() == nullptr);
	CHECK(fixture.ProjectsOpened == 0);
	fixture.Context.EndPlay();
}

TEST_CASE("ProjectContentCommands: a copy cancelled through its task answers Cancelled and leaves nothing behind")
{
	ContentFixture fixture;
	std::filesystem::path const empty = fixture.Directory.GetPath() / "Empty";
	REQUIRE(FileSystem::CreateDirectories(empty).IsOk());
	// Into a new directory and an empty one: cancelled before or after its thread finished copying (Cancel waits for nothing),
	// the copy goes either way.
	for (std::filesystem::path const& target : {fixture.Directory.GetPath() / "New" / "Copy", empty})
	{
		CAPTURE(FileSystem::PathToUtf8(target));
		std::optional<CommandResult> result;
		fixture.Registry.Execute("project.copy-example",
		                         Json::object({{"example", "tiny"}, {"directory", FileSystem::PathToUtf8(target)}, {"open", false}}),
		                         [&result](CommandResult value)
		                         {
									 result.emplace(std::move(value));
								 });
		REQUIRE(fixture.Copies->IsRunning());
		fixture.Copies->Cancel();
		fixture.Await(result);
		REQUIRE(result->IsError());
		CHECK(result->GetError().Code == AutomationErrorCode::Cancelled);
		CHECK_FALSE(fixture.Copies->IsRunning());
	}
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "New"));
	CHECK(FileSystem::IsDirectory(empty));
	CHECK(std::filesystem::is_empty(empty));
	CHECK(fixture.ProjectsOpened == 0);
}

TEST_CASE("ExampleCopyTask: a cancelled copy completes as cancelled, also when its thread had finished copying")
{
	Testing::TemporaryDirectory directory;
	Result<ExampleCatalog> catalog = ExampleCatalog::Load(GetContent() / "Examples" / "catalog.json");
	REQUIRE(catalog.IsOk());
	ExampleEntry const* tiny = catalog.GetValue().Find("tiny");
	REQUIRE(tiny != nullptr);
	ExampleCopyTask copies;
	// Once at once, once after every file was copied (its thread may then have finished, or not).
	for (bool const waitForFiles : {false, true})
	{
		CAPTURE(waitForFiles);
		std::filesystem::path const target = directory.GetPath() / (waitForFiles ? "Late" : "Early") / "Copy";
		std::optional<std::pair<bool, bool>> completed;
		REQUIRE(copies
		            .Start(catalog.GetValue(), *tiny, target,
		                   [&completed](Result<ExampleCopyOutcome> outcome, bool cancelled)
		                   {
							   completed.emplace(outcome.IsOk(), cancelled);
						   })
		            .IsOk());
		auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
		while (waitForFiles && std::chrono::steady_clock::now() < deadline &&
		       (copies.GetProgress().TotalFiles.load() == 0 || copies.GetProgress().Files.load() < copies.GetProgress().TotalFiles.load()))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		copies.Cancel();
		while (!completed && std::chrono::steady_clock::now() < deadline)
		{
			copies.Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE(completed.has_value());
		CHECK_FALSE(completed->first);
		CHECK(completed->second);
		CHECK_FALSE(copies.IsRunning());
		CHECK_FALSE(FileSystem::Exists(target.parent_path()));
	}

	// Without Cancel, the copy stays.
	std::optional<bool> copied;
	REQUIRE(copies
	            .Start(catalog.GetValue(), *tiny, directory.GetPath() / "Kept",
	                   [&copied](Result<ExampleCopyOutcome> outcome, bool)
	                   {
						   copied.emplace(outcome.IsOk());
					   })
	            .IsOk());
	auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
	while (!copied && std::chrono::steady_clock::now() < deadline)
	{
		copies.Update();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(copied.has_value());
	CHECK(*copied);
	CHECK(FileSystem::IsRegularFile(directory.GetPath() / "Kept" / "Tiny.sproj"));
}

TEST_CASE("ProjectContentCommands: a copy that fails answers FileError and leaves nothing behind")
{
	ContentFixture fixture;
	// A folder below a file passes the checks of a new directory and cannot be made.
	REQUIRE(FileSystem::WriteTextFile(fixture.Directory.GetPath() / "File.txt", "file").IsOk());
	CommandError const failed = fixture.Fails(
		"project.copy-example", Json::object({{"example", "tiny"}, {"directory", fixture.Path("File.txt/Copy")}, {"open", false}}));
	CHECK(failed.Code == AutomationErrorCode::FileError);
	CHECK(Read(fixture.Directory.GetPath() / "File.txt") == "file");
	CHECK_FALSE(fixture.Copies->IsRunning());
	CHECK(fixture.ProjectsOpened == 0);
}

TEST_CASE("ProjectContentCommands: an example's copy leaves its root .gitignore and is refused in its folder under any spelling")
{
	ContentFixture fixture;
	CommandRegistry registry;
	std::filesystem::path const examples = fixture.RegisterCopiedContent(registry);
	// The repository's rules for the example: one ignoring files the copy has (as the Showcase's ignores its downloads).
	REQUIRE(FileSystem::WriteTextFile(examples / "Tiny" / ".gitignore", "/Assets/Scenes/\n").IsOk());
	REQUIRE(FileSystem::WriteTextFile(examples / "Tiny" / "Scripts" / ".gitignore", "bin/\n").IsOk());

	CommandResult copied = fixture.Run(registry, "project.copy-example",
	                                   Json::object({{"example", "tiny"}, {"directory", fixture.Path("Copy")}, {"open", false}}));
	REQUIRE_MESSAGE(copied.IsOk(), (copied.IsError() ? copied.GetError().Message : std::string()));
	CHECK(copied.GetValue()["files"] == 6);
	std::filesystem::path const copy = fixture.Directory.GetPath() / "Copy";
	CHECK_FALSE(FileSystem::Exists(copy / ".gitignore"));
	CHECK(Read(copy / "Scripts" / ".gitignore") == "bin/\n");
	CHECK(FileSystem::IsRegularFile(copy / "Assets" / "Scenes" / "Main.sscene"));

	if (IgnoresLetterCase(examples / "Tiny"))
	{
		CommandResult const inside = fixture.Run(
			registry, "project.copy-example",
			Json::object({{"example", "tiny"}, {"directory", FileSystem::PathToUtf8(examples / "TINY" / "Inside")}, {"open", false}}));
		REQUIRE(inside.IsError());
		CHECK(inside.GetError().Code == AutomationErrorCode::InvalidParams);
		CHECK_FALSE(FileSystem::Exists(examples / "Tiny" / "Inside"));
	}
}
