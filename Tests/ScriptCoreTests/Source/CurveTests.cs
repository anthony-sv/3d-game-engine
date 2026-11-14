using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The scripting API's curve asset (Strada.Curve) without an engine: Curve_Read is served by a fake of the engine's binding
// over a table of curves, so that the tests see what the runtime reads, when, and what it evaluates. The engine's side is
// StradaTests' (Scene/SceneScriptingHostTests.cpp: scripts evaluate a curve asset as the engine does and see its edits).
// The fake binding and the caches it fills are the process's, so the tests run alone.
[Collection(ProcessStaticsCollection.Name)]
public sealed unsafe class CurveTests : IDisposable
{
	// What the fake engine holds: per handle, the version and the data of a curve asset.
	private sealed record FakeCurve(ulong Version, NativeCurveKey[] Keys, CurveExtrapolation Pre, CurveExtrapolation Post);

	private static readonly Dictionary<ulong, FakeCurve> s_Curves = [];
	// Every call of Curve_Read, with the version the runtime held, and how it was answered.
	private static readonly List<(ulong Handle, ulong HeldVersion, CurveReadStatus Status)> s_Reads = [];

	private readonly delegate* unmanaged<ulong, ulong, NativeCurveHeader*, NativeCurveKey*, int, int> m_PreviousRead;

	public CurveTests()
	{
		m_PreviousRead = InternalCalls.Curve_Read;
		InternalCalls.Curve_Read = &Read;
		s_Curves.Clear();
		s_Reads.Clear();
		DataAssets.Reset();
	}

	public void Dispose()
	{
		InternalCalls.Curve_Read = m_PreviousRead;
		s_Curves.Clear();
		s_Reads.Clear();
		DataAssets.Reset();
	}

	// Curve_Read as ScriptBindingsAssets.cpp answers: Failed for no curve, Unchanged for the held version, TooSmall when the
	// keys do not fit.
	[UnmanagedCallersOnly]
	private static int Read(ulong handle, ulong heldVersion, NativeCurveHeader* header, NativeCurveKey* keys, int capacity)
	{
		CurveReadStatus status = Answer(handle, heldVersion, header, keys, capacity);
		s_Reads.Add((handle, heldVersion, status));
		return (int)status;
	}

	private static CurveReadStatus Answer(ulong handle, ulong heldVersion, NativeCurveHeader* header, NativeCurveKey* keys, int capacity)
	{
		if (!s_Curves.TryGetValue(handle, out FakeCurve? curve))
		{
			return CurveReadStatus.Failed;
		}
		header->Version = curve.Version;
		if (curve.Version == heldVersion)
		{
			return CurveReadStatus.Unchanged;
		}
		header->KeyCount = curve.Keys.Length;
		header->PreExtrapolation = (int)curve.Pre;
		header->PostExtrapolation = (int)curve.Post;
		if (capacity < curve.Keys.Length)
		{
			return CurveReadStatus.TooSmall;
		}
		curve.Keys.CopyTo(new Span<NativeCurveKey>(keys, capacity));
		return CurveReadStatus.Read;
	}

	private static NativeCurveKey Key(float time, float value, CurveInterpolation interpolation = CurveInterpolation.Linear,
		float inSlope = float.NaN, float outSlope = float.NaN) =>
		new() { Time = time, Value = value, In = inSlope, Out = outSlope, Interpolation = (int)interpolation };

	// FeatureTest's Curves/Response.scurve: every field away from its default.
	private static FakeCurve Response(ulong version) => new(version,
		[Key(0.0f, 0.0f, CurveInterpolation.Cubic, outSlope: 0.5f), Key(0.25f, 0.125f), Key(0.5f, 0.5f, CurveInterpolation.Cubic, inSlope: 1.25f),
		Key(1.0f, 1.0f, CurveInterpolation.Constant)], CurveExtrapolation.Linear, CurveExtrapolation.PingPong);

	[Fact]
	public void ACurveEvaluatesTheAssetsKeysAndExtrapolations()
	{
		s_Curves[7] = Response(11);
		Curve curve = new(new AssetHandle(7));
		Assert.Equal(-0.5f, curve.Evaluate(-1.0f));
		Assert.Equal(0.0f, curve.Evaluate(0.0f));
		Assert.Equal(0.3125f, curve.Evaluate(0.375f));
		Assert.Equal(1.0f, curve.Evaluate(1.0f));
		Assert.Equal(0.5f, curve.Evaluate(1.5f));
		Assert.Equal(0.125f, curve.Evaluate(2.25f));
		// The managed copy is the runtime's evaluator over the keys as read: unset slopes stay automatic.
		CurveDefinition expected = new([0.0, 0.25, 0.5, 1.0], [0.0, 0.125, 0.5, 1.0], [double.NaN, double.NaN, 1.25, double.NaN],
			[0.5, double.NaN, double.NaN, double.NaN],
			[CurveInterpolation.Cubic, CurveInterpolation.Linear, CurveInterpolation.Cubic, CurveInterpolation.Constant],
			CurveExtrapolation.Linear, CurveExtrapolation.PingPong);
		for (int sample = -40; sample <= 80; sample++)
		{
			float time = sample * 0.03125f;
			Assert.Equal(expected.Evaluate(time), curve.Evaluate(time));
		}
		Assert.Single(s_Reads);
		Assert.Equal((7ul, 0ul, CurveReadStatus.Read), s_Reads[0]);
	}

