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
├── Strada-ScriptCore/                                C# scripting API (assembly Strada.ScriptCore.dll)
├── StradaTool/                                       C# CLI + MCP stdio bridge (executable `strada`)
├── Tests/
│   ├── StradaTests/                                  C++ unit tests (doctest) for the engine library
│   ├── StradaEditorTests/                            C++ tests for editor automation commands (headless)
│   ├── StradaRuntimeTests/                           C++ tests running the game player on generated games
│   ├── ScriptCoreTests/                              C# unit tests (math, API coverage checks)
│   ├── StradaToolTests/                              C# tests of strada (MCP server, sessions with the headless editor)
│   ├── TestScripts/                                  C# script assembly used by C++ scripting tests
│   └── Data/                                         Test assets and golden images
├── Projects/FeatureTest/                             Project whose scene exercises every feature and the whole script API
├── Projects/Blocks/                                  Sample game (falling blocks) made through the agent tools
├── Projects/Showcase/                                Showcase scene (Poly Haven assets, physics playground, sound)
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
| Asset | Asset handles and metadata, the persistent registry, the asset manager, CPU asset types, model importers, built-in primitives | `AssetHandle`, `AssetManager`, `AssetRegistry`, `MeshSource`, `MaterialAsset`, `TextureAsset`, `EnvironmentAsset`, `PrefabAsset`, `AudioClipAsset`, `FontAsset`, `MeshImporter` |
| Renderer | GPU resources created from assets (meshes, textures, materials, environments, fonts), samplers and fallback textures, the scene renderer and its passes, environment processing, shadow math, text layout, sprites, picking, presentation blits | `Renderer`, `SceneRenderer`, `EnvironmentProcessor`, `QuadRenderer`, `FontAtlas`, `TextureBlitter` |
| ImGui | ImGui context/layer, NVRHI renderer backend with multi-viewport support, UI helpers | `ImGuiLayer`, `ImGuiRenderer` |
| Physics | Jolt init/shutdown, per-scene physics world, layers, contact events, queries | `PhysicsSystem`, `PhysicsScene` |
| Audio | miniaudio engine, per-scene sound sources and listener, waveforms of clips | `AudioEngine`, `AudioScene`, `ComputeAudioWaveform` |
| Script | .NET runtime hosting (hostfxr), managed bridge, native bindings, script instances and fields, hot reload | `ScriptEngine`, `DotNet::LoadAssembly`, `ScriptBinding` |
| Scene | ECS scene, entities, components, component registry/reflection, serialization, prefab instantiation, runtime lifecycle, post-process volume blending, screen-space UI layout and interaction, render submission | `Scene`, `Entity`, `ComponentRegistry`, `ComponentTraits`, `SceneSerializer`, `PostProcessVolumes`, `UILayout`, `RenderScene` |
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
  them with the model. Imported material parameters are brought into the material ranges (`FitMaterialToFieldRanges`),
  so copies of the materials make valid material files: emitted light above 1 (MTL's `Ke`, FBX emission colors) moves
  from `EmissiveColor` into `EmissiveIntensity`, which keeps its brightness; other values outside a range (FBX colors
  above 1, broken files) are clamped, and NaN or infinite ones take the defaults, with a warning.
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

Every file starts with a header: `"Strada": { "Version": <int>, "Type": "Scene" | "Prefab" | "Material" | "Project" | "AssetRegistry" }`.

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
intra-prefab references (hierarchy and entity-typed script fields).

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

## 8. Physics (Jolt)

`PhysicsSystem` (global) initializes Jolt: the default allocator, the type factory and a job system with a worker
thread per core but one. `PhysicsScene` is one physics world, independent of the ECS (bodies are described by
`BodyDesc` and keyed by entity UUID), so it is tested on its own. Object layers encode the project layer and whether
the body moves (static bodies never meet each other); broad-phase layers separate static from moving bodies; the
project's ignored layer pairs never collide. An entity's colliders form one compound shape scaled by the entity's
world scale (mirroring is ignored); triangle-mesh colliders on dynamic bodies use the mesh's convex hull. Friction
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
scripts.

### 10.1 Hosting

`DotNetHost` locates `hostfxr` in the .NET host's search order (an app-local `dotnet/` directory for exported games,
`DOTNET_ROOT_<ARCH>` and `DOTNET_ROOT`, the registered install location, the default one) and loads it directly: the
SDK's static nethost library links against the static C runtime, which the engine (`/MD`) cannot mix in, so only the
SDK's hosting headers are used. It initializes the runtime from `Strada.ScriptCore.runtimeconfig.json` (CoreCLR starts
once per process and is never unloaded: `ScriptEngine::Shutdown` unloads the game and later `Init`s reuse the
runtime), loads `Strada.ScriptCore.dll` into the default load context and resolves the `[UnmanagedCallersOnly]`
entry points of `Strada.Interop.Host`. Game assemblies load into a collectible `AssemblyLoadContext` from bytes (with
their symbols, for line numbers), so the files are never locked (hot reload); `Strada.ScriptCore` and the framework
resolve to the default context's assemblies, other dependencies from the game assembly's directory. Managed
exceptions never cross into native code: every entry point catches, logs (with stack trace) and returns a status.

