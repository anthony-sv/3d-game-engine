#include "HeadlessImGui.h"
#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/GameInputRouter.h"
#include "Editor/InputBindingCapture.h"
#include "Editor/Panels/InputActionsPanel.h"
#include "Editor/PlayMode.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionMap.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Asset/InputRebinding.h"
#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/InputCapture.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Core/Timestep.h"
#include "Strada/ImGui/ImGuiLayer.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;
using Strada::Testing::HeadlessImGui;

namespace
{
	using Record = InputActionsPanel::DrawRecord;

	ImVec2 ToImVec2(Record::Point point)
	{
		return ImVec2(point[0], point[1]);
	}

	// Leaves Input and the binding table as every other test expects them.
	struct InputScope
	{
		InputScope()
		{
			Input::Reset();
			InputRebinding::Clear();
		}
		~InputScope()
		{
			Input::Reset();
			InputRebinding::Clear();
		}
		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	constexpr char const* GameActions = R"({ "Actions": [
		{ "Name": "Jump", "Bindings": [ { "Controls": "Key/Space" }, { "Controls": "Gamepad/A" } ] },
		{ "Name": "Fire", "Bindings": [ { "Controls": "Key/Space" } ] },
		{ "Name": "Steer", "Type": "Axis", "Bindings": [ { "Controls": "Key/A Key/D" }, { "Controls": "Gamepad/LeftX" } ] },
		{ "Name": "Move", "Type": "Vector2", "Bindings": [ { "Controls": "Key/W Key/S Key/A Key/D" }, { "Controls": "Gamepad/LeftStick" } ] }
	] })";

	// A project whose Project Settings name an actions asset (Input/Game.sinput), shown by an Input Actions panel placed at
	// the top left of the display.
	struct InputActionsPanelFixture
	{
		InputScope Inputs;
		Testing::AssetManagerScope Assets;
		HeadlessImGui Gui;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		InputActionsPanel Panel;
		AssetHandle Asset;
		std::filesystem::path File;
		bool Open = true;
		bool Draw = true;
		GameInputRouter Router;

		explicit InputActionsPanelFixture(bool nameActions = true)
		{
			REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			Asset = CreateActions("Input/Game.sinput", GameActions);
			File = Context.GetProject()->GetAssetDirectory() / "Input" / "Game.sinput";
			if (nameActions)
			{
				REQUIRE(Operations.ApplyProjectSettings(Json::object({{"InputActions", Asset.ToString()}})).IsOk());
			}
			Router.SetCaptureFilter(
				[this](Event const& event, ImGuiLayer::InputContext const& context)
				{
					return Panel.TakeCaptureEvent(event, context);
				});
			Frames(3);
		}

		AssetHandle CreateActions(std::string_view path, char const* fields)
		{
			DataAssetType const* const type = DataAssetRegistry::FindByName("InputActions");
			REQUIRE(type != nullptr);
			Result<AssetHandle> const created = Operations.CreateDataAsset(type->GetAssetType(), path, Json::parse(fields));
			REQUIRE(created.IsOk());
			return created.GetValue();
		}

		std::function<void()> Drawer()
		{
			return [this]
			{
				if (!Draw)
				{
					return;
				}
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(1500.0f, 900.0f));
				Panel.OnImGuiRender(Operations, Open);
			};
		}

		void Frames(int count, float seconds = 1.0f / 60.0f)
		{
			for (int frame = 0; frame < count; frame++)
			{
				Gui.Frame(Drawer(), seconds);
			}
		}

		Record const& GetRecord() const { return Panel.GetLastDraw(); }

		Record::ActionRow const& GetAction(std::string_view name) const
		{
			for (Record::ActionRow const& row : GetRecord().Actions)
			{
				if (row.Name == name)
				{
					return row;
				}
			}
			FAIL("no row for the action ", name);
			return GetRecord().Actions.front();
		}

		InputActionMap ReadMap() const
		{
			Ref<TypedDataAsset<InputActionMap>> const asset = AssetManager::GetAsset<TypedDataAsset<InputActionMap>>(Asset);
			REQUIRE(asset != nullptr);
			return asset->GetData();
		}

		std::string ReadFile() const
		{
			Result<std::string> const text = FileSystem::ReadTextFile(File);
			REQUIRE(text.IsOk());
			return text.GetValue();
		}

		size_t GetUndoCount() const { return Context.GetHistory().GetUndoCount(); }

		static ImGuiLayer::InputContext At(Record::Point point)
		{
			ImGuiLayer::InputContext context;
			context.Cursor = glm::vec2(point[0], point[1]);
			context.Viewport = ImGui::GetMainViewport()->ID;
			return context;
		}

		// A key pressed in an editor window, through the game input router as the ImGui layer's filter sends it: returns
		// whether the editor's interface would receive it.
		bool PressKey(KeyCode key) { return Router.Route(KeyPressedEvent(key, false), At({0.0f, 0.0f})); }

		bool ReleaseKey(KeyCode key) { return Router.Route(KeyReleasedEvent(key), At({0.0f, 0.0f})); }

		// Clicks a button of the panel: the press and the release reach ImGui (the capture takes nothing while it does not
		// listen), with frames between them, then draws a frame that records what the click did.
		void Click(Record::Point point)
		{
			Gui.Click(ToImVec2(point), Drawer());
			Frames(1);
		}

		// A left click as the editor's window reports it: the router sees the press first and passes it on (or not), and
		// ImGui gets the press in the same frame, then the release.
		void ClickThroughRouter(Record::Point point)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddMousePosEvent(point[0], point[1]);
			Frames(1);
			if (Router.Route(MouseButtonPressedEvent(MouseButton::Left), At(point)))
			{
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			}
			Frames(1);
			if (Router.Route(MouseButtonReleasedEvent(MouseButton::Left), At(point)))
			{
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			}
			Frames(2);
		}

		// Presses a key that ImGui receives, then draws a frame that records what it did.
		void PressImGuiKey(ImGuiKey key)
		{
			Gui.Press(key, Drawer());
			Frames(1);
		}
	};

	// Points along a row of a window, for finding a widget by hovering.
	std::optional<ImVec2> FindHovered(HeadlessImGui& imgui, std::function<void()> const& draw, ImGuiWindow const* window, ImGuiID id)
	{
		for (float y = window->Pos.y + 1.0f; y < window->Pos.y + window->Size.y - 1.0f; y += 2.0f)
		{
			ImVec2 const point(window->Pos.x + window->Size.x * 0.5f, y);
			ImGui::GetIO().AddMousePosEvent(point.x, point.y);
			imgui.Frame(draw);
			if (GImGui->HoveredId == id)
			{
				return point;
			}
		}
		return std::nullopt;
	}
}

