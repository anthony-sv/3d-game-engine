using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// "Did you mean" candidates for unknown names (spec section 6.2): the nearest by Levenshtein distance, ties in ordinal
// order. Among every name of a kind, a candidate farther away than a typo could be (half the text's length, from 2 to 24
// edits) is no suggestion, which also keeps the search cheap for long texts; among candidates that are all relevant (the
// overloads of a member), the nearest are suggested however far they are. Short names typed fast are swapped letters more
// often than anything else: searches for them may count a swap of two neighbouring letters as one edit (transpositions).
internal static class Suggestions
{
	internal const int Count = 3;

	internal static int MaxDistance(string text) => Math.Clamp(text.Length / 2, 2, 24);

	internal static IReadOnlyList<string> Nearest(string text, IEnumerable<string> candidates, bool bounded = true, bool transpositions = false)
	{
		int maxDistance = bounded ? MaxDistance(text) : int.MaxValue;
		List<(int Distance, string Candidate)> nearest = [];
		HashSet<string> seen = new(StringComparer.Ordinal);
		foreach (string candidate in candidates)
		{
			if (!seen.Add(candidate))
			{
				continue;
			}
			// The length difference bounds the distance from below.
			int limit = nearest.Count < Count ? maxDistance : nearest[^1].Distance;
			if (Math.Abs(candidate.Length - text.Length) > limit)
			{
				continue;
			}
			int distance = transpositions ? TranspositionDistance(text, candidate) : Distance(text, candidate, limit);
			if (distance > limit)
			{
				continue;
			}
			nearest.Add((distance, candidate));
			nearest.Sort(static (a, b) => a.Distance != b.Distance ? a.Distance.CompareTo(b.Distance) : string.CompareOrdinal(a.Candidate, b.Candidate));
			if (nearest.Count > Count)
			{
				nearest.RemoveAt(Count);
			}
		}
		List<string> names = new(nearest.Count);
		foreach ((_, string candidate) in nearest)
		{
			names.Add(candidate);
		}
		return names;
	}

	// The optimal string alignment distance of two strings: Levenshtein's, with a swap of two neighbouring characters as one
	// edit.
	internal static int TranspositionDistance(string a, string b)
	{
		int[,] distances = new int[a.Length + 1, b.Length + 1];
		for (int row = 0; row <= a.Length; row++)
		{
			distances[row, 0] = row;
		}
		for (int column = 0; column <= b.Length; column++)
		{
			distances[0, column] = column;
		}
		for (int row = 1; row <= a.Length; row++)
		{
			for (int column = 1; column <= b.Length; column++)
			{
				int cost = a[row - 1] == b[column - 1] ? 0 : 1;
				int value = Math.Min(distances[row - 1, column - 1] + cost, Math.Min(distances[row - 1, column] + 1, distances[row, column - 1] + 1));
				if (row > 1 && column > 1 && a[row - 1] == b[column - 2] && a[row - 2] == b[column - 1])
				{
					value = Math.Min(value, distances[row - 2, column - 2] + 1);
				}
				distances[row, column] = value;
			}
		}
		return distances[a.Length, b.Length];
	}

	// The Levenshtein distance of two strings, or some value above the limit once the distance exceeds it.
	internal static int Distance(string a, string b, int limit = int.MaxValue)
	{
		int[] previous = new int[b.Length + 1];
		int[] current = new int[b.Length + 1];
		for (int column = 0; column <= b.Length; column++)
		{
			previous[column] = column;
		}
		for (int row = 1; row <= a.Length; row++)
		{
			current[0] = row;
			int rowMinimum = row;
			for (int column = 1; column <= b.Length; column++)
			{
				int substitution = previous[column - 1] + (a[row - 1] == b[column - 1] ? 0 : 1);
				int value = Math.Min(substitution, Math.Min(previous[column] + 1, current[column - 1] + 1));
				current[column] = value;
				rowMinimum = Math.Min(rowMinimum, value);
			}
			if (rowMinimum > limit)
			{
				return rowMinimum;
			}
			(previous, current) = (current, previous);
		}
		return previous[b.Length];
	}
}
