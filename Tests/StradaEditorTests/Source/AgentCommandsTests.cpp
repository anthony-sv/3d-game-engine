#include "EditorLocationsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Agents/AgentKit.h"
#include "Editor/Automation/AgentCommands.h"
#include "Editor/Automation/EditorCommands.h"
#include "Editor/EditorOperations.h"

#include "Strada/Core/EngineLayout.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace Strada;

namespace
{
	// The editor's commands as the editor layer registers them, with the test content as the engine content, and a project
	// that the test creates.
	struct AgentCommandFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		CommandRegistry Registry;

		explicit AgentCommandFixture(std::optional<EditorLocations> locations = std::nullopt, AgentToolLookup findTool = {})
		{
			EditorCommandEnvironment environment;
			environment.Locations = locations ? std::move(*locations) : Testing::MakeEditorLocations(Directory.GetPath());
			environment.FindToolOnPath = std::move(findTool);
			REQUIRE(RegisterEditorCommands(Registry, Operations, std::move(environment)).IsOk());
		}

		std::filesystem::path CreateProject(std::string const& name = "Orb Run!")
		{
			std::filesystem::path const directory = Directory.GetPath() / "Game";
			REQUIRE(Operations.CreateProject(directory, name, Scene("Main")).IsOk());
			return directory;
		}

		CommandResult Run(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			return std::move(*result);
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
			return result.TakeError();
		}
	};

	bool IsEmptyOrMissing(std::filesystem::path const& directory, std::filesystem::path const& except)
	{
		std::error_code error;
		for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(directory, error))
		{
			if (entry.path().filename() != except)
			{
				return false;
			}
		}
		return true;
	}

	// The files a new project holds before it is set up for agents.
	bool HoldsNoAgentFile(std::filesystem::path const& project)
	{
		for (std::string_view const file : {"AGENTS.md", "CLAUDE.md", ".mcp.json", ".gitignore", ".claude", ".strada"})
		{
			if (FileSystem::Exists(project / FileSystem::PathFromUtf8(file)))
			{
				return false;
			}
		}
		return true;
	}
}

TEST_CASE("AgentCommands: project.agents and project.setup-agents are registered, documented as not undoable, and need a project")
{
	AgentCommandFixture fixture;
	CommandDefinition const* const status = fixture.Registry.Find("project.agents");
	CommandDefinition const* const setup = fixture.Registry.Find("project.setup-agents");
	REQUIRE(status != nullptr);
	REQUIRE(setup != nullptr);
	CHECK(status->ReadOnly);
	CHECK_FALSE(setup->ReadOnly);
	CHECK(setup->Description.find("not an undo step") != std::string::npos);
	CHECK(setup->Description.find("ConsentRequired") != std::string::npos);

	CHECK(fixture.Fails("project.agents").Code == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("project.setup-agents", Json::object({{"confirmed", true}})).Code == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("project.setup-agents", Json::object({{"dryRun", true}})).Code == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("project.setup-agents", Json::object({{"confirmed", "yes"}})).Code == AutomationErrorCode::InvalidParams);
}

TEST_CASE(
	"AgentCommands: project.setup-agents needs the user's consent (1010 ConsentRequired), and a dry run needs none and writes nothing")
{
	AgentCommandFixture fixture;
	std::filesystem::path const project = fixture.CreateProject();
	CHECK(static_cast<int32_t>(AutomationErrorCode::ConsentRequired) == 1010);
	CHECK(GetAutomationErrorCodeName(AutomationErrorCode::ConsentRequired) == "ConsentRequired");

	for (Json const& params : {Json::object(), Json::object({{"confirmed", false}}), Json::object({{"dryRun", false}})})
	{
		CAPTURE(params.dump());
		CommandError const refused = fixture.Fails("project.setup-agents", params);
		CHECK(refused.Code == AutomationErrorCode::ConsentRequired);
		CHECK(refused.Message.find("ask the user, then pass confirmed: true") != std::string::npos);
	}
	CHECK(HoldsNoAgentFile(project));

	for (Json const& params : {Json::object({{"dryRun", true}}), Json::object({{"dryRun", true}, {"confirmed", true}})})
	{
		CAPTURE(params.dump());
		Json const plan = fixture.Ok("project.setup-agents", params);
		REQUIRE(plan["files"].size() == AgentKit::ListFiles(Testing::MakeEditorLocations(fixture.Directory.GetPath()).Layout).size());
		for (Json const& file : plan["files"])
		{
			CAPTURE(file.dump());
			CHECK(file["action"] == "Create");
			CHECK(file["diff"].get<std::string>().starts_with("@@ -0,0 +1,"));
		}
		CHECK(plan["warnings"].is_array());
	}
	CHECK(HoldsNoAgentFile(project));
}

