using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace Strada.Tests;

// Exercises Strada.SaveSlot in OnCreate and names its entity "SaveSlots:ok", or "SaveSlots:" and the first thing that went
// otherwise. With a game data folder (Available) it saves slot Slot with a value of every type, reads it back through a
// second object opened in other letter case, checks the refusals, lists the slots and, unless Keep, deletes it; Get of a
// value as another type is asked twice (the engine tests count one warning). Without one, the slot is Unavailable, keeps
// values in memory, and Save and Delete answer false.
//
// With BenchmarkRounds above 0 it measures instead (StradaTests' SaveSlotBenchmark, never asserted on timings): the slot of
// SaveSlotBenchmarkTests.cpp (a lap's ghost at 10 samples a second and BenchmarkLaps laps' positions) set on an object,
// saved, opened again and read back, each round; it names its entity "Benchmark: set <median> <95th percentile> ms; save
// ...; open ...; get ...; round trip ...".
//
// With ExpectedLists above 0 it opens slot Slot, which the test wrote, and checks that it holds the values List0, List1, ...
// of 4,096 Vector4 each.
public sealed class SaveSlotProbe : Script
{
	public string Slot = "Probe";
	public bool Available = true;
	public bool Keep;
	public int BenchmarkRounds;
	public int BenchmarkLaps;
	public int ExpectedLists;

	private readonly List<string> m_Failures = [];

