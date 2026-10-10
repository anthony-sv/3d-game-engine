using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Cameras, lights, meshes, text, sprites, audio listeners and script components: every property is written and
/// read back, and the scene's values come first.</summary>
public sealed class ComponentTests : FeatureTestScript
{
	protected override void Start()
	{
		Check("cameras", CameraChecks);
		Check("lights", LightChecks);
		Check("sky lights", SkyChecks);
		Check("meshes", MeshChecks);
		Check("text", TextChecks);
		Check("sprites", SpriteChecks);
		Check("audio listeners", () =>
		{
			AudioListenerComponent listener = Find<AudioListenerComponent>("Main Camera");
			Assert.IsTrue(listener.Active);
			listener.Active = false;
			Assert.IsFalse(listener.Active);
			listener.Active = true;
		});
		Check("script components", () =>
		{
			ScriptComponent script = GetComponent<ScriptComponent>()!;
			Assert.AreEqual("FeatureTest.ComponentTests", script.ClassName);
			Assert.IsTrue(script.Instance == this);
		});
	}

	private static T Find<T>(string entityName)
		where T : Component
	{
		T? component = Entity.FindByName(entityName)?.GetComponent<T>();
		Assert.IsNotNull(component, $"{entityName} has no {typeof(T).Name}");
		return component;
	}

	private static void CameraChecks()
	{
		CameraComponent camera = Find<CameraComponent>("Main Camera");
		Assert.IsTrue(camera.Primary);
		Assert.AreEqual(ProjectionType.Perspective, camera.Projection);

		// The middle of the view looks along the camera.
		Ray ray = camera.ScreenToWorldRay(new Vector2(Application.WindowWidth * 0.5f, Application.WindowHeight * 0.5f));
		TransformComponent transform = camera.Entity.Transform;
		Assert.AreApproximatelyEqual(transform.Forward, ray.Direction, 1e-3f);
		Assert.IsTrue(Vector3.Distance(transform.WorldTranslation, ray.Origin) < camera.PerspectiveNear + 1e-3f);

		camera.PerspectiveFOV = 60.0f;
		Assert.AreEqual(60.0f, camera.PerspectiveFOV);
		camera.PerspectiveNear = 0.2f;
		Assert.AreEqual(0.2f, camera.PerspectiveNear);
		camera.PerspectiveFar = 500.0f;
		Assert.AreEqual(500.0f, camera.PerspectiveFar);
		camera.OrthographicSize = 8.0f;
		Assert.AreEqual(8.0f, camera.OrthographicSize);
		camera.OrthographicNear = -5.0f;
		Assert.AreEqual(-5.0f, camera.OrthographicNear);
		camera.OrthographicFar = 50.0f;
		Assert.AreEqual(50.0f, camera.OrthographicFar);
		camera.Projection = ProjectionType.Orthographic;
		Assert.AreEqual(ProjectionType.Orthographic, camera.Projection);
		camera.Projection = ProjectionType.Perspective;
		camera.FixedAspectRatio = true;
		camera.AspectRatio = 2.0f;
		Assert.IsTrue(camera.FixedAspectRatio);
		Assert.AreEqual(2.0f, camera.AspectRatio);
		camera.FixedAspectRatio = false;
		camera.Primary = false;
		Assert.IsFalse(camera.Primary);
		camera.Primary = true;
	}

	private static void LightChecks()
	{
		DirectionalLightComponent sun = Find<DirectionalLightComponent>("Sun");
		sun.Color = new Color(1.0f, 0.9f, 0.8f);
		Assert.AreEqual(new Color(1.0f, 0.9f, 0.8f), sun.Color);
		sun.Intensity = 3.0f;
		Assert.AreEqual(3.0f, sun.Intensity);
		sun.CastShadows = true;
		Assert.IsTrue(sun.CastShadows);
		sun.LightSize = 0.5f;
		Assert.AreEqual(0.5f, sun.LightSize);

		PointLightComponent point = Find<PointLightComponent>("Point Light");
		point.Color = Color.Green;
		Assert.AreEqual(Color.Green, point.Color);
		point.Intensity = 20.0f;
		Assert.AreEqual(20.0f, point.Intensity);
		point.Range = 6.0f;
		Assert.AreEqual(6.0f, point.Range);
		point.CastShadows = false;
		Assert.IsFalse(point.CastShadows);

		SpotLightComponent spot = Find<SpotLightComponent>("Spot Light");
		spot.Color = Color.Blue;
		Assert.AreEqual(Color.Blue, spot.Color);
		spot.Intensity = 30.0f;
		Assert.AreEqual(30.0f, spot.Intensity);
		spot.Range = 12.0f;
		Assert.AreEqual(12.0f, spot.Range);
		spot.InnerConeAngle = 15.0f;
		Assert.AreEqual(15.0f, spot.InnerConeAngle);
		spot.OuterConeAngle = 30.0f;
		Assert.AreEqual(30.0f, spot.OuterConeAngle);
		spot.CastShadows = true;
		Assert.IsTrue(spot.CastShadows);
	}

