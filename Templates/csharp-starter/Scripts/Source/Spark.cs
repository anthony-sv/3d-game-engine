using System;
using Strada;

namespace Game;

/// <summary>A spark: the rolling orb collects it, and it flashes and goes. Its gem floats through a visual script,
/// Scripts/Hover.sgraph, which asks <see cref="Hover"/> where to put it: a C# member that graphs call as a node.</summary>
public sealed class Spark : Script
{
	[Tooltip("The flash of light a collected spark leaves")]
	public Prefab? Burst;

	[Tooltip("Metres the gem rises and falls")]
	[Range(0.0f, 1.0f)]
	public float Bob = 0.18f;

	// Bobs a second, in radians.
	private const float BobRate = 2.4f;

	// Sparks at different spots float at different points of their bob.
	private float m_Phase;
	private bool m_Taken;

	/// <summary>Raised once, when the orb collects the spark during a run.</summary>
	public event Action<Spark>? Collected;

	/// <summary>Where the gem floats above its spark at a time: it bobs up and down by <see cref="Bob"/> metres.</summary>
	[VisualNode(Pure = true, Category = "Game/Orb Run", Description = "Where the gem floats above its spark at a time, in seconds")]
	public Vector3 Hover(float time) => new(0.0f, Mathf.Sin((time + m_Phase) * BobRate) * Bob, 0.0f);

	protected override void OnCreate()
	{
		Vector3 spot = Transform.WorldTranslation;
		m_Phase = (spot.X * 0.7f) + (spot.Z * 0.4f);
	}

	protected override void OnTriggerEnter(Entity other)
	{
		// Only the orb collects, and only during a run.
		if (m_Taken || other.As<Orb>() is not { Rolling: true })
		{
			return;
		}
		m_Taken = true;
		if (Burst is not null)
		{
			Entity.Instantiate(Burst, Transform.WorldTranslation, Quaternion.Identity);
		}
		Collected?.Invoke(this);
		Destroy();
	}
}
