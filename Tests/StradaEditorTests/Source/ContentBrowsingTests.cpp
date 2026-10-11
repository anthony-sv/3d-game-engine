#include "Audio/AudioTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/AssetBrowsing.h"
#include "Editor/AssetDrops.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/ProjectImport.h"
#include "Editor/ScriptBrowsing.h"
#include "Editor/UI/EditorUI.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Log.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ScriptProject.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Script/ScriptEngine.h"

#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	ScriptClassInfo MakeClass(std::string name)
	{
		ScriptClassInfo info;
		info.Name = std::move(name);
		return info;
	}

	std::filesystem::path GetScriptCoreAssembly()
	{
		return FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll";
	}

	std::string GetAssetPath(AssetHandle asset)
	{
		std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(asset);
		REQUIRE(metadata.has_value());
		return metadata->Path;
	}

	UUID CreateEntity(EditorOperations& operations, std::string name, Json components = Json::object())
	{
		EntityCreateInfo info;
		info.Name = std::move(name);
		info.Components = std::move(components);
		Result<UUID> entity = operations.CreateEntity(info);
		REQUIRE(entity.IsOk());
		return entity.GetValue();
	}
}

TEST_CASE("UI: durations read as milliseconds, seconds or minutes and seconds")
{
	CHECK(UI::FormatDuration(0.04) == "40 ms");
	CHECK(UI::FormatDuration(0.0004) == "0 ms");
	CHECK(UI::FormatDuration(0.9994) == "999 ms");
	CHECK(UI::FormatDuration(0.9996) == "1.0 s");
	CHECK(UI::FormatDuration(2.44) == "2.4 s");
	CHECK(UI::FormatDuration(9.94) == "9.9 s");
	// Values that would round to "10.0 s" read as minutes and seconds.
	CHECK(UI::FormatDuration(9.96) == "0:10");
	CHECK(UI::FormatDuration(24.4) == "0:24");
	CHECK(UI::FormatDuration(65.0) == "1:05");
	CHECK(UI::FormatDuration(3725.0) == "62:05");
	// Lengths that are not lengths read as none.
	CHECK(UI::FormatDuration(-1.0) == "0 ms");
	CHECK(UI::FormatDuration(std::numeric_limits<double>::quiet_NaN()) == "0 ms");
	CHECK(UI::FormatDuration(std::numeric_limits<double>::infinity()) == "0 ms");
}

TEST_CASE("ScriptBrowsing: the classes a C# source declares are found with their namespaces")
{
	// The template's file-scoped namespace.
	CHECK(ScriptBrowsing::FindDeclaredClasses("using Strada;\n\nnamespace Game;\n\npublic class Player : Script\n{\n}\n") ==
	      std::vector<std::string>{"Game.Player"});

	// Block namespaces nest; classes outside them have none.
	std::string_view const blocks = R"cs(
namespace Game.Enemies
{
	namespace Bosses
	{
		public sealed partial class Dragon : Strada.Script { }
	}
	class Goblin : Script { }
}
class Global { }
)cs";
	CHECK(ScriptBrowsing::FindDeclaredClasses(blocks) ==
	      std::vector<std::string>{"Game.Enemies.Bosses.Dragon", "Game.Enemies.Goblin", "Global"});

	// Comments, literals, preprocessor lines, constraints and verbatim identifiers declare nothing.
	std::string_view const noise = R"cs(