Class metadata and field values cross the boundary as JSON in the scene files' script field format: the runtime
describes every concrete, non-generic `Script` class with a public parameterless constructor (field names, types,
defaults read from a default instance, `[Range]`, `[Tooltip]`, `[HideInInspector]`), and instances receive their
`ScriptComponent.Fields`; stored values of fields that changed type are logged and skipped.

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

### 10.4 Scripting API (namespace `Strada`)

- `Entity`: `ID`, `Name`, `IsValid`, `Translation`/`Rotation`/`Scale` shortcuts, `Transform`, `Parent`, `Children`,
  `HasComponent<T>()`, `GetComponent<T>()`, `AddComponent<T>()`, `RemoveComponent<T>()`, `Destroy()`,
  `As<T>()` (script instance), static `Create(name)`, `FindByName(name)`, `FindByID(id)`,
  `Instantiate(Prefab, Vector3 position, Quaternion rotation, Entity parent = null)`.
- Components: `TransformComponent` (`Translation`, `Rotation`, `EulerAngles` (deg), `Scale`, `WorldTransform`,
  `WorldTranslation`, `Forward`, `Right`, `Up`), `CameraComponent` (projection fields, `Primary`,
  `ScreenToWorldRay(Vector2)`), `MeshComponent` (`Mesh`, `GetMaterial(i)`, `SetMaterial(i, Material)`,
  `CastShadows`, `Visible`), `DirectionalLightComponent`, `PointLightComponent`, `SpotLightComponent`,
  `SkyLightComponent`, `RigidBodyComponent` (`Type`, `Mass`, `LinearVelocity`, `AngularVelocity`, `GravityFactor`,
  `LinearDamping`, `AngularDamping`, `AddForce(Vector3, ForceMode)`, `AddTorque(Vector3, ForceMode)`,
  `MoveKinematic(Vector3, Quaternion)`, `Teleport(Vector3, Quaternion)`, `IsSleeping`, `WakeUp()`),
  `BoxColliderComponent`, `SphereColliderComponent`, `CapsuleColliderComponent`, `MeshColliderComponent`,
  `JointComponent`, `CharacterControllerComponent` (every field, `Move(Vector3 velocity)`, `Velocity`, `IsGrounded`,
  `GroundNormal`, `Teleport(Vector3)`),
  `AudioSourceComponent` (`Play()`, `Stop()`, `Pause()`, `IsPlaying`, `Volume`, `Pitch`, `Loop`, `Clip`, `Stream`),
  `AudioListenerComponent`, `TextComponent`, `SpriteRendererComponent`, `ParticleSystemComponent` (every field,
  `Emit(count)`, `Clear()`, `ParticleCount`), `PostProcessComponent` (every field; `Tonemapper` is a `TonemapOperator`),
  `UICanvasComponent`, `UIRectComponent`, `UIImageComponent`, `UITextComponent` (every field), `UIButtonComponent`
  (every field, `WasClicked`, `State` (`UIButtonState`), `Click()`), `ScriptComponent` (`ClassName`, `Instance`).
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
- `Assets`: `Load<T>(path)` for `Prefab`, `Material`, `Mesh`, `AudioClip`, `Texture`.
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
`Prefab`). Calls without a running scene
or on missing entities and components log a script error and do nothing; values from scripts are checked (non-finite
numbers and unknown enumerators are rejected and logged, rotations are normalized), and null arguments throw.
`Entity.Instantiate` places the prefab's root at a world position and rotation (keeping its scale) and starts the
instance's scripts before it returns. The project's materials are shared by every scene and the editor, so scripts
change only runtime materials (`Material.Create`, `Clone`), which the running scene owns and releases when it stops.
`Debug.DrawLine` lines last a duration of scaled runtime (0: one rendered frame); Dist builds ignore them.

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
  menus create preset entities (empty, primitives, lights, camera, audio, text, sprite, particle effects, a global
  post-process volume with a 10 m box for when it is made local, a character, UI), duplicate and delete; UI elements
  (Panel, Image, Text, Button) go into a canvas: the parent's, else under a parent a new canvas there, else the
  scene's first canvas or a new one, created with the element as one undo step; a search field filters by name.
  Selections made elsewhere reveal and scroll to the primary entity.
