using System.Linq;
using Strada;
using Strada.Testing;

namespace Showcase;

/// <summary>The checks of Scenes/Tests.sscene (the main scene with this script): the stacks stand on their own, a thrown ball
/// knocks props over with sound, and the lantern flickers. The run ends once they are done (play the scene, or
/// `StradaRuntime --project Showcase.sproj --scene Scenes/Tests.sscene --test`).</summary>
public sealed class ShowcaseTests : Script
{
	[Tooltip("The playground the checks play")]
	public Entity? Playground;

	[Tooltip("The camera that throws")]
	public Entity? Camera;

	[Tooltip("The lantern's light")]
	public Entity? Lantern;

	private float m_Time;
	private int m_Step;
	private Entity? m_Ball;
	private float m_LowestLight = float.MaxValue;
	private float m_BrightestLight;

	protected override void OnUpdate(float deltaTime)
	{
		m_Time += deltaTime;
		float light = Lantern?.GetComponent<PointLightComponent>()?.Intensity ?? 0.0f;
		m_LowestLight = System.MathF.Min(m_LowestLight, light);
		m_BrightestLight = System.MathF.Max(m_BrightestLight, light);

		Playground? playground = Playground?.As<Playground>();
		if (m_Step == 0 && m_Time > 2.0f)
		{
			m_Step = 1;
			TestReporter.Run("the stacks stand on their own", () =>
			{
				Assert.IsNotNull(playground);
				Assert.IsTrue(playground.Total >= 10, $"{playground.Total} props");
				Assert.AreEqual(0, playground.Knocked);
			});
			TestReporter.Run("a ball flies from the camera", () =>
			{
				FlyCamera? camera = Camera?.As<FlyCamera>();
				Assert.IsNotNull(camera);
				Assert.IsNotNull(playground?.Props);
				// The highest crate, at about eye height, so nothing is in the way. Box colliders do not depend on the downloaded
				// models, so this works without them.
				Entity target = playground.Props.Children.Where(prop => prop.HasComponent<BoxColliderComponent>())
					.OrderByDescending(prop => prop.Transform.WorldTranslation.Y).First();
				// Aimed above the crate by the distance the ball falls on its way, so it hits the crate itself on every platform
				// (a hit low on the tower moves the heavy crates there too little, by amounts that differ between machines).
				Vector3 toTarget = target.Transform.WorldTranslation + new Vector3(0.0f, 0.15f, 0.0f) - Camera!.Transform.WorldTranslation;
				float flight = toTarget.Length / camera.ThrowSpeed;
				m_Ball = camera.Throw(toTarget + new Vector3(0.0f, 0.5f * 9.81f * flight * flight, 0.0f));
				Assert.IsNotNull(m_Ball);
				Assert.IsTrue(m_Ball.GetComponent<RigidBodyComponent>()!.LinearVelocity.Length > 10.0f);
				Assert.AreEqual(1, camera.Thrown);
			});
		}
		else if (m_Step == 1 && m_Time > 6.0f)
		{
			m_Step = 2;
			TestReporter.Run("the ball knocks props over", () =>
			{
				Assert.IsNotNull(playground);
				Assert.IsTrue(playground.Knocked > 0, $"{playground.Knocked} knocked");
			});
			TestReporter.Run("hits make sounds", () =>
			{
				Assert.IsNotNull(playground?.Props);
				int hits = playground.Props.Children.Sum(prop => prop.As<Impact>()?.Hits ?? 0);
				hits += m_Ball is not null && m_Ball.IsValid ? m_Ball.As<Impact>()?.Hits ?? 0 : 0;
				Assert.IsTrue(hits > 0, $"{hits} hits");
			});
			TestReporter.Run("the lantern flickers around its intensity", () =>
			{
				Assert.IsNotNull(Lantern?.GetComponent<PointLightComponent>());
				Assert.IsTrue(m_BrightestLight > m_LowestLight, $"{m_LowestLight} to {m_BrightestLight}");
				Assert.IsTrue(m_LowestLight > 0.0f);
			});
			TestReporter.Finish();
			Application.Quit();
		}
	}
}
