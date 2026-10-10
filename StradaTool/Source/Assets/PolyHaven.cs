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

/// <summary>An HDRI of the Poly Haven catalog.</summary>
internal sealed class PolyHavenHdri
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

/// <summary>HDRIs from Poly Haven (https://polyhaven.com/hdris, CC0): the catalog's search, and downloads of the
/// equirectangular Radiance (.hdr) files that the editor imports as environments for image-based lighting.</summary>
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

	/// <summary>Whether this is a Poly Haven asset ID: lowercase letters, digits and underscores (it becomes part of URLs
	/// and file names).</summary>
	public static bool IsValidId(string id) => IdPattern().IsMatch(id);

	/// <summary>The page of an HDRI on polyhaven.com.</summary>
	public static string GetPage(string id) => $"https://polyhaven.com/a/{id}";

	/// <summary>The HDRIs whose ID, name, categories or tags contain every word of <paramref name="query"/> (all HDRIs for an
	/// empty query), ordered by name, at most <paramref name="maxResults"/>.</summary>
	public async Task<List<PolyHavenHdri>> SearchAsync(string? query, int maxResults, CancellationToken cancellationToken)
	{
		if (await GetJsonAsync($"{ApiAddress}/assets?t=hdris", null, cancellationToken) is not JsonObject catalog)
		{
			throw new PolyHavenException("the catalog is not a list of assets");
		}
		string[] words = (query ?? "").ToLowerInvariant().Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
		List<PolyHavenHdri> found = [];
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
				found.Add(new PolyHavenHdri { Id = id, Name = name, Categories = categories, Authors = authors });
			}
		}
		return found.OrderBy(hdri => hdri.Name, StringComparer.OrdinalIgnoreCase).ThenBy(hdri => hdri.Id, StringComparer.Ordinal)
			.Take(maxResults).ToList();
	}

	/// <summary>Downloads the HDRI's Radiance file at the resolution into <paramref name="directory"/> as
	/// &lt;id&gt;_&lt;resolution&gt;.hdr and returns its path. The file is checked against the size and MD5 that the catalog
	/// lists; a download that fails leaves nothing behind.</summary>
	public async Task<string> DownloadHdrAsync(string id, string resolution, string directory, CancellationToken cancellationToken)
	{
		if (!IsValidId(id))
		{
			throw new PolyHavenException($"'{id}' is not a Poly Haven asset ID (lowercase letters, digits and underscores)");
		}
		if (!Resolutions.Contains(resolution))
		{
			throw new PolyHavenException($"the resolution '{resolution}' is not one of {string.Join(", ", Resolutions)}");
		}
		JsonNode? files = await GetJsonAsync($"{ApiAddress}/files/{id}", $"there is no HDRI '{id}'", cancellationToken);
		if (files?["hdri"] is not JsonObject resolutions)
		{
			throw new PolyHavenException($"'{id}' is not an HDRI (see {GetPage(id)})");
		}
		if (resolutions[resolution]?["hdr"] is not JsonObject file)
		{
			string available = string.Join(", ", resolutions.Where(entry => entry.Value?["hdr"] is JsonObject).Select(entry => entry.Key));
			throw new PolyHavenException($"'{id}' has no {resolution} .hdr file (it has {available})");
		}
		if (file["url"] is not JsonValue urlValue || !urlValue.TryGetValue(out string? url) ||
			!Uri.TryCreate(url, UriKind.Absolute, out Uri? address) || address.Scheme != Uri.UriSchemeHttps ||
			file["md5"] is not JsonValue md5Value || !md5Value.TryGetValue(out string? md5) ||
			file["size"] is not JsonValue sizeValue || !sizeValue.TryGetValue(out long size))
		{
			throw new PolyHavenException($"the catalog lists no usable {resolution} file for '{id}'");
		}

		string target = Path.Combine(directory, $"{id}_{resolution}.hdr");
		string partial = target + ".download";
		try
		{
			using (HttpResponseMessage response = await SendAsync(address, cancellationToken))
			{
				await using Stream body = await response.Content.ReadAsStreamAsync(cancellationToken);
				await using FileStream output = WriteFile(() =>
				{
					Directory.CreateDirectory(directory);
					return new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None);
				}, target);
				await CopyAsync(body, output, url, target, cancellationToken);
			}
			long length = new FileInfo(partial).Length;
			if (length != size)
			{
				throw new PolyHavenException($"the download of {url} has {length} bytes instead of {size}");
			}
			string actual = await ComputeMd5Async(partial, cancellationToken);
			if (!string.Equals(actual, md5, StringComparison.OrdinalIgnoreCase))
			{
				throw new PolyHavenException($"the download of {url} is damaged (MD5 {actual} instead of {md5})");
			}
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

	[GeneratedRegex("^[a-z0-9_]+$")]
	private static partial Regex IdPattern();

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
			return JsonNode.Parse(await response.Content.ReadAsStringAsync(cancellationToken));
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
