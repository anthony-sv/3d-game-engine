using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace Strada.Tool.Assets;

/// <summary>The kinds of Poly Haven assets strada downloads.</summary>
internal enum PolyHavenAssetType
{
	/// <summary>Equirectangular sky and environment panoramas (Radiance .hdr).</summary>
	Hdri,
	/// <summary>Models with their materials (glTF with its buffer and textures).</summary>
	Model,
	/// <summary>PBR texture sets (color, normal, and ambient occlusion/roughness/metallic maps).</summary>
	Texture,
}

/// <summary>The maps of a downloaded Poly Haven texture set; the optional ones are null when the set has none.</summary>
internal sealed class PolyHavenTextureFiles
{
	public required string BaseColor { get; init; }
	public string? Normal { get; init; }
	/// <summary>Ambient occlusion (R), roughness (G) and metallic (B): glTF's packing.</summary>
	public string? OcclusionRoughnessMetallic { get; init; }
}

/// <summary>An asset of the Poly Haven catalog.</summary>
internal sealed class PolyHavenAsset
{
	public required string Id { get; init; }
	public required string Name { get; init; }
	public required IReadOnlyList<string> Categories { get; init; }
	public required IReadOnlyList<string> Authors { get; init; }

	public JsonObject ToJson() => new()
	{
		["id"] = Id,
		["name"] = Name,
		["categories"] = new JsonArray([.. Categories.Select(category => JsonValue.Create(category))]),
		["authors"] = new JsonArray([.. Authors.Select(author => JsonValue.Create(author))]),
	};
}

/// <summary>A Poly Haven request that failed: no connection, an unknown HDRI or resolution, or a damaged download.</summary>
internal sealed class PolyHavenException : Exception
{
	public PolyHavenException(string message)
		: base(message)
	{
	}

	public PolyHavenException(string message, Exception innerException)
		: base(message, innerException)
	{
	}
}

/// <summary>Assets from Poly Haven (https://polyhaven.com, CC0): the catalog's search, and downloads of HDRIs (equirectangular
/// Radiance files that the editor imports as environments for image-based lighting), models (glTF) and texture sets.
/// Every downloaded file is checked against the size and MD5 that the catalog lists.</summary>
internal sealed partial class PolyHaven
{
	public const string ApiAddress = "https://api.polyhaven.com";
	public const string DefaultResolution = "2k";
	public const string License = "CC0 (public domain), from Poly Haven: https://polyhaven.com";

	private readonly HttpClient m_Http;

	/// <param name="http">Sends the requests; <see cref="CreateHttpClient"/> makes the one strada uses.</param>
	public PolyHaven(HttpClient http)
	{
		m_Http = http;
	}

	/// <summary>The resolutions strada downloads (the catalog also has 16k files for some HDRIs).</summary>
	public static IReadOnlyList<string> Resolutions { get; } = ["1k", "2k", "4k", "8k"];

	/// <summary>An HTTP client that names strada in its User-Agent, as Poly Haven asks of the programs that use its API.</summary>
	/// <param name="handler">Sends the requests; null uses the network.</param>
	public static HttpClient CreateHttpClient(HttpMessageHandler? handler = null)
	{
		HttpClient http = handler is null ? new HttpClient() : new HttpClient(handler);
		http.Timeout = TimeSpan.FromMinutes(15);
		http.DefaultRequestHeaders.UserAgent.ParseAdd($"strada/{ToolInfo.Version}");
		return http;
	}

	/// <summary>Whether this is a Poly Haven asset ID: letters, digits and underscores (it becomes part of URLs and file
	/// names).</summary>
	public static bool IsValidId(string id) => IdPattern().IsMatch(id);

	/// <summary>The page of an asset on polyhaven.com.</summary>
	public static string GetPage(string id) => $"https://polyhaven.com/a/{id}";

