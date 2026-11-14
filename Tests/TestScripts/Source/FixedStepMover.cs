namespace Strada.Tests;

// Notes where its entity is as each fixed update reads it (FixedRead, FixedUpdates). With Velocity set on a kinematic body,
// it moves the body that far per second in its fixed updates (from where the last one put it), as a lift or a platform
// moved by its steps does.
public sealed class FixedStepMover : Script
{
	public Vector3 Velocity;
	public Vector3 FixedRead;
	public int FixedUpdates;

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		FixedUpdates++;
		FixedRead = Translation;
		if (Velocity != Vector3.Zero && GetComponent<RigidBodyComponent>() is { Type: RigidBodyType.Kinematic } body)
		{
			body.MoveKinematic(Translation + (Velocity * fixedDeltaTime), Rotation);
		}
	}
}
