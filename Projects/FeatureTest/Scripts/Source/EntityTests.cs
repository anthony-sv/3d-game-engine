using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Entities: creating, naming, finding and destroying them, components, the hierarchy, scripts through their
/// entities and prefab instances. The scene sets Crate (Prefabs/Crate.sprefab) and Neighbor (the Sun).</summary>
public sealed class EntityTests : FeatureTestScript
{
	public Prefab? Crate;
	public Entity? Neighbor;

	private Entity? m_Spawned;
	private Entity? m_Detached;

	protected override void Start()
	{
		Entity spawned = Entity.Create("Spawned Entity");
		m_Spawned = spawned;
		Check("entities are created, named and found", () =>
		{
			Assert.IsTrue(spawned.IsValid);
			Assert.AreNotEqual(0ul, spawned.ID);
			Assert.AreEqual("Spawned Entity", spawned.Name);
			spawned.Name = "Renamed Entity";
			Assert.AreEqual("Renamed Entity", spawned.Name);
			Assert.IsTrue(Entity.FindByName("Renamed Entity") == spawned);
			Assert.IsTrue(Entity.FindByID(spawned.ID)!.Equals(spawned));
			Assert.IsNull(Entity.FindByName("No Such Entity"));
			Assert.IsNull(Entity.FindByID(0));
			Assert.IsTrue(spawned != this);
		});
		Check("the transform through the entity", () =>
		{
			spawned.Translation = new Vector3(1.0f, 2.0f, 3.0f);
			spawned.Rotation = Quaternion.AngleAxis(45.0f, Vector3.Up);
			spawned.Scale = new Vector3(2.0f);
			Assert.AreEqual(new Vector3(1.0f, 2.0f, 3.0f), spawned.Translation);
			Assert.AreApproximatelyEqual(1.0f, Mathf.Abs(Quaternion.Dot(Quaternion.AngleAxis(45.0f, Vector3.Up), spawned.Rotation)));
			Assert.AreEqual(new Vector3(2.0f), spawned.Scale);
			Assert.AreEqual(spawned.Translation, spawned.Transform.Translation);
		});
		Check("components are added, found and removed", () =>
		{
			Assert.IsTrue(spawned.HasComponent<TransformComponent>());
			Assert.IsFalse(spawned.HasComponent<PointLightComponent>());
			Assert.IsNull(spawned.GetComponent<PointLightComponent>());
			PointLightComponent light = spawned.AddComponent<PointLightComponent>();
			Assert.IsTrue(light.Entity == spawned);
			Assert.IsNotNull(spawned.GetComponent<PointLightComponent>());
			spawned.RemoveComponent<PointLightComponent>();
			Assert.IsFalse(spawned.HasComponent<PointLightComponent>());
		});
		Check("entities form a hierarchy", () =>
		{
			Entity child = Entity.Create("Spawned Child");
			child.Parent = spawned;
			Assert.IsTrue(child.Parent == spawned);
			Entity[] children = spawned.Children;
			Assert.AreEqual(1, children.Length);
			Assert.IsTrue(children[0] == child);
			child.Parent = null;
			Assert.IsNull(child.Parent);
			Assert.AreEqual(0, spawned.Children.Length);
			m_Detached = child;
		});
		Check("scripts are found through their entities", () =>
		{
			Assert.IsTrue(As<EntityTests>() == this);
			Assert.IsNull(As<MathTests>());
			Assert.IsNotNull(Entity.FindByName("Math Tests")?.As<MathTests>());
		});
		Check("entity fields refer to entities of the scene", () =>
		{
			Assert.IsNotNull(Neighbor);
			Assert.AreEqual("Sun", Neighbor.Name);
		});
		Check("prefabs are instantiated", () =>
		{
			Quaternion turn = Quaternion.AngleAxis(30.0f, Vector3.Up);
			Entity? instance = Entity.Instantiate(Crate!, new Vector3(-6.0f, 3.0f, -4.0f), turn);
			Assert.IsNotNull(instance);
			Assert.AreEqual("Crate", instance.Name);
			Assert.AreApproximatelyEqual(new Vector3(-6.0f, 3.0f, -4.0f), instance.Translation, 1e-4f);
			Assert.AreApproximatelyEqual(1.0f, Mathf.Abs(Quaternion.Dot(turn, instance.Rotation)), 1e-5f);
			Entity? child = Entity.Instantiate(Crate!, spawned.Translation, Quaternion.Identity, spawned);
			Assert.IsNotNull(child);
			Assert.IsTrue(child.Parent == spawned);
		});
	}

	protected override bool Step(int frame, float deltaTime)
	{
		Entity spawned = m_Spawned!;
		Entity detached = m_Detached!;
		if (frame == 1)
		{
			// Destroyed entities stay until the end of the frame.
			spawned.Destroy();
			detached.Destroy();
			Check("destroyed entities last until the end of the frame", () =>
			{
				Assert.IsTrue(spawned.IsValid);
				Assert.IsTrue(detached.IsValid);
			});
			return false;
		}
		Check("destroyed entities are gone with their children", () =>
		{
			Assert.IsFalse(spawned.IsValid);
			Assert.IsFalse(detached.IsValid);
			Assert.IsNull(Entity.FindByName("Renamed Entity"));
			Assert.IsNull(Entity.FindByName("Spawned Child"));
		});
		return true;
	}
}
