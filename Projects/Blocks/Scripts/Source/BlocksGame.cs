using System.Globalization;
using System.IO;
using Strada;

namespace Blocks;

/// <summary>Where the game is.</summary>
public enum GameState
{
	/// <summary>The title screen, where the board plays itself (attract mode): its Play button starts a game, its Quit button
	/// ends the application.</summary>
	Title,
	Playing,
	/// <summary>Held: Escape or P goes on, and so does the pause menu's Resume; its Restart starts again and its Quit goes back
	/// to the title.</summary>
	Paused,
	/// <summary>The stack reached the top: Play Again (or R) plays again and Quit goes back to the title, as the game-over
	/// screen does by itself after a while.</summary>
	Over,
}

/// <summary>The game. Pieces fall into the board; the player moves them (Left/Right or A/D, held keys repeat), turns them
/// (Up/W/X clockwise, Z counter-clockwise), drops them faster (Down/S) or at once (Space), and holds one for later (C or
/// Shift, once per piece). Full rows clear: their blocks burst into debris and sparks, a callout names the clear, and the
/// score, lines and level rise; clears in a row add combo points and a four-row clear after another adds half again.
/// Escape or P pauses, R starts again. On the title screen the <see cref="AutoPlayer"/> plays a demo game. The best score
/// is kept between runs. The board's cells are drawn by blocks this script creates; settled cells take the color of their
/// piece. The screen (scores, menus, callouts) is the <see cref="Hud"/> on the scene's canvas.</summary>
public sealed class BlocksGame : Script
{
	[Tooltip("The camera, shaken by drops and clears (needs a CameraShake script)")]
	public Entity? Camera;

	[Tooltip("The canvas of the game's screen: scores, labels, menus and callouts (see Hud)")]
	public Entity? HudCanvas;

	[Tooltip("Where the HOLD label stands: the middle of the hold panel's header, scaled to the header's size")]
	public Entity? HoldHeader;

	[Tooltip("Where the NEXT label stands: the middle of the next panel's header, scaled to the header's size")]
	public Entity? NextHeader;

	[Tooltip("Seconds a piece takes to fall one row at level 1")]
	[Range(0.05f, 2.0f)]
	public float FallInterval = 0.8f;

	[Tooltip("Where the first piece of the next queue is shown (the center of its shape)")]
	public Vector3 NextPosition = new(13.8f, 16.6f, 0.0f);

	[Tooltip("How far apart the pieces of the next queue are")]
	public float NextSpacing = 3.4f;

	[Tooltip("Where the held piece is shown (the center of its shape)")]
	public Vector3 HoldPosition = new(-4.8f, 16.6f, 0.0f);

	[Tooltip("Starts on the title screen; otherwise the game starts at once (tests)")]
	public bool StartOnTitle = true;

	[Tooltip("On the title screen the board plays itself")]
	public bool AttractMode = true;

	[Tooltip("Seeds the order of the demo's pieces, so the title screen plays the same demo every time (0: a new demo each time)")]
	public int DemoSeed;

	[Tooltip("Seconds the game-over screen waits before the title comes back (0 waits for a key)")]
	[Range(0.0f, 120.0f)]
	public float TitleReturnDelay = 25.0f;

	[Tooltip("Keeps the best score in the user's data folder between runs")]
	public bool SaveBestScore = true;

	[Tooltip("The mesh of a block (a cube one unit wide)")]
	public Mesh? BlockMesh;

	[Tooltip("What a cleared block bursts into")]
	public Prefab? Debris;

	[Tooltip("Sparks along a cleared row (a particle system centered on the row)")]
	public Prefab? RowBurst;

	[Tooltip("Dust where a hard drop lands")]
	public Prefab? DropBurst;

	[Tooltip("A piece moves a column")]
	public AudioClip? MoveSound;

	[Tooltip("A piece turns")]
	public AudioClip? RotateSound;

	[Tooltip("A piece is put aside")]
	public AudioClip? HoldSound;

	[Tooltip("A soft drop moves the piece a row")]
	public AudioClip? SoftDropSound;

	[Tooltip("A piece settles")]
	public AudioClip? LockSound;

	[Tooltip("A hard drop lands")]
	public AudioClip? DropSound;

	[Tooltip("One row clears")]
	public AudioClip? SingleSound;

	[Tooltip("Two rows clear at once")]
	public AudioClip? DoubleSound;

	[Tooltip("Three rows clear at once")]
	public AudioClip? TripleSound;

	[Tooltip("Four rows clear at once")]
	public AudioClip? TetrisSound;

	[Tooltip("The level rises")]
	public AudioClip? LevelUpSound;

	[Tooltip("The game ends")]
	public AudioClip? GameOverSound;

	[Tooltip("A game starts")]
	public AudioClip? StartSound;

	[Tooltip("The game pauses or goes on")]
	public AudioClip? PauseSound;

