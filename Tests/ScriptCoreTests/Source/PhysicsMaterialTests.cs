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

// The scripting API's physics material asset (Strada.PhysicsMaterial) and the material of raycast hits without an engine:
// PhysicsMaterial_Read and Physics_Raycast are served by fakes of the engine's bindings, so that the tests see what the
// runtime reads, when, and what scripts get. The engine's side is StradaTests' (Scene/SceneScriptingHostTests.cpp: scripts
// read physics materials and the materials raycasts hit, and see their edits). The fake bindings and the caches they fill are
// the process's, so the tests run alone.
[Collection(ProcessStaticsCollection.Name)]
public sealed unsafe class PhysicsMaterialTests : IDisposable
{
	// What the fake engine holds: per handle, the version and the values of a physics material asset.
	private sealed record FakeMaterial(ulong Version, NativePhysicsMaterial Values, string Surface);

	private static readonly Dictionary<ulong, FakeMaterial> s_Materials = [];
	// Every call of PhysicsMaterial_Read, with the version the runtime held, and how it was answered.
	private static readonly List<(ulong Handle, ulong HeldVersion, PhysicsMaterialReadStatus Status)> s_Reads = [];
	// The surface's text of the last answer, kept alive while the runtime copies it.
	private static byte[] s_Surface = [];
	// The material the fake raycast reports for its hit.
	private static ulong s_HitMaterial;

	private readonly delegate* unmanaged<ulong, ulong, NativePhysicsMaterial*, byte**, int> m_PreviousRead;
	private readonly delegate* unmanaged<Vector3*, Vector3*, float, uint, NativeRaycastHit*, byte> m_PreviousRaycast;

	public PhysicsMaterialTests()
	{
		m_PreviousRead = InternalCalls.PhysicsMaterial_Read;
		m_PreviousRaycast = InternalCalls.Physics_Raycast;
		InternalCalls.PhysicsMaterial_Read = &Read;
		InternalCalls.Physics_Raycast = &Raycast;
		s_Materials.Clear();
		s_Reads.Clear();
		s_HitMaterial = 0;
		DataAssets.Reset();
	}

	public void Dispose()
	{
		InternalCalls.PhysicsMaterial_Read = m_PreviousRead;
		InternalCalls.Physics_Raycast = m_PreviousRaycast;
		s_Materials.Clear();
		s_Reads.Clear();
		DataAssets.Reset();
	}

	// PhysicsMaterial_Read as ScriptBindingsAssets.cpp answers: Failed for no material, Unchanged for the held version.
	[UnmanagedCallersOnly]
	private static int Read(ulong handle, ulong heldVersion, NativePhysicsMaterial* values, byte** surface)
	{
		PhysicsMaterialReadStatus status = Answer(handle, heldVersion, values, surface);
		s_Reads.Add((handle, heldVersion, status));
		return (int)status;
	}

	private static PhysicsMaterialReadStatus Answer(ulong handle, ulong heldVersion, NativePhysicsMaterial* values, byte** surface)
	{
		if (!s_Materials.TryGetValue(handle, out FakeMaterial? material))
		{
			return PhysicsMaterialReadStatus.Failed;
		}
		values->Version = material.Version;
		if (material.Version == heldVersion)
		{
			return PhysicsMaterialReadStatus.Unchanged;
		}
		*values = material.Values;
		values->Version = material.Version;
		// A pinned array never moves: the text stays where the answer points until the next answer, as the engine's does
		// while its asset lives.
		byte[] text = Encoding.UTF8.GetBytes(material.Surface);
		s_Surface = GC.AllocateArray<byte>(text.Length, pinned: true);
		text.CopyTo(s_Surface, 0);
		values->SurfaceLength = s_Surface.Length;
		*surface = (byte*)Unsafe.AsPointer(ref MemoryMarshal.GetArrayDataReference(s_Surface));
		return PhysicsMaterialReadStatus.Read;
	}

	// Physics_Raycast as ScriptBindingsRuntime.cpp answers: a hit 2 along the ray on entity 3, naming s_HitMaterial.
	[UnmanagedCallersOnly]
	private static byte Raycast(Vector3* origin, Vector3* direction, float maxDistance, uint layerMask, NativeRaycastHit* hit)
	{
		*hit = new NativeRaycastHit
		{
			Entity = 3,
			Point = *origin + (*direction * 2.0f),
			Normal = Vector3.Up,
			Distance = 2.0f,
			Material = s_HitMaterial,
		};
		return 1;
	}

	// FeatureTest's PhysicsMaterials/Rubber.sphysmat: every field away from its default.
	private static FakeMaterial Rubber(ulong version) => new(version,
		new NativePhysicsMaterial
		{
			Friction = 1.1f,
			Restitution = 0.35f,
			FrictionCombine = (int)PhysicsMaterialCombine.Maximum,
			RestitutionCombine = (int)PhysicsMaterialCombine.Average,
			RollingResistance = 0.015f,
			ImpactSound = 9906031496213900225,
			ImpactParticles = 17000004240335447843,
		}, "Rubber");

