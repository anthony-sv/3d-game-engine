#include "TestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/EditorCamera.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/Viewport/ViewportOverlays.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/Base64.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Log.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/ComponentTraits.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// An editor model with every command registered, as the editor layer sets it up.
	struct CommandFixture
	{
		EditorContext Context;
		EditorOperations Operations{Context};
		CommandRegistry Registry;
		bool QuitRequested = false;
		int ProjectsOpened = 0;
		std::optional<EditorCommandEnvironment::ScreenshotCallback> PendingScreenshot;
		// The viewport's camera and renderer statistics when there is a window (no statistics: no GPU renderer).
		EditorCamera Camera;
		SceneViewOverlays Overlays;
		std::optional<SceneRendererStatistics> Statistics;
		ContentBrowserPanel ContentBrowser;
		// What inspector.add-component-menu asked the inspector to do: open (or close) the menu with a search text.
		std::vector<std::pair<bool, std::string>> AddComponentMenuRequests;

		explicit CommandFixture(bool withWindow = false)
		{
			EditorCommandEnvironment environment;
			environment.RequestQuit = [this]
			{
				QuitRequested = true;
			};
			environment.ProjectOpened = [this]
			{
				ProjectsOpened++;
			};
			if (withWindow)
			{
				environment.CaptureScreenshot = [this](EditorCommandEnvironment::ScreenshotCallback callback)
				{
					PendingScreenshot = std::move(callback);
				};
				environment.ViewportCamera = [this]() -> EditorCamera&
				{
					return Camera;
				};
				environment.ViewportStatistics = [this]() -> SceneRendererStatistics const*
				{
					return Statistics ? &*Statistics : nullptr;
				};
				environment.ViewportOverlays = [this]() -> SceneViewOverlays&
				{
					return Overlays;
				};
				environment.ContentBrowser = [this]() -> ContentBrowserPanel&
				{
					return ContentBrowser;
				};
				environment.ShowAddComponentMenu = [this](bool open, std::string const& search)
				{
					AddComponentMenuRequests.emplace_back(open, search);
				};
			}
			REQUIRE(RegisterEditorCommands(Registry, Operations, std::move(environment)).IsOk());
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

		AutomationErrorCode Fails(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE(result.IsError());
			return result.GetError().Code;
		}

		std::string Create(std::string name, Json extra = Json::object())
		{
			extra["name"] = std::move(name);
			return Ok("entity.create", std::move(extra))["id"].get<std::string>();
		}
	};
}

TEST_CASE("EditorCommands: every command is registered with a schema and description")
{
	CommandFixture fixture;
	for (char const* name :
	     {"editor.status",    "editor.commands",     "editor.undo",     "editor.redo",      "editor.quit",         "scene.new",
	      "scene.open",       "scene.save",          "scene.hierarchy", "scene.dump",       "scene.settings",      "entity.create",
	      "entity.delete",    "entity.duplicate",    "entity.rename",   "entity.reparent",  "entity.find",         "entity.get",
	      "entity.select",    "component.types",     "component.add",   "component.remove", "component.get",       "component.set",
	      "log.read",         "viewport.screenshot", "viewport.camera", "viewport.frame",   "viewport.statistics", "asset.browse",
	      "viewport.overlays"})
	{
		CAPTURE(name);
		CHECK(fixture.Registry.Contains(name));
	}
	Json const commands = fixture.Ok("editor.commands");
	CHECK(commands.size() == fixture.Registry.GetCommandCount());
	for (Json const& command : commands)
	{
		CHECK_FALSE(command["description"].get<std::string>().empty());
	}
}

TEST_CASE("EditorCommands: every command is documented in Docs/Automation.md")
{
	CommandFixture fixture;
	Result<std::string> reference = FileSystem::ReadTextFile(STRADA_AUTOMATION_DOC_PATH);
	REQUIRE(reference.IsOk());
	for (CommandDefinition const* command : fixture.Registry.GetCommands())
	{
		CAPTURE(command->Name);
		CHECK(reference.GetValue().find("`" + command->Name + "`") != std::string::npos);
	}
	for (AutomationErrorCode const code : AllAutomationErrorCodes)
	{
		CAPTURE(GetAutomationErrorCodeName(code));
		CHECK(reference.GetValue().find("| " + std::string(GetAutomationErrorCodeName(code)) + " |") != std::string::npos);
	}
}

