using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The scripting API's input actions without an engine (Docs/Architecture.md section 7.4): what Strada.Input's action members
// and Strada.InputActions hand the engine's bindings, served by fakes, and the view of the asking code that graph executions
// record (the fixed-step rule's managed half): code of a frame reads the frame's edges, code of a step that step's, a
// debugger resume the step it stopped in, a latent continuation of step code none. The engine's side is StradaTests'
// (Scene/SceneScriptingInputActionsTests.cpp). The fake bindings are the process's, so the tests run alone.
[Collection(ProcessStaticsCollection.Name)]
public sealed unsafe class InputActionTests : IDisposable
{
	// What the fake engine saw: each query as "<binding> <name> <view> <step>", each override as "<binding> <name> <x> <y>".
	private static readonly List<string> s_Calls = [];
	private static readonly List<string> s_Names = [];
	private static ulong s_Version;
	private static bool s_ReadFails;
	private static int s_Reads;

	private readonly delegate* unmanaged<byte*, int, float> m_GetAction;
	private readonly delegate* unmanaged<byte*, int, Vector2*, void> m_GetActionVector;
	private readonly delegate* unmanaged<byte*, int, byte> m_IsActionDown;
	private readonly delegate* unmanaged<byte*, int, int, ulong, byte> m_IsActionPressed;
	private readonly delegate* unmanaged<byte*, int, int, ulong, byte> m_IsActionReleased;
	private readonly delegate* unmanaged<byte*, int, Vector2*, void> m_SetActionOverride;
	private readonly delegate* unmanaged<byte*, int, void> m_ClearActionOverride;
	private readonly delegate* unmanaged<ulong, ulong, ulong*, int*, int> m_Read;
	private readonly delegate* unmanaged<ulong, int, int*, byte*> m_GetActionName;

	public InputActionTests()
	{
		m_GetAction = InternalCalls.Input_GetAction;
		m_GetActionVector = InternalCalls.Input_GetActionVector;
		m_IsActionDown = InternalCalls.Input_IsActionDown;
		m_IsActionPressed = InternalCalls.Input_IsActionPressed;
		m_IsActionReleased = InternalCalls.Input_IsActionReleased;
		m_SetActionOverride = InternalCalls.Input_SetActionOverride;
		m_ClearActionOverride = InternalCalls.Input_ClearActionOverride;
		m_Read = InternalCalls.InputActions_Read;
		m_GetActionName = InternalCalls.InputActions_GetActionName;
		InternalCalls.Input_GetAction = &GetAction;
		InternalCalls.Input_GetActionVector = &GetActionVector;
		InternalCalls.Input_IsActionDown = &IsActionDown;
		InternalCalls.Input_IsActionPressed = &IsActionPressed;
		InternalCalls.Input_IsActionReleased = &IsActionReleased;
		InternalCalls.Input_SetActionOverride = &SetActionOverride;
		InternalCalls.Input_ClearActionOverride = &ClearActionOverride;
		InternalCalls.InputActions_Read = &Read;
		InternalCalls.InputActions_GetActionName = &GetActionName;
		s_Calls.Clear();
		s_Names.Clear();
		s_Version = 0;
		s_ReadFails = false;
		s_Reads = 0;
		InputActionContext.Current = InputActionOrigin.Frame;
		DataAssets.Reset();
	}

	public void Dispose()
	{
		InternalCalls.Input_GetAction = m_GetAction;
		InternalCalls.Input_GetActionVector = m_GetActionVector;
		InternalCalls.Input_IsActionDown = m_IsActionDown;
		InternalCalls.Input_IsActionPressed = m_IsActionPressed;
		InternalCalls.Input_IsActionReleased = m_IsActionReleased;
		InternalCalls.Input_SetActionOverride = m_SetActionOverride;
		InternalCalls.Input_ClearActionOverride = m_ClearActionOverride;
		InternalCalls.InputActions_Read = m_Read;
		InternalCalls.InputActions_GetActionName = m_GetActionName;
		InputActionContext.Current = InputActionOrigin.Frame;
		DataAssets.Reset();
		foreach (nint text in s_NameTexts)
		{
			NativeMemory.Free((void*)text);
		}
		s_NameTexts.Clear();
	}

	private static string Text(byte* name, int length) => Encoding.UTF8.GetString(name, length);

	// Jump reads 0.5 and is down; Move reads (0, 1); everything else reads nothing.
	[UnmanagedCallersOnly]
	private static float GetAction(byte* name, int length) => Text(name, length) == "Jump" ? 0.5f : 0.0f;

