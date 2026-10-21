using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Globalization;
using System.Text.Json;
using System.Text.Unicode;

namespace Strada.VisualScripting;

// Reads the content form of a document. The engine sends documents its loader validated and repaired (spec section 1.3),
// but the VM accepts any bytes: text that is not a JSON object is one Error InvalidDocument, and a structural defect the
// loader would have repaired or refused is an Error InvalidDocument naming its path, with the element skipped (an
// element with a missing, invalid or duplicate ID, Name or Type; a malformed link or one between graphs; a duplicate
// link). Keys the VM does not use (Description, positions, comments, tooltips, ranges) are not checked.
internal static class DocumentReader
{
	internal const int MaxDepth = 64;
	internal const int MaxTypeBytes = 1024;
	internal const int MaxDeclaredTypeBytes = 256;
	internal const int MaxPinBytes = 128;
	private const uint MaxId = 2147483647;

	// Null when the text is not a JSON object.
	internal static GraphDocument? Read(ReadOnlySpan<byte> utf8, List<Diagnostic> diagnostics)
	{
		if (utf8.IsEmpty)
		{
			diagnostics.Add(new Diagnostic(DiagnosticCode.InvalidDocument, 0, "the document is empty: expected a JSON object"));
			return null;
		}
		// JSON text is UTF-8 (RFC 8259); the parser checks only its structure.
		if (!Utf8.IsValid(utf8))
		{
			diagnostics.Add(new Diagnostic(DiagnosticCode.InvalidDocument, 0, "the document is not UTF-8 text"));
			return null;
		}
		JsonElement root;
		try
		{
			using JsonDocument json = JsonDocument.Parse(utf8.ToArray(), new JsonDocumentOptions { MaxDepth = MaxDepth });
			root = json.RootElement.Clone();
		}
		catch (JsonException exception)
		{
			string location = FormattableString.Invariant($"line {(exception.LineNumber ?? 0) + 1}, byte {(exception.BytePositionInLine ?? 0) + 1}");
			diagnostics.Add(new Diagnostic(DiagnosticCode.InvalidDocument, 0,
				$"the document is not JSON (nested at most {MaxDepth.ToString(CultureInfo.InvariantCulture)} levels deep) at {location}"));
			return null;
		}
		if (root.ValueKind != JsonValueKind.Object)
		{
			diagnostics.Add(new Diagnostic(DiagnosticCode.InvalidDocument, 0, "the document is not a JSON object"));
			return null;
		}
		if (!JsonText.HasValidText(root))
		{
			diagnostics.Add(new Diagnostic(DiagnosticCode.InvalidDocument, 0,
				"the document has a string or member name that is not text: an escaped surrogate without its pair"));
			return null;
		}
		return new Reader(diagnostics).ReadDocument(root);
	}

	private sealed class Reader
	{
		private readonly List<Diagnostic> m_Diagnostics;
		private readonly HashSet<uint> m_Ids = [];
		private uint m_Function;

		public Reader(List<Diagnostic> diagnostics)
		{
			m_Diagnostics = diagnostics;
		}

		public GraphDocument ReadDocument(JsonElement root)
		{
			List<DocumentVariable> variables = ReadVariables(root);
			HashSet<string> variableNames = new(StringComparer.Ordinal);
			foreach (DocumentVariable variable in variables)
			{
				variableNames.Add(variable.Name);
			}
			List<DocumentFunction> functions = ReadFunctions(root, variableNames);
			m_Function = 0;
			DocumentGraph graph = ReadGraph(root, "Graph", 0);
			return new GraphDocument(variables, functions, graph);
		}

