---
name: strada-make-game
description: How to make or change a game with the Strada editor through its MCP tools (strada mcp) - projects, scenes from entities and components, materials, prefabs, C# scripts, visual scripts (graphs), watching, debugging and fixing graphs while the game plays, play mode and test runs, export. Use when asked to build a game, level or prototype with Strada, or to drive the editor as an agent.
---

# Making a game with the Strada editor

The editor is reached through the `strada` MCP server that the folder's `.mcp.json` registers (`strada mcp`): every
editor automation command is a tool named `domain_action` (`entity.create` is `entity_create`). The tools act on the
editor the user sees, or on a headless one, through the same undo history as the UI: work in small, checked steps. When
no `strada` tools are available, ask the user to start their agent in the folder that holds `.mcp.json` and to allow its
`strada` server.

The guides this skill names lie under `.strada/docs/` in a game project set up for coding agents, and under `Docs/` in
the engine's repository: `Automation.md` is the reference for every command's parameters and results, `Scripting.md`
and `VisualScripting.md` explain C# scripts and visual scripts, and `Components.md` and `ScriptingAPI.md` list every
component and the whole scripting API.

## 1. Orient

- `editor_status`: the open project and scene, unsaved changes, what undo would revert.
- `component_types`: every component with its category (Rendering, Physics, Audio, ...) and its fields (types,
  defaults, ranges, enum values, asset types).
- `asset_list` with `builtIn: true`: built-in meshes (`builtin://Cube`, `Sphere`, `Capsule`, `Cylinder`, `Plane`...),
  materials, textures, fonts and environments (`builtin://DefaultSky`).
- `log_read` whenever something is unexpected: the editor explains failures there.

## 2. Project

- In a game project set up for coding agents, the `strada` server works on the folder's project (its `.mcp.json` names
  it with `--project`): `editor_status` shows it open. Elsewhere, start one:
  - `project_templates` lists the templates: `empty-3d` (a camera, a sun, a sky, a floor and a few shapes),
    `graph-starter` (a small game whose rules are all graphs) and `csharp-starter` (the same game in C#; it needs the
    .NET 10 SDK). `project_create` with `directory` (new or empty), `name` and `template` (default `empty-3d`) makes the
    project and opens it. It writes a `.gitignore` unless `gitIgnore` is false, and sets the project up for coding
    agents unless `agents` is false (its result's `agents` lists the files).
  - `project_examples` lists the example games, and `project_copy_example` with `example` and `directory` (new or
    empty) copies one there and opens the copy: an example is only ever opened as a copy. One that is not `available`
    says why in its `hint`.
  - `project_open` with `path` (a `.sproj`) opens an existing project.
- `project_agents` gives the state of a project's agent files (`AGENTS.md`, `CLAUDE.md`, `.mcp.json`, this skill and the
  guides under `.strada/docs/`). `project_setup_agents` writes or refreshes them only with `confirmed: true`: ask the user
  first, show them what `dryRun: true` answers, and pass `confirmed` only when they agree (`ConsentRequired` without it).
- An editor that `strada mcp` started shows no start screen. To show it to the user, `start_show` with `visible: true`;
  `start_info` tells what it offers (the recent projects, the templates, the examples, the guides and the default
  location), `editor_recent` removes a project from the recent list or clears it, and `editor_dialog` opens the File
  menu's `new-project` or `agent-setup` dialog. While the start screen shows, `viewport_screenshot` captures only the
  `Editor` window.
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
  controller in `Scripting.md` (yaw kept in a field, jump buffer and coyote time for stair edges).
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
  for large surfaces). The engine's repository builds its Showcase sample this way, exhibit by exhibit
  (`Projects/Showcase/Tools/Author.py`).
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
  `Scripting.md` shows the API in use, with examples.
- `script_build` compiles and loads the scripts; fix every diagnostic it returns. `script_classes` lists the classes
  and their fields.
- Attach scripts with a `Script` component: `{"ClassName": "Game.Player", "Fields": {"Speed": {"Type": "Float",
  "Value": 5}}}`. Public fields (and `[SerializeField]` private ones) are stored in the scene.

## 6. Behavior in visual scripts

Visual scripts are graphs of nodes (`.sgraph` assets) that a `VisualScript` component runs on its entity, with or
without C# scripts; they need no C# project, only the scripting runtime (.NET 10).
`VisualScripting.md` explains them with examples; the `graph` section of `Automation.md` describes documents,
`graph_edit`'s operations, literals and every diagnostic code.

