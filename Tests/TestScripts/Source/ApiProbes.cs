using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace Strada.Tests;

// Sets every component field through the API, reads each back (logging "Mismatch" when it differs) and names its entity
// "Checked <count>"; the engine tests then read the fields natively. The entity may already have components with asset
// references, whose handles arrive in the Expected* fields.
public sealed class ComponentWriter : Script
{
	public AssetHandle ExpectedMesh;
	public AssetHandle ExpectedClip;
	public AssetHandle ExpectedFont;
	private int m_Checked;

	protected override void OnCreate()
	{
		CameraComponent camera = AddComponent<CameraComponent>();
		Check(() => camera.Projection, value => camera.Projection = value, ProjectionType.Orthographic, "Camera.Projection");
		Check(() => camera.PerspectiveFOV, value => camera.PerspectiveFOV = value, 50.0f, "Camera.PerspectiveFOV");
		Check(() => camera.PerspectiveNear, value => camera.PerspectiveNear = value, 0.5f, "Camera.PerspectiveNear");
		Check(() => camera.PerspectiveFar, value => camera.PerspectiveFar = value, 500.0f, "Camera.PerspectiveFar");
		Check(() => camera.OrthographicSize, value => camera.OrthographicSize = value, 20.0f, "Camera.OrthographicSize");
		Check(() => camera.OrthographicNear, value => camera.OrthographicNear = value, -50.0f, "Camera.OrthographicNear");
		Check(() => camera.OrthographicFar, value => camera.OrthographicFar = value, 60.0f, "Camera.OrthographicFar");
		Check(() => camera.Primary, value => camera.Primary = value, false, "Camera.Primary");
		Check(() => camera.FixedAspectRatio, value => camera.FixedAspectRatio = value, true, "Camera.FixedAspectRatio");
		Check(() => camera.AspectRatio, value => camera.AspectRatio = value, 2.0f, "Camera.AspectRatio");

		MeshComponent mesh = AddComponent<MeshComponent>();
		Expect(mesh.Mesh?.Handle ?? AssetHandle.Invalid, ExpectedMesh, "Mesh.Mesh");
		Check(() => mesh.CastShadows, value => mesh.CastShadows = value, false, "Mesh.CastShadows");
		Check(() => mesh.Visible, value => mesh.Visible = value, false, "Mesh.Visible");
		Expect(mesh.MaterialCount, 0, "Mesh.MaterialCount");
		Expect(mesh.GetMaterial(3) == null, true, "Mesh.GetMaterial");

		DirectionalLightComponent sun = AddComponent<DirectionalLightComponent>();
		Check(() => sun.Color, value => sun.Color = value, new Color(1.0f, 0.5f, 0.25f), "DirectionalLight.Color");
		Check(() => sun.Intensity, value => sun.Intensity = value, 4.0f, "DirectionalLight.Intensity");
		Check(() => sun.CastShadows, value => sun.CastShadows = value, false, "DirectionalLight.CastShadows");
		Check(() => sun.LightSize, value => sun.LightSize = value, 2.0f, "DirectionalLight.LightSize");

		PointLightComponent point = AddComponent<PointLightComponent>();
		Check(() => point.Color, value => point.Color = value, new Color(0.5f, 0.25f, 1.0f), "PointLight.Color");
		Check(() => point.Intensity, value => point.Intensity = value, 7.0f, "PointLight.Intensity");
		Check(() => point.Range, value => point.Range = value, 12.0f, "PointLight.Range");
		Check(() => point.CastShadows, value => point.CastShadows = value, true, "PointLight.CastShadows");

		SpotLightComponent spot = AddComponent<SpotLightComponent>();
		Check(() => spot.Color, value => spot.Color = value, new Color(0.25f, 1.0f, 0.5f), "SpotLight.Color");
		Check(() => spot.Intensity, value => spot.Intensity = value, 8.0f, "SpotLight.Intensity");
		Check(() => spot.Range, value => spot.Range = value, 15.0f, "SpotLight.Range");
		Check(() => spot.InnerConeAngle, value => spot.InnerConeAngle = value, 10.0f, "SpotLight.InnerConeAngle");
		Check(() => spot.OuterConeAngle, value => spot.OuterConeAngle = value, 25.0f, "SpotLight.OuterConeAngle");
		Check(() => spot.CastShadows, value => spot.CastShadows = value, true, "SpotLight.CastShadows");

		SkyLightComponent sky = AddComponent<SkyLightComponent>();
		Expect(sky.Environment == null, true, "SkyLight.Environment");
		Check(() => sky.Intensity, value => sky.Intensity = value, 0.5f, "SkyLight.Intensity");
		Check(() => sky.Rotation, value => sky.Rotation = value, 90.0f, "SkyLight.Rotation");
		Check(() => sky.SkyboxBlur, value => sky.SkyboxBlur = value, 0.25f, "SkyLight.SkyboxBlur");
		Check(() => sky.DrawSkybox, value => sky.DrawSkybox = value, false, "SkyLight.DrawSkybox");
		Check(() => sky.AmbientColor, value => sky.AmbientColor = value, new Color(0.1f, 0.2f, 0.3f), "SkyLight.AmbientColor");

		RigidBodyComponent body = AddComponent<RigidBodyComponent>();
		Check(() => body.Type, value => body.Type = value, RigidBodyType.Kinematic, "RigidBody.Type");
		Check(() => body.Mass, value => body.Mass = value, 3.0f, "RigidBody.Mass");
		Check(() => body.Layer, value => body.Layer = value, 2u, "RigidBody.Layer");
		Check(() => body.GravityFactor, value => body.GravityFactor = value, 0.5f, "RigidBody.GravityFactor");
		Check(() => body.LinearDamping, value => body.LinearDamping = value, 0.2f, "RigidBody.LinearDamping");
		Check(() => body.AngularDamping, value => body.AngularDamping = value, 0.3f, "RigidBody.AngularDamping");
		Check(() => body.LinearVelocity, value => body.LinearVelocity = value, new Vector3(1.0f, 2.0f, 3.0f), "RigidBody.LinearVelocity");
		Check(() => body.AngularVelocity, value => body.AngularVelocity = value, new Vector3(4.0f, 5.0f, 6.0f), "RigidBody.AngularVelocity");

		BoxColliderComponent box = AddComponent<BoxColliderComponent>();
		Check(() => box.HalfExtents, value => box.HalfExtents = value, new Vector3(1.0f, 2.0f, 3.0f), "BoxCollider.HalfExtents");
		Check(() => box.Offset, value => box.Offset = value, new Vector3(0.5f, 0.0f, 0.0f), "BoxCollider.Offset");
		Check(() => box.IsTrigger, value => box.IsTrigger = value, true, "BoxCollider.IsTrigger");
		Check(() => box.Friction, value => box.Friction = value, 0.25f, "BoxCollider.Friction");
		Check(() => box.Restitution, value => box.Restitution = value, 0.75f, "BoxCollider.Restitution");

		SphereColliderComponent sphere = AddComponent<SphereColliderComponent>();
		Check(() => sphere.Radius, value => sphere.Radius = value, 2.5f, "SphereCollider.Radius");
		Check(() => sphere.Offset, value => sphere.Offset = value, new Vector3(0.0f, 1.0f, 0.0f), "SphereCollider.Offset");
		Check(() => sphere.IsTrigger, value => sphere.IsTrigger = value, true, "SphereCollider.IsTrigger");
		Check(() => sphere.Friction, value => sphere.Friction = value, 0.1f, "SphereCollider.Friction");
		Check(() => sphere.Restitution, value => sphere.Restitution = value, 0.9f, "SphereCollider.Restitution");

		CapsuleColliderComponent capsule = AddComponent<CapsuleColliderComponent>();
		Check(() => capsule.Radius, value => capsule.Radius = value, 0.25f, "CapsuleCollider.Radius");
		Check(() => capsule.HalfHeight, value => capsule.HalfHeight = value, 1.5f, "CapsuleCollider.HalfHeight");
		Check(() => capsule.Offset, value => capsule.Offset = value, new Vector3(0.0f, 0.0f, 1.0f), "CapsuleCollider.Offset");
		Check(() => capsule.IsTrigger, value => capsule.IsTrigger = value, true, "CapsuleCollider.IsTrigger");
		Check(() => capsule.Friction, value => capsule.Friction = value, 0.2f, "CapsuleCollider.Friction");
		Check(() => capsule.Restitution, value => capsule.Restitution = value, 0.3f, "CapsuleCollider.Restitution");

		MeshColliderComponent meshCollider = AddComponent<MeshColliderComponent>();
		meshCollider.Mesh = mesh.Mesh;
		Expect(meshCollider.Mesh?.Handle ?? AssetHandle.Invalid, ExpectedMesh, "MeshCollider.Mesh");
		Check(() => meshCollider.Convex, value => meshCollider.Convex = value, false, "MeshCollider.Convex");
		Check(() => meshCollider.IsTrigger, value => meshCollider.IsTrigger = value, true, "MeshCollider.IsTrigger");
		Check(() => meshCollider.Friction, value => meshCollider.Friction = value, 0.4f, "MeshCollider.Friction");
		Check(() => meshCollider.Restitution, value => meshCollider.Restitution = value, 0.6f, "MeshCollider.Restitution");

		AudioSourceComponent audio = AddComponent<AudioSourceComponent>();
		Expect(audio.Clip?.Handle ?? AssetHandle.Invalid, ExpectedClip, "AudioSource.Clip");
		audio.Clip = null;
		Expect(audio.Clip == null, true, "AudioSource.Clip cleared");
		Check(() => audio.Volume, value => audio.Volume = value, 0.5f, "AudioSource.Volume");
		Check(() => audio.Pitch, value => audio.Pitch = value, 1.5f, "AudioSource.Pitch");
		Check(() => audio.Loop, value => audio.Loop = value, true, "AudioSource.Loop");
		Check(() => audio.PlayOnStart, value => audio.PlayOnStart = value, true, "AudioSource.PlayOnStart");
		Check(() => audio.Spatial, value => audio.Spatial = value, false, "AudioSource.Spatial");
		Check(() => audio.MinDistance, value => audio.MinDistance = value, 2.0f, "AudioSource.MinDistance");
		Check(() => audio.MaxDistance, value => audio.MaxDistance = value, 50.0f, "AudioSource.MaxDistance");

		AudioListenerComponent listener = AddComponent<AudioListenerComponent>();
		Check(() => listener.Active, value => listener.Active = value, false, "AudioListener.Active");

		TextComponent text = AddComponent<TextComponent>();
		Expect(text.Font?.Handle ?? AssetHandle.Invalid, ExpectedFont, "Text.Font");
		text.Font = null;
		Check(() => text.Text, value => text.Text = value, "Héllo ✓", "Text.Text");
		Check(() => text.Color, value => text.Color = value, new Color(1.0f, 0.0f, 0.0f, 0.5f), "Text.Color");
		Check(() => text.FontSize, value => text.FontSize = value, 32.0f, "Text.FontSize");
		Check(() => text.ScreenSpace, value => text.ScreenSpace = value, true, "Text.ScreenSpace");
		Check(() => text.Alignment, value => text.Alignment = value, TextAlignment.Center, "Text.Alignment");
		Check(() => text.LineSpacing, value => text.LineSpacing = value, 1.5f, "Text.LineSpacing");

		SpriteRendererComponent sprite = AddComponent<SpriteRendererComponent>();
		Expect(sprite.Texture == null, true, "SpriteRenderer.Texture");
		Check(() => sprite.Color, value => sprite.Color = value, new Color(0.0f, 1.0f, 0.0f), "SpriteRenderer.Color");
		Check(() => sprite.Tiling, value => sprite.Tiling = value, 4.0f, "SpriteRenderer.Tiling");
		Check(() => sprite.ScreenSpace, value => sprite.ScreenSpace = value, true, "SpriteRenderer.ScreenSpace");

		ScriptComponent script = GetComponent<ScriptComponent>()!;
		Expect(script.ClassName, "Strada.Tests.ComponentWriter", "Script.ClassName");
		Expect(ReferenceEquals(script.Instance, this), true, "Script.Instance");

		Name = string.Create(CultureInfo.InvariantCulture, $"Checked {m_Checked}");
	}

