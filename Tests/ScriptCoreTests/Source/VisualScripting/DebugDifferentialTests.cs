using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The debugger changes nothing a graph does (spec-slice4 section 20.2): over the shared fixtures and rewired versions of them
// (the fuzz mutator's), restricted to one instance, one event, no latent nodes and no input events, a run with a breakpoint on
// every node, continued after each dispatch, logs and ends as the plain run does; and Step Into from the first stop visits,
// in order, the instructions a recording debugger saw the plain run start.
public sealed class DebugDifferentialTests
{
	private const ulong Graph = 1;
	private const string Name = "Assets/Graph.sgraph";
	// A program whose plain run starts more instructions is not stepped through: it is a loop that runs to the budget.
	private const int MaxInstructions = 2000;

	// A debugger that stops nothing and records, in order, each instruction a run starts at a stop point (an Enter or a
	// Continue position): "<entity> <document>/<function>/<node> <place>". Implicit returns start none.
	private sealed class RecordingDebugger : IVisualScriptDebugger
	{
		internal List<string> Instructions { get; } = [];

		public bool Armed => true;

		public DebugMode Attach(Execution execution) => DebugMode.Break;

		public bool ShouldStop(Execution execution)
		{
			bool enter = execution.Node >= 0;
			CompiledNode node = execution.TopFrame.Graph.Nodes[enter ? execution.Node : execution.Flow[execution.FlowCount - 1].Node];
			Instructions.Add(Position(execution.Instance.Entity, node, enter ? DebugPlace.Enter : DebugPlace.Continue));
			return false;
		}

		public void OnStep(Execution execution, CompiledNode node, int exit)
		{
		}

		public void OnEvaluated(Execution execution, CompiledNode node, Slot[] slots)
		{
		}

		public void OnFrameReleasing(Execution execution, Frame frame)
		{
		}

		public void OnAborted(Execution execution)
		{
		}

		public void OnStoppedReleased(Execution execution, string reason)
		{
		}

		public void OnStoppedMigrated(Execution execution)
		{
		}

		public void OnParked(Execution execution)
		{
		}

		public void OnReleased(Execution execution)
		{
		}
	}

	private static string Position(ulong entity, CompiledNode node, DebugPlace place) =>
		string.Create(CultureInfo.InvariantCulture, $"{entity} {node.Owner.Graph}/{node.Owner.Function?.Id ?? 0}/{node.Id} {place}");

	// A run of a document: its log lines, what the stand-ins logged, the variables at the end and the instructions recorded.
	private sealed record Run(List<string> Lines, List<string> Instructions);

	// The documents the comparison runs, with the one event each dispatches: the fixtures and rewired versions that compile,
	// have no latent node and no input event, and handle one of the events.
	private static IEnumerable<(string Name, byte[] Document, ScriptEvent Event)> Programs()
	{
		List<(string Name, byte[] Document)> candidates = [];
		foreach ((string name, JsonNode document) in FuzzLibrary.Fixtures)
		{
			candidates.Add((name, Encoding.UTF8.GetBytes(document.ToJsonString())));
		}
		DocumentMutator mutator = new(0x44454255474446u);
		int rounds = Fuzzing.GetRounds(60);
		for (int round = 0; round < rounds; round++)
		{
			(string name, JsonNode document) = FuzzLibrary.Fixtures[round % FuzzLibrary.Fixtures.Count];
			JsonNode? rewired = mutator.Rewire(document, round);
			if (rewired != null)
			{
				candidates.Add((FormattableString.Invariant($"{name} (rewired in round {round})"), Encoding.UTF8.GetBytes(rewired.ToJsonString())));
			}
		}
		foreach ((string name, byte[] document) in candidates)
		{
			VisualScriptRuntime runtime = new(FuzzLibrary.CreateEnvironment(), () => FuzzLibrary.Library);
			runtime.Load(Graph, Name, document);
			if (runtime.FindLatestProgram(Graph) is not { IsBroken: false } program || program.InputEvents.Length > 0 ||
				BreakpointConditions.GraphsOf(program).Any(static graph => graph.Nodes.Any(static node => node.Op == Opcode.Latent)))
			{
				continue;
			}
			ScriptEvent? handled = null;
			foreach (ScriptEvent scriptEvent in (ReadOnlySpan<ScriptEvent>)[ScriptEvent.Create, ScriptEvent.Update, ScriptEvent.FixedUpdate, ScriptEvent.CollisionEnter])
			{
				if (program.Events[(int)scriptEvent] >= 0)
				{
					handled = scriptEvent;
					break;
				}
			}
			if (handled is { } chosen)
			{
				yield return (name, document, chosen);
			}
		}
	}

