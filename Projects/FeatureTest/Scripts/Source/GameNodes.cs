using System;
using Strada;

namespace FeatureTest;

// The game's members that visual scripts use as nodes ([VisualNode]): Scripts/Features.sgraph calls every one of them, here
// and on VisualScriptTests, and the engine's coverage test fails when a graph of this project leaves one out. Each form of
// marking is here once: a whole static class, a struct with a constructor, an enum, and a generic method marked alone.

/// <summary>The rules of the feature test's little game: every public member of a marked class is a node, in its
/// category.</summary>
[VisualNode(Category = "Feature Test/Rules")]
public static class GameRules
{
	/// <summary>The most points one hit scores.</summary>
	public const int Limit = 10;

	/// <summary>The points a hit of that strength scores: twice the strength, at most Limit, none for a strength of 0 or
	/// less. A method, so its node has flow pins.</summary>
	public static int Points(int strength) => Math.Clamp(strength * 2, 0, Limit);
}

/// <summary>A medal, which graphs switch on and store like any enum.</summary>
[VisualNode]
public enum Medal
{
	None = 0,
	Bronze = 1,
	Silver = 2,
	Gold = 3,
}

/// <summary>What a run scored: its constructor, fields and property are nodes (a struct's fields are read, not set).</summary>
[VisualNode(Category = "Feature Test/Rules")]
public struct Tally
{
	public int Hits;
	public int Points;

	public Tally(int hits, int points)
	{
		Hits = hits;
		Points = points;
	}

	/// <summary>Gold from 9 points on, Silver from 6, Bronze from 3.</summary>
	public readonly Medal Medal => Points >= 9 ? Medal.Gold : Points >= 6 ? Medal.Silver : Points >= 3 ? Medal.Bronze : Medal.None;
}

/// <summary>Helpers marked one by one: the class itself is no node.</summary>
public static class GameChoices
{
	/// <summary>First or Second: a generic method, whose T graphs infer from the links or name in TypeArguments.</summary>
	[VisualNode(Pure = true, Category = "Feature Test/Rules")]
	public static T Either<T>(T first, T second, bool takeSecond) => takeSecond ? second : first;
}
