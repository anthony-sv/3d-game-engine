# Scripting guide

Games get their behavior from C# scripts: classes deriving from `Strada.Script`, attached to entities by Script
components, and from [visual scripts](#visual-scripts), graphs of nodes that VisualScript components run. This guide
shows how to write them. §10 of [Architecture.md](Architecture.md) is the reference of the scripting runtime, and every
public member of the API has XML documentation that C# editors show as you type.

## A first script

A project's scripts form one C# project, `Scripts/<Name>.csproj`, where `<Name>` is the project's script module
(`Game` unless the project settings say otherwise); it is also the scripts' namespace. In the editor, Scripts > New
Script, or "+" > C# Script in the content browser, asks for a class name and writes `<Class>.cs` from a template into
the script folder the content browser shows, else into `Scripts/Source` (agents use the `script_create` tool); a class
that a source of the scripts declares already is refused, as the C# project would declare it twice. The content
browser's Scripts root shows the C# sources in `Scripts/Source`: double-click one to open it in the desktop's C#
editor, drag it onto another folder to move it, and drop `.cs` files or folders of them onto the editor from the file
manager to copy them in. Open the project folder in any C# editor (Visual Studio, Rider, VS Code with the C#
extension): the C# project references this engine's `Strada.ScriptCore.dll`, so completion and documentation work.

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

Save the file: the editor notices changed sources within a second, builds them and loads the new assembly (Scripts >
Build Scripts, Ctrl+B, builds at once). Build errors go to the console with their file and line, and the menu bar shows
a failed build. Once the build has finished, drag the script from the content browser onto an entity in the hierarchy,
the inspector or the scene view: the entity gets a Script component running `Game.Spinner` (or select the entity, add a
Script component in the inspector and pick the class), and `Speed` appears below it. Press Play (Ctrl+P) and the entity
turns. A build that finishes while the game plays is loaded when play mode stops.

## How a script runs

Override the methods you need:

| Method | Called |
|--------|--------|
| `OnCreate()` | Once, when the scene starts or the entity is created while it runs, after every script of the scene exists |
| `OnUpdate(float deltaTime)` | Every frame, with the seconds since the previous frame |
| `OnFixedUpdate(float fixedDeltaTime)` | Before every physics step, at the scene's fixed rate |
| `OnCollisionEnter(Entity other)`, `OnCollisionExit(Entity other)` | When a collider of the entity starts or stops touching one of `other` |
| `OnTriggerEnter(Entity other)`, `OnTriggerExit(Entity other)` | When a trigger collider of either entity starts or stops overlapping the other |
| `OnClick(Entity button)` | When a UI button that is the entity or one of its descendants was clicked, before the frame's `OnUpdate` |
| `OnDestroy()` | When the entity is destroyed or the scene stops |

Because every script exists before the first `OnCreate`, scripts can look each other up there. Exceptions thrown by
these methods are logged with their stack trace and line numbers and do not stop the scene; in a test run they count
as failures.

## Fields

Public fields, and private ones marked `[SerializeField]`, are saved with the scene and edited in the inspector, which
shows them by name. Their values are set before `OnCreate`. Supported types: `bool`, `int`, `uint`, `long`, `ulong`,
`float`, `double`, `string`, `Vector2`, `Vector3`, `Vector4`, `Quaternion`, `Color`, enums, `Entity` (an entity of the
scene; drag one from the hierarchy), and assets: `Prefab`, `Material`, `Mesh`, `Texture`, `AudioClip`, `Font`,
`EnvironmentMap` (pick one, or drag it from the content browser).

```csharp
public class Turret : Script
{
	[Tooltip("The entity it fires at")]
	public Entity? Target;

	public Prefab? Projectile;

	[Range(1.0f, 50.0f)]
	public float Reach = 20.0f;

	[SerializeField]
	private float reloadSeconds = 0.5f;

	[HideInInspector]
	public int ShotsFired;

	private float untilReloaded;

	protected override void OnUpdate(float deltaTime)
	{
		untilReloaded -= deltaTime;
		if (Target is null || Projectile is null || untilReloaded > 0.0f ||
			Vector3.Distance(Target.Transform.WorldTranslation, Transform.WorldTranslation) > Reach)
		{
			return;
		}
		Entity.Instantiate(Projectile, Transform.WorldTranslation, Rotation);
		untilReloaded = reloadSeconds;
		ShotsFired++;
	}
}
```

`[Range(min, max)]` limits what the inspector accepts, `[Tooltip(text)]` explains the field, and `[HideInInspector]`
keeps a field out of the inspector while still saving it. Fields that are neither public nor marked belong to the
script alone and start with their initial values.

While the game plays in the editor, the inspector shows the values the running script holds, also those it changes
itself, and a value you set reaches the script at once (also while paused), so you can tune a game while it runs; undo
gives the script back the value it had. Stopping discards these edits, as it discards every change made while playing.

## Entities and components

A script is the entity it is attached to: `Name`, `Translation`, `Rotation` and `Scale` (local, relative to the
parent), `Transform` (with `WorldTranslation`, `Forward`, `Right`, `Up` and `EulerAngles` in degrees), `Parent` and
`Children` are right there. Turn entities by multiplying rotations (`Quaternion.AngleAxis(degrees, axis) * Rotation`),
or keep angles of your own and set them whole: the Euler angles read back from a rotation are one of several sets of
angles for it (the Y angle comes back within ±90°), so adding to them does not turn an entity all the way around.
Components are views of the entity's data: `GetComponent<T>()` returns null when the entity
has none, and every property reads or writes the component itself, so there is nothing to copy back.

```csharp
public class Door : Script
{
	public Entity? Light;

	protected override void OnCreate()
	{
		if (GetComponent<MeshComponent>() is MeshComponent mesh)
		{
			mesh.CastShadows = false;
		}
		if (Light?.GetComponent<PointLightComponent>() is PointLightComponent lamp)
		{
			lamp.Intensity = 20.0f;
		}
		Entity? player = FindByName("Player");
		Log.Info($"{Name} sees {player?.Name ?? "nobody"}");
	}
}
```

`AddComponent<T>()` and `RemoveComponent<T>()` change the entity's components, `Entity.Create(name)` makes an empty
entity and `Destroy()` removes one with its children (at the end of the frame, see below). `Entity.FindByName(name)`,
`Entity.FindByID(id)`, `Parent`, `Children`, contact callbacks and raycast hits return the script instance of entities
that have one: `As<T>()` gets the script of another entity, to call its methods.

Every property access crosses into the engine. That is cheap, but read a value once into a local when you use it
several times in a frame.

## Input

`Input` has the state of the current frame: `IsKeyDown` (held), `IsKeyPressed` and `IsKeyReleased` (changed this
frame), the same for mouse buttons, `MousePosition`, `MouseDelta`, `MouseScrollDelta`, `CursorMode` (to lock the cursor
for mouse look) and gamepads (`IsGamepadConnected`, `GetGamepadAxis`, `IsGamepadButtonDown`, ...).

```csharp
public class Mover : Script
{
	public float Speed = 4.0f;

	protected override void OnUpdate(float deltaTime)
	{
		Vector3 direction = Vector3.Zero;
		if (Input.IsKeyDown(KeyCode.W))
		{
			direction += Vector3.Forward;
		}
		if (Input.IsKeyDown(KeyCode.S))
		{
			direction += Vector3.Back;
		}
		if (Input.IsKeyDown(KeyCode.A))
		{
			direction += Vector3.Left;
		}
		if (Input.IsKeyDown(KeyCode.D))
		{
			direction += Vector3.Right;
		}
		if (direction != Vector3.Zero)
		{
			Translation += direction.Normalized * (Speed * deltaTime);
		}
	}
}
```

In the editor the game has the keyboard and mouse from the moment Play starts: every key and click on the Game View
goes to the game alone, Escape and Enter included, except Shift+Esc, which gives the input back to the editor, and the
play shortcuts (Ctrl+P, Ctrl+Shift+P, Ctrl+Alt+P). A click elsewhere in the editor gives the input back too, and a click
on the view returns it to the game. `CursorMode` applies while the game has the input: a hidden cursor is hidden over
the Game View only, a locked one is locked until the input goes back to the editor. Gamepads reach the game whoever has
the keyboard and mouse. `Application.WindowWidth` and `WindowHeight` are the size the game view renders at, which its
resolution menu chooses.

## Physics

Give an entity a `RigidBodyComponent` (dynamic, static or kinematic) and a collider (box, sphere, capsule or mesh) in
the editor; the scene's physics moves it. Act on it from scripts through the body:

```csharp
public class Jumper : Script
{
	public float JumpSpeed = 6.0f;

	private bool grounded;

	protected override void OnUpdate(float deltaTime)
	{
		if (grounded && Input.IsKeyPressed(KeyCode.Space))
		{
			GetComponent<RigidBodyComponent>()!.AddForce(Vector3.Up * JumpSpeed, ForceMode.VelocityChange);
			grounded = false;
		}
	}

	protected override void OnCollisionEnter(Entity other)
	{
		grounded = true;
	}

	protected override void OnTriggerEnter(Entity other)
	{
		if (other.Name == "Coin")
		{
			other.Destroy();
		}
	}
}
```

