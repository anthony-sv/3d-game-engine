#include "HeadlessImGui.h"
#include "TestUtilities.h"

#include "Editor/EditorOperations.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/UI/CurveAssetEditor.h"
#include "Editor/UI/DataAssetEditor.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/CurveAsset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

using namespace Strada;
using Strada::Testing::HeadlessImGui;

namespace
{
	ImVec2 ToImVec2(glm::vec2 value)
	{
		return ImVec2(value.x, value.y);
	}

	// A project with a curve asset (keys at 0, 1 and 2, looping after them) selected in an inspector whose curve drawer the
	// test holds.
	struct CurveInspectorFixture
	{
		Testing::AssetManagerScope Assets;
		HeadlessImGui Gui;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		InspectorPanel Inspector;
		Ref<CurveAssetEditor> Curves = CreateRef<CurveAssetEditor>();
		AssetHandle Asset;
		std::filesystem::path File;
		bool Open = true;
		// Whether the editor's global Delete route fired in a frame since the last check (it runs after the panels).
		bool GlobalDelete = false;

		CurveInspectorFixture()
		{
			REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			DataAssetType const* const type = DataAssetRegistry::FindByName("Curve");
			REQUIRE(type != nullptr);
			Json const fields = Json::parse(R"({ "PostExtrapolation": "Loop", "Keys": [
				{ "Time": 0, "Value": 0 }, { "Time": 1, "Value": 1 }, { "Time": 2, "Value": 0 } ] })");
			Result<AssetHandle> const created = Operations.CreateDataAsset(type->GetAssetType(), "Curves/Ramp.scurve", fields);
			REQUIRE(created.IsOk());
			Asset = created.GetValue();
			File = Context.GetProject()->GetAssetDirectory() / "Curves" / "Ramp.scurve";
			Inspector.GetDataAssetEditor().SetDrawer("Curve", CurveAssetEditor::MakeDrawer(Curves));
			Context.SelectAsset(Asset);
			Frames(3);
		}

		std::function<void()> Drawer()
		{
			return [this]
			{
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(600.0f, 900.0f));
				Inspector.OnImGuiRender(Operations, Open);
				GlobalDelete = ImGui::Shortcut(ImGuiKey_Delete, ImGuiInputFlags_RouteGlobal) || GlobalDelete;
			};
		}

		void Frames(int count, float seconds = 1.0f / 60.0f)
		{
			for (int frame = 0; frame < count; frame++)
			{
				Gui.Frame(Drawer(), seconds);
			}
		}

		Curve ReadCurve() const
		{
			Ref<TypedDataAsset<Curve>> const asset = AssetManager::GetAsset<TypedDataAsset<Curve>>(Asset);
			REQUIRE(asset != nullptr);
			return asset->GetData();
		}

		std::string ReadFile() const
		{
			Result<std::string> const text = FileSystem::ReadTextFile(File);
			REQUIRE(text.IsOk());
			return text.GetValue();
		}

		glm::vec2 Key(uint32_t key) const
		{
			std::optional<glm::vec2> const position = Curves->GetEditor().GetView().GetKeyPosition(ReadCurve(), 0, key, 0);
			REQUIRE(position.has_value());
			return *position;
		}

		size_t GetUndoCount() const { return Context.GetHistory().GetUndoCount(); }
	};
}

TEST_CASE("CurveAssetEditor: the inspector draws a curve asset in the curve editor above its fields, changing nothing")
{
	// The data asset editor gives the engine's curve type its drawer.
	CHECK(DataAssetEditor().HasDrawer("Curve"));

	CurveInspectorFixture fixture;
	std::string const bytes = fixture.ReadFile();
	size_t const undo = fixture.GetUndoCount();
	fixture.Frames(2);
	CHECK(fixture.Curves->GetAsset() == fixture.Asset);
	CurveEditorDrawRecord const& record = fixture.Curves->GetEditor().GetDrawRecord();
	CHECK(record.Curves == 1);
	CHECK(record.CurveKeys == 3);
	CHECK(record.EventKeys == 0);
	CHECK_FALSE(record.Empty);
	for (char const* const part : {"Curves", "Time", "Value", "Cubic", "Linear", "Constant", "Auto", "Flat", "Frame"})
	{
		CAPTURE(part);
		CHECK(fixture.Curves->GetEditor().FindRect(part).has_value());
	}
	// Framed on the keys' span (an unbounded domain): every key within the view.
	glm::dvec2 const times = fixture.Curves->GetEditor().GetView().GetTimeRange();
	CHECK(times.x < 0.0);
	CHECK(times.y > 2.0);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);
	CHECK_FALSE(fixture.Context.IsDirty());
}

