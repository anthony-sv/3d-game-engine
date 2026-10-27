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
  `ImportDropped` (which `asset.drop` calls for agents) selects what came in and shows a notice of what did not.
- Global shortcuts use `ImGui::Shortcut(chord, ImGuiInputFlags_RouteGlobal)` in `EditorLayer::HandleShortcuts` and skip
  editing keys while `io.WantTextInput`; show the chord in the matching menu item. A panel with its own meaning for a
  key (Delete or F2 in the content browser) registers `ImGuiInputFlags_RouteFocused` every frame while it is focused,
  even when it has nothing to act on, so the key never falls through to the global shortcut.
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
- Match raw keys as exact chords with `ImGui::IsKeyChordPressed(chord, flags, ownerId)` (a plain `IsKeyPressed(C)`
  also fires on Ctrl+C). Claim editing keys that share a global chord (Delete, Ctrl+D, Ctrl+A) with
  `ImGui::Shortcut(chord, ImGuiInputFlags_RouteFocused)` every frame the window is drawn. Keep the keys and gestures in
  tables (`VisualScriptShortcuts.h`) that the handler, the menus, the shortcut sheet and the docs read; the tests check
  them against the editor's global chords, the chords macOS keeps, and Docs/Architecture.md.
- One widget interaction is one undo step even when the user clicks straight from one widget into another: ImGui makes
  the clicked one active in the frame the first releases, so `IsAnyItemActive()` never turns false between them. End
  the step when the active ID changes (`EditSession::Update(activeId != 0 && activeId == previousActiveId, history)`),
  after the widget committed its last value in the frame it was deactivated.
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
- Check the result visually with an automation screenshot (`viewport.screenshot`) of the running editor.
