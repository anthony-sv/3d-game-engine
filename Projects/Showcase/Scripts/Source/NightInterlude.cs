using Strada;

namespace Showcase;

/// <summary>A night for a while: when cued (the tour cues it on its way to the campfire), night falls (DayNight), and
/// <see cref="Seconds"/> later, or as soon as the tour ends or is skipped, the lighting turns back to what it was before. If
/// day and night were switched meanwhile (the L key or the HUD), that choice stands: the interlude ends without turning the
/// lighting back.</summary>
public sealed class NightInterlude : Script, ITourCue
{
	[Tooltip("The day and night switch (a DayNight)")]
	public Entity? Lighting;

	[Tooltip("Seconds from the cue until the lighting turns back")]
	[Range(0.0f, 120.0f)]
	public float Seconds = 9.0f;

	private float m_Left = -1.0f;
	private bool m_NightBefore;
	private int m_Switches;

	/// <summary>Whether the interlude is on (until the lighting starts turning back).</summary>
	public bool IsActive => m_Left >= 0.0f;

	/// <inheritdoc/>
	public void Perform()
	{
		if (Lighting?.As<DayNight>() is not { } lighting)
		{
			return;
		}
		// A cue during the interlude prolongs it and keeps what the lighting was before the first.
		if (!IsActive)
		{
			m_NightBefore = lighting.IsNight;
		}
		lighting.SetNight(true);
		m_Switches = lighting.Switches;
		m_Left = Seconds;
	}

	/// <inheritdoc/>
	public void TourEnded() => End();

	/// <summary>Ends the interlude now: the lighting starts turning back to what it was before the cue, unless day and night were
	/// switched since the interlude set the night.</summary>
	public void End()
	{
		if (!IsActive)
		{
			return;
		}
		m_Left = -1.0f;
		if (Lighting?.As<DayNight>() is { } lighting && lighting.Switches == m_Switches)
		{
			lighting.SetNight(m_NightBefore);
		}
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (!IsActive)
		{
			return;
		}
		if (m_Left > deltaTime)
		{
			m_Left -= deltaTime;
			return;
		}
		End();
	}
}