	[Fact]
	public void AMaterialReadsEveryValueOfTheAsset()
	{
		s_Materials[7] = Rubber(11);
		PhysicsMaterial material = new(new AssetHandle(7));
		Assert.Equal(1.1f, material.Friction);
		Assert.Equal(0.35f, material.Restitution);
		Assert.Equal(PhysicsMaterialCombine.Maximum, material.FrictionCombine);
		Assert.Equal(PhysicsMaterialCombine.Average, material.RestitutionCombine);
		Assert.Equal(0.015f, material.RollingResistance);
		Assert.Equal("Rubber", material.Surface);
		Assert.Equal(new AudioClip(new AssetHandle(9906031496213900225)), material.ImpactSound);
		Assert.Equal(new Prefab(new AssetHandle(17000004240335447843)), material.ImpactParticles);
		Assert.Single(s_Reads);
		Assert.Equal((7ul, 0ul, PhysicsMaterialReadStatus.Read), s_Reads[0]);
	}

	[Fact]
	public void ReadingAMemberAllocatesNothingAndCrossesNoBoundary()
	{
		s_Materials[7] = Rubber(11);
		PhysicsMaterial material = new(new AssetHandle(7));
		float sum = material.Friction;
		long before = GC.GetAllocatedBytesForCurrentThread();
		int surfaces = 0;
		for (int index = 0; index < 100; index++)
		{
			sum += material.Friction + material.Restitution + material.RollingResistance;
			surfaces += material.Surface.Length + (material.ImpactSound != null ? 1 : 0) + (material.ImpactParticles != null ? 1 : 0) +
				(int)material.FrictionCombine + (int)material.RestitutionCombine;
		}
		Assert.Equal(before, GC.GetAllocatedBytesForCurrentThread());
		Assert.True(float.IsFinite(sum));
		Assert.Equal(100 * (6 + 2 + 4 + 1), surfaces);
		Assert.Single(s_Reads);
	}

	[Fact]
	public void ReferencesToOneAssetShareOneCopy()
	{
		s_Materials[7] = Rubber(11);
		s_Materials[8] = new FakeMaterial(12, new NativePhysicsMaterial { Friction = 0.05f }, "Ice");
		Assert.Equal("Rubber", new PhysicsMaterial(new AssetHandle(7)).Surface);
		Assert.Equal(1.1f, new PhysicsMaterial(new AssetHandle(7)).Friction);
		Assert.Equal("Ice", new PhysicsMaterial(new AssetHandle(8)).Surface);
		Assert.Equal([7ul, 8ul], s_Reads.Select(static read => read.Handle));
		Assert.Equal(2, PhysicsMaterialCache.EntryCount);
	}

	[Fact]
	public void AfterAnInvalidationTheRuntimeAsksAgainAndCopiesOnlyANewVersion()
	{
		s_Materials[7] = Rubber(11);
		PhysicsMaterial material = new(new AssetHandle(7));
		Assert.Equal("Rubber", material.Surface);
		PhysicsMaterialValues read = PhysicsMaterialCache.Find(material.Handle).Values;

		// Another data asset changed: the material asks with the version it holds, and keeps its copy.
		DataAssets.Invalidate();
		Assert.Equal(1.1f, material.Friction);
		Assert.Equal((7ul, 11ul, PhysicsMaterialReadStatus.Unchanged), s_Reads[^1]);
		Assert.Same(read, PhysicsMaterialCache.Find(material.Handle).Values);
		Assert.Equal(2, s_Reads.Count);

		// The material itself changed: the next use after the engine says so reads the new values.
		s_Materials[7] = new FakeMaterial(13, new NativePhysicsMaterial { Friction = 0.4f, RestitutionCombine = (int)PhysicsMaterialCombine.Minimum },
			"Gravel");
		Assert.Equal(1.1f, material.Friction);
		DataAssets.Invalidate();
		Assert.Equal(0.4f, material.Friction);
		Assert.Equal("Gravel", material.Surface);
		Assert.Equal(PhysicsMaterialCombine.Minimum, material.RestitutionCombine);
		Assert.Null(material.ImpactSound);
		Assert.Null(material.ImpactParticles);
		Assert.Equal((7ul, 11ul, PhysicsMaterialReadStatus.Read), s_Reads[^1]);
		Assert.Equal(13ul, PhysicsMaterialCache.Find(material.Handle).Version);
	}