	// How fast a held key moves the piece: the first repeat after the delay, then one per interval.
	private const float RepeatDelay = 0.17f;
	private const float RepeatInterval = 0.05f;
	// Seconds a landed piece may still slide before it settles; moving or turning it restarts the wait.
	private const float LockDelay = 0.5f;
	private const float SoftDropInterval = 0.04f;
	private const int RowsPerLevel = 10;
	private const int QueueLength = 3;
	// Sounds play on a few sources, so quick ones do not cut each other off: the ticks of moves, turns and soft drops take
	// turns on their own few, so a quick run of them never cuts off a clear's sound on the others.
	private const int TickSources = 3;
	private const int SoundSources = 9;
	// The demo on the title screen is quieter and shakes less than a game.
	private const float AttractVolume = 0.45f;
	private const float AttractShake = 0.6f;
	// The demo's pace: a look at the new piece, then turns, slides and a short pause before the drop (seconds, from-to).
	private const float ThinkMin = 0.3f;
	private const float ThinkMax = 0.55f;
	private const float TurnMin = 0.08f;
	private const float TurnMax = 0.13f;
	private const float SlideMin = 0.045f;
	private const float SlideMax = 0.075f;
	private const float AimMin = 0.08f;
	private const float AimMax = 0.22f;
	// After putting a piece aside, the demo looks at the piece it brought for this part of a look at a new piece.
	private const float HoldLook = 0.6f;
	// Plans the demo may remake for a piece (when a move is blocked) before it drops it where it is.
	private const int Replans = 3;

	private static readonly Color[] s_Colors =
	[
		new(0.1f, 0.85f, 0.95f), new(1.0f, 0.82f, 0.1f), new(0.7f, 0.25f, 1.0f), new(0.2f, 0.9f, 0.3f),
		new(1.0f, 0.2f, 0.25f), new(0.2f, 0.4f, 1.0f), new(1.0f, 0.5f, 0.1f),
	];

	private static readonly string[] s_ClearNames = ["SINGLE", "DOUBLE", "TRIPLE", "TETRIS!"];

	private static readonly Color[] s_ClearColors =
	[
		new(0.75f, 0.92f, 1.0f), new(0.45f, 1.0f, 0.6f), new(1.0f, 0.65f, 0.25f), new(1.0f, 0.82f, 0.25f),
	];

	private readonly Board m_Board = new();
	private readonly Entity[,] m_CellCubes = new Entity[Board.Width, Board.Height];
	private readonly PieceKind?[,] m_Shown = new PieceKind?[Board.Width, Board.Height];
	private readonly Entity[] m_PieceCubes = new Entity[4];
	private readonly Entity[] m_GhostCubes = new Entity[4];
	private readonly Entity[] m_HoldCubes = new Entity[4];
	private readonly Entity[,] m_QueueCubes = new Entity[QueueLength, 4];
	private readonly Material[] m_Materials = new Material[s_Colors.Length];
	private readonly AudioSourceComponent[] m_Sources = new AudioSourceComponent[SoundSources];
	private readonly AutoPlayer m_AutoPlayer = new();
	private PieceBag m_Bag = new(Random.Range);
	private Material? m_GhostMaterial;
	private Hud? m_Hud;
	private float m_FallTimer;
	private float m_LockTimer;
	private float m_RepeatTimer;
	private int m_RepeatDirection;
	private int m_NextTick;
	private int m_NextSound = TickSources;
	private bool m_HoldUsed;
	private bool m_NewBest;
	private bool m_LastClearWasTetris;
	private float m_OverTime;
	// The demo's plan for the active piece, the time to its next move, whether it thought about holding the piece, how often
	// the plan was remade and whether the piece is in place, waiting for its drop.
	private Placement? m_Plan;
	private float m_AutoTimer;
	private bool m_HoldConsidered;
	private int m_Replanned;
	private bool m_Aimed;

	/// <summary>The piece the player controls.</summary>
	public Piece Active { get; private set; }

	/// <summary>The piece after the active one.</summary>
	public PieceKind Next => m_Bag.Peek();

	/// <summary>The held piece, if any.</summary>
	public PieceKind? Held { get; private set; }

	public GameState State { get; private set; } = GameState.Title;

	public int Score { get; private set; }

	public int Rows { get; private set; }

	public int Level => 1 + (Rows / RowsPerLevel);

	/// <summary>Clears in a row: -1 after a piece that cleared nothing, 0 after one clear, 1 after two in a row, and so on.</summary>
	public int Combo { get; private set; } = -1;

	/// <summary>The highest score of every game so far (kept between runs with <see cref="SaveBestScore"/>); demos do not
	/// count.</summary>
	public int BestScore { get; private set; }

	/// <summary>Whether the game that ended made a new best score.</summary>
	public bool IsNewBest => m_NewBest;

	/// <summary>How far the rows have come toward the next level, from 0 to 1.</summary>
	public float LevelProgress => (Rows % RowsPerLevel) / (float)RowsPerLevel;

	public bool IsOver => State == GameState.Over;

	public bool IsPaused => State == GameState.Paused;

	/// <summary>Whether the board plays itself: the title screen's demo.</summary>
	public bool IsDemo => State == GameState.Title && AttractMode;

