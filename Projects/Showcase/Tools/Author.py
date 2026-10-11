"""Builds the Showcase project through `strada mcp`, the way an agent does: Poly Haven assets (sky, ground, props), the
generated sounds, the C# scripts, materials, the ball prefab, the main scene and the test scene; then runs the tests.
The downloads are not in the repository (FetchAssets.py restores them).

    python Projects/Showcase/Tools/Author.py [strada executable]

The project is rebuilt from scratch: everything but this Tools folder, the .gitignore and the scripts' sources
(Scripts/Source) is replaced. Without an executable, strada runs from the repository (dotnet run).
"""
import json
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(TOOLS)
ROOT = os.path.dirname(os.path.dirname(PROJECT))
SCRIPTS = ["FlyCamera", "Impact", "Projectile", "LanternFlicker", "Playground", "ShowcaseTests"]

HDRI = "autumn_forest_04"
GROUND = "dry_decay_leaves"
CRATE = "wooden_crate_01"
BARREL = "wine_barrel_01"
TABLE = "wooden_picnic_table"
LANTERN = "Lantern_01"
BOULDER = "boulder_01"
LOG = "dead_tree_trunk"
FERN = "fern_02"
BRANCHES = "dry_branches_medium_01"
STUMP = "tree_stump_01"
STUMP_2 = "tree_stump_02"
ROCKS = "rock_moss_set_02"
ROCKS_2 = "rock_moss_set_01"
BIG_LOG = "dead_tree_trunk_02"
MODELS = ("wooden_crate_01", "wine_barrel_01", "wooden_picnic_table", "Lantern_01", "boulder_01", "dead_tree_trunk", "fern_02",
          "dry_branches_medium_01", "tree_stump_01", "tree_stump_02", "rock_moss_set_02", "rock_moss_set_01", "dead_tree_trunk_02")
# The camera and the layout turn by this many degrees around the clearing's center, which puts the late sun of the sky at
# the side of the view: the props are lit from the side instead of from behind.
VIEW_YAW = 85.0


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


# --- glTF bounds: colliders are fitted to the models ------------------------------------------------------------------

def multiply(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def node_matrix(node):
    if "matrix" in node:
        m = node["matrix"]
        return [[m[column * 4 + row] for column in range(4)] for row in range(4)]
    tx, ty, tz = node.get("translation", [0.0, 0.0, 0.0])
    qx, qy, qz, qw = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    sx, sy, sz = node.get("scale", [1.0, 1.0, 1.0])
    rotation = [[1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)],
                [2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)],
                [2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)]]
    scale = (sx, sy, sz)
    return [[rotation[r][0] * scale[0], rotation[r][1] * scale[1], rotation[r][2] * scale[2], (tx, ty, tz)[r]] for r in range(3)] + \
           [[0.0, 0.0, 0.0, 1.0]]


def gltf_bounds(path):
    """The model's bounds in meters (Y up), from the accessors' min and max through the node transforms."""
    with open(path, encoding="utf-8") as file:
        document = json.load(file)
    nodes = document["nodes"]
    scene = document["scenes"][document.get("scene", 0)]
    low = [math.inf] * 3
    high = [-math.inf] * 3
    stack = [(index, [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]) for index in scene["nodes"]]
    while stack:
        index, parent = stack.pop()
        node = nodes[index]
        world = multiply(parent, node_matrix(node))
        if "mesh" in node:
            for primitive in document["meshes"][node["mesh"]]["primitives"]:
                accessor = document["accessors"][primitive["attributes"]["POSITION"]]
                minimum, maximum = accessor["min"], accessor["max"]
                for corner in range(8):
                    point = [maximum[axis] if corner >> axis & 1 else minimum[axis] for axis in range(3)]
                    transformed = [sum(world[row][k] * point[k] for k in range(3)) + world[row][3] for row in range(3)]
                    low = [min(low[axis], transformed[axis]) for axis in range(3)]
                    high = [max(high[axis], transformed[axis]) for axis in range(3)]
        stack.extend((child, world) for child in node.get("children", []))
    return low, high


# --- Scene helpers ----------------------------------------------------------------------------------------------------

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


