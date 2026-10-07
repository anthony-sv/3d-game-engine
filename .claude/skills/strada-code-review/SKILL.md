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
