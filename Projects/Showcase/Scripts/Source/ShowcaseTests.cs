using System.Linq;
using Strada;
using Strada.Testing;

namespace Showcase;

/// <summary>The checks of Scenes/Tests.sscene (the main scene with this script, and without the tour playing at the start). They
/// play the exhibits through their public methods and look at what happened: the stacks stand and fall, joints hold, balls
/// score and open the gate, the dominoes fall, the lights move, particles burn, the camera shakes and the tour hands over.
/// None of them needs the downloaded models. The run ends once they are done (play the scene, or
/// `StradaRuntime --project Showcase.sproj --scene Scenes/Tests.sscene --test`).</summary>
public sealed class ShowcaseTests : Script
{
	[Tooltip("The playground that counts the props")]
	public Entity? Playground;

	[Tooltip("The camera rig (a FlyCamera)")]
	public Entity? Rig;

	[Tooltip("The camera under the rig (a CameraShake)")]
	public Entity? Camera;

	[Tooltip("The lantern's light")]
	public Entity? Lantern;

	[Tooltip("The ball hanging from the beam")]
	public Entity? WreckingBall;

	[Tooltip("The bar a motorized hinge turns")]
	public Entity? Paddle;

	[Tooltip("The campfire, whose children burn")]
	public Entity? Campfire;

	[Tooltip("The goal's trigger (a GoalZone)")]
	public Entity? Goal;

	[Tooltip("The ball launcher")]
	public Entity? Launcher;

	[Tooltip("The pressure plate's trigger")]
	public Entity? Plate;

	[Tooltip("The gate the plate raises")]
	public Entity? Gate;

	[Tooltip("The domino run")]
	public Entity? Dominoes;

	[Tooltip("The parent of the bridge's planks")]
	public Entity? Bridge;

	[Tooltip("The opening tour (a Flythrough)")]
	public Entity? Tour;

	[Tooltip("The stage's light rig (a LightShow)")]
	public Entity? LightShow;

	[Tooltip("The lighthouse's turning beams (a Spinner)")]
	public Entity? Lighthouse;

	[Tooltip("The parent of the gallery's spheres")]
	public Entity? Gallery;

	[Tooltip("The parent of the fountain's jets")]
	public Entity? Fountain;

	[Tooltip("The fireflies' particle system")]
	public Entity? Fireflies;

	[Tooltip("The magic sparks' particle system")]
	public Entity? Sparks;

	[Tooltip("The parent of the falling leaves' particle systems")]
	public Entity? Leaves;

	[Tooltip("The debug view")]
	public Entity? DebugView;

	private float m_Time;
	private int m_Step;
	private Entity? m_Ball;
	private Entity? m_Dropped;
	private float m_LowestLight = float.MaxValue;
	private float m_BrightestLight;
	private float m_LighthouseAngle;
	private Color m_ShowColor;
	private float m_GateClosed;
	private Vector3 m_TourStart;

