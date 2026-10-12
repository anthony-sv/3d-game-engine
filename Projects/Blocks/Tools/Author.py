"""Builds the Blocks project through `strada mcp`, the way an agent does: the project and its settings, the C# scripts,
the generated sounds, block mesh, grid and screen images, a dusk sky from Poly Haven, materials, the debris and particle
burst prefabs, the game scene with its screen (a UI canvas: scores, controls, title, menus, callouts) and the test scene;
then runs the tests.

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


# --- The screen (UI) -------------------------------------------------------------------------------------------------------
# Colors are linear with straight alpha, as the UI components take them. Sizes are canvas units of a 1920 x 1080 design.

GLASS = (0.004, 0.004, 0.016, 0.78)
MENU_GLASS = (0.003, 0.003, 0.012, 0.93)
CAPTION = (0.32, 0.37, 0.55, 1.0)
VALUE = (0.96, 0.97, 1.0, 1.0)
GOLD = (1.0, 0.68, 0.16, 1.0)
DIVIDER = (1.0, 1.0, 1.0, 0.07)
LABEL = (0.92, 0.94, 1.0, 1.0)
DARK_TEXT = (0.004, 0.008, 0.035, 1.0)
SHADOW = (0.0, 0.0, 0.04, 0.6)
# The rest colors of the menus' buttons: the main one blue, the others dark (the focused one lights up, see Menu.cs).
PRIMARY = (0.015, 0.2, 0.55, 1.0)
SECONDARY = (0.012, 0.014, 0.04, 0.95)
FOCUS = (0.06, 0.62, 1.0, 1.0)
# Keys drawn as keycaps: a dark side under a lighter top.
KEY_SIDE = (0.003, 0.003, 0.01, 1.0)
KEY_TOP = (0.045, 0.05, 0.095, 1.0)
KEY_TEXT = (0.8, 0.84, 0.95, 1.0)
# The pieces' colors (BlocksGame.s_Colors) for the title's block letters.
TILES = (("B", (0.1, 0.85, 0.95)), ("L", (1.0, 0.82, 0.1)), ("O", (0.7, 0.25, 1.0)), ("C", (0.2, 0.9, 0.3)),
         ("K", (1.0, 0.2, 0.25)), ("S", (1.0, 0.5, 0.1)))
# Text widths for laying out rows of keys and words: about this many times the font size per letter, in capitals.
LETTER_WIDTH = 0.56


def ui_rect(anchor_min, anchor_max, pivot, position, size, visible=True, opacity=1.0):
    return {"AnchorMin": list(anchor_min), "AnchorMax": list(anchor_max), "Pivot": list(pivot), "Position": list(position),
            "Size": list(size), "Visible": visible, "Opacity": opacity}


def at(anchor, pivot, position, size, **flags):
    """A fixed size, its pivot placed at a point of the parent."""
    return ui_rect(anchor, anchor, pivot, position, size, **flags)


def fill(left=0.0, top=0.0, right=0.0, bottom=0.0, **flags):
    """The parent's rectangle, inset by margins."""
    return ui_rect((0.0, 0.0), (1.0, 1.0), (0.5, 0.5), ((left - right) / 2.0, (top - bottom) / 2.0), (-(left + right), -(top + bottom)),
                   **flags)


def row(top, height, left=0.0, right=0.0):
    """As wide as the parent less margins, at a distance from its top."""
    return ui_rect((0.0, 0.0), (1.0, 0.0), (0.5, 0.0), ((left - right) / 2.0, top), (-(left + right), height))


def image(color, texture=None, radius=0.0):
    fields = {"Color": list(color), "CornerRadius": radius}
    if texture:
        fields["Texture"] = f"asset://Textures/{texture}.png"
    return fields


def text(value, size, color, alignment="Center", vertical="Middle"):
    return {"Text": value, "FontSize": size, "Color": list(color), "Alignment": alignment, "VerticalAlignment": vertical, "Wrap": False}


def text_width(value, size):
    return len(value) * LETTER_WIDTH * size


def key_width(label):
    """A keycap's width: square for a letter or an arrow (labels ArrowUp, ...), wider for a word."""
    return 38.0 if label.startswith("Arrow") else max(38.0, text_width(label, 16.0) + 22.0)


