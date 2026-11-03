#pragma once

#include "Editor/VisualScripting/VisualScriptEditorTypes.h"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// The keys and mouse gestures of the graph windows as data: the key handler dispatches from these tables, the menus take
// their shortcut texts from them, the shortcut sheet (F1) lists them, and Docs/Architecture.md lists every row. "Ctrl" is
// Cmd on macOS (ImGui swaps them with io.ConfigMacOSXBehaviors); FormatVisualScriptChord names the keys a Mac shows.
namespace Strada
{
	// The shortcut sheet's groups, in its order.
	enum class VisualScriptShortcutGroup : uint8_t
	{
		Create = 0,
		Edit,
		Node,
		Select,
		Arrange,
		View,
		Navigate,
		Find,
		// The drawer's Timeline mode (spec-slice3 §9.5).
		Timeline,
		Help
	};

	[[nodiscard]] constexpr std::string_view GetVisualScriptShortcutGroupName(VisualScriptShortcutGroup group)
	{
		switch (group)
		{
			case VisualScriptShortcutGroup::Create:
				return "Create";
			case VisualScriptShortcutGroup::Edit:
				return "Edit";
			case VisualScriptShortcutGroup::Node:
				return "Node";
			case VisualScriptShortcutGroup::Select:
				return "Select";
			case VisualScriptShortcutGroup::Arrange:
				return "Arrange";
			case VisualScriptShortcutGroup::View:
				return "View";
			case VisualScriptShortcutGroup::Navigate:
				return "Navigate";
			case VisualScriptShortcutGroup::Find:
				return "Find";
			case VisualScriptShortcutGroup::Timeline:
				return "Timeline";
			case VisualScriptShortcutGroup::Help:
				return "Help";
		}
		return "Other";
	}

	// How the graph window reads a key.
	enum class VisualScriptKeyRoute : uint8_t
	{
		// Claimed with ImGui::Shortcut(chord, ImGuiInputFlags_RouteFocused) on the graph window every frame it is drawn, so it
		// wins over the editor's global editing keys of the same chords (Ctrl+X, Ctrl+C, Ctrl+V, Ctrl+D, Delete, Ctrl+A:
		// EditorEditingKeys) while the window is focused; the panel gives it to the focused part (the canvas, My Script or
		// Details).
		Focused = 0,
		// Matched exactly, modifiers included, with ImGui::IsKeyChordPressed and the canvas's key owner while the canvas has
		// the keyboard and no text field has; never one of the editor's global chords (VisualScriptEditorChords).
		Canvas,
		// Never dispatched as a chord: Escape is read with the canvas's key ownership, and a Space tap by the canvas model from
		// its input.
		Owned,
		// The Timeline drawer's keys, while it has the keyboard and no text field has (spec-slice3 §9.5): matched exactly under
		// the drawer's key owner, as the canvas's are, except the chords a Focused row claims (Delete, Ctrl+A), which reach the
		// drawer through the window's claim. Escape ends the drawer's gesture, else gives the keyboard back to the canvas.
		Timeline
	};

	struct VisualScriptShortcut
	{
		ImGuiKeyChord Chord = ImGuiKey_None;
		VisualScriptEditorAction Action = VisualScriptEditorAction::Delete;
		VisualScriptShortcutGroup Group = VisualScriptShortcutGroup::Edit;
		VisualScriptKeyRoute Route = VisualScriptKeyRoute::Canvas;
		// Held down, the key repeats the action (ImGuiInputFlags_Repeat).
		bool Repeat = false;
		// What the shortcut sheet says it does.
		std::string_view Label;
	};

	namespace VisualScriptShortcutsDetail
	{
		using Action = VisualScriptEditorAction;
		using Group = VisualScriptShortcutGroup;
		using Route = VisualScriptKeyRoute;

		inline constexpr ImGuiKeyChord Ctrl = ImGuiMod_Ctrl;
		inline constexpr ImGuiKeyChord Shift = ImGuiMod_Shift;
		inline constexpr ImGuiKeyChord Alt = ImGuiMod_Alt;