		private List<DocumentVariable> ReadVariables(JsonElement root)
		{
			List<DocumentVariable> variables = [];
			HashSet<string> names = new(StringComparer.Ordinal);
			List<(JsonElement Element, string Path)> elements = Elements(root, "Variables", "Variables");
			foreach ((JsonElement element, string path) in elements)
			{
				if (!TryReadId(element, path, out uint id) || !TryReadName(element, path, out string? name) ||
					!TryReadType(element, path, MaxDeclaredTypeBytes, out string? type))
				{
					continue;
				}
				if (!names.Add(name))
				{
					Problem($"{path}.Name", $"a variable named {name} comes earlier");
					continue;
				}
				if (!m_Ids.Add(id))
				{
					Problem($"{path}.ID", Duplicate(id));
					continue;
				}
				variables.Add(new DocumentVariable(id, name, type, ReadValue(element), ReadBool(element, path, "Public"),
					ReadFormerNames(element, path)));
			}
			// Former names identify renamed variables: each is unique in the document and differs from every current name.
			HashSet<string> formerNames = new(StringComparer.Ordinal);
			for (int index = 0; index < variables.Count; index++)
			{
				DocumentVariable variable = variables[index];
				List<string> kept = [];
				foreach (string formerName in variable.FormerNames)
				{
					if (names.Contains(formerName) || !formerNames.Add(formerName))
					{
						Problem($"Variables[{Format(index)}].FormerNames", $"{formerName} is the name of a variable or another former name");
						continue;
					}
					kept.Add(formerName);
				}
				if (kept.Count != variable.FormerNames.Count)
				{
					variables[index] = variable with { FormerNames = kept };
				}
			}
			return variables;
		}

		private List<string> ReadFormerNames(JsonElement element, string path)
		{
			List<string> formerNames = [];
			if (!element.TryGetProperty("FormerNames", out JsonElement list))
			{
				return formerNames;
			}
			if (list.ValueKind != JsonValueKind.Array)
			{
				Problem($"{path}.FormerNames", "expected an array of names");
				return formerNames;
			}
			int index = 0;
			foreach (JsonElement entry in list.EnumerateArray())
			{
				string entryPath = $"{path}.FormerNames[{Format(index++)}]";
				if (!JsonText.TryGetString(entry, out string? name))
				{
					Problem(entryPath, "expected a name");
					continue;
				}
				string? problem = Names.Check(name);
				if (problem != null)
				{
					Problem(entryPath, problem);
					continue;
				}
				formerNames.Add(name);
			}
			return formerNames;
		}

		private List<DocumentFunction> ReadFunctions(JsonElement root, HashSet<string> variableNames)
		{
			List<DocumentFunction> functions = [];
			HashSet<string> names = new(StringComparer.Ordinal);
			foreach ((JsonElement element, string path) in Elements(root, "Functions", "Functions"))
			{
				m_Function = 0;
				if (!TryReadId(element, path, out uint id) || !TryReadName(element, path, out string? name))
				{
					continue;
				}
				if (!names.Add(name))
				{
					Problem($"{path}.Name", $"a function named {name} comes earlier");
					continue;
				}
				if (!m_Ids.Add(id))
				{
					Problem($"{path}.ID", Duplicate(id));
					continue;
				}
				m_Function = id;
				bool pure = ReadBool(element, path, "Pure");
				List<DocumentParameter> inputs = ReadParameters(element, path, "Inputs");
				List<DocumentParameter> outputs = ReadParameters(element, path, "Outputs");
				List<DocumentLocal> locals = ReadLocals(element, path, variableNames);
				DocumentGraph graph = ReadGraph(element, $"{path}.Graph", id);
				functions.Add(new DocumentFunction(id, name, pure, inputs, outputs, locals, graph));
			}
			m_Function = 0;
			return functions;
		}

		private List<DocumentParameter> ReadParameters(JsonElement function, string functionPath, string key)
		{
			List<DocumentParameter> parameters = [];
			HashSet<string> names = new(StringComparer.Ordinal);
			foreach ((JsonElement element, string path) in Elements(function, key, $"{functionPath}.{key}"))
			{
				if (!TryReadName(element, path, out string? name) || !TryReadType(element, path, MaxDeclaredTypeBytes, out string? type))
				{
					continue;
				}
				if (!names.Add(name))
				{
					Problem($"{path}.Name", $"a parameter named {name} comes earlier");
					continue;
				}
				parameters.Add(new DocumentParameter(name, type));
			}
			return parameters;
		}