**Make and edit graphs with `graph_edit`.**

- `graph_create` with `path` (`Scripts/Spin.sgraph`; `template` `Events` (default: an `Event.Create` and an `Event.Update`
  node), `Empty` or `Library`) returns the graph's `asset` handle.
- Look up what to use; never guess type strings or pin names. `graph_library` lists node kinds, filtered by `query`
  ("rotation", "delay"), `category` (`Flow`, `Math`, `Components/Rigid Body`, `Game`, ...) or `pinType` with `direction`:
  each kind's `Type` (what a node stores), its pins (name, direction, type, default; `exec` for flow) and its settings.
  `graph_types` lists the types (`float`, `Strada.Vector3`, `Strada.RigidBodyComponent`, `float[]`, ...); those with a
  `FieldType` can be public variables.
- `graph_edit` with `graph`, `description` (the undo step's text) and `operations` applies a batch as one undo step. Name
  what you add with `"as": "$label"` and use the labels in later operations; name variables, locals and functions by
  their names (also in `Variable` and `Function` settings); integers are IDs of the document as the call found it (node 2
  is the Events template's `Event.Update`). Add the nodes first and connect them after, so the batch analyzes once for
  the connections. Leave `position` out and finish with `{ "op": "Arrange", "scope": "Graph" }` in a graph you made, or
  `"scope": "Created"` in a user's graph (it places only the new nodes, beside the ones they continue); place nodes
  before moving, aligning or framing them.
- Read the result: `summary` (errors, warnings, new ones), `diagnostics` (each Error must go: a graph with one does not
  run), each diagnostic's `Fixes` (`[{ label, operation }]`: append a fix's `operation` unchanged to the next batch, or
  send an empty batch to get the current ones), `hints` (`NeverRuns`: no flow into an impure node; `Unused`: a pure
  node nobody reads; `UnlinkedInput`: an Expression input read as 0), `results` (`replaced` links a Connect took over)
  and `warnings`. `refuseNewErrors: true` writes nothing when the result has an Error the stored graph did not have,
  and `dryRun: true` answers what the call would do.
- When the user works in the same graph, pass the `programRevision` `graph_get` (or your last edit) returned: the call
  fails with InvalidOperation, writing nothing, when the program changed since, but not for the user's moves, comments or
  arranging. Pass `revision` when the exact bytes matter. Then `graph_get` and edit again.
- `graph_set` stays for generated or wholesale documents (a graph written from scratch by a tool): `Variables` (`ID`,
  `Name`, `Type`, `Value`, `Public`), `Functions`, and in `Graph` the `Nodes` (`ID`, `Type`, `Settings`, `Inputs` for
  the values of unlinked inputs) and `Links` (`{"From": [<node>, "<output>"], "To": [<node>, "<input>"]}`), IDs unique
  across the document, `Position` left out. Check it first with `graph_check` (`document`, and `graph` to find renamed
  variables against the stored one and to check nodes that name the graph itself) until `valid` is true and only
  Warnings remain.
- `graph_arrange` lays out a graph (or `nodes`) as the window's Arrange does, with node sizes as a window draws them;
  its `bounds` is where a comment box framing them goes. `graph_copy` returns the clipboard text of `elements`, which a
  `Paste` operation (`clipboard`, `at`, `labels`) takes into any graph, binding variables and functions by name.
- Attach a script, never a library: `component_add` with `component` `VisualScript` and `fields` `{"Graph": "<asset>",
  "Variables": {"Speed": {"Type": "Float", "Value": 180}}}`: the values of public variables, per entity, in the format of
  Script `Fields` (an entity is `{"Type": "Entity", "Value": "<id>"}`). A `Variables` patch replaces the whole map.
- Show the result: `graph_open` with `graph` opens its window, then `viewport_screenshot` (below).

This batch, the one `VisualScripting.md` and `Automation.md` show, turns its entity at `Speed` degrees per second in a
graph `graph_create` just made from the Events template:

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

Operations used most (`Automation.md` lists all 51 with their fields and results):

| Op | Fields | Does |
|----|--------|------|
| `AddNode` | `type`*, `settings`, `literals`, `position`, `as` | adds a node; a typed Send or Broadcast gets its target event's `Parameters` |
| `Connect` | `from`* `[node, "pin"]`, `to`* | links an output to an input (an exec output and a data input keep one link: `replaced`) |
| `Disconnect` | `from` and/or `to`, or `node` | removes links |
| `SetInput` | `node`*, `pin`*, `value` | the value of an unlinked input (null removes it) |
| `SetSetting` | `node`*, `name`*, `value` | a setting; Count, Cases, Parameters, Name, Formula, Timeline and Graph go to the operations that keep links |
| `Delete` | `elements`*, `bridge` | deletes nodes and comments, keeping the flow through them |
| `AddVariable` | `name`*, `type`*, `value`, `public`, `tooltip`, `range`, `as` | a variable |
| `SetVariable` | `variable`*, `name`, `type`, `value`, `public`, ... | changes a variable (a rename keeps the old name for components) |
| `AddFunction` | `name`*, `pure`, `inputs`, `outputs`, `as`, `entryAs`, `returnAs` | a function with its Entry and Return |
| `Arrange` | `scope` (`Graph`, `Created`, `Nodes`), `nodes`, `extents` | lays nodes out |
| `Fix` | `code`*, `fix`*, and the diagnostic's `node`, `pin`, ... | applies a quick fix: copy it from `Fixes` |
| `Splice` | `node`*, `link`* `{ from, to }`, `input`, `output` | puts a node into a link (a flow chain) |
| `Paste` | `clipboard`*, `function`, `at`, `labels` | pastes `graph_copy`'s text |
| `SetFormula` | `node`*, `formula`* | an Expression's formula; inputs renamed in order keep their links |

Errors and what to do:

| Symptom | What `graph_edit` answers | What to do |
|---------|---------------------------|------------|
| a type string from memory (no signature, a typo) | a unique signature-less match is stored canonically (`warnings`: `node updated`); else `UnknownNodeType` or `AmbiguousNode`, whose `Fixes` are "Change to ..." or "Use ..." | the Fix with the label, or `graph_library` |
| a pin name that is not the node's (`Exec`, `Output`) | InvalidParams `node 12 has no input Exec` with `data.pins` | use a listed name |
| flow linked into data or data into flow | `TypeMismatch` on the link | flow pins are `Execute` and `Then`; Splice puts a node into a chain |
| a Connect that took over an existing link | `replaced: [{ from, to }]` (an exec output and a data input keep one link), and a `NeverRuns` hint on the node that lost its flow | meant for chains; Splice inserts |
| a node added but never run | `hints`: `NeverRuns` | connect flow into its `Execute` |
| an Expression input left unlinked | `hints`: `UnlinkedInput` (`reads dt as 0`) | link it, or store a value with SetInput |
| too few Sequence outputs, a missing Switch case | `node 7 has no output Then3` | SetSetting of `Count` (renumbers), SetSwitchCases |
| a literal of the wrong form | `InvalidValue`, `EntityValue` | `Automation.md`'s literals; entities come from public `Strada.Entity` variables; `asset://` paths are converted |
| a formula mistake | `InvalidExpression` with `Setting`, `Span` and a line and column in the message; for a misspelled function a `Fixes` entry "Use sin" | the Fix, or a corrected formula with SetFormula (inputs renamed in order keep their links) |
| a formula input named like a function (`distance / speed`) | nothing: any name not followed by `(` is an input | – |
| `DuplicateEvent` after adding an `Event.Update` | the diagnostic, `New: true` | connect to the existing event (node 2 of the Events template) |
| impure nodes in a pure function | `ImpureInPureFunction`, with the fix "Make <function> Impure" | the Fix |
| a library function called without `Graph`, or a script's function with one | `UnknownFunction`, `NotALibrary` | `graph_interface`, then Retarget |
| a typed Send whose `Target` is left unlinked | `TargetIsSelf` (a Warning): Self runs this script, not the target | link the entity to send to |
| an edit that breaks other scripts (a removed function, a variable made private, a renamed event) | `referrers` with each node's `problem`, `summary.brokenReferrers` and `staleSenders` | one `graph_edit` per listed script, or `refuseBrokenReferrers` to keep the edit out |
| the user changed the graph meanwhile | InvalidOperation on `programRevision` (the program changed) or `revision` (anything changed) | `graph_get`, then edit again; prefer `programRevision`, which the user's moves leave alone |
| no .NET runtime | Unavailable for the operations that need pins; deletions and measured arranging run degraded | `graph_set` writes links as given |

**Build a game of graphs.** Write graphs that read well and refer to each other safely, as the Prism Break sample of
the engine's repository does (`Projects/PrismBreak/Tools/Author.py`):

- Declare first: one `graph_edit` per graph that adds only its variables (with tooltips, and ranges for numbers), its
  `Event.Custom` nodes with their `Parameters`, and its functions with their inputs and outputs. Then every typed Send,
  remote Get and Set and call of the bodies resolves, even between graphs that refer to each other in cycles. Write a
  graph's functions (and a callee's waiting body) before the regions that call them.
