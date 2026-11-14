#include "Asset/TestDataAsset.h"
#include "EditorLocationsTestUtilities.h"
#include "HeadlessImGui.h"
#include "TestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/EditorOperations.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/Panels/ProjectSettingsPanel.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;
using Strada::Testing::HeadlessImGui;

namespace
{
	enum class TrackKind : uint8_t
	{
		Road = 0,
		Oval
	};

	// An element struct with a visibility condition and an array of its own.
	struct Track
	{
		std::string Name;
		TrackKind Kind = TrackKind::Road;
		float Grip = 1.0f;
		float Banking = 0.0f;
		std::vector<std::string> Tags;
	};

	struct Circuit
	{
		std::string Title;
		std::vector<Track> Tracks;
	};
}

template<>
struct Strada::EnumTraits<TrackKind>
{
	static constexpr std::array<std::pair<TrackKind, std::string_view>, 2> Values = {{
		{TrackKind::Road, "Road"},
		{TrackKind::Oval, "Oval"},
	}};
};

template<>
struct Strada::StructTraits<Track>
{
	static constexpr std::string_view Name = "Track";
	static constexpr auto Fields =
		std::make_tuple(Field("Name", &Track::Name), Field("Kind", &Track::Kind), Field("Grip", &Track::Grip).Range(0.0, 2.0),
	                    Field("Banking", &Track::Banking).VisibleWhen("Kind", "Oval"), Field("Tags", &Track::Tags).MaxCount(2));
};

template<>
struct Strada::StructTraits<Circuit>
{
	static constexpr std::string_view Name = "Circuit";
	static constexpr auto Fields = std::make_tuple(Field("Title", &Circuit::Title), Field("Tracks", &Circuit::Tracks).MaxCount(3));
};

namespace
{
	Json MakeTrack(std::string name, std::string kind, double grip, Json tags = Json::array())
	{
		return Json::object(
			{{"Name", std::move(name)}, {"Kind", std::move(kind)}, {"Grip", grip}, {"Banking", 0.0}, {"Tags", std::move(tags)}});
	}

	FieldChange MakeChange(std::vector<FieldPathStep> path, Json value, std::optional<FieldArrayEdit> edit = std::nullopt)
	{
		FieldChange change;
		change.Path = std::move(path);
		change.Value = std::move(value);
		change.ArrayEdit = edit;
		return change;
	}

	// Moves the mouse down the window at x, a frame per step, and returns the first point where the item with the ID is
	// hovered.
	std::optional<ImVec2> FindItemAt(HeadlessImGui& imgui, ImGuiID item, ImGuiWindow const* window, float x,
	                                 std::function<void()> const& draw)
	{
		REQUIRE(window != nullptr);
		for (float y = window->Pos.y + 2.0f; y < window->Pos.y + window->Size.y - 2.0f; y += 4.0f)
		{
			ImGui::GetIO().AddMousePosEvent(x, y);
			imgui.Frame(draw);
			if (GImGui->HoveredId == item)
			{
				return ImVec2(x, y);
			}
		}
		return std::nullopt;
	}

	ImGuiID GetIntId(int index, ImGuiID seed)
	{
		return ImHashData(&index, sizeof(index), seed);
	}

	// Where a column of a table drawn last frame starts its content, plus an offset.
	float GetColumnX(ImGuiID tableId, int column, float offset)
	{
		ImGuiTable const* table = ImGui::TableFindByID(tableId);
		REQUIRE(table != nullptr);
		REQUIRE(column < table->ColumnsCount);
		return table->Columns[column].WorkMinX + offset;
	}

	// The x of the buttons at the right end of a field table's value column ("-" and "+" of plain arrays, the menu of
	// elements).
	float GetButtonX(ImGuiID tableId)
	{
		ImGuiTable const* table = ImGui::TableFindByID(tableId);
		REQUIRE(table != nullptr);
		return table->Columns[1].WorkMaxX - ImGui::GetFrameHeight() * 0.5f;
	}

	// Drives the rows of a field table: the table's ID (in a window drawn by draw) and the IDs of its arrays' parts.
	struct FieldTableDriver
	{
		HeadlessImGui& Gui;
		std::function<void()> Draw;
		std::function<ImGuiWindow const*()> Window;
		std::function<ImGuiID()> Table;

		ImGuiID GetFieldId(std::string_view name, ImGuiID scope = 0) const
		{
			return ImHashStr(name.data(), name.size(), scope != 0 ? scope : Table());
		}
		ImGuiID GetElementId(ImGuiID array, int index) const { return GetIntId(index, array); }

		std::optional<ImVec2> Find(ImGuiID item, float x) const { return FindItemAt(Gui, item, Window(), x, Draw); }

		void Click(ImGuiID item, float x) const
		{
			std::optional<ImVec2> const point = Find(item, x);
			REQUIRE(point);
			Gui.Click(*point, Draw);
		}

		float GetNameX() const { return GetColumnX(Table(), 0, ImGui::GetFontSize() * 3.0f); }
		float GetValueX() const { return GetColumnX(Table(), 1, ImGui::GetFrameHeight()); }
		// The x of a check box, and of the "+" of a plain array: at the start of the value column.
		float GetValueStartX() const { return GetColumnX(Table(), 1, ImGui::GetFrameHeight() * 0.5f); }
		float GetButtonX() const { return ::GetButtonX(Table()); }