	[UnmanagedCallersOnly]
	private static void GetActionVector(byte* name, int length, Vector2* value)
	{
		*value = Text(name, length) == "Move" ? new Vector2(0.0f, 1.0f) : Vector2.Zero;
	}

	[UnmanagedCallersOnly]
	private static byte IsActionDown(byte* name, int length) => Text(name, length) == "Jump" ? (byte)1 : (byte)0;

	// Every action is pressed but Awaited, which a WaitUntil waits for.
	[UnmanagedCallersOnly]
	private static byte IsActionPressed(byte* name, int length, int view, ulong step)
	{
		string action = Text(name, length);
		s_Calls.Add(FormattableString.Invariant($"Pressed {action} {view} {step}"));
		return action == "Awaited" ? (byte)0 : (byte)1;
	}

	[UnmanagedCallersOnly]
	private static byte IsActionReleased(byte* name, int length, int view, ulong step)
	{
		s_Calls.Add(FormattableString.Invariant($"Released {Text(name, length)} {view} {step}"));
		return 0;
	}

	[UnmanagedCallersOnly]
	private static void SetActionOverride(byte* name, int length, Vector2* value)
	{
		s_Calls.Add(FormattableString.Invariant($"Set {Text(name, length)} {value->X} {value->Y}"));
	}

	[UnmanagedCallersOnly]
	private static void ClearActionOverride(byte* name, int length)
	{
		s_Calls.Add($"Clear {Text(name, length)}");
	}

	// InputActions_Read as ScriptBindingsAssets.cpp answers: the version always, the count only for a version the caller does
	// not hold.
	[UnmanagedCallersOnly]
	private static int Read(ulong actions, ulong heldVersion, ulong* version, int* count)
	{
		s_Reads++;
		if (s_ReadFails || actions != 5)
		{
			return (int)InputActionsReadStatus.Failed;
		}
		*version = s_Version;
		if (s_Version == heldVersion)
		{
			return (int)InputActionsReadStatus.Unchanged;
		}
		*count = s_Names.Count;
		return (int)InputActionsReadStatus.Read;
	}

	// The names' UTF-8 as the engine keeps it, in native memory that lives until the test ends.
	private static readonly List<nint> s_NameTexts = [];

	[UnmanagedCallersOnly]
	private static byte* GetActionName(ulong actions, int index, int* length)
	{
		*length = 0;
		if (actions != 5 || index < 0 || index >= s_Names.Count)
		{
			return null;
		}
		byte[] bytes = Encoding.UTF8.GetBytes(s_Names[index]);
		byte* text = (byte*)NativeMemory.Alloc((nuint)System.Math.Max(bytes.Length, 1));
		bytes.CopyTo(new Span<byte>(text, bytes.Length));
		s_NameTexts.Add((nint)text);
		*length = bytes.Length;
		return text;
	}

	[Fact]
	public void QueriesPassTheActionsNameAndTheViewOfTheCodeThatAsks()
	{
		Assert.Equal(0.5f, Input.GetAction("Jump"));
		Assert.Equal(0.0f, Input.GetAction("jump"));
		Assert.Equal(new Vector2(0.0f, 1.0f), Input.GetActionVector("Move"));
		Assert.True(Input.IsActionDown("Jump"));
		Assert.False(Input.IsActionDown("Move"));
		Assert.True(Input.IsActionPressed("Jump"));
		Assert.False(Input.IsActionReleased("Jump"));
		InputActionOrigin previous = InputActionContext.Enter(new InputActionOrigin(InputActionView.FixedStep, 9));
		Input.IsActionPressed("Sauté");
		InputActionContext.Restore(previous);
		InputActionContext.Enter(new InputActionOrigin(InputActionView.NoEdges, 0));
		Input.IsActionReleased("Move");
		InputActionContext.Restore(previous);
		// A name longer than any action's is passed whole (on the heap).
		string longName = new('x', 200);
		Input.IsActionPressed(longName);
		Assert.Equal(["Pressed Jump 0 0", "Released Jump 0 0", "Pressed Sauté 1 9", "Released Move 2 0", $"Pressed {longName} 0 0"], s_Calls);
		Assert.Throws<ArgumentNullException>(() => Input.GetAction(null!));
		Assert.Throws<ArgumentNullException>(() => Input.IsActionPressed(null!));
	}

