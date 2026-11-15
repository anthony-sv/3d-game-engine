---
name: strada-render-pass
description: How to add a shader, render pass or GPU resource to the Strada scene renderer (HLSL conventions, interop structs, strada_add_shaders registration, NVRHI bindings, resource caching, golden-image tests). Use for any renderer change.
---

# Adding a render pass or shader

Read `Docs/Architecture.md` §5–6 and `Strada/Source/Strada/Renderer/SceneRenderer.cpp` first.

## Shaders

- File `Strada/Shaders/<Technique>.hlsl`; shared code in `Shaders/Include/*.hlsli`; structures shared with C++ in
  `Shaders/Include/RendererInterop.h` (16-byte rows: `float3` + scalar). Add `static_assert`s for their size in the C++
  file that uses them.
- Register every entry point in `strada_add_shaders` (`Strada/CMakeLists.txt`) as `Name:File.hlsl:Entry:stage`, and new
  include files under `INCLUDES` so dependent shaders recompile. Load with `ShaderLibrary::Get("Name")`.
- D3D clip conventions (NVRHI flips the viewport): NDC +Y up, depth 0..1 with reversed Z (clear 0, `GreaterOrEqual`),
  UV origin top-left, counter-clockwise front faces (`frontCounterClockwise = true`).
- Register spaces are descriptor sets (`registerSpaceIsDescriptorSet = true`); push constants use `[[vk::push_constant]]`
  and must stay within 128 bytes.

## Recording

- Never create or upload GPU resources while a command list is open: NVRHI allows one open immediate command list, and
  `Renderer` caches upload through their own. Resolve resources first (see `SceneRenderer::ResolveFrameResources`,
  which resolves each distinct mesh and material of the frame once).
- Meshes are instances (`ShaderInterop::InstanceData` in one structured buffer per renderer, Docs/Architecture.md §6.2
  "Instancing"): a mesh draw is a `MeshBatch` of every instance of one pipeline, GPU mesh, submesh and material binding
  set, and its vertex shader reads `g_Instances[g_InstanceIndices[InstanceOffset + SV_InstanceID]]` with
  `startInstanceLocation = 0`. A pass that draws meshes gets its batches from `BuildBatches` (`Renderer.Batch`) and
  records them with `DrawBatches`; a new pass puts no matrix per object in push constants (the shadow pass still draws
  one caster per draw with its matrix there, until its views are batched). Blended items stay one instance per batch,
  back to front.
- The frame's items keep shadow casters outside the camera's view (`CullToView`): passes that render from the camera
  skip items whose `InView` is false; passes that render from somewhere else (shadow views) cull against their own
  volume.
- Per-frame data goes in volatile constant buffers or buffers written at the start of the frame's command list.
- Keep render targets as members so clears (which NVRHI does not track) never release a texture in use.
- Record each pass inside a `SceneRenderer::PassScope pass(*this, commandList, "<Pass>")`: it opens an NVRHI marker
  (RenderDoc captures read by pass name, in every build) and, when the frame's passes are timed, the pass's GPU timer;
  both end with the scope. The pass's time then appears in the Profiler panel, `profiler.status` and
  `StradaRuntime --profile` as `<view>/<Pass>` (at most `MaxTimedPasses` passes a frame). CPU work of the renderer that
  runs every frame gets a `Renderer.<What>` zone (`ST_PROFILE_SCOPE`, Docs/Architecture.md §4.4).

## Tests

- CPU logic: plain doctest cases.
- GPU: `ST_REQUIRE_GPU()` + `Testing::RenderTestScope`, render offscreen, `ReadbackTexture`, then
  `Testing::CheckGoldenImage("<Name>", image)`. Generate or update references deliberately with
  `STRADA_UPDATE_GOLDEN=1`, inspect the PNG in `Tests/Data/Golden/`, and commit it. Validation must stay silent.
- Look at the result in the editor too: `StradaEditor --frames 90 --screenshot out.png`.