Forces and torques take a `ForceMode` (`Force`, `Impulse`, `Acceleration`, `VelocityChange`). `LinearVelocity` (meters
per second, up to 500) and `AngularVelocity` (radians per second, up to about 47) can be read and set, `MoveKinematic`
moves kinematic bodies smoothly and `Teleport` places any body.
Apply continuous forces in `OnFixedUpdate`, which runs before each physics step. `Physics.Raycast` finds the closest
solid collider along a ray (`Physics.Gravity` is the scene's gravity):

```csharp
if (Physics.Raycast(Transform.WorldTranslation, Transform.Forward, 100.0f, out RaycastHit hit))
{
	Log.Info($"{hit.Entity.Name} is {hit.Distance} m ahead");
}
```

A camera turns screen positions into rays: `camera.ScreenToWorldRay(Input.MousePosition)` picks what is under the
cursor. Collision layers and which layers collide are set in the project settings; raycasts take a layer mask.

A `JointComponent` joins a body to another (`ConnectedEntity`) or to the world (null): `Hinge` for doors, wheels and
pendulums (with `LimitsEnabled`, `MinLimit`/`MaxLimit` in degrees, and a motor: `MotorEnabled`, `MotorSpeed` in degrees
per second, `MotorMaxForce`), `Ball` for chains and ropes, `Distance` to keep two anchors within `MinDistance` and
`MaxDistance`, `Slider` for pistons and `Fixed` to weld. Anchors and axes are in the entity's local space; the joint is
made where the bodies are when the scene starts, and again whenever a property changes. Destroying a link of a chain
breaks it there:

```csharp
public class Windmill : Script
{
	protected override void OnCreate()
	{
		JointComponent hub = GetComponent<JointComponent>()!;
		hub.Type = JointType.Hinge;
		hub.Axis = Vector3.Forward;
		hub.MotorEnabled = true;
		hub.MotorSpeed = 45.0f;
	}
}
```

### Characters

Players and walking creatures use a `CharacterControllerComponent` (Entity > Physics > Character Controller makes one
with a capsule mesh) instead of a rigid body and colliders: an upright capsule (`Radius`, `Height`, its center at
`Offset` from the entity) that your script moves. `Move(velocity)` walks it at a velocity during the next physics
step, so call it from `OnFixedUpdate` every step it should move; it slides along walls, climbs steps up to
`StepHeight` and slopes up to `MaxSlopeAngle`, slides down steeper ones, rides moving platforms and elevators, and
pushes dynamic bodies with up to `PushForce` newtons. Gravity acts on top of it: `IsGrounded` tells whether it stands
on walkable ground (`GroundNormal` is the ground's normal), and `Velocity` is its own velocity (falling, jumping),
which you set to jump. `Teleport(position)` places it and stops it. The entity follows the capsule, which does not
turn with it, so the script turns the entity freely. Triggers notice characters (`OnTriggerEnter` on both sides), and
raycasts hit them. Colliders of the entities below a character are its own parts (a held object, a trigger around it
for its reach): they neither block it nor report touching it.

The capsule's round bottom rolls over the edges of steps and ledges, which are too steep to stand on, so walking down
stairs or off a ledge `IsGrounded` is false for a physics step or two at each edge. Give jumps a little grace, as the
controller below does: a jump pressed just before landing still happens (a jump buffer), and so does one pressed just
after the character left the ground (coyote time).

A first-person controller: the character entity has a child, `Head`, with the camera at eye height. It keeps the yaw and
pitch itself and sets the rotations whole.

```csharp
public class FirstPersonController : Script
{
	public float Speed = 5.0f;
	public float JumpSpeed = 5.0f;
	public float MouseSensitivity = 0.15f;
	[Tooltip("Seconds a jump press is kept, and seconds after leaving the ground in which the character can still jump")]
	public float JumpGrace = 0.1f;
	public Entity? Head;

	private float m_Yaw;
	private float m_Pitch;
	private float m_JumpBuffer;
	private float m_CoyoteTime;

	protected override void OnCreate()
	{
		Input.CursorMode = CursorMode.Locked;
		// Start looking where the entity faces.
		Vector3 forward = Transform.Forward;
		m_Yaw = Mathf.Atan2(-forward.X, -forward.Z) * Mathf.Rad2Deg;
	}

	protected override void OnUpdate(float deltaTime)
	{
		// Look around: the body turns left and right, the head up and down.
		Vector2 mouse = Input.MouseDelta * MouseSensitivity;
		m_Yaw -= mouse.X;
		m_Pitch = Mathf.Clamp(m_Pitch - mouse.Y, -85.0f, 85.0f);
		Transform.Rotation = Quaternion.AngleAxis(m_Yaw, Vector3.Up);
		Head!.Transform.Rotation = Quaternion.AngleAxis(m_Pitch, Vector3.Right);
		// Key presses last one frame: keep the jump for the next physics steps.
		if (Input.IsKeyPressed(KeyCode.Space))
		{
			m_JumpBuffer = JumpGrace;
		}
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		CharacterControllerComponent character = GetComponent<CharacterControllerComponent>()!;
		Vector3 direction = (Transform.Forward * Axis(KeyCode.W, KeyCode.S)) + (Transform.Right * Axis(KeyCode.D, KeyCode.A));
		character.Move(direction.Normalized * Speed);

		m_CoyoteTime = character.IsGrounded ? JumpGrace : m_CoyoteTime - fixedDeltaTime;
		if (m_JumpBuffer > 0.0f && m_CoyoteTime > 0.0f)
		{
			character.Velocity = new Vector3(0.0f, JumpSpeed, 0.0f);
			m_JumpBuffer = 0.0f;
			m_CoyoteTime = 0.0f;
		}
		m_JumpBuffer -= fixedDeltaTime;
	}

	private static float Axis(KeyCode positive, KeyCode negative) =>
		(Input.IsKeyDown(positive) ? 1.0f : 0.0f) - (Input.IsKeyDown(negative) ? 1.0f : 0.0f);
}
```

Characters move in physics steps (60 a second unless the project says otherwise), and the screen shows as many frames as
the display does. Between steps, a character's entity is drawn on its way from where the step before the last left it to
where the last one did: a step behind, so it and a camera on it move smoothly at any frame rate. Only the way it walked,
fell or jumped is drawn like that; on a moving platform it stays where it stands on the platform as drawn.
`OnFixedUpdate` finds the entity where the last step left it, `OnUpdate` where it is drawn.

## Spawning and destroying

Prefabs are entities saved with their children (Create Prefab in the hierarchy's context menu). Spawn them with
`Entity.Instantiate`, from a `Prefab` field or from `Assets.Load<Prefab>(path)`:

```csharp
public class Spawner : Script
{
	public Prefab? Enemy;
	public float Interval = 2.0f;

	private float untilNext;

	protected override void OnUpdate(float deltaTime)
	{
		untilNext -= deltaTime;
		if (untilNext > 0.0f || Enemy is null)
		{
			return;
		}
		untilNext = Interval;
		Vector3 position = Translation + new Vector3(Random.Range(-5.0f, 5.0f), 0.0f, 0.0f);
		Entity.Instantiate(Enemy, position, Quaternion.Identity);
	}
}
```

The instance's scripts have run `OnCreate` by the time `Instantiate` returns. `Destroy()` removes an entity and its
children at the end of the frame: until then they stay valid, so other scripts of that frame can still read them, and
their scripts get `OnDestroy` while they are intact.

## Sound, text and materials

`AudioSourceComponent` plays its clip with `Play()`, `Pause()` and `Stop()` (`IsPlaying`, `Volume`, `Pitch`, `Loop`,
`Clip` can be set); sources are positioned in 3D relative to the entity with the `AudioListenerComponent`. Set
`Stream` on music and long ambience: the clip is then read from its file while it plays instead of being loaded whole
(changing it, like changing `Clip`, stops the sound until the next `Play()`).
`TextComponent` draws text in the world or, with `ScreenSpace`, over the image: set `Text` for scores and messages.
Materials of the project are shared by every scene, so scripts change copies: `Material.Clone()` (or
`Material.Create()`) makes one the running scene owns, which `MeshComponent.SetMaterial` gives to a mesh.

```csharp
public class Scoreboard : Script
{
	public Entity? Label;
	private int score;

	public void Add(int points)
	{
		score += points;
		Label!.GetComponent<TextComponent>()!.Text = $"Score: {score}";
		GetComponent<AudioSourceComponent>()?.Play();
	}
}
```

## HUDs and menus

The screen-space UI draws over the game's image. Build it in the editor with Entity > UI: a **Canvas** is the root,
and its descendants are the UI: **Panel**, **Image**, **Text** and **Button** go into a canvas (one is made when the
scene has none). Children draw over their parents and later siblings over earlier ones. Each element's
`UIRectComponent` places it in its parent's rectangle, in canvas units with y pointing down:

- `AnchorMin` and `AnchorMax` say where it attaches, from (0, 0) at the parent's top-left corner to (1, 1) at its
  bottom-right corner. Equal anchors pin it to a point (a corner, the middle of an edge, the center); different ones
  stretch it with the parent.
- `Pivot` is the point of the element that `Position` moves from there, and `Size` its size (added to the anchors'
  span when they stretch, so (-20, -20) leaves a 10-unit margin all around).
- `Visible` hides it with its children; `Opacity` fades them.

The canvas's `ScaleMode` decides how the UI follows the screen's size. `ScaleWithScreenSize`, the default, fits the
whole `ReferenceResolution` on any screen: a UI designed for 1920 x 1080 keeps its proportions in a small window and on
a 4K screen, wider screens add room at the sides and narrower ones (4:3, portrait) above and below, so elements anchored
to opposite corners never overlap. `ScaleWithScreenHeight` fits the height alone (narrower screens cut off the sides),
and `ConstantPixelSize` keeps sizes in pixels.

`UIImageComponent` fills the element's rectangle with a color or a texture (`CornerRadius` rounds it),
`UITextComponent` lays text out in it (wrapped at its width, placed by `Alignment` and `VerticalAlignment`), and
`UIButtonComponent` makes it clickable: its state colors replace the image's color, and a click sets `WasClicked` for
one frame and calls `OnClick(Entity button)` on the scripts of the button and of every ancestor, so one script on a
menu hears all of its buttons. `Click()` clicks a button in code, for keyboard and gamepad menus. Images take the
clicks over whatever they cover; text does not. A press that ends because the window lost the focus (alt-tab, a
notification) is canceled rather than clicked.

`Input.IsPointerOverUI` says whether the mouse is over an image or a button of the UI. A game that shoots, places or
selects with the mouse checks it, so a click on a HUD or menu button does not also act in the world:

```csharp
public class Turret : Script
{
	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsMouseButtonPressed(MouseButton.Left) && !Input.IsPointerOverUI)
		{
			Log.Info("Fire!");
		}
	}
}
```

A HUD that shows the score and a health bar (the fill is an image stretched over the bar, whose right anchor follows the
health). Its number is formatted with `CultureInfo.InvariantCulture` (`System.Globalization`): formatted for the
player's language, numbers can take characters the font lacks, such as the narrow no-break space that groups thousands
in French, which would show as a placeholder glyph:

```csharp
public class Hud : Script
{
	public Entity? Score;
	public Entity? HealthFill;

	public void Show(int score, float health)
	{
		Score!.GetComponent<UITextComponent>()!.Text = string.Create(CultureInfo.InvariantCulture, $"SCORE  {score:N0}");
		HealthFill!.GetComponent<UIRectComponent>()!.AnchorMax = new Vector2(Mathf.Clamp01(health), 1.0f);
	}
}
```

A label that follows something in the world (a name over a character, a marker over a goal) goes where the camera shows
that point: `WorldToScreenPoint` returns its position in pixels from the top-left corner and its depth in front of the
camera, which is negative behind it. A perspective camera shows only what is in front of it, so a label whose point has
a depth of zero or less hides; an orthographic camera shows every point at its position, between its `OrthographicNear`
and `OrthographicFar` depths. In a canvas whose `ScaleMode` is `ConstantPixelSize`, canvas units are pixels, so the
position goes into the label's `UIRect` as it is (here anchored to the top-left corner, with the pivot at its bottom
middle, so the label stands above the point).

Scripts update one after another in no set order, so a label placed in its own `OnUpdate` may use where the camera stood
before it moved this frame, and trail the point by a frame while the camera moves. Place it once the camera has moved:
the script that moves the camera raises an event when it is done, and the label follows it then:

```csharp
public class FollowCamera : Script
{
	public Entity? Target;

	// Raised every frame once the camera stands where it shows the frame from.
	public event System.Action? Moved;

	protected override void OnUpdate(float deltaTime)
	{
		Vector3 goal = Target!.Transform.WorldTranslation + new Vector3(0.0f, 4.0f, 10.0f);
		Translation = Vector3.Lerp(Translation, goal, 1.0f - Mathf.Exp(-5.0f * deltaTime));
		Moved?.Invoke();
	}
}

public class Nameplate : Script
{
	public Entity? Target;
	public Entity? Camera;

	protected override void OnCreate()
	{
		if (Camera?.As<FollowCamera>() is { } camera)
		{
			camera.Moved += Place;
		}
	}

	protected override void OnDestroy()
	{
		if (Camera?.As<FollowCamera>() is { } camera)
		{
			camera.Moved -= Place;
		}
	}

	private void Place()
	{
		CameraComponent camera = Camera!.GetComponent<CameraComponent>()!;
		Vector3 screen = camera.WorldToScreenPoint(Target!.Transform.WorldTranslation + (Vector3.Up * 2.0f));
		UIRectComponent rect = GetComponent<UIRectComponent>()!;
		// The camera is a perspective one: behind it the point has no place on the screen.
		rect.Visible = screen.Z > 0.0f;
		rect.Position = new Vector2(screen.X, screen.Y);
	}
}
```

A pause menu: a panel with Resume and Quit buttons, and this script on the panel:

```csharp
public class PauseMenu : Script
{
	protected override void OnCreate() => SetOpen(false);

	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsKeyPressed(KeyCode.Escape))
		{
			SetOpen(!GetComponent<UIRectComponent>()!.Visible);
		}
	}

	// Every button inside the menu reports here.
	protected override void OnClick(Entity button)
	{
		switch (button.Name)
		{
			case "Resume":
				SetOpen(false);
				break;
			case "Quit":
				SceneManager.LoadScene("Scenes/Title.sscene");
				break;
		}
	}

	private void SetOpen(bool open)
	{
		GetComponent<UIRectComponent>()!.Visible = open;
		Time.TimeScale = open ? 0.0f : 1.0f;
	}
}
```

Title, menu and loading screens need no camera: a scene with a canvas and no camera shows its UI over black. The title
scene that the pause menu's Quit loads can be a canvas with Play and Quit buttons and this script on the canvas:

```csharp
public class TitleMenu : Script
{
	protected override void OnClick(Entity button)
	{
		switch (button.Name)
		{
			case "Play":
				SceneManager.LoadScene("Scenes/Level1.sscene");
				break;
			case "Quit":
				Application.Quit();
				break;
		}
	}
}
```

The UI shows in the game view and the exported game. The scene view draws each canvas standing where its entity is,
faint until the canvas or one of its elements is selected; double-clicking an element in the hierarchy frames it.

## Particles

A `ParticleSystemComponent` makes fire, sparks, smoke, dust or magic: the editor's Entity > Effects menu creates
ready-made Fire, Smoke and Sparks systems to start from. Systems emit `Rate` particles per second while `Emitting`, and
`Burst` particles when they start; scripts emit more at once with `Emit(count)`. For a one-shot effect (an explosion,
a hit), make a prefab with `Emitting` off, a `Burst` and `DestroyWhenFinished`, and instantiate it where it happens:

```csharp
public class Crate : Script
{
	public Prefab? Explosion;

	protected override void OnCollisionEnter(Entity other)
	{
		// The prefab's system bursts once, and its entity goes away with its last particle.
		if (Explosion != null)
		{
			Entity.Instantiate(Explosion, Transform.WorldTranslation, Quaternion.Identity);
		}
		Destroy();
	}
}
```

`Additive` particles add light (bright colors with an `Intensity` above 1 glow through the bloom); `Alpha` particles
cover what is behind them, for smoke and dust.

## Post-processing

The scene's look (exposure, tonemapper, bloom, ambient occlusion, color grading, vignette, chromatic aberration, film
grain) is set in its Scene Settings; a `PostProcessComponent` (the editor's Entity > Rendering > Post Process Volume)
changes it where the camera is. Only the settings whose `Override` property is set change: `OverrideSaturation` and
`Saturation`, `OverrideVignette` and `Vignette`, and so on. A `Global` volume applies everywhere; a local one applies
inside its entity's box (scale the entity to size it) and fades out over `BlendDistance` meters around it, so walking
into a cave or under water changes the look smoothly. Overlapping volumes apply in order of `Priority`, and `Weight`
says how much of a volume applies, which makes short effects easy:

```csharp
public class DamageFlash : Script
{
	private PostProcessComponent? flash;
	private float strength;

	protected override void OnCreate()
	{
		// A global volume with OverrideVignette, Vignette 0.8, OverrideColorFilter and a red ColorFilter, Weight 0.
		flash = GetComponent<PostProcessComponent>();
	}

	public void Hit()
	{
		strength = 1.0f;
	}

	protected override void OnUpdate(float deltaTime)
	{
		strength = Mathf.Max(strength - deltaTime * 2.0f, 0.0f);
		flash!.Weight = strength;
	}
}
```

Numbers blend smoothly with the weight, and so does `Bloom`: a volume that turns bloom off fades the glow out through
its intensity, one that turns it on fades it in. `AmbientOcclusion` and `Tonemapper` switch once a volume applies at
least halfway.

## Time, scenes and the application

`Time.DeltaTime`, `Time.FixedDeltaTime`, `Time.Elapsed` and `Time.FrameCount` tell the time. `Time.TimeScale` scales
the time of scripts and physics: at 0 physics stops and `OnUpdate` gets a delta time of 0, while sounds keep playing.
`SceneManager.LoadScene("Scenes/Level2.sscene")` switches to another scene of the project after the current frame,
`Application.Quit()` ends the game (play mode in the editor), and `Application.IsEditor` tells the editor from an
exported game. `Application.WindowWidth` and `WindowHeight` are the size of the view the game renders to.

## Debugging

`Log.Info`, `Log.Warn` and `Log.Error` write to the editor's console (and to the game's log file),
`Debug.DrawLine(from, to, color, seconds)` draws lines into the view, and exceptions report the file and line of the
script that threw. Players built in the Dist configuration log only warnings and errors and draw no debug lines.

Two of the API's names exist in .NET as well: with `using System;` (or `using System.Diagnostics;`), write
`Strada.Random` (or `Strada.Debug`), or leave that `using` out.

## Visual scripts

A visual script is a graph of nodes instead of code: a `.sgraph` asset that a `VisualScriptComponent` runs on its
entity, beside a C# script or instead of one. Its nodes are events (those of the methods above, keys and buttons,
events of your own), flow control (branches, sequences, loops, delays), variables and functions, operators, and a node
for each member of the scripting API, so graphs reach the engine the way scripts do. Graphs run in the same .NET
runtime as C# scripts but need no C# project: a game can be made of graphs alone.