- Then one `graph_edit` per region: its nodes and links, `{ "op": "Arrange", "scope": "Created" }`, `{ "op": "Arrange",
  "scope": "Nodes", "nodes": [...] }` and an `AddComment` with `text`, `elements` and `color`, with
  `refuseNewErrors: true`; require `summary.errors` and `summary.warnings` to be 0 before going on.
- One region per behaviour, its comment box titled "<What>: <how>" or a short noun phrase (at most 72 characters) in its
  role's colour (blue events and input, orange rules, purple effects, green the title, red development only); regions
  stack from top to bottom in reading order, and inside a region the nodes run from left to right. Regions share no nodes
  and no links: give each its own Get nodes and calls. Name events for what happened or what to do and functions as
  verbs; put node comments where the reason is not plain; give every graph a `description` and every public variable a
  tooltip. The engine's tests hold its samples to these rules (every node in exactly one titled box, none closer than
  16 units, no link between boxes, each box arranged).
- Verify at the end: `graph_check` of each graph's `graph_get` document has no diagnostic, `graph_arrange` with
  `dryRun: true` per box changes nothing, and `graph_interface` of each library lists its callers in `usedBy`.

**The language.**

- Graphs hold no entity values: other entities come from public `Strada.Entity` variables (set per component), `Self`,
  an event's `Other` or lookups (`Call:Strada.Entity.FindByName(string)`). The `Target` of an entity's or a component's
  member, left unlinked, is the entity the graph runs on.
