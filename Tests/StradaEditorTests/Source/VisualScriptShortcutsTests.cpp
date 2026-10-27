#include "Editor/VisualScripting/VisualScriptEditorTypes.h"
#include "Editor/VisualScripting/VisualScriptShortcuts.h"

#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <algorithm>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace Strada;

namespace
{
	using Action = VisualScriptEditorAction;

	bool IsListed(std::span<ImGuiKeyChord const> chords, ImGuiKeyChord chord)
	{
		return std::find(chords.begin(), chords.end(), chord) != chords.end();
	}

	// The text of a top-level section of a Markdown document, from its "## <number>." heading to the next "## " heading.
	std::string_view FindSection(std::string_view document, std::string_view number)
	{
		std::string const heading = "\n## " + std::string(number) + ". ";
		size_t const start = document.find(heading);
		if (start == std::string_view::npos)
		{
			return {};
		}
		size_t const end = document.find("\n## ", start + heading.size());
		return document.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
	}
}

TEST_CASE("VisualScriptEditor: no key and no mouse gesture of the graph windows is listed twice")
{
	std::set<ImGuiKeyChord> chords;
	for (VisualScriptShortcut const& shortcut : VisualScriptShortcuts)
	{
		CAPTURE(shortcut.Label);
		CHECK(chords.insert(shortcut.Chord).second);
		CHECK_FALSE(shortcut.Label.empty());
	}

	std::set<VisualScriptMouseGestureId> ids;
	std::set<std::tuple<ImGuiKeyChord, bool, VisualScriptMouseButton, VisualScriptMouseMotion, std::string_view>> gestures;
	for (VisualScriptMouseGesture const& gesture : VisualScriptMouseGestures)
	{
		CAPTURE(gesture.Label);
		CHECK(ids.insert(gesture.Id).second);
		CHECK(gestures.insert({gesture.Modifiers, gesture.Space, gesture.Button, gesture.Motion, gesture.Target}).second);
		CHECK_FALSE(gesture.Label.empty());
		CHECK_FALSE(gesture.Target.empty());
		// Gestures name their modifiers only.
		CHECK((gesture.Modifiers & ~ImGuiMod_Mask_) == 0);
	}
	// Every gesture of the enumeration has its row.
	CHECK(ids.size() == static_cast<size_t>(VisualScriptMouseGestureId::Drop) + 1);
}

TEST_CASE("VisualScriptEditor: the graph windows claim only their editing chords from the editor; their canvas keys are none of its chords")
{
	// The chords a focused graph window takes from the editor's global ones (and the editing chords beside them).
	std::vector<ImGuiKeyChord> const focused = {
		ImGuiKey_Delete,
		ImGuiMod_Ctrl | ImGuiKey_Backspace,
		ImGuiMod_Shift | ImGuiKey_Delete,
		ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Backspace,
		ImGuiMod_Ctrl | ImGuiKey_D,
		ImGuiMod_Ctrl | ImGuiKey_A,
		ImGuiMod_Ctrl | ImGuiKey_C,
		ImGuiMod_Ctrl | ImGuiKey_X,
		ImGuiMod_Ctrl | ImGuiKey_V,
		ImGuiMod_Ctrl | ImGuiKey_F,
		ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F,
		ImGuiKey_F2,
	};
	std::vector<ImGuiKeyChord> claimed;
	for (VisualScriptShortcut const& shortcut : VisualScriptShortcuts)
	{
		CAPTURE(shortcut.Label);
		switch (shortcut.Route)
		{
			case VisualScriptKeyRoute::Focused:
				claimed.push_back(shortcut.Chord);
				break;
			case VisualScriptKeyRoute::Canvas:
			case VisualScriptKeyRoute::Owned:
				CHECK_FALSE(IsListed(VisualScriptEditorChords, shortcut.Chord));
				break;
		}
	}
	std::sort(claimed.begin(), claimed.end());
	std::vector<ImGuiKeyChord> expected = focused;
	std::sort(expected.begin(), expected.end());
	CHECK(claimed == expected);
	// Ctrl+S saves the scene as everywhere: no row takes it.
	CHECK(std::none_of(VisualScriptShortcuts.begin(), VisualScriptShortcuts.end(),
	                   [](VisualScriptShortcut const& shortcut)
	                   {
						   return shortcut.Chord == (ImGuiMod_Ctrl | ImGuiKey_S);
					   }));
}

TEST_CASE("VisualScriptEditor: no key of the graph windows is one macOS keeps for itself")
{
	for (VisualScriptShortcut const& shortcut : VisualScriptShortcuts)
	{
		CAPTURE(shortcut.Label);
		CHECK_FALSE(IsListed(VisualScriptMacReservedChords, shortcut.Chord));
	}
	// Bookmarks are stored with Ctrl+Alt+digits, never Ctrl+Shift+digits (Cmd+Shift+3, 4 and 5 take screenshots).
	VisualScriptShortcut const* store = FindVisualScriptShortcut(Action::StoreBookmark3);
	REQUIRE(store != nullptr);
	CHECK(store->Chord == (ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_3));
}