	protected override void OnUpdate(float deltaTime)
	{
		m_Time += deltaTime;
		float light = Lantern?.GetComponent<PointLightComponent>()?.Intensity ?? 0.0f;
		m_LowestLight = Mathf.Min(m_LowestLight, light);
		m_BrightestLight = Mathf.Max(m_BrightestLight, light);

		if (m_Step == 0 && m_Time > 0.5f)
		{
			m_Step = 1;
			m_LighthouseAngle = Lighthouse?.As<Spinner>()?.Angle ?? 0.0f;
			m_ShowColor = LightShow?.As<LightShow>()?.ColorOf(0) ?? Color.Black;
			m_GateClosed = Gate?.Transform.WorldTranslation.Y ?? 0.0f;
		}
		else if (m_Step == 1 && m_Time > 2.0f)
		{
			m_Step = 2;
			CheckTheSceneAtRest();
			ThrowAtTheTower();
			DropABallIntoTheGoal();
			Dominoes?.As<DominoRun>()?.Push();
		}
		else if (m_Step == 2 && m_Time > 4.0f)
		{
			m_Step = 3;
			TestReporter.Run("a ball dropped through the hoop scores", () =>
			{
				GoalZone? goal = Goal?.As<GoalZone>();
				Assert.IsNotNull(goal);
				Assert.AreEqual(1, goal.Count);
				Assert.AreEqual("1", goal.Counter?.GetComponent<TextComponent>()?.Text);
			});
			TestReporter.Run("the ball lands on the plate and the gate rises", () =>
			{
				PressurePlate? plate = Plate?.As<PressurePlate>();
				Assert.IsNotNull(plate);
				Assert.IsTrue(plate.Presses >= 1, $"{plate.Presses} presses");
				Assert.IsTrue(plate.Openness > 0.5f, $"the gate is {plate.Openness} open");
			});
			TestReporter.Run("the launcher fires a ball at the goal", () =>
			{
				BallLauncher? launcher = Launcher?.As<BallLauncher>();
				Assert.IsNotNull(launcher);
				Entity? ball = launcher.Fire();
				Assert.IsNotNull(ball);
				Assert.AreEqual(1, launcher.Shots);
				Assert.IsTrue(launcher.LaunchVelocity.Y > 1.0f, $"it lobs: {launcher.LaunchVelocity}");
			});
		}
		else if (m_Step == 3 && m_Time > 6.5f)
		{
			m_Step = 4;
			CheckWhatHappened();
			Camera?.As<CameraShake>()?.Shake(1.0f);
			if (DebugView?.As<DebugView>() is { } debug)
			{
				debug.Visible = true;
			}
			// The balls leave the plate: the gate closes after a while.
			Rig?.As<FlyCamera>()?.ClearBalls();
			Launcher?.As<BallLauncher>()?.ClearBalls();
			m_Dropped?.Destroy();
			m_Ball = null;
			m_Dropped = null;
		}
		else if (m_Step == 4 && m_Time > 6.6f)
		{
			m_Step = 5;
			TestReporter.Run("impacts shake the camera", () =>
			{
				CameraShake? shake = Camera?.As<CameraShake>();
				Assert.IsNotNull(shake);
				Assert.IsTrue(shake.Trauma > 0.8f, $"trauma {shake.Trauma}");
				Assert.IsTrue(Camera!.Translation.Length > 1e-4f || Camera.Transform.EulerAngles.Length > 1e-3f, "the camera moved");
			});
			TestReporter.Run("the debug view draws paths and trigger bounds", () =>
			{
				DebugView? debug = DebugView?.As<DebugView>();
				Assert.IsNotNull(debug);
				// Two trigger boxes (12 lines each), the bridge's links and the two ballistic paths.
				Assert.IsTrue(debug.LinesDrawn > 40, $"{debug.LinesDrawn} lines");
				debug.Visible = false;
			});
			TestReporter.Run("the tour takes the camera along its shots", () =>
			{
				Flythrough? tour = Tour?.As<Flythrough>();
				FlyCamera? rig = Rig?.As<FlyCamera>();
				Assert.IsNotNull(tour);
				Assert.IsNotNull(rig);
				Assert.IsFalse(tour.IsPlaying, "the test scene starts without the tour");
				Assert.IsTrue(tour.ShotCount == 0 || tour.ShotCount >= 8);
				m_TourStart = rig.Translation;
				tour.Play();
				Assert.IsTrue(tour.IsPlaying);
				Assert.IsTrue(tour.ShotCount >= 8, $"{tour.ShotCount} shots");
				Assert.IsFalse(rig.Controllable, "the tour has the camera");
			});
		}
		else if (m_Step == 5 && m_Time > 8.0f)
		{
			m_Step = 6;
			TestReporter.Run("the tour moves the camera, and a skip hands it back", () =>
			{
				Flythrough? tour = Tour?.As<Flythrough>();
				FlyCamera? rig = Rig?.As<FlyCamera>();
				Assert.IsNotNull(tour);
				Assert.IsNotNull(rig);
				Assert.IsTrue(tour.IsPlaying);
				Assert.IsTrue(Vector3.Distance(rig.Translation, m_TourStart) > 1.0f, "the camera left its place");
				tour.Skip();
				Assert.IsTrue(tour.IsSkipping);
			});
		}
		else if (m_Step == 6 && m_Time > 10.0f)
		{
			m_Step = 7;
			TestReporter.Run("after the tour the free camera has control", () =>
			{
				Flythrough? tour = Tour?.As<Flythrough>();
				FlyCamera? rig = Rig?.As<FlyCamera>();
				Assert.IsNotNull(tour);
				Assert.IsNotNull(rig);
				Assert.IsFalse(tour.IsPlaying);
				Assert.AreEqual(1, tour.HandOvers);
				Assert.IsTrue(rig.Controllable);
				// It ends where the last shot is: the home view.
				Entity last = tour.Children[^1];
				Assert.IsTrue(Vector3.Distance(rig.Translation, last.Transform.WorldTranslation) < 0.01f, $"the camera is at {rig.Translation}");
			});
			TestReporter.Run("the dominoes fall one after the other", () =>
			{
				DominoRun? run = Dominoes?.As<DominoRun>();
				Assert.IsNotNull(run);
				Assert.AreEqual(1, run.Pushes);
				Assert.IsTrue(run.Count >= 20, $"{run.Count} dominoes");
				Assert.IsTrue(run.LastFell, $"{run.Fallen} of {run.Count} fell");
				Assert.IsTrue(run.Fallen >= run.Count - 1, $"{run.Fallen} of {run.Count} fell");
			});
			TestReporter.Run("the gate closes again once the plate is clear", () =>
			{
				PressurePlate? plate = Plate?.As<PressurePlate>();
				Assert.IsNotNull(plate);
				Assert.IsFalse(plate.IsPressed);
				Assert.AreEqual(0.0f, plate.Openness);
				Assert.IsTrue(Mathf.Abs(Gate!.Transform.WorldTranslation.Y - m_GateClosed) < 0.05f, $"the gate is at {Gate.Transform.WorldTranslation.Y}");
			});
			TestReporter.Run("the shake wears off", () =>
			{
				CameraShake? shake = Camera?.As<CameraShake>();
				Assert.IsNotNull(shake);
				Assert.AreEqual(0.0f, shake.Trauma);
				Assert.IsTrue(Camera!.Translation.Length < 1e-4f, $"the camera is at {Camera.Translation}");
			});
			Playground?.As<Playground>()?.PutBack();
		}
		else if (m_Step == 7 && m_Time > 11.0f)
		{
			m_Step = 8;
			TestReporter.Run("R puts everything back", () =>
			{
				Playground? playground = Playground?.As<Playground>();
				DominoRun? run = Dominoes?.As<DominoRun>();
				Assert.IsNotNull(playground);
				Assert.IsNotNull(run);
				Assert.AreEqual(1, playground.Resets);
				Assert.AreEqual(0, playground.Knocked);
				Assert.AreEqual(0, run.Fallen);
			});
			TestReporter.Finish();
			Application.Quit();
		}
	}

