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
or the scene view runs it there. A graph is stored as a document of nodes and links, which agents also write through
the editor's automation commands (MCP tools, or `strada call` from a shell): `graph.library` lists the kinds of nodes
with their pins, `graph.types` the types, `graph.check` checks a document and `graph.set` saves it, and `graph.open`
shows it in its window. [Automation.md](Automation.md) describes the document and the commands. This graph turns its
entity like the `Spinner` script at the top of this guide:

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
back.
Outputs split the same way (a raycast's hit point, then its `Y`).

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
Delete, F2, Ctrl+X or Ctrl+D act on what you found. Ctrl+Shift+F searches every script of the project. Find References on a
variable, Find Callers on a function and Find Senders on a custom event (in their right-click menus in My Script) list
their uses; senders are found in every script.

**Editing while playing.** Keep the graph window open while the game plays. Graph windows open as tabs beside the
Scene View and the Game View, and Play brings the Game View to the front (Stop the Scene View): to watch the game while you edit, drag the
graph window's tab to the side of the editor (or onto another panel's edge) so both show. Every edit reaches the running
game at once (the toolbar's LIVE pill says so), variables keep their values, and waiting nodes keep waiting while they still
exist. When an edit leaves an error, the pill turns amber ("last working version") and the game goes on with the last
version that worked until you fix it. The toolbar's Instance list picks which entity running the script the window
shows: My Script then shows its public variables' current values, and dropped entities set its values. Edits made
while playing stay in the script after Stop, and stay undoable.

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
  built-in node (`Assets/Scripts/Features.sgraph`, `Contacts.sgraph` and `Clicks.sgraph`), checked as a test run.
