using System.Runtime.InteropServices;

namespace Strada.Interop;

// The numbers of a rebind's answer, as Input_StartRebind, Input_Rebind, Input_ResetBindings and Input_GetCurrentRebind fill
// them (ScriptBindingsRuntime.cpp declares the same layout, without implicit padding). Its text is read with
// Input_ReadRebindText.
[StructLayout(LayoutKind.Sequential)]
internal struct NativeRebindAnswer
{
	// What Status holds when no scene runs scripts: nothing was asked of any.
	public const int NoScene = -1;

	// A RebindStatus, or NoScene.
	public int Status;
	public int Binding;
	public int Part;
	public int ConflictCount;
}