	private void Check<T>(Func<T> get, Action<T> set, T value, string field)
	{
		set(value);
		Expect(get(), value, field);
	}

	private void Expect<T>(T actual, T expected, string field)
	{
		m_Checked++;
		if (!EqualityComparer<T>.Default.Equals(actual, expected))
		{
			Log.Error($"Mismatch: {field} is {actual}, expected {expected}");
		}
	}
}

// Builds a small hierarchy and reports what it saw in its name.
public sealed class HierarchyProbe : Script
{
	protected override void OnCreate()
	{
		Entity parent = Create("Probe Parent");
		Entity child = Create("Probe Child");
		Entity second = Create("Probe Second");
		child.Parent = parent;
		second.Parent = parent;
		string children = string.Join("+", parent.Children.Select(entity => entity.Name));
		second.Parent = null;
		// Scripted entities come back as their instances.
		Entity? self = FindByID(ID);
		bool sameInstance = ReferenceEquals(self, this) && self?.As<HierarchyProbe>() == this && self.As<Mover>() == null;
		AddComponent<PointLightComponent>();
		bool added = HasComponent<PointLightComponent>();
		RemoveComponent<PointLightComponent>();
		bool removed = !HasComponent<PointLightComponent>();
		// Refused: every entity keeps its transform.
		RemoveComponent<TransformComponent>();
		Name = $"Children={children};Self={sameInstance};Added={added};Removed={removed};ChildParent={child.Parent?.Name};"
			+ $"SecondParent={second.Parent?.Name ?? "none"};Transform={HasComponent<TransformComponent>()};Valid={child.IsValid}";
	}
}

