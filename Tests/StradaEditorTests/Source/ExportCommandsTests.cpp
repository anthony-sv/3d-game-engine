#include "TestUtilities.h"

#include "Editor/Automation/ExportCommands.h"
#include "Editor/Automation/ScriptCommands.h"
#include "Editor/EditorExport.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/PlayMode.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Project/GameExporter.h"

#include <doctest/doctest.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>

using namespace Strada;

namespace
{
	// The editor model with the export and script commands registered, as the editor layer sets them up.
	struct ExportFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};
		EditorExport Export{Context, Scripts};
		CommandRegistry Registry;

		ExportFixture()
		{
			REQUIRE(RegisterExportCommands(Registry, Export, Context).IsOk());
			REQUIRE(RegisterScriptCommands(Registry, Scripts, Context).IsOk());
		}

		// Updates the editor's scripts and export, as the editor does every frame, until the condition holds.
		void UpdateUntil(std::function<bool()> const& condition)
		{
			auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
			while (!condition() && std::chrono::steady_clock::now() < deadline)
			{
				Scripts.Update();
				Export.Update();
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			REQUIRE(condition());
		}

		CommandResult Run(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			UpdateUntil(
				[&result]
				{
					return result.has_value();
				});
			return std::move(*result);
		}

		Json Ok(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
			return result.TakeValue();
		}

		AutomationErrorCode Fails(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE(result.IsError());
			return result.GetError().Code;
		}
	};

	// Where Game.sgame and the rest of an exported game are: the app bundle's resources on macOS.
	std::filesystem::path GetGameDirectory(std::filesystem::path const& output, std::string const& gameName)
	{
		return GameExporter::GetHostPlatform() == ExportPlatform::MacOS
		           ? output / FileSystem::PathFromUtf8(gameName + ".app") / "Contents" / "Resources"
		           : output;
	}

	Json ReadGameFile(std::filesystem::path const& gameDirectory)
	{
		Result<std::string> text = FileSystem::ReadTextFile(gameDirectory / "Game.sgame");
		REQUIRE(text.IsOk());
		return Json::parse(text.GetValue());
	}
}

TEST_CASE("ExportCommands: project.export is documented in Docs/Automation.md")
{
	CommandRegistry registry;
	EditorContext context;
	EditorScripts scripts(context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll");
	EditorExport gameExport(context, scripts);
	REQUIRE(RegisterExportCommands(registry, gameExport, context).IsOk());
	Result<std::string> reference = FileSystem::ReadTextFile(STRADA_AUTOMATION_DOC_PATH);
	REQUIRE(reference.IsOk());
	CHECK(registry.GetCommandCount() == 1);
	CHECK(reference.GetValue().find("`project.export`") != std::string::npos);
}

TEST_CASE("ExportCommands: projects export as games with their scripts built in Release")
{
	ExportFixture fixture;
	CHECK(fixture.Fails("project.export") == AutomationErrorCode::InvalidOperation);
	REQUIRE(fixture.Operations.CreateProject(fixture.Directory.GetPath() / "Game", "Exported Game", Scene("Main")).IsOk());
	fixture.Scripts.Update();
	fixture.Ok("script.create", Json::object({{"className", "Mover"}}));

	// The project's Build directory by default.
	Json const exported = fixture.Ok("project.export");
	CAPTURE(exported.dump());
	std::filesystem::path const build = fixture.Directory.GetPath() / "Game" / "Build";
	CHECK(exported["succeeded"] == true);
	CHECK(exported["error"].is_null());
	CHECK(exported["directory"] == FileSystem::PathToUtf8(build));
	CHECK(exported["unsavedChanges"] == false);
	CHECK(exported["scripts"]["built"] == true);
	CHECK(exported["scripts"]["errors"] == 0);
	CHECK(FileSystem::Exists(FileSystem::PathFromUtf8(exported["executable"].get<std::string>())));
	std::filesystem::path const game = GetGameDirectory(build, "Exported Game");
	CHECK(ReadGameFile(game)["Game"]["ScriptModule"] == "Scripts/Game.dll");
	CHECK(FileSystem::IsRegularFile(game / "Scripts" / "Game.dll"));
	CHECK(FileSystem::IsRegularFile(game / "Scripts" / "Strada.ScriptCore.dll"));
	CHECK(FileSystem::IsRegularFile(game / "Assets" / "Scenes" / "Main.sscene"));

	// The game is built from the saved files, which the result points out.
	EntityCreateInfo entity;
	entity.Name = "Unsaved";
	REQUIRE(fixture.Operations.CreateEntity(entity).IsOk());
	Json const again = fixture.Ok("project.export", Json::object({{"directory", "Out/Game"}}));
	CAPTURE(again.dump());
	CHECK(again["unsavedChanges"] == true);
	CHECK(again["directory"] == FileSystem::PathToUtf8(fixture.Directory.GetPath() / "Game" / "Out" / "Game"));

	// The editor's own builds wait while the export builds the same C# project.
	REQUIRE(fixture.Export.Start(build).IsOk());
	CHECK(fixture.Fails("script.build") == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("project.export") == AutomationErrorCode::InvalidOperation);
	fixture.UpdateUntil(
		[&fixture]
		{
			return !fixture.Export.IsRunning();
		});

	// Scripts that do not compile stop the export, which leaves the earlier game.
	REQUIRE(FileSystem::WriteTextFile(fixture.Directory.GetPath() / "Game" / "Scripts" / "Source" / "Mover.cs",
	                                  "namespace Game;\n\npublic class Mover : Strada.Script\n{\n\tint m_Broken = ;\n}\n")
	            .IsOk());
	Json const broken = fixture.Ok("project.export");
	CAPTURE(broken.dump());
	CHECK(broken["succeeded"] == false);
	CHECK(broken["scripts"]["built"] == false);
	CHECK(broken["scripts"]["errors"].get<int>() > 0);
	CHECK(broken["executable"].is_null());
	CHECK(FileSystem::IsRegularFile(game / "Scripts" / "Game.dll"));

	// Directories that hold something else are not written to; playing editors do not export.
	REQUIRE(FileSystem::WriteTextFile(fixture.Directory.GetPath() / "Taken" / "Notes.txt", "mine").IsOk());
	CHECK(fixture.Fails("project.export", Json::object({{"directory", FileSystem::PathToUtf8(fixture.Directory.GetPath() / "Taken")}})) ==
	      AutomationErrorCode::FileError);
	REQUIRE(fixture.Play.Start(EditorPlayState::Simulate, 64, 64).IsOk());
	CHECK(fixture.Fails("project.export") == AutomationErrorCode::InvalidOperation);
	fixture.Play.Stop();
}
