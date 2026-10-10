# Strada Editor Automation

The editor is fully controllable by programs and AI agents through a local JSON-RPC 2.0 endpoint. Every command runs
on the editor's main thread through the same operations and undo history as the UI, so an agent and a human see and
edit exactly the same state. `editor.commands` returns every command with its JSON-schema parameters at runtime;
this document is the reference (a test fails when a registered command is missing here).

## Connecting

1. Start the editor (`StradaEditor`, or `StradaEditor --headless` without a window). It listens on `127.0.0.1` only,
   on a free port (`--automation-port <port>` fixes it; `--no-automation` disables the server).
2. Read the instance file `<user data>/Editor/Instances/<pid>.json` (`%APPDATA%/Strada` on Windows,
   `~/Library/Application Support/Strada` on macOS, `$XDG_DATA_HOME/Strada` or `~/.local/share/Strada` on Linux):

   ```json
   { "Strada": { "Version": 1, "Type": "AutomationInstance" }, "ProcessID": 1234, "Port": 51234,
     "Token": "<64 hex characters>", "Project": "", "Version": "0.1.0" }
   ```

   The file holds a secret: its directory is user-only (POSIX 0700, files 0600; on Windows the per-user application
   data folder is private by its default ACL). The editor deletes it on exit; files of processes that no longer run are
   stale and removed by scanners.
3. Open a TCP connection and authenticate first:

   ```json
   {"jsonrpc": "2.0", "id": 1, "method": "authenticate", "params": {"token": "<token>"}}
   ```

   Anything else before a successful `authenticate`, or a wrong token, is answered with `Unauthenticated` and the
   connection is closed.

## Messages

- One compact JSON document per line (`\n`, an optional `\r` is ignored), UTF-8.
- Requests use named parameters (`"params"` is an object or omitted). Batches (arrays of up to 64 requests) are
  answered with one array; notifications (no `"id"`) get no response.
- Lines longer than 16 MiB close the connection (`MessageTooLarge`); nesting deeper than 64 levels is rejected.
- Up to 8 connections; further ones receive `ServerBusy` and are closed.
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

### project

Project settings are saved to the project file immediately and are not part of the undo history.

| Command | Parameters | Result |
|---------|-----------|--------|
| `project.info` | — | `project` { name, file, assetDirectory } (null when none is open), `settings` (the `.sproj` "Project" object) |
| `project.create` | `directory`* (new or empty), `name`*, `discardChanges` | `project`, `scene` (the start scene `Assets/Scenes/Main.sscene`) |
| `project.open` | `path`* (`.sproj` file), `discardChanges` | `project`, `scene` (its start scene), `warnings` (skipped unknown settings, start scene problems) |
| `project.close` | `discardChanges` | `closed`, `scene` (empty); InvalidOperation without a project |
| `project.settings` | `settings` (partial patch, e.g. `{"Window": {"Width": 1920}, "Physics": {"Layers": ["Default", "Player"]}}`) | `settings`; InvalidOperation without a project |

### scene

| Command | Parameters | Result |
|---------|-----------|--------|
| `scene.new` | `name`, `discardChanges` | `scene` |
| `scene.open` | `path`*, `discardChanges` | `scene`, `warnings` (skipped unknown components/fields) |
| `scene.save` | `path` (required for never-saved scenes) | `scene` |
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
| `component.types` | — | `components`: `[{ Name, Description, Fields: [{ Name, Type, Default, ... }], Core }]` (see below) |
| `component.add` | `entity`*, `component`*, `fields` | `component`, `fields` |
| `component.remove` | `entity`*, `component`* (core components cannot be removed) | `removed` |
| `component.get` | `entity`*, `component`* | `component`, `fields` |
| `component.set` | `entity`*, `component`*, `fields`* (partial patch; invalid values change nothing) | `component`, `fields` |

