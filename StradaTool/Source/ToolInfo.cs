using System.Reflection;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace Strada.Tool;

/// <summary>What strada reports about itself.</summary>
internal static class ToolInfo
{
	/// <summary>strada's version, the engine's (without build metadata).</summary>
	public static string Version { get; } = ReadVersion();

	private static string ReadVersion()
	{
		string? version = typeof(ToolInfo).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
		if (version is null)
		{
			return "0.0.0";
		}
		int metadata = version.IndexOf('+');
		return metadata < 0 ? version : version[..metadata];
	}
}

/// <summary>How strada writes JSON: only the escaping JSON requires (these are not HTML pages).</summary>
internal static class JsonText
{
	public static JsonSerializerOptions Compact { get; } = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

	public static JsonSerializerOptions Indented { get; } = new()
	{
		Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
		WriteIndented = true,
	};
}
