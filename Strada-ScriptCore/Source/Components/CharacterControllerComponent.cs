using Strada.Interop;

namespace Strada;

/// <summary>Makes the entity a character that scripts move, such as the player: an upright capsule that walks at the velocity
/// given to <see cref="Move"/>, slides along walls, climbs steps up to <see cref="StepHeight"/> and slopes up to
/// <see cref="MaxSlopeAngle"/>, slides down steeper slopes, falls with gravity, rides what it stands on (moving platforms,
/// elevators) and pushes dynamic bodies. The entity's transform follows the capsule, which does not turn with the entity.
/// Raycasts hit the character, and triggers and bodies report it in their contact callbacks like a kinematic body. The
/// colliders of the entities below it are its own parts: they neither block it nor report touching it.</summary>
[NativeComponent("CharacterController")]
public sealed unsafe class CharacterControllerComponent : Component
{
	private CharacterControllerComponent()
	{
	}

	/// <summary>Radius of the capsule in meters.</summary>
	public float Radius
	{
		get => NativeField.Get<float>(InternalCalls.CharacterControllerComponent_GetRadius, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CharacterControllerComponent_SetRadius, Entity.ID, value);
	}

	/// <summary>Height of the whole capsule in meters, rounded ends included; below twice the radius it is a sphere.</summary>
	public float Height
	{
		get => NativeField.Get<float>(InternalCalls.CharacterControllerComponent_GetHeight, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CharacterControllerComponent_SetHeight, Entity.ID, value);
	}

	/// <summary>Center of the capsule relative to the entity, in meters (scaled with the entity, not turned with it).</summary>
	public Vector3 Offset
	{
		get => NativeField.Get<Vector3>(InternalCalls.CharacterControllerComponent_GetOffset, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.CharacterControllerComponent_SetOffset, Entity.ID, value);
	}

	/// <summary>The highest step the character climbs, and the deepest drop it walks down without leaving the ground, in
	/// meters.</summary>
	public float StepHeight
	{
		get => NativeField.Get<float>(InternalCalls.CharacterControllerComponent_GetStepHeight, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CharacterControllerComponent_SetStepHeight, Entity.ID, value);
	}

	/// <summary>The steepest slope the character walks up, in degrees (1 to 89); it slides down steeper ones.</summary>
	public float MaxSlopeAngle
	{
		get => NativeField.Get<float>(InternalCalls.CharacterControllerComponent_GetMaxSlopeAngle, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CharacterControllerComponent_SetMaxSlopeAngle, Entity.ID, value);
	}

	/// <summary>Kilograms with which the character weighs on what it stands on.</summary>
	public float Mass
	{
		get => NativeField.Get<float>(InternalCalls.CharacterControllerComponent_GetMass, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CharacterControllerComponent_SetMass, Entity.ID, value);
	}

	/// <summary>The largest force, in newtons, with which the character pushes the dynamic bodies it walks into; 0 leaves them
	/// where they are.</summary>
	public float PushForce
	{
		get => NativeField.Get<float>(InternalCalls.CharacterControllerComponent_GetPushForce, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CharacterControllerComponent_SetPushForce, Entity.ID, value);
	}

	/// <summary>Multiplier of the scene's gravity for the character; 0 lets the script fly it (ladders, swimming).</summary>
	public float GravityFactor
	{
		get => NativeField.Get<float>(InternalCalls.CharacterControllerComponent_GetGravityFactor, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CharacterControllerComponent_SetGravityFactor, Entity.ID, value);
	}

	/// <summary>Physics layer index (0-15) of the project settings: the character collides with the layers it collides
	/// with.</summary>
	public uint Layer
	{
		get => NativeField.Get<uint>(InternalCalls.CharacterControllerComponent_GetLayer, Entity.ID);
		set => NativeField.Set<uint>(InternalCalls.CharacterControllerComponent_SetLayer, Entity.ID, value);
	}

	/// <summary>The character's own velocity in meters per second, on top of what <see cref="Move"/> asks for: gravity speeds
	/// it up while it is in the air or on a slope too steep to stand on, landing on walkable ground stops it, and what it runs
	/// into removes the part towards it. Set an upward velocity to jump; on walkable ground, a velocity without an upward
	/// part stops at once. Zero while the scene does not simulate the character.</summary>
	public Vector3 Velocity
	{
		get => NativeField.Get<Vector3>(InternalCalls.CharacterControllerComponent_GetVelocity, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.CharacterControllerComponent_SetVelocity, Entity.ID, value);
	}

	/// <summary>Whether the character stands on ground it can walk on: false in the air and on slopes steeper than
	/// <see cref="MaxSlopeAngle"/>. The capsule's round bottom rolls over the edges of steps and ledges, which are too steep
	/// to stand on, so walking down stairs or off a ledge it is false for a physics step or two at each edge: let a jump
	/// pressed a moment before landing or after leaving the ground still happen (the scripting guide's first-person
	/// controller does).</summary>
	public bool IsGrounded => InternalCalls.CharacterControllerComponent_IsGrounded(Entity.ID) != 0;

	/// <summary>The normal of the ground under the character, walkable or too steep; zero when nothing is under it.</summary>
	public Vector3 GroundNormal => NativeField.Get<Vector3>(InternalCalls.CharacterControllerComponent_GetGroundNormal, Entity.ID);

	/// <summary>Walks the character at a velocity (meters per second, world space) during the next physics step: call it from
	/// <see cref="Script.OnFixedUpdate"/> every step the character should move (a later call before the step replaces the
	/// velocity). Gravity acts on top of it, so the velocity is usually horizontal; to jump, set <see cref="Velocity"/>.</summary>
	public void Move(Vector3 velocity)
	{
		InternalCalls.CharacterControllerComponent_Move(Entity.ID, &velocity);
	}

	/// <summary>Places the entity at a world position at once, keeping its rotation, and stops the character, which finds the
	/// ground where it is placed.</summary>
	public void Teleport(Vector3 position)
	{
		InternalCalls.CharacterControllerComponent_Teleport(Entity.ID, &position);
	}
}
