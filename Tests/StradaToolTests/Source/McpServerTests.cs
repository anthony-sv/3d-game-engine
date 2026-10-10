using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using Strada.Tool.Editor;
using Strada.Tool.Mcp;
using Xunit;

namespace Strada.Tool.Tests;

public sealed class McpServerTests
{
	private static McpTestClient Connect(string instanceDirectory) =>
		new(new EditorSessionOptions { InstanceDirectory = instanceDirectory });

	[Fact]
	public async Task InitializeAnswersWithTheRequestedOrTheNewestVersion()
	{
		using TemporaryDirectory directory = new();
		await using McpTestClient client = new(new EditorSessionOptions { InstanceDirectory = directory.Path });

		JsonObject result = await client.ResultAsync("initialize", new JsonObject
		{
			["protocolVersion"] = "2025-06-18",
			["capabilities"] = new JsonObject(),
			["clientInfo"] = new JsonObject { ["name"] = "test", ["version"] = "1" },
		});
		Assert.Equal("2025-06-18", result["protocolVersion"]!.GetValue<string>());
		Assert.Equal("strada", result["serverInfo"]!["name"]!.GetValue<string>());
		Assert.Equal("Strada Editor", result["serverInfo"]!["title"]!.GetValue<string>());
		Assert.False(result["capabilities"]!["tools"]!["listChanged"]!.GetValue<bool>());
		Assert.Contains("editor_status", result["instructions"]!.GetValue<string>());

		JsonObject unknown = await client.ResultAsync("initialize", new JsonObject { ["protocolVersion"] = "1999-01-01" });
		Assert.Equal(McpServer.ProtocolVersions[0], unknown["protocolVersion"]!.GetValue<string>());
		JsonObject oldest = await client.ResultAsync("initialize", new JsonObject { ["protocolVersion"] = "2024-11-05" });
		Assert.Equal("2024-11-05", oldest["protocolVersion"]!.GetValue<string>());
		Assert.Null(oldest["serverInfo"]!["title"]);
		Assert.Empty((await client.ResultAsync("ping")).AsEnumerable());
	}

