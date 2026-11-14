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
through includes of `Scene.h` in headers. Two Core translation units reach to the right, never their headers:
`Application.cpp` starts the subsystems above Core, and `EngineLayout.cpp` reads its marker with Serialization's file
header functions, as every Strada file is read.

| Module | Responsibility | Key types |
|--------|----------------|-----------|
| Core | Base macros, logging, asserts, UUID, time, buffers, events, input, window, application loop (and its main-thread queue), layers, file system, OS queries (the user's Documents folder among them), the engine's layout (where its content lies), images, embedded resources, command line, the profiler's recorder (§4.4) | `Application`, `Layer`, `Window`, `Input`, `Log`, `UUID`, `Timestep`, `Buffer`, `FileSystem`, `Platform`, `EngineLayout`, `Image`, `Result<T>`, `Profiler` |
| Math | Transform compose/decompose, projections, sRGB transfer functions, bounding boxes, view frustums | `Math::ComposeTransform`, `Math::DecomposeTransform`, `Math::PerspectiveReversedZ`, `AABB`, `Frustum` |
| Serialization | JSON conversions for glm, UUIDs, enums and reflected structs; deserialization warnings; profiler captures (§7.3) | `JsonTraits<T>`, `EnumTraits<T>`, `StructTraits<T>`, `DeserializationContext`, `ProfilerCaptureSerializer` |
| Platform | Child processes (reproc++): spawning, output capture, exit codes, termination | `Process` |
| RHI | Vulkan instance/device creation, NVRHI device, swapchains (one per OS window), frame pacing, embedded shader library, texture readback | `GraphicsDevice`, `Swapchain`, `ShaderLibrary`, `ReadbackTexture` |
| Asset | Asset handles and metadata, the persistent registry, the asset manager, CPU asset types, model importers, built-in primitives, the visual script format and its layout | `AssetHandle`, `AssetManager`, `AssetRegistry`, `MeshSource`, `MaterialAsset`, `TextureAsset`, `EnvironmentAsset`, `PrefabAsset`, `AudioClipAsset`, `FontAsset`, `VisualScriptAsset`, `VisualScriptSerializer`, `MeshImporter` |
| Renderer | GPU resources created from assets (meshes, textures, materials, environments, fonts), samplers and fallback textures, the scene renderer and its passes, environment processing, shadow math, text layout, sprites, picking, presentation blits | `Renderer`, `SceneRenderer`, `EnvironmentProcessor`, `QuadRenderer`, `FontAtlas`, `TextureBlitter` |
| ImGui | ImGui context/layer, NVRHI renderer backend with multi-viewport support, UI helpers | `ImGuiLayer`, `ImGuiRenderer` |
| Physics | Jolt init/shutdown, per-scene physics world, layers, contact events, queries | `PhysicsSystem`, `PhysicsScene` |
| Audio | miniaudio engine, per-scene sound sources and listener, waveforms of clips | `AudioEngine`, `AudioScene`, `ComputeAudioWaveform` |
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
- miniaudio runs its own audio thread; only the miniaudio API is touched from the main thread. The audio streaming
  thread reads and decodes streamed sounds into their ring buffers, which the audio thread copies from without waiting.
- Background work (script builds, exports, the automation socket) never touches engine state; results are
  marshalled with `Application::SubmitToMainThread(std::function<void()>)`. The editor notices edited scripts by
  checking their modification times on the main thread once a second, and rescans the assets on the main thread when
  it gets the focus back.
- The profiler (§4.4) is the one engine service every thread writes to: any thread records zones into a buffer of its
  own without allocating or locking, and only the main thread collects them.

### 4.3 Error handling

- No exceptions cross module boundaries. Third-party code that throws (nlohmann::json) is wrapped at the boundary.
- Fallible operations return `Result<T>` (value or error message) or `bool` plus a logged error.
- `ST_CORE_ASSERT`/`ST_ASSERT` are for programmer errors only. User data (scene files, assets, scripts, automation
  requests) must never crash the engine: validate, log, and report the error.
- Crashes leave a report: the entry point installs `CrashHandler`, which writes
  `<user data>/Crashes/<executable>-<process ID>.txt` on a fatal exception or signal, `abort()` (failed asserts) or an
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
  - `Scene.Update` around `Scene::OnUpdateRuntime`'s phases, in their order: `Scene.UpdateUI`, `Scene.UpdateScripts`,
    `Scene.UpdatePhysics`, `Scene.ContactEvents`, `Scene.UpdateAudio`, `Scene.UpdateParticles`, `Scene.Destroy`; in
    each fixed step `Scene.FixedUpdateScripts`, `Scene.SyncToPhysics`, `Physics.Step` and `Scene.SyncFromPhysics`;
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
    `AudioEngine::ReadFrames` (manual output, on the calling thread);
    `Audio.Stream` around each pass of the streaming thread (`Audio Streaming`), never its waits;
  - `Renderer.RenderScene`, with `Renderer.Submit` (the scene's draws, lights and UI) and `Renderer.EndScene`, which
    holds `Renderer.Cull`, `Renderer.Sort`, `Renderer.Prepare` (resources, uploads, shadow views, frame bindings) and
    `Renderer.Record` (recording and submitting the frame); `Renderer.UploadTexture`, `Renderer.UploadMesh` and
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
  spares up on the main thread, so no thread ever allocates its own ring. A slot's state word holds the recording's
  number and the slot's state (free, being claimed, owned or ending). On its first zone of a recording, a thread takes a
  free slot of that recording with a compare-exchange. It remembers the slot in a constant-initialized, trivially
  destructible `thread_local` holding the slot index, the recording's number, its depth and its name. A `thread_local`
  with a destructor is avoided because registering its destructor may allocate on the audio thread. Thread names are
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
- **ThreadSanitizer.** `.github/workflows/tsan.yml` runs nightly, on manual dispatch and on a push to a `tsan/**`
  branch, on Linux with Clang in Debug (the `linux-clang-tsan` preset, which sets `STRADA_ENABLE_THREAD_SANITIZER`). It
  runs the suites' threading cases, one step per area: the profiler, the main-thread queue, audio with manual output,
  the editor's worker-thread loads (thumbnails, the example copy; the engine's own asset loads run on the main thread),
  Jolt's workers, the automation server's network thread, and, in a step of its own, a profiled scene that hosts the
  .NET runtime. Every target the build compiles is instrumented, third-party ones included: synchronization in code
  the sanitizer cannot see makes it report races that are none, and Jolt compiles other inline code under it
  (`JPH_TSAN_ENABLED`). Only prebuilt libraries are suppressed (`.github/tsan-suppressions.txt`), never Strada's code.
  It never gates a merge.

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
structured buffer, material sampler, per-draw push constants (model + normal matrix, 128 bytes); set 1 = material.
Structures shared with HLSL live in `Strada/Shaders/Include/RendererInterop.h`.

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

## 7. Scene and ECS

### 7.1 Scene

`Scene` owns an `entt::registry`, a `UUID → entt::entity` map, scene settings (`SceneSettings`: `Physics` with the
gravity and `Renderer`, field tables like components) and the runtime subsystems while playing (`PhysicsScene`,
`AudioScene`, script instances).

Lifecycle: `OnRuntimeStart/Stop` (physics + scripts + audio), `OnSimulationStart/Stop` (physics only),
`OnUpdateRuntime(ts)`, `OnUpdateSimulation(ts, camera)`, `OnUpdateEditor(ts, camera)`, `OnViewportResize(w, h)`.
Play mode runs on a deep copy (`Scene::Copy`); stopping restores the editor scene untouched.

Runtime update order per frame: UI (the mouse over buttons, clicks and `OnClick`) → scripts `OnUpdate` → physics
fixed steps (accumulator, default 60 Hz; scripts' `OnFixedUpdate` before each step; characters are then drawn between
their last two steps, §8) → contact/trigger events dispatched to scripts → transform sync → audio update → deferred
entity destruction → render submission. Entity destruction requested during iteration is **deferred** to the end of
the frame; creation is immediate. Editor edits of the running scene happen between frames, so `EditorContext`
completes their destruction at once (`Scene::FlushPendingDestruction`): later edits and undo, also while paused, find
the entities gone.

### 7.2 Components (`Strada/Scene/Components.h`)

Plain data structs, copyable, with PascalCase public fields. Light intensities are artist-friendly multipliers
(the renderer's exposure defaults suit them), not photometric units. Each component's serialized name, description and
field list are declared once in `ComponentTraits.h`; generic code derives JSON serialization (strict, typed,
transactional partial updates with readable errors), schemas, copies, the inspector and the `ComponentRegistry` entry
from that table. Fields carry optional hints: an inclusive range (enforced whenever JSON is read, so files, automation
and the editor all reject out-of-range values), a presentation (color, angle, multi-line text), the referenced asset
type, a one-sentence description, a visibility condition (the field only matters while an enum or bool field of the
same struct has one of some values, like a camera's orthographic size) and a switch (`EnabledBy`: another bool field of
the table, not switched itself, that turns the field on; drawn by the inspector as a check box in front of the field);
both are checked at compile time. Values that skip the JSON checks (scripts) can be brought into range with
`ClampToFieldRanges`. Field tables of other structs (renderer settings, materials) use the same hints. Each component
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
| `RigidBody` | `RigidBodyComponent` | `Type` (Static/Dynamic/Kinematic), `Mass`, `LinearDamping`, `AngularDamping`, `GravityFactor`, `Layer`, `LockTranslation` bvec3, `LockRotation` bvec3, `ContinuousCollision`, `AllowSleep`, `InitialLinearVelocity`, `InitialAngularVelocity` |
| `BoxCollider` | `BoxColliderComponent` | `HalfExtents`, `Offset`, `IsTrigger`, `Friction`, `Restitution` |
| `SphereCollider` | `SphereColliderComponent` | `Radius`, `Offset`, `IsTrigger`, `Friction`, `Restitution` |
| `CapsuleCollider` | `CapsuleColliderComponent` | `Radius`, `HalfHeight`, `Offset`, `IsTrigger`, `Friction`, `Restitution` |
| `MeshCollider` | `MeshColliderComponent` | `Mesh` asset (0 = use `Mesh` component), `Convex`, `IsTrigger`, `Friction`, `Restitution` |
| `Joint` | `JointComponent` | `Type` (Fixed/Hinge/Ball/Distance/Slider), `ConnectedEntity` UUID (0 = the world), `Anchor` (local), `ConnectedAnchor` (the connected entity's local space, or world), `Axis` (local), `LimitsEnabled`, `MinLimit`/`MaxLimit` (hinges: deg; sliders: m), `MinDistance`/`MaxDistance` (-1 = the starting distance), `MotorEnabled`, `MotorSpeed` (deg/s or m/s), `MotorMaxForce` |
| `CharacterController` | `CharacterControllerComponent` | `Radius`, `Height` (the whole capsule), `Offset` (the capsule's center), `StepHeight`, `MaxSlopeAngle` (deg, 1..89), `Mass` (kg), `PushForce` (N), `GravityFactor` (0..1000), `Layer` |
| `AudioSource` | `AudioSourceComponent` | `Clip` asset, `Stream` (read the clip's file while playing), `Volume`, `Pitch`, `Loop`, `PlayOnStart`, `Spatial`, `MinDistance`, `MaxDistance` |
| `AudioListener` | `AudioListenerComponent` | `Active` |
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

Every file starts with a header: `"Strada": { "Version": <int>, "Type": "Scene" | "Prefab" | "Material" | "Project" | "AssetRegistry" | "VisualScript" }`.

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

Project (`<Name>.sproj`, `ProjectSettings` with field tables, validated on every read): `Name`, `AssetDirectory`
(relative, inside the project), `ScriptModule` (the game assembly, relative), `StartScene` (scene asset reference),
`Window` (`Title`, `Width`, `Height`, `Fullscreen`, `VSync`, `Resizable`, `Icon` texture; for exported games, whose
player sets the icon on its window) and `Physics`
(`FixedTimestep`, `Layers`: 1 to 16 unique names indexed by `RigidBody.Layer`, `IgnoredCollisions`: layer index pairs
that do not collide). Opening a project opens its asset directory first so asset references resolve; settings from
newer versions are skipped with warnings. New projects (`Project::Create`) get the project file, `Assets/` and the
start scene `Assets/Scenes/Main.sscene` in an empty or new directory.

Asset registry (`Assets/AssetRegistry.sreg`): `"Assets": [ { "Handle", "Type", "Path" } ]` sorted by path, paths
relative to `Assets/` with forward slashes (case-sensitive, portable characters only). The asset directory is scanned
when a project opens: new files get fresh handles, missing files keep theirs. Hidden files and folders (names starting
with `.`) are ignored, and symbolic links are skipped with a warning (they could make any file of the machine part
of the project and its exported games). Invalid entries are skipped with warnings; an unreadable registry refuses to open (continuing
would break references).

Material (`.smat`): `"Material": { <MaterialAsset fields> }`; missing fields keep their defaults.

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

- metadata events (`"ph": "M"`): `process_name` of process 1 (`"Strada"`) and, when there are GPU passes, of process 2
  (`"GPU"`); then a `thread_name` (`"args": { "name": ... }`) for every thread of process 1 (`"tid"`: its index + 1,
  so the main thread is `"tid": 1`, named in order from 1) and for every GPU view of process 2 (`"tid"`: the view's
  index + 1, named in order from 1). Other metadata is ignored;
- one complete event (`"ph": "X"`) per frame on process 1, thread 1, named `Frame` with `"cat": "Frame"`, with
  `"args": { "Frame": <index>, "DroppedZones": <n> }`. A frame's events follow it;
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
frame, so it never builds more than one frame past the bound), holding more than the reader accepts, or with a frame
whose zones are not grouped by thread in the thread table's order.

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

## 8. Physics (Jolt)

`PhysicsSystem` (global) initializes Jolt: the default allocator, the type factory and a job system with a worker
thread per core but one. `PhysicsScene` is one physics world, independent of the ECS (bodies are described by
`BodyDesc` and keyed by entity UUID), so it is tested on its own. Object layers encode the project layer and whether
the body moves (static bodies never meet each other); broad-phase layers separate static from moving bodies; the
project's ignored layer pairs never collide. An entity's colliders form one compound shape scaled by the entity's
world scale (mirroring is ignored); triangle-mesh colliders on dynamic bodies use the mesh's convex hull, kinematic ones
keep the triangles (bodies that move take the `RigidBody`'s mass, which Jolt cannot compute for triangles). Friction
(geometric mean), restitution (maximum) and triggers are applied per contact by the contact listener, so one entity
can mix solid and trigger colliders. The listener, called on Jolt's worker threads, turns sub-shape contacts into one
enter and one exit event per entity pair and contact kind; removing a body ends its contacts with exit events. Jolt
drops the contacts of bodies that fall asleep and finds them again when they wake up; such contacts persist (no exit
while asleep, no second enter on waking) and end only when the awake bodies no longer touch. Jolt is
built at the SSE4.2 baseline (its instruction-set flags reach the engine sources that include it) with its asserts,
routed to the engine log, in Debug builds only.

The scene drives it while running. `OnRuntimeStart(SceneRuntimeSettings)` (from `MakeSceneRuntimeSettings(project
settings)`) creates bodies for entities with colliders (static without a `RigidBody`; a `RigidBody` without colliders
is not simulated). EnTT signals rebuild a body when its physics components, or the mesh of its mesh collider, change;
the rebuilt body keeps its velocities. Physics runs in fixed steps (`Physics.FixedTimestep` of the project, at most 8
per frame): before each step kinematic bodies move towards their transforms (`MoveKinematic`) and static or dynamic
bodies whose transforms changed (scripts, the editor, a moving parent) are teleported; after it, awake dynamic bodies
write their world transforms back (parents first, converted to parent-local space, keeping their scale). Events
delivered to scripts: `OnCollisionEnter/Exit(Entity other)`, `OnTriggerEnter/Exit(Entity other)`. Queries: raycast
(closest solid hit; triggers and colliders containing the origin are ignored), with a layer mask.

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
ALSA, JACK) are loaded at runtime and are not build requirements.

`AudioClipAsset`s hold the encoded file (WAV, FLAC, MP3, Ogg Vorbis; the format is detected from the data).
`ComputeAudioWaveform` decodes a clip with a decoder of its own (the `AudioDecoder` of its format, as sounds do), so it
runs on any thread and can be cancelled, into the peaks of equally long parts (blocks merge as the clip decodes, so
memory stays bounded for any length); the editor's content browser draws them.
`AudioScene` holds the sounds of one running scene, independent of the ECS (keyed by entity UUID); a scene's sounds play
through one sound group that pauses them together. Each sound has its own decoder (`AudioDecoder`): miniaudio's decoder
of the detected format for WAV, FLAC and MP3 and libvorbis for Ogg Vorbis, so no other decoder ever reads the data (left
to itself, miniaudio would search a whole file for an MP3 frame). Loaded sounds decode the clip's data in memory while
they play. Streamed sounds (`AudioSourceDesc::StreamFile`) open the clip's file when they are created, on the main
thread, which reads only its format and headers (at most `MaxAudioStreamHeaderBytes`: damaged files whose audio would
start later are refused), and play from a ring buffer that the audio streaming thread (`AudioStreamer`, owned by
`AudioEngine`) keeps decoded about a second ahead (`AudioStreamBufferSeconds`). The thread that mixes them only copies
decoded samples and never waits: a read that stalls silences that one sound for a moment, and it continues where it was.
The streaming thread decodes on past the end of the file from its start and marks where the file ended, where the mixer
decides whether the sound loops, so loops are seamless and a sound played again after it ended starts at once; stopping
a sound empties its buffer and decodes the start again. `AudioEngineSettings::StreamFileOpener` opens the files (the
file system unless set; tests count and hold up reads through it), and `AudioEngine::WaitForStreams` lets manual output
hear streams without gaps. Files that are missing or not audio are refused when the source is created; damaged ones
play what can be decoded. Sounds are positioned relative to the scene's own listener (miniaudio's relative positioning),
so scenes never move each other's listener. Spatial sounds are attenuated by inverse distance (full volume within
`MinDistance`, `MinDistance / distance` up to `MaxDistance`, constant beyond) and panned by miniaudio's model (its
stereo speakers face left and right, so a sound in front reaches each at half gain); there is no doppler effect.

The scene drives it while running. `OnRuntimeStart` creates a sound for every `AudioSourceComponent` with a clip,
playing when `PlayOnStart` is set; sources with `Stream` stream the clip asset's file without loading the asset. Each
update compares the components with the settings last applied, so changes from anywhere (inspector, scripts,
automation) reach the sounds: a new clip, or a change of `Stream`, makes a new, stopped sound; sources added
while running start like the others; removed components and destroyed entities lose their sound. Spatial sources
follow their entities' world positions, and the listener follows the first active `AudioListenerComponent` in hierarchy
order, else the primary camera, else the origin facing -Z. `Scene::SetPaused` holds the scene's sounds.

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
entry points of `Strada.Interop.Host` by name: 11 for the runtime, the game assembly and C# script instances, and 10
for visual scripts (`VisualScript*`, §10.6); a missing one fails `ScriptEngine::Init`. Game assemblies load into a
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
`OnClick(Entity button)` (a UI button that is the entity or one of its descendants was clicked).
Public fields (and private fields marked `[SerializeField]`) of supported types are editable in the inspector and
serialized in the scene: `bool`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `string`, `Vector2`, `Vector3`,
`Vector4`, `Quaternion`, `Color`, `Entity`, `Prefab`, `AssetHandle`, typed asset references (`Mesh`, `Material`,
`Texture`, `AudioClip`, `Font`, `EnvironmentMap`), and enums. Enums are stored as integers of their size (the class
metadata lists the enumerators in declaration order and marks `[Flags]` enums; stored values out of the enum's range
are ignored) and typed references as asset handles (the metadata names the accepted asset type).

One scene runs scripts at a time (the `ScriptEngine`'s scene context, which the bindings act on). At runtime start
every instance is created before the first `OnCreate`, so scripts can find each other there. Each update first
follows `ScriptComponent` changes (new components get instances and `OnCreate`; a changed class or a removed
component gets `OnDestroy`; unknown classes are logged once per class name), then calls `OnUpdate`; `OnFixedUpdate`
runs before every fixed step, with or without physics; contacts reach the scripts of both entities, the other entity
arriving as its script instance when it has one; clicks reach the scripts of the button and its ancestors before the
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
  `LinearDamping`, `AngularDamping`, `AddForce(Vector3, ForceMode)`, `AddTorque(Vector3, ForceMode)`,
  `MoveKinematic(Vector3, Quaternion)`, `Teleport(Vector3, Quaternion)`, `IsSleeping`, `WakeUp()`),
  `BoxColliderComponent`, `SphereColliderComponent`, `CapsuleColliderComponent`, `MeshColliderComponent`,
  `JointComponent`, `CharacterControllerComponent` (every field, `Move(Vector3 velocity)`, `Velocity`, `IsGrounded`,
  `GroundNormal`, `Teleport(Vector3)`),
  `AudioSourceComponent` (`Play()`, `Stop()`, `Pause()`, `IsPlaying`, `Volume`, `Pitch`, `Loop`, `Clip`, `Stream`),
  `AudioListenerComponent`, `TextComponent`, `SpriteRendererComponent`, `ParticleSystemComponent` (every field,
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
  `GetGamepadAxis`, `IsGamepadButtonDown/Pressed`).
- `Time`: `DeltaTime`, `FixedDeltaTime`, `Elapsed`, `FrameCount`, `TimeScale`.
- `Physics`: `Gravity`, `Raycast(origin, direction, maxDistance, out RaycastHit, layerMask)`.
- `Log`: `Trace/Info/Warn/Error`. `Debug`: `DrawLine(from, to, color, duration)`.
- `SceneManager`: `LoadScene(path)`, `CurrentSceneName`. `Application`: `Quit()`, `WindowWidth/Height`, `IsEditor`.
- `Assets`: `Load<T>(path)` for `Prefab`, `Material`, `Mesh`, `AudioClip`, `Texture`, `Font`, `EnvironmentMap`,
  `VisualScript`.
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
`Prefab`, `VisualScript`). Calls without a running scene
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
  (Constant, Linear and cubic Hermite with Catmull-Rom auto tangents; keys at one time make a jump). A run is a fiber
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
  graphs as their interfaces (kind, variables, events). The runtime accepts any bytes there (a malformed bundle counts
  as empty). Each library is analyzed as its own document, callees first, without nesting, and kept in the runtime's
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
  type, `Time.DeltaTime` and input. ScriptCoreTests replace it with a fake engine. Calls of reflected API members are
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
  `Invoke` dispatches the C# `ScriptEvent`s. Update sets `Time.DeltaTime`, runs the input events in node order and then
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
  Renderer Info, Console, Scene Settings, Project Settings; a menu bar and a toolbar with the play controls (Play/Stop,
  Pause, Step and Simulate buttons with drawn icons). View > Reset Layout restores the default docking layout. The look
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
  tooltips. The default layout gives the inspector about a quarter of the window.
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
  the scene view's and the game view's renderers drew in their last frames side by side (GPU time, draw calls and the
  shadow-map share, triangles, culled submeshes, lights and culled lights, dropped shadows, sprite, text and UI quads).
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
  scripts draw, and the UI canvases. Colliders show the shapes physics collides with: mesh colliders as the convex hull
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
  input as well. Play's start begins a clean input frame with held keys staying down; Stop releases them and begins one.
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
  directly, and `ui.layout` tells where the UI's elements and buttons are in the game view.
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
  | L2a Document editing | `VisualScriptEditing` (every edit as an operation on a copy of the document, `SetSetting`'s table of settings other operations own), `VisualScriptLanguageEditing` (arrays, formulas, timelines), `VisualScriptTimeline` (the curve model), `VisualScriptCrossGraph` (retargeting, kinds, the declared pins of cross-graph nodes, broken referrers), Arrange, the clipboard, the palette's index and ranking, quick fixes, find |
  | L2b Commit | `VisualScriptNormalization` (graph.set's transforms), `PrepareVisualScriptCommit` with its timings, `SetVisualScriptCommand`, `SetVisualScriptAndComponentsCommand`, the history changes, layout-only commits |
  | L3 Canvas model | The semantics seam (`EngineVisualScriptSemantics`, or the fixture semantics of tests), the view, metrics and text ladder, theme tables, the visual model, geometry, the spatial index, hit testing, the editor model (gestures in, intents out), the per-user view settings, `VisualScriptTextEstimate` (text widths without a font), `VisualScriptProjectIndex`, `VisualScriptCurveView` (the curve editor's view, hit testing and gestures) |
  | L3b Batch | `VisualScriptBatch` (`graph.edit`'s executor), `VisualScriptHeadlessCanvas` (node sizes, pin rows and anchors without a window), `VisualScriptCanvasPins` (pin rows from a canvas, shared with the panes) |
  | L4 Panes | The palette popup, My Script, Details (the formula field, the array literal editor, the Timeline section, the Graph pickers), the drawer (with `VisualScriptCurveEditor`, its Timeline mode), the find bar, the literal editor (built on `FieldEditor`) |
  | L5 Canvas UI | The controller (an intent becomes at most one commit request), the renderer (comment labels among its layers), brushes, the panel, the in-place formula editor, the key and gesture tables |
  | L6 Shell | `VisualScriptEditors` (the window manager, the project index's owner, opening and Back and Forward across windows), `EditorEditingKeys`, `EditorLayer`'s wiring, the entry points, the `graph.*` UI commands |

  The editor model and the document editing are independent of each other: the model emits intents, which are data,
  and only the controller turns them into edits. L2a and L3 include nothing of each other, with one exception: the curve
  model (`VisualScriptTimeline.h`, pure, without state) is shared by both, so the curve view clamps and evaluates with
  the document editing's own functions; the types both layers read (`VisualScriptProjectGraph` among them) are in
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
  `FindClaimedEditingChords` returns) act on the selected file there. An Edit item shows its shortcut exactly when
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
  (`VisualScriptCurveEditor` over the pure `VisualScriptCurveView`, whose view, hit testing and gestures are tested
  without ImGui). It opens from a double-click on a Timeline's header, the node menu's Edit Timeline, Details' Edit and
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
  (`ArrangeNodes` with the window's or the headless canvas's extents). A sub-graph's headless canvas is laid out again
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
  `material.*` (create, get, set; edits are undoable without marking the scene modified), `graph.*` (visual scripts:
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
  view), `viewport.*` (screenshot, camera, frame entities, overlays, renderer statistics),
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
  100. A game that cannot start exits with 1, a wrong command line with 2.

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

Dist builds of the runtime have no console window on Windows, log only warnings and errors, to a file in the user
data directory, and compile out asserts and script debug lines.

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
  multiplies the rounds). Crash handling runs in a child process (`StradaCrashTester`). Visual scripts are tested with
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
  third round, the shared language document (expressions, arrays, strings, generics, a timeline).
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
  skips, command-line errors, a windowed run's presented image).
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
  and no warning. Its test scene's 29 checks are graphs too (`Tests.sgraph`, a latent function per check called in
  order from a Sequence, reporting through the `Checks` library); `StradaRuntimeTests` runs them through the player
  ("Tests passed: 29 checks"), plays the title and runs the exported game, refusing any line from a visual script;
  `PlayModeTests` plays it through `input.set`, and the fuzz test of damaged graphs includes its `Game.sgraph`.
- The Showcase's **Graph Garden**: chimes played by six visual scripts (`Scripts/GraphGarden`; `Tools/GraphGarden.py`
  authors them and `Tools/Author.py --exhibit GraphGarden` rebuilds the exhibit in place, with a hash guard that names
  any file outside the exhibit that changed), whose score is a Timeline; seven of the Showcase's 68 checks look at it.
- **Sample graphs read well**: `SampleGraphsTests` (StradaEditorTests) requires of every graph of Prism Break and the
  Graph Garden that every node is placed in exactly one titled comment box, none overlapping or closer than 16 units,
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
  reads values 10 times a second, about 0.15 ms per frame. The remainders are runtime work (a cheaper Record mode, a
  binary value block, reading only the sources that changed, a document reader without a JSON document per member).
- CI: Windows (MSVC), Ubuntu 24.04 (GCC + Clang), macOS (Apple Clang, arm64); format check; tests on every push.
