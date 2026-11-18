# Strada Engine — Architecture

This document is the architectural contract for Strada. Every subsystem is implemented against it, and changes to
the contract must be made here first. Coding rules live in [AGENTS.md](../AGENTS.md).

## 1. Goals

- Production-grade, simple and straightforward 3D engine for **Windows, macOS and Linux (Ubuntu 24.04+)**.
- Stable and fully tested: unit tests for every module, a feature-test project that exercises every component and
  the entire scripting API, golden-image renderer tests, and CI on all three platforms.
- An editor to build games, and an **export** step that produces a distributable game without editing ability.
- The editor is **fully controllable by AI agents** (automation server + MCP bridge + CLI), so an agent can build a
  complete game (e.g. Tetris) without human intervention.

## 2. Technology

| Area | Choice |
|------|--------|
| Language / build | C++20, CMake ≥ 3.28 (tested with 4.x), Ninja (all platforms) and Visual Studio generators |
| Window / input | GLFW 3.4 (X11 + Wayland on Linux) |
| Graphics | NVRHI over **Vulkan 1.3** (dynamic rendering, synchronization2, timeline semaphores; MoltenVK on macOS); HLSL shaders compiled to SPIR-V with DXC at build time and embedded in the binary |
| Math | glm (right-handed, Y-up, column-major, depth 0..1) |
| ECS | EnTT |
| Physics | Jolt Physics |
| Audio | miniaudio (playback; WAV, FLAC, MP3), libvorbis + libogg (Ogg Vorbis) |
| Scripting | C# on .NET 10, hosted through hostfxr/nethost |
| Editor UI | Dear ImGui (docking branch, docking + multi-viewports) + ImGuizmo |
| Mesh import | cgltf (glTF 2.0/GLB), ufbx (FBX, OBJ/MTL), MikkTSpace (tangents) |
| Images / fonts | stb_image, stb_image_write, stb_truetype |
| Serialization | nlohmann/json (`ordered_json` for files) |
| Logging | spdlog |
| Processes | reproc++ (script builds, editor launching) |
| File dialogs | nativefiledialog-extended (editor only, so exported games do not depend on GTK) |
| Tests | doctest (C++), xUnit (C#) |

Exact pinned versions live in `cmake/StradaDependencies.cmake` and `ThirdPartyNotices.md`.

## 3. Repository layout

```
/
├── AGENTS.md, CLAUDE.md, README.md, ThirdPartyNotices.md
├── CMakeLists.txt, CMakePresets.json, cmake/          Build system (options, compiler settings, deps, shaders, .NET)
├── Strada/                                           Engine static library
│   ├── Source/stpch.h, Source/Strada.h               Precompiled header, public umbrella header
│   ├── Source/Strada/<Module>/...                    Engine modules (see §4)
│   ├── Shaders/                                      HLSL (*.hlsl, *.hlsli) → embedded SPIR-V
│   └── ThirdParty/                                   Implementations of single-header libraries (stb, cgltf, miniaudio)
├── StradaEditor/                                     Editor: StradaEditorCore static lib + StradaEditor executable
│   └── Source/Editor/...                             Panels, automation, commands (undo/redo), export, project management
├── StradaRuntime/                                    Runtime/player executable used for exported games
├── Strada-ScriptCore/                                C# scripting API, visual script VM (Strada.ScriptCore.dll)
├── StradaTool/                                       C# CLI + MCP stdio bridge (executable `strada`)
├── Tests/
│   ├── StradaTests/                                  C++ unit tests (doctest) for the engine library
│   ├── StradaEditorTests/                            C++ tests for editor automation commands (headless)
│   ├── StradaRuntimeTests/                           C++ tests running the game player on generated games
│   ├── ScriptCoreTests/                              C# unit tests (math, API coverage, the visual script VM)
│   ├── StradaToolTests/                              C# tests of strada (MCP server, sessions with the headless editor)
│   ├── TestScripts/                                  C# script assembly used by C++ scripting tests
│   └── Data/                                         Test assets, visual script documents, golden images, engine content
├── Projects/FeatureTest/                             Project whose scene exercises every feature and the whole script API
├── Projects/Blocks/                                  Sample game (falling blocks) made through the agent tools
├── Projects/Showcase/                                Showcase of every feature (exhibits, camera tour, Poly Haven assets)
├── Projects/catalog.json, Projects/Thumbnails/       The catalog of the examples the editor copies, and their thumbnails
├── Templates/                                        Project templates (<id>/template.json, thumbnail.png, a project)
├── Templates/csharp-starter/Scripts/Source/          The C# template's sources, written by hand
├── Templates/Tools/                                  The tool that builds the templates through the agent tools
├── Agents/                                           Agent kit: the AGENTS.md block of projects set up for coding agents
├── Tools/                                            Developer scripts (configure/build/test, formatting)
├── Docs/                                             Architecture contract, automation reference, scripting guide
└── .github/workflows/, .claude/skills/, .mcp.json    CI, agent skills, MCP configuration
```

The engine library precompiles `Source/stpch.h`, which its sources include. `StradaEditorCore` and the C++ test
executables precompile the third-party and standard headers that `strada_precompile_third_party_headers`
(`cmake/StradaCompilerSettings.cmake`) lists, and CMake forces that header into their sources, which include none.
GCC builds these targets without it: with it, CI's GCC 13 Release job reported `-Warray-bounds` errors in libstdc++'s
string concatenation that it did not report without it. Each source still includes what it uses: CI's Linux Clang
Debug job builds these targets without that header (`STRADA_PRECOMPILE_THIRD_PARTY_HEADERS=OFF`), so a missing
include fails there.

Build output: `build/<preset>/bin/` contains every executable, `Strada.ScriptCore.dll` and the `strada` tool. Fonts
and other binary resources are compiled into the binaries (`cmake/StradaResources.cmake`, `EmbeddedResources`).
Beside the executables CMake writes the build-tree marker `engine.json` (`cmake/StradaEngineManifest.cmake`, §7.3),
which names the checkout: `EngineLayout` (`Strada/Core/EngineLayout.h`) reads the engine content (`EngineContent`:
templates, examples, guides, the agent kit, the game-making skill) from the checkout while an executable runs from the
build tree, and from the resources directory (the executable's folder, `Contents/Resources` in a macOS bundle)
otherwise:

| `EngineContent` | Source tree (C = the checkout) | Otherwise (R = the resources directory) |
|---|---|---|
| Templates | `C/Templates` | `R/Templates` |
| Examples | `C/Projects` | `R/Examples` |
| Docs | `C/Docs` | `R/Docs` |
| AgentKit | `C/Agents` | `R/Agents` |
| MakeGameSkill | `C/.claude/skills/strada-make-game` | `R/Agents/skills/strada-make-game` |

`EngineLayout::Resolve(executablePath)` is pure (tests resolve fake trees, and `Tests/Data/FrontDoor/Content` is a tiny
tree of the right-hand layout); `EngineLayout::Get()` is the process's, resolved once and readable from any thread, and
the entry point logs what was wrong with its marker after `Log::Init`. The files exports take from beside the
executables (the player, the scripting API, `Redist/`, the Vulkan files of macOS bundles) are not engine content: they
are looked up beside the executables.

## 4. Engine library modules (`Strada/Source/Strada/`)

Layering (a module may include only modules listed to its left):

```
Core ← Math ← Serialization ← Platform ← RHI ← Asset ← Renderer ← Physics, Audio ← Script ← Scene ← Project
                                                     ImGui (RHI, Core)
```

`Scene` sits on top: it owns the ECS and drives the physics/audio/script runtimes and submits to the renderer.
Physics, Audio and Script read component data through `Scene`/`Entity` forward declarations, never the reverse
through includes of `Scene.h` in headers. Three Core translation units reach to the right, never their headers:
`Application.cpp` starts the subsystems above Core, `EngineLayout.cpp` reads its marker with Serialization's file
header functions, as every Strada file is read, and `EntryPoint.cpp` reads the identity of an exported game beside the
executable with Project's reader before logging starts (§13.1).

| Module | Responsibility | Key types |
|--------|----------------|-----------|
| Core | Base macros, logging, asserts, UUID, time, buffers, events, input, window, application loop (and its main-thread queue), layers, file system, OS queries (the user's Documents folder among them), the engine's layout (where its content lies), images, embedded resources, command line, the profiler's recorder (§4.4), the per-user folders and the game data service (§13.1), input actions' runtime and the capture that rebinds them (§7.4) | `Application`, `Layer`, `Window`, `Input`, `InputActionSet`, `InputActionState`, `InputControlCapture`, `Log`, `UUID`, `Timestep`, `Buffer`, `FileSystem`, `Platform`, `EngineLayout`, `Image`, `Result<T>`, `Profiler`, `UserDataLocations`, `GameData` |
| Math | Transform compose/decompose, projections, sRGB transfer functions, bounding boxes, view frustums, curves (§6.5) | `Math::ComposeTransform`, `Math::DecomposeTransform`, `Math::PerspectiveReversedZ`, `AABB`, `Frustum`, `Curve`, `EvaluateCurveKeys` |
| Serialization | JSON conversions for glm, UUIDs, enums and reflected structs; deserialization warnings; versioned documents of one struct, with migrations; profiler captures (§7.3) | `JsonTraits<T>`, `EnumTraits<T>`, `StructTraits<T>`, `DeserializationContext`, `StructDocumentFormat`, `ProfilerCaptureSerializer` |
| Platform | Child processes (reproc++): spawning, output capture, exit codes, termination | `Process` |
| RHI | Vulkan instance/device creation, NVRHI device, swapchains (one per OS window), frame pacing, embedded shader library, texture readback | `GraphicsDevice`, `Swapchain`, `ShaderLibrary`, `ReadbackTexture` |
| Asset | Asset handles and metadata, the persistent registry, the asset manager, CPU asset types, data asset types (one registration each, with their structs), model importers, built-in primitives, the visual script format and its layout, the player's rebound input bindings and their profile (§7.4) | `AssetHandle`, `AssetManager`, `AssetRegistry`, `MeshSource`, `MaterialAsset`, `TextureAsset`, `EnvironmentAsset`, `PrefabAsset`, `AudioClipAsset`, `FontAsset`, `VisualScriptAsset`, `VisualScriptSerializer`, `MeshImporter`, `DataAssetRegistry`, `DataAsset`, `TypedDataAsset<T>`, `DataAssetSerializer`, `PhysicsMaterial`, `InputActionMap`, `InputRebinding`, `InputBindingProfile` |
| Renderer | GPU resources created from assets (meshes, textures, materials, environments, fonts), samplers and fallback textures, the scene renderer and its passes, environment processing, shadow math, text layout, sprites, picking, presentation blits | `Renderer`, `SceneRenderer`, `EnvironmentProcessor`, `QuadRenderer`, `FontAtlas`, `TextureBlitter` |
| ImGui | ImGui context/layer, NVRHI renderer backend with multi-viewport support, UI helpers | `ImGuiLayer`, `ImGuiRenderer` |
| Physics | Jolt init/shutdown, per-scene physics world, layers, contact events, queries | `PhysicsSystem`, `PhysicsScene` |
| Audio | miniaudio engine, the audio queue, voices, buses and their effects (the mixer, §9), per-scene sound sources and listener, waveforms of clips | `AudioEngine`, `AudioScene`, `AudioMixer`, `AudioVoice`, `AudioBusNode`, `AudioEffect`, `ComputeAudioWaveform` |
| Script | .NET runtime hosting (hostfxr), managed bridge, native bindings, script instances and fields, hot reload, the public variables of visual scripts as script fields, the visual scripts of the runtime (node library, analysis, programs, graph instances), the literal codec of visual scripts (the C++ forms of document literals: floats written as their shortest text that reads back exactly, integral floats as integers) | `ScriptEngine`, `DotNet::LoadAssembly`, `ScriptBinding`, `DescribeVisualScriptVariables`, `VisualScriptEngine`, `VisualScriptLibrary`, `DecodeVisualScriptLiteral`, `EncodeVisualScriptLiteral` |
| Scene | ECS scene, entities, components, component registry/reflection, serialization, prefab instantiation, runtime lifecycle (C# scripts and visual scripts), post-process volume blending, screen-space UI layout and interaction, render submission | `Scene`, `Entity`, `ComponentRegistry`, `ComponentTraits`, `SceneSerializer`, `PostProcessVolumes`, `UILayout`, `RenderScene` |
| Project | Project files and settings, the C# script project, the scene runner (play mode and the player), game export | `Project`, `ProjectSettings`, `ScriptProject`, `SceneRunner`, `GameExporter` |

### 4.1 Subsystem lifetime

Subsystems are static facades with explicit `Init()`/`Shutdown()` (Hazel style). The entry point initializes the log;
`Application` then initializes, in this order: Profiler (§4.4) → AssetManager → PhysicsSystem → AudioEngine (a null
device when headless) → ScriptEngine (optional: without .NET, scenes run without scripts) → the window (unless
headless) → GraphicsDevice → ShaderLibrary → Renderer → the ImGui layer. The graphics subsystems are skipped when
`EnableGraphics` is off (the headless player), and applications that do not require them (the headless editor) continue
without a GPU when none is available. Shutdown detaches the layers, then shuts down scripting, audio, physics and the
assets (before the renderer, which owns the GPU objects created from them), the graphics, the window and, last, the
Profiler. Every `Init` asserts it is not already initialized and every `Shutdown` must leave no state behind so tests
can init/shutdown repeatedly.

### 4.2 Threading model

- The **main thread** owns the ECS, assets, rendering submission, scripting and editor state.
- Jolt runs its own job threads; contact callbacks only enqueue events (thread-safe queue) drained on the main thread.
- miniaudio runs its own audio thread, which mixes (§9.1). The main thread changes the mix only through the audio
  queue and never calls miniaudio on an object the mixing thread holds; objects are created and destroyed on the main
  thread and handed over and back through the queue and its return list. The audio streaming thread reads and decodes
  streamed sounds into their ring buffers, which the audio thread copies from without waiting.
- Background work (script builds, exports, the automation socket) never touches engine state; results are
  marshalled with `Application::SubmitToMainThread(std::function<void()>)`. The editor notices edited scripts by
  checking their modification times on the main thread once a second, and rescans the assets on the main thread when
  it gets the focus back.
- The game data service (§13.1) is filled and emptied on the main thread and read from any.
- The profiler (§4.4) is the one engine service every thread writes to: any thread records zones into a buffer of its
  own without allocating or locking, and only the main thread collects them.

### 4.3 Error handling

- No exceptions cross module boundaries. Third-party code that throws (nlohmann::json) is wrapped at the boundary.
- Fallible operations return `Result<T>` (value or error message) or `bool` plus a logged error.
- `ST_CORE_ASSERT`/`ST_ASSERT` are for programmer errors only. User data (scene files, assets, scripts, automation
  requests) must never crash the engine: validate, log, and report the error.
- Crashes leave a report: the entry point installs `CrashHandler`, which writes
  `<user data>/Crashes/<executable>-<process ID>.txt` (`<game data>/Crashes` for an exported game, §13.1) on a fatal
  exception or signal, `abort()` (failed asserts) or an
  uncaught C++ exception: the cause, the stack on Linux and macOS, and on Windows a minidump next to it. It writes
  without allocating or logging (the crash may be inside either); uncaught exceptions are logged with their message
  first. .NET's own handlers come first for managed code and pass native crashes on.

### 4.4 Profiling

`Strada/Core/Profiler.h` is the engine's own recorder of CPU zones and GPU pass times. It is not a port of Tracy: it
adds no dependency and opens no port (the authenticated automation socket stays the only one). It lies in Core so that
every module can record, and everything that reads it lies to its right: Serialization's `ProfilerCaptureSerializer`
writes and reads captures (§7.3), each `SceneRenderer` hands the recorder its GPU pass durations, and the editor's
Profiler panel, the `profiler.*` commands and `StradaRuntime --profile` read its history.

- **Zones.** `ST_PROFILE_SCOPE(name)` takes a name with static storage duration and `ST_PROFILE_FUNCTION()` uses
  `__func__`. `ST_PROFILE_SCOPE_DYNAMIC(name)` takes a name known only at run time (a script class, a graph).
  `ST_PROFILE_ZONE(variable, name)` and `ST_PROFILE_ZONE_DYNAMIC` declare a zone that `ST_PROFILE_ZONE_VALUE` can give
  a count to (a graph's VM steps). `ST_PROFILE_THREAD(name)` names a thread for its whole body, or a pooled thread for
  one task: every `std::async` or pooled task body that can reach a zone opens one. Code whose scopes are
  not C++ scopes calls `Profiler::BeginZone`/`EndZone` and keeps the token: Jolt's `ExternalProfileMeasurement` has 64
  bytes of user data, and the token is trivially copyable and smaller than that (the header asserts both). Zones that a
  subsystem's time is read from are named `<Subsystem>.<Phase>` (`Physics.Step`, `Scene.UpdateScripts`). A name without
  a `.` belongs to the subsystem of its nearest enclosing zone on its thread that has one, and to `Other` when none has,
  so function names and Jolt's own zones need no prefix. Names given at run time carry one, because class names and
  asset paths hold dots of their own: a C# script's zones are `Script.<class>` and a graph's `Graph.<asset path>`. A
  subsystem's figure is the main thread's: per frame,
  the summed durations of its outermost zones on the main thread, the frame's wall time spent in it. The work other
  threads do in parallel (Jolt's workers, the audio and streaming threads) is measured per thread as busy time, and is
  never added to a subsystem, so Jolt's own unprefixed zones need no subsystem.
- **Where the engine records.** These zones are the figures plan targets are read from:
  - the main loop: `Application.Events`, `Application.MainThreadQueue`, `Application.BeginFrame` (the swapchain image and
    its clear), `Application.Update` (the layers), `Application.ImGui` and `Application.Present`; the waits are a
    subsystem of their own, `Wait.Gpu` (`GraphicsDevice::EndFrame`) and `Wait.FrameLimit`, so that `Application`'s
    figure is work;
  - `Scene.Update` around `Scene::OnUpdateRuntime`'s phases, in their order: `Scene.InputActions` (with
    `Input.EvaluateActions` inside it, §7.4), `Scene.UpdateUI`, `Scene.UpdateScripts`,
    `Scene.UpdatePhysics`, `Scene.ContactEvents`, `Scene.UpdateAudio` (with `Audio.ReverbZones`, the sends of the
    reverb zones, and `Scene.ImpactSounds` in the frames with collisions while the mixer plays impact sounds, inside it,
    §9.8), `Scene.UpdateParticles`, `Scene.Destroy`; in
    `Scene.UpdatePhysics`, `Scene.ReturnToSteps` before the frame's steps and `Scene.Interpolate` after them (§8); in
    each fixed step `Scene.FixedUpdateScripts`, `Scene.SyncToPhysics`, `Physics.Step` and `Scene.SyncFromPhysics`;
    `Scene.SyncPhysicsMaterials` only in the frames after a data asset changed, where the bodies take the new values of
    their physics materials (§8);
  - each C# script's update and fixed update under `Script.<class>`, and each graph invoke (every event) under
    `Graph.<asset path>` with the count `Steps`: the steps of the VM's outermost executions the invoke ran, the graphs
    they ran in turn included. The engine reads the VM's step counter (`VisualScriptReadSteps`) before and after the
    invoke only while recording, so an idle profiler adds no call;
  - Jolt's own zones (`JPH_PROFILE`) in Debug builds only: Jolt defines `JPH_EXTERNAL_PROFILE` there
    (`cmake/StradaDependencies.cmake`), and `PhysicsSystem.cpp` defines its `ExternalProfileMeasurement` as a zone whose
    token lives in the measurement's 64 bytes. Release and Dist builds compile them out of every step; whether they would
    cost under 1 % of a Release frame is not measured;
  - `Audio.Mix` around `ma_engine_read_pcm_frames`: on miniaudio's device thread through a device data callback of the
    engine's own (`ma_engine_config::dataCallback`, `miniaudio.h:11290`), which does what miniaudio's own callback does
    and records on the engine's `ProfilerThreadState` named `Audio Mixer` (see "Who writes" below), and in
    `AudioEngine::ReadFrames` (manual output, on the calling thread), around the mixer's blocks (§9.1);
    `Audio.Stream` around each pass of the streaming thread (`Audio Streaming`), never its waits; `Audio.Update` around
    the audio engine's frame work on the main thread (the return list, the one-shots that ended, the virtual voices, the
    deferred commands, a device that stopped); `Audio.Capture` around the player's frame of `--audio-capture` (the wait
    for streamed sounds, the mix it reads, which `Audio.Mix` times inside it, and the file's write, §9.8);
  - `Renderer.RenderScene`, with `Scene.UpdateTransforms` (the world-transform cache's refresh before anything is
    submitted, once per view, §7.1), `Renderer.Submit` (the scene's draws, lights and UI) and `Renderer.EndScene`,
    which holds `Renderer.Cull`, `Renderer.Prepare` (resources, uploads, shadow views, frame bindings, and within it
    `Renderer.Batch`, the batches of every pass and the instance index buffer's content, §6.2, which replaces slice 1's
    `Renderer.Sort`) and `Renderer.Record` (recording and submitting the frame). Work moves between these zones with
    instancing (the shadow views' culling and the batching join `Renderer.Prepare`, recording loses them), so slice 1's
    figures and IN's are compared by `Renderer.Prepare` and `Renderer.Record` together, and the benchmark prints
    `Renderer.Cull` and `Renderer.Batch` on their own; `Renderer.UploadTexture`, `Renderer.UploadMesh` and
    `Renderer.ProcessEnvironment` on a cache miss only, and `Asset.Load` when an asset is loaded;
  - the editor's play mode, `Play.Update` around the frame it runs, and its graph phases: `GraphEditor.Window` around a
    graph window's frame, `GraphEditor.Sync` with `GraphEditor.Adopt`, `GraphEditor.Analysis`, `GraphEditor.Canvas`
    and `GraphEditor.Index`, `GraphEditor.DrawCanvas`, and `GraphEditor.PrepareCommit`; the debugger's `GraphDebug.Frame` (its part of a game frame, which takes the
    runtime's trace), `GraphDebug.EndFrame` (the session's end of an editor frame), `GraphDebug.TakeFlow` (a window's
    flow) and `GraphDebug.ReadValues` (the live wire values);
  - zones are placed per phase, never per item, except per script instance and graph invoke: a recorded zone costs
    about 70 ns, and a thread's buffer holds `ZonesPerThread` (4,096) zones a frame.
- **Who writes.** Any thread may record: the main thread, Jolt's workers (named by the job system's thread init
  function and ended by its exit function), miniaudio's device thread, the audio streaming thread and background
  workers. A zone is written once, when it ends, as one complete record: name, start, end, the thread's depth when it
  began, and an optional count. The record goes into the calling thread's buffer, a single-producer, single-consumer
  ring that the thread fills and the main thread empties. Recording a zone takes atomic operations only: it never
  allocates, never locks and never logs. On Apple platforms a thread's first access to thread-local storage makes dyld
  allocate its block, so a thread's first zone or name there allocates once; Windows and Linux executables use static
  thread-local storage and never allocate. The audio device's callback must never allocate, so it reaches no
  thread-local storage: it records with a `ProfilerThreadState` of the audio engine's (static storage, named
  `Audio Mixer` before the device starts and ended once it stops) through `ST_PROFILE_SCOPE_ON`, and no zone inside the
  engine's mix uses the thread's own state.
- **What an idle zone costs.** `BeginZone` does a relaxed load of the recording flag and branches. When nothing records,
  it returns an empty token without reading the clock, and the zone's end branches on that token. Both are defined
  inline in the header, so an idle zone is one predictable branch at each end. The run-time forms evaluate and store
  their name only while recording (`Profiler::BeginDynamicZone`). They take the recording's number before they store
  the name, so a name that a later `Shutdown` frees can never reach a recording. The targets on the reference machine
  are at most 3 % of the frame while recording and at most 0.5 % while idle. They are measured with
  `StradaRuntime --profile`, as plan targets are.
- **Threads that start and end.** The buffer headers are a fixed table of `Profiler::MaxThreads` (64) slots with
  static storage. `Start` allocates rings for the machine's hardware threads plus four spares. Each `EndFrame` tops the
  spares up on the main thread, so no thread ever allocates its own ring. Threads that start recording between two
  frames' ends therefore find at least four free buffers (more after `Start`); a thread beyond the free ones records
  nothing until a later `EndFrame` gives it a buffer, and the zones it ends meanwhile are dropped and counted in
  `DroppedZones`. A slot's state word holds the recording's number and the slot's state (free, being claimed, owned
  or ending). On its first zone of a recording, a thread takes a free slot of that recording with a compare-exchange.
  It remembers the slot in a constant-initialized, trivially destructible `thread_local` holding the slot index, the
  recording's number, its depth and its name. A `thread_local` with a destructor is avoided because registering its
  destructor may allocate on the audio thread. Thread names are
  copied into a 63-byte `thread_local` array, without allocating, and into the slot when the thread takes one, so
  threads name themselves before their first zone. `Profiler::EndThread`, which the end of `ST_PROFILE_THREAD`'s scope
  calls, marks the slot ending and makes the thread forget its slot and its name. The thread's next zone takes a new
  slot. The old slot returns to the free ones once the main thread has emptied it at the next collection. So at most
  64 threads can record between two collections, each task of a pooled thread counting again; the zones of any others
  are dropped and counted. A thread that ends without calling `EndThread` keeps its slot until the recording stops.
  miniaudio has no reliable hook at its device thread's start (its "started" notification comes from the device thread
  for synchronous backends but from the caller of `ma_device_start` for asynchronous ones, `miniaudio.h:42827`,
  `:44312`), which is why the audio engine names the device's `ProfilerThreadState` itself and ends it after
  `ma_engine_uninit`.
- **A full buffer.** A zone that finds its thread's ring full is dropped and counted, and so is a zone whose thread
  finds no free slot or whose run-time name cannot be stored. Its frame reports `DroppedZones`. Records are written when
  zones end, so a dropped zone can leave its children without a parent, and so can a parent that ends after the
  frame that collected them (a thread whose work runs independently of frames).
- **Frames and collection.** A frame is one iteration of the main loop, from `Profiler::BeginFrame` at its top to
  `Profiler::EndFrame` after the frame limiter. Tests and tools without an `Application` call both themselves. On the
  main thread, `EndFrame` empties every ring into the frame, grouped by thread.
  - A ring holds its thread's zones in the order they ended, each zone after the zones nested in it. The collection
    turns that into pre-order: each zone before the zones nested in it, and siblings in the order they began.
  - It does this from the depths alone, because a zone adopts the deeper zones that ended just before it. It never
    sorts by time, because zones that begin on the same tick of the clock (100 ns on Windows) would then come out in the
    wrong order. A ring drops zones only once it is full, so the dropped ones are the last to end, and no zone adopts
    one that is not nested in it.
  - Each zone's `Depth` in the frame is its depth in the frame's tree: the zones open when it began that the frame
    holds. A zone whose parent was dropped, or ends in a later frame, is at its thread's root, and one whose name could
    not be stored is left out with its children raised to its place. Every consumer (the panel's trees, the medians,
    the capture writer's categories) then reads one exact rule: a zone's parent is the nearest zone before it on its
    thread with a smaller depth, and a zone with none is at the thread's root.
  - The frame then joins the history. The history holds `HistoryFrames` frames (600 by default, at most 36,000) and
    `HistoryZones` zones (2,000,000 by default, at most 3,000,000), and drops the oldest frames first.
  - A frame that alone collects more zones than that keeps the first ones and counts the rest as dropped.
  - A zone belongs to the frame whose collection finds it: the frame during which it ended. Zones of the audio and
    streaming threads run independently of frames and may begin before their frame does.
  - Frames do not nest: a `BeginFrame` while a frame is in progress ends that frame first.
  - A recording's first frame is the first one that `BeginFrame` begins after `Start`. The frame in progress when
    `Start` is called is not recorded; the editor's `profiler.start` always runs inside one. Zones that ended before the
    first frame began are discarded when it begins. Nothing in a recording therefore starts before the recording does,
    and no recorded time is negative.
- **Tables.** A recording's name and thread tables grow as it meets new names and threads, and entries stay while it
  records even when their frames leave the history. Thread 0 is always the main thread, whether or not it recorded a
  zone, because frames belong to it.
  - A slot's owner is bound to a thread entry when the main thread first collects its zones: a free entry of the same
    name if there is one, otherwise a new one.
  - The entry is freed with the slot, at the end of that collection. A later thread of the same name then reuses it, so
    a pool's tasks share as many entries as ran at once, and two threads never share an entry in one frame.
  - Unnamed threads each get an entry of their own, named `Thread <n>`.
  - A recording makes at most `MaxRecordedThreads` (1024) entries; beyond them, zones are dropped. GPU views count up to
    `MaxGpuViews` (16).
  - A capture's writer writes only the entries its frames use, renumbered, so it never writes more names, threads or
    views than its reader accepts.
  - A capture's size in bytes depends on its names. The writer refuses a capture larger than `Profiler::MaxCaptureBytes`
    (512 MiB, what the reader accepts), with an error that names the limit, rather than write one its reader would
    refuse. At the default `HistoryZones`, a capture with ordinary names takes about 270 MB.
- **GPU passes.** NVRHI's timer queries measure durations only, their results arrive frames later, and the editor
  draws with several `SceneRenderer`s per frame. Every pass is wrapped in an NVRHI marker in every build, so a
  RenderDoc capture reads by pass name. While `Profiler::IsRecording()`, each renderer also extends its ring of frame
  timers (one entry per frame in flight) with a timer per pass, noting the frame's `Profiler::GetFrameIndex()`. When
  the queries resolve, it calls `Profiler::RecordGpuPasses(frameIndex, view, passes)`, which attaches the passes to
  that frame in the history. Passes of a frame still in progress wait for its end, and passes of a frame the history
  no longer holds are dropped. Adapters without timer queries (`AdapterInfo::SupportsTimerQueries`) report CPU zones
  only.
  - **The passes** (`SceneRenderer::PassScope`, which pairs the marker with the timer): `Upload`, `Shadows`, `Opaque`,
    `AmbientOcclusion`, `Sky`, `Transparent` (blended surfaces and world sprites), `EntityIds`, `Bloom`, `Tonemap`,
    `Overlays`, `FXAA`, `SelectionOutline` and `ScreenSpace`, each only when the frame draws it. The view is the name
    the renderer was made with: `Viewport`, `Game View` and `Thumbnails` in the editor, `Game` in the player, `Scene` by
    default.
  - **The timer budget.** NVRHI's Vulkan device holds 256 timer queries by default (`nvrhi/vulkan.h:74`) and reports
    running out as an error, which GPU tests fail on. `GraphicsDevice` raises `maxTimerQueries` to 8,192.
  - A renderer creates its per-pass timers the first time it renders while recording, all of a frame timer's or none,
    and keeps them until it is destroyed; a device that cannot make them reported it once, and the renderer stops
    timing passes. It times at most `SceneRenderer::MaxTimedPasses` (64) passes per frame, so with 4 frames in flight
    and its 4 frame timers it uses 260 queries. The pool therefore holds 31 renderers; the editor has four. Passes past
    the 64th of a frame are not timed. A finished frame's pass timers are read when its frame timer is, whether or not
    the recording still runs, so that they are free again.
  - A view that renders twice in one frame (thumbnails) reports both sets in order. The frame keeps both, and the
    medians add them per pass name.
- **Counters.** A frame also carries counts that are not times: each `SceneRenderer`'s statistics (§6.2), so that a
  player run shows its draws, instances and batches. `Profiler::RecordCounters(view, counters)` (main thread) attaches
  one record, the view's name and up to `Profiler::MaxCounters` (32) named counts (`ProfilerCounter`: a name with static
  storage duration of at most `MaxCounterNameBytes` (32) ASCII letters, digits, `_` and `.`, and an unsigned 64-bit
  value), to the frame in progress (`GetFrameIndex`). The counts are known when the renderer ends its frame, so unlike
  GPU passes they never wait for the GPU. A view that renders twice in a frame records twice, in order. A frame keeps
  at most `Profiler::MaxCounterRecords` (16) records; later ones, records made while no recorded frame is in progress,
  records of a view past the `MaxGpuViews` (16) views a recording names (counters and GPU passes share the views), and
  counts whose name breaks the rule are dropped. The bounds exist for the capture: a frame's counters travel in its
  frame event (§7.3), which the reader refuses past 64 KiB, and at the bounds they take at most about 30 KiB (16
  records of 32 counts of at most 56 bytes each: a quoted name, a colon, 20 digits and a comma), so the writer never
  writes what the reader refuses.
  While the profiler is idle the renderer builds and hands over nothing: it checks `Profiler::IsRecording()` (one
  relaxed load and a branch) first. `ComputeProfilerMedians` gives each `<view>/<counter>` its median over the frames
  that hold a record of the view, a view that rendered twice adding both (`ProfilerMedians::Counters`).
  `StradaRuntime --profile` logs them as `Profile counter <view>/<counter>: <median> per frame` (the exit's draws are
  `Profile counter Game/DrawCalls`), and `profiler.status`, `profiler.capture` and `profiler.open` return them as
  `counters` (`[{ name, median }]`) beside `gpuPasses` in their medians. Each renderer records `DrawCalls`, `Batches`,
  `Instances`, `Triangles`, `ShadowDrawCalls`, `ShadowMapViews`, `ShadowsDropped`, `Lights`, `CulledLights`, `Culled`
  and `Quads`. The Profiler panel shows the shown frame's records beside its GPU passes.
- **Start, Stop and Shutdown while other threads record.** Each recording has a number. A zone token remembers whether
  its zone began and in which recording, and its end writes nothing when the zone did not begin or the recording has
  changed. A depth counts the zones of the thread's current recording only, so a token of an ended recording never
  shifts a later recording's depths. Before it touches a slot (claiming it or writing a zone), a writer counts itself
  in the slot's writers (a sequentially consistent increment) and then checks the recording number. `Stop` and
  `Shutdown` clear the recording flag, change the number and wait until every slot has no writer (each counted for a
  few instructions). Only then do they empty and free the rings. The flag, the number and the slot headers are static
  and never freed, so a writer that races `Shutdown` touches only memory that lives on. Run-time names are copied into
  a store that lives from `Init` to `Shutdown`, guarded by a static mutex (they are never recorded on the audio
  callback), and the history copies every name it holds. A capture therefore never points into a script class that hot
  reload unloaded. Every function may be called before `Init` and after `Shutdown`: zones
  record nothing, `Start` refuses, and the main-thread functions do nothing, which keeps Init and Shutdown repeatable.
- **Dist builds.** In Dist builds the macros compile to nothing. Their arguments are referenced in a lambda that is
  never called, so they are never evaluated, yet a local or a function used only as a zone name raises no unused
  warning (`sizeof` would leave Clang to report such a function as never needed). The `Profiler` class is still compiled, but
  `Start` refuses, so no ring is allocated and no GPU timer is issued. A translation unit that defines
  `ST_DISABLE_PROFILING` before its first include gets the Dist form in any configuration; StradaTests compiles one.
- **Captures.** A capture is written from the history with `JsonTextWriter`, one element at a time, and read back with
  `ScanJsonTopLevel` and `ParseJson` per event. Neither side builds the whole document (§7.3).
- **The editor.** `EditorProfiler` (`StradaEditor/Source/Editor/EditorProfiler.h`, no ImGui) is the only place the
  editor starts and stops the recorder. The profiler records while the Profiler panel is open (the layer tells it once
  per frame, before the panels draw; the start screen counts as closed) or while a recording `profiler.start` began
  runs, which outlives the panel until `profiler.stop`; a closed panel with no such recording leaves the idle cost
  alone. `profiler.start` restarts a recording in progress with its settings; `profiler.stop` (and the panel's Pause)
  stops whichever runs and holds the open panel until its Record button or until it is closed and opened again. It also
  holds a capture opened in place of the live history (`profiler.open`, the panel's Open Capture), which the panel
  shows. The panel (`ProfilerPanel`) draws the history or the capture with ImGui alone: a strip of frame bars, the shown
  frame's zones as a tree per thread built by the depth rule above (`BuildProfilerFrameTree`: total and self time per
  zone, busy time per thread), its GPU passes, and `ComputeProfilerMedians` over the frames, worked out at most once a
  second (linear in the zones, frames and names). The shown frame's trees are built once while it shows and drawn as
  flat lines through a list clipper, so a frame of millions of zones costs the rows in view. Frames are found by index,
  never by position, since the history drops its oldest. The commands
  (`Editor/Automation/ProfilerCommands.cpp`) are no undo steps; `profiler.capture` writes only a `.json` path inside the
  project or an absolute one the caller names, and replaces an existing file only when asked and only when it is a
  capture. A profiler frame is an editor frame, so `play.advance`'s frames are one.
- **ThreadSanitizer.** `.github/workflows/tsan.yml` runs on a schedule (which GitHub fires only on the default branch),
  on manual dispatch and on a push to a `tsan/**` branch, on Linux with Clang in Debug (the `linux-clang-tsan` preset,
  which sets `STRADA_ENABLE_THREAD_SANITIZER`). It runs the suites' threading cases, one step per area: the profiler,
  the main-thread queue, audio (manual output beside the streaming thread, the null device mixing while the main thread
  changes the mix, the audio queue between two threads), the editor's worker-thread loads (thumbnails, the example copy; the
  engine's own asset loads run on the main thread), Jolt's workers, the automation server's network thread, and, in a
  step of its own, a profiled scene that hosts the .NET runtime. Every target the build compiles is instrumented,
  third-party ones included: synchronization in code the sanitizer cannot see makes it report races that are none, and
  Jolt compiles other inline code under it (`JPH_TSAN_ENABLED`). Prebuilt libraries are suppressed
  (`.github/tsan-suppressions.txt`), and one piece of Jolt's Debug bookkeeping (the owner its mutexes remember for an
  assert, which `try_lock` reads before it has the lock), never Strada's code. It never gates a merge.

## 5. Core conventions

- **Coordinate system**: right-handed, +Y up, −Z forward, +X right. Units: meters, kilograms, seconds.
  Rotations are stored as quaternions; the editor and the automation API also accept Euler angles in degrees.
- **Clip space and depth**: NVRHI flips the Vulkan viewport (negative height), so clip space follows D3D conventions:
  NDC +Y is up, depth is 0..1 and texture coordinate (0, 0) is the top-left corner. Projection matrices are built with
  glm (`GLM_FORCE_DEPTH_ZERO_TO_ONE`) and are **not** Y-flipped. Perspective cameras use reversed-Z (near = 1, far = 0)
  with an infinite far plane. Front faces are counter-clockwise in world space. Gizmo/ImGuizmo code receives a
  conventional (non-reversed) projection.
- **Color**: lighting in linear space; textures tagged sRGB (albedo, emissive) vs linear (normal, metallic-roughness,
  occlusion) at import; HDR scene color is `RGBA16_FLOAT`. Swapchains and the final image use 8-bit **UNORM** formats:
  the tonemap pass sRGB-encodes in the shader, and ImGui (whose colors are authored in sRGB) draws on top unchanged.
- **IDs**: `UUID` is a random 64-bit value; `0` is invalid. In JSON, UUIDs are written as **decimal strings**
  (lossless for every JSON consumer).
- **Memory**: `Ref<T>` = `std::shared_ptr<T>`, `Scope<T>` = `std::unique_ptr<T>`, created with `CreateRef`/
  `CreateScope`. GPU objects use NVRHI handles (`nvrhi::TextureHandle`, ...).

## 6. Rendering

### 6.1 RHI

`GraphicsDevice` creates the Vulkan instance (validation layers + debug utils when available and enabled), picks
a physical device (discrete preferred, overridable with `--gpu <index>`), creates the logical device and queues, and
wraps it in an NVRHI device (plus `nvrhi::validation` in Debug). Headless mode creates no surface/swapchain.
`Swapchain` wraps a `VkSurfaceKHR` + `VkSwapchainKHR` for one GLFW window: acquire, framebuffers, present, resize
(acquire semaphores rotate, present semaphores are per image). The main window and every ImGui platform window each own
a `Swapchain`. `GraphicsDevice::EndFrame` limits the CPU to 2 frames ahead of the GPU with event queries.
`ReadbackTexture` copies textures to CPU `Image`s (screenshots for the editor and the automation API).

The ImGui renderer (`ImGuiRenderer`) implements ImGui 1.92's texture protocol (create/update/destroy requests with
partial atlas uploads) and the multi-viewport renderer callbacks.

`ImGuiLayer` installs the keyboard and mouse callbacks of the windows ImGui draws into itself (the main window and the
platform windows of panels dragged out of it; the GLFW backend is initialized without its own). Without an input filter
the application gets the main window's input as it would without ImGui, ImGui gets it as well, and the events ImGui
wants to capture are marked handled (layers below do not see them, and `Input` only their releases and cursor
movement). An input filter (`ImGuiLayer::SetInputFilter`; the editor's `GameInputRouter`) sees every keyboard and mouse
event of those windows first, with the cursor in ImGui's coordinates, the ImGui viewport of the window it comes from,
the key the keyboard layout names and the modifiers held, and returns whether ImGui receives it; the application then
gets no input events of its own, and the filter gives `Input` what the game should have. Where ImGui wants no cursor
(`ImGuiMouseCursor_None`), the layer hides the OS cursor in every one of those windows: the GLFW backend alone hides it
in the main window only, and not in the frame after cursor changes were off (`ImGuiConfigFlags_NoMouseCursorChange`).
The platform windows of panels forward the files dropped onto them to the application's window
(`Window::ForwardFileDrops`), which sends them as `WindowFileDropEvent`s like the files dropped onto it.

A window may leave its title bar to the application (`WindowSpecification::CustomTitleBar`; the editor's does). On
Windows such a window keeps the style of a window with a title bar, so the system still gives it resize borders, a
shadow, snapping, the window menu and the animations of minimizing and maximizing; `WindowFrame` (internal to `Window`)
replaces the window procedure GLFW installed and answers the messages that say where the title bar is. The client area
covers the title bar (`WM_NCCALCSIZE`: the frame's borders stay at the sides and the bottom; a maximized window's client
area stays on its monitor, and leaves an edge free for a taskbar that hides itself). Positions are answered
(`WM_NCHITTEST`) with the frame's own borders, the top border, which lies in the client area, and then what the
application's hit test (`Window::SetTitleBarHitTest`, a `TitleBarArea` for a position of the client area) says: the
caption, which the system drags the window by, or one of the three buttons. The buttons are the system's areas (the
maximize button's is the one Windows 11 shows its snap layouts at), so the window gets no client input over them: it
tracks the one under the cursor and the one held down (`Window::GetHoveredTitleBarButton`,
`GetPressedTitleBarButton`) for the application to draw, and a press released on its button posts the window menu's
command for it (a close request reaches the application as `WindowCloseEvent`). The hit test is called while events are
processed, between frames and during the system's move and resize loops, so the application answers from what its last
frame drew, in client coordinates. GLFW sizes and places windows by a frame with a title bar: `Window::SetSize` and
leaving fullscreen place such a window through `WindowFrame`. The frame leaves every message alone while the window has
no title bar style (borderless fullscreen). macOS and Linux have no such frame yet: there the window keeps the system's
title bar (`Window::HasCustomTitleBar` is false). `WindowSpecification::DarkFrame` asks Windows for the dark colors of
the frame it draws.

Shaders (`Strada/Shaders/*.hlsl`) are compiled by DXC to SPIR-V at build time with flags matching NVRHI's default
Vulkan binding offsets, embedded into the library, and fetched by name through `ShaderLibrary`.

### 6.2 Scene renderer

`SceneRenderer` renders a submitted frame (camera + draw lists + lights + environment + settings) into its own
offscreen targets. The editor shows the final images in its scene and game views; the runtime blits it to the swapchain;
headless mode reads it back for screenshots. It never depends on the ECS: `Scene` gathers and submits
(`Scene/SceneRendering.h`: `RenderScene` with an explicit camera, `RenderSceneFromPrimaryCamera`).

`Renderer` (static facade, initialized with the GraphicsDevice) owns samplers, the material binding layout and GPU
caches keyed by asset object identity (meshes, textures per color space with CPU-generated linear-space mips, material
constants + binding sets refreshed by `MaterialAsset::GetVersion`); scene renderers resolve every resource before
recording because uploads submit their own command lists. Bindings: set 0 = frame constants (volatile CB), light
structured buffer, the frame's instances and instance indices (below), material sampler, per-draw push constants (the
draw's instance offset); set 1 = material. Structures shared with HLSL live in
`Strada/Shaders/Include/RendererInterop.h`; structured buffers use the same DirectX layout as constant buffers
(`-fvk-use-dx-layout`, `cmake/StradaShaders.cmake:17`), so a `float3` may follow a scalar inside a 16-byte row.

**Instancing** (slice 2's track IN). Every mesh a frame draws is an instance whose data lies in one structured buffer,
and the instances of equal submeshes are drawn together by one `drawIndexed`:

- **`InstanceData`** (`RendererInterop.h`, 192 bytes; its size and offsets are `static_assert`ed in that header for
  C++): the model matrix (offset 0); the normal basis (64), three `float4` columns of the inverse transpose of the
  model's upper 3x3 times the absolute value of its determinant (the cofactor matrix times the determinant's sign), so
  that shaders, which normalize the normals they transform, need no division and a singular scale gives a finite
  basis; the entity ID with `EntityIdSelectedBit` (112, then three words of padding); the previous model matrix (128),
  the entity's world matrix when the scene's current frame began, which is the previous frame's when that frame was
  drawn (`CachedWorldTransform::Previous`, §7.1), for velocities. `SubmitMesh` fills one per call (one per entity's
  mesh), computing the normal basis once for every pass that draws it; `RenderScene` submits each mesh with its cached
  world and previous matrices (`MeshSubmission`).
- **Velocities and cuts.** A previous matrix is the frame before's only when the renderer drew the frame before: frames
  no view drew refresh nothing (§7.1), a scene can be replaced by another (a scene load, play mode's copy), and one
  renderer may draw several scenes (thumbnails). So a renderer that keeps history (temporal anti-aliasing, track TA1:
  designed here, not built, since it was cut from First Ride's second slice; Roadmap.md R5) compares, at each
  `BeginScene`, the scene's `Scene::GetSerial` and `Scene::GetTransformFrame` with those of its last draw:
  `SceneRendererCamera` gains `SceneSerial` and `SceneFrame` (0 for unknown, as a camera made without a scene
  has), which `RenderScene` fills. A draw that is not of the same serial at the frame after the last draw is a cut:
  its velocities read `PreviousModel` as `Model` and its history starts again. Two renderers that draw one frame of
  a scene read the same previous matrices. TA1 adds the two fields, their filling and the rule with the history that
  reads them; IN gives the scene's side.
- **The buffers.** Each `SceneRenderer` owns an instance buffer (`StructuredBuffer<InstanceData>`) and an instance
  index buffer (`StructuredBuffer<uint>`), both written once per frame by `writeBuffer` in the `Upload` pass (NVRHI
  copies a write of more than 64 KiB through its upload manager and orders it after the earlier frames that read the
  buffer with barriers, `nvrhi/src/vulkan/vulkan-buffer.cpp:444-504`). Each grows to the next power of two of what a
  frame needs (at least 1,024 entries), never shrinks, is made again together with the binding sets that name it when
  it grows, and lives as long as its renderer. The frame binding set (set 0) holds them at `t7` and `t8`, the shadow and
  entity-ID sets at `t0` and `t1`. The index buffer holds, one after another, the opaque pass's runs, each shadow
  view's and one entry per blended item. The entity-ID pass writes none of its own: its batches are the opaque pass's
  in another order and the blended items', so it draws their runs again (the order of a run's instances changes no ID,
  the depth test keeps the nearest surface).
- **What a mesh draw carries.** Its vertex shader reads its instance as
  `g_Instances[g_InstanceIndices[InstanceOffset + SV_InstanceID]]`. The push constants hold the batch's run offset
  (`InstanceOffset`; 16 bytes in the opaque, transparent and entity-ID passes) and, in the shadow pass, the shadow
  view's view-projection before it (80 bytes): what the pass needs per draw, never a matrix per object. Every mesh draw
  passes `startInstanceLocation = 0`, so `SV_InstanceID` counts from 0 whatever DXC makes of a base instance.
- **Draw items** (`SubmeshItem`): one per submitted submesh, without `Ref` copies or matrices: its world bounds and
  view depth (culling and sorting), its instance's index, its mesh and material as indices into the frame's mesh and
  material tables (`FrameMesh`, `FrameMaterial`), its submesh, whether it casts shadows and whether it is in the
  camera's view. The tables hold one `Ref` per distinct mesh and material the frame submitted, so a mesh or material
  its owner drops during the frame lives until `EndScene` returns, and are resolved once per distinct asset in
  `Renderer.Prepare`: GPU buffers (`Renderer::GetMesh`), material binding set, and the pipelines of each pass (from the
  alpha mode and double-sidedness; null where the material is not drawn: blended materials cast no shadows).
  A submission comes in two steps: `SceneRenderer::PrepareMeshDraw` resolves a mesh and the materials of its slots
  into a *mesh draw* (`MeshDraw`; the frame mesh, and each drawn submesh's frame material and group), and
  `SubmitMesh(MeshDraw, MeshSubmission)` submits one instance of it, looking nothing up (the one-step `SubmitMesh`
  overloads do both). `RenderScene` prepares one mesh draw per distinct pair of mesh handle and material handles that
  its `MeshComponent`s name in a submission (`FrameMeshDraws` in `Scene/SceneRendering.cpp`, one lookup of a hash of
  the handles per instance, no asset lookup and no `Ref` copied per instance), and walks the meshes with
  `Scene::ForEachCachedWorldTransform`, which finds the cache's storage once instead of once per entity. An item's
  world bounds come from Arvo's method as `AABB::Transform` computes them (the matrix's absolute linear part made once
  per instance), and its view depth from the same world center (the view matrix's third row), not from a product of the
  view and model matrices. Sorting goes through 64-bit keys of the depth's ordered bits and the item's index, listed in
  item order and ordered by a stable radix sort of the depth bits (bytes equal in every key are skipped), which gives
  the front-to-back (or back-to-front) order with ties by submission in time linear in the items.
- **Culling** stays per item on the CPU: against the camera's frustum, and for the casters against each shadow view's
  volume (a cascade's projection, a local light's range sphere). `PrepareShadows` lists the frame's casters once, each
  with its world bounds, its item, group and instance (`ShadowCaster`), so that the views read one contiguous list. A
  directional light's cascades share the light's view (`CascadeProjection::LightView`) and differ by an orthographic
  projection along its axes: each caster's box is moved into the light's space once (center and extents, in the form
  `AABB::Transform` uses, `Shadows::LightSpaceTransform`), and each cascade then scales and offsets it per axis
  (`Shadows::LightSpaceCascadeTest`), instead of taking the box through the whole view-projection per cascade (and
  instead of transforming its eight corners, as slice 1 did). Its verdicts are `Shadows::CascadeTest`'s but for boxes
  within rounding of a cascade's border: with 10,000 casters and 4 cascades the test runs 40,000 times a frame inside
  `Renderer.Prepare`, whose budget it shares with submission.
- **Batching** (`Renderer.Batch`, inside `Renderer.Prepare`, after the pipelines and the shadow views are known). In the
  opaque (masked materials included), shadow and entity-ID passes, the items a pass draws (in the camera's view; for a
  shadow view, the casters that pass its test) with equal pipeline, GPU mesh, submesh and material binding set are one
  batch (`MeshBatch`): one `drawIndexed` of as many instances. Masked and double-sided materials have binding sets or
  pipelines of their own, so they never join another material's batch. Each item gets one dense group index when it
  is submitted, keyed on its mesh, submesh, material (through the frame tables' indices) and mirroring: a mesh draw
  looks up each of its submeshes' groups once when it is prepared, and the mirrored group the first time it submits a
  mirrored instance, so an instance looks nothing up. Each pass then buckets the items it draws by that index (a
  count, then a placement), so no pass hashes and none sorts every item on pointers. The orders:
  - opaque: batches front to back by their nearest instance's view depth (ties by the batch's first submission), and
    each batch's instances front to back (ties by submission), which keeps the early depth rejection that slice 1's
    front-to-back sort gave;
  - shadow: for each shadow view, its batches in the order of their first submission, their instances in submission
    order. A view's instances reach it as runs of indices, never as copies of their data. The order changes no texel:
    the depth test keeps the nearest surface whatever comes first. The frame's casters are listed once, then tested
    against each view; a point light's six faces share their range sphere and so the same casters, and draw the same
    runs (the index buffer holds them once);
  - entity IDs: the opaque batches in the order of their first submission, then the blended items;
  - blended items are never batched: each is a batch of its one instance, back to front by view depth (ties by
    submission), in the transparent and in the entity-ID pass, as before.
- **Statistics** (`SceneRendererStatistics`; the fields' comments are the definitions):
  - `DrawCalls`: every draw command the frame recorded, in every pass that ran: the mesh batches; every quad batch
    where it is recorded (world sprites and text in `Transparent`, the UI placed in the world in `Overlays`, the
    screen-space sprites, text and UI in `ScreenSpace`, and the IDs of all of them in `EntityIds`); and one each for
    the sky, the tonemap, FXAA, the grid, the debug lines' instanced draw and their composite, and the selection
    outline, when they are drawn. Compute dispatches (ambient occlusion, bloom), copies and clears are not draws, and
    neither are the draws a host records outside every scene renderer (the player's blit of the frame to its window,
    the editor's ImGui): the exit's "at most 400 draws" reads the renderer's `Profile counter Game/DrawCalls`. Slice
    1 counted only mesh and quad draws, and quads before recording.
  - `Batches`: the mesh draws of the shadow, opaque, transparent and entity-ID passes (part of `DrawCalls`);
    `ShadowDrawCalls` is the shadow pass's part of them.
  - `Instances`: the instances those batches drew, summed, so an instance counts once in every batch that draws it
    (once per pass and shadow view): the mesh draws a renderer without instancing would issue. `Instances / Batches` is
    the mean batch.
  - `Triangles`: the batches' triangles (a submesh's triangles times its batch's instances, the entity-ID pass
    included), plus two per quad.
  - While the profiler records, `EndScene` hands them to `Profiler::RecordCounters` under the renderer's view name
    (§4.4, "Counters"), after the frame is submitted; an idle profiler costs one relaxed load. `viewport.statistics`
    returns `instances` and `batches` beside `drawCalls`, and the editor's Renderer panel shows them (Mesh batches,
    Mesh instances).
  - A mirrored instance (a world matrix of negative determinant) turns the winding of its triangles. It is drawn with
    pipeline variants whose front faces are the clockwise ones (in the opaque, transparent, shadow and entity-ID passes,
    double-sided materials included, whose pixel shader turns back faces' normals), so back-face culling keeps its outer
    faces; its normals keep their direction through the normal basis, and its vertex shader turns the tangent frame's
    handedness (the bitangent's sign) with the determinant's sign. Mirroring is part of the batch key (a group's
    `GroupKey::Mirrored`): mirrored and unmirrored instances of one mesh, submesh and material are two batches. A turn
    by two negative scales keeps the winding and is no mirror. Slice 1 and IN's first steps drew mirrored instances
    inside out.
  - For tests, `SceneRenderer::GetFrameInstances` returns the frame's `InstanceData` as it was uploaded and
    `GetPassInstanceCounts` the instances each mesh pass and each shadow view drew, both valid until the next
    `BeginScene`: what a test reads to check the matrices a frame drew with and what reached each shadow view.
- **Costs.** The benchmark case of `Renderer/InstancingBenchmarkTests.cpp` (`STRADA_INSTANCING_BENCHMARK=1`, Release)
  draws 10,000 instances of 4 meshes and 3 materials among 12,001 entities, with a sun of 4 cascades, at 1920x1080,
  in three scenarios (nothing moved, 1,000 children moved, every root moved), entity IDs off and on. The targets are
  at most 400 draws, `Renderer.Submit` and `Renderer.Prepare` together at most 2.0 ms, and the cache's refresh at most
  0.5 ms. Draws are 47 with entity IDs off and 62 with them on. Measured on IN's final tree on the reference machine
  with no build or test running (two runs of the case, 24 medians of 300 frames), `Renderer.Submit` and
  `Renderer.Prepare` together took 1.19 to 1.93 ms (`Renderer.Batch` 0.37 to 0.58 ms of it); the first scenario of a
  run is the fastest, and the later ones settle at 1.7 to 1.9 ms. Between those two runs, in the same sitting, IN's
  first batched tree took 4.19 to 4.68 ms; it had taken 2.72 to 2.95 ms in an earlier sitting, so the in-frame figures
  and Submit + Prepare read about 1.5 times higher in that sitting, cause unknown (the isolated reading of the cache's
  refresh was at its usual value, §7.1), and the final tree's figure is an upper bound. At slice 2's gate, with the
  machine otherwise idle, two sittings about a quarter of a minute apart read the merged tree at 1.19 to 1.71 ms while
  the first tree, as the gauge, read 3.67 to 3.87 ms: out of the band of 3.3 ms in which a sitting judges. Two later
  sittings, ten minutes apart, with no build, test or Strada process at the start or the end of either, read the gauge at
  2.65 to 2.83 ms, in band, and the merged tree at 1.15 to 1.28 ms (`Renderer.Batch` 0.37 to 0.40 ms of it): the
  target is met in band. What changed against the first tree, in the order of what each saved: the sky light found without a
  walk through the hierarchy, mesh draws prepared once per frame, the light-space cascade test, the radix sort.

Visibility: `EndScene` tests the world bounds of every submitted submesh against the camera's `Frustum`
(`Math/Frustum.h`), built from its view-projection and limited to `SceneRendererCamera::MaxDistance` (the camera
component's far distance; unlimited for the editor camera). Submeshes outside it skip the main, transparent and
entity-ID passes and are not even prepared, unless they cast shadows that the frame draws: casters outside the view can
shadow what it shows, so the shadow pass culls them separately, against each cascade's projection or each local light's
range. `SceneRendererStatistics::Culled` counts them. Point and spot lights are tested when submitted, by their range
sphere (spot lights by the smaller sphere around their cone when its outer angle is below 60 degrees): lights that cannot
reach the view light nothing visible, so they take no place in the light buffer and get no shadow maps
(`CulledLights`).

GPU time: when the adapter's graphics queue writes timestamps NVRHI can use (`AdapterInfo::SupportsTimerQueries`, at
least 32 valid bits), each scene renderer brackets its frames with timer queries from a ring of four and reads the
finished ones at the next `EndScene` without waiting: `SceneRendererStatistics::GpuMilliseconds` holds the latest. A
frame whose timer is still in use goes unmeasured.

Lighting is forward PBR (opaque front to back, then blended back to front) with image-based lighting (split-sum
specular with Fdez-Aguera multiple-scattering compensation; a uniform ambient color through the same terms when the
sky light has no environment); the HDR target is `RGBA16_FLOAT` with a `D32` reversed-Z depth buffer. Environments
are processed on first use by compute shaders (`EnvironmentProcessor`): equirect to an RGBA16F cubemap (a quarter of
the equirect width, power of two, 16-1024) with box-filtered mips, a 32x32 irradiance cube and a 256x256
GGX-prefiltered cube with 6 mips (filtered importance sampling, perceptual roughness linear in mip); the 128x128 BRDF
table is computed at `Renderer::Init`. The first shadow-casting directional light gets PCSS soft shadows sized by its
`LightSize` (a fixed filter with `SoftShadows` off), with cascade cross-fades and a fade at the shadow distance; local
lights use a rotated 16-tap PCF. Shadow projection math lives in `Renderer/ShadowMath` (unit tested).

Frame passes, in order:

1. **Shadow pass** — directional light cascaded shadow maps (up to 4 cascades, `D32_FLOAT` texture array, stable
   cascades: bounding spheres with texel snapping, depth range extended to every caster), plus a local shadow-map
   array (`D32_FLOAT`, up to 24 slices) with one slice per shadow-casting spot light and six per point light (cube
   faces with guard bands, face chosen in the shader). Shadow maps use reversed Z; blended materials do not cast.
2. **Opaque forward PBR** — metallic-roughness GGX (height-correlated Smith visibility, Schlick Fresnel,
   multi-scatter energy compensation), directional/point/spot lights (physical units with smooth range window),
   IBL (cube irradiance + GGX-prefiltered specular + split-sum BRDF LUT), soft shadows (PCF with rotated
   Vogel disk and PCSS blocker search), alpha-mask support. Lights live in a structured buffer (directional
   lights and the local lights that reach the view, up to 256; further lights are ignored). Besides the HDR color it writes, as extra render targets, the
   octahedral world normal (`RG16_FLOAT`) and the exposed indirect light (`RGBA16_FLOAT`). There is no depth
   prepass: an equal-depth main pass would need position invariance across pipelines, which DXC's SPIR-V output
   does not guarantee.
3. **Ambient occlusion** — GTAO (horizon-based, cosine-weighted; 3 slices x 6 steps per side, per-pixel noise) at
   full resolution from depth and normals, a 5x5 depth-aware denoise, then a composite that subtracts the occluded
   part of the indirect light from the color (direct light is never occluded).
4. **Sky** — environment cubemap with rotation, intensity and blur (radiance mip).
5. **Transparent forward** — alpha-blended materials sorted back to front (color only).
   **Entity IDs** (editor only, `SetEntityIdsEnabled`) — every mesh again into an `R32_UINT` target with its own
   depth buffer (for the same invariance reason): opaque items first writing depth, then blended ones back to front
   testing only, alpha-masked cut-outs discarded. Values are picking IDs with the top bit marking selected meshes
   (0 = not pickable, but still occluding). `RequestPick` copies one texel to a staging texture; the result is read
   a frame or two later behind an event query, so picking never stalls the GPU.
6. **Bloom** — half-resolution chain of up to 6 levels: 13-tap downsample with Karis average on the first level,
   3x3 tent upsample accumulating into the first level; mixed into the color (energy conserving) by intensity.
7. **Tonemap** — chromatic aberration (red and blue sampled outwards and inwards, growing towards the edges), exposure
   (EV100), color grading before the curve (contrast around middle gray in log space, saturation, white balance by
   temperature and tint, a color filter), operators: ACES (Hill fit), AgX, Khronos PBR Neutral, Reinhard; then a
   vignette, animated film grain (strongest in the mid-tones) and dithering; sRGB encoding in the shader; output
   `RGBA8_UNORM`. Every effect is a scene renderer setting, neutral by default.
8. **FXAA** (optional) — luma edge search on the tonemapped image.
9. **Overlays** — world-space sprites and text are drawn into the HDR image after the transparent pass: unlit (their
    color is used as is), alpha blended, tested against the scene depth without writing it, back to front. On the
    tonemapped image (before FXAA when it is enabled): the editor ground grid (analytic ray/plane intersection on y = 0, anti-aliased minor and
    major lines and colored axes, faded with distance, hidden behind surfaces with a small depth tolerance so ground
    planes do not z-fight) and debug lines (editor shapes, script `Debug.DrawLine`): every line is an instanced
    screen-space quad (cut at the near plane) with round caps, an anti-aliased core of the overlays' width and an
    optional dark outline, drawn into a line layer where each pixel keeps only the nearest line's color (the distance
    to the line's center is its depth), so lines that meet or cross never blend twice; the layer is then blended over
    the image. Depth-tested lines compare their depth with the scene depth in the shader: hidden behind surfaces, or
    dimmed there (`OccludedLineOpacity`, which the scene view uses). After FXAA: the selection outline, an anti-aliased band
    around the visible silhouette of selected meshes, sprites and text read from the ID target. Screen-space sprites
    and text follow the overlays (their linear colors are sRGB-encoded in the shader), then the screen-space UI in
    submission order: rectangles (`UIRectSubmission`, with a texture or a color) whose edges and rounded corners are
    anti-aliased over a pixel from a signed distance computed in the shader (their quads reach a pixel beyond them),
    and text laid out in rectangles (`UITextSubmission`). All other overlay colors are sRGB-encoded with straight
    alpha. Sprites, text and the UI also write their picking IDs where they are opaque (screen-space ones on top).

Text uses signed-distance glyphs (`FontAtlas`): stb_truetype renders each character the first time it is used at
64 pixels per line with a 6-pixel distance range into a single-channel atlas (shelf packing; the atlas doubles up to
4096x4096 when full, keeping glyph positions), and the shader anti-aliases the outline over one screen pixel at any
size. `LayoutText` lays out UTF-8 (kerning, `\n`, tab stops of four spaces, left/center/right alignment, line
spacing, and wrapping at a width: lines break at the last spaces before a word that does not fit, words longer than
a line before the character that does not fit) in units of lines. The renderer keeps one atlas per font asset and
uploads it when glyphs were added. `QuadRenderer` batches the quads of a frame by texture into one vertex buffer.

Render targets are recreated on resize and reconfiguration only after the GPU has drained: NVRHI does not track
clears, which can be a target's only use in a frame.

`SceneRendererSettings` (shadows, SSAO, bloom, exposure, tonemapper, FXAA, sky) are stored per scene, editable in the
editor and through automation. Post-process volumes (`PostProcess` components, §7.2) change the exposure, tonemapper,
bloom, ambient occlusion, color grading and lens effects for each frame: `RenderScene` resolves them for the rendering
camera's position (`PostProcessVolumes::Resolve`) before `BeginScene`, so the editor's scene view (the editor camera),
its game view and the player all show them; the scene's own settings never change.

### 6.3 Assets

Assets are CPU-side data owned by the `AssetManager` (Asset module); the renderer creates and caches GPU resources from
them, so assets load (and are testable) without a GPU, and physics reads mesh geometry directly.

- **Handles**: `AssetHandle` (UUID). Values below 1024 are reserved for built-in assets and never generated.
- **Kinds**: *file assets* (files in the project's `Assets/` directory, persistent handles in the registry), *built-in
  assets* (`builtin://<Name>`), *memory assets* (created at runtime, e.g. cloned materials), and *sub-assets* (the
  materials and embedded textures of a mesh; deterministic handles derived from the mesh handle, alive while the mesh
  is loaded).
- **Loading**: on first use (`AssetManager::GetAsset<T>(handle)`); failures are logged once and remembered until the
  asset is reloaded or the directory refreshed. `Refresh` registers new files, flags missing ones (they keep their
  handles) and unloads modified ones. A write the editor makes to a loaded asset's file (a material or visual script
  edit, its undo and redo) is recorded (`AssetManager::RecordFileWrite`), so the next refresh keeps the asset object; a
  change of the file made after it is still a modification. Moves and deletes go through the manager so handles stay
  valid.
- **References in JSON**: handles as decimal strings; `"asset://<path>"` and `"builtin://<Name>"` are accepted on input
  and resolved to handles (`AssetManager::CreateDeserializationContext`). Engine-written files store handles, so
  moving files never breaks them; hand-written path references follow the path.

Asset types:

- `MeshSource` (`.gltf`, `.glb`, `.fbx`, `.obj`): one vertex layout for every mesh (position, normal, tangent (xyz +
  bitangent sign), UV0 with the origin at the top-left; 48 bytes), 32-bit indices, submeshes (index range, material
  slot, bounds) and one default material per slot. Import bakes node transforms (Y-up, meters, counter-clockwise front
  faces; mirrored nodes flip winding and tangent sign), generates flat normals and MikkTSpace tangents when missing
  (V is flipped for MikkTSpace so the bitangent points to the top of the image, matching glTF), converts FBX/OBJ UVs to
  the top-left origin, and validates every index. Embedded images become texture sub-assets; external images inside
  `Assets/` resolve to their registered texture assets. Skinning, morph targets and point/line primitives are skipped
  with warnings. Damaged files are refused before their data is read: glTF buffer views, accessors and sparse data
  must lie inside their buffers (checked overflow-safely before `cgltf_validate`), ufbx loads from memory with a
  memory limit of 256 MB plus 64 times the file size, and models with more than 2^24 vertices or triangles (every
  node instance counted) fail before any geometry is built. Files a model references (glTF buffers and images, FBX/OBJ
  textures, OBJ material libraries) are only read inside the asset directory: buffers outside it fail the import,
  textures and material libraries outside it are skipped with a warning. `MeshImporter::FindExternalFiles` lists the
  files a model reads, found like the import finds them but without building geometry, so the editor copies and moves
  them with the model; each once, spelled as stored (`FileSystem::FindInside`), as file systems that ignore letter case
  (Windows, macOS) take a model's several spellings of one file. Imported material parameters are brought into the
  material ranges (`FitMaterialToFieldRanges`), so copies of the materials make valid material files: emitted light
  above 1 (MTL's `Ke`, FBX emission colors) moves from `EmissiveColor` into `EmissiveIntensity`, which keeps its
  brightness; other values outside a range (FBX colors above 1, broken files) are clamped, and NaN or infinite ones take
  the defaults, with a warning.
- `MaterialAsset` (`.smat`): metallic-roughness parameters — `BaseColor`, `Metallic`, `Roughness`, `EmissiveColor` +
  `EmissiveIntensity`, `NormalStrength`, `OcclusionStrength`, textures (base color, normal, metallic-roughness with glTF
  packing G = roughness and B = metallic, occlusion, emissive), `AlphaMode` (Opaque/Mask/Blend) + `AlphaCutoff`,
  `DoubleSided`, `UVTiling`, `UVOffset`. A version counter tells renderers when parameters changed.
- `TextureAsset` (`.png`, `.jpg`, `.tga`, `.bmp`): keeps the encoded file and decodes on demand. The color space is
  chosen by the sampling material slot (base color and emissive sRGB, the others linear); the renderer generates mips
  at upload in linear space. stb_image is built with only these decoders and Radiance HDR's (so model textures in
  other formats are not decoded), refuses images over 16384 pixels on a side, `Image` and `HdrImage` refuse images
  over 2^27 pixels in total (`Image::MaxPixels`, read from the header before decoding), and `Image` refuses images
  without pixels.
- `EnvironmentAsset` (`.hdr`): equirectangular Radiance HDR (e.g. Poly Haven HDRIs, which `strada hdri` and the MCP tool
  `polyhaven_import_hdri` download and import) → cubemap + irradiance + prefiltered specular (compute shaders).
- `FontAsset` (`.ttf`, `.otf` with TrueType outlines; the first font of a collection), `AudioClipAsset` (`.wav`,
  `.flac`, `.mp3`, `.ogg`; format detected from the data), `PrefabAsset` (`.sprefab`). Scenes (`.sscene`) are
  registered for references but opened with `SceneSerializer`. stb_truetype, which renders fonts, trusts every offset
  in a font, so `SanitizeTrueTypeFont` (`Asset/TrueTypeSanitizer`) first checks everything it reads: damaged glyphs
  become empty, damaged kerning is ignored, other damage refuses the font, and so do CFF outlines, which stb_truetype
  cannot keep within their table. `FontAtlas` skips glyphs whose distance field would take too long.
- `VisualScriptAsset` (`.sgraph`): the document of a visual script (`VisualScriptDocument`): variables, functions (each
  with its parameters, locals and graph) and the event graph of nodes, links and comments. The engine checks only its
  structure (`VisualScriptSerializer`, §7.3); what node types, settings, inputs and values mean is left to the scripting
  runtime. A version counter tells users when the document was replaced. `PlaceUnplacedNodes`
  (`Asset/VisualScriptLayout`) gives the nodes without a position one, in columns that follow the links; it is never
  applied by loading or saving. Visual scripts have no thumbnails.
- Built-in assets: meshes `Cube`, `Sphere`, `Plane`, `Cylinder`, `Capsule`, `Cone`, `Quad` (unit sizes matching the
  default colliders), `DefaultMaterial`, textures `White`, `Black`, `FlatNormal`, the `DefaultFont` (Roboto Medium,
  compiled into the engine with `strada_embed_resources` from `cmake/StradaResources.cmake` and found with
  `EmbeddedResources::Get`), and the procedural `DefaultSky` environment (zenith/horizon/ground gradient). Later
  subsystems may register more with reserved handles.
- Data assets (§6.4): every other asset type made of one struct of data, each from one registration.

### 6.4 Data assets

A data asset type is a file of one reflected struct: curves, physics materials, input action maps, mixers, vehicle
definitions. Each comes from **one registration** (`Strada/Asset/DataAsset.h`), and everything a type needs comes from
it, so a new type costs its struct, its field table and one line, never a change to the asset manager, the content
browser, the inspector, the automation commands or the fuzz tests. Materials keep their own code (`MaterialAsset`,
`material.*`), which this generalizes and leaves unchanged.

**The registration.** `DataAssetRegistry::Register<TData>(DataAssetTypeSpecification<TData>)` names:

| Member | Rule | What uses it |
|---|---|---|
| `Name` | PascalCase ASCII letters and digits, 1 to 64 bytes; no built-in `AssetType` name or registered type's | `AssetTypeToString`, asset registries, the `References` hint of asset fields, the file header's `Type` and content key, the managed class, the commands' domain in kebab case and their asset parameter in lower camel case (`PhysicsMaterial`: `physics-material.get { "physicsMaterial": ... }`) |
| `Extension` | A dot and 2 to 16 lowercase ASCII letters or digits; no built-in or registered type's. Strada's formats start with `.s` | `GetAssetTypeForExtension`, scans and imports, `<domain>.create`'s path, the content browser |
| `FormatVersion` | At least 1 | The version every save writes |
| `Description` | One sentence | The content browser's New menu, the commands' descriptions, schemas |
| `Migrations` | `FormatVersion - 1` functions (`DocumentMigration`); `Migrations[i]` upgrades version `i + 1` to `i + 2` | Reading older files |
| `Validate` | Optional, pure | Every value an asset is about to take: loaded, patched or set |

The struct's field table (`StructTraits<TData>`, §7.2's field hints: ranges, `Doc`, `References`, `VisibleWhen`,
`EnabledBy`, `HiddenInEditors`, nested structs, arrays of structs and their `MaxCount`) is the rest: the format, the
validation of every number, the editor and the automation schema. Registration fails, registering nothing, when a rule
above is broken, when the name or the extension repeats a built-in or registered type's (names compared in any letter
case), when the command domain or the command parameter repeats a registered type's, when the command parameter is a
member of the generated commands (`fields`, `path`), when a value-initialized `TData` does not read back from its
fields or fails `Validate`, when `TData` or `DataAssetRegistry::MaxTypes` (128) types are registered already, or while
the asset manager has an asset directory open.

Every value of a data asset has a generated editor and schema: the field table holds no field of a type with a JSON
format of its own (`FieldKind::Custom`, which generic editors show as "Edited elsewhere" and schemas cannot describe),
in nested structs and array elements too, and no array of arrays; `TypedDataAssetType` checks it at compile time
(`Detail::IsDescribedFieldType`). Data that does not fit the table plainly takes these forms: an optional value is a
bool switch and the value (`EnabledBy`, which elements keep); a variant is a `Kind` enum and the fields of each kind
(`VisibleWhen`, up to 16 values); a map or a graph is an array of named structs with a `MaxCount`, whose `Validate`
checks unique names and acyclic parents. Anything else first extends the field table (a kind, its editor and its
schema) rather than adding a custom field.

**Who registers, and when.** The engine's types are one list in `DataAsset.cpp` (one line per type, sorted by name,
which merges as a union). The registry registers them when it is first used, by any of its functions or any `Asset.h`
lookup, on whatever host and whether or not the asset manager is initialized, and keeps them for the whole process: their
`AssetType` values are fixed by the list's order from `DataAssetRegistry::FirstAssetTypeValue`, and their names and
`DataAssetType` objects stay valid until the process ends. So every host and tool knows the same types by name: the
editor, the player, the headless tools, and tests that parse the visual script library or script metadata without an
asset manager. That matters: an asset registry keeps no entry of a type it does not know (`AssetRegistry::Deserialize`
skips it with a warning), a scan registers no file of an unknown extension, and the native parsers of the visual script
library and of script field metadata refuse an asset type name they do not know, so a host that missed a type would
drop the handles of its files the next time it saves the registry, and every reference to them would break. Hence the
placement rule: a data asset's struct lives in the Asset module or below it (Core, Math), and the subsystems that use it
include it from there, as the renderer uses `MaterialData` (§4). Its field table needs Serialization, so it lives in the Asset module
beside the registration: a struct of the Asset module carries it in its own header, as `MaterialData` does, and a
struct of Core or Math (a curve in `Math/`) gets it in its type's header `Asset/<Name>Asset.h`, which every user of the
asset includes. An enum the struct shares with a higher module moves down with it, as `MaterialAlphaMode` lives beside
`MaterialData`; data that is a higher module's own settings (the renderer's quality settings live in
`Renderer/SceneRendererSettings.h`) is a project setting or a struct of its own that the module maps, not a data asset
of that module's struct. Tests register types of their own with `DataAssetRegistry::Register` (with or without an asset
manager, but not while it has an asset directory open, since the directory's files were scanned with the types it
knew), from the next free value, and unregister them (`Unregister`, refused for the engine's types, while a directory
is open and while the asset manager holds an asset of the type, a memory asset included) when they end;
`Testing::TestDataTypeScope` does both for the worked example's type. A `DataAssetType` is shared by the registry and
the assets of its type, so an asset that outlives its type's registration keeps a valid type, whose value becomes
`AssetType::None`: a type registered later may take the value, and no asset of the old type passes as one of its. The
`Asset.h` lookups stop finding an unregistered type at once.

**What comes from the registration.**

- *The asset type.* The registry gives each type an `AssetType` value from `DataAssetRegistry::FirstAssetTypeValue`
  (128) up: the engine's types in their list's order, then the lowest value free; the built-in enumerators stay below. `AssetTypeToString`, `AssetTypeFromString`,
  `GetAssetTypeForExtension` and `GetSupportedAssetExtensions` consult the registry after the built-in types, so asset
  registries, pickers, drops, script fields and the visual script library's asset types work by name. The values are
  never written anywhere (files and the script boundary carry names), so the order may change between builds. A switch
  over `AssetType` handles data asset types first (`DataAssetRegistry::Find(type) != nullptr`) and the built-in
  enumerators after: the asset manager's loader, the content browser's tile style and glyph, the inspector, thumbnails
  (none: `AssetThumbnails::Supports` stays false), and the script bindings' loadable types (a data asset type becomes
  loadable by scripts with its managed class, below).
- *The file, its loader and its saver* (`DataAssetSerializer`, §7.3): a versioned struct document
  (`Serialization/StructDocument.h`), the header with the type's name and version, then the content object named after
  the type. The asset manager loads a file of a data asset type with it (fields this
  engine does not know warn, logged with the file) into a `TypedDataAsset<TData>`, whose base `DataAsset` gives generic
  code the fields as JSON (`SerializeFields`) and takes partial patches (`ApplyFields`: all or nothing, ranges and
  `Validate` checked). C++ users read `AssetManager::GetAsset<TypedDataAsset<TData>>(handle)->GetData()`.
- *A version for hot reload* (below), *the asset editor and the commands* (below), and *membership in the registry-wide
  fuzz test* (below).

**Format versions and migrations.** Saving writes the type's `FormatVersion`. Reading checks the header (a newer
version is refused), takes the content object, runs the migrations from the file's version up (each on the content
object in place), then reads the fields and validates. A field added with a default is a compatible change and needs
no new version: older files read it as its default, and an engine without it warns about the unknown key and drops it
the next time it writes the file. Renaming, removing or changing the meaning of a field takes a new version and a
migration, with a fixture file of the old version under `Tests/Data/DataAssets/<Name>/` that a test loads. A migration
checks what it reads and refuses what it cannot convert; the loader also refuses a file whose migration throws
(nlohmann throws on a value of the wrong kind) or leaves no object, so a missed check refuses the file instead of
crashing. A reflected struct that other types embed (a curve inside a vehicle definition) changes only compatibly;
otherwise its header exports a migration of one object of it (`MigrateCurveFrom1(Json&)`), and every type that embeds
it takes a new version whose migration calls that function on each embedded object, with a fixture file of its own.

**Documents that are not assets.** A save slot, the player's settings and a binding profile are files of one struct
too, but they are no data assets: they live in the game data folder, not in `Assets/`, have no handle, and are written
by the game while it runs, so they have no New item, inspector or commands. They reuse what data assets are built on,
free of the registry: `ReadStructDocument<T>(document, format, validate, context)` and `WriteStructDocument<T>(data,
format)` (`Serialization/StructDocument.h`, with `StructDocumentFormat { Type, Version, Migrations }`) read and write
the same header, run the same migrations and the same `Validate`, and their fuzz tests use the same `DocumentMutator`.
How they are written (atomically, per game) is their own track's choice.

**References to other assets.** A field of type `AssetHandle` (or an array of them, in the struct or in nested structs
and array elements) with the hint `References("<type name>")` refers to an asset of a built-in type (`"Texture"`,
`"AudioClip"`) or of a data asset type (`"Curve"`). It is stored as a handle string, so moving the referenced file
breaks nothing; input (files, automation) may also give `"asset://<path>"` or `"builtin://<Name>"`, resolved on reading.
Loading an asset never loads what it references: users resolve a reference when they use it, so cycles are harmless,
and a reference to a deleted asset is a handle that resolves to nothing, which users treat as none. The editor's
picker and drops offer only assets of the hinted type; exported games carry every file of `Assets/`.

**Hot reload.** Users of a data asset keep its handle, look the asset up when they use it, and compare
`DataAsset::GetVersion()` with the version of what they derived from it (GPU resources, compiled tables, cached
values). A version is a number no other data asset object had or will have within the process: drawn when the object is
created and again by every change of its data. An edit in the editor (the inspector, `<domain>.set`, undo and redo)
writes the file, gives the loaded object the new data and records the write (`AssetManager::RecordFileWrite`), so the
object stays and its version moves; a change of the file made outside the editor is found by the next refresh, which
unloads the asset, and the next lookup loads another object with another version. Comparing the version alone is
therefore enough (the renderer's material cache has to compare the object as well, since a material's version restarts
with each object). `DataAssetRegistry::GetEpoch()` moves whenever any data asset object is created, changes or is
unloaded: managed caches use it (below).

**Layering.** The registry, the asset classes and the serializer are in the Asset module, on top of Serialization's
field tables; the asset manager calls the registry, never the reverse, apart from the registry refusing registrations
while a directory is open. The structs live in the Asset module or below. The editor's parts are in `StradaEditorCore`;
scripts reach data assets through the Script module's bindings.

**The editor.**

- *The asset editor.* Selecting a data asset shows, below its name and reference, the body `DataAssetEditor` draws
  (`Editor/UI/DataAssetEditor.h`): by default the type's field table through the `FieldEditor`, with asset fields,
  nested structs and arrays of structs (§11), editable when the asset has a file of its own that is present and not
  read-only (`SetDataAssetCommand::CheckEditable`) and shown disabled otherwise, with the reason above it (data assets
  made in memory, read-only files). One widget interaction is one undo step, through
  `EditorOperations::SetDataAssetFields(asset, patch, description, mergeKey)` and `SetDataAssetCommand`
  (`Editor/Commands/SetDataAssetCommand.h`), with the inspector's `EditSession`, whose key the asset editor asks for only
  when an edit brings none: a drawer's own keys keep merging while the session's interaction has ended.
- *A drawer of a type's own.* `DataAssetEditor::SetDrawer(name, drawer)` replaces the default body of one type (the
  engine's drawers are one list in `DataAssetEditor.cpp`, where a type's track adds its line; `Curve`'s is the curve
  editor, §6.5). A drawer gets a
  `DataAssetDrawContext` (the asset, its type, its fields as JSON, whether it is editable, the field editor) and returns
  a `DataAssetEdit` when the user changed something: a partial patch, the step's name in the undo history (empty for
  `Edit <type name>`) and a merge key (0 for the inspector's interaction). A widget that commits at its own cadence, such
  as a curve editor whose arrow-key nudges merge by time while no ImGui item is active and whose every edit names
  itself (`Set Length on Timeline (moves 2 keys)`), gives its own key and description, which the step keeps. The asset
  editor applies the edit as the step `<domain>.set` makes, so a drawer never writes files or assets itself. It may draw
  the default body for the fields it does not draw itself (`DataAssetEditor::DrawFields`), the others being
  `HiddenInEditors` in the field table. A struct type that appears inside other data (a curve inside a vehicle
  definition) gets a struct drawer of the field editor instead, from one editor-wide list, used wherever that struct
  appears (§11).
- *The content browser.* Its New menu has an item per registered type, named by the type's display name (`New Test
  Data`, the description as its tooltip), which writes a file of the default fields named `New <display name>` (made
  unique) into the displayed folder, selects it and starts renaming it, as New Material does
  (`EditorOperations::CreateDataAsset`). Tiles of data assets show a table glyph with the type's display name as their
  label.
- *The commands.* Every type registered when the editor's commands are registered gets three, documented in
  Automation.md with each shipped type's rows:
  - `<domain>.create { path, fields? }` writes a file (path relative to `Assets/`, ending in the type's extension) with
    the default fields plus the given ones and registers it; not an undo step, as `material.create` and `graph.create`
    are not. It answers like `get`.
  - `<domain>.get { <parameter> }` answers `{ "<parameter>": <the asset as asset.get describes it>, "fields": { ... } }`;
    read-only.
  - `<domain>.set { <parameter>, fields }` applies a partial patch and writes the file: an undo step that does not mark
    the scene modified, refused for data assets without a file of their own and for read-only files.

  `fields` is read strictly: an unknown field or a value outside its range fails with InvalidParams naming it (the field
  table's message: `TestData.Size: must be at least 0`), and so does a value `Validate` refuses; nothing is written
  then. An asset of another type is InvalidParams too; `set` of a data asset without a file of its own, a missing file
  or a read-only one is InvalidOperation, and a file that does not load is FileError. The handlers find their type again
  by name at each call, so a type unregistered since answers Unavailable. `asset.list` takes the registered types' names
  as its `type`. The three work while the editor plays, as `material.*` do: an edit of a data asset is an edit of its
  file, which play mode neither snapshots nor restores, so the playing game sees it at once (through the version) and it
  stays, with its undo step, after the editor stops. The `fields` parameter's schema lists the type's fields with their
  types, ranges and descriptions, and leaves the values (and other names) to the field table, so that its messages reach
  the caller. Undo writes back every field as it was, not the file's bytes: undoing the first edit of a file of an older
  format version, or of one holding fields this engine does not know, leaves it in the current version with the same
  values, as materials do. The engine does not know the editor's command domains: a type whose commands would repeat a
  built-in command's name (a type named `Graph` gives `graph.create`) makes the editor's command registration fail,
  which the editor tests catch, and a type named after a built-in domain without a `create`, `get` or `set` (`Viewport`)
  would add its commands beside the built-in ones, so no type is named after a built-in domain.

**Scripts: the managed pattern.** A data asset type that scripts use gets a sealed managed class of the same name in
`Strada-ScriptCore`, deriving from `Asset` with an internal constructor, as `Material` does; no public member lands
without a use in the FeatureTest scripts (ScriptCoreTests' coverage), so each type's track adds its class with its first
user (CV's `Curve` is the first, PM's `PhysicsMaterial` the second, IA1's `InputActions` and MX1's `Mixer` the next two). Managed code identifies asset classes by name, never by a native number: `AssetTypes`
maps each class to its type name (`"Material"`, `"Curve"`) without calling native code, and the visual script library's
types, the script field codec, the library writer and the literal codec carry that name, as the JSON they write already
does. So ScriptCoreTests build, write and check the node library, and run graphs, without a native engine, data asset
classes included. A native number is fetched only where a call crosses into native code (`Assets.Load<T>`, the
loadable-type check, the type's bulk read): the class's number comes from its name (`Assets_FindType`), cached per class
after the first call, and a value of 128 or more coming from native code is named by `Assets_GetTypeName`
(`AssetTypes.GetTypeName(int)`, cached per value with the class's number) before `AssetTypes.Create` makes the
reference. The script bindings' loadable-type check (`IsLoadableType`) accepts a data asset type
when its class arrives, and so does the visual script editor's list of exposable types
(`FindVisualScriptExposableType`, which the node library's types are checked against), so that public variables and
literals of the class work in the editor. The data stays native; a class that reads it in managed code (a curve evaluated per sample by graphs,
which must allocate nothing and cross no boundary per call) keeps a managed copy with the version it read. Before
running scripts (the update, fixed update and event entries of C# scripts and graphs), the Script module compares
`DataAssetRegistry::GetEpoch()` with the epoch it last gave the managed side and, when it moved, tells the managed side
once (`InvalidateDataAssets`, an entry point of `Strada.Interop.Host` that moves `DataAssets.Epoch`); a class then asks again
on its next use, through one binding of its own that takes the version it holds and copies the data in bulk only when
the asset's version differs. The epoch moves on every load of any data asset, and that keeps such a load from costing
each cached curve a copy and an allocation. It also moves on a refresh after which a data asset whose load failed, or
whose file was missing, may load: nothing a user of it holds changes when it does, so the move is what tells the user
to look again. So a script sees an edit made in the inspector while playing on its next
frame, and an access costs a comparison of two managed numbers.

The `strada-add-data-asset` skill is the checklist of a new type: the struct and its field table, the line in the
engine's list, a drawer when the field table is not the editor the type needs, the FeatureTest seed file, migrations
with their fixtures, the command rows, the MCP tool list, and the managed class with its first user.

**The worked example**, the type the tests register (`Tests/StradaTests/Source/Asset/TestDataAsset.h`, shared with
StradaEditorTests): an enum, a number with a range, references to a texture and to another asset of its own type, and
an array of structs with a bound, in its second format version.

```cpp
enum class TestDataShape : uint8_t { Box = 0, Sphere };
template<> struct EnumTraits<TestDataShape>
{
	static constexpr std::array<std::pair<TestDataShape, std::string_view>, 2> Values = {{
		{TestDataShape::Box, "Box"}, {TestDataShape::Sphere, "Sphere"} }};
};

struct TestDataEntry
{
	std::string Name;
	float Weight = 1.0f;
	bool operator==(TestDataEntry const& other) const = default;
};
template<> struct StructTraits<TestDataEntry>
{
	static constexpr std::string_view Name = "TestDataEntry";
	static constexpr auto Fields = std::make_tuple(
		Field("Name", &TestDataEntry::Name).Doc("Unique within the asset."),
		Field("Weight", &TestDataEntry::Weight).Range(0.0, 10.0).Doc("How much the entry counts."));
};

struct TestData
{
	TestDataShape Shape = TestDataShape::Box;
	float Size = 1.0f;
	AssetHandle Texture;
	AssetHandle Next;
	std::vector<TestDataEntry> Entries;
	bool operator==(TestData const& other) const = default;
};
template<> struct StructTraits<TestData>
{
	static constexpr std::string_view Name = "TestData";
	static constexpr auto Fields = std::make_tuple(
		Field("Shape", &TestData::Shape).Doc("The shape the size measures."),
		Field("Size", &TestData::Size).AtLeast(0.0).Doc("Edge length of a box, diameter of a sphere."),
		Field("Texture", &TestData::Texture).References("Texture").Doc("A texture the asset refers to."),
		Field("Next", &TestData::Next).References("TestData").Doc("Another asset of the type."),
		Field("Entries", &TestData::Entries).MaxCount(16).Doc("Weighted entries, in order."));
};

// Version 1 called Size "Radius".
inline Result<void> MigrateTestDataFrom1(Json& content)
{
	if (auto const radius = content.find("Radius"); radius != content.end())
	{
		if (content.contains("Size"))
		{
			return Error{"version 1 has \"Radius\", not \"Size\""};
		}
		Json size = *radius;
		content.erase(radius);
		content["Size"] = std::move(size);
	}
	return {};
}
inline constexpr std::array<DocumentMigration, 1> TestDataMigrations = {&MigrateTestDataFrom1};

inline Result<void> ValidateTestData(TestData const& data);   // entry names are unique

Result<AssetType> const type = DataAssetRegistry::Register<TestData>({
	.Name = "TestData",
	.Extension = ".stestdata",
	.FormatVersion = 2,
	.Description = "A shape with a size, references and weighted entries (a type of the tests).",
	.Migrations = TestDataMigrations,
	.Validate = &ValidateTestData,
});
```

That registration gives the extension `.stestdata`, the asset type `TestData` (the value after the engine's types), the
commands `test-data.create`, `test-data.get` and `test-data.set` with the parameter `testData`, a New Test Data item,
the inspector's field table (the entries as an array of structs, at most 16), and a place in the fuzz test. A file:

```json
{
	"Strada": {
		"Version": 2,
		"Type": "TestData"
	},
	"TestData": {
		"Shape": "Sphere",
		"Size": 2.5,
		"Texture": "8406934185603345521",
		"Next": "0",
		"Entries": [
			{
				"Name": "First",
				"Weight": 1.0
			}
		]
	}
}
```

A version-1 file with `"Radius": 2.5` loads as the same data, and saves as the file above.

**Tests.** The test type goes through every generated surface: the registration's refusals, the asset type and
extension mapping, the native parsers that read type names (the visual script library, asset registries), file round
trips, the migration (a version-1 fixture), hot reload (an editor write keeps the object and moves the version; a
refresh after an outside write loads another object), the asset editor and a drawer headless (a drawer's own merge key
and description across frames without an active item), the commands with their undo, also while the editor plays, and
the fuzz tests. StradaTests' registry-wide fuzz test (`DataAssetFuzzTests.cpp`) damages seed documents of every
registered type: the files of each engine type's extension in the FeatureTest project (it requires at least one per
engine type, as StradaTests requires every component in its scenes) and, for `TestData`, a document of every field and
a version-1 document that goes through the migration; a damaged document is refused, or it loads, saves in the current
version and loads again strictly to the same fields. `AutomationFuzzTests` registers `TestData` before the editor's
commands, so the generated commands are fuzzed from their first day; since the engine's types are known everywhere, the
command reference test sees each engine type's commands without a fixture of its own.

### 6.5 Curves

A curve maps a number (a time, a speed, an input's deflection) to a value through keys: torque over revolutions per
minute, a response curve of an input action, a fade, a camera's move. There is **one curve semantics**, the visual script
timeline's (spec-slice3 §3.8.3), promoted to the engine: `Strada/Math/Curve.h`.

- **Keys.** A key has a `Time` (any finite number), a `Value`, the `Interpolation` of the segment to the next key and two
  optional slopes (value units per input unit), `In` and `Out`. Keys are in time order; several at one time make a jump
  (the segment before them ends at the first, the one after starts at the last). `Constant` holds the key's value to the
  next key, `Linear` interpolates, and `Cubic` is the Hermite segment through both keys with the first key's `Out` slope
  and the second key's `In` slope. An automatic slope is 0 at the first and the last key, and at an interior key the
  slope from the key before to the key after (Catmull-Rom on uneven times), 0 when those two lie at one time. Before the
  first key the curve has its value, from the last key on the last's. Everything is computed in double from the keys'
  floats and rounded to float once.
- **The scalar `Curve`** (the data of a `.scurve` asset and of curves embedded in other data) adds what it does outside
  its keys, `PreExtrapolation` and `PostExtrapolation`: `Hold` (the end key's value, the timeline's behaviour), `Loop`
  and `PingPong` (the keys repeat with the period from the first key's time to the last's, mapped with an exact `fmod`;
  `Hold` when every key lies at one time), and `Linear` (on from the end key's value along the curve's slope at that end:
  a `Linear` segment's slope, a `Cubic` segment's end slope, 0 after a `Constant` segment). A curve without keys is 0. A
  NaN input gives NaN; an infinite one lies beyond every key, where `Loop` and `PingPong`, which have no phase there, give
  NaN. At most `MaxCurveKeys` (1024) keys, `ValidateCurve` checks the order.
- **Two evaluators, one semantics.** The engine's (`Curve::Evaluate`, and `EvaluateCurveKeys` over any key type that
  specializes `CurveKeyTraits`) serves C++ users and the editor: the visual script editor's timeline evaluates its tracks
  of up to four components through it, per component. The scripting runtime keeps a managed one (`CurveMath` and
  `CurveDefinition` in `Strada-ScriptCore`), through which timeline tracks and curve assets evaluate, so that graphs
  sample curves without allocating or crossing into native code. `Tests/Data/Curves/Differential.json` holds the two
  together: its cases (curves as `.scurve` content, with every interpolation, extrapolation, jump, set and automatic
  slope, uneven and negative times, non-finite inputs) are evaluated by StradaTests and by ScriptCoreTests to the same
  values, within 1e-6 of the larger of 1 and the value. The tolerance, not bit equality, is the contract: compilers may
  fuse a multiply and an add on some platforms (Clang on arm64), which .NET never does. The values come from a third
  evaluator, apart from both under test: `Tests/Data/Curves/MakeDifferential.py` (standard library only) writes the file
  and stops where its values disagree with the ones derived by hand that most cases carry. A change to the semantics
  changes both evaluators, that tool's evaluator and the file's values (written by running it) together.
- **The timeline keeps its own format.** The `Timeline` setting inside `.sgraph` documents (§10.6) is read, written and
  checked by the timeline's code exactly as before; its keys keep vector values and optional slopes, and its
  interpolation is the engine's `CurveInterpolation` under its old name. Only the evaluation moved.
- **`.scurve`** is a data asset type (§6.4) registered as `Curve` (`Strada/Asset/CurveAsset.h`, with the field table and
  the specification): its file (§7.3), the content browser's New Curve, the inspector and the commands `curve.create`,
  `curve.get` and `curve.set` come from that registration, and the FeatureTest project's `Curves/Response.scurve` is its
  fuzz seed.
- **The curve editor** is one widget with two hosts, the visual script editor's Timeline drawer (§11.1) and the
  inspector's editor of `.scurve` assets. Its model, `CurveView<TDocument>` (`Editor/UI/CurveView.h`, pure, tested
  without ImGui), holds the view transform (ranges kept in double, zoom about a point, pan, frame), hit testing (keys,
  tangent handles, event lanes, curves, empty space) and the gestures turned into intents: documents to commit, made
  from the document at the start of the interaction and the total change so far. Its widget, `CurveEditor<TDocument>`
  (`Editor/UI/CurveEditor.h`), draws the header row (the selected key's time and value, interpolation, tangents,
  Frame), the ruler, the value lines, event lanes, curves, keys, tangent handles, the marquee, a playhead and the key
  menu, and reports `CurveEditorChange`s (a drag's frames, its end or its cancel, a header field's change, a discrete
  commit with an optional merge key) that the host commits as undo steps; it never edits a document itself. A host's
  document reaches both through `CurveDocumentTraits<TDocument>`, which the host specializes with its own reads and
  edits and its domain: the timeline's (`VisualScriptCurveView.h`) forwards to `VisualScriptTimeline.h`'s functions
  over the bounded domain `[0, Length]` with a step of 0.01 s, so the drawer edits as it did; the curve's
  (`Editor/UI/CurveEditing.h`) edits a `Curve` over an unbounded domain (any finite time, framed from its keys, its
  curve drawn across the whole view through `Curve::Evaluate`, extrapolations included), snapping to and nudging by a
  hundredth of the visible span rounded to 1, 2 or 5 times a power of ten; a key's unset slope keeps the number it
  holds, so an edit rewrites only what it changes. Member definitions live in `CurveViewImpl.h` and
  `CurveEditorImpl.h`, included only where a document type is instantiated, once (`template class
  CurveView<VisualScriptTimeline>;` in `VisualScriptCurveView.cpp`, `CurveView<Curve>` in `CurveEditing.cpp`, the
  widgets in `VisualScriptCurveEditor.cpp` and `CurveAssetEditor.cpp`). The `.scurve` host, `CurveAssetEditor`, is the
  `Curve` type's drawer (§6.4): the widget above the type's field table, each drag one step under a key of its own
  (Escape commits the start under it, so the step disappears), a run of arrow-key nudges one step, a header field one
  step while it stays active, every other edit a step of its own; a drag, or a header field's drag, whose curve changes
  elsewhere meanwhile (an undo, `curve.set`) ends without an edit (`CurveEditor::EndHeaderInteraction` lets the field go).
  While its view has the keyboard it takes Delete, Ctrl+Backspace, Ctrl+A, the arrows, F and, during a gesture, Esc as
  focused routes, so they never reach the editor's editing keys; it claims the editing chords among them
  (`CurveAssetEditor::FocusedChords`) on the inspector's root window (`UI::UpdateEditingChordClaim`,
  `Editor/UI/EditingChordClaims.h`), which `FindClaimedEditingChords` reads (§11.1), so that they never reach the
  scene while its key menu is open (no focused route takes a key then, so they do nothing) and the Edit menu shows none
  of them. Its time and value fields keep the digits their format does
  not show.
- **Scripts** read a curve asset through the managed class `Strada.Curve` (§10.4), on §6.4's managed pattern:
  `Evaluate(time)` is a pure node and evaluates the runtime's copy of the keys with `CurveDefinition`, so it allocates
  nothing and crosses no boundary per sample. The copy (`CurveCache`, one per asset handle, shared by every reference to
  the asset) is read in bulk by `Curve_Read(handle, heldVersion, header, keys, capacity)`, which writes only the version
  when the runtime holds the asset's current one, and asks for room when the keys do not fit; unset slopes cross as NaN,
  which `CurveDefinition` resolves as automatic, as the differential test's reader does. A copy asks again only after
  the engine said data assets may have changed (`InvalidateDataAssets`, before C# scripts and graphs run, and before a
  debugger request that may resume a graph, when the registry's epoch moved), so an edit in the inspector while playing shows on the next frame. A curve that cannot be read
  evaluates to 0, logged once until data assets change again. StradaTests' curve probe compares a script's values with
  `Curve::Evaluate` within the differential test's tolerance.

## 7. Scene and ECS

### 7.1 Scene

`Scene` owns an `entt::registry`, a `UUID → entt::entity` map, scene settings (`SceneSettings`: `Physics` with the
gravity and `Renderer`, field tables like components) and the runtime subsystems while playing (`PhysicsScene`,
`AudioScene`, script instances).

Lifecycle: `OnRuntimeStart/Stop` (physics + scripts + audio), `OnSimulationStart/Stop` (physics only),
`OnUpdateRuntime(ts)`, `OnUpdateSimulation(ts, camera)`, `OnUpdateEditor(ts, camera)`, `OnViewportResize(w, h)`.
Play mode runs on a deep copy (`Scene::Copy`); stopping restores the editor scene untouched.

Runtime update order per frame: the world-transform cache's frame begins (`BeginTransformFrame`, below) → UI (the mouse
over buttons, clicks and `OnClick`) → scripts `OnUpdate` → physics fixed steps (accumulator, default 60 Hz; scripts'
`OnFixedUpdate` before each step; characters are then drawn between their last two steps, §8) → contact/trigger events
dispatched to scripts → audio update → particles → deferred entity destruction → render submission, which refreshes
the cache first (`UpdateTransforms`). Entity destruction requested during iteration is **deferred** to the end of
the frame; creation is immediate. Editor edits of the running scene happen between frames, so `EditorContext`
completes their destruction at once (`Scene::FlushPendingDestruction`): later edits and undo, also while paused, find
the entities gone.

**The world-transform cache** (slice 2's track IN; `Scene/SceneTransforms.cpp`). An entity's world matrix is its
`TransformComponent`'s local matrix under its parent's world matrix. The scene keeps every entity's world matrix in a
cache, so that the renderer reads matrices instead of walking parents through the UUID map, and so that each entity's
matrix of the previous frame is known (velocities, §6.2). `Scene::GetWorldTransform` answers from it and is never
stale.

- **Who writes transforms, and how a write is noticed.** Scripts (through their component accessors), the editor
  (inspector, gizmo, commands, undo), physics (the write-back after each step and the drawing between steps, §8),
  prefab instantiation, `SetWorldTransform`, `SetParent` and the serializers all write `TransformComponent` and
  `RelationshipComponent` fields in place, and the fields stay public. Nothing tells the cache: EnTT's update signal
  fires only through `patch` and `replace` (`entt/entity/mixin.hpp:352-354`), never for a write through a reference.
  So the cache notices writes by comparison. Each entity's entry keeps the inputs its world matrix was made from: the
  bits of `Translation`, `Rotation` and `Scale` (compared with `memcmp`, so `-0` and `+0` differ and a NaN equals
  itself), the parent's UUID, the handle it resolved to, the entity it was computed under and that entity's `Version`
  (a number every computed matrix gets anew). An entry is current while its inputs are bit for bit what the
  components hold and the parent's entry is current with the same `Version`. Entries are destroyed with their
  entities; an entity made since the last refresh (created, duplicated, instantiated, made again by undo) has none
  and is computed.
- **Which parent counts.** The `Parent` UUID of `RelationshipComponent` decides, whatever the children lists say. A
  resolved handle is reused while an entry of that UUID lives there (a handle's version has 12 bits,
  `entt/entity/entity.hpp`, so a valid handle alone would not prove it after 4,096 reuses of its slot); otherwise the
  UUID is looked up again, so a parent the scene does not hold makes the entity a root until an entity of that UUID is
  made. A cycle of parents, which `SetParent` refuses but a file or a direct write may hold, makes every entity on it
  a root, whichever entity a walk starts from; what hangs below the cycle is computed under it. Walks find cycles with
  Brent's method, so every walk ends.
- **One product.** The refresh and every read compute `W(e) = Math::ComposeWorldTransform(W(parent), L(e))`, with
  `W(root) = L(root)`, top down, so a matrix read through the chain and the same matrix after a refresh have the same
  bits, in every run and at every frame rate. `ComposeWorldTransform` is the product of affine matrices: glm's sums in
  glm's order (`glm/detail/type_mat4x4.inl`, `mul4x4`) without the terms that the bottom rows make zero, and a bottom
  row of exactly (0, 0, 0, 1). It is the one function every world matrix goes through, kept out of line, so that no
  call site is contracted into fused multiply-adds differently from another. Measured in Release on the reference
  machine over 12,000 products: 0.086 ms against 0.114 ms for glm's full product. Slice 1's walk multiplied bottom up
  (`L(root) × (... × L(e))`) with the full product. The two give the same bits for roots, and for their children
  except where a sum of zeros keeps another sign of zero; two or more levels below a root they may differ in the last
  bits. Physics, which reads world transforms when it syncs bodies, may therefore see slightly different values than
  before for bodies below other entities, and a sample's state hash may change once, with this track (the three-rate
  cases compare runs of one build, so they hold).
- **A read between refreshes** (`GetWorldTransform`) walks the entity's chain up to its root and compares each level's
  inputs, then returns the cached matrix when the whole chain is current; otherwise it multiplies from the highest
  entity whose entry is not current down to the entity. A script that moves a parent and then reads a child in the same
  frame sees the move, and pays a product per level below the move. A read changes no cached matrix: a write reaches the
  cache through the refresh that follows it, in the frame that refresh belongs to, which is what keeps the previous
  matrix (below) the frame before's whatever reads came between. Main thread only, as the scene is.
- **The refresh.** `Scene::UpdateTransforms` (zone `Scene.UpdateTransforms`) visits every entity that has a
  `TransformComponent` once, in the order the transforms were added, which puts most parents before their children:
  an entity whose entry was computed under the parent link it has now, and whose parent this refresh has checked, is
  compared and, when its inputs or its parent's `Version` changed, computed again from its parent's entry; any other
  entity walks up to the first ancestor this refresh has checked (or a root, or a cycle) and brings the walk's entries
  up to date top down. `RenderScene` calls it before it submits anything, inside `Renderer.RenderScene`: after the
  scripts, physics' drawing between steps and deferred destruction's `OnDestroy`, and after what the editor's
  inspector, gizmo and commands, tests and thumbnails write between a scene's update and its views. The renderer then
  reads each entity it draws through `Scene::GetCachedWorldTransform`, which does not check the chain: exact for every
  entity when nothing wrote a transform since the refresh; an entity made since is computed as `GetWorldTransform`
  computes it. Engine phases that run before the views (physics' syncs, audio, particles, the scripts' bindings) read
  through `GetWorldTransform`. A frame of the player refreshes once; the editor playing with its scene view and game
  view open refreshes twice, and the second refresh only compares. A run that draws nothing (a headless test run)
  refreshes nothing.
- **The previous matrix.** `CachedWorldTransform::Previous` is the world matrix the cache held for the entity when the
  scene's current frame began (`Scene::BeginTransformFrame`): what the last refresh before that computed, which is the
  previous frame's when that frame was drawn. It equals `World` for an entity in its first frame, and for every entity
  of a scene that never began a frame. It is kept lazily: an entry remembers the frame its matrix last changed in and
  keeps the matrix it replaced only then, so beginning a frame costs nothing per entity, and every view of a frame
  reads the same previous matrices. Each scene has one owner that begins its frames, once per frame of the scene and
  before anything of the frame writes:
  - the scene itself, first thing in `OnUpdateRuntime` while it runs: the player's `SceneRunner` and the editor's play
    mode call it every frame, a paused scene without queued steps begins its frame and returns, and every frame
    `play.advance` runs begins its own;
  - the editor's play mode (`PlayMode::Update`, once per editor frame), for the scene the views show while no game
    frame runs: the edited scene while nothing plays, and the running scene in a frame the debugger holds
    (`GetFrameStepRefusal`).

  Frames that no view drew refresh nothing, so after them `Previous` is older than the frame before. A renderer that
  keeps history tells the frame after the one it drew by `Scene::GetTransformFrame` (the frames begun) and
  `Scene::GetSerial` (unique among the scenes this process made, a copy included): see "Velocities and cuts" in §6.2.
- **Costs.** An entry is 288 bytes, kept in a storage of the scene's registry for an internal type that
  `ComponentRegistry` does not list: it is no component, so it is never serialized, copied (`Scene::Copy`, duplicates
  and prefab instances start without entries), shown or scripted. EnTT's storage keeps elements in pages that never
  move (`entt/entity/storage.hpp`, `assure_at_least`), so the refresh adds entries while it holds references to others.
  The target is a refresh of 12,000 entities in at most 0.5 ms in Release on the reference machine, whether none, some
  or all of them moved; the benchmark case of `WorldTransformCacheTests.cpp` (`STRADA_TRANSFORM_BENCHMARK=1`) measures
  the three. An entry keeps its local matrix (`Local`, the product of its translation, rotation and scale), so an
  entity whose own inputs did not change is computed again under its parent's new matrix with one affine product: what
  every child costs when its root moves. Measured on IN's final tree (Release, 100 roots of 119 children, one refresh
  per frame, 300 frames, the zone's median), with the reference machine idle: 0.13 to 0.14 ms with nothing moved,
  0.17 to 0.18 ms with 1,000 children moved, 0.31 to 0.34 ms with every root moved, every matrix computed again; inside
  a rendered frame of the instancing benchmark (`Renderer/InstancingBenchmarkTests.cpp`, 12,001 entities, where the
  renderer's work leaves the cache colder) 0.15 to 0.22, 0.26 to 0.29 and 0.44 to 0.50 ms. In the same sitting the
  tree before the local matrix was kept took 0.51 ms alone and 0.69 to 0.71 ms in the frame with every root moved; the
  in-frame figures of that sitting read about 1.5 times higher than in the machine's fastest, where the same refresh
  took 0.31 to 0.34 ms in the frame, cause unknown, while the isolated reading was at its usual value. At slice 2's gate
  (two sittings with the gauge out of band, as in §6.2): 0.32 ms alone with every root moved, and 0.42 to 0.44 ms in the
  frame (0.14 to 0.20 with nothing moved, 0.23 to 0.27 with 1,000 children moved), while the first tree read 0.50 to
  0.51 ms alone and 0.65 to 0.66 ms in the frame. In its two later sittings, in band: 0.29 to 0.30 ms alone with every
  root moved, and 0.31 to 0.36 ms in the frame (0.15 to 0.17 with nothing moved, 0.18 to 0.21 with 1,000 children
  moved), while the first tree read 0.49 ms alone and 0.48 to 0.49 ms in the frame: the target is met in band. A read through a current chain
  costs about 0.05 µs per entity, half of slice 1's walk.

### 7.2 Components (`Strada/Scene/Components.h`)

Plain data structs, copyable, with PascalCase public fields. Light intensities are artist-friendly multipliers
(the renderer's exposure defaults suit them), not photometric units. Each component's serialized name, description and
field list are declared once in `ComponentTraits.h`; generic code derives JSON serialization (strict, typed,
transactional partial updates with readable errors), schemas, copies, the inspector and the `ComponentRegistry` entry
from that table. Fields carry optional hints: an inclusive range (enforced whenever JSON is read, so files, automation
and the editor all reject out-of-range values), a presentation (color, angle, multi-line text), the referenced asset
type, a one-sentence description, a visibility condition (the field only matters while an enum or bool field of the
same struct has one of at most 16 values, like a camera's orthographic size), a switch (`EnabledBy`: another bool field
of the table, not switched itself, that turns the field on; drawn by the inspector as a check box in front of the field),
what replaces the field (`OverriddenBy`: another field of the table that references one asset and is not replaced
itself; while it holds an asset the inspector shows the field disabled and says what sets it, as a collider's `Material`
replaces its `Friction` and `Restitution`; files and automation read and write the field as usual) and, for arrays, the most elements they hold (`MaxCount`, enforced whenever JSON is read, like ranges; the inspector's
add buttons stop there); the conditions, switches and replacements are checked at compile time. Values that skip the JSON checks
(scripts, values made in C++) can be brought into range, and their arrays down to their `MaxCount`, with
`ClampToFieldRanges` (`FitToFieldRanges` for imported data, which also replaces NaN and says what it changed). Field tables of other structs (renderer settings,
materials, data assets) use the same hints. Each component
also names its category (`Core`, `Rendering`, `Physics`, `Audio`, `Scripting` or `UI`). The registry provides
type-erased add/has/remove/serialize/deserialize/copy/describe functions. The registry is the single source of truth for
scene serialization, scene copy, automation (`component.*` commands), the inspector "Add Component" menu (grouped by
category), and script interop component lookups.

| Serialized name | Struct | Fields |
|---|---|---|
| `ID` | `IDComponent` | `ID` (UUID) |
| `Tag` | `TagComponent` | `Tag` |
| `Transform` | `TransformComponent` | `Translation` vec3, `Rotation` quat (x, y, z, w), `Scale` vec3 — local to parent |
| `Relationship` | `RelationshipComponent` | `Parent` UUID, `Children` UUID[] |
| `Camera` | `CameraComponent` | `Projection` (Perspective/Orthographic), `PerspectiveFOV` (deg), `PerspectiveNear`, `PerspectiveFar`, `OrthographicSize`, `OrthographicNear`, `OrthographicFar`, `Primary`, `FixedAspectRatio`, `AspectRatio` |
| `Mesh` | `MeshComponent` | `Mesh` asset, `Materials` asset[] (overrides per material slot, 0 = the mesh's default), `CastShadows`, `Visible` |
| `DirectionalLight` | `DirectionalLightComponent` | `Color`, `Intensity`, `CastShadows`, `LightSize` (angular diameter, deg; controls penumbra) |
| `PointLight` | `PointLightComponent` | `Color`, `Intensity`, `Range`, `CastShadows` |
| `SpotLight` | `SpotLightComponent` | `Color`, `Intensity`, `Range`, `InnerConeAngle`, `OuterConeAngle` (half-angles, deg), `CastShadows` |
| `SkyLight` | `SkyLightComponent` | `Environment` asset (HDRI), `Intensity`, `Rotation` (deg around Y), `SkyboxBlur` (0..1), `DrawSkybox`, `AmbientColor` (used when no environment) |
| `RigidBody` | `RigidBodyComponent` | `Type` (Static/Dynamic/Kinematic), `Mass`, `LinearDamping`, `AngularDamping`, `GravityFactor`, `Layer`, `LockTranslation` bvec3, `LockRotation` bvec3, `ContinuousCollision`, `AllowSleep`, `Interpolation` (None/Interpolate), `InitialLinearVelocity`, `InitialAngularVelocity` |
| `BoxCollider` | `BoxColliderComponent` | `HalfExtents`, `Offset`, `IsTrigger`, `Material` (a `.sphysmat`; 0 = none, §8), `Friction`, `Restitution` (both replaced by a set `Material`) |
| `SphereCollider` | `SphereColliderComponent` | `Radius`, `Offset`, `IsTrigger`, `Material`, `Friction`, `Restitution` |
| `CapsuleCollider` | `CapsuleColliderComponent` | `Radius`, `HalfHeight`, `Offset`, `IsTrigger`, `Material`, `Friction`, `Restitution` |
| `MeshCollider` | `MeshColliderComponent` | `Mesh` asset (0 = use `Mesh` component), `Convex`, `IsTrigger`, `Material`, `Materials` (`.sphysmat`[], one per submesh of a triangle mesh, at most 31; 0 or missing = `Material`), `Friction`, `Restitution` |
| `Joint` | `JointComponent` | `Type` (Fixed/Hinge/Ball/Distance/Slider), `ConnectedEntity` UUID (0 = the world), `Anchor` (local), `ConnectedAnchor` (the connected entity's local space, or world), `Axis` (local), `LimitsEnabled`, `MinLimit`/`MaxLimit` (hinges: deg; sliders: m), `MinDistance`/`MaxDistance` (-1 = the starting distance), `MotorEnabled`, `MotorSpeed` (deg/s or m/s), `MotorMaxForce` |
| `CharacterController` | `CharacterControllerComponent` | `Radius`, `Height` (the whole capsule), `Offset` (the capsule's center), `StepHeight`, `MaxSlopeAngle` (deg, 1..89), `Mass` (kg), `PushForce` (N), `GravityFactor` (0..1000), `Layer` |
| `AudioSource` | `AudioSourceComponent` | `Clip` asset, `Stream` (read the clip's file while playing), `Volume`, `Pitch`, `Loop`, `PlayOnStart`, `Spatial`, `MinDistance`, `MaxDistance`, `Bus` (the mixer's bus by name, any letter case; empty, or a name the mixer lacks, is Master), `Priority` (0 to 255; lower goes silent first at the engine's limit), `MaxVoices` (1 to 64; its main voice and its one-shots), `DopplerLevel` (0 to 5; how much of the doppler effect a spatial source takes, §9.8) |
| `AudioListener` | `AudioListenerComponent` | `Active` |
| `ReverbZone` | `ReverbZoneComponent` | `Shape` (Sphere/Box), `Radius` (m, a sphere), `HalfExtents` (m, a box), `FadeDistance` (m), `Bus` (the mixer's bus the zone sends to, any letter case; empty, or a name the mixer lacks, sends nothing), `SendLevel` (0 to 1); placed and turned by its entity, sized times its scale (a sphere by its largest axis), §9.8 |
| `Script` | `ScriptComponent` | `ClassName`, `Fields` (name → typed value) |
| `VisualScript` | `VisualScriptComponent` | `Graph` asset (a visual script, `.sgraph`; 0 = none), `Variables` (public variable name → typed value, as `Script.Fields`; values under a former name of a variable apply to it) |
| `Text` | `TextComponent` | `Text`, `Font` asset (0 = default), `Color`, `FontSize`, `ScreenSpace`, `Alignment` (Left/Center/Right), `LineSpacing` |
| `SpriteRenderer` | `SpriteRendererComponent` | `Color`, `Texture` asset, `Tiling`, `ScreenSpace` |
| `ParticleSystem` | `ParticleSystemComponent` | `Emitting`, `Rate` (per second), `Burst`, `MaxParticles`, `LifetimeMin`/`LifetimeMax` (s), `SpeedMin`/`SpeedMax` (m/s), `Shape` (Point/Sphere/Cone/Box), `ConeAngle` (deg), `Radius`, `BoxHalfExtents`, `GravityFactor`, `Drag`, `StartSize`/`EndSize`, `StartColor`/`EndColor`, `Intensity`, `Texture` asset (0 = soft dot), `BlendMode` (Additive/Alpha), `WorldSpace`, `DestroyWhenFinished` |
| `PostProcess` | `PostProcessComponent` | `Global`, `BlendDistance` (m), `Priority`, `Weight` (0-1), and `Override<Setting>` switching `<Setting>` for `EV100`, `Tonemapper`, `AmbientOcclusion`, `AmbientOcclusionIntensity`, `Bloom`, `BloomIntensity`, `Contrast`, `Saturation`, `Temperature`, `Tint`, `ColorFilter`, `ChromaticAberration`, `Vignette`, `VignetteSmoothness`, `FilmGrain` (the renderer settings of the same names, with their defaults and ranges) |
| `UICanvas` | `UICanvasComponent` | `ScaleMode` (ConstantPixelSize/ScaleWithScreenHeight/ScaleWithScreenSize), `ReferenceResolution` vec2 |
| `UIRect` | `UIRectComponent` | `AnchorMin`, `AnchorMax` (0..1 of the parent), `Position`, `Size` (canvas units), `Pivot` (0..1), `Visible`, `Opacity` |
| `UIImage` | `UIImageComponent` | `Color`, `Texture` asset, `PreserveAspect`, `CornerRadius` |
| `UIText` | `UITextComponent` | `Text`, `Font` asset (0 = default), `Color`, `FontSize` (canvas units per line), `Alignment` (Left/Center/Right), `VerticalAlignment` (Top/Middle/Bottom), `Wrap`, `LineSpacing` |
| `UIButton` | `UIButtonComponent` | `Interactable`, `NormalColor`, `HoveredColor`, `PressedColor`, `DisabledColor` |
| `Prefab` | `PrefabComponent` | `Prefab` asset, `SourceEntity` UUID |

Screen-space `Text`/`SpriteRenderer` use the entity translation in normalized viewport coordinates
(x, y ∈ [0, 1], origin top-left) and scale in pixels-per-unit, so games can build HUDs without a UI framework; the
translation's z orders them (higher values on top) and the transform's X and Y axes give their rotation around Z.
Text hangs from its origin (the top of the first line), where lines start, are centered or end depending on the
alignment. World-space sprites are the unit quad in the entity's XY plane (facing +Z), world-space text lies in the
same plane with `FontSize` world units per line; both are unlit.

The screen-space UI (HUDs, menus) is made of entities. A `UICanvas` entity covers the screen, wherever it is in the
hierarchy, and its descendants are the UI, laid out by `UILayout` (Scene module; pure functions of the components,
the hierarchy and the viewport size, unit tested) and drawn in hierarchy order: parents before children, siblings in
order, an element's image before its text. The scale mode sets the canvas units: with `ScaleWithScreenSize` (the
default) the whole `ReferenceResolution` fits on any screen (the screen is `ReferenceResolution.y` units high when it is
wider than the reference, `ReferenceResolution.x` units wide when it is narrower), `ScaleWithScreenHeight` fits the
height alone (narrower screens cut off the sides), and with `ConstantPixelSize` a unit is a pixel. A `UIRect` places its
element in the parent's rectangle
(the screen for the canvas's children; y downwards): on each axis the anchors mark a span of the parent, the rectangle
is `Size` larger than the span (a negative size makes an empty rectangle), and its pivot sits at the pivot's point of
the span moved by `Position`. Entities without a `UIRect` take their parent's rectangle; a hidden `UIRect` hides its
subtree; opacities multiply down the hierarchy. The Transform of UI entities, and a `UIRect` on the canvas entity, are
not used. `RenderScene` lays the UI out on the renderer's viewport and submits it (`SceneRenderOptions::DrawUI`), with
a button's state color in place of its image's color; with `SceneRenderOptions::PlaceCanvas` each top-level canvas's UI
is drawn on a plane in the world instead (the editor's scene view), after tonemapping, over the grid and behind
surfaces, its edges anti-aliased over a pixel of the screen at any distance. Title, menu and loading scenes need no
camera: without a primary camera, `RenderSceneFromPrimaryCamera` draws a scene's screen-space content (UI canvases,
screen-space sprites and text: `HasScreenSpaceContent`) over black with `SceneRenderOptions::DrawWorld` off (no meshes,
lights, sky, particles, world-space sprites or text, debug lines or post effects), and renders nothing for a scene with
neither.

While the scene runs, `Scene` follows the mouse (Input's cursor, in pixels of the scene's viewport size) at the start of
each update: the element under the cursor that takes clicks (a `UIImage` or `UIButton` whose opacity is not zero; the
last drawn wins) passes them to its nearest `UIButton` (itself or an ancestor below the canvas), so images inside a
button pass on its clicks and other images keep them from what they cover. An interactable button there is hovered; a
left button press on it presses it, and a release on the same button clicks it: `WasUIButtonClicked` holds for that
update, and `OnClick(button)` runs on the scripts of the button and of its ancestors, nearest first, before the frame's
`OnUpdate`. Presses and releases are Input's flags, which last until the next game frame in the editor's play mode and
one application frame elsewhere, or the button's changes since the last update, which cover a scene updated after
Input's frame ended. A release that `Input::ReleaseKeysAndMouseButtons` made (the window lost the focus, the editor took the
game's input back: `Input::WasMouseButtonReleasedByReset`) cancels the press instead of clicking, as interfaces cancel
a mouse capture. `IsPointerOverUI` tells whether an element that takes clicks is under the cursor (scripts'
`Input.IsPointerOverUI`), so games can ignore world clicks that land on the UI. Buttons clicked in code
(`ClickUIButton`) click at the next update unless disabled. A locked cursor or `SetUIPointerEnabled(false)` (the
editor's `PlayMode` sets it before each update of the running scene: off while the game view does not have the input)
points at nothing and drives no buttons, and stopping forgets the pointer and pending clicks.

Particle systems are simulated on the CPU by `ParticleEmitter` (Scene module, independent of the ECS, with a SplitMix64
generator of its own so a seed gives the same particles everywhere). `Scene::UpdateParticles` runs them each runtime
update (the editor also calls it while editing, as a preview): a system emits its `Burst` when it starts (with the
run, or when its entity appears), then `Rate` particles per second while `Emitting`, which age, fall with the scene's
gravity times `GravityFactor`, lose `Drag` of their velocity per second and die after their lifetime; with
`DestroyWhenFinished` a running system that stopped emitting destroys its entity after its last particle. Particles are
drawn as camera-facing sprites (the built-in `Particle` texture, a soft dot, by default): additive sprites add their
light (they bloom when bright) and are never picked; alpha sprites cover what is behind. The quad pipeline blends
premultiplied colors, so both kinds share it.

Post-process volumes (`PostProcessVolumes`, Scene module, independent of rendering) start from the scene's renderer
settings and apply each `PostProcess` component that reaches the camera, in order of `Priority` (lowest first; equal
priorities in hierarchy order), with a blend: `Weight` for global volumes; for local ones `Weight` times 1 inside their
box (the unit cube placed, rotated and scaled by the entity's world transform) fading linearly to 0 at `BlendDistance`
meters from it. A volume changes only the settings whose `Override` switch is set: numbers move towards its values by
the blend. `Bloom` fades instead of switching (`PostProcessVolumes::Blender`): how much of the bloom is on moves towards
the volume's `Bloom` by the blend like a number and scales `BloomIntensity` (the renderer treats bloom that is off like
bloom with no intensity), so it fades out into a volume that turns it off, fades in where one turns it back on, and
switches off only once faded out completely. `AmbientOcclusion` and `Tonemapper`, which the renderer has no strength
to fade by, take the volume's value once the blend reaches one half. Values scripts set outside the fields' ranges act
as the nearest bound, and scenes without volumes skip the work.

Colliders attach to the entity's own rigid body; multiple colliders on one entity form a compound shape. An
entity with colliders and no `RigidBody` is a static body. Entity scale is applied to shapes. A `CharacterController`
makes the entity a character instead (§8): its colliders and `RigidBody` are left out, with a warning. The colliders
of the entities below a character are its own parts, which neither block it nor touch it.

### 7.3 Serialization formats (JSON, written with tabs, keys in stable order)

Every file starts with a header: `"Strada": { "Version": <int>, "Type": "Scene" | "Prefab" | "Material" | "Project" | "AssetRegistry" | "VisualScript" }`,
or the name of a data asset type (§6.4).

Scene (`.sscene`):

```json
{
	"Strada": { "Version": 1, "Type": "Scene" },
	"Scene": { "Name": "Main", "Settings": { "Physics": { "Gravity": [0, -9.81, 0] }, "Renderer": { } } },
	"Entities": [
		{ "ID": "1234567890123", "Components": { "Tag": { "Tag": "Player" }, "Transform": { } } }
	]
}
```

Asset references are handle strings. When reading (files, automation), an asset field may also be given as
`"asset://<path relative to Assets/>"` or `"builtin://<Name>"`; it is resolved and stored as a handle.

Prefab (`.sprefab`): same entity array as a scene (root first). Instantiation assigns fresh UUIDs and remaps all
intra-prefab references (hierarchy, entity-typed script fields and visual script variables).

Project (`<Name>.sproj`, `ProjectSettings` with field tables, validated on every read): `Name`, `Identity` (`Company`,
`Product`, both optional: the game's data folder and bundle identifier, §13.1), `AssetDirectory`
(relative, inside the project), `ScriptModule` (the game assembly, relative), `StartScene` (scene asset reference),
`Window` (`Title`, `Width`, `Height`, `Fullscreen`, `VSync`, `Resizable`, `Icon` texture; for exported games, whose
player sets the icon on its window), `Physics`
(`FixedTimestep`, `MaxStepsPerFrame`, `CollisionSteps` (§8), `Layers`: 1 to 16 unique names indexed by
`RigidBody.Layer`, `IgnoredCollisions`: layer index pairs that do not collide), `InputActions` (an `InputActions`
asset reference, the game's actions, §7.4; 0 for none; a compatible field) and `Mixer` (a `Mixer` asset reference, the
buses the game's sounds play on, §9.5; 0 for none; a compatible field). Opening a project opens its asset directory first so asset references resolve; settings from
newer versions are skipped with warnings. New projects (`Project::Create`) get the project file, `Assets/` and the
start scene `Assets/Scenes/Main.sscene` in an empty or new directory.

Asset registry (`Assets/AssetRegistry.sreg`): `"Assets": [ { "Handle", "Type", "Path" } ]` sorted by path, paths
relative to `Assets/` with forward slashes (case-sensitive, portable characters only). The asset directory is scanned
when a project opens: new files get fresh handles, missing files keep theirs. Hidden files and folders (names starting
with `.`) are ignored, and symbolic links are skipped with a warning (they could make any file of the machine part
of the project and its exported games). Invalid entries are skipped with warnings; an unreadable registry refuses to open (continuing
would break references).

Material (`.smat`): `"Material": { <MaterialAsset fields> }`; missing fields keep their defaults.

Data asset (`<extension>` of its type, `DataAssetSerializer`, §6.4): `"Strada": { "Version", "Type": "<Name>" }`, then
`"<Name>": { <the fields of the type's struct> }`, every field written, in field table order. Reading refuses a header of
another type, a version below 1 or above the type's `FormatVersion`, a missing or non-object content, a migration that
fails, throws or leaves no object, a value of the wrong kind or outside its range, an array longer than its `MaxCount`, and a value the type's
`Validate` refuses; missing fields keep their defaults, and unknown fields are warnings in files (errors in automation
input). Older versions are migrated on reading, and saving writes the current one. Each type's fields are documented by
its field table (the descriptions the inspector and the commands' schemas show) and its section of the guides.

Curve (`.scurve`, the data asset type `Curve`, version 1, §6.5): `"Curve": { "PreExtrapolation", "PostExtrapolation",
"Keys": [ { "Time", "Value", "Interpolation", "HasIn", "In", "HasOut", "Out" } ] }`. The extrapolations are `Hold`,
`Loop`, `PingPong` or `Linear`; `Interpolation` is `Cubic`, `Linear` or `Constant`; `In` and `Out` apply only where
`HasIn` and `HasOut` are true (the slope is automatic otherwise). At most 1024 keys, in time order (refused otherwise),
every number finite.

```json
{
	"Strada": { "Version": 1, "Type": "Curve" },
	"Curve": {
		"PreExtrapolation": "Hold",
		"PostExtrapolation": "Linear",
		"Keys": [
			{ "Time": 0.0, "Value": 0.0, "Interpolation": "Cubic", "HasIn": false, "In": 0.0, "HasOut": true, "Out": 0.5 },
			{ "Time": 1.0, "Value": 1.0, "Interpolation": "Linear", "HasIn": false, "In": 0.0, "HasOut": false, "Out": 0.0 }
		]
	}
}
```

Physics material (`.sphysmat`, the data asset type `PhysicsMaterial`, version 1, §8): `"PhysicsMaterial": { "Friction",
"Restitution", "FrictionCombine", "RestitutionCombine", "RollingResistance", "Surface", "ImpactSound", "ImpactParticles" }`.
`Friction` and `RollingResistance` are at least 0 and `Restitution` 0 to 1; the combine modes are `GeometricMean`,
`Average`, `Minimum`, `Multiply` or `Maximum`; `Surface` is any text (empty for none); `ImpactSound` references an
`AudioClip` and `ImpactParticles` a `Prefab` (0 for none). A new material (`{}`) is what a collider without one does:
friction 0.6 by the geometric mean, restitution 0 by the maximum.

```json
{
	"Strada": { "Version": 1, "Type": "PhysicsMaterial" },
	"PhysicsMaterial": {
		"Friction": 0.9, "Restitution": 0.1, "FrictionCombine": "Minimum", "RestitutionCombine": "Maximum",
		"RollingResistance": 0.012, "Surface": "Gravel", "ImpactSound": "9906031496213900225", "ImpactParticles": "0"
	}
}
```

Input actions (`.sinput`, the data asset type `InputActions`, version 1, §7.4): `"InputActions": { "Actions": [ { "Name",
"Type", "PressThreshold", "ReleaseThreshold", "Bindings": [ { "Controls", "Gamepad", "DeadZone", "ResponseCurve",
"Invert", "InvertY", "Scale" } ] } ] }`. At most 256 actions of at most 16 bindings; `Name` non-empty, at most 64 bytes
and unique in any letter case; `Type` is `Button`, `Axis` or `Vector2`; `0 < ReleaseThreshold <= PressThreshold <= 1`;
`Controls` is empty or one, two or four control names separated by spaces that fit the action's type (§7.4's table);
`Gamepad` is -1 (any) to 15, `DeadZone` 0 to 0.99, `Scale` at least 0; `ResponseCurve` references a `Curve` (0 for
none). A new map (`{}`) has no actions.

```json
{
	"Strada": { "Version": 1, "Type": "InputActions" },
	"InputActions": {
		"Actions": [
			{ "Name": "Lean", "Type": "Axis", "PressThreshold": 0.5, "ReleaseThreshold": 0.4, "Bindings": [
				{ "Controls": "Key/A Key/D", "Gamepad": -1, "DeadZone": 0.0, "ResponseCurve": "0", "Invert": false, "InvertY": false, "Scale": 1.0 },
				{ "Controls": "Gamepad/LeftX", "Gamepad": -1, "DeadZone": 0.15, "ResponseCurve": "4815162342", "Invert": false, "InvertY": false, "Scale": 1.0 }
			] }
		]
	}
}
```

Mixer (`.smixer`, the data asset type `Mixer`, version 1, §9.5, §9.6 and §9.8): `"Mixer": { "Buses": [ { "Name",
"Parent", "VolumeDb", "Muted", "Effects": [ { "Kind", "Frequency", "Q", "GainDb", "DelaySeconds", "Feedback",
"RoomSize", "Damping", "Width", "WetDb", "DryDb", "ThresholdDb", "Ratio", "KneeDb", "AttackSeconds", "ReleaseSeconds",
"MakeupDb", "CeilingDb" } ], "DuckedBy", "DuckAmountDb", "DuckThresholdDb", "DuckAttackSeconds", "DuckReleaseSeconds" } ],
"Snapshots": [ { "Name", "Values": [ { "Bus", "Target", "Effect", "Parameter", "Value" } ] } ], "SpeedOfSound",
"Impacts": { "Bus", "MinImpulse", "FullImpulse", "MaxPerFrame" } }`. At most 32 buses
and 8 effects a bus; exactly one bus is `Master`, with an empty `Parent`; names are unique ignoring letter case, not
empty, at most 64 bytes; every other `Parent` names a bus of the list (empty is Master) and following parents reaches
Master. `Kind` is `LowPass`, `HighPass`, `Peaking`, `LowShelf`, `HighShelf`, `Delay`, `Reverb`, `Compressor` or
`Limiter`; an effect stores every parameter and reads those of its kind. Volumes run from -80 dB (silence) to +24 dB. A
bus's `DuckedBy` is empty or another bus whose level does not depend on this bus's output. At most 32 snapshots of at
most 128 values; snapshot names are unique ignoring letter case, not empty, at most 64 bytes; a value's `Target` is
`Volume`, `Muted` (a `Value` of 1 or 0) or `Effect` (`Effect`, the index of an effect of `Bus`, and `Parameter`, one that
effect's kind uses, both stored with every value), its `Value` within that target's range, and no two values of a
snapshot set the same thing. `SpeedOfSound` is 1 to 10,000 m/s (343 by default). `Impacts.Bus` is empty (no impact
sounds, the default) or a bus of the mixer; `MinImpulse` is 0 to 1,000,000 N·s (1 by default), `FullImpulse` 0.001 to
1,000,000 N·s (20), `MaxPerFrame` 1 to 64 (8). A new mixer (`{}`) is Master alone without snapshots or impact sounds. The
ducking and snapshot fields, the speed of sound and the impact settings joined version 1 with defaults within slice 3,
before any `.smixer` existed outside it, so no migration reads an older file.

```json
{
	"Strada": { "Version": 1, "Type": "Mixer" },
	"Mixer": {
		"Buses": [
			{ "Name": "Master", "Parent": "", "VolumeDb": 0.0, "Muted": false, "Effects": [ { "Kind": "Limiter", "CeilingDb": -1.0 } ] },
			{ "Name": "Engine", "Parent": "Master", "VolumeDb": -3.0, "Muted": false, "Effects": [ { "Kind": "LowPass", "Frequency": 8000.0, "Q": 0.7071 } ],
			  "DuckedBy": "Interface", "DuckAmountDb": -6.0, "DuckThresholdDb": -40.0, "DuckAttackSeconds": 0.05, "DuckReleaseSeconds": 0.5 },
			{ "Name": "Interface", "Parent": "Master", "VolumeDb": 0.0, "Muted": false, "Effects": [] }
		],
		"Snapshots": [
			{ "Name": "Paused", "Values": [
				{ "Bus": "Engine", "Target": "Volume", "Value": -12.0 },
				{ "Bus": "Engine", "Target": "Effect", "Effect": 0, "Parameter": "Frequency", "Value": 800.0 }
			] }
		]
	}
}
```

Binding profile (`<game data>/Input/<the actions asset's file name>.json`, `Moto.sinput.json`; `InputBindingProfile` in
`Strada/Asset/InputBindingProfile.h`, version 1, §7.4 "Rebinding"; a struct document, not a data asset):
`"InputBindingProfile": { "Bindings": [ { "Action", "Binding", "Controls", "Replaced" } ] }`, the bindings a player
changed while the game ran, only those that differ from the asset's, in the order they were set: `Action` the action's
name (with its letter case), `Binding` its index in the action (0 to 15), `Controls` the binding's text now (a
binding's text as in `.sinput`; empty unbinds it) and `Replaced` the asset's text it replaced. At most 4,096 entries.
Reading refuses another type, a newer version, a value of the wrong kind or out of range and a list too long, and warns
about unknown fields; it checks nothing against the actions. When the actions load, an entry applies only while its
action exists, its index is below the action's count, the asset's binding still reads the controls of `Replaced`
(compared as controls, in any letter case), its `Controls` fit the action's type and differ from the asset's, and no
earlier entry names the same binding; the others are dropped with one log line. The game writes the file atomically
(`FileSystem::WriteTextFile`), creating `Input/`, whenever what differs changes; a reset writes it without entries.

```json
{
	"Strada": { "Version": 1, "Type": "InputBindingProfile" },
	"InputBindingProfile": {
		"Bindings": [
			{ "Action": "Jump", "Binding": 0, "Controls": "Key/J", "Replaced": "Key/Space" },
			{ "Action": "Lean", "Binding": 0, "Controls": "Key/Left Key/D", "Replaced": "Key/A Key/D" }
		]
	}
}
```

Project template (`Templates/<id>/template.json`, `ProjectTemplate` in `Strada/Project/ProjectTemplate.h`):
`{ "Strada": { "Version": 1, "Type": "ProjectTemplate" }, "Template": { "Id", "Name", "Order", "Description",
"Requires": { "Sdk" }, "Tags", "Thumbnail", "ProjectFile" } }`. `Id` (lowercase letters and digits joined by single
hyphens, at most 40 bytes) is the folder's name; `Name` (at most 64 bytes), `Description` (400) and the tags (at most 8
of 24 bytes) are UTF-8 without control characters; `Order` is 0 to 1000; `Thumbnail` and `ProjectFile` are paths inside
the folder (no `..`, root or drive), the thumbnail a PNG of at most 1 MiB named in the folder itself (in a folder of the
project, leaving it out would leave out a file the project uses), the project file a `.sproj` (`Template.sproj`) outside
what the copy filter leaves out. A template without `ProjectFile` is the editor's default scene (`empty-3d`). Unknown
members are ignored and a newer version is refused; a damaged template is left out of the list with a warning.
Instantiating copies the folder through the project copy filter (below) without `template.json` and the thumbnail,
renames the project file to `<portable name>.sproj` and sets its `Project.Name` (and `Window.Title` when it was the
template's name), rewriting nothing else, then writes the root `.gitignore` (build output, `.strada/`, script outputs
and `Strada.props`) unless asked not to; it opens nothing and leaves nothing behind when it fails.

Example catalog (`Projects/catalog.json` in a source tree, `Examples/catalog.json` otherwise; `ExampleCatalog`):
`{ "Strada": { "Version": 1, "Type": "ExampleCatalog" }, "Examples": [ { "Id", "Name", "Order", "Description",
"Directory", "ProjectFile", "Thumbnail", "Requires": { "Sdk" }, "Needs": { "Paths", "Hint" } } ] }` with the template's
limits; `Directory` and `Thumbnail` lie under the catalog's folder, `ProjectFile` and the needed paths (at most 16, the
hint at most 400 bytes) under the example's, outside what the copy filter leaves out. An example is available when its
project file and every needed path exist (the Showcase needs the assets `Tools/FetchAssets.py` downloads). A damaged
entry, or a repeated id, is left out with a warning; a damaged catalog is an error. Examples are only ever opened as
copies, without the example's root `.gitignore` (the engine repository's: the Showcase's ignores the assets its fetch
script downloads, which a copy has and cannot fetch).

The project copy filter (`ProjectCopy`), which templates and examples share, copies a project into a new or empty
directory without `Tools/`, `Build/`, `.strada/`, `.git/`, `Scripts/bin/`, `Scripts/obj/`, `Scripts/Binaries/`,
`Scripts/Strada.props` and any `__pycache__/`, never following symbolic links, and removes what it wrote when it fails or
is cancelled; it compares folders under any spelling the file system takes, so a target inside the source is refused in
any letter case. A copied `*.csproj` loses its repository-only blocks, keeping its line endings and byte order mark: the
lines from one containing `<!-- strada:repository-only -->` to the next containing `<!-- /strada:repository-only -->`
(the samples' fallback reference to `../../../Strada-ScriptCore`); an unclosed block is copied as it is, with a warning.

Engine marker (`engine.json` beside a build tree's executables, written by CMake, never shipped in a game):
`{ "Strada": { "Version": 1, "Type": "EngineManifest" }, "Content": "<the checkout, absolute, forward slashes>" }`.
`Content` is optional and unknown members are ignored. A marker that cannot be read, is larger than 64 KiB, is not
JSON, has another type or a newer version, or whose `Content` is not a string naming an existing directory by an
absolute path makes the layout not a source tree, with one warning; reading it never throws or asserts.

Agent kit state (`.strada/agents.json` in a project set up for coding agents, written by `AgentKit`, §11):
`{ "Strada": { "Version": 1, "Type": "AgentKitState" }, "Files": { "<project-relative path>": "<hash>" } }`, the hash
(64-bit FNV-1a of the text without carriage returns, 16 lowercase hex digits) of each whole file the kit last wrote: the
game-making skill and the guides under `.strada/docs/`. A whole file whose hash is recorded is the kit's to update; a
skill that differs from both the engine's and its record was changed by someone and is kept, while the guides are always
replaced. A missing, damaged or newer state file counts as empty, and the next setup replaces it.

Profiler capture (`ProfilerCaptureSerializer`, §4.4). `profiler.capture` and `StradaRuntime --profile <file>` write it,
and `profiler.open` reads it. It is a Chrome trace event document in its object form, which `chrome://tracing` and
Perfetto are meant to open as it is (the trace event format's object form treats other top-level members as metadata;
no viewer was run against it yet):
`{ "Strada": { "Version": 1, "Type": "ProfilerCapture" }, "displayTimeUnit": "ms", "traceEvents": [ ... ] }`, compact
(one line). The events are told apart by their structure, never by their names, so a zone may be named `Frame` or
`GPU.Opaque` and a thread `GPU Viewport`:

- metadata events (`"ph": "M"`): `process_name` of process 1 (`"Strada"`) and, when there are GPU passes or counters,
  of process 2 (`"GPU"`); then a `thread_name` (`"args": { "name": ... }`) for every thread of process 1 (`"tid"`: its
  index + 1, so the main thread is `"tid": 1`, named in order from 1) and for every view of process 2 that a frame's
  GPU passes or counters use (`"tid"`: the view's index + 1, named in order from 1). Other metadata is ignored;
- one complete event (`"ph": "X"`) per frame on process 1, thread 1, named `Frame` with `"cat": "Frame"`, with
  `"args": { "Frame": <index>, "DroppedZones": <n> }`, and, when the frame holds counter records (§4.4, "Counters"),
  `"Counters": [ { "View": <tid of process 2>, "Values": { "<counter>": <count>, ... } }, ... ]` in the order they were
  recorded (a record of no counts is `"Values": {}`). A frame's events follow it;
- one complete event per zone on process 1, its `"cat"` its subsystem (`Other` when it has none), with
  `"args": { "Frame": <index>, "Depth": <depth> }`, plus `"Value": { "<label>": <count> }` when the zone carries a
  count. `Depth` is the zone's depth in its frame's tree (§4.4: a zone whose parent the frame does not hold is at
  depth 0). A complete event of process 1 whose arguments have `Depth` is a zone, one without is a frame;
- one complete event per GPU pass on process 2, on its view's thread, with `"cat": "GPU"` and
  `"args": { "Frame": <index> }`. Only durations are measured, so a frame's passes of one view are laid end to end
  from the frame's start.

`"ts"` and `"dur"` are microseconds from the recording's start: the double `nanoseconds / 1000.0`, written by
`JsonTextWriter` as the shortest text that reads back as the same double. The reader rounds `value * 1000` to the
nearest nanosecond, which gives back every value below 2^51 ns (26 days) exactly; times from 2^51 ns on are refused, and
so are frames that end there and passes laid out to end there, which the writer refuses to write. Frames are rebuilt
from the `Frame` arguments, so a zone stays in the frame that collected it. The writer writes the events frame by
frame: the frame, then its zones thread by thread in the order of the thread table, each thread's in pre-order (§4.4),
then its GPU passes. The reader keeps that order and never sorts by `ts`, since zones that begin on one clock tick share
a time: each frame's zones and passes come back in the order the file holds them.

The writer writes only the threads, names and views the frames use (the main thread always), renumbered in their
tables' order, with names made valid UTF-8 (`Utf8::Sanitize`) and cut at a character boundary to `MaxNameBytes` (thread
names `MaxThreadNameBytes`). It refuses a capture whose text would exceed `Profiler::MaxCaptureBytes` (checked after each
frame, so it never builds more than one frame past the bound), holding more than the reader accepts (counter records
included: more than `MaxCounterRecords` in a frame or `MaxCounters` in a record, a count named against the rule or twice
in a record, a view or name the tables lack), or with a frame whose zones are not grouped by thread in the thread table's
order. The views a frame's counters use are written as GPU views even when no pass of theirs was measured.

The reader refuses:

- a document larger than `Profiler::MaxCaptureBytes` (512 MiB, or a smaller bound the caller gives; `Load` checks the
  file's size before reading it), not an object (the whole text is never parsed), without the header or with one longer
  than 64 KiB, of another type or of a newer version, with `Strada` or `traceEvents` twice, or whose
  `traceEvents` is not an array;
- an event longer than 64 KiB (each event is parsed alone), of another phase than `M` and `X`, of another process than
  1 and 2, or with a missing or mistyped member;
- a negative, non-finite or too large time;
- a frame whose index does not follow the previous frame's (a capture's frames are consecutive), or not on thread 1;
- a `Frame` argument that no `Frame` event before it declares, or a `tid` that no `thread_name` before it names, a
  thread or view named out of order, and a capture that names no main thread;
- a zone whose thread comes before the thread of the zone before it in its frame (a frame's zones are grouped by
  thread in `tid` order, so that a tree builder reads each thread's as one run);
- a `Value` that is not an object of one unsigned count;
- a frame's `Counters` that is not an array of at most `Profiler::MaxCounterRecords` objects, each with a `View` that
  a `thread_name` of process 2 before it names and `Values` an object of at most `Profiler::MaxCounters` unsigned
  counts whose names are at most `Profiler::MaxCounterNameBytes` ASCII letters, digits, `_` and `.`. `Counters` is optional, so captures written
  before slice 2 read as frames without counters; the version stays 1, because a reader of version 1 ignores the
  member like any unknown one;
- more frames, zones, GPU passes, names, threads or views than a recording holds (`Profiler::MaxHistoryFrames`,
  `MaxHistoryZones`, `ProfilerCaptureSerializer::MaxGpuPasses`, `MaxCaptureNames`, `MaxRecordedThreads`,
  `MaxGpuViews`);
- a name longer than `Profiler::MaxNameBytes` (a thread's, `MaxThreadNameBytes`) or not UTF-8.

Unknown members are ignored. What the reader accepts, the writer writes back as the same frames, and that text reads and
writes again unchanged.

Visual script (`.sgraph`, `VisualScriptSerializer`; `Tests/Data/VisualScripting/Example.sgraph` is the writer's golden
file):

```json
"VisualScript": {
	"Description": "",
	"Variables": [ { "ID": 1, "Name": "Speed", "Type": "float", "Value": 4.0, "Public": true, "Tooltip": "", "Range": [0, 20], "FormerNames": [] } ],
	"Functions": [ { "ID": 2, "Name": "AddScore", "Pure": false, "Inputs": [ { "Name": "Points", "Type": "int" } ], "Outputs": [], "Locals": [], "Graph": { } } ],
	"Graph": {
		"Nodes": [ { "ID": 3, "Type": "Event.Update", "Position": [0, 0] }, { "ID": 4, "Type": "Flow.Branch", "Settings": { }, "Inputs": { "Condition": true } } ],
		"Links": [ { "From": [3, "Then"], "To": [4, "Execute"] } ],
		"Comments": [ { "ID": 5, "Text": "", "Position": [0, 0], "Size": [400, 200], "Color": [0.5, 0.5, 0.5, 0.25] } ]
	}
}
```

Variables, locals, functions, nodes and comments share one space of IDs (1 to 2147483647). Files store no pins: a link
names an output (`From`) and an input (`To`) by node and pin name, within one graph. A node without `Position` is
unplaced. `Value`, `Settings` and `Inputs` hold raw JSON (nested at most 16 levels) that only the scripting runtime
interprets. The loader checks shape only: a header other than version 1, 2 or 3 of type `VisualScript`, a content object
that is not an object, and an unknown key under `UnknownFieldPolicy::Error` refuse the file; everything else that is
damaged is repaired (an element with a bad ID, name or type is dropped, and of two with one ID the later in document
order; values of the wrong kind take their defaults; geometry is clamped) and reported as an issue with its path, such
as `Graph.Nodes[3].ID`. Files load with unknown keys as warnings and log each issue with the file; strict use
(automation input) refuses input with any issue. Names (of variables, locals, functions, parameters and former names)
are 1 to 64 bytes of UTF-8 without control characters, `.` (reserved for the fields of split pins) or spaces at either
end. Limits: 10000 nodes, 40000 links and 2000 comments per graph, 1024 variables and functions, 64 parameters per list,
256 locals per function, 16 former names per variable and per parameter. A `Range` is two floats, the minimum first; like float literals,
they are finite numbers of at most 3.4028235e38 in magnitude (float's largest value written with the fewest digits).
Floats are read as the double their text denotes, narrowed to float (`VisualScriptSerializer::ReadFloat`, and the VM
alike), and the editor and the VM write every float so that it reads back exactly: its shortest text, or its double's
text when that shortest text would not read back bit for bit, and `-0.0` for negative zero. The writer writes keys in a
fixed order, omits optional keys at their defaults and writes raw values as stored, so what loads saves and loads again
without issues. `VisualScriptSerializer`'s public `IsValidName`, `IsValidText`, `IsValidPinPath` and `IsValidValue` are
the format's rules, which the editor also applies to documents it builds in memory (where a NaN would be written as
`null` and text might not be UTF-8).

Version 2 added four optional node keys, each omitted at its default:

```json
{ "ID": 11, "Type": "Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)",
  "Split": { "Outputs": ["Hit", "Hit.Point"] }, "Comment": "Ground check", "Collapsed": true, "Enabled": "Disabled" }
```

`Split` lists the pins shown as one pin per field (`Hit.Point.X`, `Hit.Point.Y`, ...): each entry is a pin name or a
field of an entry listed before it, at most 64 per list. `Comment` is a note on the node (at most 65536 bytes),
`Collapsed` shows only its header, and `Enabled` is `"Enabled"` (the default), `"Disabled"` (the node passes the flow
and its values through, §10.6) or `"DevelopmentOnly"` (it runs in the editor and in `StradaRuntime --project`, and is
disabled in exported games). In a file of version 1 these keys are unknown keys and its nodes keep their defaults, so a
version-1 file loads unchanged and saves as version 2 differing only in its header digit
(`Tests/Data/VisualScripting/Version1/Example.sgraph` is the migration fixture); the content form that automation uses
has no version and takes every key. Repairs: a comment that is not such a string becomes `""`, a collapsed flag that is
not a bool becomes false, an unknown state becomes `Enabled`, and a split entry is dropped when it is no pin path (1 to
128 bytes without control characters, its `.`-separated parts not empty), repeats an entry, is past the 64th or names
a field of a path not kept before it.

Version 3, the version every save writes, adds two keys: the document's `Kind`, `"Script"` (the default, omitted) or
`"Library"` (a function library: functions other graphs call, §10.6), written first; and each function parameter's
`FormerNames`, its earlier names, newest last, at most 16, written last and omitted when empty, which commits record when
a library's parameter is renamed in place (§11.1), so calls in other scripts that still name the old pin keep working.
In files of versions 1 and 2 both are unknown keys, under every unknown-key policy: such files load unchanged as
scripts, and a version-2 file saves differing only in its header digit
(`Tests/Data/VisualScripting/Version2/Example.sgraph` is that migration's fixture). Engines that read version 2 refuse version-3 files rather than drop a
library's kind, which is why the version moved. Repairs: a `Kind` that is neither name makes the document a script
(`expected "Script" or "Library"; the document is a script`); a parameter's former name is dropped when it is not a
valid name, names a parameter of the same list, repeats a former name of the list or comes after the 16th; and a text
(the description, a tooltip, a node comment, a comment box's text) that is not UTF-8 becomes `""`.

Visual scripts are written as text directly. `VisualScriptSerializer::WriteText(document)` gives the file form, byte for
byte `DumpJson(Serialize(document))`, and `WriteContentText(document)` the compact content form,
`SerializeDocument(document).dump(-1, ' ', false, replace)`; neither builds a Json document. Both use `JsonTextWriter`
(`Strada/Serialization/JsonTextWriter.h`), the engine's writer of JSON text exactly as nlohmann's `dump` writes it,
pretty or compact, with `ensure_ascii` off and invalid UTF-8 replaced. `Serialize` and `SerializeDocument` stay for the
Json form (`graph.get`, `graph.find`, the clipboard, tests). Commits check a document with
`VisualScriptSerializer::Validate` and then write its text with `WriteText`: no Json is built or read back. `Validate`
applies the loader's own rules to the model, in the loader's order: the predicates `IsValidName`, `IsValidText`,
`IsValidPinPath`, `IsValidValue`, `IsValidLongText`, `IsValidID`, `IsValidPosition`, `IsValidCommentSize`, `IsValidColor`,
`IsValidRange` and `IsValidSplitPath` are the loader's rules, shared by both. It returns the first issue, named by the
loader's path (`Graph.Nodes[3].ID`); a document built in memory with text that is not UTF-8 is refused, as the writer
would replace it. Differential tests hold the writers to nlohmann (every fixture, a 2,000-node graph and a fuzz case of
damaged and in-memory documents) and `Validate` to the old round trip, which the tests keep as their oracle (a fuzz case:
`Validate` refuses exactly what does not load back as written). So a format change changes the loader, `Validate`,
`WriteText` and the differential tests together.

### 7.4 Input actions

An **input action** is a named value a game reads instead of a device: `Throttle`, `Lean`, `Jump`, `Move`. Its
**bindings** give it that value from keys, mouse buttons, gamepad buttons, axes and sticks, so a game written against
actions is played on the keyboard and on a gamepad alike, and a binding changes without a change to the game. A project
names one map of actions (`ProjectSettings::InputActions`); a player rebinds them while the game runs, kept in a binding
profile in the game's data folder ("Rebinding", below); several maps at once and map stacks are not part of this design.
`Event.KeyPressed` and the key, button and axis queries keep working beside actions, unchanged.

**Three layers, by the module layering (§4).**

- **The runtime is in Core** (`Strada/Core/InputActions.h`), beside `Input`, which it reads alone: the controls a binding
  names (`InputControl`, `ParseInputControl`, `ParseInputControls`), the compiled actions (`InputActionSet`: immutable,
  shared, with a process-unique version) and the state one consumer evaluates once per frame (`InputActionState`). Core
  can name neither an `AssetHandle` nor a `Curve` (both lie to its right), so the set it evaluates is a compiled form: the
  controls parsed and each response curve an `InputResponse` callable.
- **The asset is in Asset** (`Strada/Asset/InputActionMap.h`, the plain struct; `Strada/Asset/InputActionsAsset.h`, its
  field table, validation and registration as the data asset type `InputActions`, §6.4). `CompileInputActions` makes
  the runtime's set from a map: it parses the controls and resolves each binding's `ResponseCurve` from its Curve asset
  into a callable that evaluates a copy of the curve's keys (a curve that does not load is the straight line, logged).
  The curve is evaluated exactly, never sampled into a table, so a binding's reading is the curve's closed form.
- **The scene evaluates** (`Scene::UpdateInputActions`, the frame's one named call of input actions): each running scene
  owns an `InputActionState`, sets it at `OnRuntimeStart` from the runtime settings' asset (`SceneRuntimeSettings::
  InputActions`) and clears it at `OnRuntimeStop`.

**The asset** (`.sinput`, format in §7.3) holds `Actions`, each with a `Name`, a `Type`, a `PressThreshold` and a
`ReleaseThreshold`, and its `Bindings`, each with `Controls`, `Gamepad`, `DeadZone`, `ResponseCurve`, `Invert`, `InvertY`
and `Scale`.

- **Bindings are nested in their actions**, not a flat list of bindings that name their action. Data assets refuse an
  array whose elements are arrays (`Detail::IsDescribedFieldType` in `StructSerialization.h`; `InputActionsAsset.h`
  asserts the map passes), not an array of structs that hold an array, so an action can hold its bindings. Then no
  binding can name an action that does not exist, renaming an action is one edit, a binding's index is its place in its
  action (what rebinding and the player's profiles address, by action name and index), and the generic editor shows
  each action's bindings under it, each a list of its own with per-element undo.
- **A binding's controls are text**: one control name, or two (an axis composite: negative, then positive) or four (a
  vector composite: up, down, left, right), separated by spaces. Rows of a list are titled by their struct's first text
  field (`FieldEditor::DescribeElement`), so actions read "0: Throttle" and bindings "1: Key/A Key/D". Typed fields would
  need one enumerated field per device for each of up to four parts (`KeyCode` alone has 349 slots and no field traits)
  and would title every binding "Element n". Names are also what rebinding and profiles exchange, as engines name keys
  by text elsewhere (Unreal's key names, Unity's control paths). Letter case is ignored on reading:

| Control | Names | Reads |
|---|---|---|
| Key | `Key/<KeyCode name>`: `Key/Space`, `Key/A`, `Key/LeftShift` | 1 down, 0 up |
| Mouse button | `Mouse/<MouseButton name>`: `Mouse/Left`, `Mouse/Button4` | 1 down, 0 up |
| Gamepad button | `Gamepad/<GamepadButton name>`: `Gamepad/A`, `Gamepad/DPadUp` | 1 down, 0 up |
| Gamepad stick axis | `Gamepad/LeftX`, `Gamepad/LeftY`, `Gamepad/RightX`, `Gamepad/RightY` | -1 to 1, **up positive** (GLFW, and so `Input.GetGamepadAxis`, gives up as -1: `win32_joystick.c` negates the thumb's Y) |
| Gamepad trigger | `Gamepad/LeftTrigger`, `Gamepad/RightTrigger` | 0 released to 1 pressed (GLFW gives -1 to 1, so a released trigger would otherwise read the largest magnitude of all) |
| Gamepad stick | `Gamepad/LeftStick`, `Gamepad/RightStick` | a vector, +Y up |

- **What fits an action.** `Button` and `Axis` take one control that is not a stick, or two that are not (an axis
  composite); `Vector2` takes one stick, or four controls that are not sticks (a vector composite). An empty `Controls` is
  an unbound binding, which reads 0. `Gamepad` is -1 (any connected gamepad: the reading of the largest magnitude among
  them, and each pad's press and release counts apart, so one pad's press is judged with the others' readings) or a slot
  0 to 15; keys and mouse buttons ignore it.
- **Limits and defaults.** At most 256 actions, 16 bindings each; names non-empty, at most 64 bytes, unique in any letter
  case (queries match them with their case); `0 < ReleaseThreshold <= PressThreshold <= 1`, by default 0.5 and 0.4;
  `DeadZone` 0 to 0.99, `Scale` at least 0 (inversion is `Invert`'s). Validation (`ValidateInputActionMap`) refuses the
  rest, naming the action and the binding.

**Reading a binding**, in this order (`InputProcessing` exposes each step for the tests):

1. **The controls' value** (`ReadControls`): a single control's reading from the table, 0 for a gamepad that is not
   connected; an axis composite's positive part minus its negative part; a vector composite's (right - left, up - down).
   A composite's part reads `max(reading, 0)`, so triggers and half axes can be parts. **Every two-dimensional value is
   limited to length 1** here, a stick's as a vector composite's: GLFW clamps each axis on its own
   (`glfw-src/src/input.c:1455`), so a stick's diagonal reads up to 1.41, and a diagonal of two keys reads 0.707 each.
   One-dimensional values are -1 to 1 already. So what the dead zone and the curve take is always 0 to 1.
   - **A trigger at rest.** A trigger reads `(raw + 1) / 2`, except that a trigger that has read nothing but exactly 0
     since its pad connected reads 0 (`Input::HasGamepadAxisMoved`): a pad whose mapping lacks the trigger reads 0 there
     forever (`glfwGetGamepadState` clears the state and fills only mapped axes, `input.c:1396`), which the remap would
     make 0.5, as much as the default `PressThreshold`. A real trigger at rest reads -1 from its first poll, and an
     injected pad starts at -1 (§"Injected and polled gamepads").
2. **The dead zone**, radial on the magnitude m (the absolute value, or the vector's length): m <= d reads 0, otherwise
   the direction times (m - d) / (1 - d), so the reading still reaches 1.
3. **The response curve** on the magnitude after the dead zone, the direction (or sign) kept.
4. **Inversion**: `Invert` negates a one-dimensional value and a vector's X, `InvertY` a vector's Y.
5. **Scale** multiplies.

**An action's value** is the reading of its binding with the largest magnitude; then its override, if any (below), under
the same rule; the first in that order wins a tie. A `Button` action's value is that reading's **magnitude** (0 up: an
axis composite or a stick axis bound to a button presses it either way, and `Invert` changes nothing of a button); an
`Axis` action's is the signed reading, a `Vector2` action's the vector. Its **level** follows the magnitude with
hysteresis: an action that is up goes down when the magnitude reaches `PressThreshold`, and goes up again when it falls
below `ReleaseThreshold`. Every type has a level, so an axis pushed past its threshold is pressed too.

**Edges.** `Input` counts every button's presses and releases (`Input::ButtonCounts`), counts the input frame never
clears, and the state keeps each digital control's counts as its last evaluation read them (a gamepad button of a
binding that reads any gamepad is one control per pad). An override is one more digital control of its action (below):
it counts a press when its value leaves 0 and a release when it returns to 0 or is cleared. A control **can press** its
action when its binding, read with that control down and the binding's other controls up, reaches `PressThreshold`
(worked out once, when the state takes its set: 1 through the binding's processing); an override can when the largest
magnitude it held during its presses since the state's last evaluation reaches it. An evaluation compares the action's
previous level P with its new one D:

| P | D | Between the two evaluations | Edges, in this order |
|---|---|---|---|
| up | down | anything | pressed |
| down | up | anything | released |
| up | up | a control that can press was pressed and released (both its counts moved) | pressed, released |
| up | up | nothing of that | none |
| down | down | a control that can press was released and pressed again (both its counts moved, the control down now), and the action without that control (its binding with that control up, on that pad alone for a binding of any gamepad, its other bindings and its override) is below `ReleaseThreshold` | released, pressed |
| down | down | a control was pressed and released (both its counts moved, the control up now), and the action with that control down (its binding with that control down, its other bindings and its override) is below `ReleaseThreshold`: a composite's part that cancels the part holding the action | released, pressed |
| down | down | nothing of that | none: another binding, control or pad held the action meanwhile |

So an action has at most one press and one release per evaluation, a tap within one evaluation reports both, and a
release and press again (a double tap at 30 frames per second, an agent's release then press between two
`play.advance` calls) reports both too. Flags alone (`IsKeyPressed`) would lose a press of a scene that did not read them
in the input frame they lasted (a paused scene in the player, whose application ends the input frame every frame);
levels alone would lose a press and release within one frame.

**Counts never rewind for a running state.** `Input::Reset` (application start and shutdown, tests' input scopes) clears
every count; it also moves `Input::GetResetGeneration`, and a state that sees the generation moved takes every count as
its new baseline without an edge (the levels fall as they will, with the releases that make). The overrides' version
(`Input::GetActionOverridesVersion`) is moved by every change and never goes back, across `Reset` too.

**Where in the frame.** `Scene::OnUpdateRuntime`'s order is the runtime lane's; input actions enter it through one named
call, `UpdateInputActions`, right after `BeginTransformFrame` and before `UpdateUI`:

```
OnUpdateRuntime: BeginTransformFrame → UpdateInputActions (evaluate; the edges' events) → UpdateUI (clicks)
                 → UpdateScripts (OnUpdate) → UpdatePhysics (each fixed step: OnFixedUpdate, then physics) → ...
```

- It evaluates from the `Input` state the frame's scripts read: in the player, what the window polled at the start of
  the application frame; in the editor's play mode, the game's input frame (§11). A key set before a frame is the
  action's value in that frame: actions add no frame of latency.
- A frame that does not run evaluates nothing and loses nothing: a paused scene without queued steps returns before the
  call, and `Input`'s levels and counts keep what came meanwhile, which the next frame that runs (`play.step`,
  `play.advance`, the end of the pause) reports, once.
- At `OnRuntimeStart` (play, the player, a scene load) the state takes its set with no edge for what is already held,
  as a key held when play starts is down but not pressed, and the scene logs once how many actions it took from which
  asset. Each frame, before evaluating, the call compares one stamp of what its set was made from (the asset's handle
  and version and the data asset registry's epoch) with the current one, and compiles again when it moved (an edited
  `.sinput` or response curve reaches a running game at the next frame); the state keeps its actions' levels and latched
  edges by name, so a rebuild makes no edge. An asset that does not compile, or does not load, is logged once per stamp
  and leaves the previous set in place.
- A scene whose settings name no actions does nothing more in the call than find that out: no zone, no lookup, no walk
  of the overrides.
- Every running scene evaluates its own state from the one `Input`; scripts read the state of the scene that runs them
  (the script engine's scene context).
- The evaluation allocates nothing (the state is sized when it takes a set) and is timed by `Input.EvaluateActions`
  inside `Scene.InputActions`. Its target on the reference machine: 64 actions of three bindings each in at most
  0.05 ms a frame, judged on `Scene.InputActions` in a scene that runs no scripts (the stamp's check and the
  evaluation; such a scene dispatches no events). StradaTests' case "SceneInputActions: 64 actions of three bindings
  each, benchmark" measures it in a Release build when `STRADA_INPUT_ACTIONS_BENCHMARK=1` (it skips otherwise): 48
  Button and Axis actions (a key; a gamepad button or trigger of any pad with a dead zone and a cubic response curve; an
  axis of two keys) and 16 Vector2 actions (a stick of any pad with a dead zone and the curve; a vector of four keys; a
  stick of pad 0), with 16 injected pads, the input changed before every evaluation; it times the evaluation again with
  an override held on every action. The events' cost, which grows with the edges times the script and graph instances
  (each instance is called for each edge, whether or not it handles the action), is measured beside it by "... with
  scripts running, benchmark": the same actions, 100 graph instances and 20 C# scripts, with every input changed each
  frame and with one key a frame.

**The fixed-step rule** (the effort's rule for actions):

- In `OnUpdate`, and in everything else that runs outside the fixed steps (the action events, clicks, contact events,
  `OnDestroy`), `IsActionPressed` and `IsActionReleased` are the frame's edges.
- Every edge also increments a latch of its action. `FixedUpdateScripts` brackets each fixed step with
  `InputActionState::FixedStepScope` (also when the scene runs no scripts, so that steps take the latch all the same):
  the step gets the next **step number** (`GetFixedStepNumber`, counted from 1 for the state's life), records that
  number on each action whose latch it takes, and empties the latch.
- **While a step runs, every query reads that step's edges**, whatever started the code that asks: C# `OnFixedUpdate`,
  a graph's `Event.FixedUpdate`, a graph that one of them sent an event to or called (`SendEvent`, `Event.Send`,
  `Event.Broadcast`), a C# member a graph calls, a Timeline a step started. The state knows (`IsInFixedStep`), so no
  caller can get it wrong. Since a frame's steps run after its evaluation, each edge is seen by **exactly one** step, the
  first at or after its frame: a frame of eight steps shows it to the first alone, and frames that run no step (a frame
  shorter than a step, a time scale of 0) carry it to the next step.
- **Outside every step**, the caller says which edges (`InputActionView`), from where its code came from:
  - `Frame`: code of `OnUpdate`, an event, a click, a contact, `OnDestroy`, and everything a frame started.
  - `FixedStep` with the step's number: code that a step started and that a **debugger resume** runs between frames
    (`graph.continue`, `graph.step`, `play.pause` with `paused: false`). It reads exactly the edges of the step it
    stopped in, whichever of the frame's steps that was.
  - `NoEdges`: code that a step started and that runs later outside a step for any other reason (a latent node
    continued by the Update pass after `Delay`, a `WaitUntil` recheck, a Timeline's tick). It reads no edge: the step
    that started it saw that step's edges, and the next step sees the next ones, so reading either here would show an
    edge twice. Values and `IsActionDown` are the latest evaluation's in every view.
  - The managed side records the view and the step number on an execution when it starts (from the code it starts
    under: inside a step, the step's number; otherwise the parent execution's view), in a field of its own that a
    continuation keeps (`Continue` rewrites the execution's start as a resume, so the start cannot hold it).
- In a fixed step, values and `IsActionDown` are the frame's (the latest evaluation's). A press and release within one
  frame reach the step as both edges with the action up. Two presses before one step reach it as one press: the queries
  are booleans.
- Which step sees an edge depends on the frame it arrived in, so action edges in `OnFixedUpdate` depend on the frame
  rate, as all input does (§8, "Determinism per platform"); what is exact is the count: no edge is lost or seen twice.

**What a debugger resume sees.** While an execution is stopped the editor refuses game frames
(`PlayMode::GetFrameStepRefusal`), so no evaluation runs between frames. Code that a resume runs (`graph.continue`,
`graph.step`, `play.pause` with `paused: false`) reads the latest evaluation through its own view: code of
`OnUpdate` or an event reads the frame's edges, which its frame already showed it; code a step started reads the edges
of exactly that step, by its number, whichever of the frame's steps it stopped in (an action's step number is
overwritten only by a later step that takes a new edge of it, and no step runs while an execution is stopped). Input
given meanwhile (keys, `input.set`'s actions and gamepads) reaches actions at the next frame's evaluation only, and the
action events are dispatched by the frame's call alone, so a resume never fires one. So no action edge is seen twice,
unlike a key's: a press given while stopped is read by the resumed code and again by the next frame
(`GraphDebugCommandsTests` pins that for keys, and it stays).

**Queries.** `Strada.Input` gains `GetAction(name)` (a float: a `Vector2` action's length), `GetActionVector(name)` (a
`Button` or `Axis` action gives (value, 0)), `IsActionDown(name)`, `IsActionPressed(name)` and `IsActionReleased(name)`,
each a pure node (`[PureNode]`). They read the state of the scene that runs scripts (inside a step, that step's edges;
outside, the view of the code that asks, above), and read zero outside a running scene. A name the set lacks reads zero and is logged once per name until
the set changes; without a set (a project that names no actions, or an asset that does not load) every name does, and
the message says the project names no input actions. The editor's analysis of a graph finds such names before it runs
(§10.6, "The project's input actions in analyses"): a Warning with the nearest actions as fixes, and the names offered as a
list in Details.

**Overrides** put a value on an action as one more device would: `Input.SetActionOverride(name, value)`,
`Input.SetActionVectorOverride(name, vector)` and `Input.ClearActionOverride(name)` in scripts and graphs, `input.set`'s
`actions` in automation, `Input::SetActionOverride` in C++. They are global (`Input` holds them, so they need no running
scene), join the largest-magnitude rule after the bindings, and take effect at the next evaluation (one set in
`OnUpdate` is read from the next frame on). **They make edges as a button would**: `Input` keeps, per name, the value,
press and release counts (a press when the value leaves 0, a release when it returns to 0 or is cleared) and the largest
magnitude held during each press (`Input::ActionOverride`, `Input::ActionOverridePeaks`: a press is kept while no later
one held as much, at most 16 of them), so a value set and cleared between two evaluations reaches the next one as a tap
when the largest magnitude of the presses since that state's last evaluation reaches the action's `PressThreshold`, as
keys given to a paused game do; a strong press followed by a weak one before the next evaluation is still a press, and
every state reads the presses since its own evaluation. A cleared override keeps its record (value 0, not held) so that
states which have not evaluated since still see its counts. `Input::DropReleasedActionOverrides` drops the records not
held when no state can need them: the editor does at the end of play mode (after its scene stopped) and when it closes
or opens a project; `Input::Reset` drops every record. At most `Input::MaxActionOverrides` (1,024) names have a record at
once: a new name beyond that is refused and logged. They are held across scene loads and cleared by `ClearActionOverride`,
`Input::ReleaseInjectedInput` (`input.release`, and the end of play mode; each held one counts its release) and
`Input::Reset`. An override naming no action of the set is ignored and logged once per name; states look for such
names only when the overrides' version or their set moved, so a frame allocates nothing for them. They are how a graph
drives a game (an autopilot, an attract mode) and how a test scene presses a button; logic that must act on each fixed
step sets its targets there instead.

**Rebinding** while the game runs: a player gives an action other controls (a settings screen's "press a key for
Jump"), the game keeps them for the next run, and the actions asset is never written.

- **The table.** `InputRebinding` (`Strada/Asset/InputRebinding.h`) holds, for the process, the bindings that differ from
  the project's `InputActions` asset: each by action name and binding index, with the controls it reads now and the
  asset's controls it replaced (an `InputBindingProfile`). A scene compiles its set from the asset with the table applied
  (`InputRebinding::Apply`), and the table's version joins the stamp `UpdateInputActions` compares (beside the registry's
  epoch, the asset's version and the response curves'), so a rebind reaches every running scene at its next frame, keeps
  reaching the scenes loaded later, and makes no edge (the state keeps its actions by name). Main thread.
- **Addressing.** A rebind names an action (its exact name), a binding (its index in the action) and a part: -1 for the
  whole binding, or the index of one control of a composite (`Key/A Key/D`: 0 is A, 1 is D), which is what rebinding one
  direction of an axis or a vector needs. A part takes one control that is not a stick; the binding's new text must fit
  the action's type (`CheckInputBindingControls`). The profile stores whole bindings, so a part's rebind stores the
  binding's new text. Nothing adds or removes a binding: an empty text leaves one unbound.
- **Direct rebinding.** `Scene::RebindInput` (`Input.Rebind` in scripts) sets the binding and answers an
  `InputRebindResult` (`Strada/Core/InputCapture.h`): `Done` with the binding's new controls and its **conflicts**, the
  other actions with a binding that reads one of the same controls (the same control, or a stick and one of its own axes;
  gamepad controls only on a pad both bindings read: either reads any pad, or both the same slot), in the order of the
  actions in the map, each once. The engine applies the binding whatever it conflicts with and reports; the game decides
  (warn, swap, set it back). `Refused`, changing nothing, names the reason: no actions run (the scene does not run, the
  project names none, the asset does not load), an unknown action, a binding or part out of range, controls that do not
  parse or fit, a part below -1. A binding set back to the asset's controls leaves the profile. `Scene::ResetInputBindings`
  (`Input.ResetBindings`) returns every binding to the asset's. Refusals are answers, not logged errors.
- **Capture.** `Scene::StartInputRebind` (`Input.StartRebind`) answers `Listening` and starts an `InputControlCapture`
  (Core) that the scene advances at the start of each `UpdateInputActions`, before it compiles and evaluates, so a
  captured control is bound in the frame it comes and read by that frame's evaluation. It takes the first control given
  after the start that fits: a key, a mouse button or a gamepad button whose press count moved (one held at the start
  counts once released and pressed again), or a gamepad axis that reaches 0.5 after reading below it, read as actions read
  it (up positive; a trigger 0 at rest, also on a pad whose mapping lacks it). A whole Button or Axis binding takes any of
  these (an axis either way, as itself), a whole Vector2 binding only a stick, a part a key, a button or an axis in its
  positive direction (a part reads the positive half of its control). Gamepads count only those the binding reads. A mouse
  press while the pointer is over the game's UI (`Scene::IsPointerOverUI`, as the last UI update found it) is no control,
  then or later: the click is for the button under it (a settings screen's Cancel, whose `OnClick` calls
  `Input.CancelRebind`). In one update Escape comes first (it cancels), then keys by code, mouse buttons, gamepad buttons by pad then button, axes by
  pad then axis. `Escape` and 10 seconds (`InputControlCapture::Timeout`) of the frames' **unscaled** time cancel it, so a
  game paused by a time scale of 0 still times out; so do `CancelInputRebind` (`Input.CancelRebind`) and another start.
  The scene's stop ends it and forgets it: `Scene::GetInputRebind` (`Input.CurrentRebind`) answers `None` then, and
  otherwise `Listening`, then how it ended. A capture belongs to the scene that started it; the table it writes is the
  process's. While it listens it hides nothing from the actions: a control the capture does not take (a key while a
  Vector2 binding waits for a stick, a click over the game's UI) presses the actions that read it, so a settings screen
  pauses its own handling while the status reads `Listening`.
- **The control that ends a capture does not press.** The status reads how the capture ended before that frame's
  actions are evaluated and their events run, so no handler could tell the press that ended it from a press of its own.
  When a capture binds a control, the scene settles the action (`InputActionState::Settle`): what its digital controls
  did since the last evaluation becomes the baseline, and a reading that holds it down puts it down without a press;
  this covers a binding that already read that control, for which no new set comes. It settles every other action that
  reads the control the same way (`InputActionState::SettleControl`: the same control or a stick and one of its axes, on
  any gamepad), so a conflicting action is not pressed by it either; and when Escape cancels, it settles every action
  that reads Escape (a `Pause` action is not pressed by the Escape that cancelled a capture). Raw key queries
  (`Input.IsKeyPressed`, `Event.KeyPressed`) still see the press. When a set is replaced, an action kept by name whose bindings now read other controls (a
  rebind, or an edited asset) does the same (`InputActionState::SetActions`): its new controls' counts are baselines, and
  its current reading puts it down without a press. One that was down keeps its level, so its next evaluation reports the
  release when nothing holds it any more. The next press of the control is an edge.
- **The profile** (`InputBindingProfile`, format in §7.3) is the table on disk: `<game data>/Input/<the asset's file
  name>.json` (`Moto.sinput.json`), below the folder the `GameData` service names (§13.1): the exported game's own folder,
  `<project>/.strada/PlayData` for play mode and `StradaRuntime --project`, a temporary folder for test runs.
  - **Read when the actions load**: `RebuildInputActions` at a scene's start calls `InputRebinding::Load`, which reads the
    profile when the table holds another asset or was read with another game data folder, and keeps the table otherwise
    (a scene loaded after another keeps the player's bindings). Entries that no longer apply to the asset (§7.3's rules:
    the action or index is gone, the asset's binding no longer reads what the entry replaced, the controls no longer fit,
    a name longer than an action's or a text longer than `MaxInputBindingProfileTextLength`) are dropped with one log line
    naming the first eight and why, and counting the rest; the file keeps them until the next write. An entry that stops
    applying while the game runs (the asset edited in play mode) is dropped the same way when the set is compiled. A
    profile that cannot be read (damaged, of a newer version) is logged once and the bindings are the asset's; the next
    change writes the file again.
  - **Written atomically when a binding changes**: every `Done` that changes what differs from the asset, and a reset that
    had entries, writes the whole profile (`FileSystem::WriteTextFile`: a temporary file, then a rename), creating
    `Input/`. A rebind to the controls the binding reads already is `Done` and writes, moves and logs nothing, so a
    settings screen may apply the player's bindings every frame; a binding changed again keeps its entry's place. A write
    that fails is logged and said in the answer's `Message` ("kept for this run only: ..."); the binding holds for the run.
  - **No game data folder** (the editor while editing, tools, tests without a `GameDataScope`) means nothing is read or
    written: rebinds hold in memory for as long as the table lives. An asset that is no file (a memory or built-in asset)
    has no profile either.
  - **Who empties the table**: the editor at the end of play mode and when a project closes or opens (so the next play
    reads the profile again); tests through `InputRebinding::Clear`. The player keeps it for the process: one game per
    process.
- **Scripting.** `Strada.Input` gains `StartRebind(action, binding, part = -1)`, `Rebind(action, binding, controls, part
  = -1)`, `CancelRebind()`, `ResetBindings()`, the property `CurrentRebind` and `GetBindingControls(action, binding)` (a
  pure node: what a settings screen shows); the answers are `RebindResult` objects (`Status`, a `RebindStatus`; `Action`,
  `Binding`, `Part`, `Controls`, `Conflicts`, `Message`), whose getters are pure nodes, so a graph polls
  `CurrentRebind.Status` in its Update flow (a status is no edge: latent continuations may read it). No path is exposed.

**Events.** C# scripts override `OnActionPressed(string action)` and `OnActionReleased(string action)`; graphs have
`Event.ActionPressed` and `Event.ActionReleased`, each with a required `Action` setting (the action's name, text) and an
exec output only.

- **Dispatch.** `UpdateInputActions` dispatches the evaluation's edges (`InputActionState::GetEdges`: by action index, a
  press before a release of the same evaluation) right after evaluating, so they come before clicks and `OnUpdate`. For
  each edge, every C# script instance (in the scripts' order) gets its callback, then every graph instance gets its
  nodes of that kind and action: **C# first**, as every event reaches an entity's C# script before its graph (§10.3).
  Each edge is dispatched **once**, in the frame that evaluated it; fixed steps dispatch nothing.
- **The action's name crosses** as contacts' data does: `ScriptEngine::InputActionEventScope` holds the edge's action
  while its handlers run, and the managed side reads the name through a binding (the managed callbacks get a string; the
  `ScriptEvent` invoke itself still carries a time step and an entity).
- **Both `ScriptEvent` sides** gain `ActionPressed = 9` and `ActionReleased = 10` (`ScriptEngineInternal.h`,
  `ScriptRegistry.cs`); `ScriptEngine` and `VisualScriptEngine` gain `InvokeOnActionPressed` and
  `InvokeOnActionReleased`; `VisualScriptRuntime.Invoke` accepts events up to `ActionReleased`.
- **The two kinds** sit in `BuiltinNode` right after `EventGamepadButtonReleased` and before `EventCustom`, so
  `IsInputEvent` (whose range now ends at `EventActionReleased`) and `IsEvent` (which ends at `EventCustom`) hold: they
  are input events to the duplicate rule (two enabled nodes of one kind and one action are `DuplicateEvent`, as two of
  one key are; that rule keys on `InputEventBinding`, which carries an enumerator, so the action kinds get a binding of
  their own that carries the action's name, and `GraphRules` compares it), but they are not polled. Their `Action`
  setting is a `String` setting (`SettingKind.String`). `ProgramCompiler` compiles them into a table of (kind,
  action, node) beside the nine-slot event table, and `VisualScriptRuntime`'s `Update` keeps polling only the key,
  mouse and gamepad kinds (`IsTriggered`). The built-in kinds go from 92 to 94.

**Injected and polled gamepads.** The window writes all 16 gamepad slots on every poll (`Window::PollGamepads`, through
`Input::SetGamepadState`), so an injected pad would be wiped by the next poll of a windowed editor. `Input` therefore
keeps, per slot, the device's state and an optional injected state:

- `Input::InjectGamepadState` gives a slot a state; while the injection holds it, `SetGamepadState` (the poll) records the
  device's state aside without effect, and every reader (queries, the gamepad flags, the button counts, actions) sees
  the injected state.
- `Input::ReleaseInjectedGamepad` (or `ReleaseInjectedGamepads`) returns the slot to the device: its last polled state
  takes effect at once, with the edges that make.
- A slot's effective state changing from not connected to connected (a pad plugged in), or changing source (an
  injection beginning or ending), starts its axes as not moved (`HasGamepadAxisMoved`), which the trigger rule above
  reads: a device whose mapping lacks a trigger reads it as 0 after an injection of the slot ends, as before it began.
- `Input::ReleaseInjectedInput` (automation's `input.release`, the end of play mode) releases every injection and clears
  every override; the window losing the focus does not (it releases keys and mouse buttons, which were the window's);
  `Input::Reset` clears everything.
- The rule lives in `Input`, so a test holds an injected pad across `SetGamepadState` calls without a window, and a
  windowed test across `Window::PollEvents`.

**Automation.** `input.set` gains two parameters, both compatible (Docs/Automation.md documents them):

- `actions`: action name to a number, a bool (1 or 0), `[x, y]` (for `Vector2` actions) or null (clears the override):
  the same overrides as scripts', held until `input.release`, a null or the end of play mode. Names are checked against
  the open project's actions (the asset `ProjectSettings::InputActions` names, read from the asset manager as the scene
  reads it), and an unknown name (with its letter case), a value of the wrong shape for the action's type or one that is
  not finite as a float is refused (InvalidParams, the message listing the actions), changing nothing, as keys are. A
  project that names no actions, or names an asset that does not load as one, refuses every name, saying so
  (InvalidOperation); an empty `actions` asks for nothing. A request that would take the overrides past
  `Input::MaxActionOverrides` names is refused before it changes anything.
- `gamepads`: slot `"0"` to `"15"` (decimal, no sign, space or leading zero) to `{ "buttons": { <GamepadButton name>:
  bool }, "axes": { <GamepadAxis name>: number } }`, or null (releases the injection). The first injection of a slot
  starts it connected and at rest (buttons up, sticks 0, triggers -1: `Input`'s own convention, which
  `Input.GetGamepadAxis` reads; actions read the trigger as 0); later ones change only what they name. Axis values are
  -1 to 1 (not a number is refused).
- Every entry of both, and of the keys and mouse buttons, is checked before anything changes.
- While a game is paused, both are held and the next game frame's evaluation reads them, once, as keys given to a paused
  game reach its next frame. Code a debugger resume runs reads actions as its frame evaluated them, so an action set
  while an execution is stopped reaches the next frame alone; an injected pad's own queries (`IsGamepadButtonPressed`)
  are read by the resumed code and the next frame, as keys are. The answer reports, beside the keys, `actions` (every
  override held, scripts' too, as `[x, y]`) and `gamepads` (every injected slot: `connected`, the `buttons` down and all
  six `axes`).

**The project and exported games.** `ProjectSettings::InputActions` (an asset reference to an `InputActions` asset; a
compatible field of project format 1, edited in the Project Settings panel and through `project.settings`) names the
map, and `MakeSceneRuntimeSettings` copies it into `SceneRuntimeSettings::InputActions`. `Game.sgame` carries every
project setting and the exporter copies `Assets/` whole, so an exported game reads its project's actions.

## 8. Physics (Jolt)

`PhysicsSystem` (global) initializes Jolt: the default allocator, the type factory and a job system with a worker
thread per core but one. `PhysicsScene` is one physics world, independent of the ECS (bodies are described by
`BodyDesc` and keyed by entity UUID), so it is tested on its own. Object layers encode the project layer and whether
the body moves (static bodies never meet each other); broad-phase layers separate static from moving bodies; the
project's ignored layer pairs never collide. An entity's colliders form one compound shape scaled by the entity's
world scale (mirroring is ignored); triangle-mesh colliders on dynamic bodies use the mesh's convex hull, kinematic ones
keep the triangles (bodies that move take the `RigidBody`'s mass, which Jolt cannot compute for triangles). Friction
and restitution (combined from both sides' physics materials, below) and triggers are applied per contact by the
contact listener, so one entity can mix solid and trigger colliders and surfaces. The listener, called on Jolt's worker threads, turns sub-shape contacts into one
enter and one exit event per entity pair and contact kind; removing a body ends its contacts with exit events. Jolt
drops the contacts of bodies that fall asleep and finds them again when they wake up; such contacts persist (no exit
while asleep, no second enter on waking) and end only when the awake bodies no longer touch. Jolt is
built at the SSE4.2 baseline (its instruction-set flags reach the engine sources that include it) with its asserts,
routed to the engine log, in Debug builds only.

**Physics materials.** A physics material (`PhysicsMaterial`, `Strada/Asset/PhysicsMaterial.h`; the data of a
`.sphysmat` asset, registered as a data asset type in `Strada/Asset/PhysicsMaterialAsset.h`, §6.4 and §7.3) says how a
surface slides, bounces and rolls and what it is called. In this slice its fields do this:

| Field | Default | What it does |
|---|---|---|
| `Friction` | 0.6 | The surface's friction coefficient, combined with the other side's per contact |
| `Restitution` | 0 | Its bounciness (0 to 1), combined likewise |
| `FrictionCombine`, `RestitutionCombine` | `GeometricMean`, `Maximum` | How a contact combines the two sides' values: `GeometricMean` (the square root of the product), `Average`, `Minimum`, `Multiply` or `Maximum` |
| `RollingResistance` | 0 | A rolling resistance coefficient (force per newton of load) for scripts, graphs and, later, the vehicle; rigid-body contacts ignore it |
| `Surface` | empty | A name scripts and graphs compare (`"Asphalt"`, `"Gravel"`) |
| `ImpactSound`, `ImpactParticles` | none | An `AudioClip` and a `Prefab` (a particle system) for impacts. A collision that begins plays each side's `ImpactSound` where it begins when the project's mixer names a bus for impact sounds (`Mixer::Impacts`, §9.8; none by default); scripts and graphs read both and spawn the particles, which contacts never spawn |

Every collider component has a `Material` reference. Unset, the collider's own `Friction` and `Restitution` apply with
the modes `GeometricMean` and `Maximum`, which is how every collider combined before materials existed; set, the
material's values and modes replace them, and the inspector shows the two fields disabled, saying that `Material`
sets them (the `OverriddenBy` field hint, §7.2). A reference to no registered physics material (an unknown handle, a
missing file, an asset of another type, all of which files and `component.set` accept like any asset reference) counts
as unset, and the inspector then shows the two fields again; so does a registered file that does not load, with a
warning when the body is made (the inspector reads `AssetManager::HasLoadFailure` and loads nothing: it shows the floats
once a load has failed). A `MeshCollider` with triangles
(`Convex` off, on a static or kinematic body) also has `Materials`, one per submesh of its mesh in submesh order; an
empty entry, or a submesh past the list, uses the collider's `Material` (or its floats). Jolt's mesh shape holds at most
32 materials (`MeshShape::FLAGS_MATERIAL_BITS`, refused beyond at `MeshShape.cpp:161-163` of the pinned Jolt), so the
list holds at most 31 (`MaxMeshColliderMaterials`, the field's `MaxCount`) beside the collider's own. Convex hulls,
and the hulls that stand in for the triangles of dynamic bodies, have one surface: the collider's `Material`.

A triangle mesh's shape is built over the mesh's distinct positions (`CreateMeshShape` in `PhysicsScene.cpp`): the
importer gives each submesh vertices of its own, and splits vertices along hard edges and texture seams, but Jolt tells
the edges between triangles apart by their shared vertex indices (`MeshShape::sFindActiveEdges`, `MeshShape.cpp:299-336`
of the pinned Jolt): an edge that one triangle alone holds is an active one, and a body crossing it meets the edge
itself. Where two surfaces of a road meet, that was a bump: a ball crossing coplanar strips at 4 m/s leapt and lost an
eighth of its speed, a box sliding corner first at 8 m/s rose 0.32 m (`PhysicsScene:` case "bodies slide and roll across
the edges ..."). Welding by exact position shares those edges, which Jolt then judges by the angle between their
triangles as it judges any other. A vertex is welded only to one at its position whose triangles face the same way (the
sums of their triangles' normals less than 90 degrees apart): the back of a double-sided face repeats the front's
positions with triangles turned the other way, and welding the two would give every inner edge four triangles, which
Jolt also takes for an active edge (`MeshShape.cpp:316-322`: three or more triangles on an edge), so each face keeps its
own vertices and its edges shared by two (case "a double-faced triangle mesh keeps each face's edges to itself ..."). A
mesh without repeated positions builds the same shape as before; one with them now has fewer active edges, and a body
that touched one of those edges now slides or rolls over it.

A contact combines, per pair of sub-shapes as before, the two sides' friction and restitution
(`CombinePhysicsMaterialValues`): each side brings its value and its mode, and when the modes differ the one later in
the order `GeometricMean`, `Average`, `Minimum`, `Multiply`, `Maximum` wins (so a material that says `Maximum` for
restitution bounces off everything, and one that says `GeometricMean` defers to any other). The rule is symmetric in
the two sides, as Jolt's pairs come in body order. With both modes at their defaults it is exactly `std::sqrt(a * b)`
and `std::max(a, b)`, the expressions of the listener before materials, so a scene without materials simulates bit for
bit as it did and its state hashes do not change, with three exceptions: a character's inner body touching a body of
several colliders through one that is not its first now meets that collider's values, where before it met the first
collider's (below); a triangle mesh whose positions repeat is now welded, so a body that met one of its open edges
crosses it (above); and a body two or more levels below a root, whose world matrix the transform cache composes top
down and may give in different last bits than slice 1's walk did (§7.1), so its first sync can differ. An exact tie of
`Raycast` now goes to the smaller UUID ("Queries" below): that changes a query's answer, not the simulation. No
sample's pinned checks changed through any of them. A side is one sub-shape: a collider of a compound body, or a
triangle's submesh. Its surface is found from the sub-shape ID: `Shape::GetLeafShape`, which compound and decorated
shapes pass on to the leaf (`CompoundShape.cpp:136`, `DecoratedShape.h:52` of the pinned Jolt), gives the leaf shape, whose
user data names its collider's entry in the table below, as before materials; on a triangle mesh with submesh
materials the rest of the ID is the triangle, whose Jolt material index (`MeshShape::GetMaterialIndex`) is 0 for the
collider's own surface and i for the i-th submesh's. Such a mesh's shape carries a material list
(`MeshShapeSettings::mMaterials`) only so that the indices are valid: its entries are Jolt's default material, and the
engine's table says what each index means; a mesh without submesh materials has no list and its triangles index 0,
exactly as before. A side without a collider entry (a character's inner body) brings its body's own friction and
restitution (Jolt's defaults, 0.2 and 0) with the default modes, which is the rule Jolt's own default combination
applies to two bodies. The other side brings the surface of the sub-shape it touches. Before materials it brought its
body's body-level values, which were its first collider's, so a character that touched any other collider of a
compound body met the wrong collider's friction and restitution; that is the first of the exceptions above.
The rigid bodies' body-level values are no longer taken from their first collider, and nothing reads them. Jolt merges the
contacts of neighbouring sub-shapes with similar normals into one manifold (manifold reduction, on by default), and a
manifold names one sub-shape of each side: a body that touches two surfaces at once in one such manifold (a box
straddling the edge between two submeshes, or two coplanar colliders) takes the values of one of them, in practice the
one its contact started on, until it touches only the other.

The listener runs on Jolt's worker threads. The values it reads live in the world's table of collider materials
(`PhysicsScene`: per collider, its trigger flag and its surface, `ColliderSurface` in `PhysicsTypes.h` with friction,
restitution, both modes and the material's handle, and for a triangle mesh the surfaces of its submeshes), which only
the main thread writes, between steps (bodies made, rebuilt and removed, and material edits), and which no step
resizes: inside `PhysicsSystem::Update` it is immutable, so the workers read it without a lock and see one value per
step. The scene resolves a collider's surface when it makes the body (`ColliderDesc::Surface` and `SubmeshSurfaces`;
trailing submesh entries that do not apply are left out, and `AddBody` refuses more than `MaxMeshColliderMaterials`
with an error of its own). A material edited while a scene plays (the inspector, `physics-material.set`, undo, a
reload of its file) reaches the bodies that use it before the frame's next step: the scene counts the bodies naming
each material and keeps the version (`DataAsset::GetVersion`) of the material whose values they took (0 when it
applied none), and when the data asset registry's epoch has moved, before it next rebuilds changed bodies (which it
does before the frame's steps and before each step), it gives every material whose version changed its new values
(`PhysicsScene::SetMaterialValues`, zone `Scene.SyncPhysicsMaterials`). No body is rebuilt, so velocities, contacts and
joints stay; contacts that persist take the new values at the next step (Jolt calls `OnContactPersisted` every step).
A sleeping body has no contacts to take them (Jolt drops them until it wakes), so when an edit changes a body's surface
values, `SetMaterialValues` wakes the body and, for a static one, every non-static body whose bounds meet its own
(widened by Jolt's speculative contact distance), in entity order: a box asleep on a slope slides at the next step after
its material, or the slope's, loses its grip. An edit that changes no value (a `Surface` text, a rolling resistance, a
`set` of the same values) wakes nothing. A material that starts or stops applying (its file breaks and is reloaded or
refreshed, or loads again, or the asset is dropped) makes the bodies naming it again, which keep their velocities: what
is not a loadable physics material counts as unset. A refresh of the asset directory that retries a failed load or
finds a missing file again moves the data asset epoch (§6.4), so a material fixed on disk while the scene plays applies
before the next step, for its bodies and for scripts alike. A static body rebuilt so is added asleep, as every static
body is, and wakes nothing that sleeps on it.
Materials are inputs of the simulation, like the project's settings: the state hash and snapshots do not hold them.

`RaycastHit::Material` (and `Strada.RaycastHit.Material`) is the handle of the material of the sub-shape or triangle a
ray hits, found the same way, and none when that collider has no material.

Scripts and graphs read a material through the managed class `Strada.PhysicsMaterial` (§10.4), on §6.4's managed
pattern: its members are read-only properties (so pure nodes), answered from the runtime's copy of the values
(`PhysicsMaterialCache` in `Interop/DataAssets.cs`, one per asset handle and shared by every reference), which
`PhysicsMaterial_Read(handle, heldVersion, values, surface)` fills in bulk, writing only the version when the runtime
holds the asset's current one; the `Surface` text crosses as a pointer into the asset's data and a length, copied at
once. The values are one immutable object per version read (`PhysicsMaterialValues`, holding the `Surface` string and
the references of `ImpactSound` and `ImpactParticles`), so reading a member allocates nothing. A material that cannot
be read has a new material's values, logged once until data assets change again. A raycast hit carries the material's
handle across the boundary (`NativeRaycastHit::Material`, after an explicit `Reserved` word so neither side pads), and
the managed hit takes the cache's shared reference of that material (`PhysicsMaterialCache.GetShared`), so a hit on a
material allocates nothing once that material has been met, which the casts of later tracks rely on per step. Scripts
cannot change a material (the project's materials are files the editor owns; there is no runtime copy as there is for
render materials). The managed collider components have no `Material` property: a script finds a surface's material
through a raycast.

The scene drives it while running. `OnRuntimeStart(SceneRuntimeSettings)` (from `MakeSceneRuntimeSettings(project
settings)`) creates bodies for entities with colliders (static without a `RigidBody`; a `RigidBody` without colliders
is not simulated). EnTT signals rebuild a body when its physics components, or the mesh of its mesh collider, change;
the rebuilt body keeps its velocities. Physics runs in fixed steps: before each step kinematic bodies move towards their
transforms (`MoveKinematic`) and static or dynamic bodies whose transforms changed (scripts, the editor, a moving
parent) are teleported; after it, awake dynamic bodies write their world transforms back (parents first, converted to
parent-local space, keeping their scale). Events delivered to scripts: `OnCollisionEnter/Exit(Entity other)`,
`OnTriggerEnter/Exit(Entity other)`, with `Strada.Collision` for the rest of the event (below). Queries (below): the
closest solid hit of a ray, every hit of a ray, sphere, box and capsule casts and overlaps, each with a layer mask.

**Contact data.** An enter event (`ContactEvent`, `Physics/PhysicsTypes.h`) carries where and how its contact began:
up to `MaxContactPoints` (four, as many as Jolt keeps in a manifold its step finds: `PhysicsSystem.cpp:1238-1239` prunes
it to four before reporting it) `ContactPoint`s, each with a position, a normal, an impulse, a relative velocity and the
physics materials of both sides. They are stored as the event's `First` sees them; `ReverseContactPoint` gives
`Second`'s view (the normal and the relative velocity reversed, the materials exchanged). Exits carry none: Jolt reports
no points when contacts end.

- *The point* is halfway between the two surfaces' points of Jolt's manifold (`mRelativeContactPointsOn1` and `On2`
  around `mBaseOffset`), where Jolt's solver applies its impulse. A contact Jolt finds before the bodies touch (its
  speculative contacts, closer than `mSpeculativeContactDistance`, 2 cm) has it in the gap; one found after a fast body
  went in during its last step has it inside both.
- *The normal* has unit length and points from the other body towards this one: the way this one is pushed. Jolt's
  manifold normal points from its first body to its second (the way to move the second out, `ContactListener.h:30` of
  the pinned Jolt 5.6.0), so `First`'s points hold it reversed.
- *The impulse* is estimated, because Jolt 5.6.0 reports none: when it adds a contact its solver has not run
  (`ContactListener.h:107-109`), the impulses it solves stay in its `ContactConstraintManager`, which `PhysicsSystem`
  keeps private (`PhysicsSystem.h:373`), and no callback follows the solve. The listener calls
  `EstimateCollisionResponse` (`Jolt/Physics/Collision/EstimateCollisionResponse.h:30-42`), which Jolt provides for this
  callback: a sequential-impulse solve of the two bodies alone, from their velocities as the contact begins (the step's
  gravity applied, which Jolt does before it looks for contacts, nothing solved yet; for a body with `ContinuousCollision`
  whose sweep finds the contact, which Jolt reports from its continuous collision job after the velocity solve
  (`PhysicsSystem.cpp:2079`, called from `JobPostIntegrateVelocity`), the velocities that solve left, and a manifold
  that Jolt has not pruned to four points), with the pair's combined friction
  and restitution, Jolt's restitution threshold (`mMinVelocityForRestitution`, 1 m/s) and the world solver's iterations
  (`mNumVelocitySteps`, 10). The figure is the impulse along the normal at that point, in newton seconds, that stops the
  approach and, with restitution, sends the bodies apart again: what the collision takes in all, also when Jolt's solver
  spreads it over two steps (a speculative contact first lets the bodies close the gap). A 2 kg ball that comes in at
  6 m/s on static ground reports 12 N s, and 18 N s with a restitution of 0.5. It knows nothing of other bodies in the
  same collision (a body landing on a stack), nor of the pair's other manifolds: each manifold is estimated as if it
  alone stopped the pair. A contact that begins on several manifolds in one step therefore reports impulses that are not
  shared out between them: for a body that comes down evenly on two, too large when their normals are less than 90
  degrees apart, too small when more, exact at right angles. A 2 kg ball landing at speed v in a groove whose faces rise at 30 degrees reports m v cos 30 on each
  face, where the two solved together take m v / (2 cos 30) each (1.5 times less); at 60 degrees it reports m v cos 60,
  where they take m v each (twice as much). A joint solve was left out: it needs every candidate point of the pair's
  manifolds and the bodies' velocities from before the step's solve (a swept contact arrives after it), which the
  listener would have to store as they grow, where the fixed points allocate nothing. A trigger contact, and a pair without a dynamic body (a character's inner
  body on static ground), report 0: Jolt gives them no response.
- *The relative velocity* is this body's velocity at the point minus the other's, at the same moment as the impulse's:
  the bodies approach when it points against the normal.
- *The materials* are the `ColliderSurface::Material` of the sub-shape on each side (`FindSubShapeMaterial`, above):
  none for a collider's own values and for a character's inner body. A manifold names one sub-shape per side, so its
  points name the surfaces its contact started on (manifold reduction, above). A contact that begins on several
  manifolds in one step (a ball landing in a groove of two faces, a box across two colliders at an angle) gives the
  event the points of each, each with its own normal and materials, the four strongest kept.
- *The order* is strongest first: by impulse from the largest, then by position, normal and relative velocity (each by
  x, y and z) from the smallest, then by the materials' UUIDs (`IsContactPointBefore`, `PhysicsTypes.h`). The key is
  every value of a point, so only equal points tie, and the order is a function of the values rather than of Jolt's
  order of points. The first point is the strongest.
- *Taking them* happens in `OnContactAdded`, on Jolt's worker threads, under the collector's lock, and only for the
  manifolds of a contact that begins (each pair's state remembers its enter event while the step runs). The estimate
  works on the stack and the points go into the event's fixed array, so taking points allocates nothing; the pairs'
  bookkeeping and the event list allocate as they grow, as they did before points existed, and the list keeps its
  capacity from step to step (the main thread takes a copy).

Every event carries the world step it happened in (`ContactEvent::Step`, `PhysicsScene::GetStepCount`): the events the
step's contacts make have its number, and the exits of a body removed between steps the number of the step before. They
reach the scene in step order: when a step ends its events are sorted by `First`, then `Second`, then type (they are
simultaneous, and Jolt's threads find them in any order), and a removal's exits are sorted among themselves before the
next step's events. A scene's world starts with the scene and steps once in each fixed step, so an event's step is the
scene's fixed step count as that step ends (`Scene::GetFixedStepCount`); snapshots carry the count.

The scene dispatches the events **once per frame, after the frame's steps**, in that order, each to both entities. They
are not dispatched per step, because handlers run where `OnUpdate` runs: they read and move entities as the frame shows
them (drawn poses, below), destroy entities (deferred to the frame's end), play sounds and spawn effects. Inside the
steps they would run user code between Jolt's step and the scene's write-back, and a handler would run zero to
`MaxStepsPerFrame` times a frame depending on the frame rate. A frame's handlers see what its steps did, and each event
says which step it came from. Logic that must react inside a step (a vehicle's tyres, a sensor) asks the world from
`OnFixedUpdate` with queries rather than waiting for events; this is why contact events stay outside the determinism
promise below, although their content (entities, steps, points) is the same on every run.

**Contacts in scripts and graphs.** The handlers keep their signatures (the other entity); the rest of the event is
read through `Strada.Collision` while they run. `Scene::DispatchContactEventsToScripts` holds a
`ScriptEngine::ContactEventScope` around each entity's two invocations (its C# script, then its graph), which makes the
event, seen from that entity (`HandledContactEvent`: the event and whether the entity is its `Second`, whose points
`ReverseContactPoint` gives), the one `ScriptEngine::GetHandledContactEvent` returns; scopes nest, each restoring the one
before, and outside them it is null. One binding, `Collision_Read`, copies the handled event's entities, step and points
(`NativeCollision`, 32 bytes, and up to four `NativeContactPoint`s of 56, neither padded implicitly) for
`Collision.Entity`, `Other`, `Step` and `Points`, which read as empty outside a handler. `Points` makes a new array of
`ContactPoint`s on each read (materials through `PhysicsMaterialCache.GetShared`, so points on one material share one
reference); the managed `ContactPoint` is a struct of public fields, so graphs Make and Break it.
`Event.CollisionEnter` gives, after `Other`, the strongest point's `Point`, `Normal` and `Impulse` and the `Surface` of
its other side's material ("" for none): the outputs follow `Other`, and stored graphs name pins by text, so graphs
saved before them load unchanged. The runtime reads them (`IVisualScriptEnvironment.TryGetContactPoint`, no array) only
for a graph that has the node, as it looks up `Other`. A graph's flow reads `Strada.Collision` until its first wait: a
fiber that resumes later after a delay runs outside the handler, while the event node's outputs keep the values they were
given. A flow the debugger stopped is not waiting: like `Time.DeltaTime`, the stopped execution saves a copy of the
contact it read (`IVisualScriptEnvironment.SaveContact`, a `HandledContact`, only when it stops inside a handler), and
`ContinueStopped` makes `Collision` read that copy while the chain continues (`RestoreContact`), so a stop changes
nothing the flow computes.

**Queries.** `PhysicsScene` answers four queries (`Physics/PhysicsScene.h`, types in `Physics/PhysicsTypes.h`), and a
running `Scene` the same four:

| Query | Finds | Order |
|---|---|---|
| `Raycast` | the closest solid collider along a ray | of entities at exactly the same distance, the one of the smaller UUID (`RaycastAll`'s first hit) |
| `RaycastAll` | every entity along a ray, at its nearest hit | distance, then entity UUID |
| `ShapeCast` | the first entity a sphere, box or capsule (`PhysicsQueryShape`) touches as it moves along a line without turning | the first by distance, then entity UUID |
| `Overlap` | every entity with a collider such a shape overlaps where it is | entity UUID |

- *Mask and triggers.* The three new queries take a `PhysicsQueryFilter`: a 32-bit layer mask (bit i sees the bodies
  and characters on project layer i; a project has at most 16 layers, so bits 16 to 31 see nothing, and a mask of 0
  sees nothing) and whether trigger colliders count. By default they do not, and a query passes through them as
  `Raycast` does. The rule is per collider: a shape filter on each sub-shape (`SolidShapeFilter`, which characters'
  sweeps use too; Jolt asks it for every sub-shape before testing it, `CollisionDispatch.h:38` and `:58`), so a trigger
  collider of an entity with solid ones is skipped alone, and a trigger is never tested at all, so it cannot hide what
  lies behind it.
- *One hit per entity, so the order is total.* An entity's colliders form one body, and a query reports each body at
  most once: `RaycastAll` and `ShapeCast` its nearest hit over its colliders and, on a triangle mesh, its triangles (of
  hits at the same distance, the one of the smaller Jolt sub-shape ID; for a cast that starts overlapping several of its
  colliders, the deepest overlap), `Overlap` the entity if any of its colliders overlaps. Hits are ordered by distance
  from the nearest and then by entity UUID from the smallest (`IsQueryHitBefore`); overlaps by UUID. No two hits share
  an entity, so two different hits never tie, and the order depends neither on the order Jolt visits bodies in nor on
  the order the entities were made in. `ShapeCast` returns the first hit of that order, so it never depends on which
  of two entities at the same distance Jolt finds first.
- *What a hit carries.* `RaycastHit` (entity, point, the surface normal there, distance along the ray, material) and
  `ShapeCastHit`: the entity; the point where the shape touches the collider, on that collider's surface;
  the unit normal out of that collider towards the shape (Jolt's penetration axis reversed: the face's normal on a face,
  from the edge or corner towards the shape's center on one, so a sphere over an edge gets the direction it would roll
  off); the distance the shape travelled (its center is then at origin + direction · distance); and the physics
  material of the surface hit, found from the sub-shape as for rays and contacts (`FindSubShapeMaterial`): a submesh's
  on a triangle mesh with submesh materials, none for a collider's own values and for a character's inner body.
  Overlaps report entities only.
- *Casts.* A shape cast is Jolt's `NarrowPhaseQuery::CastShape` on exact shapes: `mUseShrunkenShapeAndConvexRadius` is
  off and the query's box has no convex radius, so a box keeps its sharp edges and corners (in Jolt's `Default`
  support mode a box is the full box, `BoxShape.cpp:111-118`). A collider the shape already overlaps where it starts is
  touched at distance 0 at the deepest point of the overlap, whichever way the cast goes: `mReturnDeepestPoint` makes
  Jolt run EPA where GJK's points at fraction 0 cannot be trusted (`EPAPenetrationDepth.h:535-546`), and
  `mBackFaceModeConvex` is `CollideWithBackFaces`, which skips the test that drops a cast moving out of what it starts
  in (`ConvexShape.cpp:284-285`). So a shape cast from where a wheel or a foot rests on the ground finds the ground at
  distance 0. That holds for the caller's own colliders too, and `PhysicsQueryFilter` excludes no entity: a cast meant
  to find what is below or ahead of a body (a wheel's under a chassis, a foot's under a character) starts outside that
  body's colliders, or leaves the body's layer out of its mask. This is unlike rays, which pass out of a convex
  collider they start inside: a ray has no volume to be touching with. Triangles are one-sided, as for
  rays (`mBackFaceModeTriangles` is `IgnoreBackFaces`, `CastConvexVsTriangles.cpp:41-42`, `CastSphereVsTriangles.cpp:133-135`):
  a cast passes a triangle it reaches from behind and leaves one it moves away from. On the inner edges of a mesh the
  normal is the face's where that hinders the movement less (`mActiveEdgeMovementDirection` is the cast's direction).
- *Overlaps.* `NarrowPhaseQuery::CollideShape`, with triangles from either side (`mBackFaceMode` is
  `CollideWithBackFaces`): a shape that meets a mesh's surface overlaps it. A triangle mesh is its surface, so a shape
  wholly inside a closed mesh does not overlap it; convex colliders are solid. Jolt's collide test runs GJK on shapes
  shrunk by their convex radius and adds the radius back (`ConvexShape::sCollideConvexVsConvex`, `ConvexShape.cpp:86-93`),
  so an overlap sees a box collider with its edges and corners rounded by its convex radius (at most 5 cm,
  `cDefaultConvexRadius`), where a cast sees it sharp: a shape that only grazes a box's edge or corner may be touched
  by a cast at distance 0 and not overlap it.
- *Limits.* A query whose origin or center is not finite or lies beyond `MaxPhysicsCoordinate`, whose direction is zero
  or not finite, whose distance is not finite and positive, whose rotation is not finite, or whose shape has a size that
  is negative or not finite, a radius beyond `MaxPhysicsQueryRadius` (100 m) or a half extent or half height beyond
  `MaxPhysicsQueryExtent` (20 m) (or an unknown shape type) finds nothing. Those sizes are far below a collider's
  (`MaxPhysicsExtent`) because a cast runs Jolt's GJK on the moving shape's core (a box whole, a capsule's segment, a
  sphere's center) and adds its radius after, and longer cores and wider capsules lose its precision. Measured against
  closed forms over 2,000 random poses of each shape, near the origin and kilometers from it: at the limits a cast is
  within 0.8 mm of where it touches (`PhysicsQueryTests.cpp` holds it to 5 mm, and checks that a 20 m box cast among
  boxes names the first it reaches); cores of 50 m miss by up to 4 mm, of 100 m and capsules of radius 1,000 m by up to
  3 cm, and from 300 m a cast touches bodies it passes by, at distance 0 among them. A size below a
  millimeter counts as a millimeter and a capsule shorter than that is a sphere, as for colliders; a rotation is
  normalized (zero is the identity); a direction is normalized without overflowing; a distance beyond
  `MaxPhysicsQueryDistance` (40,000 km, farther than any two points of the world are apart) is cut to it. So nothing a
  script or an agent sends reaches a Jolt assertion. `Raycast` keeps its own checks.
- *Which world, from where.* Queries are main-thread calls between steps; they change nothing (the simulation, its
  state hash and its contacts are the same with and without them) and read the bodies and characters as they are in
  the world, and the material table, which no step changes. Jolt calls a collector's `OnBody` with the body locked and
  releases the lock before the narrow phase that reports the body's hits (`NarrowPhaseQuery.cpp:121-124` and `:351-354`
  of the pinned Jolt), so the collectors take the entity, the shape and the inverse transform there and never touch the
  body afterwards. `Scene::Raycast`, `RaycastAll`, `ShapeCast` and `Overlap` first apply physics changes not applied yet
  (`ApplyPhysicsChanges`, as the raycast binding does), so a body made or rebuilt since the last step is found where its
  entity is. Every other body is found where the last step left it, which is where `OnFixedUpdate` reads it: an entity
  moved since by a script, the editor or its parent is found where it was until the next step takes its body there. In
  `OnUpdate` and contact handlers, bodies drawn between their last two steps (below) are found up to a step ahead of
  where they are drawn. A game that decides from queries in `OnFixedUpdate` therefore stays deterministic per platform
  at any frame rate (below). A scene that runs no physics finds nothing.
- *Many hits.* `RaycastAll` and `Overlap` replace the contents of the caller's vector with every hit, in order, keeping
  its capacity, and return the count; a caller with a fixed buffer takes the first ones of the order.
- *From scripts.* `Strada.Physics` gives the queries per shape (§10.4): the scripting bindings call the `Scene` forms and
  copy the first of the order into the script's buffer. StradaTests' `ScenePhysicsQueryTests.cpp` checks that scripts
  get exactly what the scene's queries give, and that hovering balls whose forces their `OnFixedUpdate` queries decide
  reach the same state hash, and the same query results, at 30, 60 and 144 frames per second.

**Step control.** A step lasts the project's `Physics.FixedTimestep` (0.0005 to 0.1 s, default 1/60); a frame runs at
most `Physics.MaxStepsPerFrame` of them (1 to 64, default 8; a slower frame drops the backlog beyond one step, so the
game slows down instead of falling further behind), and Jolt simulates each in `Physics.CollisionSteps` collision steps
(1 to 4, default 1: sub-steps for fast or stiff bodies, at their cost). All three are compatible fields of the project
file (`SceneRuntimeSettings` carries them to the scene). The scene counts the fixed steps it has run since it started,
with or without physics (`Scene::GetFixedStepCount`). Physics components changed in `OnFixedUpdate` reach the world
before the step that follows, as those changed between frames reach it before the frame's first step.

**What scripts read between steps.** `OnFixedUpdate` runs before each step and reads every simulated entity where the
last step left it. `OnUpdate` and the renderer read what the frame shows: characters, and dynamic and kinematic bodies
whose `RigidBody.Interpolation` is `Interpolate` (the default, so scenes that do not store the field are drawn so), are
drawn between their last two steps, the part of the way from the one before to the last one that the time left over
(the accumulator, as a fraction of a step) makes up. They move smoothly at any frame rate, a step behind the
simulation: at a frame rate equal to the step rate the time left over is about zero, so a frame shows the step before
the last one. `None` draws a body where its last step left it, so it moves in steps when frames come faster than
steps. An entity a script or the editor moves between steps stays where it was put, and the next step takes its body
or character there; a body so moved is drawn on its way from there, never from where it was before (a teleport does not
smear).

Bodies are drawn in their entities' local space: after each step the scene records each moving body's local
translation and rotation, and draws between the last two (positions along the line, rotations by the shorter slerp), so
what is below a body follows it and a body below another is drawn as it is at the steps. A kinematic body is drawn
moving only when the fixed steps move it (`OnFixedUpdate` setting its transform or calling `MoveKinematic`, as a lift
does); one moved by `OnUpdate` or the editor is shown exactly where it was put, and its next step takes it there over the
step, as `MoveKinematic` does. A kinematic body has no teleport: `Teleport` moves it there over the next step, and it is
drawn so. A body that does not move (asleep, or awake and still) is drawn exactly where it is, bit for bit, whatever the
frame. A character standing on a body drawn between its steps is drawn the whole way between its own steps, so it stays
on it; on ground drawn where its steps leave it, only the way it went by itself is drawn.

Before the physics changes and steps of each frame, the translation and the rotation of every entity drawn between
steps go back, each on its own, to exactly the local value its last step left when nothing changed them since the
frame drew them (and characters go back to their step positions), so the steps see exactly what they would have seen
without drawing: the simulation, and its state hash, are the same with `Interpolate` and with `None`. A part of a pose
set between frames is kept and the other part goes back: a script that sets only a falling body's rotation every frame
(keeping it upright, facing a target) leaves its fall to the steps, and the steps take the body to its step translation
with the new rotation, as they do with `None`; in frames without a step the part nothing set is still drawn between
the steps. A script's call that applies physics changes (a body made or rebuilt, whose joints are made again) does the
same first, so bodies and joints are made from where the simulation has them. Between frames (in `OnUpdate`, in contact
events and `OnDestroy`, which come after the frame's steps, and in the editor) the entities are then drawn again as far
as the frame drew them, so the frame shows them where it did and scripts read them there; inside the steps they stay at
the steps. `RigidBodyComponent.Interpolation` changes how a body is drawn without rebuilding it when a script sets it;
the editor's edits of a running scene rebuild the body, as for every RigidBody field.

**Determinism per platform.** The same binary on the same machine gives the same simulation for the same inputs,
whatever the frame rate: the state after step n depends on the scene, on what scripts do in their fixed steps and on n,
not on how the steps were spread over frames. Jolt is deterministic when it is called in the same order (Jolt's
`Docs/Architecture.md`, "Deterministic Simulation"), and the scene calls it in an order that depends neither on the
frame rate nor on the order entities were made in: bodies, characters and joints are made, rebuilt and removed in the
order of their entities' UUIDs, the syncs before each step (kinematic targets, teleports, characters moved by edits)
run in that order, and the world's own tables are ordered by entity; contact events are sorted per step and awake
bodies are read back sorted. Entities made while a scene runs (by scripts, prefab instances, duplicates) take their
UUIDs from a sequence the scene seeds from its entities' UUIDs as it starts, so every run makes the same IDs and their
bodies take the same places in that order. Scripts run in their components' order (§10), so their calls reach Jolt in
the same order on every run. Before the steps of a frame, characters go back exactly where their last step left them
and after each step their entities are placed exactly where it left them, so what is drawn between frames never
reaches the simulation.

It does not promise equality across platforms, compilers or configurations: Jolt is built without its
`CROSS_PLATFORM_DETERMINISTIC` option (with MSVC's `/fp:fast`), engine code is compiled with each compiler's default
floating-point contraction (arm64 fuses multiply-adds), and Debug and Release differ. Nor does it cover what happens per
frame rather than per step: `OnUpdate`, input read in it, input actions' edges in `OnFixedUpdate` (each reaches exactly
one step, but which one depends on the frame it arrived in, §7.4), contact events (they reach scripts once per frame, after the
frame's steps), destruction (deferred to the end of the frame), changes of the time scale, editor edits and scene loads.
Two things read in `OnFixedUpdate` itself depend on the frame or the run: `Time.Elapsed` and `Time.FrameCount` count
the frames' time and frames, not the steps', and `Strada.Random` starts from a different seed on every run. A game whose
physics must not depend on the frame rate acts on the simulation from `OnFixedUpdate` only, counts its time there from
the `fixedDeltaTime` each call receives, and calls `Random.Seed` before it draws numbers that reach the simulation.

**The state hash.** `Scene::ComputePhysicsStateHash` is a 64-bit FNV-1a hash (`HashBuilder`, over the bit patterns of
the values, little-endian) of the fixed step count, then of every body in UUID order (its UUID, motion type, position,
rotation, linear and angular velocity and whether it is awake, as Jolt holds them) and every character in UUID order
(position, velocity, its own and walking velocity, ground state, ground entity and displacement, and the step positions
it is drawn between). Equal hashes at one step count mean equal simulations. The accumulator is left out: at one step
count it holds a different remainder at each frame rate, which is what the hash must not see; snapshots carry it. With
`SceneRuntimeSettings::PhysicsHashStep` set to n, the scene records its hash right after step n, inside the frame that
runs it (`Scene::GetRecordedPhysicsHash`); the player's test summary prints it (`--hash-step`), or the hash after the
run's last step. The count, the hash and the recorded step are the running scene's: a scene that loads another starts
it at step 0, so a test run that moves on to another scene reports the hash and steps of the scene it ends in.

**Snapshots.** `Scene::SavePhysicsState` keeps a running scene's simulation in memory: everything Jolt's state recorder
saves (`PhysicsSystem::SaveState` in full: the last step's length and gravity, the bodies' positions, rotations,
velocities, forces and sleep state, the contacts and the joints' warm starts), each character's state
(`CharacterVirtual::SaveState`, which the system does not save, with the engine's own velocities and ground
displacement), the contacts the world has reported and the events not yet taken, and the scene's step state: the step
count, the accumulator, the state of the sequence entities made while running take their UUIDs from, the poses last
exchanged with the bodies, the local poses moving bodies are drawn between (and which parts of each are drawn, or
whether it was moved since its step), and the characters' step positions and ground. Entities made after a restore so
take the UUIDs entities made after the save took, except one an entity made since (and not destroyed) still holds,
which is skipped.
`Scene::RestorePhysicsState` puts it all back and places the entities of bodies and characters where they were then, so
the steps that follow run as they did from there (given the same fixed-step inputs). Its limit: a snapshot belongs to
the world it was saved from and to the bodies, joints and characters that world had. Once one was added, removed or
rebuilt (a physics component changed), restoring is refused and changes nothing, because Jolt's state recorder neither
adds nor removes bodies. Nor does a snapshot hold what the simulation does not change: settings scripts or components set
since (gravity factors, damping, layers, what characters ignore), the scripts' own state, entities without bodies, sounds
and particles.

Joints (`JointDesc`, `PhysicsScene::AddJoint`) are Jolt constraints made in world space from where the bodies are:
fixed, hinge (with limits within ±180° and a velocity motor with a torque limit), point (`Ball`), distance (a range;
negative ends keep the starting distance) and slider (limits around the starting position, velocity motor with a force
limit). Values that are not finite, missing bodies, joints to the entity itself and hinges or sliders without an axis
are refused; removing a body removes the joints on either side of it. The scene makes every `Joint` component's joint
when it starts and makes them all again whenever a joint component changes or a body is made, rebuilt or removed
(rebuilt bodies lose their joints; changing a character does not touch them); a joint whose connected entity no longer
exists is left out without a warning (a broken chain), while one whose connected entity has no body is logged.
Duplicates and prefab instances remap `ConnectedEntity`. The scene view draws a selected joint's anchor, axis and
connection.

Characters (`CharacterDesc`, `PhysicsScene::AddCharacter`) are Jolt `CharacterVirtual`s: upright capsules (a sphere when
the height is below twice the radius) that games move instead of the simulation. An entity has a body or a character;
adding one replaces the other. Each step moves the characters first, in the order of their entities' IDs, then simulates
the bodies. A character has a velocity of its own: on walkable ground (`MaxSlopeAngle`) its feet stop it and the ground
carries it at the ground's velocity of this step (`UpdateGroundVelocity` first: scripts set kinematic targets before the
step, so a platform that starts, stops or turns back carries it without a slip), gravity (times `GravityFactor`) only
presses it against the ground, so it does not creep down slopes; elsewhere gravity speeds it up, so it falls and slides
down slopes too steep to stand on. An upward velocity of its own jumps (it keeps the ground's velocity); after the step,
the part of it that moved into what the character hit is removed (landing, ceilings, walls). After the bodies' step, a
character on ground that went down (a descending platform or elevator, which was still in its way when it moved) follows
it down (`StickToFloor` over the ground's drop), so it neither hovers above it nor loses it. `MoveCharacter` adds a
walking velocity for the next step only. `ExtendedUpdate` slides it along what it hits, cancels walking up steep slopes,
climbs steps up to `StepHeight` and keeps it on the ground over drops up to `StepHeight` (walking down stairs and
slopes); its rounded bottom rolls over edges, which are too steep to stand on for a step or two. It pushes the dynamic
bodies it walks into with at most `PushForce` newtons and weighs `Mass` on what it stands on. A kinematic inner body,
larger by the 2 cm the character keeps from what it touches, gives it presence: raycasts hit it, dynamic bodies land on
it, other characters are blocked by it, and it reports contacts with static and kinematic bodies like kinematic bodies
do, so triggers notice it and its scripts get collision and trigger callbacks. Trigger colliders never block characters
(a shape filter skips them), and neither do the bodies of the entities a character ignores
(`SetCharacterIgnoredEntities`, its own parts: a body filter lets its sweeps pass through them, and the contact listener
rejects their contacts with its inner body in `OnContactValidate`, so they neither push it nor report touching it).
Reshaping or moving a character keeps its velocity and refreshes what it stands on, and so does removing or rebuilding
the body it stands on (rather than standing on, and riding, a body that is gone until its next step); refused
descriptions (values that are not finite, capsules larger than the world allows) leave the entity without one; speeds
are kept within 500 m/s.

The scene makes a character for each `CharacterController` when it starts, places its capsule at the entity's world
position plus `Offset` (both scaled with the entity, the radius by the larger horizontal scale; the capsule does not
turn with the entity, which scripts rotate freely), and changes it in place when the component or the entity's scale
changes; the warning about the colliders and `RigidBody` it replaces is logged once, when the character is made.
Changing the component changes only the character, while adding or removing it replaces the entity's body (which makes
the joints again). The entities below a character are its parts: the scene has the physics ignore their bodies, and
follows the hierarchy as it changes. Before each step, an entity moved by a script, the editor or its parent teleports
its character (keeping its velocity); after it, moved characters write their positions back (parents first). The
simulation moves entities without taking their transforms apart: characters change only the translation, dynamic bodies
the translation and rotation, so scales (and characters' rotations) stay exactly as they were. Between frames,
characters are drawn between their last two steps, so they (and cameras on them) move smoothly whatever the frame rate:
before a frame's physics changes and steps, entities not moved since they were drawn go back to where the last step left
them (`OnFixedUpdate`, their parts' bodies and component changes see the physics positions; a component changed between
steps keeps the character where it is), and after the steps they are drawn as far into the last step as the time left
over makes up (accumulator / step, so a step behind). The way their ground carried them in the last step
(`CharacterState::GroundDisplacement`, as far as they went that way) is not delayed, so riders stay on platforms, which
are drawn where the last step left them. Entities moved between steps stay where they were put, and the next step
teleports their characters there. Removing the component or destroying the entity removes the character.
`Scene::TeleportCharacter` places the entity and stops its character at once. The scene view draws a selected
character's capsule (with the colliders).

Jolt computes in single precision and asserts on (or, in Release, is corrupted by) values its math overflows, so
`PhysicsScene` keeps everything within limits (`Physics/PhysicsTypes.h`): positions within 10,000 km of the origin
(bodies beyond stay at the edge), colliders and their offsets up to 1,000 km (larger bodies are not simulated, with a
warning), masses within 1e-4 to 1e12 kg, gravity up to 1e6 m/s², gravity factors within ±1000, forces, torques and
impulses up to 1e9, and velocities, initial or set, within Jolt's limits (500 m/s, about 47 rad/s); values that are not
finite are ignored. Scene files and scripts cannot break the simulation that way.

## 9. Audio (miniaudio)

`AudioEngine` wraps `ma_engine` (miniaudio; its resource manager, encoders and generators are compiled out). Ogg Vorbis
is decoded by libvorbis through miniaudio's libvorbis decoding backend, not by the stb_vorbis copy miniaudio ships,
which frees uninitialized pointers when the headers of a damaged file fail to parse. libogg and libvorbis are compiled
from their pinned sources (`StradaVorbis`). Its output is the default playback device (miniaudio falls back to
its null backend, which consumes audio in real time, when no device works), the null device (headless applications),
or manual: the mix advances only when `AudioEngine::ReadFrames` pulls it, which makes audio tests deterministic. macOS
links Core Audio at build time (`MA_NO_RUNTIME_LINKING`), as notarization requires; the Linux backends (PulseAudio,
ALSA, JACK) are loaded at runtime and are not build requirements. `STRADA_AUDIO_OUTPUT` chooses a windowed
application's output (`device`, the default, or `null`, so that a capture or a test session plays nothing through the
machine's speakers); a headless application stays on the null device whatever it says.

`AudioClipAsset`s hold the encoded file (WAV, FLAC, MP3, Ogg Vorbis; the format is detected from the data).
`ComputeAudioWaveform` decodes a clip with a decoder of its own (the `AudioDecoder` of its format, as sounds do), so it
runs on any thread and can be cancelled, into the peaks of equally long parts (blocks merge as the clip decodes, so
memory stays bounded for any length); the editor's content browser draws them. Every `AudioDecoder` is miniaudio's
decoder of the detected format for WAV, FLAC and MP3 and libvorbis for Ogg Vorbis, so no other decoder ever reads the
data (left to itself, miniaudio would search a whole file for an MP3 frame).

**The mixer** (slice 3's tracks MX1 and MX2). §9.1 to §9.9 are the design of the mix: MX1 builds the queue, the voices,
the buses and the effects, and MX2 adds what §9.8 lists on the same structures. The interfaces are declared in
`Strada/Audio/AudioTypes.h` (the voices' limits), `AudioQueue.h`, `AudioVoice.h`, `AudioMixer.h` and `AudioEffects.h`
(internal to the module), `AudioEngine.h` and `AudioScene.h` (the public API), and `Strada/Asset/Mixer.h` with
`MixerAsset.h` (the `.smixer` data and its field tables).

### 9.1 Threads and ownership

- **The main thread** runs the scenes and every `AudioScene` and `AudioEngine` call. It creates and destroys every object
  the mix is made of: voices (an `ma_sound` each), the scenes' groups (an `ma_sound_group` each), the buses' nodes and
  their effects. Each is made unattached (`MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT`, `miniaudio.h:11130`, for sounds and
  groups) and through the engine's allocation callbacks (`ma_engine_config::allocationCallbacks`, which every sound
  init inherits, `:78433`; the bus nodes pass them to `ma_node_init`). Its side of the mix is `AudioVoiceManager`
  (`AudioVoice.h`): the commands it sends, the table of every voice of every scene, and the objects it lent.
- **The mixing thread** is miniaudio's device thread for device and null output (the engine's data callback,
  `MixIntoDevice`, which the engine starts only once the mixer exists: `ma_engine_config::noAutoStart`) and the caller
  of `AudioEngine::ReadFrames` for manual output, which is the main thread. Both run the same code,
  `AudioMixer::Render`, so a test hears exactly what a device would. It renders in blocks of at most `AudioBlockFrames`
  (256 frames, 5.3 ms at 48 kHz); each block it (1) applies every command the queue holds (§9.2), (2) steps the
  pitches that move, (3) renders the block with `ma_engine_read_pcm_frames` and applies the master volume, (4) reports
  the plays that reached their end (it checks `ma_sound_at_end` after the block: miniaudio's own end callback comes only
  at the sound's next pass, `:76878`), finishes the fades that are over and moves on the virtual voices' time, (5) writes
  the block's counts, and pushes what it lets go onto the return list. It never allocates (§9.9 counts it), never locks,
  never waits and never logs. Its clock is its own count of the frames it rendered, which fades and virtual voices use:
  miniaudio's graph time stands still while nothing is attached to it. On a device the graph renders in the device's
  period (`:77620`), so a command reaches the mix within a period rather than a block.
- **The audio streaming thread** and the editor's waveform worker are unchanged: the streaming thread fills streamed
  sounds' rings, which the mixing thread only copies from (§9.4).
- **One owner at a time.** An object becomes the mixing thread's with the command that hands it over (a voice's first
  `StartVoice`, `AttachGroup`, `AddBus`, `SetBusEffects`, `AddVoiceSends`, whose send node comes back with its voice)
  and the main thread's again when it comes back on the return
  list; nothing of Strada's calls miniaudio on it from another thread in between. Only the mixing thread attaches and
  detaches nodes, between blocks. miniaudio's graph reads without locking (`:74198-74202`); attaching and detaching take
  two spinlocks (`:74017`, `:74086`) and wait for readers of the bus to leave (`:74051-74059`). With one thread doing
  both, the locks are never contended and the waits end at once, which is what lets the mixing thread call them.
- **The rule of the last command.** The mixing thread returns an object only in answer to the last command the main
  thread sends that names it, and the main thread names it in no command after that one, so no command waiting in the
  ring or in the deferred commands ever names an object that came back. The last command of a voice is a
  `RetireVoice` (its source or scene went, or the main thread read that its one-shot ended) or the `StealVoice` of a
  one-shot; that of a group is `RetireGroup`. A one-shot that ends is therefore not returned by the mixing thread on its
  own: it reports the end (an atomic of the voice), stays attached and silent, and comes back once the main thread has
  read the end and retired it. A group comes back only once no voice the mixing thread holds is attached to it: the
  stolen and retired voices still fading out in it come back first. The last command of a bus is `RemoveBus`, which
  is also that of the chains it holds (its current one and one sent but not yet swapped in; a bus's first chain travels
  with its `AddBus`); a chain replaced by `SetBusEffects` comes back when it is swapped out, or at once when a newer one
  replaces it before it ran. A removed bus comes back only once nothing feeds it: the mixing thread itself moves to
  Master, with a dip, whatever still plays into it, the groups of retired scenes among them, since the main thread may
  name those no more. The main thread keeps every lent object in one table (`AudioVoiceManager`), so whatever comes back
  has one owner to go to; it is destroyed there.
- **References.** The mixing thread holds raw pointers only. Every reference (`Ref`) that keeps something alive (a
  voice's samples and stream, a chain's effects) is held on the main thread's side of the object, so the last
  reference is never dropped, and nothing freed, on the mixing thread.
- **Who frees what, and when.** An `AudioScene` owns its sources' voices, its own voices and its groups; `AudioEngine`
  owns the bus nodes, the effect chains and the table of objects being returned. Destroying something the mixer holds
  is its last command plus a move of its `Scope` into that table; it is destroyed when it comes back.
  - A scene that ends while others play retires its voices, then its groups. Heard voices fade out first (§9.3), so a
    scene's voices and groups come back a few milliseconds after the scene; voices it never handed over are destroyed
    at once. Other scenes' voices and groups are untouched.
  - The main thread takes the return list and reads the ends the mixing thread reported (retiring the ended one-shots)
    in `AudioEngine::Update` (once a frame, zone `Audio.Update`), at the end of every `ReadFrames`, and before a start
    finds a place in a pool. With manual output and no read after a scene ends, its objects wait in the table until the
    next read, `Update` or `Shutdown`.
  - `Shutdown` stops the device first (`ma_device_stop`, `:44339`, waits for the device's thread on synchronous backends,
    `:44398`, and Core Audio's stop waits for its callback, `:36463-36464`); with no mixing thread
    left, it destroys the table's objects and everything else (each sound and node detaches itself and its inputs,
    `ma_node_detach_full`, `:74790-74828`, waiting for nobody), then uninitializes the engine.
  - **A device that stops** (lost or unplugged) would hold everything it was lent and let the queue grow. The engine
    passes miniaudio a notification callback (`ma_engine_config::notificationCallback`, `:11291`); a `stopped`
    notification (`:6779`, sent from the device's thread, `:20220`) that the engine did not cause marks the device lost.
    Since a backend whose stop fails on an unplug sends none (`:42843`), a playback device (never the null device) that
    has mixed no block for `AudioMixerStallSeconds` (5 s) counts as lost too. `Update` then stops the device (which
    waits for a callback in progress), logs once, and takes the mixer over: each frame the main thread applies the queue
    and finishes every fade at once (`AudioMixer::Drain`), rendering nothing, and every `AudioDeviceRetrySeconds` (1 s)
    it tries to start the device again, handing the mixer back once it starts. `AudioEngine::SimulateDeviceLoss` stops
    the device as a loss would, which is how a test drives the same path.
- **Profiling.** `Audio.Mix` around each render (on the device's thread through the engine's own
  `ProfilerThreadState`, §4.4; on the caller's thread with manual output, once per `ReadFrames`); `Audio.Update` around
  the main thread's frame work. The render opens no zone of its own.

### 9.2 The audio queue

- **Every change to what is mixed is a command** (`AudioCommand`, `AudioQueue.h`): a voice started (played, started
  virtual, or resumed from virtual, `AudioStartFlags`), paused, stopped, stolen, retired or moved to another group; its
  volume, pitch, looping, spatialization, distances and position; a scene's groups paused together, a group moved to
  another bus or retired; a bus added, removed or re-parented; a bus's values (the mixer's and the game's) and its
  effects; an effect's parameter; the master volume; and, with MX2, a bus's ducking and a voice's sends to reverb zones
  (a snapshot transition is the bus and effect values it changes, each carrying the transition's time; doppler travels
  as the voice's pitch). A command is 48
  bytes of plain data (the type, flags, an effect's index and parameter, two object pointers, four values and a
  serial).
- **The ring** (`AudioCommandQueue`) is a single-producer, single-consumer ring of 4,096 commands (192 KiB) from the
  main thread to the mixing thread. The mixing thread applies every command the ring holds at the start of each block,
  in order. Every `ReadFrames` and every device callback starts a block, so a change made before a read is heard from
  that read's first frame (ramped as below).
- **A full ring defers, it never drops.** A command that finds the ring full, or finds commands already deferred,
  waits in order on the main thread's side (an allocation of the main thread), and reaches the ring when there is room:
  every push flushes first, `Update` flushes once a frame, and `ReadFrames` flushes before each block, so a burst
  reaches the mix in order over the blocks that follow. `GetDeferredCommandCount` says how many wait.
- **Objects travel by pointer** in the commands that hand them over, and come back on the return list
  (`AudioReturnList`): an intrusive, lock-free stack the main thread takes whole, oldest first. Each object carries its
  own link, so handing back never allocates and the list never fills.
- **What is smoothed, and over what.** Gains ramp linearly in amplitude over `AudioGainSmoothingSeconds` (5 ms): a
  voice's volume (miniaudio's volume smoothing, `ma_sound_config::volumeSmoothTimeInPCMFrames`, `:11222`), a bus's
  volumes and mutes, the master volume, the fade of a retired voice, the fades of moves and swaps (§9.5), and MX2's
  sends. A voice's first volume reaches miniaudio with its first start, which miniaudio's gainer takes at once (it
  smooths every value after the first, `:51010-51016`), so a voice starts at its own value however often it changed
  before. Pitch moves over `AudioParameterSmoothingSeconds` (20 ms) on a log2 scale, stepped once a block to the value
  of the block's end: miniaudio does not smooth pitch, and its resampler changes ratio at the block's edge without
  breaking the waveform. A pitch set while the voice is not heard is taken at once. Effect parameters move over the
  same 20 ms on their scales, the filters' coefficients every 32 frames (§9.6). Spatial gains keep miniaudio's own
  smoothing (8 ms, `:77699-77706`). Pause, Stop and a paused scene still cut at the block's edge, as they always did
  (the tests pin exact silence after a pause); stealing, retiring and moves fade instead. 5 ms keeps every existing
  test's assertion: a volume halved before a 0.1 s read adds 0.6 % of the step to the read's mean, and a source removed
  at 0.5 before a 0.2 s read of silence adds 0.006, inside their tolerance (doctest's `Approx` allows
  `epsilon * (1 + value)`, `doctest.h:6555-6556`).
- **Read-backs need no block.** The main thread keeps a shadow of every value it sets (a voice's volume, pitch, looping
  and play state, a bus's game values, the master volume) and answers getters from it at once, as the tests ask
  (`Play` then `IsPlaying`, `SetVolume` then `GetVolume`, with no read in between). The mixing thread reports back only
  through atomics: a play's end (the serial of the play that ended), the voices mixed at the last block, the blocks
  rendered, and the meters.

### 9.3 Voices

- **A voice** (`AudioVoice`) is one playing instance of a clip: an `ma_sound` that reads its clip's shared samples
  (§9.4), or a streamed source's stream, through miniaudio's resampler (pitch, and the clip's own sample rate), its
  attenuation and its panning. Sounds are positioned relative to their scene's own listener, as before: spatial
  voices are attenuated by inverse distance (full volume within `MinDistance`, `MinDistance / distance` up to
  `MaxDistance`, constant beyond) and panned by miniaudio's model (its stereo speakers face left and right, so a sound
  in front reaches each at half gain). A voice sends a position only when it changed.
- **A source's pool.** An `AudioSourceComponent` owns at most `MaxVoices` voices at once (default 4, 1 to 64), its main
  voice among them while it plays or is paused. Its main voice is made with the source and keeps today's meaning:
  `Play` starts it, resumes it, or starts it over after its end; `Pause` holds it; `Stop` rewinds it. `PlayOneShot`
  (the source's clip or another, with a volume) starts another voice that never loops and follows the source as it
  changes: its position, its volume (times the one-shot's own), pitch, spatialization, distances, bus and priority.
  `Stop` fades the source's one-shots out; `Pause` holds only the main voice. A full pool steals its oldest one-shot; a
  pool whose voices are all its main voice (`MaxVoices` 1) refuses the one-shot. A pool made smaller steals its oldest
  one-shots down to it. A one-shot that ended frees its place before the next one starts: every start first retires
  the one-shots whose end was reported, without waiting for a frame. A streamed source has no loaded clip: its
  one-shots name one, and `Scene::PlayAudioOneShot` loads the component's clip for one that names none. A source whose
  component names no clip has no sound until its first one-shot of a clip given, which makes it a pool with no main
  voice (`AudioScene::AddVoicePool`) that follows the component like any sound.
- **The scene's voices.** `PlayClipAtPoint(clip, position, volume, bus)` plays on a voice of the scene rather than of
  an entity: spatial, at a fixed position, with the default distances and priority, never looping. It is held while
  the scene is paused, fades out with the scene's end, and outlives the entity that played it, which is what a sound
  that must outlive its entity (a break, a death) uses: a source removed, or an entity destroyed, fades its voices out
  with it.
- **The engine's limit.** At most `MaxMixedAudioVoices` (64) voices are mixed at once across every scene: those
  playing, not paused, in a scene that is not paused. The budget (§9.9) is stated for 32 and also measured at 64. When
  a start (a `Play`, a one-shot, a scene unpaused) would pass the limit, the voices that would then mix are ranked by
  priority (highest kept), then by how loud they are at their listener (loudest kept), then by age (newest kept), and
  those that rank beyond the limit are stolen: a one-shot fades out and is returned, a main voice becomes virtual. The
  new voice itself is stolen when it ranks lowest: a one-shot is refused (`PlayOneShot` answers false), a main voice
  starts virtual. How loud a voice is, is the main thread's estimate: its volume, times its distance attenuation (the
  same inverse law), times the mixer's and the game's gains of its bus and the buses above it, in amplitude (a snapshot's
  values counted as soon as a transition to it starts); effects and ducking are left out.
- **Virtual voices.** A main voice stolen by the limit, or started beyond it, keeps its play state (`IsPlaying` stays
  true, as for a script it still plays) and its place in time: the mixing thread stops it and counts its time on in
  its own clock, moving its cursor by the frames played at its pitch and the clip's rate when it is heard again (a
  looping clip wraps; a clip that does not loop reports its end when its time passes it). At each `Update` while
  virtual voices exist, and at every start, the ranking runs again: a virtual voice that ranks within the limit is
  heard again where its time has taken it, faded in over `AudioStealFadeSeconds`, and the one it displaces becomes
  virtual or is returned. A virtual voice ranks as if it were a third quieter (it comes back only when clearly louder
  than what mixes), so two voices of nearly one loudness do not trade places at every update. Only the limit makes a
  voice virtual: engine layers at volume 0 in a crossfade are mixed, and count in the budget, while the voices number
  at most `MaxMixedAudioVoices`; beyond it, being the quietest, they are the ones made virtual, cost nothing, and are
  heard as soon as their volume rises above what mixes. A pitch set for a virtual voice counts from then on: the time
  it spent at its earlier pitch moves its cursor first. A streamed main voice
  holds its place while virtual (its file is read ahead from where it stopped). A virtual voice in a scene that pauses
  stops its time there and waits for the scene like its other voices.
- **One authority.** The main thread decides every start and every steal, from its shadow; the mixing thread decides
  only that a play reached its clip's end (or that a virtual voice's time passed it), and reports it. So the counts are
  exact: `GetStolenVoiceCount` counts every steal (by a pool or by the limit, a one-shot refused and a main voice made
  virtual included); `GetActiveVoiceCount` is the main thread's count of the voices mixing in scenes that are not
  paused (not virtual, not stolen), exact right after a start, which counts an ended play out at the next `Update` or
  `ReadFrames` that reads it, so it never depends on when the mixing thread ran; `GetMixedVoiceCount` is the mixing
  thread's count of the voices it mixed in its last block (MX2's meters).
- **Stealing fades.** A stolen voice fades out over `AudioStealFadeSeconds` (10 ms, miniaudio's fader,
  `ma_sound_set_fade_in_pcm_frames` from its current volume, ended on the mixer's clock); a one-shot is then returned,
  and a main voice becomes virtual where the fade ended. It is mixed while it fades, so for those 10 ms up to as many
  voices as were stolen sound beyond the limit. A retired voice that is heard fades out over
  `AudioGainSmoothingSeconds` (5 ms) and is returned; one that is not heard (stopped, ended, virtual, or in a paused
  group) is returned at once. A steal or a retire that meets the play's end in the same block returns the voice once.
- **Moves.** A source whose `Bus` changes moves its voices to its scene's group of the new bus (`MoveVoice`): faded out,
  moved and faded in over 5 ms each when heard, moved at once when not.
- **Streamed sources.** A streamed source's main voice plays its `AudioStream` as before. The stream opens the clip's
  file when the source is created, on the main thread, reading only its format and headers (at most
  `MaxAudioStreamHeaderBytes`: damaged files whose audio would start later are refused); the streaming thread
  (`AudioStreamer`, owned by `AudioEngine`) then keeps it decoded about a second ahead (`AudioStreamBufferSeconds`), and
  the mixing thread only copies decoded samples and never waits (a read that stalls silences that one voice for a
  moment, and it continues where it was). The streaming thread decodes on past the end of the file from its start and
  marks where the file ended, where the mixer decides whether the voice loops, so loops are seamless and a voice played
  again after its end starts at once; `Stop` seeks the stream from the main thread (`AudioStream::Seek` is safe from
  any thread), which empties its buffer and decodes the start again. Files that are missing or not audio are refused
  when the source is created; damaged ones play what can be decoded. `AudioEngineSettings::StreamFileOpener` opens the
  files and `AudioEngine::WaitForStreams` lets manual output hear streams without gaps, as before.

### 9.4 Clips: how samples reach voices, and what they cost

- **A loaded clip is decoded once per clip asset object**, on the main thread, the first time a source or a voice
  needs it (or when `AudioEngine::PreloadClip` asks), into an `AudioSampleBuffer`: 32-bit float frames in the clip's
  own channels and sample rate, immutable, shared by every voice of the clip in every scene. `AudioSampleCache` (the
  engine's) holds a clip's buffer for as long as the clip asset object lives, keyed weakly on that object: a one-shot
  played again after the last one ended decodes nothing, a reloaded clip is a new object and decodes again, and an
  entry goes with its clip (the cache prunes at `Update`, and while it grows). A clip that cannot be decoded keeps its
  error, so it is neither decoded again nor logged again at each play. The Asset module knows nothing of it.
  `GetDecodedClipCount` counts the decodes, for tests. A system that plays clips as one-shots (MX2's impact sounds) calls
  `PreloadClip` for them when its scene starts, so that no first play decodes in the middle of a game.
- **Each voice reads the buffer from a cursor of its own** through a data source of Strada's that reports the clip's
  own sample rate. (`ma_audio_buffer_ref` reports a rate of 0, `:60065`, which an engine node takes for the engine's
  own, `:77111`, so a 44.1 kHz clip would play fast at 48 kHz.) Reading copies; no decoder runs on the mixing thread.
  Before the mixer, every sound decoded its clip on the mixing thread with a decoder of its own, and libvorbis
  allocates while it decodes.
- **Memory**: a loaded clip costs its frames times its channels times 4 bytes, once, however many voices play it, for
  as long as its asset is loaded (one second of 48 kHz stereo is 375 KiB: twice a 16-bit WAV, and 10 to 20 times a
  compressed clip), beside the encoded file the asset holds as before. A voice adds its `ma_sound` (a few KiB of node
  state) and nothing per sample. The largest clip in the repository's samples, a 1.4 MB 16-bit WAV, decodes to about
  2.8 MB.
- **The size above which a clip must stream** is `MaxDecodedClipBytes`, 32 MiB (87 s of 48 kHz stereo, 175 s of mono).
  A clip whose length the decoder reports (WAV, FLAC, Ogg Vorbis) is refused before decoding when that length passes
  it; any clip is refused when its decoded samples pass it (an MP3's length shows only as it decodes). Nothing is sized
  from a stated length, which a damaged file can state as anything. The error says to stream the clip (set `Stream`);
  a clip that exists only in memory has no file to stream from and cannot play beyond the limit. A clip that stops
  decoding part way keeps what decoded, as a damaged clip always played what could be decoded.
- **What decoding costs.** A clip decodes on the main thread when it is first needed, which is a pause at a scene's
  start for a long clip. Measured by the benchmark case of §9.9 (`AudioBenchmark: decoding a loaded clip ...`, Release,
  the reference machine, the median of five decodes each, 2026-11-17, provisional while another lane ran its tests):
  a 16-bit mono 48 kHz WAV 0.19 ms per second of audio, a mono FLAC (the tests' encoder, verbatim subframes, so a fast
  case) 0.72 ms, a silent 128 kbit/s MP3 0.68 ms (a lower bound: music decodes slower), and miniaudio's 35 s stereo
  48 kHz Ogg Vorbis test file 2.24 ms. So a 10 s Ogg effect decodes in about 22 ms, a frame and a half at 60 Hz: what
  `PreloadClip` at a scene's start is for. Long clips (music, ambience) stream, and their main voice needs no decoded
  buffer.

### 9.5 Buses, the scenes' groups, and the `Mixer` asset

- **The graph.** A voice plays into its scene's group of its bus (`AudioSceneGroup`, an `ma_sound_group` made with
  `MA_SOUND_FLAG_NO_PITCH` and `MA_SOUND_FLAG_NO_SPATIALIZATION`, which only sums), the group into its bus's node
  (`AudioBusNode`), each bus's node into its parent's, and Master's into the engine's endpoint. Master's node exists
  from `Init` on (the mixer every engine starts with is Master alone), so there is one path whatever the project names.
  A bus node is a custom node of miniaudio's graph with one input, into which miniaudio sums the bus's groups and child
  buses, and one output, made with `MA_NODE_FLAG_CONTINUOUS_PROCESSING`: miniaudio otherwise skips a node whose input
  reads nothing (`:75293-75325`), which would cut a reverb's or a delay's tail when the bus's last voice ends or its
  scene pauses, freeze the dynamics and the ramps, and leave the meters at their last value. So every bus processes in
  every block, and the budget counts it. Without input it is given silence. It is not made with
  `MA_NODE_FLAG_ALLOW_NULL_INPUT`, which would pass it no input at all: after such a block miniaudio leaves the block's
  frames counted as cached input (the null-input path skips its counters, `:75361-75392`), so the next block would read
  stale silence instead of the graph, and a sound starting on a silent bus would be heard a block late. Its process function runs, in order: the bus's effects (§9.6), the mixer's gain (the asset's volume and mute,
  blended by MX2's snapshots), the game's gain, MX2's ducking and the dip of a move or a swap, each ramped; then it
  measures the block's peak and RMS, silence included. The master volume is applied by the mixer to the rendered
  block, after Master.
- **Buses are global, groups are per scene.** One node per bus name serves every scene. A running scene makes its group
  of a bus name when its first voice is made for it, hands it over with that voice's first start, and keeps it until it
  ends. Groups are keyed by the name their sources ask for (letters folded: bus names match ignoring letter case, as
  their uniqueness does), attached to that bus, or to Master while the mixer lacks it. A paused scene stops its groups
  all at once (`SetScenePaused`, one command for every group of the scene, so that a voice's sends to reverb zones,
  which play into other groups of its scene, stop and start with it, §9.8), which holds its own voices and no other
  scene's; any number of scenes play at once, four as
  easily as one. A paused scene's voices do not count against the limit; unpausing checks it.
- **The `Mixer` asset** (`.smixer`, the data asset type `Mixer`, version 1, §6.4 and §7.3): `Buses`, at most 32, each
  with `Name` (unique ignoring letter case, not empty, at most 64 bytes), `Parent` (empty for Master), `VolumeDb`
  (-80 dB, which is silence, to +24 dB), `Muted` and `Effects` (at most 8). Exactly one bus is `Master`, with no
  parent; every other bus's parent is a bus of the list, and following parents from any bus reaches Master
  (`ValidateMixer` refuses anything else, naming the first bus at fault). One line in
  `DataAssetRegistry::AddEngineTypes` registers it, which gives the inspector, the content browser's New item, the
  registry-wide fuzz case, `mixer.create`, `mixer.get` and `mixer.set` (an undo step, as every data asset's set) and the
  MCP tools `mixer_create`, `mixer_get` and `mixer_set`. A new mixer is Master alone.
- **The project's mixer.** `ProjectSettings::Mixer` is the handle of the project's `.smixer` (a compatible field of the
  project format: `"Mixer": "<handle>"`, beside the project's `InputActions`; settable through `project.settings` and
  the Project Settings panel; 0 for none). `MakeSceneRuntimeSettings` hands it on as `SceneRuntimeSettings::Mixer`.
  `Game.sgame` carries it as it carries every project setting, and an export copies `Assets/` whole, so an exported game
  mixes through its project's mixer. A project without one mixes through Master alone, which sounds exactly as the
  engine did before buses: Master at 0 dB without effects is the identity.
- **Applying it.** `SceneRunner` (the player, the editor's play mode and test runs) applies the project's mixer when a
  game with sound starts, logging `Mixer: <n> buses from <reference> (<names>)`, and in every update after it, paused or
  not (the editor updates a paused game every frame; a debugger stop holds the updates, so an edit made then is mixed
  when the game resumes), whenever the data asset registry's epoch moved and the asset's version changed (an edit through the inspector or
  `mixer.set`, its undo). A game without a mixer, or whose mixer does not load as a `Mixer` asset (logged once, at its
  start), mixes through Master alone; a mixer that stops loading later leaves the mix as it was. It calls
  `AudioEngine::ResetGameValues` when a game starts and stops, and when its game stops `AudioEngine::ClearEffectState`
  clears every effect's state under a dip (the chains are swapped for fresh ones of the same effects), so a long delay
  or reverb tail never rings on into editing or the next play. The mixer itself stays until the next game applies its
  own. `AudioEngine::SetMixer` compares the new mixer with the one it has and
  sends only the difference:
  - a changed volume, mute or effect parameter is a command, ramped;
  - a new bus gets a node, with its gains and first chain set before it is handed over, attached to its parent
    (`AddBus`, parents first); a bus with a new parent moves (`SetBusParent`); a removed bus's live groups are pointed
    at the buses that remain and its children move to their new parents first, then the bus is removed (`RemoveBus`),
    and the mixing thread moves to Master whatever still feeds it before it hands the bus back;
  - after every change, each live group is pointed again by its name (`MoveGroup`): a bus added, or restored by an undo,
    collects the groups that asked for its name and played on Master meanwhile, and a renamed bus (a removal and an
    addition) loses the groups of its old name to Master;
  - a bus whose effects changed in kind, number or order gets a new `AudioEffectChain` (`SetBusEffects`), swapped at the
    bottom of a dip;
  - an effect object carries over into the new chain when its bus keeps an effect of the same kind at the same rank
    among that kind's effects (the bus's first Reverb stays its first Reverb), so inserting a filter in front of a
    reverb keeps the reverb's tail.

  Moves and swaps fade the bus's or the group's output out over 5 ms, make the change, and fade it back in: a dip of
  10 ms on what they change, never a click. An edit through `mixer.set` therefore reaches the running mix at the next
  frame's apply and the next block, ramped.
- **Bus names.** A source whose `Bus` is empty plays on Master; one that names a bus the mixer lacks plays on Master
  and is logged once per name and mixer. A bus removed while voices play on it sends them to Master.
- **The game's values.** Scripts set a bus's game volume and mute (`Audio.SetBusVolume`, `Audio.SetBusMuted`): a second
  gain stage of the bus, after the mixer's own, 0 dB and not muted when a game starts. So a settings menu's volume and
  the mix design (the asset, its snapshots) never overwrite each other, and nothing a script sets is written to the
  asset. The main thread keeps them by bus name and sends them to every node made for that name, so a bus removed and
  restored by an undo gets its game value back.

### 9.6 Effects

A bus runs its effects in list order. `MixerEffect` is one struct for every kind: `Kind` first, then the parameters of
every kind; the inspector shows the ones the kind uses (`VisibleWhen`). Each effect is made on the main thread
(`AudioEffect::Create`) with everything it will need allocated, and runs inside its bus's node on the mixing thread.
Parameters move over `AudioParameterSmoothingSeconds` (20 ms): frequencies and Q on a log2 scale, decibels in
decibels, the rest linearly. Filter frequencies are lowered to 0.45 times the sample rate when they run.

- **LowPass, HighPass, Peaking, LowShelf, HighShelf**: biquads with the coefficients of Robert Bristow-Johnson's Audio
  EQ Cookbook (the Q form for the shelves), computed in double and run in float in direct form I, the last two inputs
  and outputs of each channel as its registers. While a parameter moves, the coefficients are computed again every
  `AudioEffectStepFrames` (32) for the step's end, and the step's frames move to them one by one. Direct form I holds only the signal's history, so new coefficients change no internal
  state; the transposed form II mixes the coefficients into its registers, and each update under a running signal
  leaves a step in the waveform's slope (a 300 Hz sine through a 20 ms sweep from 8 kHz to 400 Hz: a second difference
  about 40 times the sine's, against twice in direct form I). `ComputeBiquadCoefficients` and `EvaluateBiquadGain` (the response's closed form, `|H(e^jw)|`) are what the
  tests compare measured gains with.
- **Delay**: a feedback delay line (Schroeder's comb filter: the line holds the input plus `Feedback` times what it
  held `DelaySeconds` before), mixed as `DryDb` times the input plus `WetDb` times the line. Its line is allocated for
  `MaxMixerDelaySeconds` (2 s) at the engine's rate, per channel (750 KiB in stereo at 48 kHz). A change of
  `DelaySeconds` crossfades from the old read position to the new one over 5 ms.
- **Reverb**: verblib 0.5 (Philip Bennefall; MIT No Attribution or public domain, `verblib.h:712-745`), which the pinned
  miniaudio 0.11.25 archive carries in its extras (`extras/nodes/ma_reverb_node/verblib.h`). It is Freeverb (Jezar at
  Dreampoint, public domain, `verblib.h:7`): per channel eight parallel feedback combs with a damping low-pass and four
  allpasses in series (`:134-180`). The engine calls verblib from the bus's node rather than use `ma_reverb_node` (the
  node wrapper in the same folder), because a miniaudio node of its own would stand outside the bus's chain and its
  order. An instance holds about 400 KB (its buffers are sized for 4 times 44.1 kHz). verblib takes one or two channels
  and 22,050 to 176,400 Hz (`:422-430`): the reverb runs on the first two channels of the mix and passes any others
  unchanged, and at a rate outside that range passes everything unchanged and is logged once per engine `Init`
  (effects are made again at each chain change and at each game's end, `ClearEffectState`, and do not log the
  rate themselves). `ThirdPartyNotices.md` names verblib beside miniaudio. verblib runs for its two tails alone (its
  wet at unity, its dry at none, its width at 1); the effect mixes them with the input by `WetDb` and `DryDb`. `RoomSize`
  and `Damping` reach verblib once a step: they set its combs' feedback and damping, which change what the combs hold,
  not what leaves them at once. `Width` is verblib's mix of the two tails (`:360-361`), a pair of gains, so the effect
  mixes the tails itself at a width ramped frame by frame, as it ramps `WetDb` and `DryDb`; given to verblib once a
  step, a moving width moved the output at each step by the step times the tails' difference. The tails are not the
  same on every platform: verblib sizes its delay lines in `long double` (`:406-416`), which is the x87's 80-bit format
  on Linux on x86-64 and `double` on Windows and on macOS on arm64, and where the exact length is a whole number of
  frames the two round to either side of it: at 48 kHz the left channel's eighth comb and second allpass are 1,759 and
  479 frames on Linux against 1,760 and 480 elsewhere (3,519 and 959 against 3,520 and 960 at 96 kHz). A test of an
  effect's output therefore bounds what holds whatever the tails are, never their exact values.
- **Compressor**: Giannoulis, Massberg and Reiss, "Digital Dynamic Range Compressor Design: A Tutorial and Analysis"
  (J. Audio Eng. Soc. 60(6), 2012): feed-forward, in the log domain, the static curve of their eq. 4 with a quadratic
  soft knee of `KneeDb` (`ComputeCompressorOutputDb`), and their smooth decoupled peak detector with one-pole attack and
  release (`exp(-1 / (time * rate))`), linked across channels by the largest magnitude; `MakeupDb` is added after,
  gliding frame by frame while it moves (the other parameters reach the output through the detector's smoothing).
- **Limiter**: the same gain computer with an infinite ratio at `CeilingDb`, made exact by looking ahead
  `AudioLimiterLookaheadSeconds` (2 ms): each frame's needed gain (the ceiling over the frame's largest magnitude, at
  most 1), its minimum over the window (Lemire's streaming minimum, in a fixed ring), a release that lets the gain rise
  at `ReleaseSeconds` and never above that minimum, then the mean over the window, applied to the signal delayed by the
  window. Each term of a frame's mean covers that frame, so every frame's gain is at most the gain it needs and no sample
  leaves above the ceiling; the bus is 2 ms late.

### 9.7 What calls it: the scene, scripts and the editor

- **The scene** drives it while running, as before: `OnRuntimeStart` creates a source for every `AudioSourceComponent`
  with a clip, playing when `PlayOnStart` is set; sources with `Stream` stream the clip asset's file without loading the
  asset. Each update compares the components with the settings last applied, so changes from anywhere (inspector,
  scripts, automation) reach the sounds: a new clip, or a change of `Stream`, makes a new, stopped main voice; sources
  added while running start like the others; removed components and destroyed entities lose their voices. Spatial
  sources follow their entities' world positions (the transform cache, §7.1), and the listener follows the first active
  `AudioListenerComponent` in hierarchy order, else the primary camera, else the origin facing -Z. `Scene::SetPaused`
  holds the scene's voices.
- **`AudioSourceComponent`** has `Priority` (0 to 255, default 128; higher is kept) and `MaxVoices` (1 to 64, default
  4), compatible fields of the scene format that the scene sends to its sources as they change; `Bus` (a bus name; empty
  is Master) joins them with the mixer's buses; `DopplerLevel` (0 to 5, default 1) says how much of the doppler effect a
  spatial source takes (§9.8).
- **Scripting.** A static `Strada.Audio`: `MasterVolume` (linear; its getter is a pure node), `GetBusVolume(bus)` (dB)
  and `IsBusMuted(bus)` (`[PureNode]`), `SetBusVolume(bus, decibels)`, `SetBusMuted(bus, muted)`,
  `PlayClipAtPoint(clip, position, volume, bus)`, `ActiveVoiceCount` (a pure getter), and MX2's `TransitionTo(snapshot,
  seconds)` (an impure node answering whether the mix blends to it; §9.8), `CurrentSnapshot` (a pure getter, the
  mixer's own name of the snapshot) and `GetBusPeak(bus)` (`[PureNode]`: the bus's meter, §9.8); and on
  `AudioSourceComponent`, `PlayOneShot(volume)` (the source's clip), `PlayOneShot(clip, volume)`, `Bus`, `Priority`,
  `MaxVoices` and `DopplerLevel`; `ReverbZoneComponent` has every field (§9.8). Each is a node, with a recipe where its
  defaults do not run it (a one-shot needs a source with a clip; `PlayClipAtPoint` a clip; MX2's run on their defaults,
  a blend to the empty name, the mixer's own values). What scripts set is run-time state of the mix (the game's values, §9.5), never written to the asset. The calls
  reach the scene through `Scene::PlayAudioOneShot` and `Scene::PlayAudioClipAtPoint`, which bring the source up to date
  with its component first (a clip, bus or pool size set this frame applies) and load the clip asset (a streamed
  source's own clip included); a source whose component names a clip needs it to play, one that names none plays the
  clips it is given (§9.3), and numbers that are not finite are script errors naming the
  member. The managed `Mixer` reads the asset's own buses (names, parents, volumes, mutes) for a settings menu; the game's
  values are `Audio`'s. A test run mixes into the null device in real time, so FeatureTest's checks assert what the main
  thread knows at a call (a voice started, a count right after a start, the snapshot just blended to), never that a
  sound ended or how loud a bus reads.
- **The editor and the agents.** The `.smixer` inspector is the generic data asset editor; `mixer.*` are the generated
  commands. The rows of a list of buses are titled by their `Name`; an effect's row, which has no string field, by its
  kind and the first parameter that kind shows (`0: Reverb, Room Size 0.5`; `FieldEditor::DescribeElement`).
- **The Mixer panel** (`MixerPanel`, Docs/Editor.md "The Mixer panel"; docked below the scene view beside the Console,
  open by default, `viewport.screenshot`'s window `Mixer`): the shown mixer (the project's, or one chosen in the panel)
  as a strip per bus (name, meter, fader, mute), the selected bus's other fields through the generic field editor, the
  snapshots with Audition, and the mixer's other fields. Every edit is one `SetDataAssetCommand` of the whole mixer's
  fields (merge keys "MIXR" plus a count for a fader's drag and a field editor interaction, "MIXS" plus a count for every
  other edit), so validation judges the whole result and an edit it refuses changes nothing. What a rename, a removal and
  a change of an effect list do to the rest of the mixer is plain code without ImGui (`Editor/MixerEditing.h`: a rename
  reaches parents, `DuckedBy`, snapshot values and `Impacts.Bus`; a removal re-parents the bus's children to its parent
  and drops what named it; snapshot values that set effects follow their effects, by applying the list's edit to the
  effects' indices with `FieldChange::MakePatch`). The panel is **live** only while the editor plays the game and the
  shown mixer is the project's (the one `SceneRunner` applies): only then do its meters read `AudioEngine::GetBusMeters`
  and Audition blend the running mix (`AudioEngine::TransitionTo` over 0.25 s), because the audio engine holds the
  mixer the last game applied and mixes nothing while the editor edits. An audition remembers the snapshot the mix had
  and blends back to it when pressed again, when the panel is hidden or closed, when another mixer is shown or the
  project changes; it ends without a blend of its own when the game moves the mix to another snapshot or play stops
  (`ResetGameValues`). It is run-time state of the mix: never an undo step, never written to the asset.

### 9.8 What MX2 adds, on this design

- **Snapshots** (`Mixer::Snapshots`, `MixerSnapshot`): a name and the values it sets (`MixerSnapshotValue`: a bus, a
  target, its volume, its mute or an effect's parameter by the effect's index, and the value), differing from the
  mixer's own values only where they say so. They and every other MX2 field joined `.smixer` version 1 as fields with
  defaults within the slice, before any `.smixer` existed outside it, so a file without them reads them as their
  defaults and no migration exists (§7.3). `ValidateMixer` refuses a value that names a bus, an effect or a parameter
  the mixer lacks (a parameter the effect's kind does not use included: the kinds and ranges are read from
  `MixerEffect`'s field table), one out of its target's range (a volume -80 to +24 dB, a mute 1 or 0, a parameter its
  field's range), and two values of a snapshot that set the same thing.
  - **The main thread decides, the mixing thread moves.** `AudioEngine::TransitionTo(name, seconds)` (names matched
    ignoring letter case; a name the mixer lacks is logged once per name and mixer and changes nothing) makes the
    snapshot current and sends what differs between the values last sent and the mixer's own values with the
    snapshot's in their place (`ApplyMixerSnapshot`): one `SetBusMixValues` per bus whose volume or mute differs and
    one `SetEffectParameter` per effect parameter that differs, each carrying the transition's seconds, and each value
    then moves on its own from where it is to its target. The commands of one transition are pushed in one call, but a
    device's thread may take the ring between two pushes, so they can start a block apart (5.3 ms at 48 kHz, inaudible
    in a fade of a tenth of a second or more); a burst that fills the ring (a snapshot sets at most 128 values, and a
    transition sends at most what two snapshots set) waits in order like any other. This replaces the design's first
    text, a table handed over by a `BeginTransition` command: the commands that exist for edits already carry a value and
    a time, a table would be one more object to hand back, and a transition needs no state of its own on the mixing
    thread.
  - **Scales.** A bus's volume moves linearly in decibels over the transition: its amplitude is multiplied by one factor
    each frame (from -80 dB when it starts from silence, and silence at the last frame when it ends there), so no power
    is computed per frame (`AudioBusMixGain`). A mute moves linearly in amplitude between 1 and 0 over the same frames.
    Effect parameters move on their own scales (§9.6: frequencies and Q in log2, decibels in decibels, the rest
    linearly) over the transition's frames (`AudioEffect::SetParameter` takes them); a Delay's `DelaySeconds`
    crossfades at once over 5 ms, as for an edit. Seconds not above the values' own smoothing (5 ms for gains, 20 ms
    for effect parameters), NaN and negative ones included, are that smoothing, linear in amplitude as an edit is, so
    that no existing ramp changes; seconds above `MaxSnapshotTransitionSeconds` (300) are that.
  - **Time.** A transition moves with the mixer's blocks: buses process in every block whatever their scenes do, so a
    paused scene's buses move on (the pause snapshot muffles the mix while the game is paused), and with manual output
    a transition moves only as the mix is read. A transition that starts while another runs carries every value on from
    where it is.
  - **Edits.** The engine keeps the mixer's own values (`GetMixer`) apart from what it sent (`AudioBusState::Values`,
    the mixer with the current snapshot's values in place). An edit of the mixer (`SetMixer`: the inspector,
    `mixer.set`, an undo) is applied under the current snapshot, so a value the snapshot sets stays at the snapshot's and
    a transition under way goes on for the values the edit leaves alone (a bus's volume and mute ramp apart: a command
    that changes one leaves the other's ramp as it is); an edit that removes the current snapshot returns the mix to
    the mixer's own values (over an edit's ramp) and is logged once. Chains made again (a bus's effects changed in kind,
    `ClearEffectState`) are made from the values sent.
  - **The current snapshot** (`AudioEngine::GetCurrentSnapshot`) names the target as the mixer names it; the empty
    name is the mixer's own values. `ResetGameValues`, which `SceneRunner` calls when a game starts and when it stops,
    returns the mix to them, so a game that stops while paused does not leave its pause snapshot on the editor. A scene
    that a game's script loads in place of another keeps the snapshot, as it keeps the game's bus values: the mixer is
    the game's, not a scene's, and the game moves its mix on as it moves its scenes.
- **Ducking**: `MixerBus` has `DuckedBy` (a bus), `DuckAmountDb` (-80 to 0), `DuckThresholdDb` (-80 to 0),
  `DuckAttackSeconds` and `DuckReleaseSeconds` (the threshold is this track's addition: the law needs the level at which
  the duck is full). The law, per bus and frame: while the RMS of the ducking bus's output
  over the block before (its level after every gain of its own, so a muted bus ducks nothing) is at or above the
  threshold, the duck's gain heads for `DuckAmountDb`; below it, for 0 dB; it moves linearly in decibels at
  `|DuckAmountDb|` per `DuckAttackSeconds` going down and per `DuckReleaseSeconds` coming back (one factor a frame, so
  the times do not depend on how long blocks are). A duck that an edit leaves deeper than its new amount comes back up
  to the amount at its own depth per `DuckReleaseSeconds` (and from the amount on to 0 dB at the amount's rate, so it
  then takes longer than one release in all), and the release's rate stays the amount's, so every later duck comes
  back at `|DuckAmountDb|` per release. It is a gate with a ramp, not a compressor: a voice bus lowers music by the
  amount whatever its loudness above the threshold. The level read is that of the previous block, which the mixer copies for every bus at the end of
  each block (`AudioBusNode::m_PreviousRms`), because miniaudio's graph pulls buses in an order of its own; so ducking
  reacts a block after the other bus starts or stops (5.3 ms at 48 kHz) whatever that order is. The duck's gain comes
  after the game's and before the dip of a move or a swap. `ValidateMixer` refuses a bus ducked by one the mixer lacks,
  by itself, or by a bus whose level depends on its output: a level depends on what feeds the bus (its child buses) and
  on what ducks it, and a cycle of those dependencies would feed a bus's output back into its own gain (a bus ducked
  by its parent, or two buses ducking each other). The main thread sends ducking (`SetBusDucking`: the ducking bus's
  node and the four values) after every bus of an edit exists, since the ducking bus may be made after the one it
  ducks, and before any `RemoveBus`; `RemoveBus` ends any ducking by the removed bus on the mixing thread, as it moves
  the bus's groups. A bus whose ducking ends (its `DuckedBy` emptied, or the ducking bus removed) comes back up over its
  own release, never at once: the command carries the bus's values with the null node.
- **Doppler, computed by the engine.** Positions are relative to the listener, so miniaudio's own doppler has nothing to
  read (the engine sets its factor to 0). At each update of a running scene (`Scene::UpdateAudio`, after scripts and
  physics) the scene takes the listener's and every spatial source's velocity from its world position at this update
  and at the last, over the game time between them (the frame's time times the scene's time scale):
  `ComputeAudioVelocity`. The positions are the scene's own record of its last update, not the transform cache's
  previous matrices (`CachedWorldTransform::Previous`, §7.1), which the design first named: the cache is refreshed only
  by the views that draw a frame, after the update, so at the update its previous matrix is the last drawn frame's, and
  a scene that is never drawn (a headless run, a test, the player's audio capture) has none. The AudioScene then gives
  each voice the factor `c / (c - v * DopplerLevel)` (`ComputeDopplerFactor`, `AudioScene::UpdateDoppler`), `v` being the
  speed at which source and listener close in along the line between them and `c` `Mixer::SpeedOfSound` (343 m/s, 1 to
  10,000), kept within 0.5 and 2 (`MinAudioDopplerFactor`, `MaxAudioDopplerFactor`; also when `v * DopplerLevel`
  reaches `c`). The factor multiplies the pitch the voice plays at: it travels as the voice's `SetVoicePitch` (the pitch
  times the factor), ramped over 20 ms on a log2 scale as any pitch is, while `GetPitch` and the scripts read the pitch
  as it was set; this replaces the design's first `SetVoiceDoppler`, since the pitch's command already carries one value
  to one voice with the ramp the factor needs. A source's voices share its factor, a one-shot started later starting at
  it; a voice of the scene (`PlayClipAtPoint`) stands still and takes the listener's motion alone. **None** (a factor of
  1) is given for a source's first update (made, its sound made again, or made spatial since the last), for an update
  in which the source or the listener moved faster than `c / 2` (a teleport: a respawn, a cut, which no speed explains),
  for a listener that is a different entity than at the last update (a cut between cameras a few meters apart would
  otherwise pass for a speed), for an update without game time (the first, a scene whose time scale is 0), for the
  first update after the scene resumes from a pause (what an edit moved meanwhile did not move in game time), for a
  source that is not spatial or whose `DopplerLevel` is 0, and when source and listener are less than a millimeter
  apart. A source moved only in fixed steps without interpolation moves in some frames and not in others when frames
  outnumber steps, so its pitch warbles: interpolated bodies (the default, §8) and kinematic motion set per frame do
  not.
- **`ReverbZone`** (`ReverbZoneComponent`, §7.2): a sphere of `Radius` or a box of `HalfExtents` round its entity,
  placed and turned by its world transform and sized times its scale (a sphere by its largest axis), a `FadeDistance`,
  a `Bus` and a `SendLevel` (0 to 1). At each update of a running scene (`Scene::UpdateReverbZones`, after the sources'
  positions and doppler) the scene hands its zones to the AudioScene (`AudioReverbZoneDesc`, fields scripts set taken
  within their ranges), which computes each spatial voice's send to each zone's bus (`ComputeReverbZoneSend`: the
  level inside the shape and on its surface, falling linearly with the distance from the shape to nothing at
  `FadeDistance`, at once with a fade of 0), keeps the largest per bus and the two largest buses
  (`MaxAudioVoiceSends`; ties to the bus whose key sorts first, so the zones' order never matters), all in the zone
  `Audio.ReverbZones`. A source's voices send from the source's position, a voice of the scene (`PlayClipAtPoint`) from
  its own: the send follows the sound, not the listener, so a bike under a bridge echoes for a camera that stands
  outside. A voice that is not spatial sends nothing. A zone whose `Bus` is empty sends nothing, and so does one whose
  bus the mixer lacks (logged once per name and scene): the scene's group of a missing bus plays on Master, where a
  send would only double the sound. Voices started later (one-shots, `PlayClipAtPoint`, impact sounds) take the sends
  of the last update before they are handed over: with manual output they send from their first block; on a device
  the voice's start and its sends are two pushes the device's thread may take the ring between, so the sends may start
  a block later (5.3 ms at 48 kHz, which cannot be heard).
  - **The send node** (`AudioSendNode`, `AudioMixer.h`). A voice that sends for the first time gets one, made on the main
    thread (`AudioVoice::EnableSends`; its three output buses are allocated there, past miniaudio's two inline ones) and
    owned by the voice: a custom node with one input and three outputs, the dry signal unchanged and two sends, each
    the signal times a gain ramped per frame over `AudioGainSmoothingSeconds`, after the voice's volume, attenuation
    and panning. `AddVoiceSends` hands it to the mixing thread, which puts it between the sound and its group (the
    sound feeds the node, the node's dry output the group, each send the scene's group of its zone's bus, between two
    blocks); `SetVoiceSend` gives a send a group and a level, sent only when one changes. A send keeps its slot while
    its bus stays among the voice's two, so a level that changes only ramps; a send that changes group while heard
    fades out, moves at the end of a block and fades in. The node comes back with its voice, which detaches every
    output; a group comes back once no voice and no send plays into it (`m_MixSendCount`).
  - **Why every output is always attached, and why scenes pause in one command.** miniaudio processes a node of
    several outputs once a period, when an output read since the last processing is read again, and serves the
    period's other outputs from its cache (`miniaudio.h:75245-75260`, `:75417`): an output left unread for a period
    would from then on be served a period late, and with blocks of other lengths read past its request. So a send that
    has no zone plays silence into the scene's Master group, every group of a scene is read once each block, the node
    processes continuously (`MA_NODE_FLAG_CONTINUOUS_PROCESSING`, silence while its voice gives nothing, so each output
    has the block's whole length and no group reads it twice), and a scene pauses and resumes all its groups in one
    command (`SetScenePaused`, naming the scene's number that its groups carry) where it once sent a command per group,
    which a block could split.
  - A voice with sends costs a copy per output per block; voices without zones keep no node.
- **Meters**: each bus node's peak and RMS of its output over the last block it processed, after its effects and every
  gain, across all its channels, written by the mixing thread as relaxed atomics and read by any thread
  (`AudioBusNode::GetPeak`, `GetRms`). `AudioEngine::GetBusMeters` lists them in the mixer's order, `GetBusPeak(bus)`
  and `GetBusRms(bus)` read one (a name the mixer lacks reads 0 and is logged once per name and mixer). `audio.meters`
  (read-only, not an undo step) answers them, linear and in dBFS (silence, and anything below -80 dB, reads -80; a
  level that is not a finite number reads null), with the current snapshot and the active, mixed and stolen voice
  counts; without an audio engine it answers `initialized: false` and nothing else.
- **`--audio-capture <file.wav>`** on the player (Docs/GettingStarted.md): the application mixes through manual output
  (`ApplicationSpecification::ManualAudio`, whatever `--headless` or `STRADA_AUDIO_OUTPUT` say), so nothing reaches a
  speaker. After each frame's update the runtime layer waits for streamed sounds to read ahead
  (`AudioEngine::WaitForStreams`, so the file never depends on when the streaming thread ran), reads the frames of audio
  the frame's time covers (`AudioCaptureClock`: the frame's step at the mix's rate, the fraction carried into the next
  frame; a paused game and its time scale do not hold it) with `AudioEngine::ReadFrames` on the main thread, and appends
  them to the file as 16-bit PCM (`WaveFileWriter`: the header first, its sizes written when the run ends, samples
  clamped to -1 to 1, scaled by 32767 and rounded). With `--timestep` and `--frames` a run is a pure function of the game
  and two runs write the same bytes. A file that cannot be made or written stops the run with exit code 1; a path whose
  folder does not exist is a wrong command line (exit code 2). It runs headless, windowed and with `--test`.
- **Impact sounds** (`Mixer::Impacts`: `Bus`, `MinImpulse`, `FullImpulse`, `MaxPerFrame`). After the zones, the
  scene (`Scene::PlayImpactSounds`, zone `Scene.ImpactSounds`) reads the frame's contact events (§8 "Contact data"):
  each collision that begins with contact points (a `CollisionEnter`; triggers and exits never sound) whose strongest
  point's impulse is at least `MinImpulse` plays the `ImpactSound` of each side's physics material (the point's
  `Material` and `OtherMaterial`; the same clip on both sides plays once) at that point, through `PlayClipAtPoint` on
  `Impacts.Bus`, at a volume of the impulse over `FullImpulse`, at most 1. The collisions are taken by impulse, the
  strongest first (ties in the events' order: by step, then by their bodies' UUIDs), and a frame plays at most
  `MaxPerFrame` sounds, so the limit keeps the same ones on every run. An empty `Impacts.Bus`, which a new mixer has,
  plays none, and `ValidateMixer` refuses a bus the mixer lacks. A clip is loaded and decoded once while the scene runs
  (`AudioEngine::PreloadClip`): when the scene starts with a mixer that plays impact sounds, the impact sounds of the
  materials its colliders name are decoded then, so no first impact decodes in the middle of a game; a clip that does
  not load is logged once and plays nothing. The impulse is PC's estimate per manifold (§8), so a body that lands
  across two faces sounds at the larger of the two. Particles stay for scripts: contacts spawn none.

### 9.9 Testing and measuring the mix

- **Sound is measured, never listened to**: levels, frequencies and decays read from manual output
  (`Testing::AudioEngineScope`), never from a device, never depending on wall-clock time or a thread's scheduling.
- **Allocations on the mixing thread** are counted by two hooks, both keyed on `AudioEngine::IsMixingOnThisThread()`.
  The mixer records its thread's identity (`std::thread::id`) for the length of each block (its apply, its render and
  its finish, not what `ReadFrames` does between blocks on the main thread's behalf: flushing deferred commands,
  destroying what came back) in an atomic of static storage in `AudioEngine.cpp`, beside the mixer's
  `ProfilerThreadState`, and the function reads nothing else: no lock, no thread-local storage and none of the engine's
  data, so a hook may call it from any thread at any time, while `Init` starts the streaming thread and while
  `Shutdown` frees the engine included. The hooks:
  - `AudioEngine::GetMixAllocationCount()`: the engine's allocation callbacks, through which miniaudio allocates for
    every object of the module, count the calls made on the mixing thread while it mixes, in a counter of static storage
    that `Init` resets.
  - StradaTests replaces the global allocation functions (every form of `operator new` and `operator delete`) in one
    source of its own (`Audio/MixAllocationHook.cpp`), counting those called on the mixing thread while it mixes; the
    engine's own code allocates only through them. A sanitizer's runtime defines those functions itself, so in a build
    with `STRADA_ENABLE_SANITIZERS` or `STRADA_ENABLE_THREAD_SANITIZER` (which define `ST_SANITIZER_BUILD` for Strada's
    targets) the source leaves them to it: nothing is counted there, and the cases that compare this count say they
    skip that check (`Testing::IsMixingThreadAllocationCounted`) and make their other checks.
  - The case mixes starts, steals at the pools and at the limit, one-shots, voices of the scene, value changes, a scene
    paused and one ended, and expects both counts to stay 0. C library allocations of other code are
    not counted: none runs on the mixing thread (decoding moved to the main thread; libvorbis runs only on the main and
    streaming threads).
- **Snapshots, ducking and meters** are tested from manual output against their closed forms
  (`Audio/AudioMixControlTests.cpp`): a transition's volume at three frames within 0.02 dB of linear in decibels and its
  mute within 2e-4 of linear in amplitude; a peaking filter's gain in decibels and a low-pass's frequency in log2 halfway
  (3 dB down where hertz would give 0.3); a transition interrupted by another starting where the first had taken the
  value; an unknown snapshot changing nothing, logged once; edits under a snapshot and in the middle of a transition (an
  edit of a bus's volume leaving its mute's ramp alone); a transition that runs while its scene is paused; NaN,
  negative and overlong seconds; the return to the mixer's own values at a game's end (and through `SceneRunner`);
  ducking's attack and release at three frames each within 0.05 dB of the law, a muted ducking bus ducking nothing,
  and a duck that ends coming back over its release; each bus's peak and RMS against a constant level and a sine of
  whole cycles. The `.smixer` validation and round trip with the new fields are `Asset/MixerAssetTests.cpp`'s, the
  registry-wide fuzz damages the FeatureTest mixer, which sets every new field, and `audio.meters` has
  `StradaEditorTests`' `AudioCommands:` cases and a sure request in the automation fuzz. The allocation case and the
  null-output threading case below mix transitions and ducking too.
- **Doppler** (`Audio/AudioDopplerTests.cpp`) is measured in the mix of running scenes that are never drawn: a 1 kHz
  tone's frequency, read from the left channel's rising zero crossings, within 1 % of `c / (c - v)` for a source passing
  through the listener at 30 m/s (approaching and receding), a listener walking away at 30 m/s, a sound played at a
  point while the listener walks toward it, and a source's one-shot at a pitch of its own; the clip's own frequency
  within 0.2 % at level 0; no doppler at a source's first update, at a teleport, for a listener that changes entity, at
  a time scale of 0, after a pause and for a sound that is not spatial; the mixer's speed of sound and twice the level;
  and the factor and the velocity's closed forms with their limits. **The player's capture** (`StradaRuntimeTests`): the
  `AudioCaptureClock`'s counts, and runs of a game with a spatial tone and streamed music whose files hold the header,
  the length the frames' steps give (with and without a fraction of a frame) and sound on both channels, the same bytes
  in two runs; `WaveFileWriter`'s header and conversion are `Audio/WaveFileWriterTests.cpp`'s.
- **ThreadSanitizer** runs the audio cases by their prefixes (`AudioEngine:`, `AudioQueue:`, `AudioScene:`): a null
  output mixing while the main thread plays one-shots on a moving source, changes, steals and stops voices and ends a
  scene; a device taken over and started again; the mixing-thread hook asked from another thread across `Init` and
  `Shutdown`; the queue and the return list between two threads. The null-output cases count the mixer's blocks
  (`AudioEngine::GetBlockCount`), never time.
- **The budget**: 32 voices on 6 buses with a filter each, one reverb and one compressor in at most 1 ms of mixing per
  10 ms of audio, measured in Release by a StradaTests case behind `STRADA_AUDIO_BENCHMARK=1`
  (`Audio/AudioBenchmarkTests.cpp`) that mixes through manual output at 48 kHz in stereo: Master (a low shelf, then a
  compressor), Music (a low-pass), Effects (a high-pass), Engine (a peaking filter), Ambience (a high shelf) and Reverb
  (a low-pass, then a reverb without its dry signal); 4 music voices that are not spatial, 10 spatial effects of
  44.1 kHz clips at pitches of 0.8 to 1.25, 10 engine layers whose pitch moves every simulated frame, 4 of ambience and 4
  on the reverb's bus, every spatial source moving. Every 800 frames (60 Hz) a frame's commands are sent and
  `AudioEngine::Update` runs; reads of 480 frames (10 ms) are timed over 20 s of audio, three rounds, and the profiler's
  `Audio.Mix` median is printed beside them. It asserts structure only (32 voices mixed, no allocation on the mixing
  thread, sound reaching the output). The same case reports 64 voices, the engine's limit, with one stolen and fading
  every frame (a pool of 2 whose each new one-shot steals the one before), the figure later tracks plan with (two bikes
  of 16 engine layers each reach 32 alone). A third case runs the 32 voices with MX2's features in play: Music ducked by
  Effects (a threshold of -70 dB, which the effects pass), a snapshot setting a bus volume and two effect parameters
  that the run blends to and back from every half second over a second, so a blend is always under way, a sphere zone
  sending the spatial voices round it to the Reverb bus (each voice that sends gets its send node), and every source's
  velocity given each frame with doppler computed, so the engine layers' and the moving sources' pitch commands carry
  the factor; it checks that each was in play (the ducking bus above its threshold, voices sending, voices pitched by
  doppler) and that nothing allocated on the mixing thread. MX1's sitting (2026-11-17, Release, the reference machine, another lane's
  editor tests running; the gauge, the 10,000-instance benchmark's Submit + Prepare over its 12 medians, read 1.21 to
  2.15 ms, out of the 1.4 ms band): 32 voices a median of 0.24 ms and a 99th percentile of 0.34 to 0.36 ms per 10 ms of
  audio, met out of band; 64 voices with a steal every frame a median of 0.44 to 0.70 ms and a 99th percentile of 0.52 to
  0.75 ms. MX2's sitting (2026-11-17, Release, the reference machine, the cases run back to back, the gauge read 1.16 to
  1.39 ms, in band; the other lane started a build as the gauge ended): 32 voices a median of 0.227 to 0.231 ms and a
  99th percentile of 0.27 to 0.31 ms; the same with MX2's features in play a median of 0.269 to 0.272 ms and a 99th
  percentile of 0.42 to 0.43 ms (28 of the 32 voices sending to the zone's bus and pitched by doppler), met in band; 64
  voices a median of 0.42 to 0.43 ms. A track's figures are provisional; the slice's gate measures again with every lane
  idle.

## 10. Scripting (C# / .NET 10)

This section is the contract of the scripting runtime; [Scripting.md](Scripting.md) shows game developers how to write
scripts and [VisualScripting.md](VisualScripting.md) how to write visual scripts.

### 10.1 Hosting

`DotNetHost` locates `hostfxr` in the .NET host's search order (an app-local `dotnet/` directory for exported games,
`DOTNET_ROOT_<ARCH>` and `DOTNET_ROOT`, the registered install location, the default one) and loads it directly: the
SDK's static nethost library links against the static C runtime, which the engine (`/MD`) cannot mix in, so only the
SDK's hosting headers are used. It initializes the runtime from `Strada.ScriptCore.runtimeconfig.json` (CoreCLR starts
once per process and is never unloaded: `ScriptEngine::Shutdown` unloads the game and later `Init`s reuse the
runtime), loads `Strada.ScriptCore.dll` into the default load context and resolves the `[UnmanagedCallersOnly]`
entry points of `Strada.Interop.Host` by name: 12 for the runtime, the game assembly, C# script instances and data
assets (`InvalidateDataAssets`, §6.4), and 15 for visual scripts (`VisualScript*`, §10.6); a missing one fails
`ScriptEngine::Init`. Game assemblies load into a
collectible `AssemblyLoadContext` from bytes (with their symbols, for line numbers), so the files are never locked (hot
reload); `Strada.ScriptCore` and the framework resolve to the default context's assemblies, other dependencies from the
game assembly's directory. Managed exceptions never cross into native code: every entry point catches, logs (with
stack trace) and returns a status. Entry points take strings and JSON as UTF-8 pointers and lengths, borrowed for the
call, and answer with text through a `receive(context, text, length)` callback that native code copies at once; the
visual-script entry points call it exactly once when they answer Success, and never otherwise.

Class metadata and field values cross the boundary as JSON in the scene files' script field format: the runtime
describes every concrete, non-generic `Script` class with a public parameterless constructor (field names, types,
defaults read from a default instance, `[Range]`, `[Tooltip]`, `[HideInInspector]`), and instances receive their
`ScriptComponent.Fields`; stored values of fields that changed type are logged and skipped. Visual scripts cross as JSON
too: documents in their content form (§7.3, compact), the values of public variables in the script field format, and
the runtime's node library and analyses (§10.6).

CMake builds the C# projects with the .NET SDK (`strada_add_dotnet_project`; MSBuild decides what is out of date, and
`Directory.Build.props` keeps outputs in the build tree): `Strada.ScriptCore.dll` lands next to the executables. With the
tests, the scripts of the samples and of the `csharp-starter` template build too, each into a folder beside the
executables (`StradaBlocksScripts` into `Blocks/`, `StradaShowcaseScripts` into `Showcase/`, `StradaFeatureTestScripts`
into `FeatureTest/`, `StradaCSharpStarterScripts` into `CSharpStarter/`), where the tests that play them find them.

### 10.2 Bindings

Native functions are exposed **by name**: the native side provides a `{name, function pointer}` table; the managed
`InternalCalls` class declares `delegate* unmanaged<...>` static fields whose names match, assigned by reflection at
startup — a missing, extra or duplicated binding is a fatal startup error listing the names. Blittable types only across the
boundary (`byte` for bool, UTF-8 `byte*` + length for strings). glm/C# layouts match: `Vector2/3/4` = floats,
`Quaternion` = (X, Y, Z, W), `Matrix4` = 16 floats column-major.

### 10.3 Script lifecycle

Scripts derive from `Strada.Script` (which derives from `Entity`) and override any of: `OnCreate()`,
`OnUpdate(float deltaTime)`, `OnFixedUpdate(float fixedDeltaTime)`, `OnDestroy()`, `OnCollisionEnter(Entity other)`,
`OnCollisionExit(Entity other)`, `OnTriggerEnter(Entity other)`, `OnTriggerExit(Entity other)`,
`OnClick(Entity button)` (a UI button that is the entity or one of its descendants was clicked),
`OnActionPressed(string action)` and `OnActionReleased(string action)` (an input action's edge, §7.4).
Public fields (and private fields marked `[SerializeField]`) of supported types are editable in the inspector and
serialized in the scene: `bool`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `string`, `Vector2`, `Vector3`,
`Vector4`, `Quaternion`, `Color`, `Entity`, `Prefab`, `AssetHandle`, typed asset references (`Mesh`, `Material`,
`Texture`, `AudioClip`, `Font`, `EnvironmentMap`, `VisualScript`, `Curve`, `InputActions`, `Mixer`, `PhysicsMaterial`), and enums. Enums are stored as integers of their size (the class
metadata lists the enumerators in declaration order and marks `[Flags]` enums; stored values out of the enum's range
are ignored) and typed references as asset handles (the metadata names the accepted asset type).

One scene runs scripts at a time (the `ScriptEngine`'s scene context, which the bindings act on). At runtime start
every instance is created before the first `OnCreate`, so scripts can find each other there. Each update first
follows `ScriptComponent` changes (new components get instances and `OnCreate`; a changed class or a removed
component gets `OnDestroy`; unknown classes are logged once per class name), then calls `OnUpdate`; `OnFixedUpdate`
runs before every fixed step, with or without physics; contacts reach the scripts of both entities, the other entity
arriving as its script instance when it has one, and `Strada.Collision` reads the event as the handling entity sees it
while its handlers run (§8, "Contacts in scripts and graphs"); input actions' edges reach every script, once each and
before the frame's clicks (§7.4); clicks reach the scripts of the button and its ancestors before the
frame's `OnUpdate`. Destroyed entities get `OnDestroy` while still intact (children
after their parent), and stopping or destroying the running scene calls `OnDestroy` on every script.
While a scene runs, `Scene::ReadScriptFields` copies the values a script's fields hold
(`ScriptEngine::GetInstanceFields`) into its Script component, and `Scene::ApplyScriptFieldChanges` gives scripts the
stored values that differ from those they were last given or read (`ScriptEngine::SetInstanceFields`): the editor reads
the fields it shows or edits during play and applies its edits (undo included) at once, so tuning a running game
reaches its scripts.

Visual scripts (§10.6) are a second kind of instance with the same lifecycle: `VisualScriptComponent`s get graph
instances as `ScriptComponent`s get script instances, every instance of both kinds exists before the first `OnCreate`
or `Event.Create`, and each event reaches an entity's C# script before its graph. A scene runs scripts when the
scripting runtime is initialized and a game assembly is loaded or the scene has VisualScript components; C# scripts run
only when the game assembly was loaded as the scene started (one loaded while it runs does not start them), and the
editor's Simulate mode runs neither kind.

### 10.4 Scripting API (namespace `Strada`)

- `Entity`: `ID`, `Name`, `IsValid`, `Translation`/`Rotation`/`Scale` shortcuts, `Transform`, `Parent`, `Children`,
  `HasComponent<T>()`, `GetComponent<T>()`, `AddComponent<T>()`, `RemoveComponent<T>()`, `Destroy()`,
  `As<T>()` (script instance), static `Create(name)`, `FindByName(name)`, `FindByID(id)`,
  `Instantiate(Prefab, Vector3 position, Quaternion rotation, Entity parent = null)`.
- Components: `TransformComponent` (`Translation`, `Rotation`, `EulerAngles` (deg), `Scale`, `WorldTransform`,
  `WorldTranslation`, `Forward`, `Right`, `Up`), `CameraComponent` (projection fields, `Primary`,
  `ScreenToWorldRay(Vector2)`, `WorldToScreenPoint(Vector3)`: viewport pixels and the depth in front of the camera),
  `MeshComponent` (`Mesh`, `GetMaterial(i)`, `SetMaterial(i, Material)`, `CastShadows`, `Visible`),
  `DirectionalLightComponent`, `PointLightComponent`, `SpotLightComponent`, `SkyLightComponent`,
  `RigidBodyComponent` (`Type`, `Mass`, `LinearVelocity`, `AngularVelocity`, `GravityFactor`,
  `LinearDamping`, `AngularDamping`, `Interpolation` (`RigidBodyInterpolation`), `AddForce(Vector3, ForceMode)`,
  `AddTorque(Vector3, ForceMode)`, `MoveKinematic(Vector3, Quaternion)`, `Teleport(Vector3, Quaternion)`, `IsSleeping`, `WakeUp()`),
  `BoxColliderComponent`, `SphereColliderComponent`, `CapsuleColliderComponent`, `MeshColliderComponent`,
  `JointComponent`, `CharacterControllerComponent` (every field, `Move(Vector3 velocity)`, `Velocity`, `IsGrounded`,
  `GroundNormal`, `Teleport(Vector3)`),
  `AudioSourceComponent` (`Play()`, `Stop()`, `Pause()`, `IsPlaying`, `Volume`, `Pitch`, `Loop`, `Clip`, `Stream`,
  `PlayOneShot(volume)`, `PlayOneShot(clip, volume)`, `Bus`, `Priority`, `MaxVoices`; §9.3; `DopplerLevel`, §9.8),
  `AudioListenerComponent`, `ReverbZoneComponent` (every field; `Shape` is a `ReverbZoneShape`, §9.8), `TextComponent`, `SpriteRendererComponent`, `ParticleSystemComponent` (every field,
  `Emit(count)`, `Clear()`, `ParticleCount`), `PostProcessComponent` (every field; `Tonemapper` is a `TonemapOperator`),
  `UICanvasComponent`, `UIRectComponent`, `UIImageComponent`, `UITextComponent` (every field), `UIButtonComponent`
  (every field, `WasClicked`, `State` (`UIButtonState`), `Click()`), `ScriptComponent` (`ClassName`, `Instance`),
  `VisualScriptComponent` (`Graph`; `SendEvent(name, params arguments)`, `HasVariable(name)`, `GetVariable<T>(name)`,
  `SetVariable<T>(name, value)` on the entity's running graph, §10.6).
- `Material` (runtime-creatable): `Create()`, `Clone()`, `IsEditable`, `BaseColor`, `Metallic`, `Roughness`,
  `Emissive`, `EmissiveIntensity`, `NormalStrength`, `OcclusionStrength`, the five texture maps, `AlphaMode`,
  `AlphaCutoff`, `DoubleSided`, `UVTiling`, `UVOffset`.
- `Input`: `IsKeyDown/Pressed/Released(KeyCode)`, `IsMouseButtonDown/Pressed/Released(MouseButton)`,
  `MousePosition`, `MouseDelta`, `MouseScrollDelta`, `CursorMode`, `IsPointerOverUI`, gamepad (`IsGamepadConnected`,
  `GetGamepadAxis`, `IsGamepadButtonDown/Pressed`), input actions (§7.4: `GetAction`, `GetActionVector`,
  `IsActionDown/Pressed/Released(string)`, `SetActionOverride`, `SetActionVectorOverride`, `ClearActionOverride`), and
  rebinding them (§7.4 "Rebinding": `StartRebind`, `Rebind`, `CancelRebind`, `ResetBindings`, `CurrentRebind`,
  `GetBindingControls`; the answers are `RebindResult`s with a `RebindStatus`, read from the running scene through
  `Input_ReadRebindText` and the `NativeRebindAnswer` layout).
- `Time`: `DeltaTime`, `FixedDeltaTime`, `Elapsed`, `FrameCount`, `TimeScale`.
- `Physics`: `Gravity`, `Raycast(origin, direction, maxDistance, out RaycastHit, layerMask)`; `RaycastHit` (`Entity`,
  `Point`, `Normal`, `Distance`, `Material`: the `PhysicsMaterial` of the surface hit, null for none, one shared
  reference per material). The queries of §8 "Queries", each with a `layerMask` and `includeTriggers` (every layer, no
  triggers, by default): `SphereCast(origin, radius, ...)`, `BoxCast(origin, halfExtents, rotation, ...)` and
  `CapsuleCast(origin, radius, halfHeight, rotation, ...)`, each `(direction, maxDistance, out ShapeCastHit hit)`, one
  binding (`Physics_ShapeCast`, the shape as `NativeQueryShape`, the hit in `NativeRaycastHit`'s layout); `ShapeCastHit`,
  a struct of public fields (`Entity`, `Point`, `Normal`, `Distance`, `Material`) that graphs Make and Break. The
  many-hit forms fill a `Span` the caller passes and return how many they wrote, the first of the order (the nearest, or
  the smallest UUIDs): `RaycastAll(origin, direction, maxDistance, Span<RaycastHit>)` (`Physics_RaycastAll`) and
  `OverlapSphere`, `OverlapBox`, `OverlapCapsule(..., Span<Entity>)` (`Physics_Overlap`, entity IDs). The bindings copy
  at most the buffer's length out of the scene's query; the managed side gathers them on the stack (up to 32 hits or
  128 entities) or in a pooled buffer, and `ScriptRegistry.GetEntity` keeps the `Entity` it makes for an entity without
  a script (empty again when the scene's instances go, and started over past 4,096 entities, since nothing tells scripts
  that such an entity was destroyed), so a query allocates no managed memory once the entities it finds have been met;
  the bindings' own vectors are native. Sizes that are not finite, negative or above `MaxPhysicsQueryRadius` and
  `MaxPhysicsQueryExtent` and values that are not finite are script
  errors naming the member; zero directions and distances that are not positive find nothing, silently, as for
  `Raycast`. The casts are impure nodes, as `Raycast` is; the four many-hit forms are not nodes, because a `Span` is a
  ref struct and because graphs hold arrays as values that a node may not write into (an array a variable or literal
  starts with is one object every run of the graph shares, `GraphInstance`'s constructor).
- `Collision` (the contact event being handled, empty outside a contact handler): `Entity`, `Other`, `Step`, `Points`
  (up to four `ContactPoint`s, strongest first, a new array per read); `ContactPoint`, a struct of public fields
  (`Point`, `Normal` from the other side towards this one, `Impulse`, `RelativeVelocity`, `Material`, `OtherMaterial`).
- `Log`: `Trace/Info/Warn/Error`. `Debug`: `DrawLine(from, to, color, duration)`.
- `SceneManager`: `LoadScene(path)`, `CurrentSceneName`. `Application`: `Quit()`, `WindowWidth/Height`, `IsEditor`.
- `Assets`: `Load<T>(path)` for `Prefab`, `Material`, `Mesh`, `AudioClip`, `Texture`, `Font`, `EnvironmentMap`,
  `VisualScript`, `Curve`, `InputActions`, `Mixer`, `PhysicsMaterial`.
- `Curve` (a `.scurve` asset, §6.5): `Evaluate(time)`, a pure node, evaluated by the runtime's copy of the keys.
- `InputActions` (a `.sinput` asset, §7.4): `ActionNames`, the names of its actions in its order (a new array on each
  read, from the runtime's copy of the names, read again when the asset's version changes).
- `Audio` (§9.5, §9.7): `MasterVolume`, `GetBusVolume(bus)`, `SetBusVolume(bus, decibels)`, `IsBusMuted(bus)`,
  `SetBusMuted(bus, muted)` (the game's values of the mix, by bus name in any letter case), `PlayClipAtPoint(clip,
  position, volume, bus)`, `ActiveVoiceCount`, `TransitionTo(snapshot, seconds = 0.5)`, `CurrentSnapshot` and
  `GetBusPeak(bus)` (§9.8). Bindings `Audio_*` in `ScriptBindingsRuntime.cpp`; without an audio engine reads are the
  defaults (no snapshot, a peak of 0) and writes do nothing (`TransitionTo` answers false).
- `Mixer` (a `.smixer` asset, §9.5): `BusCount`, `GetBusName(index)`, `HasBus(bus)`, `GetBusParent(bus)`,
  `GetBusVolume(bus)`, `IsBusMuted(bus)`: the asset's own values, read from the runtime's copy of its buses (`Mixer_Read`,
  `MixerCache`; effects are not copied); every member a pure node.
- `PhysicsMaterial` (a `.sphysmat` asset, §8): `Friction`, `Restitution`, `FrictionCombine`, `RestitutionCombine`
  (`PhysicsMaterialCombine`), `RollingResistance`, `Surface`, `ImpactSound` (`AudioClip`), `ImpactParticles` (`Prefab`):
  read-only properties, so pure nodes, read from the runtime's copy of the values.
- Math: `Vector2/3/4`, `Quaternion`, `Matrix4`, `Color`, `Mathf`, `Random`.
- Attributes: `[SerializeField]`, `[HideInInspector]`, `[Range(min, max)]`, `[Tooltip(text)]`.
- `Strada.Testing`: `Assert` (throws `AssertionException`), `TestReporter` (`Pass`, `Fail`, `Run`, `Finish`; used by
  the feature-test project and CI).

Semantics: components are views (`GetComponent<T>()` makes no copy) whose properties read and write the entity's
component through typed bindings (`<Component>_Get<Field>`/`_Set<Field>`, generated from member pointers); writes go
through the registry, so systems watching a component (physics) see them. While the scene runs, `RigidBodyComponent`
velocities, forces, sleep, `MoveKinematic` and `Teleport` act on the simulated body (bodies of components added that
frame are created first; `Type`, `Mass` and `Layer` rebuild it), `CharacterControllerComponent.Move` (the walking
velocity of the next physics step; call it from `OnFixedUpdate`), `Velocity` (the character's own: falling, jumping),
`IsGrounded`, `GroundNormal` and `Teleport` act on the entity's character (fields change it before the next step;
without a simulated character the reads are a character at rest and `Move` and setting `Velocity` are logged), and
`AudioSourceComponent` playback acts on the entity's sound, brought up to date with the component first. Entity
lookups (`FindByName`, `FindByID`, `Parent`, `Children`, contacts, raycast hits) return the script instance of scripted
entities. `Time.TimeScale` scales script and physics time, not sounds. Asset references are typed (`Mesh`, `Material`,
`Texture`, `AudioClip`, `Font`, `EnvironmentMap` (not `Environment`, which clashes with `System.Environment`),
`Prefab`, `VisualScript`, `Curve`, `InputActions`, `Mixer`, `PhysicsMaterial`). Calls without a running scene
or on missing entities and components log a script error and do nothing; values from scripts are checked (non-finite
numbers and unknown enumerators are rejected and logged, rotations are normalized), and null arguments throw.
`Entity.Instantiate` places the prefab's root at a world position and rotation (keeping its scale) and starts the
instance's scripts before it returns. The project's materials are shared by every scene and the editor, so scripts
change only runtime materials (`Material.Create`, `Clone`), which the running scene owns and releases when it stops.
`Debug.DrawLine` lines last a duration of scaled runtime (0: one rendered frame); Dist builds ignore them. They are
drawn 1.5 points wide with a dark outline of 0.75 points, in pixels at the display's scale
(`SceneRenderOptions::DisplayScale`: the player's window content scale, the editor's UI scale in the game view), so
they read alike on every display.

The application running scenes implements `ScriptHost` and registers it with `ScriptEngine::SetHost`:
`Application.IsEditor`, `Application.Quit` and `SceneManager.LoadScene` (both acted on once the frame's update has
returned), `Strada.Testing.TestReporter` results and unhandled script exceptions go to it. Without a host, requests are
logged and ignored. `Application.WindowWidth/Height` are the running scene's viewport size. `SceneRunner` (Project
module) is that host for the editor's play mode and the game player: it starts a scene's runtime, replaces the scene
after the frame on `SceneManager.LoadScene` (the old scene stops first; loads requested while it stops are dropped; a
scene that cannot be read is logged and the current one keeps running), records `Application.Quit` for its owner and
collects a `TestRunReport` across scene loads (failures = failed checks + script exceptions). With
`SceneRuntimeSettings::RunScripts` and `PlayAudio` off it simulates physics only (the editor's Simulate mode).

### 10.5 Script projects

A project's scripts form a C# project, `<project>/Scripts/<Name>.csproj`, where `<Name>` is the file name of the
project's `ScriptModule` (default `Scripts/Binaries/Game.dll`, namespace `Game`). `ScriptProject` (Project module)
writes it once; afterwards it belongs to the user (packages, extra files), and any .NET 10 SDK or C# editor works with
it. `Scripts/Strada.props`, rewritten whenever its content changes, points the project at this machine's
`Strada.ScriptCore.dll` (referenced, not copied: the engine provides it). New scripts come from a template in
`Scripts/Source/<Class>.cs` or a folder of `Scripts/Source`; a class name that another file of the C# project has
(`<Class>.cs` anywhere outside the build output and hidden folders, which the .NET SDK leaves out too) is refused, as
the class would be declared twice (the editor also refuses a class that a source declares already). Builds run
`dotnet build` (found through `DOTNET_ROOT`, PATH, then the default install locations) as a child process (`Process`,
Platform module, reproc++) into the `ScriptModule`'s directory, without build servers (they would outlive the build
and keep its output open) and with English output, from which errors and warnings are parsed with their files and
positions. A build is out of date when a C# source or project file is newer than the assembly.

### 10.6 Visual scripting

Visual scripts are graph assets (`.sgraph`, §6.3 and §7.3) that a `VisualScriptComponent` (§7.2) runs on its entity,
beside or instead of a C# script. A component runs one graph: behaviours compose through child entities with components
of their own, whose graphs talk through custom events (`Event.Send` runs the `Event.Custom` of its target's graph) and
public variables set per component (`Self` in a child's graph is the child). C++ owns the document (format, validation
and repair, editor, undo, automation) and the scene integration. The graph VM in `Strada.ScriptCore` (namespace
`Strada.VisualScripting`, internal) owns the semantics: the node library, pin resolution and type rules, analysis,
compilation and execution. C++ never derives pins or types itself; it asks the VM through the `VisualScript*` entry
points of `Strada.Interop.Host` (§10.1: `VisualScriptDescribeLibrary`, `VisualScriptAnalyze`, `VisualScriptLoad`,
`VisualScriptCreateInstance`, `VisualScriptInvoke`, ...; `VisualScriptReadSteps` gives the steps the VM's outermost
executions ran since it started, which the profiler reads around each invoke while it records, §4.4;
`VisualScriptAnalyze` and `VisualScriptLoad` also take the
reference bundle of the other graphs a document names, below, a length of 0 meaning none). They are thin adapters that
marshal UTF-8 and answer a status:
Success 0, Failure 1 or NotFound 2. On Success, and only then, the JSON answer goes to the `receive` callback, exactly
once. Without .NET a document still loads, saves and shows in the editor; analysis and running need the runtime.

- **Node library**: the built-in node kinds (events, flow, variables and functions, data, operators) and one node per
  usable member of the scripting API:
  - methods (`Call:`);
  - property and field getters and setters (`Get:`/`Set:`; setters only for static members and reference types);
  - constructors (`New:`);
  - generic methods constrained to components, assets or scripts, through a `TypeArguments` setting, and generic methods
    whose type parameters are unconstrained, typed by their links (below);
  - with the game's scripts loaded, their members marked `[VisualNode]` (below).

  The methods of `VisualScriptComponent` are not nodes (graphs use `Event.Send` and the variable nodes); its `Graph`
  property is. The library is built by reflection over `Strada.ScriptCore` and the game's script classes the first time
  it is needed, once even when several threads ask. It is immutable and is dropped whenever that set of assemblies
  changes. A stored node type resolves in this order: exactly; through the redirect table `NodeRedirects`, for renamed
  or re-signed API members; or as the only overload of its member that has every pin the node uses. Anything else is a
  diagnostic, so an API change never makes a graph unloadable. The library JSON and the analysis JSON (resolved pins and
  types; diagnostics with stable codes and suggestions) are the same bytes in every build, run and culture.
- **Purity**: getters, struct constructors, the methods of the math types (`Mathf`, `Random`, the vectors, `Quaternion`,
  `Matrix4`, `Color`) and methods marked with the internal `[PureNode]` attribute are pure. A method returning `void`
  without `out` parameters never is, and neither is a setter. Pure nodes have no exec pins and are evaluated eagerly
  whenever a consumer needs them, so a pure `Random` rolls again for every consumer. Impure nodes run where the flow
  enters them and keep their outputs. A query member added to the API gets `[PureNode]` in the same change.
- **Disabled nodes** (`Enabled`, §7.3): a node whose `Enabled` is `"Disabled"` is a pass-through, whatever its kind,
  and compiles to one pass-through instruction. Flow entering any of its exec inputs continues at its `Then` output,
  else `Completed`, else `Default`, else its first exec output that is not a Tick output; without one the chain ends
  there (a node of an unknown type has none). Each of its data outputs takes the value of the first data input of the
  same type that no earlier output takes, both in pin order, read when the flow passes (only those inputs, and the pure
  nodes feeding them, are evaluated); the other outputs take their type's default (a disabled `Flow.Timeline`'s `Time`
  takes `NewTime` and its first float curve `Rate`, by this rule, with no case of its own). An unconnected `Target` is
  no value and feeds no output. Passing costs one step; the node never waits and keeps no state. A disabled event starts no
  execution, is no custom-event receiver (`SendEvent` to it returns false with the usual warning) and is no duplicate of
  an enabled event; a disabled latent node makes no function latent. `Function.Entry`, `Function.Return` and pure nodes
  cannot be disabled: they run as usual, with the Warning `DisabledIgnored`. A link from an output of a disabled node
  that has no type is ignored (`UnresolvedWildcard` on the link: its input takes its own value), and a wildcard input of
  an enabled node takes no type from a disabled node's output its constraint refuses (that link is a `TypeMismatch`, and
  the input, unconnected and untyped, an `UnresolvedWildcard` Error on the enabled node). Every diagnostic located on a
  disabled node, or on a link with a disabled node at either end, is a Warning, whatever its code: disabling a broken
  node is how its graph keeps running. The analysis gives a disabled node with an exit the member `PassThrough` (the
  exec output the flow continues at), absent where the chain ends and on events. `"DevelopmentOnly"` runs as
  `"Enabled"` in the VM; the engine disables such nodes in exported games (below).
- **Split pins** (`Split`, §7.3): `Split.Inputs` splits input pins and `Split.Outputs` output pins (a node may have an
  input and an output of one name, such as `Variable.Set`'s `Value`). A split pin is replaced by one pin per field,
  named `<Pin>.<Field>`, in the struct's field declaration order and typed by the field; an entry `<Pin>.<Field>` listed
  after its parent splits that field pin the same way (`Hit`, then `Hit.Point`, gives `Hit.Point.X`, `.Y` and `.Z`). An
  input splits only when its resolved type can be made (a struct whose public fields are all writable), an output only
  when it can be broken (a struct with public fields, or a readonly struct with get-only properties). A wildcard pin
  splits once its type is resolved: a link attaches once every node whose field pin one of its ends names has split; an
  end that names a pin of its own never holds it back, so a field pin of one late-splitting node types a wildcard of
  another. A split input is made from its field pins as a `Make` node makes it; an unlinked field pin without a literal
  takes that field of the value the whole pin would take unconnected (its library default, else its type's default; a
  quaternion field of an all-zero default is the identity). A split output gives its fields as a `Break` node does, read
  each time a node reads them (on an impure node, the fields of the output it stored when it last ran). Split is
  authoritative: a link to or from the split pin itself, and a link or `Inputs` key naming a field of a pin that is not
  split, are `UnknownPin`; a stored literal of a split input is `UnusedInput`; an exact pin name always wins
  (`Flow.Switch`'s case `Case:a.b` is an exec pin, never a field of `Case:a`). A redirect's pin rename applies to split
  paths through the longest renamed prefix (`Value` → `Result` renames `Value.Y` to `Result.Y`), and an overload is
  chosen by the root pins Split lists. A disabled node passes a split input on as the struct its fields make and gives
  the fields of what it passes. An entry that splits nothing is the Warning `UnusedSplit`, and its pin stays whole. Each
  split pin costs one Make or Break step when it is read and allocates nothing for vectors; split pins hold no state.
  The analysis lists the field pins in place of each split pin, each input field pin with the `Default` it takes
  unlinked.
- **Expressions**: an `Expression` node (Math, pure) computes the formula of its `Formula` setting (a setting of Kind
  `Expression`). `ExpressionParser` lexes it and parses it by recursive descent, finding the inputs from the tokens even
  when the formula has errors (an identifier is an input unless `(` follows it, `.` precedes it, it is `true`, `false`,
  `pi` or `tau`, or it is longer than 64 bytes; the reserved pin names never are; at most the first 32, in order of first
  appearance), so the node keeps its pins while its formula is broken. `ExpressionChecker` types the tree with explicit
  widenings by the link rules (an unlinked input is a float; a linked one takes its link's type, which the pin constraint
  `Expression` holds to bool, the numbers and the vectors), and `ExpressionCompiler` emits typed three-address code over
  frame slots: one template slot per distinct constant, constant widenings and casts folded, temporaries reused, `&&`,
  `||` and `?:` as jumps. `ExpressionMachine.Run` runs it in one loop that allocates nothing, so a formula costs one step
  however long it is (ten operators are one step where their ten nodes are ten, at most 20 instructions and no
  allocation). Chains of operators are typed and compiled in loops, so only nesting (at most 64 levels) uses the host's
  stack. Its findings are `InvalidExpression` diagnostics with `Setting` `Formula` and the
  `Span` of the offending bytes; caps: 4096 bytes, 1024 parts, 32 inputs. A live edit with a broken formula keeps the
  last working program.
- **Arrays**: every registered type with values has its array type `T[]` (none of arrays or of ref structs; no
  covariance), which knows its element type and holds one shared empty array. Arrays are never null: null becomes the
  type's empty array in defaults, in values entering slots, in member results, on `SetVariable(null)` and in `SendEvent`
  arguments, so a null array is no aborting error. Equality compares element by element with the element type's equality;
  element reads and writes have a typed path for every kind a slot holds in place and allocate nothing. Arrays may be
  private variables, locals, parameters, custom-event parameters, `Literal` types and type arguments; a public one is
  `NotExposable` until components can store arrays. Literals are JSON arrays of element literals, at most 4096. The
  `Utilities/Array` (Contains, IndexOf, Make, Set, Add, RemoveAt, Insert, Append, beside Length and Get) and
  `Utilities/String` nodes (15, ordinal and culture-invariant) run as intrinsics; arrays they make hold at most 65,536
  elements.
- **Generic methods**: methods whose type parameters are all unconstrained are nodes; their pins follow the anchor rules
  (an input of `T[]` decides `T` as its element type, inputs of `T` join as `Select`'s do), and type resolution closes
  the method once its anchors resolve. Their `TypeArguments` setting is optional (required only when no input decides a
  parameter); methods constrained to exactly `Component`, `Asset` or `Script` keep their required setting.
- **Game members** (`[VisualNode]`, `Strada.VisualNodeAttribute`): the library is built with the game assembly's public
  types (`ScriptRegistry.GetGameTypes`, cleared before the assembly unloads). Marked members (and every public member of a
  marked class or struct) become nodes with `"Game": true`, titled, categorized and described by the attribute, `Pure`
  standing for `[PureNode]` and `FormerNames` becoming redirects; marked enums and the game types the members' signatures
  use become types of graphs. An instance member of a script class takes `Target` as an entity and runs on that entity's
  script instance. A marked member without a node form, an attribute value outside its limits and a type or member whose
  attributes cannot be read become the library's `Notes`, logged once per build; each type's attribute is read once,
  under a guard, so an unreadable one never fails the library.
- **Timelines** (`Flow.Timeline`, latent, the event graph only): `TimelineReader` checks the `Timeline` setting with
  paths, keeping the pins of every track whose name and type are valid; `TimelineTrack` evaluates curves in double
  through the runtime's curve evaluator, `CurveMath` (§6.5: Constant, Linear and cubic Hermite with Catmull-Rom auto
  tangents; keys at one time make a jump). A run is a fiber
  that is never resumed: each advance writes the outputs, fires the event keys it crossed in the direction of play (at
  most 1000, with one Warning), then `Update`, and at the end of a timeline that does not loop ends the run and fires
  `Finished`. Inputs are evaluated one at a time, so `NewTime` is read only by SetTime and `Rate` only by the playing
  inputs. Live edit carries its state (its time clamped to a new `Length`); a disabled timeline passes data by the general
  rule above.
- **Other graphs**: the `Graph` setting (Kind `Graph`, a decimal handle) of `Event.Send` and `Event.Broadcast` (typed
  sends, verified against the target's `Event.Custom` with Warnings only), `Function.Call` (a function of a **function
  library**, a document of `Kind` `Library`, which has no variables and no event graph), `Variable.Get` and
  `Variable.Set` (a public variable of another entity's graph, through `Target`) and `RunsGraph` (the guard) names another
  visual script. `VisualScriptAnalyze` and `VisualScriptLoad` take, beside the document, the **reference bundle**:
  `{ "Self": "<handle>", "Graphs": [{ "Graph", "Name", "Document": { "Form": "Full" or "Interface", ... } or "Error" }] }`,
  every library a document's calls reach sent in full (its own calls' libraries included) and the other referenced
  graphs as their interfaces (kind, variables, events), and for an analysis of a document that names input actions
  `"InputActions": { "Names": [...] }` (the open project's actions) or `{ "Missing": "<why there are none>" }`, which
  the analysis checks the document's action names against (`UnknownAction`, `NoInputActions`, both Warnings: an unknown
  name reads zero at run time); the libraries a call links are analyzed without them, and a bundle without them checks
  nothing. The runtime accepts any bytes there (a malformed bundle counts as empty, a malformed `InputActions` as
  absent). Each library is analyzed as its own document, callees first, without nesting, and kept in the runtime's
  cache (keyed by its text, its name, the node library and what it read); cycles are found from the raw settings before
  any library is analyzed (`LibraryCycle`). A program compiles the functions of every library it links into itself,
  keyed by (graph, function ID), so each graph that calls a library runs its own copy, and live edit resolves frames by
  the same pair. Remote Get and Set check the receiver's current program at run time (by ID, then by name, with the
  node's type) and abort on a mismatch. `Event.Broadcast` snapshots the live instances whose program has the event with
  exactly these parameter types (with `Graph`, only those running it), in creation order, copies its arguments before the
  first delivery and costs a step per instance it examines. Messages name the document the node belongs to
  (`Scripts/Motion.sgraph: Divide (node 7) on Player (123): integer division by zero`), warnings are kept once per (graph,
  node) and instance, and a fiber a live edit cancels inside a library names the library's file.
- **Runtime and environment**: `VisualScriptRuntime` owns the programs (by graph handle), the graph instances (by
  entity) with their fibers, the member invokers and the attached debugger. `Host.VisualScripts` is the engine's
  runtime; `Initialize` and `Shutdown` replace it with an empty one. The VM reaches the engine only through
  `IVisualScriptEnvironment`: the log, entities and their script instances, names and validity, components by runtime
  type, `Time.DeltaTime`, input, and the strongest point of the contact being handled with its other side's surface
  (`Event.CollisionEnter`'s outputs). ScriptCoreTests replace it with a fake engine. Calls of reflected API members are
  not seam calls.
- **Programs and instances**: `Load` analyzes and compiles a document and always stores a program. A program with an
  Error diagnostic is broken and kept for its diagnostics, and its handle runs its last working program.
  - A program lays out each graph as a frame of 24-byte slots. Each slot is an explicit-layout union: booleans, numbers,
    enums and the math structs of up to 16 bytes are held in place, and any other value is an immutable object at offset
    16.
  - Each frame starts from a template holding the type defaults, the unconnected inputs and the initial node states.
  - Members are called through expression-compiled invokers built the first time a node runs.

  An instance holds the variables (their values, then the component's values for public ones), the persistent frame of
  the event graph (outputs of impure nodes; the state of DoOnce, DoN, FlipFlop, Gate and latent nodes) and its fibers.
  `Invoke` dispatches the C# `ScriptEvent`s (the two input action events, §7.4, start the nodes of their kind whose
  `Action` is the event's action). Update sets `Time.DeltaTime`, runs the polled input events (keys, mouse and gamepad
  buttons) in node order and then
  `Event.Update`, then makes one pass over the pending latent nodes. Destroy removes the instance before `Event.Destroy`
  runs.
- **Execution**: each execution keeps its flow stack and call stack on the heap. The interpreter therefore never
  recurses on the host stack, holds no static state and is re-entrant: a graph may start another execution while it
  runs. It runs the node it enters and follows the link of the exec output the node takes. When a chain ends, it
  continues at the top of its flow stack: the next output of a `Sequence`, or the next iteration of a loop, whose entry
  carries the loop's state. When the call's part of the stack is empty, it returns from the function. `Function.Call`
  pushes a call frame (inputs, locals, per-call node state); a pure call pushes one while the plan that needs it runs. A
  node's plan is the pure nodes its inputs depend on, in dependency order, computed once. Executions and frames are
  pooled, so running a graph again allocates nothing unless its nodes make objects; `PerformanceTests` reports the steps
  a second of a tight loop.
- **Suspension**: a latent node (`Delay`, `RetriggerableDelay`, `WaitFrames`, `WaitUntil`, `Tween`, `Interval`, or a
  call of a latent function) suspends the whole execution into a fiber of its instance: its position, flow stack and
  call frames. Nothing pushed on its stack runs before the fiber resumes at the node's continuation, in a later Update
  dispatch, never the one that created it. While they are pending, `Tween` and `Interval` fire their Tick output as new
  executions. An instance keeps its fibers in the order their nodes became pending; a fiber joins that order, keeps its
  place (a `WaitUntil` whose condition is still false) or leaves it in constant time, so an Update's pass over the
  fibers costs time linear in their number. Fibers die with their instance.
- **Budget and depth**: every node evaluation is one step. An outermost execution may take 1,000,000 steps, and its
  nested executions share them: `Event.Send`, `SendEvent` from C#, and an `Invoke` that arrives while a graph runs.
  Resumed fibers and Ticks start a budget of their own. One counter of active executions and call frames stops at 64.
  Endless loops and runaway recursion end with an error instead of hanging or overflowing the stack.
- **Errors**: these are aborting errors:
  - an exception thrown by a member;
  - a component missing from a Target;
  - a null Target (arrays are never null);
  - an index outside its array, or an array a node makes growing past 65,536 elements;
  - integer division or modulo by zero, and division or modulo of the minimum `int` or `long` by -1 (an overflow);
  - running out of budget or depth.

  An aborting error stops its execution and is logged once as `<graph>: <node> (node <id>) on <entity> (<id>):
  <message>`. The `Invoke` then answers Failure, which the scene counts as a script exception; an abort inside a sent
  event stops the sender too. Non-aborting problems are Warnings, logged once per node and instance (a repeated one
  builds no message and allocates nothing): text that `Convert` cannot read, a Send without a receiver, the Interval
  tick cap, a latent node reached during Destroy. Nothing crosses into native code as an exception: any input, including
  a damaged document or one that is not UTF-8 text, gives a status and diagnostics, which a fuzz test of damaged
  documents checks.
- **Live edit and assembly reload**: a working program loaded for a handle replaces the program of its instances as soon
  as no dispatch or execution of the old one runs. The editor sends every edit at once (below), so the instances take it
  in the edit's own call, also while the game is paused and into executions stopped at a breakpoint; only a step of the
  debugger that runs in the old version holds the swap until it ends (a `Pending` receipt).
  - Variables carry over by ID, then by name and type; the others start at their `Value`, else the type default. A
    value the component stores for a variable the edit adds is not given to it: the variable keeps its `Value` until
    that value is edited or a new instance starts (play starts again, or the component's graph changes). A value set
    for a variable before the instance takes the edit (while a step holds the swap) reaches the variable as the instance
    takes it.
  - Node states and stored outputs carry over by node ID while the node's type is unchanged and it stays enabled or
    disabled: disabling a node counts as removing it (a running `Tween` or `Interval` stops without `Completed`), a node
    enabled again starts fresh, and switching between `Enabled` and `DevelopmentOnly` changes nothing. Splitting or
    recombining a pin keeps the node's state and stored outputs.
  - Fibers survive while every position they hold still exists and is enabled; otherwise they are cancelled with one
    Warning (`<graph> on <entity> (<id>): the execution waiting in <title> (node <id>) is cancelled: the edit disabled
    <title> (node <id>)`, or `... the edit removed or changed ...`).

  A broken document leaves the running program alone. Before the game assembly unloads, the runtime destroys the graph
  instances, reduces every program to its name and document and drops the invokers, so nothing holds the collectible
  assembly. Programs compile again from their documents when their handle is next used, logging their errors once if
  they no longer compile (`<graph> was compiled again for the changed game assembly: ...`).
- **Debugging** (the VM, `Strada.VisualScripting.Debugging`): the interpreter is compiled twice, `Run<Untraced>` and
  `Run<Traced>`, and the stop-point check exists only in the traced form, so a runtime without a debugger, and every run a
  debugger does not trace, costs nothing per instruction. The hook (`IVisualScriptDebugger`, `AttachDebugger`; one per
  runtime) decides each run's `DebugMode` when an execution starts, a fiber resumes or a stopped execution resumes:
  `Break` when the instance passes its graph's target and its program has an enabled breakpoint of the node's type (in
  its own document or a library compiled into it), or a Break, a Run to Node or a step is armed for it; `Record` when a
  window watches one of the program's compiled graphs; `Values` while a `graph.trace` recording is armed and the instance
  passes its filter. Traced runs report every instruction (`OnStep`: the node and the exec output the flow left by) and
  every pure evaluation (`OnEvaluated`), and stamp every node they evaluate with the execution's serial.
  - **Stop points** are Enter positions (an instruction about to run, at the exec input the flow enters) and Continue
    positions (a Sequence's next output, a loop's next iteration, a `While`'s condition evaluated again). Breakpoints are
    checked through per-graph tables: a node's own, a pure node's at every consumer whose plan evaluates it (through pure
    calls and a Timeline's input plans too), and a pure node feeding a `While` at each Continue. A breakpoint whose
    compiled node has another type under its ID is inactive, so an edit can never make it stop at a node that took the
    ID. A stopped execution returns `Stopped` with its position unchanged and becomes a **debug fiber** of its instance,
    with its remaining budget and its `Time.DeltaTime` saved; the rest of the dispatch and the frame run, the pending
    pass never advances it, and Continue resumes it at that position with that budget and delta, passing only its own
    first stop point. At most 64 executions are stopped at once (`MaxStops`); beyond, hits are counted as passed.
  - **Chains**: a receiver that `Event.Send` or `Event.Broadcast` runs stops together with its sender, which waits inside
    its Send (`PendingDelivery`). Continuing a chain resumes its root, which finishes the delivery first; a Broadcast goes
    on at its next receiver, and `Count` includes the receiver that stopped. Members die apart: a dead receiver counts as
    delivered, a dead sender leaves its receiver to continue as a root of its own with the chain's saved budget, and a
    dead middle member splits the stop in two.
  - **The snapshot rule**: an execution that cannot be suspended, because it runs nested in engine code that another
    execution called (an `Event.Create` that `Entity.Instantiate` ran, a `SendEvent` from a member a graph called) or
    because its instance is being destroyed, passes its breakpoints: it records a read-only stop (`Passed`) with a
    snapshot of its frames and the frames below it, and runs on.
  - **Steps** follow a chain by its root's serial and compare its logical depth (the call depth summed over the
    senders): Into stops at any depth, Over at the depth it stepped from or above, Out above it. A step whose root
    suspends in a latent node waits (`Waiting`) until the fiber resumes; ticks, timeline outputs and executions nested
    through native code are never followed. Break stops the next stop point of a targeted execution that can stop, of
    any graph or of one graph or library; Run to Node is a one-shot breakpoint removed at the next stop of any kind.
    Every change of the step request changes the stop revision the engine polls.
  - **Conditions** are formulas compiled per program over the node's scope (the enclosing loops' outputs innermost
    first, the locals, the outputs of the one start that begins every chain to the node, the variables; names matched by
    identifier form, so "Max Health" is `Max_Health`), with comparisons of strings and enums with string literals and of
    entities with `None` lowered into bool inputs, so the Expression checker and compiler are unchanged. Conditions on a
    library's nodes read no variables. A condition that does not compile never stops and reports its error (`line L,
    column C: ...`); one that fails at run time stops with "the condition failed: ...". Breakpoints count the positions
    reached and the hits where the condition held; a hit count n stops from the n-th hit on. With break on errors an
    aborting error of a targeted execution makes one read-only error stop at the failing node.
  - **Live edit of stopped executions**: an edit migrates debug fibers by node ID as it migrates fibers (`FindMissing`,
    `MapEntry`, `Move`, which also remaps the start node); a waiting sender migrates only while its event, parameter
    types, `Graph` and `IncludeSelf` are unchanged. Otherwise the execution is cancelled with the fiber's Warning, and its
    chain loses the member. A migrated stop is marked `Migrated`, and its frame shows the literals the edit changed as
    `Edited` with `Before`; values changed elsewhere meanwhile are compared the way the edit carried them (the carry plan,
    below).
  - **Values**: a stop's snapshot copies each chain member's frames (slots and stamps), the instance's variables, its
    flow entries, its position and its start (for a resumed fiber, `Origin`: where the execution that suspended
    started). A value is `Current` when its stamp is the stopped execution's serial, `Earlier` when another execution
    stamped it, `NotComputed` when its frame has stamps but the node none, and `Unknown` when no traced run stamped its
    frame. `Describe` answers one frame of a stop: the node's inputs (at an Enter position, its sources' values, converted
    as the node will load them) and outputs, the start's outputs, the parameters, the locals, every variable, the active
    loops, the other nodes the execution computed there, values changed elsewhere (`Changed`), and formulas evaluated
    read-only in the frame's scope; at most 4,096 values, the computed ones cut first. `Values` reads a live instance
    between frames: its event frame, or the last call frame of a watched function (copied as the call returns), its
    variables and its timelines; nothing is paid while nobody asks. One encoding serves every reply: literals through
    the literal codec, strings cut at 256 bytes, arrays as their first 32 elements nested two levels, entities by ID with
    `Valid`, components and script instances as text with their entity, non-finite numbers and math values as `Text`;
    the managed side never formats values for display.
  - **Flow and recording**: Record runs count each exit (and runs without an exit) of watched graphs in per-graph
    counters; a take walks only what fired and writes a binary block (`VSDT`, version 1) merged by document and function,
    at most 8,192 entries per graph and 65,536 per take (`Dropped` counts the rest). `graph.trace`'s recording keeps the
    steps and values of Values runs, filtered by instance, documents and nodes: a capped list, or a summary per node and
    exit (counts, first and last frame) and per pin (count, first, last, minimum and maximum).
  - **Entry points**: `VisualScriptDebug(request)` takes one JSON command (`Attach`, `Detach`, `Configure`, `State`,
    `Resume` with `Continue`, `StepInto`, `StepOver` or `StepOut`, `Break`, `RunTo`, `CheckCondition`, `Describe`,
    `Values`, `Instances`, `Record`) and answers JSON in PascalCase; every refusal is `{ Error, Code }`, and member names
    that are not valid Unicode are refused, never thrown. `VisualScriptTakeTrace` gives the trace block while the editor's
    debugger is attached (NotFound otherwise). Detaching leaves nothing of the debugger in the runtime.
  - **No nested frame loop**: a stop never runs an event loop inside the stopped execution. The execution becomes a
    debug fiber, the frame finishes, and the editor's frames go on drawing and answering automation while the scene is
    paused; resumes run between frames. So nothing re-enters the editor, ImGui or the scene while an execution is
    stopped, and the budget and depth of a stopped execution continue across its stop.
- **Debugging, the engine side**: `VisualScriptDebugger` (Script module, main thread) is the native facade of
  `VisualScriptDebug`. Its readers in `ScriptEngineInternal` (and the shared strict reader `ReplyReader.h`) refuse a reply
  not of its form, naming the JSON path of the first bad member, and read the binary trace block with every count and
  size checked against the bytes received; they keep literals as sent (decoding them is the editor's formatter's job).
  Only the editor attaches it; the player, exported games and `SceneRunner` runs never do. Every editor edit applies to
  the running game in the call that made it (`Scene::ApplyVisualScriptEdits` in `EditorContext::FinishEdit`), also while
  paused and into stopped executions, running no graph code; entities whose graph could not run start at the next
  update. Play mode has one observer (`PlayModeObserver`), the debug session: it is told of each start, frame, resume and
  stop, can hold play after a frame, refuses frames while an execution is stopped, and is asked to refresh before play
  mode steps or advances and before the play commands report, so commands in one batch see each other's effects.
  `PlayMode::SetPaused(false)` is the single guarded unpause, and `Resume()` continues stopped executions first. A scene
  that a resume or a frame of a paused game loads stays paused as the scene it replaced was, and the frame steps still
  queued carry over to it.
- **The debug session** (`VisualScriptDebugSession`, editor shell, main thread, no ImGui, headless too) attaches the
  runtime's debugger for Play (never Simulate or test runs) before the scene starts and detaches before it stops. After
  each scene frame it advances the flow clock (the sum of the frames' unscaled time steps; it holds while no frame runs),
  takes the trace into per-watch accumulations that each window drains, and reads the state when the stop revision
  changed. A new stop of any kind pauses the scene at the frame's end (the held pause, when the debugger paused it) and
  cancels queued frame steps; both listeners run after that. Continue, steps, Break and Run to Node run graph code between
  frames, never a frame; they then flush destroyed entities and follow scene loads and quits, and resume play only when
  the debugger paused it. Every mutator pushes the configuration (breakpoints, targets, watches) in its own call, so a
  breakpoint set by one command holds for the next; the UI's resumes are queued for the next editor frame's `BeginFrame`,
  which runs before automation, and `EndFrame` runs after play mode's update. The session tells every stop that ends
  without a resume (an edit cancelled it, its entity was destroyed, a scene loaded by what another stop's resume ran, a
  stop made and ended within one frame, told by the stop revision changing in a frame that began with no stop and no
  step), keeping play paused with the pause held. Breakpoints are view state: kept per user and project in the views file
  (never in documents or the undo history); a node deleted, or another type under its ID, loses its breakpoint, and undo
  does not bring it back. A headless editor keeps them in memory only, and a project switch loads the other project's.
  `GraphDebugCommands` (§12) is automation's face of the session: every command first brings the session up to date
  (`Refresh`), breakpoints are checked in full before anything changes, conditions are checked against the running
  program when one is loaded and stored unchecked otherwise, and `graph.trace` runs frames as `play.advance` does,
  recording a summary per node and pin by default.
- **The live channel**: the runtime keeps an always-on, bounded live log: the last 64 receipts and up to 512 distinct
  runtime problems (`Dropped` counts the occurrences of problems that found it full); serials grow with every change, a
  drop included. Problems coalesce by graph, document, node, entity and severity: a recurrence updates the count, the
  latest message and the calls (the innermost 8) and takes a new serial; aborts are recorded once each, warnings once per
  node and instance. A live edit makes one **carry plan** per pair of programs: the variable sources (by ID with the same
  type, then by name and type) and the node-state, Timeline and stored-output slot copies of the event graph, and on
  demand those of function frames; every instance swapped between the same two programs uses it, and the debugger's
  `Changed` compare reads an edit through it. Every load gives a **receipt**: Swapped; Pending, then Swapped with
  `Deferred`, while a running step holds the swap; or Kept for a document that does not compile. A receipt reports the
  variables kept, matched (renamed), reset (TypeChanged or New), added and removed, the states kept and reset (Removed,
  Changed, Disabled), and per instance the waits and stops kept, each cancelled one with the VM's reason, and each
  Timeline's time with the time it was clamped from. `VisualScriptTakeLiveEvents(after)` gives the events after a serial
  (NotFound when none), and `VisualScriptClearLiveEvents` empties the log; serials go on after a clear. Every Timeline
  reader finding carries `Setting` `Timeline`.
  - On the engine side, `VisualScriptEngine::GetLiveLog` gives an immutable revision of the log: the last 64 receipts
    and this play session's runtime problems, ordered by serial. Taking new events (one runtime call, NotFound when none)
    makes a new revision that shares the unchanged entries, so a reader can hold a revision while later edits make newer
    ones. Every send of a program (an edit, an undo or redo, a library a graph calls, a game-assembly change) takes its
    receipt at once, completed with the time, the graph whose change caused it (none for the graph's own document) and
    the outcome: Applied and Pending pass through, and the VM's Kept becomes `KeptLastWorking` exactly when
    `HasWorkingProgram`, else `NotRunning`, with the log's own error text. `ClearLiveLog`, called by play mode when Play
    starts and before the scene starts, empties the engine's and the VM's logs, so a session's problems include those its
    `OnCreate` raised and stay after Stop as the last run. `RunsStoredProgram(graph)` is the one answer to "the game
    runs what is stored": the graph has a working program, the last send compiled, the working program is the stored
    document's program and every reference it was compiled with still holds what was sent. `DescribeLibraryUsers`
    lists the graphs compiled with a library, each with whether its working program was compiled with a version of the
    library (`Compiled`), whether that is the current one (`RunsCurrent`) and how many entities run it (`Instances`);
    a library runs, at the version they hold, through the graphs that are compiled with it and run by an entity.
    `GetWorkingSince` tells when a graph's working program last changed.
  - `graph.live` is the automation face of the log: per graph the game loaded, what runs (`running`: `Current` by
    `RunsStoredProgram`, `LastWorking` when an earlier version that worked runs, `None`), both program revisions, what of
    the stored document does not run (`FindVisualScriptNotRunning`) and the running instances, a library's users, and the
    receipts and the play session's runtime problems, filtered by graph and by serial. `graph.edit` and `graph.set` take
    the log's serial just before writing and list the receipts after it once the write returns; since the edit reaches
    the running scene inside the command, these receipts are final.
- **Dependents keep their analyses**: the runtime keeps the analysis of every loaded document that calls function
  libraries, with what it read of other graphs and, per library, the interface it read: the library's name, kind, error
  count and first error, and per function its ID, name, purity, latency and parameters with their types and former
  names. When a library changes, its dependents are loaded again with unchanged documents. A dependent whose own text,
  name and node library are unchanged, whose other reads are unchanged, and whose libraries give analyses with the same
  interfaces keeps its analysis with the libraries' new analyses linked, and is only compiled. Compiled calls find their
  callees by graph and function ID, so they reach the new library code. Unload and a game-assembly change drop the kept
  analyses. The engine keeps each graph's last sent content text, so a dependent of an edited library is not written
  again: 20 dependents of 200 nodes take a 31.6 ms update in Release (85 ms without the reuse), which a test bounds.
- **Analyses in C++**: `VisualScriptEngine::Analyze` returns a shared, immutable analysis (`Ref<VisualScriptAnalysis
  const>`) and keeps the last eight, one per graph. It reads each new analysis with the one it kept of that graph:
  `VisualScriptAnalysis::Parse(text, previous)` copies every node and diagnostic whose bytes are unchanged and parses only
  the rest. The runtime writes each record deterministically, so equal bytes are equal records. A text not in exactly the
  runtime's form, or any part that does not read, is read whole, so the result and its errors are always those of reading
  the text alone. `JsonScanner` (Serialization) finds the parts without building values. `FindNode` is a binary search of
  `NodeIndex` for parsed analyses and a scan for analyses built by hand. Automation reports the runtime's JSON by parsing
  `VisualScriptAnalysis::Text`. `VisualScriptEngine::GetLastAnalysisTimings` gives an analysis' phases (Text, References,
  Runtime, Parse, Read) and what it reused; `TakeLoadTimings` gives the summed phases of program loads.
- **C# API**: `VisualScriptComponent`'s methods forward to `Host.VisualScripts` with the component's entity. `SendEvent`
  runs the graph's `Event.Custom` of the name at once (nested in, and sharing the budget of, a graph that runs, else
  outermost); its arguments must match the event's parameters in number and convert to their types as links convert
  values (to the same type, a wider number, a base class, a component's entity, a typed asset's handle, `object` or
  `string`; null only for reference types). It returns false with a Warning, running nothing, without a graph instance,
  an event of the name or fitting arguments, and true when the event ended or waits; an aborting error in the event is
  logged and thrown to the caller as `InvalidOperationException` (a C# callback then fails as a script exception).
  `HasVariable`, `GetVariable` and `SetVariable` address a document variable by name (public or private, never a local)
  and convert values only to the same type, a wider number, a base class, a component's entity or `object`;
  `GetVariable` and `SetVariable` throw `InvalidOperationException` without a graph instance, `ArgumentException` for an
  unknown name and `InvalidCastException` for a value that does not convert.
- **Native facade** (`VisualScriptEngine`, Script module): static, main thread only, working while `ScriptEngine` is
  initialized. Its state belongs to `ScriptEngineData`, which it shares with `ScriptEngine` through the module's
  internal `ScriptEngineInternal.h`, so `ScriptEngine::Shutdown` drops it: a record per graph handle, the set of
  entities with a graph instance and the node library. A graph instance thus has an entry in three tables, all keyed by
  entity: the scene's (`VisualScriptInstanceState`), the facade's set and the VM's. `ScriptEngine::GetInstanceCount`
  counts C# and graph instances, so the game assembly cannot change while graphs run (the editor's hot reload waits for
  them). A load or unload of the game assembly, failed ones included, also drops the library, forgets the graph
  instances (the VM destroyed them) and gives every record a new generation.
- **Loading programs** (`VisualScriptEngine::EnsureLoaded`): a graph is absent when its handle is not a registered
  VisualScript asset or its file does not load; nothing is sent then. A record holds the asset object and version whose
  document the runtime has, that document and whether it compiled, the document of the working program the handle's
  instances run (`GetWorkingDocument`; null without one), the name the runtime gives the graph and a generation, so an
  unchanged graph costs a lookup and a few comparisons and the scene asks every update. Documents are compared as
  programs (`HaveSameVisualScriptProgram`): the description, tooltips, ranges, node positions, node comments, collapsed
  states and comment boxes do not count; everything else does, values as written (an integer never equals a float, so
  an int literal or an ID written as `1.0` is a change, and an invalid one), integers by value whether stored signed or
  unsigned, floats bit for bit. A new object or version whose document gives the same program (a save then a refresh, a
  node moved, commented or collapsed, a comment box, the description, a tooltip or a range) sends nothing and only
  updates the record, which keeps that newest document (the asset's own shared object, also as the working program's
  document when the program last sent is the one that runs); a document of another program is sent (`VisualScriptLoad`,
  named by the asset's path, a memory asset's name, else its handle) and the record takes it whatever the outcome, so a
  broken document is sent once. Moving nodes while playing therefore compiles nothing. Its errors are logged once:
  `Visual script '<path>' has 1 error; first: <message> (node <id>)` (`<n> errors`; `(variable <id>)` for a variable,
  nothing for the document), plus `; the previous version keeps running` when the handle has a working program. After a
  game-assembly change the working program may no longer compile: the next document sent goes after the working one,
  sent again first, which tells (not when the document is of the working program itself, whose compilation tells the
  same, so no program is compiled twice; when that document does not compile, the record forgets its working program);
  until then the suffix is not claimed. `VisualScriptEngine::HasWorkingProgram(graph)` asks the same as the suffix: the
  record has a working document compiled for the game assembly the runtime has; right after a game-assembly change that
  is not known until an instance is created or a document is sent. A document that did not compile is sent again after a
  game-assembly change, as it may compile now, so the record always knows which document instances run. The name is
  looked up again only when the asset registry changed (`AssetManager::GetVersion`): a graph moved or renamed is sent
  again under its new path, its working program first, so the runtime's messages name the file that exists. Generations
  come from one engine-wide counter: a record takes a new one when another program is sent or one is sent again after a
  game-assembly change, when its graph comes back after being absent and at every game-assembly change (a new name gives
  none: nothing can start that could not). `CreateInstance` loads first and gives the instance the component's values;
  its Failure (no working program) is a script exception (`ScriptHost::OnScriptException`), as is a Failure of `Invoke`,
  and an `Invoke` answered NotFound forgets the instance. `Analyze` sends a document for analysis and keeps nothing.
  Programs stay with the runtime until `Shutdown`: `VisualScriptUnload` is resolved but has no caller.
- **The project's input actions in analyses** (IA2): `VisualScriptEngine::SetInputActionsSource(owner, source)` gives
  the engine the host's function answering the open project's `InputActions` setting (nothing while no project is open);
  `EditorContext` sets it for its life and the player sets none, so a game's loads never check action names.
  `GetInputActions()` reads the asset's names (or why there are none) when the source answers another asset or the
  registry's version or the data assets' epoch moved, and returns the same object while the names stay the same.
  `Analyze` asks for them only for a document with an action event or a `Strada.Input` call whose library entry has an
  `InputAction` pin (a prefix test per node, then one library lookup per such call), writes them into the bundle and
  records them (`VisualScriptAnalysis::NamesInputActions`, `InputActions`); every other document is analyzed as before,
  with the same bundle and cost. A keeper of an analysis that asked compares the pointer with `GetInputActions()` (the
  graph window's model at each sync, `graph.edit`'s stored analyses) and analyzes again when it differs: a renamed or
  removed action, an asset gone, another project. The library marks the names: `InputAction` on the `Action` setting of
  the two events and on the parameters marked `[InputActionName]` (internal to the scripting API).
- **Graphs that use other graphs**: every send and `Analyze(document, self)` pass the reference bundle.
  `CollectVisualScriptReferences` (`Asset/VisualScriptReferences.h`) lists the graphs a document's six cross-graph kinds
  name (disabled nodes included), each with its strongest use (`Verification` for typed sends, `Interface` for remote
  variables and `RunsGraph`, `Library` for calls); the closure is walked breadth-first, libraries expanded and the others
  leaves, and at most `VisualScriptEngine::MaxReferencedGraphs` (256) graphs are looked up, the others listed as absent
  ("more than 256 visual scripts are referenced"). No bundle is sent when no node names a graph. Its texts are kept per
  graph handle and shared document (`ScriptEngineData::VisualScriptReferenceTexts`) and concatenated around them, so an
  unchanged neighbourhood serializes nothing; while development nodes are off, every bundled document has its
  `DevelopmentOnly` nodes disabled. A record keeps the references its document and its working program were compiled
  with and is sent again when a library it links changes its program, a graph it reads changes its kind or variables,
  or any entry, a typed send's target included, appears, disappears or is renamed (renames are checked when the registry
  changed): so editing a library recompiles every graph that calls it in the edit's own call while playing (20
  dependents of 200 nodes in 31.6 ms in Release, as they keep their analyses: below), a moved or renamed graph recompiles
  its dependents,
  whose messages then name the file that exists, and another graph's events or layout never do. A library runs on no
  entity: `EnsureLoaded` and `CreateInstance` refuse one (`<name> is a function library: graphs call its functions; it
  runs on no entity`, which the scene logs as the reason an entity runs no visual script), and `HasWorkingProgram` of a
  library answers whether some graph's working program was compiled with it present. `Analyze` fills
  `VisualScriptAnalysis::Graphs` with the graphs it read (handle and shared document, null when absent, none past the
  cap), which tells the editor when an analysis went stale. `MakeVisualScriptInterface` gives the interface form other
  graphs read, and `HaveSameVisualScriptLinkInterface` compares two.
- **Development-only nodes**: the player calls `VisualScriptEngine::SetDevelopmentNodesEnabled` (false for an exported
  game) right after `ScriptEngine::Init`, so exported games skip them, while the editor and `StradaRuntime --project`
  run them. While the flag is off, every document the runtime receives (new documents, re-sends after a game-assembly
  change or under a new name, the working document sent first) has its `DevelopmentOnly` nodes `Disabled`
  (`DisableDevelopmentOnlyNodes`), while the records and `GetWorkingDocument` keep the asset's documents; `Analyze` never
  rewrites. So a `DevelopmentOnly` node of an unknown type breaks its graph in the editor and only warns in an exported
  game. The flag lives in the engine's state: true by default, reset by `ScriptEngine::Shutdown`, set before any graph
  loads.
- **Library in C++** (`VisualScriptLibrary`, Script module): the runtime's library JSON read into types (ID, kind, field
  type, accepted asset type, enumerators, struct fields, base, element, implicit conversions) and node kinds (pins with
  their types, defaults, roles and wildcard constraints; settings), with a category tree and a search ignoring ASCII
  case; every entry keeps its JSON, which automation returns as it is. `VisualScriptEngine::GetLibrary` reads it the
  first time it is asked for (never in `Init`): null without the runtime, or when the runtime cannot describe it or the
  JSON is not of its form, which is logged once until the next game-assembly change. Lookups return pointers.
  `CanConnect(output, input)` allows the links analysis allows: the same type; `exec` only to `exec` or to a `Reroute`'s
  wildcards; a wildcard input takes the types its constraint accepts, and a wildcard output goes to an input of such a
  type or to another wildcard; then the output type's implicit conversions, and any value to `string`. A test compares
  it with analysis over every pair of 19 output and 22 input pins. `MakeEnumLookup` describes the enum types of public
  variables from the library it keeps, never calling into .NET, and is empty without a library; `FindEnum` asks the
  library the engine has, reading it when it has none yet, and `GetLibraryState` tells whether it is read. The
  inspector (§11) gets its lookup from `VisualScriptEnumLookupSource` (StradaEditorCore, updated by `EditorLayer` every
  frame): one that asks `FindEnum`, so the library is read only once a variable that may be an enum is shown (reading
  it takes a moment, and every game-assembly change drops it), and an empty one while the runtime is not running or its
  library cannot be read. `VisualScriptAnalysis` reads an analysis: each node's resolution (`CanonicalType` and
  `PinRenames` for those resolved inexactly, and for an exact call that names a former parameter name, `PassThrough` for
  a disabled node) and the diagnostics, with their `Setting`, `Span` (`[start, end)`, UTF-8 byte offsets into that
  setting's text) and `Graph`, written in that order after `Variable`. The library JSON also carries the pin constraint
  `Expression`, pins' `ArrayOf`, nodes' `Game`, the setting kinds `Expression`, `Timeline` and `Graph`, and the top-level
  `Notes` (`VisualScriptLibrary::GetNotes`: the `[VisualNode]` members without a node, and why). Nothing in C++ derives a
  severity from a code: the editor reads each diagnostic's `Severity`.
- **Scene** (`Scene/SceneVisualScripting.cpp`): graph instances follow the lifecycle of §10.3.
  - Start: C# scripts run when the game assembly was loaded as the scene started (Script components without it log
    `Scene '<name>' runs without C# scripts: no game assembly is loaded` when the scene runs graphs); graphs need no
    game assembly. Simulate runs no graph.
  - Every update first follows the components, running no managed code while it walks them: each distinct graph among
    them is loaded (`EnsureLoaded`, in handle order); new components get instances; a changed `Graph` ends the instance
    (`Event.Destroy`) and starts the new graph; a component without an instance tries again when its graph's generation
    changed (a graph fixed or added while playing starts its entities); removed components and entities lose their
    instances (in handle order, an entity's C# script before its graph). A component without a graph runs nothing; one
    whose graph is absent or cannot run logs `Entity '<name>' runs no visual script: <reason>`.
  - Creation is two-phase across both kinds: every C# and graph instance of a pass exists before the first `OnCreate`,
    then `OnCreate` runs for the new C# instances and `Event.Create` for the new graph instances. Prefab instances
    (`StartScriptsOf`) start both kinds before `Entity.Instantiate` returns.
  - Order: `Update` and `FixedUpdate` reach every C# instance, then every graph instance (in the order of their
    components, as the pass starts). A contact reaches the first entity's C# script, then its graph, then the second
    entity's; a click each receiver's C# script, then its graph, nearest receiver first; a destroyed entity's C# script,
    then its graph, before its children.
  - Stop: removed instances end, then `OnDestroy` runs for every C# instance and `Event.Destroy` for every graph
    instance before both kinds are destroyed; `~Scene` stops them too, and entities created while stopping start
    nothing.
- **Variables**: a component stores values by variable name in the script field format (`Variables`). A value stored
  under a former name applies to the renamed variable (`NormalizeVisualScriptOverrides`: the value under the current
  name wins, else the one under the most recent former name), and instances get the normalized values. While a scene
  runs, `Scene::ReadVisualScriptVariables` copies the values an instance's public variables hold into its component and
  `Scene::ApplyVisualScriptVariableChanges` gives instances the stored values that differ from those they were given or
  read, at the editor's sites for script fields (§10.3): the inspector, `EditorOperations::SetComponentFields`,
  `EditorContext::FinishEdit` (every edit, undo and redo) and the automation reads (`entity.get`, `component.get`).
  Both use the names of the program the instance runs (the working document, which may be older than the asset's): a
  value for a variable that program lacks is not given and waits. When a live edit swaps the program (a new
  generation, seen by the next update or read), the values the instance was given are renamed as its variables were
  (`FormerNames`), so values given before are not given again over the values the variables carried, while values edited
  meanwhile, those of variables the edit adds included, are given before the instance runs.
- **Games and tools**: `Project::UsesVisualScripts` (the project's asset directory, open in the `AssetManager`,
  registers a visual script file) makes the player start .NET without a game assembly and the exporter ship the
  scripting API (§13). Automation reads, checks and writes documents and lists the library with the `graph.*` commands
  (§12, [Automation.md](Automation.md)); the editor's play mode needs nothing more, as the editor starts the scripting
  runtime when it starts (§4.1).

## 11. Editor (`StradaEditor`)

- ImGui with docking and multi-viewports; panels: Scene Hierarchy, Inspector, Content Browser, Scene View, Game View,
  Renderer Info, Console, Scene Settings, Project Settings, Input Actions, Mixer; a menu bar and a toolbar with the play controls (Play/Stop,
  Pause, Step and Simulate buttons with drawn icons). The menu bar is `EditorTitleBar`: ImGui's main menu bar, and on
  Windows, where the editor's window leaves its title bar to the application (§6.1), also that title bar: higher, with the
  Strada icon before the menus and the window's title, the status and drawn minimize, maximize or restore and close
  buttons after them. The icon is an image made at the very size it is drawn at (`EditorWindowIcon`, which averages
  the embedded icon down: ImGui's images have no smaller levels to sample). The bar draws
  the buttons from the window's state and tells the window which of its parts is what (`EditorTitleBar::FindArea`): the
  menus are the editor's, another ImGui window over the bar has what it covers, an open popup takes the click that
  closes it, and the rest is the caption. View > Reset Layout restores the default docking layout. The look
  comes from the engine's `ImGuiLayer`: the embedded Roboto as the UI font, Cousine (fixed-width) for the console, and
  a dark slate theme with a blue accent. Windows get the Strada icon (`Strada/Resources/Icons`, drawn by
  `Tools/GenerateIcon.py`): embedded as the window icon by the engine and, on Windows, as the executables' icon.
- Scene Hierarchy: the entity tree; click selects, Ctrl toggles, Shift selects the displayed range; dragging rows
  reparents (onto a row) or reorders (onto its upper or lower edge) the selection as one undo step
  (`MoveEntitiesCommand`); F2 or the context menu renames; double-click frames the entity in the viewport; context
  menus create preset entities (empty, primitives, 3D text, a sprite, camera, lights, a global post-process volume with
  a 10 m box for when it is made local, particle effects, a character controller, audio, UI), duplicate and delete; UI
  elements (Panel, Image, Text, Button) go into a canvas: the parent's, else under a parent a new canvas there, else the
  scene's first canvas or a new one, created with the element as one undo step; a search field filters by name.
  Selections made elsewhere reveal and scroll to the primary entity.
- Inspector: generated from the component registry's field descriptors (no per-component UI code): numbers clamp to
  the field ranges (a range that holds no value of the field's type, which script and visual script authors can write,
  is left out), colors are picked in sRGB and stored linear, quaternions are edited as Euler angles (kept stable
  while dragging), enums are combos, fields with a visibility condition (`VisibleWhen`: a material's alpha cutoff in
  the Mask mode, the perspective or orthographic settings of a camera, what a joint type or particle shape uses) show
  only while it holds, entity references accept dropped entities, and a field's switch (a post-process volume's
  override) is a check box in front of its name, the value disabled while it is off; fields that UI of their own edits
  (`HiddenInEditors`: a Mesh component's material overrides) get no row. Asset fields (in components, materials,
  settings and script fields) show the asset's path in the asset directory with its extension (`builtin://<Name>` for
  built-ins, the name of materials and textures imported with a model, `None` when empty) in a box that opens a
  searchable picker of the assets of the field's type when clicked and takes assets dropped from the content browser,
  followed by `...` (the picker) and `x` (clears it). Paths that do not fit lose their folders first and then stand as
  their name (built-ins too: `Plane`); the files of a set, whose names repeat a folder's
  (`wooden_crate_01/textures/wooden_crate_01_diff_1k.jpg`), keep what tells them apart (`...diff_1k.jpg`, or its last
  words while they say more than a resolution); other names lose their middle, the end keeping the extension and
  starting at a word when one fits (`dry_decay_....smat`); the tooltip has all of it (`UI::FitPath`). The picker is as
  wide as its longest path within the screen (rows that still do not fit are shortened the same way and show the path in
  their tooltips), opens on the current asset, filters as the user types, and Up and Down move through its rows, which
  Enter takes; it draws only the rows in view, so long lists stay quick. In the inspector the box and the picker's rows
  start with a small image of the asset (textures, materials on a sphere, meshes, prefabs, environments, fonts) from
  the editor's one set of `AssetThumbnails`, which the content browser shares, so each image is made once; the images
  are shown larger in the boxes' tooltips. With several entities selected it shows the components they
  share, flags fields whose values differ, and an edit sets only the edited value or vector component on every entity.
  One widget interaction (a drag, typing into a field) is one undo step. Add Component opens a menu with a search field
  above a submenu per component category (`ComponentTraits::Category`: Rendering, Physics, Audio, Scripting, UI; the
  Core ones are on every entity); typing lists the matching components instead (names starting with the text first) with
  their categories, and Enter adds the first (`inspector.add-component-menu` opens the menu for agents, with search text
  or a category's submenu). Components can also be reset, copied, pasted as JSON and removed. Below
  the fields of a Mesh component, each material slot of its mesh gets a section (`MaterialSlots`): a header with the
  material on a sphere, its name, the slot and where the material comes from; an asset field of the material the slot
  renders with (the component's override, else the mesh's own material, else the default one), whose picker lists the
  materials and whose `x` gives the slot the mesh's own material back (`MaterialSlots::Clear`), each one undo step; then
  every parameter of that material. Edits change the material file as in the material inspector (one undo step per
  interaction) and show at once in the views and thumbnails; built-in and model-imported materials show their values
  disabled with a Create Material button that writes an editable copy to `Assets/Materials` and gives it to the slot
  (one undo step). Below the `Graph` of VisualScript components come the public variables of the graph
  (`DescribeVisualScriptVariables`), edited like script fields when every selected entity runs the same graph and it
  loads (nothing shows otherwise): one undo step per interaction writes each entity's whole `Variables` map, with values
  stored under a variable's former name moved to its current one (`NormalizeVisualScriptOverrides`). While playing it
  shows the values the running graph holds, and edits reach the graph at once, as for script fields (§10.6). Variables
  of enum types are listed read-only with the reason until the scripting runtime describes its enums
  (`InspectorPanel::SetVisualScriptEnumLookup`, given by `EditorLayer` from `VisualScriptEnumLookupSource`: a lookup
  that reads the runtime's node library when first asked about a type, empty without the runtime), and so are those of
  types components cannot set (arrays among them: "arrays cannot be set on components"). A component whose graph is a
  function library shows that it runs on no entity instead of variables. C# scripts and visual
  scripts dropped onto the inspector run on the selected entities. Without selected entities the inspector shows the
  asset selected in the content browser (`EditorContext::SelectAsset`; selecting entities replaces it): its type and
  reference, the parameters of materials with a preview (editable for material files; Create Material copies built-in
  and mesh-imported ones), a texture preview, mesh statistics with the mesh's materials, what a visual script holds
  (read-only: its description, the numbers of nodes and links of all its graphs, the events it uses, its variables with
  their types, values and whether they are public, and the signatures of its functions, marked when pure, under an Open
  button for its graph window, §11.1; a function library says so, and every visual script lists the scripts that use
  it, "Used by"), and the details of other asset types. VisualScript components get Open Visual
  Script, or Create Visual Script when they run no graph (§11.1). Showing an asset never writes its file. The Transform
  of UI elements, which their `UIRect` places, starts folded and is shown read-only, with a note. Every field table's
  name column starts as wide as its names need, switches and indentation included (30% to 60% of the panel, at most 50%
  beside asset paths; users can resize it); names that still do not fit end in an ellipsis and show in full in their
  tooltips. The default layout gives the inspector about a quarter of the window. Data assets show the body of the data
  asset editor (§6.4): their type's field table, or the drawer of their type.
- Arrays in field tables (`FieldEditor`, in the inspector, the data asset editor and the settings panels): an array of
  plain values (numbers, strings, assets, ...) is a row of widgets per element with `-`, then `+`. An array of reflected
  structs is a header row that folds away (its count, `2 of at most 16 elements`), then a folding row per element,
  `<index>: <title>` (the title is the element's first string field, such as its name, when it is not empty; for a
  struct without a string field, its first two numbers or choices that the element shows, a number with its name and
  a choice by its value, as a curve's keys show `0: Time 0.4, Value 1.1` and a mixer's effects `1: Low Pass, Frequency
  1000`, a field its visibility condition hides for the element being skipped; `Element <index>` otherwise), starting folded, with a menu button (Move Up, Move Down, Duplicate, Insert Above, Remove;
  also on a right click of the row) and, unfolded, the element's fields as nested rows, where visibility conditions and
  switches are those of the element's own struct; Add Element after the last appends a default element (each field's
  default). A list edit carries each element row's fold state to where it moves the element (ImGui keeps the states by
  index, which would leave them behind), and an element it puts in place (added, inserted, duplicated) opens; rows
  inside an element (a nested struct, an array of the element) keep their states by index. At the field's `MaxCount`
  the buttons that add are disabled and say why. Every change is one undo step (a drag inside an element, one
  interaction, merges as everywhere) that patches by index, so editing several selected objects (entities whose
  components hold arrays, splines among them) keeps each object's own elements: `FieldChange::Path` is a list of
  `FieldPathStep { Field, Element }` (the field names from the edited object down to the changed value, each with the
  index of the element the path goes into when the field is an array; an element of an array of plain values is changed
  by a path whose last step names it), and `FieldChange::ArrayEdit` (`FieldArrayEdit { Kind, Index, Target }`) changes
  the array the path ends at as a list: `Insert` puts `Value` at `Index` (`FieldArrayEdit::End`, or any index at or past
  the array's size, appends), `Remove` removes `Index`, `Move` takes the element at `Index` out and puts it back at
  `Target`, `Duplicate` puts a copy of that object's own element at `Index` after it. `MakePatch(values)` applies the
  change to the object whose fields `values` holds and gives the whole new value of the top-level field: an element
  edit, a removal, a move or a duplication at an index that object's array does not have leaves its array as it is, and
  an insertion past its end appends. With several objects selected, the rows show the first object's elements, and when
  the objects' arrays differ the whole array is flagged as differing, as a nested struct is (the selection's differing
  fields are named at the top level); an insertion into an object already at the field's `MaxCount` is refused with the
  field table's message and changes nothing. Values drawn without a table row (`DrawInlineValue`, `DrawValuePopup`)
  report an array's whole new value. Automation sets an array field whole (`component.set`, `<domain>.set`): what the
  editor's index edits produce, given as the array they leave. The Project Settings panel's layer list is such an array
  (it adds no layer past 16, the most a project takes): a layer it adds is named `Layer <n>` (layer names are unique and
  not empty), and removing a layer drops the ignored collision pairs that name an index the shorter list lacks. Layers
  are indices, which rigid bodies and character controllers in every scene hold as the pairs do, so nothing is
  renumbered: removing a layer before the last moves the names after it down onto the bodies and pairs of the lower
  indices. The collision matrix stays as it is.
- Struct drawers: a reflected struct type (`StructTraits<T>::Name`) may have a drawer of its own, used wherever the
  struct appears (a field, a nested struct, an array element) in every `FieldEditor` (the inspector, Project Settings,
  Scene Settings, the Build Game dialog, the visual script literal editor), in the value column of the struct's row (an
  open element's drawer gets a row of its own below the element's). The editor's struct drawers are one list in
  `Editor/UI/FieldStructDrawers.cpp` (`GetFieldStructDrawers`), where a type's track adds its line, which every
  `FieldEditor` installs when it is constructed; `FieldEditor::SetStructDrawer(structName, drawer)` adds or replaces one
  for that field editor only (tests, a panel's own), and an empty drawer removes it. A drawer gets a
  `FieldStructDrawContext` (the field's descriptor, its value, whether the selection's values differ, whether it is
  editable (false inside `ImGui::BeginDisabled`), and the field editor, for nested asset pickers) and returns, when the
  user changed something, a `FieldStructEdit`: the struct's whole new value, the step's name in the undo history and a
  merge key, empty and 0 meaning the caller's own, as a data asset drawer's are (§6.4). The field editor reports it as a
  change at the struct's path (`FieldChange` carries the description and key; the inspector, the material and scene
  settings panels and the data asset editor use a non-zero key in place of their interaction's, and the data asset
  editor the description, as its step's name), so a curve embedded in other data is edited with the curve editor and
  undone like any field.
- Content Browser: a folder tree with two roots, the open project's asset directory and its C# scripts
  (`Scripts/Source`, `ScriptBrowsing`), beside the displayed folder as tiles (folders, then assets or scripts, then
  files that are not assets, such as a glTF scene's buffers) labeled with their file names on two lines (a long name
  keeps its extension in view, `LongMat...smat`, and the maps of a texture set what tells them apart, `...diff_1k.jpg`),
  breadcrumbs, a tile size slider, and a search over the root's paths.
  Textures show small copies of themselves: decoded and halved on a worker thread, one texture at a time, until no side
  is longer than 256 pixels (`AssetThumbnails::ShrinkTexture`, colors averaged in linear light), so showing a texture
  never decodes, mip-maps or uploads the whole texture on the main thread. Materials (on a sphere), meshes, prefabs and
  environments show thumbnails that `AssetThumbnails` renders offscreen, one per frame; prefabs show their particles a
  third of a second after they start (effects alone over a dark backdrop), and those that draw nothing (a light alone)
  their card. Fonts write "Aa" with their own glyphs (rasterized from the text renderer's distance-field atlas); audio
  clips show their waveform and length, decoded on a worker thread one clip at a time (`ComputeAudioWaveform`) and kept.
  What a preview loads for itself alone (the asset, and the meshes, materials and textures a rendered one needs) is
  unloaded once it is made, so browsing an asset library keeps in memory, and on the GPU, only what the scene and the
  panels use. A preview is made again when its asset in use is reloaded or (materials) edited, or when a refresh finds the
  file of an asset it let go changed. The editor keeps one set of these previews for the
  content browser and the inspector (`EditorLayer`; another project starts a new one) and updates it once per frame
  before the panels draw, so an image is made once, whichever panel shows it, and the images it replaces are in no
  frame's draw data. Files without a picture get cards of the slate theme with a glyph and a band
  naming their type, drawn with ImDrawList (a globe for scenes, a cube for prefabs, a wireframe for meshes, a shaded
  ball for materials, a picture for textures, a sunrise for environments, three linked nodes for visual scripts, a
  speaker until a waveform is ready, "Aa" in the editor's font for unreadable fonts, a C# document for scripts, a page
  with the extension for other files);
  missing files get a red badge and a red label, files that do not load or decode an amber badge (the tooltip says
  why; `AssetThumbnails::GetLoadError`), and audio tooltips the clip's length, rate and channels. Click selects (on
  release, so dragging keeps the selection the drop target belongs to; the inspector shows assets): the selected tile
  gets an outline in the theme's accent and its name an accent plate, a hovered one a lighter card and outline.
  Double-click opens folders, scenes, visual scripts (in their graph windows, §11.1), scripts and other files (with the
  desktop's application, `Desktop::Open`), and visual scripts' context menus and tooltips offer the same; the folder
  tree opens and scrolls to the displayed folder. F2 renames, Delete deletes after a confirmation. Assets,
  other files and folders drag onto folder tiles, tree nodes and breadcrumbs to move (handles stay valid; a model
  moving to another folder takes the files it reads along to the same paths relative to it, `EditorOperations::
  MoveAsset`), scripts and script folders onto script folders; assets drag into asset fields, the hierarchy (meshes
  become entities, as children when dropped onto a row) and the viewport (meshes become entities on the ground under
  the cursor, materials go to every submesh of the mesh under the cursor, environments to the first sky light or a new
  one, scenes open); scripts drag onto hierarchy rows, the inspector and the viewport (the entity under the cursor),
  which give the entities a Script component of the class the file defines among the loaded classes as one undo step
  (`AssetDrops::AddScript`, `EditorOperations::SetScriptClass`; the console says when the scripts are not built yet).
  Visual scripts drag onto the same places, without the scripting runtime: entities without a VisualScript component
  get one running the graph and the others change graph (keeping the values they give its variables), as one undo step,
  none when every entity runs the graph already, and nothing changes when an entity is gone
  (`AssetDrops::AddVisualScript`, `EditorOperations::SetVisualScriptGraph`; a drop on the viewport's empty space says
  to drop it onto an entity; a function library is refused, `AssetDrops::AddVisualScript` saying it runs on no entity).
  Context menus (and the toolbar's "+") create folders, materials, visual scripts (`New Visual Script.sgraph`, numbered
  when taken, from the Events template: an `Event.Create` and an `Event.Update` node; `EditorOperations::CreateVisualScript`,
  selected and ready to be named), visual script libraries (`New Visual Script Library.sgraph`, from the Library template)
  and C# scripts (the New Script dialog,
  in the displayed script folder; the browser then shows the script), import files (native multi-select dialog),
  refresh, rename, delete, copy references or paths and show items in the file manager; deleting a visual script other
  scripts use names them in the confirmation ("Used by Player.sgraph, Enemy.sgraph (5 nodes): they will show errors.").
  File operations are not undoable. Files and folders dropped onto any of the editor's windows from the OS (`WindowFileDropEvent`, forwarded
  from the platform windows of floating panels too; `asset.drop` does the same for agents) are imported by
  `ProjectImport` into the folder under the cursor, else the displayed ones: assets into the asset folder (taken names
  get a number), models with the files they read at the
  same paths relative to them (`MeshImporter::FindExternalFiles`; in a new folder named after the model when those
  names are taken), C# sources into the scripts folder (creating the C# project when needed) unless their file name or
  a type they declare is taken in the scripts, where the C# project would no longer build, and folders whole but for
  hidden files, links and C# build output: their C# sources go to the scripts (all of a folder that holds no assets)
  and the rest to the assets. Other files come only with a model, and what is in a dropped folder only with it. Dropped
  paths are compared with each other and with the project's folders spelled as stored (`FileSystem::GetStoredSpelling`),
  so the project's files and folders in another letter case are still the project's. What came in is selected, and a
  notice under the toolbar says what did not (with a button that shows the console). Files that other applications
  add, change or delete are picked up when the editor gets the focus back (`AssetAutoRefresh`; one of its windows,
  floating panels included; after a running game export, which copies the assets) or with Refresh, and their
  thumbnails are made again; a preview that failed is tried again once a refresh has found its file changed.
- Scene Settings: the scene name and the physics and renderer settings, generated the same way.
- Renderer Info: the device (adapter, type, Vulkan version, driver, GPU timing support, validation and its error
  count), the frame (average and slowest frame time with a graph over 240 frames, frame count, back buffer), and what
  the scene view's and the game view's renderers drew in their last frames side by side (GPU time, draw calls, mesh
  batches and instances, the shadow-map share, triangles, culled submeshes, lights and culled lights, dropped shadows,
  sprite, text and UI quads).
- Input Actions (`InputActionsPanel`, docked below the scene view with the content browser; Docs/Editor.md "Input
  Actions panel"): an InputActions asset (§7.4) as a table, the project's (`ProjectSettings::InputActions`) unless its
  asset field or the content browser's Open chose another. A row per action (name, type, add binding, remove, and the
  running scene's value while the editor plays that asset: `InputActionSet::Find` by name on
  `Scene::GetInputActions()`, never `Resolve`, which logs) and below it a row per binding (controls as text, capture
  buttons for the whole binding and each part of a composite, the actions it conflicts with by
  `FindInputBindingConflicts`, worked out again only when the map changes, remove); the selected action's fields
  below in the field editor. Every edit is one `SetDataAssetCommand` step of the whole `Actions` array: text fields
  commit once (left or Enter; Escape keeps the asset's), and discrete edits (text, type, buttons, captures) take keys
  of their own (`INAD` plus a counter) while field editor interactions merge under the panel's `EditSession`
  (`INAC`). The editor never writes the binding table of §7.4's "Rebinding" (the player's), only the asset. A capture
  (`InputBindingCapture`, free of ImGui) takes the next control: keys and mouse buttons from the windows' events,
  which `GameInputRouter::SetCaptureFilter` hands it before ImGui and the game (the physical key; a press it takes,
  and that key's repeats, reach neither), a mouse button only over the prompt it draws (a press elsewhere ends the
  capture and goes on), and gamepads (and what automation injects into `Input`) through `InputControlCapture` with
  the binding's gamepad; Escape and `InputControlCapture::Timeout` of frame time end it. It listens only while the
  panel draws: the layer calls `OnHidden` when it does not, and the filter lets every event go once a frame passed
  without a draw.
- Menus and shortcuts: File (New Scene Ctrl+N, Open Scene Ctrl+O, Save Ctrl+S, Save As Ctrl+Shift+S) with native
  file dialogs, Edit (Undo Ctrl+Z, Redo Ctrl+Y / Ctrl+Shift+Z, Duplicate Ctrl+D, Delete, Select All Ctrl+A), Entity
  (create presets at the scene view's focal point: Create Empty, then the submenus 3D Object (the primitives and 3D
  Text, world-space text), 2D Object (Sprite), Rendering (Camera, the lights and Post Process Volume), Effects (Particle
  System, Fire, Smoke, Sparks), Physics (Character Controller), Audio (Audio Source, Audio Listener) and UI (Canvas,
  Panel, Image, Text, Button), which the hierarchy's create button and Create Child share; `EntityPresets::GetMenu`;
  `editor.entity-menu` opens it for agents), View (the panels, Visual Scripts: the open graph windows, Reset Layout),
  Play (Play/Stop, Simulate, Pause, Step). While a graph window is the editing window focused last, the Edit menu and
  its keys act on its graph (§11.1). Shortcuts are global unless a text field is being edited or the focused window
  claims them (a graph window its editing keys, the content browser Delete, Ctrl+Backspace and F2). The editing keys and
  the Edit menu's editing items live in `EditorEditingKeys` (`HandleEditorEditingKeys`, `DrawEditorEditingMenuItems`,
  and Undo and Redo through `HandleEditorHistoryKeys`, `UndoEditorStep` and `RedoEditorStep`), which the editor and the
  tests both run; while a modal popup is open (a delete confirmation, the unsaved-changes prompt) no editing key runs,
  Undo and Redo included, so nothing changes what the dialog acts on.
  Opening or creating a scene, and closing the editor, ask to save unsaved changes first; the window title shows the
  scene name and a `*` while it has unsaved changes.
- Scene View (`ViewportPanel`): the scene (the edited one, or the running copy while playing) through the editor camera
  (fly/orbit/pan/zoom/focus). A scene the editor opens shows from where its primary camera looks, orbiting the middle of
  its meshes, sprites and text ahead of it, or, without a camera, with those framed (`FrameScene`); opening the scene
  shown again keeps the view. Its toolbar picks the tool, Select (no gizmo), Move, Rotate or Scale (Q/W/E/R), the gizmo's
  orientation (local/world, X), snapping (Ctrl toggles it while dragging) and its steps, and the overlays
  (`SceneViewOverlays`, also `viewport.overlays`): ground grid (G), selection outline, light and camera gizmos
  (clickable billboard icons per kind of light and for cameras), camera frustums, light shapes (directional light rays
  kept at a constant size on screen, point light ranges, spot light cones with their inner cone), colliders and
  characters (the capsules of character controllers), joints and post-process volumes (the boxes of local volumes with
  the rounded outlines where their blend distances end), each for the selected entities or every entity, the lines
  scripts draw, the UI canvases, and, off by default, the contacts of a playing scene
  (`ViewportOverlays::AppendContactLines`: a cross at each point of the events of the scene's last frame,
  `Scene::GetContactEvents`, and a line `ContactNormalLength` (0.3 m) long from it along its normal, drawn over
  everything; the normal is shown towards the body the contact pushes, the dynamic one when only one of the two is, so
  it points out of the surface a body lands on, and from the event's `First` otherwise; a contact shows for the frame it
  began in, and stays while play is paused). Colliders show the shapes physics collides with: mesh colliders as the convex hull
  Jolt builds (`MeshColliderOutline`; also for the triangle meshes of dynamic bodies) or as their triangles, without the
  diagonals of flat faces, and character controllers as the upright capsule physics moves. Its lines are wide,
  light-colored and outlined in dark so they read on bright and dark surfaces, and dimmed where surfaces hide them,
  except joints (their anchors lie inside bodies), directional lights (their position does not matter), post-process
  volumes (the camera is often inside), the capsules of selected characters (their models enclose them) and the outlines
  of canvases, which are drawn over everything. The screen-space UI would cover the scene: the UI canvases overlay draws
  each top-level canvas's UI in its entity's XY plane instead (`CanvasOverlay`), standing on the entity (the middle of
  its bottom edge there), laid out at its reference resolution, 1 cm per canvas unit, faint (too faint to pick) until
  the canvas or one of its elements is selected, with the outline of every canvas and, for the canvas holding the
  selection, the rectangles of its elements, the selected ones highlighted. Focus and `viewport.frame` frame a canvas or
  a UI element's rectangle there (`ComputeEntityBounds`). UI elements, which their `UIRect` places, get no transform
  gizmo. ImGuizmo edits apply to every selected root around the primary selection as one undo step per drag
  (`SetComponentsCommand` with a merge key); click picking goes through the entity-ID pass (Ctrl toggles, Shift adds,
  empty space clears; picking IDs are never reused within a scene); assets can be dropped onto it.
- Game View (`GameViewPanel`): the scene through its primary camera, as the game sees it, at a chosen resolution (free,
  16:9, 16:10, 4:3, 21:9, the project's window size, 1920x1080, 1280x720), letterboxed; fixed sizes render at that size
  and are scaled down to fit. Its size is the scene's view size (`Scene::OnViewportResize`) that scripts read and the
  size Play starts with. Edit mode shows a preview; playing brings it to the front. Scenes without a primary camera show
  their screen-space UI over black, and a note when they have none.
- Undo/redo for every scene modification through a command history (JSON before/after snapshots); edits of asset files
  (visual scripts, materials) are steps that leave the scene alone. Automation commands use the same history. The
  history is bounded to 256 steps and to 256 MiB of snapshots of asset files (`EditorCommand::GetMemoryUse`), always
  keeping the last 16. Steps of asset files survive Stop, a scene the scripts load and another scene of the project being
  opened (`CommandHistory::TakeAssetSteps`/`AppendAssetSteps`), not a project change; they keep the scene's saved
  state, so a scene saved before playing stays clean through Stop, undo and redo. Deleting an asset discards its steps
  (`DiscardStepsOf`); a step that also changed the scene forgets the asset and undoes only its scene part. A file that
  vanished outside the editor is missing, not deleted: its steps stay and fail to undo until it comes back. Undo of an
  asset step gives its file its exact bytes back: a step reads the file before its first execution, except when the
  file's write time and size are those the editor recorded at its own last write of it and the remembered text is the
  stored document's (then nothing is read; an outside rewrite of the same size whose write time was restored is the one
  case this misses, and is accepted). An undo that puts back bytes the editor did not write marks the asset changed
  (`AssetManager::MarkFileChanged`), so the next refresh loads them even when both writes share a write time.
- Play mode (`PlayMode`, controls in the toolbar and the Play menu): Play (Ctrl+P; scripts, physics and audio, shown in
  the game view) and Simulate (physics only, seen in the scene view, with gizmos) run a copy of the edited scene
  through a `SceneRunner`; Pause (Ctrl+Shift+P), Step (Ctrl+Alt+P) and Stop. While playing, the panels and automation
  show and edit the copy (`EditorContext::BeginPlay`) with an undo history of its own; stopping discards the copy and
  every change made to it (steps that edited asset files while playing stay undoable, as the edited scene's newest
  steps), and the selection keeps what still exists. Play builds changed scripts first. Scene and
  project files cannot change while playing (the UI stops playing first). Scripts' `Application.Quit` stops playing
  and `SceneManager.LoadScene` replaces the copy. A blue (Play) or green (Simulate) frame marks the scene view while
  playing; stopping brings the scene view back to the front. While a visual script is stopped at a breakpoint (§10.6),
  the toolbar's Pause continues it, Step is refused with the reason, and the toolbar gains Continue, Step Over, Step
  Into, Step Out and Break (§11.1). Input's frame is the application's, which ends it after every frame, except while
  play mode plays: play mode then owns it (`Input::SetFrameOwned`) and ends it after each game frame that `Update` or
  `Advance` ran. So input given to a paused game, or while an execution is stopped, reaches the next game frame once;
  graph code that a resume of a stopped execution runs between frames reads the input of that moment, so it sees that
  input as well. Input actions are the exception: they are evaluated only by game frames, so resumed code reads the
  last frame's evaluation and the input reaches actions once, at the next frame (§7.4). Play's start begins a clean input frame with held keys staying down; Stop releases them and begins one.
- Game input (`GameInputRouter`, the ImGui layer's input filter, like Unity's game view): Play gives the game the input
  and brings the game view to the front, and while playing a click on the view gives it back to the game (the click
  reaches the game too). While the game has the input, every key and mouse event goes to the game alone (Escape, Enter,
  Space, Tab, the arrows and Alt included: ImGui's keyboard navigation, the editor's shortcuts and the scene view's tool
  keys never see them), except Shift+Esc, which gives the input back, and the play shortcuts Ctrl+P, Ctrl+Shift+P and
  Ctrl+Alt+P (Cmd on macOS; named by the key the keyboard layout types), which stay the editor's. Mouse buttons and the
  wheel are the game's over the view's area, anywhere while the game locks the cursor; a click elsewhere (another panel,
  a popup or window over the view, the window of a panel dragged out of the editor) goes to the editor and gives the
  input back, and so do the view hiding behind another tab or losing the focus and playing stopping. The game sees each
  event in the frame it happens, with the cursor in the pixels it renders and no movement when it gets the cursor back
  somewhere else; releases reach both sides, and the keys and mouse buttons the game held are released when it gives the
  input back, so none stays held on either side (a UI button held then is not clicked). Gamepads are not shared: the
  game reads them whoever has the keyboard and mouse. The cursor mode the game asks for applies while it has the input:
  a hidden cursor is hidden over the game view alone (the game view asks ImGui for no cursor there), and a locked cursor
  is locked to the window showing the game view, moves with that window only and is hidden over the editor's other
  windows, ImGui changing no cursor meanwhile (`GameViewPanel::UpdateWindowCursors`). A blue frame marks the game view
  meanwhile and its toolbar says how to release it; without the input the game gets nothing (its UI follows no cursor:
  `PlayMode::SetUIPointerEnabled` follows `GameInputRouter::HasInput` from the frame the input changes hands, before the
  scene updates) and the editor works as usual. Automation's `input.set` and `input.release` act on the game's input
  directly (keys, the mouse, input action overrides and injected gamepads, which the window's polls leave alone: §7.4),
  and `ui.layout` tells where the UI's elements and buttons are in the game view. Before any of this, a capture filter
  (`SetCaptureFilter`: the Input Actions panel's binding capture) sees key presses, typed characters and mouse button
  presses, and what it takes reaches neither side, nor do the repeats of a key it took.
- The start screen (`Editor/StartScreen/`): `StartScreenPanel` draws `StartScreenModel` over the main viewport's work
  area (under the menu bar) while the model is visible: at start without a project in an editor with a window (unless
  `--no-start-screen`, `EditorLayerSpecification::StartScreen`, which `strada mcp` passes), after File > Close Project,
  from File > Start Screen and `start.show`; any project opened or created hides it (`OnProjectOpened`). At start it
  comes up with why the project given with `--project` did not open on its error line (`DescribeProjectOpenFailure`, which
  says when the file does not exist; the screen is shown first, since the line keeps a message only while the screen is
  visible). While it shows,
  `EditorLayer` draws the menu bar, the screen and the modal dialogs only: the dockspace is kept alive without being
  drawn (`ImGuiDockNodeFlags_KeepAliveOnly`, so docked panels stay docked), no toolbar, panel, viewport, gizmo or
  floating window is submitted (so none can take the mouse or the keyboard), the game view's input goes back to the
  editor, a dropped `.sproj` opens (`FindDroppedProject`) and nothing is imported; the tabs that were in front come back
  to the front when it hides. Commands that need a panel or graph window drawn (`graph.palette`,
  `inspector.add-component-menu`) refuse while it shows (`EditorCommandEnvironment::IsStartScreenVisible`), and what
  waits for the graph windows' draw is dropped in each frame it covers them (`VisualScriptEditors::DropAfterDraw`), so no
  command waits for the screen to hide. The screen's actions run once its lists are drawn (an open that fails takes the
  project off the list it draws), and what fails in them, in the screen or in the layer, shows on its error line
  (`StartScreenModel::ReportError`) while nothing else, the Console included, is drawn. An example's copy opens without
  asking about unsaved changes again when the scene is as it was when they were answered (`EditorContext::GetSceneMark`). The New Project form is one widget (`NewProjectFormWidget`) that the screen and
  File > New Project... (`NewProjectDialog`) draw over the model's `NewProjectForm`; Create hands a `NewProjectRequest`
  (the form as it was) to the layer, which asks about unsaved changes and then runs `CreateProjectFromRequest`
  (`EditorOperations::CreateProject` from the template, then `SetUpNewProjectForAgents`), as `project.create` does.
  Examples open as copies through the layer's one `ExampleCopyTask`, shared with `project.copy-example`. Thumbnails are
  GPU textures (`StartScreenThumbnails`, none without a graphics device, released in `OnDetach`). The cards
  (`StartScreenCards`) are as tall as their longest text needs at the width they get (`StartScreenCardLayout::FitText`,
  within a maximum of description lines), so cards side by side and one under another stay even and show their texts
  whole; a card whose text is still cut shows it in its tooltip. The form's fields and the example list take the height
  of their content up to what their column leaves (`ImGuiChildFlags_AutoResizeY` with a size constraint), so Create and
  Learn follow them and the fields or the list scroll only when the column is too short. `editor.dialog` opens the File
  menu's two dialogs so that captures can show them.
- The global shortcut gate (`EditorShortcuts`): the editor's global shortcuts (New, Open and Save Scene, Build Scripts,
  Play, Pause, Step, the debugger's keys, Undo, Redo and the editing keys) are one unit the layer and its tests run.
  ImGui's global routes ignore modal popups (`ImGuiInputFlags_RouteGlobal` scores the same whatever popup is open), so
  the unit holds every one back while a modal dialog is open (`ImGui::GetTopMostPopupModal`) or the start screen shows.
- Project management: File > New Project (the New Project form: a template, name and location, the setup for coding
  agents and the `.gitignore`; `project.create` takes any template of `project.templates` and sets the new
  project up for coding agents unless its `agents` is false (`SetUpNewProjectForAgents`, `AgentKit::Apply`), and
  `project.copy-example` copies an example of `project.examples` off the main thread (`ExampleCopyTask`) and opens the
  copy; `EditorOperations` holds what both share: `ListProjectTemplates`, `CreateProject` from a template,
  `LoadExampleCatalog`, `CopyExample`),
  Open Project, Recent Projects (user data `Editor/RecentProjects.json`; the start screen's model, `StartScreenModel` in
  `Editor/StartScreen/`, owns the list, which the File menu and the start commands `start.info`, `start.show` and
  `editor.recent` reach through it), Close Project, and `--project <file>` on the
  command line. The folders the editor works with besides the open project are its `EditorLocations`
  (`Editor/EditorLocations.h`): the engine's `EngineLayout`, the user data directory `<user data>/Editor` (recent
  projects, graph views; empty: nothing is persisted) and the default location of new projects: the absolute path that
  the environment variable `STRADA_PROJECTS_DIR` names, else `<Documents>/Strada Projects`
  (`Platform::GetDocumentsDirectory`; nothing creates it but a project created in it). The pure
  `EditorLocations::ResolveDefaultProjectLocation` decides it from the variable's value and the Documents folder (a
  relative value is ignored, with a warning in the log at startup). `EditorLocations::FromProcess()` is the editor's only
  reader of the process's layout, user data and Documents folders and of that variable
  (besides it, the entry point resolves the layout once at startup to log its warnings); the editor layer builds its
  locations once (`EditorLayerSpecification::Locations` injects others) and hands them to the commands
  (`EditorCommandEnvironment::Locations`, which names no folder when default-constructed). Tests pass folders inside
  their temporary directory and the test content (`Testing::MakeEditorLocations`). The Project Settings panel edits the
  settings (generated like the inspector, with a layer collision matrix); they are saved to the project file when an
  edit ends and are not part of the scene's undo history. Scenes saved inside the asset directory are registered as
  assets. Without a project the editor works on loose scene files.
- Scripts (`EditorScripts`): the project's C# project is kept pointing at this engine; Scripts > New Script and the
  content browser's "+" > C# Script create a class from the template in the script folder the browser shows (else in
  `Scripts/Source`) and show it there, C# sources imported into the browser are copied into a folder of
  `Scripts/Source` (`AddScripts`, creating the C# project when the project has none). Both refuse what would declare a
  type twice in the C# project (C# error CS0101): a file name a file of the project has, or a type a source declares
  already (`ScriptBrowsing::FindDeclaredTypes` reads classes, structs, interfaces, enums, delegates and records with
  their namespaces; partial and file-local types are allowed), saying which source has it. Scripts > Build Scripts (Ctrl+B)
  builds on a worker thread, and changed sources (checked every second) build by themselves. Errors and warnings go to
  the console with their files and lines, the menu bar shows a failed build, and each successful build is loaded at
  once (hot reload; while C# scripts or visual scripts run, when they stop). The inspector edits Script components with
  a class picker and the fields of the class: typed asset pickers, enum combos, flag check boxes, colors and Euler
  angles like component fields, one undo step per interaction.
- Prefabs: an entity's "Create Prefab" (hierarchy context menu) writes it with its descendants to `Prefabs/`, and
  entities dragged onto the content browser become prefabs in the folder they are dropped on; neither changes the
  scene. Prefabs dragged onto the scene view, the hierarchy or an entity row are instantiated there (one undo step).
- File > Build Game (`BuildGameDialog`) exports the game for the host platform (§13): the game's settings (name, start
  scene, window title, size, fullscreen, vsync, resizing and icon, edited like Project Settings), the output directory
  (the project's `Build` at first) and a smoke test that runs the exported game headless for a few frames; then the
  export's progress with a cancel button, and the executable with the export's size and file count, the smoke test's
  result, and buttons that open the folder (`Desktop::Open`) and run the game (stopped when the editor closes), or the
  reason it failed (with the scripts' compile errors); the menu bar shows a running export.
- File > Set Up for Coding Agents (`AgentSetupDialog`) shows what `AgentKit` (`Editor/Agents/`, free of ImGui) would do
  to set the open project up for coding agents: each file (the `strada-agents` block of `AGENTS.md`, the `@AGENTS.md`
  line of `CLAUDE.md`, the `strada` entry of `.mcp.json`, the game-making skill, the guides under `.strada/docs/`, the
  `.strada/` line of `.gitignore` and the state `.strada/agents.json`) with its action and the diff of its managed part;
  Apply writes them atomically and shows the result or the file that stopped it. The plan is made when the dialog opens
  and when the project changes under it, never per frame. The kit's sources are the layout's engine content (`AgentKit`,
  `MakeGameSkill`, `Docs`); `.mcp.json` names `strada` when the one on `PATH` (`Process::FindExecutable`) is the one
  beside the editor, else that tool's absolute path. `project.agents` and `project.setup-agents` (`AgentCommands`) are
  the same for agents; neither the dialog nor the commands are undo steps.
- Headless mode (`--headless`): no window/ImGui; offscreen rendering if a GPU is available; used by automation and CI.

### 11.1 Visual script editor

Visual scripts are edited on a canvas of nodes in **graph windows** (`VisualScriptEditorPanel`), one dockable window per
open graph. Double-clicking a visual script in the content browser or choosing its Open menu item, the asset
inspector's Open button, the component inspector's Open Visual Script and `graph.open` open or focus it. The component
inspector's Create Visual Script (on entities whose component runs no graph) writes `Assets/Scripts/<entity name>.sgraph`
from the Events template (the name made portable and numbered when taken, `New Visual Script` when nothing is left of
it), gives it to every selected entity as one undo step named "Create Visual Script" and opens its window with the
inspector's primary entity as the context entity; without an asset directory the button is disabled with a tooltip.

- **Layers** (all in `StradaEditor/Source/Editor/VisualScripting/` but the panel, which is in `Editor/Panels/`; each
  includes only layers below it, everything runs on the main thread except the serializer, the text writer, `Validate`
  and the pure document edits, which run on any thread (their headers say so), and L1 to L3b include no ImGui and are
  tested with plain doctest cases):

  | Layer | What |
  |-------|------|
  | L1 Engine | The document, format version 3, the layout of unplaced nodes, the literal codec (array literals included), `HaveSameVisualScriptProgram`, `VisualScriptReferences` (the `Graph` setting, local references, the resolver type, the interface form), the text writer and `Validate` |
  | L2a Document editing | `VisualScriptEditing` (every edit as an operation on a copy of the document, `SetSetting`'s table of settings other operations own), `VisualScriptLanguageEditing` (arrays, formulas, timelines), `VisualScriptTimeline` (the curve model, evaluated through the engine's curve, §6.5), `VisualScriptCrossGraph` (retargeting, kinds, the declared pins of cross-graph nodes, broken referrers), Arrange, the clipboard, the palette's index and ranking, quick fixes, find |
  | L2b Commit | `VisualScriptNormalization` (graph.set's transforms), `PrepareVisualScriptCommit` with its timings, `SetVisualScriptCommand`, `SetVisualScriptAndComponentsCommand`, the history changes, layout-only commits |
  | L3 Canvas model | The semantics seam (`EngineVisualScriptSemantics`, or the fixture semantics of tests), the view, metrics and text ladder, theme tables, the visual model, geometry, the spatial index, hit testing, the editor model (gestures in, intents out), the per-user view settings, `VisualScriptTextEstimate` (text widths without a font), `VisualScriptProjectIndex`, `VisualScriptCurveView` (the shared curve view of `Editor/UI` over the timeline, §6.5: its view, hit testing and gestures) |
  | L3b Batch | `VisualScriptBatch` (`graph.edit`'s executor), `VisualScriptHeadlessCanvas` (node sizes, pin rows and anchors without a window), `VisualScriptCanvasPins` (pin rows from a canvas, shared with the panes) |
  | L4 Panes | The palette popup, My Script, Details (the formula field, the array literal editor, the Timeline section, the Graph pickers), the drawer (with `VisualScriptCurveEditor`, its Timeline mode, on the shared curve editor widget of §6.5), the find bar, the literal editor (built on `FieldEditor`) |
  | L5 Canvas UI | The controller (an intent becomes at most one commit request), the renderer (comment labels among its layers), brushes, the panel, the in-place formula editor, the key and gesture tables |
  | L6 Shell | `VisualScriptEditors` (the window manager, the project index's owner, opening and Back and Forward across windows), `EditorEditingKeys`, `EditorLayer`'s wiring, the entry points, the `graph.*` UI commands |

  The editor model and the document editing are independent of each other: the model emits intents, which are data,
  and only the controller turns them into edits. L2a and L3 include nothing of each other, with one exception: the curve
  model (`VisualScriptTimeline.h`, pure, without state) is shared by both, so the curve view clamps and evaluates with
  the document editing's own functions (the view itself is `Editor/UI`'s `CurveView`, which includes nothing of the
  visual script editor; `VisualScriptCurveView.h` gives it the timeline); the types both layers read (`VisualScriptProjectGraph` among them) are in
  `VisualScriptEditorTypes.h`. L3b is the only non-UI code that includes both. Operations take pin names as the document stores them
  (`VisualScriptEditing::ToStoredVisualScriptPin` maps the canvas's names of renamed pins and their fields back).
  Without the scripting runtime a window runs degraded: it shows, arranges, comments, copies and deletes nodes from the
  document alone (titles derived from it, pins inferred from its links), and what needs the node library is refused
  with a blink of its notice.
- **Windows** (`VisualScriptEditors`, owned by `EditorLayer`): a window is named `<title>###VisualScript<handle>`, so its
  ImGui ID survives renames; its title is the file's name without extension, with its folder when two open graphs share
  a name (`Player (Enemies)`, `(Assets)` for the asset directory itself). Graph windows and every popup and modal they
  open carry `ImGuiWindowFlags_NoSavedSettings`: `imgui.ini` keeps nothing of them, and their placement is computed
  each session. A new window docks into the node of the graph window focused last, else the Scene View's node, else it
  floats centred at 70 % of the main viewport. View > Reset Layout docks the open graph windows into the centre node
  again; View > Visual Scripts lists them (unchecking one closes it) and has Close All. Each frame the shell, before the
  windows draw: follows the project (on a change it flushes pending edits, saves the views, closes every window and
  reopens the new project's, skipping graphs that are gone with an Info line), closes the windows of deleted graphs,
  ends the shared edit step when the active widget changed (below), opens the windows other windows asked for (a
  function of another script, a custom event's sender) and updates titles. Its other calls (open, close, describe) make
  no ImGui call and run the project and deleted-graph checks first, so automation between frames never reaches a
  deleted graph. A missing file or one that does not load opens on a card that says why; the canvas comes back when a
  refresh finds a loadable file.
- **Other graphs** (`VisualScriptProjectIndex`, owned by `VisualScriptEditors`): every visual script of the project with
  its kind, shared document, load error and references, kept at one registry check and one pointer comparison per
  script. The graph windows refresh it while one is open, and the inspector and the delete confirmation before they read
  it; a file that does not load is read again only once it changed, so its error is logged once. Each frame the windows
  hand their model its list (`SetProjectGraphs`, of which the model keeps its own copy, sharing the documents, so nothing
  it reads dangles between draws); `MakeResolver(self, working)` resolves other graphs for titles, pins, quick fixes and
  lookups, a window's own graph to its displayed document. The canvas, the controller, Details, the palette and the quick
  fixes resolve other graphs through the model, and every reader of a node's `Variable`, `Function` or `Name` settings
  checks `IsLocalVisualScriptReference` first, so a remote node never binds to a local element of the same ID (a sweep
  test pins the sites). A window analyzes again when the index shows that a graph its analysis read
  (`VisualScriptAnalysis::Graphs`) changed what was read: the same document, else the whole program for a library and
  the interface for the others; its canvas is laid out again only when a graph its nodes name resolves to another
  document, so a commit elsewhere costs no layout. `VisualScriptCrossGraph` holds the pure cross-graph edits
  (`RetargetNode`, `SetDocumentKind`, the declared pins of cross-graph nodes) and `FindVisualScriptBrokenReferrers`, which
  `graph.set`, `graph.edit` and the windows' toasts use to tell what an edit breaks in other scripts. Open requests carry
  a `Reveal` (the element to select there) and an `Origin`; history entries may name another graph, and the window
  manager carries Back and Forward between windows, opening a closed one at the stored view.
- **Edit menu and history toasts**: the Edit menu follows the editing window focused last: a graph window whose tab is
  in front makes it act on that graph's selection (Cut, Copy, Paste, Duplicate, Delete, Select All, Deselect All);
  focusing Scene View, Scene Hierarchy or Inspector makes it the scene's again, and a graph window hidden behind another
  tab is never its target. Undo and Redo stay one history. When they run while a graph window is the target, the window
  says what they did ("Undone: Move 3 Nodes in Player", "Redone: ..."), with Show for a step of the scene or of another
  graph, which focuses Scene View or opens that graph's window (bringing it to the front when it is open). The editing
  keys (Ctrl+X, Ctrl+C, Ctrl+V, Ctrl+D, Delete or Ctrl+Backspace, Ctrl+A) follow the same target as the menu: with a
  graph window as the target they act on its selection wherever the keyboard is, also in the console or the content
  browser, and never on the entities (`VisualScriptEditors::HandleGlobalEditingKey`); with the scene as the target,
  Duplicate, Delete and Select All act on the entities. A window keeps the keys it claims while it has the keyboard: the
  content browser's Delete, Ctrl+Backspace and F2 (`ContentBrowserPanel::FocusedChords`, which
  `FindClaimedEditingChords` returns) act on the selected file there. A widget inside a window claims chords the same way
  while its own child window, or a popup it opened, has the keyboard: it updates a claim on its root window each frame it
  is drawn (`UI::UpdateEditingChordClaim`), kept while a menu, a side bar or another window has the keyboard, which
  `FindClaimedEditingChords` returns for that root (the inspector's curve editor: Delete, Ctrl+Backspace, Ctrl+A). An Edit item shows its shortcut exactly when
  pressing it would run the item (`GetEditShortcutText`), so Delete shows none while the content browser has the
  keyboard. The window that had the keyboard before a menu, a popup, the menu bar or the toolbar took it is the one that
  counts, while it is still open (`FindKeyboardWindow`); a claimed key runs nothing while a menu, a popup or the
  browser's own dialog has the keyboard, nor after a menu-bar menu closed, until a window is clicked.
- **Toolbar**: the My Script toggle, Back and Forward, the breadcrumb (`Player › Event Graph`; its list arrow shows
  every sub-graph with its problems), the Instance combo (the context entity), the LIVE pill, the problems pill (error
  and warning counts, "No problems", or "Not analyzed" without the runtime; a click toggles the problems drawer), the
  save status ("Saved", or "Not saved" in the error colour after a failed write; Ctrl+S, which saves the scene, flashes
  it), Arrange, Find, the Bookmarks menu, Snap to Grid, the minimap toggle, the zoom (a click resets 100 %), the view
  menu (scroll mode, Reduce Motion, Context palette), `?` (the shortcut sheet, also F1) and the Details toggle. It is
  laid out from measured widths every frame, and while it does not fit it degrades in stages: Arrange, Find, Bookmarks
  and the save status become glyph buttons; the zoom, Bookmarks, Snap to Grid and the minimap toggle move into the view
  menu; the Instance combo and the breadcrumb are shortened, each to no less than 8 font sizes. The pane toggles, Back,
  Forward, the pills, the view menu and `?` never hide. The editor model's `HiddenToolbarActions` lists what moved into
  the view menu; for the Bookmarks menu it holds `GoToBookmarkN` for each stored slot and `StoreBookmarkN` for the first
  free slot.
- **Notices and cards**: under the toolbar, top to bottom: degraded (no scripting runtime, with Show Console), the
  normalization notice ("3 nodes use renamed members or have no position; they are updated with your next edit.", with
  Update Nodes as one step, disabled without the runtime), the repaired file (dismissable, gone after the window's first
  successful edit), the read-only memory asset, the read-only file (with Retry, which checks the permission again;
  `FileSystem::IsReadOnly` is the one rule: no write permission bit; a file locked by another process shows as a write
  failure), and once per function the new-function hint. Cards replace the canvas for a missing file, a file that could
  not be read (naming the load error), an empty event graph (Add Create Event, Add Update Event, Add Node...) and a
  function library without functions (`EmptyLibrary`: "This library has no functions yet.", Add Function; a library's
  leftover event graph shows the card with Open of its first function). A ribbon of first steps shows over the first
  canvas a user sees, once. Toasts (4 s, kept while hovered) report paste notes, rewrites, history steps, a failed write
  ("Could not save <script>: <reason>", kept until dismissed), outside changes out of view ("2 nodes changed outside the
  view", with Show), and after each own commit that changes what the graph offers others, what happens to their nodes
  (`DescribeVisualScriptOfferChange`: "Renamed Value to Amount: 3 calls in 2 other scripts follow it", "Deleted Clamp01:
  1 call in 1 other script show errors", with Find Callers, Find References or Find Senders); an interaction that
  commits at the widget cadence repeats the same toast, which then stays once, shown anew.
- **Canvas**: views zoom between 16 levels from 0.10 to 2.00 (a wheel notch steps one level, at once; framing and
  other moves animate for 0.2 s unless Reduce Motion is on) around a centre within ±1e7 canvas units. Four levels of
  detail follow the zoom alone, each threshold widened by 0.02 against flicker: Far below 0.175 (rectangles in their
  header's hue; select and move), Overview below 0.45 (titles and pin dots; wiring), Compact below 0.65 (full nodes
  without subtitles; pin labels and values as text while that text is at least 7 pixels, from a zoom of about 0.54,
  bare pins below) and Detail (subtitles, value chips and the live widget that edits a value in
  place). Text is drawn at whole pixel sizes only, from a ladder of 33 sizes (6 to 20, then steps of at most 10 % up to
  82), so it is crisp at every zoom and the font atlas bakes few sizes. A uniform grid of 512-unit cells over the nodes,
  comments and wires (`VisualScriptSpatialIndex`; items spanning many cells are tested by every query) makes culling and
  hit testing cost what is near the view, not the graph; it is rebuilt only when the geometry changes, and items a
  gesture moves are tested at their preview places, so a drag never rebuilds it. At the Far level wires shorter than 6
  pixels are skipped, and with more than 4000 wires in view only exec wires and those of selected nodes are drawn. The
  renderer emits layers in order into one draw list (no channel splitter), keeps its buffers from frame to frame and
  allocates nothing on idle frames. `VisualScriptRenderStats` counts what a frame drew (visible nodes, wires and
  comments, index cells and candidates, vertices, indices, draw commands, text sizes); the performance tests check these
  counters always, on graphs of up to 10,000 nodes, and their wall-clock bounds (3 times the measured figures) only with
  `STRADA_PERFORMANCE_TESTS=1`. Pins and wires take their type's colour, wires with an implicit conversion a gradient
  between the two; disabled nodes are dimmed with a bypass line from the exec input to their `PassThrough` exit, and
  disabled and development-only nodes carry a strip that names their state. Links made by any means pulse once, and
  elements changed from outside (undo, automation, another window) flash. At Far and Overview, comment titles are drawn
  in a layer of their own above the nodes, on a plate of the comment's bar colour, and are hit before nodes (a press
  drags the comment, a double-click edits the title; the comment's own resize handles still win). At every level a title
  starts at the canvas's left edge when its box starts left of it, is ellipsized to the room left, and is left out when
  it has no room for three of its glyphs and an ellipsis or when its bar is outside the canvas; a title whose glyphs are
  wider than average shows fewer of them, at least the ellipsis, and a plate is never blank.
- **Palette** (Tab, a Space tap, a right click on the background, a wire released on empty canvas, `graph.palette`): the
  node library's kinds with their typed expansions ("Get Component: Rigid Body", "Make Float Array"), the document's
  variables (and the shown function's locals), functions and custom events, the project's other graphs (the functions
  of its libraries under `Libraries/<name>`; the other scripts' typed Send and Broadcast of their events, Get and Set of
  their public variables and Runs under `Scripts/<name>`; an untyped Broadcast of the script's own events; "Get/Set X of
  another <script>"), and the actions (a comment box, a reroute, a custom event, a variable; none of variables or custom
  events in a library). The game's `[VisualNode]` members are the Game category, which ends with a disabled row naming
  the members without a node ("3 [VisualNode] members have no node: see the Console"). A query of `=` and a formula
  lists the Expression entry alone, which adds an Expression of that formula; Expression is left out of context mode.
  The palette reads the project's graphs only when the index changed (`SetProjectGraphs` with its revision), and its
  index of the document follows the model's program revision, so a layout-only commit rebuilds nothing. Entries a graph cannot use are left out (events and ticking nodes in functions' graphs, impure nodes in
  pure functions', a second `Function.Entry`, `Function.Entry` and `Function.Return` in the event graph), and an event
  the event graph already handles is offered as a jump to it. Without a query it lists the 8 most recent entries, then
  the categories. A query's words (up to 8; ASCII letters in any case) must each match; a word scores the best of the
  title starting with it (700), a word of the title starting with it (600), the initials of the title's words (500, `af`
  for Add Force), the title holding it (400), a keyword starting with or holding it (350, 300), a word of the subtitle
  (250) or of the category (200) starting with it, the node type holding it (150) and, from 3 characters, the title
  holding its characters in order (100 less 5 per gap, at least 20). An entry adds 1000 when the query is its title, 300
  less 15 per place for the 20 most recent entries and 50 for the document's own, and loses 2 per byte of its title;
  equal scores keep library order. Types and keywords are compared without the `Strada.` namespace. In context mode only
  entries with a pin that connects to the dragged pin are listed (never through a wildcard only settings decide), 250
  more for a pin of exactly its type and 100 more for an implicit conversion; Convert's typed expansions are offered only
  for conversions every value (to `string`) or number has, and without a query only when nothing takes the type as it
  is. Choosing an entry adds the node at the opening point in one step, linked to the dragged pin in context mode, and
  gives the keyboard to a required setting in Details. Recent entries are kept in the views file as `{ Type, Settings }`
  records (actions as `{ "Type": "Action:<name>", "Settings": {} }`).
- **Panes**: **My Script** (left) lists the graphs (the event graph and the functions), variables (a public one with an
  eye; while playing, the context entity's live values of public variables, read at most 4 times a second), the shown
  function's locals and the custom events, with a filter; "+" creates an element and opens its name, creation and first
  name being one undo step. Rows drag onto the canvas (a Get or Set menu, Ctrl for Get, Alt for Set; onto pins) and
  reorder variables (`DragDropPayload::VisualScriptElement`, accepted only by the window of their script). Deleting an
  element that nodes use asks first and removes those nodes the way deleting them on the canvas does, bridging the flow.
  **Details** (right) edits the selected node (its settings by kind, its unlinked inputs, its comment and Enabled
  state), comment box, variable, local, function (signature, purity, locals; "Called by" leaves out calls in its own
  graph) or custom event; Easing plots are drawn for display only. Name fields check names as they are typed (the
  loader's rule, `VisualScriptSerializer::IsValidName`, and uniqueness in their scope) and commit when left by any
  means. Enum lists of more than 12 enumerators have a filter. Promoting an input to a variable or a function input
  keeps the program's value (its literal, else the library default, else the type's); an unlinked input that means the
  graph's own entity cannot be promoted, and an array promotes to a private array variable (public ones are refused:
  "Arrays cannot be public: components cannot store them"). Details edits an Expression's formula in a monospace field
  that commits like a name field, renaming inputs in order (`SetFormula`), and lists its inputs with their types and its
  problems as caret blocks whose click selects the span (through `CallbackAlways`); a double-click on the node's header
  edits the formula in place (one line, `VisualScriptTextEditKind::Formula`). Array values (variables' and locals'
  defaults, a `Literal`, unlinked array inputs) are edited by `DrawVisualScriptArrayLiteral` (elements reordered by their
  handles, within one list), a Make Array's Count renumbers its element pins (`SetArrayElements`), and a Timeline gets a
  section (Length, Loop, a row per track with a preview and Edit). Nodes that name other graphs get a Graph picker
  filtered by the setting's constraint and Variable, Function and Event lists of the target's elements, each change one
  step through `RetargetNode`; with nothing selected Details shows the document's Kind, asking first when a switch
  breaks this or other scripts' nodes. A value being dragged or typed in Details or My Script survives an outside change
  (an agent's `graph.edit`, another window's commit) of other elements: `VisualScriptWidgetCommits` rebases it over the
  change (`RebaseVisualScriptEdit`, a pure function of the base, the pending value and the current document), and drops
  it with "Your edit of Duration was replaced by a change made elsewhere" when the change touched the same element. A
  canvas gesture survives changes made elsewhere (an agent's `graph.edit`, another window, undo) that touch none of its
  elements: the model records what the change did to the shown sub-graph (nodes and comments changed or deleted, links
  removed, the sub-graph gone) and, once the canvas shows the new document, continues the gesture on it; a move, a
  comment resize, a wire and a relink go on while their elements, the wire's source and the relink's picked links and
  their ends are untouched, marquees and cuts always go on, and a press not yet a drag does nothing more when its
  element changed. A gesture whose elements were touched ends without an edit, and the window says why ("Your move was
  interrupted: Branch (node 12) was changed elsewhere"). Intents name elements by ID and are resolved on the current
  document, so the history holds the outside step, then the user's.
  Paste leaves out an input event the event graph already handles (the same key or button, compared by enumerator).
  Panes stand beside the canvas when both fit with a canvas of 40 font sizes; otherwise, with both on, they stack in one
  column on My Script's side (My Script above Details at 45 % of the column, a splitter between them, the column as wide
  as the wider pane but no wider than the canvas leaves, at least 14 font sizes, its edge a gap, not a splitter);
  otherwise Details auto-hides first, then My Script, and they open over the canvas when needed. Both stay Shown when
  stacked (`graph.editors` reports `stacked`). The **drawer** under the
  canvas lists problems (errors first, then by sub-graph, then by position; a click goes to the element) with their
  quick fixes, or the results of a find in this script or every script ("could not be read: <error>" for scripts that do
  not load), or, in its Timeline mode (its own height, 16 font sizes), a `Flow.Timeline`'s tracks beside its curves
  (`VisualScriptCurveEditor` on the shared curve editor widget over the pure `VisualScriptCurveView`, §6.5, whose
  view, hit testing and gestures are tested without ImGui). It opens from a double-click on a Timeline's header, the node menu's Edit Timeline, Details' Edit and
  `graph.open`'s `timeline`, and closes when its node is gone. Every edit is one `SetTimeline` step; drags are computed
  from the timeline as the interaction found it and merge into one step at the widget cadence; Escape cancels a drag
  (the setting goes back within the drag's own step, which then disappears) or a marquee (the selection comes back);
  arrow nudges less than 0.5 s apart are one step ("VSTN" keys, apart from the canvas's); the view keeps its ranges in
  double and follows renamed and removed tracks. Problems also show on the canvas: a badge on nodes (and in the breadcrumb and My Script for functions),
  ringed pins, dashed error links with a badge; a badge's popup lists the messages with their fixes. Quick fixes
  (`GetVisualScriptQuickFixes`) are one step each, "Fix: <label>": change a node's type to a suggestion, use another pin,
  setting, variable, function or type, remove an input literal or setting, keep an exec output's or input's first link,
  break a link, insert a Convert, reset a value, move an entity literal into a public `Strada.Entity` variable, make a
  zero quaternion the identity, make a variable private, delete a node, add a function's Entry or Return, make a
  function impure, update a node to its canonical type, remove an unused split, enable a node the runtime runs anyway,
  use a function's name in a formula, retarget a node naming another graph (Send by Name, Use <event>, Update Parameters
  from <script>, Use <variable or function> of that graph), delete a library's variable or event-graph node, remove a
  call's Graph naming its own graph; Go To, Edit Setting, Rename, Edit Formula, Choose a Graph and Open <x> in <script>
  navigate instead (`VisualScriptQuickFixGraphs` gives them the other graphs' documents and names). The **find bar** (Ctrl+F) outlines matches in amber; Enter and
  Shift+Enter go to the next and previous match, which becomes the selection, while the find field keeps the keyboard;
  Escape closes the bar and gives the keyboard back to the canvas, so the keys after a find act on what it showed. Find
  (`FindInVisualScript`, shared with `graph.find`) matches titles, subtitles, types, node comments, pin labels, literals
  and text settings, comment boxes and names, one result per element and field, in document order. References follow
  other graphs: a variable's or function's are looked for in its graph and in every graph whose nodes name it, a custom
  event's in every script, and Find References on a node naming another graph lists that graph's element.
- **Commits and undo**: every edit is a new document made by an L2 operation, prepared by `PrepareVisualScriptCommit`
  (former names of variables and of a library's parameters, canonical node types and the pin renames of exact library
  calls, the layout of unplaced nodes, then `VisualScriptSerializer::Validate` and `WriteText`, §7.3, whose text is the
  text written; `VisualScriptCommitTimings` names its phases) and executed by `EditorOperations::EditVisualScript`
  through `SetVisualScriptCommand`. A canvas edit that only moves, arranges, aligns, distributes or straightens nodes,
  changes a comment box's geometry or colour, or collapses nodes is committed with `VisualScriptCommitRequest::LayoutOnly`
  when preparing it rewrote nothing: the window then keeps its analysis (none runs and the program is not compared),
  moves only the nodes and comments that moved (`VisualScriptCanvasModel::SyncPositions`, equivalent to a full sync, to
  which it falls back when anything else must be laid out), and the palette does not read the document again. Edits write through: every executed step (first execution, merged execution, undo, redo)
  writes the file, then gives the asset the committed document object; there is no unsaved state, no save shortcut and
  no prompt on close. The command keeps the file's bytes when they are not the canonical text (version 1, repaired, a
  byte order mark, unloadable), so undo restores them byte for byte (an emptied file comes back empty); it refuses
  read-only files and records each write so a refresh does not reload it. An entity drop that sets variables edits the
  graph and the components as one scene step (`SetVisualScriptAndComponentsCommand`). One gesture or widget interaction
  is one step: widgets (chips, the live widget, the panes' fields) commit their first change at once, then at most every
  0.1 s, and the last value when the interaction ends, under the key of one `EditSession` all graph windows share
  ("VSED"), whose step ends when the active widget changes, so a click from one field into another starts a new step;
  canvas gestures commit once on release; arrow-key nudges merge while presses come less than 0.5 s apart ("VSNG");
  a creation and its first name are one step ("VSNM"; the panes count from the upper half of the counter). Steps are
  named after what they did and the script they did it in ("Add Branch in Player", "Delete 3 Nodes in Player", "Fix: Use
  Value in Player"), which the Edit menu's Undo and Redo and the history toasts show. The history's rules for asset steps
  (carried across Stop and scene changes, the memory budget, deleted assets) are in the history bullet above.
- **Agents' edits** (`graph.edit`, §12): `ApplyVisualScriptBatch` (L3b) copies the stored document and applies the
  operations in order, each the editor's own pure function, so a batch and the same gestures in a window commit the same
  document (parity tests compare them); the command then analyzes, prepares and executes one `EditVisualScript` step, as
  a window's commit does. Pins, node sizes and anchors come from a `VisualScriptHeadlessCanvas` per sub-graph, synced
  with the working document and with an analysis that runs again only when an operation needs pins that earlier
  operations may have changed; the command set keeps the stored document's analysis per document and library, dropped
  when a graph it read changed. Node sizes are measured as a window draws them, without one: `VisualScriptTextEstimate`
  holds the advances of the 95 printable ASCII glyphs of the UI font in 1/64 of the font size, rounded up, times a
  margin of 1.04 (every other code point counts one font size), so heights equal a window's and widths are at most about
  6 % wider; `Uniform` extents (256 x 128) are an option. Arranging with anchors places the nodes a batch added without a
  position beside the placed nodes they continue: the anchors stand as a fixed column, each arranged group's first
  column is ordered against the anchors' rows by the links from the anchors into that group, and the block starts 64
  right of the anchors' right edge, at their top, on the 16 grid. The Arrange button and `graph.arrange` lay out alike
  (`ArrangeNodes` with the window's or the headless canvas's `VisualScriptArrangeMeasures`: each node's size, and
  whether it is pure, from the node's `Pure` flag as the canvas draws it). A node that is not pure stands in the column
  of the longest chain of links leading to it; a pure node (a get, a literal, an operator, a pure call) that a later
  column reads moves right, into the column just before the leftmost node reading it, so a region's inputs stand beside
  their consumers instead of in its first columns with long wires. A pure input several nodes read stands before the
  first of them, a chain of pure nodes closes up behind the node it feeds (pure nodes move deepest column first, so
  the nodes a pure node feeds already stand where they end), and only links that run to a later column count, so links closing a cycle move
  nothing. Moves go right only, so every wire still runs left to right, and the node ending each longest chain keeps
  its column, so no column empties. Rows still fill each column in the order the crossing sweeps choose. A canvas
  without the node library knows no pure node, and `Uniform` extents take none for pure: both give the columns of
  `PlaceUnplacedNodes`. A sub-graph's headless canvas is laid out again
  only when what an operation reads may have changed: operations that read pin rows (Connect, Relink, Splice, deletions
  that bridge, splits) reuse a canvas across link, value and layout changes, and those that read node sizes or anchors
  (Arrange, Align, Distribute, Straighten, Promote to Variable, comment framing, Paste's corner) reuse it across moves;
  without the runtime every change lays it out again, since the degraded canvas infers pins from links and literals. The
  headless canvas resolves the other graphs nodes name, as a window does. A thousand Connects on a 2,000-node graph take
  114 ms in Release with one copy of the document and one canvas layout, which a test asserts. Arranging keeps comment
  boxes whole: a box whose contents are all arranged moves with them as one block (its nodes and the boxes inside it
  laid out on their own, the box fitted around them standing in the layout as one node linked as its nodes are), so
  boxes stay nested as they were, boxes that did not overlap do not overlap afterwards, each frames the nodes it framed,
  and the `Created` scope keeps new blocks a frame's reach clear of every existing box.
- **Live edit**: graph windows stay editable while playing. Every commit reaches the running game in the commit's own
  call (layout-only edits send nothing); a dragged value reaches it within 0.1 s. A LIVE pill in the play colour says
  so; it turns amber, "LIVE · last working version", while the game runs a version that is not the stored one
  (`HasWorkingProgram` and not `RunsStoredProgram`; for a library, while a graph that calls it runs the version that
  worked, its tooltip says so), and reads SIMULATING while simulating (graphs do not run then). Every frame the window
  manager takes one revision of the live log and hands it to each window; a window that commits takes the newer revision
  its commit made and toasts its receipt in the same frame. A window toasts a receipt of its own graph only when the
  graph's document changed since its last look (its commits, undo, redo, outside changes it adopts), or when the receipt
  ends that edit's Pending one in the same toast; loads that are no edit say nothing (an entity started mid-play with a
  graph no entity ran, a scene loaded, a graph first used in a play session). One toast per undo step: a drag's commits
  say the latest in place, and an undo's toast says what it did to the game. A library's window sums the receipts of
  the graphs that call it, and those graphs' windows stay silent. The pill opens the Live popover: since when the graph
  runs, what of it the game does not run (with the outline switch and Show first error), the last receipt in full (an
  instance's line shows that entity), the runtime problems, and for a library what each caller runs. The canvas
  outlines in dashed amber the nodes and wires the game does not run yet (`FindVisualScriptNotRunning`); a red or amber
  disc on a node's corner shows its runtime problems with their count, and for four seconds after an edit discs show
  where it cancelled a wait or reset a node's state. The problems pill counts runtime problems after a lightning bolt,
  and the drawer lists them in a Runtime section (Go to, Caller, Select Entity), kept after Stop as the last run until
  Play or Clear; a node's first runtime error of a play session toasts, with Show and Break on errors. Simulate is not a
  play session: the last run's runtime problems stay "Runtime (last run)" and do not toast again. The Instance combo picks
  the context entity among those whose component runs the graph (the open request's, else the one kept in the views
  file, else the only one; "Any instance" first while playing): entity drops write values to it, and My Script shows its
  values. A deleted context entity is kept but not used until an undo brings it back ("Instance: Enemy 3 (destroyed)").
  My Script shows every variable's live value in a cell between its name and its type: colours as swatches (with ImGui's
  colour tooltip), entities by name (a click selects), arrays and strings on one line cut before the type column
  (`RenderTextEllipsis`), a flash when a value changes, and the stopped frame's values while an execution is stopped. The
  Timeline drawer draws a playhead at the instance's time while playing. Steps made while playing stay undoable after
  Stop.
- **Debugging in the editor**: a graph window draws debugging from one pure view per frame. The window manager tells
  each window what the debug session shows of its document (`VisualScriptWindowDebugInput`: breakpoints with their
  states, the selected stop frame, callers, the computed path, wire values, the flow take, the debug strip), and the
  window maps it onto its canvas after its own sync, because link indices change with that sync; the renderer only draws
  the result. The visible windows' sub-graphs are the session's watches, each window pushes its Instance combo's entity
  as its graph's target, and automation's targets come back through the target listener; a window that has not chosen
  its entity adopts the target the session already has, so `graph.target` holds across the window a stop opens.
  - **The canvas**: flow lies behind the nodes (the `Flow` layer) and markers, the current instruction, callers, run
    counts, value pills and live marks above them (the `Debug` layer). A value pill sits at its wire's midpoint, else
    slides along the wire to the nearest place that covers no node and no pill or run count drawn before it, else to one
    that covers no pill. Flow decays on the flow clock: it halves every
    0.25 s of game frames and holds while no frame runs, so a paused game, a stop or an agent's frame-stepped session
    shows exactly the frames that ran. Run counts are exact and count each run once: a call or a latent node counts its
    entry and return together, while a sequence's outputs and a loop's iterations each count. At Far and Overview,
    comment boxes glow with their brightest node. Breakpoint markers sit on a node's top-left corner; a ghost marker
    shows where a click adds one, and dragging from it moves the node; a real marker leaves the mouse to a pin whose
    anchor lies inside its circle. One formatter (`FormatVisualScriptDebugValue`) shows every value, on the canvas, in
    My Script, the Debugger panel and the receipts: colours as swatches, entities by name, arrays as "[n] a, b, c",
    strings quoted with `\n` shown dimmed. With Wire Values set to Always, only windows that show their canvas read live
    values, for the sources of at most 200 wires in view. A comment title needs the measured room of its first three
    glyphs and the ellipsis, so a drawn title never shows fewer.
  - **Stops**: a new stop opens its innermost frame's document on its node (a library's own window for a library
    frame), once per new stop; these opens push at most one Back entry per stop session, so Back after any number of
    steps returns to the view from before debugging. The debug strip under the toolbar says why the execution stopped,
    passed, failed, waits or stepped, with its buttons (Only <entity> when one frame stopped many instances); a window
    whose graph holds a member waiting inside a Send or Broadcast shows Waiting ("Waiting in <Send> (node n): its
    receiver on <entity> is stopped in <graph>") with Show, and a window that only holds a call that led to the stop
    draws the call outlined, with no strip. The LIVE pill gains a STOPPED segment while the window holds the selected
    stop's frame.
  - **Keys and toolbar**: F9, Ctrl+F9 and Ctrl+F10 act on the selection. F5, F10, F11, Ctrl+F11 and Shift+F11 are the
    editor's global chords while an execution is stopped (`VisualScriptDebugControls`), listed in
    `VisualScriptEditorChords` and shown in the shortcut sheet as `Global` rows, and never act behind a modal. Pause while
    stopped is Continue then the unpause, queued for the next `BeginFrame` (windows never run graph code while drawing),
    Step is refused with its reason, and the toolbar gains Continue, Step Over, Step Into, Step Out and Break. While the
    debugger holds the pause the editor keeps the keyboard (`UpdateInput` takes the game's input back every frame), so F5
    and F10 always work, and the game gets its input back when play resumes where the Game View shows without a tab
    change; a graph window sharing the Game View's dock node stays in front.
  - **The Debugger panel** ("Visual Script Debugger", docked with the Console): the call stack (with a stop picker),
    the frame's values as the canvas shows them (loops' positions, the computed nodes by title) with a one-shot Evaluate
    whose result belongs to the stop and frame it was evaluated in and which lists the names a formula can read, every
    breakpoint of the project (Disable All, Remove All, Break on errors), and the running instances. `graph.debug-panel`
    picks its tab, stop and frame.
- **The game beside the graph**: while the game plays and the Game View is not visible (another tab in front, or
  closed), each graph window shows the game in a corner of its canvas, by default bottom-right, at 0.3 of the canvas's
  width (at least 12 F, at most 0.6 of the canvas), framed in the play colour (the border drawn over the image). Its
  header says "Game · Click to play" while the game does not have the input; the game takes input only through the
  Game View. Expand, or a click on the image, brings the Game View forward with the input; Beside docks the Game View
  beside the window; Close hides the preview, and the window's view menu shows it again (a per-user setting). Dragging
  the header moves the preview to another corner, and dragging its inner edge resizes it. The editor renders the game
  for the preview only when the Game View rendered nothing that frame (`WantsHiddenGameRender`), so the scene renders at
  most once per frame; the preview says "No camera renders the game" only once the game has rendered without an image.
  **Game View Beside** splits the graph window's dock node at the start of the next frame: the Game View goes above the
  graph at 0.45 of the height, or to its left when that shows a larger game, leaving the graph at least 20 F. A floating
  graph window joins the Game View's node (or the Scene View's) and splits it, also when the Game View shows, and a
  closed Game View opens. Beside is offered from the preview, the Game View's toolbar (while a graph window shares its
  node), the window's and the editor's View menus, a one-time hint the first time Play hides a graph window, and
  `graph.open {besideGameView}`, which on a window the call opens waits for the window's first draw (at most 10 frames).
  The outcome shows in `graph.editors`' `gameView.beside` and is toasted unless Applied; it is never a history step.
- **Window captures**: `viewport.screenshot {window}` cuts one editor window out of the editor's image at the rectangle
  ImGui drew it (`FindEditorWindowRect`, title bar and tab included), scaled to the back buffer (`Image::Crop`).
  `StradaEditor --maximized` and `--window-size WxH` make captures reproducible.
- **Per-user views** (`VisualScriptViewSettings`, `<user data>/Editor/VisualScriptViews.json`, version 2; never in
  `.sgraph` files, the undo history or exported games): the panes' widths and visibility (shared by every window), the
  toggles (Context palette, Reduce Motion, scroll mode, Snap to Grid, Move Comment Contents), the 20 recent palette
  entries and 10 recent types, the last comment colour and variable type, the hints seen, the debugger's and the live
  game's options (break on errors, Wire Values, Show Flow While Playing, the outline of what does not run, the game
  preview's corner, width and visibility), and per project (at most 32, the most recently used first) the graph windows
  that were open in tab order and, per graph (at most 256), the sub-graph shown last, a view per sub-graph (at most 64),
  the context entity, the bookmarks (9 slots, names of at most 64 bytes) and the breakpoints (at most 256 per graph, in
  node order: node, type, enabled, condition, hit count). A version-1 file is read with version 2's members at their
  defaults and written as version 2 at the next save point. An older editor reads a version-2 file as nothing, starts
  with default views and overwrites the file at its next save point, so going back to it loses every view. Projects are keyed by `RecentProjects::NormalizePath` of their file when the file is read: another
  spelling of the same file finds its entry, a hand-written repeat of a project is skipped with a warning, and a
  directory renamed in letter case only keeps its windows and views on file systems that ignore case. It is read at
  start and written when graph windows close, when the project changes and at exit, never per frame; what is not valid
  in it is skipped or clamped, each with a warning.
- **Keys and mouse**: keys are routed in four classes. Editing keys (Delete, Ctrl+Backspace, Shift+Delete,
  Ctrl+Shift+Backspace, Ctrl+X, Ctrl+C, Ctrl+V, Ctrl+D, Ctrl+A, Ctrl+F, Ctrl+Shift+F, F2) are claimed with
  `ImGui::Shortcut(chord, ImGuiInputFlags_RouteFocused)` every frame a window is drawn, so they beat the editor's global
  chords while it is focused, and go to the focused part (the canvas; My Script's row; the element Details shows); a
  thin focus frame shows which part has the keyboard, and while one of the window's popups has the keyboard the window
  still keeps these keys from the editor. While Details shows a My Script row, Delete, Shift+Delete, F2 and Ctrl+D act
  on it from any part of the window, opening a hidden My Script over the canvas for them. Canvas keys are matched as exact
  chords, modifiers included, while the canvas has the keyboard and no text field does; while Space is held, a typed
  character opens the palette with it as the query. Escape is owned by the canvas: it cancels the gesture, else closes
  an overlay pane or the find bar, else clears the selection. The editor's global chords (Ctrl+S, Ctrl+Z, Ctrl+Y, the
  play keys, ...) are never canvas keys, and no key is one macOS keeps for itself. The Timeline drawer is the fourth
  class: while it has the keyboard it owns its keys, Delete and Ctrl+A arriving through the window's claim of the
  focused chords and the others (the arrows, Shift and the arrows, F, Esc) matched exactly under the drawer's key owner;
  Esc cancels the drawer's drag, and otherwise gives the keyboard back to the canvas. "Ctrl" is Cmd and "Alt" Option on
  macOS. `VisualScriptShortcuts` and `VisualScriptMouseGestures` (`VisualScriptShortcuts.h`) are the tables the key
  handler, the menus, the shortcut sheet and this document read, and a test checks that every row of them is a row here:

  | Group | Key | Action |
  |-------|-----|--------|
  | Create | `Tab` | Add a node (the palette) |
  | | `Space` | Add a node: tap Space, or hold it and type the node's name |
  | | `C` | Add a comment box (around the selection) |
  | Edit | `Delete` | Delete, keeping the flow through what goes |
  | | `Ctrl+Backspace` | Delete, keeping the flow through what goes |
  | | `Shift+Delete` | Delete without relinking |
  | | `Ctrl+Shift+Backspace` | Delete without relinking |
  | | `Ctrl+X` | Cut |
  | | `Ctrl+C` | Copy |
  | | `Ctrl+V` | Paste at the cursor |
  | | `Ctrl+D` | Duplicate |
  | | `F2` | Rename the selected comment, variable, function or event |
  | Node | `H` | Collapse or expand the selected nodes |
  | | `M` | Disable or enable the selected nodes |
  | Select | `Ctrl+A` | Select every node and comment |
  | | `Esc` | Cancel the gesture; else close a pane or the find bar; else clear the selection |
  | Arrange | `Left` | Nudge left one grid step |
  | | `Right` | Nudge right one grid step |
  | | `Up` | Nudge up one grid step |
  | | `Down` | Nudge down one grid step |
  | | `Alt+Left` | Nudge left one unit |
  | | `Alt+Right` | Nudge right one unit |
  | | `Alt+Up` | Nudge up one unit |
  | | `Alt+Down` | Nudge down one unit |
  | | `Shift+W` | Align tops |
  | | `Shift+S` | Align bottoms |
  | | `Shift+A` | Align left edges |
  | | `Shift+D` | Align right edges |
  | | `Shift+H` | Distribute horizontally |
  | | `Shift+V` | Distribute vertically |
  | | `Q` | Straighten the selection's flow |
  | View | `F` | Frame the selection (everything without one) |
  | | `Home` | Frame everything |
  | | `Ctrl+=` | Zoom in |
  | | `Ctrl+-` | Zoom out |
  | | `Ctrl+0` | Zoom to 100 % |
  | | `Ctrl+1` | Go to bookmark 1 |
  | | `Ctrl+2` | Go to bookmark 2 |
  | | `Ctrl+3` | Go to bookmark 3 |
  | | `Ctrl+4` | Go to bookmark 4 |
  | | `Ctrl+5` | Go to bookmark 5 |
  | | `Ctrl+6` | Go to bookmark 6 |
  | | `Ctrl+7` | Go to bookmark 7 |
  | | `Ctrl+8` | Go to bookmark 8 |
  | | `Ctrl+9` | Go to bookmark 9 |
  | | `Ctrl+Alt+1` | Store bookmark 1 |
  | | `Ctrl+Alt+2` | Store bookmark 2 |
  | | `Ctrl+Alt+3` | Store bookmark 3 |
  | | `Ctrl+Alt+4` | Store bookmark 4 |
  | | `Ctrl+Alt+5` | Store bookmark 5 |
  | | `Ctrl+Alt+6` | Store bookmark 6 |
  | | `Ctrl+Alt+7` | Store bookmark 7 |
  | | `Ctrl+Alt+8` | Store bookmark 8 |
  | | `Ctrl+Alt+9` | Store bookmark 9 |
  | | `Shift+M` | Show or hide the minimap |
  | Navigate | `Ctrl+[` | Back |
  | | `Ctrl+]` | Forward |
  | Find | `Ctrl+F` | Find in this script |
  | | `Ctrl+Shift+F` | Find in the project |
  | Timeline | `Delete` | Remove the selected keys |
  | | `Ctrl+A` | Select every key of the visible tracks |
  | | `Left` | Move the selected keys 0.01 s earlier |
  | | `Right` | Move the selected keys 0.01 s later |
  | | `Shift+Left` | Move the selected keys 0.1 s earlier |
  | | `Shift+Right` | Move the selected keys 0.1 s later |
  | | `F` | Frame the selected keys (everything without any) |
  | | `Esc` | Cancel the drag; else give the keyboard back to the canvas |
  | Debugging | `F9` | Add or remove breakpoints on the selected nodes |
  | | `Ctrl+F9` | Enable or disable the selected nodes' breakpoints |
  | | `Ctrl+F10` | Run to the selected node (while playing) |
  | Debugging (while stopped) | `F5` | Continue: the stopped executions, then the game |
  | | `F10` | Step over |
  | | `F11` | Step into |
  | | `Ctrl+F11` | Step into (macOS keeps F11 for Show Desktop) |
  | | `Shift+F11` | Step out |
  | Help | `F1` | Show these shortcuts |

  | Gesture | On | Action |
  |---------|----|--------|
  | `Click` | Node | Select it (Shift adds, Ctrl toggles) |
  | `Click` | Background | Clear the selection |
  | `Drag` | Node | Move the selection |
  | `Drag` | Comment title bar | Move the comment, with its contents while Move Comment Contents is on (Alt: the other way) |
  | `Drag` | Comment edge or corner | Resize the comment |
  | `Press` | Collapsed node's pin dot | Choose one of its pins to wire from |
  | `Drag` | Background | Select within a rectangle (Shift adds, Ctrl removes) |
  | `Drag` | Pin | Draw a wire from it |
  | `Ctrl+Drag` | Linked pin | Move its links' near ends |
  | `Alt+Click` | Pin | Break all its links |
  | `Alt+Click` | Wire | Break the link |
  | `Double-Click` | Wire | Add a reroute |
  | `Double-Click` | Node header | Open the node |
  | `Double-Click` | Comment title or node comment | Edit the text in place |
  | `Right-Click` | Background | Add a node (the palette) |
  | `Right-Click` | Node, pin, wire or comment | Its menu |
  | `Right-Drag` | Anywhere | Pan |
  | `Middle-Drag` | Anywhere | Pan |
  | `Space+Drag` | Anywhere | Pan |
  | `Ctrl+Right-Drag` | Anywhere | Cut the links the line crosses |
  | `Wheel` | Canvas | Zoom to the cursor (Scroll: Zoom), or pan (Scroll: Pan) |
  | `Ctrl+Wheel` | Canvas | Pan up and down (Scroll: Zoom), or zoom to the cursor (Scroll: Pan) |
  | `Shift+Wheel` | Canvas | Pan sideways |
  | `Horizontal Wheel` | Canvas | Pan sideways |
  | `Mouse Back` | Window | Back |
  | `Mouse Forward` | Window | Forward |
  | `Drop` | Pin or canvas | Set a value or add a node from a dropped asset, entity or My Script row |
  | `Click` | Timeline key | Select it (Shift adds, Ctrl toggles) |
  | `Drag` | Timeline curves | Select the keys within a rectangle (Shift adds, Ctrl removes) |
  | `Drag` | Timeline key | Move the selected keys as a block: times snap to 0.01 s (Alt: free), Shift keeps one axis |
  | `Drag` | Timeline tangent handle | Set the key's two tangents (Alt: this one alone) |
  | `Double-Click` | Timeline curve or event lane | Add a key there with the curve's value, or an event |
  | `Right-Click` | Timeline key | Interpolation, Tangents and Delete |
  | `Wheel` | Timeline curves | Zoom time around the cursor |
  | `Ctrl+Wheel` | Timeline curves | Zoom values around the cursor |
  | `Right-Drag` | Timeline curves | Pan |
  | `Middle-Drag` | Timeline curves | Pan |

  The scroll mode (view menu; Pan by default on macOS, where trackpads are usual, else Zoom) decides what the wheel does.
  Over the live widget the wheel still zooms and right and middle drags pan; a drop that a pin cannot take shows the
  not-allowed cursor.

## 12. AI automation

- `CommandRegistry` (editor): named commands with a description, JSON-schema parameters and a handler returning a
  JSON result or a structured error. Long operations complete asynchronously. All commands execute on the main
  thread; the active project/scene state is identical whether a human or an agent drives the editor.
- `AutomationServer`: JSON-RPC 2.0 over TCP bound to `127.0.0.1`, newline-delimited messages, authenticated by a
  random 256-bit token (first request `authenticate`, constant-time comparison). A network thread owns the sockets
  (framing, size limits, parsing, authentication) and never touches editor state; the editor layer runs queued requests
  on the main thread once per frame (`ProcessRequests`). The editor writes `{pid, port, token, project, version}` to
  `<user data>/Editor/Instances/<pid>.json` (user-only permissions), rewrites it when the open project changes and
  removes it on exit. `AutomationInstanceFile` is its only writer: what the file states changes only when a write
  succeeds, and a failed write (on Windows the atomic replace fails while another process has the file open, as the
  strada tool does when it scans every 50 ms) is retried on later frames, at most every 100 ms by a `steady_clock`
  deadline, with the newest project, until one succeeds. A streak of failures is logged once as an error; what ends it
  is logged once at info, naming the project: the write that succeeds, or a change back to the project the file already
  states. Nothing touches the file system while nothing is pending. At exit the file is removed, even if no write
  succeeded, and a pending retry is dropped.
- The editor model (`EditorContext`: scene, file, undo `CommandHistory`, selection; `EditorOperations`) is shared by UI
  panels and automation. The protocol and the command reference are in [Automation.md](Automation.md).
- `strada` tool (C#, `StradaTool`, no dependencies beyond .NET): `strada mcp` (MCP stdio server written against the
  protocol; tools are the editor commands, names `domain_action`, schemas from `editor.commands`; attaches to a running
  editor or starts one on demand, and lists the tools from a headless editor started for that when none runs; headless
  editors it starts close with it through the editor's `--parent-process`; screenshots returned as image content;
  failed commands are tool results with `isError`; strada's own tools search and import Poly Haven HDRIs),
  `strada call <command> [json]`, `strada launch`, `strada commands`, `strada instances`, `strada hdris`, `strada hdri`,
  `strada models`, `strada model`, `strada textures`, `strada texture`. Editors ignore SIGPIPE (`Platform::IgnoreBrokenPipeSignal`), so the ones strada starts with a
  window outlive its output pipes. Each editor strada starts gets pipes of its own, and on Windows strada makes its own
  standard handles non-inheritable at its start (`StandardHandles.KeepFromChildProcesses`: every process .NET starts
  there inherits every inheritable handle), so a program reading strada's output sees it end when strada exits while an
  editor it started runs on.
- Command domains: `editor.*` (status, undo, redo, commands, quit, the Entity menu as users see it, the recent
  projects, the File menu's dialogs), `start.*` (info, show: the start screen), `project.*` (info, create from a
  template, open, close, settings, export, the templates, the examples and copy-example, and the agent kit's agents and
  setup-agents, which needs the user's consent: ConsentRequired without `confirmed`),
  `scene.*` (new, open, save, hierarchy, settings, dump), `entity.*` (create with components, delete, duplicate,
  rename, reparent, find, get, select), `component.*` (types, add, remove, get, set with partial JSON patch),
  `asset.*` (list, get, select, import, refresh, browse and drop in the content browser, move, delete, create-folder,
  move-folder, delete-folder),
  `material.*` (create, get, set; edits are undoable without marking the scene modified), one domain per data asset
  type (`<domain>.create`, `get` and `set`, from the type's registration; `set` is undoable without marking the scene
  modified, §6.4), `graph.*` (visual scripts:
  create, get, set and check documents, checked strictly, with the scripting runtime's analysis; edit applies a batch of
  the graph window's own edits as one step, with labels, quick fixes as ready operations, hints and the nodes of other
  scripts an edit breaks; arrange lays a graph out as the window's Arrange does, copy copies elements for edit's Paste,
  interface tells what a graph offers others; edits are undoable without marking the scene modified, and revisions refuse
  edits of a graph that changed since; library and types list the runtime's node kinds and types; open, close, editors and
  palette drive the graph windows; find searches scripts; the debug commands, `GraphDebugCommands` over the debug
  session, set breakpoints and targets, read the debugger's state, stopped frames, live values, running instances and
  the live log, continue, step and break, trace frames, and wait for a stop: `graph.wait` completes asynchronously on the
  main thread exactly once, also when play stops or the editor closes (Cancelled), and never blocks a frame;
  `graph.debug-panel` drives the Debugger panel), `prefab.*`,
  `script.*` (create from template, build with diagnostics, classes and fields), `play.*` (start, stop, pause, step, advance N frames, state),
  `input.*` (inject keys/mouse during play), `ui.*` (where the screen-space UI's elements and buttons are in the game
  view), `physics.*` (query: the playing scene's rays, casts and overlaps through `Scene`'s queries, read-only and
  Unavailable while nothing plays), `viewport.*` (screenshot, camera, frame entities, overlays, renderer statistics),
  `inspector.*` (the Add Component menu, as users see it), `log.*` (read since index), `test.*` (run a test scene and
  return results). Renderer settings are scene settings (`scene.settings`).
- Claude Code integration: `.mcp.json` registers `strada mcp` (through `dotnet run`, so it works in any checkout);
  the `strada-make-game` skill in `.claude/skills/` documents building games with the tools. It reads correctly both
  there and copied into a game project by the agent kit, which registers the built `strada` in the project's own
  `.mcp.json` (§11).

## 13. Runtime and export

`StradaRuntime` is the game player, with no editor code. It runs the exported game whose configuration, `Game.sgame`, is
next to the executable (in `Contents/Resources` of a macOS app bundle), another exported game (`--game <Game.sgame or
its directory>`), or a project from its directory with its last script build (`--project <file.sproj>`). `Game.sgame`
holds the project's settings, `{ "Strada": { "Version": 1, "Type": "Game" }, "Game": { <ProjectSettings> } }`, with the
asset directory and the script assembly relative to the game's directory (`Project::OpenGame`, `ProjectFileKind::Game`).

- The window (title, size, fullscreen, vsync, resizability) follows the game's window settings, read before the engine
  starts (`Project::ReadWindowSettings`). The player opens the asset directory, starts .NET only when the game has
  C# scripts or visual scripts (`Project::UsesVisualScripts`: its asset directory registers `.sgraph` files; a game
  with visual scripts only starts the scripting runtime without a game assembly; exported games carry
  `Strada.ScriptCore.dll` in `Scripts/`, projects use the engine's; a project whose C# project was never built does not
  start), loads the start scene (or `--scene <path in Assets>`) and runs it
  through a `SceneRunner` with scripts, physics and audio. Every frame the primary camera's view is rendered at the
  window's framebuffer size and drawn into the swapchain image (`TextureBlitter`); without a primary camera the
  screen-space UI, sprites and text are drawn over black, and a scene with neither leaves the window black (logged
  once). `Application.Quit` closes the player.
- The player attaches no visual-script debugger: its graphs run untraced, so the debugger costs it nothing, and
  breakpoints, traces and the editor's views never ship (they live in the editor's user data, not in the project).
- `--headless` runs without a window, rendering or sound (no GPU needed); the scenes see the game's window size.
  `--frames N` and `--screenshot <png>` serve smoke tests.
- `--profile <file>` (§4.4) starts a recording before the game's layer attaches, so its first frame is the main loop's
  first. At exit it logs the medians of the history (`ComputeProfilerMedians`: the frame and its 99th percentile, each
  subsystem, each thread name, each GPU pass and each zone name, one `Profile ...` line each), which plan targets are
  read from, and saves the capture (the last `HistoryFrames`, 600). A file in a folder that does not exist is a usage
  error (2); a capture that cannot be saved is logged and makes a run that would have exited with 0 exit with 1. Dist
  players have no profiler and refuse the option.
- `--test` runs headless with a fixed time step (`--timestep`, default 1/60 s) until the scripts call
  `TestReporter.Finish`, quit, or `--timeout` game seconds (default 60) pass. It logs a summary and exits with the
  number of failures: failed checks plus script exceptions, plus one when the scripts did not finish testing, at most
  100. A game that cannot start exits with 1, a wrong command line with 2. The summary ends with the running scene's
  physics state hash (§8) and the fixed step it was taken at: after the run's last step, or after step n with
  `--hash-step n` (taken inside the frame that runs it, so runs at different `--timestep`s compare; a run that ends
  before step n says so), which makes two test runs of one game comparable step for step. The steps are counted in the
  scene the run ends in: a scene load starts the count again.

`GameExporter` (Project module) exports a project for the host platform; the editor's File > Build Game and
`project.export` run it on a worker thread:

```
<Out>/<GameName>[.exe]             The player (StradaRuntime), renamed after the game
<Out>/Game.sgame                   Game configuration
<Out>/Assets/                      The asset directory with AssetRegistry.sreg (hidden files stay behind)
<Out>/Scripts/                     The scripts built in Release; Strada.ScriptCore.dll, .runtimeconfig.json, .deps.json
<Out>/ThirdPartyNotices.md
```

`Scripts/` ships only with games that have C# scripts or visual scripts; with visual scripts only it holds the scripting
API alone (`GameExportSettings::UsesVisualScripts`, gathered by `Prepare`), and `Game.sgame` keeps naming the assembly
the project would build.

- Windows: the MSVC runtime DLLs go next to the executable. Release and Dist builds stage them in `Redist/` next to the
  player; Debug builds have none (the debug runtime is not redistributable), so their exports run where Visual Studio
  is installed.
- macOS: `<Out>/<GameName>.app` holds the player in `Contents/MacOS`, the files above in `Contents/Resources`, and the
  Vulkan loader and MoltenVK in `Contents/Frameworks` (the build stages them from the Vulkan SDK in `Vulkan/` next to
  the player) with MoltenVK's driver manifest in `Contents/Resources/vulkan/icd.d`. The player loads the bundled
  loader, which finds the driver there. Signing the bundle for distribution is left to the developer.
- Linux: the system's Vulkan loader and drivers serve the player.
- Before an export the asset directory is refreshed and its registry saved, so the game's registry lists every asset
  and the player never rewrites it. Games are built from the saved files; the editor points out unsaved changes.
- The game is assembled next to the output directory and moved into place at the end: a failed or cancelled export,
  or scripts that do not compile, leave an earlier export untouched. Only new, empty or previously exported directories
  are written to. The editor's own script builds wait while an export builds the same C# project.
- Games with C# scripts or visual scripts need the .NET 10 runtime: installed, or shipped by the developer in
  `dotnet/` next to the executable, where the player looks first.

Dist builds of the runtime have no console window on Windows, log only warnings and errors, to a file in an exported
game's data folder (§13.1), and compile out asserts and script debug lines.

### 13.1 Identity and the game data folder

Every Strada program shares one per-user folder, the engine's (`FileSystem::GetUserDataDirectory`: the editor's state,
its automation instances, the logs and crash reports), except an exported game. A game made with Strada has a folder of
its own beside it, one per game and per user, named by the game's identity: its log, its crash reports, the player's
input bindings (`Input/`, §7.4), and the saves and player's settings to come go there.

**The identity.** `ProjectSettings::Identity` holds `Company` and `Product`, both optional strings, compatible fields of
project format 1: an engine from before them warns about the unknown key and drops it on its next save. `Game.sgame`
carries them as it carries every setting. `GetGameIdentity` resolves them into a `GameIdentity` (Core,
`GameIdentity::Resolve`). A part names something only when it holds an ASCII letter or digit
(`GameIdentity::HasLetterOrDigit`), the only characters a folder name and a bundle identifier both keep: a `Product`
without one (empty, spaces, text that is not ASCII) is the project's `Name`, and a `Company` without one means no company
level, so a project that sets neither is named by its name alone. They are edited in the Project Settings panel and through `project.settings`. The macOS bundle
identifier is `org.strada.<company>.<product>`, each part with every character other than an ASCII letter or digit
made `-`; a company with no letter or digit is left out, and a project that sets neither keeps `org.strada.<name>`
(with `game` after the dashes of a name that has no letter or digit), as before the identity existed.

**The folders**, pure functions of their inputs in `UserDataLocations` (Core), which take the platform as a parameter so
that each platform's names are tested on every platform:

| Platform | Per-user root (`FindRoot`) | The engine's folder | A game's folder (`MakeGameDirectory`) |
|---|---|---|---|
| Windows | `%APPDATA%`, else `%USERPROFILE%\AppData\Roaming` | `<root>\Strada` | `<root>\<Company>\<Product>` |
| macOS | `$HOME/Library/Application Support` | `<root>/Strada` | `<root>/<Company>/<Product>` |
| Linux | `$XDG_DATA_HOME`, else `$HOME/.local/share` | `<root>/strada` | `<root>/<company>-<product>`, lower-cased |

- A variable that is unset, empty or not an absolute path (by the platform's rules) counts as unset, so no folder is ever
  relative to the working directory; `strada` reads the variables by the same rule (`UserDataDirectory.cs`), so it looks
  for editors where they write. When none names a root, the folders go beside the executable: the engine's is
  `<executable directory>/UserData` (`MakeFallbackEngineDirectory`), and `<executable directory>/GameData`
  (`MakeFallbackGameRoot`) stands in for the root of the games' folders.
- Each company and product is made portable (`FileSystem::MakePortableFileName`: ASCII letters, digits, spaces, `-` and
  `_`; device names Windows reserves get a trailing `_`) and cut to `MaxFolderNameLength` (64) bytes, so Linux's joined
  name has at most 129. A company that then holds no ASCII letter or digit is none, and such a product is `Game` (which a
  project reaches only when its name has none either). Without a company there is no company level (`<root>/<Product>`,
  `<root>/<product>` on Linux).
- No game's folder is the engine's or inside it, with a root or without one: a first name below the root that is
  `Strada` in any letter case (a company named so, or a game without a company) gets a trailing `_`, and without a root
  the games' folders and the engine's are apart.
- Two identities can name one folder (on Linux `a-b` and `c` as `a` and `b-c`; any two that differ only in what is not
  portable, such as `Café` and `Cafè`; every game without a company whose product and name hold no ASCII letter or
  digit, which share `<root>/Game`): the identity is the developer's to keep unique, and a game whose name is not in
  ASCII sets an ASCII `Product`.
- There is no split into settings, data, state and cache folders, and no variable that moves the root: a test isolates
  a child process by the variable that names its root (`APPDATA`, `HOME`, `XDG_DATA_HOME`), as StradaToolTests do.
  StradaRuntimeTests start every player with a root of its own below a temporary directory
  (`Testing::IsolatedUserRoot`), and so do the games StradaEditorTests' exports start
  (`EditorExport::SetGameEnvironment`), so neither suite writes into the user's folders.
  `ProcessSpecification::Environment` replaces the inherited variable of each name it sets (in any letter case on
  Windows): reproc only appends to the inherited environment, and lookups find the first entry of a name.

**The service.** `GameData` (Core) tells the engine where the running game keeps its data; the host fills it with a
`GameDataScope`, which it holds for as long as the game runs and which gives back what the service held before:

| Host and run | The folder | When the run ends |
|---|---|---|
| `StradaRuntime` running an exported game | `GameData::GetUserDirectory(identity)`: the game's folder below this user's root | kept |
| `StradaRuntime --project` (a development run) | `<project>/.strada/PlayData` (`Project::GetPlayDataDirectory`) | kept |
| `StradaRuntime --test`, of a game or a project | a new folder below the system's temporary folder (`GameData::MakeTemporaryDirectory`) | removed with everything in it |
| The editor's play mode (Play, Simulate) with a project | `<project>/.strada/PlayData` | kept |
| The editor's test runs (`test.run`) | a new temporary folder | removed when the run stops |
| The editor while editing, play mode without a project, tools | none | |

- The player fills it after opening the game and before its scripts start, and empties it when the application is
  destroyed, after the layers detached and the scripts' `OnDestroy` ran; play mode fills it before the scene starts
  and empties it after the scene stopped (`PlayMode::Stop`, also when the scripts quit or play mode is destroyed).
- Playing in the editor or running a project therefore never writes into the shipped game's real folder. `.strada/` is
  left out of version control by the `.gitignore` new projects get and out of project copies (`ProjectCopy`), and test
  runs start from no data and leave none.
- The service names the folder and creates nothing: whoever writes into it first creates what it writes. Empty means no
  game runs, and callers then persist nothing rather than guess a folder. `GameData::GetDirectory` is thread-safe; scopes
  are made and destroyed on the main thread.
- A development run keeps the shared folder for what is not game data: the editor's and `StradaRuntime --project`'s logs
  and crash reports stay in `<user data>/Logs` and `<user data>/Crashes`.
- An exported game logs and crashes below its own folder (`<game data>/Logs/<executable>.log`, `<game data>/Crashes`).
  The entry point decides this before logging starts, from the game file beside the executable
  (`Project::FindExportedGameFile`: `Game.sgame` there, else in `Contents/Resources` of a macOS bundle, the file the
  player runs by default): `Project::ReadGameIdentity` reads its header, `Name` and `Identity` with the field traits
  `OpenGame` uses, without opening it and without logging, and resolves them with `GetGameIdentity`; the folder is
  `GameData::GetUserDirectory` of that identity, the computation the player fills the service with. A game that opens
  therefore logs where its data goes (the game file fuzz case checks that every file `OpenGame` opens gives the entry
  point the identity of the opened settings). Only a file whose identity cannot be read keeps the shared folder: one
  that is missing or not JSON, one that is not a game or whose header `OpenGame` refuses, one whose `Name` is empty or
  is not a string, one whose `Identity` is not what `OpenGame` reads. A file whose identity reads but whose other
  settings are damaged (a `Window` that is not an object, no physics layer) logs below the game's folder. Either way the
  player logs there why the game cannot run, so the reason sits where the game's log is looked for.
- The log follows the program, the data the run: an exported game's executable logs below its folder whatever it is
  asked to run (`--test`, whose data is temporary; `--game` or `--project` naming another), and the player of a build,
  which has no game file beside it, logs in the shared folder whatever it runs, also an exported game given by `--game`.
- `Log::Init` and `CrashHandler::Install` create `Logs` and `Crashes` as the program starts, so an exported game's
  folder exists from its first run: the service still creates nothing, the entry point is that folder's first writer.
  A crash of an exported game reports into `<game data>/Crashes` from then on (CrashHandlerTests run a program on the
  entry point that aborts, copied beside a game file).
- Scripts have no API for the folder: the player's input bindings (`<game data>/Input/<the actions asset's file
  name>.json`, §7.4 "Rebinding") are its first user beside the log and the crash reports, read when the actions load and
  written by the engine when `Input.Rebind`, a capture or `Input.ResetBindings` changes one, with no path exposed. Saves
  bring theirs with their nodes.

## 14. Build configurations

| Config | Optimization | Asserts | Logging | Vulkan validation | Profiler (§4.4) |
|--------|--------------|---------|---------|-------------------|-----------------|
| Debug | off | on | all | on if available | zones and GPU passes |
| Release | on (+ debug info) | on | all | opt-in (`--validation`) | zones and GPU passes |
| Dist | on | off | warnings+ to file | off | macros compiled out; `Profiler::Start` refuses |

## 15. Testing

- `StradaTests` (doctest): every module; GPU tests create a headless device and skip cleanly when no Vulkan
  device exists; renderer golden-image tests compare against `Tests/Data/Golden` with a tolerance. Fuzz tests damage
  the FeatureTest project's scenes, prefab, material, asset registry, project file and visual scripts deterministically
  (members and elements removed or repeated, values of another kind or extreme): every result must be refused or
  repaired, what loads must save and load again, scenes and prefab instances that load must run with physics and sound,
  graphs that load must run in a scene, and project settings that load must simulate a scene (`STRADA_FUZZ_SCALE`
  multiplies the rounds). Crash handling runs in a child process (`StradaCrashTester`, and `StradaGameCrashTester`, which
  aborts on the entry point and is copied beside a game file to show where an exported game's reports go). Visual scripts are tested with
  the shared documents of `Tests/Data/VisualScripting` (every built-in kind of node, valid and canonical): the format
  (the writer's golden file, every refusal and repair, the layout, public variables), the native facade against the
  real runtime (every entry point's statuses and single answer, documents sent once per change, a broken document
  leaving the working program running, graph instances blocking game-assembly loads, `CanConnect` against analysis) and
  scenes running C# scripts and graphs together (lifecycle and event order, prefabs, live variables, live edits,
  failures as script exceptions, the step budget, C# calling graphs, remote variables, broadcasts, libraries edited while
  running). Every graph a scene of `Projects/` uses, and every library those graphs call, must load strictly, analyze
  without errors (each with its own handle, against copies of its project's graphs) and resolve its nodes exactly or by a
  redirect, so an API change that breaks one needs a redirect entry; the graphs FeatureTest's scenes run must use every
  built-in kind of node and every `[VisualNode]` member of its scripts. `VisualScriptNodeCoverageTests` builds a graph for
  every node of the library (the API's and FeatureTest's game members), analyzes each (no Error, resolved exactly) and runs
  them in simulated scenes with physics, manual audio and restored input; a node that does not run on its defaults has a
  recipe (inputs, settings, components, a child target, held keys or buttons), a recipe for every member of its type, or
  a checked exclusion naming a graph FeatureTest's scenes run that holds it. The fuzz test of damaged graphs runs, every
  third round, the shared language document (expressions, arrays, strings, generics, a timeline). ctest runs it as
  shards by module folder of `Source/`, one entry each, defined once in `Tests/StradaTests/Shards.cmake`:
  `StradaTests.Asset`, `.Scripting` (`Script/` and `Scene/`), `.Rendering` (`RHI/`, `Renderer/` and `ImGui/`, the GPU
  tests) and `.Core` (every other folder). `StradaTests.Partition` checks them as the editor's partition entry checks
  its own (`strada_add_test_shards` in `cmake/StradaTestShards.cmake` registers the three suites' entries). Every test
  source passes alone in its own process, so no test depends on what another file's tests left behind.
- `StradaEditorTests`: automation commands executed headlessly on a temporary project. ctest runs it as shards, one
  entry each, defined once in `Tests/StradaEditorTests/Shards.cmake` as doctest source-file filters:
  `StradaEditorTests.AutomationFuzz`, `.Graph` (the graph and debug command tests and `GraphEditFuzzTests`),
  `.VisualScriptPanels` (the visual script editor's panels, panes, windows and canvases), `.VisualScriptModel` (the rest
  of the visual script editor) and `.Core` (every other file). `StradaEditorTests.Partition` lists the test cases
  unfiltered and through each shard's filters and fails when one is in no shard or in two, or when a shard selects
  nothing; a shard that passes 450 s on CI's Windows MSVC Debug job is split again by source-file patterns. A fuzz test
  sends every command parameters built from its schema and the editor's state (mostly names of what exists, plus values
  to refuse; damaged graph documents to `graph.set` and `graph.check`), while editing and playing: every request answers
  once without InternalError, the scene stays loadable, undoing and redoing everything restores it, and nothing outside
  the project changes. The graph commands are tested with and without the scripting runtime: strict refusals naming
  every issue, renamed variables, layout, undo restoring the file's bytes, the library's filters, and edits of running
  graphs and their variables while playing; the debug commands run against a real scripting runtime and play mode, every
  command, parameter and error, a sequence fuzz of random debug sessions (seed 11700) that must leave a consistent
  state, and the documented debugging session (`Tests/Data/VisualScripting/Agents/Debugging.json`, run headless; a test
  holds Automation.md, VisualScripting.md and the game-making skill to its steps and their answers, verbatim and in
  order, as one block); `graph.edit` also runs the docs' example batch
  (`Tests/Data/VisualScripting/Agents/Spinner.json`, which a test holds the docs and the game-making skill to,
  verbatim). `GraphEditFuzzTests` sends random batches drawn from a document's own nodes, pins, variables and functions,
  with labels and every operation kind: each either fails naming an operation and changes nothing, or writes one undo
  step that loads strictly and undoes byte for byte, and a dry run answers what the call then does. With
  `STRADA_PERFORMANCE_TESTS=1`, the commit tests print the phases of a commit (`VisualScriptCommitTimings`: Normalize,
  Check, Text, ReadBack, Compare, Write, SetDocument, Adopt, Analysis, Canvas, Index) for 2,000 and 200 nodes, for a
  move and for a new link, and check wall-clock bounds of three times the measured figures; without it they check that
  every phase that does work is filled. The visual script editor (§11.1) is tested headless: its pure layers with plain
  cases (the node library and analyses from `Tests/Data/VisualScripting/Editor`, checked against the real runtime's),
  its panes and graph windows drawn through `HeadlessImGui` with real mouse and key input, the window manager and the
  `graph.*` window commands between draws, the canvas's draw counters on graphs of up to 10,000 nodes, and the key and
  gesture tables against §11.1 of this document. A copy of the Blocks sample plays through `input.set` the way a player
  plays it: it opens at the title's view with its sign shown and a field of view widened for the 4:3 game view, then
  takes clicks on its menus' buttons where `ui.layout` shows them, the arrow keys and Enter in its menus, and Escape, P,
  R and Space on its title, game, pause and game-over screens (Space mashed through the end of a game leaves the
  game-over screen up), down to the title's Quit, which ends play mode.
- `StradaRuntimeTests`: the `StradaRuntime` executable runs games written to temporary directories (exported and project
  layouts, test runs and their exit codes, games whose only scripts are visual scripts, among them one whose graph calls
  a function library and broadcasts to two entities, development-only nodes the project runs and the exported game
  skips, command-line errors, a windowed run's presented image, its screenshot and its `--resolution`). ctest runs it as
  shards defined in `Tests/StradaRuntimeTests/Shards.cmake`: `StradaRuntimeTests.Samples` (`SampleTests.cpp`: the
  sample projects, each played several times), `.Templates` (`TemplateTests.cpp`: a project made from each template)
  and `.Core` (every other file), with `StradaRuntimeTests.Partition`; the helpers they share are in
  `RuntimeTestUtilities.h`.
- `StradaToolTests` (xUnit): the strada command line, instance files and editor discovery, the MCP server against a
  stand-in editor, Poly Haven downloads against a stand-in server, and MCP sessions with the real editor started
  headless (`STRADA_EDITOR`; ctest sets it), among them an agent's whole workflow: a project, a script it writes and
  builds, a scene that uses it, a test run in play mode, and an export whose player passes the same checks; and the
  documented debugging session through `strada mcp`, after which the scene is saved and exported, the exported graph
  holds the fix and the exported player runs it without a problem.
- `ScriptCoreTests` (xUnit): math types, the visual script VM and an API-coverage test asserting every public
  `Strada.ScriptCore` member is used by the FeatureTest scripts. The VM's tests build runtimes of their own over fake
  engines (`IVisualScriptEnvironment`) and never touch native code: the library (inclusion, purity, resolution,
  identical bytes in every culture), analysis and its diagnostics, every node kind, suspension and timing, budgets,
  live edits, game-assembly reloads, the protocol of the entry points, the C# API, the debugger (stops by node kind,
  conditions, steps across every construct, chains, live edit of stopped executions, values and their currencies,
  traces and recordings, the protocol with a fuzz of mutated requests, a differential test against plain runs, zero
  allocations when untraced), the live log and carry plans, and dependents' kept analyses against fresh runtimes. The coverage test
  reads the scripts' compiled metadata: references to every type, method (accessors and operators included, overloads
  matched by signature) and field; Script's callbacks and protected constructors count when derived types override or
  call them; constants and enumerators, which compile to literals, count through their types.
- `Projects/FeatureTest`: scenes that use every component (checked by `StradaTests`) and scripts that call the entire
  scripting API through `Strada.Testing`, one script per area (entities, transforms, components, physics, characters,
  audio, assets, math, input, application and time, the testing API, attributes, script lifecycles, the UI, visual
  scripts) with a coordinator that loads the second scene, which finishes the run. The graphs its scenes run use every
  built-in kind of node and every `[VisualNode]` member of its scripts (`GameNodes.cs`, and members of
  `VisualScriptTests`), and report their checks through `TestReporter` nodes: `Assets/Scripts/Features.sgraph` on the
  visual-script test entity (comparisons, logic and `Select` checked to give false as well as true; expressions, text,
  arrays, generics, game members, a timeline; the pure and latent functions of the library `MathLibrary.sgraph`; a typed
  Send to, and a remote Get and Set guarded by `RunsGraph` of, the box's graph; a broadcast), `Contacts.sgraph` on a
  falling box and ball beside their C# scripts and `Clicks.sgraph` on the UI canvas; `VisualScriptTests` waits for them,
  checks what they counted and drives them from C#. `Tools/AuthorGraphs.py` writes the graphs through `strada mcp`
  (`graph.check`, then `graph.set`, in the order they depend on each other) and runs the tests, so a new built-in kind of
  node or game member gets its check there (`FormatSample.sgraph` is a format sample no scene runs). CMake
  builds the scripts (`StradaFeatureTestScripts`) and `StradaRuntimeTests` runs a copy of the project through the player
  (`StradaRuntime --project ... --test`; exit code = failures).
- `Projects/Blocks`: a sample game made the way an agent makes one, through `strada mcp`: `Tools/Author.py` builds the
  project, its materials, both scenes with their screen (a UI canvas) and the debris and particle burst prefabs, with the
  sounds, the beveled block mesh, the grid texture and the screen's images (a glow, an accent gradient, a gloss, arrow
  keys) that `Tools/GenerateAssets.py` synthesizes (and checks the sounds: lengths, levels, clean ends, a low thump
  for hard drops, clears that grow with the rows), a Poly Haven sunset sky over a neon grid floor and a warm sun high
  behind the board that casts the shadows of the frame, the stack, the falling piece and the panels onto the floor in
  front of it. Falling blocks with pure C# rules (`Board`, `Piece`, `PieceBag` with a look-ahead queue) and an
  `AutoPlayer` that plays the title screen's demo at a person's pace: it rates every place the piece and the next one
  reach with Dellacherie's features and El-Ashi's weights, keeps a well open for three- and four-row clears and holds a
  piece when the one that comes instead fits clearly better, searching rows of bits in buffers it makes once (without
  allocations, in a fraction of a millisecond). The `BlocksGame` script (title with the demo, pause and game-over
  states, hold, a three-piece next queue, combos and back-to-back clears, a best score kept in the user's data folder, a
  sound for every action, rows that burst into physics debris and sparks, dust under hard drops), `CameraShake` (smooth
  trauma noise with roll, a spring kick for hard drops stepped finely enough for any frame time, the title's view further
  back, which the camera glides to and sways around (a game that opens on its title starts there), a field of view that
  widens on screens narrower than the 16:10 the view was made for, so they show as much across, and an event once it has
  moved each frame), its `Hud`, which brings the scene's canvas to life on the UI system (a score panel with the rows
  toward the next level, the controls as keycaps, HOLD and NEXT labels that follow the 3D panels' headers through
  `WorldToScreenPoint` in a canvas of pixels, the title in block letters that bob over the demo, shown once the camera
  has glided back far enough for them to clear the board, with Play and Quit, pause and game-over menus that fade and
  rise in, flashes, and numbers formatted alike in every language), its `Menu`s (buttons the mouse and the keys share:
  the focused one lights up and grows over a glow, a held one shrinks, keys click through `Click()`; the game-over menu
  waits a moment before it takes keys and leaves Space, the hard drop, to the game), `Callouts` that pop
  over the board ("TETRIS!", "BACK-TO-BACK", "LEVEL UP!") in slots that each keep room for their tallest callout's pop
  and drift, so callouts never run into one another, and a test scene whose `BlocksTests` check the rules, the auto
  player and the callouts' rooms, play the game and its demo, click every menu button (Play starts a game, Resume,
  Restart and Quit, Play Again, closed menus take no clicks) and check on every frame that the title leaves the board in
  sight, and that the screen's panels do so on screens from 5:4 to 21:9 (seen through the camera with a fixed aspect
  ratio), in a language that groups thousands with a narrow no-break space. The checks wait in seconds of the game and
  seed `Random`: `StradaRuntimeTests` plays that scene twice at 60 frames a second (the same frames both times) and at
  30 and 144, and runs the game's scene headless.
- `Projects/Showcase`: every engine feature in exhibit areas of an autumn forest clearing, built by `Tools/Author.py`
  through `strada mcp` from Poly Haven assets (fetched by `Tools/FetchAssets.py`, not in the repository) and the meshes,
  textures, icons and sounds that `Tools/GenerateAssets.py` and `Tools/GenerateAudio.py` make, on a ground whose outer
  band (rings of ever more transparent copies of its material, cut into sectors that sort on their own) fades into the
  sky's forest floor: physically based materials (a metallic and roughness sweep, texture sets with normal and
  occlusion-roughness-metallic maps shown as sprites with world-text labels, a neon sign that flickers on), transparency
  (blended glass, an alpha-tested lattice, two-sided banners), lights (a lighthouse whose beams turn, a light show of
  shadow-casting spots, street lamps, the sun and the sky), post-processing (a global volume carries the golden-hour
  look, a local one with a blend distance turns the light show into an evening, and `DayNight` turns day into night by
  dimming the sun to moonlight, weighting in a night volume and dimming the unlit particles of smoke, water and leaves),
  physics (a wrecking ball on a ball joint, a hinged seesaw, a motorized paddle, prefab dominoes, a plank bridge of
  hinges), triggers (a ball launcher, a goal that counts with sound and a particle burst, a pressure plate that bodies
  and characters press to raise a gate), a character controller (a yard of stairs, a ramp, posts to jump, a lift
  (`Lift`, a kinematic platform that waits while anything is under it) up a lookout tower and crates to push),
  particles, 3D sound and streamed music and ambience. The `Flythrough` script opens it with a camera tour along
  `CameraShot` children that cues the exhibits (`ITourCue`; a night for a while, `NightInterlude`, ends with the tour)
  under letterbox bars, a title and captions, with the music played up, then hands over to the camera rig (`CameraRig`,
  with a `CameraShake` child), which flies over the clearing or walks as a first-person character (`Walker`), and to the
  HUD, a UI canvas (`Hud`): a feature list whose rows (`HudFeature`) light up and open as the camera nears their
  exhibits, counters, a hint line for the way the camera moves, and buttons that put the exhibits back, set them off,
  switch day and night and switch between flying and walking; the tour's key plays it again with the exhibits put back.
  `DebugView` draws ballistic paths and trigger bounds. A test scene that `StradaRuntimeTests` plays checks every
  exhibit, the volumes, the night and the tour's night interlude, the tour's cues and replay, the music, the HUD's
  buttons, the free cursor, where the camera flies and the walker climbing, running, jumping, pushing, riding the lift,
  waiting under it, pressing the plate and staying on the ground (its checks do not need the downloads).
- `Projects/PrismBreak`: a game made of visual scripts alone (13 graphs, no C#), a neon brick breaker, built by
  `Tools/Author.py` through `strada mcp`: one batch per graph declares its variables, events and functions first, then
  one batch per region adds its nodes, arranges them and frames them in a titled comment box, each answering no error
  and no warning. It reads input actions (`Input/PrismBreak.sinput`: Move, Launch, Pause, each on the keyboard and a
  gamepad; the cheat keys stay `Event.KeyPressed`), and at its title R rebinds Launch to the next key but R (§7.4,
  "Rebinding"). Its test scene's 33 checks are graphs too (`Tests.sgraph`, a latent function per check called in
  order from a Sequence, reporting through the `Checks` library; the last four drive it through action overrides and
  `Input.Rebind`); `StradaRuntimeTests` runs them through the player ("Tests passed: 33 checks"), plays the title and
  runs the exported game, refusing any line from a visual script; `PlayModeTests` plays it through `input.set` (keys,
  the mouse and an injected gamepad, and a rebinding kept for the next play), and the fuzz test of damaged graphs
  includes its `Game.sgraph`.
- `Projects/Moto`: the circuit of a motorcycle time trial that is to be made of visual scripts alone (no C#, no
  `Scripts` folder). `Tools/Circuit.py` is the layout and its one source: a closed polygon of ten corners, each rounded
  by an arc of its radius, raced clockwise from the start and finish line (lap distance 0) for 2,638.6 m; the lap
  distance gives every point of the center line, its heading and the frame along it, and the cross-section across it
  (a crowned 12 m road, kerbs along the corners, verges, gravel traps on the corners' outsides that widen and narrow
  along the lap, grass skirts down to the ground, then a concrete wall). Its `check()` asserts the length, the ten
  corners, the closed and smooth seam, corners wide enough for the wall on their inside and no two parts of the
  circuit closer than 8 m. `Tools/GenerateAssets.py` (standard library only) makes from it the meshes (glTF with
  positions, normals and texture coordinates in meters; the track and the walls in world space, one primitive per
  material and 200 m of the lap, so that each is culled on its own), the textures, and the sky (a Radiance file with
  run-length encoded scanlines). The track's and the walls' colliders are meshes of their own whose vertices all point
  up and have no texture coordinates, so that the importer keeps them welded: Jolt finds the edges between a mesh's
  triangles by their shared vertex indices, and a seam of duplicated vertices (a material's or a chunk's border) would be
  an edge a wheel bumps on. The track's collider has one primitive per surface the physics tells apart (asphalt, kerb,
  gravel, grass), each welded, and the physics builds the shape over the mesh's distinct positions (§8, "Physics
  materials"), so the edges where two surfaces meet are shared too; the track's `MeshCollider` gives each submesh its
  physics material (`Surfaces/<name>.sphysmat`: friction, rolling resistance and the `Surface` name scripts compare;
  the gravel's friction is the larger and wins by `Maximum`, so whatever slides into a trap stops sooner than on the
  asphalt), and the ground's collider has the grass. The timing lines are painted into a texture of the asphalt
  (`TimingLines`) that the road around each line maps as the asphalt does, rather than cut into the road as thin
  geometry, which broke into dashes at a distance. The meadow beyond the barriers has a texture of fine detail only,
  which averages out at a distance where a larger pattern would repeat as a grid seen from the air. The same seed gives the same bytes (values are
  rounded before they are packed, against maths libraries that differ in their last digit); `GenerateAssets.py
  --check` generates twice and compares the runs with each other and with `Assets` (a PNG of equal pixels but other
  compression passes, naming the zlib). There is no fog: the ground lies in rings whose materials lose a share of their
  color to the horizon's, more with every ring outwards, and the ring of hills that makes the skyline, whose foot is
  meadow in the last ring's haze, emits a little of it too; that stands for the air between, so that the ground runs
  into the hills without an edge. `Tools/Author.py` builds the project
  through `strada mcp --headless` in parts, each of which owns root entities of the main scene and the files it may
  write (`circuit`: the generated assets, the materials and the `Circuit` entity with the track, the walls, the ground,
  the hills, the start gantry, the sector gates and the footbridge, every surface a static body with a triangle-mesh
  `MeshCollider`; `lighting`: the sun, the sky and the renderer settings, shadows over 220 m in four cascades;
  `camera`: the main camera, near plane 0.1 m and far plane 5 km, and its tour; `props`: the props' meshes, textures and
  materials and the `Props` entity; `bike`: the motorcycle's and the rider's meshes, livery and materials and the `Bike`
  entity on the grid; `input`: the input actions and the project file that names them; `sound`: the generated sounds,
  the mixer and the project file that names it, the `Footbridge Echo` and the `Audio Director` with its graph; `ghost`:
  the `Ghost` entity with its engine's sound, their graphs and the curves the sound reads; `trackside`: the `Trackside`
  entity with its four cameras and the camera director's graph; `tests`: the test scene and its graphs);
  `--part <name>` rebuilds one part in place, then the `tests` part (the test scene is the main scene with the checks,
  which name its entities), and fails, naming them, when files neither owns changed. A part made again keeps what it
  does not change: its roots are made where they stood among the roots, and every entity it makes takes the ID it had
  (found by its names from the root; `entity.create` takes an `id`), as do the test scene's own entities, so a part
  run again with nothing changed writes the same files. **The props** are 10,000 static entities placed by seed
  (`Tools/Props.py`): tyre stacks lining the walls round the corners' outsides and cones at their insides, braking
  markers, catch fences of posts and chain-link panels (continuous from one structure to the next: posts at most 3 m
  apart along the fence's line, panels halved where the line bends sharply), advertising boards with invented names, grandstands, marshals'
  posts, floodlights, bushes and trees, on 18 meshes `GenerateAssets.py` makes (`Meshes/Props`), each one primitive,
  and two materials: `Props`, whose texture is an atlas of flat colors, the markers' figures and the boards' names, and
  `Fence`, a chain-link cut out by an alpha mask and drawn from both sides. So the renderer draws them as instances, a
  batch per mesh and pass (§6.2, "Instancing"), and the whole frame stays within 400 draws where without instancing each prop would be a draw in
  every pass that sees it, thousands in a frame (`MotoSceneTests.cpp` holds it on the GPU tests' device). Every prop is an entity
  of a transform and a mesh in a group of its kind under `Props`, so the editor shows, selects and moves each like any
  other; the part makes each group with all its props in one `entity.create` request (its `children`), which keeps
  their IDs and makes the 10,000 in seconds where a request each would take minutes. `Props.py`'s `check()` asserts
  that no prop stands on any part of the circuit (only the stacks and cones stand at a wall's foot, in the last strip
  of its skirt), on a structure's legs, piers or stairs, in the camera tour's way or in another prop's, and that a
  fence panel covers every point of the fences' lines between structures. Graphs keep their asset handles
  when they are written again (emptied with `graph.set`, then written batch by batch). The sector gates are timing
  lines whose entities stand on the center line with their forward along the travel: gates 1 and 2 carry an arch, gate
  3 is the start and finish line under the gantry. **The bike and its rider** (`Tools/Bike.py`) are a stylized
  superbike generated to the published proportions (a wheelbase of 1,485 mm, a rake of 24°, a trail of 98 mm, 120/70
  and 200/55 ZR17 tyres, so a fork offset of 32.5 mm) and a rigid rider on it, waiting on the pole slot 9 m before the
  line: five static meshes (`Meshes/Bike`), so that what turns on a motorcycle can turn, under the `Bike` entity, whose
  origin is on the ground midway between the contact patches with its front along -Z: `Body`; `Fork`, at the foot of
  the steering axis and turned 24° about X, so that turning it about its own Y steers; `Front Wheel` under it and
  `Rear Wheel`, each on its axle; and `Rider`, at his hips. They share one livery texture and four materials (paint,
  dark, metal, glass): 16 primitives, so 16 draws in each pass that sees all of it, for 10,560 triangles (a second bike of the same meshes and materials joins their batches). `Bike.py`'s `check()` measures the wheelbase, the
  rake and the trail from the meshes and entities made, and that both tyres touch the ground; `check_grid()` keeps the
  tour 2 m from it. MO3's ghost, the vehicle and the rider's lean build on these parts. **The captures**:
  `Author.py --captures <folder> --player <StradaRuntime>` builds nothing; it takes the player's own screenshots at 1920
  by 1080 pixels (`--resolution`) from a fixed list of cameras (`Tools/Captures.py`: the bike on the grid close up and
  in profile, Turn 1 and Turn 6 with the bike leaned at mid and far range, Sector Gate 1, the footbridge, an aerial, the
  rider's view, and two adjacent frames of the tour along the fences, with the ghost hidden; and the four trackside
  cameras, where the camera director starts the game and turns them to the ghost, set out so that the picture catches
  it 0.2 s before it passes closest, about a seventh of the frame's height, which the tool asserts), each from a copy of
  the project outside the repository whose main scene puts the camera and the bike there, with a per-user root of its
  own, and fails a view whose picture is missing or of another size after three runs. With `--sounds` it also records,
  for each trackside view, 6 s of the ghost's pass (closest halfway) as the camera's listener hears it, through the
  player's `--audio-capture` without a window, into `<view>.wav`. On the reference machine (Release, 1920 by 1080,
  `StradaRuntime --profile` over the 600 frames of the whole tour, `--no-vsync`, slice 2's MO2 sitting) the frame's
  median is 3.5 to 3.9 ms (99th percentile 5.7 to 9.2 ms), with a median of 149 draws (at most 256), 146 batches and
  5,291 instances a frame; `Renderer.Submit` 0.72 to 0.74 ms and `Renderer.Prepare` 0.43 to 0.46 ms; the GPU's ambient
  occlusion 1.29 to 1.32 ms, opaque 1.00 to 1.03 ms and shadows 0.41 ms a frame. At slice 2's gate, with the fences
  continuous (two counted sittings, four runs, the machine otherwise idle, IN's gauge out of band at 3.67 to 3.87 ms):
  a median of 3.42 to 3.46 ms (99th percentile 4.0 to 4.1 ms), 149 draws (at most 256), 146 batches and 5,339
  instances a frame, `Renderer.Submit` 0.71 to 0.72 ms and `Renderer.Prepare` 0.44 to 0.46 ms, ambient occlusion 1.29
  to 1.34 ms, opaque 1.01 to 1.03 ms and shadows 0.40 to 0.41 ms; in its two later sittings, with the gauge in band
  (2.65 to 2.83 ms), a median of 3.47 to 3.56 ms (99th percentile 6.3 to 6.4 ms), the same draws, batches and
  instances, `Renderer.Submit` 0.74 to 0.81 ms and `Renderer.Prepare` 0.42 to 0.44 ms, ambient occlusion 1.28 to 1.29 ms,
  opaque 0.99 to 1.00 ms and shadows 0.41 ms. The editor's scene view draws more of the same
  scene, since `DrawCalls` counts its entity-ID pass as well: at slice 2's gate (`viewport.statistics`) 392 draws of the
  400 with the camera at the start line beside the bike (388 batches, 190 of the draws in the shadow pass), 261 over the
  whole circuit from above and 212 across the main straight's fences. A bike of other materials would add up to 16
  draws to each pass that sees it and pass 400 there, so the ghost bike shares the bike's meshes and materials and joins
  their batches: with it standing at the line, the same view reads 398 draws (394 batches, the shadow pass's 190
  unchanged) where it read 392 without it, in one sitting of the editor (MO3): the camera stands close behind the grid
  bike, three of whose parts lie outside the view, and the ghost at the line brings their batches (302 draws with
  neither bike, 392 with the grid bike alone, 398 with the ghost alone or both), and `MotoSceneTests.cpp` renders that view with entity IDs on and holds it
  within 400; the Release editor reads the same (398 with everything, 392 without the ghost and the entities that came
  with its sound). **With the ghost and its sound** (MO3's sitting: Release, the tour's 600 frames as above, the sound
  to the null device, the project as it is and a copy without the ghost, the trackside cameras, the directors and the
  echo, run alternately, the 10,000-instance gauge in band at 1.22 to 1.36 ms; provisional until the slice's gate
  measures again): the frame's median is 3.54 to 3.59 ms (99th percentile 6.3 to 6.4 ms) against 3.56 to 3.64 ms
  (5.6 to 6.6 ms) without them, a median of 170 draws (at most 265), 167 batches and 5,341 instances against 149 (256),
  146 and 5,338.5: in 287 of the 600 frames the ghost, which the tour passes, is seen by one to four passes where the
  grid bike is not, so its 16 primitives are batches of their own there (16 draws a pass), and in the others it adds
  none; the graphs take 0.08 ms a frame against 0.02 (`Scene.UpdateScripts` 0.10 ms against 0.03), and
  the mix of the sixteen engine voices, the echo's send and its reverb 0.32 ms per 1/60 s of audio (`Audio.Mix` in a
  run without a window at 1/60 s a frame with `--audio-capture`, which mixes on the main thread once a frame) against
  0.12 ms without them: 0.19 ms per 10 ms of audio. **The camera tour** is the first sample tour made of a graph
  (`Scripts/Tour.sgraph`, on the main camera): a `Flow.Timeline` with two Vector3 curves, `Eye` and `LookAt`, flies the
  camera from the home view once round the lap and back to it in about 51 s; each update sets the camera's
  translation and the Euler angles that point it at `LookAt` (a formula: pitch, then yaw, no roll, so no keyed angle
  wraps), and when the timeline finishes the `Tours` read-out counts the tour and, while `Repeat` holds, it plays
  again. `PlayOnStart` starts it with the scene; another graph starts it by sending the camera `PlayTour`.
  `Tools/Tour.py` makes the keys (every 0.5 s) from a lead point that runs the lap at a pace the corners set, faster
  the higher the camera flies, which the camera chases at a height and a distance that change along the lap (over the
  gantry, low down the straights and under the footbridge, beside the sector gates' boards, higher over the corners).
  Its `check()` evaluates the curves as the engine does (Catmull-Rom on the keys' times, flat at the ends) and asserts
  that the camera keeps 2 m from every surface of the layout the whole way, which is a property of the data, not of
  the engine. **The sounds and the mix.** `Tools/GenerateAudio.py` (standard library only, seeded, `--check` makes
  everything twice in memory and compares the runs with each other and with `Assets`, writing nothing) synthesizes
  every sound as mono 16-bit PCM at 24 kHz (half the mixer's rate): the engine of a 90° V4 whose cylinders fire at 0,
  90, 290 and 380° of the 720° cycle (intervals of 90, 200, 90 and 340°: the twin pulse), as eight layers on load
  (`Audio/Engine/On1` to `On8`) and eight off load (`Off1` to `Off8`) at 2,000 rpm to 16,000 rpm, each 1.346 times the
  one before; the wind, a tyre squeal and a crowd bed, loops of 4, 2 and 6 s; and a shift blip, a one-shot of 0.42 s.
  Each engine layer holds a whole number of engine cycles (about 0.8 s; the tool states its exact speed), each pulse
  a raised cosine into five fixed exhaust resonances, on load with the burning gas's noise, off load weaker, less even,
  with pops and the gearbox's whine. Every loop is periodic by construction (its sources lie on a circle of its length
  and its filters run round it twice), and every run measures each file and fails on a broken promise: a peak of at
  most -1 dBFS, a seam whose step and bend are no larger than the largest inside the loop and whose level lies among
  the levels inside it, a one-shot that starts and ends in silence, an engine layer that repeats each engine cycle and
  not each turn of the crank (the twin pulse), 3 MB in all (the twenty files weigh 1.2 MB). `Tools/Sound.py` is the
  mixer (`Audio/Moto.smixer`, named by `Moto.sproj`): `Master`, with a limiter at -1 dBFS, over `Engine`, `Tyres`,
  `Ambience`, `Interface` and `Reverb`, which carries a reverb's tail alone (dry at -80 dB) for the footbridge's zone;
  every bus but `Interface` runs a low-pass standing open at 20 kHz, which the `Paused` snapshot closes to 650 Hz while
  it lowers those buses by 10 dB. **The racing line** (`Tools/RacingLine.py`, data of the tools as the layout is) takes
  each corner from the road's outside edge at its turn-in to its inside edge at its apex and out again (4.6 m from the
  center line; the turn-in and the exit where the widest arc the road allows would leave the edge; apex to apex where
  the next corner turns the other way too soon), averages the line's points along the lap with a Gaussian of 6 m so
  that its curvature is continuous, then sets the speed from it: the cornering limit at 1.2 g, a forward pass at what
  150 kW at the rear wheel gives against the drag (CdA 0.3 m²) and the rolling resistance, at most 9 m/s² (the front
  lifting), and a backward pass at 11.5 m/s², both sharing the tyres' grip with cornering, round the lap three times;
  the lean is atan(v²κ/g); the gear follows a stated six-speed gearbox (up at 15,500 rpm, down under 10,500 while
  braking), and the engine speed and the load (the share of the power given) follow. Its `check()` asserts that the
  line stays on the asphalt (within 5.2 m of the center line), leans at most 52°, has no step in its speed (also in the
  speed its keys give) and that its keys reproduce it (1 cm, 0.1° of heading, 0.2° of lean, 15 rpm). **The ghost
  bike** (`Tools/Ghost.py`, the `ghost` part) is a second bike and rider of the grid bike's meshes and materials, so
  that the renderer draws both in the same batches, at the start and finish line; it has no rigid body and no
  collider, and its graph (`Scripts/Ghost.sgraph`) plays a looping timeline of the line's keys over one lap: `Position`,
  `Heading` (unwrapped degrees about +Y), `Lean`, `EngineSpeed` (two keys at one time at each shift) and `Load`, each
  key carrying the line's own slopes, as few keys as keep each within its tolerance. Every update places the ghost,
  turns it by `Heading` after `Lean` (`Quaternion.AngleAxis` twice, multiplied), counts a lap when the timeline's time
  comes round past the line, keeps the read-outs `LapTime`, `EngineSpeed` and `Load`, and sets its engine sound's
  `EngineSpeed` and `Load` (the child that runs `EngineSound.sgraph`, found when the scene starts); `StartAt` says where
  on the lap it sets out. **The engine's sound** (`Tools/EngineSound.py`, the `ghost` part) is the `Engine Sound`
  entity under the ghost, with sixteen children, each an `AudioSource` looping one layer on the `Engine` bus, spatial
  with doppler (the graph's `DopplerLevel`, given to every voice when the scene starts), playing from the start at
  volume 0. Its graph (`Scripts/EngineSound.sgraph`) takes its inputs as public variables, `EngineSpeed` and `Load`,
  which whoever runs the engine sets (the ghost now, a ridden bike later), and every update sets each voice's volume
  and pitch: the volume is the layer's fade by where the engine speed lies from it in layers (`log(speed / layer's
  speed) / log(1.346)`, read through the curve asset `Curves/EngineLayerFade.scurve`, cos(πx/2) from -1 to 1, held at 0
  below the lowest layer and above the highest) times the load's gain (`EngineOnLoad.scurve`, sin(πL/2), for the
  on-load voices; `EngineOffLoad.scurve`, cos(πL/2), for the off-load ones) times `Volume`, and 0 while `EngineSpeed`
  is 0 (an engine that does not turn, or one not set yet: the ghost and its child update in no stated order). Both
  crossfades are of equal power, so the voices' volumes squared sum to `Volume` squared at any speed and load (the curves are cubics through
  the functions' keys every 0.125 with their slopes: within 0.00023 of 1 from 1,000 to 17,000 rpm, which the tool's
  `check()` asserts). The pitch plays the layer at the engine's speed over the speed it was recorded at (the generator's
  exact figure, kept in the graph's `LayerSpeeds`), held within a layer's step either way, where its fade ends. Sixteen
  voices mix whether they sound or not (§9.3: below the engine's limit of 64 every voice mixes). **The trackside cameras**
  (`Tools/Trackside.py`, the `trackside` part) are four children of the `Trackside` entity, in the order the cuts go
  round: at the start line (80 m before it, across the road from the grandstands), on the insides of Turns 1 and 6 at
  their apexes, and 35 m before the footbridge on the side the racing line passes it, each 5 m up on a mast 1.5 m
  behind the catch fence, with a field of view of 35°, moved along the lap (a meter at a time, either way) until no
  prop stands within 1.5 m of it, nor a tall one within 6 m. Each has a camera that is not primary and a listener that
  is off. `Trackside` runs the camera director (`Scripts/CameraDirector.sgraph`): `TourCamera` (the main camera) and
  `Subject` (the ghost) are its public variables; when the scene starts it cuts to `StartCamera` (0, the tour, by
  default); the `NextCamera` action sends it the custom event `NextCamera`, which cuts to the next trackside camera
  (round to the first), and `ResetCamera` sends it `ResetCamera`, which cuts back to the tour, which flew on meanwhile.
  A cut (its function `Cut`) makes one camera primary and its listener the one that hears, and every other camera and
  listener off: the scene renders and hears from that camera (a cut gets no doppler in its update, §9.8, since the
  listener is another entity). Every update turns each trackside camera towards the subject, 0.9 m above its origin
  (the director stands at the origin unturned, so its children's angles are the world's); `Active` reads the camera
  that shows. **The echo and the pause** (`Tools/Sound.py`, the `sound` part): the `Footbridge Echo` is a
  `ReverbZone`, a box under the footbridge's deck across the road and its verges (half extents 12.2, 3 and 3 m, up
  to the deck), turned with the road, sending 0.8 of what sounds inside it to the `Reverb` bus and fading out over
  8 m round it, so the ghost's pass under the deck (about 190 km/h) sends for about 0.4 s and leaves its
  tail ringing; the `Audio Director` runs `Scripts/AudioDirector.sgraph`, to which the `Pause` action sends
  `TogglePause`: it blends the mix to the `Paused` snapshot over `Seconds` (0.5), or back to the mixer's own values,
  and keeps the read-out `Paused`; nothing else pauses yet. **The input actions** (`Tools/Controls.py`, the `input`
  part; `Input/Moto.sinput`, named by `Moto.sproj`): `Throttle` and `Brake` (axes: W and S, the right and the left
  trigger), `Lean` (A and D, the left stick's X), `ShiftUp` and `ShiftDown` (E and Q, the bumpers), `Pause` (Escape,
  Start), `NextCamera` (C, Y) and `ResetCamera` (R, Back), each on the keyboard and any gamepad; the directors use the
  last three, and the riding actions wait for the ridden bike. **The test scene** (`Scenes/Tests.sscene`, the main scene
  without its props, which stand clear of all it checks, with a crate over the road, two blocks sent
  sliding with the scene, the `Waiting Tour`, the ghost set out 30 s into its lap and the `Tests` entity) holds 24 checks, all graphs (`Tests.sgraph`, a latent function per check called in order from a Sequence,
  reporting through the `Checks` library): rays down at points of the layout hit the road, a kerb, a wall's top, the
  ground and the footbridge's deck at their heights, facing up, and name the physics material of the asphalt (also
  where a timing line is painted), a kerb, a gravel trap, a verge and the ground; the crate comes to rest on the road;
  of the two blocks sent off at 8 m/s, rotation locked, the one on the asphalt slides farther than the one in the gravel,
  each within a tenth of v²/(2gμ); and the camera is
  at the home view when the scene starts, where the path puts it under the footbridge, home again when the tour ends,
  and off on the next one after it; the `Waiting Tour`, a second entity running `Tour.sgraph` with `PlayOnStart` and
  `Repeat` off, stays where the scene put it until the `Tests` graph sends it `PlayTour`, then flies, and stays at the
  home view after its one tour; the ghost leans into Turn 6 within 1° of the line, is within 0.1 m of where the line
  puts it at three moments of its lap, facing along it within 2° (each read with its own `LapTime` of the same update,
  the line's point carried on by its velocity over the frame between), and crosses the start and finish line onto its next lap, on the line again;
  the engine's sixteen voices all play on the `Engine` bus at the ghost's engine speed, their volumes squared sum to 1
  within 0.02 and each that sounds plays within its layer's band; sent the events its actions send, the camera
  director cuts to the first trackside camera (it alone shows and hears, turned to the ghost within 2°), to the second,
  and back to the tour's camera; and sent `TogglePause` twice, the audio director blends the mix to the `Paused`
  snapshot (`Audio.CurrentSnapshot` reads it) and back.
  They wait in seconds of the game, with tolerances for a frame of the slowest test
  run, so they pass at 1/60 and 1/30 s a frame; `StradaRuntimeTests` runs them twice at 1/60 s (the same summary each
  time) and once at 1/30 s, and plays the main scene, refusing any line from a visual script. `MotoSoundTests.cpp`
  (StradaRuntimeTests) hears the ghost pass the footbridge camera through the player's `--audio-capture`, as the
  differences between captures of the same 320 frames at 1/60 s that differ in one setting (the ghost's engine speed
  changes along the straight, so a falling pitch alone would prove nothing): the ghost set out 25 s into its lap and the
  game started on that camera, a capture with the echo, one without it (`SendLevel` 0) and one without it and without
  doppler (`DopplerLevel` 0 on the engine's voices), each a copy of the project's main scene. The engine's cycle (120 /
  rpm s, read by autocorrelation in windows of 0.1 s with no gear change in them, refined between two lags) is the
  engine speed's without doppler, and with it shorter before the pass and longer after it by the factor c / (c - v)
  that the ghost's motion towards the camera predicts, within 1 % (it reads within 0.3 % against shifts of 9 to 13 %):
  the prediction comes from the copied project itself (the keys of the ghost's timeline, the camera's place, the
  mixer's speed of sound). With the echo, the capture is the same samples as without it until the ghost comes within
  the zone's reach, carries a tail of at least a tenth of the mix's level over the 0.4 s after it has left (about a
  third), and a second later under a tenth of that. `PlayModeTests` (StradaEditorTests) plays the
  main scene in the editor's play mode, where the camera leaves the home view, the actions `NextCamera`, `Pause` and
  `ResetCamera` injected through `input.set` cut to the start line's camera, blend the mix to the `Paused` snapshot and
  back and cut back to the tour, and stopping brings the camera back. Every build ends by dropping spheres, in
  play mode's copy of the scene, on the road, a kerb, a gravel trap, a verge, the ground, a wall and the footbridge,
  which must come to rest on those surfaces, by checking every graph (no diagnostic, every node in one box, every box
  arranged), by a test run that must pass every check, by playing the main scene for 10 s in play mode, where the
  camera must be on the tour's path and the ghost on its line, and by refusing any warning in the log.
- The Showcase's **Graph Garden**: chimes played by six visual scripts (`Scripts/GraphGarden`; `Tools/GraphGarden.py`
  authors them and `Tools/Author.py --exhibit GraphGarden` rebuilds the exhibit in place, with a hash guard that names
  any file outside the exhibit that changed), whose score is a Timeline; seven of the Showcase's 68 checks look at it.
- **Sample graphs read well**: `SampleGraphsTests` (StradaEditorTests) requires of every graph of Prism Break, Moto,
  the Graph Garden and the two starter templates that every node is placed in exactly one titled comment box, none overlapping or closer than 16 units,
  that no link joins two boxes, that every graph has a description and every public variable a tooltip, and that
  arranging each box's nodes moves none; its message names the authoring tool to run again after a change to arranging.
  `SampleProjectsTests` (StradaTests) checks that AGENTS.md and README.md name every folder of `Projects/` and that the
  scripting guide's table names every graph of Prism Break.
- **Performance bounds** (Release, `STRADA_PERFORMANCE_TESTS=1`; each asserted at 3 times its measured figure, the
  deterministic counters always): the untraced interpreter runs about 70 M steps a second, an attached idle debugger at
  95.9 to 99.5 % of that, Break and Record runs at 72 to 79 % and Values runs at 43 %; a 2,000-node instance's snapshot
  takes 0.02 ms; a `graph.step` round trip on 2,000 nodes 0.22 ms; a default 600-frame trace of a 200-node graph is 36,780
  bytes of compact JSON (bounded at 64 KB). Formulas run 3.7 times faster than the graphs of nodes they replace. The
  new-link frame of a 2,000-node graph takes 63.2 ms (5.97 ms at 200 nodes; 58.9 ms and 6.49 ms while playing), against a
  target of 60 ms that it misses: the analysis takes 48.9 to 51.6 ms of it, of which the managed analysis is 41 to 45 ms
  (reading the document, writing the analysis and the collections their allocations cause), the native read 4 ms. A
  watched 2,000-node graph's whole flow frame takes 1.75 ms, against a target of 1 ms: the window's own part (the flow
  model and the overlays) is 0.35 to 0.38 ms, the rest the runtime's traced runs and the trace's take and read. 200 value
  pills with their request take 1.02 to 1.12 ms against 0.4 ms, mostly the managed `Values` JSON round trip; the editor
  reads values 10 times a second, about 0.15 ms per frame. Measured again with the profiler's zones at First Ride's
  first slice gate (three runs, the machine otherwise idle): the new-link frame 72.4 to 73.5 ms, of which
  `GraphEditor.Analysis` 59.2 to 59.6 ms (the managed runtime 51.6 to 52.4 ms); watching a 2,000-node graph adds 2.0 to
  2.3 ms to its frame, of which `GraphDebug.Frame` 1.44 to 1.53 ms; 200 value pills add 1.2 to 1.4 ms, of which
  `GraphDebug.ReadValues` 0.82 to 0.98 ms. The remainders are runtime work (a cheaper Record mode, a
  binary value block, reading only the sources that changed, a document reader without a JSON document per member).
- CI: Windows (MSVC), Ubuntu 24.04 (GCC + Clang), macOS (Apple Clang, arm64); format check; tests on every push.
  Windows MSVC Release is two jobs, which divide the test suites between them through the CMake cache variable
  `STRADA_TEST_SUITES` (`StradaTests` and `ScriptCoreTests`; `StradaEditorTests`, `StradaRuntimeTests` and
  `StradaToolTests`), so that each stays far from the job's time limit: each builds every target that is not a test
  suite, and together they build every target and run every ctest entry once.
