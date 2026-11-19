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

// The scripting API's vehicle definition asset (Strada.Vehicle) and its component's nodes without an engine: Vehicle_Read is
// served by a fake of the engine's binding, so that the tests see what the runtime reads, when, and what scripts get. The
// engine's side is StradaTests' (Scene/SceneScriptingVehicleTests.cpp: scripts ride vehicles and read their definitions). The
// fake binding and the cache it fills are the process's, so the tests run alone.
[Collection(ProcessStaticsCollection.Name)]
public sealed unsafe class VehicleTests : IDisposable
{
	// What the fake engine holds: per handle, the version, the values, the gears' ratios and the wheels' names of a definition.
	private sealed record FakeVehicle(ulong Version, NativeVehicle Values, float[] GearRatios, string Front, string Rear);

	private static readonly Dictionary<ulong, FakeVehicle> s_Vehicles = [];
	// Every call of Vehicle_Read, with the version the runtime held, the capacity it gave and how it was answered.
	private static readonly List<(ulong Handle, ulong HeldVersion, int Capacity, VehicleReadStatus Status)> s_Reads = [];
	// The names of the last answer, kept alive while the runtime copies them.
	private static byte[] s_FrontName = [];
	private static byte[] s_RearName = [];

	private readonly delegate* unmanaged<ulong, ulong, NativeVehicle*, float*, int, int> m_PreviousRead;

	public VehicleTests()
	{
		m_PreviousRead = InternalCalls.Vehicle_Read;
		InternalCalls.Vehicle_Read = &Read;
		s_Vehicles.Clear();
		s_Reads.Clear();
		DataAssets.Reset();
	}

	public void Dispose()
	{
		InternalCalls.Vehicle_Read = m_PreviousRead;
		s_Vehicles.Clear();
		s_Reads.Clear();
		DataAssets.Reset();
	}

	// Vehicle_Read as ScriptBindingsAssets.cpp answers: Failed for no definition, Unchanged for the held version.
	[UnmanagedCallersOnly]
	private static int Read(ulong handle, ulong heldVersion, NativeVehicle* values, float* gearRatios, int capacity)
	{
		VehicleReadStatus status = Answer(handle, heldVersion, values, gearRatios, capacity);
		s_Reads.Add((handle, heldVersion, capacity, status));
		return (int)status;
	}

	private static VehicleReadStatus Answer(ulong handle, ulong heldVersion, NativeVehicle* values, float* gearRatios, int capacity)
	{
		if (!s_Vehicles.TryGetValue(handle, out FakeVehicle? vehicle))
		{
			return VehicleReadStatus.Failed;
		}
		values->Version = vehicle.Version;
		if (vehicle.Version == heldVersion)
		{
			return VehicleReadStatus.Unchanged;
		}
		*values = vehicle.Values;
		values->Version = vehicle.Version;
		// Pinned arrays never move: the names stay where the answer points until the next answer, as the engine's do while
		// its asset lives.
		s_FrontName = Pin(vehicle.Front);
		s_RearName = Pin(vehicle.Rear);
		values->FrontName = (byte*)Unsafe.AsPointer(ref MemoryMarshal.GetArrayDataReference(s_FrontName));
		values->RearName = (byte*)Unsafe.AsPointer(ref MemoryMarshal.GetArrayDataReference(s_RearName));
		values->FrontNameLength = s_FrontName.Length;
		values->RearNameLength = s_RearName.Length;
		int gears = Math.Min(vehicle.GearRatios.Length, capacity);
		values->GearCount = gears;
		for (int gear = 0; gear < gears; gear++)
		{
			gearRatios[gear] = vehicle.GearRatios[gear];
		}
		return VehicleReadStatus.Read;
	}

	private static byte[] Pin(string text)
	{
		byte[] bytes = Encoding.UTF8.GetBytes(text);
		byte[] pinned = GC.AllocateArray<byte>(bytes.Length, pinned: true);
		bytes.CopyTo(pinned, 0);
		return pinned;
	}

	// FeatureTest's Vehicles/Bike.svehicle, its gearbox Manual.
	private static FakeVehicle Bike(ulong version) => new(version,
		new NativeVehicle
		{
			MaxTorque = 120.0f,
			MinRPM = 1500.0f,
			MaxRPM = 12000.0f,
			ShiftUpRPM = 10500.0f,
			ShiftDownRPM = 4000.0f,
			FinalDrive = 5.2f,
			MaxLeanAngle = 48.0f,
			TransmissionMode = (int)VehicleTransmissionMode.Manual,
		}, [2.6f, 1.9f, 1.5f, 1.25f, 1.1f, 1.0f], "Front", "Hinterrad");