		// Folds an element's row open.
		void OpenElement(ImGuiID array, int index) const { Click(ImHashStr("##element", 0, GetElementId(array, index)), GetNameX()); }

		// Opens an element's menu with its button and picks an item of the menu.
		void PickFromMenu(ImGuiID array, int index, char const* item) const
		{
			Click(ImHashStr("...", 0, GetElementId(array, index)), GetButtonX());
			REQUIRE(GImGui->OpenPopupStack.Size == 1);
			ImGuiWindow const* popup = GImGui->OpenPopupStack.back().Window;
			REQUIRE(popup != nullptr);
			std::optional<ImVec2> const point =
				FindItemAt(Gui, ImHashStr(item, 0, popup->ID), popup, popup->Pos.x + popup->Size.x * 0.5f, Draw);
			REQUIRE(point);
			Gui.Click(*point, Draw);
		}

		// The state of a menu item of an element's menu, which is left open: whether it is enabled.
		bool IsMenuItemEnabled(ImGuiID array, int index, char const* item) const
		{
			Click(ImHashStr("...", 0, GetElementId(array, index)), GetButtonX());
			REQUIRE(GImGui->OpenPopupStack.Size == 1);
			ImGuiWindow const* popup = GImGui->OpenPopupStack.back().Window;
			REQUIRE(popup != nullptr);
			REQUIRE(FindItemAt(Gui, ImHashStr(item, 0, popup->ID), popup, popup->Pos.x + popup->Size.x * 0.5f, Draw));
			bool const enabled = !GImGui->HoveredIdIsDisabled;
			Gui.Press(ImGuiKey_Escape, Draw);
			return enabled;
		}

		// Clicks a text field, replaces its text and leaves it with Enter.
		void TypeInto(ImGuiID item, char const* text) const
		{
			Click(item, GetValueX());
			Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, Draw);
			Gui.Type(text);
			Gui.Frame(Draw);
			Gui.Press(ImGuiKey_Enter, Draw);
			Gui.Frame(Draw);
		}
	};

	// A field editor drawing a Circuit in a window of its own; changes are applied to the values as panels apply them.
	struct CircuitFixture
	{
		HeadlessImGui Gui;
		FieldEditor Editor;
		std::vector<FieldDescriptor> const Fields = GetFieldDescriptors<StructTraits<Circuit>, Circuit>();
		Json Values;
		std::vector<FieldChange> Changes;
		std::string Text;
		bool LogText = false;
		std::function<void()> Draw = [this]
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(600.0f, 1000.0f));
			ImGui::Begin("Circuit", nullptr, ImGuiWindowFlags_NoSavedSettings);
			if (LogText)
			{
				ImGui::LogToBuffer();
			}
			if (std::optional<FieldChange> change = Editor.Draw(Fields, Values))
			{
				Values.update(change->MakePatch(Values));
				Changes.push_back(std::move(*change));
			}
			if (LogText)
			{
				Text = GImGui->LogBuffer.c_str();
				ImGui::LogFinish();
			}
			ImGui::End();
		};
		FieldTableDriver Driver{Gui, Draw,
		                        []
		                        {
									return ImGui::FindWindowByName("Circuit");
								},
		                        []
		                        {
									return ImHashStr("##fields", 0, ImGui::FindWindowByName("Circuit")->ID);
								}};

		CircuitFixture()
		{
			Gui.UseEditorStyle();
			Values = SerializeFields<StructTraits<Circuit>>(Circuit{});
		}

		ImGuiID GetTracksId() const { return Driver.GetFieldId("Tracks"); }

		std::string CaptureText()
		{
			LogText = true;
			Gui.Frame(Draw);
			LogText = false;
			return Text;
		}

		// The change the last interaction reported, the only one since the last call.
		FieldChange TakeChange()
		{
			REQUIRE(Changes.size() == 1);
			FieldChange change = std::move(Changes.back());
			Changes.clear();
			return change;
		}
	};

	// The Project Settings panel in a window large enough for every row, without a scroll bar.
	struct ProjectSettingsFixture
	{
		Testing::AssetManagerScope Assets;
		HeadlessImGui Gui;
		Testing::TemporaryDirectory Temporary;
		EditorContext Context;
		EditorOperations Operations{Context};
		ProjectSettingsPanel Panel;
		bool Open = true;
		std::function<void()> Draw = [this]
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(700.0f, 1000.0f));
			Panel.OnImGuiRender(Operations, Open);
		};
		FieldTableDriver Driver{Gui, Draw,
		                        []
		                        {
									return ImGui::FindWindowByName("Project Settings");
								},
		                        []
		                        {
									return ImHashStr("##fields", 0, ImGui::FindWindowByName("Project Settings")->ID);
								}};

		ProjectSettingsFixture() { Gui.UseEditorStyle(); }

		ImGuiWindow const* GetWindow() const { return ImGui::FindWindowByName("Project Settings"); }
		ImGuiID GetLayersId() const { return Driver.GetFieldId("Layers", Driver.GetFieldId("Physics")); }
		ImGuiID GetMatrixId() const { return ImHashStr("##collisions", 0, GetWindow()->ID); }

		void CreateProject(Json const& settings)
		{
			REQUIRE(Operations.CreateProject(Temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			REQUIRE(Operations.ApplyProjectSettings(settings).IsOk());
			REQUIRE(Operations.SaveProject().IsOk());
			for (int frame = 0; frame < 3; frame++)
			{
				Gui.Frame(Draw);
			}
			REQUIRE(GetWindow() != nullptr);
		}

		std::string ReadProjectFile() const
		{
			Result<std::string> const text = FileSystem::ReadTextFile(Context.GetProject()->GetFilePath());
			REQUIRE(text.IsOk());
			return text.GetValue();
		}
	};
}

