using System;
using System.Collections.Generic;
using System.Linq;
using Strada;
using Strada.Testing;

namespace Showcase;

/// <summary>The checks of Scenes/Tests.sscene (the main scene with this script, and without the tour playing at the start). They
/// play the exhibits through their public methods and look at what happened: the stacks stand and fall, joints hold, balls
/// score and open the gate, the dominoes fall, the lights move, particles burn and smoke dims at night, the camera shakes, the
/// tour cues its exhibits, plays the music up, hands over and plays again with the exhibits put back, the neon sign lights up,
/// night falls and passes again in the tour's interlude (which a skip ends and the visitor's choice outlasts), the HUD's buttons
/// work, the cursor is freed, the walker climbs, runs, jumps, pushes, rides the lift, waits under it, presses the plate and stays
/// on the ground, and the camera flies within the clearing. None of them needs the downloaded models. The run ends once they
/// are done (play the scene, or `StradaRuntime --project Showcase.sproj --scene Scenes/Tests.sscene --test`).</summary>
public sealed class ShowcaseTests : Script
{
	[Tooltip("The playground that counts the props")]
	public Entity? Playground;

	[Tooltip("The camera rig (a CameraRig)")]
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

	[Tooltip("The HUD (a Hud)")]
	public Entity? Hud;

	[Tooltip("The day and night switch (a DayNight)")]
	public Entity? Lighting;

	[Tooltip("The neon sign (a NeonSign)")]
	public Entity? NeonSign;

	[Tooltip("The global post-process volume with the scene's look")]
	public Entity? Look;

	[Tooltip("The local post-process volume around the light show")]
	public Entity? EveningZone;

	[Tooltip("The walker (a Walker on a character controller)")]
	public Entity? Walker;

	[Tooltip("The yard's lift (a Lift)")]
	public Entity? Lift;

	[Tooltip("The yard's deck, which the stairs climb to")]
	public Entity? Deck;

	[Tooltip("The lowest of the stairs up to the deck")]
	public Entity? FirstStep;

	[Tooltip("The ramp down from the deck (its local X runs uphill)")]
	public Entity? Ramp;

	[Tooltip("The tour's night for a while (a NightInterlude)")]
	public Entity? NightInterlude;

	private readonly List<(float Time, Action Step)> m_Schedule = [];
	private int m_Next;
	private float m_Time;
	private Entity? m_Ball;
	private Entity? m_Dropped;
	private float m_LowestLight = float.MaxValue;
	private float m_BrightestLight;
	private float m_LighthouseAngle;
	private Color m_ShowColor;
	private float m_GateClosed;
	private float m_SmokeIntensity;
	private float m_LeavesIntensity;
	private Vector3 m_TourStart;
	private (float Frame, float Letters) m_NeonDark;
	private (float Frame, float Letters) m_NeonFrameOnly;
	private (int Swings, int Pushes, int Shots, int Resets, int Switches) m_BeforeClicks;
	private Vector3 m_WalkFrom;
	private float m_WalkRange = float.PositiveInfinity;
	private Vector3 m_PaceFrom;
	private float m_WalkPace;
	private float m_JumpPeak = float.MinValue;
	private bool m_TrackJump;
	private Entity? m_Crate;
	private Vector3 m_CrateFrom;
	private bool m_InterludeNight;
	private bool m_InterludeDay;
	private float m_MusicVolume;
	private float m_NightSmoke = -1.0f;
	private int m_SwitchesBeforeChoice;
	private int m_PlatePresses;
	private int m_ResetsBeforeReplay;

	protected override void OnCreate()
	{
		At(0.5f, Begin);
		At(0.6f, () => m_NeonDark = NeonLevels());
		At(1.2f, () => m_NeonFrameOnly = NeonLevels());
		At(2.0f, () =>
		{
			CheckTheSceneAtRest();
			CheckThePostProcessVolumes();
			ThrowAtTheTower();
			DropABallIntoTheGoal();
			Dominoes?.As<DominoRun>()?.Push();
		});
		At(2.1f, () => Lighting?.As<DayNight>()?.SetNight(true));
		At(2.6f, CheckTheNeonSign);
		At(4.0f, CheckTheGoalAndTheGate);
		At(5.3f, CheckTheNight);
		At(6.5f, () =>
		{
			CheckWhatHappened();
			Camera?.As<CameraShake>()?.Shake(1.0f);
			if (DebugView?.As<DebugView>() is { } debug)
			{
				debug.Visible = true;
			}
			// The balls leave the plate: the gate closes after a while.
			Rig?.As<CameraRig>()?.ClearBalls();
			Launcher?.As<BallLauncher>()?.ClearBalls();
			m_Dropped?.Destroy();
			m_Ball = null;
			m_Dropped = null;
		});
		At(6.6f, CheckTheShakeAndStartTheTour);
		At(8.0f, CheckTheTourUnderWay);
		At(8.6f, CheckTheDayComesBack);
		At(11.1f, CheckTheCaptionsAndSkip);
		At(13.1f, () =>
		{
			CheckTheHandOver();
			Playground?.As<Playground>()?.PutBack();
		});
		At(14.1f, () =>
		{
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
			CheckTheHudAfterTheTour();
			ClickTheHudButtons();
		});
		At(14.9f, () =>
		{
			CheckTheFeatureList();
			CheckTheClicks();
			// F: the rig walks, on what is under it (the gallery).
			Rig?.As<CameraRig>()?.SetWalking(true);
		});
		At(15.7f, CheckTheRigOnItsFeet);
		At(15.8f, ClimbTheStairs);
		At(17.8f, CheckTheStairs);
		At(17.9f, WalkDownTheRamp);
		At(19.9f, CheckTheRamp);
		// North along an open line from south of the plaza: a second at walking pace, then a second running.
		At(20.5f, () => WalkAlong(new Vector3(-1.0f, 0.04f, 19.0f), 0.0f));
		At(21.0f, () => m_PaceFrom = Walker?.Transform.WorldTranslation ?? Vector3.Zero);
		At(21.5f, () =>
		{
			m_WalkPace = PaceSince(0.5f);
			if (Walker?.As<Walker>() is { } walker)
			{
				walker.Running = true;
			}
		});
		At(22.0f, () => m_PaceFrom = Walker?.Transform.WorldTranslation ?? Vector3.Zero);
		At(22.5f, CheckTheSpeeds);
		At(23.2f, () =>
		{
			m_TrackJump = true;
			Walker?.As<Walker>()?.Jump();
		});
		At(24.8f, CheckTheJump);
		At(24.9f, PushACrate);
		At(26.5f, CheckThePush);
		At(26.6f, CheckTheCratesPutBack);
		At(26.7f, RideTheLift);
		At(33.4f, () =>
		{
			CheckTheLift();
			Hud?.As<Hud>()?.ModeButton?.GetComponent<UIButtonComponent>()?.Click();
		});
		At(33.5f, () =>
		{
			CheckTheModeButton();
			// Esc while walking.
			Rig?.As<CameraRig>()?.ReleaseCursor();
		});
		At(33.6f, () =>
		{
			CheckTheFreeCursor();
			StartTheInterlude(1.0f);
		});
		// The lift, up at the lookout, comes down from 35.2 seconds on: the walker waits under it.
		At(33.7f, StandUnderTheLift);
		At(34.1f, () => m_InterludeNight = Lighting?.As<DayNight>()?.IsNight ?? false);
		At(35.3f, () =>
		{
			// The day came back; an interlude cued at night then leaves the night.
			m_InterludeDay = Lighting?.As<DayNight>()?.IsNight == false && NightInterlude?.As<NightInterlude>()?.IsActive == false;
			Lighting?.As<DayNight>()?.SetNight(true);
			StartTheInterlude(0.5f);
		});
		At(36.1f, () =>
		{
			CheckTheInterlude();
			SwitchDuringTheInterlude();
		});
		At(36.9f, CheckTheVisitorsChoiceStands);
		At(37.5f, CheckTheLiftWaits);
		At(37.7f, () =>
		{
			CheckTheLiftGoesOn();
			StepOnThePlate();
		});
		At(38.7f, () =>
		{
			CheckThePlateUnderTheWalker();
			CheckWhereTheRigFlies();
			WalkToTheEdge();
		});
		At(40.2f, () =>
		{
			CheckTheEdge();
			// The exhibits go off before the tour plays again.
			WreckingBall?.As<WreckingBall>()?.Swing();
			Dominoes?.As<DominoRun>()?.Push();
		});
		At(42.2f, ReplayTheTour);
		At(42.3f, CheckTheReplayAndSkip);
		At(43.7f, () =>
		{
			CheckTheSecondHandOver();
			TestReporter.Finish();
			Application.Quit();
		});
	}

