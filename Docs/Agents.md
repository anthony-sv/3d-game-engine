# Building games with AI agents

The Strada editor can be driven entirely by programs. Every operation of the editor is an automation command, and
`strada mcp` serves the commands as Model Context Protocol (MCP) tools, so an AI agent such as Claude Code can make a
whole game: the project, scenes, materials, prefabs, C# scripts and visual scripts, then play it, test it, debug its
graphs while it runs, and export it. This guide explains what agents can do, how to set them up, how an agent should
work, and how the sample projects were made this way. [Automation.md](Automation.md) is the reference of every command,
and the [`strada-make-game`](../.claude/skills/strada-make-game/SKILL.md) skill is the playbook agents follow.

## Contents

- [What agents can do](#what-agents-can-do)
  - [The safety model](#the-safety-model)
- [Setup](#setup)
  - [In this repository](#in-this-repository)
  - [In a game project elsewhere](#in-a-game-project-elsewhere)
  - [Other MCP clients](#other-mcp-clients)
  - [How strada finds or starts an editor](#how-strada-finds-or-starts-an-editor)
  - [The strada command line](#the-strada-command-line)
- [How an agent should work](#how-an-agent-should-work)
- [Visual scripts with agents](#visual-scripts-with-agents)
  - [Authoring graphs](#authoring-graphs)
  - [Graphs that read well](#graphs-that-read-well)
  - [Watching and debugging](#watching-and-debugging)
  - [The documented debugging session](#the-documented-debugging-session)
- [How the samples were made](#how-the-samples-were-made)
- [Troubleshooting](#troubleshooting)

## What agents can do

The editor runs an automation server: a JSON-RPC 2.0 endpoint on `127.0.0.1`. Every command runs on the editor's main
thread through the same operations and undo history as the UI, so an agent and a person see and edit the same state.
The editor has 96 commands, each with a JSON schema of its parameters (`editor.commands` lists them at run time):

| Group | Commands | What they cover |
|-------|----------|-----------------|
| `editor` | 6 | The editor's status, the command list, undo and redo, quitting, and the Entity menu as users see it |
| `project` | 6 | Creating, opening and closing projects, their settings, and exporting the game |
| `scene` | 6 | New, open and save, the hierarchy, the whole scene file, and the scene's settings (physics, renderer) |
| `entity` | 8 | Creating entities with their components, deleting, duplicating, renaming, reparenting, finding, reading and selecting them |
| `component` | 5 | Every component type with its fields, and adding, removing, reading and setting components |
| `inspector` | 1 | The Add Component menu as users see it |
| `asset` | 12 | Listing, importing (also as a drop from the file manager), moving and deleting assets and folders, and the content browser |
| `material` | 3 | Creating, reading and setting materials |
| `graph` | 16 | Visual scripts: creating, reading, checking and editing them, arranging, copying, the node library and types, finding, and the graph windows |
| `graph` (debugging) | 11 | Breakpoints, targets, stops, continue, step, break, wait, traces, values, instances and live edits |
| `prefab` | 2 | Creating prefabs and placing instances |
| `script` | 4 | The C# scripts' status and classes, creating and building scripts |
| `play`, `input`, `ui`, `test` | 10 | Play mode with exact frame steps, keys and mouse buttons, where the UI is on screen, and test runs |
| `log`, `viewport` | 6 | The log, screenshots of the editor or one of its windows, the scene view's camera, overlays and statistics |

`strada mcp` serves each command as a tool named after it, with `.` and `-` replaced by `_` (`entity.create` is
`entity_create`, `asset.create-folder` is `asset_create_folder`); read-only commands carry `readOnlyHint`. strada adds
six tools of its own that search and import [Poly Haven](https://polyhaven.com) assets (CC0):
`polyhaven_search_hdris` and `polyhaven_import_hdri` (skies for image-based lighting), `polyhaven_search_models` and
`polyhaven_import_model` (glTF models with their textures), and `polyhaven_search_textures` and
`polyhaven_import_texture` (PBR texture sets, made into a material). That is 102 tools in all.

### The safety model

- **The server is local and authenticated.** The editor listens on `127.0.0.1` only. A client must first send the token
  from the editor's instance file, which lives in a user-only directory; strada does this for you. `--no-automation`
  starts an editor without the server.
- **Edits are undo steps.** Edits of the scene, its entities and components, materials and visual scripts made through
  the commands are steps of the editor's history, the same history as the UI's. The person at the editor can undo what
  an agent did with Ctrl+Z (Edit > Undo names the step), and `editor_undo` undoes it for the agent. A `graph_edit` batch
  is one step, however many operations it holds.
- **Agents work on the editor you see, or on a headless one.** With an editor open, strada uses it, and you watch the
  agent's edits arrive. An agent can also work on an editor without a window (`--headless`), which strada closes when
  it ends.
- **Some operations are not undo steps.** Project settings are saved to the project file at once. Asset file
  operations (importing, moving, deleting assets and folders, `asset_drop`) and `graph_create` change files directly;
  deleting an asset discards the undo steps that edited its file, and creating, opening or closing a project drops every
  step of asset files. Selections (`entity_select`, `asset_select`), the scene view's camera and overlays, and the
  visual-script debugger's breakpoints and targets are view state, not history.
- **Unsaved work is protected.** Commands that would discard unsaved changes (a new scene, another project, quitting)
  fail with `UnsavedChanges` unless they get `"discardChanges": true`.
- **Play mode runs a copy.** While the game plays, scene and entity commands act on a copy of the scene with a history
  of its own, and stopping discards it. Edits of asset files made while playing (materials, graphs) stay in their files
  and stay undoable. Scene and project file commands are refused while playing.
- **The commands are fuzzed.** The editor's tests send every registered command parameters built from its schema: each
  must answer once and never with an internal error, the scene must stay loadable, undoing and redoing everything must
  restore it, and nothing outside the project may change. The five commands that replace the open project or create and
  build scripts (`project.create`, `project.open`, `project.close`, `script.create`, `script.build`) have tests of their
  own instead.

## Setup

Build the engine first (`python Tools/build.py --config release`, see [Getting started](GettingStarted.md)): strada
starts the `StradaEditor` that the build made.

### In this repository

The repository's `.mcp.json` registers `strada mcp` for agents working in the checkout:

```json
{
	"mcpServers": {
		"strada": {
			"command": "dotnet",
			"args": ["run", "--project", "StradaTool/StradaTool.csproj", "--no-launch-profile", "--", "mcp"],
			"env": {
				"DOTNET_NOLOGO": "1"
			}
		}
	}
}
```

`dotnet run` builds strada when needed. A strada run this way finds the editor as the most recently built
`build/*/bin/StradaEditor` below the repository root. Claude Code reads `.mcp.json` when you start it in the checkout,
and the skills in `.claude/skills/` (among them `strada-make-game`) teach it the workflow.

### In a game project elsewhere

A game project outside the repository registers the built tool, which sits next to the editor in `build/<preset>/bin/`
(`strada.exe` on Windows):

```json
{
  "mcpServers": {
    "strada": { "command": "<engine>/build/<preset>/bin/strada", "args": ["mcp", "--project", "Game.sproj"] }
  }
}
```

With `--project`, strada uses the editor that has this project open, and an editor it starts opens it. A relative path
is taken relative to the directory strada runs in. Unless `--editor` or `STRADA_EDITOR` names another, strada starts the
`StradaEditor` next to it.

### Other MCP clients

Any MCP client that starts servers over standard input and output works. Give it the command `strada` (its full path)
with the arguments `mcp` and the options you want (`--project <file>`, `--headless`, `--new`, `--editor <path>`), or
the command `dotnet` with the arguments
`run --project <engine>/StradaTool/StradaTool.csproj --no-launch-profile -- mcp`.
strada speaks the protocol versions 2024-11-05 to 2025-11-25. Listing the tools needs no running editor: strada then
reads the commands from a headless editor it starts for that and closes again. Requests run concurrently.

### How strada finds or starts an editor

strada connects to an editor at its first call that needs one, and keeps using it:

1. **A running editor that fits.** Editors write an instance file to `<user data>/Editor/Instances/<pid>.json`
   (`%APPDATA%/Strada` on Windows, `~/Library/Application Support/Strada` on macOS, `$XDG_DATA_HOME/strada` or
   `~/.local/share/strada` on Linux). With `--pid <id>`, only the editor with that process ID fits, and nothing is
   started. With `--project <file>`, the editor that has the project open fits. Otherwise strada takes the newest
   instance file. With `--new` (`strada mcp` only), only editors strada itself started fit.
2. **Else an editor it starts.** strada starts the editor that `--editor <path>` names, else the one the
   `STRADA_EDITOR` environment variable names, else the `StradaEditor` next to strada, else the most recently built
   `build/*/bin/StradaEditor` below the working directory. The editor opens `--project` when given, and runs without a
   window with `--headless`. It must start its automation server within 60 seconds.
3. **What happens to it.** Editors strada starts with a window stay open for you. Editors it starts headless close with
   `strada mcp`, discarding unsaved changes: they get `--parent-process <strada's process ID>`, so they also close when
   strada is ended without closing them.

A connection that broke (the editor closed) is replaced on the next call.

### The strada command line

`strada` also works from a shell, for scripts and for you:

| Command | What it does |
|---------|--------------|
| `strada mcp` | The MCP server on standard input and output |
| `strada call <command> [params]` | Runs a command in a running editor and prints its result as JSON; `params` is a JSON object. It never starts an editor |
| `strada commands` | Lists the editor's commands (`--json`: as `editor.commands` describes them) |
| `strada launch` | Starts an editor, waits for its automation server and prints `{ processId, port, project, version }` |
| `strada instances` | Lists the running editors (`--json` for JSON) |
| `strada hdris [words]`, `strada models [words]`, `strada textures [words]` | Search Poly Haven's HDRIs, models and PBR texture sets by words in their names, categories and tags |
| `strada hdri <id>` | Downloads an HDRI and imports it into the running editor's project (`--resolution` 1k, 2k (default), 4k or 8k; `--folder` in Assets, default `Environments`) |
| `strada model <id>` | Downloads a model (glTF with its textures; `--resolution` of the textures, default 1k) into `<folder>/<id>` of the project (`--folder`, default `Models`) |
| `strada texture <id>` | Downloads a texture set into `<folder>/<id>` (default `Textures`) and creates the material `<id>.smat` from it |

| Option | Applies to | What it does |
|--------|------------|--------------|
| `--editor <path>` | `mcp`, `commands`, `launch` | The editor to start |
| `--project <file>` | `mcp`, `call`, `commands`, `launch`, imports | Use the editor that has this project open; editors strada starts open it |
| `--pid <id>` | `call`, `commands`, imports | Use only the running editor with this process ID |
| `--headless` | `mcp`, `launch` | Start editors without a window |
| `--new` | `mcp` | Never use an editor strada did not start |
| `--json` | `commands`, `instances`, searches | Print JSON |
| `--output <dir>` | imports | Only download the asset into this directory |
| `--help`, `--version` | | Show the usage or strada's version |

strada exits with 0 on success, 1 on failure (a command that failed prints its error) and 2 for wrong usage. Poly Haven
downloads are checked against the size and MD5 that Poly Haven lists, and the MCP import tools download nothing when no
project is open.

```sh
strada instances
strada launch --project Game.sproj
strada call editor.status
strada call entity.create '{"name": "Crate", "components": {"Mesh": {"Mesh": "builtin://Cube"}}}'
strada hdris sunset
```

Quote the JSON parameters as your shell needs. An editor that `strada launch` started keeps running after strada exits,
headless or not; `strada call editor.quit` closes it (`{"discardChanges": true}` to drop unsaved changes).

## How an agent should work

The [`strada-make-game`](../.claude/skills/strada-make-game/SKILL.md) skill is the full playbook. In short:

1. **Orient.** `editor_status` gives the open project and scene, unsaved changes and what undo would revert.
   `component_types` lists every component with its category and fields (types, defaults, ranges, enum values, asset
   types). `asset_list` with `builtIn: true` lists the built-in meshes (`builtin://Cube`, `Sphere`, `Capsule`,
   `Cylinder`, `Plane`, ...), materials, textures, fonts and the built-in sky (`builtin://DefaultSky`). `log_read`
   explains what went wrong whenever something is unexpected.
2. **Make or open the project.** `project_create` with a new or empty `directory` and a `name`, or `project_open`
   with a `.sproj` path. New projects start in `Assets/Scenes/Main.sscene`, their start scene. `project_settings`
   changes the window, the physics layers and the start scene.
3. **Build scenes in small, checked steps.** `entity_create` takes a `name`, a `parent` and `components` in the
   scene-file format. Entities are UUIDs written as strings, asset fields take `asset://<path in Assets>` or
   `builtin://<Name>`, and rotations are quaternions `[x, y, z, w]` or `RotationEuler` in degrees on Transform.
   `scene_hierarchy`, `entity_find` and `entity_get` show what was built. A scene needs a primary camera, light (a
   directional light, a sky light with an environment) and, for physics, a `RigidBody` and a collider on each body.
4. **Look good.** `material_create` with a `path` (`Materials/Name.smat`) and `fields` (`BaseColor`, `Metallic`,
   `Roughness`, `EmissiveColor`, textures); meshes use materials through `Mesh.Materials`. `asset_import` copies files
   into the project; with the editor's window, `asset_drop` imports a model with the files it reads. The Poly Haven
   tools bring models, texture sets and skies; a SkyLight's `Environment` takes an imported HDRI's `reference`.
   Particles, post-process volumes and HUDs are components like any other (the skill describes each).
5. **Reuse.** `prefab_create` writes an entity with its children to a `.sprefab`; `prefab_instantiate` places copies.
6. **Behavior in C#.** `script_create` with a `className` adds a class to the project's C# project and returns its
   file; write the class, then `script_build` compiles and loads it and returns every diagnostic to fix. A `Script`
   component runs it. [Scripting.md](Scripting.md) shows the API in use.
7. **Behavior in visual scripts.** `graph_create`, then `graph_edit` batches; a `VisualScript` component runs the graph
   (see [Visual scripts with agents](#visual-scripts-with-agents)).
8. **Look at the result.** In a windowed editor, `viewport_frame` points the scene view's camera at entities,
   `viewport_camera` sets the view, `viewport_overlays` chooses what the scene view draws over the scene (`colliders`
   `all` shows every physics shape), and `viewport_screenshot` returns an image of the editor or of one window
   (`window`: `SceneView`, `GameView`, `Hierarchy`, `Inspector`, `ContentBrowser`, `Console`, `Debugger` or `Graph`
   with `graph`). MCP clients receive the screenshot as image content.
9. **Play and test.** `play_start`, then `play_pause` and `play_advance` with `frames` for exact steps; `input_set`
   presses keys and mouse buttons in the game; `play_stop` discards what the run changed. To click a UI button, read its
   `center` from `ui_layout`, set the `mousePosition` there with `input_set`, press and release `Left` a frame apart.
   `test_run` runs a test scene (`scene`, `timeout`) and returns every check's result.
10. **Ship.** Save everything (`scene_save`), then `project_export` with a `directory` (and `smokeTest: true` to run the
    exported game headless for a few frames). It builds the scripts in Release and names the executable.

Read every result: errors say what to fix, and `InvalidParams` errors name the offending field in `data.path`. Use the
tools rather than editing scene, prefab or project files by hand: they keep references, the asset registry and the undo
history consistent.

Some calls, as the docs and the skill show them (tool name, then its arguments):

```
entity_create {"name": "Crate", "components": {"Mesh": {"Mesh": "builtin://Cube"}}}
component_set {"entity": "3218792578666045717", "component": "Transform", "fields": {"Translation": [0, 1, 0]}}
project_settings {"settings": {"Window": {"Width": 1920}, "Physics": {"Layers": ["Default", "Player"]}}}
scene_settings {"settings": {"Physics": {"Gravity": [0, -9.81, 0]}}}
```

Component data for `entity_create`, `component_add` and `component_set`, from the skill:

```
Transform and Mesh: {"Transform": {"Translation": [0, 1, 0], "RotationEuler": [0, 45, 0]},
                     "Mesh": {"Mesh": "builtin://Cube"}}
Script:             {"ClassName": "Game.Player", "Fields": {"Speed": {"Type": "Float", "Value": 5}}}
VisualScript:       {"Graph": "<asset>", "Variables": {"Speed": {"Type": "Float", "Value": 180}}}
PostProcess:        {"OverrideSaturation": true, "Saturation": 0.3}
```

An example of a whole workflow is one of strada's own tests (`Tests/StradaToolTests/Source/AgentWorkflowTests.cs`):
through `strada mcp` and a headless editor, it creates a project, adds a script with `script_create`, writes it and
builds it with `script_build`, creates a cube that runs it with `entity_create`, saves the scene, runs its checks with
`test_run`, exports the game with `project_export`, and runs the exported game with `--test`, which must pass the same
checks without the editor.

## Visual scripts with agents

Visual scripts are graphs of nodes (`.sgraph` assets) that a `VisualScript` component runs on its entity. They need no
C# project, only the scripting runtime (.NET 10). [VisualScripting.md](VisualScripting.md) explains the language, and
the `graph` sections of [Automation.md](Automation.md) describe the documents, `graph.edit`'s 51 operations, literals
and every diagnostic code.

### Authoring graphs

1. **Create.** `graph_create` with a `path` (`Scripts/Spin.sgraph`) and a `template`: `Events` (the default, an
   `Event.Create` and an `Event.Update` node; node 2 is the `Event.Update`), `Empty`, or `Library` (a function library
   that other graphs call and no entity runs).
2. **Look up, never guess.** `graph_library` lists the node kinds by `query`, `category` or `pinType`, each with the
   `Type` a node stores, its pins (name, direction, type) and its settings. `graph_types` lists the types; those with a
   `FieldType` can be public variables.
3. **Edit in batches.** `graph_edit` with `graph`, a `description` (the undo step's text) and `operations` applies the
   whole batch as one undo step, or nothing when one operation fails (the error names it). Name what you add with
   `"as": "$label"` and use the labels in later operations; name variables and functions by their names; integers are
   IDs of the document as the call found it. Add the nodes first and connect them after, so the batch analyzes once
   for the connections. Leave `position` out and end with `{ "op": "Arrange", "scope": "Graph" }` in a graph you made,
   or `"scope": "Created"` in a user's graph, which places only the new nodes. The batch that makes the spinning graph
   of the docs is in [Automation.md](Automation.md) and [VisualScripting.md](VisualScripting.md#a-first-graph).
4. **Read the result.** `summary` counts errors and warnings (new ones too); every Error in `diagnostics` must go, since
   a graph with one does not run. Each diagnostic's `Fixes` holds complete `Fix` operations to append to the next batch.
   `hints` name silent mistakes: `NeverRuns` (no flow into an impure node), `Unused` (a pure node nobody reads) and
   `UnlinkedInput` (an Expression input read as 0). `refuseNewErrors: true` writes nothing when the batch would add an
   Error, and `dryRun: true` answers what the call would do.
5. **Respect the user's work.** When someone works in the same graph, pass the `programRevision` that `graph_get` (or
   your last edit) returned: the call is refused, writing nothing, when the program changed since, but not for the
   user's moves, comments or arranging. Then `graph_get` and edit again.
6. **Whole documents.** `graph_set` writes a generated document at once; check it first with `graph_check` until
   `valid` is true.
7. **Attach and show.** `component_add` with the `VisualScript` component and `{"Graph": "<asset>", "Variables": ...}`
   runs the graph on an entity (never attach a library). `graph_open` opens its window in the editor, and
   `viewport_screenshot` with `window` `Graph` and `graph` shows it.

Graphs hold no entity values: other entities come from public `Strada.Entity` variables set per component, `Self`, an
event's `Other` or lookups. Graphs that use other graphs (libraries, typed `Event.Send`, remote `Variable.Get` and
`Variable.Set`) need `graph_interface` first, which gives the IDs, names and events to store; an edit that breaks other
scripts reports them in `referrers`, and `refuseBrokenReferrers: true` keeps such an edit out. Edit in dependency order:
a library first, then its callers. C# members marked `[VisualNode]` become nodes after `script_build` (`graph_library`
with `category` `Game`, whose `notes` say why a member did not become one).

### Graphs that read well

The samples' graphs follow these rules, and an agent's graphs should too. `SampleGraphsTests` holds the samples to the
layout: every node in exactly one titled comment box, no node closer than 16 units to another, no link between boxes,
and every box arranged.

- Declare first: one `graph_edit` per graph that adds only its variables (with tooltips, and ranges for numbers), its
  custom events with their parameters and its functions with their inputs and outputs. Then typed sends, remote
  variables and calls resolve, even between graphs that refer to each other in cycles.
- Then one `graph_edit` per region: its nodes and links, `{ "op": "Arrange", "scope": "Created" }`, an Arrange of the
  region's nodes (`"scope": "Nodes"`), and an `AddComment` with `text`, `elements` and `color` that frames them, with
  `refuseNewErrors: true`. Require `summary.errors` and `summary.warnings` to be 0 before going on.
- One region per behavior, its comment box titled "<What>: <how>" or a short noun phrase in its role's color (blue
  events and input, orange rules, purple effects, green the title, red development only). Regions stack from top to
  bottom and share no nodes or links; inside a region the nodes run from left to right.
- Verify at the end: `graph_check` of each graph's document finds no diagnostic, `graph_arrange` with `dryRun: true`
  per box changes nothing, and `graph_interface` of each library lists its callers in `usedBy`.

`graph_arrange` lays a graph out as the graph window's Arrange does, with node sizes as a window draws them, and its
`bounds` is where a comment box framing the nodes goes.

### Watching and debugging

The visual-script debugger attaches while the game plays (Play, never Simulate or `test_run`; the player and exported
games never stop at breakpoints). Its commands work headless and record no undo step.

| Step | Tools |
|------|-------|
| Make the run exact | `play_start` with `paused: true` creates the scene and pauses it before its first frame; `play_advance` with `frames` runs exactly those frames (1/60 s each unless `timestep` says otherwise), so every value read is reproducible |
| Pick the instance | `graph_target` with `graph` and `entity` makes the graph's breakpoints stop that entity alone (`any: true` for every one; `graph` alone lists the instances) |
| Set breakpoints | `graph_breakpoints` with `graph` and `add: [{ node, condition, hitCount }]`; a condition is a formula over the names the node reads (`Health < 0`, `State == "Serve"`). `breakOnErrors: true` stops at the next error. Read `checked`, `error` and `inactiveReason` in the result |
| Run until one stops | `play_advance` stops early after the frame in which an execution stopped (`stopped`, `stop`); `graph_wait` waits in real time while the game runs |
| Read the stop | `graph_debug` gives the state and each stop's call stack; `graph_values` with `graph` and `stop` reads the stopped frame's values (each with a `currency`), and `evaluate` computes formulas there. Without `stop` it reads the live instance |
| Step | `graph_step` with `kind` `Into`, `Over` or `Out`; a step that waits in a latent node answers `Waiting`, with `needsFrames` while the scene stays paused (run `play_advance`). `graph_continue` continues (`to` runs to a node), `graph_break` stops the next node that runs |
| Fix it live | `graph_edit` while playing changes running graphs in its own call, also an execution stopped at a breakpoint when the edit keeps the stopped node; its result's `live.edits` receipts say `Applied`, `Pending`, `KeptLastWorking` (the edit has errors: the game keeps the last version that worked) or `NotRunning` |
| Verify | `graph_trace` with `frames`, `graphs` and `nodes` runs frames and summarizes which exits each node took and each pin's first, last, minimum and maximum; `graph_live` lists what edits did to the running game and the runtime problems; `graph_instances` lists the instances with their waits and stops |
| Hand the editor back | `graph_breakpoints` with `clear: true`, then `play_stop`. Edits made while playing stay, and stay undoable |
| Show the user | `graph_open` with `show` brings a node into view, `graph_debug_panel` opens the Visual Script Debugger on a tab, a stop and a frame, and `viewport_screenshot` captures a window |

While an execution is stopped, `play_step`, `play_advance` and `graph_trace` are refused: continue or step first.
`log_read` shows graphs that do not load, entities left without their graph and errors while running.

### The documented debugging session

`Tests/Data/VisualScripting/Agents/Debugging.json` is a session that the editor's tests replay, headless and through
`strada mcp`, and that Automation.md, VisualScripting.md and the skill show step by step. In 22 calls it finds and fixes
a bug in a graph that should turn an object but only shakes it:

1. It creates the Spinner graph from the Events template and writes it with one `graph_edit` batch, with a mistake:
   its formula wraps the angle at 36 degrees (`% 36`) instead of 360. It creates an entity that runs the graph.
2. It starts play paused and sets a breakpoint on node 8, the Set Angle node, with the condition `Angle > 33.5`.
3. It runs up to 60 frames with `play_advance`; the run stops after 24 frames at the breakpoint. `graph_debug` shows
   the stop and its call stack, and `graph_values` reads `Speed` 90 and `Angle` 34.5.
4. It steps over the node with `graph_step` and reads the values again: the angle is now 0. It wrapped at 36, which is
   the bug.
5. While the execution is stopped, it fixes the formula with one `SetFormula` operation. The receipt says the running
   instance took the edit (`Applied`, one stop kept), and `graph_debug` shows the stopped execution migrated to the
   edited graph.
6. It clears the breakpoints, continues, runs 30 more frames, and traces 10 frames of node 8: it ran 10 times, its
   value climbing from 46.5 to 60, and `graph_values` reads an angle of 60.
7. It stops play, then undoes and redoes the fix with `editor_undo` and `editor_redo`, reading the graph after each to
   see the formula switch between `% 36` and `% 360`.

strada's own test of the session then saves the scene, exports the game and runs it headless: the exported graph holds
the fixed formula and runs without a problem.

## How the samples were made

The sample projects were authored by Python scripts that drive the editor through `strada mcp`, exactly as an agent
does. Each script starts its own headless editor (`strada mcp --headless --new`), so it never touches an editor you have
open. Without an argument it runs strada from the repository (`dotnet run`); pass a `strada` executable to use that one.

| Script | What it builds |
|--------|----------------|
| `python Projects/PrismBreak/Tools/Author.py` | Prism Break, a brick breaker of 13 visual scripts and no C#: the project and its settings, the sounds, mesh and textures that `Tools/GenerateAssets.py` makes, the materials, then the graphs (the Arcade and Checks function libraries first), the prefabs, the main and test scenes, checks of every graph, the test run, the title's attract mode played for 600 frames, and an exported game's smoke test |
| `python Projects/Showcase/Tools/Author.py` | The Showcase: Poly Haven assets (sky, ground, texture sets, props), the generated sounds, meshes and textures, the C# scripts, materials, prefabs, the main scene with its exhibits and the test scene; then the tests |
| `python Projects/Showcase/Tools/Author.py --exhibit GraphGarden` | The Showcase's Graph Garden alone, in place: its entities in the main and test scenes, its graphs and materials (keeping their asset handles), then the tests |
| `python Projects/Blocks/Tools/Author.py` | Blocks: the project and its settings, the C# scripts, the generated sounds, block mesh, grid and screen images, a dusk sky from Poly Haven, materials, the debris and particle burst prefabs, the game scene with its screen (a UI canvas) and the test scene; then the tests |

What they show:

- **Prism Break** is the reference for graphs made by an agent. Its script declares first (one batch per graph adds
  its variables, custom events and functions), then writes one `graph_edit` batch per region: the region's nodes and
  links, arranged (scope `Created`, then `Nodes`) and framed by a titled comment box, and every batch must answer no
  error and no warning. Two runs give the same graphs. Its graphs call function libraries, send typed events and
  broadcast to each other, read each other's variables guarded by `RunsGraph`, and its 29 test checks are graphs too.
- **The Showcase** builds a large scene exhibit by exhibit from Poly Haven models, texture sets and a sky, with C#
  scripts, prefabs, particles, sound, post-processing and a HUD. Its Graph Garden (`Tools/GraphGarden.py`) is six
  visual scripts authored with the same rules as Prism Break's.
- **Blocks** keeps its rules in plain C# classes that tests can check, builds its HUD and menus on a UI canvas with
  `entity_create` (its script shows the helpers), and has a test scene whose script plays the game and clicks its
  buttons.

The full scripts rebuild their project from scratch: everything but the `Tools` folder (and, for Blocks and the
Showcase, the scripts' sources in `Scripts/Source`; for the Showcase also its `.gitignore`) is replaced. A full
Showcase rebuild imports the Poly Haven assets again with new handles and rewrites the materials made from them, so
rebuild the Graph Garden with `--exhibit GraphGarden` instead. That run compares every file under `Assets` before and
after, and fails, naming the files, when anything outside the exhibit's own files changed. After a change to arranging,
`SampleGraphsTests` names the command to run again: `Projects/PrismBreak/Tools/Author.py`, or the Showcase's
`--exhibit GraphGarden`.

## Troubleshooting

### strada cannot find or reach an editor

| Message | What to do |
|---------|------------|
| `no StradaEditor found: pass --editor <path>, set STRADA_EDITOR, or build the engine (python Tools/build.py)` (with `.exe` on Windows) | Build the engine, or point strada at an editor with `--editor` or `STRADA_EDITOR` |
| `--editor: '<path>' does not exist`, `STRADA_EDITOR names '<path>', which does not exist` | Fix the path |
| `no editor is running: start one with strada launch`, `no editor is running to import into: start one with strada launch, or pass --output` | `strada call` and the Poly Haven imports never start an editor: start one (`strada launch`, or the editor itself) |
| `no running editor has '<file>' open: start one with strada launch --project <file>` | Open the project in an editor, or start one with it |
| `no editor with process ID <id> is running` | Check the ID with `strada instances` |
| `the editor could not open the project '<file>': <reason>` | The editor started but refused the project; the reason comes from its log |

`strada instances` lists the editors strada can see. Instance files of editors that no longer run are removed by the
next scan.

### A stale editor build

- strada uses a running editor before it starts one. An editor that was running before you rebuilt the engine keeps
  the old build: close it (or pass `--new` to `strada mcp`) to get the new one.
- Without `--editor` or `STRADA_EDITOR`, strada in the repository starts the most recently built
  `build/*/bin/StradaEditor`. With several presets built (Debug and Release), that is the one built last: set
  `STRADA_EDITOR` to pin one.
- `strada mcp` reads the tool list once per session and does not announce changes to it. After a rebuild that adds or
  changes commands, restart the MCP server (start a new session of your MCP client) to see them.
- A `MethodNotFound` error for a command that Automation.md documents means the editor is older than the docs: rebuild
  it with `python Tools/build.py`.

### Commands that fail

A command that fails is a tool result with `isError` and the error as text, such as
`EntityNotFound (1001): entity 123 does not exist`, with `data` on a second line. `data.path` names the offending
parameter of a validation error.

| Code | Name | Meaning | What to do |
|------|------|---------|------------|
| -32601 | MethodNotFound | Unknown command | See [A stale editor build](#a-stale-editor-build) |
| -32602 | InvalidParams | The parameters violate the schema, or field values are invalid | Fix the parameter `data.path` names; `component_types` and `graph_library` list what is valid |
| -32603 | InternalError | Unexpected failure inside the editor | An editor bug: report it with the call |
| -32001, -32002 | Unauthenticated, ServerBusy | Wrong token, or more than 8 connections | strada authenticates for you; close other clients |
| 1001 | EntityNotFound | An entity parameter names no entity | `scene_hierarchy` or `entity_find` for current IDs |
| 1002 | ComponentNotFound | Unknown component type, or the entity lacks the component | `component_types`, `entity_get` |
| 1003 | InvalidOperation | Not possible in the current state (nothing to undo, a hierarchy cycle, a core component, a file command while playing, a stale graph revision) | Read the message; `play_stop` before scene and project file commands |
| 1004 | FileError | Reading or writing a file failed | Check the path and the file's permissions; `log_read` |
| 1005 | Unavailable | Not available in this configuration (screenshots, menus and graph windows when headless; `graph_library` and `graph_types` without the scripting runtime) | Use a windowed editor, or install .NET 10 |
| 1006 | Cancelled | An asynchronous command was abandoned | Run it again |
| 1007 | UnsavedChanges | The command would discard unsaved changes | `scene_save` first, or pass `"discardChanges": true` when losing them is intended |
| 1008 | AssetNotFound | An asset parameter names no registered asset | `asset_list`, or `asset_refresh` after files changed on disk |
| 1010 | ConsentRequired | The command needs an explicit decision of the user (`project_setup_agents` without `confirmed`) | Ask the user; pass `"confirmed": true` only when they agree (`"dryRun": true` shows what would change) |

### Reading the log

`log_read` returns the editor's log entries (`since`, `maxCount`, `minLevel` from `trace` to `critical`) with `next` to
pass as `since` on the next call. The editor explains failures there: script build errors with their file and line,
graphs that do not load (`Visual script '<path>' has 1 error; first: ...`), entities left without their graph
(`Entity '<name>' runs no visual script: ...`), and errors while graphs run. The editor also writes its log to
`<user data>/Logs/StradaEditor.log`, and the Console panel shows it in the editor.
