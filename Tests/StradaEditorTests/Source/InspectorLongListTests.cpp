#include "HeadlessImGui.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/PlayMode.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/ScriptFieldSerialization.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;
using Strada::Testing::HeadlessImGui;

// The inspector shows a long list while a game plays without reading, converting and drawing it whole at every draw
// (Docs/Architecture.md §11, "Long lists"): what is read back and converted is what changed, the rows drawn are those in
// view, and an edit is still made from what the game holds.
namespace
{
	constexpr float FrameTime = 1.0f / 60.0f;
	constexpr size_t LongList = MaxScriptFieldListElements;
	constexpr ImVec2 InspectorSize(500.0f, 800.0f);

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		variable.Value = std::move(value);
		variable.Public = true;
		return variable;
	}

	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object(), Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	// Samples (float[]) that no update changes, Clock (float) that each update advances by 1, and Steady (int).
	VisualScriptDocument MakeClockDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Samples", "float[]", Json::array()));
		document.Variables.push_back(MakeVariable(2, "Clock", "float", 0.0));
		document.Variables.push_back(MakeVariable(3, "Steady", "int", 7));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 2}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Operator.Add", Json::object(), Json::object({{"B", 1.0}})));
		document.Graph.Nodes.push_back(MakeNode(13, "Variable.Get", Json::object({{"Variable", 2}})));
		document.Graph.Links.push_back({{10, "Then"}, {11, "Execute"}});
		document.Graph.Links.push_back({{13, "Value"}, {12, "A"}});
		document.Graph.Links.push_back({{12, "Result"}, {11, "Value"}});
		return document;
	}

	// The graph of Tests/TestScripts' ListWriter: each update counts itself in Updates and in Clock, and writes Clock into
	// Samples[Updates % length], so that after update u its element u holds u.
	VisualScriptDocument MakeWriterDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Samples", "float[]", Json::array()));
		document.Variables.push_back(MakeVariable(2, "Updates", "int", 0));
		document.Variables.push_back(MakeVariable(3, "Clock", "float", 0.0));
		std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		nodes.push_back(MakeNode(10, "Event.Update"));
		nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 2}})));
		nodes.push_back(MakeNode(12, "Operator.Add", Json::object(), Json::object({{"B", 1}})));
		nodes.push_back(MakeNode(13, "Variable.Get", Json::object({{"Variable", 2}})));
		nodes.push_back(MakeNode(14, "Variable.Set", Json::object({{"Variable", 3}})));
		nodes.push_back(MakeNode(15, "Operator.Add", Json::object(), Json::object({{"B", 1.0}})));
		nodes.push_back(MakeNode(16, "Variable.Get", Json::object({{"Variable", 3}})));
		nodes.push_back(MakeNode(17, "Variable.Set", Json::object({{"Variable", 1}})));
		nodes.push_back(MakeNode(18, "Array.Set"));
		nodes.push_back(MakeNode(19, "Variable.Get", Json::object({{"Variable", 1}})));
		nodes.push_back(MakeNode(20, "Operator.Modulo"));
		nodes.push_back(MakeNode(21, "Array.Length"));
		links.push_back({{10, "Then"}, {11, "Execute"}});
		links.push_back({{13, "Value"}, {12, "A"}});
		links.push_back({{12, "Result"}, {11, "Value"}});
		links.push_back({{11, "Then"}, {14, "Execute"}});
		links.push_back({{16, "Value"}, {15, "A"}});
		links.push_back({{15, "Result"}, {14, "Value"}});
		links.push_back({{14, "Then"}, {17, "Execute"}});
		links.push_back({{19, "Value"}, {18, "Array"}});
		links.push_back({{13, "Value"}, {20, "A"}});
		links.push_back({{19, "Value"}, {21, "Array"}});
		links.push_back({{21, "Length"}, {20, "B"}});
		links.push_back({{20, "Result"}, {18, "Index"}});
		links.push_back({{16, "Value"}, {18, "Value"}});
		links.push_back({{18, "Result"}, {17, "Value"}});
		return document;
	}

	// Element i holds start + i.
	ScriptFieldValue MakeSamples(size_t count, float start)
	{
		std::vector<ScriptFieldValue> elements;
		elements.reserve(count);
		for (size_t i = 0; i < count; i++)
		{
			elements.push_back(ScriptFieldValue::FromFloat(start + static_cast<float>(i)));
		}
		return ScriptFieldValue::FromList(ScriptFieldType::Float, std::move(elements));
	}

	Json StoredSamples(size_t count, float start)
	{
		return Json::object({{"Samples", JsonTraits<ScriptFieldMap>::ToJson({{"Samples", MakeSamples(count, start)}})["Samples"]}});
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

	// Moves the mouse along the line, a frame a step of three pixels, until the item is hovered; its point then.
	std::optional<ImVec2> FindItem(HeadlessImGui& imgui, ImGuiID item, ImVec2 from, ImVec2 to, std::function<void()> const& draw)
	{
		float const distance = std::max(std::abs(to.x - from.x), std::abs(to.y - from.y));
		int const steps = std::max(1, static_cast<int>(distance / 3.0f));
		for (int step = 0; step <= steps; step++)
		{
			float const t = static_cast<float>(step) / static_cast<float>(steps);
			ImGui::GetIO().AddMousePosEvent(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
			imgui.Frame(draw);
			if (GImGui->HoveredId == item)
			{
				return ImVec2(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
			}
		}
		return std::nullopt;
	}

	// The inspector over the playing scene, drawn at a fixed place and size, scrolled where asked, with the rows of the list
	// field it shows (its ID scope in the window: the component's section, then its table).
	struct InspectorView
	{
		InspectorView(HeadlessImGui& gui, EditorOperations& operations, std::string component, std::string section)
			: Gui(gui),
			  Operations(operations),
			  Component(std::move(component)),
			  Section(std::move(section))
		{
			Draw = [this]
			{
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(InspectorSize);
				if (Scroll)
				{
					ImGui::SetNextWindowScroll(ImVec2(-1.0f, *Scroll));
				}
				Panel.OnImGuiRender(Operations, Open);
			};
		}

		void Frame() { Gui.Frame(Draw); }

		// Scrolled there in one frame and drawn there in the next, which counts the rows of the view it settled in.
		void ScrollTo(float y)
		{
			Scroll = y;
			Frame();
			Scroll.reset();
			Frame();
		}

		ImGuiWindow* GetWindow() const
		{
			ImGuiWindow* window = ImGui::FindWindowByName("Inspector");
			REQUIRE(window != nullptr);
			return window;
		}

		ImGuiID GetTable() const
		{
			ImGuiID scope = ImHashStr(Component.c_str(), 0, GetWindow()->ID);
			if (!Section.empty())
			{
				scope = ImHashStr(Section.c_str(), 0, scope);
			}
			return ImHashStr("##fields", 0, scope);
		}

		ImGuiID GetElementValue(int index) const
		{
			ImGuiID const element = ImHashData(&index, sizeof(index), ImHashStr("Samples", 0, GetTable()));
			return ImHashStr("##value", 0, element);
		}

		// The point of an element's value widget, which must be in view.
		ImVec2 FindElementValue(int index)
		{
			ImGuiTable const* table = ImGui::TableFindByID(GetTable());
			REQUIRE(table != nullptr);
			float const x = table->Columns[1].WorkMinX + 12.0f;
			ImGuiWindow const* window = GetWindow();
			std::optional<ImVec2> const point = FindItem(Gui, GetElementValue(index), ImVec2(x, window->Pos.y + 2.0f),
			                                             ImVec2(x, window->Pos.y + window->Size.y - 2.0f), Draw);
			REQUIRE(point);
			return *point;
		}

		// The rows the window's view holds, partly shown ones counted as parts.
		float GetRowsInView() const { return GetWindow()->InnerClipRect.GetHeight() / ImGui::GetFrameHeightWithSpacing(); }

		// The list's rows were drawn for the view alone (one more on each side, and partly shown ones), the rest clipped.
		void CheckRowsInView(size_t count) const
		{
			FieldEditorDrawStatistics const& rows = Panel.GetFieldDrawStatistics();
			CHECK(rows.ArrayRowsDrawn + rows.ArrayRowsClipped == count);
			CHECK(rows.ArrayRowsDrawn >= 10);
			CHECK(static_cast<float>(rows.ArrayRowsDrawn) <= GetRowsInView() + 4.0f);
		}

		HeadlessImGui& Gui;
		EditorOperations& Operations;
		std::string Component;
		std::string Section;
		InspectorPanel Panel;
		bool Open = true;
		std::optional<float> Scroll;
		std::function<void()> Draw;
	};

	// How a case plays a list: a graph's public array (Variables of a VisualScript component, drawn in its variables
	// section) or a C# script's List<float> (Fields of a Script component).
	struct ListOwner
	{
		bool VisualScript = true;

		std::string GetComponent() const { return VisualScript ? "VisualScript" : "Script"; }
		std::string GetSection() const { return VisualScript ? "##variables" : ""; }

		ScriptFieldMap const& GetStored(Scene& scene, UUID entity) const
		{
			Entity const found = scene.GetEntityByUUID(entity);
			return VisualScript ? found.GetComponent<VisualScriptComponent>().Variables : found.GetComponent<ScriptComponent>().Fields;
		}

		// Another reader of the scene's running values (as entity.get and component.get are), which takes what changed into
		// the component before the inspector's next draw does.
		void ReadAll(Scene& scene, UUID entity) const
		{
			Entity const found = scene.GetEntityByUUID(entity);
			Result<ScriptFieldMap> const read = VisualScript ? scene.ReadVisualScriptVariables(found) : scene.ReadScriptFields(found);
			REQUIRE_MESSAGE(read.IsOk(), read.GetError());
		}

		// What the running instance holds.
		ScriptFieldValue ReadRunning(UUID entity, std::string const& name) const
		{
			Result<ScriptFieldMap> values =
				VisualScript ? VisualScriptEngine::GetInstanceVariables(entity) : ScriptEngine::GetInstanceFields(entity);
			REQUIRE_MESSAGE(values.IsOk(), values.GetError());
			REQUIRE(values.GetValue().contains(name));
			return values.GetValue().at(name);
		}
	};

	// The rows drawn and the values read back and converted with the list at its start, scrolled to its middle and to its
	// end, each the same from one draw to the next while the game holds still. changedPerUpdate: the values an update
	// changes.
	void CheckLongListInView(HeadlessImGui& imgui, EditorOperations& operations, PlayMode& play, ListOwner const& owner, UUID entity,
	                         size_t valueCount, size_t changedPerUpdate)
	{
		InspectorView view(imgui, operations, owner.GetComponent(), owner.GetSection());
		std::array<UUID, 1> const selection = {entity};
		REQUIRE(operations.Select(selection).IsOk());
		play.Advance(1, Timestep(FrameTime));

		// The first draw reads and converts every value; the next, with nothing changed, none.
		view.Frame();
		CHECK(view.Panel.GetDrawStatistics().ValuesReadBack == valueCount);
		CHECK(view.Panel.GetDrawStatistics().ValuesConverted == valueCount);
		view.CheckRowsInView(LongList);
		view.Frame();
		CHECK(view.Panel.GetDrawStatistics().ValuesReadBack == 0);
		CHECK(view.Panel.GetDrawStatistics().ValuesConverted == 0);
		view.CheckRowsInView(LongList);
		// An update changes what it changes alone.
		play.Advance(1, Timestep(FrameTime));
		view.Frame();
		CHECK(view.Panel.GetDrawStatistics().ValuesReadBack == changedPerUpdate);
		CHECK(view.Panel.GetDrawStatistics().ValuesConverted == changedPerUpdate);

		// Scrolled to the middle and to the end: the rows of the view, the values as they were.
		ImGuiWindow const* window = view.GetWindow();
		REQUIRE(window->ScrollMax.y > static_cast<float>(LongList) * ImGui::GetFrameHeight());
		view.ScrollTo(window->ScrollMax.y * 0.5f);
		CHECK(window->Scroll.y == doctest::Approx(window->ScrollMax.y * 0.5f).epsilon(0.01));
		view.CheckRowsInView(LongList);
		CHECK(view.Panel.GetDrawStatistics().ValuesConverted == 0);
		view.ScrollTo(1.0e9f);
		CHECK(window->Scroll.y == window->ScrollMax.y);
		view.CheckRowsInView(LongList);
		CHECK(view.Panel.GetDrawStatistics().ValuesReadBack == 0);
		CHECK(view.Panel.GetDrawStatistics().ValuesConverted == 0);
		// The last element's row is drawn where it is with every row drawn: at the end of the scrolled view.
		ImGuiTable const* table = ImGui::TableFindByID(view.GetTable());
		REQUIRE(table != nullptr);
		float const x = table->Columns[1].WorkMinX + 12.0f;
		CHECK(FindItem(imgui, view.GetElementValue(static_cast<int>(LongList) - 1), ImVec2(x, window->Pos.y + window->Size.y * 0.5f),
		               ImVec2(x, window->Pos.y + window->Size.y - 2.0f), view.Draw)
		          .has_value());
		CHECK(operations.GetContext().GetHistory().GetUndoCount() == 0);
	}

	// An element dragged while the game writes other elements of the list on every frame: each frame's edit writes the list
	// the game holds then with the element changed, so no element the game wrote meanwhile goes back (also when another
	// reader took the change into the component first), and undoing the edit gives back what the game held. Element u holds
	// u after update u, 100 + i before.
	void CheckEditWhileTheGameWrites(HeadlessImGui& imgui, EditorOperations& operations, PlayMode& play, ListOwner const& owner,
	                                 UUID entity)
	{
		InspectorView view(imgui, operations, owner.GetComponent(), owner.GetSection());
		std::array<UUID, 1> const selection = {entity};
		REQUIRE(operations.Select(selection).IsOk());
		play.Advance(5, Timestep(FrameTime));
		view.Frame();
		constexpr int Edited = 15;
		ImVec2 const point = view.FindElementValue(Edited);

		ImGuiIO& io = ImGui::GetIO();
		io.AddMousePosEvent(point.x, point.y);
		view.Frame();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		view.Frame();
		for (int step = 1; step <= 3; step++)
		{
			play.Advance(1, Timestep(FrameTime));
			if (step == 2)
			{
				owner.ReadAll(operations.GetContext().GetScene(), entity);
			}
			io.AddMousePosEvent(point.x + 15.0f * static_cast<float>(step), point.y);
			view.Frame();
		}
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		view.Frame();
		view.Frame();

		// One undo step; the running list is the game's with the element changed.
		EditorContext& context = operations.GetContext();
		CHECK(context.GetHistory().GetUndoCount() == 1);
		ScriptFieldValue const running = owner.ReadRunning(entity, "Samples");
		REQUIRE(running.GetElements().size() == LongList);
		CHECK(owner.GetStored(context.GetScene(), entity).at("Samples") == running);
		for (size_t i = 1; i <= 8; i++)
		{
			CAPTURE(i);
			CHECK(running.GetElements()[i] == ScriptFieldValue::FromFloat(static_cast<float>(i)));
		}
		CHECK(running.GetElements()[Edited].GetFloat() > 100.0f + static_cast<float>(Edited));
		CHECK(running.GetElements()[100] == ScriptFieldValue::FromFloat(200.0f));

		// Undone, the element is what the game held, and so are those it wrote before the edit.
		REQUIRE(operations.Undo().IsOk());
		ScriptFieldValue const undone = owner.ReadRunning(entity, "Samples");
		REQUIRE(undone.GetElements().size() == LongList);
		CHECK(undone.GetElements()[Edited] == ScriptFieldValue::FromFloat(100.0f + static_cast<float>(Edited)));
		for (size_t i = 1; i <= 5; i++)
		{
			CAPTURE(i);
			CHECK(undone.GetElements()[i] == ScriptFieldValue::FromFloat(static_cast<float>(i)));
		}
		view.Frame();
		CHECK(owner.GetStored(context.GetScene(), entity).at("Samples") == undone);
	}
}

TEST_CASE("Inspector: a graph's list of 4096 elements is drawn for the rows in view and converted only where it changed")
{
	Testing::ScriptRuntimeScope scripting;
	Testing::AssetManagerScope const assets;
	HeadlessImGui imgui;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll");
	PlayMode play(context, scripts);
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeClockDocument()), "Clock");
	UUID const entity =
		Create(operations, "Clock",
	           Json::object({{"VisualScript", Json::object({{"Graph", graph.ToString()}, {"Variables", StoredSamples(LongList, 0.0f)}})}}));
	REQUIRE(play.Start(EditorPlayState::Play, 640, 480).IsOk());
	// Samples, Clock and Steady; an update changes the clock alone.
	CheckLongListInView(imgui, operations, play, ListOwner{true}, entity, 3, 1);
	play.Stop();
}

