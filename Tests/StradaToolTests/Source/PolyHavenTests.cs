using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Strada.Tool.Assets;
using Strada.Tool.Editor;
using Strada.Tool.Mcp;
using Xunit;

namespace Strada.Tool.Tests;

public sealed class PolyHavenTests
{
	private static string FilesAddress(string id) => $"{PolyHaven.ApiAddress}/files/{id}";

	[Fact]
	public async Task SearchesMatchEveryWordInNamesCategoriesAndTags()
	{
		FakePolyHaven fake = new();
		using HttpClient http = PolyHaven.CreateHttpClient(fake);
		PolyHaven polyHaven = new(http);

		List<PolyHavenHdri> skies = await polyHaven.SearchAsync("SKIES outdoor", 10, CancellationToken.None);
		Assert.Equal(["Blue Hour Sky", "Sunny Meadow"], skies.Select(hdri => hdri.Name));
		PolyHavenHdri meadow = skies[1];
		Assert.Equal(FakePolyHaven.MeadowId, meadow.Id);
		Assert.Equal(["outdoor", "skies"], meadow.Categories);
		Assert.Equal(["Ann Author"], meadow.Authors);
		Assert.Equal(["sunny_meadow"], (await polyHaven.SearchAsync("grass", 10, CancellationToken.None)).Select(hdri => hdri.Id));
		Assert.Empty(await polyHaven.SearchAsync("underwater", 10, CancellationToken.None));
		// Everything without a query (invalid IDs left out), at most the limit.
		Assert.Equal(3, (await polyHaven.SearchAsync(" ", 10, CancellationToken.None)).Count);
		Assert.Single(await polyHaven.SearchAsync(null, 1, CancellationToken.None));
	}

	[Fact]
	public async Task DownloadsAreCheckedAgainstTheCatalog()
	{
		using TemporaryDirectory directory = new();
		FakePolyHaven fake = new();
		using HttpClient http = PolyHaven.CreateHttpClient(fake);
		PolyHaven polyHaven = new(http);

		string file = await polyHaven.DownloadHdrAsync(FakePolyHaven.MeadowId, "1k", directory.Path, CancellationToken.None);
		Assert.Equal(Path.Combine(directory.Path, "sunny_meadow_1k.hdr"), file);
		Assert.Equal(FakePolyHaven.MeadowContents, File.ReadAllBytes(file));
		Assert.Equal([file], Directory.GetFiles(directory.Path));

		// A file that differs from the catalog's is not kept.
		using TemporaryDirectory damaged = new();
		fake.Set(FakePolyHaven.MeadowFile, HttpStatusCode.OK, Encoding.ASCII.GetBytes("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\nfake pixelz"));
		PolyHavenException wrongHash = await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.DownloadHdrAsync(FakePolyHaven.MeadowId, "1k", damaged.Path, CancellationToken.None));
		Assert.Contains("damaged", wrongHash.Message);
		fake.Set(FakePolyHaven.MeadowFile, HttpStatusCode.OK, [1, 2, 3]);
		PolyHavenException wrongSize = await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.DownloadHdrAsync(FakePolyHaven.MeadowId, "1k", damaged.Path, CancellationToken.None));
		Assert.Contains("3 bytes instead of", wrongSize.Message);
		Assert.Empty(Directory.GetFiles(damaged.Path));
	}

	[Fact]
	public async Task UnknownHdrisAndResolutionsAreReported()
	{
		using TemporaryDirectory directory = new();
		FakePolyHaven fake = new();
		using HttpClient http = PolyHaven.CreateHttpClient(fake);
		PolyHaven polyHaven = new(http);

		PolyHavenException missing = await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.DownloadHdrAsync("no_such_sky", "1k", directory.Path, CancellationToken.None));
		Assert.Equal("there is no HDRI 'no_such_sky'", missing.Message);
		PolyHavenException exrOnly = await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.DownloadHdrAsync(FakePolyHaven.MeadowId, "2k", directory.Path, CancellationToken.None));
		Assert.Equal("'sunny_meadow' has no 2k .hdr file (it has 1k)", exrOnly.Message);

		// Mistakes in the request are found before anything is sent.
		int requests = fake.Requests.Count;
		await Assert.ThrowsAsync<PolyHavenException>(() => polyHaven.DownloadHdrAsync("../escape", "1k", directory.Path, CancellationToken.None));
		await Assert.ThrowsAsync<PolyHavenException>(() => polyHaven.DownloadHdrAsync(FakePolyHaven.MeadowId, "3k", directory.Path, CancellationToken.None));
		Assert.Equal(requests, fake.Requests.Count);
		Assert.True(PolyHaven.IsValidId("kloofendal_48d_partly_cloudy_puresky"));
		Assert.False(PolyHaven.IsValidId("Studio Small"));
		Assert.False(PolyHaven.IsValidId(""));
	}

	[Fact]
	public async Task NetworkAndServerFailuresAreReported()
	{
		using TemporaryDirectory directory = new();
		FakePolyHaven fake = new();
		using HttpClient http = PolyHaven.CreateHttpClient(fake);
		PolyHaven polyHaven = new(http);

		fake.Set($"{PolyHaven.ApiAddress}/assets?t=hdris", HttpStatusCode.InternalServerError, []);
		Assert.Contains("answered 500", (await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.SearchAsync("sky", 10, CancellationToken.None))).Message);
		fake.Set($"{PolyHaven.ApiAddress}/assets?t=hdris", HttpStatusCode.OK, Encoding.UTF8.GetBytes("<html>maintenance</html>"));
		Assert.Contains("did not answer with JSON", (await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.SearchAsync("sky", 10, CancellationToken.None))).Message);

		// Downloads come from HTTPS addresses only.
		fake.SetJson(FilesAddress("plain_sky"), new JsonObject
		{
			["hdri"] = new JsonObject { ["1k"] = new JsonObject { ["hdr"] = FakePolyHaven.File("http://dl.example.org/plain_sky_1k.hdr", [1]) } },
		});
		Assert.Contains("no usable 1k file", (await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.DownloadHdrAsync("plain_sky", "1k", directory.Path, CancellationToken.None))).Message);

		fake.Offline = true;
		Assert.StartsWith("cannot reach api.polyhaven.com", (await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.SearchAsync("sky", 10, CancellationToken.None))).Message);
	}

	[Fact]
	public void RequestsNameStrada()
	{
		using HttpClient http = PolyHaven.CreateHttpClient();
		Assert.Equal($"strada/{ToolInfo.Version}", http.DefaultRequestHeaders.UserAgent.ToString());
	}
}

