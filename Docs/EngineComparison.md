# Strada compared with Unreal Engine, Unity, Godot and O3DE

Research snapshot of October 2026, revised November 2026: what the four best-known engines offer, where Strada stands
against them, what it lacks to be a professional engine, how the engines reach their users, and what a realistic
motorcycle game asks of it. [Roadmap.md](Roadmap.md) turns this into a plan. Strada is described as the tree stands
after Front Door, on 2026-11-13: visual scripting (landed on 2026-11-04, at `d130bc1`), the documentation set, and Front
Door's start screen, project templates, examples and agent-ready projects (§2.1); the October snapshot described
`fc3d877` (2026-10-13). The other engines are described as of the versions in §3, and §3.1 lists what was re-checked on
2026-11-07 and what was not.

## 1. Summary

- **Strada is a sound foundation with the breadth of an early engine.** About 58,000 lines of engine code (C++ and
  HLSL), 6,800 of C# scripting API plus 29,000 of the visual-script VM, about 120,000 of editor (77,000 of it the
  visual-script editor) and about 155,000 of tests cover rendering, physics, sound, UI, scripting in C# and in visual
  scripts, an editor and export cleanly. But every 3D game ships with systems Strada does not have yet: skeletal
  animation, compressed and streamed textures, LODs and instancing, temporal anti-aliasing, input actions, an audio
  mixer, a UI widget set and a profiler. Neither visual scripting nor Front Door closed any of those gaps.
- **It is ahead where others are weak.** Every editor operation, graph authoring and debugging included, is an
  authenticated, fuzz-tested automation command behind an MCP server, and every edit of a scene, material or graph is
  one undo step. Unreal (Unreal MCP, experimental in 5.8, without authentication) and Unity (the Unity CLI with an MCP
  mode, in beta and free since July 2026) are building the same thing; Godot leaves it to community add-ons, and
  nothing was found for O3DE. Since Front Door a new project is also set up for agents by default (§4.10). Strada's
  testing culture (fuzzing of every file format and command, golden images, enforced coverage of the scripting API and
  of every graph node) goes further than the test frameworks the four engines give game projects. It runs C# on .NET 10
  CoreCLR, which Unity
  reaches with Unity 7. And it now has visual scripting with a debugger that agents drive; Godot removed its visual
  scripting in 4.0.
- **It cannot be installed yet, but it has a first run.** All four engines ship an editor that people download and
  start (§4.10). Strada is a source build whose editor runs from the build folder, with no installer or release; it has
  been MIT-licensed since `ea34866`, and the maintainer decided on 2026-11-10 that signing and installing are not worth
  it now (Roadmap.md §3.10). Since Front Door an editor started without a project shows a start screen with recent
  projects, templates, examples that open as copies and the guides.
- **Nobody ships a production-grade vehicle simulation.** Chaos Vehicles is still experimental and has no two-wheeler
  balance, Unity's `WheelCollider` and Godot's `VehicleBody3D` are raycast cars, O3DE has none. Every serious racing
  game, including Milestone's MotoGP and Ride on Unreal Engine 5, runs vehicle and tire physics of its own. A motorcycle
  game needs the same on any engine, so Strada's distance to the goal is mostly the general engine gaps above plus a
  vehicle module that would be custom anywhere.
- **Strada Moto needs**, beyond that module: a 1-2 kHz simulation loop decoupled from the frame rate, a motorcycle tire
  model (large camber, toroidal contact, relaxation), a rider control layer that makes counter-steering rideable on a
  gamepad, procedural audio on the audio thread, spline-built tracks with terrain, motion blur and TAA, skeletal
  animation with IK and ragdolls for the rider, telemetry and deterministic replays.
- **Unreal-scale features are not the target.** Virtualized geometry, Lumen-class GI and continent-scale streaming took
  thousands of engineer-years. Godot 4's 3D breadth is a realistic benchmark for "professional"; the roadmap reaches its
  essentials and then specializes in vehicle simulation.

## 2. Strada today

### 2.1 What it has

