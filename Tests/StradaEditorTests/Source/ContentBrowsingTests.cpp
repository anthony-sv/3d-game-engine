#include "Asset/GltfTestBuilder.h"
#include "Audio/AudioTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/AssetBrowsing.h"
#include "Editor/AssetDrops.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/FileCopies.h"
#include "Editor/ProjectImport.h"
#include "Editor/ScriptBrowsing.h"
#include "Editor/UI/EditorUI.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
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
	CHECK(ScriptBrowsing::FindDeclaredClasses(noise) == std::vector<std::string>{"Game.Real`1"});

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

TEST_CASE("ScriptBrowsing: the types a C# source declares are found with their kinds, nesting and modifiers")
{
	using ScriptBrowsing::DeclaredType;
	using ScriptBrowsing::DeclaredTypeKind;
	std::string_view const source = R"cs(
using Strada;
namespace Game;

[System.Serializable]
public sealed partial class Player : Script
{
	public enum State { Idle, Running }
	private record Snapshot(int Health);
	struct Cell<T> where T : struct { }
	public delegate void Hit(Entity other);
	void Move<T>() where T : class { var record = 1; foreach (var item in Items) { } }
	string file = "x";
	Action callback = delegate { };
}
file class Helper { }
public interface IDamageable { }
public record struct Point(int X, int Y);
readonly record struct Size(int Width, int Height) { }
public delegate System.Collections.Generic.List<int> Make<TKey, TValue>(TKey key);
enum Direction : byte { Up, Down }
public class Pool<T, U> where T : class where U : struct { class Node { } }
)cs";
	auto const type = [](std::string name, DeclaredTypeKind kind, bool partial = false, bool fileLocal = false, bool nested = false)
	{
		return DeclaredType{std::move(name), kind, partial, fileLocal, nested};
	};
	std::vector<DeclaredType> const expected = {
		type("Game.Player", DeclaredTypeKind::Class, true),
		type("Game.Player+State", DeclaredTypeKind::Enum, false, false, true),
		type("Game.Player+Snapshot", DeclaredTypeKind::RecordClass, false, false, true),
		type("Game.Player+Cell`1", DeclaredTypeKind::Struct, false, false, true),
		type("Game.Player+Hit", DeclaredTypeKind::Delegate, false, false, true),
		type("Game.Helper", DeclaredTypeKind::Class, false, true),
		type("Game.IDamageable", DeclaredTypeKind::Interface),
		type("Game.Point", DeclaredTypeKind::RecordStruct),
		type("Game.Size", DeclaredTypeKind::RecordStruct),
		type("Game.Make`2", DeclaredTypeKind::Delegate),
		type("Game.Direction", DeclaredTypeKind::Enum),
		type("Game.Pool`2", DeclaredTypeKind::Class),
		type("Game.Pool`2+Node", DeclaredTypeKind::Class, false, false, true),
	};
	std::vector<DeclaredType> const types = ScriptBrowsing::FindDeclaredTypes(source);
	REQUIRE(types.size() == expected.size());
	for (size_t index = 0; index < types.size(); index++)
	{
		CAPTURE(expected[index].Name);
		CHECK(types[index].Name == expected[index].Name);
		CHECK(types[index] == expected[index]);
	}
	// Nested classes are named as .NET names them, so scripts nested in other classes are found too.
	CHECK(ScriptBrowsing::FindDeclaredClasses(source) ==
	      std::vector<std::string>{"Game.Player", "Game.Helper", "Game.Pool`2", "Game.Pool`2+Node"});

	// One type declared twice keeps an assembly from building, unless both halves are partial declarations of one kind or one
	// of them is visible only in its file; nested types are left to the compiler.
	DeclaredType const player = type("Game.Player", DeclaredTypeKind::Class);
	CHECK(ScriptBrowsing::IsDuplicateDeclaration(player, player));
	CHECK(ScriptBrowsing::IsDuplicateDeclaration(player, type("Game.Player", DeclaredTypeKind::Enum)));
	CHECK(ScriptBrowsing::IsDuplicateDeclaration(type("Game.Player", DeclaredTypeKind::Class, true), player));
	CHECK(ScriptBrowsing::IsDuplicateDeclaration(type("Game.Player", DeclaredTypeKind::Class, true),
	                                             type("Game.Player", DeclaredTypeKind::Struct, true)));
	CHECK_FALSE(ScriptBrowsing::IsDuplicateDeclaration(type("Game.Player", DeclaredTypeKind::Class, true),
	                                                   type("Game.Player", DeclaredTypeKind::Class, true)));
	CHECK_FALSE(ScriptBrowsing::IsDuplicateDeclaration(type("Game.Player", DeclaredTypeKind::Class, false, true), player));
	CHECK_FALSE(ScriptBrowsing::IsDuplicateDeclaration(type("Game.A+Player", DeclaredTypeKind::Class, false, false, true),
	                                                   type("Game.A+Player", DeclaredTypeKind::Class, false, false, true)));
	CHECK_FALSE(ScriptBrowsing::IsDuplicateDeclaration(player, type("Game.Player`1", DeclaredTypeKind::Class)));
	CHECK_FALSE(ScriptBrowsing::IsDuplicateDeclaration(player, type("Tools.Player", DeclaredTypeKind::Class)));

	CHECK(ScriptBrowsing::GetTypeDisplayName("Game.Pool`1+Node") == "Game.Pool<>.Node");
	CHECK(ScriptBrowsing::GetTypeDisplayName("Game.Map`2") == "Game.Map<,>");
	CHECK(ScriptBrowsing::GetTypeDisplayName("Global") == "Global");

	// Damaged sources declare what can be read; nothing breaks.
	for (std::string_view const damaged : {"class Open<T, {", "delegate*<int, void> pointer;", "namespace ;;; record", "struct",
	                                       "enum : byte {", "class A<<<<", "}}}{{{ partial file class", "delegate void (int x);"})
	{
		CAPTURE(damaged);
		CHECK_NOTHROW(ScriptBrowsing::FindDeclaredTypes(damaged));
	}
	CHECK(ScriptBrowsing::FindDeclaredTypes("class Open<T, {").size() == 1);
	CHECK(ScriptBrowsing::FindDeclaredTypes("delegate*<int, void> pointer;").empty());
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

