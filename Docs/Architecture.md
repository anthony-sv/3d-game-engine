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
| Audio | miniaudio |
| Scripting | C# on .NET 10, hosted through hostfxr/nethost |
| Editor UI | Dear ImGui (docking branch, docking + multi-viewports) + ImGuizmo |
| Mesh import | cgltf (glTF 2.0/GLB), ufbx (FBX, OBJ/MTL), MikkTSpace (tangents) |
| Images / fonts | stb_image, stb_image_write, stb_truetype |
| Serialization | nlohmann/json (`ordered_json` for files) |
| Logging | spdlog |
| Processes | reproc++ (script builds, editor launching) |
| File dialogs | nativefiledialog-extended |
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
│   └── Resources/                                    Files embedded into the engine binary (default font, ...)
├── StradaEditor/                                     Editor: StradaEditorCore static lib + StradaEditor executable
│   ├── Source/Editor/...                             Panels, automation, commands (undo/redo), export, project management
│   └── Resources/                                    Editor fonts, icons, project templates (copied next to the binary)
├── StradaRuntime/                                    Runtime/player executable used for exported games
├── Strada-ScriptCore/                                C# scripting API (assembly Strada.ScriptCore.dll)
├── StradaTool/                                       C# CLI + MCP stdio bridge (executable `strada`)
├── Tests/
│   ├── StradaTests/                                  C++ unit tests (doctest) for the engine library
│   ├── StradaEditorTests/                            C++ tests for editor automation commands (headless)
│   ├── ScriptCoreTests/                              C# unit tests (math, API coverage checks)
│   ├── TestScripts/                                  C# script assembly used by C++ scripting tests
│   └── Data/                                         Test assets and golden images
├── Projects/FeatureTest/                             Project whose scene exercises every feature and the whole script API
├── Tools/                                            Developer scripts (formatting, asset download, code generation)
├── Docs/                                             Architecture, scripting guide, automation reference, rendering notes
└── .github/workflows/, .claude/skills/, .mcp.json    CI, agent skills, MCP configuration
```

Build output: `build/<preset>/bin/` contains every executable, `Strada.ScriptCore.dll`, `strada` tool, and the
copied editor `Resources/`.

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
| Core | Base macros, logging, asserts, UUID, time, buffers, events, input, window, application loop, layers, file system, threading helpers | `Application`, `Layer`, `Window`, `Input`, `Log`, `UUID`, `Timestep`, `Buffer`, `FileSystem`, `Result<T>` |
| Math | Transform compose/decompose, AABB, frustum, ray, intersection | `Math::DecomposeTransform`, `AABB`, `Frustum`, `Ray` |
| Serialization | JSON helpers for glm/UUID/enums, versioned file headers | `JsonUtils` |
| Platform | Process spawning (reproc++), file dialogs, OS paths (user data dir), file watcher | `Process`, `FileDialogs`, `FileWatcher`, `Platform` |
| RHI | Vulkan instance/device creation, NVRHI device, swapchains (one per OS window), frame pacing, embedded shader library, common samplers/default textures | `GraphicsDevice`, `Swapchain`, `ShaderLibrary` |
| Asset | Asset handles and metadata, the persistent registry, the asset manager, CPU asset types, model importers, built-in primitives | `AssetHandle`, `AssetManager`, `AssetRegistry`, `MeshSource`, `MaterialAsset`, `TextureAsset`, `EnvironmentAsset`, `MeshImporter` |
| Renderer | GPU resources created from assets (meshes, textures, materials, environments, fonts), scene renderer passes, debug lines, text, sprites, picking | `SceneRenderer`, `Texture2D`, `TextureCube` |
| ImGui | ImGui context/layer, NVRHI renderer backend with multi-viewport support, UI helpers | `ImGuiLayer`, `ImGuiRenderer` |
| Physics | Jolt init/shutdown, per-scene physics world, layers, contact events, queries | `PhysicsSystem`, `PhysicsScene` |
| Audio | miniaudio engine, clips, per-scene sound sources, listener | `AudioEngine`, `AudioClip`, `AudioScene` |
| Script | .NET runtime hosting, managed bridge, native bindings, script instances and fields, hot reload | `ScriptEngine`, `DotNetHost`, `ScriptBindings` |
| Scene | ECS scene, entities, components, component registry/reflection, serialization, prefabs, runtime lifecycle | `Scene`, `Entity`, `ComponentRegistry`, `SceneSerializer`, `Prefab` |
| Project | Project file, paths, settings | `Project`, `ProjectSerializer` |

### 4.1 Subsystem lifetime

Subsystems are static facades with explicit `Init()`/`Shutdown()` (Hazel style), initialized by `Application` in this
order and shut down in reverse: Log → FileSystem/Platform → GraphicsDevice (unless `--no-gpu`) → Renderer →
AssetManager → PhysicsSystem → AudioEngine → ScriptEngine. Every `Init` asserts it is not already initialized and
every `Shutdown` must leave no state behind so tests can init/shutdown repeatedly.

### 4.2 Threading model

- The **main thread** owns the ECS, assets, rendering submission, scripting and editor state.
- Jolt runs its own job threads; contact callbacks only enqueue events (thread-safe queue) drained on the main thread.
- miniaudio runs its own audio thread; only the miniaudio API is touched from the main thread.
- Background work (script builds, exports, file watching, the automation socket) never touches engine state;
  results are marshalled with `Application::SubmitToMainThread(std::function<void()>)`.

### 4.3 Error handling

- No exceptions cross module boundaries. Third-party code that throws (nlohmann::json) is wrapped at the boundary.
- Fallible operations return `Result<T>` (value or error message) or `bool` plus a logged error.
- `ST_CORE_ASSERT`/`ST_ASSERT` are for programmer errors only. User data (scene files, assets, scripts, automation
  requests) must never crash the engine: validate, log, and report the error.

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

Shaders (`Strada/Shaders/*.hlsl`) are compiled by DXC to SPIR-V at build time with flags matching NVRHI's default
Vulkan binding offsets, embedded into the library, and fetched by name through `ShaderLibrary`.

### 6.2 Scene renderer

`SceneRenderer` renders a submitted frame (camera + draw lists + lights + environment + settings) into its own
offscreen targets. The editor shows the final image in the viewport panel; the runtime blits it to the swapchain;
headless mode reads it back for screenshots. It never depends on the ECS: `Scene` gathers and submits
(`Scene/SceneRendering.h`: `RenderScene` with an explicit camera, `RenderSceneFromPrimaryCamera`).

`Renderer` (static facade, initialized with the GraphicsDevice) owns samplers, the material binding layout and GPU
caches keyed by asset object identity (meshes, textures per color space with CPU-generated linear-space mips, material
constants + binding sets refreshed by `MaterialAsset::GetVersion`); scene renderers resolve every resource before
recording because uploads submit their own command lists. Bindings: set 0 = frame constants (volatile CB), light
structured buffer, material sampler, per-draw push constants (model + normal matrix, 128 bytes); set 1 = material.
Structures shared with HLSL live in `Strada/Shaders/Include/RendererInterop.h`.

Implemented so far: forward PBR (opaque front to back, then blended back to front) with image-based lighting
(split-sum specular with Fdez-Aguera multiple-scattering compensation; a uniform ambient color through the same terms
when the sky light has no environment), the sky pass, and the tonemap pass (with dithering and sRGB encoding into
`RGBA8_UNORM`); the HDR target is `RGBA16_FLOAT` with a `D32` reversed-Z depth buffer. Environments are processed on
first use by compute shaders (`EnvironmentProcessor`): equirect to an RGBA16F cubemap (a quarter of the equirect width,
power of two, 16-1024) with box-filtered mips, a 32x32 irradiance cube and a 256x256 GGX-prefiltered cube with 6 mips
(filtered importance sampling, perceptual roughness linear in mip); the 128x128 BRDF table is computed at
`Renderer::Init`. Shadows (pass 1 below) are implemented: the first shadow-casting directional light gets PCSS soft
shadows sized by its `LightSize`, with cascade cross-fades and a fade at the shadow distance; local lights use a
rotated 16-tap PCF. Shadow projection math lives in `Renderer/ShadowMath` (unit tested). The remaining passes below
are added in order without changing that structure.

Frame passes, in order (passes 1-8 are implemented):

1. **Shadow pass** — directional light cascaded shadow maps (up to 4 cascades, `D32_FLOAT` texture array, stable
   cascades: bounding spheres with texel snapping, depth range extended to every caster), plus a local shadow-map
   array (`D32_FLOAT`, up to 24 slices) with one slice per shadow-casting spot light and six per point light (cube
   faces with guard bands, face chosen in the shader). Shadow maps use reversed Z; blended materials do not cast.
2. **Opaque forward PBR** — metallic-roughness GGX (height-correlated Smith visibility, Schlick Fresnel,
   multi-scatter energy compensation), directional/point/spot lights (physical units with smooth range window),
   IBL (cube irradiance + GGX-prefiltered specular + split-sum BRDF LUT), soft shadows (PCF with rotated
   Vogel disk and PCSS blocker search), alpha-mask support. Lights live in a structured buffer (no culling
   initially; capped at 256 visible lights). Besides the HDR color it writes, as extra render targets, the
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
7. **Tonemap** — exposure (EV100), operators: ACES (Hill fit), AgX, Khronos PBR Neutral, Reinhard; dithering;
   sRGB encoding in the shader; output `RGBA8_UNORM`.
8. **FXAA** (optional) — luma edge search on the tonemapped image.
9. **Overlays** — world-space text and sprites are lit/unlit in the HDR passes. On the tonemapped image (before FXAA
    when it is enabled): the editor ground grid (analytic ray/plane intersection on y = 0, anti-aliased minor and
    major lines and colored axes, faded with distance, hidden behind surfaces with a small depth tolerance so ground
    planes do not z-fight) and debug lines (depth-tested through the scene depth attached read-only, or drawn on
    top; collider visualization, script `Debug.DrawLine`). After FXAA: the selection outline, an anti-aliased band
    around the visible silhouette of selected meshes read from the ID target. Screen-space text and sprites follow
    the overlays. All overlay colors are sRGB-encoded with straight alpha.

Render targets are recreated on resize and reconfiguration only after the GPU has drained: NVRHI does not track
clears, which can be a target's only use in a frame.

`SceneRendererSettings` (shadows, SSAO, bloom, exposure, tonemapper, FXAA, sky) are stored per scene, editable in the
editor and through automation.

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
  with warnings.
- `MaterialAsset` (`.smat`): metallic-roughness parameters — `BaseColor`, `Metallic`, `Roughness`, `EmissiveColor` +
  `EmissiveIntensity`, `NormalStrength`, `OcclusionStrength`, textures (base color, normal, metallic-roughness with glTF
  packing G = roughness and B = metallic, occlusion, emissive), `AlphaMode` (Opaque/Mask/Blend) + `AlphaCutoff`,
  `DoubleSided`, `UVTiling`, `UVOffset`. A version counter tells renderers when parameters changed.
- `TextureAsset` (`.png`, `.jpg`, `.tga`, `.bmp`): keeps the encoded file and decodes on demand. The color space is
  chosen by the sampling material slot (base color and emissive sRGB, the others linear); the renderer generates mips
  at upload in linear space.
- `EnvironmentAsset` (`.hdr`): equirectangular Radiance HDR (e.g. Poly Haven HDRIs) → cubemap + irradiance +
  prefiltered specular (compute shaders).
- `FontAsset` (`.ttf`, `.otf`), `AudioClipAsset` (`.wav`, `.flac`, `.mp3`, `.ogg`; format detected from the data),
  `PrefabAsset` (`.sprefab`). Scenes (`.sscene`) are registered for references but opened with `SceneSerializer`.
- Built-in assets: meshes `Cube`, `Sphere`, `Plane`, `Cylinder`, `Capsule`, `Cone`, `Quad` (unit sizes matching the
  default colliders), `DefaultMaterial`, textures `White`, `Black`, `FlatNormal`, and the procedural `DefaultSky`
  environment (zenith/horizon/ground gradient). Later subsystems may register more
  (e.g. the default font) with reserved handles.

## 7. Scene and ECS

### 7.1 Scene

`Scene` owns an `entt::registry`, a `UUID → entt::entity` map, scene settings (`SceneRendererSettings`, physics
gravity) and the runtime subsystems while playing (`PhysicsScene`, `AudioScene`, script instances).

Lifecycle: `OnRuntimeStart/Stop` (physics + scripts + audio), `OnSimulationStart/Stop` (physics only),
`OnUpdateRuntime(ts)`, `OnUpdateSimulation(ts, camera)`, `OnUpdateEditor(ts, camera)`, `OnViewportResize(w, h)`.
Play mode runs on a deep copy (`Scene::Copy`); stopping restores the editor scene untouched.

Runtime update order per frame: scripts `OnUpdate` → physics fixed steps (accumulator, default 60 Hz; scripts'
`OnFixedUpdate` before each step) → contact/trigger events dispatched to scripts → transform sync → audio update →
deferred entity destruction → render submission. Entity destruction requested during iteration is **deferred** to the
end of the frame; creation is immediate.

### 7.2 Components (`Strada/Scene/Components.h`)

Plain data structs, copyable, with PascalCase public fields. Light intensities are artist-friendly multipliers
(the renderer's exposure defaults suit them), not photometric units. Each component's serialized name and field list
are declared once in `ComponentTraits.h`; generic code derives JSON serialization (strict, typed, transactional
partial updates with readable errors), schemas, copies and the `ComponentRegistry` entry from that table. The
registry provides type-erased add/has/remove/serialize/deserialize/copy/describe functions. The registry is the single source of truth for scene
serialization, scene copy, automation (`component.*` commands), the inspector "Add Component" menu, and script
interop component lookups.

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
| `AudioSource` | `AudioSourceComponent` | `Clip` asset, `Volume`, `Pitch`, `Loop`, `PlayOnStart`, `Spatial`, `MinDistance`, `MaxDistance` |
| `AudioListener` | `AudioListenerComponent` | `Active` |
| `Script` | `ScriptComponent` | `ClassName`, `Fields` (name → typed value) |
| `Text` | `TextComponent` | `Text`, `Font` asset (0 = default), `Color`, `FontSize`, `ScreenSpace`, `Alignment` (Left/Center/Right), `LineSpacing` |
| `SpriteRenderer` | `SpriteRendererComponent` | `Color`, `Texture` asset, `Tiling`, `ScreenSpace` |
| `Prefab` | `PrefabComponent` | `Prefab` asset, `SourceEntity` UUID |

Screen-space `Text`/`SpriteRenderer` use the entity translation in normalized viewport coordinates
(x, y ∈ [0, 1], origin top-left) and scale in pixels-per-unit, so games can build HUDs without a UI framework.

Colliders attach to the entity's own rigid body; multiple colliders on one entity form a compound shape. An
entity with colliders and no `RigidBody` is a static body. Entity scale is applied to shapes.

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

Project (`.sproj`): name, asset directory, script module path, start scene (path relative to Assets), window
settings (title, width, height, fullscreen, vsync, resizable), physics settings (fixed timestep, named layers
(max 16) and collision matrix).

Asset registry (`Assets/AssetRegistry.sreg`): `"Assets": [ { "Handle", "Type", "Path" } ]` sorted by path, paths
relative to `Assets/` with forward slashes (case-sensitive, portable characters only). The asset directory is scanned
when a project opens: new files get fresh handles, missing files keep theirs. Hidden files and folders (names starting
with `.`) are ignored. Invalid entries are skipped with warnings; an unreadable registry refuses to open (continuing
would break references).

Material (`.smat`): `"Material": { <MaterialAsset fields> }`; missing fields keep their defaults.

## 8. Physics (Jolt)

`PhysicsSystem` (global) initializes Jolt (allocator, factory, types, job system). `PhysicsScene` (per playing scene)
owns the `JPH::PhysicsSystem`, layer interfaces built from project layer settings (object layers = project layers;
broad-phase layers: static and moving), and a contact listener that queues events. Bodies are created from
components at runtime start and when components/entities are added at runtime; destroyed on removal. Kinematic
bodies follow their transform via `MoveKinematic`; scripts setting the transform of a dynamic/static body teleport it.
Physics transforms are written back to `TransformComponent` (converted to parent-local space) after each step.
Events delivered to scripts: `OnCollisionEnter/Exit(Entity other)`, `OnTriggerEnter/Exit(Entity other)`.
Queries: raycast (closest hit), with layer mask.

## 9. Audio (miniaudio)

`AudioEngine` wraps `ma_engine` (falls back to the null backend when no device is available or in tests).
`AudioClip` assets reference WAV/FLAC/MP3/OGG files. `AudioScene` creates an `ma_sound` per `AudioSourceComponent`
at runtime start, updates spatial positions from transforms each frame, and positions the listener from the active
`AudioListenerComponent` (or the primary camera).

## 10. Scripting (C# / .NET 10)

### 10.1 Hosting

`DotNetHost` locates `hostfxr` via nethost (respecting an app-local `dotnet/` directory for exported games),
initializes the runtime from `Strada.ScriptCore.runtimeconfig.json`, loads `Strada.ScriptCore.dll` into the default
load context, and calls `[UnmanagedCallersOnly]` entry points. Game assemblies load into a collectible
`AssemblyLoadContext` that resolves `Strada.ScriptCore` to the default-context instance, loaded from bytes so the
DLL is never locked (hot reload). Managed exceptions never cross into native code: every entry point catches,
logs (with stack trace) and returns an error code.

### 10.2 Bindings

Native functions are exposed **by name**: the native side provides a `{name, function pointer}` table; the managed
`InternalCalls` class declares `delegate* unmanaged<...>` static fields whose names match, assigned by reflection at
startup — a missing or extra binding is a fatal startup error listing the names. Blittable types only across the
boundary (`byte` for bool, UTF-8 `byte*` + length for strings). glm/C# layouts match: `Vector2/3/4` = floats,
`Quaternion` = (X, Y, Z, W), `Matrix4` = 16 floats column-major.

### 10.3 Script lifecycle

Scripts derive from `Strada.Script` (which derives from `Entity`) and override any of: `OnCreate()`,
`OnUpdate(float deltaTime)`, `OnFixedUpdate(float fixedDeltaTime)`, `OnDestroy()`, `OnCollisionEnter(Entity other)`,
`OnCollisionExit(Entity other)`, `OnTriggerEnter(Entity other)`, `OnTriggerExit(Entity other)`.
Public fields (and private fields marked `[SerializeField]`) of supported types are editable in the inspector and
serialized in the scene: `bool`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `string`, `Vector2`, `Vector3`,
`Vector4`, `Quaternion`, `Color`, `Entity`, `Prefab`, `AssetHandle`, and enums.

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
  `AudioSourceComponent` (`Play()`, `Stop()`, `Pause()`, `IsPlaying`, `Volume`, `Pitch`, `Loop`, `Clip`),
  `AudioListenerComponent`, `TextComponent`, `SpriteRendererComponent`, `ScriptComponent` (`ClassName`, `Instance`).
- `Material` (runtime-creatable): `Create()`, `Clone()`, `BaseColor`, `Metallic`, `Roughness`, `Emissive`,
  `EmissiveIntensity`, textures.
- `Input`: `IsKeyDown/Pressed/Released(KeyCode)`, `IsMouseButtonDown/Pressed/Released(MouseButton)`,
  `MousePosition`, `MouseDelta`, `MouseScrollDelta`, `CursorMode`, gamepad (`IsGamepadConnected`,
  `GetGamepadAxis`, `IsGamepadButtonDown/Pressed`).
- `Time`: `DeltaTime`, `FixedDeltaTime`, `Elapsed`, `FrameCount`, `TimeScale`.
- `Physics`: `Gravity`, `Raycast(origin, direction, maxDistance, out RaycastHit, layerMask)`.
- `Log`: `Trace/Info/Warn/Error`. `Debug`: `DrawLine(from, to, color, duration)`.
- `SceneManager`: `LoadScene(path)`, `CurrentSceneName`. `Application`: `Quit()`, `WindowWidth/Height`, `IsEditor`.
- `Assets`: `Load<T>(path)` for `Prefab`, `Material`, `Mesh`, `AudioClip`, `Texture`.
- Math: `Vector2/3/4`, `Quaternion`, `Matrix4`, `Color`, `Mathf`, `Random`.
- Attributes: `[SerializeField]`, `[HideInInspector]`, `[Range(min, max)]`, `[Tooltip(text)]`.
- `Strada.Testing`: `Assert`, `TestReporter` (used by the feature-test project and CI).

## 11. Editor (`StradaEditor`)

- ImGui with docking and multi-viewports; panels: Scene Hierarchy, Inspector, Content Browser, Viewport,
  Console, Renderer Settings, Statistics, Project Settings; menu bar and play toolbar.
- Viewport: editor camera (fly/orbit/pan/zoom/focus), ImGuizmo translate/rotate/scale (local/world, snapping;
  W/E/R, X, Ctrl toggles snapping while dragging) applied to every selected root around the primary selection as
  one undo step per drag (`SetComponentsCommand` with a merge key), click picking through the entity-ID pass (Ctrl
  toggles, Shift adds, empty space clears; picking IDs are never reused within a scene), selection outline, ground
  grid (G), icons for cameras and lights (clickable), shapes of selected cameras (frustums), lights (directions,
  ranges, cones) and colliders, asset drag-and-drop.
- Undo/redo for every scene modification through a command history (JSON before/after snapshots). Automation
  commands use the same history.
- Play (scripts + physics + audio), Simulate (physics only), Pause, Step, Stop.
- Project management (new from template, open, recent), C# script project generation and build (`dotnet build`)
  with diagnostics, hot reload of the game assembly.
- Export (Build Game) for the host platform.
- Headless mode (`--headless`): no window/ImGui; offscreen rendering if a GPU is available; used by automation and CI.

## 12. AI automation

- `CommandRegistry` (editor): named commands with a description, JSON-schema parameters and a handler returning a
  JSON result or a structured error. Long operations complete asynchronously. All commands execute on the main
  thread; the active project/scene state is identical whether a human or an agent drives the editor.
- `AutomationServer`: JSON-RPC 2.0 over TCP bound to `127.0.0.1`, newline-delimited messages, authenticated by a
  random 256-bit token (first request `authenticate`, constant-time comparison). A network thread owns the sockets
  (framing, size limits, parsing, authentication) and never touches editor state; the editor layer runs queued requests
  on the main thread once per frame (`ProcessRequests`). The editor writes `{pid, port, token, project, version}` to
  `<user data>/Editor/Instances/<pid>.json` (user-only permissions) and removes it on exit.
- The editor model (`EditorContext`: scene, file, undo `CommandHistory`, selection; `EditorOperations`) is shared by UI
  panels and automation. The protocol and the command reference are in [Automation.md](Automation.md).
- `strada` tool (C#): `strada mcp` (MCP stdio server; tools are the editor commands, names `domain_action`;
  launches or attaches to an editor on demand; screenshots returned as image content), `strada call <command>
  [json]`, `strada launch`, `strada commands`.
- Command domains: `editor.*` (status, undo, redo, commands), `project.*` (create, open, save, settings, export),
  `scene.*` (new, open, save, hierarchy, settings, dump), `entity.*` (create with components, delete, duplicate,
  rename, reparent, find, get, select), `component.*` (types, add, remove, get, set with partial JSON patch),
  `asset.*` (list, import, refresh, get, delete, move), `material.*`, `prefab.*`, `script.*` (create from template,
  build with diagnostics, classes and fields), `play.*` (start, stop, pause, step, advance N frames, state),
  `input.*` (inject keys/mouse during play), `viewport.*` (screenshot, camera, frame entity), `renderer.*`
  (settings), `log.*` (read since index), `test.*` (run a test scene and return results).
- Claude Code integration: `.mcp.json` registers `strada mcp`; skills in `.claude/skills/` document building games.

## 13. Runtime and export

`StradaRuntime` loads `Game.sgame` (JSON: name, start scene, window settings, version) found next to the executable
(or in `Contents/Resources` on macOS), then runs the start scene with scripts, physics and audio. No editor code.

Export produces, for the host platform:

```
<Out>/<GameName>[.exe | .app]      Renamed StradaRuntime (macOS: app bundle with MoltenVK + Vulkan loader)
<Out>/Game.sgame                   Game configuration
<Out>/Assets/                      Project assets + AssetRegistry.sreg
<Out>/Scripts/                     Strada.ScriptCore.dll (+ runtimeconfig), game assembly (Release build)
<Out>/dotnet/                      Optional app-local .NET runtime
<Out>/ThirdPartyNotices.md
```

Dist builds of the runtime have no console window on Windows, log to a file in the user data directory, and
compile out asserts and dev tools.

## 14. Build configurations

| Config | Optimization | Asserts | Logging | Vulkan validation |
|--------|--------------|---------|---------|-------------------|
| Debug | off | on | all | on if available |
| Release | on (+ debug info) | on | all | opt-in (`--validation`) |
| Dist | on | off | warnings+ to file | off |

## 15. Testing

- `StradaTests` (doctest): every module; GPU tests create a headless device and skip cleanly when no Vulkan
  device exists; renderer golden-image tests compare against `Tests/Data/Golden` with a tolerance.
- `StradaEditorTests`: automation commands executed headlessly on a temporary project.
- `ScriptCoreTests` (xUnit): math types and an API-coverage test asserting every public `Strada.ScriptCore`
  member is referenced by the FeatureTest scripts.
- `Projects/FeatureTest`: a scene that uses every component and scripts that call the entire scripting API through
  `Strada.Testing`; run headlessly in CI (`StradaRuntime --project ... --test`), exit code = failures.
- CI: Windows (MSVC), Ubuntu 24.04 (GCC + Clang), macOS (Apple Clang, arm64); format check; tests on every push.
