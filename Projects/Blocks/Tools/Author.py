"""Builds the Blocks project through `strada mcp`, the way an agent does: the project and its settings, the C# scripts,
the generated sounds, block mesh and grid, a studio HDRI for reflections, materials, the debris prefab, the game scene and
the test scene; then runs the tests.

    python Projects/Blocks/Tools/Author.py [strada executable]

The project is rebuilt from scratch: everything but this Tools folder and the scripts' sources (Scripts/Source) is
replaced. Without an executable, strada runs from the repository (dotnet run).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(TOOLS)
ROOT = os.path.dirname(os.path.dirname(PROJECT))
SCRIPTS = ["BlocksGame", "BlocksTests", "Debris", "CameraShake"]
STUDIO = "studio_small_09"


class McpClient:
    """A minimal MCP client over stdio."""

    def __init__(self, command):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=ROOT)
        self.counter = 0
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "author", "version": "1"}})
        self.process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        self.process.stdin.flush()

    def request(self, method, params=None):
        self.counter += 1
        message = {"jsonrpc": "2.0", "id": self.counter, "method": method}
        if params is not None:
            message["params"] = params
        self.process.stdin.write((json.dumps(message) + "\n").encode())
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise SystemExit(f"strada mcp ended during {method}: {self.process.stderr.read().decode()}")
        response = json.loads(line)
        if "error" in response:
            raise SystemExit(f"{method} failed: {response['error']}")
        return response["result"]

    def call(self, tool, arguments=None):
        result = self.request("tools/call", {"name": tool, "arguments": arguments or {}})
        text = result["content"][-1]["text"]
        if result["isError"]:
            raise SystemExit(f"{tool} {json.dumps(arguments)} failed: {text}")
        return json.loads(text)

    def close(self):
        self.process.stdin.close()
        return self.process.wait(timeout=120)


def use_engine_sources(csproj):
    """In the engine's repository, builds without the machine's Strada.props (dotnet format in CI, IDEs) take the scripting
    API from its sources; CMake builds pass StradaScriptCoreAssembly."""
    with open(csproj, encoding="utf-8") as file:
        text = file.read()
    reference = '\t<ItemGroup>\n\t\t<Reference Include="Strada.ScriptCore">'
    if reference not in text:
        raise SystemExit(f"{csproj} does not reference Strada.ScriptCore as expected")
    text = text.replace(reference, '\t<ItemGroup Condition="\'$(StradaScriptCoreAssembly)\' != \'\'">\n\t\t<Reference Include="Strada.ScriptCore">')
    text = text.replace("</Project>", """\t<!-- In the engine's repository, builds without Strada.props (dotnet format, IDEs) use the scripting API from source;
