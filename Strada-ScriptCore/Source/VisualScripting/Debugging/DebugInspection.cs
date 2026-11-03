using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;

namespace Strada.VisualScripting;

// The protocol's commands that read values and instances (spec-slice4 sections 5.3 to 5.5 and 7.8): Describe (a frame of a
// stop), Values (an instance while the game runs), Instances (who runs which graph and what each waits for) and Record
// (graph.trace's recording).
internal static partial class DebugProtocol
{
	// Spec-slice4 sections 5.3 and 7.8: the values of one reply, the formulas of one Describe, the instances of one page.
	internal const int MaxReplyValues = 4096;
	internal const int MaxFormulas = 16;
	internal const int MaxInstancesPerPage = 500;
	internal const int DefaultInstancesPerPage = 100;

	// Describe: one frame of a stop's snapshot, innermost first across the chain (the frames State's Stop lists, a Native
	// boundary included), with the formulas of Evaluate evaluated against it.
	private static void Describe(VisualScriptRuntime runtime, JsonElement request, Utf8JsonWriter writer)
	{
		DebugRequest.ExpectMembers(request, string.Empty, "Command", "Stop", "Frame", "Evaluate");
		int id = DebugRequest.ReadId(request, "Stop", string.Empty);
		uint frame = DebugRequest.ReadUInt(request, "Frame", string.Empty);
		List<string> formulas = [];
		if (DebugRequest.Has(request, "Evaluate"))
		{
			int position = 0;
			foreach (JsonElement element in DebugRequest.ReadArray(request, "Evaluate", string.Empty))
			{
				string path = DebugRequest.Index("Evaluate", position++);
				string formula = DebugRequest.TextOf(element, path);
				if (JsonText.ByteCount(formula) > CompiledCondition.MaxBytes)
				{
					throw new DebugRequestException(DebugErrorCode.InvalidRequest,
						FormattableString.Invariant($"{path}: a formula has at most {CompiledCondition.MaxBytes} bytes"));
				}
				formulas.Add(formula);
			}
			if (formulas.Count > MaxFormulas)
			{
				throw new DebugRequestException(DebugErrorCode.InvalidRequest, FormattableString.Invariant($"Evaluate: at most {MaxFormulas} formulas"));
			}
		}
		VisualScriptDebugger debugger = Require(runtime);
		DebugStop stop = debugger.FindStop(id) ?? throw new DebugRequestException(DebugErrorCode.NotFound, "Stop: no stop of the ID");
		List<(int Member, int Frame)> frames = FramesOf(stop.Snapshot);
		if (frame >= frames.Count)
		{
			throw new DebugRequestException(DebugErrorCode.NotFound, FormattableString.Invariant($"Frame: stop {id} has {frames.Count} frames"));
		}
		(int member, int index) = frames[(int)frame];
		if (index < 0)
		{
			throw new DebugRequestException(DebugErrorCode.InvalidState,
				FormattableString.Invariant($"Frame: frame {frame} is engine code ({stop.Snapshot.Members[member].NativeAbove}): it holds no values"));
		}
		new FrameDescriber(writer, runtime, stop, member, index).Write(id, frame, formulas);
	}

	// The frames of a snapshot as State lists them: per member, innermost first, the Native boundary to the member above
	// first (Frame -1), then its frames from the top of its call stack down.
	private static List<(int Member, int Frame)> FramesOf(DebugSnapshot snapshot)
	{
		List<(int, int)> frames = [];
		for (int member = 0; member < snapshot.Members.Length; member++)
		{
			DebugSnapshotMember executions = snapshot.Members[member];
			if (executions.NativeAbove != null)
			{
				frames.Add((member, -1));
			}
			for (int index = executions.Frames.Length - 1; index >= 0; index--)
			{
				frames.Add((member, index));
			}
		}
		return frames;
	}

