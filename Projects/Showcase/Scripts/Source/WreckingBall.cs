using Strada;

namespace Showcase;

/// <summary>A heavy ball hanging from a beam by a joint: its key (or the tour) pushes it, and it swings into whatever stands in
/// its way, clanging and shaking the camera as it hits.</summary>
public sealed class WreckingBall : Impact, ITourCue
{
	[Tooltip("The velocity a push gives the ball, in meters per second")]
	public Vector3 Push = new(7.0f, 0.0f, 0.0f);

	[Tooltip("The key that pushes the ball")]
	public KeyCode Key = KeyCode.D1;

	private Vector3 m_Rest;
	private Quaternion m_RestRotation;

	/// <summary>How many times the ball was pushed.</summary>
	public int Swings { get; private set; }

	protected override void OnCreate()
	{
		m_Rest = Transform.WorldTranslation;
		m_RestRotation = Rotation;
	}

	protected override void OnUpdate(float deltaTime)
	{
		base.OnUpdate(deltaTime);
		if (Input.IsKeyPressed(Key))
		{
			Swing();
		}
	}

	/// <summary>Sends the ball swinging.</summary>
	public void Swing()
	{
		if (GetComponent<RigidBodyComponent>() is { } body)
		{
			body.LinearVelocity = Push;
			Swings++;
		}
	}

	/// <inheritdoc/>
	public void Perform() => Swing();

	/// <summary>Stops the ball where it hung at the start.</summary>
	public void Stop()
	{
		if (GetComponent<RigidBodyComponent>() is { } body)
		{
			body.Teleport(m_Rest, m_RestRotation);
			body.LinearVelocity = Vector3.Zero;
			body.AngularVelocity = Vector3.Zero;
		}
	}
}
