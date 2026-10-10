using System.Collections.Generic;
using Xunit;

namespace Strada.Tool.Tests;

public sealed class ToolArgumentsTests
{
	[Fact]
	public void CommandsOperandsAndOptionsAreParsed()
	{
		ToolArguments arguments = ToolArguments.Parse(["mcp", "--editor", "/opt/StradaEditor", "--project=Game/Game.sproj", "--headless", "--new"]);
		Assert.Equal("mcp", arguments.Command);
		Assert.Empty(arguments.Operands);
		Assert.Equal("/opt/StradaEditor", arguments.Editor);
		Assert.Equal("Game/Game.sproj", arguments.Project);
		Assert.True(arguments.Headless);
		Assert.True(arguments.StartNew);
		Assert.False(arguments.Json);

		ToolArguments call = ToolArguments.Parse(["call", "entity.create", "{\"name\":\"Crate\"}", "--pid", "1234"]);
		Assert.Equal("call", call.Command);
		Assert.Equal(["entity.create", "{\"name\":\"Crate\"}"], call.Operands);
		Assert.Equal(1234, call.ProcessId);

		// After --, everything is an operand.
		ToolArguments separated = ToolArguments.Parse(["call", "--", "--odd-command"]);
		Assert.Equal(["--odd-command"], separated.Operands);
		Assert.True(ToolArguments.Parse(["-h"]).Help);
		Assert.True(ToolArguments.Parse(["--version"]).Version);
	}

	[Theory]
	[InlineData("--unknown")]
	[InlineData("--editor")]
	[InlineData("--editor=")]
	[InlineData("--headless=yes")]
	[InlineData("--pid", "x")]
	[InlineData("--pid", "-5")]
	[InlineData("--pid", "0")]
	[InlineData("--json", "--json")]
	public void MistakesAreUsageErrors(params string[] arguments)
	{
		Assert.Throws<UsageException>(() => ToolArguments.Parse(["instances", .. arguments]));
	}

	[Fact]
	public void CommandsAcceptOnlyTheirOptionsAndOperands()
	{
		HashSet<string> options = ["--json"];
		ToolArguments.Parse(["instances", "--json"]).Require(options, 0, 0);
		Assert.Contains("does not take --headless",
			Assert.Throws<UsageException>(() => ToolArguments.Parse(["instances", "--headless"]).Require(options, 0, 0)).Message);
		Assert.Contains("takes no operands", Assert.Throws<UsageException>(() => ToolArguments.Parse(["instances", "x"]).Require(options, 0, 0)).Message);
		Assert.Throws<UsageException>(() => ToolArguments.Parse(["call"]).Require(options, 1, 2));
		Assert.Throws<UsageException>(() => ToolArguments.Parse(["call", "a", "b", "c"]).Require(options, 1, 2));
	}
}
