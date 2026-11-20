using System.Runtime.InteropServices;

namespace Strada.Interop;

// The player's window as Settings_GetWindow and Settings_SetWindow pass it (ScriptBindingsSaves.cpp declares the same
// layout, without implicit padding): a WindowMode, the windowed size and vsync (0 or 1), so that a mode and a size change
// together.
[StructLayout(LayoutKind.Sequential)]
internal struct NativePlayerWindow
{
	public int Mode;
	public int Width;
	public int Height;
	public int VSync;
}
