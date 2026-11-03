using System.Collections.Generic;
using System.Text.Json;

namespace Strada.VisualScripting;

// One breakpoint as the editor configured it (spec-slice4 section 2.1): a node of a document (a script graph or a function
// library, by handle), the node's Type when it was set (a compiled node of another Type is not it, section 2.6), whether it
// is enabled, its condition and its hit count (section 2.4).
internal sealed record DebugBreakpointSetting(ulong Document, uint Node, string Type, bool Enabled, string Condition, uint HitCount);

// What the editor configured the debugger with (spec-slice4 section 7.8, Configure): the breakpoints, the target of each
// script graph (a graph without one, or with entity 0, targets any instance, section 3.1), the watched graphs (a document and
// a function of it, 0 for a script graph's event graph) and whether errors stop.
internal sealed class DebugConfiguration
{
	internal static DebugConfiguration Empty { get; } = new(0, [], new Dictionary<ulong, ulong>(), new HashSet<(ulong, uint)>(), false);

	private DebugConfiguration(uint revision, IReadOnlyList<DebugBreakpointSetting> breakpoints, IReadOnlyDictionary<ulong, ulong> targets,
		IReadOnlySet<(ulong Document, uint Function)> watches, bool breakOnErrors)
	{
		Revision = revision;
		Breakpoints = breakpoints;
		Targets = targets;
		Watches = watches;
		BreakOnErrors = breakOnErrors;
	}

	// The editor's number of this configuration, answered back.
	internal uint Revision { get; }

	internal IReadOnlyList<DebugBreakpointSetting> Breakpoints { get; }

	// The entity each script graph's breakpoints stop, by graph handle (0: any instance).
	internal IReadOnlyDictionary<ulong, ulong> Targets { get; }

	internal IReadOnlySet<(ulong Document, uint Function)> Watches { get; }

	internal bool BreakOnErrors { get; }

	// Reads Configure's members; a member that is missing, unknown or of the wrong type throws a DebugRequestException naming
	// its path, as does a second breakpoint of one node, a second target of one graph or a condition (this debugger stops
	// at every hit of a breakpoint, counted by its hit count).
	internal static DebugConfiguration Read(JsonElement request)
	{
		DebugRequest.ExpectMembers(request, string.Empty, "Command", "Revision", "Breakpoints", "Targets", "Watches", "BreakOnErrors");
		uint revision = DebugRequest.ReadUInt(request, "Revision", string.Empty);
		List<DebugBreakpointSetting> breakpoints = [];
		HashSet<(ulong, uint)> nodes = [];
		int index = 0;
		foreach (JsonElement element in DebugRequest.ReadArray(request, "Breakpoints", string.Empty))
		{
			string path = DebugRequest.Index("Breakpoints", index++);
			DebugRequest.ExpectObject(element, path);
			DebugRequest.ExpectMembers(element, path, "Document", "Node", "Type", "Enabled", "Condition", "HitCount");
			DebugBreakpointSetting breakpoint = new(DebugRequest.ReadHandle(element, "Document", path), DebugRequest.ReadUInt(element, "Node", path),
				DebugRequest.ReadString(element, "Type", path), DebugRequest.ReadBool(element, "Enabled", path),
				DebugRequest.ReadString(element, "Condition", path), DebugRequest.ReadUInt(element, "HitCount", path));
			if (breakpoint.Condition.Length > 0)
			{
				throw new DebugRequestException(DebugErrorCode.InvalidRequest, $"{path}.Condition: breakpoint conditions are not supported");
			}
			if (!nodes.Add((breakpoint.Document, breakpoint.Node)))
			{
				throw new DebugRequestException(DebugErrorCode.InvalidRequest, $"{path}: a second breakpoint of the node");
			}
			breakpoints.Add(breakpoint);
		}
		Dictionary<ulong, ulong> targets = [];
		index = 0;
		foreach (JsonElement element in DebugRequest.ReadArray(request, "Targets", string.Empty))
		{
			string path = DebugRequest.Index("Targets", index++);
			DebugRequest.ExpectObject(element, path);
			DebugRequest.ExpectMembers(element, path, "Graph", "Entity");
			if (!targets.TryAdd(DebugRequest.ReadHandle(element, "Graph", path), DebugRequest.ReadHandle(element, "Entity", path)))
			{
				throw new DebugRequestException(DebugErrorCode.InvalidRequest, $"{path}: a second target of the graph");
			}
		}
		HashSet<(ulong, uint)> watches = [];
		index = 0;
		foreach (JsonElement element in DebugRequest.ReadArray(request, "Watches", string.Empty))
		{
			string path = DebugRequest.Index("Watches", index++);
			DebugRequest.ExpectObject(element, path);
			DebugRequest.ExpectMembers(element, path, "Document", "Function");
			watches.Add((DebugRequest.ReadHandle(element, "Document", path), DebugRequest.ReadUInt(element, "Function", path)));
		}
		return new DebugConfiguration(revision, breakpoints, targets, watches, DebugRequest.ReadBool(request, "BreakOnErrors", string.Empty));
	}
}
