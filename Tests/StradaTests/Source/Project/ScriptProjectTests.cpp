#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ScriptProject.h"
#include "Strada/Script/ScriptEngine.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>

using namespace Strada;

namespace
{
	// A project in memory: the script project is all the tests write.
	Project MakeProject(std::filesystem::path const& directory)
	{
		ProjectSettings settings;
		settings.Name = "Script Test";
		return Project(directory / "ScriptTest.sproj", settings);
	}

	std::filesystem::path GetScriptCoreAssembly()
	{
		return FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll";
	}

	std::string ReadFile(std::filesystem::path const& path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE_MESSAGE(text.IsOk(), FileSystem::PathToUtf8(path));
		return text.GetValue();
	}

	bool Contains(std::string const& text, std::string const& part)
	{
		return text.find(part) != std::string::npos;
	}
}

TEST_CASE("ScriptProject: generates the C# project once and keeps Strada.props current")
{
	Testing::TemporaryDirectory directory;
	Project const project = MakeProject(directory.GetPath());
	std::filesystem::path const scripts = directory.GetPath() / "Scripts";
	CHECK_FALSE(ScriptProject::Exists(project));
	CHECK(ScriptProject::GetProjectFile(project) == scripts / "Game.csproj");
	CHECK(ScriptProject::GetAssemblyPath(project) == scripts / "Binaries" / "Game.dll");
	CHECK(ScriptProject::UpdateProps(project, GetScriptCoreAssembly()).IsError());

	REQUIRE(ScriptProject::Generate(project, GetScriptCoreAssembly()).IsOk());
	CHECK(ScriptProject::Exists(project));
	std::string const projectFile = ReadFile(scripts / "Game.csproj");
	CHECK(Contains(projectFile, "<RootNamespace>Game</RootNamespace>"));
	CHECK(Contains(projectFile, "<Import Project=\"Strada.props\""));
	CHECK(Contains(ReadFile(scripts / "Strada.props"), FileSystem::PathToNativeUtf8(GetScriptCoreAssembly())));
	CHECK(Contains(ReadFile(scripts / ".gitignore"), "Strada.props"));
	CHECK(std::filesystem::is_directory(scripts / "Source"));

	// The C# project belongs to the user once written; Strada.props follows the engine.
	REQUIRE(FileSystem::WriteTextFile(scripts / "Game.csproj", "<Project>edited</Project>").IsOk());
	std::filesystem::path const moved = directory.GetPath() / "Moved & Renamed" / "Strada.ScriptCore.dll";
	REQUIRE(ScriptProject::Generate(project, moved).IsOk());
	CHECK(ReadFile(scripts / "Game.csproj") == "<Project>edited</Project>");
	CHECK(Contains(ReadFile(scripts / "Strada.props"), "Moved &amp; Renamed"));
	REQUIRE(ScriptProject::UpdateProps(project, GetScriptCoreAssembly()).IsOk());
	CHECK(Contains(ReadFile(scripts / "Strada.props"), FileSystem::PathToNativeUtf8(GetScriptCoreAssembly())));
}

TEST_CASE("ScriptProject: script classes come from a template and need valid names")
{
	CHECK(ScriptProject::IsValidClassName("Player"));
	CHECK(ScriptProject::IsValidClassName("_Enemy2"));
	CHECK_FALSE(ScriptProject::IsValidClassName(""));
	CHECK_FALSE(ScriptProject::IsValidClassName("2Fast"));
	CHECK_FALSE(ScriptProject::IsValidClassName("Bad Name"));
	CHECK_FALSE(ScriptProject::IsValidClassName("class"));
	CHECK_FALSE(ScriptProject::IsValidClassName("Script"));
	CHECK_FALSE(ScriptProject::IsValidClassName("Caf\xC3\xA9"));

	Testing::TemporaryDirectory directory;
	Project const project = MakeProject(directory.GetPath());
	Result<std::filesystem::path> const script = ScriptProject::CreateScript(project, "Player");
	REQUIRE(script.IsOk());
	CHECK(script.GetValue() == directory.GetPath() / "Scripts" / "Source" / "Player.cs");
	CHECK(ScriptProject::GetSourceDirectory(project) == directory.GetPath() / "Scripts" / "Source");
	std::string const source = ReadFile(script.GetValue());
	CHECK(Contains(source, "namespace Game;"));
	CHECK(Contains(source, "public class Player : Script"));
	CHECK(Contains(source, "protected override void OnUpdate(float deltaTime)"));

	Result<std::filesystem::path> const again = ScriptProject::CreateScript(project, "Player");
	REQUIRE(again.IsError());
	CHECK(Contains(again.GetError(), "already exists"));
	CHECK(ScriptProject::CreateScript(project, "class").IsError());

	// In folders of the sources, created when missing.
	std::filesystem::path const sources = ScriptProject::GetSourceDirectory(project);
	Result<std::filesystem::path> const brain = ScriptProject::CreateScript(project, "Brain", "AI/Steering/");
	REQUIRE(brain.IsOk());
	CHECK(brain.GetValue() == sources / "AI" / "Steering" / "Brain.cs");
	CHECK(Contains(ReadFile(brain.GetValue()), "public class Brain : Script"));
	CHECK(ScriptProject::GetSourceFolder(project, "").GetValue() == sources);
	CHECK(ScriptProject::GetSourceFolder(project, "AI/./Steering").GetValue() == sources / "AI" / "Steering");
	CHECK(ScriptProject::GetSourceFolder(project, "AI/..").GetValue() == sources);
	// A source of the same name anywhere in the C# project would declare the class twice; build output does not count.
	Result<std::filesystem::path> const twice = ScriptProject::CreateScript(project, "Player", "AI");
	REQUIRE(twice.IsError());
	CHECK(Contains(twice.GetError(), "Player.cs"));
	REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Scripts" / "Tools" / "Tool.cs", "").IsOk());
	CHECK(ScriptProject::CreateScript(project, "Tool").IsError());
	REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Scripts" / "obj" / "Debug" / "Enemy.cs", "").IsOk());
	CHECK(ScriptProject::CreateScript(project, "Enemy").IsOk());
	// Folders outside the sources are refused, and nothing is written there.
	for (char const* folder : {"..", "../..", "AI/../../Other", "/Absolute"})
	{
		CAPTURE(folder);
		CHECK(ScriptProject::GetSourceFolder(project, folder).IsError());
		CHECK(ScriptProject::CreateScript(project, "Outside", folder).IsError());
	}
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "Scripts" / "Outside.cs"));
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "Outside.cs"));
}

