using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Transform components: local and world transforms, Euler angles and directions.</summary>
public sealed class TransformTests : FeatureTestScript
{
	protected override void Start()
	{
		Entity parent = Entity.Create("Transform Parent");
		Entity child = Entity.Create("Transform Child");
		child.Parent = parent;
		TransformComponent transform = child.Transform;

		Check("local transforms", () =>
		{
			transform.Translation = new Vector3(1.0f, 0.0f, 0.0f);
			transform.Rotation = Quaternion.AngleAxis(90.0f, Vector3.Up);
			transform.Scale = new Vector3(2.0f);
			Assert.AreEqual(new Vector3(1.0f, 0.0f, 0.0f), transform.Translation);
			Assert.AreApproximatelyEqual(new Vector3(0.0f, 90.0f, 0.0f), transform.EulerAngles, 1e-3f);
			Assert.AreEqual(new Vector3(2.0f), transform.Scale);
			transform.EulerAngles = new Vector3(0.0f, -90.0f, 0.0f);
			Assert.AreApproximatelyEqual(1.0f, Mathf.Abs(Quaternion.Dot(Quaternion.AngleAxis(-90.0f, Vector3.Up), transform.Rotation)), 1e-5f);
			transform.Rotation = Quaternion.AngleAxis(90.0f, Vector3.Up);
		});
		Check("directions follow the rotation", () =>
		{
			Assert.AreApproximatelyEqual(Vector3.Left, transform.Forward, 1e-5f);
			Assert.AreApproximatelyEqual(Vector3.Forward, transform.Right, 1e-5f);
			Assert.AreApproximatelyEqual(Vector3.Up, transform.Up, 1e-5f);
		});
		Check("world transforms include the parent's", () =>
		{
			parent.Transform.Translation = new Vector3(0.0f, 5.0f, 0.0f);
			Assert.AreApproximatelyEqual(new Vector3(1.0f, 5.0f, 0.0f), transform.WorldTranslation, 1e-5f);
			Matrix4 world = transform.WorldTransform;
			Assert.AreApproximatelyEqual(new Vector3(1.0f, 5.0f, -2.0f), world.TransformPoint(Vector3.Right), 1e-5f);
			transform.WorldTranslation = new Vector3(3.0f, 3.0f, 3.0f);
			Assert.AreApproximatelyEqual(new Vector3(3.0f, -2.0f, 3.0f), transform.Translation, 1e-5f);
		});
		parent.Destroy();
	}
}

/// <summary>Counts the callbacks it receives; LifecycleTests adds it to an entity at run time.</summary>
public sealed class LifecycleProbe : Script
{
	public static int Created;
	public static int Updated;
	public static int FixedUpdated;
	public static int Destroyed;

	public static void Reset()
	{
		Created = 0;
		Updated = 0;
		FixedUpdated = 0;
		Destroyed = 0;
	}

	protected override void OnCreate() => Created++;

	protected override void OnUpdate(float deltaTime) => Updated++;

	protected override void OnFixedUpdate(float fixedDeltaTime) => FixedUpdated++;

	protected override void OnDestroy() => Destroyed++;
}

/// <summary>Scripts added while the scene runs start on the next frame and get every callback until their entity goes.</summary>
public sealed class LifecycleTests : FeatureTestScript
{
	private Entity? m_Probe;

	protected override void Start()
	{
		LifecycleProbe.Reset();
		Entity probe = Entity.Create("Lifecycle Probe");
		ScriptComponent script = probe.AddComponent<ScriptComponent>();
		script.ClassName = "FeatureTest.LifecycleProbe";
		Check("the script component names its class", () => Assert.AreEqual("FeatureTest.LifecycleProbe", script.ClassName));
		m_Probe = probe;
	}

	protected override bool Step(int frame, float deltaTime)
	{
		Entity probe = m_Probe!;
		if (frame == 1)
		{
			Check("the script started with the frame", () =>
			{
				Assert.AreEqual(1, LifecycleProbe.Created);
				Assert.IsNotNull(probe.GetComponent<ScriptComponent>()!.Instance);
				Assert.IsNotNull(probe.As<LifecycleProbe>());
			});
			return false;
		}
		if (probe.IsValid)
		{
			bool updated = LifecycleProbe.Updated >= 3 && LifecycleProbe.FixedUpdated > 0;
			if (!updated && frame < 120)
			{
				return false;
			}
			Check("the script updates", () =>
				Assert.IsTrue(updated, $"{LifecycleProbe.Updated} updates, {LifecycleProbe.FixedUpdated} fixed updates"));
			probe.Destroy();
			return false;
		}
		Check("the script was destroyed with its entity", () => Assert.AreEqual(1, LifecycleProbe.Destroyed));
		return true;
	}
}
