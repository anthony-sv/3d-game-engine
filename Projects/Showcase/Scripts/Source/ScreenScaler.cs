using System.Collections.Generic;
using Strada;

namespace Showcase;

/// <summary>Keeps the screen-space texts and sprites under the entity the same size relative to the window: their sizes are
/// authored for a window ReferenceHeight pixels tall and scale with the window's height (screen-space sizes are in pixels).</summary>
public sealed class ScreenScaler : Script
{
	[Tooltip("The window height, in pixels, that the sizes are authored for")]
	[Range(100.0f, 4320.0f)]
	public float ReferenceHeight = 900.0f;

	private readonly List<(TextComponent Text, float Size)> m_Texts = [];
	private readonly List<(Entity Sprite, Vector3 Scale)> m_Sprites = [];
	private int m_Height;

	/// <summary>The factor the sizes are scaled by now.</summary>
	public float Factor { get; private set; } = 1.0f;

	protected override void OnCreate()
	{
		Collect(this);
		Apply();
	}

	protected override void OnUpdate(float deltaTime)
	{
		Apply();
	}

	private void Collect(Entity entity)
	{
		foreach (Entity child in entity.Children)
		{
			if (child.GetComponent<TextComponent>() is { ScreenSpace: true } text)
			{
				m_Texts.Add((text, text.FontSize));
			}
			if (child.GetComponent<SpriteRendererComponent>() is { ScreenSpace: true })
			{
				m_Sprites.Add((child, child.Scale));
			}
			Collect(child);
		}
	}

	private void Apply()
	{
		int height = Application.WindowHeight;
		if (height <= 0 || height == m_Height)
		{
			return;
		}
		m_Height = height;
		Factor = height / ReferenceHeight;
		foreach ((TextComponent text, float size) in m_Texts)
		{
			text.FontSize = size * Factor;
		}
		foreach ((Entity sprite, Vector3 scale) in m_Sprites)
		{
			sprite.Scale = new Vector3(scale.X * Factor, scale.Y * Factor, scale.Z);
		}
	}
}
