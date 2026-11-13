#include "EditorLocationsTestUtilities.h"
#include "Fuzzing.h"
#include "TestUtilities.h"

#include "Editor/Agents/AgentKit.h"

#include "Strada/Core/EngineLayout.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Hash.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace Strada;

namespace
{
	constexpr std::string_view SkillPath = ".claude/skills/strada-make-game/SKILL.md";
	constexpr std::string_view GitIgnoreText = "# Strada: build output, caches and machine-specific files.\n"
											   "Build/\n"
											   ".strada/\n"
											   "Scripts/Binaries/\n"
											   "Scripts/bin/\n"
											   "Scripts/obj/\n"
											   "Scripts/Strada.props\n";

	std::string ReadFile(std::filesystem::path const& path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE_MESSAGE(text.IsOk(), FileSystem::PathToUtf8(path));
		return text.TakeValue();
	}

	void WriteFile(std::filesystem::path const& path, std::string_view text)
	{
		REQUIRE(FileSystem::WriteTextFile(path, text).IsOk());
	}

	std::string WithCrLf(std::string_view text)
	{
		std::string result;
		for (char const character : text)
		{
			if (character == '\n')
			{
				result += '\r';
			}
			result += character;
		}
		return result;
	}

	// Every file below a directory with its bytes, by path relative to it.
	std::map<std::string, std::string> Snapshot(std::filesystem::path const& directory)
	{
		std::map<std::string, std::string> files;
		std::error_code error;
		if (!std::filesystem::exists(directory, error))
		{
			return files;
		}
		for (std::filesystem::directory_entry const& entry : std::filesystem::recursive_directory_iterator(directory, error))
		{
			if (entry.is_regular_file(error))
			{
				files.emplace(FileSystem::PathToUtf8(entry.path().lexically_relative(directory)), ReadFile(entry.path()));
			}
		}
		return files;
	}

	// A project of the agent kit's tests: "Orb Run" with its project file "Orb Run.sproj", in a temporary directory, set up
	// with the test content's agent kit unless a test gives it another layout.
	struct KitFixture
	{
		Testing::TemporaryDirectory Directory;
		EditorLocations Locations = Testing::MakeEditorLocations(Directory.GetPath());
		EngineLayout Layout = Locations.Layout;
		AgentProject Project{Directory.GetPath() / "Orb Run", "Orb Run", "Orb Run.sproj"};
		AgentToolLookup FindTool;

		KitFixture() { REQUIRE(FileSystem::CreateDirectories(Project.Directory).IsOk()); }

		std::filesystem::path PathOf(std::string_view relative) const { return Project.Directory / FileSystem::PathFromUtf8(relative); }
		std::string Read(std::string_view relative) const { return ReadFile(PathOf(relative)); }
		void Write(std::string_view relative, std::string_view text) const { WriteFile(PathOf(relative), text); }

		std::map<std::string, AgentFileState> States() const
		{
			Result<AgentStatus> status = AgentKit::GetStatus(Project, Layout, FindTool);
			REQUIRE_MESSAGE(status.IsOk(), (status ? std::string() : status.GetError()));
			std::map<std::string, AgentFileState> states;
			for (AgentFileStatus const& file : status.GetValue().Files)
			{
				states.emplace(file.Path, file.State);
			}
			return states;
		}

		AgentPlan Plan() const
		{
			Result<AgentPlan> plan = AgentKit::Plan(Project, Layout, FindTool);
			REQUIRE_MESSAGE(plan.IsOk(), (plan ? std::string() : plan.GetError()));
			return plan.TakeValue();
		}

		std::map<std::string, AgentFileAction> Actions() const
		{
			std::map<std::string, AgentFileAction> actions;
			AgentPlan const plan = Plan();
			for (AgentFilePlan const& file : plan.Files)
			{
				actions.emplace(file.Path, file.Action);
			}
			return actions;
		}

		AgentApplyResult Apply() const
		{
			AgentApplyResult result = AgentKit::Apply(Project, Layout, FindTool);
			REQUIRE_MESSAGE(result.Succeeded(), result.Error);
			return result;
		}

		// The block AGENTS.md holds for this project, markers included, from the test content's AGENTS.block.md.
		std::string ExpectedBlock() const
		{
			std::string block = ReadFile(Testing::GetTestContentDirectory() / "Agents" / "AGENTS.block.md");
			std::string filled;
			for (size_t position = 0; position < block.size();)
			{
				if (block.compare(position, 15, "{{ProjectName}}") == 0)
				{
					filled += Project.Name;
					position += 15;
				}
				else if (block.compare(position, 15, "{{ProjectFile}}") == 0)
				{
					filled += Project.ProjectFile;
					position += 15;
				}
				else
				{
					filled += block[position++];
				}
			}
			return fmt::format("{}\n{}{}\n", AgentKit::BeginMarker, filled, AgentKit::EndMarker);
		}
	};

	// A copy of the test content in the temporary directory, to change what the engine's kit holds.
	EngineLayout MakeContentCopy(Testing::TemporaryDirectory const& directory, std::string_view name)
	{
		std::filesystem::path const content = directory.GetPath() / FileSystem::PathFromUtf8(name);
		REQUIRE(FileSystem::CopyDirectory(Testing::GetTestContentDirectory(), content).IsOk());
		Result<EngineLayout> layout = EngineLayout::Resolve(content / "StradaEditor");
		REQUIRE(layout.IsOk());
		return layout.TakeValue();
	}

	bool HasWarning(std::vector<std::string> const& warnings, std::string_view text)
	{
		return std::ranges::any_of(warnings,
		                           [text](std::string const& warning)
		                           {
									   return warning.find(text) != std::string::npos;
								   });
	}

