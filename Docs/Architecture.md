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
| Mesh import | cgltf (glTF 2.0/GLB), ufbx (FBX), tinyobjloader (OBJ/MTL) |
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
| Asset | Asset handles, registry, metadata, manager (editor file-based / runtime), importers | `AssetHandle`, `AssetManager`, `AssetRegistry`, `*Importer` |
| Renderer | GPU resources (mesh, texture, material, environment), scene renderer passes, debug lines, text, sprites, picking | `SceneRenderer`, `Mesh`, `MeshSource`, `Texture2D`, `TextureCube`, `Material`, `Environment`, `Font` |
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
headless mode reads it back for screenshots. It never depends on the ECS: `Scene` gathers and submits.

Frame passes, in order:

1. **Shadow pass** — directional light cascaded shadow maps (up to 4 cascades, `D32_FLOAT` texture array, stable
   cascades with texel snapping), plus spot-light shadow maps (atlas) for shadow-casting spot lights.
2. **Depth/normal prepass** — depth (`D32_FLOAT`, reversed-Z), view-space normals (`RGBA16_FLOAT` or octahedral
   `RG16_FLOAT`), and in the editor the entity-ID target (`R32_UINT`) for picking.
3. **SSAO** — GTAO (horizon-based, cosine-weighted) at full or half resolution + edge-aware spatial denoise;
   applied to indirect (IBL/ambient) lighting only, with multi-bounce approximation.
4. **Opaque forward PBR** — metallic-roughness GGX (height-correlated Smith visibility, Schlick Fresnel,
   multi-scatter energy compensation), directional/point/spot lights (physical units with smooth range window),
   IBL (irradiance SH or cube + GGX-prefiltered specular + split-sum BRDF LUT), soft shadows (PCF with rotated
   Vogel disk and PCSS blocker search), alpha-mask support. Lights live in a structured buffer (no culling
   initially; capped at 256 visible lights).
5. **Sky** — environment cubemap with rotation, intensity and blur (prefiltered mip).
6. **Transparent forward** — alpha-blended materials sorted back to front.
7. **Bloom** — 13-tap downsample with Karis average on the first mip + tent upsample chain.
8. **Tonemap** — exposure (EV100), operators: ACES (Hill fit), AgX, Khronos PBR Neutral, Reinhard; dithering;
   sRGB encoding in the shader; output `RGBA8_UNORM`.
9. **FXAA** (optional).
10. **Overlays** — world-space text and sprites are lit/unlit in the HDR passes; screen-space text and sprites,
    debug lines (collider visualization, script `Debug.DrawLine`), editor grid, selection outline (from the ID
    buffer) and light/camera icons are drawn after tonemapping.

`SceneRendererSettings` (shadows, SSAO, bloom, exposure, tonemapper, FXAA, sky) are stored per scene, editable in the
editor and through automation.

### 6.3 Assets used by the renderer

- `MeshSource`: imported model (vertices: position, normal, tangent (xyz + handedness), uv0; 32-bit indices;
  submeshes with material index and AABB; embedded material descriptions and textures).
- `Mesh` component data references a mesh asset and optional per-submesh material overrides.
- `Material` (`.smat`): base color, metallic, roughness, emissive color + intensity, normal strength,
  textures (base color, normal, metallic-roughness (glTF packing G = roughness, B = metallic), occlusion, emissive),
  alpha mode (opaque/mask/blend) + cutoff, double-sided, UV tiling/offset.
- `Texture2D`: decoded with stb_image, mip chain generated on the CPU in linear space at load.
- `Environment`: equirectangular `.hdr` → cubemap → irradiance + prefiltered specular (compute shaders).
- Built-in assets with reserved handles: meshes (Cube, Sphere, Plane, Cylinder, Capsule, Cone, Quad), default
  material, white/black/flat-normal textures, default font. JSON may reference them as `"builtin://Cube"`.

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
| `Mesh` | `MeshComponent` | `Mesh` asset, `Materials` asset[] (per-submesh overrides, 0 = mesh default), `CastShadows`, `Visible` |
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

Asset registry (`Assets/AssetRegistry.sreg`): handle → { type, path }. The editor scans `Assets/` on project open,
registers new files with fresh handles, and flags missing files.

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
- Viewport: editor camera (fly/orbit/pan/zoom/focus), ImGuizmo translate/rotate/scale (local/world, snapping),
  picking, selection outline, grid, icons, collider visualization, asset drag-and-drop.
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
  random token. The editor writes `{pid, port, token, project, version}` to an instance file in the user data
  directory (user-only permissions).
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