// class Commented
/* class Blocked { } */
#region class Region
namespace Game;
public class Real<T> : Script where T : class, new()
{
	string a = "class InString \" class Escaped";
	string b = @"class ""Verbatim""
class OnNextLine";
	string c = $"{1} class Interpolated";
	string d = """class "Raw" text""";
	char e = '"';
	char f = '\'';
	int @class = 1;
}
)cs";
	CHECK(ScriptBrowsing::FindDeclaredClasses(noise) == std::vector<std::string>{"Game.Real"});

	CHECK(ScriptBrowsing::FindDeclaredClasses("").empty());
	CHECK(ScriptBrowsing::FindDeclaredClasses("class").empty());
	CHECK(ScriptBrowsing::FindDeclaredClasses("namespace").empty());
	CHECK(ScriptBrowsing::FindDeclaredClasses("}}} class Unbalanced {") == std::vector<std::string>{"Unbalanced"});
	// An unterminated string ends with its line.
	CHECK(ScriptBrowsing::FindDeclaredClasses("string s = \"open\nclass After {}") == std::vector<std::string>{"After"});
	// Names in other scripts.
	CHECK(ScriptBrowsing::FindDeclaredClasses("namespace Spiel; class Gr\xC3\xB6\xC3\x9F"
	                                          "e : Script {}") == std::vector<std::string>{"Spiel.Gr\xC3\xB6\xC3\x9F"
	                                                                                       "e"});
}

TEST_CASE("ScriptBrowsing: a C# source maps to the loaded script class it defines")
{
	std::vector<ScriptClassInfo> const classes = {MakeClass("Game.Player"), MakeClass("Game.Enemy"), MakeClass("Tools.Player"),
	                                              MakeClass("Game.Helpers+Nested")};
	// The class named like the file wins over the others the file declares.
	CHECK(ScriptBrowsing::FindScriptClass("namespace Game; class Enemy : Script {} class Player : Script {}", "Player", classes) ==
	      "Game.Player");
	// Else the first declared class that is loaded.
	CHECK(ScriptBrowsing::FindScriptClass("namespace Game; class Helper {} class Enemy : Script {}", "Enemies", classes) == "Game.Enemy");
	// Declared but not loaded (not built yet): nothing, even when a class of another file carries the file's name.
	CHECK_FALSE(ScriptBrowsing::FindScriptClass("namespace Game; class Boss : Script {}", "Player", classes).has_value());
	// Without readable declarations the file's name decides when exactly one loaded class carries it.
	CHECK(ScriptBrowsing::FindScriptClass("// nothing", "Enemy", classes) == "Game.Enemy");
	CHECK(ScriptBrowsing::FindScriptClass("", "Nested", classes) == "Game.Helpers+Nested");
	CHECK_FALSE(ScriptBrowsing::FindScriptClass("", "Player", classes).has_value());
	CHECK_FALSE(ScriptBrowsing::FindScriptClass("", "Missing", classes).has_value());
	CHECK_FALSE(
		ScriptBrowsing::FindScriptClass("namespace Game; class Player {}", "Player", std::span<ScriptClassInfo const>()).has_value());

	// Files are read first; unreadable ones fail, and so do sources whose classes are not loaded.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const file = directory.GetPath() / "Player.cs";
	REQUIRE(FileSystem::WriteTextFile(file, "namespace Game;\npublic class Player : Script {}\n").IsOk());
	Result<std::string> const found = ScriptBrowsing::FindScriptClass(file, classes);
	REQUIRE(found.IsOk());
	CHECK(found.GetValue() == "Game.Player");
	CHECK(ScriptBrowsing::FindScriptClass(directory.GetPath() / "Missing.cs", classes).IsError());
	std::array<ScriptClassInfo, 1> const others = {MakeClass("Other.Thing")};
	Result<std::string> const unbuilt = ScriptBrowsing::FindScriptClass(file, others);
	REQUIRE(unbuilt.IsError());
	CHECK(unbuilt.GetError().find("build the scripts") != std::string::npos);
}

