# Strada documentation

Everything Strada can do, and how to use it. Start with [Getting started](GettingStarted.md) if you are new; the
inventory below lists what the engine has, area by area, with the page that covers each part.

## The documents

| Document | Read it to |
|---|---|
| [GettingStarted.md](GettingStarted.md) | Build Strada, open the sample games, make a first game step by step, test it and export it |
| [Editor.md](Editor.md) | Learn the editor: panels, the scene view, assets and materials, prefabs, settings, play mode, Build Game, shortcuts |
| [Scripting.md](Scripting.md) | Write C# scripts: fields, entities and components, input, physics, sound, UI, particles, testing, exporting |
| [VisualScripting.md](VisualScripting.md) | Make visual scripts: graphs, flow, functions, formulas, timelines, graphs that talk, debugging, live editing |
| [Agents.md](Agents.md) | Build games with AI agents through the editor's MCP tools, and debug their graphs |
| [Components.md](Components.md) | Look up a component: every field with its type, default, range and meaning (generated) |
| [ScriptingAPI.md](ScriptingAPI.md) | Look up the scripting API: every public type and member with its summary (generated) |
| [Automation.md](Automation.md) | Look up an automation command: the protocol, every command's parameters and results, the `strada` tool |
| [Architecture.md](Architecture.md) | Understand how the engine is built: modules, rendering, ECS, physics, audio, scripting, editor, runtime |
| [Roadmap.md](Roadmap.md) | See where Strada is going: the backlog, the efforts and their schedule, the technical plan of a motorcycle game, risks and open decisions |
| [EngineComparison.md](EngineComparison.md) | See where Strada stands against Unreal Engine, Unity, Godot and O3DE, how those engines reach their users, and what a motorcycle game needs |

The root [README](../README.md) shows the engine in screenshots, and [AGENTS.md](../AGENTS.md) is the guide for
working on the engine itself (building, style, tests, commits).

## What Strada has

| Area | What is there | Where |
|---|---|---|
| Platforms | Windows, macOS and Linux; Vulkan through NVRHI (MoltenVK on macOS); one code base built with MSVC, GCC or Clang | [GettingStarted.md](GettingStarted.md) |
| Rendering | Forward PBR (metallic-roughness, GGX), image-based lighting from HDR environments, a sky, cascaded PCSS soft shadows for the sun and shadow maps for spot and point lights, GTAO, HDR with bloom and four tonemappers, FXAA, post-process volumes (exposure, grading, vignette, chromatic aberration, film grain), particles, world text and sprites, frustum and light culling | [Components.md](Components.md#rendering), [Architecture.md](Architecture.md#6-rendering) |
| Scenes | Entities with 28 components in 6 categories (201 fields), hierarchies, prefabs, JSON scene files, scene and renderer settings per scene | [Components.md](Components.md), [Editor.md](Editor.md) |
| Assets | Scenes (`.sscene`), prefabs (`.sprefab`), models (`.gltf`, `.glb`, `.fbx`, `.obj`) with their materials and textures, materials (`.smat`), textures (`.png`, `.jpg`, `.tga`, `.bmp`), HDR environments (`.hdr`), audio (`.wav`, `.mp3`, `.flac`, `.ogg`), fonts (`.ttf`, `.otf`), visual scripts (`.sgraph`); built-in shapes, materials and a default font and sky | [Editor.md](Editor.md) |
| Physics | Jolt: static, dynamic and kinematic bodies; box, sphere, capsule and mesh colliders; triggers; 16 layers; raycasts; fixed, hinge, ball, distance and slider joints; character controllers that walk, climb, jump and push | [Components.md](Components.md#physics), [Scripting.md](Scripting.md#physics) |
| Audio | miniaudio: 2D and 3D sources, listeners, streamed music and ambience | [Components.md](Components.md#audio) |
| UI | Screen-space canvases, anchored rectangles, images with rounded corners, wrapped text, buttons that scripts hear | [Components.md](Components.md#ui), [Scripting.md](Scripting.md#huds-and-menus) |
| C# scripting | .NET 10 hosted in the engine, hot reload in the editor, inspector fields, 84 API types with 737 members and enum values | [Scripting.md](Scripting.md), [ScriptingAPI.md](ScriptingAPI.md) |
| Visual scripting | Typed graphs on a VM: 92 built-in node kinds plus a node for every API member and every C# member marked `[VisualNode]`; functions, libraries, formulas, timelines, events between graphs; a debugger with conditional breakpoints and stepping across graphs; live editing while the game plays | [VisualScripting.md](VisualScripting.md) |
| Editor | Docking, multi-viewport UI; scene and game views with gizmos and overlays; hierarchy; generated inspector; content browser with previews; undo and redo for every edit of scenes, materials and graphs; play, pause, step and simulate; a node editor for graphs; Build Game | [Editor.md](Editor.md) |
| AI agents | 96 automation commands in 17 domains (27 of them for graphs), served as MCP tools by `strada mcp` together with Poly Haven search and import (102 tools); a command-line tool, `strada` | [Agents.md](Agents.md), [Automation.md](Automation.md) |
| Player and export | `StradaRuntime` plays projects and exported games, windowed or headless, and runs test scenes with a fixed time step; Build Game writes a self-contained game folder | [GettingStarted.md](GettingStarted.md), [Editor.md](Editor.md) |
| Testing | `Strada.Testing` checks in C# or in graphs, run in the editor, by agents or by the player in CI | [Scripting.md](Scripting.md#testing-a-game) |
| Samples | `Showcase` (every feature in a forest clearing), `PrismBreak` (a game made of graphs alone), `Blocks` (a falling-blocks game in C#), `FeatureTest` (every component and the whole API, checked) | [GettingStarted.md](GettingStarted.md) |

## How these pages stay accurate

The references are generated from the engine, and the engine's tests fail when a page and the engine disagree:

- [Components.md](Components.md) comes from the component registry (StradaTests,
  `Tests/StradaTests/Source/Scene/ComponentReferenceTests.cpp`) and [ScriptingAPI.md](ScriptingAPI.md) from the
  scripting API and its XML documentation (ScriptCoreTests, `Tests/ScriptCoreTests/Source/ApiReferenceTests.cs`). After
  changing a component's fields or descriptions, or the API, run that suite with `STRADA_UPDATE_DOCS=1` and commit the
  regenerated page.
- Every automation command must have its row in [Automation.md](Automation.md) (StradaEditorTests).
- The examples the guides show are the code the tests run: the C# examples of [Scripting.md](Scripting.md), the
  example batch and the agent's debugging session of [VisualScripting.md](VisualScripting.md), and the table of Prism
  Break's graphs.
- The visual script editor's shortcuts in [Architecture.md](Architecture.md) §11 are checked against the editor's.

The guides are written by hand: when a change alters what a guide says, the same change updates the guide.
