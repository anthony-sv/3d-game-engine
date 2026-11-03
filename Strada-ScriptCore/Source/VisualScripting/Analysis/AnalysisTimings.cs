using System;
using System.Diagnostics;

namespace Strada.VisualScripting;

// The phases of an analysis and of a load (spec-slice4 section 9.0), in the order they run. The function libraries a
// document calls are analyzed inside the phase that first needs them (shaping or latency), and count there.
internal enum AnalysisPhase
{
	// The document's JSON into the document model.
	Read,
	// Variables, locals and function signatures.
	Declare,
	// A node for every document node, resolved against the node library.
	Create,
	// Latency, the pins each node's settings shape, and the splits of pins whose types are final.
	Shape,
	// The document's links attached to pins.
	Links,
	// Wildcard types resolved, late splits and the links that waited for them.
	Types,
	// Disabled nodes, link rules and unlinked targets.
	Checks,
	// Stored literals decoded with the resolved pin types.
	Decode,
	// The graph rules and the demotion of disabled nodes' findings.
	Rules,
	// The analysis JSON (VisualScriptAnalyze only).
	Write,
	// The program compiled from the analysis (loads only).
	Compile,
	// The instances moved to the new program and the receipt (loads only).
	Swap,
}

// What analyses and loads spent in each phase, in milliseconds, summed over every analysis and load that filled it since
// the last Reset (spec-slice4 section 9.0). Filled when a runtime's Timings or an analysis' AnalysisContext.Timings is set;
// main thread only, like the runtime.
internal sealed class AnalysisTimings
{
	private static readonly int s_PhaseCount = Enum.GetValues<AnalysisPhase>().Length;

	private readonly double[] m_Milliseconds = new double[s_PhaseCount];

	internal double Read => this[AnalysisPhase.Read];

	internal double Declare => this[AnalysisPhase.Declare];

	internal double Create => this[AnalysisPhase.Create];

	internal double Shape => this[AnalysisPhase.Shape];

	internal double Links => this[AnalysisPhase.Links];

	internal double Types => this[AnalysisPhase.Types];

	internal double Checks => this[AnalysisPhase.Checks];

	internal double Decode => this[AnalysisPhase.Decode];

	internal double Rules => this[AnalysisPhase.Rules];

	internal double Write => this[AnalysisPhase.Write];

	internal double Compile => this[AnalysisPhase.Compile];

	internal double Swap => this[AnalysisPhase.Swap];

	// The analysis itself: every phase from Read to Rules.
	internal double Analysis
	{
		get
		{
			double total = 0;
			for (AnalysisPhase phase = AnalysisPhase.Read; phase <= AnalysisPhase.Rules; phase++)
			{
				total += this[phase];
			}
			return total;
		}
	}

	// The analyses that filled the timings: each Read counts one.
	internal int Analyses { get; private set; }

	internal double this[AnalysisPhase phase] => m_Milliseconds[(int)phase];

	internal void Reset()
	{
		Array.Clear(m_Milliseconds);
		Analyses = 0;
	}

	internal static long Now() => Stopwatch.GetTimestamp();

	// Adds the time since start to the phase and returns now: the start of the next phase.
	internal long Lap(AnalysisPhase phase, long start)
	{
		long now = Stopwatch.GetTimestamp();
		m_Milliseconds[(int)phase] += Stopwatch.GetElapsedTime(start, now).TotalMilliseconds;
		if (phase == AnalysisPhase.Read)
		{
			Analyses++;
		}
		return now;
	}
}