// Child of a rotated Holder: reports its world-space values, then moves in world space and turns.
public sealed class TransformProbe : Script
{
	protected override void OnCreate()
	{
		TransformComponent transform = Transform;
		Vector3 world = transform.WorldTranslation;
		Vector3 fromMatrix = transform.WorldTransform.TransformPoint(Vector3.Zero);
		Name = string.Create(CultureInfo.InvariantCulture,
			$"World={Round(world)};Matrix={Round(fromMatrix)};Forward={Round(transform.Forward)};Right={Round(transform.Right)};Up={Round(transform.Up)}");
		transform.WorldTranslation = Vector3.Zero;
		transform.EulerAngles = new Vector3(0.0f, 45.0f, 0.0f);
	}

	private static Vector3 Round(Vector3 value) => new(MathF.Round(value.X, 3) + 0.0f, MathF.Round(value.Y, 3) + 0.0f, MathF.Round(value.Z, 3) + 0.0f);
}

// Drives physics from a script: a body added this frame takes a velocity change at once.
public sealed class PhysicsProbe : Script
{
	protected override void OnCreate()
	{
		RigidBodyComponent body = AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType.Dynamic;
		AddComponent<SphereColliderComponent>().Radius = 0.5f;
		body.AddForce(new Vector3(0.0f, 10.0f, 0.0f), ForceMode.VelocityChange);
		float velocity = body.LinearVelocity.Y;
		bool hit = Physics.Raycast(new Vector3(0.0f, 5.0f, 5.0f), new Vector3(0.0f, -1.0f, 0.0f), 100.0f, out RaycastHit result);
		Vector3 gravity = Physics.Gravity;
		Physics.Gravity = new Vector3(0.0f, -20.0f, 0.0f);
		// Logged: only kinematic bodies move kinematically.
		body.MoveKinematic(Vector3.Zero, Quaternion.Identity);
		body.Teleport(new Vector3(3.0f, 4.0f, 5.0f), Quaternion.Identity);
		body.WakeUp();
		Name = string.Create(CultureInfo.InvariantCulture,
			$"Velocity={velocity};Hit={hit};HitEntity={result.Entity?.Name};Distance={MathF.Round(result.Distance, 3)};Gravity={gravity.Y};"
			+ $"Sleeping={body.IsSleeping}");
	}
}

