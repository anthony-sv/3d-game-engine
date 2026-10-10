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

/// <summary>strada's own MCP tools for Poly Haven (CC0): finding HDRIs, models and texture sets, and importing them into the
/// editor's project as an environment, a mesh with its materials, or a material.</summary>
internal static class PolyHavenTools
{
	public const string SearchName = "polyhaven_search_hdris";
	public const string ImportName = "polyhaven_import_hdri";
	public const string SearchModelsName = "polyhaven_search_models";
	public const string ImportModelName = "polyhaven_import_model";
	public const string SearchTexturesName = "polyhaven_search_textures";
	public const string ImportTextureName = "polyhaven_import_texture";
	public const string DefaultFolder = "Environments";
	public const string DefaultModelFolder = "Models";
	public const string DefaultTextureFolder = "Textures";
	public const string DefaultModelResolution = "1k";

	private const string NoProject = "the editor has no project open: open or create one first (project_open, project_create)";
	private const int DefaultLimit = 20;
	private const int MaxLimit = 100;

	public static IReadOnlyList<McpTool> Create(PolyHaven polyHaven, EditorSession session) =>
	[
		SearchTool(polyHaven, PolyHavenAssetType.Hdri, SearchName, "Search Poly Haven HDRIs",
			"Finds HDRIs (CC0 sky and environment panoramas) on polyhaven.com whose names, categories or tags contain every word of " +
			"the query, such as \"sunset\", \"overcast\" or \"studio\". polyhaven_import_hdri imports one."),
		ImportTool(polyHaven, session, ImportName, "Import a Poly Haven HDRI",
			"Downloads a Poly Haven HDRI (.hdr, CC0) and imports it into the open project as an environment asset. Set a SkyLight's " +
			"Environment to the asset's reference for image-based lighting and the skybox.",
			"The HDRI's Poly Haven ID, such as kloofendal_48d_partly_cloudy_puresky (polyhaven_search_hdris finds them)",
			PolyHaven.DefaultResolution, "Width of the panorama: 1k is enough for lighting, 4k and up for sharp skyboxes", DefaultFolder,
			ImportHdriAsync),
		SearchTool(polyHaven, PolyHavenAssetType.Model, SearchModelsName, "Search Poly Haven models",
			"Finds models (CC0 props, furniture, plants, rocks, ...) on polyhaven.com whose names, categories or tags contain every " +
			"word of the query, such as \"crate\", \"chair\" or \"rock\". polyhaven_import_model imports one."),
		ImportTool(polyHaven, session, ImportModelName, "Import a Poly Haven model",
			"Downloads a Poly Haven model (glTF with its buffer and PBR textures, CC0) into its own folder of the open project and " +
			"imports it: the result's mesh asset goes into a MeshComponent's Mesh (its materials come with it), and into a " +
			"MeshColliderComponent for physics. Models are in meters.",
			"The model's Poly Haven ID, such as wooden_crate_01 or ArmChair_01 (polyhaven_search_models finds them)",
			DefaultModelResolution, "Size of the textures: 1k keeps projects small, 2k and up for close-ups", DefaultModelFolder,
			ImportModelAsync),
		SearchTool(polyHaven, PolyHavenAssetType.Texture, SearchTexturesName, "Search Poly Haven textures",
			"Finds PBR texture sets (CC0 ground, wood, stone, metal, fabric, ...) on polyhaven.com whose names, categories or tags " +
			"contain every word of the query, such as \"grass\", \"planks\" or \"brick\". polyhaven_import_texture imports one."),
		ImportTool(polyHaven, session, ImportTextureName, "Import a Poly Haven texture set",
			"Downloads a Poly Haven texture set (color, normal, and ambient occlusion/roughness/metallic maps, CC0) into its own " +
			"folder of the open project and creates a material (.smat) from it. Put the material's reference into a MeshComponent's " +
			"Materials; set its UVTiling (material_set) to repeat it over large surfaces.",
			"The texture set's Poly Haven ID, such as brown_mud_leaves_01 (polyhaven_search_textures finds them)",
			DefaultModelResolution, "Size of the maps: 1k keeps projects small, 2k and up for close-ups", DefaultTextureFolder,
			ImportTextureAsync),
	];

	private delegate Task<JsonObject> ImportAction(PolyHaven polyHaven, EditorConnection connection, string id, string resolution, string folder,
		CancellationToken cancellationToken);