- Inspector: generated from the component registry's field descriptors (no per-component UI code): numbers clamp to
  the field ranges, colors are picked in sRGB and stored linear, quaternions are edited as Euler angles (kept stable
  while dragging), enums are combos, fields with a visibility condition (`VisibleWhen`: a material's alpha cutoff in
  the Mask mode, the perspective or orthographic settings of a camera, what a joint type or particle shape uses) show
  only while it holds, entity references accept dropped entities, and a field's switch (a post-process volume's
  override) is a check box in front of its name, the value disabled while it is off. Asset fields (in components,
  materials, settings and script fields) show the asset's path in the asset directory with its extension
  (`builtin://<Name>` for built-ins, the name of materials and textures imported with a model, `None` when empty; long
  paths lose their folders first, then the middle of the name, so its start and its end with the extension stay; the
  tooltip has all of it) in a box that opens a searchable picker of the assets of the field's type when clicked and
  takes assets dropped from the content browser, followed by `...` (the picker) and `x` (clears it). In the inspector
  the box and the picker's rows start with a small image of the asset (textures, materials on a sphere, meshes,
  prefabs, environments; `AssetThumbnails`), shown larger in the box's tooltip; the picker draws only the rows in view,
  so long lists stay quick. With several entities selected it shows the components they share, flags fields whose
  values differ, and an edit sets only the edited value or vector component on every entity. One widget interaction (a
  drag, typing into a field) is one undo step. Add Component opens a menu with a search field above a submenu per
  component category (`ComponentTraits::Category`: Rendering, Physics, Audio, Scripting, UI; the Core ones are on every
  entity); typing lists the matching components instead (names starting with the text first) with their categories,
  and Enter adds the first. Components can also be reset, copied, pasted as JSON and removed. Below the fields of a
  Mesh component, each material slot of its mesh gets a section (`MaterialSlots`): a header with the material on a
  sphere, its name, the slot and where the material comes from, then every parameter of the material the slot renders
  with (the component's override, else the mesh's own material, else the default one). Edits change the material file
  as in the material inspector (one undo step per interaction) and show at once in the views and thumbnails; built-in
  and model-imported materials show their values disabled with a Create Material button that writes an editable copy
  to `Assets/Materials` and gives it to the slot (one undo step). Without selected entities the inspector shows the
  asset selected in the content browser (`EditorContext::SelectAsset`; selecting entities replaces it): its type and
  reference, the parameters of materials with a preview (editable for material files; Create Material copies built-in
  and mesh-imported ones), a texture preview, mesh statistics with the mesh's materials, and the details of other
  asset types. The Transform of UI elements, which their `UIRect` places, starts folded and is shown read-only, with a
  note. Every field table's name column starts as wide as its names need, switches and indentation included (40% to
  60% of the panel; users can resize it); names that still do not fit end in an ellipsis and show in full in their
  tooltips.
