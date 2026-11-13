---
name: strada-code-review
description: Mandatory pre-commit review for Strada changes covering correctness, Hazel naming, Allman and east-const formatting, tests, cross-platform portability, threading, resource lifetime and error handling, followed by the commit and push procedure. Use before every commit, when asked to review a diff, or when preparing to push.
---

# Strada pre-commit review

Every commit is reviewed before it is made. The review covers the **entire** staged diff, not a sample.

## 1. Gather the change

```sh
git status --short
git diff            # unstaged
git diff --cached   # staged
```

Read every changed file in full where the diff alone hides context (new files, moved code, changed invariants).

## 2. Mechanical gates (all must pass)

```sh
python Tools/format.py --check                 # clang-format 22, Allman/tabs/east const
python Tools/build.py --config debug --test    # zero warnings, all tests pass
python Tools/build.py --config release --test
```

When C# code exists: `dotnet format --verify-no-changes` on each changed project and `dotnet test` for the C# suites.

## 3. Review checklist

**Correctness**
- Logic does what the change claims; edge cases (empty input, zero sizes, missing files, invalid IDs, repeated calls,
  shutdown order) are handled.
- No undefined behavior: lifetimes of references/pointers/iterators, signed overflow, uninitialized members,
  out-of-bounds indexing, strict aliasing, misaligned access.
- Init/Shutdown pairs leave no residual state; subsystems can be re-initialized (tests do this).

**Style (AGENTS.md)**
- Hazel naming: PascalCase types/functions/namespaces/files, camelCase locals and parameters, `m_` private members,
  `s_` statics, PascalCase public struct fields and constants, `ST_` macros.
- East const (`T const&`, `char const*`), Allman braces, tabs, `#pragma once`, include order.
- No Win32-macro names as identifiers; `<Windows.h>` only in `.cpp` files.
- Comments explain *why*; no commented-out code, no TODOs standing in for missing work.

**Tests**
- New behavior and bug fixes have doctest/xUnit coverage, including failure paths.
- New components and scripting APIs are exercised in `Projects/FeatureTest` (coverage tests enforce this).
- Tests are deterministic (no timing races, no dependence on machine state) and clean up temporary files.

**Portability**
- Compiles on MSVC, GCC and Apple Clang: no compiler extensions, no MSVC-only headers in shared code, correct
  `#if defined(ST_PLATFORM_*)` branches for every platform, `std::filesystem::path` for paths and UTF-8 conversions
  through `FileSystem::PathFromUtf8`/`PathToUtf8`.
- No reliance on locale (`strtod`, `std::to_string(double)` for persisted data), endianness, or `char` signedness.

**Threading**
- Engine state is touched only on the main thread; background work uses `Application::SubmitToMainThread`.
- Shared data has a clear owner and synchronization; callbacks from third-party threads (Jolt, miniaudio, sockets)
  only enqueue.

**Resources and errors**
- RAII ownership, no leaks on early returns or failures, GPU resources released before device shutdown.
- Fallible operations return `Result<T>`/`bool` and their results are checked; user data never triggers an assert or
  crash; errors carry actionable messages.

**Visual scripting** (checks no build catches)
- A new editing operation of the visual script editor has its `graph.edit` operation, its entries in the batch
  generators of `GraphEditFuzzTests` and `AutomationFuzzTests`, and its row in Docs/Automation.md.
- A new reader of a node's `Variable`, `Function` or `Name` settings calls `IsLocalVisualScriptReference` first.
- A new built-in node kind has an explicit arm in `BuiltinNodes.TypeOf`, a recipe or a checked exclusion in
  `VisualScriptNodeCoverageTests.cpp` when its defaults do not run it, a use in the FeatureTest graphs, and a row of
  `BuiltInTitles` (`VisualScriptNodeTitlesTests.cpp`); a new API member that does not run on its defaults has a recipe.
- A changed library entry, or a changed type `Tests/Data/VisualScripting/Editor/Library.json` holds, regenerates that
  fixture from the parity test's expected file in the same change.
- The visual script editor's L2a and L3 include nothing of each other; the one exception is L3's
  `VisualScriptCurveView` including L2a's `VisualScriptTimeline.h`.
- Portability no Windows build shows: elements are removed with `std::erase_if` or `std::remove_if`, never a
  hand-written move loop, and nothing is move-assigned to itself (libstdc++ and libc++ empty a self-moved string, MSVC
  does not); a nested struct with default member initializers held in a `std::optional` member and created with
  `emplace()` declares its default constructor and defaults it in the .cpp (Clang with libstdc++ refuses it otherwise);
  headless tests send modifiers through `HeadlessImGui::Chord` or `SetModifiers` and set the scroll mode explicitly
  (macOS swaps Ctrl and Super and defaults to Pan); no reference is bound to a result reached through a temporary (GCC 13
  `-Wdangling-reference`); numbers are read as documents are read (ParseJson, then `VisualScriptSerializer::ReadFloat`),
  never with floating-point `std::from_chars`, which Apple's libc++ lacks at the deployment target; a lambda's locals
  and parameters are named apart from the locals of the function around it, captured or not (GCC `-Wshadow` warns,
  Clang and MSVC do not: run `build/smoke/shadow_check.py` where no GCC is at hand); a range-for loop variable of class
  type binds by reference (`auto const&`, `auto const& [a, b]`), because libstdc++'s `std::pair` and `std::tuple` are
  never trivially copyable and GCC's `-Wrange-loop-construct` refuses the copy MSVC accepts.
- The debugger costs nothing untraced: debugging code runs only in the traced interpreter (`Run<Traced>`) or behind a
  debugger the editor attaches; the untraced and attached-idle performance cases keep their figures and allocate
  nothing. Breakpoints, traces and the views file never reach the player or an exported game.
- Sample projects and templates: their graphs pass `SampleGraphsTests` (after a change to arranging, re-run the authoring
  command the failure names: `python Projects/PrismBreak/Tools/Author.py`, `python Templates/Tools/AuthorTemplates.py
  --template graph-starter`, or `python Projects/Showcase/Tools/Author.py --exhibit GraphGarden` for the Showcase, never
  the Showcase's full rebuild, which rewrites its materials); a change to a sample or a template updates the check counts
  its tests pin (`RuntimeTests.cpp`); doc fixtures
  (`Agents/Spinner.json`, `Agents/Debugging.json`) are shown verbatim where the docs show them, which their doc tests
  check. A change under `Projects/Showcase` leaves the two protected materials
  (`Assets/Materials/Gallery/Metallic100Roughness0.smat`, `Assets/Textures/rusty_metal_02/rusty_metal_02.smat`) untouched:
  `git diff --name-only` shows neither.

**Architecture**
- Module layering from `Docs/Architecture.md` §4 is respected; public headers avoid third-party includes where
  practical; `ComponentRegistry` stays the single source of truth for components.
- File formats changed? Version bumped and migration added.
- `ThirdPartyNotices.md`, `Docs/` and skills updated when behavior, dependencies or workflows change.

## 4. Fix, then re-run the gates

Fix every finding (or document precisely why it is not an issue) and re-run section 2.

## 5. Commit and push

```sh
git add <files>
git commit -m "<area>: <imperative summary>" -m "<what and why>"
git push origin main
```

Areas: `core`, `rhi`, `renderer`, `scene`, `asset`, `physics`, `audio`, `script`, `editor`, `runtime`, `automation`,
`build`, `ci`, `docs`, `tests`. After pushing, check CI on all three platforms (`gh run list --limit 5`,
`gh run view <id> --log-failed`) and fix failures immediately.
