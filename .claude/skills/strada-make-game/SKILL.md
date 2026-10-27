---
name: strada-make-game
description: How to make or change a game with the Strada editor through its MCP tools (strada mcp) - projects, scenes from entities and components, materials, prefabs, C# scripts, visual scripts (graphs), play mode and test runs, export. Use when asked to build a game, level or prototype with Strada, or to drive the editor as an agent.
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
  (draw calls, triangles, lights, what was culled): check it when a scene grows large. `editor_entity_menu` and
  `inspector_add_component_menu` open the Entity menu and the Add Component menu as users see them (a `submenu` or
  `category` open) and list what they offer.

## 5. Behavior in C#

- `script_create` with `className` adds a class to the project's C# project and returns its file; write the class
  (a `Strada.Script` with `OnCreate`, `OnUpdate(float)`, `OnFixedUpdate(float)`, `OnCollisionEnter(Entity)`...,
  using `Entity`, the component classes, `Input`, `Physics`, `Time`, `Assets`, `SceneManager`, `Application`, `Log`).
  [Docs/Scripting.md](../../../Docs/Scripting.md) shows the API in use, with examples.
- `script_build` compiles and loads the scripts; fix every diagnostic it returns. `script_classes` lists the classes
  and their fields.
- Attach scripts with a `Script` component: `{"ClassName": "Game.Player", "Fields": {"Speed": {"Type": "Float",
  "Value": 5}}}`. Public fields (and `[SerializeField]` private ones) are stored in the scene.

## 6. Behavior in visual scripts

Visual scripts are graphs of nodes (`.sgraph` assets) that a `VisualScript` component runs on its entity, with or
without C# scripts; they need no C# project, only the scripting runtime (.NET 10). The "Visual scripts" section of
[Docs/Scripting.md](../../../Docs/Scripting.md) explains them with examples; the `graph` section of Docs/Automation.md
describes documents, literals and every diagnostic code.

- `graph_create` with `path` (`Scripts/Spin.sgraph`; `template` `Events` (default) or `Empty`) returns the graph's
  `asset` handle.
- Look up what to use; never guess type strings or pin names. `graph_library` lists node kinds, filtered by `query`
  ("rotation", "delay"), `category` (`Flow`, `Math`, `Components/Rigid Body`, ...) or `pinType` with `direction` (nodes
  with an input taking, or an output giving, a type): each kind's `Type` (what a node stores), its pins (name,
  direction, type, default; `exec` for flow) and its settings. `graph_types` lists the types (`float`, `Strada.Vector3`,
  `Strada.RigidBodyComponent`, ...); those with a `FieldType` can be public variables.
- Write the document: `Variables` (`ID`, `Name`, `Type`, `Value`, `Public`), `Functions` if any, and in `Graph` the
  `Nodes` (`ID`, `Type`, `Settings`, `Inputs` for the values of unlinked inputs) and `Links`
  (`{"From": [<node>, "<output>"], "To": [<node>, "<input>"]}`). IDs are unique across the document. Leave `Position`
  out: `graph_set` lays out nodes without one. The `Target` of an entity's or a component's member, left unlinked, is
  the entity the graph runs on. Graphs hold no entity values: other entities come from public `Strada.Entity` variables
  (set per component), `Self`, an event's `Other` or lookups (`Call:Strada.Entity.FindByName(string)`).
- `graph_check` with `document` (and `graph`, to see renamed variables): fix every `issues` entry (structure:
  `graph_set` refuses them) and every Error in `diagnostics` (meaning: a graph with one does not run), until `valid` is
  true and only Warnings remain. Then `graph_set` with `graph` and `document`, and read its `warnings` and
  `diagnostics`. It saves the file as an undo step.
- Attach it: `component_add` with `component` `VisualScript` and `fields` `{"Graph": "<asset>", "Variables": {"Speed":
  {"Type": "Float", "Value": 180}}}`: the values of public variables, per entity, in the format of Script `Fields`
  (an entity is `{"Type": "Entity", "Value": "<id>"}`). A `Variables` patch replaces the whole map.