	std::string HashOf(std::string_view text)
	{
		std::string withoutReturns(text);
		std::erase(withoutReturns, '\r');
		return fmt::format("{:016x}", Hash::FNV1a(withoutReturns));
	}
}

TEST_CASE("AgentKit: a setup creates every file of the kit, and a second one changes nothing")
{
	KitFixture fixture;
	std::vector<std::string> const files = AgentKit::ListFiles(fixture.Layout);
	CHECK(files == std::vector<std::string>{"AGENTS.md", "CLAUDE.md", ".mcp.json", std::string(SkillPath), ".strada/docs/GettingStarted.md",
	                                        ".strada/docs/Scripting.md", ".strada/docs/VisualScripting.md", ".strada/docs/Automation.md",
	                                        ".strada/docs/Agents.md", ".strada/docs/Components.md", ".strada/docs/ScriptingAPI.md",
	                                        ".strada/docs/README.md", ".gitignore", ".strada/agents.json"});

	// Every file is missing, and asking about them writes nothing.
	Result<AgentStatus> const status = AgentKit::GetStatus(fixture.Project, fixture.Layout, fixture.FindTool);
	REQUIRE(status.IsOk());
	REQUIRE(status.GetValue().Files.size() == files.size());
	for (size_t index = 0; index < files.size(); index++)
	{
		CAPTURE(files[index]);
		CHECK(status.GetValue().Files[index].Path == files[index]);
		CHECK(status.GetValue().Files[index].State == AgentFileState::Missing);
	}
	AgentPlan const plan = fixture.Plan();
	REQUIRE(plan.Files.size() == files.size());
	for (AgentFilePlan const& file : plan.Files)
	{
		CAPTURE(file.Path);
		CHECK(file.Action == AgentFileAction::Create);
		CHECK(file.Diff.starts_with("@@ -0,0 +1,"));
	}
	CHECK(Snapshot(fixture.Project.Directory).empty());

	AgentApplyResult const applied = fixture.Apply();
	CHECK(applied.FailedPath.empty());
	REQUIRE(applied.Plan.Files.size() == files.size());
	CHECK(fixture.Read("AGENTS.md") == fixture.ExpectedBlock());
	CHECK(fixture.Read("CLAUDE.md") == "@AGENTS.md\n");
	Result<Json> const mcp = ParseJson(fixture.Read(".mcp.json"));
	REQUIRE(mcp.IsOk());
	CHECK(mcp.GetValue() ==
	      Json::parse(R"({"mcpServers": {"strada": {"command": "strada", "args": ["mcp", "--project", "Orb Run.sproj"]}}})"));
	std::filesystem::path const content = Testing::GetTestContentDirectory();
	CHECK(fixture.Read(SkillPath) == ReadFile(content / "Agents" / "skills" / "strada-make-game" / "SKILL.md"));
	for (std::string_view const guide : AgentKit::Guides)
	{
		CAPTURE(guide);
		CHECK(fixture.Read(fmt::format(".strada/docs/{}", guide)) == ReadFile(content / "Docs" / FileSystem::PathFromUtf8(guide)));
	}
	CHECK(fixture.Read(".strada/docs/README.md").find("Strada manages this folder") != std::string::npos);
	CHECK(fixture.Read(".gitignore") == GitIgnoreText);

	// The state file records the whole files the kit wrote.
	Result<Json> const stateFile = ParseJson(fixture.Read(".strada/agents.json"));
	REQUIRE(stateFile.IsOk());
	CHECK(ReadFileHeader(stateFile.GetValue(), "AgentKitState", 1).IsOk());
	Json const& recorded = stateFile.GetValue().at("Files");
	CHECK(recorded.size() == 1 + AgentKit::Guides.size() + 1);
	CHECK(recorded.at(std::string(SkillPath)) == HashOf(fixture.Read(SkillPath)));
	CHECK(recorded.at(".strada/docs/README.md") == HashOf(fixture.Read(".strada/docs/README.md")));

	for (auto const& [path, state] : fixture.States())
	{
		CAPTURE(path);
		CHECK(state == AgentFileState::Current);
	}
	std::map<std::string, std::string> const before = Snapshot(fixture.Project.Directory);
	AgentApplyResult const again = fixture.Apply();
	for (AgentFilePlan const& file : again.Plan.Files)
	{
		CAPTURE(file.Path);
		CHECK(file.Action == AgentFileAction::Unchanged);
		CHECK(file.Diff.empty());
	}
	CHECK(Snapshot(fixture.Project.Directory) == before);
}