	[Fact]
	public void ADefinitionReadsEveryValueOfTheAsset()
	{
		s_Vehicles[7] = Bike(11);
		Vehicle vehicle = new(new AssetHandle(7));
		Assert.Equal(VehicleTransmissionMode.Manual, vehicle.TransmissionMode);
		Assert.Equal(6, vehicle.GearCount);
		Assert.Equal(2.6f, vehicle.GetGearRatio(1));
		Assert.Equal(1.0f, vehicle.GetGearRatio(6));
		Assert.Equal(0.0f, vehicle.GetGearRatio(0));
		Assert.Equal(0.0f, vehicle.GetGearRatio(7));
		Assert.Equal(5.2f, vehicle.FinalDrive);
		Assert.Equal(120.0f, vehicle.MaxTorque);
		Assert.Equal(1500.0f, vehicle.MinRPM);
		Assert.Equal(12000.0f, vehicle.MaxRPM);
		Assert.Equal(10500.0f, vehicle.ShiftUpRPM);
		Assert.Equal(4000.0f, vehicle.ShiftDownRPM);
		Assert.Equal(48.0f, vehicle.MaxLeanAngle);
		Assert.Equal("Front", vehicle.GetWheelName(0));
		Assert.Equal("Hinterrad", vehicle.GetWheelName(1));
		Assert.Equal(string.Empty, vehicle.GetWheelName(2));
		Assert.Equal(string.Empty, vehicle.GetWheelName(-1));
		// One read, with room for every gear a definition that loads can have.
		Assert.Equal([(7ul, 0ul, 16, VehicleReadStatus.Read)], s_Reads);
	}

	[Fact]
	public void ReadingAMemberAllocatesNothingAndCrossesNoBoundary()
	{
		s_Vehicles[7] = Bike(11);
		Vehicle vehicle = new(new AssetHandle(7));
		float sum = vehicle.MaxRPM;
		long before = GC.GetAllocatedBytesForCurrentThread();
		int names = 0;
		for (int index = 0; index < 100; index++)
		{
			sum += vehicle.MaxRPM + vehicle.GetGearRatio(2) + vehicle.FinalDrive + vehicle.MaxLeanAngle;
			names += vehicle.GetWheelName(0).Length + vehicle.GearCount + (int)vehicle.TransmissionMode;
		}
		Assert.Equal(before, GC.GetAllocatedBytesForCurrentThread());
		Assert.True(float.IsFinite(sum));
		Assert.Equal(100 * (5 + 6 + 1), names);
		Assert.Single(s_Reads);
	}

	[Fact]
	public void AfterAnInvalidationTheRuntimeAsksAgainAndCopiesOnlyANewVersion()
	{
		s_Vehicles[7] = Bike(11);
		Vehicle vehicle = new(new AssetHandle(7));
		Assert.Equal(6, vehicle.GearCount);
		VehicleValues read = VehicleCache.Find(vehicle.Handle).Values;

		// Another data asset changed: the definition asks with the version it holds, and keeps its copy.
		DataAssets.Invalidate();
		Assert.Equal(12000.0f, vehicle.MaxRPM);
		Assert.Equal((7ul, 11ul, 16, VehicleReadStatus.Unchanged), s_Reads[^1]);
		Assert.Same(read, VehicleCache.Find(vehicle.Handle).Values);

		// The definition itself changed: the next use after the engine says so reads the new values.
		s_Vehicles[7] = new FakeVehicle(13, new NativeVehicle { MaxRPM = 16000.0f, TransmissionMode = (int)VehicleTransmissionMode.Automatic },
			[2.714f, 2.118f], "Vorne", "Hinten");
		Assert.Equal(12000.0f, vehicle.MaxRPM);
		DataAssets.Invalidate();
		Assert.Equal(16000.0f, vehicle.MaxRPM);
		Assert.Equal(VehicleTransmissionMode.Automatic, vehicle.TransmissionMode);
		Assert.Equal(2, vehicle.GearCount);
		Assert.Equal(2.118f, vehicle.GetGearRatio(2));
		Assert.Equal("Vorne", vehicle.GetWheelName(0));
		Assert.Equal((7ul, 11ul, 16, VehicleReadStatus.Read), s_Reads[^1]);
		Assert.Equal(13ul, VehicleCache.Find(vehicle.Handle).Version);
	}

