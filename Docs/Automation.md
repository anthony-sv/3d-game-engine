# Strada Editor Automation

The editor is fully controllable by programs and AI agents through a local JSON-RPC 2.0 endpoint. Every command runs
on the editor's main thread through the same operations and undo history as the UI, so an agent and a human see and
edit exactly the same state. `editor.commands` returns every command with its JSON-schema parameters at runtime;
this document is the reference (a test fails when a registered command is missing here).

## Connecting

1. Start the editor (`StradaEditor`, or `StradaEditor --headless` without a window). It listens on `127.0.0.1` only,
   on a free port (`--automation-port <port>` fixes it; `--no-automation` disables the server).
2. Read the instance file `<user data>/Editor/Instances/<pid>.json` (`%APPDATA%/Strada` on Windows,
   `~/Library/Application Support/Strada` on macOS, `$XDG_DATA_HOME/strada` or `~/.local/share/strada` on Linux):

   ```json
   { "Strada": { "Version": 1, "Type": "AutomationInstance" }, "ProcessID": 1234, "Port": 51234,
     "Token": "<64 hex characters>", "Project": "", "Version": "0.1.0" }
   ```

   `Project` is the project file the editor has open (empty without one); the editor rewrites the file when that
   changes, until the file names it: a write that fails (on Windows the replace fails while another process has the
   file open) is retried at most every 100 ms, always with the newest project. The file holds a secret: its directory
   is user-only (POSIX 0700, files 0600; on Windows the per-user application data folder is private by its default
   ACL). The editor deletes it on exit; files of processes that no longer run are stale and removed by scanners.
3. Open a TCP connection and authenticate first:

   ```json
   {"jsonrpc": "2.0", "id": 1, "method": "authenticate", "params": {"token": "<token>"}}
   ```

   Anything else before a successful `authenticate`, or a wrong token, is answered with `Unauthenticated` and the
   connection is closed.

## The strada tool and MCP

`strada` (`StradaTool`, C#; CMake builds it next to `StradaEditor`) speaks this protocol for agents and scripts:

| Command | What it does |
|---------|--------------|
| `strada mcp` | A Model Context Protocol server on standard input and output (below) |
| `strada call <command> [params]` | Runs a command in a running editor and prints its result as JSON; `params` is a JSON object. Exit code 1 with the error when the command fails |
| `strada commands` | Lists the editor's commands (`--json`: as `editor.commands` describes them) |
| `strada launch` | Starts an editor, waits for its automation server and prints `{ processId, port, project, version }` |
| `strada instances` | Lists the running editors (`--json` for JSON) |
| `strada hdris [words]` | Searches Poly Haven's HDRIs (CC0) by words in their names, categories and tags (`--json` for JSON) |
| `strada hdri <id>` | Downloads a Poly Haven HDRI (`--resolution` 1k, 2k (default), 4k or 8k) and imports it into the running editor's project (`--folder` in Assets, default `Environments`), or only downloads it into `--output <directory>` |
| `strada models [words]`, `strada textures [words]` | Search Poly Haven's models and PBR texture sets the same way |
| `strada model <id>` | Downloads a Poly Haven model (glTF with its buffer and textures; `--resolution` of the textures, default 1k) into `<folder>/<id>` of the running editor's project (`--folder`, default `Models`) and registers it, or only downloads it into `--output <directory>` |
| `strada texture <id>` | Downloads a Poly Haven texture set (color, normal, ambient occlusion/roughness/metallic maps) into `<folder>/<id>` (default `Textures`) and creates the material `<id>.smat` from it, or only downloads it into `--output <directory>` |

Options: `--editor <path>` (the editor to start), `--project <file>` (use the editor that has this project open; editors
strada starts open it), `--pid <id>` (only the running editor with this process ID), `--headless` (start editors without
a window), `--new` (`strada mcp`: never use an editor strada did not start). strada starts the editor that `--editor`
names, else `STRADA_EDITOR`, else the `StradaEditor` next to strada, else the most recently built
`build/*/bin/StradaEditor` below the working directory (an engine checkout). Exit codes: 0 success, 1 failure, 2 wrong
usage.

`strada mcp` serves the editor's commands as MCP tools (protocol versions 2024-11-05 to 2025-11-25):

- Tool names are the command names with `.` and `-` replaced by `_` (`entity_create`, `asset_create_folder`). The
  descriptions and JSON-schema parameters are the editor's (`editor.commands`); read-only commands have `readOnlyHint`.
- A tool call runs its command on the editor strada works with: a running editor (the one that has `--project` open,
  else the newest instance file), or one it starts when none fits. Results are JSON text; results with an image
  (`mimeType` `image/...` and base64 `data`, such as `viewport.screenshot`'s) are image content. Commands that fail are
  tool results with `isError` and the error as text (`EntityNotFound (1001): entity 123 does not exist`, `data` on a
  second line), which the model reads; unknown tools are protocol errors.
- `tools/list` needs no running editor: strada then reads the commands from a headless editor it starts for that and
  closes again.
- Requests run concurrently. A connection that broke (the editor closed) is replaced on the next call. Cancelled
  requests (`notifications/cancelled`) get no response; the editor still finishes the command.
- strada adds tools of its own after the editor's: `polyhaven_search_hdris` (`query`, `limit`) finds Poly Haven HDRIs,
  and `polyhaven_import_hdri` (`id`, `resolution`, `folder`) downloads one and imports it into the open project;
  `polyhaven_search_models`/`polyhaven_import_model` and `polyhaven_search_textures`/`polyhaven_import_texture` do the
  same for models (the result's `mesh` goes into a MeshComponent) and texture sets (the result's `material`); a
  SkyLight's `Environment` takes the returned asset. Downloads are checked against the size and MD5 that Poly Haven
  lists; nothing is downloaded when no project is open.
- Editors strada starts with a window stay open for the user. Editors it starts headless close with `strada mcp`: they
  get `--parent-process <strada's process ID>`, the editor option that closes the editor, discarding unsaved changes,
  once that process has exited (a safeguard for when strada itself is ended without closing them).

The repository's `.mcp.json` registers `strada mcp` for agents working in the checkout (through `dotnet run`, which
builds strada when needed). A game project elsewhere registers the built tool (`strada.exe` on Windows):

```json
{
  "mcpServers": {
    "strada": { "command": "<engine>/build/<preset>/bin/strada", "args": ["mcp", "--project", "Game.sproj"] }
  }
}
```

## Messages

- One compact JSON document per line (`\n`, an optional `\r` is ignored), UTF-8.
- Requests use named parameters (`"params"` is an object or omitted). Batches (arrays of up to 64 requests) are
  answered with one array; notifications (no `"id"`) get no response.
- Lines longer than 16 MiB close the connection (`MessageTooLarge`); nesting deeper than 64 levels is rejected.
- Up to 8 connections; further ones receive `ServerBusy` and are closed.
- A connection that has not authenticated within 10 seconds receives `Unauthenticated` ("authentication timed out")
  and is closed.
- Entity references are UUIDs written as decimal strings. Component data uses the scene-file format: asset fields take
  handles or `"builtin://<Name>"` / `"asset://<path>"`, rotations are quaternions `[x, y, z, w]` (Transform also
  accepts `"RotationEuler": [x, y, z]` in degrees).
- Asynchronous commands (such as `viewport.screenshot`) answer when they complete; responses can arrive out of request
  order, matched by `"id"`.

## Errors

`{"jsonrpc": "2.0", "id": ..., "error": {"code": <int>, "message": "<explanation>", "data": {...}}}`. `data.path` names
the offending parameter for validation errors.

| Code | Name | Meaning |
|------|------|---------|
| -32700 | ParseError | The line is not valid JSON |
| -32600 | InvalidRequest | Not a valid JSON-RPC request (or an empty/oversized batch) |
| -32601 | MethodNotFound | Unknown command |
| -32602 | InvalidParams | Parameters violate the schema, or field values are invalid |
| -32603 | InternalError | Unexpected failure inside the editor |
| -32001 | Unauthenticated | Not authenticated, or wrong token |
| -32002 | ServerBusy | Too many connections |
| -32003 | MessageTooLarge | Message exceeds the size limit |
| 1001 | EntityNotFound | An entity parameter names no entity |
| 1002 | ComponentNotFound | Unknown component type, or the entity lacks the component |
| 1003 | InvalidOperation | Not possible in the current state (nothing to undo, hierarchy cycle, core component, ...) |
| 1004 | FileError | Reading or writing a file failed |
| 1005 | Unavailable | Not available in this configuration (screenshots when headless) |
| 1006 | Cancelled | An asynchronous command was abandoned |
| 1007 | UnsavedChanges | The command would discard unsaved changes (pass `"discardChanges": true`) |
| 1008 | AssetNotFound | An asset parameter names no registered asset |

## Commands

Parameters marked * are required.

### editor

| Command | Parameters | Result |
|---------|-----------|--------|
| `editor.status` | — | `version`, `project` { name, file, assetDirectory } (null without a project), `scene` { name, path, dirty, entityCount }, `undo`/`redo` (step descriptions or null), `selection` |
| `editor.commands` | — | `[{ name, description, params (JSON schema), readOnly, async }]` |
| `editor.undo` | — | `undone` (description), `scene` |
| `editor.redo` | — | `redone`, `scene` |
| `editor.quit` | `discardChanges` | `quitting`; fails with UnsavedChanges when the scene is dirty |
| `editor.entity-menu` | `submenu` (`3D Object`, `2D Object`, `Rendering`, `Effects`, `Physics`, `Audio` or `UI`), `open` (default true; false closes the menu) | Opens the menu bar's Entity menu, which creates entities from presets (the hierarchy's create menus are the same), as if the user clicked Entity and pointed at `submenu`; returns `open`, `submenu`, `sections`: `[{ category, submenu, presets }]`, the menu in its order (Core's Create Empty in the menu itself, then a submenu per category with the labels of its presets). Changes nothing in the scene (`entity.create` creates entities); InvalidOperation while a dialog is open; Unavailable when headless |

### project

Project settings are saved to the project file immediately and are not part of the undo history.

| Command | Parameters | Result |
|---------|-----------|--------|
| `project.info` | — | `project` { name, file, assetDirectory } (null when none is open), `settings` (the `.sproj` "Project" object) |
| `project.create` | `directory`* (new or empty), `name`*, `discardChanges` | `project`, `scene` (the start scene `Assets/Scenes/Main.sscene`) |
| `project.open` | `path`* (`.sproj` file), `discardChanges` | `project`, `scene` (its start scene), `warnings` (skipped unknown settings, start scene problems) |
| `project.close` | `discardChanges` | `closed`, `scene` (empty); InvalidOperation without a project |
| `project.settings` | `settings` (partial patch, e.g. `{"Window": {"Width": 1920}, "Physics": {"Layers": ["Default", "Player"]}}`) | `settings`; InvalidOperation without a project |
| `project.export` | `directory` (new or empty directory, or an earlier export, which is replaced; absolute or relative to the project directory; default `Build`), `smokeTest` (runs the exported game headless for a few frames; default false) | `succeeded`, `error`, `directory`, `executable` (on macOS the application bundle, whose program is `Contents/MacOS/<name>`), `unsavedChanges` (the game uses the saved files), `seconds`, `bytes` and `files` (what the export wrote), `smokeTest` (`{ passed, output }`: whether the game started and exited cleanly, the end of its output; null without one), `scripts`: `{ built, errors, warnings, diagnostics }`; asynchronous. Scripts that do not compile end it with `succeeded` false and leave the earlier export. InvalidOperation without a project, while playing or exporting, or without a start scene; FileError when the game cannot be written; Cancelled when cancelled |

### scene

| Command | Parameters | Result |
|---------|-----------|--------|
| `scene.new` | `name`, `discardChanges` | `scene`; the history keeps its steps of asset files (`graph.set`, `material.set`) |
| `scene.open` | `path`* (absolute, or relative to Assets), `discardChanges` | `scene`, `warnings` (skipped unknown components/fields); the history keeps its steps of asset files (`graph.set`, `material.set`) |
| `scene.save` | `path` (absolute, or relative to Assets; required for never-saved scenes) | `scene` |
| `scene.hierarchy` | — | `scene`, `entities`: tree of `{ id, name, components, children }` |
| `scene.dump` | — | the scene in the `.sscene` format |
| `scene.settings` | `name`, `settings` (partial patch, e.g. `{"Physics": {"Gravity": [0, -9.81, 0]}}`) | `name`, `settings` |

### entity

| Command | Parameters | Result |
|---------|-----------|--------|
| `entity.create` | `name`, `parent`, `siblingIndex`, `components` (`{ "<Component>": { fields } }`) | `id` |
| `entity.delete` | `entities`* (deletes descendants too, one undo step) | `deleted` |
| `entity.duplicate` | `entities`* | `ids` of the copies |
| `entity.rename` | `entity`*, `name`* | `id`, `name` |
| `entity.reparent` | `entity`*, `parent` (null = root), `siblingIndex`, `keepWorldTransform` (default true) | `id`, `parent` |
| `entity.find` | `name` (exact), `component` | `entities`: `[{ id, name, components }]` in hierarchy order |
| `entity.get` | `entity`* | `id`, `name`, `parent`, `children`, `components` (all fields) |
| `entity.select` | `entities`*, `mode` (`replace`/`add`/`remove`/`toggle`), `primary` | the selection `{ entities, primary, asset }` (not undoable; replacing it or selecting entities stops showing an asset) |

### component

| Command | Parameters | Result |
|---------|-----------|--------|
| `component.types` | — | `components`: `[{ Name, Description, Fields: [{ Name, Type, Default, ... }], Core, Category }]` (see below) |
| `component.add` | `entity`*, `component`*, `fields` | `component`, `fields` |
| `component.remove` | `entity`*, `component`* (core components cannot be removed) | `removed` |
| `component.get` | `entity`*, `component`* | `component`, `fields` |
| `component.set` | `entity`*, `component`*, `fields`* (partial patch; invalid values change nothing) | `component`, `fields` |

While playing, a `Script` component's `Fields` are the values its running script holds (`entity.get` and
`component.get` read them from the script), and `component.set` sets the given ones in the script at once, also while
paused; undo gives the script back the values it had. The `Fields` object replaces the stored ones, so values the patch
leaves out stay the script's own. A `VisualScript` component's `Variables` (the values the entity gives the public
variables of its graph, by name) work the same way with its running graph: a `Variables` patch replaces the whole map,
so variables it leaves out have no stored value (the graph gives them their own) and, while playing, keep the values the
graph holds. While the running graph has not taken an edit of its document yet (a step of the visual-script debugger
that runs in the old version holds the swap: the receipt is `Pending`), the read values are under the names of the
variables it runs, and a value set for a variable only the edit has (renamed or added) reaches it as the graph takes the
edit. A `Graph` that is a function library is taken as any
graph, but runs on no entity: playing logs `Entity '<name>' runs no visual script: <path> is a function library: graphs
call its functions; it runs on no entity`.

