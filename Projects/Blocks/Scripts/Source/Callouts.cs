using Strada;

namespace Blocks;

/// <summary>Where a callout shows over the board. Each slot shows one callout; a new one replaces it.</summary>
public enum CalloutSlot
{
	/// <summary>Rows cleared ("SINGLE" to "TETRIS!") with the points they made.</summary>
	Clear,
	/// <summary>A bonus above the clear: back-to-back clears and combos.</summary>
	Bonus,
	/// <summary>A new level, below the clear.</summary>
	Level,
}

/// <summary>Words that pop over the board when something happens: they scale in with a little overshoot, hold, then drift
/// up and fade out. Each is a screen-space text with a drop shadow and an optional smaller line under it; sizes are given
/// for an 800 pixel high window and follow its height.</summary>
public sealed class Callouts
{
	private const float ReferenceHeight = 800.0f;
	// A callout appears over the first moments of its pop, then settles to its size; it fades over its last moments.
	private const float AppearSeconds = 0.06f;
	private const float PopSeconds = 0.16f;
	private const float SettleSeconds = 0.12f;
	private const float FadeSeconds = 0.45f;
	// How far a fading callout drifts up, as a fraction of the window's height.
	private const float Drift = 0.035f;
	// The drop shadow's offset in reference pixels.
	private const float ShadowOffset = 3.0f;

	// A slot's texts and the callout it shows; Y is the height of its center as a fraction of the window's height.
	private sealed class Callout(Entity root, float y)
	{
		public readonly TextComponent Text = CreateText(root, "Callout", 2.6f);
		public readonly TextComponent Shadow = CreateText(root, "Callout Shadow", 2.5f);
		public readonly TextComponent Detail = CreateText(root, "Callout Detail", 2.6f);
		public readonly TextComponent DetailShadow = CreateText(root, "Callout Detail Shadow", 2.5f);
		public readonly float Y = y;
		public float Age;
		public float Duration;
		public float Size;
		public Color Color;
		public bool Shimmer;
		public bool Active;
	}

	private readonly Callout[] m_Callouts;

	public Callouts(Entity parent)
	{
		Entity root = Entity.Create("Callouts");
		root.Parent = parent;
		// Centered over the board, the bonus above the clear and the level below it.
		m_Callouts = [new Callout(root, 0.46f), new Callout(root, 0.38f), new Callout(root, 0.585f)];
	}

	/// <summary>Pops a callout: the text in a color with an optional smaller line under it, for a few seconds. A shimmering
	/// callout cycles through colors.</summary>
	public void Show(CalloutSlot slot, string text, Color color, string detail = "", float size = 60.0f, float seconds = 1.4f, bool shimmer = false)
	{
		Callout callout = m_Callouts[(int)slot];
		callout.Text.Text = text;
		callout.Shadow.Text = text;
		callout.Detail.Text = detail;
		callout.DetailShadow.Text = detail;
		callout.Age = 0.0f;
		callout.Duration = seconds;
		callout.Size = size;
		callout.Color = color;
		callout.Shimmer = shimmer;
		callout.Active = true;
	}

	/// <summary>The text of the callout showing in the slot, or null when none shows.</summary>
	public string? Showing(CalloutSlot slot)
	{
		Callout callout = m_Callouts[(int)slot];
		return callout.Active ? callout.Text.Text : null;
	}

	/// <summary>Hides every callout at once.</summary>
	public void Clear()
	{
		foreach (Callout callout in m_Callouts)
		{
			Hide(callout);
		}
	}