	private static McpTool SearchTool(PolyHaven polyHaven, PolyHavenAssetType type, string name, string title, string description) => new()
	{
		Name = name,
		Title = title,
		Description = description,
		InputSchema = new JsonObject
		{
			["type"] = "object",
			["properties"] = new JsonObject
			{
				["query"] = new JsonObject { ["type"] = "string", ["description"] = "Words every result contains; empty lists all of them" },
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
		Run = async (arguments, cancellationToken) =>
			ToolCatalog.ToCallResult(await SearchAsync(polyHaven, type, ResultKey(type), arguments, cancellationToken)),
	};

	private static McpTool ImportTool(PolyHaven polyHaven, EditorSession session, string name, string title, string description,
		string idDescription, string defaultResolution, string resolutionDescription, string defaultFolder, ImportAction import) => new()
		{
			Name = name,
			Title = title,
			Description = description,
			InputSchema = new JsonObject
			{
				["type"] = "object",
				["properties"] = new JsonObject
				{
					["id"] = new JsonObject { ["type"] = "string", ["pattern"] = "^[A-Za-z0-9_]+$", ["description"] = idDescription },
					["resolution"] = new JsonObject
					{
						["type"] = "string",
						["enum"] = new JsonArray([.. PolyHaven.Resolutions.Select(resolution => JsonValue.Create(resolution))]),
						["default"] = defaultResolution,
						["description"] = resolutionDescription,
					},
					["folder"] = new JsonObject { ["type"] = "string", ["default"] = defaultFolder, ["description"] = "Folder in the project's Assets" },
				},
				["required"] = new JsonArray("id"),
				["additionalProperties"] = false,
			},
			OpenWorld = true,
			Run = async (arguments, cancellationToken) =>
			{
				string id = ReadString(arguments, "id") ?? throw new McpToolException("id is required: the asset's Poly Haven ID");
				string resolution = ReadString(arguments, "resolution") ?? defaultResolution;
				string folder = ReadString(arguments, "folder") ?? defaultFolder;
				EditorConnection connection = await session.ConnectAsync(cancellationToken);
				return ToolCatalog.ToCallResult(await import(polyHaven, connection, id, resolution, folder, cancellationToken));
			},
		};

	private static string ResultKey(PolyHavenAssetType type) => type switch
	{
		PolyHavenAssetType.Hdri => "hdris",
		PolyHavenAssetType.Model => "models",
		_ => "textures",
	};

	private static async Task<JsonObject> SearchAsync(PolyHaven polyHaven, PolyHavenAssetType type, string key, JsonObject? arguments,
		CancellationToken cancellationToken)
	{
		string? query = ReadString(arguments, "query");
		int limit = ReadLimit(arguments);
		List<PolyHavenAsset> found = await polyHaven.SearchAsync(type, query, limit, cancellationToken);
		JsonArray assets = [.. found.Select(asset => asset.ToJson())];
		return new JsonObject { [key] = assets, ["license"] = PolyHaven.License };
	}

	private static Task<JsonObject> ImportHdriAsync(PolyHaven polyHaven, EditorConnection connection, string id, string resolution,
		string folder, CancellationToken cancellationToken) =>
		ImportIntoProjectAsync(polyHaven, connection, id, resolution, folder, cancellationToken);

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
			throw new McpToolException(NoProject);
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

	/// <summary>Downloads the model into <paramref name="folder"/>/&lt;id&gt; of the project's Assets and registers its files:
	/// { mesh, textures, source, license }.</summary>
	public static async Task<JsonObject> ImportModelAsync(PolyHaven polyHaven, EditorConnection connection, string id, string resolution,
		string folder, CancellationToken cancellationToken)
	{
		string assets = await GetAssetDirectoryAsync(connection, cancellationToken);
		string gltf = await polyHaven.DownloadModelAsync(id, resolution, ResolveFolder(assets, folder), cancellationToken);
		JsonArray added = await RefreshAsync(connection, cancellationToken);
		string meshPath = ToAssetPath(assets, gltf);
		JsonNode? mesh = added.FirstOrDefault(asset => PathOf(asset) == meshPath)?.DeepClone();
		return new JsonObject
		{
			["mesh"] = mesh ?? throw new McpToolException($"the editor did not register '{meshPath}' as a mesh"),
			["textures"] = new JsonArray([.. added.Where(asset => PathOf(asset) != meshPath).Select(asset => asset?.DeepClone())]),
			["source"] = PolyHaven.GetPage(id),
			["license"] = PolyHaven.License,
		};
	}

	/// <summary>Downloads the texture set into <paramref name="folder"/>/&lt;id&gt; of the project's Assets, registers its maps and
	/// creates the material &lt;id&gt;.smat next to them: { material, textures, source, license }.</summary>
	public static async Task<JsonObject> ImportTextureAsync(PolyHaven polyHaven, EditorConnection connection, string id, string resolution,
		string folder, CancellationToken cancellationToken)
	{
		string assets = await GetAssetDirectoryAsync(connection, cancellationToken);
		PolyHavenTextureFiles files = await polyHaven.DownloadTextureAsync(id, resolution, ResolveFolder(assets, folder), cancellationToken);
		JsonArray added = await RefreshAsync(connection, cancellationToken);
		string? ReferenceOf(string? file)
		{
			if (file is null)
			{
				return null;
			}
			string path = ToAssetPath(assets, file);
			JsonNode? asset = added.FirstOrDefault(entry => PathOf(entry) == path);
			return asset?["reference"] is JsonValue reference && reference.TryGetValue(out string? text)
				? text
				: throw new McpToolException($"the editor did not register '{path}' as a texture");
		}

		// The factors multiply the maps: at 1 the maps decide.
		JsonObject fields = new() { ["BaseColorTexture"] = ReferenceOf(files.BaseColor), ["Metallic"] = 1.0, ["Roughness"] = 1.0 };
		if (ReferenceOf(files.Normal) is { } normal)
		{
			fields["NormalTexture"] = normal;
		}
		if (ReferenceOf(files.OcclusionRoughnessMetallic) is { } packed)
		{
			fields["MetallicRoughnessTexture"] = packed;
			fields["OcclusionTexture"] = packed;
		}
		else
		{
			fields["Metallic"] = 0.0;
			fields["Roughness"] = 0.8;
		}
		string materialPath = $"{ToAssetPath(assets, Path.GetDirectoryName(files.BaseColor)!)}/{id}.smat";
		JsonNode? material = await connection.CallAsync("material.create",
			new JsonObject { ["path"] = materialPath, ["fields"] = fields }, cancellationToken);
		return new JsonObject
		{
			// material.create answers { material, fields }: the asset is what callers use.
			["material"] = material?["material"]?.DeepClone(),
			["textures"] = new JsonArray([.. added.Select(asset => asset?.DeepClone())]),
			["source"] = PolyHaven.GetPage(id),
			["license"] = PolyHaven.License,
		};
	}

	// The open project's asset directory; checked before anything is downloaded.
	private static async Task<string> GetAssetDirectoryAsync(EditorConnection connection, CancellationToken cancellationToken)
	{
		JsonNode? project = await connection.CallAsync("project.info", null, cancellationToken);
		if (project?["project"] is not JsonObject info)
		{
			throw new McpToolException(NoProject);
		}
		return info["assetDirectory"] is JsonValue value && value.TryGetValue(out string? directory) && directory.Length > 0
			? directory
			: throw new McpToolException("the editor did not report the project's asset directory");
	}

	// A folder of the asset directory; folders that would reach outside it are refused.
	private static string ResolveFolder(string assetDirectory, string folder)
	{
		string root = Path.GetFullPath(assetDirectory);
		string resolved = Path.GetFullPath(Path.Combine(root, folder));
		string relative = Path.GetRelativePath(root, resolved);
		if (Path.IsPathRooted(folder) || relative == ".." || relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) ||
			Path.IsPathRooted(relative))
		{
			throw new McpToolException($"the folder '{folder}' is not inside the project's Assets");
		}
		return resolved;
	}

	// Registers the downloaded files: the assets the editor added.
	private static async Task<JsonArray> RefreshAsync(EditorConnection connection, CancellationToken cancellationToken)
	{
		JsonNode? refreshed = await connection.CallAsync("asset.refresh", null, cancellationToken);
		return refreshed?["added"] as JsonArray ?? [];
	}

	private static string ToAssetPath(string assetDirectory, string file) =>
		Path.GetRelativePath(Path.GetFullPath(assetDirectory), Path.GetFullPath(file)).Replace('\\', '/');

	private static string? PathOf(JsonNode? asset) => asset?["path"] is JsonValue value && value.TryGetValue(out string? path) ? path : null;

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
