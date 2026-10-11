#include "Audio/AudioTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/EditorExport.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/Panels/BuildGameDialog.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/Panels/ProjectSettingsPanel.h"
#include "Editor/Panels/SceneHierarchyPanel.h"
#include "Editor/Panels/SceneSettingsPanel.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Core/EmbeddedResources.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Utf8.h"
#include "Strada/Project/ScriptProject.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_impl_null.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	// A Dear ImGui context with the null platform and renderer backends: panels draw without a window or GPU, and tests
	// feed keyboard input between frames.
	class HeadlessImGui
	{
	public:
		HeadlessImGui()
		{
			m_Context = ImGui::CreateContext();
			ImGui::GetIO().IniFilename = nullptr;
			ImGui_ImplNull_Init();
		}

		~HeadlessImGui()
		{
			ImGui_ImplNull_Shutdown();
			ImGui::DestroyContext(m_Context);
		}

		HeadlessImGui(HeadlessImGui const&) = delete;
		HeadlessImGui& operator=(HeadlessImGui const&) = delete;

		void Frame(std::function<void()> const& draw)
		{
			ImGui_ImplNull_NewFrame();
			ImGui::NewFrame();
			draw();
			ImGui::Render();
			ImGui_ImplNullRender_RenderDrawData(ImGui::GetDrawData());
		}

		void Type(char const* text) { ImGui::GetIO().AddInputCharactersUTF8(text); }

		// Drags with the left mouse button from one point to another (screen coordinates), a frame per step, and releases.
		void Drag(ImVec2 from, ImVec2 to, std::function<void()> const& draw)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddMousePosEvent(from.x, from.y);
			Frame(draw);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			Frame(draw);
			// Past the drag threshold, then onto the target long enough for it to accept the payload.
			io.AddMousePosEvent(from.x + 20.0f, from.y + 20.0f);
			Frame(draw);
			io.AddMousePosEvent(to.x, to.y);
			Frame(draw);
			Frame(draw);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			Frame(draw);
			Frame(draw);
		}

		void Press(ImGuiKey key)
		{
			ImGui::GetIO().AddKeyEvent(key, true);
			Frame([] {});
			ImGui::GetIO().AddKeyEvent(key, false);
		}

		// Presses and releases a key while the panels are drawn.
		void Press(ImGuiKey key, std::function<void()> const& draw)
		{
			ImGui::GetIO().AddKeyEvent(key, true);
			Frame(draw);
			ImGui::GetIO().AddKeyEvent(key, false);
			Frame(draw);
		}

		// Left clicks (one or two times) at a screen position.
		void Click(ImVec2 position, std::function<void()> const& draw, int count = 1)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddMousePosEvent(position.x, position.y);
			Frame(draw);
			for (int i = 0; i < count; i++)
			{
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				Frame(draw);
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				Frame(draw);
			}
		}

	private:
		ImGuiContext* m_Context = nullptr;
	};

	struct Probe
	{
		float Gain = 1.0f;
		std::string Label = "Probe";
		uint32_t Count = 2;
	};
}

template<>
struct Strada::StructTraits<Probe>
{
	static constexpr std::string_view Name = "Probe";
	static constexpr auto Fields = std::make_tuple(Field("Gain", &Probe::Gain).Range(0.0, 10.0), Field("Label", &Probe::Label),
	                                               Field("Count", &Probe::Count).Range(1.0, 4.0));
};

namespace
{
	// Draws one field of Probe in a window with keyboard focus on its widget, then types text and presses Enter; returns
	// the last change the editor reported.
	std::optional<FieldChange> TypeIntoField(HeadlessImGui& imgui, size_t fieldIndex, char const* text)
	{
		std::vector<FieldDescriptor> const fields = GetFieldDescriptors<StructTraits<Probe>, Probe>();
		std::span<FieldDescriptor const> const field(&fields[fieldIndex], 1);
		Json const values = SerializeFields<StructTraits<Probe>>(Probe{});
		FieldEditor editor;
		std::optional<FieldChange> last;
		bool focus = true;
		auto const draw = [&]
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
			ImGui::Begin("Fields");
			if (focus)
			{
				ImGui::SetKeyboardFocusHere();
				focus = false;
			}
			if (std::optional<FieldChange> change = editor.Draw(field, values))
			{
				last = std::move(change);
			}
			ImGui::End();
		};

		imgui.Frame(draw);
		imgui.Frame(draw);
		imgui.Type(text);
		imgui.Frame(draw);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
		imgui.Frame(draw);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
		imgui.Frame(draw);
		return last;
	}

	UUID Create(EditorOperations& operations, std::string name, Json components)
	{
		EntityCreateInfo info;
		info.Name = std::move(name);
		info.Components = std::move(components);
		Result<UUID> entity = operations.CreateEntity(info);
		REQUIRE(entity.IsOk());
		return entity.GetValue();
	}
}

TEST_CASE("FieldEditor: typed values are reported with their path and clamped to the field's range")
{
	HeadlessImGui imgui;

	std::optional<FieldChange> const gain = TypeIntoField(imgui, 0, "2.5");
	REQUIRE(gain);
	CHECK(gain->Path == std::vector<std::string_view>{"Gain"});
	CHECK(gain->Value.get<float>() == doctest::Approx(2.5f));

	std::optional<FieldChange> const clamped = TypeIntoField(imgui, 0, "42");
	REQUIRE(clamped);
	CHECK(clamped->Value.get<float>() == doctest::Approx(10.0f));

	std::optional<FieldChange> const count = TypeIntoField(imgui, 2, "9");
	REQUIRE(count);
	CHECK(count->Value.is_number_unsigned());
	CHECK(count->Value.get<uint64_t>() == 4u);

	std::optional<FieldChange> const label = TypeIntoField(imgui, 1, "Lamp");
	REQUIRE(label);
	CHECK(label->Path == std::vector<std::string_view>{"Label"});
	CHECK(label->Value.get<std::string>().ends_with("Lamp"));
}

