using Strada.Interop;

namespace Strada;

/// <summary>Drawing for debugging, in the editor and development builds (shipped Dist games draw nothing).</summary>
public static unsafe class Debug
{
	/// <summary>Draws a line in world space for <paramref name="duration"/> seconds of game time (0 draws it in the next
	/// frame only), hidden behind surfaces.</summary>
	public static void DrawLine(Vector3 from, Vector3 to, Color color, float duration = 0.0f)
	{
		InternalCalls.Debug_DrawLine(&from, &to, &color, duration);
	}
}
