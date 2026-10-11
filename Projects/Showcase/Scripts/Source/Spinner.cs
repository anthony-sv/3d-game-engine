using System.Diagnostics.CodeAnalysis;
using Strada;

namespace Showcase;

/// <summary>Turns the entity steadily about an axis: lighthouse lamps, mirror balls, sculptures. A kinematic body is turned by
/// the physics, so it pushes what it touches.</summary>
public sealed class Spinner : Script
{
	[Tooltip("The axis to turn about, in the parent's space")]
	public Vector3 Axis = Vector3.Up;

	[Tooltip("Degrees per second (negative turns the other way)")]
	[Range(-720.0f, 720.0f)]
	public float Speed = 30.0f;

	private Quaternion m_Start;
	private float m_Angle;

	/// <summary>Degrees turned since the start (0 to 360).</summary>
	public float Angle => m_Angle;

	protected override void OnCreate()
	{
		m_Start = Rotation;
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (!IsKinematic(out _))
		{
			Turn(deltaTime);
			Rotation = Current;
		}
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		if (IsKinematic(out RigidBodyComponent? body))
		{
			Turn(fixedDeltaTime);
			// Spinning bodies sit at the root (or under unturned parents): their rotation is their world rotation.
			body.MoveKinematic(Transform.WorldTranslation, Current);
		}
	}

	private Quaternion Current => Quaternion.AngleAxis(m_Angle, Axis) * m_Start;

	private void Turn(float seconds)
	{
		m_Angle = Mathf.Repeat(m_Angle + (Speed * seconds), 360.0f);
	}

	private bool IsKinematic([NotNullWhen(true)] out RigidBodyComponent? body)
	{
		body = GetComponent<RigidBodyComponent>();
		return body is { Type: RigidBodyType.Kinematic };
	}
}
