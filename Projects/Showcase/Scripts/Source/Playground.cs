using Strada;

namespace Showcase;

/// <summary>The physics playground: counts the props knocked over (children of Props that moved or tipped), shows it with the
/// controls, and restarts the scene with R.</summary>
public sealed class Playground : Script
{
	[Tooltip("The parent of the crates and barrels that count")]
	public Entity? Props;

	[Tooltip("Screen-space text that shows the count")]
	public Entity? Status;

	[Tooltip("The scene R restarts")]
	public string Scene = "Scenes/Main.sscene";

	[Tooltip("Meters a prop moves to count as knocked over")]
	[Range(0.05f, 5.0f)]
	public float KnockedDistance = 0.3f;

	[Tooltip("Degrees a prop tips to count as knocked over")]
	[Range(1.0f, 90.0f)]
	public float KnockedAngle = 25.0f;

	private Entity[] m_Props = [];
	private Vector3[] m_Starts = [];
	private Vector3[] m_Ups = [];

	/// <summary>Props knocked over so far.</summary>
	public int Knocked { get; private set; }

	/// <summary>Props that count.</summary>
	public int Total => m_Props.Length;

	protected override void OnCreate()
	{
		m_Props = Props?.Children ?? [];
		m_Starts = new Vector3[m_Props.Length];
		m_Ups = new Vector3[m_Props.Length];
		for (int i = 0; i < m_Props.Length; i++)
		{
			m_Starts[i] = m_Props[i].Transform.WorldTranslation;
			m_Ups[i] = m_Props[i].Transform.Up;
		}
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsKeyPressed(KeyCode.R))
		{
			SceneManager.LoadScene(Scene);
		}

		int knocked = 0;
		float tipped = System.MathF.Cos(KnockedAngle * System.MathF.PI / 180.0f);
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

		TextComponent? status = Status?.GetComponent<TextComponent>();
		if (status is not null)
		{
			status.Text = knocked == Total && Total > 0
				? $"All {Total} knocked down!  Press R to build them again"
				: $"Knocked down: {knocked} / {Total}";
		}
	}
}
