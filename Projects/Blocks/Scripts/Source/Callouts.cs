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

/// <summary>Words that pop over the board when something happens: they scale in with a little overshoot, hold, then drift up
/// and fade out. Each slot is a UI element of the canvas (named "Clear Callout", "Bonus Callout" and "Level Callout" under
/// "Callouts", a point over the board's middle) with four texts: its line and the smaller one under it, each over its drop
/// shadow (named "&lt;Slot&gt; Callout Text", "... Text Shadow", "... Detail" and "... Detail Shadow"). Sizes are in canvas
/// units.
///
/// The slots stand in a column over the board, the bonus above the clear and the level below it. Each slot has room of its
/// own for the tallest callout it takes, at the top of its pop and while it drifts up, so callouts never run into one
/// another, however they overlap in time.</summary>
public sealed class Callouts
{
	// A callout appears over the first moments of its pop, grows past its size to the top of the pop, then settles; over its
	// last moments it fades while it grows a little and drifts up. It lasts at least as long as all of that.
	private const float AppearSeconds = 0.06f;
	private const float PopSeconds = 0.16f;
	private const float SettleSeconds = 0.12f;
	private const float FadeSeconds = 0.45f;
	private const float ShortestSeconds = PopSeconds + SettleSeconds + FadeSeconds;
	private const float PopScale = 1.15f;
	private const float FadeGrowth = 0.06f;
	// How far a fading callout drifts up (canvas units).
	private const float Drift = 24.0f;
	// The line under the main one: its size and its gap from the main line, in parts of the main line's size.
	private const float DetailScale = 0.45f;
	private const float DetailGap = 0.02f;
	// The drop shadow's offset (canvas units at a callout's size).
	private const float ShadowOffset = 4.0f;
	// The room between slots (canvas units).
	private const float SlotGap = 8.0f;
	private static readonly Color s_ShadowColor = new(0.0f, 0.0f, 0.04f, 0.65f);
	private static readonly Color s_DetailColor = new(1.0f, 1.0f, 1.0f, 0.92f);

	// The slots from the top of the column down, with the tallest callout each takes: the size of its main line and whether a
	// line shows under it.
	private static readonly (CalloutSlot Slot, float Size, bool Detail)[] s_Column =
	[
		(CalloutSlot.Bonus, 46.0f, false), (CalloutSlot.Clear, 114.0f, true), (CalloutSlot.Level, 70.0f, true),
	];

	// Each slot's room, indexed by slot.
	private static readonly SlotRoom[] s_Rooms = Arrange();

	private readonly Callout[] m_Callouts = new Callout[s_Column.Length];

	/// <summary>The callouts of the canvas's slots, whose elements find returns by their names.</summary>
	public Callouts(System.Func<string, Entity> find)
	{
		foreach ((CalloutSlot Slot, float Size, bool Detail) entry in s_Column)
		{
			m_Callouts[(int)entry.Slot] = new Callout(find, entry.Slot);
		}
		Clear();
	}

	/// <summary>Whether the callouts wait out of sight (while a menu covers the board): hidden callouts neither show nor age,
	/// and go on where they were when shown again.</summary>
	public bool Hidden { get; set; }

	/// <summary>The band a slot's callouts keep to, from top to bottom in canvas units from the middle of the column (y
	/// downwards): the slots' bands do not overlap.</summary>
	public static (float Top, float Bottom) Room(CalloutSlot slot) => (s_Rooms[(int)slot].Top, s_Rooms[(int)slot].Bottom);

	/// <summary>Pops a callout: the text in a color with an optional smaller line under it, for a few seconds (at least the
	/// time its pop and fade take). A callout taller than its slot's room allows is made smaller to fit. A shimmering callout
	/// cycles through colors. It shows from the next <see cref="Update"/>, which places it.</summary>
	public void Show(CalloutSlot slot, string text, Color color, string detail = "", float size = 80.0f, float seconds = 1.4f, bool shimmer = false)
	{
		Callout callout = m_Callouts[(int)slot];
		callout.Message = text;
		callout.HasDetail = detail.Length > 0;
		callout.Text.Text.Text = text;
		callout.TextShadow.Text.Text = text;
		callout.Detail.Text.Text = detail;
		callout.DetailShadow.Text.Text = detail;
		callout.Group.Opacity = 0.0f;
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
		return callout.Active ? callout.Message : null;
	}