TEST_CASE("AgentKit: AGENTS.md keeps the text outside its markers byte for byte, line endings included")
{
	KitFixture fixture;
	std::string const block = fixture.ExpectedBlock();
	SUBCASE("a file without markers gets the block after one blank line")
	{
		fixture.Write("AGENTS.md", "# Mine\nNotes");
		CHECK(fixture.States()["AGENTS.md"] == AgentFileState::Outdated);
		fixture.Apply();
		CHECK(fixture.Read("AGENTS.md") == "# Mine\nNotes\n\n" + block);
	}
	SUBCASE("a file that already ends with a blank line gets no second one")
	{
		fixture.Write("AGENTS.md", "# Mine\n\n");
		fixture.Apply();
		CHECK(fixture.Read("AGENTS.md") == "# Mine\n\n" + block);
	}
	SUBCASE("an empty file gets the block alone")
	{
		fixture.Write("AGENTS.md", "");
		fixture.Apply();
		CHECK(fixture.Read("AGENTS.md") == block);
	}
	SUBCASE("a file with Windows line endings gets the block in them, and is current then")
	{
		fixture.Write("AGENTS.md", "# Mine\r\nNotes\r\n");
		fixture.Apply();
		CHECK(fixture.Read("AGENTS.md") == "# Mine\r\nNotes\r\n\r\n" + WithCrLf(block));
		CHECK(fixture.States()["AGENTS.md"] == AgentFileState::Current);
	}
	SUBCASE("an outdated block is replaced, and only the block")
	{
		std::string const before = "Before\r\n  <!-- BEGIN strada-agents -->  \r\nold text\r\n<!-- END strada-agents -->\r\nAfter\r\n";
		fixture.Write("AGENTS.md", before);
		CHECK(fixture.States()["AGENTS.md"] == AgentFileState::Outdated);
		AgentPlan const plan = fixture.Plan();
		auto const agents = std::ranges::find(plan.Files, std::string("AGENTS.md"), &AgentFilePlan::Path);
		REQUIRE(agents != plan.Files.end());
		CHECK(agents->Action == AgentFileAction::Update);
		CHECK(agents->Diff.find("\n-old text\n") != std::string::npos);
		CHECK(agents->Diff.find("\n+## Fixture agent block\n") != std::string::npos);
		fixture.Apply();
		std::string const crlfBlock = WithCrLf(block);
		CHECK(fixture.Read("AGENTS.md") == "Before\r\n" + crlfBlock.substr(0, crlfBlock.size() - 2) + "\r\nAfter\r\n");
		CHECK(fixture.States()["AGENTS.md"] == AgentFileState::Current);
	}
	SUBCASE("a block at the end without a final line ending keeps it so")
	{
		fixture.Write("AGENTS.md", "Mine\n<!-- BEGIN strada-agents -->\n<!-- END strada-agents -->");
		fixture.Apply();
		CHECK(fixture.Read("AGENTS.md") == "Mine\n" + block.substr(0, block.size() - 1));
		CHECK(fixture.States()["AGENTS.md"] == AgentFileState::Current);
	}
}

TEST_CASE("AgentKit: nested, repeated and unbalanced markers make AGENTS.md Damaged, and it is never rewritten")
{
	std::string_view const beginLine = "<!-- BEGIN strada-agents -->\n";
	std::string_view const endLine = "<!-- END strada-agents -->\n";
	for (std::string const& text :
	     {fmt::format("Mine\n{}", beginLine), fmt::format("Mine\n{}", endLine), fmt::format("{}x\n{}", endLine, beginLine),
	      fmt::format("{}{}x\n{}{}", beginLine, beginLine, endLine, endLine),
	      fmt::format("{}{}{}{}", beginLine, endLine, beginLine, endLine)})
	{
		CAPTURE(text);
		KitFixture fixture;
		fixture.Write("AGENTS.md", text);
		CHECK(fixture.States()["AGENTS.md"] == AgentFileState::Damaged);
		AgentPlan const plan = fixture.Plan();
		CHECK(fixture.Actions()["AGENTS.md"] == AgentFileAction::SkipDamaged);
		CHECK(HasWarning(plan.Warnings, "'AGENTS.md' is left as it is: its '<!-- BEGIN strada-agents -->'"));
		fixture.Apply();
		CHECK(fixture.Read("AGENTS.md") == text);
		// The rest of the kit is still set up.
		CHECK(fixture.Read("CLAUDE.md") == "@AGENTS.md\n");
		CHECK(fixture.States()["AGENTS.md"] == AgentFileState::Damaged);
	}
}

TEST_CASE("AgentKit: CLAUDE.md and .gitignore get their line once, after the user's text")
{
	KitFixture fixture;
	fixture.Write("CLAUDE.md", "Read the notes.");
	fixture.Write(".gitignore", "Build/\r\n*.log\r\n");
	auto states = fixture.States();
	CHECK(states["CLAUDE.md"] == AgentFileState::Outdated);
	CHECK(states[".gitignore"] == AgentFileState::Outdated);
	fixture.Apply();
	CHECK(fixture.Read("CLAUDE.md") == "Read the notes.\n@AGENTS.md\n");
	CHECK(fixture.Read(".gitignore") == "Build/\r\n*.log\r\n.strada/\r\n");
	states = fixture.States();
	CHECK(states["CLAUDE.md"] == AgentFileState::Current);
	CHECK(states[".gitignore"] == AgentFileState::Current);

	// The line counts however it is spaced, and .strada in its other spellings counts too.
	KitFixture spelled;
	spelled.Write("CLAUDE.md", "Notes\n  @AGENTS.md  \nMore\n");
	spelled.Write(".gitignore", "/.strada\n");
	states = spelled.States();
	CHECK(states["CLAUDE.md"] == AgentFileState::Current);
	CHECK(states[".gitignore"] == AgentFileState::Current);
}

