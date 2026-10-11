using Strada;

namespace Blocks;

/// <summary>Where a callout shows over the board. Each slot shows one callout; a new one replaces it.</summary>
public enum CalloutSlot
{
	/// <summary>Rows cleared ("SINGLE" to "TETRIS!") with the points they made.</summary>
	Clear,
	/// <summary>A bonus above the clear: back-to-back clears and combos.</summary>
	Bonus,
	/// <summary>A new level (or the start of a game), below the clear.</summary>
	Level,
}

/// <summary>Words that pop over the board when something happens: they scale in with a little overshoot, hold, then drift
/// up and fade out. Each is a screen-space text with a drop shadow and an optional smaller line under it; sizes are given
/// for an 800 pixel high window and follow its height.
///
/// The slots stand in a column over the board, the bonus above the clear and the level below it. Each slot has room of its
/// own for the tallest callout it takes, at the top of its pop and while it drifts up, so callouts never run into one
/// another, however they overlap in time.</summary>
public sealed class Callouts
{
	private const float ReferenceHeight = 800.0f;
	// A callout appears over the first moments of its pop, grows past its size to the top of the pop, then settles; over its
	// last moments it fades while it grows a little and drifts up. It lasts at least as long as all of that.
	private const float AppearSeconds = 0.06f;
	private const float PopSeconds = 0.16f;
	private const float SettleSeconds = 0.12f;
	private const float FadeSeconds = 0.45f;
	private const float ShortestSeconds = PopSeconds + SettleSeconds + FadeSeconds;
	private const float PopScale = 1.15f;
	private const float FadeGrowth = 0.06f;
	// How far a fading callout drifts up (reference pixels).
	private const float Drift = 18.0f;
	// The line under the main one: its size and its gap from the main line, in parts of the main line's size.
	private const float DetailScale = 0.45f;
	private const float DetailGap = 0.02f;
	// The drop shadow's offset (reference pixels).
	private const float ShadowOffset = 3.0f;
	// The middle of the column of slots (a fraction of the window's height) and the room between slots (reference pixels).
	private const float ColumnMiddle = 0.47f;
	private const float SlotGap = 6.0f;

	// The slots from the top of the column down, with the tallest callout each takes: the size of its main line and whether a
	// line shows under it.
	private static readonly (CalloutSlot Slot, float Size, bool Detail)[] s_Column =
	[
		(CalloutSlot.Bonus, 34.0f, false), (CalloutSlot.Clear, 84.0f, true), (CalloutSlot.Level, 52.0f, true),
	];

	// Each slot's room, indexed by slot.
	private static readonly SlotRoom[] s_Rooms = Arrange();

	private readonly Callout[] m_Callouts = new Callout[s_Column.Length];

	public Callouts(Entity parent)
	{
		Entity root = Entity.Create("Callouts");
		root.Parent = parent;
		foreach ((CalloutSlot Slot, float Size, bool Detail) entry in s_Column)
		{
			m_Callouts[(int)entry.Slot] = new Callout(root, entry.Slot);
		}
	}

	/// <summary>Whether the callouts wait out of sight (while an overlay covers the board): hidden callouts neither show nor
	/// age, and go on where they were when shown again.</summary>
	public bool Hidden { get; set; }

	/// <summary>The band a slot's callouts keep to, from top to bottom as fractions of the window's height: the slots' bands
	/// do not overlap.</summary>
	public static (float Top, float Bottom) Room(CalloutSlot slot) => (s_Rooms[(int)slot].Top, s_Rooms[(int)slot].Bottom);