"+" > Visual Script in the content browser creates a graph (`New Visual Script.sgraph`, with an `Event.Create` and an
`Event.Update` node), double-clicking it opens it on a canvas in the editor (see
[Editing visual scripts](#editing-visual-scripts) below), and dragging it onto entities in the hierarchy, the inspector
or the scene view runs it there. A graph is stored as a document of nodes and links, which agents also edit through
the editor's automation commands (MCP tools, or `strada call` from a shell): `graph.library` lists the kinds of nodes
with their pins, `graph.types` the types, `graph.edit` applies operations (add nodes, connect pins, set values,
arrange) as one undo step, `graph.check` checks a whole document and `graph.set` saves one, and `graph.open` shows a
graph in its window. [Automation.md](Automation.md) describes the document and the commands. This graph, written
whole, turns its entity like the `Spinner` script at the top of this guide:

```json
{
	"Variables": [
		{ "ID": 1, "Name": "Speed", "Type": "float", "Value": 90.0, "Public": true, "Tooltip": "Turn speed in degrees per second" }
	],
	"Graph": {
		"Nodes": [
			{ "ID": 2, "Type": "Event.Update" },
			{ "ID": 3, "Type": "Variable.Get", "Settings": { "Variable": 1 } },
			{ "ID": 4, "Type": "Operator.Multiply" },
			{ "ID": 5, "Type": "Call:Strada.Quaternion.AngleAxis(float,Strada.Vector3)", "Inputs": { "Axis": [0, 1, 0] } },
			{ "ID": 6, "Type": "Get:Strada.Entity.Rotation" },
			{ "ID": 7, "Type": "Operator.Multiply" },
			{ "ID": 8, "Type": "Set:Strada.Entity.Rotation" }
		],
		"Links": [
			{ "From": [2, "Then"], "To": [8, "Execute"] },
			{ "From": [3, "Value"], "To": [4, "A"] },
			{ "From": [2, "DeltaTime"], "To": [4, "B"] },
			{ "From": [4, "Result"], "To": [5, "Degrees"] },
			{ "From": [5, "Result"], "To": [7, "A"] },
			{ "From": [6, "Value"], "To": [7, "B"] },
			{ "From": [7, "Result"], "To": [8, "Value"] }
		]
	}
}
```

Variables, functions and nodes share one numbering of IDs. A node's `Type` is a kind `graph.library` lists, its
`Settings` configure it (which variable `Variable.Get` reads) and its `Inputs` hold the values of inputs no link feeds
(the axis). Links go from an output to an input, each named by its node and pin. `Execute` and `Then` are flow pins: the
flow starts at the event and runs `Set Rotation`. The other nodes are pure: they have no flow pins and work out their
outputs whenever a node that runs needs them, here the turn from the speed and the frame's `DeltaTime`, and the current
rotation. The `Target` of an entity's or a component's member, left unlinked, is the entity the graph runs on. Nodes
need no `Position`: `graph.set` places those without one.

An agent builds such a graph with `graph.edit` instead, in a graph `graph.create` made from the Events template (node 2
is its `Event.Update`): names stand for the variables, `"$labels"` for the nodes it adds, two Expressions do the maths,
and Arrange lays it out. This batch keeps an angle of its own, because `EulerAngles` reads back one of several sets of
angles for a rotation, so adding to the angles read would stall:

```json
{ "graph": "asset://Scripts/Spinner.sgraph", "description": "Spin at Speed degrees per second", "operations": [
	{ "op": "AddVariable", "name": "Speed", "type": "float", "value": 90.0, "public": true, "tooltip": "Degrees per second" },
	{ "op": "AddVariable", "name": "Angle", "type": "float" },
	{ "op": "AddNode", "type": "Variable.Get", "settings": { "Variable": "Speed" }, "as": "$speed" },
	{ "op": "AddNode", "type": "Variable.Get", "settings": { "Variable": "Angle" }, "as": "$angle" },
	{ "op": "AddNode", "type": "Expression", "settings": { "Formula": "(angle + speed * dt) % 360" }, "as": "$next" },
	{ "op": "AddNode", "type": "Variable.Set", "settings": { "Variable": "Angle" }, "as": "$store" },
	{ "op": "AddNode", "type": "Expression", "settings": { "Formula": "vec3(0, a, 0)" }, "as": "$euler" },
	{ "op": "AddNode", "type": "Set:Strada.TransformComponent.EulerAngles", "as": "$turn" },
	{ "op": "Connect", "from": ["$angle", "Value"], "to": ["$next", "angle"] },
	{ "op": "Connect", "from": ["$speed", "Value"], "to": ["$next", "speed"] },
	{ "op": "Connect", "from": [2, "DeltaTime"], "to": ["$next", "dt"] },
	{ "op": "Connect", "from": ["$next", "Result"], "to": ["$store", "Value"] },
	{ "op": "Connect", "from": ["$store", "Value"], "to": ["$euler", "a"] },
	{ "op": "Connect", "from": ["$euler", "Result"], "to": ["$turn", "Value"] },
	{ "op": "Connect", "from": [2, "Then"], "to": ["$store", "Execute"] },
	{ "op": "Connect", "from": ["$store", "Then"], "to": ["$turn", "Execute"] },
	{ "op": "Arrange", "scope": "Graph" }
] }
```

Public variables show in the inspector below the component's `Graph`, where each entity gives them values of its own
(the component's `Variables`, saved with the scene like script fields), so one graph serves many entities: this one
turns each at its own speed. They take the types C# fields take, named by type IDs (`bool`, `int`, `float`, `string`,
`Strada.Vector3`, `Strada.Color`, `Strada.Entity`, `Strada.Material`, enums such as `Strada.KeyCode`; `graph.types`
gives those types a `FieldType`). Graphs hold no entities themselves, as every entity that runs a graph shares it: an
entity a graph needs comes from a public `Strada.Entity` variable its component sets, from `Self`, an event's `Other` or
a lookup such as `Entity.FindByName`.

| Event node | Runs |
|------------|------|
| `Event.Create`, `Event.Update` (`DeltaTime`), `Event.FixedUpdate` (`FixedDeltaTime`), `Event.Destroy` | When a script's `OnCreate`, `OnUpdate`, `OnFixedUpdate` and `OnDestroy` would |
| `Event.CollisionEnter`, `Event.CollisionExit`, `Event.TriggerEnter`, `Event.TriggerExit` (`Other`) | When the contacts of the C# methods of those names happen |
| `Event.Click` (`Button`) | When a UI button that is the entity or one of its descendants was clicked |
| `Event.KeyPressed`, `Event.KeyReleased` (setting `Key`), `Event.MouseButtonPressed`, `Event.MouseButtonReleased`, `Event.GamepadButtonPressed`, `Event.GamepadButtonReleased` (setting `Button`) | When the key or button went down or up this frame, before `Event.Update` |
| `Event.Custom` (settings `Name` and `Parameters`, which become outputs) | When a graph's `Event.Send` or a C# script's `SendEvent` raises it |

A graph has each event once (each key or button once, each custom event name once). An entity can run a C# script and a
graph: each event reaches its C# script first, and as for scripts, every script and graph of the scene exists before the
first `OnCreate` or `Event.Create`. `Flow.Delay`, `Flow.RetriggerableDelay`, `Flow.WaitFrames` and `Flow.WaitUntil` make
the flow wait without holding up the game: it goes on at their `Completed` output in a later frame, its loops and
sequences where it left them. `Flow.Tween` runs its `Update` output every frame for a duration, with an eased `Alpha`,
and `Flow.Interval` its `Tick` output at intervals.

A VisualScript component runs one graph. To give an entity several behaviors, put each on a child entity with a
component of its own ("Spin", "Pickup", "Health"): `Self` in a child's graph is the child (`Get:Strada.Entity.Parent`
the entity it belongs to), and graphs talk through custom events. `Event.Send` runs the `Event.Custom` of the same
`Name` and parameter types in the graph of its `Target` at once, passing its inputs; a target without such an event only
logs a warning. A coin that gives its points to whatever touches it, then destroys itself:

```json
{
	"Variables": [
		{ "ID": 1, "Name": "Points", "Type": "int", "Value": 10, "Public": true }
	],
	"Graph": {
		"Nodes": [
			{ "ID": 2, "Type": "Event.TriggerEnter" },
			{ "ID": 3, "Type": "Variable.Get", "Settings": { "Variable": 1 } },
			{ "ID": 4, "Type": "Event.Send", "Settings": { "Name": "Collected", "Parameters": [{ "Name": "Points", "Type": "int" }] } },
			{ "ID": 5, "Type": "Call:Strada.Entity.Destroy()" }
		],
		"Links": [
			{ "From": [2, "Then"], "To": [4, "Execute"] },
			{ "From": [2, "Other"], "To": [4, "Target"] },
			{ "From": [3, "Value"], "To": [4, "Points"] },
			{ "From": [4, "Then"], "To": [5, "Execute"] }
		]
	}
}
```

The player's graph counts them:

```json
{
	"Variables": [
		{ "ID": 1, "Name": "Score", "Type": "int", "Public": true }
	],
	"Graph": {
		"Nodes": [
			{ "ID": 2, "Type": "Event.Custom", "Settings": { "Name": "Collected", "Parameters": [{ "Name": "Points", "Type": "int" }] } },
			{ "ID": 3, "Type": "Variable.Get", "Settings": { "Variable": 1 } },
			{ "ID": 4, "Type": "Operator.Add" },
			{ "ID": 5, "Type": "Variable.Set", "Settings": { "Variable": 1 } }
		],
		"Links": [
			{ "From": [2, "Then"], "To": [5, "Execute"] },
			{ "From": [3, "Value"], "To": [4, "A"] },
			{ "From": [2, "Points"], "To": [4, "B"] },
			{ "From": [4, "Result"], "To": [5, "Value"] }
		]
	}
}
```

C# scripts reach an entity's graph through its `VisualScriptComponent`: `SendEvent(name, arguments)` raises a custom
event, `HasVariable(name)`, `GetVariable<T>(name)` and `SetVariable<T>(name, value)` read and write its variables
(public or not), and `Graph` is the graph it runs (`Assets.Load<VisualScript>(path)` loads one; a new graph starts at
the scene's next update). A label that shows the player's score, with a key that adds a bonus:

```csharp
public class ScoreLabel : Script
{
	[Tooltip("The entity whose visual script counts the score")]
	public Entity? Player;

	protected override void OnUpdate(float deltaTime)
	{
		VisualScriptComponent? graph = Player?.GetComponent<VisualScriptComponent>();
		if (graph is null || !graph.HasVariable("Score"))
		{
			// The player runs no graph with a score.
			return;
		}
		if (Input.IsKeyPressed(KeyCode.B))
		{
			// Runs the graph's Collected event at once, as a coin's Event.Send does.
			graph.SendEvent("Collected", 100);
		}
		GetComponent<UITextComponent>()!.Text = $"Score: {graph.GetVariable<int>("Score")}";
	}
}
```

`GetVariable<T>` and `SetVariable<T>` convert values only to their own type, a wider number (`int` to `long`, `float` or
`double`), a base class or `object`, and a component to its `Entity`. `SendEvent` converts its arguments as links
between nodes convert values, which also turns any value into text for a `string` parameter and a typed asset into its
`AssetHandle`; it returns false with a warning, running nothing, when the entity runs no graph, its graph has no such
event or the arguments do not fit its parameters. The variable methods throw: `InvalidOperationException` when the
entity runs no graph, `ArgumentException` for a variable the graph does not have, and `InvalidCastException` for a value
that does not convert. An error in an event raised from C# reaches the caller as an `InvalidOperationException`.

A graph that does not make sense does not run: `graph.check` and `graph.set` report every problem with the node it
concerns (the codes are listed in [Automation.md](Automation.md)), and the console names the first one
(`Visual script 'Scripts/Door.sgraph' has 2 errors; first: ...`) and each entity left without its graph
(`Entity 'Door' runs no visual script: ...`). An error while a graph runs (a member call that throws, a `Target`
without the component, an integer division by zero, more than 1,000,000 nodes in one go, calls nested deeper than 64)
stops that flow and is logged with the graph, the node and the entity; like an exception in a C# script, it counts as a
failure in test runs, and the game goes on.

While the game plays in the editor, the inspector shows the values the public variables of a running graph hold, and an
edit reaches the graph at once, as for script fields. A graph saved while the game plays (`graph.set`) changes its
running instances at their next update: variables keep their values (a new variable, or one whose type changed, starts
at its `Value`; one renamed or added and edited before that update takes the edited value), waiting nodes keep waiting
while they still exist, and a graph saved with an error keeps running its last working version.

### Expressions

An `Expression` node computes a formula such as `a * b + sin(t)`, in place of a chain of operator nodes. Every name in the
formula that is not followed by `(` becomes an input of that name, in the order the names first appear: `distance /
speed` has the inputs `distance` and `speed`, `exp + 1` the input `exp`, and `v.X` the input `v` only. `true`, `false`,
`pi` and `tau` are constants. An input you link takes the type of its link (`bool`, `int`, `uint`, `long`, `ulong`,
`float`, `double`, `Vector2`, `Vector3` or `Vector4`); an input you leave unlinked is a `float`, and you can type its value
on the node.

- Integers divide as integers (`7 / 2` is 3; `%` keeps the sign of the dividend) and floats as floats (`7 / 2.0` is 3.5).
  Mixed numbers widen as links do: `int` and `float` give `float`, `int` and `uint` give `long`; `int` and `ulong` have no
  common type, so convert one with `long(...)` or `double(...)`.
- Write `1e40d`, or any number ending in `d`, for a `double`; a float literal is at most 3.4028235e38. Vectors add,
  subtract, multiply and divide component by component, and multiply or divide by a number.
- Functions: `abs`, `sign`, `min`, `max`, `clamp`, `floor`, `ceil`, `round` (half to even), `trunc`, `frac`, `sqrt`, `exp`,
  `log`, `log2`, `log10`, `pow`, `atan2`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `radians`, `degrees`, `lerp` (not
  clamped), `smoothstep`, `step`, `length`, `normalize`, `dot`, `distance`, `cross`, and `vec2`, `vec3` and `vec4`. Casts:
  `int`, `uint`, `long`, `ulong`, `float` and `double`; a float cast to an integer is truncated and saturated, and NaN gives 0.
- `.X`, `.Y`, `.Z` and `.W` (or `.x`, ...) read a vector's components. `&&`, `||` and `?:` evaluate only what they need.
  There is no `^`: use `pow(a, b)`.
- An integer division or modulo by zero stops the execution with a message naming the part of the formula ("integer
  division by zero in a / b").
- Mistakes are reported on the node with the part of the formula they are about; messages about brackets and `?:` give
  the line and column (columns count characters, a tab is one). A graph keeps running its last working version while you
  fix a formula.
- An Expression is pure and costs one step, however long its formula. A formula has at most 4096 bytes, 1024 parts, 64
  levels of nesting and 32 inputs; a name has at most 64 bytes.

### Text and arrays

The `Utilities/String` nodes (subtitled String) work on text: Length, Is Empty, Contains, Starts With, Ends With, Index
Of, Substring, Replace, To Upper, To Lower, Trim, Split, Join, To Int and To Float. They compare ordinally (case matters),
change case the same way in every language, count UTF-16 code units (as C#'s `string.Length` does) and never stop the
graph: Index Of clamps `StartIndex` and gives -1 when nothing is found (`""` is found at the clamped start); Substring
clamps `Start` and `Length`, and a negative `Length` takes the rest; Replace with an empty `Old` changes nothing; Split's
`Separator` is `,` by default, an empty one gives the whole text and Remove Empty drops empty parts; Join writes the
elements as text (None for no entity) with `Separator`, `, ` by default. To Int reads an optional sign and digits, To
Float a sign, digits, one decimal point and an exponent, white space around them allowed, the same in every language;
text that does not read, overflows or is not finite gives 0 with `Success` false.

Every type has an array type (`float[]`, `Strada.Vector3[]`, ...), but there are no arrays of arrays. Arrays can be
private variables, locals, function inputs and outputs, custom-event parameters, `Literal` values and type arguments.
Public variables cannot be arrays yet, as components cannot store them: keep the variable private and fill it in the
graph (Make Array from public variables, or the `Children` of a child entity that holds the points). An array is never
None: an array left unset, or one C# gives as null, is empty, and Is Valid of an array is always true. Arrays compare
element by element (NaN equals nothing), read as text like `[1, 2, 3]`, and a `Player[]` is not an `Entity[]`.

The `Utilities/Array` nodes (subtitled Array) never change an array: Set Element, Add Element, Remove Element, Insert
Element (before `Index`; an `Index` equal to the length appends) and Append Arrays (A's elements, then B's, in one copy)
give new arrays of the graph's element type, even when C# gave an array of a derived type. Make Array builds one from
`Count` elements (0 to 64) of its `Type`, or of the type its linked elements share (`int` and `float` give `float[]`);
without links, set `Type`. Contains and Index Of search an array, Length and Get read it. An array literal is a JSON
array of at most 4096 element values, and arrays made in graphs hold at most 65,536 elements. C# code that changes an
array it got from a graph changes it for the graph too: copy it first.

Generic methods whose type parameters have no constraints (`Assert.AreEqual<T>`, `AreNotEqual<T>`) take their type from
their links: an input of type `T[]` decides `T` as its element type; otherwise the inputs of type `T` join as Select's do
(`int` and `float` give `float`; `int` and an entity have no common type). An output of type `T[]` is the array of that
type. Setting `TypeArguments` fixes the types instead, arrays included; a method whose type parameter appears in no input
needs it.

### Timelines

A `Flow.Timeline` plays curves over `Length` seconds (more than 0, at most 3600) and can `Loop`. Its tracks (at most 32)
are float, Vector2, Vector3, Vector4 or Color curves, each a data output of its type, or event tracks, each an exec output
that fires at its keys; each track has at most 1024 keys in time order. Curve keys have a time and a value, an
interpolation to the next key (Cubic, the default, Linear or Constant) and optional in and out slopes (automatic
otherwise); keys at one time make a jump. Automation writes the setting as JSON ([Automation.md](Automation.md) shows its
form); the editor's Timeline drawer edits it (below).

- Inputs: Play and Reverse continue from where it is; PlayFromStart and ReverseFromEnd jump to the start or the end;
  Stop pauses where it is; SetTime moves to `NewTime` (clamped) without firing anything. `Rate` scales the speed and is
  read when it starts playing; 0 or less holds it still. Each input ends its chain.
- While it plays, every update fires the event keys it crossed (in playing order), then `Update`. At the end of a
  timeline that does not loop, `Playing` becomes false and `Finished` fires, which may play it again. Looping timelines
  never finish: in a loop the end and the start are one moment, so playing forward a key at 0 fires every cycle and a key
  at `Length` never (backward the reverse).
- Outputs: `Time`, `Direction` (`TimelineDirection.Forward` or `Backward`), `Playing` and each curve's value; before it
  plays they hold the values at time 0.
- At most 1000 event keys fire in one update; the time beyond is dropped with one Warning.
- Timelines belong in the event graph. Editing a playing timeline changes its motion at the next frame without
  restarting it (its time is clamped into a shorter `Length`), so you can tune it live; disabling it stops it, and a
  disabled timeline passes its data inputs through by type, as every disabled node does: `Time` takes `NewTime`, the
  first float curve `Rate`, the other outputs their defaults.

### Your own nodes

Mark a public method, property, field or constructor of your game's scripts with `[VisualNode]` and it becomes a node,
in the palette's Game category (`Game/<type name>`, or the `Category` you give). Mark a class or struct and every public
member declared on it becomes one; mark an enum and graphs can use it (variables, switches, literals, public variables).
Types a marked member uses become types of graphs too.

```csharp
using Strada;

namespace Game;

public static class Combat
{
	[VisualNode(Pure = true, Category = "Game/Combat", Description = "The health a hit leaves, never below 0")]
	public static float Hit(float health, float damage) => health > damage ? health - damage : 0.0f;
}

public class Door : Script
{
	private bool m_Open;

	[VisualNode(Pure = true)]
	public bool IsOpen() => m_Open;

	// Graphs made when it was called Unlock keep working.
	[VisualNode(Title = "Open Door", FormerNames = ["Unlock"])]
	public void Open()
	{
		m_Open = true;
	}
}
```

- `Title` (1 to 128 characters) replaces the member's name in words (properties and fields: "Get <Title>" and "Set
  <Title>"); `Category` is 1 to 8 names of 1 to 64 characters separated by `/`; `Description` has at most 4096
  characters; each of `Keywords` 1 to 64. On a type, only `Category` applies, to the members that name none.
- Methods have flow pins unless `Pure = true`, which needs a result or `out` parameters; getters are pure, setters never.
- An instance member of a script class takes a `Target`, an entity (the graph's own when unlinked), and runs on that
  entity's script; on an entity whose script is of another class the graph stops with "<entity> has no <class> script".
  Script classes' constructors have no node.
- Not supported: `ref`, `in` and `params` parameters, pointers, delegates, ref structs, and members that return `Task`,
  `ValueTask` or `IEnumerator`. Members run to their end, so a member cannot make a graph wait: start the work in C# and
  give graphs a property to wait on with Wait Until (`[VisualNode] public bool IsLoaded`).
- Renaming: `FormerNames = ["Heal"]` (the same type) or `["Game.Hero.Heal"]` (moved to another type; for a constructor,
  the type's former full name) keeps graphs that use the old name working, and they store the new one at their next
  save. A former name that is another node's type is ignored, with a note.
- Every marked member that has no node, every attribute value outside its limits and every type or member whose
  attributes cannot be read (a dependency of the game that is not deployed) is a note of the node library, logged once
  when the library is built (the Console) and listed at the end of the palette's Game category ("3 [VisualNode] members
  have no node: see the Console") and by `graph.library`'s `notes`; the rest of the library builds as usual. A game type
  with the full name of a scripting API type is not a type of graphs.

### Function libraries

A function library is a visual script whose kind is Library ("+" > Visual Script Library in the content browser,
`graph.create` with the template `Library`, or Details' Kind with nothing selected). It runs on no entity: it has no
variables and no event graph, only functions (and comment boxes). Other graphs call its functions with a Call Function
node whose Graph setting names the library; the call takes the function's inputs, outputs, purity and waiting. `Self`, and
the unlinked `Target`s inside a library function, mean the entity whose graph called it. A library is the place for
maths and helpers several graphs share.

- A library function may wait (Delay, Wait Until), send events and call other libraries' functions; Tween, Interval and
  Timeline belong in event graphs. Libraries cannot call each other in a cycle (`LibraryCycle`). A call of a library
  that has an error is an error itself (`ReferenceError`), and the graph keeps running its last working version.
- Renaming a library function's input or output keeps its former name, so graphs that still use the old name keep
  working, and their next save stores the new one. Removing an input or output, or the function, breaks its callers:
  the editor tells you which scripts call it (below).
- Each graph that calls a library runs its own copy of the library's functions. Editing the library reloads every graph
  that calls it, also while playing: running graphs swap to the new version, and an execution waiting inside a library
  function keeps waiting while the function still has its nodes. When an edit cancels one, the warning names the
  library's file ("the execution waiting in Delay (node 12) of Scripts/Motion.sgraph is cancelled: the edit disabled
  Delay (node 12) of Scripts/Motion.sgraph"). A broken library leaves the graphs that call it running the version that
  worked, and errors in a library function are logged with the library's path.
- Dropping a library on an entity is refused ("Motion is a function library: graphs call its functions; it runs on no
  entity"), and an entity whose VisualScript component names one shows that sentence instead of variables and runs
  nothing. The asset inspector says when a visual script is a function library and which scripts use it ("Used by
  Player.sgraph (3 nodes)"), and deleting a graph other scripts use names them ("they will show errors").

### Talking to other graphs

`Event.Send` raises a custom event by name, as above. Its optional Graph setting names the script that should receive
it: with Graph set, the event reaches only an entity that runs that script (any other is skipped with a warning, once per
node and entity), and the editor checks the Send against that script's custom event. A missing script, a missing event
(with suggestions), other parameter types or a disabled event is a warning, never an error: the event is still sent by
name, so a typed Send is never required. Link its `Target`: a typed Send whose `Target` is left unlinked in a script sends
to Self, which runs this script, not the one Graph names (`TargetIsSelf`).

`Event.Broadcast` raises a custom event on every graph that has an event of that name with exactly these parameter types:
a project-wide message such as GameOver. With Graph set, it reaches only entities running that script, checked as a typed
Send is. The receivers are the instances alive when the node runs, in the order the entities started running their
graphs; the broadcasting entity is one of them unless IncludeSelf is off. Each receiver runs the event as Send's receiver
does (it may wait, send or broadcast again) and gets the arguments of the broadcast that reached it, also when a receiver
broadcasts the same event again from the same node. An entity destroyed by an earlier receiver is skipped, one started
during the broadcast is not reached, and an error in a receiver stops the broadcast and the graph that broadcast it.
`Count` is how many received it (none is normal and logs nothing). Every instance it examines costs one step of the
execution's budget of 1,000,000, and a pure function cannot broadcast.

Get Variable and Set Variable have an optional Graph setting naming another script: with it they take a `Target` entity
(Self when unlinked) and read or write that entity's variable. The variable must be public there; public is the one
access rule, and it is also what makes a variable editable on components. A graph naming itself may also use another
entity's private variables. In a script, an unlinked `Target` with Graph naming another script is an error
(`UnlinkedTarget`): Self runs this script, so link the entity to read from or write to; in a library function an unlinked
`Target` is the entity whose graph called it. While the game runs, the read or the write stops the execution with an
error when the `Target` is None, the entity runs no visual script, it runs another script ("Window (9) runs
Scripts/Window.sgraph, not Scripts/Door.sgraph"), the variable is gone ("Scripts/Door.sgraph has no variable Health any
more"), its type changed ("Door's Health is a string now, not a float") or it became private. The node finds the
variable by its ID, and by its name after a rename that changed the ID, so renaming Health in Door keeps readers working.

`Runs Graph` tells whether its `Target` (Self when unlinked) runs the script Graph names: false for None and for an entity
that runs no or another visual script. It is the guard for Get and Set of another script's variables. A library is never
run by an entity, so naming one is an error; in a script, an unlinked `Target` with Graph naming another script makes it
always false (`TargetIsSelf`): link the entity to test.

A graph is compiled with the other graphs it uses: the libraries it calls (and the libraries those call) are compiled
into it, and the graphs whose variables it reads or writes are checked by their kind and variables. Renaming or moving a
graph that others use, or one appearing or disappearing, recompiles the graphs that refer to it; changing another
graph's layout never does. A graph refers to at most 256 other graphs, its libraries' references included; the others
are treated as missing ("more than 256 visual scripts are referenced"). When an edit renames or deletes what other
scripts use, the editor says what happens to them (below), and agents' `graph.edit` and `graph.set` list those nodes in
`referrers`.

### Editing visual scripts

**Opening a script.** Double-click a visual script in the content browser (or use its Open menu item, the Open button
of the inspector that shows it, or Open Visual Script on a VisualScript component) and it opens in a graph window of
its own, docked beside the scene view. An entity without a graph gets one from its component's Create Visual Script,
which writes `Assets/Scripts/<entity name>.sgraph`, runs it on the selected entities and opens it. Each script has one
window, and the editor reopens the windows a project had open the next time it opens. The window holds the canvas in
the middle, **My Script** on the left (the script's graphs, variables, locals and custom events), **Details** on the
right (what is selected) and a toolbar; View > Visual Scripts lists the open windows. Scripts save themselves: every
edit is written to the file at once, the toolbar says "Saved" (or "Not saved" when a write failed), and Ctrl+Z undoes it
like any other edit, from any window. A script whose file is read-only can be looked at and copied from, not edited.

**Moving around.** Drag with the right or middle mouse button, or hold Space and drag, to pan. With a wheel mouse the
wheel zooms to the cursor, Ctrl+wheel pans up and down and Shift+wheel sideways. On a trackpad, open the toolbar's view
menu (the three dots) and set Scroll to Pan: two fingers then pan, and Ctrl+scroll (Cmd on macOS) zooms; Pan is the
default on macOS. F frames the selection (everything when nothing is selected) and Home everything. Zoomed far out,
nodes become simple boxes you can still select and move; zoomed in, every value shows on its node. Back and Forward in
the toolbar (Ctrl+[ and Ctrl+], or the mouse's side buttons) retrace where you looked, across functions too.
Ctrl+Alt+1 to 9 stores the view as a bookmark and Ctrl+1 to 9 goes back to it; the toolbar's Bookmarks menu renames
and deletes them. Bookmarks, views and pane sizes are yours alone: they are kept in the editor's user data, never in the
script.

**Adding and wiring nodes.** Press Tab, tap Space or right-click the empty canvas to open the palette, type part of a
node's name ("add force", or the initials "af") and press Enter: the node appears where you opened the palette. Holding
Space and typing opens it with what you typed. Drag from a pin to another pin to link them; the wire turns red and
its tooltip says why when the two cannot connect; a wire shows a gradient where a value converts on its way (an `int`
into a `float`). Release a wire on empty canvas and the palette lists only the nodes that connect to it, and links the
one you choose. Ctrl+drag a linked pin to move its links, Alt+click a pin or a wire to break links, Ctrl+right-drag
across wires to cut them, and double-click a wire to add a reroute point. Select with a click or by dragging a rectangle
(Shift adds; Ctrl toggles a clicked node and removes what a rectangle covers), move the selection by dragging (it snaps to the grid; hold Alt to place it freely), nudge it
with the arrow keys, and tidy it with the toolbar's Arrange or the align keys (Shift+W, A, S, D) and Q, which
straightens a flow. Ctrl+C, Ctrl+X, Ctrl+V and Ctrl+D copy, cut, paste and duplicate, also between scripts. Delete
removes the selection and keeps the flow going through what it removed (a node between two others no longer breaks the
chain); Shift+Delete removes it without that. F1, or `?` in the toolbar, lists every key and mouse gesture.

**Values.** An input that nothing is linked to shows its value on the node; click it to change it (drag a number,
pick an enum, a colour or an asset), or select the node and change it in Details. Drop an asset from the content browser
onto an input to use it, or onto the canvas to get a value node holding it; drop an entity from the hierarchy onto the
canvas to give the graph a public variable for it, set on the entity this window shows (below). Right-click a pin for
Reset to Default, Promote to Variable and its links.

**Variables, functions and custom events.** The "+" of each section of My Script adds a variable, a function, a local
(while a function is shown) or a custom event and lets you name it at once. Drag a variable onto the canvas for a Get
or a Set (hold Ctrl for a Get, Alt for a Set), or onto a pin. Details sets a variable's type, default value, tooltip
and range and whether it is public (public variables show in the inspector, where each entity sets its own values); a
function's inputs, outputs, locals and purity; a custom event's parameters. Double-click a function call to open the
function's graph, and a Send to go to the custom event it raises in this script. Deleting a variable
or function that nodes use asks first and removes those nodes too, keeping the flow through them. Renaming keeps every
use, and a variable remembers its former names, so the values entities stored keep applying.

**Comments.** Press C to put a comment box around the selection (or a new one at the cursor) and type its title; moving
the box by its title moves what is inside it (hold Alt for the box alone). Right-click a node and choose Add Node
Comment... to add a note shown above it (Edit Node Comment... once it has one); double-click either text to edit it in
place. H collapses selected nodes to their header.

**Split pins.** Right-click a vector, colour or other struct pin and choose Split Struct Pin to show one pin per field
(a `Vector3` input becomes `X`, `Y` and `Z`), so you can link or set each field on its own; Recombine Struct Pin puts it
back. Outputs split the same way (a raycast's hit point, then its `Y`).

**Disabling nodes.** M disables the selected nodes (or enables them again): a disabled node is dimmed and passes the
flow on to its next node without running, and passes its values through where it can, so you can switch part of a graph
off without unlinking it. A disabled node's errors become warnings, so disabling a broken node keeps its graph running.
Details also offers Development Only: such a node runs in the editor and in test runs, and is left out of exported
games (a log line or a cheat key). Entry and Return nodes and nodes without flow pins cannot be disabled.

**Problems and quick fixes.** Nodes with problems get a badge, and the toolbar counts errors and warnings; click either
to see the messages, and their fix buttons where a fix is known: use the suggested node or pin, insert a conversion,
keep the first of two links, add a missing Return, and more. A script with an error does not run (while playing, the
game keeps the last version that worked), so fix errors first; warnings can wait. The problems drawer under the canvas
lists everything, errors first; click a row to go there.

**Find.** Ctrl+F finds text in the script (titles, values, comments, names): Enter goes to the next match and selects
it, and the find field keeps the keyboard; Escape closes the bar and gives the keyboard back to the canvas, where
Delete, F2, Ctrl+X or Ctrl+D act on what you found. Ctrl+Shift+F searches every script of the project. Find References
on a variable, Find Callers on a function and Find Senders on a custom event (in their right-click menus in My Script)
list their uses; senders are found in every script.

**Editing while playing.** Keep the graph window open while the game plays. Graph windows open as tabs beside the Scene
View and the Game View, and Play brings the Game View to the front (and Stop brings the Scene View back): to watch the
game while you edit, drag the graph window's tab to the side of the editor (or onto another panel's edge) so both show.
Every edit reaches the running game at once (the toolbar's LIVE pill says so), variables keep their values, and waiting
nodes keep waiting while they still exist. When an edit leaves an error, the pill turns amber ("last working version")
and the game goes on with the last version that worked until you fix it. The toolbar's Instance list picks which entity
running the script the window shows: My Script then shows its public variables' current values, and dropped entities set
its values. Edits made while playing stay in the script after Stop, and stay undoable.

**Formulas.** Type a formula in an Expression's Formula field in Details (a monospace field of a few lines), or
double-click the node's header to type it on the node. Ctrl+Enter, Enter on the node, or leaving the field commits it;
Escape cancels. Retyping a name renames the input and keeps its link, and the Inputs row under the field lists the
inputs with their types. While only numbers and operators change, a running game gets the formula as you type.
Problems show under the field with `^` under the part they are about; a click selects that part, and "Use sin" fixes a
misspelled function in place. A formula Details cannot take (over 4096 bytes) is refused with "The formula stays:
<why>"; after Ctrl+Enter the text stays in the field to be fixed. In the palette, type `=` and a formula
(`=a*b+sin(t)`) to add an Expression of it.

**Arrays.** Details edits an array's elements: a variable's or local's default, a Literal's value and an unlinked array
input (+ Add Element; drag an element's handle onto another handle of the same array to move it there, while a drop
onto another array's handle moves nothing; x removes it). Make Array's Count adds and removes element pins, keeping their
links. On the canvas a chip shows an unlinked array input's length. My Script's eye refuses to make an array public
("Arrays cannot be public: components cannot store them"), and the inspector lists a public array a graph declares as
read-only ("arrays cannot be set on components").

**Timelines.** Double-click a Timeline's header, choose Edit Timeline in its menu, or click Edit on a track in Details,
to open its tracks and curves in the drawer under the canvas. On the left, + Add Track adds a Float, Vector2, Vector3,
Vector4, Color or Event track; double-click a name to rename it (its links follow); its menu has Rename, Change Type and
Remove; the eye hides a track, and X Y Z W (R G B A) hide components. On the right, click a key to select it (Shift adds,
Ctrl toggles), or drag on empty space for a rectangle. Drag keys to move them as a block: times snap to 0.01 s (Alt:
free), Shift keeps one axis, and a block stops at the next key that is not selected and at the timeline's ends. Drag a
selected cubic key's handle to set both its slopes, or with Alt only that one. Double-click a curve to add a key with
the curve's value there, or a lane to add an event. Right-click a key for Interpolation (Cubic, Linear, Constant),
Tangents (Auto, Flat) and Delete. The wheel zooms time, Ctrl+wheel zooms values, a right or middle drag pans, and F
frames the selected keys (everything without any). While the drawer has the keyboard, Delete removes keys, Ctrl+A selects
the visible tracks' keys, and the arrows move the selected keys by 0.01 s (Shift: 0.1 s); presses less than half a
second apart are one undo step. Esc while dragging keys or a slope cancels the drag (the keys go back where they were and
no undo step is left), Esc during a rectangle gives back the selection you had, and otherwise Esc gives the keyboard back
to the canvas. The selection, the eye and the component toggles follow tracks that are renamed, or removed elsewhere
(Details, undo, an agent), and tracks with very large values (up to the largest floats) frame and draw like any other.
Details shows the Length (lowering it moves later keys onto the new end in the same step, and dragging it back up
restores them), Loop, and a row per track with a preview. While playing, a dragged key reaches the running timeline at
the next commit, within 0.1 s, without restarting it.

**Other graphs.** Nodes that name another graph get it in Details: the Graph picker lists the project's scripts or
libraries, as the setting takes, with "None (this script)" or "None (any receiver, by name)" first (a graph the project
no longer has shows "Missing (8146…)"). With a Graph, the Variable list shows that script's public variables (every
variable when it is this script), the Function list the library's functions, and Send and Broadcast get an Event list
that sets their Name and Parameters. Each change is one undo step that removes only the links whose pins went ("Set
Graph on Send Hit (removes 1 link)"); None makes a Send untyped and keeps its parameters. With nothing selected, Details
shows the script's Kind (Script or Function Library): making a script with variables or event nodes a library, or a
library other scripts call a script, asks first and says what will show errors ("Make Motion a script? 3 nodes in 2 other
scripts call its functions: they will show errors until fixed."), and a toast says it again afterwards. A public
variable shows "Used by N nodes in M other scripts" and a library's function "Called by N nodes in M scripts", with
[Find Callers]. The palette lists the functions of the project's libraries (under Libraries/<name>), the other scripts'
typed Send and Broadcast of their events, Get and Set of their public variables and Runs <name> (under
Scripts/<name>), an untyped Broadcast of this script's events, and "Get/Set X of another <script>"; typing a category
word finds entries ("health door" finds Get Health). A library's palette offers no variables or custom events of its
own. Double-clicking a node that names another graph, or its menu's first item, opens that graph's window: "Open Clamp01
in Motion", "Go to Event Hit in Door", "Select Variable Health in Door" (which selects it there), or "Open Door" when
that graph lacks what the node names; a node naming a graph the project does not have shows in Details. Back in that
window returns to the caller's window and view (opening it again if it was closed), and Forward goes there again. F2 and
Find References on such a node show nothing of this script; Find References lists the other graph's element across every
script. Renaming a custom event renames this script's untyped Sends and Broadcasts and those typed with this script.

After an edit that changes what the graph offers others, a toast says what happens to their nodes, with a button that
lists them: "Renamed Value to Amount: 3 calls in 2 other scripts follow it", "Deleted Clamp01: 1 call in 1 other script
show errors", "Made Speed private: 2 nodes in 1 other script show errors" (Find Callers, Find References or Find
Senders). Deleting a variable, function or event other scripts use asks first, counting them: "2 nodes here use it
(they are deleted too). 3 nodes in 2 other scripts use it: they will show errors until fixed.", with [Delete], [Find
References] and [Cancel]. A library's window opens on its first function, and My Script shows "Function library · used
by N nodes in M scripts" and its Functions (an event graph, variables or custom events left in it are shown so they can
be deleted, with no + to add more); an empty library shows "This library has no functions yet." with [Add Function].
While playing, a broken library's pill reads "LIVE · last working version", and its tooltip says that the graphs that
call it keep running the last version that worked.

**The window's layout.** My Script and Details stand beside the canvas when both fit with a canvas of 40 font sizes;
when the window is narrower, they stack in one column on My Script's side (My Script above Details, a splitter between
them), and in a window narrower still Details and then My Script hide and open over the canvas when needed. At the
farthest zooms comment titles are drawn above the nodes, on a plate of the comment's colour, so the regions of a large
graph stay readable; a title starts at the canvas's edge when its box starts beyond it.

**Edits made elsewhere.** An agent's or another window's edit lands in an open window at once, and its changed nodes
flash. A value you are dragging or typing in Details or My Script survives an edit of other elements; when the edit
changed the same element, your value is dropped with "Your edit of Duration was replaced by a change made elsewhere". A
gesture on the canvas that such an edit interrupts ends with "Your move was interrupted by a change made elsewhere".

**The Edit menu's keys.** Cut, Copy, Paste, Duplicate, Delete and Select All, in the Edit menu and as keys (Ctrl+X,
Ctrl+C, Ctrl+V, Ctrl+D, Delete or Ctrl+Backspace, Ctrl+A), act on the graph window focused last whose tab is in front,
wherever the keyboard is (the console, for one), and never on the entities then; with the scene as the target they act
on the entities. A window keeps the keys it claims while it has the keyboard: the content browser's Delete,
Ctrl+Backspace and F2 act on its selected file, and the Edit menu then shows no shortcut for Delete. While a dialog is
open (a delete confirmation, the unsaved-changes prompt), no editing key runs, Undo and Redo included, so nothing
changes what the dialog acts on.

## Testing a game

`Strada.Testing` turns a scene into a test: run checks with `TestReporter.Run`, using `Assert` inside them, and call
`TestReporter.Finish()` when every check has reported.

```csharp
using Strada;
using Strada.Testing;

namespace Game;

public class GameTests : Script
{
	public Entity? Player;

	protected override void OnCreate()
	{
		TestReporter.Run("the player starts on the ground", () =>
		{
			Assert.IsNotNull(Player);
			Assert.AreApproximatelyEqual(0.0f, Player.Translation.Y, 0.01f);
		});
		TestReporter.Finish();
		Application.Quit();
	}
}
```

Put it in a scene of its own and run that scene: in the editor with Play, through an agent with the `test_run` tool,
or headless with the player: `StradaRuntime --project Game.sproj --scene Scenes/Tests.sscene --test` exits with a
non-zero code when a check failed, a script threw or the run did not finish, which suits continuous integration.
Checks can also let frames pass in `OnUpdate` first, as `Projects/Blocks/Scripts/Source/BlocksTests.cs` does. Graphs
report checks with `TestReporter` nodes (`Call:Strada.Testing.TestReporter.Pass(string)`, `Fail(string,string)`), and a
C# test can read what graphs counted (`GetVariable<T>`), as `Projects/FeatureTest/Scripts/Source/VisualScriptTests.cs`
does.

## Exporting

File > Build Game builds the scripts in Release and ships the assembly with the game; a game with visual scripts ships
the scripting API they run on, also without C# scripts. Players of games with C# scripts or visual scripts need the .NET
10 runtime, installed or shipped by you in a `dotnet` folder next to the executable.

## Examples

- `Projects/Blocks`: a complete falling-blocks game (board logic, input, game states with title, pause and game-over
  screens, an AI that plays the title screen's demo, hold and a next queue, a best score saved with `System.IO`, blocks
  created by script, rows that burst into physics debris and particle sparks from prefabs, callouts that pop over the
  board, a camera shake with a spring kick, pools of sound sources, a HUD and menus on a UI canvas that a script brings
  to life: buttons for the mouse and the keys, screens that fade in, labels that follow 3D panels; tests that click the
  buttons).
- `Projects/Showcase`: a camera tour on a smooth path (`Flythrough`, its stops are `CameraShot` scripts with inspector
  fields) that cues exhibits through an interface (`ITourCue`, which also tells them when the tour ends or is skipped:
  its night for a while, `NightInterlude`, ends then, unless the visitor switched day and night meanwhile), fades UI
  elements and letterbox bars, plays the music up through its audio source's volume and plays again with the exhibits
  put back, a camera rig that flies over the clearing and throws balls with a camera shake under it or walks as a
  first-person character (`Walker`: speeding up and slowing down, a jump buffer and coyote time, footsteps, a head bob
  and a dip on landing, kept within the clearing's ground; Esc frees the cursor for the HUD), a lift that characters
  ride and that stops on its way down while rays from its underside find a character or a loose body below (`Lift`, a
  kinematic platform), trigger scripts (a goal that counts, a pressure plate that dynamic bodies and characters press
  to raise a kinematic gate), ballistics for a ball launcher, dominoes and a wrecking ball set off by keys, a light show
  and a neon sign that flickers on through cloned materials, day and night from lights, a post-process volume's weight
  and the intensity of unlit particles (also of smoke spawned later), a HUD whose buttons act in `OnClick` and whose
  rows open as the camera nears their exhibits, `Debug.DrawLine` previews of throws and trigger bounds, and tests that
  play every exhibit, click the HUD's buttons and walk the yard (run `Tools/FetchAssets.py` first for its models and
  sky).
- `Projects/FeatureTest`: scenes whose scripts use every component and the whole API, and graphs that use every kind of
  built-in node and every `[VisualNode]` member of its scripts (`Assets/Scripts/Features.sgraph`, which calls the function
  library `MathLibrary.sgraph`, sends to and reads the variables of `Contacts.sgraph`, and `Clicks.sgraph`), checked as a
  test run. The engine's tests also run every node of the node library on its own (`VisualScriptNodeCoverageTests`).
