using System.IO;
using Strada;

namespace Blocks;

/// <summary>Where the game is.</summary>
public enum GameState
{
	/// <summary>The title screen: Enter starts.</summary>
	Title,
	Playing,
	/// <summary>Held: Escape, P or Enter goes on.</summary>
	Paused,
	/// <summary>The stack reached the top: Enter plays again.</summary>
	Over,
}

/// <summary>The game. Pieces fall into the board; the player moves them (Left/Right or A/D, held keys repeat), turns them
/// (Up/W/X clockwise, Z counter-clockwise), drops them faster (Down/S) or at once (Space), and holds one for later (C or
/// Shift, once per piece). Full rows clear: their blocks burst into debris and sparks, and the score, lines and level rise.
/// Escape or P pauses, R starts again. The best score is kept between runs. The board's cells are drawn by blocks this
/// script creates; settled cells take the color of their piece.</summary>
public sealed class BlocksGame : Script
{
	[Tooltip("The camera, shaken by drops and clears (needs a CameraShake script)")]
	public Entity? Camera;

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

	public AudioClip? MoveSound;
	public AudioClip? RotateSound;
	public AudioClip? LockSound;
	public AudioClip? DropSound;
	public AudioClip? ClearSound;

	[Tooltip("Plays instead of ClearSound when four rows clear at once")]
	public AudioClip? BigClearSound;

	public AudioClip? LevelUpSound;
	public AudioClip? GameOverSound;

	// How fast a held key moves the piece: the first repeat after the delay, then one per interval.
	private const float RepeatDelay = 0.17f;
	private const float RepeatInterval = 0.05f;
	// Seconds a landed piece may still slide before it settles; moving or turning it restarts the wait.
	private const float LockDelay = 0.5f;
	private const float SoftDropInterval = 0.04f;
	private const int RowsPerLevel = 10;
	private const int QueueLength = 3;
	// Sounds play on a few sources in turn, so quick ones do not cut each other off.
	private const int SoundSources = 6;