- Maths: an `Expression` node with the setting `Formula` (`"(angle + speed * dt) % 360"`, `"vec3(0, a, 0)"`) does what a
  chain of operator nodes does, in one step. Every name not followed by `(` becomes an input pin of that name; unlinked
  inputs are floats. `VisualScripting.md` lists the functions and rules.
- Text and arrays: the `Utilities/String` and `Utilities/Array` nodes (`graph_library` with `category`). Arrays (`float[]`,
  ...) can be private variables, locals and parameters, never public; their literals are JSON arrays; nodes give changed
  copies, never change an array.
- Timelines: a `Flow.Timeline` node's `Timeline` setting is JSON: `{ "Length": 2, "Loop": false, "Tracks": [{ "Name":
  "Lift", "Type": "float", "Keys": [{ "Time": 0, "Value": 0 }, { "Time": 1, "Value": 2.5, "Interpolation": "Linear" }] },
  { "Name": "Step", "Type": "exec", "Keys": [{ "Time": 0.5 }] }] }` (curve tracks `float`, `Strada.Vector2/3/4`,
  `Strada.Color`; `exec` event tracks). Set it with the `SetTimeline` operation (`renames` keeps renamed tracks' links);
  play it from `Event.Create` (`PlayFromStart`) and use its `Update`, `Finished` and track outputs.
- Your own nodes: mark a public member of the game's C# scripts `[VisualNode]` (`Title`, `Category`, `Description`,
  `Keywords`, `Pure`, `FormerNames`), `script_build`, then `graph_library` with `category` `Game` (or the `Category` you
  gave, or a `query` of the member's name): its `nodes` are the members that became nodes and its `notes` say why the
  others did not (a `ref` parameter, a `Task` result, ...). Then use them with `graph_edit` like any node. Members cannot make a graph wait: give graphs a property to wait on.
- Several behaviors: one graph per entity, so put further behaviors on child entities with VisualScript components of
  their own. C# scripts use `VisualScriptComponent` (`SendEvent`, `HasVariable`, `GetVariable<T>`, `SetVariable<T>`,
  `Graph`).

**Graphs that use other graphs.**

- Function libraries hold shared maths and helpers: `graph_create` with `template` `Library`, then `AddFunction` in it.
  Other graphs call a function with a `Function.Call` whose settings are `{ "Graph": "<library>", "Function": <id> }`.
  `Self` inside a library function is the caller's entity. A library has no variables and no event graph, and runs on no
  entity: never attach one.
- Call `graph_interface` on a library or a script before writing a `Function.Call`, a `Variable.Get` or `Set`, or a
  typed `Event.Send` that names it: it gives the IDs and names to store and the events to send. `Graph` settings take
  `asset://` paths in `graph_edit`.
