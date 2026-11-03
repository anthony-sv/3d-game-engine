using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text.Json;

namespace Strada.VisualScripting;

// What a recording records (spec-slice4 section 7.8, Record Start): the instance (null: each graph's target), the documents
// and node IDs within them whose steps and values it keeps (null: all), whether it keeps values, whether it aggregates a
// summary instead of listing, and the caps of the list.
internal sealed record DebugRecordingSettings(ulong? Entity, IReadOnlySet<ulong>? Documents, IReadOnlySet<uint>? Nodes, bool Values, bool Summary,
	int MaxSteps, int MaxValues);

// graph.trace's recording (spec-slice4 sections 7.5 and 17.2): the steps of Values runs and their values (the staged inputs
// and the outputs of impure nodes as they leave, the outputs of pure nodes as they are evaluated), filtered by document, node
// and instance, kept as slot copies and encoded when taken; or, with Summary, the counts per node and exit with their first
// and last frame and per pin the count, first, last, minimum and maximum value, aggregated as the steps come. Every step a
// Values run reports has a number (I, counting from 0, filtered steps included); a value belongs to the step of the
// instruction that computed it. Main thread only.
internal sealed class DebugRecording
{
	// Spec-slice4 section 7.8: the summary's entries at most.
	internal const int MaxSummaryEntries = 4096;
	// graph.trace's range of MaxSteps and MaxValues.
	internal const int MaxListed = 100_000;

	private readonly List<RecordedStep> m_Steps = [];
	private readonly List<RecordedValue> m_Values = [];
	private readonly Dictionary<CompiledGraph, GraphSummary> m_Summaries = [];
	// The summary the last step or value went to: most reach the graph of the one before.
	private GraphSummary? m_LastSummary;
	private int m_StepsLeftOut;
	private int m_ValuesLeftOut;

	internal DebugRecording(DebugRecordingSettings settings)
	{
		Settings = settings;
	}

	internal DebugRecordingSettings Settings { get; }

	// The frame index the steps from now on belong to (Record Frame).
	internal int CurrentFrame { get; set; }

	// The number of the next step a Values run reports.
	internal long NextStep { get; private set; }

	// Whether a run of the execution is recorded (a Values run): its instance passes the filter (the recording's entity, or the
	// graph's target) and its program has nodes of the recorded documents.
	internal bool Admits(Execution execution, VisualScriptDebugger debugger)
	{
		GraphInstance instance = execution.Instance;
		if (Settings.Entity is { } entity ? instance.Entity != entity : !debugger.PassesTarget(instance))
		{
			return false;
		}
		if (Settings.Documents is not { } documents || documents.Contains(instance.Graph))
		{
			return true;
		}
		foreach (CompiledFunction function in execution.Program.Functions)
		{
			if (function.Graph != 0 && documents.Contains(function.Graph))
			{
				return true;
			}
		}
		return false;
	}

	// A Values run left the node through the exit (-1: none): the step, and the node's staged inputs and outputs in the frame
	// that holds it. A function call reports twice: entering its function (its inputs) and returning through Then (its
	// outputs).
	internal void OnStep(Execution execution, CompiledNode node, int exit)
	{
		long step = NextStep++;
		if (node.IsSplit || !Passes(execution, node))
		{
			return;
		}
		Frame? frame = FrameOf(execution, node, exit);
		bool call = node.Op == Opcode.FunctionCall;
		int inputs = call && exit >= 0 ? 0 : node.InputCount;
		int outputs = call && exit < 0 ? 0 : node.OutputCount;
		if (Settings.Summary)
		{
			GraphSummary summary = SummaryOf(execution, node.Owner);
			summary.Step(node.ExitBase + (exit >= 0 ? exit : node.Exits.Length), CurrentFrame, step);
			if (Settings.Values && frame != null)
			{
				Span<Slot> slots = frame.Slots;
				for (int slot = node.InputBase; slot < node.InputBase + inputs; slot++)
				{
					summary.Value(slot, slots[slot], step);
				}
				for (int slot = node.OutputBase; slot < node.OutputBase + outputs; slot++)
				{
					summary.Value(slot, slots[slot], step);
				}
			}
			return;
		}
		if (m_Steps.Count < Settings.MaxSteps)
		{
			m_Steps.Add(new RecordedStep(step, CurrentFrame, execution.Serial, execution.Instance.Entity, execution.Instance.Graph, node, exit));
		}
		else
		{
			m_StepsLeftOut++;
		}
		if (Settings.Values && frame != null)
		{
			for (int input = 0; input < inputs; input++)
			{
				AddValue(new RecordedValue(step, node, input, true, frame.Slots[node.InputBase + input]));
			}
			AddOutputs(step, node, frame.Slots, outputs);
		}
	}

