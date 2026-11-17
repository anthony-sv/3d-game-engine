using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The scripting API's mixer asset (Strada.Mixer) and the static Strada.Audio without an engine: Mixer_Read is served by a
// fake of the engine's binding, so that the tests see what the runtime reads, when, and what scripts get. The engine's side is
// StradaTests' (Scene/SceneScriptingApiTests.cpp: scripts play one-shots and clips at points and set the game's bus values;
// FeatureTest's AudioTests read its mixer). The fake binding and the caches it fills are the process's, so the tests run
// alone.
[Collection(ProcessStaticsCollection.Name)]
public sealed unsafe class MixerTests : IDisposable
{
	// One bus of a fake mixer asset.
	private sealed record FakeBus(string Name, string Parent, float VolumeDb, bool Muted);

	// What the fake engine holds: per handle, the version and the buses of a mixer asset.
	private sealed record FakeMixer(ulong Version, FakeBus[] Buses);

	private static readonly Dictionary<ulong, FakeMixer> s_Mixers = [];
	// Every call of Mixer_Read, with the version the runtime held, the room it gave, and how it was answered.
	private static readonly List<(ulong Handle, ulong HeldVersion, int Capacity, MixerReadStatus Status)> s_Reads = [];
	// The texts of the last answer, kept alive and in place while the runtime copies them.
	private static readonly List<byte[]> s_Texts = [];

	private readonly delegate* unmanaged<ulong, ulong, NativeMixerHeader*, NativeMixerBus*, int, int> m_PreviousRead;

	public MixerTests()
	{
		m_PreviousRead = InternalCalls.Mixer_Read;
		InternalCalls.Mixer_Read = &Read;
		s_Mixers.Clear();
		s_Reads.Clear();
		s_Texts.Clear();
		DataAssets.Reset();
	}

	public void Dispose()
	{
		InternalCalls.Mixer_Read = m_PreviousRead;
		s_Mixers.Clear();
		s_Reads.Clear();
		s_Texts.Clear();
		DataAssets.Reset();
	}

	// Mixer_Read as ScriptBindingsAssets.cpp answers: Failed for no mixer, Unchanged for the held version, TooSmall when the
	// buses do not fit.
	[UnmanagedCallersOnly]
	private static int Read(ulong handle, ulong heldVersion, NativeMixerHeader* header, NativeMixerBus* buses, int capacity)
	{
		MixerReadStatus status = Answer(handle, heldVersion, header, buses, capacity);
		s_Reads.Add((handle, heldVersion, capacity, status));
		return (int)status;
	}

	private static MixerReadStatus Answer(ulong handle, ulong heldVersion, NativeMixerHeader* header, NativeMixerBus* buses, int capacity)
	{
		if (!s_Mixers.TryGetValue(handle, out FakeMixer? mixer))
		{
			return MixerReadStatus.Failed;
		}
		header->Version = mixer.Version;
		if (mixer.Version == heldVersion)
		{
			return MixerReadStatus.Unchanged;
		}
		header->BusCount = mixer.Buses.Length;
		if (capacity < mixer.Buses.Length)
		{
			return MixerReadStatus.TooSmall;
		}
		s_Texts.Clear();
		for (int index = 0; index < mixer.Buses.Length; index++)
		{
			FakeBus bus = mixer.Buses[index];
			buses[index] = new NativeMixerBus
			{
				Name = Pin(bus.Name, out int nameLength),
				NameLength = nameLength,
				Parent = Pin(bus.Parent, out int parentLength),
				ParentLength = parentLength,
				VolumeDb = bus.VolumeDb,
				Muted = bus.Muted ? (byte)1 : (byte)0,
			};
		}
		return MixerReadStatus.Read;
	}

	// A pinned array never moves: the text stays where the answer points until the next answer, as the engine's does while
	// its asset lives.
	private static byte* Pin(string text, out int length)
	{
		byte[] bytes = Encoding.UTF8.GetBytes(text);
		byte[] pinned = GC.AllocateArray<byte>(Math.Max(bytes.Length, 1), pinned: true);
		bytes.CopyTo(pinned, 0);
		s_Texts.Add(pinned);
		length = bytes.Length;
		return (byte*)Unsafe.AsPointer(ref MemoryMarshal.GetArrayDataReference(pinned));
	}