TEST_CASE("FieldChange: element edits and list edits patch each object's own array by index")
{
	Json const longer =
		Json::object({{"Tracks", Json::array({MakeTrack("A", "Road", 1.0), MakeTrack("B", "Oval", 1.5), MakeTrack("C", "Road", 0.5)})}});
	Json const shorter = Json::object({{"Tracks", Json::array({MakeTrack("X", "Oval", 2.0)})}});

	SUBCASE("a field of an element")
	{
		FieldChange const change = MakeChange({{"Tracks", 1}, {"Grip"}}, 0.25);
		Json const patched = change.MakePatch(longer);
		CHECK(patched["Tracks"][1]["Grip"] == 0.25);
		CHECK(patched["Tracks"][0] == longer["Tracks"][0]);
		CHECK(patched["Tracks"][1]["Name"] == "B");
		CHECK(patched["Tracks"].size() == 3);
		// An object without element 1 keeps its array as it is, and no element is added to it.
		CHECK(change.MakePatch(shorter) == shorter);
		CHECK(MakeChange({{"Tracks", 7}, {"Grip"}}, 0.25).MakePatch(longer) == longer);
		// A path into an element of a field the object does not have changes nothing.
		CHECK(change.MakePatch(Json::object({{"Title", "T"}})) == Json::object());
	}
	SUBCASE("an element of an array of plain values, and of an array inside an element")
	{
		Json const layers = Json::object({{"Layers", {"Default", "Player", "Enemy"}}});
		CHECK(MakeChange({{"Layers", 1}}, "Hero").MakePatch(layers) == Json::object({{"Layers", {"Default", "Hero", "Enemy"}}}));
		CHECK(MakeChange({{"Layers", 3}}, "Hero").MakePatch(layers) == layers);

		Json const tagged = Json::object({{"Tracks", Json::array({MakeTrack("A", "Road", 1.0, {"wet", "night"})})}});
		Json const retagged = MakeChange({{"Tracks", 0}, {"Tags", 1}}, "day").MakePatch(tagged);
		CHECK(retagged["Tracks"][0]["Tags"] == Json::array({"wet", "day"}));
		Json const untagged = MakeChange({{"Tracks", 0}, {"Tags"}}, {}, FieldArrayEdit{FieldArrayEditKind::Remove, 0, 0}).MakePatch(tagged);
		CHECK(untagged["Tracks"][0]["Tags"] == Json::array({"night"}));
		CHECK(untagged["Tracks"][0]["Name"] == "A");
	}
	SUBCASE("one component of a vector element")
	{
		Json const points = Json::object({{"Points", Json::array({{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}})}});
		FieldChange change = MakeChange({{"Points", 1}}, {9.0, 9.0, 9.0});
		change.Component = 1;
		CHECK(change.MakePatch(points) == Json::object({{"Points", Json::array({{1.0, 2.0, 3.0}, {4.0, 9.0, 6.0}})}}));
	}
	SUBCASE("insertions")
	{
		Json const inserted = MakeTrack("New", "Road", 1.0);
		FieldChange const above = MakeChange({{"Tracks"}}, inserted, FieldArrayEdit{FieldArrayEditKind::Insert, 1, 0});
		Json const patched = above.MakePatch(longer);
		REQUIRE(patched["Tracks"].size() == 4);
		CHECK(patched["Tracks"][1] == inserted);
		CHECK(patched["Tracks"][2]["Name"] == "B");
		// Past the end of a shorter array: appended.
		Json const appended = above.MakePatch(shorter);
		REQUIRE(appended["Tracks"].size() == 2);
		CHECK(appended["Tracks"][1] == inserted);
		// End appends to every array, whatever its size; an object without the array gets one.
		FieldChange const end = MakeChange({{"Tracks"}}, inserted, FieldArrayEdit{FieldArrayEditKind::Insert, FieldArrayEdit::End, 0});
		CHECK(end.MakePatch(longer)["Tracks"].size() == 4);
		CHECK(end.MakePatch(longer)["Tracks"][3] == inserted);
		CHECK(end.MakePatch(shorter)["Tracks"][1] == inserted);
		CHECK(end.MakePatch(Json::object({{"Tracks", Json()}})) == Json::object({{"Tracks", Json::array({inserted})}}));
	}
	SUBCASE("removals, moves and duplicates")
	{
		auto const names = [](Json const& patch)
		{
			std::vector<std::string> result;
			for (Json const& track : patch["Tracks"])
			{
				result.push_back(track["Name"].get<std::string>());
			}
			return result;
		};
		auto const edit = [](FieldArrayEditKind kind, uint32_t index, uint32_t target = 0)
		{
			return MakeChange({{"Tracks"}}, {}, FieldArrayEdit{kind, index, target});
		};
		CHECK(names(edit(FieldArrayEditKind::Remove, 1).MakePatch(longer)) == std::vector<std::string>{"A", "C"});
		CHECK(edit(FieldArrayEditKind::Remove, 1).MakePatch(shorter) == shorter);
		CHECK(names(edit(FieldArrayEditKind::Move, 0, 1).MakePatch(longer)) == std::vector<std::string>{"B", "A", "C"});
		CHECK(names(edit(FieldArrayEditKind::Move, 2, 1).MakePatch(longer)) == std::vector<std::string>{"A", "C", "B"});
		CHECK(names(edit(FieldArrayEditKind::Move, 0, 2).MakePatch(longer)) == std::vector<std::string>{"B", "C", "A"});
		CHECK(edit(FieldArrayEditKind::Move, 0, 1).MakePatch(shorter) == shorter);
		CHECK(names(edit(FieldArrayEditKind::Duplicate, 1).MakePatch(longer)) == std::vector<std::string>{"A", "B", "B", "C"});
		// Each object duplicates its own element.
		CHECK(names(edit(FieldArrayEditKind::Duplicate, 0).MakePatch(shorter)) == std::vector<std::string>{"X", "X"});
		CHECK(edit(FieldArrayEditKind::Duplicate, 3).MakePatch(longer) == longer);
	}
}