	protected override void OnUpdate(float deltaTime)
	{
		m_Time += deltaTime;
		if (m_TrackJump && Walker is not null)
		{
			m_JumpPeak = Mathf.Max(m_JumpPeak, Walker.Transform.WorldTranslation.Y);
		}
		// A walk with a range stops once the walker has come that far along the ground.
		if (m_WalkRange < float.PositiveInfinity && Walker?.As<Walker>() is { } walker &&
			Vector3.Distance(Flat(walker.Transform.WorldTranslation), Flat(m_WalkFrom)) >= m_WalkRange)
		{
			walker.MoveInput = Vector2.Zero;
			m_WalkRange = float.PositiveInfinity;
		}
		float light = Lantern?.GetComponent<PointLightComponent>()?.Intensity ?? 0.0f;
		m_LowestLight = Mathf.Min(m_LowestLight, light);
		m_BrightestLight = Mathf.Max(m_BrightestLight, light);
		while (m_Next < m_Schedule.Count && m_Schedule[m_Next].Time <= m_Time)
		{
			m_Schedule[m_Next++].Step();
		}
	}

	// The steps run in the order they are added, each once the run is that many seconds old.
	private void At(float time, Action step) => m_Schedule.Add((time, step));

	private void Begin()
	{
		m_LighthouseAngle = Lighthouse?.As<Spinner>()?.Angle ?? 0.0f;
		m_ShowColor = LightShow?.As<LightShow>()?.ColorOf(0) ?? Color.Black;
		m_GateClosed = Gate?.Transform.WorldTranslation.Y ?? 0.0f;
		(m_SmokeIntensity, m_LeavesIntensity) = MatterIntensities();
		m_MusicVolume = Tour?.As<Flythrough>()?.Music?.GetComponent<AudioSourceComponent>()?.Volume ?? 0.0f;
		NeonSign?.As<NeonSign>()?.Ignite();
	}

	private (float Frame, float Letters) NeonLevels()
	{
		NeonSign? sign = NeonSign?.As<NeonSign>();
		return sign is null ? (-1.0f, -1.0f) : (sign.FrameLevel, sign.LettersLevel);
	}

	private void CheckTheNeonSign()
	{
		TestReporter.Run("the neon sign flickers on, its frame before its letters", () =>
		{
			NeonSign? sign = NeonSign?.As<NeonSign>();
			Assert.IsNotNull(sign);
			Assert.AreEqual(1, sign.Ignitions);
			// A tenth of a second after it is switched on, the tubes are dark; 0.7 seconds after, the frame holds and the
			// letters have not flickered yet; two seconds after, both hold.
			Assert.AreEqual((0.0f, 0.0f), m_NeonDark);
			Assert.AreEqual((1.0f, 0.0f), m_NeonFrameOnly);
			Assert.IsFalse(sign.IsIgniting);
			Assert.AreEqual(1.0f, sign.FrameLevel);
			Assert.AreEqual(1.0f, sign.LettersLevel);
			Assert.IsTrue(sign.Letters?.GetComponent<MeshComponent>()?.GetMaterial(0)?.IsEditable == true, "the letters glow through a copy");
		});
	}

	private void CheckThePostProcessVolumes()
	{
		TestReporter.Run("a global volume carries the scene's look", () =>
		{
			PostProcessComponent? look = Look?.GetComponent<PostProcessComponent>();
			Assert.IsNotNull(look);
			Assert.IsTrue(look.Global);
			Assert.AreEqual(1.0f, look.Weight);
			Assert.IsTrue(look.OverrideContrast && look.OverrideSaturation && look.OverrideTemperature && look.OverrideVignette &&
				look.OverrideFilmGrain && look.OverrideBloomIntensity, "it grades the scene");
			Assert.IsTrue(look.Temperature > 0.0f, "warm, at golden hour");
		});
		TestReporter.Run("the evening zone surrounds the light show and fades out before the plaza", () =>
		{
			PostProcessComponent? zone = EveningZone?.GetComponent<PostProcessComponent>();
			Assert.IsNotNull(zone);
			Assert.IsNotNull(LightShow);
			Assert.IsFalse(zone.Global);
			Assert.IsTrue(zone.BlendDistance >= 3.0f, $"it fades over {zone.BlendDistance} m");
			Assert.IsTrue(zone.OverrideEV100 && zone.OverrideBloomIntensity && zone.OverrideTemperature, "an evening grade");
			Assert.IsTrue(zone.Priority > Look!.GetComponent<PostProcessComponent>()!.Priority, "over the scene's look");
			Assert.AreEqual(0.0f, DistanceToBox(EveningZone!, LightShow.Transform.WorldTranslation + new Vector3(0.0f, 1.7f, 0.0f)));
			float plaza = DistanceToBox(EveningZone!, new Vector3(0.0f, 1.7f, 0.0f));
			Assert.IsTrue(plaza > zone.BlendDistance, $"the plaza is {plaza} m from the zone");
		});
	}

