using System.Buffers;
using System.Collections.Generic;
using System.Text.Json;

namespace Strada.VisualScripting;

// The analysis JSON of VisualScriptAnalyze (spec section 6.2): { "Nodes": [...], "Diagnostics": [...] }, every node of the
// document (the function graphs in order, then the event graph) with its resolved pins.
internal static class AnalysisWriter
{
	internal static byte[] Write(AnalyzedDocument document)
	{
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer, LibraryWriter.CreateOptions()))
		{
			writer.WriteStartObject();
			writer.WriteStartArray("Nodes");
			foreach (AnalyzedGraph graph in document.Graphs)
			{
				foreach (AnalyzedNode node in graph.Nodes)
				{
					WriteNode(writer, node);
				}
			}
			writer.WriteEndArray();
			writer.WritePropertyName("Diagnostics");
			WriteDiagnostics(writer, document.Diagnostics);
			writer.WriteEndObject();
		}
		return buffer.WrittenSpan.ToArray();
	}

	// The diagnostics array alone (VisualScriptLoad's result).
	internal static byte[] WriteDiagnostics(IReadOnlyList<Diagnostic> diagnostics)
	{
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer, LibraryWriter.CreateOptions()))
		{
			WriteDiagnostics(writer, diagnostics);
		}
		return buffer.WrittenSpan.ToArray();
	}

	internal static void WriteDiagnostics(Utf8JsonWriter writer, IReadOnlyList<Diagnostic> diagnostics)
	{
		writer.WriteStartArray();
		foreach (Diagnostic diagnostic in diagnostics)
		{
			writer.WriteStartObject();
			writer.WriteString("Severity", diagnostic.Severity.ToString());
			writer.WriteString("Code", diagnostic.Code.ToString());
			writer.WriteNumber("Function", diagnostic.Function);
			if (diagnostic.Node is { } node)
			{
				writer.WriteNumber("Node", node);
			}
			if (diagnostic.Pin != null)
			{
				writer.WriteString("Pin", diagnostic.Pin);
			}
			if (diagnostic.Direction is { } direction)
			{
				writer.WriteString("Direction", direction.ToString());
			}
			if (diagnostic.Link is { } link)
			{
				writer.WriteStartObject("Link");
				WriteEndpoint(writer, "From", link.From);
				WriteEndpoint(writer, "To", link.To);
				writer.WriteEndObject();
			}
			if (diagnostic.Variable is { } variable)
			{
				writer.WriteNumber("Variable", variable);
			}
			writer.WriteString("Message", diagnostic.Message);
			if (diagnostic.Suggestions.Count > 0)
			{
				writer.WriteStartArray("Suggestions");
				foreach (string suggestion in diagnostic.Suggestions)
				{
					writer.WriteStringValue(suggestion);
				}
				writer.WriteEndArray();
			}
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
	}

	private static void WriteNode(Utf8JsonWriter writer, AnalyzedNode node)
	{
		writer.WriteStartObject();
		writer.WriteNumber("ID", node.ID);
		writer.WriteNumber("Function", node.Graph.Function);
		writer.WriteString("Type", node.Source.Type);
		writer.WriteString("Resolved", node.Resolution.ToString());
		if (node.Resolution is NodeResolution.Redirect or NodeResolution.Unique)
		{
			writer.WriteString("CanonicalType", node.CanonicalType);
			writer.WriteStartArray("PinRenames");
			foreach (PinRename rename in node.PinRenames)
			{
				writer.WriteStartObject();
				writer.WriteString("Direction", rename.Direction.ToString());
				writer.WriteString("From", rename.From);
				writer.WriteString("To", rename.To);
				writer.WriteEndObject();
			}
			writer.WriteEndArray();
		}
		writer.WriteString("Title", node.Title);
		if (node.Subtitle.Length > 0)
		{
			writer.WriteString("Subtitle", node.Subtitle);
		}
		writer.WriteString("Kind", node.Kind.ToString());
		writer.WriteBoolean("Pure", node.Pure);
		writer.WriteBoolean("Latent", node.Latent);
		writer.WriteStartArray("Pins");
		foreach (AnalyzedPin pin in node.Pins)
		{
			LibraryWriter.WritePin(writer, pin.Description, pin.Type);
		}
		writer.WriteEndArray();
		writer.WriteEndObject();
	}

	private static void WriteEndpoint(Utf8JsonWriter writer, string name, PinReference endpoint)
	{
		writer.WriteStartArray(name);
		writer.WriteNumberValue(endpoint.Node);
		writer.WriteStringValue(endpoint.Pin);
		writer.WriteEndArray();
	}
}
