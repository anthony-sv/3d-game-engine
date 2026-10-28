using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The native protocol of visual scripts (spec section 6): the entry points the engine binds by name, and the operations
// behind them, which answer a status and, exactly on Success, the text the engine receives.
public sealed class HostProtocolTests
{
	// The entry points and their parameters, in order (spec section 6): "receive" is delegate* unmanaged<void*, byte*, int,
	// void>.
	private static readonly (string Name, string[] Parameters)[] s_EntryPoints =
	[
		("VisualScriptDescribeLibrary", ["void*", "receive"]),
		("VisualScriptAnalyze", ["byte*", "int", "byte*", "int", "void*", "receive"]),
		("VisualScriptLoad", ["ulong", "byte*", "int", "byte*", "int", "byte*", "int", "void*", "receive"]),
		("VisualScriptUnload", ["ulong"]),
		("VisualScriptCreateInstance", ["ulong", "ulong", "byte*", "int"]),
		("VisualScriptDestroyInstance", ["ulong"]),
		("VisualScriptDestroyAllInstances", []),
		("VisualScriptInvoke", ["ulong", "int", "float", "ulong"]),
		("VisualScriptDescribeInstanceVariables", ["ulong", "void*", "receive"]),
		("VisualScriptSetInstanceVariables", ["ulong", "byte*", "int"]),
	];

	public static TheoryData<string> EntryPoints => [.. s_EntryPoints.Select(static entryPoint => entryPoint.Name)];

	private static Type TypeOf(string parameter) => parameter switch
	{
		"void*" => typeof(void).MakePointerType(),
		"byte*" => typeof(byte).MakePointerType(),
		"int" => typeof(int),
		"ulong" => typeof(ulong),
		"float" => typeof(float),
		_ => throw new ArgumentException($"unknown parameter type {parameter}", nameof(parameter)),
	};

	[Theory]
	[MemberData(nameof(EntryPoints))]
	public void TheEntryPointHasTheSignatureTheEngineBindsByName(string name)
	{
		string[] parameters = s_EntryPoints.Single(entryPoint => entryPoint.Name == name).Parameters;
		MethodInfo? method = typeof(Host).GetMethod(name, BindingFlags.Static | BindingFlags.NonPublic);
		Assert.NotNull(method);
		Assert.True(method.IsAssembly, $"{name} is not internal");
		Assert.NotNull(method.GetCustomAttribute<UnmanagedCallersOnlyAttribute>());
		Assert.Equal(typeof(int), method.ReturnType);
		ParameterInfo[] actual = method.GetParameters();
		Assert.Equal(parameters.Length, actual.Length);
		for (int index = 0; index < parameters.Length; index++)
		{
			Type type = actual[index].ParameterType;
			if (parameters[index] != "receive")
			{
				Assert.Equal(TypeOf(parameters[index]), type);
				continue;
			}
			Assert.True(type.IsFunctionPointer && type.IsUnmanagedFunctionPointer, $"{name}: {actual[index].Name} is not an unmanaged function pointer");
			Assert.Equal([TypeOf("void*"), TypeOf("byte*"), typeof(int)], type.GetFunctionPointerParameterTypes());
			Assert.Equal(typeof(void), type.GetFunctionPointerReturnType());
		}
	}

	[Fact]
	public void TheHostHasNoOtherVisualScriptEntryPoints()
	{
		string[] declared = [.. typeof(Host).GetMethods(BindingFlags.Static | BindingFlags.NonPublic)
			.Where(static method => method.Name.StartsWith("VisualScript", StringComparison.Ordinal) && method.IsDefined(typeof(UnmanagedCallersOnlyAttribute)))
			.Select(static method => method.Name).Order(StringComparer.Ordinal)];
		Assert.Equal(s_EntryPoints.Select(static entryPoint => entryPoint.Name).Order(StringComparer.Ordinal), declared);
	}

	// receive as the engine passes it: each call adds a copy of the bytes to the list context refers to (a GCHandle).
	[UnmanagedCallersOnly]
	private static unsafe void Receive(void* context, byte* bytes, int length)
	{
		List<byte[]> received = (List<byte[]>)GCHandle.FromIntPtr((IntPtr)context).Target!;
		received.Add(new ReadOnlySpan<byte>(bytes, length).ToArray());
	}

	// The status the entry points answer for a reply, and the texts receive got.
	private static unsafe (int Status, List<byte[]> Received) Deliver(HostReply reply)
	{
		List<byte[]> received = [];
		GCHandle handle = GCHandle.Alloc(received);
		try
		{
			return (Host.Deliver(reply, (void*)GCHandle.ToIntPtr(handle), &Receive), received);
		}
		finally
		{
			handle.Free();
		}
	}