TEST_CASE("FieldEditor: arrays of structs show a row per element over the element's own fields, and edit them by index")
{
	CircuitFixture fixture;
	FieldTableDriver const& driver = fixture.Driver;
	fixture.Values["Tracks"] = Json::array({MakeTrack("Alpha", "Oval", 1.0, {"wet"}), MakeTrack("", "Road", 1.0)});
	for (int frame = 0; frame < 3; frame++)
	{
		fixture.Gui.Frame(fixture.Draw);
	}
	// Drawing changes nothing. Elements start folded, titled by their first string field.
	CHECK(fixture.Changes.empty());
	ImGuiID const tracks = fixture.GetTracksId();
	CHECK_FALSE(driver.Find(ImHashStr("##value", 0, driver.GetFieldId("Grip", driver.GetElementId(tracks, 0))), driver.GetValueX()));

	// Unfolded, each element shows its own fields: Banking only where the element's own Kind is Oval. (A log of the
	// window shows every tree node open.)
	driver.OpenElement(tracks, 0);
	driver.OpenElement(tracks, 1);
	std::string const open = fixture.CaptureText();
	CHECK(open.find("2 of at most 3 elements") != std::string::npos);
	CHECK(open.find("0: Alpha") != std::string::npos);
	CHECK(open.find("Element 1") != std::string::npos);
	CAPTURE(open);
	size_t const banking = open.find("Banking");
	CHECK(banking != std::string::npos);
	CHECK(open.find("Banking", banking + 1) == std::string::npos);
	CHECK(fixture.Changes.empty());

	// A drag inside element 1 is a change of that element's field, reported every frame of the drag.
	ImGuiID const second = driver.GetElementId(tracks, 1);
	std::optional<ImVec2> const grip = driver.Find(ImHashStr("##value", 0, driver.GetFieldId("Grip", second)), driver.GetValueX());
	REQUIRE(grip);
	fixture.Gui.Drag(*grip, ImVec2(grip->x + 60.0f, grip->y), fixture.Draw);
	REQUIRE_FALSE(fixture.Changes.empty());
	for (FieldChange const& change : fixture.Changes)
	{
		CHECK(change.Path == std::vector<FieldPathStep>{{"Tracks", 1}, {"Grip"}});
		CHECK_FALSE(change.ArrayEdit);
	}
	CHECK(fixture.Values["Tracks"][1]["Grip"].get<float>() > 1.0f);
	CHECK(fixture.Values["Tracks"][0]["Grip"] == 1.0);
	fixture.Changes.clear();

	// The array inside element 0: its "+" adds a tag to that element's own list.
	ImGuiID const first = driver.GetElementId(tracks, 0);
	driver.Click(ImHashStr("+", 0, driver.GetFieldId("Tags", first)), driver.GetValueStartX());
	FieldChange const tag = fixture.TakeChange();
	CHECK(tag.Path == std::vector<FieldPathStep>{{"Tracks", 0}, {"Tags"}});
	REQUIRE(tag.ArrayEdit);
	CHECK(*tag.ArrayEdit == FieldArrayEdit{FieldArrayEditKind::Insert, FieldArrayEdit::End, 0});
	CHECK(fixture.Values["Tracks"][0]["Tags"] == Json::array({"wet", ""}));
	// At its MaxCount of 2 its "+" is disabled.
	fixture.Gui.Frame(fixture.Draw);
	REQUIRE(driver.Find(ImHashStr("+", 0, driver.GetFieldId("Tags", first)), driver.GetValueStartX()));
	CHECK(GImGui->HoveredIdIsDisabled);
	CHECK(fixture.Changes.empty());
}

