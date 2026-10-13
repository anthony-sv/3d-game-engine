using System.Collections.Generic;
using System.Globalization;
using Strada;

namespace Blocks;

/// <summary>The game's screen, on the engine's UI system: a canvas of the scene (<see cref="BlocksGame.HudCanvas"/>) whose
/// elements this class finds by name and brings to life. It shows the score panel (the score, the level with the rows to the
/// next one, the lines and the best score, tagged DEMO while the title's demo plays), the HOLD and NEXT labels over the
/// board's panels, the title (the game's name in block letters over the demo, with Play and Quit), the pause menu (Resume,
/// Restart, Quit), the game-over screen (the score, a new best, Play Again and Quit), flashes and the callouts over the
/// board. Screens fade in and out; the title's sign (its block letters and subtitle) waits for the camera to stand back
/// from the board before it shows. The menus take the mouse and the keyboard (see <see cref="Menu"/>) and their clicks
/// reach the game.</summary>
public sealed class Hud : System.IDisposable
{
	// Seconds a flash takes to fade.
	private const float FlashSeconds = 0.3f;
	// Seconds screens and the dim take to fade in and out, eased at both ends.
	private const float FadeInSeconds = 0.22f;
	private const float FadeOutSeconds = 0.16f;
	// How far a menu rises as it fades in (canvas units).
	private const float MenuRise = 28.0f;
	// The dim over the game: none during a game and over the title's demo, dark under the pause and game-over menus.
	private const float MenuDim = 0.62f;
	// The title's block letters bob: by this much (canvas units), this fast (radians per second), each a little after the
	// one before it (radians).
	private const float TileBob = 7.0f;
	private const float TileBobSpeed = 2.2f;
	private const float TileBobDelay = 0.7f;
	// A number grows by this part of its size when it goes up, and settles back over the time (seconds).
	private const float PopGrowth = 0.22f;
	private const float PopSeconds = 0.3f;
	// How fast the level's bar follows the rows (the part of the way left per second, exponentially).
	private const float BarResponse = 8.0f;
	// Seconds the game-over menu waits before it takes keys, so the keys pressed for the game as it ends do not move its focus.
	// Only Enter clicks it: Space, the hard drop, stays the game's, so a player who keeps pressing it as the stack reaches the
	// top keeps the game-over screen in front of them.
	private const float GameOverKeyDelay = 0.5f;
	// The HOLD and NEXT labels: their letters' size and the drop shadow's offset, in parts of their header's height.
	private const float LabelSize = 0.56f;
	private const float LabelShadow = 0.05f;
	// The title's sign fades in over the end of the camera's glide to the attract view, from this part of the way on (see
	// CameraShake.AttractWeight): by then the board's panels and their labels have moved down out from under it, on screens
	// from 5:4 to 21:9.
	private const float SignFromGlide = 0.85f;

	private static readonly Color s_Value = new(0.96f, 0.97f, 1.0f);
	private static readonly Color s_Best = new(1.0f, 0.68f, 0.16f);

	private readonly BlocksGame m_Game;
	// The canvas's elements by name.
	private readonly Dictionary<string, Entity> m_Elements = [];
	private readonly CameraShake? m_Shake;
	private readonly CameraComponent? m_Camera;
	private readonly BoardLabel m_HoldLabel;
	private readonly BoardLabel m_NextLabel;
	private readonly UIImageComponent m_Flash;
	private readonly UIRectComponent m_FlashRect;
	private readonly Screen m_Dim;
	private readonly Counter m_Score;
	private readonly Counter m_Level;
	private readonly Counter m_Lines;
	private readonly Counter m_Best;
	private readonly UIRectComponent m_LevelBar;
	private readonly UIRectComponent m_DemoTag;
	private readonly Screen m_Title;
	private readonly UIRectComponent m_Sign;
	private readonly Screen m_Pause;
	private readonly Screen m_Over;
	private readonly List<(UIRectComponent Rect, Vector2 Home)> m_Tiles = [];
	private readonly Menu m_TitleMenu;
	private readonly Menu m_PauseMenu;
	private readonly Menu m_OverMenu;
	private readonly UITextComponent m_FinalScore;
	private readonly UITextComponent m_FinalStats;
	private readonly UIRectComponent m_NewBest;
	private float m_FlashStrength;
	private float m_FlashAge = FlashSeconds;
	// What the screen shows of the flash, the level's bar and the DEMO tag (written when it changes).
	private float m_FlashShown = -1.0f;
	private float m_Bar;
	private float m_BarShown = -1.0f;
	private bool? m_DemoShown;
	// How far the title's sign shows (written when it changes).
	private float m_SignShown = -1.0f;
	// The state the screen showed last, to fill the game-over screen once when the game ends.
	private GameState? m_State;

