using System.Reflection;
using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>The attributes: serialized private fields, hidden fields, ranges and tooltips (the scene stores this script's field
/// values, so they arrive changed from their defaults), and [VisualNode], which makes game members nodes of visual
/// scripts.</summary>
public sealed class AttributeTests : FeatureTestScript
{
	[Tooltip("A public field with a range, stored by the scene as 7")]
	[Range(0.0f, 10.0f)]
	public float Strength = 1.0f;

	[SerializeField]
	private int m_Count = 1;

	[HideInInspector]
	public string Hidden = "default";

	protected override void Start()
	{
		Check("the scene's values arrive in the fields", () =>
		{
			Assert.AreEqual(7.0f, Strength);
			Assert.AreEqual(3, m_Count);
			Assert.AreEqual("stored", Hidden);
		});
		Check("the attributes describe the fields", () =>
		{
			FieldInfo strength = typeof(AttributeTests).GetField(nameof(Strength))!;
			RangeAttribute range = strength.GetCustomAttribute<RangeAttribute>()!;
			Assert.AreEqual(0.0f, range.Min);
			Assert.AreEqual(10.0f, range.Max);
			Assert.AreEqual("A public field with a range, stored by the scene as 7", strength.GetCustomAttribute<TooltipAttribute>()!.Text);
			FieldInfo count = typeof(AttributeTests).GetField(nameof(m_Count), BindingFlags.NonPublic | BindingFlags.Instance)!;
			Assert.IsNotNull(count.GetCustomAttribute<SerializeFieldAttribute>());
			Assert.IsNotNull(typeof(AttributeTests).GetField(nameof(Hidden))!.GetCustomAttribute<HideInInspectorAttribute>());
		});
		Check("[VisualNode] keeps what it is given", () =>
		{
			VisualNodeAttribute empty = new();
			Assert.AreEqual(string.Empty, empty.Title);
			Assert.AreEqual(0, empty.Keywords.Length);
			Assert.IsFalse(empty.Pure);
			VisualNodeAttribute attribute = new()
			{
				Title = "Deal Damage",
				Category = "Game/Combat",
				Description = "Hurts the target.",
				Keywords = ["hit", "hurt"],
				Pure = true,
				FormerNames = ["Hit"],
			};
			Assert.AreEqual("Deal Damage", attribute.Title);
			Assert.AreEqual("Game/Combat", attribute.Category);
			Assert.AreEqual("Hurts the target.", attribute.Description);
			Assert.AreEqual("hit, hurt", string.Join(", ", attribute.Keywords));
			Assert.IsTrue(attribute.Pure);
			Assert.AreEqual("Hit", attribute.FormerNames[0]);
		});
	}
}