	[Fact]
	public async Task TheToolsAreTheEditorsCommands()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path);
		await using McpTestClient client = Connect(directory.Path);
		await client.ResultAsync("initialize", new JsonObject { ["protocolVersion"] = "2025-06-18" });

		JsonArray tools = (await client.ResultAsync("tools/list"))["tools"]!.AsArray();
		Assert.Equal(["editor_status", "entity_create", "entity_get", "asset_create_folder", "viewport_screenshot", "test_slow"],
			tools.Select(tool => tool!["name"]!.GetValue<string>()));
		JsonObject create = tools[1]!.AsObject();
		Assert.Equal("entity.create", create["title"]!.GetValue<string>());
		Assert.Equal("Creates an entity (editor command entity.create)", create["description"]!.GetValue<string>());
		Assert.Equal("string", create["inputSchema"]!["properties"]!["name"]!["type"]!.GetValue<string>());
		Assert.False(create["annotations"]!["readOnlyHint"]!.GetValue<bool>());
		// Commands without parameters take an empty object.
		JsonObject status = tools[0]!.AsObject();
		Assert.Equal("object", status["inputSchema"]!["type"]!.GetValue<string>());
		Assert.Empty(status["inputSchema"]!["properties"]!.AsObject());
		Assert.True(status["annotations"]!["readOnlyHint"]!.GetValue<bool>());

		// Older clients get neither titles nor annotations.
		await client.ResultAsync("initialize", new JsonObject { ["protocolVersion"] = "2024-11-05" });
		JsonObject plain = (await client.ResultAsync("tools/list"))["tools"]![1]!.AsObject();
		Assert.Null(plain["title"]);
		Assert.Null(plain["annotations"]);
		Assert.Equal(1, editor.Connections);
	}

	[Fact]
	public async Task ToolCallsRunTheirCommands()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path);
		await using McpTestClient client = Connect(directory.Path);

		JsonObject created = await client.CallToolAsync("entity_create", new JsonObject { ["name"] = "Crate" });
		Assert.False(created["isError"]!.GetValue<bool>());
		JsonNode entity = McpTestClient.ParseText(created);
		Assert.Equal("42", entity["id"]!.GetValue<string>());
		Assert.Equal("Crate", entity["name"]!.GetValue<string>());

		// Screenshots are images, followed by the rest of the result.
		JsonObject screenshot = await client.CallToolAsync("viewport_screenshot");
		JsonArray content = screenshot["content"]!.AsArray();
		Assert.Equal("image", content[0]!["type"]!.GetValue<string>());
		Assert.Equal("image/png", content[0]!["mimeType"]!.GetValue<string>());
		Assert.Equal("iVBORw0KGgo=", content[0]!["data"]!.GetValue<string>());
		JsonNode details = McpTestClient.ParseText(screenshot);
		Assert.Equal(2, details["width"]!.GetValue<int>());
		Assert.Null(details["data"]);
	}

	[Fact]
	public async Task CommandErrorsAreToolErrorsTheModelReads()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path);
		await using McpTestClient client = Connect(directory.Path);

		JsonObject failed = await client.CallToolAsync("entity_get", new JsonObject { ["entity"] = "123" });
		Assert.True(failed["isError"]!.GetValue<bool>());
		Assert.Equal("EntityNotFound (1001): entity 123 does not exist\n{\"path\":\"entity\"}", McpTestClient.Text(failed));
	}

	[Fact]
	public async Task ProtocolMistakesAreJsonRpcErrors()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path);
		await using McpTestClient client = Connect(directory.Path);

		Assert.Equal(McpServer.MethodNotFound, (await client.RequestAsync("resources/list"))["error"]!["code"]!.GetValue<int>());
		JsonObject unknownTool = await client.RequestAsync("tools/call", new JsonObject { ["name"] = "no_such_tool" });
		Assert.Equal(McpServer.InvalidParams, unknownTool["error"]!["code"]!.GetValue<int>());
		Assert.Equal("Unknown tool: no_such_tool", unknownTool["error"]!["message"]!.GetValue<string>());
		JsonObject badArguments = await client.RequestAsync("tools/call", new JsonObject { ["name"] = "entity_create", ["arguments"] = "x" });
		Assert.Equal(McpServer.InvalidParams, badArguments["error"]!["code"]!.GetValue<int>());

		client.Send("{not json");
		JsonNode parseError = await client.ReceiveAsync();
		Assert.Equal(McpServer.ParseError, parseError["error"]!["code"]!.GetValue<int>());
		Assert.Null(parseError["id"]);
		client.Send("{\"jsonrpc\":\"2.0\",\"id\":\"no-method\"}");
		JsonNode noMethod = await client.ReceiveAsync();
		Assert.Equal(McpServer.InvalidRequest, noMethod["error"]!["code"]!.GetValue<int>());
		Assert.Equal("no-method", noMethod["id"]!.GetValue<string>());

		// Notifications and responses get no answer: the next message is the ping's.
		client.Send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
		client.Send("{\"jsonrpc\":\"2.0\",\"id\":99,\"result\":{}}");
		Assert.NotNull((await client.RequestAsync("ping"))["result"]);
	}

	[Fact]
	public async Task CancelledRequestsGetNoResponse()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path);
		await using McpTestClient client = Connect(directory.Path);
		await client.ResultAsync("tools/list");

		client.Send(McpTestClient.Request("slow", "tools/call", new JsonObject { ["name"] = "test_slow" }));
		// Other requests are answered while it runs.
		client.Send(McpTestClient.Request("first", "ping"));
		Assert.Equal("first", (await client.ReceiveAsync())["id"]!.GetValue<string>());

		client.Send(new JsonObject
		{
			["jsonrpc"] = "2.0",
			["method"] = "notifications/cancelled",
			["params"] = new JsonObject { ["requestId"] = "slow", ["reason"] = "test" },
		});
		// Messages are read in order: once this ping is answered, the cancellation was handled.
		client.Send(McpTestClient.Request("second", "ping"));
		Assert.Equal("second", (await client.ReceiveAsync())["id"]!.GetValue<string>());

		// The editor answers the cancelled request first, then this one: only this one is answered.
		editor.ReleaseSlowCommands();
		client.Send(McpTestClient.Request("answered", "tools/call", new JsonObject { ["name"] = "test_slow" }));
		JsonNode answer = await client.ReceiveAsync();
		Assert.Equal("answered", answer["id"]!.GetValue<string>());
		Assert.True(McpTestClient.ParseText(answer["result"]!.AsObject())["done"]!.GetValue<bool>());
		await client.StopAsync();
		Assert.Empty(client.TakeRemainingMessages());
	}

	[Fact]
	public async Task BatchesAreAnsweredTogether()
	{
		using TemporaryDirectory directory = new();
		await using McpTestClient client = new(new EditorSessionOptions { InstanceDirectory = directory.Path });

		client.Send(new JsonArray(McpTestClient.Request(1, "ping"), new JsonObject { ["jsonrpc"] = "2.0", ["method"] = "notifications/initialized" },
			McpTestClient.Request(2, "ping")));
		JsonArray answers = (await client.ReceiveAsync()).AsArray();
		Assert.Equal([1, 2], answers.Select(answer => answer!["id"]!.GetValue<int>()).Order());
		client.Send("[]");
		Assert.Equal(McpServer.InvalidRequest, (await client.ReceiveAsync())["error"]!["code"]!.GetValue<int>());
	}

	[Fact]
	public async Task BrokenConnectionsAreReplaced()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path);
		await using McpTestClient client = Connect(directory.Path);
		Assert.False((await client.CallToolAsync("editor_status"))["isError"]!.GetValue<bool>());

		await editor.DropConnectionsAsync();
		// A call that still finds the old connection fails with it; the editor still runs, so the next call connects again.
		JsonObject first = await client.CallToolAsync("editor_status");
		if (first["isError"]!.GetValue<bool>())
		{
			Assert.Contains("connection", McpTestClient.Text(first));
		}
		JsonObject status = await client.CallToolAsync("editor_status");
		Assert.False(status["isError"]!.GetValue<bool>(), McpTestClient.Text(status));
		Assert.Equal(2, editor.Connections);
	}

	[Fact]
	public async Task WithoutAnEditorTheToolsCannotBeListed()
	{
		using TemporaryDirectory directory = new();
		string missing = Path.Combine(directory.Path, "missing", EditorLocator.ExecutableName);
		await using McpTestClient client = new(new EditorSessionOptions { InstanceDirectory = directory.Path, EditorPath = missing });

		JsonObject response = await client.RequestAsync("tools/list");
		Assert.Equal(McpServer.InternalError, response["error"]!["code"]!.GetValue<int>());
		Assert.Contains("does not exist", response["error"]!["message"]!.GetValue<string>());
	}
}