TEST_CASE("ScriptBrowsing: C# sources and their folders are listed and searched")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const sources = directory.GetPath();
	for (char const* file :
	     {"Player.cs", "enemy.CS", "AI/Brain.cs", "AI/Steering/Seek.cs", "AI/Notes.txt", ".hidden/Secret.cs", "Readme.md"})
	{
		REQUIRE(FileSystem::WriteTextFile(sources / FileSystem::PathFromUtf8(file), "").IsOk());
	}
	REQUIRE(FileSystem::CreateDirectories(sources / "Empty").IsOk());

	CHECK(ScriptBrowsing::ListFolders(sources, "") == std::vector<std::string>{"AI", "Empty"});
	CHECK(ScriptBrowsing::ListFolders(sources, "AI") == std::vector<std::string>{"AI/Steering"});
	CHECK(ScriptBrowsing::ListScripts(sources, "") == std::vector<std::string>{"enemy.CS", "Player.cs"});
	CHECK(ScriptBrowsing::ListScripts(sources, "AI") == std::vector<std::string>{"AI/Brain.cs"});
	CHECK(ScriptBrowsing::SearchScripts(sources, "ai") == std::vector<std::string>{"AI/Brain.cs", "AI/Steering/Seek.cs"});
	CHECK(ScriptBrowsing::SearchScripts(sources, "SEEK") == std::vector<std::string>{"AI/Steering/Seek.cs"});
	// Hidden folders are not browsed.
	CHECK(ScriptBrowsing::SearchScripts(sources, "Secret").empty());
	CHECK(ScriptBrowsing::ListScripts(sources / "Missing", "").empty());
	CHECK(ScriptBrowsing::ListFolders(sources / "Missing", "").empty());
	CHECK(ScriptBrowsing::SearchScripts(sources / "Missing", "").empty());

	CHECK(ScriptBrowsing::IsScriptFile("Player.Cs"));
	CHECK_FALSE(ScriptBrowsing::IsScriptFile("Player.cs.txt"));
	CHECK_FALSE(ScriptBrowsing::IsScriptFile("cs"));
}

TEST_CASE("AssetBrowsing: files of the asset directory that are not assets are listed and searched")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	std::filesystem::path const directory = context.GetProject()->GetAssetDirectory();
	for (char const* file : {"Models/Crate.mtl", "Models/scene.bin", "Readme.txt", ".hidden.txt", ".git/config", "Models/.DS_Store"})
	{
		REQUIRE(FileSystem::WriteTextFile(directory / FileSystem::PathFromUtf8(file), "x").IsOk());
	}
	REQUIRE(FileSystem::WriteTextFile(directory / "Models" / "Crate.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n").IsOk());
	REQUIRE(operations.RefreshAssets().IsOk());
	REQUIRE(AssetManager::FindByPath("Models/Crate.obj").IsValid());

	// Assets, hidden files and the asset registry are not listed.
	CHECK(AssetBrowsing::ListOtherFiles("") == std::vector<std::string>{"Readme.txt"});
	CHECK(AssetBrowsing::ListOtherFiles("Models") == std::vector<std::string>{"Models/Crate.mtl", "Models/scene.bin"});
	CHECK(AssetBrowsing::ListOtherFiles("Missing").empty());
	CHECK(AssetBrowsing::SearchOtherFiles("CRATE") == std::vector<std::string>{"Models/Crate.mtl"});
	CHECK(AssetBrowsing::SearchOtherFiles("e") == std::vector<std::string>{"Models/Crate.mtl", "Models/scene.bin", "Readme.txt"});
	CHECK(AssetBrowsing::SearchOtherFiles("config").empty());

	REQUIRE(operations.CloseProject().IsOk());
	CHECK(AssetBrowsing::ListOtherFiles("").empty());
	CHECK(AssetBrowsing::SearchOtherFiles("").empty());
}

