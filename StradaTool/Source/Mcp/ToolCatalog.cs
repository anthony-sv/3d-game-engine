using System;
using System.Collections.Generic;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Strada.Tool.Assets;
using Strada.Tool.Editor;

namespace Strada.Tool.Mcp;

/// <summary>An MCP tool: one editor command, or one of strada's own tools.</summary>
internal sealed class McpTool
{
	public required string Name { get; init; }
	/// <summary>For people: the editor command (entity.create), or the title of one of strada's tools.</summary>
	public required string Title { get; init; }
	public required string Description { get; init; }
	/// <summary>The parameters as a JSON schema of an object.</summary>
	public required JsonObject InputSchema { get; init; }
	public bool ReadOnly { get; init; }
	/// <summary>Whether the tool reaches beyond the editor (the internet).</summary>
	public bool OpenWorld { get; init; }
	/// <summary>The editor command the tool runs; null for strada's own tools, which <see cref="Run"/> carries out.</summary>
	public string? Command { get; init; }
	/// <summary>Runs one of strada's own tools: the tools/call result for the arguments.</summary>
	public Func<JsonObject?, CancellationToken, Task<JsonObject>>? Run { get; init; }
}

/// <summary>A mistake in the arguments of one of strada's own tools: a tool error the model reads.</summary>
internal sealed class McpToolException : Exception
{
	public McpToolException(string message)
		: base(message)
	{
	}
}

/// <summary>The MCP tools: the editor's commands and strada's own tools. Command tools are named after their commands, with
/// '.' and '-' replaced by '_' (entity.create is entity_create, asset.create-folder is asset_create_folder), and carry the
/// editor's descriptions and parameter schemas.</summary>
internal sealed class ToolCatalog
{
	private readonly Dictionary<string, McpTool> m_ToolsByName = new(StringComparer.Ordinal);

	private ToolCatalog(List<McpTool> tools)
	{
		Tools = tools;
		foreach (McpTool tool in tools)
		{
			m_ToolsByName[tool.Name] = tool;
		}
	}

	public IReadOnlyList<McpTool> Tools { get; }

	/// <summary>The catalog for the editor.commands result ([{ name, description, params, readOnly, async }]) followed by
	/// strada's own tools. Entries that are not commands, and tools whose name an earlier tool has, are left out.</summary>
	public static ToolCatalog FromCommands(JsonArray commands, IEnumerable<McpTool>? ownTools = null)
	{
		List<McpTool> tools = [];
		HashSet<string> names = new(StringComparer.Ordinal);
		foreach (JsonNode? entry in commands)
		{
			if (entry is not JsonObject command || command["name"] is not JsonValue nameValue || !nameValue.TryGetValue(out string? name) ||
				name.Length == 0)
			{
				continue;
			}
			string toolName = ToToolName(name);
			if (!names.Add(toolName))
			{
				continue;
			}
			string description = command["description"] is JsonValue descriptionValue && descriptionValue.TryGetValue(out string? text)
				? text
				: "";
			bool readOnly = command["readOnly"] is JsonValue readOnlyValue && readOnlyValue.TryGetValue(out bool isReadOnly) && isReadOnly;
			tools.Add(new McpTool
			{
				Name = toolName,
				Title = name,
				Command = name,
				Description = description.Length > 0 ? $"{description} (editor command {name})" : $"Editor command {name}",
				InputSchema = ToInputSchema(command["params"]),
				ReadOnly = readOnly,
			});
		}
		foreach (McpTool tool in ownTools ?? [])
		{
			if (names.Add(tool.Name))
			{
				tools.Add(tool);
			}
		}
		return new ToolCatalog(tools);
	}

	public static string ToToolName(string command) => command.Replace('.', '_').Replace('-', '_');

	public bool TryGetTool(string name, out McpTool tool) => m_ToolsByName.TryGetValue(name, out tool!);

	/// <summary>The tools/list entries for the negotiated protocol version: annotations exist since 2025-03-26, titles
	/// since 2025-06-18.</summary>
	public JsonArray Describe(string protocolVersion)
	{
		bool annotations = string.CompareOrdinal(protocolVersion, "2025-03-26") >= 0;
		bool titles = string.CompareOrdinal(protocolVersion, "2025-06-18") >= 0;
		JsonArray list = [];
		foreach (McpTool tool in Tools)
		{
			JsonObject entry = new() { ["name"] = tool.Name };
			if (titles)
			{
				entry["title"] = tool.Title;
			}
			entry["description"] = tool.Description;
			entry["inputSchema"] = tool.InputSchema.DeepClone();
			if (annotations)
			{
				// Open world is what clients assume without the hint: the editor's tools say that they stay in the editor.
				entry["annotations"] = new JsonObject { ["readOnlyHint"] = tool.ReadOnly, ["openWorldHint"] = tool.OpenWorld };
			}
			list.Add(entry);
		}
		return list;
	}

	/// <summary>The tools/call result for a command's result: its JSON as text, and results with an image (a "mimeType"
	/// image/... and base64 "data", such as viewport.screenshot's) as image content followed by the rest as text.</summary>
	public static JsonObject ToCallResult(JsonNode? result)
	{
		JsonArray content = [];
		if (result is JsonObject image && image["mimeType"] is JsonValue mimeValue && mimeValue.TryGetValue(out string? mimeType) &&
			mimeType.StartsWith("image/", StringComparison.Ordinal) && image["data"] is JsonValue dataValue && dataValue.TryGetValue(out string? data))
		{
			content.Add(new JsonObject { ["type"] = "image", ["data"] = data, ["mimeType"] = mimeType });
			JsonObject details = (JsonObject)image.DeepClone();
			details.Remove("data");
			content.Add(Text(details.ToJsonString(JsonText.Compact)));
		}
		else
		{
			content.Add(Text(result?.ToJsonString(JsonText.Compact) ?? "null"));
		}
		return new JsonObject { ["content"] = content, ["isError"] = false };
	}

	/// <summary>The tools/call result for a failed command: errors are results the model reads, not protocol errors.</summary>
	public static JsonObject ToErrorResult(Exception exception)
	{
		string text = exception switch
		{
			EditorCommandException command => command.Describe(),
			EditorUnavailableException unavailable => $"No editor is available: {unavailable.Message}",
			PolyHavenException polyHaven => $"Poly Haven: {polyHaven.Message}",
			_ => exception.Message,
		};
		return new JsonObject { ["content"] = new JsonArray(Text(text)), ["isError"] = true };
	}

	private static JsonObject Text(string text) => new() { ["type"] = "text", ["text"] = text };

	// MCP input schemas are object schemas; commands without parameters take an empty object.
	private static JsonObject ToInputSchema(JsonNode? parameters)
	{
		if (parameters is JsonObject schema && schema["type"] is JsonValue type && type.TryGetValue(out string? kind) && kind == "object")
		{
			return (JsonObject)schema.DeepClone();
		}
		return new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() };
	}
}
