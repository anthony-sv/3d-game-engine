using Strada;

namespace Showcase;

/// <summary>The physics playground: counts the props knocked over (children of Props that moved or tipped), which the HUD shows;
/// R (or the HUD's reset button) puts everything back: the props where they stood, the dominoes up, the wrecking ball at rest,
/// and the thrown and launched balls away.</summary>
public sealed class Playground : Script
{
	[Tooltip("The parent of the crates and barrels that count")]
	public Entity? Props;

	[Tooltip("The domino run R stands up again")]
	public Entity? Dominoes;

	[Tooltip("The camera rig whose balls R clears")]
	public Entity? Rig;

	[Tooltip("The ball launcher whose balls R clears")]
	public Entity? Launcher;

	[Tooltip("The wrecking ball R stops")]
	public Entity? WreckingBall;

	[Tooltip("The key that puts everything back")]
	public KeyCode Key = KeyCode.R;

	[Tooltip("Meters a prop moves to count as knocked over")]
	[Range(0.05f, 5.0f)]
	public float KnockedDistance = 0.3f;

	[Tooltip("Degrees a prop tips to count as knocked over")]
	[Range(1.0f, 90.0f)]
	public float KnockedAngle = 25.0f;

	private Entity[] m_Props = [];
	private Vector3[] m_Starts = [];
	private Quaternion[] m_Rotations = [];
	private Vector3[] m_Ups = [];

	/// <summary>Props knocked over so far.</summary>
	public int Knocked { get; private set; }

	/// <summary>Props that count.</summary>
	public int Total => m_Props.Length;

	/// <summary>Times everything was put back.</summary>
	public int Resets { get; private set; }

	protected override void OnCreate()
	{
		m_Props = Props?.Children ?? [];
		m_Starts = new Vector3[m_Props.Length];
		m_Rotations = new Quaternion[m_Props.Length];
		m_Ups = new Vector3[m_Props.Length];
		for (int i = 0; i < m_Props.Length; i++)
		{
			m_Starts[i] = m_Props[i].Transform.WorldTranslation;
			// Props is not turned: the props' rotations are their world rotations.
			m_Rotations[i] = m_Props[i].Rotation;
			m_Ups[i] = m_Props[i].Transform.Up;
		}
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsKeyPressed(Key))
		{
			PutBack();
		}

		int knocked = 0;
		float tipped = Mathf.Cos(KnockedAngle * Mathf.Deg2Rad);
		for (int i = 0; i < m_Props.Length; i++)
		{
			Entity prop = m_Props[i];
			if (!prop.IsValid)
			{
				continue;
			}
			bool moved = Vector3.Distance(prop.Transform.WorldTranslation, m_Starts[i]) > KnockedDistance;
			bool tilted = Vector3.Dot(prop.Transform.Up, m_Ups[i]) < tipped;
			if (moved || tilted)
			{
				knocked++;
			}
		}
		Knocked = knocked;
	}

	/// <summary>Puts every prop back where it stood, stands the dominoes up, stops the wrecking ball and clears the balls.</summary>
	public void PutBack()
	{
		for (int i = 0; i < m_Props.Length; i++)
		{
			if (m_Props[i].IsValid && m_Props[i].GetComponent<RigidBodyComponent>() is { } body)
			{
				body.Teleport(m_Starts[i], m_Rotations[i]);
				body.LinearVelocity = Vector3.Zero;
				body.AngularVelocity = Vector3.Zero;
			}
		}
		Dominoes?.As<DominoRun>()?.StandUp();
		WreckingBall?.As<WreckingBall>()?.Stop();
		Rig?.As<FlyCamera>()?.ClearBalls();
		Launcher?.As<BallLauncher>()?.ClearBalls();
		Resets++;
	}
}
