#include "HeadlessImGui.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorScripts.h"
#include "Editor/GameInputRouter.h"
#include "Editor/Panels/GameViewPanel.h"
#include "Editor/PlayMode.h"

#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Scene/Entity.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <initializer_list>
#include <string_view>
#include <utility>

using namespace Strada;
using Strada::Testing::HeadlessImGui;

namespace
{
	using Context = ImGuiLayer::InputContext;

	// Input is global: tests that set it restore it.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	Context At(glm::vec2 cursor)
	{
		Context context;
		context.Cursor = cursor;
		return context;
	}

	// A game view whose area, from (100, 50) to (740, 410), shows a 1280x720 game at half its size: the game's pixels are
	// twice the cursor's distance from the area's corner.
	GameInputRouter::View PlayingView()
	{
		GameInputRouter::View view;
		view.Playing = true;
		view.Focused = true;
		view.AreaMin = glm::vec2(100.0f, 50.0f);
		view.AreaMax = glm::vec2(740.0f, 410.0f);
		view.ImageMin = view.AreaMin;
		view.ImageSize = glm::vec2(640.0f, 360.0f);
		view.RenderSize = glm::uvec2(1280, 720);
		return view;
	}

	// The game's pixel (400, 400).
	constexpr glm::vec2 InView(300.0f, 250.0f);
	constexpr glm::vec2 OutsideView(900.0f, 100.0f);

	// Sends window input to a router as the ImGui layer does; each call returns whether ImGui receives the event.
	struct RouterFixture
	{
		InputScope Scope;
		GameInputRouter Router;

		RouterFixture() { Router.SetView(PlayingView()); }

		// Key events name the key the layout types as the physical key unless the context says otherwise.
		bool Key(KeyCode key, bool down, Context context = {})
		{
			context.LayoutKey = context.LayoutKey == KeyCode::Unknown ? key : context.LayoutKey;
			if (down)
			{
				return Router.Route(KeyPressedEvent(key, false), context);
			}
			return Router.Route(KeyReleasedEvent(key), context);
		}

		bool Repeat(KeyCode key, Context context = {})
		{
			context.LayoutKey = key;
			return Router.Route(KeyPressedEvent(key, true), context);
		}

		bool Button(MouseButton button, bool down, glm::vec2 cursor)
		{
			if (down)
			{
				return Router.Route(MouseButtonPressedEvent(button), At(cursor));
			}
			return Router.Route(MouseButtonReleasedEvent(button), At(cursor));
		}

		bool Move(glm::vec2 cursor) { return Router.Route(MouseMovedEvent(cursor.x, cursor.y), At(cursor)); }
		bool Scroll(glm::vec2 offset, glm::vec2 cursor) { return Router.Route(MouseScrolledEvent(offset.x, offset.y), At(cursor)); }
		bool Type(uint32_t codepoint) { return Router.Route(KeyTypedEvent(codepoint), Context()); }

		// Gives the game the input the way a user does: a click on its view.
		void ClickView()
		{
			Move(InView);
			Button(MouseButton::Left, true, InView);
			Button(MouseButton::Left, false, InView);
		}