TEST_CASE("FieldEditor: drawing the fields of every component and setting changes nothing")
{
	HeadlessImGui imgui;
	FieldEditor editor;
	std::vector<FieldDescriptor> const settings = GetFieldDescriptors<StructTraits<SceneSettings>, SceneSettings>();
	Json const settingsValues = SceneSerializer::SerializeSettings(SceneSettings());
	for (int frame = 0; frame < 3; frame++)
	{
		imgui.Frame(
			[&]
			{
				ImGui::Begin("Components");
				for (ComponentInfo const& component : ComponentRegistry::GetComponents())
				{
					ImGui::PushID(component.Name.data(), component.Name.data() + component.Name.size());
					Json defaults = Json::object();
					for (FieldDescriptor const& field : component.Fields)
					{
						defaults[std::string(field.Name)] = field.Default;
					}
					std::vector<std::string> const mixed = {std::string(component.Fields.front().Name)};
					CHECK_FALSE(editor.Draw(component.Fields, defaults, mixed));
					ImGui::PopID();
				}
				CHECK_FALSE(editor.Draw(settings, settingsValues));
				ImGui::End();
			});
	}
}

TEST_CASE("Panels: the hierarchy, inspector and scene settings draw every component without modifying the scene")
{
	HeadlessImGui imgui;
	EditorContext context;
	EditorOperations operations(context);

	// One entity with every component users can add, a child, and a second entity sharing some components with
	// different values (mixed values in the inspector).
	Json everything = Json::object();
	for (ComponentInfo const& component : ComponentRegistry::GetComponents())
	{
		if (!component.IsInternal())
		{
			everything[std::string(component.Name)] = Json::object();
		}
	}
	everything["Script"] = Json::object({{"ClassName", "Game.Player"}});
	UUID const full = Create(operations, "Everything", everything);
	EntityCreateInfo childInfo;
	childInfo.Name = "Child";
	childInfo.Parent = full;
	REQUIRE(operations.CreateEntity(childInfo).IsOk());
	UUID const other = Create(operations, "Other", Json::object({{"PointLight", {{"Range", 3.0}}}, {"Mesh", Json::object()}}));
	size_t const steps = context.GetHistory().GetUndoCount();
	Json const before = SceneSerializer::Serialize(context.GetScene());

	SceneHierarchyPanel hierarchy;
	InspectorPanel inspector;
	SceneSettingsPanel settings;
	bool hierarchyOpen = true;
	bool inspectorOpen = true;
	bool settingsOpen = true;
	auto const drawPanels = [&]
	{
		hierarchy.OnImGuiRender(operations, hierarchyOpen);
		inspector.OnImGuiRender(operations, inspectorOpen);
		settings.OnImGuiRender(operations, settingsOpen);
	};

	std::vector<std::vector<UUID>> const selections = {{}, {full}, {full, other}, {other, full}};
	for (std::vector<UUID> const& selection : selections)
	{
		REQUIRE(operations.Select(selection).IsOk());
		for (int frame = 0; frame < 3; frame++)
		{
			imgui.Frame(drawPanels);
		}
	}

	CHECK(hierarchyOpen);
	CHECK(inspectorOpen);
	CHECK(settingsOpen);
	CHECK(context.GetHistory().GetUndoCount() == steps);
	CHECK(SceneSerializer::Serialize(context.GetScene()) == before);
}

TEST_CASE("Panels: the inspector shows the fields of loaded script classes without changing them")
{
	Testing::ScriptEngineScope scripting;
	HeadlessImGui imgui;
	EditorContext context;
	EditorOperations operations(context);
	// Fields of every kind (FieldTypes), shared and differing classes, and a class the assembly does not have.
	Json const stored = Json::object({{"Count", Json::object({{"Type", "Int32"}, {"Value", 12}})}});
	UUID const first = Create(operations, "First",
	                          Json::object({{"Script", Json::object({{"ClassName", "Strada.Tests.FieldTypes"}, {"Fields", stored}})}}));
	UUID const second = Create(operations, "Second", Json::object({{"Script", Json::object({{"ClassName", "Strada.Tests.FieldTypes"}})}}));
	UUID const mover = Create(operations, "Mover", Json::object({{"Script", Json::object({{"ClassName", "Strada.Tests.Mover"}})}}));
	UUID const missing = Create(operations, "Missing", Json::object({{"Script", Json::object({{"ClassName", "Game.Gone"}})}}));
	size_t const steps = context.GetHistory().GetUndoCount();
	Json const before = SceneSerializer::Serialize(context.GetScene());

	InspectorPanel inspector;
	bool open = true;
	std::vector<std::vector<UUID>> const selections = {{first}, {first, second}, {first, mover}, {missing}};
	for (std::vector<UUID> const& selection : selections)
	{
		REQUIRE(operations.Select(selection).IsOk());
		for (int frame = 0; frame < 3; frame++)
		{
			imgui.Frame(
				[&]
				{
					inspector.OnImGuiRender(operations, open);
				});
		}
	}
	CHECK(open);
	CHECK(context.GetHistory().GetUndoCount() == steps);
	CHECK(SceneSerializer::Serialize(context.GetScene()) == before);
}

