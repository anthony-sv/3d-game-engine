using Strada.Interop;

namespace Strada;

/// <summary>The scenes of the game.</summary>
public static unsafe class SceneManager
{
	/// <summary>The name of the running scene.</summary>
	public static string CurrentSceneName
	{
		get
		{
			int length = 0;
			byte* name = InternalCalls.SceneManager_GetCurrentSceneName(&length);
			return NativeString.FromUtf8(name, length);
		}
	}

	/// <summary>Replaces the running scene with the scene at a path of the project's asset directory
	/// ("Scenes/Level2.sscene") after this frame: every script of the current scene gets OnDestroy, then the new scene
	/// starts. Returns false (logged) when there is no such scene.</summary>
	public static bool LoadScene(string path)
	{
		byte[] text = NativeString.ToUtf8(path);
		fixed (byte* bytes = text)
		{
			return InternalCalls.SceneManager_LoadScene(bytes, text.Length) != 0;
		}
	}
}