TEST_CASE("AgentCommands: a confirmed setup writes the kit, project.agents reports it current, and a second setup changes nothing")
{
	AgentCommandFixture fixture;
	std::filesystem::path const project = fixture.CreateProject();
	Json const before = fixture.Ok("project.agents");
	CHECK(before["tool"] == "strada");
	for (Json const& file : before["files"])
	{
		CAPTURE(file.dump());
		CHECK(file["state"] == "Missing");
	}

	Json const setup = fixture.Ok("project.setup-agents", Json::object({{"confirmed", true}}));
	REQUIRE(setup["files"].size() == before["files"].size());
	CHECK(setup["files"][0]["path"] == "AGENTS.md");
	CHECK(setup["files"][0]["action"] == "Create");
	bool warned = false;
	for (Json const& warning : setup["warnings"])
	{
		warned = warned || warning.get<std::string>().find("strada is not beside the editor") != std::string::npos;
	}
	CHECK(warned);

	// The block and .mcp.json name the project file, which is not the project's name.
	REQUIRE(FileSystem::IsRegularFile(project / "Orb Run_.sproj"));
	Result<std::string> const agents = FileSystem::ReadTextFile(project / "AGENTS.md");
	REQUIRE(agents.IsOk());
	CHECK(agents.GetValue().find("The project \"Orb Run!\" opens from `Orb Run_.sproj`") != std::string::npos);
	Result<std::string> const mcp = FileSystem::ReadTextFile(project / ".mcp.json");
	REQUIRE(mcp.IsOk());
	CHECK(Json::parse(mcp.GetValue())["mcpServers"]["strada"] ==
	      Json::parse(R"({"command": "strada", "args": ["mcp", "--project", "Orb Run_.sproj"]})"));

	Json const after = fixture.Ok("project.agents");
	for (Json const& file : after["files"])
	{
		CAPTURE(file.dump());
		CHECK(file["state"] == "Current");
	}
	Json const again = fixture.Ok("project.setup-agents", Json::object({{"confirmed", true}}));
	for (Json const& file : again["files"])
	{
		CAPTURE(file.dump());
		CHECK(file["action"] == "Unchanged");
		CHECK(file["diff"] == "");
	}

	// Setting up records no undo step and leaves the scene as it was.
	Json const status = fixture.Ok("editor.status");
	CHECK(status["undo"].is_null());
	CHECK(status["scene"]["dirty"] == false);
}

TEST_CASE("AgentCommands: a file that cannot be written is a FileError naming it")
{
	AgentCommandFixture fixture;
	std::filesystem::path const project = fixture.CreateProject();
	REQUIRE(FileSystem::WriteTextFile(project / "CLAUDE.md", "Mine.\n").IsOk());
	Testing::ReadOnlyFileScope const readOnly(project / "CLAUDE.md");
	CommandError const error = fixture.Fails("project.setup-agents", Json::object({{"confirmed", true}}));
	CHECK(error.Code == AutomationErrorCode::FileError);
	CHECK(error.Message.find("'CLAUDE.md' cannot be written") != std::string::npos);
	CHECK(error.Data["file"] == "CLAUDE.md");
	REQUIRE(error.Data["files"].size() == 1);
	CHECK(error.Data["files"][0]["path"] == "AGENTS.md");
	CHECK(FileSystem::IsRegularFile(project / "AGENTS.md"));
}

TEST_CASE("AgentCommands: an editor without engine content has no agent kit, and writes nothing")
{
	AgentCommandFixture fixture{EditorLocations()};
	std::filesystem::path const project = fixture.CreateProject();
	CHECK(fixture.Fails("project.agents").Code == AutomationErrorCode::FileError);
	CHECK(fixture.Fails("project.setup-agents", Json::object({{"dryRun", true}})).Code == AutomationErrorCode::FileError);
	CommandError const error = fixture.Fails("project.setup-agents", Json::object({{"confirmed", true}}));
	CHECK(error.Code == AutomationErrorCode::FileError);
	CHECK(error.Data.is_null());
	CHECK(HoldsNoAgentFile(project));
	CHECK(IsEmptyOrMissing(fixture.Directory.GetPath(), "Game"));
}

TEST_CASE("AgentCommands: the editor's tool lookup decides what .mcp.json names")
{
	Testing::TemporaryDirectory content;
	REQUIRE(FileSystem::CopyDirectory(Testing::GetTestContentDirectory(), content.GetPath() / "Engine").IsOk());
	Result<EngineLayout> layout = EngineLayout::Resolve(content.GetPath() / "Engine" / "StradaEditor");
	REQUIRE(layout.IsOk());
	std::filesystem::path const tool = layout.GetValue().GetToolPath();
	REQUIRE(FileSystem::WriteTextFile(tool, "a tool").IsOk());
	EditorLocations locations;
	locations.Layout = layout.TakeValue();

	// One editor at a time: each fixture holds the asset manager.
	{
		AgentCommandFixture onPath(locations,
		                           [&tool]
		                           {
									   return std::optional<std::filesystem::path>(tool);
								   });
		onPath.CreateProject();
		CHECK(onPath.Ok("project.agents")["tool"] == "strada");
	}
	{
		AgentCommandFixture notOnPath(locations);
		notOnPath.CreateProject();
		Json const status = notOnPath.Ok("project.agents");
		CHECK(status["tool"] == FileSystem::PathToUtf8(tool.lexically_normal()));
		CHECK(status["warnings"].dump().find("a path of this machine") != std::string::npos);
	}
}
