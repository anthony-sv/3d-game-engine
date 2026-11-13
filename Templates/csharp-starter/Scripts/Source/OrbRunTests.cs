using System;
using System.Collections.Generic;
using System.Globalization;
using Strada;
using Strada.Testing;

namespace Game;

/// <summary>The checks of Scenes/Tests.sscene: Orb Run's rules, its screen and its flow, one after another. Each check acts,
/// waits for what it expects (at most a few seconds of the game, so the checks pass at any frame rate) and reports it; the
/// test run ends once every check has reported. Play the scene, or run it headless with the player:
/// <c>StradaRuntime --project &lt;the project file&gt; --scene Scenes/Tests.sscene --test</c>.</summary>
public sealed class OrbRunTests : Script
{
	[Tooltip("The entity whose OrbRun the checks play")]
	public Entity? Game;

	[Tooltip("The orb")]
	public Entity? Orb;

	[Tooltip("The container of the run's sparks")]
	public Entity? Sparks;

	[Tooltip("The HUD's spark counter")]
	public Entity? SparksText;

	[Tooltip("The HUD's seconds left")]
	public Entity? TimeText;

	[Tooltip("The win screen")]
	public Entity? WinScreen;

	[Tooltip("The win screen's line with the seconds to spare")]
	public Entity? WinDetail;

	[Tooltip("The lose screen")]
	public Entity? LoseScreen;

	[Tooltip("The lose screen's line with the sparks found")]
	public Entity? LoseDetail;

	[Tooltip("The prompt to play again")]
	public Entity? Prompt;

	// The sparks a run places.
	private const int SparkCount = 12;
	// Physics steps the orb stays on each spark it is put on.
	private const int FramesOnSpark = 4;

	// One check: what it does first, what it does every frame while it waits, when it is done, how long it may wait
	// (seconds), and what it reports, which fails with a detail unless what it expects holds.
	private sealed record Check(string Name, Action? Begin, Action? Tick, Func<bool> Done, float Limit, Action Report);

	private readonly List<Check> m_Checks = [];
	private int m_Current = -1;
	private bool m_Finished;
	private float m_Time;
	private int m_Frames;
	private OrbRun? m_Run;
	private Orb? m_Orb;
	// What the checks remember between their steps.
	private float m_Before;
	private Entity? m_Target;
	private readonly Queue<Entity> m_Left = new();

	protected override void OnCreate()
	{
		m_Run = Game?.As<OrbRun>();
		m_Orb = Orb?.As<Orb>();
		AddChecks();
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (m_Finished)
		{
			return;
		}
		if (m_Run is null || m_Orb is null)
		{
			TestReporter.Fail("Orb Run: the test scene names its game and its orb", "Game or Orb is not set, or runs no script");
			Finish();
			return;
		}
		if (m_Current < 0)
		{
			// Every entity has run its OnCreate: the first run has started.
			Next();
			return;
		}
		Check check = m_Checks[m_Current];
		m_Time += deltaTime;
		m_Frames++;
		check.Tick?.Invoke();
		if (check.Done() || m_Time > check.Limit)
		{
			TestReporter.Run($"Orb Run: {check.Name}", check.Report);
			Next();
		}
	}

	private void Next()
	{
		m_Current++;
		m_Time = 0.0f;
		m_Frames = 0;
		if (m_Current >= m_Checks.Count)
		{
			Finish();
			return;
		}
		m_Checks[m_Current].Begin?.Invoke();
	}

	private void Finish()
	{
		m_Finished = true;
		TestReporter.Finish();
	}

