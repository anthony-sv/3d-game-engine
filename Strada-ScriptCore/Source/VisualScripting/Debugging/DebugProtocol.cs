using System;
using System.Buffers;
using System.Collections.Generic;
using System.Globalization;
using System.Text.Json;
using System.Text.Unicode;

namespace Strada.VisualScripting;

// The error codes of the protocol's error replies (spec-slice4 section 7.8).
internal enum DebugErrorCode : byte
{
	InvalidRequest,
	NotAttached,
	NotFound,
	InvalidState,
}

// A request the protocol refuses: answered as { "Error", "Code" }, never thrown past DebugProtocol.
internal sealed class DebugRequestException : Exception
{
	internal DebugRequestException(DebugErrorCode code, string message)
		: base(message)
	{
		Code = code;
	}

	internal DebugErrorCode Code { get; }
}

// Reading the members of requests: each read names the member's path in the error it throws (DebugRequestException with
// InvalidRequest) when the member is missing or of the wrong type.
internal static class DebugRequest
{
	internal static string Member(string path, string name) => path.Length == 0 ? name : $"{path}.{name}";

	internal static string Index(string path, int index) => string.Create(CultureInfo.InvariantCulture, $"{path}[{index}]");

	internal static void ExpectObject(JsonElement element, string path)
	{
		if (element.ValueKind != JsonValueKind.Object)
		{
			throw Invalid(path.Length == 0 ? "the request is not a JSON object" : $"{path}: not an object");
		}
	}

	// Every member of the object is one of the names, once.
	internal static void ExpectMembers(JsonElement element, string path, params ReadOnlySpan<string> names)
	{
		HashSet<string> seen = new(StringComparer.Ordinal);
		foreach (JsonProperty property in element.EnumerateObject())
		{
			string name = NameOf(property, path);
			string member = Member(path, name);
			if (names.IndexOf(name) < 0)
			{
				throw Invalid($"{member}: unknown member");
			}
			if (!seen.Add(name))
			{
				throw Invalid($"{member}: a second member of the name");
			}
		}
	}

	internal static bool Has(JsonElement element, string name, string path = "")
	{
		ExpectTextNames(element, path);
		return element.TryGetProperty(name, out _);
	}

	internal static uint ReadUInt(JsonElement element, string name, string path)
	{
		JsonElement value = Require(element, name, path);
		return value.ValueKind == JsonValueKind.Number && value.TryGetUInt32(out uint number) ? number
			: throw Invalid($"{Member(path, name)}: not an unsigned 32-bit integer");
	}

