using Strada.Interop;
using Strada.VisualScripting;

namespace Strada;

/// <summary>A curve asset (<c>.scurve</c>): keys with values, how each segment interpolates (smooth, linear or a step) and
/// their slopes, and what the curve does before its first key and after its last (hold, loop, ping-pong or go on along a
/// line). Use one to map a number to another, such as a speed to an engine's pitch or a time to a light's intensity. Edit it
/// in the editor's curve editor; scripts read it.</summary>
public sealed class Curve : Asset
{
	// The runtime's copy of the asset's data, shared by every reference to the asset; found on first use.
	private CurveCache.Entry? m_Entry;

	internal Curve(AssetHandle handle)
		: base(handle)
	{
	}

	/// <summary>The curve's value at <paramref name="time"/> (the curve's input: a time, a speed or a distance). A curve
	/// without keys is 0 everywhere; a curve that cannot be loaded is 0 too (logged). An edit of the asset while the game plays
	/// shows on the next frame.</summary>
	/// <param name="time">Where on the curve to read it.</param>
	[PureNode]
	public float Evaluate(float time)
	{
		m_Entry ??= CurveCache.Find(Handle);
		return m_Entry.GetDefinition().Evaluate(time);
	}
}
