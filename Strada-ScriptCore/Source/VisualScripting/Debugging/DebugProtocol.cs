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

	internal static bool Has(JsonElement element, string name) => element.TryGetProperty(name, out _);

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

	private static JsonElement Require(JsonElement element, string name, string path) =>
		element.TryGetProperty(name, out JsonElement value) ? value : throw Invalid($"{Member(path, name)}: missing");

	private static DebugRequestException Invalid(string message) => new(DebugErrorCode.InvalidRequest, message);
}

// The commands of VisualScriptDebug (spec-slice4 section 7.8): a request is a JSON object { "Command": "<name>", ... }; the
// reply is a JSON object, an error reply { "Error", "Code" } for anything refused. Handles and entities are decimal strings,
// node, function and stop IDs integers. Commands: Attach, Detach, Configure, State and Resume (Continue).
internal static class DebugProtocol
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
		string action = DebugRequest.ReadString(request, "Action", string.Empty);
		if (action is "StepInto" or "StepOver" or "StepOut")
		{
			throw new DebugRequestException(DebugErrorCode.InvalidRequest, $"Action: {action} is not supported: only Continue");
		}
		if (action != "Continue")
		{
			throw new DebugRequestException(DebugErrorCode.InvalidRequest, $"Action: unknown action {JsonText.Quote(action)}");
		}
		bool all = DebugRequest.Has(request, "All") && DebugRequest.ReadBool(request, "All", string.Empty);
		bool hasStop = DebugRequest.Has(request, "Stop");
		if (all && hasStop)
		{
			throw new DebugRequestException(DebugErrorCode.InvalidRequest, "Stop: All continues every stop");
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
			result = debugger.Continue(stop);
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
		writer.WriteNull("Stepping");
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
			writer.WriteNull("Error");
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
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
		foreach (DebugChange change in stop.Changed)
		{
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