TEST_CASE("Inspector: a C# script's list of 4096 elements is drawn for the rows in view and converted only where it changed")
{
	Testing::ScriptEngineScope scripting;
	HeadlessImGui imgui;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll");
	PlayMode play(context, scripts);
	UUID const entity = Create(
		operations, "Writer",
		Json::object({{"Script", Json::object({{"ClassName", "Strada.Tests.ListWriter"}, {"Fields", StoredSamples(LongList, 100.0f)}})}}));
	REQUIRE(play.Start(EditorPlayState::Play, 640, 480).IsOk());
	// Samples, Updates, Length and Steady; an update changes Updates and an element of Samples.
	CheckLongListInView(imgui, operations, play, ListOwner{false}, entity, 4, 2);
	play.Stop();
}

TEST_CASE("Inspector: an element of a graph's list edited while the graph writes others writes the list the graph holds")
{
	Testing::ScriptRuntimeScope scripting;
	Testing::AssetManagerScope const assets;
	HeadlessImGui imgui;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll");
	PlayMode play(context, scripts);
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeWriterDocument()), "Writer");
	UUID const entity = Create(
		operations, "Writer",
		Json::object({{"VisualScript", Json::object({{"Graph", graph.ToString()}, {"Variables", StoredSamples(LongList, 100.0f)}})}}));
	REQUIRE(play.Start(EditorPlayState::Play, 640, 480).IsOk());
	CheckEditWhileTheGameWrites(imgui, operations, play, ListOwner{true}, entity);
	play.Stop();
}

