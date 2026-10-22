using System;
using Strada.Interop;

namespace Strada;

/// <summary>Runs a visual script (a graph asset) on the entity, beside or instead of a C# script. While the scene runs, C#
/// scripts raise the graph's custom events and read and write its variables through this component.</summary>
/// <remarks>Values cross between C# and the graph as links between nodes convert them. A value converts to a type when it has
/// the type, when it is a number that widens to it (int to long, float or double; uint to long, ulong, float or double; long
/// and ulong to float or double; float to double), when the type is a base type of it (an entity or script instance as one of
/// its base classes, a typed asset as <see cref="Asset"/>), when it is a component and the type is <see cref="Strada.Entity"/>
/// (the component's entity), and when the type is <see cref="object"/>.</remarks>
[NativeComponent("VisualScript")]
public sealed unsafe class VisualScriptComponent : Component
{
	private VisualScriptComponent()
	{
	}

	/// <summary>The visual script the entity runs, or null. Changed while the scene runs, it takes effect at the scene's next
	/// update: the running graph ends (its Destroy event runs) and the new one starts. The values the component stores for
	/// the graph's public variables stay as they are.</summary>
	public VisualScript? Graph
	{
		get
		{
			AssetHandle handle = NativeField.GetAsset(InternalCalls.VisualScriptComponent_GetGraph, Entity.ID);
			return handle.IsValid ? new VisualScript(handle) : null;
		}
		set => NativeField.SetAsset(InternalCalls.VisualScriptComponent_SetGraph, Entity.ID, value);
	}

	/// <summary>Raises the custom event <paramref name="name"/> of the entity's running graph with the arguments, which
	/// must match the event's parameters in number. Each argument converts to its parameter's type as a link converts it
	/// (see the remarks), and also to text for a <see cref="string"/> parameter and from a typed asset to its
	/// <see cref="AssetHandle"/>; null is accepted for parameters of reference types only. The event runs at once; raised
	/// while a graph runs (the creation of a prefab instance a graph spawns), it runs within that graph's step budget.</summary>
	/// <param name="name">The name of the event (an <c>Event.Custom</c> node of the graph).</param>
	/// <param name="arguments">The values of the event's parameters, in their order. To pass one null argument, write
	/// <c>(object?)null</c>.</param>
	/// <returns>True when the event ran to its end or waits in a latent node (a delay, ...). False, with a warning logged,
	/// when the entity runs no visual script, its graph has no custom event of the name, or the arguments do not fit the
	/// event's parameters; the event does not run then.</returns>
	/// <exception cref="ArgumentNullException"><paramref name="name"/> or <paramref name="arguments"/> is null.</exception>
	/// <exception cref="InvalidOperationException">The event stopped with an error, for example a division by zero or a
	/// member call that threw; the exception carries the error, which is also logged.</exception>
	public bool SendEvent(string name, params object?[] arguments)
	{
		ArgumentNullException.ThrowIfNull(name);
		ArgumentNullException.ThrowIfNull(arguments);
		return Host.VisualScripts.SendEvent(Entity.ID, name, arguments);
	}

	/// <summary>The value of the variable <paramref name="name"/> (public or private, not a function's local) of the entity's
	/// running graph, as a <typeparamref name="T"/>: the variable's own type, a wider numeric type, a base type or
	/// <see cref="object"/>. A variable of a reference type that holds no value reads as null.</summary>
	/// <typeparam name="T">The type to read the value as.</typeparam>
	/// <param name="name">The name of the variable.</param>
	/// <returns>The variable's value.</returns>
	/// <exception cref="ArgumentNullException"><paramref name="name"/> is null.</exception>
	/// <exception cref="InvalidOperationException">The entity runs no visual script (it has none set, the scene does not
	/// run, or the graph cannot run).</exception>
	/// <exception cref="ArgumentException">The graph has no variable of the name.</exception>
	/// <exception cref="InvalidCastException">The value does not convert to <typeparamref name="T"/>, or it holds no value
	/// and <typeparamref name="T"/> is a value type that cannot be null.</exception>
	public T? GetVariable<T>(string name)
	{
		ArgumentNullException.ThrowIfNull(name);
		return (T?)Host.VisualScripts.GetVariable(Entity.ID, name, typeof(T));
	}

	/// <summary>Sets the variable <paramref name="name"/> (public or private, not a function's local) of the entity's running
	/// graph, which sees the new value at once. The value must convert to the variable's type (see the remarks). Null sets a
	/// variable of a reference type to none (a <see cref="string"/> variable to the empty string) when
	/// <typeparamref name="T"/> converts to the variable's type.</summary>
	/// <typeparam name="T">The type of the value.</typeparam>
	/// <param name="name">The name of the variable.</param>
	/// <param name="value">The value.</param>
	/// <exception cref="ArgumentNullException"><paramref name="name"/> is null.</exception>
	/// <exception cref="InvalidOperationException">The entity runs no visual script (it has none set, the scene does not
	/// run, or the graph cannot run).</exception>
	/// <exception cref="ArgumentException">The graph has no variable of the name.</exception>
	/// <exception cref="InvalidCastException">The value does not convert to the variable's type.</exception>
	public void SetVariable<T>(string name, T value)
	{
		ArgumentNullException.ThrowIfNull(name);
		Host.VisualScripts.SetVariable(Entity.ID, name, typeof(T), value);
	}

	/// <summary>Whether the entity's running graph has a variable (public or private) of the name.</summary>
	/// <param name="name">The name of the variable.</param>
	/// <returns>False also when the entity runs no visual script.</returns>
	/// <exception cref="ArgumentNullException"><paramref name="name"/> is null.</exception>
	public bool HasVariable(string name)
	{
		ArgumentNullException.ThrowIfNull(name);
		return Host.VisualScripts.HasVariable(Entity.ID, name);
	}
}