		inline constexpr std::array<VisualScriptShortcut, 68> Shortcuts = {{
			{ImGuiKey_Tab, Action::OpenPalette, Group::Create, Route::Canvas, false, "Add a node (the palette)"},
			{ImGuiKey_Space, Action::OpenPalette, Group::Create, Route::Owned, false,
		     "Add a node: tap Space, or hold it and type the node's name"},
			{ImGuiKey_C, Action::AddCommentBox, Group::Create, Route::Canvas, false, "Add a comment box (around the selection)"},

			{ImGuiKey_Delete, Action::Delete, Group::Edit, Route::Focused, false, "Delete, keeping the flow through what goes"},
			{Ctrl | ImGuiKey_Backspace, Action::Delete, Group::Edit, Route::Focused, false, "Delete, keeping the flow through what goes"},
			{Shift | ImGuiKey_Delete, Action::DeleteWithoutRelink, Group::Edit, Route::Focused, false, "Delete without relinking"},
			{Ctrl | Shift | ImGuiKey_Backspace, Action::DeleteWithoutRelink, Group::Edit, Route::Focused, false,
		     "Delete without relinking"},
			{Ctrl | ImGuiKey_X, Action::Cut, Group::Edit, Route::Focused, false, "Cut"},
			{Ctrl | ImGuiKey_C, Action::Copy, Group::Edit, Route::Focused, false, "Copy"},
			{Ctrl | ImGuiKey_V, Action::Paste, Group::Edit, Route::Focused, false, "Paste at the cursor"},
			{Ctrl | ImGuiKey_D, Action::Duplicate, Group::Edit, Route::Focused, false, "Duplicate"},
			{ImGuiKey_F2, Action::Rename, Group::Edit, Route::Focused, false, "Rename the selected comment, variable, function or event"},

			{ImGuiKey_H, Action::ToggleCollapsed, Group::Node, Route::Canvas, false, "Collapse or expand the selected nodes"},
			{ImGuiKey_M, Action::ToggleDisabled, Group::Node, Route::Canvas, false, "Disable or enable the selected nodes"},

			{Ctrl | ImGuiKey_A, Action::SelectAll, Group::Select, Route::Focused, false, "Select every node and comment"},
			{ImGuiKey_Escape, Action::Cancel, Group::Select, Route::Owned, false,
		     "Cancel the gesture; else close a pane or the find bar; else clear the selection"},

			{ImGuiKey_LeftArrow, Action::NudgeLeft, Group::Arrange, Route::Canvas, true, "Nudge left one grid step"},
			{ImGuiKey_RightArrow, Action::NudgeRight, Group::Arrange, Route::Canvas, true, "Nudge right one grid step"},
			{ImGuiKey_UpArrow, Action::NudgeUp, Group::Arrange, Route::Canvas, true, "Nudge up one grid step"},
			{ImGuiKey_DownArrow, Action::NudgeDown, Group::Arrange, Route::Canvas, true, "Nudge down one grid step"},
			{Alt | ImGuiKey_LeftArrow, Action::NudgeLeftFine, Group::Arrange, Route::Canvas, true, "Nudge left one unit"},
			{Alt | ImGuiKey_RightArrow, Action::NudgeRightFine, Group::Arrange, Route::Canvas, true, "Nudge right one unit"},
			{Alt | ImGuiKey_UpArrow, Action::NudgeUpFine, Group::Arrange, Route::Canvas, true, "Nudge up one unit"},
			{Alt | ImGuiKey_DownArrow, Action::NudgeDownFine, Group::Arrange, Route::Canvas, true, "Nudge down one unit"},
			{Shift | ImGuiKey_W, Action::AlignTop, Group::Arrange, Route::Canvas, false, "Align tops"},
			{Shift | ImGuiKey_S, Action::AlignBottom, Group::Arrange, Route::Canvas, false, "Align bottoms"},
			{Shift | ImGuiKey_A, Action::AlignLeft, Group::Arrange, Route::Canvas, false, "Align left edges"},
			{Shift | ImGuiKey_D, Action::AlignRight, Group::Arrange, Route::Canvas, false, "Align right edges"},
			{Shift | ImGuiKey_H, Action::DistributeHorizontally, Group::Arrange, Route::Canvas, false, "Distribute horizontally"},
			{Shift | ImGuiKey_V, Action::DistributeVertically, Group::Arrange, Route::Canvas, false, "Distribute vertically"},
			{ImGuiKey_Q, Action::Straighten, Group::Arrange, Route::Canvas, false, "Straighten the selection's flow"},

			{ImGuiKey_F, Action::FrameSelection, Group::View, Route::Canvas, false, "Frame the selection (everything without one)"},
			{ImGuiKey_Home, Action::FrameAll, Group::View, Route::Canvas, false, "Frame everything"},
			{Ctrl | ImGuiKey_Equal, Action::ZoomIn, Group::View, Route::Canvas, true, "Zoom in"},
			{Ctrl | ImGuiKey_Minus, Action::ZoomOut, Group::View, Route::Canvas, true, "Zoom out"},
			{Ctrl | ImGuiKey_0, Action::ZoomReset, Group::View, Route::Canvas, false, "Zoom to 100 %"},
			{Ctrl | ImGuiKey_1, Action::GoToBookmark1, Group::View, Route::Canvas, false, "Go to bookmark 1"},
			{Ctrl | ImGuiKey_2, Action::GoToBookmark2, Group::View, Route::Canvas, false, "Go to bookmark 2"},
			{Ctrl | ImGuiKey_3, Action::GoToBookmark3, Group::View, Route::Canvas, false, "Go to bookmark 3"},
			{Ctrl | ImGuiKey_4, Action::GoToBookmark4, Group::View, Route::Canvas, false, "Go to bookmark 4"},
			{Ctrl | ImGuiKey_5, Action::GoToBookmark5, Group::View, Route::Canvas, false, "Go to bookmark 5"},
			{Ctrl | ImGuiKey_6, Action::GoToBookmark6, Group::View, Route::Canvas, false, "Go to bookmark 6"},
			{Ctrl | ImGuiKey_7, Action::GoToBookmark7, Group::View, Route::Canvas, false, "Go to bookmark 7"},
			{Ctrl | ImGuiKey_8, Action::GoToBookmark8, Group::View, Route::Canvas, false, "Go to bookmark 8"},
			{Ctrl | ImGuiKey_9, Action::GoToBookmark9, Group::View, Route::Canvas, false, "Go to bookmark 9"},
			{Ctrl | Alt | ImGuiKey_1, Action::StoreBookmark1, Group::View, Route::Canvas, false, "Store bookmark 1"},
			{Ctrl | Alt | ImGuiKey_2, Action::StoreBookmark2, Group::View, Route::Canvas, false, "Store bookmark 2"},
			{Ctrl | Alt | ImGuiKey_3, Action::StoreBookmark3, Group::View, Route::Canvas, false, "Store bookmark 3"},
			{Ctrl | Alt | ImGuiKey_4, Action::StoreBookmark4, Group::View, Route::Canvas, false, "Store bookmark 4"},
			{Ctrl | Alt | ImGuiKey_5, Action::StoreBookmark5, Group::View, Route::Canvas, false, "Store bookmark 5"},
			{Ctrl | Alt | ImGuiKey_6, Action::StoreBookmark6, Group::View, Route::Canvas, false, "Store bookmark 6"},
			{Ctrl | Alt | ImGuiKey_7, Action::StoreBookmark7, Group::View, Route::Canvas, false, "Store bookmark 7"},
			{Ctrl | Alt | ImGuiKey_8, Action::StoreBookmark8, Group::View, Route::Canvas, false, "Store bookmark 8"},
			{Ctrl | Alt | ImGuiKey_9, Action::StoreBookmark9, Group::View, Route::Canvas, false, "Store bookmark 9"},
			{Shift | ImGuiKey_M, Action::ToggleMinimap, Group::View, Route::Canvas, false, "Show or hide the minimap"},

			{Ctrl | ImGuiKey_LeftBracket, Action::Back, Group::Navigate, Route::Canvas, false, "Back"},
			{Ctrl | ImGuiKey_RightBracket, Action::Forward, Group::Navigate, Route::Canvas, false, "Forward"},

			{Ctrl | ImGuiKey_F, Action::Find, Group::Find, Route::Focused, false, "Find in this script"},
			{Ctrl | Shift | ImGuiKey_F, Action::FindInProject, Group::Find, Route::Focused, false, "Find in the project"},

			{ImGuiKey_Delete, Action::Delete, Group::Timeline, Route::Timeline, false, "Remove the selected keys"},
			{Ctrl | ImGuiKey_A, Action::SelectAll, Group::Timeline, Route::Timeline, false, "Select every key of the visible tracks"},
			{ImGuiKey_LeftArrow, Action::NudgeLeft, Group::Timeline, Route::Timeline, true, "Move the selected keys 0.01 s earlier"},
			{ImGuiKey_RightArrow, Action::NudgeRight, Group::Timeline, Route::Timeline, true, "Move the selected keys 0.01 s later"},
			{Shift | ImGuiKey_LeftArrow, Action::NudgeLeftCoarse, Group::Timeline, Route::Timeline, true,
		     "Move the selected keys 0.1 s earlier"},
			{Shift | ImGuiKey_RightArrow, Action::NudgeRightCoarse, Group::Timeline, Route::Timeline, true,
		     "Move the selected keys 0.1 s later"},
			{ImGuiKey_F, Action::FrameSelection, Group::Timeline, Route::Timeline, false,
		     "Frame the selected keys (everything without any)"},
			{ImGuiKey_Escape, Action::Cancel, Group::Timeline, Route::Timeline, false,
		     "End the drag; else give the keyboard back to the canvas"},

			{ImGuiKey_F1, Action::ShowShortcuts, Group::Help, Route::Canvas, false, "Show these shortcuts"},
		}};
	}