	private void CheckTheNight()
	{
		TestReporter.Run("night falls: moonlight, a dark sky, the night's grade, dim smoke and leaves, and crickets", () =>
		{
			DayNight? lighting = Lighting?.As<DayNight>();
			Assert.IsNotNull(lighting);
			Assert.IsTrue(lighting.IsNight);
			Assert.AreEqual(1.0f, lighting.Blend);
			Assert.AreApproximatelyEqual(lighting.MoonIntensity, lighting.Sun!.GetComponent<DirectionalLightComponent>()!.Intensity, 1e-4f);
			Assert.AreApproximatelyEqual(lighting.NightSkyIntensity, lighting.Sky!.GetComponent<SkyLightComponent>()!.Intensity, 1e-4f);
			Assert.AreEqual(1.0f, lighting.NightGrade!.GetComponent<PostProcessComponent>()!.Weight);
			Assert.AreEqual(0.0f, lighting.EveningZone!.GetComponent<PostProcessComponent>()!.Weight);
			Assert.AreEqual(0.0f, lighting.DayAmbience!.GetComponent<AudioSourceComponent>()!.Volume);
			Assert.AreApproximatelyEqual(lighting.NightAmbienceVolume, lighting.NightAmbience!.GetComponent<AudioSourceComponent>()!.Volume, 1e-4f);
			(float smoke, float leaves) = MatterIntensities();
			Assert.IsTrue(m_SmokeIntensity > 0.0f && m_LeavesIntensity > 0.0f, "the smoke and the leaves glow by day");
			Assert.AreApproximatelyEqual(m_SmokeIntensity * lighting.NightMatterBrightness, smoke, 1e-4f, "the smoke dims");
			Assert.AreApproximatelyEqual(m_LeavesIntensity * lighting.NightMatterBrightness, leaves, 1e-4f, "the leaves dim");
			Assert.AreApproximatelyEqual(lighting.NightMatterBrightness, lighting.MatterBrightness, 1e-4f);
			m_NightSmoke = FireForSmoke();
			lighting.SetNight(false);
			Assert.IsFalse(lighting.IsNight);
		});
	}

	private void CheckTheDayComesBack()
	{
		TestReporter.Run("the day comes back as it was", () =>
		{
			DayNight? lighting = Lighting?.As<DayNight>();
			Assert.IsNotNull(lighting);
			Assert.AreEqual(0.0f, lighting.Blend);
			Assert.AreEqual(2, lighting.Switches);
			Assert.AreApproximatelyEqual(lighting.DaySunIntensity, lighting.Sun!.GetComponent<DirectionalLightComponent>()!.Intensity, 1e-4f);
			Assert.AreApproximatelyEqual(lighting.DaySkyIntensity, lighting.Sky!.GetComponent<SkyLightComponent>()!.Intensity, 1e-4f);
			Assert.AreEqual(0.0f, lighting.NightGrade!.GetComponent<PostProcessComponent>()!.Weight);
			Assert.AreApproximatelyEqual(lighting.DayZoneWeight, lighting.EveningZone!.GetComponent<PostProcessComponent>()!.Weight, 1e-4f);
			Assert.AreEqual(0.0f, lighting.NightAmbience!.GetComponent<AudioSourceComponent>()!.Volume);
			(float smoke, float leaves) = MatterIntensities();
			Assert.AreApproximatelyEqual(m_SmokeIntensity, smoke, 1e-4f);
			Assert.AreApproximatelyEqual(m_LeavesIntensity, leaves, 1e-4f);
		});
		TestReporter.Run("the launcher's smoke dims at night like the campfire's", () =>
		{
			DayNight? lighting = Lighting?.As<DayNight>();
			Assert.IsNotNull(lighting);
			Assert.AreEqual(1.0f, lighting.MatterBrightness);
			float daySmoke = FireForSmoke();
			Assert.IsTrue(daySmoke > 0.0f, $"the smoke shows at {daySmoke} by day");
			Assert.AreApproximatelyEqual(daySmoke * lighting.NightMatterBrightness, m_NightSmoke, 1e-4f);
		});
	}

	// Fires the launcher and takes its ball away at once (nothing lands anywhere); returns how bright the shot's smoke is.
	private float FireForSmoke()
	{
		BallLauncher? launcher = Launcher?.As<BallLauncher>();
		launcher?.Fire()?.Destroy();
		return launcher?.LastFlash?.GetComponent<ParticleSystemComponent>()?.Intensity ?? -1.0f;
	}

	// The brightness of the campfire's smoke and of the first of the falling leaves (a child of the leaves DayNight dims).
	private (float Smoke, float Leaves) MatterIntensities()
	{
		DayNight? lighting = Lighting?.As<DayNight>();
		float smoke = lighting?.Smoke?.GetComponent<ParticleSystemComponent>()?.Intensity ?? -1.0f;
		Entity[] leaves = lighting?.Leaves?.Children ?? [];
		return (smoke, leaves.Length > 0 ? leaves[0].GetComponent<ParticleSystemComponent>()?.Intensity ?? -1.0f : -1.0f);
	}

