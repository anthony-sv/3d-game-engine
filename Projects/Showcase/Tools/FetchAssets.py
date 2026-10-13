"""Downloads the Showcase project's Poly Haven assets (CC0: the sky, the texture sets' maps and the models), which the
repository does not carry because of their size (about 95 MB). Run it once after cloning, before opening the project:

    python Projects/Showcase/Tools/FetchAssets.py

It uses the repository's strada tool (`dotnet run --project StradaTool`, or the strada executable given as the argument)
and downloads only what is missing. Every file is checked against the size and MD5 that Poly Haven lists. The project's
asset registry already knows the files, so the scenes and materials find them by the same handles.
"""
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import Author  # noqa: E402  (the asset IDs and resolutions the project was built with)

ASSETS = os.path.join(Author.PROJECT, "Assets")


def strada_command():
    if len(sys.argv) > 1:
        return [sys.argv[1]]
    return ["dotnet", "run", "--project", os.path.join(Author.ROOT, "StradaTool"), "-c", "Release", "--no-launch-profile", "--"]


def download(command, kind, identifier, resolution, output):
    subprocess.run(command + [kind, identifier, "--resolution", resolution, "--output", output], check=True, cwd=Author.ROOT)


def fetch(command, kind, identifier, resolution, folder, present):
    if os.path.exists(present):
        print(f"{identifier}: present")
        return
    print(f"{identifier}: downloading")
    download(command, kind, identifier, resolution, os.path.join(ASSETS, folder))


def fetch_texture(command, texture):
    """A texture set's folder is in the repository already (it holds the set's material), and strada downloads into new
    folders only: the maps download into a staging folder beside it and move in. The color map moves last, so a set whose
    color map is there is complete."""
    resolution = Author.texture_resolution(texture)
    textures = os.path.join(ASSETS, "Textures")
    folder = os.path.join(textures, texture)
    color = f"{texture}_diff_{resolution}.jpg"
    if os.path.exists(os.path.join(folder, color)):
        print(f"{texture}: present")
        return
    print(f"{texture}: downloading")
    # Hidden (its name starts with a dot), so that an editor that has the project open ignores it.
    staging = tempfile.mkdtemp(prefix=f".{texture}-", dir=textures)
    try:
        download(command, "texture", texture, resolution, staging)
        downloaded = os.path.join(staging, texture)
        names = sorted(os.listdir(downloaded), key=lambda name: name == color)
        if not names or names[-1] != color:
            raise SystemExit(f"{texture}: the download has no {color}")
        os.makedirs(folder, exist_ok=True)
        for name in names:
            os.replace(os.path.join(downloaded, name), os.path.join(folder, name))
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def main():
    command = strada_command()
    fetch(command, "hdri", Author.HDRI, "4k", "Environments", os.path.join(ASSETS, "Environments", f"{Author.HDRI}_4k.hdr"))
    for texture in Author.TEXTURES:
        fetch_texture(command, texture)
    for model in Author.MODELS:
        fetch(command, "model", model, "1k", "Models", os.path.join(ASSETS, "Models", model))


if __name__ == "__main__":
    main()
