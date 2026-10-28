using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Flow.Timeline (spec-slice3 section 3.8): curves evaluated exactly, event keys firing on their time in the direction of
// play, the six inputs, Rate and Playing, the setting's checks with their paths, and where timelines may be.
public sealed class TimelineTests
{
	// The keys that start the timeline's inputs in the test graphs: P Play, F PlayFromStart, R Reverse, E ReverseFromEnd,
	// S Stop, T SetTime.
	private static readonly (KeyCode Key, string Input)[] s_Inputs =
	[
		(KeyCode.P, "Play"), (KeyCode.F, "PlayFromStart"), (KeyCode.R, "Reverse"), (KeyCode.E, "ReverseFromEnd"), (KeyCode.S, "Stop"),
		(KeyCode.T, "SetTime"),
	];

	// Event tracks K0, K5 and K1 with a key at 0, 0.5 and 1 (spec-slice3 section 3.8.4's table).
	private const string EventKeys =
		"""{ "Name": "K0", "Type": "exec", "Keys": [{ "Time": 0 }] }, { "Name": "K5", "Type": "exec", "Keys": [{ "Time": 0.5 }] }, { "Name": "K1", "Type": "exec", "Keys": [{ "Time": 1 }] }""";

	// A graph with a timeline of the setting: the keys start its inputs, Event.Create the input start names (none when
	// null), each exec output writes its name, and extra adds to the graph, given the timeline's ID and the write node of
	// each exec output (whose Then continues the chain).
	private sealed class TimelineGraph
	{
		internal TimelineGraph(string setting, string? start = "PlayFromStart", string inputs = "{}",
			Action<GraphBuilder, uint, Dictionary<string, uint>>? extra = null)
		{
			DocumentBuilder document = new();
			GraphBuilder graph = document.Graph;
			Node = graph.Node("Flow.Timeline", $$"""{ "Timeline": {{setting}} }""", inputs);
			foreach ((KeyCode key, string input) in s_Inputs)
			{
				graph.Flow(graph.Node("Event.KeyPressed", $$"""{ "Key": "{{key}}" }"""), Node, "Then", input);
			}
			if (start != null)
			{
				graph.Flow(graph.Node("Event.Create"), Node, "Then", start);
			}
			AnalyzedDocument analyzed = VisualScriptTesting.Analyze(RuntimeTesting.Library, document.Build());
			Dictionary<string, uint> writers = [];
			foreach (AnalyzedPin pin in VisualScriptTesting.Node(analyzed, Node).Pins.Where(static pin => !pin.IsInput && pin.Type.IsExec))
			{
				writers[pin.Name] = graph.Writes(Node, pin.Name, pin.Name);
			}
			extra?.Invoke(graph, Node, writers);
			Runtime = TestRuntime.Start(document);
			Runtime.Invoke(ScriptEvent.Create);
		}

		internal TestRuntime Runtime { get; }

		internal uint Node { get; }

		internal List<string> Records => Runtime.Records;

		// One Update dispatch with the step; the keys pressed in it start their inputs first.
		internal string[] Update(float step, params KeyCode[] keys)
		{
			int before = Records.Count;
			foreach (KeyCode key in keys)
			{
				Runtime.Environment.PressedKeys.Add(key);
			}
			Runtime.Invoke(ScriptEvent.Update, step);
			Runtime.Environment.PressedKeys.Clear();
			return [.. Records.Skip(before)];
		}

		internal object? Output(string pin) => OutputOf(Runtime, Node, pin);

		internal float Time => (float)Output("Time")!;

		internal bool Playing => (bool)Output("Playing")!;
	}

	// The value an output of a node of Hero's event graph holds.
	internal static object? OutputOf(TestRuntime runtime, uint id, string pin)
	{
		GraphInstance instance = runtime.Instance();
		CompiledGraph graph = instance.Program.EventGraph;
		CompiledNode node = graph.Nodes[graph.FindNode(id)];
		int output = Array.IndexOf(node.OutputPins, pin);
		Assert.True(output >= 0, $"{node} has no output {pin}");
		return SlotValues.ToValue(instance.Frame.Slots[node.OutputBase + output], node.OutputTypes[output]);
	}

	private static TimelineDefinition Read(string setting)
	{
		using JsonDocument json = JsonDocument.Parse(setting);
		List<string> problems = [];
		TimelineDefinition? timeline = TimelineReader.Read(json.RootElement, VisualScriptTesting.Library, (_, message) => problems.Add(message), []);
		Assert.True(timeline != null, string.Join("\n", problems));
		return timeline;
	}

