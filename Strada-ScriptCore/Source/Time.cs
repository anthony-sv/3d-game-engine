namespace Strada;

/// <summary>Timing of the running scene.</summary>
public static class Time
{
	/// <summary>Seconds since the previous frame; the value passed to <see cref="Script.OnUpdate"/>.</summary>
	public static float DeltaTime { get; internal set; }

	/// <summary>Seconds per physics step; the value passed to <see cref="Script.OnFixedUpdate"/>.</summary>
	public static float FixedDeltaTime { get; internal set; }
}
