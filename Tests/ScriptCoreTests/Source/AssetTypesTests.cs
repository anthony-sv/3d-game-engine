using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Strada.Interop;
using Xunit;

namespace Strada.ScriptCore.Tests;

// How managed code names the engine's asset types without an engine: Assets_FindType and Assets_GetTypeName are served by
// fakes of the engine's bindings that know one data asset type, Curve, so that the tests see when the engine is asked. The
// engine's side is StradaTests' (Scene/SceneScriptingHostTests.cpp: data asset types are found and named).
[Collection(ProcessStaticsCollection.Name)]
public sealed unsafe class AssetTypesTests : IDisposable
{
	// The value the fake engine gives the Curve type, as the engine gives values to data asset types from 128 up.
	private const int CurveValue = 130;

	// Every value Assets_GetTypeName was asked to name, and every name Assets_FindType was asked to find, in order.
	private static readonly List<int> s_NameRequests = [];
	private static readonly List<string> s_TypeRequests = [];
	// "Curve" in native memory, as the engine's names live as long as the engine.
	private static byte* s_CurveName;

	private readonly delegate* unmanaged<byte*, int, int> m_PreviousFindType;
	private readonly delegate* unmanaged<int, int*, byte*> m_PreviousGetTypeName;

	public AssetTypesTests()
	{
		m_PreviousFindType = InternalCalls.Assets_FindType;
		m_PreviousGetTypeName = InternalCalls.Assets_GetTypeName;
		InternalCalls.Assets_FindType = &FindType;
		InternalCalls.Assets_GetTypeName = &GetTypeName;
		s_CurveName = (byte*)NativeMemory.Alloc(5);
		"Curve"u8.CopyTo(new Span<byte>(s_CurveName, 5));
		s_NameRequests.Clear();
		s_TypeRequests.Clear();
		AssetTypes.Reset();
	}

	public void Dispose()
	{
		InternalCalls.Assets_FindType = m_PreviousFindType;
		InternalCalls.Assets_GetTypeName = m_PreviousGetTypeName;
		NativeMemory.Free(s_CurveName);
		s_CurveName = null;
		s_NameRequests.Clear();
		s_TypeRequests.Clear();
		AssetTypes.Reset();
	}

	// Assets_FindType as ScriptBindingsAssets.cpp answers for the fake engine's one data asset type: 0 for no type.
	[UnmanagedCallersOnly]
	private static int FindType(byte* name, int length)
	{
		string typeName = NativeString.FromUtf8(name, length);
		s_TypeRequests.Add(typeName);
		return typeName == "Curve" ? CurveValue : 0;
	}

	// Assets_GetTypeName likewise: null for a value that is no type.
	[UnmanagedCallersOnly]
	private static byte* GetTypeName(int type, int* length)
	{
		s_NameRequests.Add(type);
		*length = type == CurveValue ? 5 : 0;
		return type == CurveValue ? s_CurveName : null;
	}

	[Fact]
	public void BuiltInValuesAreNamedWithoutTheEngineAndADataAssetTypesValueIsAskedOnce()
	{
		Assert.Equal("Scene", AssetTypes.GetTypeName((int)NativeAssetType.Scene));
		Assert.Equal("Mesh", AssetTypes.GetTypeName((int)NativeAssetType.Mesh));
		Assert.Equal("VisualScript", AssetTypes.GetTypeName((int)NativeAssetType.VisualScript));
		Assert.Null(AssetTypes.GetTypeName((int)NativeAssetType.None));
		// Below the data asset types' values, past the built-in ones: no type.
		Assert.Null(AssetTypes.GetTypeName(AssetTypes.FirstDataAssetTypeValue - 1));
		Assert.Empty(s_NameRequests);

		// The engine names a data asset type's value once; the name makes references of its class.
		Assert.Equal("Curve", AssetTypes.GetTypeName(CurveValue));
		Assert.Equal("Curve", AssetTypes.GetTypeName(CurveValue));
		Assert.Equal([CurveValue], s_NameRequests);
		Asset curve = AssetTypes.Create(AssetTypes.GetTypeName(CurveValue)!, new AssetHandle(7));
		Assert.IsType<Curve>(curve);
		Assert.Equal(new AssetHandle(7), curve.Handle);
		// The value named is the class's value too: Assets_FindType is not asked for it.
		Assert.Equal(CurveValue, AssetTypes.GetNativeType(typeof(Curve)));
		Assert.Empty(s_TypeRequests);

		// A value that is no type is named by nothing, and asked again each time.
		Assert.Null(AssetTypes.GetTypeName(200));
		Assert.Null(AssetTypes.GetTypeName(200));
		Assert.Equal([CurveValue, 200, 200], s_NameRequests);
	}

	[Fact]
	public void TheValueFoundForAClassNamesItAndAResetForgetsBoth()
	{
		Assert.Equal(CurveValue, AssetTypes.GetNativeType(typeof(Curve)));
		Assert.Equal(CurveValue, AssetTypes.GetNativeType(typeof(Curve)));
		Assert.Equal(["Curve"], s_TypeRequests);
		Assert.Equal("Curve", AssetTypes.GetTypeName(CurveValue));
		Assert.Empty(s_NameRequests);

		// Bound again, the engine may give other values: both are asked again.
		AssetTypes.Reset();
		Assert.Equal("Curve", AssetTypes.GetTypeName(CurveValue));
		Assert.Equal([CurveValue], s_NameRequests);
		AssetTypes.Reset();
		Assert.Equal(CurveValue, AssetTypes.GetNativeType(typeof(Curve)));
		Assert.Equal(["Curve", "Curve"], s_TypeRequests);
	}
}
