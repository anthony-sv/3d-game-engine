#pragma once

#include "Strada/Core/Telemetry.h"
#include "Strada/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What the Telemetry panel plots, worked out without ImGui (Docs/Architecture.md §4.5, "The Telemetry panel"): the spans of
// rows it shows (a lap, a lap in progress, or the session), where each row lies along the axis (its time from the span's
// first row, or its distance), and each channel reduced to at most two points per pixel column of a lane. Pure functions
// of the sessions they are given; main thread or any thread that holds the sessions still.
namespace Strada
{
	class Scene;

	// What the rows are plotted against.
	enum class TelemetryAxis : uint8_t
	{
		// Seconds from the span's first row.
		Time = 0,
		// A distance channel's value at each row (Ride.Distance: metres along the lap).
		Distance
	};

	// The distance channel the panel plots against by default when a session has it.
	inline constexpr std::string_view DefaultTelemetryDistanceChannel = "Ride.Distance";
	// How much of a live session the panel plots when it shows the session rather than a lap: its last 30 seconds.
	inline constexpr double LiveTelemetryWindowSeconds = 30.0;

	// Rows of one session the panel plots: a lap (from the mark that begins it to the row before the one that ends it), the
	// lap in progress (from its mark to the session's last row), or the session (all of it, or a live one's last rows).
	struct TelemetrySpan
	{
		uint32_t Session = 0;
		uint64_t FirstRow = 0;
		// One past the last row.
		uint64_t EndRow = 0;
		// The lap's label ("Lap 2"), empty for the session.
		std::string Label;
		uint32_t Number = 0;
		bool InProgress = false;
		// The lap's time (TelemetryLap::Seconds; for the lap in progress, its rows so far), 0 for the session.
		double Seconds = 0.0;

		bool IsLap() const { return !Label.empty(); }
	};

	// A lap of a session, by its session and the row of the mark that begins it, which tell it apart from every other lap
	// while the session is held.
	struct TelemetryLapKey
	{
		uint32_t Session = 0;
		uint64_t StartRow = 0;

		bool operator==(TelemetryLapKey const&) const = default;
	};

	// The entity an engine channel names (<Subsystem>.<entity ID>.<Quantity>, the ID in decimal), when its name has that
	// shape; none otherwise.
	[[nodiscard]] std::optional<UUID> ParseTelemetryChannelEntity(std::string_view name);
	// How the panel names a channel: an engine channel of an entity the scene has as <Subsystem>.<entity name>.<Quantity>,
	// every other channel by its name. scene may be null.
	[[nodiscard]] std::string DescribeTelemetryChannel(std::string_view name, Scene* scene);

	// The session's laps in row order (FindTelemetryLaps), then its lap in progress (FindTelemetryLapInProgress), as spans.
	[[nodiscard]] std::vector<TelemetrySpan> ListTelemetryLapSpans(TelemetrySession const& session);
	// The span of a lap, by its key, when its session still holds it.
	[[nodiscard]] std::optional<TelemetrySpan> FindTelemetryLapSpan(std::deque<TelemetrySession> const& sessions, TelemetryLapKey key);
	// The span of the session itself: every row, or, while it is live, its last LiveTelemetryWindowSeconds.
	[[nodiscard]] TelemetrySpan GetTelemetrySessionSpan(TelemetrySession const& session);
	// The lap the panel shows by default: the lap in progress while the session is live, else its last lap; none without
	// laps (the panel then shows the session).
	[[nodiscard]] std::optional<TelemetrySpan> FindDefaultTelemetryLap(TelemetrySession const& session);
	// The lap the panel overlays by default on `shown` (a span of `session`, a lap or not): the best of the session's
	// finished laps other than the shown one; when it has none, the best lap of the newest other session held that has a
	// finished lap; none otherwise.
	[[nodiscard]] std::optional<TelemetrySpan> FindDefaultTelemetryReference(std::deque<TelemetrySession> const& sessions,
	                                                                         TelemetrySession const& session, TelemetrySpan const& shown);

	// Where the rows of a span lie along an axis. By time, each row at its seconds from the span's first row. By distance,
	// each row at its distance channel's value; a row without one is left out, and so are rows outside the span's own run at
	// either end: a first row that still carries the run before (a distance above the next row's) and a last one that has
	// already wrapped (a distance below the row before), so that a lap is drawn from where it starts to where it ends.
	struct TelemetryAxisRows
	{
		// The rows placed: from FirstRow to one before EndRow (by distance, those with a distance).
		uint64_t FirstRow = 0;
		uint64_t EndRow = 0;
		// The smallest and largest position of the rows placed; Start > End when no row is placed.
		double Start = 0.0;
		double End = -1.0;

		bool IsEmpty() const { return !(End >= Start); }
	};

	// distance: the session's distance channel for TelemetryAxis::Distance (null leaves every row out), ignored by time.
	[[nodiscard]] TelemetryAxisRows PlaceTelemetryRows(TelemetrySession const& session, TelemetrySpan const& span, TelemetryAxis axis,
	                                                   TelemetryChannel const* distance);
	// The position of a row placed (one of rows' range): its seconds from the span's first row, or its distance; none for a
	// row by distance without a distance.
	[[nodiscard]] std::optional<double> GetTelemetryRowPosition(TelemetrySession const& session, TelemetrySpan const& span,
	                                                            TelemetryAxis axis, TelemetryChannel const* distance, uint64_t row);

	// A channel over the rows placed, reduced for a lane of `columns` pixel columns spanning positions start to end: each
	// column the rows fall in gives at most two points, its smallest and largest value in the order the rows had them, at
	// the column's middle (X in columns from the lane's left edge, Y the value). The line runs from value to value over rows
	// without one (a channel recorded on some steps only, as a gear recorded when it changes, reads as a line, never as
	// dots); by distance, a row that lies before the row placed above it (the distance wrapped) begins a new segment.
	struct TelemetrySeries
	{
		std::vector<glm::vec2> Points;
		// The index in Points of each segment's first point, in order; empty when there are no points.
		std::vector<uint32_t> Segments;
		// Rows with a value and a position.
		uint64_t Rows = 0;
		// The smallest and largest value of those rows (0 when there are none).
		float Minimum = 0.0f;
		float Maximum = 0.0f;
	};

	struct TelemetrySeriesRequest
	{
		TelemetrySession const* Session = nullptr;
		TelemetrySpan Span;
		TelemetryChannel const* Channel = nullptr;
		TelemetryAxis Axis = TelemetryAxis::Time;
		TelemetryChannel const* Distance = nullptr;
		TelemetryAxisRows Rows;
		// The positions at the lane's left and right edges, Start < End.
		double Start = 0.0;
		double End = 1.0;
		uint32_t Columns = 1;
	};

	[[nodiscard]] TelemetrySeries ReduceTelemetrySeries(TelemetrySeriesRequest const& request);

	// Labels of a lap for the panel's lists: "Lap 2 (12.400 s)", "Lap 4 (in progress)" or "Session".
	[[nodiscard]] std::string DescribeTelemetrySpan(TelemetrySpan const& span);
}
