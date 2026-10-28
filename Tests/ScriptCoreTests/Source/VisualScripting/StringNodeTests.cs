using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using System.Threading;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The String nodes (spec-slice3 section 3.5): ordinal, culture-invariant, in UTF-16 code units, and never aborting on their
// own data.
public sealed class StringNodeTests
{
	// What a String node of the type outputs for the stored inputs (an object of JSON literals), each output as text.
	private static List<string> Outputs(string type, object inputs, params string[] outputs)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint node = graph.Node(type, inputs: JsonSerializer.Serialize(inputs));
		uint previous = graph.Node("Event.Create");
		foreach (string output in outputs)
		{
			uint write = graph.Write(node, output);
			graph.Flow(previous, write);
			previous = write;
		}
		TestRuntime runtime = TestRuntime.Start(document);
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Empty(runtime.Environment.Logs);
		return runtime.Records;
	}

	private static string Output(string type, object inputs, string output) => Assert.Single(Outputs(type, inputs, output));

	[Theory]
	[InlineData("", "0")]
	[InlineData("héllo", "5")]
	// A character outside the Basic Multilingual Plane is two UTF-16 code units, as C#'s string.Length counts it.
	[InlineData("a\U0001F600b", "4")]
	public void LengthCountsCodeUnits(string text, string expected)
	{
		Assert.Equal(expected, Output("String.Length", new { Text = text }, "Length"));
		Assert.Equal(text.Length == 0 ? "True" : "False", Output("String.IsEmpty", new { Text = text }, "Result"));
	}

	[Theory]
	[InlineData("String.Contains", "Substring", "abc", "b", "True")]
	[InlineData("String.Contains", "Substring", "abc", "B", "False")]
	[InlineData("String.Contains", "Substring", "abc", "", "True")]
	[InlineData("String.Contains", "Substring", "", "", "True")]
	[InlineData("String.Contains", "Substring", "", "a", "False")]
	[InlineData("String.StartsWith", "Prefix", "Hello", "He", "True")]
	[InlineData("String.StartsWith", "Prefix", "Hello", "he", "False")]
	[InlineData("String.StartsWith", "Prefix", "Hello", "", "True")]
	[InlineData("String.EndsWith", "Suffix", "Hello", "lo", "True")]
	[InlineData("String.EndsWith", "Suffix", "Hello", "LO", "False")]
	[InlineData("String.EndsWith", "Suffix", "", "", "True")]
	// Ordinal: no culture folds these together.
	[InlineData("String.Contains", "Substring", "straße", "strasse", "False")]
	public void SearchesAreOrdinal(string type, string pin, string text, string other, string expected)
	{
		Dictionary<string, string> inputs = new() { ["Text"] = text, [pin] = other };
		Assert.Equal(expected, Output(type, inputs, "Result"));
	}

	[Theory]
	[InlineData("abcabc", "c", 0, "2")]
	[InlineData("abcabc", "c", 3, "5")]
	[InlineData("abcabc", "c", -5, "2")]
	[InlineData("abc", "c", 6, "-1")]
	[InlineData("abc", "x", 0, "-1")]
	[InlineData("abc", "", 1, "1")]
	// "" is found at the clamped start: Length + 3 clamps to Length.
	[InlineData("abc", "", 6, "3")]
	[InlineData("", "", 0, "0")]
	public void IndexOfClampsItsStart(string text, string substring, int start, string expected)
	{
		Assert.Equal(expected, Output("String.IndexOf", new { Text = text, Substring = substring, StartIndex = start }, "Index"));
	}

	[Theory]
	[InlineData("Hello world", 6, -1, "world")]
	[InlineData("Hello world", 0, 5, "Hello")]
	[InlineData("Hello world", 3, 100, "lo world")]
	[InlineData("Hello world", -2, 2, "He")]
	[InlineData("Hello world", 20, 3, "")]
	[InlineData("Hello world", 4, 0, "")]
	[InlineData("", 0, -1, "")]
	public void SubstringIsClampedToTheText(string text, int start, int length, string expected)
	{
		Assert.Equal(expected, Output("String.Substring", new { Text = text, Start = start, Length = length }, "Result"));
		// The defaults: from 0, the whole rest.
		Assert.Equal(text, Output("String.Substring", new { Text = text }, "Result"));
	}

	// A cut through a surrogate pair leaves half of it, as C# does (the engine reads it as U+FFFD).
	[Fact]
	public void SubstringCanCutASurrogatePair()
	{
		Assert.Equal("a\uD83D", Output("String.Substring", new { Text = "a\U0001F600b", Start = 0, Length = 2 }, "Result"));
	}

	[Theory]
	[InlineData("aaa", "a", "b", "bbb")]
	[InlineData("aaaa", "aa", "b", "bb")]
	[InlineData("abc", "B", "x", "abc")]
	[InlineData("abc", "", "x", "abc")]
	[InlineData("a.b.c", ".", "", "abc")]
	[InlineData("", "a", "b", "")]
	public void ReplaceReplacesEveryOccurrenceLeftToRight(string text, string old, string replacement, string expected)
	{
		Assert.Equal(expected, Output("String.Replace", new { Text = text, Old = old, New = replacement }, "Result"));
	}

	[Theory]
	[InlineData("String.ToUpper", "Hello i ß", "HELLO I ß")]
	[InlineData("String.ToLower", "Hello I Ä", "hello i ä")]
	[InlineData("String.Trim", " \t a b \n ", "a b")]
	[InlineData("String.Trim", "   ", "")]
	public void CaseAndTrim(string type, string text, string expected)
	{
		Assert.Equal(expected, Output(type, new { Text = text }, "Result"));
	}

	[Theory]
	[InlineData("a,b,,c", ",", false, "4", "a|b||c")]
	[InlineData("a,b,,c", ",", true, "3", "a|b|c")]
	[InlineData("a::b", "::", false, "2", "a|b")]
	[InlineData("a,b", "", false, "1", "a,b")]
	[InlineData("", ",", false, "1", "")]
	[InlineData("", ",", true, "0", "")]
	[InlineData(",", ",", true, "0", "")]
	public void SplitSeparatesAtEveryOccurrence(string text, string separator, bool removeEmpty, string count, string joined)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint split = graph.Node("String.Split", inputs: JsonSerializer.Serialize(new { Text = text, Separator = separator, RemoveEmpty = removeEmpty }));
		uint length = graph.Node("Array.Length");
		graph.Link(split, "Parts", length, "Array");
		uint join = graph.Node("String.Join", inputs: """{ "Separator": "|" }""");
		graph.Link(split, "Parts", join, "Parts");
		uint write = graph.Write(length, "Length");
		graph.Flow(graph.Node("Event.Create"), write);
		graph.Flow(write, graph.Write(join, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal([count, joined], runtime.Records);
	}

	[Fact]
	public void SplitSeparatesAtCommasByDefault()
	{
		Assert.Equal("[a, b]", Output("String.Split", new { Text = "a,b" }, "Parts"));
	}

	[Theory]
	[InlineData("int[]", "[1, 2, 3]", ", ", "1, 2, 3")]
	[InlineData("int[]", "[]", ", ", "")]
	[InlineData("float[]", "[0.1, 1.5, 3.4028235e38, 1e-7]", ";", "0.1;1.5;3.4028235E+38;1E-07")]
	[InlineData("Strada.Entity[]", """["0", "0"]""", ", ", "None, None")]
	[InlineData("string[]", """["a", "", "b"]""", "-", "a--b")]
	[InlineData("Strada.KeyCode[]", """["Space"]""", ", ", "Space")]
	public void JoinListsTheElementsAsText(string type, string array, string separator, string expected)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint join = graph.Node("String.Join", inputs: JsonSerializer.Serialize(new { Separator = separator }));
		graph.Link(graph.Literal(type, array), "Value", join, "Parts");
		graph.Flow(graph.Node("Event.Create"), graph.Write(join, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal([expected], runtime.Records);
		if (type == "float[]")
		{
			// The texts read back as the same floats.
			float[] values = [0.1f, 1.5f, 3.4028235e38f, 1e-7f];
			Assert.Equal(values, expected.Split(';').Select(static text => float.Parse(text, CultureInfo.InvariantCulture)));
		}
	}

	[Theory]
	[InlineData(" 42 ", "42", "True")]
	[InlineData("-7", "-7", "True")]
	[InlineData("+5", "5", "True")]
	[InlineData("4.5", "0", "False")]
	[InlineData("99999999999", "0", "False")]
	[InlineData("2147483647", "2147483647", "True")]
	[InlineData("", "0", "False")]
	[InlineData("1,000", "0", "False")]
	[InlineData("0x10", "0", "False")]
	public void ToIntReadsIntegers(string text, string value, string success)
	{
		Assert.Equal([value, success], Outputs("String.ToInt", new { Text = text }, "Value", "Success"));
	}

	[Theory]
	[InlineData("1.5", "1.5", "True")]
	[InlineData(" -2e3 ", "-2000", "True")]
	[InlineData(".5", "0.5", "True")]
	[InlineData("1e40", "0", "False")]
	[InlineData("NaN", "0", "False")]
	[InlineData("Infinity", "0", "False")]
	[InlineData("1,5", "0", "False")]
	[InlineData("x", "0", "False")]
	[InlineData("", "0", "False")]
	public void ToFloatReadsFiniteNumbers(string text, string value, string success)
	{
		Assert.Equal([value, success], Outputs("String.ToFloat", new { Text = text }, "Value", "Success"));
	}

	// Random strings and indices through every String node: none ever aborts.
	[Fact]
	public void StringNodesNeverAbort()
	{
		DocumentBuilder document = new();
		uint text = document.Variable("Text", "string");
		uint other = document.Variable("Other", "string");
		uint number = document.Variable("Number", "int");
		uint flag = document.Variable("Flag", "bool");
		GraphBuilder graph = document.Graph;
		uint previous = graph.Node("Event.Update");
		string[] types =
		[
			"String.Length", "String.IsEmpty", "String.Contains", "String.StartsWith", "String.EndsWith", "String.IndexOf", "String.Substring",
			"String.Replace", "String.ToUpper", "String.ToLower", "String.Trim", "String.Split", "String.ToInt", "String.ToFloat",
		];
		Dictionary<string, uint> sources = new()
		{
			["Text"] = text,
			["Substring"] = other,
			["Prefix"] = other,
			["Suffix"] = other,
			["Old"] = other,
			["New"] = text,
			["Separator"] = other,
			["StartIndex"] = number,
			["Start"] = number,
			["Length"] = number,
			["RemoveEmpty"] = flag,
		};
		foreach (string type in types)
		{
			uint node = graph.Node(type);
			foreach (PinDescription pin in VisualScriptTesting.LibraryNode(type).Pins)
			{
				if (pin.IsInput)
				{
					graph.Link(graph.Get(sources[pin.Name]), "Value", node, pin.Name);
				}
			}
			string output = VisualScriptTesting.LibraryNode(type).Pins.First(static pin => !pin.IsInput).Name;
			uint join = graph.Node("FormatString", """{ "Format": "{Value}" }""");
			graph.Link(node, output, join, "Value");
			uint write = graph.Write(join, "Result");
			graph.Flow(previous, write);
			previous = write;
		}
		uint joined = graph.Node("String.Join");
		uint split = graph.Node("String.Split");
		graph.Link(graph.Get(text), "Value", split, "Text");
		graph.Link(graph.Get(other), "Value", split, "Separator");
		graph.Link(split, "Parts", joined, "Parts");
		graph.Link(graph.Get(other), "Value", joined, "Separator");
		graph.Flow(previous, graph.Write(joined, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		FuzzRandom random = new(0x535452494E4753u);
		string[] pieces = ["", "a", "ab", ",", " ", "\t", "ß", "İ", "\uD83D", "\uDE00", "\U0001F600", "\0", "1", "-", "e", ".", "NaN"];
		int rounds = Fuzzing.GetRounds(400);
		for (int round = 0; round < rounds; round++)
		{
			string Random()
			{
				int count = random.Pick(6);
				return string.Concat(Enumerable.Range(0, count).Select(_ => pieces[random.Pick(pieces.Length)]));
			}
			runtime.Runtime.SetVariable(TestRuntime.Hero, "Text", typeof(string), Random());
			runtime.Runtime.SetVariable(TestRuntime.Hero, "Other", typeof(string), Random());
			int[] numbers = [int.MinValue, -5, -1, 0, 1, 2, 3, 7, int.MaxValue];
			runtime.Runtime.SetVariable(TestRuntime.Hero, "Number", typeof(int), numbers[random.Pick(numbers.Length)]);
			runtime.Runtime.SetVariable(TestRuntime.Hero, "Flag", typeof(bool), random.Pick(2) == 0);
			Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update));
		}
		Assert.Empty(runtime.Environment.Errors);
	}

	// Parsing and case mapping are the invariant culture's, whatever the thread's culture.
	[Theory]
	[InlineData("tr-TR")]
	[InlineData("de-DE")]
	public void ParsingIgnoresTheCulture(string name)
	{
		CultureInfo culture = Thread.CurrentThread.CurrentCulture;
		try
		{
			Thread.CurrentThread.CurrentCulture = new CultureInfo(name);
			Assert.Equal(["1.5", "True"], Outputs("String.ToFloat", new { Text = "1.5" }, "Value", "Success"));
			Assert.Equal(["0", "False"], Outputs("String.ToFloat", new { Text = "1,5" }, "Value", "Success"));
			Assert.Equal(["-3", "True"], Outputs("String.ToInt", new { Text = "-3" }, "Value", "Success"));
			Assert.Equal("TITLE", Output("String.ToUpper", new { Text = "title" }, "Result"));
			Assert.Equal("title", Output("String.ToLower", new { Text = "TITLE" }, "Result"));
			Assert.Equal("1.5, 2.25", JoinFloats());
		}
		finally
		{
			Thread.CurrentThread.CurrentCulture = culture;
		}
	}

	private static string JoinFloats()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint join = graph.Node("String.Join");
		graph.Link(graph.Literal("float[]", "[1.5, 2.25]"), "Value", join, "Parts");
		graph.Flow(graph.Node("Event.Create"), graph.Write(join, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		return Assert.Single(runtime.Records);
	}
}