TEST_CASE("CurveAssetEditor: a key dragged in the curve editor is one step, undone and redone byte for byte, and Escape leaves none")
{
	CurveInspectorFixture fixture;
	std::string const bytes = fixture.ReadFile();
	size_t const undo = fixture.GetUndoCount();
	CommandHistory const& history = fixture.Context.GetHistory();

	// A drag in several frames: each frame's curve reaches the asset, all of it one step.
	glm::vec2 const key = fixture.Key(1);
	fixture.Gui.DragPath(ToImVec2(key), ToImVec2(key + glm::vec2(40.0f, -30.0f)), fixture.Drawer());
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(history.GetUndoDescription() == "Move Key of Curve on Ramp");
	Curve const dragged = fixture.ReadCurve();
	CHECK(dragged.Keys[1].Time > 1.0f);
	CHECK(dragged.Keys[1].Value > 1.0f);
	CHECK(dragged.PostExtrapolation == CurveExtrapolation::Loop);
	CHECK(ValidateCurve(dragged).IsOk());
	std::string const after = fixture.ReadFile();
	CHECK(after != bytes);
	CHECK_FALSE(fixture.Context.IsDirty());
	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Frames(1);
	CHECK(fixture.ReadFile() == bytes);
	CHECK(fixture.ReadCurve().Keys[1].Time == 1.0f);
	REQUIRE(fixture.Operations.Redo().IsOk());
	fixture.Frames(1);
	CHECK(fixture.ReadFile() == after);
	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Frames(1);

	// Escape with the button still down: the key goes back, and the drag's step disappears (a second later, so that the
	// press is no double click).
	ImGuiIO& io = ImGui::GetIO();
	io.AddMousePosEvent(key.x, key.y);
	fixture.Frames(1, 1.0f);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	fixture.Frames(1);
	for (int frame = 1; frame <= 3; frame++)
	{
		io.AddMousePosEvent(key.x + 15.0f * static_cast<float>(frame), key.y);
		fixture.Frames(1);
	}
	REQUIRE(fixture.ReadCurve().Keys[1].Time > 1.0f);
	REQUIRE(fixture.Curves->GetEditor().IsGestureActive());
	fixture.Gui.Press(ImGuiKey_Escape, fixture.Drawer());
	fixture.Frames(1);
	CHECK_FALSE(fixture.Curves->GetEditor().IsGestureActive());
	io.AddMousePosEvent(key.x + 80.0f, key.y);
	fixture.Frames(1);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	fixture.Frames(2);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);
	CHECK(fixture.ReadCurve().Keys[1].Time == 1.0f);
}

