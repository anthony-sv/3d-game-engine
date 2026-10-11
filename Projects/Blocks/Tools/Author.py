"""Builds the Blocks project through `strada mcp`, the way an agent does: the project and its settings, the C# scripts,
the generated sounds, block mesh and grid, a dusk sky from Poly Haven, materials, the debris and particle burst prefabs,
the game scene and the test scene; then runs the tests.

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
SKY = "industrial_sunset_02_puresky"


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


def main():
    command = [sys.argv[1]] if len(sys.argv) > 1 else \
        ["dotnet", "run", "--project", os.path.join(ROOT, "StradaTool"), "-c", "Release", "--no-launch-profile", "--"]

    # The editor starts before anything is touched: a tool that cannot run leaves the project as it is.
    client = McpClient(command + ["mcp", "--headless", "--new"])
    # The editor creates projects in empty directories only: what is kept waits outside meanwhile.
    aside = tempfile.mkdtemp(prefix="blocks-author-")
    sources = os.path.join(aside, "Source")
    shutil.copytree(os.path.join(PROJECT, "Scripts", "Source"), sources)
    shutil.move(TOOLS, os.path.join(aside, "Tools"))
    try:
        shutil.rmtree(PROJECT)
        client.call("project_create", {"directory": PROJECT, "name": "Blocks"})
    except BaseException:
        # The sources come back when the project could not be made (on success the scripts are created from them).
        if not os.path.isdir(os.path.join(PROJECT, "Scripts", "Source")):
            shutil.copytree(sources, os.path.join(PROJECT, "Scripts", "Source"))
        raise
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
    sky = client.call("polyhaven_import_hdri", {"id": SKY, "resolution": "1k"})["asset"]["reference"]

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
    # The floor under the board runs to the horizon: a dusky plane with glowing grid lines, light enough for the sun to
    # light it and for the shadows of the board and its blocks to read.
    client.call("material_create", {"path": "Materials/Floor.smat", "fields": {
        "BaseColor": [0.15, 0.115, 0.21, 1.0], "Metallic": 0.1, "Roughness": 0.58, "EmissiveTexture": "asset://Textures/Grid.png",
        "EmissiveColor": [0.55, 0.3, 1.0], "EmissiveIntensity": 1.2, "UVTiling": [160.0, 160.0]}})

    # The game scene: the project's start scene, which is open.
    client.call("scene_settings", {"name": "Blocks", "settings": {"Renderer": {
        "Tonemapper": "ACES", "EV100": 0.0, "Shadows": True, "SoftShadows": True, "ShadowDistance": 80.0,
        "AmbientOcclusion": True, "Bloom": True, "BloomIntensity": 0.09, "FXAA": True, "Saturation": 1.1, "Vignette": 0.3,
        "VignetteSmoothness": 0.6}}})
    found = {entity["name"]: entity["id"] for entity in client.call("entity_find", {})["entities"]}
    client.call("entity_delete", {"entities": [found[name] for name in ("Floor", "Cube", "Sphere", "Capsule")]})
    camera = found["Main Camera"]
    # Above the board and looking down at it: the floor in front of the board shows, with the shadows of the stack and of
    # the falling piece.
    client.call("component_set", {"entity": camera, "component": "Transform",
                                  "fields": {"Translation": [4.5, 15.0, 38.0], "RotationEuler": [-11.0, 0.0, 0.0]}})
    client.call("component_set", {"entity": camera, "component": "Camera", "fields": {"PerspectiveFOV": 42.0, "PerspectiveFar": 400.0}})
    # On the title screen the camera stands further back and higher: the title's sign fits above the board and the panels,
    # the prompt below them, over the floor.
    attract_view = client.call("entity_create", {"name": "Attract View", "components": {
        "Transform": transform((4.5, 26.0, 44.0), euler=(-18.0, 0.0, 0.0))}})["id"]
    # The camera shakes and kicks with the game; on the title screen it glides to the attract view and sways around the
    # board's middle.
    client.call("component_add", {"entity": camera, "component": "Script", "fields": {"ClassName": "Blocks.CameraShake", "Fields": {
        "SwayPivot": {"Type": "Vector3", "Value": [4.5, 9.5, 0.0]}, "AttractView": {"Type": "Entity", "Value": attract_view}}}})
    # A warm sun high behind the board, on the side where the sky glows: the frame, the stack, the falling piece and the
    # panels' lower edges cast their shadows onto the floor in front of the board, where the camera sees them. A cool light
    # from the front, without shadows, keeps the blocks' faces bright.
    client.call("component_set", {"entity": found["Sun"], "component": "Transform", "fields": {"RotationEuler": [-56.0, 165.0, 0.0]}})
    client.call("component_set", {"entity": found["Sun"], "component": "DirectionalLight",
                                  "fields": {"Color": [1.0, 0.72, 0.46], "Intensity": 3.6, "CastShadows": True, "LightSize": 0.5}})
    client.call("entity_create", {"name": "Fill Light", "components": {
        "Transform": transform((0.0, 0.0, 0.0), euler=(-20.0, 15.0, 0.0)),
        "DirectionalLight": {"Color": [0.55, 0.68, 1.0], "Intensity": 0.8, "CastShadows": False}}})
    # The sky's sunset glows on the horizon behind the board.
    client.call("component_set", {"entity": found["Sky"], "component": "SkyLight", "fields": {
        "Environment": sky, "Intensity": 0.5, "Rotation": 10.0, "DrawSkybox": True, "SkyboxBlur": 0.05}})
    client.call("entity_create", {"name": "Ground", "components": {
        "Transform": transform((4.5, -1.5, -100.0), scale=(320.0, 1.0, 320.0)),
        "Mesh": {"Mesh": "builtin://Plane", "Materials": ["asset://Materials/Floor.smat"]}}})

    def box(name, position, scale, material, parent=None, cast_shadows=True):
        entity = client.call("entity_create", {"name": name, "components": {
            "Transform": transform(position, scale=scale),
            "Mesh": {"Mesh": "builtin://Cube", "Materials": [f"asset://Materials/{material}.smat"], "CastShadows": cast_shadows}}})["id"]
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
    # The sun shines through the board's backdrop, so the floor shows the shadow of the stack, cell by cell.
    box("Backdrop", (4.5, 9.5, -0.62), (10.0, 20.0, 0.2), "Backdrop", cast_shadows=False)
    # The held piece's panel left of the board, the next queue's right of it; the game draws their pieces. Each panel has a
    # header strip with its label above the area its pieces use, so the label reads against the dark panel, not the sky.
    def panel(name, center, size):
        x, y = center
        width, height = size
        header = 1.3
        top = y + (height / 2.0) + header
        box(f"{name} Panel", (x, y + (header / 2.0), -0.9), (width, height + header, 0.2), "Panel")
        box(f"{name} Neon", (x, top, -0.8), (width, 0.06, 0.06), "Neon")
        box(f"{name} Neon 2", (x, y - (height / 2.0), -0.8), (width, 0.06, 0.06), "Neon")
        client.call("entity_create", {"name": f"{name} Label", "components": {
            "Transform": transform((x, top - 0.25, -0.75)),
            "Text": {"Text": name.upper(), "Font": "builtin://DefaultFont", "FontSize": 0.8, "Color": [0.65, 0.9, 1.0, 1.0],
                     "Alignment": "Center"}}})

    panel("Hold", (-4.8, 16.6), (4.6, 4.6))
    panel("Next", (13.8, 13.2), (4.6, 11.4))

    # Debris: a block that flies off when its row clears.
    debris = client.call("entity_create", {"name": "Debris", "components": {
        "Transform": transform((0.0, -100.0, 0.0), scale=(0.7, 0.7, 0.7)),
        "Mesh": {"Mesh": "asset://Meshes/Block.gltf"},
        "RigidBody": {"Type": "Dynamic", "Mass": 1.0},
        "BoxCollider": {"HalfExtents": [0.5, 0.5, 0.5], "Restitution": 0.3},
        "Script": {"ClassName": "Blocks.Debris"}}})["id"]
    debris_prefab = client.call("prefab_create", {"entity": debris, "path": "Prefabs/Debris.sprefab"})["reference"]
    client.call("entity_delete", {"entities": [debris]})

    # Particle bursts: sparks along a cleared row, and dust where a hard drop lands. Both burst once and go away.
    def burst(name, settings):
        settings.update({"Emitting": False, "DestroyWhenFinished": True, "Shape": "Box", "ConeAngle": 180.0})
        entity = client.call("entity_create", {"name": name, "components": {
            "Transform": transform((0.0, -100.0, 0.0)), "ParticleSystem": settings}})["id"]
        reference = client.call("prefab_create", {"entity": entity, "path": f"Prefabs/{name}.sprefab"})["reference"]
        client.call("entity_delete", {"entities": [entity]})
        return reference

    row_burst = burst("RowBurst", {
        "Burst": 120, "MaxParticles": 200, "BoxHalfExtents": [5.0, 0.35, 0.3], "LifetimeMin": 0.45, "LifetimeMax": 1.0,
        "SpeedMin": 2.0, "SpeedMax": 7.0, "GravityFactor": 0.9, "Drag": 1.2, "StartSize": 0.2, "EndSize": 0.02,
        "StartColor": [1.0, 1.0, 1.0, 1.0], "EndColor": [0.3, 0.8, 1.0, 0.0], "Intensity": 5.0})
    drop_burst = burst("DropBurst", {
        "Burst": 30, "MaxParticles": 40, "BoxHalfExtents": [1.0, 0.05, 0.3], "LifetimeMin": 0.25, "LifetimeMax": 0.55,
        "SpeedMin": 1.0, "SpeedMax": 3.5, "GravityFactor": 0.4, "Drag": 2.5, "StartSize": 0.28, "EndSize": 0.02,
        "StartColor": [0.75, 0.85, 1.0, 0.8], "EndColor": [0.5, 0.6, 1.0, 0.0], "Intensity": 2.0})

    def clip(name):
        return {"Type": "Asset", "Value": f"asset://Audio/{name}.wav"}

    game_fields = {
        "Camera": {"Type": "Entity", "Value": camera},
        "FallInterval": {"Type": "Float", "Value": 0.8},
        "NextPosition": {"Type": "Vector3", "Value": [13.8, 16.6, 0.0]},
        "NextSpacing": {"Type": "Float", "Value": 3.4},
        "HoldPosition": {"Type": "Vector3", "Value": [-4.8, 16.6, 0.0]},
        "BlockMesh": {"Type": "Asset", "Value": "asset://Meshes/Block.gltf"},
        "Debris": {"Type": "Prefab", "Value": debris_prefab},
        "RowBurst": {"Type": "Prefab", "Value": row_burst},
        "DropBurst": {"Type": "Prefab", "Value": drop_burst},
        "MoveSound": clip("Move"), "RotateSound": clip("Rotate"), "HoldSound": clip("Hold"), "SoftDropSound": clip("SoftDrop"),
        "LockSound": clip("Lock"), "DropSound": clip("Drop"), "SingleSound": clip("Single"), "DoubleSound": clip("Double"),
        "TripleSound": clip("Triple"), "TetrisSound": clip("Tetris"), "LevelUpSound": clip("LevelUp"),
        "GameOverSound": clip("GameOver"), "StartSound": clip("Start"), "PauseSound": clip("Pause"),
    }
    game = client.call("entity_create", {"name": "Game", "components": {
        "Script": {"ClassName": "Blocks.BlocksGame", "Fields": game_fields}}})["id"]
    client.call("entity_create", {"name": "Music", "components": {
        "AudioSource": {"Clip": "asset://Audio/Music.wav", "Volume": 0.3, "Loop": True, "PlayOnStart": True, "Spatial": False}}})
    client.call("scene_save")

    # The test scene: the game scene, starting at once and falling fast, with the checks; it keeps no best score, and its
    # demo plays a fixed order of pieces (one whose first piece the auto player puts aside).
    client.call("scene_save", {"path": "Scenes/Tests.sscene"})
    client.call("scene_settings", {"name": "Tests"})
    game_fields["FallInterval"] = {"Type": "Float", "Value": 0.1}
    game_fields["StartOnTitle"] = {"Type": "Bool", "Value": False}
    game_fields["SaveBestScore"] = {"Type": "Bool", "Value": False}
    game_fields["DemoSeed"] = {"Type": "Int32", "Value": 1}
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
