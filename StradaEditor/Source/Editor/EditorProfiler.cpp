#include "Editor/EditorProfiler.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Serialization/ProfilerCaptureSerializer.h"

#include <algorithm>
#include <utility>

namespace Strada
{
	Result<void> EditorProfiler::Start(ProfilerSettings const& settings)
	{
		if (!Profiler::IsInitialized())
		{
			return MakeError("the profiler is not initialized");
		}
		if (!Profiler::GetStatus().Available)
		{
			return MakeError("the profiler records nothing in Dist builds");
		}
		// Settings out of bounds leave a recording in progress as it is: they are refused before anything stops.
		if (settings.HistoryFrames < Profiler::MinHistoryFrames || settings.HistoryFrames > Profiler::MaxHistoryFrames)
		{
			return MakeError("the history holds {} to {} frames", Profiler::MinHistoryFrames, Profiler::MaxHistoryFrames);
		}
		if (settings.HistoryZones < Profiler::MinHistoryZones || settings.HistoryZones > Profiler::MaxHistoryZones)
		{
			return MakeError("the history holds {} to {} zones", Profiler::MinHistoryZones, Profiler::MaxHistoryZones);
		}
		if (settings.ZonesPerThread < Profiler::MinZonesPerThread || settings.ZonesPerThread > Profiler::MaxZonesPerThread)
		{
			return MakeError("a thread's buffer holds {} to {} zones", Profiler::MinZonesPerThread, Profiler::MaxZonesPerThread);
		}
		Profiler::Stop();
		m_Owner = Owner::None;
		Result<void> started = Profiler::Start(settings);
		if (!started)
		{
			return started;
		}
		m_Owner = Owner::Command;
		m_Held = false;
		m_RecordingError.clear();
		return {};
	}

	bool EditorProfiler::Stop()
	{
		bool const recording = Profiler::IsRecording();
		Profiler::Stop();
		m_Owner = Owner::None;
		m_Held = true;
		return recording;
	}

	void EditorProfiler::Resume()
	{
		m_Held = false;
		m_RecordingError.clear();
	}

	void EditorProfiler::Update(bool panelOpen)
	{
		if (!panelOpen)
		{
			// A panel opened again tries to record again, whatever held it or failed before.
			m_Held = false;
			m_RecordingError.clear();
			if (m_PanelWasOpen && GetOwner() == Owner::Panel)
			{
				Profiler::Stop();
				m_Owner = Owner::None;
			}
			m_PanelWasOpen = false;
			return;
		}
		m_PanelWasOpen = true;
		if (m_Held || Profiler::IsRecording() || !m_RecordingError.empty())
		{
			return;
		}
		if (!Profiler::IsInitialized())
		{
			m_RecordingError = "the profiler is not initialized";
			return;
		}
		if (!Profiler::GetStatus().Available)
		{
			m_RecordingError = "the profiler records nothing in Dist builds";
			return;
		}
		// A recording that ended elsewhere is not restarted every frame: the attempt is made once until Resume.
		if (Result<void> started = Profiler::Start(GetPanelSettings()); !started)
		{
			m_RecordingError = started.GetError();
			return;
		}
		m_Owner = Owner::Panel;
	}

	bool EditorProfiler::IsCommandRecording() const
	{
		return GetOwner() == Owner::Command;
	}

	bool EditorProfiler::IsPanelRecording() const
	{
		return GetOwner() == Owner::Panel;
	}

	EditorProfiler::Owner EditorProfiler::GetOwner() const
	{
		return Profiler::IsRecording() ? m_Owner : Owner::None;
	}

	Result<void> EditorProfiler::Open(std::filesystem::path const& path)
	{
		Result<ProfilerCapture> loaded = ProfilerCaptureSerializer::Load(path);
		if (!loaded)
		{
			return MakeError("{}", loaded.GetError());
		}
		SetCapture(loaded.TakeValue(), FileSystem::PathToUtf8(path.filename()));
		m_CapturePath = path;
		return {};
	}

	void EditorProfiler::SetCapture(ProfilerCapture capture, std::string name)
	{
		m_Capture = std::move(capture);
		m_CaptureName = std::move(name);
		m_CapturePath.clear();
		m_CaptureRevision++;
	}

	void EditorProfiler::CloseCapture()
	{
		if (!m_Capture)
		{
			return;
		}
		m_Capture.reset();
		m_CaptureName.clear();
		m_CapturePath.clear();
		m_CaptureRevision++;
	}

	std::vector<ProfilerThreadTree> BuildProfilerFrameTree(ProfilerFrame const& frame)
	{
		std::vector<ProfilerThreadTree> trees;
		// The rows of the zones still open above the zone placed: (recorded depth, row).
		std::vector<std::pair<uint32_t, uint32_t>> open;
		for (uint32_t zoneIndex = 0; zoneIndex < frame.Zones.size(); zoneIndex++)
		{
			ProfilerZone const& zone = frame.Zones[zoneIndex];
			if (trees.empty() || trees.back().Thread != zone.Thread)
			{
				trees.push_back({zone.Thread, 0, {}});
				open.clear();
			}
			ProfilerThreadTree& tree = trees.back();
			while (!open.empty() && open.back().first >= zone.Depth)
			{
				open.pop_back();
			}
			uint32_t const row = static_cast<uint32_t>(tree.Rows.size());
			if (open.empty())
			{
				tree.BusyNanoseconds += zone.DurationNanoseconds;
			}
			else
			{
				tree.Rows[open.back().second].SelfNanoseconds -= zone.DurationNanoseconds;
			}
			tree.Rows.push_back({zoneIndex, static_cast<uint32_t>(open.size()), 0, zone.DurationNanoseconds});
			open.emplace_back(zone.Depth, row);
		}
		for (ProfilerThreadTree& tree : trees)
		{
			// A subtree ends at the first later row that is not deeper.
			std::vector<uint32_t> pending;
			for (uint32_t row = 0; row < tree.Rows.size(); row++)
			{
				while (!pending.empty() && tree.Rows[pending.back()].Depth >= tree.Rows[row].Depth)
				{
					tree.Rows[pending.back()].SubtreeEnd = row;
					pending.pop_back();
				}
				pending.push_back(row);
			}
			for (uint32_t const row : pending)
			{
				tree.Rows[row].SubtreeEnd = static_cast<uint32_t>(tree.Rows.size());
			}
			for (ProfilerTreeRow& row : tree.Rows)
			{
				row.SelfNanoseconds = std::max<int64_t>(row.SelfNanoseconds, 0);
			}
		}
		return trees;
	}

	ProfilerFrame const* FindProfilerFrame(ProfilerCapture const& capture, uint64_t index)
	{
		if (capture.Frames.empty() || index < capture.Frames.front().Index)
		{
			return nullptr;
		}
		uint64_t const position = index - capture.Frames.front().Index;
		if (position >= capture.Frames.size())
		{
			return nullptr;
		}
		ProfilerFrame const& frame = capture.Frames[static_cast<size_t>(position)];
		return frame.Index == index ? &frame : nullptr;
	}

	std::string_view GetProfilerName(ProfilerCapture const& capture, uint32_t name)
	{
		return name < capture.Names.size() ? std::string_view(capture.Names[name]) : std::string_view("?");
	}
}
