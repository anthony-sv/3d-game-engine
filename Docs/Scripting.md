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
		Transform.EulerAngles += new Vector3(0.0f, Speed * deltaTime, 0.0f);
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
`Children` are right there. Components are views of the entity's data: `GetComponent<T>()` returns null when the entity
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

In the editor the game gets the input while the viewport has the focus during play.

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

Forces and torques take a `ForceMode` (`Force`, `Impulse`, `Acceleration`, `VelocityChange`). `LinearVelocity` and
`AngularVelocity` can be read and set, `MoveKinematic` moves kinematic bodies smoothly and `Teleport` places any body.
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

- `Projects/Blocks`: a complete falling-blocks game (board logic, input, spawning cubes, score text, sound, tests).
- `Projects/FeatureTest`: scenes whose scripts use every component and the whole API, checked as a test run.