TEST_CASE("EditorScripts: C# sources are copied into a folder of the scripts unless a type would be declared twice")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, GetScriptCoreAssembly());
	std::filesystem::path const outside = temporary.GetPath() / "Downloads";
	REQUIRE(FileSystem::WriteTextFile(outside / "Mover.cs", "namespace Game; public class Mover : Strada.Script {}").IsOk());
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
	CHECK(FileSystem::ReadTextFile(added.GetValue().front()).GetValue() == "namespace Game; public class Mover : Strada.Script {}");

	// The same script again, into any folder, would declare its class twice: refused, saying where the other one is.
	for (char const* folder : {"AI/Steering", "", "Other"})
	{
		CAPTURE(folder);
		Result<std::vector<std::filesystem::path>> const again = scripts.AddScripts(mover, folder);
		REQUIRE(again.IsError());
		CHECK(again.GetError().find("'Mover.cs' already exists in Scripts/Source/AI/Steering") != std::string::npos);
	}
	// Under another name it still declares Game.Mover.
	REQUIRE(FileSystem::Copy(outside / "Mover.cs", outside / "Mover (2).cs", false).IsOk());
	std::array<std::filesystem::path, 1> const copy = {outside / "Mover (2).cs"};
	Result<std::vector<std::filesystem::path>> const renamed = scripts.AddScripts(copy, "");
	REQUIRE(renamed.IsError());
	CHECK(renamed.GetError().find("Game.Mover") != std::string::npos);
	CHECK(renamed.GetError().find("Scripts/Source/AI/Steering/Mover.cs") != std::string::npos);
	// Two imports of one script never leave two declarations.
	size_t declarations = 0;
	for (std::filesystem::path const& source : ScriptProject::ListSources(project))
	{
		for (ScriptBrowsing::DeclaredType const& type : ScriptBrowsing::FindDeclaredTypes(FileSystem::ReadTextFile(source).GetValue()))
		{
			declarations += type.Name == "Game.Mover" ? 1 : 0;
		}
	}
	CHECK(declarations == 1);

	// Sources that clash with each other are refused together; partial halves of one class come in.
	REQUIRE(FileSystem::WriteTextFile(outside / "Enemy.cs", "namespace Game; class Enemy {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Boss.cs", "namespace Game { class Enemy { } }").IsOk());
	std::array<std::filesystem::path, 2> const clashing = {outside / "Enemy.cs", outside / "Boss.cs"};
	Result<std::vector<std::filesystem::path>> const clash = scripts.AddScripts(clashing, "");
	REQUIRE(clash.IsError());
	CHECK(clash.GetError().find("Enemy.cs (added with it)") != std::string::npos);
	CHECK_FALSE(FileSystem::Exists(sources / "Enemy.cs"));
	REQUIRE(FileSystem::WriteTextFile(outside / "PlayerMove.cs", "namespace Game; public partial class Player {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "PlayerLook.cs", "namespace Game; public partial class Player {}").IsOk());
	std::array<std::filesystem::path, 2> const halves = {outside / "PlayerMove.cs", outside / "PlayerLook.cs"};
	CHECK(scripts.AddScripts(halves, "Player").IsOk());

	// Sources of the C# project stay where they are, also outside Scripts/Source.
	std::filesystem::path const tool = project.GetDirectory() / "Scripts" / "Tools" / "Tool.cs";
	REQUIRE(FileSystem::WriteTextFile(tool, "namespace Game; class Tool {}").IsOk());
	std::array<std::filesystem::path, 2> const inside = {tool, sources / "AI" / "Steering" / "Mover.cs"};
	Result<std::vector<std::filesystem::path>> const kept = scripts.AddScripts(inside, "");
	REQUIRE(kept.IsOk());
	CHECK(kept.GetValue() == std::vector<std::filesystem::path>(inside.begin(), inside.end()));
	CHECK_FALSE(FileSystem::Exists(sources / "Tool.cs"));

	// Other files, files that do not exist and folders outside the scripts are refused without copying anything.
	REQUIRE(FileSystem::WriteTextFile(outside / "Fresh.cs", "namespace Game; class Fresh {}").IsOk());
	std::array<std::filesystem::path, 2> const mixed = {outside / "Fresh.cs", outside / "Notes.txt"};
	CHECK(scripts.AddScripts(mixed, "").IsError());
	std::array<std::filesystem::path, 1> const missing = {outside / "Missing.cs"};
	CHECK(scripts.AddScripts(missing, "").IsError());
	std::array<std::filesystem::path, 1> const fresh = {outside / "Fresh.cs"};
	for (char const* folder : {"..", "../..", "AI/../../Assets"})
	{
		CAPTURE(folder);
		CHECK(scripts.AddScripts(fresh, folder).IsError());
		CHECK(scripts.CreateScript("Outside", folder).IsError());
	}
	CHECK_FALSE(FileSystem::Exists(sources / "Fresh.cs"));
	CHECK_FALSE(FileSystem::Exists(project.GetDirectory() / "Fresh.cs"));
	CHECK_FALSE(FileSystem::Exists(project.GetDirectory() / "Outside.cs"));

	// Files with paths of their own: targets must stay inside the folder, and copies made before a failure go again with
	// the folders made for them (here the second file needs a folder where the first one became a file).
	REQUIRE(FileSystem::WriteTextFile(outside / "Lib" / "Alpha.cs", "namespace Lib; class Alpha {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Lib" / "Beta.cs", "namespace Lib; class Beta {}").IsOk());
	for (char const* target : {"../Escape.cs", "", "Lib/../../Escape.cs"})
	{
		CAPTURE(target);
		std::array<ScriptFileCopy, 1> const escaping = {{{outside / "Lib" / "Alpha.cs", target}}};
		CHECK(scripts.AddScriptFiles(escaping, "Vendor").IsError());
	}
	std::array<ScriptFileCopy, 2> const blocked = {
		{{outside / "Lib" / "Alpha.cs", "Lib/Alpha.cs"}, {outside / "Lib" / "Beta.cs", "Lib/Alpha.cs/Beta.cs"}}};
	CHECK(scripts.AddScriptFiles(blocked, "Vendor").IsError());
	CHECK_FALSE(FileSystem::Exists(sources / "Vendor"));
	CHECK_FALSE(FileSystem::Exists(project.GetDirectory() / "Escape.cs"));
	std::array<ScriptFileCopy, 2> const library = {
		{{outside / "Lib" / "Alpha.cs", "Lib/Alpha.cs"}, {outside / "Notes.txt", "Lib/Docs/Notes.txt"}}};
	Result<std::vector<std::filesystem::path>> const vendored = scripts.AddScriptFiles(library, "Vendor");
	REQUIRE(vendored.IsOk());
	CHECK(vendored.GetValue() ==
	      std::vector<std::filesystem::path>{(sources / "Vendor" / "Lib" / "Alpha.cs").lexically_normal(),
	                                         (sources / "Vendor" / "Lib" / "Docs" / "Notes.txt").lexically_normal()});

	// New scripts go into the folder the content browser shows, unless their class is declared already.
	Result<std::filesystem::path> const brain = scripts.CreateScript("Brain", "AI");
	REQUIRE(brain.IsOk());
	CHECK(brain.GetValue() == sources / "AI" / "Brain.cs");
	REQUIRE(FileSystem::WriteTextFile(sources / "Helpers.cs", "namespace Game { public class Spawner { } }").IsOk());
	Result<std::filesystem::path> const spawner = scripts.CreateScript("Spawner", "");
	REQUIRE(spawner.IsError());
	CHECK(spawner.GetError().find("Scripts/Source/Helpers.cs declares Game.Spawner") != std::string::npos);
	CHECK_FALSE(FileSystem::Exists(sources / "Spawner.cs"));
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

	// Taken names of assets get a number; a script whose name is taken is refused, as its class would be declared twice.
	std::vector<std::filesystem::path> const again = {outside / "Oak.png", outside / "Mover.cs", outside / "Crate"};
	ProjectImport::Report const second = ProjectImport::Import(operations, &scripts, again, {"Textures", "Gameplay"});
	REQUIRE(second.Errors.size() == 1);
	CHECK(second.Errors.front().find("'Mover.cs' already exists in Scripts/Source/Gameplay") != std::string::npos);
	REQUIRE(second.Assets.size() == 3);
	CHECK(GetAssetPath(second.Assets[0]) == "Textures/Oak 1.png");
	CHECK(second.Folders == std::vector<std::string>{"Textures/Crate 1"});
	CHECK(second.Scripts.empty());
	CHECK_FALSE(FileSystem::Exists(sources / "Gameplay" / "Mover 1.cs"));

	// What is already in the project is registered where it is.
	REQUIRE(Image(2, 2, 4).WritePNG(assetDirectory / "Loose" / "Leaf.png").IsOk());
	std::vector<std::filesystem::path> const inside = {assetDirectory / "Loose"};
	ProjectImport::Report const registered = ProjectImport::Import(operations, &scripts, inside, {"Textures", ""});
	CHECK(registered.Errors.empty());
	REQUIRE(registered.Assets.size() == 1);
	CHECK(GetAssetPath(registered.Assets[0]) == "Loose/Leaf.png");
	CHECK(registered.Folders.empty());

	// Refused: a folder holding the project's assets (with what lies inside it), a drive, a file that is gone, C# sources
	// without the scripts.
	std::vector<std::filesystem::path> const refusedPaths = {temporary.GetPath(), outside / "Oak.png", temporary.GetPath().root_path()};
	ProjectImport::Report const refused = ProjectImport::Import(operations, nullptr, refusedPaths, {});
	CHECK(refused.Errors.size() == 2);
	CHECK(refused.Assets.empty());
	CHECK(refused.Folders.empty());
	std::vector<std::filesystem::path> const unavailable = {outside / "Gone.png", outside / "Mover.cs"};
	ProjectImport::Report const missing = ProjectImport::Import(operations, nullptr, unavailable, {});
	CHECK(missing.Errors.size() == 2);
	CHECK(missing.Assets.empty());
	CHECK(missing.Scripts.empty());

	// Without a project nothing is imported.
	REQUIRE(operations.CloseProject().IsOk());
	CHECK(ProjectImport::Import(operations, &scripts, dropped, {}).Errors == std::vector<std::string>{"no project is open"});
}

namespace
{
	// A one-quad glTF scene with its buffer in <name>.bin beside it and a texture in textures/ (a "glTF Separate" export), or
	// the existing image the URI names.
	void WriteSeparateGltf(std::filesystem::path const& directory, std::string const& name, std::string imageUri = {})
	{
		Testing::GltfTestBuilder builder;
		if (imageUri.empty())
		{
			imageUri = "textures/" + name + "_albedo.png";
			REQUIRE(Image(2, 2, 4).WritePNG(directory / FileSystem::PathFromUtf8(imageUri)).IsOk());
		}
		int const texture = builder.AddTexture(builder.AddImageUri(imageUri));
		Json material = Json::object();
		material["pbrMetallicRoughness"] = Json::object({{"baseColorTexture", Json::object({{"index", texture}})}});
		Testing::GltfTestBuilder::Primitive quad;
		quad.Positions = {{-0.5f, 0.5f, 0.0f}, {-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 0.0f}};
		quad.TexCoords = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
		quad.Indices = {0, 1, 2, 0, 2, 3};
		quad.Material = builder.AddMaterial(material);
		builder.SetScene({builder.AddNode("Quad", builder.AddMesh("Quad", {quad}))});
		REQUIRE(builder.WriteGltf(directory / (name + ".gltf"), false).IsOk());
		REQUIRE(FileSystem::IsRegularFile(directory / (name + ".bin")));
	}

	// Loads a mesh asset: it must load, and each of its materials with its texture (a texture that cannot be read only
	// gives a warning).
	void CheckModelLoads(AssetHandle mesh)
	{
		REQUIRE(mesh.IsValid());
		Result<Ref<MeshSource>> const loaded = AssetManager::TryGetAsset<MeshSource>(mesh);
		REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
		REQUIRE_FALSE(loaded.GetValue()->GetMaterials().empty());
		for (AssetHandle const material : loaded.GetValue()->GetMaterials())
		{
			Ref<MaterialAsset> const data = AssetManager::GetAsset<MaterialAsset>(material);
			REQUIRE(data);
			CHECK(data->GetData().BaseColorTexture.IsValid());
		}
	}
}

TEST_CASE("ProjectImport: models dropped as files come with the files they read")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, GetScriptCoreAssembly());
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	REQUIRE(operations.CreateAssetFolder("Models").IsOk());
	std::filesystem::path const assetDirectory = context.GetProject()->GetAssetDirectory();
	std::filesystem::path const outside = temporary.GetPath() / "Downloads";
	WriteSeparateGltf(outside, "Quad");
	REQUIRE(FileSystem::WriteTextFile(outside / "License.txt", "CC0").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Crate.obj",
	                                  "mtllib Crate.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nusemtl Wood\nf 1/1 2/1 3/1\n")
	            .IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Crate.mtl", "newmtl Wood\nKd 1 1 1\nmap_Kd Wood.png\n").IsOk());
	REQUIRE(Image(2, 2, 4).WritePNG(outside / "Wood.png").IsOk());

	// The glTF alone: its buffer and texture come along at the same paths, and a file dropped with it is kept.
	std::vector<std::filesystem::path> const gltf = {outside / "Quad.gltf", outside / "License.txt"};
	ProjectImport::Report const first = ProjectImport::Import(operations, &scripts, gltf, {"Models", ""});
	CHECK(first.Errors.empty());
	CHECK(first.Skipped.empty());
	REQUIRE(first.Assets.size() == 2);
	CHECK(GetAssetPath(first.Assets[0]) == "Models/Quad.gltf");
	CHECK(GetAssetPath(first.Assets[1]) == "Models/textures/Quad_albedo.png");
	CHECK(first.KeptFiles == std::vector<std::string>{"Models/Quad.bin", "Models/License.txt"});
	CHECK(first.Folders.empty());
	CheckModelLoads(first.Assets[0]);

	// Dropped again (the buffer too), the names are taken: the model and its files go into a folder of their own.
	std::vector<std::filesystem::path> const both = {outside / "Quad.gltf", outside / "Quad.bin"};
	ProjectImport::Report const second = ProjectImport::Import(operations, &scripts, both, {"Models", ""});
	CHECK(second.Errors.empty());
	CHECK(second.Skipped.empty());
	CHECK(second.Folders == std::vector<std::string>{"Models/Quad"});
	REQUIRE(second.Assets.size() == 2);
	CHECK(GetAssetPath(second.Assets[0]) == "Models/Quad/Quad.gltf");
	CHECK(second.KeptFiles == std::vector<std::string>{"Models/Quad/Quad.bin"});
	REQUIRE(second.Notes.size() == 1);
	CHECK(second.Notes.front().find("Quad.gltf") != std::string::npos);
	CheckModelLoads(second.Assets[0]);

	// An OBJ with its material library dropped together: the library is not skipped, and the texture it names comes too.
	std::vector<std::filesystem::path> const obj = {outside / "Crate.obj", outside / "Crate.mtl"};
	ProjectImport::Report const third = ProjectImport::Import(operations, &scripts, obj, {"", ""});
	CHECK(third.Errors.empty());
	CHECK(third.Skipped.empty());
	REQUIRE(third.Assets.size() == 2);
	CHECK(GetAssetPath(third.Assets[0]) == "Crate.obj");
	CHECK(GetAssetPath(third.Assets[1]) == "Wood.png");
	CHECK(third.KeptFiles == std::vector<std::string>{"Crate.mtl"});
	CheckModelLoads(third.Assets[0]);
	Ref<MeshSource> const crate = AssetManager::GetAsset<MeshSource>(third.Assets[0]);
	REQUIRE(crate);
	REQUIRE(crate->GetMaterials().size() == 1);
	Ref<MaterialAsset> const wood = AssetManager::GetAsset<MaterialAsset>(crate->GetMaterials().front());
	REQUIRE(wood);
	CHECK(wood->GetData().BaseColorTexture == third.Assets[1]);

	// A folder dropped with a model that holds the files it reads comes whole, files the model does not read included.
	std::filesystem::path const pack = temporary.GetPath() / "Pack";
	WriteSeparateGltf(pack, "Tree");
	REQUIRE(Image(2, 2, 4).WritePNG(pack / "textures" / "Unused.png").IsOk());
	std::vector<std::filesystem::path> const withFolder = {pack / "Tree.gltf", pack / "textures"};
	ProjectImport::Report const fourth = ProjectImport::Import(operations, &scripts, withFolder, {"Trees", ""});
	CHECK(fourth.Errors.empty());
	CHECK(fourth.Folders.empty());
	CHECK(FileSystem::IsRegularFile(assetDirectory / "Trees" / "textures" / "Unused.png"));
	CHECK(FileSystem::IsRegularFile(assetDirectory / "Trees" / "Tree.bin"));
	CHECK(AssetManager::FindByPath("Trees/textures/Tree_albedo.png").IsValid());
	CheckModelLoads(AssetManager::FindByPath("Trees/Tree.gltf"));

	// Files that are not assets and come without a model are skipped; so are a model's files dropped without it.
	std::vector<std::filesystem::path> const alone = {outside / "Quad.bin", outside / "License.txt"};
	ProjectImport::Report const fifth = ProjectImport::Import(operations, &scripts, alone, {"", ""});
	CHECK(fifth.Skipped == std::vector<std::string>{"Quad.bin", "License.txt"});
	CHECK(fifth.KeptFiles.empty());
	CHECK_FALSE(ProjectImport::DescribeSkipped(fifth).empty());
}