TEST_CASE("FieldEditor: the menu of an element moves, duplicates, inserts and removes it, and adding stops at MaxCount")
{
	CircuitFixture fixture;
	FieldTableDriver const& driver = fixture.Driver;
	fixture.Values["Tracks"] = Json::array({MakeTrack("A", "Road", 0.5), MakeTrack("B", "Oval", 1.5)});
	for (int frame = 0; frame < 3; frame++)
	{
		fixture.Gui.Frame(fixture.Draw);
	}
	ImGuiID const tracks = fixture.GetTracksId();
	auto const names = [&fixture]
	{
		std::vector<std::string> result;
		for (Json const& track : fixture.Values["Tracks"])
		{
			result.push_back(track["Name"].get<std::string>());
		}
		return result;
	};
	auto const expectEdit = [&fixture](FieldArrayEdit const& expected)
	{
		FieldChange const change = fixture.TakeChange();
		CHECK(change.Path == std::vector<FieldPathStep>{{"Tracks"}});
		REQUIRE(change.ArrayEdit);
		CHECK(*change.ArrayEdit == expected);
		return change;
	};

	// The first element cannot move up, the last cannot move down.
	CHECK_FALSE(driver.IsMenuItemEnabled(tracks, 0, "Move Up"));
	CHECK_FALSE(driver.IsMenuItemEnabled(tracks, 1, "Move Down"));
	CHECK(fixture.Changes.empty());

	driver.PickFromMenu(tracks, 0, "Move Down");
	expectEdit({FieldArrayEditKind::Move, 0, 1});
	CHECK(names() == std::vector<std::string>{"B", "A"});
	driver.PickFromMenu(tracks, 1, "Move Up");
	expectEdit({FieldArrayEditKind::Move, 1, 0});
	CHECK(names() == std::vector<std::string>{"A", "B"});

	// Insert Above puts a default element in the element's place; it is open to be edited.
	driver.PickFromMenu(tracks, 1, "Insert Above");
	FieldChange const inserted = expectEdit({FieldArrayEditKind::Insert, 1, 0});
	CHECK(inserted.Value == MakeTrack("", "Road", 1.0));
	CHECK(names() == std::vector<std::string>{"A", "", "B"});
	fixture.Gui.Frame(fixture.Draw);
	CHECK(driver.Find(ImHashStr("##value", 0, driver.GetFieldId("Grip", driver.GetElementId(tracks, 1))), driver.GetValueX()));

	// At MaxCount (3) nothing adds: Add Element, Duplicate and Insert Above are disabled.
	REQUIRE(driver.Find(ImHashStr("Add Element", 0, tracks), driver.GetValueX()));
	CHECK(GImGui->HoveredIdIsDisabled);
	CHECK_FALSE(driver.IsMenuItemEnabled(tracks, 0, "Duplicate"));
	CHECK_FALSE(driver.IsMenuItemEnabled(tracks, 0, "Insert Above"));
	driver.Click(ImHashStr("Add Element", 0, tracks), driver.GetValueX());
	CHECK(fixture.Changes.empty());

	driver.PickFromMenu(tracks, 1, "Remove");
	expectEdit({FieldArrayEditKind::Remove, 1, 0});
	CHECK(names() == std::vector<std::string>{"A", "B"});

	driver.PickFromMenu(tracks, 0, "Duplicate");
	expectEdit({FieldArrayEditKind::Duplicate, 0, 0});
	CHECK(names() == std::vector<std::string>{"A", "A", "B"});
	driver.PickFromMenu(tracks, 1, "Remove");
	expectEdit({FieldArrayEditKind::Remove, 1, 0});

	driver.Click(ImHashStr("Add Element", 0, tracks), driver.GetValueX());
	FieldChange const added = expectEdit({FieldArrayEditKind::Insert, FieldArrayEdit::End, 0});
	CHECK(added.Value == MakeTrack("", "Road", 1.0));
	CHECK(names() == std::vector<std::string>{"A", "B", ""});
}