TEST_CASE("CurveAssetEditor: a drag whose curve changes elsewhere ends and keeps that change, and removed keys leave the selection")
{
	CurveInspectorFixture fixture;
	size_t const undo = fixture.GetUndoCount();
	glm::vec2 const key = fixture.Key(1);
	ImGuiIO& io = ImGui::GetIO();
	io.AddMousePosEvent(key.x, key.y);
	fixture.Frames(1);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	fixture.Frames(1);
	io.AddMousePosEvent(key.x + 30.0f, key.y);
	fixture.Frames(2);
	REQUIRE(fixture.Curves->GetEditor().IsGestureActive());
	REQUIRE(fixture.GetUndoCount() == undo + 1);

	// An agent sets the keys meanwhile (curve.set's step): the drag ends without an edit, and the agent's keys stay.
	Json const keys = Json::parse(R"({ "Keys": [ { "Time": 0, "Value": 0.25 }, { "Time": 1.5, "Value": 0.75 } ] })");
	REQUIRE(fixture.Operations.SetDataAssetFields(fixture.Asset, keys, "Set Keys").IsOk());
	fixture.Frames(1);
	CHECK_FALSE(fixture.Curves->GetEditor().IsGestureActive());
	io.AddMousePosEvent(key.x + 90.0f, key.y + 20.0f);
	fixture.Frames(2);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	fixture.Frames(2);
	Curve const kept = fixture.ReadCurve();
	REQUIRE(kept.Keys.size() == 2);
	CHECK(kept.Keys[1].Time == 1.5f);
	CHECK(kept.Keys[1].Value == 0.75f);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Keys");

	// The last key selected, then removed elsewhere: it leaves the selection before the header would read it.
	fixture.Frames(1, 1.0f);
	fixture.Gui.Click(ToImVec2(fixture.Key(1)), fixture.Drawer());
	REQUIRE(fixture.Curves->GetEditor().GetView().IsSelected(0, 1));
	Json const one = Json::parse(R"({ "Keys": [ { "Time": 0.5, "Value": 0.5 } ] })");
	REQUIRE(fixture.Operations.SetDataAssetFields(fixture.Asset, one, "Set One Key").IsOk());
	fixture.Frames(2);
	CHECK(fixture.Curves->GetEditor().GetView().GetSelection().empty());
	CHECK(fixture.Curves->GetEditor().GetDrawRecord().CurveKeys == 1);
	CHECK(fixture.GetUndoCount() == undo + 3);
}

TEST_CASE("CurveAssetEditor: while the curve editor has the keyboard, the arrows nudge in runs of one step and Delete removes keys")
{
	CurveInspectorFixture fixture;
	CommandHistory const& history = fixture.Context.GetHistory();
	// Before the curve view has the keyboard, Delete is the editor's and removes no key.
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.GlobalDelete);
	CHECK(fixture.ReadCurve().Keys.size() == 3);
	// A click on a key selects it and gives the curve view the keyboard.
	fixture.Gui.Click(ToImVec2(fixture.Key(1)), fixture.Drawer());
	REQUIRE(fixture.Curves->GetEditor().GetView().IsSelected(0, 1));
	size_t const undo = fixture.GetUndoCount();
	double const step = fixture.Curves->GetEditor().GetView().GetTimeStep();
	REQUIRE(step > 0.0);

	// Three presses 0.2 s apart: one step; 0.6 s later, Shift's ten steps back are a step of their own.
	for (int press = 0; press < 3; press++)
	{
		fixture.Frames(1, 0.2f);
		fixture.Gui.Press(ImGuiKey_RightArrow, fixture.Drawer());
	}
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(history.GetUndoDescription() == "Move Key of Curve on Ramp");
	CHECK(fixture.ReadCurve().Keys[1].Time == doctest::Approx(1.0 + 3.0 * step));
	fixture.Frames(1, 0.6f);
	fixture.Gui.Chord(ImGuiMod_Shift | ImGuiKey_LeftArrow, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.ReadCurve().Keys[1].Time == doctest::Approx(1.0 - 7.0 * step));

	// Delete removes the selected key in a step of its own, and never reaches the editor's editing keys.
	fixture.GlobalDelete = false;
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	fixture.Frames(1);
	CHECK_FALSE(fixture.GlobalDelete);
	CHECK(fixture.GetUndoCount() == undo + 3);
	CHECK(history.GetUndoDescription() == "Delete Key of Curve on Ramp");
	CHECK(fixture.ReadCurve().Keys.size() == 2);
	// Ctrl+A selects every key.
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Drawer());
	CHECK(fixture.Curves->GetEditor().GetView().GetSelection().size() == 2);
	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Frames(1);
	CHECK(fixture.ReadCurve().Keys.size() == 3);

	// A double click on the curve adds a key with the curve's value there.
	Curve const before = fixture.ReadCurve();
	CurveView<Curve> const& view = fixture.Curves->GetEditor().GetView();
	double const time = 0.5;
	glm::vec2 const onCurve(view.ToScreenX(time), view.ToScreenY(before.Evaluate(time)));
	fixture.Gui.MouseClick(ToImVec2(onCurve), ImGuiMouseButton_Left, fixture.Drawer(), 2);
	fixture.Frames(1);
	CHECK(history.GetUndoDescription() == "Add Key to Curve on Ramp");
	CHECK(fixture.ReadCurve().Keys.size() == 4);
	CHECK(ValidateCurve(fixture.ReadCurve()).IsOk());
}

