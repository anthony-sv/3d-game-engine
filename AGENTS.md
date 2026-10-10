# AGENTS.md — Strada Engine development guide

Strada is a production-grade C++20 3D game engine (Windows, macOS, Linux) with a C# (.NET 10) scripting layer, an
ImGui editor that AI agents can fully control, and an exporter for distributable games. This file is the rulebook for
everyone — humans and AI agents — working in this repository. Read [Docs/Architecture.md](Docs/Architecture.md)
before changing anything structural; it is the architectural contract.

**Golden rules**

1. Production quality only: no hacks, no shortcuts, no "temporary" code, no TODO-driven features. If something
   cannot be done properly now, leave it out and say so.
2. Every behavior change ships with tests, and every test passes before a commit.
3. Follow the style guide below exactly (Hazel naming, Allman braces, east const, tabs).
4. Code review happens **before** every commit (see [Commit process](#commit-process)).
5. Never look outside this repository for reference code. Third-party dependencies are fetched by CMake.

## Repository map

| Path | What |
|------|------|
| `Strada/` | Engine static library (`Source/Strada/<Module>`, `Shaders/`, `ThirdParty/`) |
| `StradaEditor/` | `StradaEditorCore` static library + `StradaEditor` executable |
| `StradaRuntime/` | Game player (`StradaRuntime`): runs exported games and projects, windowed, headless or as test runs |
| `Strada-ScriptCore/` | C# scripting API (`Strada.ScriptCore.dll`) |
| `StradaTool/` | `strada`: the editor's command-line tool and MCP stdio bridge (C#) |
| `Tests/StradaTests/` | C++ engine tests (doctest) |
| `Tests/StradaEditorTests/` | Editor model and automation tests (doctest, headless) |
| `Tests/StradaRuntimeTests/` | Game player tests: run `StradaRuntime` on generated games (doctest) |
| `Tests/TestScripts/` | C# game scripts the scripting tests run |
| `Tests/ScriptCoreTests/` | C# unit tests of the scripting API (xUnit v3) |
| `Tests/StradaToolTests/` | Tests of `strada`: command line, MCP server, sessions with the headless editor (xUnit v3) |
| `Tests/Data/` | Test data (renderer golden images) |
| `Projects/FeatureTest/` | Project exercising every component and the entire scripting API |
| `Projects/Blocks/` | Sample game (falling blocks) made through the agent tools, with a test scene |
| `cmake/` | Build modules: options, compiler settings, dependencies |
| `Tools/` | `build.py` (configure/build/test), `format.py` (clang-format and dotnet format) |
| `Docs/` | `Architecture.md` (the contract), `Automation.md` (automation protocol and command reference), `Scripting.md` (guide to writing game scripts) |
| `.claude/skills/` | Task-specific playbooks (see [Skills](#skills)) |

## Building

Prerequisites: CMake ≥ 3.28, Ninja, a C++20 compiler (MSVC 19.40+/VS 2022 17.10+ or VS 2026, GCC 13+, Clang 17+,
Apple Clang 15+), the Vulkan SDK 1.4.x (`VULKAN_SDK` set), the .NET 10 SDK, Python 3.10+ (tooling only).
Linux additionally needs the X11/Wayland/GTK development packages listed in `README.md`.

```sh
python Tools/build.py                    # configure + build the default preset for this OS (Debug)
python Tools/build.py --config release   # Release
python Tools/build.py --test             # build and run every test suite
cmake --preset windows-debug && cmake --build --preset windows-debug   # plain CMake (needs a VS dev shell on Windows)
```

`Tools/build.py` locates the Visual Studio environment on Windows automatically. Presets: `windows-{debug,release,dist}`,
`linux-gcc-{debug,release,dist}`, `linux-clang-{debug,release}`, `macos-{debug,release,dist}`. Output goes to
`build/<preset>/bin/`. Limit parallelism with `--jobs N` when several builds run at once.

## C++ style guide

### Naming (Hazel conventions)

| Element | Convention | Example |
|---------|-----------|---------|
| Classes, structs, enums, unions, type aliases, concepts | PascalCase | `SceneRenderer`, `AssetHandle` |
| Functions and member functions | PascalCase | `CreateEntity()`, `GetComponent<T>()` |
| Namespaces | PascalCase | `Strada`, `Strada::Math`, `Strada::Utils` |
| Local variables and parameters | camelCase | `deltaTime`, `entityCount` |
| Private/protected data members | `m_` + PascalCase | `m_Registry`, `m_IsRunning` |
| Static variables (class, file or function scope) | `s_` + PascalCase | `s_Instance`, `s_Data` |
| Global variables (avoid) | `g_` + PascalCase | `g_Allocator` |
| Public data members of plain structs (components, descs, specifications) | PascalCase, no prefix | `Translation`, `Width` |
| Compile-time constants (`constexpr`, `static constexpr`) | PascalCase | `MaxFramesInFlight` |
| Enumerators (`enum class` only) | PascalCase | `ProjectionType::Perspective` |
| Macros | `ST_` + UPPER_SNAKE_CASE | `ST_CORE_ASSERT`, `ST_PLATFORM_WINDOWS` |
| Template parameters | `T`, or `T` + PascalCase; packs `Args` | `typename TComponent`, `typename... Args` |
| Files and folders | PascalCase, named after the main type | `SceneRenderer.h`, `Renderer/` (exception: `stpch.h`) |

Getters/setters are `GetX()`/`SetX()`; boolean queries are `IsX()`/`HasX()`/`CanX()`.

### Formatting

Enforced by `.clang-format` (clang-format 22). Run `python Tools/format.py` before committing; CI rejects unformatted
code.

- **Allman braces** for everything, including namespaces, classes, functions, control statements and lambdas.
- **Tabs** for indentation (4 columns), spaces for alignment. Column limit 140.
- **East const**: `std::string const& name`, `char const* text`, `auto const& entity`, `glm::mat4 const& transform`.
  `constexpr`, `static` and `inline` stay on the left (`static constexpr uint32_t MaxLights = 256;`).
- Pointer and reference symbols bind to the type: `Entity* entity`, `Scene& scene`.
- Namespace contents are indented. No `// namespace` closing comments.
- `#pragma once` in every header.
- Include order (one block, sorted by clang-format): `stpch.h` (in `.cpp` files of the engine), the matching header,
  other `"Strada/..."` headers, third-party headers, standard headers.

```cpp
#pragma once

#include "Strada/Core/Base.h"

#include <string>

namespace Strada
{
	struct WindowSpecification
	{
		std::string Title = "Strada";
		uint32_t Width = 1600;
		uint32_t Height = 900;
		bool VSync = true;
	};

	class Window
	{
	public:
		explicit Window(WindowSpecification const& specification);
		~Window();

		uint32_t GetWidth() const { return m_Specification.Width; }
		bool IsMinimized() const { return m_IsMinimized; }

		void SetTitle(std::string const& title);

	private:
		WindowSpecification m_Specification;
		bool m_IsMinimized = false;

		static uint32_t s_WindowCount;
	};
}
```

### Language rules

- C++20. Prefer the standard library; no compiler extensions; code must compile warning-free on MSVC, GCC and Clang
  (`/W4 /WX`, `-Wall -Wextra -Wpedantic -Werror` on our targets).
- RAII everywhere. No owning raw pointers; no naked `new`/`delete` (except where a third-party API demands it, wrapped
  immediately). `Ref<T>` (`std::shared_ptr`) for shared ownership, `Scope<T>` (`std::unique_ptr`) for unique.
- `enum class` with explicit underlying types for anything serialized or passed across the script boundary.
- Fixed-width integers (`uint32_t`, `uint64_t`) for sizes, IDs and serialized data.
- `override` on every override; `final` where a class is not designed for inheritance; `explicit` single-argument
  constructors; `[[nodiscard]]` on functions whose result must be checked (`Result<T>`, factory functions).
- Pass small trivially-copyable values (`glm::vec3`, `UUID`, handles, `Timestep`) by value; everything else by
  `T const&`; sinks by value + `std::move`.
- `auto` only when the type is obvious from the right-hand side or unspellable; never to hide a primitive type.
- No `using namespace` in headers. No global mutable state outside the subsystem `s_Data` pattern.
- Engine code does not throw. Wrap throwing third-party calls at the boundary. Fallible operations return
  `Result<T>` or `bool` + log. `ST_CORE_ASSERT` is for programmer errors only; user data must never crash the engine.
- Logging only through `ST_CORE_*` (engine) and `ST_*` (editor/runtime) macros — never `std::cout`/`printf`. The only
  exception is command-line usage output (`--help`, argument errors), which goes to stdout/stderr directly.
- Keep third-party headers out of public engine headers when practical (Jolt, Vulkan, miniaudio, hostfxr, cgltf,
  ufbx live in `.cpp` files). NVRHI handle types are allowed in Renderer/RHI headers.
- Document thread affinity of public functions when not main-thread-only.
- Comments explain *why*, not *what*. Public API headers get brief doc comments where intent is not obvious.

### HLSL

- Files: `Strada/Shaders/<Technique>.hlsl`, shared code in `Strada/Shaders/Include/*.hlsli`. Register every entry
  point in the `strada_add_shaders` list in `Strada/CMakeLists.txt`; `ShaderLibrary::Get("<Name>")` loads it.
- Formatted by hand with the C++ rules (Allman, tabs): clang-format cannot format HLSL semantics.
- Clip space follows D3D conventions because NVRHI flips the Vulkan viewport: NDC +Y up, depth 0..1, UV (0, 0) top-left.
- Constant buffers and push constants use `-fvk-use-dx-layout` packing; matrices are column-major (`mul(matrix, vector)`).
- Entry points `VSMain`, `PSMain`, `CSMain`; functions PascalCase, locals camelCase, constant buffers
  `cbuffer <Name>Constants`.
- Structures shared with C++ live in `Strada/Shaders/Include/*.h` (compiled as both HLSL and C++ through
  `ShaderInterop.h`) and are `static_assert`-checked for size/alignment on the C++ side.

### CMake

- Target-based only: no `include_directories`, `add_definitions` or global flag edits. Warnings and options are
  applied with `strada_configure_target(<target>)` from `cmake/StradaCompilerSettings.cmake` — never to third-party
  targets.
- Every dependency is pinned (tag + archive SHA256 or commit) in `cmake/StradaDependencies.cmake` and listed in
  `ThirdPartyNotices.md`.
- Sources are listed explicitly per target (no globbing) so reviews see every file.

## C# style guide

- .NET 10, C# latest, `<Nullable>enable</Nullable>`, `<TreatWarningsAsErrors>true</TreatWarningsAsErrors>`,
  `<AllowUnsafeBlocks>true</AllowUnsafeBlocks>` only in projects that need it.
- File-scoped namespaces, Allman braces, tabs. PascalCase for types/methods/properties/public fields/constants,
  camelCase for locals/parameters, `m_` + PascalCase private fields, `s_` + PascalCase private static fields
  (enforced by `.editorconfig`; `dotnet format --verify-no-changes` runs in CI).
- The scripting API (`Strada-ScriptCore`) is user-facing: every public member has XML documentation.
- Exceptions never cross into native code: every `[UnmanagedCallersOnly]` method catches everything and reports
  through the native log.

## Architecture rules (summary)

- Module layering is in `Docs/Architecture.md` §4; never include "upwards" (e.g. Renderer must not include Scene).
- The main thread owns engine state. Background threads communicate via `Application::SubmitToMainThread`.
- `ComponentRegistry` is the single source of truth for components: serialization, copying, automation,
  inspector and script interop all go through it.
- Script bindings are bound by name (`ScriptBindings.cpp` ↔ `InternalCalls.cs`); both sides change together.
- Every editor modification goes through the command history (undo/redo), including automation commands.
- File formats are versioned JSON; bump the version and add a migration when changing a format.

## Testing

- C++ tests use doctest: `Tests/StradaTests/Source/<Module>/<Topic>Tests.cpp`, test cases named
  `"<Module>: <behavior>"`. Tests that touch global state (Input, Log) restore it; temporary files go through
  `Testing::TemporaryDirectory`.
- Run everything with `python Tools/build.py --test`; CI runs the same command on Windows, Linux and macOS.
- Every file format users write gets a fuzz test (`Testing::DocumentMutator` in `Tests/StradaTests/Source/Fuzzing.h`):
  damaged documents must be refused or repaired, what loads must save and load again, and what runs must run. Set
  `STRADA_FUZZ_SCALE=<n>` for n times more rounds after changing a loader (Debug builds keep Jolt's asserts).

Rules for the other suites:

- Editor automation tests live in `Tests/StradaEditorTests` and run the editor headless against a temporary project.
- Game player tests live in `Tests/StradaRuntimeTests`: they write games to temporary directories and run the
  `StradaRuntime` executable on them, checking exit codes and output (windowed runs skip like windowed tests).
- Scripting tests (`Testing::ScriptEngineScope`) run the game scripts of `Tests/TestScripts`; scripts report what they
  saw through entity names and the log.
- Audio tests run the `AudioEngine` with manual output (`Testing::AudioEngineScope`) and measure the mix they read,
  so they need no audio device and never depend on timing.
- GPU tests start with `ST_REQUIRE_GPU()` (skips when no Vulkan device is available). Renderer golden images live in
  `Tests/Data/Golden/`; update them only deliberately and review the diff images.
- C# tests use xUnit v3 in `Tests/ScriptCoreTests` and `Tests/StradaToolTests` (executables ctest runs; packages are
  pinned by `packages.lock.json`).
- **Feature-test project**: any new component or scripting API must be exercised in `Projects/FeatureTest`
  (scene + scripts using `Strada.Testing`). `ScriptCoreTests` fails if a public scripting API member is not used by
  the feature-test scripts (read from their compiled metadata), `StradaTests` fails if a registered component is
  missing from its scenes, and `StradaRuntimeTests` runs it through the player (`--test`) and fails on any failed
  check. Edit its scenes with the editor (open `Projects/FeatureTest/FeatureTest.sproj`); CMake builds its scripts.

## Commit process

1. Build Debug **and** Release with zero warnings.
2. Run all tests (`python Tools/build.py --test` for both configurations). All must pass.
3. Format: `python Tools/format.py` (clang-format for C++, dotnet format for the C# projects);
   `python Tools/format.py --check` must pass.
4. **Code review before committing**: review the full diff (use the `strada-code-review` skill) for correctness,
   style compliance, test coverage, cross-platform portability, thread safety, resource lifetime and error handling.
   Fix every finding, then re-run steps 1–3.
5. Commit with an imperative, scoped message: `<area>: <summary>` (e.g. `renderer: add GTAO pass`), with a body
   explaining what and why for non-trivial changes.
6. Push to `origin main` and check CI on all three platforms; fix failures immediately.

## Platform notes

- Windows: MSVC with the dynamic CRT (`/MD`). The Vulkan loader ships with the GPU driver; never redistribute
  `vulkan-1.dll`. Exported games carry the MSVC runtime DLLs, which Release and Dist builds stage in `Redist/`.
- `<Windows.h>` is only included in `.cpp` files (CMake defines `WIN32_LEAN_AND_MEAN`, `NOMINMAX`, `UNICODE`), and
  only the explicit wide (`...W`) Win32 functions are called. Never name functions, methods or variables after Win32
  macros — they are silently renamed in any translation unit that includes `<Windows.h>`. Known offenders:
  `FormatMessage`, `GetEnvironmentVariable`, `CreateDirectory`, `CopyFile`, `MoveFile`, `DeleteFile`,
  `GetCurrentDirectory`, `CreateProcess`, `LoadLibrary`, `LoadImage`, `CreateWindow`, `CreateFont`, `DrawText`,
  `GetObject`, `GetMessage`, `SendMessage`, `PlaySound`, `GetClassName`, `CreateEvent`, `CreateMutex`, `Yield`,
  `near`, `far`, `min`, `max`, `interface`, `ERROR`, `OPAQUE`, `TRANSPARENT`.
- macOS: MoltenVK through the Vulkan SDK; enable `VK_KHR_portability_enumeration` and `VK_KHR_portability_subset`.
  Exported games bundle MoltenVK and the loader in the `.app` (staged from the Vulkan SDK in the build's `Vulkan/`).
- Linux: GLFW is built with X11 and Wayland. ImGui multi-viewports are disabled on Wayland (unsupported); set
  `STRADA_GLFW_PLATFORM=x11` to force X11/XWayland.

## RHI notes (NVRHI on Vulkan)

- Vulkan objects and vulkan.hpp stay inside `Strada/Source/Strada/RHI/*.cpp` (`VulkanContext.h` is internal). The rest of
  the engine uses NVRHI interfaces from `GraphicsDevice::GetDevice()`.
- `GraphicsDevice::GetDevice()` may return NVRHI's validation wrapper. Vulkan-specific calls (queue semaphores,
  submissions that must not be filtered) go to `Vulkan::GetNvrhiVulkanDevice()`; the wrapper silently drops
  `executeCommandLists` calls without command lists.
- NVRHI keeps resources alive while command lists that bind or copy them are in flight, but `clearTexture*` does not
  record the texture. Never release a texture whose only GPU use was a clear before the GPU finished with it.
- Swapchains use 8-bit UNORM formats: everything written to them must already be sRGB-encoded (ImGui colors are).
- Every acquired swapchain image is written (the application clears it) before it is presented.
- GPU tests run with validation enabled and fail on any validation message; `STRADA_TESTS_ALLOW_NO_GPU=1` skips them on
  machines without a Vulkan device (CI macOS runners). CI runs them on Mesa lavapipe on Windows and Linux. Windowed
  tests additionally need a display; `STRADA_TESTS_ALLOW_NO_DISPLAY=1` lets them skip on headless machines.
- Paths: use `std::filesystem::path`; store project-relative paths with forward slashes in files.

## Skills

Task playbooks live in `.claude/skills/`. Each subsystem adds its playbook when it lands; keep them accurate when
workflows change.

| Skill | Use it for |
|-------|-----------|
| `strada-build` | Configuring, building and testing; adding sources and dependencies |
| `strada-code-review` | The mandatory pre-commit review and the commit/push procedure |
| `strada-add-component` | Adding or changing an ECS component end to end |
| `strada-add-script-api` | Adding or changing C# scripting API (bindings, managed API, tests) |
| `strada-automation-command` | Adding or changing an editor automation command |
| `strada-render-pass` | Adding a shader or render pass, golden-image tests |
| `strada-editor-ui` | Editor panels, the generated inspector, undo merge keys, shortcuts, headless UI tests |
| `strada-make-game` | Building a game through the editor's MCP tools (`strada mcp`, registered in `.mcp.json`) |