		// Ends a frame and starts the next as the editor does: Input's frame flags clear, then the router releases what the
		// game gave back, before the game updates.
		void NextFrame()
		{
			Input::EndFrame();
			Router.BeginFrame();
		}
	};
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: the editor gets every event until a click on the game view while playing")
{
	// Editing (or simulating): the game view is a preview and a click on it changes nothing.
	GameInputRouter::View editing = PlayingView();
	editing.Playing = false;
	Router.SetView(editing);
	CHECK(Move(InView));
	CHECK(Button(MouseButton::Left, true, InView));
	CHECK(Button(MouseButton::Left, false, InView));
	CHECK(Key(KeyCode::Space, true));
	CHECK(Key(KeyCode::Space, false));
	CHECK(Type('a'));
	CHECK(Scroll(glm::vec2(0.0f, 1.0f), InView));
	CHECK_FALSE(Router.HasInput());
	CHECK_FALSE(Router.TakeFocusRequest());
	CHECK_FALSE(Input::IsMouseButtonPressed(MouseButton::Left));
	CHECK_FALSE(Input::IsKeyPressed(KeyCode::Space));
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f));
	CHECK(Input::GetMousePosition() == glm::vec2(0.0f));

	// Playing: keys, the wheel and clicks elsewhere stay the editor's until the game takes the input.
	Router.SetView(PlayingView());
	CHECK(Key(KeyCode::W, true));
	CHECK_FALSE(Input::IsKeyDown(KeyCode::W));
	CHECK(Key(KeyCode::W, false));
	CHECK(Button(MouseButton::Left, true, OutsideView));
	CHECK(Button(MouseButton::Left, false, OutsideView));
	CHECK(Scroll(glm::vec2(0.0f, 1.0f), InView));
	CHECK(Move(InView));
	CHECK_FALSE(Router.HasInput());
	CHECK_FALSE(Input::IsMouseButtonPressed(MouseButton::Left));
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f));
	CHECK(Input::GetMousePosition() == glm::vec2(0.0f));
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: the click that gives the game the input reaches it in the same frame")
{
	// Where the game last saw the cursor.
	Input::SetMousePosition(glm::vec2(5.0f, 5.0f));
	NextFrame();

	CHECK(Move(InView));
	// ImGui gets the click too: it focuses the game view with it.
	CHECK(Button(MouseButton::Left, true, InView));
	CHECK(Router.HasInput());
	CHECK(Router.TakeFocusRequest());
	CHECK_FALSE(Router.TakeFocusRequest());
	CHECK(Input::IsMouseButtonPressed(MouseButton::Left));
	CHECK(Input::GetMousePosition() == glm::vec2(400.0f, 400.0f));
	// The cursor moved while the editor had the input: the jump is no movement for the game.
	CHECK(Input::GetMouseDelta() == glm::vec2(0.0f));
	CHECK(Button(MouseButton::Left, false, InView));
	CHECK(Input::IsMouseButtonReleased(MouseButton::Left));

	// From then on clicks on the view are the game's alone (releases reach both sides, which ignore unknown releases).
	NextFrame();
	glm::vec2 const elsewhere = InView + glm::vec2(10.0f, 0.0f);
	CHECK_FALSE(Button(MouseButton::Right, true, elsewhere));
	CHECK(Input::IsMouseButtonPressed(MouseButton::Right));
	CHECK(Button(MouseButton::Right, false, elsewhere));
	CHECK(Input::IsMouseButtonReleased(MouseButton::Right));
	CHECK(Router.HasInput());
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: while the game has the input every key is the game's alone")
{
	ClickView();
	NextFrame();

	// Keys that would navigate ImGui, the scene view's tools, Escape, Alt and the editing keys.
	for (KeyCode const key : {KeyCode::Space, KeyCode::Enter, KeyCode::Tab, KeyCode::Up, KeyCode::Escape, KeyCode::LeftAlt, KeyCode::W,
	                          KeyCode::E, KeyCode::R, KeyCode::F, KeyCode::G, KeyCode::Q, KeyCode::Delete, KeyCode::F2})
	{
		CAPTURE(KeyCodeToString(key));
		CHECK_FALSE(Key(key, true));
		CHECK(Input::IsKeyPressed(key));
		CHECK(Key(key, false));
		CHECK(Input::IsKeyReleased(key));
	}
	// The editor's shortcuts with Control.
	Context control;
	control.Control = true;
	CHECK_FALSE(Key(KeyCode::LeftControl, true, control));
	for (KeyCode const key : {KeyCode::Z, KeyCode::Y, KeyCode::D, KeyCode::A, KeyCode::S, KeyCode::N, KeyCode::O, KeyCode::B})
	{
		CAPTURE(KeyCodeToString(key));
		CHECK_FALSE(Key(key, true, control));
		CHECK(Input::IsKeyDown(key));
		CHECK(Key(key, false, control));
	}
	// Typing reaches nobody: games read keys.
	CHECK_FALSE(Type('w'));
	CHECK(Router.HasInput());
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: Shift+Esc gives the input back and reaches neither side")
{
	ClickView();
	NextFrame();
	Context shift;
	shift.Shift = true;
	CHECK_FALSE(Key(KeyCode::LeftShift, true, shift));
	CHECK_FALSE(Key(KeyCode::W, true, shift));

	// With another modifier Escape is the game's.
	Context shiftControl = shift;
	shiftControl.Control = true;
	CHECK_FALSE(Key(KeyCode::Escape, true, shiftControl));
	CHECK(Input::IsKeyPressed(KeyCode::Escape));
	CHECK(Key(KeyCode::Escape, false, shiftControl));
	CHECK(Router.HasInput());
	NextFrame();

	CHECK_FALSE(Key(KeyCode::Escape, true, shift));
	CHECK_FALSE(Router.HasInput());
	CHECK_FALSE(Input::IsKeyPressed(KeyCode::Escape));
	// The game sees what it held released when its next frame starts.
	CHECK(Input::IsKeyDown(KeyCode::W));
	NextFrame();
	CHECK_FALSE(Input::IsKeyDown(KeyCode::W));
	CHECK(Input::IsKeyReleased(KeyCode::W));
	CHECK(Input::IsKeyReleased(KeyCode::LeftShift));

	// The editor has the keys again; a key the game held repeats for nobody.
	CHECK(Key(KeyCode::Space, true));
	CHECK_FALSE(Input::IsKeyDown(KeyCode::Space));
	CHECK(Repeat(KeyCode::W));
	CHECK_FALSE(Input::IsKeyDown(KeyCode::W));
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: the play shortcuts stay the editor's, named by the key the layout types")
{
	ClickView();
	NextFrame();
	Context control;
	control.Control = true;
	for (bool const shift : {false, true})
	{
		for (bool const alt : {false, true})
		{
			CAPTURE(shift);
			CAPTURE(alt);
			Context chord = control;
			chord.Shift = shift;
			chord.Alt = alt;
			CHECK(Key(KeyCode::P, true, chord));
			CHECK_FALSE(Input::IsKeyDown(KeyCode::P));
			CHECK(Key(KeyCode::P, false, chord));
		}
	}

	// On a layout that types 'p' with another key (Dvorak: the one in R's place), that key is the shortcut, and P's place
	// is not.
	Context dvorakP = control;
	dvorakP.LayoutKey = KeyCode::P;
	CHECK(Key(KeyCode::R, true, dvorakP));
	CHECK_FALSE(Input::IsKeyDown(KeyCode::R));
	CHECK(Key(KeyCode::R, false, dvorakP));
	Context dvorakL = control;
	dvorakL.LayoutKey = KeyCode::L;
	CHECK_FALSE(Key(KeyCode::P, true, dvorakL));
	CHECK(Input::IsKeyDown(KeyCode::P));
	CHECK(Key(KeyCode::P, false, dvorakL));

	// Without the shortcut modifier P is the game's.
	CHECK_FALSE(Key(KeyCode::P, true));
	CHECK(Key(KeyCode::P, false));

	// Where shortcuts use Cmd (macOS), Cmd+P is the editor's and Control+P the game's.
	Router.SetShortcutsUseSuper(true);
	Context command;
	command.Super = true;
	CHECK(Key(KeyCode::P, true, command));
	CHECK(Key(KeyCode::P, false, command));
	CHECK_FALSE(Key(KeyCode::P, true, control));
	CHECK(Router.HasInput());
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: a click anywhere but the game view is the editor's and gives the input back")
{
	ClickView();
	NextFrame();
	CHECK_FALSE(Key(KeyCode::W, true));
	CHECK(Button(MouseButton::Left, true, OutsideView));
	CHECK_FALSE(Router.HasInput());
	CHECK_FALSE(Input::IsMouseButtonDown(MouseButton::Left));
	CHECK(Button(MouseButton::Left, false, OutsideView));
	NextFrame();
	CHECK(Input::IsKeyReleased(KeyCode::W));

	// Something over the view (a window, a popup, a modal dialog) covers it: clicks there are the editor's as well.
	glm::vec2 const covered(150.0f, 80.0f);
	Router.SetCoverageTest(
		[](glm::vec2 position)
		{
			return position.x < 200.0f && position.y < 100.0f;
		});
	CHECK(Button(MouseButton::Left, true, covered));
	CHECK_FALSE(Router.HasInput());
	CHECK_FALSE(Input::IsMouseButtonDown(MouseButton::Left));
	CHECK(Button(MouseButton::Left, false, covered));
	ClickView();
	CHECK(Router.HasInput());
	CHECK(Button(MouseButton::Left, true, covered));
	CHECK_FALSE(Router.HasInput());
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: clicks from another window over the game view's area are the editor's")
{
	// A panel dragged out of the main window has a window of its own, which can lie over the view: the operating system
	// sends the clicks there to that window.
	Context panel = At(InView);
	panel.Viewport = PlayingView().Viewport + 1;
	CHECK(Router.Route(MouseButtonPressedEvent(MouseButton::Left), panel));
	CHECK_FALSE(Router.HasInput());
	CHECK_FALSE(Input::IsMouseButtonDown(MouseButton::Left));
	CHECK(Router.Route(MouseButtonReleasedEvent(MouseButton::Left), panel));

	// While the game has the input, such a click gives it back, and the wheel there scrolls the panel.
	ClickView();
	NextFrame();
	REQUIRE(Router.HasInput());
	CHECK(Router.Route(MouseScrolledEvent(0.0f, 1.0f), panel));
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f));
	CHECK(Router.HasInput());
	CHECK(Router.Route(MouseButtonPressedEvent(MouseButton::Left), panel));
	CHECK_FALSE(Router.HasInput());
	CHECK_FALSE(Input::IsMouseButtonDown(MouseButton::Left));
	CHECK(Router.Route(MouseButtonReleasedEvent(MouseButton::Left), panel));

	// A locked cursor stays the game's whichever window reports its clicks.
	Router.SetGameCursorMode(CursorMode::Locked);
	ClickView();
	NextFrame();
	CHECK_FALSE(Router.Route(MouseButtonPressedEvent(MouseButton::Right), panel));
	CHECK(Input::IsMouseButtonPressed(MouseButton::Right));
	CHECK(Router.HasInput());
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: presses keep to the side that got them")
{
	// A drag from the editor that ends over the game is the editor's: the game sees neither its press nor its release.
	CHECK(Button(MouseButton::Left, true, OutsideView));
	CHECK(Move(InView));
	CHECK(Button(MouseButton::Left, false, InView));
	CHECK_FALSE(Router.HasInput());
	CHECK_FALSE(Input::IsMouseButtonPressed(MouseButton::Left));
	CHECK_FALSE(Input::IsMouseButtonReleased(MouseButton::Left));

	// A key the editor holds when the game takes the input stays the editor's: its repeats never reach the game.
	CHECK(Key(KeyCode::LeftControl, true));
	ClickView();
	NextFrame();
	CHECK(Repeat(KeyCode::LeftControl));
	CHECK_FALSE(Input::IsKeyDown(KeyCode::LeftControl));
	// A key the game holds repeats for it.
	CHECK_FALSE(Key(KeyCode::D, true));
	CHECK_FALSE(Repeat(KeyCode::D));
	CHECK(Input::IsKeyDown(KeyCode::D));

	// A drag that starts in the game is the game's to its end, wherever it ends, and the cursor stays the game's on the way.
	CHECK_FALSE(Button(MouseButton::Left, true, InView));
	CHECK(Move(OutsideView));
	CHECK(Input::GetMousePosition() == glm::vec2(1600.0f, 100.0f));
	CHECK(Button(MouseButton::Left, false, OutsideView));
	CHECK(Input::IsMouseButtonReleased(MouseButton::Left));
	CHECK(Router.HasInput());
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: the game's cursor is in its pixels and moves only while it has the input")
{
	CHECK(Move(InView));
	CHECK(Input::GetMousePosition() == glm::vec2(0.0f));
	ClickView();
	NextFrame();
	CHECK(Move(InView + glm::vec2(5.0f, -2.5f)));
	CHECK(Input::GetMousePosition() == glm::vec2(410.0f, 395.0f));
	CHECK(Input::GetMouseDelta() == glm::vec2(10.0f, -5.0f));

	// The view moves and grows under a still cursor: the game sees the cursor at its new pixels without movement.
	NextFrame();
	GameInputRouter::View larger = PlayingView();
	larger.ImageMin = glm::vec2(0.0f, 50.0f);
	larger.ImageSize = glm::vec2(1280.0f, 720.0f);
	Router.SetView(larger);
	CHECK(Input::GetMousePosition() == glm::vec2(305.0f, 197.5f));
	CHECK(Input::GetMouseDelta() == glm::vec2(0.0f));

	// Without the input the game's cursor stays where it was.
	GameInputRouter::View unfocused = larger;
	unfocused.Focused = false;
	Router.SetView(unfocused);
	CHECK(Move(OutsideView));
	CHECK(Input::GetMousePosition() == glm::vec2(305.0f, 197.5f));
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: the wheel over the game view is the game's while it has the input")
{
	CHECK(Scroll(glm::vec2(0.0f, 1.0f), InView));
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f));
	ClickView();
	CHECK_FALSE(Scroll(glm::vec2(0.0f, 2.0f), InView));
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f, 2.0f));
	// Elsewhere the wheel scrolls the editor's panels.
	CHECK(Scroll(glm::vec2(0.0f, 3.0f), OutsideView));
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f, 2.0f));
	CHECK(Router.HasInput());
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: a locked cursor keeps every click, movement and scroll for the game")
{
	Router.SetGameCursorMode(CursorMode::Locked);
	CHECK(Router.GetCursorMode() == CursorMode::Normal);
	ClickView();
	NextFrame();
	CHECK(Router.GetCursorMode() == CursorMode::Locked);

	// Where a locked cursor is means nothing to the editor.
	CHECK_FALSE(Move(OutsideView));
	CHECK_FALSE(Button(MouseButton::Left, true, OutsideView));
	CHECK(Input::IsMouseButtonPressed(MouseButton::Left));
	CHECK(Button(MouseButton::Left, false, OutsideView));
	CHECK_FALSE(Scroll(glm::vec2(0.0f, 1.0f), OutsideView));
	CHECK(Router.HasInput());

	// Shift+Esc frees the cursor and a click on the view locks it again.
	Context shift;
	shift.Shift = true;
	CHECK_FALSE(Key(KeyCode::Escape, true, shift));
	CHECK(Router.GetCursorMode() == CursorMode::Normal);
	ClickView();
	CHECK(Router.GetCursorMode() == CursorMode::Locked);
	Router.SetGameCursorMode(CursorMode::Hidden);
	CHECK(Router.GetCursorMode() == CursorMode::Hidden);
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: the game gives the input back when its view loses the focus or hides, or playing stops")
{
	GameInputRouter::View unfocused = PlayingView();
	unfocused.Focused = false;
	GameInputRouter::View stopped = PlayingView();
	stopped.Playing = false;
	std::array<GameInputRouter::View, 3> const lost = {unfocused, stopped, GameInputRouter::View()};
	for (GameInputRouter::View const& view : lost)
	{
		Router.SetView(PlayingView());
		ClickView();
		NextFrame();
		CHECK_FALSE(Key(KeyCode::Space, true));
		Router.SetView(view);
		CHECK_FALSE(Router.HasInput());
		// The game updates before the editor draws, so it sees the release in its next frame.
		CHECK(Input::IsKeyDown(KeyCode::Space));
		NextFrame();
		CHECK(Input::IsKeyReleased(KeyCode::Space));
		CHECK(Key(KeyCode::Space, false));
		NextFrame();
	}

	// Input taken back in a frame the game takes the input again is released before the game's new input.
	Router.SetView(PlayingView());
	ClickView();
	NextFrame();
	CHECK_FALSE(Key(KeyCode::W, true));
	Router.SetView(unfocused);
	Router.SetView(PlayingView());
	ClickView();
	CHECK_FALSE(Input::IsKeyDown(KeyCode::W));
	CHECK(Input::IsKeyReleased(KeyCode::W));
	CHECK(Input::IsMouseButtonPressed(MouseButton::Left));
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: playing starts with the input and asks for the game view's focus")
{
	Router.Grant();
	CHECK(Router.HasInput());
	CHECK(Router.TakeFocusRequest());
	// The view takes the focus in the frame it draws next.
	Router.SetView(PlayingView());
	CHECK(Router.HasInput());
	CHECK_FALSE(Key(KeyCode::Space, true));
	CHECK(Input::IsKeyPressed(KeyCode::Space));
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: a locked cursor moves only with the window showing the game view")
{
	Router.SetGameCursorMode(CursorMode::Locked);
	ClickView();
	NextFrame();
	CHECK_FALSE(Move(InView + glm::vec2(10.0f, 0.0f)));
	glm::vec2 const position = Input::GetMousePosition();
	glm::vec2 const delta = Input::GetMouseDelta();
	CHECK(delta == glm::vec2(20.0f, 0.0f));

	// The window of a panel dragged out of the editor lying over the locked window's middle reports where the hidden cursor
	// really is: neither the game nor ImGui sees it.
	Context panel = At(InView + glm::vec2(-150.0f, 80.0f));
	panel.Viewport = PlayingView().Viewport + 1;
	CHECK_FALSE(Router.Route(MouseMovedEvent(panel.Cursor.x, panel.Cursor.y), panel));
	CHECK(Input::GetMousePosition() == position);
	CHECK(Input::GetMouseDelta() == delta);

	// The locked window's movement goes on from where it was.
	CHECK_FALSE(Move(InView + glm::vec2(15.0f, 0.0f)));
	CHECK(Input::GetMousePosition() == position + glm::vec2(10.0f, 0.0f));
	CHECK(Input::GetMouseDelta() == delta + glm::vec2(10.0f, 0.0f));

	// A cursor the game does not lock moves with whichever window it is over.
	Router.SetGameCursorMode(CursorMode::Hidden);
	CHECK(Router.Route(MouseMovedEvent(panel.Cursor.x, panel.Cursor.y), panel));
	CHECK(Input::GetMousePosition() == glm::vec2(100.0f, 560.0f));
	CHECK(Router.HasInput());
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: only a locked game cursor takes the windows' cursor modes")
{
	// The windows of the view's viewport and of a panel dragged out of the editor.
	uint32_t const viewWindow = PlayingView().Viewport;
	uint32_t const panelWindow = viewWindow + 1;

	// Without the input every window shows the editor's cursors, whatever the game asked for.
	Router.SetGameCursorMode(CursorMode::Locked);
	CHECK(Router.GetCursorMode() == CursorMode::Normal);
	CHECK(Router.GetWindowCursorMode(viewWindow) == CursorMode::Normal);
	CHECK(Router.GetWindowCursorMode(panelWindow) == CursorMode::Normal);

	// While the game has the input, the window showing the game view locks the cursor and every other window hides it.
	ClickView();
	CHECK(Router.GetCursorMode() == CursorMode::Locked);
	CHECK(Router.GetWindowCursorMode(viewWindow) == CursorMode::Locked);
	CHECK(Router.GetWindowCursorMode(panelWindow) == CursorMode::Hidden);

	// The view dragged out into the panel's window takes the lock with it.
	GameInputRouter::View moved = PlayingView();
	moved.Viewport = panelWindow;
	Router.SetView(moved);
	CHECK(Router.HasInput());
	CHECK(Router.GetWindowCursorMode(panelWindow) == CursorMode::Locked);
	CHECK(Router.GetWindowCursorMode(viewWindow) == CursorMode::Hidden);
	Router.SetView(PlayingView());

	// A hidden cursor is hidden by the game view over itself alone: the windows keep showing the editor's cursors.
	Router.SetGameCursorMode(CursorMode::Hidden);
	CHECK(Router.GetCursorMode() == CursorMode::Hidden);
	CHECK(Router.GetWindowCursorMode(viewWindow) == CursorMode::Normal);
	CHECK(Router.GetWindowCursorMode(panelWindow) == CursorMode::Normal);
	Router.SetGameCursorMode(CursorMode::Normal);
	CHECK(Router.GetWindowCursorMode(viewWindow) == CursorMode::Normal);
	CHECK(Router.GetWindowCursorMode(panelWindow) == CursorMode::Normal);

	// Giving the input back frees a locked cursor everywhere.
	Router.SetGameCursorMode(CursorMode::Locked);
	CHECK(Router.GetWindowCursorMode(viewWindow) == CursorMode::Locked);
	Router.Release();
	CHECK(Router.GetWindowCursorMode(viewWindow) == CursorMode::Normal);
	CHECK(Router.GetWindowCursorMode(panelWindow) == CursorMode::Normal);
}

TEST_CASE_FIXTURE(RouterFixture, "GameInput: giving the input back releases the game's keys and buttons, not its gamepads")
{
	Input::GamepadState gamepad;
	gamepad.Connected = true;
	gamepad.Buttons[static_cast<size_t>(GamepadButton::A)] = true;
	Input::SetGamepadState(0, gamepad);
	ClickView();
	NextFrame();
	CHECK_FALSE(Key(KeyCode::W, true));
	CHECK_FALSE(Button(MouseButton::Right, true, InView));

	Router.Release();
	NextFrame();
	CHECK(Input::IsKeyReleased(KeyCode::W));
	CHECK(Input::IsMouseButtonReleased(MouseButton::Right));
	// Gamepads are polled every frame rather than shared: a button held on one stays held, without a release and a second
	// press when it is polled again.
	CHECK(Input::IsGamepadButtonDown(0, GamepadButton::A));
	CHECK_FALSE(Input::IsGamepadButtonReleased(0, GamepadButton::A));
	Input::SetGamepadState(0, gamepad);
	CHECK(Input::IsGamepadButtonDown(0, GamepadButton::A));
	CHECK_FALSE(Input::IsGamepadButtonPressed(0, GamepadButton::A));
}

namespace
{
	ImGuiKey ToImGuiKey(KeyCode key)
	{
		switch (key)
		{
			case KeyCode::Space:
				return ImGuiKey_Space;
			case KeyCode::Enter:
				return ImGuiKey_Enter;
			case KeyCode::Escape:
				return ImGuiKey_Escape;
			case KeyCode::Tab:
				return ImGuiKey_Tab;
			case KeyCode::Up:
				return ImGuiKey_UpArrow;
			case KeyCode::Down:
				return ImGuiKey_DownArrow;
			case KeyCode::LeftAlt:
				return ImGuiKey_LeftAlt;
			case KeyCode::LeftControl:
				return ImGuiKey_LeftCtrl;
			case KeyCode::LeftShift:
				return ImGuiKey_LeftShift;
			case KeyCode::P:
				return ImGuiKey_P;
			case KeyCode::W:
				return ImGuiKey_W;
			default:
				break;
		}
		CHECK_MESSAGE(false, "the test has no ImGui key for " << KeyCodeToString(key));
		return ImGuiKey_None;
	}

	// The editor's game view drawn headless beside another window while the editor plays: without a GPU the view shows a
	// message where the game would be, and lays out the same. Window input goes through the router as the ImGui layer sends
	// it, and what ImGui receives is queued for it as its GLFW backend does. Docked, the windows are laid out like the
	// editor's: the view shares the middle of a dock space with another tab ("Tab"), and the other window is on the right.
	struct GameViewFixture
	{
		// The game view's area starts below its title bar and toolbar; this is well inside it.
		static constexpr glm::vec2 ViewPoint = glm::vec2(500.0f, 450.0f);
		// The other window's button.
		static constexpr glm::vec2 ButtonPoint = glm::vec2(1100.0f, 170.0f);

		bool Docked = false;
		InputScope Scope;
		HeadlessImGui Interface;
		EditorContext Context;
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};
		GameInputRouter Router;
		GameViewPanel View{Router};
		bool Open = true;
		// Draws a window over the game view.
		bool Covered = false;
		bool OpenModal = false;
		// What the editor's interface did: play shortcuts (EditorLayer::HandleShortcuts), clicks on the other window's button.
		int Shortcuts = 0;
		int Clicks = 0;
		// Where the other window's button was drawn last.
		glm::vec2 ButtonCenter = glm::vec2(0.0f);

		explicit GameViewFixture(bool docked = false)
			: Docked(docked)
		{
			// As in the editor: play shortcuts use Cmd where ImGui's shortcuts do (macOS).
			ImGuiIO& io = ImGui::GetIO();
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			Router.SetShortcutsUseSuper(io.ConfigMacOSXBehaviors);
			if (Docked)
			{
				io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
			}
			REQUIRE(Play.Start(EditorPlayState::Play, 640, 480).IsOk());
			Frame();
			Frame();
		}

		void Draw()
		{
			if (Docked)
			{
				DrawDockspace();
			}
			else
			{
				ImGui::SetNextWindowPos(ImVec2(100.0f, 100.0f));
				ImGui::SetNextWindowSize(ImVec2(800.0f, 600.0f));
			}
			View.OnImGuiRender(Context, Open);

			if (Docked)
			{
				ImGui::Begin("Tab");
				ImGui::End();
			}
			else
			{
				ImGui::SetNextWindowPos(ImVec2(1000.0f, 100.0f));
				ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
			}
			ImGui::Begin("Other");
			if (ImGui::Button("Button", ImVec2(200.0f, 100.0f)))
			{
				Clicks++;
			}
			ButtonCenter = glm::vec2(ImGui::GetItemRectMin().x + ImGui::GetItemRectSize().x * 0.5f,
			                         ImGui::GetItemRectMin().y + ImGui::GetItemRectSize().y * 0.5f);
			ImGui::End();

			if (Covered)
			{
				ImGui::SetNextWindowPos(ImVec2(400.0f, 350.0f));
				ImGui::SetNextWindowSize(ImVec2(200.0f, 150.0f));
				ImGui::Begin("Cover");
				ImGui::End();
			}
			if (std::exchange(OpenModal, false))
			{
				ImGui::OpenPopup("Modal");
			}
			ImGui::SetNextWindowPos(ImVec2(1000.0f, 500.0f));
			if (ImGui::BeginPopupModal("Modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::TextUnformatted("A dialog");
				ImGui::EndPopup();
			}
			if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, ImGuiInputFlags_RouteGlobal))
			{
				Shortcuts++;
			}
			// As the editor does after its panels (without platform windows headless, only ImGui's side shows).
			View.UpdateWindowCursors();
		}

		void DrawDockspace()
		{
			ImGuiID const dockspace = ImGui::DockSpaceOverViewport(ImGui::GetID("Dockspace"), ImGui::GetMainViewport());
			if (std::exchange(m_BuildLayout, false))
			{
				ImGui::DockBuilderRemoveNode(dockspace);
				ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
				ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
				ImGuiID center = dockspace;
				ImGuiID const right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.3f, nullptr, &center);
				ImGui::DockBuilderDockWindow("Tab", center);
				ImGui::DockBuilderDockWindow("Game View", center);
				ImGui::DockBuilderDockWindow("Other", right);
				ImGui::DockBuilderFinish(dockspace);
			}
		}

		// Draws a frame that brings a window to the front (a tab of its dock node), and a frame in which it is.
		void FocusWindow(char const* name)
		{
			Interface.Frame(
				[this, name]
				{
					Draw();
					ImGui::SetWindowFocus(name);
				});
			Frame();
		}

		// The middle of the game view's area, from where ImGui last drew it.
		static glm::vec2 GetViewCenter()
		{
			ImGuiWindow const* const window = ImGui::FindWindowByName("Game View");
			REQUIRE(window != nullptr);
			ImVec2 const center = window->InnerRect.GetCenter();
			return glm::vec2(center.x, center.y);
		}

		// One editor frame after the window's events: the router releases what the game gave back, the game would update,
		// the editor draws, and Input's frame ends.
		void Frame()
		{
			Router.BeginFrame();
			Interface.Frame(
				[this]
				{
					Draw();
				});
			Input::EndFrame();
		}

		// Routes an event from the main window (unless the context names another viewport); what ImGui receives goes to it
		// as ImGui's GLFW backend passes it on (modifiers first).
		bool Send(Event const& event, ImGuiLayer::InputContext context)
		{
			if (context.Viewport == 0)
			{
				context.Viewport = ImGui::GetMainViewport()->ID;
			}
			if (!Router.Route(event, context))
			{
				return false;
			}
			ImGuiIO& io = ImGui::GetIO();
			io.AddKeyEvent(ImGuiMod_Ctrl, context.Control);
			io.AddKeyEvent(ImGuiMod_Shift, context.Shift);
			io.AddKeyEvent(ImGuiMod_Alt, context.Alt);
			io.AddKeyEvent(ImGuiMod_Super, context.Super);
			switch (event.GetEventType())
			{
				case EventType::KeyPressed:
					if (!static_cast<KeyPressedEvent const&>(event).IsRepeat())
					{
						io.AddKeyEvent(ToImGuiKey(static_cast<KeyEvent const&>(event).GetKeyCode()), true);
					}
					break;
				case EventType::KeyReleased:
					io.AddKeyEvent(ToImGuiKey(static_cast<KeyEvent const&>(event).GetKeyCode()), false);
					break;
				case EventType::KeyTyped:
					io.AddInputCharacter(static_cast<KeyTypedEvent const&>(event).GetCodepoint());
					break;
				case EventType::MouseButtonPressed:
				case EventType::MouseButtonReleased:
					io.AddMouseButtonEvent(static_cast<int>(static_cast<MouseButtonEvent const&>(event).GetMouseButton()),
					                       event.GetEventType() == EventType::MouseButtonPressed);
					break;
				case EventType::MouseMoved:
				{
					auto const& move = static_cast<MouseMovedEvent const&>(event);
					io.AddMousePosEvent(move.GetX(), move.GetY());
					break;
				}
				case EventType::MouseScrolled:
				{
					auto const& scroll = static_cast<MouseScrolledEvent const&>(event);
					io.AddMouseWheelEvent(scroll.GetXOffset(), scroll.GetYOffset());
					break;
				}
				default:
					break;
			}
			return true;
		}

		bool SendKey(KeyCode key, bool down, ImGuiLayer::InputContext context = {})
		{
			context.LayoutKey = context.LayoutKey == KeyCode::Unknown ? key : context.LayoutKey;
			if (down)
			{
				return Send(KeyPressedEvent(key, false), context);
			}
			return Send(KeyReleasedEvent(key), context);
		}

		// Moves the cursor there and clicks with the left button, a frame per step.
		void Click(glm::vec2 position)
		{
			Send(MouseMovedEvent(position.x, position.y), At(position));
			Frame();
			Send(MouseButtonPressedEvent(MouseButton::Left), At(position));
			Frame();
			Send(MouseButtonReleasedEvent(MouseButton::Left), At(position));
			Frame();
		}

		static bool IsFocused(std::string_view window)
		{
			ImGuiWindow const* const focused = ImGui::GetCurrentContext()->NavWindow;
			return focused != nullptr && window == focused->Name;
		}

	private:
		bool m_BuildLayout = true;
	};
}

TEST_CASE("GameInput: a click on the game view focuses it and gives the game the input with the click")
{
	GameViewFixture fixture;
	fixture.Interface.Frame(
		[&fixture]
		{
			fixture.Draw();
			ImGui::SetWindowFocus("Other");
		});
	fixture.Frame();
	REQUIRE(GameViewFixture::IsFocused("Other"));

	glm::vec2 const point = GameViewFixture::ViewPoint;
	CHECK(fixture.Send(MouseMovedEvent(point.x, point.y), At(point)));
	fixture.Frame();
	CHECK(fixture.Send(MouseButtonPressedEvent(MouseButton::Left), At(point)));
	// The game updates before the editor draws its next frame: it has the click by then.
	CHECK(fixture.Router.HasInput());
	CHECK(Input::IsMouseButtonPressed(MouseButton::Left));
	// The view renders at its own size here (free aspect): the game's pixels start at the view area's corner, at the
	// window's left edge.
	CHECK(Input::GetMousePosition().x == doctest::Approx(point.x - 100.0f));
	fixture.Frame();
	CHECK(GameViewFixture::IsFocused("Game View"));
	CHECK(fixture.Send(MouseButtonReleasedEvent(MouseButton::Left), At(point)));
	fixture.Frame();
	CHECK(fixture.Router.HasInput());
	CHECK(GameViewFixture::IsFocused("Game View"));
}

TEST_CASE("GameInput: while the game has the input the editor's interface sees no key but the play shortcuts")
{
	GameViewFixture fixture;
	fixture.Click(GameViewFixture::ViewPoint);
	REQUIRE(fixture.Router.HasInput());

	// Keys that would move ImGui's keyboard focus, open the view's resolution list, press buttons or toggle menus.
	for (KeyCode const key : {KeyCode::Tab, KeyCode::Down, KeyCode::Up, KeyCode::Space, KeyCode::Enter, KeyCode::Escape, KeyCode::LeftAlt})
	{
		CAPTURE(KeyCodeToString(key));
		CHECK_FALSE(fixture.SendKey(key, true));
		fixture.Frame();
		CHECK(Input::IsKeyDown(key));
		CHECK_FALSE(ImGui::IsKeyDown(ToImGuiKey(key)));
		CHECK(fixture.SendKey(key, false));
		fixture.Frame();
	}
	CHECK(ImGui::GetCurrentContext()->OpenPopupStack.empty());
	CHECK(GameViewFixture::IsFocused("Game View"));
	CHECK(fixture.Clicks == 0);
	CHECK(fixture.Router.HasInput());

	// Ctrl+P (Cmd+P on macOS, where ImGui's Ctrl shortcuts take Cmd) is the editor's: its shortcut sees it, the game does
	// not.
	bool const command = ImGui::GetIO().ConfigMacOSXBehaviors;
	KeyCode const modifier = command ? KeyCode::LeftSuper : KeyCode::LeftControl;
	ImGuiLayer::InputContext shortcut;
	shortcut.Control = !command;
	shortcut.Super = command;
	CHECK_FALSE(fixture.SendKey(modifier, true, shortcut));
	CHECK(fixture.SendKey(KeyCode::P, true, shortcut));
	fixture.Frame();
	CHECK(fixture.Shortcuts == 1);
	CHECK_FALSE(Input::IsKeyDown(KeyCode::P));
	CHECK(fixture.SendKey(KeyCode::P, false, shortcut));
	CHECK(fixture.SendKey(modifier, false));
	fixture.Frame();
	CHECK(fixture.Shortcuts == 1);
}

TEST_CASE("GameInput: a click on another window is the editor's, focuses it and gives the input back")
{
	GameViewFixture fixture;
	fixture.Click(GameViewFixture::ViewPoint);
	REQUIRE(fixture.Router.HasInput());
	CHECK_FALSE(fixture.SendKey(KeyCode::W, true));

	glm::vec2 const button = GameViewFixture::ButtonPoint;
	CHECK(fixture.Send(MouseMovedEvent(button.x, button.y), At(button)));
	fixture.Frame();
	CHECK(fixture.Send(MouseButtonPressedEvent(MouseButton::Left), At(button)));
	CHECK_FALSE(fixture.Router.HasInput());
	CHECK_FALSE(Input::IsMouseButtonDown(MouseButton::Left));
	// The game sees W released when the frame starts.
	CHECK(Input::IsKeyDown(KeyCode::W));
	fixture.Frame();
	CHECK_FALSE(Input::IsKeyDown(KeyCode::W));
	CHECK(fixture.Send(MouseButtonReleasedEvent(MouseButton::Left), At(button)));
	fixture.Frame();
	CHECK(GameViewFixture::IsFocused("Other"));
	CHECK(fixture.Clicks == 1);
	CHECK_FALSE(fixture.Router.HasInput());
}

TEST_CASE("GameInput: a window or a modal dialog over the game view keeps the click")
{
	GameViewFixture fixture;
	fixture.Covered = true;
	fixture.Frame();
	fixture.Frame();

	// The window's click is the editor's; the game gets nothing.
	glm::vec2 const covered(450.0f, 400.0f);
	CHECK(fixture.Send(MouseMovedEvent(covered.x, covered.y), At(covered)));
	fixture.Frame();
	CHECK(fixture.Send(MouseButtonPressedEvent(MouseButton::Left), At(covered)));
	CHECK_FALSE(fixture.Router.HasInput());
	CHECK_FALSE(Input::IsMouseButtonDown(MouseButton::Left));
	fixture.Frame();
	CHECK(fixture.Send(MouseButtonReleasedEvent(MouseButton::Left), At(covered)));
	fixture.Frame();
	CHECK(GameViewFixture::IsFocused("Cover"));

	// Beside the window the view takes the click.
	glm::vec2 const uncovered(200.0f, 600.0f);
	fixture.Click(uncovered);
	CHECK(fixture.Router.HasInput());
	CHECK(GameViewFixture::IsFocused("Game View"));

	// A modal dialog takes the focus, so the game gives the input back, and keeps the view from taking clicks.
	fixture.OpenModal = true;
	fixture.Frame();
	fixture.Frame();
	CHECK_FALSE(fixture.Router.HasInput());
	CHECK(fixture.Send(MouseButtonPressedEvent(MouseButton::Left), At(uncovered)));
	CHECK_FALSE(fixture.Router.HasInput());
	CHECK_FALSE(Input::IsMouseButtonDown(MouseButton::Left));
	CHECK(fixture.Send(MouseButtonReleasedEvent(MouseButton::Left), At(uncovered)));
	fixture.Frame();
	CHECK_FALSE(fixture.Router.HasInput());
}

TEST_CASE("GameInput: the game view gives the input back when another window takes the focus or playing stops")
{
	GameViewFixture fixture;
	fixture.Click(GameViewFixture::ViewPoint);
	REQUIRE(fixture.Router.HasInput());
	fixture.Interface.Frame(
		[&fixture]
		{
			fixture.Draw();
			ImGui::SetWindowFocus("Other");
		});
	fixture.Frame();
	CHECK_FALSE(fixture.Router.HasInput());

	fixture.Click(GameViewFixture::ViewPoint);
	CHECK(fixture.Router.HasInput());
	fixture.Play.Stop();
	fixture.Frame();
	CHECK_FALSE(fixture.Router.HasInput());

	// Editing: the view is a preview and a click does not give the game the input.
	fixture.Click(GameViewFixture::ViewPoint);
	CHECK_FALSE(fixture.Router.HasInput());
	CHECK(GameViewFixture::IsFocused("Game View"));
}

TEST_CASE("GameInput: docked like in the editor, the game view takes clicks in its area until another tab hides it")
{
	GameViewFixture fixture(true);
	fixture.FocusWindow("Game View");
	ImGuiWindow const* const view = ImGui::FindWindowByName("Game View");
	REQUIRE(view != nullptr);
	REQUIRE(view->DockIsActive);
	glm::vec2 const point = GameViewFixture::GetViewCenter();

	// The dock space's windows behind the view and the tab hidden behind it in the same node take none of its clicks.
	fixture.Click(point);
	CHECK(fixture.Router.HasInput());
	CHECK(GameViewFixture::IsFocused("Game View"));
	CHECK_FALSE(fixture.SendKey(KeyCode::Space, true));
	CHECK(Input::IsKeyDown(KeyCode::Space));
	CHECK(fixture.SendKey(KeyCode::Space, false));

	// The docked window beside it is the editor's.
	fixture.Click(fixture.ButtonCenter);
	CHECK_FALSE(fixture.Router.HasInput());
	CHECK(fixture.Clicks == 1);
	CHECK(GameViewFixture::IsFocused("Other"));

	// The other tab of the view's node comes to the front: the hidden view gives the input back and takes no clicks.
	fixture.Click(point);
	REQUIRE(fixture.Router.HasInput());
	fixture.FocusWindow("Tab");
	CHECK_FALSE(fixture.Router.HasInput());
	fixture.Click(point);
	CHECK_FALSE(fixture.Router.HasInput());
	CHECK_FALSE(Input::IsMouseButtonDown(MouseButton::Left));
	CHECK(GameViewFixture::IsFocused("Tab"));
}

TEST_CASE("GameInput: docked behind another tab, the game view comes forward with the input when playing starts")
{
	// The editor's layout keeps the game view behind the scene view until Play gives the game the input.
	GameViewFixture fixture(true);
	fixture.FocusWindow("Tab");
	ImGuiWindow const* const view = ImGui::FindWindowByName("Game View");
	REQUIRE(view != nullptr);
	REQUIRE(view->DockIsActive);
	REQUIRE_FALSE(view->DockTabIsVisible);

	// The hidden tab takes the focus in the frame after: ImGui draws it in that frame already, so the view keeps the input
	// rather than giving it back as hidden, and it shows from the frame after that.
	fixture.Router.Grant();
	fixture.Frame();
	CHECK(fixture.Router.HasInput());
	CHECK(GameViewFixture::IsFocused("Game View"));
	fixture.Frame();
	CHECK(fixture.Router.HasInput());
	CHECK(GameViewFixture::IsFocused("Game View"));
	CHECK(view->DockTabIsVisible);

	// The game has the keys at once, and ImGui none of them.
	CHECK_FALSE(fixture.SendKey(KeyCode::Space, true));
	fixture.Frame();
	CHECK(Input::IsKeyDown(KeyCode::Space));
	CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_Space));
	CHECK(fixture.SendKey(KeyCode::Space, false));
	fixture.Frame();
	CHECK_FALSE(Input::IsKeyDown(KeyCode::Space));
	CHECK(fixture.Router.HasInput());
}

