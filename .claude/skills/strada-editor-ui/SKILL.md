---
name: strada-editor-ui
description: How to build or change Strada editor panels and widgets (ImGui panels, the generated inspector/FieldEditor, undo merge keys with EditSession, deferring scene changes while drawing, drag-and-drop payloads, menus and shortcuts, native file dialogs, headless ImGui tests). Use for any change under StradaEditor/Source/Editor/Panels or Editor/UI.
---

# Editor panels and widgets

The editor UI is Dear ImGui (docking branch) drawn from `EditorLayer::OnImGuiRender`. Panels never change the scene
directly: every modification goes through `EditorOperations` (undoable commands, the same ones automation uses).

## Panels

- One class per panel in `StradaEditor/Source/Editor/Panels/`, drawn with
  `OnImGuiRender(EditorOperations& operations, bool& open)` between `ImGui::Begin(<title>, &open)` and `End()`.
- Register it in `EditorLayer` (member, `m_Show<Panel>` flag, View menu entry, `BuildDefaultLayout`). Changing the panel
  set changes the default layout: bump `DockspaceName` in `EditorLayer.cpp` so existing users get the new layout once.
- Keep logic that does not need ImGui in plain functions (`HierarchyEditing`, `ComponentInspection`, `EntityPresets`)
  so it is unit tested without a UI.

## Editing values

- Reflected data (components, settings, materials) is edited with `FieldEditor::Draw(descriptors, valuesJson,
  mixedFields)`, which returns a `FieldChange`; turn it into a patch per object with `FieldChange::MakePatch(values)`.
  Never write per-component inspector code: improve the field hints (`strada-add-component`) or the FieldEditor.
- Arrays in field tables report index edits, never the whole array: `FieldChange::Path` is a list of
  `FieldPathStep { Field, Element }` and `FieldChange::ArrayEdit` an insert, remove, move or duplicate by index, which
  `MakePatch(values)` applies to each object's own array (an index it lacks leaves it as it is). Arrays of structs fold
  into a row per element with a menu (Move Up, Move Down, Duplicate, Insert Above, Remove) over the element's own rows,
  and buttons that add stop at the field's `MaxCount`. A panel that fixes up other data after a list edit never
  renumbers what other files also hold by index: the Project Settings layers are indices that rigid bodies in every
  scene keep, so removing one only drops the ignored pairs naming an index the shorter list lacks. Values drawn without
  a row (`DrawInlineValue`) still report whole arrays.
- A struct that needs a widget of its own gets a struct drawer (`GetFieldStructDrawers` in
  `Editor/UI/FieldStructDrawers.cpp`, or `FieldEditor::SetStructDrawer` for one editor), drawn wherever the struct
  appears; its `FieldStructEdit` brings a description and merge key, which panels that make undo steps of field changes
  use when they are set (`change->MergeKey != 0 ? change->MergeKey : m_EditSession.GetMergeKey()`).
