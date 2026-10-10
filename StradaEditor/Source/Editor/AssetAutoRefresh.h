#pragma once

namespace Strada
{
	class EditorOperations;

	// Rescans the open project's assets when the editor gets the focus back, so files that other applications created,
	// changed or deleted meanwhile (an image editor, a modeling tool, a file manager) are registered, reloaded or marked
	// missing without pressing Refresh. Main thread only.
	class AssetAutoRefresh
	{
	public:
		// Call once per frame with whether one of the editor's windows has the focus and whether the asset directory may be
		// rescanned now (not while a game export copies it); a scan that has to wait happens as soon as it may. Returns
		// whether it rescanned.
		bool Update(bool editorFocused, bool canScan, EditorOperations& operations);

	private:
		// The editor starts focused: its project was scanned when it was opened.
		bool m_Focused = true;
		bool m_ScanPending = false;
	};
}