def placed(translation, euler=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    """A transform of the layout, turned by VIEW_YAW around the clearing's center."""
    angle = math.radians(VIEW_YAW)
    x, y, z = translation
    turned = (x * math.cos(angle) + z * math.sin(angle), y, -x * math.sin(angle) + z * math.cos(angle))
    return transform(turned, (euler[0], euler[1] + VIEW_YAW, euler[2]), scale)


def screen_text(text, position, size, color=(1.0, 1.0, 1.0, 1.0), alignment=None):
    text_fields = {"Text": text, "Font": "builtin://DefaultFont", "Color": list(color), "FontSize": size, "ScreenSpace": True}
    if alignment:
        text_fields["Alignment"] = alignment
    return {"Transform": transform(position), "Text": text_fields}


def main():
    command = [sys.argv[1]] if len(sys.argv) > 1 else \
        ["dotnet", "run", "--project", os.path.join(ROOT, "StradaTool"), "-c", "Release", "--no-launch-profile", "--"]

    # The editor starts before anything is touched: a tool that cannot run leaves the project as it is.
    client = McpClient(command + ["mcp", "--headless", "--new"])
    # The editor creates projects in empty directories only: what is kept waits outside meanwhile.
    aside = tempfile.mkdtemp(prefix="showcase-author-")
    sources = os.path.join(aside, "Source")
    shutil.copytree(os.path.join(PROJECT, "Scripts", "Source"), sources)
    kept = ("Tools", ".gitignore")
    for entry in kept:
        if os.path.exists(os.path.join(PROJECT, entry)):
            shutil.move(os.path.join(PROJECT, entry), os.path.join(aside, entry))
    try:
        shutil.rmtree(PROJECT)
        client.call("project_create", {"directory": PROJECT, "name": "Showcase"})
    except BaseException:
        # The sources come back when the project could not be made (on success the scripts are created from them).
        if not os.path.isdir(os.path.join(PROJECT, "Scripts", "Source")):
            shutil.copytree(sources, os.path.join(PROJECT, "Scripts", "Source"))
        raise
    finally:
        os.makedirs(PROJECT, exist_ok=True)
        for entry in kept:
            if os.path.exists(os.path.join(aside, entry)):
                shutil.move(os.path.join(aside, entry), os.path.join(PROJECT, entry))
    try:
        build(client, sources)
    finally:
        client.close()


def build(client, sources):
    client.call("project_settings", {"settings": {"ScriptModule": "Scripts/Binaries/Showcase.dll",
                                                  "Window": {"Title": "Strada Showcase", "Width": 1600, "Height": 900}}})

    # Scripts: the editor makes the C# project with the first class; the sources replace the templates.
    for name in SCRIPTS:
        client.call("script_create", {"className": name})
    for name in SCRIPTS:
        shutil.copyfile(os.path.join(sources, f"{name}.cs"), os.path.join(PROJECT, "Scripts", "Source", f"{name}.cs"))
    use_engine_sources(os.path.join(PROJECT, "Scripts", "Showcase.csproj"))
    built = client.call("script_build")
    if not built["succeeded"]:
        raise SystemExit(f"the scripts did not build: {built}")

    # Sounds.
    subprocess.run([sys.executable, os.path.join(TOOLS, "GenerateAudio.py")], check=True)
    client.call("asset_refresh")

    # Poly Haven: the sky, the ground and the props.
    sky = client.call("polyhaven_import_hdri", {"id": HDRI, "resolution": "4k"})["asset"]["reference"]
    ground = client.call("polyhaven_import_texture", {"id": GROUND, "resolution": "2k"})["material"]["reference"]
    client.call("material_set", {"material": ground, "fields": {"UVTiling": [18.0, 18.0]}})
    models = {}
    for model in MODELS:
        imported = client.call("polyhaven_import_model", {"id": model, "resolution": "1k"})
        path = os.path.join(PROJECT, "Assets", *imported["mesh"]["path"].split("/"))
        models[model] = (imported["mesh"]["reference"], gltf_bounds(path))
        print(model, "bounds", [[round(v, 3) for v in corner] for corner in models[model][1]])

    client.call("material_create", {"path": "Materials/BallMetal.smat",
                                    "fields": {"BaseColor": [0.95, 0.93, 0.88, 1.0], "Metallic": 1.0, "Roughness": 0.18}})

    # The main scene: the project's start scene, which is open.
    client.call("scene_settings", {"name": "Showcase", "settings": {"Renderer": {
        "Tonemapper": "ACES", "EV100": 0.2, "Shadows": True, "SoftShadows": True, "ShadowDistance": 45.0, "CascadeCount": 4,
        "ShadowMapSize": 4096, "AmbientOcclusion": True, "AmbientOcclusionRadius": 0.8, "AmbientOcclusionIntensity": 1.2,
        "Bloom": True, "BloomIntensity": 0.05, "FXAA": True}}})
    found = {entity["name"]: entity["id"] for entity in client.call("entity_find", {})["entities"]}
    client.call("entity_delete", {"entities": [found[name] for name in ("Floor", "Cube", "Sphere", "Capsule")]})

    camera = found["Main Camera"]
    client.call("component_set", {"entity": camera, "component": "Transform", "fields": placed((0.0, 1.9, 8.2), (-9.0, 0.0, 0.0))})
    client.call("component_set", {"entity": camera, "component": "Camera", "fields": {"PerspectiveFOV": 55.0}})
    client.call("component_set", {"entity": found["Sun"], "component": "Transform", "fields": {"RotationEuler": [-18.0, 160.0, 0.0]}})
    client.call("component_set", {"entity": found["Sun"], "component": "DirectionalLight",
                                  "fields": {"Color": [1.0, 0.78, 0.55], "Intensity": 5.0, "CastShadows": True, "LightSize": 1.2}})
    client.call("component_set", {"entity": found["Sky"], "component": "SkyLight",
                                  "fields": {"Environment": sky, "Intensity": 1.0, "DrawSkybox": True}})

    # The ground ends before the horizon: beyond it, the sky's own forest floor continues it.
    client.call("entity_create", {"name": "Ground", "components": {
        "Transform": transform((0.0, 0.0, 0.0), scale=(76.0, 1.0, 76.0)),
        "Mesh": {"Mesh": "builtin://Plane", "Materials": [ground]},
        "RigidBody": {"Type": "Static"},
        "BoxCollider": {"HalfExtents": [0.5, 0.05, 0.5], "Offset": [0.0, -0.05, 0.0], "Friction": 0.8}}})

    def model_entity(name, model, position, euler=(0.0, 0.0, 0.0), components=None, scale=(1.0, 1.0, 1.0), local=False):
        fields = {"Transform": transform(position, euler, scale) if local else placed(position, euler, scale), "Mesh": {"Mesh": models[model][0]}}
        fields.update(components or {})
        return client.call("entity_create", {"name": name, "components": fields})["id"]

    def static_mesh(name, model, position, euler=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
        return model_entity(name, model, position, euler, {
            "RigidBody": {"Type": "Static"}, "MeshCollider": {"Mesh": models[model][0], "Convex": False, "Friction": 0.7}}, scale)

    # Props: crates in a pyramid and a tower, barrels in a group; every one knocks when hit.
    props = client.call("entity_create", {"name": "Props"})["id"]
    (low, high) = models[CRATE][1]
    width, height, depth = (high[0] - low[0], high[1] - low[1], high[2] - low[2])
    center = [(low[axis] + high[axis]) / 2.0 for axis in range(3)]

    def crate(name, position, yaw=0.0):
        # The mesh's center, turned with the crate, lands on the position.
        angle = math.radians(yaw)
        offset = (center[0] * math.cos(angle) + center[2] * math.sin(angle), -center[0] * math.sin(angle) + center[2] * math.cos(angle))
        entity = model_entity(name, CRATE, (position[0] - offset[0], position[1] - low[1], position[2] - offset[1]), (0.0, yaw, 0.0), {
            "RigidBody": {"Type": "Dynamic", "Mass": 9.0, "AngularDamping": 0.1},
            "BoxCollider": {"HalfExtents": [width / 2.0 - 0.002, height / 2.0, depth / 2.0 - 0.002], "Offset": center,
                            "Friction": 0.65, "Restitution": 0.05},
            "AudioSource": {"Clip": "asset://Audio/WoodHit.wav", "Spatial": True, "MinDistance": 2.0, "MaxDistance": 40.0},
            "Script": {"ClassName": "Showcase.Impact", "Fields": {"Volume": {"Type": "Float", "Value": 0.9}}}})
        client.call("entity_reparent", {"entity": entity, "parent": props})

    gap = 0.015
    for row, count in enumerate((4, 3, 2, 1)):
        for column in range(count):
            x = (column - (count - 1) / 2.0) * (width + gap)
            crate(f"Crate {row + 1}-{column + 1}", (x, row * height, 0.0))
    for level in range(6):
        crate(f"Tower Crate {level + 1}", (-3.0, level * height, -1.0), 90.0 * (level % 2))

    (barrel_low, barrel_high) = models[BARREL][1]
    barrel_height = barrel_high[1] - barrel_low[1]
    barrel_width = max(barrel_high[0] - barrel_low[0], barrel_high[2] - barrel_low[2]) + 0.03
    barrels = ((2.9 - barrel_width, 0), (2.9, 0), (2.9 + barrel_width, 0), (2.9 - barrel_width / 2.0, 1), (2.9 + barrel_width / 2.0, 1))
    for index, (x, level) in enumerate(barrels):
        entity = model_entity(f"Barrel {index + 1}", BARREL, (x, level * barrel_height - barrel_low[1] + 0.002, -1.6), (0.0, 37.0 * index, 0.0), {
            "RigidBody": {"Type": "Dynamic", "Mass": 14.0, "AngularDamping": 0.15},
            "MeshCollider": {"Mesh": models[BARREL][0], "Convex": True, "Friction": 0.6, "Restitution": 0.05},
            "AudioSource": {"Clip": "asset://Audio/WoodHit.wav", "Pitch": 0.8, "Spatial": True, "MinDistance": 2.0, "MaxDistance": 40.0},
            "Script": {"ClassName": "Showcase.Impact", "Fields": {"Volume": {"Type": "Float", "Value": 1.0}}}})
        client.call("entity_reparent", {"entity": entity, "parent": props})

    # Scenery: a picnic table with a lantern, boulders and a fallen trunk around the clearing.
    (table_low, table_high) = models[TABLE][1]
    static_mesh("Picnic Table", TABLE, (4.3, -table_low[1], 2.6), (0.0, -20.0, 0.0))
    (lantern_low, lantern_high) = models[LANTERN][1]
    lantern = model_entity("Lantern", LANTERN, (4.1, table_high[1] - table_low[1] - lantern_low[1], 2.5))
    light = client.call("entity_create", {"name": "Lantern Light", "components": {
        "Transform": transform((0.0, (lantern_high[1] - lantern_low[1]) * 0.45, 0.0)),
        "PointLight": {"Color": [1.0, 0.6, 0.28], "Intensity": 6.0, "Range": 7.0, "CastShadows": True},
        "Script": {"ClassName": "Showcase.LanternFlicker"}}})["id"]
    # Placed in the lantern: its transform is relative to it.
    client.call("entity_reparent", {"entity": light, "parent": lantern, "keepWorldTransform": False})
    static_mesh("Boulder", BOULDER, (-5.6, -models[BOULDER][1][0][1] * 1.8 - 0.2, -3.6), (0.0, 40.0, 0.0), (1.8, 1.8, 1.8))
    static_mesh("Fallen Trunk", LOG, (-4.2, -models[LOG][1][0][1], 2.8), (0.0, 62.0, 0.0))

    # The forest floor around the clearing: rocks and logs that stop balls, plants and branches that do not.
    scenery = client.call("entity_create", {"name": "Scenery"})["id"]
    for name, model, position, yaw, size, solid in (
            ("Mossy Rocks", ROCKS, (0.5, 0.0, -13.0), 8.0, 1.0, True),
            ("Mossy Rocks 2", ROCKS_2, (-12.5, 0.0, -5.0), 70.0, 0.9, True),
            ("Old Trunk", BIG_LOG, (8.5, 0.0, -7.5), -35.0, 1.0, True),
            ("Stump", STUMP, (-1.8, 0.0, -6.8), 20.0, 1.0, True),
            ("Stump 2", STUMP_2, (7.2, 0.0, -2.5), 130.0, 0.9, True),
            ("Fern", FERN, (-2.3, 0.0, 1.4), 15.0, 1.2, False),
            ("Fern 2", FERN, (1.9, 0.0, -3.8), 140.0, 1.3, False),
            ("Fern 3", FERN, (-6.9, 0.0, 0.2), 260.0, 1.4, False),
            ("Fern 4", FERN, (6.6, 0.0, 6.2), 80.0, 1.3, False),
            ("Fern 5", FERN, (-3.4, 0.0, -7.6), 200.0, 1.5, False),
            ("Fern 6", FERN, (9.5, 0.0, 1.5), 300.0, 1.4, False),
            ("Branches", BRANCHES, (0.9, 0.0, 3.2), 30.0, 1.0, False),
            ("Branches 2", BRANCHES, (-2.6, 0.0, -2.9), 120.0, 1.0, False),
            ("Branches 3", BRANCHES, (3.6, 0.0, 0.9), 250.0, 0.9, False)):
        position = (position[0], -models[model][1][0][1] * size, position[2])
        components = {"RigidBody": {"Type": "Static"}, "MeshCollider": {"Mesh": models[model][0], "Convex": False}} if solid else {}
        entity = model_entity(name, model, position, (0.0, yaw, 0.0), components, (size, size, size))
        client.call("entity_reparent", {"entity": entity, "parent": scenery})

    # Undergrowth in a ring around the clearing (seeded, so every build is the same): it fills the distance and breaks the
    # line where the ground meets the sky's trees.
    undergrowth = client.call("entity_create", {"name": "Undergrowth"})["id"]
    generator = random.Random(7)
    for index in range(70):
        model = generator.choices((FERN, BRANCHES, ROCKS_2, STUMP), weights=(10, 4, 1, 1))[0]
        angle = generator.uniform(0.0, 2.0 * math.pi)
        distance = generator.uniform(11.0, 34.0)
        size = generator.uniform(1.1, 2.0) if model == FERN else generator.uniform(0.45, 0.8) if model == ROCKS_2 else generator.uniform(0.8, 1.3)
        position = (math.cos(angle) * distance, -models[model][1][0][1] * size, math.sin(angle) * distance)
        entity = model_entity(f"Undergrowth {index + 1}", model, position, (0.0, generator.uniform(0.0, 360.0), 0.0), {}, (size, size, size))
        client.call("entity_reparent", {"entity": entity, "parent": undergrowth})

    # Sound: music and the forest, everywhere.
    client.call("entity_create", {"name": "Music", "components": {
        "AudioSource": {"Clip": "asset://Audio/Music.wav", "Volume": 0.32, "Loop": True, "PlayOnStart": True, "Spatial": False}}})
    client.call("entity_create", {"name": "Forest Ambience", "components": {
        "AudioSource": {"Clip": "asset://Audio/Ambience.wav", "Volume": 0.55, "Loop": True, "PlayOnStart": True, "Spatial": False}}})

    # The ball the camera throws, as a prefab.
    ball = client.call("entity_create", {"name": "Ball", "components": {
        "Transform": transform((0.0, -50.0, 0.0), scale=(0.24, 0.24, 0.24)),
        "Mesh": {"Mesh": "builtin://Sphere", "Materials": ["asset://Materials/BallMetal.smat"]},
        "RigidBody": {"Type": "Dynamic", "Mass": 5.0, "ContinuousCollision": True, "AngularDamping": 0.2},
        "SphereCollider": {"Radius": 0.5, "Friction": 0.4, "Restitution": 0.35},
        "AudioSource": {"Clip": "asset://Audio/MetalHit.wav", "Spatial": True, "MinDistance": 2.0, "MaxDistance": 40.0},
        "Script": {"ClassName": "Showcase.Impact", "Fields": {"Volume": {"Type": "Float", "Value": 0.7}}}}})["id"]
    prefab = client.call("prefab_create", {"entity": ball, "path": "Prefabs/Ball.sprefab"})["reference"]
    client.call("entity_delete", {"entities": [ball]})

    client.call("component_add", {"entity": camera, "component": "AudioListener"})
    client.call("component_add", {"entity": camera, "component": "AudioSource",
                                  "fields": {"Clip": "asset://Audio/Throw.wav", "Volume": 0.6, "Spatial": False}})
    client.call("component_add", {"entity": camera, "component": "Script", "fields": {
        "ClassName": "Showcase.FlyCamera", "Fields": {"Ball": {"Type": "Prefab", "Value": prefab}}}})

    # The screen: title, controls and the playground's count.
    client.call("entity_create", {"name": "Title", "components": screen_text("STRADA ENGINE  |  Showcase", (0.025, 0.04, 0.0), 30.0,
                                                                            (1.0, 0.86, 0.55, 1.0))})
    client.call("entity_create", {"name": "Controls", "components": screen_text(
        "Right mouse  look      WASD  move      Q / E  down / up      Shift  faster\n"
        "Left click  throw a ball      R  rebuild the stacks", (0.025, 0.91, 0.0), 18.0, (0.92, 0.92, 0.9, 0.9))})
    status = client.call("entity_create", {"name": "Status", "components": screen_text("", (0.025, 0.09, 0.0), 22.0,
                                                                                      (1.0, 1.0, 1.0, 0.95))})["id"]
    playground = client.call("entity_create", {"name": "Playground", "components": {"Script": {
        "ClassName": "Showcase.Playground", "Fields": {
            "Props": {"Type": "Entity", "Value": props}, "Status": {"Type": "Entity", "Value": status}}}}})["id"]
    client.call("scene_save")

    # The test scene: the main scene with the checks.
    client.call("scene_save", {"path": "Scenes/Tests.sscene"})
    client.call("scene_settings", {"name": "Showcase Tests"})
    client.call("entity_create", {"name": "Tests", "components": {"Script": {"ClassName": "Showcase.ShowcaseTests", "Fields": {
        "Playground": {"Type": "Entity", "Value": playground}, "Camera": {"Type": "Entity", "Value": camera},
        "Lantern": {"Type": "Entity", "Value": light}}}}})
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