		private List<DocumentLocal> ReadLocals(JsonElement function, string functionPath, HashSet<string> variableNames)
		{
			List<DocumentLocal> locals = [];
			HashSet<string> names = new(StringComparer.Ordinal);
			foreach ((JsonElement element, string path) in Elements(function, "Locals", $"{functionPath}.Locals"))
			{
				if (!TryReadId(element, path, out uint id) || !TryReadName(element, path, out string? name) ||
					!TryReadType(element, path, MaxDeclaredTypeBytes, out string? type))
				{
					continue;
				}
				if (!names.Add(name) || variableNames.Contains(name))
				{
					Problem($"{path}.Name", $"{name} is the name of a variable or an earlier local");
					continue;
				}
				if (!m_Ids.Add(id))
				{
					Problem($"{path}.ID", Duplicate(id));
					continue;
				}
				locals.Add(new DocumentLocal(id, name, type, ReadValue(element)));
			}
			return locals;
		}

		private DocumentGraph ReadGraph(JsonElement parent, string path, uint function)
		{
			if (!parent.TryGetProperty("Graph", out JsonElement graph))
			{
				return new DocumentGraph(function, [], []);
			}
			if (graph.ValueKind != JsonValueKind.Object)
			{
				Problem(path, "expected an object");
				return new DocumentGraph(function, [], []);
			}
			List<DocumentNode> nodes = [];
			HashSet<uint> nodeIds = [];
			foreach ((JsonElement element, string nodePath) in Elements(graph, "Nodes", $"{path}.Nodes"))
			{
				if (!TryReadId(element, nodePath, out uint id) || !TryReadType(element, nodePath, MaxTypeBytes, out string? type))
				{
					continue;
				}
				if (!m_Ids.Add(id))
				{
					Problem($"{nodePath}.ID", Duplicate(id));
					continue;
				}
				nodeIds.Add(id);
				nodes.Add(new DocumentNode(id, type, ReadMembers(element, nodePath, "Settings"), ReadMembers(element, nodePath, "Inputs"),
					nodes.Count));
			}
			List<DocumentLink> links = [];
			HashSet<(PinReference, PinReference)> seen = [];
			foreach ((JsonElement element, string linkPath) in Elements(graph, "Links", $"{path}.Links"))
			{
				if (!TryReadEndpoint(element, linkPath, "From", nodeIds, out PinReference from) ||
					!TryReadEndpoint(element, linkPath, "To", nodeIds, out PinReference to))
				{
					continue;
				}
				if (!seen.Add((from, to)))
				{
					Problem(linkPath, "the same link comes earlier");
					continue;
				}
				links.Add(new DocumentLink(from, to, links.Count));
			}
			return new DocumentGraph(function, nodes, links);
		}

		private bool TryReadEndpoint(JsonElement link, string linkPath, string key, HashSet<uint> nodeIds, out PinReference endpoint)
		{
			endpoint = default;
			string path = $"{linkPath}.{key}";
			if (!link.TryGetProperty(key, out JsonElement value) || value.ValueKind != JsonValueKind.Array || value.GetArrayLength() != 2 ||
				value[0].ValueKind != JsonValueKind.Number || !value[0].TryGetUInt32(out uint node) || !JsonText.TryGetString(value[1], out string? pin))
			{
				Problem(path, "expected [node ID, \"Pin\"]");
				return false;
			}
			if (!nodeIds.Contains(node))
			{
				Problem(path, $"node {Format(node)} is not a node of this graph");
				return false;
			}
			int bytes = JsonText.ByteCount(pin);
			if (bytes == 0 || bytes > MaxPinBytes || JsonText.HasControlCharacter(pin))
			{
				Problem(path, $"a pin name has 1 to {Format(MaxPinBytes)} bytes without control characters");
				return false;
			}
			endpoint = new PinReference(node, pin);
			return true;
		}

