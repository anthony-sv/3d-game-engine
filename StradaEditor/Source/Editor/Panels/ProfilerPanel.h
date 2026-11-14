#pragma once

#include "Editor/EditorProfiler.h"

#include "Strada/Core/Profiler.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Strada
{
	// The tabs below the Profiler panel's frame strip.
	enum class ProfilerPanelTab : uint8_t
	{
		// The shown frame's zones by thread, as trees with their total and self times.
		Zones = 0,
		// The shown frame's GPU passes per view.
		GpuPasses,
		// Medians over every frame held.
		Medians
	};

	// The Profiler panel: the frame times of the live history (or of a capture opened in its place) as a strip of bars, and
	// for the shown frame (the one clicked, else the latest) its zones by thread as trees with total and self times, its GPU
	// passes, and medians over all the frames. It records through EditorProfiler, which records while the panel is open; the
	// layer calls EditorProfiler::Update with whether it is. ImGui only; it reads the recorder on the main thread.
	class ProfilerPanel
	{
	public:
		static constexpr char const* WindowName = "Profiler";

		// What the last draw showed, read by tests.
		struct DrawRecord
		{
			struct ZoneRow
			{
				std::string Name;
				// 0 for a thread's row, 1 for the zones at its root, and so on.
				uint32_t Depth = 0;
				bool Thread = false;
				double TotalMilliseconds = 0.0;
				// The time not spent in nested zones (a thread's row, which draws no self time: its busy time).
				double SelfMilliseconds = 0.0;
				// "<label> <value>" for a zone with a count, else empty.
				std::string Value;
				// The middle of the row's label in screen coordinates, where a click opens or closes it.
				std::array<float, 2> Center = {0.0f, 0.0f};
			};

			struct GpuRow
			{
				std::string View;
				std::string Pass;
				double Milliseconds = 0.0;
			};

			// Whether the window's contents were drawn (not collapsed or hidden behind another tab).
			bool Drawn = false;
			// The live history, or a capture opened in its place.
			bool Live = true;
			std::string Status;
			// The middle of the Pause or Record button (the live history only), in screen coordinates.
			std::array<float, 2> ToggleCenter = {0.0f, 0.0f};
			// The frames of the strip, and where their bars lie in screen coordinates: the bar of the frame of index
			// FirstFrame + n spans BarsLeft + n * BarWidth to BarsLeft + (n + 1) * BarWidth, from BarsTop to BarsBottom.
			size_t Frames = 0;
			uint64_t FirstFrame = 0;
			float BarsLeft = 0.0f;
			float BarsTop = 0.0f;
			float BarsBottom = 0.0f;
			float BarWidth = 0.0f;
			// The frame whose details show, none without frames.
			std::optional<uint64_t> Frame;
			ProfilerPanelTab Tab = ProfilerPanelTab::Zones;
			// The rows of the tree in view, in order: closed nodes hide their subtrees, and the rows scrolled out of view are
			// not drawn.
			std::vector<ZoneRow> ZoneRows;
			// Whether this draw built the shown frame's trees (another frame or source showed); otherwise it drew those it
			// kept.
			bool TreesBuilt = false;
			std::vector<GpuRow> GpuRows;
			// Rows of the medians' tables.
			size_t MedianRows = 0;
		};

		// dialogDirectory: where Open Capture and Save Capture start (the project's directory; empty for the platform's).
		void OnImGuiRender(EditorProfiler& profiler, bool& open, std::filesystem::path const& dialogDirectory);

		DrawRecord const& GetLastDraw() const { return m_Record; }
		// The frame whose details show: none follows the latest frame.
		std::optional<uint64_t> GetSelectedFrame() const { return m_SelectedFrame; }
		void SelectFrame(std::optional<uint64_t> frame) { m_SelectedFrame = frame; }
		// Brings a tab to the front: it shows from the second draw on (ImGui selects a tab in the frame after the one that
		// asks).
		void ShowTab(ProfilerPanelTab tab) { m_RequestedTab = tab; }

	private:
		// What a toolbar button asked for, done after the draw.
		enum class ToolbarAction : uint8_t
		{
			None = 0,
			Pause,
			Record,
			CloseCapture,
			OpenCapture
		};

		// The frame whose trees the panel keeps: the source it came from and what tells its frames apart (the live history's
		// indices are never reused, but a frame's start and zone count tell a later recording's apart too).
		struct ShownTrees
		{
			bool Live = true;
			uint64_t CaptureRevision = 0;
			uint64_t Frame = 0;
			int64_t StartNanoseconds = 0;
			size_t Zones = 0;

			bool operator==(ShownTrees const&) const = default;
		};

		// A row of the zone table: a thread's, or one of its zones'.
		struct ZoneLine
		{
			// Index into m_Trees.
			uint32_t Tree = 0;
			// The zone's row in its tree; unused for a thread's row (Depth 0).
			uint32_t Row = 0;
			// 0 for a thread's row, 1 for the zones at its root, and so on.
			uint32_t Depth = 0;
			// One past the last line of its subtree.
			uint32_t End = 0;
			// Its place in the tree: the thread, then each zone's name and its count among the siblings of that name, so that
			// a node opened stays open from frame to frame.
			uint64_t Path = 0;
		};

		void DrawContents(EditorProfiler& profiler, std::filesystem::path const& dialogDirectory);
		void ApplyToolbarAction(EditorProfiler& profiler);
		void DrawToolbar(EditorProfiler& profiler, ProfilerCapture const& capture, std::filesystem::path const& dialogDirectory);
		void DrawFrameStrip(ProfilerCapture const& capture);
		// Builds the trees and lines of the frame when another one shows.
		void UpdateTrees(ProfilerFrame const& frame, ShownTrees const& shown);
		// The lines that show: those not inside a closed node.
		void UpdateVisibleLines();
		bool IsLineOpen(ZoneLine const& line) const;
		void DrawZoneLine(ProfilerCapture const& capture, ProfilerFrame const& frame, uint32_t visible);
		void DrawZones(ProfilerCapture const& capture, ProfilerFrame const& frame, ShownTrees const& shown);
		void DrawGpuPasses(ProfilerCapture const& capture, ProfilerFrame const& frame);
		void DrawMedians(ProfilerCapture const& capture);

		DrawRecord m_Record;
		ToolbarAction m_Action = ToolbarAction::None;
		// The capture file OpenCapture opens.
		std::filesystem::path m_ActionPath;
		std::optional<uint64_t> m_SelectedFrame;
		std::optional<ProfilerPanelTab> m_RequestedTab;
		ProfilerPanelTab m_Tab = ProfilerPanelTab::Zones;
		// What the panel showed in the last draw: the live history, or the capture of this revision.
		bool m_ShowedLive = true;
		uint64_t m_ShownCaptureRevision = 0;
		// The shown frame's trees, kept while it shows, as lines; the lines in view of a table that draws only what it
		// shows (closed nodes hide theirs); and the paths of the nodes not in their default state (threads and the zones at
		// their root open, the rest closed).
		std::optional<ShownTrees> m_ShownTrees;
		std::vector<ProfilerThreadTree> m_Trees;
		std::vector<ZoneLine> m_Lines;
		std::vector<uint32_t> m_VisibleLines;
		bool m_VisibleLinesDirty = true;
		std::unordered_set<uint64_t> m_ToggledPaths;
		// The medians of the shown frames, worked out at most once a second for the live history and once for a capture.
		std::optional<ProfilerMedians> m_Medians;
		double m_MediansTime = 0.0;
		// The strip is asked to scroll to the newest frame (another source, the Latest button); it follows it by itself while it
		// shows the end, its scroll at the end of the last draw (m_StripScrollMax).
		bool m_ScrollToLatest = true;
		float m_StripScrollMax = 0.0f;
	};
}
