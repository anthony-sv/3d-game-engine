using System;
using System.Text.Json.Nodes;

namespace Strada.Tool.Editor;

/// <summary>An editor command failed: the JSON-RPC error the editor answered with (Docs/Automation.md, "Errors").</summary>
internal sealed class EditorCommandException : Exception
{
	public EditorCommandException(int code, string message, JsonNode? errorData)
		: base(message)
	{
		Code = code;
		ErrorData = errorData;
	}

	public int Code { get; }
	/// <summary>The error's "data" (such as the offending parameter's path), or null.</summary>
	public JsonNode? ErrorData { get; }

	/// <summary>The name of the code in the automation reference.</summary>
	public string CodeName => Code switch
	{
		-32700 => "ParseError",
		-32600 => "InvalidRequest",
		-32601 => "MethodNotFound",
		-32602 => "InvalidParams",
		-32603 => "InternalError",
		-32001 => "Unauthenticated",
		-32002 => "ServerBusy",
		-32003 => "MessageTooLarge",
		1001 => "EntityNotFound",
		1002 => "ComponentNotFound",
		1003 => "InvalidOperation",
		1004 => "FileError",
		1005 => "Unavailable",
		1006 => "Cancelled",
		1007 => "UnsavedChanges",
		1008 => "AssetNotFound",
		_ => "Error",
	};

	/// <summary>The error from a JSON-RPC error object ({ code, message, data }).</summary>
	public static EditorCommandException FromError(JsonObject error)
	{
		int code = error["code"] is JsonValue codeValue && codeValue.TryGetValue(out int value) ? value : -32603;
		string message = error["message"] is JsonValue messageValue && messageValue.TryGetValue(out string? text) ? text : "the editor reported an error";
		return new EditorCommandException(code, message, error["data"]?.DeepClone());
	}

	/// <summary>"&lt;name&gt; (&lt;code&gt;): &lt;message&gt;", followed by the data on its own line when there is any.</summary>
	public string Describe() => ErrorData is null ? $"{CodeName} ({Code}): {Message}" : $"{CodeName} ({Code}): {Message}\n{ErrorData.ToJsonString(JsonText.Compact)}";
}

/// <summary>No editor can be used: none runs, one could not be started, or the connection to it broke.</summary>
internal sealed class EditorUnavailableException : Exception
{
	public EditorUnavailableException(string message)
		: base(message)
	{
	}

	public EditorUnavailableException(string message, Exception innerException)
		: base(message, innerException)
	{
	}
}
