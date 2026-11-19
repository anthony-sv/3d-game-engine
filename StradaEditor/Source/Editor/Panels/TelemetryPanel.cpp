#include "Editor/Panels/TelemetryPanel.h"

#include "Editor/EditorTelemetry.h"
#include "Editor/FileDialogs.h"
#include "Editor/UI/CurveEditor.h"
#include "Editor/UI/CurveView.h"
#include "Editor/UI/EditorUI.h"

#include "Strada/Core/Telemetry.h"
#include "Strada/Serialization/TelemetryCsv.h"

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// Pixels between two tick labels of the axis at least.
		constexpr float TickSpacing = 80.0f;
		// The channel list's width, in font sizes.
		constexpr float ChannelListEms = 16.0f;
		// A lane's height at least, in text lines.
		constexpr float MinLaneLines = 3.0f;
		// The part of a lane's height kept free above and below the values it spans.
		constexpr float LaneMargin = 0.08f;
		// The width of a combo of the session's controls, in font sizes.
		constexpr float ComboEms = 11.0f;

		std::array<FileDialogFilter, 1> const& GetCsvFilters()
		{
			static std::array<FileDialogFilter, 1> const s_Filters = {{{"Telemetry CSV", "csv"}}};
			return s_Filters;
		}

		ImU32 GetColor(ImGuiCol color, float alpha = 1.0f)
		{
			return ImGui::GetColorU32(color, alpha);
		}

		// The shown lap's line, and the overlaid lap's beneath it.
		ImU32 GetShownColor()
		{
			return GetColor(ImGuiCol_PlotLinesHovered);
		}

		ImU32 GetReferenceColor()
		{
			return GetColor(ImGuiCol_PlotHistogram, 0.85f);
		}

		std::string FormatBytes(uint64_t bytes)
		{
			constexpr double Kibibyte = 1024.0;
			if (static_cast<double>(bytes) < Kibibyte * Kibibyte)
			{
				return fmt::format("{:.0f} KiB", static_cast<double>(bytes) / Kibibyte);
			}
			return fmt::format("{:.1f} MiB", static_cast<double>(bytes) / (Kibibyte * Kibibyte));
		}

		std::string DescribeSession(TelemetrySession const& session)
		{
			return fmt::format("Session {} ({} rows{})", session.Id, session.Rows, session.Live ? ", live" : "");
		}

		// A lap of any session held, as the overlay's list names it.
		std::string DescribeSessionLap(TelemetrySpan const& span)
		{
			return fmt::format("Session {}: {}", span.Session, DescribeTelemetrySpan(span));
		}

		bool IsSameSpan(TelemetrySpan const& first, TelemetrySpan const& second)
		{
			return first.Session == second.Session && first.FirstRow == second.FirstRow && first.IsLap() == second.IsLap();
		}

		TelemetryPanel::DrawRecord::Span MakeSpanRecord(TelemetrySpan const& span, TelemetryAxisRows const& rows)
		{
			TelemetryPanel::DrawRecord::Span record;
			record.Session = span.Session;
			record.Label = span.Label;
			record.InProgress = span.InProgress;
			record.Seconds = span.Seconds;
			record.FirstRow = span.FirstRow;
			record.EndRow = span.EndRow;
			record.PlacedFirstRow = rows.IsEmpty() ? 0 : rows.FirstRow;
			record.PlacedEndRow = rows.IsEmpty() ? 0 : rows.EndRow;
			return record;
		}

		// The value range a lane spans: the values of its series with a margin, never empty.
		std::pair<float, float> GetLaneRange(TelemetrySeries const& shown, TelemetrySeries const* reference)
		{
			bool any = false;
			float minimum = 0.0f;
			float maximum = 0.0f;
			for (TelemetrySeries const* series : {&shown, reference})
			{
				if (series == nullptr || series->Rows == 0)
				{
					continue;
				}
				minimum = any ? std::min(minimum, series->Minimum) : series->Minimum;
				maximum = any ? std::max(maximum, series->Maximum) : series->Maximum;
				any = true;
			}
			if (!any)
			{
				return {0.0f, 1.0f};
			}
			if (!(maximum > minimum))
			{
				float const half = std::max(std::abs(minimum) * 0.05f, 0.5f);
				return {minimum - half, maximum + half};
			}
			return {minimum, maximum};
		}
	}

	TelemetryPanel::DrawRecord::Span::Span() = default;
	TelemetryPanel::DrawRecord::Series::Series() = default;

	void TelemetryPanel::OnImGuiRender(EditorTelemetry& telemetry, bool& open, Scene* scene, std::filesystem::path const& dialogDirectory)
	{
		m_Record = DrawRecord();
		if (!ImGui::Begin(WindowName, &open))
		{
			ImGui::End();
			return;
		}
		m_Record.Drawn = true;
		DrawContents(telemetry, scene, dialogDirectory);
		ImGui::End();
		// Pause and Record stop and start the recording, which changes the sessions the draw read: done once it is over.
		ApplyToolbarAction(telemetry);
	}

	TelemetrySession const* TelemetryPanel::ChooseSession()
	{
		std::deque<TelemetrySession> const& sessions = Telemetry::GetSessions();
		if (sessions.empty())
		{
			return nullptr;
		}
		if (m_Session)
		{
			if (TelemetrySession const* const chosen = Telemetry::FindSession(*m_Session))
			{
				return chosen;
			}
			// A session dropped from the history: the panel follows the newest again.
			m_Session.reset();
		}
		return &sessions.back();
	}

	void TelemetryPanel::DrawContents(EditorTelemetry& telemetry, Scene* scene, std::filesystem::path const& dialogDirectory)
	{
		// Valid until the recorder steps, starts or stops, none of which happens while the panel draws.
		TelemetrySession const* const session = ChooseSession();
		DrawToolbar(telemetry, session, dialogDirectory);
		if (session == nullptr)
		{
			ImGui::TextDisabled("No session recorded yet: play a scene while the panel records.");
			return;
		}
		m_Record.Session = session->Id;
		m_Record.Live = session->Live;
		m_Record.Rows = session->Rows;
		std::deque<TelemetrySession> const& sessions = Telemetry::GetSessions();

		// The lap shown, the session when there is none.
		std::vector<TelemetrySpan> const laps = ListTelemetryLapSpans(*session);
		std::optional<TelemetrySpan> shown;
		if (m_LapChoice == LapChoice::Lap)
		{
			auto const found = std::find_if(laps.begin(), laps.end(),
			                                [this](TelemetrySpan const& lap)
			                                {
												return lap.FirstRow == m_LapRow;
											});
			if (found != laps.end())
			{
				shown = *found;
			}
			else
			{
				m_LapChoice = LapChoice::Default;
			}
		}
		if (m_LapChoice == LapChoice::Default)
		{
			shown = FindDefaultTelemetryLap(*session);
		}
		TelemetrySpan const shownSpan = shown ? *shown : GetTelemetrySessionSpan(*session);

		// The lap overlaid, never the shown span itself; a session shown whole has none by default.
		std::optional<TelemetrySpan> reference;
		if (m_ReferenceChoice == ReferenceChoice::Lap)
		{
			reference = FindTelemetryLapSpan(sessions, m_ReferenceLap);
			if (!reference)
			{
				m_ReferenceChoice = ReferenceChoice::Default;
			}
		}
		if (m_ReferenceChoice == ReferenceChoice::Default && shownSpan.IsLap())
		{
			reference = FindDefaultTelemetryReference(sessions, *session, shownSpan);
		}
		if (reference && IsSameSpan(*reference, shownSpan))
		{
			reference.reset();
		}

		// What the rows are plotted against.
		TelemetryChannel const* distance = nullptr;
		if (m_AxisChoice == AxisChoice::Default)
		{
			distance = FindTelemetryChannel(*session, DefaultTelemetryDistanceChannel);
		}
		else if (m_AxisChoice == AxisChoice::Channel)
		{
			distance = FindTelemetryChannel(*session, m_AxisChannel);
		}
		TelemetryAxis const axis = distance != nullptr ? TelemetryAxis::Distance : TelemetryAxis::Time;
		m_Record.Axis = axis;
		m_Record.DistanceChannel = distance != nullptr ? distance->Name : std::string();

		DrawSessionControls(*session, laps, shown, reference);

		Plotted shownPlotted;
		shownPlotted.Session = session;
		shownPlotted.Span = shownSpan;
		shownPlotted.Distance = distance;
		shownPlotted.Rows = PlaceTelemetryRows(*session, shownSpan, axis, distance);
		m_Record.Shown = MakeSpanRecord(shownSpan, shownPlotted.Rows);
		std::optional<Plotted> referencePlotted;
		if (reference)
		{
			Plotted plotted;
			plotted.Session = Telemetry::FindSession(reference->Session);
			plotted.Span = *reference;
			plotted.Distance =
				plotted.Session != nullptr && distance != nullptr ? FindTelemetryChannel(*plotted.Session, distance->Name) : nullptr;
			if (plotted.Session != nullptr)
			{
				plotted.Rows = PlaceTelemetryRows(*plotted.Session, plotted.Span, axis, plotted.Distance);
				m_Record.Reference = MakeSpanRecord(plotted.Span, plotted.Rows);
				referencePlotted = plotted;
			}
		}

		// The channels plotted: those chosen that the session has; the panel's own choice (its first ones but the axis's
		// distance) when none was made, or when the session has none of those chosen.
		std::vector<std::string> chosen;
		for (std::string const& name : m_Channels)
		{
			if (chosen.size() < MaxLanes && FindTelemetryChannel(*session, name) != nullptr &&
			    std::find(chosen.begin(), chosen.end(), name) == chosen.end())
			{
				chosen.push_back(name);
			}
		}
		if (!m_ChannelsChosen || (chosen.empty() && !m_Channels.empty()))
		{
			chosen.clear();
			for (TelemetryChannel const& channel : session->Channels)
			{
				if (chosen.size() < DefaultLanes && &channel != distance)
				{
					chosen.push_back(channel.Name);
				}
			}
		}

		float const listWidth = ImGui::GetFontSize() * ChannelListEms;
		if (ImGui::BeginChild("##Channels", ImVec2(listWidth, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX))
		{
			DrawChannelList(*session, scene, chosen);
		}
		ImGui::EndChild();
		ImGui::SameLine();
		// Lanes that do not fit at their least height scroll.
		if (ImGui::BeginChild("##Lanes", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders))
		{
			DrawLanes(shownPlotted, referencePlotted, chosen, scene);
		}
		ImGui::EndChild();
	}

	void TelemetryPanel::DrawToolbar(EditorTelemetry& telemetry, TelemetrySession const* session,
	                                 std::filesystem::path const& dialogDirectory)
	{
		TelemetryStatus const status = Telemetry::GetStatus();
		m_Record.Recording = status.Recording;
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
		uint64_t const dropped = status.DroppedValues + status.DroppedMarks + status.DroppedSteps;
		if (status.Recording)
		{
			m_Record.Status = fmt::format("Recording{}: {} sessions held, {}{}", telemetry.IsCommandRecording() ? " (telemetry.start)" : "",
			                              status.SessionsHeld, FormatBytes(status.BytesHeld),
			                              dropped > 0 ? fmt::format(", {} dropped", dropped) : std::string());
		}
		else if (!telemetry.GetRecordingError().empty())
		{
			m_Record.Status = "Not recording: " + telemetry.GetRecordingError();
		}
		else
		{
			m_Record.Status = fmt::format("Paused: {} sessions held", status.SessionsHeld);
		}
		ImGui::TextUnformatted(m_Record.Status.c_str());
		if (session != nullptr && session->Rows > 0)
		{
			ImGui::SameLine();
			if (ImGui::Button("Export CSV..."))
			{
				Result<std::optional<std::filesystem::path>> const chosen =
					FileDialogs::SaveFile(GetCsvFilters(), dialogDirectory, fmt::format("Session{}.csv", session->Id));
				UI::ReportFailure(chosen, "Choosing where to export the telemetry session");
				if (chosen && chosen.GetValue())
				{
					std::filesystem::path path = *chosen.GetValue();
					if (path.extension() != ".csv")
					{
						path += ".csv";
					}
					UI::ReportFailure(TelemetryCsvWriter::Save(*session, path), "Exporting the telemetry session");
				}
			}
		}
	}

	void TelemetryPanel::DrawSessionControls(TelemetrySession const& session, std::vector<TelemetrySpan> const& laps,
	                                         std::optional<TelemetrySpan> const& shown, std::optional<TelemetrySpan> const& reference)
	{
		std::deque<TelemetrySession> const& sessions = Telemetry::GetSessions();
		float const comboWidth = ImGui::GetFontSize() * ComboEms;
		std::optional<size_t> const best = [&laps]() -> std::optional<size_t>
		{
			// The lap in progress is the list's last, so the finished laps keep their places in it.
			std::vector<TelemetryLap> finished;
			for (TelemetrySpan const& candidate : laps)
			{
				if (!candidate.InProgress)
				{
					finished.push_back(
						TelemetryLap{candidate.Label, candidate.Number, candidate.FirstRow, candidate.EndRow, candidate.Seconds});
				}
			}
			return FindBestTelemetryLap(finished);
		}();

		ImGui::SetNextItemWidth(comboWidth);
		std::string const sessionPreview =
			m_Session ? fmt::format("Session {}", session.Id) : fmt::format("Newest: Session {}", session.Id);
		if (ImGui::BeginCombo("##Session", sessionPreview.c_str()))
		{
			if (ImGui::Selectable("Newest", !m_Session))
			{
				m_Session.reset();
			}
			for (auto held = sessions.rbegin(); held != sessions.rend(); ++held)
			{
				ImGui::PushID(static_cast<int>(held->Id));
				if (ImGui::Selectable(DescribeSession(*held).c_str(), m_Session == held->Id))
				{
					m_Session = held->Id;
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		ImGui::SetItemTooltip("The session shown: one per play recorded");

		ImGui::SameLine();
		ImGui::SetNextItemWidth(comboWidth);
		std::string const lapPreview = shown ? DescribeTelemetrySpan(*shown) : "Whole session";
		if (ImGui::BeginCombo("##Lap", lapPreview.c_str()))
		{
			if (ImGui::Selectable("Default", m_LapChoice == LapChoice::Default))
			{
				ShowDefaultLap();
			}
			if (ImGui::Selectable("Whole session", m_LapChoice == LapChoice::Session))
			{
				ShowSession();
			}
			for (size_t index = 0; index < laps.size(); ++index)
			{
				TelemetrySpan const& lap = laps[index];
				ImGui::PushID(static_cast<int>(index));
				std::string const label = DescribeTelemetrySpan(lap) + (best && *best == index ? " best" : "");
				if (ImGui::Selectable(label.c_str(), m_LapChoice == LapChoice::Lap && m_LapRow == lap.FirstRow))
				{
					ShowLap(lap.FirstRow);
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		ImGui::SetItemTooltip("The lap shown, or the whole session");

		ImGui::SameLine();
		ImGui::TextUnformatted("over");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(comboWidth);
		// A lap of the shown session by its label alone, one of another session with that session's number.
		std::string const referencePreview = !reference                         ? std::string("Nothing")
		                                     : reference->Session == session.Id ? DescribeTelemetrySpan(*reference)
		                                                                        : DescribeSessionLap(*reference);
		if (ImGui::BeginCombo("##Reference", referencePreview.c_str()))
		{
			if (ImGui::Selectable("Default", m_ReferenceChoice == ReferenceChoice::Default))
			{
				OverlayDefault();
			}
			if (ImGui::Selectable("Nothing", m_ReferenceChoice == ReferenceChoice::None))
			{
				OverlayNone();
			}
			for (auto held = sessions.rbegin(); held != sessions.rend(); ++held)
			{
				ImGui::PushID(static_cast<int>(held->Id));
				std::vector<TelemetrySpan> const heldLaps = ListTelemetryLapSpans(*held);
				for (size_t index = 0; index < heldLaps.size(); ++index)
				{
					TelemetrySpan const& lap = heldLaps[index];
					ImGui::PushID(static_cast<int>(index));
					TelemetryLapKey const key{lap.Session, lap.FirstRow};
					if (ImGui::Selectable(DescribeSessionLap(lap).c_str(),
					                      m_ReferenceChoice == ReferenceChoice::Lap && m_ReferenceLap == key))
					{
						OverlayLap(key);
					}
					ImGui::PopID();
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		ImGui::SetItemTooltip("The lap drawn beneath the shown one: by default the best other lap");

		ImGui::SameLine();
		ImGui::TextUnformatted("against");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(comboWidth);
		std::string const axisPreview = m_Record.Axis == TelemetryAxis::Distance ? m_Record.DistanceChannel : std::string("Time");
		if (ImGui::BeginCombo("##Axis", axisPreview.c_str()))
		{
			if (ImGui::Selectable("Default", m_AxisChoice == AxisChoice::Default))
			{
				PlotAgainstDefault();
			}
			if (ImGui::Selectable("Time", m_AxisChoice == AxisChoice::Time))
			{
				PlotAgainstTime();
			}
			for (TelemetryChannel const& channel : session.Channels)
			{
				ImGui::PushID(channel.Name.c_str());
				if (ImGui::Selectable(channel.Name.c_str(), m_AxisChoice == AxisChoice::Channel && m_AxisChannel == channel.Name))
				{
					PlotAgainstChannel(channel.Name);
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		ImGui::SetItemTooltip("What the rows are plotted against: time from the lap's start, or a distance channel");
	}

	void TelemetryPanel::DrawChannelList(TelemetrySession const& session, Scene* scene, std::vector<std::string> const& chosen)
	{
		for (TelemetryChannel const& channel : session.Channels)
		{
			DrawRecord::ChannelRow row;
			row.Name = channel.Name;
			row.Label = DescribeTelemetryChannel(channel.Name, scene);
			row.Chosen = std::find(chosen.begin(), chosen.end(), channel.Name) != chosen.end();
			row.Last = channel.Last;
			ImGui::PushID(channel.Name.c_str());
			bool checked = row.Chosen;
			if (ImGui::Checkbox(row.Label.c_str(), &checked))
			{
				// The panel's own choice becomes the list to change.
				std::vector<std::string> channels = chosen;
				if (checked)
				{
					channels.push_back(channel.Name);
				}
				else
				{
					std::erase(channels, channel.Name);
				}
				// Unchecking the last lane leaves none, rather than the panel's own choice.
				m_Channels = std::move(channels);
				m_ChannelsChosen = true;
			}
			ImVec2 const boxMin = ImGui::GetItemRectMin();
			float const box = ImGui::GetFrameHeight();
			row.Center = {boxMin.x + box * 0.5f, boxMin.y + box * 0.5f};
			ImGui::SetItemTooltip("%s", channel.Name.c_str());
			// The last value, at the list's right edge.
			char value[32];
			std::snprintf(value, sizeof(value), "%.4g", static_cast<double>(channel.Last));
			float const valueWidth = ImGui::CalcTextSize(value).x;
			ImGui::SameLine();
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - valueWidth, 0.0f));
			ImGui::TextDisabled("%s", value);
			ImGui::PopID();
			m_Record.Channels.push_back(std::move(row));
		}
	}

	TelemetrySeries const& TelemetryPanel::GetSeries(size_t slot, Plotted const& plotted, TelemetryChannel const& channel, double start,
	                                                 double end, uint32_t columns)
	{
		SeriesKey key;
		key.Session = plotted.Span.Session;
		key.Channel = channel.Name;
		key.Axis = m_Record.Axis;
		key.Distance = plotted.Distance != nullptr ? plotted.Distance->Name : std::string();
		key.FirstRow = plotted.Rows.FirstRow;
		key.EndRow = plotted.Rows.EndRow;
		key.Start = start;
		key.End = end;
		key.Columns = columns;
		CachedSeries& cached = m_Series[slot];
		if (!(cached.Key == key))
		{
			TelemetrySeriesRequest request;
			request.Session = plotted.Session;
			request.Span = plotted.Span;
			request.Channel = &channel;
			request.Axis = m_Record.Axis;
			request.Distance = plotted.Distance;
			request.Rows = plotted.Rows;
			request.Start = start;
			request.End = end;
			request.Columns = columns;
			cached.Series = ReduceTelemetrySeries(request);
			cached.Key = std::move(key);
		}
		return cached.Series;
	}

	void TelemetryPanel::DrawLanes(Plotted const& shown, std::optional<Plotted> const& reference, std::vector<std::string> const& chosen,
	                               Scene* scene)
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		float const lineHeight = ImGui::GetTextLineHeight();
		ImVec2 const origin = ImGui::GetCursorScreenPos();
		ImVec2 const available = ImGui::GetContentRegionAvail();
		float const legendHeight = lineHeight + style.ItemSpacing.y;
		float const axisHeight = lineHeight + style.ItemSpacing.y * 2.0f;
		float const left = std::floor(origin.x + style.FramePadding.x);
		float const right = std::floor(origin.x + std::max(available.x - style.FramePadding.x, style.FramePadding.x + 1.0f));
		uint32_t const columns = static_cast<uint32_t>(std::max(right - left, 1.0f));
		m_Record.LanesLeft = left;
		m_Record.LanesRight = left + static_cast<float>(columns);

		// The legend: the shown span over the overlaid one.
		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		std::string const shownLabel = shown.Span.IsLap() ? DescribeSessionLap(shown.Span) : fmt::format("Session {}", shown.Span.Session);
		drawList->AddText(ImVec2(left, origin.y), GetShownColor(), shownLabel.c_str());
		if (reference)
		{
			std::string const over = "over " + DescribeSessionLap(reference->Span);
			float const offset = ImGui::CalcTextSize(shownLabel.c_str()).x + ImGui::GetFontSize();
			drawList->AddText(ImVec2(left + offset, origin.y), GetReferenceColor(), over.c_str());
		}
		if (chosen.empty())
		{
			ImGui::Dummy(ImVec2(available.x, legendHeight));
			ImGui::TextDisabled("Choose channels to plot in the list.");
			return;
		}

		// The positions the lanes span: both spans' rows.
		bool placed = false;
		double start = 0.0;
		double end = 1.0;
		for (Plotted const* plotted : {&shown, reference ? &*reference : nullptr})
		{
			if (plotted == nullptr || plotted->Rows.IsEmpty())
			{
				continue;
			}
			start = placed ? std::min(start, plotted->Rows.Start) : plotted->Rows.Start;
			end = placed ? std::max(end, plotted->Rows.End) : plotted->Rows.End;
			placed = true;
		}
		if (!(end > start))
		{
			end = start + (m_Record.Axis == TelemetryAxis::Time ? std::max(static_cast<double>(shown.Session->StepSeconds), 1e-3) : 1.0);
		}
		m_Record.AxisStart = start;
		m_Record.AxisEnd = end;

		float const lanesTop = std::floor(origin.y + legendHeight);
		float const lanesHeight = std::max(available.y - legendHeight - axisHeight, 1.0f);
		float const laneHeight = std::floor(std::max(lanesHeight / static_cast<float>(chosen.size()), lineHeight * MinLaneLines));
		float const lanesBottom = lanesTop + laneHeight * static_cast<float>(chosen.size());
		ImGui::Dummy(ImVec2(available.x, lanesBottom - origin.y + axisHeight));

		auto const toScreenX = [left, start, end, columns](double along)
		{
			return left + static_cast<float>((along - start) / (end - start)) * static_cast<float>(columns);
		};

		// Ticks along the axis, with a faint line across the lanes at each.
		double const tick = GetNiceCurveStep((end - start) * static_cast<double>(TickSpacing) / static_cast<double>(columns));
		std::pair<double, int> const ticks =
			GetCurveRulerLines(start, end, tick, static_cast<int>(static_cast<float>(columns) / TickSpacing) + 2);
		char const* const unit = m_Record.Axis == TelemetryAxis::Time ? "s" : "m";
		for (int index = 0; index < ticks.second; ++index)
		{
			double const position = (ticks.first + static_cast<double>(index)) * tick;
			float const x = std::floor(toScreenX(position)) + 0.5f;
			drawList->AddLine(ImVec2(x, lanesTop), ImVec2(x, lanesBottom), GetColor(ImGuiCol_Separator, 0.35f));
			std::string const label = fmt::format("{:g} {}", position, unit);
			drawList->AddText(ImVec2(x + 2.0f, lanesBottom + style.ItemSpacing.y), GetColor(ImGuiCol_TextDisabled), label.c_str());
		}
		m_Record.Ticks = static_cast<size_t>(std::max(ticks.second, 0));

		// Two series a lane, sized before any is read: GetSeries hands out references into the list.
		if (m_Series.size() < chosen.size() * 2)
		{
			m_Series.resize(chosen.size() * 2);
		}
		drawList->PushClipRect(ImVec2(left, lanesTop), ImVec2(left + static_cast<float>(columns), lanesBottom), true);
		for (size_t laneIndex = 0; laneIndex < chosen.size(); ++laneIndex)
		{
			std::string const& name = chosen[laneIndex];
			TelemetryChannel const* const channel = FindTelemetryChannel(*shown.Session, name);
			if (channel == nullptr)
			{
				continue;
			}
			float const top = lanesTop + laneHeight * static_cast<float>(laneIndex);
			float const bottom = top + laneHeight - 1.0f;
			DrawRecord::Lane lane;
			lane.Channel = name;
			lane.Label = DescribeTelemetryChannel(name, scene);
			lane.Top = top;
			lane.Bottom = bottom;

			TelemetrySeries const& shownSeries = GetSeries(laneIndex * 2, shown, *channel, start, end, columns);
			TelemetrySeries const* referenceSeries = nullptr;
			if (reference)
			{
				if (TelemetryChannel const* const referenceChannel = FindTelemetryChannel(*reference->Session, name))
				{
					referenceSeries = &GetSeries(laneIndex * 2 + 1, *reference, *referenceChannel, start, end, columns);
				}
			}
			std::pair<float, float> const range = GetLaneRange(shownSeries, referenceSeries);
			float const minimum = range.first;
			float const maximum = range.second;
			lane.Minimum = minimum;
			lane.Maximum = maximum;
			float const margin = (bottom - top) * LaneMargin;
			auto const toScreenY = [top, bottom, margin, minimum, maximum](float level)
			{
				float const fraction = (level - minimum) / (maximum - minimum);
				return bottom - margin - fraction * (bottom - top - 2.0f * margin);
			};

			drawList->AddRectFilled(ImVec2(left, top), ImVec2(left + static_cast<float>(columns), bottom), GetColor(ImGuiCol_FrameBg));
			auto const drawSeries = [drawList, left, &toScreenY](TelemetrySeries const& series, ImU32 color)
			{
				DrawRecord::Series drawn;
				drawn.Rows = series.Rows;
				drawn.Points = series.Points.size();
				drawn.Segments = series.Segments.size();
				drawn.Minimum = series.Minimum;
				drawn.Maximum = series.Maximum;
				std::vector<ImVec2> screenPoints;
				for (size_t segment = 0; segment < series.Segments.size(); ++segment)
				{
					size_t const segmentStart = series.Segments[segment];
					size_t const segmentEnd = segment + 1 < series.Segments.size() ? series.Segments[segment + 1] : series.Points.size();
					screenPoints.clear();
					for (size_t point = segmentStart; point < segmentEnd; ++point)
					{
						screenPoints.emplace_back(left + series.Points[point].x, toScreenY(series.Points[point].y));
					}
					if (screenPoints.size() == 1)
					{
						drawList->AddCircleFilled(screenPoints.front(), 1.5f, color);
					}
					else if (!screenPoints.empty())
					{
						drawList->AddPolyline(screenPoints.data(), static_cast<int>(screenPoints.size()), color, 1.5f);
					}
				}
				if (!series.Points.empty())
				{
					glm::vec2 const firstPoint = series.Points.front();
					glm::vec2 const lastPoint = series.Points.back();
					drawn.First = std::array<float, 2>{left + firstPoint.x, toScreenY(firstPoint.y)};
					drawn.Last = std::array<float, 2>{left + lastPoint.x, toScreenY(lastPoint.y)};
				}
				return drawn;
			};
			if (referenceSeries != nullptr)
			{
				lane.Reference = drawSeries(*referenceSeries, GetReferenceColor());
			}
			lane.Shown = drawSeries(shownSeries, GetShownColor());

			// The lane's name and the values it spans, over its top and bottom.
			std::string const heading = fmt::format("{}  {:.4g}", lane.Label, maximum);
			drawList->AddText(ImVec2(left + 4.0f, top + 1.0f), GetColor(ImGuiCol_Text), heading.c_str());
			std::string const low = fmt::format("{:.4g}", minimum);
			drawList->AddText(ImVec2(left + 4.0f, bottom - lineHeight - 1.0f), GetColor(ImGuiCol_TextDisabled), low.c_str());
			drawList->AddLine(ImVec2(left, bottom), ImVec2(left + static_cast<float>(columns), bottom), GetColor(ImGuiCol_Border));
			m_Record.Lanes.push_back(std::move(lane));
		}
		drawList->PopClipRect();
	}

	void TelemetryPanel::ApplyToolbarAction(EditorTelemetry& telemetry)
	{
		ToolbarAction const action = std::exchange(m_Action, ToolbarAction::None);
		switch (action)
		{
			case ToolbarAction::None:
				break;
			case ToolbarAction::Pause:
				telemetry.Stop();
				break;
			case ToolbarAction::Record:
				telemetry.Resume();
				break;
		}
	}
}
