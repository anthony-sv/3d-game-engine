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

Entries marked *(planned)* are introduced by upcoming subsystems; their location and purpose are fixed by
`Docs/Architecture.md`.

| Path | What |
|------|------|
| `Strada/` | Engine static library (`Source/Strada/<Module>`, `Shaders/`, `Resources/`) |
| `StradaEditor/` *(planned)* | `StradaEditorCore` static library + `StradaEditor` executable |
| `StradaRuntime/` *(planned)* | Player executable used by exported games |
| `Strada-ScriptCore/` *(planned)* | C# scripting API (`Strada.ScriptCore.dll`) |
| `StradaTool/` *(planned)* | `strada` CLI + MCP stdio bridge |
| `Tests/StradaTests/` | C++ engine tests (doctest); `StradaEditorTests`, `ScriptCoreTests`, `TestScripts` and `Data` are *(planned)* |
| `Projects/FeatureTest/` *(planned)* | Project exercising every component and the entire scripting API |
| `cmake/` | Build modules: options, compiler settings, dependencies |
| `Tools/` | `build.py` (configure/build/test), `format.py` (clang-format) |
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
- Logging only through `ST_CORE_*` (engine) and `ST_*` (editor/runtime) macros — never `std::cout`/`printf`.
- Keep third-party headers out of public engine headers when practical (Jolt, Vulkan, miniaudio, hostfxr, cgltf,
  ufbx live in `.cpp` files). NVRHI handle types are allowed in Renderer/RHI headers.
- Document thread affinity of public functions when not main-thread-only.
- Comments explain *why*, not *what*. Public API headers get brief doc comments where intent is not obvious.

### HLSL

- Files: `Strada/Shaders/<Technique>.hlsl`, shared code in `Strada/Shaders/Include/*.hlsli`.
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

Rules for suites that arrive with later subsystems (binding as soon as the subsystem exists):

- Editor automation tests live in `Tests/StradaEditorTests` and run the editor headless against a temporary project.
- GPU tests start with `ST_REQUIRE_GPU()` (skips when no Vulkan device is available). Renderer golden images live in
  `Tests/Data/Golden/`; update them only deliberately and review the diff images.
- C# tests use xUnit in `Tests/ScriptCoreTests`.
- **Feature-test project**: any new component or scripting API must be exercised in `Projects/FeatureTest`
  (scene + scripts using `Strada.Testing`). `ScriptCoreTests` fails if a public scripting API member is not
  referenced by the feature-test scripts, and `StradaTests` fails if a registered component is missing from the
  feature-test scene.

## Commit process

1. Build Debug **and** Release with zero warnings.
2. Run all tests (`python Tools/build.py --test` for both configurations). All must pass.
3. Format: `python Tools/format.py` (C++/HLSL) and `dotnet format` (C#); `python Tools/format.py --check` must pass.
4. **Code review before committing**: review the full diff (use the `strada-code-review` skill) for correctness,
   style compliance, test coverage, cross-platform portability, thread safety, resource lifetime and error handling.
   Fix every finding, then re-run steps 1–3.
5. Commit with an imperative, scoped message: `<area>: <summary>` (e.g. `renderer: add GTAO pass`), with a body
   explaining what and why for non-trivial changes.
6. Push to `origin main` and check CI on all three platforms; fix failures immediately.

## Platform notes

- Windows: MSVC with the dynamic CRT (`/MD`). The Vulkan loader ships with the GPU driver; never redistribute
  `vulkan-1.dll`.
- `<Windows.h>` is only included in `.cpp` files (CMake defines `WIN32_LEAN_AND_MEAN`, `NOMINMAX`, `UNICODE`), and
  only the explicit wide (`...W`) Win32 functions are called. Never name functions, methods or variables after Win32
  macros — they are silently renamed in any translation unit that includes `<Windows.h>`. Known offenders:
  `FormatMessage`, `GetEnvironmentVariable`, `CreateDirectory`, `CopyFile`, `MoveFile`, `DeleteFile`,
  `GetCurrentDirectory`, `CreateProcess`, `LoadLibrary`, `LoadImage`, `CreateWindow`, `CreateFont`, `DrawText`,
  `GetObject`, `GetMessage`, `SendMessage`, `PlaySound`, `GetClassName`, `CreateEvent`, `CreateMutex`, `Yield`,
  `near`, `far`, `min`, `max`, `ERROR`, `OPAQUE`, `TRANSPARENT`.
- macOS: MoltenVK through the Vulkan SDK; enable `VK_KHR_portability_enumeration` and `VK_KHR_portability_subset`.
  Exported games bundle MoltenVK and the loader in the `.app`.
- Linux: GLFW is built with X11 and Wayland. ImGui multi-viewports are disabled on Wayland (unsupported).
- Paths: use `std::filesystem::path`; store project-relative paths with forward slashes in files.

## Skills

Task playbooks live in `.claude/skills/`. Each subsystem adds its playbook when it lands; keep them accurate when
workflows change.

| Skill | Use it for |
|-------|-----------|
| `strada-build` | Configuring, building and testing; adding sources and dependencies |
| `strada-code-review` | The mandatory pre-commit review and the commit/push procedure |
