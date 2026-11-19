#include "Editor/TelemetryPlot.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Strada
{
	namespace
	{
		// The longest decimal UUID of an engine channel's name (<Subsystem>.<entity ID>.<Quantity>).
		constexpr size_t MaxEntityDigits = 20;

		TelemetrySpan MakeLapSpan(TelemetrySession const& session, TelemetryLap const& lap, bool inProgress)
		{
			TelemetrySpan span;
			span.Session = session.Id;
			span.FirstRow = lap.StartRow;
			// A finished lap ends before the mark that ends it; the lap in progress runs to the session's last row.
			span.EndRow = inProgress ? session.Rows : lap.EndRow;
			span.Label = lap.Label;
			span.Number = lap.Number;
			span.InProgress = inProgress;
			span.Seconds = lap.Seconds;
			return span;
		}

		// The first row from `row` on (before `end`) that has a value, or `end`.
		uint64_t FindValuedRow(TelemetryChannel const& channel, uint64_t row, uint64_t end)
		{
			while (row < end && !GetTelemetryValue(channel, row))
			{
				++row;
			}
			return row;
		}

		// The last row before `end` (from `first` on) that has a value, or `end` when none has.
		uint64_t FindLastValuedRow(TelemetryChannel const& channel, uint64_t first, uint64_t end)
		{
			for (uint64_t row = end; row > first; --row)
			{
				if (GetTelemetryValue(channel, row - 1))
				{
					return row - 1;
				}
			}
			return end;
		}
	}

	std::optional<UUID> ParseTelemetryChannelEntity(std::string_view name)
	{
		size_t const first = name.find('.');
		if (first == std::string_view::npos || first == 0)
		{
			return std::nullopt;
		}
		size_t const second = name.find('.', first + 1);
		if (second == std::string_view::npos || second + 1 >= name.size())
		{
			return std::nullopt;
		}
		std::string_view const digits = name.substr(first + 1, second - first - 1);
		bool const decimal = !digits.empty() && digits.size() <= MaxEntityDigits &&
		                     std::all_of(digits.begin(), digits.end(),
		                                 [](char character)
		                                 {
											 return character >= '0' && character <= '9';
										 });
		if (!decimal)
		{
			return std::nullopt;
		}
		uint64_t value = 0;
		std::from_chars_result const parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
		if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size() || value == 0)
		{
			return std::nullopt;
		}
		return UUID(value);
	}

	std::string DescribeTelemetryChannel(std::string_view name, Scene* scene)
	{
		std::optional<UUID> const id = ParseTelemetryChannelEntity(name);
		if (!id || scene == nullptr)
		{
			return std::string(name);
		}
		Entity const entity = scene->GetEntityByUUID(*id);
		if (!entity)
		{
			return std::string(name);
		}
		size_t const first = name.find('.');
		size_t const second = name.find('.', first + 1);
		return fmt::format("{}.{}.{}", name.substr(0, first), entity.GetName(), name.substr(second + 1));
	}

	std::vector<TelemetrySpan> ListTelemetryLapSpans(TelemetrySession const& session)
	{
		std::vector<TelemetrySpan> spans;
		for (TelemetryLap const& lap : FindTelemetryLaps(session))
		{
			spans.push_back(MakeLapSpan(session, lap, false));
		}
		if (std::optional<TelemetryLap> const progress = FindTelemetryLapInProgress(session))
		{
			spans.push_back(MakeLapSpan(session, *progress, true));
		}
		return spans;
	}

	std::optional<TelemetrySpan> FindTelemetryLapSpan(std::deque<TelemetrySession> const& sessions, TelemetryLapKey key)
	{
		for (TelemetrySession const& session : sessions)
		{
			if (session.Id != key.Session)
			{
				continue;
			}
			for (TelemetrySpan& span : ListTelemetryLapSpans(session))
			{
				if (span.FirstRow == key.StartRow)
				{
					return std::move(span);
				}
			}
			return std::nullopt;
		}
		return std::nullopt;
	}

	TelemetrySpan GetTelemetrySessionSpan(TelemetrySession const& session)
	{
		TelemetrySpan span;
		span.Session = session.Id;
		span.EndRow = session.Rows;
		if (session.Live && session.StepSeconds > 0.0f)
		{
			double const window = std::ceil(LiveTelemetryWindowSeconds / static_cast<double>(session.StepSeconds));
			uint64_t const rows = static_cast<uint64_t>(std::max(window, 1.0));
			span.FirstRow = session.Rows > rows ? session.Rows - rows : 0;
		}
		return span;
	}

	std::optional<TelemetrySpan> FindDefaultTelemetryLap(TelemetrySession const& session)
	{
		std::vector<TelemetrySpan> spans = ListTelemetryLapSpans(session);
		if (spans.empty())
		{
			return std::nullopt;
		}
		// The lap in progress is the last of the list when there is one; a session that is not live shows its last finished
		// lap instead.
		if (spans.back().InProgress && !session.Live)
		{
			spans.pop_back();
			if (spans.empty())
			{
				return std::nullopt;
			}
		}
		return std::move(spans.back());
	}

	std::optional<TelemetrySpan> FindDefaultTelemetryReference(std::deque<TelemetrySession> const& sessions,
	                                                           TelemetrySession const& session, TelemetrySpan const& shown)
	{
		std::vector<TelemetryLap> candidates;
		for (TelemetryLap& lap : FindTelemetryLaps(session))
		{
			if (!shown.IsLap() || shown.InProgress || lap.StartRow != shown.FirstRow)
			{
				candidates.push_back(std::move(lap));
			}
		}
		if (std::optional<size_t> const best = FindBestTelemetryLap(candidates))
		{
			return MakeLapSpan(session, candidates[*best], false);
		}
		for (auto other = sessions.rbegin(); other != sessions.rend(); ++other)
		{
			if (other->Id == session.Id)
			{
				continue;
			}
			std::vector<TelemetryLap> const laps = FindTelemetryLaps(*other);
			if (std::optional<size_t> const best = FindBestTelemetryLap(laps))
			{
				return MakeLapSpan(*other, laps[*best], false);
			}
		}
		return std::nullopt;
	}

	TelemetryAxisRows PlaceTelemetryRows(TelemetrySession const& session, TelemetrySpan const& span, TelemetryAxis axis,
	                                     TelemetryChannel const* distance)
	{
		TelemetryAxisRows rows;
		uint64_t const end = std::min(span.EndRow, session.Rows);
		if (span.FirstRow >= end)
		{
			return rows;
		}
		if (axis == TelemetryAxis::Time)
		{
			rows.FirstRow = span.FirstRow;
			rows.EndRow = end;
			rows.Start = 0.0;
			rows.End = static_cast<double>(end - 1 - span.FirstRow) * static_cast<double>(session.StepSeconds);
			return rows;
		}
		if (distance == nullptr)
		{
			return rows;
		}
		uint64_t first = FindValuedRow(*distance, span.FirstRow, end);
		uint64_t last = FindLastValuedRow(*distance, span.FirstRow, end);
		if (first >= end || last >= end)
		{
			return rows;
		}
		// The run before the span's own: its first rows carry distances above the rows after them.
		while (first < last)
		{
			uint64_t const next = FindValuedRow(*distance, first + 1, last + 1);
			if (!(*GetTelemetryValue(*distance, first) > *GetTelemetryValue(*distance, next)))
			{
				break;
			}
			first = next;
		}
		// The run after it: its last rows have wrapped below the rows before them.
		while (last > first)
		{
			// The valued row before the last (the first row is one, so there is such a row).
			uint64_t const previous = FindLastValuedRow(*distance, first, last);
			if (!(*GetTelemetryValue(*distance, last) < *GetTelemetryValue(*distance, previous)))
			{
				break;
			}
			last = previous;
		}
		rows.FirstRow = first;
		rows.EndRow = last + 1;
		rows.Start = static_cast<double>(*GetTelemetryValue(*distance, first));
		rows.End = rows.Start;
		for (uint64_t row = first; row <= last; ++row)
		{
			if (std::optional<float> const value = GetTelemetryValue(*distance, row))
			{
				rows.Start = std::min(rows.Start, static_cast<double>(*value));
				rows.End = std::max(rows.End, static_cast<double>(*value));
			}
		}
		return rows;
	}

	std::optional<double> GetTelemetryRowPosition(TelemetrySession const& session, TelemetrySpan const& span, TelemetryAxis axis,
	                                              TelemetryChannel const* distance, uint64_t row)
	{
		if (axis == TelemetryAxis::Time)
		{
			if (row < span.FirstRow)
			{
				return std::nullopt;
			}
			return static_cast<double>(row - span.FirstRow) * static_cast<double>(session.StepSeconds);
		}
		if (distance == nullptr)
		{
			return std::nullopt;
		}
		std::optional<float> const value = GetTelemetryValue(*distance, row);
		return value ? std::optional<double>(static_cast<double>(*value)) : std::nullopt;
	}

	TelemetrySeries ReduceTelemetrySeries(TelemetrySeriesRequest const& request)
	{
		TelemetrySeries series;
		if (request.Session == nullptr || request.Channel == nullptr || request.Rows.IsEmpty() || request.Columns == 0 ||
		    !(request.End > request.Start))
		{
			return series;
		}
		double const scale = static_cast<double>(request.Columns) / (request.End - request.Start);
		int64_t const lastColumn = static_cast<int64_t>(request.Columns) - 1;

		// The column being gathered: its smallest and largest value and which came first.
		bool gathering = false;
		bool startsSegment = true;
		int64_t column = 0;
		float minimum = 0.0f;
		float maximum = 0.0f;
		bool minimumFirst = true;
		auto const flush = [&series, &gathering, &startsSegment, &column, &minimum, &maximum, &minimumFirst]
		{
			if (!gathering)
			{
				return;
			}
			if (startsSegment)
			{
				series.Segments.push_back(static_cast<uint32_t>(series.Points.size()));
			}
			float const x = static_cast<float>(column) + 0.5f;
			float const firstValue = minimumFirst ? minimum : maximum;
			float const secondValue = minimumFirst ? maximum : minimum;
			series.Points.emplace_back(x, firstValue);
			if (secondValue != firstValue)
			{
				series.Points.emplace_back(x, secondValue);
			}
			gathering = false;
			startsSegment = false;
		};

		std::optional<double> previous;
		for (uint64_t row = request.Rows.FirstRow; row < request.Rows.EndRow; ++row)
		{
			std::optional<float> const value = GetTelemetryValue(*request.Channel, row);
			if (!value)
			{
				continue;
			}
			std::optional<double> const position =
				GetTelemetryRowPosition(*request.Session, request.Span, request.Axis, request.Distance, row);
			if (!position)
			{
				continue;
			}
			bool const wrapped = request.Axis == TelemetryAxis::Distance && previous && *position < *previous;
			previous = position;
			int64_t const at = std::clamp(static_cast<int64_t>(std::floor((*position - request.Start) * scale)), int64_t{0}, lastColumn);
			if (gathering && (wrapped || at != column))
			{
				flush();
			}
			if (wrapped)
			{
				startsSegment = true;
			}
			if (!gathering)
			{
				gathering = true;
				column = at;
				minimum = *value;
				maximum = *value;
				minimumFirst = true;
			}
			else
			{
				if (*value < minimum)
				{
					minimum = *value;
					minimumFirst = false;
				}
				if (*value > maximum)
				{
					maximum = *value;
					minimumFirst = true;
				}
			}
			series.Minimum = series.Rows == 0 ? *value : std::min(series.Minimum, *value);
			series.Maximum = series.Rows == 0 ? *value : std::max(series.Maximum, *value);
			++series.Rows;
		}
		flush();
		return series;
	}

	std::string DescribeTelemetrySpan(TelemetrySpan const& span)
	{
		if (!span.IsLap())
		{
			return "Session";
		}
		if (span.InProgress)
		{
			return fmt::format("{} (in progress)", span.Label);
		}
		return fmt::format("{} ({:.3f} s)", span.Label, span.Seconds);
	}
}