TEST_CASE("EditorCommands: entities can be created, inspected, edited and undone")
{
	CommandFixture fixture;
	std::string const parent = fixture.Create("Parent");
	std::string const crate =
		fixture.Create("Crate", Json::object({{"parent", parent},
	                                          {"components", Json::object({{"PointLight", Json::object({{"Range", 4.0}})},
	                                                                       {"Transform", Json::object({{"Translation", {1, 2, 3}}})}})}}));

	Json const entity = fixture.Ok("entity.get", Json::object({{"entity", crate}}));
	CHECK(entity["name"] == "Crate");
	CHECK(entity["parent"] == parent);
	CHECK(entity["components"]["PointLight"]["Range"] == 4.0);
	CHECK(entity["components"]["Transform"]["Translation"] == Json::array({1.0, 2.0, 3.0}));
	CHECK_FALSE(entity["components"].contains("Relationship"));

	Json const set =
		fixture.Ok("component.set", Json::object({{"entity", crate}, {"component", "PointLight"}, {"fields", {{"Intensity", 2.5}}}}));
	CHECK(set["fields"]["Intensity"] == 2.5);
	CHECK(set["fields"]["Range"] == 4.0);
	fixture.Ok("entity.rename", Json::object({{"entity", crate}, {"name", "Box"}}));
	fixture.Ok("component.add", Json::object({{"entity", crate}, {"component", "Camera"}, {"fields", {{"Primary", false}}}}));
	CHECK(fixture.Ok("component.get", Json::object({{"entity", crate}, {"component", "Camera"}}))["fields"]["Primary"] == false);
	fixture.Ok("component.remove", Json::object({{"entity", crate}, {"component", "Camera"}}));
	fixture.Ok("entity.reparent", Json::object({{"entity", crate}, {"parent", nullptr}}));

	Json const hierarchy = fixture.Ok("scene.hierarchy");
	CHECK(hierarchy["entities"].size() == 2);
	CHECK(fixture.Ok("entity.find", Json::object({{"component", "PointLight"}}))["entities"][0]["name"] == "Box");

	Json const copies = fixture.Ok("entity.duplicate", Json::object({{"entities", {crate}}}));
	REQUIRE(copies["ids"].size() == 1);
	fixture.Ok("entity.delete", Json::object({{"entities", {parent, copies["ids"][0]}}}));
	CHECK(fixture.Context.GetScene().GetEntityCount() == 1);

	Json const status = fixture.Ok("editor.status");
	CHECK(status["scene"]["dirty"] == true);
	CHECK(status["undo"].is_string());
	while (fixture.Context.GetHistory().CanUndo())
	{
		fixture.Ok("editor.undo");
	}
	CHECK(fixture.Context.GetScene().GetEntityCount() == 0);
	CHECK(fixture.Fails("editor.undo") == AutomationErrorCode::InvalidOperation);
	fixture.Ok("editor.redo");
	CHECK(fixture.Context.GetScene().GetEntityCount() == 1);
}