TEST_CASE("EditorScripts: C# sources are copied into a folder of the scripts, creating the C# project")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, GetScriptCoreAssembly());
	std::filesystem::path const outside = temporary.GetPath() / "Downloads";
	REQUIRE(FileSystem::WriteTextFile(outside / "Mover.cs", "public class Mover : Strada.Script {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Notes.txt", "").IsOk());
	std::array<std::filesystem::path, 1> const mover = {outside / "Mover.cs"};

	// Without a project there are no scripts.
	CHECK(scripts.AddScripts(mover, "").IsError());

	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	Project const& project = *context.GetProject();
	std::filesystem::path const sources = ScriptProject::GetSourceDirectory(project);
	CHECK_FALSE(ScriptProject::Exists(project));
	Result<std::vector<std::filesystem::path>> const added = scripts.AddScripts(mover, "AI/Steering");
	REQUIRE(added.IsOk());
	CHECK(added.GetValue() == std::vector<std::filesystem::path>{sources / "AI" / "Steering" / "Mover.cs"});
	CHECK(ScriptProject::Exists(project));
	CHECK(FileSystem::ReadTextFile(added.GetValue().front()).GetValue() == "public class Mover : Strada.Script {}");

	// Taken names get a number; sources already in the scripts stay where they are.
	std::array<std::filesystem::path, 2> const again = {outside / "Mover.cs", sources / "AI" / "Steering" / "Mover.cs"};
	Result<std::vector<std::filesystem::path>> const numbered = scripts.AddScripts(again, "AI/Steering");
	REQUIRE(numbered.IsOk());
	CHECK(numbered.GetValue() ==
	      std::vector<std::filesystem::path>{sources / "AI" / "Steering" / "Mover 1.cs", sources / "AI" / "Steering" / "Mover.cs"});

	// Other files, files that do not exist and folders outside the scripts are refused without copying anything.
	std::array<std::filesystem::path, 2> const mixed = {outside / "Mover.cs", outside / "Notes.txt"};
	CHECK(scripts.AddScripts(mixed, "").IsError());
	std::array<std::filesystem::path, 1> const missing = {outside / "Missing.cs"};
	CHECK(scripts.AddScripts(missing, "").IsError());
	for (char const* folder : {"..", "../..", "AI/../../Assets"})
	{
		CAPTURE(folder);
		CHECK(scripts.AddScripts(mover, folder).IsError());
		CHECK(scripts.CreateScript("Outside", folder).IsError());
	}
	CHECK_FALSE(FileSystem::Exists(sources / "Mover.cs"));
	CHECK_FALSE(FileSystem::Exists(project.GetDirectory() / "Mover.cs"));
	CHECK_FALSE(FileSystem::Exists(project.GetDirectory() / "Outside.cs"));

	// New scripts go into the folder the content browser shows.
	Result<std::filesystem::path> const brain = scripts.CreateScript("Brain", "AI");
	REQUIRE(brain.IsOk());
	CHECK(brain.GetValue() == sources / "AI" / "Brain.cs");
}

