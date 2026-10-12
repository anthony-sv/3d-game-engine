using Strada.Interop;

namespace Strada;

/// <summary>Places a UI element in its parent's rectangle (the screen for a canvas's children), in canvas units with x to the
/// right and y downwards. On each axis the anchors mark a span of the parent (0 is its left or top edge, 1 its right or
/// bottom edge): the rectangle is <see cref="Size"/> larger than the span, so equal anchors give it exactly that size, and
/// its <see cref="Pivot"/> sits at the pivot's point of the span, moved by <see cref="Position"/>.</summary>
[NativeComponent("UIRect")]
public sealed unsafe class UIRectComponent : Component
{
	private UIRectComponent()
	{
	}

	/// <summary>Where the anchors' span starts in the parent's rectangle, from (0, 0) at its top-left corner to (1, 1) at its
	/// bottom-right corner.</summary>
	public Vector2 AnchorMin
	{
		get => NativeField.Get<Vector2>(InternalCalls.UIRectComponent_GetAnchorMin, Entity.ID);
		set => NativeField.Set<Vector2>(InternalCalls.UIRectComponent_SetAnchorMin, Entity.ID, value);
	}

	/// <summary>Where the anchors' span ends; equal anchors attach the rectangle to a point, different ones stretch it with
	/// the parent.</summary>
	public Vector2 AnchorMax
	{
		get => NativeField.Get<Vector2>(InternalCalls.UIRectComponent_GetAnchorMax, Entity.ID);
		set => NativeField.Set<Vector2>(InternalCalls.UIRectComponent_SetAnchorMax, Entity.ID, value);
	}

	/// <summary>How far the pivot is moved from its point of the anchors' span, in canvas units (y downwards).</summary>
	public Vector2 Position
	{
		get => NativeField.Get<Vector2>(InternalCalls.UIRectComponent_GetPosition, Entity.ID);
		set => NativeField.Set<Vector2>(InternalCalls.UIRectComponent_SetPosition, Entity.ID, value);
	}

	/// <summary>Canvas units added to the anchors' span: the size itself when the anchors are equal.</summary>
	public Vector2 Size
	{
		get => NativeField.Get<Vector2>(InternalCalls.UIRectComponent_GetSize, Entity.ID);
		set => NativeField.Set<Vector2>(InternalCalls.UIRectComponent_SetSize, Entity.ID, value);
	}

	/// <summary>The point of the rectangle that <see cref="Position"/> places, from (0, 0) at its top-left corner to (1, 1) at
	/// its bottom-right corner.</summary>
	public Vector2 Pivot
	{
		get => NativeField.Get<Vector2>(InternalCalls.UIRectComponent_GetPivot, Entity.ID);
		set => NativeField.Set<Vector2>(InternalCalls.UIRectComponent_SetPivot, Entity.ID, value);
	}

	/// <summary>Whether the element is shown; hidden elements and their descendants are neither drawn nor clicked.</summary>
	public bool Visible
	{
		get => NativeField.GetBool(InternalCalls.UIRectComponent_GetVisible, Entity.ID);
		set => NativeField.SetBool(InternalCalls.UIRectComponent_SetVisible, Entity.ID, value);
	}

	/// <summary>Multiplies the alpha of the element and its descendants, from 0 (invisible, and not clicked) to 1.</summary>
	public float Opacity
	{
		get => NativeField.Get<float>(InternalCalls.UIRectComponent_GetOpacity, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.UIRectComponent_SetOpacity, Entity.ID, value);
	}
}
