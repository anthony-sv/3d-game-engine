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
- One widget interaction must be one undo step: keep an `EditSession` per panel (unique key prefix), call
  `Update(ImGui::IsAnyItemActive(), history)` at the start of the panel, and pass `GetMergeKey()` to operations.
- Report failed operations with `UI::ReportFailure(result, "Doing X")` (logged, shown in the console).

## Changing the scene while drawing

- Deleting, creating or moving entities while iterating the hierarchy invalidates the iteration: queue the action
  (`Defer` in `SceneHierarchyPanel`, `m_Deferred` in `InspectorPanel`) and run it after drawing.
- Selection changes from a panel set a "from panel" flag so other code can tell them from external selections.

## Drag and drop, menus, shortcuts, dialogs

- Payload types live in `DragDropPayload` (`Editor/UI/EditorUI.h`): `Entities` carries `uint64_t` UUIDs, `Asset` one
  `uint64_t` handle, `AssetFolder` a folder path (UTF-8, no terminator). Validate dropped data (entities may not exist,
  assets may have the wrong type).
- Assets: make an item a drag source with `UI::AssetDragSource(handle, label)`; inside a drop target accept with
  `UI::AcceptAssetDrop(types)`, which highlights only for registered assets of those types. What dropping an asset onto
  the scene does lives in `AssetDrops` and `EntityPresets::CreateFromMesh` (unit tested without UI).
- Global shortcuts use `ImGui::Shortcut(chord, ImGuiInputFlags_RouteGlobal)` in `EditorLayer::HandleShortcuts` and skip
  editing keys while `io.WantTextInput`; show the chord in the matching menu item. A panel with its own meaning for a
  key (Delete or F2 in the content browser) registers `ImGuiInputFlags_RouteFocused` every frame while it is focused,
  even when it has nothing to act on, so the key never falls through to the global shortcut.
- Native dialogs: `FileDialogs::OpenFile/OpenFiles/SaveFile/PickFolder` (editor only; add the extension when it is
  missing).
- Actions that discard the scene go through `EditorLayer::RequestSceneAction` (unsaved-changes prompt).

## Tests

- Logic: plain doctest cases in `Tests/StradaEditorTests`.
- Widgets and panels: the headless ImGui fixture in `PanelTests.cpp` (null backends, 1920x1080 display) draws panels
  for a few frames; assert that drawing alone changes nothing (undo count, serialized scene, files) and drive inputs
  with `SetKeyboardFocusHere`, typed characters, key events and `HeadlessImGui::Drag` (a real mouse drag between two
  screen points; place panels with `SetNextWindowPos/Size` and find positions from `ImGuiWindow` data).
- Check the result visually with an automation screenshot (`viewport.screenshot`) of the running editor.
