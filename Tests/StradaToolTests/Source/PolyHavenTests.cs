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

		List<PolyHavenAsset> skies = await polyHaven.SearchAsync(PolyHavenAssetType.Hdri, "SKIES outdoor", 10, CancellationToken.None);
		Assert.Equal(["Blue Hour Sky", "Sunny Meadow"], skies.Select(hdri => hdri.Name));
		PolyHavenAsset meadow = skies[1];
		Assert.Equal(FakePolyHaven.MeadowId, meadow.Id);
		Assert.Equal(["outdoor", "skies"], meadow.Categories);
		Assert.Equal(["Ann Author"], meadow.Authors);
		Assert.Equal(["sunny_meadow"], (await polyHaven.SearchAsync(PolyHavenAssetType.Hdri, "grass", 10, CancellationToken.None)).Select(hdri => hdri.Id));
		Assert.Empty(await polyHaven.SearchAsync(PolyHavenAssetType.Hdri, "underwater", 10, CancellationToken.None));
		// Everything without a query (invalid IDs left out), at most the limit.
		Assert.Equal(3, (await polyHaven.SearchAsync(PolyHavenAssetType.Hdri, " ", 10, CancellationToken.None)).Count);
		Assert.Single(await polyHaven.SearchAsync(PolyHavenAssetType.Hdri, null, 1, CancellationToken.None));
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
	public async Task ModelsDownloadWithTheFilesTheyReference()
	{
		using TemporaryDirectory directory = new();
		FakePolyHaven fake = new();
		using HttpClient http = PolyHaven.CreateHttpClient(fake);
		PolyHaven polyHaven = new(http);

		Assert.Equal(["Arm Chair 01", "Wooden Crate"],
			(await polyHaven.SearchAsync(PolyHavenAssetType.Model, "", 10, CancellationToken.None)).Select(model => model.Name));
		string gltf = await polyHaven.DownloadModelAsync(FakePolyHaven.CrateId, "1k", directory.Path, CancellationToken.None);
		string modelDirectory = Path.Combine(directory.Path, FakePolyHaven.CrateId);
		Assert.Equal(Path.Combine(modelDirectory, "crate_1k.gltf"), gltf);
		foreach ((string path, byte[] contents) in FakePolyHaven.CrateContents)
		{
			Assert.Equal(contents, File.ReadAllBytes(Path.Combine(modelDirectory, path.Replace('/', Path.DirectorySeparatorChar))));
		}
		Assert.Equal([modelDirectory], Directory.GetDirectories(directory.Path));

		// A second download does not replace the first.
		PolyHavenException exists = await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.DownloadModelAsync(FakePolyHaven.CrateId, "1k", directory.Path, CancellationToken.None));
		Assert.Contains("already exists", exists.Message);
		Assert.True(PolyHaven.IsValidId("ArmChair_01"));
	}

	[Fact]
	public async Task ModelDownloadsAreAllOrNothingAndStayInTheirDirectory()
	{
		using TemporaryDirectory directory = new();
		FakePolyHaven fake = new();
		using HttpClient http = PolyHaven.CreateHttpClient(fake);
		PolyHaven polyHaven = new(http);
		string files = FilesAddress(FakePolyHaven.CrateId);

		// A damaged referenced file: nothing is kept.
		fake.Set(FakePolyHaven.CrateFile("crate.bin"), HttpStatusCode.OK, [0, 1, 2, 4]);
		Assert.Contains("damaged", (await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.DownloadModelAsync(FakePolyHaven.CrateId, "1k", directory.Path, CancellationToken.None))).Message);
		Assert.Empty(Directory.GetFileSystemEntries(directory.Path));

		// Paths that would leave the model's directory are refused before anything is downloaded.
		foreach (string path in new[] { "../escape.bin", "/absolute.bin", "textures/../../escape.jpg", "a\\b.bin", "", "textures//x.jpg" })
		{
			fake.SetJson(files, new JsonObject
			{
				["gltf"] = new JsonObject
				{
					["1k"] = new JsonObject
					{
						["gltf"] = FakePolyHaven.Include(FakePolyHaven.File(FakePolyHaven.CrateFile("crate_1k.gltf"), FakePolyHaven.CrateContents["crate_1k.gltf"]),
							new JsonObject { [path] = FakePolyHaven.File(FakePolyHaven.CrateFile("crate.bin"), [0]) }),
					},
				},
			});
			int requests = fake.Requests.Count;
			await Assert.ThrowsAsync<PolyHavenException>(() =>
				polyHaven.DownloadModelAsync(FakePolyHaven.CrateId, "1k", directory.Path, CancellationToken.None));
			Assert.Equal(requests + 1, fake.Requests.Count);
			Assert.Empty(Directory.GetFileSystemEntries(directory.Path));
		}

		// HDRIs are not models, and models need the resolution asked for.
		await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.DownloadModelAsync(FakePolyHaven.MeadowId, "1k", directory.Path, CancellationToken.None));
		fake.SetJson(files, new JsonObject { ["gltf"] = new JsonObject { ["2k"] = new JsonObject() } });
		await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.DownloadModelAsync(FakePolyHaven.CrateId, "1k", directory.Path, CancellationToken.None));
	}

	[Fact]
	public async Task TextureSetsDownloadTheirMaps()
	{
		using TemporaryDirectory directory = new();
		FakePolyHaven fake = new();
		using HttpClient http = PolyHaven.CreateHttpClient(fake);
		PolyHaven polyHaven = new(http);

		PolyHavenTextureFiles files = await polyHaven.DownloadTextureAsync(FakePolyHaven.MudId, "1k", directory.Path, CancellationToken.None);
		string mud = Path.Combine(directory.Path, FakePolyHaven.MudId);
		Assert.Equal(Path.Combine(mud, "mud_diff_1k.jpg"), files.BaseColor);
		Assert.Equal(Path.Combine(mud, "mud_nor_gl_1k.jpg"), files.Normal);
		Assert.Equal(Path.Combine(mud, "mud_arm_1k.jpg"), files.OcclusionRoughnessMetallic);
		Assert.Equal([2], File.ReadAllBytes(files.Normal!));

		// The maps besides the color are optional.
		using TemporaryDirectory colorOnly = new();
		fake.SetJson(FilesAddress(FakePolyHaven.MudId), new JsonObject
		{
			["Diffuse"] = new JsonObject { ["1k"] = new JsonObject { ["jpg"] = FakePolyHaven.File(FakePolyHaven.MudFile("mud_diff_1k.jpg"), [1]) } },
		});
		PolyHavenTextureFiles color = await polyHaven.DownloadTextureAsync(FakePolyHaven.MudId, "1k", colorOnly.Path, CancellationToken.None);
		Assert.Null(color.Normal);
		Assert.Null(color.OcclusionRoughnessMetallic);
		// Models and HDRIs are not texture sets.
		await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.DownloadTextureAsync(FakePolyHaven.CrateId, "1k", colorOnly.Path, CancellationToken.None));
		await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.DownloadTextureAsync(FakePolyHaven.MeadowId, "1k", colorOnly.Path, CancellationToken.None));
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
			polyHaven.SearchAsync(PolyHavenAssetType.Hdri, "sky", 10, CancellationToken.None))).Message);
		fake.Set($"{PolyHaven.ApiAddress}/assets?t=hdris", HttpStatusCode.OK, Encoding.UTF8.GetBytes("<html>maintenance</html>"));
		Assert.Contains("did not answer with JSON", (await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.SearchAsync(PolyHavenAssetType.Hdri, "sky", 10, CancellationToken.None))).Message);

		// Downloads come from HTTPS addresses only.
		fake.SetJson(FilesAddress("plain_sky"), new JsonObject
		{
			["hdri"] = new JsonObject { ["1k"] = new JsonObject { ["hdr"] = FakePolyHaven.File("http://dl.example.org/plain_sky_1k.hdr", [1]) } },
		});
		Assert.Contains("no usable 1k file", (await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.DownloadHdrAsync("plain_sky", "1k", directory.Path, CancellationToken.None))).Message);

		// Answers of an unexpected shape are reported, not thrown as other exceptions.
		foreach (string files in new[] { "[1]", "{\"hdri\":[1]}", "{\"hdri\":{\"1k\":[1]}}", "{\"hdri\":{\"1k\":\"x\",\"2k\":{\"hdr\":1}}}",
					 "{\"hdri\":{\"1k\":{\"hdr\":{\"url\":1}}}}", "{\"hdri\":1,\"hdri\":2}" })
		{
			fake.Set(FilesAddress("odd_sky"), HttpStatusCode.OK, Encoding.UTF8.GetBytes(files));
			await Assert.ThrowsAsync<PolyHavenException>(() => polyHaven.DownloadHdrAsync("odd_sky", "1k", directory.Path, CancellationToken.None));
		}
		foreach (string catalog in new[] { "[1]", "{\"a\":[1],\"b\":{\"name\":[1],\"categories\":{},\"tags\":[{}],\"authors\":[1]}}",
					 "{\"a\":{},\"a\":{}}" })
		{
			fake.Set($"{PolyHaven.ApiAddress}/assets?t=hdris", HttpStatusCode.OK, Encoding.UTF8.GetBytes(catalog));
			try
			{
				await polyHaven.SearchAsync(PolyHavenAssetType.Hdri, "", 10, CancellationToken.None);
			}
			catch (PolyHavenException)
			{
				// Refusing the catalog is fine; any other exception fails the test.
			}
		}

		fake.Offline = true;
		Assert.StartsWith("cannot reach api.polyhaven.com", (await Assert.ThrowsAsync<PolyHavenException>(() =>
			polyHaven.SearchAsync(PolyHavenAssetType.Hdri, "sky", 10, CancellationToken.None))).Message);
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
		Assert.Equal([PolyHavenTools.SearchName, PolyHavenTools.ImportName, PolyHavenTools.SearchModelsName, PolyHavenTools.ImportModelName,
				PolyHavenTools.SearchTexturesName, PolyHavenTools.ImportTextureName],
			tools.TakeLast(6).Select(tool => tool!["name"]!.GetValue<string>()));
		JsonObject import = tools[^5]!.AsObject();
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
	public async Task ModelsAndTextureSetsAreImportedIntoTheirOwnFolders()
	{
		using TemporaryDirectory directory = new();
		using TemporaryDirectory assets = new();
		await using FakeEditor editor = new(directory.Path) { AssetDirectory = assets.Path };
		FakePolyHaven fake = new();
		using HttpClient http = PolyHaven.CreateHttpClient(fake);
		await using McpTestClient client = Connect(directory.Path, fake, http);

		JsonObject search = await client.CallToolAsync(PolyHavenTools.SearchModelsName, new JsonObject { ["query"] = "chair" });
		Assert.Equal("ArmChair_01", McpTestClient.ParseText(search)["models"]![0]!["id"]!.GetValue<string>());

		JsonObject model = await client.CallToolAsync(PolyHavenTools.ImportModelName, new JsonObject { ["id"] = FakePolyHaven.CrateId });
		Assert.False(model["isError"]!.GetValue<bool>(), McpTestClient.Text(model));
		JsonNode modelResult = McpTestClient.ParseText(model);
		Assert.Equal("asset://Models/wooden_crate/crate_1k.gltf", modelResult["mesh"]!["reference"]!.GetValue<string>());
		Assert.Equal("Models/wooden_crate/textures/crate_diff_1k.jpg", modelResult["textures"]![0]!["path"]!.GetValue<string>());
		Assert.True(File.Exists(Path.Combine(assets.Path, "Models", "wooden_crate", "crate.bin")));

		JsonObject texture = await client.CallToolAsync(PolyHavenTools.ImportTextureName,
			new JsonObject { ["id"] = FakePolyHaven.MudId, ["folder"] = "Ground" });
		Assert.False(texture["isError"]!.GetValue<bool>(), McpTestClient.Text(texture));
		Assert.Equal("asset://Ground/brown_mud/brown_mud.smat", McpTestClient.ParseText(texture)["material"]!["reference"]!.GetValue<string>());
		JsonObject created = Assert.Single(editor.CreatedMaterials);
		JsonNode fields = created["fields"]!;
		Assert.Equal("asset://Ground/brown_mud/mud_diff_1k.jpg", fields["BaseColorTexture"]!.GetValue<string>());
		Assert.Equal("asset://Ground/brown_mud/mud_nor_gl_1k.jpg", fields["NormalTexture"]!.GetValue<string>());
		Assert.Equal("asset://Ground/brown_mud/mud_arm_1k.jpg", fields["MetallicRoughnessTexture"]!.GetValue<string>());
		Assert.Equal("asset://Ground/brown_mud/mud_arm_1k.jpg", fields["OcclusionTexture"]!.GetValue<string>());

		// Folders outside Assets are refused before anything is downloaded.
		int requests = fake.Requests.Count;
		JsonObject outside = await client.CallToolAsync(PolyHavenTools.ImportModelName,
			new JsonObject { ["id"] = FakePolyHaven.CrateId, ["folder"] = "../Elsewhere" });
		Assert.True(outside["isError"]!.GetValue<bool>());
		Assert.Contains("not inside the project's Assets", McpTestClient.Text(outside));
		Assert.Equal(requests, fake.Requests.Count);
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
		Assert.Equal("id is required: the asset's Poly Haven ID", McpTestClient.Text(noId));
		JsonObject badLimit = await client.CallToolAsync(PolyHavenTools.SearchName, new JsonObject { ["limit"] = 0 });
		Assert.True(badLimit["isError"]!.GetValue<bool>());
		JsonObject unknown = await client.CallToolAsync(PolyHavenTools.ImportName, new JsonObject { ["id"] = "no_such_sky" });
		Assert.Equal("Poly Haven: there is no HDRI 'no_such_sky'", McpTestClient.Text(unknown));
		Assert.Empty(editor.Imports);
	}
}