- Custom events: `Event.Send` (`Name`, `Parameters`; link its `Target`) runs the `Event.Custom` of that name in the
  target's graph. With `Graph` it is typed: its `Parameters` are copied from the target's event when left out, and the
  analysis checks it against that event (Warnings only: a typed Send is never required). `Event.Broadcast` raises an event
  on every graph that has it (`Count` says how many received it).
- Variables of another entity's graph: `Variable.Get` and `Set` with `Graph` and a linked `Target` read and write that
  script's public variables; guard them with `RunsGraph`.
- Edits of a graph others use: `graph_edit` and `graph_set` return `referrers`, the nodes of other scripts the edit
  breaks (a function removed, a variable made private, an event renamed or retyped; `Stale` for untyped senders of an
  event name that went). Follow each with one `graph_edit` of that script (a Fix "Use <name>", `Retarget`, or a
  `SetSetting` of `Name`), or pass `refuseBrokenReferrers: true` to keep other scripts working. Renaming a library
  function's parameter keeps callers working; removing one does not. Edit in dependency order: the library first, then
  its callers.

**Test, play and show.**

- Test graphs like scripts: `TestReporter` nodes (`Call:Strada.Testing.TestReporter.Pass(string)`,
  `Fail(string,string)`, `Finish()`) report checks to `test_run`, or a C# test reads the graphs' variables. `log_read`
  shows graphs that do not load (`Visual script '<path>' has 1 error; first: ...`), entities left without theirs
  (`Entity '<name>' runs no visual script: ...`) and errors while running
  (`<graph>: <node> (node <id>) on <entity> (<id>): <message>`).
- While playing, `graph_edit` and `graph_set` change running graphs in their own call (live edit; their result's `live`
  says what the game took, section 7), `entity_get` shows the values the public variables hold, and `component_set` sets
  them in the running graph.
- Nodes can carry a comment (`SetNodeComment`), be collapsed, disabled (`SetEnabled` `Disabled`: passes the flow and
  values through, its errors become warnings) or `DevelopmentOnly` (runs in the editor and test runs, not in exported
  games: debug logging, cheat keys), and have split struct pins (`SplitPin`: `Force.X`, `Force.Y`, `Force.Z`).
- Show graphs to the user and check what they see: `graph_open` with `graph` opens (or focuses) its window in the
  editor, optionally on a function, with nodes selected and framed (`select`), at a `zoom` or `center`, with the problems
  drawer, the find bar, the drawer's Find on the references of a variable, function or custom event (`references`,
  as `graph_find` takes and lists them, so a custom event without `graph` is every script's) or a Timeline's tracks (`timeline`) open, or with a variable, function or event selected
  (`reveal`). A frame later, `graph_editors` reports every window as the user sees it (the view, the selection, errors and
  warnings, notices such as the one for a missing scripting runtime, the LIVE pill while playing), and
  `viewport_screenshot` shows it. `graph_palette` opens the node palette with a `query` (or as if a wire were dragged
  `from` a pin) and lists what it offers, in rank order. `graph_find` searches every script's nodes, values, comments and
  names, or lists the references of a variable, function or custom event across every script, and works without the
  editor's UI. `graph_close` closes windows. The window tools need the editor's window (Unavailable when headless);
  `graph_palette` also needs the scripting runtime.
- In the engine's repository, the graphs the scenes of `Projects/FeatureTest` run, and the `MathLibrary.sgraph` they
  call, use every built-in kind of node and every `[VisualNode]` member of its scripts (`FormatSample.sgraph` is a
  format sample no scene runs): read them for examples.

## 7. Watch, debug and fix graphs while the game plays

- Make the session exact: `play_start` with `paused: true` creates the scene (every `OnCreate` and `Event.Create` run)
  and pauses it before its first frame, and `play_advance` with `frames` runs exactly those frames (1/60 s each;
  `timestep` changes it), so every value read is reproducible.
