using Strada;

namespace Showcase;

/// <summary>Sways the entity back and forth about an axis, unevenly like cloth in a breeze (banners).</summary>
public sealed class Sway : Script
{
	[Tooltip("The axis to sway about, in the parent's space")]
	public Vector3 Axis = Vector3.Right;

	[Tooltip("Degrees of the largest swing")]
	[Range(0.0f, 90.0f)]
	public float Amplitude = 6.0f;

	[Tooltip("Swings per second")]
	[Range(0.01f, 5.0f)]
	public float Frequency = 0.3f;

	private Quaternion m_Start;
	private float m_Phase;
	private float m_Time;

	protected override void OnCreate()
	{
		m_Start = Rotation;
		m_Phase = Random.Range(0.0f, 2.0f * Mathf.PI);
	}

	protected override void OnUpdate(float deltaTime)
	{
		m_Time += deltaTime;
		float wave = 2.0f * Mathf.PI * Frequency * m_Time;
		float angle = Amplitude * ((0.7f * Mathf.Sin(wave + m_Phase)) + (0.3f * Mathf.Sin((wave * 2.3f) + (m_Phase * 1.7f))));
		Rotation = Quaternion.AngleAxis(angle, Axis) * m_Start;
	}
}
