# Strada

Strada is a production-grade, straightforward 3D game engine for **Windows, macOS and Linux** written in modern C++20,
with C# (.NET 10) scripting, a Vulkan renderer built on NVRHI, an ImGui editor that AI agents can fully control, and
an exporter for distributable games.

> **Status: early development.** In place: the engine core (application loop, windowing, input, events, logging, file
> system, command line), the Vulkan/NVRHI device and swapchains, build-time shader compilation, the ImGui renderer with
> docking and multi-viewports, the ECS scene system (components, hierarchy, reflection-driven JSON scenes, prefabs),
> the asset system (registry, glTF/FBX/OBJ import, materials, textures, HDR environments, built-in primitives), the
> editor shell, screenshots, and CI on all three platforms. The scene renderer, physics, audio, scripting, full editor
> and exporter are being built next.
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
| Other | Python 3.10+ | Python 3.10+ | Python 3.10+, `apt install pkg-config libwayland-dev libxkbcommon-dev xorg-dev` |

The [Vulkan SDK](https://vulkan.lunarg.com/) 1.4.x is required (DXC shader compiler; validation layers for Debug
builds), and a GPU driver with Vulkan 1.3 support to run the editor. The scripting layer will additionally require the
[.NET 10 SDK](https://dotnet.microsoft.com/download/dotnet/10.0).

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
| `Tests/` | Test suites |
| `cmake/` | Build system modules |
| `Tools/` | Build and formatting scripts |
| `Docs/` | Architecture and design documentation |
| `.claude/skills/` | Development playbooks for AI agents |

## Contributing

Read [AGENTS.md](AGENTS.md) first. In short: Hazel naming conventions, Allman braces, east const, tabs; every change
ships with tests; every commit is reviewed before it is made; CI must pass on all three platforms.

## Third-party software

See [ThirdPartyNotices.md](ThirdPartyNotices.md).