	// Every key of the graph windows, grouped as the shortcut sheet lists them. An action may have several keys (Delete and
	// Ctrl+Backspace); its first row names it in menus.
	inline constexpr std::array<VisualScriptShortcut, 68> const& VisualScriptShortcuts = VisualScriptShortcutsDetail::Shortcuts;

	// The editor's global chords (EditorLayer's and its editing keys', EditorEditingKeys), which no canvas key equals: graph
	// windows shadow only the editing chords they claim while focused (Ctrl+X, Ctrl+C, Ctrl+V, Ctrl+D, Delete,
	// Ctrl+Backspace, Ctrl+A), and Ctrl+S saves the scene everywhere.
	inline constexpr std::array<ImGuiKeyChord, 18> VisualScriptEditorChords = {
		ImGuiMod_Ctrl | ImGuiKey_N,
		ImGuiMod_Ctrl | ImGuiKey_O,
		ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S,
		ImGuiMod_Ctrl | ImGuiKey_S,
		ImGuiMod_Ctrl | ImGuiKey_B,
		ImGuiMod_Ctrl | ImGuiKey_P,
		ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P,
		ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_P,
		ImGuiMod_Ctrl | ImGuiKey_Z,
		ImGuiMod_Ctrl | ImGuiKey_Y,
		ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z,
		ImGuiMod_Ctrl | ImGuiKey_X,
		ImGuiMod_Ctrl | ImGuiKey_C,
		ImGuiMod_Ctrl | ImGuiKey_V,
		ImGuiMod_Ctrl | ImGuiKey_D,
		ImGuiKey_Delete,
		ImGuiMod_Ctrl | ImGuiKey_Backspace,
		ImGuiMod_Ctrl | ImGuiKey_A,
	};