`Core` components are on every entity and cannot be added or removed. `Category` says what a component is for: `Core`,
`Rendering`, `Physics`, `Audio`, `Scripting` or `UI` (the inspector's Add Component menu groups components by it).
Field descriptions may also contain `Values` (the names of enum values), `Min`/`Max` (inclusive bounds of numbers and
of every vector component; values outside them are rejected), `Display` (`Color`, `Angle` in degrees or
`MultilineText`), `AssetType` (the type of asset an asset field references), `Description`, `VisibleWhen`
(`{ Field, Values }`: the field only matters, and editors only show it, while that field of the same object has one of
those values, such as a material's `AlphaCutoff` in the `Mask` alpha mode or a joint's `Axis` for the `Hinge` and
`Slider` types; values are enumerator names, or `true`/`false` for bool fields), `EnabledBy` (the bool field that
switches the field on: a `PostProcess` volume's `Saturation` applies only while its `OverrideSaturation` is true, so set
both) and `HiddenInEditors` (`true` for fields the editor edits with UI of its own instead of a row of the generic
inspector, such as `Mesh.Materials`, which the material slot sections edit; set them like any other field).

### inspector

| Command | Parameters | Result |
|---------|-----------|--------|
| `inspector.add-component-menu` | `search` (text typed into the menu's search field), `category` (without `search`: `Rendering`, `Physics`, `Audio`, `Scripting` or `UI`, the category whose submenu opens), `open` (default true; false closes the menu) | Brings the inspector to the front with its Add Component menu open for the selected entities, as if the user clicked Add Component and typed `search` (without one the menu shows the categories, and `category` opens one of their submenus as if the user pointed at it); returns `open`, `search`, `category`, `components` (the components the menu offers, in its order: those of the open submenu with a `category`). Changes nothing in the scene; InvalidParams for `search` with `category`; InvalidOperation without selected entities, for a category the menu offers nothing of (the entities have its components) and while a dialog is open; Unavailable when headless |

### asset

Asset parameters accept a handle (decimal string), `"asset://<path in Assets>"` or `"builtin://<name>"`. Paths are
relative to the project's `Assets` directory with forward slashes. Asset file operations need an open project
(InvalidOperation otherwise) and are not part of the undo history; handles stay valid when assets move. Deleting an
asset (`asset.delete`, `asset.delete-folder`) discards the undo steps that edited its file; a step that also changed the
scene keeps its scene part. `project.create`, `project.open` and `project.close` drop every step of asset files.

| Command | Parameters | Result |
|---------|-----------|--------|
| `asset.list` | `folder`, `type` (`Scene`/`Prefab`/`Mesh`/`Material`/`Texture`/`Environment`/`AudioClip`/`Font`/`VisualScript`), `recursive` (default true), `builtIn` | `assets`: `[{ id, name, type, path, reference, missing }]`, `folders` |
| `asset.get` | `asset`* | `{ id, name, type, path, reference, missing }` |
| `asset.select` | `asset`* | the selection: the inspector shows the asset instead of entities (not undoable) |
| `asset.import` | `files`* (absolute paths), `folder` | `assets` (all or nothing; taken names get a number) |
| `asset.refresh` | — | `added`, `missing`, `modified` (assets), `warnings` |
| `asset.browse` | `root` (`Assets` or `Scripts`: the project's C# sources in `Scripts/Source`), `folder` (relative to the root, `""` for the root itself), `search` (text the root's paths are searched for), `tileScale` (0.6 to 2.5); all optional | Shows the folder in the content browser, or with `search` the root's files whose paths contain the text (ignoring case), brings the browser to the front and sizes its tiles; without `root` the displayed root stays, without `folder` the root's displayed folder, without `search` a search ends; returns `root`, `folder`, `search`, `tileScale`; changes nothing in the project; InvalidParams for folders outside the root's directory; Unavailable when headless |
| `asset.drop` | `paths`* (absolute paths of files and folders), `root` (`Assets` or `Scripts`), `folder` (of the root, relative to it); by default the displayed root and folder | Imports them as if they were dropped onto the content browser from the OS: assets into the asset folder (taken names get a number), models with the files they read (glTF buffers and images, OBJ material libraries and their textures), C# sources into the script folder unless their file name or a type they declare is taken in the scripts, folders whole (their C# sources to the scripts, the rest to the assets), other files only with a model; the browser selects what came in and its notice says what did not. Returns `assets`, `scripts` (relative to the project directory), `folders` (made in Assets), `scriptFolders` (made in `Scripts/Source`), `keptFiles` (copied files that are not assets), `skipped`, `notes`, `errors` (one failure does not stop the others); not undoable; InvalidParams for relative paths and folders outside the root's directory; Unavailable when headless |
| `asset.move` | `asset`*, `path`* | the moved asset; a model moving to another folder takes the files it reads along (refused when one outside its folder would no longer be found or another model reads one) |
| `asset.delete` | `asset`* | `deleted` (id) |
| `asset.create-folder` | `folder`* | `folder` |
| `asset.move-folder` | `folder`*, `newFolder`* | `folder` |
| `asset.delete-folder` | `folder`* (deletes every file inside) | `deleted` |

### material

Material fields are those of `.smat` files (`BaseColor`, `Metallic`, `Roughness`, `EmissiveColor`, ...,
`BaseColorTexture` and the other textures as asset references, `AlphaMode`, `UVTiling`, ...).

| Command | Parameters | Result |
|---------|-----------|--------|
| `material.create` | `path`* (`.smat`, relative to Assets), `fields` | `material` (asset), `fields` |
| `material.get` | `material`* | `material`, `fields` |
| `material.set` | `material`*, `fields`* (partial patch; undoable without marking the scene modified; built-in and mesh-embedded materials are read-only) | `material`, `fields` |

### graph

Visual scripts are graph assets (`.sgraph` files) that a `VisualScript` component runs on its entity: its `Graph` is the
asset, its `Variables` the values the entity gives the graph's public variables. The graph commands read and write a
graph's **document**, the content of its file without the header: `{ "Kind"?, "Description", "Variables", "Functions",
"Graph" }` (Docs/Architecture.md section 7.3; every save writes version 3 of the format, and files of versions 1 and 2
load unchanged, as scripts). `Kind` is `"Script"` (the default, left out) or `"Library"`: a **function library** holds
functions that other graphs call through a `Function.Call` whose `Graph` setting names it; it has no variables and no
event graph (comment boxes aside) and runs on no entity. Variables, function locals, functions, nodes and comments share
one space of IDs (1 to 2147483647). A function's inputs and outputs are `{ "Name", "Type", "FormerNames"? }`:
`FormerNames` are the parameter's earlier names, newest last, at most 16, which a commit records when a library's
parameter is renamed in place (below), so calls in other scripts that still name the old pin keep working and store the
new name at their next save. A node is `{ "ID", "Type", "Position"?, "Settings"?, "Inputs"?, "Split"?, "Comment"?, "Collapsed"?,
"Enabled"? }`: `Type` is a node kind of `graph.library` (`Flow.Branch`, `Call:Strada.Input.IsKeyDown(Strada.KeyCode)`),
a node without `Position` is laid out by `graph.set`, `Settings` configure the kind and `Inputs` hold the literals of
unconnected inputs. The other four are those of version 2 of the format, each left out at its default: `Split`
(`{ "Inputs": [...], "Outputs": [...] }`) shows each listed pin as one pin per field (`"Hit"` gives `Hit.Point`,
`Hit.Normal`, ...; `"Hit.Point"` after it splits that field again; at most 64 per list), and links and `Inputs` then
name the field pins; `Comment` is a note on the node (at most 65536 bytes); `Collapsed` (true) shows only its header;
`Enabled` is `"Enabled"` (the default), `"Disabled"` (the node passes the flow and its values through instead of
running: below) or `"DevelopmentOnly"` (it runs in the editor and in `StradaRuntime --project`, and is disabled in
exported games). A link is `{ "From": [<node>, "<output pin>"], "To": [<node>, "<input pin>"] }`, within one graph (the
event graph, or one function's). Documents store no pins: the scripting runtime derives them (`graph.library` lists
each kind's, an analysis each node's). Six kinds of node may name another visual script by its handle in a `Graph`
setting: `Event.Send` and `Event.Broadcast` (typed: checked against that script's custom event), `Function.Call` (a
library's function), `Variable.Get` and `Variable.Set` (a public variable of another entity's graph) and `RunsGraph`;
`graph.interface` tells which IDs, names and events a graph offers them.

| Command | Parameters | Result |
|---------|-----------|--------|
| `graph.create` | `path`* (`.sgraph`, relative to Assets), `template` (`Events` (default): an `Event.Create` and an `Event.Update` node; `Empty`; `Library`: an empty function library, whose functions other graphs call through a `Function.Call` whose `Graph` names it; it runs on no entity) | `asset` (the graph's handle), `path`; not undoable (file creation); InvalidParams for a path that is taken, outside Assets or not `.sgraph` |
| `graph.get` | `graph`* (handle or `"asset://<path>.sgraph"`), `analyze` (default true) | `asset`, `path`, `document`, `revision` and `programRevision` (below: `graph.set`, `graph.edit` and `graph.arrange` take them to refuse an edit when the graph changed since), `analyzed`, `nodes` and `diagnostics` (the analysis, below); FileError when the file does not load |
| `graph.set` | `graph`*, `document`* (strict), `revision` and `programRevision` (as `graph.get` returns them: each refuses the call with InvalidOperation, `data: { path, <parameter>: <current> }`, writing nothing, unless the stored document still has it; a file that does not load has none, `null`), `refuseBrokenReferrers` (default false: nothing is written when the document breaks nodes of other scripts, InvalidOperation with `data.referrers`; replacing a file that does not load reports no referrers and refuses none, since the nodes naming it were broken already, so `graph.set` repairs a broken graph even with it) | `asset`, `path`, `changed` (false when the document is the stored one, as written: no undo step; an integer written as a float, `1` as `1.0`, is a change), `revision` and `programRevision` (of the document written), `analyzed`, `warnings` (what `graph.set` changed besides the given document), `summary` (`{ brokenReferrers, staleSenders }`), `referrers` (the nodes of other scripts the document breaks, below), `live` (what the running game took: below), `nodes`, `diagnostics`; saves the file as one undo step that does not mark the scene modified, whose undo gives the file its exact bytes back (a version-1 file stays version 1); allowed while playing (below) |
| `graph.check` | `document`*, `graph` (the graph the document is for: renamed variables are found against its stored document, and nodes whose `Graph` setting names it are checked against the document itself; pass it when the document refers to its own graph) | `valid`, `issues` (`[{ path, message }]`: what `graph.set` would refuse), `warnings` (what it would change), `analyzed`, `nodes`, `diagnostics`; writes nothing |
| `graph.edit` | `graph`*, `operations`* (0 to 10000 operations `{ "op": "<Kind>", ...fields }`, applied in order to the stored document, each the graph window's own edit: below), `function` (the sub-graph AddNode, AddComment, Paste and Arrange use when they name none: 0, the default, for the event graph, else a function's ID or name), `description` (1 to 128 code points, default "Edit Visual Script"), `revision` and `programRevision` (as for `graph.set`; `programRevision` ignores moves, comments and arranging, so a user working in the same graph does not refuse the call), `dryRun` (default false: everything but the write and the undo step), `refuseNewErrors` (default false: nothing is written when the result has an Error the stored document does not have; InvalidOperation with `data.diagnostics`), `refuseBrokenReferrers` (default false: as for `graph.set`; a dry run refuses as the call would), `analyze` (default true; operations that need pins analyze anyway), `nodes` (`None`, `Changed` (default: the nodes the batch created or whose program it changed, not those it only moved) or `All`), `document` (default false: the resulting document in the content form) | `asset`, `path`, `changed`, `dryRun`, `revision` and `programRevision` (of the document written, or that a dry run would write), `analyses`, `labels`, `results`, `warnings`, `analyzed`, `summary`, `referrers`, `diagnostics`, `nodes`, `hints`, `document`, `live` (below); one undo step "<description> in <script>", allowed while playing; an open window shows it as an outside change |
| `graph.arrange` | `graph`*, `function` (0, the default: the event graph; else a function's ID or name), `nodes` (IDs of nodes of that sub-graph, at most 10000; absent or empty: every node), `extents` (`Measured` (default): node sizes as a graph window draws them, measured without one, from the analysis with the scripting runtime and from the pins links and literals name without it: heights equal a window's, widths are at most about 6 % wider; `Uniform`: 256 × 128 for every node, the pitches of `graph.set`'s layout, where tall nodes such as a raycast overlap), `revision` (the call fails with InvalidOperation, `data: { path: "revision", revision }` naming the current one, unless the stored document has it), `dryRun` (default false: everything but the write and the undo step) | `asset`, `path`, `changed` (false when the nodes stand arranged already: arranging is idempotent, and no step is recorded), `revision` (16 lowercase hex digits of the 64-bit FNV-1a hash of the file's text as the editor writes the document) and `programRevision` (the same of the document's content without what the scripting runtime never reads: the description, variables' tooltips and ranges, nodes' positions, comments and collapsing, comment boxes; arranging leaves it unchanged), of the document written or that a dry run would write, `arranged` (how many nodes), `bounds` (`{ min: [x, y], max: [x, y] }`: the arranged nodes' extent afterwards as a window draws them, where a comment box framing them goes; null for none); lays the nodes out as the window's Arrange does (comment boxes whose contents are all arranged nodes and boxes are fitted around them again, nested boxes inside the boxes that hold them: below) as one undo step "Arrange N Nodes in <script>"; an open window shows it as an outside change. InvalidParams with `data.path` `graph` (no file of its own), `function`, `nodes[<i>]` or `revision` (not 16 lowercase hex digits); InvalidOperation without a project; FileError when the file does not load (`... does not load: <error>; graph.set replaces it`) or is read-only |
| `graph.copy` | `graph`*, `function` (as `graph.arrange`), `elements`* (1 to 10000 IDs of nodes and comment boxes of that sub-graph) | `text`: the graph window's clipboard text of the elements (the links between them, the variables they use and the functions they call), which `graph.edit`'s `Paste` takes into any graph; read-only, works headless and never touches the system clipboard; InvalidParams with `data.path` `function` or `elements[<i>]`; FileError when the file does not load |
| `graph.interface` | `graph`* | `asset`, `path`, `kind` (`Script` or `Library`), `variables` (`[{ id, name, type, public }]`: a `Variable.Get` or `Variable.Set` whose `Graph` setting names the script reads and writes its public ones on another entity), `events` (`[{ node, name, parameters: [{ name, type }], enabled }]`: its custom events, which a typed `Event.Send` or `Event.Broadcast` sends; `enabled` is `Enabled`, `Disabled` or `DevelopmentOnly`), `functions` (`[{ id, name, pure, inputs: [{ name, type, formerNames? }], outputs, latent? }]`: a library's are what a `Function.Call` whose `Graph` names it calls; `latent` only with the scripting runtime), `uses` (`[{ asset, path, use, nodes }]`: the graphs its nodes name, `use` `Verification` (sends), `Interface` (variables, `RunsGraph`) or `Library` (calls)), `usedBy` (`[{ asset, path, nodes }]`: the other visual scripts whose nodes name it, by path), `skipped` (`[{ asset, path, error }]`: scripts that do not load, so not read for `usedBy`), `analyzed`; read-only, works headless; FileError when its file does not load |
| `graph.library` | `query` (in titles, keywords, categories and types, ignoring case), `category` (`Flow`, `Math`, `Components/Rigid Body`, ...: with the categories below it), `pinType` (a type ID, `exec` for flow), `direction` (with `pinType`: `In` for nodes with an input taking the type, `Out` for nodes with an output giving it; without it either), `limit` (1 to 500, default 50), `offset` | `nodes` (in library order: by category, then title), `total` (matches before paging), `notes` (`[{ member, category, message }]`: the game's members marked `[VisualNode]` that have no node, and why, filtered by `category` (with the categories below it) and by `query` (a case-insensitive substring of the member or the message), never paged; `[]` when there are none); each node `{ Type, Title, Subtitle?, Category, Description?, Keywords, Kind, Pure, Latent, Game?, Pins, Settings }` (`Game`: true for a node of a `[VisualNode]` member of the game's scripts, in the category `Game/<type>` unless the attribute names another), its pins `{ Name, Direction, Type, Label?, Default?, DefaultsToSelf?, Role?, Constraint?, Group?, ElementOf?, ArrayOf? }` (wildcards have the type `*`; `ArrayOf` names the pin whose type, followed by `[]`, is this pin's; the constraint `Expression` takes bool, the numbers and `Strada.Vector2`, `3` and `4`, what formulas compute with), its settings `{ Name, Kind, ... }` (the kinds `Expression`, `Timeline` and `Graph` among them, below); InvalidParams for an unknown `pinType` and for `direction` without one |
| `graph.types` | `query` (in IDs and titles, ignoring case), `kind` (`Exec`, `Bool`, `Integer`, `Float`, `String`, `Vector`, `Quaternion`, `Color`, `Matrix`, `Entity`, `Component`, `Asset`, `Enum`, `Struct`, `Class`, `Array` or `Object`), `limit` (1 to 500, default 50), `offset` | `types` (by ID), `total`; each type `{ ID, Title, Kind, FieldType? (the types public variables can have), AssetType?, Literal? (documents can hold its values), Enumerators? and Flags (enums), Makeable?, Breakable?, Fields? (structs), Base? (classes), Element? (arrays), ImplicitTo }` (`ImplicitTo`: the types its values convert to; every type also converts to `string`) |
| `graph.open` | `graph`*, `function` (0: the event graph, else a function's ID; default the sub-graph shown last), `select` (IDs of nodes and comments of that sub-graph, at most 10000; the first is the primary selection), `frame` (`Keep`, `Selection` (the default with `select`) or `All`), `zoom` (0.1 to 2), `center` (`[x, y]` in canvas units, within ±1e7), `entity` (the context entity), `problems` (opens the problems drawer), `find` (opens the find bar with the text and goes to the first match, which becomes the selection), `references` (`{ kind*, graph, id, name }`, as `graph.find`'s: opens the drawer's Find on the references of that variable, function or custom event, as Find References does; checked as `graph.find` checks it), `timeline` (a `Flow.Timeline` node of that sub-graph: opens the drawer on its tracks and curves), `reveal` (`{ kind* (Variable, Function or CustomEvent), id* }`: selects the row of My Script, which Details then shows; a local reveals in its function, and a custom event's `id` is its `Event.Custom` node's), `show` (a node of that sub-graph to bring into view: the view pans the least distance that shows it with a margin, keeping the zoom unless the node is larger than the view, and keeping the selection), `wireValues` (`Off`, `WhileStopped` or `Always`: what the data wires show while debugging, per user), `showFlow` (bool: light the flow while playing, per user), `outlineNotRunning` (bool: while the game runs the last version that worked, outline the nodes and links it does not run yet, per user), `gamePreview` (bool: while the game plays and the Game View is hidden, show the game in a corner of the graph windows' canvases, per user), `besideGameView` (bool: dock the Game View beside the graph's window at the start of the editor's next frame that follows the window's first draw, so also for a window the same call opens, Game View Beside; `graph.editors`' `gameView.beside` tells the outcome), `detailsRow` (a row of Details to scroll to the top of its pane, by the label it shows: `Formula`, `Settings`, `Length`, a setting's or an input's name; with `select`, a row of the node selected; at most 256 bytes; a row Details does not draw within three draws is dropped) | `asset`, `path`, `function` (the sub-graph shown), `selection` (the `select` given, as applied: empty without one or for a file that does not load; an empty `select` keeps the window's selection), `loaded`, `warnings`, `besideGameView` (`Queued`, when asked); opens the graph's window or brings it to the front, applied at its next draw; InvalidParams with `data.path` `function`, `select[<i>]`, `entity`, `timeline` (another node), `reveal.id`, `show` or `references.*` for what the graph or the scene does not have; FileError when `references.graph` does not load |
| `graph.close` | `graph` (default: every window) | `closed` (the assets whose windows closed, in tab order); a graph whose window is not open is no error, nor is a deleted graph named by its handle (an `asset://` path no graph has is AssetNotFound) |
| `graph.editors` | – | `editors`: the open graph windows in tab order, each `{ asset, path, function, focused, visible, readOnly, loaded, analyzed, errors, warnings, view: { center, zoom, level }, selection: { nodes, comments }, contextEntity, palette: { open, query }, notices, card, toasts, live, save, drawer, find: { open, text, index, count }, timeline?, bookmarks, minimap, panes: { sidebar, details, stacked }, shortcuts, canGoBack, canGoForward, debug: { state (None, Stopped, Waiting, Passed, Error, Stepping or StepEnded), stop, frame, node, function }, breakpoints (the graph's count), flow: { drawn, held }, liveReceipt (the window's last receipt since it opened or Play started, in `graph.live`'s form, or null), runtimeProblems: { errors, warnings } (the runtime problems at its document's nodes), notRunning (the nodes the game does not run as stored while it runs the last version that worked), gamePreview: { shown, rect } (the game in a corner of its canvas; rect `[x, y, width, height]` in screen pixels, or null) }` (below); `settings: { wireValues, showFlow, outlineNotRunning, gamePreview: { shown, corner (TopLeft, TopRight, BottomLeft or BottomRight), width (of the canvas's width) } }`; `gameView: { open, visible (its tab in front this frame), rect (where it was last drawn, or null), beside: { graph, outcome (Queued, Applied, AlreadyVisible or Failed), reason (Failed's, else null) } or null (the last Game View Beside) }`; `debugPanel: { open, visible, tab }` (null without the Debugger panel) |
| `graph.palette` | `graph`* (an open window), `query`, `at` (`[x, y]` in canvas units; default the view's centre), `from` (`{ node*, pin*, direction* (In or Out) }`: context mode), `limit` (1 to 200, default 50), `open` (default true; false closes it) | `open`, `query`, `context`, `entries` (`[{ title, subtitle, category, type, settings, connects?, converts? }]` in rank order), `total`; asynchronous: answers once the palette was drawn (below) |
| `graph.debug-panel` | `open` (bool: shows the Debugger panel, brought in front of the tabs it is docked with at its next draw, or hides it), `tab` (`CallStack`, `Values`, `Breakpoints` or `Instances`), `stop` (selects a stop of `graph.debug`), `frame` (selects a frame of the selected stop's call stack and opens its graph's window on the frame's node, as a click on its row does); with none it only reads | `open`, `visible` (its tab was in front at the last draw), `tab`, `stop`, `frame` (null while nothing is stopped); no history step; Unavailable without the editor's UI (headless); InvalidOperation for `stop` or `frame` while nothing is stopped; InvalidParams with `data.path` `stop` (no such stop) or `frame` (out of the call stack) |
| `graph.find` | `query` or `references` (`{ kind* (Variable, Function or CustomEvent), graph and id (Variable, Function), name and graph (CustomEvent; `graph` keeps the event and the typed senders of that script) }`), `graph` (only this script), `limit` (1 to 500, default 50), `offset` | `results` (`[{ asset, path, function, node?, comment?, variable?, field, text }]` in document order, the scripts by path), `total`, `analyzed`, `skipped` (scripts that do not load: `[{ asset, path, error }]`); works headless |

`graph.library` and `graph.types` need the scripting runtime (.NET 10): without it they fail with Unavailable, and
`graph.get`, `graph.set`, `graph.check` and `graph.edit` answer `analyzed: false` without `nodes` and `diagnostics`.
`graph.arrange`, `graph.copy` and `graph.interface` work without it, and every command but the window commands below
works headless. Every graph command fails with Unavailable without assets and `graph.create`, `graph.set`, `graph.edit`
and `graph.arrange` with InvalidOperation without a project; a `graph` that is not registered is AssetNotFound, and one
that is not a visual script (or, for `graph.set`, `graph.edit` and `graph.arrange`, has no file of its own)
InvalidParams with `data.path` `"graph"`. `graph.set` fails with FileError when the file cannot be read (missing) or
written, or is read-only (`the file of visual script '<path>' is read-only`), and with InternalError when the document
it prepared would not load back as written (an editor bug); `graph.edit` and `graph.arrange` fail with FileError also
when the file does not load (`... does not load: <error>; graph.set replaces it`).

**Revisions.** `revision` is 16 lowercase hex digits of the 64-bit FNV-1a hash of the document's file text as the editor
writes it (a version-2 or repaired file has the revision of its document as it loaded); it changes with any change.
`programRevision` is the same hash of the document's content without what the scripting runtime never reads: the
description, variables' tooltips and ranges, nodes' positions, comments and collapsing, and comment boxes. So a user's
move, comment or Arrange leaves it unchanged, and a link, value or setting changes it. `graph.get` returns both;
`graph.set`, `graph.edit` and `graph.arrange` (`revision` only) take them to refuse a call, writing nothing, when the
stored document changed since (InvalidOperation, `data: { path, <parameter>: <current> }`; InvalidParams for text that is
not 16 lowercase hex digits), and return both for the document they wrote. An agent editing a graph a user works in
passes `programRevision`, which the user's layout changes leave alone, and `revision` when the exact bytes matter.

**Other scripts.** `graph.set` and `graph.edit` report in `referrers` the nodes of other scripts that the new document
breaks: `[{ asset, path, nodes: [{ id, type, problem, message }] }]`, by path. `problem` is `Removed` (a library function
they call, a variable they read or write or a custom event they send is gone, or a kind switch took it away), `Private`
(a variable they use was made private), `Changed` (its type, purity or parameters changed: a used parameter gone or
retyped, former names followed) or `Stale` (an untyped `Event.Send` or `Event.Broadcast` of a custom event name that
went, which may have reached this script and no longer does). `summary.brokenReferrers` counts the nodes `Removed`,
`Private` or `Changed`, `summary.staleSenders` the `Stale` ones; nodes broken already are not reported. With
`refuseBrokenReferrers`, a `Removed`, `Private` or `Changed` referrer fails the call with InvalidOperation and
`data.referrers`, writing nothing (`Stale` senders are never refused). An edit that keeps what the graph offers others (its
kind, variables, custom events and its functions' IDs, purity and signatures with their former names) reads no other
script. Follow each referrer with a `graph.edit` of that script: its diagnostics offer the fixes (`Use <name>`,
Retarget, a `SetSetting` of `Name`).

**Graph windows.** `graph.open`, `graph.close`, `graph.editors` and `graph.palette` drive the editor's graph windows, one
per open visual script (Docs/Architecture.md section 11); without the editor's UI (headless) they fail with Unavailable,
and `graph.open` and `graph.palette` with InvalidOperation while a dialog is open. `graph.open` works while playing and
applies at the window's next draw: `graph.editors` a frame later shows what the user sees, its members as that draw saw
them. A file that is missing or does not load opens on its card (`loaded` false); `function`, `select`, `frame`,
`find`, `timeline`, `reveal` and `show` cannot be checked then and are ignored, which `warnings` says. The view parameters combine in a fixed order: the
framing (`frame`, or its default) first, then `zoom` around the framed centre, then `center`, so `{ select, zoom: 1.25 }`
frames the selection at 125 %. Views move smoothly unless the user turned on Reduce Motion, so a view read right after
`graph.open` may still be on its way.

In `graph.editors`, `function` 0 is the event graph; `visible` is false while another tab is in front of the window;
`readOnly` is true for memory assets and files without write permission; `view.level` is the level of detail (`Detail`,
`Compact`, `Overview` or `Far`); `notices` lists, top to bottom, `Degraded` (no scripting runtime), `Normalization`,
`Repaired`, `ReadOnlyMemory`, `ReadOnlyFile`, `NewFunction`, `Debug` (the debug strip), `FirstCanvas`, `BesideHint` (the one-time hint that Game View Beside keeps the game and the graph in view) and `GestureHint`; `card` is `None`,
`Missing`, `Unloadable`, `EmptyGraph` or `EmptyLibrary` (a function library without functions, or the empty event graph
of a library that has some); `live` the toolbar's pill (`None`, `Live`, `LastWorking` or `Simulating`); `save` `Saved`
or `NotSaved`; `drawer` `Closed`, `Problems`, `Find` or `Timeline`, with `timeline: { node }` (the `Flow.Timeline` node
it shows) when it is `Timeline`; `bookmarks` the slots stored (1 to 9); the panes `Hidden`, `Shown`, `AutoHidden` or
`Overlay`, and `stacked` true when both are shown in one column beside the canvas, My Script above Details, because the
window is too narrow for them side by side (both still report `Shown`); `shortcuts` whether the shortcut sheet is open;
`canGoBack` and `canGoForward` whether Back and Forward have a view to go to, in this window or, after opening a node
that names another graph, in that graph's window. A window opened since the last frame reports its state before its
first draw: not visible, not focused, nothing selected.

`graph.palette` opens the window's node palette as if the user pressed Tab at `at` and typed `query`, or, with `from`,
released a wire dragged from that pin there (named as the canvas names it: `Hit.Point` for a field of a split pin); it
changes nothing in the document (`graph.set` adds nodes). It answers once the palette was drawn, a frame later (two for a
window `graph.open` opened in the same frame): `entries` are its rows in rank order, at most `limit` (without `query` and
`from`, the recent entries), `total` the rows before the limit, `type` the node type a row adds (empty for actions that
add none), and with `from`, `connects` is the row's pin the wire links to and `converts` whether the link converts its
value. A query of `=` followed by a formula (`=a*b+sin(t)`) lists the `Expression` entry alone, titled
`Expression: <formula>` with the settings `{ "Formula": "<formula>" }`; `=` alone and `==` rank as other queries. Each
call opens the palette again with its own query, even a frame after the previous call. Its checks come in this order: Unavailable without the UI; the asset checks; InvalidOperation when the graph's
window is not open (`graph.open` first), its file does not load, or a dialog is open; InvalidParams for a `from.node`
that is not a node of the sub-graph shown, a `from.pin` that is no pin name and an `at` beyond ±1e7 (`data.path`
`from.node`, `from.pin`, `at`); Unavailable without the scripting runtime (the palette lists the node library); then
InvalidOperation when the window is not shown (another tab in front of it) after a draw; then InvalidParams (`from.pin`)
for a pin the node does not have.

`graph.find` searches what the find drawer searches: node titles, subtitles, types, comments, pin labels and stored
values (a literal's compact JSON, a `Format`), comment boxes, and the names of variables, locals, functions and custom
events, one result per element and field (ASCII letters in any case). References search every script: the
`Variable.Get` and `Variable.Set` nodes of a variable or local and the `Function.Call` nodes of a function, in its own
graph and in every graph whose nodes name it through their `Graph` setting; and a custom event's `Event.Custom`, the
untyped `Event.Send` and `Event.Broadcast` nodes of its name anywhere, and the typed ones naming `references.graph` (when
it is given, else any graph). Without the runtime, titles are derived from the documents (a node naming another graph
is titled from that graph's document) and subtitles and pin labels are not searched (`analyzed` false). InvalidParams when both or
neither of `query` and `references` are given, for `references` without what its kind needs and for an `id` the script
does not have (`data.path` `references.id`); AssetNotFound for a `graph` or `references.graph` that is not registered;
FileError when a script it names does not load.

**graph.edit.** `graph.edit` edits one visual script with an ordered batch of operations instead of a whole document:
each operation is the graph window's own edit, applied to the stored document as the call finds it, so a user's
concurrent work is kept and only what the operations touch changes. The batch is atomic: when one operation fails,
nothing is written, and the error names it. A successful call is one undo step "<description> in <script>" (default
"Edit Visual Script"), allowed while playing; an open window shows it as an outside change and flashes the changed
nodes, and a value the user is editing in Details or My Script survives when the batch touched none of its elements.
`graph.set` stays the tool for generated or wholesale documents. One call edits one graph: an edit that spans graphs (a
library function and its callers) is two calls in dependency order, the missing half reported meanwhile as a
diagnostic. Every operation is `{ "op": "<Kind>", ...fields }` (fields marked * are required); field names are
camelCase, while values inside `settings`, `literals`, `timeline` and `value` keep the document's PascalCase
(`{ "Parameters": [{ "Name": "Damage", "Type": "float" }] }`). A field an operation does not take is refused
(`operations[3].offset: AddNode takes no offset`), even when another operation takes it. Operations that name one thing
fail when it is absent (a Disconnect of a link that does not exist, a Fix that matches no diagnostic or two); those that
name a set succeed with an empty set.

| Op | Fields | What it does | Result |
|----|--------|--------------|--------|
| `AddNode` | `type`*, `function`, `position` (absent: unplaced), `settings`, `literals` (input pin → value), `collapsed`, `enabled`, `as` | Adds a node with its settings, then stores its literals and its collapsed and enabled states. A typed `Event.Send` or `Event.Broadcast` (`Graph` and `Name` among its settings) without `Parameters` gets the target event's; an unknown `Name` fails with `data.names`, the target's custom events | `{ id }` |
| `Delete` | `elements`*, `bridge` (default true) | Deletes nodes and comment boxes, linking the flow through the nodes that go, as Delete does (`bridge: false`: without that, as Shift+Delete) | `{ nodes, comments, notes }` |
| `Duplicate` | `elements`*, `labels` (by pasted order) | Duplicates them, as Ctrl+D does | `{ ids, notes }` |
| `Paste` | `clipboard`* (`graph.copy`'s text), `function`, `at` (default: below the sub-graph's placed nodes), `labels` | Pastes into the sub-graph, binding variables and functions by name and adding the missing ones | `{ ids, notes }` |
| `Move` | `elements`*, `offset`* | Moves nodes and comment boxes | `{}` |
| `SetPosition` | `element`*, `position`* | Places a node or comment box | `{}` |
| `Connect` | `from`* (`[node, "pin"]`), `to`* | Links an output to an input. An exec output and a data input keep one link, so it replaces theirs | `{ replaced: [{ from, to }] }` |
| `Disconnect` | `from` and/or `to`, or `node` | Removes one exact link (an error when it is absent), every link of an output (`from` alone), of an input (`to` alone) or of a node | `{ removed }` |
| `Relink` | `remove`* (`[{ from, to }]`), `add`* | Removes and adds links in one go | `{ replaced }` |
| `Splice` | `node`*, `link`* (`{ from, to }`), `input` (default `Execute`), `output` (default the node's first exec output) | Puts the node into a link: `{ "op": "Splice", "node": "$log", "link": { "from": [12, "True"], "to": [13, "Execute"] } }` puts it into a chain | `{ replaced }` |
| `InsertReroute` | `link`*, `position`*, `as` | Adds a reroute point on a link | `{ id }` |
| `SetInput` | `node`*, `pin`*, `value` (null removes) | Stores the literal of an unlinked input | `{}` |
| `SetSetting` | `node`*, `name`*, `value` (null removes) | Sets a setting. A setting that changes pins or senders goes to its operation: a Sequence's `Count` to SetSequenceOutputs, `Cases` to SetSwitchCases, `Parameters` to SetEventParameters, an `Event.Custom`'s `Name` to RenameCustomEvent, an `Array.Make`'s `Count` to SetArrayElements, an `Expression`'s `Formula` to SetFormula, a `Flow.Timeline`'s `Timeline` to SetTimeline (renaming no track; null sets the default timeline: 1 s, no loop, no tracks), and `Graph`, or the `Function`, `Variable` or sender `Name` of a node naming another graph, to Retarget | the routed operation's, else `{}` |
| `SetSequenceOutputs` | `node`*, `count`* (2 to 64), `insertAt` or `removeAt` | A Sequence's outputs, `Then<k>` renumbered with their links | `{ removedLinks }` |
| `SetSwitchCases` | `node`*, `cases`* (an array, or null) | A Switch's cases, with their links | `{ removedLinks }` |
| `SetEventParameters` | `node`*, `parameters`* (`[{ name, type }]`) | The parameters of an `Event.Custom`, `Event.Send` or `Event.Broadcast`, with their links | `{ removedLinks }` |
| `RenameCustomEvent` | `node`*, `name`* | Renames an `Event.Custom`: this script's untyped senders and those typed with this script follow (other scripts' senders of the old name are `Stale` referrers) | `{ renamedSenders }` |
| `SetArrayElements` | `node`*, `count`* (0 to 64), `insertAt` or `removeAt` | A Make Array's elements, renumbered with their links, literals and split pins as SetSequenceOutputs renumbers a Sequence's outputs | `{ removedLinks }` |
| `SetFormula` | `node`*, `formula`* (at most 4096 bytes) | An Expression's formula. An input kept keeps its links and value; when the formula keeps its number of inputs, inputs whose names changed are renamed in order with them; the others' go | `{ renamedInputs: [{ old, new }], removedLinks }` |
| `SetTimeline` | `node`*, `timeline`* (a `Timeline` setting, below), `renames` (`[{ old, new }]`: track names) | A Timeline's tracks and keys, written in the canonical form. Tracks kept by name keep their links, and renamed tracks keep theirs under the new name; removed tracks, and tracks whose type changed (between exec and data, or between data types), lose theirs. A damaged setting is refused at `operations[i].timeline` with the runtime's path and words (`Timeline.Tracks[1].Keys[3].Time: keys are in time order: 0.5 comes after 0.75`) | `{ removedLinks }` |
| `SetNodeComment` | `nodes`*, `text`* (`""` removes) | The note above each node | `{}` |
| `SetCollapsed` | `nodes`*, `collapsed`* | Collapses or expands nodes | `{}` |
| `SetEnabled` | `nodes`*, `enabled`* (`Enabled`, `Disabled` or `DevelopmentOnly`) | Each node's state | `{}` |
| `SplitPin`, `RecombinePin` | `node`*, `pin`*, `direction`* (`In` or `Out`) | Splits a struct pin into its fields, or puts it back (a field pin recombines the innermost split pin it belongs to) | `{ removedLinks }` |
| `AddComment` | `text`*, `function`, `elements` (framed), `position`, `size`, `color`, `as` | Adds a comment box, around `elements` when given (measured as a window draws them), else below the sub-graph's placed nodes | `{ id }` |
| `SetComment` | `comment`*, `text`, `position`, `size`, `color` | Changes a comment box | `{}` |
| `SetDescription` | `description`* | The document's description | `{}` |
| `SetKind` | `kind`* (`Script` or `Library`) | The document's kind; it changes nothing else, and a library's variables and event nodes show as errors until removed | `{}` |
| `AddVariable` | `name`*, `type`*, `value`, `public`, `tooltip`, `range`, `as` | Adds a variable | `{ id }` |
| `SetVariable` | `variable`*, any of `name`, `type`, `value` (null clears), `public`, `tooltip`, `range` (null clears), `formerNames` | Changes a variable; a rename keeps the old name in `FormerNames` | `{ valueCleared }` |
| `DeleteVariable` | `variable`*, `removeNodes` (default true) | Deletes a variable with its nodes, bridging the flow through them as Delete does | `{ removedNodes }` |
| `ReorderVariables` | `variables`* (every variable, in order) | Orders the variables | `{}` |
| `DuplicateVariable` | `variable`*, `name` | Duplicates a variable | `{ id }` |
| `AddLocal` | `function`*, `name`*, `type`*, `value`, `as` | Adds a local to a function | `{ id }` |
| `SetLocal` | `function`*, `local`*, any of `name`, `type`, `value` | Changes a local | `{ valueCleared }` |
| `DeleteLocal` | `function`*, `local`*, `removeNodes` (default true) | Deletes a local with its nodes | `{ removedNodes }` |
| `AddFunction` | `name`*, `pure`, `inputs` (`[{ name, type }]`), `outputs`, `as`, `entryAs`, `returnAs` | Adds a function with its Entry and Return nodes | `{ id, entry, return }` |
| `RenameFunction` | `function`*, `name`* | Renames a function (calls name it by ID) | `{}` |
| `SetFunctionSignature` | `function`*, `inputs`*, `outputs`* | A function's parameters: one kept by name keeps its links wherever it moved; when a list keeps its length, the k-th name that went is renamed to the k-th that came, with its links. A parameter given without former names keeps those of the parameter it continues | `{ removedLinks }` |
| `SetFunctionPure` | `function`*, `pure`* | Makes a function pure or impure, its calls gaining or losing flow pins | `{ removedLinks }` |
| `DeleteFunction` | `function`*, `removeCalls` (default true) | Deletes a function with its calls | `{ removedCalls }` |
| `DuplicateFunction` | `function`*, `name` | Duplicates a function (nodes naming another graph are copied as they are) | `{ id }` |
| `PromoteToVariable` | `node`*, `pin`*, `direction`*, `name`, `as` (the new Get or Set) | Makes a pin's value a variable, keeping the program's value | `{ variable, node }` |
| `PromoteToFunctionInput` | `node`*, `pin`*, `name` | Makes a pin of a function's node an input of the function | `{}` |
| `Retarget` | `node`*, `settings`* (`Graph`, `Function`, `Variable`, `Name`, `Parameters`; null removes one) | Gives an `Event.Send`, `Event.Broadcast`, `Function.Call`, `Variable.Get`, `Variable.Set` or `RunsGraph` new settings that change what it names. `Variable` and `Function` may be names in the named graph, and `Graph` may be `asset://`. A typed Send or Broadcast without `Parameters` takes its event's; `{ "Graph": null }` makes a Send untyped, keeping its parameters. Links to pins the new target keeps stay | `{ removedLinks }` |
| `Arrange` | `function`, `scope` (`Graph` (default), `Created` or `Nodes`), `nodes` (with `Nodes` only), `extents` (`Measured` (default) or `Uniform`) | Lays out every node of the sub-graph (`Graph`), the nodes the batch added without a position (`Created`: beside the nodes they continue, in columns ordered against the links from those nodes, then down until they and the frame a comment box framing them would get stand clear of every node and comment box, so framing them with AddComment {elements} overlaps no earlier box) or the given ones; comment boxes move with their nodes as `graph.arrange` moves them | `{ arranged }` |
| `Align` | `elements`* (2 or more), `alignment`* (`Left`, `Right`, `Top`, `Bottom`, `Centers` or `Middles`), `extents` | Aligns them | `{}` |
| `Distribute` | `elements`* (3 or more), `axis`* (`Horizontal` or `Vertical`), `extents` | Spaces them evenly | `{}` |
| `Straighten` | `nodes`* (2 or more) | Straightens their flow, as Q does | `{ moved }` |
| `Fix` | `code`*, `fix`* (the fix's label), and the diagnostic's fields that locate it: `node`, `pin`, `direction`, `link`, `variable`, `function` | Applies the quick fix of the one diagnostic of the working document that matches. Agents need not build it: each diagnostic's `Fixes` lists it ready. Fixes that only navigate (Go To, Edit Setting, Rename) change nothing and are refused, naming the operation that makes the edit (SetSetting, RenameFunction, ...) | `{ fix }` |

Nodes added without a position must be placed, by an Arrange (scope `Created`) or a `position`, before Move, Align,
Distribute, Straighten, AddComment's framing or PromoteToVariable take them (`node N has no position yet: Arrange the
nodes added without one (scope Created) or give it a position before moving, aligning, framing or promoting it`): add
nodes, connect them, arrange, then frame or comment them. Arrange with `Graph` suits a graph the agent made (every node
of a graph just made from the Events template is the agent's, so its events join the first column); in a user's graph,
`Created` places the new nodes and moves nothing else. Nodes left without a position are placed by the commit, below the
graph's placed nodes.

**Arranging and comment boxes.** A comment box that holds only arranged nodes moves with them when it holds one, stands in
such a box, or when every node of the graph is arranged. What it holds moves as one block: its nodes and the boxes inside
it are laid out on their own, as arranging them alone lays them out, and the box, fitted around them as a new comment box
framing them would be, stands in the layout as one node linked as its nodes are. Boxes inside it are blocks of that
layout in turn, so boxes stay nested as they were. A box that holds nothing keeps its size and goes after the nodes.
Boxes that overlap without one holding the other, and hold a node or a box in common, move as one block. So of the boxes
that move, those that did not overlap before arranging do not overlap after it, a box that held another still holds it,
and each frames the nodes it framed; one that overlapped no other box (except those it held or stood in) holds no other
node. The arranged nodes' top-left corner stays where it was, so arranging twice changes nothing.

**References.** An integer names a node, comment box, variable, local or function by its ID in the document **as the
call found it**: an ID an earlier operation deleted is an error (`node 20 was deleted by operations[2]`), even when a new
element took it again, so new elements are reached through labels or names. A `"$label"` (`$` and 1 to 63 ASCII
letters, digits or `_`) names what an earlier operation made with `as` (or `labels`, `entryAs`, `returnAs`); a label is
defined once, and using it before its definition is an error that lists the labels defined (`operations[5].to[0]:
$branch is not defined by an earlier operation; defined: $speed, $step`). The result maps every label to its ID.
Variables, locals and functions may also be named, as earlier operations left them (a function an earlier AddFunction
added, a variable by its new name); where a node is expected a `$` string is always a label. Names also work in the
`Variable` and `Function` settings (`{ "Variable": "Speed" }`), the node's function's locals first, then the variables;
in a node whose `Graph` names another script, the name is looked up in that script's document, and a `Graph` naming the
edited script itself resolves to the batch's working document. An unknown name is an error naming the closest three
(`data.names`). An `"asset://<path>"` or `"builtin://<name>"` text given as the value of an asset-typed input, variable,
local or array element, an asset setting or a `Graph` setting is stored as the asset's handle (AssetNotFound when it
names no asset); a `string` pin keeps the text. Pins may be named as `graph.get`'s analysis names them or as the
document stores them.

**Pins and the scripting runtime.** The batch keeps one analysis of its working document and analyzes again only when
an operation needs pins that earlier operations may have changed: Connect, Relink, Splice, SplitPin, RecombinePin, the
promotions, Fix and the measured layouts read pins and types; an AddNode with an asset literal analyzes once to know the
types; renaming a variable, local or function before a measured layout costs one more analysis, as it changes titles.
So group the creations before the connections: the result's `analyses` counts them, and the stored document's analysis
is kept for the next call, so a chain of `graph.edit` calls analyzes each written document once. A typical batch
(variables, nodes, connections, literals, an Arrange) costs at most three. Without the scripting runtime the operations
that need pins fail with Unavailable (`operations[3] (Connect) needs the scripting runtime: it knows the pins of nodes;
without it, graph.set writes links as given`), except Delete, DeleteVariable, DeleteLocal and DeleteFunction, which run
without bridging flow (only reroutes bridge), and the measured layouts (Arrange, Align, Distribute, Straighten,
AddComment's framing), which measure nodes from the pins their links and literals name, as `graph.arrange` does; every
other operation works. When the runtime fails to analyze, an operation that needs pins fails with Unavailable ("see the
log"), and `refuseNewErrors` with it.

**Canvases.** graph.edit lays a sub-graph's headless canvas out only when what the operation reads may have changed:
operations that read pin rows (Connect, Relink, Splice, deletions that bridge, splits) reuse a canvas across link, value
and layout changes; those that read node sizes or pin anchors (Arrange, Align, Distribute, Straighten, Promote to
Variable, comment framing, Paste's corner) reuse it across moves. Without the runtime every change lays it out again,
since the degraded canvas infers pins from links and literals. The headless canvas resolves the other graphs nodes name,
as a window does, so a node that names another graph is sized by its title from that graph also without the runtime. A
thousand Connects on a 2,000-node graph take about 114 ms in Release.

**The result.** `changed` is false when the batch left the document as it found it: nothing is written and no step is
recorded, even when the file holds nodes without a position or types a commit would store canonically (an empty batch
above all, which answers the current analysis and its fixes from one cached analysis). `results` has one object per
operation (the table's last column). `warnings` are what the commit changed besides the operations, as `graph.set`'s.
`summary` is `{ errors, warnings, newErrors, newWarnings, brokenReferrers, staleSenders }` (the first four with the
runtime only). `diagnostics` are `graph.get`'s, each with three members more: `Fixes`, `[{ label, operation }]`, its quick
fixes as complete Fix operations that a next batch appends unchanged (`{ "op": "Fix", "code": "UnknownPin", "fix": "Use
Value", "node": 12, ... }`); `New`, true when the stored document did not have it; and `Label`, the batch's label of its
node when the batch created it. `nodes` lists the analysis of the nodes the batch created or whose program it changed
(`Changed`). `hints` (`[{ node, kind, message }]`) name the commonest silent mistakes in the nodes the batch created or
changed and those at either end of a link it removed or replaced: `NeverRuns`, an impure node without flow into any exec
input (`<title> (node 7) has no flow into Execute: it never runs`); `Unused`, a pure node whose outputs nothing reads;
`UnlinkedInput`, an Expression input neither linked nor stored, read as 0 (`Expression (node 6) reads dt as 0: link it
or store a value`). `referrers` are the nodes of other scripts the edit breaks (above). `document`, with `"document":
true`, is the result in the content form. Without the scripting runtime `analyzed` is false, `diagnostics`, `nodes` and
`hints` are left out, and `summary` holds only `brokenReferrers` and `staleSenders`.

| Error | When | `data` |
|-------|------|--------|
| InvalidParams | the schema (an unknown parameter, a wrong type, more than 10000 operations); an operation that does not apply to the document as the earlier operations left it; a `graph` that is not a visual script or has no file of its own | `{ path: "operations[3].from", operation: 3, op: "Connect" }` plus `pins` (the node's pins: name, direction, type) for an unknown pin, `labels` for an unknown label, `names` (the closest three) for an unknown name or type, `candidates` for a Fix that matches several diagnostics; `{ path: "graph" }` |
| InvalidOperation | no project; a stale `revision` or `programRevision`; `refuseNewErrors` with new Errors; `refuseBrokenReferrers` with broken referrers | `{ path, revision }` or `{ path, programRevision }`; `{ diagnostics }` (the new Errors); `{ referrers }` |
| Unavailable | no assets; an operation needs the scripting runtime, or pins it failed to analyze; `refuseNewErrors` when the analysis fails | as InvalidParams' for an operation |
| AssetNotFound | `graph`, or an `asset://` value of an operation, names no registered asset | `{ path }`, as InvalidParams' for an operation |
| FileError | the file is missing, does not load (`graph.set` replaces it), is read-only or cannot be written | – |
| InternalError | the prepared document fails the strict check (an editor bug) | – |

Every error of an operation carries `data.path`, `data.operation` and `data.op`; its message names the operation and its
kind, then the editing operation's own words (`operations[3] (Connect): node 12 has no output Then`).

This batch turns its entity, like the `Spinner` script of Docs/Scripting.md, in a graph `graph.create` just made from the
Events template (node 2 is its `Event.Update`). It keeps an angle of its own, because `TransformComponent.EulerAngles`
reads back one of several sets of angles (Y within ±90 degrees), so adding to the angles read would stall. It is one
undo step, "Spin at Speed degrees per second in Spinner", and makes three analyses in a first call (the stored
template's, one at the first Connect, whose Expression inputs exist only once analyzed, and one before the Measured
Arrange, reused for the result):

```json
{ "graph": "asset://Scripts/Spinner.sgraph", "description": "Spin at Speed degrees per second", "operations": [
	{ "op": "AddVariable", "name": "Speed", "type": "float", "value": 90.0, "public": true, "tooltip": "Degrees per second" },
	{ "op": "AddVariable", "name": "Angle", "type": "float" },
	{ "op": "AddNode", "type": "Variable.Get", "settings": { "Variable": "Speed" }, "as": "$speed" },
	{ "op": "AddNode", "type": "Variable.Get", "settings": { "Variable": "Angle" }, "as": "$angle" },
	{ "op": "AddNode", "type": "Expression", "settings": { "Formula": "(angle + speed * dt) % 360" }, "as": "$next" },
	{ "op": "AddNode", "type": "Variable.Set", "settings": { "Variable": "Angle" }, "as": "$store" },
	{ "op": "AddNode", "type": "Expression", "settings": { "Formula": "vec3(0, a, 0)" }, "as": "$euler" },
	{ "op": "AddNode", "type": "Set:Strada.TransformComponent.EulerAngles", "as": "$turn" },
	{ "op": "Connect", "from": ["$angle", "Value"], "to": ["$next", "angle"] },
	{ "op": "Connect", "from": ["$speed", "Value"], "to": ["$next", "speed"] },
	{ "op": "Connect", "from": [2, "DeltaTime"], "to": ["$next", "dt"] },
	{ "op": "Connect", "from": ["$next", "Result"], "to": ["$store", "Value"] },
	{ "op": "Connect", "from": ["$store", "Value"], "to": ["$euler", "a"] },
	{ "op": "Connect", "from": ["$euler", "Result"], "to": ["$turn", "Value"] },
	{ "op": "Connect", "from": [2, "Then"], "to": ["$store", "Execute"] },
	{ "op": "Connect", "from": ["$store", "Then"], "to": ["$turn", "Execute"] },
	{ "op": "Arrange", "scope": "Graph" }
] }
```

What agents most often need explained:

| Symptom | What `graph.edit` answers | What to do |
|---------|---------------------------|------------|
| a type string from memory (no signature, a typo) | a unique signature-less match is stored canonically (`warnings`: `node updated`); else `UnknownNodeType` or `AmbiguousNode`, whose `Fixes` are "Change to ..." or "Use ..." | the Fix with the label, or `graph.library` |
| a pin name that is not the node's (`Exec`, `Output`) | InvalidParams `node 12 has no input Exec` with `data.pins` | use a listed name |
| flow linked into data or data into flow | `TypeMismatch` on the link | flow pins are `Execute` and `Then`; Splice puts a node into a chain |
| a Connect that took over an existing link | `replaced: [{ from, to }]` (an exec output and a data input keep one link), and a `NeverRuns` hint on the node that lost its flow | meant for chains; Splice inserts |
| a node added but never run | `hints`: `NeverRuns` | connect flow into its `Execute` |
| an Expression input left unlinked | `hints`: `UnlinkedInput` (`reads dt as 0`) | link it, or store a value with SetInput |
| too few Sequence outputs, a missing Switch case | `node 7 has no output Then3` | SetSetting of `Count` (renumbers), SetSwitchCases |
| a literal of the wrong form | `InvalidValue`, `EntityValue` | the literals below; entities come from public `Strada.Entity` variables; `asset://` paths are converted |
| a formula mistake | `InvalidExpression` with `Setting`, `Span` and a line and column in the message; for a misspelled function a `Fixes` entry "Use sin" | the Fix, or a corrected formula with SetFormula (inputs renamed in order keep their links) |
| a formula input named like a function (`distance / speed`) | nothing: any name not followed by `(` is an input | – |
| `DuplicateEvent` after adding an `Event.Update` | the diagnostic, `New: true` | connect to the existing event (node 2 of the Events template) |
| impure nodes in a pure function | `ImpureInPureFunction`, with the fix "Make <function> Impure" | the Fix |
| a library function called without `Graph`, or a script's function with one | `UnknownFunction`, `NotALibrary` | `graph.interface`, then Retarget |
| a typed Send whose `Target` is left unlinked | `TargetIsSelf` (a Warning): Self runs this script, not the target | link the entity to send to |
| an edit that breaks other scripts (a removed function, a variable made private, a renamed event) | `referrers` with each node's `problem`, `summary.brokenReferrers` and `staleSenders` | one `graph.edit` per listed script, or `refuseBrokenReferrers` to keep the edit out |
| the user changed the graph meanwhile | InvalidOperation on `programRevision` (the program changed) or `revision` (anything changed) | `graph.get`, then edit again; prefer `programRevision`, which the user's moves leave alone |
| no .NET runtime | Unavailable for the operations that need pins; deletions and measured arranging run degraded | `graph.set` writes links as given |

**Strictness.** `graph.set` checks its `document` as the loader checks files, but refuses what the loader would repair:
an unknown key, a missing or duplicate ID, a name or type that is invalid or taken, a link to a node of another graph or
one that repeats another, a value nested more than 16 levels deep, a position outside ±1e7, a list past its limit, ...
It then fails with InvalidParams: the message is the first issue (`"<path>: <message>"`), `data.path` is `"document."`
followed by its path and `data.issues` lists every issue as `{ path, message }`, with paths relative to the document
(`Graph.Nodes[3].ID`, `Functions[0].Locals[1].Name`, `Variables[2].Range`). Nothing is written then. Keys left out take
their defaults, and `"Value": null` means no value; neither is an issue. `graph.check` reports the issues in `issues`
instead (`valid` is true without any) and checks the repaired document further. Problems of meaning (unknown node types,
pins, links of types that do not convert, ...) are never issues: the analysis reports them as diagnostics, and
`graph.set` stores such documents (they do not run until fixed).

**What graph.set and graph.edit change.** Besides storing the document given (or the one the operations made),
`graph.set` and `graph.edit` report in `warnings` (and `graph.check` lists) what the commit changes, as the graph window's
commits do:
- A variable whose ID the stored document has under another name keeps that name in its `FormerNames`, so values
  components store under it still apply to the variable (`"variable renamed: <old> → <new> (<old> is kept in
  FormerNames)"`); a variable keeps its 16 most recent former names. A name that is a variable's current name, or a
  former name of another variable, is not kept.
- In a function library, a parameter renamed in place keeps its old name in its `FormerNames` (`"parameter renamed:
  Clamp01's Value → Amount (Value is kept in FormerNames)"`), so calls in other scripts that still name the old pin keep
  working. The pairing is the one by which `SetFunctionSignature` moves the pins' links: a parameter kept by name records
  nothing, wherever it moved; when a list keeps its length, the k-th name that went is kept by the k-th name that came; a
  list whose length changed records nothing. In every document, a former name that a parameter of the same list now has
  is dropped.
- With the runtime, a node whose `Type` resolved through a redirect (an API member that was renamed or changed its
  signature) or as the only overload of its member with the node's pins is stored under its exact type, its renamed pins'
  links, inputs and `Split` entries following with the paths of their fields (`Force.X` follows a renamed `Force`;
  `Forces.X` does not), `Split` entries that end up repeated dropped (`"node updated: <old> → <new>"`). A node resolved
  exactly whose analysis lists `PinRenames` (a call of a library function whose links, `Inputs` or `Split` still name a
  former parameter name) keeps its type and stores the new names (`"pin renamed: <title> (node <id>) <old> → <new>"`),
  but only when the stored document has that node with the same type and settings: an analysis made before the node
  changed renames nothing.
- Nodes without a `Position` are placed (not reported): each group of linked nodes below the placed ones, links running
  left to right in columns 320 apart, the nodes of a column 160 apart.

While playing, the step of `graph.set` or `graph.edit` goes into the play history; `play.stop` moves the steps that edited asset files
(`graph.set`, `graph.edit`, `graph.arrange`, `material.set`) into the edited scene's history as its newest steps, so they stay undoable and the scene
keeps the unsaved state it had (steps undone while playing are not kept). The file keeps the change either way. Running
instances of the graph take the edit in the edit's own call, before the command answers, also while the game is paused
and into an execution stopped at a breakpoint: variables keep their values (matched by ID, then by name and type; others
start at their `Value`, else their type's default, not at a value the component stored before they existed), values set
for them with `component.set` reach them as they take it, nodes keep their state (DoOnce, DoN, FlipFlop, Gate, a
Timeline's time, clamped into a shorter `Length`) while their type stays and they stay enabled, and nodes waiting (a
`Flow.Delay`, ...) keep waiting while they still exist and are not disabled by the edit (disabling a node counts as
removing it: its state does not carry over, and an execution waiting in it is cancelled with a Warning). An edit that
changes no program (positions, comments, collapsed states, the description, tooltips, ranges) reaches nothing and
compiles nothing.

**Live edits.** While playing, `graph.edit` and `graph.set` change running graphs at once (stopped executions included)
and their result gains `live` (`{ playState, paused, edits }`): `edits` are the receipts of this call, final when it
answers, in the form `graph.live` lists them (below), one per graph the edit compiled again: the graph edited, or for a
function library each graph that calls it (with the library as `cause`). It is empty for a dry run, a result that is the
stored document, and while not playing (`playState` `edit` or `simulate`). A receipt's `outcome` is `Applied` (the
running instances took the new version: `instances` of them, with what each kept, reset and cancelled), `Pending` (a step
of the debugger runs in the old version and holds the swap; a receipt with `deferred` true follows when it ends),
`KeptLastWorking` (the edited version does not compile, and the game keeps running the last version that worked) or
`NotRunning` (it does not compile and no version of the graph runs). Whether a graph keeps a last working version is one
rule: a receipt is `KeptLastWorking` exactly when the log's error line for the send ends with `; the previous version
keeps running`, and the graph windows' LIVE pill turns amber ("last working version") by the same rule. `graph.live`
says what each graph runs: `running` is `Current` only when the game runs exactly what is stored, compiled with the
graphs it refers to as they are now (its `runningProgramRevision` then equals `programRevision`), and lists the nodes and
links the game does not run yet in `notRunning`. It also keeps the receipts and the runtime problems of the play
session, each with a serial: pass the last answer's `serial` as `after` to read only what is new.

**Analysis.** `nodes` lists every node of the document (those of the functions' graphs, then those of the event graph):
`{ ID, Function (0 for the event graph), Type (as stored), Resolved (Exact, Redirect, Unique or Unknown), CanonicalType?
(Redirect and Unique), PinRenames? ([{ Direction, From, To }]: Redirect and Unique, and an Exact call of a library
function whose links name a former parameter name), Title, Subtitle?, Kind, Pure, Latent, PassThrough?, Pins }`, with the types the runtime resolved its wildcards to (`*` while open) and, in place of each split pin, its field
pins (`Hit.Point`, typed by the field; an input field pin with the `Default` it takes unlinked). `PassThrough` is the
exec output where the flow entering a disabled node continues; it is absent where the chain ends, on disabled nodes no
flow enters (events) and on every node that is not disabled. `diagnostics` are `{ Severity (Error or Warning), Code,
Function, Node?, Pin?, Direction?, Link? ({ From, To }), Variable? (a variable or local ID), Setting?, Span?, Graph?,
Message, Suggestions? }`: `Setting` names the node's setting a finding is about (`Formula`), `Span` (`[start, end]`) the
part of that setting's text it concerns, in UTF-8 byte offsets, the end exclusive, and `Graph` (a decimal handle) the
other visual script a cross-graph finding concerns (such a finding is located on its node, never on a variable).
`Suggestions` lists the nearest known names for unknown ones (decimal IDs for a variable or function of another graph)
and the candidates of an ambiguous node. The analysis runs with the graph itself as the graph the document is for, so
nodes whose `Graph` setting names their own script are checked against the document itself (`graph.check` does so when
given `graph`). A graph with an
Error does not run: the log names its first error (`Visual script '<path>' has 1 error; first: <message> (node <id>)`)
and each entity left without the graph (`Entity '<name>' runs no visual script: <reason>`), and a graph edited while
playing into one with an Error keeps running its last working version. A code is an Error unless it is marked W or
located on a disabled node: on its node, or at either end of its link. Disabling a broken node is how its graph keeps
running.

**Disabled nodes.** A node whose `Enabled` is `"Disabled"` passes through, whatever its kind: flow entering any of its
exec inputs continues at its `Then` output, else `Completed`, else `Default`, else its first exec output that is not a
Tick output; without one the chain ends there (a node of an unknown type has none). Each of its data outputs takes the
value of the first data input of the same type that no earlier output takes, both in pin order (only those inputs, and
the pure nodes feeding them, are evaluated); the other outputs take their type's default, and an unconnected `Target`
feeds none (a disabled `Flow.Timeline`: `Time` takes `NewTime`, its first float curve `Rate`, and `Playing`, `Direction`
and its other curves their defaults). It costs one step, never waits and keeps no state. A disabled event starts nothing and is no duplicate of an
enabled one; a disabled latent node makes no function latent. `Function.Entry`, `Function.Return` and pure nodes cannot
be disabled and run as usual (`DisabledIgnored`). `"DevelopmentOnly"` nodes run as `"Enabled"` ones, except in exported
games, where they are disabled.

The codes:

| Code | Meaning |
|------|---------|
| InvalidDocument | The document is not a JSON object, or not text: "the document is not UTF-8 text", "the document has a string or member name that is not text: an escaped surrogate without its pair" (the editor parses what it is given, so its documents are always text); or an `Enabled` or `Split` the loader would repair: `<path>.Enabled: expected "Enabled", "Disabled" or "DevelopmentOnly"`, `<path>: expected a pin path: 1 to 128 bytes without control characters, its '.'-separated parts not empty`, `a list of split pins has at most 64 entries`, `"<entry>" comes earlier`, `"<entry>" is a field of "<parent>", which is not an earlier entry`, `Split: expected an object`, `Split.Inputs: expected an array` (the entry is skipped; a skipped parent skips its fields) |
| UnknownNodeType | No node kind has the node's `Type` |
| AmbiguousNode | A `Type` without a signature (`Call:<class>.<member>`) fits several overloads of the member; `Suggestions` lists them |
| NodeUpdated (W) | The `Type` resolved through a redirect or as the only overload of its member: `"node updated: <old> → <new>"` (`graph.set` stores the new type) |
| UnknownPin | A link names a pin the node does not have, or links an output as an input; a link to or from a split pin itself (`<Title> has no input pin "<Pin>": it is split, so links reach its fields`), or a link or `Inputs` key naming a field of a pin that is not split (`... "<Pin>.<Field>": <Pin> is not split`) |
| UnusedInput (W) | An `Inputs` key names no input of the node, or holds a literal of a split input (`<Pin> is split: the stored value is not used (its fields take values of their own)`) |
| UnusedSplit (W) | An entry of a node's `Split` splits nothing, and the pin stays whole. `Direction` is In for an entry of `Split.Inputs`, Out for one of `Split.Outputs`; `Pin` is the entry exactly as stored. `Split.<Inputs\|Outputs> "<entry>" is ignored: <why>`: the node has no such pin, it is an exec pin, its type is not resolved, its type is no struct that splits (inputs split only structs whose public fields are all writable; outputs structs with public fields or readonly structs with get-only properties), it is a field of a pin that is not split, or an earlier entry splits it. Not reported on unknown nodes and nodes with broken settings |
| DisabledIgnored (W) | `Function.Entry`, `Function.Return` or a pure node is disabled, and runs anyway (`Node` only): "<title> cannot be disabled: every call of the function starts there, so it runs", "... the call returns there, so it runs", "<title> is pure: it runs only when a node reads it, so it cannot be disabled and runs (disable the node that reads it)" |
| MultipleExecLinks | An exec output has more than one link |
| MultipleDataLinks | A data input has more than one link |
| TypeMismatch | A link joins types that do not convert (a `Convert` node converts explicitly), or an exec pin to a data pin; also a link from a disabled node's output that a wildcard input's constraint refuses (the input then takes no type from it) |
| UnresolvedWildcard | A wildcard input gets no type: link it, or give it a literal another operand types (`the type of <pin> is unknown: none of its links gives it a value` when all its links are errors); on a link, a link from a wildcard output nothing typed (`the type of <id>.<pin> is unknown: <id>.<pin> takes no value from it`), and from an untyped output of a disabled node (`... and <title> is disabled: <id>.<pin> takes no value from it`, which the input ignores) |
| NoOperator | An `Operator.*` node has no operator for its operands' types |
| NoConversion | A `Convert` node cannot convert between its types |
| InvalidValue | A literal (a variable's `Value`, an `Inputs` member, `Literal`'s `Value`) is not one of its type (below), or a `DefaultsToSelf` input has one; an element of an array literal is named by its index (`Speeds: Value[2]: ...`) |
| EntityValue | An entity literal other than `"0"`: graphs are shared, so entities come from public variables set per component |
| ZeroQuaternion (W) | A quaternion literal of length 0 reads as the identity |
| InvalidSetting | A setting has a value its node kind does not take |
| MissingSetting | A required setting is missing |
| UnknownSetting (W) | A `Settings` key names no setting of the node kind |
| MissingTypeArgument | A generic member's node has no `TypeArguments`; a method whose type parameters have no constraints needs them only when no input decides a type (`<Title> needs TypeArguments: N type ID(s), as no input decides T`) |
| InvalidCase | A `Flow.Switch` case is not a literal of the switched type |
| DuplicateEvent | An event the document may have once (`Event.Update`, one `Event.KeyPressed` per key, one `Event.Custom` per name, ...) appears again |
| EventInFunction | An event node in a function's graph |
| EventInputLinked | A link into an input of an event node (they take literals) |
| InvalidName | A name declared in settings (a custom event, its parameters, a `FormatString` placeholder) breaks the name rule: 1 to 64 bytes, no control characters or `.`, no surrounding spaces |
| ReservedName | A function parameter, custom event parameter or placeholder is named `Execute`, `Then`, `Target`, `Self` or `Default` |
| UnknownType | A type ID names no type (`graph.types` lists them) |
| InvalidType | `exec` or `*` for a variable, local or parameter |
| NotExposable | A public variable of a type components cannot store (the types with a `FieldType`), arrays among them: `arrays cannot be set on components yet: keep the variable private and fill it in the graph (Make Array from public variables, or the Children of a child entity that holds the points)` |
| UnknownVariable | A `Variable` setting names no variable (or local) of the graph; with `Graph`, no variable of that graph (`Scripts/Door.sgraph has no variable 5`, the `Suggestions` its variables' IDs), or a library (`... is a function library: it has no variables`) |
| UnknownFunction | A `Function` setting names no function; with `Graph`, no function of that library (`Scripts/Motion.sgraph has no function 9`, the `Suggestions` its functions' IDs) |
| MissingEntry, DuplicateEntry | A function's graph needs exactly one `Function.Entry` |
| MissingReturn, DuplicateReturn | A pure function's graph needs exactly one `Function.Return` |
| ImpureInPureFunction | An impure node in a pure function's graph |
| NotInFunction | `Function.Entry` or `Function.Return` in the event graph |
| LatentInPureContext | A latent node (`Flow.Delay`, ...) or a call of a latent function in a pure function's graph |
| TickInFunction | `Flow.Tween` or `Flow.Interval` in a function's graph |
| PureCycle | Pure nodes that need each other's values |
| PureRecursion | A pure function that calls itself, directly or not |
| InvalidExpression | An `Expression`'s formula does not read or type-check. Its diagnostic names the setting `Formula` and the `Span` of the offending bytes; messages about brackets and `?:` give a line and column (`expected ')' to close the '(' at line 1, column 4`), and an unknown function suggests the nearest ones (`unknown function sni: did you mean sin?`). The node's `MissingSetting` and `InvalidSetting` carry `Setting` `Formula` without a `Span` |
| UnknownGraph | The `Graph` of a call, a Get, a Set or `RunsGraph` names no visual script that loads: `Graph: no visual script N (deleted, or not a visual script)`, or why it is absent |
| UncheckedGraph (W) | The target of a typed Send or Broadcast is missing, so it is not checked: `Graph: no visual script N to check Send Hit against; it sends Hit by name` |
| UnknownEvent (W) | The target of a typed Send or Broadcast has no custom event of its `Name` (`Scripts/Door.sgraph has no custom event Opn`, its `Suggestions` the nearest names), or is a function library (`... is a function library: it has no events`) |
| EventSignatureMismatch (W) | The target's event takes other parameter types: `Door's Hit takes (float Damage); this Send passes (int Damage)` |
| DisabledTarget (W) | The target's custom event is disabled: `Door's Hit is disabled: it receives nothing` |
| TargetIsSelf (W) | In a script, a typed Send whose `Target` is unlinked sends to Self, which runs this script (`Target: Self runs this graph, not Scripts/Door.sgraph: link the entity to send to`), or a `RunsGraph` whose unlinked `Target` makes it always false (`... so this is always false: link the entity to test`) |
| NotALibrary | A call's `Graph` names a script: `<path> is not a function library: only libraries' functions are called from other graphs` |
| ReferenceError | A library the graph calls has errors (`<path> has N errors, first: <message> (node N): fix the library`), or a variable of another graph has a type that is unknown (`Door's Health has the unknown type Game.Hp: fix Scripts/Door.sgraph`) |
| LibraryCycle | Libraries call each other in a cycle: `A.sgraph calls B.sgraph, which calls A.sgraph: libraries cannot depend on themselves` |
| LibraryVariable | A variable of a function library: `<name>: a library has no variables: its functions run for the graph that calls them; use a local or an input` |
| LibraryEventGraph | A node of a function library's event graph: `a library has no event graph: its functions run when graphs call them` |
| PrivateVariable | A Get or Set naming another script's private variable: `Health is private to Scripts/Door.sgraph: make it public there to use it from other graphs` |
| UnlinkedTarget | In a script, a Get or Set naming another script with its `Target` unlinked: `Target: Self runs Scripts/Player.sgraph, not Scripts/Door.sgraph: link the entity to read from` (`write to` on a Set) |

The codes from `UnknownGraph` on are about other graphs; each of their findings carries `Graph` and `Node`, never
`Variable` (but `LibraryVariable`, which names the library's own variable). `NodeUpdated` also reports a call that still
names a library parameter by a former name (`Twice's input X is now Value: the next save stores the new name`, with
`PinRenames` on the node), `InvalidSetting` a `Graph` that is not a handle and a `RunsGraph` naming a library
(`Graph: Motion is a function library: no entity runs it`), and `MissingSetting` a `RunsGraph` without `Graph`.

**Literals** (variables' `Value`, `Inputs`, `Literal`'s `Value`, `Flow.Switch` cases): `bool` true/false; `int` and
`uint` JSON integers; `long` and `ulong` decimal strings (`"-9000000000"`); `float` (up to 3.4028235e38) and `double`
numbers (a float is read as the double its text denotes, narrowed to float; the editor and the VM write every float so
that it reads back exactly); `string` strings; `Strada.Vector2`/`3`/`4` arrays of 2, 3 or 4 numbers; `Strada.Quaternion`
`[x, y, z, w]` (normalized); `Strada.Color` `[r, g, b, a]` (linear); `Strada.Matrix4` 16 numbers, column by column;
`Strada.Entity` `"0"` only; assets (`Strada.Material`, `Strada.Prefab`, ..., `Strada.AssetHandle`) decimal handle
strings (`"0"` for none; `asset.list` gives handles); enums the enumerator's name (`"Space"`), and `[Flags]` enums an
array of names (`[]` for none, `["Read", "Write"]`; a `Flow.Switch` case on such an enum is an array too), or the value
as an integer (a decimal string for 64-bit enums); arrays (`float[]`, `Strada.Vector3[]`, ...) a JSON array of their
elements' literals, at most 4096 (`[1.5, 2]`, `[[0, 0, 0], [1, 0, 0.5]]`; `[]` is empty, and an array is never None).
Other types have no literal, nor do arrays of them: link their inputs.

**Settings that are not literals.** An `Expression`'s `Formula` is the formula's text (at most 4096 bytes; Docs/Scripting.md
describes formulas): every name in it that is not followed by `(` is an input pin of that name, in the order the names
first appear. A `Flow.Timeline`'s `Timeline` is an object (no `Default`; missing, the timeline is 1 s long, does not
loop and has no tracks), written by the editor in this canonical form:

```json
"Settings": { "Timeline": {
	"Length": 2, "Loop": false,
	"Tracks": [
		{ "Name": "Lift", "Type": "float", "Keys": [
			{ "Time": 0, "Value": 0 },
			{ "Time": 1, "Value": 2.5, "Interpolation": "Linear" },
			{ "Time": 2, "Value": 0, "In": 0, "Out": 0 } ] },
		{ "Name": "Tint", "Type": "Strada.Color", "Keys": [ { "Time": 0, "Value": [1, 1, 1, 1] }, { "Time": 2, "Value": [1, 0.25, 0.25, 1] } ] },
		{ "Name": "Step", "Type": "exec", "Keys": [ { "Time": 0.5 }, { "Time": 1.5 } ] }
	] } }
```

`Length` is more than 0 and at most 3600 seconds, `Loop` a bool, and there are at most 32 `Tracks`. A track's `Name` is a
name (`InvalidName`; `ReservedName` for `Execute`, `Then`, `Target`, `Self` and `Default`), unique among the tracks and
none of the node's own pins (Play, PlayFromStart, Reverse, ReverseFromEnd, Stop, SetTime, NewTime, Rate, Update,
Finished, Time, Direction, Playing); its `Type` is `float`, `Strada.Vector2`, `Strada.Vector3`, `Strada.Vector4`,
`Strada.Color` (a curve track: a data output of its type) or `exec` (an event track: an exec output that fires at its
keys); it has at most 1024 `Keys`, in time order. A curve key has a `Time` (0 to `Length`), a `Value` (a literal of the
track's type), an `Interpolation` for the segment to the next key (`Cubic`, the default and left out, `Linear` or
`Constant`) and optional `In` and `Out` slopes (a number, or one per component for vectors and colours; left out: auto); an
event key has a `Time` only. The canonical form writes the members in this order, leaves out `Interpolation` when it is
`Cubic` and the slopes when they are auto, and writes whole numbers without a fraction. What is wrong is an
`InvalidSetting` with the path (`Timeline.Tracks[1].Keys[3].Time: keys are in time order: 0.5 comes after 0.75`,
`Timeline.Length: expected a number greater than 0 and at most 3600`), and a track with a valid name and type keeps its
pins when other parts are wrong. A `Graph` setting (Kind `Graph`, its `Constraint` `Script` or `Library`) is a decimal
handle string of a visual script (`asset.list` gives handles; `graph.edit` takes `asset://` paths).

### graph debugging

**Debugging graphs.** The visual-script debugger stops executions of graphs at breakpoints while the game plays, steps
them with every value visible and continues them, between frames. It attaches when play starts (Play, never Simulate or
`test.run`) and detaches when it stops; the player and exported games never attach one, so breakpoints never reach them.
Its `state` is `Detached` (not playing), `Running`, `Stopped` (an execution is stopped), `Stepping` (a step waits in a
latent node) or `StepEnded` (a step ended its execution); `pauseHeld` says the debugger paused the scene and has not
resumed it. A stopped execution lets the rest of its frame run (other entities, C# scripts, physics); then play pauses at
the frame's end, `play.advance` and `graph.trace` stop after that frame, and no frame runs (`play.step`, `play.advance` and
`graph.trace` are refused with InvalidOperation, naming `graph.continue` or `graph.step`) until it continues. An execution
that `Event.Send` or `Event.Broadcast` started stops with its sender, which waits inside its Send; one that engine code
started inside another execution (`Entity.Instantiate` running an `Event.Create`, a C# script's `SendEvent` from a member
a graph called), and one whose instance was destroyed, cannot stop: it runs on and passes the breakpoint, recorded as a
read-only stop with `reason` `Passed` that keeps a snapshot of its values. `graph.continue`, `graph.step` and
`play.pause {paused: false}` resume between frames and never run a frame; a step that waits in a
latent node completes only when frames run: with the scene paused by the agent (`play.start {paused: true}`) it answers
`needsFrames`, and `play.advance` runs them, stopping early when the step stops. `graph.wait` waits in real time for a
stop and says at once when no frame can run. While an execution is stopped, `play.pause {paused: false}` continues it
and then the game, `play.step` and `play.advance` are refused and `play.stop` ends everything. When the last stopped
execution ends without being continued (an edit cancelled it, its entity was destroyed, another scene loaded, also by
what another stop's resume ran or in the frame that stopped it), the scene stays paused with the pause held, and
`graph.continue` resumes play (`Resumed`). Edits reach stopped executions at once: one that keeps the stopped node
migrates the execution (`migrated` in `graph.debug`, `edited` and `before` on the literals it changed in `graph.values`);
one that removes the node, retypes or disables it, or changes the signature of its function, cancels it with a Warning.
These commands work headless and record no undo step: breakpoints and targets are per-user view state (a headless editor
keeps them in memory), and resuming runs the game. Stops have integer IDs (`graph.debug` lists them); `graph` parameters
are handles or `asset://` paths of visual scripts. Values are `{ name, type }` with one of `value` (the literal; `path`
too for an asset), `entity` (with `entityName` and `valid`; components and script instances have `text` too) and `text`
(what has no literal, and non-finite numbers), plus `length` and `truncated` for a cut string (256 bytes) or array (32
elements, nested two levels); array elements, `before` and a summary's `first`, `last`, `min` and `max` are the literal
itself when it is one and whole, else such an object without `name` and `type`. Errors: InvalidOperation while not
playing or simulating, with nothing to continue or step, and for `graph.trace` while an execution is stopped; Unavailable
without the scripting runtime, or without the debugger (test runs attach none); InvalidParams with `data.path`;
AssetNotFound; EntityNotFound; Cancelled for an abandoned `graph.wait`.

| Command | Parameters | Result |
|---------|-----------|--------|
| `graph.breakpoints` | `graph` (the graph whose breakpoints change; omitted: lists every graph's), `clear` (with `graph` that graph's, without it every graph's), `remove` (node IDs), `add` (`[{ node*, enabled, condition, hitCount }]`, at most 256; merges: for a node that has a breakpoint only the members given change; a new one is enabled with no condition and hit count 0), `breakOnErrors`; applied in that order, after checking everything | `breakpoints` (of `graph`, or every graph's by path: `[{ asset, path, node, function, title, type, enabled, condition, hitCount, reached, hits, active, inactiveReason, checked, error }]`), `breakOnErrors`. A condition is a formula that must be true for the breakpoint to stop (`Health < 0`, `Index == 3`, `State == "Serve"`, `Target != None`), at most 1024 bytes; `hitCount` n lets hits 1 to n - 1 run on. `reached` counts the passes, `hits` those where the condition held, since play started or the condition changed. `inactiveReason`: `not in the running program: the graph has errors`, `the target <entity> was destroyed`, `no entity runs it` (a library's: `no running graph calls this library`; also while not playing), or null. `checked`: the condition was compiled against a running program (true without a condition); `error`: its compile error, or null. InvalidParams: `add` or `remove` without `graph`; `add[i].node` not a node of the stored document; a condition or hit count on a node no flow enters (a pure node: it stops where its value is read); a condition over 1024 bytes or refused by the runtime's check (`line 1, column 1: unknown name angle: did you mean Angle?`); more than 256 in the graph. FileError when the graph does not load |
| `graph.target` | `graph`*, `entity` (an entity that runs it) or `any` (true); with neither it only reads | `asset`, `path`, `entity` (null: any instance), `instances` (`[{ entity, name }]`: the scene's entities that run it, by name); the target chooses which instance's executions the graph's breakpoints stop. InvalidParams: both given; `entity` does not run the graph; a function library (it runs on no entity). EntityNotFound |
| `graph.debug` | – | `attached`, `state` (`Detached`, `Running`, `Stopped`, `Stepping`, `StepEnded`), `pauseHeld` (the debugger paused the scene for a stop and has not resumed it), `current` (a stop ID or null), `stops` (`[{ id, reason (Breakpoint, Step, Break, RunTo, Error, Passed), resumable, entity, name, graph, path, place (Enter, Continue, Error), message, migrated (an edit reached it: it continues with the edited graph), changed ([{ node, name }]: values changed elsewhere while stopped), changedMore, breakpoint ({ asset, path, node } or null), frames (the call stack, innermost first: { kind (Graph, Call, Delivery, Native), entity, name, graph, document, path, function, functionName, node, title, pin, note }; a Native frame has only kind and title), start ({ kind, graph, document, path, node, title, pin, origin }) }]`), `passed` (breakpoint hits beyond the 64 stops kept), `stepping` (`{ stop, action (Into, Over, Out), waitsIn ({ asset, path, function, node, title, entity } or null), needsFrames }` or null), `breakOnErrors`, `targets` (`[{ asset, path, entity }]`), `breakpoints` (those inactive or with an error: `[{ asset, path, node, title, active, inactiveReason, error }]`); read-only |
| `graph.continue` | `stop` (one stop; default every stop), `to` (`{ graph*, node* }`: until that node; with nothing stopped it only arms) | `outcome` (`Stopped`, `Ended`, `Waiting`, `Dismissed` (read-only stops), `Armed`, `Resumed`: nothing was stopped and the pause was held), `failed` (an aborting error ended an execution), `stop` (the stop reached, or null), `paused`, `state` (as `graph.debug`); runs graph code between frames, never a frame, and that code reads the input of that moment (also what `input.set` gave while stopped, which the next game frame sees again: the input frame ends only after a game frame); play resumes only if the debugger paused it. InvalidOperation: nothing stopped, no `to` and no held pause. InvalidParams: `stop` names no stop; `to.node` not a node of `to.graph`; both `stop` and `to` |
| `graph.step` | `kind`* (`Into`, `Over`, `Out`), `stop` (default the current one) | As `graph.continue` (the code a step runs reads input as `graph.continue`'s does), plus `needsFrames` and `hint`: a step that reaches a node answers `Stopped` with the new stop; one waiting in a latent node answers `Waiting` (play resumes if the debugger had paused it); after a step ended its execution (`StepEnded`), `Into` and `Over` arm a break at that graph's next node (`Armed`). `needsFrames` (with the `hint` `the scene is paused: play.advance runs frames`): the scene stays paused (`play.start {paused}`, `play.pause`), so frames must be run. InvalidOperation: nothing to step; `Out` in `StepEnded` (`the execution ended: nothing to step out of`). InvalidParams: `stop` names no stop, or a read-only one |
| `graph.break` | `cancel` (cancels the armed break and a waiting step), `graph` (only that script's executions, or that library's nodes; default any graph) | The state, as `graph.debug`; the first node a targeted execution reaches in a frame that runs stops; it unpauses nothing. InvalidParams: `graph` with `cancel` (`cancel takes no graph: it cancels the armed break and the step`) |
| `graph.wait` | `timeout` (0.1 to 60 real seconds, default 10) | `stopped`, `stop` (as `graph.debug`'s `stops`, or null), `reason` (`Stopped`, `Timeout`, `NotPlaying`, `PlayStopped`, `Paused`), `hint`; asynchronous; answers at once when a stop exists, nothing plays, or no frame can run (the scene is paused and nothing is stopped: `Paused`, with `the scene is paused: play.advance runs frames`, or `graph.continue resumes the game` when the pause is held after a stop ended) |
| `graph.trace` | `frames`* (1 to 600), `timestep` (default 1/60), `entity` (only this instance; default each graph's target), `graphs` (only steps in these documents), `nodes` (only these node IDs of `graphs`), `values` (default true), `summary` (default true), `maxSteps`, `maxValues` (1 to 100000, default 500) | Runs frames as `play.advance` does, recording: `framesRun`, `stopped`, `stop`; with `summary`: `summary: { graphs: [{ asset, path, function, steps: [{ node, title, exit, count, firstFrame, lastFrame }], values: [{ node, pin, direction (In: staged inputs of impure nodes; Out), type, count, first, last, min, max }] }] }` (`min` and `max` for numbers); without: `steps` (`[{ i, frame, execution, entity, asset, path, function, node, title, exit }]` in order) and `values` (`[{ step, node, pin, direction, ...value }]`); frames count from 0, the first frame run; `truncated: { steps, values, hint }` (the hint names the filters that narrow it: `pass graphs and nodes, or entity`). A breakpoint that stops ends the trace after that frame, as does a frame after which the debugger keeps the scene paused (a waiting step ended its execution, or a stop ended in its frame). InvalidOperation while an execution is stopped. InvalidParams: `nodes` without `graphs` (`node IDs repeat across graphs: pass graphs`) |
| `graph.values` | `graph`*, `entity` (default the graph's target, else its only instance), `stop` and `frame` (default 0, the innermost: read the stop's snapshot instead of the live instance), `function` (default 0, the event graph; with a stop, the frame's), `nodes` (IDs; default: with a stop, the frame's node with its inputs and the nodes its execution computed; else none), `variables`, `timelines` (default true), `evaluate` (with `stop`: at most 16 formulas evaluated in the frame's scope, read-only) | `asset`, `path`, `entity`, `name`, `function`, `source` (`Stop`, `Frame`, `LastCall`: a watched function's last call, `None`), `nodes` (`[{ node, title, inputs?, outputs }]`; with a stop, a node its execution did not compute shows the values its links bring to the frame's node), `variables`, `parameters`, `locals`, `loops`, `event`, `evaluated` (`[{ formula, value or error }]`), `timelines` (`[{ node, time, length, playing, reverse }]`, the instance's now), `truncated`; with a stop also `stop` and `frame`, and its values carry `currency` (`Current`: this execution computed it, `Earlier`, `NotComputed`, `Unknown`), `changed` and, for an input a live edit changed, `edited: true` with `before`; read-only. InvalidParams: `stop` names no stop; `frame` out of range or engine code; the frame not of `graph`; `entity` does not run the graph (or is not the frame's); several instances, no `entity` and no target; `function` not in the graph; `nodes[i]` not in the sub-graph; `evaluate` or `frame` without `stop`; a function library without `stop`. EntityNotFound |
| `graph.instances` | `graph`, `entity` (filters), `limit` (1 to 500, default 100), `offset` | `instances` (in creation order: `[{ entity, name, asset, path, state (Idle, Waiting, Stopped), waits: [{ asset, path, function, node, title, kind, remaining?, frames?, index? }], stops: [IDs] }]`), `total`; read-only |
| `graph.live` | `graph` (one graph; default every graph the game loaded or a problem names), `after` (default 0: only receipts and problems with a later serial; pass the last answer's `serial` to read what is new), `edits`, `problems` (default true: false leaves the list out) | `playState` (`play`, `simulate`, `edit`), `paused`, `serial` (the newest event's), `dropped` (runtime problems the runtime could not keep, beyond 512), `graphs` (by path: `[{ asset, path, kind (Script, Library), running (Current: the game runs what is stored, compiled with the libraries and graphs it refers to as they are; LastWorking: an earlier version that worked, the stored one not running (it, or a library it calls, does not compile, or was not sent since); None; a library's is over the graphs that run it: Current when each runs its current version, LastWorking when one runs an earlier one, None when no entity runs a graph compiled with it), generation, runningSince (seconds since the running version started, or null), programRevision (the stored document's, as `graph.get`'s), runningProgramRevision (the same of what runs: equal means the game runs exactly what is stored; null when nothing runs), notRunning ({ nodes, links: [{ from: [node, pin], to: [node, pin] }] }: what the stored document has, in any sub-graph, that the running version has not or holds otherwise; empty when only a graph it refers to changed; null when nothing runs and for a library), instances ([{ entity, name }]: the running instances), users (a library's: [{ asset, path, compiled, runsCurrent, instances }], the graphs whose programs were compiled with it: `compiled` when its working program was compiled with a version of the library, `runsCurrent` when that is the current one, `instances` the entities that run the graph now; a graph runs the library when it is `compiled` and has instances) }]`), `edits` (receipts, oldest first, the last 64: `[{ serial, graph, path, name, cause (the library or other graph whose change made it compile again; null for its own edit), causePath, outcome (Applied, Pending: a running step holds the swap and a deferred Applied follows, KeptLastWorking: it does not compile and the game keeps the last version that worked, NotRunning: it does not compile and nothing runs it), deferred, instances, pending, keptVariables, matchedVariables ([{ name, from }]), resetVariables ([{ name, reason }]: `type changed from <type> to <type>` or `new`), addedVariables, removedVariables, keptStates, resetStates ([{ document, path, function, node, title, reason (Removed, Changed, Disabled) }]), instanceNotes (instances with waits, stopped executions or Timelines, at most 64: [{ entity, name, waitsKept, stopsKept, cancelled: [{ document, path, function, node, title, reason, stopped }], timelines: [{ node, time, clampedFrom }] }]), moreInstances, errors (KeptLastWorking and NotRunning: `<n> error(s); first: <message> (node <id>)`), age (seconds since it was taken) }]`), `problems` (this play session's runtime errors and warnings, one per graph, document, node, entity and severity, by serial: `[{ serial, severity (Error, Warning), graph, path, document, documentPath (the library for a library's node), function, node, title, entity, name, message (the latest), count, calls ([{ document, path, function, node }]: the calls that led there, innermost last), firstAge, lastAge }]`); read-only. Play clears the receipts and problems; after `play.stop` they are the last run's. AssetNotFound or InvalidParams for `graph` |

### prefab

| Command | Parameters | Result |
|---------|-----------|--------|
| `prefab.create` | `entity`*, `path`* (relative to Assets, ending in `.sprefab`) | The prefab asset (`id`, `name`, `type`, `path`, `reference`, `missing`); writes the entity with its descendants; the scene does not change |
| `prefab.instantiate` | `prefab`* (handle or `asset://<path>`), `parent`, `position` ([x, y, z] in world space; default: the prefab's own transform) | `id` of the instance's root; one undo step; the instance's entities get new IDs and a `Prefab` component linking them to the prefab |

### script

The project's C# scripts (`Scripts/<Name>.csproj`, see Docs/Architecture.md section 10.5). The editor builds them by
itself when their sources change and loads the built assembly (hot reload); `script.build` waits for a build.

| Command | Parameters | Result |
|---------|-----------|--------|
| `script.status` | — | `available` (.NET found), `scriptProject`, `building`, `outOfDate`, `loaded`, `classCount`, `lastBuild` (build report or null) |
| `script.classes` | — | `classes`: `[{ name, fields: [{ name, type, default, hidden?, tooltip?, range?, assetType?, enumerators?, flags? }] }]`; Unavailable without .NET |
| `script.create` | `className`* (C# identifier), `folder` (of `Scripts/Source`, relative with forward slashes; created when missing) | `path` (relative to the project directory), `class` (full name); creates the C# project when needed; InvalidParams for folders outside `Scripts/Source`, FileError when another file of the C# project has the name or a source declares the class already |
| `script.build` | — | Build report `{ succeeded, error, loaded, seconds, errors, warnings, diagnostics: [{ severity, file, line, column, code, message }] }`; asynchronous; compile errors give `succeeded: false`; InvalidOperation without a project, scripts or .NET SDK |

### play, input and test

Play mode runs a copy of the edited scene; while it plays, every scene and entity command acts on the copy (with an
undo history of its own) and `play.stop` discards it (the steps of asset files made while playing stay undoable).
`input.set` and `input.release` act on the game's input directly, whether or not the game view has the window's input.
To click a UI button, read its `center` from `ui.layout`, then `input.set` the `mousePosition` there and press and
release `Left` a frame apart. Scene and project file commands (`scene.new`, `scene.open`, `scene.save`,
`project.create`, `project.open`, `project.close`) fail with InvalidOperation while playing.

| Command | Parameters | Result |
|---------|-----------|--------|
| `play.start` | `mode` (`play` (default): scripts, physics, audio; `simulate`: physics only), `paused` (default false: the scene starts, its scripts' `OnCreate` run, and it is paused before its first frame, for sessions that advance it with fixed steps) | Play state; asynchronous: Play builds changed scripts first and fails when they do not build |
| `play.stop` | — | Play state (the copy and every change to it are discarded; steps that edited asset files stay undoable) |
| `play.pause` | `paused` (default true) | Play state; resuming while an execution is stopped at a breakpoint continues every stopped execution first, and the scene stays paused when one stops again (`debug` tells) |
| `play.step` | `frames` (default 1) | Play state; advances the paused scene one frame per editor frame, and a scene those frames load stays paused and runs the frames still asked for, so exactly `frames` frames run; the input `input.set` gave the paused game reaches the first frame stepped; InvalidOperation while an execution is stopped at a breakpoint |
| `play.advance` | `frames`*, `timestep` (seconds, default 1/60) | Play state plus `framesRun`, `stopped` (an execution stopped in the frames run) and `stop` (the stop, as `graph.debug`'s `stops`, or null, also when the stop ended in the frame that made it: its entity was destroyed or another scene loaded, and the scene stays paused with the pause held); runs the frames at once (pause first for deterministic runs: a paused scene stays paused, a scene its frames load included), and the input `input.set` gave before, also to the paused game with editor frames between, reaches the first of them; stops early when the scripts quit and after a frame in which an execution stopped at a breakpoint; InvalidOperation while one is stopped |
| `play.state` | — | `state` (`edit`, `play`, `simulate`), `paused`, `startPending`, `scene`, `frame` and `time` while playing, `tests` (the run's test report or null), `debug` (the visual-script debugger: `state` (`Detached`, `Running`, `Stopped`, `Stepping`, `StepEnded`), `stops`, `current` (the current stop's ID or null), `pauseHeld`) |
| `input.set` | `keys` (name -> down), `mouseButtons` (name -> down), `mousePosition` ([x, y] in game-view pixels), `scroll` | `keys`, `mouseButtons` down and `mousePosition`; key and button names are KeyCode and MouseButton names; pressed and released last one frame. While playing, the input frame is the game's: it ends after each game frame and only then, so a paused game keeps the presses, releases, mouse movement and scroll until its next frame (`play.step`, `play.advance`), however many editor frames come between, and that frame sees them once (held keys and buttons stay down). Graph code that a resume of a stopped execution runs between frames (`graph.continue`, `graph.step`, `play.pause {paused: false}`) reads the input of that moment, so input given while an execution is stopped is read by the resumed code and then seen again by the next game frame. `play.start` begins a clean input frame (held keys and buttons stay down); `play.stop` releases them and begins one |
| `input.release` | — | Same as `input.set`; releases every key and mouse button (gamepads keep their polled state), as the window losing the focus does: a UI button held is not clicked |
| `ui.layout` | `entity` (one UI element) | Where the screen-space UI is in the game view, laid out as the game lays it out to draw it and to find what the mouse is over (the running scene while playing, else the edited one, at the game view's size): `width`, `height`, `pointerOverUI` (an element that takes clicks was under the mouse at the latest update), `elements`: `[{ id, name, canvas, min, max, center, opacity, hitTarget, button, state }]` in drawing order (topmost last). `min`, `max` and `center` are game-view pixels from the top-left corner, as `input.set`'s `mousePosition`; `hitTarget` says whether the element takes clicks, `button` is the button its clicks go to (or null) and `state` (buttons only) is `normal`, `hovered`, `pressed` or `disabled`. InvalidParams when `entity` is not on the screen (outside every canvas, or hidden) |
| `test.run` | `scene` (scene file relative to Assets; default a copy of the edited scene), `timeout` (game seconds, default 60), `timestep` (default 1/60) | `finished`, `timedOut`, `frames`, `passed`, `failed`, `exceptions`, `failures`, `results`: `[{ name, passed, message }]`; asynchronous; plays until the scripts call `TestReporter.Finish`, quit or time out; breakpoints are ignored (no debugger is attached) |

### log and viewport

| Command | Parameters | Result |
|---------|-----------|--------|
| `log.read` | `since` (entry index), `maxCount` (default 500), `minLevel` (`trace`..`critical`) | `entries`: `[{ index, level, logger, message, timestampMs }]`, `next` (pass as `since`) |
| `viewport.screenshot` | `window` (`Editor`, the default: the whole editor window; `SceneView`, `GameView`, `Hierarchy`, `Inspector`, `ContentBrowser`, `Console`, `Debugger` or `Graph`), `graph` (with `window` `Graph` only: the visual script whose window to capture) | `mimeType` (`image/png`), `width`, `height`, `data` (base64), `window`, `rect` (`[x, y, width, height]`: the cut's place in the editor window's image, in pixels; `[0, 0, width, height]` for `Editor`); a window is cut as ImGui drew it in the frame captured, title bar and tab included; asynchronous; Unavailable when headless; InvalidParams (`data.path` `graph`) for `graph` without `window` `Graph` or the reverse; AssetNotFound or InvalidParams for a `graph` that is not a visual script; InvalidOperation when the window is not open, is behind another tab, is in a window of its own (dragged out of the editor: captures read the editor window) or has no area |
| `viewport.camera` | `focalPoint` ([x, y, z]), `distance` (0.05 to 10000), `yaw`, `pitch` (degrees, -89 to 89); all optional | The view of the viewport's camera, which orbits its focal point, after setting the given values: `focalPoint`, `distance`, `yaw`, `pitch`, `position`, `forward` (yaw 0 and pitch 0 look down -Z); not undoable; Unavailable when headless |
| `viewport.frame` | `entities`* | The view after pointing the camera at the entities from its current direction, framing their meshes (canvases and UI elements: their rectangles where the scene view draws the UI; other entities without a mesh: their origins), as the hierarchy's Focus does; not undoable; Unavailable when headless |
| `viewport.overlays` | `grid`, `selectionOutline`, `icons` (light and camera gizmos), `cameras` (frustums), `lights` (directional light rays, point light ranges, spot light cones), `colliders` (with character capsules), `joints` and `volumes` (boxes of local post-process volumes), each `off`, `selected` or `all` (for the selected entities or every entity), `debugDrawing` (lines scripts draw), `canvases` (the UI of each canvas in its entity's plane, with its outline); all optional | What the scene view draws over the scene after setting the given values, in the same fields; changes nothing in the scene; not undoable; Unavailable when headless |
| `viewport.statistics` | — | What the scene view's renderer drew in the last frame it rendered (the scene through the editor camera): `gpuMilliseconds` (GPU time of its latest finished frame; null before one has finished or when the GPU cannot measure time), `drawCalls` (shadow-map draws included), `triangles`, `shadowDrawCalls`, `shadowMapViews`, `shadowsDropped` (shadow-casting lights beyond the shadow-map budget), `lights`, `culledLights` (point and spot lights out of the view's reach), `culled` (submeshes outside the view), `quads` (sprites, text glyphs and UI rectangles); Unavailable when headless or without a GPU |

Renderer settings (shadows, ambient occlusion, bloom, exposure, tonemapping, FXAA) are scene settings: `scene.settings`
reads and changes them under `Renderer`. `PostProcess` components (post-process volumes) change the exposure,
tonemapper, bloom, ambient occlusion, color grading and lens effects where the camera is; the scene settings stay as
they are.

## Example session

```
→ {"jsonrpc":"2.0","id":1,"method":"authenticate","params":{"token":"..."}}
← {"jsonrpc":"2.0","id":1,"result":{"authenticated":true,"version":"0.1.0"}}
→ {"jsonrpc":"2.0","id":2,"method":"entity.create","params":{"name":"Crate","components":{"Mesh":{"Mesh":"builtin://Cube"}}}}
← {"jsonrpc":"2.0","id":2,"result":{"id":"3218792578666045717"}}
→ {"jsonrpc":"2.0","id":3,"method":"component.set","params":{"entity":"3218792578666045717","component":"Transform","fields":{"Translation":[0,1,0]}}}
← {"jsonrpc":"2.0","id":3,"result":{"component":"Transform","fields":{"Translation":[0,1,0],"Rotation":[0,0,0,1],"Scale":[1,1,1]}}}
```

### Debugging a graph

The documented session of `Tests/Data/VisualScripting/Agents/Debugging.json`, which the editor's tests run headless and
through `strada mcp`: after `authenticate` and `project.create`, it makes the Spinner of `graph.edit`'s example with a
mistake (`% 36` for `% 360`: it shakes instead of turning), stops it at a conditional breakpoint while the game is paused,
reads the bug in the stopped frame, fixes it while the execution is stopped, traces the fix and undoes and redoes it
after Stop. The steps run back to back; each answer shows only the members the session checks (the results hold more):

```
→ {"jsonrpc":"2.0","id":3,"method":"graph.create","params":{"path":"Scripts/Spinner.sgraph"}}
← {"jsonrpc":"2.0","id":3,"result":{"path":"Scripts/Spinner.sgraph"}}
→ {"jsonrpc":"2.0","id":4,"method":"graph.edit","params":{"graph":"asset://Scripts/Spinner.sgraph","description":"Spin at Speed degrees per second","operations":[{"op":"AddVariable","name":"Speed","type":"float","value":90.0,"public":true,"tooltip":"Degrees per second"},{"op":"AddVariable","name":"Angle","type":"float"},{"op":"AddNode","type":"Variable.Get","settings":{"Variable":"Speed"},"as":"$speed"},{"op":"AddNode","type":"Variable.Get","settings":{"Variable":"Angle"},"as":"$angle"},{"op":"AddNode","type":"Expression","settings":{"Formula":"(angle + speed * dt) % 36"},"as":"$next"},{"op":"AddNode","type":"Variable.Set","settings":{"Variable":"Angle"},"as":"$store"},{"op":"AddNode","type":"Expression","settings":{"Formula":"vec3(0, a, 0)"},"as":"$euler"},{"op":"AddNode","type":"Set:Strada.TransformComponent.EulerAngles","as":"$turn"},{"op":"Connect","from":["$angle","Value"],"to":["$next","angle"]},{"op":"Connect","from":["$speed","Value"],"to":["$next","speed"]},{"op":"Connect","from":[2,"DeltaTime"],"to":["$next","dt"]},{"op":"Connect","from":["$next","Result"],"to":["$store","Value"]},{"op":"Connect","from":["$store","Value"],"to":["$euler","a"]},{"op":"Connect","from":["$euler","Result"],"to":["$turn","Value"]},{"op":"Connect","from":[2,"Then"],"to":["$store","Execute"]},{"op":"Connect","from":["$store","Then"],"to":["$turn","Execute"]},{"op":"Arrange","scope":"Graph"}]}}
← {"jsonrpc":"2.0","id":4,"result":{"summary":{"errors":0},"labels":{"$next":7,"$store":8,"$turn":10}}}
→ {"jsonrpc":"2.0","id":5,"method":"entity.create","params":{"name":"Spinner","components":{"VisualScript":{"Graph":"asset://Scripts/Spinner.sgraph"}}}}
← {"jsonrpc":"2.0","id":5,"result":{}}
→ {"jsonrpc":"2.0","id":6,"method":"play.start","params":{"paused":true}}
← {"jsonrpc":"2.0","id":6,"result":{"state":"play","paused":true}}
→ {"jsonrpc":"2.0","id":7,"method":"graph.breakpoints","params":{"graph":"asset://Scripts/Spinner.sgraph","add":[{"node":8,"condition":"Angle > 33.5"}]}}
← {"jsonrpc":"2.0","id":7,"result":{"breakpoints":[{"node":8,"title":"Set Angle","condition":"Angle > 33.5","checked":true}]}}
→ {"jsonrpc":"2.0","id":8,"method":"play.advance","params":{"frames":60}}
← {"jsonrpc":"2.0","id":8,"result":{"framesRun":24,"stopped":true,"stop":{"reason":"Breakpoint","frames":[{"node":8,"pin":"Execute"}]}}}
→ {"jsonrpc":"2.0","id":9,"method":"graph.debug","params":{}}
← {"jsonrpc":"2.0","id":9,"result":{"state":"Stopped","stops":[{"id":1,"frames":[{"node":8,"title":"Set Angle","functionName":"Event Graph"}],"start":{"kind":"Event","node":2}}]}}
→ {"jsonrpc":"2.0","id":10,"method":"graph.values","params":{"graph":"asset://Scripts/Spinner.sgraph","stop":1,"nodes":[7]}}
← {"jsonrpc":"2.0","id":10,"result":{"source":"Stop","variables":[{"name":"Speed","value":90},{"name":"Angle","value":34.5}],"nodes":[{"node":7,"outputs":[{"name":"Result","value":34.5,"currency":"Earlier"}]}]}}
→ {"jsonrpc":"2.0","id":11,"method":"graph.step","params":{"kind":"Over"}}
← {"jsonrpc":"2.0","id":11,"result":{"outcome":"Stopped","stop":{"id":2,"frames":[{"node":10}]}}}
→ {"jsonrpc":"2.0","id":12,"method":"graph.values","params":{"graph":"asset://Scripts/Spinner.sgraph","stop":2,"nodes":[7]}}
← {"jsonrpc":"2.0","id":12,"result":{"nodes":[{"node":7,"outputs":[{"name":"Result","value":0,"currency":"Current"}]}],"variables":[{"name":"Angle","value":0}]}}
→ {"jsonrpc":"2.0","id":13,"method":"graph.edit","params":{"graph":"asset://Scripts/Spinner.sgraph","operations":[{"op":"SetFormula","node":7,"formula":"(angle + speed * dt) % 360"}]}}
← {"jsonrpc":"2.0","id":13,"result":{"changed":true,"live":{"edits":[{"outcome":"Applied","instances":1,"instanceNotes":[{"stopsKept":1}]}]}}}
→ {"jsonrpc":"2.0","id":14,"method":"graph.debug","params":{}}
← {"jsonrpc":"2.0","id":14,"result":{"state":"Stopped","stops":[{"id":2,"migrated":true}]}}
→ {"jsonrpc":"2.0","id":15,"method":"graph.breakpoints","params":{"graph":"asset://Scripts/Spinner.sgraph","clear":true}}
← {"jsonrpc":"2.0","id":15,"result":{"breakpoints":[]}}
→ {"jsonrpc":"2.0","id":16,"method":"graph.continue","params":{}}
← {"jsonrpc":"2.0","id":16,"result":{"outcome":"Ended","paused":true}}
→ {"jsonrpc":"2.0","id":17,"method":"play.advance","params":{"frames":30}}
← {"jsonrpc":"2.0","id":17,"result":{"framesRun":30,"stopped":false}}
→ {"jsonrpc":"2.0","id":18,"method":"graph.trace","params":{"frames":10,"graphs":["asset://Scripts/Spinner.sgraph"],"nodes":[8]}}
← {"jsonrpc":"2.0","id":18,"result":{"framesRun":10,"summary":{"graphs":[{"steps":[{"node":8,"exit":"Then","count":10}],"values":[{"node":8,"pin":"Value","direction":"In","count":10,"min":46.5,"max":60}]}]}}}
→ {"jsonrpc":"2.0","id":19,"method":"graph.values","params":{"graph":"asset://Scripts/Spinner.sgraph"}}
← {"jsonrpc":"2.0","id":19,"result":{"variables":[{"name":"Angle","value":60}]}}
→ {"jsonrpc":"2.0","id":20,"method":"play.stop","params":{}}
← {"jsonrpc":"2.0","id":20,"result":{"state":"edit"}}
→ {"jsonrpc":"2.0","id":21,"method":"editor.undo","params":{}}
← {"jsonrpc":"2.0","id":21,"result":{}}
→ {"jsonrpc":"2.0","id":22,"method":"graph.get","params":{"graph":"asset://Scripts/Spinner.sgraph"}}
← {"jsonrpc":"2.0","id":22,"result":{"document":{"Graph":{"Nodes":[{"ID":7,"Settings":{"Formula":"(angle + speed * dt) % 36"}}]}}}}
→ {"jsonrpc":"2.0","id":23,"method":"editor.redo","params":{}}
← {"jsonrpc":"2.0","id":23,"result":{}}
→ {"jsonrpc":"2.0","id":24,"method":"graph.get","params":{"graph":"asset://Scripts/Spinner.sgraph"}}
← {"jsonrpc":"2.0","id":24,"result":{"document":{"Graph":{"Nodes":[{"ID":7,"Settings":{"Formula":"(angle + speed * dt) % 360"}}]}}}}
```
