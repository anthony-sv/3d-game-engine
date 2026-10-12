using Strada.Interop;

namespace Strada;

/// <summary>Makes a UI element a button: while the scene runs the mouse hovers, presses and clicks it (the left button pressed
/// and released over it). A click sets <see cref="WasClicked"/> for the frame and calls <c>OnClick</c> on the scripts of the
/// button and of its ancestors. The button's colors replace its <see cref="UIImageComponent"/>'s color in each state.</summary>
[NativeComponent("UIButton")]
public sealed unsafe class UIButtonComponent : Component
{
	private UIButtonComponent()
	{
	}

	/// <summary>Whether the button can be clicked; a disabled button shows <see cref="DisabledColor"/> and still covers the UI
	/// below it.</summary>
	public bool Interactable
	{
		get => NativeField.GetBool(InternalCalls.UIButtonComponent_GetInteractable, Entity.ID);
		set => NativeField.SetBool(InternalCalls.UIButtonComponent_SetInteractable, Entity.ID, value);
	}

	/// <summary>The image's color at rest.</summary>
	public Color NormalColor
	{
		get => NativeField.Get<Color>(InternalCalls.UIButtonComponent_GetNormalColor, Entity.ID);
		set => NativeField.Set<Color>(InternalCalls.UIButtonComponent_SetNormalColor, Entity.ID, value);
	}

	/// <summary>The image's color under the mouse.</summary>
	public Color HoveredColor
	{
		get => NativeField.Get<Color>(InternalCalls.UIButtonComponent_GetHoveredColor, Entity.ID);
		set => NativeField.Set<Color>(InternalCalls.UIButtonComponent_SetHoveredColor, Entity.ID, value);
	}

	/// <summary>The image's color while the mouse holds the button down.</summary>
	public Color PressedColor
	{
		get => NativeField.Get<Color>(InternalCalls.UIButtonComponent_GetPressedColor, Entity.ID);
		set => NativeField.Set<Color>(InternalCalls.UIButtonComponent_SetPressedColor, Entity.ID, value);
	}

	/// <summary>The image's color while the button is not <see cref="Interactable"/>.</summary>
	public Color DisabledColor
	{
		get => NativeField.Get<Color>(InternalCalls.UIButtonComponent_GetDisabledColor, Entity.ID);
		set => NativeField.Set<Color>(InternalCalls.UIButtonComponent_SetDisabledColor, Entity.ID, value);
	}

	/// <summary>Whether the button was clicked this frame (true from the frame's first callback to the next frame's).</summary>
	public bool WasClicked => InternalCalls.UIButtonComponent_WasClicked(Entity.ID) != 0;

	/// <summary>The state the button is drawn in.</summary>
	public UIButtonState State => (UIButtonState)InternalCalls.UIButtonComponent_GetState(Entity.ID);

	/// <summary>Clicks the button as the mouse would (for keyboard and gamepad menus): at the start of the next frame,
	/// <see cref="WasClicked"/> is set and the scripts get <c>OnClick</c>, unless the button is not interactable then.</summary>
	public void Click() => InternalCalls.UIButtonComponent_Click(Entity.ID);
}