Field descriptions may also contain `Values` (the names of enum values), `Min`/`Max` (inclusive bounds of numbers and
of every vector component; values outside them are rejected), `Display` (`Color`, `Angle` in degrees or
`MultilineText`), `AssetType` (the type of asset an asset field references) and `Description`.

### asset

Asset parameters accept a handle (decimal string), `"asset://<path in Assets>"` or `"builtin://<name>"`. Paths are
relative to the project's `Assets` directory with forward slashes. Asset file operations need an open project
(InvalidOperation otherwise) and are not part of the undo history; handles stay valid when assets move.

| Command | Parameters | Result |
|---------|-----------|--------|
| `asset.list` | `folder`, `type` (`Scene`/`Prefab`/`Mesh`/`Material`/`Texture`/`Environment`/`AudioClip`/`Font`), `recursive` (default true), `builtIn` | `assets`: `[{ id, name, type, path, reference, missing }]`, `folders` |
| `asset.get` | `asset`* | `{ id, name, type, path, reference, missing }` |
| `asset.select` | `asset`* | the selection: the inspector shows the asset instead of entities (not undoable) |
| `asset.import` | `files`* (absolute paths), `folder` | `assets` (all or nothing; taken names get a number) |
| `asset.refresh` | — | `added`, `missing`, `modified` (assets), `warnings` |
| `asset.move` | `asset`*, `path`* | the moved asset |
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

### script

The project's C# scripts (`Scripts/<Name>.csproj`, see Docs/Architecture.md section 10.5). The editor builds them by
itself when their sources change and loads the built assembly (hot reload); `script.build` waits for a build.

| Command | Parameters | Result |
|---------|-----------|--------|
| `script.status` | — | `available` (.NET found), `scriptProject`, `building`, `outOfDate`, `loaded`, `classCount`, `lastBuild` (build report or null) |
| `script.classes` | — | `classes`: `[{ name, fields: [{ name, type, default, hidden?, tooltip?, range?, assetType?, enumerators?, flags? }] }]`; Unavailable without .NET |
| `script.create` | `className`* (C# identifier) | `path` (relative to the project directory), `class` (full name); creates the C# project when needed |
| `script.build` | — | Build report `{ succeeded, error, loaded, seconds, errors, warnings, diagnostics: [{ severity, file, line, column, code, message }] }`; asynchronous; compile errors give `succeeded: false`; InvalidOperation without a project, scripts or .NET SDK |

### log and viewport

| Command | Parameters | Result |
|---------|-----------|--------|
| `log.read` | `since` (entry index), `maxCount` (default 500), `minLevel` (`trace`..`critical`) | `entries`: `[{ index, level, logger, message, timestampMs }]`, `next` (pass as `since`) |
| `viewport.screenshot` | — | `mimeType` (`image/png`), `width`, `height`, `data` (base64); asynchronous; Unavailable when headless |

Later subsystems add `prefab.*`, `play.*`, `input.*`, `renderer.*` and `test.*` commands (Docs/Architecture.md
section 12); each is added to this reference.

## Example session

```
→ {"jsonrpc":"2.0","id":1,"method":"authenticate","params":{"token":"..."}}
← {"jsonrpc":"2.0","id":1,"result":{"authenticated":true,"version":"0.1.0"}}
→ {"jsonrpc":"2.0","id":2,"method":"entity.create","params":{"name":"Crate","components":{"Mesh":{"Mesh":"builtin://Cube"}}}}
← {"jsonrpc":"2.0","id":2,"result":{"id":"3218792578666045717"}}
→ {"jsonrpc":"2.0","id":3,"method":"component.set","params":{"entity":"3218792578666045717","component":"Transform","fields":{"Translation":[0,1,0]}}}
← {"jsonrpc":"2.0","id":3,"result":{"component":"Transform","fields":{"Translation":[0,1,0],"Rotation":[0,0,0,1],"Scale":[1,1,1]}}}
```
