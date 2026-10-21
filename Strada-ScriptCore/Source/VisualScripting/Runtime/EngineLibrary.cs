using System;
using System.Threading;
using Strada.Interop;

namespace Strada.VisualScripting;

// The node library of the engine's runtime (spec section 5.1): built over Strada.ScriptCore and the game's script classes
// the first time it is needed, once even when several threads ask at the same time, and dropped whenever that set of
// assemblies changes (a game assembly loads or unloads, the engine initializes or shuts down), so it never holds the types
// of a game assembly that is gone. Libraries are immutable: a runtime may keep using the one it compiled with. Get and Drop
// are thread-safe; the script classes a build reads are ScriptRegistry's, which only the main thread changes.
internal static class EngineLibrary
{
	private static Lazy<NodeLibrary> s_Library = Create();

	internal static NodeLibrary Get() => Volatile.Read(ref s_Library).Value;

	internal static void Drop()
	{
		Interlocked.Exchange(ref s_Library, Create());
	}

	private static Lazy<NodeLibrary> Create() => new(Build, LazyThreadSafetyMode.ExecutionAndPublication);

	private static NodeLibrary Build() => NodeLibraryBuilder.Build(NodeLibrarySources.ForScriptCore(ScriptRegistry.GetScriptClasses()));
}