	// Chords macOS keeps for itself (screenshots, Spotlight, the app switcher, hide, minimize, quit), with Ctrl read as Cmd:
	// they never reach the editor there, so no key of the graph windows is one of them.
	inline constexpr std::array<ImGuiKeyChord, 9> VisualScriptMacReservedChords = {
		ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_3,
		ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_4,
		ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_5,
		ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_6,
		ImGuiMod_Ctrl | ImGuiKey_Space,
		ImGuiMod_Ctrl | ImGuiKey_Tab,
		ImGuiMod_Ctrl | ImGuiKey_H,
		ImGuiMod_Ctrl | ImGuiKey_M,
		ImGuiMod_Ctrl | ImGuiKey_Q,
	};

	// The mouse gestures of the canvas, one per row of the shortcut sheet's "Mouse" group.
	enum class VisualScriptMouseGestureId : uint8_t
	{
		Select = 0,
		ClearSelection,
		MoveNodes,
		MoveComment,
		ResizeComment,
		ChoosePin,
		Marquee,
		Wire,
		Relink,
		BreakPinLinks,
		BreakLink,
		InsertReroute,
		OpenNode,
		EditText,
		Palette,
		ContextMenu,
		PanRight,
		PanMiddle,
		PanSpace,
		CutLinks,
		Wheel,
		CtrlWheel,
		ShiftWheel,
		HorizontalWheel,
		Back,
		Forward,
		Drop,
		// The Timeline drawer's curve view (spec-slice3 §9.5).
		TimelineSelectKey,
		TimelineMarquee,
		TimelineMoveKeys,
		TimelineTangent,
		TimelineAddKey,
		TimelineKeyMenu,
		TimelineZoomTime,
		TimelineZoomValues,
		TimelinePanRight,
		TimelinePanMiddle
	};

