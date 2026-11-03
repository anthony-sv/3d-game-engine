using System;
using System.Buffers;
using System.Collections.Generic;
using System.Globalization;
using System.Text.Json;

namespace Strada.VisualScripting;

// What a live edit did to a graph's instances (spec-slice4 section 8.4): how the load went, what its carry plan carries
// (Variables and States, the same for every instance) and what happened to each instance's waits, stopped executions and
// Timelines.
internal enum LiveOutcome : byte
{
	// The instances run the new program (Instances may be 0).
	Swapped,
	// A running step of the old program holds the swap of some instances; a later Swapped with Deferred follows.
	Pending,
	// The document does not compile: the instances keep the program they run.
	Kept,
}

// One instance's part of a receipt: listed when the instance had waits, stopped executions or Timelines.
internal sealed class InstanceNote
{
	internal InstanceNote(ulong entity)
	{
		Entity = entity;
	}

	internal ulong Entity { get; }

	internal int WaitsKept { get; set; }

	internal int StopsKept { get; set; }

	// The waits and stopped executions the edit cancelled, with the VM's reason.
	internal List<NodeNote> Cancelled { get; } = [];

	internal List<TimelineNote> Timelines { get; } = [];

	internal bool IsEmpty => WaitsKept == 0 && StopsKept == 0 && Cancelled.Count == 0 && Timelines.Count == 0;
}

// A receipt: one per load of a graph handle, and one more for a deferred swap.
internal sealed class LiveEditReceipt
{
	// At most this many instances are noted; MoreInstances counts the others with something to note.
	internal const int MaxInstanceNotes = 64;

	internal ulong Serial { get; set; }

	internal required ulong Graph { get; init; }

	internal required string Name { get; init; }

	internal required LiveOutcome Outcome { get; init; }

	internal bool Deferred { get; init; }

	// The instances this receipt swapped, and those a running step of the old program holds.
	internal int Instances { get; init; }

	internal int Pending { get; init; }

	internal VariableReport Variables { get; init; } = VariableReport.None;

	internal int KeptStates { get; init; }

	internal IReadOnlyList<NodeNote> ResetStates { get; init; } = [];

	internal IReadOnlyList<InstanceNote> InstanceNotes { get; init; } = [];

	internal int MoreInstances { get; init; }

	// Kept: "<n> errors, first: <message> (node <id>)"; empty otherwise.
	internal string Errors { get; init; } = string.Empty;
}

internal enum LiveSeverity : byte
{
	Error,
	Warning,
}

// What a runtime problem is coalesced by (spec-slice4 section 8.2): the graph the instance runs, the document holding the node
// (that graph, or a library), the node, the entity and the severity.
internal readonly record struct LiveProblemKey(ulong Graph, ulong Document, uint Node, ulong Entity, LiveSeverity Severity);

// A call on the stack of the execution a problem happened in: the Function.Call node (its document, function and ID).
internal readonly record struct LiveCallFrame(ulong Document, uint Function, uint Node);

// What live edits did and what went wrong at run time, for the editor: a bounded log that the engine reads with
// VisualScriptTakeLiveEvents (spec-slice4 sections 8.2 and 8.4). Serials grow with every change; a problem that recurs is
// updated in place (Count, Message, Calls) and takes a new serial. Problems are counted until the engine clears the log
// (VisualScriptClearLiveEvents, when play starts), so a play session's counts run on across the scene loads it makes, which
// destroy every instance and create them again for the same entities. Main thread only, like the runtime.
internal sealed class LiveEventLog
{
	// The receipts kept; the oldest goes first.
	internal const int MaxEdits = 64;
	// The distinct problems kept; beyond, the occurrences of problems the log does not hold are counted in Dropped.
	internal const int MaxProblems = 512;

	private readonly Queue<LiveEditReceipt> m_Edits = new();
	private readonly Dictionary<LiveProblemKey, LiveProblem> m_Problems = [];

	internal ulong LastSerial { get; private set; }

	// Occurrences of problems that found the log full since it was last cleared. Each one is a change, with its serial, so
	// that a reader that took everything sees the count grow.
	internal int Dropped { get; private set; }

	internal int ProblemCount => m_Problems.Count;

	internal void AddEdit(LiveEditReceipt edit)
	{
		edit.Serial = ++LastSerial;
		if (m_Edits.Count == MaxEdits)
		{
			m_Edits.Dequeue();
		}
		m_Edits.Enqueue(edit);
	}

