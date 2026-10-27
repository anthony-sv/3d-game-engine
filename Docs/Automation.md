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
   changes. The file holds a secret: its directory is user-only (POSIX 0700, files 0600; on Windows the per-user
   application data folder is private by its default ACL). The editor deletes it on exit; files of processes that no
   longer run are stale and removed by scanners.
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
| `scene.new` | `name`, `discardChanges` | `scene` |
| `scene.open` | `path`* (absolute, or relative to Assets), `discardChanges` | `scene`, `warnings` (skipped unknown components/fields) |
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
graph holds. While the running graph has not taken an edit of its document yet (`graph.set` while paused), the read
values are under the names of the variables it runs, and a value set for a variable only the edit has (renamed or
added) reaches it as the graph takes the edit, at its next update.

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
(InvalidOperation otherwise) and are not part of the undo history; handles stay valid when assets move.

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
graph's **document**, the content of its file without the header: `{ "Description", "Variables", "Functions", "Graph" }`
(Docs/Architecture.md section 7.3). Variables, function locals, functions, nodes and comments share one space of IDs (1
to 2147483647). A node is `{ "ID", "Type", "Position"?, "Settings"?, "Inputs"? }`: `Type` is a node kind of
`graph.library` (`Flow.Branch`, `Call:Strada.Input.IsKeyDown(Strada.KeyCode)`), a node without `Position` is laid out
by `graph.set`, `Settings` configure the kind and `Inputs` hold the literals of unconnected inputs. A link is
`{ "From": [<node>, "<output pin>"], "To": [<node>, "<input pin>"] }`, within one graph (the event graph, or one
function's). Documents store no pins: the scripting runtime derives them (`graph.library` lists each kind's, an analysis
each node's).

| Command | Parameters | Result |
|---------|-----------|--------|
| `graph.create` | `path`* (`.sgraph`, relative to Assets), `template` (`Events` (default): an `Event.Create` and an `Event.Update` node; `Empty`) | `asset` (the graph's handle), `path`; not undoable (file creation); InvalidParams for a path that is taken, outside Assets or not `.sgraph` |
| `graph.get` | `graph`* (handle or `"asset://<path>.sgraph"`), `analyze` (default true) | `asset`, `path`, `document`, `analyzed`, `nodes` and `diagnostics` (the analysis, below); FileError when the file does not load |
| `graph.set` | `graph`*, `document`* (strict) | `asset`, `path`, `changed` (false when the document is the stored one: no undo step), `analyzed`, `warnings` (what `graph.set` changed besides the given document), `nodes`, `diagnostics`; saves the file as one undo step that does not mark the scene modified; allowed while playing (below) |
| `graph.check` | `document`*, `graph` (the graph the document is for) | `valid`, `issues` (`[{ path, message }]`: what `graph.set` would refuse), `warnings` (what it would change), `analyzed`, `nodes`, `diagnostics`; writes nothing |
| `graph.library` | `query` (in titles, keywords, categories and types, ignoring case), `category` (`Flow`, `Math`, `Components/Rigid Body`, ...: with the categories below it), `pinType` (a type ID, `exec` for flow), `direction` (with `pinType`: `In` for nodes with an input taking the type, `Out` for nodes with an output giving it; without it either), `limit` (1 to 500, default 50), `offset` | `nodes` (in library order: by category, then title), `total` (matches before paging); each node `{ Type, Title, Subtitle?, Category, Description?, Keywords, Kind, Pure, Latent, Pins, Settings }`, its pins `{ Name, Direction, Type, Label?, Default?, DefaultsToSelf?, Role?, Constraint?, Group?, ElementOf? }` (wildcards have the type `*`); InvalidParams for an unknown `pinType` and for `direction` without one |
| `graph.types` | `query` (in IDs and titles, ignoring case), `kind` (`Exec`, `Bool`, `Integer`, `Float`, `String`, `Vector`, `Quaternion`, `Color`, `Matrix`, `Entity`, `Component`, `Asset`, `Enum`, `Struct`, `Class`, `Array` or `Object`), `limit` (1 to 500, default 50), `offset` | `types` (by ID), `total`; each type `{ ID, Title, Kind, FieldType? (the types public variables can have), AssetType?, Literal? (documents can hold its values), Enumerators? and Flags (enums), Makeable?, Breakable?, Fields? (structs), Base? (classes), Element? (arrays), ImplicitTo }` (`ImplicitTo`: the types its values convert to; every type also converts to `string`) |
| `graph.open` | `graph`*, `function` (0: the event graph, else a function's ID; default the sub-graph shown last), `select` (IDs of nodes and comments of that sub-graph, at most 10000; the first is the primary selection), `frame` (`Keep`, `Selection` (the default with `select`) or `All`), `zoom` (0.1 to 2), `center` (`[x, y]` in canvas units, within ±1e7), `entity` (the context entity), `problems` (opens the problems drawer), `find` (opens the find bar with the text and goes to the first match, which becomes the selection) | `asset`, `path`, `function` (the sub-graph shown), `selection` (the IDs selected), `loaded`, `warnings`; opens the graph's window or brings it to the front, applied at its next draw; InvalidParams with `data.path` `function`, `select[<i>]` or `entity` for what the graph or the scene does not have |
| `graph.close` | `graph` (default: every window) | `closed` (the assets whose windows closed, in tab order); a graph whose window is not open, or that was deleted, is no error |
| `graph.editors` | – | `editors`: the open graph windows in tab order, each `{ asset, path, function, focused, visible, readOnly, loaded, analyzed, errors, warnings, view: { center, zoom, level }, selection: { nodes, comments }, contextEntity, palette: { open, query }, notices, card, toasts, live, save, drawer, find: { open, text, index, count }, bookmarks, minimap, panes: { sidebar, details }, shortcuts }` (below) |
| `graph.palette` | `graph`* (an open window), `query`, `at` (`[x, y]` in canvas units; default the view's centre), `from` (`{ node*, pin*, direction* (In or Out) }`: context mode), `limit` (1 to 200, default 50), `open` (default true; false closes it) | `open`, `query`, `context`, `entries` (`[{ title, subtitle, category, type, settings, connects?, converts? }]` in rank order), `total`; asynchronous: answers once the palette was drawn (below) |
| `graph.find` | `query` or `references` (`{ kind* (Variable, Function or CustomEvent), graph and id (Variable, Function), name (CustomEvent) }`), `graph` (only this script), `limit` (1 to 500, default 50), `offset` | `results` (`[{ asset, path, function, node?, comment?, variable?, field, text }]` in document order, the scripts by path), `total`, `analyzed`, `skipped` (scripts that do not load: `[{ asset, path, error }]`); works headless |

`graph.library` and `graph.types` need the scripting runtime (.NET 10): without it they fail with Unavailable, and
`graph.get`, `graph.set` and `graph.check` answer `analyzed: false` without `nodes` and `diagnostics`. Every graph command
fails with Unavailable without assets and `graph.create` and `graph.set` with InvalidOperation without a project; a
`graph` that is not registered is AssetNotFound, and one that is not a visual script (or, for `graph.set`, has no file
of its own) InvalidParams with `data.path` `"graph"`. `graph.set` fails with FileError when the file cannot be read
(missing) or written.

**Graph windows.** `graph.open`, `graph.close`, `graph.editors` and `graph.palette` drive the editor's graph windows, one
per open visual script (Docs/Architecture.md section 11); without the editor's UI (headless) they fail with Unavailable,
and `graph.open` and `graph.palette` with InvalidOperation while a dialog is open. `graph.open` works while playing and
applies at the window's next draw: `graph.editors` a frame later shows what the user sees, its members as that draw saw
them. A file that is missing or does not load opens on its card (`loaded` false); `function`, `select`, `frame` and
`find` cannot be checked then and are ignored, which `warnings` says. The view parameters combine in a fixed order: the
framing (`frame`, or its default) first, then `zoom` around the framed centre, then `center`, so `{ select, zoom: 1.25 }`
frames the selection at 125 %. Views move smoothly unless the user turned on Reduce Motion, so a view read right after
`graph.open` may still be on its way.

In `graph.editors`, `function` 0 is the event graph; `visible` is false while another tab is in front of the window;
`readOnly` is true for memory assets and files without write permission; `view.level` is the level of detail (`Detail`,
`Compact`, `Overview` or `Far`); `notices` lists, top to bottom, `Degraded` (no scripting runtime), `Normalization`,
`Repaired`, `ReadOnlyMemory`, `ReadOnlyFile`, `NewFunction`, `FirstCanvas` and `GestureHint`; `card` is `None`,
`Missing`, `Unloadable` or `EmptyGraph`; `live` the toolbar's pill (`None`, `Live`, `LastWorking` or `Simulating`);
`save` `Saved` or `NotSaved`; `drawer` `Closed`, `Problems` or `Find`; `bookmarks` the slots stored (1 to 9); the panes
`Hidden`, `Shown`, `AutoHidden` or `Overlay`; `shortcuts` whether the shortcut sheet is open. A window opened since the
last frame reports its state before its first draw: not visible, not focused, nothing selected.

`graph.palette` opens the window's node palette as if the user pressed Tab at `at` and typed `query`, or, with `from`,
released a wire dragged from that pin there (named as the canvas names it: `Hit.Point` for a field of a split pin); it
changes nothing in the document (`graph.set` adds nodes). It answers once the palette was drawn, a frame later (two for a
window `graph.open` opened in the same frame): `entries` are its rows in rank order, at most `limit` (without `query` and
`from`, the recent entries), `total` the rows before the limit, `type` the node type a row adds (empty for actions that
add none), and with `from`, `connects` is the row's pin the wire links to and `converts` whether the link converts its
value. Its checks come in this order: Unavailable without the UI; the asset checks; InvalidOperation when the graph's
window is not open (`graph.open` first), its file does not load, or a dialog is open; InvalidParams for a `from.node`
that is not a node of the sub-graph shown, a `from.pin` that is no pin name and an `at` beyond ±1e7 (`data.path`
`from.node`, `from.pin`, `at`); Unavailable without the scripting runtime (the palette lists the node library); then
InvalidParams (`from.pin`) for a pin the node does not have, and InvalidOperation when the window is not shown (another
tab in front of it).

`graph.find` searches what the find drawer searches: node titles, subtitles, types, comments, pin labels and stored
values (a literal's compact JSON, a `Format`), comment boxes, and the names of variables, locals, functions and custom
events, one result per element and field (ASCII letters in any case). References are the `Variable.Get` and
`Variable.Set` nodes of a variable or local and the `Function.Call` nodes of a function, in their script, and the
`Event.Custom` and `Event.Send` nodes of a custom event's name, in every script. Without the runtime, titles are
derived from the documents and subtitles and pin labels are not searched (`analyzed` false). InvalidParams when both or
neither of `query` and `references` are given, for `references` without what its kind needs and for an `id` the script
does not have (`data.path` `references.id`); AssetNotFound for a `graph` or `references.graph` that is not registered;
FileError when a script it names does not load.

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

**What graph.set changes.** Besides storing the document it is given, `graph.set` reports in `warnings` (and `graph.check`
lists) what it changes:
- A variable whose ID the stored document has under another name keeps that name in its `FormerNames`, so values
  components store under it still apply to the variable (`"variable renamed: <old> → <new> (<old> is kept in
  FormerNames)"`); a variable keeps its 16 most recent former names. A name that is a variable's current name, or a
  former name of another variable, is not kept.
- With the runtime, a node whose `Type` resolved through a redirect (an API member that was renamed or changed its
  signature) or as the only overload of its member with the node's pins is stored under its exact type, its renamed pins'
  links and inputs following (`"node updated: <old> → <new>"`).
- Nodes without a `Position` are placed (not reported): each group of linked nodes below the placed ones, links running
  left to right in columns 320 apart, the nodes of a column 160 apart.

While playing, `graph.set` goes into the play history (dropped at `play.stop`; the file keeps the change), and running
instances of the graph take the edit at their next update: variables keep their values (matched by ID, then by name and
type; others start at their `Value`, else their type's default, not at a value the component stored before they
existed), values set for them since (`component.set`) reach them then, and nodes waiting (a `Flow.Delay`, ...) keep
waiting while they still exist.

**Analysis.** `nodes` lists every node of the document (those of the functions' graphs, then those of the event graph):
`{ ID, Function (0 for the event graph), Type (as stored), Resolved (Exact, Redirect, Unique or Unknown), CanonicalType?
and PinRenames? (Redirect and Unique: [{ Direction, From, To }]), Title, Subtitle?, Kind, Pure, Latent, Pins }`, with the
types the runtime resolved its wildcards to (`*` while open). `diagnostics` are `{ Severity (Error or Warning), Code,
Function, Message, Node?, Pin?, Direction?, Link? ({ From, To }), Variable? (a variable or local ID), Suggestions? }`;
`Suggestions` lists the nearest known names for unknown ones and the candidates of an ambiguous node. A graph with an
Error does not run: the log names its first error (`Visual script '<path>' has 1 error; first: <message> (node <id>)`)
and each entity left without the graph (`Entity '<name>' runs no visual script: <reason>`), and a graph edited while
playing into one with an Error keeps running its last working version. The codes (W: Warning, the others are Errors):

| Code | Meaning |
|------|---------|
| InvalidDocument | The document is not a JSON object, or not text: "the document is not UTF-8 text", "the document has a string or member name that is not text: an escaped surrogate without its pair" (the editor parses what it is given, so its documents are always text) |
| UnknownNodeType | No node kind has the node's `Type` |
| AmbiguousNode | A `Type` without a signature (`Call:<class>.<member>`) fits several overloads of the member; `Suggestions` lists them |
| NodeUpdated (W) | The `Type` resolved through a redirect or as the only overload of its member: `"node updated: <old> → <new>"` (`graph.set` stores the new type) |
| UnknownPin | A link names a pin the node does not have, or links an output as an input |
| UnusedInput (W) | An `Inputs` key names no input of the node |
| MultipleExecLinks | An exec output has more than one link |
| MultipleDataLinks | A data input has more than one link |
| TypeMismatch | A link joins types that do not convert (a `Convert` node converts explicitly), or an exec pin to a data pin |
| UnresolvedWildcard | A wildcard input gets no type: link it, or give it a literal another operand types |
| NoOperator | An `Operator.*` node has no operator for its operands' types |
| NoConversion | A `Convert` node cannot convert between its types |
| InvalidValue | A literal (a variable's `Value`, an `Inputs` member, `Literal`'s `Value`) is not one of its type (below), or a `DefaultsToSelf` input has one |
| EntityValue | An entity literal other than `"0"`: graphs are shared, so entities come from public variables set per component |
| ZeroQuaternion (W) | A quaternion literal of length 0 reads as the identity |
| InvalidSetting | A setting has a value its node kind does not take |
| MissingSetting | A required setting is missing |
| UnknownSetting (W) | A `Settings` key names no setting of the node kind |
| MissingTypeArgument | A generic member's node has no `TypeArguments` |
| InvalidCase | A `Flow.Switch` case is not a literal of the switched type |
| DuplicateEvent | An event the document may have once (`Event.Update`, one `Event.KeyPressed` per key, one `Event.Custom` per name, ...) appears again |
| EventInFunction | An event node in a function's graph |
| EventInputLinked | A link into an input of an event node (they take literals) |
| InvalidName | A name declared in settings (a custom event, its parameters, a `FormatString` placeholder) breaks the name rule: 1 to 64 bytes, no control characters or `.`, no surrounding spaces |
| ReservedName | A function parameter, custom event parameter or placeholder is named `Execute`, `Then`, `Target`, `Self` or `Default` |
| UnknownType | A type ID names no type (`graph.types` lists them) |
| InvalidType | `exec`, `*` or an array type for a variable, local or parameter |
| NotExposable | A public variable of a type components cannot store (the types with a `FieldType`) |
| UnknownVariable | A `Variable` setting names no variable (or local) of the graph |
| UnknownFunction | A `Function` setting names no function |
| MissingEntry, DuplicateEntry | A function's graph needs exactly one `Function.Entry` |
| MissingReturn, DuplicateReturn | A pure function's graph needs exactly one `Function.Return` |
| ImpureInPureFunction | An impure node in a pure function's graph |
| NotInFunction | `Function.Entry` or `Function.Return` in the event graph |
| LatentInPureContext | A latent node (`Flow.Delay`, ...) or a call of a latent function in a pure function's graph |
| TickInFunction | `Flow.Tween` or `Flow.Interval` in a function's graph |
| PureCycle | Pure nodes that need each other's values |
| PureRecursion | A pure function that calls itself, directly or not |

**Literals** (variables' `Value`, `Inputs`, `Literal`'s `Value`, `Flow.Switch` cases): `bool` true/false; `int` and
`uint` JSON integers; `long` and `ulong` decimal strings (`"-9000000000"`); `float` (up to 3.4028235e38) and `double`
numbers; `string` strings; `Strada.Vector2`/`3`/`4` arrays of 2, 3 or 4 numbers; `Strada.Quaternion` `[x, y, z, w]`
(normalized); `Strada.Color` `[r, g, b, a]` (linear); `Strada.Matrix4` 16 numbers, column by column; `Strada.Entity`
`"0"` only; assets (`Strada.Material`, `Strada.Prefab`, ..., `Strada.AssetHandle`) decimal handle strings (`"0"` for
none; `asset.list` gives handles); enums the enumerator's name (`"Space"`), and `[Flags]` enums an array of names
(`[]` for none, `["Read", "Write"]`; a `Flow.Switch` case on such an enum is an array too), or the value as an integer (a
decimal string for 64-bit enums). Other types have no literal: link their inputs.

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
undo history of its own) and `play.stop` discards it. `input.set` and `input.release` act on the game's input directly,
whether or not the game view has the window's input. To click a UI button, read its `center` from `ui.layout`, then
`input.set` the `mousePosition` there and press and release `Left` a frame apart. Scene and project file commands
(`scene.new`, `scene.open`, `scene.save`, `project.create`, `project.open`, `project.close`) fail with InvalidOperation
while playing.

| Command | Parameters | Result |
|---------|-----------|--------|
| `play.start` | `mode` (`play` (default): scripts, physics, audio; `simulate`: physics only) | Play state; asynchronous: Play builds changed scripts first and fails when they do not build |
| `play.stop` | — | Play state (the copy and every change to it are discarded) |
| `play.pause` | `paused` (default true) | Play state |
| `play.step` | `frames` (default 1) | Play state; advances the paused scene one frame per editor frame |
| `play.advance` | `frames`*, `timestep` (seconds, default 1/60) | Play state plus `framesRun`; runs the frames at once (pause first for deterministic runs); stops early when the scripts quit |
| `play.state` | — | `state` (`edit`, `play`, `simulate`), `paused`, `startPending`, `scene`, `frame` and `time` while playing, `tests` (the run's test report or null) |
| `input.set` | `keys` (name -> down), `mouseButtons` (name -> down), `mousePosition` ([x, y] in game-view pixels), `scroll` | `keys`, `mouseButtons` down and `mousePosition`; key and button names are KeyCode and MouseButton names; pressed and released last one frame |
| `input.release` | — | Same as `input.set`; releases every key and mouse button (gamepads keep their polled state), as the window losing the focus does: a UI button held is not clicked |
| `ui.layout` | `entity` (one UI element) | Where the screen-space UI is in the game view, laid out as the game lays it out to draw it and to find what the mouse is over (the running scene while playing, else the edited one, at the game view's size): `width`, `height`, `pointerOverUI` (an element that takes clicks was under the mouse at the latest update), `elements`: `[{ id, name, canvas, min, max, center, opacity, hitTarget, button, state }]` in drawing order (topmost last). `min`, `max` and `center` are game-view pixels from the top-left corner, as `input.set`'s `mousePosition`; `hitTarget` says whether the element takes clicks, `button` is the button its clicks go to (or null) and `state` (buttons only) is `normal`, `hovered`, `pressed` or `disabled`. InvalidParams when `entity` is not on the screen (outside every canvas, or hidden) |
| `test.run` | `scene` (scene file relative to Assets; default a copy of the edited scene), `timeout` (game seconds, default 60), `timestep` (default 1/60) | `finished`, `timedOut`, `frames`, `passed`, `failed`, `exceptions`, `failures`, `results`: `[{ name, passed, message }]`; asynchronous; plays until the scripts call `TestReporter.Finish`, quit or time out |

### log and viewport

| Command | Parameters | Result |
|---------|-----------|--------|
| `log.read` | `since` (entry index), `maxCount` (default 500), `minLevel` (`trace`..`critical`) | `entries`: `[{ index, level, logger, message, timestampMs }]`, `next` (pass as `since`) |
| `viewport.screenshot` | — | `mimeType` (`image/png`), `width`, `height`, `data` (base64); asynchronous; Unavailable when headless |
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
