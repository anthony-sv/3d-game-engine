#pragma once

#include "Editor/EditorTelemetry.h"
#include "Editor/TelemetryPlot.h"

#include "Strada/Core/Telemetry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Strada
{
	class Scene;

	// The Telemetry panel (Docs/Architecture.md §4.5, Docs/Editor.md "Telemetry"): a session the recorder holds (by default
	// the newest, followed while it is live) with its channels, those chosen plotted each in a lane of its own against time
	// or a distance channel, a lap overlaid on another (by default the lap in progress or the last lap, over the best other
	// lap), drawn with ImGui's draw lists. It records through EditorTelemetry, which records while the panel is open; the
	// layer calls EditorTelemetry::Update with whether it is. Drawing reads the sessions and changes nothing in them. ImGui
	// only; main thread.
	class TelemetryPanel
	{
	public:
		static constexpr char const* WindowName = "Telemetry";
		// Lanes shown at most at once, and how many the panel chooses for a session on its own.
		static constexpr size_t MaxLanes = 8;
		static constexpr size_t DefaultLanes = 4;

		// What the last draw showed, read by tests.
		struct DrawRecord
		{
			// A channel of the shown session in the panel's list.
			struct ChannelRow
			{
				std::string Name;
				// The name the list shows (an engine channel under its entity's name, DescribeTelemetryChannel).
				std::string Label;
				bool Chosen = false;
				// The channel's last value.
				float Last = 0.0f;
				// The middle of its check box, in screen coordinates.
				std::array<float, 2> Center = {0.0f, 0.0f};
			};

			// A lap or the session, as plotted. Held in std::optional by its enclosing struct, so its constructor is
			// defaulted in the .cpp (Clang with libstdc++ sees its initializers only once DrawRecord is complete).
			struct Span
			{
				Span();

				uint32_t Session = 0;
				// "Lap 2", empty for the session.
				std::string Label;
				bool InProgress = false;
				double Seconds = 0.0;
				// The rows it covers, and those placed along the axis (by distance, those with a distance within its run).
				uint64_t FirstRow = 0;
				uint64_t EndRow = 0;
				uint64_t PlacedFirstRow = 0;
				uint64_t PlacedEndRow = 0;
			};

			// One channel of one span in a lane; held in std::optional too (Span).
			struct Series
			{
				Series();

				// Rows with a value and a place on the axis.
				uint64_t Rows = 0;
				size_t Points = 0;
				size_t Segments = 0;
				float Minimum = 0.0f;
				float Maximum = 0.0f;
				// The first and last point in screen coordinates (none without points).
				std::optional<std::array<float, 2>> First;
				std::optional<std::array<float, 2>> Last;
			};

			struct Lane
			{
				std::string Channel;
				std::string Label;
				// The value range the lane spans, bottom to top.
				float Minimum = 0.0f;
				float Maximum = 0.0f;
				float Top = 0.0f;
				float Bottom = 0.0f;
				Series Shown;
				// The overlaid lap's, when one is overlaid and its session holds the channel.
				std::optional<Series> Reference;
			};

			// Whether the window's contents were drawn (not collapsed or hidden behind another tab).
			bool Drawn = false;
			bool Recording = false;
			std::string Status;
			// The middle of the Pause or Record button, in screen coordinates.
			std::array<float, 2> ToggleCenter = {0.0f, 0.0f};
			// The shown session (0 for none) and its rows.
			uint32_t Session = 0;
			bool Live = false;
			uint64_t Rows = 0;
			std::vector<ChannelRow> Channels;
			TelemetryAxis Axis = TelemetryAxis::Time;
			// The distance channel the rows are plotted against (by distance).
			std::string DistanceChannel;
			// The positions at the lanes' left and right edges, and where the edges lie in screen coordinates.
			double AxisStart = 0.0;
			double AxisEnd = 0.0;
			float LanesLeft = 0.0f;
			float LanesRight = 0.0f;
			// The tick labels along the axis.
			size_t Ticks = 0;
			std::optional<Span> Shown;
			std::optional<Span> Reference;
			std::vector<Lane> Lanes;
		};

		// dialogDirectory: where Export CSV... starts (the project's directory; empty for the platform's). scene: the scene whose
		// entities name the engine's channels (the playing or edited one), may be null.
		void OnImGuiRender(EditorTelemetry& telemetry, bool& open, Scene* scene, std::filesystem::path const& dialogDirectory);

		DrawRecord const& GetLastDraw() const { return m_Record; }

		// The session shown: none follows the newest.
		void SelectSession(std::optional<uint32_t> session) { m_Session = session; }
		// The channels plotted, in order (at most MaxLanes are, and those the session has); an empty list goes back to the
		// panel's own choice (the session's first DefaultLanes channels but the axis's distance channel), which a session
		// that has none of the channels chosen shows too. The list's check boxes choose in the same way, except that
		// unchecking every channel plots none.
		void ChooseChannels(std::vector<std::string> channels)
		{
			m_ChannelsChosen = !channels.empty();
			m_Channels = std::move(channels);
		}
		// What the rows are plotted against: by default the session's DefaultTelemetryDistanceChannel when it has that
		// channel, else time; or always time; or a channel of the session (time while the session lacks it).
		void PlotAgainstDefault() { m_AxisChoice = AxisChoice::Default; }
		void PlotAgainstTime() { m_AxisChoice = AxisChoice::Time; }
		void PlotAgainstChannel(std::string channel)
		{
			m_AxisChoice = AxisChoice::Channel;
			m_AxisChannel = std::move(channel);
		}
		// The lap shown: by default (FindDefaultTelemetryLap), the whole session, or a lap of the shown session by the row of
		// its mark (the default again once the session no longer has it).
		void ShowDefaultLap() { m_LapChoice = LapChoice::Default; }
		void ShowSession() { m_LapChoice = LapChoice::Session; }
		void ShowLap(uint64_t startRow)
		{
			m_LapChoice = LapChoice::Lap;
			m_LapRow = startRow;
		}
		// The lap overlaid: by default (FindDefaultTelemetryReference), none, or a lap of any session held (the default again
		// once no session holds it).
		void OverlayDefault() { m_ReferenceChoice = ReferenceChoice::Default; }
		void OverlayNone() { m_ReferenceChoice = ReferenceChoice::None; }
		void OverlayLap(TelemetryLapKey lap)
		{
			m_ReferenceChoice = ReferenceChoice::Lap;
			m_ReferenceLap = lap;
		}

	private:
		enum class AxisChoice : uint8_t
		{
			Default = 0,
			Time,
			Channel
		};

		enum class LapChoice : uint8_t
		{
			Default = 0,
			Session,
			Lap
		};

		enum class ReferenceChoice : uint8_t
		{
			Default = 0,
			None,
			Lap
		};

		// What a toolbar button asked for, done after the draw (Stop and Resume change the sessions the draw reads).
		enum class ToolbarAction : uint8_t
		{
			None = 0,
			Pause,
			Record
		};

		// What a lane's series was reduced from: it is reduced again only when one of these changes.
		struct SeriesKey
		{
			uint32_t Session = 0;
			std::string Channel;
			TelemetryAxis Axis = TelemetryAxis::Time;
			std::string Distance;
			uint64_t FirstRow = 0;
			uint64_t EndRow = 0;
			double Start = 0.0;
			double End = 0.0;
			uint32_t Columns = 0;

			bool operator==(SeriesKey const&) const = default;
		};

		struct CachedSeries
		{
			SeriesKey Key;
			TelemetrySeries Series;
		};

		// A span chosen for drawing, with its session and where its rows lie along the axis.
		struct Plotted
		{
			TelemetrySession const* Session = nullptr;
			TelemetrySpan Span;
			TelemetryChannel const* Distance = nullptr;
			TelemetryAxisRows Rows;
		};

		void DrawContents(EditorTelemetry& telemetry, Scene* scene, std::filesystem::path const& dialogDirectory);
		void DrawToolbar(EditorTelemetry& telemetry, TelemetrySession const* session, std::filesystem::path const& dialogDirectory);
		TelemetrySession const* ChooseSession();
		void DrawSessionControls(TelemetrySession const& session, std::vector<TelemetrySpan> const& laps,
		                         std::optional<TelemetrySpan> const& shown, std::optional<TelemetrySpan> const& reference);
		void DrawChannelList(TelemetrySession const& session, Scene* scene, std::vector<std::string> const& chosen);
		void DrawLanes(Plotted const& shown, std::optional<Plotted> const& reference, std::vector<std::string> const& chosen, Scene* scene);
		// The series of a channel over a span, kept in m_Series[slot] (sized by the caller first) and reduced again only when
		// what it is reduced from changed.
		TelemetrySeries const& GetSeries(size_t slot, Plotted const& plotted, TelemetryChannel const& channel, double start, double end,
		                                 uint32_t columns);
		void ApplyToolbarAction(EditorTelemetry& telemetry);

		DrawRecord m_Record;
		ToolbarAction m_Action = ToolbarAction::None;
		std::optional<uint32_t> m_Session;
		std::vector<std::string> m_Channels;
		// Whether m_Channels is a choice (else the panel chooses).
		bool m_ChannelsChosen = false;
		AxisChoice m_AxisChoice = AxisChoice::Default;
		std::string m_AxisChannel;
		LapChoice m_LapChoice = LapChoice::Default;
		uint64_t m_LapRow = 0;
		ReferenceChoice m_ReferenceChoice = ReferenceChoice::Default;
		TelemetryLapKey m_ReferenceLap;
		// The reduced series of the lanes drawn last: two slots per lane (the shown span's, the overlaid one's).
		std::vector<CachedSeries> m_Series;
	};
}
