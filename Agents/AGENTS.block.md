## Strada game project

This folder is the Strada game project "{{ProjectName}}", whose project file is `{{ProjectFile}}`. Strada's editor
makes it: scenes of entities and components, materials, prefabs, C# scripts and visual scripts (graphs).

- **Work through the editor's tools.** `.mcp.json` registers the `strada` MCP server, which serves every editor
  automation command as a tool named after it with `_` for `.` and `-` (`entity.create` is `entity_create`). The tools
  act on the editor the user sees, or on one without a window, through the same undo history as the user's edits. Use
  them instead of editing scene, prefab, material, graph or project files by hand: they keep references and the asset
  registry consistent.
- **Read first.** Read `.strada/docs/Agents.md` before you start, and follow the `strada-make-game` skill
  (`.claude/skills/strada-make-game/SKILL.md`). `.strada/docs/Automation.md` is the reference of every command's
  parameters and results; `Scripting.md`, `VisualScripting.md`, `Components.md` and `ScriptingAPI.md` beside it describe
  C# scripts, graphs, components and the scripting API.
- **Look up, never guess.** `editor_status` shows the open project and scene and whether they have unsaved changes;
  `component_types`, `graph_library` and `asset_list` list what exists. Read `log_read` whenever a result surprises you.
- **Save your work.** `scene_save` writes the scene; an editor without a window drops unsaved changes when the session
  ends.
- **Prove it in the game before you say it works.** `script_build` must report no errors, and `graph_get` no Error
  in a graph's `diagnostics` (a graph with one does not run). Then play: `play_start` (with `paused: true` for exact steps), `play_advance` with `frames`, `input_set`
  for keys, `viewport_screenshot` to look, `play_stop` to end. Run the test scene with `test_run` and read every check.
- **Ask the user first** before you delete assets (`asset_delete`, `asset_delete_folder`), export the game
  (`project_export`), drop unsaved changes (`discardChanges: true`), or change anything outside this folder.

Strada writes the text between the `strada-agents` lines of this file: keep your own notes outside them. `.strada/`
holds files Strada manages; the editor's `File > Set Up for Coding Agents...` refreshes them and this text.