TEST_CASE("CurveAssetEditor: the header's time and value fields are one step each and keep the digits they do not show")
{
	CurveInspectorFixture fixture;
	CommandHistory const& history = fixture.Context.GetHistory();
	Json const keys =
		Json::parse(R"({ "Keys": [ { "Time": 0, "Value": 0 }, { "Time": 1.000123, "Value": 1.234567 }, { "Time": 2, "Value": 0 } ] })");
	REQUIRE(fixture.Operations.SetDataAssetFields(fixture.Asset, keys, "Set Keys").IsOk());
	fixture.Frames(1);
	fixture.Gui.Click(ToImVec2(fixture.Key(1)), fixture.Drawer());
	REQUIRE(fixture.Curves->GetEditor().GetView().IsSelected(0, 1));
	size_t const undo = fixture.GetUndoCount();

	// The time field dragged a little later: the key moves, one step; its digits past the field's format are kept (a drag
	// rounded to the format would leave 1.3, not 1.300123).
	std::optional<std::pair<glm::vec2, glm::vec2>> const timeField = fixture.Curves->GetEditor().FindRect("Time");
	REQUIRE(timeField.has_value());
	glm::vec2 const timeAt = (timeField->first + timeField->second) * 0.5f;
	fixture.Frames(1, 1.0f);
	fixture.Gui.DragPath(ToImVec2(timeAt), ToImVec2(timeAt + glm::vec2(8.0f, 0.0f)), fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(history.GetUndoDescription() == "Move Key of Curve on Ramp");
	float const time = fixture.ReadCurve().Keys[1].Time;
	CHECK(time > 1.000123f);
	CHECK(time < 2.0f);
	CHECK(std::abs(time * 1.0e4f - std::round(time * 1.0e4f)) > 0.05f);

	// The value field dragged up: one step more, the digits kept as well.
	std::optional<std::pair<glm::vec2, glm::vec2>> const valueField = fixture.Curves->GetEditor().FindRect("Value");
	REQUIRE(valueField.has_value());
	glm::vec2 const valueAt((valueField->first.x + valueField->second.x) * 0.5f, (valueField->first.y + valueField->second.y) * 0.5f);
	fixture.Frames(1, 1.0f);
	fixture.Gui.DragPath(ToImVec2(valueAt), ToImVec2(valueAt + glm::vec2(40.0f, 0.0f)), fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(history.GetUndoDescription() == "Set Value of Key of Curve on Ramp");
	float const value = fixture.ReadCurve().Keys[1].Value;
	CHECK(value > 1.234567f);
	CHECK(std::abs(value * 1.0e3f - std::round(value * 1.0e3f)) > 0.05f);
	CHECK(fixture.ReadCurve().Keys[1].Time == time);
	CHECK(ValidateCurve(fixture.ReadCurve()).IsOk());
}

TEST_CASE("CurveAssetEditor: a curve whose file is read-only is drawn and changes nothing")
{
	CurveInspectorFixture fixture;
	std::string const bytes = fixture.ReadFile();
	size_t const undo = fixture.GetUndoCount();
	Testing::ReadOnlyFileScope const readOnly(fixture.File);
	fixture.Frames(2);
	CHECK(fixture.Curves->GetEditor().GetDrawRecord().CurveKeys == 3);
	glm::vec2 const key = fixture.Key(1);
	fixture.Gui.DragPath(ToImVec2(key), ToImVec2(key + glm::vec2(40.0f, -30.0f)), fixture.Drawer());
	fixture.Gui.Press(ImGuiKey_Delete, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);
	CHECK(fixture.ReadCurve().Keys.size() == 3);
}