TEST_CASE("ProjectImport: a file named in several spellings is one file, in the project and outside it")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, GetScriptCoreAssembly());
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	std::filesystem::path const assetDirectory = context.GetProject()->GetAssetDirectory();

	// A model whose material library names its texture in two letter cases, as exporters on Windows write them: file
	// systems that ignore case find one file under both. Dropped, the model comes with the texture once.
	std::filesystem::path const outside = temporary.GetPath() / "Downloads" / "Crate";
	REQUIRE(FileSystem::WriteTextFile(outside / "Crate.obj",
	                                  "mtllib Crate.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nusemtl Wood\nf 1/1 2/1 3/1\n")
	            .IsOk());
	REQUIRE(
		FileSystem::WriteTextFile(outside / "Crate.mtl", "newmtl Wood\nKd 1 1 1\nmap_Kd Textures/Wood.png\nmap_bump textures/WOOD.png\n")
			.IsOk());
	REQUIRE(Image(2, 2, 4).WritePNG(outside / "Textures" / "Wood.png").IsOk());
	std::vector<std::filesystem::path> const model = {outside / "Crate.obj"};
	ProjectImport::Report const dropped = ProjectImport::Import(operations, &scripts, model, {"Props", ""});
	CHECK(dropped.Errors.empty());
	REQUIRE(dropped.Assets.size() == 2);
	CHECK(GetAssetPath(dropped.Assets[0]) == "Props/Crate.obj");
	CHECK(GetAssetPath(dropped.Assets[1]) == "Props/Textures/Wood.png");
	CHECK(dropped.KeptFiles == std::vector<std::string>{"Props/Crate.mtl"});
	CheckModelLoads(dropped.Assets[0]);

	// Moved to another folder, the texture goes along once, keeping its handle.
	REQUIRE(operations.MoveAsset(dropped.Assets[0], "Crates/Crate.obj").IsOk());
	CHECK(GetAssetPath(dropped.Assets[1]) == "Crates/Textures/Wood.png");
	CHECK(FileSystem::IsRegularFile(assetDirectory / "Crates" / "Crate.mtl"));
	CHECK_FALSE(FileSystem::Exists(assetDirectory / "Props" / "Textures"));
	AssetManager::UnloadAsset(dropped.Assets[0]);
	CheckModelLoads(dropped.Assets[0]);

	if (!Testing::IgnoresLetterCase(temporary.GetPath()))
	{
		MESSAGE("The file system tells letter cases apart: other spellings name other files");
		return;
	}
	// The project's folders and files spelled otherwise are still the project's: the asset directory is not copied into
	// itself, and a texture of the project is the asset it is.
	size_t const registered = AssetManager::GetAssets().size();
	std::vector<std::filesystem::path> const ownFolder = {temporary.GetPath() / "GAME" / "ASSETS"};
	ProjectImport::Report const folder = ProjectImport::Import(operations, &scripts, ownFolder, {"", ""});
	REQUIRE(folder.Errors.size() == 1);
	CHECK(folder.Errors.front().find("holds the project's assets") != std::string::npos);
	std::vector<std::filesystem::path> const ownTexture = {temporary.GetPath() / "game" / "assets" / "crates" / "textures" / "wood.PNG"};
	ProjectImport::Report const texture = ProjectImport::Import(operations, &scripts, ownTexture, {"", ""});
	CHECK(texture.Errors.empty());
	CHECK(texture.Assets == std::vector<AssetHandle>{dropped.Assets[1]});
	CHECK(AssetManager::GetAssets().size() == registered);
	CHECK_FALSE(FileSystem::Exists(assetDirectory / "Wood.png"));
	// The same goes for files given to the operations directly.
	Result<std::vector<AssetHandle>> const imported = operations.ImportAssets(ownTexture, "Elsewhere");
	REQUIRE(imported.IsOk());
	CHECK(imported.GetValue() == std::vector<AssetHandle>{dropped.Assets[1]});
	CHECK(AssetManager::GetAssets().size() == registered);
}