	/// <summary>The assets of the type whose ID, name, categories or tags contain every word of <paramref name="query"/> (all of
	/// them for an empty query), ordered by name, at most <paramref name="maxResults"/>.</summary>
	public async Task<List<PolyHavenAsset>> SearchAsync(PolyHavenAssetType type, string? query, int maxResults,
		CancellationToken cancellationToken)
	{
		if (await GetJsonAsync($"{ApiAddress}/assets?t={CatalogType(type)}", null, cancellationToken) is not JsonObject catalog)
		{
			throw new PolyHavenException("the catalog is not a list of assets");
		}
		string[] words = (query ?? "").ToLowerInvariant().Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
		List<PolyHavenAsset> found = [];
		foreach ((string id, JsonNode? entry) in catalog)
		{
			if (entry is not JsonObject asset || !IsValidId(id))
			{
				continue;
			}
			string name = asset["name"] is JsonValue nameValue && nameValue.TryGetValue(out string? text) ? text : id;
			List<string> categories = Strings(asset["categories"]);
			IEnumerable<string> terms = new[] { id, name }.Concat(categories).Concat(Strings(asset["tags"]));
			string searched = string.Join(' ', terms).ToLowerInvariant();
			if (words.All(searched.Contains))
			{
				List<string> authors = asset["authors"] is JsonObject credits ? credits.Select(author => author.Key).ToList() : [];
				found.Add(new PolyHavenAsset { Id = id, Name = name, Categories = categories, Authors = authors });
			}
		}
		return found.OrderBy(asset => asset.Name, StringComparer.OrdinalIgnoreCase).ThenBy(asset => asset.Id, StringComparer.Ordinal)
			.Take(maxResults).ToList();
	}

	/// <summary>Downloads the HDRI's Radiance file at the resolution into <paramref name="directory"/> as
	/// &lt;id&gt;_&lt;resolution&gt;.hdr and returns its path. The file is checked against the size and MD5 that the catalog
	/// lists; a download that fails leaves nothing behind.</summary>
	public async Task<string> DownloadHdrAsync(string id, string resolution, string directory, CancellationToken cancellationToken)
	{
		CheckRequest(id, resolution);
		JsonNode? files = await GetJsonAsync($"{ApiAddress}/files/{id}", $"there is no HDRI '{id}'", cancellationToken);
		if (Member(files, "hdri") is not JsonObject resolutions)
		{
			throw new PolyHavenException($"'{id}' is not an HDRI (see {GetPage(id)})");
		}
		if (Member(resolutions[resolution], "hdr") is not JsonObject file)
		{
			string available = string.Join(", ", resolutions.Where(entry => Member(entry.Value, "hdr") is JsonObject).Select(entry => entry.Key));
			throw new PolyHavenException($"'{id}' has no {resolution} .hdr file (it has {available})");
		}
		RemoteFile remote = ReadRemoteFile(file) ?? throw new PolyHavenException($"the catalog lists no usable {resolution} file for '{id}'");

		string target = Path.Combine(directory, $"{id}_{resolution}.hdr");
		string partial = target + ".download";
		try
		{
			WriteFile(() => Directory.CreateDirectory(directory), target);
			await DownloadFileAsync(remote, partial, target, cancellationToken);
			WriteFile(() =>
			{
				File.Move(partial, target, overwrite: true);
				return true;
			}, target);
			return target;
		}
		finally
		{
			DeleteQuietly(partial);
		}
	}

