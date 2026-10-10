using Strada.Interop;

namespace Strada;

/// <summary>The application running the game: the exported game's player, or the editor's play mode.</summary>
public static unsafe class Application
{
	/// <summary>Whether the scene runs in the editor's play mode (false in exported games).</summary>
	public static bool IsEditor => InternalCalls.Application_IsEditor() != 0;

	/// <summary>The width in pixels of what the scene renders to: the game's window, or the editor's viewport.</summary>
	public static int WindowWidth
	{
		get
		{
			uint width = 0;
			uint height = 0;
			InternalCalls.Application_GetWindowSize(&width, &height);
			return (int)width;
		}
	}

	/// <summary>The height in pixels of what the scene renders to: the game's window, or the editor's viewport.</summary>
	public static int WindowHeight
	{
		get
		{
			uint width = 0;
			uint height = 0;
			InternalCalls.Application_GetWindowSize(&width, &height);
			return (int)height;
		}
	}

	/// <summary>Ends the game after this frame (the editor leaves play mode).</summary>
	public static void Quit()
	{
		InternalCalls.Application_Quit();
	}
}