TEST_CASE("InputActionsPanel: the project's actions and bindings show with their conflicts, and drawing changes nothing")
{
	InputActionsPanelFixture fixture;
	std::string const bytes = fixture.ReadFile();
	size_t const undo = fixture.GetUndoCount();
	fixture.Frames(2);
	Record const& record = fixture.GetRecord();
	CHECK(record.Drawn);
	CHECK(record.Asset == fixture.Asset);
	CHECK(record.FollowsProject);
	CHECK(record.Editable);
	CHECK_FALSE(record.Live);
	CHECK(record.Status.empty());
	REQUIRE(record.Actions.size() == 4);
	CHECK(record.Actions[0].Name == "Jump");
	CHECK(record.Actions[2].Type == InputActionType::Axis);
	CHECK(record.Actions[3].Type == InputActionType::Vector2);
	CHECK(record.Actions[0].Value.empty());

	// Conflicts in the order of the map: a key both read, a stick and one of its axes.
	Record::ActionRow const& jump = fixture.GetAction("Jump");
	REQUIRE(jump.Bindings.size() == 2);
	CHECK(jump.Bindings[0].Controls == "Key/Space");
	CHECK(jump.Bindings[0].Conflicts == std::vector<std::string>{"Fire"});
	CHECK(jump.Bindings[1].Conflicts.empty());
	CHECK(fixture.GetAction("Fire").Bindings[0].Conflicts == std::vector<std::string>{"Jump"});
	Record::ActionRow const& steer = fixture.GetAction("Steer");
	CHECK(steer.Bindings[0].Conflicts == std::vector<std::string>{"Move"});
	CHECK(steer.Bindings[1].Conflicts == std::vector<std::string>{"Move"});
	CHECK(fixture.GetAction("Move").Bindings[0].Conflicts == std::vector<std::string>{"Steer"});
	// One button per part of a composite: an axis has two, a vector four.
	CHECK(steer.Bindings[0].Parts.size() == 2);
	CHECK(steer.Bindings[1].Parts.empty());
	CHECK(fixture.GetAction("Move").Bindings[0].Parts.size() == 4);

	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);
	CHECK_FALSE(fixture.Context.IsDirty());
}