TEST_CASE("EditorCommands: errors carry precise codes")
{
	CommandFixture fixture;
	std::string const entity = fixture.Create("E");
	std::string const child = fixture.Create("Child", Json::object({{"parent", entity}}));

	CHECK(fixture.Fails("entity.get", Json::object({{"entity", "12345"}})) == AutomationErrorCode::EntityNotFound);
	CHECK(fixture.Fails("entity.get", Json::object({{"entity", "not-a-number"}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("entity.rename", Json::object({{"entity", entity}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("component.add", Json::object({{"entity", entity}, {"component", "Teleporter"}})) ==
	      AutomationErrorCode::ComponentNotFound);
	CHECK(fixture.Fails("component.add", Json::object({{"entity", entity}, {"component", "Relationship"}})) ==
	      AutomationErrorCode::ComponentNotFound);
	CHECK(fixture.Fails("component.add", Json::object({{"entity", entity}, {"component", "Transform"}})) ==
	      AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("component.remove", Json::object({{"entity", entity}, {"component", "Tag"}})) ==
	      AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("component.get", Json::object({{"entity", entity}, {"component", "Camera"}})) ==
	      AutomationErrorCode::ComponentNotFound);
	CHECK(fixture.Fails("component.set", Json::object({{"entity", entity}, {"component", "Transform"}, {"fields", {{"Scale", "big"}}}})) ==
	      AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("entity.reparent", Json::object({{"entity", entity}, {"parent", child}})) == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("entity.create", Json::object({{"components", {{"Nope", Json::object()}}}})) ==
	      AutomationErrorCode::ComponentNotFound);
	CHECK(fixture.Fails("scene.save") == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("viewport.screenshot") == AutomationErrorCode::Unavailable);
	CHECK(fixture.Fails("viewport.camera") == AutomationErrorCode::Unavailable);
	CHECK(fixture.Fails("viewport.frame", Json::object({{"entities", {entity}}})) == AutomationErrorCode::Unavailable);
	CHECK(fixture.Fails("viewport.statistics") == AutomationErrorCode::Unavailable);
	CHECK(fixture.Fails("asset.browse", Json::object({{"folder", ""}})) == AutomationErrorCode::Unavailable);
	CHECK(fixture.Fails("inspector.add-component-menu") == AutomationErrorCode::Unavailable);
	CHECK(fixture.Fails("viewport.overlays") == AutomationErrorCode::Unavailable);
}

TEST_CASE("EditorCommands: inspector.add-component-menu opens the inspector's menu for the selected entities")
{
	CommandFixture fixture(true);
	std::string const lamp = fixture.Create("Lamp", Json::object({{"components", {{"PointLight", Json::object()}}}}));
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	Json const scene = fixture.Ok("scene.dump");

	// The menu adds components to the selected entities.
	CHECK(fixture.Fails("inspector.add-component-menu") == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.AddComponentMenuRequests.empty());
	fixture.Ok("entity.select", Json::object({{"entities", {lamp}}}));

	// A search lists the matches in the menu's order; components the entity has are not offered.
	Json const lights = fixture.Ok("inspector.add-component-menu", Json::object({{"search", "light"}}));
	CHECK(lights["open"] == true);
	CHECK(lights["search"] == "light");
	CHECK(lights["components"] == Json::array({"DirectionalLight", "SkyLight", "SpotLight"}));
	REQUIRE(fixture.AddComponentMenuRequests.size() == 1);
	CHECK(fixture.AddComponentMenuRequests.back() == std::pair<bool, std::string>(true, "light"));

	// Without a search the menu shows the categories: every component the lamp can gain, once, category by category.
	Json const all = fixture.Ok("inspector.add-component-menu")["components"];
	size_t addable = 0;
	for (ComponentInfo const& info : ComponentRegistry::GetComponents())
	{
		if (!info.IsCore() && !info.IsInternal() && info.Name != "PointLight")
		{
			addable++;
		}
	}
	REQUIRE(all.size() == addable);
	CHECK(all.front() == "Camera");
	size_t previousCategory = 0;
	std::set<std::string> listed;
	for (Json const& name : all)
	{
		CAPTURE(name);
		ComponentInfo const* component = ComponentRegistry::Find(name.get<std::string>());
		REQUIRE(component != nullptr);
		CHECK(component->Name != "PointLight");
		CHECK(listed.insert(name.get<std::string>()).second);
		size_t const category = static_cast<size_t>(
			std::find(ComponentCategory::All.begin(), ComponentCategory::All.end(), component->Category) - ComponentCategory::All.begin());
		CHECK(category >= previousCategory);
		previousCategory = category;
	}
	CHECK(fixture.Ok("inspector.add-component-menu", Json::object({{"search", "nothing like this"}}))["components"].empty());

	// Closing needs no selection.
	fixture.Ok("entity.select", Json::object({{"entities", {lamp}}, {"mode", "remove"}}));
	REQUIRE(fixture.Context.GetSelection().IsEmpty());
	Json const closed = fixture.Ok("inspector.add-component-menu", Json::object({{"open", false}}));
	CHECK(closed["open"] == false);
	CHECK(closed["components"].empty());
	CHECK(fixture.AddComponentMenuRequests.back() == std::pair<bool, std::string>(false, ""));

	// Nothing in the scene changed.
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
	CHECK(fixture.Ok("scene.dump") == scene);
}

TEST_CASE("EditorCommands: scene files and unsaved changes")
{
	Testing::TemporaryDirectory directory;
	CommandFixture fixture;
	fixture.Create("Keep");
	std::string const path = FileSystem::PathToUtf8(directory.GetPath() / "Main.sscene");

	CHECK(fixture.Fails("scene.new") == AutomationErrorCode::UnsavedChanges);
	CHECK(fixture.Fails("editor.quit") == AutomationErrorCode::UnsavedChanges);
	CHECK_FALSE(fixture.QuitRequested);

	Json const saved = fixture.Ok("scene.save", Json::object({{"path", path}}));
	CHECK(saved["scene"]["dirty"] == false);
	fixture.Ok("scene.settings", Json::object({{"name", "Renamed"}, {"settings", {{"Physics", {{"Gravity", {0, -1, 0}}}}}}}));
	Json const settings = fixture.Ok("scene.settings");
	CHECK(settings["name"] == "Renamed");
	CHECK(settings["settings"]["Physics"]["Gravity"] == Json::array({0.0, -1.0, 0.0}));
	CHECK(fixture.Fails("scene.settings", Json::object({{"settings", {{"Physics", {{"Gravity", "down"}}}}}})) ==
	      AutomationErrorCode::InvalidParams);

	fixture.Ok("scene.new", Json::object({{"name", "Fresh"}, {"discardChanges", true}}));
	CHECK(fixture.Context.GetScene().GetEntityCount() == 0);
	Json const opened = fixture.Ok("scene.open", Json::object({{"path", path}}));
	CHECK(opened["scene"]["entityCount"] == 1);
	CHECK(fixture.Fails("scene.open", Json::object({{"path", path + ".missing"}})) == AutomationErrorCode::FileError);
	CHECK(fixture.Ok("scene.dump")["Entities"].size() == 1);

	fixture.Ok("editor.quit");
	CHECK(fixture.QuitRequested);
}

TEST_CASE("EditorCommands: scene paths are absolute or relative to the asset directory")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory directory;
	CommandFixture fixture;
	// Without a project, relative paths have nothing to be relative to.
	fixture.Create("Hero");
	CHECK(fixture.Fails("scene.save", Json::object({{"path", "Scenes/Level.sscene"}})) == AutomationErrorCode::InvalidParams);

	REQUIRE(fixture.Operations.CreateProject(directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	std::filesystem::path const assetDirectory = fixture.Context.GetProject()->GetAssetDirectory();
	fixture.Create("Hero");
	fixture.Ok("scene.save", Json::object({{"path", "Scenes/Level.sscene"}}));
	CHECK(FileSystem::Exists(assetDirectory / "Scenes" / "Level.sscene"));
	CHECK(AssetManager::FindByPath("Scenes/Level.sscene").IsValid());

	// Relative paths stay in the asset directory; absolute ones may point anywhere.
	for (char const* const path : {"../Level.sscene", "Scenes/../../Level.sscene", "C:Level.sscene"})
	{
		CAPTURE(path);
		CHECK(fixture.Fails("scene.save", Json::object({{"path", path}})) == AutomationErrorCode::InvalidParams);
		CHECK(fixture.Fails("scene.open", Json::object({{"path", path}})) == AutomationErrorCode::InvalidParams);
	}
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "Game" / "Level.sscene"));
	fixture.Ok("scene.save", Json::object({{"path", FileSystem::PathToUtf8(directory.GetPath() / "Elsewhere.sscene")}}));
	CHECK(FileSystem::Exists(directory.GetPath() / "Elsewhere.sscene"));

	fixture.Ok("scene.new", Json::object({{"discardChanges", true}}));
	CHECK(fixture.Ok("scene.open", Json::object({{"path", "Scenes/Level.sscene"}}))["scene"]["entityCount"] == 1);
}

TEST_CASE("EditorCommands: selection, component types and log")
{
	CommandFixture fixture;
	std::string const a = fixture.Create("A");
	std::string const b = fixture.Create("B");
	Json const selection = fixture.Ok("entity.select", Json::object({{"entities", {a, b}}, {"primary", a}}));
	CHECK(selection["entities"].size() == 2);
	CHECK(selection["primary"] == a);
	CHECK(fixture.Ok("entity.select", Json::object({{"entities", {a}}, {"mode", "remove"}}))["primary"] == b);

	Json const types = fixture.Ok("component.types")["components"];
	bool hasTransform = false;
	for (Json const& type : types)
	{
		CAPTURE(type["Name"]);
		CHECK(type["Name"] != "Relationship");
		hasTransform |= type["Name"] == "Transform";
		// Every component names its category; core components are in the Core one.
		REQUIRE(type["Category"].is_string());
		std::string const category = type["Category"].get<std::string>();
		CHECK(std::find(ComponentCategory::All.begin(), ComponentCategory::All.end(), category) != ComponentCategory::All.end());
		if (type["Core"].get<bool>())
		{
			CHECK(category == ComponentCategory::Core);
		}
		if (type["Name"] == "PointLight")
		{
			CHECK(category == "Rendering");
		}
		if (type["Name"] == "BoxCollider")
		{
			CHECK(category == "Physics");
		}
	}
	CHECK(hasTransform);

	uint64_t const since = Log::GetNextEntryIndex();
	ST_CORE_WARN("automation log test message");
	ST_CORE_INFO("automation log info message");
	Json const log = fixture.Ok("log.read", Json::object({{"since", since}, {"minLevel", "warn"}}));
	REQUIRE(log["entries"].size() == 1);
	CHECK(log["entries"][0]["message"] == "automation log test message");
	CHECK(log["entries"][0]["level"] == "warn");
	CHECK(log["next"].get<uint64_t>() >= since + 2);
	CHECK(fixture.Ok("log.read", Json::object({{"since", log["next"]}}))["entries"].empty());
}

TEST_CASE("EditorCommands: screenshots complete asynchronously with PNG data")
{
	CommandFixture fixture(true);
	std::optional<CommandResult> result;
	fixture.Registry.Execute("viewport.screenshot", Json::object(),
	                         [&result](CommandResult value)
	                         {
								 result.emplace(std::move(value));
							 });
	CHECK_FALSE(result);
	REQUIRE(fixture.PendingScreenshot);

	Image image(3, 2, 4);
	image.GetPixel(2, 1)[0] = 200;
	(*fixture.PendingScreenshot)(image);
	REQUIRE(result);
	REQUIRE(result->IsOk());
	Json const& shot = result->GetValue();
	CHECK(shot["mimeType"] == "image/png");
	CHECK(shot["width"] == 3);
	Result<std::vector<uint8_t>> png = Base64Decode(shot["data"].get<std::string>());
	REQUIRE(png.IsOk());
	Result<Image> decoded = Image::LoadFromMemory(png.GetValue());
	REQUIRE(decoded.IsOk());
	CHECK(decoded.GetValue().GetPixel(2, 1)[0] == 200);

	result.reset();
	fixture.Registry.Execute("viewport.screenshot", Json::object(),
	                         [&result](CommandResult value)
	                         {
								 result.emplace(std::move(value));
							 });
	(*fixture.PendingScreenshot)(Error{"minimized"});
	REQUIRE(result);
	CHECK(result->GetError().Code == AutomationErrorCode::Unavailable);
}

TEST_CASE("EditorCommands: projects are created, configured, closed and opened")
{
	AssetManager::Init();
	struct AssetManagerShutdown
	{
		~AssetManagerShutdown() { AssetManager::Shutdown(); }
	} const shutdown;
	Testing::TemporaryDirectory directory;
	CommandFixture fixture;
	std::string const projectDirectory = FileSystem::PathToUtf8(directory.GetPath() / "Game");

	Json const none = fixture.Ok("project.info");
	CHECK(none["project"].is_null());
	CHECK(none["settings"].is_null());
	CHECK(fixture.Fails("project.settings") == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("project.close") == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("project.create", Json::object({{"directory", projectDirectory}})) == AutomationErrorCode::InvalidParams);

	// The default editor scene is unsaved but unchanged; a modified one needs discardChanges.
	fixture.Create("Unsaved");
	CHECK(fixture.Fails("project.create", Json::object({{"directory", projectDirectory}, {"name", "Game"}})) ==
	      AutomationErrorCode::UnsavedChanges);
	Json const created =
		fixture.Ok("project.create", Json::object({{"directory", projectDirectory}, {"name", "Game"}, {"discardChanges", true}}));
	CHECK(created["project"]["name"] == "Game");
	CHECK(created["scene"]["path"].get<std::string>().ends_with("Main.sscene"));
	CHECK(fixture.ProjectsOpened == 1);
	CHECK(fixture.Ok("editor.status")["project"]["name"] == "Game");
	// The directory is no longer empty.
	CHECK(fixture.Fails("project.create", Json::object({{"directory", projectDirectory}, {"name", "Other"}})) ==
	      AutomationErrorCode::FileError);

	Json const settings = fixture.Ok("project.settings", Json::object({{"settings", {{"Window", {{"Width", 1600}}}}}}));
	CHECK(settings["settings"]["Window"]["Width"] == 1600);
	CHECK(settings["settings"]["Window"]["Height"] == 720);
	CHECK(fixture.Fails("project.settings", Json::object({{"settings", {{"Physics", {{"Layers", Json::array()}}}}}})) ==
	      AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("project.settings", Json::object({{"settings", {{"Typo", 1}}}})) == AutomationErrorCode::InvalidParams);
	std::string const projectFile = created["project"]["file"].get<std::string>();
	Result<std::string> const written = FileSystem::ReadTextFile(FileSystem::PathFromUtf8(projectFile));
	REQUIRE(written.IsOk());
	CHECK(ParseJson(written.GetValue()).GetValue()["Project"]["Window"]["Width"] == 1600);

	fixture.Create("Unsaved");
	CHECK(fixture.Fails("project.close") == AutomationErrorCode::UnsavedChanges);
	Json const closed = fixture.Ok("project.close", Json::object({{"discardChanges", true}}));
	CHECK(closed["closed"] == true);
	CHECK(fixture.Ok("project.info")["project"].is_null());

	Json const opened = fixture.Ok("project.open", Json::object({{"path", projectFile}}));
	CHECK(opened["project"]["name"] == "Game");
	CHECK(opened["warnings"].empty());
	CHECK(opened["scene"]["entityCount"].get<size_t>() > 0);
	CHECK(fixture.ProjectsOpened == 2);
	CHECK(fixture.Ok("project.info")["settings"]["Window"]["Width"] == 1600);
	CHECK(fixture.Fails("project.open", Json::object({{"path", projectFile + ".missing"}})) == AutomationErrorCode::FileError);
	CHECK(fixture.Ok("project.info")["project"]["name"] == "Game");
}

TEST_CASE("EditorCommands: assets and materials are listed, imported, edited, moved and deleted")
{
	AssetManager::Init();
	struct AssetManagerShutdown
	{
		~AssetManagerShutdown() { AssetManager::Shutdown(); }
	} const shutdown;
	Testing::TemporaryDirectory directory;
	CommandFixture fixture;

	// Built-in assets are available without a project; file operations are not.
	Json const builtIn = fixture.Ok("asset.list", Json::object({{"builtIn", true}, {"type", "Mesh"}}));
	CHECK(builtIn["assets"].size() == 7);
	CHECK(fixture.Ok("asset.get", Json::object({{"asset", "builtin://Cube"}}))["type"] == "Mesh");
	CHECK(fixture.Fails("asset.get", Json::object({{"asset", "builtin://Teapot"}})) == AutomationErrorCode::AssetNotFound);
	CHECK(fixture.Fails("asset.create-folder", Json::object({{"folder", "Textures"}})) == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("material.set", Json::object({{"material", "builtin://DefaultMaterial"}, {"fields", {{"Roughness", 0.1}}}})) ==
	      AutomationErrorCode::InvalidOperation);

	fixture.Ok("project.create", Json::object({{"directory", FileSystem::PathToUtf8(directory.GetPath() / "Game")}, {"name", "Game"}}));
	fixture.Ok("asset.create-folder", Json::object({{"folder", "Textures/Wood"}}));
	CHECK(fixture.Fails("asset.create-folder", Json::object({{"folder", "Textures/Wood"}})) == AutomationErrorCode::FileError);

	Image image(2, 2, 4);
	std::filesystem::path const download = directory.GetPath() / "Download" / "Oak.png";
	REQUIRE(image.WritePNG(download).IsOk());
	Json const imported =
		fixture.Ok("asset.import", Json::object({{"files", {FileSystem::PathToUtf8(download)}}, {"folder", "Textures/Wood"}}));
	REQUIRE(imported["assets"].size() == 1);
	CHECK(imported["assets"][0]["path"] == "Textures/Wood/Oak.png");
	CHECK(imported["assets"][0]["reference"] == "asset://Textures/Wood/Oak.png");
	std::string const texture = imported["assets"][0]["id"].get<std::string>();

	Json const listed = fixture.Ok("asset.list", Json::object({{"folder", "Textures"}}));
	CHECK(listed["assets"].size() == 1);
	CHECK(listed["folders"] == Json::array({"Textures/Wood"}));
	CHECK(fixture.Ok("asset.list", Json::object({{"folder", "Textures"}, {"recursive", false}}))["assets"].empty());
	CHECK(fixture.Ok("asset.list", Json::object({{"type", "Scene"}}))["assets"].size() == 1);

	Json const material =
		fixture.Ok("material.create",
	               Json::object({{"path", "Materials/Oak.smat"}, {"fields", {{"BaseColorTexture", "asset://Textures/Wood/Oak.png"}}}}));
	CHECK(material["fields"]["BaseColorTexture"] == texture);
	std::string const materialID = material["material"]["id"].get<std::string>();
	CHECK(fixture.Fails("material.create", Json::object({{"path", "Materials/Bad.smat"}, {"fields", {{"Metallic", 5}}}})) ==
	      AutomationErrorCode::InvalidParams);

	Json const edited = fixture.Ok("material.set", Json::object({{"material", materialID}, {"fields", {{"Roughness", 0.25}}}}));
	CHECK(edited["fields"]["Roughness"] == 0.25f);
	CHECK(fixture.Ok("editor.status")["scene"]["dirty"] == false);
	CHECK(fixture.Ok("editor.undo")["undone"] == "Edit Material");
	CHECK(fixture.Ok("material.get", Json::object({{"material", "asset://Materials/Oak.smat"}}))["fields"]["Roughness"] == 0.5f);
	CHECK(fixture.Fails("material.set", Json::object({{"material", materialID}, {"fields", {{"Roughness", "smooth"}}}})) ==
	      AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("material.get", Json::object({{"material", "asset://Materials/Missing.smat"}})) ==
	      AutomationErrorCode::AssetNotFound);

	// Moving keeps references: the material still points at the texture.
	CHECK(fixture.Ok("asset.move", Json::object({{"asset", texture}, {"path", "Textures/Oak.png"}}))["path"] == "Textures/Oak.png");
	fixture.Ok("asset.move-folder", Json::object({{"folder", "Materials"}, {"newFolder", "Art/Materials"}}));
	CHECK(fixture.Ok("material.get", Json::object({{"material", materialID}}))["fields"]["BaseColorTexture"] == texture);

	CHECK(fixture.Ok("asset.refresh")["added"].empty());

	// The inspector shows either selected entities or one asset; deleting the asset deselects it.
	std::string const entity = fixture.Create("Entity");
	fixture.Ok("entity.select", Json::object({{"entities", {entity}}}));
	Json const assetSelection = fixture.Ok("asset.select", Json::object({{"asset", texture}}));
	CHECK(assetSelection["entities"].empty());
	CHECK(assetSelection["asset"]["id"] == texture);
	CHECK(fixture.Ok("editor.status")["selection"]["asset"]["path"] == "Textures/Oak.png");
	CHECK(fixture.Ok("entity.select", Json::object({{"entities", Json::array()}}))["asset"].is_null());
	CHECK(fixture.Fails("asset.select", Json::object({{"asset", "asset://Textures/None.png"}})) == AutomationErrorCode::AssetNotFound);
	fixture.Ok("asset.select", Json::object({{"asset", texture}}));

	CHECK(fixture.Ok("asset.delete", Json::object({{"asset", texture}}))["deleted"] == texture);
	CHECK(fixture.Ok("editor.status")["selection"]["asset"].is_null());
	CHECK_FALSE(fixture.Context.GetSelectedAsset().IsValid());
	CHECK(fixture.Fails("asset.delete", Json::object({{"asset", texture}})) == AutomationErrorCode::AssetNotFound);
	fixture.Ok("asset.delete-folder", Json::object({{"folder", "Art"}}));
	CHECK(fixture.Fails("material.get", Json::object({{"material", materialID}})) == AutomationErrorCode::AssetNotFound);
	CHECK(fixture.Fails("asset.delete-folder", Json::object({{"folder", "Art"}})) == AutomationErrorCode::FileError);
}

TEST_CASE("EditorCommands: the viewport camera is read, set and pointed at entities")
{
	CommandFixture fixture(true);
	auto const vector = [](Json const& value)
	{
		return glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
	};
	auto const nearlyEqual = [](glm::vec3 const& a, glm::vec3 const& b)
	{
		return glm::length(a - b) < 1e-4f;
	};

	Json const view = fixture.Ok("viewport.camera");
	CHECK(nearlyEqual(vector(view["focalPoint"]), fixture.Camera.GetFocalPoint()));
	CHECK(view["distance"].get<float>() == fixture.Camera.GetDistance());

	// Looking down from the side: forward follows yaw and pitch, and the camera sits behind the focal point.
	Json const set =
		fixture.Ok("viewport.camera", Json::object({{"focalPoint", {1, 2, 3}}, {"distance", 10}, {"yaw", 90}, {"pitch", -30}}));
	CHECK(nearlyEqual(vector(set["focalPoint"]), {1.0f, 2.0f, 3.0f}));
	CHECK(set["distance"].get<float>() == 10.0f);
	CHECK(set["yaw"].get<float>() == 90.0f);
	CHECK(set["pitch"].get<float>() == -30.0f);
	glm::vec3 const forward = vector(set["forward"]);
	CHECK(nearlyEqual(forward, {-0.8660254f, -0.5f, 0.0f}));
	CHECK(nearlyEqual(vector(set["position"]), glm::vec3(1.0f, 2.0f, 3.0f) - forward * 10.0f));
	// Values left out stay.
	Json const closer = fixture.Ok("viewport.camera", Json::object({{"distance", 4}}));
	CHECK(closer["distance"].get<float>() == 4.0f);
	CHECK(closer["yaw"].get<float>() == 90.0f);
	CHECK(nearlyEqual(vector(closer["focalPoint"]), {1.0f, 2.0f, 3.0f}));
	CHECK(fixture.Fails("viewport.camera", Json::object({{"pitch", 95}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("viewport.camera", Json::object({{"distance", 0}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("viewport.camera", Json::object({{"focalPoint", {1, 2}}})) == AutomationErrorCode::InvalidParams);

	// Framing centers the entities and backs off to see them all.
	std::string const left = fixture.Create("Left", Json::object({{"components", {{"Transform", {{"Translation", {-2, 0, 0}}}}}}}));
	std::string const right = fixture.Create("Right", Json::object({{"components", {{"Transform", {{"Translation", {6, 0, 0}}}}}}}));
	Json const one = fixture.Ok("viewport.frame", Json::object({{"entities", {left}}}));
	CHECK(nearlyEqual(vector(one["focalPoint"]), {-2.0f, 0.0f, 0.0f}));
	Json const both = fixture.Ok("viewport.frame", Json::object({{"entities", {left, right}}}));
	CHECK(nearlyEqual(vector(both["focalPoint"]), {2.0f, 0.0f, 0.0f}));
	CHECK(both["distance"].get<float>() > one["distance"].get<float>());
	// Framing keeps the direction.
	CHECK(both["yaw"].get<float>() == 90.0f);
	CHECK(fixture.Fails("viewport.frame", Json::object({{"entities", {"123"}}})) == AutomationErrorCode::EntityNotFound);
	CHECK(fixture.Fails("viewport.frame", Json::object({{"entities", Json::array()}})) == AutomationErrorCode::InvalidParams);
}

TEST_CASE("EditorCommands: asset.browse shows folders of the asset directory in the content browser")
{
	AssetManager::Init();
	struct AssetManagerShutdown
	{
		~AssetManagerShutdown() { AssetManager::Shutdown(); }
	} const shutdown;
	Testing::TemporaryDirectory directory;
	CommandFixture fixture(true);
	// Without a project there are no folders to show.
	CHECK(fixture.Fails("asset.browse", Json::object({{"folder", ""}})) == AutomationErrorCode::InvalidOperation);

	fixture.Ok("project.create", Json::object({{"directory", FileSystem::PathToUtf8(directory.GetPath() / "Game")}, {"name", "Game"}}));
	fixture.Ok("asset.create-folder", Json::object({{"folder", "Textures/Wood"}}));
	Json const shown = fixture.Ok("asset.browse", Json::object({{"folder", "Textures/Wood/"}, {"tileScale", 1.5}}));
	CHECK(shown["folder"] == "Textures/Wood");
	CHECK(shown["tileScale"].get<float>() == 1.5f);
	CHECK(fixture.ContentBrowser.GetFolder() == "Textures/Wood");
	CHECK(fixture.ContentBrowser.GetTileScale() == 1.5f);
	// Equivalent spellings of Assets itself; the scale stays when not given.
	CHECK(fixture.Ok("asset.browse", Json::object({{"folder", "./Textures/.."}}))["folder"] == "");
	CHECK(fixture.Ok("asset.browse", Json::object({{"folder", "."}}))["tileScale"].get<float>() == 1.5f);
	CHECK(fixture.Ok("asset.browse")["folder"] == "");

	// Only existing folders inside the asset directory.
	for (char const* folder : {"Missing", "..", "../Game", "Textures/../../Other"})
	{
		CAPTURE(folder);
		CHECK(fixture.Fails("asset.browse", Json::object({{"folder", folder}})) == AutomationErrorCode::InvalidParams);
	}
	CHECK(fixture.Fails("asset.browse", Json::object({{"folder", FileSystem::PathToUtf8(directory.GetPath())}})) ==
	      AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("asset.browse", Json::object({{"tileScale", 10.0}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.ContentBrowser.GetFolder().empty());
}

TEST_CASE("EditorCommands: the viewport's renderer statistics are reported")
{
	CommandFixture fixture(true);
	// A viewport without a GPU renderer.
	CHECK(fixture.Fails("viewport.statistics") == AutomationErrorCode::Unavailable);

	SceneRendererStatistics statistics;
	statistics.DrawCalls = 12;
	statistics.Triangles = 3400;
	statistics.Lights = 3;
	statistics.ShadowDrawCalls = 7;
	statistics.ShadowMapViews = 5;
	statistics.ShadowsDropped = 1;
	statistics.Quads = 9;
	statistics.Culled = 4;
	statistics.CulledLights = 2;
	fixture.Statistics = statistics;
	Json const reported = fixture.Ok("viewport.statistics");
	// No frame has finished (or the GPU cannot measure time).
	CHECK(reported["gpuMilliseconds"].is_null());
	fixture.Statistics->GpuMilliseconds = 1.5f;
	CHECK(fixture.Ok("viewport.statistics") == Json::object({{"gpuMilliseconds", 1.5f},
	                                                         {"drawCalls", 12},
	                                                         {"triangles", 3400},
	                                                         {"shadowDrawCalls", 7},
	                                                         {"shadowMapViews", 5},
	                                                         {"shadowsDropped", 1},
	                                                         {"lights", 3},
	                                                         {"culledLights", 2},
	                                                         {"culled", 4},
	                                                         {"quads", 9}}));
}

TEST_CASE("EditorCommands: the viewport's overlays are read and set")
{
	CommandFixture fixture(true);
	Json const defaults = fixture.Ok("viewport.overlays");
	CHECK(defaults == Json::object({{"grid", true},
	                                {"selectionOutline", true},
	                                {"icons", true},
	                                {"cameras", "selected"},
	                                {"lights", "selected"},
	                                {"colliders", "selected"},
	                                {"joints", "selected"},
	                                {"volumes", "selected"},
	                                {"debugDrawing", true},
	                                {"canvases", true}}));
	CHECK(fixture.Overlays == SceneViewOverlays());

	// Only the given overlays change.
	Json const changed = fixture.Ok("viewport.overlays", Json::object({{"grid", false},
	                                                                   {"icons", false},
	                                                                   {"lights", "all"},
	                                                                   {"colliders", "off"},
	                                                                   {"volumes", "all"},
	                                                                   {"debugDrawing", false},
	                                                                   {"canvases", false}}));
	CHECK(changed["grid"] == false);
	CHECK(changed["selectionOutline"] == true);
	CHECK(changed["lights"] == "all");
	CHECK(changed["colliders"] == "off");
	CHECK(changed["volumes"] == "all");
	CHECK(changed["cameras"] == "selected");
	CHECK(changed["canvases"] == false);
	CHECK_FALSE(fixture.Overlays.Grid);
	CHECK_FALSE(fixture.Overlays.Icons);
	CHECK_FALSE(fixture.Overlays.DebugLines);
	CHECK_FALSE(fixture.Overlays.Canvases);
	CHECK(fixture.Overlays.Lights == OverlayShapeSetting{true, true});
	CHECK_FALSE(fixture.Overlays.Colliders.Enabled);
	CHECK(fixture.Overlays.Volumes == OverlayShapeSetting{true, true});

	// Turning shapes off keeps the choice of entities for when they are on again.
	CHECK(fixture.Ok("viewport.overlays", Json::object({{"lights", "off"}}))["lights"] == "off");
	CHECK(fixture.Overlays.Lights == OverlayShapeSetting{false, true});
	CHECK(fixture.Ok("viewport.overlays", Json::object({{"lights", "selected"}}))["lights"] == "selected");
	CHECK(fixture.Overlays.Lights == OverlayShapeSetting{true, false});

	// Invalid values change nothing.
	SceneViewOverlays const before = fixture.Overlays;
	CHECK(fixture.Fails("viewport.overlays", Json::object({{"joints", "some"}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("viewport.overlays", Json::object({{"grid", "yes"}, {"icons", true}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("viewport.overlays", Json::object({{"canvases", 1}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Overlays == before);
	// The scene and its history are untouched.
	CHECK(fixture.Context.GetHistory().GetUndoCount() == 0);
	CHECK_FALSE(fixture.Context.IsDirty());
}
