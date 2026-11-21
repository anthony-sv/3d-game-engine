---
name: strada-build
description: Configure, build and test the Strada engine on Windows, macOS or Linux with Tools/build.py and the CMake presets. Use when building, running tests, adding source files or third-party dependencies to CMake, or diagnosing build and test failures.
---

# Building and testing Strada

## Everyday commands

```sh
python Tools/build.py                          # Debug build for this OS (configure + build)
python Tools/build.py --test                   # build, then run every test suite through ctest
python Tools/build.py --config release --test  # Release (optimized, asserts on)
python Tools/build.py --config dist            # Dist (shipping configuration, asserts off)
python Tools/build.py --compiler clang         # Linux: Clang instead of GCC
python Tools/build.py --target StradaTests     # build one target
python Tools/build.py --jobs 4                 # limit parallelism (use when other builds run concurrently)
python Tools/build.py --clean                  # wipe build/<preset> first
python Tools/build.py --keep-going             # build past failed compiles, report them all, still fail
```

- Presets: `windows-{debug,release,dist}`, `windows-vs` (Visual Studio solution), `linux-gcc-{debug,release,dist}`,
  `linux-clang-{debug,release}`, `linux-clang-tsan` (ThreadSanitizer, below), `macos-{debug,release,dist}`. Build trees live in `build/<preset>/`, binaries in
  `build/<preset>/bin/`.
- On Windows the script finds Visual Studio with `vswhere` and runs everything inside the `vcvars64` environment, so it
  works from PowerShell, cmd or Git Bash. Plain `cmake --preset ...` needs a VS developer shell.
- The script fills in `VULKAN_SDK` from the default install location when the variable is missing.
- Pass extra CMake arguments with `--cmake-arg=-DNAME=VALUE` (note the `=`), e.g.
  `--cmake-arg=-DSTRADA_DEPENDENCY_CACHE_DIR=<dir>` to reuse downloaded dependency archives across build trees.
- `STRADA_TEST_SUITES` names the test suites a build adds, separated by semicolons; by default every one
  (`StradaTests;StradaEditorTests;StradaRuntimeTests;ScriptCoreTests;StradaToolTests`), and a name that is not a suite
  fails the configure. Quote the argument in a shell, where `;` ends a command:
  `python Tools/build.py "--cmake-arg=-DSTRADA_TEST_SUITES=StradaTests;ScriptCoreTests"`. Every target that is not a
  test suite is built whatever it names, and so are the script projects the suites share (the test scripts and the
  scripts of FeatureTest, Blocks, Showcase and the csharp-starter template, defined in the root `CMakeLists.txt`). A
  target that more than one suite uses belongs in the root file too: in one suite's directory it would be missing from
  a build without that suite. CI's Windows MSVC Debug, Windows MSVC Release and Linux GCC Release builds are two jobs
  each with it, `StradaTests;ScriptCoreTests` and `StradaEditorTests;StradaRuntimeTests;StradaToolTests`, so that each
  stays far from the job's time limit; each pair together builds every target and runs every ctest entry once. The value
  stays in the build tree's cache until `--cmake-arg=-USTRADA_TEST_SUITES`.
