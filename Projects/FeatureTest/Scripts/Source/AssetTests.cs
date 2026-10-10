using System;
using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Loading assets, typed asset fields, asset handles and runtime materials. The scene sets the fields: the Cube
/// mesh, Materials/Painted.smat, the White texture, Audio/Beep.wav, the default font and sky, and Prefabs/Crate.sprefab.</summary>
public sealed class AssetTests : FeatureTestScript
{
	public Mesh? Mesh;
	public Material? Material;
	public Texture? Texture;
	public AudioClip? Clip;
	public Font? Font;
	public EnvironmentMap? Environment;
	public Prefab? Prefab;

	protected override void Start()
	{
		Check("assets load by path and by reference", () =>
		{
			Assert.AreEqual(Mesh, Assets.Load<Mesh>("builtin://Cube"));
			Assert.AreEqual(Material, Assets.Load<Material>("Materials/Painted.smat"));
			Assert.AreEqual(Texture, Assets.Load<Texture>("builtin://White"));
			Assert.AreEqual(Clip, Assets.Load<AudioClip>("Audio/Beep.wav"));
			Assert.AreEqual(Font, Assets.Load<Font>("builtin://DefaultFont"));
			Assert.AreEqual(Environment, Assets.Load<EnvironmentMap>("builtin://DefaultSky"));
			Assert.AreEqual(Prefab, Assets.Load<Prefab>("Prefabs/Crate.sprefab"));
		});
		Check("missing assets and assets of another type do not load", () =>
		{
			Assert.IsNull(Assets.Load<Mesh>("Meshes/Missing.glb"));
			Assert.IsNull(Assets.Load<Texture>("builtin://Cube"));
			Assert.Throws<ArgumentException>(() => Assets.Load<Asset>("builtin://Cube"));
		});
		Check("assets compare by handle and type", () =>
		{
			Mesh cube = Assets.Load<Mesh>("builtin://Cube")!;
			Mesh sphere = Assets.Load<Mesh>("builtin://Sphere")!;
			Assert.IsTrue(cube == Mesh);
			Assert.IsTrue(cube != sphere);
			Assert.IsTrue(cube.Equals(Mesh));
			Assert.IsFalse(cube.Equals(sphere));
			Assert.IsTrue(cube.Handle.IsValid);
		});
		Check("asset handles", () =>
		{
			AssetHandle handle = Mesh!.Handle;
			AssetHandle copy = new(handle.ID);
			Assert.IsTrue(copy == handle);
			Assert.IsFalse(copy != handle);
			Assert.IsTrue(copy.Equals(handle));
			Assert.IsTrue(handle.IsValid);
			Assert.IsFalse(AssetHandle.Invalid.IsValid);
			Assert.AreEqual(0ul, AssetHandle.Invalid.ID);
		});
		Check("project materials are shared and read-only", ProjectMaterialChecks);
		Check("runtime materials are created and edited", RuntimeMaterialChecks);
	}

	private void ProjectMaterialChecks()
	{
		Material painted = Material!;
		Assert.IsFalse(painted.IsEditable);
		float roughness = painted.Roughness;
		// Logged and ignored: scripts change copies.
		painted.Roughness = 0.95f;
		Assert.AreEqual(roughness, painted.Roughness);
	}

	private void RuntimeMaterialChecks()
	{
		Material painted = Material!;
		Material? copy = painted.Clone();
		Assert.IsNotNull(copy);
		Assert.IsTrue(copy.IsEditable);
		Assert.AreNotEqual(painted, copy);
		Assert.AreEqual(painted.BaseColor, copy.BaseColor);
		Assert.AreEqual(painted.Roughness, copy.Roughness);

		Material? created = Material.Create();
		Assert.IsNotNull(created);
		Assert.IsTrue(created.IsEditable);
		created.BaseColor = new Color(0.2f, 0.4f, 0.6f, 1.0f);
		Assert.AreEqual(new Color(0.2f, 0.4f, 0.6f, 1.0f), created.BaseColor);
		created.Metallic = 0.25f;
		Assert.AreEqual(0.25f, created.Metallic);
		created.Roughness = 0.75f;
		Assert.AreEqual(0.75f, created.Roughness);
		created.Emissive = new Color(1.0f, 0.5f, 0.0f);
		Assert.AreEqual(new Color(1.0f, 0.5f, 0.0f), created.Emissive);
		created.EmissiveIntensity = 3.0f;
		Assert.AreEqual(3.0f, created.EmissiveIntensity);
		created.NormalStrength = 0.5f;
		Assert.AreEqual(0.5f, created.NormalStrength);
		created.OcclusionStrength = 0.8f;
		Assert.AreEqual(0.8f, created.OcclusionStrength);
		created.BaseColorTexture = Texture;
		Assert.AreEqual(Texture, created.BaseColorTexture);
		Texture? flatNormal = Assets.Load<Texture>("builtin://FlatNormal");
		created.NormalTexture = flatNormal;
		Assert.AreEqual(flatNormal, created.NormalTexture);
		created.MetallicRoughnessTexture = Texture;
		Assert.AreEqual(Texture, created.MetallicRoughnessTexture);
		created.OcclusionTexture = Texture;
		Assert.AreEqual(Texture, created.OcclusionTexture);
		created.EmissiveTexture = Texture;
		Assert.AreEqual(Texture, created.EmissiveTexture);
		created.EmissiveTexture = null;
		Assert.IsNull(created.EmissiveTexture);
		created.AlphaMode = MaterialAlphaMode.Mask;
		Assert.AreEqual(MaterialAlphaMode.Mask, created.AlphaMode);
		created.AlphaCutoff = 0.3f;
		Assert.AreEqual(0.3f, created.AlphaCutoff);
		created.DoubleSided = true;
		Assert.IsTrue(created.DoubleSided);
		created.UVTiling = new Vector2(2.0f, 3.0f);
		Assert.AreEqual(new Vector2(2.0f, 3.0f), created.UVTiling);
		created.UVOffset = new Vector2(0.5f, 0.25f);
		Assert.AreEqual(new Vector2(0.5f, 0.25f), created.UVOffset);
	}
}