	/// <summary>Hides every callout at once.</summary>
	public void Clear()
	{
		foreach (Callout callout in m_Callouts)
		{
			Hide(callout);
		}
	}

	/// <summary>Moves the callouts on by the time step.</summary>
	public void Update(float deltaTime)
	{
		for (int slot = 0; slot < m_Callouts.Length; slot++)
		{
			Callout callout = m_Callouts[slot];
			if (!callout.Active)
			{
				continue;
			}
			if (Hidden)
			{
				callout.Group.Opacity = 0.0f;
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
			callout.Group.Opacity = Mathf.Clamp01(age / AppearSeconds) * (1.0f - (fade * fade));

			// A shimmering callout runs through the rainbow, from gold.
			Color color = callout.Shimmer ? Rainbow(0.12f + (age * 0.9f), 0.6f) : callout.Color;
			float main = callout.Size * size;
			float detail = DetailScale * main;
			float shadow = ShadowOffset * size;
			// The main line and the line under it are centered together on the slot's middle, which drifts up as the callout
			// fades.
			float top = s_Rooms[slot].Middle - (Drift * fade * fade) - (0.5f * main * Lines(callout.HasDetail));
			callout.Text.Place(top, main, 0.0f, color);
			callout.TextShadow.Place(top, main, shadow, s_ShadowColor);
			float detailTop = top + ((1.0f + DetailGap) * main);
			callout.Detail.Place(detailTop, detail, 0.0f, s_DetailColor);
			callout.DetailShadow.Place(detailTop, detail, shadow, s_ShadowColor);
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
		float top = -0.5f * column;
		for (int i = 0; i < s_Column.Length; i++)
		{
			float tallest = s_Column[i].Size * Lines(s_Column[i].Detail);
			rooms[(int)s_Column[i].Slot] = new SlotRoom(tallest, top + above[i], top, top + above[i] + below[i]);
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

	private static void Hide(Callout callout)
	{
		callout.Active = false;
		callout.Group.Opacity = 0.0f;
		foreach (Line line in callout.Lines)
		{
			line.Text.Text = "";
		}
	}

	// A slot's room: the height of the tallest callout it takes (at its size, canvas units), where its middle rests and the
	// band it keeps to (canvas units from the middle of the column).
	private readonly record struct SlotRoom(float Tallest, float Middle, float Top, float Bottom);

	// A slot's element, its lines and the callout it shows.
	private sealed class Callout
	{
		public readonly UIRectComponent Group;
		public readonly Line Text;
		public readonly Line TextShadow;
		public readonly Line Detail;
		public readonly Line DetailShadow;
		// All four, to empty them together.
		public readonly Line[] Lines;
		public string Message = "";
		public bool HasDetail;
		public float Age;
		public float Duration;
		public float Size;
		public Color Color;
		public bool Shimmer;
		public bool Active;

		public Callout(System.Func<string, Entity> find, CalloutSlot slot)
		{
			string name = $"{slot} Callout";
			Group = find(name).GetComponent<UIRectComponent>()!;
			Text = new Line(find($"{name} Text"));
			TextShadow = new Line(find($"{name} Text Shadow"));
			Detail = new Line(find($"{name} Detail"));
			DetailShadow = new Line(find($"{name} Detail Shadow"));
			Lines = [Text, TextShadow, Detail, DetailShadow];
		}
	}

	// A line of a callout: its text, in a rectangle as tall as the line and centered across the column.
	private sealed class Line
	{
		public readonly UITextComponent Text;
		private readonly UIRectComponent m_Rect;
		private readonly float m_Width;

		public Line(Entity entity)
		{
			Text = entity.GetComponent<UITextComponent>()!;
			m_Rect = entity.GetComponent<UIRectComponent>()!;
			m_Width = m_Rect.Size.X;
		}

		// Places the line: its top (canvas units from the column's middle), its size, how far its shadow falls and its color.
		public void Place(float top, float size, float offset, Color color)
		{
			m_Rect.Position = new Vector2(offset, top + offset);
			m_Rect.Size = new Vector2(m_Width, size);
			Text.FontSize = size;
			Text.Color = color;
		}
	}
}