- Pick the instance: when many entities run the graph (one enemy among many), `graph_target` with `graph` and `entity`
  makes its breakpoints stop that one alone (`any: true`: every instance); with `graph` alone it lists the instances.
- Set breakpoints: `graph_breakpoints` with `graph` and `add: [{ "node": 8, "condition": "Angle > 33.5" }]` (`enabled`,
  `hitCount`; `add` merges with a breakpoint the node has). A condition is a formula over the names the node reads: the
  `Index` and `Element` of the loops around it (innermost first), its function's locals, the outputs of the one event or
  function entry every chain to it starts from (a custom event's parameters, `Other`, `DeltaTime`, a function's inputs)
  and the graph's variables; names with spaces are written with `_` (`Max_Health`); strings and enums compare with
  string literals (`State == "Serve"`), entities with `None`. Read `checked`, `error` and `inactiveReason` in the result;
  a refused condition says what was meant ("unknown name angle: did you mean Angle?"). A pure node's breakpoint stops the
  nodes that read it and takes no condition.
- Run until one stops: `play_advance` stops early after the frame in which an execution stopped (`stopped`, `stop`).
  `graph_wait` waits in real time while the game runs, and answers at once when no frame can run (`reason` `Paused`).
- Read the stop: `graph_debug` gives the state (`Stopped`, `Stepping`, `StepEnded`), each stop's call stack (`frames`,
  innermost first) and `start`; `graph_values` with `graph` and `stop` reads every value of the stopped frame (`nodes`
  adds nodes' values; each value has a `currency`: `Current`, `Earlier`, `NotComputed`, `Unknown`), and `evaluate`
  computes formulas in that frame (`["Health * 2"]`). Without `stop` it reads the live instance. `graph_instances` lists
  the instances with their waits and stops.
- Step: `graph_step` with `kind` `Into`, `Over` or `Out` answers `Stopped` with the next stop. A step that waits in a
  latent node (a Delay) answers `Waiting`, with `needsFrames` while the scene stays paused: run `play_advance`, which
  stops where the step stops. `graph_continue` continues every stop (`stop` one; `to` `{ graph, node }` runs to a node),
  and `graph_break` stops the next node that runs. While an execution is stopped, `play_step`, `play_advance` and
  `graph_trace` are refused: continue or step first.
- Fix it: `graph_edit` while stopped applies in its own call and reaches the stopped execution when it keeps the stopped
  node (`live.edits[].instanceNotes[].stopsKept`, then `migrated` in `graph_debug`; `graph_values` marks a changed literal
  `edited` with its `before`). A receipt's `outcome` is `Applied`, `Pending` (a step holds the swap), `KeptLastWorking`
  (the edit has errors: the game keeps the last version that worked) or `NotRunning`; it lists the variables kept and
  reset and the waits cancelled. An edit that removes the stopped node cancels the execution; the game stays paused and
  `graph_continue` resumes it.
- Verify: `graph_trace` with `frames`, `graphs` and `nodes` runs frames and summarizes which exits each node took how
  often and each pin's first, last, minimum and maximum (`summary: false` lists every step; `truncated.hint` says how to
  narrow it); `graph_values` reads what the instance holds now.
- Errors while running: `graph_live` lists what edits did to the running game and the runtime problems (one per graph,
  node, entity and severity, with a count and the calls that led there); pass the last answer's `serial` as `after` to
  read only what is new. `graph_breakpoints` with `breakOnErrors: true` stops at the next error.
- Hand the editor back clean: `graph_breakpoints` with `clear: true` removes every graph's breakpoints (with `graph`, that
  graph's), then `play_stop`. Edits made while playing stay, and stay undoable.
- Show the user: `graph_open` with `show` (a node) brings a node into view, `graph_debug_panel` opens the Debugger panel
  on a tab, a stop and a frame, and `viewport_screenshot` with `window` (`Graph` with `graph`, `Debugger`, `GameView`)
  captures one window. Test runs (`test_run`), the player and exported games ignore breakpoints.