	internal static int ReadId(JsonElement element, string name, string path)
	{
		JsonElement value = Require(element, name, path);
		return value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int number) && number > 0 ? number
			: throw Invalid($"{Member(path, name)}: not a stop ID");
	}

	internal static bool ReadBool(JsonElement element, string name, string path)
	{
		JsonElement value = Require(element, name, path);
		return value.ValueKind switch
		{
			JsonValueKind.True => true,
			JsonValueKind.False => false,
			_ => throw Invalid($"{Member(path, name)}: not a boolean"),
		};
	}

	internal static string ReadString(JsonElement element, string name, string path)
	{
		JsonElement value = Require(element, name, path);
		return value.ValueKind == JsonValueKind.String ? StringOf(value, Member(path, name)) : throw Invalid($"{Member(path, name)}: not a string");
	}

	// A handle or an entity: a decimal string of an unsigned 64-bit integer.
	internal static ulong ReadHandle(JsonElement element, string name, string path)
	{
		JsonElement value = Require(element, name, path);
		return value.ValueKind == JsonValueKind.String &&
			ulong.TryParse(StringOf(value, Member(path, name)), NumberStyles.None, CultureInfo.InvariantCulture, out ulong handle) ? handle
			: throw Invalid($"{Member(path, name)}: not a decimal string of an unsigned 64-bit integer");
	}

	internal static JsonElement.ArrayEnumerator ReadArray(JsonElement element, string name, string path)
	{
		JsonElement value = Require(element, name, path);
		return value.ValueKind == JsonValueKind.Array ? value.EnumerateArray() : throw Invalid($"{Member(path, name)}: not an array");
	}

	// An array of unsigned 32-bit integers (node IDs).
	internal static List<uint> ReadUIntArray(JsonElement element, string name, string path)
	{
		List<uint> numbers = [];
		int index = 0;
		foreach (JsonElement item in ReadArray(element, name, path))
		{
			string at = Index(Member(path, name), index++);
			numbers.Add(item.ValueKind == JsonValueKind.Number && item.TryGetUInt32(out uint number) ? number
				: throw Invalid($"{at}: not an unsigned 32-bit integer"));
		}
		return numbers;
	}

	// Whether a member that must be there is null.
	internal static bool IsNull(JsonElement element, string name, string path) => Require(element, name, path).ValueKind == JsonValueKind.Null;

	// A string element of an array; path names it.
	internal static string TextOf(JsonElement element, string path) =>
		element.ValueKind == JsonValueKind.String ? StringOf(element, path) : throw Invalid($"{path}: not a string");

	// A handle element of an array; path names it.
	internal static ulong HandleOf(JsonElement element, string path) => element.ValueKind == JsonValueKind.String &&
		ulong.TryParse(StringOf(element, path), NumberStyles.None, CultureInfo.InvariantCulture, out ulong handle) ? handle
		: throw Invalid($"{path}: not a decimal string of an unsigned 64-bit integer");

	// The texts of the request: escapes of lone surrogates make no string (JsonElement throws InvalidOperationException).
	private static string StringOf(JsonElement value, string member)
	{
		try
		{
			return value.GetString()!;
		}
		catch (InvalidOperationException)
		{
			throw Invalid($"{member}: not valid Unicode text");
		}
	}

	private static string NameOf(JsonProperty property, string path)
	{
		try
		{
			return property.Name;
		}
		catch (InvalidOperationException)
		{
			throw Invalid(path.Length == 0 ? "a member's name: not valid Unicode text" : $"{path}: a member's name: not valid Unicode text");
		}
	}

	private static JsonElement Require(JsonElement element, string name, string path)
	{
		ExpectTextNames(element, path);
		return element.TryGetProperty(name, out JsonElement value) ? value : throw Invalid($"{Member(path, name)}: missing");
	}

	// A lookup by name reads every member name it passes, and throws at one that holds an escaped surrogate without its pair:
	// an object's names are checked before any of its members is looked up.
	private static void ExpectTextNames(JsonElement element, string path)
	{
		if (element.ValueKind == JsonValueKind.Object && !JsonText.HasTextNames(element))
		{
			throw Invalid(path.Length == 0 ? "a member's name: not valid Unicode text" : $"{path}: a member's name: not valid Unicode text");
		}
	}

	private static DebugRequestException Invalid(string message) => new(DebugErrorCode.InvalidRequest, message);
}

// The commands of VisualScriptDebug (spec-slice4 section 7.8): a request is a JSON object { "Command": "<name>", ... }; the
// reply is a JSON object, an error reply { "Error", "Code" } for anything refused. Handles and entities are decimal strings,
// node, function and stop IDs integers. Commands: Attach, Detach, Configure, State, Resume, Break, RunTo, CheckCondition,
// and those that read values and instances (DebugInspection.cs): Describe, Values, Instances and Record.
internal static partial class DebugProtocol
{
	private static readonly JsonDocumentOptions s_Options = new() { MaxDepth = 64 };

	internal static byte[] Handle(VisualScriptRuntime runtime, ReadOnlySpan<byte> request)
	{
		if (!Utf8.IsValid(request))
		{
			return Error(DebugErrorCode.InvalidRequest, "the request is not UTF-8 text");
		}
		JsonDocument document;
		try
		{
			document = JsonDocument.Parse(request.ToArray(), s_Options);
		}
		catch (JsonException exception)
		{
			return Error(DebugErrorCode.InvalidRequest, $"the request is not JSON: {exception.Message}");
		}
		using (document)
		{
			try
			{
				// A refused request writes nothing: its checks come before anything changes and before the reply is written.
				ArrayBufferWriter<byte> buffer = new();
				using (Utf8JsonWriter writer = new(buffer, LibraryWriter.CreateOptions()))
				{
					Dispatch(runtime, document.RootElement, writer);
				}
				return buffer.WrittenSpan.ToArray();
			}
			catch (DebugRequestException exception)
			{
				return Error(exception.Code, exception.Message);
			}
		}
	}