	/// <summary>The screen of a game on its canvas; the HOLD and NEXT labels stand over the middles of the headers (entities
	/// of the world whose scale is the header's size). It shows the game from <see cref="ShowAtOnce"/> or the first
	/// <see cref="Update"/> on. Throws when an element of the screen is missing from the canvas.</summary>
	public Hud(BlocksGame game, Entity canvas, Entity? holdHeader, Entity? nextHeader)
	{
		m_Game = game;
		Collect(canvas);
		m_HoldLabel = new BoardLabel(Find("Hold Label"), holdHeader);
		m_NextLabel = new BoardLabel(Find("Next Label"), nextHeader);
		m_Flash = Find("Flash").GetComponent<UIImageComponent>()!;
		m_FlashRect = Find("Flash").GetComponent<UIRectComponent>()!;
		m_Dim = new Screen(Find("Dim"), 0.0f, MenuDim);
		m_Score = new Counter(Find("Score Value"), true);
		m_Level = new Counter(Find("Level Value"), false);
		m_Lines = new Counter(Find("Lines Value"), false);
		m_Best = new Counter(Find("Best Value"), true);
		m_LevelBar = Find("Level Bar Fill").GetComponent<UIRectComponent>()!;
		m_DemoTag = Find("Demo Tag").GetComponent<UIRectComponent>()!;
		m_Title = new Screen(Find("Title Screen"), 0.0f);
		m_Sign = Find("Title Sign").GetComponent<UIRectComponent>()!;
		m_Pause = new Screen(Find("Pause Menu"), MenuRise);
		m_Over = new Screen(Find("Game Over"), MenuRise);
		foreach (Entity tile in Find("Logo").Children)
		{
			UIRectComponent rect = tile.GetComponent<UIRectComponent>()!;
			m_Tiles.Add((rect, rect.Position));
		}
		m_TitleMenu = new Menu([Find("Play Button"), Find("Quit Button")], game.PlayMenuTick);
		m_PauseMenu = new Menu([Find("Resume Button"), Find("Restart Button"), Find("Pause Quit Button")], game.PlayMenuTick);
		m_OverMenu = new Menu([Find("Play Again Button"), Find("Game Over Quit Button")], game.PlayMenuTick, GameOverKeyDelay,
			spaceClicks: false);
		m_FinalScore = Find("Final Score").GetComponent<UITextComponent>()!;
		m_FinalStats = Find("Final Stats").GetComponent<UITextComponent>()!;
		m_NewBest = Find("New Best Tag").GetComponent<UIRectComponent>()!;
		Callouts = new Callouts(Find);

		// The labels follow the board's panels as the camera shows them, and the title's sign the camera's glide: where the
		// camera is once it has moved this frame.
		m_Camera = game.Camera?.GetComponent<CameraComponent>();
		m_Shake = game.Camera?.As<CameraShake>();
		if (m_Shake is not null)
		{
			m_Shake.Moved += FollowCamera;
		}
	}

	/// <summary>The words that pop over the board.</summary>
	public Callouts Callouts { get; }

	/// <summary>The menu the title, the pause or the game over shows, if any.</summary>
	public Menu? OpenMenu => m_TitleMenu.IsOpen ? m_TitleMenu : m_PauseMenu.IsOpen ? m_PauseMenu : m_OverMenu.IsOpen ? m_OverMenu : null;

	/// <summary>A score as the screen shows it, with commas between the thousands in every language: the font has none of the
	/// spaces some languages group digits with.</summary>
	public static string FormatScore(int score) => score.ToString("N0", CultureInfo.InvariantCulture);

	/// <summary>A number as the screen shows it, the same in every language.</summary>
	public static string FormatNumber(int number) => number.ToString(CultureInfo.InvariantCulture);

	/// <summary>Stops following the camera.</summary>
	public void Dispose()
	{
		if (m_Shake is not null)
		{
			m_Shake.Moved -= FollowCamera;
		}
	}

