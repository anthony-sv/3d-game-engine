using System;
using System.Threading;
using Strada.Interop;

namespace Strada.VisualScripting;

// The node library of the engine's runtime (spec section 5.1): built over Strada.ScriptCore and the game's script classes
// and public types the first time it is needed, once even when several threads ask at the same time, and dropped whenever
// that set of assemblies changes (a game assembly loads or unloads, the engine initializes or shuts down), so it never
// holds the types of a game assembly that is gone. Libraries are immutable: a runtime may keep using the one it compiled
// with. Get and Drop are thread-safe; the script classes and game types a build reads are ScriptRegistry's, which only the
// main thread changes. Each build logs the notes of the game's [VisualNode] members once (spec-slice3 section 3.7.2).
internal static class EngineLibrary
{
	private static Lazy<NodeLibrary> s_Library = Create();

	internal static NodeLibrary Get() => Volatile.Read(ref s_Library).Value;

	internal static void Drop()
	{
		Interlocked.Exchange(ref s_Library, Create());
	}

	// Builds a library and reports each of its notes to warn, once.
	internal static NodeLibrary Build(NodeLibrarySources sources, Action<string> warn)
	{
		NodeLibrary library = NodeLibraryBuilder.Build(sources);
		foreach (LibraryNote note in library.Notes)
		{
			warn(note.Message);
		}
		return library;
	}

	private static Lazy<NodeLibrary> Create() => new(Build, LazyThreadSafetyMode.ExecutionAndPublication);

	private static NodeLibrary Build() =>
		Build(NodeLibrarySources.ForScriptCore(ScriptRegistry.GetScriptClasses(), ScriptRegistry.GetGameTypes()), Log.Warn);
}
