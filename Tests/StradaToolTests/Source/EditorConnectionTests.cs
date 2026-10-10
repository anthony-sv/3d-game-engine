using System.IO;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Strada.Tool.Editor;
using Xunit;

namespace Strada.Tool.Tests;

public sealed class EditorConnectionTests
{
	private static EditorInstance ReadInstance(FakeEditor editor) => EditorInstance.Parse(File.ReadAllText(editor.InstanceFile));

	[Fact]
	public async Task ConnectionsAuthenticateAndRunCommandsConcurrently()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path);
		await using EditorConnection connection = await EditorConnection.ConnectAsync(ReadInstance(editor), CancellationToken.None);
		Assert.True(connection.IsOpen);
		Assert.Equal(1, editor.Connections);

		Task<JsonNode?> slow = connection.CallAsync("test.slow", null, CancellationToken.None);
		JsonNode? created = await connection.CallAsync("entity.create", new JsonObject { ["name"] = "Crate" }, CancellationToken.None);
		Assert.Equal("Crate", created!["name"]!.GetValue<string>());
		Assert.False(slow.IsCompleted);
		editor.ReleaseSlowCommands();
		Assert.True((await slow)!["done"]!.GetValue<bool>());

		EditorCommandException error =
			await Assert.ThrowsAsync<EditorCommandException>(() => connection.CallAsync("entity.get", null, CancellationToken.None));
		Assert.Equal(1001, error.Code);
		Assert.Equal("EntityNotFound", error.CodeName);
		Assert.Equal("entity", error.ErrorData!["path"]!.GetValue<string>());
	}

	[Fact]
	public async Task WrongTokensAreRefused()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path);
		EditorInstance instance = ReadInstance(editor);
		EditorInstance forged = new()
		{
			ProcessId = instance.ProcessId,
			Port = instance.Port,
			Token = "wrong",
			Project = instance.Project,
			Version = instance.Version,
		};

		EditorCommandException refused =
			await Assert.ThrowsAsync<EditorCommandException>(() => EditorConnection.ConnectAsync(forged, CancellationToken.None));
		Assert.Equal(-32001, refused.Code);
		Assert.Equal(0, editor.Connections);
	}

	[Fact]
	public async Task WaitingCallsFailWhenTheConnectionEnds()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path);
		await using EditorConnection connection = await EditorConnection.ConnectAsync(ReadInstance(editor), CancellationToken.None);

		Task<JsonNode?> slow = connection.CallAsync("test.slow", null, CancellationToken.None);
		await editor.DropConnectionsAsync();
		await Assert.ThrowsAsync<EditorUnavailableException>(() => slow);
		await connection.Completion;
		Assert.False(connection.IsOpen);
		await Assert.ThrowsAsync<EditorUnavailableException>(() => connection.CallAsync("editor.status", null, CancellationToken.None));
	}

	[Fact]
	public async Task CancellingStopsWaiting()
	{
		using TemporaryDirectory directory = new();
		await using FakeEditor editor = new(directory.Path);
		await using EditorConnection connection = await EditorConnection.ConnectAsync(ReadInstance(editor), CancellationToken.None);

		using CancellationTokenSource cancellation = new();
		Task<JsonNode?> slow = connection.CallAsync("test.slow", null, cancellation.Token);
		await cancellation.CancelAsync();
		await Assert.ThrowsAnyAsync<System.OperationCanceledException>(() => slow);
		// The late answer is dropped; the connection goes on.
		editor.ReleaseSlowCommands();
		Assert.NotNull(await connection.CallAsync("editor.status", null, CancellationToken.None));
	}

	[Fact]
	public async Task ClosedPortsAreUnavailable()
	{
		using TemporaryDirectory directory = new();
		EditorInstance instance;
		await using (FakeEditor editor = new(directory.Path))
		{
			instance = ReadInstance(editor);
		}
		await Assert.ThrowsAsync<EditorUnavailableException>(() => EditorConnection.ConnectAsync(instance, CancellationToken.None));
	}
}
