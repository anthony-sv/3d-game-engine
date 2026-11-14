#pragma once

#include "Strada/Core/Profiler.h"
#include "Strada/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strada
{
	// Who records in the editor, and the capture its Profiler panel shows (Docs/Architecture.md §4.4). The profiler records
	// only while the Profiler panel is open or a recording that profiler.start began runs, so an editor that does neither
	// pays the idle cost alone. Main thread only; makes no ImGui call, so automation and tests use it headless.
	class EditorProfiler
	{
	public:
		// The settings the Profiler panel records with (the recorder's defaults).
		static ProfilerSettings GetPanelSettings() { return {}; }

		// profiler.start: starts a recording with the settings, clearing the history. A recording in progress (the panel's or
		// an earlier profiler.start's) is stopped first and restarted with these settings. The recording then runs until
		// Stop, whether or not the panel is open. Fails when the profiler is not initialized, in Dist builds, or for settings
		// out of bounds (Profiler::Start's error).
		[[nodiscard]] Result<void> Start(ProfilerSettings const& settings);
		// profiler.stop and the panel's Pause: ends the recording in progress, whoever began it, and keeps its history. The
		// open panel then does not record again until Resume, or until it is closed and opened again. Returns whether a
		// recording was in progress.
		bool Stop();
		// The panel's Record: lets the open panel record again from its next Update.
		void Resume();

		// Once per editor frame, before the panels draw: panelOpen says whether the Profiler panel is open and drawn. While
		// it is (and not held by Stop), a recording runs, started with GetPanelSettings when none does; when it closes, the
		// recording it started stops (one profiler.start began goes on).
		void Update(bool panelOpen);

		// Whether a recording that profiler.start began is in progress, and whether one the open panel began is.
		bool IsCommandRecording() const;
		bool IsPanelRecording() const;
		// Whether Stop holds the open panel from recording.
		bool IsHeld() const { return m_Held; }
		// Why the panel's last attempt to record failed (empty when it did not fail).
		std::string const& GetRecordingError() const { return m_RecordingError; }

		// profiler.open and the panel's Open Capture: reads a capture file (ProfilerCaptureSerializer::Load) and shows it in
		// place of the live history, replacing a capture opened before. A capture that does not read leaves the one shown.
		[[nodiscard]] Result<void> Open(std::filesystem::path const& path);
		// Shows a capture read elsewhere, named by name (what the panel shows of it: a file name, never a path).
		void SetCapture(ProfilerCapture capture, std::string name);
		// Back to the live history.
		void CloseCapture();
		// The capture shown in place of the live history; null when there is none.
		ProfilerCapture const* GetCapture() const { return m_Capture ? &*m_Capture : nullptr; }
		// Its name (the file name it was opened from), and the path it was read from (empty for SetCapture).
		std::string const& GetCaptureName() const { return m_CaptureName; }
		std::filesystem::path const& GetCapturePath() const { return m_CapturePath; }
		// Changes whenever a capture is opened or closed, so views can tell the capture changed.
		uint64_t GetCaptureRevision() const { return m_CaptureRevision; }

	private:
		// Who began the recording in progress. The editor starts and stops the profiler only through this object.
		enum class Owner : uint8_t
		{
			None = 0,
			Panel,
			Command
		};

		// The owner of the recording in progress: None once the recorder no longer records.
		Owner GetOwner() const;

		Owner m_Owner = Owner::None;
		bool m_Held = false;
		bool m_PanelWasOpen = false;
		std::string m_RecordingError;
		std::optional<ProfilerCapture> m_Capture;
		std::string m_CaptureName;
		std::filesystem::path m_CapturePath;
		uint64_t m_CaptureRevision = 0;
	};

	// One zone of a frame placed in its thread's tree, in pre-order: a zone before the zones nested in it, siblings in the
	// order they began.
	struct ProfilerTreeRow
	{
		// Index into ProfilerFrame::Zones.
		uint32_t Zone = 0;
		// Depth in the tree (0 for a zone at its thread's root), which is the recorded depth unless the zone's parent was
		// dropped or collected by an earlier frame.
		uint32_t Depth = 0;
		// One past the last row of its subtree: the row of its next sibling, or of the next one up.
		uint32_t SubtreeEnd = 0;
		// The zone's duration less the durations of the zones directly nested in it (never negative).
		int64_t SelfNanoseconds = 0;
	};

	// A thread's zones in one frame as a tree.
	struct ProfilerThreadTree
	{
		// Index into ProfilerCapture::Threads.
		uint32_t Thread = 0;
		// The summed durations of the zones at its root: the thread's busy time in the frame.
		int64_t BusyNanoseconds = 0;
		std::vector<ProfilerTreeRow> Rows;
	};

	// The frame's zones as one tree per thread that recorded in it, in the order of the capture's threads. Each zone is
	// placed under the nearest zone before it on its thread with a smaller recorded depth, or at the thread's root (the rule
	// ProfilerZone states). Pure; any thread.
	[[nodiscard]] std::vector<ProfilerThreadTree> BuildProfilerFrameTree(ProfilerFrame const& frame);

	// The frame of a capture with the given index (frames have consecutive indices), or null when the capture does not hold
	// it.
	[[nodiscard]] ProfilerFrame const* FindProfilerFrame(ProfilerCapture const& capture, uint64_t index);

	// A capture's name or a thread's, or "?" for an index the table does not hold (ProfilerNoName included).
	[[nodiscard]] std::string_view GetProfilerName(ProfilerCapture const& capture, uint32_t name);
}
