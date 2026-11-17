using Strada.Interop;

namespace Strada;

/// <summary>An input actions asset (<c>.sinput</c>): the named actions a game reads instead of keys and gamepads (Throttle,
/// Steer, Jump), each with its bindings. The project names the one its scenes evaluate (Project Settings, Input Actions);
/// scripts read its actions by name through <see cref="Input.GetAction"/>, <see cref="Input.IsActionPressed"/> and the other
/// action queries, and <see cref="Script.OnActionPressed"/> and <see cref="Script.OnActionReleased"/> report their edges.
/// Edit it in the editor; an edit made while the game plays shows on the next frame. An asset that cannot be loaded has no
/// actions (logged).</summary>
public sealed class InputActions : Asset
{
	// The runtime's copy of the asset's names, shared by every reference to the asset; found on first use.
	private InputActionsCache.Entry? m_Entry;

	internal InputActions(AssetHandle handle)
		: base(handle)
	{
	}

	/// <summary>The names of the asset's actions, in its order (the names <see cref="Input.GetAction"/> and the other action
	/// queries take, with their letter case): what a settings screen lists. A new array on each read.</summary>
	public string[] ActionNames => [.. (m_Entry ??= InputActionsCache.Find(Handle)).GetNames()];
}