TEST_CASE("ProjectImport: dropped folders of C# sources go to the scripts, and so do the C# sources of other folders")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, GetScriptCoreAssembly());
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	std::filesystem::path const assetDirectory = context.GetProject()->GetAssetDirectory();
	std::filesystem::path const sources = ScriptProject::GetSourceDirectory(*context.GetProject());
	std::filesystem::path const outside = temporary.GetPath() / "Downloads";

	// An AI folder: C# sources in subfolders and notes, no assets. A library with its own C# project and build output.
	REQUIRE(FileSystem::WriteTextFile(outside / "AI" / "Brain.cs", "namespace Game.AI; class Brain {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "AI" / "Steering" / "Seek.cs", "namespace Game.AI; class Seek {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "AI" / "README.md", "notes").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Lib" / "Lib.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\" />").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Lib" / "Util.cs", "namespace Lib; static class Util {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Lib" / "obj" / "Debug" / "Lib.AssemblyInfo.cs",
	                                  "[assembly: System.Reflection.AssemblyTitle(\"Lib\")]")
	            .IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Lib" / "bin" / "Old.cs", "class Old {}").IsOk());
	// A pack: assets with a script among them.
	REQUIRE(Image(2, 2, 4).WritePNG(outside / "Pack" / "Textures" / "Wood.png").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Pack" / "Scripts" / "Spin.cs", "namespace Game; class Spin {}").IsOk());

	std::vector<std::filesystem::path> const dropped = {outside / "AI", outside / "Lib", outside / "Pack"};
	ProjectImport::Report const report = ProjectImport::Import(operations, &scripts, dropped, {"Art", "Gameplay"});
	ProjectImport::Log(report);
	CHECK(report.Errors.empty());
	CHECK(report.ScriptFolders == std::vector<std::string>{"Gameplay/AI", "Gameplay/Lib", "Gameplay/Pack"});
	CHECK(report.Folders == std::vector<std::string>{"Art/Pack"});
	CHECK(FileSystem::IsRegularFile(sources / "Gameplay" / "AI" / "Brain.cs"));
	CHECK(FileSystem::IsRegularFile(sources / "Gameplay" / "AI" / "Steering" / "Seek.cs"));
	CHECK(FileSystem::IsRegularFile(sources / "Gameplay" / "AI" / "README.md"));
	CHECK(FileSystem::IsRegularFile(sources / "Gameplay" / "Lib" / "Util.cs"));
	// The library's build output stays behind: its generated sources would break the scripts' build.
	CHECK_FALSE(FileSystem::Exists(sources / "Gameplay" / "Lib" / "obj"));
	CHECK_FALSE(FileSystem::Exists(sources / "Gameplay" / "Lib" / "bin"));
	// The pack's assets go to the assets, its script to the scripts; no folder is left that held only C# sources.
	CHECK(AssetManager::FindByPath("Art/Pack/Textures/Wood.png").IsValid());
	CHECK(FileSystem::IsRegularFile(sources / "Gameplay" / "Pack" / "Scripts" / "Spin.cs"));
	CHECK_FALSE(FileSystem::Exists(assetDirectory / "Art" / "Pack" / "Scripts"));
	REQUIRE(report.Notes.size() == 1);
	CHECK(report.Notes.front().find("Scripts/Source/Gameplay/Pack") != std::string::npos);
	CHECK(report.Scripts.size() == 4);
	CHECK(report.KeptFiles.empty());
	CHECK(FileSystem::IsRegularFile(context.GetProject()->GetDirectory() / "Scripts" / "Game.csproj"));

	// Dropped again, the folders would declare their classes twice: refused whole, the pack's assets included.
	ProjectImport::Report const again = ProjectImport::Import(operations, &scripts, dropped, {"Art", "Gameplay"});
	CHECK(again.Errors.size() == 3);
	CHECK(again.ScriptFolders.empty());
	CHECK(again.Folders.empty());
	CHECK(again.Assets.empty());
	CHECK_FALSE(FileSystem::Exists(sources / "Gameplay" / "AI 1"));
	CHECK_FALSE(FileSystem::Exists(assetDirectory / "Art" / "Pack 1"));
	CHECK_FALSE(AssetManager::FindByPath("Art/Pack 1/Textures/Wood.png").IsValid());

	// Without the scripts, a folder with C# sources is refused; one without any comes in.
	REQUIRE(FileSystem::WriteTextFile(outside / "Docs" / "Guide.md", "guide").IsOk());
	std::vector<std::filesystem::path> const plain = {outside / "Docs", outside / "Pack"};
	ProjectImport::Report const unscripted = ProjectImport::Import(operations, nullptr, plain, {"", ""});
	REQUIRE(unscripted.Errors.size() == 1);
	CHECK(unscripted.Errors.front().find("Pack") != std::string::npos);
	CHECK(unscripted.Folders == std::vector<std::string>{"Docs"});
	CHECK(unscripted.KeptFiles == std::vector<std::string>{"Docs/Guide.md"});
}

TEST_CASE("ProjectImport: large folders are refused and failed imports leave nothing behind")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, GetScriptCoreAssembly());
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	std::filesystem::path const assetDirectory = context.GetProject()->GetAssetDirectory();
	std::filesystem::path const outside = temporary.GetPath() / "Downloads";
	for (char const* file : {"Big/A.png", "Big/B.png", "Big/Sub/C.png", "Big/Sub/D.txt"})
	{
		REQUIRE(Image(2, 2, 4).WritePNG(outside / FileSystem::PathFromUtf8(file)).IsOk());
	}
	std::vector<std::filesystem::path> const big = {outside / "Big"};

	// More files than the limit: refused before anything is copied.
	ProjectImport::Report const refused = ProjectImport::Import(operations, &scripts, big, {"", ""}, 3);
	REQUIRE(refused.Errors.size() == 1);
	CHECK(refused.Errors.front().find("more than 3 files") != std::string::npos);
	CHECK_FALSE(FileSystem::Exists(assetDirectory / "Big"));
	ProjectImport::Report const allowed = ProjectImport::Import(operations, &scripts, big, {"", ""}, 4);
	CHECK(allowed.Errors.empty());
	CHECK(allowed.Folders == std::vector<std::string>{"Big"});

	// What lies inside a dropped folder comes with it, not again on its own.
	REQUIRE(operations.CreateAssetFolder("Nested").IsOk());
	std::vector<std::filesystem::path> const nested = {outside / "Big" / "Sub" / "C.png", outside / "Big"};
	ProjectImport::Report const once = ProjectImport::Import(operations, &scripts, nested, {"Nested", ""});
	CHECK(once.Errors.empty());
	CHECK(once.Folders == std::vector<std::string>{"Nested/Big"});
	CHECK(once.Assets.size() == 3);
	CHECK_FALSE(FileSystem::Exists(assetDirectory / "Nested" / "C.png"));

	// A destination that cannot hold folders (a file of that name): nothing is copied or registered.
	REQUIRE(FileSystem::WriteTextFile(assetDirectory / "Blocked", "a file").IsOk());
	REQUIRE(Image(2, 2, 4).WritePNG(outside / "Loose.png").IsOk());
	std::vector<std::filesystem::path> const mixed = {outside / "Big", outside / "Loose.png"};
	ProjectImport::Report const blocked = ProjectImport::Import(operations, &scripts, mixed, {"Blocked", ""});
	CHECK(blocked.Errors.size() == 2);
	CHECK(blocked.Assets.empty());
	CHECK(blocked.Folders.empty());
	CHECK(FileSystem::IsRegularFile(assetDirectory / "Blocked"));
}

