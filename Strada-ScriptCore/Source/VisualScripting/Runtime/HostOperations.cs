using System;

namespace Strada.VisualScripting;

// What an operation behind an engine entry point answers (spec section 6): its status and, exactly when that is Success,
// the text the engine receives.
internal readonly struct HostReply
{
	private HostReply(VisualScriptStatus status, byte[]? text)
	{
		Status = status;
		Text = text;
	}

	internal static HostReply NotFound { get; } = new(VisualScriptStatus.NotFound, null);

	internal VisualScriptStatus Status { get; }

	// UTF-8 JSON; null unless the status is Success.
	internal byte[]? Text { get; }

	internal static HostReply Success(byte[] text) => new(VisualScriptStatus.Success, text);
}

// The operations behind the visual-script entry points of Strada.Interop.Host (spec section 6), which only marshal their
// arguments and results; the other entry points call CreateInstance, DestroyInstance, DestroyAllInstances, Invoke and
// SetInstanceVariables directly. Spec section 5.6 for the game-assembly hooks ScriptRegistry calls.
internal sealed partial class VisualScriptRuntime
{
	// VisualScriptDescribeLibrary: the library JSON (spec section 6.1).
	internal HostReply DescribeLibrary() => HostReply.Success(LibraryWriter.Write(m_Library()));

	// VisualScriptAnalyze: the analysis JSON of any input (spec section 6.2); a document that is not a JSON object has the
	// single Error InvalidDocument. references is the bundle of the other graphs the document's nodes refer to (slice 3, spec
	// section 2.4), empty for none; no node refers to another graph yet, so no bundle is read.
	internal HostReply AnalyzeDocument(ReadOnlySpan<byte> document, ReadOnlySpan<byte> references = default) =>
		HostReply.Success(AnalysisWriter.Write(GraphAnalyzer.Analyze(m_Library(), document)));

	// VisualScriptLoad: stores a program for the handle whatever the document holds and answers its diagnostics array.
	// references as for AnalyzeDocument.
	internal HostReply LoadDocument(ulong graph, string name, ReadOnlySpan<byte> document, ReadOnlySpan<byte> references = default) =>
		HostReply.Success(AnalysisWriter.WriteDiagnostics(Load(graph, name, document)));

	// VisualScriptDescribeInstanceVariables: the stored-map JSON of the instance's public variables (spec section 5.4).
	internal HostReply DescribeVariables(ulong entity) => DescribeInstanceVariables(entity) is { } text ? HostReply.Success(text) : HostReply.NotFound;

	// Before the game assembly unloads nothing may reference its types (spec section 5.6): the instances go (no events run),
	// every program is reduced to its name and document, and the members' invokers are dropped. Graph instances block
	// assembly loads, so the engine has none then.
	internal void OnGameAssemblyUnloading()
	{
		DestroyAllInstances();
		DropPrograms();
	}

	// After a game assembly loaded, programs compiled without its types may work: every program compiles again from its
	// document when its handle is next used.
	internal void OnGameAssemblyLoaded()
	{
		DropPrograms();
	}

	private void DropPrograms()
	{
		foreach (GraphEntry entry in m_Graphs.Values)
		{
			entry.Latest = null;
			entry.Effective = null;
			entry.IsStale = true;
		}
		m_PendingSwaps.Clear();
		m_Invokers.Clear();
		m_Makers.Clear();
		m_Breakers.Clear();
	}
}
