#include "TestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/EditorCamera.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/Base64.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Log.h"
#include "Strada/Renderer/SceneRenderer.h"

#include <doctest/doctest.h>

#include <optional>

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
		std::optional<SceneRendererStatistics> Statistics;

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
	     {"editor.status", "editor.commands",     "editor.undo",     "editor.redo",      "editor.quit",        "scene.new",
	      "scene.open",    "scene.save",          "scene.hierarchy", "scene.dump",       "scene.settings",     "entity.create",
	      "entity.delete", "entity.duplicate",    "entity.rename",   "entity.reparent",  "entity.find",        "entity.get",
	      "entity.select", "component.types",     "component.add",   "component.remove", "component.get",      "component.set",
	      "log.read",      "viewport.screenshot", "viewport.camera", "viewport.frame",   "viewport.statistics"})
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
		CHECK(type["Name"] != "Relationship");
		hasTransform |= type["Name"] == "Transform";
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
	CHECK(fixture.Ok("viewport.statistics") == Json::object({{"drawCalls", 12},
	                                                         {"triangles", 3400},
	                                                         {"shadowDrawCalls", 7},
	                                                         {"shadowMapViews", 5},
	                                                         {"shadowsDropped", 1},
	                                                         {"lights", 3},
	                                                         {"culledLights", 2},
	                                                         {"culled", 4},
	                                                         {"quads", 9}}));
}
