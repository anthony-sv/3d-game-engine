using System.Collections.Generic;
using System.Globalization;
using Strada;

namespace Showcase;

/// <summary>The heads-up display, on a UI element of the screen's canvas: the feature list lights up and opens the rows of the
/// exhibits around the camera (and the scripting row while the mouse is over the HUD), the status shows the props knocked down
/// and the goals scored, and the buttons put the exhibits back, swing the wrecking ball, launch a ball and switch day and night.
/// A button lights up as its exhibit's key is pressed too. Its key hides it; the tour hides it while it plays.</summary>
public sealed class Hud : Script
{
	[Tooltip("The camera rig (a FlyCamera): the features near it light up")]
	public Entity? Rig;

	[Tooltip("The playground (a Playground): its count of props knocked down shows, and the reset button puts everything back")]
	public Entity? Playground;

	[Tooltip("The goal (a GoalZone) whose count shows")]
	public Entity? Goal;

	[Tooltip("The wrecking ball (a WreckingBall) the swing button pushes")]
	public Entity? WreckingBall;

	[Tooltip("The ball launcher (a BallLauncher) the launch button fires")]
	public Entity? Launcher;

	[Tooltip("The day and night switch (a DayNight) the lights button turns")]
	public Entity? Lighting;

	[Tooltip("The parent of the feature list's rows (HudFeature scripts), which stacks them")]
	public Entity? Features;

	[Tooltip("The feature list's panel, which grows and shrinks with its rows")]
	public Entity? FeaturePanel;

	[Tooltip("Canvas units between the rows of the feature list")]
	[Range(0.0f, 50.0f)]
	public float RowGap = 4.0f;

	[Tooltip("UI text that shows the props knocked down")]
	public Entity? KnockedText;

	[Tooltip("UI text that shows the goals")]
	public Entity? GoalsText;

	[Tooltip("The button that puts the exhibits back")]
	public Entity? ResetButton;

	[Tooltip("The button that swings the wrecking ball")]
	public Entity? SwingButton;

	[Tooltip("The button that launches a ball")]
	public Entity? LaunchButton;

	[Tooltip("The button that switches day and night")]
	public Entity? LightsButton;

	[Tooltip("The lights button's icon (a UI image): the sun by day, the moon by night")]
	public Entity? LightsIcon;

	[Tooltip("The icon the lights button shows by day")]
	public Texture? SunIcon;

	[Tooltip("The icon the lights button shows at night")]
	public Texture? MoonIcon;

	[Tooltip("An audio source with the tick played as the mouse moves onto a button")]
	public Entity? HoverSound;

	[Tooltip("The key that hides and shows the HUD")]
	public KeyCode HideKey = KeyCode.H;

	[Tooltip("Seconds a button stays lit after it is clicked or its key is pressed")]
	[Range(0.05f, 2.0f)]
	public float FlashSeconds = 0.35f;

	private readonly Dictionary<ulong, (Color Normal, Color Flash, float Left)> m_Flashes = [];
	private HudFeature[] m_Features = [];
	private Entity[] m_Buttons = [];
	private Entity? m_Hovered;
	private bool m_ShowsMoon;
	private float m_PanelPadding;
	private (int Knocked, int Total, int Goals) m_Shown = (-1, -1, -1);

	/// <summary>The rows of the feature list.</summary>
	public IReadOnlyList<HudFeature> FeatureRows => m_Features;

	/// <summary>Buttons clicked so far.</summary>
	public int Clicks { get; private set; }

	/// <summary>Whether the HUD shows (its key hides it).</summary>
	public bool Shown
	{
		get => GetComponent<UIRectComponent>()?.Visible ?? false;
		set
		{
			if (GetComponent<UIRectComponent>() is { } rect)
			{
				rect.Visible = value;
			}
		}
	}