	[Fact]
	public void AMaterialThatCannotBeReadHasANewMaterialsValuesAndAsksAgainOnlyAfterAnInvalidation()
	{
		PhysicsMaterial missing = new(new AssetHandle(5));
		Assert.Equal(0.6f, missing.Friction);
		Assert.Equal(0.0f, missing.Restitution);
		Assert.Equal(PhysicsMaterialCombine.GeometricMean, missing.FrictionCombine);
		Assert.Equal(PhysicsMaterialCombine.Maximum, missing.RestitutionCombine);
		Assert.Equal(0.0f, missing.RollingResistance);
		Assert.Equal(string.Empty, missing.Surface);
		Assert.Null(missing.ImpactSound);
		Assert.Null(missing.ImpactParticles);
		Assert.Single(s_Reads);
		Assert.Equal(PhysicsMaterialReadStatus.Failed, s_Reads[0].Status);

		s_Materials[5] = Rubber(30);
		DataAssets.Invalidate();
		Assert.Equal("Rubber", missing.Surface);
		Assert.Equal((5ul, 0ul, PhysicsMaterialReadStatus.Read), s_Reads[^1]);
	}

	[Fact]
	public void AResetForgetsTheCopiesAndReferencesReadAgain()
	{
		s_Materials[7] = Rubber(11);
		PhysicsMaterial material = new(new AssetHandle(7));
		Assert.Equal(1.1f, material.Friction);
		DataAssets.Reset();
		Assert.Equal(0, PhysicsMaterialCache.EntryCount);
		Assert.Equal(1.1f, material.Friction);
		Assert.Equal(2, s_Reads.Count);
		Assert.Equal(1.1f, new PhysicsMaterial(new AssetHandle(7)).Friction);
		Assert.Equal(3, s_Reads.Count);
	}

	[Fact]
	public void RaycastHitsNameTheMaterialHitAndShareOneReferencePerMaterial()
	{
		s_Materials[7] = Rubber(11);
		Assert.True(Physics.Raycast(Vector3.Zero, Vector3.Forward, 10.0f, out RaycastHit plain));
		Assert.Null(plain.Material);

		s_HitMaterial = 7;
		Assert.True(Physics.Raycast(Vector3.Zero, Vector3.Forward, 10.0f, out RaycastHit first));
		Assert.Equal(new PhysicsMaterial(new AssetHandle(7)), first.Material);
		Assert.Equal("Rubber", first.Material!.Surface);
		// Later hits on the material share its reference: a hit on a material allocates no more than one on none (the hit's
		// Entity of an entity without a script is the only allocation of both).
		long before = GC.GetAllocatedBytesForCurrentThread();
		Assert.True(Physics.Raycast(Vector3.Zero, Vector3.Forward, 10.0f, out RaycastHit second));
		long withMaterial = GC.GetAllocatedBytesForCurrentThread() - before;
		s_HitMaterial = 0;
		before = GC.GetAllocatedBytesForCurrentThread();
		Assert.True(Physics.Raycast(Vector3.Zero, Vector3.Forward, 10.0f, out _));
		Assert.Equal(GC.GetAllocatedBytesForCurrentThread() - before, withMaterial);
		Assert.Same(first.Material, second.Material);
		// A reference made otherwise shares the copy the hits read.
		Assert.Equal(1.1f, new PhysicsMaterial(new AssetHandle(7)).Friction);
		Assert.Single(s_Reads);
		Assert.Equal(1, PhysicsMaterialCache.EntryCount);
	}

	[Fact]
	public void PhysicsMaterialsAreAnAssetTypeOfGraphsWhoseMembersArePureReads()
	{
		VisualType type = VisualScriptTesting.Library.FindType("Strada.PhysicsMaterial")!;
		Assert.Equal(TypeKind.Asset, type.Kind);
		Assert.Equal("PhysicsMaterial", type.AssetType);
		Assert.True(type.HasLiteral);
		Assert.Equal(TypeKind.Enum, VisualScriptTesting.Library.FindType("Strada.PhysicsMaterialCombine")!.Kind);
		// Every member is a getter, so a pure node without flow pins; scripts and graphs never set a project's material.
		string[] members = ["Friction", "Restitution", "FrictionCombine", "RestitutionCombine", "RollingResistance", "Surface", "ImpactSound",
			"ImpactParticles"];
		foreach (string member in members)
		{
			NodeDefinition node = VisualScriptTesting.LibraryNode($"Get:Strada.PhysicsMaterial.{member}");
			Assert.True(node.Pure, member);
			Assert.Equal(NodeKind.Pure, node.Kind);
			Assert.Equal(["In:Target", "Out:Value"], node.Pins.Select(static pin => $"{pin.Direction}:{pin.Name}"));
			Assert.Null(VisualScriptTesting.Library.FindNode($"Set:Strada.PhysicsMaterial.{member}"));
		}
		NodeDefinition material = VisualScriptTesting.LibraryNode("Get:Strada.RaycastHit.Material");
		Assert.True(material.Pure);
		Assert.Equal("Strada.PhysicsMaterial", material.Pins.Single(static pin => pin.Name == "Value").Type.Id);
	}
}
