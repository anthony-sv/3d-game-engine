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
```

- Presets: `windows-{debug,release,dist}`, `windows-vs` (Visual Studio solution), `linux-gcc-{debug,release,dist}`,
  `linux-clang-{debug,release}`, `macos-{debug,release,dist}`. Build trees live in `build/<preset>/`, binaries in
  `build/<preset>/bin/`.
- On Windows the script finds Visual Studio with `vswhere` and runs everything inside the `vcvars64` environment, so it
  works from PowerShell, cmd or Git Bash. Plain `cmake --preset ...` needs a VS developer shell.
- The script fills in `VULKAN_SDK` from the default install location when the variable is missing.
- Pass extra CMake arguments with `--cmake-arg=-DNAME=VALUE` (note the `=`), e.g.
  `--cmake-arg=-DSTRADA_DEPENDENCY_CACHE_DIR=<dir>` to reuse downloaded dependency archives across build trees.

## Running tests directly

```sh
build/<preset>/bin/StradaTests                         # all C++ engine tests
build/<preset>/bin/StradaTests -tc="FileSystem*"       # filter test cases by name
build/<preset>/bin/StradaTests -sc="*round trip*"      # filter subcases
build/<preset>/bin/StradaTests --list-test-cases
```

Test cases are named `"<Module>: <behavior>"`, so module filters such as `-tc="Input*"` work.

## Adding source files

Sources are listed explicitly in each target's `CMakeLists.txt` (no globbing). Add both the `.cpp` and the `.h` to the
module's list in `Strada/CMakeLists.txt` (or the test/editor/runtime target), keeping the list sorted.

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
