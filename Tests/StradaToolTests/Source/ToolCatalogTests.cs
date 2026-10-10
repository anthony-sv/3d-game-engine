using System.Linq;
using System.Text.Json.Nodes;
using Strada.Tool.Editor;
using Strada.Tool.Mcp;
using Xunit;

namespace Strada.Tool.Tests;

public sealed class ToolCatalogTests
{
	[Theory]
	[InlineData("entity.create", "entity_create")]
	[InlineData("asset.create-folder", "asset_create_folder")]
	[InlineData("editor.status", "editor_status")]
	public void ToolNamesAreCommandNamesWithUnderscores(string command, string tool)
	{
		Assert.Equal(tool, ToolCatalog.ToToolName(command));
	}

	[Fact]
	public void EntriesThatAreNotCommandsAndDuplicateNamesAreLeftOut()
	{
		JsonArray commands =
		[
			new JsonObject { ["name"] = "entity.create", ["description"] = "Creates" },
			new JsonObject { ["name"] = "entity_create", ["description"] = "Same tool name" },
			new JsonObject { ["description"] = "No name" },
			new JsonObject { ["name"] = "" },
			"not a command",
			null,
			new JsonObject { ["name"] = "scene.dump", ["readOnly"] = true, ["params"] = new JsonObject { ["type"] = "string" } },
		];
		ToolCatalog catalog = ToolCatalog.FromCommands(commands);

		Assert.Equal(["entity_create", "scene_dump"], catalog.Tools.Select(tool => tool.Name));
		Assert.True(catalog.TryGetTool("scene_dump", out McpTool dump));
		Assert.Equal("scene.dump", dump.Command);
		Assert.True(dump.ReadOnly);
		Assert.Equal("Editor command scene.dump", dump.Description);
		// MCP input schemas describe objects.
		Assert.Equal("object", dump.InputSchema["type"]!.GetValue<string>());
		Assert.False(catalog.TryGetTool("scene.dump", out _));
	}

	[Fact]
	public void ResultsAreTextAndImagesAreImageContent()
	{
		JsonObject text = ToolCatalog.ToCallResult(new JsonObject { ["id"] = "7", ["name"] = "Crate <A> & 'B'" });
		Assert.False(text["isError"]!.GetValue<bool>());
		Assert.Equal("{\"id\":\"7\",\"name\":\"Crate <A> & 'B'\"}", text["content"]![0]!["text"]!.GetValue<string>());
		Assert.Equal("null", ToolCatalog.ToCallResult(null)["content"]![0]!["text"]!.GetValue<string>());

		JsonObject image = ToolCatalog.ToCallResult(new JsonObject { ["mimeType"] = "image/png", ["width"] = 4, ["data"] = "AAAA" });
		JsonArray content = image["content"]!.AsArray();
		Assert.Equal(2, content.Count);
		Assert.Equal("image", content[0]!["type"]!.GetValue<string>());
		Assert.Equal("AAAA", content[0]!["data"]!.GetValue<string>());
		Assert.Equal("{\"mimeType\":\"image/png\",\"width\":4}", content[1]!["text"]!.GetValue<string>());

		// Only images become image content.
		JsonObject document = ToolCatalog.ToCallResult(new JsonObject { ["mimeType"] = "application/json", ["data"] = "{}" });
		Assert.Single(document["content"]!.AsArray());
	}

	[Fact]
	public void FailuresAreToolErrors()
	{
		JsonObject command = ToolCatalog.ToErrorResult(new EditorCommandException(1007, "the scene has unsaved changes", null));
		Assert.True(command["isError"]!.GetValue<bool>());
		Assert.Equal("UnsavedChanges (1007): the scene has unsaved changes", command["content"]![0]!["text"]!.GetValue<string>());
		JsonObject unavailable = ToolCatalog.ToErrorResult(new EditorUnavailableException("no StradaEditor found"));
		Assert.Equal("No editor is available: no StradaEditor found", unavailable["content"]![0]!["text"]!.GetValue<string>());
	}
}
