using Strada;

namespace Game;

/// <summary>Where a run of Orb Run stands.</summary>
public enum RunState
{
	/// <summary>The clock runs and the controls roll the orb.</summary>
	Playing,
	/// <summary>Every spark was collected before the time ran out.</summary>
	Won,
	/// <summary>The time ran out first.</summary>
	Lost,
}

/// <summary>The run: it places the sparks, runs the clock, counts the sparks the orb collects, and ends in a win or a loss
/// that Space, Enter or a gamepad's A or Start plays again.</summary>
public sealed class OrbRun : Script
{
	[Tooltip("The spark a run places at every spot")]
	public Prefab? SparkPrefab;

	[Tooltip("The container of the run's sparks")]
	public Entity? Sparks;

	[Tooltip("The orb the player rolls")]
	public Entity? Orb;

	[Tooltip("The sound of a collected spark")]
	public Entity? PickupSound;

	[Tooltip("The sound of a won run")]
	public Entity? WinSound;

	[Tooltip("The sound of a run whose time ran out")]
	public Entity? LoseSound;

	[Tooltip("Seconds a run lasts")]
	[Range(5.0f, 600.0f)]
	public float TimeLimit = 35.0f;

	[Tooltip("Where the orb starts a run")]
	public Vector3 Start = new(0.0f, 0.5f, 0.0f);

	// The height the sparks float at.
	private const float SparkHeight = 0.9f;
	// Twelve spots: eight round the edge of the arena and four nearer the middle, so a run sweeps the whole floor.
	private static readonly Vector3[] s_Spots = MakeSpots();

	/// <summary>Where the run stands.</summary>
	public RunState State { get; private set; } = RunState.Playing;

	/// <summary>The sparks collected in this run.</summary>
	public int Collected { get; private set; }

	/// <summary>The sparks this run placed.</summary>
	public int Total { get; private set; }

	/// <summary>The seconds left in this run; the tests set it to skip ahead.</summary>
	public float TimeLeft { get; set; }

	/// <summary>How many runs have started: the screen notices a new run by it.</summary>
	public int Runs { get; private set; }

	protected override void OnCreate()
	{
		// The first run begins at once.
		StartRun();
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (State == RunState.Playing)
		{
			TimeLeft = Mathf.Max(0.0f, TimeLeft - deltaTime);
			if (TimeLeft <= 0.0f)
			{
				End(RunState.Lost, LoseSound);
			}
			return;
		}
		// After the end: Space, Enter, or a gamepad's A or Start plays again.
		if (Input.IsKeyPressed(KeyCode.Space) || Input.IsKeyPressed(KeyCode.Enter) || Input.IsGamepadButtonPressed(GamepadButton.A) ||
			Input.IsGamepadButtonPressed(GamepadButton.Start))
		{
			StartRun();
		}
	}

	/// <summary>Starts a new run: fresh sparks, a full clock and the orb back in the middle.</summary>
	public void StartRun()
	{
		PlaceSparks();
		Collected = 0;
		TimeLeft = TimeLimit;
		if (Orb?.GetComponent<RigidBodyComponent>() is RigidBodyComponent body)
		{
			body.Teleport(Start, Quaternion.Identity);
			body.LinearVelocity = Vector3.Zero;
			body.AngularVelocity = Vector3.Zero;
		}
		Runs++;
		SetState(RunState.Playing);
	}

	// One spark at every spot, the old ones gone.
	private void PlaceSparks()
	{
		if (Sparks is null || SparkPrefab is null)
		{
			return;
		}
		foreach (Entity old in Sparks.Children)
		{
			old.Destroy();
		}
		Total = 0;
		foreach (Vector3 spot in s_Spots)
		{
			if (Entity.Instantiate(SparkPrefab, spot, Quaternion.Identity, Sparks)?.As<Spark>() is Spark spark)
			{
				spark.Collected += OnSparkCollected;
				Total++;
			}
		}
	}

	// Count the spark, ring a semitone higher for each one, and win at the last.
	private void OnSparkCollected(Spark spark)
	{
		if (State != RunState.Playing)
		{
			return;
		}
		Collected++;
		if (PickupSound?.GetComponent<AudioSourceComponent>() is AudioSourceComponent pickup)
		{
			// The last of twelve rings an octave up.
			pickup.Pitch = Mathf.Pow(2.0f, (Collected - 1) / 12.0f);
			pickup.Play();
		}
		if (Collected >= Total)
		{
			End(RunState.Won, WinSound);
		}
	}

	private void End(RunState state, Entity? sound)
	{
		sound?.GetComponent<AudioSourceComponent>()?.Play();
		SetState(state);
	}

	// The orb rolls only while a run is on.
	private void SetState(RunState state)
	{
		State = state;
		if (Orb?.As<Orb>() is Orb orb)
		{
			orb.Rolling = state == RunState.Playing;
		}
	}

	private static Vector3[] MakeSpots()
	{
		Vector3[] spots = new Vector3[12];
		for (int index = 0; index < 8; index++)
		{
			float angle = (22.5f + (45.0f * index)) * Mathf.Deg2Rad;
			spots[index] = new Vector3(8.0f * Mathf.Cos(angle), SparkHeight, 8.0f * Mathf.Sin(angle));
		}
		for (int index = 0; index < 4; index++)
		{
			float angle = 90.0f * index * Mathf.Deg2Rad;
			spots[8 + index] = new Vector3(4.0f * Mathf.Cos(angle), SparkHeight, 4.0f * Mathf.Sin(angle));
		}
		return spots;
	}
}
