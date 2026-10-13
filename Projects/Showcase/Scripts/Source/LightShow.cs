using System.Collections.Generic;
using Strada;

namespace Showcase;

/// <summary>A light show: the spot lights among the children cycle through the colors of the rainbow, each a step apart, and
/// sweep back and forth; their glowing parts (meshes with emissive materials under each light, such as a lens and the cones of
/// its shaft) take the light's color.</summary>
public sealed class LightShow : Script
{
	[Tooltip("Turns of the color wheel per second")]
	[Range(0.0f, 2.0f)]
	public float CycleSpeed = 0.07f;

	[Tooltip("Degrees the lights sweep to either side")]
	[Range(0.0f, 60.0f)]
	public float SweepAngle = 16.0f;

	[Tooltip("Sweeps per second")]
	[Range(0.0f, 3.0f)]
	public float SweepSpeed = 0.22f;

	[Tooltip("How colorful the lights are, from white (0) to pure hues (1)")]
	[Range(0.0f, 1.0f)]
	public float Saturation = 0.85f;

	private Entity[] m_Lights = [];
	private Quaternion[] m_Rests = [];
	private List<Material>[] m_Glows = [];
	private float m_Time;

	/// <summary>The spot lights the show drives.</summary>
	public int LightCount => m_Lights.Length;

	protected override void OnCreate()
	{
		List<Entity> lights = [];
		foreach (Entity child in Children)
		{
			if (child.HasComponent<SpotLightComponent>())
			{
				lights.Add(child);
			}
		}
		m_Lights = [.. lights];
		m_Rests = new Quaternion[m_Lights.Length];
		m_Glows = new List<Material>[m_Lights.Length];
		for (int i = 0; i < m_Lights.Length; i++)
		{
			m_Rests[i] = m_Lights[i].Rotation;
			m_Glows[i] = [];
			// The glowing parts get materials of their own to glow in their light's color: one copy of each material, shared
			// by the light's parts that use it.
			Dictionary<Material, Material> copies = [];
			CollectGlows(m_Lights[i], copies);
			m_Glows[i].AddRange(copies.Values);
		}
		Apply();
	}

	protected override void OnUpdate(float deltaTime)
	{
		m_Time += deltaTime;
		Apply();
	}

	/// <summary>The color of the light at the index now.</summary>
	public Color ColorOf(int index)
	{
		float hue = Mathf.Repeat((m_Time * CycleSpeed) + ((float)index / Mathf.Max(1, m_Lights.Length)), 1.0f);
		return FromHsv(hue, Saturation, 1.0f);
	}

	/// <summary>The materials that glow in the color of the light at the index.</summary>
	public IReadOnlyList<Material> GlowsOf(int index) => m_Glows[index];

	private static void CollectGlows(Entity entity, Dictionary<Material, Material> copies)
	{
		foreach (Entity part in entity.Children)
		{
			if (part.GetComponent<MeshComponent>() is { } mesh && mesh.GetMaterial(0) is { } material && IsGlowing(material))
			{
				if (!copies.TryGetValue(material, out Material? copy) && material.Clone() is { } clone)
				{
					copies[material] = clone;
					copy = clone;
				}
				if (copy is not null)
				{
					mesh.SetMaterial(0, copy);
				}
			}
			CollectGlows(part, copies);
		}
	}

	private void Apply()
	{
		for (int i = 0; i < m_Lights.Length; i++)
		{
			Color color = ColorOf(i);
			if (m_Lights[i].GetComponent<SpotLightComponent>() is { } light)
			{
				light.Color = color;
			}
			foreach (Material glow in m_Glows[i])
			{
				glow.Emissive = color;
			}
			float wave = 2.0f * Mathf.PI * SweepSpeed * m_Time;
			float yaw = SweepAngle * Mathf.Sin(wave + (i * 1.9f));
			float pitch = 0.5f * SweepAngle * Mathf.Sin((wave * 0.7f) + (i * 1.3f));
			m_Lights[i].Rotation = m_Rests[i] * Quaternion.AngleAxis(yaw, Vector3.Up) * Quaternion.AngleAxis(pitch, Vector3.Right);
		}
	}

	private static bool IsGlowing(Material material)
	{
		Color emissive = material.Emissive;
		return material.EmissiveIntensity > 0.0f && emissive.R + emissive.G + emissive.B > 0.0f;
	}

	private static Color FromHsv(float hue, float saturation, float value)
	{
		float sector = hue * 6.0f;
		float r = Mathf.Clamp01(Mathf.Abs(sector - 3.0f) - 1.0f);
		float g = Mathf.Clamp01(2.0f - Mathf.Abs(sector - 2.0f));
		float b = Mathf.Clamp01(2.0f - Mathf.Abs(sector - 4.0f));
		return new Color(value * (1.0f - (saturation * (1.0f - r))), value * (1.0f - (saturation * (1.0f - g))),
			value * (1.0f - (saturation * (1.0f - b))));
	}
}