	/// <summary>Downloads the model's glTF at the resolution with the files it references (its buffer and textures, at their
	/// relative paths) into the new directory <paramref name="directory"/>/&lt;id&gt; and returns the .gltf file's path. The
	/// directory must not exist yet; a download that fails leaves nothing behind.</summary>
	public async Task<string> DownloadModelAsync(string id, string resolution, string directory, CancellationToken cancellationToken)
	{
		CheckRequest(id, resolution);
		JsonNode? files = await GetJsonAsync($"{ApiAddress}/files/{id}", $"there is no model '{id}'", cancellationToken);
		if (Member(files, "gltf") is not JsonObject resolutions || Member(files, "hdri") is not null)
		{
			throw new PolyHavenException($"'{id}' is not a model (see {GetPage(id)})");
		}
		if (Member(resolutions[resolution], "gltf") is not JsonObject gltf)
		{
			string available = string.Join(", ", resolutions.Where(entry => Member(entry.Value, "gltf") is JsonObject).Select(entry => entry.Key));
			throw new PolyHavenException($"'{id}' has no {resolution} glTF (it has {available})");
		}
		RemoteFile main = ReadRemoteFile(gltf) ?? throw new PolyHavenException($"the catalog lists no usable {resolution} glTF for '{id}'");
		string mainName = Path.GetFileName(main.Address.AbsolutePath);
		List<(string Path, RemoteFile File)> downloads = [(mainName, main)];
		if (Member(gltf, "include") is JsonObject include)
		{
			foreach ((string path, JsonNode? entry) in include)
			{
				RemoteFile? referenced = entry is JsonObject included ? ReadRemoteFile(included) : null;
				if (referenced is null || !IsSafeRelativePath(path))
				{
					throw new PolyHavenException($"the catalog lists an unusable file '{path}' for '{id}'");
				}
				downloads.Add((path, referenced));
			}
		}
		if (!IsSafeRelativePath(mainName) || downloads.Select(download => download.Path).Distinct(StringComparer.OrdinalIgnoreCase).Count() != downloads.Count)
		{
			throw new PolyHavenException($"the catalog lists unusable files for '{id}'");
		}

		string target = Path.Combine(directory, id);
		await DownloadDirectoryAsync(downloads, target, cancellationToken);
		return Path.Combine(target, mainName);
	}

	/// <summary>Downloads the texture set's color, normal and ambient occlusion/roughness/metallic maps (JPEG) at the resolution
	/// into the new directory <paramref name="directory"/>/&lt;id&gt;. The directory must not exist yet; a download that fails
	/// leaves nothing behind.</summary>
	public async Task<PolyHavenTextureFiles> DownloadTextureAsync(string id, string resolution, string directory,
		CancellationToken cancellationToken)
	{
		CheckRequest(id, resolution);
		JsonNode? files = await GetJsonAsync($"{ApiAddress}/files/{id}", $"there is no texture '{id}'", cancellationToken);
		if (Member(files, "Diffuse") is not JsonObject diffuse || Member(files, "hdri") is not null || Member(files, "fbx") is not null)
		{
			throw new PolyHavenException($"'{id}' is not a texture set (see {GetPage(id)})");
		}
		if (ReadRemoteFile(Member(diffuse[resolution], "jpg")) is not { } color)
		{
			string available = string.Join(", ", diffuse.Where(entry => Member(entry.Value, "jpg") is JsonObject).Select(entry => entry.Key));
			throw new PolyHavenException($"'{id}' has no {resolution} color map (it has {available})");
		}
		RemoteFile? normal = ReadRemoteFile(Member(Member(Member(files, "nor_gl"), resolution), "jpg"));
		RemoteFile? arm = ReadRemoteFile(Member(Member(Member(files, "arm"), resolution), "jpg"));

		List<(string Path, RemoteFile File)> downloads = [(Path.GetFileName(color.Address.AbsolutePath), color)];
		if (normal is not null)
		{
			downloads.Add((Path.GetFileName(normal.Address.AbsolutePath), normal));
		}
		if (arm is not null)
		{
			downloads.Add((Path.GetFileName(arm.Address.AbsolutePath), arm));
		}
		if (downloads.Any(download => !IsSafeRelativePath(download.Path)) ||
			downloads.Select(download => download.Path).Distinct(StringComparer.OrdinalIgnoreCase).Count() != downloads.Count)
		{
			throw new PolyHavenException($"the catalog lists unusable files for '{id}'");
		}

		string target = Path.Combine(directory, id);
		await DownloadDirectoryAsync(downloads, target, cancellationToken);
		return new PolyHavenTextureFiles
		{
			BaseColor = Path.Combine(target, downloads[0].Path),
			Normal = normal is null ? null : Path.Combine(target, downloads[1].Path),
			OcclusionRoughnessMetallic = arm is null ? null : Path.Combine(target, downloads[^1].Path),
		};
	}

