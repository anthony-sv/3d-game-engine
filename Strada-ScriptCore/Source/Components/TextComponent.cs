using Strada.Interop;

namespace Strada;

/// <summary>Renders text in the world (in the entity's XY plane) or on the screen.</summary>
[NativeComponent("Text")]
public sealed unsafe class TextComponent : Component
{
	private TextComponent()
	{
	}

	/// <summary>The text; newlines start new lines.</summary>
	public string Text
	{
		get => NativeField.GetString(InternalCalls.TextComponent_GetText, Entity.ID);
		set => NativeField.SetString(InternalCalls.TextComponent_SetText, Entity.ID, value);
	}

	/// <summary>The font, or null for the default font.</summary>
	public Font? Font
	{
		get
		{
			AssetHandle handle = NativeField.GetAsset(InternalCalls.TextComponent_GetFont, Entity.ID);
			return handle.IsValid ? new Font(handle) : null;
		}
		set => NativeField.SetAsset(InternalCalls.TextComponent_SetFont, Entity.ID, value);
	}

	/// <summary>The text's color.</summary>
	public Color Color
	{
		get => NativeField.Get<Color>(InternalCalls.TextComponent_GetColor, Entity.ID);
		set => NativeField.Set<Color>(InternalCalls.TextComponent_SetColor, Entity.ID, value);
	}

	/// <summary>World units per line, or pixels for screen-space text.</summary>
	public float FontSize
	{
		get => NativeField.Get<float>(InternalCalls.TextComponent_GetFontSize, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.TextComponent_SetFontSize, Entity.ID, value);
	}

	/// <summary>Whether the text is placed on the screen (translation in normalized viewport coordinates).</summary>
	public bool ScreenSpace
	{
		get => NativeField.GetBool(InternalCalls.TextComponent_GetScreenSpace, Entity.ID);
		set => NativeField.SetBool(InternalCalls.TextComponent_SetScreenSpace, Entity.ID, value);
	}

	/// <summary>How lines align to the entity's position.</summary>
	public TextAlignment Alignment
	{
		get => (TextAlignment)NativeField.Get<int>(InternalCalls.TextComponent_GetAlignment, Entity.ID);
		set => NativeField.Set<int>(InternalCalls.TextComponent_SetAlignment, Entity.ID, (int)value);
	}

	/// <summary>Distance between lines as a multiple of the font's line height.</summary>
	public float LineSpacing
	{
		get => NativeField.Get<float>(InternalCalls.TextComponent_GetLineSpacing, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.TextComponent_SetLineSpacing, Entity.ID, value);
	}
}
