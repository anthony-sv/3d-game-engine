---
name: strada-make-game
description: How to make or change a game with the Strada editor through its MCP tools (strada mcp) - projects, scenes from entities and components, materials, prefabs, C# scripts, play mode and test runs, export. Use when asked to build a game, level or prototype with Strada, or to drive the editor as an agent.
---

# Making a game with the Strada editor

`.mcp.json` registers `strada mcp`: every editor automation command is a tool named `domain_action`
(`entity.create` is `entity_create`). [Docs/Automation.md](../../../Docs/Automation.md) is the reference for every
command's parameters and results. The tools act on the editor the user sees, or on a headless one, through the same
undo history as the UI: work in small, checked steps.

## 1. Orient

- `editor_status`: the open project and scene, unsaved changes, what undo would revert.
- `component_types`: every component with its fields (types, defaults, ranges, enum values, asset types).
- `asset_list` with `builtIn: true`: built-in meshes (`builtin://Cube`, `Sphere`, `Capsule`, `Cylinder`, `Plane`...),
  materials, textures, fonts and environments (`builtin://DefaultSky`).
- `log_read` whenever something is unexpected: the editor explains failures there.

## 2. Project

- `project_create` with `directory` (new or empty) and `name`, or `project_open` with `path` (a `.sproj`).
- New projects start in `Assets/Scenes/Main.sscene`, their start scene. `project_settings` changes the window, the
  physics layers and ignored layer pairs, and the start scene.

## 3. Scenes

- `entity_create` with `name`, `parent` and `components`, such as
  `{"Transform": {"Translation": [0, 1, 0], "RotationEuler": [0, 45, 0]}, "Mesh": {"Mesh": "builtin://Cube"}}`.
- Entities are UUIDs written as strings. Asset fields take `asset://<path in Assets>` or `builtin://<Name>`.
  Rotations are quaternions `[x, y, z, w]`, or `RotationEuler` in degrees on Transform.
- `component_add`, `component_set` (partial patch) and `component_remove` change entities; `entity_reparent`,
  `entity_duplicate` and `entity_delete` change the hierarchy.
- A scene needs a primary `Camera`, light (`DirectionalLight`, a `SkyLight` with an environment) and, for physics, a
  `RigidBody` with a collider on each body (`BoxCollider`, `SphereCollider`, `CapsuleCollider`, `MeshCollider`).
- `scene_hierarchy`, `entity_find` and `entity_get` show what was built. `scene_save` writes the scene (a `path` the
  first time; scenes inside Assets become assets).

## 4. Looks and assets

- `material_create` with `path` (`Materials/Name.smat`) and `fields` (`BaseColor`, `Metallic`, `Roughness`,
  `EmissiveColor`, textures...); meshes use it through `Mesh.Materials`.
- `asset_import` copies models (glTF, FBX, OBJ), textures, HDR environments and sounds into the project from absolute
  paths.
- `prefab_create` writes an entity with its children to a `.sprefab`; `prefab_instantiate` places copies.
- `viewport_screenshot` returns an image of the editor (windowed editors only): look at the result.

## 5. Behavior in C#

- `script_create` with `className` adds a class to the project's C# project and returns its file; write the class
  (a `Strada.Script` with `OnCreate`, `OnUpdate(float)`, `OnFixedUpdate(float)`, `OnCollisionEnter(Entity)`...,
  using `Entity`, the component classes, `Input`, `Physics`, `Time`, `Assets`, `SceneManager`, `Application`, `Log`).
- `script_build` compiles and loads the scripts; fix every diagnostic it returns. `script_classes` lists the classes
  and their fields.
- Attach scripts with a `Script` component: `{"ClassName": "Game.Player", "Fields": {"Speed": {"Type": "Float",
  "Value": 5}}}`. Public fields (and `[SerializeField]` private ones) are stored in the scene.

## 6. Play and test

- `play_start`, then `play_pause` and `play_advance` with `frames` for deterministic steps; `input_set` presses keys
  and buttons in the game; `play_state` reports the frame and time; `play_stop` discards everything the run changed.
- Verify behavior with test scripts: `Strada.Testing` (`TestReporter.Run(name, check)`, `Assert`,
  `TestReporter.Finish()`) in a scene, run by `test_run` with `scene` and `timeout`, which returns every check's
  result. Projects/FeatureTest shows the pattern.

## 7. Ship

- Save everything (`scene_save`), then `project_export` with `directory`: it builds the scripts and writes a game
  that runs on its own; the result names the executable.

## Rules

- Use the tools rather than editing scene, prefab or project files by hand: they keep references, the asset registry
  and the undo history consistent.
- Read every result. Errors say what to fix: `InvalidParams` errors name the offending field in `data.path`.
- `UnsavedChanges` errors protect work: save first, and pass `discardChanges` only when losing the changes is intended.
