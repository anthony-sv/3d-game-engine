using Strada.Interop;

namespace Strada;

/// <summary>The root of a screen-space UI (HUDs, menus). The canvas covers the screen; its descendants are the UI, laid out by
/// their <see cref="UIRectComponent"/> and drawn over the scene in hierarchy order (children over their parents, later
/// siblings over earlier ones).</summary>
[NativeComponent("UICanvas")]
public sealed unsafe class UICanvasComponent : Component
{
	private UICanvasComponent()
	{
	}

	/// <summary>How canvas units relate to the screen's pixels.</summary>
	public UIScaleMode ScaleMode
	{
		get => (UIScaleMode)NativeField.Get<int>(InternalCalls.UICanvasComponent_GetScaleMode, Entity.ID);
		set => NativeField.Set<int>(InternalCalls.UICanvasComponent_SetScaleMode, Entity.ID, (int)value);
	}

	/// <summary>The screen size the UI is designed for, in canvas units, which the <see cref="ScaleMode"/> fits to the
	/// screen.</summary>
	public Vector2 ReferenceResolution
	{
		get => NativeField.Get<Vector2>(InternalCalls.UICanvasComponent_GetReferenceResolution, Entity.ID);
		set => NativeField.Set<Vector2>(InternalCalls.UICanvasComponent_SetReferenceResolution, Entity.ID, value);
	}
}
