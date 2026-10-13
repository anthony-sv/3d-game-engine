using System.Collections.Generic;
using Strada;

namespace Showcase;

/// <summary>A pressure plate: a trigger collider that raises a gate (a kinematic body) while anything heavy lies or stands on it
/// (a dynamic body, or a character such as the walker), and lowers it again a while after the plate is clear. The plate's own
/// mesh (Plate) glows while it is pressed; the gate's sound plays whenever it starts to move.</summary>
public sealed class PressurePlate : Script
{
	[Tooltip("The gate: a kinematic body the plate raises")]
	public Entity? Gate;

	[Tooltip("The plate's mesh, which glows while pressed")]
	public Entity? Plate;

	[Tooltip("Meters the gate rises")]
	[Range(0.1f, 20.0f)]
	public float OpenHeight = 2.4f;

	[Tooltip("Meters per second the gate moves")]
	[Range(0.1f, 20.0f)]
	public float Speed = 1.8f;

	[Tooltip("Seconds the gate stays open after the plate is clear")]
	[Range(0.0f, 30.0f)]
	public float HoldSeconds = 2.0f;

	[Tooltip("Kilograms a body or a character needs to press the plate")]
	[Range(0.0f, 1000.0f)]
	public float MinimumMass = 1.0f;

	private readonly HashSet<ulong> m_Bodies = [];
	private Vector3 m_Closed;
	private Quaternion m_Rotation;
	private float m_Hold;
	private float m_Target;
	private Material? m_PlateMaterial;
	private float m_PlateIntensity;
	private float m_Glow;

	/// <summary>How far the gate is open, from 0 (closed) to 1 (open).</summary>
	public float Openness { get; private set; }

	/// <summary>Whether something lies on the plate.</summary>
	public bool IsPressed => m_Bodies.Count > 0;

	/// <summary>Times the plate was pressed.</summary>
	public int Presses { get; private set; }

	protected override void OnCreate()
	{
		if (Gate is not null)
		{
			m_Closed = Gate.Transform.WorldTranslation;
			m_Rotation = Gate.Rotation;
		}
		if (Plate?.GetComponent<MeshComponent>() is { } mesh && mesh.GetMaterial(0)?.Clone() is { } material)
		{
			mesh.SetMaterial(0, material);
			m_PlateMaterial = material;
			m_PlateIntensity = material.EmissiveIntensity;
		}
	}

	protected override void OnTriggerEnter(Entity other)
	{
		if (!IsHeavyEnough(other))
		{
			return;
		}
		if (m_Bodies.Count == 0)
		{
			Presses++;
		}
		m_Bodies.Add(other.ID);
	}

	// Characters and dynamic bodies of at least MinimumMass press the plate; static and kinematic bodies do not.
	private bool IsHeavyEnough(Entity other)
	{
		if (other.GetComponent<CharacterControllerComponent>() is { } character)
		{
			return character.Mass >= MinimumMass;
		}
		return other.GetComponent<RigidBodyComponent>() is { Type: RigidBodyType.Dynamic } body && body.Mass >= MinimumMass;
	}

	protected override void OnTriggerExit(Entity other)
	{
		m_Bodies.Remove(other.ID);
	}

	protected override void OnUpdate(float deltaTime)
	{
		// Bodies destroyed while lying on the plate leave without an exit.
		m_Bodies.RemoveWhere(id => Entity.FindByID(id) is null);
		m_Hold = IsPressed ? HoldSeconds : Mathf.Max(0.0f, m_Hold - deltaTime);
		float target = IsPressed || m_Hold > 0.0f ? 1.0f : 0.0f;
		if (target != m_Target)
		{
			m_Target = target;
			Gate?.GetComponent<AudioSourceComponent>()?.Play();
		}
		m_Glow = Mathf.MoveTowards(m_Glow, IsPressed ? 1.0f : 0.0f, deltaTime * 4.0f);
		if (m_PlateMaterial is not null)
		{
			m_PlateMaterial.EmissiveIntensity = m_PlateIntensity * (0.15f + (0.85f * m_Glow));
		}
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		if (Gate?.GetComponent<RigidBodyComponent>() is not { Type: RigidBodyType.Kinematic } body)
		{
			return;
		}
		// Every step, also at rest: a kinematic body keeps the velocity of its last move.
		Openness = Mathf.MoveTowards(Openness, m_Target, Speed / OpenHeight * fixedDeltaTime);
		body.MoveKinematic(m_Closed + (Vector3.Up * (OpenHeight * Mathf.SmoothStep(0.0f, 1.0f, Openness))), m_Rotation);
	}
}
