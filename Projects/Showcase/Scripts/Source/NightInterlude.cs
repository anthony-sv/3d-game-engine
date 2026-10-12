using Strada;

namespace Showcase;

/// <summary>A night for a while: when cued (the tour cues it on its way to the campfire), night falls (DayNight), and
/// <see cref="Seconds"/> later the lighting turns back to what it was before, whether or not the tour is still playing.</summary>
public sealed class NightInterlude : Script, ITourCue
{
	[Tooltip("The day and night switch (a DayNight)")]
	public Entity? Lighting;

	[Tooltip("Seconds from the cue until the lighting turns back")]
	[Range(0.0f, 120.0f)]
	public float Seconds = 9.0f;

	private float m_Left = -1.0f;
	private bool m_NightBefore;

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
		m_Left = Seconds;
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (m_Left < 0.0f)
		{
			return;
		}
		m_Left -= deltaTime;
		if (m_Left < 0.0f)
		{
			Lighting?.As<DayNight>()?.SetNight(m_NightBefore);
		}
	}
}
