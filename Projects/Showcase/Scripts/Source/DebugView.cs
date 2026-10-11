using Strada;

namespace Showcase;

/// <summary>The debug view, which its key turns on and off: with Debug.DrawLine it draws the path a ball thrown now would
/// take (and where it lands), the ball launcher's path to its target, the bounds of the trigger colliders under Triggers and the
/// links of the bridge's chain.</summary>
public sealed class DebugView : Script
{
	[Tooltip("The key that shows and hides the debug view")]
	public KeyCode Key = KeyCode.T;

	[Tooltip("Whether the debug view shows")]
	public bool Visible;

	[Tooltip("The camera rig whose throws are previewed (a FlyCamera)")]
	public Entity? Rig;

	[Tooltip("The ball launcher whose path is drawn")]
	public Entity? Launcher;

	[Tooltip("The entities whose trigger colliders are outlined (they and their children)")]
	public Entity? Triggers;

	[Tooltip("The bridge whose chain of planks is drawn")]
	public Entity? Bridge;

	private static readonly Color s_ThrowColor = new(0.35f, 1.0f, 0.45f);
	private static readonly Color s_LauncherColor = new(1.0f, 0.6f, 0.2f);
	private static readonly Color s_TriggerColor = new(0.25f, 0.8f, 1.0f);
	private static readonly Color s_BridgeColor = new(1.0f, 0.9f, 0.25f);

	private readonly Vector3[] m_Corners = new Vector3[8];

	/// <summary>Lines drawn in the last frame.</summary>
	public int LinesDrawn { get; private set; }

	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsKeyPressed(Key))
		{
			Visible = !Visible;
		}
		LinesDrawn = 0;
		if (!Visible)
		{
			return;
		}
		if (Rig?.As<FlyCamera>() is { } rig)
		{
			Vector3 aim = rig.AimDirection.Normalized;
			DrawPath(rig.ThrowOrigin(aim), aim * rig.ThrowSpeed, 4.0f, s_ThrowColor);
		}
		if (Launcher?.As<BallLauncher>() is { } launcher)
		{
			DrawPath(launcher.MuzzlePosition, launcher.LaunchVelocity, 4.0f, s_LauncherColor);
		}
		if (Triggers is not null)
		{
			OutlineTriggers(Triggers);
		}
		Entity[] planks = Bridge?.Children ?? [];
		for (int i = 1; i < planks.Length; i++)
		{
			Line(planks[i - 1].Transform.WorldTranslation, planks[i].Transform.WorldTranslation, s_BridgeColor);
		}
	}

	// A ballistic path in steps of 1/20 second, up to where it first hits a collider (marked with a cross).
	private void DrawPath(Vector3 start, Vector3 velocity, float seconds, Color color)
	{
		Vector3 gravity = Physics.Gravity;
		Vector3 previous = start;
		for (float time = 0.05f; time <= seconds; time += 0.05f)
		{
			Vector3 next = start + (velocity * time) + (gravity * (0.5f * time * time));
			Vector3 step = next - previous;
			if (Physics.Raycast(previous, step, step.Length, out RaycastHit hit))
			{
				Line(previous, hit.Point, color);
				Cross(hit.Point, 0.25f, color);
				return;
			}
			Line(previous, next, color);
			previous = next;
		}
	}

	private void OutlineTriggers(Entity entity)
	{
		if (entity.GetComponent<BoxColliderComponent>() is { IsTrigger: true } box)
		{
			Matrix4 world = entity.Transform.WorldTransform;
			Vector3 half = box.HalfExtents;
			for (int i = 0; i < 8; i++)
			{
				Vector3 local = box.Offset + new Vector3((i & 1) != 0 ? half.X : -half.X, (i & 2) != 0 ? half.Y : -half.Y,
					(i & 4) != 0 ? half.Z : -half.Z);
				m_Corners[i] = world.TransformPoint(local);
			}
			// The 12 edges join corners that differ in one axis.
			for (int i = 0; i < 8; i++)
			{
				for (int bit = 1; bit < 8; bit <<= 1)
				{
					if ((i & bit) == 0)
					{
						Line(m_Corners[i], m_Corners[i | bit], s_TriggerColor);
					}
				}
			}
		}
		foreach (Entity child in entity.Children)
		{
			OutlineTriggers(child);
		}
	}

	private void Cross(Vector3 center, float size, Color color)
	{
		Line(center - new Vector3(size, 0.0f, 0.0f), center + new Vector3(size, 0.0f, 0.0f), color);
		Line(center - new Vector3(0.0f, size, 0.0f), center + new Vector3(0.0f, size, 0.0f), color);
		Line(center - new Vector3(0.0f, 0.0f, size), center + new Vector3(0.0f, 0.0f, size), color);
	}

	private void Line(Vector3 from, Vector3 to, Color color)
	{
		Debug.DrawLine(from, to, color);
		LinesDrawn++;
	}
}