	// FeatureTest's Audio/FeatureTest.smixer: Master over Music and Effects, Voices under Effects.
	private static FakeMixer FeatureTestMixer(ulong version) => new(version,
	[
		new FakeBus("Master", "", -1.0f, false), new FakeBus("Music", "Master", -6.0f, false), new FakeBus("Effects", "Master", 0.0f, true),
		new FakeBus("Voices", "Effects", 3.0f, false),
	]);

	[Fact]
	public void AMixerReadsItsBusesByPlaceAndByNameInAnyLetterCase()
	{
		s_Mixers[7] = FeatureTestMixer(11);
		Mixer mixer = new(new AssetHandle(7));
		Assert.Equal(4, mixer.BusCount);
		Assert.Equal(["Master", "Music", "Effects", "Voices"], Enumerable.Range(0, mixer.BusCount).Select(mixer.GetBusName));
		Assert.Equal(string.Empty, mixer.GetBusName(-1));
		Assert.Equal(string.Empty, mixer.GetBusName(4));
		Assert.True(mixer.HasBus("voices"));
		Assert.True(mixer.HasBus("MUSIC"));
		Assert.False(mixer.HasBus("Ambience"));
		Assert.False(mixer.HasBus("Musi"));
		Assert.Equal("Effects", mixer.GetBusParent("Voices"));
		Assert.Equal(string.Empty, mixer.GetBusParent("Master"));
		Assert.Equal(string.Empty, mixer.GetBusParent("Ambience"));
		Assert.Equal(-6.0f, mixer.GetBusVolume("music"));
		Assert.Equal(3.0f, mixer.GetBusVolume("Voices"));
		Assert.Equal(0.0f, mixer.GetBusVolume("Ambience"));
		Assert.True(mixer.IsBusMuted("effects"));
		Assert.False(mixer.IsBusMuted("Voices"));
		Assert.False(mixer.IsBusMuted("Ambience"));
		Assert.Single(s_Reads);
		Assert.Equal((7ul, 0ul, 32, MixerReadStatus.Read), s_Reads[0]);
	}

	[Fact]
	public void TheEmptyNameIsMastersAndOnlyAsciiLettersMatchInAnyCase()
	{
		s_Mixers[7] = new FakeMixer(3, [new FakeBus("Master", "", -2.5f, true), new FakeBus("Straße", "Master", -1.0f, false)]);
		Mixer mixer = new(new AssetHandle(7));
		Assert.True(mixer.HasBus(string.Empty));
		Assert.Equal(-2.5f, mixer.GetBusVolume(string.Empty));
		Assert.True(mixer.IsBusMuted(string.Empty));
		// The engine folds ASCII letters only (MakeMixerBusKey), so a name differs in any other letter.
		Assert.True(mixer.HasBus("STRAßE"));
		Assert.False(mixer.HasBus("STRASSE"));
		Assert.Throws<ArgumentNullException>(() => mixer.HasBus(null!));
	}

	[Fact]
	public void ReadingAMemberAllocatesNothingAndCrossesNoBoundary()
	{
		s_Mixers[7] = FeatureTestMixer(11);
		Mixer mixer = new(new AssetHandle(7));
		int sum = mixer.BusCount;
		long before = GC.GetAllocatedBytesForCurrentThread();
		float volumes = 0.0f;
		for (int index = 0; index < 100; index++)
		{
			sum += mixer.GetBusName(index % 4).Length + mixer.GetBusParent("Voices").Length + (mixer.HasBus("music") ? 1 : 0) +
				(mixer.IsBusMuted("Effects") ? 1 : 0);
			volumes += mixer.GetBusVolume("Music");
		}
		Assert.Equal(before, GC.GetAllocatedBytesForCurrentThread());
		Assert.Equal(4 + (25 * (6 + 5 + 7 + 6)) + (100 * (7 + 1 + 1)), sum);
		Assert.Equal(-600.0f, volumes);
		Assert.Single(s_Reads);
	}

