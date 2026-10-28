using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Strada;

// A game's types with [VisualNode] (spec-slice3 section 3.7): VisualNodeTests builds libraries with them as the game
// assembly's public types. Every member a test reads is here, with what it should give.
namespace TestGame;

// Members marked one by one.
public static class Probe
{
	private static readonly List<string> s_Calls = [];

	// What the members graphs called did, in order.
	public static IReadOnlyList<string> Calls => s_Calls;

	// Not marked: no node.
	public static int Unmarked => 0;

	[VisualNode]
	public static int Counter { get; set; }

	[VisualNode]
	public static float Gravity = 9.5f;

	[VisualNode]
	public const int Limit = 3;

	public static void Reset()
	{
		s_Calls.Clear();
		Counter = 0;
	}

	// An impure call: methods have flow pins unless Pure is set.
	[VisualNode]
	public static int Add(int a, int b)
	{
		s_Calls.Add($"Add {a} {b}");
		return a + b;
	}

	[VisualNode(Pure = true)]
	public static int Twice(int value) => value * 2;

	[VisualNode(Pure = true)]
	public static void Nothing()
	{
	}

	[VisualNode(Title = "Deal Damage", Category = "Game/Combat", Description = "Hurts whatever it hits.", Keywords = ["hit", "hurt"])]
	public static void Strike(Damage damage)
	{
		s_Calls.Add($"Strike {damage.Amount} {damage.Element}");
	}

	[VisualNode]
	public static void Swap(ref int a, ref int b)
	{
		(a, b) = (b, a);
	}

	[VisualNode]
	public static Task Wait() => Task.CompletedTask;

	[VisualNode]
	internal static void Hidden()
	{
	}

	// Execute is a pin of every impure node, so its parameter is ExecuteParameter, the name of the next one.
	[VisualNode]
	public static void Twins(int execute, int executeParameter)
	{
		s_Calls.Add($"Twins {execute} {executeParameter}");
	}

	[VisualNode]
	public static int Measure(Span<int> values) => values.Length;

	[VisualNode]
	public static void Open(Window window)
	{
		s_Calls.Add($"Open {window.Size}");
	}

	[VisualNode(Title = "A title of more than one hundred and twenty-eight characters, which is more than the palette shows of any node and so is not taken")]
	public static void Long()
	{
	}

	[VisualNode(Category = "Game//Empty")]
	public static void Lost()
	{
	}

	[VisualNode(Keywords = ["", "fine"])]
	public static void Findable()
	{
	}

	[VisualNode(FormerNames = ["Cure", "TestGame.Healer.Mend"])]
	public static int Heal(int amount)
	{
		s_Calls.Add($"Heal {amount}");
		return amount;
	}

	[VisualNode(FormerNames = ["Add"])]
	public static int Sum(int a, int b) => a + b;

	// An inferred generic method (spec-slice3 section 3.6).
	[VisualNode(Pure = true)]
	public static T Pick<T>(T first, T second, bool takeSecond) => takeSecond ? second : first;

	[VisualNode]
	public static void Record(Element[] elements)
	{
		s_Calls.Add($"Record {string.Join(",", elements)}");
	}
}

// An enum of the game: a type of graphs.
[VisualNode]
public enum Element
{
	Fire = 0,
	Water = 1,
	Earth = 2,
}

// Every public member declared on a marked type with a node form is a node: the constructor, the fields and the method.
[VisualNode(Category = "Game/Rules")]
public struct Damage
{
	public float Amount;
	public Element Element;

	public Damage(float amount, Element element)
	{
		Amount = amount;
		Element = element;
	}

	public readonly float Doubled() => Amount * 2.0f;

	// Has no node form, skipped without a note: the type marks it, not its own attribute.
	public readonly void Fill(ref float target)
	{
		target = Amount;
	}
}

// A game type only a marked member's signature uses: a type of graphs too.
public sealed class Inventory
{
	[VisualNode]
	public int Count;
}

// A script class whose instance members take the entity whose script they run on.
public sealed class Player : Script
{
	[VisualNode]
	public Player()
	{
	}

	[VisualNode]
	public int Health { get; set; } = 100;

	[VisualNode]
	public void ApplyDamage(Damage damage)
	{
		Health -= (int)damage.Amount;
	}
}

// Ref structs are never types of graphs.
[VisualNode]
public ref struct Window
{
	public int Size;
}

// A member of an unmarked class that [VisualNode] marks is a node; a former name that is no name is noted and ignored.
public static class Renamed
{
	[VisualNode(FormerNames = ["Old Name"])]
	public static void Current()
	{
	}
}