Line counts are physical lines per directory and file type (C++ and HLSL under `Strada/`, C# under
`Strada-ScriptCore/`, C++ under `StradaEditor/`, C++ and C# under `Tests/`), measured the same way at every commit. On
2026-11-13, before the start screen's code lands, the engine library has 57,700 lines, the C# scripting API and the
visual-script VM 6,800 and 29,000, the editor 118,900 (76,600 of them visual scripting) and the tests 154,400; the
figures in the text are rounded. At `d130bc1` they were 55,600, 6,800 and 29,000, 115,800 (76,600) and 148,200.

| Area | Strada |
|---|---|
| Platforms | Windows, macOS (MoltenVK), Linux; games export for the host platform |
| Rendering | Vulkan 1.3 through NVRHI; forward PBR (metallic-roughness, multiple-scattering compensation), image-based lighting from HDRIs, cascaded shadows with PCSS, point and spot shadows, GTAO, bloom, ACES/AgX/PBR Neutral/Reinhard tonemapping, color grading and lens effects in blended post-process volumes, FXAA, frustum and light culling, GPU timers, picking, signed-distance text, sprites, CPU particles |
| Assets | Static meshes from glTF, FBX and OBJ with materials; PNG/JPG/TGA/BMP textures; HDR environments; fonts; WAV/FLAC/MP3/Ogg clips; prefabs; visual scripts (`.sgraph` version 3: scripts and function libraries; versions 1 and 2 load); persistent handles; refresh when files change |
| Scene | EnTT ECS with 28 components and 201 fields (31 are registered; the other three are the entity's ID, its hierarchy links and its prefab link, which no inspector lists) described once by field tables (serialization, inspector, automation, scripting all derive from them), hierarchies, JSON scenes, prefabs |
| Physics | Jolt 5.6: dynamic, kinematic and static bodies, box/sphere/capsule/mesh colliders, triggers, fixed/hinge/ball/distance/slider joints with limits and motors, character controllers, raycasts, layers |
| Audio | miniaudio: 2D and 3D sources with inverse-distance attenuation, streamed music, a listener |
| UI | Entity-based screen-space UI: canvases with scale modes, anchored rectangles, images with rounded corners, wrapped text, buttons |
| Scripting | C# on .NET 10 (CoreCLR) with hot reload, typed component views, inspector fields, an in-game test framework. Visual scripts in the same runtime: statically typed graphs whose node library holds 92 built-in kinds plus a node for every usable member of the scripting API and every `[VisualNode]` game member; formulas compiled to one VM step; latent flow; typed events across graphs; function libraries; timelines; a debugger; edits that apply to the running game |
| Editor | Docking ImGui editor: hierarchy, generated inspector, content browser with thumbnails, scene and game views, gizmos and overlays, undo/redo for everything, play/simulate/pause/step, game export with a smoke test. A start screen when no project is open (recent projects, a New Project form with a card per template, examples that open as copies, the guides, and the .NET and GPU the machine has) and File > Set Up for Coding Agents.... Graph windows (palette, comments, find, quick fixes, minimap, bookmarks, arranging, a curve editor for timelines), the Visual Script Debugger panel, a game preview in the graph canvas and Game View Beside, window captures, and the `--maximized` and `--window-size` options |
| Automation | 105 JSON-RPC commands over an authenticated local socket (78 plus 27 `graph.*`: 15 for authoring and windows, 12 for debugging and live edits), the `strada` CLI and MCP server (111 tools: the commands plus 6 Poly Haven tools), Poly Haven imports |
| Quality | About 1,960 C++ test cases and 710 C# test methods (counted on 2026-11-13, before the start screen's tests); fuzzing of every file format and automation command (templates, the example catalog, the build-tree marker and the agent kit's files included), graph documents, the views file, `graph.edit` batches, debug sessions and the debug protocol included; a coverage test that builds and runs a graph for every node of the library; enforced feature-test coverage of every component and scripting API member; readability tests of sample graphs; doc tests that hold documented agent sessions verbatim; 4 golden images; the editor's tests in five ctest shards whose partition a test checks; CI on three platforms (8 jobs) |
| Samples | FeatureTest (every component, the whole scripting API, graphs that use every built-in node kind); Blocks (falling blocks in C#); the Showcase (every engine feature in exhibits, among them a garden of chimes played by six graphs); Prism Break (a brick breaker made of 13 graphs and no C#, authored through MCP, whose 29 test checks are graphs too and pass in the exported game) |
| Templates and examples | Three templates that new projects start from: Empty 3D (the editor's default scene), Graph Starter (Orb Run, a game made of visual scripts that needs no .NET SDK) and C# Starter (Orb Run in C#, with one graph that calls a node of its code); the test scenes of the two games pass in the player. Three examples that open as copies and never in place: Prism Break, Blocks and the Showcase, which says which command restores its downloaded assets when they are missing. `template.json` and `catalog.json` are versioned and fuzzed |
| Agent kit | A new project is set up for coding agents by default: a managed block in `AGENTS.md`, `CLAUDE.md`, a `.mcp.json` entry for the engine's `strada` MCP server, the game-making skill and the guides under `.strada/docs/`. Text outside the managed parts is never touched, and an existing project is changed only through a plan the user confirms (error 1010; File > Set Up for Coding Agents...) |
| Documentation | Guides to getting started, the editor, C# scripting, visual scripting and working with agents; the automation reference and the architecture contract; generated references of the components (28, 201 fields) and the scripting API (84 types, 737 members), which the engine's tests fail on when they disagree with it; guide examples that are code the tests run. The README shows the engine in screenshots the engine took of itself (the player's `--screenshot`, the editor's `viewport.screenshot`) |

### 2.2 Limits found in the code

These are the concrete limits behind the gaps of §5, re-checked on 2026-11-13 (the files they cite have not changed
since `d130bc1`); neither visual scripting nor Front Door lifted any of the first eight.

- **No skinning or animation.** `MeshImporter.cpp:551-556` imports skinned meshes in their bind pose with a warning;
  morph targets are skipped (`:595-598`) and FBX animation is ignored (`:1425`). There are no skeletons or clips.
- **Uncompressed textures.** `Renderer.cpp:102-138` uploads every texture as `RGBA8`/`SRGBA8` with mips generated at
  load time, decoded on the main thread and uploaded lazily inside `EndScene`: a 4K texture takes about 85 MB of video
  memory instead of about 21 MB as BC7.
- **One draw call per submesh.** `SceneRenderer.cpp:1556-1588` draws each submesh with its own push constants, which are
  already full at 128 bytes (`RendererInterop.h:117-122`); only debug lines are instanced. There are no LODs, so a track
  lined with trees, barriers and grandstands costs a draw per object at full detail.
- **Every light for every pixel.** `ForwardPBR.hlsl:288` loops over all lights that reach the view (up to 256,
  `RendererInterop.h:14`): no clustered or tiled light lists.
- **Physics for gameplay, not simulation.** `PhysicsScene` offers raycasts only (`PhysicsScene.h:108-111`; no shape
  casts, overlaps or contact data for scripts), one collision step per update (`PhysicsScene.cpp:1578`), and the scene
  caps a frame at 8 fixed steps (`SceneRuntimeSettings::MaxStepsPerFrame`, `Scene.h:66-79`) with steps of at least 1 ms
  (`ProjectSettings.h:84-86`): a 1 kHz loop at 60 fps needs 17.
- **Plain sound.** `AudioScene.cpp:172-175` turns miniaudio's doppler off; each entity has one sound and there is no
  one-shot API (`AudioScene.cpp:64`); there are no buses, effects, snapshots or procedural sources, though miniaudio's
  node graph supports all of them.
- **Raw input.** GLFW keys, mouse and gamepad polling (`Input.h:16-91`, `Window.cpp:86-106`): no action maps, rebinding,
  rumble, haptics or wheels.
- **One scene at a time, loaded synchronously** (`SceneRunner.cpp:114-136`), and no asset packages: exports copy the
  asset directory.
- **Visual scripts' limits.** No public array variables (a public array is `NotExposable`), no maps or sets, no state
  graphs, no macros, interfaces or inheritance, and no latent `[VisualNode]` members. The debugger attaches only in the
  editor. Three performance targets were missed (Architecture.md §15): a watched 2,000-node graph's flow frame takes
  1.75 ms against 1 ms, 200 value pills 1.02-1.12 ms against 0.4 ms, and a new link on a 2,000-node graph 63.2 ms
  against 60 ms.
- **No distribution.** The build tree is the only installation: no `install()` rules, CPack, CI artifacts, tags or
  releases (Strada has been MIT-licensed since `ea34866`). A Windows Release `bin` folder measured at `d130bc1` was
  701 MB in 190 files, of which about 22.9 MB is the product and about 654 MB (624 MiB) is debug symbols, beside the
  test executables. The editor is a console-subsystem executable that imports the MSVC runtime, which the build places
  only in `Redist/`. Positional arguments are ignored (`--project` opens a project) and nothing registers a file
  association; the editor has no macOS app bundle (exported games do); projects record no engine version; every
  exported game shares one "Strada" user-data folder (First Ride's E11 gives each game its own). Front Door added a
  first run that works from the build folder (§2.1) and `EngineLayout`, which finds the engine's templates, examples,
  guides and agent kit from any of its executables (in a build tree through the marker `engine.json`, which CMake
  writes), so an installer would only have to produce that layout. The maintainer cut the installer, signing and
  versioned engine lines on 2026-11-10 (Roadmap.md §3.10).

### 2.3 Where it is ahead

- **Agent control.** Complete (every editor operation, graphs included), authenticated and fuzzed, and every edit of a
  scene, material or graph is one undo step. Agents author, arrange, debug and live-fix graphs through 27 `graph.*`
  commands; Prism Break's 13 graphs were all authored that way. A new project is set up for agents by default (§4.10),
  and an existing one only through a plan the user confirms. `graph.edit` applies up to 10,000 operations
  of 51 kinds as one undo step, with dry runs and revision checks, so agents and users can edit the same graph. CI runs
  a 22-step debugging session through `strada mcp`, and AGENTS.md requires every new graph-editing operation to ship
  with its `graph.edit` operation, fuzz entries and documentation. By comparison:
  - Unreal 5.8's Unreal MCP (experimental) runs inside the editor over HTTP on 127.0.0.1, is off by default, has "no
    authentication layer" by Epic's own documentation, and exposes the tools its plugins register.
  - Unity moved its agent path to the Unity CLI (beta since 2026-07-15, free). `unity mcp` bridges agents to running
    editors with per-instance tokens, the design `strada mcp` already has, and Unity reports that about 98% of CLI runs
    come from coding agents and automation. The earlier MCP server in the Unity AI package is deprecated.
  - Godot has community add-ons; nothing was found for O3DE.

  `strada mcp` speaks the MCP revisions 2024-11-05 to 2025-11-25; the current revision is 2026-07-28.
- **Verification.** Fuzz tests for every format users write and every command, golden images, determinism checks of
  sample games at several frame rates, and CI that fails when a component, a scripting API member or a graph node is not
  exercised. Sample graphs must analyze cleanly, resolve their nodes exactly or through a redirect, and read well
  (titled, arranged comment boxes); documented agent sessions run in CI.
- **Visual scripting with a debugger agents can drive.** Conditional breakpoints written as formulas, hit counts, break
  on errors, stepping into, over and out across graphs and event sends, a call stack across function libraries, values
  for every frame, and live edits that reach the running game within the edit's own call, with receipts. That is deeper
  than the debuggers Unity, O3DE and Unreal document (§4.7.1), and all of it is reachable through MCP.
- **Modern C#.** .NET 10 CoreCLR in-process with collectible hot reload, and graphs that hot-swap while carrying their
  state. Unity reaches .NET 10 with Unity 7 (2027); Godot 4.8's .NET build requires .NET 10 (both from the October
  research).
- **A small engine library.** The engine library stays small (58,000 lines) and builds from pinned, permissively
  licensed sources with CMake on three platforms; neither visual scripting nor Front Door added a dependency. The editor
  (about 120,000 lines, 77,000 of them visual scripting) and the tests (about 155,000) are large.

## 3. The engines compared

| Engine | Version compared | Recent and next releases | Model and license | Re-checked on 2026-11-07 |
|---|---|---|---|---|
| Unreal Engine | 5.8 (17 Jun 2026; hotfix 5.8.2) | 5.6 (Jun 2025), 5.7 (Nov 2025); 5.8 is the last planned UE5 major release; UE6 (Verse and Scene Graph, merged with UEFN) targets early access around late 2027 | Source-available under Epic's EULA; 5% royalty above $1M per product (3.5% with Launch Everywhere with Epic) | 5.8's release on 2026-06-17 through the launcher, GitHub and the Linux page; not the hotfix, UE6's plans or the license terms |
| Unity | 6.6 (Sep 2026); 6.3 LTS current | 6.4 (Mar 2026), 6.5 (Jun 2026); 6.7 LTS due Q4 2026; Unity 7 (one render pipeline, CoreCLR, .NET 10) previews Dec 2026 | Proprietary, per-seat subscriptions (Personal free under $200k); the runtime fee was cancelled in 2024 | Unity's releases page still listed 6.3 as the latest LTS (possibly cached), and 6.7 LTS was not found released; Hub 4.0 in beta since 2026-10-14; the Unity CLI in beta since 2026-07-15; not Unity 7's schedule or the pricing |
| Godot | 4.7.2 (18 Aug 2026) | 4.5 (Sep 2025), 4.6 (Jan 2026), 4.7 (Jun 2026); 4.8 in beta | MIT | 4.7.2's date; 4.8 still in beta (beta 1 on 2026-10-15, beta 2 reported in early November) |
| O3DE | 26.05 (May 2026) | 25.05, 25.10; two releases a year | Apache-2.0 or MIT, Linux Foundation | The 26.05 installer and its Project Manager, on O3DE's download and setup pages |

Versions and features were checked through web searches of official release notes and press coverage in October 2026
(direct page fetches were unavailable then); claims that could not be confirmed are marked *(unverified)*. Long-standing
features are stated from established documentation. In November 2026 page fetches worked, and the parts listed in §3.1
were re-checked.

### 3.1 What was re-checked in November 2026

| Part | Re-checked on 2026-11-07 (Strada's cells again on 2026-11-13) | From October, not re-checked |
|---|---|---|
| §2 and every Strada cell | All of it, from the repository as Front Door leaves it (first at `d130bc1`): counts, limits with their evidence lines. The build folder's sizes (§2.2, §4.10) are the `d130bc1` measurement | – |
| §3 | The last column of the table | The rest of the table |
| §4.1-§4.6, §4.9 | – | Every competitor cell |
| §4.7 and §4.7.1, scripting | The vendors' debugger and live-edit documentation | Cells marked *(unverified)* and the other §4.7 rows |
| §4.8, tooling | The AI agents row and the new "Getting the editor" row | The other competitor cells |
| §4.10, distribution | All of it (new); Strada's column again on 2026-11-13 | – |
| §6, the motorcycle game | Jolt's version (§6.8) | Everything else |
| §7, libraries | Jolt, Tracy, ImPlot, ozz-animation, enkiTS, SDL3, KTX-Software; the distribution rows (new) | The other rows |

## 4. Feature comparison

✅ production feature · 🟡 partial, experimental or through official add-ons · ❌ missing (community add-ons at most)

Competitor cells come from the October 2026 research except where §3.1 lists them as re-checked.

### 4.1 Rendering

| Feature | Unreal 5.8 | Unity 6.6 | Godot 4.7 | O3DE 26.05 | Strada |
|---|---|---|---|---|---|
| Pipeline | ✅ Deferred, forward option, GPU-driven | ✅ URP Forward+/Deferred on a render graph (HDRP maintained) | ✅ Clustered Forward+, Mobile, Compatibility | ✅ Clustered Forward+, deferred option | 🟡 Forward, all lights per pixel, no render graph |
| Global illumination | ✅ Lumen, Lumen Lite | ✅ Lightmaps, Adaptive Probe Volumes; 🟡 Surface Cache GI (6.7) | ✅ SDFGI, VoxelGI, LightmapGI, SSIL | ✅ DiffuseProbeGrid (needs RT) | ❌ IBL and GTAO only |
| Reflections | ✅ Lumen, SSR, captures, RT | ✅ Probes; SSR and RT in HDRP | ✅ SSR, probes | ✅ SSR, probes | ❌ Sky IBL only |
| Many lights | ✅ MegaLights | ✅ Forward+ | ✅ Clustered | ✅ Clustered | 🟡 Up to 256, unclustered |
| Shadows | ✅ Virtual shadow maps | ✅ Cascades; contact and RT in HDRP | ✅ PSSM, atlas; 🟡 contact (4.8) | ✅ Cascades | ✅ 4 cascades with PCSS, 24 local slices |
| Geometry at scale | ✅ Nanite (incl. foliage, skinning) | ✅ GPU Resident Drawer, GPU occlusion, mesh LOD | ✅ Auto LOD, occlusion culling | ✅ Occlusion culling | ❌ Frustum culling only: no LODs, no instancing |
| Anti-aliasing, upscaling | ✅ TSR, TAA; DLSS/FSR/XeSS plugins | ✅ STP, TAA, SMAA; DLSS 4/FSR 2 in HDRP | ✅ TAA, FSR 2.2, MetalFX, SMAA | *(unverified)* | 🟡 FXAA |
| Ray tracing, path tracer | ✅ ✅ | 🟡 HDRP only | ❌ | 🟡 GI probes, groundwork | ❌ |
| Sky, atmosphere, clouds, fog | ✅ SkyAtmosphere, volumetric clouds and fog | ✅ in HDRP; 🟡 URP fog during 7.x | 🟡 Physical sky, volumetric fog, no clouds | 🟡 Sky Atmosphere | ❌ HDRI or gradient sky |
| Materials | ✅ Material Editor, Substrate | ✅ Shader Graph | ✅ Shading language, visual shaders | ✅ Material Canvas | ❌ One fixed metallic-roughness model, no custom shaders |
| Decals | ✅ | ✅ | ✅ | ✅ | ❌ |
| Textures | ✅ BCn/ASTC, Oodle, virtual texturing | ✅ BCn/ASTC/Crunch, mip streaming | ✅ BCn/ASTC/Basis; 🟡 streaming (4.8) | ✅ Per-platform presets | ❌ Uncompressed RGBA8 |
| Particles | ✅ Niagara (GPU, fluids) | ✅ VFX Graph, Shuriken | ✅ GPU particles, trails (4.8) | 🟡 PopcornFX, preview Open Particle System | 🟡 CPU sprites |
| HDR output | ✅ | ✅ | ✅ (4.7) | *(unverified)* | ❌ |
| Post-processing | ✅ | ✅ | ✅ | ✅ | ✅ Bloom, GTAO, four tonemappers, grading, lens effects, blended volumes; ❌ depth of field, motion blur |

### 4.2 World building

| Feature | Unreal | Unity | Godot | O3DE | Strada |
|---|---|---|---|---|---|
| Terrain | ✅ Landscape (layers, splines), 🟡 Mesh Terrain | ✅ Terrain and Terrain Tools | ❌ Add-ons (Terrain3D) | ✅ Terrain gem | ❌ |
| Foliage, procedural placement | ✅ Foliage, PCG framework | 🟡 Terrain trees and details | 🟡 MultiMesh | ✅ Dynamic Vegetation | ❌ |
| Splines | ✅ Spline components, landscape splines | ✅ Splines package | ✅ Path3D, CSG | ✅ | ❌ |
| Streaming, sub-levels | ✅ World Partition, level instances, HLOD | ✅ Additive scenes, Addressables | 🟡 Threaded loading by hand | 🟡 *(unverified)* | ❌ One scene at a time |
| Large worlds | ✅ Double precision | ❌ Floating origin by hand | 🟡 Double-precision builds | *(unverified)* | ❌ Single precision (Jolt can build with doubles) |
| Prefabs | ✅ Blueprints, packed level actors | ✅ Nested prefabs, variants | ✅ Inherited scenes | ✅ Nested prefabs | 🟡 Prefabs without variants or propagation |

### 4.3 Animation

| Feature | Unreal | Unity | Godot | O3DE | Strada |
|---|---|---|---|---|---|
| Skeletal animation, blending | ✅ Animation Blueprints | ✅ Mecanim | ✅ AnimationTree | ✅ EMotionFX | ❌ |
| State machines, blend spaces | ✅ | ✅ | ✅ | ✅ | ❌ |
| IK, rigging | ✅ Control Rig, full-body IK, IK Rig | ✅ Animation Rigging | ✅ IK modifiers (4.6) | 🟡 Simple IK | ❌ |
| Motion matching | ✅ Pose Search | ❌ Third party | ❌ | 🟡 Experimental gem | ❌ |
| Retargeting | ✅ | ✅ Humanoid | ✅ At import | 🟡 | ❌ |
| Ragdolls, physical animation | ✅ Physics assets, 🟡 Control Rig Physics | ✅ Ragdoll wizard | ✅ Physical bones | ✅ PhysX ragdoll | ❌ (Jolt supports ragdolls) |
| Morph targets | ✅ Plus ML Deformer | ✅ | ✅ | ✅ | ❌ |
| Timeline, cinematics | ✅ Sequencer, Movie Render Graph | ✅ Timeline, Cinemachine | ✅ AnimationPlayer, Movie Maker | ✅ Track View | ❌ Graphs have a curve-keyed `Flow.Timeline`, the analogue of Blueprint's Timeline node, not a sequencer |

### 4.4 Physics and vehicles

| Feature | Unreal | Unity | Godot | O3DE | Strada |
|---|---|---|---|---|---|
| Engine | Chaos | PhysX (GameObjects), Unity Physics (DOTS) | Jolt (default since 4.6) | PhysX 5 (default since 26.05) | Jolt 5.6 |
| Rigid bodies, joints, characters | ✅ | ✅ | ✅ | ✅ | ✅ Five joint types, character controller |
| Queries | ✅ | ✅ | ✅ | ✅ | 🟡 Raycasts only |
| Sub-stepping, async physics | ✅ | 🟡 Manual stepping | 🟡 Physics interpolation | ✅ | ❌ One step per update, 8 steps a frame |
| Cloth, soft bodies | ✅ Chaos Cloth | 🟡 Legacy cloth | ✅ SoftBody3D | ✅ NvCloth | ❌ (Jolt has soft bodies) |
| Destruction | ✅ Geometry collections | ❌ | ❌ | ❌ | ❌ |
| Vehicles | 🟡 Chaos Vehicles (experimental, no two-wheeler balance) | 🟡 WheelCollider (raycast car) | 🟡 VehicleBody3D (raycast car) | ❌ (robotics gem only) | ❌ (Jolt has wheeled, tracked and motorcycle controllers) |

### 4.5 Audio

| Feature | Unreal | Unity | Godot | O3DE | Strada |
|---|---|---|---|---|---|
| Mixing, effects | ✅ Submixes, modulation | ✅ AudioMixer, snapshots | ✅ Buses with effects | ✅ Through Wwise | ❌ |
| Procedural audio, DSP graphs | ✅ MetaSounds, Quartz | 🟡 Scriptable processors (6.3) | 🟡 Generator streams | ❌ | ❌ |
| Spatial audio | ✅ | ✅ | ✅ | ✅ | 🟡 Distance and panning, doppler off |
| Middleware | ✅ Wwise, FMOD | ✅ Wwise, FMOD | 🟡 Community | ✅ Wwise | ❌ |

### 4.6 Input, UI and gameplay systems

| Feature | Unreal | Unity | Godot | O3DE | Strada |
|---|---|---|---|---|---|
| Input actions and rebinding | ✅ Enhanced Input | ✅ Input System | ✅ InputMap | ✅ Input bindings | ❌ Raw keys, mouse and gamepads |
| Rumble, haptics | ✅ Force feedback assets | ✅ | ✅ (SDL3 since 4.5) | *(unverified)* | ❌ |
| Racing wheels with force feedback | ❌ Third-party plugins | ❌ Logitech SDK | ❌ | ❌ | ❌ |
| UI framework | ✅ UMG, CommonUI | ✅ UI Toolkit (world space too), uGUI | ✅ Controls, containers, themes | ✅ LyShine | 🟡 Five element kinds, no layouts, scrolling or text input |
| Localization | ✅ | ✅ | ✅ | *(unverified)* | ❌ |
| Navigation, AI | ✅ NavMesh, behavior trees, StateTree, Mass | ✅ AI Navigation, Behavior | ✅ Navigation server | ✅ Recast gem | ❌ |
| Networking | ✅ Replication, Iris | ✅ Netcode for GameObjects/Entities | ✅ Multiplayer API | ✅ Multiplayer gem | ❌ |
| Saves and settings | ✅ SaveGame | 🟡 PlayerPrefs | ✅ ConfigFile, resources | *(unverified)* | ❌ (scripts write files themselves; graphs have no file API) |

### 4.7 Scripting and extensibility

| Feature | Unreal | Unity | Godot | O3DE | Strada |
|---|---|---|---|---|---|
| Languages | C++ (Live Coding), Blueprints, Python (editor); Verse in UEFN and UE6 | C# (Mono and IL2CPP, moving to CoreCLR), Visual Scripting | GDScript, C# (.NET 10 in 4.8), GDExtension | C++ gems, Script Canvas, Lua | C# on .NET 10 CoreCLR; visual scripts (graphs) |
| Hot reload | ✅ Live Coding | ✅ (domain reload) | ✅ | *(unverified)* | ✅ Collectible assemblies; running graphs take each edit within the edit's own call, also while paused and into stopped executions, with receipts |
| Visual scripting | ✅ Blueprints | ✅ Visual Scripting | ❌ (removed in 4.0) | ✅ Script Canvas | ✅ Graphs (§4.7.1) |
| Native plugins | ✅ Plugins | ✅ Native plugins, packages | ✅ GDExtension | ✅ Gems | ❌ |
| Asset store | ✅ Fab | ✅ Asset Store | ✅ Godot Asset Store (2026) | 🟡 | ❌ (Poly Haven imports) |

#### 4.7.1 Visual scripting in detail

Re-checked in November 2026 against the vendors' documentation (§8); what could not be confirmed is marked
*(unverified)*. Godot has had no visual scripting since 4.0. Strada's column describes the tree of 2026-11-13; visual
scripting itself has not changed since `d130bc1`.

| Feature | Strada | Blueprints (UE 5.8) | Unity Visual Scripting (1.9) | O3DE Script Canvas (26.05) |
|---|---|---|---|---|
| Graph and code units | Scripts and function libraries | Blueprint classes, function and macro libraries, interfaces, event dispatchers | Script Graphs, State Graphs, subgraphs | Graphs, functions, Script Events *(unverified)* |
| Macros, inheritance, interfaces | ❌ None; composition through entities and typed events | ✅ | Subgraphs only *(unverified)* | ❌ *(unverified)* |
| State machines | ❌ | Outside Blueprints (Animation Blueprints, StateTree) | ✅ State Graphs | ❌ *(unverified)* |
| Containers | Arrays only | Arrays, sets, maps | Lists, dictionaries *(unverified)* | *(unverified)* |
| Formula node | ✅ Expression, compiled to one VM step | ✅ Math Expression | ✅ Formula *(unverified)* | *(unverified)* |
| Timeline node | ✅ Curve and event tracks | ✅ | ❌ (Timeline is a separate tool) | *(unverified)* |
| Code members as nodes | ✅ The whole scripting API by reflection; `[VisualNode]` game members | ✅ BlueprintCallable functions and properties | ✅ Reflection-based node library | ✅ Behavior Context *(unverified)* |
| Breakpoints | ✅ Conditions, hit counts, break on errors, instance targets | ✅; the 5.8 page mentions no conditions or hit counts | Not documented | In the debugger API, not in the user guide |
| Stepping, call stack | ✅ Into, Over, Out, Run to Node, across graphs and event sends | ✅ Stepping, call stack, execution trace | ❌ (live highlighting only) | ❌ (navigates captured logs) |
| Values while debugging | ✅ Every frame value, Evaluate, wire values | ✅ Pin watches, hover tooltips, a Data Flow tab | Flow highlighting, a red error node | Captured logs with node data |
| Live edit while playing | ✅ Within the edit's own call, into stopped executions, with receipts | Recompile during PIE *(unverified)* | ✅ Asset graphs keep play-mode edits; embedded graphs revert; edits after Start need a restart | *(unverified)* |
| Debugging standalone games | ❌ Editor only | *(unverified)* | ❌ | ✅ Standalone and launcher targets |
| Agent authoring and debugging through MCP | ✅ 27 commands, 51 edit operations, debugging sessions tested in CI | 🟡 The experimental Unreal MCP exposes Blueprints (secondary sources) | 🟡 Through Unity's agent tools, beta *(scope unverified)* | ❌ |

Reading the table:

- Strada leads on agent control and on the debugger depth that the vendors document.
- For everyday scripting it is at or above parity with Unity Visual Scripting and Script Canvas, though Unity has state
  graphs.
- It trails Blueprints on language breadth: macros, interfaces, inheritance, maps and sets.
- Its debugger stops at the editor, while O3DE debugs standalone games.
- Coverage of Unity's 2026 roadmap mentions its Graph Toolkit moving into the core editor and says nothing official
  about Visual Scripting's future.

### 4.8 Tooling and pipeline

| Feature | Unreal | Unity | Godot | O3DE | Strada |
|---|---|---|---|---|---|
| Profiling | ✅ Unreal Insights | ✅ Profiler, Memory Profiler, Frame Debugger | ✅ Profilers, Tracy and Perfetto (4.6) | ✅ Profiler gem | 🟡 Frame times, GPU time and draw statistics; graph run counts and `graph.trace` are debugging aids, not a profiler |
| Asset cooking, packages | ✅ DDC, Zen server, IoStore | ✅ Accelerator, Addressables | ✅ Import cache, PCK | ✅ Asset Processor, bundler | ❌ Exports copy the asset directory |
| Version control, collaboration | ✅ Perforce, Git; multi-user editing; Lore (2026) | ✅ Unity Version Control | 🟡 Git-friendly text files | 🟡 | 🟡 Git-friendly JSON files, and a new project gets a `.gitignore` for build output and caches; graphs are canonical text from a deterministic writer, with no visual graph diff |
| Automated testing | ✅ Automation framework | ✅ Test Framework | 🟡 Add-ons (GUT) | 🟡 | ✅ In-game test framework, headless test runs, CI-ready exit codes; test checks can be graphs |
| AI agents | 🟡 AI Assistant (5.7); Unreal MCP inside the editor, experimental in 5.8, HTTP on localhost without authentication | 🟡 The Unity CLI with an MCP mode (beta, free since July 2026); the Unity AI MCP server is deprecated | 🟡 Community MCP add-ons | ❌ | ✅ Every operation, graphs included, through authenticated, fuzzed automation and MCP |
| Getting the editor | ✅ Epic Games Launcher (sign-in required), a Linux zip, or a source build | ✅ Unity Hub (sign-in required), then one editor per version | ✅ A self-contained download (86 MB on Windows) | ✅ An installer per version on Windows, a `.deb` on Linux, then the Project Manager | ❌ A source build; the editor runs from the build folder (§4.10) |

### 4.9 Platforms

| Platform | Unreal | Unity | Godot | O3DE | Strada |
|---|---|---|---|---|---|
| Windows, macOS, Linux | ✅ | ✅ | ✅ | ✅ (macOS experimental) | ✅ |
| Consoles | ✅ | ✅ | 🟡 Through third parties | ❌ | ❌ |
| Mobile | ✅ | ✅ | ✅ | 🟡 Android | ❌ |
| Web | ❌ | ✅ WebGPU | ✅ | ❌ | ❌ |
| XR | ✅ OpenXR | ✅ OpenXR | ✅ OpenXR | 🟡 OpenXR gem | ❌ |

### 4.10 Distribution and first run

Researched on 2026-11-07 from the vendors' documentation, release pages and package manifests. Cells state what a new
user meets; no ratings. Strada's column was updated on 2026-11-13 for Front Door.

| | Unreal 5.8 | Unity 6.6 | Godot 4.7 | O3DE 26.05 | Strada (Front Door) |
|---|---|---|---|---|---|
| Install form | Epic Games Launcher; a precompiled Linux zip; offline MSI or PKG installers for organizations with seats; source on GitHub | Unity Hub (exe, dmg, MSIX; apt, dnf, AppImage), which installs one editor per version | A self-contained zip; Steam, itch.io and the Epic Games Store (without C#); community winget, Scoop, Homebrew and Flathub packages | A Windows installer per version (`C:\O3DE\<version>`) and a `.deb` per release; no macOS editor | A source build (CMake, a C++20 compiler, the Vulkan SDK, the .NET SDK); the editor runs from `build/<preset>/bin` |
| Administrator rights | The launcher may show security prompts | Needed to install editors by default; an administrator can allow standard users | None | Not stated | None (nothing is installed) |
| Sign-in | Required to download | Required to use the Hub and get a license | None; offline until the user goes online | None (a EULA) | None |
| Several versions | One install per engine version | Side by side, managed by the Hub | Separate executables; version managers are community tools | Side by side, registered in `~/.o3de/o3de_manifest.json` | No |
| Project file pins the engine | `.uproject` `EngineAssociation`: a version, or a source build's GUID | `ProjectSettings/ProjectVersion.txt` | `config/features`; C# projects pin `Godot.NET.Sdk/<version>` | `project.json`: `engine` and `compatible_engines` | No: `.sproj` records a format version only |
| Double-clicking a project | UnrealVersionSelector opens the associated engine | No project file; projects open from the Hub | Registered by the Linux packages only | Not found | Does not work: the editor ignores positional arguments and nothing is registered |
| First window | The Project Browser: templates by category, Blueprint or C++ | Hub onboarding, templates, learning content | The Project Manager; demos and templates online only with consent | The Project Manager; a native build before the editor opens | A start screen when no project is open: recent projects (marked when missing or unreadable), a New Project form with a card per template, example games that open as copies, links to the guides, the version, and the .NET and GPU the machine has; `--no-start-screen` hides it for agent sessions. There is no Help menu |
| Updates | Through the launcher | The Hub updates itself; editors install per version | A notice when online and allowed; no self-update | A new installer per release | None |
| Command line for agents | UnrealEditor-Cmd, UnrealBuildTool, RunUAT | The Unity CLI (beta): JSON output, documented exit codes, `doctor`, `mcp`, `mcp configure`, `self-update` | The editor binary is its own command line | The `o3de` Python script | `strada`: `mcp`, `call`, `commands`, `launch`, `instances`; JSON-RPC to running editors |
| C# toolchain | Not applicable (UnrealBuildTool runs on .NET) | Unity's own scripting runtime; no separate .NET SDK *(unverified)* | A separate .NET edition; the .NET SDK 8 or later builds scripts | Not applicable | The .NET 10 runtime for any scripting, graphs included; the SDK for C#. The start screen shows which of them the machine has, and a C# template's card says it needs the SDK |
| Agent files in new projects | No; `ModelContextProtocol.GenerateClientConfig` writes `.mcp.json` on command | No; Hub checkboxes add the CLI's and the AI's packages | No | No | Yes, by default and visibly: an `AGENTS.md` block, `CLAUDE.md`, a `.mcp.json` entry for `strada mcp`, the game-making skill and the guides in `.strada/docs/`; an existing project only through a plan the user confirms. The repository's own AGENTS.md and skills serve engine work |
| Size | No official figure; forum answers cite tens of GB | About 5 GB to download, 8.5-10 GB installed (a Unity 6.2 macOS install, forum report, 2025) | 86 MB (117 MB with .NET) on Windows | 40 GB of disk with the installer | About 23 MB of product files inside a 701 MB build folder (a Release build at `d130bc1`) |

Reading the table:

- Self-contained archives (Godot) and per-user installers (VS Code's user setup, outside engines) need no administrator;
  Unity's administrator default needs enterprise workarounds.
- Every engine with a project file pins its engine version in it; Strada's `.sproj` does not.
- Only Unreal routes a double-clicked project to the project's own engine version.
- The big engines require sign-in; the small ones do not, and GameMaker dropped its mandatory sign-in in 2026.
- In 2026 the command line became the agents' front door: by Unity's figures, about 98% of Unity CLI runs from
  2026-07-20 to 2026-10-13 came from coding agents and automation, and `unity mcp configure` writes the configuration of
  16 clients.
- No game engine found writes AGENTS.md, CLAUDE.md, skills or MCP client configuration into a new project when it is
  created; web frameworks do (Next.js 16.2 by default, Angular's `ng new --ai-config`). Strada's new projects have
  them by default since Front Door.

Front Door (Roadmap.md's Effort 1) took the first window and the agent files of this table. The rest of it, an
installer, versioned engine lines side by side, a project file that pins its engine, double-clicking a project and
updates, is designed and waits in Roadmap.md's backlog (§3.10), not scheduled.

## 5. What Strada is missing

Three tiers, by how far a game gets without them. Visual scripting, which the October snapshot listed in no tier because
the roadmap had ruled it out, is now present: for everyday scripting it puts Strada at or above parity with Unity Visual
Scripting and O3DE Script Canvas, behind Blueprints on language breadth, and ahead of Godot, which has none (§4.7.1).

**Tier 1, table stakes** (nearly every commercial 3D game uses them; Godot has almost all of them, Unreal and Unity
all):

1. Skeletal animation: skins, clips, blending, IK, ragdolls, state machines.
2. Texture compression (BCn), offline mips, texture streaming, asynchronous loading.
3. Instancing, mesh LODs and better culling for scenes with thousands of objects.
4. Temporal anti-aliasing with motion vectors, motion blur and an upscaler.
5. Input actions with rebinding, rumble and more devices.
6. An audio mixer: buses, effects, doppler, snapshots.
7. A UI widget set with layouts, scrolling, text input and gamepad navigation.
8. A CPU and GPU profiler.
9. Saves, player settings and graphics quality levels.
10. Physics queries beyond raycasts, contact data, more constraint types.
11. Cooked packages for shipping.

All eleven are still missing on 2026-11-13.

**Tier 2, expected of a professional engine:** terrain; foliage; a physical sky with fog and time of day; reflection
probes and SSR; decals; custom shaders and a material graph; GPU particles; a timeline and a camera system; navigation
meshes and behavior trees; localization; sub-scenes and streaming; prefab variants; networking; native plugins. All are
still missing. A material graph can build on the node canvas visual scripting brought, and graph timelines cover part of
"a timeline".

**Tier 3, high end:** dynamic GI, hardware ray tracing, virtualized geometry, procedural generation frameworks, motion
matching, a DSP graph editor like MetaSounds, multi-user editing, and consoles, mobile, web and XR. A DSP graph editor
could reuse the same canvas.

**Beyond game features**, Strada lacks what any engine needs before strangers can use it: an installed editor and
versioned releases (§2.2, §4.10). It has had a license since `ea34866` (MIT) and, since Front Door, a first run that
works from a source build: a start screen, templates, examples and projects set up for agents. The installer, signing
and the rest of the design are in Roadmap.md's backlog (§3.10), not scheduled.

Strada should close Tier 1 in full, take from Tier 2 what its games need (for a motorcycle game: terrain, splines,
foliage, sky, probes, decals, a camera system and a timeline), and treat Tier 3 as optional.

## 6. What a realistic motorcycle game needs

The goal is a game whose bikes behave like real superbikes and MotoGP prototypes because they are simulated, as
BeamNG.drive's cars are, and whose engines sound like engines because the sound comes from a model of the engine, as in
Ange Yaghi's engine-sim. This section collects what that takes; Roadmap.md §5 plans it. It is the October 2026 research
and was not re-checked in November, apart from Jolt's version (§6.8).

### 6.1 Reference points

**BeamNG.drive.** Vehicles are soft bodies: about 400 nodes (point masses) joined by about 4,000 beams (springs and
dampers that deform and break), stepped at 2,000 Hz, vehicles in parallel. JBeam files build them from parts, including
a powertrain tree of devices (engines, clutches, gearboxes, shafts, differentials) and tires made of nodes around a
pressurized volume. Vehicle logic is Lua, at the physics or the graphics rate. The engine began as Torque3D and is now
its own: v0.39 (July 2026) made Direct3D 12 the Windows default, with Vulkan 1.3 on Linux, and added clustered shading,
GTAO, volumetric clouds and slipstreams; the PS5 version was announced for early access on 19 October 2026. There is no
official motorcycle: mods need Lua balance controllers to stay up, and players find them hard to ride. Lessons: a high
fixed rate, data-driven vehicles with a device tree for the powertrain and vehicles simulated in parallel work well;
node-beam tires do not give the camber thrust a bike lives on, and nothing in BeamNG rides a bike with steer torque.

**engine-sim** (Ange Yaghi; MIT license, Windows only). It simulates an engine to make its sound: a 2D constraint solver
moves crank, rods and pistons; each cylinder is a combustion chamber with its gas mixture, flame propagation and heat
loss; gas flows through intake runners, valves (flow tables times cam lift), exhaust primaries and collectors; engines
are written in his Piranha scripting language. The simulation steps at 10-20 kHz. Each cylinder's exhaust pressure,
delayed by its pipe lengths at the speed of sound and summed, is resampled to 44.1 kHz, filtered and convolved with an
exhaust impulse response, on a thread of its own. The open-source version is no longer maintained, and its successor (a
3D engine simulator with live pipe tuning and a movable microphone, funded through Patreon) will not be published as
source. Lesson: physically based engine sound runs in real time, on its own thread at 10-20 kHz, given an engine model
detailed down to its pipe lengths; Strada builds its own from the published methods (§6.5).

**Motorcycle games.**

- **GP Bikes and MX Bikes** (PiBoSo, own engine) are the players' reference for bike physics: a model with chassis flex,
  a track that rubbers in and gathers marbles, wet tracks that dry, a simulated rider whose lean and fore-aft position
  are separate inputs, an optional direct steer-torque mode (counter-steering by hand, hard), plugins and CSV telemetry,
  no AI.
- **Milestone** (MotoGP 25 and 26, Ride 6 on Unreal Engine 5) offers Pro and Arcade handling on physics of its own.
  MotoGP 26 (April 2026) rebuilt it as "Rider Based Handling": input moves the rider, and the rider's body steers the
  bike.
- **RaceWard's** RiMS Racing and TT Isle of Man: Ride on the Edge 3 (KT Engine) are praised for weight and slides;
  RiMS's component wear changes how the bike behaves.
- What players judge: how input maps onto the rider, how the rider's body couples to the bike, front-end feel and tucks,
  slides under braking and on exit, believable high-sides and low-sides with a rider that separates from the bike, kerbs
  (a common complaint: they launch bikes), and assists they can tune.
- Hardware: handlebar controllers such as the Moza MotoGP handlebar (2026) offer direct-lean and counter-steering modes.

Across racing games, studios that render with Unreal (Milestone, Kunos on Assetto Corsa Competizione, Codemasters on EA
Sports WRC, Assetto Corsa Rally) run vehicle physics of their own, and Kunos wrote a new engine for Assetto Corsa EVO.
Physics rates: BeamNG 2,000 Hz, Assetto Corsa Competizione 400 Hz.

### 6.2 Motorcycle dynamics

- **Models.** The Whipple bicycle model (1899), with the benchmark linearization of Meijaard, Papadopoulos, Ruina and
  Schwab (2007) whose published eigenvalues make a ready test oracle; Sharp (1971), who added tire side-slip and found
  the capsize, weave and wobble modes; Sharp and Limebeer (2001) and Sharp, Evangelou and Limebeer (2004), with tire
  contact geometry, Magic Formula tires, relaxation, the rear linkage and frame flexibility, and full parameter sets;
  Cossalter's *Motorcycle Dynamics* (the engineering text) and Limebeer and Massaro's *Dynamics and Optimal Control of
  Road Vehicles* (2018).
- **Modes a simulation must reproduce.** A simulation that gets them right feels right; one that does not never will,
  whatever its tuning.

| Mode | What happens | Frequency |
|---|---|---|
| Capsize | Slow, non-oscillatory fall into the lean; mildly unstable at speed, easily held by the rider | - |
| Weave | The whole bike yaws and rolls; unstable at walking pace, lightly damped again at high speed | about 2-4 Hz |
| Wobble | The front frame oscillates about the steering axis; held down by the steering damper | about 4 Hz (heavy bikes) to 9-10 Hz (light bikes) |
| Chatter | Racing bikes under braking: the unsprung masses vibrate at 5-10 g; needs tire carcass and fork compliance | about 17-22 Hz |

- **Counter-steering.** To lean right, the rider first steers left: the tires push the contact patches out from under
  the bike, it rolls into the turn, then the front wheel steers into it. Players never think about it, so input must ask
  for a lean or a path and a controller must find the steer torque, or the player gives steer torque directly, as in GP
  Bikes.
- **Geometry.** Rake and trail (normal trail is trail × cos(rake)) give self-steering into the lean and stability;
  wheelbase; center-of-mass height (the rear wheel lifts at g times the ratio of the center of mass's horizontal
  distance to the front contact to its height: about 1.3 g for a superbike and rider); the swingarm and the chain line
  (anti-squat: the chain's force acts along the upper run between the sprockets, not as a torque on the swingarm, and
  teams move the pivot by millimeters); fork and shock rates and damping, the shock through a linkage ratio curve;
  frame, swingarm and fork flex (tuned for feel at full lean: the 2025 Panigale's swingarm is laterally softer than its
  predecessor's).
- **Gyroscopic effects.** The wheels resist changes of lean; a counter-rotating crank, turning about ten times faster
  than the front wheel, cancels part of that, and as the engine revs up its reaction torque pushes the front down. Model
  wheel spin, crank and gearbox inertias explicitly.
- **The rider** is about 30% of the mass (a 157 kg MotoGP bike with a 60-70 kg rider) and moves: hang-off shifts the
  center of mass inwards so less lean is needed, a tuck cuts drag, the grip on the bars damps the steering. At the
  least, an inverted-pendulum torso on a hip joint.
- **Rates and precision.** About 11-13 degrees of freedom in minimal coordinates (chassis 6, steering, fork, swingarm,
  two wheel spins, the rider's lean and fore-aft position, optionally flex) plus the tires' lag states, integrated
  semi-implicitly at 1-2 kHz in double precision. Single precision is not enough: 4 km from the origin a float resolves
  0.5 mm, about 1% of the 4 cm a bike at 300 km/h covers in a 2 kHz step. Commercial real-time bike models (VI-grade's
  VI-BikeRealTime, used by BMW Motorrad Motorsport; BikeSim) take the same approach.

### 6.3 Tires

- **Magic Formula.** Pacejka's semi-empirical formula gives the longitudinal and lateral forces and the aligning,
  overturning and rolling moments from slip, camber, load and (from version 6.1) pressure. MF-Tyre 6.1 and 6.2 cover
  large camber, including motorcycle tires, and turn slip; the older PAC2002 stops at about 15° of camber. At 50° of
  lean most of the lateral force is camber thrust, so a bike corners with little slip angle.
- **Toroidal contact.** The contact point moves across the crown as the bike leans and the rolling radius shrinks, which
  produces a steering torque and changes the gearing: the contact lies a crown radius from the center of the crown
  circle, which sits at the wheel radius minus the crown radius from the hub, in the wheel plane.
- **Transient response.** Side force from slip builds up over a relaxation length (first-order lags), which shapes weave
  and wobble; Cossalter and Doria's measurements show that camber thrust has almost no lag.
- **Temperature and wear.** Grip peaks within a window of temperatures and falls with wear. Real-time thermal models
  with a few layers (surface, bulk, inflation gas) exist for motorcycle tires: thermoRIDE, by Farroni and colleagues
  with Ducati Corse.
- **Data.** Sharp, Evangelou and Limebeer (2004) publish Magic Formula parameters for modern sports tires;
  manufacturers' data is confidential. Expect to fit and tune.

### 6.4 Powertrain and electronics

- **Engine.** Torque from a map over RPM and throttle (ride-by-wire, shaped by the ECU's power modes), friction and
  pumping losses (engine braking), and a crank-angle model when the torque ripple matters (traction, sound). Published
  figures differ by market (the Panigale V4 S: 216 hp in Europe, 209 hp in the US), so treat curves as approximate.
- **Inertias.** The engine as a rotating inertia coupled to the chassis: its reaction torque when the RPM changes and
  the counter-rotating crank's gyroscopic moment change wheelies, stoppies and direction changes.
- **Driveline.** A wet multi-plate friction clutch with stuck and slipping states, a slipper (back-torque limiter) and a
  self-servo ramp; a six-speed sequential gearbox (1-N-2-...-6) with dog engagement times (MotoGP bikes use seamless
  gearboxes); a quickshifter that cuts the ignition for up to about 50 ms on upshifts and an auto-blipper on downshifts;
  primary and chain drives; a rev limiter.
- **Electronics.** A six-axis IMU computes lean and pitch more than 100 times a second; with wheel speeds it feeds
  traction control (slip targets by lean), wheelie control (pitch), slide control (yaw and drift angle), engine-brake
  control, launch control and cornering ABS with rear-lift mitigation, each with levels grouped into riding modes.
  Simulate the ECU at 100-1,000 Hz with a modeled IMU (noise and bias), so the electronics behave like real ones; the
  bikes are unrideable at the limit without them, and players expect to tune them.

### 6.5 Sound

Racing games make engine sound three ways:

- **Loop crossfading.** Steady-RPM recordings (often around 20 each inside and outside), in on-load and off-load banks,
  pitched and crossfaded by RPM and load (FMOD and Wwise events in most racing games). Cheap and authentic at steady
  state; smeared in fast transients, and superbikes rev by more than 10,000 rpm a second.
- **Granular synthesis.** Recorded sweeps cut into grains per engine cycle and replayed at the simulated RPM (Crankcase
  Audio's REV for Wwise, Audiokinetic's SoundSeed Grain). Handles fast revs; needs clean sweeps of the real bike.
- **Physical modeling.** The sound comes from a model of the engine: engine-sim (§6.1); Baldan and colleagues'
  physically informed synthesis (2015, used in a driving simulator); Doerfler and Wyse's pulse-train resonators (2026,
  code published), with pulses aligned to the firing order, Karplus-Strong exhaust resonators and modes for throttle and
  fuel cut. A practical hybrid: physical pulses, impulse responses recorded from a real exhaust, granular texture
  layers. At 15,000 rpm the crank turns 90,000° a second, so a 10-20 kHz step resolves 4.5-9° of crank angle, the least
  that valve events and pulse timing need. The Panigale's Twin Pulse V4 fires at 0°, 90°, 290° and 380°, so its sound
  has energy at the cycle rate (rpm/120) as well as the mean firing rate (rpm/30).

The rest of a bike's sound: intake and airbox roar (the airbox sits under the rider's chin), pops and crackles on
overrun (fuel cuts at high RPM; random variations, or they repeat), gear and primary-drive whine, the quickshifter's
bark and the blipper's blip, chain slap, tire scrub and slides (subtler than a car's), kerb rumble, and wind and helmet
noise: 95-106 dB at 100 km/h in helmet tests, steeply rising, masking the engine for the rider above about 150 km/h, so
the onboard mix needs speed-dependent filtering and ducking. Trackside listeners need doppler and distance filtering.
miniaudio's node graph, custom nodes and ring buffers fit the design: synthesis in a node at 48 kHz fed by the
simulation thread, with about 20 ms of latency at most.

### 6.6 Tracks, telemetry and replays

- **Circuits.** Laser scans (iRacing reports millimeter-level LiDAR over 250 million points for a road course) give the
  bumps and camber; the physics surface wants 1-2 cm vertex spacing on the racing line, apart from the render mesh. Each
  triangle names a surface (grip, rolling resistance, bump amplitude and wavelength, vibration; Assetto Corsa's
  `surfaces.ini` is the pattern), kerbs keep their real profiles, gravel traps add drag and sinkage (where crashed bikes
  and riders tumble), and the racing line rubbers in, gathers marbles, wets and dries.
- **Distances.** At 100 m/s (368.6 km/h is MotoGP's record, set in 2026) a rider sees kilometers ahead: LODs and
  impostors. The Isle of Man's Snaefell Mountain Course is 60.7 km: streaming and double precision.
- **Timing.** Start-finish and sector gates as oriented planes, crossing times interpolated between physics steps to
  below a millisecond, track limits and penalties.
- **Telemetry.** Channels at high rates, compared lap against lap as riders and engineers do in MoTeC i2; live UDP
  streams at 20-60 Hz (EA's F1 format is the common pattern) and shared memory for dashboards, SimHub and motion rigs;
  CSV export (MoTeC's `.ld` format is closed and only reverse-engineered).
- **Replays and ghosts.** Inputs plus periodic snapshots for scrubbing; ghosts are recorded poses. Jolt is deterministic
  only for the same binary and the same order of calls, so the bike's own solver should be deterministic by
  construction.
- **Motion rigs and handlebars.** Expose roll, pitch, yaw, accelerations, steer torque and slips; a force channel for
  handlebars (research simulators found that steer torque that follows the roll makes riding feel real).

### 6.7 Reference bikes

**Ducati Panigale V4 S (2025 model, unchanged for 2026 as far as published):**

| Item | Value |
|---|---|
| Engine | 1,103 cc Desmosedici Stradale 90° V4, counter-rotating crank, 70° crankpin offset ("Twin Pulse": 0°, 90°, 290°, 380°) |
| Power, torque | 216 hp at 13,500 rpm and 120.9 Nm (Europe; 228 hp with the Akrapovič race exhaust); 209 hp at 12,750 rpm (US) |
| Weight | 187 kg wet without fuel |
| Geometry | Wheelbase 1,485 mm, rake 24°, trail 98 mm; a new double-sided aluminum swingarm |
| Suspension | Öhlins NPX 25/30 pressurized fork, TTX36 shock, electronic steering damper; semi-active (Öhlins Smart EC) |
| Brakes | Brembo Hypure calipers on 2 × 330 mm discs, 245 mm rear disc |
| Tires | Pirelli Diablo Supercorsa SP, 120/70 ZR17 front, 200/60 ZR17 rear; forged wheels |
| Electronics | Ducati Vehicle Observer ("virtual sensors"), six-axis IMU, Race eCBS, traction (DTC), wheelie (DWC), slide (DSC) and engine-brake (EBC) control, pit limiter, quickshifter (DQS 2.0), cornering ABS; riding modes Race A, Race B, Sport, Road and Wet |

**MotoGP.** Ducati's Desmosedici GP25: 1,000 cc 90° V4 desmo, "over 250 hp" (estimates 260-290 hp), over 350 km/h,
157 kg minimum weight, Öhlins suspension with carbon fork tubes, carbon front discs, magnesium wheels, 17-inch Michelin
tires, aerodynamics; front ride-height devices are banned (the start device since June 2026), rear ones until the end
of 2026. **2027 rules:** 850 cc with a 75 mm bore limit, every ride-height and holeshot device banned, the front fairing
at most 550 mm wide, a 153 kg minimum weight, 20 liters of fuel (11 in sprints) of non-fossil origin, and Pirelli
instead of Michelin as the control tire.

### 6.8 Jolt's vehicles

Jolt 5.6 (July 2026; Strada pins it, and it was still Jolt's latest release on 2026-11-07) has a `VehicleConstraint`
with wheeled, tracked and motorcycle controllers.

- **Wheeled vehicles.** Each step a ray, sphere or cylinder cast finds each wheel's contact; suspension, longitudinal
  and lateral impulses are solved in the constraint solver; friction comes from curves over slip ratio and slip angle;
  engine (torque curve, inertia), transmission (ratios, clutch, shift times) and differentials are modeled. A tire
  callback can replace the default friction, which limits each axis separately.
- **`MotorcycleController`** (its header calls it "still in development") keeps the bike upright with a PD spring on the
  roll axis toward the lean that balances the contact forces, smoothed and clamped (45° by default), and limits steering
  to the angle the lean allows. It has no gyroscopic effects, counter-steering, camber thrust, toroidal contact,
  relaxation, rider, chain geometry or fork compliance.
- **Verdict:** good for arcade bikes and traffic, not for a simulation. Use Jolt for the world: collision queries for
  the tires, props, other bikes, crashes and ragdolls.
- **Precision and stepping.** `JPH_DOUBLE_PRECISION` stores positions as doubles (about 5-10% slower; floats are fine
  within about 5 km of the origin); cross-platform determinism costs about 8%. Jolt recommends 60 Hz with one collision
  step; running the whole world at 1-2 kHz would cost 16-33 times as much, which is why the bike runs its own solver.

### 6.9 What this asks of Strada

| Requirement | Roadmap items |
|---|---|
| A deterministic, double-precision bike solver at 1-2 kHz on worker threads, interpolated for rendering | V1, P3a, P3b, T3 |
| Magic Formula motorcycle tires with toroidal contact, relaxation and temperatures, from data files | V2, T7 |
| Steering geometry, fork and swingarm with chain force, linkage curves, gyroscopic terms, flex, the rider's torso | V3, V5 |
| Simulated ECU and IMU, clutch, gearbox, quickshifter and blipper | V4 |
| Assists that turn a gamepad's lean request into steer torque; a direct mode; an AI rider on the same physics | V5, V12, I1, I2 |
| Engine sound on its own thread at 10-20 kHz into miniaudio through a ring buffer, plus the other sounds | V8, AU1, AU2, AU3 |
| Jolt for the world: surface queries for tires, props, other bikes, crashes and ragdolls | P1, P2, A3 |
| Track tools: road surfaces with surface types, kerbs, gravel, timing gates; terrain, foliage, sky, long views | V9, W1, W2, W3, R2, R9, R17 |
| Telemetry (UDP, CSV), deterministic replays, ghosts, handlebar and motion-rig output | V7, V11, I2 |
| Scripts and graphs stay out of the fast loops and exchange controls and state with them once a frame | V1 |

## 7. Libraries to build on

Strada's dependencies are pinned, permissively licensed and built from source by CMake on all three platforms. The pins
are unchanged since `d130bc1`, because neither visual scripting nor Front Door added a dependency: Jolt 5.6.0, miniaudio
0.11.25, NVRHI's main branch, Vulkan headers 1.4.363. Jolt 5.6.0 was still Jolt's latest release on 2026-11-07; whether
the other pins are still the latest was not re-checked. Candidates for the roadmap's items were checked in October 2026
(release feeds and repositories; *(unverified)* where only secondary sources were available). The Jolt, Tracy, ImPlot,
ozz-animation, enkiTS, SDL3 and KTX-Software rows were re-checked on 2026-11-07; where a release page shows day and
month only, the year is inferred. The distribution rows (the Windows installer, .NET, Linux packages and file dialogs,
release provenance) are new and were researched on 2026-11-07; their sources are in §8. They back the distribution items
DI1-DI9 of Roadmap.md §3.10, which the maintainer cut from the schedule on 2026-11-10: the rows stay as research for the
day distribution returns.

| Need (roadmap) | Library | Version (date) | License | Notes |
|---|---|---|---|---|
| Profiling (T1) | Tracy | 0.15 (5 Nov; year inferred as 2026) | BSD-3 | Its Vulkan GPU zones need absolute GPU timestamps, which NVRHI's timer queries do not give (they return durations); client and viewer versions must match; it listens on an unauthenticated port. The roadmap's T1 records with a recorder of its own and exports Chrome traces; Tracy stays an option for later |
| GPU captures (T2) | RenderDoc in-application API | - | MIT | Adopt; Windows and Linux only (RenderDoc does not support MoltenVK) |
| Jobs (T3) | enkiTS | 1.12 (4 Jul; year not shown) | zlib | Pinned tasks for the main thread and I/O, priorities; can also serve Jolt's `JobSystem`. Adopt (Taskflow 4.1, MIT, is the alternative) |
| Textures (T4) | KTX-Software; Basis Universal; bc7enc_rdo | 4.4.2 (Oct 2025; 5.0 in release candidates, rc2 the latest); 2.50 (Aug 2026); - | Apache-2.0; Apache-2.0; MIT or Unlicense | KTX2 containers, BC1-BC7 encoding (BC6H from the archived ISPC Texture Compressor, MIT). KTX-Software 4.4.2's notes call Basis Universal encoding "non-deterministic across platforms", so tests compare decoded images with tolerances. Metal supports BC formats on Apple Silicon, so BCn can serve every desktop (whether MoltenVK exposes them there is *(unverified)*) |
| Packages (T6) | zstd; lz4 | 1.5.7; 1.10.0 | BSD | Adopt |
| Plots (T7, V7) | ImPlot | 1.1 (25 Oct, "supports ImGui 1.93"); 1.0 (Apr) | MIT | Strada pins ImGui 1.92.9b; whether 1.1 or 1.0 builds against it is *(unverified)*, and v0.17 carries fixes for ImGui 1.92.x. Check before adopting. ImPlot is for plots only: the curve model and editor already exist (`VisualScriptTimeline.h`, `VisualScriptCurveView.h`, `VisualScriptCurveEditor.h`), and T7 reuses them for its curve editing |
| LODs (R2), clusters (R16) | meshoptimizer | 1.3 (Sep 2026) | MIT | Simplification, meshlets, cluster LOD hierarchies. Adopt |
| Upscaling (R6) | AMD FidelityFX SDK (FSR 3.1); NVIDIA Streamline (DLSS); Intel XeSS | 1.1.4 (May 2025); 2.14.16 (Oct 2026); 3.0.2 (Jul 2026) | MIT; MIT with DLSS under NVIDIA's SDK license; Intel's binary-only license | FSR 3.1 has a Vulkan back end; AMD's FSR 4 SDK is Direct3D 12 only. Streamline builds on Windows only. XeSS's license needs review. None targets Metal |
| Reflection of shaders (R4, R13) | SPIRV-Reflect | SDK 1.4.363 (Sep 2026) | Apache-2.0 | Validate binding layouts against the interop structs |
| Graph editors (A4, R14) | Strada's own node canvas | – | – | Superseded in November: the visual-script editor (`StradaEditor/Source/Editor/VisualScripting/`, 97 files: model, controller, renderer, palette, headless tests, automation) has its own canvas and no third-party node editor. A4 and R14 should generalize it; imgui-node-editor (MIT) would split the user experience, automation and tests |
| Animation (A1, A2) | ozz-animation | 0.17.0 (1 Aug; year not shown) | MIT | Sampling, blending, two-bone and aim IK, compression, root motion; the runtime depends only on C++17; fed from the engine's own importers, not ozz's FBX SDK or glTF tools. Adopt (ACL, MIT, if memory matters) |
| Devices (I2) | SDL3 | 3.4.20 (3 Nov 2026) | zlib | Gamepads, rumble, trigger rumble, gyro, LEDs and force feedback (including a steering axis) without SDL's windows, beside GLFW. Adopt |
| Effects (AU1, AU2) | miniaudio node graph; Signalsmith DSP | 0.11.25; - | MIT-0 or public domain; MIT | Custom nodes and built-in filters and delays; reverb in miniaudio's extras. Build the mixer on them |
| Spatial audio (AU4) | Steam Audio | 4.8.1 (Feb 2026) | Apache-2.0 | HRTF, occlusion, reflections, baked reverb, as a miniaudio node |
| Text (U2) | HarfBuzz; FreeType; msdfgen | 14.6.0 (Oct 2026); 2.14.3; 1.13 | MIT; FTL; MIT | Shaping and multi-channel distance fields; follow FreeType's security fixes |
| UI (U1) | RmlUi | 6.3 (Aug 2026) | MIT | HTML/CSS UI with Vulkan and GLFW back ends. Not recommended: Strada's UI is made of components that the inspector, automation and scripts already understand; extend it instead |
| Navigation (S5) | recastnavigation | main (2026) | zlib | Pin a commit of the maintained main branch |
| Script debugging (S4) | netcoredbg; vsdbg; Rider | 3.2 (Jun 2026); -; - | MIT; Microsoft IDEs only; commercial | For C#: attach to the player or editor; portable PDBs and a wait-for-debugger option. Graphs have their own debugger, in the editor |
| Networking (N2) | yojimbo; GameNetworkingSockets | main (Oct 2026); 1.6.1 (Oct 2026) | BSD-3; BSD-3 | yojimbo for dedicated servers; GameNetworkingSockets needs protobuf and a crypto library |
| Steam (N1) | Steamworks SDK | 1.65 (Jul 2026) | Proprietary | Optional platform module |
| XR (X1) | OpenXR SDK; Monado | 1.1.65 (Oct 2026); 25.1 | Apache-2.0; BSL-1.0 | The runtime takes part in creating the Vulkan device; Monado tests without a headset. No macOS runtime |
| Crash reports (T10) | sentry-native; cpptrace | 0.17.4 (Oct 2026); 1.0.4 | MIT; MIT | Upload crash reports from shipped games (the engine writes them locally today) |
| Windows installer (DI3, backlog) | Inno Setup, through CPack's `INNOSETUP` generator | 7.1.1 and 6.7.4 (both 2026-10-28) | Inno Setup's own (free; since 6.5.0 commercial users are asked, not required, to buy a license) | A tool, not a library. Per-user installs with `PrivilegesRequired=lowest`, as VS Code's user setup does. CPack has generated Inno scripts since CMake 3.27; whether it handles Inno 7 is a day-one spike, with 6.7.4 as the fallback. WiX was set aside: CPack's per-user WiX scope needs CMake 4.3, and from WiX 6 organizations with more than USD 10,000 of annual revenue must pay a maintenance fee |
| .NET runtime and NativeAOT (DI2, DI3, backlog) | Microsoft .NET 10 | Runtime 10.0.14, SDK 10.0.403 (both 2026-10-25) | MIT on Linux and macOS; on Windows `coreclr.dll` and runtimes inside single-file apps are under the .NET Library License, while the NuGet runtime pack declares MIT | LTS until 2028-11-14; .NET 11 ships on 2026-11-10. A runtime archive is 80-89 MB unpacked (33-37 MB to download). NativeAOT publishes `strada` without a runtime; whether NativeAOT output falls under the Library License on Windows is for a legal review. The SDK is 640-810 MB installed, so it is not bundled |
| Linux packages (DI6, backlog) | appimagetool and the AppImage type-2 runtime | Continuous builds (no numbered release) | *(unverified)* | Tools. The static type-2 runtime needs no libfuse2 on the target; AppImage's guidance is to build on the oldest base to support and to bundle no newer libstdc++ than it has; drivers always come from the host, and the design relies on the host's Vulkan loader too; `appimagetool -u` embeds zsync update information. The signatures it embeds are not checked at run time |
| Linux file dialogs (DI6, backlog) | nativefiledialog-extended's xdg-desktop-portal back end | 1.3.0 (pinned) | zlib | `NFD_PORTAL ON` replaces the GTK 3 back end the editor builds today (`cmake/StradaDependencies.cmake:169-179`), so a Linux package needs no bundled GTK and raises no LGPL question |
| Release provenance (DI4, backlog) | GitHub artifact attestations; immutable releases | `actions/attest@v4`; immutable releases generally available since 2025-10-28 | GitHub service | A service, not a library. Attestations give SLSA v1.0 Build Level 2 and are checked with `gh attestation verify`; immutable releases keep tags and assets unchanged once published |
| Vehicles (V1, V2) | none to adopt | - | - | No maintained permissive Magic Formula 6.x library exists: Project Chrono (BSD-3) implements Pacejka 2002 inside a large framework, MuJoCo and RBDL target robotics. Write the solver and the tire model from the published equations (the repository's rule is to take no code from outside it) and validate against `.tir` data and published values |

Two platform notes:

- **macOS.** LunarG's KosmicKrisp, a Vulkan driver on Metal built on Mesa, is Vulkan 1.4 conformant on Apple Silicon
  (macOS 26 and later) and ships in the Vulkan SDK; MoltenVK remains for Intel Macs and older systems. MoltenVK 1.4.2
  offers neither mesh shaders nor ray tracing, so GPU-driven and ray-traced features stay optional behind NVRHI's
  feature queries.
- **NVRHI** already supports ray-tracing pipelines and ray queries, mesh shaders, variable-rate shading and bindless
  descriptor tables on Vulkan, so R16 and R22 need no new graphics layer.

## 8. Sources

The October research was done with web searches and with GitHub repositories, release feeds and source files read
directly. Pages outside GitHub could not be fetched from the research environment then, so their facts come from search
results quoting them; claims that could not be confirmed are marked *(unverified)* in the text. In November 2026 pages
were fetched directly; those sources are listed first, with the date shown on each page or "retrieved" where it shows
none.

### Re-checked in November 2026

**Visual scripting.** [Blueprint Debugger, UE 5.8 docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/blueprint-debugger-in-unreal-engine) (retrieved) ·
[Unity Visual Scripting 1.9, debugging](https://docs.unity3d.com/Packages/com.unity.visualscripting@1.9/manual/vs-debugging.html) (2025) ·
[Unity Visual Scripting, live edit](https://docs.unity3d.com/Packages/com.unity.visualscripting@1.8/manual/vs-live.html) ·
[Script Canvas debugging](https://docs.o3de.org/docs/user-guide/scripting/script-canvas/debugging/) (retrieved) ·
[Script Canvas debugger API](https://docs.o3de.org/docs/api/gems/scriptcanvas/class_script_canvas_1_1_debugger_1_1_service_component) (25.10) ·
[Unity's 2026 roadmap, Digital Production](https://digitalproduction.com/2025/11/26/unitys-2026-roadmap-coreclr-verified-packages-fewer-surprises/) (2025-11-26)

**Agents.** [Unreal MCP in Unreal Editor](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-mcp-in-unreal-editor) (retrieved) ·
[Unreal Engine MCP hands-on, Puget Systems](https://www.pugetsystems.com/blog/2026/07/09/unreal-engine-mcp-hands-on-testing-ai-inside-the-editor/) (2026-07-09) ·
[Announcing the Unity CLI](https://discussions.unity.com/t/announcing-the-unity-cli-a-new-way-to-connect-your-tools-and-agents/1731104) (2026-07-15) ·
[Unity CLI usage data](https://unity.com/blog/unity-cli-usage-data-ai-coding-agents) (2026-10-22) ·
[Unity agent plugin](https://github.com/Unity-Technologies/unity-agent-plugin) (read 2026-11-07) ·
[MCP specification](https://modelcontextprotocol.io/specification/latest) (revision 2026-07-28)

**Distribution.** [Unreal Engine 5.8 released](https://forums.unrealengine.com/t/unreal-engine-5-8-released/2729274) (2026-06-17) ·
[Install Unreal Engine](https://dev.epicgames.com/documentation/en-us/unreal-engine/install-unreal-engine) (retrieved) ·
[Managing game code (EngineAssociation)](https://dev.epicgames.com/documentation/en-us/unreal-engine/managing-game-code-in-unreal-engine) (retrieved) ·
[Install the Unity Hub](https://docs.unity.com/en-us/hub/install-hub-win-mac) (retrieved) ·
[Unity Hub release notes](https://unity.com/en/unity-hub/release-notes) (to 2026-11-06) ·
[Unprivileged Unity installs](https://docs.unity.com/en-us/engine/current/manual/get-started/install-and-upgrade/ent-deployment/ent-unpriv-install) (retrieved) ·
[Unity 6 releases](https://unity.com/releases/unity-6-releases) (retrieved) ·
[Godot downloads](https://godotengine.org/download/windows/) (4.7.2) ·
[Godot 4.7.2 release](https://github.com/godotengine/godot/releases/tag/4.7.2-stable) (2026-08-18) ·
[Godot 4.8 beta 1](https://godotengine.org/article/dev-snapshot-godot-4-8-beta-1/) (2026-10-15) ·
[Godot data paths](https://docs.godotengine.org/en/stable/tutorials/io/data_paths.html) (retrieved) ·
[Godot Project Manager](https://docs.godotengine.org/en/stable/tutorials/editor/project_manager.html) (retrieved) ·
[Installing O3DE on Windows](https://docs.o3de.org/docs/welcome-guide/setup/installing-windows/) (retrieved) ·
[O3DE download](https://o3de.org/download/) (retrieved) ·
[O3DE project configuration](https://docs.o3de.org/docs/user-guide/project-config/) (retrieved) ·
[GameMaker LTS 2026.0](https://gamemaker.io/blog/lts-2026-release) (2026-05-21) ·
[VS Code on Windows](https://code.visualstudio.com/docs/setup/windows) (retrieved) ·
[Next.js 16.2 AI improvements](https://nextjs.org/blog/next-16-2-ai) (2026-03-18) ·
[Angular `ng new`](https://angular.dev/cli/new) (retrieved)

**Packaging, signing and .NET** (retrieved 2026-11-07; they back Roadmap.md §3.10, §3.14 and §7-§9, and the
distribution rows of §7). Windows: [Inno Setup 7 what's new](https://jrsoftware.org/files/is7-whatsnew.htm) (7.1.1,
2026-10-28) · [Inno Setup 6 what's new](https://jrsoftware.org/files/is6-whatsnew.htm) (6.7.4, 2026-10-28) ·
[CPack Inno Setup generator](https://cmake.org/cmake/help/latest/cpack_gen/innosetup.html) ·
[CPack WiX generator](https://cmake.org/cmake/help/latest/cpack_gen/wix.html) ·
[WiX maintenance fee](https://docs.firegiant.com/wix/osmf/) ·
[MSIX preparation](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-prepare) (2025-06-10) ·
[SmartScreen reputation](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation) (updated 2026-08-17) ·
[Code signing options](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options) (2026-08-29) ·
[Smart App Control](https://support.microsoft.com/en-us/topic/what-is-smart-app-control-285ea03d-fa88-4d56-882e-6698afdb7003) ·
[Artifact Signing overview](https://learn.microsoft.com/en-us/azure/artifact-signing/overview) (2026-01-02) ·
[Artifact Signing pricing](https://azure.microsoft.com/en-us/pricing/details/artifact-signing/) ·
[CA/Browser Forum ballot CSC-31](https://cabforum.org/2025/11/17/ballot-csc-31-maximum-validity-reduction) (2025-11-17) ·
[SignPath terms](https://signpath.org/terms) ·
[winget manifest](https://learn.microsoft.com/en-us/windows/package-manager/package/manifest) (2026-09-14) ·
[winget submission](https://learn.microsoft.com/en-us/windows/package-manager/package/repository) (updated 2026-07-14).
macOS: [Apple Support 102445](https://support.apple.com/en-us/102445) (2026-05-27) ·
[Sequoia removes the Control-click override, AppleInsider](https://appleinsider.com/articles/24/08/06/apple-removes-control-click-option-for-skipping-gatekeeper-in-macos-sequoia) (2024-08-06) ·
[Running code that has not been notarized, Eclectic Light](https://eclecticlight.co/2026/08/11/how-can-you-run-code-that-hasnt-been-notarised/) (2026-08-11) ·
[Apple Developer Program enrollment](https://developer.apple.com/programs/enroll/) ·
[Homebrew/brew#20755](https://github.com/Homebrew/brew/issues/20755) (2025-09-23) ·
[Acceptable Casks](https://docs.brew.sh/Acceptable-Casks) ·
[.NET and macOS notarization](https://learn.microsoft.com/en-us/dotnet/core/install/macos-notarization-issues) (2026-04-23).
Linux: [AppImage type-2 runtime](https://github.com/AppImage/type2-runtime) ·
[appimagetool](https://github.com/AppImage/appimagetool/releases/continuous) ·
[AppImage best practices](https://docs.appimage.org/reference/best-practices.html) ·
[AppImage signatures](https://docs.appimage.org/packaging-guide/optional/signatures.html) ·
[Flathub requirements](https://docs.flathub.org/docs/for-app-authors/requirements) ·
[nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended).
Releases: [artifact attestations](https://docs.github.com/en/actions/security-for-github-actions/using-artifact-attestations/using-artifact-attestations-to-establish-provenance-for-builds) ·
[immutable releases](https://github.blog/changelog/2025-10-28-immutable-releases-are-now-generally-available/) (2025-10-28).
.NET: [license information](https://github.com/dotnet/core/blob/main/license-information.md) ·
[license information for Windows](https://github.com/dotnet/core/blob/main/license-information-windows.md) ·
[.NET Library License](https://dotnet.microsoft.com/en-us/dotnet_library_license.htm) ·
[support policy](https://dotnet.microsoft.com/en-us/platform/support/policy/dotnet-core) (updated 2026-10-25) ·
[releases-index.json](https://builds.dotnet.microsoft.com/dotnet/release-metadata/releases-index.json) ·
[.NET Conf 2026](https://dotnet.microsoft.com/en-us/conf) ·
[Native AOT overview](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/) (2026-11-03) ·
[Install .NET on Windows](https://learn.microsoft.com/en-us/dotnet/core/install/windows) (2026-06-01) ·
[dotnet-install scripts](https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-install-script) (updated 2026-09-03) ·
[.NET downloads](https://dotnet.microsoft.com/en-us/download/dotnet). The MIT declaration of the Windows runtime pack was
read from the `Microsoft.NETCore.App.Runtime.win-x64` 10.0.14 package itself, and the runtime and SDK sizes were
measured from Microsoft's 10.0.14 and 10.0.403 archives.

**Libraries.** [Jolt releases](https://github.com/jrouwe/JoltPhysics/releases) · [Tracy releases](https://github.com/wolfpld/tracy/releases) ·
[ImPlot releases](https://github.com/epezent/implot/releases) · [ozz-animation releases](https://github.com/guillaumeblanc/ozz-animation/releases) ·
[enkiTS releases](https://github.com/dougbinks/enkiTS/releases) · [SDL releases](https://github.com/libsdl-org/SDL/releases) ·
[KTX-Software releases](https://github.com/KhronosGroup/KTX-Software/releases)

### October 2026

**Unreal Engine.** [5.8 is available](https://www.unrealengine.com/news/unreal-engine-5-8-is-now-available) ·
[5.8 release notes](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes?lang=en-US) ·
[5.7 release notes](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-7-release-notes) ·
[5.8: Lumen Lite and MegaLights](https://www.guru3d.com/story/unreal-engine-58-debuts-lumen-lite-and-productionready-megalights/) ·
[The road to Unreal Engine 6](https://www.epicgames.com/site/news/unreal-fest-chicago-the-road-to-unreal-engine-6) ·
[UE6 merges UE5 and UEFN](https://www.gamedeveloper.com/programming/unreal-engine-6-will-merge-ue5-and-uefn-into-a-single-unified-engine-) ·
[MCP plugin hands-on](https://www.pugetsystems.com/?p=48127) ·
[Chaos Vehicles forum thread](https://forums.unrealengine.com/t/chaosvehicles/1333154) ·
[A custom motorcycle system for UE5](https://80.lv/articles/creating-a-custom-motorcycle-system-for-unreal-engine-5) ·
[License](https://www.unrealengine.com/en-US/license)

**Unity.** [6.3 LTS](https://unity.com/blog/unity-6-3-lts-is-now-available) ·
[6.5](https://discussions.unity.com/t/unity-6-5-is-now-available/1723176) ·
[6.6](https://discussions.unity.com/t/unity-6-6-is-now-available/1735357) ·
[Unite Seoul 2026 keynote](https://unity.com/blog/unite-seoul-keynote-2026-recap) ·
[Render pipelines strategy for 2026](https://unity.com/topics/render-pipelines-strategy-for-2026) ·
[Surface Cache GI](https://docs.unity.com/en-us/engine/6000.7/manual/lighting-overview/lighting/surface-cache-gi-volume-override-reference) ·
[Scriptable audio processors](https://docs.unity3d.com/Manual/audio-scriptable-processors.html) ·
[Path to CoreCLR](https://discussions.unity.com/t/path-to-coreclr-2026-upgrade-guide/1714279) ·
[Unity MCP overview](https://docs.unity3d.com/Packages/com.unity.ai.assistant@2.18/manual/integration/unity-mcp-overview.html) ·
[Runtime fee cancelled](https://unity.com/blog/unity-is-canceling-the-runtime-fee)

**Godot.** [4.5](https://godotengine.org/releases/4.5/) · [4.6](https://godotengine.org/releases/4.6/) ·
[4.7](https://godotengine.org/releases/4.7/) ·
[4.7.2](https://godotengine.org/article/maintenance-release-godot-4-7-2/) ·
[4.8 beta 1](https://godotengine.org/article/dev-snapshot-godot-4-8-beta-1/) ·
[Jolt in Godot](https://docs.godotengine.org/en/4.6/tutorials/physics/using_jolt_physics.html) ·
[Controllers and wheels](https://docs.godotengine.org/en/4.5/tutorials/inputs/controllers_gamepads_joysticks.html) ·
[Godot MCP servers](https://www.summerengine.com/blog/godot-mcp-servers-guide)

**O3DE.** [26.05 release notes](https://www.docs.o3de.org/docs/release-notes/2605-0-release-notes/) ·
[2025 in review](https://o3de.org/o3de-2025-in-review-and-a-look-into-2026/) ·
[Supported platforms](https://docs.o3de.org/docs/welcome-guide/supported-platforms/) ·
[Render pipelines](https://www.docs.o3de.org/docs/atom-guide/dev-guide/render-pipelines) ·
[Diffuse probe grid](https://docs.o3de.org/docs/user-guide/components/reference/atom/diffuse-probe-grid/) ·
[MiniAudio gem](https://docs.o3de.org/docs/user-guide/gems/reference/audio/miniaudio/miniaudio/)

**Racing games.** [MotoGP 25 on UE5](https://www.unrealengine.com/developer-interviews/milestone-leans-into-unreal-engine-5-to-keep-motogp-25-out-in-front) ·
[MotoGP 26 handling](https://traxion.gg/what-to-expect-from-motogp-26s-significant-riding-dynamics-change/) ·
[MotoGP 26 sim guide](https://www.redbull.com/int-en/motogp-26-new-sim-guide) ·
[Ride 6](https://www.gamedeveloper.com/press-release/milestone-announces-the-release-of-ride-6) ·
[GP Bikes](https://store.steampowered.com/app/848050/GP_Bikes/) · [GP Bikes news](https://gp-bikes.com/?page=news) ·
[EA Sports WRC on UE4](https://racinggames.gg/wrc/why-ea-sports-wrc-is-ditching-codemasters-ego-engine) ·
[Assetto Corsa Rally](https://traxion.gg/how-assetto-corsa-rally-is-being-developed-by-a-brand-new-team-of-racing-game-veterans) ·
[ACC at 400 Hz](https://traxion.gg/assetto-corsa-competizione-console-update-out-now-adds-new-tyre-model-and-400hz-physics) ·
[Moza MotoGP handlebar](https://www.visordown.com/news/moza-racing-launches-officially-licensed-motogp-gaming-handlebar-controller)

**BeamNG.drive.** [Physics](https://beamng.com/game/about/physics) ·
[JBeam physics theory](https://wiki.beamng.com/JBeam_Physics_Theory) · [Powertrain](https://wiki.beamng.com/Powertrain) ·
[Wheels](https://documentation.beamng.com/modding/vehicle/sections/wheels/) ·
[Virtual machines](https://documentation.beamng.com/modding/programming/virtualmachines/) ·
[BeamNG.tech architecture](https://documentation.beamng.com/beamng_tech/architecture/) ·
[v0.39](https://beamng.com/game/news/patch/beamng-drive-v0-39/) ·
[PS5 early access](https://www.gematsu.com/2026/09/beamng-drive-early-access-for-ps5-launches-october-19) ·
[Motorcycles thread](https://www.beamng.com/threads/motorcycles.71017/)

**engine-sim.** [Repository](https://github.com/ange-yaghi/engine-sim) ·
[Synthesizer](https://github.com/ange-yaghi/engine-sim/blob/master/src/synthesizer.cpp) ·
[Community edition](https://github.com/Engine-Simulator/engine-sim-community-edition) ·
[Patreon](https://www.patreon.com/atg_engine_simulator/about) ·
[3D engine simulator](https://www.thedrive.com/news/new-3d-engine-sim-can-synthesize-super-realistic-exhaust-sounds-in-real-time)

**Motorcycle dynamics and tires.** [Meijaard et al. 2007, benchmark bicycle](https://eklausmeier.lima-city.de/pdf/meijaard2007.pdf) ·
[Sharp, Evangelou and Limebeer 2004](https://spiral.imperial.ac.uk/handle/10044/1/4326) ·
[Wobble and weave](https://dynamotion.it/dinamoto/8_on-line_papers/wobble-weave/wobble-weave_eng.html) ·
[The chatter of racing motorcycles](https://www.dynamotion.it/dinamoto/8_on-line_papers/chattering/2007_02_1%20The%20chatter%20of%20racing%20motorcycles%20VSD.pdf) ·
[Engine spin direction](https://research.unipd.it/bitstream/11577/3243930/2/2018%20-%20The%20effect%20of%20engine%20spin%20direction%20on%20the%20dynamics%20of%20powered%20two%20wheelers%20PP%20-%20MM.pdf) ·
[Cossalter, Motorcycle Dynamics](https://www.abebooks.com/9781430308614/Motorcycle-Dynamics-Cossalter-Vittore-1430308613/plp) ·
[Limebeer and Massaro](https://academic.oup.com/book/43936) ·
[Pacejka, Tire and Vehicle Dynamics](https://shop.elsevier.com/books/tire-and-vehicle-dynamics/pacejka/978-0-08-097016-5) ·
[MF-Tyre/MF-Swift](https://functionbay.com/documentation/onlinehelp/Documents/Tire/MFTyre-MFSwift_Help.pdf) ·
[thermoRIDE](https://www.mdpi.com/2076-3417/10/5/1604) ·
[VI-BikeRealTime](https://www.vi-grade.com/dynatc/attachments-2355-1845/zps2024-vi-grade-vi-motorcycle-and-vi-bikerealtime-solutions.pdf) ·
[BikeSim math models](https://www.carsim.com/users/pdf/release_notes/bikesim/BikeSim_Math_Models.pdf) ·
[Rider steering patent](https://patents.google.com/patent/US10307670) ·
[Anti-squat](https://suspensionsecrets.co.uk/anti-squat-geometry)

**Bikes and rules.** [2025 Panigale V4 S review](https://www.cyclenews.com/2024/09/article/2025-ducati-panigale-v4-s-review/) ·
[Panigale V4 engine](https://www.ducati.com/ww/en/bikes/panigale/panigale-v4/engine) ·
[Ducati press release](https://mediahouse.ducati.com/new-ducati-panigale-v4-wonder-engineered-358833/) ·
[Desmosedici GP](https://www.ducati.com/ww/en/racing/motogp/ducati-lenovo-team-bike-desmosedici-gp) ·
[Ride-height devices](https://www.motogp.com/pt/news/2026/02/10/what-is-a-motogp-ride-height-device/873331) ·
[2027 technical rules](https://www.motorsport.com/motogp/news/full-details-of-motogps-2027-technical-regulation-changes/10607681/)

**Sound.** [Crankcase REV](https://www.audiokinetic.com/products/plug-ins/crankcase-rev/) ·
[From sampling to granular synthesis](https://www.audiokinetic.com/engine-sound-modeling-from-sampling-to-granular-synthesis-in-wwise) ·
[Physically informed engine sound synthesis](https://air.iuav.it/handle/11578/264484) ·
[Pulse-train resonators](https://arxiv.org/abs/2603.09391) ·
[Wind noise in helmets](https://swov.nl/en/publicatie/wind-noise-motorcycle-helmets)

**Tracks and telemetry.** [iRacing track technology](https://www.iracing.com/track-technology/) ·
[Assetto Corsa surfaces](https://www.sites.google.com/site/hagn99/assettocorsa/modding/tracks/data/surfaces-ini) ·
[F1 UDP telemetry](https://www.f1laps.com/faqs/f1-25-udp-telemetry-settings/) ·
[SimHub games](https://www.simhubdash.com/supported-games/) · [ldparser](https://github.com/gotzl/ldparser)

**Jolt.** [Releases](https://github.com/jrouwe/JoltPhysics/releases) ·
[MotorcycleController.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Vehicle/MotorcycleController.h) ·
[WheeledVehicleController.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Vehicle/WheeledVehicleController.h) ·
[Architecture](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)

**Libraries.** [Tracy](https://github.com/wolfpld/tracy) · [enkiTS](https://github.com/dougbinks/enkiTS) ·
[KTX-Software](https://github.com/KhronosGroup/KTX-Software) · [Basis Universal](https://github.com/BinomialLLC/basis_universal) ·
[bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) · [meshoptimizer](https://github.com/zeux/meshoptimizer) ·
[FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) · [Streamline](https://github.com/NVIDIA-RTX/Streamline) ·
[XeSS](https://github.com/intel/xess) · [ozz-animation](https://github.com/guillaumeblanc/ozz-animation) ·
[SDL](https://github.com/libsdl-org/SDL) · [Steam Audio](https://github.com/ValveSoftware/steam-audio) ·
[miniaudio](https://github.com/mackron/miniaudio) · [Signalsmith DSP](https://github.com/Signalsmith-Audio/dsp) ·
[HarfBuzz](https://github.com/harfbuzz/harfbuzz) · [msdfgen](https://github.com/Chlumsky/msdfgen) ·
[RmlUi](https://github.com/mikke89/RmlUi) · [recastnavigation](https://github.com/recastnavigation/recastnavigation) ·
[ImPlot](https://github.com/epezent/implot) · [imgui-node-editor](https://github.com/thedmd/imgui-node-editor) ·
[SPIRV-Reflect](https://github.com/KhronosGroup/SPIRV-Reflect) · [netcoredbg](https://github.com/Samsung/netcoredbg) ·
[yojimbo](https://github.com/mas-bandwidth/yojimbo) · [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) ·
[OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) · [sentry-native](https://github.com/getsentry/sentry-native) ·
[Project Chrono tires](https://api.projectchrono.org/wheeled_tire.html) · [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) ·
[KosmicKrisp conformance](https://www.khronos.org/news/permalink/kosmickrisp-achieves-vulkan-1.4-conformance-on-apple-silicon) ·
[MoltenVK](https://github.com/KhronosGroup/MoltenVK)