	private sealed record RemoteFile(Uri Address, string Md5, long Size);

	private static string CatalogType(PolyHavenAssetType type) => type switch
	{
		PolyHavenAssetType.Hdri => "hdris",
		PolyHavenAssetType.Model => "models",
		_ => "textures",
	};

	private static void CheckRequest(string id, string resolution)
	{
		if (!IsValidId(id))
		{
			throw new PolyHavenException($"'{id}' is not a Poly Haven asset ID (letters, digits and underscores)");
		}
		if (!Resolutions.Contains(resolution))
		{
			throw new PolyHavenException($"the resolution '{resolution}' is not one of {string.Join(", ", Resolutions)}");
		}
	}

	// An HTTPS file with the size and MD5 the catalog lists; null when the entry does not describe one.
	private static RemoteFile? ReadRemoteFile(JsonNode? file) =>
		Member(file, "url") is JsonValue urlValue && urlValue.TryGetValue(out string? url) &&
		Uri.TryCreate(url, UriKind.Absolute, out Uri? address) && address.Scheme == Uri.UriSchemeHttps &&
		Member(file, "md5") is JsonValue md5Value && md5Value.TryGetValue(out string? md5) &&
		Member(file, "size") is JsonValue sizeValue && sizeValue.TryGetValue(out long size) && size >= 0
			? new RemoteFile(address, md5, size)
			: null;

	// A path inside the download's directory: relative, with portable names, and without "." or ".." parts (the catalog decides
	// these paths, and they must not reach outside the directory).
	private static bool IsSafeRelativePath(string path) =>
		path.Length > 0 && path.Split('/').All(part => part is not ("" or "." or "..") && SafeNamePattern().IsMatch(part));

	// Downloads the files into a new directory: all of them, or none (a failed download removes what it wrote).
	private async Task DownloadDirectoryAsync(List<(string Path, RemoteFile File)> files, string target, CancellationToken cancellationToken)
	{
		if (Directory.Exists(target) || File.Exists(target))
		{
			throw new PolyHavenException($"'{target}' already exists: delete it, or download into another folder");
		}
		string staging = $"{target}.download-{Guid.NewGuid():N}";
		try
		{
			foreach ((string path, RemoteFile file) in files)
			{
				string destination = Path.Combine(staging, path.Replace('/', Path.DirectorySeparatorChar));
				WriteFile(() => Directory.CreateDirectory(Path.GetDirectoryName(destination)!), destination);
				await DownloadFileAsync(file, destination, destination, cancellationToken);
			}
			WriteFile(() =>
			{
				Directory.Move(staging, target);
				return true;
			}, target);
		}
		finally
		{
			DeleteDirectoryQuietly(staging);
		}
	}

