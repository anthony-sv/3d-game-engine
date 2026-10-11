using Strada.Interop;

namespace Strada;

/// <summary>Emits camera-facing particles (fire, sparks, smoke, dust, magic) while the scene runs: each lives between
/// <see cref="LifetimeMin"/> and <see cref="LifetimeMax"/> seconds, starts from the <see cref="Shape"/> with a speed between
/// <see cref="SpeedMin"/> and <see cref="SpeedMax"/>, falls with the scene's gravity times <see cref="GravityFactor"/>, slows
/// down with <see cref="Drag"/>, and blends from the start to the end color and size.</summary>
[NativeComponent("ParticleSystem")]
public sealed unsafe class ParticleSystemComponent : Component
{
	private ParticleSystemComponent()
	{
	}

	/// <summary>Whether the system emits <see cref="Rate"/> particles per second.</summary>
	public bool Emitting
	{
		get => NativeField.GetBool(InternalCalls.ParticleSystemComponent_GetEmitting, Entity.ID);
		set => NativeField.SetBool(InternalCalls.ParticleSystemComponent_SetEmitting, Entity.ID, value);
	}

	/// <summary>Particles emitted per second while <see cref="Emitting"/>.</summary>
	public float Rate
	{
		get => NativeField.Get<float>(InternalCalls.ParticleSystemComponent_GetRate, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ParticleSystemComponent_SetRate, Entity.ID, value);
	}

	/// <summary>Particles emitted at once when the system starts (with the scene, or when the entity is created).</summary>
	public uint Burst
	{
		get => NativeField.Get<uint>(InternalCalls.ParticleSystemComponent_GetBurst, Entity.ID);
		set => NativeField.Set<uint>(InternalCalls.ParticleSystemComponent_SetBurst, Entity.ID, value);
	}

	/// <summary>How many particles can be alive at once; emission waits while the system is full.</summary>
	public uint MaxParticles
	{
		get => NativeField.Get<uint>(InternalCalls.ParticleSystemComponent_GetMaxParticles, Entity.ID);
		set => NativeField.Set<uint>(InternalCalls.ParticleSystemComponent_SetMaxParticles, Entity.ID, value);
	}

	/// <summary>Shortest life of a particle in seconds.</summary>
	public float LifetimeMin
	{
		get => NativeField.Get<float>(InternalCalls.ParticleSystemComponent_GetLifetimeMin, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ParticleSystemComponent_SetLifetimeMin, Entity.ID, value);
	}

	/// <summary>Longest life of a particle in seconds.</summary>
	public float LifetimeMax
	{
		get => NativeField.Get<float>(InternalCalls.ParticleSystemComponent_GetLifetimeMax, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ParticleSystemComponent_SetLifetimeMax, Entity.ID, value);
	}

	/// <summary>Slowest start speed in meters per second.</summary>
	public float SpeedMin
	{
		get => NativeField.Get<float>(InternalCalls.ParticleSystemComponent_GetSpeedMin, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ParticleSystemComponent_SetSpeedMin, Entity.ID, value);
	}

	/// <summary>Fastest start speed in meters per second.</summary>
	public float SpeedMax
	{
		get => NativeField.Get<float>(InternalCalls.ParticleSystemComponent_GetSpeedMax, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ParticleSystemComponent_SetSpeedMax, Entity.ID, value);
	}

	/// <summary>Where particles start and which way they go.</summary>
	public ParticleEmitterShape Shape
	{
		get => (ParticleEmitterShape)NativeField.Get<int>(InternalCalls.ParticleSystemComponent_GetShape, Entity.ID);
		set => NativeField.Set<int>(InternalCalls.ParticleSystemComponent_SetShape, Entity.ID, (int)value);
	}

	/// <summary>Spread around the forward axis (-Z) in degrees, for cones, boxes and points.</summary>
	public float ConeAngle
	{
		get => NativeField.Get<float>(InternalCalls.ParticleSystemComponent_GetConeAngle, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ParticleSystemComponent_SetConeAngle, Entity.ID, value);
	}

	/// <summary>Radius of the sphere or of the cone's base, in meters.</summary>
	public float Radius
	{
		get => NativeField.Get<float>(InternalCalls.ParticleSystemComponent_GetRadius, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ParticleSystemComponent_SetRadius, Entity.ID, value);
	}

	/// <summary>Half size of the box shape in meters.</summary>
	public Vector3 BoxHalfExtents
	{
		get => NativeField.Get<Vector3>(InternalCalls.ParticleSystemComponent_GetBoxHalfExtents, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.ParticleSystemComponent_SetBoxHalfExtents, Entity.ID, value);
	}