TEST_CASE("AgentKit: .mcp.json gets its strada entry merged, with every other server and member kept")
{
	Json const entry = Json::parse(R"({"command": "strada", "args": ["mcp", "--project", "Orb Run.sproj"]})");
	SUBCASE("other servers, members and the file's indentation are kept")
	{
		KitFixture fixture;
		fixture.Write(".mcp.json", "{\n  \"mcpServers\": {\n    \"other\": {\"command\": \"other\", \"args\": [\"serve\"]}\n  },\n"
		                           "  \"note\": 7\n}\n");
		CHECK(fixture.States()[".mcp.json"] == AgentFileState::Outdated);
		fixture.Apply();
		std::string const text = fixture.Read(".mcp.json");
		CHECK(text.starts_with("{\n  \"mcpServers\": {\n    \"other\": {\n      \"command\": \"other\""));
		Json const written = Json::parse(text);
		CHECK(written["note"] == 7);
		CHECK(written["mcpServers"]["other"] == Json::parse(R"({"command": "other", "args": ["serve"]})"));
		CHECK(written["mcpServers"]["strada"] == entry);
		// The servers keep their order; strada comes after them.
		CHECK(written["mcpServers"].begin().key() == "other");
		CHECK(fixture.States()[".mcp.json"] == AgentFileState::Current);
	}
	SUBCASE("a foreign strada entry is Outdated, and a setup replaces it")
	{
		KitFixture fixture;
		fixture.Write(".mcp.json", R"({"mcpServers": {"strada": {"command": "dotnet", "args": ["run"]}, "other": {"command": "x"}}})");
		CHECK(fixture.States()[".mcp.json"] == AgentFileState::Outdated);
		AgentPlan const plan = fixture.Plan();
		auto const mcp = std::ranges::find(plan.Files, std::string(".mcp.json"), &AgentFilePlan::Path);
		REQUIRE(mcp != plan.Files.end());
		CHECK(mcp->Diff.find("dotnet") != std::string::npos);
		fixture.Apply();
		Json const written = Json::parse(fixture.Read(".mcp.json"));
		CHECK(written["mcpServers"]["strada"] == entry);
		CHECK(written["mcpServers"]["other"] == Json::parse(R"({"command": "x"})"));
		CHECK(written["mcpServers"].begin().key() == "strada");
	}
	SUBCASE("the kit's entry in another member order is current")
	{
		KitFixture fixture;
		fixture.Write(".mcp.json", R"({"mcpServers": {"strada": {"args": ["mcp", "--project", "Orb Run.sproj"], "command": "strada"}}})");
		CHECK(fixture.States()[".mcp.json"] == AgentFileState::Current);
	}
	SUBCASE("an empty file or object gets the entry")
	{
		for (std::string_view const text : {"", "  \n", "{}", "{\"mcpServers\": {}}"})
		{
			CAPTURE(text);
			KitFixture fixture;
			fixture.Write(".mcp.json", text);
			CHECK(fixture.States()[".mcp.json"] == AgentFileState::Outdated);
			fixture.Apply();
			CHECK(Json::parse(fixture.Read(".mcp.json"))["mcpServers"]["strada"] == entry);
			CHECK(fixture.States()[".mcp.json"] == AgentFileState::Current);
		}
	}
	SUBCASE("comments, trailing commas, arrays and servers that are no object make it Damaged, never rewritten")
	{
		for (std::string_view const text : {"{\n  // mine\n  \"mcpServers\": {}\n}\n", "{\"mcpServers\": {\"a\": {\"command\": \"a\"},}}",
		                                    "[]", "[{\"mcpServers\": {}}]", "{\"mcpServers\": []}", "\"text\"", "{\"mcpServers\": {"})
		{
			CAPTURE(text);
			KitFixture fixture;
			fixture.Write(".mcp.json", text);
			CHECK(fixture.States()[".mcp.json"] == AgentFileState::Damaged);
			AgentPlan const plan = fixture.Plan();
			CHECK(HasWarning(plan.Warnings, "'.mcp.json' is left as it is"));
			fixture.Apply();
			CHECK(fixture.Read(".mcp.json") == text);
		}
	}
}

TEST_CASE("AgentKit: whole files are current, outdated when the engine's changed, and user-modified when someone edited them")
{
	KitFixture fixture;
	fixture.Apply();
	std::string const oldSkill = fixture.Read(SkillPath);

	// The engine's skill and a guide change: the files the kit wrote are Outdated and a setup updates them.
	fixture.Layout = MakeContentCopy(fixture.Directory, "NewerEngine");
	std::filesystem::path const content = fixture.Layout.GetResourcesDirectory();
	WriteFile(content / "Agents" / "skills" / "strada-make-game" / "SKILL.md", oldSkill + "A new rule.\n");
	WriteFile(content / "Docs" / "Agents.md", "# Newer agents guide\n");
	auto states = fixture.States();
	CHECK(states[std::string(SkillPath)] == AgentFileState::Outdated);
	CHECK(states[".strada/docs/Agents.md"] == AgentFileState::Outdated);
	CHECK(states[".strada/docs/Scripting.md"] == AgentFileState::Current);
	CHECK(states[".strada/agents.json"] == AgentFileState::Outdated);
	AgentPlan const plan = fixture.Plan();
	auto const skill = std::ranges::find(plan.Files, std::string(SkillPath), &AgentFilePlan::Path);
	REQUIRE(skill != plan.Files.end());
	CHECK(skill->Action == AgentFileAction::Update);
	CHECK(skill->Diff.find("\n+A new rule.\n") != std::string::npos);
	fixture.Apply();
	CHECK(fixture.Read(SkillPath) == oldSkill + "A new rule.\n");
	CHECK(fixture.Read(".strada/docs/Agents.md") == "# Newer agents guide\n");

	// The user edits the skill and a guide: the skill is kept, the guide (Strada's folder) replaced.
	fixture.Write(SkillPath, "My own skill.\n");
	fixture.Write(".strada/docs/Agents.md", "My notes.\n");
	states = fixture.States();
	CHECK(states[std::string(SkillPath)] == AgentFileState::UserModified);
	CHECK(states[".strada/docs/Agents.md"] == AgentFileState::UserModified);
	auto const actions = fixture.Actions();
	CHECK(actions.at(std::string(SkillPath)) == AgentFileAction::SkipUserModified);
	CHECK(actions.at(".strada/docs/Agents.md") == AgentFileAction::Update);
	AgentApplyResult const applied = fixture.Apply();
	CHECK(HasWarning(applied.Plan.Warnings, fmt::format("'{}' was changed since Strada wrote it", SkillPath)));
	CHECK(fixture.Read(SkillPath) == "My own skill.\n");
	CHECK(fixture.Read(".strada/docs/Agents.md") == "# Newer agents guide\n");
	// The state keeps the hash of what the kit wrote, so the skill stays the user's.
	states = fixture.States();
	CHECK(states[std::string(SkillPath)] == AgentFileState::UserModified);
	CHECK(states[".strada/agents.json"] == AgentFileState::Current);

	// A checkout that changed the line endings leaves the files current.
	fixture.Write(SkillPath, WithCrLf(oldSkill + "A new rule.\n"));
	fixture.Write(".strada/docs/Scripting.md", WithCrLf(fixture.Read(".strada/docs/Scripting.md")));
	states = fixture.States();
	CHECK(states[std::string(SkillPath)] == AgentFileState::Current);
	CHECK(states[".strada/docs/Scripting.md"] == AgentFileState::Current);
}

