# Scripting API reference

<!-- Generated from Strada.ScriptCore by ScriptCoreTests (Tests/ScriptCoreTests/Source/ApiReferenceTests.cs):
     edit the API's XML documentation in Strada-ScriptCore, then run ScriptCoreTests with
     STRADA_UPDATE_DOCS=1. -->

Every public type and member of `Strada.ScriptCore`, the API that C# scripts and visual scripts use, with the
summary C# editors show as you type. The [scripting guide](Scripting.md) shows the API in use and the
[visual scripting guide](VisualScripting.md) how graphs call it: every member listed here is a node, but for
the few whose summary says graphs have none.
The engine's tests generate this page from the API, so it lists exactly what the API has.

108 types in 2 namespaces, with 912 members and enum values in all.

## Strada

| Type | Kind | Summary |
|---|---|---|
| [Application](#application) | static class | The application running the game: the exported game's player, or the editor's play mode. |
| [Asset](#asset) | abstract class | Base class of references to the project's assets. References compare equal when their handles do. |
| [AssetHandle](#assethandle) | struct | Identifies an asset of the project; the default value refers to no asset. |
| [Assets](#assets) | static class | Access to the project's assets. |
| [Audio](#audio) | static class | The game's side of the mix: the master volume, the game's volume and mute of each bus of the project's mixer, blends to the mixer's snapshots, each bus's level as a meter reads it, sounds played at a point, and how many voices play. The values set here are the game's, a gain stage after the mixer's own (the `Mixer` asset's), so that a settings menu and the mix never overwrite each other; they are never written to the asset, and every game starts with them at their defaults (a master volume of 1, every bus at 0 dB and not muted, the mixer's own values without a snapshot). Bus and snapshot names match ignoring letter case; the empty bus name is Master's. Without sound (no audio engine) reads give the defaults and writes do nothing. |
| [AudioClip](#audioclip) | class | An audio clip asset (WAV, FLAC, MP3 or Ogg Vorbis). |
| [AudioListenerComponent](#audiolistenercomponent) | class | The ears of the scene: the first active listener hears it. |
| [AudioSourceComponent](#audiosourcecomponent) | class | Plays an audio clip, optionally positioned in 3D. |
| [BoxColliderComponent](#boxcollidercomponent) | class | A box collision shape of the entity's body. |
| [CameraComponent](#cameracomponent) | class | Renders the scene from the entity's position, looking down its -Z axis. |
| [CapsuleColliderComponent](#capsulecollidercomponent) | class | A capsule collision shape along the entity's local Y axis. |
| [CharacterControllerComponent](#charactercontrollercomponent) | class | Makes the entity a character that scripts move, such as the player: an upright capsule that walks at the velocity given to `CharacterControllerComponent.Move`, slides along walls, climbs steps up to `CharacterControllerComponent.StepHeight` and slopes up to `CharacterControllerComponent.MaxSlopeAngle`, slides down steeper slopes, falls with gravity, rides what it stands on (moving platforms, elevators) and pushes dynamic bodies. The entity's transform follows the capsule, which does not turn with the entity: in `Script.OnFixedUpdate` it is where the last physics step left the character, and between steps it is drawn on its way there (a step behind), so it moves smoothly at any frame rate. Raycasts hit the character, and triggers and bodies report it in their contact callbacks like a kinematic body. The colliders of the entities below it are its own parts: they neither block it nor report touching it. |
| [Collision](#collision) | static class | The contact event being handled: readable inside a script's `Script.OnCollisionEnter`, `Script.OnCollisionExit`, `Script.OnTriggerEnter` and `Script.OnTriggerExit`, and while a graph's contact event runs, until its first wait (a flow the visual script debugger stopped continues with the contact it read). Physics reports contacts once per frame, after the frame's physics steps, so `Collision.Step` says which step a contact happened in. Outside a handler `Collision.Entity` and `Collision.Other` are null, `Collision.Step` is 0 and `Collision.Points` is empty. |
| [Color](#color) | struct | A linear RGBA color with single-precision components (1 is full intensity; lights may exceed it). |
| [Component](#component) | abstract class | Base class of the engine components scripts can access through `Entity.GetComponent`. Component objects are views: they read and write the entity's data directly. |
| [ContactPoint](#contactpoint) | struct | Where and how two colliders touch as their contact begins, seen from one of them: the entity whose handler reads it (`Collision.Entity`), against `Collision.Other`. |
| [CursorMode](#cursormode) | enum | How the mouse cursor behaves. |
| [Curve](#curve) | class | A curve asset (`.scurve`): keys with values, how each segment interpolates (smooth, linear or a step) and their slopes, and what the curve does before its first key and after its last (hold, loop, ping-pong or go on along a line). Use one to map a number to another, such as a speed to an engine's pitch or a time to a light's intensity. Edit it in the editor's curve editor; scripts read it. |
| [Debug](#debug) | static class | Drawing for debugging, in the editor and development builds (shipped Dist games draw nothing). |
| [DirectionalLightComponent](#directionallightcomponent) | class | Light from infinitely far away, shining along the entity's -Z axis. |
| [Entity](#entity) | class | An object of the running scene, identified by its ID. Entities compare equal when their IDs do. |
| [EnvironmentMap](#environmentmap) | class | An HDR environment map: the sky and image-based lighting of a sky light. |
| [Font](#font) | class | A font asset for text. |
| [ForceMode](#forcemode) | enum | How `RigidBodyComponent.AddForce` and `RigidBodyComponent.AddTorque` interpret their value. |
| [GamepadAxis](#gamepadaxis) | enum | Gamepad axes in the standard layout: sticks from -1 to 1 (up is -1), triggers from -1 (released) to 1. |
| [GamepadButton](#gamepadbutton) | enum | Gamepad buttons in the standard (SDL_GameControllerDB) layout. |
| [HideInInspectorAttribute](#hideininspectorattribute) | attribute | Keeps a serialized field out of the inspector; its value is still saved with the scene. |
| [Input](#input) | static class | Keyboard, mouse and gamepad state of the current frame, and the project's input actions (named values such as Jump or Steer that bindings give from keys and gamepads alike). "Pressed" and "released" are true only during the frame the button changed. |
| [InputActions](#inputactions) | class | An input actions asset (`.sinput`): the named actions a game reads instead of keys and gamepads (Throttle, Steer, Jump), each with its bindings. The project names the one its scenes evaluate (Project Settings, Input Actions); scripts read its actions by name through `Input.GetAction`, `Input.IsActionPressed` and the other action queries, and `Script.OnActionPressed` and `Script.OnActionReleased` report their edges. Edit it in the editor; an edit made while the game plays shows on the next frame. An asset that cannot be loaded has no actions (logged). |
| [JointComponent](#jointcomponent) | class | Joins this entity's body to another entity's body, or to the world, while the scene runs: welded (`JointType.Fixed`), turning around an axis (`JointType.Hinge`, with limits and a motor), turning freely (`JointType.Ball`), kept within distances (`JointType.Distance`) or sliding along an axis (`JointType.Slider`). Both entities need a rigid body with a collider. Changing a property makes the joint again from where the bodies are. |
| [JointType](#jointtype) | enum | How a `JointComponent` joins two bodies. |
| [KeyCode](#keycode) | enum | Keyboard keys by their position on a US keyboard layout (GLFW's key codes). |
| [Log](#log) | static class | Writes messages to the engine's script log: the editor console, the terminal and the log file. |
| [Material](#material) | class | A material asset: the metallic-roughness surface of meshes (colors are linear). The project's materials are shared by every scene, so scripts change copies made with `Material.Create` or `Material.Clone`; those last until the scene stops. Changing a shared material logs a script error and does nothing. |
| [MaterialAlphaMode](#materialalphamode) | enum | How a material uses the alpha of its base color. |
| [Mathf](#mathf) | static class | Single-precision math helpers for games. |
| [Matrix4](#matrix4) | struct | A 4x4 matrix of single-precision values stored as four columns, like the engine's matrices. Matrices transform column vectors: `matrix * vector`, and `a * b` applies `b` first. |
| [Mesh](#mesh) | class | A mesh asset (imported model or built-in shape). |
| [MeshColliderComponent](#meshcollidercomponent) | class | A collision shape from a mesh: its convex hull, or its triangles (static and kinematic bodies). |
| [MeshComponent](#meshcomponent) | class | Renders a mesh asset with its materials. |
| [Mixer](#mixer) | class | A mixer asset (`.smixer`): the buses sounds play on, in a tree under Master, each with its volume, its mute and its effects. The project names the mixer its games play through; a sound source names its bus (`AudioSourceComponent.Bus`). Edit it in the editor; scripts read its buses, for example to list the volumes a settings menu offers, and set the game's own values of a bus through `Audio`, which never change the asset. Bus names match ignoring letter case, and the empty name is Master's. An edit made while the game plays shows on the next frame. A mixer that cannot be loaded reads as a new one, Master alone (logged). |
| [MouseButton](#mousebutton) | enum | Mouse buttons. |
| [ParticleBlendMode](#particleblendmode) | enum | How particles combine with what is behind them. |
| [ParticleEmitterShape](#particleemittershape) | enum | Where a particle system's particles start. |
| [ParticleSystemComponent](#particlesystemcomponent) | class | Emits camera-facing particles (fire, sparks, smoke, dust, magic) while the scene runs: each lives between `ParticleSystemComponent.LifetimeMin` and `ParticleSystemComponent.LifetimeMax` seconds, starts from the `ParticleSystemComponent.Shape` with a speed between `ParticleSystemComponent.SpeedMin` and `ParticleSystemComponent.SpeedMax`, falls with the scene's gravity times `ParticleSystemComponent.GravityFactor`, slows down with `ParticleSystemComponent.Drag`, and blends from the start to the end color and size. |
| [Physics](#physics) | static class | Queries and settings of the running scene's physics. Queries read the world as its last physics step left it: in `Script.OnFixedUpdate` that is where bodies are; in `Script.OnUpdate` a body drawn between its last two steps is found up to a step ahead of where it is drawn, and an entity moved since the last step (by a script, or with its parent) is found where it was until the next step takes its body there. A body made or changed since the last step is found where its entity is. A query sees the bodies and characters on the layers of its layer mask (bit n is layer n), each entity at most once, and trigger colliders only when it includes them. Directions need not be unit length. Values that are not finite, sizes below 0, radii above 100 meters and half extents or half heights above 20 meters are script errors and find nothing (longer shapes lose the physics engine's precision); a zero direction or a distance that is not positive finds nothing. Queries allocate no managed memory once the entities they find have been met: the `Entity` of an entity without a script is made when a query first finds it, then kept. |
| [PhysicsMaterial](#physicsmaterial) | class | A physics material asset (`.sphysmat`): how a surface slides, bounces and rolls, the name scripts tell it by, and what impacts on it sound and look like. A collider whose Material names one takes its friction, restitution and combine modes in place of its own; a raycast hit names the material of the surface it hit (`RaycastHit.Material`). Edit it in the editor; scripts read it, and an edit made while the game plays shows on the next frame. A material that cannot be loaded reads as a new one's values (logged). |
| [PhysicsMaterialCombine](#physicsmaterialcombine) | enum | How the two sides of a contact combine a value of their physics materials (friction or restitution). When the two sides name different modes, the one later in this list wins. |
| [PointLightComponent](#pointlightcomponent) | class | Light shining from the entity's position in every direction. |
| [PostProcessComponent](#postprocesscomponent) | class | A post-process volume: changes the scene's post-processing (exposure, tonemapper, bloom, ambient occlusion, color grading and lens effects) where the camera is. Each setting applies only while its `Override` property is set. `PostProcessComponent.Global` volumes apply everywhere; local volumes apply inside their box (the unit cube placed, rotated and scaled by the entity, so the entity's scale is the box's size in meters) and fade out over `PostProcessComponent.BlendDistance` around it. Volumes apply in order of `PostProcessComponent.Priority`, each scaled by its `PostProcessComponent.Weight`: numbers move towards the volume's values, `PostProcessComponent.Bloom` fades in or out through the bloom's intensity, and `PostProcessComponent.AmbientOcclusion` and `PostProcessComponent.Tonemapper` take the volume's value once its blend reaches one half. Animate `PostProcessComponent.Weight` for effects such as a damage flash or an underwater tint. |
| [Prefab](#prefab) | class | A prefab asset: entities saved for instantiation (see `Entity.Instantiate`). |
| [ProjectionType](#projectiontype) | enum | How a camera projects the scene. |
| [QualityPreset](#qualitypreset) | enum | The graphics quality of the game's views: presets made of the scene's own settings, each only lowering them. |
| [Quaternion](#quaternion) | struct | A rotation stored as a unit quaternion (X, Y, Z, W), laid out like the engine's quaternions. |
| [Random](#random) | static class | Random numbers for games. Seeding makes the sequence repeat (main thread use). |
| [RangeAttribute](#rangeattribute) | attribute | Limits the value the inspector lets a numeric field take. |
| [Ray](#ray) | struct | A half-line from an origin along a direction. |
| [RaycastHit](#raycasthit) | struct | What a raycast hit. |
| [RebindResult](#rebindresult) | class | The answer of a rebind of an input action's binding: how it ended, the binding's controls and the other actions that now read one of them. The engine applies a binding whatever it conflicts with and reports the conflicts; the game decides what to do about them (warn, swap, or undo with another `Input.Rebind`). |
| [RebindStatus](#rebindstatus) | enum | How a rebind of an input action's binding ended (`Input.Rebind`, `Input.ResetBindings`), or that a capture listens for a control (`Input.StartRebind`). |
| [ReverbZoneComponent](#reverbzonecomponent) | class | A reverb zone: a sphere or a box round the entity inside which spatial sounds send part of themselves to a bus of the project's mixer that carries a reverb (an echo under a bridge, a tunnel, a hall). It follows the sound, not the listener: a sound inside the zone echoes for a listener outside it. The shape is placed and turned by the entity and sized in meters times its scale (a sphere by its largest axis). A sound sends `ReverbZoneComponent.SendLevel` of itself inside the shape, less and less across `ReverbZoneComponent.FadeDistance` outside it; of several zones of one bus the largest send counts, and a sound sends to the two buses it sends most to. Sounds that are not spatial send nothing. A change applies from the next frame. |
| [ReverbZoneShape](#reverbzoneshape) | enum | The shape of a reverb zone. |
| [RigidBodyComponent](#rigidbodycomponent) | class | Makes the entity a physics body; its colliders give it shape. While the scene runs, velocities, forces and sleep act on the simulated body. |
| [RigidBodyInterpolation](#rigidbodyinterpolation) | enum | How `RigidBodyComponent.Interpolation` draws a moving body between physics steps. |
| [RigidBodyType](#rigidbodytype) | enum | How a rigid body moves. |
| [SaveSlot](#saveslot) | class | A save slot: named values the game keeps for its player between runs (a best lap, its splits, progress), in a file of the game's own data folder that this class never names. Open a slot by its name, read values with `SaveSlot.Get`, change them with `SaveSlot.Set` and `SaveSlot.Remove`, and write them with `SaveSlot.Save`, which returns once the slot is on disk: the frame that saves waits for the device, so save at a results screen, a menu or between races, never while the player plays, and keep what a ride learns in memory until then. A slot holds numbers, text, vectors, quaternions, colors, entities, prefabs, assets, enums (as their integers) and arrays or lists of any of them of up to 4,096 elements, at most 4,096 values, and is written whole, at most 16 MiB. The object holds the values: reading and changing them touches no file, and two objects opened with one name are separate copies, the last to save winning. Main thread. |
| [SaveSlotStatus](#saveslotstatus) | enum | What a save slot read as when it was opened, or what its last `SaveSlot.Save` or `SaveSlot.Delete` left it. |
| [SceneManager](#scenemanager) | static class | The scenes of the game. |
| [Script](#script) | abstract class | Base class of entity behaviours. A Script component names a class deriving from Script; when the scene runs, the engine creates one instance per such entity, sets its serialized fields (public fields and fields marked `SerializeFieldAttribute`) and calls the overridden methods. Exceptions thrown by them are logged and do not stop the scene. |
| [ScriptComponent](#scriptcomponent) | class | Runs a script class on the entity. |
| [SerializeFieldAttribute](#serializefieldattribute) | attribute | Exposes a private or protected field of a script to the editor and to scene files, like a public field. |
| [Settings](#settings) | static class | The player's settings of the game: the window, the graphics quality, the master volume, each bus's volume and mute, and named values the game owns (its assists, its options), kept for the player in the game's own data folder, which this class never names. The game starts with them as the player last saved them (or the game's own window settings and High quality); every setter applies at once (the window once the frame's update has returned; the player's command line wins over the window, and the editor's play mode keeps the window without applying it) and reaches the file only at `Settings.Save`. The volumes are the player's, a stage of the mix after `Audio`'s, so a settings screen and the game's own mixing never overwrite each other. Main thread. |
| [ShapeCastHit](#shapecasthit) | struct | What a sphere, box or capsule cast touched first (`Physics.SphereCast`, `Physics.BoxCast`, `Physics.CapsuleCast`). |
| [SkyLightComponent](#skylightcomponent) | class | The sky and image-based lighting from an HDR environment map. |
| [SphereColliderComponent](#spherecollidercomponent) | class | A sphere collision shape of the entity's body. |
| [SpotLightComponent](#spotlightcomponent) | class | Light shining from the entity's position in a cone along its -Z axis. |
| [SpriteRendererComponent](#spriterenderercomponent) | class | Renders an unlit textured or colored quad, in the world or on the screen. |
| [Telemetry](#telemetry) | static class | The game's telemetry: named channels of one number per fixed step, and marks on steps, which the editor records while its Telemetry panel is open or an agent's recording runs (the telemetry tools) and plots, reads and exports; the game player records nothing. A channel's name and a mark's label are 1 to 64 bytes of UTF-8 without control characters; a longer one is refused, never cut. A value recorded during a fixed step (`Script.OnFixedUpdate`) is that step's, and the last of several wins; one recorded between steps (`Script.OnUpdate`, an event) is the next step's; a step that records nothing for a channel has no value there. Laps run from a mark `Lap n` to the next mark `Lap n+1` on a later step; `Lap end` ends a lap in progress without making one. What the recorder refuses (a name too long, a value that is not finite, too many channels) is dropped, counted and warned about once per recording session. Any thread may call these, and while nothing records each returns after one native call. |
| [TextAlignment](#textalignment) | enum | How text lines align to the entity's position. |
| [TextComponent](#textcomponent) | class | Renders text in the world (in the entity's XY plane) or on the screen. |
| [TextVerticalAlignment](#textverticalalignment) | enum | Where a `UITextComponent`'s text block sits in its rectangle. |
| [Texture](#texture) | class | A texture asset. |
| [Time](#time) | static class | Timing of the running scene. All times are scaled by `Time.TimeScale`. |
| [TimelineDirection](#timelinedirection) | enum | The direction a visual script's Timeline node plays in: its Direction output. |
| [TonemapOperator](#tonemapoperator) | enum | How the renderer maps the scene's HDR colors to the display (a `PostProcessComponent` setting). |
| [TooltipAttribute](#tooltipattribute) | attribute | Explains a field in the inspector. |
| [TransformComponent](#transformcomponent) | class | Position, rotation and scale of an entity relative to its parent (or the world at the root). |
| [UIButtonComponent](#uibuttoncomponent) | class | Makes a UI element a button: while the scene runs the mouse hovers, presses and clicks it (the left button pressed and released over it). A click sets `UIButtonComponent.WasClicked` for the frame and calls `OnClick` on the scripts of the button and of its ancestors. The button's colors replace its `UIImageComponent`'s color in each state. |
| [UIButtonState](#uibuttonstate) | enum | The state a `UIButtonComponent` is drawn in. |
| [UICanvasComponent](#uicanvascomponent) | class | The root of a screen-space UI (HUDs, menus). The canvas covers the screen; its descendants are the UI, laid out by their `UIRectComponent` and drawn over the scene in hierarchy order (children over their parents, later siblings over earlier ones). |
| [UIImageComponent](#uiimagecomponent) | class | Fills a UI element's rectangle with a color or a texture, optionally with rounded corners. Images take the clicks over the UI they cover. |
| [UIRectComponent](#uirectcomponent) | class | Places a UI element in its parent's rectangle (the screen for a canvas's children), in canvas units with x to the right and y downwards. On each axis the anchors mark a span of the parent (0 is its left or top edge, 1 its right or bottom edge): the rectangle is `UIRectComponent.Size` larger than the span, so equal anchors give it exactly that size, and its `UIRectComponent.Pivot` sits at the pivot's point of the span, moved by `UIRectComponent.Position`. |
| [UIScaleMode](#uiscalemode) | enum | How a `UICanvasComponent`'s units relate to the screen's pixels. |
| [UITextComponent](#uitextcomponent) | class | Draws text in a UI element's rectangle: lines wrap at its width and the text block is aligned in it. |
| [Vector2](#vector2) | struct | A 2D vector of single-precision components, laid out like the engine's vectors. |
| [Vector3](#vector3) | struct | A 3D vector of single-precision components, laid out like the engine's vectors. |
| [Vector4](#vector4) | struct | A 4D vector of single-precision components, laid out like the engine's vectors. |
| [Vehicle](#vehicle) | class | A vehicle definition asset (`.svehicle`): a motorcycle's two wheels (the front steers, the engine drives the rear), its engine, its gearbox and final drive, and how it leans. A `VehicleComponent` names the definition its entity rides with. Edit it in the editor; scripts read it, for example the engine's speeds a rev counter shows or the gears a gear indicator counts, and an edit made while the game plays reaches the vehicles and the scripts on the next frame. A definition that cannot be loaded reads as a new one's values (logged). |
| [VehicleComponent](#vehiclecomponent) | class | Makes the entity a motorcycle of its `VehicleComponent.Definition`: the entity needs a dynamic `RigidBodyComponent` and a collider (its chassis), and the entities below it that draw its wheels a VehicleWheel component each. While the scene simulates it, a rider drives it with `VehicleComponent.SetDriverInput` (the throttle, the front and the rear brake, and the lean) and, in a Manual gearbox, `VehicleComponent.ShiftUp` and `VehicleComponent.ShiftDown`; its engine, gearbox, lean and wheels read as its last physics step left them. Set the inputs in `Script.OnFixedUpdate` (or a graph's Event.FixedUpdate): each physics step applies the latest given before it, so a ride set there as a function of the simulation is the same at every frame rate. Without a simulated vehicle (the scene is not playing, the entity has no dynamic body or no definition that loads) the reads are a vehicle at rest in neutral, and the inputs and shifts are logged and do nothing. |
| [VehicleTransmissionMode](#vehicletransmissionmode) | enum | How a vehicle's gearbox changes gear. |
| [VehicleWheelState](#vehiclewheelstate) | struct | A wheel of a vehicle as its last physics step left it (`VehicleComponent.GetWheelState`), a struct of public fields that graphs Make and Break. |
| [VisualNodeAttribute](#visualnodeattribute) | attribute | Makes a member of the game's scripts a node of visual scripts, or every public member of a type: graphs call your code as they call the scripting API. |
| [VisualScript](#visualscript) | class | A visual script asset (`.sgraph`): a graph of nodes that a `VisualScriptComponent` runs on its entity. |
| [VisualScriptComponent](#visualscriptcomponent) | class | Runs a visual script (a graph asset) on the entity, beside or instead of a C# script. While the scene runs, C# scripts raise the graph's custom events and read and write its variables through this component. |
| [WindowMode](#windowmode) | enum | How the game's window is shown. |

### Application

```csharp
public static class Application
```

The application running the game: the exported game's player, or the editor's play mode.

| Member | Summary |
|---|---|
| `static bool IsEditor { get; }` | Whether the scene runs in the editor's play mode (false in exported games). |
| `static int WindowWidth { get; }` | The width in pixels of what the scene renders to: the game's window, or the editor's viewport. |
| `static int WindowHeight { get; }` | The height in pixels of what the scene renders to: the game's window, or the editor's viewport. |
| `static void Quit()` | Ends the game after this frame (the editor leaves play mode). |

### Asset

```csharp
public abstract class Asset : IEquatable<Asset>
```

Base class of references to the project's assets. References compare equal when their handles do.

| Member | Summary |
|---|---|
| `AssetHandle Handle { get; }` | The asset's handle. |
| `static bool operator ==(Asset? a, Asset? b)` | Whether both refer to the same asset (or both are null). |
| `static bool operator !=(Asset? a, Asset? b)` | Whether they refer to different assets. |
| `bool Equals(Asset? other)` | Whether `other` refers to the same asset (of the same type). |

### AssetHandle

```csharp
public struct AssetHandle : IEquatable<AssetHandle>
```

Identifies an asset of the project; the default value refers to no asset.

| Member | Summary |
|---|---|
| `AssetHandle(ulong id)` | Wraps an asset identifier. |
| `ulong ID { get; }` | The asset's identifier (0 for no asset). |
| `bool IsValid { get; }` | Whether the handle refers to an asset (the asset itself may still be missing). |
| `static AssetHandle Invalid { get; }` | The handle that refers to no asset. |
| `static bool operator ==(AssetHandle a, AssetHandle b)` | Identifier equality. |
| `static bool operator !=(AssetHandle a, AssetHandle b)` | Identifier inequality. |
| `bool Equals(AssetHandle other)` | Identifier equality. |

### Assets

```csharp
public static class Assets
```

Access to the project's assets.

| Member | Summary |
|---|---|
| `static T? Load<T>(string path)` | Loads an asset by its path in the project's asset directory ("Prefabs/Enemy.sprefab") or by reference ("builtin://Cube"). Returns null (logged) when there is no such asset of type `T` or it cannot be loaded. |

### Audio

```csharp
public static class Audio
```

The game's side of the mix: the master volume, the game's volume and mute of each bus of the project's mixer, blends to the mixer's snapshots, each bus's level as a meter reads it, sounds played at a point, and how many voices play. The values set here are the game's, a gain stage after the mixer's own (the `Mixer` asset's), so that a settings menu and the mix never overwrite each other; they are never written to the asset, and every game starts with them at their defaults (a master volume of 1, every bus at 0 dB and not muted, the mixer's own values without a snapshot). Bus and snapshot names match ignoring letter case; the empty bus name is Master's. Without sound (no audio engine) reads give the defaults and writes do nothing.

| Member | Summary |
|---|---|
| `static float MasterVolume { get; set; }` | The gain applied to the whole mix, after Master's effects and volume: 1 leaves it unchanged, 0 is silence (values below 0 are 0), above 1 louder. It moves without a click. |
| `static int ActiveVoiceCount { get; }` | The voices mixed now across every running scene: those playing, in scenes that are not paused, within the engine's limit of voices mixed at once (beyond it, the lowest priority, then the quietest, then the oldest go silent). It counts a voice from the call that starts it, and a voice that ended from the next frame. |
| `static string CurrentSnapshot { get; }` | The snapshot of the project's mixer the mix blends to or holds, as the mixer names it; empty for the mixer's own values, which every game starts and ends with (and without sound). |
| `static float GetBusVolume(string bus)` | The game's volume of a bus, in decibels (0 leaves the bus as the mixer sets it); 0 for a bus the project's mixer lacks (logged once). |
| `static void SetBusVolume(string bus, float decibels)` | Sets the game's volume of a bus, in decibels: added to the mixer's own volume of the bus, from -80 (silence) to +24 (values beyond are brought within). It moves without a click. A bus the project's mixer lacks is logged once, and nothing changes. |
| `static bool IsBusMuted(string bus)` | Whether the game has muted a bus (the mixer's own mute is the `Mixer` asset's); false for a bus the project's mixer lacks (logged once). |
| `static void SetBusMuted(string bus, bool muted)` | Mutes or unmutes a bus for the game: a muted bus, and every bus below it, is silent whatever the mixer sets. It moves without a click. A bus the project's mixer lacks is logged once, and nothing changes. |
| `static bool PlayClipAtPoint(AudioClip clip, Vector3 position, float volume = 1, string bus = "")` | Plays a clip once at a point of the world, on a voice of the running scene rather than of an entity: it outlives whatever played it (a break, a pickup taken), is held while the scene is paused and ends with the scene. It is spatial, full within 1 m and falling with distance to 100 m, never loops, and at the engine's limit of voices it may be the one refused. Returns whether it started; false without a running scene with sound, or for a clip that does not load (logged). |
| `static bool TransitionTo(string snapshot, float seconds = 0.5f)` | Blends the mix to a snapshot of the project's mixer: every value the snapshot sets (bus volumes and mutes, effect parameters) moves from where it is to the snapshot's over the time given, in steps that sound even (volumes in decibels, frequencies by octave); the values it does not set keep the mixer's own. A transition that starts while another runs carries on from where that one is. The blend runs on mixed audio, so a scene paused does not hold it. Returns whether the mix blends to it; false for a snapshot the mixer lacks (logged once per name), which changes nothing, and without sound. |
| `static float GetBusPeak(string bus)` | The peak of a bus's output over the last block the engine mixed (about 5 ms), after its effects and every gain: a linear level, 0 for silence and 1 for full scale (it can pass 1 before a limiter). It shows how loud a bus plays now, as a meter does; 0 for a bus the project's mixer lacks (logged once) and without sound. |

### AudioClip

```csharp
public sealed class AudioClip : Asset
```

An audio clip asset (WAV, FLAC, MP3 or Ogg Vorbis).


### AudioListenerComponent

```csharp
public sealed class AudioListenerComponent : Component
```

The ears of the scene: the first active listener hears it.

| Member | Summary |
|---|---|
| `bool Active { get; set; }` | Whether this listener can hear the scene. |

### AudioSourceComponent

```csharp
public sealed class AudioSourceComponent : Component
```

Plays an audio clip, optionally positioned in 3D.

| Member | Summary |
|---|---|
| `AudioClip? Clip { get; set; }` | The clip played, or null. |
| `bool Stream { get; set; }` | Whether the clip is read from its file while it plays instead of being loaded whole first: only a little of it is in memory at a time, which suits music and long ambience. Changing it, like changing `AudioSourceComponent.Clip`, makes a new sound that is stopped until `AudioSourceComponent.Play`. |
| `float Volume { get; set; }` | Linear gain; 1 plays the clip unchanged. |
| `float Pitch { get; set; }` | Playback speed multiplier, which also shifts the pitch. |
| `bool Loop { get; set; }` | Whether the clip repeats. |
| `bool PlayOnStart { get; set; }` | Whether the sound starts with the scene (or when the component is added while it runs). |
| `bool Spatial { get; set; }` | Whether the sound is positioned in 3D (otherwise it plays at a constant level). |
| `float MinDistance { get; set; }` | Distance in meters within which the sound plays at full volume. |
| `float MaxDistance { get; set; }` | Distance in meters beyond which the sound gets no quieter. |
| `string Bus { get; set; }` | The bus of the project's mixer the source's sounds play on, by name in any letter case: empty for Master, and a name the mixer lacks plays on Master (logged once). Changing it moves the sounds playing, without a click. |
| `uint Priority { get; set; }` | From 0 to 255 (higher values play as 255), 128 by default: when more sounds play than the engine mixes at once, those of lower priority go silent first. A one-shot playing takes a new priority too. |
| `uint MaxVoices { get; set; }` | How many sounds of the source play at once, its clip and its one-shots together: from 1 to 64 (values beyond play as the nearest), 4 by default. A one-shot past it stops the source's oldest one-shot. |
| `float DopplerLevel { get; set; }` | How much of the doppler effect the source's sounds take when it is spatial: their pitch rises as source and listener close in and falls as they part, by the factor c / (c - v * DopplerLevel), c being the mixer's speed of sound and v how fast they close in. 0 turns it off, 1 (the default) is physical, up to 5 exaggerates it (values beyond play as the nearest). Sources that are not spatial take none. |
| `bool IsPlaying { get; }` | Whether the sound is playing (held sounds of a paused scene count as playing). |
| `void Play()` | Plays the clip from the start, or from where `AudioSourceComponent.Pause` left it. |
| `void Pause()` | Pauses the sound, keeping its position. |
| `void Stop()` | Stops the sound and rewinds it. |
| `bool PlayOneShot(float volume = 1)` | Plays the source's clip once more on a voice of its own, over whatever the source plays already (a shot, a footstep): it never loops, follows the source as it moves and changes, and fades out with `AudioSourceComponent.Stop`. When the source already plays `AudioSourceComponent.MaxVoices` sounds, its oldest one-shot stops; with `AudioSourceComponent.MaxVoices` 1 and the clip playing, nothing more plays. A source whose clip streams loads it for its one-shots. Returns whether it started; false when the source has no clip that plays (or none at all: give it the clip to play) or nothing runs with sound. |
| `bool PlayOneShot(AudioClip clip, float volume = 1)` | Plays another clip once on a voice of the source, over whatever the source plays already, as `AudioSourceComponent.PlayOneShot` plays the source's own: it never loops, follows the source and fades out with `AudioSourceComponent.Stop`. A source with no clip of its own plays the clips it is given this way (footsteps, impacts). Returns whether it started; false when the source's own clip does not play, when nothing runs with sound, and for a clip that does not load (logged). |

### BoxColliderComponent

```csharp
public sealed class BoxColliderComponent : Component
```

A box collision shape of the entity's body.

| Member | Summary |
|---|---|
| `Vector3 HalfExtents { get; set; }` | Half the box's size along each local axis. |
| `Vector3 Offset { get; set; }` | Position of the box's center relative to the entity. |
| `bool IsTrigger { get; set; }` | Whether the collider only reports overlaps instead of colliding. |
| `float Friction { get; set; }` | Friction coefficient, used while the collider's Material (set in the editor) names no physics material: two colliders without materials combine theirs as the square root of the product. |
| `float Restitution { get; set; }` | Bounciness from 0 to 1, used while the collider's Material (set in the editor) names no physics material: of two colliders without materials, the larger applies. |

### CameraComponent

```csharp
public sealed class CameraComponent : Component
```

Renders the scene from the entity's position, looking down its -Z axis.

| Member | Summary |
|---|---|
| `ProjectionType Projection { get; set; }` | Perspective or orthographic projection. |
| `float PerspectiveFOV { get; set; }` | Vertical field of view of perspective projection, in degrees. |
| `float PerspectiveNear { get; set; }` | Near clipping distance of perspective projection. |
| `float PerspectiveFar { get; set; }` | Far view distance of perspective projection (culling and shadows). |
| `float OrthographicSize { get; set; }` | Full view height of orthographic projection, in world units. |
| `float OrthographicNear { get; set; }` | Near clipping distance of orthographic projection. |
| `float OrthographicFar { get; set; }` | Far clipping distance of orthographic projection. |
| `bool Primary { get; set; }` | Whether the scene renders from this camera (the first primary camera in the hierarchy). |
| `bool FixedAspectRatio { get; set; }` | Keeps `CameraComponent.AspectRatio` instead of following the viewport. |
| `float AspectRatio { get; set; }` | Width over height when `CameraComponent.FixedAspectRatio` is set. |
| `Ray ScreenToWorldRay(Vector2 screenPosition)` | The ray from the camera through a point of the scene's viewport, in pixels from its top-left corner. |
| `Vector3 WorldToScreenPoint(Vector3 worldPosition)` | Where a point of the world shows in the scene's viewport: X and Y in pixels from its top-left corner (as `Input.MousePosition` and `CameraComponent.ScreenToWorldRay` use them), and Z the point's distance in front of the camera along its view direction (negative behind it). A perspective camera shows only points in front of it: for a point with a Z of zero or less, X and Y mean nothing. An orthographic camera shows every point at its X and Y, and draws it when Z lies between `CameraComponent.OrthographicNear` and `CameraComponent.OrthographicFar` (behind the camera too, when the near distance is negative). |

### CapsuleColliderComponent

```csharp
public sealed class CapsuleColliderComponent : Component
```

A capsule collision shape along the entity's local Y axis.

| Member | Summary |
|---|---|
| `float Radius { get; set; }` | Radius of the capsule's cylinder and caps. |
| `float HalfHeight { get; set; }` | Half the height of the cylinder between the caps. |
| `Vector3 Offset { get; set; }` | Position of the capsule's center relative to the entity. |
| `bool IsTrigger { get; set; }` | Whether the collider only reports overlaps instead of colliding. |
| `float Friction { get; set; }` | Friction coefficient, used while the collider's Material (set in the editor) names no physics material: two colliders without materials combine theirs as the square root of the product. |
| `float Restitution { get; set; }` | Bounciness from 0 to 1, used while the collider's Material (set in the editor) names no physics material: of two colliders without materials, the larger applies. |

### CharacterControllerComponent

```csharp
public sealed class CharacterControllerComponent : Component
```

Makes the entity a character that scripts move, such as the player: an upright capsule that walks at the velocity given to `CharacterControllerComponent.Move`, slides along walls, climbs steps up to `CharacterControllerComponent.StepHeight` and slopes up to `CharacterControllerComponent.MaxSlopeAngle`, slides down steeper slopes, falls with gravity, rides what it stands on (moving platforms, elevators) and pushes dynamic bodies. The entity's transform follows the capsule, which does not turn with the entity: in `Script.OnFixedUpdate` it is where the last physics step left the character, and between steps it is drawn on its way there (a step behind), so it moves smoothly at any frame rate. Raycasts hit the character, and triggers and bodies report it in their contact callbacks like a kinematic body. The colliders of the entities below it are its own parts: they neither block it nor report touching it.

| Member | Summary |
|---|---|
| `float Radius { get; set; }` | Radius of the capsule in meters. |
| `float Height { get; set; }` | Height of the whole capsule in meters, rounded ends included; below twice the radius it is a sphere. |
| `Vector3 Offset { get; set; }` | Center of the capsule relative to the entity, in meters (scaled with the entity, not turned with it). |
| `float StepHeight { get; set; }` | The highest step the character climbs, and the deepest drop it walks down without leaving the ground, in meters. |
| `float MaxSlopeAngle { get; set; }` | The steepest slope the character walks up, in degrees (1 to 89); it slides down steeper ones. |
| `float Mass { get; set; }` | Kilograms with which the character weighs on what it stands on. |
| `float PushForce { get; set; }` | The largest force, in newtons, with which the character pushes the dynamic bodies it walks into; 0 leaves them where they are. |
| `float GravityFactor { get; set; }` | Multiplier of the scene's gravity for the character; 0 lets the script fly it (ladders, swimming). |
| `uint Layer { get; set; }` | Physics layer index (0-15) of the project settings: the character collides with the layers it collides with. |
| `Vector3 Velocity { get; set; }` | The character's own velocity in meters per second, on top of what `CharacterControllerComponent.Move` asks for: gravity speeds it up while it is in the air or on a slope too steep to stand on, landing on walkable ground stops it, and what it runs into removes the part towards it. Set an upward velocity to jump; on walkable ground, a velocity without an upward part stops at once. Zero while the scene does not simulate the character. |
| `bool IsGrounded { get; }` | Whether the character stands on ground it can walk on: false in the air and on slopes steeper than `CharacterControllerComponent.MaxSlopeAngle`. The capsule's round bottom rolls over the edges of steps and ledges, which are too steep to stand on, so walking down stairs or off a ledge it is false for a physics step or two at each edge: let a jump pressed a moment before landing or after leaving the ground still happen (the scripting guide's first-person controller does). |
| `Vector3 GroundNormal { get; }` | The normal of the ground under the character, walkable or too steep; zero when nothing is under it. |
| `void Move(Vector3 velocity)` | Walks the character at a velocity (meters per second, world space) during the next physics step: call it from `Script.OnFixedUpdate` every step the character should move (a later call before the step replaces the velocity). Gravity acts on top of it, so the velocity is usually horizontal; to jump, set `CharacterControllerComponent.Velocity`. |
| `void Teleport(Vector3 position)` | Places the entity at a world position at once, keeping its rotation, and stops the character, which finds the ground where it is placed. |

### Collision

```csharp
public static class Collision
```

The contact event being handled: readable inside a script's `Script.OnCollisionEnter`, `Script.OnCollisionExit`, `Script.OnTriggerEnter` and `Script.OnTriggerExit`, and while a graph's contact event runs, until its first wait (a flow the visual script debugger stopped continues with the contact it read). Physics reports contacts once per frame, after the frame's physics steps, so `Collision.Step` says which step a contact happened in. Outside a handler `Collision.Entity` and `Collision.Other` are null, `Collision.Step` is 0 and `Collision.Points` is empty.

| Member | Summary |
|---|---|
| `static Entity? Entity { get; }` | The entity whose handler runs (its script instance when it has one): the script's own entity. |
| `static Entity? Other { get; }` | The other entity of the contact (its script instance when it has one): the handler's argument. |
| `static ulong Step { get; }` | The physics step the contact began or ended in, counting the scene's physics steps from 1 (a contact that ends because an entity was destroyed between steps has the number of the step before). |
| `static ContactPoint[] Points { get; }` | Where the colliders touch as the contact begins, seen from `Collision.Entity`: up to four points, the strongest first (by `ContactPoint.Impulse`, then by position, normal, relative velocity and materials). Empty when a contact ends, for a contact sleeping bodies resume on waking (which raises no event), and outside a handler. A new array on each read. |

### Color

```csharp
public struct Color : IEquatable<Color>
```

A linear RGBA color with single-precision components (1 is full intensity; lights may exceed it).

| Member | Summary |
|---|---|
| `Color(float r, float g, float b, float a = 1)` | Creates a color from its components. |
| `float R` | The red component. |
| `float G` | The green component. |
| `float B` | The blue component. |
| `float A` | The alpha (opacity) component. |
| `static Color White { get; }` | Opaque white. |
| `static Color Black { get; }` | Opaque black. |
| `static Color Red { get; }` | Opaque red. |
| `static Color Green { get; }` | Opaque green. |
| `static Color Blue { get; }` | Opaque blue. |
| `static Color Clear { get; }` | Fully transparent black. |
| `static Color Lerp(Color a, Color b, float t)` | Linear interpolation from `a` (t = 0) to `b` (t = 1); t is not clamped. |
| `static explicit operator Vector4(Color color)` | The components as a vector (R, G, B, A). |
| `static explicit operator Color(Vector4 vector)` | A color from a vector (X, Y, Z, W as R, G, B, A). |
| `static bool operator ==(Color a, Color b)` | Exact component-wise equality. |
| `static bool operator !=(Color a, Color b)` | Exact component-wise inequality. |
| `bool Equals(Color other)` | Exact component-wise equality. |

### Component

```csharp
public abstract class Component
```

Base class of the engine components scripts can access through `Entity.GetComponent`. Component objects are views: they read and write the entity's data directly.

| Member | Summary |
|---|---|
| `Entity Entity { get; }` | The entity the component belongs to. |

### ContactPoint

```csharp
public struct ContactPoint
```

Where and how two colliders touch as their contact begins, seen from one of them: the entity whose handler reads it (`Collision.Entity`), against `Collision.Other`.

| Member | Summary |
|---|---|
| `Vector3 Point` | The world-space point where they touch: halfway between the two surfaces (a contact found just before they touch has it in the gap between them). |
| `Vector3 Normal` | The contact's normal, unit length, from the other collider towards this one: the way this entity is pushed. |
| `float Impulse` | The impulse along the normal at this point (newton seconds) that stops the two bodies' approach and, with their restitution, bounces them apart: how hard the impact is. Estimated by the physics engine from the two bodies alone, for the points that share this point's normal, as if they alone stopped the bodies: when a contact begins on surfaces at an angle at once (a ball landing in a groove), each surface's points are not shared out with the others', so they read too high when the surfaces' normals are less than 90 degrees apart and too low when more. Inaccurate as well when more than two bodies collide at once; 0 for a trigger and for a contact without a dynamic body. |
| `Vector3 RelativeVelocity` | This entity's velocity at the point minus the other's (meters per second) as the contact began: they approach when it points against `ContactPoint.Normal`. |
| `PhysicsMaterial? Material` | The physics material of this side's surface at the point (its collider's Material, or for a triangle of a mesh collider its submesh's); null when the collider's own Friction and Restitution apply. |
| `PhysicsMaterial? OtherMaterial` | The physics material of the other side's surface at the point (see `ContactPoint.Material`); its `PhysicsMaterial.Surface` tells what was hit. |

### CursorMode

```csharp
public enum CursorMode
```

How the mouse cursor behaves.

Values: `Normal`, `Hidden`, `Locked`.

### Curve

```csharp
public sealed class Curve : Asset
```

A curve asset (`.scurve`): keys with values, how each segment interpolates (smooth, linear or a step) and their slopes, and what the curve does before its first key and after its last (hold, loop, ping-pong or go on along a line). Use one to map a number to another, such as a speed to an engine's pitch or a time to a light's intensity. Edit it in the editor's curve editor; scripts read it.

| Member | Summary |
|---|---|
| `float Evaluate(float time)` | The curve's value at `time` (the curve's input: a time, a speed or a distance). A curve without keys is 0 everywhere; a curve that cannot be loaded is 0 too (logged). An edit of the asset while the game plays shows on the next frame. |

### Debug

```csharp
public static class Debug
```

Drawing for debugging, in the editor and development builds (shipped Dist games draw nothing).

| Member | Summary |
|---|---|
| `static void DrawLine(Vector3 from, Vector3 to, Color color, float duration = 0)` | Draws a line in world space for `duration` seconds of game time (0 draws it in the next frame only), hidden behind surfaces. |

### DirectionalLightComponent

```csharp
public sealed class DirectionalLightComponent : Component
```

Light from infinitely far away, shining along the entity's -Z axis.

| Member | Summary |
|---|---|
| `Color Color { get; set; }` | The light's color (the alpha is ignored). |
| `float Intensity { get; set; }` | Brightness multiplier. |
| `bool CastShadows { get; set; }` | Whether the light casts shadows. |
| `float LightSize { get; set; }` | Apparent angular diameter of the light source in degrees; larger values soften shadows. |

### Entity

```csharp
public class Entity : IEquatable<Entity>
```

An object of the running scene, identified by its ID. Entities compare equal when their IDs do.

| Member | Summary |
|---|---|
| `protected Entity()` | For scripts: the engine sets the ID before `Script.OnCreate`. |
| `ulong ID { get; }` | The entity's unique, persistent identifier (0 for no entity). |
| `bool IsValid { get; }` | Whether the entity exists in the running scene (destroyed entities stop existing at the end of the frame). |
| `string Name { get; set; }` | The entity's name. |
| `TransformComponent Transform { get; }` | The entity's transform (every entity has one). |
| `Vector3 Translation { get; set; }` | The position relative to the parent. |
| `Quaternion Rotation { get; set; }` | The rotation relative to the parent. |
| `Vector3 Scale { get; set; }` | The scale relative to the parent. |
| `Entity? Parent { get; set; }` | The parent entity, or null at the root. Setting it keeps the world transform; null moves the entity to the root. |
| `Entity[] Children { get; }` | The child entities in order. |
| `bool HasComponent<T>()` | Whether the entity has a component of type `T`. |
| `T? GetComponent<T>()` | The entity's component of type `T`, or null when it has none. |
| `T AddComponent<T>()` | Adds a component of type `T` with default values (keeping the one the entity has) and returns it. |
| `void RemoveComponent<T>()` | Removes the entity's component of type `T` (every entity keeps its transform). |
| `T? As<T>()` | The entity's script instance as `T`, or null when it runs no such script. |
| `void Destroy()` | Destroys the entity and its children at the end of the frame. |
| `static Entity Create(string name)` | Creates an entity at the root of the running scene. |
| `static Entity? Instantiate(Prefab prefab, Vector3 position, Quaternion rotation, Entity? parent = null)` | Creates an instance of a prefab with its root at a world position and rotation (keeping the prefab's scale), under `parent` or at the root of the running scene. The instance's scripts have run OnCreate when this returns. Returns the root (its script instance when it runs a script), or null (logged) when the prefab cannot be instantiated. |
| `static Entity? FindByName(string name)` | The first entity (in hierarchy order) with the given name, or null. Entities running a script are returned as their script instance. |
| `static Entity? FindByID(ulong id)` | The entity with the given ID, or null when there is none. Entities running a script are returned as their script instance. |
| `static bool operator ==(Entity? a, Entity? b)` | Whether both refer to the same entity (or both are null). |
| `static bool operator !=(Entity? a, Entity? b)` | Whether they refer to different entities. |
| `bool Equals(Entity? other)` | Whether `other` refers to the same entity. |

### EnvironmentMap

```csharp
public sealed class EnvironmentMap : Asset
```

An HDR environment map: the sky and image-based lighting of a sky light.


### Font

```csharp
public sealed class Font : Asset
```

A font asset for text.


### ForceMode

```csharp
public enum ForceMode
```

How `RigidBodyComponent.AddForce` and `RigidBodyComponent.AddTorque` interpret their value.

Values: `Force`, `Impulse`, `Acceleration`, `VelocityChange`.

### GamepadAxis

```csharp
public enum GamepadAxis
```

Gamepad axes in the standard layout: sticks from -1 to 1 (up is -1), triggers from -1 (released) to 1.

Values: `LeftX`, `LeftY`, `RightX`, `RightY`, `LeftTrigger`, `RightTrigger`.

### GamepadButton

```csharp
public enum GamepadButton
```

Gamepad buttons in the standard (SDL_GameControllerDB) layout.

Values: `A`, `B`, `X`, `Y`, `LeftBumper`, `RightBumper`, `Back`, `Start`, `Guide`, `LeftThumb`, `RightThumb`, `DPadUp`, `DPadRight`, `DPadDown`, `DPadLeft`.

### HideInInspectorAttribute

```csharp
public sealed class HideInInspectorAttribute : Attribute
```

Keeps a serialized field out of the inspector; its value is still saved with the scene.

| Member | Summary |
|---|---|
| `HideInInspectorAttribute()` |  |

### Input

```csharp
public static class Input
```

Keyboard, mouse and gamepad state of the current frame, and the project's input actions (named values such as Jump or Steer that bindings give from keys and gamepads alike). "Pressed" and "released" are true only during the frame the button changed.

| Member | Summary |
|---|---|
| `static Vector2 MousePosition { get; }` | The cursor position in pixels from the window's top-left corner. |
| `static Vector2 MouseDelta { get; }` | How far the mouse moved this frame, in pixels. |
| `static Vector2 MouseScrollDelta { get; }` | How far the mouse wheel scrolled this frame (Y is the usual wheel). |
| `static bool IsPointerOverUI { get; }` | Whether the mouse is over the screen-space UI: an image or a button of a canvas is under the cursor (as of this frame's UI update, before `OnUpdate`), so a click there goes to the UI. Scripts that act on mouse clicks in the world skip those, so clicking a HUD or menu button does not also shoot, place or select in the game. False while the cursor is locked. |
| `static CursorMode CursorMode { get; set; }` | Whether the cursor is visible, hidden or locked. |
| `static RebindResult CurrentRebind { get; }` | The latest capture of the running scene (`Input.StartRebind`): `RebindStatus.Listening` while it waits, then how it ended, with the conflicts when `RebindStatus.Done`; `RebindStatus.None` before any. |
| `static bool IsKeyDown(KeyCode key)` | Whether the key is held down. |
| `static bool IsKeyPressed(KeyCode key)` | Whether the key went down this frame. |
| `static bool IsKeyReleased(KeyCode key)` | Whether the key went up this frame. |
| `static bool IsMouseButtonDown(MouseButton button)` | Whether the mouse button is held down. |
| `static bool IsMouseButtonPressed(MouseButton button)` | Whether the mouse button went down this frame. |
| `static bool IsMouseButtonReleased(MouseButton button)` | Whether the mouse button went up this frame. |
| `static bool IsGamepadConnected(int gamepad = 0)` | Whether a gamepad is connected at the index (0-15). |
| `static float GetGamepadAxis(GamepadAxis axis, int gamepad = 0)` | The position of a gamepad axis (0 when the gamepad is not connected). |
| `static bool IsGamepadButtonDown(GamepadButton button, int gamepad = 0)` | Whether a gamepad button is held down. |
| `static bool IsGamepadButtonPressed(GamepadButton button, int gamepad = 0)` | Whether a gamepad button went down this frame. |
| `static bool IsGamepadButtonReleased(GamepadButton button, int gamepad = 0)` | Whether a gamepad button went up this frame. |
| `static float GetAction(string action)` | The value of an input action of the project's `InputActions`: a Button action's from 0 (up) to 1 (more with a binding's scale), an Axis action's signed value (typically -1 to 1), a Vector2 action's length. It is the reading of the binding with the largest magnitude (after its dead zone, response curve, inversion and scale) or the action's override (`Input.SetActionOverride`), evaluated once per frame before `OnUpdate`. Names match with their letter case; an unknown name, and every name in a project that names no actions, reads 0 (logged once per name). |
| `static Vector2 GetActionVector(string action)` | The value of an input action as a vector: a Vector2 action's, +Y up (a stick pushed forward, the W of a WASD composite); (value, 0) for a Button or Axis action. Reads as `Input.GetAction` does. |
| `static bool IsActionDown(string action)` | Whether an input action is down: its value reached its press threshold and has not fallen below its release threshold since. Every action type has it, so an axis pushed far enough is down too. |
| `static bool IsActionPressed(string action)` | Whether an input action went down: in `OnUpdate` and the frame's events, during the frame it went down; in `OnFixedUpdate`, during exactly one physics step, the first at or after that frame (carried across frames that run no step), so fixed steps never miss a press nor see one twice. A press and release within one frame reads pressed and released, and not down. |
| `static bool IsActionReleased(string action)` | Whether an input action went up again, by the same frame and fixed-step rule as `Input.IsActionPressed`. |
| `static void SetActionOverride(string action, float value)` | Puts a value on an input action as one more device would (on a Vector2 action, the vector (value, 0)): the action takes it when it is larger than what its bindings read, and it presses and releases the action as a button does (a press when the value leaves 0, a release when it returns to 0 or is cleared). It is read from the next frame's evaluation on and held, across scene loads, until `Input.ClearActionOverride`: how a graph drives a game (an autopilot, an attract mode) and how a test scene presses a button. A value set and cleared before the next frame still reaches it as a press and a release. |
| `static void SetActionVectorOverride(string action, Vector2 value)` | Puts a vector on an input action as `Input.SetActionOverride` does, for a Vector2 action (+Y up); a Button or Axis action reads its X. |
| `static void ClearActionOverride(string action)` | Ends the override of an input action (`Input.SetActionOverride`): from the next frame's evaluation on the action reads its bindings alone, released if only the override held it down. Nothing for an action without one. |
| `static RebindResult StartRebind(string action, int binding, int part = -1)` | Listens for the next key, mouse button, gamepad button or axis the player gives, and binds it to binding `binding` (its index in the action) of the action: the whole binding with `part` -1, or one control of a composite ("Key/A Key/D": part 0 is A, 1 is D). A key or button counts when it is pressed after this call, an axis when it moves past half way (a Vector2 action takes a stick; a part takes an axis in its positive direction); gamepads count only those the binding reads. Escape, or 10 seconds of frames (counted also while `Time.TimeScale` is 0), cancel it, as does `Input.CancelRebind` or another StartRebind. Answers `RebindStatus.Listening`, or `RebindStatus.Refused` as `Input.Rebind` refuses; read how it ends from `Input.CurrentRebind`. The control that ends the capture (the one bound, or Escape) presses no action in that frame, whichever reads it. |
| `static RebindResult Rebind(string action, int binding, string controls, int part = -1)` | Binds controls to binding `binding` of an input action while the game runs: with `part` -1 the binding's controls as text ("Key/J", "Key/A Key/D", "Gamepad/LeftStick", or "" to leave it unbound), else the one control that replaces that part of a composite. The new binding is read from the next frame on, by every running scene and the scenes loaded later, and kept for the next run in the player's profile in the game's data folder; the actions asset is never changed. The answer lists the other actions that now read one of the same controls (the engine does not unbind them). Refused, changing nothing, for an unknown action, a binding or part it lacks, or controls that do not fit the action's type. |
| `static void CancelRebind()` | Ends the capture `Input.StartRebind` started, if one still listens: `Input.CurrentRebind` then reads `RebindStatus.Cancelled`. |
| `static RebindResult ResetBindings()` | Every binding back to the actions asset's, and the player's profile emptied: `RebindStatus.Done`, or `RebindStatus.Refused` when the project names no input actions. |
| `static string GetBindingControls(string action, int binding)` | The controls binding `binding` of an input action reads now, as text ("Key/Space", "Key/A Key/D"), the player's rebinding included: what a settings screen shows. Empty for an action or binding the project's actions lack. |

### InputActions

```csharp
public sealed class InputActions : Asset
```

An input actions asset (`.sinput`): the named actions a game reads instead of keys and gamepads (Throttle, Steer, Jump), each with its bindings. The project names the one its scenes evaluate (Project Settings, Input Actions); scripts read its actions by name through `Input.GetAction`, `Input.IsActionPressed` and the other action queries, and `Script.OnActionPressed` and `Script.OnActionReleased` report their edges. Edit it in the editor; an edit made while the game plays shows on the next frame. An asset that cannot be loaded has no actions (logged).

| Member | Summary |
|---|---|
| `string[] ActionNames { get; }` | The names of the asset's actions, in its order (the names `Input.GetAction` and the other action queries take, with their letter case): what a settings screen lists. A new array on each read. |

### JointComponent

```csharp
public sealed class JointComponent : Component
```

Joins this entity's body to another entity's body, or to the world, while the scene runs: welded (`JointType.Fixed`), turning around an axis (`JointType.Hinge`, with limits and a motor), turning freely (`JointType.Ball`), kept within distances (`JointType.Distance`) or sliding along an axis (`JointType.Slider`). Both entities need a rigid body with a collider. Changing a property makes the joint again from where the bodies are.

| Member | Summary |
|---|---|
| `JointType Type { get; set; }` | How the bodies are joined. |
| `Entity? ConnectedEntity { get; set; }` | The entity whose body this one is joined to, or null for the world. |
| `Vector3 Anchor { get; set; }` | Where the joint is, in this entity's local space. |
| `Vector3 ConnectedAnchor { get; set; }` | Distance joints: the far point, in the connected entity's local space (world space without one). |
| `Vector3 Axis { get; set; }` | The hinge or slider axis in this entity's local space. |
| `bool LimitsEnabled { get; set; }` | Whether a hinge's angle or a slider's travel is limited. |
| `float MinLimit { get; set; }` | Hinges: the lowest angle in degrees (down to -180); sliders: meters (0 is the start). |
| `float MaxLimit { get; set; }` | Hinges: the highest angle in degrees (up to 180); sliders: meters. |
| `float MinDistance { get; set; }` | Distance joints: the shortest distance; -1 keeps the starting distance. |
| `float MaxDistance { get; set; }` | Distance joints: the longest distance; -1 keeps the starting distance. |
| `bool MotorEnabled { get; set; }` | Whether a motor drives the hinge or slider at `JointComponent.MotorSpeed`. |
| `float MotorSpeed { get; set; }` | Hinges: degrees per second; sliders: meters per second. |
| `float MotorMaxForce { get; set; }` | The most torque (hinges, newton meters) or force (sliders, newtons) the motor uses. |

### JointType

```csharp
public enum JointType
```

How a `JointComponent` joins two bodies.

Values: `Fixed`, `Hinge`, `Ball`, `Distance`, `Slider`.

### KeyCode

```csharp
public enum KeyCode
```

Keyboard keys by their position on a US keyboard layout (GLFW's key codes).

Values: `Unknown`, `Space`, `Apostrophe`, `Comma`, `Minus`, `Period`, `Slash`, `D0`, `D1`, `D2`, `D3`, `D4`, `D5`, `D6`, `D7`, `D8`, `D9`, `Semicolon`, `Equal`, `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H`, `I`, `J`, `K`, `L`, `M`, `N`, `O`, `P`, `Q`, `R`, `S`, `T`, `U`, `V`, `W`, `X`, `Y`, `Z`, `LeftBracket`, `Backslash`, `RightBracket`, `GraveAccent`, `World1`, `World2`, `Escape`, `Enter`, `Tab`, `Backspace`, `Insert`, `Delete`, `Right`, `Left`, `Down`, `Up`, `PageUp`, `PageDown`, `Home`, `End`, `CapsLock`, `ScrollLock`, `NumLock`, `PrintScreen`, `Pause`, `F1`, `F2`, `F3`, `F4`, `F5`, `F6`, `F7`, `F8`, `F9`, `F10`, `F11`, `F12`, `F13`, `F14`, `F15`, `F16`, `F17`, `F18`, `F19`, `F20`, `F21`, `F22`, `F23`, `F24`, `F25`, `KP0`, `KP1`, `KP2`, `KP3`, `KP4`, `KP5`, `KP6`, `KP7`, `KP8`, `KP9`, `KPDecimal`, `KPDivide`, `KPMultiply`, `KPSubtract`, `KPAdd`, `KPEnter`, `KPEqual`, `LeftShift`, `LeftControl`, `LeftAlt`, `LeftSuper`, `RightShift`, `RightControl`, `RightAlt`, `RightSuper`, `Menu`.

### Log

```csharp
public static class Log
```

Writes messages to the engine's script log: the editor console, the terminal and the log file.

| Member | Summary |
|---|---|
| `static void Trace(string message)` | Logs a detailed diagnostic message. |
| `static void Info(string message)` | Logs an informational message. |
| `static void Warn(string message)` | Logs a warning. |
| `static void Error(string message)` | Logs an error. |

### Material

```csharp
public sealed class Material : Asset
```

A material asset: the metallic-roughness surface of meshes (colors are linear). The project's materials are shared by every scene, so scripts change copies made with `Material.Create` or `Material.Clone`; those last until the scene stops. Changing a shared material logs a script error and does nothing.

| Member | Summary |
|---|---|
| `bool IsEditable { get; }` | Whether scripts can change the material: it was made with `Material.Create` or `Material.Clone` by the running scene. |
| `Color BaseColor { get; set; }` | Albedo and opacity. |
| `float Metallic { get; set; }` | 0 for dielectrics, 1 for metals. |
| `float Roughness { get; set; }` | 0 for mirrors, 1 for fully diffuse surfaces. |
| `Color Emissive { get; set; }` | The color of emitted light (alpha is ignored). |
| `float EmissiveIntensity { get; set; }` | Multiplier of the emitted light. |
| `float NormalStrength { get; set; }` | Scales the bumps of the normal map. |
| `float OcclusionStrength { get; set; }` | How much the occlusion map darkens indirect light (0 to 1). |
| `Texture? BaseColorTexture { get; set; }` | Base color texture (sRGB colors, opacity in alpha), or null. |
| `Texture? NormalTexture { get; set; }` | Tangent-space normal map (+Y up), or null. |
| `Texture? MetallicRoughnessTexture { get; set; }` | Roughness (green) and metallic (blue) map multiplied with the factors, or null. |
| `Texture? OcclusionTexture { get; set; }` | Ambient occlusion map (red), or null. |
| `Texture? EmissiveTexture { get; set; }` | Emitted color map (sRGB) multiplied with `Material.Emissive` and `Material.EmissiveIntensity`, or null. |
| `MaterialAlphaMode AlphaMode { get; set; }` | How the alpha of the base color is used. |
| `float AlphaCutoff { get; set; }` | The alpha below which `MaterialAlphaMode.Mask` discards pixels. |
| `bool DoubleSided { get; set; }` | Whether back faces are rendered too. |
| `Vector2 UVTiling { get; set; }` | Texture coordinate scale. |
| `Vector2 UVOffset { get; set; }` | Texture coordinate offset. |
| `static Material? Create()` | A new material with default parameters (white, not metallic, half rough), or null (logged) without a running scene. |
| `Material? Clone()` | A copy of the material that scripts can change, or null (logged) when it cannot be made. |

### MaterialAlphaMode

```csharp
public enum MaterialAlphaMode
```

How a material uses the alpha of its base color.

Values: `Opaque`, `Mask`, `Blend`.

### Mathf

```csharp
public static class Mathf
```

Single-precision math helpers for games.

| Member | Summary |
|---|---|
| `const float PI = 3.1415927f` | The ratio of a circle's circumference to its diameter. |
| `const float Deg2Rad = 0.017453292f` | Multiply degrees by this to get radians. |
| `const float Rad2Deg = 57.295776f` | Multiply radians by this to get degrees. |
| `const float Epsilon = 1E-45f` | The smallest positive float. |
| `static float Abs(float value)` | The absolute value. |
| `static float Min(float a, float b)` | The smaller value. |
| `static float Max(float a, float b)` | The larger value. |
| `static float Clamp(float value, float min, float max)` | The value limited to [min, max]. |
| `static float Clamp01(float value)` | The value limited to [0, 1]. |
| `static float Lerp(float a, float b, float t)` | Linear interpolation from `a` to `b`, with t clamped to [0, 1]. |
| `static float LerpUnclamped(float a, float b, float t)` | Linear interpolation from `a` to `b` without clamping t. |
| `static float InverseLerp(float a, float b, float value)` | Where `value` lies between `a` (0) and `b` (1), clamped; 0 when they are equal. |
| `static float SmoothStep(float from, float to, float t)` | Hermite interpolation: eases in and out between `from` and `to`. |
| `static float MoveTowards(float current, float target, float maxDelta)` | Moves `current` towards `target` by at most `maxDelta`. |
| `static float DeltaAngle(float current, float target)` | The shortest difference between two angles in degrees, in [-180, 180). |
| `static float Repeat(float value, float length)` | Wraps the value into [0, length). |
| `static float PingPong(float value, float length)` | Bounces the value between 0 and `length`. |
| `static bool Approximately(float a, float b)` | Whether two values are equal within a small tolerance relative to their size. |
| `static float Sign(float value)` | -1, 0 or 1. |
| `static float Sqrt(float value)` | The square root. |
| `static float Pow(float value, float power)` | `value` raised to `power`. |
| `static float Exp(float value)` | e raised to the value. |
| `static float Log(float value)` | The natural logarithm. |
| `static float Floor(float value)` | The largest whole number not above the value. |
| `static float Ceil(float value)` | The smallest whole number not below the value. |
| `static float Round(float value)` | The nearest whole number (halves to even). |
| `static float Sin(float radians)` | The sine of an angle in radians. |
| `static float Cos(float radians)` | The cosine of an angle in radians. |
| `static float Tan(float radians)` | The tangent of an angle in radians. |
| `static float Asin(float value)` | The angle in radians whose sine is the value. |
| `static float Acos(float value)` | The angle in radians whose cosine is the value. |
| `static float Atan(float value)` | The angle in radians whose tangent is the value. |
| `static float Atan2(float y, float x)` | The angle in radians of the point (x, y) from the X axis. |

### Matrix4

```csharp
public struct Matrix4 : IEquatable<Matrix4>
```

A 4x4 matrix of single-precision values stored as four columns, like the engine's matrices. Matrices transform column vectors: `matrix * vector`, and `a * b` applies `b` first.

| Member | Summary |
|---|---|
| `Matrix4(Vector4 column0, Vector4 column1, Vector4 column2, Vector4 column3)` | Creates a matrix from its columns. |
| `Vector4 Column0` | The first column (the X axis of a transform). |
| `Vector4 Column1` | The second column (the Y axis of a transform). |
| `Vector4 Column2` | The third column (the Z axis of a transform). |
| `Vector4 Column3` | The fourth column (the translation of a transform). |
| `static Matrix4 Identity { get; }` | The matrix that changes nothing. |
| `float this[int column, int row] { get; set; }` | The element at a column and row (each 0 to 3). |
| `Matrix4? Inverse { get; }` | The matrix that undoes this one, or null when it is singular. |
| `static Matrix4 Translation(Vector3 translation)` | A translation. |
| `static Matrix4 Rotation(Quaternion rotation)` | A rotation. |
| `static Matrix4 Scaling(Vector3 scale)` | A scale along the axes. |
| `static Matrix4 TRS(Vector3 translation, Quaternion rotation, Vector3 scale)` | Scale, then rotation, then translation: an entity's transform. |
| `Vector3 TransformPoint(Vector3 point)` | Transforms a point (w = 1). |
| `Vector3 TransformDirection(Vector3 direction)` | Transforms a direction (w = 0: translation does not apply). |
| `static Matrix4 operator *(Matrix4 a, Matrix4 b)` | The product: `b` applied first, then `a`. |
| `static Vector4 operator *(Matrix4 matrix, Vector4 vector)` | Transforms a vector. |
| `static bool operator ==(Matrix4 a, Matrix4 b)` | Exact element-wise equality. |
| `static bool operator !=(Matrix4 a, Matrix4 b)` | Exact element-wise inequality. |
| `bool Equals(Matrix4 other)` | Exact element-wise equality. |

### Mesh

```csharp
public sealed class Mesh : Asset
```

A mesh asset (imported model or built-in shape).


### MeshColliderComponent

```csharp
public sealed class MeshColliderComponent : Component
```

A collision shape from a mesh: its convex hull, or its triangles (static and kinematic bodies).

| Member | Summary |
|---|---|
| `Mesh? Mesh { get; set; }` | The collision mesh, or null for the entity's MeshComponent mesh. |
| `bool Convex { get; set; }` | Whether the shape is the mesh's convex hull (required for dynamic bodies). |
| `bool IsTrigger { get; set; }` | Whether the collider only reports overlaps instead of colliding. |
| `float Friction { get; set; }` | Friction coefficient, used while the collider's Material (set in the editor) names no physics material: two colliders without materials combine theirs as the square root of the product. |
| `float Restitution { get; set; }` | Bounciness from 0 to 1, used while the collider's Material (set in the editor) names no physics material: of two colliders without materials, the larger applies. |

### MeshComponent

```csharp
public sealed class MeshComponent : Component
```

Renders a mesh asset with its materials.

| Member | Summary |
|---|---|
| `Mesh? Mesh { get; set; }` | The rendered mesh, or null. |
| `bool CastShadows { get; set; }` | Whether the mesh casts shadows. |
| `bool Visible { get; set; }` | Whether the mesh is rendered. |
| `int MaterialCount { get; }` | The number of material overrides (submeshes without one use the mesh's own material). |
| `Material? GetMaterial(int index)` | The material override of a submesh, or null when it uses the mesh's own material. |
| `void SetMaterial(int index, Material? material)` | Overrides the material of a submesh; null restores the mesh's own material. |

### Mixer

```csharp
public sealed class Mixer : Asset
```

A mixer asset (`.smixer`): the buses sounds play on, in a tree under Master, each with its volume, its mute and its effects. The project names the mixer its games play through; a sound source names its bus (`AudioSourceComponent.Bus`). Edit it in the editor; scripts read its buses, for example to list the volumes a settings menu offers, and set the game's own values of a bus through `Audio`, which never change the asset. Bus names match ignoring letter case, and the empty name is Master's. An edit made while the game plays shows on the next frame. A mixer that cannot be loaded reads as a new one, Master alone (logged).

| Member | Summary |
|---|---|
| `int BusCount { get; }` | How many buses the mixer has, Master included (at least 1, at most 32). |
| `string GetBusName(int index)` | The name of a bus by its place in the mixer's list (Master is the one without a parent); empty for an index outside the list. |
| `bool HasBus(string bus)` | Whether the mixer has a bus of that name. |
| `string GetBusParent(string bus)` | The name of the bus a bus feeds; empty for Master, and for a bus the mixer lacks. |
| `float GetBusVolume(string bus)` | The mixer's own volume of a bus, in decibels (-80 is silence); 0 for a bus the mixer lacks. The game's volume comes after it (`Audio.GetBusVolume`). |
| `bool IsBusMuted(string bus)` | Whether the mixer mutes a bus; false for a bus the mixer lacks. The game's mute is `Audio.IsBusMuted`. |

### MouseButton

```csharp
public enum MouseButton
```

Mouse buttons.

Values: `Left`, `Right`, `Middle`, `Button3`, `Button4`, `Button5`, `Button6`, `Button7`.

### ParticleBlendMode

```csharp
public enum ParticleBlendMode
```

How particles combine with what is behind them.

Values: `Additive`, `Alpha`.

### ParticleEmitterShape

```csharp
public enum ParticleEmitterShape
```

Where a particle system's particles start.

Values: `Point`, `Sphere`, `Cone`, `Box`.

### ParticleSystemComponent

```csharp
public sealed class ParticleSystemComponent : Component
```

Emits camera-facing particles (fire, sparks, smoke, dust, magic) while the scene runs: each lives between `ParticleSystemComponent.LifetimeMin` and `ParticleSystemComponent.LifetimeMax` seconds, starts from the `ParticleSystemComponent.Shape` with a speed between `ParticleSystemComponent.SpeedMin` and `ParticleSystemComponent.SpeedMax`, falls with the scene's gravity times `ParticleSystemComponent.GravityFactor`, slows down with `ParticleSystemComponent.Drag`, and blends from the start to the end color and size.

| Member | Summary |
|---|---|
| `bool Emitting { get; set; }` | Whether the system emits `ParticleSystemComponent.Rate` particles per second. |
| `float Rate { get; set; }` | Particles emitted per second while `ParticleSystemComponent.Emitting`. |
| `uint Burst { get; set; }` | Particles emitted at once when the system starts (with the scene, or when the entity is created). |
| `uint MaxParticles { get; set; }` | How many particles can be alive at once; emission waits while the system is full. |
| `float LifetimeMin { get; set; }` | Shortest life of a particle in seconds. |
| `float LifetimeMax { get; set; }` | Longest life of a particle in seconds. |
| `float SpeedMin { get; set; }` | Slowest start speed in meters per second. |
| `float SpeedMax { get; set; }` | Fastest start speed in meters per second. |
| `ParticleEmitterShape Shape { get; set; }` | Where particles start and which way they go. |
| `float ConeAngle { get; set; }` | Spread around the forward axis (-Z) in degrees, for cones, boxes and points. |
| `float Radius { get; set; }` | Radius of the sphere or of the cone's base, in meters. |
| `Vector3 BoxHalfExtents { get; set; }` | Half size of the box shape in meters. |
| `float GravityFactor { get; set; }` | Multiplies the scene's gravity; negative values make particles rise. |
| `float Drag { get; set; }` | Fraction of the velocity lost per second. |
| `float StartSize { get; set; }` | Width in meters when a particle is born. |
| `float EndSize { get; set; }` | Width in meters when a particle dies. |
| `Color StartColor { get; set; }` | Color and opacity at birth. |
| `Color EndColor { get; set; }` | Color and opacity at death. |
| `float Intensity { get; set; }` | Multiplies the colors; values above 1 glow with bloom. |
| `Texture? Texture { get; set; }` | The particles' texture, or null for a soft round dot. |
| `ParticleBlendMode BlendMode { get; set; }` | Whether particles add light or cover what is behind them. |
| `bool WorldSpace { get; set; }` | Whether particles stay where they were emitted when the entity moves (otherwise they move with it). |
| `bool DestroyWhenFinished { get; set; }` | Whether the entity is destroyed once it no longer emits and its last particle died. |
| `uint ParticleCount { get; }` | How many particles are alive. |
| `void Emit(uint count)` | Emits particles at once (fewer when `ParticleSystemComponent.MaxParticles` are alive), such as an explosion's burst. |
| `void Clear()` | Removes every particle at once. |

### Physics

```csharp
public static class Physics
```

Queries and settings of the running scene's physics. Queries read the world as its last physics step left it: in `Script.OnFixedUpdate` that is where bodies are; in `Script.OnUpdate` a body drawn between its last two steps is found up to a step ahead of where it is drawn, and an entity moved since the last step (by a script, or with its parent) is found where it was until the next step takes its body there. A body made or changed since the last step is found where its entity is. A query sees the bodies and characters on the layers of its layer mask (bit n is layer n), each entity at most once, and trigger colliders only when it includes them. Directions need not be unit length. Values that are not finite, sizes below 0, radii above 100 meters and half extents or half heights above 20 meters are script errors and find nothing (longer shapes lose the physics engine's precision); a zero direction or a distance that is not positive finds nothing. Queries allocate no managed memory once the entities they find have been met: the `Entity` of an entity without a script is made when a query first finds it, then kept.

| Member | Summary |
|---|---|
| `const uint AllLayers = 4294967295` | Every physics layer. |
| `static Vector3 Gravity { get; set; }` | The acceleration of gravity in meters per second squared. |
| `static bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit, uint layerMask = 4294967295)` | The closest solid collider along a ray, on the layers of `layerMask` (bit n is layer n). Triggers and colliders containing the origin are ignored. |
| `static bool Raycast(Ray ray, float maxDistance, out RaycastHit hit, uint layerMask = 4294967295)` | The closest solid collider along a ray (see `Physics.Raycast`). |
| `static int RaycastAll(Vector3 origin, Vector3 direction, float maxDistance, Span<RaycastHit> hits, uint layerMask = 4294967295, bool includeTriggers = false)` | Every entity along a ray, each at its nearest hit, nearest first (at equal distances by entity ID), written into `hits`; returns how many were written. A buffer smaller than the hits keeps the nearest. As with `Physics.Raycast`, a ray starting inside a convex collider passes out of it, and triangles are hit from their front only; triggers count when `includeTriggers` is true. Allocates no managed memory once the entities it finds have been met. Graphs have no node of it: they hold arrays as values, which a node may not write into. |
| `static bool SphereCast(Vector3 origin, float radius, Vector3 direction, float maxDistance, out ShapeCastHit hit, uint layerMask = 4294967295, bool includeTriggers = false)` | The first entity a sphere of `radius` touches as it moves from `origin` (its center) along `direction` for at most `maxDistance`: of those it touches, the nearest (at equal distances by entity ID). A collider the sphere overlaps where it starts is touched at distance 0, whichever way it moves, and so is the caster's own collider when the cast starts inside it: leave its layer out of `layerMask`, or start outside it. Triangles are touched from their front only, as by rays. |
| `static bool BoxCast(Vector3 origin, Vector3 halfExtents, Quaternion rotation, Vector3 direction, float maxDistance, out ShapeCastHit hit, uint layerMask = 4294967295, bool includeTriggers = false)` | The first entity a box of `halfExtents`, turned by `rotation`, touches as it moves from `origin` (its center) along `direction` for at most `maxDistance` without turning (see `Physics.SphereCast`). The box keeps its sharp edges and corners. |
| `static bool CapsuleCast(Vector3 origin, float radius, float halfHeight, Quaternion rotation, Vector3 direction, float maxDistance, out ShapeCastHit hit, uint layerMask = 4294967295, bool includeTriggers = false)` | The first entity a capsule touches as it moves from `origin` (its center) along `direction` for at most `maxDistance` without turning (see `Physics.SphereCast`). The capsule lies along its local Y axis, turned by `rotation`, as a capsule collider: caps of `radius` at either end of a cylinder whose half length is `halfHeight`. |
| `static int OverlapSphere(Vector3 center, float radius, Span<Entity> entities, uint layerMask = 4294967295, bool includeTriggers = false)` | Every entity with a collider that a sphere of `radius` at `center` overlaps, by entity ID from the smallest, written into `entities`; returns how many were written. A buffer smaller than the overlaps keeps the smallest IDs. A triangle counts from either side; a shape wholly inside a closed triangle mesh does not meet its surface and does not overlap it. Allocates no managed memory once the entities it finds have been met. Graphs have no node of it: they hold arrays as values, which a node may not write into. |
| `static int OverlapBox(Vector3 center, Vector3 halfExtents, Quaternion rotation, Span<Entity> entities, uint layerMask = 4294967295, bool includeTriggers = false)` | Every entity with a collider that a box of `halfExtents` at `center`, turned by `rotation`, overlaps (see `Physics.OverlapSphere`). Graphs have no node of it. |
| `static int OverlapCapsule(Vector3 center, float radius, float halfHeight, Quaternion rotation, Span<Entity> entities, uint layerMask = 4294967295, bool includeTriggers = false)` | Every entity with a collider that a capsule at `center` overlaps (see `Physics.OverlapSphere`; the capsule as in `Physics.CapsuleCast`). Graphs have no node of it. |

### PhysicsMaterial

```csharp
public sealed class PhysicsMaterial : Asset
```

A physics material asset (`.sphysmat`): how a surface slides, bounces and rolls, the name scripts tell it by, and what impacts on it sound and look like. A collider whose Material names one takes its friction, restitution and combine modes in place of its own; a raycast hit names the material of the surface it hit (`RaycastHit.Material`). Edit it in the editor; scripts read it, and an edit made while the game plays shows on the next frame. A material that cannot be loaded reads as a new one's values (logged).

| Member | Summary |
|---|---|
| `float Friction { get; }` | The friction coefficient: 0 slides freely, 1 holds about as well as rubber on concrete (a collider's own default is 0.6). A contact combines it with the other side's by the modes of `PhysicsMaterial.FrictionCombine`. |
| `float Restitution { get; }` | Bounciness from 0 to 1: 0 does not bounce, 1 keeps all of the speed along the contact's normal. A contact combines it with the other side's by the modes of `PhysicsMaterial.RestitutionCombine`. |
| `PhysicsMaterialCombine FrictionCombine { get; }` | How a contact combines the frictions of its two sides (a collider without a material says `PhysicsMaterialCombine.GeometricMean`); when the sides differ, the mode later in `PhysicsMaterialCombine` wins. |
| `PhysicsMaterialCombine RestitutionCombine { get; }` | How a contact combines the restitutions of its two sides (a collider without a material says `PhysicsMaterialCombine.Maximum`); when the sides differ, the mode later in `PhysicsMaterialCombine` wins. |
| `float RollingResistance { get; }` | The force opposing a wheel that rolls on the surface, per newton of load (about 0.01 for a tyre on asphalt), for scripts and graphs to apply; rigid bodies roll without it. |
| `string Surface { get; }` | The surface's name (such as Asphalt, Gravel or Ice), which scripts and graphs compare to tell surfaces apart; empty for none. |
| `AudioClip? ImpactSound { get; }` | A sound for impacts on the surface, or null. A collision that begins plays it where it begins when the project's mixer names a bus for impact sounds (its Impacts), louder for a stronger impact; scripts can play it too. |
| `Prefab? ImpactParticles { get; }` | A prefab (usually a particle system) for impacts on the surface, or null; contacts spawn nothing by themselves. |

### PhysicsMaterialCombine

```csharp
public enum PhysicsMaterialCombine
```

How the two sides of a contact combine a value of their physics materials (friction or restitution). When the two sides name different modes, the one later in this list wins.

Values: `GeometricMean`, `Average`, `Minimum`, `Multiply`, `Maximum`.

### PointLightComponent

```csharp
public sealed class PointLightComponent : Component
```

Light shining from the entity's position in every direction.

| Member | Summary |
|---|---|
| `Color Color { get; set; }` | The light's color (the alpha is ignored). |
| `float Intensity { get; set; }` | Brightness multiplier. |
| `float Range { get; set; }` | Distance at which the light's contribution reaches zero. |
| `bool CastShadows { get; set; }` | Whether the light casts shadows. |

### PostProcessComponent

```csharp
public sealed class PostProcessComponent : Component
```

A post-process volume: changes the scene's post-processing (exposure, tonemapper, bloom, ambient occlusion, color grading and lens effects) where the camera is. Each setting applies only while its `Override` property is set. `PostProcessComponent.Global` volumes apply everywhere; local volumes apply inside their box (the unit cube placed, rotated and scaled by the entity, so the entity's scale is the box's size in meters) and fade out over `PostProcessComponent.BlendDistance` around it. Volumes apply in order of `PostProcessComponent.Priority`, each scaled by its `PostProcessComponent.Weight`: numbers move towards the volume's values, `PostProcessComponent.Bloom` fades in or out through the bloom's intensity, and `PostProcessComponent.AmbientOcclusion` and `PostProcessComponent.Tonemapper` take the volume's value once its blend reaches one half. Animate `PostProcessComponent.Weight` for effects such as a damage flash or an underwater tint.

| Member | Summary |
|---|---|
| `bool Global { get; set; }` | Whether the volume applies everywhere instead of only inside its box. |
| `float BlendDistance { get; set; }` | Local volumes: meters around the box over which the volume fades out. |
| `float Priority { get; set; }` | Volumes with higher priorities apply over those with lower ones. |
| `float Weight { get; set; }` | How much of the volume applies, from 0 (nothing) to 1 (all of it). |
| `bool OverrideEV100 { get; set; }` | Whether the volume's `PostProcessComponent.EV100` applies. |
| `float EV100 { get; set; }` | Exposure value at ISO 100 (-20 to 20); higher values darken the image. |
| `bool OverrideTonemapper { get; set; }` | Whether the volume's `PostProcessComponent.Tonemapper` applies. |
| `TonemapOperator Tonemapper { get; set; }` | How HDR colors map to the display. |
| `bool OverrideAmbientOcclusion { get; set; }` | Whether the volume switches ambient occlusion as `PostProcessComponent.AmbientOcclusion` says. |
| `bool AmbientOcclusion { get; set; }` | Whether ambient occlusion darkens indirect light in creases and corners. |
| `bool OverrideAmbientOcclusionIntensity { get; set; }` | Whether the volume's `PostProcessComponent.AmbientOcclusionIntensity` applies. |
| `float AmbientOcclusionIntensity { get; set; }` | Contrast exponent of ambient occlusion (0.1 to 8). |
| `bool OverrideBloom { get; set; }` | Whether the volume fades bloom in or out as `PostProcessComponent.Bloom` says. |
| `bool Bloom { get; set; }` | Whether bright areas glow. |
| `bool OverrideBloomIntensity { get; set; }` | Whether the volume's `PostProcessComponent.BloomIntensity` applies. |
| `float BloomIntensity { get; set; }` | Fraction of the blurred image mixed into the scene (0 to 1). |
| `bool OverrideContrast { get; set; }` | Whether the volume's `PostProcessComponent.Contrast` applies. |
| `float Contrast { get; set; }` | Contrast around middle gray (0 to 2); 1 leaves colors unchanged. |
| `bool OverrideSaturation { get; set; }` | Whether the volume's `PostProcessComponent.Saturation` applies. |
| `float Saturation { get; set; }` | Color intensity: 0 is grayscale, 1 unchanged, 2 doubles it. |
| `bool OverrideTemperature { get; set; }` | Whether the volume's `PostProcessComponent.Temperature` applies. |
| `float Temperature { get; set; }` | White balance from cool (-1, blue) to warm (1, orange). |
| `bool OverrideTint { get; set; }` | Whether the volume's `PostProcessComponent.Tint` applies. |
| `float Tint { get; set; }` | White balance from green (-1) to magenta (1). |
| `bool OverrideColorFilter { get; set; }` | Whether the volume's `PostProcessComponent.ColorFilter` applies. |
| `Color ColorFilter { get; set; }` | A linear color (components 0 to 2) every color is multiplied by; the alpha is ignored. |
| `bool OverrideChromaticAberration { get; set; }` | Whether the volume's `PostProcessComponent.ChromaticAberration` applies. |
| `float ChromaticAberration { get; set; }` | Lens color fringes that grow towards the edges of the image (0 to 1). |
| `bool OverrideVignette { get; set; }` | Whether the volume's `PostProcessComponent.Vignette` applies. |
| `float Vignette { get; set; }` | How much the corners of the image darken (0 to 1). |
| `bool OverrideVignetteSmoothness { get; set; }` | Whether the volume's `PostProcessComponent.VignetteSmoothness` applies. |
| `float VignetteSmoothness { get; set; }` | How gradually the vignette fades in from the center (0.01 to 1). |
| `bool OverrideFilmGrain { get; set; }` | Whether the volume's `PostProcessComponent.FilmGrain` applies. |
| `float FilmGrain { get; set; }` | Strength of the animated film grain (0 to 1). |

### Prefab

```csharp
public sealed class Prefab : Asset
```

A prefab asset: entities saved for instantiation (see `Entity.Instantiate`).


### ProjectionType

```csharp
public enum ProjectionType
```

How a camera projects the scene.

Values: `Perspective`, `Orthographic`.

### QualityPreset

```csharp
public enum QualityPreset
```

The graphics quality of the game's views: presets made of the scene's own settings, each only lowering them.

Values: `Low`, `Medium`, `High`.

### Quaternion

```csharp
public struct Quaternion : IEquatable<Quaternion>
```

A rotation stored as a unit quaternion (X, Y, Z, W), laid out like the engine's quaternions.

| Member | Summary |
|---|---|
| `Quaternion(float x, float y, float z, float w)` | Creates a quaternion from its components. |
| `float X` | The X component of the vector part. |
| `float Y` | The Y component of the vector part. |
| `float Z` | The Z component of the vector part. |
| `float W` | The scalar part. |
| `static Quaternion Identity { get; }` | The rotation that changes nothing. |
| `float LengthSquared { get; }` | The squared length; 1 for rotations. |
| `Quaternion Normalized { get; }` | This quaternion scaled to length 1, or the identity when its length is zero. |
| `Quaternion Inverse { get; }` | The opposite rotation (the conjugate scaled by the inverse squared length). |
| `static Quaternion AngleAxis(float degrees, Vector3 axis)` | A rotation of `degrees` around `axis` (counterclockwise looking down the axis towards the origin). A zero axis gives the identity. |
| `static float Dot(Quaternion a, Quaternion b)` | The four-dimensional dot product of two quaternions. |
| `static Quaternion Slerp(Quaternion a, Quaternion b, float t)` | Spherical interpolation along the shortest arc from `a` (t = 0) to `b` (t = 1); t is clamped to [0, 1]. |
| `static Quaternion operator *(Quaternion a, Quaternion b)` | The rotation `b` followed by `a`. |
| `static Vector3 operator *(Quaternion rotation, Vector3 vector)` | Rotates a vector. |
| `static bool operator ==(Quaternion a, Quaternion b)` | Exact component-wise equality. |
| `static bool operator !=(Quaternion a, Quaternion b)` | Exact component-wise inequality. |
| `bool Equals(Quaternion other)` | Exact component-wise equality (a rotation and its negation compare unequal). |

### Random

```csharp
public static class Random
```

Random numbers for games. Seeding makes the sequence repeat (main thread use).

| Member | Summary |
|---|---|
| `static float Value { get; }` | A number in [0, 1). |
| `static Vector3 InsideUnitSphere { get; }` | A point inside the sphere of radius 1, evenly distributed. |
| `static Vector3 OnUnitSphere { get; }` | A point on the sphere of radius 1, evenly distributed. |
| `static Quaternion Rotation { get; }` | A rotation, evenly distributed over all orientations. |
| `static void Seed(int seed)` | Restarts the sequence from a seed, so the same calls give the same numbers. |
| `static float Range(float min, float max)` | A number in [min, max). |
| `static int Range(int min, int max)` | A whole number in [min, max); `min` when the range is empty. |

### RangeAttribute

```csharp
public sealed class RangeAttribute : Attribute
```

Limits the value the inspector lets a numeric field take.

| Member | Summary |
|---|---|
| `RangeAttribute(float min, float max)` | Limits the field to [`min`, `max`]. |
| `float Min { get; }` | The smallest value. |
| `float Max { get; }` | The largest value. |

### Ray

```csharp
public struct Ray
```

A half-line from an origin along a direction.

| Member | Summary |
|---|---|
| `Ray(Vector3 origin, Vector3 direction)` | Creates a ray. |
| `Vector3 Origin` | Where the ray starts. |
| `Vector3 Direction` | The direction it goes (unit length for rays from the engine). |
| `Vector3 GetPoint(float distance)` | The point `distance` along the ray (in units of the direction's length). |

### RaycastHit

```csharp
public struct RaycastHit
```

What a raycast hit.

| Member | Summary |
|---|---|
| `Entity Entity { get; }` | The entity of the collider hit (its script instance when it has one). |
| `Vector3 Point { get; }` | The world-space point hit. |
| `Vector3 Normal { get; }` | The surface normal at the point. |
| `float Distance { get; }` | Distance from the ray's origin to the point. |
| `PhysicsMaterial? Material { get; }` | The physics material of the surface hit: the collider's Material, or for a triangle of a mesh collider its submesh's entry in the collider's Materials (the collider's Material when that entry is empty); null when the surface has none and its collider's own Friction and Restitution apply. Hits on one material share one reference. |

### RebindResult

```csharp
public sealed class RebindResult
```

The answer of a rebind of an input action's binding: how it ended, the binding's controls and the other actions that now read one of them. The engine applies a binding whatever it conflicts with and reports the conflicts; the game decides what to do about them (warn, swap, or undo with another `Input.Rebind`).

| Member | Summary |
|---|---|
| `RebindStatus Status { get; }` | How the rebind ended, or `RebindStatus.Listening` while a capture waits. |
| `string Action { get; }` | The action asked for (empty for a reset). |
| `int Binding { get; }` | The binding's index in its action (-1 for a reset). |
| `int Part { get; }` | The control of a composite binding that was asked for (0 to 3), or -1 for the whole binding. |
| `string Controls { get; }` | The binding's controls as text ("Key/J", "Key/A Key/D", "Gamepad/LeftStick"): the new ones when `RebindStatus.Done`, those it reads while a capture listens or after it was cancelled; empty when refused. |
| `string[] Conflicts { get; }` | When `RebindStatus.Done`, the other actions with a binding that reads one of the binding's controls (on a gamepad both read), in the order of the actions in the asset; empty otherwise. |
| `string Message { get; }` | Why the rebind was refused or cancelled, or that a binding that was done could not be saved; empty otherwise. |

### RebindStatus

```csharp
public enum RebindStatus
```

How a rebind of an input action's binding ended (`Input.Rebind`, `Input.ResetBindings`), or that a capture listens for a control (`Input.StartRebind`).

Values: `None`, `Listening`, `Done`, `Cancelled`, `Refused`.

### ReverbZoneComponent

```csharp
public sealed class ReverbZoneComponent : Component
```

A reverb zone: a sphere or a box round the entity inside which spatial sounds send part of themselves to a bus of the project's mixer that carries a reverb (an echo under a bridge, a tunnel, a hall). It follows the sound, not the listener: a sound inside the zone echoes for a listener outside it. The shape is placed and turned by the entity and sized in meters times its scale (a sphere by its largest axis). A sound sends `ReverbZoneComponent.SendLevel` of itself inside the shape, less and less across `ReverbZoneComponent.FadeDistance` outside it; of several zones of one bus the largest send counts, and a sound sends to the two buses it sends most to. Sounds that are not spatial send nothing. A change applies from the next frame.

| Member | Summary |
|---|---|
| `ReverbZoneShape Shape { get; set; }` | A sphere of `ReverbZoneComponent.Radius` or a box of `ReverbZoneComponent.HalfExtents`. |
| `float Radius { get; set; }` | The sphere's radius in meters (times the entity's largest scale), from 0 to 100,000. |
| `Vector3 HalfExtents { get; set; }` | Meters from the box's center to each face (times the entity's scale), each from 0 to 100,000. |
| `float FadeDistance { get; set; }` | Meters outside the shape across which the send falls to nothing, from 0 (it stops at the surface) to 100,000. |
| `string Bus { get; set; }` | The bus of the project's mixer the zone sends to, by name in any letter case: one that carries a reverb. Empty, or a name the mixer lacks (logged once), sends nothing. |
| `float SendLevel { get; set; }` | How much of a spatial sound inside the zone reaches the bus, from 0 to 1 (linear). |

### ReverbZoneShape

```csharp
public enum ReverbZoneShape
```

The shape of a reverb zone.

Values: `Sphere`, `Box`.

### RigidBodyComponent

```csharp
public sealed class RigidBodyComponent : Component
```

Makes the entity a physics body; its colliders give it shape. While the scene runs, velocities, forces and sleep act on the simulated body.

| Member | Summary |
|---|---|
| `RigidBodyType Type { get; set; }` | Static, dynamic or kinematic. Changing it rebuilds the body. |
| `float Mass { get; set; }` | Mass in kilograms. Changing it rebuilds the body. |
| `uint Layer { get; set; }` | Physics layer index (0-15) of the project settings. Changing it rebuilds the body. |
| `float GravityFactor { get; set; }` | Multiplier of the scene's gravity for this body. |
| `float LinearDamping { get; set; }` | Fraction of linear velocity lost per second. |
| `float AngularDamping { get; set; }` | Fraction of angular velocity lost per second. |
| `RigidBodyInterpolation Interpolation { get; set; }` | How a dynamic or kinematic body is drawn between physics steps: between its last two steps (smooth when frames come faster than steps, a step behind) or where its last step left it. Changing it does not rebuild the body. |
| `Vector3 LinearVelocity { get; set; }` | Velocity in meters per second (the initial velocity while the scene is not simulated); the simulation allows up to 500. |
| `Vector3 AngularVelocity { get; set; }` | Angular velocity in radians per second (the initial one while the scene is not simulated); the simulation allows up to about 47 (7.5 turns per second). |
| `bool IsSleeping { get; }` | Whether the simulated body is asleep (at rest and not simulated until something wakes it). |
| `void AddForce(Vector3 force, ForceMode mode = ForceMode.Force)` | Applies a force (or impulse, acceleration or velocity change, see `ForceMode`) at the center of mass. Dynamic bodies only; wakes the body. |
| `void AddTorque(Vector3 torque, ForceMode mode = ForceMode.Force)` | Applies a torque (or angular impulse, acceleration or velocity change). Dynamic bodies only; wakes the body. |
| `void MoveKinematic(Vector3 position, Quaternion rotation)` | Moves a kinematic body to a world position and rotation over the next physics step, pushing dynamic bodies on the way. |
| `void Teleport(Vector3 position, Quaternion rotation)` | Places the body at a world position and rotation at once, keeping its velocity. |
| `void WakeUp()` | Wakes the simulated body up. |

### RigidBodyInterpolation

```csharp
public enum RigidBodyInterpolation
```

How `RigidBodyComponent.Interpolation` draws a moving body between physics steps.

Values: `None`, `Interpolate`.

### RigidBodyType

```csharp
public enum RigidBodyType
```

How a rigid body moves.

Values: `Static`, `Dynamic`, `Kinematic`.

### SaveSlot

```csharp
public sealed class SaveSlot
```

A save slot: named values the game keeps for its player between runs (a best lap, its splits, progress), in a file of the game's own data folder that this class never names. Open a slot by its name, read values with `SaveSlot.Get`, change them with `SaveSlot.Set` and `SaveSlot.Remove`, and write them with `SaveSlot.Save`, which returns once the slot is on disk: the frame that saves waits for the device, so save at a results screen, a menu or between races, never while the player plays, and keep what a ride learns in memory until then. A slot holds numbers, text, vectors, quaternions, colors, entities, prefabs, assets, enums (as their integers) and arrays or lists of any of them of up to 4,096 elements, at most 4,096 values, and is written whole, at most 16 MiB. The object holds the values: reading and changing them touches no file, and two objects opened with one name are separate copies, the last to save winning. Main thread.

| Member | Summary |
|---|---|
| `string Name { get; }` | The slot's name as the game's folder spells it, in lower case: `Best` and `best` are one slot. |
| `SaveSlotStatus Status { get; }` | What the slot read as when it was opened, or what its last save or delete left it. |
| `static SaveSlot Open(string name)` | Reads a slot. A slot without a file is empty (`SaveSlotStatus.Missing`); a damaged slot, one of a newer version and any slot while no game runs are empty too, and logged. It reads the disk where the flow passes, so open a slot once and keep the object. |
| `static string[] GetNames()` | The names of the slots the game has saved, in lower case and sorted; empty while no game runs. It reads the disk where the flow passes. |
| `bool Has(string name)` | Whether the slot holds a value of the name, of any type. |
| `T? Get<T>(string name, T? fallback)` | The value of the name, when the slot holds one of `T`'s type (an array and a list of a type are one type, an enum is its integers); otherwise the fallback, and a value of another type is warned about once per name. |
| `void Set<T>(string name, T? value)` | Sets a value of the slot, kept by this object until `SaveSlot.Save`. An array or a list is copied. |
| `bool Remove(string name)` | Removes a value of the slot; `SaveSlot.Save` writes the slot without it. |
| `bool Save()` | Writes every value of the slot to its file and returns once the file is on disk, whole: the old file stays whole until then, whatever happens. False, logged, while no game runs, for a slot of a newer version, for a file on disk that cannot be read at all, and when the slot cannot be written (larger than 16 MiB, a full disk); `SaveSlot.Status` is `SaveSlotStatus.Present` after one that succeeds. |
| `bool Delete()` | Removes the slot's file and every value of this object; a slot without a file is deleted already. False, logged, while no game runs, for a slot of a newer version and for a file on disk that cannot be read at all, which keep their file and values. |

### SaveSlotStatus

```csharp
public enum SaveSlotStatus
```

What a save slot read as when it was opened, or what its last `SaveSlot.Save` or `SaveSlot.Delete` left it.

Values: `Missing`, `Present`, `Damaged`, `Newer`, `Unavailable`.

### SceneManager

```csharp
public static class SceneManager
```

The scenes of the game.

| Member | Summary |
|---|---|
| `static string CurrentSceneName { get; }` | The name of the running scene. |
| `static bool LoadScene(string path)` | Replaces the running scene with the scene at a path of the project's asset directory ("Scenes/Level2.sscene") after this frame: every script of the current scene gets OnDestroy, then the new scene starts. Returns false (logged) when there is no such scene. |

### Script

```csharp
public abstract class Script : Entity
```

Base class of entity behaviours. A Script component names a class deriving from Script; when the scene runs, the engine creates one instance per such entity, sets its serialized fields (public fields and fields marked `SerializeFieldAttribute`) and calls the overridden methods. Exceptions thrown by them are logged and do not stop the scene.

| Member | Summary |
|---|---|
| `protected Script()` |  |
| `protected virtual void OnCreate()` | Called once when the scene starts (or the instance is created while it runs), after every script of the scene was created and before the first update. |
| `protected virtual void OnUpdate(float deltaTime)` | Called every frame with the seconds since the previous frame. |
| `protected virtual void OnFixedUpdate(float fixedDeltaTime)` | Called before every physics step with the seconds per step. |
| `protected virtual void OnDestroy()` | Called when the entity is destroyed or the scene stops. |
| `protected virtual void OnCollisionEnter(Entity other)` | Called when one of the entity's colliders starts touching a collider of `other`. |
| `protected virtual void OnCollisionExit(Entity other)` | Called when the entity's colliders stop touching the colliders of `other`. |
| `protected virtual void OnTriggerEnter(Entity other)` | Called when a trigger collider of either entity starts overlapping a collider of the other. |
| `protected virtual void OnTriggerExit(Entity other)` | Called when the overlap between a trigger collider and `other` ends. |
| `protected virtual void OnClick(Entity button)` | Called when a `UIButtonComponent` is clicked, before the frame's `Script.OnUpdate`: on the script of the button's entity, then on the scripts of its ancestors, nearest first (so a menu's script hears the clicks of every button in it). `button` is the clicked button's entity. |
| `protected virtual void OnActionPressed(string action)` | Called when an input action of the project's `InputActions` goes down (its value reaches its press threshold), once per press, before the frame's `Script.OnUpdate` and its clicks: on every script, then on every graph's `Event.ActionPressed` of that action. `action` is the action's name; compare it with the names of the asset (they match with their letter case). A press and release within one frame calls this, then `Script.OnActionReleased`. |
| `protected virtual void OnActionReleased(string action)` | Called when an input action of the project's `InputActions` goes up again (its value falls below its release threshold), once per release, before the frame's `Script.OnUpdate`: on every script, then on every graph's `Event.ActionReleased` of that action. `action` is the action's name. |

### ScriptComponent

```csharp
public sealed class ScriptComponent : Component
```

Runs a script class on the entity.

| Member | Summary |
|---|---|
| `string ClassName { get; set; }` | The full name of the script class, e.g. Game.PlayerController. Changing it replaces the instance. |
| `Script? Instance { get; }` | The entity's script instance, or null when it has none (unknown class, or the scene is not running). |

### SerializeFieldAttribute

```csharp
public sealed class SerializeFieldAttribute : Attribute
```

Exposes a private or protected field of a script to the editor and to scene files, like a public field.

| Member | Summary |
|---|---|
| `SerializeFieldAttribute()` |  |

### Settings

```csharp
public static class Settings
```

The player's settings of the game: the window, the graphics quality, the master volume, each bus's volume and mute, and named values the game owns (its assists, its options), kept for the player in the game's own data folder, which this class never names. The game starts with them as the player last saved them (or the game's own window settings and High quality); every setter applies at once (the window once the frame's update has returned; the player's command line wins over the window, and the editor's play mode keeps the window without applying it) and reaches the file only at `Settings.Save`. The volumes are the player's, a stage of the mix after `Audio`'s, so a settings screen and the game's own mixing never overwrite each other. Main thread.

| Member | Summary |
|---|---|
| `static QualityPreset Quality { get; set; }` | The graphics quality of the game's views, applied from the next frame. |
| `static WindowMode WindowMode { get; set; }` | Whether the game's window is a window or covers its monitor. |
| `static int WindowWidth { get; }` | The width of the game's window when it is a window, in screen coordinates. |
| `static int WindowHeight { get; }` | The height of the game's window when it is a window, in screen coordinates. |
| `static bool VSync { get; set; }` | Whether frames are shown in step with the display's refresh. |
| `static float MasterVolume { get; set; }` | The player's master volume, from 0 (silence) to 1 (the mix as the game makes it); values beyond are brought within. It applies at once, without a click. |
| `static void SetWindowSize(int width, int height)` | Sets the size of the game's window when it is a window, in screen coordinates (64 to 16,384; values beyond are brought within). |
| `static float GetBusVolume(string bus)` | The player's volume of a bus of the game's mixer, in decibels (0 leaves the bus as the game sets it). |
| `static void SetBusVolume(string bus, float decibels)` | Sets the player's volume of a bus, in decibels: added to the game's own, from -80 (silence) to +24 (values beyond are brought within). It applies at once, without a click; a bus the game's mixer lacks is kept by name. |
| `static bool IsBusMuted(string bus)` | Whether the player muted a bus of the game's mixer. |
| `static void SetBusMuted(string bus, bool muted)` | Mutes or unmutes a bus for the player; it applies at once. |
| `static bool Has(string name)` | Whether the game keeps a value of the name in the settings, of any type. |
| `static T? Get<T>(string name, T? fallback)` | The game's value of the name, when the settings hold one of `T`'s type (as `SaveSlot.Get` says); otherwise the fallback, and a value of another type is warned about once per name. |
| `static void Set<T>(string name, T? value)` | Sets a value of the game's (an assist, an option), with a save slot's rules for names and values (as `SaveSlot.Set` says). Keep many values in a slot instead: each changed value measures the settings' file. |
| `static bool Remove(string name)` | Removes a value of the game's; `Settings.Save` writes the settings without it. |
| `static bool Save()` | Writes the settings to the game's data folder and returns once the file is on disk, whole (the old file stays whole until then). False, logged, while no game runs and when the file cannot be written. |

### ShapeCastHit

```csharp
public struct ShapeCastHit
```

What a sphere, box or capsule cast touched first (`Physics.SphereCast`, `Physics.BoxCast`, `Physics.CapsuleCast`).

| Member | Summary |
|---|---|
| `Entity? Entity` | The entity of the collider touched (its script instance when it has one); null in a hit made by hand that names none. |
| `Vector3 Point` | The world-space point where the shape touches the collider, on the collider's surface; for a collider the shape overlapped where the cast started, the deepest point of the overlap. |
| `Vector3 Normal` | Unit length, out of the collider towards the shape: the way that pushes the shape off it. On a face it is the face's normal; on an edge or a corner it points from there towards the shape. |
| `float Distance` | How far the shape travelled along the cast's direction until it touched, in meters: its center is then at the origin plus the unit direction times this. 0 for a collider the shape overlapped where the cast started. |
| `PhysicsMaterial? Material` | The physics material of the surface touched, as `RaycastHit.Material`: null when the surface has none and its collider's own Friction and Restitution apply. |

### SkyLightComponent

```csharp
public sealed class SkyLightComponent : Component
```

The sky and image-based lighting from an HDR environment map.

| Member | Summary |
|---|---|
| `EnvironmentMap? Environment { get; set; }` | The environment map, or null for the ambient color. |
| `float Intensity { get; set; }` | Brightness multiplier. |
| `float Rotation { get; set; }` | Rotation of the environment around the world Y axis, in degrees. |
| `float SkyboxBlur { get; set; }` | 0 draws a sharp sky, 1 a fully blurred one. |
| `bool DrawSkybox { get; set; }` | Whether the environment is drawn as the background. |
| `Color AmbientColor { get; set; }` | Ambient light without an environment map (the alpha is ignored). |

### SphereColliderComponent

```csharp
public sealed class SphereColliderComponent : Component
```

A sphere collision shape of the entity's body.

| Member | Summary |
|---|---|
| `float Radius { get; set; }` | The sphere's radius. |
| `Vector3 Offset { get; set; }` | Position of the sphere's center relative to the entity. |
| `bool IsTrigger { get; set; }` | Whether the collider only reports overlaps instead of colliding. |
| `float Friction { get; set; }` | Friction coefficient, used while the collider's Material (set in the editor) names no physics material: two colliders without materials combine theirs as the square root of the product. |
| `float Restitution { get; set; }` | Bounciness from 0 to 1, used while the collider's Material (set in the editor) names no physics material: of two colliders without materials, the larger applies. |

### SpotLightComponent

```csharp
public sealed class SpotLightComponent : Component
```

Light shining from the entity's position in a cone along its -Z axis.

| Member | Summary |
|---|---|
| `Color Color { get; set; }` | The light's color (the alpha is ignored). |
| `float Intensity { get; set; }` | Brightness multiplier. |
| `float Range { get; set; }` | Distance at which the light's contribution reaches zero. |
| `float InnerConeAngle { get; set; }` | Half-angle of the full-intensity cone, in degrees. |
| `float OuterConeAngle { get; set; }` | Half-angle of the cone the light fades out at, in degrees. |
| `bool CastShadows { get; set; }` | Whether the light casts shadows. |

### SpriteRendererComponent

```csharp
public sealed class SpriteRendererComponent : Component
```

Renders an unlit textured or colored quad, in the world or on the screen.

| Member | Summary |
|---|---|
| `Color Color { get; set; }` | The quad's color (multiplies the texture). |
| `Texture? Texture { get; set; }` | The texture, or null for a plain color. |
| `float Tiling { get; set; }` | How many times the texture repeats across the quad. |
| `bool ScreenSpace { get; set; }` | Whether the quad is placed on the screen (translation in normalized viewport coordinates). |

### Telemetry

```csharp
public static class Telemetry
```

The game's telemetry: named channels of one number per fixed step, and marks on steps, which the editor records while its Telemetry panel is open or an agent's recording runs (the telemetry tools) and plots, reads and exports; the game player records nothing. A channel's name and a mark's label are 1 to 64 bytes of UTF-8 without control characters; a longer one is refused, never cut. A value recorded during a fixed step (`Script.OnFixedUpdate`) is that step's, and the last of several wins; one recorded between steps (`Script.OnUpdate`, an event) is the next step's; a step that records nothing for a channel has no value there. Laps run from a mark `Lap n` to the next mark `Lap n+1` on a later step; `Lap end` ends a lap in progress without making one. What the recorder refuses (a name too long, a value that is not finite, too many channels) is dropped, counted and warned about once per recording session. Any thread may call these, and while nothing records each returns after one native call.

| Member | Summary |
|---|---|
| `static bool IsRecording { get; }` | Whether a recording runs, so that values costly to compute are computed only when they are kept. |
| `static void Record(string channel, float value)` | Records a channel's value for the current fixed step: in `Script.OnFixedUpdate` that step, between steps the next one. Nothing happens while no recording runs. |
| `static void Mark(string label)` | Marks the current fixed step with a label (a lap boundary such as `Lap 2`, a sector, an event), placed as `Telemetry.Record` places a value. A step keeps every mark it gets, in order. Nothing happens while no recording runs. |
| `static void MarkStep(string label, ulong step)` | Marks a given fixed step, counted from 1 as `Collision.Step` counts it: a contact handler, which runs after the frame's steps, marks the step its contact happened in, whatever the frame rate. A step before the recording's session began, one it never reaches and step 0 drop the mark, counted. Nothing happens while no recording runs. |

### TextAlignment

```csharp
public enum TextAlignment
```

How text lines align to the entity's position.

Values: `Left`, `Center`, `Right`.

### TextComponent

```csharp
public sealed class TextComponent : Component
```

Renders text in the world (in the entity's XY plane) or on the screen.

| Member | Summary |
|---|---|
| `string Text { get; set; }` | The text; newlines start new lines. |
| `Font? Font { get; set; }` | The font, or null for the default font. |
| `Color Color { get; set; }` | The text's color. |
| `float FontSize { get; set; }` | World units per line, or pixels for screen-space text. |
| `bool ScreenSpace { get; set; }` | Whether the text is placed on the screen (translation in normalized viewport coordinates). |
| `TextAlignment Alignment { get; set; }` | How lines align to the entity's position. |
| `float LineSpacing { get; set; }` | Distance between lines as a multiple of the font's line height. |

### TextVerticalAlignment

```csharp
public enum TextVerticalAlignment
```

Where a `UITextComponent`'s text block sits in its rectangle.

Values: `Top`, `Middle`, `Bottom`.

### Texture

```csharp
public sealed class Texture : Asset
```

A texture asset.


### Time

```csharp
public static class Time
```

Timing of the running scene. All times are scaled by `Time.TimeScale`.

| Member | Summary |
|---|---|
| `static float DeltaTime { get; }` | Seconds since the previous frame; the value passed to `Script.OnUpdate`. |
| `static float FixedDeltaTime { get; }` | Seconds per physics step; the value passed to `Script.OnFixedUpdate`. |
| `static double Elapsed { get; }` | Seconds since the scene started running. |
| `static ulong FrameCount { get; }` | Frames since the scene started running. |
| `static float TimeScale { get; set; }` | How fast time passes for scripts and physics (1 is real time, 0 freezes the simulation; sounds are not affected). |

### TimelineDirection

```csharp
public enum TimelineDirection
```

The direction a visual script's Timeline node plays in: its Direction output.

Values: `Forward`, `Backward`.

### TonemapOperator

```csharp
public enum TonemapOperator
```

How the renderer maps the scene's HDR colors to the display (a `PostProcessComponent` setting).

Values: `ACES`, `AgX`, `PBRNeutral`, `Reinhard`, `None`.

### TooltipAttribute

```csharp
public sealed class TooltipAttribute : Attribute
```

Explains a field in the inspector.

| Member | Summary |
|---|---|
| `TooltipAttribute(string text)` | Shows `text` when the field is hovered. |
| `string Text { get; }` | The explanation. |

### TransformComponent

```csharp
public sealed class TransformComponent : Component
```

Position, rotation and scale of an entity relative to its parent (or the world at the root).

| Member | Summary |
|---|---|
| `Vector3 Translation { get; set; }` | The position relative to the parent. |
| `Quaternion Rotation { get; set; }` | The rotation relative to the parent. |
| `Vector3 EulerAngles { get; set; }` | The rotation relative to the parent as Euler angles in degrees (X, then Y, then Z), as the editor shows them. Reading gives one of several sets of angles for the rotation, with Y within ±90 degrees, so adding to the angles read does not turn an entity all the way around: multiply `TransformComponent.Rotation` by a turn instead, or keep angles of your own and set them. |
| `Vector3 Scale { get; set; }` | The scale relative to the parent. |
| `Matrix4 WorldTransform { get; }` | The transform from the entity's space to world space. |
| `Vector3 WorldTranslation { get; set; }` | The position in world space; setting it keeps the world rotation and scale. |
| `Vector3 Forward { get; }` | The entity's forward direction (-Z) in world space. |
| `Vector3 Right { get; }` | The entity's right direction (+X) in world space. |
| `Vector3 Up { get; }` | The entity's up direction (+Y) in world space. |

### UIButtonComponent

```csharp
public sealed class UIButtonComponent : Component
```

Makes a UI element a button: while the scene runs the mouse hovers, presses and clicks it (the left button pressed and released over it). A click sets `UIButtonComponent.WasClicked` for the frame and calls `OnClick` on the scripts of the button and of its ancestors. The button's colors replace its `UIImageComponent`'s color in each state.

| Member | Summary |
|---|---|
| `bool Interactable { get; set; }` | Whether the button can be clicked; a disabled button shows `UIButtonComponent.DisabledColor` and still covers the UI below it. |
| `Color NormalColor { get; set; }` | The image's color at rest. |
| `Color HoveredColor { get; set; }` | The image's color under the mouse. |
| `Color PressedColor { get; set; }` | The image's color while the mouse holds the button down. |
| `Color DisabledColor { get; set; }` | The image's color while the button is not `UIButtonComponent.Interactable`. |
| `bool WasClicked { get; }` | Whether the button was clicked this frame (true from the frame's first callback to the next frame's). |
| `UIButtonState State { get; }` | The state the button is drawn in. |
| `void Click()` | Clicks the button as the mouse would (for keyboard and gamepad menus): at the start of the next frame, `UIButtonComponent.WasClicked` is set and the scripts get `OnClick`, unless the button is not interactable then. |

### UIButtonState

```csharp
public enum UIButtonState
```

The state a `UIButtonComponent` is drawn in.

Values: `Normal`, `Hovered`, `Pressed`, `Disabled`.

### UICanvasComponent

```csharp
public sealed class UICanvasComponent : Component
```

The root of a screen-space UI (HUDs, menus). The canvas covers the screen; its descendants are the UI, laid out by their `UIRectComponent` and drawn over the scene in hierarchy order (children over their parents, later siblings over earlier ones).

| Member | Summary |
|---|---|
| `UIScaleMode ScaleMode { get; set; }` | How canvas units relate to the screen's pixels. |
| `Vector2 ReferenceResolution { get; set; }` | The screen size the UI is designed for, in canvas units, which the `UICanvasComponent.ScaleMode` fits to the screen. |

### UIImageComponent

```csharp
public sealed class UIImageComponent : Component
```

Fills a UI element's rectangle with a color or a texture, optionally with rounded corners. Images take the clicks over the UI they cover.

| Member | Summary |
|---|---|
| `Color Color { get; set; }` | The image's color (multiplies the texture). A `UIButtonComponent` on the entity replaces it with the color of its state. |
| `Texture? Texture { get; set; }` | The texture, or null to fill the rectangle with the color. |
| `bool PreserveAspect { get; set; }` | Whether the texture keeps its aspect ratio: as large as fits, centered in the rectangle. |
| `float CornerRadius { get; set; }` | Radius of the rounded corners in canvas units (at most half the rectangle's shorter side). |

### UIRectComponent

```csharp
public sealed class UIRectComponent : Component
```

Places a UI element in its parent's rectangle (the screen for a canvas's children), in canvas units with x to the right and y downwards. On each axis the anchors mark a span of the parent (0 is its left or top edge, 1 its right or bottom edge): the rectangle is `UIRectComponent.Size` larger than the span, so equal anchors give it exactly that size, and its `UIRectComponent.Pivot` sits at the pivot's point of the span, moved by `UIRectComponent.Position`.

| Member | Summary |
|---|---|
| `Vector2 AnchorMin { get; set; }` | Where the anchors' span starts in the parent's rectangle, from (0, 0) at its top-left corner to (1, 1) at its bottom-right corner. |
| `Vector2 AnchorMax { get; set; }` | Where the anchors' span ends; equal anchors attach the rectangle to a point, different ones stretch it with the parent. |
| `Vector2 Position { get; set; }` | How far the pivot is moved from its point of the anchors' span, in canvas units (y downwards). |
| `Vector2 Size { get; set; }` | Canvas units added to the anchors' span: the size itself when the anchors are equal. |
| `Vector2 Pivot { get; set; }` | The point of the rectangle that `UIRectComponent.Position` places, from (0, 0) at its top-left corner to (1, 1) at its bottom-right corner. |
| `bool Visible { get; set; }` | Whether the element is shown; hidden elements and their descendants are neither drawn nor clicked. |
| `float Opacity { get; set; }` | Multiplies the alpha of the element and its descendants, from 0 (invisible, and not clicked) to 1. |

### UIScaleMode

```csharp
public enum UIScaleMode
```

How a `UICanvasComponent`'s units relate to the screen's pixels.

Values: `ConstantPixelSize`, `ScaleWithScreenHeight`, `ScaleWithScreenSize`.

### UITextComponent

```csharp
public sealed class UITextComponent : Component
```

Draws text in a UI element's rectangle: lines wrap at its width and the text block is aligned in it.

| Member | Summary |
|---|---|
| `string Text { get; set; }` | The text; newlines start new lines. |
| `Font? Font { get; set; }` | The font, or null for the default font. |
| `Color Color { get; set; }` | The text's color. |
| `float FontSize { get; set; }` | Line height in canvas units. |
| `TextAlignment Alignment { get; set; }` | Whether the lines start at the rectangle's left edge, are centered in it or end at its right edge. |
| `TextVerticalAlignment VerticalAlignment { get; set; }` | Whether the text block sits at the top, middle or bottom of the rectangle. |
| `bool Wrap { get; set; }` | Whether lines that do not fit the rectangle's width break at spaces. |
| `float LineSpacing { get; set; }` | Distance between lines as a multiple of the font's line height. |

### Vector2

```csharp
public struct Vector2 : IEquatable<Vector2>
```

A 2D vector of single-precision components, laid out like the engine's vectors.

| Member | Summary |
|---|---|
| `Vector2(float x, float y)` | Creates a vector from its components. |
| `Vector2(float scalar)` | Creates a vector with both components set to `scalar`. |
| `float X` | The X component. |
| `float Y` | The Y component. |
| `static Vector2 Zero { get; }` | (0, 0). |
| `static Vector2 One { get; }` | (1, 1). |
| `float Length { get; }` | The Euclidean length. |
| `float LengthSquared { get; }` | The squared length (cheaper than `Vector2.Length` for comparisons). |
| `Vector2 Normalized { get; }` | This vector scaled to length 1, or zero when its length is zero. |
| `static float Dot(Vector2 a, Vector2 b)` | The dot product of two vectors. |
| `static float Distance(Vector2 a, Vector2 b)` | The distance between two points. |
| `static Vector2 Lerp(Vector2 a, Vector2 b, float t)` | Linear interpolation from `a` (t = 0) to `b` (t = 1); t is not clamped. |
| `static Vector2 operator +(Vector2 a, Vector2 b)` | Component-wise sum. |
| `static Vector2 operator -(Vector2 a, Vector2 b)` | Component-wise difference. |
| `static Vector2 operator -(Vector2 value)` | Negation. |
| `static Vector2 operator *(Vector2 a, Vector2 b)` | Component-wise product. |
| `static Vector2 operator *(Vector2 value, float scalar)` | Scales both components. |
| `static Vector2 operator *(float scalar, Vector2 value)` | Scales both components. |
| `static Vector2 operator /(Vector2 value, float scalar)` | Divides both components. |
| `static bool operator ==(Vector2 a, Vector2 b)` | Exact component-wise equality. |
| `static bool operator !=(Vector2 a, Vector2 b)` | Exact component-wise inequality. |
| `bool Equals(Vector2 other)` | Exact component-wise equality. |

### Vector3

```csharp
public struct Vector3 : IEquatable<Vector3>
```

A 3D vector of single-precision components, laid out like the engine's vectors.

| Member | Summary |
|---|---|
| `Vector3(float x, float y, float z)` | Creates a vector from its components. |
| `Vector3(float scalar)` | Creates a vector with every component set to `scalar`. |
| `Vector3(Vector2 xy, float z)` | Creates a vector from a 2D vector and a Z component. |
| `float X` | The X component. |
| `float Y` | The Y component. |
| `float Z` | The Z component. |
| `static Vector3 Zero { get; }` | (0, 0, 0). |
| `static Vector3 One { get; }` | (1, 1, 1). |
| `static Vector3 Right { get; }` | (1, 0, 0), the engine's right direction. |
| `static Vector3 Up { get; }` | (0, 1, 0), the engine's up direction. |
| `static Vector3 Forward { get; }` | (0, 0, -1), the engine's forward direction (cameras look down -Z). |
| `static Vector3 Left { get; }` | (-1, 0, 0). |
| `static Vector3 Down { get; }` | (0, -1, 0). |
| `static Vector3 Back { get; }` | (0, 0, 1). |
| `Vector2 XY { get; }` | The X and Y components. |
| `float Length { get; }` | The Euclidean length. |
| `float LengthSquared { get; }` | The squared length (cheaper than `Vector3.Length` for comparisons). |
| `Vector3 Normalized { get; }` | This vector scaled to length 1, or zero when its length is zero. |
| `static float Dot(Vector3 a, Vector3 b)` | The dot product of two vectors. |
| `static Vector3 Cross(Vector3 a, Vector3 b)` | The cross product of two vectors (right-handed). |
| `static float Distance(Vector3 a, Vector3 b)` | The distance between two points. |
| `static Vector3 Lerp(Vector3 a, Vector3 b, float t)` | Linear interpolation from `a` (t = 0) to `b` (t = 1); t is not clamped. |
| `static Vector3 Min(Vector3 a, Vector3 b)` | The component-wise minimum. |
| `static Vector3 Max(Vector3 a, Vector3 b)` | The component-wise maximum. |
| `static Vector3 operator +(Vector3 a, Vector3 b)` | Component-wise sum. |
| `static Vector3 operator -(Vector3 a, Vector3 b)` | Component-wise difference. |
| `static Vector3 operator -(Vector3 value)` | Negation. |
| `static Vector3 operator *(Vector3 a, Vector3 b)` | Component-wise product. |
| `static Vector3 operator *(Vector3 value, float scalar)` | Scales every component. |
| `static Vector3 operator *(float scalar, Vector3 value)` | Scales every component. |
| `static Vector3 operator /(Vector3 a, Vector3 b)` | Component-wise quotient. |
| `static Vector3 operator /(Vector3 value, float scalar)` | Divides every component. |
| `static bool operator ==(Vector3 a, Vector3 b)` | Exact component-wise equality. |
| `static bool operator !=(Vector3 a, Vector3 b)` | Exact component-wise inequality. |
| `bool Equals(Vector3 other)` | Exact component-wise equality. |

### Vector4

```csharp
public struct Vector4 : IEquatable<Vector4>
```

A 4D vector of single-precision components, laid out like the engine's vectors.

| Member | Summary |
|---|---|
| `Vector4(float x, float y, float z, float w)` | Creates a vector from its components. |
| `Vector4(float scalar)` | Creates a vector with every component set to `scalar`. |
| `Vector4(Vector3 xyz, float w)` | Creates a vector from a 3D vector and a W component. |
| `float X` | The X component. |
| `float Y` | The Y component. |
| `float Z` | The Z component. |
| `float W` | The W component. |
| `static Vector4 Zero { get; }` | (0, 0, 0, 0). |
| `static Vector4 One { get; }` | (1, 1, 1, 1). |
| `Vector3 XYZ { get; }` | The X, Y and Z components. |
| `float Length { get; }` | The Euclidean length. |
| `static float Dot(Vector4 a, Vector4 b)` | The dot product of two vectors. |
| `static Vector4 Lerp(Vector4 a, Vector4 b, float t)` | Linear interpolation from `a` (t = 0) to `b` (t = 1); t is not clamped. |
| `static Vector4 operator +(Vector4 a, Vector4 b)` | Component-wise sum. |
| `static Vector4 operator -(Vector4 a, Vector4 b)` | Component-wise difference. |
| `static Vector4 operator -(Vector4 value)` | Negation. |
| `static Vector4 operator *(Vector4 a, Vector4 b)` | Component-wise product. |
| `static Vector4 operator *(Vector4 value, float scalar)` | Scales every component. |
| `static Vector4 operator *(float scalar, Vector4 value)` | Scales every component. |
| `static Vector4 operator /(Vector4 value, float scalar)` | Divides every component. |
| `static bool operator ==(Vector4 a, Vector4 b)` | Exact component-wise equality. |
| `static bool operator !=(Vector4 a, Vector4 b)` | Exact component-wise inequality. |
| `bool Equals(Vector4 other)` | Exact component-wise equality. |

### Vehicle

```csharp
public sealed class Vehicle : Asset
```

A vehicle definition asset (`.svehicle`): a motorcycle's two wheels (the front steers, the engine drives the rear), its engine, its gearbox and final drive, and how it leans. A `VehicleComponent` names the definition its entity rides with. Edit it in the editor; scripts read it, for example the engine's speeds a rev counter shows or the gears a gear indicator counts, and an edit made while the game plays reaches the vehicles and the scripts on the next frame. A definition that cannot be loaded reads as a new one's values (logged).

| Member | Summary |
|---|---|
| `VehicleTransmissionMode TransmissionMode { get; }` | Whether the gearbox shifts by itself or when the rider asks. |
| `int GearCount { get; }` | How many forward gears the gearbox has (1 to 16); there is no reverse gear. |
| `float FinalDrive { get; }` | The ratio from the gearbox's output to the rear wheel: the primary and final drives together. |
| `float MaxTorque { get; }` | The largest torque the engine gives, in newton meters, at full throttle. |
| `float MinRPM { get; }` | The engine's lowest speed in revolutions per minute: it idles there. |
| `float MaxRPM { get; }` | The engine's highest speed in revolutions per minute: it never turns faster (the red line). |
| `float ShiftUpRPM { get; }` | The engine speed in revolutions per minute above which an Automatic gearbox shifts up. |
| `float ShiftDownRPM { get; }` | The engine speed in revolutions per minute below which an Automatic gearbox shifts down. |
| `float MaxLeanAngle { get; }` | The farthest the bike leans in turns, in degrees. |
| `float GetGearRatio(int gear)` | A forward gear's ratio between the engine and the gearbox's output (the final drive comes after it); 0 for a gear the gearbox lacks. |
| `string GetWheelName(int index)` | A wheel's name, which a VehicleWheel component names to draw it; empty for an index other than 0 and 1. |

### VehicleComponent

```csharp
public sealed class VehicleComponent : Component
```

Makes the entity a motorcycle of its `VehicleComponent.Definition`: the entity needs a dynamic `RigidBodyComponent` and a collider (its chassis), and the entities below it that draw its wheels a VehicleWheel component each. While the scene simulates it, a rider drives it with `VehicleComponent.SetDriverInput` (the throttle, the front and the rear brake, and the lean) and, in a Manual gearbox, `VehicleComponent.ShiftUp` and `VehicleComponent.ShiftDown`; its engine, gearbox, lean and wheels read as its last physics step left them. Set the inputs in `Script.OnFixedUpdate` (or a graph's Event.FixedUpdate): each physics step applies the latest given before it, so a ride set there as a function of the simulation is the same at every frame rate. Without a simulated vehicle (the scene is not playing, the entity has no dynamic body or no definition that loads) the reads are a vehicle at rest in neutral, and the inputs and shifts are logged and do nothing.

| Member | Summary |
|---|---|
| `Vehicle? Definition { get; set; }` | The vehicle definition (a `.svehicle` asset): its wheels, engine, gearbox and lean. Another definition makes the vehicle again; null leaves the entity without a vehicle. |
| `float EngineSpeed { get; }` | The engine's speed in revolutions per minute, as the last physics step left it: from the definition's `Vehicle.MinRPM` (idle) to its `Vehicle.MaxRPM`. |
| `int Gear { get; }` | The gear the gearbox is in: 0 is neutral, 1 the first gear, up to the definition's `Vehicle.GearCount`. A Manual gearbox starts in neutral. |
| `float ForwardSpeed { get; }` | The chassis's speed along its front in meters per second, as the last physics step left it: negative when it rolls backwards. |
| `float LeanAngle { get; }` | How far the chassis leans from the world's up around its front, in degrees: positive to its right, negative to its left. |
| `float Throttle { get; }` | The throttle the last physics step applied, from 0 to 1 (`VehicleComponent.SetDriverInput` gives the next steps'). |
| `float FrontBrake { get; }` | The front brake the last physics step applied, from 0 to 1. |
| `float RearBrake { get; }` | The rear brake the last physics step applied, from 0 to 1. |
| `float Lean { get; }` | The lean the last physics step applied, from -1 (fully to the left) to 1 (fully to the right). |
| `void SetDriverInput(float throttle, float frontBrake, float rearBrake, float lean)` | What the rider asks of the vehicle from its next physics step on, until the next call: each value is clamped to its range. The throttle opens the engine; each brake applies its torque at each wheel the definition gives it (the front brake is usually the front wheel's, the rear brake the rear wheel's); the lean steers, leans and turns the bike (positive to its right). The latest call before a step wins, from `Script.OnFixedUpdate` or `Script.OnUpdate`; a value that is not a finite number is a script error and the call does nothing. |
| `void ShiftUp()` | Asks a Manual gearbox for the next gear up at the next physics step (requests before a step add up); the clutch opens for the definition's switch time and then closes. Nothing happens in top gear, nor in an Automatic gearbox, which shifts by itself. |
| `void ShiftDown()` | Asks a Manual gearbox for the next gear down at the next physics step, down to neutral; nothing happens in neutral, nor in an Automatic gearbox. |
| `VehicleWheelState GetWheelState(int index)` | A wheel as the last physics step left it: what it stands on, its suspension, its spin and its slips. Without a simulated vehicle, a wheel without contact. |

### VehicleTransmissionMode

```csharp
public enum VehicleTransmissionMode
```

How a vehicle's gearbox changes gear.

Values: `Automatic`, `Manual`.

### VehicleWheelState

```csharp
public struct VehicleWheelState
```

A wheel of a vehicle as its last physics step left it (`VehicleComponent.GetWheelState`), a struct of public fields that graphs Make and Break.

| Member | Summary |
|---|---|
| `bool HasContact` | Whether the wheel touches the ground: its suspension then carries the chassis and its tyre grips. |
| `Entity? Ground` | The entity the wheel stands on (its script instance when it has one); null without contact. |
| `PhysicsMaterial? Material` | The physics material of the surface under the wheel; null without contact and on a surface without one, whose tyre grip is 1 (see `VehicleWheelState.Surface`). |
| `Vector3 ContactPoint` | Where the tyre touches the ground, in world space; zero without contact. |
| `Vector3 ContactNormal` | The ground's normal where the tyre touches it, unit length; zero without contact. |
| `float SuspensionLength` | Meters from where the suspension is attached to the wheel's center, between the definition's shortest and longest suspension lengths: short when compressed. |
| `float Spin` | How fast the wheel turns around its axle, in radians per second: positive when it rolls forward. |
| `float LongitudinalSlip` | The size of the longitudinal slip ratio: how much faster or slower the tyre's tread moves than the ground goes by under it, as a share of the ground's speed. It is 0 when the tyre rolls with the ground and grows as the tyre spins or slides, either way: 1 for a locked wheel, and large for a wheel that turns while the bike barely moves. It is never negative, so it does not tell spinning from braking: compare `VehicleWheelState.Spin` times the wheel's radius with `VehicleComponent.ForwardSpeed` for that (faster when the tyre spins, slower when it brakes). 0 without contact. |
| `float LateralSlip` | The slip angle in degrees, between where the tyre points and where it moves over the ground: from 0 to 90, whichever side the tyre slides to (never negative); 0 without contact or while the tyre barely moves. |
| `string Surface { get; }` | The name of the surface under the wheel (its material's `PhysicsMaterial.Surface`, such as Asphalt or Gravel); empty without contact or a material. |

### VisualNodeAttribute

```csharp
public sealed class VisualNodeAttribute : Attribute
```

Makes a member of the game's scripts a node of visual scripts, or every public member of a type: graphs call your code as they call the scripting API.

| Member | Summary |
|---|---|
| `VisualNodeAttribute()` |  |
| `string Title { get; set; }` | The node's title; empty for the member's name in words ("AddHealth" gives "Add Health"). A property's or field's nodes are titled "Get" and "Set" followed by it. |
| `string Category { get; set; }` | Where the node palette lists the node, as a path of categories ("Game/Combat"); empty for "Game/" and the declaring type's name in words. On a type, the category of the nodes of its members that name none. |
| `string Description { get; set; }` | What the node does, shown by the palette and the node's tooltip. |
| `string[] Keywords { get; set; }` | More words the palette finds the node by. |
| `bool Pure { get; set; }` | Makes a method a pure node: no flow pins, and it runs each time a node reads its outputs. Only for methods that return a value or have `out` parameters and change nothing. |
| `string[] FormerNames { get; set; }` | Names the member had before: "Heal" for a member of the same type, "Game.Hero.Heal" for one that moved to another type (for a constructor, the type's former full name). Graphs that use a former name keep working and store the current one when next saved. |

### VisualScript

```csharp
public sealed class VisualScript : Asset
```

A visual script asset (`.sgraph`): a graph of nodes that a `VisualScriptComponent` runs on its entity.


### VisualScriptComponent

```csharp
public sealed class VisualScriptComponent : Component
```

Runs a visual script (a graph asset) on the entity, beside or instead of a C# script. While the scene runs, C# scripts raise the graph's custom events and read and write its variables through this component.

| Member | Summary |
|---|---|
| `VisualScript? Graph { get; set; }` | The visual script the entity runs, or null. Changed while the scene runs, it takes effect at the scene's next update: the running graph ends (its Destroy event runs) and the new one starts. The values the component stores for the graph's public variables stay as they are. |
| `bool SendEvent(string name, params object?[] arguments)` | Raises the custom event `name` of the entity's running graph with the arguments, which must match the event's parameters in number. Each argument converts to its parameter's type as a link converts it (see the remarks), and also to text for a `String` parameter and from a typed asset to its `AssetHandle`; null is accepted for parameters of reference types only. The event runs at once; raised while a graph runs (the creation of a prefab instance a graph spawns), it runs within that graph's step budget. |
| `T? GetVariable<T>(string name)` | The value of the variable `name` (public or private, not a function's local) of the entity's running graph, as a `T`: the variable's own type, a wider numeric type, a base type or `Object`. A variable of a reference type that holds no value reads as null. |
| `void SetVariable<T>(string name, T? value)` | Sets the variable `name` (public or private, not a function's local) of the entity's running graph, which sees the new value at once. The value must convert to the variable's type (see the remarks). Null sets a variable of a reference type to none (a `String` variable to the empty string) when `T` converts to the variable's type. |
| `bool HasVariable(string name)` | Whether the entity's running graph has a variable (public or private) of the name. |

### WindowMode

```csharp
public enum WindowMode
```

How the game's window is shown.

Values: `Windowed`, `Fullscreen`.

## Strada.Testing

| Type | Kind | Summary |
|---|---|---|
| [Assert](#assert) | static class | Checks for tests. Each throws an `AssertionException` describing the failure, followed by `message` when one is given. |
| [AssertionException](#assertionexception) | class | Thrown by `Assert` when a check fails; `TestReporter.Run` reports its message. |
| [TestReporter](#testreporter) | static class | Reports the results of checks to the application running the scene: the game player's test mode (`--test`) exits with the number of failures, and the editor's test commands return them. Results are logged too. |

### Assert

```csharp
public static class Assert
```

Checks for tests. Each throws an `AssertionException` describing the failure, followed by `message` when one is given.

| Member | Summary |
|---|---|
| `static void IsTrue(bool condition, string message = "")` | Fails unless `condition` is true. |
| `static void IsFalse(bool condition, string message = "")` | Fails unless `condition` is false. |
| `static void AreEqual<T>(T? expected, T? actual, string message = "")` | Fails unless the values are equal (by `Collections.Generic.EqualityComparer`). |
| `static void AreNotEqual<T>(T? notExpected, T? actual, string message = "")` | Fails when the values are equal (by `Collections.Generic.EqualityComparer`). |
| `static void AreApproximatelyEqual(float expected, float actual, float tolerance = 1E-05f, string message = "")` | Fails unless the numbers differ by at most `tolerance`. |
| `static void AreApproximatelyEqual(Vector3 expected, Vector3 actual, float tolerance = 1E-05f, string message = "")` | Fails unless the vectors are at most `tolerance` apart. |
| `static void IsNull(object? value, string message = "")` | Fails unless `value` is null. |
| `static void IsNotNull(object value, string message = "")` | Fails when `value` is null. |
| `static TException Throws<TException>(Action action, string message = "")` | Fails unless `action` throws a `TException` (or a derived exception), which it returns. |
| `static void Fail(string message)` | Fails with `message`. |

### AssertionException

```csharp
public sealed class AssertionException : Exception
```

Thrown by `Assert` when a check fails; `TestReporter.Run` reports its message.


### TestReporter

```csharp
public static class TestReporter
```

Reports the results of checks to the application running the scene: the game player's test mode (`--test`) exits with the number of failures, and the editor's test commands return them. Results are logged too.

| Member | Summary |
|---|---|
| `static void Pass(string name)` | Records a passed check. |
| `static void Fail(string name, string message)` | Records a failed check and why it failed. |
| `static bool Run(string name, Action check)` | Runs a check, which passes unless it throws (assertion failures report their message, other exceptions their type, message and stack trace). Returns whether it passed. |
| `static void Finish()` | Ends the test run: every check has reported. |