	private static double[] Evaluate(TimelineTrack track, double time)
	{
		double[] value = new double[track.Components];
		track.Evaluate(time, value);
		return value;
	}

	[Fact]
	public void CurvesEvaluate()
	{
		TimelineDefinition timeline = Read("""
			{ "Length": 2, "Tracks": [
				{ "Name": "Auto", "Type": "float", "Keys": [{ "Time": 0, "Value": 0 }, { "Time": 1, "Value": 1 }] },
				{ "Name": "Linear", "Type": "float", "Keys": [{ "Time": 0, "Value": 0, "Interpolation": "Linear" }, { "Time": 1, "Value": 1 }] },
				{ "Name": "Constant", "Type": "float", "Keys": [{ "Time": 0, "Value": 0, "Interpolation": "Constant" }, { "Time": 1, "Value": 1 }] },
				{ "Name": "Tangents", "Type": "float", "Keys": [{ "Time": 0, "Value": 0, "Out": 2 }, { "Time": 1, "Value": 1, "In": 0 }] },
				{ "Name": "Three", "Type": "float", "Keys": [{ "Time": 0, "Value": 0 }, { "Time": 1, "Value": 1 }, { "Time": 2, "Value": 4 }] },
				{ "Name": "Vector", "Type": "Strada.Vector3", "Keys": [{ "Time": 0, "Value": [0, 0, 0] }, { "Time": 1, "Value": [1, 2, -1] }] },
				{ "Name": "Jump", "Type": "float", "Keys": [{ "Time": 0, "Value": 0, "Interpolation": "Linear" }, { "Time": 1, "Value": 1 }, { "Time": 1, "Value": 5, "Interpolation": "Linear" }, { "Time": 2, "Value": 7 }] },
				{ "Name": "Empty", "Type": "Strada.Color" },
				{ "Name": "Single", "Type": "Strada.Vector2", "Keys": [{ "Time": 1, "Value": [3, 4] }] }
			] }
			""");
		TimelineTrack Track(string name) => timeline.Curves.Single(track => track.Name == name);
		double[] times = [0.25, 0.5, 0.75, 1.0];
		Assert.Equal([0.15625, 0.5, 0.84375, 1.0], times.Select(time => Evaluate(Track("Auto"), time)[0]));
		Assert.Equal([0.25, 0.5, 0.75, 1.0], times.Select(time => Evaluate(Track("Linear"), time)[0]));
		Assert.Equal([0.0, 0.0, 0.0, 1.0], times.Select(time => Evaluate(Track("Constant"), time)[0]));
		Assert.Equal(0.75, Evaluate(Track("Tangents"), 0.5)[0]);
		Assert.Equal(0.25, Evaluate(Track("Three"), 0.5)[0]);
		Assert.Equal(2.75, Evaluate(Track("Three"), 1.5)[0]);
		Assert.Equal([0.15625, 0.3125, -0.15625], Evaluate(Track("Vector"), 0.25));
		Assert.Equal([1.0, 2.0, -1.0], Evaluate(Track("Vector"), 0.5).Select(static component => component / 0.5).ToArray());
		// Before the first key its value, after the last the last's; keys at one time jump.
		Assert.Equal([0.0, 0.0, 0.0], Evaluate(Track("Vector"), -1.0));
		Assert.Equal([1.0, 2.0, -1.0], Evaluate(Track("Vector"), 2.0));
		Assert.Equal(0.999, Evaluate(Track("Jump"), 0.999)[0], 12);
		Assert.Equal(5.0, Evaluate(Track("Jump"), 1.0)[0]);
		Assert.Equal(6.0, Evaluate(Track("Jump"), 1.5)[0]);
		// No key: the type default (white for a colour); one key: its value.
		Assert.Equal([1.0, 1.0, 1.0, 1.0], Evaluate(Track("Empty"), 0.5));
		Assert.Equal([3.0, 4.0], Evaluate(Track("Single"), 0.0));
		// The outputs are floats of the double result.
		Assert.Equal(new Vector3(0.15625f, 0.3125f, -0.15625f), Track("Vector").Evaluate(0.25).Vector3);
		Assert.Equal(new Color(1.0f, 1.0f, 1.0f, 1.0f), Track("Empty").Evaluate(0.0).Color);
	}