TEST_CASE("FileCopies: copies are undone with the folders made for them")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const from = temporary.GetPath() / "From";
	std::filesystem::path const to = temporary.GetPath() / "To";
	REQUIRE(FileSystem::WriteTextFile(from / "A.txt", "a").IsOk());
	REQUIRE(FileSystem::WriteTextFile(from / "B.txt", "b").IsOk());
	REQUIRE(FileSystem::WriteTextFile(to / "Existing" / "Keep.txt", "keep").IsOk());

	FileCopies copies;
	REQUIRE(copies.Copy(from / "A.txt", to / "New" / "Deep" / "A.txt").IsOk());
	REQUIRE(copies.Copy(from / "B.txt", to / "Existing" / "B.txt").IsOk());
	CHECK(copies.GetCopies() == std::vector<std::filesystem::path>{to / "New" / "Deep" / "A.txt", to / "Existing" / "B.txt"});
	// Nothing is replaced.
	CHECK(copies.Copy(from / "A.txt", to / "Existing" / "Keep.txt").IsError());
	CHECK(FileSystem::ReadTextFile(to / "Existing" / "Keep.txt").GetValue() == "keep");

	copies.Rollback();
	CHECK_FALSE(FileSystem::Exists(to / "New"));
	CHECK_FALSE(FileSystem::Exists(to / "Existing" / "B.txt"));
	CHECK(FileSystem::IsRegularFile(to / "Existing" / "Keep.txt"));
	CHECK(copies.GetCopies().empty());
	CHECK(FileSystem::IsRegularFile(from / "A.txt"));
}