TEST_CASE("InputActionsPanel: without actions named by the project it says so, and an asset opened in it is shown instead")
{
	InputActionsPanelFixture fixture(false);
	Record const& record = fixture.GetRecord();
	CHECK_FALSE(record.Asset.IsValid());
	CHECK(record.Actions.empty());
	CHECK(record.Status.find("names no input actions") != std::string::npos);

	fixture.Panel.OpenAsset(fixture.Asset);
	fixture.Frames(1);
	CHECK(record.Asset == fixture.Asset);
	CHECK_FALSE(record.FollowsProject);
	CHECK(record.Actions.size() == 4);

	// Another project forgets what was opened.
	REQUIRE(fixture.Operations.CreateProject(fixture.Directory.GetPath() / "Other", "Other", Scene("Main")).IsOk());
	fixture.Frames(1);
	CHECK_FALSE(record.Asset.IsValid());
	CHECK(record.FollowsProject);
	CHECK_FALSE(fixture.Panel.GetOpenedAsset().IsValid());
}

TEST_CASE("InputActionsPanel: a key pressed while a binding listens binds it as one step, and reaches neither the editor nor the game")
{
	InputActionsPanelFixture fixture;
	std::string const bytes = fixture.ReadFile();
	size_t const undo = fixture.GetUndoCount();
	// Not listening: keys go on to the editor as usual.
	CHECK(fixture.PressKey(KeyCode::J));
	CHECK(fixture.ReleaseKey(KeyCode::J));

	fixture.Click(fixture.GetAction("Jump").Bindings[0].Bind);
	CHECK(fixture.Panel.GetCapture().IsListening());
	CHECK(fixture.GetAction("Jump").Bindings[0].Listening);
	CHECK(fixture.GetAction("Jump").Bindings[0].ListeningPart == -1);
	CHECK_FALSE(fixture.GetRecord().Prompt.empty());
	CHECK(fixture.GetUndoCount() == undo);

	// The physical key, taken before ImGui and the game; its repeats too, until it is released.
	CHECK_FALSE(fixture.PressKey(KeyCode::J));
	CHECK_FALSE(Input::IsKeyDown(KeyCode::J));
	CHECK_FALSE(fixture.Router.Route(KeyPressedEvent(KeyCode::J, true), InputActionsPanelFixture::At({0.0f, 0.0f})));
	fixture.Frames(1);
	CHECK_FALSE(fixture.Panel.GetCapture().IsListening());
	CHECK(fixture.ReadMap().Actions[0].Bindings[0].Controls == "Key/J");
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Bind Jump to Key/J on Game");
	CHECK_FALSE(fixture.Router.Route(KeyPressedEvent(KeyCode::J, true), InputActionsPanelFixture::At({0.0f, 0.0f})));
	CHECK(fixture.ReleaseKey(KeyCode::J));
	CHECK(fixture.Router.Route(KeyPressedEvent(KeyCode::J, true), InputActionsPanelFixture::At({0.0f, 0.0f})));
	CHECK(fixture.PressKey(KeyCode::K));
	CHECK(fixture.ReleaseKey(KeyCode::K));
	fixture.Frames(1);
	// Jump no longer shares Space with Fire.
	CHECK(fixture.GetAction("Jump").Bindings[0].Conflicts.empty());
	CHECK(fixture.GetAction("Fire").Bindings[0].Conflicts.empty());
	std::string const after = fixture.ReadFile();

	REQUIRE(fixture.Operations.Undo().IsOk());
	fixture.Frames(1);
	CHECK(fixture.ReadFile() == bytes);
	REQUIRE(fixture.Operations.Redo().IsOk());
	fixture.Frames(1);
	CHECK(fixture.ReadFile() == after);
	CHECK_FALSE(fixture.Context.IsDirty());

	// The key the binding reads already: no step.
	fixture.Frames(40);
	fixture.Click(fixture.GetAction("Jump").Bindings[0].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	CHECK_FALSE(fixture.PressKey(KeyCode::J));
	fixture.ReleaseKey(KeyCode::J);
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.ReadFile() == after);
	CHECK(fixture.GetRecord().Status == "Binding 0 of Jump reads Key/J already.");
}