	public static TheoryData<bool, bool, float, string[][]> Sequences => new()
	{
		{
			true, false, 0.25f,
			[["K0", "Update"], ["K5", "Update"], ["Update"], ["K1", "Update", "Finished"], []]
		},
		{
			false, false, 0.25f,
			[["K1", "Update"], ["K5", "Update"], ["Update"], ["K0", "Update", "Finished"], []]
		},
		{
			true, true, 0.4f,
			[["K0", "Update"], ["K5", "Update"], ["K0", "Update"], ["K5", "Update"], ["K0", "Update"]]
		},
	};

	// Spec-slice3 section 3.8.4's table: PlayFromStart, ReverseFromEnd, and PlayFromStart of a loop.
	[Theory]
	[MemberData(nameof(Sequences))]
	public void EventsFireOnTheirTime(bool forward, bool loop, float step, string[][] dispatches)
	{
		TimelineGraph timeline = new($$"""{ "Length": 1, "Loop": {{(loop ? "true" : "false")}}, "Tracks": [{{EventKeys}}] }""",
			forward ? "PlayFromStart" : "ReverseFromEnd");
		Assert.Empty(timeline.Records);
		foreach (string[] expected in dispatches)
		{
			Assert.Equal(expected, timeline.Update(step));
		}
	}

	// Keys at one time fire by track, then key order; backward by time descending, ties in the same order.
	[Fact]
	public void KeysAtOneTimeFireInTrackAndKeyOrder()
	{
		const string setting = """
			{ "Length": 1, "Tracks": [
				{ "Name": "B", "Type": "exec", "Keys": [{ "Time": 0.25 }, { "Time": 0.5 }, { "Time": 0.5 }] },
				{ "Name": "A", "Type": "exec", "Keys": [{ "Time": 0.5 }, { "Time": 0.75 }] } ] }
			""";
		TimelineGraph forward = new(setting);
		Assert.Equal(["B", "B", "B", "A", "A", "Update", "Finished"], forward.Update(1.0f));
		TimelineGraph backward = new(setting, "ReverseFromEnd");
		Assert.Equal(["A", "B", "B", "A", "B", "Update", "Finished"], backward.Update(1.0f));
	}

	// Spec-slice3 section 3.8.4's table of inputs. The dispatches that start an input have no time step, so the advance
	// that follows in them moves nothing.
	[Fact]
	public void InputsFollowTheirTable()
	{
		TimelineGraph timeline = new("""{ "Length": 2 }""", null, """{ "NewTime": 1.5, "Rate": 1 }""");
		(float, TimelineDirection, bool) State() => (timeline.Time, (TimelineDirection)timeline.Output("Direction")!, timeline.Playing);
		// Before any input: at 0, forward, not playing.
		Assert.Equal((0.0f, TimelineDirection.Forward, false), State());

		timeline.Update(0.0f, KeyCode.P);
		Assert.Equal((0.0f, TimelineDirection.Forward, true), State());
		timeline.Update(0.5f);
		Assert.Equal(0.5f, timeline.Time);
		// Reverse turns it around where it is; ReverseFromEnd jumps to the end.
		timeline.Update(0.0f, KeyCode.R);
		Assert.Equal((0.5f, TimelineDirection.Backward, true), State());
		timeline.Update(0.25f);
		Assert.Equal(0.25f, timeline.Time);
		timeline.Update(0.0f, KeyCode.E);
		Assert.Equal((2.0f, TimelineDirection.Backward, true), State());
		// SetTime moves the time and keeps playing; PlayFromStart starts over forward.
		timeline.Update(0.0f, KeyCode.T);
		Assert.Equal((1.5f, TimelineDirection.Backward, true), State());
		timeline.Update(0.0f, KeyCode.F);
		Assert.Equal((0.0f, TimelineDirection.Forward, true), State());
		// Stop pauses: the time and the direction stay.
		timeline.Update(0.5f);
		timeline.Update(0.0f, KeyCode.S);
		Assert.Equal((0.5f, TimelineDirection.Forward, false), State());
		timeline.Update(0.5f);
		Assert.Equal(0.5f, timeline.Time);
	}

	// Play at the end of a timeline that does not loop finishes at the next update; PlayFromStart plays it again.
	[Fact]
	public void PlayAtTheEndFinishesAtOnce()
	{
		TimelineGraph timeline = new("""{ "Length": 1 }""");
		Assert.Equal(["Update", "Finished"], timeline.Update(2.0f));
		Assert.False(timeline.Playing);
		Assert.Empty(timeline.Update(0.25f, KeyCode.P));
		Assert.Equal(["Update", "Finished"], timeline.Update(0.25f));
		Assert.Equal(1.0f, timeline.Time);
		Assert.Empty(timeline.Update(0.25f, KeyCode.F));
		Assert.Equal(["Update"], timeline.Update(0.25f));
		Assert.Equal(0.25f, timeline.Time);
	}

