#pragma once

#include "Strada/Core/Result.h"
#include "Strada/Core/Telemetry.h"

#include <cstdint>
#include <string>

namespace Strada
{
	// Who records telemetry in the editor (Docs/Architecture.md §4.5, "When it records"), as EditorProfiler is for the
	// profiler: the recorder runs only while the Telemetry panel is open (the start screen counts as closed) or a recording
	// telemetry.start began runs, which outlives the panel until telemetry.stop. The editor starts and stops the recorder
	// only through this object; play mode ends the live session when its game stops (PlayMode::Stop). Main thread only;
	// makes no ImGui call, so automation and tests use it headless.
	class EditorTelemetry
	{
	public:
		// The settings the Telemetry panel records with (the recorder's defaults).
		static TelemetrySettings GetPanelSettings() { return {}; }

		// telemetry.start: starts a recording with the settings. A recording in progress (the panel's or an earlier
		// telemetry.start's) is stopped first and a new one started, which keeps the sessions held (Telemetry::Start). The
		// recording then runs until Stop, whether or not the panel is open. Fails when the recorder is not initialized and
		// for settings out of bounds (Telemetry::CheckSettings), leaving a recording in progress as it was.
		[[nodiscard]] Result<void> Start(TelemetrySettings const& settings);
		// telemetry.stop and the panel's Pause: ends the recording in progress, whoever began it; its sessions stay. The open
		// panel then does not record again until Resume, or until it is closed and opened again. Returns whether a recording
		// was in progress.
		bool Stop();
		// The panel's Record: lets the open panel record again from its next Update.
		void Resume();

		// Once per editor frame, before the panels draw: panelOpen says whether the Telemetry panel is open and drawn. While it
		// is (and not held by Stop), a recording runs, started with GetPanelSettings when none does; when it closes, the
		// recording it started stops (one telemetry.start began goes on).
		void Update(bool panelOpen);

		// Whether a recording telemetry.start began is in progress, and whether one the open panel began is.
		bool IsCommandRecording() const;
		bool IsPanelRecording() const;
		// Whether Stop holds the open panel from recording.
		bool IsHeld() const { return m_Held; }
		// Why the panel's last attempt to record failed (empty when it did not fail).
		std::string const& GetRecordingError() const { return m_RecordingError; }

	private:
		// Who began the recording in progress.
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
	};
}
