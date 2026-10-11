using System.Collections.Generic;
using Strada;

namespace Blocks;

/// <summary>The screen of the game: the score column (score, level, lines, best), the controls, and the title, pause and
/// game-over overlays over a dimmed view. Every text is a screen-space entity it creates; sizes are given for an 800 pixel
/// high window and follow the window's height.</summary>
public sealed class Hud
{
	private const float ReferenceHeight = 800.0f;

	private static readonly Color s_Label = new(0.62f, 0.68f, 0.82f);
	private static readonly Color s_Value = new(0.97f, 0.97f, 1.0f);
	private static readonly Color s_Best = new(1.0f, 0.86f, 0.45f);
	private static readonly Color s_Prompt = new(1.0f, 1.0f, 1.0f);
	private static readonly Color[] s_TitleColors =
	[
		new(0.25f, 0.9f, 1.0f), new(1.0f, 0.85f, 0.2f), new(0.8f, 0.4f, 1.0f), new(0.35f, 1.0f, 0.45f), new(1.0f, 0.35f, 0.4f),
		new(1.0f, 0.6f, 0.2f),
	];

	// A text placed in normalized screen coordinates with a size in reference pixels.
	private sealed record Item(TextComponent Text, float Size);

	private readonly Entity m_Root;
	private readonly List<Item> m_Items = [];
	private readonly TextComponent m_Score;
	private readonly TextComponent m_Level;
	private readonly TextComponent m_Lines;
	private readonly TextComponent m_Best;
	private readonly List<TextComponent> m_Title = [];
	private readonly List<float> m_TitleOffsets = [];
	private readonly TextComponent m_Subtitle;
	private readonly TextComponent m_Headline;
	private readonly TextComponent m_Detail;
	private readonly TextComponent m_Prompt;
	private readonly SpriteRendererComponent m_Dim;
	private readonly Entity m_DimEntity;
	private readonly Entity m_Band;
	private int m_LaidOutWidth;
	private int m_LaidOutHeight;

	public Hud(Entity parent)
	{
		m_Root = Entity.Create("HUD");
		m_Root.Parent = parent;

		// The score column right of the next queue, on a dark band so it reads against a bright sky.
		Entity band = Entity.Create("Score Band");
		band.Parent = m_Root;
		band.Translation = new Vector3(0.905f, 0.42f, 0.5f);
		m_Band = band;
		SpriteRendererComponent bandSprite = band.AddComponent<SpriteRendererComponent>();
		bandSprite.ScreenSpace = true;
		bandSprite.Color = new Color(0.0f, 0.0f, 0.04f, 0.4f);
		float column = 0.865f;
		Label("SCORE", column, 0.13f);
		m_Score = Text("0", column, 0.165f, 44.0f, s_Value, TextAlignment.Left);
		Label("LEVEL", column, 0.30f);
		m_Level = Text("1", column, 0.335f, 44.0f, s_Value, TextAlignment.Left);
		Label("LINES", column, 0.47f);
		m_Lines = Text("0", column, 0.505f, 44.0f, s_Value, TextAlignment.Left);
		Label("BEST", column, 0.64f);
		m_Best = Text("0", column, 0.675f, 44.0f, s_Best, TextAlignment.Left);

		Text("LEFT / RIGHT   move\nDOWN   soft drop\nUP / X   rotate\nZ   rotate back\nSPACE   hard drop\nC / SHIFT   hold\nESC   pause",
			 0.035f, 0.66f, 18.0f, s_Label, TextAlignment.Left);

		// The overlays, above a dimmed view.
		m_DimEntity = Entity.Create("Dim");
		m_DimEntity.Parent = m_Root;
		m_Dim = m_DimEntity.AddComponent<SpriteRendererComponent>();
		m_Dim.ScreenSpace = true;
		m_Dim.Color = new Color(0.0f, 0.0f, 0.03f, 0.55f);
		m_DimEntity.Translation = new Vector3(0.5f, 0.5f, 1.0f);

		string title = "BLOCKS";
		for (int i = 0; i < title.Length; i++)
		{
			m_Title.Add(Text(title[i].ToString(), 0.5f, 0.24f, 150.0f, s_TitleColors[i % s_TitleColors.Length], TextAlignment.Center));
			// Letters about two thirds of the size apart, centered.
			m_TitleOffsets.Add((i - ((title.Length - 1) * 0.5f)) * 108.0f);
		}
		m_Subtitle = Text("a Strada engine example", 0.5f, 0.47f, 30.0f, s_Value, TextAlignment.Center);
		m_Headline = Text("", 0.5f, 0.33f, 96.0f, s_Value, TextAlignment.Center);
		m_Detail = Text("", 0.5f, 0.5f, 30.0f, s_Value, TextAlignment.Center);
		m_Prompt = Text("", 0.5f, 0.62f, 34.0f, s_Prompt, TextAlignment.Center);
		Layout();
	}