TEST_CASE("Panels: project settings draw with and without a project and do not change it by drawing")
{
	AssetManager::Init();
	struct AssetManagerShutdown
	{
		~AssetManagerShutdown() { AssetManager::Shutdown(); }
	} const shutdown;
	HeadlessImGui imgui;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	ProjectSettingsPanel panel;
	bool open = true;
	auto const draw = [&]
	{
		panel.OnImGuiRender(operations, open);
	};

	imgui.Frame(draw);
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	Json const layers = Json::object(
		{{"Physics", {{"Layers", {"Default", "Player", "Enemy"}}, {"IgnoredCollisions", Json::array({{{"First", 1}, {"Second", 2}}})}}}});
	REQUIRE(operations.ApplyProjectSettings(layers).IsOk());
	REQUIRE(operations.SaveProject().IsOk());
	ProjectSettings const before = context.GetProject()->GetSettings();
	Result<std::string> const fileBefore = FileSystem::ReadTextFile(context.GetProject()->GetFilePath());
	REQUIRE(fileBefore.IsOk());

	for (int frame = 0; frame < 3; frame++)
	{
		imgui.Frame(draw);
	}
	panel.Flush(operations);
	CHECK(open);
	CHECK(context.GetProject()->GetSettings() == before);
	CHECK(FileSystem::ReadTextFile(context.GetProject()->GetFilePath()).GetValue() == fileBefore.GetValue());
}

TEST_CASE("Panels: the Build Game dialog draws the game's settings without changing them")
{
	AssetManager::Init();
	struct AssetManagerShutdown
	{
		~AssetManagerShutdown() { AssetManager::Shutdown(); }
	} const shutdown;
	HeadlessImGui imgui;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll");
	EditorExport exporter(context, scripts);
	BuildGameDialog dialog;
	auto const draw = [&]
	{
		dialog.OnImGuiRender(operations, exporter);
	};

	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	ProjectSettings const before = context.GetProject()->GetSettings();
	Result<std::string> const fileBefore = FileSystem::ReadTextFile(context.GetProject()->GetFilePath());
	REQUIRE(fileBefore.IsOk());
	imgui.Frame(draw);
	dialog.Open(*context.GetProject());
	for (int frame = 0; frame < 3; frame++)
	{
		imgui.Frame(draw);
	}
	// The dialog is open (a modal popup) and nothing was exported or changed.
	CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
	CHECK_FALSE(exporter.IsRunning());
	dialog.Flush(operations);
	CHECK(context.GetProject()->GetSettings() == before);
	CHECK(FileSystem::ReadTextFile(context.GetProject()->GetFilePath()).GetValue() == fileBefore.GetValue());
}