def build_hud(client):
    """Builds the game's screen on a canvas (Hud.cs finds its elements by name and brings them to life) and returns the
    canvas. In the scene view the canvas stands above the board."""
    def element(name, parent, rect, image_fields=None, text_fields=None, button=None):
        components = {"UIRect": rect}
        if image_fields is not None:
            components["UIImage"] = image_fields
        if text_fields is not None:
            components["UIText"] = text_fields
        if button is not None:
            components["UIButton"] = button
        return client.call("entity_create", {"name": name, "parent": parent, "components": components})["id"]

    def panel(name, parent, rect, color=GLASS):
        """A rounded pane of dark glass with a strip of the accent along its top."""
        pane = element(name, parent, rect, image(color, radius=22.0))
        element(f"{name} Accent", pane, row(12.0, 5.0, 26.0, 26.0), image((1.0, 1.0, 1.0, 1.0), "Accent", 2.5))
        return pane

    def key(name, parent, label, x, row_height):
        """A keycap at a distance from the parent's left edge: a word or letter, or an arrow (label ArrowUp...). Returns its
        width."""
        arrow = label.startswith("Arrow")
        width = key_width(label)
        cap = element(name, parent, at((0.0, 0.5), (0.0, 0.5), (x, 0.0), (width, row_height)), image(KEY_SIDE, radius=7.0))
        top = element(f"{name} Top", cap, fill(bottom=4.0), image(KEY_TOP, radius=7.0))
        if arrow:
            element(f"{name} Icon", top, at((0.5, 0.5), (0.5, 0.5), (0.0, 0.0), (15.0, 15.0)), image(KEY_TEXT, label))
        else:
            element(f"{name} Text", top, fill(), text_fields=text(label, 16.0, KEY_TEXT))
        return width

    def hint(name, parent, rect_for_size, items, plate=False):
        """A row of keys and what they do, centered: items are (keys, words). On a plate of dark glass, it reads over the
        scene."""
        size, cap, gap, spacing = 17.0, 28.0, 6.0, 22.0
        padding = (18.0, 7.0) if plate else (0.0, 0.0)
        widths = []
        for keys, words in items:
            keys_width = sum(key_width(k) for k in keys)
            widths.append(keys_width + gap * (len(keys) - 1) + 10.0 + text_width(words, size))
        total = sum(widths) + spacing * (len(items) - 1)
        height = cap + (2.0 * padding[1])
        line = element(name, parent, rect_for_size(total + (2.0 * padding[0]), height),
                       image(GLASS, radius=height / 2.0) if plate else None)
        x = padding[0]
        for index, (keys, words) in enumerate(items):
            for number, k in enumerate(keys):
                x += key(f"{name} Key {index + 1}.{number + 1}", line, k, x, cap) + gap
            x += 10.0 - gap
            element(f"{name} Words {index + 1}", line, at((0.0, 0.5), (0.0, 0.5), (x, 0.0), (text_width(words, size) + 4.0, cap)),
                    text_fields=text(words, size, CAPTION, "Left"))
            x += text_width(words, size) + spacing
        return line

    def button(name, parent, center, size, label, label_size, primary):
        """A menu button over its glow (Menu.cs lights it up when it has the focus)."""
        anchor, pivot = (0.5, 0.0), (0.5, 0.5)
        element(f"{name} Glow", parent, at(anchor, pivot, center, (size[0] + 90.0, size[1] + 90.0), opacity=0.0),
                image(FOCUS[:3] + (0.75,), "Glow"))
        rest = PRIMARY if primary else SECONDARY
        entity = element(name, parent, at(anchor, pivot, center, size), image(rest, radius=size[1] * 0.25),
                         button={"NormalColor": list(rest), "HoveredColor": list(FOCUS), "PressedColor": [0.02, 0.3, 0.62, 1.0],
                                 "DisabledColor": list(rest)})
        element(f"{name} Label", entity, fill(), text_fields=text(label, label_size, LABEL))
        return entity

    canvas = client.call("entity_create", {"name": "HUD", "components": {
        "Transform": transform((4.5, 21.6, -1.2)),
        "UICanvas": {"ScaleMode": "ScaleWithScreenSize", "ReferenceResolution": [1920.0, 1080.0]}}})["id"]

    # HOLD and NEXT over the board's panels: a canvas of its own whose units are pixels, where the game places them as the
    # camera shows the panels (here where a 1920 x 1080 view shows them during a game).
    labels = client.call("entity_create", {"name": "Board Labels", "parent": canvas, "components": {
        "UICanvas": {"ScaleMode": "ConstantPixelSize"}}})["id"]
    for name, x in (("Hold", 664.0), ("Next", 1256.0)):
        label = element(f"{name} Label", labels, at((0.0, 0.0), (0.5, 0.5), (x, 92.0), (190.0, 54.0)))
        element(f"{name} Label Shadow", label, fill(), text_fields=text(name.upper(), 30.0, SHADOW))
        element(f"{name} Label Text", label, fill(), text_fields=text(name.upper(), 30.0, (0.45, 0.86, 1.0, 1.0)))

    # The score panel, top right: the score, the level and the lines (with a bar of the rows toward the next level) and the
    # best score; DEMO while the title's demo plays.
    scores = panel("Score Panel", canvas, at((1.0, 0.0), (1.0, 0.0), (-36.0, 72.0), (324.0, 420.0)))
    element("Score Caption", scores, row(30.0, 26.0, 24.0, 24.0), text_fields=text("SCORE", 21.0, CAPTION))
    element("Score Value", scores, row(58.0, 76.0, 12.0, 12.0), text_fields=text("0", 64.0, VALUE))
    element("Score Divider", scores, row(146.0, 2.0, 28.0, 28.0), image(DIVIDER))
    for name, x in (("Level", 0.25), ("Lines", 0.75)):
        element(f"{name} Caption", scores, at((x, 0.0), (0.5, 0.0), (0.0, 166.0), (150.0, 24.0)), text_fields=text(name.upper(), 19.0, CAPTION))
        element(f"{name} Value", scores, at((x, 0.0), (0.5, 0.0), (0.0, 192.0), (150.0, 58.0)), text_fields=text("0", 46.0, VALUE))
    bar = element("Level Bar", scores, row(262.0, 8.0, 32.0, 32.0), image((1.0, 1.0, 1.0, 0.08), radius=4.0))
    element("Level Bar Fill", bar, ui_rect((0.0, 0.0), (0.0, 1.0), (0.0, 0.5), (0.0, 0.0), (0.0, 0.0), visible=False),
            image((1.0, 1.0, 1.0, 1.0), "Accent", 4.0))
    element("Best Divider", scores, row(292.0, 2.0, 28.0, 28.0), image(DIVIDER))
    element("Best Caption", scores, row(310.0, 26.0, 24.0, 24.0), text_fields=text("BEST", 19.0, CAPTION))
    element("Best Value", scores, row(338.0, 58.0, 12.0, 12.0), text_fields=text("0", 44.0, GOLD))
    demo = element("Demo Tag", scores, at((1.0, 0.0), (1.0, 0.0), (-18.0, 26.0), (86.0, 28.0)), image((0.75, 0.04, 0.38, 1.0), radius=14.0))
    element("Demo Tag Text", demo, fill(), text_fields=text("DEMO", 16.0, VALUE))

    # The controls, bottom left: keycaps and what they do.
    controls = panel("Controls Panel", canvas, at((0.0, 1.0), (0.0, 1.0), (36.0, -56.0), (372.0, 390.0)))
    element("Controls Caption", controls, row(30.0, 26.0, 24.0, 24.0), text_fields=text("CONTROLS", 21.0, CAPTION))
    rows = (("Move", ("ArrowLeft", "ArrowRight"), "MOVE"), ("Soft Drop", ("ArrowDown",), "SOFT DROP"),
            ("Rotate", ("ArrowUp", "X"), "ROTATE"), ("Rotate Back", ("Z",), "ROTATE BACK"), ("Hard Drop", ("SPACE",), "HARD DROP"),
            ("Hold", ("C", "SHIFT"), "HOLD"), ("Pause", ("ESC",), "PAUSE"))
    for index, (name, keys, action) in enumerate(rows):
        line = element(f"{name} Control", controls, row(72.0 + (44.0 * index), 34.0, 24.0, 24.0))
        x = 0.0
        for number, k in enumerate(keys):
            x += key(f"{name} Key {number + 1}", line, k, x, 34.0) + 6.0
        element(f"{name} Action", line, at((0.0, 0.5), (0.0, 0.5), (132.0, 0.0), (190.0, 34.0)),
                text_fields=text(action, 20.0, (0.72, 0.76, 0.9, 1.0), "Left"))

    # Flashes light up the view for a moment.
    element("Flash", canvas, fill(visible=False, opacity=0.0), image((1.0, 1.0, 1.0, 1.0)))

    # The callouts: a column of slots over the board's middle (Callouts.cs), each a line and a smaller one under it, over
    # their shadows.
    column = element("Callouts", canvas, at((0.5, 0.47), (0.5, 0.5), (0.0, 0.0), (0.0, 0.0)))
    for slot in ("Bonus", "Clear", "Level"):
        group = element(f"{slot} Callout", column, at((0.5, 0.5), (0.5, 0.5), (0.0, 0.0), (0.0, 0.0), opacity=0.0))
        for part in ("Text Shadow", "Text", "Detail Shadow", "Detail"):
            element(f"{slot} Callout {part}", group, at((0.5, 0.5), (0.5, 0.0), (0.0, 0.0), (1600.0, 80.0)),
                    text_fields=text("", 80.0, VALUE, vertical="Top"))

    # The menus dim the game under them.
    element("Dim", canvas, fill(visible=False, opacity=0.0), image((0.0, 0.0, 0.012, 1.0)))

    # The title: the game's name in block letters over a glow, a subtitle, and Play and Quit at the bottom, over the floor in
    # front of the board the demo plays on.
    title = element("Title Screen", canvas, fill())
    tile, gap = 128.0, 14.0
    logo_width = (len(TILES) * tile) + ((len(TILES) - 1) * gap)
    element("Logo Glow", title, at((0.5, 0.0), (0.5, 0.0), (0.0, -26.0), (logo_width + 300.0, tile + 150.0)),
            image((0.32, 0.08, 1.0, 0.42), "Glow"))
    logo = element("Logo", title, at((0.5, 0.0), (0.5, 0.0), (0.0, 46.0), (logo_width, tile)))
    for index, (letter, color) in enumerate(TILES):
        side = tuple(c * 0.4 for c in color) + (1.0,)
        block = element(f"Tile {letter}", logo, at((0.0, 0.0), (0.0, 0.0), (index * (tile + gap), 0.0), (tile, tile)), image(side, radius=22.0))
        face = element(f"Tile {letter} Face", block, fill(7.0, 6.0, 7.0, 14.0), image(color + (1.0,), radius=17.0))
        element(f"Tile {letter} Gloss", face, ui_rect((0.0, 0.0), (1.0, 0.5), (0.5, 0.0), (0.0, 5.0), (-12.0, 0.0)),
                image((1.0, 1.0, 1.0, 1.0), "Gloss", 13.0))
        element(f"Tile {letter} Letter", face, fill(), text_fields=text(letter, 90.0, DARK_TEXT))
    subtitle = element("Subtitle", title, at((0.5, 0.0), (0.5, 0.0), (0.0, 46.0 + tile + 26.0), (470.0, 42.0)), image(GLASS, radius=21.0))
    element("Subtitle Text", subtitle, fill(), text_fields=text("A STRADA ENGINE EXAMPLE", 20.0, (0.78, 0.82, 1.0, 1.0)))
    menu = element("Title Menu", title, at((0.5, 1.0), (0.5, 1.0), (0.0, -78.0), (600.0, 84.0)))
    button("Play Button", menu, (-130.0, 42.0), (340.0, 84.0), "PLAY", 40.0, True)
    button("Quit Button", menu, (182.0, 42.0), (236.0, 84.0), "QUIT", 32.0, False)
    hint("Title Hint", title, lambda width, height: at((0.5, 1.0), (0.5, 1.0), (0.0, -22.0), (width, height)),
         ((("ArrowLeft", "ArrowRight"), "SELECT"), (("ENTER",), "CONFIRM")), plate=True)

    # The pause menu: Resume, Restart, Quit (back to the title).
    pause = panel("Pause Menu", canvas, at((0.5, 0.5), (0.5, 0.5), (0.0, 0.0), (500.0, 456.0), visible=False, opacity=0.0), MENU_GLASS)
    element("Pause Title", pause, row(36.0, 80.0, 24.0, 24.0), text_fields=text("PAUSED", 64.0, VALUE))
    button("Resume Button", pause, (0.0, 168.0), (404.0, 72.0), "RESUME", 32.0, True)
    button("Restart Button", pause, (0.0, 256.0), (404.0, 72.0), "RESTART", 30.0, False)
    button("Pause Quit Button", pause, (0.0, 344.0), (404.0, 72.0), "QUIT", 30.0, False)
    hint("Pause Hint", pause, lambda width, height: at((0.5, 1.0), (0.5, 1.0), (0.0, -26.0), (width, height)),
         ((("ArrowUp", "ArrowDown"), "SELECT"), (("ENTER",), "CONFIRM"), (("ESC",), "RESUME")))

    # The game-over screen: the score (with a tag for a new best), the level and lines reached, Play Again and Quit.
    over = panel("Game Over", canvas, at((0.5, 0.5), (0.5, 0.5), (0.0, 0.0), (540.0, 590.0), visible=False, opacity=0.0), MENU_GLASS)
    element("Game Over Title", over, row(36.0, 80.0, 24.0, 24.0), text_fields=text("GAME OVER", 64.0, VALUE))
    best = element("New Best Tag", over, at((0.5, 0.0), (0.5, 0.0), (0.0, 122.0), (196.0, 36.0), visible=False), image(GOLD, radius=18.0))
    element("New Best Text", best, fill(), text_fields=text("NEW BEST!", 20.0, DARK_TEXT))
    element("Final Caption", over, row(170.0, 26.0, 24.0, 24.0), text_fields=text("SCORE", 21.0, CAPTION))
    element("Final Score", over, row(196.0, 92.0, 12.0, 12.0), text_fields=text("0", 84.0, VALUE))
    element("Final Stats", over, row(290.0, 30.0, 24.0, 24.0), text_fields=text("", 22.0, CAPTION))
    button("Play Again Button", over, (0.0, 382.0), (440.0, 76.0), "PLAY AGAIN", 32.0, True)
    button("Game Over Quit Button", over, (0.0, 472.0), (440.0, 64.0), "QUIT", 28.0, False)
    hint("Game Over Hint", over, lambda width, height: at((0.5, 1.0), (0.5, 1.0), (0.0, -24.0), (width, height)),
         ((("ArrowUp", "ArrowDown"), "SELECT"), (("ENTER",), "CONFIRM")))
    return canvas


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
    # header strip above the area its pieces use, where the screen's HOLD or NEXT label stands, so it reads against the dark
    # panel, not the sky. The label follows the strip as the camera shows it: an empty entity in the middle of the strip's
    # face, scaled to the strip's size, marks it.
    def panel(name, center, size):
        x, y = center
        width, height = size
        header = 1.3
        top = y + (height / 2.0) + header
        box(f"{name} Panel", (x, y + (header / 2.0), -0.9), (width, height + header, 0.2), "Panel")
        box(f"{name} Neon", (x, top, -0.8), (width, 0.06, 0.06), "Neon")
        box(f"{name} Neon 2", (x, y - (height / 2.0), -0.8), (width, 0.06, 0.06), "Neon")
        box(f"{name} Neon 3", (x, top - header, -0.8), (width, 0.035, 0.035), "Neon")
        return client.call("entity_create", {"name": f"{name} Header", "components": {
            "Transform": transform((x, top - (header / 2.0), -0.8), scale=(width, header, 1.0))}})["id"]

    hold_header = panel("Hold", (-4.8, 16.6), (4.6, 4.6))
    next_header = panel("Next", (13.8, 13.2), (4.6, 11.4))

    # The screen: scores, the controls, the labels over the panels, the title, the menus and the callouts.
    hud = build_hud(client)

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
        "HudCanvas": {"Type": "Entity", "Value": hud},
        "HoldHeader": {"Type": "Entity", "Value": hold_header},
        "NextHeader": {"Type": "Entity", "Value": next_header},
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