	/// <summary>Lights up the view in a color for a moment; strength is the opacity it starts with.</summary>
	public void Flash(Color color, float strength)
	{
		m_Flash.Color = new Color(color.R, color.G, color.B);
		m_FlashStrength = Mathf.Clamp01(strength);
		m_FlashAge = 0.0f;
	}

	/// <summary>Shows the screen of the game's state at once, without fading it in (when the game starts).</summary>
	public void ShowAtOnce() => Refresh(0.0f, true);

	/// <summary>Runs the menu of the game's state for a frame, before the game's own: the mouse and the keys move its focus,
	/// and its clicks start, resume, restart or quit the game at once.</summary>
	public void UpdateMenus(float deltaTime)
	{
		OpenMenus();
		// Every menu animates; at most one is open to click.
		Entity? title = m_TitleMenu.Update(deltaTime);
		Entity? pause = m_PauseMenu.Update(deltaTime);
		Entity? over = m_OverMenu.Update(deltaTime);
		if ((title ?? pause ?? over) is { } clicked)
		{
			Click(clicked);
			OpenMenus();
		}
	}

	/// <summary>Shows the game as it is after its frame and moves the screen's animations on by the time step: the screen of
	/// the game's state, the numbers, flashes and callouts.</summary>
	public void Update(float deltaTime) => Refresh(deltaTime, false);

	private void Refresh(float deltaTime, bool snap)
	{
		OpenMenus();
		GameState state = m_Game.State;
		m_Title.Update(state == GameState.Title, deltaTime, snap);
		m_Pause.Update(state == GameState.Paused, deltaTime, snap);
		m_Over.Update(state == GameState.Over, deltaTime, snap);
		m_Dim.Update(state is GameState.Paused or GameState.Over, deltaTime, snap);

		ShowNumbers(deltaTime, snap);
		if (m_Title.Opacity > 0.0f)
		{
			float time = (float)Time.Elapsed;
			for (int i = 0; i < m_Tiles.Count; i++)
			{
				(UIRectComponent rect, Vector2 home) = m_Tiles[i];
				rect.Position = home + new Vector2(0.0f, TileBob * Mathf.Sin((time * TileBobSpeed) - (i * TileBobDelay)));
			}
		}
		if (state == GameState.Over && m_State != GameState.Over)
		{
			bool newBest = m_Game.IsNewBest;
			m_FinalScore.Text = FormatScore(m_Game.Score);
			m_FinalScore.Color = newBest ? s_Best : s_Value;
			m_FinalStats.Text = $"LEVEL {FormatNumber(m_Game.Level)}   ·   LINES {FormatNumber(m_Game.Rows)}";
			m_NewBest.Visible = newBest;
		}
		m_State = state;

		m_FlashAge += deltaTime;
		float flash = Mathf.Clamp01(1.0f - (m_FlashAge / FlashSeconds));
		float flashOpacity = m_FlashStrength * flash * flash;
		if (flashOpacity != m_FlashShown)
		{
			m_FlashShown = flashOpacity;
			m_FlashRect.Opacity = flashOpacity;
			m_FlashRect.Visible = flashOpacity > 0.0f;
		}
		Callouts.Hidden = state == GameState.Paused;
		Callouts.Update(deltaTime);
		if (m_Shake is null)
		{
			FollowCamera();
		}
	}

	// The menu of the game's state opens; the others close.
	private void OpenMenus()
	{
		m_TitleMenu.Open(m_Game.State == GameState.Title);
		m_PauseMenu.Open(m_Game.State == GameState.Paused);
		m_OverMenu.Open(m_Game.State == GameState.Over);
	}

	// What the menus' buttons do.
	private void Click(Entity button)
	{
		switch (button.Name)
		{
			case "Play Button":
			case "Play Again Button":
			case "Restart Button":
				m_Game.Restart();
				break;
			case "Resume Button":
				m_Game.SetPaused(false);
				break;
			case "Pause Quit Button":
			case "Game Over Quit Button":
				m_Game.ShowTitle();
				break;
			case "Quit Button":
				Application.Quit();
				break;
		}
	}