TEST_CASE("AgentKit: a missing, damaged or newer state file counts as empty, and a setup replaces it")
{
	for (std::string_view const state : {"", "{", "[]", R"({"Strada": {"Version": 2, "Type": "AgentKitState"}, "Files": {}})",
	                                     R"({"Strada": {"Version": 1, "Type": "RecentProjects"}, "Files": {}})",
	                                     R"({"Strada": {"Version": 1, "Type": "AgentKitState"}, "Files": []})"})
	{
		CAPTURE(state);
		KitFixture fixture;
		fixture.Apply();
		std::string const skill = fixture.Read(SkillPath);
		// With the state lost, a skill the kit wrote and the engine changed since cannot be told from the user's.
		fixture.Layout = MakeContentCopy(fixture.Directory, "NewerEngine");
		WriteFile(fixture.Layout.GetResourcesDirectory() / "Agents" / "skills" / "strada-make-game" / "SKILL.md", skill + "More.\n");
		if (state.empty())
		{
			REQUIRE(FileSystem::Remove(fixture.PathOf(".strada/agents.json")).IsOk());
		}
		else
		{
			fixture.Write(".strada/agents.json", state);
		}
		auto const states = fixture.States();
		CHECK(states.at(std::string(SkillPath)) == AgentFileState::UserModified);
		CHECK(states.at(".strada/agents.json") == (state.empty() ? AgentFileState::Missing : AgentFileState::Outdated));
		fixture.Apply();
		CHECK(fixture.Read(SkillPath) == skill);
		CHECK(fixture.States().at(".strada/agents.json") == AgentFileState::Current);
		CHECK(ReadFileHeader(Json::parse(fixture.Read(".strada/agents.json")), "AgentKitState", 1).IsOk());
	}

	// A state file that is a folder is Damaged and left alone; the rest is still set up.
	KitFixture fixture;
	REQUIRE(FileSystem::CreateDirectories(fixture.PathOf(".strada/agents.json")).IsOk());
	CHECK(fixture.States()[".strada/agents.json"] == AgentFileState::Damaged);
	CHECK(fixture.Actions()[".strada/agents.json"] == AgentFileAction::SkipDamaged);
	fixture.Apply();
	CHECK(FileSystem::IsDirectory(fixture.PathOf(".strada/agents.json")));
	CHECK(fixture.Read("CLAUDE.md") == "@AGENTS.md\n");
}

TEST_CASE("AgentKit: .mcp.json names strada, or the tool's path when the strada on PATH is another")
{
	// The test content has no strada beside its editor: the name, and the advice to put it on PATH.
	{
		KitFixture fixture;
		std::vector<std::string> warnings;
		CHECK(AgentKit::ResolveTool(fixture.Layout, fixture.FindTool, &warnings) == "strada");
		CHECK(HasWarning(warnings, "strada is not beside the editor: put it on PATH"));
		CHECK(HasWarning(fixture.Plan().Warnings, "strada is not beside the editor"));
	}

	KitFixture fixture;
	fixture.Layout = MakeContentCopy(fixture.Directory, "Engine");
	std::filesystem::path const tool = fixture.Layout.GetToolPath();
	WriteFile(tool, "a tool");
	std::filesystem::path const other = fixture.Directory.GetPath() / "Elsewhere" / tool.filename();
	WriteFile(other, "another tool");
	std::string const toolPath = FileSystem::PathToUtf8(tool.lexically_normal());
	REQUIRE(toolPath.find('\\') == std::string::npos);
	SUBCASE("the strada on PATH is the one beside the editor")
	{
		fixture.FindTool = [&tool]
		{
			return std::optional<std::filesystem::path>(tool);
		};
		std::vector<std::string> warnings;
		CHECK(AgentKit::ResolveTool(fixture.Layout, fixture.FindTool, &warnings) == "strada");
		CHECK(warnings.empty());
	}
	SUBCASE("no strada on PATH")
	{
		std::vector<std::string> warnings;
		CHECK(AgentKit::ResolveTool(fixture.Layout, fixture.FindTool, &warnings) == toolPath);
		CHECK(HasWarning(warnings, "a path of this machine"));
		fixture.Apply();
		CHECK(Json::parse(fixture.Read(".mcp.json"))["mcpServers"]["strada"]["command"] == toolPath);
		Result<AgentStatus> const status = AgentKit::GetStatus(fixture.Project, fixture.Layout, fixture.FindTool);
		REQUIRE(status.IsOk());
		CHECK(status.GetValue().Tool == toolPath);
	}
	SUBCASE("another strada on PATH")
	{
		fixture.FindTool = [&other]
		{
			return std::optional<std::filesystem::path>(other);
		};
		CHECK(AgentKit::ResolveTool(fixture.Layout, fixture.FindTool) == toolPath);
	}
}