	protected override void OnCreate()
	{
		if (BenchmarkRounds > 0)
		{
			Benchmark();
			return;
		}
		if (ExpectedLists > 0)
		{
			ReadLists();
			return;
		}
		SaveSlot slot = SaveSlot.Open(Slot);
		Expect(slot.Name, Slot.ToLowerInvariant(), "Name");
		Expect(slot.Status, Available ? SaveSlotStatus.Missing : SaveSlotStatus.Unavailable, "Status after Open");
		Expect(slot.Has("Best"), false, "Has before Set");
		Expect(slot.Get("Best", 7.5f), 7.5f, "Get's fallback");

		slot.Set("Bool", true);
		slot.Set("Int", -3);
		slot.Set("UInt", 4u);
		slot.Set("Long", -5_000_000_000L);
		slot.Set("ULong", 18_000_000_000_000_000_000UL);
		slot.Set("Best", 92.25f);
		slot.Set("Double", 0.1);
		slot.Set("Text", "Strada Moto");
		slot.Set("Vector2", new Vector2(1.0f, 2.0f));
		slot.Set("Vector3", new Vector3(1.0f, 2.0f, 3.0f));
		slot.Set("Vector4", new Vector4(1.0f, 2.0f, 3.0f, 4.0f));
		// Kept of length one, as it reads back.
		slot.Set("Rotation", new Quaternion(0.0f, 0.0f, 0.0f, 2.0f));
		slot.Set("Color", new Color(1.0f, 0.5f, 0.25f, 1.0f));
		slot.Set<Entity>("Self", this);
		slot.Set<Prefab?>("Prefab", null);
		slot.Set("Asset", new AssetHandle(42));
		slot.Set("Key", KeyCode.Space);
		slot.Set("Splits", new[] { 30.5f, 61.25f });
		slot.Set("Points", new[] { new Vector3(1.0f, 2.0f, 3.0f), new Vector3(4.0f, 5.0f, 6.0f) });
		slot.Set("Laps", new List<int> { 1, 2, 3 });
		slot.Set("Rotations", new[] { Quaternion.Identity });
		Expect(slot.Has("Best"), true, "Has after Set");
		Expect(slot.Get("Best", 0.0f), 92.25f, "Get after Set");

		Refuses(() => slot.Set("Nan", float.NaN), "Set of NaN");
		Refuses(() => slot.Set("Far", new Vector3(float.PositiveInfinity, 0.0f, 0.0f)), "Set of an infinite vector");
		Refuses(() => slot.Set("Zero", new Quaternion(0.0f, 0.0f, 0.0f, 0.0f)), "Set of a zero quaternion");
		Refuses(() => slot.Set("Long list", new float[4097]), "Set of 4,097 elements");
		Refuses(() => slot.Set("", 1), "Set of an empty name");
		Refuses(() => slot.Set("Tab\tName", 1), "Set of a name with a control character");
		Refuses(() => slot.Set("Object", new object()), "Set of an object");
		Refuses(() => SaveSlot.Open("a/b"), "Open of a/b");
		Refuses(() => SaveSlot.Open("CON"), "Open of CON");
		bool threw = false;
		try
		{
			slot.Has(null!);
		}
		catch (ArgumentNullException)
		{
			threw = true;
		}
		Expect(threw, true, "Has(null) throws");

		slot.Set("Gone", 1);
		Expect(slot.Remove("Gone"), true, "Remove of a value");
		Expect(slot.Remove("Gone"), false, "Remove of a removed value");

		if (!Available)
		{
			Expect(slot.Save(), false, "Save without a folder");
			Expect(slot.Delete(), false, "Delete without a folder");
			Expect(slot.Status, SaveSlotStatus.Unavailable, "Status after a refused Save");
			Expect(slot.Has("Best"), true, "values kept after a refused Delete");
			Expect(SaveSlot.GetNames().Length, 0, "GetNames without a folder");
			Report();
			return;
		}

		Expect(slot.Save(), true, "Save");
		Expect(slot.Status, SaveSlotStatus.Present, "Status after Save");
		SaveSlot read = SaveSlot.Open(Slot.ToUpperInvariant());
		Expect(read.Status, SaveSlotStatus.Present, "Status of the read slot");
		Expect(read.Get("Bool", false), true, "Bool");
		Expect(read.Get("Int", 0), -3, "Int");
		Expect(read.Get("UInt", 0u), 4u, "UInt");
		Expect(read.Get("Long", 0L), -5_000_000_000L, "Long");
		Expect(read.Get("ULong", 0UL), 18_000_000_000_000_000_000UL, "ULong");
		Expect(read.Get("Best", 0.0f), 92.25f, "Float");
		Expect(read.Get("Double", 0.0), 0.1, "Double");
		Expect(read.Get("Text", ""), "Strada Moto", "String");
		Expect(read.Get("Vector2", Vector2.Zero), new Vector2(1.0f, 2.0f), "Vector2");
		Expect(read.Get("Vector3", Vector3.Zero), new Vector3(1.0f, 2.0f, 3.0f), "Vector3");
		Expect(read.Get("Vector4", Vector4.Zero), new Vector4(1.0f, 2.0f, 3.0f, 4.0f), "Vector4");
		Expect(read.Get("Rotation", new Quaternion(0.0f, 0.0f, 0.0f, 0.5f)), Quaternion.Identity, "Quaternion");
		Expect(read.Get("Color", Color.Black), new Color(1.0f, 0.5f, 0.25f, 1.0f), "Color");
		Expect(read.Get<Entity?>("Self", null)?.ID ?? 0, ID, "Entity");
		Expect(read.Has("Prefab") && read.Get<Prefab?>("Prefab", null) == null, true, "Prefab");
		Expect(read.Get("Asset", AssetHandle.Invalid), new AssetHandle(42), "Asset");
		Expect(read.Get("Key", KeyCode.W), KeyCode.Space, "enum");
		Expect(string.Join(",", read.Get("Splits", Array.Empty<float>())), "30.5,61.25", "float[]");
		Expect(read.Get("Points", Array.Empty<Vector3>()).SequenceEqual([new Vector3(1.0f, 2.0f, 3.0f), new Vector3(4.0f, 5.0f, 6.0f)]),
			true, "Vector3[]");
		Expect(string.Join(",", read.Get("Laps", new List<int>())), "1,2,3", "List<int>");
		// An array and a list of a type are one type.
		Expect(string.Join(",", read.Get("Laps", Array.Empty<int>())), "1,2,3", "List<int> as int[]");
		Expect(read.Get("Rotations", Array.Empty<Quaternion>()).SequenceEqual([Quaternion.Identity]), true, "Quaternion[]");
		// A value of another type answers the fallback, warned about once.
		Expect(read.Get("Text", 5), 5, "Get of another type");
		Expect(read.Get("Text", 6), 6, "Get of another type again");
		Expect(read.Has("Gone"), false, "a removed value is not saved");
		Expect(SaveSlot.GetNames().Contains(Slot.ToLowerInvariant()), true, "GetNames");

		if (!Keep)
		{
			Expect(read.Delete(), true, "Delete");
			Expect(read.Status, SaveSlotStatus.Missing, "Status after Delete");
			Expect(read.Has("Best"), false, "values after Delete");
			Expect(SaveSlot.Open(Slot).Status, SaveSlotStatus.Missing, "Status of a deleted slot");
			Expect(read.Delete(), true, "Delete of a missing slot");
		}
		Report();
	}

