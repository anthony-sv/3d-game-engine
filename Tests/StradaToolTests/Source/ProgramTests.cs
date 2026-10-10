using System.IO;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Xunit;

namespace Strada.Tool.Tests;

public sealed class ProgramTests
{
	private static async Task<(int ExitCode, string Output, string Error)> RunAsync(params string[] args)
	{
		using StringWriter output = new();
		using StringWriter error = new();
		int exitCode = await Program.RunAsync(args, output, error, CancellationToken.None);
		return (exitCode, output.ToString(), error.ToString());
	}

	[Fact]
	public async Task HelpAndVersionArePrinted()
	{
		(int exitCode, string output, string error) = await RunAsync("--version");
		Assert.Equal(Program.Success, exitCode);
		Assert.Equal($"strada {ToolInfo.Version}", output.Trim());
		Assert.Empty(error);
		Assert.Matches(@"^\d+\.\d+\.\d+$", ToolInfo.Version);

		(exitCode, output, _) = await RunAsync("--help");
		Assert.Equal(Program.Success, exitCode);
		Assert.Contains("strada <command>", output);
		(exitCode, _, error) = await RunAsync();
		Assert.Equal(Program.UsageError, exitCode);
		Assert.Contains("Usage:", error);
	}

	[Theory]
	[InlineData("bogus")]
	[InlineData("mcp", "--json")]
	[InlineData("mcp", "operand")]
	[InlineData("call")]
	[InlineData("call", "entity.create", "[1, 2]")]
	[InlineData("call", "entity.create", "{not json")]
	[InlineData("instances", "--pid", "nope")]
	[InlineData("hdri")]
	[InlineData("hdri", "first", "second")]
	[InlineData("hdri", "sunny_meadow", "--output", "out", "--folder", "Skies")]
	[InlineData("hdris", "--resolution", "1k")]
	public async Task MistakesExitWithTwo(params string[] args)
	{
		(int exitCode, string output, string error) = await RunAsync(args);
		Assert.Equal(Program.UsageError, exitCode);
		Assert.Empty(output);
		Assert.StartsWith("strada: ", error);
		Assert.Contains("strada --help", error);
	}

	[Fact]
	public async Task RunningEditorsAreListedAsJson()
	{
		(int exitCode, string output, string error) = await RunAsync("instances", "--json");
		Assert.Equal(Program.Success, exitCode);
		Assert.Empty(error);
		// Whatever editors run on this machine: a list.
		Assert.IsType<JsonArray>(JsonNode.Parse(output));
	}
}
