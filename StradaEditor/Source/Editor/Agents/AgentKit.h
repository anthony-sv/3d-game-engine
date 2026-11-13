#pragma once

#include "Strada/Core/EngineLayout.h"
#include "Strada/Core/Result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// What a file of a project set up for coding agents holds compared with what the agent kit would write. AGENTS.md,
	// CLAUDE.md, .mcp.json and .gitignore are judged by their managed part alone (the block, the line, the strada entry,
	// the .strada/ line); the skill, the guides and the state file are whole files.
	enum class AgentFileState : uint8_t
	{
		// The file does not exist.
		Missing = 0,
		// It holds what the kit would write.
		Current,
		// It differs, and the kit replaces its managed part: the kit wrote it and nobody changed it since, or the part is
		// the kit's alone.
		Outdated,
		// A whole file the kit wrote that someone changed since (or one it never wrote): a setup leaves it alone, except the
		// guides under .strada/docs/, which are always replaced.
		UserModified,
		// The kit cannot read the file as what it should be (unbalanced markers in AGENTS.md, a .mcp.json that is not a JSON
		// object, a folder, a symbolic link, a file too large to read): it is never rewritten.
		Damaged
	};

	// What a setup does to a file.
	enum class AgentFileAction : uint8_t
	{
		Create = 0,
		Update,
		Unchanged,
		SkipUserModified,
		SkipDamaged
	};

	// "Missing", "Current", "Outdated", "UserModified", "Damaged": the names project.agents reports.
	[[nodiscard]] std::string_view GetAgentFileStateName(AgentFileState state);
	// "Create", "Update", "Unchanged", "SkipUserModified", "SkipDamaged": the names project.setup-agents reports.
	[[nodiscard]] std::string_view GetAgentFileActionName(AgentFileAction action);

	struct AgentFileStatus
	{
		// Relative to the project's directory, with forward slashes.
		std::string Path;
		AgentFileState State = AgentFileState::Missing;
	};

	struct AgentFilePlan
	{
		// Relative to the project's directory, with forward slashes.
		std::string Path;
		AgentFileAction Action = AgentFileAction::Unchanged;
		// A short unified-style text of what changes in the managed part ("@@ -1,2 +1,3 @@" hunks with three lines of context,
		// at most AgentKit::MaxDiffLines lines and then one saying how many more, each line cut after
		// AgentKit::MaxDiffLineBytes bytes with a note of how many more it holds); empty unless the file is created or
		// updated.
		std::string Diff;
	};

	struct AgentPlan
	{
		// In the order of AgentKit::ListFiles.
		std::vector<AgentFilePlan> Files;
		// What the user should know: guides the engine lacks, where .mcp.json's tool comes from.
		std::vector<std::string> Warnings;
	};

	struct AgentStatus
	{
		// In the order of AgentKit::ListFiles.
		std::vector<AgentFileStatus> Files;
		// The command .mcp.json's strada entry names (AgentKit::ResolveTool).
		std::string Tool;
		std::vector<std::string> Warnings;
	};

	// What applying a setup did, and the failure that stopped it.
	struct AgentApplyResult
	{
		// The files it handled, written or left as they were: every file of the plan when it succeeded, those before
		// FailedPath when one could not be written.
		AgentPlan Plan;
		// The file that could not be written (relative, forward slashes); empty when the failure was not a file's (the
		// engine's agent kit is incomplete, and nothing was written) or when it succeeded.
		std::string FailedPath;
		// Why it stopped; empty when every file was written.
		std::string Error;

		bool Succeeded() const { return Error.empty(); }
	};

	// The project a setup is for.
	struct AgentProject
	{
		// The project's directory: everything the kit writes lies inside it.
		std::filesystem::path Directory;
		// The project's name (Project.Name), which the AGENTS.md block names.
		std::string Name;
		// The file name of the project file in Directory ("Orb Run.sproj"), which the block and .mcp.json name.
		std::string ProjectFile;
	};

	// What a setup includes besides the files every setup writes.
	struct AgentSetupOptions
	{
		// The .gitignore row: the .strada/ line added to the project's .gitignore, or the file created with it. False leaves
		// .gitignore out of the status, the plan and the files (the New Project form's "Add a .gitignore" unchecked).
		bool GitIgnore = true;
	};

	// Finds the strada tool on PATH. The editor's is Process::FindExecutable("strada"); an unset lookup finds nothing, so
	// tests never depend on what the machine has.
	using AgentToolLookup = std::function<std::optional<std::filesystem::path>()>;

	// The agent kit: the files that set a project up for coding agents (an AGENTS.md block, CLAUDE.md, .mcp.json's strada
	// entry, the game-making skill, the guides under .strada/docs/, the .strada/ line of .gitignore) and the state file
	// .strada/agents.json, which records the hashes of the whole files the kit last wrote. The text a setup adds to a file
	// it keeps (AGENTS.md, CLAUDE.md, .mcp.json, .gitignore) changes nothing outside the managed part, a UTF-8 byte order
	// mark included. The guides' copies link to each other, and to the skill, where they lie in the project; a link to a
	// file a setup does not copy names that file's path in the engine's repository instead. The sources are the engine
	// content of a layout (EngineContent::AgentKit, MakeGameSkill and Docs). Reads and writes only inside the project's
	// directory and never follows a symbolic link there. Free of ImGui; any thread, one call at a time per project.
	class AgentKit
	{
	public:
		static constexpr std::string_view BeginMarker = "<!-- BEGIN strada-agents -->";
		static constexpr std::string_view EndMarker = "<!-- END strada-agents -->";
		// The block of AGENTS.md in the engine's agent kit, with {{ProjectName}} and {{ProjectFile}} to fill in.
		static constexpr std::string_view BlockFileName = "AGENTS.block.md";
		static constexpr std::string_view StateFilePath = ".strada/agents.json";
		static constexpr std::string_view StateFileType = "AgentKitState";
		static constexpr int StateFileVersion = 1;
		// The guides a setup copies from the engine's Docs to .strada/docs/, in this order.
		static constexpr std::array<std::string_view, 7> Guides = {
			"GettingStarted.md", "Scripting.md", "VisualScripting.md", "Automation.md", "Agents.md", "Components.md", "ScriptingAPI.md"};
		// Files larger than this are not read (and so never rewritten): Damaged in a project, missing in the engine.
		static constexpr uint64_t MaxFileBytes = 16ull * 1024 * 1024;
		static constexpr size_t MaxDiffLines = 200;
		// A diff line longer than this is cut (at a UTF-8 character's start) and ends with "... (N more bytes)".
		static constexpr size_t MaxDiffLineBytes = 400;

		// The files a setup of a project writes, relative with forward slashes, in the order plans list them: what the New
		// Project form shows before the project exists. Leaves out the guides the layout's Docs folder lacks.
		[[nodiscard]] static std::vector<std::string> ListFiles(EngineLayout const& layout, AgentSetupOptions const& options = {});

		// The state of every file of the kit in the project. Fails when the engine's agent kit is incomplete (its block or
		// the game-making skill cannot be read). Writes nothing.
		[[nodiscard]] static Result<AgentStatus> GetStatus(AgentProject const& project, EngineLayout const& layout,
		                                                   AgentToolLookup const& findTool, AgentSetupOptions const& options = {});
		// What a setup would do. Fails as GetStatus does. Writes nothing.
		[[nodiscard]] static Result<AgentPlan> Plan(AgentProject const& project, EngineLayout const& layout,
		                                            AgentToolLookup const& findTool, AgentSetupOptions const& options = {});
		// Sets the project up: writes every file the plan creates or updates, each atomically, then the state file. Running it
		// again changes nothing. A file that cannot be written (read-only, or the write fails) stops it there: the files
		// written before stay, and the state file records them.
		[[nodiscard]] static AgentApplyResult Apply(AgentProject const& project, EngineLayout const& layout,
		                                            AgentToolLookup const& findTool, AgentSetupOptions const& options = {});

		// The command of .mcp.json's strada entry: "strada" when the tool beside the editor (EngineLayout::GetToolPath) is the
		// strada findTool finds, or when there is no tool beside the editor (with a warning to put one on PATH); otherwise
		// the tool's absolute path with forward slashes (with a warning that .mcp.json then names a path of this machine).
		[[nodiscard]] static std::string ResolveTool(EngineLayout const& layout, AgentToolLookup const& findTool,
		                                             std::vector<std::string>* warnings = nullptr);
	};
}
