using System;
using System.Collections.Generic;
using System.Text.Json;

namespace Strada.VisualScripting;

// A visual script in the content form (spec section 1, the "VisualScript" object without the file header), as the VM
// reads it: what analysis, compilation and execution need. Raw values (Value, Settings, Inputs) stay JSON; the elements
// own them (they outlive the parsed text).
internal sealed class GraphDocument
{
	internal GraphDocument(IReadOnlyList<DocumentVariable> variables, IReadOnlyList<DocumentFunction> functions, DocumentGraph graph)
	{
		Variables = variables;
		Functions = functions;
		Graph = graph;
	}

	internal IReadOnlyList<DocumentVariable> Variables { get; }

	internal IReadOnlyList<DocumentFunction> Functions { get; }

	// The event graph.
	internal DocumentGraph Graph { get; }
}

// A document variable. Value is null when the document gives none (the type default applies).
internal sealed record DocumentVariable(uint ID, string Name, string Type, JsonElement? Value, bool Public, IReadOnlyList<string> FormerNames);

// A local of a function: call-frame storage initialized on every call.
internal sealed record DocumentLocal(uint ID, string Name, string Type, JsonElement? Value);

// An input or output of a function.
internal sealed record DocumentParameter(string Name, string Type);

internal sealed record DocumentFunction(uint ID, string Name, bool Pure, IReadOnlyList<DocumentParameter> Inputs,
	IReadOnlyList<DocumentParameter> Outputs, IReadOnlyList<DocumentLocal> Locals, DocumentGraph Graph);

// The nodes and links of the event graph (Function 0) or of a function's graph.
internal sealed record DocumentGraph(uint Function, IReadOnlyList<DocumentNode> Nodes, IReadOnlyList<DocumentLink> Links);

// A node. Index is its position in its graph's Nodes ("later" means a higher index).
internal sealed record DocumentNode(uint ID, string Type, DocumentMembers Settings, DocumentMembers Inputs, int Index);

internal readonly record struct PinReference(uint Node, string Pin);

// A link from an output to an input of nodes of one graph. Index is its position in the graph's Links.
internal sealed record DocumentLink(PinReference From, PinReference To, int Index);

// The members of a node's Settings or Inputs object, in document order.
internal sealed class DocumentMembers
{
	private readonly List<KeyValuePair<string, JsonElement>> m_Members = [];
	private readonly Dictionary<string, JsonElement> m_ByName = new(StringComparer.Ordinal);

	internal int Count => m_Members.Count;

	internal IReadOnlyList<KeyValuePair<string, JsonElement>> Members => m_Members;

	internal bool TryGet(string name, out JsonElement value) => m_ByName.TryGetValue(name, out value);

	// False when the name is already present (the first member keeps it).
	internal bool Add(string name, JsonElement value)
	{
		if (!m_ByName.TryAdd(name, value))
		{
			return false;
		}
		m_Members.Add(new KeyValuePair<string, JsonElement>(name, value));
		return true;
	}
}