TEST_CASE("EditorOperations: a model moved to another folder takes the files it reads along")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	std::filesystem::path const assetDirectory = context.GetProject()->GetAssetDirectory();
	WriteSeparateGltf(assetDirectory / "Models", "Quad");
	REQUIRE(operations.RefreshAssets().IsOk());
	AssetHandle const quad = AssetManager::FindByPath("Models/Quad.gltf");
	AssetHandle const albedo = AssetManager::FindByPath("Models/textures/Quad_albedo.png");
	REQUIRE(quad.IsValid());
	REQUIRE(albedo.IsValid());

	// Renamed in its folder, it moves alone.
	REQUIRE(operations.MoveAsset(quad, "Models/Plane.gltf").IsOk());
	CHECK(FileSystem::IsRegularFile(assetDirectory / "Models" / "Quad.bin"));
	CheckModelLoads(quad);

	// Into another folder, its buffer and texture follow at the same paths; the texture keeps its handle, and the folder it
	// leaves empty goes.
	REQUIRE(operations.MoveAsset(quad, "Props/Plane.gltf").IsOk());
	CHECK(FileSystem::IsRegularFile(assetDirectory / "Props" / "Plane.gltf"));
	CHECK(FileSystem::IsRegularFile(assetDirectory / "Props" / "Quad.bin"));
	CHECK(AssetManager::GetMetadata(albedo)->Path == "Props/textures/Quad_albedo.png");
	CHECK_FALSE(FileSystem::Exists(assetDirectory / "Models" / "Quad.bin"));
	CHECK_FALSE(FileSystem::Exists(assetDirectory / "Models" / "textures"));
	AssetManager::UnloadAsset(quad);
	CheckModelLoads(quad);

	// Refused, with nothing moved: another model of the folder reads the same buffer, or the target's files exist.
	REQUIRE(FileSystem::Copy(assetDirectory / "Props" / "Plane.gltf", assetDirectory / "Props" / "Twin.gltf", false).IsOk());
	REQUIRE(operations.RefreshAssets().IsOk());
	Result<void> const shared = operations.MoveAsset(quad, "Elsewhere/Plane.gltf");
	REQUIRE(shared.IsError());
	CHECK(shared.GetError().find("Props/Twin.gltf") != std::string::npos);
	REQUIRE(operations.DeleteAsset(AssetManager::FindByPath("Props/Twin.gltf")).IsOk());
	REQUIRE(FileSystem::WriteTextFile(assetDirectory / "Elsewhere" / "Quad.bin", "taken").IsOk());
	Result<void> const taken = operations.MoveAsset(quad, "Elsewhere/Plane.gltf");
	REQUIRE(taken.IsError());
	CHECK(taken.GetError().find("Elsewhere/Quad.bin") != std::string::npos);
	CHECK(AssetManager::GetMetadata(quad)->Path == "Props/Plane.gltf");
	CHECK(AssetManager::GetMetadata(albedo)->Path == "Props/textures/Quad_albedo.png");
	CHECK(FileSystem::IsRegularFile(assetDirectory / "Props" / "Quad.bin"));
	CHECK_FALSE(FileSystem::Exists(assetDirectory / "Elsewhere" / "textures"));

	// A texture outside the model's folder stays where it is: the model moves to a folder beside its own, from where the
	// texture is found the same way, but not deeper.
	REQUIRE(Image(2, 2, 4).WritePNG(assetDirectory / "Textures" / "Shared.png").IsOk());
	WriteSeparateGltf(assetDirectory / "Kits", "Panel", "../Textures/Shared.png");
	REQUIRE(operations.RefreshAssets().IsOk());
	AssetHandle const panel = AssetManager::FindByPath("Kits/Panel.gltf");
	REQUIRE(panel.IsValid());
	Result<void> const deeper = operations.MoveAsset(panel, "Kits/Sub/Panel.gltf");
	REQUIRE(deeper.IsError());
	CHECK(deeper.GetError().find("Textures/Shared.png") != std::string::npos);
	CHECK(FileSystem::IsRegularFile(assetDirectory / "Kits" / "Panel.bin"));
	REQUIRE(operations.MoveAsset(panel, "Walls/Panel.gltf").IsOk());
	CHECK(FileSystem::IsRegularFile(assetDirectory / "Walls" / "Panel.bin"));
	CHECK(FileSystem::IsRegularFile(assetDirectory / "Textures" / "Shared.png"));
	AssetManager::UnloadAsset(panel);
	CheckModelLoads(panel);

	// A model of another folder reading a file of the moved model's folder keeps it there: the move is refused.
	WriteSeparateGltf(assetDirectory / "Rocks" / "A", "RockA");
	WriteSeparateGltf(assetDirectory / "Rocks" / "B", "RockB", "../A/textures/RockA_albedo.png");
	REQUIRE(operations.RefreshAssets().IsOk());
	AssetHandle const rockA = AssetManager::FindByPath("Rocks/A/RockA.gltf");
	REQUIRE(rockA.IsValid());
	Result<void> const read = operations.MoveAsset(rockA, "Cliffs/RockA.gltf");
	REQUIRE(read.IsError());
	CHECK(read.GetError().find("Rocks/B/RockB.gltf") != std::string::npos);
	CHECK(read.GetError().find("Rocks/A/textures/RockA_albedo.png") != std::string::npos);
	CHECK(AssetManager::GetMetadata(rockA)->Path == "Rocks/A/RockA.gltf");
	CHECK(FileSystem::IsRegularFile(assetDirectory / "Rocks" / "A" / "textures" / "RockA_albedo.png"));
	CHECK_FALSE(FileSystem::Exists(assetDirectory / "Cliffs"));
	CheckModelLoads(AssetManager::FindByPath("Rocks/B/RockB.gltf"));
}