	// A Values run evaluated a pure node: its outputs belong to the step of the instruction that evaluated it.
	internal void OnEvaluated(Execution execution, CompiledNode node, Slot[] slots)
	{
		if (!Settings.Values || node.IsSplit || !Passes(execution, node))
		{
			return;
		}
		if (Settings.Summary)
		{
			GraphSummary summary = SummaryOf(execution, node.Owner);
			for (int slot = node.OutputBase; slot < node.OutputBase + node.OutputCount; slot++)
			{
				summary.Value(slot, slots[slot], NextStep);
			}
			return;
		}
		AddOutputs(NextStep, node, slots, node.OutputCount);
	}

	// Record Stop's reply members: the steps and values in order, or the summary, and what the caps left out.
	internal void Write(Utf8JsonWriter writer, IVisualScriptEnvironment environment, NodeLibrary library)
	{
		if (Settings.Summary)
		{
			WriteSummary(writer, new DebugValueWriter(writer, environment, library));
			return;
		}
		writer.WriteStartArray("Steps");
		foreach (RecordedStep step in m_Steps)
		{
			CompiledNode node = step.Node;
			writer.WriteStartObject();
			writer.WriteNumber("I", step.Index);
			writer.WriteNumber("Frame", step.Frame);
			writer.WriteNumber("Execution", step.Execution);
			WriteHandle(writer, "Entity", step.Entity);
			WriteHandle(writer, "Graph", step.Graph);
			WriteHandle(writer, "Document", ProgramTrace.DocumentOf(node.Owner, step.Graph));
			writer.WriteNumber("Function", node.Owner.Function?.Id ?? 0);
			writer.WriteNumber("Node", node.Id);
			writer.WriteString("Title", node.Title);
			WriteExit(writer, node, step.Exit);
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
		writer.WriteStartArray("Values");
		DebugValueWriter encoder = new(writer, environment, library);
		foreach (RecordedValue value in m_Values)
		{
			CompiledNode node = value.Node;
			VisualType type = value.Input ? node.InputTypes[value.Pin] : node.OutputTypes[value.Pin];
			writer.WriteStartObject();
			writer.WriteNumber("Step", value.Step);
			writer.WriteNumber("Node", node.Id);
			writer.WriteString("Pin", value.Input ? node.InputPins[value.Pin] : node.OutputPins[value.Pin]);
			writer.WriteString("Direction", value.Input ? "In" : "Out");
			encoder.WriteMembers(value.Slot, type);
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
		WriteTruncated(writer, m_StepsLeftOut, m_ValuesLeftOut);
	}

	private void AddOutputs(long step, CompiledNode node, Slot[] slots, int count)
	{
		for (int output = 0; output < count; output++)
		{
			AddValue(new RecordedValue(step, node, output, false, slots[node.OutputBase + output]));
		}
	}

	private void AddValue(in RecordedValue value)
	{
		if (m_Values.Count < Settings.MaxValues)
		{
			m_Values.Add(value);
		}
		else
		{
			m_ValuesLeftOut++;
		}
	}

	// Whether the node is one the recording keeps: of a recorded document, and of the recorded node IDs.
	private bool Passes(Execution execution, CompiledNode node)
	{
		if (Settings.Documents is { } documents && !documents.Contains(ProgramTrace.DocumentOf(node.Owner, execution.Instance.Graph)))
		{
			return false;
		}
		return Settings.Nodes is not { } nodes || nodes.Contains(node.Id);
	}

	// The frame that holds a node the execution reports: its top frame, or the caller's when the node is a call that just
	// entered its function.
	private static Frame? FrameOf(Execution execution, CompiledNode node, int exit)
	{
		int index = execution.CallCount - 1;
		if (node.Op == Opcode.FunctionCall && exit < 0 && index > 0 && execution.Calls[index].CallNode == node.Index)
		{
			index--;
		}
		Frame frame = execution.Calls[index].Frame;
		return ReferenceEquals(frame.Graph, node.Owner) ? frame : null;
	}

	private GraphSummary SummaryOf(Execution execution, CompiledGraph graph)
	{
		if (m_LastSummary is { } last && ReferenceEquals(last.Graph, graph))
		{
			return last;
		}
		if (!m_Summaries.TryGetValue(graph, out GraphSummary? summary))
		{
			summary = new GraphSummary(graph, execution.Instance.Graph);
			m_Summaries[graph] = summary;
		}
		m_LastSummary = summary;
		return summary;
	}

	// The summary of every compiled graph merged by graph, document and function (a live edit makes a new program; the steps
	// and values of one node ID, exit or pin add up), in that order; the entries beyond MaxSummaryEntries are counted.
	private void WriteSummary(Utf8JsonWriter writer, DebugValueWriter encoder)
	{
		SortedDictionary<(ulong Graph, ulong Document, uint Function), MergedSummary> merged = [];
		foreach (GraphSummary summary in m_Summaries.Values)
		{
			CompiledGraph graph = summary.Graph;
			(ulong, ulong, uint) key = (summary.Handle, ProgramTrace.DocumentOf(graph, summary.Handle), graph.Function?.Id ?? 0);
			if (!merged.TryGetValue(key, out MergedSummary? into))
			{
				into = new MergedSummary();
				merged[key] = into;
			}
			summary.MergeInto(into);
		}
		int steps = 0;
		int values = 0;
		int stepsLeftOut = 0;
		int valuesLeftOut = 0;
		writer.WriteStartObject("Summary");
		writer.WriteStartArray("Graphs");
		foreach (((ulong handle, ulong document, uint function), MergedSummary summary) in merged)
		{
			writer.WriteStartObject();
			WriteHandle(writer, "Graph", handle);
			WriteHandle(writer, "Document", document);
			writer.WriteNumber("Function", function);
			writer.WriteStartArray("Steps");
			foreach (MergedStep step in summary.OrderedSteps())
			{
				if (steps == MaxSummaryEntries)
				{
					stepsLeftOut++;
					continue;
				}
				steps++;
				writer.WriteStartObject();
				writer.WriteNumber("Node", step.Node.Id);
				writer.WriteString("Title", step.Node.Title);
				WriteExit(writer, step.Node, step.Exit);
				writer.WriteNumber("Count", step.Count);
				writer.WriteNumber("FirstFrame", step.FirstFrame);
				writer.WriteNumber("LastFrame", step.LastFrame);
				writer.WriteEndObject();
			}
			writer.WriteEndArray();
			writer.WriteStartArray("Values");
			foreach (MergedValue value in summary.OrderedValues())
			{
				if (values == MaxSummaryEntries)
				{
					valuesLeftOut++;
					continue;
				}
				values++;
				writer.WriteStartObject();
				writer.WriteNumber("Node", value.Node.Id);
				writer.WriteString("Pin", value.Pin);
				writer.WriteString("Direction", value.Input ? "In" : "Out");
				writer.WriteString("Type", value.Type.Id);
				writer.WriteNumber("Count", value.Count);
				encoder.WriteBare("First", value.First, value.Type);
				encoder.WriteBare("Last", value.Last, value.Type);
				if (value.HasRange)
				{
					encoder.WriteBare("Min", value.Min, value.Type);
					encoder.WriteBare("Max", value.Max, value.Type);
				}
				writer.WriteEndObject();
			}
			writer.WriteEndArray();
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
		writer.WriteEndObject();
		WriteTruncated(writer, stepsLeftOut + m_StepsLeftOut, valuesLeftOut + m_ValuesLeftOut);
	}

	private static void WriteExit(Utf8JsonWriter writer, CompiledNode node, int exit)
	{
		if (exit >= 0 && exit < node.ExitPins.Length)
		{
			writer.WriteString("Exit", node.ExitPins[exit]);
		}
		else
		{
			writer.WriteNull("Exit");
		}
	}

	private static void WriteTruncated(Utf8JsonWriter writer, int steps, int values)
	{
		writer.WriteStartObject("Truncated");
		writer.WriteNumber("Steps", steps);
		writer.WriteNumber("Values", values);
		writer.WriteEndObject();
	}

	private static void WriteHandle(Utf8JsonWriter writer, string name, ulong handle)
	{
		writer.WriteString(name, handle.ToString(CultureInfo.InvariantCulture));
	}

	// A step as it was reported: its number, the frame index, the execution, its instance and the node and exit.
	private readonly record struct RecordedStep(long Index, int Frame, ulong Execution, ulong Entity, ulong Graph, CompiledNode Node, int Exit);

	// A value as it was: the step it belongs to, the node, the input or output it is (Input: Pin indexes the staged inputs)
	// and a copy of its slot.
	private readonly record struct RecordedValue(long Step, CompiledNode Node, int Pin, bool Input, Slot Slot);

	// The summary of one compiled graph, by flow counter (CompiledNode.ExitBase + exit, the extra counter for no exit) and by
	// frame slot (the staged inputs and outputs of its nodes), so a step or value costs no lookup.
	private sealed class GraphSummary
	{
		private readonly StepSummary[] m_Steps;
		private readonly ValueSummary[] m_Values;
		// Which node and pin each frame slot is: the node's index, or -1 for slots of no pin (node state, scratch).
		private readonly int[] m_SlotNodes;

		internal GraphSummary(CompiledGraph graph, ulong handle)
		{
			Graph = graph;
			Handle = handle;
			m_Steps = new StepSummary[graph.CounterCount];
			m_Values = new ValueSummary[graph.Template.Length];
			m_SlotNodes = new int[graph.Template.Length];
			Array.Fill(m_SlotNodes, -1);
			foreach (CompiledNode node in graph.Nodes)
			{
				for (int slot = node.InputBase; slot < node.InputBase + node.InputCount; slot++)
				{
					m_SlotNodes[slot] = node.Index;
				}
				for (int slot = node.OutputBase; slot < node.OutputBase + node.OutputCount; slot++)
				{
					m_SlotNodes[slot] = node.Index;
				}
			}
		}

		internal CompiledGraph Graph { get; }

		// The graph handle of the instances whose executions ran the graph.
		internal ulong Handle { get; }

		internal void Step(int counter, int frame, long index)
		{
			ref StepSummary step = ref m_Steps[counter];
			if (step.Count++ == 0)
			{
				step.FirstFrame = frame;
				step.FirstStep = index;
			}
			step.LastFrame = frame;
		}

		internal void Value(int slot, in Slot value, long step)
		{
			ref ValueSummary summary = ref m_Values[slot];
			if (summary.Count++ == 0)
			{
				summary.First = value;
				summary.FirstStep = step;
				summary.Kind = KindOf(slot);
			}
			summary.Last = value;
			summary.LastStep = step;
			if (summary.Kind != RangeKind.None)
			{
				Widen(ref summary, value);
			}
		}

		internal void MergeInto(MergedSummary merged)
		{
			foreach (CompiledNode node in Graph.Nodes)
			{
				for (int exit = 0; exit <= node.Exits.Length; exit++)
				{
					StepSummary step = m_Steps[node.ExitBase + exit];
					if (step.Count > 0)
					{
						merged.AddStep(node, exit < node.Exits.Length ? exit : -1, step);
					}
				}
				for (int input = 0; input < node.InputCount; input++)
				{
					AddValue(merged, node, node.InputBase + input, node.InputPins[input], node.InputTypes[input], true);
				}
				for (int output = 0; output < node.OutputCount; output++)
				{
					AddValue(merged, node, node.OutputBase + output, node.OutputPins[output], node.OutputTypes[output], false);
				}
			}
		}

		private void AddValue(MergedSummary merged, CompiledNode node, int slot, string pin, VisualType type, bool input)
		{
			if (m_Values[slot].Count > 0)
			{
				merged.AddValue(node, pin, type, input, m_Values[slot]);
			}
		}

		private RangeKind KindOf(int slot)
		{
			CompiledNode node = Graph.Nodes[m_SlotNodes[slot]];
			VisualType type = slot >= node.OutputBase && slot < node.OutputBase + node.OutputCount ? node.OutputTypes[slot - node.OutputBase]
				: node.InputTypes[slot - node.InputBase];
			return RangeOf(type);
		}
	}

	// Which comparison a pin's minimum and maximum use: none for values that are not numbers.
	private enum RangeKind : byte
	{
		None,
		Int,
		UInt,
		Long,
		ULong,
		Float,
		Double,
	}

	private static RangeKind RangeOf(VisualType type) => !type.IsNumeric ? RangeKind.None : SlotValues.KindOf(type) switch
	{
		SlotKind.Int => RangeKind.Int,
		SlotKind.UInt => RangeKind.UInt,
		SlotKind.Long => RangeKind.Long,
		SlotKind.ULong => RangeKind.ULong,
		SlotKind.Float => RangeKind.Float,
		SlotKind.Double => RangeKind.Double,
		_ => RangeKind.None,
	};

	private struct StepSummary
	{
		internal uint Count;
		internal int FirstFrame;
		internal int LastFrame;
		internal long FirstStep;
	}

	private struct ValueSummary
	{
		internal uint Count;
		internal RangeKind Kind;
		internal bool HasRange;
		internal Slot First;
		internal Slot Last;
		internal Slot Min;
		internal Slot Max;
		internal long FirstStep;
		internal long LastStep;
	}

	// Widens the value's range by a number: NaN is no number of a range.
	private static void Widen(ref ValueSummary summary, in Slot value)
	{
		if (summary.Kind is RangeKind.Float or RangeKind.Double && double.IsNaN(summary.Kind == RangeKind.Float ? value.Float : value.Double))
		{
			return;
		}
		if (!summary.HasRange)
		{
			summary.Min = value;
			summary.Max = value;
			summary.HasRange = true;
			return;
		}
		if (Less(summary.Kind, value, summary.Min))
		{
			summary.Min = value;
		}
		if (Less(summary.Kind, summary.Max, value))
		{
			summary.Max = value;
		}
	}

	private static bool Less(RangeKind kind, in Slot a, in Slot b) => kind switch
	{
		RangeKind.Int => a.Int < b.Int,
		RangeKind.UInt => a.UInt < b.UInt,
		RangeKind.Long => a.Long < b.Long,
		RangeKind.ULong => a.ULong < b.ULong,
		RangeKind.Float => a.Float < b.Float,
		_ => a.Double < b.Double,
	};

	private sealed class MergedStep
	{
		internal required CompiledNode Node { get; init; }

		internal required int Exit { get; init; }

		internal uint Count { get; set; }

		internal int FirstFrame { get; set; }

		internal int LastFrame { get; set; }

		internal long FirstStep { get; set; }
	}

	private sealed class MergedValue
	{
		internal required CompiledNode Node { get; init; }

		internal required string Pin { get; init; }

		internal required VisualType Type { get; init; }

		internal required bool Input { get; init; }

		internal uint Count { get; set; }

		internal Slot First { get; set; }

		internal Slot Last { get; set; }

		internal long FirstStep { get; set; }

		internal long LastStep { get; set; }

		internal bool HasRange { get; set; }

		internal Slot Min { get; set; }

		internal Slot Max { get; set; }
	}

	// The summaries of one graph, document and function across compiled graphs: steps by node ID and exit pin, values by node
	// ID, pin and direction (of one type: a pin whose type an edit changed is another entry).
	private sealed class MergedSummary
	{
		private readonly Dictionary<(uint Node, string? Exit), MergedStep> m_Steps = [];
		private readonly Dictionary<(uint Node, string Pin, bool Input, string Type), MergedValue> m_Values = [];

		internal void AddStep(CompiledNode node, int exit, StepSummary step)
		{
			(uint, string?) key = (node.Id, exit >= 0 ? node.ExitPins[exit] : null);
			if (!m_Steps.TryGetValue(key, out MergedStep? merged))
			{
				m_Steps[key] = new MergedStep
				{
					Node = node,
					Exit = exit,
					Count = step.Count,
					FirstFrame = step.FirstFrame,
					LastFrame = step.LastFrame,
					FirstStep = step.FirstStep,
				};
				return;
			}
			merged.Count += step.Count;
			merged.FirstStep = Math.Min(merged.FirstStep, step.FirstStep);
			merged.FirstFrame = Math.Min(merged.FirstFrame, step.FirstFrame);
			merged.LastFrame = Math.Max(merged.LastFrame, step.LastFrame);
		}

		internal void AddValue(CompiledNode node, string pin, VisualType type, bool input, in ValueSummary value)
		{
			RangeKind kind = RangeOf(type);
			(uint, string, bool, string) key = (node.Id, pin, input, type.Id);
			if (!m_Values.TryGetValue(key, out MergedValue? merged))
			{
				m_Values[key] = new MergedValue
				{
					Node = node,
					Pin = pin,
					Type = type,
					Input = input,
					Count = value.Count,
					First = value.First,
					Last = value.Last,
					FirstStep = value.FirstStep,
					LastStep = value.LastStep,
					HasRange = value.HasRange,
					Min = value.Min,
					Max = value.Max,
				};
				return;
			}
			merged.Count += value.Count;
			if (value.FirstStep < merged.FirstStep)
			{
				merged.First = value.First;
				merged.FirstStep = value.FirstStep;
			}
			if (value.LastStep > merged.LastStep)
			{
				merged.Last = value.Last;
				merged.LastStep = value.LastStep;
			}
			if (value.HasRange)
			{
				if (!merged.HasRange)
				{
					merged.Min = value.Min;
					merged.Max = value.Max;
					merged.HasRange = true;
				}
				else
				{
					merged.Min = Less(kind, value.Min, merged.Min) ? value.Min : merged.Min;
					merged.Max = Less(kind, merged.Max, value.Max) ? value.Max : merged.Max;
				}
			}
		}

		// Steps in the order they first ran (a live edit's programs ran the same step number never twice).
		internal IEnumerable<MergedStep> OrderedSteps()
		{
			List<MergedStep> steps = [.. m_Steps.Values];
			steps.Sort(static (a, b) => a.FirstStep.CompareTo(b.FirstStep));
			return steps;
		}

		// Values in the order they were first seen.
		internal IEnumerable<MergedValue> OrderedValues()
		{
			List<MergedValue> values = [.. m_Values.Values];
			values.Sort(static (a, b) => a.FirstStep != b.FirstStep ? a.FirstStep.CompareTo(b.FirstStep)
				: a.Node.Id != b.Node.Id ? a.Node.Id.CompareTo(b.Node.Id)
				: string.CompareOrdinal(a.Pin, b.Pin));
			return values;
		}
	}
}
