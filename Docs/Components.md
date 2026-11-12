# Component reference

<!-- Generated from the component registry by StradaTests (Tests/StradaTests/Source/Scene/ComponentReferenceTests.cpp):
     edit the components' descriptions in the engine, then run StradaTests with STRADA_UPDATE_DOCS=1. -->

Every component you can add to an entity, with every field: its type, its default, the values it accepts and what
it does. The inspector, scene files, the automation command `component.types` and the scripting API all read the same
registry, so this page matches the engine: the engine's tests fail when it does not.

28 components in 6 categories, with 201 fields in all. Vectors are written `(x, y, z)`, colors are
linear RGB or RGBA from 0 to 1, distances are in meters and angles in degrees. Fields that only matter for some
settings say when the inspector shows them. Scripts reach the same fields through the component classes of
[ScriptingAPI.md](ScriptingAPI.md), and agents through `component.get` and `component.set`
([Automation.md](Automation.md)).

| Category | Components |
|---|---|
| Core | [Tag](#tag), [Transform](#transform) |
| Rendering | [Camera](#camera), [Mesh](#mesh), [DirectionalLight](#directionallight), [PointLight](#pointlight), [SpotLight](#spotlight), [SkyLight](#skylight), [Text](#text), [SpriteRenderer](#spriterenderer), [ParticleSystem](#particlesystem), [PostProcess](#postprocess) |
| Physics | [RigidBody](#rigidbody), [BoxCollider](#boxcollider), [SphereCollider](#spherecollider), [CapsuleCollider](#capsulecollider), [MeshCollider](#meshcollider), [Joint](#joint), [CharacterController](#charactercontroller) |
| Audio | [AudioSource](#audiosource), [AudioListener](#audiolistener) |
| Scripting | [Script](#script), [VisualScript](#visualscript) |
| UI | [UICanvas](#uicanvas), [UIRect](#uirect), [UIImage](#uiimage), [UIText](#uitext), [UIButton](#uibutton) |

## Core

Every entity has these: its name and its place in the world.

### Tag

The entity's name.

Every entity has it; it cannot be removed.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Tag` | string | `"Entity"` |  | The name the hierarchy shows; scripts and agents find entities by it. |

### Transform

Position, rotation and scale relative to the parent entity.

Every entity has it; it cannot be removed.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Translation` | vec3 | `(0, 0, 0)` |  | Position relative to the parent, in meters. |
| `Rotation` | rotation | `(0, 0, 0, 1)` |  | Orientation relative to the parent as a quaternion [x, y, z, w]; input also accepts RotationEuler, [pitch, yaw, roll] in degrees. |
| `Scale` | vec3 | `(1, 1, 1)` |  | Scale relative to the parent. |

## Rendering

What the renderer draws: cameras, meshes, lights, the sky, text, sprites, particles and post-processing.

### Camera

Renders the scene in play mode and in exported games.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Projection` | enum | `Perspective` |  | Perspective draws distant things smaller; orthographic keeps their size at any distance. One of `Perspective`, `Orthographic`. |
| `PerspectiveFOV` | float (degrees) | `60` | 1 to 179 | Vertical field of view in degrees. Shown when `Projection` is `Perspective`. |
| `PerspectiveNear` | float | `0.1` | at least 0.001 | Near clip distance in meters. Shown when `Projection` is `Perspective`. |
| `PerspectiveFar` | float | `1000` | at least 0.01 | Maximum view distance for culling and shadows; the projection itself has no far plane. Shown when `Projection` is `Perspective`. |
| `OrthographicSize` | float | `10` | at least 0.001 | Full view height in world units. Shown when `Projection` is `Orthographic`. |
| `OrthographicNear` | float | `-100` |  | Near clip plane distance in meters (may be negative). Shown when `Projection` is `Orthographic`. |
| `OrthographicFar` | float | `100` |  | Far clip plane distance in meters. Shown when `Projection` is `Orthographic`. |
| `Primary` | bool | `true` |  | The scene renders from the first primary camera. |
| `FixedAspectRatio` | bool | `false` |  | Uses AspectRatio instead of following the viewport. |
| `AspectRatio` | float | `1.7777778` | at least 0.01 | Width divided by height when FixedAspectRatio is set. Shown when `FixedAspectRatio` is on. |

### Mesh

Draws a mesh with materials.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Mesh` | Mesh asset | none |  | The mesh to draw: one of an imported model or a built-in shape. |
| `Materials` | Material assets | none |  | Material overrides per material slot of the mesh; an empty entry keeps the mesh's own material. The inspector edits it with controls of its own. |
| `CastShadows` | bool | `true` |  | Draws the mesh into the shadow maps of the lights that cast shadows. |
| `Visible` | bool | `true` |  | Draws the mesh; a hidden mesh casts no shadow either. |

### DirectionalLight

Sun-like light shining along the entity's forward direction (-Z).

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Color` | color (RGB) | `(1, 1, 1)` | 0 to 1 | The light's color. |
| `Intensity` | float | `3` | at least 0 | Brightness: multiplies Color before the exposure. |
| `CastShadows` | bool | `true` |  | Meshes that cast shadows shadow what this light lights, in soft cascades; only the first directional light that casts shadows does. |
| `LightSize` | float (degrees) | `0.5` | 0 to 20 | Apparent angular diameter of the light source in degrees; larger values give softer shadows. |

### PointLight

Light shining in every direction from the entity's position.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Color` | color (RGB) | `(1, 1, 1)` | 0 to 1 | The light's color. |
| `Intensity` | float | `10` | at least 0 | Brightness: multiplies Color before the exposure; it falls off with the square of the distance, to nothing at Range. |
| `Range` | float | `10` | at least 0 | Distance in meters at which the light fades out completely. |
| `CastShadows` | bool | `false` |  | Meshes that cast shadows shadow what this light lights. |

### SpotLight

Cone of light shining along the entity's forward direction (-Z).

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Color` | color (RGB) | `(1, 1, 1)` | 0 to 1 | The light's color. |
| `Intensity` | float | `10` | at least 0 | Brightness: multiplies Color before the exposure; it falls off with the square of the distance, to nothing at Range. |
| `Range` | float | `10` | at least 0 | Distance in meters at which the light fades out completely. |
| `InnerConeAngle` | float (degrees) | `20` | 0 to 89.9 | Half-angle of the full-intensity cone in degrees. |
| `OuterConeAngle` | float (degrees) | `30` | 0.1 to 89.9 | Half-angle of the cone at which the light fades out, in degrees. |
| `CastShadows` | bool | `false` |  | Meshes that cast shadows shadow what this light lights. |

### SkyLight

Image-based lighting and the sky from an HDR environment map.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Environment` | Environment asset | none |  | HDR environment map; without one, AmbientColor lights the scene. |
| `Intensity` | float | `1` | at least 0 | Multiplies the environment's light and the sky's brightness, or AmbientColor without an environment. |
| `Rotation` | float (degrees) | `0` |  | Rotation of the environment around the world Y axis in degrees. |
| `SkyboxBlur` | float | `0` | 0 to 1 | 0 shows a sharp sky, 1 a fully blurred one. Shown when `DrawSkybox` is on. |
| `DrawSkybox` | bool | `true` |  | Draws the environment as the background. |
| `AmbientColor` | color (RGB) | `(0.03, 0.03, 0.03)` | 0 to 1 | Ambient light used when no environment map is set. |

### Text

Draws text in the world or on the screen.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Text` | string (multiline) | empty |  | The text to draw; a line break starts a new line. |
| `Font` | Font asset | none |  | None uses the default font. |
| `Color` | color (RGBA) | `(1, 1, 1, 1)` | 0 to 1 | The text's color and opacity. |
| `FontSize` | float | `1` | at least 0.001 | Line height: pixels for screen-space text, world units otherwise. |
| `ScreenSpace` | bool | `false` |  | Places the text on the screen: translation x and y are viewport coordinates in [0, 1] from the top-left. |
| `Alignment` | enum | `Left` |  | Aligns the lines to the left, the center or the right. One of `Left`, `Center`, `Right`. |
| `LineSpacing` | float | `1` | at least 0 | Multiplier of the font's line height. |

### SpriteRenderer

Draws a textured or colored quad in the world or on the screen.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Color` | color (RGBA) | `(1, 1, 1, 1)` | 0 to 1 | Tint multiplied with the texture. |
| `Texture` | Texture asset | none |  | None draws a solid color. |
| `Tiling` | float | `1` |  | Number of texture repetitions across the sprite. |
| `ScreenSpace` | bool | `false` |  | Places the sprite on the screen like screen-space text; scale is in pixels. |

### ParticleSystem

Emits glowing or smoky camera-facing particles (fire, sparks, smoke, dust, magic) while the scene runs.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Emitting` | bool | `true` |  | Emits Rate particles per second while set. |
| `Rate` | float | `20` | 0 to 10000 | Particles per second while emitting. |
| `Burst` | uint | `0` | 0 to 10000 | Particles emitted at once when the system starts (with the scene, or when the entity is created). |
| `MaxParticles` | uint | `1000` | 1 to 100000 | Particles alive at once; emission waits while the system is full. |
| `LifetimeMin` | float | `1` | 0.01 to 600 | Shortest life in seconds. |
| `LifetimeMax` | float | `2` | 0.01 to 600 | Longest life in seconds. |
| `SpeedMin` | float | `1` | 0 to 1000 | Slowest start speed in meters per second. |
| `SpeedMax` | float | `3` | 0 to 1000 | Fastest start speed in meters per second. |
| `Shape` | enum | `Cone` |  | Point, Sphere (outwards), Cone (along -Z from a disc) or Box (along -Z from inside a box). One of `Point`, `Sphere`, `Cone`, `Box`. |
| `ConeAngle` | float (degrees) | `25` | 0 to 180 | Spread around the forward axis (-Z) in degrees, for cones and boxes. Shown when `Shape` is `Cone` or `Box`. |
| `Radius` | float | `0.1` | 0 to 1000 | Radius of the sphere or of the cone's base, in meters. Shown when `Shape` is `Sphere` or `Cone`. |
| `BoxHalfExtents` | vec3 | `(0.5, 0.5, 0.5)` | 0 to 1000 | Half size of the box in meters. Shown when `Shape` is `Box`. |
| `GravityFactor` | float | `0` | -100 to 100 | Multiplies the scene's gravity; negative values make particles rise. |
| `Drag` | float | `0` | 0 to 100 | Fraction of the velocity lost per second. |
| `StartSize` | float | `0.2` | 0 to 1000 | Width in meters when a particle is born. |
| `EndSize` | float | `0.05` | 0 to 1000 | Width in meters when a particle dies. |
| `StartColor` | color (RGBA) | `(1, 0.75, 0.35, 1)` | 0 to 1 | Color and opacity at birth. |
| `EndColor` | color (RGBA) | `(1, 0.2, 0.05, 0)` | 0 to 1 | Color and opacity at death. |
| `Intensity` | float | `1` | 0 to 1000 | Multiplies the colors; values above 1 glow with bloom. |
| `Texture` | Texture asset | none |  | None draws a soft round dot. |
| `BlendMode` | enum | `Additive` |  | Additive adds light (fire, sparks); Alpha covers what is behind (smoke, dust). One of `Additive`, `Alpha`. |
| `WorldSpace` | bool | `true` |  | Particles stay where they were emitted when the entity moves; otherwise they move with it. |
| `DestroyWhenFinished` | bool | `false` |  | Destroys the entity once it no longer emits and its last particle died (one-shot effects). |

### PostProcess

Post-process volume: overrides the scene's exposure, tonemapper, bloom, ambient occlusion, color grading and lens effects everywhere (global) or while the camera is in the entity's box (local, fading over a blend distance), in order of priority and scaled by weight.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Global` | bool | `true` |  | Applies everywhere; otherwise only inside the box the entity's transform makes of the unit cube. |
| `BlendDistance` | float | `1` | at least 0 | Local volumes: meters around the box over which the volume fades out. Shown when `Global` is off. |
| `Priority` | float | `0` |  | Volumes with higher priorities apply over those with lower ones. |
| `Weight` | float | `1` | 0 to 1 | How much of the volume applies. |
| `OverrideEV100` | bool | `false` |  | Applies the volume's EV100. |
| `EV100` | float | `0` | -20 to 20 | Exposure value at ISO 100; higher values darken the image. Applies while `OverrideEV100` is on. |
| `OverrideTonemapper` | bool | `false` |  | Applies the volume's tonemapper. |
| `Tonemapper` | enum | `ACES` |  | Maps HDR colors to the display range. One of `ACES`, `AgX`, `PBRNeutral`, `Reinhard`, `None`. Applies while `OverrideTonemapper` is on. |
| `OverrideAmbientOcclusion` | bool | `false` |  | Switches ambient occlusion on or off as the volume says. |
| `AmbientOcclusion` | bool | `true` |  | Darkens indirect light in creases and corners. Applies while `OverrideAmbientOcclusion` is on. |
| `OverrideAmbientOcclusionIntensity` | bool | `false` |  | Applies the volume's ambient occlusion intensity. |
| `AmbientOcclusionIntensity` | float | `1.5` | 0.1 to 8 | Contrast exponent of ambient occlusion. Applies while `OverrideAmbientOcclusionIntensity` is on. |
| `OverrideBloom` | bool | `false` |  | Fades bloom in or out as the volume says. |
| `Bloom` | bool | `true` |  | Glow around bright areas. Applies while `OverrideBloom` is on. |
| `OverrideBloomIntensity` | bool | `false` |  | Applies the volume's bloom intensity. |
| `BloomIntensity` | float | `0.04` | 0 to 1 | Fraction of the blurred image mixed into the scene. Applies while `OverrideBloomIntensity` is on. |
| `OverrideContrast` | bool | `false` |  | Applies the volume's contrast. |
| `Contrast` | float | `1` | 0 to 2 | Contrast around middle gray; 1 leaves colors unchanged. Applies while `OverrideContrast` is on. |
| `OverrideSaturation` | bool | `false` |  | Applies the volume's saturation. |
| `Saturation` | float | `1` | 0 to 2 | Color intensity: 0 is grayscale, 1 unchanged, 2 doubles it. Applies while `OverrideSaturation` is on. |
| `OverrideTemperature` | bool | `false` |  | Applies the volume's temperature. |
| `Temperature` | float | `0` | -1 to 1 | White balance from cool (-1, blue) to warm (1, orange). Applies while `OverrideTemperature` is on. |
| `OverrideTint` | bool | `false` |  | Applies the volume's tint. |
| `Tint` | float | `0` | -1 to 1 | White balance from green (-1) to magenta (1). Applies while `OverrideTint` is on. |
| `OverrideColorFilter` | bool | `false` |  | Applies the volume's color filter. |
| `ColorFilter` | color (RGB) | `(1, 1, 1)` | 0 to 2 | Multiplies every color. Applies while `OverrideColorFilter` is on. |
| `OverrideChromaticAberration` | bool | `false` |  | Applies the volume's chromatic aberration. |
| `ChromaticAberration` | float | `0` | 0 to 1 | Lens color fringes that grow towards the edges of the image. Applies while `OverrideChromaticAberration` is on. |
| `OverrideVignette` | bool | `false` |  | Applies the volume's vignette. |
| `Vignette` | float | `0` | 0 to 1 | Darkens the corners of the image. Applies while `OverrideVignette` is on. |
| `OverrideVignetteSmoothness` | bool | `false` |  | Applies the volume's vignette smoothness. |
| `VignetteSmoothness` | float | `0.45` | 0.01 to 1 | How gradually the vignette fades in from the center. Applies while `OverrideVignetteSmoothness` is on. |
| `OverrideFilmGrain` | bool | `false` |  | Applies the volume's film grain. |
| `FilmGrain` | float | `0` | 0 to 1 | Animated film grain. Applies while `OverrideFilmGrain` is on. |

## Physics

Jolt physics: bodies, their colliders, joints between bodies and character controllers.

### RigidBody

Simulates the entity with physics; the entity's colliders give it its shape.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Type` | enum | `Static` |  | Static bodies never move, dynamic bodies are simulated and kinematic bodies are moved by scripts. One of `Static`, `Dynamic`, `Kinematic`. |
| `Mass` | float | `1` | at least 0.001 | Mass in kilograms. Shown when `Type` is `Dynamic`. |
| `LinearDamping` | float | `0.05` | at least 0 | Slows the body's movement over time, like air resistance. Shown when `Type` is `Dynamic`. |
| `AngularDamping` | float | `0.05` | at least 0 | Slows the body's rotation over time. Shown when `Type` is `Dynamic`. |
| `GravityFactor` | float | `1` |  | Multiplier of the scene's gravity. Shown when `Type` is `Dynamic`. |
| `Layer` | uint | `0` | 0 to 15 | Physics layer index as configured in the project settings. |
| `LockTranslation` | bool per axis | `(false, false, false)` |  | Prevents movement along the X, Y and Z axes. Shown when `Type` is `Dynamic`. |
| `LockRotation` | bool per axis | `(false, false, false)` |  | Prevents rotation around the X, Y and Z axes. Shown when `Type` is `Dynamic`. |
| `ContinuousCollision` | bool | `false` |  | Keeps fast bodies from passing through thin geometry, at a performance cost. Shown when `Type` is `Dynamic`. |
| `AllowSleep` | bool | `true` |  | Lets the body stop simulating while it is at rest. Shown when `Type` is `Dynamic` or `Kinematic`. |
| `InitialLinearVelocity` | vec3 | `(0, 0, 0)` |  | Velocity in meters per second when play starts (the simulation allows up to 500). Shown when `Type` is `Dynamic` or `Kinematic`. |
| `InitialAngularVelocity` | vec3 | `(0, 0, 0)` |  | Angular velocity in radians per second when play starts (the simulation allows up to about 47). Shown when `Type` is `Dynamic` or `Kinematic`. |

### BoxCollider

Box collision shape, scaled by the entity.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `HalfExtents` | vec3 | `(0.5, 0.5, 0.5)` | at least 0.001 | Half the size of the box along each axis, in meters. |
| `Offset` | vec3 | `(0, 0, 0)` |  | Center of the shape relative to the entity, in meters. |
| `IsTrigger` | bool | `false` |  | Reports overlaps instead of colliding. |
| `Friction` | float | `0.6` | at least 0 | Resists sliding along other bodies; two touching bodies use the square root of the product of their frictions. |
| `Restitution` | float | `0` | 0 to 1 | Bounciness: 0 does not bounce, 1 is perfectly elastic. |

### SphereCollider

Sphere collision shape, scaled by the entity.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Radius` | float | `0.5` | at least 0.001 | Radius in meters. |
| `Offset` | vec3 | `(0, 0, 0)` |  | Center of the shape relative to the entity, in meters. |
| `IsTrigger` | bool | `false` |  | Reports overlaps instead of colliding. |
| `Friction` | float | `0.6` | at least 0 | Resists sliding along other bodies; two touching bodies use the square root of the product of their frictions. |
| `Restitution` | float | `0` | 0 to 1 | Bounciness: 0 does not bounce, 1 is perfectly elastic. |

### CapsuleCollider

Capsule collision shape along the local Y axis, scaled by the entity.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Radius` | float | `0.5` | at least 0.001 | Radius in meters. |
| `HalfHeight` | float | `0.5` | at least 0 | Half the height of the cylindrical part (without the caps), in meters. |
| `Offset` | vec3 | `(0, 0, 0)` |  | Center of the shape relative to the entity, in meters. |
| `IsTrigger` | bool | `false` |  | Reports overlaps instead of colliding. |
| `Friction` | float | `0.6` | at least 0 | Resists sliding along other bodies; two touching bodies use the square root of the product of their frictions. |
| `Restitution` | float | `0` | 0 to 1 | Bounciness: 0 does not bounce, 1 is perfectly elastic. |

### MeshCollider

Collision shape made from a mesh, as a convex hull or as triangles.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Mesh` | Mesh asset | none |  | Collision mesh; none uses the entity's Mesh component. |
| `Convex` | bool | `true` |  | Convex hulls can move; triangle meshes are only valid for static and kinematic bodies. |
| `IsTrigger` | bool | `false` |  | Reports overlaps instead of colliding. |
| `Friction` | float | `0.6` | at least 0 | Resists sliding along other bodies; two touching bodies use the square root of the product of their frictions. |
| `Restitution` | float | `0` | 0 to 1 | Bounciness: 0 does not bounce, 1 is perfectly elastic. |

### Joint

Joins the body to another body or the world: fixed, hinge (doors, wheels, pendulums), ball (chains, ropes), distance or slider (pistons), with limits and motors.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Type` | enum | `Hinge` |  | Fixed (welded), Hinge (turns around Axis), Ball (turns freely), Distance (keeps the anchors apart) or Slider (slides along Axis). One of `Fixed`, `Hinge`, `Ball`, `Distance`, `Slider`. |
| `ConnectedEntity` | entity | none |  | The entity whose body this one is joined to (it needs a RigidBody and a collider); none joins it to the world. |
| `Anchor` | vec3 | `(0, 0, 0)` | -1000000 to 1000000 | Where the joint is, in this entity's local space. |
| `ConnectedAnchor` | vec3 | `(0, 0, 0)` | -1000000 to 1000000 | Distance joints: the far point, in the connected entity's local space (world space without one). Shown when `Type` is `Distance`. |
| `Axis` | vec3 | `(0, 1, 0)` | -1 to 1 | Hinge or slider axis in this entity's local space. Shown when `Type` is `Hinge` or `Slider`. |
| `LimitsEnabled` | bool | `false` |  | Limits a hinge's angle or a slider's travel. Shown when `Type` is `Hinge` or `Slider`. |
| `MinLimit` | float | `-45` | -1000000 to 0 | Hinges: degrees (down to -180); sliders: meters (0 is where the bodies start). Shown when `Type` is `Hinge` or `Slider`. |
| `MaxLimit` | float | `45` | 0 to 1000000 | Hinges: degrees (up to 180); sliders: meters. Shown when `Type` is `Hinge` or `Slider`. |
| `MinDistance` | float | `-1` | -1 to 1000000 | Distance joints: the shortest distance; -1 keeps the starting distance. Shown when `Type` is `Distance`. |
| `MaxDistance` | float | `-1` | -1 to 1000000 | Distance joints: the longest distance; -1 keeps the starting distance. Shown when `Type` is `Distance`. |
| `MotorEnabled` | bool | `false` |  | Drives a hinge or slider at MotorSpeed. Shown when `Type` is `Hinge` or `Slider`. |
| `MotorSpeed` | float | `90` | -10000 to 10000 | Hinges: degrees per second; sliders: meters per second. Shown when `Type` is `Hinge` or `Slider`. |
| `MotorMaxForce` | float | `1000` | 0 to 1e+09 | The most torque (hinges, newton meters) or force (sliders, newtons) the motor uses. Shown when `Type` is `Hinge` or `Slider`. |

### CharacterController

Makes the entity a character that scripts move (players, creatures): an upright capsule that walks, climbs steps and slopes, falls, rides what it stands on and pushes dynamic bodies. It replaces the entity's colliders and RigidBody; colliders of its children are its own parts, which neither block nor touch it.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Radius` | float | `0.5` | at least 0.01 | Radius of the capsule in meters. |
| `Height` | float | `2` | at least 0.01 | Height of the whole capsule in meters, rounded ends included; below twice the radius it is a sphere. |
| `Offset` | vec3 | `(0, 0, 0)` |  | Center of the capsule relative to the entity, in meters (scaled with the entity; the capsule does not turn with it). |
| `StepHeight` | float | `0.3` | at least 0 | The highest step the character climbs, and the deepest drop it walks down without leaving the ground, in meters. |
| `MaxSlopeAngle` | float (degrees) | `45` | 1 to 89 | The steepest slope the character walks up, in degrees; it slides down steeper ones. |
| `Mass` | float | `70` | at least 0 | Kilograms with which the character weighs on what it stands on. |
| `PushForce` | float | `200` | at least 0 | The largest force, in newtons, with which the character pushes dynamic bodies; 0 leaves them where they are. |
| `GravityFactor` | float | `1` | 0 to 1000 | Multiplier of the scene's gravity; 0 lets scripts fly the character (ladders, swimming). |
| `Layer` | uint | `0` | 0 to 15 | Physics layer index as configured in the project settings. |

## Audio

Sound sources, positioned in 3D or not, and the listener that hears them.

### AudioSource

Plays an audio clip, optionally positioned in 3D.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Clip` | AudioClip asset | none |  | The sound to play. |
| `Stream` | bool | `false` |  | Reads the clip from its file while it plays instead of loading it whole first: for music and long ambience. |
| `Volume` | float | `1` | at least 0 | Linear gain; 1 plays the clip unchanged. |
| `Pitch` | float | `1` | at least 0.01 | Playback speed multiplier; 1 plays the clip unchanged. |
| `Loop` | bool | `false` |  | Plays the clip again from its start when it ends. |
| `PlayOnStart` | bool | `false` |  | Starts playing when play mode starts. |
| `Spatial` | bool | `true` |  | Positions the sound in 3D; otherwise it plays at a constant level. |
| `MinDistance` | float | `1` | at least 0 | Distance in meters within which the sound plays at full volume. Shown when `Spatial` is on. |
| `MaxDistance` | float | `100` | at least 0 | Distance in meters beyond which the sound gets no quieter. Shown when `Spatial` is on. |

### AudioListener

The ears of the scene, usually on the camera.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Active` | bool | `true` |  | The first active listener hears the scene. |

## Scripting

Behavior: C# scripts and visual scripts that run on the entity.

### Script

Attaches a C# script class to the entity.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `ClassName` | string | empty |  | Fully qualified C# class name, e.g. Game.PlayerController. |
| `Fields` | field values | none |  | Values of the script's public fields. |

### VisualScript

Runs a visual script (.sgraph) on the entity.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Graph` | VisualScript asset | none |  | The visual script the entity runs; none runs nothing. |
| `Variables` | field values | none |  | Values of the graph's public variables for this entity, by name; the others start with the graph's values. The inspector edits it with controls of its own. |

## UI

Screen-space interfaces: canvases, rectangles laid out by anchors, images, text and buttons.

### UICanvas

The root of a screen-space UI (HUDs, menus): its descendants are laid out on the screen by their UIRect and drawn over the scene in hierarchy order.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `ScaleMode` | enum | `ScaleWithScreenSize` |  | ScaleWithScreenSize fits the whole reference resolution on any screen; ScaleWithScreenHeight fits its height (narrower screens cut off the sides); ConstantPixelSize keeps the UI's size in pixels. One of `ConstantPixelSize`, `ScaleWithScreenHeight`, `ScaleWithScreenSize`. |
| `ReferenceResolution` | vec2 | `(1920, 1080)` | 1 to 100000 | The screen size the UI is designed for, in canvas units, which the scale mode fits to the screen. |

### UIRect

Places a UI element in its parent's rectangle (the screen for a canvas's children) with anchors, a position, a size and a pivot.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `AnchorMin` | vec2 | `(0.5, 0.5)` | 0 to 1 | Where the rectangle's span starts in the parent's: 0 is its left or top edge, 1 its right or bottom edge. |
| `AnchorMax` | vec2 | `(0.5, 0.5)` | 0 to 1 | Where the span ends; equal anchors attach the rectangle to a point, others stretch it with the parent. |
| `Position` | vec2 | `(0, 0)` | -1000000 to 1000000 | Moves the pivot from its point of the anchors' span, in canvas units (y downwards). |
| `Size` | vec2 | `(200, 60)` | -1000000 to 1000000 | Canvas units added to the anchors' span: the size itself when the anchors are equal. |
| `Pivot` | vec2 | `(0.5, 0.5)` | 0 to 1 | The point of the rectangle Position places, from its top-left corner (0, 0) to its bottom-right corner (1, 1). |
| `Visible` | bool | `true` |  | Hidden rectangles and their descendants are neither drawn nor clicked. |
| `Opacity` | float | `1` | 0 to 1 | Multiplies the alpha of the element and its descendants. |

### UIImage

Fills a UI element's rectangle with a color or a texture, with optionally rounded corners (panels, icons, buttons).

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Color` | color (RGBA) | `(1, 1, 1, 1)` | 0 to 1 | Multiplied with the texture; a UIButton on the entity replaces it with the color of its state. |
| `Texture` | Texture asset | none |  | None fills the rectangle with the color. |
| `PreserveAspect` | bool | `false` |  | Draws the texture at its own aspect ratio, as large as fits and centered in the rectangle. |
| `CornerRadius` | float | `0` | 0 to 1000000 | Radius of the rounded corners in canvas units (at most half the rectangle's shorter side). |

### UIText

Draws text in a UI element's rectangle, wrapped at its width and aligned in it.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Text` | string (multiline) | empty |  | The text to draw; a line break starts a new line. |
| `Font` | Font asset | none |  | None uses the default font. |
| `Color` | color (RGBA) | `(1, 1, 1, 1)` | 0 to 1 | The text's color and opacity. |
| `FontSize` | float | `32` | 1 to 4096 | Line height in canvas units. |
| `Alignment` | enum | `Center` |  | Places the lines at the left, center or right of the rectangle. One of `Left`, `Center`, `Right`. |
| `VerticalAlignment` | enum | `Middle` |  | Places the text block at the top, middle or bottom of the rectangle. One of `Top`, `Middle`, `Bottom`. |
| `Wrap` | bool | `true` |  | Breaks lines that do not fit the rectangle's width at spaces. |
| `LineSpacing` | float | `1` | 0 to 100 | Multiplier of the font's line height. |

### UIButton

Makes a UI element clickable: scripts read WasClicked and get OnClick; its colors replace the UIImage's color in each state.

| Field | Type | Default | Range | Description |
|---|---|---|---|---|
| `Interactable` | bool | `true` |  | Disabled buttons take no clicks and show DisabledColor. |
| `NormalColor` | color (RGBA) | `(0.022, 0.1473, 0.7106, 1)` | 0 to 1 | The image's color at rest. |
| `HoveredColor` | color (RGBA) | `(0.055, 0.2429, 0.9331, 1)` | 0 to 1 | The image's color under the mouse. |
| `PressedColor` | color (RGBA) | `(0.01, 0.0732, 0.3931, 1)` | 0 to 1 | The image's color while the mouse button is held on it. |
| `DisabledColor` | color (RGBA) | `(0.1065, 0.1193, 0.1473, 0.6)` | 0 to 1 | The image's color while the button is not interactable. |