	// Runs the document's event twice on Hero: plainly with the recording debugger, with breakpoints on every node (each stop
	// continued), or stepping into everything from a breakpoint on each event node.
	private static Run Execute(byte[] document, ScriptEvent scriptEvent, string mode)
	{
		FakeEnvironment environment = FuzzLibrary.CreateEnvironment();
		VisualScriptRuntime runtime = new(environment, () => FuzzLibrary.Library);
		runtime.Load(Graph, Name, document);
		Assert.Equal(VisualScriptStatus.Success, runtime.CreateInstance(FuzzLibrary.Hero, Graph, []));
		RecordingDebugger recording = new();
		VisualScriptDebugger? debugger = null;
		if (mode == "plain")
		{
			runtime.AttachDebugger(recording);
		}
		else
		{
			debugger = new VisualScriptDebugger(runtime);
			runtime.AttachDebugger(debugger);
			GraphProgram program = runtime.FindInstance(FuzzLibrary.Hero)!.Program;
			List<DebugBreakpointSetting> breakpoints = [];
			foreach (CompiledGraph graph in BreakpointConditions.GraphsOf(program))
			{
				foreach (CompiledNode node in graph.Nodes)
				{
					if (!node.IsSplit && (mode == "breakpoints" || node.Op == Opcode.Event))
					{
						breakpoints.Add(new DebugBreakpointSetting(ProgramTrace.DocumentOf(graph, Graph), node.Id, node.Type, true, string.Empty, 0));
					}
				}
			}
			debugger.Configure(DebugTesting.Configuration(breakpoints));
		}
		List<string> instructions = [];
		FuzzStandIns.Messages = [];
		try
		{
			for (int dispatch = 0; dispatch < 2; dispatch++)
			{
				runtime.Invoke(FuzzLibrary.Hero, scriptEvent, 0.25f, FuzzLibrary.Other);
				while (debugger?.Current is { } stop)
				{
					Assert.True(stop.Resumable, $"stop {stop.Id} is {stop.Reason}: {stop.Message}");
					instructions.Add(Position(stop.Entity, stop.Leaf.Node!, stop.Place));
					if (mode == "steps")
					{
						debugger.Step(stop, ResumeAction.StepInto);
					}
					else
					{
						debugger.Continue(stop);
					}
				}
			}
			List<string> lines = [.. environment.Logs.Select(static log => $"{log.Level}: {log.Message}"), .. FuzzStandIns.Messages];
			HostReply variables = runtime.DescribeVariables(FuzzLibrary.Hero);
			lines.Add(variables.Text != null ? Encoding.UTF8.GetString(variables.Text) : variables.Status.ToString());
			return new Run(lines, mode == "plain" ? recording.Instructions : instructions);
		}
		finally
		{
			FuzzStandIns.Messages = null;
		}
	}

	[Fact]
	public void BreakpointsOnEveryNodeAndSteppingChangeNothingTheGraphsDo()
	{
		int compared = 0;
		foreach ((string name, byte[] document, ScriptEvent scriptEvent) in Programs())
		{
			Run plain = Execute(document, scriptEvent, "plain");
			if (plain.Instructions.Count > MaxInstructions)
			{
				continue;
			}
			Run stopped = Execute(document, scriptEvent, "breakpoints");
			Assert.True(plain.Lines.SequenceEqual(stopped.Lines),
				$"{name} runs differently with breakpoints on every node:\n{string.Join("\n", plain.Lines)}\n---\n{string.Join("\n", stopped.Lines)}");
			Run steps = Execute(document, scriptEvent, "steps");
			Assert.True(plain.Lines.SequenceEqual(steps.Lines), $"{name} runs differently stepped into:\n{string.Join("\n", plain.Lines)}\n---\n{string.Join("\n", steps.Lines)}");
			Assert.True(plain.Instructions.SequenceEqual(steps.Instructions),
				$"Step Into visits other instructions of {name} than the plain run starts:\n{string.Join("\n", plain.Instructions)}\n---\n{string.Join("\n", steps.Instructions)}");
			compared++;
		}
		// The restrictions leave enough programs to compare.
		Assert.True(compared >= 8, FormattableString.Invariant($"only {compared} programs were compared"));
	}
}
