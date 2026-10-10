namespace Strada.Tests;

// Moves its entity along Direction at Speed units per second.
public sealed class Mover : Script
{
	public float Speed = 1.0f;
	public Vector3 Direction = new(1.0f, 0.0f, 0.0f);

	protected override void OnUpdate(float deltaTime)
	{
		Translation += Direction * (Speed * deltaTime);
	}
}
