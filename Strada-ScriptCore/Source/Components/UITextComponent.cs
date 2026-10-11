using Strada.Interop;

namespace Strada;

/// <summary>Draws text in a UI element's rectangle: lines wrap at its width and the text block is aligned in it.</summary>
[NativeComponent("UIText")]
public sealed unsafe class UITextComponent : Component
{
	private UITextComponent()
	{
	}

	/// <summary>The text; newlines start new lines.</summary>
	public string Text
	{
		get => NativeField.GetString(InternalCalls.UITextComponent_GetText, Entity.ID);
		set => NativeField.SetString(InternalCalls.UITextComponent_SetText, Entity.ID, value);
	}

	/// <summary>The font, or null for the default font.</summary>
	public Font? Font
	{
		get
		{
			AssetHandle handle = NativeField.GetAsset(InternalCalls.UITextComponent_GetFont, Entity.ID);
			return handle.IsValid ? new Font(handle) : null;
		}
		set => NativeField.SetAsset(InternalCalls.UITextComponent_SetFont, Entity.ID, value);
	}

	/// <summary>The text's color.</summary>
	public Color Color
	{
		get => NativeField.Get<Color>(InternalCalls.UITextComponent_GetColor, Entity.ID);
		set => NativeField.Set<Color>(InternalCalls.UITextComponent_SetColor, Entity.ID, value);
	}

	/// <summary>Line height in canvas units.</summary>
	public float FontSize
	{
		get => NativeField.Get<float>(InternalCalls.UITextComponent_GetFontSize, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.UITextComponent_SetFontSize, Entity.ID, value);
	}

	/// <summary>Whether the lines start at the rectangle's left edge, are centered in it or end at its right edge.</summary>
	public TextAlignment Alignment
	{
		get => (TextAlignment)NativeField.Get<int>(InternalCalls.UITextComponent_GetAlignment, Entity.ID);
		set => NativeField.Set<int>(InternalCalls.UITextComponent_SetAlignment, Entity.ID, (int)value);
	}

	/// <summary>Whether the text block sits at the top, middle or bottom of the rectangle.</summary>
	public TextVerticalAlignment VerticalAlignment
	{
		get => (TextVerticalAlignment)NativeField.Get<int>(InternalCalls.UITextComponent_GetVerticalAlignment, Entity.ID);
		set => NativeField.Set<int>(InternalCalls.UITextComponent_SetVerticalAlignment, Entity.ID, (int)value);
	}

	/// <summary>Whether lines that do not fit the rectangle's width break at spaces.</summary>
	public bool Wrap
	{
		get => NativeField.GetBool(InternalCalls.UITextComponent_GetWrap, Entity.ID);
		set => NativeField.SetBool(InternalCalls.UITextComponent_SetWrap, Entity.ID, value);
	}

	/// <summary>Distance between lines as a multiple of the font's line height.</summary>
	public float LineSpacing
	{
		get => NativeField.Get<float>(InternalCalls.UITextComponent_GetLineSpacing, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.UITextComponent_SetLineSpacing, Entity.ID, value);
	}
}