	/// <summary>Pieces the demo has settled since it started.</summary>
	public int DemoPieces { get; private set; }

	/// <summary>Plans the demo remade since it started because a move of its plan was blocked.</summary>
	public int DemoReplans { get; private set; }

	/// <summary>Pieces the demo has put aside since it started.</summary>
	public int DemoHolds { get; private set; }

	/// <summary>Where the demo is bringing the active piece; null while it looks at the piece.</summary>
	public Placement? DemoPlan => m_Plan;

	/// <summary>Blocks that burst from cleared rows so far.</summary>
	public int DebrisSpawned { get; private set; }

	/// <summary>Particle bursts started so far (cleared rows and hard drops).</summary>
	public int BurstsSpawned { get; private set; }

	/// <summary>Sounds started so far.</summary>
	public int SoundsPlayed { get; private set; }

	/// <summary>The sound started last.</summary>
	public AudioClip? LastSound { get; private set; }

	/// <summary>Seconds per row at the current level: falls get faster as the level rises (the demo keeps the first
	/// level's pace).</summary>
	public float CurrentFallInterval => IsDemo ? FallInterval : Mathf.Max(0.05f, FallInterval * Mathf.Pow(0.85f, Level - 1));

	/// <summary>The settled cells (tests set up situations on it).</summary>
	public Board Board => m_Board;

	private static string BestScoreFile =>
		Path.Combine(System.Environment.GetFolderPath(System.Environment.SpecialFolder.LocalApplicationData), "Strada", "Blocks", "Best.txt");

	// Whether pieces move: in a game, and in the title screen's demo.
	private bool IsRunning => State == GameState.Playing || IsDemo;

	private float SoundScale => IsDemo ? AttractVolume : 1.0f;

	protected override void OnCreate()
	{
		for (int kind = 0; kind < s_Colors.Length; kind++)
		{
			Material material = Material.Create()!;
			material.BaseColor = s_Colors[kind];
			material.Metallic = 0.1f;
			material.Roughness = 0.22f;
			material.Emissive = s_Colors[kind];
			material.EmissiveIntensity = 0.28f;
			m_Materials[kind] = material;
		}
		m_GhostMaterial = Material.Create()!;
		m_GhostMaterial.BaseColor = new Color(1.0f, 1.0f, 1.0f, 0.12f);
		m_GhostMaterial.Emissive = new Color(0.6f, 0.8f, 1.0f);
		m_GhostMaterial.EmissiveIntensity = 0.15f;
		m_GhostMaterial.AlphaMode = MaterialAlphaMode.Blend;

		Mesh mesh = BlockMesh ?? Assets.Load<Mesh>("builtin://Cube")!;
		for (int x = 0; x < Board.Width; x++)
		{
			for (int y = 0; y < Board.Height; y++)
			{
				m_CellCubes[x, y] = CreateBlock($"Cell {x},{y}", new Vector3(x, y, 0.0f), mesh, true);
			}
		}
		for (int i = 0; i < 4; i++)
		{
			m_PieceCubes[i] = CreateBlock("Piece", Vector3.Zero, mesh, true);
			// The ghost is see-through: it casts no shadow.
			m_GhostCubes[i] = CreateBlock("Ghost", Vector3.Zero, mesh, false);
			m_HoldCubes[i] = CreateBlock("Held", Vector3.Zero, mesh, true);
			for (int slot = 0; slot < QueueLength; slot++)
			{
				m_QueueCubes[slot, i] = CreateBlock($"Next {slot + 1}", Vector3.Zero, mesh, true);
			}
		}
		for (int i = 0; i < SoundSources; i++)
		{
			Entity source = Entity.Create($"Sound {i + 1}");
			source.Parent = this;
			m_Sources[i] = source.AddComponent<AudioSourceComponent>();
		}
		if (HudCanvas is not null)
		{
			m_Hud = new Hud(this, HudCanvas, HoldHeader, NextHeader);
		}
		else
		{
			Log.Warn("Blocks has no HUD canvas: no scores, menus or callouts show");
		}
		BestScore = LoadBestScore();
		if (StartOnTitle)
		{
			ShowTitle();
		}
		else
		{
			Restart();
		}
		// The first state shows at once: the camera stands where it shows the board from (back at the attract view on the
		// title screen, without gliding there from the game's place), and the screen shows its title or game.
		if (Camera?.As<CameraShake>() is { } camera)
		{
			camera.Attract = State == GameState.Title;
			camera.SkipGlide();
		}
		m_Hud?.ShowAtOnce();
	}

	protected override void OnDestroy()
	{
		m_Hud?.Dispose();
	}

