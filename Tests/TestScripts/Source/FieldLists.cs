using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace Strada.Tests;

// A list field of every element type, as an array (T[]) and as a List<T>, plus serialized fields of types no field type
// stores, which the engine reports once for the class. OnCreate and OnUpdate write what the instance holds into its name.
public sealed class FieldLists : Script
{
	public bool[] Flags = [true, false];
	public int[] Counts = [1, -2, 3];
	public uint[] Masks = [7];
	public long[] Bigs = [-9000000000];
	public ulong[] Huges = [18000000000000000000];
	[Range(0.0f, 10.0f)]
	[Tooltip("Units per second, each")]
	public float[] Speeds = [2.5f];
	public double[] Precises = [0.125];
	public string[] Titles = ["Hello", ""];
	public Vector2[] Sizes = [new(1.0f, 2.0f)];
	public Vector3[] Offsets = [];
	public Vector4[] Weights = [new(1.0f, 2.0f, 3.0f, 4.0f)];
	public Quaternion[] Turns = [Quaternion.Identity];
	public Color[] Tints = [new(0.5f, 0.25f, 1.0f, 1.0f)];
	public Entity?[] Targets = [null];
	public Prefab?[] Crates = [];
	public AssetHandle[] Models = [];
	public Mesh?[] Shapes = [];
	public Difficulty[] Levels = [Difficulty.Hard, Difficulty.Easy];
	public Permissions[] Accesses = [Permissions.Read | Permissions.Write];
	public Tiny[] Smalls = [];

	public List<bool> FlagList = [true];
	public List<int> CountList = [4, 5];
	public List<uint> MaskList = [];
	public List<long> BigList = [];
	public List<ulong> HugeList = [];
	public List<float> SpeedList = [0.5f];
	public List<double> PreciseList = [];
	public List<string> TitleList = ["first"];
	public List<Vector2> SizeList = [];
	public List<Vector3> OffsetList = [new(1.0f, 2.0f, 3.0f)];
	public List<Vector4> WeightList = [];
	public List<Quaternion> TurnList = [];
	public List<Color> TintList = [];
	public List<Entity?> TargetList = [];
	public List<Prefab?> CrateList = [];
	public List<AssetHandle> ModelList = [];
	public List<Mesh?> ShapeList = [];
	public List<Difficulty> LevelList = [Difficulty.Normal];
	public List<Permissions> AccessList = [];
	public List<Tiny> SmallList = [];

	// A null array is the empty list.
	public float[]? Missing;
	// Quaternions of length zero (C#'s default) are stored as the identity, alone and in lists.
	public Quaternion UnsetTurn;
	public Quaternion[] UnsetTurns = new Quaternion[2];

	// Not fields: no field type stores them, and the engine says so once for the class.
	public Dictionary<string, int> Table = [];
	public int[][] Jagged = [];
	public int[,] Grid = new int[1, 1];
	public List<List<int>> Nested = [];
	public List<Matrix4> Transforms = [];
	// More elements than a list holds.
	public int[] Oversized = new int[5000];

	protected override void OnCreate() => Describe();

	protected override void OnUpdate(float deltaTime)
	{
		// Counts of [-1] asks for a list longer than lists are: the engine reads the instance back without it.
		if (Counts is [-1])
		{
			CountList = [.. Enumerable.Range(0, 5000)];
		}
		Describe();
	}

	private void Describe()
	{
		Name = string.Create(CultureInfo.InvariantCulture,
			$"{string.Join(",", Counts)}|{string.Join(",", CountList)}|{string.Join(",", Speeds)}|{string.Join(",", TitleList)}|{string.Join(",", Levels)}|{string.Join(",", LevelList)}|{string.Join(",", Targets.Select(target => target?.ID ?? 0))}|{string.Join(",", Shapes.Select(shape => shape?.Handle.ID ?? 0))}|{Missing?.Length ?? -1}|{Oversized.Length}");
	}
}

// A long list the game changes while the inspector shows it (Docs/Architecture.md §11, "Long lists"): each update counts
// itself in Updates and writes the count into one element of Samples, walking along the list; while Length is 0 or more,
// Samples is first resized to it (elements added at its end are 0). Steady never changes.
public sealed class ListWriter : Script
{
	public List<float> Samples = [];
	public int Updates;
	public int Length = -1;
	public int Steady = 7;

	protected override void OnUpdate(float deltaTime)
	{
		Updates++;
		if (Length >= 0 && Samples.Count > Length)
		{
			Samples.RemoveRange(Length, Samples.Count - Length);
		}
		while (Length >= 0 && Samples.Count < Length)
		{
			Samples.Add(0.0f);
		}
		if (Samples.Count > 0)
		{
			Samples[Updates % Samples.Count] = Updates;
		}
	}
}
