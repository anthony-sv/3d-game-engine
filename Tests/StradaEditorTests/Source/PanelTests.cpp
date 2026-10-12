#include "HeadlessImGui.h"
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
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace Strada;
using Strada::Testing::HeadlessImGui;

namespace
{
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
	struct SwitchedProbe
	{
		bool UseGain = false;
		float Gain = 1.0f;
		bool Visible = true;
	};
}

template<>
struct Strada::StructTraits<SwitchedProbe>
{
	static constexpr std::string_view Name = "SwitchedProbe";
	static constexpr auto Fields = std::make_tuple(Field("UseGain", &SwitchedProbe::UseGain).Doc("Applies Gain."),
	                                               Field("Gain", &SwitchedProbe::Gain).Range(0.0, 10.0).EnabledBy("UseGain"),
	                                               Field("Visible", &SwitchedProbe::Visible));
};

namespace
{
	// A long name behind a switch, without a description: its label only has a tooltip when it is shortened.
	struct LongNameProbe
	{
		bool OverrideAmbientOcclusionIntensity = false;
		float AmbientOcclusionIntensity = 1.5f;
	};
}

template<>
struct Strada::StructTraits<LongNameProbe>
{
	static constexpr std::string_view Name = "LongNameProbe";
	static constexpr auto Fields = std::make_tuple(
		Field("OverrideAmbientOcclusionIntensity", &LongNameProbe::OverrideAmbientOcclusionIntensity),
		Field("AmbientOcclusionIntensity", &LongNameProbe::AmbientOcclusionIntensity).EnabledBy("OverrideAmbientOcclusionIntensity"));
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

TEST_CASE("FieldEditor: a switch is a check box in front of the field it switches, which is disabled while it is off")
{
	HeadlessImGui imgui;
	std::vector<FieldDescriptor> const fields = GetFieldDescriptors<StructTraits<SwitchedProbe>, SwitchedProbe>();
	Json values = SerializeFields<StructTraits<SwitchedProbe>>(SwitchedProbe{});
	FieldEditor editor;
	std::optional<FieldChange> last;
	bool focusGain = false;
	int rows = 0;
	ImVec2 switchCenter;
	auto const draw = [&]
	{
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
		ImGui::Begin("Switches");
		if (focusGain)
		{
			// The item after the switch, when Gain can be edited.
			ImGui::SetKeyboardFocusHere(1);
			focusGain = false;
		}
		// The first row's first item: a square check box one frame high.
		ImVec2 const tableStart = ImGui::GetCursorScreenPos();
		float const half = ImGui::GetFrameHeight() * 0.5f;
		switchCenter = ImVec2(tableStart.x + half, tableStart.y + ImGui::GetStyle().CellPadding.y + half);
		if (std::optional<FieldChange> change = editor.Draw(fields, values))
		{
			last = std::move(change);
		}
		ImGuiTable const* table = ImGui::TableFindByID(ImGui::GetID("##fields"));
		rows = table != nullptr ? table->CurrentRow + 1 : 0;
		ImGui::End();
	};
	auto const typeIntoGain = [&](char const* text)
	{
		focusGain = true;
		imgui.Frame(draw);
		imgui.Frame(draw);
		imgui.Type(text);
		imgui.Frame(draw);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
		imgui.Frame(draw);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
		imgui.Frame(draw);
	};

	// The switch has no row of its own: one for Gain, one for Visible.
	imgui.Frame(draw);
	imgui.Frame(draw);
	CHECK(rows == 2);
	CHECK_FALSE(last);

	// The check box at the start of Gain's row is the switch.
	imgui.Click(switchCenter, draw);
	REQUIRE(last);
	CHECK(last->Path == std::vector<std::string_view>{"UseGain"});
	CHECK(last->Value == true);

	// While the switch is off Gain is disabled: typing changes nothing.
	last.reset();
	typeIntoGain("5");
	CHECK_FALSE(last);

	// Switched on, Gain takes typed values.
	values["UseGain"] = true;
	typeIntoGain("5");
	REQUIRE(last);
	CHECK(last->Path == std::vector<std::string_view>{"Gain"});
	CHECK(last->Value.get<float>() == doctest::Approx(5.0f));
}

namespace
{
	// The widths of the contents of a field table's name column and of both of its columns, as the last frame laid them out,
	// and where the table starts on the screen.
	struct NameColumnLayout
	{
		float Name = 0.0f;
		float Table = 0.0f;
		ImVec2 Start;
	};

	NameColumnLayout GetNameColumnLayout(ImGuiID tableId, ImVec2 start)
	{
		ImGuiTable const* table = ImGui::TableFindByID(tableId);
		REQUIRE(table != nullptr);
		float const name = table->Columns[0].WorkMaxX - table->Columns[0].WorkMinX;
		return {name, name + table->Columns[1].WorkMaxX - table->Columns[1].WorkMinX, start};
	}