	private void CheckTheSceneAtRest()
	{
		Playground? playground = Playground?.As<Playground>();
		TestReporter.Run("the stacks stand on their own", () =>
		{
			Assert.IsNotNull(playground);
			Assert.IsTrue(playground.Total >= 10, $"{playground.Total} props");
			Assert.AreEqual(0, playground.Knocked);
		});
		TestReporter.Run("joints hold the wrecking ball and turn the paddle", () =>
		{
			Assert.IsNotNull(WreckingBall);
			Assert.IsNotNull(Paddle);
			// Two seconds after the start the ball still hangs where it was, held up by its joint.
			Assert.IsTrue(WreckingBall.Transform.WorldTranslation.Y > 0.9f, $"the ball is at {WreckingBall.Transform.WorldTranslation}");
			Assert.IsTrue(WreckingBall.GetComponent<RigidBodyComponent>()!.LinearVelocity.Length < 0.5f, "and hangs still");
			float spin = Paddle.GetComponent<RigidBodyComponent>()!.AngularVelocity.Length;
			Assert.IsTrue(spin > 0.5f && spin < 0.9f, $"the paddle turns at {spin} rad/s (40 degrees per second)");
		});
		TestReporter.Run("the bridge hangs from its towers", () =>
		{
			Assert.IsNotNull(Bridge);
			Entity[] planks = Bridge.Children;
			Assert.IsTrue(planks.Length >= 10, $"{planks.Length} planks");
			for (int i = 0; i < planks.Length; i++)
			{
				float height = planks[i].Transform.WorldTranslation.Y;
				Assert.IsTrue(height > 0.6f && height < 2.0f, $"plank {i + 1} is at {height} m");
				if (i > 0)
				{
					float link = Vector3.Distance(planks[i].Transform.WorldTranslation, planks[i - 1].Transform.WorldTranslation);
					Assert.IsTrue(link < 0.5f, $"planks {i} and {i + 1} are {link} m apart");
				}
			}
			// It sags: the middle hangs lower than the ends.
			float middle = planks[planks.Length / 2].Transform.WorldTranslation.Y;
			Assert.IsTrue(middle < planks[0].Transform.WorldTranslation.Y - 0.2f, $"the middle is at {middle} m");
		});
		TestReporter.Run("the campfire, the fountain, the fireflies, the sparks and the leaves emit", () =>
		{
			Assert.IsNotNull(Campfire);
			Assert.IsNotNull(Fountain);
			Assert.IsNotNull(Leaves);
			long fire = Campfire.Children.Sum(child => (long)(child.GetComponent<ParticleSystemComponent>()?.ParticleCount ?? 0));
			Assert.IsTrue(fire > 50, $"{fire} campfire particles");
			long water = Fountain.Children.Sum(child => (long)(child.GetComponent<ParticleSystemComponent>()?.ParticleCount ?? 0));
			Assert.IsTrue(water > 100, $"{water} fountain particles");
			uint fireflies = Fireflies?.GetComponent<ParticleSystemComponent>()?.ParticleCount ?? 0;
			Assert.IsTrue(fireflies > 5, $"{fireflies} fireflies");
			uint sparks = Sparks?.GetComponent<ParticleSystemComponent>()?.ParticleCount ?? 0;
			Assert.IsTrue(sparks > 10, $"{sparks} sparks");
			// Three tints of leaves, a few a second each, all still falling two seconds in.
			long leaves = Leaves.Children.Sum(child => (long)(child.GetComponent<ParticleSystemComponent>()?.ParticleCount ?? 0));
			Assert.AreEqual(3, Leaves.Children.Length);
			Assert.IsTrue(leaves > 15, $"{leaves} leaves");
		});
		TestReporter.Run("the gallery sweeps metallic and roughness", () =>
		{
			Assert.IsNotNull(Gallery);
			Entity[] spheres = Gallery.Children;
			Assert.AreEqual(25, spheres.Length);
			for (int i = 0; i < spheres.Length; i++)
			{
				Material? material = spheres[i].GetComponent<MeshComponent>()?.GetMaterial(0);
				Assert.IsNotNull(material);
				// Columns of five, metallic rising from column to column and roughness down each column.
				Assert.AreApproximatelyEqual((i / 5) / 4.0f, material.Metallic, 1e-4f);
				Assert.AreApproximatelyEqual((i % 5) / 4.0f, material.Roughness, 1e-4f);
			}
		});
		TestReporter.Run("the lighthouse turns and the light show changes color", () =>
		{
			Spinner? beams = Lighthouse?.As<Spinner>();
			LightShow? show = LightShow?.As<LightShow>();
			Assert.IsNotNull(beams);
			Assert.IsNotNull(show);
			Assert.AreEqual(4, show.LightCount);
			float turned = Mathf.Repeat(beams.Angle - m_LighthouseAngle, 360.0f);
			Assert.IsTrue(turned > 40.0f && turned < 70.0f, $"turned {turned} degrees in 1.5 seconds");
			Color color = show.ColorOf(0);
			Assert.IsTrue(Mathf.Abs(color.R - m_ShowColor.R) + Mathf.Abs(color.G - m_ShowColor.G) + Mathf.Abs(color.B - m_ShowColor.B) > 0.05f,
				$"{m_ShowColor} became {color}");
			// Each spot's lens and the cones of its shaft glow in its color, through copies of their materials.
			for (int i = 0; i < show.LightCount; i++)
			{
				Color light = show.ColorOf(i);
				Assert.AreEqual(2, show.GlowsOf(i).Count);
				foreach (Material glow in show.GlowsOf(i))
				{
					Assert.IsTrue(glow.IsEditable, "the glow is a copy the scene owns");
					Color emissive = glow.Emissive;
					Assert.IsTrue(Mathf.Abs(emissive.R - light.R) + Mathf.Abs(emissive.G - light.G) + Mathf.Abs(emissive.B - light.B) < 1e-3f,
						$"light {i} glows {emissive}, its light is {light}");
				}
			}
		});
	}

