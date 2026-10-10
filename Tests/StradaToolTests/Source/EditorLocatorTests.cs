using System;
using System.Collections.Generic;
using System.IO;
using Strada.Tool.Editor;
using Xunit;

namespace Strada.Tool.Tests;

public sealed class EditorLocatorTests
{
	private static readonly Func<string, string?> s_NoVariables = _ => null;

	private static string CreateFile(params string[] parts)
	{
		string path = Path.Combine(parts);
		Directory.CreateDirectory(Path.GetDirectoryName(path)!);
		File.WriteAllText(path, "");
		return path;
	}

	[Fact]
	public void TheGivenEditorComesFirst()
	{
		using TemporaryDirectory directory = new();
		string given = CreateFile(directory.Path, "Given", EditorLocator.ExecutableName);
		string variable = CreateFile(directory.Path, "Variable", EditorLocator.ExecutableName);
		Dictionary<string, string> environment = new() { [EditorLocator.EnvironmentVariable] = variable };

		Assert.Equal(given, EditorLocator.Find(given, name => environment.GetValueOrDefault(name), directory.Path, directory.Path));
		Assert.Equal(variable, EditorLocator.Find(null, name => environment.GetValueOrDefault(name), directory.Path, directory.Path));

		string missing = Path.Combine(directory.Path, "Missing", EditorLocator.ExecutableName);
		Assert.Contains("--editor", Assert.Throws<EditorUnavailableException>(() => EditorLocator.Find(missing, s_NoVariables, directory.Path, directory.Path)).Message);
		environment[EditorLocator.EnvironmentVariable] = missing;
		Assert.Contains(EditorLocator.EnvironmentVariable,
			Assert.Throws<EditorUnavailableException>(() => EditorLocator.Find(null, name => environment.GetValueOrDefault(name), directory.Path, directory.Path)).Message);
	}

	[Fact]
	public void TheEditorNextToStradaComesBeforeBuilds()
	{
		using TemporaryDirectory directory = new();
		string tool = Path.Combine(directory.Path, "bin");
		string sibling = CreateFile(tool, EditorLocator.ExecutableName);
		CreateFile(directory.Path, "build", "debug", "bin", EditorLocator.ExecutableName);

		Assert.Equal(sibling, EditorLocator.Find(null, s_NoVariables, tool, directory.Path));
	}

	[Fact]
	public void TheNewestBuildIsUsedInACheckout()
	{
		using TemporaryDirectory directory = new();
		string older = CreateFile(directory.Path, "build", "release", "bin", EditorLocator.ExecutableName);
		string newer = CreateFile(directory.Path, "build", "debug", "bin", EditorLocator.ExecutableName);
		File.SetLastWriteTimeUtc(older, DateTime.UtcNow.AddHours(-1));
		CreateFile(directory.Path, "build", "empty", "bin", "other.txt");
		string tool = Path.Combine(directory.Path, "artifacts");

		Assert.Equal(newer, EditorLocator.Find(null, s_NoVariables, tool, directory.Path));
		using TemporaryDirectory empty = new();
		EditorUnavailableException none = Assert.Throws<EditorUnavailableException>(() => EditorLocator.Find(null, s_NoVariables, tool, empty.Path));
		Assert.Contains("Tools/build.py", none.Message);
	}
}
