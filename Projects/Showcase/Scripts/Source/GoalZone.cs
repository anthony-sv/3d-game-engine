using System.Collections.Generic;
using Strada;

namespace Showcase;

/// <summary>A goal: a trigger collider that counts the balls (any dynamic body) that fall into it, each once. A goal rings the
/// entity's sound, bursts a particle effect at the hoop, makes the hoop flare, shakes the camera a little and shows the count
/// on a world-space text.</summary>
public sealed class GoalZone : Script
{
	[Tooltip("World-space text that shows the count")]
	public Entity? Counter;

	[Tooltip("The hoop: its material flares at every goal")]
	public Entity? Hoop;

	[Tooltip("A one-shot particle effect made where the hoop is at every goal")]
	public Prefab? Burst;

	[Tooltip("The camera a goal shakes a little (it needs a CameraShake script)")]
	public Entity? Camera;

	[Tooltip("How much brighter the hoop glows right after a goal")]
	[Range(0.0f, 20.0f)]
	public float Flare = 6.0f;

	[Tooltip("Seconds the flare takes to fade")]
	[Range(0.05f, 5.0f)]
	public float FlareSeconds = 0.9f;

	private readonly HashSet<ulong> m_Counted = [];
	private Material? m_HoopMaterial;
	private float m_BaseIntensity;
	private float m_Flare;

	/// <summary>Goals scored.</summary>
	public int Count { get; private set; }

	protected override void OnCreate()
	{
		// The hoop gets a material of its own, which the flare changes.
		if (Hoop?.GetComponent<MeshComponent>() is { } mesh && mesh.GetMaterial(0)?.Clone() is { } material)
		{
			mesh.SetMaterial(0, material);
			m_HoopMaterial = material;
			m_BaseIntensity = material.EmissiveIntensity;
		}
		ShowCount();
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (m_Flare <= 0.0f)
		{
			return;
		}
		m_Flare = Mathf.Max(0.0f, m_Flare - (deltaTime / FlareSeconds));
		if (m_HoopMaterial is not null)
		{
			m_HoopMaterial.EmissiveIntensity = m_BaseIntensity * (1.0f + (Flare * m_Flare * m_Flare));
		}
	}

	protected override void OnTriggerEnter(Entity other)
	{
		if (other.GetComponent<RigidBodyComponent>()?.Type != RigidBodyType.Dynamic || !m_Counted.Add(other.ID))
		{
			return;
		}
		Count++;
		ShowCount();
		if (GetComponent<AudioSourceComponent>() is { } sound)
		{
			sound.Pitch = 1.0f + (0.04f * ((Count - 1) % 6));
			sound.Stop();
			sound.Play();
		}
		Vector3 hoop = Hoop?.Transform.WorldTranslation ?? Transform.WorldTranslation;
		if (Burst is not null)
		{
			Entity.Instantiate(Burst, hoop, Quaternion.Identity);
		}
		m_Flare = 1.0f;
		Camera?.As<CameraShake>()?.ShakeFrom(hoop, 0.35f);
	}

	/// <summary>Starts counting from zero again.</summary>
	public void ResetCount()
	{
		Count = 0;
		m_Counted.Clear();
		ShowCount();
	}

	private void ShowCount()
	{
		if (Counter?.GetComponent<TextComponent>() is { } text)
		{
			text.Text = Count.ToString(System.Globalization.CultureInfo.InvariantCulture);
		}
	}
}