The documented session (`VisualScripting.md` and `Automation.md` show it too, and the engine's tests run it) makes the
Spinner with a mistake, `% 36` for `% 360` (it shakes instead of turning), finds it with a breakpoint,
fixes it while the execution is stopped, traces the fix, and undoes and redoes it after Stop. Each call is followed by
the members of its result the session checks:

```
graph_create {"path":"Scripts/Spinner.sgraph"}
  → {"path":"Scripts/Spinner.sgraph"}
graph_edit {"graph":"asset://Scripts/Spinner.sgraph","description":"Spin at Speed degrees per second","operations":[{"op":"AddVariable","name":"Speed","type":"float","value":90.0,"public":true,"tooltip":"Degrees per second"},{"op":"AddVariable","name":"Angle","type":"float"},{"op":"AddNode","type":"Variable.Get","settings":{"Variable":"Speed"},"as":"$speed"},{"op":"AddNode","type":"Variable.Get","settings":{"Variable":"Angle"},"as":"$angle"},{"op":"AddNode","type":"Expression","settings":{"Formula":"(angle + speed * dt) % 36"},"as":"$next"},{"op":"AddNode","type":"Variable.Set","settings":{"Variable":"Angle"},"as":"$store"},{"op":"AddNode","type":"Expression","settings":{"Formula":"vec3(0, a, 0)"},"as":"$euler"},{"op":"AddNode","type":"Set:Strada.TransformComponent.EulerAngles","as":"$turn"},{"op":"Connect","from":["$angle","Value"],"to":["$next","angle"]},{"op":"Connect","from":["$speed","Value"],"to":["$next","speed"]},{"op":"Connect","from":[2,"DeltaTime"],"to":["$next","dt"]},{"op":"Connect","from":["$next","Result"],"to":["$store","Value"]},{"op":"Connect","from":["$store","Value"],"to":["$euler","a"]},{"op":"Connect","from":["$euler","Result"],"to":["$turn","Value"]},{"op":"Connect","from":[2,"Then"],"to":["$store","Execute"]},{"op":"Connect","from":["$store","Then"],"to":["$turn","Execute"]},{"op":"Arrange","scope":"Graph"}]}
  → {"summary":{"errors":0},"labels":{"$next":7,"$store":8,"$turn":10}}
entity_create {"name":"Spinner","components":{"VisualScript":{"Graph":"asset://Scripts/Spinner.sgraph"}}}
  → {}
play_start {"paused":true}
  → {"state":"play","paused":true}
graph_breakpoints {"graph":"asset://Scripts/Spinner.sgraph","add":[{"node":8,"condition":"Angle > 33.5"}]}
  → {"breakpoints":[{"node":8,"title":"Set Angle","condition":"Angle > 33.5","checked":true}]}
play_advance {"frames":60}
  → {"framesRun":24,"stopped":true,"stop":{"reason":"Breakpoint","frames":[{"node":8,"pin":"Execute"}]}}
graph_debug {}
  → {"state":"Stopped","stops":[{"id":1,"frames":[{"node":8,"title":"Set Angle","functionName":"Event Graph"}],"start":{"kind":"Event","node":2}}]}
graph_values {"graph":"asset://Scripts/Spinner.sgraph","stop":1,"nodes":[7]}
  → {"source":"Stop","variables":[{"name":"Speed","value":90},{"name":"Angle","value":34.5}],"nodes":[{"node":7,"outputs":[{"name":"Result","value":34.5,"currency":"Earlier"}]}]}
graph_step {"kind":"Over"}
  → {"outcome":"Stopped","stop":{"id":2,"frames":[{"node":10}]}}
graph_values {"graph":"asset://Scripts/Spinner.sgraph","stop":2,"nodes":[7]}
  → {"nodes":[{"node":7,"outputs":[{"name":"Result","value":0,"currency":"Current"}]}],"variables":[{"name":"Angle","value":0}]}
graph_edit {"graph":"asset://Scripts/Spinner.sgraph","operations":[{"op":"SetFormula","node":7,"formula":"(angle + speed * dt) % 360"}]}
  → {"changed":true,"live":{"edits":[{"outcome":"Applied","instances":1,"instanceNotes":[{"stopsKept":1}]}]}}
graph_debug {}
  → {"state":"Stopped","stops":[{"id":2,"migrated":true}]}
graph_breakpoints {"graph":"asset://Scripts/Spinner.sgraph","clear":true}
  → {"breakpoints":[]}
graph_continue {}
  → {"outcome":"Ended","paused":true}
play_advance {"frames":30}
  → {"framesRun":30,"stopped":false}
graph_trace {"frames":10,"graphs":["asset://Scripts/Spinner.sgraph"],"nodes":[8]}
  → {"framesRun":10,"summary":{"graphs":[{"steps":[{"node":8,"exit":"Then","count":10}],"values":[{"node":8,"pin":"Value","direction":"In","count":10,"min":46.5,"max":60}]}]}}
graph_values {"graph":"asset://Scripts/Spinner.sgraph"}
  → {"variables":[{"name":"Angle","value":60}]}
play_stop {}
  → {"state":"edit"}
editor_undo {}
  → {}
graph_get {"graph":"asset://Scripts/Spinner.sgraph"}
  → {"document":{"Graph":{"Nodes":[{"ID":7,"Settings":{"Formula":"(angle + speed * dt) % 36"}}]}}}
editor_redo {}
  → {}
graph_get {"graph":"asset://Scripts/Spinner.sgraph"}
  → {"document":{"Graph":{"Nodes":[{"ID":7,"Settings":{"Formula":"(angle + speed * dt) % 360"}}]}}}
```

## 8. Play and test

- `play_start`, then `play_pause` and `play_advance` with `frames` for deterministic steps; `input_set` presses keys
  and buttons in the game; `play_state` reports the frame and time; `play_stop` discards everything the run changed.
  With `play_start {paused: true}`, an `input_set` press reaches the next `play_advance` or `play_step` frame even with
  other calls between; the frame after it sees the key held, not pressed.
  To click a UI button, read its `center` from `ui_layout` (game-view pixels), `input_set` the `mousePosition` there
  with `mouseButtons` `{"Left": true}`, advance a frame, then release `Left` and advance another; `ui_layout` also
  reports each button's `state`. Prefer a headless editor for this (a windowed editor's game view also follows the
  real mouse).
