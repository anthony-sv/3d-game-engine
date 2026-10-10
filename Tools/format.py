#!/usr/bin/env python3
"""Formats (or checks) all C++ sources with the repository's .clang-format and the C# projects with dotnet format.

Examples:
    python Tools/format.py            # format in place
    python Tools/format.py --check    # exit with an error if any file is not formatted

clang-format 22 is required so every machine and CI produce identical output (`pip install clang-format==22.1.3`
provides it on any platform; Visual Studio 2026 also ships it). C# formatting follows .editorconfig and needs the
.NET 10 SDK.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
REQUIRED_MAJOR_VERSION = 22
SOURCE_DIRECTORIES = ["Strada", "StradaEditor", "StradaRuntime", "Tests"]
# HLSL is not formatted: clang-format has no HLSL mode and mangles semantics such as "float4 PSMain(...) : SV_Target0".
CPP_EXTENSIONS = {".h", ".hpp", ".inl", ".c", ".cpp"}
EXCLUDED_DIRECTORY_NAMES = {"build", "bin", "obj", "ThirdParty", "Output"}
DOTNET_PROJECTS = ["Strada-ScriptCore/Strada.ScriptCore.csproj", "Tests/TestScripts/Strada.TestScripts.csproj"]


def find_clang_format() -> str:
    candidates: list[str] = []
    if os.environ.get("CLANG_FORMAT"):
        candidates.append(os.environ["CLANG_FORMAT"])
    found = shutil.which("clang-format")
    if found:
        candidates.append(found)
    if platform.system() == "Windows":
        program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
        for edition in ("Community", "Professional", "Enterprise", "BuildTools"):
            for version in ("18", "2022"):
                path = Path(program_files) / "Microsoft Visual Studio" / version / edition / "VC" / "Tools" / "Llvm" / "x64" / "bin" / "clang-format.exe"
                if path.exists():
                    candidates.append(str(path))

    for candidate in candidates:
        output = subprocess.run([candidate, "--version"], capture_output=True, text=True, check=False).stdout
        match = re.search(r"clang-format version (\d+)", output)
        if match and int(match.group(1)) == REQUIRED_MAJOR_VERSION:
            return candidate

    print(f"error: clang-format {REQUIRED_MAJOR_VERSION}.x not found (pip install clang-format==22.1.3, or set CLANG_FORMAT)",
          file=sys.stderr)
    sys.exit(1)


def collect_files() -> list[Path]:
    files: list[Path] = []
    for directory in SOURCE_DIRECTORIES:
        root = REPOSITORY_ROOT / directory
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if any(part in EXCLUDED_DIRECTORY_NAMES for part in path.relative_to(REPOSITORY_ROOT).parts):
                continue
            if path.suffix in CPP_EXTENSIONS:
                files.append(path)
    return sorted(files)


def format_dotnet_projects(check: bool) -> bool:
    """Runs dotnet format on every C# project; returns whether all were (or are now) formatted."""
    dotnet = shutil.which("dotnet")
    if dotnet is None:
        print("error: dotnet not found (install the .NET 10 SDK)", file=sys.stderr)
        sys.exit(1)

    formatted = True
    for project in DOTNET_PROJECTS:
        command = [dotnet, "format", str(REPOSITORY_ROOT / project)]
        if check:
            command.append("--verify-no-changes")
        result = subprocess.run(command, cwd=REPOSITORY_ROOT, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            formatted = False
            print(f"{'needs formatting' if check else 'dotnet format failed on'}: {project}\n{result.stdout}{result.stderr}",
                  file=sys.stderr)
    return formatted


def main() -> None:
    parser = argparse.ArgumentParser(description="Format Strada C++ sources with clang-format.")
    parser.add_argument("--check", action="store_true", help="report unformatted files instead of rewriting them")
    arguments = parser.parse_args()

    clang_format = find_clang_format()
    files = collect_files()
    failures: list[Path] = []

    errors = 0
    for path in files:
        # Sources go through stdin in binary mode so no newline translation happens; --assume-filename makes
        # clang-format pick the language and the repository .clang-format.
        original = path.read_bytes()
        result = subprocess.run([clang_format, "--style=file", f"--assume-filename={path}"], input=original,
                                cwd=REPOSITORY_ROOT, capture_output=True, check=False)
        relative = path.relative_to(REPOSITORY_ROOT).as_posix()
        if result.returncode != 0:
            errors += 1
            print(f"clang-format failed on {relative}: {result.stderr.decode(errors='replace')}", file=sys.stderr)
            continue
        if result.stdout != original:
            failures.append(path)
            if arguments.check:
                print(f"needs formatting: {relative}")
            else:
                path.write_bytes(result.stdout)
                print(f"formatted: {relative}")

    dotnet_formatted = format_dotnet_projects(arguments.check)
    if errors or not dotnet_formatted:
        sys.exit(1)
    if arguments.check and failures:
        print(f"{len(failures)} file(s) need formatting; run 'python Tools/format.py'", file=sys.stderr)
        sys.exit(1)
    print(f"{len(files)} file(s) checked, {len(failures)} {'need formatting' if arguments.check else 'reformatted'}; "
          f"{len(DOTNET_PROJECTS)} C# project(s) {'checked' if arguments.check else 'formatted'}")


if __name__ == "__main__":
    main()
