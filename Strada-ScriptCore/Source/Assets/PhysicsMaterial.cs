using Strada.Interop;

namespace Strada;

/// <summary>How the two sides of a contact combine a value of their physics materials (friction or restitution). When the
/// two sides name different modes, the one later in this list wins.</summary>
public enum PhysicsMaterialCombine
{
	/// <summary>The square root of the product: the friction rule of colliders without a physics material.</summary>
	GeometricMean = 0,
	/// <summary>Half the sum.</summary>
	Average = 1,
	/// <summary>The smaller value.</summary>
	Minimum = 2,
	/// <summary>The product.</summary>
	Multiply = 3,
	/// <summary>The larger value: the restitution rule of colliders without a physics material.</summary>
	Maximum = 4,
}

/// <summary>A physics material asset (<c>.sphysmat</c>): how a surface slides, bounces and rolls, the name scripts tell it
/// by, and what impacts on it sound and look like. A collider whose Material names one takes its friction, restitution and
/// combine modes in place of its own; a raycast hit names the material of the surface it hit
/// (<see cref="RaycastHit.Material"/>). Edit it in the editor; scripts read it, and an edit made while the game plays shows
/// on the next frame. A material that cannot be loaded reads as a new one's values (logged).</summary>
public sealed class PhysicsMaterial : Asset
{
	// The runtime's copy of the asset's values, shared by every reference to the asset; found on first use.
	private PhysicsMaterialCache.Entry? m_Entry;

	internal PhysicsMaterial(AssetHandle handle)
		: base(handle)
	{
	}

	internal PhysicsMaterial(PhysicsMaterialCache.Entry entry)
		: base(entry.Handle)
	{
		m_Entry = entry;
	}

	/// <summary>The friction coefficient: 0 slides freely, 1 holds about as well as rubber on concrete (a collider's own
	/// default is 0.6). A contact combines it with the other side's by the modes of
	/// <see cref="FrictionCombine"/>.</summary>
	public float Friction => GetValues().Friction;

	/// <summary>Bounciness from 0 to 1: 0 does not bounce, 1 keeps all of the speed along the contact's normal. A contact
	/// combines it with the other side's by the modes of <see cref="RestitutionCombine"/>.</summary>
	public float Restitution => GetValues().Restitution;

	/// <summary>How a contact combines the frictions of its two sides (a collider without a material says
	/// <see cref="PhysicsMaterialCombine.GeometricMean"/>); when the sides differ, the mode later in
	/// <see cref="PhysicsMaterialCombine"/> wins.</summary>
	public PhysicsMaterialCombine FrictionCombine => GetValues().FrictionCombine;

	/// <summary>How a contact combines the restitutions of its two sides (a collider without a material says
	/// <see cref="PhysicsMaterialCombine.Maximum"/>); when the sides differ, the mode later in
	/// <see cref="PhysicsMaterialCombine"/> wins.</summary>
	public PhysicsMaterialCombine RestitutionCombine => GetValues().RestitutionCombine;

	/// <summary>The force opposing a wheel that rolls on the surface, per newton of load (about 0.01 for a tyre on asphalt),
	/// for scripts and graphs to apply; rigid bodies roll without it.</summary>
	public float RollingResistance => GetValues().RollingResistance;

	/// <summary>The surface's name (such as Asphalt, Gravel or Ice), which scripts and graphs compare to tell surfaces
	/// apart; empty for none.</summary>
	public string Surface => GetValues().Surface;

	/// <summary>A sound for impacts on the surface, or null. A collision that begins plays it where it begins when the
	/// project's mixer names a bus for impact sounds (its Impacts), louder for a stronger impact; scripts can play it
	/// too.</summary>
	public AudioClip? ImpactSound => GetValues().ImpactSound;

	/// <summary>A prefab (usually a particle system) for impacts on the surface, or null; contacts spawn nothing by
	/// themselves.</summary>
	public Prefab? ImpactParticles => GetValues().ImpactParticles;

	private PhysicsMaterialValues GetValues()
	{
		m_Entry ??= PhysicsMaterialCache.Find(Handle);
		return m_Entry.GetValues();
	}
}