- Verify behavior with test scripts: `Strada.Testing` (`TestReporter.Run(name, check)`, `Assert`,
  `TestReporter.Finish()`) in a scene, run by `test_run` with `scene` and `timeout`, which returns every check's
  result. The engine repository's `Projects/FeatureTest` shows the pattern.
- Or test with graphs alone, as Prism Break's `Tests.sgraph` does: a `Checks` function library (a `Check(Name, Passed,
  Detail)` that calls `TestReporter.Pass` or `Fail`, and comparisons such as `Near`), one latent function per check that
  acts, notes `Time.Elapsed`, waits with `Flow.WaitUntil` on a formula such as `done || now - start > 2` (what it
  expects, or its time limit), and reports; the test graph's `Event.Create` calls them in order from a `Sequence`, then
  `TestReporter.Finish`. Physical checks place what they test (teleport the ball in front of the target) rather than
  depend on a path.

## 9. Ship

- Ask the user before exporting. Save everything (`scene_save`), then `project_export` with `directory`: it builds the
  scripts and writes a game that runs on its own (with the scripting runtime's API for visual scripts); the result
  names the executable.

## Examples

The engine's repository holds sample games made with these tools. `Projects/Blocks` is a complete game: rules in plain
C# classes that tests can check (with an AI that plays the title screen's demo), a game script that creates its entities
and materials at run time, a HUD and menus on a UI canvas built with `entity_create` (its `Tools/Author.py` shows the
helpers) and brought to life by a script, sounds, and a test scene (`Scenes/Tests.sscene`) whose script checks the
rules, plays the game through its public methods and clicks its buttons.

`Projects/PrismBreak` is a game made of 13 visual scripts and no C#, a neon brick breaker, built entirely through
`graph_edit` by its `Tools/Author.py` (declare first, then one batch per region): its graphs read well by the rules of
section 6, call two function libraries, send typed events and broadcast to each other, read each other's variables
guarded by `RunsGraph`, and its test scene's 29 checks are graphs too. Read it for every feature of the language in use.

## Rules

- Use the tools rather than editing scene, prefab or project files by hand: they keep references, the asset registry
  and the undo history consistent.
- Read every result. Errors say what to fix: `InvalidParams` errors name the offending field in `data.path`.
- `UnsavedChanges` errors protect work: save first, and pass `discardChanges` only when losing the changes is intended.
- Ask the user before deleting assets (`asset_delete`, `asset_delete_folder`), exporting the game, discarding unsaved
  changes or changing anything outside the project. `project_setup_agents` writes the project's agent files only with
  `confirmed: true`, which only the user's agreement allows (`ConsentRequired` without it; `dryRun: true` shows what it
  would change).