TEST_CASE("ProjectImport: dropped files and folders go into the project and the rest is reported")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, GetScriptCoreAssembly());
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	REQUIRE(operations.CreateAssetFolder("Textures").IsOk());
	std::filesystem::path const assetDirectory = context.GetProject()->GetAssetDirectory();
	std::filesystem::path const sources = ScriptProject::GetSourceDirectory(*context.GetProject());

	std::filesystem::path const outside = temporary.GetPath() / "Downloads";
	REQUIRE(FileSystem::CreateDirectories(outside / "Crate" / "Textures").IsOk());
	REQUIRE(Image(2, 2, 4).WritePNG(outside / "Oak.png").IsOk());
	std::vector<float> const samples = Testing::MakeSamples({{0.5f, 0.1f}});
	REQUIRE(FileSystem::WriteBinaryFile(outside / "Hit.wav", Testing::MakeWav(samples).GetSpan()).IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Notes.docx", "notes").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Mover.cs", "namespace Game; public class Mover : Strada.Script {}").IsOk());
	// A downloaded model: a mesh with the material library it needs, a texture in a subfolder and a hidden file.
	REQUIRE(FileSystem::WriteTextFile(outside / "Crate" / "Crate.obj", "mtllib Crate.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Crate" / "Crate.mtl", "newmtl Wood\n").IsOk());
	REQUIRE(Image(2, 2, 4).WritePNG(outside / "Crate" / "Textures" / "Wood.png").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Crate" / ".DS_Store", "x").IsOk());

	uint64_t const firstLog = Log::GetNextEntryIndex();
	std::vector<std::filesystem::path> const dropped = {outside / "Oak.png", outside / "Hit.wav", outside / "Notes.docx",
	                                                    outside / "Mover.cs", outside / "Crate"};
	ProjectImport::Report const report = ProjectImport::Import(operations, &scripts, dropped, {"Textures", "Gameplay"});
	ProjectImport::Log(report);
	CHECK(report.Errors.empty());
	CHECK(report.Skipped == std::vector<std::string>{"Notes.docx"});
	REQUIRE(report.Assets.size() == 4);
	CHECK(GetAssetPath(report.Assets[0]) == "Textures/Oak.png");
	CHECK(GetAssetPath(report.Assets[1]) == "Textures/Hit.wav");
	CHECK(GetAssetPath(report.Assets[2]) == "Textures/Crate/Crate.obj");
	CHECK(GetAssetPath(report.Assets[3]) == "Textures/Crate/Textures/Wood.png");
	CHECK(report.Folders == std::vector<std::string>{"Textures/Crate"});
	CHECK(report.KeptFiles == std::vector<std::string>{"Textures/Crate/Crate.mtl"});
	CHECK_FALSE(FileSystem::Exists(assetDirectory / "Textures" / "Crate" / ".DS_Store"));
	CHECK(report.Scripts == std::vector<std::filesystem::path>{sources / "Gameplay" / "Mover.cs"});
	CHECK(ScriptProject::Exists(*context.GetProject()));
	CHECK(Testing::WasLogged(firstLog, "Notes.docx"));
	CHECK(Testing::WasLogged(firstLog, "Crate.mtl"));
	CHECK(Testing::WasLogged(firstLog, "Imported 4 assets, 1 C# script and 1 folder"));

	// Taken names get a number.
	std::vector<std::filesystem::path> const again = {outside / "Oak.png", outside / "Mover.cs", outside / "Crate"};
	ProjectImport::Report const second = ProjectImport::Import(operations, &scripts, again, {"Textures", "Gameplay"});
	CHECK(second.Errors.empty());
	REQUIRE(second.Assets.size() == 3);
	CHECK(GetAssetPath(second.Assets[0]) == "Textures/Oak 1.png");
	CHECK(second.Folders == std::vector<std::string>{"Textures/Crate 1"});
	CHECK(second.Scripts == std::vector<std::filesystem::path>{sources / "Gameplay" / "Mover 1.cs"});

	// What is already in the project is registered where it is.
	REQUIRE(Image(2, 2, 4).WritePNG(assetDirectory / "Loose" / "Leaf.png").IsOk());
	std::vector<std::filesystem::path> const inside = {assetDirectory / "Loose"};
	ProjectImport::Report const registered = ProjectImport::Import(operations, &scripts, inside, {"Textures", ""});
	CHECK(registered.Errors.empty());
	REQUIRE(registered.Assets.size() == 1);
	CHECK(GetAssetPath(registered.Assets[0]) == "Loose/Leaf.png");
	CHECK(registered.Folders.empty());

	// Refused: a folder holding the project's assets, a drive, a file that is gone, C# sources without the scripts.
	std::vector<std::filesystem::path> const refusedPaths = {temporary.GetPath(), temporary.GetPath().root_path(), outside / "Gone.png",
	                                                         outside / "Mover.cs"};
	ProjectImport::Report const refused = ProjectImport::Import(operations, nullptr, refusedPaths, {});
	CHECK(refused.Errors.size() == 4);
	CHECK(refused.Assets.empty());
	CHECK(refused.Scripts.empty());
	CHECK(refused.Folders.empty());

	// Without a project nothing is imported.
	REQUIRE(operations.CloseProject().IsOk());
	CHECK(ProjectImport::Import(operations, &scripts, dropped, {}).Errors == std::vector<std::string>{"no project is open"});
}

