#include "Editor/Panels/ProfilerPanel.h"

#include "Editor/EditorProfiler.h"
#include "Editor/FileDialogs.h"
#include "Editor/UI/EditorUI.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Serialization/ProfilerCaptureSerializer.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr double NanosecondsPerMillisecond = 1000000.0;
		// A frame at 60 frames per second: the strip marks it, and its scale is never lower.
		constexpr double FrameBudgetMilliseconds = 1000.0 / 60.0;
		constexpr float MinBarWidth = 3.0f;
		constexpr float MaxBarWidth = 12.0f;
		// How often the live medians are worked out again, in seconds.
		constexpr double MediansInterval = 1.0;

		std::array<FileDialogFilter, 1> const& GetCaptureFilters()
		{
			static std::array<FileDialogFilter, 1> const s_Filters = {{{"Profiler Capture", "json"}}};
			return s_Filters;
		}

		double ToMilliseconds(int64_t nanoseconds)
		{
			return static_cast<double>(nanoseconds) / NanosecondsPerMillisecond;
		}

		double GetFrameMilliseconds(ProfilerFrame const& frame)
		{
			return ToMilliseconds(frame.EndNanoseconds - frame.StartNanoseconds);
		}

		ImU32 GetColor(ImGuiCol color, float alpha = 1.0f)
		{
			return ImGui::GetColorU32(color, alpha);
		}

		bool BeginTable(char const* name, int columns, ImVec2 size = ImVec2(0.0f, 0.0f))
		{
			ImGuiTableFlags flags =
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
			if (size.y > 0.0f)
			{
				// A table that scrolls keeps at least a few rows' height, however little room is left.
				flags |= ImGuiTableFlags_ScrollY;
				size.y = std::max(size.y, ImGui::GetFrameHeightWithSpacing() * 4.0f);
			}
			return ImGui::BeginTable(name, columns, flags, size);
		}

		void SetupTimeColumns(char const* first, std::initializer_list<char const*> times)
		{
			ImGui::TableSetupColumn(first, ImGuiTableColumnFlags_WidthStretch, 3.0f);
			for (char const* const time : times)
			{
				ImGui::TableSetupColumn(time, ImGuiTableColumnFlags_WidthStretch, 1.0f);
			}
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();
		}

		void TimeCell(double milliseconds)
		{
			ImGui::TableNextColumn();
			ImGui::Text("%.3f", milliseconds);
		}

		// The path of a tree node's child: its parent's path mixed with a value of the child's (SplitMix64's finalizer).
		uint64_t CombinePath(uint64_t parent, uint64_t value)
		{
			uint64_t mixed = parent ^ (value + 0x9E3779B97F4A7C15ull + (parent << 6) + (parent >> 2));
			mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
			mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
			return mixed ^ (mixed >> 31);
		}
	}

	void ProfilerPanel::OnImGuiRender(EditorProfiler& profiler, bool& open, std::filesystem::path const& dialogDirectory)
	{
		m_Record = DrawRecord();
		if (!ImGui::Begin(WindowName, &open))
		{
			ImGui::End();
			return;
		}
		m_Record.Drawn = true;
		DrawContents(profiler, dialogDirectory);
		ImGui::End();
		// What the toolbar asked for changes what the panel showed (the recording's history, the capture), so it is done once
		// nothing refers to them any more.
		ApplyToolbarAction(profiler);
	}

	void ProfilerPanel::DrawContents(EditorProfiler& profiler, std::filesystem::path const& dialogDirectory)
	{
		ProfilerCapture const* const opened = profiler.GetCapture();
		bool const live = opened == nullptr;
		ProfilerCapture const& capture = live ? Profiler::GetRecording() : *opened;
		// Another source shows from its latest frame, with medians of its own.
		if (live != m_ShowedLive || (!live && profiler.GetCaptureRevision() != m_ShownCaptureRevision))
		{
			m_ShowedLive = live;
			m_ShownCaptureRevision = profiler.GetCaptureRevision();
			m_SelectedFrame.reset();
			m_Medians.reset();
			m_ScrollToLatest = true;
		}
		m_Record.Live = live;

		DrawToolbar(profiler, capture, dialogDirectory);
		DrawFrameStrip(capture);

		if (capture.Frames.empty())
		{
			ImGui::TextDisabled(live ? "No frames recorded yet." : "The capture holds no frames.");
			return;
		}
		// A frame that left the history shows the latest again.
		ProfilerFrame const* frame = m_SelectedFrame ? FindProfilerFrame(capture, *m_SelectedFrame) : nullptr;
		if (frame == nullptr)
		{
			m_SelectedFrame.reset();
			frame = &capture.Frames.back();
		}
		m_Record.Frame = frame->Index;
		ImGui::Text("Frame %llu: %.2f ms, %zu zones", static_cast<unsigned long long>(frame->Index), GetFrameMilliseconds(*frame),
		            frame->Zones.size());
		if (frame->DroppedZones > 0)
		{
			ImGui::SameLine();
			ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogramHovered), "%u zones dropped", frame->DroppedZones);
		}

		if (ImGui::BeginTabBar("##ProfilerTabs"))
		{
			auto const tab = [this](char const* label, ProfilerPanelTab kind)
			{
				ImGuiTabItemFlags const flags = m_RequestedTab == kind ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
				bool const shown = ImGui::BeginTabItem(label, nullptr, flags);
				if (shown)
				{
					m_Tab = kind;
				}
				return shown;
			};
			if (tab("Zones", ProfilerPanelTab::Zones))
			{
				ShownTrees const shown{live, live ? 0 : profiler.GetCaptureRevision(), frame->Index, frame->StartNanoseconds,
				                       frame->Zones.size()};
				DrawZones(capture, *frame, shown);
				ImGui::EndTabItem();
			}
			if (tab("GPU Passes", ProfilerPanelTab::GpuPasses))
			{
				DrawGpuPasses(capture, *frame);
				ImGui::EndTabItem();
			}
			if (tab("Medians", ProfilerPanelTab::Medians))
			{
				DrawMedians(capture);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
			// ImGui selects a tab asked for in the frame after the one that asks.
			if (m_RequestedTab == m_Tab)
			{
				m_RequestedTab.reset();
			}
		}
		m_Record.Tab = m_Tab;
	}

	void ProfilerPanel::ApplyToolbarAction(EditorProfiler& profiler)
	{
		ToolbarAction const action = std::exchange(m_Action, ToolbarAction::None);
		switch (action)
		{
			case ToolbarAction::None:
				break;
			case ToolbarAction::Pause:
				profiler.Stop();
				break;
			case ToolbarAction::Record:
				profiler.Resume();
				break;
			case ToolbarAction::CloseCapture:
				profiler.CloseCapture();
				break;
			case ToolbarAction::OpenCapture:
				UI::ReportFailure(profiler.Open(m_ActionPath), "Opening the profiler capture");
				break;
		}
		m_ActionPath.clear();
	}

	void ProfilerPanel::DrawToolbar(EditorProfiler& profiler, ProfilerCapture const& capture, std::filesystem::path const& dialogDirectory)
	{
		if (m_Record.Live)
		{
			ProfilerStatus const status = Profiler::GetStatus();
			if (status.Recording)
			{
				if (ImGui::Button("Pause"))
				{
					m_Action = ToolbarAction::Pause;
				}
			}
			else if (ImGui::Button("Record"))
			{
				m_Action = ToolbarAction::Record;
			}
			m_Record.ToggleCenter = {(ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
			                         (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f};
			ImGui::SameLine();
			char text[160];
			if (status.Recording)
			{
				std::snprintf(text, sizeof(text), "Recording%s: %u frames held, %llu zones dropped",
				              profiler.IsCommandRecording() ? " (profiler.start)" : "", status.FramesHeld,
				              static_cast<unsigned long long>(status.DroppedZones));
			}
			else if (!profiler.GetRecordingError().empty())
			{
				std::snprintf(text, sizeof(text), "Not recording: %s", profiler.GetRecordingError().c_str());
			}
			else
			{
				std::snprintf(text, sizeof(text), "Paused: %zu frames held", capture.Frames.size());
			}
			m_Record.Status = text;
			ImGui::TextUnformatted(text);
		}
		else
		{
			if (ImGui::Button("Close Capture"))
			{
				m_Action = ToolbarAction::CloseCapture;
			}
			ImGui::SameLine();
			// The file's name only: the panel never shows where it lies.
			m_Record.Status = "Capture " + profiler.GetCaptureName();
			ImGui::Text("%s: %zu frames", m_Record.Status.c_str(), capture.Frames.size());
		}

		ImGui::SameLine();
		if (ImGui::Button("Open Capture..."))
		{
			Result<std::optional<std::filesystem::path>> const chosen = FileDialogs::OpenFile(GetCaptureFilters(), dialogDirectory);
			UI::ReportFailure(chosen, "Choosing a profiler capture");
			if (chosen && chosen.GetValue())
			{
				m_Action = ToolbarAction::OpenCapture;
				m_ActionPath = *chosen.GetValue();
			}
		}
		if (m_Record.Live && !capture.Frames.empty())
		{
			ImGui::SameLine();
			if (ImGui::Button("Save Capture..."))
			{
				Result<std::optional<std::filesystem::path>> const chosen =
					FileDialogs::SaveFile(GetCaptureFilters(), dialogDirectory, "Capture.json");
				UI::ReportFailure(chosen, "Choosing where to save the profiler capture");
				if (chosen && chosen.GetValue())
				{
					std::filesystem::path path = *chosen.GetValue();
					if (path.extension() != ".json")
					{
						path += ".json";
					}
					UI::ReportFailure(ProfilerCaptureSerializer::Save(capture, path), "Saving the profiler capture");
				}
			}
		}
		if (m_SelectedFrame)
		{
			ImGui::SameLine();
			if (ImGui::Button("Latest"))
			{
				m_SelectedFrame.reset();
				m_ScrollToLatest = true;
			}
		}
	}

	void ProfilerPanel::DrawFrameStrip(ProfilerCapture const& capture)
	{
		float const height = ImGui::GetFrameHeight() * 3.5f + ImGui::GetStyle().ScrollbarSize;
		if (!ImGui::BeginChild("##Frames", ImVec2(0.0f, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
		{
			ImGui::EndChild();
			return;
		}
		size_t const count = capture.Frames.size();
		ImVec2 const available = ImGui::GetContentRegionAvail();
		float const barWidth =
			count > 0 ? std::clamp(std::floor(available.x / static_cast<float>(count)), MinBarWidth, MaxBarWidth) : MaxBarWidth;
		float const barsHeight = std::max(available.y, 1.0f);
		ImVec2 const origin = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##Bars", ImVec2(std::max(barWidth * static_cast<float>(count), 1.0f), barsHeight));
		m_Record.Frames = count;
		m_Record.FirstFrame = count > 0 ? capture.Frames.front().Index : 0;
		m_Record.BarsLeft = origin.x;
		m_Record.BarsTop = origin.y;
		m_Record.BarsBottom = origin.y + barsHeight;
		m_Record.BarWidth = barWidth;
		if (count == 0)
		{
			ImGui::EndChild();
			return;
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			float const offset = (ImGui::GetIO().MousePos.x - origin.x) / barWidth;
			size_t const clicked = std::min(static_cast<size_t>(std::max(offset, 0.0f)), count - 1);
			m_SelectedFrame = capture.Frames[clicked].Index;
			m_ScrollToLatest = false;
		}
		// Only the bars in view are drawn; the scale fits the tallest of them, and never goes below a 60 Hz frame.
		float const scroll = ImGui::GetScrollX();
		size_t const first = std::min(static_cast<size_t>(scroll / barWidth), count - 1);
		size_t const last = std::min(count, first + static_cast<size_t>(available.x / barWidth) + 2);
		double scale = FrameBudgetMilliseconds * 1.25;
		for (size_t index = first; index < last; index++)
		{
			scale = std::max(scale, GetFrameMilliseconds(capture.Frames[index]));
		}
		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		uint64_t const shown = m_SelectedFrame.value_or(capture.Frames.back().Index);
		for (size_t index = first; index < last; index++)
		{
			ProfilerFrame const& frame = capture.Frames[index];
			double const milliseconds = GetFrameMilliseconds(frame);
			float const barHeight = static_cast<float>(milliseconds / scale) * barsHeight;
			float const left = origin.x + static_cast<float>(index) * barWidth;
			ImGuiCol const color = frame.Index == shown                           ? ImGuiCol_PlotLinesHovered
			                       : milliseconds > 2.0 * FrameBudgetMilliseconds ? ImGuiCol_PlotHistogramHovered
			                                                                      : ImGuiCol_PlotHistogram;
			drawList->AddRectFilled(ImVec2(left, origin.y + barsHeight - std::max(barHeight, 1.0f)),
			                        ImVec2(left + std::max(barWidth - 1.0f, 1.0f), origin.y + barsHeight), GetColor(color));
		}
		// The 60 Hz line, labelled at the left of the view.
		float const budget = origin.y + barsHeight - static_cast<float>(FrameBudgetMilliseconds / scale) * barsHeight;
		float const viewLeft = origin.x + scroll;
		drawList->AddLine(ImVec2(viewLeft, budget), ImVec2(viewLeft + available.x, budget), GetColor(ImGuiCol_TextDisabled, 0.6f));
		drawList->AddText(ImVec2(viewLeft + 4.0f, budget - ImGui::GetTextLineHeight()), GetColor(ImGuiCol_TextDisabled), "16.7 ms");
		// The strip follows the newest frame while it shows the end and no frame is clicked; scrolled back, it stays where the
		// user put it.
		float const scrollMax = ImGui::GetScrollMaxX();
		bool const atEnd = scroll >= m_StripScrollMax - 1.0f;
		if (m_ScrollToLatest || (atEnd && !m_SelectedFrame))
		{
			ImGui::SetScrollX(scrollMax);
		}
		// A request holds until the strip got there: the bars' width is known, and scrolling applied, a frame later.
		if (scrollMax > 0.0f && scroll >= scrollMax - 1.0f)
		{
			m_ScrollToLatest = false;
		}
		m_StripScrollMax = scrollMax;
		ImGui::EndChild();
	}

	void ProfilerPanel::UpdateTrees(ProfilerFrame const& frame, ShownTrees const& shown)
	{
		if (m_ShownTrees == shown)
		{
			return;
		}
		m_ShownTrees = shown;
		m_Record.TreesBuilt = true;
		m_Trees = BuildProfilerFrameTree(frame);
		m_Lines.clear();
		m_VisibleLinesDirty = true;
		// How many siblings of each name came before, by the path of the name's place.
		std::unordered_map<uint64_t, uint32_t> occurrences;
		std::vector<uint64_t> parentPaths;
		for (uint32_t treeIndex = 0; treeIndex < m_Trees.size(); treeIndex++)
		{
			ProfilerThreadTree const& tree = m_Trees[treeIndex];
			uint64_t const threadPath = CombinePath(0, tree.Thread);
			uint32_t const first = static_cast<uint32_t>(m_Lines.size()) + 1;
			m_Lines.push_back({treeIndex, 0, 0, first + static_cast<uint32_t>(tree.Rows.size()), threadPath});
			parentPaths.clear();
			for (uint32_t row = 0; row < tree.Rows.size(); row++)
			{
				ProfilerTreeRow const& treeRow = tree.Rows[row];
				parentPaths.resize(treeRow.Depth);
				uint64_t const named = CombinePath(treeRow.Depth == 0 ? threadPath : parentPaths.back(), frame.Zones[treeRow.Zone].Name);
				uint64_t const path = CombinePath(named, occurrences[named]++);
				parentPaths.push_back(path);
				m_Lines.push_back({treeIndex, row, treeRow.Depth + 1, first + treeRow.SubtreeEnd, path});
			}
		}
	}

	bool ProfilerPanel::IsLineOpen(ZoneLine const& line) const
	{
		bool const openByDefault = line.Depth <= 1;
		return openByDefault != m_ToggledPaths.contains(line.Path);
	}

	void ProfilerPanel::UpdateVisibleLines()
	{
		if (!m_VisibleLinesDirty)
		{
			return;
		}
		m_VisibleLinesDirty = false;
		m_VisibleLines.clear();
		for (uint32_t index = 0; index < m_Lines.size();)
		{
			m_VisibleLines.push_back(index);
			ZoneLine const& line = m_Lines[index];
			index = line.End > index + 1 && !IsLineOpen(line) ? line.End : index + 1;
		}
	}

	void ProfilerPanel::DrawZoneLine(ProfilerCapture const& capture, ProfilerFrame const& frame, uint32_t visible)
	{
		ZoneLine const& line = m_Lines[m_VisibleLines[visible]];
		ProfilerThreadTree const& tree = m_Trees[line.Tree];
		bool const thread = line.Depth == 0;
		ProfilerZone const* const zone = thread ? nullptr : &frame.Zones[tree.Rows[line.Row].Zone];
		std::string_view const name =
			thread ? (tree.Thread < capture.Threads.size() ? std::string_view(capture.Threads[tree.Thread].Name) : std::string_view("?"))
				   : GetProfilerName(capture, zone->Name);
		bool const leaf = line.End == m_VisibleLines[visible] + 1;

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::PushID(static_cast<int>(visible));
		// Lines are drawn flat, so that only those in view are: the indent stands for the tree's nesting.
		float const indent = static_cast<float>(line.Depth) * ImGui::GetStyle().IndentSpacing;
		if (indent > 0.0f)
		{
			ImGui::Indent(indent);
		}
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		if (leaf)
		{
			flags |= ImGuiTreeNodeFlags_Leaf;
		}
		else
		{
			ImGui::SetNextItemOpen(IsLineOpen(line), ImGuiCond_Always);
		}
		ImGui::TreeNodeEx("##Line", flags, "%.*s", static_cast<int>(name.size()), name.data());
		if (ImGui::IsItemToggledOpen())
		{
			// The node's subtree shows or hides from the next draw.
			if (!m_ToggledPaths.erase(line.Path))
			{
				m_ToggledPaths.insert(line.Path);
			}
			m_VisibleLinesDirty = true;
		}
		std::array<float, 2> const center = {(ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
		                                     (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f};
		if (indent > 0.0f)
		{
			ImGui::Unindent(indent);
		}

		if (thread)
		{
			double const busy = ToMilliseconds(tree.BusyNanoseconds);
			TimeCell(busy);
			// A thread has no time of its own outside its zones: its row shows its busy time once.
			ImGui::TableNextColumn();
			ImGui::TableNextColumn();
			m_Record.ZoneRows.push_back({std::string(name), 0, true, busy, busy, {}, center});
		}
		else
		{
			double const total = ToMilliseconds(zone->DurationNanoseconds);
			double const selfTime = ToMilliseconds(tree.Rows[line.Row].SelfNanoseconds);
			TimeCell(total);
			TimeCell(selfTime);
			ImGui::TableNextColumn();
			std::string value;
			if (zone->ValueLabel != ProfilerNoName)
			{
				value = std::string(GetProfilerName(capture, zone->ValueLabel)) + " " + std::to_string(zone->Value);
				ImGui::TextUnformatted(value.c_str());
			}
			m_Record.ZoneRows.push_back({std::string(name), line.Depth, false, total, selfTime, std::move(value), center});
		}
		ImGui::PopID();
	}

	void ProfilerPanel::DrawZones(ProfilerCapture const& capture, ProfilerFrame const& frame, ShownTrees const& shown)
	{
		UpdateTrees(frame, shown);
		if (m_Trees.empty())
		{
			ImGui::TextDisabled("The frame recorded no zone.");
			return;
		}
		UpdateVisibleLines();
		if (!BeginTable("##Zones", 4, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
		{
			return;
		}
		ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("Total (ms)", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Self (ms)", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();

		// A frame can hold millions of zones: only the lines in view are drawn.
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(m_VisibleLines.size()));
		while (clipper.Step())
		{
			for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; visible++)
			{
				DrawZoneLine(capture, frame, static_cast<uint32_t>(visible));
			}
		}
		ImGui::EndTable();
	}

	void ProfilerPanel::DrawGpuPasses(ProfilerCapture const& capture, ProfilerFrame const& frame)
	{
		if (frame.GpuPasses.empty())
		{
			ImGui::TextDisabled("No GPU pass was measured for this frame. Passes arrive a few frames late, and a GPU without "
			                    "timer queries measures none.");
			return;
		}
		if (!BeginTable("##GpuPasses", 3, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
		{
			return;
		}
		ImGui::TableSetupColumn("View", ImGuiTableColumnFlags_WidthStretch, 1.5f);
		SetupTimeColumns("Pass", {"Time (ms)"});
		for (ProfilerGpuPass const& pass : frame.GpuPasses)
		{
			std::string_view const view = GetProfilerName(capture, pass.View);
			std::string_view const name = GetProfilerName(capture, pass.Name);
			double const milliseconds = ToMilliseconds(pass.DurationNanoseconds);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(view.data(), view.data() + view.size());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(name.data(), name.data() + name.size());
			TimeCell(milliseconds);
			m_Record.GpuRows.push_back({std::string(view), std::string(name), milliseconds});
		}
		ImGui::EndTable();
	}

	void ProfilerPanel::DrawMedians(ProfilerCapture const& capture)
	{
		double const now = ImGui::GetTime();
		if (!m_Medians || (m_ShowedLive && now - m_MediansTime >= MediansInterval))
		{
			m_Medians = ComputeProfilerMedians(capture);
			m_MediansTime = now;
		}
		ProfilerMedians const& medians = *m_Medians;
		ImGui::Text("Frame median %.2f ms, 99th percentile %.2f ms", medians.FrameMilliseconds, medians.FramePercentile99Milliseconds);
		if (!ImGui::BeginChild("##Medians", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None))
		{
			ImGui::EndChild();
			return;
		}
		auto const section = [this](char const* title, char const* table, std::vector<ProfilerMedian> const& rows)
		{
			if (rows.empty())
			{
				return;
			}
			ImGui::SeparatorText(title);
			if (!BeginTable(table, 2))
			{
				return;
			}
			SetupTimeColumns("Name", {"Per frame (ms)"});
			for (ProfilerMedian const& row : rows)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.Name.c_str());
				TimeCell(row.Milliseconds);
				m_Record.MedianRows++;
			}
			ImGui::EndTable();
		};
		section("Subsystems (main thread)", "##Subsystems", medians.Subsystems);
		section("Other threads (busy time)", "##Threads", medians.Threads);
		section("GPU passes", "##GpuMedians", medians.GpuPasses);
		if (!medians.Zones.empty())
		{
			ImGui::SeparatorText("Zones");
			std::vector<ProfilerZoneMedian const*> zones;
			for (ProfilerZoneMedian const& zone : medians.Zones)
			{
				zones.push_back(&zone);
			}
			std::stable_sort(zones.begin(), zones.end(),
			                 [](ProfilerZoneMedian const* first, ProfilerZoneMedian const* second)
			                 {
								 return first->MillisecondsPerFrame > second->MillisecondsPerFrame;
							 });
			if (BeginTable("##ZoneMedians", 4))
			{
				SetupTimeColumns("Zone", {"Per frame (ms)", "Per call (ms)", "Calls per frame"});
				for (ProfilerZoneMedian const* zone : zones)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(zone->Name.c_str());
					TimeCell(zone->MillisecondsPerFrame);
					TimeCell(zone->MillisecondsPerCall);
					ImGui::TableNextColumn();
					ImGui::Text("%.2f", zone->CallsPerFrame);
					m_Record.MedianRows++;
				}
				ImGui::EndTable();
			}
		}
		ImGui::EndChild();
	}
}