	/// <summary>Multiplies the scene's gravity; negative values make particles rise.</summary>
	public float GravityFactor
	{
		get => NativeField.Get<float>(InternalCalls.ParticleSystemComponent_GetGravityFactor, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ParticleSystemComponent_SetGravityFactor, Entity.ID, value);
	}

	/// <summary>Fraction of the velocity lost per second.</summary>
	public float Drag
	{
		get => NativeField.Get<float>(InternalCalls.ParticleSystemComponent_GetDrag, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ParticleSystemComponent_SetDrag, Entity.ID, value);
	}

	/// <summary>Width in meters when a particle is born.</summary>
	public float StartSize
	{
		get => NativeField.Get<float>(InternalCalls.ParticleSystemComponent_GetStartSize, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ParticleSystemComponent_SetStartSize, Entity.ID, value);
	}

	/// <summary>Width in meters when a particle dies.</summary>
	public float EndSize
	{
		get => NativeField.Get<float>(InternalCalls.ParticleSystemComponent_GetEndSize, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ParticleSystemComponent_SetEndSize, Entity.ID, value);
	}

	/// <summary>Color and opacity at birth.</summary>
	public Color StartColor
	{
		get => NativeField.Get<Color>(InternalCalls.ParticleSystemComponent_GetStartColor, Entity.ID);
		set => NativeField.Set<Color>(InternalCalls.ParticleSystemComponent_SetStartColor, Entity.ID, value);
	}

	/// <summary>Color and opacity at death.</summary>
	public Color EndColor
	{
		get => NativeField.Get<Color>(InternalCalls.ParticleSystemComponent_GetEndColor, Entity.ID);
		set => NativeField.Set<Color>(InternalCalls.ParticleSystemComponent_SetEndColor, Entity.ID, value);
	}

	/// <summary>Multiplies the colors; values above 1 glow with bloom.</summary>
	public float Intensity
	{
		get => NativeField.Get<float>(InternalCalls.ParticleSystemComponent_GetIntensity, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.ParticleSystemComponent_SetIntensity, Entity.ID, value);
	}

	/// <summary>The particles' texture, or null for a soft round dot.</summary>
	public Texture? Texture
	{
		get
		{
			AssetHandle handle = NativeField.GetAsset(InternalCalls.ParticleSystemComponent_GetTexture, Entity.ID);
			return handle.IsValid ? new Texture(handle) : null;
		}
		set => NativeField.SetAsset(InternalCalls.ParticleSystemComponent_SetTexture, Entity.ID, value);
	}

	/// <summary>Whether particles add light or cover what is behind them.</summary>
	public ParticleBlendMode BlendMode
	{
		get => (ParticleBlendMode)NativeField.Get<int>(InternalCalls.ParticleSystemComponent_GetBlendMode, Entity.ID);
		set => NativeField.Set<int>(InternalCalls.ParticleSystemComponent_SetBlendMode, Entity.ID, (int)value);
	}

	/// <summary>Whether particles stay where they were emitted when the entity moves (otherwise they move with it).</summary>
	public bool WorldSpace
	{
		get => NativeField.GetBool(InternalCalls.ParticleSystemComponent_GetWorldSpace, Entity.ID);
		set => NativeField.SetBool(InternalCalls.ParticleSystemComponent_SetWorldSpace, Entity.ID, value);
	}

	/// <summary>Whether the entity is destroyed once it no longer emits and its last particle died.</summary>
	public bool DestroyWhenFinished
	{
		get => NativeField.GetBool(InternalCalls.ParticleSystemComponent_GetDestroyWhenFinished, Entity.ID);
		set => NativeField.SetBool(InternalCalls.ParticleSystemComponent_SetDestroyWhenFinished, Entity.ID, value);
	}

	/// <summary>How many particles are alive.</summary>
	public uint ParticleCount => InternalCalls.ParticleSystemComponent_GetParticleCount(Entity.ID);

	/// <summary>Emits particles at once (fewer when <see cref="MaxParticles"/> are alive), such as an explosion's burst.</summary>
	/// <param name="count">How many particles to emit.</param>
	public void Emit(uint count)
	{
		InternalCalls.ParticleSystemComponent_Emit(Entity.ID, count);
	}

	/// <summary>Removes every particle at once.</summary>
	public void Clear()
	{
		InternalCalls.ParticleSystemComponent_Clear(Entity.ID);
	}
}