TEST_CASE("Inspector: an element of a C# script's list edited while the script writes others writes the list the script holds")
{
	Testing::ScriptEngineScope scripting;
	HeadlessImGui imgui;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll");
	PlayMode play(context, scripts);
	UUID const entity = Create(
		operations, "Writer",
		Json::object({{"Script", Json::object({{"ClassName", "Strada.Tests.ListWriter"}, {"Fields", StoredSamples(LongList, 100.0f)}})}}));
	REQUIRE(play.Start(EditorPlayState::Play, 640, 480).IsOk());
	CheckEditWhileTheGameWrites(imgui, operations, play, ListOwner{false}, entity);
	play.Stop();
}

TEST_CASE("Inspector: a list that grows and shrinks while it is shown is drawn at its length")
{
	Testing::ScriptEngineScope scripting;
	Testing::AssetManagerScope const assets;
	HeadlessImGui imgui;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll");
	PlayMode play(context, scripts);
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeClockDocument()), "Clock");
	UUID const writer = Create(operations, "Writer", Json::object({{"Script", Json::object({{"ClassName", "Strada.Tests.ListWriter"}})}}));
	UUID const clock = Create(operations, "Clock", Json::object({{"VisualScript", Json::object({{"Graph", graph.ToString()}})}}));
	REQUIRE(play.Start(EditorPlayState::Play, 640, 480).IsOk());
	Scene& scene = context.GetScene();

	// The script resizes its list to Length on every update.
	{
		InspectorView view(imgui, operations, "Script", "");
		std::array<UUID, 1> const selection = {writer};
		REQUIRE(operations.Select(selection).IsOk());
		auto const resize = [&](int32_t length)
		{
			scene.GetEntityByUUID(writer).GetComponent<ScriptComponent>().Fields["Length"] = ScriptFieldValue::FromInt32(length);
			scene.ApplyScriptFieldChanges();
			play.Advance(1, Timestep(FrameTime));
			view.Frame();
		};
		resize(3000);
		view.CheckRowsInView(3000);
		CHECK(view.Panel.GetDrawStatistics().ValuesConverted >= 2);
		view.ScrollTo(1.0e9f);
		view.CheckRowsInView(3000);
		// Shrunk while scrolled to its end: every row shows.
		resize(12);
		view.Frame();
		CHECK(view.Panel.GetFieldDrawStatistics().ArrayRowsDrawn == 12);
		CHECK(view.Panel.GetFieldDrawStatistics().ArrayRowsClipped == 0);
		resize(static_cast<int32_t>(LongList));
		view.ScrollTo(0.0f);
		view.CheckRowsInView(LongList);
		CHECK(ScriptEngine::GetInstanceFields(writer).GetValue().at("Samples").GetElements().size() == LongList);
	}

	// A graph's list given another length (as an edit, a paste or the graph itself would).
	{
		InspectorView view(imgui, operations, "VisualScript", "##variables");
		std::array<UUID, 1> const selection = {clock};
		REQUIRE(operations.Select(selection).IsOk());
		auto const resize = [&](size_t length)
		{
			scene.GetEntityByUUID(clock).GetComponent<VisualScriptComponent>().Variables["Samples"] = MakeSamples(length, 0.0f);
			scene.ApplyVisualScriptVariableChanges();
			play.Advance(1, Timestep(FrameTime));
			view.Frame();
		};
		resize(LongList);
		view.CheckRowsInView(LongList);
		resize(20);
		CHECK(view.Panel.GetFieldDrawStatistics().ArrayRowsDrawn == 20);
		CHECK(view.Panel.GetFieldDrawStatistics().ArrayRowsClipped == 0);
		resize(1000);
		view.CheckRowsInView(1000);
	}
	CHECK(context.GetHistory().GetUndoCount() == 0);
	play.Stop();
}

