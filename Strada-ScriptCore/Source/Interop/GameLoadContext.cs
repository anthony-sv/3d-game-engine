using System.IO;
using System.Reflection;
using System.Runtime.Loader;

namespace Strada.Interop;

// Holds the game's script assembly so it can be unloaded and replaced (hot reload). Assemblies are loaded from bytes,
// leaving the files free for the next build to overwrite.
internal sealed class GameLoadContext : AssemblyLoadContext
{
	private readonly string m_Directory;

	internal GameLoadContext(string directory)
		: base("Strada.Game", isCollectible: true)
	{
		m_Directory = directory;
	}

	internal Assembly LoadFromFile(string path)
	{
		using MemoryStream assembly = new(File.ReadAllBytes(path));
		string symbolsPath = Path.ChangeExtension(path, ".pdb");
		if (!File.Exists(symbolsPath))
		{
			return LoadFromStream(assembly);
		}
		// Symbols give script exceptions file names and line numbers.
		using MemoryStream symbols = new(File.ReadAllBytes(symbolsPath));
		return LoadFromStream(assembly, symbols);
	}

	protected override Assembly? Load(AssemblyName assemblyName)
	{
		// The scripting API and the framework come from the default context, shared with the engine.
		if (assemblyName.Name == null || assemblyName.Name == typeof(Script).Assembly.GetName().Name)
		{
			return null;
		}
		// Other dependencies of the game are next to its assembly.
		string path = Path.Combine(m_Directory, assemblyName.Name + ".dll");
		return File.Exists(path) ? LoadFromFile(path) : null;
	}
}