TEST_CASE("AgentKit: a file that cannot be written stops the setup there, naming it, and the files before it stay written")
{
	KitFixture fixture;
	fixture.Write("CLAUDE.md", "Mine.\n");
	Testing::ReadOnlyFileScope const readOnly(fixture.PathOf("CLAUDE.md"));
	AgentApplyResult const applied = AgentKit::Apply(fixture.Project, fixture.Layout, fixture.FindTool);
	CHECK_FALSE(applied.Succeeded());
	CHECK(applied.FailedPath == "CLAUDE.md");
	CHECK(applied.Error.find("'CLAUDE.md' cannot be written: the file is read-only") != std::string::npos);
	REQUIRE(applied.Plan.Files.size() == 1);
	CHECK(applied.Plan.Files[0].Path == "AGENTS.md");
	CHECK(fixture.Read("AGENTS.md") == fixture.ExpectedBlock());
	CHECK(fixture.Read("CLAUDE.md") == "Mine.\n");
	CHECK_FALSE(FileSystem::Exists(fixture.PathOf(".mcp.json")));
	CHECK_FALSE(FileSystem::Exists(fixture.PathOf(SkillPath)));
	// The state file records no whole file yet.
	Json const state = Json::parse(fixture.Read(".strada/agents.json"));
	CHECK(state["Files"].empty());
}

TEST_CASE("AgentKit: symbolic links in the project are never followed")
{
	KitFixture fixture;
	std::filesystem::path const outside = fixture.Directory.GetPath() / "Outside";
	REQUIRE(FileSystem::CreateDirectories(outside).IsOk());
	std::error_code error;
	std::filesystem::create_directory_symlink(outside, fixture.PathOf(".strada"), error);
	if (error)
	{
		MESSAGE("symbolic links cannot be made here (" << error.message() << "): skipped");
		return;
	}
	auto const states = fixture.States();
	CHECK(states.at(".strada/docs/README.md") == AgentFileState::Damaged);
	CHECK(states.at(".strada/agents.json") == AgentFileState::Damaged);
	AgentApplyResult const applied = fixture.Apply();
	CHECK(HasWarning(applied.Plan.Warnings, "'.strada' is a symbolic link"));
	CHECK(Snapshot(outside).empty());
	CHECK(fixture.Read("CLAUDE.md") == "@AGENTS.md\n");
}

TEST_CASE("AgentKit: an incomplete engine kit is an error, and a missing guide is left out with a warning")
{
	KitFixture fixture;
	SUBCASE("no engine content")
	{
		fixture.Layout = EngineLayout();
		CHECK_FALSE(AgentKit::GetStatus(fixture.Project, fixture.Layout, fixture.FindTool).IsOk());
		CHECK_FALSE(AgentKit::Plan(fixture.Project, fixture.Layout, fixture.FindTool).IsOk());
		AgentApplyResult const applied = AgentKit::Apply(fixture.Project, fixture.Layout, fixture.FindTool);
		CHECK_FALSE(applied.Succeeded());
		CHECK(applied.FailedPath.empty());
		CHECK(applied.Plan.Files.empty());
	}
	SUBCASE("no block")
	{
		fixture.Layout = MakeContentCopy(fixture.Directory, "Engine");
		REQUIRE(FileSystem::Remove(fixture.Layout.GetContentPath(EngineContent::AgentKit) / "AGENTS.block.md").IsOk());
		Result<AgentPlan> const plan = AgentKit::Plan(fixture.Project, fixture.Layout, fixture.FindTool);
		REQUIRE_FALSE(plan.IsOk());
		CHECK(plan.GetError().find("AGENTS.block.md") != std::string::npos);
	}
	SUBCASE("no skill")
	{
		fixture.Layout = MakeContentCopy(fixture.Directory, "Engine");
		REQUIRE(FileSystem::Remove(fixture.Layout.GetContentPath(EngineContent::MakeGameSkill) / "SKILL.md").IsOk());
		Result<AgentPlan> const plan = AgentKit::Plan(fixture.Project, fixture.Layout, fixture.FindTool);
		REQUIRE_FALSE(plan.IsOk());
		CHECK(plan.GetError().find("SKILL.md") != std::string::npos);
	}
	SUBCASE("a block with a marker line in it")
	{
		fixture.Layout = MakeContentCopy(fixture.Directory, "Engine");
		WriteFile(fixture.Layout.GetContentPath(EngineContent::AgentKit) / "AGENTS.block.md", "Text\n<!-- END strada-agents -->\n");
		CHECK_FALSE(AgentKit::Plan(fixture.Project, fixture.Layout, fixture.FindTool).IsOk());
	}
	SUBCASE("no guide on visual scripting")
	{
		fixture.Layout = MakeContentCopy(fixture.Directory, "Engine");
		REQUIRE(FileSystem::Remove(fixture.Layout.GetContentPath(EngineContent::Docs) / "VisualScripting.md").IsOk());
		std::vector<std::string> const files = AgentKit::ListFiles(fixture.Layout);
		CHECK(std::ranges::find(files, ".strada/docs/VisualScripting.md") == files.end());
		AgentApplyResult const applied = fixture.Apply();
		CHECK(HasWarning(applied.Plan.Warnings, "the engine has no guide 'VisualScripting.md'"));
		CHECK(applied.Plan.Files.size() == files.size());
		CHECK_FALSE(FileSystem::Exists(fixture.PathOf(".strada/docs/VisualScripting.md")));
		CHECK(FileSystem::Exists(fixture.PathOf(".strada/docs/Scripting.md")));
	}
	// Nothing was written into the project by the failures.
	if (FileSystem::Exists(fixture.PathOf("CLAUDE.md")))
	{
		CHECK(fixture.Read("CLAUDE.md") == "@AGENTS.md\n");
	}
}