TEST_CASE("FieldEditor: a struct drawer draws its struct wherever it appears, and its edit carries its name and merge key")
{
	CircuitFixture fixture;
	FieldTableDriver const& driver = fixture.Driver;
	fixture.Values["Tracks"] = Json::array({MakeTrack("A", "Road", 0.5), MakeTrack("B", "Oval", 1.5)});
	CHECK_FALSE(fixture.Editor.HasStructDrawer("Track"));

	constexpr uint64_t DrawerKey = 0x5452434B00000001ull;
	std::vector<std::string> drawn;
	std::optional<FieldStructEdit> next;
	fixture.Editor.SetStructDrawer("Track",
	                               [&](FieldStructDrawContext const& context) -> std::optional<FieldStructEdit>
	                               {
									   CHECK(context.Field.Kind == FieldKind::Struct);
									   CHECK(&context.Editor == &fixture.Editor);
									   CHECK(context.Editable);
									   drawn.push_back(context.Value["Name"].get<std::string>());
									   ImGui::TextUnformatted("Drawn by the drawer of Track");
									   return std::exchange(next, std::nullopt);
								   });
	REQUIRE(fixture.Editor.HasStructDrawer("Track"));
	for (int frame = 0; frame < 3; frame++)
	{
		fixture.Gui.Frame(fixture.Draw);
	}
	// Folded elements draw nothing; an open one draws its drawer instead of its rows.
	CHECK(drawn.empty());
	ImGuiID const tracks = fixture.GetTracksId();
	driver.OpenElement(tracks, 1);
	REQUIRE_FALSE(drawn.empty());
	CHECK(drawn.back() == "B");
	std::string const text = fixture.CaptureText();
	CHECK(text.find("Drawn by the drawer of Track") != std::string::npos);
	CHECK(text.find("Grip") == std::string::npos);

	Json changed = fixture.Values["Tracks"][1];
	changed["Grip"] = 2.0;
	next = FieldStructEdit{changed, "Bank Track", DrawerKey};
	fixture.Gui.Frame(fixture.Draw);
	FieldChange const change = fixture.TakeChange();
	CHECK(change.Path == std::vector<FieldPathStep>{{"Tracks", 1}});
	CHECK(change.Value == changed);
	CHECK(change.Description == "Bank Track");
	CHECK(change.MergeKey == DrawerKey);
	CHECK(fixture.Values["Tracks"][1]["Grip"] == 2.0);

	// Disabled, the drawer is told so and its edit is not reported.
	drawn.clear();
	fixture.Editor.SetStructDrawer("Track",
	                               [&](FieldStructDrawContext const& context) -> std::optional<FieldStructEdit>
	                               {
									   CHECK_FALSE(context.Editable);
									   drawn.push_back(context.Value["Name"].get<std::string>());
									   return FieldStructEdit{context.Value, {}, 0};
								   });
	fixture.Gui.Frame(
		[&fixture]
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(600.0f, 1000.0f));
			ImGui::Begin("Circuit", nullptr, ImGuiWindowFlags_NoSavedSettings);
			ImGui::BeginDisabled();
			CHECK_FALSE(fixture.Editor.Draw(fixture.Fields, fixture.Values));
			ImGui::EndDisabled();
			ImGui::End();
		});
	CHECK_FALSE(drawn.empty());

	// An empty drawer removes it: the element shows its rows again.
	fixture.Editor.SetStructDrawer("Track", {});
	CHECK_FALSE(fixture.Editor.HasStructDrawer("Track"));
	CHECK(fixture.CaptureText().find("Grip") != std::string::npos);
}