	[Fact]
	public void StopKeepsTheTime()
	{
		TimelineGraph timeline = new($$"""{ "Length": 1, "Tracks": [{{EventKeys}}] }""");
		Assert.Equal(["K0", "Update"], timeline.Update(0.25f));
		Assert.Empty(timeline.Update(0.25f, KeyCode.S));
		Assert.Equal(0.25f, timeline.Time);
		Assert.Empty(timeline.Update(0.25f));
		// Play continues from there; the start is closed, so a key at the paused time would fire.
		Assert.Empty(timeline.Update(0.25f, KeyCode.P));
		Assert.Equal(["K5", "Update"], timeline.Update(0.25f));
		Assert.Equal(0.5f, timeline.Time);
	}

	[Fact]
	public void SetTimeFiresNothing()
	{
		TimelineGraph timeline = new($$"""{ "Length": 1, "Tracks": [{{EventKeys}}] }""", null, """{ "NewTime": 0.5 }""");
		Assert.Empty(timeline.Update(0.25f, KeyCode.T));
		Assert.Equal((0.5f, false), (timeline.Time, timeline.Playing));
		Assert.Empty(timeline.Update(0.25f));
		// Playing from a time SetTime set does fire the key there: Play closes the start.
		timeline.Update(0.0f, KeyCode.P);
		Assert.Equal(["K5", "Update"], timeline.Update(0.25f));
		// While it plays, SetTime opens the start: the key at the new time does not fire.
		Assert.Equal(["Update"], timeline.Update(0.1f, KeyCode.T));
		Assert.Equal(["Update"], timeline.Update(0.25f));
		Assert.Equal(0.85f, timeline.Time);
		// NaN and times outside the timeline are clamped.
		TimelineGraph clamped = new("""{ "Length": 2 }""", null, """{ "NewTime": 7 }""");
		clamped.Update(0.0f, KeyCode.T);
		Assert.Equal(2.0f, clamped.Time);
	}

	// A tick handler that calls an input ends the advance's ticks: an Update that stops fires no Finished, an event whose
	// handler stops fires no Update.
	[Fact]
	public void ATickHandlerThatStopsEndsTheAdvance()
	{
		TimelineGraph updateStops = new("""{ "Length": 1 }""", extra: static (graph, node, writers) => graph.Flow(writers["Update"], node, "Then", "Stop"));
		Assert.Equal(["Update"], updateStops.Update(2.0f));
		Assert.False(updateStops.Playing);
		Assert.Equal(1.0f, updateStops.Time);

		TimelineGraph eventStops = new($$"""{ "Length": 1, "Tracks": [{{EventKeys}}] }""",
			extra: static (graph, node, writers) => graph.Flow(writers["K5"], node, "Then", "Stop"));
		Assert.Equal(["K0", "Update"], eventStops.Update(0.25f));
		Assert.Equal(["K5"], eventStops.Update(0.25f));
		Assert.Empty(eventStops.Update(0.25f));
	}

	[Fact]
	public void FinishedThatRestartsPlaysAgain()
	{
		TimelineGraph timeline = new($$"""{ "Length": 1, "Tracks": [{{EventKeys}}] }""",
			extra: static (graph, node, writers) => graph.Flow(writers["Finished"], node, "Then", "PlayFromStart"));
		Assert.Equal(["K0", "K5", "K1", "Update", "Finished"], timeline.Update(1.0f));
		Assert.True(timeline.Playing);
		Assert.Equal(0.0f, timeline.Time);
		Assert.Equal(["K0", "Update"], timeline.Update(0.25f));
	}