	// The screen's menus take the mouse, the arrow keys, Enter and Space on the title, pause and game-over screens (see Hud):
	// they go first, so what they start or resume plays this frame. The game takes its own keys.
	protected override void OnUpdate(float deltaTime)
	{
		m_Hud?.UpdateMenus(deltaTime);
		bool restart = Input.IsKeyPressed(KeyCode.R);
		bool pause = Input.IsKeyPressed(KeyCode.Escape) || Input.IsKeyPressed(KeyCode.P);
		switch (State)
		{
			case GameState.Title:
				if (restart)
				{
					Restart();
				}
				else if (IsDemo)
				{
					PlayDemo(deltaTime);
					Fall(deltaTime, false);
				}
				break;
			case GameState.Over:
				m_OverTime += deltaTime;
				if (restart)
				{
					Restart();
				}
				else if (TitleReturnDelay > 0.0f && m_OverTime >= TitleReturnDelay)
				{
					ShowTitle();
				}
				break;
			case GameState.Paused:
				if (pause)
				{
					SetPaused(false);
				}
				else if (restart)
				{
					Restart();
				}
				break;
			case GameState.Playing:
				if (restart)
				{
					Restart();
				}
				else if (pause)
				{
					SetPaused(true);
				}
				else
				{
					HandleInput(deltaTime);
					Fall(deltaTime, Input.IsKeyDown(KeyCode.Down) || Input.IsKeyDown(KeyCode.S));
				}
				break;
		}
		Show(deltaTime);
	}

	/// <summary>Starts a new game: an empty board, a new bag, no held piece and a new score.</summary>
	public void Restart()
	{
		ResetBoard(new PieceBag(Random.Range));
		m_NewBest = false;
		State = GameState.Playing;
		m_Hud?.Callouts.Clear();
		m_Hud?.Callouts.Show(CalloutSlot.Level, "GO!", new Color(0.4f, 1.0f, 0.6f), "", 96.0f, 0.9f);
		PlaySound(StartSound, 1.0f, 1.0f);
		SpawnNext();
	}

	/// <summary>Goes to the title screen, where the board plays a demo game by itself when <see cref="AttractMode"/> is on.</summary>
	public void ShowTitle()
	{
		ResetBoard(DemoSeed != 0 ? new PieceBag(SeededRange(DemoSeed)) : new PieceBag(Random.Range));
		State = GameState.Title;
		DemoPieces = 0;
		DemoReplans = 0;
		DemoHolds = 0;
		m_Hud?.Callouts.Clear();
		SpawnNext();
	}

	/// <summary>Pauses or resumes a running game.</summary>
	public void SetPaused(bool paused)
	{
		if ((State is GameState.Playing or GameState.Paused) && paused != IsPaused)
		{
			State = paused ? GameState.Paused : GameState.Playing;
			PlaySound(PauseSound, 0.5f, paused ? 0.84f : 1.0f);
		}
	}

	/// <summary>Moves the piece sideways by a column when it fits. True when it moved.</summary>
	public bool Shift(int direction)
	{
		Piece moved = Active.Moved(direction, 0);
		if (!IsRunning || !m_Board.Fits(moved))
		{
			return false;
		}
		Active = moved;
		m_LockTimer = 0.0f;
		PlaySound(MoveSound, 0.32f, Random.Range(0.96f, 1.04f), true);
		return true;
	}

	/// <summary>Turns the piece by quarter turns (positive clockwise), kicking it off walls when needed. True when it turned.</summary>
	public bool Turn(int quarterTurns)
	{
		if (!IsRunning || m_Board.Turn(Active, quarterTurns) is not { } turned)
		{
			return false;
		}
		Active = turned;
		m_LockTimer = 0.0f;
		PlaySound(RotateSound, 0.42f, quarterTurns > 0 ? 1.0f : 0.92f, true);
		return true;
	}

	/// <summary>Puts the active piece aside and brings back the held one (or the next): once per piece. True when it held.</summary>
	public bool Hold()
	{
		if (!IsRunning || m_HoldUsed)
		{
			return false;
		}
		PieceKind current = Active.Kind;
		Active = Board.Spawn(Held ?? m_Bag.Next());
		Held = current;
		m_HoldUsed = true;
		m_FallTimer = 0.0f;
		m_LockTimer = 0.0f;
		m_Plan = null;
		PlaySound(HoldSound, 0.6f, 1.0f);
		if (!m_Board.Fits(Active))
		{
			EndGame();
		}
		return true;
	}

	/// <summary>Moves the piece down a row, as holding Down does, for a point. True when it moved.</summary>
	public bool SoftDrop()
	{
		Piece lower = Active.Moved(0, -1);
		if (!IsRunning || !m_Board.Fits(lower))
		{
			return false;
		}
		Active = lower;
		Score += 1;
		PlaySound(SoftDropSound, 0.22f, 1.0f, true);
		return true;
	}

	/// <summary>Drops the piece as far as it goes and settles it; scores two points per row it fell.</summary>
	public void HardDrop()
	{
		if (!IsRunning)
		{
			return;
		}
		Piece landed = m_Board.Drop(Active);
		int fallen = Active.Y - landed.Y;
		Score += 2 * fallen;
		Active = landed;
		float weight = Mathf.Clamp01(fallen / 18.0f);
		PlaySound(DropSound, 0.7f + (0.3f * weight), Random.Range(0.97f, 1.03f));
		Kick(0.09f + (0.13f * weight));
		Shake(0.08f + (0.1f * weight));
		SpawnDropBurst(landed);
		Settle(true);
	}