TEST_CASE("Panels: the content browser and the asset inspector show a project's assets without changing them")
{
	AssetManager::Init();
	struct AssetManagerShutdown
	{
		~AssetManagerShutdown() { AssetManager::Shutdown(); }
	} const shutdown;
	HeadlessImGui imgui;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	ContentBrowserPanel browser;
	InspectorPanel inspector;
	bool browserOpen = true;
	bool inspectorOpen = true;
	auto const draw = [&]
	{
		browser.OnImGuiRender(operations, browserOpen);
		inspector.OnImGuiRender(operations, inspectorOpen);
	};
	auto const drawFrames = [&]
	{
		for (int frame = 0; frame < 3; frame++)
		{
			imgui.Frame(draw);
		}
	};

	// Without a project there is nothing to browse.
	drawFrames();

	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	std::filesystem::path const assets = context.GetProject()->GetAssetDirectory();
	REQUIRE(operations.CreateAssetFolder("Textures/Wood").IsOk());
	Image image(4, 2, 4);
	REQUIRE(image.WritePNG(assets / "Textures" / "Wood" / "Oak.png").IsOk());
	REQUIRE(operations.RefreshAssets().IsOk());
	AssetHandle const texture = AssetManager::FindByPath("Textures/Wood/Oak.png");
	REQUIRE(texture.IsValid());
	Result<AssetHandle> const material = operations.CreateMaterial("Rock.smat", Json::object({{"BaseColorTexture", texture.ToString()}}));
	REQUIRE(material.IsOk());
	// Tile labels wrap long names and cut multi-byte UTF-8 names between code points ("Ölfarbe" and "Größe").
	REQUIRE(operations.CreateMaterial("A Very Long Material Name That Needs Two Lines.smat").IsOk());
	REQUIRE(operations
	            .CreateMaterial("\xC3\x96lfarbe\xC3\x96lfarbe\xC3\x96lfarbe Gr\xC3\xB6\xC3\x9F"
	                            "e.smat")
	            .IsOk());
	REQUIRE(operations.CreateMaterial("NoSpacesInThisRatherLongMaterialFileName.smat").IsOk());
	REQUIRE(FileSystem::WriteTextFile(assets / "Ghost.smat", "{}").IsOk());
	// Every kind of tile: audio (a waveform), a font, a scene, a prefab and files that fail to load (their type icons), and
	// C# scripts under the Scripts root.
	std::vector<float> const samples = Testing::MakeSamples({{0.5f, 0.2f}, {0.1f, 0.2f}});
	REQUIRE(FileSystem::WriteBinaryFile(assets / "Audio" / "Hit.wav", Testing::MakeWav(samples).GetSpan()).IsOk());
	std::span<uint8_t const> const font = EmbeddedResources::Get(EmbeddedResources::DefaultFont);
	REQUIRE(FileSystem::WriteBinaryFile(assets / "Fonts" / "Title.ttf", font).IsOk());
	for (char const* broken : {"Broken/Broken.gltf", "Broken/Broken.hdr", "Broken/Broken.sprefab", "Broken/Broken.sscene",
	                           "Broken/Broken.ogg", "Broken/Notes.txt"})
	{
		REQUIRE(FileSystem::WriteTextFile(assets / FileSystem::PathFromUtf8(broken), "broken").IsOk());
	}
	REQUIRE(operations.CreatePrefabInFolder(Create(operations, "Crate", Json::object({{"Mesh", Json::object()}})), "Prefabs").IsOk());
	std::filesystem::path const sources = ScriptProject::GetSourceDirectory(*context.GetProject());
	REQUIRE(FileSystem::WriteTextFile(sources / "Player.cs", "public class Player : Strada.Script {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(sources / "AI" / "Brain.cs", "").IsOk());
	REQUIRE(operations.RefreshAssets().IsOk());
	AssetHandle const ghost = AssetManager::FindByPath("Ghost.smat");
	REQUIRE(ghost.IsValid());
	REQUIRE(FileSystem::Remove(assets / "Ghost.smat").IsOk());
	REQUIRE(operations.RefreshAssets().IsOk());
	REQUIRE(AssetManager::IsMissing(ghost));

	Result<std::string> const materialFile = FileSystem::ReadTextFile(assets / "Rock.smat");
	REQUIRE(materialFile.IsOk());
	size_t const steps = context.GetHistory().GetUndoCount();
	Json const scene = SceneSerializer::Serialize(context.GetScene());

	for (std::string const folder : {"", "Textures", "Textures/Wood", "Scenes", "Audio", "Fonts", "Broken", "Prefabs", "Missing"})
	{
		browser.SetFolder(folder);
		drawFrames();
	}
	// A folder that does not exist falls back to the asset directory.
	CHECK(browser.GetFolder().empty());
	for (std::string const folder : {"", "AI", "Missing"})
	{
		browser.SetScriptFolder(folder);
		drawFrames();
	}
	CHECK(browser.GetRoot() == ContentRoot::Scripts);
	CHECK(browser.GetScriptFolder().empty());

	// Every kind of asset the inspector shows: material files, built-in and missing assets, textures, meshes, environments
	// and scenes.
	std::vector<AssetHandle> const inspected = {material.GetValue(),
	                                            texture,
	                                            ghost,
	                                            GetBuiltInHandle(BuiltInAsset::DefaultMaterial),
	                                            GetBuiltInHandle(BuiltInAsset::CubeMesh),
	                                            GetBuiltInHandle(BuiltInAsset::DefaultSky),
	                                            context.GetProject()->GetSettings().StartScene,
	                                            AssetHandle(UUID(987654321))};
	for (AssetHandle const asset : inspected)
	{
		context.SelectAsset(asset);
		drawFrames();
	}
	// An unknown asset is deselected by the inspector.
	CHECK_FALSE(context.GetSelectedAsset().IsValid());

	CHECK(browserOpen);
	CHECK(inspectorOpen);
	CHECK(context.GetHistory().GetUndoCount() == steps);
	CHECK(SceneSerializer::Serialize(context.GetScene()) == scene);
	CHECK(FileSystem::ReadTextFile(assets / "Rock.smat").GetValue() == materialFile.GetValue());

	// Closing the project returns the browser to the (next) asset directory.
	browser.SetFolder("Textures");
	drawFrames();
	CHECK(browser.GetFolder() == "Textures");
	REQUIRE(operations.CloseProject().IsOk());
	drawFrames();
	CHECK(browser.GetFolder().empty());
}

TEST_CASE("Panels: assets dragged from the content browser move into folders and become entities in the hierarchy")
{
	AssetManager::Init();
	struct AssetManagerShutdown
	{
		~AssetManagerShutdown() { AssetManager::Shutdown(); }
	} const shutdown;
	HeadlessImGui imgui;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	std::filesystem::path const assets = context.GetProject()->GetAssetDirectory();
	REQUIRE(operations.CreateAssetFolder("Models/Sub").IsOk());
	REQUIRE(FileSystem::WriteTextFile(assets / "Models" / "Triangle.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n").IsOk());
	REQUIRE(operations.RefreshAssets().IsOk());
	AssetHandle const mesh = AssetManager::FindByPath("Models/Triangle.obj");
	REQUIRE(mesh.IsValid());

	// The browser shows Models/: the folder Sub, then Triangle.obj. The panels sit side by side.
	ContentBrowserPanel browser;
	browser.SetFolder("Models");
	SceneHierarchyPanel hierarchy;
	bool browserOpen = true;
	bool hierarchyOpen = true;
	float fontSize = 0.0f;
	auto const draw = [&]
	{
		fontSize = ImGui::GetFontSize();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(800.0f, 500.0f));
		browser.OnImGuiRender(operations, browserOpen);
		ImGui::SetNextWindowPos(ImVec2(900.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(500.0f, 500.0f));
		hierarchy.OnImGuiRender(operations, hierarchyOpen);
	};
	imgui.Frame(draw);
	imgui.Frame(draw);
	// A folder chosen before the panel first showed the project is kept.
	REQUIRE(browser.GetFolder() == "Models");

	// The tile grid starts where the browser's item area places its first item; tiles are 5.5 font sizes wide.
	ImGuiWindow const* browserWindow = ImGui::FindWindowByName("Content Browser");
	REQUIRE(browserWindow != nullptr);
	ImGuiWindow const* items = nullptr;
	for (ImGuiWindow const* window : GImGui->Windows)
	{
		if (window->ParentWindow == browserWindow && std::strstr(window->Name, "##items") != nullptr)
		{
			items = window;
		}
	}
	REQUIRE(items != nullptr);
	float const tileSize = std::round(fontSize * 5.5f);
	ImVec2 const folderTile(items->DC.CursorStartPos.x + tileSize * 0.5f, items->DC.CursorStartPos.y + tileSize * 0.5f);
	ImVec2 const meshTile(folderTile.x + tileSize + ImGui::GetStyle().ItemSpacing.x, folderTile.y);
	ImGuiWindow const* hierarchyWindow = ImGui::FindWindowByName("Scene Hierarchy");
	REQUIRE(hierarchyWindow != nullptr);
	ImVec2 const hierarchyBackground(hierarchyWindow->Pos.x + hierarchyWindow->Size.x * 0.5f,
	                                 hierarchyWindow->Pos.y + hierarchyWindow->Size.y - 40.0f);

	// Dropped onto the hierarchy's empty space, a mesh becomes a root entity named after it and is selected.
	size_t const entities = context.GetScene().GetEntityCount();
	imgui.Drag(meshTile, hierarchyBackground, draw);
	REQUIRE(context.GetScene().GetEntityCount() == entities + 1);
	REQUIRE(context.GetSelection().GetEntities().size() == 1);
	Entity const created = context.GetScene().GetEntityByUUID(context.GetSelection().GetEntities().front());
	CHECK(created.GetName() == "Triangle");
	CHECK(created.GetComponent<MeshComponent>().Mesh == mesh);
	CHECK_FALSE(context.GetSelectedAsset().IsValid());

	// Dropped onto a folder tile, an asset moves into the folder and keeps its handle.
	imgui.Drag(meshTile, folderTile, draw);
	CHECK(AssetManager::GetMetadata(mesh)->Path == "Models/Sub/Triangle.obj");
	CHECK(FileSystem::Exists(assets / "Models" / "Sub" / "Triangle.obj"));
	CHECK(context.GetScene().GetEntityCount() == entities + 1);
	CHECK(browserOpen);
	CHECK(hierarchyOpen);
}

namespace
{
	// The item area (child window) of the content browser.
	ImGuiWindow const* FindContentBrowserItems()
	{
		ImGuiWindow const* browser = ImGui::FindWindowByName("Content Browser");
		REQUIRE(browser != nullptr);
		for (ImGuiWindow const* window : GImGui->Windows)
		{
			if (window->ParentWindow == browser && std::strstr(window->Name, "##items") != nullptr)
			{
				return window;
			}
		}
		FAIL("the content browser has no item area");
		return nullptr;
	}
}

TEST_CASE("Panels: the content browser renames with F2, confirms deletions and keeps its keys while focused")
{
	AssetManager::Init();
	struct AssetManagerShutdown
	{
		~AssetManagerShutdown() { AssetManager::Shutdown(); }
	} const shutdown;
	HeadlessImGui imgui;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	std::filesystem::path const assets = context.GetProject()->GetAssetDirectory();
	Result<AssetHandle> const rock = operations.CreateMaterial("Art/Rock.smat");
	REQUIRE(rock.IsOk());
	UUID const hero = Create(operations, "Hero", Json::object());

	ContentBrowserPanel browser;
	browser.SetFolder("Art");
	bool open = true;
	// The editor's global Delete shortcut, which must not fire while the browser is focused.
	bool globalDelete = false;
	auto const draw = [&]
	{
		globalDelete = globalDelete || ImGui::Shortcut(ImGuiKey_Delete, ImGuiInputFlags_RouteGlobal);
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(800.0f, 500.0f));
		browser.OnImGuiRender(operations, open);
	};
	imgui.Frame(draw);
	imgui.Frame(draw);
	ImGuiWindow const* items = FindContentBrowserItems();
	float const tileSize = std::round(ImGui::GetFontSize() * 5.5f);
	ImVec2 const firstTile(items->DC.CursorStartPos.x + tileSize * 0.5f, items->DC.CursorStartPos.y + tileSize * 0.5f);
	ImVec2 const background(items->Pos.x + items->Size.x - 20.0f, items->Pos.y + items->Size.y - 20.0f);

	// Clicking a tile shows the asset in the inspector instead of the selected entities.
	std::vector<UUID> const heroSelection = {hero};
	REQUIRE(operations.Select(heroSelection).IsOk());
	imgui.Click(firstTile, draw);
	CHECK(context.GetSelectedAsset() == rock.GetValue());
	CHECK(context.GetSelection().IsEmpty());

	// F2 renames in place; the extension is kept.
	imgui.Press(ImGuiKey_F2, draw);
	imgui.Frame(draw);
	imgui.Frame(draw);
	imgui.Type("Granite");
	imgui.Frame(draw);
	imgui.Press(ImGuiKey_Enter, draw);
	imgui.Frame(draw);
	CHECK(AssetManager::GetMetadata(rock.GetValue())->Path == "Art/Granite.smat");
	CHECK(FileSystem::Exists(assets / "Art" / "Granite.smat"));

	// Names with path separators are refused.
	imgui.Press(ImGuiKey_F2, draw);
	imgui.Frame(draw);
	imgui.Frame(draw);
	imgui.Type("Sub/Stone");
	imgui.Frame(draw);
	imgui.Press(ImGuiKey_Enter, draw);
	imgui.Frame(draw);
	CHECK(AssetManager::GetMetadata(rock.GetValue())->Path == "Art/Granite.smat");

	// Delete asks first; Escape keeps the file.
	imgui.Press(ImGuiKey_Delete, draw);
	ImGuiWindow const* confirmation = ImGui::FindWindowByName("Delete###ContentBrowserDelete");
	REQUIRE(confirmation != nullptr);
	CHECK(confirmation->Active);
	imgui.Press(ImGuiKey_Escape, draw);
	imgui.Frame(draw);
	CHECK_FALSE(confirmation->Active);
	CHECK(AssetManager::IsValid(rock.GetValue()));
	CHECK(FileSystem::Exists(assets / "Art" / "Granite.smat"));

	// With entities selected and nothing selected in the focused browser, Delete reaches neither the browser nor the
	// editor's entity deletion.
	REQUIRE(operations.Select(heroSelection).IsOk());
	imgui.Click(background, draw);
	globalDelete = false;
	imgui.Press(ImGuiKey_Delete, draw);
	imgui.Frame(draw);
	CHECK_FALSE(globalDelete);
	CHECK_FALSE(confirmation->Active);
	CHECK(context.GetSelection().Contains(hero));
	CHECK(open);

	// Folders created outside the editor appear within a second; double-clicking a folder opens it.
	browser.SetFolder("");
	imgui.Frame(draw);
	REQUIRE(FileSystem::CreateDirectories(assets / "Aaa").IsOk());
	for (int frame = 0; frame < 75; frame++)
	{
		imgui.Frame(draw);
	}
	imgui.Click(firstTile, draw, 2);
	CHECK(browser.GetFolder() == "Aaa");
}

TEST_CASE("UI: tile labels show file names on two lines with their extensions in view")
{
	HeadlessImGui imgui;
	imgui.Frame(
		[]
		{
			auto const width = [](char const* text)
			{
				return ImGui::CalcTextSize(text).x;
			};
			// Names that fit stay on one line.
			CHECK(UI::SplitTileLabel("Crate.sprefab", width("Crate.sprefab"), ".sprefab") ==
		          std::array<std::string, 2>{"Crate.sprefab", ""});
			// The extension starts the second line instead of ending the first, also when its dot would not fit.
			CHECK(UI::SplitTileLabel("CrateStack.sprefab", width("CrateStack.spr"), ".sprefab") ==
		          std::array<std::string, 2>{"CrateStack", ".sprefab"});
			CHECK(UI::SplitTileLabel("GameOver.wav", width("GameOver"), ".wav") == std::array<std::string, 2>{"GameOver", ".wav"});
			// A PascalCase name too long for a line breaks between its words, not inside one.
			CHECK(UI::SplitTileLabel("ShowcaseTests.cs", width("ShowcaseTes"), ".cs") ==
		          std::array<std::string, 2>{"Showcase", "Tests.cs"});
			CHECK(UI::SplitTileLabel("XMLParserTests.cs", width("XMLParserTe"), ".cs") ==
		          std::array<std::string, 2>{"XMLParser", "Tests.cs"});
			std::array<std::string, 2> const acronym = UI::SplitTileLabel("XMLParser.cs", width("XMLPar"), ".cs");
			CHECK(acronym[0] == "XML");
			CHECK(acronym[1].starts_with("P"));
			CHECK(acronym[1].ends_with("cs"));
			CHECK(width(acronym[1].c_str()) <= width("XMLPar"));
			// Lines break after spaces; a second line that is too long keeps its extension.
			std::array<std::string, 2> const spaced =
				UI::SplitTileLabel("A Very Long Material Name.smat", width("A Very Long Ma"), ".smat");
			CHECK(spaced[0] == "A Very Long");
			CHECK(spaced[1].ends_with("...smat"));
			CHECK(width(spaced[1].c_str()) <= width("A Very Long Ma"));
			// Long words break where the line is full.
			float const narrow = width("MMMMMMMMMM");
			std::array<std::string, 2> const word = UI::SplitTileLabel("NoSpacesInThisRatherLongMaterialFileName.smat", narrow, ".smat");
			CHECK(width(word[0].c_str()) <= narrow);
			CHECK(word[1].ends_with("...smat"));
			CHECK(width(word[1].c_str()) <= narrow);
			// Folders (no extension) are shortened at their end.
			CHECK(UI::SplitTileLabel("Extremely_Long_Folder_Name_That_Goes_On", narrow)[1].ends_with("..."));
			// Names in other scripts are cut between code points.
			std::string const german = "\xC3\x96lfarbe\xC3\x96lfarbe\xC3\x96lfarbe Gr\xC3\xB6\xC3\x9F"
									   "e.smat";
			for (float const lineWidth : {width("M"), width("MMM"), narrow})
			{
				std::array<std::string, 2> const lines = UI::SplitTileLabel(german, lineWidth, ".smat");
				CHECK_FALSE(lines[0].empty());
				CHECK(Utf8::IsValid(lines[0]));
				CHECK(Utf8::IsValid(lines[1]));
			}
		});
}

namespace
{
	// The panels of a content browser test: placed side by side on the 1920x1080 display.
	struct ContentBrowserFixture
	{
		Testing::AssetManagerScope Assets;
		HeadlessImGui Gui;
		Testing::TemporaryDirectory Temporary;
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		ContentBrowserPanel Browser;
		SceneHierarchyPanel Hierarchy;
		InspectorPanel Inspector;
		bool BrowserOpen = true;
		bool HierarchyOpen = true;
		bool InspectorOpen = true;
		std::vector<std::filesystem::path> Opened;

		ContentBrowserFixture()
		{
			REQUIRE(Operations.CreateProject(Temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			Browser.SetScripts(&Scripts);
			Browser.SetOpenFileCallback(
				[this](std::filesystem::path const& path) -> Result<void>
				{
					Opened.push_back(path);
					return {};
				});
		}

		std::function<void()> GetDraw()
		{
			return [this]
			{
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(800.0f, 500.0f));
				Browser.OnImGuiRender(Operations, BrowserOpen);
				ImGui::SetNextWindowPos(ImVec2(900.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(500.0f, 400.0f));
				Hierarchy.OnImGuiRender(Operations, HierarchyOpen);
				ImGui::SetNextWindowPos(ImVec2(900.0f, 450.0f));
				ImGui::SetNextWindowSize(ImVec2(500.0f, 500.0f));
				Inspector.OnImGuiRender(Operations, InspectorOpen);
			};
		}

		std::filesystem::path GetAssetDirectory() const { return Context.GetProject()->GetAssetDirectory(); }
		std::filesystem::path GetScriptsDirectory() const { return ScriptProject::GetSourceDirectory(*Context.GetProject()); }

		// The center of the index-th tile of the first row of the browser.
		ImVec2 GetTile(size_t index) const
		{
			ImGuiWindow const* items = FindContentBrowserItems();
			float const tileSize = std::round(ImGui::GetFontSize() * 5.5f);
			float const step = tileSize + ImGui::GetStyle().ItemSpacing.x;
			return ImVec2(items->DC.CursorStartPos.x + tileSize * 0.5f + step * static_cast<float>(index),
			              items->DC.CursorStartPos.y + tileSize * 0.5f);
		}
	};
}

TEST_CASE("Panels: files dropped onto the editor are imported into the folder under the cursor or the displayed one")
{
	ContentBrowserFixture fixture;
	HeadlessImGui& imgui = fixture.Gui;
	REQUIRE(fixture.Operations.CreateAssetFolder("Models/Sub").IsOk());
	std::filesystem::path const outside = fixture.Temporary.GetPath() / "Downloads";
	REQUIRE(Image(2, 2, 4).WritePNG(outside / "Oak.png").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Notes.docx", "notes").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Mover.cs", "public class Mover : Strada.Script {}").IsOk());
	fixture.Browser.SetFolder("Models");
	std::function<void()> const draw = fixture.GetDraw();
	imgui.Frame(draw);
	imgui.Frame(draw);
	ImGuiWindow const* items = FindContentBrowserItems();
	ImVec2 const folderTile = fixture.GetTile(0);
	ImVec2 const background(items->Pos.x + items->Size.x - 20.0f, items->Pos.y + items->Size.y - 20.0f);

	// Onto the folder Sub's tile: into Sub, and the new asset is selected.
	ImGui::GetIO().AddMousePosEvent(folderTile.x, folderTile.y);
	imgui.Frame(draw);
	fixture.Browser.DropFiles({outside / "Oak.png"});
	imgui.Frame(draw);
	AssetHandle const oak = AssetManager::FindByPath("Models/Sub/Oak.png");
	REQUIRE(oak.IsValid());
	CHECK(fixture.Context.GetSelectedAsset() == oak);
	CHECK(fixture.Browser.GetFolder() == "Models");

	// Onto the empty space: into the displayed folder. Files that are neither assets nor scripts are reported.
	ImGui::GetIO().AddMousePosEvent(background.x, background.y);
	imgui.Frame(draw);
	uint64_t const firstLog = Log::GetNextEntryIndex();
	fixture.Browser.DropFiles({outside / "Oak.png", outside / "Notes.docx"});
	imgui.Frame(draw);
	CHECK(AssetManager::FindByPath("Models/Oak.png").IsValid());
	CHECK(Testing::WasLogged(firstLog, "Notes.docx"));
	CHECK_FALSE(FileSystem::Exists(fixture.GetAssetDirectory() / "Models" / "Notes.docx"));

	// C# sources go to the scripts, which the browser then shows with the new script selected.
	fixture.Browser.DropFiles({outside / "Mover.cs"});
	imgui.Frame(draw);
	CHECK(FileSystem::Exists(fixture.GetScriptsDirectory() / "Mover.cs"));
	CHECK(fixture.Browser.GetRoot() == ContentRoot::Scripts);
	imgui.Frame(draw);

	// Dropped onto the Scripts root, assets still go to the displayed asset folder, shown again.
	fixture.Browser.DropFiles({outside / "Oak.png"});
	imgui.Frame(draw);
	CHECK(AssetManager::FindByPath("Models/Oak 1.png").IsValid());
	CHECK(fixture.Browser.GetRoot() == ContentRoot::Assets);

	// A closed browser that is not drawn imports nothing until it is drawn again.
	fixture.Browser.DropFiles({outside / "Oak.png"});
	CHECK_FALSE(AssetManager::FindByPath("Models/Oak 2.png").IsValid());
	imgui.Frame(draw);
	CHECK(AssetManager::FindByPath("Models/Oak 2.png").IsValid());
	CHECK(fixture.BrowserOpen);
}

TEST_CASE("Panels: the content browser shows the project's C# scripts under their own root, opens and renames them")
{
	ContentBrowserFixture fixture;
	HeadlessImGui& imgui = fixture.Gui;
	std::filesystem::path const sources = fixture.GetScriptsDirectory();
	REQUIRE(FileSystem::WriteTextFile(sources / "AI" / "Brain.cs", "class Brain {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(sources / "Player.cs", "class Player {}").IsOk());
	fixture.Browser.SetScriptFolder("");
	std::function<void()> const draw = fixture.GetDraw();
	imgui.Frame(draw);
	imgui.Frame(draw);
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();

	// The folder AI comes first, then Player.cs; double-clicking a script opens it with the desktop.
	imgui.Click(fixture.GetTile(1), draw, 2);
	REQUIRE(fixture.Opened.size() == 1);
	CHECK(fixture.Opened.front() == sources / "Player.cs");
	CHECK_FALSE(fixture.Context.GetSelectedAsset().IsValid());

	// Double-clicking a folder browses it.
	imgui.Click(fixture.GetTile(0), draw, 2);
	CHECK(fixture.Browser.GetScriptFolder() == "AI");
	CHECK(fixture.Browser.GetRoot() == ContentRoot::Scripts);

	// F2 renames the selected script; its extension stays.
	imgui.Click(fixture.GetTile(0), draw);
	imgui.Press(ImGuiKey_F2, draw);
	imgui.Frame(draw);
	imgui.Frame(draw);
	imgui.Type("Mind");
	imgui.Frame(draw);
	imgui.Press(ImGuiKey_Enter, draw);
	imgui.Frame(draw);
	CHECK(FileSystem::Exists(sources / "AI" / "Mind.cs"));
	CHECK_FALSE(FileSystem::Exists(sources / "AI" / "Brain.cs"));

	// Delete asks first; Escape keeps the file.
	imgui.Press(ImGuiKey_Delete, draw);
	ImGuiWindow const* confirmation = ImGui::FindWindowByName("Delete###ContentBrowserDelete");
	REQUIRE(confirmation != nullptr);
	CHECK(confirmation->Active);
	imgui.Press(ImGuiKey_Escape, draw);
	imgui.Frame(draw);
	CHECK(FileSystem::Exists(sources / "AI" / "Mind.cs"));
	// The scene and its history do not change.
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
	CHECK(fixture.BrowserOpen);
}

TEST_CASE("Panels: scripts and script folders move when dragged onto script folders, and C# files dropped there go in")
{
	ContentBrowserFixture fixture;
	HeadlessImGui& imgui = fixture.Gui;
	std::filesystem::path const sources = fixture.GetScriptsDirectory();
	REQUIRE(FileSystem::WriteTextFile(sources / "AI" / "Brain.cs", "class Brain {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(sources / "Tools" / "Helper.cs", "class Helper {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(sources / "Player.cs", "class Player {}").IsOk());
	fixture.Browser.SetScriptFolder("");
	std::function<void()> const draw = fixture.GetDraw();
	imgui.Frame(draw);
	imgui.Frame(draw);
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();

	// The folders AI and Tools come first, then Player.cs, which goes into AI.
	imgui.Drag(fixture.GetTile(2), fixture.GetTile(0), draw);
	CHECK(FileSystem::Exists(sources / "AI" / "Player.cs"));
	CHECK_FALSE(FileSystem::Exists(sources / "Player.cs"));

	// Tools goes into AI with what it holds; a folder dropped onto itself stays where it is.
	imgui.Frame(draw);
	imgui.Drag(fixture.GetTile(1), fixture.GetTile(0), draw);
	CHECK(FileSystem::Exists(sources / "AI" / "Tools" / "Helper.cs"));
	CHECK_FALSE(FileSystem::Exists(sources / "Tools"));
	imgui.Frame(draw);
	imgui.Drag(fixture.GetTile(0), fixture.GetTile(0), draw);
	CHECK(FileSystem::Exists(sources / "AI" / "Brain.cs"));

	// C# files dropped from the OS onto a script folder's tile go into that folder.
	std::filesystem::path const outside = fixture.Temporary.GetPath() / "Downloads";
	REQUIRE(FileSystem::WriteTextFile(outside / "Mover.cs", "class Mover {}").IsOk());
	ImVec2 const folderTile = fixture.GetTile(0);
	ImGui::GetIO().AddMousePosEvent(folderTile.x, folderTile.y);
	imgui.Frame(draw);
	fixture.Browser.DropFiles({outside / "Mover.cs"});
	imgui.Frame(draw);
	CHECK(FileSystem::Exists(sources / "AI" / "Mover.cs"));
	CHECK(fixture.Browser.GetRoot() == ContentRoot::Scripts);
	CHECK(fixture.Browser.GetScriptFolder().empty());
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
}

TEST_CASE("Panels: scripts dragged from the content browser onto a hierarchy row or the inspector run on the entities")
{
	Testing::ScriptEngineScope scripting;
	ContentBrowserFixture fixture;
	HeadlessImGui& imgui = fixture.Gui;
	REQUIRE(FileSystem::WriteTextFile(fixture.GetScriptsDirectory() / "Mover.cs",
	                                  "namespace Strada.Tests;\npublic class Mover : Strada.Script {}\n")
	            .IsOk());
	UUID const hero = Create(fixture.Operations, "Hero", Json::object());
	UUID const other = Create(fixture.Operations, "Other", Json::object());
	fixture.Browser.SetScriptFolder("");
	std::function<void()> const draw = fixture.GetDraw();
	imgui.Frame(draw);
	imgui.Frame(draw);
	ImVec2 const scriptTile = fixture.GetTile(0);

	// Onto the row of Hero (the first row of the hierarchy).
	ImGuiWindow const* hierarchy = ImGui::FindWindowByName("Scene Hierarchy");
	REQUIRE(hierarchy != nullptr);
	ImGuiWindow const* rows = nullptr;
	for (ImGuiWindow const* window : GImGui->Windows)
	{
		if (window->ParentWindow == hierarchy && std::strstr(window->Name, "##entities") != nullptr)
		{
			rows = window;
		}
	}
	REQUIRE(rows != nullptr);
	ImVec2 const heroRow(rows->DC.CursorStartPos.x + 60.0f, rows->DC.CursorStartPos.y + ImGui::GetTextLineHeight() * 0.5f);
	imgui.Drag(scriptTile, heroRow, draw);
	Scene& scene = fixture.Context.GetScene();
	REQUIRE(scene.GetEntityByUUID(hero).HasComponent<ScriptComponent>());
	CHECK(scene.GetEntityByUUID(hero).GetComponent<ScriptComponent>().ClassName == "Strada.Tests.Mover");
	CHECK_FALSE(scene.GetEntityByUUID(other).HasComponent<ScriptComponent>());

	// Onto the inspector: the inspected entity. Pressing on the tile to drag it keeps the selection. (Frames pass first: a
	// second press on the tile right after the first would be a double click, which opens the script.)
	std::vector<UUID> const selection = {other};
	REQUIRE(fixture.Operations.Select(selection).IsOk());
	for (int frame = 0; frame < 40; frame++)
	{
		imgui.Frame(draw);
	}
	ImGuiWindow const* inspector = ImGui::FindWindowByName("Inspector");
	REQUIRE(inspector != nullptr);
	ImVec2 const inspectorSpace(inspector->Pos.x + inspector->Size.x * 0.5f, inspector->Pos.y + inspector->Size.y - 40.0f);
	imgui.Drag(scriptTile, inspectorSpace, draw);
	CHECK(fixture.Context.GetSelection().Contains(other));
	REQUIRE(scene.GetEntityByUUID(other).HasComponent<ScriptComponent>());
	CHECK(scene.GetEntityByUUID(other).GetComponent<ScriptComponent>().ClassName == "Strada.Tests.Mover");
}