	// Draws one field table in a window of its own at (x, 0), `width` wide.
	NameColumnLayout DrawFieldsWindow(FieldEditor& editor, char const* window, float x, float width,
	                                  std::span<FieldDescriptor const> fields, Json const& values)
	{
		ImGui::SetNextWindowPos(ImVec2(x, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(width, 600.0f));
		ImGui::Begin(window);
		ImVec2 const start = ImGui::GetCursorScreenPos();
		CHECK_FALSE(editor.Draw(fields, values));
		NameColumnLayout const layout = GetNameColumnLayout(ImGui::GetID("##fields"), start);
		ImGui::End();
		return layout;
	}

	bool IsTooltipShown()
	{
		ImGuiWindow const* tooltip = ImGui::FindWindowByName("##Tooltip_00");
		return tooltip != nullptr && tooltip->Active;
	}
}

TEST_CASE("FieldEditor: the name column starts as wide as the names need, from 40% to 60% of the table")
{
	HeadlessImGui imgui;
	FieldEditor editor;
	ComponentInfo const& postProcess = ComponentRegistry::Get<PostProcessComponent>();
	Json postProcessValues = Json::object();
	for (FieldDescriptor const& field : postProcess.Fields)
	{
		postProcessValues[std::string(field.Name)] = field.Default;
	}
	std::vector<FieldDescriptor> const probe = GetFieldDescriptors<StructTraits<Probe>, Probe>();
	Json const probeValues = SerializeFields<StructTraits<Probe>>(Probe{});
	std::vector<FieldDescriptor> const longName = GetFieldDescriptors<StructTraits<LongNameProbe>, LongNameProbe>();
	Json const longNameValues = SerializeFields<StructTraits<LongNameProbe>>(LongNameProbe{});

	// Tables keep the share they start with, so each width gets windows of its own.
	char const* const label = "Ambient Occlusion Intensity";
	float labelWidth = 0.0f;
	float switchedLabelWidth = 0.0f;
	NameColumnLayout postProcessColumn;
	NameColumnLayout probeColumn;
	NameColumnLayout wideColumn;
	NameColumnLayout narrowColumn;
	auto const draw = [&]
	{
		labelWidth = ImGui::CalcTextSize(label).x;
		switchedLabelWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + labelWidth;
		postProcessColumn = DrawFieldsWindow(editor, "Post Process", 0.0f, 400.0f, postProcess.Fields, postProcessValues);
		probeColumn = DrawFieldsWindow(editor, "Probe", 420.0f, 400.0f, probe, probeValues);
		wideColumn = DrawFieldsWindow(editor, "Wide", 840.0f, 400.0f, longName, longNameValues);
		narrowColumn = DrawFieldsWindow(editor, "Narrow", 1260.0f, 250.0f, longName, longNameValues);
	};
	imgui.Frame(draw);
	imgui.Frame(draw);

	// At 400 pixels the post-process volume's longest switched name, check box and all, fits in the name column.
	CHECK(postProcessColumn.Name >= switchedLabelWidth);
	CHECK(postProcessColumn.Name <= postProcessColumn.Table * 0.6f + 1.0f);
	// Short names keep the values at 60%, so their tables line up.
	CHECK(probeColumn.Name == doctest::Approx(probeColumn.Table * 0.4f).epsilon(0.01));
	CHECK(wideColumn.Name >= switchedLabelWidth);
	// Too narrow for the name: the column stops at 60% and the name is shortened.
	CHECK(narrowColumn.Name == doctest::Approx(narrowColumn.Table * 0.6f).epsilon(0.01));
	CHECK(narrowColumn.Name < switchedLabelWidth);

	// Rests the mouse on the long name, which starts after its switch on the first row, until tooltips show.
	auto const hoverName = [&](NameColumnLayout const& layout)
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		float const frameHeight = ImGui::GetFrameHeight();
		ImGui::GetIO().AddMousePosEvent(layout.Start.x + frameHeight + style.ItemInnerSpacing.x + 10.0f,
		                                layout.Start.y + style.CellPadding.y + frameHeight * 0.5f);
		for (int frame = 0; frame < 30; frame++)
		{
			imgui.Frame(draw);
		}
	};

