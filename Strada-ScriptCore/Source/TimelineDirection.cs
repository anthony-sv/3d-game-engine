namespace Strada;

/// <summary>The direction a visual script's Timeline node plays in: its Direction output.</summary>
public enum TimelineDirection
{
	/// <summary>Towards the end: Play and PlayFromStart play forward.</summary>
	Forward = 0,
	/// <summary>Towards the start: Reverse and ReverseFromEnd play backward.</summary>
	Backward = 1,
}