	// The entry points that answer text pass their operation's reply on: receive gets the text of a Success exactly once.
	[Fact]
	public void ReceiveGetsTheTextOfASuccessOnce()
	{
		DocumentBuilder document = new();
		document.Variable("Speed", "float", "2.0", isPublic: true);
		TestRuntime runtime = TestRuntime.Start(document);
		VisualScriptRuntime host = runtime.Runtime;
		HostReply[] replies =
		[
			host.DescribeLibrary(), host.AnalyzeDocument([]), host.AnalyzeDocument(Encoding.UTF8.GetBytes(document.Build())),
			host.LoadDocument(TestRuntime.Graph, TestRuntime.Name, Encoding.UTF8.GetBytes(document.Build())), host.DescribeVariables(TestRuntime.Hero),
		];

		foreach (HostReply reply in replies)
		{
			(int status, List<byte[]> received) = Deliver(reply);
			Assert.Equal(Host.Success, status);
			Assert.Equal(reply.Text, Assert.Single(received));
		}
	}

	[Fact]
	public void ReceiveIsNotCalledWithoutASuccess()
	{
		TestRuntime runtime = new();

		(int status, List<byte[]> received) = Deliver(runtime.Runtime.DescribeVariables(TestRuntime.Hero));
		Assert.Equal(Host.NotFound, status);
		Assert.Empty(received);
	}

	private static JsonElement Parse(HostReply reply)
	{
		Assert.Equal(VisualScriptStatus.Success, reply.Status);
		Assert.NotNull(reply.Text);
		using JsonDocument document = JsonDocument.Parse(reply.Text);
		return document.RootElement.Clone();
	}

	private static void AssertNoText(HostReply reply, VisualScriptStatus status)
	{
		Assert.Equal(status, reply.Status);
		Assert.Null(reply.Text);
	}

	[Fact]
	public void DescribeLibraryAnswersTheLibraryJson()
	{
		TestRuntime runtime = new();

		JsonElement library = Parse(runtime.Runtime.DescribeLibrary());
		Assert.True(library.GetProperty("Types").GetArrayLength() > 0);
		Assert.Contains(library.GetProperty("Nodes").EnumerateArray(), static node => node.GetProperty("Type").GetString() == "Flow.Delay");
		Assert.Equal(LibraryWriter.Write(RuntimeTesting.Library), runtime.Runtime.DescribeLibrary().Text);
	}

	[Theory]
	[InlineData("")]
	[InlineData("[1, 2]")]
	[InlineData("\"text\"")]
	[InlineData("{ \"Graph\": ")]
	public void AnalyzeAnswersOneInvalidDocumentForWhatIsNotADocument(string text)
	{
		TestRuntime runtime = new();

		JsonElement analysis = Parse(runtime.Runtime.AnalyzeDocument(Encoding.UTF8.GetBytes(text)));
		Assert.Equal(0, analysis.GetProperty("Nodes").GetArrayLength());
		JsonElement diagnostic = Assert.Single(analysis.GetProperty("Diagnostics").EnumerateArray());
		Assert.Equal("InvalidDocument", diagnostic.GetProperty("Code").GetString());
		Assert.Equal("Error", diagnostic.GetProperty("Severity").GetString());
	}

	[Fact]
	public void AnalyzeAnswersTheNodesAndDiagnosticsOfADocument()
	{
		DocumentBuilder document = new();
		uint delay = document.Graph.Node("Flow.Delay");
		document.Graph.Flow(document.Graph.Node("Event.Create"), delay);
		TestRuntime runtime = new();

		JsonElement analysis = Parse(runtime.Runtime.AnalyzeDocument(Encoding.UTF8.GetBytes(document.Build())));
		Assert.Equal(2, analysis.GetProperty("Nodes").GetArrayLength());
		Assert.Equal(0, analysis.GetProperty("Diagnostics").GetArrayLength());
	}