	enum class VisualScriptMouseButton : uint8_t
	{
		Left = 0,
		Right,
		Middle,
		// The vertical and the horizontal wheel (or a trackpad's swipe).
		Wheel,
		HorizontalWheel,
		// The mouse's X1 and X2 buttons.
		Back,
		Forward
	};

	enum class VisualScriptMouseMotion : uint8_t
	{
		Click = 0,
		DoubleClick,
		// Acts at the press, before the release.
		Press,
		Drag,
		Scroll,
		// A drag-and-drop payload released over the canvas.
		Drop
	};

	struct VisualScriptMouseGesture
	{
		VisualScriptMouseGestureId Id = VisualScriptMouseGestureId::Select;
		// ImGuiMod_ flags held.
		ImGuiKeyChord Modifiers = ImGuiKey_None;
		// Space is held.
		bool Space = false;
		VisualScriptMouseButton Button = VisualScriptMouseButton::Left;
		VisualScriptMouseMotion Motion = VisualScriptMouseMotion::Click;
		// What it is done on.
		std::string_view Target;
		// What it does.
		std::string_view Label;
	};

	namespace VisualScriptShortcutsDetail
	{
		using Id = VisualScriptMouseGestureId;
		using Button = VisualScriptMouseButton;
		using Motion = VisualScriptMouseMotion;

