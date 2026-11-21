using System;
using System.Collections.Generic;
using Strada.Interop;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The copies a read-back of changes compares running values with (Docs/Architecture.md §11, "Long lists"): plain values bit
// for bit, references by what their stored form keeps, lists element by element whatever list holds them.
public sealed class ReadBackCopyTests
{
	private static ReadBackCopy Take(Type type, object? value)
	{
		ReadBackCopy copy = ReadBackCopy.Create(type);
		copy.Take(value);
		return copy;
	}

	[Fact]
	public void ACopyMatchesNothingBeforeItTakesAValue()
	{
		Assert.False(ReadBackCopy.Create(typeof(float)).Matches(0.0f));
		Assert.False(ReadBackCopy.Create(typeof(float[])).Matches(Array.Empty<float>()));
		Assert.False(ReadBackCopy.Create(typeof(string)).Matches(null));
	}

	[Fact]
	public void FloatsCompareByTheirBits()
	{
		Assert.True(Take(typeof(float), 1.5f).Matches(1.5f));
		Assert.False(Take(typeof(float), 1.5f).Matches(1.25f));
		// A NaN that stays is unchanged; 0 and -0 differ.
		Assert.True(Take(typeof(float), float.NaN).Matches(float.NaN));
		Assert.False(Take(typeof(float), 0.0f).Matches(-0.0f));
		Assert.True(Take(typeof(double[]), new[] { double.NaN, 2.0 }).Matches(new[] { double.NaN, 2.0 }));
		Assert.False(Take(typeof(double[]), new[] { 0.0 }).Matches(new[] { -0.0 }));
	}

	[Fact]
	public void AValueOfAnotherTypeDiffers()
	{
		Assert.False(Take(typeof(int), 3).Matches(3u));
		Assert.False(Take(typeof(int), 3).Matches(null));
		Assert.False(Take(typeof(float[]), new[] { 1.0f }).Matches(new[] { 1.0 }));
	}

	[Fact]
	public void ListsCompareElementByElementWhateverListHoldsThem()
	{
		float[] taken = [1.0f, 2.0f, 3.0f];
		ReadBackCopy copy = Take(typeof(float[]), taken);
		// The copy is of the elements: the running array changed in place differs.
		taken[1] = 5.0f;
		Assert.False(copy.Matches(taken));
		Assert.True(copy.Matches(new[] { 1.0f, 2.0f, 3.0f }));
		Assert.False(copy.Matches(new[] { 1.0f, 2.0f }));
		Assert.False(copy.Matches(new[] { 1.0f, 2.0f, 3.0f, 4.0f }));

		ReadBackCopy list = Take(typeof(List<float>), new List<float> { 1.0f, 2.0f });
		Assert.True(list.Matches(new List<float> { 1.0f, 2.0f }));
		Assert.False(list.Matches(new List<float> { 1.0f }));
		// A null list is the empty list, as the stored form has it.
		ReadBackCopy empty = Take(typeof(List<float>), null);
		Assert.True(empty.Matches(new List<float>()));
		Assert.True(empty.Matches(null));
		Assert.False(empty.Matches(new List<float> { 0.0f }));
	}

	[Fact]
	public void ACopyTakenAgainHoldsTheNewValueAlone()
	{
		ReadBackCopy copy = Take(typeof(string[]), new[] { "a", "b", "c" });
		copy.Take(new[] { "a" });
		Assert.True(copy.Matches(new[] { "a" }));
		Assert.False(copy.Matches(new[] { "a", "b", "c" }));
		copy.Take(new[] { "x", "y", "z", "w" });
		Assert.True(copy.Matches(new[] { "x", "y", "z", "w" }));
	}

	[Fact]
	public void MathStructsAndEnumsCompareByTheirBits()
	{
		Assert.True(Take(typeof(Vector3[]), new[] { new Vector3(1.0f, 2.0f, 3.0f) }).Matches(new[] { new Vector3(1.0f, 2.0f, 3.0f) }));
		Assert.False(Take(typeof(Vector3[]), new[] { new Vector3(1.0f, 2.0f, 3.0f) }).Matches(new[] { new Vector3(1.0f, 2.0f, 4.0f) }));
		// A quaternion is compared as it runs, not as it is stored (of length one).
		Assert.True(Take(typeof(Quaternion), new Quaternion(0.0f, 0.0f, 0.0f, 0.0f)).Matches(new Quaternion(0.0f, 0.0f, 0.0f, 0.0f)));
		Assert.False(Take(typeof(Quaternion), new Quaternion(0.0f, 0.0f, 0.0f, 0.0f)).Matches(Quaternion.Identity));
		Assert.True(Take(typeof(List<ForceMode>), new List<ForceMode> { ForceMode.Impulse }).Matches(new List<ForceMode> { ForceMode.Impulse }));
		Assert.False(Take(typeof(ForceMode), ForceMode.Impulse).Matches(ForceMode.Force));
		Assert.True(Take(typeof(AssetHandle), new AssetHandle(12)).Matches(new AssetHandle(12)));
	}

	[Fact]
	public void ReferencesCompareByWhatTheirStoredFormKeeps()
	{
		Assert.True(Take(typeof(string), "Hello").Matches(string.Concat("Hel", "lo")));
		Assert.False(Take(typeof(string), "Hello").Matches("hello"));
		Assert.True(Take(typeof(string), null).Matches(null));
		Assert.False(Take(typeof(string), null).Matches(string.Empty));
		// Entities and assets by their IDs, whichever object holds them.
		Assert.True(Take(typeof(Entity[]), new[] { new Entity(8), null }).Matches(new[] { new Entity(8), null }));
		Assert.False(Take(typeof(Entity[]), new[] { new Entity(8) }).Matches(new[] { new Entity(9) }));
		Assert.False(Take(typeof(Entity), new Entity(8)).Matches(null));
		Assert.True(Take(typeof(Prefab), new Prefab(new AssetHandle(4))).Matches(new Prefab(new AssetHandle(4))));
		Assert.False(Take(typeof(Prefab), new Prefab(new AssetHandle(4))).Matches(new Prefab(new AssetHandle(5))));
	}

	[Fact]
	public void TheCountOfAValueIsItsListsLength()
	{
		Assert.Equal(3, ReadBackCopy.CountOf(new[] { 1, 2, 3 }));
		Assert.Equal(1, ReadBackCopy.CountOf(new List<string> { "a" }));
		Assert.Equal(0, ReadBackCopy.CountOf(null));
		Assert.Equal(0, ReadBackCopy.CountOf(4.0f));
	}
}