	private static readonly Color[] s_Colors =
	[
		new(0.1f, 0.85f, 0.95f), new(1.0f, 0.82f, 0.1f), new(0.7f, 0.25f, 1.0f), new(0.2f, 0.9f, 0.3f),
		new(1.0f, 0.2f, 0.25f), new(0.2f, 0.4f, 1.0f), new(1.0f, 0.5f, 0.1f),
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
	private PieceBag m_Bag = new(Random.Range);
	private Material? m_GhostMaterial;
	private Hud? m_Hud;
	private float m_FallTimer;
	private float m_LockTimer;
	private float m_RepeatTimer;
	private int m_RepeatDirection;
	private int m_NextSource;
	private bool m_HoldUsed;
	private bool m_NewBest;

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

	/// <summary>The highest score of every game so far (kept between runs with <see cref="SaveBestScore"/>).</summary>
	public int BestScore { get; private set; }

	public bool IsOver => State == GameState.Over;

	public bool IsPaused => State == GameState.Paused;

	/// <summary>Blocks that burst from cleared rows so far.</summary>
	public int DebrisSpawned { get; private set; }

	/// <summary>Particle bursts started so far (cleared rows and hard drops).</summary>
	public int BurstsSpawned { get; private set; }

	/// <summary>Sounds started so far.</summary>
	public int SoundsPlayed { get; private set; }

	/// <summary>Seconds per row at the current level: falls get faster as the level rises.</summary>
	public float CurrentFallInterval => Mathf.Max(0.05f, FallInterval * Mathf.Pow(0.85f, Level - 1));

	/// <summary>The settled cells (tests set up situations on it).</summary>
	public Board Board => m_Board;

	private static string BestScoreFile =>
		Path.Combine(System.Environment.GetFolderPath(System.Environment.SpecialFolder.LocalApplicationData), "Strada", "Blocks", "Best.txt");

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
				m_CellCubes[x, y] = CreateBlock($"Cell {x},{y}", new Vector3(x, y, 0.0f), mesh);
			}
		}
		for (int i = 0; i < 4; i++)
		{
			m_PieceCubes[i] = CreateBlock("Piece", Vector3.Zero, mesh);
			m_GhostCubes[i] = CreateBlock("Ghost", Vector3.Zero, mesh);
			m_HoldCubes[i] = CreateBlock("Held", Vector3.Zero, mesh);
			for (int slot = 0; slot < QueueLength; slot++)
			{
				m_QueueCubes[slot, i] = CreateBlock($"Next {slot + 1}", Vector3.Zero, mesh);
			}
		}
		for (int i = 0; i < SoundSources; i++)
		{
			Entity source = Entity.Create($"Sound {i + 1}");
			source.Parent = this;
			m_Sources[i] = source.AddComponent<AudioSourceComponent>();
		}
		m_Hud = new Hud(this);
		BestScore = LoadBestScore();
		if (StartOnTitle)
		{
			State = GameState.Title;
			Active = Board.Spawn(Next);
		}
		else
		{
			Restart();
		}
	}

	protected override void OnUpdate(float deltaTime)
	{
		bool confirm = Input.IsKeyPressed(KeyCode.Enter) || Input.IsKeyPressed(KeyCode.KPEnter);
		bool pause = Input.IsKeyPressed(KeyCode.Escape) || Input.IsKeyPressed(KeyCode.P);
		switch (State)
		{
			case GameState.Title:
			case GameState.Over:
				if (confirm || Input.IsKeyPressed(KeyCode.R))
				{
					Restart();
				}
				break;
			case GameState.Paused:
				if (pause || confirm)
				{
					State = GameState.Playing;
				}
				break;
			case GameState.Playing:
				if (Input.IsKeyPressed(KeyCode.R))
				{
					Restart();
				}
				else if (pause)
				{
					State = GameState.Paused;
				}
				else
				{
					HandleInput(deltaTime);
					Fall(deltaTime);
				}
				break;
		}
		Show();
	}

	/// <summary>Starts a new game: an empty board, a new bag, no held piece and a new score.</summary>
	public void Restart()
	{
		m_Board.Clear();
		m_Bag = new PieceBag(Random.Range);
		Score = 0;
		Rows = 0;
		Held = null;
		m_NewBest = false;
		State = GameState.Playing;
		SpawnNext();
	}

	/// <summary>Pauses or resumes a running game.</summary>
	public void SetPaused(bool paused)
	{
		if (State is GameState.Playing or GameState.Paused)
		{
			State = paused ? GameState.Paused : GameState.Playing;
		}
	}

	/// <summary>Moves the piece sideways by a column when it fits. True when it moved.</summary>
	public bool Shift(int direction)
	{
		Piece moved = Active.Moved(direction, 0);
		if (State != GameState.Playing || !m_Board.Fits(moved))
		{
			return false;
		}
		Active = moved;
		m_LockTimer = 0.0f;
		PlaySound(MoveSound, 0.35f, Random.Range(0.95f, 1.05f));
		return true;
	}

	/// <summary>Turns the piece by quarter turns (positive clockwise), kicking it off walls when needed. True when it turned.</summary>
	public bool Turn(int quarterTurns)
	{
		if (State != GameState.Playing || m_Board.Turn(Active, quarterTurns) is not { } turned)
		{
			return false;
		}
		Active = turned;
		m_LockTimer = 0.0f;
		PlaySound(RotateSound, 0.45f, quarterTurns > 0 ? 1.0f : 0.9f);
		return true;
	}

	/// <summary>Puts the active piece aside and brings back the held one (or the next): once per piece. True when it held.</summary>
	public bool Hold()
	{
		if (State != GameState.Playing || m_HoldUsed)
		{
			return false;
		}
		PieceKind current = Active.Kind;
		Active = Board.Spawn(Held ?? m_Bag.Next());
		Held = current;
		m_HoldUsed = true;
		m_FallTimer = 0.0f;
		m_LockTimer = 0.0f;
		PlaySound(RotateSound, 0.5f, 0.75f);
		if (!m_Board.Fits(Active))
		{
			EndGame();
		}
		return true;
	}

	/// <summary>Drops the piece as far as it goes and settles it; scores two points per row it fell.</summary>
	public void HardDrop()
	{
		if (State != GameState.Playing)
		{
			return;
		}
		Piece landed = m_Board.Drop(Active);
		int fallen = Active.Y - landed.Y;
		Score += 2 * fallen;
		Active = landed;
		PlaySound(DropSound, 0.8f, 1.0f);
		Shake(0.12f + (fallen * 0.012f));
		SpawnDropBurst(landed);
		Settle();
	}

	/// <summary>Clears the full rows: their blocks burst into debris and sparks, the score and level rise, and a sound plays.
	/// Returns the rows cleared.</summary>
	public int ClearFullRows()
	{
		int levelBefore = Level;
		int cleared = 0;
		for (int y = 0; y < Board.Height; y++)
		{
			bool full = true;
			for (int x = 0; x < Board.Width && full; x++)
			{
				full = m_Board[x, y] is not null;
			}
			if (!full)
			{
				continue;
			}
			cleared++;
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
		Score += Board.Score(cleared, Level);
		Rows += cleared;
		PlaySound(cleared >= 4 ? BigClearSound : ClearSound, 0.9f, 1.0f);
		if (Level > levelBefore)
		{
			PlaySound(LevelUpSound, 0.8f, 1.0f);
		}
		Shake(0.2f * cleared);
		return cleared;
	}

	private Entity CreateBlock(string name, Vector3 position, Mesh mesh)
	{
		Entity block = Entity.Create(name);
		// Children of the game: the board follows the game entity's transform.
		block.Parent = this;
		block.Translation = position;
		block.Scale = new Vector3(0.96f);
		MeshComponent component = block.AddComponent<MeshComponent>();
		component.Mesh = mesh;
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

	private void Fall(float deltaTime)
	{
		if (State != GameState.Playing)
		{
			return;
		}
		bool softDrop = Input.IsKeyDown(KeyCode.Down) || Input.IsKeyDown(KeyCode.S);
		float interval = softDrop ? Mathf.Min(SoftDropInterval, CurrentFallInterval) : CurrentFallInterval;
		if (!m_Board.Fits(Active.Moved(0, -1)))
		{
			m_FallTimer = 0.0f;
			m_LockTimer += deltaTime;
			if (m_LockTimer >= LockDelay)
			{
				Settle();
			}
			return;
		}
		m_FallTimer += deltaTime;
		while (m_FallTimer >= interval && m_Board.Fits(Active.Moved(0, -1)))
		{
			m_FallTimer -= interval;
			Active = Active.Moved(0, -1);
			if (softDrop)
			{
				Score += 1;
			}
		}
	}

	private void Settle()
	{
		if (!m_Board.Lock(Active))
		{
			EndGame();
			return;
		}
		PlaySound(LockSound, 0.5f, Random.Range(0.95f, 1.05f));
		ClearFullRows();
		SpawnNext();
	}

	private void SpawnNext()
	{
		Active = Board.Spawn(m_Bag.Next());
		m_FallTimer = 0.0f;
		m_LockTimer = 0.0f;
		m_HoldUsed = false;
		// The stack reached the top: the new piece has no room.
		if (!m_Board.Fits(Active))
		{
			EndGame();
		}
	}

	private void EndGame()
	{
		if (State != GameState.Over)
		{
			PlaySound(GameOverSound, 0.9f, 1.0f);
			Shake(0.5f);
			if (Score > BestScore)
			{
				BestScore = Score;
				m_NewBest = true;
				SaveBest();
			}
		}
		State = GameState.Over;
	}

	private int LoadBestScore()
	{
		if (!SaveBestScore)
		{
			return 0;
		}
		try
		{
			return File.Exists(BestScoreFile) && int.TryParse(File.ReadAllText(BestScoreFile).Trim(), out int best) ? System.Math.Max(best, 0) : 0;
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
			File.WriteAllText(BestScoreFile, BestScore.ToString());
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

	private void PlaySound(AudioClip? clip, float volume, float pitch)
	{
		if (clip is null || m_Sources[0] is null)
		{
			return;
		}
		AudioSourceComponent source = m_Sources[m_NextSource];
		m_NextSource = (m_NextSource + 1) % SoundSources;
		source.Stop();
		source.Clip = clip;
		source.Volume = volume;
		source.Pitch = pitch;
		source.Play();
		SoundsPlayed++;
	}

	private void Shake(float strength)
	{
		Camera?.As<CameraShake>()?.Shake(strength);
	}

	private void Show()
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

		bool inGame = State is GameState.Playing or GameState.Paused;
		ShowPiece(m_PieceCubes, Active, m_Materials[(int)Active.Kind], inGame);
		ShowPiece(m_GhostCubes, m_Board.Drop(Active), m_GhostMaterial!, State == GameState.Playing);
		ShowShape(m_HoldCubes, Held, HoldPosition, Quaternion.Identity, 1.0f);
		// The next pieces sway gently; the ones further back are smaller.
		Quaternion spin = Quaternion.AngleAxis(25.0f * Mathf.Sin((float)Time.Elapsed * 1.5f), Vector3.Up);
		for (int slot = 0; slot < QueueLength; slot++)
		{
			Entity[] cubes = [m_QueueCubes[slot, 0], m_QueueCubes[slot, 1], m_QueueCubes[slot, 2], m_QueueCubes[slot, 3]];
			Vector3 position = NextPosition - new Vector3(0.0f, slot * NextSpacing, 0.0f);
			ShowShape(cubes, m_Bag.Peek(slot), position, spin, slot == 0 ? 1.0f : 0.8f);
		}

		m_Hud!.SetStats(Score, Level, Rows, System.Math.Max(BestScore, Score));
		m_Hud.Show(State, Score, m_NewBest, (float)Time.Elapsed);
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