TEST_CASE("AssetDrops: a C# source dropped onto entities makes them run its class, as one undoable step")
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

	// One step adds the component to one entity and replaces the class of the other (stored field values stay).
	std::array<UUID, 2> const both = {plain, scripted};
	Result<std::string> const added = AssetDrops::AddScript(operations, both, mover);
	REQUIRE_MESSAGE(added.IsOk(), (added ? std::string() : added.GetError()));
	CHECK(added.GetValue() == "Strada.Tests.Mover");
	Scene& scene = context.GetScene();
	CHECK(scene.GetEntityByUUID(plain).GetComponent<ScriptComponent>().ClassName == "Strada.Tests.Mover");
	CHECK(scene.GetEntityByUUID(scripted).GetComponent<ScriptComponent>().ClassName == "Strada.Tests.Mover");
	CHECK(scene.GetEntityByUUID(scripted).GetComponent<ScriptComponent>().Fields.size() == 1);
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);

	// Entities that run the class already are left alone.
	CHECK(AssetDrops::AddScript(operations, both, mover).IsOk());
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);

	// One undo restores both entities; redo makes the drop again.
	REQUIRE(operations.Undo().IsOk());
	CHECK(scene.GetEntityByUUID(scripted).GetComponent<ScriptComponent>().ClassName == "Strada.Tests.FieldTypes");
	CHECK_FALSE(scene.GetEntityByUUID(plain).HasComponent<ScriptComponent>());
	CHECK(context.GetHistory().GetUndoCount() == steps);
	REQUIRE(operations.Redo().IsOk());
	CHECK(scene.GetEntityByUUID(plain).GetComponent<ScriptComponent>().ClassName == "Strada.Tests.Mover");
	CHECK(scene.GetEntityByUUID(scripted).GetComponent<ScriptComponent>().ClassName == "Strada.Tests.Mover");
	REQUIRE(operations.Undo().IsOk());
	std::array<UUID, 1> const plainOnly = {plain};
	CHECK(operations.SetScriptClass(plainOnly, "").IsError());

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

