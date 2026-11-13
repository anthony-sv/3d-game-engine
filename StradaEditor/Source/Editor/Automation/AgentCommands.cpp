#include "Editor/Automation/AgentCommands.h"

#include "Editor/Automation/JsonSchema.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Project/Project.h"

#include <spdlog/fmt/fmt.h>

#include <memory>
#include <string>
#include <utility>

namespace Strada
{
	namespace
	{
		bool GetFlag(Json const& params, char const* key)
		{
			auto const member = params.find(key);
			return member != params.end() && member->is_boolean() && member->get<bool>();
		}

		Json DescribeWarnings(std::vector<std::string> const& warnings)
		{
			Json result = Json::array();
			for (std::string const& warning : warnings)
			{
				result.push_back(warning);
			}
			return result;
		}

		Json DescribePlanFiles(std::vector<AgentFilePlan> const& files)
		{
			Json result = Json::array();
			for (AgentFilePlan const& file : files)
			{
				result.push_back(
					Json::object({{"path", file.Path}, {"action", std::string(GetAgentFileActionName(file.Action))}, {"diff", file.Diff}}));
			}
			return result;
		}

		// What the commands share: the layout and the tool lookup, held as long as a command is registered.
		struct AgentCommandState
		{
			EditorContext& Context;
			EngineLayout Layout;
			AgentToolLookup FindTool;
		};

		CommandError NoProject()
		{
			return MakeCommandError(AutomationErrorCode::InvalidOperation, "no project is open");
		}
	}

	std::optional<AgentProject> GetAgentProject(EditorContext const& context)
	{
		Project const* const project = context.GetProject();
		if (project == nullptr)
		{
			return std::nullopt;
		}
		AgentProject agentProject;
		agentProject.Directory = project->GetDirectory();
		agentProject.Name = project->GetSettings().Name;
		agentProject.ProjectFile = FileSystem::PathToUtf8(project->GetFilePath().filename());
		return agentProject;
	}

	Result<void> RegisterAgentCommands(CommandRegistry& registry, EditorContext& context, EngineLayout layout, AgentToolLookup findTool)
	{
		auto const state = std::make_shared<AgentCommandState>(AgentCommandState{context, std::move(layout), std::move(findTool)});

		CommandDefinition status;
		status.Name = "project.agents";
		status.Description =
			"How far the open project is set up for coding agents: the state of each file project.setup-agents writes (AGENTS.md's "
			"strada-agents block, CLAUDE.md's @AGENTS.md line, .mcp.json's strada entry, the strada-make-game skill, the guides "
			"under .strada/docs/, the .strada/ line of .gitignore and the kit's state .strada/agents.json). Returns { files: [{ "
			"path, state }], tool, warnings }: state Missing, Current, Outdated (a setup updates it), UserModified (a whole file "
			"changed since Strada wrote it, which a setup keeps; the guides are replaced anyway) or Damaged (it cannot be read as "
			"what it should be, and a setup never rewrites it); tool is the command .mcp.json's strada entry names. Fails "
			"without a project, or with FileError when the engine's agent kit is incomplete.";
		status.ReadOnly = true;
		status.Handler = [state](Json const&) -> CommandResult
		{
			std::optional<AgentProject> const project = GetAgentProject(state->Context);
			if (!project)
			{
				return NoProject();
			}
			Result<AgentStatus> const agentStatus = AgentKit::GetStatus(*project, state->Layout, state->FindTool);
			if (!agentStatus)
			{
				return MakeCommandError(AutomationErrorCode::FileError, "{}", agentStatus.GetError());
			}
			Json files = Json::array();
			for (AgentFileStatus const& file : agentStatus.GetValue().Files)
			{
				files.push_back(Json::object({{"path", file.Path}, {"state", std::string(GetAgentFileStateName(file.State))}}));
			}
			return Json::object({{"files", std::move(files)},
			                     {"tool", agentStatus.GetValue().Tool},
			                     {"warnings", DescribeWarnings(agentStatus.GetValue().Warnings)}});
		};
		if (Result<void> registered = registry.Register(std::move(status)); !registered)
		{
			return registered;
		}

		CommandDefinition setup;
		setup.Name = "project.setup-agents";
		setup.Description =
			"Sets the open project up for coding agents: writes the strada-agents block of AGENTS.md (text outside its markers "
			"is kept), the @AGENTS.md line of CLAUDE.md, the strada entry of .mcp.json (other servers and members are kept), the "
			"strada-make-game skill under .claude/skills/, the guides under .strada/docs/, the .strada/ line of .gitignore and "
			"the state .strada/agents.json. Files the user changed and files that cannot be read as what they should be are left "
			"as they are, with a warning; running it again changes nothing. It writes files in the project directly and is not "
			"an undo step. It needs the user's consent: ask the user, then pass confirmed true; without it the command fails "
			"with ConsentRequired. dryRun true only answers what it would do. Returns { files: [{ path, action (Create, Update, "
			"Unchanged, SkipUserModified, SkipDamaged), diff (a unified-style diff of the managed part, empty unless created or "
			"updated) }], warnings }. Fails without a project, or with FileError when a file cannot be written (data.file "
			"names it; the files before it stay written) or the engine's agent kit is incomplete.";
		setup.Parameters =
			SchemaBuilder::Object()
				.Property("dryRun", SchemaBuilder::Boolean("Only answer what a setup would do, writing nothing").Default(false))
				.Property("confirmed",
		                  SchemaBuilder::Boolean("The user agreed to set the project up for coding agents (ask them first)").Default(false))
				.Build();
		setup.Handler = [state](Json const& params) -> CommandResult
		{
			std::optional<AgentProject> const project = GetAgentProject(state->Context);
			if (!project)
			{
				return NoProject();
			}
			bool const dryRun = GetFlag(params, "dryRun");
			if (!dryRun && !GetFlag(params, "confirmed"))
			{
				return MakeCommandError(AutomationErrorCode::ConsentRequired,
				                        "setting the project up for coding agents writes AGENTS.md, CLAUDE.md, .mcp.json, .gitignore, "
				                        "the strada-make-game skill and .strada/ in it: ask the user, then pass confirmed: true "
				                        "(dryRun: true shows what would change)");
			}
			if (dryRun)
			{
				Result<AgentPlan> const plan = AgentKit::Plan(*project, state->Layout, state->FindTool);
				if (!plan)
				{
					return MakeCommandError(AutomationErrorCode::FileError, "{}", plan.GetError());
				}
				return Json::object(
					{{"files", DescribePlanFiles(plan.GetValue().Files)}, {"warnings", DescribeWarnings(plan.GetValue().Warnings)}});
			}
			AgentApplyResult const applied = AgentKit::Apply(*project, state->Layout, state->FindTool);
			if (!applied.Succeeded())
			{
				CommandError error = MakeCommandError(AutomationErrorCode::FileError, "{}", applied.Error);
				if (!applied.FailedPath.empty())
				{
					error.Data = Json::object({{"file", applied.FailedPath},
					                           {"files", DescribePlanFiles(applied.Plan.Files)},
					                           {"warnings", DescribeWarnings(applied.Plan.Warnings)}});
				}
				return error;
			}
			return Json::object({{"files", DescribePlanFiles(applied.Plan.Files)}, {"warnings", DescribeWarnings(applied.Plan.Warnings)}});
		};
		return registry.Register(std::move(setup));
	}
}