	[Fact]
	public void OverridesPassTheirValuesAsVectors()
	{
		Input.SetActionOverride("Jump", 0.75f);
		Input.SetActionVectorOverride("Move", new Vector2(-1.0f, 0.5f));
		Input.ClearActionOverride("Jump");
		Assert.Equal(["Set Jump 0.75 0", "Set Move -1 0.5", "Clear Jump"], s_Calls);
		Assert.Throws<ArgumentNullException>(() => Input.SetActionOverride(null!, 1.0f));
		Assert.Throws<ArgumentNullException>(() => Input.ClearActionOverride(null!));
	}

	[Fact]
	public void ActionNamesAreReadOncePerVersionOfTheAsset()
	{
		s_Names.AddRange(["Jump", "Steer", "Move"]);
		s_Version = 3;
		InputActions actions = new(new AssetHandle(5));
		string[] names = actions.ActionNames;
		Assert.Equal(["Jump", "Steer", "Move"], names);
		// A new array on each read, from one copy shared by every reference.
		names[0] = "Changed";
		Assert.Equal(["Jump", "Steer", "Move"], new InputActions(new AssetHandle(5)).ActionNames);
		Assert.Equal(1, s_Reads);
		Assert.Equal(1, InputActionsCache.EntryCount);

		// Data assets changed, the asset did not: asked again, nothing copied.
		DataAssets.Invalidate();
		Assert.Equal(["Jump", "Steer", "Move"], actions.ActionNames);
		Assert.Equal(2, s_Reads);
		// An edit: the new names from the next read on.
		s_Names.RemoveAt(1);
		s_Version = 4;
		Assert.Equal(["Jump", "Steer", "Move"], actions.ActionNames);
		DataAssets.Invalidate();
		Assert.Equal(["Jump", "Move"], actions.ActionNames);
		// An asset that does not load has no actions.
		s_ReadFails = true;
		DataAssets.Invalidate();
		Assert.Empty(actions.ActionNames);
		Assert.Empty(new InputActions(new AssetHandle(6)).ActionNames);
	}

	// What a graph's IsActionPressed node passes, read by Probe.Write: "True" (the fake presses every action).
	private const string Pressed = "Call:Strada.Input.IsActionPressed(string)";
	private const string Jump = """{ "Action": "Jump" }""";