// Records input and time each update, and lists the key codes it knows.
public sealed class InputProbe : Script
{
	public Entity? Recorder;
	public float NextTimeScale = 1.0f;

	protected override void OnCreate()
	{
		if (Recorder != null)
		{
			Recorder.Name = string.Join(",", Enum.GetValues<KeyCode>().Select(key => string.Create(CultureInfo.InvariantCulture, $"{key}={(int)key}")));
		}
	}

	protected override void OnUpdate(float deltaTime)
	{
		Name = string.Create(CultureInfo.InvariantCulture,
			$"Down={Input.IsKeyDown(KeyCode.A)};Pressed={Input.IsKeyPressed(KeyCode.A)};Released={Input.IsKeyReleased(KeyCode.A)};"
			+ $"Mouse={Input.MousePosition};Button={Input.IsMouseButtonDown(MouseButton.Left)};Gamepad={Input.IsGamepadConnected()};"
			+ $"Delta={deltaTime};Fixed={Time.FixedDeltaTime};Frames={Time.FrameCount};Elapsed={Time.Elapsed}");
		Time.TimeScale = NextTimeScale;
	}
}

// Plays its AudioSource from OnCreate.
public sealed class AudioProbe : Script
{
	protected override void OnCreate()
	{
		AudioSourceComponent source = GetComponent<AudioSourceComponent>()!;
		bool before = source.IsPlaying;
		source.Play();
		Name = $"Before={before};Playing={source.IsPlaying}";
	}

	protected override void OnUpdate(float deltaTime)
	{
		AudioSourceComponent source = GetComponent<AudioSourceComponent>()!;
		if (Input.IsKeyPressed(KeyCode.P))
		{
			source.Pause();
		}
		if (Input.IsKeyPressed(KeyCode.S))
		{
			source.Stop();
		}
		Name = $"Playing={source.IsPlaying}";
	}
}
