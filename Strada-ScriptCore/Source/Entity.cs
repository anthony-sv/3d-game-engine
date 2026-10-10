using System;
using System.Globalization;
using Strada.Interop;

namespace Strada;

/// <summary>An object of the running scene, identified by its ID. Entities compare equal when their IDs do.</summary>
public unsafe class Entity : IEquatable<Entity>
{
	/// <summary>For scripts: the engine sets the ID before <see cref="Script.OnCreate"/>.</summary>
	protected Entity()
	{
	}

	internal Entity(ulong id)
	{
		ID = id;
	}

	/// <summary>The entity's unique, persistent identifier (0 for no entity).</summary>
	public ulong ID { get; internal set; }

	/// <summary>Whether the entity exists in the running scene (destroyed entities stop existing at the end of the
	/// frame).</summary>
	public bool IsValid => ID != 0 && InternalCalls.Entity_IsValid(ID) != 0;

	/// <summary>The entity's name.</summary>
	public string Name
	{
		get
		{
			int length = 0;
			byte* name = InternalCalls.Entity_GetName(ID, &length);
			return NativeString.FromUtf8(name, length);
		}
		set
		{
			byte[] name = NativeString.ToUtf8(value);
			fixed (byte* bytes = name)
			{
				InternalCalls.Entity_SetName(ID, bytes, name.Length);
			}
		}
	}

	/// <summary>The entity's transform (every entity has one).</summary>
	public TransformComponent Transform => ComponentInfo<TransformComponent>.Create(this);

	/// <summary>The position relative to the parent.</summary>
	public Vector3 Translation
	{
		get => Transform.Translation;
		set => Transform.Translation = value;
	}

	/// <summary>The rotation relative to the parent.</summary>
	public Quaternion Rotation
	{
		get => Transform.Rotation;
		set => Transform.Rotation = value;
	}

	/// <summary>The scale relative to the parent.</summary>
	public Vector3 Scale
	{
		get => Transform.Scale;
		set => Transform.Scale = value;
	}

	/// <summary>Whether the entity has a component of type <typeparamref name="T"/>.</summary>
	public bool HasComponent<T>()
		where T : Component
	{
		fixed (byte* name = ComponentInfo<T>.NativeName)
		{
			return InternalCalls.Entity_HasComponent(ID, name, ComponentInfo<T>.NativeName.Length) != 0;
		}
	}

	/// <summary>The entity's component of type <typeparamref name="T"/>, or null when it has none.</summary>
	public T? GetComponent<T>()
		where T : Component
	{
		return HasComponent<T>() ? ComponentInfo<T>.Create(this) : null;
	}

	/// <summary>Destroys the entity and its children at the end of the frame.</summary>
	public void Destroy()
	{
		InternalCalls.Entity_Destroy(ID);
	}

	/// <summary>Creates an entity at the root of the running scene.</summary>
	public static Entity Create(string name)
	{
		byte[] text = NativeString.ToUtf8(name);
		fixed (byte* bytes = text)
		{
			return new Entity(InternalCalls.Entity_Create(bytes, text.Length));
		}
	}

	/// <summary>The first entity (in hierarchy order) with the given name, or null.</summary>
	public static Entity? FindByName(string name)
	{
		byte[] text = NativeString.ToUtf8(name);
		fixed (byte* bytes = text)
		{
			ulong id = InternalCalls.Entity_FindByName(bytes, text.Length);
			return id != 0 ? new Entity(id) : null;
		}
	}

	/// <summary>The entity with the given ID, or null when there is none.</summary>
	public static Entity? FindByID(ulong id)
	{
		return id != 0 && InternalCalls.Entity_IsValid(id) != 0 ? new Entity(id) : null;
	}

	/// <summary>Whether both refer to the same entity (or both are null).</summary>
	public static bool operator ==(Entity? a, Entity? b) => a is null ? b is null : a.Equals(b);

	/// <summary>Whether they refer to different entities.</summary>
	public static bool operator !=(Entity? a, Entity? b) => !(a == b);

	/// <summary>Whether <paramref name="other"/> refers to the same entity.</summary>
	public bool Equals(Entity? other) => other is not null && ID == other.ID;

	/// <inheritdoc/>
	public override bool Equals(object? obj) => obj is Entity other && Equals(other);

	/// <inheritdoc/>
	public override int GetHashCode() => ID.GetHashCode();

	/// <summary>"Entity(ID)".</summary>
	public override string ToString() => string.Create(CultureInfo.InvariantCulture, $"Entity({ID})");
}