	/// <summary>Clears the full rows: their blocks burst into debris and sparks, a callout and a sound name the clear, the
	/// camera shakes, and the score (with combo and back-to-back bonuses) and level rise. Returns the rows cleared.</summary>
	public int ClearFullRows()
	{
		int levelBefore = Level;
		int cleared = 0;
		float top = 0.0f;
		for (int y = 0; y < Board.Height + Board.HiddenRows; y++)
		{
			if (!m_Board.IsFull(y))
			{
				continue;
			}
			cleared++;
			top = y;
			for (int x = 0; x < Board.Width; x++)
			{
				Burst(new Vector3(x, y, 0.0f), m_Materials[(int)m_Board[x, y]!.Value]);
			}
			SpawnBurst(RowBurst, new Vector3((Board.Width - 1) * 0.5f, y, 0.6f));
		}
		if (cleared == 0)
		{
			return 0;
		}
		m_Board.ClearRows();
		int lines = System.Math.Min(cleared, 4);
		bool backToBack = lines == 4 && m_LastClearWasTetris;
		m_LastClearWasTetris = lines == 4;
		Combo++;
		int points = Board.Score(lines, Level);
		if (backToBack)
		{
			points += points / 2;
		}
		points += 50 * Combo * Level;
		Score += points;
		Rows += cleared;

		// Each clear sounds its own; combos raise it a semitone per clear in a row.
		AudioClip? sound = lines switch
		{
			1 => SingleSound,
			2 => DoubleSound,
			3 => TripleSound,
			_ => TetrisSound,
		};
		PlaySound(sound, 0.9f, Mathf.Pow(2.0f, System.Math.Min(Combo, 7) / 12.0f));
		ShowClear(lines, points, backToBack);
		Shake(lines switch
		{
			1 => 0.14f,
			2 => 0.24f,
			3 => 0.34f,
			_ => 0.6f,
		});
		Kick(0.05f * lines);
		if (Level > levelBefore)
		{
			PlaySound(LevelUpSound, 0.85f, 1.0f);
			m_Hud?.Callouts.Show(CalloutSlot.Level, "LEVEL UP!", new Color(0.35f, 1.0f, 0.95f), $"LEVEL {Hud.FormatNumber(Level)}", 70.0f, 1.8f);
			m_Hud?.Flash(new Color(0.3f, 0.9f, 1.0f), 0.12f * (IsDemo ? AttractShake : 1.0f));
		}
		// Sparks rise from the top cleared row a moment later in a big clear.
		if (lines == 4)
		{
			SpawnBurst(RowBurst, new Vector3((Board.Width - 1) * 0.5f, top + 1.0f, 0.8f));
		}
		return cleared;
	}

	/// <summary>The text of the callout showing in a slot over the board, or null when none shows.</summary>
	public string? Callout(CalloutSlot slot) => m_Hud?.Callouts.Showing(slot);

	/// <summary>The screen, or null when the game has no <see cref="HudCanvas"/>.</summary>
	public Hud? Hud => m_Hud;

	/// <summary>Ticks when a menu's selection moves: at the menus' own volume, which the title's quieter demo leaves
	/// alone.</summary>
	public void PlayMenuTick()
	{
		Play(MoveSound, 0.4f, 1.3f, true);
	}

	private void ShowClear(int lines, int points, bool backToBack)
	{
		if (m_Hud is null)
		{
			return;
		}
		bool tetris = lines == 4;
		m_Hud.Callouts.Show(CalloutSlot.Clear, s_ClearNames[lines - 1], s_ClearColors[lines - 1], $"+{Hud.FormatScore(points)}",
							tetris ? 114.0f : 84.0f, tetris ? 1.9f : 1.35f, tetris);
		if (backToBack)
		{
			m_Hud.Callouts.Show(CalloutSlot.Bonus, "BACK-TO-BACK", new Color(1.0f, 0.45f, 0.85f), "", 46.0f, 1.9f);
		}
		else if (Combo > 0)
		{
			m_Hud.Callouts.Show(CalloutSlot.Bonus, $"COMBO x{Hud.FormatNumber(Combo + 1)}", new Color(0.55f, 0.8f, 1.0f), "", 46.0f, 1.4f);
		}
		if (tetris)
		{
			m_Hud.Flash(new Color(1.0f, 0.9f, 0.6f), 0.22f * (IsDemo ? AttractShake : 1.0f));
		}
	}

	// A random whole number in [min, max) from a sequence the seed fixes.
	private static System.Func<int, int, int> SeededRange(int seed)
	{
		System.Random random = new(seed);
		return random.Next;
	}

	private void ResetBoard(PieceBag bag)
	{
		m_Board.Clear();
		m_Bag = bag;
		Score = 0;
		Rows = 0;
		Combo = -1;
		Held = null;
		m_LastClearWasTetris = false;
		m_RepeatDirection = 0;
		m_OverTime = 0.0f;
	}