	private void CheckTheGoalAndTheGate()
	{
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

	private void CheckTheShakeAndStartTheTour()
	{
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
			CameraRig? rig = Rig?.As<CameraRig>();
			Assert.IsNotNull(tour);
			Assert.IsNotNull(rig);
			Assert.IsFalse(tour.IsPlaying, "the test scene starts without the tour");
			Assert.IsTrue(tour.ShotCount == 0 || tour.ShotCount >= 8);
			Assert.AreEqual(0.0f, tour.Fade?.GetComponent<UIRectComponent>()?.Opacity, "the screen is clear");
			Assert.AreEqual(0.0f, tour.LetterboxAmount, "no letterbox while exploring");
			m_TourStart = rig.Translation;
			tour.Play();
			Assert.IsTrue(tour.IsPlaying);
			Assert.IsTrue(tour.ShotCount >= 8, $"{tour.ShotCount} shots");
			Assert.IsFalse(rig.Controllable, "the tour has the camera");
			// It fades in from black.
			Assert.AreEqual(1.0f, tour.FadeOpacity);
			Assert.AreEqual(1.0f, tour.Fade?.GetComponent<UIRectComponent>()?.Opacity);
		});
		TestReporter.Run("the tour's shots cue exhibits that perform, and night falls on its way to the campfire", () =>
		{
			Flythrough? tour = Tour?.As<Flythrough>();
			Assert.IsNotNull(tour);
			int cues = 0;
			foreach (Entity child in tour.Children)
			{
				CameraShot? shot = child.As<CameraShot>();
				Assert.IsNotNull(shot, $"{child.Name} is a shot");
				if (shot.Cue is not null)
				{
					Assert.IsTrue(shot.Cue.As<Script>() is ITourCue, $"{child.Name} cues {shot.Cue.Name}, which performs");
					cues++;
				}
			}
			// The neon sign, the wrecking ball, the dominoes, the launcher and the night.
			Assert.AreEqual(5, cues);
			Entity? campfire = tour.Children.FirstOrDefault(child => child.Name == "Shot: Campfire");
			Assert.IsNotNull(campfire);
			Assert.AreEqual(NightInterlude?.ID, campfire.As<CameraShot>()?.Cue?.ID);
		});
	}

	private void CheckTheTourUnderWay()
	{
		TestReporter.Run("the tour moves the camera and opens with its title", () =>
		{
			Flythrough? tour = Tour?.As<Flythrough>();
			CameraRig? rig = Rig?.As<CameraRig>();
			Assert.IsNotNull(tour);
			Assert.IsNotNull(rig);
			Assert.IsTrue(tour.IsPlaying);
			Assert.IsTrue(Vector3.Distance(rig.Translation, m_TourStart) > 1.0f, "the camera left its place");
			Assert.IsTrue(tour.FadeOpacity < 0.1f, $"the fade is at {tour.FadeOpacity}");
			Assert.IsTrue(tour.TitleVisibility > 0.5f, $"the title shows at {tour.TitleVisibility}");
			Assert.AreEqual(0.0f, tour.CaptionVisibility);
			// The first shot cued the neon sign, switched on once before by the test.
			Assert.AreEqual(2, NeonSign?.As<NeonSign>()?.Ignitions);
		});
		TestReporter.Run("the tour letterboxes the screen, hides the HUD and plays the music up", () =>
		{
			Flythrough? tour = Tour?.As<Flythrough>();
			Assert.IsNotNull(tour?.Letterbox);
			Assert.AreEqual(1.0f, tour.LetterboxAmount);
			Assert.AreEqual(2, tour.Letterbox.Children.Length);
			foreach (Entity bar in tour.Letterbox.Children)
			{
				Assert.AreApproximatelyEqual(tour.LetterboxHeight, bar.GetComponent<UIRectComponent>()!.Size.Y, 1e-3f);
			}
			Assert.AreEqual(0.0f, tour.Hud?.GetComponent<UIRectComponent>()?.Opacity, "the HUD is hidden");
			float music = tour.Music?.GetComponent<AudioSourceComponent>()?.Volume ?? 0.0f;
			Assert.IsTrue(music > m_MusicVolume, $"the music plays up to {music}");
		});
	}

	private void CheckTheCaptionsAndSkip()
	{
		// About 4.5 seconds in, the title fades out while the camera nears the first shot with a caption.
		TestReporter.Run("captions wait until the title is gone, and a skip hands the camera back", () =>
		{
			Flythrough? tour = Tour?.As<Flythrough>();
			Assert.IsNotNull(tour);
			Assert.IsTrue(tour.IsPlaying);
			Assert.IsTrue(tour.TitleVisibility > 0.0f && tour.TitleVisibility < 1.0f, $"the title fades at {tour.TitleVisibility}");
			Assert.IsFalse(string.IsNullOrEmpty(tour.CaptionText?.GetComponent<UITextComponent>()?.Text), "a caption is due");
			Assert.AreEqual(0.0f, tour.CaptionVisibility);
			tour.Skip();
			Assert.IsTrue(tour.IsSkipping);
		});
	}

	private void CheckTheHandOver()
	{
		TestReporter.Run("after the tour the free camera has control", () =>
		{
			Flythrough? tour = Tour?.As<Flythrough>();
			CameraRig? rig = Rig?.As<CameraRig>();
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
	}

	private void CheckTheHudAfterTheTour()
	{
		TestReporter.Run("after the tour the letterbox opens, the HUD fades in and the music settles back", () =>
		{
			Flythrough? tour = Tour?.As<Flythrough>();
			Assert.IsNotNull(tour?.Letterbox);
			Assert.AreEqual(0.0f, tour.LetterboxAmount);
			foreach (Entity bar in tour.Letterbox.Children)
			{
				Assert.AreEqual(0.0f, bar.GetComponent<UIRectComponent>()!.Size.Y);
			}
			Assert.AreEqual(1.0f, tour.HudVisibility);
			Assert.AreEqual(1.0f, tour.Hud?.GetComponent<UIRectComponent>()?.Opacity);
			Assert.AreApproximatelyEqual(m_MusicVolume, tour.Music?.GetComponent<AudioSourceComponent>()?.Volume ?? -1.0f, 1e-4f,
				"the music settles back");
		});
		// The camera moves to the gallery, whose row of the feature list lights up.
		Hud? hud = Hud?.As<Hud>();
		HudFeature? materials = hud?.FeatureRows.FirstOrDefault(row => row.Radius > 0.0f);
		if (Rig?.As<CameraRig>() is { } rig && materials is not null)
		{
			rig.SetPose(materials.Center + new Vector3(0.0f, 2.0f, 0.0f), 0.0f, rig.Yaw);
		}
	}

	private void ClickTheHudButtons()
	{
		Hud? hud = Hud?.As<Hud>();
		m_BeforeClicks = (WreckingBall?.As<WreckingBall>()?.Swings ?? 0, Dominoes?.As<DominoRun>()?.Pushes ?? 0,
			Launcher?.As<BallLauncher>()?.Shots ?? 0, Playground?.As<Playground>()?.Resets ?? 0, Lighting?.As<DayNight>()?.Switches ?? 0);
		// Clicks in code land at the next frame's UI update, as the mouse's would.
		foreach (Entity? button in new[] { hud?.SwingButton, hud?.DominoesButton, hud?.LaunchButton, hud?.DayNightButton, hud?.ResetButton })
		{
			button?.GetComponent<UIButtonComponent>()?.Click();
		}
	}

	private void CheckTheFeatureList()
	{
		TestReporter.Run("the feature list lights up the exhibits around the camera", () =>
		{
			Hud? hud = Hud?.As<Hud>();
			CameraRig? rig = Rig?.As<CameraRig>();
			Assert.IsNotNull(hud);
			Assert.IsNotNull(rig);
			Assert.IsTrue(hud.FeatureRows.Count >= 8, $"{hud.FeatureRows.Count} rows");
			int lit = 0;
			foreach (HudFeature row in hud.FeatureRows.Where(row => row.Radius > 0.0f))
			{
				// Every row has come to rest at its nearness to the camera.
				Assert.AreApproximatelyEqual(row.Nearness(rig.Transform.WorldTranslation), row.Lit, 1e-3f, row.Name);
				lit += row.Lit > 0.99f ? 1 : 0;
			}
			Assert.IsTrue(lit >= 1, "the gallery's row is lit");
			Assert.IsTrue(lit < hud.FeatureRows.Count - 1, $"{lit} rows are lit");
			// Lit rows open for their detail lines, and the rows stack under each other.
			for (int i = 0; i < hud.FeatureRows.Count; i++)
			{
				HudFeature row = hud.FeatureRows[i];
				UIRectComponent rect = row.GetComponent<UIRectComponent>()!;
				Assert.AreApproximatelyEqual(row.ClosedHeight + (row.DetailHeight * Mathf.SmoothStep(0.0f, 1.0f, row.Lit)), rect.Size.Y, 1e-3f, row.Name);
				if (i > 0)
				{
					UIRectComponent above = hud.FeatureRows[i - 1].GetComponent<UIRectComponent>()!;
					Assert.AreApproximatelyEqual(above.Position.Y + above.Size.Y + hud.RowGap, rect.Position.Y, 1e-3f, row.Name);
				}
			}
			Assert.IsTrue(hud.FeatureRows[0].GetComponent<UIRectComponent>()!.Size.Y > hud.FeatureRows[0].ClosedHeight + 1.0f, "the gallery's row is open");
		});
	}

	private void CheckTheClicks()
	{
		TestReporter.Run("the HUD's buttons put the exhibits back, swing the ball, tip the dominoes, launch and switch day and night", () =>
		{
			Hud? hud = Hud?.As<Hud>();
			DayNight? lighting = Lighting?.As<DayNight>();
			Assert.IsNotNull(hud);
			Assert.IsNotNull(lighting);
			Assert.AreEqual(5, hud.Clicks);
			Assert.AreEqual(m_BeforeClicks.Swings + 1, WreckingBall?.As<WreckingBall>()?.Swings);
			Assert.AreEqual(m_BeforeClicks.Pushes + 1, Dominoes?.As<DominoRun>()?.Pushes);
			Assert.AreEqual(m_BeforeClicks.Shots + 1, Launcher?.As<BallLauncher>()?.Shots);
			Assert.AreEqual(m_BeforeClicks.Resets + 1, Playground?.As<Playground>()?.Resets);
			Assert.AreEqual(m_BeforeClicks.Switches + 1, lighting.Switches);
			Assert.IsTrue(lighting.IsNight, "night falls");
			lighting.SetNight(false);
			// The HUD hides and shows again (its key does this).
			hud.Shown = false;
			Assert.IsFalse(hud.GetComponent<UIRectComponent>()!.Visible);
			hud.Shown = true;
			Assert.IsTrue(hud.Shown);
		});
	}

	private void CheckTheRigOnItsFeet()
	{
		TestReporter.Run("F walks: the walker stands under the camera, which drops to its eyes", () =>
		{
			CameraRig? rig = Rig?.As<CameraRig>();
			Walker? walker = Walker?.As<Walker>();
			Assert.IsNotNull(rig);
			Assert.IsNotNull(walker);
			Assert.IsTrue(rig.IsWalking);
			Assert.IsTrue(rig.IsCursorCaptured, "the mouse looks around");
			Assert.IsTrue(walker.IsActive);
			Assert.IsTrue(walker.IsGrounded, "it stands");
			Assert.AreApproximatelyEqual(walker.EyePosition, rig.Translation, 0.05f, "the view is at its eyes");
			// F again: the rig flies on from there, and the walker waits out of the way.
			rig.SetWalking(false);
			Assert.IsFalse(rig.IsWalking);
			Assert.IsFalse(walker.IsActive);
			Assert.AreEqual(2, rig.ModeSwitches);
		});
	}

	// The walker is driven directly from here on (the rig flies and leaves it alone): forward along the heading, as far as the
	// range along the ground.
	private void WalkAlong(Vector3 feet, float yaw, float range = float.PositiveInfinity)
	{
		if (Walker?.As<Walker>() is { } walker)
		{
			walker.Enter(feet, yaw);
			walker.MoveInput = new Vector2(0.0f, 1.0f);
			walker.Running = false;
			m_WalkFrom = feet;
			m_WalkRange = range;
		}
	}

	private static float YawTowards(Vector3 from, Vector3 to) => Mathf.Atan2(-(to.X - from.X), -(to.Z - from.Z)) * Mathf.Rad2Deg;

	private static Vector3 Flat(Vector3 value) => new(value.X, 0.0f, value.Z);

	// From a meter in front of the first step, towards the deck.
	private void ClimbTheStairs()
	{
		if (FirstStep is null || Deck is null)
		{
			return;
		}
		Vector3 towards = Flat(Deck.Transform.WorldTranslation - FirstStep.Transform.WorldTranslation).Normalized;
		Vector3 start = Flat(FirstStep.Transform.WorldTranslation) - (towards * 1.0f);
		// Up the stairs to the middle of the deck.
		WalkAlong(start, YawTowards(start, Deck.Transform.WorldTranslation), Vector3.Distance(start, Flat(Deck.Transform.WorldTranslation)));
	}

	private void CheckTheStairs()
	{
		TestReporter.Run("the walker climbs the stairs onto the deck", () =>
		{
			Walker? walker = Walker?.As<Walker>();
			Assert.IsNotNull(walker);
			Assert.IsNotNull(Deck);
			float deckTop = Deck.Transform.WorldTranslation.Y + (Deck.Scale.Y * 0.5f);
			float feet = walker.Transform.WorldTranslation.Y;
			Assert.IsTrue(Mathf.Abs(feet - deckTop) < 0.08f, $"its feet are at {feet} m, the deck at {deckTop} m");
			Assert.IsTrue(walker.IsGrounded);
			Assert.IsTrue(walker.Steps >= 3, $"{walker.Steps} footsteps");
		});
	}

	// From the deck, a meter before the ramp's top, down the ramp.
	private void WalkDownTheRamp()
	{
		if (Ramp is null)
		{
			return;
		}
		Vector3 uphill = Ramp.Transform.Right;
		Vector3 top = Ramp.Transform.WorldTranslation + (uphill * (Ramp.Scale.X * 0.5f));
		Vector3 start = top + (Flat(uphill).Normalized * 1.0f) + new Vector3(0.0f, 0.1f, 0.0f);
		// Down to a meter and a half past its foot.
		float run = Ramp.Scale.X * Flat(uphill).Length;
		WalkAlong(start, YawTowards(start, Ramp.Transform.WorldTranslation), 1.0f + run + 1.5f);
	}

	private void CheckTheRamp()
	{
		TestReporter.Run("the walker walks down the ramp to the ground", () =>
		{
			Walker? walker = Walker?.As<Walker>();
			Assert.IsNotNull(walker);
			Assert.IsNotNull(Ramp);
			Vector3 feet = walker.Transform.WorldTranslation;
			Assert.IsTrue(feet.Y < 0.1f, $"its feet are at {feet.Y} m");
			Assert.IsTrue(walker.IsGrounded);
			float run = Vector3.Distance(Flat(feet), Flat(m_WalkFrom));
			Assert.IsTrue(run > Ramp.Scale.X, $"it walked {run} m");
		});
	}

	// Meters per second along the ground since the pace was last marked, over the seconds.
	private float PaceSince(float seconds) =>
		Vector3.Distance(Flat(Walker?.Transform.WorldTranslation ?? Vector3.Zero), Flat(m_PaceFrom)) / seconds;

	private void CheckTheSpeeds()
	{
		float runPace = PaceSince(0.5f);
		TestReporter.Run("the walker walks and runs at its speeds", () =>
		{
			Walker? walker = Walker?.As<Walker>();
			Assert.IsNotNull(walker);
			Assert.IsTrue(Mathf.Abs(m_WalkPace - walker.WalkSpeed) < 0.3f, $"it walked at {m_WalkPace} m/s");
			Assert.IsTrue(Mathf.Abs(runPace - walker.RunSpeed) < 0.4f, $"it ran at {runPace} m/s");
			walker.MoveInput = Vector2.Zero;
			walker.Running = false;
		});
	}

	private void CheckTheJump()
	{
		m_TrackJump = false;
		TestReporter.Run("the walker jumps and lands", () =>
		{
			Walker? walker = Walker?.As<Walker>();
			Assert.IsNotNull(walker);
			Assert.AreEqual(1, walker.Jumps);
			Assert.IsTrue(m_JumpPeak > 1.0f, $"its feet rose to {m_JumpPeak} m");
			Assert.IsTrue(walker.IsGrounded, "it is back on the ground");
			Assert.IsTrue(walker.Landings >= 1, "it landed");
		});
	}

	// From beyond a crate by the ramp's foot, towards the deck: the walker walks into the crate and shoves it.
	private void PushACrate()
	{
		Playground? playground = Playground?.As<Playground>();
		m_Crate = playground?.Movables?.Children.Where(crate => crate.Transform.WorldTranslation.Y < 0.3f).FirstOrDefault();
		if (m_Crate is null || Deck is null)
		{
			return;
		}
		m_CrateFrom = m_Crate.Transform.WorldTranslation;
		Vector3 away = Flat(m_CrateFrom - Deck.Transform.WorldTranslation).Normalized;
		Vector3 start = Flat(m_CrateFrom) + (away * 1.1f);
		WalkAlong(start, YawTowards(start, Deck.Transform.WorldTranslation), 2.4f);
	}

	private void CheckThePush()
	{
		TestReporter.Run("the walker pushes a crate", () =>
		{
			Assert.IsNotNull(m_Crate);
			float moved = Vector3.Distance(Flat(m_Crate.Transform.WorldTranslation), Flat(m_CrateFrom));
			Assert.IsTrue(moved > 0.3f, $"the crate moved {moved} m");
		});
		Walker?.As<Walker>()?.Park();
		Playground?.As<Playground>()?.PutBack();
	}

	private void CheckTheCratesPutBack()
	{
		TestReporter.Run("putting the exhibits back puts the yard's crates back too", () =>
		{
			Assert.IsNotNull(m_Crate);
			Assert.AreApproximatelyEqual(m_CrateFrom, m_Crate.Transform.WorldTranslation, 0.02f, "the pushed crate is back");
		});
	}

	// The walker steps onto the lift as it starts its cycle at the bottom.
	private void RideTheLift()
	{
		if (Lift?.As<Lift>() is not { } lift || Walker?.As<Walker>() is not { } walker)
		{
			return;
		}
		lift.Restart();
		walker.Enter(Lift.Transform.WorldTranslation + new Vector3(0.0f, Lift.Scale.Y * 0.5f + 0.05f, 0.0f), walker.Yaw);
		walker.MoveInput = Vector2.Zero;
	}

	private void CheckTheLift()
	{
		TestReporter.Run("the lift carries the walker up to the lookout", () =>
		{
			Lift? lift = Lift?.As<Lift>();
			Walker? walker = Walker?.As<Walker>();
			Assert.IsNotNull(lift);
			Assert.IsNotNull(walker);
			Assert.IsFalse(lift.IsMoving, "it waits at the top");
			Assert.AreApproximatelyEqual(lift.Rise, lift.Height, 1e-3f);
			Assert.IsTrue(lift.Trips >= 1);
			float top = Lift!.Transform.WorldTranslation.Y + (Lift.Scale.Y * 0.5f);
			float feet = walker.Transform.WorldTranslation.Y;
			Assert.IsTrue(Mathf.Abs(feet - top) < 0.08f, $"its feet are at {feet} m, the platform at {top} m");
			Assert.IsTrue(walker.IsGrounded);
			walker.Park();
		});
	}

	private void CheckTheModeButton()
	{
		TestReporter.Run("the HUD's walk button walks and its label offers to fly", () =>
		{
			Hud? hud = Hud?.As<Hud>();
			CameraRig? rig = Rig?.As<CameraRig>();
			Assert.IsNotNull(hud);
			Assert.IsNotNull(rig);
			Assert.IsTrue(rig.IsWalking);
			Assert.AreEqual("Fly", hud.ModeLabel?.GetComponent<UITextComponent>()?.Text);
			Assert.AreEqual(hud.WalkHint, hud.HintText?.GetComponent<UITextComponent>()?.Text);
		});
	}

	private void CheckTheFreeCursor()
	{
		TestReporter.Run("Esc frees the cursor while walking, and the hint line says how to look around again", () =>
		{
			Hud? hud = Hud?.As<Hud>();
			CameraRig? rig = Rig?.As<CameraRig>();
			Assert.IsNotNull(hud);
			Assert.IsNotNull(rig);
			Assert.IsTrue(rig.IsWalking, "it still walks");
			Assert.IsFalse(rig.IsCursorCaptured);
			Assert.IsFalse(rig.IsLooking, "the mouse no longer looks around");
			Assert.AreEqual(CursorMode.Normal, Input.CursorMode);
			Assert.AreEqual(hud.FreeCursorHint, hud.HintText?.GetComponent<UITextComponent>()?.Text);
			rig.SetWalking(false);
		});
	}

	private void StartTheInterlude(float seconds)
	{
		if (NightInterlude?.As<NightInterlude>() is { } interlude)
		{
			interlude.Seconds = seconds;
			interlude.Perform();
		}
	}

	private void CheckTheInterlude()
	{
		TestReporter.Run("the tour's night interlude falls and turns the lighting back to what it was", () =>
		{
			NightInterlude? interlude = NightInterlude?.As<NightInterlude>();
			DayNight? lighting = Lighting?.As<DayNight>();
			Assert.IsNotNull(interlude);
			Assert.IsNotNull(lighting);
			Assert.IsTrue(m_InterludeNight, "night fell");
			Assert.IsTrue(m_InterludeDay, "the day came back");
			Assert.IsFalse(interlude.IsActive);
			Assert.IsTrue(lighting.IsNight, "cued at night, it leaves the night");
		});
	}

	// The interlude sets the night; the visitor switches to day and back to night meanwhile, which the interlude leaves be.
	private void SwitchDuringTheInterlude()
	{
		if (Lighting?.As<DayNight>() is not { } lighting)
		{
			return;
		}
		lighting.SetNight(false);
		StartTheInterlude(0.6f);
		m_SwitchesBeforeChoice = lighting.Switches;
		lighting.Toggle();
		lighting.Toggle();
	}

	private void CheckTheVisitorsChoiceStands()
	{
		TestReporter.Run("switching day and night during the interlude keeps the visitor's choice", () =>
		{
			NightInterlude? interlude = NightInterlude?.As<NightInterlude>();
			DayNight? lighting = Lighting?.As<DayNight>();
			Assert.IsNotNull(interlude);
			Assert.IsNotNull(lighting);
			Assert.IsFalse(interlude.IsActive, "the interlude is over");
			// It found the night it set switched away and back: it switched nothing more.
			Assert.AreEqual(m_SwitchesBeforeChoice + 2, lighting.Switches);
			Assert.IsTrue(lighting.IsNight, "the night the visitor chose stays");
			lighting.SetNight(false);
		});
	}

	// Under the lift's platform, up at the lookout: its center, on the ground.
	private void StandUnderTheLift()
	{
		if (Lift is null || Walker?.As<Walker>() is not { } walker)
		{
			return;
		}
		Vector3 center = Lift.Transform.WorldTranslation;
		walker.Enter(new Vector3(center.X, 0.02f, center.Z), walker.Yaw);
		walker.MoveInput = Vector2.Zero;
	}

	private void CheckTheLiftWaits()
	{
		TestReporter.Run("the lift stops on its way down while the walker stands under it", () =>
		{
			Lift? lift = Lift?.As<Lift>();
			Walker? walker = Walker?.As<Walker>();
			Assert.IsNotNull(lift);
			Assert.IsNotNull(walker);
			Assert.IsTrue(lift.IsHeld, $"the lift is at {lift.Height} m");
			Assert.IsFalse(lift.IsMoving);
			Assert.AreEqual(1, lift.Holds);
			Assert.IsTrue(lift.Height < lift.Rise, "it had started down");
			// Its underside stays above the walker's head (the capsule is 1.8 m tall), which stays on the ground.
			float underside = Lift!.Transform.WorldTranslation.Y - (Lift.Scale.Y * 0.5f);
			float feet = walker.Transform.WorldTranslation.Y;
			Assert.IsTrue(underside > feet + 1.9f, $"the platform's underside is at {underside} m, the feet at {feet} m");
			Assert.IsTrue(feet < 0.1f && walker.IsGrounded, $"the walker stands at {feet} m");
			walker.Park();
		});
	}

	private void CheckTheLiftGoesOn()
	{
		TestReporter.Run("the lift goes on down once the walker is gone", () =>
		{
			Lift? lift = Lift?.As<Lift>();
			Assert.IsNotNull(lift);
			Assert.IsFalse(lift.IsHeld);
			Assert.IsTrue(lift.IsMoving, $"the lift is at {lift.Height} m");
		});
	}

	// The walker steps onto the pressure plate, as a visitor would.
	private void StepOnThePlate()
	{
		if (Plate?.As<PressurePlate>() is not { } plate || Walker?.As<Walker>() is not { } walker)
		{
			return;
		}
		m_PlatePresses = plate.Presses;
		Vector3 center = Plate.Transform.WorldTranslation;
		walker.Enter(new Vector3(center.X, 0.12f, center.Z), walker.Yaw);
		walker.MoveInput = Vector2.Zero;
	}

	private void CheckThePlateUnderTheWalker()
	{
		TestReporter.Run("the walker standing on the plate raises the gate", () =>
		{
			PressurePlate? plate = Plate?.As<PressurePlate>();
			Walker? walker = Walker?.As<Walker>();
			Assert.IsNotNull(plate);
			Assert.IsNotNull(walker);
			Assert.IsTrue(plate.IsPressed);
			Assert.AreEqual(m_PlatePresses + 1, plate.Presses);
			Assert.IsTrue(plate.Openness > 0.5f, $"the gate is {plate.Openness} open");
			walker.Park();
		});
	}

	private void CheckWhereTheRigFlies()
	{
		TestReporter.Run("the camera flies within the clearing, and walks only where there is ground", () =>
		{
			CameraRig? rig = Rig?.As<CameraRig>();
			Assert.IsNotNull(rig);
			Assert.IsTrue(rig.FlightRadius > 0.0f);
			// Out at the edge of where it flies, flying on outwards slides along the edge.
			rig.SetPose(new Vector3(rig.FlightRadius - 1.0f, 3.0f, 0.0f), 0.0f, -90.0f);
			rig.Fly(new Vector3(20.0f, 0.0f, 20.0f), 1.0f);
			Assert.AreApproximatelyEqual(rig.FlightRadius, Flat(rig.Translation).Length, 1e-3f);
			Assert.IsTrue(rig.Translation.Z > 1.0f, $"it slid along the edge to {rig.Translation}");
			rig.Fly(new Vector3(0.0f, 5000.0f, 0.0f), 1.0f);
			Assert.AreApproximatelyEqual(rig.MaximumHeight, rig.Translation.Y, 1e-3f);
			// Put far out (the tour or a script may), it only comes nearer.
			rig.SetPose(new Vector3(500.0f, 5.0f, 0.0f), 0.0f, -90.0f);
			rig.Fly(new Vector3(10.0f, 0.0f, 0.0f), 1.0f);
			Assert.AreApproximatelyEqual(500.0f, rig.Translation.X, 1e-3f);
			rig.Fly(new Vector3(-10.0f, 0.0f, 0.0f), 1.0f);
			Assert.AreApproximatelyEqual(490.0f, rig.Translation.X, 1e-3f);
			// Nothing is under it out there: F does not walk.
			Assert.IsFalse(rig.SetWalking(true));
			Assert.IsFalse(rig.IsWalking);
			Assert.IsFalse(Walker?.As<Walker>()?.IsActive ?? true, "the walker waits");
		});
	}

	// Out towards the edge of the walker's area, across open ground.
	private void WalkToTheEdge()
	{
		if (Walker?.As<Walker>() is { } walker)
		{
			WalkAlong(new Vector3(walker.AreaRadius - 1.5f, 0.04f, 0.0f), -90.0f);
		}
	}

	private void CheckTheEdge()
	{
		TestReporter.Run("the plate lets go once the walker has stepped off", () =>
		{
			PressurePlate? plate = Plate?.As<PressurePlate>();
			Assert.IsNotNull(plate);
			Assert.IsFalse(plate.IsPressed);
		});
		TestReporter.Run("the walker stops at the edge of the clearing's ground", () =>
		{
			Walker? walker = Walker?.As<Walker>();
			Assert.IsNotNull(walker);
			Assert.IsTrue(walker.AreaRadius > 0.0f);
			Vector3 feet = walker.Transform.WorldTranslation;
			float distance = Flat(feet).Length;
			Assert.IsTrue(distance > walker.AreaRadius - 0.2f && distance < walker.AreaRadius + 0.2f, $"it walked {distance} m out");
			Assert.IsTrue(walker.IsGrounded && Mathf.Abs(feet.Y) < 0.1f, $"it stands at {feet.Y} m");
			walker.Park();
		});
	}

	// The exhibits are down; the tour plays again (with a night of its own under way).
	private void ReplayTheTour()
	{
		TestReporter.Run("the exhibits went off before the tour plays again", () =>
		{
			Playground? playground = Playground?.As<Playground>();
			DominoRun? run = Dominoes?.As<DominoRun>();
			Flythrough? tour = Tour?.As<Flythrough>();
			Assert.IsNotNull(playground);
			Assert.IsNotNull(run);
			Assert.IsNotNull(tour);
			Assert.IsTrue(playground.Knocked > 0, $"{playground.Knocked} knocked down");
			Assert.IsTrue(run.Fallen > 0, $"{run.Fallen} dominoes fell");
			m_ResetsBeforeReplay = playground.Resets;
			Lighting?.As<DayNight>()?.SetNight(false);
			StartTheInterlude(9.0f);
			tour.Replay();
		});
	}

	private void CheckTheReplayAndSkip()
	{
		TestReporter.Run("the tour plays again with the exhibits put back", () =>
		{
			Playground? playground = Playground?.As<Playground>();
			DominoRun? run = Dominoes?.As<DominoRun>();
			Flythrough? tour = Tour?.As<Flythrough>();
			Assert.IsNotNull(playground);
			Assert.IsNotNull(run);
			Assert.IsNotNull(tour);
			Assert.IsTrue(tour.IsPlaying);
			Assert.AreEqual(m_ResetsBeforeReplay + 1, playground.Resets);
			Assert.AreEqual(0, playground.Knocked);
			Assert.AreEqual(0, run.Fallen);
		});
		TestReporter.Run("skipping the tour ends its night at once", () =>
		{
			Flythrough? tour = Tour?.As<Flythrough>();
			NightInterlude? interlude = NightInterlude?.As<NightInterlude>();
			DayNight? lighting = Lighting?.As<DayNight>();
			Assert.IsNotNull(tour);
			Assert.IsNotNull(interlude);
			Assert.IsNotNull(lighting);
			Assert.IsTrue(interlude.IsActive && lighting.IsNight, "night fell");
			tour.Skip();
			Assert.IsFalse(interlude.IsActive);
			Assert.IsFalse(lighting.IsNight, "the day comes back");
		});
	}

	private void CheckTheSecondHandOver()
	{
		TestReporter.Run("the replayed tour hands the camera over again", () =>
		{
			Flythrough? tour = Tour?.As<Flythrough>();
			CameraRig? rig = Rig?.As<CameraRig>();
			Assert.IsNotNull(tour);
			Assert.IsNotNull(rig);
			Assert.IsFalse(tour.IsPlaying);
			Assert.AreEqual(2, tour.HandOvers);
			Assert.IsTrue(rig.Controllable);
		});
	}

	// Meters from a point to the volume's box: the unit cube under its transform (its parents are not scaled); 0 inside.
	private static float DistanceToBox(Entity volume, Vector3 point)
	{
		Vector3 offset = point - volume.Transform.WorldTranslation;
		Vector3 half = volume.Scale * 0.5f;
		float x = Mathf.Max(0.0f, Mathf.Abs(Vector3.Dot(offset, volume.Transform.Right)) - half.X);
		float y = Mathf.Max(0.0f, Mathf.Abs(Vector3.Dot(offset, volume.Transform.Up)) - half.Y);
		float z = Mathf.Max(0.0f, Mathf.Abs(Vector3.Dot(offset, volume.Transform.Forward)) - half.Z);
		return new Vector3(x, y, z).Length;
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
			CameraRig? rig = Rig?.As<CameraRig>();
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
			CameraRig? rig = Rig?.As<CameraRig>();
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