	// The score panel: the demo's numbers count on the title, but only games count for the best.
	private void ShowNumbers(float deltaTime, bool snap)
	{
		bool demo = m_Game.IsDemo;
		int score = m_Game.Score;
		m_Score.Show(score, deltaTime);
		m_Level.Show(m_Game.Level, deltaTime);
		m_Lines.Show(m_Game.Rows, deltaTime);
		m_Best.Show(demo ? m_Game.BestScore : System.Math.Max(m_Game.BestScore, score), deltaTime);
		// The bar fills with the rows of the level and empties when the next one starts.
		float progress = m_Game.LevelProgress;
		m_Bar = progress < m_Bar || snap ? progress : Mathf.Lerp(m_Bar, progress, 1.0f - Mathf.Exp(-BarResponse * deltaTime));
		if (Mathf.Abs(progress - m_Bar) < 0.001f)
		{
			m_Bar = progress;
		}
		if (m_Bar != m_BarShown)
		{
			m_BarShown = m_Bar;
			m_LevelBar.AnchorMax = new Vector2(m_Bar, 1.0f);
			m_LevelBar.Visible = m_Bar > 0.0f;
		}
		if (demo != m_DemoShown)
		{
			m_DemoShown = demo;
			m_DemoTag.Visible = demo;
		}
	}

	// What follows the camera: the labels over the board's panels, and the title's sign, which shows over the end of the
	// camera's glide to the attract view (at once without a CameraShake), once the panels have moved down out from under it.
	private void FollowCamera()
	{
		m_HoldLabel.Place(m_Camera);
		m_NextLabel.Place(m_Camera);
		float glide = m_Shake?.AttractWeight ?? 1.0f;
		float sign = Mathf.SmoothStep(0.0f, 1.0f, Mathf.InverseLerp(SignFromGlide, 1.0f, glide));
		if (sign != m_SignShown)
		{
			m_SignShown = sign;
			m_Sign.Opacity = sign;
		}
	}

	private void Collect(Entity parent)
	{
		foreach (Entity child in parent.Children)
		{
			if (!m_Elements.TryAdd(child.Name, child))
			{
				throw new System.InvalidOperationException($"The HUD's canvas has two elements named \"{child.Name}\"");
			}
			Collect(child);
		}
	}

	private Entity Find(string name) =>
		m_Elements.TryGetValue(name, out Entity? element)
			? element
			: throw new System.InvalidOperationException($"The HUD's canvas has no element named \"{name}\"");

	// An element that fades in to its opacity when its state comes and out when it goes, each in a fixed time and eased at
	// both ends (menus rise as they come); one faded out is hidden, so it takes no clicks.
	private sealed class Screen
	{
		private readonly UIRectComponent m_Rect;
		private readonly Vector2 m_Home;
		private readonly float m_Rise;
		private readonly float m_Opacity;
		// How far it has faded in, from 0 to 1, and whether the element shows that yet.
		private float m_Shown;
		private bool m_Applied;

		public Screen(Entity entity, float rise, float opacity = 1.0f)
		{
			m_Rect = entity.GetComponent<UIRectComponent>()!;
			m_Home = m_Rect.Position;
			m_Rise = rise;
			m_Opacity = opacity;
			m_Shown = m_Rect.Visible && m_Rect.Opacity > 0.0f ? 1.0f : 0.0f;
		}

		public float Opacity => m_Opacity * Mathf.SmoothStep(0.0f, 1.0f, m_Shown);

		public void Update(bool active, float deltaTime, bool snap)
		{
			float target = active ? 1.0f : 0.0f;
			float shown = snap ? target : Mathf.MoveTowards(m_Shown, target, deltaTime / (active ? FadeInSeconds : FadeOutSeconds));
			if (m_Applied && shown == m_Shown)
			{
				return;
			}
			m_Applied = true;
			m_Shown = shown;
			m_Rect.Opacity = Opacity;
			m_Rect.Visible = m_Shown > 0.0f;
			float away = 1.0f - Mathf.SmoothStep(0.0f, 1.0f, m_Shown);
			m_Rect.Position = m_Home + new Vector2(0.0f, m_Rise * away * away);
		}
	}

	// A number of the score panel that grows for a moment when it goes up.
	private sealed class Counter
	{
		private readonly UITextComponent m_Text;
		private readonly float m_Size;
		// Whether the number's thousands are grouped (scores).
		private readonly bool m_Grouped;
		private int? m_Value;
		private float m_Pop;

		public Counter(Entity entity, bool grouped)
		{
			m_Text = entity.GetComponent<UITextComponent>()!;
			m_Size = m_Text.FontSize;
			m_Grouped = grouped;
		}

