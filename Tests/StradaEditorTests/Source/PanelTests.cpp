#include "Audio/AudioTestUtilities.h"
#include "GpuTestUtilities.h"
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
#include "Editor/Panels/ViewportPanel.h"
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
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/Renderer/Renderer.h"
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

namespace
{
	// The text the panels write in one frame (labels, buttons, messages), through ImGui's logging.
	std::string CaptureText(HeadlessImGui& imgui, std::function<void()> const& draw)
	{
		std::string text;
		imgui.Frame(
			[&]
			{
				ImGui::LogToBuffer();
				draw();
				text = GImGui->LogBuffer.c_str();
				ImGui::LogFinish();
			});
		return text;
	}

	// Moves the mouse over the points, a frame each, and returns the first one where the item with the ID is hovered.
	std::optional<ImVec2> FindHovered(HeadlessImGui& imgui, std::function<void()> const& draw, std::vector<ImVec2> const& points,
	                                  ImGuiID id)
	{
		for (ImVec2 const& point : points)
		{
			ImGui::GetIO().AddMousePosEvent(point.x, point.y);
			imgui.Frame(draw);
			if (GImGui->HoveredId == id)
			{
				return point;
			}
		}
		return std::nullopt;
	}

	// Points down the middle of a window (menu items span its width), or along a row of it.
	std::vector<ImVec2> GetColumnPoints(ImGuiWindow const* window)
	{
		std::vector<ImVec2> points;
		for (float y = window->Pos.y + 1.0f; y < window->Pos.y + window->Size.y - 1.0f; y += 2.0f)
		{
			points.emplace_back(window->Pos.x + window->Size.x * 0.5f, y);
		}
		return points;
	}

	std::vector<ImVec2> GetRowPoints(ImGuiWindow const* window, float y)
	{
		std::vector<ImVec2> points;
		for (float x = window->Pos.x + 1.0f; x < window->Pos.x + window->Size.x - 1.0f; x += 2.0f)
		{
			points.emplace_back(x, y);
		}
		return points;
	}

	// The distinct items of the content browser's toolbar from right to left: Refresh, Import, "+", the search field, the size
	// slider, then the breadcrumbs. Each with a point inside it.
	std::vector<ImVec2> FindToolbarItems(HeadlessImGui& imgui, std::function<void()> const& draw)
	{
		ImGuiWindow const* browser = ImGui::FindWindowByName("Content Browser");
		REQUIRE(browser != nullptr);
		float const y = browser->DC.CursorStartPos.y + ImGui::GetStyle().CellPadding.y + ImGui::GetFrameHeight() * 0.5f;
		std::vector<ImVec2> found;
		ImGuiID last = 0;
		for (float x = browser->Pos.x + browser->Size.x - ImGui::GetStyle().WindowPadding.x - 2.0f; x > browser->Pos.x && found.size() < 5;
		     x -= 2.0f)
		{
			ImGui::GetIO().AddMousePosEvent(x, y);
			imgui.Frame(draw);
			ImGuiID const hovered = GImGui->HoveredId;
			if (hovered != 0 && hovered != last)
			{
				found.emplace_back(x, y);
			}
			last = hovered;
		}
		REQUIRE(found.size() == 5);
		return found;
	}

	// The popup opened last.
	ImGuiWindow* GetOpenPopup()
	{
		REQUIRE_FALSE(GImGui->OpenPopupStack.empty());
		ImGuiWindow* popup = GImGui->OpenPopupStack.back().Window;
		REQUIRE(popup != nullptr);
		return popup;
	}

	ImGuiWindow* FindChildWindow(char const* parent, char const* name)
	{
		ImGuiWindow const* window = ImGui::FindWindowByName(parent);
		REQUIRE(window != nullptr);
		for (ImGuiWindow* child : GImGui->Windows)
		{
			if (child->ParentWindow == window && std::strstr(child->Name, name) != nullptr && child->Active)
			{
				return child;
			}
		}
		return nullptr;
	}
}

