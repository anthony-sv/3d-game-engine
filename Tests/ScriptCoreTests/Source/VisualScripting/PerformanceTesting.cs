using System;
using System.Diagnostics;
using System.Reflection;
using Strada.VisualScripting;

namespace Strada.ScriptCore.Tests;

// What the timed tests share: whether the build measures what matters (optimized, with STRADA_PERFORMANCE_TESTS=1), and the
// statistics they report.
internal static class PerformanceTesting
{
	// An optimized build of the VM: only its figures are the ones the bounds of spec-slice4 section 21 are about.
	internal static bool IsOptimized { get; } = typeof(VisualScriptRuntime).Assembly.GetCustomAttribute<DebuggableAttribute>()?.IsJITOptimizerDisabled != true;

	// Whether the run asks for figures (STRADA_PERFORMANCE_TESTS=1).
	internal static bool IsRequested => System.Environment.GetEnvironmentVariable("STRADA_PERFORMANCE_TESTS") == "1";

	// Whether wall-clock bounds are asserted: an optimized build that asks for figures.
	internal static bool IsMeasured => IsOptimized && IsRequested;

	internal static double Median(double[] values) => Percentile(values, 0.5);

	// The value at the fraction of the sorted values (linear between the two nearest); the values are not changed.
	internal static double Percentile(double[] values, double fraction)
	{
		if (values.Length == 0)
		{
			return double.NaN;
		}
		double[] sorted = (double[])values.Clone();
		Array.Sort(sorted);
		double position = fraction * (sorted.Length - 1);
		int below = (int)Math.Floor(position);
		int above = Math.Min(below + 1, sorted.Length - 1);
		return sorted[below] + ((sorted[above] - sorted[below]) * (position - below));
	}
}