	[Fact]
	public void EvaluatingReadsOnceAndThenAllocatesNothingAndCrossesNoBoundary()
	{
		s_Curves[7] = Response(11);
		Curve curve = new(new AssetHandle(7));
		float sum = curve.Evaluate(0.1f);
		long before = GC.GetAllocatedBytesForCurrentThread();
		for (int sample = -100; sample < 100; sample++)
		{
			sum += curve.Evaluate(sample * 0.037f);
		}
		Assert.Equal(before, GC.GetAllocatedBytesForCurrentThread());
		Assert.True(float.IsFinite(sum));
		Assert.Single(s_Reads);
	}

	[Fact]
	public void ReferencesToOneAssetShareOneCopy()
	{
		s_Curves[7] = Response(11);
		s_Curves[8] = new FakeCurve(12, [Key(0.0f, 4.0f)], CurveExtrapolation.Hold, CurveExtrapolation.Hold);
		Assert.Equal(0.125f, new Curve(new AssetHandle(7)).Evaluate(0.25f));
		Assert.Equal(0.125f, new Curve(new AssetHandle(7)).Evaluate(0.25f));
		Assert.Equal(4.0f, new Curve(new AssetHandle(8)).Evaluate(3.0f));
		Assert.Equal([7ul, 8ul], s_Reads.Select(static read => read.Handle));
		Assert.Equal(2, CurveCache.EntryCount);
	}

	[Fact]
	public void AfterAnInvalidationTheRuntimeAsksAgainAndCopiesOnlyANewVersion()
	{
		s_Curves[7] = Response(11);
		Curve curve = new(new AssetHandle(7));
		Assert.Equal(0.125f, curve.Evaluate(0.25f));
		CurveDefinition read = CurveCache.Find(curve.Handle).Definition;

		// Another data asset changed: the curve asks with the version it holds, and keeps its copy.
		DataAssets.Invalidate();
		Assert.Equal(0.125f, curve.Evaluate(0.25f));
		Assert.Equal((7ul, 11ul, CurveReadStatus.Unchanged), s_Reads[^1]);
		Assert.Same(read, CurveCache.Find(curve.Handle).Definition);
		Assert.Equal(0.125f, curve.Evaluate(0.25f));
		Assert.Equal(2, s_Reads.Count);

		// The curve itself changed: the next use reads the new data.
		s_Curves[7] = new FakeCurve(13, [Key(0.0f, 2.0f), Key(1.0f, 4.0f)], CurveExtrapolation.Hold, CurveExtrapolation.Hold);
		Assert.Equal(0.125f, curve.Evaluate(0.25f));
		DataAssets.Invalidate();
		Assert.Equal(2.5f, curve.Evaluate(0.25f));
		Assert.Equal((7ul, 11ul, CurveReadStatus.Read), s_Reads[^1]);
		Assert.Equal(13ul, CurveCache.Find(curve.Handle).Version);
	}

	[Fact]
	public void ACurveWithMoreKeysThanTheBufferIsReadAgainWithRoom()
	{
		NativeCurveKey[] keys = [.. Enumerable.Range(0, 300).Select(static index => Key(index, index * 2.0f))];
		s_Curves[9] = new FakeCurve(21, keys, CurveExtrapolation.Hold, CurveExtrapolation.Linear);
		Curve curve = new(new AssetHandle(9));
		Assert.Equal(299.0f, curve.Evaluate(149.5f));
		Assert.Equal(602.0f, curve.Evaluate(301.0f));
		Assert.Equal([CurveReadStatus.TooSmall, CurveReadStatus.Read], s_Reads.Select(static read => read.Status));
	}

	[Fact]
	public void ACurveThatCannotBeReadIsZeroAndAsksAgainOnlyAfterAnInvalidation()
	{
		Curve missing = new(new AssetHandle(5));
		Assert.Equal(0.0f, missing.Evaluate(1.0f));
		Assert.Equal(0.0f, missing.Evaluate(2.0f));
		Assert.Single(s_Reads);
		Assert.Equal(CurveReadStatus.Failed, s_Reads[0].Status);

		s_Curves[5] = new FakeCurve(30, [Key(0.0f, 6.0f)], CurveExtrapolation.Hold, CurveExtrapolation.Hold);
		DataAssets.Invalidate();
		Assert.Equal(6.0f, missing.Evaluate(1.0f));
		Assert.Equal((5ul, 0ul, CurveReadStatus.Read), s_Reads[^1]);
	}

	[Fact]
	public void AResetForgetsTheCopiesAndReferencesReadAgain()
	{
		s_Curves[7] = Response(11);
		Curve curve = new(new AssetHandle(7));
		Assert.Equal(0.125f, curve.Evaluate(0.25f));
		DataAssets.Reset();
		Assert.Equal(0, CurveCache.EntryCount);
		Assert.Equal(0.125f, curve.Evaluate(0.25f));
		Assert.Equal(2, s_Reads.Count);
		Assert.Equal(0.125f, new Curve(new AssetHandle(7)).Evaluate(0.25f));
		Assert.Equal(3, s_Reads.Count);
	}

	[Fact]
	public void CurvesAreAnAssetTypeOfGraphsAndEvaluateIsAPureNode()
	{
		VisualType type = VisualScriptTesting.Library.FindType("Strada.Curve")!;
		Assert.Equal(TypeKind.Asset, type.Kind);
		Assert.Equal("Curve", type.AssetType);
		Assert.True(type.HasLiteral);
		NodeDefinition evaluate = VisualScriptTesting.LibraryNode("Call:Strada.Curve.Evaluate(float)");
		Assert.True(evaluate.Pure);
		Assert.Equal(NodeKind.Pure, evaluate.Kind);
		Assert.Equal(["In:Target", "In:Time", "Out:Result"], evaluate.Pins.Select(static pin => $"{pin.Direction}:{pin.Name}"));
		Assert.Contains(VisualScriptTesting.Library.FindNode("Call:Strada.Assets.Load<T>(string)")!.Pins, static pin => pin.Name == "Path");
	}
}