	[Fact]
	public void AfterAnInvalidationTheRuntimeAsksAgainAndCopiesOnlyANewVersion()
	{
		s_Mixers[7] = FeatureTestMixer(11);
		Mixer mixer = new(new AssetHandle(7));
		Assert.Equal(4, mixer.BusCount);
		MixerValues read = MixerCache.Find(mixer.Handle).Values;

		// Another data asset changed: the mixer asks with the version it holds, and keeps its copy.
		DataAssets.Invalidate();
		Assert.True(mixer.HasBus("Voices"));
		Assert.Equal((7ul, 11ul, 32, MixerReadStatus.Unchanged), s_Reads[^1]);
		Assert.Same(read, MixerCache.Find(mixer.Handle).Values);

		// The mixer itself changed (an undo removed Voices, a bus was renamed): the next use after the engine says so reads it.
		s_Mixers[7] = new FakeMixer(12, [new FakeBus("Master", "", 0.0f, false), new FakeBus("Ambience", "Master", -12.0f, false)]);
		Assert.True(mixer.HasBus("Voices"));
		DataAssets.Invalidate();
		Assert.False(mixer.HasBus("Voices"));
		Assert.Equal(-12.0f, mixer.GetBusVolume("Ambience"));
		Assert.Equal(2, mixer.BusCount);
		Assert.Equal((7ul, 11ul, 32, MixerReadStatus.Read), s_Reads[^1]);
		Assert.Equal(12ul, MixerCache.Find(mixer.Handle).Version);
	}

	[Fact]
	public void BusesThatDoNotFitAreReadAgainWithRoomForThem()
	{
		FakeBus[] buses = [new FakeBus("Master", "", 0.0f, false), .. Enumerable.Range(1, 39).Select(static index => new FakeBus($"Bus {index}", "Master", 0.0f, false))];
		s_Mixers[7] = new FakeMixer(5, buses);
		Mixer mixer = new(new AssetHandle(7));
		Assert.Equal(40, mixer.BusCount);
		Assert.Equal("Bus 39", mixer.GetBusName(39));
		Assert.Equal([(7ul, 0ul, 32, MixerReadStatus.TooSmall), (7ul, 0ul, 40, MixerReadStatus.Read)], s_Reads);
	}

	[Fact]
	public void AMixerThatCannotBeReadIsMasterAloneAndAsksAgainOnlyAfterAnInvalidation()
	{
		Mixer missing = new(new AssetHandle(5));
		Assert.Equal(1, missing.BusCount);
		Assert.Equal("Master", missing.GetBusName(0));
		Assert.True(missing.HasBus("master"));
		Assert.Equal(0.0f, missing.GetBusVolume("Master"));
		Assert.False(missing.IsBusMuted("Master"));
		Assert.Single(s_Reads);
		Assert.Equal(MixerReadStatus.Failed, s_Reads[0].Status);

		s_Mixers[5] = FeatureTestMixer(30);
		Assert.Equal(1, missing.BusCount);
		DataAssets.Invalidate();
		Assert.Equal(4, missing.BusCount);
		Assert.Equal((5ul, 0ul, 32, MixerReadStatus.Read), s_Reads[^1]);
	}

	[Fact]
	public void ReferencesToOneAssetShareOneCopyAndAResetForgetsIt()
	{
		s_Mixers[7] = FeatureTestMixer(11);
		Assert.Equal(4, new Mixer(new AssetHandle(7)).BusCount);
		Assert.True(new Mixer(new AssetHandle(7)).HasBus("Music"));
		Assert.Equal(1, MixerCache.EntryCount);
		Assert.Single(s_Reads);
		DataAssets.Reset();
		Assert.Equal(0, MixerCache.EntryCount);
		Assert.Equal(4, new Mixer(new AssetHandle(7)).BusCount);
		Assert.Equal(2, s_Reads.Count);
	}

	[Fact]
	public void MixersAreAnAssetTypeOfGraphsWhoseQueriesArePure()
	{
		VisualType type = VisualScriptTesting.Library.FindType("Strada.Mixer")!;
		Assert.Equal(TypeKind.Asset, type.Kind);
		Assert.Equal("Mixer", type.AssetType);
		Assert.True(type.HasLiteral);
		// Scripts and graphs never write a project's mixer: its members are reads, so pure nodes without flow pins.
		NodeDefinition count = VisualScriptTesting.LibraryNode("Get:Strada.Mixer.BusCount");
		Assert.True(count.Pure);
		Assert.Null(VisualScriptTesting.Library.FindNode("Set:Strada.Mixer.BusCount"));
		foreach (string query in new[] { "GetBusName(int)", "HasBus(string)", "GetBusParent(string)", "GetBusVolume(string)", "IsBusMuted(string)" })
		{
			NodeDefinition node = VisualScriptTesting.LibraryNode($"Call:Strada.Mixer.{query}");
			Assert.True(node.Pure, query);
			Assert.Equal(NodeKind.Pure, node.Kind);
		}
		AssertPins(["In:Target", "In:Bus", "Out:Result"], VisualScriptTesting.LibraryNode("Call:Strada.Mixer.GetBusVolume(string)"));
	}