public sealed class PolyHavenToolsTests
{
	private static McpTestClient Connect(string instanceDirectory, FakePolyHaven fake, HttpClient http) =>
		new(new EditorSessionOptions { InstanceDirectory = instanceDirectory }, session => PolyHavenTools.Create(new PolyHaven(http), session));

	[Fact]
	public async Task TheToolsAreListedAfterTheEditorsCommands()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path);
		FakePolyHaven fake = new();
		using HttpClient http = PolyHaven.CreateHttpClient(fake);
		await using McpTestClient client = Connect(directory.Path, fake, http);
		await client.ResultAsync("initialize", new JsonObject { ["protocolVersion"] = "2025-06-18" });

		JsonArray tools = (await client.ResultAsync("tools/list"))["tools"]!.AsArray();
		Assert.Equal([PolyHavenTools.SearchName, PolyHavenTools.ImportName], tools.TakeLast(2).Select(tool => tool!["name"]!.GetValue<string>()));
		JsonObject import = tools[^1]!.AsObject();
		Assert.Equal("Import a Poly Haven HDRI", import["title"]!.GetValue<string>());
		Assert.True(import["annotations"]!["openWorldHint"]!.GetValue<bool>());
		Assert.False(import["annotations"]!["readOnlyHint"]!.GetValue<bool>());
		Assert.Equal(["id"], import["inputSchema"]!["required"]!.AsArray().Select(name => name!.GetValue<string>()));
		// The editor's own tools stay in the editor.
		Assert.False(tools[0]!["annotations"]!["openWorldHint"]!.GetValue<bool>());
	}

	[Fact]
	public async Task HdrisAreFoundAndImportedIntoTheProject()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path);
		FakePolyHaven fake = new();
		using HttpClient http = PolyHaven.CreateHttpClient(fake);
		await using McpTestClient client = Connect(directory.Path, fake, http);

		JsonObject search = await client.CallToolAsync(PolyHavenTools.SearchName, new JsonObject { ["query"] = "meadow" });
		Assert.False(search["isError"]!.GetValue<bool>(), McpTestClient.Text(search));
		JsonNode found = McpTestClient.ParseText(search);
		Assert.Equal(FakePolyHaven.MeadowId, found["hdris"]![0]!["id"]!.GetValue<string>());
		Assert.Contains("CC0", found["license"]!.GetValue<string>());

		JsonObject imported = await client.CallToolAsync(PolyHavenTools.ImportName,
			new JsonObject { ["id"] = FakePolyHaven.MeadowId, ["resolution"] = "1k", ["folder"] = "Skies" });
		Assert.False(imported["isError"]!.GetValue<bool>(), McpTestClient.Text(imported));
		JsonNode result = McpTestClient.ParseText(imported);
		Assert.Equal("asset://Skies/sunny_meadow_1k.hdr", result["asset"]!["reference"]!.GetValue<string>());
		Assert.Equal("https://polyhaven.com/a/sunny_meadow", result["source"]!.GetValue<string>());

		// The editor got the verified file, which strada removed again once the editor had copied it.
		(string folder, string file, byte[]? contents) = Assert.Single(editor.Imports);
		Assert.Equal("Skies", folder);
		Assert.Equal(FakePolyHaven.MeadowContents, contents);
		Assert.False(File.Exists(file));
		Assert.All(fake.UserAgents, agent => Assert.StartsWith("strada/", agent));
	}

	[Fact]
	public async Task ImportingNeedsAProjectAndAValidRequest()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path) { HasProject = false };
		FakePolyHaven fake = new();
		using HttpClient http = PolyHaven.CreateHttpClient(fake);
		await using McpTestClient client = Connect(directory.Path, fake, http);

		JsonObject noProject = await client.CallToolAsync(PolyHavenTools.ImportName, new JsonObject { ["id"] = FakePolyHaven.MeadowId });
		Assert.True(noProject["isError"]!.GetValue<bool>());
		Assert.Contains("no project open", McpTestClient.Text(noProject));
		// Nothing was downloaded for it.
		Assert.DoesNotContain(fake.Requests, address => address.Contains("files", System.StringComparison.Ordinal));

		editor.HasProject = true;
		JsonObject noId = await client.CallToolAsync(PolyHavenTools.ImportName);
		Assert.Equal("id is required: the HDRI's Poly Haven ID", McpTestClient.Text(noId));
		JsonObject badLimit = await client.CallToolAsync(PolyHavenTools.SearchName, new JsonObject { ["limit"] = 0 });
		Assert.True(badLimit["isError"]!.GetValue<bool>());
		JsonObject unknown = await client.CallToolAsync(PolyHavenTools.ImportName, new JsonObject { ["id"] = "no_such_sky" });
		Assert.Equal("Poly Haven: there is no HDRI 'no_such_sky'", McpTestClient.Text(unknown));
		Assert.Empty(editor.Imports);
	}
}