	// Coalesces by key: one entry per node and instance, with its count and its latest message and calls (innermost last).
	internal void AddProblem(in LiveProblemKey key, uint function, string title, string message, ReadOnlySpan<LiveCallFrame> calls)
	{
		if (!m_Problems.TryGetValue(key, out LiveProblem? problem))
		{
			if (m_Problems.Count == MaxProblems)
			{
				// Counts saturate: a long session that drops problems every frame never shows a negative count.
				Dropped = Math.Min(Dropped, int.MaxValue - 1) + 1;
				++LastSerial;
				return;
			}
			problem = new LiveProblem(key);
			m_Problems[key] = problem;
		}
		problem.Serial = ++LastSerial;
		problem.Function = function;
		problem.Title = title;
		problem.Message = message;
		problem.Calls = calls.ToArray();
		problem.Count = Math.Min(problem.Count, int.MaxValue - 1) + 1;
	}

	// A new play session starts (the engine clears its own copy of the log at the same time): the receipts, the problems and
	// Dropped go, and a problem that happens from now on counts from 1. Serials go on growing, so the reader's last serial
	// stays the one to read after.
	internal void Clear()
	{
		m_Edits.Clear();
		m_Problems.Clear();
		Dropped = 0;
	}

	// The events whose serial is after `after`, as JSON (spec-slice4 section 8.4), receipts and problems each by serial; null
	// when no serial is after it. A change that left no entry (a problem dropped, or entries cleared since) still answers, with
	// the last serial and Dropped.
	internal byte[]? Write(ulong after)
	{
		if (LastSerial <= after)
		{
			return null;
		}
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer, LibraryWriter.CreateOptions()))
		{
			writer.WriteStartObject();
			writer.WriteNumber("Last", LastSerial);
			writer.WriteNumber("Dropped", Dropped);
			writer.WriteStartArray("Edits");
			foreach (LiveEditReceipt edit in m_Edits)
			{
				if (edit.Serial > after)
				{
					WriteEdit(writer, edit);
				}
			}
			writer.WriteEndArray();
			List<LiveProblem> problems = [];
			foreach (LiveProblem problem in m_Problems.Values)
			{
				if (problem.Serial > after)
				{
					problems.Add(problem);
				}
			}
			problems.Sort(static (a, b) => a.Serial.CompareTo(b.Serial));
			writer.WriteStartArray("Problems");
			foreach (LiveProblem problem in problems)
			{
				WriteProblem(writer, problem);
			}
			writer.WriteEndArray();
			writer.WriteEndObject();
		}
		return buffer.WrittenSpan.ToArray();
	}

	private static void WriteEdit(Utf8JsonWriter writer, LiveEditReceipt edit)
	{
		writer.WriteStartObject();
		writer.WriteNumber("Serial", edit.Serial);
		writer.WriteString("Graph", Id(edit.Graph));
		writer.WriteString("Name", edit.Name);
		writer.WriteString("Outcome", edit.Outcome.ToString());
		writer.WriteBoolean("Deferred", edit.Deferred);
		writer.WriteNumber("Instances", edit.Instances);
		writer.WriteNumber("Pending", edit.Pending);
		VariableReport variables = edit.Variables;
		writer.WriteStartObject("Variables");
		WriteNames(writer, "Kept", variables.Kept);
		writer.WriteStartArray("Matched");
		foreach (MatchedVariable matched in variables.Matched)
		{
			writer.WriteStartObject();
			writer.WriteString("Name", matched.Name);
			writer.WriteString("From", matched.From);
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
		writer.WriteStartArray("Reset");
		foreach (ResetVariable reset in variables.Reset)
		{
			writer.WriteStartObject();
			writer.WriteString("Name", reset.Name);
			writer.WriteString("Reason", reset.Reason.ToString());
			if (reset.Reason == VariableResetReason.TypeChanged)
			{
				writer.WriteString("From", reset.From);
				writer.WriteString("To", reset.To);
			}
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
		WriteNames(writer, "Added", variables.Added);
		WriteNames(writer, "Removed", variables.Removed);
		writer.WriteEndObject();
		writer.WriteStartObject("States");
		writer.WriteNumber("Kept", edit.KeptStates);
		writer.WriteStartArray("Reset");
		foreach (NodeNote state in edit.ResetStates)
		{
			WriteNode(writer, edit.Graph, state, false);
		}
		writer.WriteEndArray();
		writer.WriteEndObject();
		writer.WriteStartArray("InstanceNotes");
		foreach (InstanceNote note in edit.InstanceNotes)
		{
			writer.WriteStartObject();
			writer.WriteString("Entity", Id(note.Entity));
			writer.WriteNumber("WaitsKept", note.WaitsKept);
			writer.WriteNumber("StopsKept", note.StopsKept);
			writer.WriteStartArray("Cancelled");
			foreach (NodeNote cancelled in note.Cancelled)
			{
				WriteNode(writer, edit.Graph, cancelled, true);
			}
			writer.WriteEndArray();
			writer.WriteStartArray("Timelines");
			foreach (TimelineNote timeline in note.Timelines)
			{
				writer.WriteStartObject();
				writer.WriteNumber("Node", timeline.Node);
				WriteFinite(writer, "Time", timeline.Time);
				if (timeline.ClampedFrom is { } clampedFrom)
				{
					WriteFinite(writer, "ClampedFrom", clampedFrom);
				}
				else
				{
					writer.WriteNull("ClampedFrom");
				}
				writer.WriteEndObject();
			}
			writer.WriteEndArray();
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
		writer.WriteNumber("MoreInstances", edit.MoreInstances);
		writer.WriteString("Errors", edit.Errors);
		writer.WriteEndObject();
	}

	// A node of the graph's own document has document 0 in the plan's notes: it is the receipt's graph.
	private static void WriteNode(Utf8JsonWriter writer, ulong graph, NodeNote node, bool cancelled)
	{
		writer.WriteStartObject();
		writer.WriteString("Document", Id(node.Document != 0 ? node.Document : graph));
		writer.WriteNumber("Function", node.Function);
		writer.WriteNumber("Node", node.Node);
		writer.WriteString("Title", node.Title);
		if (cancelled)
		{
			writer.WriteBoolean("Stopped", node.Stopped);
		}
		writer.WriteString("Reason", node.Reason);
		writer.WriteEndObject();
	}

	private static void WriteProblem(Utf8JsonWriter writer, LiveProblem problem)
	{
		LiveProblemKey key = problem.Key;
		writer.WriteStartObject();
		writer.WriteNumber("Serial", problem.Serial);
		writer.WriteString("Severity", key.Severity.ToString());
		writer.WriteString("Graph", Id(key.Graph));
		writer.WriteString("Document", Id(key.Document));
		writer.WriteNumber("Function", problem.Function);
		writer.WriteNumber("Node", key.Node);
		writer.WriteString("Title", problem.Title);
		writer.WriteString("Entity", Id(key.Entity));
		writer.WriteString("Message", problem.Message);
		writer.WriteNumber("Count", problem.Count);
		writer.WriteStartArray("Calls");
		foreach (LiveCallFrame call in problem.Calls)
		{
			writer.WriteStartObject();
			writer.WriteString("Document", Id(call.Document));
			writer.WriteNumber("Function", call.Function);
			writer.WriteNumber("Node", call.Node);
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
		writer.WriteEndObject();
	}

	private static void WriteNames(Utf8JsonWriter writer, string name, IReadOnlyList<string> names)
	{
		writer.WriteStartArray(name);
		foreach (string variable in names)
		{
			writer.WriteStringValue(variable);
		}
		writer.WriteEndArray();
	}

	// JSON has no infinities and no NaN: a time that is not finite is written as null.
	private static void WriteFinite(Utf8JsonWriter writer, string name, double value)
	{
		if (double.IsFinite(value))
		{
			writer.WriteNumber(name, value);
		}
		else
		{
			writer.WriteNull(name);
		}
	}

	// Handles and entity IDs are 64-bit: they cross as decimal strings, as the debugger's replies write them.
	private static string Id(ulong id) => id.ToString(CultureInfo.InvariantCulture);

	private sealed class LiveProblem
	{
		internal LiveProblem(LiveProblemKey key)
		{
			Key = key;
		}

		internal LiveProblemKey Key { get; }

		internal ulong Serial { get; set; }

		internal uint Function { get; set; }

		internal string Title { get; set; } = string.Empty;

		internal string Message { get; set; } = string.Empty;

		internal int Count { get; set; }

		internal LiveCallFrame[] Calls { get; set; } = [];
	}
}