TEST_CASE("Panels: the content browser's + menu makes C# scripts in the shown script folder, and materials among the assets")
{
	ContentBrowserFixture fixture;
	HeadlessImGui& imgui = fixture.Gui;
	REQUIRE(FileSystem::WriteTextFile(fixture.GetScriptsDirectory() / "AI" / "Brain.cs", "class Brain {}").IsOk());
	std::vector<std::string> requested;
	fixture.Browser.SetNewScriptCallback(
		[&requested](std::string const& folder)
		{
			requested.push_back(folder);
		});
	std::function<void()> const draw = fixture.GetDraw();
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();

	struct Root
	{
		bool Scripts;
		std::string ExpectedFolder;
	};
	for (Root const& root : {Root{false, ""}, Root{true, "AI"}})
	{
		CAPTURE(root.Scripts);
		if (root.Scripts)
		{
			fixture.Browser.SetScriptFolder("AI");
		}
		else
		{
			fixture.Browser.SetFolder("");
		}
		imgui.Frame(draw);
		imgui.Frame(draw);
		std::vector<ImVec2> const toolbar = FindToolbarItems(imgui, draw);
		imgui.Click(toolbar[2], draw);
		// A popup that appears is sized after its first frame.
		imgui.Frame(draw);
		imgui.Frame(draw);
		ImGuiWindow* menu = GetOpenPopup();
		// Materials belong to the assets: the item is disabled among the scripts (ImGui reports disabled items as hovered).
		std::optional<ImVec2> const material = FindHovered(imgui, draw, GetColumnPoints(menu), menu->GetID("Material"));
		REQUIRE(material.has_value());
		CHECK(GImGui->HoveredIdIsDisabled == root.Scripts);
		menu = GetOpenPopup();
		std::optional<ImVec2> const script = FindHovered(imgui, draw, GetColumnPoints(menu), menu->GetID("C# Script..."));
		REQUIRE(script.has_value());
		imgui.Click(*script, draw);
		REQUIRE(requested.size() == (root.Scripts ? 2u : 1u));
		CHECK(requested.back() == root.ExpectedFolder);
	}
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
}