- Content Browser: a folder tree with two roots, the open project's asset directory and its C# scripts
  (`Scripts/Source`, `ScriptBrowsing`), beside the displayed folder as tiles (folders, then assets or scripts, then
  files that are not assets, such as a glTF scene's buffers) labeled with their file names on two lines (a long name
  keeps its extension in view: `LongMat...smat`), breadcrumbs, a tile size slider, and a search over the root's paths.
  Textures show themselves, and materials (on a sphere), meshes, prefabs and environments show thumbnails that
  `AssetThumbnails` renders offscreen, one per frame, again when the asset is reloaded, unloaded because its file
  changed, or a material edited; fonts write "Aa" with their own glyphs (rasterized from the text renderer's
  distance-field atlas); audio clips show their waveform and length, decoded on a worker thread one clip at a time
  (`ComputeAudioWaveform`) and kept. Files without a picture get cards of the slate theme with a glyph and a band
  naming their type, drawn with ImDrawList (a globe for scenes, a cube for prefabs, a wireframe for meshes, a shaded
  ball for materials, a picture for textures, a sunrise for environments, a speaker until a waveform is ready, "Aa" in
  the editor's font for unreadable fonts, a C# document for scripts, a page with the extension for other files);
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
  Context menus create folders, materials and C# scripts (the New Script dialog, in the displayed script folder; the
  browser then shows the script), import files (native multi-select dialog), refresh, rename, delete, copy references
  or paths and show items in the file manager. File operations are not undoable. Files and folders dropped onto any of
  the editor's windows from the OS (`WindowFileDropEvent`, forwarded from the platform windows of floating panels
  too; `asset.drop` does the same for agents) are imported by `ProjectImport` into the folder under the cursor, else
  the displayed ones: assets into the asset folder (taken names get a number), models with the files they read at the
  same paths relative to them (`MeshImporter::FindExternalFiles`; in a new folder named after the model when those
  names are taken), C# sources into the scripts folder (creating the C# project when needed) unless their file name or
  a type they declare is taken in the scripts, where the C# project would no longer build, and folders whole but for
  hidden files, links and C# build output: their C# sources go to the scripts (all of a folder that holds no assets)
  and the rest to the assets. Other files come only with a model, and what is in a dropped folder only with it. What
  came in is selected, and a notice under the toolbar says what did not (with a button that shows the console). Files
  that other applications add, change or delete are picked up when the editor gets the focus back
  (`AssetAutoRefresh`; one of its windows, floating panels included; after a running game export, which copies the
  assets) or with Refresh, and their thumbnails are made again; a preview that failed is tried again once a refresh
  has found its file changed.
- Scene Settings: the scene name and the physics and renderer settings, generated the same way.
- Renderer Info: the device (adapter, type, Vulkan version, driver, GPU timing support, validation and its error
  count), the frame (average and slowest frame time with a graph over 240 frames, frame count, back buffer), and what
  the scene view's and the game view's renderers drew in their last frames side by side (GPU time, draw calls and the
  shadow-map share, triangles, culled submeshes, lights and culled lights, dropped shadows, sprite, text and UI quads).