	/// <summary>Pops a callout: the text in a color with an optional smaller line under it, for a few seconds (at least the
	/// time its pop and fade take). A callout taller than its slot's room allows is made smaller to fit. A shimmering callout
	/// cycles through colors. It shows from the next <see cref="Update"/>, which places it.</summary>
	public void Show(CalloutSlot slot, string text, Color color, string detail = "", float size = 60.0f, float seconds = 1.4f, bool shimmer = false)
	{
		Callout callout = m_Callouts[(int)slot];
		callout.Text.Text = text;
		callout.Shadow.Text = text;
		callout.Detail.Text = detail;
		callout.DetailShadow.Text = detail;
		Conceal(callout);
		callout.Age = 0.0f;
		callout.Duration = Mathf.Max(seconds, ShortestSeconds);
		callout.Size = Mathf.Min(size, s_Rooms[(int)slot].Tallest / Lines(detail.Length > 0));
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
		for (int slot = 0; slot < m_Callouts.Length; slot++)
		{
			Callout callout = m_Callouts[slot];
			if (!callout.Active)
			{
				continue;
			}
			if (Hidden)
			{
				Conceal(callout);
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
							 ? Mathf.LerpUnclamped(0.35f, PopScale, EaseOut(age / PopSeconds))
							 : Mathf.Lerp(PopScale, 1.0f, (age - PopSeconds) / SettleSeconds) + (FadeGrowth * fade);
			float alpha = Mathf.Clamp01(age / AppearSeconds) * (1.0f - (fade * fade));

			// A shimmering callout runs through the rainbow, from gold.
			Color color = callout.Shimmer ? Rainbow(0.12f + (age * 0.9f), 0.6f) : callout.Color;
			float pixels = callout.Size * scale * size;
			float detailPixels = DetailScale * pixels;
			float shadow = ShadowOffset * scale * size;
			// Texts hang from their top: the main line and the line under it are centered together on the slot's middle,
			// which drifts up as the callout fades.
			float lines = pixels * Lines(callout.Detail.Text.Length > 0);
			float top = s_Rooms[slot].Middle - (Drift * fade * fade / ReferenceHeight) - (0.5f * lines / height);
			Place(callout.Text, 0.5f, top, pixels, new Color(color.R, color.G, color.B, alpha));
			Place(callout.Shadow, 0.5f + (shadow / width), top + (shadow / height), pixels, new Color(0.0f, 0.0f, 0.05f, 0.6f * alpha));
			float detailTop = top + ((1.0f + DetailGap) * pixels / height);
			Place(callout.Detail, 0.5f, detailTop, detailPixels, new Color(1.0f, 1.0f, 1.0f, 0.92f * alpha));
			Place(callout.DetailShadow, 0.5f + (shadow / width), detailTop + (shadow / height), detailPixels,
				  new Color(0.0f, 0.0f, 0.05f, 0.6f * alpha));
		}
	}

	// The height of a callout's lines (without the shadow) in parts of its main line's size.
	private static float Lines(bool detail) => detail ? 1.0f + DetailGap + DetailScale : 1.0f;

	// Stacks the slots' rooms from the top of the column, centered on its middle. Above its resting middle a slot keeps room
	// for the top of the pop or for the drift, below it for the top of the pop with the shadow under it.
	private static SlotRoom[] Arrange()
	{
		float[] above = new float[s_Column.Length];
		float[] below = new float[s_Column.Length];
		float column = -SlotGap;
		for (int i = 0; i < s_Column.Length; i++)
		{
			float tallest = s_Column[i].Size * Lines(s_Column[i].Detail);
			above[i] = Mathf.Max(0.5f * PopScale * tallest, Drift + (0.5f * (1.0f + FadeGrowth) * tallest));
			below[i] = PopScale * ((0.5f * tallest) + ShadowOffset);
			column += SlotGap + above[i] + below[i];
		}
		SlotRoom[] rooms = new SlotRoom[s_Column.Length];
		float top = (ColumnMiddle * ReferenceHeight) - (0.5f * column);
		for (int i = 0; i < s_Column.Length; i++)
		{
			float tallest = s_Column[i].Size * Lines(s_Column[i].Detail);
			rooms[(int)s_Column[i].Slot] = new SlotRoom(tallest, (top + above[i]) / ReferenceHeight, top / ReferenceHeight,
														(top + above[i] + below[i]) / ReferenceHeight);
			top += above[i] + below[i] + SlotGap;
		}
		return rooms;
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

	// Makes the callout's texts transparent where they are.
	private static void Conceal(Callout callout)
	{
		foreach (TextComponent text in callout.Texts)
		{
			text.Color = new Color(text.Color.R, text.Color.G, text.Color.B, 0.0f);
		}
	}

	private static void Hide(Callout callout)
	{
		callout.Active = false;
		foreach (TextComponent text in callout.Texts)
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

	// A slot's room: the height of the tallest callout it takes (at its size, reference pixels), where its middle rests and
	// the band it keeps to (fractions of the window's height).
	private readonly record struct SlotRoom(float Tallest, float Middle, float Top, float Bottom);

	// A slot's texts and the callout it shows.
	private sealed class Callout
	{
		public readonly TextComponent Text;
		public readonly TextComponent Shadow;
		public readonly TextComponent Detail;
		public readonly TextComponent DetailShadow;
		// All four, to hide them together.
		public readonly TextComponent[] Texts;
		public float Age;
		public float Duration;
		public float Size;
		public Color Color;
		public bool Shimmer;
		public bool Active;

		public Callout(Entity root, CalloutSlot slot)
		{
			Text = CreateText(root, $"{slot} Callout", 2.6f);
			Shadow = CreateText(root, $"{slot} Callout Shadow", 2.5f);
			Detail = CreateText(root, $"{slot} Callout Detail", 2.6f);
			DetailShadow = CreateText(root, $"{slot} Callout Detail Shadow", 2.5f);
			Texts = [Text, Shadow, Detail, DetailShadow];
		}
	}
}
