# Getting started

This guide takes you from a fresh checkout to a game of your own: build the engine, look at the sample projects, make
a small game in the editor with physics, a material, a visual script, a C# script and a HUD, test it, and export it as
a game that runs without the editor. Strada has no installer or release yet: you build it from source.

## Contents

- [What you need](#what-you-need)
- [Build the engine](#build-the-engine)
- [A first look](#a-first-look)
  - [Start the editor](#start-the-editor)
  - [Open the samples](#open-the-samples)
  - [The Showcase's controls](#the-showcases-controls)
  - [What each sample shows](#what-each-sample-shows)
  - [Run a project with the player](#run-a-project-with-the-player)
- [Your first game](#your-first-game)
  - [Create a project](#create-a-project)
  - [Build the scene](#build-the-scene)
  - [Give it a material](#give-it-a-material)
  - [Make something happen with a visual script](#make-something-happen-with-a-visual-script)
  - [The same in C#](#the-same-in-c)
  - [Add a HUD](#add-a-hud)
  - [Play, pause and step](#play-pause-and-step)
  - [Save](#save)
- [Test the game](#test-the-game)
- [Export the game](#export-the-game)
- [Work with an AI agent](#work-with-an-ai-agent)
- [Where to go next](#where-to-go-next)

## What you need

| | Windows | macOS | Linux (Ubuntu 24.04+) |
|---|---|---|---|
| Compiler | Visual Studio 2022 17.10+ or 2026 (C++ workload) | Xcode 15+ | GCC 13+ or Clang 17+ |
| CMake and Ninja | bundled with Visual Studio | `brew install cmake ninja` | `apt install cmake ninja-build` |
| Other | Python 3.10+ | Python 3.10+ | Python 3.10+, `apt install pkg-config libwayland-dev libxkbcommon-dev xorg-dev libgtk-3-dev` |

All three also need:

- the [Vulkan SDK](https://vulkan.lunarg.com/) 1.4.x (the DXC shader compiler, and the validation layers for Debug
  builds);
- the [.NET 10 SDK](https://dotnet.microsoft.com/download/dotnet/10.0), for scripting, the `strada` tool and the C#
  tests;
- a GPU driver with Vulkan 1.3 to run the editor.

CMake downloads the third-party libraries itself, pinned and checked by SHA256.

## Build the engine

1. Clone the repository and change into it:

   ```sh
   git clone https://github.com/anthony-sv/3d-game-engine.git strada
   cd strada
   ```

2. Configure and build the Release configuration:

   ```sh
   python Tools/build.py --config release
   ```

   `Tools/build.py` picks the CMake preset for your operating system and runs CMake with it. On Windows it finds the
   Visual Studio developer environment by itself, so any shell works. When `VULKAN_SDK` is not set it uses the newest
   SDK in `C:/VulkanSDK` (Windows) or `~/VulkanSDK` (macOS).

3. Find the programs in `build/<preset>/bin/`:

   | Platform | Release preset | Debug preset |
   |----------|----------------|--------------|
   | Windows | `build/windows-release/bin/` | `build/windows-debug/bin/` |
   | Linux | `build/linux-gcc-release/bin/` (`--compiler clang`: `linux-clang-release`) | `build/linux-gcc-debug/bin/` |
   | macOS | `build/macos-release/bin/` | `build/macos-debug/bin/` |

   The folder holds `StradaEditor` (the editor), `StradaRuntime` (the game player) and `strada` (the command-line tool
   and MCP server for agents), with `.exe` on Windows.

Other options of `Tools/build.py`:

| Option | What it does |
|--------|--------------|
| `--config debug`, `release` or `dist` | The configuration (default `debug`). Release is optimized with asserts and debug information; Dist is optimized without asserts or development tooling |
| `--compiler gcc` or `clang` | The Linux compiler (default `gcc`) |
| `--preset <name>` | An explicit preset from `CMakePresets.json` instead of `--config` and `--compiler` |
| `--target <name>` | Build one target only |
| `--jobs <n>` | Limit the parallel build jobs |
| `--test` | Run the test suites after building |
| `--clean` | Delete the build directory first |

`python Tools/build.py --test` builds Debug and runs every test suite; `python Tools/build.py --config release --test`
does the same for Release. GPU tests need a Vulkan device and windowed tests a display: set
`STRADA_TESTS_ALLOW_NO_GPU=1` or `STRADA_TESTS_ALLOW_NO_DISPLAY=1` to let them skip on machines without one.
[AGENTS.md](../AGENTS.md) covers the presets and the development workflow in full.

## A first look

### Start the editor

Start the editor from the repository root, with a project or without one:

```sh
build/windows-release/bin/StradaEditor --project Projects/PrismBreak/PrismBreak.sproj    # Windows (PowerShell)
build/linux-gcc-release/bin/StradaEditor --project Projects/PrismBreak/PrismBreak.sproj  # Linux
build/macos-release/bin/StradaEditor --project Projects/PrismBreak/PrismBreak.sproj      # macOS
```

Without a project, the editor starts with a sample scene made of built-in assets. The default layout puts the Scene
View, the Game View and Renderer Info in the middle, the Scene Hierarchy on the left, the Inspector (with Scene
Settings and Project Settings) on the right, and the Content Browser, the Console and the Visual Script Debugger at the
bottom. The View menu shows and hides each of them, and View > Reset Layout brings the default back. The toolbar above
them holds Play, Pause, Step and Simulate. The right end of the menu bar shows the automation server's address
(`Automation: 127.0.0.1:<port> (<n> connected)`), a script build in progress or failed, and an export in progress.

Move around the Scene View with the mouse over it:

| Input | What it does |
|-------|--------------|
| Right mouse button held, with W, A, S, D (Q and E down and up, Shift faster) | Look around and fly |
| Middle mouse button held | Pan |
| Alt + left mouse button held | Orbit around the focal point |
| Mouse wheel | Move towards or away from the focal point |
| F | Frame the selected entities (double-clicking an entity in the hierarchy does the same) |
| Q, W, E, R | Select, move, rotate and scale gizmo |
| X | Switch the gizmo between local and world space |
| G | Show or hide the grid |

Some of the editor's command-line options (`StradaEditor --help` lists them all):

| Option | What it does |
|--------|--------------|
| `--project <path>` | Open this project file (`.sproj`) at startup |
| `--scene <path>` | Open this scene file at startup, after the project |
| `--maximized`, `--window-size <W>x<H>` | Open the window maximized, or at this size (default 1600x900) |
| `--headless` | Run without a window or UI, for automation |
| `--automation-port <port>`, `--no-automation` | Fix the automation server's port on 127.0.0.1, or do not start it |

### Open the samples

Open a project with File > Open Project..., File > Recent Projects, or `--project` on the command line. The samples
are in the repository, so you can open them in place to look at them. To make one your own, take a copy: an agent (or
any automation client) copies Prism Break, Blocks or the Showcase with `project.copy-example` (`project.examples` lists
them), leaving out their authoring tools and build output, and opens the copy.

- **Showcase** (`Projects/Showcase/Showcase.sproj`). Its Poly Haven sky, models and texture maps are not in the
  repository (about 95 MB, CC0). Download them once after cloning, before you open the project:

  ```sh
  python Projects/Showcase/Tools/FetchAssets.py
  ```

  The script uses the repository's `strada` tool (`dotnet run --project StradaTool`, or a `strada` executable given as
  its argument), downloads only what is missing and checks every file against the size and MD5 that Poly Haven lists.
- **Prism Break** (`Projects/PrismBreak/PrismBreak.sproj`).
- **Blocks** (`Projects/Blocks/Blocks.sproj`).
- **FeatureTest** (`Projects/FeatureTest/FeatureTest.sproj`), which uses every component, the entire scripting API and
  every built-in node kind.

When you open a project with C# scripts (Showcase, Blocks, FeatureTest), the editor builds them if they changed since
the last build; the menu bar shows "Scripts: building..." until it is done.

Press Play in the toolbar (or Ctrl+P). The Game View comes to the front and takes the keyboard and mouse: every key and
click goes to the game, except Shift+Esc, which gives the input back to the editor, and the play shortcuts (Ctrl+P,
Ctrl+Shift+P, Ctrl+Alt+P). A click elsewhere in the editor also gives the input back; a click on the Game View returns
it to the game. Press Ctrl+P again (or the toolbar's Stop) to stop: everything the game changed is discarded.

### The Showcase's controls

The Showcase opens with a camera tour; any key or click skips it. Then the camera rig flies:

| Input | What it does |
|-------|--------------|
| Right mouse button held | Look around |
| W, A, S, D; Q and E | Fly; down and up |
| Left Shift | Fly faster |
| Left click | Throw a ball where the cursor points |
| F | Walk (the camera drops onto the character there) or fly again |
| While walking: mouse, W, A, S, D, Shift, Space | Look, walk, run, jump; clicks throw along the view |
| While walking: Esc | Free the cursor for the HUD; a click into the scene takes it back |
| L | Switch between day and night |
| C | Play the tour again, with the exhibits put back |
| H | Hide or show the HUD |
| R | Put the props, dominoes, wrecking ball and balls back |
| 1, 2, 3 | Push the wrecking ball, tip the dominoes, fire the ball launcher |
| T | Show or hide the debug view (throw previews, trigger bounds) |
| G | Play the Graph Garden's flourish |

The HUD's hint line shows the keys of the way the camera moves, and its buttons put the exhibits back, set them off,
switch day and night and switch between flying and walking.

Prism Break: Space or a click starts a game and launches the ball; the mouse, A and D or the arrow keys move the paddle;
Escape pauses. Blocks: Left and Right (or A and D) move a piece, Up, W or X turns it clockwise and Z counter-clockwise,
Down or S drops it faster and Space at once, C or Shift holds it, Escape or P pauses and R starts again.

### What each sample shows

| Project | What it shows |
|---------|---------------|
| `Projects/Showcase` | Every engine feature in exhibits around a fountain in a forest clearing: physically based materials, glass and cutouts, colored lights and shadows in a post-process evening zone, rigid bodies and joints (a wrecking ball, dominoes placed as prefabs), triggers, particles and 3D sound, a yard to walk and climb in with a character controller, day and night, a camera tour, a HUD with buttons, and a garden of chimes played by six visual scripts. Its test scene has 68 checks |
| `Projects/PrismBreak` | A neon brick breaker made of 13 visual scripts and no C#: function libraries, typed events between graphs, remote variables, timelines and formulas. Its test scene's 29 checks are graphs too. Open `Scripts/Game.sgraph` while it plays to watch it run |
| `Projects/Blocks` | A falling-blocks game in C#: game rules in plain classes, an AI that plays the title screen's demo, hold and preview queues, debris and sparks from prefabs, sounds, and a HUD and menus on a UI canvas. Its test scene has 57 checks |
| `Projects/FeatureTest` | Scenes and scripts that use every component and the entire scripting API, and graphs that use every built-in node kind; CI plays it and fails on any failed check |

The [scripting guide](Scripting.md#examples) and the
[visual scripting guide](VisualScripting.md#a-complete-game-prism-break) describe the samples in more detail.

### Run a project with the player

`StradaRuntime` plays a project, or an exported game, without the editor:

```sh
build/windows-release/bin/StradaRuntime --project Projects/PrismBreak/PrismBreak.sproj
```

A project runs from its directory with its last script build. A project whose C# scripts were never built is refused
("the project's scripts are not built ... build them in the editor first"): open it in the editor once, which builds
them. Prism Break has no C# scripts and runs at once.

| Option | What it does |
|--------|--------------|
| `--project <path>` | Run a project (`.sproj`) |
| `--game <path>` | Run the exported game configured by this `Game.sgame` (default: the one next to the player) |
| `--scene <path>` | Start with this scene (relative to the asset directory) instead of the start scene |
| `--test` | Run headless with a fixed time step until the scripts finish testing, quit or time out |
| `--timeout <seconds>` | Game time a test run may take (default 60; only with `--test`) |
| `--timestep <seconds>` | Advance the game by this time each frame (default: the frame's real time; 1/60 in tests) |
| `--headless` | Run without a window, rendering or sound |
| `--frames <count>` | Exit after this many frames |
| `--screenshot <path>` | Save a PNG of the window (on the last frame with `--frames`, otherwise frame 60) |
| `--gpu <index>` | Use the GPU with this adapter index |
| `--validation`, `--no-validation` | Enable or disable Vulkan and NVRHI validation |
| `--no-vsync` | Present without waiting for vertical sync |

The player exits with 0 after a normal run, 1 when the game cannot start and 2 for wrong usage. Test runs exit with the
number of failures (see [Test the game](#test-the-game)).

## Your first game

You will make a small scene: three crates that fall onto a floor, a cube that a visual script turns (and stops for two
seconds when you press Space), a cube that a C# script turns, and a line of text over the game.

### Create a project

1. Choose File > New Project....
2. Type a **Name** (for example `First Game`) and a **Location**, the folder the project goes into (Browse... picks
   one). The dialog shows the directory it creates: a folder named after the project inside the location.
3. Click **Create**. If the open scene has unsaved changes, the editor first asks whether to save them.

The editor opens the new project, made from the template `empty-3d`. Its folder holds:

| Path | What it is |
|------|------------|
| `<Name>.sproj` | The project file: the name, the window, the physics layers, the script module and the start scene |
| `Assets/` | The asset directory: scenes, materials, meshes, textures, sounds, fonts, visual scripts and prefabs |
| `Assets/AssetRegistry.sreg` | The registry that gives every asset file a handle, so references survive moving files |
| `Assets/Scenes/Main.sscene` | The start scene |
| `.gitignore` | Keeps build output, caches and machine-specific files (`Build/`, `.strada/`, the script build) out of git |
| `Scripts/` | Created with the first C# script: the C# project `Game.csproj`, the sources in `Source/`, the build in `Binaries/` |

The start scene is open. It holds a camera (Main Camera), a directional light (Sun), a sky light with the built-in sky
(Sky), and a floor plane, a cube, a sphere and a capsule made of built-in meshes.

### Build the scene

The Entity menu creates entities from presets: Create Empty, then the submenus 3D Object, 2D Object, Rendering,
Effects, Physics, Audio and UI. Right-clicking empty space in the Scene Hierarchy opens the same presets, and a row's
right-click menu has Create Child, Rename (F2), Duplicate (Ctrl+D), Delete and Create Prefab. New entities appear at the
Scene View's focal point and are selected, and the Inspector shows their components.

1. Make room. In the Scene Hierarchy, click **Floor**, Ctrl+click **Cube**, **Sphere** and **Capsule**, then
   right-click one of them and choose **Delete**.
2. Make a floor that things can land on. Choose Entity > 3D Object > Cube, right-click it in the hierarchy, choose
   **Rename** and type `Floor`. In the Inspector, set its Transform's **Translation** to `0, -0.5, 0` and **Scale** to
   `20, 1, 20`: a box 20 m wide whose top is at height 0.
3. Click **Add Component** at the bottom of the Inspector and choose Physics > **Rigid Body**. Its **Type** is Static,
   which suits a floor. Add Physics > **Box Collider** the same way: its half extents of 0.5 match the built-in cube,
   and colliders scale with their entity. (Typing in the menu's search field finds components by name; Enter adds the
   first match.)
4. Make a crate. Choose Entity > 3D Object > Cube, rename it `Crate`, and set its **Translation** to `-1.5, 3, 0`. Add
   a **Rigid Body** and set its **Type** to Dynamic, then add a **Box Collider**.
5. Right-click **Crate** in the hierarchy and choose **Duplicate** twice. Set the copies' **Translation** to `0, 4, 0`
   and `1.5, 5, 0`.
6. Add a light. Choose Entity > Rendering > **Point Light** and set its **Translation** to `0, 3, 2`; its **Color**,
   **Intensity** and **Range** are in the Inspector. The Rendering submenu also has Camera, Directional Light, Spot
   Light, Sky Light and Post Process Volume.
7. Look at the sky. **Sky** is a Sky Light whose **Environment** is the built-in sky (Entity > Rendering > Sky Light
   makes one like it). To light the scene with an HDR panorama of your own, import the `.hdr` file (the Content
   Browser's **Import** button, or drop the file onto the Content Browser from your file manager), then drag it from the
   Content Browser onto the Scene View: it becomes the first sky light's Environment, or a new Sky Light when the scene
   has none. `strada hdri <id>` imports one from Poly Haven into the open project (see [Agents.md](Agents.md)).
8. Press Play: the crates fall and land on the floor. Press Ctrl+P to stop, which puts them back.

### Give it a material

1. In the Content Browser, with the Assets folder shown, click **+** and choose **Material**. The browser makes
   `New Material.smat` in the folder it shows and opens its name for editing: type `Red` and press Enter.
2. The Inspector shows the material with a preview. Set its **Base Color** to a red, and try **Metallic** and
   **Roughness**. Material edits are written to the file as you make them, and Ctrl+Z undoes them.
3. Drag `Red` from the Content Browser onto a crate in the Scene View: the crate draws with it.
4. To give all three crates the material at once, select them in the hierarchy and drop the material onto the material
   header under their Mesh component in the Inspector ("Drop a material here to use it in this slot"). The header's
   **Material** field picks a material too, and its x goes back to the mesh's own one.

### Make something happen with a visual script

A visual script is a graph of nodes (a `.sgraph` asset) that a Visual Script component runs on its entity. This one
turns a cube about its vertical axis, like the first graph of the
[visual scripting guide](VisualScripting.md#a-first-graph).

1. Choose Entity > 3D Object > Cube, rename it `Graph Cube` and set its **Translation** to `-2, 1.5, -2`. Leave it
   without a Rigid Body: the graph moves it, not physics.
2. In the Content Browser, click **+** and choose **Visual Script**, and name it `Spin`. It starts with a Create and an
   Update event.
3. Drag `Spin` from the Content Browser onto **Graph Cube** in the hierarchy. The cube gets a Visual Script component
   that runs it. (A Visual Script component without a graph offers **Create Visual Script**, which writes
   `Assets/Scripts/<entity name>.sgraph` and opens it.)
4. Double-click `Spin`. Its graph window opens beside the Scene View: **My Script** on the left (its graphs, variables
   and custom events), the canvas in the middle and **Details** on the right.
5. In My Script, click the **+** of **Variables** and name the variable `Speed`. In Details, set its **Type** to Float,
   its **Default Value** to `90` and turn on **Public**, so that each entity can set its own speed in the Inspector.
6. Add the nodes: press Tab over the canvas (or right-click it), type part of a node's name and press Enter for the
   first match, or click another. Add "get speed", "multiply", "angle axis", "get rotation", a second "multiply" and
   "set rotation". Several nodes are called Get Rotation and Set Rotation: click the ones whose subtitle, at the right
   of the list, reads "Target is Entity" (the sky light's come first).
7. Drag from pin to pin to wire them:
   - Update's **Then** to Set Rotation's **Execute** (the flow);
   - Get Speed's **Value** to the first Multiply's **A**, and Update's **DeltaTime** to its **B**;
   - the first Multiply's **Result** to Angle Axis's **Degrees**; click Angle Axis's **Axis** on the node and set it to
     `0, 1, 0`;
   - Angle Axis's **Result** to the second Multiply's **A**, Get Rotation's **Value** to its **B**, and its **Result**
     to Set Rotation's **Value**.

   A wire turns red and its tooltip says why when two pins cannot connect. The toolbar counts errors and warnings; a
   graph with an error does not run. The toolbar's Arrange tidies the nodes.
8. Press Play: the cube turns at 90 degrees per second. The graph saves itself with every edit (the toolbar says
   "Saved"), and its window can stay open while the game plays: the flow lights up as it runs.

Now make it react to a key, as the visual scripting guide's [events section](VisualScripting.md#events-flow-and-waiting)
does:

1. Add a "key pressed" node, select it and set its **Key** to Space in Details.
2. Add a Set Speed node (drag `Speed` from My Script onto the canvas with Alt held, or type "set speed") with the value
   `0`, and wire Key Pressed's **Then** to its **Execute**.
3. Add a "delay" node with a **Duration** of `2`, and wire the Set Speed's **Then** to its **Execute**. Add a second
   Set Speed with the value `90` and wire the Delay's **Completed** to its **Execute**.
4. Press Play, click the Game View and press Space: the cube stops for two seconds.

The [visual scripting guide](VisualScripting.md) explains events, variables, functions, formulas, timelines, graphs that
talk to each other, and the debugger.

### The same in C#

C# scripts are classes deriving from `Strada.Script`. This one is the first script of the
[scripting guide](Scripting.md#a-first-script).

1. Choose Entity > 3D Object > Cube, rename it `Script Cube` and set its **Translation** to `2, 1.5, -2`.
2. Choose Scripts > **New Script...** (or **+** > **C# Script...** in the Content Browser). Type the class name
   `Spinner`; the dialog shows what it creates, `Scripts/Source/Spinner.cs (class Game.Spinner)`. Click **Create**. The
   first script also creates the project's C# project, `Scripts/Game.csproj`.
3. The Content Browser shows the new file under its Scripts root. Double-click it to open it in your C# editor (Visual
   Studio, Rider, or VS Code with the C# extension; the C# project references the engine's `Strada.ScriptCore.dll`, so
   completion and documentation work). Replace its contents with:

   ```csharp
   using Strada;

   namespace Game;

   public class Spinner : Script
   {
   	[Tooltip("Turn speed in degrees per second")]
   	public float Speed = 90.0f;

   	protected override void OnUpdate(float deltaTime)
   	{
   		// A little more of a turn around the up axis each frame.
   		Transform.Rotation = Quaternion.AngleAxis(Speed * deltaTime, Vector3.Up) * Transform.Rotation;
   	}
   }
   ```

4. Save the file. The editor notices the change within a second, builds the scripts and loads them (Scripts > Build
   Scripts, Ctrl+B, builds at once). Build errors go to the Console with their file and line, and the menu bar shows
   "Scripts: <n> errors".
5. Drag `Spinner` from the Content Browser onto **Script Cube** in the hierarchy, the Inspector or the Scene View. The
   cube gets a Script component running `Game.Spinner`, and `Speed` appears under it. (You can also add Scripting >
   Script with Add Component and pick the class in its **Class** list.)
6. Press Play: the second cube turns too.

Scripts reload while the editor runs. Stop the game, add a key check to `OnUpdate`, and save:

```csharp
if (Input.IsKeyPressed(KeyCode.R))
{
	Speed = -Speed;
}
```

The editor builds and loads the new version; press Play and R turns the cube the other way. A build that finishes
while the game plays is loaded when play mode stops. While the game plays, the Inspector shows the values the running
script holds, and a value you change reaches the script at once.

### Add a HUD

The screen-space UI is made of entities too: a canvas and the elements inside it.

1. Choose Entity > UI > **Text**. The scene had no canvas, so the editor makes **Canvas** (a UI Canvas that fits a
   1920 x 1080 design to any screen) with **Text** inside it. The Scene View draws the canvas at its entity's position;
   the Game View draws it over the game.
2. Select **Text**. In its UI Text component, set **Text** to `Space stops the left cube, R turns the right one back`
   and **Alignment** to Left.
3. In its UI Rect component, set **Anchor Min** and **Anchor Max** to `0, 0` (the top-left corner), **Pivot** to
   `0, 0`, **Position** to `40, 40` and **Size** to `1000, 80`. Canvas units have y pointing down.
4. To change the text while the game runs, give the Text entity a script that sets it: make a script named `PlayTime`
   with Scripts > New Script..., write it as below, save it and drag it onto **Text**. It shows the time played
   (numbers are formatted with the invariant culture, so they never use characters the font lacks):

   ```csharp
   using System.Globalization;
   using Strada;

   namespace Game;

   public class PlayTime : Script
   {
   	protected override void OnUpdate(float deltaTime)
   	{
   		GetComponent<UITextComponent>()!.Text = string.Create(CultureInfo.InvariantCulture, $"Time: {Time.Elapsed:F1} s");
   	}
   }
   ```

The UI menu also has Panel, Image and Button; [HUDs and menus](Scripting.md#huds-and-menus) explains anchors, scale
modes and buttons that scripts hear.

### Play, pause and step

| Toolbar | Menu | Key | What it does |
|---------|------|-----|--------------|
| Play / Stop | Play > Play / Stop | Ctrl+P | Plays a copy of the scene in the Game View; Stop discards everything the game changed |
| Pause | Play > Pause | Ctrl+Shift+P | Pauses and resumes the game |
| Step | Play > Step | Ctrl+Alt+P | Advances the paused game by one frame |
| Simulate | Play > Simulate | | Simulates physics without scripts, seen through the editor camera |

Stopping discards the changes made to the scene while playing. Changes to asset files made while playing, such as a
material's color or a graph edit, stay in their files and stay undoable.

### Save

Choose File > Save Scene (Ctrl+S). The window title marks a scene with unsaved changes with `*`; File > Save Scene
As... (Ctrl+Shift+S) saves it under another name. Materials and visual scripts save themselves as you edit them. When
an action would discard unsaved changes (a new scene, another project, quitting), the editor asks: Save, Don't Save or
Cancel. Edit > Undo (Ctrl+Z) and Edit > Redo (Ctrl+Y) cover every edit, and their menu items name the step.

## Test the game

A test is a scene whose scripts run checks. `Strada.Testing` provides `TestReporter.Run(name, check)`, which runs a
check and reports it, `Assert` (`IsTrue`, `AreEqual`, `AreApproximatelyEqual`, `IsNotNull`, ...) for use inside checks,
and `TestReporter.Finish()`, which says that every check has reported. [Testing a game](Scripting.md#testing-a-game)
describes it; graphs report checks with `TestReporter` nodes, as Prism Break's test scene does.

1. Make a test script: Scripts > New Script..., class name `FirstGameTests`, with these contents. It lets the crates
   fall for two seconds of game time, then checks that one has landed:

   ```csharp
   using Strada;
   using Strada.Testing;

   namespace Game;

   public class FirstGameTests : Script
   {
   	protected override void OnUpdate(float deltaTime)
   	{
   		if (Time.Elapsed < 2.0)
   		{
   			return;
   		}
   		TestReporter.Run("a crate landed on the floor", () =>
   		{
   			Entity? crate = FindByName("Crate");
   			Assert.IsNotNull(crate);
   			Assert.IsTrue(crate.Translation.Y < 1.0f, "the crate is still in the air");
   		});
   		TestReporter.Finish();
   		Application.Quit();
   	}
   }
   ```

2. Choose File > Save Scene As... and save the scene as `Scenes/Tests.sscene` inside the project's `Assets` folder. The
   test scene is now open.
3. Choose Entity > Create Empty, rename the entity `Tests` and drag `FirstGameTests` onto it. Save the scene.
4. Press Play to run the checks in the editor: the Console shows "Test passed: a crate landed on the floor" and
   "Tests finished", and play mode stops when the script quits.
5. Run the test scene with the player (`StradaRuntime` in `build/<preset>/bin/`), headless:

   ```sh
   StradaRuntime --project "<project folder>/First Game.sproj" --scene Scenes/Tests.sscene --test
   ```

   A test run uses a fixed time step (1/60 s, `--timestep` changes it) and ends when the scripts call
   `TestReporter.Finish()`, quit, or reach the time limit (60 s of game time, `--timeout` changes it). It prints
   "Tests passed: 1 check in <n> frames", or "Tests failed: ..." with what failed.
6. Read the exit code: the number of failures (failed checks and script exceptions, plus one when the run did not
   finish), at most 100. 0 means every check passed, which suits continuous integration. A game that cannot start
   exits with 1.

The sample projects test themselves the same way:

```sh
StradaRuntime --project Projects/PrismBreak/PrismBreak.sproj --scene Scenes/Tests.sscene --test
```

Choose File > Open Scene... (Ctrl+O) and open `Scenes/Main.sscene` to go back to the game's scene.

## Export the game

1. Save the scene. The export uses the saved files; the dialog warns when the scene has unsaved changes.
2. Choose File > **Build Game...**. The Build Game dialog says what it builds ("Builds <name> for <platform> from its
   saved files"): games are exported for the platform the editor runs on.
3. Under **Game**, check the game's **Name**, **Start Scene** and **Window** settings; changes are saved to the project.
4. Under **Output**, choose the directory: a new or empty directory, or an earlier export, which the new one replaces.
   The default is the project's `Build` folder. **Run a smoke test** runs the exported game headless for a few frames
   and checks that it exits cleanly.
5. Click **Build**. The editor builds the C# scripts in Release and writes the game. The result shows the time, the
   files and the size, and **Open Folder** and **Run Game** buttons. Scripts that do not compile stop the export and
   leave an earlier export as it was.

The exported folder holds:

| Path | What it is |
|------|------------|
| `<Name>` (`<Name>.exe` on Windows) | The game player, named after the game |
| `Game.sgame` | The game's configuration |
| `Assets/` | The asset directory with its registry |
| `Scripts/` | The C# scripts built in Release with `Strada.ScriptCore.dll`, for games with C# scripts or visual scripts (with visual scripts alone, only the scripting API) |
| `ThirdPartyNotices.md` | The licenses of the engine's dependencies |

On Windows the MSVC runtime DLLs are next to the executable; editors built in Release or Dist provide them, Debug builds
do not (their runtime is not redistributable), so export games you ship from a Release or Dist build. On macOS the
folder holds `<Name>.app`, with the player in `Contents/MacOS`, the files above in `Contents/Resources`, and the Vulkan
loader and MoltenVK inside the bundle. Players need no Vulkan SDK. Games with C# scripts or visual scripts need the
.NET 10 runtime: installed on the player's machine, or shipped by you in a `dotnet` folder next to the executable.

Run the game by starting its executable. It is the player renamed, so it takes the player's options:
`<Name> --scene Scenes/Tests.sscene --test` runs the exported test scene. Visual-script nodes marked Development Only
are disabled in exported games.

## Work with an AI agent

Every editor operation is an automation command, and `strada mcp` serves them as Model Context Protocol tools: an AI
agent can build scenes, materials, scripts and graphs, play and test the game, debug its graphs and export it, in the
editor you are looking at, through the same undo history. This repository's `.mcp.json` registers the server for
agents working in the checkout. [Agents.md](Agents.md) explains the setup, how an agent should work and how the samples
were made this way.

## Where to go next

| Document | What it covers |
|----------|----------------|
| [Docs/README.md](README.md) | The documentation home |
| [Editor.md](Editor.md) | The editor manual: panels, menus, keys and workflows |
| [Components.md](Components.md) | Every component and field |
| [Scripting.md](Scripting.md) | Writing games in C#, from a first script to testing a game |
| [ScriptingAPI.md](ScriptingAPI.md) | Every member of the C# scripting API |
| [VisualScripting.md](VisualScripting.md) | Visual scripts: the language, the graph editor, the debugger and live editing |
| [Agents.md](Agents.md) | Building games with AI agents through the editor's MCP tools |
| [Automation.md](Automation.md) | The automation protocol, every command, the `strada` tool and its MCP server |
| [Architecture.md](Architecture.md) | The architectural contract of the engine |
| [README](../README.md) | The project overview, features and screenshots |
| [AGENTS.md](../AGENTS.md) | The development guide: building, style, testing and the commit process |