TEST_CASE("ScriptProject: the C# sources the project compiles are listed and found by name")
{
	Testing::TemporaryDirectory directory;
	Project const project = MakeProject(directory.GetPath());
	std::filesystem::path const scripts = directory.GetPath() / "Scripts";
	// Nothing yet: the C# project's directory does not exist.
	CHECK(ScriptProject::ListSources(project).empty());
	CHECK_FALSE(ScriptProject::FindSource(project, "Player.cs").has_value());

	for (char const* file : {"Source/Player.cs", "Source/AI/Brain.CS", "Tools/Tool.cs", "Source/Notes.txt", "obj/Debug/Generated.cs",
	                         "bin/Old.cs", "Binaries/Built.cs", ".vs/Cache.cs", "Source/.hidden/Secret.cs"})
	{
		REQUIRE(FileSystem::WriteTextFile(scripts / FileSystem::PathFromUtf8(file), "").IsOk());
	}
	// Build output and hidden folders are not part of the project.
	CHECK(ScriptProject::ListSources(project) ==
	      std::vector<std::filesystem::path>{(scripts / "Source" / "AI" / "Brain.CS").lexically_normal(),
	                                         (scripts / "Source" / "Player.cs").lexically_normal(),
	                                         (scripts / "Tools" / "Tool.cs").lexically_normal()});
	CHECK(ScriptProject::FindSource(project, "Tool.cs") == (scripts / "Tools" / "Tool.cs").lexically_normal());
	CHECK(ScriptProject::FindSource(project, "Notes.txt") == (scripts / "Source" / "Notes.txt").lexically_normal());
	for (char const* name : {"Generated.cs", "Old.cs", "Built.cs", "Cache.cs", "Secret.cs", "Missing.cs", "player.cs"})
	{
		CAPTURE(name);
		CHECK_FALSE(ScriptProject::FindSource(project, name).has_value());
	}
}

TEST_CASE("ScriptProject: diagnostics are read from MSBuild output")
{
	std::string const output =
		"C:\\Game\\Scripts\\Source\\Player.cs(12,5): error CS1002: ; expected [C:\\Game\\Scripts\\Game.csproj]\n"
		"C:\\Game\\Scripts\\Source\\Player.cs(3,13): warning CS0168: The variable 'x' is declared but never used "
		"[C:\\Game\\Scripts\\Game.csproj]\r\n"
		"MSBUILD : error MSB1009: Project file does not exist.\n"
		"C:\\Game\\Scripts\\Game.csproj : error NU1101: Unable to find package Foo. [C:\\Game\\Scripts\\Game.csproj]\n"
		"/home/user/Game/Scripts/Source/Enemy.cs(7,1,7,4): error CS0246: The type or namespace name 'Foo' could not be found (are "
		"you missing a using directive or an assembly reference?) [/home/user/Game/Scripts/Game.csproj]\n"
		"C:\\Game\\Scripts\\Source\\Player.cs(12,5): error CS1002: ; expected [C:\\Game\\Scripts\\Game.csproj]\n"
		"Build FAILED.\n"
		"    1 Warning(s)\n";
	std::vector<ScriptDiagnostic> const diagnostics = ScriptProject::ParseDiagnostics(output);
	REQUIRE(diagnostics.size() == 5);

	CHECK(diagnostics[0].Severity == ScriptDiagnosticSeverity::Error);
	CHECK(diagnostics[0].File == FileSystem::PathFromUtf8("C:\\Game\\Scripts\\Source\\Player.cs"));
	CHECK(diagnostics[0].Line == 12);
	CHECK(diagnostics[0].Column == 5);
	CHECK(diagnostics[0].Code == "CS1002");
	CHECK(diagnostics[0].Message == "; expected");

	CHECK(diagnostics[1].Severity == ScriptDiagnosticSeverity::Warning);
	CHECK(diagnostics[1].Code == "CS0168");
	CHECK(diagnostics[1].Line == 3);
	CHECK(diagnostics[1].Message == "The variable 'x' is declared but never used");

	CHECK(diagnostics[2].File.empty());
	CHECK(diagnostics[2].Line == 0);
	CHECK(diagnostics[2].Code == "MSB1009");
	CHECK(diagnostics[2].Message == "Project file does not exist.");

	CHECK(diagnostics[3].File == FileSystem::PathFromUtf8("C:\\Game\\Scripts\\Game.csproj"));
	CHECK(diagnostics[3].Line == 0);
	CHECK(diagnostics[3].Code == "NU1101");

	CHECK(diagnostics[4].File == FileSystem::PathFromUtf8("/home/user/Game/Scripts/Source/Enemy.cs"));
	CHECK(diagnostics[4].Line == 7);
	CHECK(diagnostics[4].Column == 1);
	CHECK(Contains(diagnostics[4].Message, "(are you missing a using directive"));
}