	[Fact]
	public void ADefinitionThatCannotBeReadHasANewDefinitionsValuesAndAsksAgainOnlyAfterAnInvalidation()
	{
		Vehicle missing = new(new AssetHandle(5));
		Assert.Equal(VehicleTransmissionMode.Automatic, missing.TransmissionMode);
		Assert.Equal(6, missing.GearCount);
		Assert.Equal(2.27f, missing.GetGearRatio(1));
		Assert.Equal(4.825f, missing.FinalDrive);
		Assert.Equal(10000.0f, missing.MaxRPM);
		Assert.Equal("Front", missing.GetWheelName(0));
		Assert.Equal("Rear", missing.GetWheelName(1));
		Assert.Single(s_Reads);
		Assert.Equal(VehicleReadStatus.Failed, s_Reads[0].Status);

		s_Vehicles[5] = Bike(30);
		DataAssets.Invalidate();
		Assert.Equal("Hinterrad", missing.GetWheelName(1));
		Assert.Equal((5ul, 0ul, 16, VehicleReadStatus.Read), s_Reads[^1]);
	}

	[Fact]
	public void AResetForgetsTheCopiesAndReferencesReadAgain()
	{
		s_Vehicles[7] = Bike(11);
		Vehicle vehicle = new(new AssetHandle(7));
		Assert.Equal(6, vehicle.GearCount);
		DataAssets.Reset();
		Assert.Equal(0, VehicleCache.EntryCount);
		Assert.Equal(6, vehicle.GearCount);
		Assert.Equal(2, s_Reads.Count);
	}

	[Fact]
	public void VehiclesAndTheirComponentAreNodesWhoseQueriesArePure()
	{
		VisualType type = VisualScriptTesting.Library.FindType("Strada.Vehicle")!;
		Assert.Equal(TypeKind.Asset, type.Kind);
		Assert.Equal("Vehicle", type.AssetType);
		Assert.True(type.HasLiteral);
		Assert.Equal(TypeKind.Enum, VisualScriptTesting.Library.FindType("Strada.VehicleTransmissionMode")!.Kind);
		foreach (string member in new[] { "TransmissionMode", "GearCount", "FinalDrive", "MaxTorque", "MinRPM", "MaxRPM", "ShiftUpRPM",
			"ShiftDownRPM", "MaxLeanAngle" })
		{
			Assert.True(VisualScriptTesting.LibraryNode($"Get:Strada.Vehicle.{member}").Pure, member);
			Assert.Null(VisualScriptTesting.Library.FindNode($"Set:Strada.Vehicle.{member}"));
		}
		Assert.True(VisualScriptTesting.LibraryNode("Call:Strada.Vehicle.GetGearRatio(int)").Pure);
		Assert.True(VisualScriptTesting.LibraryNode("Call:Strada.Vehicle.GetWheelName(int)").Pure);

		// The component: the reads are pure, the rider's input and the shifts are impure, and a wheel's state is a struct that
		// graphs Break.
		foreach (string member in new[] { "EngineSpeed", "Gear", "ForwardSpeed", "LeanAngle", "Throttle", "FrontBrake", "RearBrake", "Lean" })
		{
			Assert.True(VisualScriptTesting.LibraryNode($"Get:Strada.VehicleComponent.{member}").Pure, member);
			Assert.Null(VisualScriptTesting.Library.FindNode($"Set:Strada.VehicleComponent.{member}"));
		}
		Assert.NotNull(VisualScriptTesting.Library.FindNode("Set:Strada.VehicleComponent.Definition"));
		NodeDefinition wheel = VisualScriptTesting.LibraryNode("Call:Strada.VehicleComponent.GetWheelState(int)");
		Assert.True(wheel.Pure);
		Assert.Equal(["In:Target", "In:Index", "Out:Result"], wheel.Pins.Select(static pin => $"{pin.Direction}:{pin.Name}"));
		foreach (string member in new[] { "SetDriverInput(float,float,float,float)", "ShiftUp()", "ShiftDown()" })
		{
			Assert.False(VisualScriptTesting.LibraryNode($"Call:Strada.VehicleComponent.{member}").Pure, member);
		}
		Assert.True(VisualScriptTesting.LibraryNode("Get:Strada.VehicleWheelState.Surface").Pure);
		VisualType wheelState = VisualScriptTesting.Library.FindType("Strada.VehicleWheelState")!;
		Assert.Equal(TypeKind.Struct, wheelState.Kind);
		Assert.True(wheelState.Breakable);
	}
}