		inline constexpr std::array<VisualScriptMouseGesture, 37> MouseGestures = {{
			{Id::Select, ImGuiKey_None, false, Button::Left, Motion::Click, "Node", "Select it (Shift adds, Ctrl toggles)"},
			{Id::ClearSelection, ImGuiKey_None, false, Button::Left, Motion::Click, "Background", "Clear the selection"},
			{Id::MoveNodes, ImGuiKey_None, false, Button::Left, Motion::Drag, "Node", "Move the selection"},
			{Id::MoveComment, ImGuiKey_None, false, Button::Left, Motion::Drag, "Comment title bar",
		     "Move the comment, with its contents while Move Comment Contents is on (Alt: the other way)"},
			{Id::ResizeComment, ImGuiKey_None, false, Button::Left, Motion::Drag, "Comment edge or corner", "Resize the comment"},
			{Id::ChoosePin, ImGuiKey_None, false, Button::Left, Motion::Press, "Collapsed node's pin dot",
		     "Choose one of its pins to wire from"},
			{Id::Marquee, ImGuiKey_None, false, Button::Left, Motion::Drag, "Background",
		     "Select within a rectangle (Shift adds, Ctrl removes)"},
			{Id::Wire, ImGuiKey_None, false, Button::Left, Motion::Drag, "Pin", "Draw a wire from it"},
			{Id::Relink, ImGuiMod_Ctrl, false, Button::Left, Motion::Drag, "Linked pin", "Move its links' near ends"},
			{Id::BreakPinLinks, ImGuiMod_Alt, false, Button::Left, Motion::Click, "Pin", "Break all its links"},
			{Id::BreakLink, ImGuiMod_Alt, false, Button::Left, Motion::Click, "Wire", "Break the link"},
			{Id::InsertReroute, ImGuiKey_None, false, Button::Left, Motion::DoubleClick, "Wire", "Add a reroute"},
			{Id::OpenNode, ImGuiKey_None, false, Button::Left, Motion::DoubleClick, "Node header", "Open the node"},
			{Id::EditText, ImGuiKey_None, false, Button::Left, Motion::DoubleClick, "Comment title or node comment",
		     "Edit the text in place"},
			{Id::Palette, ImGuiKey_None, false, Button::Right, Motion::Click, "Background", "Add a node (the palette)"},
			{Id::ContextMenu, ImGuiKey_None, false, Button::Right, Motion::Click, "Node, pin, wire or comment", "Its menu"},
			{Id::PanRight, ImGuiKey_None, false, Button::Right, Motion::Drag, "Anywhere", "Pan"},
			{Id::PanMiddle, ImGuiKey_None, false, Button::Middle, Motion::Drag, "Anywhere", "Pan"},
			{Id::PanSpace, ImGuiKey_None, true, Button::Left, Motion::Drag, "Anywhere", "Pan"},
			{Id::CutLinks, ImGuiMod_Ctrl, false, Button::Right, Motion::Drag, "Anywhere", "Cut the links the line crosses"},
			{Id::Wheel, ImGuiKey_None, false, Button::Wheel, Motion::Scroll, "Canvas",
		     "Zoom to the cursor (Scroll: Zoom), or pan (Scroll: Pan)"},
			{Id::CtrlWheel, ImGuiMod_Ctrl, false, Button::Wheel, Motion::Scroll, "Canvas",
		     "Pan up and down (Scroll: Zoom), or zoom to the cursor (Scroll: Pan)"},
			{Id::ShiftWheel, ImGuiMod_Shift, false, Button::Wheel, Motion::Scroll, "Canvas", "Pan sideways"},
			{Id::HorizontalWheel, ImGuiKey_None, false, Button::HorizontalWheel, Motion::Scroll, "Canvas", "Pan sideways"},
			{Id::Back, ImGuiKey_None, false, Button::Back, Motion::Click, "Window", "Back"},
			{Id::Forward, ImGuiKey_None, false, Button::Forward, Motion::Click, "Window", "Forward"},
			{Id::Drop, ImGuiKey_None, false, Button::Left, Motion::Drop, "Pin or canvas",
		     "Set a value or add a node from a dropped asset, entity or My Script row"},
			{Id::TimelineSelectKey, ImGuiKey_None, false, Button::Left, Motion::Click, "Timeline key",
		     "Select it (Shift adds, Ctrl toggles)"},
			{Id::TimelineMarquee, ImGuiKey_None, false, Button::Left, Motion::Drag, "Timeline curves",
		     "Select the keys within a rectangle (Shift adds, Ctrl removes)"},
			{Id::TimelineMoveKeys, ImGuiKey_None, false, Button::Left, Motion::Drag, "Timeline key",
		     "Move the selected keys as a block: times snap to 0.01 s (Alt: free), Shift keeps one axis"},
			{Id::TimelineTangent, ImGuiKey_None, false, Button::Left, Motion::Drag, "Timeline tangent handle",
		     "Set the key's two tangents (Alt: this one alone)"},
			{Id::TimelineAddKey, ImGuiKey_None, false, Button::Left, Motion::DoubleClick, "Timeline curve or event lane",
		     "Add a key there with the curve's value, or an event"},
			{Id::TimelineKeyMenu, ImGuiKey_None, false, Button::Right, Motion::Click, "Timeline key", "Interpolation, Tangents and Delete"},
			{Id::TimelineZoomTime, ImGuiKey_None, false, Button::Wheel, Motion::Scroll, "Timeline curves", "Zoom time around the cursor"},
			{Id::TimelineZoomValues, ImGuiMod_Ctrl, false, Button::Wheel, Motion::Scroll, "Timeline curves",
		     "Zoom values around the cursor"},
			{Id::TimelinePanRight, ImGuiKey_None, false, Button::Right, Motion::Drag, "Timeline curves", "Pan"},
			{Id::TimelinePanMiddle, ImGuiKey_None, false, Button::Middle, Motion::Drag, "Timeline curves", "Pan"},
		}};
	}

	inline constexpr std::array<VisualScriptMouseGesture, 37> const& VisualScriptMouseGestures = VisualScriptShortcutsDetail::MouseGestures;