- Menus and shortcuts: File (New Scene Ctrl+N, Open Scene Ctrl+O, Save Ctrl+S, Save As Ctrl+Shift+S) with native
  file dialogs, Edit (Undo Ctrl+Z, Redo Ctrl+Y / Ctrl+Shift+Z, Duplicate Ctrl+D, Delete, Select All Ctrl+A), Entity
  (create presets at the scene view's focal point: Create Empty, then the 3D Object, 2D Object, Rendering, Effects,
  Physics, Audio and UI submenus, which the hierarchy's create button and Create Child share;
  `EntityPresets::GetMenu`), View, Play (Play/Stop, Simulate, Pause, Step). Shortcuts are global unless a text field
  is being edited.
  Opening or creating a scene, and closing the editor, ask to save unsaved changes first; the window title shows the
  scene name and a `*` while it has unsaved changes.
- Scene View (`ViewportPanel`): the scene (the edited one, or the running copy while playing) through the editor camera
  (fly/orbit/pan/zoom/focus). Its toolbar picks the tool, Select (no gizmo), Move, Rotate or Scale (Q/W/E/R), the gizmo's
  orientation (local/world, X), snapping (Ctrl toggles it while dragging) and its steps, and the overlays
  (`SceneViewOverlays`, also `viewport.overlays`): ground grid (G), selection outline, light and camera gizmos
  (clickable billboard icons per kind of light and for cameras), camera frustums, light shapes (directional light rays
  kept at a constant size on screen, point light ranges, spot light cones with their inner cone), colliders (character
  capsules among them), joints and post-process volumes (the boxes of local volumes with the rounded outlines where
  their blend distances end), each for the selected entities or every entity, the lines scripts draw, and the UI
  canvases. Colliders show the shapes physics collides with: mesh colliders as the convex hull Jolt builds
  (`MeshColliderOutline`; also for the triangle meshes of dynamic bodies) or as their triangles, without the diagonals
  of flat faces, and character controllers as the upright capsule physics moves. Its lines are wide, light-colored and
  outlined in dark so they read on bright and dark surfaces, and dimmed where surfaces hide them, except joints (their
  anchors lie inside bodies), directional lights (their position does not matter), post-process volumes (the camera
  is often inside), the capsules of selected characters (their models enclose them) and the outlines of canvases,
  which are drawn over everything. The screen-space UI would cover the scene: the UI canvases overlay draws each
  top-level canvas's UI in its entity's XY plane instead (`CanvasOverlay`), standing on the entity (the middle of its
  bottom edge there), laid out at its reference resolution, 1 cm per canvas unit, faint (too faint to pick) until the
  canvas or one of its elements is selected, with the outline of every canvas and, for the canvas holding the
  selection, the rectangles of its elements, the selected ones highlighted. Focus and `viewport.frame` frame a canvas
  or a UI element's rectangle there (`ComputeEntityBounds`). UI elements, which their `UIRect` places, get no
  transform gizmo. ImGuizmo edits apply to every selected root around the primary selection as one undo step per drag
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
  once (hot reload; while scripts run, when they stop). The inspector edits Script components with a class picker and
  the fields of the class: typed asset pickers, enum combos, flag check boxes, colors and Euler angles like component
  fields, one undo step per interaction.
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
- Command domains: `editor.*` (status, undo, redo, commands), `project.*` (info, create, open, close, settings, export),
  `scene.*` (new, open, save, hierarchy, settings, dump), `entity.*` (create with components, delete, duplicate,
  rename, reparent, find, get, select), `component.*` (types, add, remove, get, set with partial JSON patch),
  `asset.*` (list, get, select, import, refresh, browse and drop in the content browser, move, delete, create-folder,
  move-folder, delete-folder),
  `material.*` (create, get, set; edits are undoable without marking the scene modified), `prefab.*`,
  `script.*` (create from template, build with diagnostics, classes and fields), `play.*` (start, stop, pause, step, advance N frames, state),
  `input.*` (inject keys/mouse during play), `viewport.*` (screenshot, camera, frame entities, renderer statistics),
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
  scripts (exported games carry `Strada.ScriptCore.dll` next to their assembly, projects use the engine's; a project
  whose C# project was never built does not start), loads the start scene (or `--scene <path in Assets>`) and runs it
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
- Games with scripts need the .NET 10 runtime: installed, or shipped by the developer in `dotnet/` next to the
  executable, where the player looks first.

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
  the FeatureTest project's scenes, prefab, material, asset registry and project file deterministically (members and
  elements removed or repeated, values of another kind or extreme): every result must be refused or repaired, what
  loads must save and load again, scenes and prefab instances that load must run with physics and sound, and project
  settings that load must simulate a scene (`STRADA_FUZZ_SCALE` multiplies the rounds). Crash handling runs in a child
  process (`StradaCrashTester`).