TEST_CASE("FieldEditor: rows of a long list out of view keep their space, the active one stays drawn, and multiline texts are drawn whole")
{
	HeadlessImGui imgui;
	FieldEditor editor;
	FieldDescriptor samples;
	samples.Name = "Samples";
	samples.Kind = FieldKind::Array;
	samples.ElementKind = FieldKind::Float;
	samples.Hints.MaxCount = LongList;
	std::array<FieldDescriptor, 1> const fields = {samples};
	Json values = Json::object({{"Samples", Json::array()}});
	for (int i = 0; i < 200; i++)
	{
		values["Samples"].push_back(static_cast<double>(i));
	}

	float height = 6000.0f;
	std::optional<float> scroll;
	std::optional<FieldChange> last;
	float end = 0.0f;
	std::function<void()> const draw = [&]
	{
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(500.0f, height));
		if (scroll)
		{
			ImGui::SetNextWindowScroll(ImVec2(-1.0f, *scroll));
		}
		ImGui::Begin("Fields", nullptr, ImGuiWindowFlags_NoSavedSettings);
		if (std::optional<FieldChange> change = editor.Draw(fields, values))
		{
			last = std::move(change);
		}
		end = ImGui::GetCursorPosY();
		ImGui::End();
	};

	// Drawn whole in a window taller than the list (on a display as tall), and clipped in a short one: the list ends at the
	// same place.
	imgui.SetDisplaySize(ImVec2(1920.0f, height));
	imgui.Frame(draw);
	imgui.Frame(draw);
	CHECK(editor.GetDrawStatistics().ArrayRowsDrawn == 200);
	CHECK(editor.GetDrawStatistics().ArrayRowsClipped == 0);
	float const wholeEnd = end;
	imgui.SetDisplaySize(std::nullopt);
	height = 300.0f;
	imgui.Frame(draw);
	imgui.Frame(draw);
	CHECK(editor.GetDrawStatistics().ArrayRowsDrawn < 20);
	CHECK(editor.GetDrawStatistics().ArrayRowsDrawn + editor.GetDrawStatistics().ArrayRowsClipped == 200);
	CHECK(end == wholeEnd);

	// A row's widget held active stays drawn and active while the list scrolls it out of view, and its edit goes on.
	ImGuiWindow const* window = ImGui::FindWindowByName("Fields");
	REQUIRE(window != nullptr);
	ImGuiID const table = ImHashStr("##fields", 0, window->ID);
	int const index = 2;
	ImGuiID const value = ImHashStr("##value", 0, ImHashData(&index, sizeof(index), ImHashStr("Samples", 0, table)));
	ImGuiTable const* found = ImGui::TableFindByID(table);
	REQUIRE(found != nullptr);
	float const x = found->Columns[1].WorkMinX + 12.0f;
	std::optional<ImVec2> const point = FindItem(imgui, value, ImVec2(x, 2.0f), ImVec2(x, 298.0f), draw);
	REQUIRE(point);
	ImGuiIO& io = ImGui::GetIO();
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	imgui.Frame(draw);
	REQUIRE(GImGui->ActiveId == value);
	float const rowsInView = window->InnerClipRect.GetHeight() / ImGui::GetFrameHeightWithSpacing();
	scroll = 1.0e9f;
	imgui.Frame(draw);
	scroll.reset();
	imgui.Frame(draw);
	CHECK(window->Scroll.y == window->ScrollMax.y);
	CHECK(GImGui->ActiveId == value);
	// The rows of the view at the list's end, and the active one.
	CHECK(static_cast<float>(editor.GetDrawStatistics().ArrayRowsDrawn) <= rowsInView + 5.0f);
	CHECK(end == wholeEnd);
	last.reset();
	io.AddMousePosEvent(point->x + 30.0f, point->y);
	imgui.Frame(draw);
	REQUIRE(last);
	CHECK(last->Path == std::vector<FieldPathStep>{{"Samples", 2u}});
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	imgui.Frame(draw);
	imgui.Frame(draw);
	CHECK(GImGui->ActiveId == 0);

	// Rows of multiline texts are taller than a frame: their array is drawn whole.
	std::array<FieldDescriptor, 1> texts = {samples};
	texts[0].ElementKind = FieldKind::String;
	texts[0].Hints.Display = FieldDisplay::MultilineText;
	Json lines = Json::object({{"Samples", Json::array()}});
	for (int i = 0; i < 50; i++)
	{
		lines["Samples"].push_back("line");
	}
	std::function<void()> const drawTexts = [&]
	{
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(500.0f, 300.0f));
		ImGui::Begin("Texts", nullptr, ImGuiWindowFlags_NoSavedSettings);
		static_cast<void>(editor.Draw(texts, lines));
		ImGui::End();
	};
	imgui.Frame(drawTexts);
	imgui.Frame(drawTexts);
	CHECK(editor.GetDrawStatistics().ArrayRowsDrawn == 50);
	CHECK(editor.GetDrawStatistics().ArrayRowsClipped == 0);
}