	protected override void OnCreate()
	{
		List<HudFeature> features = [];
		foreach (Entity row in Features?.Children ?? [])
		{
			if (row.As<HudFeature>() is { } feature)
			{
				features.Add(feature);
			}
		}
		m_Features = [.. features];
		List<Entity> buttons = [];
		foreach (Entity? button in new[] { ResetButton, SwingButton, LaunchButton, LightsButton })
		{
			if (button?.GetComponent<UIButtonComponent>() is { } component)
			{
				buttons.Add(button);
				m_Flashes[button.ID] = (component.NormalColor, component.HoveredColor, 0.0f);
			}
		}
		m_Buttons = [.. buttons];
		// The space the panel leaves under the list, as authored.
		if (Features?.GetComponent<UIRectComponent>() is { } list && FeaturePanel?.GetComponent<UIRectComponent>() is { } panel)
		{
			m_PanelPadding = panel.Size.Y - list.Position.Y - list.Size.Y;
		}
		PlaceFeatures();
		ShowStatus();
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsKeyPressed(HideKey))
		{
			Shown = !Shown;
		}
		ShowStatus();
		LightFeatures(deltaTime);
		FlashOnKeys();
		Flash(deltaTime);
		TickOnHover();
		bool night = Lighting?.As<DayNight>()?.IsNight ?? false;
		if (night != m_ShowsMoon && LightsIcon?.GetComponent<UIImageComponent>() is { } icon)
		{
			icon.Texture = night ? MoonIcon : SunIcon;
			m_ShowsMoon = night;
		}
	}

	protected override void OnClick(Entity button)
	{
		if (button == ResetButton)
		{
			Playground?.As<Playground>()?.PutBack();
		}
		else if (button == SwingButton)
		{
			WreckingBall?.As<WreckingBall>()?.Swing();
		}
		else if (button == LaunchButton)
		{
			Launcher?.As<BallLauncher>()?.Fire();
		}
		else if (button == LightsButton)
		{
			Lighting?.As<DayNight>()?.Toggle();
		}
		else
		{
			return;
		}
		Clicks++;
		StartFlash(button);
		if (GetComponent<AudioSourceComponent>() is { } click)
		{
			click.Stop();
			click.Play();
		}
	}

	// The texts change when the counts do.
	private void ShowStatus()
	{
		Playground? playground = Playground?.As<Playground>();
		(int Knocked, int Total, int Goals) counts = (playground?.Knocked ?? 0, playground?.Total ?? 0, Goal?.As<GoalZone>()?.Count ?? 0);
		if (counts == m_Shown)
		{
			return;
		}
		m_Shown = counts;
		if (KnockedText?.GetComponent<UITextComponent>() is { } knocked)
		{
			knocked.Text = string.Create(CultureInfo.InvariantCulture, $"{counts.Knocked} / {counts.Total}");
		}
		if (GoalsText?.GetComponent<UITextComponent>() is { } goals)
		{
			goals.Text = counts.Goals.ToString(CultureInfo.InvariantCulture);
		}
	}

	// Rows light up while the camera is near their exhibits; the row without an exhibit (the UI's own) while the mouse is
	// over the HUD.
	private void LightFeatures(float deltaTime)
	{
		Vector3 camera = Rig?.Transform.WorldTranslation ?? Vector3.Zero;
		bool overHud = Input.IsPointerOverUI;
		bool changed = false;
		foreach (HudFeature feature in m_Features)
		{
			changed |= feature.Animate(feature.Radius > 0.0f ? feature.Nearness(camera) : overHud ? 1.0f : 0.0f, deltaTime);
		}
		if (changed)
		{
			PlaceFeatures();
		}
	}

	// The rows stack from the top of the list at their heights (lit rows open for their detail lines); the panel fits them.
	private void PlaceFeatures()
	{
		float top = 0.0f;
		foreach (HudFeature feature in m_Features)
		{
			feature.Place(top);
			top += feature.Height + RowGap;
		}
		float height = Mathf.Max(0.0f, top - RowGap);
		if (Features?.GetComponent<UIRectComponent>() is { } list)
		{
			list.Size = new Vector2(list.Size.X, height);
			if (FeaturePanel?.GetComponent<UIRectComponent>() is { } panel)
			{
				panel.Size = new Vector2(panel.Size.X, list.Position.Y + height + m_PanelPadding);
			}
		}
	}

	// The exhibits' own keys light up their buttons, so the HUD shows what the keys do.
	private void FlashOnKeys()
	{
		if (Playground?.As<Playground>() is { } playground && Input.IsKeyPressed(playground.Key))
		{
			StartFlash(ResetButton);
		}
		if (WreckingBall?.As<WreckingBall>() is { } ball && Input.IsKeyPressed(ball.Key))
		{
			StartFlash(SwingButton);
		}
		if (Launcher?.As<BallLauncher>() is { } launcher && Input.IsKeyPressed(launcher.Key))
		{
			StartFlash(LaunchButton);
		}
		if (Lighting?.As<DayNight>() is { } lighting && Input.IsKeyPressed(lighting.Key))
		{
			StartFlash(LightsButton);
		}
	}

	private void StartFlash(Entity? button)
	{
		if (button is not null && m_Flashes.TryGetValue(button.ID, out (Color Normal, Color Flash, float Left) flash))
		{
			m_Flashes[button.ID] = (flash.Normal, flash.Flash, FlashSeconds);
		}
	}

	// A lit button shows its hovered color at rest, fading back to its normal color.
	private void Flash(float deltaTime)
	{
		foreach (Entity button in m_Buttons)
		{
			if (!m_Flashes.TryGetValue(button.ID, out (Color Normal, Color Flash, float Left) flash) || flash.Left <= 0.0f)
			{
				continue;
			}
			float left = Mathf.Max(0.0f, flash.Left - deltaTime);
			m_Flashes[button.ID] = (flash.Normal, flash.Flash, left);
			if (button.GetComponent<UIButtonComponent>() is { } component)
			{
				component.NormalColor = Color.Lerp(flash.Normal, flash.Flash, Mathf.SmoothStep(0.0f, 1.0f, left / FlashSeconds));
			}
		}
	}

	private void TickOnHover()
	{
		// A button held down is still the one under the mouse: releasing it does not tick again.
		Entity? hovered = null;
		foreach (Entity button in m_Buttons)
		{
			if (button.GetComponent<UIButtonComponent>()?.State is UIButtonState.Hovered or UIButtonState.Pressed)
			{
				hovered = button;
			}
		}
		if (hovered is not null && hovered != m_Hovered && HoverSound?.GetComponent<AudioSourceComponent>() is { } tick)
		{
			tick.Stop();
			tick.Play();
		}
		m_Hovered = hovered;
	}
}
