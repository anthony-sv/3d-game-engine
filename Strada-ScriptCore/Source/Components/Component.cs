using System;
using System.Collections.Concurrent;
using System.Reflection;
using Strada.Interop;

namespace Strada;

/// <summary>Base class of the engine components scripts can access through <see cref="Strada.Entity.GetComponent{T}"/>.
/// Component objects are views: they read and write the entity's data directly.</summary>
public abstract class Component
{
	// The engine name and view factory of each component type that IsOn and FetchFrom met.
	private static readonly ConcurrentDictionary<Type, ComponentAccess> s_Access = new();

	// Only the engine's component types derive from Component.
	private protected Component()
	{
	}

	/// <summary>The entity the component belongs to.</summary>
	public Entity Entity { get; internal set; } = null!;

	// Entity.HasComponent<T> for a component type known only at run time (visual scripts).
	internal static unsafe bool IsOn(ulong entity, Type componentType)
	{
		byte[] name = GetAccess(componentType).NativeName;
		fixed (byte* bytes = name)
		{
			return InternalCalls.Entity_HasComponent(entity, bytes, name.Length) != 0;
		}
	}

	// Entity.GetComponent<T> for a component type known only at run time: a view of the entity's component, or null when the
	// entity has none.
	internal static Component? FetchFrom(ulong entity, Type componentType) =>
		IsOn(entity, componentType) ? GetAccess(componentType).Create(ScriptRegistry.GetEntity(entity)) : null;

	// Found once per type, so later calls use no reflection.
	private static ComponentAccess GetAccess(Type componentType) => s_Access.GetOrAdd(componentType, static type =>
	{
		Type info = typeof(ComponentInfo<>).MakeGenericType(type);
		byte[] name = (byte[])info.GetField(nameof(ComponentInfo<TransformComponent>.NativeName), BindingFlags.NonPublic | BindingFlags.Static)!
			.GetValue(null)!;
		Func<Entity, Component> create = info.GetMethod(nameof(ComponentInfo<TransformComponent>.Create), BindingFlags.NonPublic | BindingFlags.Static)!
			.CreateDelegate<Func<Entity, Component>>();
		return new ComponentAccess(name, create);
	});

	private sealed record ComponentAccess(byte[] NativeName, Func<Entity, Component> Create);
}

// Names the engine component a component type views (the ComponentRegistry name).
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
internal sealed class NativeComponentAttribute : Attribute
{
	public NativeComponentAttribute(string name)
	{
		Name = name;
	}

	public string Name { get; }
}

internal static class ComponentInfo<T>
	where T : Component
{
	internal static readonly byte[] NativeName = NativeString.ToUtf8(
		typeof(T).GetCustomAttribute<NativeComponentAttribute>()?.Name
		?? throw new InvalidOperationException($"{typeof(T).FullName} does not name its engine component"));

	internal static T Create(Entity entity)
	{
		T component = (T)Activator.CreateInstance(typeof(T), nonPublic: true)!;
		component.Entity = entity;
		return component;
	}
}