	// The first key of an action (the one menus show); null when it has none.
	[[nodiscard]] constexpr VisualScriptShortcut const* FindVisualScriptShortcut(VisualScriptEditorAction action)
	{
		for (VisualScriptShortcut const& shortcut : VisualScriptShortcuts)
		{
			if (shortcut.Action == action)
			{
				return &shortcut;
			}
		}
		return nullptr;
	}

	[[nodiscard]] constexpr VisualScriptMouseGesture const* FindVisualScriptMouseGesture(VisualScriptMouseGestureId id)
	{
		for (VisualScriptMouseGesture const& gesture : VisualScriptMouseGestures)
		{
			if (gesture.Id == id)
			{
				return &gesture;
			}
		}
		return nullptr;
	}

	// The modifiers of a chord as a key's text starts: "Ctrl+Shift+", or on macOS "Cmd+Shift+" (ImGui's Ctrl is Cmd there,
	// its Super the Control key, and Alt is Option).
	[[nodiscard]] inline std::string FormatVisualScriptModifiers(ImGuiKeyChord chord, bool macOS)
	{
		std::string text;
		if ((chord & ImGuiMod_Ctrl) != 0)
		{
			text += macOS ? "Cmd+" : "Ctrl+";
		}
		if ((chord & ImGuiMod_Super) != 0)
		{
			text += macOS ? "Ctrl+" : "Super+";
		}
		if ((chord & ImGuiMod_Shift) != 0)
		{
			text += "Shift+";
		}
		if ((chord & ImGuiMod_Alt) != 0)
		{
			text += macOS ? "Option+" : "Alt+";
		}
		return text;
	}

	// A chord as the shortcut sheet and the menus show it: "Ctrl+Shift+F", "Shift+Delete", "Ctrl+=", "Left".
	[[nodiscard]] inline std::string FormatVisualScriptChord(ImGuiKeyChord chord, bool macOS)
	{
		ImGuiKey const key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
		std::string_view name;
		switch (key)
		{
			case ImGuiKey_LeftArrow:
				name = "Left";
				break;
			case ImGuiKey_RightArrow:
				name = "Right";
				break;
			case ImGuiKey_UpArrow:
				name = "Up";
				break;
			case ImGuiKey_DownArrow:
				name = "Down";
				break;
			case ImGuiKey_Escape:
				name = "Esc";
				break;
			case ImGuiKey_Equal:
				name = "=";
				break;
			case ImGuiKey_Minus:
				name = "-";
				break;
			case ImGuiKey_LeftBracket:
				name = "[";
				break;
			case ImGuiKey_RightBracket:
				name = "]";
				break;
			default:
				name = ImGui::GetKeyName(key);
				break;
		}
		return FormatVisualScriptModifiers(chord, macOS) + std::string(name);
	}

	// A gesture as the shortcut sheet and the menus show it: "Alt+Click", "Double-Click", "Ctrl+Right-Drag", "Space+Drag".
	[[nodiscard]] inline std::string FormatVisualScriptMouseGesture(VisualScriptMouseGesture const& gesture, bool macOS)
	{
		std::string text = FormatVisualScriptModifiers(gesture.Modifiers, macOS);
		if (gesture.Space)
		{
			text += "Space+";
		}
		switch (gesture.Button)
		{
			case VisualScriptMouseButton::Wheel:
				return text + "Wheel";
			case VisualScriptMouseButton::HorizontalWheel:
				return text + "Horizontal Wheel";
			case VisualScriptMouseButton::Back:
				return text + "Mouse Back";
			case VisualScriptMouseButton::Forward:
				return text + "Mouse Forward";
			case VisualScriptMouseButton::Right:
				text += "Right-";
				break;
			case VisualScriptMouseButton::Middle:
				text += "Middle-";
				break;
			case VisualScriptMouseButton::Left:
				break;
		}
		switch (gesture.Motion)
		{
			case VisualScriptMouseMotion::Click:
				return text + "Click";
			case VisualScriptMouseMotion::DoubleClick:
				return text + "Double-Click";
			case VisualScriptMouseMotion::Press:
				return text + "Press";
			case VisualScriptMouseMotion::Drag:
				return text + "Drag";
			case VisualScriptMouseMotion::Scroll:
				return text + "Scroll";
			case VisualScriptMouseMotion::Drop:
				return text + "Drop";
		}
		return text;
	}
}
