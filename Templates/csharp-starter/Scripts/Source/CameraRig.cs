using Strada;

namespace Game;

/// <summary>The camera rig: it drifts after the orb, so the camera leans towards where the orb rolls.</summary>
public sealed class CameraRig : Script
{
	[Tooltip("The orb the camera follows")]
	public Entity? Orb;

	[Tooltip("Where the rig stands while the orb is in the middle")]
	public Vector3 Home = new(0.0f, 17.5f, 15.0f);

	[Tooltip("The share of the orb's way from the middle the camera follows")]
	[Range(0.0f, 1.0f)]
	public float Follow = 0.3f;

	[Tooltip("How quickly the camera catches up, per second")]
	[Range(0.1f, 20.0f)]
	public float Ease = 2.5f;

	protected override void OnUpdate(float deltaTime)
	{
		if (Orb is null)
		{
			return;
		}
		Vector3 orb = Orb.Transform.WorldTranslation;
		Vector3 target = Home + (new Vector3(orb.X, 0.0f, orb.Z) * Follow);
		// Easing by the time that passed keeps the drift the same at any frame rate.
		Translation = Vector3.Lerp(Translation, target, 1.0f - Mathf.Exp(-Ease * deltaTime));
	}
}