	[Fact]
	public void LoopsNeverFinish()
	{
		TimelineGraph timeline = new($$"""{ "Length": 1, "Loop": true, "Tracks": [{{EventKeys}}] }""");
		// Several cycles in one update: the key at 0 fires in each, the key at Length never.
		Assert.Equal(["K0", "K5", "K0", "K5", "K0", "K5", "K0", "Update"], timeline.Update(3.25f));
		Assert.Equal(0.25f, timeline.Time);
		for (int update = 0; update < 20; update++)
		{
			Assert.DoesNotContain("Finished", timeline.Update(0.5f));
		}
		Assert.True(timeline.Playing);
		// Backward, a key at Length fires every cycle and one at 0 never; landing on 0 wraps to Length.
		TimelineGraph backward = new($$"""{ "Length": 1, "Loop": true, "Tracks": [{{EventKeys}}] }""", "ReverseFromEnd");
		Assert.Equal(["K1", "K5", "K1", "Update"], backward.Update(1.0f));
		Assert.Equal(1.0f, backward.Time);
		// A long step without event keys wraps without counting cycles.
		TimelineGraph quiet = new("""{ "Length": 0.001, "Loop": true }""");
		Assert.Equal(["Update"], quiet.Update(1.0e6f));
		Assert.InRange(quiet.Time, 0.0f, 0.001f);
	}

	[Fact]
	public void TheEventCapDropsTimeWithOneWarning()
	{
		string keys = string.Join(", ", Enumerable.Range(0, 1024).Select(static key => FormattableString.Invariant($$"""{ "Time": {{key / 1024.0}} }""")));
		TimelineGraph timeline = new($$"""{ "Length": 1, "Tracks": [{ "Name": "Many", "Type": "exec", "Keys": [{{keys}}] }] }""");
		string[] first = timeline.Update(1.0f);
		Assert.Equal(VisualScriptRuntime.MaxTimelineFirings, first.Count(static record => record == "Many"));
		Assert.Equal("Update", first[^1]);
		Assert.Equal(999.0f / 1024.0f, timeline.Time);
		Assert.True(timeline.Playing);
		string warning = Assert.Single(timeline.Runtime.Environment.Warnings);
		Assert.Equal(TestRuntime.On("Timeline", timeline.Node) + "more than 1000 timeline events were due in one update: the time beyond them is dropped", warning);
		// The next update fires the rest and finishes; the Warning is logged once per node per instance.
		string[] second = timeline.Update(1.0f);
		Assert.Equal(24, second.Count(static record => record == "Many"));
		Assert.Equal(["Update", "Finished"], second[^2..]);
		Assert.Single(timeline.Runtime.Environment.Warnings);
	}

