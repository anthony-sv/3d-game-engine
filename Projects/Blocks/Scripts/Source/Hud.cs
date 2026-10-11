using System.Collections.Generic;
using Strada;

namespace Blocks;

/// <summary>The screen of the game: the score column (score, level, lines, best), the controls, the title (a sign with the
/// game's name above the board and a prompt below it), pause and game-over overlays over a dimmed view, flashes and the
/// callouts over the board. Every text is a screen-space entity it creates; sizes are given for an 800 pixel high window
/// and follow the window's height.</summary>
public sealed class Hud
{
	private const float ReferenceHeight = 800.0f;
	// The title's letters: their size, the top of their line, and how far apart they stand (reference pixels).
	private const float TitleSize = 128.0f;
	private const float TitleY = 0.014f;
	private const float TitleSpacing = 92.0f;
	// The letters bob by this fraction of the window's height; their shadows fall this many reference pixels away.
	private const float TitleBob = 0.005f;
	private const float TitleShadowOffset = 5.0f;
	// The sign behind the title, from top to bottom as fractions of the window's height: as wide as the letters with a margin
	// on each side (reference pixels), with glowing edges. The title camera keeps the board below it.
	private const float SignTop = 0.022f;
	private const float SignBottom = 0.228f;
	private const float SignMargin = 64.0f;
	private const float SignEdge = 3.0f;
	private const float SubtitleY = 0.172f;
	// The prompt: low on the title screen, over the floor in front of the board; under the headline on the overlays.
	private const float TitlePromptY = 0.918f;
	private const float OverlayPromptY = 0.62f;
	private const float FlashSeconds = 0.3f;

	private static readonly Color s_Label = new(0.62f, 0.68f, 0.82f);
	private static readonly Color s_Value = new(0.97f, 0.97f, 1.0f);
	private static readonly Color s_Best = new(1.0f, 0.86f, 0.45f);
	private static readonly Color s_Prompt = new(1.0f, 1.0f, 1.0f);
	private static readonly Color s_Shadow = new(0.0f, 0.0f, 0.05f, 0.6f);
	private static readonly Color s_Sign = new(0.02f, 0.0f, 0.07f, 0.62f);
	private static readonly Color s_SignTop = new(0.25f, 0.9f, 1.0f, 0.9f);
	private static readonly Color s_SignBottom = new(1.0f, 0.3f, 0.8f, 0.9f);
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
	private readonly List<TextComponent> m_TitleShadows = [];
	private readonly List<float> m_TitleOffsets = [];
	private readonly TextComponent m_Subtitle;
	private readonly TextComponent m_Headline;
	private readonly TextComponent m_Detail;
	private readonly TextComponent m_Prompt;
	private readonly TextComponent m_PromptShadow;
	private readonly SpriteRendererComponent m_Dim;
	private readonly Entity m_DimEntity;
	private readonly SpriteRendererComponent m_Flash;
	private readonly Entity m_FlashEntity;
	private readonly SpriteRendererComponent m_Sign;
	private readonly SpriteRendererComponent m_SignTop;
	private readonly SpriteRendererComponent m_SignBottom;
	private readonly Entity m_Band;
	private Color m_FlashColor;
	private float m_FlashStrength;
	private float m_FlashAge = FlashSeconds;
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
		m_Score = Text("Score", "0", column, 0.165f, 44.0f, s_Value, TextAlignment.Left);
		Label("LEVEL", column, 0.30f);
		m_Level = Text("Level", "1", column, 0.335f, 44.0f, s_Value, TextAlignment.Left);
		Label("LINES", column, 0.47f);
		m_Lines = Text("Lines", "0", column, 0.505f, 44.0f, s_Value, TextAlignment.Left);
		Label("BEST", column, 0.64f);
		m_Best = Text("Best", "0", column, 0.675f, 44.0f, s_Best, TextAlignment.Left);

		Text("Controls", "LEFT / RIGHT   move\nDOWN   soft drop\nUP / X   rotate\nZ   rotate back\nSPACE   hard drop\nC / SHIFT   hold\nESC   pause",
			 0.035f, 0.66f, 18.0f, s_Label, TextAlignment.Left);

		// Flashes light up the whole view for a moment, under the texts.
		m_FlashEntity = Entity.Create("Flash");
		m_FlashEntity.Parent = m_Root;
		m_Flash = m_FlashEntity.AddComponent<SpriteRendererComponent>();
		m_Flash.ScreenSpace = true;
		m_Flash.Color = new Color(1.0f, 1.0f, 1.0f, 0.0f);
		m_FlashEntity.Translation = new Vector3(0.5f, 0.5f, 0.8f);

