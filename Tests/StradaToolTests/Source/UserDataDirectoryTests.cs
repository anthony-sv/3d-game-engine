using System;
using System.Collections.Generic;
using System.IO;
using Strada.Tool.Editor;
using Xunit;

namespace Strada.Tool.Tests;

/// <summary>The user data directory strada looks for editors in, which must be the one the engine computes
/// (UserDataLocations::FindRoot, tested on every platform's rules in StradaTests): the rules of the platform the tests run
/// on.</summary>
public sealed class UserDataDirectoryTests
{
	private static string? Get(Dictionary<string, string> environment) =>
		UserDataDirectory.Get(name => environment.TryGetValue(name, out string? value) ? value : null);

	[Fact]
	public void AbsoluteVariablesNameTheDirectory()
	{
		if (OperatingSystem.IsWindows())
		{
			Assert.Equal(Path.Combine(@"C:\Users\ana\AppData\Roaming", "Strada"),
				Get(new() { ["APPDATA"] = @"C:\Users\ana\AppData\Roaming", ["USERPROFILE"] = @"C:\Users\bo" }));
			Assert.Equal(Path.Combine("C:/Users/ana", "AppData", "Roaming", "Strada"), Get(new() { ["USERPROFILE"] = "C:/Users/ana" }));
			Assert.Equal(Path.Combine(@"\\server\profiles\ana", "Strada"), Get(new() { ["APPDATA"] = @"\\server\profiles\ana" }));
			Assert.Null(Get(new() { ["HOME"] = "C:/Users/ana" }));
		}
		else if (OperatingSystem.IsMacOS())
		{
			Assert.Equal(Path.Combine("/Users/ana", "Library", "Application Support", "Strada"), Get(new() { ["HOME"] = "/Users/ana" }));
			Assert.Null(Get(new() { ["XDG_DATA_HOME"] = "/data" }));
		}
		else
		{
			Assert.Equal(Path.Combine("/data/ana", "strada"), Get(new() { ["XDG_DATA_HOME"] = "/data/ana", ["HOME"] = "/home/ana" }));
			Assert.Equal(Path.Combine("/home/ana", ".local", "share", "strada"), Get(new() { ["HOME"] = "/home/ana" }));
		}
		Assert.Null(Get(new()));
	}

	[Theory]
	[InlineData("")]
	[InlineData("Roaming")]
	[InlineData("./data")]
	[InlineData("C:Roaming")]
	[InlineData("\\Roaming")]
	public void EmptyAndRelativeVariablesCountAsUnset(string value)
	{
		// The engine never puts its folder relative to the working directory: such a value falls through to the next
		// variable, as it does in FindRoot, and strada looks where the editor writes.
		if (OperatingSystem.IsWindows())
		{
			Assert.Equal(Path.Combine("C:/Users/ana", "AppData", "Roaming", "Strada"),
				Get(new() { ["APPDATA"] = value, ["USERPROFILE"] = "C:/Users/ana" }));
			Assert.Null(Get(new() { ["APPDATA"] = value }));
			Assert.Null(Get(new() { ["APPDATA"] = value, ["USERPROFILE"] = value }));
		}
		else if (OperatingSystem.IsMacOS())
		{
			Assert.Null(Get(new() { ["HOME"] = value }));
		}
		else
		{
			Assert.Equal(Path.Combine("/home/ana", ".local", "share", "strada"),
				Get(new() { ["XDG_DATA_HOME"] = value, ["HOME"] = "/home/ana" }));
			Assert.Null(Get(new() { ["XDG_DATA_HOME"] = value }));
			Assert.Null(Get(new() { ["XDG_DATA_HOME"] = value, ["HOME"] = value }));
		}
		Assert.Null(UserDataDirectory.GetInstanceDirectory(name => name is "APPDATA" or "USERPROFILE" or "HOME" or "XDG_DATA_HOME" ? value : null));
	}

	[Fact]
	public void AbsolutePathsFollowThePlatformsRules()
	{
		Assert.True(UserDataDirectory.IsAbsolute(OperatingSystem.IsWindows() ? "C:/" : "/"));
		Assert.False(UserDataDirectory.IsAbsolute("data"));
		Assert.False(UserDataDirectory.IsAbsolute(""));
		if (OperatingSystem.IsWindows())
		{
			Assert.True(UserDataDirectory.IsAbsolute(@"C:\Users"));
			Assert.True(UserDataDirectory.IsAbsolute("//server/share"));
			Assert.False(UserDataDirectory.IsAbsolute("/home/ana"));
			Assert.False(UserDataDirectory.IsAbsolute("C:"));
			Assert.False(UserDataDirectory.IsAbsolute(@"\\\server"));
		}
		else
		{
			Assert.True(UserDataDirectory.IsAbsolute("/home/ana"));
			Assert.False(UserDataDirectory.IsAbsolute(@"C:\Users"));
		}
	}
}