	private void Expect<T>(T actual, T expected, string what)
	{
		if (!EqualityComparer<T>.Default.Equals(actual, expected))
		{
			m_Failures.Add($"{what}: {actual} != {expected}");
		}
	}

	private void Refuses(Action action, string what)
	{
		try
		{
			action();
			m_Failures.Add($"{what} was not refused");
		}
		catch (ArgumentException)
		{
		}
	}

	private void Report() => Name = m_Failures.Count == 0 ? "SaveSlots:ok" : "SaveSlots:" + m_Failures[0];

	private void ReadLists()
	{
		SaveSlot slot = SaveSlot.Open(Slot);
		Expect(slot.Status, SaveSlotStatus.Present, "Status of the written slot");
		for (int index = 0; index < ExpectedLists; index++)
		{
			Expect(slot.Get(string.Create(CultureInfo.InvariantCulture, $"List{index}"), Array.Empty<Vector4>()).Length, 4096,
				string.Create(CultureInfo.InvariantCulture, $"List{index}'s length"));
		}
		Report();
	}

	private void Benchmark()
	{
		const int samples = 4096;
		Quaternion[] rotations = new Quaternion[samples];
		float[] speeds = new float[samples];
		for (int sample = 0; sample < samples; sample++)
		{
			float angle = sample * 0.0015f;
			rotations[sample] = Quaternion.AngleAxis(angle * 180.0f / MathF.PI, Vector3.Up);
			speeds[sample] = 40.0f + 30.0f * MathF.Sin(angle * 7.0f);
		}
		List<Vector3[]> laps = [];
		for (int lap = 0; lap < BenchmarkLaps; lap++)
		{
			float offset = lap * 0.25f;
			Vector3[] positions = new Vector3[samples];
			for (int index = 0; index < samples; index++)
			{
				float turn = index * 0.0015f + offset;
				positions[index] = new Vector3(MathF.Cos(turn) * 412.37f, 0.5f + offset, MathF.Sin(turn) * 297.61f);
			}
			laps.Add(positions);
		}

		List<double> set = [];
		List<double> save = [];
		List<double> open = [];
		List<double> get = [];
		List<double> total = [];
		for (int round = 0; round < BenchmarkRounds; round++)
		{
			SaveSlot slot = SaveSlot.Open(Slot);
			long start = System.Diagnostics.Stopwatch.GetTimestamp();
			Fill(slot, rotations, speeds, laps);
			long filled = System.Diagnostics.Stopwatch.GetTimestamp();
			if (!slot.Save())
			{
				Name = "Benchmark: the save failed";
				return;
			}
			long saved = System.Diagnostics.Stopwatch.GetTimestamp();
			SaveSlot read = SaveSlot.Open(Slot);
			long opened = System.Diagnostics.Stopwatch.GetTimestamp();
			float checksum = read.Get("BestLap", 0.0f) + read.Get("Ghost.Rotations", Array.Empty<Quaternion>()).Length +
				read.Get("Ghost.Speeds", Array.Empty<float>()).Length;
			for (int lap = 0; lap < laps.Count; lap++)
			{
				checksum += read.Get($"Ghost.Positions.{lap}", Array.Empty<Vector3>()).Length;
			}
			long got = System.Diagnostics.Stopwatch.GetTimestamp();
			if (checksum < samples)
			{
				Name = "Benchmark: the slot did not read back";
				return;
			}
			set.Add(Milliseconds(start, filled));
			save.Add(Milliseconds(filled, saved));
			open.Add(Milliseconds(saved, opened));
			get.Add(Milliseconds(opened, got));
			total.Add(Milliseconds(start, got));
		}
		Name = string.Create(CultureInfo.InvariantCulture,
			$"Benchmark: set {Describe(set)}; save {Describe(save)}; open {Describe(open)}; get {Describe(get)}; round trip {Describe(total)}");
	}

	private static void Fill(SaveSlot slot, Quaternion[] rotations, float[] speeds, List<Vector3[]> laps)
	{
		slot.Set("Ghost.Rotations", rotations);
		slot.Set("Ghost.Speeds", speeds);
		slot.Set("Splits", new[] { 30.2f, 61.7f, 92.4f });
		slot.Set("BestLap", 92.418f);
		slot.Set("Laps", 3);
		slot.Set("Rider", "Rider");
		for (int lap = 0; lap < laps.Count; lap++)
		{
			slot.Set($"Ghost.Positions.{lap}", laps[lap]);
		}
	}