	// The camera flies close to the tower and throws at its top crate, so nothing is in the way.
	private void ThrowAtTheTower()
	{
		Playground? playground = Playground?.As<Playground>();
		TestReporter.Run("a ball flies from the camera", () =>
		{
			FlyCamera? rig = Rig?.As<FlyCamera>();
			Assert.IsNotNull(rig);
			Assert.IsNotNull(playground?.Props);
			// The highest crate, at about eye height. Box colliders do not depend on the downloaded models, so this works
			// without them.
			Entity target = playground.Props.Children.Where(prop => prop.HasComponent<BoxColliderComponent>())
				.OrderByDescending(prop => prop.Transform.WorldTranslation.Y).First();
			Vector3 crate = target.Transform.WorldTranslation;
			Vector3 back = rig.Translation - crate;
			back.Y = 0.0f;
			rig.SetPose(crate + (back.Normalized * 7.0f) + new Vector3(0.0f, 0.6f, 0.0f), rig.Pitch, rig.Yaw);
			// Aimed above the crate by the distance the ball falls on its way, so it hits the crate itself on every platform.
			Vector3 toTarget = crate + new Vector3(0.0f, 0.15f, 0.0f) - rig.Translation;
			float flight = toTarget.Length / rig.ThrowSpeed;
			m_Ball = rig.Throw(toTarget + new Vector3(0.0f, 0.5f * 9.81f * flight * flight, 0.0f));
			Assert.IsNotNull(m_Ball);
			Assert.IsTrue(m_Ball.GetComponent<RigidBodyComponent>()!.LinearVelocity.Length > 10.0f);
			Assert.AreEqual(1, rig.Thrown);
		});
	}