	[Fact]
	public void LoadStoresAProgramForAnyDocumentAndAnswersItsDiagnostics()
	{
		TestRuntime runtime = new();
		VisualScriptRuntime host = runtime.Runtime;

		JsonElement broken = Parse(host.LoadDocument(TestRuntime.Graph, TestRuntime.Name, []));
		Assert.Equal("InvalidDocument", Assert.Single(broken.EnumerateArray()).GetProperty("Code").GetString());
		Assert.Equal(VisualScriptStatus.Failure, host.CreateInstance(TestRuntime.Hero, TestRuntime.Graph, []));
		Assert.Equal(VisualScriptStatus.NotFound, host.CreateInstance(TestRuntime.Hero, 404, []));

		DocumentBuilder document = new();
		document.Variable("Speed", "float", "2.0", isPublic: true);
		document.Graph.Writes(document.Graph.Node("Event.Create"), "Then", "created");
		Assert.Equal(0, Parse(host.LoadDocument(TestRuntime.Graph, TestRuntime.Name, Encoding.UTF8.GetBytes(document.Build()))).GetArrayLength());
		// An empty variables span holds no overrides.
		Assert.Equal(VisualScriptStatus.Success, host.CreateInstance(TestRuntime.Hero, TestRuntime.Graph, []));
		Assert.Equal(2.0f, runtime.Variable("Speed"));
		Assert.Equal(VisualScriptStatus.Success, host.Invoke(TestRuntime.Hero, ScriptEvent.Create, 0.0f, 0));
		Assert.Equal(["created"], runtime.Records);

		// A document that is not an object keeps the working program for the instances.
		Assert.Equal(1, Parse(host.LoadDocument(TestRuntime.Graph, TestRuntime.Name, Encoding.UTF8.GetBytes("[]"))).GetArrayLength());
		Assert.Equal(VisualScriptStatus.Success, host.CreateInstance(8, TestRuntime.Graph, []));
	}

	// The references bundle accompanies a document whatever its bytes: a document whose nodes refer to no other graph analyzes
	// and loads as it does without one.
	[Theory]
	[InlineData("")]
	[InlineData("not a bundle")]
	[InlineData("""{ "Self": "0", "Graphs": [] }""")]
	[InlineData("""{ "Self": "7", "Graphs": [ { "Graph": "42", "Name": "", "Error": "no visual script 42 in the project" } ] }""")]
	public void ReferencesLeaveADocumentThatRefersToNoOtherGraphAsItIs(string references)
	{
		DocumentBuilder document = new();
		document.Variable("Speed", "float", "2.0", isPublic: true);
		document.Graph.Writes(document.Graph.Node("Event.Create"), "Then", "created");
		byte[] text = Encoding.UTF8.GetBytes(document.Build());
		byte[] bundle = Encoding.UTF8.GetBytes(references);
		TestRuntime runtime = new();
		VisualScriptRuntime host = runtime.Runtime;

		Assert.Equal(host.AnalyzeDocument(text).Text, host.AnalyzeDocument(text, bundle).Text);
		Assert.Equal(host.LoadDocument(TestRuntime.Graph, TestRuntime.Name, text).Text,
			host.LoadDocument(TestRuntime.Graph, TestRuntime.Name, text, bundle).Text);
		Assert.Equal(VisualScriptStatus.Success, host.CreateInstance(TestRuntime.Hero, TestRuntime.Graph, []));
		Assert.Equal(VisualScriptStatus.Success, host.Invoke(TestRuntime.Hero, ScriptEvent.Create, 0.0f, 0));
		Assert.Equal(["created"], runtime.Records);
	}

	[Fact]
	public void TheInstanceOperationsAnswerTheirStatuses()
	{
		DocumentBuilder document = new();
		document.Variable("Speed", "float", "2.0", isPublic: true);
		TestRuntime runtime = TestRuntime.Start(document);
		VisualScriptRuntime host = runtime.Runtime;

		JsonElement variables = Parse(host.DescribeVariables(TestRuntime.Hero));
		Assert.Equal(2.0f, variables.GetProperty("Speed").GetProperty("Value").GetSingle());
		AssertNoText(host.DescribeVariables(99), VisualScriptStatus.NotFound);
		Assert.Equal(VisualScriptStatus.Success, host.SetInstanceVariables(TestRuntime.Hero, []));
		Assert.Equal(VisualScriptStatus.Success, host.SetInstanceVariables(TestRuntime.Hero, """{ "Speed": { "Type": "Float", "Value": 4 } }"""u8));
		Assert.Equal(4.0f, runtime.Variable("Speed"));
		Assert.Equal(VisualScriptStatus.NotFound, host.SetInstanceVariables(99, []));
		Assert.Equal(VisualScriptStatus.NotFound, host.Invoke(99, ScriptEvent.Update, 0.0f, 0));
		Assert.Equal(VisualScriptStatus.Success, host.Invoke(TestRuntime.Hero, ScriptEvent.Update, 0.0f, 0));
		Assert.Equal(VisualScriptStatus.Success, host.DestroyInstance(TestRuntime.Hero));
		Assert.Equal(VisualScriptStatus.NotFound, host.DestroyInstance(TestRuntime.Hero));
		Assert.Equal(VisualScriptStatus.Success, host.Unload(TestRuntime.Graph));
		Assert.Equal(VisualScriptStatus.NotFound, host.Unload(TestRuntime.Graph));
		Assert.Equal(VisualScriptStatus.NotFound, host.CreateInstance(TestRuntime.Hero, TestRuntime.Graph, []));
		Assert.Empty(runtime.Environment.Logs);
	}
}
