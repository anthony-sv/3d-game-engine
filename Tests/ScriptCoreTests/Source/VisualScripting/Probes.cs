using System;
using System.Collections.Generic;
using Strada.Interop;
using Strada.VisualScripting;

namespace Strada.ScriptCore.Tests;

// API members only the runtime tests' node library has (the library is built over them as over the scripting API):
// graphs record what ran through them, and they stand in for engine members with outcomes the tests choose. None of
// them reaches native code, so graphs that use them run without the engine.
public static class Probe
{
	[ThreadStatic]
	private static List<string>? s_Records;

	[ThreadStatic]
	private static int s_Evaluations;

	[ThreadStatic]
	private static VisualScriptRuntime? s_Runtime;

	// What graphs recorded on this thread (tests run their graphs on their own thread).
	internal static List<string> Records => s_Records ??= [];

	internal static int Evaluations => s_Evaluations;

	// The runtime Reenter dispatches to.
	internal static VisualScriptRuntime? Runtime
	{
		get => s_Runtime;
		set => s_Runtime = value;
	}

	internal static void Reset()
	{
		s_Records = [];
		s_Evaluations = 0;
		s_Runtime = null;
	}

	// Dispatches an event to an entity's instance while the calling graph runs, as engine calls that run scripts do
	// (Entity.Instantiate runs the Create event of the new instance before it returns); returns the status.
	public static int Reenter(ulong entity, int scriptEvent) => (int)s_Runtime!.Invoke(entity, (ScriptEvent)scriptEvent, 0.0f, 0);

	// Records a line (an impure node: Execute, Text, Then).
	public static void Write(string text)
	{
		Records.Add(text);
	}

	// A new number each time a consumer evaluates it (a pure node that counts its evaluations).
	[PureNode]
	public static int Next() => ++s_Evaluations;

	// A member that throws (an impure node).
	public static void Throw(string message) => throw new InvalidOperationException(message);

	// A pure member with out parameters.
	[PureNode]
	public static bool Split(int value, out int half, out int rest)
	{
		half = value / 2;
		rest = value - half;
		return value % 2 == 0;
	}

	// An entity by ID, as a plain Entity (0 gives None).
	[PureNode]
	public static Entity? EntityOf(ulong id) => id != 0 ? new Entity(id) : null;

	// A new view of the entity's Script component, as GetComponent makes them.
	[PureNode]
	public static ScriptComponent ScriptComponentOf(Entity entity) => ComponentInfo<ScriptComponent>.Create(entity);

	[PureNode]
	public static TransformComponent TransformOf(Entity entity) => ComponentInfo<TransformComponent>.Create(entity);

	[PureNode]
	public static Material MaterialOf(ulong handle) => new(new AssetHandle(handle));

	[PureNode]
	public static Mesh MeshOf(ulong handle) => new(new AssetHandle(handle));

	// count entities with the IDs 1 to count.
	[PureNode]
	public static Entity[] Entities(int count)
	{
		Entity[] entities = new Entity[count];
		for (int index = 0; index < count; index++)
		{
			entities[index] = new Entity((ulong)index + 1);
		}
		return entities;
	}

	[PureNode]
	public static Entity[]? NoEntities() => null;

	[PureNode]
	public static Vector3[] Points(int count)
	{
		Vector3[] points = new Vector3[count];
		for (int index = 0; index < count; index++)
		{
			points[index] = new Vector3(index, index * 2, index * 3);
		}
		return points;
	}

	[PureNode]
	public static RaycastHit HitOf(Entity entity, float distance) => new(entity, Vector3.Up, Vector3.Forward, distance);

	[PureNode]
	public static Holder? NoHolder() => null;
}

// A struct of the test library: Make and Break build and split it, and it has a member with a struct Target.
public struct Pair
{
	public int First;
	public int Second;

	[PureNode]
	public readonly int Sum() => First + Second;
}

// A class of the test library: graphs construct it, set and read its field and call members on it.
public sealed class Holder
{
	public int Count;

	public void Increment()
	{
		Count++;
	}

	[PureNode]
	public int Twice() => Count * 2;
}

// Script classes of the test library's game (Kind Entity).
public sealed class Mover : Script
{
}

public sealed class Jumper : Script
{
}