	private static double Milliseconds(long from, long to) => (to - from) * 1000.0 / System.Diagnostics.Stopwatch.Frequency;

	private static string Describe(List<double> values)
	{
		List<double> sorted = [.. values.Order()];
		double median = sorted[sorted.Count / 2];
		double high = sorted[Math.Min(sorted.Count - 1, (int)(0.95 * sorted.Count))];
		return string.Create(CultureInfo.InvariantCulture, $"{median:F2} {high:F2} ms");
	}
}

// Exercises Strada.Settings in OnCreate and names its entity "Settings:ok", or "Settings:" and the first thing that went
// otherwise: the defaults the game started with, then every setter read back at once, the game's named values, and Save,
// which succeeds only with a game data folder (Available). Its window ends at 400 x 100, windowed, vsync off; Music at -6 dB
// and muted, the master volume at 0.5, the quality Low, and LinkedBrakes true.
public sealed class SettingsProbe : Script
{
	public bool Available = true;

	private readonly List<string> m_Failures = [];

	protected override void OnCreate()
	{
		Expect(Settings.Quality, QualityPreset.High, "Quality's default");
		Expect(Settings.MasterVolume, 1.0f, "MasterVolume's default");
		Expect(Settings.GetBusVolume("Music"), 0.0f, "a bus's default");
		Expect(Settings.IsBusMuted("Music"), false, "a bus's default mute");

		Settings.Quality = QualityPreset.Low;
		Expect(Settings.Quality, QualityPreset.Low, "Quality");
		Settings.MasterVolume = 0.5f;
		Expect(Settings.MasterVolume, 0.5f, "MasterVolume");
		Settings.MasterVolume = 3.0f;
		Expect(Settings.MasterVolume, 1.0f, "MasterVolume brought within");
		Settings.MasterVolume = 0.5f;
		Settings.SetBusVolume("Music", -6.0f);
		Expect(Settings.GetBusVolume("MUSIC"), -6.0f, "SetBusVolume");
		Settings.SetBusMuted("Music", true);
		Expect(Settings.IsBusMuted("music"), true, "SetBusMuted");

		Settings.WindowMode = WindowMode.Fullscreen;
		Expect(Settings.WindowMode, WindowMode.Fullscreen, "WindowMode");
		Settings.SetWindowSize(10, 100_000);
		Expect((Settings.WindowWidth, Settings.WindowHeight), (64, 16384), "SetWindowSize brought within");
		Settings.SetWindowSize(400, 100);
		Settings.WindowMode = WindowMode.Windowed;
		Settings.VSync = false;
		Expect((Settings.WindowWidth, Settings.WindowHeight, Settings.WindowMode, Settings.VSync), (400, 100, WindowMode.Windowed, false),
			"the window");

		Expect(Settings.Has("LinkedBrakes"), false, "Has before Set");
		Expect(Settings.Get("LinkedBrakes", false), false, "Get's fallback");
		Settings.Set("LinkedBrakes", true);
		Settings.Set("Share", 0.3f);
		Expect(Settings.Get("LinkedBrakes", false), true, "Get after Set");
		Expect(Settings.Get("Share", 0.0f), 0.3f, "Get of a float");
		// A value of another type answers the fallback, warned about once.
		Expect(Settings.Get("Share", 2), 2, "Get of another type");
		Expect(Settings.Get("Share", 3), 3, "Get of another type again");
		Expect(Settings.Remove("Share"), true, "Remove");
		Expect(Settings.Remove("Share"), false, "Remove again");
		Refuses(() => Settings.Set("Nan", double.NaN), "Set of NaN");
		Refuses(() => Settings.Set("", 1), "Set of an empty name");
		// One value larger than the file may be is refused, and the rest stay.
		Refuses(() => Settings.Set("Huge", Enumerable.Repeat(new string('x', 512), 4096).ToArray()), "Set past the file's size");
		Expect(Settings.Has("Huge"), false, "a refused value is not kept");

		Expect(Settings.Save(), Available, "Save");
		Name = m_Failures.Count == 0 ? "Settings:ok" : "Settings:" + m_Failures[0];
	}

	private void Expect<T>(T actual, T expected, string what)
	{
		if (!EqualityComparer<T>.Default.Equals(actual, expected))
		{
			m_Failures.Add($"{what}: {actual} != {expected}");
		}
	}

	private void Refuses(Action action, string what)
	{
		try
		{
			action();
			m_Failures.Add($"{what} was not refused");
		}
		catch (ArgumentException)
		{
		}
	}
}