- One graph per entity: put further behaviors on child entities with VisualScript components of their own. Graphs call
  each other's `Event.Custom` nodes with `Event.Send`; C# scripts use `VisualScriptComponent` (`SendEvent`,
  `HasVariable`, `GetVariable<T>`, `SetVariable<T>`, `Graph`).
- Test graphs like scripts: `TestReporter` nodes (`Call:Strada.Testing.TestReporter.Pass(string)`,
  `Fail(string,string)`, `Finish()`) report checks to `test_run`, or a C# test reads the graphs' variables. `log_read`
  shows graphs that do not load (`Visual script '<path>' has 1 error; first: ...`), entities left without theirs
  (`Entity '<name>' runs no visual script: ...`) and errors while running
  (`<graph>: <node> (node <id>) on <entity> (<id>): <message>`).
- While playing, `graph_set` changes running graphs at their next update (live edit), `entity_get` shows the values the
  public variables hold, and `component_set` sets them in the running graph.
- Nodes can carry `Comment` (a note), `Collapsed`, `Enabled` (`Disabled` passes the flow and values through without
  running the node, which also turns its errors into warnings; `DevelopmentOnly` runs in the editor and test runs but
  not in exported games: debug logging, cheat keys) and `Split` (`{"Inputs": ["Force"]}` gives `Force.X`, `Force.Y`,
  `Force.Z` pins to link or set one by one).
- Show graphs to the user and check what they see: `graph_open` with `graph` opens (or focuses) its window in the
  editor, optionally on a function, with nodes selected and framed (`select`), at a `zoom` or `center`, with the problems
  drawer or the find bar open. A frame later, `graph_editors` reports every window as the user sees it (the view, the
  selection, errors and warnings, notices such as the one for a missing scripting runtime, the LIVE pill while playing),
  and `viewport_screenshot` shows it. `graph_palette` opens the node palette with a `query` (or as if a wire were dragged
  `from` a pin) and lists what it offers, in rank order: what a user typing that would find. `graph_find` searches every
  script's nodes, values, comments and names, or lists the references of a variable, function or custom event (custom
  events across every script), and works without the editor's UI. `graph_close` closes windows. The window tools need
  the editor's window (Unavailable when headless); `graph_palette` also needs the scripting runtime.
- `Projects/FeatureTest/Assets/Scripts/*.sgraph` together use every built-in kind of node.

## 7. Play and test

- `play_start`, then `play_pause` and `play_advance` with `frames` for deterministic steps; `input_set` presses keys
  and buttons in the game; `play_state` reports the frame and time; `play_stop` discards everything the run changed.
  To click a UI button, read its `center` from `ui_layout` (game-view pixels), `input_set` the `mousePosition` there
  with `mouseButtons` `{"Left": true}`, advance a frame, then release `Left` and advance another; `ui_layout` also
  reports each button's `state`. Prefer a headless editor for this (a windowed editor's game view also follows the
  real mouse).
- Verify behavior with test scripts: `Strada.Testing` (`TestReporter.Run(name, check)`, `Assert`,
  `TestReporter.Finish()`) in a scene, run by `test_run` with `scene` and `timeout`, which returns every check's
  result. Projects/FeatureTest shows the pattern.

## 8. Ship

- Save everything (`scene_save`), then `project_export` with `directory`: it builds the scripts and writes a game
  that runs on its own (with the scripting runtime's API for visual scripts); the result names the executable.

## Example

`Projects/Blocks` is a complete game made with these tools: rules in plain C# classes that tests can check (with an AI
that plays the title screen's demo), a game script that creates its entities and materials at run time, a HUD and menus
on a UI canvas built with `entity_create` (its `Tools/Author.py` shows the helpers) and brought to life by a script,
sounds, and a test scene (`Scenes/Tests.sscene`) whose script checks the rules, plays the game through its public
methods and clicks its buttons.

## Rules

- Use the tools rather than editing scene, prefab or project files by hand: they keep references, the asset registry
  and the undo history consistent.
- Read every result. Errors say what to fix: `InvalidParams` errors name the offending field in `data.path`.
- `UnsavedChanges` errors protect work: save first, and pass `discardChanges` only when losing the changes is intended.
