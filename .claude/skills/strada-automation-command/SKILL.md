---
name: strada-automation-command
description: How to add or change an editor automation command in Strada (CommandRegistry definition with JSON schema, undoable EditorOperations, error codes, async completion, Docs/Automation.md entry, StradaEditorTests). Use whenever an editor capability must be reachable by AI agents or the strada CLI/MCP bridge.
---

# Adding an automation command

Automation commands are how agents drive the editor. They must behave exactly like the UI: same operations, same undo
history, same validation. Read `Docs/Automation.md` (protocol and reference) and `StradaEditor/Source/Editor/Automation/`.

## 1. Put the behavior in EditorOperations first

Scene changes are `EditorCommand`s executed through `EditorContext::ExecuteCommand` (undo/redo, dirty tracking,
merging). Add an `EditorOperations` method (and a command class in `Editor/Commands/` if no existing one fits) so the
future UI panel calls the same code. Never mutate the scene directly from a handler.

## 2. Register the command

In `EditorCommands.cpp` (or a new `<Domain>Commands.cpp` registered from `RegisterEditorCommands`):

- Name `domain.action` (lowercase, digits, hyphens). `strada mcp` exposes it as the tool `domain_action` (dots and
  hyphens become underscores), with the command's description and schema.
- Description: what it does and what it returns, written for an agent that has never seen the code.
- Parameters with `SchemaBuilder` (descriptions on every property, `MinLength`, `Minimum`, `Enum`, defaults). Schema
  validation runs before the handler; the handler may assume types.
- `ReadOnly = true` when nothing changes.
- Entity parameters are decimal UUID strings: use `ParseExistingEntity` / `ParseExistingEntities`.
- Errors: `EntityNotFound`, `ComponentNotFound`, `InvalidParams` (bad values), `InvalidOperation` (state), `FileError`,
  `Unavailable`, `UnsavedChanges` (offer `discardChanges`), `ConsentRequired` (the user must decide: offer `confirmed`). Never add new numeric codes without updating
  `AutomationErrorCode`, `AllAutomationErrorCodes` and the docs table.
- Long operations use `AsyncHandler` + `CommandCompletion` and complete on the main thread; an abandoned completion
  reports `Cancelled` automatically. A command that must see the result of a UI draw (`graph.palette`) completes after
  that draw (`VisualScriptEditors::AfterNextDraw`).
- A command that waits on play mode (a stop, frames, real time: `graph.wait`) completes through `CommandCompletion` on
  the main thread exactly once, also when play stops (`PlayStopped`) or the editor closes (`Cancelled`); it never blocks
  a frame, and answers at once when nothing can happen (no frame can run): with nothing playing `graph.wait` succeeds
  at once with reason `NotPlaying`, while a command that must run frames (`graph.trace`, `graph.step`) refuses with
  InvalidOperation. The automation fuzz holds every debug command's errors to its parameter and state refusals
  (InvalidParams, AssetNotFound, EntityNotFound, FileError, Unavailable, InvalidOperation).
- Commands run back to back with no editor frame between them (one JSON-RPC batch): a command that reads or changes the
  debug session or play mode brings it up to date first (`Refresh`, `PlayMode::RefreshObserver`), and session mutators
  push the configuration in their own call, so each command sees the one before it. Report play states with
  `GetEditorPlayStateName`, describe stops with `DescribeVisualScriptStop` and receipts with
  `DescribeVisualScriptLiveEdits`, and read unsigned parameters (`after`, `offset`) through a saturating reader.

## Commands that drive editor UI

Some commands act on UI objects that exist only with an ImGui context (the content browser, the graph windows). Reach
them through an optional member of `EditorCommandEnvironment` (`ContentBrowser`, `GraphEditors`), set by `EditorLayer`
in `OnAttach` inside its ImGui block:

- Unset (headless), the command fails with `Unavailable` ("the editor has no graph windows (headless)"). Check the
  parameters the command can check without the UI first only when its documented error order says so.
- Refuse while a dialog is open (`EditorCommandEnvironment::IsDialogOpen`, `InvalidOperation`) when the command would
  open a popup over it.
- Changes the UI applies at its next draw are reported as requested; a ReadOnly command that reports UI state
  (`graph.editors`) reports what the last draw saw, and documents that.
- `AutomationFuzzTests` builds the real object without an ImGui context, so its non-draw methods must make no ImGui
  call (the fuzz then proves it). Give the fuzz fixture a seed that reaches the command's work (a graph with functions,
  comments and every key, an open window), bias parameters toward what exists, check outcomes after the loop
  (`outcomes["graph.open"].first > 0`, error codes that must occur), and keep the files the command writes (the views
  file) inside the test's `writable` directories.

## 3. Document it

Add a row to the command tables in `Docs/Automation.md`. `EditorCommands: every command is documented` fails otherwise
(`GraphCommands: every graph command is registered, documented and read-only when it changes nothing` for `graph.*`).
Name the command in the MCP server's `Instructions` (`StradaTool/Source/Mcp/McpServer.cs`) and the `strada-make-game`
skill when agents should know it exists.

## Commands and operations of the visual script editor

- Every editing operation of the visual script editor (`VisualScriptEditing`, `VisualScriptLanguageEditing`,
  `VisualScriptCrossGraph`) is an operation of `graph.edit` too, in the same change: a row of `BatchOperations` in
  `VisualScriptBatch.cpp` (its name, a field list where `*` marks a required field, and a handler that calls the editing
  function), its route in `BatchExecutor::SetSetting` when a setting belongs to it, its fields in
  `MakeGraphOperationSchema`, its name in `graph.edit`'s description (a test checks every operation is named), its entry
  in the batch generators of `GraphEditFuzzTests.cpp` and `AutomationFuzzTests.cpp`, and its row in the graph.edit
  operations table of `Docs/Automation.md`. Handlers that need pins call `Require` first; those that move or frame nodes
  call `RequirePlaced`; those that rename what titles show record `TitleLevel`; those that delete or create elements
  record it (`RecordRemovals`, `RecordCreated`).
- Fuzz seeds name other graphs: the automation fuzz's seed script holds a typed Send, a library call and a remote Get
  naming `Scripts/FuzzLibrary.sgraph` and itself, and `ReseedGraph` rebuilds both. A new cross-graph kind or setting gets
  a node there.
- Every new reader of a node's `Variable`, `Function` or `Name` settings checks `IsLocalVisualScriptReference` first: a
  node whose `Graph` names another script names that script's elements, never a local one of the same ID.

## 4. Test it

`Tests/StradaEditorTests/Source/EditorCommandsTests.cpp`: success path, every error code it can return, and undo/redo
when it modifies the scene. Network-level behavior is covered once in `AutomationServerTests.cpp`; new commands do not
need TCP tests.

## 5. Verify end to end

Run `StradaEditor --headless`, read the instance file, and call the command over TCP (see the example session in
`Docs/Automation.md`).
