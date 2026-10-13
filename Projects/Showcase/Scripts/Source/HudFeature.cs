using Strada;

namespace Showcase;

/// <summary>A row of the HUD's feature list (a UI element): it lights up while the camera is near the exhibit it names, its glow
/// fading in, its accent bar and name brightening, and it opens to show its detail line. The HUD drives it (<see cref="Hud"/>)
/// and stacks the rows by their heights.</summary>
public sealed class HudFeature : Script
{
	[Tooltip("Where the exhibit stands (world space; its height does not matter)")]
	public Vector3 Center;

	[Tooltip("Meters from the center within which the row is fully lit; it fades out over as many meters again (0: the HUD lights it)")]
	[Range(0.0f, 100.0f)]
	public float Radius = 6.0f;

	[Tooltip("The row's glow: a UI element that fades in while the row is lit")]
	public Entity? Glow;

	[Tooltip("The row's accent bar: a UI element, faint until the row is lit")]
	public Entity? Bar;

	[Tooltip("The row's name (UI text), which brightens while the row is lit")]
	public Entity? Label;

	[Tooltip("The row's detail line (a UI element), shown while the row is lit")]
	public Entity? Detail;

	[Tooltip("The name's color while the row is lit")]
	public Color LitColor = new(1.0f, 0.86f, 0.62f);

	[Tooltip("The row's height with its name alone, in canvas units")]
	[Range(1.0f, 200.0f)]
	public float ClosedHeight = 34.0f;

	[Tooltip("Canvas units the detail line adds to the row while it is lit")]
	[Range(0.0f, 200.0f)]
	public float DetailHeight = 22.0f;

	private Color m_LabelColor = Color.White;

	/// <summary>How lit the row is, from 0 to 1.</summary>
	public float Lit { get; private set; }

	/// <summary>The row's height now (canvas units): it opens as it lights up.</summary>
	public float Height => ClosedHeight + (DetailHeight * Mathf.SmoothStep(0.0f, 1.0f, Lit));

	protected override void OnCreate()
	{
		m_LabelColor = Label?.GetComponent<UITextComponent>()?.Color ?? Color.White;
		Show(0.0f);
	}

	/// <summary>How near a point is to the exhibit along the ground: 1 within <see cref="Radius"/>, falling smoothly to 0 at twice
	/// that; 0 for rows without a radius.</summary>
	public float Nearness(Vector3 point)
	{
		if (Radius <= 0.0f)
		{
			return 0.0f;
		}
		float distance = Vector2.Distance(new Vector2(point.X, point.Z), new Vector2(Center.X, Center.Z));
		return 1.0f - Mathf.SmoothStep(0.0f, 1.0f, (distance - Radius) / Radius);
	}

	/// <summary>Moves the row's light towards the target (0 to 1) over a quarter of a second; returns whether it changed.</summary>
	public bool Animate(float target, float deltaTime)
	{
		float lit = Mathf.MoveTowards(Lit, Mathf.Clamp01(target), deltaTime * 4.0f);
		if (lit == Lit)
		{
			return false;
		}
		Show(lit);
		return true;
	}

	/// <summary>Places the row at a distance from the top of the list (canvas units) at its current height.</summary>
	public void Place(float top)
	{
		if (GetComponent<UIRectComponent>() is { } rect)
		{
			rect.Position = new Vector2(rect.Position.X, top);
			rect.Size = new Vector2(rect.Size.X, Height);
		}
	}

	private void Show(float lit)
	{
		Lit = lit;
		if (Glow?.GetComponent<UIRectComponent>() is { } glow)
		{
			glow.Opacity = lit;
		}
		if (Bar?.GetComponent<UIRectComponent>() is { } bar)
		{
			bar.Opacity = 0.25f + (0.75f * lit);
		}
		if (Label?.GetComponent<UITextComponent>() is { } label)
		{
			label.Color = Color.Lerp(m_LabelColor, LitColor, lit);
		}
		// The detail line shows as the row has opened for it.
		if (Detail?.GetComponent<UIRectComponent>() is { } detail)
		{
			detail.Opacity = Mathf.SmoothStep(0.0f, 1.0f, (lit - 0.4f) / 0.6f);
		}
	}
}
