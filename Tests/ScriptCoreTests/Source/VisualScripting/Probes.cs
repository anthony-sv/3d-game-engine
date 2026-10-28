using System;
using System.Collections.Generic;
using System.Text;
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

	[ThreadStatic]
	private static (Color Color, Matrix4 Transform, Quaternion Rotation, ProbeAccess Access)? s_Tinted;

	// What graphs recorded on this thread (tests run their graphs on their own thread).
	internal static List<string> Records => s_Records ??= [];

	internal static int Evaluations => s_Evaluations;

	// What the last Tint received.
	internal static (Color Color, Matrix4 Transform, Quaternion Rotation, ProbeAccess Access)? Tinted => s_Tinted;

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
		s_Tinted = null;
	}

	// Dispatches an event to an entity's instance while the calling graph runs, as engine calls that run scripts do
	// (Entity.Instantiate runs the Create event of the new instance before it returns); returns the status.
	public static int Reenter(ulong entity, int scriptEvent) => (int)s_Runtime!.Invoke(entity, (ScriptEvent)scriptEvent, 0.0f, 0);

	// Loads a new document for the test graph while the calling graph runs, as the engine does when an asset changed while
	// a script it called instantiated a prefab.
	public static void Reload(string document)
	{
		s_Runtime!.Load(TestRuntime.Graph, TestRuntime.Name, Encoding.UTF8.GetBytes(document));
	}

	// Sends a custom event without arguments through the managed API, as a C# script would; returns what SendEvent did.
	public static bool Send(ulong entity, string name) => s_Runtime!.SendEvent(entity, name, []);

	// Records a line (an impure node: Execute, Text, Then).
	public static void Write(string text)
	{
		Records.Add(text);
	}

	// Optional parameters: structs declared "= default" (all zeros in C#) and a [Flags] enum with an enumerator default.
	public static void Tint(Color color = default, Matrix4 transform = default, Quaternion rotation = default, ProbeAccess access = ProbeAccess.Read)
	{
		s_Tinted = (color, transform, rotation, access);
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

	// The integers 0 to count - 1.
	[PureNode]
	public static int[] Numbers(int count)
	{
		int[] numbers = new int[count];
		for (int index = 0; index < count; index++)
		{
			numbers[index] = index;
		}
		return numbers;
	}

	// text count times.
	[PureNode]
	public static string RepeatText(string text, int count) => string.Concat(System.Linq.Enumerable.Repeat(text, count));

	// Unconstrained generic methods (spec-slice3 section 3.6): T from an array input, an array result from a value input,
	// T only in the result, and T in and out.
	[PureNode]
	public static T First<T>(T[] items) => items[0];

	[PureNode]
	public static T[] Repeat<T>(T value, int count)
	{
		T[] values = new T[count];
		System.Array.Fill(values, value);
		return values;
	}

	[PureNode]
	public static T Default<T>() => default!;

	[PureNode]
	public static T Echo<T>(T value) => value;

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
