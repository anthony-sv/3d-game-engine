using Strada;

namespace Showcase;

/// <summary>A heavy ball hanging from a beam by a joint: B pushes it, and it swings into whatever stands in its way.</summary>
public sealed class WreckingBall : Script
{
	[Tooltip("The velocity a push gives the ball, in meters per second")]
	public Vector3 Push = new(7.0f, 0.0f, 0.0f);

	/// <summary>How many times the ball was pushed.</summary>
	public int Swings { get; private set; }

	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsKeyPressed(KeyCode.B))
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
}