	[Fact]
	public void TheAudioApiIsNodesWithTheirPurityAndPinsRecorded()
	{
		// The game's values: reads are pure, writes and plays have flow pins.
		Assert.True(VisualScriptTesting.LibraryNode("Get:Strada.Audio.MasterVolume").Pure);
		Assert.False(VisualScriptTesting.LibraryNode("Set:Strada.Audio.MasterVolume").Pure);
		Assert.True(VisualScriptTesting.LibraryNode("Get:Strada.Audio.ActiveVoiceCount").Pure);
		Assert.Null(VisualScriptTesting.Library.FindNode("Set:Strada.Audio.ActiveVoiceCount"));
		Assert.True(VisualScriptTesting.LibraryNode("Call:Strada.Audio.GetBusVolume(string)").Pure);
		Assert.True(VisualScriptTesting.LibraryNode("Call:Strada.Audio.IsBusMuted(string)").Pure);
		AssertPins(["In:Execute", "Out:Then", "In:Bus", "In:Decibels"], VisualScriptTesting.LibraryNode("Call:Strada.Audio.SetBusVolume(string,float)"));
		AssertPins(["In:Execute", "Out:Then", "In:Bus", "In:Muted"], VisualScriptTesting.LibraryNode("Call:Strada.Audio.SetBusMuted(string,bool)"));
		NodeDefinition atPoint = VisualScriptTesting.LibraryNode("Call:Strada.Audio.PlayClipAtPoint(Strada.AudioClip,Strada.Vector3,float,string)");
		Assert.False(atPoint.Pure);
		AssertPins(["In:Execute", "Out:Then", "In:Clip", "In:Position", "In:Volume", "In:Bus", "Out:Result"], atPoint);

		// A source's one-shots, of its own clip and of another, and its new fields.
		NodeDefinition own = VisualScriptTesting.LibraryNode("Call:Strada.AudioSourceComponent.PlayOneShot(float)");
		Assert.False(own.Pure);
		AssertPins(["In:Execute", "Out:Then", "In:Target", "In:Volume", "Out:Result"], own);
		NodeDefinition given = VisualScriptTesting.LibraryNode("Call:Strada.AudioSourceComponent.PlayOneShot(Strada.AudioClip,float)");
		AssertPins(["In:Execute", "Out:Then", "In:Target", "In:Clip", "In:Volume", "Out:Result"], given);
		foreach (string field in new[] { "Bus", "Priority", "MaxVoices" })
		{
			Assert.True(VisualScriptTesting.LibraryNode($"Get:Strada.AudioSourceComponent.{field}").Pure, field);
			Assert.False(VisualScriptTesting.LibraryNode($"Set:Strada.AudioSourceComponent.{field}").Pure, field);
		}
	}

	// The node's pins, by direction and name, in any order: these names are what graphs link to.
	private static void AssertPins(string[] expected, NodeDefinition node)
	{
		Assert.Equal(expected.Order(StringComparer.Ordinal), node.Pins.Select(static pin => $"{pin.Direction}:{pin.Name}").Order(StringComparer.Ordinal));
	}

	[Fact]
	public void NullArgumentsThrow()
	{
		Assert.Throws<ArgumentNullException>(() => Audio.GetBusVolume(null!));
		Assert.Throws<ArgumentNullException>(() => Audio.SetBusVolume(null!, 0.0f));
		Assert.Throws<ArgumentNullException>(() => Audio.IsBusMuted(null!));
		Assert.Throws<ArgumentNullException>(() => Audio.SetBusMuted(null!, true));
		Assert.Throws<ArgumentNullException>(() => Audio.PlayClipAtPoint(null!, Vector3.Zero));
		Assert.Throws<ArgumentNullException>(() => Audio.PlayClipAtPoint(new AudioClip(new AssetHandle(3)), Vector3.Zero, 1.0f, null!));
	}
}
