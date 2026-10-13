"""Downloads the Showcase project's Poly Haven assets (CC0: the sky, the texture sets' maps and the models), which the
repository does not carry because of their size (about 95 MB). Run it once after cloning, before opening the project:

    python Projects/Showcase/Tools/FetchAssets.py

It uses the repository's strada tool (`dotnet run --project StradaTool`, or the strada executable given as the argument)
and downloads only what is missing. Every file is checked against the size and MD5 that Poly Haven lists. The project's
asset registry already knows the files, so the scenes and materials find them by the same handles.
"""
import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import Author  # noqa: E402  (the asset IDs and resolutions the project was built with)

ASSETS = os.path.join(Author.PROJECT, "Assets")


def strada_command():
    if len(sys.argv) > 1:
        return [sys.argv[1]]
    return ["dotnet", "run", "--project", os.path.join(Author.ROOT, "StradaTool"), "-c", "Release", "--no-launch-profile", "--"]


def fetch(command, kind, identifier, resolution, folder, present):
    if os.path.exists(present):
        print(f"{identifier}: present")
        return
    print(f"{identifier}: downloading")
    subprocess.run(command + [kind, identifier, "--resolution", resolution, "--output", os.path.join(ASSETS, folder)], check=True,
                   cwd=Author.ROOT)


def main():
    command = strada_command()
    fetch(command, "hdri", Author.HDRI, "4k", "Environments", os.path.join(ASSETS, "Environments", f"{Author.HDRI}_4k.hdr"))
    for texture in Author.TEXTURES:
        resolution = Author.texture_resolution(texture)
        fetch(command, "texture", texture, resolution, "Textures", os.path.join(ASSETS, "Textures", texture, f"{texture}_diff_{resolution}.jpg"))
    for model in Author.MODELS:
        fetch(command, "model", model, "1k", "Models", os.path.join(ASSETS, "Models", model))


if __name__ == "__main__":
    main()
