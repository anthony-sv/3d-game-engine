using Strada;

namespace Blocks;

/// <summary>The game. Pieces fall into the board; the player moves them (Left/Right or A/D, held keys repeat), turns them
/// (Up/W/X clockwise, Z counter-clockwise), drops them faster (Down/S) or at once (Space); full rows clear and score. P
/// pauses, R starts again. The board's cells are drawn by cubes this script creates; settled cells take the color of
/// their piece.</summary>
public sealed class BlocksGame : Script
{
	[Tooltip("The text that shows the score, the cleared rows and the level")]
	public Entity? ScoreText;

	[Tooltip("The text that shows that the game is paused or over")]
	public Entity? MessageText;

	[Tooltip("Seconds a piece takes to fall one row at level 1")]
	[Range(0.05f, 2.0f)]
	public float FallInterval = 0.8f;

	[Tooltip("Where the next piece is shown (its shape's top-left cell)")]
	public Vector3 PreviewPosition = new(12.0f, 17.0f, 0.0f);

	// How fast a held key moves the piece: the first repeat after the delay, then one per interval.
	private const float RepeatDelay = 0.17f;
	private const float RepeatInterval = 0.05f;
	// Seconds a landed piece may still slide before it settles; moving or turning it restarts the wait.
	private const float LockDelay = 0.5f;
	private const float SoftDropInterval = 0.04f;
	private const int RowsPerLevel = 10;

	private static readonly Color[] s_Colors =
	[
		new(0.1f, 0.85f, 0.9f), new(0.95f, 0.85f, 0.1f), new(0.65f, 0.25f, 0.9f), new(0.2f, 0.85f, 0.25f),
		new(0.9f, 0.2f, 0.2f), new(0.2f, 0.35f, 0.95f), new(0.95f, 0.55f, 0.1f),
	];

	private readonly Board m_Board = new();
	private readonly Entity[,] m_CellCubes = new Entity[Board.Width, Board.Height];
	private readonly PieceKind?[,] m_Shown = new PieceKind?[Board.Width, Board.Height];
	private readonly Entity[] m_PieceCubes = new Entity[4];
	private readonly Entity[] m_GhostCubes = new Entity[4];
	private readonly Entity[] m_PreviewCubes = new Entity[4];
	private readonly Material[] m_Materials = new Material[s_Colors.Length];
	private PieceBag m_Bag = new(Random.Range);
	private Material? m_GhostMaterial;
	private float m_FallTimer;
	private float m_LockTimer;
	private float m_RepeatTimer;
	private int m_RepeatDirection;
	private string m_ShownScore = "";

	/// <summary>The piece the player controls.</summary>
	public Piece Active { get; private set; }

	public PieceKind Next => m_Bag.Peek();

	public int Score { get; private set; }

	public int Rows { get; private set; }

	public int Level => 1 + (Rows / RowsPerLevel);

	public bool IsOver { get; private set; }

	public bool IsPaused { get; private set; }

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
			material.Roughness = 0.35f;
			material.Emissive = s_Colors[kind];
			material.EmissiveIntensity = 0.15f;
			m_Materials[kind] = material;
		}
		m_GhostMaterial = Material.Create()!;
		m_GhostMaterial.BaseColor = new Color(1.0f, 1.0f, 1.0f, 0.18f);
		m_GhostMaterial.AlphaMode = MaterialAlphaMode.Blend;

		Mesh cube = Assets.Load<Mesh>("builtin://Cube")!;
		for (int x = 0; x < Board.Width; x++)
		{
			for (int y = 0; y < Board.Height; y++)
			{
				m_CellCubes[x, y] = CreateCube($"Cell {x},{y}", new Vector3(x, y, 0.0f), cube);
			}
		}
		for (int i = 0; i < 4; i++)
		{
			m_PieceCubes[i] = CreateCube("Piece", Vector3.Zero, cube);
			m_GhostCubes[i] = CreateCube("Ghost", Vector3.Zero, cube);
			m_PreviewCubes[i] = CreateCube("Next", Vector3.Zero, cube);
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
		Score += 2 * (Active.Y - landed.Y);
		Active = landed;
		Settle();
	}

	private Entity CreateCube(string name, Vector3 position, Mesh mesh)
	{
		Entity cube = Entity.Create(name);
		// Children of the game: the board follows the game entity's transform.
		cube.Parent = this;
		cube.Translation = position;
		cube.Scale = new Vector3(0.94f);
		MeshComponent component = cube.AddComponent<MeshComponent>();
		component.Mesh = mesh;
		component.Visible = false;
		return cube;
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
			IsOver = true;
			return;
		}
		int cleared = m_Board.ClearRows();
		if (cleared > 0)
		{
			Score += Board.Score(cleared, Level);
			Rows += cleared;
			GetComponent<AudioSourceComponent>()?.Play();
		}
		SpawnNext();
	}

	private void SpawnNext()
	{
		Active = Board.Spawn(m_Bag.Next());
		m_FallTimer = 0.0f;
		m_LockTimer = 0.0f;
		// The stack reached the top: the new piece has no room.
		IsOver = !m_Board.Fits(Active);
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
		Piece preview = new(Next, 0, 0, 0);
		ShowPiece(m_PreviewCubes, preview, PreviewPosition, m_Materials[(int)Next], true);

		string score = $"Score {Score}\nRows {Rows}\nLevel {Level}";
		if (score != m_ShownScore && ScoreText?.GetComponent<TextComponent>() is { } scoreText)
		{
			scoreText.Text = score;
			m_ShownScore = score;
		}
		if (MessageText?.GetComponent<TextComponent>() is { } message)
		{
			message.Text = IsOver ? "Game over\nR to play again" : IsPaused ? "Paused\nP to go on" : "";
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
