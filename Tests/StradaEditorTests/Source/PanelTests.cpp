#include "Editor/EditorOperations.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/Panels/SceneHierarchyPanel.h"
#include "Editor/Panels/SceneSettingsPanel.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_impl_null.h>

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

		void Press(ImGuiKey key)
		{
			ImGui::GetIO().AddKeyEvent(key, true);
			Frame([] {});
			ImGui::GetIO().AddKeyEvent(key, false);
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
