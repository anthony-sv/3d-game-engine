using Strada;

namespace Showcase;

/// <summary>A stop of the tour: the entity's position and view direction are where the camera passes, and the fields say
/// what the screen shows meanwhile, how long the camera takes to the next stop, and which exhibit it sets off on the way.</summary>
public sealed class CameraShot : Script
{
	[Tooltip("The caption shown while the camera is around this shot (empty: none)")]
	public string Caption = "";

	[Tooltip("A smaller line under the caption")]
	public string Detail = "";

	[Tooltip("Seconds the camera takes from this shot to the next")]
	[Range(0.5f, 30.0f)]
	public float Seconds = 4.0f;

	[Tooltip("An exhibit the camera sets off on its way to this shot (its script is an ITourCue)")]
	public Entity? Cue;

	[Tooltip("Seconds before the camera reaches this shot that it sets the exhibit off, so the action peaks as it arrives")]
	[Range(0.0f, 10.0f)]
	public float CueLead;
}