TEST_CASE("VisualScriptEditor: shortcut rows name every key action the canvas handles, and read as the menus show them")
{
	// Each action of the node menu and the Edit menu that has a key is found by its first row.
	for (Action const action : {Action::Delete,
	                            Action::DeleteWithoutRelink,
	                            Action::Cut,
	                            Action::Copy,
	                            Action::Paste,
	                            Action::Duplicate,
	                            Action::SelectAll,
	                            Action::Rename,
	                            Action::ToggleCollapsed,
	                            Action::ToggleDisabled,
	                            Action::AddCommentBox,
	                            Action::Straighten,
	                            Action::AlignLeft,
	                            Action::AlignRight,
	                            Action::AlignTop,
	                            Action::AlignBottom,
	                            Action::DistributeHorizontally,
	                            Action::DistributeVertically,
	                            Action::FrameSelection,
	                            Action::FrameAll,
	                            Action::ZoomIn,
	                            Action::ZoomOut,
	                            Action::ZoomReset,
	                            Action::ToggleMinimap,
	                            Action::Back,
	                            Action::Forward,
	                            Action::Find,
	                            Action::FindInProject,
	                            Action::ShowShortcuts,
	                            Action::OpenPalette,
	                            Action::Cancel})
	{
		CAPTURE(static_cast<int>(action));
		CHECK(FindVisualScriptShortcut(action) != nullptr);
	}
	for (int slot = 0; slot < 9; slot++)
	{
		Action const go = static_cast<Action>(static_cast<int>(Action::GoToBookmark1) + slot);
		Action const store = static_cast<Action>(static_cast<int>(Action::StoreBookmark1) + slot);
		REQUIRE(FindVisualScriptShortcut(go) != nullptr);
		REQUIRE(FindVisualScriptShortcut(store) != nullptr);
		CHECK(FindVisualScriptShortcut(go)->Chord == (ImGuiMod_Ctrl | (ImGuiKey_1 + slot)));
	}
	// Delete's first key is Delete itself; held keys repeat only for nudges and zoom steps.
	CHECK(FindVisualScriptShortcut(Action::Delete)->Chord == ImGuiKey_Delete);
	for (VisualScriptShortcut const& shortcut : VisualScriptShortcuts)
	{
		CAPTURE(shortcut.Label);
		ImGuiKey const key = static_cast<ImGuiKey>(shortcut.Chord & ~ImGuiMod_Mask_);
		bool const arrow = key == ImGuiKey_LeftArrow || key == ImGuiKey_RightArrow || key == ImGuiKey_UpArrow || key == ImGuiKey_DownArrow;
		bool const repeats = arrow || shortcut.Action == Action::ZoomIn || shortcut.Action == Action::ZoomOut;
		CHECK(shortcut.Repeat == repeats);
	}

	CHECK(FormatVisualScriptChord(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, false) == "Ctrl+Shift+F");
	CHECK(FormatVisualScriptChord(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, true) == "Cmd+Shift+F");
	CHECK(FormatVisualScriptChord(ImGuiMod_Alt | ImGuiKey_LeftArrow, false) == "Alt+Left");
	CHECK(FormatVisualScriptChord(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_3, true) == "Cmd+Option+3");
	CHECK(FormatVisualScriptChord(ImGuiMod_Ctrl | ImGuiKey_Equal, false) == "Ctrl+=");
	CHECK(FormatVisualScriptChord(ImGuiMod_Ctrl | ImGuiKey_LeftBracket, false) == "Ctrl+[");
	CHECK(FormatVisualScriptChord(ImGuiMod_Shift | ImGuiKey_Delete, false) == "Shift+Delete");
	CHECK(FormatVisualScriptChord(ImGuiKey_Escape, false) == "Esc");
	CHECK(FormatVisualScriptChord(ImGuiKey_F1, false) == "F1");

	auto const gesture = [](VisualScriptMouseGestureId id, bool macOS)
	{
		VisualScriptMouseGesture const* found = FindVisualScriptMouseGesture(id);
		REQUIRE(found != nullptr);
		return FormatVisualScriptMouseGesture(*found, macOS);
	};
	CHECK(gesture(VisualScriptMouseGestureId::BreakPinLinks, false) == "Alt+Click");
	CHECK(gesture(VisualScriptMouseGestureId::BreakPinLinks, true) == "Option+Click");
	CHECK(gesture(VisualScriptMouseGestureId::InsertReroute, false) == "Double-Click");
	CHECK(gesture(VisualScriptMouseGestureId::CutLinks, false) == "Ctrl+Right-Drag");
	CHECK(gesture(VisualScriptMouseGestureId::PanSpace, false) == "Space+Drag");
	CHECK(gesture(VisualScriptMouseGestureId::PanMiddle, false) == "Middle-Drag");
	CHECK(gesture(VisualScriptMouseGestureId::CtrlWheel, true) == "Cmd+Wheel");
	CHECK(gesture(VisualScriptMouseGestureId::Back, false) == "Mouse Back");
	CHECK(gesture(VisualScriptMouseGestureId::ChoosePin, false) == "Press");
}

TEST_CASE("VisualScriptEditor: Docs/Architecture.md lists every key and mouse gesture of the graph windows in section 11")
{
	Result<std::string> const document = FileSystem::ReadTextFile(STRADA_ARCHITECTURE_DOC_PATH);
	REQUIRE(document.IsOk());
	std::string_view const section = FindSection(document.GetValue(), "11");
	REQUIRE_FALSE(section.empty());

	// Each row of the tables is a row of the document's tables, written as the shortcut sheet shows it (Windows and Linux names).
	for (VisualScriptShortcut const& shortcut : VisualScriptShortcuts)
	{
		std::string const row = "| `" + FormatVisualScriptChord(shortcut.Chord, false) + "` | " + std::string(shortcut.Label) + " |";
		CHECK_MESSAGE(section.find(row) != std::string_view::npos, "missing key row: " << row);
	}
	for (VisualScriptMouseGesture const& gesture : VisualScriptMouseGestures)
	{
		std::string const row = "| `" + FormatVisualScriptMouseGesture(gesture, false) + "` | " + std::string(gesture.Target) + " | " +
		                        std::string(gesture.Label) + " |";
		CHECK_MESSAGE(section.find(row) != std::string_view::npos, "missing gesture row: " << row);
	}
}