	private void AddChecks()
	{
		OrbRun run = m_Run!;
		Orb orb = m_Orb!;

		Add("a run starts with twelve sparks and a full clock", null, () => CountSparks() == SparkCount && run.State == RunState.Playing, 2.0f,
			() =>
			{
				Assert.AreEqual(SparkCount, CountSparks(), "the sparks placed");
				Assert.AreEqual(SparkCount, run.Total, "the run's total");
				Assert.AreEqual(RunState.Playing, run.State, "the state");
				Assert.IsTrue(orb.Rolling, "the orb does not roll");
			});

		Add("the sparks float where Hover, a C# node of Hover.sgraph, puts them", null, () => m_Frames >= 3, 1.0f, () =>
		{
			bool raised = false;
			foreach (Entity entity in Sparks!.Children)
			{
				Spark spark = entity.As<Spark>()!;
				float clock = entity.GetComponent<VisualScriptComponent>()!.GetVariable<float>("Clock");
				Vector3 gem = entity.Children[0].Translation;
				Assert.AreApproximatelyEqual(spark.Hover(clock), gem, 1e-4f, $"the gem of {entity.Name} at {clock} s");
				raised |= Mathf.Abs(gem.Y) > 0.01f;
			}
			Assert.IsTrue(raised, "no gem bobs");
		});

		Add("the clock runs down in seconds of the game", () => m_Before = run.TimeLeft, null, () => m_Time >= 1.0f, 1.5f,
			() => Assert.AreApproximatelyEqual(1.0f, m_Before - run.TimeLeft, 0.05f, "the seconds off the clock in a second"));

		Add("the HUD shows the sparks and the seconds left", null, () => HudShowsStart(run), 1.0f, () =>
		{
			Assert.AreEqual($"0 / {SparkCount}", TextOf(SparksText), "the counter");
			Assert.AreEqual(Whole(run.TimeLeft), TextOf(TimeText), "the clock");
		});

		Add("the orb collects a spark it reaches", () =>
		{
			m_Target = Sparks!.Children[0];
			RollOnto(m_Target);
		}, null, () => !m_Target!.IsValid && run.Collected == 1, 2.0f, () =>
		{
			Assert.IsFalse(m_Target!.IsValid, "the spark is still there");
			Assert.AreEqual(1, run.Collected, "the sparks collected");
			Assert.AreEqual(SparkCount - 1, CountSparks(), "the sparks left");
		});

		Add("the HUD counts the spark", null, () => TextOf(SparksText) == $"1 / {SparkCount}", 1.0f,
			() => Assert.AreEqual($"1 / {SparkCount}", TextOf(SparksText), "the counter"));

		Add("the last ten seconds turn the clock red", () => run.TimeLeft = 9.5f, null, () => TextOf(TimeText) == "10" && IsRed(TimeText), 1.0f,
			() =>
			{
				Assert.AreEqual("10", TextOf(TimeText), "the clock");
				Assert.IsTrue(IsRed(TimeText), $"the clock's colour is {ColorOf(TimeText)}");
			});

		Add("collecting every spark wins", () =>
		{
			// Time enough for every spark.
			run.TimeLeft = 30.0f;
			m_Left.Clear();
			foreach (Entity spark in Sparks!.Children)
			{
				m_Left.Enqueue(spark);
			}
		}, () =>
		{
			if (m_Frames % FramesOnSpark == 1 && m_Left.TryDequeue(out Entity? spark))
			{
				RollOnto(spark);
			}
		}, () => run.State == RunState.Won, 6.0f, () =>
		{
			Assert.AreEqual(RunState.Won, run.State, "the state");
			Assert.AreEqual(SparkCount, run.Collected, "the sparks collected");
		});

		Add("the win screen shows the seconds to spare", null, () => IsVisible(WinScreen) && TextOf(WinDetail) == SecondsToSpare(run), 1.0f, () =>
		{
			Assert.IsTrue(IsVisible(WinScreen), "the win screen is hidden");
			Assert.IsFalse(IsVisible(LoseScreen), "the lose screen shows");
			Assert.IsTrue(IsVisible(Prompt), "the prompt is hidden");
			Assert.AreEqual(SecondsToSpare(run), TextOf(WinDetail), "the win screen's line");
		});

		Add("the orb stops rolling when the run ends", null, () => !orb.Rolling, 1.0f, () => Assert.IsFalse(orb.Rolling, "the orb still rolls"));

		Add("a new run starts fresh", run.StartRun, null, () => IsFresh(run), 2.0f, () =>
		{
			Assert.AreEqual(RunState.Playing, run.State, "the state");
			Assert.AreEqual(SparkCount, CountSparks(), "the sparks placed");
			Assert.AreEqual(0, run.Collected, "the sparks collected");
			Assert.AreApproximatelyEqual(run.TimeLimit, run.TimeLeft, 0.2f, "the seconds left");
			Assert.AreApproximatelyEqual(run.Start, Orb!.Translation, 0.3f, "the orb's place");
			Assert.IsFalse(IsVisible(WinScreen), "the win screen shows");
		});

		Add("running out of time loses", () => run.TimeLeft = 0.3f, null, () => run.State == RunState.Lost && IsVisible(LoseScreen), 2.0f, () =>
		{
			Assert.AreEqual(RunState.Lost, run.State, "the state");
			Assert.IsTrue(IsVisible(LoseScreen), "the lose screen is hidden");
		});

		Add("the lose screen counts the sparks", null, () => TextOf(LoseDetail) == $"0 OF {SparkCount} SPARKS" && IsVisible(Prompt), 1.0f, () =>
		{
			Assert.AreEqual($"0 OF {SparkCount} SPARKS", TextOf(LoseDetail), "the lose screen's line");
			Assert.IsTrue(IsVisible(Prompt), "the prompt is hidden");
		});

		Add("a run after a loss plays again", run.StartRun, null, () => run.State == RunState.Playing && !IsVisible(LoseScreen) && !IsVisible(Prompt),
			2.0f, () =>
			{
				Assert.AreEqual(RunState.Playing, run.State, "the state");
				Assert.IsFalse(IsVisible(LoseScreen), "the lose screen shows");
				Assert.IsFalse(IsVisible(Prompt), "the prompt shows");
				Assert.IsTrue(orb.Rolling, "the orb does not roll");
			});
	}

