using Strada;

namespace Blocks;

/// <summary>The game. Pieces fall into the board; the player moves them (Left/Right or A/D, held keys repeat), turns them
/// (Up/W/X clockwise, Z counter-clockwise), drops them faster (Down/S) or at once (Space); full rows clear, burst into
/// debris and score. P pauses, R starts again. The board's cells are drawn by blocks this script creates; settled cells
/// take the color of their piece.</summary>
public sealed class BlocksGame : Script
{
	[Tooltip("The text that shows the score, the cleared rows and the level")]
	public Entity? ScoreText;

	[Tooltip("The text that shows that the game is paused or over")]
	public Entity? MessageText;

	[Tooltip("The camera, shaken by drops and clears (needs a CameraShake script)")]
	public Entity? Camera;

	[Tooltip("Seconds a piece takes to fall one row at level 1")]
	[Range(0.05f, 2.0f)]
	public float FallInterval = 0.8f;

	[Tooltip("Where the next piece is shown (its shape's top-left cell)")]
	public Vector3 PreviewPosition = new(12.0f, 17.0f, 0.0f);

	[Tooltip("The mesh of a block (a cube one unit wide)")]
	public Mesh? BlockMesh;

	[Tooltip("What a cleared block bursts into")]
	public Prefab? Debris;

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
	private readonly Entity[] m_PreviewCubes = new Entity[4];
	private readonly Material[] m_Materials = new Material[s_Colors.Length];
	private readonly AudioSourceComponent[] m_Sources = new AudioSourceComponent[SoundSources];
	private PieceBag m_Bag = new(Random.Range);
	private Material? m_GhostMaterial;
	private float m_FallTimer;
	private float m_LockTimer;
	private float m_RepeatTimer;
	private int m_RepeatDirection;
	private int m_NextSource;
	private string m_ShownScore = "";

	/// <summary>The piece the player controls.</summary>
	public Piece Active { get; private set; }

	public PieceKind Next => m_Bag.Peek();

	public int Score { get; private set; }

	public int Rows { get; private set; }

	public int Level => 1 + (Rows / RowsPerLevel);

	public bool IsOver { get; private set; }

	public bool IsPaused { get; private set; }

	/// <summary>Blocks that burst from cleared rows so far.</summary>
	public int DebrisSpawned { get; private set; }

	/// <summary>Sounds started so far.</summary>
	public int SoundsPlayed { get; private set; }

	/// <summary>Seconds per row at the current level: falls get faster as the level rises.</summary>
	public float CurrentFallInterval => Mathf.Max(0.05f, FallInterval * Mathf.Pow(0.85f, Level - 1));

	/// <summary>The settled cells (tests set up situations on it).</summary>
	public Board Board => m_Board;

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
			m_PreviewCubes[i] = CreateBlock("Next", Vector3.Zero, mesh);
		}
		for (int i = 0; i < SoundSources; i++)
		{
			Entity source = Entity.Create($"Sound {i + 1}");
			source.Parent = this;
			m_Sources[i] = source.AddComponent<AudioSourceComponent>();
		}
		Restart();
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsKeyPressed(KeyCode.R))
		{
			Restart();
		}
		if (Input.IsKeyPressed(KeyCode.P) && !IsOver)
		{
			IsPaused = !IsPaused;
		}
		if (!IsOver && !IsPaused)
		{
			HandleInput(deltaTime);
			Fall(deltaTime);
		}
		Show();
	}

	/// <summary>Starts a new game: an empty board, a new bag and a new score.</summary>
	public void Restart()
	{
		m_Board.Clear();
		m_Bag = new PieceBag(Random.Range);
		Score = 0;
		Rows = 0;
		IsOver = false;
		IsPaused = false;
		SpawnNext();
	}

	/// <summary>Moves the piece sideways by a column when it fits. True when it moved.</summary>
	public bool Shift(int direction)
	{
		Piece moved = Active.Moved(direction, 0);
		if (IsOver || !m_Board.Fits(moved))
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
		if (IsOver || m_Board.Turn(Active, quarterTurns) is not { } turned)
		{
			return false;
		}
		Active = turned;
		m_LockTimer = 0.0f;
		PlaySound(RotateSound, 0.45f, quarterTurns > 0 ? 1.0f : 0.9f);
		return true;
	}

	/// <summary>Drops the piece as far as it goes and settles it; scores two points per row it fell.</summary>
	public void HardDrop()
	{
		if (IsOver)
		{
			return;
		}
		Piece landed = m_Board.Drop(Active);
		int fallen = Active.Y - landed.Y;
		Score += 2 * fallen;
		Active = landed;
		PlaySound(DropSound, 0.8f, 1.0f);
		Shake(0.12f + (fallen * 0.012f));
		Settle();
	}

	/// <summary>Clears the full rows: their blocks burst into debris, the score and level rise, and a sound plays. Returns the
	/// rows cleared.</summary>
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
		// The stack reached the top: the new piece has no room.
		if (!m_Board.Fits(Active))
		{
			EndGame();
		}
		else
		{
			IsOver = false;
		}
	}

	private void EndGame()
	{
		if (!IsOver)
		{
			PlaySound(GameOverSound, 0.9f, 1.0f);
			Shake(0.5f);
		}
		IsOver = true;
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

		bool playing = !IsOver;
		ShowPiece(m_PieceCubes, Active, Vector3.Zero, m_Materials[(int)Active.Kind], playing);
		ShowPiece(m_GhostCubes, m_Board.Drop(Active), Vector3.Zero, m_GhostMaterial!, playing && !IsPaused);
		ShowPreview();

		string score = $"SCORE  {Score}\nROWS   {Rows}\nLEVEL  {Level}";
		if (score != m_ShownScore && ScoreText?.GetComponent<TextComponent>() is { } scoreText)
		{
			scoreText.Text = score;
			m_ShownScore = score;
		}
		if (MessageText?.GetComponent<TextComponent>() is { } message)
		{
			message.Text = IsOver ? "GAME OVER\nR to play again" : IsPaused ? "PAUSED\nP to go on" : "";
		}
	}

	// The next piece turns slowly around its own center.
	private void ShowPreview()
	{
		Piece preview = new(Next, 0, 0, 0);
		Vector3 center = Vector3.Zero;
		foreach ((int x, int y) in preview.Cells)
		{
			center += new Vector3(x, y, 0.0f) * 0.25f;
		}
		Quaternion spin = Quaternion.AngleAxis((float)Time.Elapsed * 50.0f, Vector3.Up);
		int i = 0;
		foreach ((int x, int y) in preview.Cells)
		{
			Entity cube = m_PreviewCubes[i++];
			cube.Translation = PreviewPosition + center + (spin * (new Vector3(x, y, 0.0f) - center));
			cube.Rotation = spin;
			MeshComponent mesh = cube.GetComponent<MeshComponent>()!;
			mesh.Visible = true;
			mesh.SetMaterial(0, m_Materials[(int)Next]);
		}
	}

	private static void ShowPiece(Entity[] cubes, Piece piece, Vector3 offset, Material material, bool visible)
	{
		int i = 0;
		foreach ((int x, int y) in piece.Cells)
		{
			Entity cube = cubes[i++];
			cube.Translation = offset + new Vector3(x, y, 0.0f);
			MeshComponent mesh = cube.GetComponent<MeshComponent>()!;
			// Hidden rows are above the board's top.
			mesh.Visible = visible && (y < Board.Height || offset != Vector3.Zero);
			mesh.SetMaterial(0, material);
		}
	}
}