TEST_CASE("FieldEditor: the row of a long list whose asset picker is open stays drawn while the list scrolls it out of view, and the "
          "picker with it")
{
	HeadlessImGui imgui;
	FieldEditor editor;
	FieldDescriptor models;
	models.Name = "Models";
	models.Kind = FieldKind::Array;
	models.ElementKind = FieldKind::Asset;
	models.Hints.AssetTypeName = "Mesh";
	models.Hints.MaxCount = LongList;
	std::array<FieldDescriptor, 1> const fields = {models};
	Json values = Json::object({{"Models", Json::array()}});
	for (int i = 0; i < 200; i++)
	{
		values["Models"].push_back(UUID::Invalid().ToString());
	}

	std::optional<float> scroll;
	// The picker of element 2, opened as its "..." button opens it: in the row's ID scope.
	ImGuiID rowScope = 0;
	ImGuiID picker = 0;
	bool openPicker = false;
	bool deactivate = false;
	bool closePopups = false;
	std::function<void()> const draw = [&]
	{
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(500.0f, 300.0f));
		if (scroll)
		{
			ImGui::SetNextWindowScroll(ImVec2(-1.0f, *scroll));
		}
		ImGui::Begin("Fields", nullptr, ImGuiWindowFlags_NoSavedSettings);
		if (std::exchange(openPicker, false))
		{
			ImGui::PushOverrideID(rowScope);
			ImGui::OpenPopupEx(picker);
			ImGui::PopID();
		}
		if (std::exchange(deactivate, false))
		{
			ImGui::ClearActiveID();
		}
		if (std::exchange(closePopups, false))
		{
			ImGui::ClosePopupsExceptModals();
		}
		static_cast<void>(editor.Draw(fields, values));
		ImGui::End();
	};
	imgui.Frame(draw);
	imgui.Frame(draw);
	ImGuiWindow const* window = ImGui::FindWindowByName("Fields");
	REQUIRE(window != nullptr);
	CHECK(editor.GetDrawStatistics().ArrayRowsDrawn < 20);
	int const index = 2;
	rowScope = ImHashData(&index, sizeof(index), ImHashStr("Models", 0, ImHashStr("##fields", 0, window->ID)));
	picker = ImHashStr("##assetPicker", 0, rowScope);
	auto const isPickerShown = [picker]
	{
		ImGuiContext const& context = *GImGui;
		return ImGui::IsPopupOpen(picker, ImGuiPopupFlags_AnyPopupLevel) && context.OpenPopupStack.Size == 1 &&
		       context.OpenPopupStack[0].Window != nullptr && context.OpenPopupStack[0].Window->LastFrameActive == context.FrameCount;
	};
	openPicker = true;
	imgui.Frame(draw);
	imgui.Frame(draw);
	REQUIRE(isPickerShown());
	// Its search field takes the keyboard as it opens (a frame or two after it shows), which keeps the row drawn as the active
	// one; a click elsewhere in the picker (its list's scrollbar, its margin) gives that up, and the picker stays open.
	for (int frame = 0; frame < 3; frame++)
	{
		imgui.Frame(draw);
	}
	REQUIRE(GImGui->ActiveId == ImHashStr("##search", 0, GImGui->OpenPopupStack[0].Window->ID));
	deactivate = true;
	imgui.Frame(draw);
	imgui.Frame(draw);
	REQUIRE(GImGui->ActiveId == 0);
	REQUIRE(isPickerShown());

	// Scrolled to the list's end, far from the row: the row is drawn beside the view's, and submits its picker.
	scroll = 1.0e9f;
	imgui.Frame(draw);
	scroll.reset();
	imgui.Frame(draw);
	CHECK(window->Scroll.y == window->ScrollMax.y);
	CHECK(isPickerShown());
	size_t const drawnWithPicker = editor.GetDrawStatistics().ArrayRowsDrawn;
	float const rowsInView = window->InnerClipRect.GetHeight() / ImGui::GetFrameHeightWithSpacing();
	CHECK(static_cast<float>(drawnWithPicker) <= rowsInView + 5.0f);
	imgui.Frame(draw);
	CHECK(isPickerShown());

	// Closed, the row is no longer drawn out of view.
	closePopups = true;
	imgui.Frame(draw);
	imgui.Frame(draw);
	CHECK_FALSE(ImGui::IsPopupOpen(picker, ImGuiPopupFlags_AnyPopupLevel));
	CHECK(editor.GetDrawStatistics().ArrayRowsDrawn == drawnWithPicker - 1);
	CHECK(editor.GetDrawStatistics().ArrayRowsDrawn + editor.GetDrawStatistics().ArrayRowsClipped == 200);
}