TEST_CASE("GameInput: a cursor the game hides is hidden over the game view alone")
{
	GameViewFixture fixture;
	fixture.Router.SetGameCursorMode(CursorMode::Hidden);
	// The cursor ImGui wants once the cursor moved there and a frame was drawn.
	auto const cursorAt = [&fixture](glm::vec2 position)
	{
		fixture.Send(MouseMovedEvent(position.x, position.y), At(position));
		fixture.Frame();
		return ImGui::GetMouseCursor();
	};
	glm::vec2 const point = GameViewFixture::ViewPoint;

	// Until the game has the input, the view shows the editor's cursor.
	CHECK(cursorAt(point) == ImGuiMouseCursor_Arrow);
	fixture.Click(point);
	REQUIRE(fixture.Router.HasInput());
	CHECK(ImGui::GetMouseCursor() == ImGuiMouseCursor_None);

	// Over the view's toolbar and the other window the editor's cursor shows, and the game keeps the input.
	ImGuiWindow const* const window = ImGui::FindWindowByName("Game View");
	REQUIRE(window != nullptr);
	glm::vec2 const toolbar(window->InnerRect.GetCenter().x, window->InnerRect.Min.y + 2.0f);
	CHECK(cursorAt(toolbar) == ImGuiMouseCursor_Arrow);
	CHECK(cursorAt(fixture.ButtonCenter) == ImGuiMouseCursor_Arrow);
	CHECK(cursorAt(point) == ImGuiMouseCursor_None);
	CHECK(fixture.Router.HasInput());

	// A cursor the game shows stays, and a locked one takes the windows' cursors from ImGui instead (UpdateWindowCursors).
	fixture.Router.SetGameCursorMode(CursorMode::Normal);
	CHECK(cursorAt(point + glm::vec2(5.0f, 0.0f)) == ImGuiMouseCursor_Arrow);
	fixture.Router.SetGameCursorMode(CursorMode::Locked);
	fixture.Frame();
	CHECK(ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow);
	CHECK((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) != 0);

	// Hidden again right after: ImGui has the cursors back, and hides the one over the view.
	fixture.Router.SetGameCursorMode(CursorMode::Hidden);
	fixture.Frame();
	CHECK(ImGui::GetMouseCursor() == ImGuiMouseCursor_None);
	CHECK((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == 0);

	// Once the game gave the input back, the view shows the editor's cursor again.
	ImGuiLayer::InputContext shift;
	shift.Shift = true;
	CHECK_FALSE(fixture.SendKey(KeyCode::Escape, true, shift));
	CHECK_FALSE(fixture.Router.HasInput());
	CHECK(fixture.SendKey(KeyCode::Escape, false, shift));
	CHECK(cursorAt(point) == ImGuiMouseCursor_Arrow);
}

TEST_CASE("GameInput: a cursor the game locks takes the editor's windows from ImGui while the game has the input")
{
	GameViewFixture fixture;
	auto const cursorChangesOff = []
	{
		return (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) != 0;
	};
	// The view is drawn in the main window; any other viewport is the window of a panel dragged out of the editor.
	uint32_t const mainWindow = ImGui::GetMainViewport()->ID;
	uint32_t const panelWindow = mainWindow + 1;

	// Until the game has the input, ImGui keeps the cursors.
	fixture.Router.SetGameCursorMode(CursorMode::Locked);
	fixture.Frame();
	CHECK_FALSE(cursorChangesOff());
	CHECK(fixture.Router.GetWindowCursorMode(mainWindow) == CursorMode::Normal);

	// With the click on the view, the main window locks the cursor, the others hide it, and ImGui changes no cursor.
	fixture.Click(GameViewFixture::ViewPoint);
	REQUIRE(fixture.Router.HasInput());
	CHECK(cursorChangesOff());
	CHECK(fixture.Router.GetWindowCursorMode(mainWindow) == CursorMode::Locked);
	CHECK(fixture.Router.GetWindowCursorMode(panelWindow) == CursorMode::Hidden);

	// Shift+Esc gives the cursors back to ImGui.
	ImGuiLayer::InputContext shift;
	shift.Shift = true;
	CHECK_FALSE(fixture.SendKey(KeyCode::Escape, true, shift));
	CHECK(fixture.SendKey(KeyCode::Escape, false, shift));
	fixture.Frame();
	CHECK_FALSE(fixture.Router.HasInput());
	CHECK_FALSE(cursorChangesOff());
	CHECK(fixture.Router.GetWindowCursorMode(mainWindow) == CursorMode::Normal);
	CHECK(fixture.Router.GetWindowCursorMode(panelWindow) == CursorMode::Normal);

	// So does the game freeing the cursor while it keeps the input.
	fixture.Click(GameViewFixture::ViewPoint);
	REQUIRE(cursorChangesOff());
	fixture.Router.SetGameCursorMode(CursorMode::Normal);
	fixture.Frame();
	CHECK_FALSE(cursorChangesOff());
	CHECK(fixture.Router.HasInput());

	// And playing stopping.
	fixture.Router.SetGameCursorMode(CursorMode::Locked);
	fixture.Frame();
	REQUIRE(cursorChangesOff());
	fixture.Play.Stop();
	fixture.Frame();
	CHECK_FALSE(fixture.Router.HasInput());
	CHECK_FALSE(cursorChangesOff());
	CHECK(fixture.Router.GetWindowCursorMode(mainWindow) == CursorMode::Normal);
}

TEST_CASE("GameInput: the game's UI follows the cursor only while the game has the input, which cancels a press it takes back")
{
	GameViewFixture fixture;
	// A button over the whole game, whatever size the view renders at.
	Scene& scene = fixture.Context.GetScene();
	Entity canvas = scene.CreateEntity("Canvas");
	canvas.AddComponent<UICanvasComponent>();
	Entity button = scene.CreateEntity("Button", canvas);
	UIRectComponent& rect = button.AddComponent<UIRectComponent>();
	rect.AnchorMin = glm::vec2(0.0f);
	rect.AnchorMax = glm::vec2(1.0f);
	rect.Size = glm::vec2(0.0f);
	button.AddComponent<UIImageComponent>();
	button.AddComponent<UIButtonComponent>();

	// An editor frame as EditorLayer::OnUpdate runs it: the router releases what the game gave back, the game's UI gets the
	// pointer while the game has the input, the game updates, then the editor draws.
	int clicks = 0;
	auto const frame = [&]
	{
		fixture.Router.BeginFrame();
		fixture.Play.SetUIPointerEnabled(fixture.Router.HasInput());
		fixture.Play.Update(Timestep(1.0f / 60.0f));
		clicks += scene.WasUIButtonClicked(button) ? 1 : 0;
		fixture.Interface.Frame(
			[&fixture]
			{
				fixture.Draw();
			});
		Input::EndFrame();
	};
	glm::vec2 const point = GameViewFixture::ViewPoint;

	// Without the input, the cursor does not reach the game.
	fixture.Send(MouseMovedEvent(point.x, point.y), At(point));
	frame();
	CHECK(scene.GetUIButtonState(button) == UIButtonState::Normal);
	CHECK_FALSE(scene.IsPointerOverUI());
	// The click that gives the game the input clicks its button too.
	fixture.Send(MouseButtonPressedEvent(MouseButton::Left), At(point));
	frame();
	REQUIRE(fixture.Router.HasInput());
	CHECK(scene.GetUIButtonState(button) == UIButtonState::Pressed);
	fixture.Send(MouseButtonReleasedEvent(MouseButton::Left), At(point));
	frame();
	CHECK(clicks == 1);
	CHECK(scene.GetUIButtonState(button) == UIButtonState::Hovered);
	CHECK(scene.IsPointerOverUI());

	// Taking the input back while the button is held, the cursor still over it (Shift+Esc), cancels the press.
	fixture.Send(MouseButtonPressedEvent(MouseButton::Left), At(point));
	frame();
	CHECK(scene.GetUIButtonState(button) == UIButtonState::Pressed);
	ImGuiLayer::InputContext shift;
	shift.Shift = true;
	CHECK_FALSE(fixture.SendKey(KeyCode::Escape, true, shift));
	frame();
	CHECK_FALSE(fixture.Router.HasInput());
	CHECK(fixture.SendKey(KeyCode::Escape, false, shift));
	fixture.Send(MouseButtonReleasedEvent(MouseButton::Left), At(point));
	frame();
	CHECK(clicks == 1);

	// While the editor has the input, the button under the game's cursor is neither hovered nor clicked, not even by input
	// set directly (as automation does).
	CHECK(scene.GetUIButtonState(button) == UIButtonState::Normal);
	CHECK_FALSE(scene.IsPointerOverUI());
	Input::SetMouseButtonState(MouseButton::Left, true);
	frame();
	CHECK(scene.GetUIButtonState(button) == UIButtonState::Normal);
	Input::SetMouseButtonState(MouseButton::Left, false);
	frame();
	CHECK(clicks == 1);

	// A click on the view gives the input and the button back.
	fixture.Send(MouseButtonPressedEvent(MouseButton::Left), At(point));
	frame();
	fixture.Send(MouseButtonReleasedEvent(MouseButton::Left), At(point));
	frame();
	CHECK(clicks == 2);
}