	/// <summary>Whether an overlay (title, pause or game over) shows.</summary>
	public bool OverlayVisible { get; private set; }

	public void SetStats(int score, int level, int lines, int best)
	{
		m_Score.Text = score.ToString("N0");
		m_Level.Text = level.ToString();
		m_Lines.Text = lines.ToString();
		m_Best.Text = best.ToString("N0");
	}

	/// <summary>Shows the overlay of a state (none while playing). The prompt pulses with the time.</summary>
	public void Show(GameState state, int score, bool newBest, float time)
	{
		Layout();
		bool title = state == GameState.Title;
		foreach (TextComponent letter in m_Title)
		{
			letter.Color = WithAlpha(letter.Color, title ? 1.0f : 0.0f);
		}
		m_Subtitle.Color = WithAlpha(m_Subtitle.Color, title ? 1.0f : 0.0f);
		(string headline, string detail, string prompt) = state switch
		{
			GameState.Title => ("", "", "PRESS ENTER TO START"),
			GameState.Paused => ("PAUSED", "", "ESC or ENTER to resume"),
			GameState.Over => ("GAME OVER", newBest ? $"NEW BEST  {score:N0}" : $"SCORE  {score:N0}", "PRESS ENTER TO PLAY AGAIN"),
			_ => ("", "", ""),
		};
		m_Headline.Text = headline;
		m_Detail.Text = detail;
		m_Detail.Color = newBest ? s_Best : s_Value;
		m_Prompt.Text = prompt;
		// The prompt breathes.
		m_Prompt.Color = WithAlpha(s_Prompt, 0.8f + (0.2f * Mathf.Sin(time * 3.0f)));
		OverlayVisible = state != GameState.Playing;
		m_Dim.Color = WithAlpha(m_Dim.Color, OverlayVisible ? (title ? 0.25f : 0.55f) : 0.0f);
	}

	private static Color WithAlpha(Color color, float alpha) => new(color.R, color.G, color.B, alpha);

	private void Label(string text, float x, float y)
	{
		Text(text, x, y, 22.0f, s_Label, TextAlignment.Left);
	}

	private TextComponent Text(string text, float x, float y, float size, Color color, TextAlignment alignment)
	{
		Entity entity = Entity.Create(text.Length > 0 && text.Length < 24 ? text : "Text");
		entity.Parent = m_Root;
		// Above the dimming sprite.
		entity.Translation = new Vector3(x, y, 2.0f);
		TextComponent component = entity.AddComponent<TextComponent>();
		component.Text = text;
		component.Color = color;
		component.ScreenSpace = true;
		component.Alignment = alignment;
		component.FontSize = size;
		m_Items.Add(new Item(component, size));
		return component;
	}

	// Sizes follow the window's height; the title's letters keep their spacing.
	private void Layout()
	{
		int width = System.Math.Max(Application.WindowWidth, 1);
		int height = System.Math.Max(Application.WindowHeight, 1);
		if (width == m_LaidOutWidth && height == m_LaidOutHeight)
		{
			return;
		}
		m_LaidOutWidth = width;
		m_LaidOutHeight = height;
		float scale = height / ReferenceHeight;
		foreach (Item item in m_Items)
		{
			item.Text.FontSize = item.Size * scale;
		}
		for (int i = 0; i < m_Title.Count; i++)
		{
			Entity letter = m_Title[i].Entity;
			letter.Translation = new Vector3(0.5f + (m_TitleOffsets[i] * scale / width), letter.Translation.Y, letter.Translation.Z);
		}
		m_DimEntity.Scale = new Vector3(width, height, 1.0f);
		m_Band.Scale = new Vector3(width * 0.15f, height * 0.66f, 1.0f);
	}
}