- `StradaEditorTests`: automation commands executed headlessly on a temporary project. A fuzz test sends every command
  parameters built from its schema and the editor's state (mostly names of what exists, plus values to refuse), while
  editing and playing: every request answers once without InternalError, the scene stays loadable, undoing and redoing
  everything restores it, and nothing outside the project changes.
- `StradaRuntimeTests`: the `StradaRuntime` executable runs games written to temporary directories (exported and project
  layouts, test runs and their exit codes, command-line errors, a windowed run's presented image).
- `StradaToolTests` (xUnit): the strada command line, instance files and editor discovery, the MCP server against a
  stand-in editor, Poly Haven downloads against a stand-in server, and MCP sessions with the real editor started
  headless (`STRADA_EDITOR`; ctest sets it), among them an agent's whole workflow: a project, a script it writes and
  builds, a scene that uses it, a test run in play mode, and an export whose player passes the same checks.
- `ScriptCoreTests` (xUnit): math types and an API-coverage test asserting every public `Strada.ScriptCore` member
  is used by the FeatureTest scripts. It reads their compiled metadata: references to every type, method (accessors
  and operators included, overloads matched by signature) and field; Script's callbacks and protected constructors
  count when derived types override or call them; constants and enumerators, which compile to literals, count
  through their types.
- `Projects/FeatureTest`: scenes that use every component (checked by `StradaTests`) and scripts that call the entire
  scripting API through `Strada.Testing`, one script per area (entities, transforms, components, physics, audio,
  assets, math, input, application and time, the testing API, attributes, script lifecycles, the UI) with a
  coordinator that loads the second scene, which finishes the run. CMake builds the scripts
  (`StradaFeatureTestScripts`) and `StradaRuntimeTests` runs a copy of the project through the player
  (`StradaRuntime --project ... --test`; exit code = failures).
- `Projects/Blocks`: a sample game made the way an agent makes one, through `strada mcp`: `Tools/Author.py` builds the
  project, its materials, both scenes and the debris and particle burst prefabs, with the sounds, the beveled block mesh
  and the grid texture that `Tools/GenerateAssets.py` synthesizes and a Poly Haven sunset sky over a neon grid floor.
  Falling blocks with pure C# rules (`Board`, `Piece`, `PieceBag` with a look-ahead queue), the `BlocksGame` script
  (title, pause and game-over states, hold, a three-piece next queue, a best score kept in the user's data folder,
  sounds, rows that burst into physics debris and sparks, dust under hard drops, a camera shake), its `Hud` (screen
  texts that follow the window's size) and a test scene whose `BlocksTests` check the rules and play the game. `StradaRuntimeTests` plays that scene
  and runs the game's scene headless.
- `Projects/Showcase`: an autumn forest clearing built by `Tools/Author.py` through `strada mcp` from Poly Haven assets
  (fetched by `Tools/FetchAssets.py`, not in the repository) with a physics playground, a corner of joints (a wrecking
  ball on a ball joint, a hinged seesaw, a paddle on a motorized hinge), a campfire of particles with a flickering light,
  sound and a test scene that `StradaRuntimeTests` plays (its checks do not need the downloads).
- CI: Windows (MSVC), Ubuntu 24.04 (GCC + Clang), macOS (Apple Clang, arm64); format check; tests on every push.
