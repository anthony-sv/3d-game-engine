#include "TestUtilities.h"

#include "Editor/Automation/ScriptCommands.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Script/ScriptEngine.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <thread>

using namespace Strada;

namespace
{
	// Starts the scripting runtime for as long as it lives.
	class ScriptEngineRuntime
	{
	public:
		ScriptEngineRuntime()
		{
			Result<void> const initialized = ScriptEngine::Init();
			REQUIRE_MESSAGE(initialized.IsOk(), (initialized ? std::string() : initialized.GetError()));
		}

		~ScriptEngineRuntime()
		{
			if (ScriptEngine::IsInitialized())
			{
				ScriptEngine::Shutdown();
			}
		}

		ScriptEngineRuntime(ScriptEngineRuntime const&) = delete;
		ScriptEngineRuntime& operator=(ScriptEngineRuntime const&) = delete;
	};

	// The editor model with the script commands registered, as the editor layer sets them up.
	struct ScriptFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		CommandRegistry Registry;

		ScriptFixture() { REQUIRE(RegisterScriptCommands(Registry, Scripts, Context).IsOk()); }

		// Updates the scripts, as the editor does every frame, until the condition holds.
		void UpdateUntil(std::function<bool()> const& condition)
		{
			auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
			while (!condition() && std::chrono::steady_clock::now() < deadline)
			{
				Scripts.Update();
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

	// Writes a source with a modification time past every earlier one, so the change is noticed even on file systems with
	// coarse timestamps.
	void WriteSource(std::filesystem::path const& path, std::string_view text, std::chrono::hours later)
	{
		REQUIRE(FileSystem::WriteTextFile(path, text).IsOk());
		std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + later);
	}
}

TEST_CASE("ScriptCommands: every script command is documented in Docs/Automation.md")
{
	ScriptFixture fixture;
	Result<std::string> reference = FileSystem::ReadTextFile(STRADA_AUTOMATION_DOC_PATH);
	REQUIRE(reference.IsOk());
	CHECK(fixture.Registry.GetCommandCount() == 4);
	for (CommandDefinition const* command : fixture.Registry.GetCommands())
	{
		CAPTURE(command->Name);
		CHECK(reference.GetValue().find("`" + command->Name + "`") != std::string::npos);
	}
}

TEST_CASE("ScriptCommands: scripts are created, built, hot reloaded and reported through automation")
{
	ScriptEngineRuntime runtime;
	ScriptFixture fixture;

	// Nothing to do without a project, or before the first script.
	CHECK(fixture.Fails("script.create", Json::object({{"className", "Player"}})) == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("script.build") == AutomationErrorCode::InvalidOperation);
	REQUIRE(fixture.Operations.CreateProject(fixture.Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	fixture.Scripts.Update();
	Json const empty = fixture.Ok("script.status");
	CHECK(empty["available"] == true);
	CHECK(empty["scriptProject"] == false);
	CHECK(empty["lastBuild"].is_null());
	CHECK(fixture.Fails("script.build") == AutomationErrorCode::InvalidOperation);

	Json const created = fixture.Ok("script.create", Json::object({{"className", "Player"}}));
	CHECK(created["path"] == "Scripts/Source/Player.cs");
	CHECK(created["class"] == "Game.Player");
	CHECK(fixture.Fails("script.create", Json::object({{"className", "Player"}})) == AutomationErrorCode::FileError);
	CHECK(fixture.Fails("script.create", Json::object({{"className", "class"}})) == AutomationErrorCode::InvalidOperation);

	Json const built = fixture.Ok("script.build");
	CAPTURE(built.dump());
	CHECK(built["succeeded"] == true);
	CHECK(built["loaded"] == true);
	CHECK(built["errors"] == 0);
	Json const status = fixture.Ok("script.status");
	CHECK(status["loaded"] == true);
	CHECK(status["scriptProject"] == true);
	CHECK(status["lastBuild"]["succeeded"] == true);
	Json const classes = fixture.Ok("script.classes")["classes"];
	CHECK(std::any_of(classes.begin(), classes.end(),
	                  [](Json const& scriptClass)
	                  {
						  return scriptClass["name"] == "Game.Player";
					  }));

	// A compile error is reported with its file and line; the scripts built before stay loaded.
	std::filesystem::path const player = fixture.Directory.GetPath() / "Game" / "Scripts" / "Source" / "Player.cs";
	WriteSource(player, "using Strada;\n\nnamespace Game;\n\npublic class Player : Script\n{\n\tint m_Broken = ;\n}\n",
	            std::chrono::hours(1));
	Json const broken = fixture.Ok("script.build");
	CAPTURE(broken.dump());
	CHECK(broken["succeeded"] == false);
	CHECK(broken["loaded"] == false);
	Json const& diagnostics = broken["diagnostics"];
	CHECK(std::any_of(diagnostics.begin(), diagnostics.end(),
	                  [](Json const& diagnostic)
	                  {
						  return diagnostic["severity"] == "error" && diagnostic["line"] == 7 &&
		                         diagnostic["file"].get<std::string>().ends_with("Player.cs");
					  }));
	CHECK(ScriptEngine::FindClass("Game.Player") != nullptr);

	// Fixed sources build and load by themselves.
	WriteSource(player, "using Strada;\n\nnamespace Game;\n\npublic class Player : Script\n{\n\tpublic float Speed = 3.0f;\n}\n",
	            std::chrono::hours(2));
	fixture.UpdateUntil(
		[&fixture]
		{
			ScriptBuildReport const* const last = fixture.Scripts.GetLastBuild();
			return !fixture.Scripts.IsBuilding() && last != nullptr && last->Succeeded;
		});
	ScriptClassInfo const* const reloaded = ScriptEngine::FindClass("Game.Player");
	REQUIRE(reloaded != nullptr);
	CHECK(reloaded->FindField("Speed") != nullptr);

	// The project's scripts go with it.
	REQUIRE(fixture.Operations.CloseProject().IsOk());
	fixture.Scripts.Update();
	CHECK_FALSE(ScriptEngine::HasGameAssembly());
	CHECK(fixture.Scripts.GetLastBuild() == nullptr);
}