TEST_CASE("AgentKit: names that look like placeholders or markers, or hold line breaks, keep the block intact")
{
	KitFixture fixture;
	fixture.Project.Name = "Orb {{ProjectFile}}\n<!-- END strada-agents -->\r\nRun";
	fixture.Apply();
	std::string const agents = fixture.Read("AGENTS.md");
	CHECK(agents.find("Orb {{ProjectFile}} <!-- END strada-agents -->  Run") != std::string::npos);
	CHECK(fixture.States()["AGENTS.md"] == AgentFileState::Current);
	CHECK(fixture.Actions()["AGENTS.md"] == AgentFileAction::Unchanged);
}

TEST_CASE("AgentKit: diffs are short unified-style texts")
{
	KitFixture fixture;
	fixture.Layout = MakeContentCopy(fixture.Directory, "Engine");
	std::string longGuide;
	for (int line = 0; line < 300; line++)
	{
		longGuide += fmt::format("Line {}\n", line);
	}
	WriteFile(fixture.Layout.GetContentPath(EngineContent::Docs) / "Components.md", longGuide);
	fixture.Write("AGENTS.md", "Keep\n<!-- BEGIN strada-agents -->\n## Fixture agent block\n\nOld line\n<!-- END strada-agents -->\n");
	AgentPlan const plan = fixture.Plan();
	auto const find = [&plan](std::string_view path) -> AgentFilePlan const&
	{
		auto const file = std::ranges::find(plan.Files, std::string(path), &AgentFilePlan::Path);
		REQUIRE(file != plan.Files.end());
		return *file;
	};
	// A created file: every line added, cut after MaxDiffLines lines.
	AgentFilePlan const& components = find(".strada/docs/Components.md");
	CHECK(components.Diff.starts_with("@@ -0,0 +1,300 @@\n+Line 0\n+Line 1\n"));
	CHECK(components.Diff.ends_with("\n... 101 more lines\n"));
	// An updated block: the changed lines with three lines of context.
	AgentFilePlan const& agents = find("AGENTS.md");
	CHECK(agents.Diff ==
	      "@@ -1,5 +1,5 @@\n <!-- BEGIN strada-agents -->\n ## Fixture agent block\n \n-Old line\n+The project \"Orb Run\" opens from "
	      "`Orb Run.sproj`; this line names Orb Run twice.\n <!-- END strada-agents -->\n");
	// Files left as they are have no diff.
	fixture.Apply();
	AgentPlan const unchanged = fixture.Plan();
	for (AgentFilePlan const& file : unchanged.Files)
	{
		CHECK(file.Diff.empty());
	}
}

TEST_CASE("AgentKit: the engine's own kit sets a project up, and its block names only real tools and files")
{
	Result<EngineLayout> layout = EngineLayout::Resolve(FileSystem::GetExecutablePath());
	REQUIRE(layout.IsOk());
	REQUIRE(layout.GetValue().IsSourceTree());
	KitFixture fixture;
	fixture.Layout = layout.TakeValue();
	AgentApplyResult const applied = fixture.Apply();
	CHECK(applied.Plan.Files.size() == AgentKit::ListFiles(fixture.Layout).size());
	for (std::string const& warning : applied.Plan.Warnings)
	{
		// The only warning is about where .mcp.json's tool comes from.
		bool const aboutTheTool = warning.starts_with("strada is not beside the editor") || warning.starts_with("no strada on PATH");
		CHECK_MESSAGE(aboutTheTool, warning);
	}
	CHECK(fixture.Read(SkillPath) == ReadFile(fixture.Layout.GetContentPath(EngineContent::MakeGameSkill) / "SKILL.md"));

	std::string const block = ReadFile(fixture.Layout.GetContentPath(EngineContent::AgentKit) / "AGENTS.block.md");
	CHECK(block.find("{{ProjectName}}") != std::string::npos);
	CHECK(block.find("{{ProjectFile}}") != std::string::npos);
	CHECK(block.find("strada-agents -->") == std::string::npos);
	CHECK(block.find('\r') == std::string::npos);
	for (std::string_view const forbidden : {"dotnet run", "StradaTool/", "build/", "Projects/", "Docs/", "Tools/"})
	{
		CAPTURE(forbidden);
		CHECK(block.find(forbidden) == std::string::npos);
	}
	// Every tool it names is a documented command, under its MCP name.
	std::string const reference = ReadFile(FileSystem::PathFromUtf8(STRADA_AUTOMATION_DOC_PATH));
	std::set<std::string> tools;
	std::regex const command(R"(\| `([a-z]+\.[a-z-]+)` \|)");
	for (auto match = std::sregex_iterator(reference.begin(), reference.end(), command); match != std::sregex_iterator(); ++match)
	{
		std::string name = (*match)[1].str();
		std::ranges::replace(name, '.', '_');
		std::ranges::replace(name, '-', '_');
		tools.insert(name);
	}
	REQUIRE(tools.size() > 50);
	std::regex const named(R"(`([a-z]+(_[a-z]+)+)`)");
	size_t namedTools = 0;
	for (auto match = std::sregex_iterator(block.begin(), block.end(), named); match != std::sregex_iterator(); ++match)
	{
		CAPTURE((*match)[1].str());
		CHECK(tools.contains((*match)[1].str()));
		namedTools++;
	}
	CHECK(namedTools >= 10);
	// The guides it points to are the ones a setup copies.
	std::regex const guide(R"(`?([A-Za-z]+\.md)`)");
	for (auto match = std::sregex_iterator(block.begin(), block.end(), guide); match != std::sregex_iterator(); ++match)
	{
		std::string const name = (*match)[1].str();
		if (name != "AGENTS.md" && name != "SKILL.md")
		{
			CAPTURE(name);
			CHECK(std::ranges::find(AgentKit::Guides, std::string_view(name)) != AgentKit::Guides.end());
		}
	}
}