	public static TheoryData<string, string, string> Settings => new()
	{
		{ "[]", nameof(DiagnosticCode.InvalidSetting), "Timeline: expected an object with Length, Loop and Tracks, not []" },
		{ """{ "Speed": 1 }""", nameof(DiagnosticCode.InvalidSetting), "Timeline: a timeline has only Length, Loop and Tracks, not \"Speed\"" },
		{ """{ "Length": 0 }""", nameof(DiagnosticCode.InvalidSetting), "Timeline.Length: expected a number greater than 0 and at most 3600" },
		{ """{ "Length": 3601 }""", nameof(DiagnosticCode.InvalidSetting), "Timeline.Length: expected a number greater than 0 and at most 3600" },
		{ """{ "Loop": 1 }""", nameof(DiagnosticCode.InvalidSetting), "Timeline.Loop: expected true or false, not 1" },
		{
			"""{ "Tracks": [{ "Name": "A", "Type": "int" }] }""", nameof(DiagnosticCode.InvalidSetting),
			"Timeline.Tracks[0].Type: expected float, Strada.Vector2, Strada.Vector3, Strada.Vector4, Strada.Color or exec"
		},
		{
			"""{ "Tracks": [{ "Name": "A", "Type": "exec" }, { "Name": "B", "Type": "exec" }, { "Name": "Update", "Type": "exec" }] }""",
			nameof(DiagnosticCode.InvalidSetting), "Timeline.Tracks[2].Name: Update is a pin of the timeline: name the track otherwise"
		},
		{
			"""{ "Tracks": [{ "Name": "Rate", "Type": "float" }] }""", nameof(DiagnosticCode.InvalidSetting),
			"Timeline.Tracks[0].Name: Rate is a pin of the timeline: name the track otherwise"
		},
		{
			"""{ "Tracks": [{ "Name": "Playing", "Type": "exec" }] }""", nameof(DiagnosticCode.InvalidSetting),
			"Timeline.Tracks[0].Name: Playing is a pin of the timeline: name the track otherwise"
		},
		{
			"""{ "Tracks": [{ "Name": "A", "Type": "exec" }, { "Name": "A", "Type": "float" }] }""", nameof(DiagnosticCode.InvalidSetting),
			"Timeline.Tracks[1].Name: a track named A comes earlier"
		},
		{ """{ "Tracks": [{ "Name": "a.b", "Type": "exec" }] }""", nameof(DiagnosticCode.InvalidName), "Timeline.Tracks[0].Name \"a.b\": a name cannot contain '.'" },
		{
			"""{ "Tracks": [{ "Name": "Then", "Type": "exec" }] }""", nameof(DiagnosticCode.ReservedName),
			"Timeline.Tracks[0].Name \"Then\": Then is a reserved pin name (Execute, Then, Target, Self, Default)"
		},
		{
			"""{ "Tracks": [{ "Name": "A", "Type": "float", "Keys": [{ "Time": 0, "Value": 0 }, { "Time": 0.75, "Value": 0 }, { "Time": 0.75, "Value": 0 }, { "Time": 0.5, "Value": 0 }] }] }""",
			nameof(DiagnosticCode.InvalidSetting), "Timeline.Tracks[0].Keys[3].Time: keys are in time order: 0.5 comes after 0.75"
		},
		{
			"""{ "Length": 2, "Tracks": [{ "Name": "A", "Type": "exec", "Keys": [{ "Time": 2.5 }] }] }""", nameof(DiagnosticCode.InvalidSetting),
			"Timeline.Tracks[0].Keys[0].Time: expected a number from 0 to 2, not 2.5"
		},
		{
			"""{ "Tracks": [{ "Name": "A", "Type": "float", "Keys": [{ "Time": 0, "Value": 0 }, { "Time": 1, "Value": 1, "Ease": 2 }] }] }""",
			nameof(DiagnosticCode.InvalidSetting), "Timeline.Tracks[0].Keys[1]: a key has only Time, Value, Interpolation, In and Out, not \"Ease\""
		},
		{
			"""{ "Tracks": [{ "Name": "A", "Type": "exec", "Keys": [{ "Time": 0, "Value": 1 }] }] }""", nameof(DiagnosticCode.InvalidSetting),
			"Timeline.Tracks[0].Keys[0]: an event key has only Time, not \"Value\""
		},
		{
			"""{ "Tracks": [{ "Name": "A", "Type": "Strada.Vector3", "Keys": [{ "Time": 0, "Value": [1, 2] }] }] }""", nameof(DiagnosticCode.InvalidSetting),
			"Timeline.Tracks[0].Keys[0].Value: expected an array of 3 finite numbers, not [1, 2]"
		},
		{
			"""{ "Tracks": [{ "Name": "A", "Type": "float", "Keys": [{ "Time": 0 }] }] }""", nameof(DiagnosticCode.InvalidSetting),
			"Timeline.Tracks[0].Keys[0]: a key of a float track needs a Value"
		},
		{
			"""{ "Tracks": [{ "Name": "A", "Type": "float", "Keys": [{ "Time": 0, "Value": 0, "Interpolation": "Smooth" }] }] }""",
			nameof(DiagnosticCode.InvalidSetting), "Timeline.Tracks[0].Keys[0].Interpolation: expected Cubic, Linear or Constant, not \"Smooth\""
		},
		{
			"""{ "Tracks": [{ "Name": "A", "Type": "Strada.Color", "Keys": [{ "Time": 0, "Value": [1, 1, 1, 1], "In": 1 }] }] }""",
			nameof(DiagnosticCode.InvalidSetting), "Timeline.Tracks[0].Keys[0].In: expected an array of 4 numbers, not 1"
		},
		{
			"""{ "Tracks": [{ "Name": "A", "Type": "float", "Keys": [{ "Time": 0, "Value": 0, "Out": [1] }] }] }""", nameof(DiagnosticCode.InvalidSetting),
			"Timeline.Tracks[0].Keys[0].Out: expected a number, not [1]"
		},
	};

	[Theory]
	[MemberData(nameof(Settings))]
	public void SettingsAreValidatedWithTheirPath(string setting, string code, string message)
	{
		AnalyzedDocument document = VisualScriptTesting.Analyze(VisualScriptTesting.Document($$"""{ "ID": 1, "Type": "Flow.Timeline", "Settings": { "Timeline": {{setting}} } }"""));
		Diagnostic diagnostic = Assert.Single(document.Diagnostics);
		Assert.Equal((code, DiagnosticSeverity.Error, 1u, message), (diagnostic.Code.ToString(), diagnostic.Severity, diagnostic.Node, diagnostic.Message));
		Assert.False(VisualScriptTesting.Node(document, 1).PinsComplete);
	}