	private Entity CreateBlock(string name, Vector3 position, Mesh mesh, bool castShadows)
	{
		Entity block = Entity.Create(name);
		// Children of the game: the board follows the game entity's transform.
		block.Parent = this;
		block.Translation = position;
		block.Scale = new Vector3(0.96f);
		MeshComponent component = block.AddComponent<MeshComponent>();
		component.Mesh = mesh;
		component.CastShadows = castShadows;
		component.Visible = false;
		return block;
	}

	private void HandleInput(float deltaTime)
	{
		if (Input.IsKeyPressed(KeyCode.C) || Input.IsKeyPressed(KeyCode.LeftShift) || Input.IsKeyPressed(KeyCode.RightShift))
		{
			Hold();
		}
		if (Input.IsKeyPressed(KeyCode.Up) || Input.IsKeyPressed(KeyCode.W) || Input.IsKeyPressed(KeyCode.X))
		{
			Turn(1);
		}
		if (Input.IsKeyPressed(KeyCode.Z))
		{
			Turn(-1);
		}
		if (Input.IsKeyPressed(KeyCode.Space))
		{
			HardDrop();
			return;
		}

		bool left = Input.IsKeyDown(KeyCode.Left) || Input.IsKeyDown(KeyCode.A);
		bool right = Input.IsKeyDown(KeyCode.Right) || Input.IsKeyDown(KeyCode.D);
		int direction = left == right ? 0 : (left ? -1 : 1);
		if (direction != m_RepeatDirection)
		{
			// A new direction moves at once, then repeats after the delay.
			m_RepeatDirection = direction;
			m_RepeatTimer = RepeatDelay;
			if (direction != 0)
			{
				Shift(direction);
			}
		}
		else if (direction != 0)
		{
			m_RepeatTimer -= deltaTime;
			while (m_RepeatTimer <= 0.0f)
			{
				m_RepeatTimer += RepeatInterval;
				Shift(direction);
			}
		}
	}

	// The demo's moves, one at a time at a person's pace: a look at the new piece (which it may put aside for a better one),
	// then turns to the planned rotation, slides to the planned column, a short aim and the drop. Each frame searches once at
	// most.
	private void PlayDemo(float deltaTime)
	{
		m_AutoTimer -= deltaTime;
		if (m_AutoTimer > 0.0f)
		{
			return;
		}
		if (m_Plan is not { } plan)
		{
			if (!m_HoldUsed && !m_HoldConsidered)
			{
				// It has looked at the new piece: would the held one (or the next) do clearly better? The plan follows on the
				// next frame.
				m_HoldConsidered = true;
				PieceKind instead = Held ?? Next;
				PieceKind insteadNext = Held is null ? m_Bag.Peek(1) : Next;
				if (m_AutoPlayer.ShouldHold(m_Board, Active, Next, instead, insteadNext))
				{
					DemoHolds++;
					Hold();
					// A look at the piece it brought.
					m_AutoTimer = Random.Range(ThinkMin, ThinkMax) * HoldLook;
				}
				return;
			}
			// Where does it go?
			m_Plan = m_AutoPlayer.Choose(m_Board, Active, Next);
			if (m_Plan is null)
			{
				HardDrop();
			}
			return;
		}
		int turns = (((plan.Landed.Rotation - Active.Rotation) % 4) + 4) % 4;
		if (turns != 0)
		{
			m_AutoTimer = Random.Range(TurnMin, TurnMax);
			if (!Turn(turns == 3 ? -1 : 1))
			{
				Replan();
			}
			return;
		}
		if (Active.X != plan.Landed.X)
		{
			m_AutoTimer = Random.Range(SlideMin, SlideMax);
			if (!Shift(System.Math.Sign(plan.Landed.X - Active.X)))
			{
				Replan();
			}
			return;
		}
		if (!m_Aimed)
		{
			// In place: a short look, then down it goes.
			m_Aimed = true;
			m_AutoTimer = Random.Range(AimMin, AimMax);
			return;
		}
		HardDrop();
	}

	// A move of the plan was blocked (the piece fell meanwhile): plan again from where the piece is, a few times at most.
	private void Replan()
	{
		m_Replanned++;
		DemoReplans++;
		m_Plan = m_Replanned <= Replans ? m_AutoPlayer.Choose(m_Board, Active, Next) : null;
		if (m_Plan is null)
		{
			HardDrop();
		}
	}

	private void Fall(float deltaTime, bool softDrop)
	{
		if (!IsRunning)
		{
			return;
		}
		float interval = softDrop ? Mathf.Min(SoftDropInterval, CurrentFallInterval) : CurrentFallInterval;
		if (!m_Board.Fits(Active.Moved(0, -1)))
		{
			m_FallTimer = 0.0f;
			m_LockTimer += deltaTime;
			if (m_LockTimer >= LockDelay)
			{
				Settle(false);
			}
			return;
		}
		m_FallTimer += deltaTime;
		while (m_FallTimer >= interval && m_Board.Fits(Active.Moved(0, -1)))
		{
			m_FallTimer -= interval;
			if (softDrop)
			{
				SoftDrop();
			}
			else
			{
				Active = Active.Moved(0, -1);
			}
		}
	}

