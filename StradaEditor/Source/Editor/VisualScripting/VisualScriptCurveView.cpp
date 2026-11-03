#include "Editor/VisualScripting/VisualScriptCurveView.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace Strada
{
	namespace
	{
		// The curves are sampled this many times across the length when the view frames them.
		constexpr int FrameSamples = 64;
		// The share of a framed span added on each side.
		constexpr float FramePadding = 0.08f;

		char const* GetInterpolationLabel(VisualScriptKeyInterpolation interpolation)
		{
			switch (interpolation)
			{
				case VisualScriptKeyInterpolation::Cubic:
					return "Cubic";
				case VisualScriptKeyInterpolation::Linear:
					return "Linear";
				case VisualScriptKeyInterpolation::Constant:
					return "Constant";
			}
			return "Cubic";
		}

		// The span [low, high] padded on both sides; a span of one point grows to minimum around it.
		std::pair<float, float> Pad(double low, double high, double minimum)
		{
			double span = high - low;
			if (!(span > minimum))
			{
				double const middle = (low + high) * 0.5;
				return {static_cast<float>(middle - minimum * 0.5), static_cast<float>(middle + minimum * 0.5)};
			}
			span *= FramePadding;
			return {static_cast<float>(low - span), static_cast<float>(high + span)};
		}
	}

	void VisualScriptCurveView::SetRect(glm::vec2 min, glm::vec2 max, float fontSize)
	{
		m_Min = min;
		m_Max = glm::max(max, min + glm::vec2(1.0f));
		m_FontSize = std::max(fontSize, 1.0f);
	}

	void VisualScriptCurveView::Sync(VisualScriptTimeline const& timeline)
	{
		m_Lanes.clear();
		for (uint32_t track = 0; track < timeline.Tracks.size(); track++)
		{
			if (timeline.Tracks[track].Components == 0 && GetTrackView(timeline.Tracks[track].Name).Visible)
			{
				m_Lanes.push_back(track);
			}
		}
		std::erase_if(m_Selection,
		              [&timeline](VisualScriptTimelineKeyRef const& key)
		              {
						  return key.first >= timeline.Tracks.size() || key.second >= timeline.Tracks[key.first].Keys.size();
					  });
		if (!m_Framed)
		{
			m_Framed = true;
			Frame(timeline);
		}
	}

	void VisualScriptCurveView::SetTimeRange(float start, float end)
	{
		float const span = std::clamp(end - start, MinTimeSpan, MaxTimeSpan);
		m_TimeStart = start;
		m_TimeEnd = start + span;
		m_Framed = true;
	}

	void VisualScriptCurveView::SetValueRange(float start, float end)
	{
		float const span = std::clamp(end - start, MinValueSpan, MaxValueSpan);
		m_ValueStart = start;
		m_ValueEnd = start + span;
		m_Framed = true;
	}

	float VisualScriptCurveView::GetPixelsPerSecond() const
	{
		return (m_Max.x - m_Min.x) / (m_TimeEnd - m_TimeStart);
	}

	float VisualScriptCurveView::GetPixelsPerValue() const
	{
		return std::max(m_Max.y - GetCurvesTop(), 1.0f) / (m_ValueEnd - m_ValueStart);
	}

	float VisualScriptCurveView::ToScreenX(double time) const
	{
		return m_Min.x + static_cast<float>((time - m_TimeStart) * GetPixelsPerSecond());
	}

	float VisualScriptCurveView::ToScreenY(double value) const
	{
		return m_Max.y - static_cast<float>((value - m_ValueStart) * GetPixelsPerValue());
	}

	double VisualScriptCurveView::ToTime(float x) const
	{
		return m_TimeStart + static_cast<double>(x - m_Min.x) / GetPixelsPerSecond();
	}

	double VisualScriptCurveView::ToValue(float y) const
	{
		return m_ValueStart + static_cast<double>(m_Max.y - y) / GetPixelsPerValue();
	}

	float VisualScriptCurveView::GetLanesTop() const
	{
		return std::min(m_Min.y + RulerHeight * m_FontSize, m_Max.y);
	}

	float VisualScriptCurveView::GetCurvesTop() const
	{
		// The curves keep at least two font sizes of height however many lanes there are.
		float const lanes = static_cast<float>(m_Lanes.size()) * LaneHeight * m_FontSize;
		return std::max(std::min(GetLanesTop() + lanes, m_Max.y - 2.0f * m_FontSize), GetLanesTop());
	}

	std::optional<std::pair<float, float>> VisualScriptCurveView::GetLane(uint32_t track) const
	{
		auto const found = std::find(m_Lanes.begin(), m_Lanes.end(), track);
		if (found == m_Lanes.end())
		{
			return std::nullopt;
		}
		float const top = GetLanesTop() + static_cast<float>(std::distance(m_Lanes.begin(), found)) * LaneHeight * m_FontSize;
		return std::pair(top, top + LaneHeight * m_FontSize);
	}

	std::optional<glm::vec2> VisualScriptCurveView::GetKeyPosition(VisualScriptTimeline const& timeline, uint32_t track, uint32_t key,
	                                                               uint32_t component) const
	{
		if (track >= timeline.Tracks.size() || key >= timeline.Tracks[track].Keys.size())
		{
			return std::nullopt;
		}
		VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
		VisualScriptTimelineKey const& stored = owner.Keys[key];
		float const x = ToScreenX(stored.Time);
		if (owner.Components == 0)
		{
			std::optional<std::pair<float, float>> const lane = GetLane(track);
			return lane ? std::optional<glm::vec2>(glm::vec2(x, (lane->first + lane->second) * 0.5f)) : std::nullopt;
		}
		if (component >= owner.Components)
		{
			return std::nullopt;
		}
		return glm::vec2(x, ToScreenY(stored.Value[static_cast<glm::length_t>(component)]));
	}

	bool VisualScriptCurveView::HasTangentHandles(VisualScriptTimeline const& timeline, uint32_t track, uint32_t key) const
	{
		if (track >= timeline.Tracks.size() || key >= timeline.Tracks[track].Keys.size() || timeline.Tracks[track].Components == 0)
		{
			return false;
		}
		return IsSelected(track, key) && timeline.Tracks[track].Keys[key].Interpolation == VisualScriptKeyInterpolation::Cubic;
	}

	std::optional<glm::vec2> VisualScriptCurveView::GetTangentHandle(VisualScriptTimeline const& timeline, uint32_t track, uint32_t key,
	                                                                 uint32_t component, bool out) const
	{
		if (!HasTangentHandles(timeline, track, key))
		{
			return std::nullopt;
		}
		VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
		if ((out && key + 1 >= owner.Keys.size()) || (!out && key == 0))
		{
			return std::nullopt;
		}
		std::optional<glm::vec2> const position = GetKeyPosition(timeline, track, key, component);
		if (!position)
		{
			return std::nullopt;
		}
		std::pair<glm::vec4, glm::vec4> const tangents = GetVisualScriptTimelineKeyTangents(owner, key);
		float const slope = (out ? tangents.second : tangents.first)[static_cast<glm::length_t>(component)];
		glm::vec2 direction(GetPixelsPerSecond(), -GetPixelsPerValue() * slope);
		float const length = glm::length(direction);
		if (!(length > 0.0f) || !std::isfinite(length))
		{
			direction = glm::vec2(1.0f, 0.0f);
		}
		else
		{
			direction /= length;
		}
		return *position + direction * (out ? HandleLength : -HandleLength) * m_FontSize;
	}

	void VisualScriptCurveView::ZoomTime(float factor, float aboutX)
	{
		double const anchor = ToTime(aboutX);
		double const ratio = static_cast<double>(aboutX - m_Min.x) / static_cast<double>(m_Max.x - m_Min.x);
		double const span = std::clamp(static_cast<double>(m_TimeEnd - m_TimeStart) * factor, static_cast<double>(MinTimeSpan),
		                               static_cast<double>(MaxTimeSpan));
		m_TimeStart = static_cast<float>(anchor - ratio * span);
		m_TimeEnd = static_cast<float>(anchor - ratio * span + span);
	}

	void VisualScriptCurveView::ZoomValues(float factor, float aboutY)
	{
		double const anchor = ToValue(aboutY);
		double const ratio = static_cast<double>(m_Max.y - aboutY) / static_cast<double>(std::max(m_Max.y - GetCurvesTop(), 1.0f));
		double const span = std::clamp(static_cast<double>(m_ValueEnd - m_ValueStart) * factor, static_cast<double>(MinValueSpan),
		                               static_cast<double>(MaxValueSpan));
		m_ValueStart = static_cast<float>(anchor - ratio * span);
		m_ValueEnd = static_cast<float>(anchor - ratio * span + span);
	}

	void VisualScriptCurveView::Pan(glm::vec2 pixels)
	{
		float const time = pixels.x / GetPixelsPerSecond();
		float const value = pixels.y / GetPixelsPerValue();
		m_TimeStart -= time;
		m_TimeEnd -= time;
		m_ValueStart += value;
		m_ValueEnd += value;
	}

	void VisualScriptCurveView::Frame(VisualScriptTimeline const& timeline)
	{
		double timeLow = std::numeric_limits<double>::infinity();
		double timeHigh = -std::numeric_limits<double>::infinity();
		double valueLow = std::numeric_limits<double>::infinity();
		double valueHigh = -std::numeric_limits<double>::infinity();
		auto const addValue = [&valueLow, &valueHigh](double value)
		{
			valueLow = std::min(valueLow, value);
			valueHigh = std::max(valueHigh, value);
		};
		if (!m_Selection.empty())
		{
			for (auto const& [track, key] : m_Selection)
			{
				VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
				double const time = owner.Keys[key].Time;
				timeLow = std::min(timeLow, time);
				timeHigh = std::max(timeHigh, time);
				for (uint32_t component = 0; component < owner.Components; component++)
				{
					if (IsVisible(timeline, track, component))
					{
						addValue(owner.Keys[key].Value[static_cast<glm::length_t>(component)]);
					}
				}
			}
		}
		else
		{
			timeLow = 0.0;
			timeHigh = timeline.Length;
			for (uint32_t track = 0; track < timeline.Tracks.size(); track++)
			{
				VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
				for (uint32_t component = 0; component < owner.Components; component++)
				{
					if (!IsVisible(timeline, track, component))
					{
						continue;
					}
					auto const index = static_cast<glm::length_t>(component);
					for (VisualScriptTimelineKey const& key : owner.Keys)
					{
						addValue(key.Value[index]);
					}
					for (int sample = 0; sample <= FrameSamples; sample++)
					{
						double const time = static_cast<double>(timeline.Length) * sample / FrameSamples;
						addValue(EvaluateVisualScriptTimelineTrack(owner, time)[index]);
					}
				}
			}
		}
		double const minimumTime = std::max(0.1 * static_cast<double>(timeline.Length), 0.05);
		auto const [timeStart, timeEnd] = Pad(timeLow, timeHigh, minimumTime);
		SetTimeRange(timeStart, timeEnd);
		if (std::isfinite(valueLow) && std::isfinite(valueHigh))
		{
			auto const [valueStart, valueEnd] = Pad(valueLow, valueHigh, 2.0);
			SetValueRange(valueStart, valueEnd);
		}
		else if (m_Selection.empty())
		{
			SetValueRange(-1.0f, 1.0f);
		}
	}

	VisualScriptCurveHit VisualScriptCurveView::HitTest(VisualScriptTimeline const& timeline, glm::vec2 point) const
	{
		VisualScriptCurveHit hit;
		hit.Time = static_cast<float>(ToTime(point.x));
		hit.Value = static_cast<float>(ToValue(point.y));
		if (point.x < m_Min.x || point.y < m_Min.y || point.x > m_Max.x || point.y > m_Max.y)
		{
			return hit;
		}

		// Keys: the nearest diamond within reach.
		float best = HitRadius * m_FontSize;
		bool found = false;
		auto const consider =
			[&](VisualScriptCurveHitKind kind, uint32_t track, uint32_t key, uint32_t component, glm::vec2 position, bool manhattan)
		{
			glm::vec2 const offset = glm::abs(position - point);
			float const distance = manhattan ? offset.x + offset.y : glm::length(position - point);
			if (distance <= best)
			{
				best = distance;
				found = true;
				hit.Kind = kind;
				hit.Track = track;
				hit.Key = key;
				hit.Component = component;
			}
		};
		for (uint32_t track = 0; track < timeline.Tracks.size(); track++)
		{
			VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
			uint32_t const components = std::max<uint32_t>(owner.Components, 1);
			for (uint32_t component = 0; component < components; component++)
			{
				if (!IsVisible(timeline, track, component))
				{
					continue;
				}
				for (uint32_t key = 0; key < owner.Keys.size(); key++)
				{
					if (std::optional<glm::vec2> const position = GetKeyPosition(timeline, track, key, component))
					{
						consider(VisualScriptCurveHitKind::Key, track, key, component, *position, true);
					}
				}
			}
		}
		if (found)
		{
			return hit;
		}

		// The tangent handles of the selected cubic keys.
		for (auto const& [track, key] : m_Selection)
		{
			if (!HasTangentHandles(timeline, track, key))
			{
				continue;
			}
			for (uint32_t component = 0; component < timeline.Tracks[track].Components; component++)
			{
				if (!IsVisible(timeline, track, component))
				{
					continue;
				}
				for (bool const out : {false, true})
				{
					if (std::optional<glm::vec2> const handle = GetTangentHandle(timeline, track, key, component, out))
					{
						consider(out ? VisualScriptCurveHitKind::OutTangent : VisualScriptCurveHitKind::InTangent, track, key, component,
						         *handle, false);
					}
				}
			}
		}
		if (found)
		{
			return hit;
		}

		// The event lanes.
		for (uint32_t const track : m_Lanes)
		{
			std::optional<std::pair<float, float>> const lane = GetLane(track);
			if (lane && point.y >= lane->first && point.y < lane->second)
			{
				hit.Kind = VisualScriptCurveHitKind::EventLane;
				hit.Track = track;
				return hit;
			}
		}

		// The curves: the nearest one at the point's time.
		if (point.y >= GetCurvesTop())
		{
			best = CurveHitRadius * m_FontSize;
			for (uint32_t track = 0; track < timeline.Tracks.size(); track++)
			{
				VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
				if (owner.Components == 0)
				{
					continue;
				}
				glm::vec4 const value = EvaluateVisualScriptTimelineTrack(owner, ToTime(point.x));
				for (uint32_t component = 0; component < owner.Components; component++)
				{
					if (!IsVisible(timeline, track, component))
					{
						continue;
					}
					float const distance = std::abs(ToScreenY(value[static_cast<glm::length_t>(component)]) - point.y);
					if (distance <= best)
					{
						best = distance;
						hit.Kind = VisualScriptCurveHitKind::Curve;
						hit.Track = track;
						hit.Component = component;
					}
				}
			}
			if (hit.Kind == VisualScriptCurveHitKind::Curve)
			{
				return hit;
			}
		}
		hit.Kind = VisualScriptCurveHitKind::Empty;
		return hit;
	}

	VisualScriptCurveTrackView VisualScriptCurveView::GetTrackView(std::string_view track) const
	{
		auto const found = std::find_if(m_TrackViews.begin(), m_TrackViews.end(),
		                                [track](NamedTrackView const& view)
		                                {
											return view.Name == track;
										});
		return found != m_TrackViews.end() ? found->View : VisualScriptCurveTrackView{};
	}

	void VisualScriptCurveView::SetTrackVisible(std::string_view track, bool visible)
	{
		auto found = std::find_if(m_TrackViews.begin(), m_TrackViews.end(),
		                          [track](NamedTrackView const& view)
		                          {
									  return view.Name == track;
								  });
		if (found == m_TrackViews.end())
		{
			m_TrackViews.push_back({std::string(track), {}});
			found = std::prev(m_TrackViews.end());
		}
		found->View.Visible = visible;
	}

	void VisualScriptCurveView::SetComponentVisible(std::string_view track, uint32_t component, bool visible)
	{
		if (component >= 4)
		{
			return;
		}
		auto found = std::find_if(m_TrackViews.begin(), m_TrackViews.end(),
		                          [track](NamedTrackView const& view)
		                          {
									  return view.Name == track;
								  });
		if (found == m_TrackViews.end())
		{
			m_TrackViews.push_back({std::string(track), {}});
			found = std::prev(m_TrackViews.end());
		}
		found->View.Components[component] = visible;
	}

	bool VisualScriptCurveView::IsVisible(VisualScriptTimeline const& timeline, uint32_t track, uint32_t component) const
	{
		if (track >= timeline.Tracks.size())
		{
			return false;
		}
		VisualScriptCurveTrackView const view = GetTrackView(timeline.Tracks[track].Name);
		if (!view.Visible)
		{
			return false;
		}
		// An event track has its lane only; a curve its components.
		return timeline.Tracks[track].Components == 0 ? component == 0 : component < 4 && view.Components[component];
	}

	bool VisualScriptCurveView::IsSelected(uint32_t track, uint32_t key) const
	{
		return std::find(m_Selection.begin(), m_Selection.end(), VisualScriptTimelineKeyRef(track, key)) != m_Selection.end();
	}

	void VisualScriptCurveView::Select(std::vector<VisualScriptTimelineKeyRef> keys)
	{
		std::sort(keys.begin(), keys.end());
		keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
		m_Selection = std::move(keys);
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptCurveView::GetMarquee() const
	{
		if (m_Gesture != VisualScriptCurveGesture::Marquee)
		{
			return std::nullopt;
		}
		return std::pair(glm::min(m_PressMouse, m_LastMouse), glm::max(m_PressMouse, m_LastMouse));
	}

	double VisualScriptCurveView::Snap(double time) const
	{
		return std::round(time / static_cast<double>(TimeStep)) * static_cast<double>(TimeStep);
	}

	std::vector<VisualScriptCurveIntent> VisualScriptCurveView::Update(VisualScriptCurveViewInput const& input,
	                                                                   VisualScriptTimeline const& timeline, std::string_view title)
	{
		std::vector<VisualScriptCurveIntent> intents;
		float const threshold = DragThreshold * m_FontSize;
		bool const moved = glm::length(input.Mouse - m_PressMouse) > threshold;
		if (input.Hovered && input.Wheel != 0.0f && m_Gesture == VisualScriptCurveGesture::Idle)
		{
			float const factor = std::pow(ZoomStep, -input.Wheel);
			if (input.Ctrl)
			{
				ZoomValues(factor, input.Mouse.y);
			}
			else
			{
				ZoomTime(factor, input.Mouse.x);
			}
		}
		switch (m_Gesture)
		{
			case VisualScriptCurveGesture::Idle:
				if (input.Hovered && input.Pressed[0])
				{
					BeginPress(input, timeline, title, intents);
				}
				else if (input.Hovered && (input.Pressed[1] || input.Pressed[2]))
				{
					m_Gesture = VisualScriptCurveGesture::PanPress;
					m_Button = input.Pressed[1] ? 1 : 2;
					m_PressMouse = input.Mouse;
					m_PressHit = HitTest(timeline, input.Mouse);
				}
				break;
			case VisualScriptCurveGesture::Press:
				if (!input.Down[0])
				{
					// A click: a key alone becomes the selection; empty space clears it (Shift and Ctrl acted at the press).
					if (m_PressHit.Kind == VisualScriptCurveHitKind::Key && !m_PressShift && !m_PressCtrl)
					{
						Select({{m_PressHit.Track, m_PressHit.Key}});
					}
					else if (m_PressHit.Kind != VisualScriptCurveHitKind::Key && m_PressHit.Kind != VisualScriptCurveHitKind::InTangent &&
					         m_PressHit.Kind != VisualScriptCurveHitKind::OutTangent && !m_PressShift && !m_PressCtrl)
					{
						m_Selection.clear();
					}
					m_Gesture = VisualScriptCurveGesture::Idle;
				}
				else if (moved)
				{
					m_Start = timeline;
					m_StartSelection = m_Selection;
					m_LastEdit.reset();
					switch (m_PressHit.Kind)
					{
						case VisualScriptCurveHitKind::Key:
							// A Ctrl press toggled the key: it moves nothing.
							m_Gesture = m_PressCtrl ? VisualScriptCurveGesture::Idle : VisualScriptCurveGesture::MoveKeys;
							break;
						case VisualScriptCurveHitKind::InTangent:
						case VisualScriptCurveHitKind::OutTangent:
							m_Gesture = VisualScriptCurveGesture::Tangent;
							break;
						case VisualScriptCurveHitKind::None:
						case VisualScriptCurveHitKind::EventLane:
						case VisualScriptCurveHitKind::Curve:
						case VisualScriptCurveHitKind::Empty:
							m_Gesture = VisualScriptCurveGesture::Marquee;
							break;
					}
					m_LastMouse = input.Mouse;
					if (m_Gesture == VisualScriptCurveGesture::Marquee)
					{
						UpdateMarquee(timeline);
					}
					else if (m_Gesture != VisualScriptCurveGesture::Idle)
					{
						UpdateDrag(input, title, intents);
					}
				}
				break;
			case VisualScriptCurveGesture::MoveKeys:
			case VisualScriptCurveGesture::Tangent:
				if (!input.Down[0])
				{
					if (m_LastEdit)
					{
						intents.push_back({VisualScriptCurveIntentKind::EndEdit, {}, {}});
					}
					m_LastEdit.reset();
					m_Gesture = VisualScriptCurveGesture::Idle;
				}
				else
				{
					UpdateDrag(input, title, intents);
				}
				break;
			case VisualScriptCurveGesture::Marquee:
				m_LastMouse = input.Mouse;
				if (!input.Down[0])
				{
					m_Gesture = VisualScriptCurveGesture::Idle;
				}
				else
				{
					UpdateMarquee(timeline);
				}
				break;
			case VisualScriptCurveGesture::PanPress:
				if (!input.Down[static_cast<size_t>(m_Button)])
				{
					// A right click on a key opens the menu of the selected keys, the key among them.
					if (m_Button == 1 && m_PressHit.Kind == VisualScriptCurveHitKind::Key)
					{
						if (!IsSelected(m_PressHit.Track, m_PressHit.Key))
						{
							Select({{m_PressHit.Track, m_PressHit.Key}});
						}
						intents.push_back({VisualScriptCurveIntentKind::OpenKeyMenu, {}, {}});
					}
					m_Gesture = VisualScriptCurveGesture::Idle;
				}
				else if (moved)
				{
					m_Gesture = VisualScriptCurveGesture::Pan;
					Pan(input.Mouse - m_PressMouse);
				}
				break;
			case VisualScriptCurveGesture::Pan:
				Pan(input.Mouse - m_LastMouse);
				if (!input.Down[static_cast<size_t>(m_Button)])
				{
					m_Gesture = VisualScriptCurveGesture::Idle;
				}
				break;
		}
		m_LastMouse = input.Mouse;
		return intents;
	}

	void VisualScriptCurveView::BeginPress(VisualScriptCurveViewInput const& input, VisualScriptTimeline const& timeline,
	                                       std::string_view title, std::vector<VisualScriptCurveIntent>& intents)
	{
		VisualScriptCurveHit const hit = HitTest(timeline, input.Mouse);
		if (input.DoubleClicked)
		{
			// A double-click on a curve adds a key with the curve's value there; on an event lane, an event.
			if (hit.Kind == VisualScriptCurveHitKind::Curve || hit.Kind == VisualScriptCurveHitKind::EventLane)
			{
				if (std::optional<VisualScriptCurveIntent> added = AddKey(timeline, hit.Track, hit.Time, input.Alt, title))
				{
					intents.push_back(std::move(*added));
				}
			}
			return;
		}
		m_PressHit = hit;
		m_PressMouse = input.Mouse;
		m_LastMouse = input.Mouse;
		m_PressShift = input.Shift;
		m_PressCtrl = input.Ctrl;
		m_StartSelection = m_Selection;
		if (hit.Kind == VisualScriptCurveHitKind::Key)
		{
			VisualScriptTimelineKeyRef const key(hit.Track, hit.Key);
			if (input.Ctrl)
			{
				if (IsSelected(hit.Track, hit.Key))
				{
					std::erase(m_Selection, key);
				}
				else
				{
					m_Selection.push_back(key);
					Select(m_Selection);
				}
			}
			else if (!IsSelected(hit.Track, hit.Key))
			{
				if (input.Shift)
				{
					m_Selection.push_back(key);
					Select(m_Selection);
				}
				else
				{
					Select({key});
				}
			}
		}
		m_Gesture = VisualScriptCurveGesture::Press;
	}

	void VisualScriptCurveView::UpdateDrag(VisualScriptCurveViewInput const& input, std::string_view title,
	                                       std::vector<VisualScriptCurveIntent>& intents)
	{
		uint32_t const track = m_PressHit.Track;
		uint32_t const key = m_PressHit.Key;
		uint32_t const component = m_PressHit.Component;
		if (track >= m_Start.Tracks.size() || key >= m_Start.Tracks[track].Keys.size())
		{
			return;
		}
		VisualScriptTimelineTrack const& owner = m_Start.Tracks[track];
		VisualScriptTimelineKey const& grabbed = owner.Keys[key];
		VisualScriptTimeline edited = m_Start;
		std::string action;
		if (m_Gesture == VisualScriptCurveGesture::MoveKeys)
		{
			glm::vec2 const delta = input.Mouse - m_PressMouse;
			double timeOffset = delta.x / GetPixelsPerSecond();
			double valueOffset = owner.Components > 0 ? -delta.y / GetPixelsPerValue() : 0.0;
			if (input.Shift)
			{
				// One axis: the one the mouse has moved along more.
				if (std::abs(delta.x) >= std::abs(delta.y))
				{
					valueOffset = 0.0;
				}
				else
				{
					timeOffset = 0.0;
				}
			}
			if (!input.Alt && timeOffset != 0.0)
			{
				timeOffset = Snap(grabbed.Time + timeOffset) - grabbed.Time;
			}
			glm::vec4 offset(0.0f);
			if (owner.Components > 0 && component < owner.Components)
			{
				offset[static_cast<glm::length_t>(component)] = static_cast<float>(valueOffset);
			}
			if (!MoveVisualScriptTimelineKeys(edited, m_StartSelection, static_cast<float>(timeOffset), offset))
			{
				return;
			}
			action = DescribeKeys(m_Start, m_StartSelection, "Move", title);
		}
		else
		{
			bool const out = m_PressHit.Kind == VisualScriptCurveHitKind::OutTangent;
			float const keyX = ToScreenX(grabbed.Time);
			float const keyY = ToScreenY(grabbed.Value[static_cast<glm::length_t>(component)]);
			// The handle stays on its side of the key.
			float const dx = out ? std::max(input.Mouse.x - keyX, 1.0f) : std::min(input.Mouse.x - keyX, -1.0f);
			double const slope = (static_cast<double>(keyY - input.Mouse.y) / GetPixelsPerValue()) / (dx / GetPixelsPerSecond());
			if (!std::isfinite(slope))
			{
				return;
			}
			std::pair<glm::vec4, glm::vec4> const tangents = GetVisualScriptTimelineKeyTangents(owner, key);
			glm::vec4 in = tangents.first;
			glm::vec4 outgoing = tangents.second;
			auto const index = static_cast<glm::length_t>(component);
			std::optional<glm::vec4> newIn = grabbed.In;
			std::optional<glm::vec4> newOut = grabbed.Out;
			if (input.Alt)
			{
				// Broken: only the dragged side.
				(out ? outgoing : in)[index] = static_cast<float>(slope);
				(out ? newOut : newIn) = out ? outgoing : in;
			}
			else
			{
				in[index] = static_cast<float>(slope);
				outgoing[index] = static_cast<float>(slope);
				newIn = in;
				newOut = outgoing;
			}
			if (!SetVisualScriptTimelineKeyTangents(edited, {track, key}, newIn, newOut))
			{
				return;
			}
			action = fmt::format("Edit Tangent of {} on {}", owner.Name, title);
		}
		if (m_LastEdit && *m_LastEdit == edited)
		{
			return;
		}
		m_LastEdit = edited;
		intents.push_back({VisualScriptCurveIntentKind::Edit, std::move(edited), std::move(action)});
	}

	void VisualScriptCurveView::UpdateMarquee(VisualScriptTimeline const& timeline)
	{
		std::vector<VisualScriptTimelineKeyRef> const inside =
			FindKeysIn(timeline, glm::min(m_PressMouse, m_LastMouse), glm::max(m_PressMouse, m_LastMouse));
		std::vector<VisualScriptTimelineKeyRef> selection;
		if (m_PressShift)
		{
			selection = m_StartSelection;
			selection.insert(selection.end(), inside.begin(), inside.end());
		}
		else if (m_PressCtrl)
		{
			selection = m_StartSelection;
			std::erase_if(selection,
			              [&inside](VisualScriptTimelineKeyRef const& key)
			              {
							  return std::find(inside.begin(), inside.end(), key) != inside.end();
						  });
		}
		else
		{
			selection = inside;
		}
		Select(std::move(selection));
	}

	std::vector<VisualScriptTimelineKeyRef> VisualScriptCurveView::FindKeysIn(VisualScriptTimeline const& timeline, glm::vec2 min,
	                                                                          glm::vec2 max) const
	{
		std::vector<VisualScriptTimelineKeyRef> keys;
		for (uint32_t track = 0; track < timeline.Tracks.size(); track++)
		{
			VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
			uint32_t const components = std::max<uint32_t>(owner.Components, 1);
			for (uint32_t key = 0; key < owner.Keys.size(); key++)
			{
				for (uint32_t component = 0; component < components; component++)
				{
					if (!IsVisible(timeline, track, component))
					{
						continue;
					}
					std::optional<glm::vec2> const position = GetKeyPosition(timeline, track, key, component);
					if (position && position->x >= min.x && position->x <= max.x && position->y >= min.y && position->y <= max.y)
					{
						keys.emplace_back(track, key);
						break;
					}
				}
			}
		}
		return keys;
	}

	void VisualScriptCurveView::SelectAll(VisualScriptTimeline const& timeline)
	{
		std::vector<VisualScriptTimelineKeyRef> keys;
		for (uint32_t track = 0; track < timeline.Tracks.size(); track++)
		{
			VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
			bool visible = false;
			for (uint32_t component = 0; component < std::max<uint32_t>(owner.Components, 1); component++)
			{
				visible = visible || IsVisible(timeline, track, component);
			}
			if (!visible)
			{
				continue;
			}
			for (uint32_t key = 0; key < owner.Keys.size(); key++)
			{
				keys.emplace_back(track, key);
			}
		}
		Select(std::move(keys));
	}

	std::optional<VisualScriptCurveIntent> VisualScriptCurveView::DeleteSelection(VisualScriptTimeline const& timeline,
	                                                                              std::string_view title)
	{
		if (m_Selection.empty())
		{
			return std::nullopt;
		}
		VisualScriptTimeline edited = timeline;
		if (!RemoveVisualScriptTimelineKeys(edited, m_Selection))
		{
			return std::nullopt;
		}
		std::string action = DescribeKeys(timeline, m_Selection, "Delete", title);
		m_Selection.clear();
		return VisualScriptCurveIntent{VisualScriptCurveIntentKind::Commit, std::move(edited), std::move(action)};
	}

	std::optional<VisualScriptCurveIntent> VisualScriptCurveView::Nudge(VisualScriptTimeline const& timeline, int steps,
	                                                                    std::string_view title)
	{
		if (m_Selection.empty() || steps == 0)
		{
			return std::nullopt;
		}
		VisualScriptTimeline edited = timeline;
		if (!MoveVisualScriptTimelineKeys(edited, m_Selection, static_cast<float>(steps) * TimeStep, glm::vec4(0.0f)) || edited == timeline)
		{
			return std::nullopt;
		}
		return VisualScriptCurveIntent{VisualScriptCurveIntentKind::Commit, std::move(edited),
		                               DescribeKeys(timeline, m_Selection, "Move", title)};
	}

	std::optional<VisualScriptCurveIntent> VisualScriptCurveView::SetInterpolation(VisualScriptTimeline const& timeline,
	                                                                               VisualScriptKeyInterpolation interpolation,
	                                                                               std::string_view title)
	{
		VisualScriptTimeline edited = timeline;
		if (!SetVisualScriptTimelineKeyInterpolation(edited, m_Selection, interpolation))
		{
			return std::nullopt;
		}
		std::vector<VisualScriptTimelineKeyRef> curves;
		std::copy_if(m_Selection.begin(), m_Selection.end(), std::back_inserter(curves),
		             [&timeline](VisualScriptTimelineKeyRef const& key)
		             {
						 return timeline.Tracks[key.first].Components > 0;
					 });
		return VisualScriptCurveIntent{
			VisualScriptCurveIntentKind::Commit, std::move(edited),
			DescribeKeys(timeline, curves, "Set", title, fmt::format(" to {}", GetInterpolationLabel(interpolation)))};
	}

	std::optional<VisualScriptCurveIntent> VisualScriptCurveView::FlattenTangents(VisualScriptTimeline const& timeline,
	                                                                              std::string_view title)
	{
		VisualScriptTimeline edited = timeline;
		if (!FlattenVisualScriptTimelineTangents(edited, m_Selection))
		{
			return std::nullopt;
		}
		std::vector<VisualScriptTimelineKeyRef> curves;
		std::copy_if(m_Selection.begin(), m_Selection.end(), std::back_inserter(curves),
		             [&timeline](VisualScriptTimelineKeyRef const& key)
		             {
						 return timeline.Tracks[key.first].Components > 0;
					 });
		return VisualScriptCurveIntent{VisualScriptCurveIntentKind::Commit, std::move(edited),
		                               DescribeKeys(timeline, curves, "Flatten Tangents of", title)};
	}

	std::optional<VisualScriptCurveIntent> VisualScriptCurveView::AutoTangents(VisualScriptTimeline const& timeline, std::string_view title)
	{
		VisualScriptTimeline edited = timeline;
		if (!ResetVisualScriptTimelineTangents(edited, m_Selection))
		{
			return std::nullopt;
		}
		std::vector<VisualScriptTimelineKeyRef> curves;
		std::copy_if(m_Selection.begin(), m_Selection.end(), std::back_inserter(curves),
		             [&timeline](VisualScriptTimelineKeyRef const& key)
		             {
						 return timeline.Tracks[key.first].Components > 0;
					 });
		return VisualScriptCurveIntent{VisualScriptCurveIntentKind::Commit, std::move(edited),
		                               DescribeKeys(timeline, curves, "Set Tangents of", title, " to Auto")};
	}

	std::optional<VisualScriptCurveIntent> VisualScriptCurveView::AddKey(VisualScriptTimeline const& timeline, uint32_t track, double time,
	                                                                     bool free, std::string_view title)
	{
		if (track >= timeline.Tracks.size())
		{
			return std::nullopt;
		}
		double const at = std::clamp(free ? time : Snap(time), 0.0, static_cast<double>(timeline.Length));
		VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
		VisualScriptTimeline edited = timeline;
		Result<uint32_t> const added =
			AddVisualScriptTimelineKey(edited, track, static_cast<float>(at), EvaluateVisualScriptTimelineTrack(owner, at));
		if (!added)
		{
			return std::nullopt;
		}
		Select({{track, added.GetValue()}});
		return VisualScriptCurveIntent{VisualScriptCurveIntentKind::Commit, std::move(edited),
		                               fmt::format("Add Key to {} on {}", owner.Name, title)};
	}

	std::vector<VisualScriptCurveIntent> VisualScriptCurveView::CancelGesture()
	{
		std::vector<VisualScriptCurveIntent> intents;
		if ((m_Gesture == VisualScriptCurveGesture::MoveKeys || m_Gesture == VisualScriptCurveGesture::Tangent) && m_LastEdit)
		{
			intents.push_back({VisualScriptCurveIntentKind::EndEdit, {}, {}});
		}
		m_LastEdit.reset();
		m_Gesture = VisualScriptCurveGesture::Idle;
		return intents;
	}

	std::string VisualScriptCurveView::DescribeKeys(VisualScriptTimeline const& timeline, std::span<VisualScriptTimelineKeyRef const> keys,
	                                                std::string_view verb, std::string_view title, std::string_view suffix)
	{
		if (keys.size() == 1 && keys.front().first < timeline.Tracks.size())
		{
			return fmt::format("{} Key of {}{} on {}", verb, timeline.Tracks[keys.front().first].Name, suffix, title);
		}
		return fmt::format("{} {} Keys{} on {}", verb, keys.size(), suffix, title);
	}
}
