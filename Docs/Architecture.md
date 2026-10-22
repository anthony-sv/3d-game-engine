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
│   └── Data/                                         Test assets, visual script documents and golden images
├── Projects/FeatureTest/                             Project whose scene exercises every feature and the whole script API
├── Projects/Blocks/                                  Sample game (falling blocks) made through the agent tools
├── Projects/Showcase/                                Showcase of every feature (exhibits, camera tour, Poly Haven assets)
├── Tools/                                            Developer scripts (configure/build/test, formatting)
├── Docs/                                             Architecture contract, automation reference, scripting guide
└── .github/workflows/, .claude/skills/, .mcp.json    CI, agent skills, MCP configuration
```

Build output: `build/<preset>/bin/` contains every executable, `Strada.ScriptCore.dll` and the `strada` tool. Fonts
and other binary resources are compiled into the binaries (`cmake/StradaResources.cmake`, `EmbeddedResources`).

## 4. Engine library modules (`Strada/Source/Strada/`)

Layering (a module may include only modules listed to its left):

```
Core ← Math ← Serialization ← Platform ← RHI ← Asset ← Renderer ← Physics, Audio ← Script ← Scene ← Project
                                                     ImGui (RHI, Core)
```

`Scene` sits on top: it owns the ECS and drives the physics/audio/script runtimes and submits to the renderer.
Physics, Audio and Script read component data through `Scene`/`Entity` forward declarations, never the reverse
through includes of `Scene.h` in headers.

| Module | Responsibility | Key types |
|--------|----------------|-----------|
| Core | Base macros, logging, asserts, UUID, time, buffers, events, input, window, application loop (and its main-thread queue), layers, file system, OS queries, images, embedded resources, command line | `Application`, `Layer`, `Window`, `Input`, `Log`, `UUID`, `Timestep`, `Buffer`, `FileSystem`, `Platform`, `Image`, `Result<T>` |
| Math | Transform compose/decompose, projections, sRGB transfer functions, bounding boxes, view frustums | `Math::ComposeTransform`, `Math::DecomposeTransform`, `Math::PerspectiveReversedZ`, `AABB`, `Frustum` |
| Serialization | JSON conversions for glm, UUIDs, enums and reflected structs; deserialization warnings | `JsonTraits<T>`, `EnumTraits<T>`, `StructTraits<T>`, `DeserializationContext` |
| Platform | Child processes (reproc++): spawning, output capture, exit codes, termination | `Process` |
| RHI | Vulkan instance/device creation, NVRHI device, swapchains (one per OS window), frame pacing, embedded shader library, texture readback | `GraphicsDevice`, `Swapchain`, `ShaderLibrary`, `ReadbackTexture` |
| Asset | Asset handles and metadata, the persistent registry, the asset manager, CPU asset types, model importers, built-in primitives, the visual script format and its layout | `AssetHandle`, `AssetManager`, `AssetRegistry`, `MeshSource`, `MaterialAsset`, `TextureAsset`, `EnvironmentAsset`, `PrefabAsset`, `AudioClipAsset`, `FontAsset`, `VisualScriptAsset`, `VisualScriptSerializer`, `MeshImporter` |
| Renderer | GPU resources created from assets (meshes, textures, materials, environments, fonts), samplers and fallback textures, the scene renderer and its passes, environment processing, shadow math, text layout, sprites, picking, presentation blits | `Renderer`, `SceneRenderer`, `EnvironmentProcessor`, `QuadRenderer`, `FontAtlas`, `TextureBlitter` |
| ImGui | ImGui context/layer, NVRHI renderer backend with multi-viewport support, UI helpers | `ImGuiLayer`, `ImGuiRenderer` |
| Physics | Jolt init/shutdown, per-scene physics world, layers, contact events, queries | `PhysicsSystem`, `PhysicsScene` |
| Audio | miniaudio engine, per-scene sound sources and listener, waveforms of clips | `AudioEngine`, `AudioScene`, `ComputeAudioWaveform` |
| Script | .NET runtime hosting (hostfxr), managed bridge, native bindings, script instances and fields, hot reload, the public variables of visual scripts as script fields, the visual scripts of the runtime (node library, analysis, programs, graph instances) | `ScriptEngine`, `DotNet::LoadAssembly`, `ScriptBinding`, `DescribeVisualScriptVariables`, `VisualScriptEngine`, `VisualScriptLibrary` |
| Scene | ECS scene, entities, components, component registry/reflection, serialization, prefab instantiation, runtime lifecycle (C# scripts and visual scripts), post-process volume blending, screen-space UI layout and interaction, render submission | `Scene`, `Entity`, `ComponentRegistry`, `ComponentTraits`, `SceneSerializer`, `PostProcessVolumes`, `UILayout`, `RenderScene` |
| Project | Project files and settings, the C# script project, the scene runner (play mode and the player), game export | `Project`, `ProjectSettings`, `ScriptProject`, `SceneRunner`, `GameExporter` |

### 4.1 Subsystem lifetime

Subsystems are static facades with explicit `Init()`/`Shutdown()` (Hazel style). The entry point initializes the log;
`Application` then initializes, in this order: AssetManager → PhysicsSystem → AudioEngine (a null device when
headless) → ScriptEngine (optional: without .NET, scenes run without scripts) → the window (unless headless) →
GraphicsDevice → ShaderLibrary → Renderer → the ImGui layer. The graphics subsystems are skipped when `EnableGraphics`
is off (the headless player), and applications that do not require them (the headless editor) continue without a
GPU when none is available. Shutdown detaches the layers, then shuts down scripting, audio, physics and the assets
(before the renderer, which owns the GPU objects created from them), the graphics and the window. Every `Init` asserts
it is not already initialized and every `Shutdown` must leave no state behind so tests can init/shutdown repeatedly.

### 4.2 Threading model

- The **main thread** owns the ECS, assets, rendering submission, scripting and editor state.
- Jolt runs its own job threads; contact callbacks only enqueue events (thread-safe queue) drained on the main thread.
- miniaudio runs its own audio thread; only the miniaudio API is touched from the main thread. The audio streaming
  thread reads and decodes streamed sounds into their ring buffers, which the audio thread copies from without waiting.
- Background work (script builds, exports, the automation socket) never touches engine state; results are
  marshalled with `Application::SubmitToMainThread(std::function<void()>)`. The editor notices edited scripts by
  checking their modification times on the main thread once a second, and rescans the assets on the main thread when
  it gets the focus back.

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
  handles) and unloads modified ones. Moves and deletes go through the manager so handles stay valid.
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
`OnUpdate`. Presses and releases are Input's one-frame flags or the button's changes since the last update, so they
also count when updates are further apart (a paused scene stepped, automation setting the button before it advances
the scene). A release that `Input::ReleaseKeysAndMouseButtons` made (the window lost the focus, the editor took the
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
interprets. The loader checks shape only: a header other than version 1 of type `VisualScript`, a content object that
is not an object, and an unknown key under `UnknownFieldPolicy::Error` refuse the file; everything else that is damaged
is repaired (an element with a bad ID, name or type is dropped, and of two with one ID the later in document order;
values of the wrong kind take their defaults; geometry is clamped) and reported as an issue with its path, such as
`Graph.Nodes[3].ID`. Files load with unknown keys as warnings and log each issue with the file; strict use (automation
input) refuses input with any issue. Names (of variables, locals, functions, parameters and former names) are 1 to 64
bytes of UTF-8 without control characters, `.` (reserved for the fields of split pins) or spaces at either end. Limits:
10000 nodes, 40000 links and 2000 comments per graph, 1024 variables and functions, 64 parameters per list, 256 locals
per function, 16 former names per variable. A `Range` is two floats, the minimum first; like float literals, they are
finite numbers of at most 3.4028235e38 in magnitude (float's largest value written with the fewest digits). The writer
writes keys in a fixed order, omits optional keys at their defaults and writes raw values as stored, so what loads
saves and loads again without issues.

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
scripts and visual scripts.

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
`Directory.Build.props` keeps outputs in the build tree): `Strada.ScriptCore.dll` lands next to the executables.

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
compilation and execution. C++ never derives pins or types itself; it asks the VM through the ten `VisualScript*` entry
points of `Strada.Interop.Host` (§10.1: `VisualScriptDescribeLibrary`, `VisualScriptAnalyze`, `VisualScriptLoad`,
`VisualScriptCreateInstance`, `VisualScriptInvoke`, ...). They are thin adapters that marshal UTF-8 and answer a status:
Success 0, Failure 1 or NotFound 2. On Success, and only then, the JSON answer goes to the `receive` callback, exactly
once. Without .NET a document still loads, saves and shows in the editor; analysis and running need the runtime.

- **Node library**: the built-in node kinds (events, flow, variables and functions, data, operators) and one node per
  usable member of the scripting API:
  - methods (`Call:`);
  - property and field getters and setters (`Get:`/`Set:`; setters only for static members and reference types);
  - constructors (`New:`);
  - generic methods constrained to components, assets or scripts, through a `TypeArguments` setting.

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
  - a null Target or array;
  - an index outside its array;
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
  as no dispatch or execution of the old one runs.
  - Variables carry over by ID, then by name and type; the others start at their `Value`, else the type default. A
    value the component stores for a variable the edit adds is not given to it: the variable keeps its `Value` until
    that value is edited or a new instance starts (play starts again, or the component's graph changes). An edit made
    before the instance takes the live edit (the graph saved while paused, its variable edited before the next frame)
    reaches the variable as the instance takes it.
  - Node states and stored outputs carry over by node ID while the node's type is unchanged.
  - Fibers survive while every position they hold still exists; otherwise they are cancelled with one log line.

  A broken document leaves the running program alone. Before the game assembly unloads, the runtime destroys the graph
  instances, reduces every program to its name and document and drops the invokers, so nothing holds the collectible
  assembly. Programs compile again from their documents when their handle is next used, logging their errors once if
  they no longer compile (`<graph> was compiled again for the changed game assembly: ...`).
- **Debug hook**: `AttachDebugger(IVisualScriptDebugger)` lets a debugger choose which executions to trace, by entity:
  `IsTarget` is asked when an execution starts or a fiber resumes. The debugger then hears of every instruction those
  executions run (`OnStep`: the node and the exec output the flow left through) and of every value their pure nodes
  compute (`OnValue`). Without a debugger an instruction costs one null check. Breakpoints and stepping (slice 4) will
  suspend executions with the same fiber machinery.
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
  unchanged graph costs a lookup and a few comparisons and the scene asks every update. A new object or version with an
  equal document (a save, then a refresh) only updates the record; another document is sent (`VisualScriptLoad`, named
  by the asset's path, a memory asset's name, else its handle) and the record takes it whatever the outcome, so a broken
  document is sent once. Its errors are logged once: `Visual script '<path>' has 1 error; first: <message> (node <id>)`
  (`<n> errors`; `(variable <id>)` for a variable, nothing for the document), plus `; the previous version keeps
  running` when the handle has a working program. After a game-assembly change the working program may no longer
  compile: the next document sent goes after the working one, sent again first, which tells; until then the suffix is
  not claimed. A document that did not compile is sent again after a game-assembly change, as it may compile now, so
  the record always knows which document instances run. The name is looked up again only when the asset registry
  changed (`AssetManager::GetVersion`): a graph moved or renamed is sent again under its new path, its working program
  first, so the runtime's messages name the file that exists. Generations come from one engine-wide counter: a record
  takes a new one when another document is sent or one is sent again after a game-assembly change, when its graph comes
  back after being absent and at every game-assembly change (a new name gives none: nothing can start that could not).
  `CreateInstance` loads first and gives the instance the component's values; its Failure (no working program) is a
  script exception (`ScriptHost::OnScriptException`), as is a Failure of `Invoke`, and an `Invoke` answered NotFound
  forgets the instance. `Analyze` sends a document for analysis and keeps nothing. Programs stay with the runtime until
  `Shutdown`: `VisualScriptUnload` is resolved but has no caller.
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
  `PinRenames` for those resolved inexactly) and the diagnostics.
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
  types components cannot set. C# scripts and visual
  scripts dropped onto the inspector run on the selected entities. Without selected entities the inspector shows the
  asset selected in the content browser (`EditorContext::SelectAsset`; selecting entities replaces it): its type and
  reference, the parameters of materials with a preview (editable for material files; Create Material copies built-in
  and mesh-imported ones), a texture preview, mesh statistics with the mesh's materials, what a visual script holds
  (read-only: its description, the numbers of nodes and links of all its graphs, the events it uses, its variables with
  their types, values and whether they are public, and the signatures of its functions, marked when pure), and the
  details of other asset types. Showing an asset never writes its file. The Transform of UI
  elements, which their `UIRect` places, starts folded and is shown read-only, with a note. Every field table's name
  column starts as wide as its names need, switches and indentation included (30% to 60% of the panel, at most 50%
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
  Double-click opens folders, scenes, scripts and other files (with the desktop's application, `Desktop::Open`); the
  folder tree opens and scrolls to the displayed folder. F2 renames, Delete deletes after a confirmation. Assets,
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
  to drop it onto an entity). Context menus (and the toolbar's "+") create folders, materials, visual scripts
  (`New Visual Script.sgraph`, numbered when taken, from the Events template: an `Event.Create` and an `Event.Update`
  node; `EditorOperations::CreateVisualScript`, selected and ready to be named) and C# scripts (the New Script dialog,
  in the displayed script folder; the browser then shows the script), import files (native multi-select dialog),
  refresh, rename, delete, copy references or paths and show items in the file manager. File operations are not
  undoable. Files and folders dropped onto any of the editor's windows from the OS (`WindowFileDropEvent`, forwarded
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
  `editor.entity-menu` opens it for agents), View, Play (Play/Stop, Simulate, Pause, Step). Shortcuts are global
  unless a text field is being edited.
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
- Undo/redo for every scene modification through a command history (JSON before/after snapshots). Automation
  commands use the same history.
- Play mode (`PlayMode`, controls in the toolbar and the Play menu): Play (Ctrl+P; scripts, physics and audio, shown in
  the game view) and Simulate (physics only, seen in the scene view, with gizmos) run a copy of the edited scene
  through a `SceneRunner`; Pause (Ctrl+Shift+P), Step (Ctrl+Alt+P) and Stop. While playing, the panels and automation
  show and edit the copy (`EditorContext::BeginPlay`) with an undo history of its own; stopping discards the copy, its
  history and every change, and the selection keeps what still exists. Play builds changed scripts first. Scene and
  project files cannot change while playing (the UI stops playing first). Scripts' `Application.Quit` stops playing
  and `SceneManager.LoadScene` replaces the copy. A blue (Play) or green (Simulate) frame marks the scene view while
  playing; stopping brings the scene view back to the front.
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
- Project management: File > New Project (name and location; the start scene is the editor's sample scene),
  Open Project, Recent Projects (user data `Editor/RecentProjects.json`), Close Project, and `--project <file>` on the
  command line. The Project Settings panel edits the settings (generated like the inspector, with a layer collision
  matrix); they are saved to the project file when an edit ends and are not part of the scene's undo history. Scenes
  saved inside the asset directory are registered as assets. Without a project the editor works on loose scene files.
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
- Headless mode (`--headless`): no window/ImGui; offscreen rendering if a GPU is available; used by automation and CI.

## 12. AI automation

- `CommandRegistry` (editor): named commands with a description, JSON-schema parameters and a handler returning a
  JSON result or a structured error. Long operations complete asynchronously. All commands execute on the main
  thread; the active project/scene state is identical whether a human or an agent drives the editor.
- `AutomationServer`: JSON-RPC 2.0 over TCP bound to `127.0.0.1`, newline-delimited messages, authenticated by a
  random 256-bit token (first request `authenticate`, constant-time comparison). A network thread owns the sockets
  (framing, size limits, parsing, authentication) and never touches editor state; the editor layer runs queued requests
  on the main thread once per frame (`ProcessRequests`). The editor writes `{pid, port, token, project, version}` to
  `<user data>/Editor/Instances/<pid>.json` (user-only permissions), rewrites it when the open project changes and
  removes it on exit.
- The editor model (`EditorContext`: scene, file, undo `CommandHistory`, selection; `EditorOperations`) is shared by UI
  panels and automation. The protocol and the command reference are in [Automation.md](Automation.md).
- `strada` tool (C#, `StradaTool`, no dependencies beyond .NET): `strada mcp` (MCP stdio server written against the
  protocol; tools are the editor commands, names `domain_action`, schemas from `editor.commands`; attaches to a running
  editor or starts one on demand, and lists the tools from a headless editor started for that when none runs; headless
  editors it starts close with it through the editor's `--parent-process`; screenshots returned as image content;
  failed commands are tool results with `isError`; strada's own tools search and import Poly Haven HDRIs),
  `strada call <command> [json]`, `strada launch`, `strada commands`, `strada instances`, `strada hdris`, `strada hdri`,
  `strada models`, `strada model`, `strada textures`, `strada texture`. Editors ignore SIGPIPE (`Platform::IgnoreBrokenPipeSignal`), so the ones strada starts with a
  window outlive its output pipes.
- Command domains: `editor.*` (status, undo, redo, commands, quit, the Entity menu as users see it), `project.*` (info,
  create, open, close, settings, export),
  `scene.*` (new, open, save, hierarchy, settings, dump), `entity.*` (create with components, delete, duplicate,
  rename, reparent, find, get, select), `component.*` (types, add, remove, get, set with partial JSON patch),
  `asset.*` (list, get, select, import, refresh, browse and drop in the content browser, move, delete, create-folder,
  move-folder, delete-folder),
  `material.*` (create, get, set; edits are undoable without marking the scene modified), `graph.*` (visual scripts:
  create, get, set and check documents, checked strictly, with the scripting runtime's analysis; edits are undoable
  without marking the scene modified; library and types list the runtime's node kinds and types), `prefab.*`,
  `script.*` (create from template, build with diagnostics, classes and fields), `play.*` (start, stop, pause, step, advance N frames, state),
  `input.*` (inject keys/mouse during play), `ui.*` (where the screen-space UI's elements and buttons are in the game
  view), `viewport.*` (screenshot, camera, frame entities, overlays, renderer statistics),
  `inspector.*` (the Add Component menu, as users see it), `log.*` (read since index), `test.*` (run a test scene and
  return results). Renderer settings are scene settings (`scene.settings`).
- Claude Code integration: `.mcp.json` registers `strada mcp` (through `dotnet run`, so it works in any checkout);
  the `strada-make-game` skill in `.claude/skills/` documents building games with the tools.

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
- `--headless` runs without a window, rendering or sound (no GPU needed); the scenes see the game's window size.
  `--frames N` and `--screenshot <png>` serve smoke tests.
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

| Config | Optimization | Asserts | Logging | Vulkan validation |
|--------|--------------|---------|---------|-------------------|
| Debug | off | on | all | on if available |
| Release | on (+ debug info) | on | all | opt-in (`--validation`) |
| Dist | on | off | warnings+ to file | off |

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
  failures as script exceptions, the step budget, C# calling graphs). Every graph a scene of `Projects/` uses must load
  strictly, analyze without errors and resolve its nodes exactly or by a redirect, so an API change that breaks one
  needs a redirect entry.
- `StradaEditorTests`: automation commands executed headlessly on a temporary project. A fuzz test sends every command
  parameters built from its schema and the editor's state (mostly names of what exists, plus values to refuse; damaged
  graph documents to `graph.set` and `graph.check`), while editing and playing: every request answers once without
  InternalError, the scene stays loadable, undoing and redoing everything restores it, and nothing outside the project
  changes. The graph commands are tested with and without the scripting runtime: strict refusals naming every issue,
  renamed variables, layout, undo restoring the file's bytes, the library's filters, and edits of running graphs and
  their variables while playing. A copy of the Blocks sample plays through
  `input.set` the way a player plays it: it opens at the title's view with its sign shown and a field of view widened
  for the 4:3 game view, then takes clicks on its menus' buttons where `ui.layout` shows them, the arrow keys and Enter
  in its menus, and Escape, P, R and Space on its title, game, pause and game-over screens (Space mashed through the
  end of a game leaves the game-over screen up), down to the title's Quit, which ends play mode.
- `StradaRuntimeTests`: the `StradaRuntime` executable runs games written to temporary directories (exported and project
  layouts, test runs and their exit codes, games whose only scripts are visual scripts, command-line errors, a windowed
  run's presented image).
- `StradaToolTests` (xUnit): the strada command line, instance files and editor discovery, the MCP server against a
  stand-in editor, Poly Haven downloads against a stand-in server, and MCP sessions with the real editor started
  headless (`STRADA_EDITOR`; ctest sets it), among them an agent's whole workflow: a project, a script it writes and
  builds, a scene that uses it, a test run in play mode, and an export whose player passes the same checks.
- `ScriptCoreTests` (xUnit): math types, the visual script VM and an API-coverage test asserting every public
  `Strada.ScriptCore` member is used by the FeatureTest scripts. The VM's tests build runtimes of their own over fake
  engines (`IVisualScriptEnvironment`) and never touch native code: the library (inclusion, purity, resolution,
  identical bytes in every culture), analysis and its diagnostics, every node kind, suspension and timing, budgets,
  live edits, game-assembly reloads, the protocol of the entry points, the C# API and the debug hook. The coverage test
  reads the scripts' compiled metadata: references to every type, method (accessors and operators included, overloads
  matched by signature) and field; Script's callbacks and protected constructors count when derived types override or
  call them; constants and enumerators, which compile to literals, count through their types.
- `Projects/FeatureTest`: scenes that use every component (checked by `StradaTests`) and scripts that call the entire
  scripting API through `Strada.Testing`, one script per area (entities, transforms, components, physics, characters,
  audio, assets, math, input, application and time, the testing API, attributes, script lifecycles, the UI, visual
  scripts) with a coordinator that loads the second scene, which finishes the run. Its graphs use every built-in kind of
  node and report their checks through `TestReporter` nodes: `Assets/Scripts/Features.sgraph` on the visual-script test
  entity (comparisons, logic and `Select` checked to give false as well as true), `Contacts.sgraph` on a falling box and
  ball beside their C# scripts and `Clicks.sgraph` on the UI canvas; `VisualScriptTests` waits for them, checks what
  they counted and drives them from C#. `Tools/AuthorGraphs.py` writes the three graphs through `strada mcp`
  (`graph.check`, then `graph.set`) and runs the tests, so a new built-in kind of node gets its check there. CMake
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
- CI: Windows (MSVC), Ubuntu 24.04 (GCC + Clang), macOS (Apple Clang, arm64); format check; tests on every push.
