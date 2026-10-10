using Strada.Interop;

namespace Strada;

/// <summary>Timing of the running scene. All times are scaled by <see cref="TimeScale"/>.</summary>
public static unsafe class Time
{
	/// <summary>Seconds since the previous frame; the value passed to <see cref="Script.OnUpdate"/>.</summary>
	public static float DeltaTime { get; internal set; }

	/// <summary>Seconds per physics step; the value passed to <see cref="Script.OnFixedUpdate"/>.</summary>
	public static float FixedDeltaTime { get; internal set; }

	/// <summary>Seconds since the scene started running.</summary>
	public static double Elapsed => InternalCalls.Time_GetElapsed();

	/// <summary>Frames since the scene started running.</summary>
	public static ulong FrameCount => InternalCalls.Time_GetFrameCount();

	/// <summary>How fast time passes for scripts and physics (1 is real time, 0 freezes the simulation; sounds are not
	/// affected).</summary>
	public static float TimeScale
	{
		get => InternalCalls.Time_GetTimeScale();
		set => InternalCalls.Time_SetTimeScale(value);
	}
}