TEST_CASE("Panels: the inspector edits a data asset's array of structs, one undo step per element edit, as test-data.set does")
{
	Testing::TestDataTypeScope const type;
	Testing::AssetManagerScope const assets;
	HeadlessImGui imgui;
	Testing::TemporaryDirectory temporary;
	EditorContext context;
	EditorOperations operations(context);
	CommandRegistry registry;
	EditorCommandEnvironment environment;
	environment.Locations = Testing::MakeEditorLocations(temporary.GetPath());
	REQUIRE(RegisterEditorCommands(registry, operations, std::move(environment)).IsOk());
	REQUIRE(operations.CreateProject(temporary.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	Result<AssetHandle> const created = operations.CreateDataAsset(type.GetType(), "Data/Crate.stestdata");
	REQUIRE(created.IsOk());
	AssetHandle const crate = created.GetValue();
	std::filesystem::path const file = context.GetProject()->GetAssetDirectory() / "Data" / "Crate.stestdata";
	auto const readFile = [](std::filesystem::path const& path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE(text.IsOk());
		return text.TakeValue();
	};
	std::string const original = readFile(file);
	Ref<TypedDataAsset<Testing::TestData>> const asset = AssetManager::GetAsset<TypedDataAsset<Testing::TestData>>(crate);
	REQUIRE(asset != nullptr);
	CommandHistory const& history = context.GetHistory();
	size_t const steps = history.GetUndoCount();

	InspectorPanel inspector;
	bool open = true;
	auto const draw = [&]
	{
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(500.0f, 1000.0f));
		inspector.OnImGuiRender(operations, open);
	};
	context.SelectAsset(crate);
	for (int frame = 0; frame < 3; frame++)
	{
		imgui.Frame(draw);
	}
	FieldTableDriver const driver{imgui, draw,
	                              []
	                              {
									  return ImGui::FindWindowByName("Inspector");
								  },
	                              []
	                              {
									  return ImHashStr("##fields", 0, ImHashStr("##asset", 0, ImGui::FindWindowByName("Inspector")->ID));
								  }};
	ImGuiID const entries = driver.GetFieldId("Entries");
	CHECK(history.GetUndoCount() == steps);
	auto const names = [&asset]
	{
		std::vector<std::string> result;
		for (Testing::TestDataEntry const& entry : asset->GetData().Entries)
		{
			result.push_back(entry.Name);
		}
		return result;
	};

	// Add Element, then the new element's name typed: two steps.
	driver.Click(ImHashStr("Add Element", 0, entries), driver.GetValueX());
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(names() == std::vector<std::string>{""});
	driver.TypeInto(ImHashStr("##value", 0, driver.GetFieldId("Name", driver.GetElementId(entries, 0))), "First");
	CHECK(history.GetUndoCount() == steps + 2);
	CHECK(names() == std::vector<std::string>{"First"});

	// A second element, then a drag of its weight: one step for the whole drag (a second later, so that its press is no
	// double-click).
	imgui.Frame(draw, 1.0f);
	driver.Click(ImHashStr("Add Element", 0, entries), driver.GetValueX());
	CHECK(history.GetUndoCount() == steps + 3);
	imgui.Frame(draw, 1.0f);
	std::optional<ImVec2> const weight =
		driver.Find(ImHashStr("##value", 0, driver.GetFieldId("Weight", driver.GetElementId(entries, 1))), driver.GetValueX());
	REQUIRE(weight);
	imgui.Drag(*weight, ImVec2(weight->x + 60.0f, weight->y), draw);
	CHECK(history.GetUndoCount() == steps + 4);
	float const dragged = asset->GetData().Entries.at(1).Weight;
	CHECK(dragged > 1.0f);
	CHECK(asset->GetData().Entries.at(0).Weight == 1.0f);

	// Move Up and Remove: a step each, named after the type.
	imgui.Frame(draw, 1.0f);
	driver.PickFromMenu(entries, 1, "Move Up");
	CHECK(history.GetUndoCount() == steps + 5);
	CHECK(names() == std::vector<std::string>{"", "First"});
	CHECK(asset->GetData().Entries.at(0).Weight == dragged);
	driver.PickFromMenu(entries, 1, "Remove");
	CHECK(history.GetUndoCount() == steps + 6);
	CHECK(history.GetUndoDescription() == "Edit TestData");
	CHECK(names() == std::vector<std::string>{""});
	std::string const edited = readFile(file);

	// Undone one at a time: each step restores its own element edit, down to the file as it was made.
	REQUIRE(operations.Undo().IsOk());
	CHECK(names() == std::vector<std::string>{"", "First"});
	REQUIRE(operations.Undo().IsOk());
	CHECK(names() == std::vector<std::string>{"First", ""});
	CHECK(asset->GetData().Entries.at(1).Weight == dragged);
	REQUIRE(operations.Undo().IsOk());
	CHECK(asset->GetData().Entries.at(1).Weight == 1.0f);
	REQUIRE(operations.Undo().IsOk());
	CHECK(names() == std::vector<std::string>{"First"});
	REQUIRE(operations.Undo().IsOk());
	CHECK(names() == std::vector<std::string>{""});
	REQUIRE(operations.Undo().IsOk());
	CHECK(names().empty());
	CHECK(readFile(file) == original);
	for (int step = 0; step < 6; step++)
	{
		REQUIRE(operations.Redo().IsOk());
	}
	CHECK(readFile(file) == edited);

	// test-data.set with the same entries writes the same file.
	REQUIRE(operations.CreateDataAsset(type.GetType(), "Data/Copy.stestdata").IsOk());
	std::optional<CommandResult> result;
	registry.Execute(
		"test-data.set",
		Json::object({{"testData", "asset://Data/Copy.stestdata"}, {"fields", {{"Entries", asset->SerializeFields()["Entries"]}}}}),
		[&result](CommandResult value)
		{
			result.emplace(std::move(value));
		});
	REQUIRE(result);
	REQUIRE_MESSAGE(result->IsOk(), (result->IsError() ? result->GetError().Message : std::string()));
	CHECK(readFile(context.GetProject()->GetAssetDirectory() / "Data" / "Copy.stestdata") == edited);

	// At its MaxCount of 16 the array takes no more: Add Element is disabled.
	Json full = Json::array();
	for (int index = 0; index < 16; index++)
	{
		full.push_back(Json::object({{"Name", fmt::format("E{}", index)}, {"Weight", 1.0}}));
	}
	REQUIRE(operations.SetDataAssetFields(crate, Json::object({{"Entries", full}})).IsOk());
	size_t const fullSteps = history.GetUndoCount();
	imgui.Frame(draw);
	REQUIRE(driver.Find(ImHashStr("Add Element", 0, entries), driver.GetValueX()));
	CHECK(GImGui->HoveredIdIsDisabled);
	driver.Click(ImHashStr("Add Element", 0, entries), driver.GetValueX());
	CHECK(history.GetUndoCount() == fullSteps);
	CHECK(asset->GetData().Entries.size() == 16);
	CHECK(open);
}

TEST_CASE("ProjectSettingsPanel: edits through the panel write the project file the same settings patches write")
{
	ProjectSettingsFixture fixture;
	FieldTableDriver const& driver = fixture.Driver;
	Json const layers =
		Json::object({{"Physics",
	                   {{"Layers", {"Default", "Player", "Enemy"}},
	                    {"IgnoredCollisions", Json::array({{{"First", 1}, {"Second", 2}}, {{"First", 0}, {"Second", 1}}})}}}});
	fixture.CreateProject(layers);

	// The name, a check box, a layer renamed, the last layer removed (its pair goes with it) and a pair of the matrix
	// switched back to colliding.
	driver.TypeInto(ImHashStr("##value", 0, driver.GetFieldId("Name")), "Racer");
	driver.Click(ImHashStr("##value", 0, driver.GetFieldId("Fullscreen", driver.GetFieldId("Window"))), driver.GetValueStartX());
	driver.TypeInto(ImHashStr("##value", 0, GetIntId(1, fixture.GetLayersId())), "Hero");
	driver.Click(ImHashStr("-", 0, GetIntId(2, fixture.GetLayersId())), driver.GetButtonX());
	// Layers 0 and 1 of two: the cell of row 0 in the column of layer 1 (the table's third, after the names).
	ImGuiID const cell = ImHashStr("##collides", 0, GetIntId(1, fixture.GetMatrixId()));
	driver.Click(cell, GetColumnX(fixture.GetMatrixId(), 2, ImGui::GetFrameHeight() * 0.5f));
	fixture.Gui.Frame(fixture.Draw);
	fixture.Panel.Flush(fixture.Operations);
	std::string const written = fixture.ReadProjectFile();

	// The same edits as settings patches, on a project of their own.
	EditorContext expectedContext;
	EditorOperations expected(expectedContext);
	REQUIRE(expected.CreateProject(fixture.Temporary.GetPath() / "Expected", "Game", Scene("Main")).IsOk());
	REQUIRE(expected.ApplyProjectSettings(layers).IsOk());
	REQUIRE(expected
	            .ApplyProjectSettings(Json::object({{"Name", "Racer"},
	                                                {"Window", {{"Fullscreen", true}}},
	                                                {"Physics", {{"Layers", {"Default", "Hero"}}, {"IgnoredCollisions", Json::array()}}}}))
	            .IsOk());
	REQUIRE(expected.SaveProject().IsOk());
	Result<std::string> const expectedText = FileSystem::ReadTextFile(expectedContext.GetProject()->GetFilePath());
	REQUIRE(expectedText.IsOk());
	// Each project made its own start scene: the expected file names the panel project's.
	std::string expectedFile = expectedText.GetValue();
	std::string const ownScene = expectedContext.GetProject()->GetSettings().StartScene.ToString();
	size_t const scene = expectedFile.find(ownScene);
	REQUIRE(scene != std::string::npos);
	expectedFile.replace(scene, ownScene.size(), fixture.Context.GetProject()->GetSettings().StartScene.ToString());
	CHECK(written == expectedFile);
	CHECK(fixture.Open);
}

TEST_CASE("ProjectSettingsPanel: removing a layer renumbers the ignored pairs, and a layer added is named and stops at 16")
{
	ProjectSettingsFixture fixture;
	FieldTableDriver const& driver = fixture.Driver;
	fixture.CreateProject(
		Json::object({{"Physics",
	                   {{"Layers", {"Default", "Player", "Enemy", "Pickup"}},
	                    {"IgnoredCollisions",
	                     Json::array({{{"First", 1}, {"Second", 2}}, {{"First", 0}, {"Second", 3}}, {{"First", 2}, {"Second", 3}}})}}}}));
	auto const pairs = [&fixture]
	{
		return fixture.Context.GetProject()->GetSettings().Physics.IgnoredCollisions;
	};
	auto const layerNames = [&fixture]
	{
		return fixture.Context.GetProject()->GetSettings().Physics.Layers;
	};

	// Removing Player (1): its pair goes, and Enemy and Pickup move down to 1 and 2.
	driver.Click(ImHashStr("-", 0, GetIntId(1, fixture.GetLayersId())), driver.GetButtonX());
	CHECK(layerNames() == std::vector<std::string>{"Default", "Enemy", "Pickup"});
	CHECK(pairs() == std::vector<PhysicsLayerPair>{{0, 2}, {1, 2}});

	// "+" adds a layer with a name of its own (layer names are unique and not empty); the pairs stay.
	driver.Click(ImHashStr("+", 0, fixture.GetLayersId()), driver.GetValueStartX());
	CHECK(layerNames() == std::vector<std::string>{"Default", "Enemy", "Pickup", "Layer 3"});
	CHECK(pairs() == std::vector<PhysicsLayerPair>{{0, 2}, {1, 2}});

	// At 16 layers "+" is disabled.
	Json sixteen = Json::array();
	for (size_t layer = 0; layer < MaxPhysicsLayers; layer++)
	{
		sixteen.push_back(fmt::format("L{}", layer));
	}
	REQUIRE(fixture.Operations.ApplyProjectSettings(Json::object({{"Physics", {{"Layers", sixteen}}}})).IsOk());
	fixture.Gui.Frame(fixture.Draw);
	REQUIRE(driver.Find(ImHashStr("+", 0, fixture.GetLayersId()), driver.GetValueStartX()));
	CHECK(GImGui->HoveredIdIsDisabled);
	driver.Click(ImHashStr("+", 0, fixture.GetLayersId()), driver.GetValueStartX());
	CHECK(layerNames().size() == MaxPhysicsLayers);
	fixture.Panel.Flush(fixture.Operations);
	CHECK(fixture.Open);
}