TEST_CASE("Panels: the content browser searches the shown root for what is typed into its search field")
{
	ContentBrowserFixture fixture;
	HeadlessImGui& imgui = fixture.Gui;
	REQUIRE(FileSystem::WriteTextFile(fixture.GetScriptsDirectory() / "AI" / "Brain.cs", "class Brain {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(fixture.GetScriptsDirectory() / "Player.cs", "class Player {}").IsOk());
	REQUIRE(Image(2, 2, 4).WritePNG(fixture.GetAssetDirectory() / "Wood" / "Oak.png").IsOk());
	REQUIRE(FileSystem::WriteTextFile(fixture.GetAssetDirectory() / "Wood" / "Oak.txt", "notes").IsOk());
	REQUIRE(fixture.Operations.CreateMaterial("Rock.smat").IsOk());
	REQUIRE(fixture.Operations.RefreshAssets().IsOk());
	std::function<void()> const draw = fixture.GetDraw();

	auto const search = [&](char const* text)
	{
		// Escape ends an edit of the field (restoring what it held when the edit began); then it is emptied.
		imgui.Press(ImGuiKey_Escape, draw);
		fixture.Browser.SetSearch("");
		imgui.Frame(draw);
		std::vector<ImVec2> const toolbar = FindToolbarItems(imgui, draw);
		imgui.Click(toolbar[3], draw);
		imgui.Type(text);
		imgui.Frame(draw);
		imgui.Frame(draw);
		CHECK(fixture.Browser.GetSearch() == text);
		return CaptureText(imgui, draw);
	};

	// Among the assets: registered assets and the other files whose paths hold the text.
	fixture.Browser.SetFolder("");
	imgui.Frame(draw);
	std::string const oak = search("oak");
	CHECK(oak.find("Oak.png") != std::string::npos);
	CHECK(oak.find("Oak.txt") != std::string::npos);
	CHECK(oak.find("Rock.smat") == std::string::npos);
	CHECK(search("nothing").find("No assets match the search.") != std::string::npos);

	// Among the scripts: their paths, folders included.
	fixture.Browser.SetScriptFolder("");
	imgui.Frame(draw);
	std::string const ai = search("ai/");
	CHECK(ai.find("Brain.cs") != std::string::npos);
	CHECK(ai.find("Player.cs") == std::string::npos);
	CHECK(search("nothing").find("No scripts match the search.") != std::string::npos);

	// Browsing a folder ends the search.
	fixture.Browser.SetScriptFolder("");
	CHECK(fixture.Browser.GetSearch().empty());
	std::string const all = CaptureText(imgui, draw);
	CHECK(all.find("Player.cs") != std::string::npos);
}

TEST_CASE("Panels: a script revealed in the content browser is shown selected in its folder")
{
	ContentBrowserFixture fixture;
	HeadlessImGui& imgui = fixture.Gui;
	std::filesystem::path const sources = fixture.GetScriptsDirectory();
	REQUIRE(FileSystem::WriteTextFile(sources / "AI" / "Brain.cs", "class Brain {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(sources / "AI" / "Eyes.cs", "class Eyes {}").IsOk());
	UUID const hero = Create(fixture.Operations, "Hero", Json::object());
	std::vector<UUID> const selection = {hero};
	REQUIRE(fixture.Operations.Select(selection).IsOk());
	std::function<void()> const draw = fixture.GetDraw();
	fixture.Browser.SetFolder("");
	imgui.Frame(draw);

	// Files outside the scripts change nothing.
	fixture.Browser.RevealScript(fixture.Context, fixture.GetAssetDirectory() / "Brain.cs");
	CHECK(fixture.Browser.GetRoot() == ContentRoot::Assets);
	CHECK(fixture.Context.GetSelection().Contains(hero));

	fixture.Browser.RevealScript(fixture.Context, sources / "AI" / "Eyes.cs");
	CHECK(fixture.Browser.GetRoot() == ContentRoot::Scripts);
	CHECK(fixture.Browser.GetScriptFolder() == "AI");
	CHECK(fixture.Context.GetSelection().IsEmpty());
	imgui.Frame(draw);
	imgui.Frame(draw);
	// The selected script is the one F2 renames.
	imgui.Press(ImGuiKey_F2, draw);
	imgui.Frame(draw);
	imgui.Frame(draw);
	imgui.Type("Sight");
	imgui.Frame(draw);
	imgui.Press(ImGuiKey_Enter, draw);
	imgui.Frame(draw);
	CHECK(FileSystem::Exists(sources / "AI" / "Sight.cs"));
	CHECK(FileSystem::Exists(sources / "AI" / "Brain.cs"));
}

TEST_CASE("Panels: folders of C# sources dropped onto the editor go to the scripts, and failed imports are noticed")
{
	ContentBrowserFixture fixture;
	HeadlessImGui& imgui = fixture.Gui;
	int consoles = 0;
	fixture.Browser.SetShowConsoleCallback(
		[&consoles]
		{
			consoles++;
		});
	std::filesystem::path const outside = fixture.Temporary.GetPath() / "Downloads";
	REQUIRE(FileSystem::WriteTextFile(outside / "AI" / "Brain.cs", "namespace Game; class Brain {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "AI" / "Steering" / "Seek.cs", "namespace Game; class Seek {}").IsOk());
	REQUIRE(FileSystem::WriteTextFile(outside / "Model.blend", "blender").IsOk());
	REQUIRE(Image(2, 2, 4).WritePNG(outside / "Oak.png").IsOk());
	std::function<void()> const draw = fixture.GetDraw();

	// Onto the empty space of the Scripts root: the folder goes into the shown script folder with its subfolders.
	fixture.Browser.SetScriptFolder("");
	imgui.Frame(draw);
	imgui.Frame(draw);
	ImGuiWindow const* items = FindContentBrowserItems();
	ImVec2 const background(items->Pos.x + items->Size.x - 20.0f, items->Pos.y + items->Size.y - 20.0f);
	ImGui::GetIO().AddMousePosEvent(background.x, background.y);
	imgui.Frame(draw);
	fixture.Browser.DropFiles({outside / "AI"});
	imgui.Frame(draw);
	CHECK(FileSystem::IsRegularFile(fixture.GetScriptsDirectory() / "AI" / "Brain.cs"));
	CHECK(FileSystem::IsRegularFile(fixture.GetScriptsDirectory() / "AI" / "Steering" / "Seek.cs"));
	CHECK_FALSE(FileSystem::Exists(fixture.GetAssetDirectory() / "AI"));
	CHECK(fixture.Browser.GetRoot() == ContentRoot::Scripts);
	CHECK(fixture.Browser.GetNotice().empty());

	// A file that is neither an asset nor a script: the browser says so under its toolbar.
	fixture.Browser.DropFiles({outside / "Model.blend"});
	imgui.Frame(draw);
	std::string const notice = fixture.Browser.GetNotice();
	CHECK(notice.find("Model.blend") != std::string::npos);
	CHECK(notice.find("PNG") != std::string::npos);
	CHECK(CaptureText(imgui, draw).find("Model.blend") != std::string::npos);
	ImGuiWindow* bar = FindChildWindow("Content Browser", "##notice");
	REQUIRE(bar != nullptr);
	float const buttonsY = bar->DC.CursorStartPos.y + ImGui::GetTextLineHeight() * 0.5f;
	std::optional<ImVec2> const console = FindHovered(imgui, draw, GetRowPoints(bar, buttonsY), bar->GetID("Show Console"));
	REQUIRE(console.has_value());
	imgui.Click(*console, draw);
	CHECK(consoles == 1);
	bar = FindChildWindow("Content Browser", "##notice");
	REQUIRE(bar != nullptr);
	std::optional<ImVec2> const dismiss = FindHovered(imgui, draw, GetRowPoints(bar, buttonsY), bar->GetID("##dismiss"));
	REQUIRE(dismiss.has_value());
	imgui.Click(*dismiss, draw);
	CHECK(fixture.Browser.GetNotice().empty());
	imgui.Frame(draw);
	CHECK(FindChildWindow("Content Browser", "##notice") == nullptr);

	// The same folder again would declare its classes twice: refused, which the notice says; an import that goes well ends it.
	fixture.Browser.DropFiles({outside / "AI"});
	imgui.Frame(draw);
	CHECK(fixture.Browser.GetNotice().find("Brain.cs") != std::string::npos);
	CHECK_FALSE(FileSystem::Exists(fixture.GetScriptsDirectory() / "AI 1"));
	fixture.Browser.DropFiles({outside / "Oak.png"});
	imgui.Frame(draw);
	CHECK(fixture.Browser.GetNotice().empty());
	CHECK(AssetManager::FindByPath("Oak.png").IsValid());
	CHECK(fixture.Browser.GetRoot() == ContentRoot::Assets);

	// It goes by itself after a while without the mouse over it.
	fixture.Browser.DropFiles({outside / "Model.blend"});
	imgui.Frame(draw);
	REQUIRE_FALSE(fixture.Browser.GetNotice().empty());
	ImGui::GetIO().AddMousePosEvent(background.x, background.y);
	imgui.Frame(draw);
	CHECK_FALSE(fixture.Browser.GetNotice().empty());
	imgui.Frame(draw, static_cast<float>(ContentBrowserPanel::NoticeSeconds) + 1.0f);
	CHECK(fixture.Browser.GetNotice().empty());
}

TEST_CASE("Panels: what an import brings in is selected and scrolled into view")
{
	ContentBrowserFixture fixture;
	HeadlessImGui& imgui = fixture.Gui;
	// More scripts than the browser shows at once; the dropped one sorts last.
	for (int index = 0; index < 40; index++)
	{
		std::string const name = fmt::format("Script{:02}", index);
		REQUIRE(FileSystem::WriteTextFile(fixture.GetScriptsDirectory() / (name + ".cs"), "class " + name + " {}").IsOk());
	}
	std::filesystem::path const outside = fixture.Temporary.GetPath() / "Downloads";
	REQUIRE(FileSystem::WriteTextFile(outside / "Zeta.cs", "namespace Game; class Zeta {}").IsOk());
	fixture.Browser.SetScriptFolder("");
	std::function<void()> const draw = fixture.GetDraw();
	imgui.Frame(draw);
	imgui.Frame(draw);
	CHECK(FindContentBrowserItems()->Scroll.y == 0.0f);

	std::vector<std::filesystem::path> const dropped = {outside / "Zeta.cs"};
	ProjectImport::Report const report = fixture.Browser.ImportDropped(fixture.Operations, dropped, ContentRoot::Scripts, "");
	REQUIRE(report.Scripts.size() == 1);
	imgui.Frame(draw);
	imgui.Frame(draw);
	ImGuiWindow const* items = FindContentBrowserItems();
	CHECK(items->Scroll.y > 0.0f);
	// The last row, which holds it, is in view (below it is only the empty space that takes drops); selected, it is the
	// script F2 renames.
	float const rowHeight =
		std::round(ImGui::GetFontSize() * 5.5f) + ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y * 3.0f;
	CHECK(items->Scroll.y >= items->ScrollMax.y - rowHeight);
	imgui.Press(ImGuiKey_F2, draw);
	imgui.Frame(draw);
	imgui.Frame(draw);
	imgui.Type("Omega");
	imgui.Frame(draw);
	imgui.Press(ImGuiKey_Enter, draw);
	imgui.Frame(draw);
	CHECK(FileSystem::Exists(fixture.GetScriptsDirectory() / "Omega.cs"));
}

TEST_CASE("Panels: files of the asset directory that are not assets move when dragged onto folders")
{
	ContentBrowserFixture fixture;
	HeadlessImGui& imgui = fixture.Gui;
	REQUIRE(fixture.Operations.CreateAssetFolder("Models/Sub").IsOk());
	REQUIRE(FileSystem::WriteTextFile(fixture.GetAssetDirectory() / "Models" / "Crate.mtl", "newmtl Wood\n").IsOk());
	fixture.Browser.SetFolder("Models");
	std::function<void()> const draw = fixture.GetDraw();
	imgui.Frame(draw);
	imgui.Frame(draw);

	// The folder Sub comes first, then Crate.mtl.
	imgui.Drag(fixture.GetTile(1), fixture.GetTile(0), draw);
	CHECK(FileSystem::IsRegularFile(fixture.GetAssetDirectory() / "Models" / "Sub" / "Crate.mtl"));
	CHECK_FALSE(FileSystem::Exists(fixture.GetAssetDirectory() / "Models" / "Crate.mtl"));
	CHECK(fixture.BrowserOpen);
}

TEST_CASE("Panels: C# scripts dropped onto the scene view run on the entity under the cursor")
{
	ST_REQUIRE_GPU();
	Testing::ScriptEngineScope scripting;
	Testing::AssetManagerScope assets;
	REQUIRE(Renderer::Init().IsOk());
	{
		HeadlessImGui imgui;
		Testing::TemporaryDirectory temporary;
		EditorContext context;
		EditorOperations operations(context);
		std::filesystem::path const mover = temporary.GetPath() / "Mover.cs";
		REQUIRE(FileSystem::WriteTextFile(mover, "namespace Strada.Tests;\npublic class Mover : Strada.Script {}\n").IsOk());
		// A cube where the scene view looks.
		UUID const cube = Create(operations, "Cube",
		                         Json::object({{"Transform", Json::object({{"Translation", {0.0, 0.5, 0.0}}})},
		                                       {"Mesh", Json::object({{"Mesh", GetBuiltInHandle(BuiltInAsset::CubeMesh).ToString()}})}}));
		size_t const steps = context.GetHistory().GetUndoCount();

		ViewportPanel viewport;
		bool open = true;
		auto const draw = [&]
		{
			// The panel first begins ImGuizmo's frame, which takes SetNextWindowPos/Size for a window of its own: the scene view
			// is placed by name once it exists.
			ImGui::SetWindowPos("Scene View", ImVec2(0.0f, 0.0f));
			ImGui::SetWindowSize("Scene View", ImVec2(800.0f, 600.0f));
			viewport.OnImGuiRender(operations, open);
			// A script to drag, as the content browser's tiles are.
			ImGui::SetNextWindowPos(ImVec2(900.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
			ImGui::Begin("Scripts");
			ImGui::Button("Mover.cs", ImVec2(200.0f, 60.0f));
			UI::ScriptDragSource(mover, "Mover.cs");
			ImGui::End();
			// The GPU finishes every frame, so a pick requested in one arrives in the next, and the frame's resources are
			// recycled as the editor's frame loop does.
			GraphicsDevice::WaitForIdle();
			GraphicsDevice::EndFrame();
		};
		for (int frame = 0; frame < 3; frame++)
		{
			imgui.Frame(draw);
		}
		ImGuiWindow const* view = ImGui::FindWindowByName("Scene View");
		REQUIRE(view != nullptr);
		ImVec2 const source(1000.0f, 60.0f);
		auto const dropAt = [&](ImVec2 target)
		{
			imgui.Drag(source, target, draw);
			for (int frame = 0; frame < 4; frame++)
			{
				imgui.Frame(draw);
			}
		};

		// Onto empty space: nothing changes, and the console says why.
		uint64_t const firstLog = Log::GetNextEntryIndex();
		REQUIRE(view->Size.x == 800.0f);
		dropAt(ImVec2(view->Pos.x + 40.0f, view->Pos.y + view->Size.y - 40.0f));
		CHECK(Testing::WasLogged(firstLog, "drop it onto an entity"));
		CHECK_FALSE(context.GetScene().GetEntityByUUID(cube).HasComponent<ScriptComponent>());
		CHECK(context.GetHistory().GetUndoCount() == steps);

		// Onto the cube: it runs the class the file defines, as one undo step.
		dropAt(ImVec2(view->Pos.x + view->Size.x * 0.5f, view->Pos.y + view->Size.y * 0.5f));
		REQUIRE(context.GetScene().GetEntityByUUID(cube).HasComponent<ScriptComponent>());
		CHECK(context.GetScene().GetEntityByUUID(cube).GetComponent<ScriptComponent>().ClassName == "Strada.Tests.Mover");
		CHECK(context.GetHistory().GetUndoCount() == steps + 1);
		CHECK(open);
		GraphicsDevice::WaitForIdle();
	}
	Renderer::Shutdown();
}
