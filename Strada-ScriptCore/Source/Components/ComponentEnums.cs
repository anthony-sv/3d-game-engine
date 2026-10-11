namespace Strada;

/// <summary>How a camera projects the scene.</summary>
public enum ProjectionType
{
	/// <summary>Perspective projection: distant things look smaller.</summary>
	Perspective = 0,
	/// <summary>Orthographic projection: sizes do not change with distance.</summary>
	Orthographic = 1,
}

/// <summary>How a rigid body moves.</summary>
public enum RigidBodyType
{
	/// <summary>Never moves.</summary>
	Static = 0,
	/// <summary>Moved by the simulation: gravity, forces and collisions.</summary>
	Dynamic = 1,
	/// <summary>Moved by its transform (scripts, animation), pushing dynamic bodies.</summary>
	Kinematic = 2,
}

/// <summary>How <see cref="RigidBodyComponent.AddForce"/> and <see cref="RigidBodyComponent.AddTorque"/> interpret
/// their value.</summary>
public enum ForceMode
{
	/// <summary>A force (newtons) applied during the next physics step; depends on mass.</summary>
	Force = 0,
	/// <summary>An instant impulse (newton-seconds); depends on mass.</summary>
	Impulse = 1,
	/// <summary>An acceleration (m/s²) applied during the next physics step; ignores mass.</summary>
	Acceleration = 2,
	/// <summary>An instant velocity change (m/s); ignores mass.</summary>
	VelocityChange = 3,
}

/// <summary>Where a particle system's particles start.</summary>
public enum ParticleEmitterShape
{
	/// <summary>At the entity's position, moving within <see cref="ParticleSystemComponent.ConeAngle"/> of its forward axis.</summary>
	Point = 0,
	/// <summary>Inside a sphere of <see cref="ParticleSystemComponent.Radius"/>, moving outwards.</summary>
	Sphere = 1,
	/// <summary>On a disc of <see cref="ParticleSystemComponent.Radius"/>, moving along the forward axis (-Z) spread by
	/// <see cref="ParticleSystemComponent.ConeAngle"/>.</summary>
	Cone = 2,
	/// <summary>Inside a box of <see cref="ParticleSystemComponent.BoxHalfExtents"/>, moving along -Z spread by
	/// <see cref="ParticleSystemComponent.ConeAngle"/>.</summary>
	Box = 3,
}

/// <summary>How particles combine with what is behind them.</summary>
public enum ParticleBlendMode
{
	/// <summary>Their light adds up (fire, sparks, magic); bright colors bloom.</summary>
	Additive = 0,
	/// <summary>They cover what is behind by their alpha (smoke, dust).</summary>
	Alpha = 1,
}

/// <summary>How text lines align to the entity's position.</summary>
public enum TextAlignment
{
	/// <summary>Lines start at the position.</summary>
	Left = 0,
	/// <summary>Lines are centered on the position.</summary>
	Center = 1,
	/// <summary>Lines end at the position.</summary>
	Right = 2,
}
