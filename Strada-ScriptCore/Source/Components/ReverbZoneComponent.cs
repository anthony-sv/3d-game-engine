using System;
using Strada.Interop;

namespace Strada;

/// <summary>A reverb zone: a sphere or a box round the entity inside which spatial sounds send part of themselves to a bus
/// of the project's mixer that carries a reverb (an echo under a bridge, a tunnel, a hall). It follows the sound, not the
/// listener: a sound inside the zone echoes for a listener outside it. The shape is placed and turned by the entity and sized
/// in meters times its scale (a sphere by its largest axis). A sound sends <see cref="SendLevel"/> of itself inside the
/// shape, less and less across <see cref="FadeDistance"/> outside it; of several zones of one bus the largest send counts,
/// and a sound sends to the two buses it sends most to. Sounds that are not spatial send nothing. A change applies from
/// the next frame.</summary>
[NativeComponent("ReverbZone")]
public sealed unsafe class ReverbZoneComponent : Component
{
	private ReverbZoneComponent()
	{
	}

	/// <summary>A sphere of <see cref="Radius"/> or a box of <see cref="HalfExtents"/>.</summary>
	public ReverbZoneShape Shape
	{
		get => (ReverbZoneShape)NativeField.Get<int>(InternalCalls.ReverbZoneComponent_GetShape, Entity.ID);
		set => NativeField.Set<int>(InternalCalls.ReverbZoneComponent_SetShape, Entity.ID, (int)value);
	}

	/// <summary>The sphere's radius in meters (times the entity's largest scale), from 0 to 100,000.</summary>
	public float Radius
	{
		get => NativeField.Get<float>(InternalCalls.ReverbZoneComponent_GetRadius, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ReverbZoneComponent_SetRadius, Entity.ID, value);
	}

	/// <summary>Meters from the box's center to each face (times the entity's scale), each from 0 to 100,000.</summary>
	public Vector3 HalfExtents
	{
		get => NativeField.Get<Vector3>(InternalCalls.ReverbZoneComponent_GetHalfExtents, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.ReverbZoneComponent_SetHalfExtents, Entity.ID, value);
	}

	/// <summary>Meters outside the shape across which the send falls to nothing, from 0 (it stops at the surface) to
	/// 100,000.</summary>
	public float FadeDistance
	{
		get => NativeField.Get<float>(InternalCalls.ReverbZoneComponent_GetFadeDistance, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ReverbZoneComponent_SetFadeDistance, Entity.ID, value);
	}

	/// <summary>The bus of the project's mixer the zone sends to, by name in any letter case: one that carries a reverb.
	/// Empty, or a name the mixer lacks (logged once), sends nothing.</summary>
	public string Bus
	{
		get => NativeField.GetString(InternalCalls.ReverbZoneComponent_GetBus, Entity.ID);
		set
		{
			ArgumentNullException.ThrowIfNull(value);
			NativeField.SetString(InternalCalls.ReverbZoneComponent_SetBus, Entity.ID, value);
		}
	}

	/// <summary>How much of a spatial sound inside the zone reaches the bus, from 0 to 1 (linear).</summary>
	public float SendLevel
	{
		get => NativeField.Get<float>(InternalCalls.ReverbZoneComponent_GetSendLevel, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ReverbZoneComponent_SetSendLevel, Entity.ID, value);
	}
}
