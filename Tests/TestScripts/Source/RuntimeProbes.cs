using System;
using System.Globalization;
using Strada;
using Strada.Testing;

namespace Strada.Tests;

// Loads assets and makes and changes materials; the engine tests read the results from the name and the mesh's
// material overrides (0: the created material, 1: the clone of the default material).
public sealed class AssetProbe : Script
{
	protected override void OnCreate()
	{
		Mesh? cube = Assets.Load<Mesh>("builtin://Cube");
		Mesh? missing = Assets.Load<Mesh>("Meshes/Missing.glb");
		Material? wrongType = Assets.Load<Material>("builtin://Cube");
		Texture? white = Assets.Load<Texture>("builtin://White");
		Material shared = Assets.Load<Material>("builtin://DefaultMaterial")!;

		Material created = Material.Create()!;
		created.BaseColor = new Color(1.0f, 0.0f, 0.0f, 0.5f);
		created.Metallic = 0.75f;
		created.Roughness = 0.25f;
		created.Emissive = new Color(0.0f, 1.0f, 0.0f);
		created.EmissiveIntensity = 3.0f;
		created.NormalStrength = 0.5f;
		created.OcclusionStrength = 0.25f;
		created.BaseColorTexture = white;
		created.NormalTexture = white;
		created.MetallicRoughnessTexture = white;
		created.OcclusionTexture = white;
		created.EmissiveTexture = white;
		created.AlphaMode = MaterialAlphaMode.Blend;
		created.AlphaCutoff = 0.3f;
		created.DoubleSided = true;
		created.UVTiling = new Vector2(2.0f, 3.0f);
		created.UVOffset = new Vector2(0.5f, 0.25f);
		created.Metallic = float.NaN;
		created.AlphaMode = (MaterialAlphaMode)7;

		Material clone = shared.Clone()!;
		shared.Roughness = 0.9f;

		MeshComponent mesh = GetComponent<MeshComponent>()!;
		mesh.SetMaterial(0, created);
		mesh.SetMaterial(1, clone);

		Name = string.Create(CultureInfo.InvariantCulture,
			$"Cube={cube != null};Missing={missing == null};WrongType={wrongType == null};SharedEditable={shared.IsEditable};"
			+ $"CreatedEditable={created.IsEditable};CloneEditable={clone.IsEditable};SharedUnchanged={shared.Roughness == clone.Roughness};"
			+ $"Metallic={created.Metallic};Blend={created.AlphaMode == MaterialAlphaMode.Blend};"
			+ $"Texture={created.EmissiveTexture == white};Emissive={created.Emissive}");
	}
}

// A script of the prefab the engine tests instantiate.
public sealed class PrefabMember : Script
{
	public int Value;
	public bool Created;

	protected override void OnCreate()
	{
		Created = true;
	}
}

// Instantiates its Template under itself and at the root.
public sealed class PrefabProbe : Script
{
	public Prefab? Template;

	protected override void OnCreate()
	{
		Entity? instance = Entity.Instantiate(Template!, new Vector3(1.0f, 2.0f, 3.0f), Quaternion.AngleAxis(90.0f, Vector3.Up), this);
		Entity? rootInstance = Entity.Instantiate(Template!, Vector3.Zero, Quaternion.Identity);
		PrefabMember? member = instance as PrefabMember;
		Name = string.Create(CultureInfo.InvariantCulture,
			$"Started={member?.Created};Value={member?.Value};Parent={instance?.Parent == this};Children={instance?.Children.Length};"
			+ $"AtRoot={rootInstance != null && rootInstance.Parent == null};Distinct={instance != rootInstance}");
	}
}

// Talks to the application: reads it, asks it to load scenes and quit, reports tests, draws debug lines and throws on
// request (Q quits, F finishes the tests, T throws).
public sealed class HostProbe : Script
{
	protected override void OnCreate()
	{
		Name = string.Create(CultureInfo.InvariantCulture,
			$"Editor={Application.IsEditor};Size={Application.WindowWidth}x{Application.WindowHeight};"
			+ $"Scene={SceneManager.CurrentSceneName};Load={SceneManager.LoadScene("Scenes/Next.sscene")};"
			+ $"Missing={SceneManager.LoadScene("Scenes/Missing.sscene")};Wrong={SceneManager.LoadScene("builtin://Cube")}");

		TestReporter.Pass("passes");
		TestReporter.Fail("fails", "on purpose");
		TestReporter.Run("asserts", () => Assert.AreEqual(1, 2));
		TestReporter.Run("throws", () => throw new InvalidOperationException("boom"));
		TestReporter.Run("checks", () => Assert.IsTrue(true));

		Debug.DrawLine(Vector3.Zero, Vector3.Up, Color.Red);
		Debug.DrawLine(Vector3.Zero, Vector3.Right, new Color(0.0f, 0.5f, 0.0f), 1.0f);
		Debug.DrawLine(Vector3.Zero, new Vector3(float.NaN, 0.0f, 0.0f), Color.Blue);
		Debug.DrawLine(Vector3.Zero, Vector3.Forward, Color.Blue, -1.0f);
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsKeyPressed(KeyCode.Q))
		{
			Application.Quit();
		}
		if (Input.IsKeyPressed(KeyCode.F))
		{
			TestReporter.Finish();
		}
		if (Input.IsKeyPressed(KeyCode.T))
		{
			throw new InvalidOperationException("thrown on purpose");
		}
	}
}

// Sets values the engine rejects: non-finite numbers and unknown enumerators keep the previous values; a zero rotation
// is the identity.
public sealed class ValueGuard : Script
{
	protected override void OnCreate()
	{
		Translation = new Vector3(1.0f, 2.0f, 3.0f);
		Translation = new Vector3(float.NaN, 0.0f, 0.0f);
		Transform.EulerAngles = new Vector3(float.PositiveInfinity, 0.0f, 0.0f);
		Rotation = new Quaternion(0.0f, 0.0f, 0.0f, 0.0f);
		CameraComponent camera = AddComponent<CameraComponent>();
		camera.OrthographicSize = 5.0f;
		camera.OrthographicSize = float.NegativeInfinity;
		camera.Projection = (ProjectionType)42;
		RigidBodyComponent body = AddComponent<RigidBodyComponent>();
		body.Mass = 2.0f;
		body.Mass = float.NaN;
		body.LinearVelocity = new Vector3(float.NaN, 0.0f, 0.0f);
		body.AddForce(new Vector3(0.0f, float.NaN, 0.0f), ForceMode.Impulse);
		body.Teleport(new Vector3(float.NaN, 0.0f, 0.0f), Quaternion.Identity);
		Physics.Gravity = new Vector3(0.0f, float.NaN, 0.0f);
		bool hit = Physics.Raycast(Vector3.Zero, Vector3.Down, float.PositiveInfinity, out _);
		Name = string.Create(CultureInfo.InvariantCulture, $"Translation={Translation};Rotation={Rotation};Hit={hit}");
	}
}