	// Writes the file at the address to path and checks it against the catalog's size and MD5 (target names it in errors).
	private async Task DownloadFileAsync(RemoteFile file, string path, string target, CancellationToken cancellationToken)
	{
		string url = file.Address.ToString();
		using (HttpResponseMessage response = await SendAsync(file.Address, cancellationToken))
		{
			await using Stream body = await response.Content.ReadAsStreamAsync(cancellationToken);
			await using FileStream output = WriteFile(() => new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None), target);
			await CopyAsync(body, output, url, target, cancellationToken);
		}
		long length = new FileInfo(path).Length;
		if (length != file.Size)
		{
			throw new PolyHavenException($"the download of {url} has {length} bytes instead of {file.Size}");
		}
		string actual = await ComputeMd5Async(path, cancellationToken);
		if (!string.Equals(actual, file.Md5, StringComparison.OrdinalIgnoreCase))
		{
			throw new PolyHavenException($"the download of {url} is damaged (MD5 {actual} instead of {file.Md5})");
		}
	}

	[GeneratedRegex("^[A-Za-z0-9_]+$")]
	private static partial Regex IdPattern();

	[GeneratedRegex("^[A-Za-z0-9_.-]+$")]
	private static partial Regex SafeNamePattern();

	// The member of an object; null for anything else. The answers come from a server whose format may change, and JsonNode's
	// indexer throws for arrays and values.
	private static JsonNode? Member(JsonNode? node, string name) => node is JsonObject json ? json[name] : null;

	private static List<string> Strings(JsonNode? node) =>
		node is JsonArray array ? array.OfType<JsonValue>().Select(value => value.TryGetValue(out string? text) ? text : null).OfType<string>().ToList() : [];

	// Network and file errors are told apart: a broken download and a full disk call for different remedies.
	private static async Task CopyAsync(Stream body, Stream output, string url, string target, CancellationToken cancellationToken)
	{
		byte[] buffer = new byte[81920];
		while (true)
		{
			int read;
			try
			{
				read = await body.ReadAsync(buffer, cancellationToken);
			}
			catch (IOException exception)
			{
				throw new PolyHavenException($"the download of {url} broke off: {exception.Message}", exception);
			}
			if (read == 0)
			{
				return;
			}
			await WriteFile(async () =>
			{
				await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
				return true;
			}, target);
		}
	}

	private static T WriteFile<T>(Func<T> write, string target)
	{
		try
		{
			return write();
		}
		catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
		{
			throw new PolyHavenException($"cannot write '{target}': {exception.Message}", exception);
		}
	}

	private static async Task<T> WriteFile<T>(Func<Task<T>> write, string target)
	{
		try
		{
			return await write();
		}
		catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
		{
			throw new PolyHavenException($"cannot write '{target}': {exception.Message}", exception);
		}
	}

	private static void DeleteDirectoryQuietly(string path)
	{
		try
		{
			if (Directory.Exists(path))
			{
				Directory.Delete(path, recursive: true);
			}
		}
		catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
		{
			// A leftover partial download is harmless: it has a unique name, and the system cleans temporary files up.
		}
	}

	private static void DeleteQuietly(string path)
	{
		try
		{
			File.Delete(path);
		}
		catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
		{
			// A leftover partial download is harmless: the next one replaces it.
		}
	}

	// MD5 is what the catalog lists: it detects damaged downloads (HTTPS already authenticates the server).
	private static async Task<string> ComputeMd5Async(string path, CancellationToken cancellationToken)
	{
		await using FileStream file = File.OpenRead(path);
		return Convert.ToHexStringLower(await MD5.HashDataAsync(file, cancellationToken));
	}

	private async Task<JsonNode?> GetJsonAsync(string address, string? notFound, CancellationToken cancellationToken)
	{
		using HttpResponseMessage response = await SendAsync(new Uri(address), cancellationToken, notFound);
		try
		{
			// JsonObject throws on duplicate members when they are first read; refused here, they are a parse error.
			return JsonNode.Parse(await response.Content.ReadAsStringAsync(cancellationToken),
				documentOptions: new JsonDocumentOptions { AllowDuplicateProperties = false });
		}
		catch (JsonException exception)
		{
			throw new PolyHavenException($"{address} did not answer with JSON", exception);
		}
	}

	private async Task<HttpResponseMessage> SendAsync(Uri address, CancellationToken cancellationToken, string? notFound = null)
	{
		HttpResponseMessage response;
		try
		{
			response = await m_Http.GetAsync(address, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
		}
		catch (HttpRequestException exception)
		{
			throw new PolyHavenException($"cannot reach {address.Host}: {exception.Message}", exception);
		}
		catch (TaskCanceledException exception) when (!cancellationToken.IsCancellationRequested)
		{
			throw new PolyHavenException($"{address} did not answer in time", exception);
		}
		if (response.IsSuccessStatusCode)
		{
			return response;
		}
		response.Dispose();
		throw new PolyHavenException(response.StatusCode == HttpStatusCode.NotFound && notFound is not null
			? notFound
			: $"{address} answered {(int)response.StatusCode} {response.ReasonPhrase}");
	}
}