TEST_CASE("AssetDrops: a C# source dropped onto entities makes them run its class, as undoable steps")
{
	Testing::ScriptEngineScope scripting;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	// Sources of classes of the test scripts, and of a class that is not built.
	std::filesystem::path const mover = temporary.GetPath() / "Mover.cs";
	REQUIRE(FileSystem::WriteTextFile(mover, "using Strada;\nnamespace Strada.Tests;\npublic class Mover : Script {}\n").IsOk());
	std::filesystem::path const unbuilt = temporary.GetPath() / "Unbuilt.cs";
	REQUIRE(FileSystem::WriteTextFile(unbuilt, "namespace Game;\npublic class Unbuilt : Strada.Script {}\n").IsOk());

	UUID const plain = CreateEntity(operations, "Plain");
	UUID const scripted = CreateEntity(
		operations, "Scripted",
		Json::object({{"Script", Json::object({{"ClassName", "Strada.Tests.FieldTypes"},
	                                           {"Fields", Json::object({{"Count", Json::object({{"Type", "Int32"}, {"Value", 3}})}})}})}}));
	size_t const steps = context.GetHistory().GetUndoCount();

	// One step adds the component, one more replaces the class (stored field values stay).
	std::array<UUID, 2> const both = {plain, scripted};
	Result<std::string> const added = AssetDrops::AddScript(operations, both, mover);
	REQUIRE_MESSAGE(added.IsOk(), (added ? std::string() : added.GetError()));
	CHECK(added.GetValue() == "Strada.Tests.Mover");
	Scene& scene = context.GetScene();
	CHECK(scene.GetEntityByUUID(plain).GetComponent<ScriptComponent>().ClassName == "Strada.Tests.Mover");
	CHECK(scene.GetEntityByUUID(scripted).GetComponent<ScriptComponent>().ClassName == "Strada.Tests.Mover");
	CHECK(scene.GetEntityByUUID(scripted).GetComponent<ScriptComponent>().Fields.size() == 1);
	CHECK(context.GetHistory().GetUndoCount() == steps + 2);

	// Entities that run the class already are left alone.
	CHECK(AssetDrops::AddScript(operations, both, mover).IsOk());
	CHECK(context.GetHistory().GetUndoCount() == steps + 2);

	REQUIRE(operations.Undo().IsOk());
	CHECK(scene.GetEntityByUUID(scripted).GetComponent<ScriptComponent>().ClassName == "Strada.Tests.FieldTypes");
	REQUIRE(operations.Undo().IsOk());
	CHECK_FALSE(scene.GetEntityByUUID(plain).HasComponent<ScriptComponent>());

	// Failures change nothing: a class that is not built, a missing file, unknown or no entities.
	std::array<UUID, 1> const one = {plain};
	Result<std::string> const notBuilt = AssetDrops::AddScript(operations, one, unbuilt);
	REQUIRE(notBuilt.IsError());
	CHECK(notBuilt.GetError().find("build the scripts") != std::string::npos);
	CHECK(AssetDrops::AddScript(operations, one, temporary.GetPath() / "Missing.cs").IsError());
	std::array<UUID, 2> const unknown = {plain, UUID(12345)};
	CHECK(AssetDrops::AddScript(operations, unknown, mover).IsError());
	CHECK(AssetDrops::AddScript(operations, std::span<UUID const>(), mover).IsError());
	CHECK_FALSE(scene.GetEntityByUUID(plain).HasComponent<ScriptComponent>());
	CHECK(context.GetHistory().GetUndoCount() == steps);

	// Without the game's scripts the error says they need building.
	ScriptEngine::UnloadGameAssembly();
	Result<std::string> const noAssembly = AssetDrops::AddScript(operations, one, mover);
	REQUIRE(noAssembly.IsError());
	CHECK(noAssembly.GetError().find("not built yet") != std::string::npos);
}
