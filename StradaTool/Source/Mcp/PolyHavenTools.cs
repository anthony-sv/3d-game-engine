using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Strada.Tool.Assets;
using Strada.Tool.Editor;

namespace Strada.Tool.Mcp;

/// <summary>strada's own MCP tools for Poly Haven HDRIs (CC0 sky and environment panoramas): finding them, and importing
/// one into the editor's project as an environment for image-based lighting.</summary>
internal static class PolyHavenTools
{
	public const string SearchName = "polyhaven_search_hdris";
	public const string ImportName = "polyhaven_import_hdri";
	public const string DefaultFolder = "Environments";

	private const int DefaultLimit = 20;
	private const int MaxLimit = 100;

	public static IReadOnlyList<McpTool> Create(PolyHaven polyHaven, EditorSession session) =>
	[
		new McpTool
		{
			Name = SearchName,
			Title = "Search Poly Haven HDRIs",
			Description = "Finds HDRIs (CC0 sky and environment panoramas) on polyhaven.com whose names, categories or tags contain " +
						  "every word of the query, such as \"sunset\", \"overcast\" or \"studio\". polyhaven_import_hdri imports one.",
			InputSchema = new JsonObject
			{
				["type"] = "object",
				["properties"] = new JsonObject
				{
					["query"] = new JsonObject { ["type"] = "string", ["description"] = "Words every result contains; empty lists every HDRI" },
					["limit"] = new JsonObject
					{
						["type"] = "integer",
						["minimum"] = 1,
						["maximum"] = MaxLimit,
						["default"] = DefaultLimit,
						["description"] = "The most results to return",
					},
				},
				["additionalProperties"] = false,
			},
			ReadOnly = true,
			OpenWorld = true,
			Run = (arguments, cancellationToken) => SearchAsync(polyHaven, arguments, cancellationToken),
		},
		new McpTool
		{
			Name = ImportName,
			Title = "Import a Poly Haven HDRI",
			Description = "Downloads a Poly Haven HDRI (.hdr, CC0) and imports it into the open project as an environment asset. Set a " +
						  "SkyLight's Environment to the asset's reference for image-based lighting and the skybox.",
			InputSchema = new JsonObject
			{
				["type"] = "object",
				["properties"] = new JsonObject
				{
					["id"] = new JsonObject
					{
						["type"] = "string",
						["pattern"] = "^[a-z0-9_]+$",
						["description"] = "The HDRI's Poly Haven ID, such as kloofendal_48d_partly_cloudy_puresky (polyhaven_search_hdris finds them)",
					},
					["resolution"] = new JsonObject
					{
						["type"] = "string",
						["enum"] = new JsonArray([.. PolyHaven.Resolutions.Select(resolution => JsonValue.Create(resolution))]),
						["default"] = PolyHaven.DefaultResolution,
						["description"] = "Width of the panorama: 1k is enough for lighting, 4k and up for sharp skyboxes",
					},
					["folder"] = new JsonObject
					{
						["type"] = "string",
						["default"] = DefaultFolder,
						["description"] = "Folder in the project's Assets",
					},
				},
				["required"] = new JsonArray("id"),
				["additionalProperties"] = false,
			},
			OpenWorld = true,
			Run = (arguments, cancellationToken) => ImportAsync(polyHaven, session, arguments, cancellationToken),
		},
	];

	private static async Task<JsonObject> SearchAsync(PolyHaven polyHaven, JsonObject? arguments, CancellationToken cancellationToken)
	{
		string? query = ReadString(arguments, "query");
		int limit = ReadLimit(arguments);
		List<PolyHavenHdri> found = await polyHaven.SearchAsync(query, limit, cancellationToken);
		JsonArray hdris = [.. found.Select(hdri => hdri.ToJson())];
		return ToolCatalog.ToCallResult(new JsonObject { ["hdris"] = hdris, ["license"] = PolyHaven.License });
	}

	private static async Task<JsonObject> ImportAsync(PolyHaven polyHaven, EditorSession session, JsonObject? arguments,
		CancellationToken cancellationToken)
	{
		string id = ReadString(arguments, "id") ?? throw new McpToolException("id is required: the HDRI's Poly Haven ID");
		string resolution = ReadString(arguments, "resolution") ?? PolyHaven.DefaultResolution;
		string folder = ReadString(arguments, "folder") ?? DefaultFolder;
		EditorConnection connection = await session.ConnectAsync(cancellationToken);
		return ToolCatalog.ToCallResult(await ImportIntoProjectAsync(polyHaven, connection, id, resolution, folder, cancellationToken));
	}

	/// <summary>Downloads the HDRI and imports it into the editor's project, in <paramref name="folder"/> of its Assets:
	/// { asset, source, license }. Throws <see cref="McpToolException"/> when the editor has no project open (checked before
	/// anything is downloaded), <see cref="PolyHavenException"/> when the download fails and the editor's errors when the
	/// import does.</summary>
	public static async Task<JsonObject> ImportIntoProjectAsync(PolyHaven polyHaven, EditorConnection connection, string id, string resolution,
		string folder, CancellationToken cancellationToken)
	{
		JsonNode? project = await connection.CallAsync("project.info", null, cancellationToken);
		if (project?["project"] is not JsonObject)
		{
			throw new McpToolException("the editor has no project open: open or create one first (project_open, project_create)");
		}

		string directory = Path.Combine(Path.GetTempPath(), $"strada-polyhaven-{Guid.NewGuid():N}");
		try
		{
			string file = await polyHaven.DownloadHdrAsync(id, resolution, directory, cancellationToken);
			JsonNode? imported = await connection.CallAsync("asset.import",
				new JsonObject { ["files"] = new JsonArray(file), ["folder"] = folder }, cancellationToken);
			JsonNode? asset = imported?["assets"] is JsonArray { Count: > 0 } assets ? assets[0]?.DeepClone() : null;
			return new JsonObject
			{
				["asset"] = asset,
				["source"] = PolyHaven.GetPage(id),
				["license"] = PolyHaven.License,
			};
		}
		finally
		{
			// The editor copied the file into the project.
			try
			{
				Directory.Delete(directory, recursive: true);
			}
			catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
			{
				// Never created (the download failed first), or the system cleans its temporary files up.
			}
		}
	}

	private static string? ReadString(JsonObject? arguments, string name)
	{
		if (arguments?[name] is not { } value)
		{
			return null;
		}
		return value is JsonValue text && text.TryGetValue(out string? result) ? result : throw new McpToolException($"{name} is a string");
	}

	private static int ReadLimit(JsonObject? arguments)
	{
		if (arguments?["limit"] is not { } value)
		{
			return DefaultLimit;
		}
		return value is JsonValue number && number.TryGetValue(out int limit) && limit >= 1 && limit <= MaxLimit
			? limit
			: throw new McpToolException($"limit is a whole number from 1 to {MaxLimit}");
	}
}
