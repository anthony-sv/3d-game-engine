using Strada.Interop;

namespace Strada;

/// <summary>Fills a UI element's rectangle with a color or a texture, optionally with rounded corners. Images take the clicks
/// over the UI they cover.</summary>
[NativeComponent("UIImage")]
public sealed unsafe class UIImageComponent : Component
{
	private UIImageComponent()
	{
	}

	/// <summary>The image's color (multiplies the texture). A <see cref="UIButtonComponent"/> on the entity replaces it with the
	/// color of its state.</summary>
	public Color Color
	{
		get => NativeField.Get<Color>(InternalCalls.UIImageComponent_GetColor, Entity.ID);
		set => NativeField.Set<Color>(InternalCalls.UIImageComponent_SetColor, Entity.ID, value);
	}

	/// <summary>The texture, or null to fill the rectangle with the color.</summary>
	public Texture? Texture
	{
		get
		{
			AssetHandle handle = NativeField.GetAsset(InternalCalls.UIImageComponent_GetTexture, Entity.ID);
			return handle.IsValid ? new Texture(handle) : null;
		}
		set => NativeField.SetAsset(InternalCalls.UIImageComponent_SetTexture, Entity.ID, value);
	}

	/// <summary>Whether the texture keeps its aspect ratio: as large as fits, centered in the rectangle.</summary>
	public bool PreserveAspect
	{
		get => NativeField.GetBool(InternalCalls.UIImageComponent_GetPreserveAspect, Entity.ID);
		set => NativeField.SetBool(InternalCalls.UIImageComponent_SetPreserveAspect, Entity.ID, value);
	}

	/// <summary>Radius of the rounded corners in canvas units (at most half the rectangle's shorter side).</summary>
	public float CornerRadius
	{
		get => NativeField.Get<float>(InternalCalls.UIImageComponent_GetCornerRadius, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.UIImageComponent_SetCornerRadius, Entity.ID, value);
	}
}