TEST_CASE("InputActionsPanel: a mouse button pressed over the prompt binds it, and a click elsewhere ends the capture and goes on")
{
	InputActionsPanelFixture fixture;
	size_t const undo = fixture.GetUndoCount();
	fixture.Click(fixture.GetAction("Fire").Bindings[0].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	Record::Point const prompt = fixture.GetAction("Fire").Bindings[0].ControlsField;
	CHECK_FALSE(fixture.Router.Route(MouseButtonPressedEvent(MouseButton::Right), InputActionsPanelFixture::At(prompt)));
	CHECK(fixture.Router.Route(MouseButtonReleasedEvent(MouseButton::Right), InputActionsPanelFixture::At(prompt)));
	fixture.Frames(1);
	CHECK(fixture.ReadMap().Actions[1].Bindings[0].Controls == "Mouse/Right");
	CHECK(fixture.GetUndoCount() == undo + 1);

	// Elsewhere: the click goes on to the editor, and nothing is bound.
	fixture.Frames(40);
	fixture.Click(fixture.GetAction("Fire").Bindings[0].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	CHECK(fixture.Router.Route(MouseButtonPressedEvent(MouseButton::Left), InputActionsPanelFixture::At({1450.0f, 880.0f})));
	CHECK(fixture.Router.Route(MouseButtonReleasedEvent(MouseButton::Left), InputActionsPanelFixture::At({1450.0f, 880.0f})));
	fixture.Frames(1);
	CHECK_FALSE(fixture.Panel.GetCapture().IsListening());
	CHECK(fixture.GetRecord().Status == "Nothing was bound.");
	CHECK(fixture.ReadMap().Actions[1].Bindings[0].Controls == "Mouse/Right");
	CHECK(fixture.GetUndoCount() == undo + 1);
}

TEST_CASE("InputActionsPanel: a composite's part takes a key, a stick binding takes a stick, and a gamepad's button binds")
{
	InputActionsPanelFixture fixture;
	size_t const undo = fixture.GetUndoCount();

	// The positive part of Steer's keys.
	fixture.Click(fixture.GetAction("Steer").Bindings[0].Parts[1]);
	CHECK(fixture.GetAction("Steer").Bindings[0].ListeningPart == 1);
	CHECK_FALSE(fixture.PressKey(KeyCode::L));
	fixture.ReleaseKey(KeyCode::L);
	fixture.Frames(1);
	CHECK(fixture.ReadMap().Actions[2].Bindings[0].Controls == "Key/A Key/L");
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Bind Steer to Key/A Key/L on Game");

	// Move's stick binding: a key is taken and binds nothing; the right stick moved past the threshold binds.
	fixture.Click(fixture.GetAction("Move").Bindings[1].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	CHECK(fixture.Panel.GetCapture().GetTarget().Kind == InputCaptureTarget::Stick);
	CHECK_FALSE(fixture.PressKey(KeyCode::X));
	fixture.ReleaseKey(KeyCode::X);
	fixture.Frames(1);
	CHECK(fixture.Panel.GetCapture().IsListening());
	CHECK(fixture.GetRecord().Prompt.find("stick") != std::string::npos);
	Input::GamepadState pad = Input::GetRestingGamepadState();
	pad.Axes[static_cast<size_t>(GamepadAxis::RightX)] = 0.9f;
	Input::InjectGamepadState(0, pad);
	fixture.Frames(1);
	CHECK_FALSE(fixture.Panel.GetCapture().IsListening());
	CHECK(fixture.ReadMap().Actions[3].Bindings[1].Controls == "Gamepad/RightStick");
	Input::ReleaseInjectedGamepads();

	// Jump's gamepad binding takes a gamepad button.
	fixture.Click(fixture.GetAction("Jump").Bindings[1].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	pad = Input::GetRestingGamepadState();
	pad.Buttons[static_cast<size_t>(GamepadButton::Y)] = true;
	Input::InjectGamepadState(1, pad);
	fixture.Frames(1);
	CHECK(fixture.ReadMap().Actions[0].Bindings[1].Controls == "Gamepad/Y");
	CHECK(fixture.GetUndoCount() == undo + 3);
}

TEST_CASE("InputActionsPanel: Escape and the time limit end a capture with nothing bound")
{
	InputActionsPanelFixture fixture;
	std::string const bytes = fixture.ReadFile();
	size_t const undo = fixture.GetUndoCount();

	fixture.Click(fixture.GetAction("Jump").Bindings[0].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	CHECK_FALSE(fixture.PressKey(KeyCode::Escape));
	fixture.ReleaseKey(KeyCode::Escape);
	fixture.Frames(1);
	CHECK_FALSE(fixture.Panel.GetCapture().IsListening());
	CHECK(fixture.GetRecord().Status == "Nothing was bound.");

	// The time limit counts the frames' seconds.
	fixture.Frames(40);
	fixture.Click(fixture.GetAction("Jump").Bindings[0].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	fixture.Frames(3, 3.0f);
	CHECK(fixture.Panel.GetCapture().IsListening());
	fixture.Frames(1, 3.0f);
	CHECK_FALSE(fixture.Panel.GetCapture().IsListening());
	CHECK(fixture.GetRecord().Status.find("within 10 seconds") != std::string::npos);

	// Bind pressed again while it listens ends it too.
	fixture.Frames(40);
	fixture.Click(fixture.GetAction("Jump").Bindings[0].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	fixture.Frames(40);
	fixture.Click(fixture.GetAction("Jump").Bindings[0].Bind);
	CHECK_FALSE(fixture.Panel.GetCapture().IsListening());
	// In the editor the press of that click reaches the capture first, which it ends as a click away from the prompt; the
	// click then ends it rather than starting it again.
	fixture.Frames(40);
	fixture.Click(fixture.GetAction("Jump").Bindings[0].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	fixture.Frames(40);
	fixture.ClickThroughRouter(fixture.GetAction("Jump").Bindings[0].Bind);
	CHECK_FALSE(fixture.Panel.GetCapture().IsListening());
	// A click on another binding's button starts that one.
	fixture.Frames(40);
	fixture.Click(fixture.GetAction("Jump").Bindings[0].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	fixture.Frames(40);
	fixture.ClickThroughRouter(fixture.GetAction("Fire").Bindings[0].Bind);
	CHECK(fixture.Panel.GetCapture().IsListening());
	CHECK(fixture.Panel.GetCapture().GetTarget().Action == "Fire");
	fixture.Panel.OnHidden();

	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);
}

TEST_CASE("InputActionsPanel: a capture ends when its binding changes elsewhere or the panel is not drawn, and keys go on then")
{
	InputActionsPanelFixture fixture;

	// input-actions.set's path removes the binding listened to.
	fixture.Click(fixture.GetAction("Jump").Bindings[1].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	InputActionMap changed = fixture.ReadMap();
	changed.Actions[0].Bindings.pop_back();
	Json patch = Json::object();
	patch["Actions"] = JsonTraits<std::vector<InputAction>>::ToJson(changed.Actions);
	REQUIRE(fixture.Operations.SetDataAssetFields(fixture.Asset, patch).IsOk());
	size_t const undo = fixture.GetUndoCount();
	fixture.Frames(1);
	CHECK_FALSE(fixture.Panel.GetCapture().IsListening());
	CHECK(fixture.GetRecord().Status.find("changed elsewhere") != std::string::npos);
	CHECK(fixture.PressKey(KeyCode::J));
	fixture.ReleaseKey(KeyCode::J);

	// The layer hides the panel.
	fixture.Frames(40);
	fixture.Click(fixture.GetAction("Jump").Bindings[0].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	fixture.Panel.OnHidden();
	CHECK_FALSE(fixture.Panel.GetCapture().IsListening());
	CHECK(fixture.PressKey(KeyCode::J));
	fixture.ReleaseKey(KeyCode::J);

	// A panel that stops being drawn without a word: its capture lets the next press go and ends.
	fixture.Frames(40);
	fixture.Click(fixture.GetAction("Jump").Bindings[0].Bind);
	REQUIRE(fixture.Panel.GetCapture().IsListening());
	fixture.Draw = false;
	fixture.Frames(2);
	CHECK(fixture.PressKey(KeyCode::J));
	fixture.ReleaseKey(KeyCode::J);
	CHECK_FALSE(fixture.Panel.GetCapture().IsListening());
	fixture.Draw = true;
	fixture.Frames(2);
	CHECK(fixture.ReadMap().Actions[0].Bindings[0].Controls == "Key/Space");
	CHECK(fixture.GetUndoCount() == undo);
}

TEST_CASE("InputActionsPanel: names and controls typed into the table are one step each when committed, and a refusal says why")
{
	InputActionsPanelFixture fixture;
	size_t const undo = fixture.GetUndoCount();
	std::function<void()> const draw = fixture.Drawer();

	// A name, committed with Enter: typing alone changes nothing.
	fixture.Click(fixture.GetAction("Jump").NameField);
	REQUIRE(ImGui::GetIO().WantTextInput);
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, draw);
	fixture.Gui.Type("Hop");
	fixture.Frames(2);
	CHECK(fixture.ReadMap().Actions[0].Name == "Jump");
	CHECK(fixture.GetUndoCount() == undo);
	fixture.PressImGuiKey(ImGuiKey_Enter);
	CHECK(fixture.ReadMap().Actions[0].Name == "Hop");
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Rename Jump to Hop on Game");
	CHECK(fixture.GetRecord().Selected == std::optional<uint32_t>(0));

	// Escape leaves the field as the asset has it.
	fixture.Click(fixture.GetAction("Hop").NameField);
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, draw);
	fixture.Gui.Type("Skip");
	fixture.Frames(1);
	fixture.PressImGuiKey(ImGuiKey_Escape);
	fixture.Frames(1);
	CHECK(fixture.ReadMap().Actions[0].Name == "Hop");
	CHECK(fixture.GetUndoCount() == undo + 1);

	// Controls that name nothing: refused, nothing changes, and the panel says why.
	fixture.Click(fixture.GetAction("Fire").Bindings[0].ControlsField);
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, draw);
	fixture.Gui.Type("Key/Nothing");
	fixture.PressImGuiKey(ImGuiKey_Enter);
	CHECK(fixture.ReadMap().Actions[1].Bindings[0].Controls == "Key/Space");
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.GetRecord().Status.find("Nothing") != std::string::npos);

	// Controls that do, left by a click elsewhere: one step, and the refusal's line goes.
	fixture.Click(fixture.GetAction("Fire").Bindings[0].ControlsField);
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, draw);
	fixture.Gui.Type("Key/F Key/G");
	fixture.Frames(1);
	fixture.Click(fixture.GetAction("Fire").Row);
	CHECK(fixture.ReadMap().Actions[1].Bindings[0].Controls == "Key/F Key/G");
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Controls of Fire on Game");
	CHECK(fixture.GetRecord().Status.empty());
}

TEST_CASE("InputActionsPanel: a type chosen, actions and bindings added and removed are one step each, undone byte for byte")
{
	InputActionsPanelFixture fixture;
	std::string const bytes = fixture.ReadFile();
	size_t const undo = fixture.GetUndoCount();
	std::function<void()> const draw = fixture.Drawer();

	// The type combo: Fire becomes an axis.
	fixture.Click(fixture.GetAction("Fire").TypeCombo);
	REQUIRE_FALSE(GImGui->OpenPopupStack.empty());
	ImGuiWindow* popup = GImGui->OpenPopupStack.back().Window;
	REQUIRE(popup != nullptr);
	std::optional<ImVec2> const axis = FindHovered(fixture.Gui, draw, popup, popup->GetID("Axis"));
	REQUIRE(axis.has_value());
	fixture.Gui.Click(*axis, draw);
	fixture.Frames(1);
	CHECK(fixture.ReadMap().Actions[1].Type == InputActionType::Axis);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set Type of Fire to Axis on Game");

	// A new action, selected, with a binding added and removed, then removed itself.
	fixture.Click(fixture.GetRecord().AddAction);
	REQUIRE(fixture.ReadMap().Actions.size() == 5);
	CHECK(fixture.ReadMap().Actions[4].Name == "Action");
	CHECK(fixture.GetRecord().Selected == std::optional<uint32_t>(4));
	fixture.Click(fixture.GetAction("Action").AddBinding);
	CHECK(fixture.ReadMap().Actions[4].Bindings.size() == 1);
	fixture.Click(fixture.GetRecord().AddAction);
	CHECK(fixture.ReadMap().Actions[5].Name == "Action2");
	fixture.Click(fixture.GetAction("Action").Bindings[0].Remove);
	CHECK(fixture.ReadMap().Actions[4].Bindings.empty());
	fixture.Click(fixture.GetAction("Action").Remove);
	REQUIRE(fixture.ReadMap().Actions.size() == 5);
	CHECK(fixture.ReadMap().Actions[4].Name == "Action2");
	CHECK(fixture.GetUndoCount() == undo + 6);

	for (int step = 0; step < 6; step++)
	{
		REQUIRE(fixture.Operations.Undo().IsOk());
	}
	fixture.Frames(1);
	CHECK(fixture.ReadFile() == bytes);
}

TEST_CASE("InputActionsPanel: the selected action's fields edit below the table, one step per interaction")
{
	InputActionsPanelFixture fixture;
	size_t const undo = fixture.GetUndoCount();
	std::function<void()> const draw = fixture.Drawer();
	CHECK_FALSE(fixture.GetRecord().Selected.has_value());
	fixture.Click(fixture.GetAction("Steer").Row);
	REQUIRE(fixture.GetRecord().Selected == std::optional<uint32_t>(2));

	// The details' first row is the action's name, which the field editor writes as it is typed: one step in all.
	ImGuiWindow const* details = nullptr;
	for (ImGuiWindow* window : GImGui->Windows)
	{
		if (window->Active && std::string_view(window->Name).find("##details") != std::string_view::npos)
		{
			details = window;
		}
	}
	REQUIRE(details != nullptr);
	// The first widget down the right half of the details.
	std::optional<ImVec2> name;
	for (float y = details->Pos.y + 1.0f; y < details->Pos.y + details->Size.y && !name; y += 2.0f)
	{
		ImVec2 const point(details->Pos.x + details->Size.x * 0.75f, y);
		ImGui::GetIO().AddMousePosEvent(point.x, point.y);
		fixture.Gui.Frame(draw);
		if (GImGui->HoveredId != 0)
		{
			name = point;
		}
	}
	REQUIRE(name.has_value());
	fixture.Gui.Click(*name, draw);
	fixture.Frames(1);
	REQUIRE(ImGui::GetIO().WantTextInput);
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, draw);
	fixture.Gui.Type("Turn");
	fixture.Frames(2);
	fixture.PressImGuiKey(ImGuiKey_Enter);
	fixture.Frames(2);
	CHECK(fixture.ReadMap().Actions[2].Name == "Turn");
	CHECK(fixture.GetUndoCount() == undo + 1);
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadMap().Actions[2].Name == "Steer");
}

TEST_CASE("InputActionsPanel: an actions file that is read-only is drawn and changes nothing")
{
	InputActionsPanelFixture fixture;
	std::string const bytes = fixture.ReadFile();
	size_t const undo = fixture.GetUndoCount();
	Testing::ReadOnlyFileScope const readOnly(fixture.File);
	fixture.Frames(2);
	CHECK_FALSE(fixture.GetRecord().Editable);
	CHECK_FALSE(fixture.GetRecord().Status.empty());
	CHECK(fixture.GetRecord().Actions.size() == 4);
	fixture.Click(fixture.GetAction("Jump").Bindings[0].Bind);
	CHECK_FALSE(fixture.Panel.GetCapture().IsListening());
	fixture.Click(fixture.GetRecord().AddAction);
	fixture.Click(fixture.GetAction("Jump").Remove);
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);
}

TEST_CASE("InputActionsPanel: while the game plays, the Value column shows each action's value in the running scene")
{
	Testing::PhysicsSystemScope physics;
	InputActionsPanelFixture fixture;
	EditorScripts scripts{fixture.Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
	PlayMode play{fixture.Context, scripts};
	REQUIRE(play.Start(EditorPlayState::Play, 640, 480).IsOk());
	Input::SetKeyState(KeyCode::Space, true);
	Input::SetKeyState(KeyCode::D, true);
	play.Update(Timestep(1.0f / 60.0f));
	fixture.Frames(1);
	Record const& record = fixture.GetRecord();
	CHECK(record.Live);
	CHECK(fixture.GetAction("Jump").Value == "1.00");
	CHECK(fixture.GetAction("Jump").Down);
	CHECK(fixture.GetAction("Steer").Value == "1.00");
	CHECK(fixture.GetAction("Move").Value == "1.00, 0.00");
	Input::SetKeyState(KeyCode::Space, false);
	play.Update(Timestep(1.0f / 60.0f));
	fixture.Frames(1);
	CHECK(fixture.GetAction("Jump").Value == "0.00");
	CHECK_FALSE(fixture.GetAction("Jump").Down);

	// Another asset is not what the game evaluates.
	AssetHandle const other = fixture.CreateActions("Input/Other.sinput", R"({ "Actions": [ { "Name": "Jump" } ] })");
	fixture.Panel.OpenAsset(other);
	fixture.Frames(1);
	CHECK_FALSE(record.Live);
	CHECK(fixture.GetAction("Jump").Value.empty());
	fixture.Panel.OpenAsset(AssetHandle());

	play.Stop();
	fixture.Frames(1);
	CHECK_FALSE(record.Live);
	CHECK(fixture.GetAction("Jump").Value.empty());
}

TEST_CASE("InputBindingCapture: a part's control replaces that part, and a capture's kind follows the action's type")
{
	InputControl const key{InputControlKind::Key, static_cast<uint16_t>(KeyCode::K)};
	CHECK(ReplaceInputBindingControl("Key/A Key/D", -1, key).GetValue() == "Key/K");
	CHECK(ReplaceInputBindingControl("", -1, key).GetValue() == "Key/K");
	CHECK(ReplaceInputBindingControl("Key/A Key/D", 0, key).GetValue() == "Key/K Key/D");
	CHECK(ReplaceInputBindingControl("key/w  KEY/S Key/A Key/D", 3, key).GetValue() == "Key/W Key/S Key/A Key/K");
	CHECK_FALSE(ReplaceInputBindingControl("Key/A", 0, key).IsOk());
	CHECK_FALSE(ReplaceInputBindingControl("Key/A Key/D", 2, key).IsOk());
	CHECK_FALSE(ReplaceInputBindingControl("Key/A Key/D", 0, InputControl{InputControlKind::PadStick, 0}).IsOk());
	CHECK_FALSE(ReplaceInputBindingControl("Key/Nothing Key/D", 0, key).IsOk());

	CHECK(GetInputBindingCaptureKind(InputActionType::Button, -1) == InputCaptureTarget::Control);
	CHECK(GetInputBindingCaptureKind(InputActionType::Axis, -1) == InputCaptureTarget::Control);
	CHECK(GetInputBindingCaptureKind(InputActionType::Vector2, -1) == InputCaptureTarget::Stick);
	CHECK(GetInputBindingCaptureKind(InputActionType::Vector2, 2) == InputCaptureTarget::Part);

	// Not listening, it takes nothing; listening, typed characters reach nobody.
	InputScope inputs;
	InputBindingCapture capture;
	ImGuiLayer::InputContext const context;
	CHECK_FALSE(capture.TakeEvent(KeyPressedEvent(KeyCode::A, false), context));
	capture.Start(InputBindingCaptureTarget{});
	CHECK(capture.TakeEvent(KeyTypedEvent('a'), context));
	CHECK_FALSE(capture.TakeEvent(KeyReleasedEvent(KeyCode::A), context));
	// Without a prompt a mouse press is elsewhere.
	CHECK_FALSE(capture.TakeEvent(MouseButtonPressedEvent(MouseButton::Left), context));
	CHECK(capture.Update(0.0f) == InputBindingCaptureOutcome::Cancelled);
	CHECK(capture.Update(0.0f) == InputBindingCaptureOutcome::Waiting);
	// A key injected into Input (automation's input.set) is captured too.
	capture.Start(InputBindingCaptureTarget{});
	Input::SetKeyState(KeyCode::Q, true);
	CHECK(capture.Update(0.0f) == InputBindingCaptureOutcome::Captured);
	CHECK(capture.GetControl() == InputControl{InputControlKind::Key, static_cast<uint16_t>(KeyCode::Q)});
}
