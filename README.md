# Strada

Strada is a production-grade, straightforward 3D game engine for **Windows, macOS and Linux** written in modern C++20,
with C# (.NET 10) scripting, a Vulkan renderer built on NVRHI, an ImGui editor that AI agents can fully control, and
an exporter for distributable games.

> **Status: in development.** In place: the engine core (application loop, windowing, input, events, logging, file
> system, command line), the Vulkan/NVRHI device and swapchains, build-time shader compilation, the ImGui renderer with
> docking and multi-viewports, the ECS scene system (components, hierarchy, reflection-driven JSON scenes, prefabs),
> projects, the asset system (registry, glTF/FBX/OBJ import, materials, textures, HDR environments, built-in
> primitives and font), the forward PBR scene renderer (image-based lighting, soft shadows, GTAO, bloom, FXAA,
> tonemapping, signed-distance text and sprites), Jolt physics and miniaudio sound (spatial audio sources and
> listeners) driven by the scene runtime, C# scripting with hot reload, the editor (viewport with gizmos and picking,
> hierarchy, generated inspector, content browser, project settings, undo/redo, play mode, AI automation server), the
> game player, game export, the `strada` tool with its MCP server for AI agents, a feature-test project exercising
> every component and the whole scripting API, and CI on all three platforms.
> [Docs/Architecture.md](Docs/Architecture.md) describes the complete design.

## Planned feature set

- **Renderer**: Vulkan (MoltenVK on macOS) through NVRHI; PBR metallic-roughness materials, image-based lighting from
  HDRIs, soft cascaded shadow maps, GTAO ambient occlusion, HDR pipeline with bloom and tonemapping.
- **Scene**: EnTT-based entities and components, hierarchies, prefabs, JSON scene files.
- **Assets**: glTF/GLB, FBX and OBJ import with materials and textures.
- **Physics**: Jolt Physics, authored through components and controlled from scripts.
- **Audio**: miniaudio with 3D spatialization.
- **Scripting**: C# on .NET 10, hosted in-process, with hot reload in the editor.
- **Editor**: docking + multi-viewport ImGui, gizmos, undo/redo, play mode, content browser.
- **AI automation**: every editor operation is available to agents through an automation server and an MCP bridge.
- **Export**: one command produces a self-contained game folder for the host platform.

## Building

Prerequisites:

| | Windows | macOS | Linux (Ubuntu 24.04+) |
|---|---|---|---|
| Compiler | Visual Studio 2022 17.10+ or 2026 (C++ workload) | Xcode 15+ | GCC 13+ or Clang 17+ |
| CMake / Ninja | bundled with Visual Studio | `brew install cmake ninja` | `apt install cmake ninja-build` |
| Other | Python 3.10+ | Python 3.10+ | Python 3.10+, `apt install pkg-config libwayland-dev libxkbcommon-dev xorg-dev libgtk-3-dev` |

The [Vulkan SDK](https://vulkan.lunarg.com/) 1.4.x is required (DXC shader compiler; validation layers for Debug
builds), and a GPU driver with Vulkan 1.3 support to run the editor. The [.NET 10
SDK](https://dotnet.microsoft.com/download/dotnet/10.0) builds the scripting layer, the `strada` tool and the C# tests.

```sh
python Tools/build.py --test                    # Debug build + all tests
python Tools/build.py --config release --test   # Release build + all tests
```

Third-party libraries are downloaded automatically by CMake (pinned versions, verified by SHA256). See
[AGENTS.md](AGENTS.md) for presets, options and the development workflow.

## Repository layout

| Path | Contents |
|------|----------|
| `Strada/` | Engine static library |
| `StradaEditor/` | Editor (`StradaEditor --help` lists its options) |
| `StradaRuntime/` | Game player (`StradaRuntime --help` lists its options) |
| `Strada-ScriptCore/` | C# scripting API |
| `StradaTool/` | `strada`: command-line tool and MCP server for the editor (`strada --help`) |
| `Tests/` | Test suites |
| `cmake/` | Build system modules |
| `Tools/` | Build and formatting scripts |
| `Docs/` | Architecture and design documentation |
| `.claude/skills/` | Development playbooks for AI agents |

## AI agents

Agents control the editor through `strada mcp`, a Model Context Protocol server whose tools are the editor's
automation commands: they create projects, scenes, entities, materials and scripts, play and test the game, and export
it. In this repository `.mcp.json` registers it for Claude Code; build the engine first, and strada attaches to a
running editor or starts one. [Docs/Automation.md](Docs/Automation.md) describes the tools and how to register them for
a game project of your own.

## Contributing

Read [AGENTS.md](AGENTS.md) first. In short: Hazel naming conventions, Allman braces, east const, tabs; every change
ships with tests; every commit is reviewed before it is made; CI must pass on all three platforms.

## Third-party software

See [ThirdPartyNotices.md](ThirdPartyNotices.md).