	// Values (spec-slice4 section 5.5): an instance's values between frames, without a debugger: the outputs of nodes in its
	// event graph's persistent frame (Function 0), or in the last call frame of a watched function of its document (any
	// entity's call: the reply's Entity is the caller's), its variables and its timelines. Node IDs the frame's graph has not
	// (the running program may be the last working version of an edited document) are left out.
	private static void Values(VisualScriptRuntime runtime, JsonElement request, Utf8JsonWriter writer)
	{
		DebugRequest.ExpectMembers(request, string.Empty, "Command", "Entity", "Function", "Nodes", "Variables", "Timelines");
		ulong entity = DebugRequest.ReadHandle(request, "Entity", string.Empty);
		uint function = DebugRequest.Has(request, "Function") ? DebugRequest.ReadUInt(request, "Function", string.Empty) : 0;
		List<uint> nodes = DebugRequest.Has(request, "Nodes") ? DebugRequest.ReadUIntArray(request, "Nodes", string.Empty) : [];
		bool variables = DebugRequest.Has(request, "Variables") && DebugRequest.ReadBool(request, "Variables", string.Empty);
		bool timelines = DebugRequest.Has(request, "Timelines") && DebugRequest.ReadBool(request, "Timelines", string.Empty);
		GraphInstance instance = runtime.FindInstance(entity) ?? throw new DebugRequestException(DebugErrorCode.NotFound, "Entity: no instance runs on the entity");
		GraphProgram program = instance.Program;
		if (function != 0 && program.FindFunction(0, function) == null)
		{
			throw new DebugRequestException(DebugErrorCode.NotFound, FormattableString.Invariant($"Function: the graph has no function {function}"));
		}
		CompiledGraph? graph;
		Slot[]? slots;
		string source;
		ulong owner = entity;
		if (function == 0)
		{
			graph = instance.Frame.Graph;
			slots = instance.Frame.Slots;
			source = "Frame";
		}
		else if (runtime.Debugger is VisualScriptDebugger debugger && debugger.FindLastFrame(instance.Graph, function) is { } last)
		{
			graph = last.Graph;
			slots = last.Slots;
			source = "LastCall";
			owner = last.Entity;
		}
		else
		{
			graph = null;
			slots = null;
			source = "None";
		}
		DebugValueWriter encoder = new(writer, runtime.Environment, runtime.Library);
		int written = 0;
		int left = 0;
		writer.WriteStartObject();
		WriteHandle(writer, "Entity", owner);
		WriteHandle(writer, "Graph", instance.Graph);
		writer.WriteString("Source", source);
		writer.WriteStartArray("Variables");
		if (variables)
		{
			CompiledVariable[] declared = program.Variables;
			for (int index = 0; index < declared.Length && index < instance.Variables.Length; index++)
			{
				if (!Admit(ref written, ref left))
				{
					continue;
				}
				CompiledVariable variable = declared[index];
				encoder.StartNamed(variable.Name, instance.Variables[index], variable.Type);
				writer.WriteNumber("ID", variable.Id);
				writer.WriteBoolean("Public", variable.Public);
				writer.WriteEndObject();
			}
		}
		writer.WriteEndArray();
		writer.WriteStartArray("Timelines");
		if (timelines)
		{
			WriteTimelines(writer, instance.Frame);
		}
		writer.WriteEndArray();
		writer.WriteStartArray("Nodes");
		if (graph != null && slots != null)
		{
			foreach (uint id in nodes)
			{
				int index = graph.FindNode(id);
				if (index < 0)
				{
					continue;
				}
				CompiledNode node = graph.Nodes[index];
				writer.WriteStartObject();
				writer.WriteNumber("ID", id);
				writer.WriteStartArray("Outputs");
				for (int output = 0; output < node.OutputCount; output++)
				{
					if (Admit(ref written, ref left))
					{
						encoder.StartNamed(node.OutputPins[output], slots[node.OutputBase + output], node.OutputTypes[output]);
						writer.WriteEndObject();
					}
				}
				writer.WriteEndArray();
				writer.WriteEndObject();
			}
		}
		writer.WriteEndArray();
		writer.WriteNumber("Truncated", left);
		writer.WriteEndObject();
	}

	// Whether one more value fits the reply; one that does not is counted.
	private static bool Admit(ref int written, ref int left)
	{
		if (written < MaxReplyValues)
		{
			written++;
			return true;
		}
		left++;
		return false;
	}

	// The timelines of a frame: where each is, its length, whether it plays (its run waits in it) and in which direction.
	private static void WriteTimelines(Utf8JsonWriter writer, Frame frame)
	{
		foreach (CompiledNode node in frame.Graph.Nodes)
		{
			if (node.Timeline is not { } timeline)
			{
				continue;
			}
			TimelineState? state = frame.Slots[node.TimelineSlot].Object as TimelineState;
			bool playing = frame.Slots[node.StateSlot].Object is Execution run && run.SuspendedAt == node.Index && ReferenceEquals(run.TopFrame, frame);
			writer.WriteStartObject();
			writer.WriteNumber("Node", node.Id);
			writer.WriteNumber("Time", state?.Time ?? 0.0);
			writer.WriteNumber("Length", timeline.Definition.Length);
			writer.WriteBoolean("Playing", playing);
			writer.WriteBoolean("Reverse", state?.Direction == TimelineDirection.Backward);
			writer.WriteEndObject();
		}
	}