		private DocumentMembers ReadMembers(JsonElement node, string nodePath, string key)
		{
			DocumentMembers members = new();
			if (!node.TryGetProperty(key, out JsonElement value))
			{
				return members;
			}
			string path = $"{nodePath}.{key}";
			if (value.ValueKind != JsonValueKind.Object)
			{
				Problem(path, "expected an object");
				return members;
			}
			foreach (JsonProperty property in value.EnumerateObject())
			{
				if (!JsonText.TryGetName(property, out string? name) || name.Length == 0 || JsonText.ByteCount(name) > MaxPinBytes ||
					JsonText.HasControlCharacter(name))
				{
					Problem(path, $"a member name has 1 to {Format(MaxPinBytes)} bytes without control characters");
					continue;
				}
				if (!members.Add(name, property.Value))
				{
					Problem($"{path}.{name}", "the member comes earlier");
				}
			}
			return members;
		}

		// The object elements of an optional array.
		private List<(JsonElement Element, string Path)> Elements(JsonElement parent, string key, string path)
		{
			List<(JsonElement, string)> elements = [];
			if (!parent.TryGetProperty(key, out JsonElement list))
			{
				return elements;
			}
			if (list.ValueKind != JsonValueKind.Array)
			{
				Problem(path, "expected an array");
				return elements;
			}
			int index = 0;
			foreach (JsonElement element in list.EnumerateArray())
			{
				string elementPath = $"{path}[{Format(index++)}]";
				if (element.ValueKind != JsonValueKind.Object)
				{
					Problem(elementPath, "expected an object");
					continue;
				}
				elements.Add((element, elementPath));
			}
			return elements;
		}

		private bool TryReadId(JsonElement element, string path, out uint id)
		{
			if (element.TryGetProperty("ID", out JsonElement value) && value.ValueKind == JsonValueKind.Number && value.TryGetUInt32(out id) &&
				id >= 1 && id <= MaxId)
			{
				return true;
			}
			id = 0;
			Problem($"{path}.ID", $"expected an integer from 1 to {Format(MaxId)}");
			return false;
		}

		private bool TryReadName(JsonElement element, string path, [NotNullWhen(true)] out string? name)
		{
			if (!element.TryGetProperty("Name", out JsonElement value) || !JsonText.TryGetString(value, out name))
			{
				name = null;
				Problem($"{path}.Name", "expected a name");
				return false;
			}
			string? problem = Names.Check(name);
			if (problem != null)
			{
				Problem($"{path}.Name", problem);
				name = null;
				return false;
			}
			return true;
		}

		private bool TryReadType(JsonElement element, string path, int maxBytes, [NotNullWhen(true)] out string? type)
		{
			if (element.TryGetProperty("Type", out JsonElement value) && JsonText.TryGetString(value, out type))
			{
				int bytes = JsonText.ByteCount(type);
				if (bytes > 0 && bytes <= maxBytes && !JsonText.HasControlCharacter(type))
				{
					return true;
				}
			}
			type = null;
			Problem($"{path}.Type", $"expected a type of 1 to {Format(maxBytes)} bytes without control characters");
			return false;
		}

		// A raw value; null when absent or JSON null (no value).
		private static JsonElement? ReadValue(JsonElement element)
		{
			return element.TryGetProperty("Value", out JsonElement value) && value.ValueKind != JsonValueKind.Null ? value : null;
		}

		private bool ReadBool(JsonElement element, string path, string key)
		{
			if (!element.TryGetProperty(key, out JsonElement value))
			{
				return false;
			}
			if (value.ValueKind is JsonValueKind.True or JsonValueKind.False)
			{
				return value.GetBoolean();
			}
			Problem($"{path}.{key}", "expected true or false");
			return false;
		}

		private void Problem(string path, string message)
		{
			m_Diagnostics.Add(new Diagnostic(DiagnosticCode.InvalidDocument, m_Function, $"{path}: {message}"));
		}

		private static string Duplicate(uint id) => $"ID {Format(id)} is used by an earlier element";

		private static string Format(long value) => value.ToString(CultureInfo.InvariantCulture);
	}
}