	private void Settle(bool hardDropped)
	{
		if (!m_Board.Lock(Active))
		{
			EndGame();
			return;
		}
		if (!hardDropped)
		{
			PlaySound(LockSound, 0.55f, Random.Range(0.95f, 1.05f));
		}
		if (IsDemo)
		{
			DemoPieces++;
		}
		if (ClearFullRows() == 0)
		{
			Combo = -1;
		}
		SpawnNext();
	}

	private void SpawnNext()
	{
		Active = Board.Spawn(m_Bag.Next());
		m_FallTimer = 0.0f;
		m_LockTimer = 0.0f;
		m_HoldUsed = false;
		// The demo looks at a new piece for a moment before it moves.
		m_Plan = null;
		m_HoldConsidered = false;
		m_Replanned = 0;
		m_Aimed = false;
		m_AutoTimer = Random.Range(ThinkMin, ThinkMax);
		// The stack reached the top: the new piece has no room.
		if (!m_Board.Fits(Active))
		{
			EndGame();
		}
	}

	private void EndGame()
	{
		if (IsDemo)
		{
			// The demo topped out: it starts over.
			ShowTitle();
			return;
		}
		if (State != GameState.Over)
		{
			PlaySound(GameOverSound, 0.9f, 1.0f);
			Shake(0.6f);
			m_Hud?.Flash(new Color(1.0f, 0.25f, 0.2f), 0.25f);
			// The game-over screen shows the score; what the last clear called out is over.
			m_Hud?.Callouts.Clear();
			if (Score > BestScore)
			{
				BestScore = Score;
				m_NewBest = true;
				SaveBest();
			}
		}
		State = GameState.Over;
		m_OverTime = 0.0f;
	}