		// The overlays, above a dimmed view.
		m_DimEntity = Entity.Create("Dim");
		m_DimEntity.Parent = m_Root;
		m_Dim = m_DimEntity.AddComponent<SpriteRendererComponent>();
		m_Dim.ScreenSpace = true;
		m_Dim.Color = new Color(0.0f, 0.0f, 0.03f, 0.55f);
		m_DimEntity.Translation = new Vector3(0.5f, 0.5f, 1.0f);

		// The title stands on a dark sign with glowing edges, above the board.
		m_Sign = Sprite("Title Sign", (SignTop + SignBottom) * 0.5f, 1.5f, s_Sign);
		m_SignTop = Sprite("Title Sign Top", SignTop, 1.6f, s_SignTop);
		m_SignBottom = Sprite("Title Sign Bottom", SignBottom, 1.6f, s_SignBottom);

		string title = "BLOCKS";
		for (int i = 0; i < title.Length; i++)
		{
			string letter = title[i].ToString();
			m_TitleShadows.Add(Text($"Title {letter} Shadow", letter, 0.5f, TitleY, TitleSize, s_Shadow, TextAlignment.Center, 1.9f));
			m_Title.Add(Text($"Title {letter}", letter, 0.5f, TitleY, TitleSize, s_TitleColors[i % s_TitleColors.Length], TextAlignment.Center));
			m_TitleOffsets.Add((i - ((title.Length - 1) * 0.5f)) * TitleSpacing);
		}
		m_Subtitle = Text("Subtitle", "a Strada engine example", 0.5f, SubtitleY, 24.0f, s_Value, TextAlignment.Center);
		m_Headline = Text("Headline", "", 0.5f, 0.33f, 96.0f, s_Value, TextAlignment.Center);
		m_Detail = Text("Detail", "", 0.5f, 0.5f, 30.0f, s_Value, TextAlignment.Center);
		m_PromptShadow = Text("Prompt Shadow", "", 0.5f, OverlayPromptY, 34.0f, s_Shadow, TextAlignment.Center, 1.9f);
		m_Prompt = Text("Prompt", "", 0.5f, OverlayPromptY, 34.0f, s_Prompt, TextAlignment.Center);
		Callouts = new Callouts(m_Root);
		Layout();
	}

	/// <summary>Whether an overlay (title, pause or game over) shows.</summary>
	public bool OverlayVisible { get; private set; }

	/// <summary>The words that pop over the board.</summary>
	public Callouts Callouts { get; }

	public void SetStats(int score, int level, int lines, int best)
	{
		m_Score.Text = score.ToString("N0");
		m_Level.Text = level.ToString();
		m_Lines.Text = lines.ToString();
		m_Best.Text = best.ToString("N0");
	}

	/// <summary>Lights up the view in a color for a moment; strength is the opacity it starts with.</summary>
	public void Flash(Color color, float strength)
	{
		m_FlashColor = color;
		m_FlashStrength = Mathf.Clamp01(strength);
		m_FlashAge = 0.0f;
	}

	/// <summary>Shows the overlay of a state (none while playing) and moves flashes and callouts on by the time step. On the
	/// title the board plays itself under a light dimming, between the sign and the prompt; the prompt pulses with the time.
	/// Callouts wait, hidden, while the game is paused.</summary>
	public void Show(GameState state, int score, bool newBest, float time, float deltaTime)
	{
		Layout();
		bool title = state == GameState.Title;
		float offset = TitleShadowOffset * m_LaidOutHeight / ReferenceHeight;
		for (int i = 0; i < m_Title.Count; i++)
		{
			float bob = TitleBob * Mathf.Sin((time * 2.2f) - (i * 0.7f));
			Entity letter = m_Title[i].Entity;
			letter.Translation = new Vector3(letter.Translation.X, TitleY + bob, letter.Translation.Z);
			Entity shadow = m_TitleShadows[i].Entity;
			shadow.Translation = new Vector3(letter.Translation.X + (offset / m_LaidOutWidth), TitleY + bob + (offset / m_LaidOutHeight),
											 shadow.Translation.Z);
			m_Title[i].Color = WithAlpha(m_Title[i].Color, title ? 1.0f : 0.0f);
			m_TitleShadows[i].Color = WithAlpha(s_Shadow, title ? s_Shadow.A : 0.0f);
		}
		m_Subtitle.Color = WithAlpha(m_Subtitle.Color, title ? 1.0f : 0.0f);
		m_Sign.Color = WithAlpha(s_Sign, title ? s_Sign.A : 0.0f);
		m_SignTop.Color = WithAlpha(s_SignTop, title ? s_SignTop.A : 0.0f);
		m_SignBottom.Color = WithAlpha(s_SignBottom, title ? s_SignBottom.A : 0.0f);
		(string headline, string detail, string prompt) = state switch
		{
			GameState.Title => ("", "", "PRESS ENTER OR CLICK TO PLAY"),
			GameState.Paused => ("PAUSED", "", "PRESS ESC OR ENTER TO RESUME"),
			GameState.Over => ("GAME OVER", newBest ? $"NEW BEST  {score:N0}" : $"SCORE  {score:N0}", "PRESS ENTER OR CLICK TO PLAY AGAIN"),
			_ => ("", "", ""),
		};
		m_Headline.Text = headline;
		m_Detail.Text = detail;
		m_Detail.Color = newBest ? s_Best : s_Value;
		m_Prompt.Text = prompt;
		m_PromptShadow.Text = prompt;
		float promptY = title ? TitlePromptY : OverlayPromptY;
		m_Prompt.Entity.Translation = new Vector3(0.5f, promptY, m_Prompt.Entity.Translation.Z);
		m_PromptShadow.Entity.Translation = new Vector3(0.5f + (offset * 0.5f / m_LaidOutWidth), promptY + (offset * 0.5f / m_LaidOutHeight),
														m_PromptShadow.Entity.Translation.Z);
		// The prompt breathes.
		float breath = 0.75f + (0.25f * Mathf.Sin(time * 3.0f));
		m_Prompt.Color = WithAlpha(s_Prompt, breath);
		m_PromptShadow.Color = WithAlpha(s_Shadow, s_Shadow.A * breath);
		OverlayVisible = state != GameState.Playing;
		m_Dim.Color = WithAlpha(m_Dim.Color, OverlayVisible ? (title ? 0.12f : 0.55f) : 0.0f);

		m_FlashAge += deltaTime;
		float flash = Mathf.Clamp01(1.0f - (m_FlashAge / FlashSeconds));
		m_Flash.Color = WithAlpha(m_FlashColor, m_FlashStrength * flash * flash);
		Callouts.Hidden = state == GameState.Paused;
		Callouts.Update(deltaTime, m_LaidOutWidth, m_LaidOutHeight);
	}

	private static Color WithAlpha(Color color, float alpha) => new(color.R, color.G, color.B, alpha);

	private void Label(string text, float x, float y)
	{
		Text($"{text} Label", text, x, y, 22.0f, s_Label, TextAlignment.Left);
	}

	private TextComponent Text(string name, string text, float x, float y, float size, Color color, TextAlignment alignment, float layer = 2.0f)
	{
		Entity entity = Entity.Create(name);
		entity.Parent = m_Root;
		// Above the dimming sprite.
		entity.Translation = new Vector3(x, y, layer);
		TextComponent component = entity.AddComponent<TextComponent>();
		component.Text = text;
		component.Color = color;
		component.ScreenSpace = true;
		component.Alignment = alignment;
		component.FontSize = size;
		m_Items.Add(new Item(component, size));
		return component;
	}

	// A quad centered on the window at a height (a fraction of the window's height), sized by Layout.
	private SpriteRendererComponent Sprite(string name, float y, float layer, Color color)
	{
		Entity entity = Entity.Create(name);
		entity.Parent = m_Root;
		entity.Translation = new Vector3(0.5f, y, layer);
		SpriteRendererComponent sprite = entity.AddComponent<SpriteRendererComponent>();
		sprite.ScreenSpace = true;
		sprite.Color = color;
		return sprite;
	}

	// Sizes follow the window's height; the title's letters keep their spacing and the sign fits them.
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
		m_FlashEntity.Scale = new Vector3(width, height, 1.0f);
		float signWidth = System.Math.Min(((m_Title.Count * TitleSpacing) + (2.0f * SignMargin)) * scale, width);
		m_Sign.Entity.Scale = new Vector3(signWidth, height * (SignBottom - SignTop), 1.0f);
		m_SignTop.Entity.Scale = new Vector3(signWidth, SignEdge * scale, 1.0f);
		m_SignBottom.Entity.Scale = new Vector3(signWidth, SignEdge * scale, 1.0f);
		m_Band.Scale = new Vector3(width * 0.15f, height * 0.66f, 1.0f);
	}
}
