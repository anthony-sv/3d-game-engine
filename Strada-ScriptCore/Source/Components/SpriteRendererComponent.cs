using Strada.Interop;

namespace Strada;

/// <summary>Renders an unlit textured or colored quad, in the world or on the screen.</summary>
[NativeComponent("SpriteRenderer")]
public sealed unsafe class SpriteRendererComponent : Component
{
	private SpriteRendererComponent()
	{
	}

	/// <summary>The quad's color (multiplies the texture).</summary>
	public Color Color
	{
		get => NativeField.Get<Color>(InternalCalls.SpriteRendererComponent_GetColor, Entity.ID);
		set => NativeField.Set<Color>(InternalCalls.SpriteRendererComponent_SetColor, Entity.ID, value);
	}

	/// <summary>The texture, or null for a plain color.</summary>
	public Texture? Texture
	{
		get
		{
			AssetHandle handle = NativeField.GetAsset(InternalCalls.SpriteRendererComponent_GetTexture, Entity.ID);
			return handle.IsValid ? new Texture(handle) : null;
		}
		set => NativeField.SetAsset(InternalCalls.SpriteRendererComponent_SetTexture, Entity.ID, value);
	}

	/// <summary>How many times the texture repeats across the quad.</summary>
	public float Tiling
	{
		get => NativeField.Get<float>(InternalCalls.SpriteRendererComponent_GetTiling, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.SpriteRendererComponent_SetTiling, Entity.ID, value);
	}

	/// <summary>Whether the quad is placed on the screen (translation in normalized viewport coordinates).</summary>
	public bool ScreenSpace
	{
		get => NativeField.GetBool(InternalCalls.SpriteRendererComponent_GetScreenSpace, Entity.ID);
		set => NativeField.SetBool(InternalCalls.SpriteRendererComponent_SetScreenSpace, Entity.ID, value);
	}
}