- A build stops at the first failed command. `--keep-going` (Ninja's `-k 0`) builds everything that does not depend on
  a failed command, so one build reports every compile error, and still exits with a failure. CI's Build step passes
  it, so that one run reports every compile error on each platform: most contributors cannot build two of the three
  platforms locally.

## Running tests directly

```sh
build/<preset>/bin/StradaTests                         # all C++ engine tests
build/<preset>/bin/StradaTests -tc="FileSystem*"       # filter test cases by name
build/<preset>/bin/StradaTests -sc="*round trip*"      # filter subcases
build/<preset>/bin/StradaTests --list-test-cases
build/<preset>/bin/StradaEditorTests                   # editor model and automation tests (headless)
build/<preset>/bin/StradaRuntimeTests                  # game player tests (they run StradaRuntime)
dotnet build/<preset>/bin/ScriptCoreTests/Strada.ScriptCore.Tests.dll   # C# scripting API tests
STRADA_EDITOR=build/<preset>/bin/StradaEditor dotnet build/<preset>/bin/StradaToolTests/Strada.Tool.Tests.dll
                                                       # strada tests (they start the editor headless)
```

Test cases are named `"<Module>: <behavior>"`, so module filters such as `-tc="Input*"` work.

## Test shards

ctest runs StradaTests, StradaEditorTests and StradaRuntimeTests as shards, one entry each with its own 600 s timeout,
defined once per suite in its `Shards.cmake` (`Tests/StradaTests/Shards.cmake`, `Tests/StradaEditorTests/Shards.cmake`,
`Tests/StradaRuntimeTests/Shards.cmake`) as doctest
source-file filters (`--source-file=`, `--source-file-exclude=`): `<suite>.<shard>` for every shard listed there, with
`<suite>.Core` taking every file no other shard names. `strada_add_test_shards` (`cmake/StradaTestShards.cmake`)
registers the suites' entries. `<suite>.Partition` (a `cmake -P` script, `cmake/CheckShardPartition.cmake`) lists the
test cases unfiltered and through each shard's filters (skipped test cases included) and fails when a test case is in no
shard or in two, or a shard selects nothing.

| Suite | Shards |
|-------|--------|
| StradaTests (by module folder of `Source/`) | `Asset`; `Scripting` (`Script/`, `Scene/`); `Rendering` (`RHI/`, `Renderer/`, `ImGui/`: the GPU tests); `Core` (every other folder) |
| StradaEditorTests (by file) | `AutomationFuzz`; `Graph`; `VisualScriptPanels`; `VisualScriptModel`; `Moto`; `Core` (every other file) |
| StradaRuntimeTests (by file) | `Samples` (`SampleTests.cpp`: the sample projects); `Templates` (`TemplateTests.cpp`); `Core` (every other file) |

```sh
ctest --preset windows-debug -R StradaTests                # every StradaTests shard and its partition check
ctest --preset windows-debug -R StradaEditorTests          # every StradaEditorTests shard and its partition check
ctest --preset windows-debug -R StradaEditorTests.Graph    # one shard
ctest --preset windows-debug -R StradaRuntimeTests         # every StradaRuntimeTests shard and its partition check
build/<preset>/bin/StradaEditorTests "--source-file=*AutomationFuzzTests.cpp"   # the AutomationFuzz shard, run directly
build/<preset>/bin/StradaTests "--source-file=*StradaTests?Source?Asset?*"     # the Asset shard, run directly
```

- A new test source of StradaTests lands in the shard that names its module folder, or in Core; a new folder lands in
  Core. A new test file of StradaEditorTests lands in Core unless its name matches another shard's pattern; one of
  StradaRuntimeTests lands in Core. When a
  change to the patterns leaves a test source in no shard or in two, `<suite>.Partition` fails and names its test cases.
- Patterns match the test case's full `__FILE__` path, without regard to case. Never write a path separator in a
  pattern (Windows paths use `\`): `*Source?PanelTests.cpp` selects `PanelTests.cpp` alone.
- `*` matches separators too, so an inner `*` can reach into the checkout's path (in a clone under `D:\VisualScriptLab`,
  `*VisualScript*Tests.cpp` matches every file). Anchor a pattern with an inner `*` to the suite's directory:
  `*StradaEditorTests?Source?VisualScript*Tests.cpp`, `*StradaTests?Source?Asset?*`.
- A shard that passes 450 s on CI's Windows MSVC Debug job that runs its suite is split by source-file patterns: add the
  shard to the suite's `Shards.cmake`, take its files out of the shard that held them (for Core: add its patterns to
  Core's exclusions), and check the partition with
  `cmake -DTEST_EXECUTABLE=<suite executable> -DSHARDS_FILE=Tests/<suite>/Shards.cmake -P cmake/CheckShardPartition.cmake`
  (name a changed copy as `SHARDS_FILE` to check it first).
- `STRADA_UPDATE_DOCS=1` and `STRADA_UPDATE_GOLDEN=1` reach the shards through ctest's environment; or run the
  executable with the variable:
  `STRADA_UPDATE_DOCS=1 build/<preset>/bin/StradaTests -tc="Components: Docs/Components.md*"`.

## Adding source files

Sources are listed explicitly in each target's `CMakeLists.txt` (no globbing). Add both the `.cpp` and the `.h` to the
module's list in `Strada/CMakeLists.txt` (or the test/editor/runtime target), keeping the list sorted. A new test
source of StradaTests, StradaEditorTests or StradaRuntimeTests must also land in exactly one shard of its suite's `Shards.cmake` (see
[Test shards](#test-shards)): `<suite>.Partition` fails, naming its test cases, when none or two select it.

## Precompiled headers

- The engine library precompiles `Strada/Source/stpch.h`, which its `.cpp` files include first.
  `StradaEditorCore`, `StradaTests`, `StradaEditorTests` and `StradaRuntimeTests` precompile
  `STRADA_PRECOMPILED_THIRD_PARTY_HEADERS` instead, through `strada_precompile_third_party_headers(<target>)`
  (`cmake/StradaCompilerSettings.cmake`). That list holds the common standard headers, glm, spdlog with its fmt, and
  nlohmann/json. CMake forces the header (`cmake_pch.hxx` in the target's build directory) into every C++ source of
  those targets, so their sources include no precompiled header. Each target compiles its own copy, because their
  definitions differ.
- MSVC, clang-cl, Clang and Apple Clang get that header; GCC builds the four targets without it, in every version and
  configuration. With it, CI's Linux GCC Release job (GCC 13) failed with `-Warray-bounds` errors inside libstdc++'s
  string concatenation at six ordinary concatenations in five test sources, none of which it reported without the
  header. The cause is not established.
- Every source still includes everything it uses, as if there were no precompiled header. CI's Linux Clang Debug job
  builds the four targets without it (`STRADA_PRECOMPILE_THIRD_PARTY_HEADERS=OFF`; the engine library keeps
  `stpch.h`), so a forgotten include fails there. A GCC build checks includes too, but only through the exclusion
  above; the Clang job is the check that does not depend on it. To reproduce that job's failure locally, build with
  `python Tools/build.py --cmake-arg=-DSTRADA_PRECOMPILE_THIRD_PARTY_HEADERS=OFF`. The value stays in the build tree's
  cache until `--cmake-arg=-DSTRADA_PRECOMPILE_THIRD_PARTY_HEADERS=ON` or
  `--cmake-arg=-USTRADA_PRECOMPILE_THIRD_PARTY_HEADERS`; each switch recompiles the four targets.
- A header joins the list only when it is third-party or standard, most of the targets' sources parse it, and a
  measurement shows a gain (MSVC `/Bt+` front-end times on a sample of sources, with and without it). EnTT, Dear ImGui,
  NVRHI and doctest were measured that way: on top of the list, they saved about 1 s over eight sources at most,
  within noise.
- Never add a Strada header to the list. An edit to it would rebuild every source of the four targets, and a source
  that defines a configuration macro before its first include (`Core/ProfilerDistFormTests.cpp`) would no longer see
  it first.
- A header in the list needs no macro defined before it: the configuration macros of glm, ImGui and Vulkan-HPP are
  target definitions. A source that must not see the header gets `SKIP_PRECOMPILE_HEADERS`, as the generated resource
  and shader sources do. A change to the list rebuilds the four targets.

## C# projects

- CMake builds each C# project with `strada_add_dotnet_project(<target> PROJECT <csproj> OUTPUT_DIRECTORY <dir>
  [DEPENDS <targets>] [PROPERTIES Name=Value...])` (`cmake/StradaDotNet.cmake`). The target runs `dotnet build` on every
  build and MSBuild skips what is up to date, so C# files need no CMake listing.
- `Directory.Build.props` holds the shared settings (nullable, warnings as errors, code style in the build) and keeps
  outputs out of the sources: in `build/<preset>/csharp` from CMake, in `artifacts/` from IDEs.
- `Strada.ScriptCore.dll` is built next to the executables; game script projects reference it by path
  (`Tests/TestScripts` receives `StradaScriptCoreDirectory` from CMake and references the project from IDEs).
- New C# projects go into `DOTNET_PROJECTS` in `Tools/format.py`.

## Adding or updating a third-party dependency

1. Pick a release archive (prefer a GitHub release asset or tag tarball) and compute its SHA256:
   `python -c "import hashlib,sys,urllib.request;print(hashlib.sha256(urllib.request.urlopen(sys.argv[1]).read()).hexdigest())" <url>`
2. Add a `strada_declare_dependency(<name> <url> <sha256>)` block to `cmake/StradaDependencies.cmake`, set the
   dependency's options as `CACHE ... FORCE` variables (some projects use old CMake policies where plain variables are
   ignored), and add it to the matching `FetchContent_MakeAvailable` call.
3. Link it with the narrowest visibility (PRIVATE unless engine headers expose its types) and never apply
   `strada_configure_target` to third-party targets. Single-header implementations compile in their own target via
   `strada_configure_third_party_target`.
4. Add the dependency, version, license and URL to `ThirdPartyNotices.md`.
5. Reconfigure from scratch (`--clean`) on at least one platform; CI covers the others.

## Troubleshooting

- **Configure fails after changing dependency options**: run with `--clean`; cached `FORCE` values persist otherwise.
- **`error: no Visual Studio installation`**: install the "Desktop development with C++" workload (VS 2022 17.10+ or
  VS 2026).
- **Warnings fail the build**: Strada targets compile with `/W4 /WX` or `-Wall -Wextra -Wpedantic -Werror`. Fix the
  warning; do not silence it. `-DSTRADA_WARNINGS_AS_ERRORS=OFF` exists only for unsupported newer compilers.
- **Name clashes on Windows** (e.g. a method silently becoming `FormatMessageW`): a Win32 macro collided with an
  identifier; see the Platform notes in `AGENTS.md` and rename the identifier.
- **Sanitizers**: `--cmake-arg=-DSTRADA_ENABLE_SANITIZERS=ON` (ASan + UBSan on GCC/Clang, ASan on MSVC).
- **ThreadSanitizer** (GCC or Clang, never MSVC; it excludes `STRADA_ENABLE_SANITIZERS`): `python Tools/build.py
  --preset linux-clang-tsan --target StradaTests` builds Debug with `STRADA_ENABLE_THREAD_SANITIZER`, which
  `strada_add_thread_sanitizer` applies to every Strada target and every third-party target the build compiles (a new
  compiled dependency is added to the list at the end of `cmake/StradaDependencies.cmake` unless it goes through
  `strada_configure_third_party_target`). The preset has no test preset: the whole suites host the .NET runtime and
  time-bound cases, so run the threading cases by filter, as `.github/workflows/tsan.yml` does, with
  `TSAN_OPTIONS=suppressions=$PWD/.github/tsan-suppressions.txt`. A race in Strada's code is fixed, never suppressed;
  the file lists prebuilt libraries and one piece of Jolt's Debug bookkeeping (the owner its mutexes remember for an
  assert). The workflow runs on a schedule, by hand and on a push to a `tsan/**` branch,
  which is how to try a change to it, and it never gates a merge. A threading case added to a suite gets a filter in its
  step.
- **Global allocation functions under a sanitizer**: a sanitizer's runtime defines every `operator new` and
  `operator delete` itself, and Clang on Linux links it statically, so a source that replaces them fails to link there
  ("multiple definition"). Both sanitizer options define `ST_SANITIZER_BUILD` for Strada's targets: such a source
  compiles its replacements only without it (as StradaTests' `Audio/MixAllocationHook.cpp` does), and a case that
  relies on them says it skips that check there.