- Field tables size their name column for their names (`FieldEditor::GetNameColumnShare`, 30% to 60%, at most 50%
  beside asset fields). A table of
  your own rows that lines up with a field table (the script class picker above the script's fields) sets up its two
  stretched columns with the same share.
- Asset references are FieldEditor asset fields (path box, `...` picker, `x`; give the editor the editor's shared
  `AssetThumbnails` with `SetAssetPreviews` for images of the assets: `EditorLayer` owns the one set, hands it to panels
  through `SetThumbnails` and updates it before the panels draw, so never give a panel thumbnails of its own); an asset
  reference outside a field table uses `FieldEditor::DrawAssetReference` (options: type, None in the picker, what `x`
  does). Text that names an asset elsewhere uses `UI::DescribeAssetReference` and `UI::FitPath`. A field that UI of its
  own edits gets the `HiddenInEditors` hint so the generic table skips it (Mesh.Materials: the material sections of Mesh
  components, which go through `MaterialSlots`: the material of each slot, assigning and clearing one, editable copies;
  tested without a UI).
- One widget interaction must be one undo step: keep an `EditSession` per panel (unique key prefix), call
  `Update(ImGui::IsAnyItemActive(), history)` at the start of the panel, and pass `GetMergeKey()` to operations.
- Report failed operations with `UI::ReportFailure(result, "Doing X")` (logged, shown in the console).

## Changing the scene while drawing

- Deleting, creating or moving entities while iterating the hierarchy invalidates the iteration: queue the action
  (`Defer` in `SceneHierarchyPanel`, `m_Deferred` in `InspectorPanel`) and run it after drawing.
- Selection changes from a panel set a "from panel" flag so other code can tell them from external selections.

## Drag and drop, menus, shortcuts, dialogs

- Payload types live in `DragDropPayload` (`Editor/UI/EditorUI.h`): `Entities` carries `uint64_t` UUIDs, `Asset` one
  `uint64_t` handle, `AssetFolder` and `ScriptFolder` a folder path relative to their root, `AssetFile` the path of a
  file of the asset directory that is not an asset (relative to it), `ScriptFile` the absolute path of a C# source (all
  paths UTF-8, no terminator), `VisualScriptElement` a `VisualScriptElementPayload` (a My Script row: the graph, the
  element's kind and ID; a graph window accepts it only for its own graph, and My Script reorders variables with it).
  Validate dropped data (entities may not exist, assets may have the wrong type, files may be gone).
- Assets: make an item a drag source with `UI::AssetDragSource(handle, label)`; inside a drop target accept with
  `UI::AcceptAssetDrop(types)`, which highlights only for registered assets of those types. What dropping an asset onto
  the scene does lives in `AssetDrops` and `EntityPresets::CreateFromMesh` (unit tested without UI). Scripts use
  `UI::ScriptDragSource` and `UI::AcceptScriptDrop`; `AssetDrops::AddScript` maps the file to its loaded class and sets
  it as one undo step (`EditorOperations::SetScriptClass`). Visual scripts are assets: `AssetDrops::AddVisualScript`
  gives the entities a VisualScript component running the graph, or changes the graph of the one they have, as one undo
  step (`EditorOperations::SetVisualScriptGraph`), without the scripting runtime.
- Files dropped onto the editor from the OS arrive as `WindowFileDropEvent`s (from every window, floating panels
  included) and go to `ContentBrowserPanel::DropFiles`; what an import does lives in `ProjectImport`, and the browser's
  `ImportDropped` (which `asset.drop` calls for agents) selects what came in and shows a notice of what did not. While
  the start screen shows, a dropped `.sproj` opens instead (`FindDroppedProject`) and nothing is imported.
- Global shortcuts use `ImGui::Shortcut(chord, ImGuiInputFlags_RouteGlobal)` in `EditorShortcuts`
  (`HandleEditorShortcuts`, with an `EditorShortcutActions` the layer fills and the tests record) and skip editing keys
  while `io.WantTextInput`; show the chord in the matching menu item. ImGui's global routes ignore modals, so
  `HandleEditorShortcuts` holds every global shortcut back while a modal popup is open or the start screen shows
  (`AreEditorShortcutsHeldBack`); a new global shortcut goes into its table. The editing keys and the Edit menu's
  editing items live in `EditorEditingKeys` (`HandleEditorEditingKeys(operations, visualScripts,
  FindClaimedEditingChords(keyboardWindow))`, `DrawEditorEditingMenuItems`, and Undo and Redo in
  `HandleEditorHistoryKeys`, `UndoEditorStep` and `RedoEditorStep`), which the editor and the tests both run: they act on
  the Edit menu's target (a graph window through `VisualScriptEditors::HandleGlobalEditingKey`, else the entities). A
  panel with its own meaning for a key registers `ImGuiInputFlags_RouteFocused` every frame while it is drawn, even with
  nothing to act on, from a `static constexpr` chord list (`ContentBrowserPanel::FocusedChords`) that
  `FindClaimedEditingChords` also returns: the editing keys never run a claimed chord, and the Edit menu drops that
  item's shortcut text (`GetEditShortcutText`) while the panel has the keyboard (`FindKeyboardWindow`). A widget in a
  child window of a panel (the `.scurve` curve editor in the inspector) claims its chords with
  `UI::UpdateEditingChordClaim(chords)` (`Editor/UI/EditingChordClaims.h`) inside that child every frame it is drawn;
  `FindClaimedEditingChords` returns them for the panel's root window, also while the widget's own popup is open (no
  focused route takes a key then). Test it through the real `FindKeyboardWindow`, `HandleEditorEditingKeys` and
  `DrawEditorEditingMenuItems` (CurveAssetEditorTests.cpp does). While a modal
  popup is open (`ImGui::GetTopMostPopupModal() != nullptr`) no editing key runs, Undo and Redo included, so nothing
  changes what the dialog acts on.
- While the game has the input (`GameInputRouter`, the ImGui layer's input filter), ImGui receives no key presses but
  the play shortcuts and no clicks over the game view: global shortcuts need no play-mode checks, and a new shortcut that
  must work while the game has the input goes into `GameInputRouter::RouteKeyPress` too. While the game also locks the
  cursor, ImGui receives no mouse movement and changes no cursor (`GameViewPanel::UpdateWindowCursors`).
- The create menus (the Entity menu, the hierarchy's create button and Create Child) all draw
  `UI::DrawEntityPresetMenuItems` over `EntityPresets` (categories, presets and what they create, unit tested without
  UI): new kinds of entities are presets there. Menus that agents open (`editor.entity-menu`,
  `inspector.add-component-menu`) take a request the panel applies in its next frame, calling `ImGui::OpenPopup` with
  the menu's label just before the `BeginMenu`/`BeginPopup` that draws it (same window, same ID); the commands refuse
  to open a menu over a dialog (`EditorCommandEnvironment::IsDialogOpen`), whose modal popup it would close.
- Native dialogs: `FileDialogs::OpenFile/OpenFiles/SaveFile/PickFolder` (editor only; add the extension when it is
  missing).
- Actions that discard the scene go through `EditorLayer::RequestSceneAction` (unsaved-changes prompt).

## Windows that exist once per asset

The visual script editor's graph windows (`VisualScriptEditorPanel`, managed by `VisualScriptEditors`) are the pattern
for windows that exist once per open asset (Docs/Architecture.md §11.1):

- Name the window `<title>###<kind><handle>` (`VisualScriptEditorPanel::GetWindowName`): the ID after `###` survives
  renames and title changes, and other code finds the window by it.
- Pass `ImGuiWindowFlags_NoSavedSettings` to the window and to every popup and modal it opens: `imgui.ini` must not keep
  windows that may not exist next session. Compute the placement each session instead (`SetNextWindowDockID` with
  `ImGuiCond_FirstUseEver`: the node of the window of its kind focused last, else a default panel's node, else a floating
  rectangle), re-dock them in View > Reset Layout, and keep what users expect back (which were open, their views) in a
  per-user file of your own (`VisualScriptViewSettings`), written when windows close, the project changes and at exit,
  never per frame.
- Give the manager non-draw methods (open, close, describe) that make no ImGui call, so automation and the fuzz test can
  use it without an ImGui context, and draw only from `OnImGuiRender` and the menu functions.
- Inject services (the monospace font, a Show Console callback) through a struct such as `VisualScriptEditorServices`
  rather than reading `ImGuiLayer` from a panel: tests and headless editors have none.

## Canvases

- Input: the window and the canvas child carry `ImGuiWindowFlags_NoNavInputs` (so Tab, Space, Enter and the arrows are
  the canvas's), plus `NoScrollbar` and `NoScrollWithMouse` (so the wheel is). The canvas's first item is a whole-area
  `InvisibleButton` for every mouse button after `SetNextItemAllowOverlap()`: a press makes it active until release, so
  gestures keep their capture outside the canvas, while widgets drawn later on top still win the hover. Hit-test your own
  elements; they are not ImGui items.
- Open popups explicitly with `ImGui::OpenPopup` on a right release that moved at most a few pixels since the press,
  never with `BeginPopupContextItem`, which opens after a right-drag pan too.
- Own Escape: while the canvas is focused and no text field is, call `ImGui::SetKeyOwner(ImGuiKey_Escape, ownerId)` every
  frame and read it with `IsKeyPressed(ImGuiKey_Escape, 0, ownerId)`; otherwise ImGui's keyboard navigation cancels the
  active item or moves the focus to the parent window.
- A part of a window that owns keys of its own while it has the keyboard (the Timeline drawer) is a route class of its
  own (`VisualScriptKeyRoute::Timeline`, its rows a group of `VisualScriptShortcuts`): it takes the focused chords it
  shares with the window (Delete, Ctrl+A) through the window's claim of them, matches its other keys exactly under a key
  owner of its own, and gives the keyboard back to the canvas with Escape when no drag is in progress (a drag's Escape
  cancels it). Its pure model (`VisualScriptCurveView`: the view, hit testing and the gestures as intents) is tested
  without ImGui beside its L4 editor (`VisualScriptCurveEditor`).
- Curves (keys over an input, with interpolation and slopes) are edited with the one curve editor
  (Docs/Architecture.md §6.5): the pure `CurveView<TDocument>` and the widget `CurveEditor<TDocument>` in `Editor/UI`,
  over a `CurveDocumentTraits<TDocument>` the host specializes with its document's reads, edits and domain, instantiated
  once in the host's `.cpp` (which alone includes `CurveViewImpl.h` and `CurveEditorImpl.h`). The widget reports
  `CurveEditorChange`s and the host commits them: the Timeline drawer through its widget commits, the `.scurve` drawer
  (`CurveAssetEditor`) as data asset edits with a merge key per drag and per nudge run. A host in a panel claims the
  widget's keys as focused routes of the widget's child window while it is drawn, as `CurveAssetEditor` does.
- Match raw keys as exact chords with `ImGui::IsKeyChordPressed(chord, flags, ownerId)` (a plain `IsKeyPressed(C)`
  also fires on Ctrl+C). Claim editing keys that share a global chord (Delete, Ctrl+D, Ctrl+A) with
  `ImGui::Shortcut(chord, ImGuiInputFlags_RouteFocused)` every frame the window is drawn. Keep the keys and gestures in
  tables (`VisualScriptShortcuts.h`) that the handler, the menus, the shortcut sheet and the docs read; the tests check
  them against the editor's global chords, the chords macOS keeps, and Docs/Architecture.md.
- One widget interaction is one undo step even when the user clicks straight from one widget into another: ImGui makes
  the clicked one active in the frame the first releases, so `IsAnyItemActive()` never turns false between them. End
  the step when the active ID changes (`EditSession::Update(activeId != 0 && activeId == previousActiveId, history)`),
  after the widget committed its last value in the frame it was deactivated.
- Drags are computed from the state the interaction started with (the timeline, the length, the positions), never by
  accumulating per-frame deltas into the document, so a drag back to its start leaves nothing changed and a refusal
  midway cannot drift.
- A text field whose text names pins (an Expression's formula) commits like a name field (once, when left, on
  Ctrl+Enter or Enter; Escape cancels), against the text the interaction started from, so inputs renamed in order keep
  their links. A block of messages that selects part of the field's text when clicked (the formula's caret blocks) sets
  the selection through the field's `ImGuiInputTextFlags_CallbackAlways` callback, whose `SelectionStart` and
  `SelectionEnd` are writable on every frame the field is active (imgui.h:1348, :2808-2809).
- Values being edited survive edits made elsewhere: `VisualScriptWidgetCommits` rebases a pending value over an outside
  change (`RebaseVisualScriptEdit`, a pure function) and drops it with a note only when the change touched the same
  element. Canvas gestures do the same: the model records what the outside change did (`OutsideChange`) and
  `RebaseGesture` continues the gesture on the new document while its elements are untouched, or ends it with a toast
  naming the element ("Your move was interrupted: Branch (node 12) was changed elsewhere"). A new press kind gets a case
  in `RebaseGesture`'s Idle branch (which makes a press inert), and a new gesture a case in `RebaseGesture` and, when its
  interruption names an element, in `IsGestureElement`.
- A window whose panes do not fit side by side stacks them (`VisualScriptWindowLayout::Stacked`: one column, My Script
  above Details at a share of the column, a splitter between them) before it hides any; both report Shown.
- Drawing: emit layers in order into one draw list (no `ImDrawListSplitter`: with 16-bit indices and `VtxOffset` every
  64K vertices and clip change makes a draw command anyway). Draw text only at whole pixel sizes from a fixed ladder
  (`VisualScriptTextLadder`), so it stays crisp and the atlas bakes few sizes. `ImDrawList::AddText` takes the final
  pixel size, while `PushFont` takes the unscaled base size. Widgets drawn inside a zoomed `PushFont` (a value edited on
  the canvas) never open their popups there: report the request and open the popup after the scope, at normal size.
  Stroke widths are whole pixels. Custom primitives read `_VtxCurrentIdx` after `PrimReserve`. Atlas custom rectangles
  are read again whenever the font atlas's `TexData` changes.
- Text with → or ← is drawn with `UI::DrawArrowText` and measured with `UI::CalcArrowTextSize`: the editor's fonts
  (Roboto, Cousine) have no glyphs for U+2192 and U+2190, which are drawn as shapes.
- Values edited on a canvas use `FieldEditor::DrawInlineValue` in `FieldInlineMode::Compact` (sized cells, no tooltips,
  a popup request instead of a popup), whose popup body `FieldEditor::DrawValuePopup` draws.
- Names users type are checked with `VisualScriptSerializer::IsValidName`, the loader's own rule; read-only files with
  `FileSystem::IsReadOnly`, the editor's one rule.

## Debugging and the live game

- A graph window draws debugging and the live game from pure views the window manager builds once per frame
  (`VisualScriptWindowDebugInput`, `VisualScriptCanvasDebug.h`): the window maps them onto its canvas after its own sync
  and the renderer draws only the result. Keep the session itself (`VisualScriptDebugSession`) free of ImGui and panel
  headers.
- Windows never run graph code while they draw. Resumes and Run to Node that the UI asks for, the play controls'
  unpause included, are queued (`RequestResume`) for the next `BeginFrame`, which runs before automation. Read-only VM
  queries that run no graph code (`ReadValues`, `DescribeInstances`, `CheckCondition`, `DescribeStop`) may be made while
  drawing, at a bounded rate (live values at 4 or 10 Hz, only for windows that show their canvas).
- Put behaviour in units `EditorLayer` only calls (`VisualScriptDebugControls`, `FindEditorWindowRect`,
  `WantsHiddenGameRender`), so headless tests drive the real code instead of a copy of the editor's frame.
- Values in cells go through the one formatter (`FormatVisualScriptDebugValue`): colours as swatches, entities by name,
  arrays and strings on one line, clipped with `RenderTextEllipsis` before the next column, the whole value in the
  tooltip. My Script's live value cells, the Debugger panel and value pills share it.
- Texts use only the fonts' glyphs: arrows through `UI::DrawArrowText` (or `UI::WrapArrowText`), other symbols as drawn
  shapes, key hints through `FormatVisualScriptChord`; new texts are constants in a list the glyph test reads
  (`VisualScriptPanelTexts::All`, `VisualScriptDebuggerPanel::Texts::All`, built with `std::to_array`, so append to them).
- The debug keys (F5, F10, F11, Ctrl+F11, Shift+F11) are global chords listed in `VisualScriptEditorChords` and shown in
  the shortcut sheet as `Global` rows; they are inert behind modals (`ImGui::GetTopMostPopupModal()`), and canvas keys
  never equal them. While the session holds the pause the controls take the game's input back every frame, and give it
  back only where the Game View shows without a tab change.
- The live-edit feedback reads one revision of the live log per frame (`VisualScriptEngine::GetLiveLog`): receipt toasts
  for the window's own document changes only, the Live popover, the outline of what does not run
  (`FindVisualScriptNotRunning`), runtime discs and the drawer's Runtime section. Tell Play from Simulate by
  `EditorPlayState::Play`: Simulate is no play session.
- The game preview renders the game only when the Game View rendered nothing that frame (`WantsHiddenGameRender`), and
  Game View Beside runs at the start of a frame, before the dock node's windows are submitted
  (`DockBuilderSplitNode` moves a node's windows to the opposite child; `DockBuilderDockWindow` only sets a window's dock
  ID).

## Tests

- Logic: plain doctest cases in `Tests/StradaEditorTests`.
- Widgets and panels: the headless ImGui fixture (`HeadlessImGui.h`: null backends, 1920x1080 display) draws panels
  for a few frames; assert that drawing alone changes nothing (undo count, serialized scene, files) and drive inputs
  with `SetKeyboardFocusHere`, typed characters, key events and `HeadlessImGui::Drag` (a real mouse drag between two
  screen points; place panels with `SetNextWindowPos/Size` and find positions from `ImGuiWindow` data).
- `HeadlessImGui` keeps the clipboard in memory (`ImGui::SetClipboardText`/`GetClipboardText` never reach the
  system's) and adds `DragPath(from, to, draw, button = Left, steps = 4)` for straight drags with any button,
  `Wheel(position, delta, draw)`, `Chord(ImGuiMod_Ctrl | ImGuiKey_X, draw)`,
  `MouseClick(position, button, draw, count = 1)` and `UseEditorStyle(docking = false)`, which sets up keyboard
  navigation, the theme and the editor's fonts as `ImGuiLayer::OnAttach` does and returns the monospace font (call it
  before the first frame when docking).
- Send modifiers through `HeadlessImGui::Chord` or `HeadlessImGui::SetModifiers`, never as raw `ImGuiMod_Ctrl` key
  events: on macOS `ConfigMacOSXBehaviors` swaps Ctrl and Super as the platform reports them. A test of something that
  depends on a per-user default that differs by platform (the visual script editor's scroll mode is Pan on macOS) sets
  it first.
- Portability traps no Windows build shows: a nested struct with default member initializers held in a `std::optional`
  member and created with `emplace()` declares its default constructor and defaults it in the .cpp (Clang with
  libstdc++ sees the initializers only once the enclosing class is complete); remove elements with `std::erase_if` or
  `std::remove_if`, never with a hand-written move loop that may move-assign an object to itself (libstdc++ and libc++
  empty it, MSVC does not).
- A test of a shortcut's text expects it as `FormatVisualScriptChord` names it for the ImGui context's
  `ConfigMacOSXBehaviors` (Cmd and Option on macOS, Ctrl and Alt elsewhere), or passes the flag explicitly; never a fixed
  "Ctrl+...". The editor's own menus format through ImGui and name Ctrl everywhere.
- Debugger UI tests run a scripted session under `Testing::ScriptRuntimeScope` with `PlayMode` and the debug session
  (the runtime tier), stop on real breakpoints, and check the draw record (`Kind` and `Mark` of the debug items), the
  strip's rows and buttons and the panel's values after the panel invariant.
- Check the result visually with an automation screenshot (`viewport.screenshot`, or `viewport.screenshot {window}` for
  one window) of the running editor.
