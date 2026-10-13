using Strada;

namespace Showcase;

/// <summary>A run of dominoes (the children, in order): its key (or the tour) tips the first towards the second, and the rest
/// follow. Counts the dominoes that fell and can stand them all up again.</summary>
public sealed class DominoRun : Script, ITourCue
{
	[Tooltip("The key that tips the first domino")]
	public KeyCode Key = KeyCode.D2;

	[Tooltip("Radians per second the push turns the first domino at")]
	[Range(0.1f, 20.0f)]
	public float PushSpin = 2.2f;

	[Tooltip("Degrees a domino leans to count as fallen")]
	[Range(1.0f, 90.0f)]
	public float FallenAngle = 40.0f;

	private Entity[] m_Dominoes = [];
	private Vector3[] m_Starts = [];
	private Quaternion[] m_Rotations = [];
	private Vector3[] m_Ups = [];

	/// <summary>The dominoes of the run.</summary>
	public int Count => m_Dominoes.Length;

	/// <summary>Times the run was tipped.</summary>
	public int Pushes { get; private set; }

	protected override void OnCreate()
	{
		m_Dominoes = Children;
		m_Starts = new Vector3[m_Dominoes.Length];
		m_Rotations = new Quaternion[m_Dominoes.Length];
		m_Ups = new Vector3[m_Dominoes.Length];
		for (int i = 0; i < m_Dominoes.Length; i++)
		{
			m_Starts[i] = m_Dominoes[i].Transform.WorldTranslation;
			// The run's entity is not turned: the dominoes' rotations are their world rotations.
			m_Rotations[i] = m_Dominoes[i].Rotation;
			m_Ups[i] = m_Dominoes[i].Transform.Up;
		}
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsKeyPressed(Key))
		{
			Push();
		}
	}

	/// <summary>Tips the first domino towards the second.</summary>
	public void Push()
	{
		if (m_Dominoes.Length < 2 || m_Dominoes[0].GetComponent<RigidBodyComponent>() is not { } body)
		{
			return;
		}
		Vector3 towards = m_Starts[1] - m_Starts[0];
		towards.Y = 0.0f;
		// Turning about this axis leans the domino's top towards the next one.
		body.AngularVelocity = Vector3.Cross(Vector3.Up, towards.Normalized) * PushSpin;
		body.WakeUp();
		Pushes++;
	}

	/// <inheritdoc/>
	public void Perform() => Push();

	/// <summary>Dominoes that lean more than FallenAngle.</summary>
	public int Fallen
	{
		get
		{
			int fallen = 0;
			float upright = Mathf.Cos(FallenAngle * Mathf.Deg2Rad);
			for (int i = 0; i < m_Dominoes.Length; i++)
			{
				if (m_Dominoes[i].IsValid && Vector3.Dot(m_Dominoes[i].Transform.Up, m_Ups[i]) < upright)
				{
					fallen++;
				}
			}
			return fallen;
		}
	}

	/// <summary>Whether the last domino of the run fell.</summary>
	public bool LastFell => m_Dominoes.Length > 0 && m_Dominoes[^1].IsValid &&
		Vector3.Dot(m_Dominoes[^1].Transform.Up, m_Ups[^1]) < Mathf.Cos(FallenAngle * Mathf.Deg2Rad);

	/// <summary>Stands every domino up where it started.</summary>
	public void StandUp()
	{
		for (int i = 0; i < m_Dominoes.Length; i++)
		{
			if (m_Dominoes[i].IsValid && m_Dominoes[i].GetComponent<RigidBodyComponent>() is { } body)
			{
				body.Teleport(m_Starts[i], m_Rotations[i]);
				body.LinearVelocity = Vector3.Zero;
				body.AngularVelocity = Vector3.Zero;
			}
		}
	}
}