		public void Show(int value, float deltaTime)
		{
			if (value != m_Value)
			{
				if (m_Value is { } before && value > before)
				{
					m_Pop = 1.0f;
				}
				m_Value = value;
				m_Text.Text = m_Grouped ? FormatScore(value) : FormatNumber(value);
			}
			if (m_Pop > 0.0f)
			{
				m_Pop = Mathf.Max(m_Pop - (deltaTime / PopSeconds), 0.0f);
				m_Text.FontSize = m_Size * (1.0f + (PopGrowth * m_Pop * m_Pop));
			}
		}
	}

	// HOLD or NEXT over a panel of the board: a label of the board's labels canvas (whose units are pixels) placed where the
	// camera shows the middle of the panel's header and sized by the header's height there, with its drop shadow.
	private sealed class BoardLabel
	{
		private readonly Entity? m_Header;
		private readonly TransformComponent? m_HeaderTransform;
		private readonly UIRectComponent m_Rect;
		private readonly UITextComponent m_Text;
		private readonly UITextComponent m_Shadow;
		private readonly UIRectComponent m_ShadowRect;
		// Where the label stands and how high it is in pixels (written when they change); a height of zero hides it.
		private Vector2 m_Center;
		private float m_Height = -1.0f;
		private float m_Width;

		public BoardLabel(Entity label, Entity? header)
		{
			m_Header = header;
			m_HeaderTransform = header?.Transform;
			m_Rect = label.GetComponent<UIRectComponent>()!;
			Entity? text = null;
			Entity? shadow = null;
			foreach (Entity child in label.Children)
			{
				if (child.Name == $"{label.Name} Text")
				{
					text = child;
				}
				else if (child.Name == $"{label.Name} Shadow")
				{
					shadow = child;
				}
			}
			m_Text = text?.GetComponent<UITextComponent>() ?? throw new System.InvalidOperationException($"{label.Name} has no text");
			m_Shadow = shadow?.GetComponent<UITextComponent>() ?? throw new System.InvalidOperationException($"{label.Name} has no shadow");
			m_ShadowRect = shadow.GetComponent<UIRectComponent>()!;
			Show(Vector2.Zero, 0.0f, 0.0f);
		}

		public void Place(CameraComponent? camera)
		{
			if (camera is null || m_Header is null || m_HeaderTransform is null)
			{
				Show(Vector2.Zero, 0.0f, 0.0f);
				return;
			}
			Vector3 middle = m_HeaderTransform.WorldTranslation;
			Vector3 scale = m_Header.Scale;
			Vector3 up = m_HeaderTransform.Up * (0.5f * scale.Y);
			Vector3 right = m_HeaderTransform.Right * (0.5f * scale.X);
			Vector3 center = camera.WorldToScreenPoint(middle);
			Vector3 top = camera.WorldToScreenPoint(middle + up);
			Vector3 bottom = camera.WorldToScreenPoint(middle - up);
			Vector3 left = camera.WorldToScreenPoint(middle - right);
			Vector3 rightEdge = camera.WorldToScreenPoint(middle + right);
			// A header behind the camera has no place on the screen.
			if (center.Z <= 0.0f || top.Z <= 0.0f || bottom.Z <= 0.0f || left.Z <= 0.0f || rightEdge.Z <= 0.0f)
			{
				Show(Vector2.Zero, 0.0f, 0.0f);
				return;
			}
			Show(Flat(center), Vector2.Distance(Flat(left), Flat(rightEdge)), Vector2.Distance(Flat(top), Flat(bottom)));
		}

		private static Vector2 Flat(Vector3 screen) => new(screen.X, screen.Y);

		private void Show(Vector2 center, float width, float height)
		{
			if (center == m_Center && width == m_Width && height == m_Height)
			{
				return;
			}
			m_Center = center;
			m_Width = width;
			m_Height = height;
			m_Rect.Visible = height > 0.0f;
			if (height <= 0.0f)
			{
				return;
			}
			m_Rect.Position = center;
			m_Rect.Size = new Vector2(width, height);
			m_Text.FontSize = height * LabelSize;
			m_Shadow.FontSize = height * LabelSize;
			m_ShadowRect.Position = new Vector2(height * LabelShadow, height * LabelShadow);
		}
	}
}