	[Fact]
	public void AnExecutionReadsTheEdgesOfTheFrameOrOfTheStepItRunsIn()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		graph.Flow(graph.Node("Event.Update"), graph.Write(graph.Node(Pressed, inputs: Jump), "Result"));
		graph.Flow(graph.Node("Event.FixedUpdate"), graph.Write(graph.Node(Pressed, inputs: Jump), "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update, 0.016f);
		runtime.Environment.ActionStep = 4;
		runtime.Invoke(ScriptEvent.FixedUpdate, 0.016f);
		runtime.Environment.ActionStep = 0;
		Assert.Equal(["Pressed Jump 0 0", "Pressed Jump 1 4"], s_Calls);
		Assert.Equal(["True", "True"], runtime.Records);
		// The engine's dispatch leaves the view as it was.
		Assert.Equal(InputActionOrigin.Frame, InputActionContext.Current);
	}

	// A Delay, a WaitUntil, a Tween's ticks and a Timeline's (its Finished fires after its run is released) continue step code
	// in a later Update dispatch: they read no edge there (the step saw its own, the next step sees the next). The same nodes
	// started by Event.Update read the frame's.
	[Theory]
	[InlineData("Event.FixedUpdate", 2)]
	[InlineData("Event.Update", 0)]
	public void LatentNodesContinueStepCodeWithoutEdges(string started, int view)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		// Each latent node in a custom event of its own, so that each waits in an execution of its own: a receiver that waits
		// continues on its own, with the view of the execution that sent to it.
		uint once = graph.Node("Flow.DoOnce");
		uint sequence = graph.Node("Flow.Sequence", """{ "Count": 4 }""");
		graph.Flow(graph.Node(started), once);
		graph.Flow(once, sequence, "Completed");
		string[] events = ["Delaying", "Awaiting", "Tweening", "Timing"];
		for (int index = 0; index < events.Length; index++)
		{
			graph.Flow(sequence, graph.Node("Event.Send", $$"""{ "Name": "{{events[index]}}" }"""), FormattableString.Invariant($"Then{index}"));
		}
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.0 }""");
		graph.Flow(graph.Node("Event.Custom", """{ "Name": "Delaying" }"""), delay);
		graph.Flow(delay, graph.Write(graph.Node(Pressed, inputs: """{ "Action": "Delayed" }"""), "Result"), "Completed");
		uint wait = graph.Node("Flow.WaitUntil");
		graph.Link(graph.Node(Pressed, inputs: """{ "Action": "Awaited" }"""), "Result", wait, "Condition");
		graph.Flow(graph.Node("Event.Custom", """{ "Name": "Awaiting" }"""), wait);
		uint tween = graph.Node("Flow.Tween", inputs: """{ "Duration": 1.0 }""");
		graph.Flow(graph.Node("Event.Custom", """{ "Name": "Tweening" }"""), tween);
		graph.Flow(tween, graph.Write(graph.Node(Pressed, inputs: """{ "Action": "Ticked" }"""), "Result"), "Update");
		uint timeline = graph.Node("Flow.Timeline", """{ "Timeline": { "Length": 0.01 } }""");
		graph.Flow(graph.Node("Event.Custom", """{ "Name": "Timing" }"""), timeline, "Then", "PlayFromStart");
		graph.Flow(timeline, graph.Write(graph.Node(Pressed, inputs: """{ "Action": "Timed" }"""), "Result"), "Update");
		graph.Flow(timeline, graph.Write(graph.Node(Pressed, inputs: """{ "Action": "Finished" }"""), "Result"), "Finished");
		TestRuntime runtime = TestRuntime.Start(document);

		// The engine is inside a step only while it runs one.
		bool step = started == "Event.FixedUpdate";
		runtime.Environment.ActionStep = step ? 4u : 0u;
		runtime.Invoke(step ? ScriptEvent.FixedUpdate : ScriptEvent.Update, 0.016f);
		Assert.Equal(FormattableString.Invariant($"Pressed Awaited {(step ? "1 4" : "0 0")}"), Assert.Single(s_Calls));
		runtime.Environment.ActionStep = 0;
		s_Calls.Clear();
		runtime.Invoke(ScriptEvent.Update, 0.016f);
		string expected = FormattableString.Invariant($"{view} 0");
		Assert.Contains($"Pressed Delayed {expected}", s_Calls);
		Assert.Contains($"Pressed Awaited {expected}", s_Calls);
		Assert.Contains($"Pressed Ticked {expected}", s_Calls);
		Assert.Contains($"Pressed Timed {expected}", s_Calls);
		Assert.Contains($"Pressed Finished {expected}", s_Calls);
		Assert.All(s_Calls, call => Assert.EndsWith(expected, call));
	}

	// A debugger stop in a fixed step resumed between frames reads that step's edges, as does an execution it starts (a Send);
	// code a frame started reads the frame's again when it resumes.
	[Fact]
	public void ADebuggerResumeReadsTheEdgesOfTheStepItStoppedIn()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint fixedUpdate = graph.Node("Event.FixedUpdate");
		uint stop = graph.Write("stopped");
		uint read = graph.Write(graph.Node(Pressed, inputs: Jump), "Result");
		uint send = graph.Node("Event.Send", """{ "Name": "Ping" }""");
		graph.Flow(fixedUpdate, stop);
		graph.Flow(stop, read);
		graph.Flow(read, send);
		graph.Flow(graph.Node("Event.Custom", """{ "Name": "Ping" }"""), graph.Write(graph.Node(Pressed, inputs: """{ "Action": "Pinged" }"""), "Result"));
		uint update = graph.Node("Event.Update");
		uint updateStop = graph.Write("update");
		graph.Flow(update, updateStop);
		graph.Flow(updateStop, graph.Write(graph.Node(Pressed, inputs: """{ "Action": "Updated" }"""), "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(stop, updateStop);

		runtime.Environment.ActionStep = 4;
		runtime.Invoke(ScriptEvent.FixedUpdate, 0.016f);
		runtime.Environment.ActionStep = 0;
		Assert.Single(session.Stops());
		Assert.Empty(s_Calls);
		session.Continue();
		Assert.Equal(["Pressed Jump 1 4", "Pressed Pinged 1 4"], s_Calls);

		s_Calls.Clear();
		runtime.Invoke(ScriptEvent.Update, 0.016f);
		session.Continue();
		Assert.Equal(["Pressed Updated 0 0"], s_Calls);
		Assert.Equal(InputActionOrigin.Frame, InputActionContext.Current);
	}

	[Fact]
	public void ActionEventsRunTheNodeOfTheHandledActionOnly()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		graph.Writes(graph.Node("Event.ActionPressed", Jump), "Then", "jump pressed");
		graph.Writes(graph.Node("Event.ActionPressed", """{ "Action": "Move" }"""), "Then", "move pressed");
		graph.Writes(graph.Node("Event.ActionReleased", Jump), "Then", "jump released");
		// Key events are polled as before, beside them.
		graph.Writes(graph.Node("Event.KeyPressed", """{ "Key": "Space" }"""), "Then", "space");
		TestRuntime runtime = TestRuntime.Start(document);
		FakeEnvironment environment = runtime.Environment;

		environment.HandledAction = "Jump";
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.ActionPressed));
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.ActionReleased));
		environment.HandledAction = "Move";
		runtime.Invoke(ScriptEvent.ActionPressed);
		runtime.Invoke(ScriptEvent.ActionReleased);
		// Names match with their letter case.
		environment.HandledAction = "jump";
		runtime.Invoke(ScriptEvent.ActionPressed);
		environment.HandledAction = string.Empty;
		runtime.Invoke(ScriptEvent.ActionPressed);
		environment.PressedKeys.Add(KeyCode.Space);
		runtime.Invoke(ScriptEvent.Update, 0.016f);
		Assert.Equal(["jump pressed", "jump released", "move pressed", "space"], runtime.Records);
		Assert.Equal(VisualScriptStatus.Failure, runtime.Runtime.Invoke(TestRuntime.Hero, (ScriptEvent)11, 0.0f, 0));
	}

	[Fact]
	public void AGraphWithoutAnActionEventReadsNoActionName()
	{
		DocumentBuilder document = new();
		document.Graph.Writes(document.Graph.Node("Event.Update"), "Then", "updated");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.HandledAction = "Jump";

		for (int edge = 0; edge < 10; edge++)
		{
			Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.ActionPressed));
			Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.ActionReleased));
		}
		Assert.Equal(0, runtime.Environment.HandledActionReads);
		Assert.Empty(runtime.Records);
	}

	[Fact]
	public void ActionEventsNeedTheNameOfAnActionAndTakeOneNodePerAction()
	{
		static AnalyzedDocument Analyze(string nodes) => VisualScriptTesting.Analyze($$"""{ "Variables": [], "Functions": [], "Graph": { "Nodes": [{{nodes}}], "Links": [] } }""");

		AnalyzedDocument twice = Analyze("""
			{ "ID": 1, "Type": "Event.ActionPressed", "Settings": { "Action": "Jump" } },
			{ "ID": 2, "Type": "Event.ActionPressed", "Settings": { "Action": "Jump" } },
			{ "ID": 3, "Type": "Event.ActionPressed", "Settings": { "Action": "Move" } },
			{ "ID": 4, "Type": "Event.ActionReleased", "Settings": { "Action": "Jump" } },
			{ "ID": 5, "Type": "Event.ActionPressed", "Settings": { "Action": "jump" } }
			""");
		Assert.Equal(2u, VisualScriptTesting.Single(twice, DiagnosticCode.DuplicateEvent).Node);
		Assert.Single(VisualScriptTesting.Errors(twice));
		Assert.Equal(DiagnosticCode.MissingSetting, VisualScriptTesting.Single(Analyze("""{ "ID": 1, "Type": "Event.ActionPressed" }"""), DiagnosticCode.MissingSetting).Code);
		foreach (string invalid in new[] { "\"\"", "3", "null" })
		{
			AnalyzedDocument document = Analyze($$"""{ "ID": 1, "Type": "Event.ActionReleased", "Settings": { "Action": {{invalid}} } }""");
			Assert.Equal(1u, VisualScriptTesting.Single(document, DiagnosticCode.InvalidSetting).Node);
		}
		Assert.True(BuiltinNodes.IsInputEvent(BuiltinNode.EventActionPressed));
		Assert.True(BuiltinNodes.IsInputEvent(BuiltinNode.EventActionReleased));
		Assert.True(BuiltinNodes.IsActionEvent(BuiltinNode.EventActionReleased));
		Assert.False(BuiltinNodes.IsActionEvent(BuiltinNode.EventGamepadButtonReleased));
		Assert.True(BuiltinNodes.IsEvent(BuiltinNode.EventActionReleased));
		NodeDefinition pressed = VisualScriptTesting.LibraryNode("Event.ActionPressed");
		Assert.Equal("Action Pressed", pressed.Title);
		Assert.Equal(SettingKind.String, pressed.FindSetting("Action")!.Kind);
		Assert.True(pressed.FindSetting("Action")!.Required);
		Assert.Equal("Action Released", VisualScriptTesting.LibraryNode("Event.ActionReleased").Title);
	}
}