TEST_CASE("ScriptProject: diagnostics with invalid UTF-8 get replacement characters")
{
	// A message in a legacy code page (Latin-1 "Caf\xE9").
	std::vector<ScriptDiagnostic> const diagnostics =
		ScriptProject::ParseDiagnostics("C:\\Game\\Scripts\\Source\\Caf\xE9.cs(1,1): error CS1001: Caf\xE9 [C:\\Game\\Game.csproj]\n");
	REQUIRE(diagnostics.size() == 1);
	CHECK(diagnostics[0].Message == "Caf\xEF\xBF\xBD");
	CHECK(diagnostics[0].File == FileSystem::PathFromUtf8("C:\\Game\\Scripts\\Source\\Caf\xEF\xBF\xBD.cs"));
}

TEST_CASE("ScriptProject: builds the game's scripts with the .NET SDK and reports compile errors")
{
	REQUIRE(ScriptProject::FindDotNet().has_value());
	Testing::TemporaryDirectory directory;
	Project const project = MakeProject(directory.GetPath());
	REQUIRE(ScriptProject::Generate(project, GetScriptCoreAssembly()).IsOk());
	Result<std::filesystem::path> const player = ScriptProject::CreateScript(project, "Player");
	REQUIRE(player.IsOk());
	CHECK(ScriptProject::IsOutOfDate(project));

	ScriptBuildSettings const settings = ScriptProject::MakeBuildSettings(project, ScriptBuildConfiguration::Debug);
	Result<ScriptBuildResult> const built = ScriptProject::Build(settings);
	REQUIRE_MESSAGE(built.IsOk(), (built ? std::string() : built.GetError()));
	CAPTURE(built.GetValue().Output);
	REQUIRE(built.GetValue().Succeeded);
	CHECK(built.GetValue().Assembly == ScriptProject::GetAssemblyPath(project));
	CHECK_FALSE(ScriptProject::IsOutOfDate(project));

	// The engine runs what was built.
	REQUIRE(ScriptEngine::Init().IsOk());
	CHECK(ScriptEngine::LoadGameAssembly(built.GetValue().Assembly).IsOk());
	CHECK(ScriptEngine::FindClass("Game.Player") != nullptr);
	ScriptEngine::Shutdown();

	// A compile error is reported where it is; the stale assembly is not reported as built.
	REQUIRE(FileSystem::WriteTextFile(player.GetValue(),
	                                  "using Strada;\n\nnamespace Game;\n\npublic class Player : Script\n{\n\tint m_Broken = ;\n}\n")
	            .IsOk());
	std::filesystem::last_write_time(player.GetValue(),
	                                 std::filesystem::last_write_time(built.GetValue().Assembly) + std::chrono::hours(1));
	CHECK(ScriptProject::IsOutOfDate(project));
	Result<ScriptBuildResult> const broken = ScriptProject::Build(settings);
	REQUIRE(broken.IsOk());
	CAPTURE(broken.GetValue().Output);
	CHECK_FALSE(broken.GetValue().Succeeded);
	CHECK(broken.GetValue().Assembly.empty());
	std::vector<ScriptDiagnostic> const& diagnostics = broken.GetValue().Diagnostics;
	CHECK(std::any_of(diagnostics.begin(), diagnostics.end(),
	                  [&player](ScriptDiagnostic const& diagnostic)
	                  {
						  std::error_code error;
						  return diagnostic.Severity == ScriptDiagnosticSeverity::Error && diagnostic.Line == 7 &&
		                         std::filesystem::equivalent(diagnostic.File, player.GetValue(), error);
					  }));

	// A cancelled build stops at once.
	std::atomic<bool> const cancel = true;
	Result<ScriptBuildResult> const cancelled = ScriptProject::Build(settings, &cancel);
	REQUIRE(cancelled.IsError());
	CHECK(Contains(cancelled.GetError(), "cancelled"));
}