TEST_CASE("AgentKit: damaged .mcp.json, AGENTS.md and state files never crash the kit, Damaged files are never rewritten, and what it "
          "writes is current")
{
	Testing::TemporaryDirectory directory;
	EditorLocations const locations = Testing::MakeEditorLocations(directory.GetPath());
	Json const mcpSeed = Json::parse(R"({"mcpServers": {"other": {"command": "other", "args": ["a", 1, true], "env": {"A": "1"}},
		"strada": {"command": "strada", "args": ["mcp", "--project", "Fuzz.sproj"]}}, "inputs": [{"id": "x"}], "note": null})");
	Json const stateSeed = Json::parse(fmt::format(R"({{"Strada": {{"Version": 1, "Type": "AgentKitState"}},
		"Files": {{"{}": "{}", ".strada/docs/Agents.md": "0123456789abcdef"}}}})",
	                                               SkillPath, HashOf("Mine.\n")));
	std::array<std::string_view, 10> const agentLines = {"# Mine",
	                                                     "<!-- BEGIN strada-agents -->",
	                                                     "<!-- END strada-agents -->",
	                                                     "  <!-- BEGIN strada-agents -->\r",
	                                                     "",
	                                                     "## Fixture agent block",
	                                                     "The project \"Fuzz\" opens from `Fuzz.sproj`; this line names Fuzz twice.",
	                                                     "text <!-- END strada-agents --> inline",
	                                                     "\r",
	                                                     "Notes\r"};
	Testing::DocumentMutator mutator(29);
	Testing::FuzzRandom random(31);
	int damaged = 0;
	int written = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(100); round < rounds; round++)
	{
		CAPTURE(round);
		AgentProject const project{directory.GetPath() / fmt::format("Project{}", round), "Fuzz", "Fuzz.sproj"};
		REQUIRE(FileSystem::CreateDirectories(project.Directory).IsOk());
		auto const write = [&project](std::string_view relative, std::string_view text)
		{
			WriteFile(project.Directory / FileSystem::PathFromUtf8(relative), text);
		};

		std::string mcp = mutator.Damage(mcpSeed, round).dump(static_cast<int>(random.Pick(4)) - 1, random.Pick(2) == 0 ? ' ' : '\t');
		switch (random.Pick(6))
		{
			case 0:
				mcp.insert(random.Pick(mcp.size() + 1), random.Pick(2) == 0 ? "," : "// note\n");
				break;
			case 1:
				mcp.resize(random.Pick(mcp.size() + 1));
				break;
			default:
				break;
		}
		write(".mcp.json", mcp);
		std::string agents;
		for (size_t line = 0, lines = random.Pick(9); line < lines; line++)
		{
			agents += agentLines[random.Pick(agentLines.size())];
			if (line + 1 < lines || random.Pick(2) == 0)
			{
				agents += random.Pick(3) == 0 ? "\r\n" : "\n";
			}
		}
		write("AGENTS.md", agents);
		write(".strada/agents.json", random.Pick(4) == 0 ? std::string("{\"Strada\": ") : mutator.Damage(stateSeed, round).dump());
		if (random.Pick(2) == 0)
		{
			write(SkillPath, random.Pick(2) == 0
			                     ? "Mine.\n"
			                     : ReadFile(Testing::GetTestContentDirectory() / "Agents" / "skills" / "strada-make-game" / "SKILL.md"));
		}

		std::map<std::string, std::string> const before = Snapshot(project.Directory);
		Result<AgentStatus> const status = AgentKit::GetStatus(project, locations.Layout, AgentToolLookup());
		REQUIRE(status.IsOk());
		AgentApplyResult const applied = AgentKit::Apply(project, locations.Layout, AgentToolLookup());
		REQUIRE_MESSAGE(applied.Succeeded(), applied.Error);
		Result<AgentStatus> const after = AgentKit::GetStatus(project, locations.Layout, AgentToolLookup());
		REQUIRE(after.IsOk());
		REQUIRE(applied.Plan.Files.size() == status.GetValue().Files.size());
		for (size_t index = 0; index < status.GetValue().Files.size(); index++)
		{
			AgentFileStatus const& file = status.GetValue().Files[index];
			CAPTURE(file.Path);
			std::filesystem::path const path = project.Directory / FileSystem::PathFromUtf8(file.Path);
			AgentFileAction const action = applied.Plan.Files[index].Action;
			if (file.State == AgentFileState::Damaged)
			{
				damaged++;
				CHECK(action == AgentFileAction::SkipDamaged);
				CHECK(ReadFile(path) == before.at(file.Path));
				CHECK(after.GetValue().Files[index].State == AgentFileState::Damaged);
			}
			else if (action == AgentFileAction::Create || action == AgentFileAction::Update)
			{
				written++;
				CHECK(after.GetValue().Files[index].State == AgentFileState::Current);
			}
		}
		Result<AgentPlan> const again = AgentKit::Plan(project, locations.Layout, AgentToolLookup());
		REQUIRE(again.IsOk());
		for (AgentFilePlan const& file : again.GetValue().Files)
		{
			CAPTURE(file.Path);
			CHECK((file.Action == AgentFileAction::Unchanged || file.Action == AgentFileAction::SkipDamaged ||
			       file.Action == AgentFileAction::SkipUserModified));
		}
	}
	// The rounds reach both outcomes.
	CHECK(damaged > 0);
	CHECK(written > 0);
}