	private void Add(string name, Action? begin, Action? tick, Func<bool> done, float limit, Action report) =>
		m_Checks.Add(new Check(name, begin, tick, done, limit, report));

	private void Add(string name, Action? begin, Func<bool> done, float limit, Action report) => Add(name, begin, null, done, limit, report);

	// Puts the orb where a spark stands, still.
	private void RollOnto(Entity spark)
	{
		Vector3 spot = spark.Transform.WorldTranslation;
		RigidBodyComponent body = Orb!.GetComponent<RigidBodyComponent>()!;
		body.Teleport(new Vector3(spot.X, 0.5f, spot.Z), Quaternion.Identity);
		body.LinearVelocity = Vector3.Zero;
	}

	private int CountSparks() => Sparks!.Children.Length;

	private bool HudShowsStart(OrbRun run) => TextOf(SparksText) == $"0 / {SparkCount}" && TextOf(TimeText) == Whole(run.TimeLeft);

	private bool IsFresh(OrbRun run) =>
		run.State == RunState.Playing && CountSparks() == SparkCount && run.Collected == 0 && Mathf.Abs(run.TimeLeft - run.TimeLimit) <= 0.2f &&
		Vector3.Distance(run.Start, Orb!.Translation) <= 0.3f && !IsVisible(WinScreen);

	private static string Whole(float seconds) => ((int)Mathf.Ceil(seconds)).ToString(CultureInfo.InvariantCulture);

	private static string SecondsToSpare(OrbRun run) => $"{Whole(run.TimeLeft)} SECONDS TO SPARE";

	private static string TextOf(Entity? element) => element?.GetComponent<UITextComponent>()?.Text ?? string.Empty;

	private static Color ColorOf(Entity? element) => element?.GetComponent<UITextComponent>()?.Color ?? default;

	private static bool IsRed(Entity? element)
	{
		Color color = ColorOf(element);
		return color.R > 0.9f && color.G < 0.5f;
	}

	private static bool IsVisible(Entity? element) => element?.GetComponent<UIRectComponent>()?.Visible ?? false;
}