TEST_CASE("AssetDrops: a visual script dropped onto entities makes them run it, as one undoable step")
{
	// No scripting runtime: visual scripts are assets the editor sets without it.
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	Result<AssetHandle> const spin = operations.CreateVisualScript("Scripts/Spin.sgraph");
	Result<AssetHandle> const hover = operations.CreateVisualScript("Scripts/Hover.sgraph");
	Result<AssetHandle> const rock = operations.CreateMaterial("Rock.smat");
	REQUIRE(spin.IsOk());
	REQUIRE(hover.IsOk());
	REQUIRE(rock.IsOk());

	Json const speed = Json::object({{"Speed", Json::object({{"Type", "Float"}, {"Value", 3.0}})}});
	UUID const plain = CreateEntity(operations, "Plain");
	UUID const hovering = CreateEntity(operations, "Hovering",
	                                   Json::object({{"VisualScript", {{"Graph", hover.GetValue().ToString()}, {"Variables", speed}}}}));
	size_t const steps = context.GetHistory().GetUndoCount();
	Scene& scene = context.GetScene();
	auto const getGraph = [&scene](UUID id)
	{
		Entity const entity = scene.GetEntityByUUID(id);
		return entity.HasComponent<VisualScriptComponent>() ? entity.GetComponent<VisualScriptComponent>().Graph : AssetHandle();
	};
	auto const getVariables = [&scene](UUID id)
	{
		Entity const entity = scene.GetEntityByUUID(id);
		return entity.GetComponent<VisualScriptComponent>().Variables;
	};
	ScriptFieldMap const speedValues = {{"Speed", ScriptFieldValue::FromFloat(3.0f)}};

	// One step adds the component to one entity and replaces the graph of the other, whose variable values stay.
	std::array<UUID, 2> const both = {plain, hovering};
	Result<std::string> const added = AssetDrops::AddVisualScript(operations, both, spin.GetValue());
	REQUIRE_MESSAGE(added.IsOk(), (added ? std::string() : added.GetError()));
	CHECK(added.GetValue() == "Spin.sgraph");
	CHECK(getGraph(plain) == spin.GetValue());
	CHECK(getVariables(plain).empty());
	CHECK(getGraph(hovering) == spin.GetValue());
	CHECK(getVariables(hovering) == speedValues);
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);
	CHECK(context.GetHistory().GetUndoDescription() == "Set Visual Script Spin.sgraph");

	// Entities that run the graph already are left alone: no step at all.
	CHECK(AssetDrops::AddVisualScript(operations, both, spin.GetValue()).IsOk());
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);

	// One undo restores both entities; redo makes the drop again.
	REQUIRE(operations.Undo().IsOk());
	CHECK(getGraph(plain) == AssetHandle());
	CHECK(getGraph(hovering) == hover.GetValue());
	CHECK(getVariables(hovering) == speedValues);
	CHECK(context.GetHistory().GetUndoCount() == steps);
	REQUIRE(operations.Redo().IsOk());
	CHECK(getGraph(plain) == spin.GetValue());
	CHECK(getGraph(hovering) == spin.GetValue());
	REQUIRE(operations.Undo().IsOk());

	// Only the entities that need it change: the one running Hover already keeps it, as the step does not touch it.
	std::array<UUID, 2> const again = {plain, hovering};
	REQUIRE(operations.SetVisualScriptGraph(again, hover.GetValue()).IsOk());
	CHECK(getGraph(plain) == hover.GetValue());
	CHECK(getGraph(hovering) == hover.GetValue());
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);
	REQUIRE(operations.Undo().IsOk());
	std::array<UUID, 1> const one = {plain};
	CHECK(operations.SetVisualScriptGraph(one, AssetHandle()).IsError());

	// Failures change nothing and say why: no entities, an entity that does not exist (all or nothing), an asset that is no
	// visual script, a handle that is not registered.
	Result<std::string> const nobody = AssetDrops::AddVisualScript(operations, std::span<UUID const>(), spin.GetValue());
	REQUIRE(nobody.IsError());
	CHECK(nobody.GetError() == "there is no entity to add the visual script to");
	std::array<UUID, 2> const unknown = {plain, UUID(12345)};
	Result<std::string> const missing = AssetDrops::AddVisualScript(operations, unknown, spin.GetValue());
	REQUIRE(missing.IsError());
	CHECK(missing.GetError() == "entity 12345 does not exist");
	Result<std::string> const material = AssetDrops::AddVisualScript(operations, one, rock.GetValue());
	REQUIRE(material.IsError());
	CHECK(material.GetError() == "asset " + rock.GetValue().ToString() + " is not a VisualScript");
	CHECK(AssetDrops::AddVisualScript(operations, one, AssetHandle(UUID(987654321))).IsError());
	CHECK(getGraph(plain) == AssetHandle());
	CHECK(getGraph(hovering) == hover.GetValue());
	CHECK(context.GetHistory().GetUndoCount() == steps);
}
