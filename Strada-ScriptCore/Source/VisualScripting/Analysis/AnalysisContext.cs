using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Strada.VisualScripting;

// The state one analysis shares between its passes: the library, the document's declarations and the diagnostics.
internal sealed class AnalysisContext
{
	// Suggestion searches per analysis: a document full of unknown names stays quick to analyze.
	internal const int MaxSuggestionSearches = 256;

	private readonly Dictionary<uint, AnalyzedVariable> m_VariablesById = [];
	private readonly Dictionary<uint, AnalyzedFunction> m_FunctionsById = [];
	private readonly List<AnalyzedVariable> m_Variables = [];
	private readonly List<AnalyzedFunction> m_Functions = [];
	private int m_SuggestionSearches;

	internal AnalysisContext(NodeLibrary library, GraphDocument document, List<Diagnostic> diagnostics)
	{
		Library = library;
		Document = document;
		Diagnostics = diagnostics;
	}

	internal NodeLibrary Library { get; }

	internal GraphDocument Document { get; }

	internal List<Diagnostic> Diagnostics { get; }

	internal IReadOnlyList<AnalyzedVariable> Variables => m_Variables;

	internal IReadOnlyList<AnalyzedFunction> Functions => m_Functions;

	internal void Add(AnalyzedVariable variable)
	{
		m_Variables.Add(variable);
		m_VariablesById[variable.ID] = variable;
	}

	internal void Add(AnalyzedFunction function)
	{
		m_Functions.Add(function);
		m_FunctionsById[function.ID] = function;
	}

	// A document variable, or a local of the function (IDs are unique across the whole document).
	internal AnalyzedVariable? FindVariable(uint id, AnalyzedFunction? function)
	{
		if (function != null)
		{
			foreach (AnalyzedVariable local in function.Locals)
			{
				if (local.ID == id)
				{
					return local;
				}
			}
		}
		return m_VariablesById.TryGetValue(id, out AnalyzedVariable? variable) ? variable : null;
	}

	internal AnalyzedFunction? FindFunction(uint id) => m_FunctionsById.TryGetValue(id, out AnalyzedFunction? function) ? function : null;

	// The function a local belongs to, when the ID names a local.
	internal AnalyzedFunction? FindLocalOwner(uint id)
	{
		foreach (AnalyzedFunction function in m_Functions)
		{
			foreach (AnalyzedVariable local in function.Locals)
			{
				if (local.ID == id)
				{
					return function;
				}
			}
		}
		return null;
	}

	// The nearest candidates for an unknown name (none once the analysis made MaxSuggestionSearches searches).
	internal IReadOnlyList<string> Suggest(string text, IEnumerable<string> candidates, bool bounded = true, bool transpositions = false)
	{
		if (m_SuggestionSearches >= MaxSuggestionSearches)
		{
			return [];
		}
		m_SuggestionSearches++;
		return Suggestions.Nearest(text, candidates, bounded, transpositions);
	}

	internal void ReportNode(DiagnosticCode code, AnalyzedNode node, string message, IReadOnlyList<string>? suggestions = null)
	{
		Diagnostics.Add(new Diagnostic(code, node.Graph.Function, message) { Node = node.ID, Suggestions = suggestions ?? [] });
	}

	internal void ReportPin(DiagnosticCode code, AnalyzedNode node, string pin, PinDirection direction, string message,
		DocumentLink? link = null, IReadOnlyList<string>? suggestions = null)
	{
		Diagnostics.Add(new Diagnostic(code, node.Graph.Function, message)
		{
			Node = node.ID,
			Pin = pin,
			Direction = direction,
			Link = link,
			Suggestions = suggestions ?? [],
		});
	}

	// A finding about a node's setting: span is the part of the setting's string it names, in UTF-16 indices of text, which
	// the diagnostic holds as UTF-8 byte offsets.
	internal void ReportSetting(DiagnosticCode code, AnalyzedNode node, string setting, string text, (int Start, int End)? span, string message,
		IReadOnlyList<string>? suggestions = null)
	{
		Diagnostics.Add(new Diagnostic(code, node.Graph.Function, message)
		{
			Node = node.ID,
			Setting = setting,
			Span = span is { } range ? (ByteOffset(text, range.Start), ByteOffset(text, range.End)) : null,
			Suggestions = suggestions ?? [],
		});
	}

	// The UTF-8 byte offset of a UTF-16 index of a text.
	internal static int ByteOffset(string text, int index) => Encoding.UTF8.GetByteCount(text.AsSpan(0, index));

	internal void ReportLink(DiagnosticCode code, AnalyzedLink link, string message)
	{
		Diagnostics.Add(new Diagnostic(code, link.To.Node.Graph.Function, message) { Link = link.Document });
	}

	internal void ReportVariable(DiagnosticCode code, uint function, uint variable, string message, IReadOnlyList<string>? suggestions = null)
	{
		Diagnostics.Add(new Diagnostic(code, function, message) { Variable = variable, Suggestions = suggestions ?? [] });
	}

	internal void ReportFunction(DiagnosticCode code, uint function, string message, IReadOnlyList<string>? suggestions = null)
	{
		Diagnostics.Add(new Diagnostic(code, function, message) { Suggestions = suggestions ?? [] });
	}

	internal static string Format(long value) => value.ToString(CultureInfo.InvariantCulture);
}