	private void DropABallIntoTheGoal()
	{
		TestReporter.Run("a ball can be dropped over the hoop", () =>
		{
			GoalZone? goal = Goal?.As<GoalZone>();
			FlyCamera? rig = Rig?.As<FlyCamera>();
			Assert.IsNotNull(goal?.Hoop);
			Assert.IsNotNull(rig?.Ball);
			Assert.AreEqual(0, goal.Count);
			m_Dropped = Entity.Instantiate(rig.Ball, goal.Hoop.Transform.WorldTranslation + new Vector3(0.0f, 1.2f, 0.0f), Quaternion.Identity);
			Assert.IsNotNull(m_Dropped);
		});
	}

	private void CheckWhatHappened()
	{
		Playground? playground = Playground?.As<Playground>();
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
		TestReporter.Run("the launcher's ball goes through the hoop", () =>
		{
			GoalZone? goal = Goal?.As<GoalZone>();
			Assert.IsNotNull(goal);
			Assert.AreEqual(2, goal.Count);
			Assert.AreEqual("2", goal.Counter?.GetComponent<TextComponent>()?.Text);
		});
		TestReporter.Run("the gate stands open while the plate is pressed", () =>
		{
			PressurePlate? plate = Plate?.As<PressurePlate>();
			Assert.IsNotNull(plate);
			Assert.IsNotNull(Gate);
			Assert.IsTrue(Gate.Transform.WorldTranslation.Y > m_GateClosed + 2.0f, $"the gate rose to {Gate.Transform.WorldTranslation.Y}");
		});
		TestReporter.Run("a push swings the wrecking ball", () =>
		{
			WreckingBall? ball = WreckingBall?.As<WreckingBall>();
			Assert.IsNotNull(ball);
			ball.Swing();
			Assert.AreEqual(1, ball.Swings);
			Assert.IsTrue(WreckingBall!.GetComponent<RigidBodyComponent>()!.LinearVelocity.Length > 5.0f);
		});
		TestReporter.Run("the lantern flickers around its intensity", () =>
		{
			Assert.IsNotNull(Lantern?.GetComponent<PointLightComponent>());
			Assert.IsTrue(m_BrightestLight > m_LowestLight, $"{m_LowestLight} to {m_BrightestLight}");
			Assert.IsTrue(m_LowestLight > 0.0f);
		});
	}
}