	private static byte[] Error(DebugErrorCode code, string message)
	{
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer, LibraryWriter.CreateOptions()))
		{
			writer.WriteStartObject();
			writer.WriteString("Error", message);
			writer.WriteString("Code", code.ToString());
			writer.WriteEndObject();
		}
		return buffer.WrittenSpan.ToArray();
	}

	private static void Dispatch(VisualScriptRuntime runtime, JsonElement request, Utf8JsonWriter writer)
	{
		DebugRequest.ExpectObject(request, string.Empty);
		string command = DebugRequest.ReadString(request, "Command", string.Empty);
		switch (command)
		{
			case "Attach":
				DebugRequest.ExpectMembers(request, string.Empty, "Command");
				if (runtime.Debugger is not VisualScriptDebugger)
				{
					runtime.AttachDebugger(new VisualScriptDebugger(runtime));
				}
				writer.WriteStartObject();
				writer.WriteEndObject();
				break;
			case "Detach":
				DebugRequest.ExpectMembers(request, string.Empty, "Command");
				RefuseWhileExecuting(runtime);
				int released = runtime.Debugger is VisualScriptDebugger ? runtime.AttachDebugger(null) : 0;
				writer.WriteStartObject();
				writer.WriteNumber("Released", released);
				writer.WriteEndObject();
				break;
			case "Configure":
				DebugConfiguration configuration = DebugConfiguration.Read(request);
				VisualScriptDebugger configured = Require(runtime);
				configured.Configure(configuration);
				writer.WriteStartObject();
				writer.WriteNumber("Revision", configuration.Revision);
				writer.WriteEndObject();
				break;
			case "State":
				DebugRequest.ExpectMembers(request, string.Empty, "Command");
				WriteState(writer, Require(runtime));
				break;
			case "Resume":
				Resume(runtime, request, writer);
				break;
			case "Break":
				Break(runtime, request, writer);
				break;
			case "RunTo":
				RunTo(runtime, request, writer);
				break;
			case "CheckCondition":
				CheckCondition(runtime, request, writer);
				break;
			case "Describe":
				Describe(runtime, request, writer);
				break;
			case "Values":
				Values(runtime, request, writer);
				break;
			case "Instances":
				Instances(runtime, request, writer);
				break;
			case "Record":
				Record(runtime, request, writer);
				break;
			default:
				throw new DebugRequestException(DebugErrorCode.InvalidRequest, $"Command: unknown command {JsonText.Quote(command)}");
		}
	}

	private static VisualScriptDebugger Require(VisualScriptRuntime runtime) => runtime.Debugger as VisualScriptDebugger ??
		throw new DebugRequestException(DebugErrorCode.NotAttached, "no debugger is attached");

	// Nothing continues or detaches while an execution runs: stopped executions continue only between frames.
	private static void RefuseWhileExecuting(VisualScriptRuntime runtime)
	{
		if (runtime.IsExecuting)
		{
			throw new DebugRequestException(DebugErrorCode.InvalidState, "an execution is running");
		}
	}

	private static void Resume(VisualScriptRuntime runtime, JsonElement request, Utf8JsonWriter writer)
	{
		DebugRequest.ExpectMembers(request, string.Empty, "Command", "Stop", "Action", "All");
		string name = DebugRequest.ReadString(request, "Action", string.Empty);
		if (!Enum.TryParse(name, ignoreCase: false, out ResumeAction action) || !Enum.IsDefined(action) || name != action.ToString())
		{
			throw new DebugRequestException(DebugErrorCode.InvalidRequest, $"Action: unknown action {JsonText.Quote(name)}");
		}
		bool all = DebugRequest.Has(request, "All") && DebugRequest.ReadBool(request, "All", string.Empty);
		bool hasStop = DebugRequest.Has(request, "Stop");
		if (all && hasStop)
		{
			throw new DebugRequestException(DebugErrorCode.InvalidRequest, "Stop: All continues every stop");
		}
		if (all && action != ResumeAction.Continue)
		{
			throw new DebugRequestException(DebugErrorCode.InvalidRequest, "All: only Continue resumes every stop; a step resumes one");
		}
		VisualScriptDebugger debugger = Require(runtime);
		RefuseWhileExecuting(runtime);
		ResumeResult result;
		if (all)
		{
			result = debugger.ContinueAll();
		}
		else
		{
			DebugStop stop = hasStop ? debugger.FindStop(DebugRequest.ReadId(request, "Stop", string.Empty)) ??
					throw new DebugRequestException(DebugErrorCode.NotFound, "Stop: no stop of the ID")
				: debugger.Current ?? throw new DebugRequestException(DebugErrorCode.NotFound, "no execution is stopped");
			if (action != ResumeAction.Continue && !stop.Resumable)
			{
				throw new DebugRequestException(DebugErrorCode.InvalidState,
					FormattableString.Invariant($"Stop: stop {stop.Id} is read-only ({stop.Reason}): it does not step, and Continue dismisses it"));
			}
			result = action == ResumeAction.Continue ? debugger.Continue(stop) : debugger.Step(stop, action);
		}
		writer.WriteStartObject();
		writer.WriteString("Outcome", result.Outcome.ToString());
		writer.WriteBoolean("Failed", result.Failed);
		if (result.Stop is { } reached)
		{
			writer.WriteNumber("Stop", reached.Id);
		}
		else
		{
			writer.WriteNull("Stop");
		}
		writer.WriteNumber("StopRevision", debugger.StopRevision);
		writer.WriteEndObject();
	}

	// Break (spec-slice4 section 4.5): arms the next stop point of a targeted execution that can stop, of one graph with a
	// Document, or cancels the armed Break and the step request.
	private static void Break(VisualScriptRuntime runtime, JsonElement request, Utf8JsonWriter writer)
	{
		DebugRequest.ExpectMembers(request, string.Empty, "Command", "Cancel", "Document");
		bool cancel = DebugRequest.ReadBool(request, "Cancel", string.Empty);
		bool hasDocument = DebugRequest.Has(request, "Document");
		ulong document = hasDocument ? DebugRequest.ReadHandle(request, "Document", string.Empty) : 0;
		if (cancel && hasDocument)
		{
			throw new DebugRequestException(DebugErrorCode.InvalidRequest, "Document: a cancel names no graph");
		}
		if (hasDocument && document == 0)
		{
			throw new DebugRequestException(DebugErrorCode.InvalidRequest, "Document: 0 is no graph: leave Document out to break in any graph");
		}
		VisualScriptDebugger debugger = Require(runtime);
		if (cancel)
		{
			debugger.CancelBreak();
		}
		else
		{
			debugger.ArmBreak(document);
		}
		writer.WriteStartObject();
		writer.WriteEndObject();
	}

	// RunTo (spec-slice4 section 2.5): arms a one-shot breakpoint on a node of a document; the caller then continues.
	private static void RunTo(VisualScriptRuntime runtime, JsonElement request, Utf8JsonWriter writer)
	{
		DebugRequest.ExpectMembers(request, string.Empty, "Command", "Document", "Node");
		ulong document = DebugRequest.ReadHandle(request, "Document", string.Empty);
		uint node = DebugRequest.ReadUInt(request, "Node", string.Empty);
		VisualScriptDebugger debugger = Require(runtime);
		if (!debugger.ArmRunTo(document, node))
		{
			throw new DebugRequestException(DebugErrorCode.InvalidState,
				FormattableString.Invariant($"Node: no loaded program has the node {node} of the document {document}"));
		}
		writer.WriteStartObject();
		writer.WriteEndObject();
	}

	// CheckCondition (spec-slice4 sections 2.3 and 7.8): compiles a condition for a node of a document against the program it
	// runs in (the graph's working program, for a library node a loaded program compiled with it), and lists the node's scope.
	// Needs no debugger.
	private static void CheckCondition(VisualScriptRuntime runtime, JsonElement request, Utf8JsonWriter writer)
	{
		DebugRequest.ExpectMembers(request, string.Empty, "Command", "Document", "Node", "Condition");
		ulong document = DebugRequest.ReadHandle(request, "Document", string.Empty);
		uint id = DebugRequest.ReadUInt(request, "Node", string.Empty);
		string condition = DebugRequest.ReadString(request, "Condition", string.Empty);
		if (JsonText.ByteCount(condition) > CompiledCondition.MaxBytes)
		{
			throw new DebugRequestException(DebugErrorCode.InvalidRequest,
				FormattableString.Invariant($"Condition: a condition has at most {CompiledCondition.MaxBytes} bytes"));
		}
		(CompiledGraph Graph, CompiledNode Node, GraphProgram Program)? found = FindNode(runtime, document, id);
		writer.WriteStartObject();
		writer.WriteBoolean("Checked", found != null);
		CompiledCondition? compiled = null;
		string? error = null;
		if (found is ({ } graph, { } node, { } program))
		{
			if (!BreakpointConditions.IsEntered(node))
			{
				error = condition.Length > 0 ? BreakpointConditions.PureCondition : null;
			}
			else
			{
				compiled = CompiledCondition.Compile(condition, graph, node, program, condition: true);
				error = compiled.Error;
			}
		}
		WriteNullable(writer, "Error", error);
		writer.WriteStartArray("Reads");
		foreach (string read in compiled?.Reads ?? [])
		{
			writer.WriteStringValue(read);
		}
		writer.WriteEndArray();
		writer.WriteStartArray("Scope");
		foreach (ConditionName name in compiled?.Scope.Readable ?? [])
		{
			writer.WriteStartObject();
			writer.WriteString("Name", name.Name);
			writer.WriteString("Form", name.Form);
			writer.WriteString("Type", name.Type.Id);
			writer.WriteString("Source", name.Source.ToString());
			writer.WriteNumber("Node", name.Node);
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
		writer.WriteEndObject();
	}

	// A node of a document in the program a condition on it is checked against: the working program of the script graph of
	// that handle, else a working or running program compiled with the library of that handle.
	private static (CompiledGraph Graph, CompiledNode Node, GraphProgram Program)? FindNode(VisualScriptRuntime runtime, ulong document, uint id)
	{
		List<(GraphProgram Program, ulong Handle)> programs = [];
		foreach ((ulong handle, GraphProgram program) in runtime.EffectivePrograms)
		{
			// The script graph's own working program is looked in first.
			programs.Insert(handle == document ? 0 : programs.Count, (program, handle));
		}
		foreach (GraphInstance instance in runtime.Instances)
		{
			programs.Add((instance.Program, instance.Graph));
		}
		foreach ((GraphProgram program, ulong handle) in programs)
		{
			if (BreakpointConditions.Find(program, handle, document, id) is ({ } graph, { } node))
			{
				return (graph, node, program);
			}
		}
		return null;
	}

	private static void WriteNullable(Utf8JsonWriter writer, string name, string? value)
	{
		if (value != null)
		{
			writer.WriteString(name, value);
		}
		else
		{
			writer.WriteNull(name);
		}
	}

	private static void WriteState(Utf8JsonWriter writer, VisualScriptDebugger debugger)
	{
		writer.WriteStartObject();
		writer.WriteNumber("Revision", debugger.Configuration.Revision);
		writer.WriteNumber("StopRevision", debugger.StopRevision);
		if (debugger.Current is { } current)
		{
			writer.WriteNumber("Current", current.Id);
		}
		else
		{
			writer.WriteNull("Current");
		}
		writer.WriteNumber("Passed", debugger.Passed);
		WriteStepping(writer, debugger.Stepping);
		writer.WriteStartArray("Stops");
		foreach (DebugStop stop in debugger.Stops)
		{
			WriteStop(writer, stop);
		}
		writer.WriteEndArray();
		writer.WriteStartArray("Breakpoints");
		List<(GraphProgram Program, ulong Handle)> programs = debugger.RunningPrograms();
		foreach (DebugBreakpoint breakpoint in debugger.Breakpoints)
		{
			DebugBreakpointSetting setting = breakpoint.Setting;
			BreakpointInactivity inactivity = VisualScriptDebugger.InactivityOf(setting, programs);
			writer.WriteStartObject();
			WriteHandle(writer, "Document", setting.Document);
			writer.WriteNumber("Node", setting.Node);
			writer.WriteNumber("Hits", breakpoint.Hits);
			writer.WriteNumber("Reached", breakpoint.Reached);
			writer.WriteBoolean("Active", inactivity == BreakpointInactivity.None);
			writer.WriteString("Reason", inactivity == BreakpointInactivity.None ? string.Empty : inactivity.ToString());
			WriteNullable(writer, "Error", breakpoint.Error);
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
		writer.WriteEndObject();
	}

	// The step that runs or waits: the stop it was made from, its action and where its chain waits (null while it does not).
	private static void WriteStepping(Utf8JsonWriter writer, DebugStepRequest? step)
	{
		if (step == null)
		{
			writer.WriteNull("Stepping");
			return;
		}
		writer.WriteStartObject("Stepping");
		writer.WriteNumber("Stop", step.Stop);
		writer.WriteString("Action", step.Action.ToString());
		if (step.WaitsIn is { } node)
		{
			Execution root = step.Root;
			writer.WriteStartObject("WaitsIn");
			WriteHandle(writer, "Document", ProgramTrace.DocumentOf(node.Owner, root.Instance.Graph));
			writer.WriteNumber("Function", node.Owner.Function?.Id ?? 0);
			writer.WriteNumber("Node", node.Id);
			writer.WriteString("Title", node.Title);
			WriteHandle(writer, "Entity", root.Instance.Entity);
			writer.WriteEndObject();
		}
		else
		{
			writer.WriteNull("WaitsIn");
		}
		writer.WriteEndObject();
	}

	private static void WriteStop(Utf8JsonWriter writer, DebugStop stop)
	{
		writer.WriteStartObject();
		writer.WriteNumber("ID", stop.Id);
		writer.WriteString("Reason", stop.Reason.ToString());
		writer.WriteBoolean("Resumable", stop.Resumable);
		WriteHandle(writer, "Entity", stop.Entity);
		WriteHandle(writer, "Graph", stop.Graph);
		writer.WriteString("Place", stop.Place.ToString());
		writer.WriteString("Message", stop.Message);
		writer.WriteBoolean("Migrated", stop.Migrated);
		writer.WriteStartArray("Changed");
		// Variables by name and outputs by node and pin: members of several instances may have seen the same ones change.
		HashSet<(uint, string)> listed = [];
		foreach (DebugChange change in stop.Changed)
		{
			if (!listed.Add((change.Node, change.Name)))
			{
				continue;
			}
			writer.WriteStartObject();
			writer.WriteNumber("Node", change.Node);
			writer.WriteString("Name", change.Name);
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
		writer.WriteNumber("ChangedMore", stop.ChangedMore);
		if (stop.Breakpoint is { } breakpoint)
		{
			writer.WriteStartObject("Breakpoint");
			WriteHandle(writer, "Document", breakpoint.Document);
			writer.WriteNumber("Node", breakpoint.Node);
			writer.WriteEndObject();
		}
		else
		{
			writer.WriteNull("Breakpoint");
		}
		writer.WriteStartArray("Frames");
		foreach (DebugSnapshotMember member in stop.Snapshot.Members)
		{
			WriteFrames(writer, member);
		}
		writer.WriteEndArray();
		WriteStart(writer, stop.Leaf);
		writer.WriteEndObject();
	}

	// A member's frames, innermost first, after the native boundary between it and the member above.
	private static void WriteFrames(Utf8JsonWriter writer, DebugSnapshotMember member)
	{
		if (member.NativeAbove is { } native)
		{
			writer.WriteStartObject();
			writer.WriteString("Kind", "Native");
			writer.WriteString("Title", native);
			writer.WriteEndObject();
		}
		int top = member.Frames.Length - 1;
		for (int index = top; index >= 0; index--)
		{
			DebugSnapshotFrame frame = member.Frames[index];
			CompiledGraph graph = frame.Graph;
			CompiledNode? node = index == top ? member.Node : graph.Nodes[member.Frames[index + 1].CallNode];
			string kind = index == top && member.Waiting ? "Delivery" : index == 0 ? "Graph" : "Call";
			writer.WriteStartObject();
			writer.WriteString("Kind", kind);
			WriteHandle(writer, "Entity", member.Entity);
			WriteHandle(writer, "Graph", member.Graph);
			WriteHandle(writer, "Document", ProgramTrace.DocumentOf(graph, member.Graph));
			writer.WriteNumber("Function", graph.Function?.Id ?? 0);
			writer.WriteString("FunctionName", graph.Function?.Name ?? string.Empty);
			writer.WriteNumber("Node", node?.Id ?? 0);
			writer.WriteString("Title", node?.Title ?? string.Empty);
			writer.WriteString("Pin", index == top && node != null ? PinOf(member, node) : string.Empty);
			writer.WriteString("Note", index == top && node != null && member.Place == DebugPlace.Continue ? NoteOf(member, node) : string.Empty);
			writer.WriteEndObject();
		}
	}

	// The entry pin at an Enter position, the exit a Continue takes.
	private static string PinOf(DebugSnapshotMember member, CompiledNode node) => member.Place switch
	{
		DebugPlace.Enter => member.Entry >= 0 && member.Entry < node.EntryPins.Length ? node.EntryPins[member.Entry] : string.Empty,
		DebugPlace.Continue => member.Exit >= 0 && member.Exit < node.ExitPins.Length ? node.ExitPins[member.Exit] : string.Empty,
		_ => string.Empty,
	};

	// What a Continue position does next: "next: Then1", "next iteration: Index 3", "done: Completed", "checks its condition".
	private static string NoteOf(DebugSnapshotMember member, CompiledNode node)
	{
		FlowEntry entry = member.Flow[^1];
		string exit = member.Exit >= 0 && member.Exit < node.ExitPins.Length ? node.ExitPins[member.Exit] : string.Empty;
		return entry.Kind switch
		{
			FlowKind.Sequence => $"next: {exit}",
			FlowKind.While => "checks its condition",
			_ => member.Exit == 1 ? $"done: {exit}" : string.Create(CultureInfo.InvariantCulture, $"next iteration: Index {entry.Index + 1}"),
		};
	}

	private static void WriteStart(Utf8JsonWriter writer, DebugSnapshotMember member)
	{
		ExecutionStart start = member.Start;
		writer.WriteStartObject("Start");
		writer.WriteString("Kind", start.Kind.ToString());
		WriteHandle(writer, "Graph", member.Graph);
		if (start.Node is { } node)
		{
			WriteHandle(writer, "Document", ProgramTrace.DocumentOf(node.Owner, member.Graph));
			writer.WriteNumber("Node", node.Id);
			writer.WriteString("Title", node.Title);
			writer.WriteString("Pin", start.Exit >= 0 && start.Exit < node.ExitPins.Length ? node.ExitPins[start.Exit] : string.Empty);
		}
		else
		{
			WriteHandle(writer, "Document", member.Graph);
			writer.WriteNumber("Node", 0);
			writer.WriteString("Title", string.Empty);
			writer.WriteString("Pin", string.Empty);
		}
		writer.WriteEndObject();
	}

	private static void WriteHandle(Utf8JsonWriter writer, string name, ulong handle)
	{
		writer.WriteString(name, handle.ToString(CultureInfo.InvariantCulture));
	}
}
