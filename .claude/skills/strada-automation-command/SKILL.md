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

- Name `domain.action` (lowercase, digits, hyphens). The MCP bridge exposes it as `domain_action`.
- Description: what it does and what it returns, written for an agent that has never seen the code.
- Parameters with `SchemaBuilder` (descriptions on every property, `MinLength`, `Minimum`, `Enum`, defaults). Schema
  validation runs before the handler; the handler may assume types.
- `ReadOnly = true` when nothing changes.
- Entity parameters are decimal UUID strings: use `ParseExistingEntity` / `ParseExistingEntities`.
- Errors: `EntityNotFound`, `ComponentNotFound`, `InvalidParams` (bad values), `InvalidOperation` (state), `FileError`,
  `Unavailable`, `UnsavedChanges` (offer `discardChanges`). Never add new numeric codes without updating
  `AutomationErrorCode`, `AllAutomationErrorCodes` and the docs table.
- Long operations use `AsyncHandler` + `CommandCompletion` and complete on the main thread; an abandoned completion
  reports `Cancelled` automatically.

## 3. Document it

Add a row to the command tables in `Docs/Automation.md`. `EditorCommands: every command is documented` fails otherwise.

## 4. Test it

`Tests/StradaEditorTests/Source/EditorCommandsTests.cpp`: success path, every error code it can return, and undo/redo
when it modifies the scene. Network-level behavior is covered once in `AutomationServerTests.cpp`; new commands do not
need TCP tests.

## 5. Verify end to end

Run `StradaEditor --headless`, read the instance file, and call the command over TCP (see the example session in
`Docs/Automation.md`).