	// A setting with problems keeps the pins of its valid tracks, so links to them stay.
	[Fact]
	public void TracksWithValidNamesKeepTheirPins()
	{
		AnalyzedDocument document = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Flow.Timeline", "Settings": { "Timeline": { "Tracks": [{ "Name": "Step", "Type": "exec", "Keys": [{ "Time": -1 }] }, { "Name": "Lift", "Type": "float" }] } } }"""));
		Assert.Equal(DiagnosticCode.InvalidSetting, Assert.Single(document.Diagnostics).Code);
		AnalyzedNode node = VisualScriptTesting.Node(document, 1);
		Assert.Equal(
			["Play", "PlayFromStart", "Reverse", "ReverseFromEnd", "Stop", "SetTime", "NewTime", "Rate", "Update", "Finished", "Step", "Time", "Direction", "Playing", "Lift"],
			node.Pins.Select(static pin => pin.Name));
		Assert.Equal(PinRole.Tick, node.FindPin("Step", PinDirection.Out)!.Description.Role);
	}

	public static TheoryData<string, float[]> Rates => new()
	{
		{ "2", [0.5f, 1.0f, 1.5f] },
		{ "0.5", [0.125f, 0.25f, 0.375f] },
		{ "0", [0.0f, 0.0f, 0.0f] },
		{ "-1", [0.0f, 0.0f, 0.0f] },
	};

	// Rate scales the time step (spec-slice3 section 3.8.4); 0, a negative and a non-finite rate hold the time still while
	// the timeline plays.
	[Theory]
	[MemberData(nameof(Rates))]
	public void RateScalesTheAdvance(string rate, float[] times)
	{
		TimelineGraph timeline = new("""{ "Length": 2 }""", "Play", $$"""{ "Rate": {{rate}} }""");
		foreach (float time in times)
		{
			Assert.Equal(["Update"], timeline.Update(0.25f));
			Assert.Equal(time, timeline.Time);
		}
		Assert.True(timeline.Playing);

		TimelineGraph notANumber = new("""{ "Length": 1 }""", "Play", extra: static (graph, node, _) =>
			graph.Link(graph.Node("Expression", """{ "Formula": "0.0 / 0.0" }"""), "Result", node, "Rate"));
		notANumber.Update(0.25f);
		Assert.Equal((0.0f, true), (notANumber.Time, notANumber.Playing));
	}

	// Rate is read by the inputs that play, NewTime by SetTime only: what feeds the other is not evaluated.
	[Fact]
	public void InputsReadOnlyTheirOwnValues()
	{
		TimelineGraph timeline = new("""{ "Length": 1 }""", null, extra: static (graph, node, _) =>
		{
			uint divide = graph.Node("Expression", """{ "Formula": "a / b" }""");
			graph.Link(graph.Literal("int", "1"), "Value", divide, "a");
			graph.Link(graph.Literal("int", "0"), "Value", divide, "b");
			graph.Link(divide, "Result", node, "NewTime");
		});
		timeline.Update(0.0f, KeyCode.P);
		Assert.True(timeline.Playing);
		Assert.Empty(timeline.Runtime.Environment.Errors);
		timeline.Update(0.0f, KeyCode.T);
		Assert.Contains("integer division by zero", timeline.Runtime.SingleError(), StringComparison.Ordinal);
	}

	[Fact]
	public void PlayingFollowsTheRun()
	{
		TimelineGraph timeline = new("""{ "Length": 1 }""", null, extra: static (graph, node, writers) =>
		{
			uint playing = graph.Write(node, "Playing");
			graph.Flow(writers["Finished"], playing);
			graph.Flow(playing, node, "Then", "PlayFromStart");
		});
		Assert.False(timeline.Playing);
		timeline.Update(0.0f, KeyCode.P);
		Assert.True(timeline.Playing);
		timeline.Update(0.0f, KeyCode.S);
		Assert.False(timeline.Playing);
		timeline.Update(0.0f, KeyCode.P);
		// Finished sees Playing false; its PlayFromStart plays again.
		Assert.Equal(["Update", "Finished", "False", "Update"], timeline.Update(2.0f).Concat(timeline.Update(0.25f)));
		Assert.True(timeline.Playing);
	}

	[Fact]
	public void TimelinesBelongInTheEventGraph()
	{
		string Function(bool pure) => $$"""{ "ID": 10, "Name": "Run", "Pure": {{(pure ? "true" : "false")}}, "Inputs": [], "Outputs": [], "Locals": [], "Graph": { "Nodes": [{ "ID": 11, "Type": "Function.Entry" }, { "ID": 12, "Type": "Flow.Timeline" }{{(pure ? """, { "ID": 13, "Type": "Function.Return" }""" : "")}}], "Links": [], "Comments": [] } }""";
		AnalyzedDocument impure = VisualScriptTesting.Analyze(VisualScriptTesting.Document("", functions: Function(false)));
		Assert.Equal(12u, VisualScriptTesting.Single(impure, DiagnosticCode.TickInFunction).Node);
		AnalyzedDocument pure = VisualScriptTesting.Analyze(VisualScriptTesting.Document("", functions: Function(true)));
		Assert.Equal(12u, VisualScriptTesting.Single(pure, DiagnosticCode.TickInFunction).Node);
		Assert.Equal(12u, VisualScriptTesting.Single(pure, DiagnosticCode.LatentInPureContext).Node);
		Assert.Equal(NodeKind.Latent, VisualScriptTesting.LibraryNode("Flow.Timeline").Kind);
	}

	// A disabled timeline ends the chain that reaches it (every exec output is a Tick output, an event track named
	// Completed included). Its data outputs follow s2 section 3.2 rule 2: Time takes NewTime, the first float input, and
	// the first float curve the next one, Rate; Playing and Direction take their defaults.
	[Fact]
	public void ADisabledTimelinePassesNothing()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint timeline = graph.Node("Flow.Timeline", """
			{ "Timeline": { "Tracks": [
				{ "Name": "Completed", "Type": "exec", "Keys": [{ "Time": 0 }] },
				{ "Name": "Lift", "Type": "float", "Keys": [{ "Time": 0, "Value": 5 }] },
				{ "Name": "Drop", "Type": "float", "Keys": [{ "Time": 0, "Value": 6 }] }
			] } }
			""", """{ "NewTime": 0.75, "Rate": 3 }""", "Disabled");
		graph.Flow(graph.Node("Event.Create"), timeline, "Then", "Play");
		graph.Writes(timeline, "Completed", "completed");
		graph.Writes(timeline, "Update", "update");
		uint text = graph.Node("FormatString", """{ "Format": "{Time} {Playing} {Direction} {Lift} {Drop}" }""");
		graph.Link(timeline, "Time", text, "Time");
		graph.Link(timeline, "Playing", text, "Playing");
		graph.Link(timeline, "Direction", text, "Direction");
		graph.Link(timeline, "Lift", text, "Lift");
		graph.Link(timeline, "Drop", text, "Drop");
		graph.Flow(graph.Node("Event.KeyPressed", """{ "Key": "W" }"""), graph.Write(text, "Result"));
		AnalyzedDocument analyzed = VisualScriptTesting.Analyze(RuntimeTesting.Library, document.Build());
		Assert.Null(VisualScriptTesting.Node(analyzed, timeline).PassThrough);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Empty(runtime.Records);
		runtime.Environment.PressedKeys.Add(KeyCode.W);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(["0.75 False Forward 3 0"], runtime.Records);
	}

	// During Event.Destroy the instance is gone: the playing inputs start no run (a Warning, once per node), the others still
	// apply.
	[Fact]
	public void DestroyStartsNoRun()
	{
		TimelineGraph timeline = new("""{ "Length": 1 }""", null, """{ "NewTime": 0.5 }""", static (graph, node, _) =>
		{
			uint sequence = graph.Node("Flow.Sequence", """{ "Count": 4 }""");
			graph.Flow(graph.Node("Event.Destroy"), sequence);
			graph.Flow(sequence, node, "Then0", "Play");
			graph.Flow(sequence, node, "Then1", "SetTime");
			graph.Flow(sequence, node, "Then2", "PlayFromStart");
			graph.Flow(sequence, graph.Write(node, "Time"), "Then3");
		});
		GraphInstance instance = timeline.Runtime.Instance();
		Assert.Equal(VisualScriptStatus.Success, timeline.Runtime.Invoke(ScriptEvent.Destroy));
		// PlayFromStart's other effect, the time set to 0, applies too.
		Assert.Equal(["0"], timeline.Records);
		Assert.Equal(0, instance.FiberCount);
		Assert.Equal(TestRuntime.On("Timeline", timeline.Node) + "the graph's instance was destroyed, so nothing can play",
			Assert.Single(timeline.Runtime.Environment.Warnings));
	}
}