	// Instances: the instances that run a graph (or every one), in the order they were created, a page of them, each with
	// its state (Stopped: it holds a stop or a stopped execution; Waiting: a fiber waits in a latent node; else Idle), what
	// its fibers wait for and the stops of its executions. Needs no debugger.
	private static void Instances(VisualScriptRuntime runtime, JsonElement request, Utf8JsonWriter writer)
	{
		DebugRequest.ExpectMembers(request, string.Empty, "Command", "Graph", "Entity", "Offset", "Limit");
		ulong? graph = DebugRequest.Has(request, "Graph") ? DebugRequest.ReadHandle(request, "Graph", string.Empty) : null;
		ulong? entity = DebugRequest.Has(request, "Entity") ? DebugRequest.ReadHandle(request, "Entity", string.Empty) : null;
		uint offset = DebugRequest.Has(request, "Offset") ? DebugRequest.ReadUInt(request, "Offset", string.Empty) : 0;
		uint limit = DebugRequest.Has(request, "Limit") ? DebugRequest.ReadUInt(request, "Limit", string.Empty) : DefaultInstancesPerPage;
		if (limit is 0 or > MaxInstancesPerPage)
		{
			throw new DebugRequestException(DebugErrorCode.InvalidRequest, FormattableString.Invariant($"Limit: 1 to {MaxInstancesPerPage} instances"));
		}
		List<GraphInstance> instances = [.. runtime.Instances.Where(instance => (graph == null || instance.Graph == graph) &&
			(entity == null || instance.Entity == entity)).OrderBy(static instance => instance.Serial)];
		VisualScriptDebugger? debugger = runtime.Debugger as VisualScriptDebugger;
		writer.WriteStartObject();
		writer.WriteNumber("Total", instances.Count);
		writer.WriteStartArray("Instances");
		for (long index = offset; index < instances.Count && index < (long)offset + limit; index++)
		{
			GraphInstance instance = instances[(int)index];
			List<int> stops = debugger != null ? StopsOf(debugger, instance) : [];
			writer.WriteStartObject();
			WriteHandle(writer, "Entity", instance.Entity);
			WriteHandle(writer, "Graph", instance.Graph);
			writer.WriteString("State", stops.Count > 0 || instance.DebugFibers.Count > 0 ? "Stopped" : instance.FiberCount > 0 ? "Waiting" : "Idle");
			writer.WriteStartArray("Waits");
			foreach (Execution fiber in instance.Fibers)
			{
				WriteWait(writer, instance, fiber);
			}
			writer.WriteEndArray();
			writer.WriteStartArray("Stops");
			foreach (int stop in stops)
			{
				writer.WriteNumberValue(stop);
			}
			writer.WriteEndArray();
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
		writer.WriteEndObject();
	}

	// The stops of an instance: those made on it and those whose chain holds one of its executions.
	private static List<int> StopsOf(VisualScriptDebugger debugger, GraphInstance instance)
	{
		List<int> stops = [];
		foreach (DebugStop stop in debugger.Stops)
		{
			bool held = stop.Entity == instance.Entity;
			foreach (DebugChainMember member in stop.Chain)
			{
				held |= member.IsWaiting && ReferenceEquals(member.Execution.Instance, instance);
			}
			if (held)
			{
				stops.Add(stop.Id);
			}
		}
		return stops;
	}

	// What a fiber waits for: its latent node, the node's kind and, as the kind has them, the seconds left (Delay, Tween;
	// Interval: to its next tick), the updates left (WaitFrames) and the next tick's index (Interval).
	private static void WriteWait(Utf8JsonWriter writer, GraphInstance instance, Execution fiber)
	{
		if (fiber.SuspendedAt < 0)
		{
			return;
		}
		CompiledNode node = fiber.TopFrame.Graph.Nodes[fiber.SuspendedAt];
		writer.WriteStartObject();
		WriteHandle(writer, "Document", ProgramTrace.DocumentOf(node.Owner, instance.Graph));
		writer.WriteNumber("Function", node.Owner.Function?.Id ?? 0);
		writer.WriteNumber("Node", node.Id);
		writer.WriteString("Title", node.Title);
		writer.WriteString("Kind", WaitKind(node));
		double? remaining = node.Builtin switch
		{
			BuiltinNode.FlowDelay or BuiltinNode.FlowRetriggerableDelay or BuiltinNode.FlowTween => Math.Max(0.0, fiber.WaitLimit - fiber.WaitTime),
			BuiltinNode.FlowInterval when fiber.WaitLimit > 0.0 => Math.Max(0.0, fiber.WaitLimit - fiber.WaitTime),
			_ => null,
		};
		WriteNullableNumber(writer, "Remaining", remaining is { } seconds && double.IsFinite(seconds) ? seconds : null);
		if (node.Builtin == BuiltinNode.FlowWaitFrames)
		{
			writer.WriteNumber("Frames", Math.Max(0, fiber.WaitCount));
		}
		else
		{
			writer.WriteNull("Frames");
		}
		if (node.Builtin == BuiltinNode.FlowInterval)
		{
			writer.WriteNumber("Index", fiber.WaitIndex);
		}
		else
		{
			writer.WriteNull("Index");
		}
		writer.WriteEndObject();
	}

	private static string WaitKind(CompiledNode node) => node.Builtin switch
	{
		BuiltinNode.FlowDelay => "Delay",
		BuiltinNode.FlowRetriggerableDelay => "RetriggerableDelay",
		BuiltinNode.FlowWaitFrames => "WaitFrames",
		BuiltinNode.FlowWaitUntil => "WaitUntil",
		BuiltinNode.FlowTween => "Tween",
		BuiltinNode.FlowInterval => "Interval",
		BuiltinNode.FlowTimeline => "Timeline",
		_ => node.Type,
	};

	private static void WriteNullableNumber(Utf8JsonWriter writer, string name, double? value)
	{
		if (value is { } number)
		{
			writer.WriteNumber(name, number);
		}
		else
		{
			writer.WriteNull(name);
		}
	}

	// Record: Start arms graph.trace's recording (replacing one armed before), Frame says which frame the following steps
	// belong to, Stop ends it and answers what it recorded.
	private static void Record(VisualScriptRuntime runtime, JsonElement request, Utf8JsonWriter writer)
	{
		DebugRequest.ExpectMembers(request, string.Empty, "Command", "Start", "Frame", "Stop");
		int forms = (DebugRequest.Has(request, "Start") ? 1 : 0) + (DebugRequest.Has(request, "Frame") ? 1 : 0) + (DebugRequest.Has(request, "Stop") ? 1 : 0);
		if (forms != 1)
		{
			throw new DebugRequestException(DebugErrorCode.InvalidRequest, "Record: exactly one of Start, Frame and Stop");
		}
		if (DebugRequest.Has(request, "Start"))
		{
			DebugRecordingSettings settings = ReadRecording(request);
			Require(runtime).StartRecording(settings);
			writer.WriteStartObject();
			writer.WriteEndObject();
			return;
		}
		if (DebugRequest.Has(request, "Frame"))
		{
			uint frame = DebugRequest.ReadUInt(request, "Frame", string.Empty);
			if (frame > int.MaxValue)
			{
				throw new DebugRequestException(DebugErrorCode.InvalidRequest, "Frame: not a frame index");
			}
			DebugRecording recording = Require(runtime).Recording ?? throw NoRecording();
			recording.CurrentFrame = (int)frame;
			writer.WriteStartObject();
			writer.WriteEndObject();
			return;
		}
		if (!DebugRequest.ReadBool(request, "Stop", string.Empty))
		{
			throw new DebugRequestException(DebugErrorCode.InvalidRequest, "Stop: only true stops the recording");
		}
		VisualScriptDebugger debugger = Require(runtime);
		if (debugger.Recording == null)
		{
			throw NoRecording();
		}
		DebugRecording stopped = debugger.StopRecording()!;
		writer.WriteStartObject();
		stopped.Write(writer, runtime.Environment, runtime.Library);
		writer.WriteEndObject();
	}

	private static DebugRequestException NoRecording() => new(DebugErrorCode.InvalidState, "no recording runs: Record Start begins one");

	private static DebugRecordingSettings ReadRecording(JsonElement request)
	{
		const string path = "Start";
		JsonElement start = request.GetProperty(path);
		DebugRequest.ExpectObject(start, path);
		DebugRequest.ExpectMembers(start, path, "Entity", "Documents", "Nodes", "Values", "Summary", "MaxSteps", "MaxValues");
		ulong? entity = DebugRequest.IsNull(start, "Entity", path) ? null : DebugRequest.ReadHandle(start, "Entity", path);
		HashSet<ulong>? documents = null;
		if (!DebugRequest.IsNull(start, "Documents", path))
		{
			documents = [];
			int index = 0;
			foreach (JsonElement element in DebugRequest.ReadArray(start, "Documents", path))
			{
				documents.Add(DebugRequest.HandleOf(element, DebugRequest.Index($"{path}.Documents", index++)));
			}
		}
		HashSet<uint>? nodes = null;
		if (!DebugRequest.IsNull(start, "Nodes", path))
		{
			nodes = [.. DebugRequest.ReadUIntArray(start, "Nodes", path)];
			if (documents == null)
			{
				throw new DebugRequestException(DebugErrorCode.InvalidRequest, "Start.Nodes: node IDs repeat across documents: name the Documents");
			}
		}
		bool values = DebugRequest.ReadBool(start, "Values", path);
		bool summary = DebugRequest.ReadBool(start, "Summary", path);
		return new DebugRecordingSettings(entity, documents, nodes, values, summary, ReadCap(start, "MaxSteps"), ReadCap(start, "MaxValues"));
	}

	private static int ReadCap(JsonElement start, string name)
	{
		uint cap = DebugRequest.ReadUInt(start, name, "Start");
		return cap is >= 1 and <= DebugRecording.MaxListed ? (int)cap
			: throw new DebugRequestException(DebugErrorCode.InvalidRequest,
				FormattableString.Invariant($"Start.{name}: 1 to {DebugRecording.MaxListed}"));
	}

	// One frame of a stop described (spec-slice4 section 5.3): its node's inputs and outputs, its member's start event, its
	// function's parameters and locals, the instance's variables, its active loops, the other nodes the stopped execution
	// computed in it and the formulas evaluated against it, each value with its currency (section 5.2), Changed where the rest
	// of the frame changed it (section 4.7) and, for literals a live edit changed, Edited with Before (section 4.8); at most
	// MaxReplyValues values, Computed cut first.
	private sealed class FrameDescriber
	{
		private readonly Utf8JsonWriter m_Writer;
		private readonly DebugValueWriter m_Encoder;
		private readonly DebugStop m_Stop;
		private readonly int m_MemberIndex;
		private readonly DebugSnapshotMember m_Member;
		private readonly DebugSnapshotFrame m_Frame;
		private readonly int m_FrameIndex;
		private readonly CompiledNode? m_Node;
		private readonly CompiledNode? m_Event;
		private readonly IVisualScriptEnvironment m_Environment;
		private int m_Written;
		private int m_Allowance = MaxReplyValues;
		private int m_Left;

		internal FrameDescriber(Utf8JsonWriter writer, VisualScriptRuntime runtime, DebugStop stop, int member, int frame)
		{
			m_Writer = writer;
			m_Environment = runtime.Environment;
			m_Encoder = new DebugValueWriter(writer, runtime.Environment, runtime.Library);
			m_Stop = stop;
			m_MemberIndex = member;
			m_Member = stop.Snapshot.Members[member];
			m_FrameIndex = frame;
			m_Frame = m_Member.Frames[frame];
			m_Node = frame == m_Member.Frames.Length - 1 ? m_Member.Node : m_Frame.Graph.Nodes[m_Member.Frames[frame + 1].CallNode];
			m_Event = EventOf(m_Member);
		}

		private CompiledGraph Graph => m_Frame.Graph;

		private CompiledFunction? Function => m_Frame.Graph.Function;

		internal void Write(int id, uint frame, List<string> formulas)
		{
			List<CompiledNode> computed = Computed();
			// Every section but Computed is kept whole when it can be: Computed gets what they leave.
			int others = CountOthers(formulas.Count);
			int computedValues = computed.Sum(static node => node.OutputCount);
			m_Allowance = Math.Max(0, MaxReplyValues - others);
			m_Writer.WriteStartObject();
			m_Writer.WriteNumber("Stop", id);
			m_Writer.WriteNumber("Frame", frame);
			WriteHandle(m_Writer, "Entity", m_Member.Entity);
			WriteHandle(m_Writer, "Graph", m_Member.Graph);
			WriteHandle(m_Writer, "Document", ProgramTrace.DocumentOf(Graph, m_Member.Graph));
			m_Writer.WriteNumber("Function", Function?.Id ?? 0);
			WriteNode();
			WriteEvent();
			WriteParameters();
			WriteLocals();
			WriteVariables();
			WriteLoops();
			WriteComputed(computed, Math.Min(computedValues, m_Allowance));
			WriteEvaluated(formulas);
			m_Writer.WriteNumber("Truncated", m_Left);
			m_Writer.WriteEndObject();
		}

		// The values of every section but Computed.
		private int CountOthers(int formulas)
		{
			int count = formulas + (m_Node != null ? m_Node.InputCount + m_Node.OutputCount : 0) + (m_Event?.OutputCount ?? 0);
			if (Function is { } function)
			{
				count += Graph.Nodes[function.EntryNode].OutputCount + function.Locals.Length;
			}
			count += Math.Min(m_Member.Variables.Length, m_Member.Program.Variables.Length);
			foreach (FlowEntry entry in Segment())
			{
				count += entry.Kind == FlowKind.ForEach ? 1 : 0;
			}
			return count;
		}

		// Whether one more value of a section fits: others first, then Computed within what they left.
		private bool Admit(bool computed)
		{
			if (computed ? m_Written < MaxReplyValues && m_Allowance > 0 : m_Written < MaxReplyValues)
			{
				m_Written++;
				if (computed)
				{
					m_Allowance--;
				}
				return true;
			}
			m_Left++;
			return false;
		}

		private void WriteNode()
		{
			m_Writer.WriteStartObject("Node");
			m_Writer.WriteNumber("ID", m_Node?.Id ?? 0);
			m_Writer.WriteStartArray("Inputs");
			if (m_Node is { } node)
			{
				// At an Enter position the node's plan has not run: each input is its source's value as the node will read it.
				bool pending = m_FrameIndex == m_Member.Frames.Length - 1 && m_Member.Place == DebugPlace.Enter && !m_Member.Waiting;
				List<DebugEdit> edits = Edits(node);
				for (int input = 0; input < node.InputCount; input++)
				{
					if (!Admit(computed: false))
					{
						continue;
					}
					string pin = node.InputPins[input];
					VisualType type = node.InputTypes[input];
					int edited = edits.FindIndex(edit => edit.Pin == pin);
					if (edited >= 0)
					{
						m_Encoder.StartNamed(pin, edits[edited].After, type);
						WriteCurrency(DebugCurrency.Current);
						m_Writer.WriteBoolean("Edited", true);
						m_Encoder.WriteBare("Before", edits[edited].Before, type);
						m_Writer.WriteEndObject();
						continue;
					}
					(Slot value, VisualType shown, DebugCurrency currency) = pending ? SourceOf(node, input) : (m_Frame.Slots[node.InputBase + input], type,
						CurrencyOf(node));
					m_Encoder.StartNamed(pin, value, shown);
					WriteCurrency(currency);
					m_Writer.WriteEndObject();
				}
			}
			m_Writer.WriteEndArray();
			m_Writer.WriteStartArray("Outputs");
			if (m_Node is { } owner)
			{
				WriteOutputs(owner, computed: false);
			}
			m_Writer.WriteEndArray();
			m_Writer.WriteEndObject();
		}

		// The literal inputs of the node a live edit changed since the snapshot, read from the program the execution runs now
		// (spec-slice4 section 4.8): only a migrated stop's members still waiting have any.
		private List<DebugEdit> Edits(CompiledNode node)
		{
			if (!m_Stop.Migrated || m_MemberIndex >= m_Stop.Chain.Count || !m_Stop.Chain[m_MemberIndex].IsWaiting)
			{
				return [];
			}
			Execution live = m_Stop.Chain[m_MemberIndex].Execution;
			return m_FrameIndex < live.CallCount ? DebugSnapshot.EditedInputs(node, live.Calls[m_FrameIndex].Frame.Graph) : [];
		}

		// An input of a node whose plan has not run: the value its operand loads (converted as the load converts it), with
		// the currency of the node that computed it; a literal as the program holds it; Target's Self as the instance's
		// entity.
		private (Slot Value, VisualType Type, DebugCurrency Currency) SourceOf(CompiledNode node, int input)
		{
			int target = node.InputBase + input;
			VisualType type = node.InputTypes[input];
			foreach (Operand operand in node.Operands)
			{
				if (operand.Target != target)
				{
					continue;
				}
				Slot value;
				DebugCurrency currency;
				switch (operand.Source)
				{
					case OperandSource.Slot:
						value = m_Frame.Slots[operand.Index];
						currency = NodeOfSlot(operand.Index) is { } source ? CurrencyOf(source) : DebugCurrency.NotComputed;
						break;
					default:
						int entry = FindLoop(operand.Index);
						currency = entry >= 0 ? DebugCurrency.Current : DebugCurrency.NotComputed;
						FlowEntry loop = entry >= 0 ? m_Member.Flow[entry] : default;
						value = operand.Source == OperandSource.LoopIndex ? Slot.FromInt(entry >= 0 ? loop.Index : 0)
							: entry >= 0 ? ArrayElements.Read(loop.Items!, loop.Index, operand.From)
							: Graph.Nodes[operand.Index].Constant;
						break;
				}
				if (operand.Conversion == ConversionOp.None)
				{
					return (value, type, currency);
				}
				Slot converted = value;
				try
				{
					Conversions.Apply(ref converted, operand.Conversion, operand.From);
					return (converted, type, currency);
				}
				catch (VisualScriptException)
				{
					// The conversion fails when the node runs: the source's value shows as it is.
					return (value, operand.From, currency);
				}
			}
			if (input == 0 && node.TargetIsSelf)
			{
				return (Slot.FromObject(m_Environment.GetEntity(m_Member.Entity)), type, DebugCurrency.Current);
			}
			return (Graph.Template[target], type, DebugCurrency.Current);
		}

		// The node whose output is the frame slot; null for a slot of no output.
		private CompiledNode? NodeOfSlot(int slot)
		{
			foreach (CompiledNode node in Graph.Nodes)
			{
				if (slot >= node.OutputBase && slot < node.OutputBase + node.OutputCount)
				{
					return node;
				}
			}
			return null;
		}

		private DebugCurrency CurrencyOf(CompiledNode node) => DebugValueWriter.CurrencyOf(m_Frame.Stamps, node.Index, m_Member.Serial);

		private void WriteCurrency(DebugCurrency currency)
		{
			m_Writer.WriteString("Currency", currency.ToString());
		}

		// The outputs of a node of the frame with their currency and, in the event graph's frame, Changed.
		private void WriteOutputs(CompiledNode node, bool computed)
		{
			DebugCurrency currency = CurrencyOf(node);
			for (int output = 0; output < node.OutputCount; output++)
			{
				if (!Admit(computed))
				{
					continue;
				}
				string pin = node.OutputPins[output];
				m_Encoder.StartNamed(pin, m_Frame.Slots[node.OutputBase + output], node.OutputTypes[output]);
				WriteCurrency(currency);
				WriteChanged(m_FrameIndex == 0 && IsChanged(node.Id, pin));
				m_Writer.WriteEndObject();
			}
		}

		private void WriteChanged(bool changed)
		{
			if (changed)
			{
				m_Writer.WriteBoolean("Changed", true);
			}
		}

		private bool IsChanged(uint node, string name) => m_Stop.Changed.Contains(new DebugChange(m_Member.Entity, node, name));

		// The member's start event: the node it started at (an event, a Tick or Timeline output) or, for a resumed fiber, the
		// node the suspended execution started at, when it is a node of the event graph.
		private static CompiledNode? EventOf(DebugSnapshotMember member)
		{
			CompiledGraph events = member.Frames[0].Graph;
			ExecutionStart start = member.Start;
			CompiledNode? node = start.Node;
			if (start.Kind == StartKind.Resume)
			{
				int index = start.OriginDocument == member.Graph ? events.FindNode(start.OriginNode) : -1;
				node = index >= 0 ? events.Nodes[index] : null;
			}
			return node != null && ReferenceEquals(node.Owner, events) ? node : null;
		}

		private void WriteEvent()
		{
			m_Writer.WriteStartArray("Event");
			if (m_Event is { } node)
			{
				DebugSnapshotFrame events = m_Member.Frames[0];
				DebugCurrency currency = DebugValueWriter.CurrencyOf(events.Stamps, node.Index, m_Member.Serial);
				for (int output = 0; output < node.OutputCount; output++)
				{
					if (!Admit(computed: false))
					{
						continue;
					}
					string pin = node.OutputPins[output];
					m_Encoder.StartNamed(pin, events.Slots[node.OutputBase + output], node.OutputTypes[output]);
					WriteCurrency(currency);
					WriteChanged(IsChanged(node.Id, pin));
					m_Writer.WriteEndObject();
				}
			}
			m_Writer.WriteEndArray();
		}

		private void WriteParameters()
		{
			m_Writer.WriteStartArray("Parameters");
			if (Function is { } function)
			{
				WriteOutputs(Graph.Nodes[function.EntryNode], computed: false);
			}
			m_Writer.WriteEndArray();
		}

		// A function's locals belong to its call alone: current when a traced run stamped the frame.
		private void WriteLocals()
		{
			m_Writer.WriteStartArray("Locals");
			if (Function is { } function)
			{
				foreach (LocalSlot local in function.Locals)
				{
					if (!Admit(computed: false))
					{
						continue;
					}
					m_Encoder.StartNamed(local.Name, m_Frame.Slots[local.Slot], local.Type);
					WriteCurrency(m_Frame.Stamps != null ? DebugCurrency.Current : DebugCurrency.Unknown);
					m_Writer.WriteEndObject();
				}
			}
			m_Writer.WriteEndArray();
		}

		// The instance's variables as the stop saw them, public and private: the instance's state, so always current.
		private void WriteVariables()
		{
			m_Writer.WriteStartArray("Variables");
			CompiledVariable[] variables = m_Member.Program.Variables;
			for (int index = 0; index < variables.Length && index < m_Member.Variables.Length; index++)
			{
				if (!Admit(computed: false))
				{
					continue;
				}
				CompiledVariable variable = variables[index];
				m_Encoder.StartNamed(variable.Name, m_Member.Variables[index], variable.Type);
				WriteCurrency(DebugCurrency.Current);
				WriteChanged(IsChanged(0, variable.Name));
				m_Writer.WriteNumber("ID", variable.Id);
				m_Writer.WriteBoolean("Public", variable.Public);
				m_Writer.WriteEndObject();
			}
			m_Writer.WriteEndArray();
		}

		// The flow entries of the frame's segment of the member's flow stack.
		private ArraySegment<FlowEntry> Segment()
		{
			int end = m_FrameIndex + 1 < m_Member.Frames.Length ? m_Member.Frames[m_FrameIndex + 1].FlowBase : m_Member.Flow.Length;
			int start = Math.Min(m_Frame.FlowBase, end);
			return new ArraySegment<FlowEntry>(m_Member.Flow, start, end - start);
		}

		private int FindLoop(int node)
		{
			ArraySegment<FlowEntry> segment = Segment();
			for (int index = segment.Count - 1; index >= 0; index--)
			{
				FlowEntry entry = segment[index];
				if (entry.Node == node && entry.Kind is FlowKind.ForLoop or FlowKind.ForEach)
				{
					return segment.Offset + index;
				}
			}
			return -1;
		}

		// Each active loop and Sequence of the frame, outermost first.
		private void WriteLoops()
		{
			m_Writer.WriteStartArray("Loops");
			foreach (FlowEntry entry in Segment())
			{
				CompiledNode node = Graph.Nodes[entry.Node];
				m_Writer.WriteStartObject();
				m_Writer.WriteNumber("Node", node.Id);
				m_Writer.WriteString("Kind", entry.Kind.ToString());
				switch (entry.Kind)
				{
					case FlowKind.ForLoop:
						m_Writer.WriteNumber("Index", entry.Index);
						m_Writer.WriteNumber("First", node.InputCount > 0 ? m_Frame.Slots[node.InputBase].Int : 0);
						m_Writer.WriteNumber("Last", entry.Last);
						break;
					case FlowKind.ForEach:
						Array items = entry.Items!;
						m_Writer.WriteNumber("Index", entry.Index);
						m_Writer.WriteNumber("Length", items.Length);
						if (ElementTypeOf(node) is { } element && entry.Index < items.Length && Admit(computed: false))
						{
							m_Writer.WritePropertyName("Element");
							m_Encoder.StartNamed("Element", ArrayElements.Read(items, entry.Index, element), element);
							m_Writer.WriteEndObject();
						}
						break;
					case FlowKind.Sequence:
						m_Writer.WriteString("Next", entry.Index < node.ExitPins.Length ? node.ExitPins[entry.Index] : string.Empty);
						break;
				}
				m_Writer.WriteEndObject();
			}
			m_Writer.WriteEndArray();
		}

		// The type of a ForEach's elements: its Element output's.
		private static VisualType? ElementTypeOf(CompiledNode node)
		{
			int output = Array.IndexOf(node.OutputPins, "Element");
			return output >= 0 ? node.OutputTypes[output] : node.InputTypes.Length > 0 ? node.InputTypes[0].Element : null;
		}

		// The other nodes with outputs the stopped execution computed in the frame: not the frame's node, its start event or
		// its function's Entry (their sections show them), nor the Make and Break nodes of split pins.
		private List<CompiledNode> Computed()
		{
			List<CompiledNode> nodes = [];
			if (m_Frame.Stamps is not { } stamps)
			{
				return nodes;
			}
			int entry = Function?.EntryNode ?? -1;
			foreach (CompiledNode node in Graph.Nodes)
			{
				if (stamps[node.Index] == m_Member.Serial && node.OutputCount > 0 && !node.IsSplit && !ReferenceEquals(node, m_Node) &&
					!ReferenceEquals(node, m_Event) && node.Index != entry)
				{
					nodes.Add(node);
				}
			}
			return nodes;
		}

		private void WriteComputed(List<CompiledNode> nodes, int allowance)
		{
			m_Allowance = allowance;
			m_Writer.WriteStartArray("Computed");
			foreach (CompiledNode node in nodes)
			{
				if (m_Allowance == 0)
				{
					m_Left += node.OutputCount;
					continue;
				}
				m_Writer.WriteStartObject();
				m_Writer.WriteNumber("Node", node.Id);
				m_Writer.WriteStartArray("Outputs");
				WriteOutputs(node, computed: true);
				m_Writer.WriteEndArray();
				m_Writer.WriteEndObject();
			}
			m_Writer.WriteEndArray();
		}

		private void WriteEvaluated(List<string> formulas)
		{
			m_Writer.WriteStartArray("Evaluated");
			foreach (string formula in formulas)
			{
				m_Writer.WriteStartObject();
				m_Writer.WriteString("Formula", formula);
				(Slot value, VisualType? type, string? error) = BreakpointConditions.Evaluate(formula, m_Member, m_FrameIndex);
				if (type == null)
				{
					m_Writer.WriteString("Error", error ?? string.Empty);
				}
				else if (Admit(computed: false))
				{
					m_Writer.WriteStartObject("Value");
					m_Encoder.WriteMembers(value, type);
					m_Writer.WriteEndObject();
				}
				else
				{
					m_Writer.WriteString("Error", FormattableString.Invariant($"left out: a reply holds at most {MaxReplyValues} values"));
				}
				m_Writer.WriteEndObject();
			}
			m_Writer.WriteEndArray();
		}
	}
}
