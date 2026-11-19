#include "Editor/EditorTelemetry.h"

namespace Strada
{
	Result<void> EditorTelemetry::Start(TelemetrySettings const& settings)
	{
		if (!Telemetry::IsInitialized())
		{
			return MakeError("telemetry is not initialized");
		}
		// Settings out of bounds leave a recording in progress as it is: they are refused before anything stops.
		if (Result<void> checked = Telemetry::CheckSettings(settings); !checked)
		{
			return checked;
		}
		Telemetry::Stop();
		m_Owner = Owner::None;
		Result<void> started = Telemetry::Start(settings);
		if (!started)
		{
			return started;
		}
		m_Owner = Owner::Command;
		m_Held = false;
		m_RecordingError.clear();
		return {};
	}

	bool EditorTelemetry::Stop()
	{
		bool const recording = Telemetry::IsRecording();
		Telemetry::Stop();
		m_Owner = Owner::None;
		m_Held = true;
		return recording;
	}

	void EditorTelemetry::Resume()
	{
		m_Held = false;
		m_RecordingError.clear();
	}

	void EditorTelemetry::Update(bool panelOpen)
	{
		if (!panelOpen)
		{
			// A panel opened again tries to record again, whatever held it or failed before.
			m_Held = false;
			m_RecordingError.clear();
			if (m_PanelWasOpen && GetOwner() == Owner::Panel)
			{
				Telemetry::Stop();
				m_Owner = Owner::None;
			}
			m_PanelWasOpen = false;
			return;
		}
		m_PanelWasOpen = true;
		if (m_Held || Telemetry::IsRecording() || !m_RecordingError.empty())
		{
			return;
		}
		if (!Telemetry::IsInitialized())
		{
			m_RecordingError = "telemetry is not initialized";
			return;
		}
		// A recording that ended elsewhere is not restarted every frame: the attempt is made once until Resume.
		if (Result<void> started = Telemetry::Start(GetPanelSettings()); !started)
		{
			m_RecordingError = started.GetError();
			return;
		}
		m_Owner = Owner::Panel;
	}

	bool EditorTelemetry::IsCommandRecording() const
	{
		return GetOwner() == Owner::Command;
	}

	bool EditorTelemetry::IsPanelRecording() const
	{
		return GetOwner() == Owner::Panel;
	}

	EditorTelemetry::Owner EditorTelemetry::GetOwner() const
	{
		return Telemetry::IsRecording() ? m_Owner : Owner::None;
	}
}
