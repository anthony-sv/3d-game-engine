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
- `component_types`: every component with its category (Rendering, Physics, Audio, ...) and its fields (types,
  defaults, ranges, enum values, asset types).
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
  A `Joint` joins a body to another (`ConnectedEntity`) or the world: hinges (doors, pendulums, wheels with a motor),
  ball joints (chains, Newton's cradles), distance, slider and fixed joints; anchors and axes are local to the entity.
  The player (or a walking creature) gets a `CharacterController` instead of a RigidBody and colliders: an upright
  capsule (default radius 0.5, height 2, centered on the entity like `builtin://Capsule`) that its script walks with
  `Move(velocity)` in `OnFixedUpdate`, jumps by setting `Velocity` and checks with `IsGrounded`; it climbs `StepHeight`
  steps and `MaxSlopeAngle` slopes and pushes dynamic bodies. Put the camera on a child entity at eye height; colliders
  of its children (a held item, a reach trigger) are its own parts and never block it. Start from the first-person
  controller in `Docs/Scripting.md` (yaw kept in a field, jump buffer and coyote time for stair edges).
- `scene_hierarchy`, `entity_find` and `entity_get` show what was built. `scene_save` writes the scene (a `path` such as
  `Scenes/Level.sscene`, relative to Assets, the first time; scenes inside Assets become assets).

## 4. Looks and assets

- `material_create` with `path` (`Materials/Name.smat`) and `fields` (`BaseColor`, `Metallic`, `Roughness`,
  `EmissiveColor`, textures...); meshes use it through `Mesh.Materials`.
- `asset_import` copies models (glTF, FBX, OBJ), textures, HDR environments and sounds into the project from absolute
  paths. A model whose data is in other files (a glTF with its `.bin` and textures, an OBJ with its `.mtl`) needs them
  too: with the editor's window, `asset_drop` imports it as a drop from the file manager does, with the files it reads
  (also whole folders, and C# sources into the scripts).
- Models and materials: `polyhaven_search_models` / `polyhaven_import_model` bring CC0 props, furniture, plants and rocks
  (meters, with their materials; give dynamic ones a box or convex mesh collider fitted to the model's bounds), and
  `polyhaven_search_textures` / `polyhaven_import_texture` make a material from a PBR texture set (set its `UVTiling`
  for large surfaces). `Projects/Showcase/Tools/Author.py` builds a whole scene this way, exhibit by exhibit.
- Skies and image-based lighting: `polyhaven_search_hdris` finds CC0 HDRIs ("sunset", "overcast", "studio"),
  `polyhaven_import_hdri` imports one (1k lights well, 4k makes a sharp skybox); set the SkyLight's `Environment` to the
  returned `reference`.
- Effects: a `ParticleSystem` makes fire, smoke, sparks, dust or magic (`component_types` lists its fields; it emits
  along -Z, so `RotationEuler` `[90, 0, 0]` points it up). Additive particles with an `Intensity` above 1 glow with the
  scene's bloom; `Alpha` ones suit smoke. One-shot effects (explosions, hits) are prefabs with `Emitting` off, a `Burst`
  and `DestroyWhenFinished` that scripts instantiate; `ParticleSystemComponent.Emit(count)` bursts from scripts.
- Post-processing: the scene's renderer settings (`scene_settings`, under `Renderer`) set the look everywhere; a
  `PostProcess` component changes it where the camera is. Set each setting together with its `Override` switch (`{"OverrideSaturation": true,
  "Saturation": 0.3}`); `Global` volumes apply everywhere, local ones inside the entity's box (its scale, in meters) and
  fade out over `BlendDistance`; `Priority` orders overlapping volumes and `Weight` (0 to 1) scales one, which scripts
  animate for flashes and transitions.
- HUDs and menus: a root entity with `UICanvas` (the default `ScaleWithScreenSize` fits a 1920 x 1080 design on any
  screen), and children with `UIRect` (`AnchorMin`/`AnchorMax` from (0, 0) at the parent's top-left to (1, 1) at
  its bottom-right, `Pivot`, `Position` and `Size` in canvas units, y down) plus `UIImage` (color or texture,
  `CornerRadius`), `UIText` (wrapped in the rectangle) or `UIButton` (scripts read `WasClicked` or override
  `OnClick(Entity button)` on the button or an ancestor; `Input.IsPointerOverUI` keeps world clicks off the UI). Title
  and menu scenes need no camera: a canvas alone shows over black. The scene view draws each canvas standing on its
  entity's position (`viewport_frame` on an element frames it); `ui_layout` returns where every element is in the
  game view.
- `prefab_create` writes an entity with its children to a `.sprefab`; `prefab_instantiate` places copies.
- Look at the result in a windowed editor: `viewport_frame` points its camera at entities, `viewport_camera` sets the
  view, and `viewport_screenshot` returns an image of the editor. `viewport_overlays` sets what the scene view draws
  over the scene: `colliders` `all` shows every physics shape (check that colliders fit their meshes), and `grid`,
  `icons`, `canvases` and the shapes `off` give clean screenshots. `viewport_statistics` tells what the last frame drew
  (draw calls, triangles, lights, what was culled): check it when a scene grows large.

## 5. Behavior in C#

- `script_create` with `className` adds a class to the project's C# project and returns its file; write the class
  (a `Strada.Script` with `OnCreate`, `OnUpdate(float)`, `OnFixedUpdate(float)`, `OnCollisionEnter(Entity)`...,
  using `Entity`, the component classes, `Input`, `Physics`, `Time`, `Assets`, `SceneManager`, `Application`, `Log`).
  [Docs/Scripting.md](../../../Docs/Scripting.md) shows the API in use, with examples.
- `script_build` compiles and loads the scripts; fix every diagnostic it returns. `script_classes` lists the classes
  and their fields.
- Attach scripts with a `Script` component: `{"ClassName": "Game.Player", "Fields": {"Speed": {"Type": "Float",
  "Value": 5}}}`. Public fields (and `[SerializeField]` private ones) are stored in the scene.

## 6. Play and test

- `play_start`, then `play_pause` and `play_advance` with `frames` for deterministic steps; `input_set` presses keys
  and buttons in the game; `play_state` reports the frame and time; `play_stop` discards everything the run changed.
  To click a UI button, read its `center` from `ui_layout` (game-view pixels), `input_set` the `mousePosition` there
  with `mouseButtons` `{"Left": true}`, advance a frame, then release `Left` and advance another; `ui_layout` also
  reports each button's `state`. Prefer a headless editor for this (a windowed editor's game view also follows the
  real mouse).
- Verify behavior with test scripts: `Strada.Testing` (`TestReporter.Run(name, check)`, `Assert`,
  `TestReporter.Finish()`) in a scene, run by `test_run` with `scene` and `timeout`, which returns every check's
  result. Projects/FeatureTest shows the pattern.

## 7. Ship

- Save everything (`scene_save`), then `project_export` with `directory`: it builds the scripts and writes a game
  that runs on its own; the result names the executable.

## Example

`Projects/Blocks` is a complete game made with these tools: rules in plain C# classes that tests can check, a game
script that creates its entities and materials at run time, screen-space text for the score, a sound, and a test scene
(`Scenes/Tests.sscene`) whose script checks the rules and plays the game through its public methods.

## Rules

- Use the tools rather than editing scene, prefab or project files by hand: they keep references, the asset registry
  and the undo history consistent.
- Read every result. Errors say what to fix: `InvalidParams` errors name the offending field in `data.path`.
- `UnsavedChanges` errors protect work: save first, and pass `discardChanges` only when losing the changes is intended.