\t     CMake builds pass StradaScriptCoreAssembly. -->
\t<ItemGroup Condition="'$(StradaScriptCoreAssembly)' == ''">
\t\t<ProjectReference Include="../../../Strada-ScriptCore/Strada.ScriptCore.csproj">
\t\t\t<Private>false</Private>
\t\t</ProjectReference>
\t</ItemGroup>
</Project>""")
    with open(csproj, "w", encoding="utf-8", newline="\n") as file:
        file.write(text)


def transform(translation, euler=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    return {"Translation": list(translation), "RotationEuler": list(euler), "Scale": list(scale)}


def screen_text(text, position, size, color=(1.0, 1.0, 1.0, 1.0)):
    return {"Transform": transform(position),
            "Text": {"Text": text, "Font": "builtin://DefaultFont", "Color": list(color), "FontSize": size, "ScreenSpace": True}}


def main():
    command = [sys.argv[1]] if len(sys.argv) > 1 else \
        ["dotnet", "run", "--project", os.path.join(ROOT, "StradaTool"), "-c", "Release", "--no-launch-profile", "--"]

    # The editor creates projects in empty directories only: what is kept waits outside meanwhile.
    aside = tempfile.mkdtemp(prefix="blocks-author-")
    sources = os.path.join(aside, "Source")
    shutil.copytree(os.path.join(PROJECT, "Scripts", "Source"), sources)
    shutil.move(TOOLS, os.path.join(aside, "Tools"))
    shutil.rmtree(PROJECT)

    client = McpClient(command + ["mcp", "--headless", "--new"])
    try:
        client.call("project_create", {"directory": PROJECT, "name": "Blocks"})
    finally:
        shutil.move(os.path.join(aside, "Tools"), TOOLS)
    try:
        build(client, sources)
    finally:
        client.close()


def build(client, sources):
    client.call("project_settings", {"settings": {"ScriptModule": "Scripts/Binaries/Blocks.dll",
                                                  "Window": {"Title": "Blocks", "Width": 1280, "Height": 800}}})

    # Scripts: the editor makes the C# project with the first class; the sources (with Board.cs) replace the templates.
    for name in SCRIPTS:
        client.call("script_create", {"className": name})
    for name in os.listdir(sources):
        shutil.copyfile(os.path.join(sources, name), os.path.join(PROJECT, "Scripts", "Source", name))
    use_engine_sources(os.path.join(PROJECT, "Scripts", "Blocks.csproj"))
    built = client.call("script_build")
    if not built["succeeded"]:
        raise SystemExit(f"the scripts did not build: {built}")

    subprocess.run([sys.executable, os.path.join(TOOLS, "GenerateAssets.py")], check=True)
    client.call("asset_refresh")
    studio = client.call("polyhaven_import_hdri", {"id": STUDIO, "resolution": "1k"})["asset"]["reference"]

    client.call("material_create", {"path": "Materials/Frame.smat", "fields": {
        "BaseColor": [0.07, 0.08, 0.11, 1.0], "Metallic": 1.0, "Roughness": 0.22}})
    client.call("material_create", {"path": "Materials/Backdrop.smat", "fields": {
        "BaseColor": [0.015, 0.016, 0.03, 1.0], "Roughness": 0.55, "EmissiveTexture": "asset://Textures/Grid.png",
        "EmissiveColor": [0.25, 0.5, 1.0], "EmissiveIntensity": 0.6, "UVTiling": [10.0, 20.0]}})
    client.call("material_create", {"path": "Materials/Panel.smat", "fields": {
        "BaseColor": [0.015, 0.016, 0.03, 1.0], "Roughness": 0.55, "EmissiveTexture": "asset://Textures/Grid.png",
        "EmissiveColor": [0.25, 0.5, 1.0], "EmissiveIntensity": 0.35, "UVTiling": [5.0, 5.0]}})
    client.call("material_create", {"path": "Materials/Neon.smat", "fields": {
        "BaseColor": [0.0, 0.0, 0.0, 1.0], "EmissiveColor": [0.2, 0.75, 1.0], "EmissiveIntensity": 6.0}})

    # The game scene: the project's start scene, which is open.
    client.call("scene_settings", {"name": "Blocks", "settings": {"Renderer": {
        "Tonemapper": "ACES", "EV100": 0.0, "Shadows": True, "SoftShadows": True, "ShadowDistance": 60.0,
        "AmbientOcclusion": True, "Bloom": True, "BloomIntensity": 0.09, "FXAA": True}}})
    found = {entity["name"]: entity["id"] for entity in client.call("entity_find", {})["entities"]}
    client.call("entity_delete", {"entities": [found[name] for name in ("Floor", "Cube", "Sphere", "Capsule")]})
    camera = found["Main Camera"]
    client.call("component_set", {"entity": camera, "component": "Transform",
                                  "fields": {"Translation": [7.4, 9.2, 31.0], "RotationEuler": [0.0, 0.0, 0.0]}})
    client.call("component_set", {"entity": camera, "component": "Camera", "fields": {"PerspectiveFOV": 45.0}})
    client.call("component_add", {"entity": camera, "component": "Script", "fields": {"ClassName": "Blocks.CameraShake"}})
    client.call("component_set", {"entity": found["Sun"], "component": "Transform", "fields": {"RotationEuler": [-35.0, -32.0, 0.0]}})
    client.call("component_set", {"entity": found["Sun"], "component": "DirectionalLight",
                                  "fields": {"Color": [0.9, 0.93, 1.0], "Intensity": 1.6, "CastShadows": True, "LightSize": 0.8}})
    client.call("component_set", {"entity": found["Sky"], "component": "SkyLight", "fields": {
        "Environment": studio, "Intensity": 0.8, "DrawSkybox": False, "AmbientColor": [0.02, 0.025, 0.05]}})

    def box(name, position, scale, material, parent=None):
        entity = client.call("entity_create", {"name": name, "components": {
            "Transform": transform(position, scale=scale), "Mesh": {"Mesh": "builtin://Cube", "Materials": [f"asset://Materials/{material}.smat"]}}})["id"]
        if parent:
            client.call("entity_reparent", {"entity": entity, "parent": parent})
        return entity

    frame = client.call("entity_create", {"name": "Frame"})["id"]
    box("Left Wall", (-1.0, 9.0, 0.0), (1.0, 21.0, 1.4), "Frame", frame)
    box("Right Wall", (10.0, 9.0, 0.0), (1.0, 21.0, 1.4), "Frame", frame)
    box("Floor", (4.5, -1.0, 0.0), (12.0, 1.0, 1.4), "Frame", frame)
    box("Left Neon", (-0.47, 9.0, 0.7), (0.06, 21.0, 0.06), "Neon", frame)
    box("Right Neon", (9.47, 9.0, 0.7), (0.06, 21.0, 0.06), "Neon", frame)
    box("Floor Neon", (4.5, -0.47, 0.7), (10.0, 0.06, 0.06), "Neon", frame)
    box("Backdrop", (4.5, 9.5, -0.62), (10.0, 20.0, 0.2), "Backdrop")
    box("Next Panel", (13.5, 16.0, -0.9), (4.6, 4.6, 0.2), "Panel")
    box("Next Neon", (13.5, 18.33, -0.8), (4.6, 0.06, 0.06), "Neon")
    box("Next Neon 2", (13.5, 13.67, -0.8), (4.6, 0.06, 0.06), "Neon")
    client.call("entity_create", {"name": "Next Label", "components": {
        "Transform": transform((11.3, 19.45, -0.8)),
        "Text": {"Text": "NEXT", "Font": "builtin://DefaultFont", "FontSize": 0.7, "Color": [0.55, 0.85, 1.0, 1.0]}}})

    client.call("entity_create", {"name": "Title", "components": screen_text("BLOCKS", (0.68, 0.43, 0.0), 84.0, (1.0, 0.78, 0.2, 1.0))})
    score = client.call("entity_create", {"name": "Score", "components": screen_text("", (0.68, 0.56, 0.0), 34.0)})["id"]
    message = client.call("entity_create", {"name": "Message", "components": screen_text("", (0.68, 0.73, 0.0), 34.0,
                                                                                        (1.0, 0.85, 0.3, 1.0))})["id"]
    client.call("entity_create", {"name": "Controls", "components": screen_text(
        "Left / Right   move\nUp   turn      Z   turn back\nDown   faster      Space   drop\nP   pause      R   restart",
        (0.68, 0.86, 0.0), 18.0, (0.7, 0.75, 0.85, 1.0))})

    # Debris: a block that flies off when its row clears.
    debris = client.call("entity_create", {"name": "Debris", "components": {
        "Transform": transform((0.0, -100.0, 0.0), scale=(0.7, 0.7, 0.7)),
        "Mesh": {"Mesh": "asset://Meshes/Block.gltf", "CastShadows": False},
        "RigidBody": {"Type": "Dynamic", "Mass": 1.0},
        "BoxCollider": {"HalfExtents": [0.5, 0.5, 0.5], "Restitution": 0.3},
        "Script": {"ClassName": "Blocks.Debris"}}})["id"]
    debris_prefab = client.call("prefab_create", {"entity": debris, "path": "Prefabs/Debris.sprefab"})["reference"]
    client.call("entity_delete", {"entities": [debris]})

    def clip(name):
        return {"Type": "Asset", "Value": f"asset://Audio/{name}.wav"}

    game_fields = {
        "ScoreText": {"Type": "Entity", "Value": score},
        "MessageText": {"Type": "Entity", "Value": message},
        "Camera": {"Type": "Entity", "Value": camera},
        "FallInterval": {"Type": "Float", "Value": 0.8},
        "PreviewPosition": {"Type": "Vector3", "Value": [12.0, 17.0, 0.0]},
        "BlockMesh": {"Type": "Asset", "Value": "asset://Meshes/Block.gltf"},
        "Debris": {"Type": "Prefab", "Value": debris_prefab},
        "MoveSound": clip("Move"), "RotateSound": clip("Rotate"), "LockSound": clip("Lock"), "DropSound": clip("Drop"),
        "ClearSound": clip("Clear"), "BigClearSound": clip("BigClear"), "LevelUpSound": clip("LevelUp"),
        "GameOverSound": clip("GameOver"),
    }
    game = client.call("entity_create", {"name": "Game", "components": {
        "Script": {"ClassName": "Blocks.BlocksGame", "Fields": game_fields}}})["id"]
    client.call("entity_create", {"name": "Music", "components": {
        "AudioSource": {"Clip": "asset://Audio/Music.wav", "Volume": 0.3, "Loop": True, "PlayOnStart": True, "Spatial": False}}})
    client.call("scene_save")

    # The test scene: the game scene, falling fast, with the checks.
    client.call("scene_save", {"path": "Scenes/Tests.sscene"})
    client.call("scene_settings", {"name": "Tests"})
    game_fields["FallInterval"] = {"Type": "Float", "Value": 0.1}
    client.call("component_set", {"entity": game, "component": "Script", "fields": {"ClassName": "Blocks.BlocksGame", "Fields": game_fields}})
    client.call("entity_create", {"name": "Tests", "components": {
        "Script": {"ClassName": "Blocks.BlocksTests", "Fields": {"Game": {"Type": "Entity", "Value": game}}}}})
    client.call("scene_save")
    client.call("scene_open", {"path": "Scenes/Main.sscene"})

    run = client.call("test_run", {"scene": "Scenes/Tests.sscene", "timeout": 30})
    print("test run:", {key: run[key] for key in ("finished", "timedOut", "frames", "passed", "failed", "exceptions")})
    for result in run["results"]:
        print("  ", "PASS" if result["passed"] else "FAIL", result["name"], result.get("message", ""))
    if run["failed"] or run["exceptions"] or not run["finished"]:
        raise SystemExit("the tests failed")


if __name__ == "__main__":
    main()
