#!/usr/bin/env python3
"""Configure, build and test Strada with the CMake presets for the current platform.

Examples:
    python Tools/build.py                       # Debug build for this OS
    python Tools/build.py --config release --test
    python Tools/build.py --compiler clang      # Linux only
    python Tools/build.py --target StradaTests --jobs 4

On Windows the Visual Studio developer environment (cl.exe, CMake, Ninja) is located automatically with vswhere,
so the script works from any shell.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def run(command: list[str], environment: dict[str, str]) -> None:
    print("+ " + " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=REPOSITORY_ROOT, env=environment)
    if completed.returncode != 0:
        sys.exit(completed.returncode)


def find_visual_studio_environment() -> dict[str, str]:
    """Returns the environment produced by vcvars64.bat of the newest Visual Studio with the C++ tools."""
    program_files = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        fail("vswhere.exe not found; install Visual Studio 2022 17.10+ or 2026 with the C++ workload")

    query = subprocess.run(
        [str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-format", "json", "-utf8"],
        capture_output=True, text=True, encoding="utf-8", check=False)
    installations = json.loads(query.stdout or "[]")
    if not installations:
        fail("no Visual Studio installation with the C++ x64 tools was found")

    vcvars = Path(installations[0]["installationPath"]) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not vcvars.exists():
        fail(f"vcvars64.bat not found at {vcvars}")

    dump = subprocess.run(f'cmd /s /c ""{vcvars}" >nul && set"', capture_output=True, text=True, shell=True,
                          check=False)
    if dump.returncode != 0:
        fail("failed to initialize the Visual Studio developer environment")

    environment: dict[str, str] = {}
    for line in dump.stdout.splitlines():
        name, separator, value = line.partition("=")
        if separator and name:
            environment[name] = value
    return environment


def locate_vulkan_sdk(environment: dict[str, str]) -> None:
    if environment.get("VULKAN_SDK"):
        return
    candidates: list[Path] = []
    if platform.system() == "Windows":
        root = Path("C:/VulkanSDK")
        if root.is_dir():
            candidates = sorted((path for path in root.iterdir() if path.is_dir()), reverse=True)
    elif platform.system() == "Darwin":
        root = Path.home() / "VulkanSDK"
        if root.is_dir():
            candidates = sorted(((path / "macOS") for path in root.iterdir() if (path / "macOS").is_dir()), reverse=True)
    if candidates:
        environment["VULKAN_SDK"] = str(candidates[0])
        print(f"VULKAN_SDK not set; using {candidates[0]}")


def preset_name(config: str, compiler: str | None) -> str:
    system = platform.system()
    if system == "Windows":
        return f"windows-{config}"
    if system == "Darwin":
        return f"macos-{config}"
    if system == "Linux":
        return f"linux-{compiler or 'gcc'}-{config}"
    fail(f"unsupported platform '{system}'")
    return ""


def main() -> None:
    parser = argparse.ArgumentParser(description="Configure, build and test Strada.")
    parser.add_argument("--config", choices=["debug", "release", "dist"], default="debug")
    parser.add_argument("--compiler", choices=["gcc", "clang"], help="Linux compiler (default: gcc)")
    parser.add_argument("--preset", help="explicit configure/build preset (overrides --config/--compiler)")
    parser.add_argument("--target", help="build only this target")
    parser.add_argument("--jobs", type=int, help="parallel build jobs")
    parser.add_argument("--test", action="store_true", help="run the test suites after building")
    parser.add_argument("--clean", action="store_true", help="delete the build directory first")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--cmake-arg", action="append", default=[], help="extra argument passed to the configure step")
    arguments = parser.parse_args()

    preset = arguments.preset or preset_name(arguments.config, arguments.compiler)
    environment = dict(os.environ)
    if platform.system() == "Windows":
        environment = find_visual_studio_environment()
    locate_vulkan_sdk(environment)

    cmake = shutil.which("cmake", path=environment.get("PATH") or environment.get("Path"))
    if cmake is None:
        fail("cmake not found on PATH")

    build_directory = REPOSITORY_ROOT / "build" / preset
    if arguments.clean and build_directory.exists():
        shutil.rmtree(build_directory)

    run([cmake, "--preset", preset, *arguments.cmake_arg], environment)
    if arguments.configure_only:
        return

    build_command = [cmake, "--build", "--preset", preset]
    if arguments.target:
        build_command += ["--target", arguments.target]
    if arguments.jobs:
        build_command += ["--parallel", str(arguments.jobs)]
    run(build_command, environment)

    if arguments.test:
        ctest = shutil.which("ctest", path=environment.get("PATH") or environment.get("Path"))
        if ctest is None:
            fail("ctest not found on PATH")
        run([ctest, "--preset", preset], environment)


if __name__ == "__main__":
    main()
