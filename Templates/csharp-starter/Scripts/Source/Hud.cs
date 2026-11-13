using System;
using System.Globalization;
using Strada;

namespace Game;

/// <summary>The screen: the sparks and the seconds left, the hint at the start of a run, and the win and lose screens with
/// the prompt to play again. It reads the run every frame and changes what changed.</summary>
public sealed class Hud : Script
{
	[Tooltip("The entity whose OrbRun the screen reads")]
	public Entity? Game;

	[Tooltip("The text that counts the sparks")]
	public Entity? SparksText;

	[Tooltip("The text that shows the seconds left")]
	public Entity? TimeText;

	[Tooltip("The goal and the controls, shown at the start of a run")]
	public Entity? Hint;

	[Tooltip("The screen of a won run")]
	public Entity? WinScreen;

	[Tooltip("The win screen's line with the seconds to spare")]
	public Entity? WinDetail;

	[Tooltip("The screen of a run whose time ran out")]
	public Entity? LoseScreen;

	[Tooltip("The lose screen's line with the sparks collected")]
	public Entity? LoseDetail;

	[Tooltip("The prompt to play again, under either screen")]
	public Entity? Prompt;

	[Tooltip("The tick of each of the last ten seconds")]
	public Entity? TickSound;

	[Tooltip("The colour of the seconds left")]
	public Color TimeColor = new(0.96f, 0.98f, 1.0f, 1.0f);

	[Tooltip("The colour of the last ten seconds")]
	public Color UrgentColor = new(1.0f, 0.25f, 0.2f, 1.0f);

	// The hint fades in, stays and fades out (seconds).
	private const float HintFadeIn = 0.4f;
	private const float HintFadeOut = 3.5f;
	private const float HintLength = 4.5f;
	// The prompt breathes once in this many seconds.
	private const float PromptBreath = 1.4f;
	// The clock's size, and the last seconds that turn it red.
	private const float ClockSize = 72.0f;
	private const int UrgentSeconds = 10;

	// What the screen shows, so it changes only what changed.
	private int m_ShownRun = -1;
	private RunState m_ShownState;
	private int m_ShownCollected = -1;
	private int m_ShownSeconds = -1;
	private float m_HintTime = HintLength;
	private float m_PromptTime;

	protected override void OnUpdate(float deltaTime)
	{
		// Reads the run only when the Game entity runs one.
		if (Game?.As<OrbRun>() is not OrbRun run)
		{
			return;
		}
		ShowRun(run);
		ShowSparks(run);
		ShowClock(run);
		Fade(deltaTime);
	}

	// Each state of each run shows its screen.
	private void ShowRun(OrbRun run)
	{
		if (run.Runs == m_ShownRun && run.State == m_ShownState)
		{
			return;
		}
		m_ShownRun = run.Runs;
		m_ShownState = run.State;
		m_ShownCollected = -1;
		bool playing = run.State == RunState.Playing;
		SetVisible(WinScreen, run.State == RunState.Won);
		SetVisible(LoseScreen, run.State == RunState.Lost);
		SetVisible(Prompt, !playing);
		// A run starts with the hint; the end screens hide it.
		m_HintTime = playing ? 0.0f : HintLength;
		m_PromptTime = 0.0f;
		if (run.State == RunState.Won)
		{
			SetText(WinDetail, Format($"{WholeSeconds(run.TimeLeft)} SECONDS TO SPARE"));
		}
		else if (run.State == RunState.Lost)
		{
			SetText(LoseDetail, Format($"{run.Collected} OF {run.Total} SPARKS"));
		}
	}

	// The counter changes when the run's count does.
	private void ShowSparks(OrbRun run)
	{
		if (run.Collected != m_ShownCollected)
		{
			m_ShownCollected = run.Collected;
			SetText(SparksText, Format($"{run.Collected} / {run.Total}"));
		}
	}

	// Whole seconds, red and ticking in the last ten, each of which starts large and shrinks.
	private void ShowClock(OrbRun run)
	{
		if (TimeText?.GetComponent<UITextComponent>() is not UITextComponent clock)
		{
			return;
		}
		int seconds = WholeSeconds(run.TimeLeft);
		if (seconds != m_ShownSeconds)
		{
			m_ShownSeconds = seconds;
			clock.Text = Format($"{seconds}");
			clock.Color = seconds <= UrgentSeconds ? UrgentColor : TimeColor;
			if (seconds <= UrgentSeconds && seconds > 0 && run.State == RunState.Playing)
			{
				TickSound?.GetComponent<AudioSourceComponent>()?.Play();
			}
		}
		float left = run.TimeLeft;
		float pulse = left <= UrgentSeconds ? 0.35f * Mathf.Pow(left - Mathf.Floor(left), 3.0f) : 0.0f;
		clock.FontSize = ClockSize * (1.0f + pulse);
	}

	// The hint fades in and out at the start of a run, and the prompt to play again breathes.
	private void Fade(float deltaTime)
	{
		if (Hint?.GetComponent<UIRectComponent>() is UIRectComponent hint)
		{
			m_HintTime = Mathf.Min(m_HintTime + deltaTime, HintLength);
			hint.Visible = m_HintTime < HintLength;
			hint.Opacity = m_HintTime < HintFadeIn ? m_HintTime / HintFadeIn
				: m_HintTime < HintFadeOut ? 1.0f
				: (HintLength - m_HintTime) / (HintLength - HintFadeOut);
		}
		if (Prompt?.GetComponent<UIRectComponent>() is UIRectComponent prompt)
		{
			m_PromptTime = Mathf.Repeat(m_PromptTime + deltaTime, PromptBreath);
			prompt.Opacity = 0.675f + (0.325f * Mathf.Cos(m_PromptTime / PromptBreath * 2.0f * Mathf.PI));
		}
	}

	private static int WholeSeconds(float seconds) => (int)Mathf.Ceil(seconds);

	// Numbers in the invariant culture: in some languages they take characters the font lacks.
	private static string Format(FormattableString text) => text.ToString(CultureInfo.InvariantCulture);

	private static void SetVisible(Entity? element, bool visible)
	{
		if (element?.GetComponent<UIRectComponent>() is UIRectComponent rect)
		{
			rect.Visible = visible;
		}
	}

	private static void SetText(Entity? element, string text)
	{
		if (element?.GetComponent<UITextComponent>() is UITextComponent label)
		{
			label.Text = text;
		}
	}
}