	// A shortened name shows in full in its tooltip; a name that fits has no tooltip without a description.
	hoverName(narrowColumn);
	REQUIRE(IsTooltipShown());
	CHECK(ImGui::FindWindowByName("##Tooltip_00")->ContentSize.x >= labelWidth - 1.0f);
	hoverName(wideColumn);
	CHECK_FALSE(IsTooltipShown());
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
					ImGui::SetNextWindowSize(ImVec2(420.0f, 900.0f));
					inspector.OnImGuiRender(operations, open);
				});
		}
		if (selection.size() == 1 && selection.front() == first)
		{
			// The class picker's row lines up with the rows of the class's fields below it.
			ImGuiWindow const* window = ImGui::FindWindowByName("Inspector");
			REQUIRE(window != nullptr);
			ImGuiID const component = ImHashStr("Script", 0, window->ID);
			CHECK(GetNameColumnLayout(ImHashStr("##scriptClass", 0, component), ImVec2()).Name ==
			      GetNameColumnLayout(ImHashStr("##fields", 0, component), ImVec2()).Name);
		}
	}
	CHECK(open);
	CHECK(context.GetHistory().GetUndoCount() == steps);
	CHECK(SceneSerializer::Serialize(context.GetScene()) == before);
}

TEST_CASE("Panels: the inspector folds the Transform of UI elements away and shows it read-only")
{
	HeadlessImGui imgui;
	EditorContext context;
	EditorOperations operations(context);
	UUID const canvas = Create(operations, "Canvas", Json::object({{"UICanvas", Json::object()}}));
	EntityCreateInfo panelInfo;
	panelInfo.Name = "Panel";
	panelInfo.Parent = canvas;
	panelInfo.Components = Json::object({{"UIRect", Json::object()}, {"UIImage", Json::object()}});
	Result<UUID> const created = operations.CreateEntity(panelInfo);
	REQUIRE(created.IsOk());
	UUID const panel = created.GetValue();
	UUID const crate = Create(operations, "Crate", Json::object());

	InspectorPanel inspector;
	bool open = true;
	auto const draw = [&]
	{
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(420.0f, 900.0f));
		inspector.OnImGuiRender(operations, open);
	};
	// The Transform's field table, when the latest frame drew it.
	auto const transformFields = []() -> ImGuiTable const*
	{
		ImGuiWindow const* window = ImGui::FindWindowByName("Inspector");
		REQUIRE(window != nullptr);
		ImGuiTable const* table = ImGui::TableFindByID(ImHashStr("##fields", 0, ImHashStr("Transform", 0, window->ID)));
		return table != nullptr && table->LastFrameActive == ImGui::GetFrameCount() ? table : nullptr;
	};
	auto const select = [&](std::vector<UUID> const& selection)
	{
		REQUIRE(operations.Select(selection).IsOk());
		for (int frame = 0; frame < 3; frame++)
		{
			imgui.Frame(draw);
		}
	};
	// Drags the first field (Translation's X, after its reset button) to the right.
	auto const dragTranslation = [&]
	{
		ImGuiTable const* table = transformFields();
		REQUIRE(table != nullptr);
		ImVec2 const from(table->Columns[1].WorkMinX + ImGui::GetFrameHeight() + 10.0f,
		                  table->OuterRect.Min.y + ImGui::GetStyle().CellPadding.y + ImGui::GetFrameHeight() * 0.5f);
		imgui.Drag(from, ImVec2(from.x + 60.0f, from.y), draw);
	};

	// Entities placed by their Transform show it open, the canvas too (it places the canvas in the scene view).
	select({crate});
	CHECK(transformFields() != nullptr);
	select({canvas});
	CHECK(transformFields() != nullptr);
	// A UI element's is folded; selected with an entity that is not UI, it is the other's to edit.
	select({panel});
	CHECK(transformFields() == nullptr);
	select({panel, crate});
	CHECK(transformFields() != nullptr);

	// Unfolded, the fields of a UI element are disabled: dragging the one that moves the crate changes nothing.
	select({crate});
	dragTranslation();
	CHECK(context.GetScene().GetEntityByUUID(crate).GetComponent<TransformComponent>().Translation.x != 0.0f);
	select({panel});
	ImGuiWindow* window = ImGui::FindWindowByName("Inspector");
	REQUIRE(window != nullptr);
	window->StateStorage.SetInt(ImHashStr("###TransformPlacedByRect", 0, ImHashStr("Transform", 0, window->ID)), 1);
	imgui.Frame(draw);
	REQUIRE(transformFields() != nullptr);
	size_t const steps = context.GetHistory().GetUndoCount();
	dragTranslation();
	CHECK(context.GetHistory().GetUndoCount() == steps);
	CHECK(context.GetScene().GetEntityByUUID(panel).GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
	CHECK(open);
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

	for (std::string const folder : {"", "Textures", "Textures/Wood", "Scenes", "Missing"})
	{
		browser.SetFolder(folder);
		drawFrames();
	}
	// A folder that does not exist falls back to the asset directory.
	CHECK(browser.GetFolder().empty());

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
	ImGuiWindow const* confirmation = ImGui::FindWindowByName("Delete Asset");
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
