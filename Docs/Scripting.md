# Scripting guide

Games get their behavior from C# scripts: classes deriving from `Strada.Script`, attached to entities by Script
components. This guide shows how to write them. §10 of [Architecture.md](Architecture.md) is the reference of the
scripting runtime, and every public member of the API has XML documentation that C# editors show as you type.

## A first script

A project's scripts form one C# project, `Scripts/<Name>.csproj`, where `<Name>` is the project's script module
(`Game` unless the project settings say otherwise); it is also the scripts' namespace. In the editor, Scripts > New
Script asks for a class name and writes `Scripts/Source/<Class>.cs` from a template (agents use the `script_create`
tool). Open the project folder in any C# editor (Visual Studio, Rider, VS Code with the C# extension): the C# project
references this engine's `Strada.ScriptCore.dll`, so completion and documentation work.

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
a failed build. Select an entity, add a Script component in the inspector and pick `Game.Spinner` as its class: `Speed`
appears below it. Press Play (Ctrl+P) and the entity turns. A build that finishes while the game plays is loaded when
play mode stops.

## How a script runs

Override the methods you need:

| Method | Called |
|--------|--------|
| `OnCreate()` | Once, when the scene starts or the entity is created while it runs, after every script of the scene exists |
| `OnUpdate(float deltaTime)` | Every frame, with the seconds since the previous frame |
| `OnFixedUpdate(float fixedDeltaTime)` | Before every physics step, at the scene's fixed rate |
| `OnCollisionEnter(Entity other)`, `OnCollisionExit(Entity other)` | When a collider of the entity starts or stops touching one of `other` |
| `OnTriggerEnter(Entity other)`, `OnTriggerExit(Entity other)` | When a trigger collider of either entity starts or stops overlapping the other |
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

In the editor the game gets the input while the Game View has the focus during play (Shift+Esc gives it back to the
editor; a click on the view returns it to the game). `Application.WindowWidth` and `WindowHeight` are the size the game
view renders at, which its resolution menu chooses.

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

Players and walking creatures use a `CharacterControllerComponent` (Entity > Physics > Character makes one with a
capsule mesh) instead of a rigid body and colliders: an upright capsule (`Radius`, `Height`, its center at `Offset` from
the entity) that your script moves. `Move(velocity)` walks it at a velocity during the next physics step, so call it
from `OnFixedUpdate` every step it should move; it slides along walls, climbs steps up to `StepHeight` and slopes up to
`MaxSlopeAngle`, slides down steeper ones, rides moving platforms and elevators, and pushes dynamic bodies with up to
`PushForce` newtons. Gravity acts on top of it: `IsGrounded` tells whether it stands on walkable ground (`GroundNormal`
is the ground's normal), and `Velocity` is its own velocity (falling, jumping), which you set to jump.
`Teleport(position)` places it and stops it. The entity follows the capsule, which does not turn with it, so the script
turns the entity freely. Triggers notice characters (`OnTriggerEnter` on both sides), and raycasts hit them. Colliders
of the entities below a character are its own parts (a held object, a trigger around it for its reach): they neither
block it nor report touching it.

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
`Clip` can be set); sources are positioned in 3D relative to the entity with the `AudioListenerComponent`.
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
Checks can also let frames pass in `OnUpdate` first, as `Projects/Blocks/Scripts/Source/BlocksTests.cs` does.

## Exporting

File > Build Game builds the scripts in Release and ships the assembly with the game. Players need the .NET 10 runtime,
installed or shipped by you in a `dotnet` folder next to the executable.

## Examples

- `Projects/Blocks`: a complete falling-blocks game (board logic, input, game states with title, pause and game-over
  screens, hold and a next queue, a best score saved with `System.IO`, blocks created by script, rows that burst into
  physics debris and particle sparks from prefabs, a camera shake, a pool of sound sources, a HUD of screen texts built
  in code, tests).
- `Projects/Showcase`: a free camera that throws physics balls, impact sounds scaled by speed, flickering lights, a
  wrecking ball on a joint that a key pushes, and a playground that counts what was knocked over (run
  `Tools/FetchAssets.py` first for its models and sky).
- `Projects/FeatureTest`: scenes whose scripts use every component and the whole API, checked as a test run.