	private int LoadBestScore()
	{
		if (!SaveBestScore)
		{
			return 0;
		}
		try
		{
			if (!File.Exists(BestScoreFile))
			{
				return 0;
			}
			// The file holds the score's digits, whatever the player's language.
			string text = File.ReadAllText(BestScoreFile).Trim();
			return int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out int best) ? best : 0;
		}
		catch (System.Exception exception) when (exception is IOException or System.UnauthorizedAccessException)
		{
			Log.Warn($"The best score cannot be read: {exception.Message}");
			return 0;
		}
	}

	private void SaveBest()
	{
		if (!SaveBestScore)
		{
			return;
		}
		try
		{
			Directory.CreateDirectory(Path.GetDirectoryName(BestScoreFile)!);
			File.WriteAllText(BestScoreFile, BestScore.ToString(CultureInfo.InvariantCulture));
		}
		catch (System.Exception exception) when (exception is IOException or System.UnauthorizedAccessException)
		{
			Log.Warn($"The best score cannot be saved: {exception.Message}");
		}
	}

	// A cleared block flies off: it spins away from the board's center and falls.
	private void Burst(Vector3 cell, Material material)
	{
		if (Debris is null)
		{
			return;
		}
		Vector3 position = Transform.WorldTranslation + cell;
		Entity? piece = Entity.Instantiate(Debris, position, Quaternion.Identity);
		if (piece is null)
		{
			return;
		}
		piece.GetComponent<MeshComponent>()?.SetMaterial(0, material);
		RigidBodyComponent? body = piece.GetComponent<RigidBodyComponent>();
		if (body is not null)
		{
			float outward = (cell.X - ((Board.Width - 1) / 2.0f)) * 1.6f;
			body.LinearVelocity = new Vector3(outward + Random.Range(-2.5f, 2.5f), Random.Range(4.0f, 11.0f), Random.Range(0.5f, 4.0f));
			body.AngularVelocity = new Vector3(Random.Range(-12.0f, 12.0f), Random.Range(-12.0f, 12.0f), Random.Range(-12.0f, 12.0f));
		}
		DebrisSpawned++;
	}

	// Dust where the piece lands: under its lowest cells.
	private void SpawnDropBurst(Piece landed)
	{
		float left = float.MaxValue;
		float right = float.MinValue;
		float bottom = float.MaxValue;
		foreach ((int x, int y) in landed.Cells)
		{
			left = Mathf.Min(left, x);
			right = Mathf.Max(right, x);
			bottom = Mathf.Min(bottom, y);
		}
		SpawnBurst(DropBurst, new Vector3((left + right) * 0.5f, bottom - 0.45f, 0.4f));
	}

	private void SpawnBurst(Prefab? prefab, Vector3 cell)
	{
		if (prefab is not null && Entity.Instantiate(prefab, Transform.WorldTranslation + cell, Quaternion.Identity) is not null)
		{
			BurstsSpawned++;
		}
	}

	// A sound of the game: the title's demo plays them quieter.
	private void PlaySound(AudioClip? clip, float volume, float pitch, bool tick = false)
	{
		Play(clip, volume * SoundScale, pitch, tick);
	}

	// Ticks take turns on their sources; other sounds take a quiet source, or the one after the last when all are busy.
	private void Play(AudioClip? clip, float volume, float pitch, bool tick)
	{
		if (clip is null || m_Sources[0] is null)
		{
			return;
		}
		AudioSourceComponent source;
		if (tick)
		{
			source = m_Sources[m_NextTick];
			m_NextTick = (m_NextTick + 1) % TickSources;
		}
		else
		{
			int index = m_NextSound;
			for (int i = TickSources; i < SoundSources; i++)
			{
				if (!m_Sources[i].IsPlaying)
				{
					index = i;
					break;
				}
			}
			source = m_Sources[index];
			m_NextSound = index + 1 < SoundSources ? index + 1 : TickSources;
		}
		source.Stop();
		source.Clip = clip;
		source.Volume = volume;
		source.Pitch = pitch;
		source.Play();
		SoundsPlayed++;
		LastSound = clip;
	}

	private void Shake(float trauma)
	{
		Camera?.As<CameraShake>()?.Shake(trauma * (IsDemo ? AttractShake : 1.0f));
	}

	// The scene jolts down by the distance (in units) and springs back.
	private void Kick(float distance)
	{
		Camera?.As<CameraShake>()?.Kick(new Vector3(0.0f, -distance * (IsDemo ? AttractShake : 1.0f), 0.0f));
	}

	private void Show(float deltaTime)
	{
		for (int x = 0; x < Board.Width; x++)
		{
			for (int y = 0; y < Board.Height; y++)
			{
				PieceKind? kind = m_Board[x, y];
				if (kind == m_Shown[x, y])
				{
					continue;
				}
				m_Shown[x, y] = kind;
				MeshComponent mesh = m_CellCubes[x, y].GetComponent<MeshComponent>()!;
				mesh.Visible = kind is not null;
				if (kind is { } cellKind)
				{
					mesh.SetMaterial(0, m_Materials[(int)cellKind]);
				}
			}
		}

		bool inGame = State is GameState.Playing or GameState.Paused || IsDemo;
		ShowPiece(m_PieceCubes, Active, m_Materials[(int)Active.Kind], inGame);
		ShowPiece(m_GhostCubes, m_Board.Drop(Active), m_GhostMaterial!, State == GameState.Playing || IsDemo);
		ShowShape(m_HoldCubes, Held, HoldPosition, Quaternion.Identity, 1.0f);
		// The next pieces sway gently; the ones further back are smaller.
		Quaternion spin = Quaternion.AngleAxis(25.0f * Mathf.Sin((float)Time.Elapsed * 1.5f), Vector3.Up);
		for (int slot = 0; slot < QueueLength; slot++)
		{
			Entity[] cubes = [m_QueueCubes[slot, 0], m_QueueCubes[slot, 1], m_QueueCubes[slot, 2], m_QueueCubes[slot, 3]];
			Vector3 position = NextPosition - new Vector3(0.0f, slot * NextSpacing, 0.0f);
			ShowShape(cubes, m_Bag.Peek(slot), position, spin, slot == 0 ? 1.0f : 0.8f);
		}

		// The title screen's camera stands back to show the title over the board and sways slowly; it glides in when a game
		// starts.
		if (Camera?.As<CameraShake>() is { } camera)
		{
			camera.Attract = State == GameState.Title;
		}

		m_Hud?.Update(deltaTime);
	}

	// A piece's shape around a point, turned and scaled; none hides the blocks.
	private void ShowShape(Entity[] cubes, PieceKind? kind, Vector3 center, Quaternion rotation, float scale)
	{
		if (kind is not { } shapeKind)
		{
			foreach (Entity cube in cubes)
			{
				cube.GetComponent<MeshComponent>()!.Visible = false;
			}
			return;
		}
		Piece shape = new(shapeKind, 0, 0, 0);
		Vector3 middle = Vector3.Zero;
		foreach ((int x, int y) in shape.Cells)
		{
			middle += new Vector3(x, y, 0.0f) * 0.25f;
		}
		int i = 0;
		foreach ((int x, int y) in shape.Cells)
		{
			Entity cube = cubes[i++];
			cube.Translation = center + (rotation * ((new Vector3(x, y, 0.0f) - middle) * scale));
			cube.Rotation = rotation;
			cube.Scale = new Vector3(0.96f * scale);
			MeshComponent mesh = cube.GetComponent<MeshComponent>()!;
			mesh.Visible = true;
			mesh.SetMaterial(0, m_Materials[(int)shapeKind]);
		}
	}

	private static void ShowPiece(Entity[] cubes, Piece piece, Material material, bool visible)
	{
		int i = 0;
		foreach ((int x, int y) in piece.Cells)
		{
			Entity cube = cubes[i++];
			cube.Translation = new Vector3(x, y, 0.0f);
			MeshComponent mesh = cube.GetComponent<MeshComponent>()!;
			// Hidden rows are above the board's top.
			mesh.Visible = visible && y < Board.Height;
			mesh.SetMaterial(0, material);
		}
	}
}