	private static void SkyChecks()
	{
		SkyLightComponent sky = Find<SkyLightComponent>("Sky");
		Assert.AreEqual(Assets.Load<EnvironmentMap>("builtin://DefaultSky"), sky.Environment);
		sky.Intensity = 0.8f;
		Assert.AreEqual(0.8f, sky.Intensity);
		sky.Rotation = 90.0f;
		Assert.AreEqual(90.0f, sky.Rotation);
		sky.SkyboxBlur = 0.25f;
		Assert.AreEqual(0.25f, sky.SkyboxBlur);
		sky.DrawSkybox = false;
		Assert.IsFalse(sky.DrawSkybox);
		sky.DrawSkybox = true;
		sky.AmbientColor = new Color(0.1f, 0.2f, 0.3f);
		Assert.AreEqual(new Color(0.1f, 0.2f, 0.3f), sky.AmbientColor);
		EnvironmentMap? environment = sky.Environment;
		sky.Environment = null;
		Assert.IsNull(sky.Environment);
		sky.Environment = environment;
	}

	private static void MeshChecks()
	{
		MeshComponent mesh = Find<MeshComponent>("Cube");
		Mesh? cube = Assets.Load<Mesh>("builtin://Cube");
		Assert.AreEqual(cube, mesh.Mesh);
		mesh.Mesh = Assets.Load<Mesh>("builtin://Cylinder");
		Assert.AreEqual(Assets.Load<Mesh>("builtin://Cylinder"), mesh.Mesh);
		mesh.Mesh = cube;
		mesh.CastShadows = false;
		Assert.IsFalse(mesh.CastShadows);
		mesh.CastShadows = true;
		mesh.Visible = false;
		Assert.IsFalse(mesh.Visible);
		mesh.Visible = true;

		// Overrides of the mesh's materials, one per slot; the list grows as needed.
		Assert.AreEqual(0, mesh.MaterialCount);
		Assert.IsNull(mesh.GetMaterial(0));
		Material? painted = Assets.Load<Material>("Materials/Painted.smat");
		mesh.SetMaterial(1, painted);
		Assert.AreEqual(2, mesh.MaterialCount);
		Assert.IsNull(mesh.GetMaterial(0));
		Assert.AreEqual(painted, mesh.GetMaterial(1));
		mesh.SetMaterial(1, null);
		Assert.IsNull(mesh.GetMaterial(1));
	}

	private static void TextChecks()
	{
		TextComponent label = Find<TextComponent>("Label");
		Assert.AreEqual("Strada Feature Test", label.Text);
		label.Text = "Running";
		Assert.AreEqual("Running", label.Text);
		Assert.AreEqual(Assets.Load<Font>("builtin://DefaultFont"), label.Font);
		label.Font = null;
		Assert.IsNull(label.Font);
		label.Font = Assets.Load<Font>("builtin://DefaultFont");
		label.Color = Color.Red;
		Assert.AreEqual(Color.Red, label.Color);
		label.FontSize = 24.0f;
		Assert.AreEqual(24.0f, label.FontSize);
		label.ScreenSpace = true;
		Assert.IsTrue(label.ScreenSpace);
		label.Alignment = TextAlignment.Right;
		Assert.AreEqual(TextAlignment.Right, label.Alignment);
		label.LineSpacing = 1.5f;
		Assert.AreEqual(1.5f, label.LineSpacing);
	}

	private static void SpriteChecks()
	{
		SpriteRendererComponent sprite = Find<SpriteRendererComponent>("Badge");
		Assert.AreEqual(Assets.Load<Texture>("builtin://White"), sprite.Texture);
		sprite.Texture = null;
		Assert.IsNull(sprite.Texture);
		sprite.Texture = Assets.Load<Texture>("builtin://White");
		sprite.Color = new Color(1.0f, 1.0f, 1.0f, 0.5f);
		Assert.AreEqual(new Color(1.0f, 1.0f, 1.0f, 0.5f), sprite.Color);
		sprite.Tiling = 2.0f;
		Assert.AreEqual(2.0f, sprite.Tiling);
		sprite.ScreenSpace = false;
		Assert.IsFalse(sprite.ScreenSpace);
	}
}
