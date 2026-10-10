using System;
using System.Reflection;
using Strada.Interop;

namespace Strada;

/// <summary>Base class of the engine components scripts can access through <see cref="Strada.Entity.GetComponent{T}"/>.
/// Component objects are views: they read and write the entity's data directly.</summary>
public abstract class Component
{
	// Only the engine's component types derive from Component.
	private protected Component()
	{
	}

	/// <summary>The entity the component belongs to.</summary>
	public Entity Entity { get; internal set; } = null!;
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
