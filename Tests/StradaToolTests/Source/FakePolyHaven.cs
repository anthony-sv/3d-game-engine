using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Strada.Tool.Assets;

namespace Strada.Tool.Tests;

/// <summary>A stand-in for Poly Haven's API and downloads: answers the requests strada makes from a few fixed responses,
/// and records them.</summary>
internal sealed class FakePolyHaven : HttpMessageHandler
{
	public const string MeadowId = "sunny_meadow";
	public const string MeadowFile = "https://dl.example.org/sunny_meadow_1k.hdr";

	private readonly ConcurrentDictionary<string, (HttpStatusCode Status, byte[] Body)> m_Responses = new(StringComparer.Ordinal);

	public FakePolyHaven()
	{
		JsonObject catalog = new()
		{
			[MeadowId] = Asset("Sunny Meadow", ["outdoor", "skies"], ["sunny", "grass"], "Ann Author"),
			["studio_small"] = Asset("Studio Small", ["indoor", "studio"], ["softbox"], "Bo Builder"),
			["blue_hour_sky"] = Asset("Blue Hour Sky", ["outdoor", "skies", "dusk"], ["clear"], "Ann Author"),
			// Not an ID strada accepts: it is left out.
			["Bad ID"] = Asset("Broken Entry", ["skies"], [], "Nobody"),
		};
		SetJson($"{PolyHaven.ApiAddress}/assets?t=hdris", catalog);
		SetJson($"{PolyHaven.ApiAddress}/files/{MeadowId}", new JsonObject
		{
			["hdri"] = new JsonObject
			{
				["1k"] = new JsonObject { ["hdr"] = File(MeadowFile, MeadowContents), ["exr"] = File("https://dl.example.org/sunny_meadow_1k.exr", [1]) },
				["2k"] = new JsonObject { ["exr"] = File("https://dl.example.org/sunny_meadow_2k.exr", [2]) },
			},
		});
		Set(MeadowFile, HttpStatusCode.OK, MeadowContents);

		SetJson($"{PolyHaven.ApiAddress}/assets?t=models", new JsonObject
		{
			[CrateId] = Asset("Wooden Crate", ["props"], ["box"], "Ann Author"),
			["ArmChair_01"] = Asset("Arm Chair 01", ["furniture"], ["chair"], "Bo Builder"),
		});
		SetJson($"{PolyHaven.ApiAddress}/files/{CrateId}", new JsonObject
		{
			["gltf"] = new JsonObject
			{
				["1k"] = new JsonObject
				{
					["gltf"] = Include(File(CrateFile("crate_1k.gltf"), CrateContents["crate_1k.gltf"]), new JsonObject
					{
						["crate.bin"] = File(CrateFile("crate.bin"), CrateContents["crate.bin"]),
						["textures/crate_diff_1k.jpg"] = File(CrateFile("crate_diff_1k.jpg"), CrateContents["textures/crate_diff_1k.jpg"]),
					}),
				},
			},
		});
		Set(CrateFile("crate_1k.gltf"), HttpStatusCode.OK, CrateContents["crate_1k.gltf"]);
		Set(CrateFile("crate.bin"), HttpStatusCode.OK, CrateContents["crate.bin"]);
		Set(CrateFile("crate_diff_1k.jpg"), HttpStatusCode.OK, CrateContents["textures/crate_diff_1k.jpg"]);

		SetJson($"{PolyHaven.ApiAddress}/files/{MudId}", new JsonObject
		{
			["Diffuse"] = new JsonObject { ["1k"] = new JsonObject { ["jpg"] = File(MudFile("mud_diff_1k.jpg"), [1]) } },
			["nor_gl"] = new JsonObject { ["1k"] = new JsonObject { ["jpg"] = File(MudFile("mud_nor_gl_1k.jpg"), [2]) } },
			["arm"] = new JsonObject { ["1k"] = new JsonObject { ["jpg"] = File(MudFile("mud_arm_1k.jpg"), [3]) } },
		});
		Set(MudFile("mud_diff_1k.jpg"), HttpStatusCode.OK, [1]);
		Set(MudFile("mud_nor_gl_1k.jpg"), HttpStatusCode.OK, [2]);
		Set(MudFile("mud_arm_1k.jpg"), HttpStatusCode.OK, [3]);
	}

	public const string CrateId = "wooden_crate";
	public const string MudId = "brown_mud";

	/// <summary>The crate model's files by their path in its download.</summary>
	public static IReadOnlyDictionary<string, byte[]> CrateContents { get; } = new Dictionary<string, byte[]>
	{
		["crate_1k.gltf"] = Encoding.ASCII.GetBytes("{\"asset\":{\"version\":\"2.0\"}}"),
		["crate.bin"] = [0, 1, 2, 3],
		["textures/crate_diff_1k.jpg"] = [9, 8, 7],
	};

	public static string CrateFile(string name) => $"https://dl.example.org/models/{CrateId}/{name}";

	public static string MudFile(string name) => $"https://dl.example.org/textures/{MudId}/{name}";

	/// <summary>A catalog entry of a glTF with the files it references.</summary>
	public static JsonObject Include(JsonObject file, JsonObject include)
	{
		file["include"] = include;
		return file;
	}

	/// <summary>The contents of the meadow's 1k .hdr file.</summary>
	public static byte[] MeadowContents { get; } = Encoding.ASCII.GetBytes("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\nfake pixels");

	/// <summary>The addresses requested, in order.</summary>
	public ConcurrentQueue<string> Requests { get; } = new();
	public ConcurrentQueue<string> UserAgents { get; } = new();
	/// <summary>When set, requests fail as without a network.</summary>
	public bool Offline { get; set; }

	public void Set(string address, HttpStatusCode status, byte[] body) => m_Responses[address] = (status, body);

	public void SetJson(string address, JsonNode json) => Set(address, HttpStatusCode.OK, Encoding.UTF8.GetBytes(json.ToJsonString()));

	/// <summary>A catalog entry of a downloadable file.</summary>
	public static JsonObject File(string url, byte[] contents) => new()
	{
		["url"] = url,
		["md5"] = Convert.ToHexStringLower(MD5.HashData(contents)),
		["size"] = contents.Length,
	};

	protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
	{
		string address = request.RequestUri!.ToString();
		Requests.Enqueue(address);
		UserAgents.Enqueue(request.Headers.UserAgent.ToString());
		if (Offline)
		{
			throw new HttpRequestException("No such host is known.");
		}
		(HttpStatusCode status, byte[] body) = m_Responses.TryGetValue(address, out (HttpStatusCode, byte[]) response)
			? response
			: (HttpStatusCode.NotFound, Encoding.UTF8.GetBytes("{\"error\":\"not found\"}"));
		return Task.FromResult(new HttpResponseMessage(status) { Content = new ByteArrayContent(body), RequestMessage = request });
	}

	private static JsonObject Asset(string name, string[] categories, string[] tags, string author) => new()
	{
		["name"] = name,
		["categories"] = new JsonArray([.. Array.ConvertAll(categories, category => (JsonNode?)JsonValue.Create(category))]),
		["tags"] = new JsonArray([.. Array.ConvertAll(tags, tag => (JsonNode?)JsonValue.Create(tag))]),
		["authors"] = new JsonObject { [author] = "All" },
	};
}