	/// <summary>Moves the callouts on by the time step, for a window of the given size.</summary>
	public void Update(float deltaTime, int width, int height)
	{
		float scale = height / ReferenceHeight;
		foreach (Callout callout in m_Callouts)
		{
			if (!callout.Active)
			{
				continue;
			}
			callout.Age += deltaTime;
			if (callout.Age >= callout.Duration)
			{
				Hide(callout);
				continue;
			}

			// Pops in past full size, settles back, and grows a little as it fades away.
			float age = callout.Age;
			float fade = Mathf.Clamp01((age - (callout.Duration - FadeSeconds)) / FadeSeconds);
			float size = age < PopSeconds
							 ? Mathf.LerpUnclamped(0.35f, 1.15f, EaseOut(age / PopSeconds))
							 : Mathf.Lerp(1.15f, 1.0f, (age - PopSeconds) / SettleSeconds) + (0.06f * fade);
			float alpha = Mathf.Clamp01(age / AppearSeconds) * (1.0f - (fade * fade));
			float y = callout.Y - (Drift * fade * fade);

			// A shimmering callout runs through the rainbow, from gold.
			Color color = callout.Shimmer ? Rainbow(0.12f + (age * 0.9f), 0.6f) : callout.Color;
			float pixels = callout.Size * scale * size;
			float detailPixels = 0.45f * callout.Size * scale * size;
			float shadow = ShadowOffset * scale * size;
			// Texts hang from their top: the main line is centered on the slot, the detail just under it.
			float top = y - (0.5f * pixels / height);
			Place(callout.Text, 0.5f, top, pixels, new Color(color.R, color.G, color.B, alpha));
			Place(callout.Shadow, 0.5f + (shadow / width), top + (shadow / height), pixels, new Color(0.0f, 0.0f, 0.05f, 0.6f * alpha));
			float detailTop = top + (1.02f * pixels / height);
			Place(callout.Detail, 0.5f, detailTop, detailPixels, new Color(1.0f, 1.0f, 1.0f, 0.92f * alpha));
			Place(callout.DetailShadow, 0.5f + (shadow / width), detailTop + (shadow / height), detailPixels,
				  new Color(0.0f, 0.0f, 0.05f, 0.6f * alpha));
		}
	}

	private static float EaseOut(float t) => 1.0f - ((1.0f - t) * (1.0f - t) * (1.0f - t));

	// The bright color of a hue (0 to 1, repeating: red, yellow, green, cyan, blue, magenta) at a saturation.
	private static Color Rainbow(float hue, float saturation)
	{
		// Six sectors: in each, one channel is full, one is off and the middle one rises or falls between them.
		float sector = (hue - Mathf.Floor(hue)) * 6.0f;
		float middle = saturation * (1.0f - Mathf.Abs((sector % 2.0f) - 1.0f));
		(float r, float g, float b) = sector switch
		{
			< 1.0f => (saturation, middle, 0.0f),
			< 2.0f => (middle, saturation, 0.0f),
			< 3.0f => (0.0f, saturation, middle),
			< 4.0f => (0.0f, middle, saturation),
			< 5.0f => (middle, 0.0f, saturation),
			_ => (saturation, 0.0f, middle),
		};
		float white = 1.0f - saturation;
		return new Color(r + white, g + white, b + white);
	}

	private static void Place(TextComponent text, float x, float y, float pixels, Color color)
	{
		Entity entity = text.Entity;
		entity.Translation = new Vector3(x, y, entity.Translation.Z);
		text.FontSize = pixels;
		text.Color = color;
	}

	private static void Hide(Callout callout)
	{
		callout.Active = false;
		foreach (TextComponent text in new[] { callout.Text, callout.Shadow, callout.Detail, callout.DetailShadow })
		{
			text.Text = "";
		}
	}

	private static TextComponent CreateText(Entity root, string name, float layer)
	{
		Entity entity = Entity.Create(name);
		entity.Parent = root;
		// Above the dimmed view and the flash; shadows under their texts.
		entity.Translation = new Vector3(0.5f, 0.5f, layer);
		TextComponent text = entity.AddComponent<TextComponent>();
		text.ScreenSpace = true;
		text.Alignment = TextAlignment.Center;
		text.Text = "";
		return text;
	}
}
