#pragma once

#include "Editor/UI/CurveView.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iterator>
#include <limits>
#include <tuple>

// The definitions of CurveView's members (CurveView.h). Included only by the translation unit that instantiates the view for
// a document type (template class CurveView<TDocument>;), after the document's CurveDocumentTraits. Pure: no ImGui.
namespace Strada
{
	namespace CurveViewDetail
	{
		// The curves are sampled this many times across the domain when the view frames them.
		inline constexpr int FrameSamples = 64;
		// The share of a framed span added on each side.
		inline constexpr float FramePadding = 0.08f;
		// How far past the view's rectangle screen positions go: far enough to be off screen, near enough to stay exact floats.
		inline constexpr double ScreenReach = 1.0e6;

		inline char const* GetInterpolationLabel(CurveInterpolation interpolation)
		{
			switch (interpolation)
			{
				case CurveInterpolation::Cubic:
					return "Cubic";
				case CurveInterpolation::Linear:
					return "Linear";
				case CurveInterpolation::Constant:
					return "Constant";
			}
			return "Cubic";
		}

		// The span [low, high] padded on both sides; a span of one point grows to minimum around it.
		inline std::pair<double, double> Pad(double low, double high, double minimum)
		{
			double span = high - low;
			if (!(span > minimum))
			{
				double const middle = low * 0.5 + high * 0.5;
				return {middle - minimum * 0.5, middle + minimum * 0.5};
			}
			span *= FramePadding;
			return {low - span, high + span};
		}

		// A range the view can show: its span at least minimum and relative of its middle's magnitude (so the range never
		// collapses to one float, or one double), at most maximum, and its middle within magnitude of 0. The range stays as
		// given when it is one already; nothing for a range that is not finite.
		inline std::optional<std::pair<double, double>> FitRange(double start, double end, double minimum, double maximum, double magnitude,
		                                                         double relative)
		{
			if (!std::isfinite(start) || !std::isfinite(end))
			{
				return std::nullopt;
			}
			double const middle = start * 0.5 + end * 0.5;
			double const fitted = std::clamp(middle, -magnitude, magnitude);
			double const span = end - start;
			double const bounded = std::clamp(span, std::max(minimum, std::abs(fitted) * relative), maximum);
			if (fitted == middle && bounded == span)
			{
				return std::pair(start, end);
			}
			return std::pair(fitted - bounded * 0.5, fitted + bounded * 0.5);
		}

		// A screen coordinate: positions far outside the view are clamped to ScreenReach past it (so curves stay drawable
		// however far they are), and a position that is not a number lies past its far end.
		inline float ToScreen(double position, float low, float high)
		{
			if (std::isnan(position))
			{
				return static_cast<float>(static_cast<double>(high) + ScreenReach);
			}
			return static_cast<float>(
				std::clamp(position, static_cast<double>(low) - ScreenReach, static_cast<double>(high) + ScreenReach));
		}

		// A double as the nearest float (beyond the float range, the largest float; NaN, 0): converting a double outside the
		// float range is undefined.
		inline float ToFloat(double value)
		{
			if (std::isnan(value))
			{
				return 0.0f;
			}
			return static_cast<float>(std::clamp(value, -static_cast<double>(FLT_MAX), static_cast<double>(FLT_MAX)));
		}
	}

	template<typename TDocument>
	void CurveView<TDocument>::SetRect(glm::vec2 min, glm::vec2 max, float fontSize)
	{
		m_Min = min;
		m_Max = glm::max(max, min + glm::vec2(1.0f));
		m_FontSize = std::max(fontSize, 1.0f);
	}

	template<typename TDocument>
	void CurveView<TDocument>::Sync(TDocument const& document)
	{
		uint32_t const trackCount = Traits::GetTrackCount(document);
		std::vector<std::string> names;
		names.reserve(trackCount);
		for (uint32_t track = 0; track < trackCount; track++)
		{
			names.emplace_back(Traits::GetTrackName(document, track));
		}
		if (names != m_TrackNames)
		{
			// Followed by name first; then a name that changed in its place is a rename.
			m_TrackMap.assign(m_TrackNames.size(), std::nullopt);
			std::vector<bool> taken(names.size(), false);
			for (uint32_t previous = 0; previous < m_TrackNames.size(); previous++)
			{
				for (uint32_t track = 0; track < names.size(); track++)
				{
					if (!taken[track] && names[track] == m_TrackNames[previous])
					{
						m_TrackMap[previous] = track;
						taken[track] = true;
						break;
					}
				}
			}
			for (uint32_t previous = 0; previous < m_TrackNames.size(); previous++)
			{
				if (m_TrackMap[previous] || previous >= names.size() || taken[previous])
				{
					continue;
				}
				m_TrackMap[previous] = previous;
				taken[previous] = true;
				// The toggles go with the renamed track; an entry its new name had (of a track removed earlier) goes.
				std::erase_if(m_TrackViews,
				              [&names, previous](NamedTrackView const& view)
				              {
								  return view.Name == names[previous];
							  });
				for (NamedTrackView& view : m_TrackViews)
				{
					if (view.Name == m_TrackNames[previous])
					{
						view.Name = names[previous];
					}
				}
			}
			if (m_Synced)
			{
				std::vector<CurveKeyRef> followed;
				for (auto const& [track, key] : m_Selection)
				{
					if (track < m_TrackMap.size() && m_TrackMap[track])
					{
						followed.emplace_back(*m_TrackMap[track], key);
					}
				}
				Select(std::move(followed));
			}
			m_TrackNames = std::move(names);
		}
		else
		{
			m_TrackMap.resize(m_TrackNames.size());
			for (uint32_t track = 0; track < m_TrackNames.size(); track++)
			{
				m_TrackMap[track] = track;
			}
		}
		m_Synced = true;

		m_Lanes.clear();
		for (uint32_t track = 0; track < trackCount; track++)
		{
			if (Traits::GetComponents(document, track) == 0 && GetTrackView(Traits::GetTrackName(document, track)).Visible)
			{
				m_Lanes.push_back(track);
			}
		}
		std::erase_if(m_Selection,
		              [this, &document](CurveKeyRef const& key)
		              {
						  return !HasKey(document, key);
					  });
		if (!m_Framed)
		{
			m_Framed = true;
			Frame(document);
		}
	}

	template<typename TDocument>
	std::optional<uint32_t> CurveView<TDocument>::FollowTrack(uint32_t track) const
	{
		return track < m_TrackMap.size() ? m_TrackMap[track] : std::nullopt;
	}

	template<typename TDocument>
	void CurveView<TDocument>::SetTimeRange(double start, double end)
	{
		if (std::optional<std::pair<double, double>> const range =
		        CurveViewDetail::FitRange(start, end, MinTimeSpan, MaxTimeSpan, MaxTimeMagnitude, RelativeMinSpan))
		{
			std::tie(m_TimeStart, m_TimeEnd) = *range;
		}
		m_Framed = true;
	}

	template<typename TDocument>
	void CurveView<TDocument>::SetValueRange(double start, double end)
	{
		if (std::optional<std::pair<double, double>> const range =
		        CurveViewDetail::FitRange(start, end, MinValueSpan, MaxValueSpan, MaxValueMagnitude, RelativeMinSpan))
		{
			std::tie(m_ValueStart, m_ValueEnd) = *range;
		}
		m_Framed = true;
	}

	template<typename TDocument>
	double CurveView<TDocument>::GetPixelsPerSecond() const
	{
		return static_cast<double>(m_Max.x - m_Min.x) / (m_TimeEnd - m_TimeStart);
	}

	template<typename TDocument>
	double CurveView<TDocument>::GetPixelsPerValue() const
	{
		return static_cast<double>(std::max(m_Max.y - GetCurvesTop(), 1.0f)) / (m_ValueEnd - m_ValueStart);
	}

	template<typename TDocument>
	float CurveView<TDocument>::ToScreenX(double time) const
	{
		return CurveViewDetail::ToScreen(static_cast<double>(m_Min.x) + (time - m_TimeStart) * GetPixelsPerSecond(), m_Min.x, m_Max.x);
	}

	template<typename TDocument>
	float CurveView<TDocument>::ToScreenY(double value) const
	{
		return CurveViewDetail::ToScreen(static_cast<double>(m_Max.y) - (value - m_ValueStart) * GetPixelsPerValue(), m_Min.y, m_Max.y);
	}

	template<typename TDocument>
	double CurveView<TDocument>::ToTime(float x) const
	{
		return m_TimeStart + static_cast<double>(x - m_Min.x) / GetPixelsPerSecond();
	}

	template<typename TDocument>
	double CurveView<TDocument>::ToValue(float y) const
	{
		return m_ValueStart + static_cast<double>(m_Max.y - y) / GetPixelsPerValue();
	}

	template<typename TDocument>
	double CurveView<TDocument>::GetTimeStep() const
	{
		if constexpr (TimeStep > 0.0f)
		{
			return static_cast<double>(TimeStep);
		}
		else
		{
			return GetNiceCurveStep((m_TimeEnd - m_TimeStart) * 0.01);
		}
	}

	template<typename TDocument>
	float CurveView<TDocument>::GetLanesTop() const
	{
		return std::min(m_Min.y + RulerHeight * m_FontSize, m_Max.y);
	}

	template<typename TDocument>
	float CurveView<TDocument>::GetCurvesTop() const
	{
		// The curves keep at least two font sizes of height however many lanes there are.
		float const lanes = static_cast<float>(m_Lanes.size()) * LaneHeight * m_FontSize;
		return std::max(std::min(GetLanesTop() + lanes, m_Max.y - 2.0f * m_FontSize), GetLanesTop());
	}

	template<typename TDocument>
	std::optional<std::pair<float, float>> CurveView<TDocument>::GetLane(uint32_t track) const
	{
		auto const found = std::find(m_Lanes.begin(), m_Lanes.end(), track);
		if (found == m_Lanes.end())
		{
			return std::nullopt;
		}
		float const top = GetLanesTop() + static_cast<float>(std::distance(m_Lanes.begin(), found)) * LaneHeight * m_FontSize;
		return std::pair(top, top + LaneHeight * m_FontSize);
	}

	template<typename TDocument>
	bool CurveView<TDocument>::HasKey(TDocument const& document, CurveKeyRef key) const
	{
		return key.first < Traits::GetTrackCount(document) && key.second < Traits::GetKeyCount(document, key.first);
	}

	template<typename TDocument>
	std::optional<glm::vec2> CurveView<TDocument>::GetKeyPosition(TDocument const& document, uint32_t track, uint32_t key,
	                                                              uint32_t component) const
	{
		if (!HasKey(document, {track, key}))
		{
			return std::nullopt;
		}
		uint32_t const components = Traits::GetComponents(document, track);
		float const x = ToScreenX(Traits::GetKeyTime(document, track, key));
		if (components == 0)
		{
			std::optional<std::pair<float, float>> const lane = GetLane(track);
			return lane ? std::optional<glm::vec2>(glm::vec2(x, (lane->first + lane->second) * 0.5f)) : std::nullopt;
		}
		if (component >= components)
		{
			return std::nullopt;
		}
		return glm::vec2(x, ToScreenY(Traits::GetKeyValue(document, track, key, component)));
	}

	template<typename TDocument>
	bool CurveView<TDocument>::HasTangentHandles(TDocument const& document, uint32_t track, uint32_t key) const
	{
		if (!HasKey(document, {track, key}) || Traits::GetComponents(document, track) == 0)
		{
			return false;
		}
		return IsSelected(track, key) && Traits::GetKeyInterpolation(document, track, key) == CurveInterpolation::Cubic;
	}

	template<typename TDocument>
	std::optional<glm::vec2> CurveView<TDocument>::GetTangentHandle(TDocument const& document, uint32_t track, uint32_t key,
	                                                                uint32_t component, bool out) const
	{
		if (!HasTangentHandles(document, track, key))
		{
			return std::nullopt;
		}
		if ((out && key + 1 >= Traits::GetKeyCount(document, track)) || (!out && key == 0))
		{
			return std::nullopt;
		}
		std::optional<glm::vec2> const position = GetKeyPosition(document, track, key, component);
		if (!position)
		{
			return std::nullopt;
		}
		std::pair<glm::vec4, glm::vec4> const tangents = Traits::GetKeySlopes(document, track, key);
		double const slope = (out ? tangents.second : tangents.first)[static_cast<glm::length_t>(component)];
		glm::dvec2 direction(GetPixelsPerSecond(), -GetPixelsPerValue() * slope);
		double const length = glm::length(direction);
		if (!(length > 0.0) || !std::isfinite(length))
		{
			direction = glm::dvec2(1.0, 0.0);
		}
		else
		{
			direction /= length;
		}
		return *position + glm::vec2(direction) * (out ? HandleLength : -HandleLength) * m_FontSize;
	}

	template<typename TDocument>
	void CurveView<TDocument>::ZoomTime(float factor, float aboutX)
	{
		double const anchor = ToTime(aboutX);
		double const ratio = static_cast<double>(aboutX - m_Min.x) / static_cast<double>(m_Max.x - m_Min.x);
		double const span =
			std::clamp((m_TimeEnd - m_TimeStart) * factor, std::max(MinTimeSpan, std::abs(anchor) * RelativeMinSpan), MaxTimeSpan);
		double const start = anchor - ratio * span;
		if (std::optional<std::pair<double, double>> const range =
		        CurveViewDetail::FitRange(start, start + span, MinTimeSpan, MaxTimeSpan, MaxTimeMagnitude, RelativeMinSpan))
		{
			std::tie(m_TimeStart, m_TimeEnd) = *range;
		}
	}

	template<typename TDocument>
	void CurveView<TDocument>::ZoomValues(float factor, float aboutY)
	{
		double const anchor = ToValue(aboutY);
		double const ratio = static_cast<double>(m_Max.y - aboutY) / static_cast<double>(std::max(m_Max.y - GetCurvesTop(), 1.0f));
		double const span =
			std::clamp((m_ValueEnd - m_ValueStart) * factor, std::max(MinValueSpan, std::abs(anchor) * RelativeMinSpan), MaxValueSpan);
		double const start = anchor - ratio * span;
		if (std::optional<std::pair<double, double>> const range =
		        CurveViewDetail::FitRange(start, start + span, MinValueSpan, MaxValueSpan, MaxValueMagnitude, RelativeMinSpan))
		{
			std::tie(m_ValueStart, m_ValueEnd) = *range;
		}
	}

	template<typename TDocument>
	void CurveView<TDocument>::Pan(glm::vec2 pixels)
	{
		double const time = static_cast<double>(pixels.x) / GetPixelsPerSecond();
		double const value = static_cast<double>(pixels.y) / GetPixelsPerValue();
		if (std::optional<std::pair<double, double>> const times = CurveViewDetail::FitRange(
				m_TimeStart - time, m_TimeEnd - time, MinTimeSpan, MaxTimeSpan, MaxTimeMagnitude, RelativeMinSpan))
		{
			std::tie(m_TimeStart, m_TimeEnd) = *times;
		}
		if (std::optional<std::pair<double, double>> const values = CurveViewDetail::FitRange(
				m_ValueStart + value, m_ValueEnd + value, MinValueSpan, MaxValueSpan, MaxValueMagnitude, RelativeMinSpan))
		{
			std::tie(m_ValueStart, m_ValueEnd) = *values;
		}
	}

	template<typename TDocument>
	void CurveView<TDocument>::Frame(TDocument const& document)
	{
		double timeLow = std::numeric_limits<double>::infinity();
		double timeHigh = -std::numeric_limits<double>::infinity();
		double valueLow = std::numeric_limits<double>::infinity();
		double valueHigh = -std::numeric_limits<double>::infinity();
		auto const addValue = [&valueLow, &valueHigh](double value)
		{
			// A curve that overshoots past the floats evaluates to infinity: what can be framed is framed.
			if (std::isfinite(value))
			{
				valueLow = std::min(valueLow, value);
				valueHigh = std::max(valueHigh, value);
			}
		};
		uint32_t const trackCount = Traits::GetTrackCount(document);
		// The selection may name keys the document given does not have (a key added this frame, before its commit).
		bool const selected = std::any_of(m_Selection.begin(), m_Selection.end(),
		                                  [this, &document](CurveKeyRef const& key)
		                                  {
											  return HasKey(document, key);
										  });
		CurveDomain const domain = Traits::GetDomain(document);
		if (selected)
		{
			for (CurveKeyRef const& selectedKey : m_Selection)
			{
				if (!HasKey(document, selectedKey))
				{
					continue;
				}
				auto const [track, key] = selectedKey;
				double const time = Traits::GetKeyTime(document, track, key);
				timeLow = std::min(timeLow, time);
				timeHigh = std::max(timeHigh, time);
				for (uint32_t component = 0; component < Traits::GetComponents(document, track); component++)
				{
					if (IsVisible(document, track, component))
					{
						addValue(Traits::GetKeyValue(document, track, key, component));
					}
				}
			}
		}
		else
		{
			if (domain.Bounded)
			{
				timeLow = domain.Start;
				timeHigh = domain.End;
			}
			else
			{
				// The keys' span, of every visible track.
				for (uint32_t track = 0; track < trackCount; track++)
				{
					uint32_t const keyCount = Traits::GetKeyCount(document, track);
					if (keyCount == 0 || !IsVisible(document, track, 0))
					{
						continue;
					}
					timeLow = std::min(timeLow, static_cast<double>(Traits::GetKeyTime(document, track, 0)));
					timeHigh = std::max(timeHigh, static_cast<double>(Traits::GetKeyTime(document, track, keyCount - 1)));
				}
				if (!(timeLow <= timeHigh))
				{
					timeLow = 0.0;
					timeHigh = 1.0;
				}
			}
			double const extent = timeHigh - timeLow;
			for (uint32_t track = 0; track < trackCount; track++)
			{
				uint32_t const components = Traits::GetComponents(document, track);
				for (uint32_t component = 0; component < components; component++)
				{
					if (!IsVisible(document, track, component))
					{
						continue;
					}
					auto const index = static_cast<glm::length_t>(component);
					for (uint32_t key = 0; key < Traits::GetKeyCount(document, track); key++)
					{
						addValue(Traits::GetKeyValue(document, track, key, component));
					}
					for (int sample = 0; sample <= CurveViewDetail::FrameSamples; sample++)
					{
						double const time = timeLow + extent * sample / CurveViewDetail::FrameSamples;
						addValue(Traits::Evaluate(document, track, time)[index]);
					}
				}
			}
		}
		double const minimumTime = std::max(0.1 * (domain.Bounded ? domain.End - domain.Start : timeHigh - timeLow), 0.05);
		auto const [timeStart, timeEnd] = CurveViewDetail::Pad(timeLow, timeHigh, minimumTime);
		SetTimeRange(timeStart, timeEnd);
		if (std::isfinite(valueLow) && std::isfinite(valueHigh))
		{
			auto const [valueStart, valueEnd] = CurveViewDetail::Pad(valueLow, valueHigh, 2.0);
			SetValueRange(valueStart, valueEnd);
		}
		else if (!selected)
		{
			SetValueRange(-1.0, 1.0);
		}
	}

	template<typename TDocument>
	CurveHit CurveView<TDocument>::HitTest(TDocument const& document, glm::vec2 point) const
	{
		CurveHit hit;
		hit.Time = CurveViewDetail::ToFloat(ToTime(point.x));
		hit.Value = CurveViewDetail::ToFloat(ToValue(point.y));
		if (point.x < m_Min.x || point.y < m_Min.y || point.x > m_Max.x || point.y > m_Max.y)
		{
			return hit;
		}

		// Keys: the nearest diamond within reach.
		float best = HitRadius * m_FontSize;
		bool found = false;
		auto const consider = [&](CurveHitKind kind, uint32_t track, uint32_t key, uint32_t component, glm::vec2 position, bool manhattan)
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
		uint32_t const trackCount = Traits::GetTrackCount(document);
		for (uint32_t track = 0; track < trackCount; track++)
		{
			uint32_t const components = std::max<uint32_t>(Traits::GetComponents(document, track), 1);
			for (uint32_t component = 0; component < components; component++)
			{
				if (!IsVisible(document, track, component))
				{
					continue;
				}
				for (uint32_t key = 0; key < Traits::GetKeyCount(document, track); key++)
				{
					if (std::optional<glm::vec2> const position = GetKeyPosition(document, track, key, component))
					{
						consider(CurveHitKind::Key, track, key, component, *position, true);
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
			if (!HasTangentHandles(document, track, key))
			{
				continue;
			}
			for (uint32_t component = 0; component < Traits::GetComponents(document, track); component++)
			{
				if (!IsVisible(document, track, component))
				{
					continue;
				}
				for (bool const out : {false, true})
				{
					if (std::optional<glm::vec2> const handle = GetTangentHandle(document, track, key, component, out))
					{
						consider(out ? CurveHitKind::OutTangent : CurveHitKind::InTangent, track, key, component, *handle, false);
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
				hit.Kind = CurveHitKind::EventLane;
				hit.Track = track;
				return hit;
			}
		}

		// The curves: the nearest one at the point's time.
		if (point.y >= GetCurvesTop())
		{
			best = CurveHitRadius * m_FontSize;
			for (uint32_t track = 0; track < trackCount; track++)
			{
				uint32_t const components = Traits::GetComponents(document, track);
				if (components == 0)
				{
					continue;
				}
				glm::vec4 const value = Traits::Evaluate(document, track, ToTime(point.x));
				for (uint32_t component = 0; component < components; component++)
				{
					if (!IsVisible(document, track, component))
					{
						continue;
					}
					float const distance = std::abs(ToScreenY(value[static_cast<glm::length_t>(component)]) - point.y);
					if (distance <= best)
					{
						best = distance;
						hit.Kind = CurveHitKind::Curve;
						hit.Track = track;
						hit.Component = component;
					}
				}
			}
			if (hit.Kind == CurveHitKind::Curve)
			{
				return hit;
			}
		}
		hit.Kind = CurveHitKind::Empty;
		return hit;
	}

	template<typename TDocument>
	CurveTrackView CurveView<TDocument>::GetTrackView(std::string_view track) const
	{
		auto const found = std::find_if(m_TrackViews.begin(), m_TrackViews.end(),
		                                [track](NamedTrackView const& view)
		                                {
											return view.Name == track;
										});
		return found != m_TrackViews.end() ? found->View : CurveTrackView{};
	}

	template<typename TDocument>
	void CurveView<TDocument>::SetTrackVisible(std::string_view track, bool visible)
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

	template<typename TDocument>
	void CurveView<TDocument>::SetComponentVisible(std::string_view track, uint32_t component, bool visible)
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

	template<typename TDocument>
	bool CurveView<TDocument>::IsVisible(TDocument const& document, uint32_t track, uint32_t component) const
	{
		if (track >= Traits::GetTrackCount(document))
		{
			return false;
		}
		CurveTrackView const view = GetTrackView(Traits::GetTrackName(document, track));
		if (!view.Visible)
		{
			return false;
		}
		// An event track has its lane only; a curve its components.
		return Traits::GetComponents(document, track) == 0 ? component == 0 : component < 4 && view.Components[component];
	}

	template<typename TDocument>
	bool CurveView<TDocument>::IsSelected(uint32_t track, uint32_t key) const
	{
		return std::find(m_Selection.begin(), m_Selection.end(), CurveKeyRef(track, key)) != m_Selection.end();
	}

	template<typename TDocument>
	void CurveView<TDocument>::Select(std::vector<CurveKeyRef> keys)
	{
		std::sort(keys.begin(), keys.end());
		keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
		m_Selection = std::move(keys);
	}

	template<typename TDocument>
	std::optional<std::pair<glm::vec2, glm::vec2>> CurveView<TDocument>::GetMarquee() const
	{
		if (m_Gesture != CurveGesture::Marquee)
		{
			return std::nullopt;
		}
		return std::pair(glm::min(m_PressMouse, m_LastMouse), glm::max(m_PressMouse, m_LastMouse));
	}

	template<typename TDocument>
	double CurveView<TDocument>::Snap(double time) const
	{
		double const step = GetTimeStep();
		return std::round(time / step) * step;
	}

	template<typename TDocument>
	std::vector<typename CurveView<TDocument>::Intent> CurveView<TDocument>::Update(CurveViewInput const& input, TDocument const& document,
	                                                                                std::string_view title)
	{
		std::vector<Intent> intents;
		float const threshold = DragThreshold * m_FontSize;
		bool const moved = glm::length(input.Mouse - m_PressMouse) > threshold;
		if (input.Hovered && input.Wheel != 0.0f && m_Gesture == CurveGesture::Idle)
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
			case CurveGesture::Idle:
				if (input.Hovered && input.Pressed[0])
				{
					BeginPress(input, document, title, intents);
				}
				else if (input.Hovered && (input.Pressed[1] || input.Pressed[2]))
				{
					m_Gesture = CurveGesture::PanPress;
					m_Button = input.Pressed[1] ? 1 : 2;
					m_PressMouse = input.Mouse;
					m_PressHit = HitTest(document, input.Mouse);
				}
				break;
			case CurveGesture::Press:
				if (!input.Down[0])
				{
					// A click: a key alone becomes the selection; empty space clears it (Shift and Ctrl acted at the press).
					if (m_PressHit.Kind == CurveHitKind::Key && !m_PressShift && !m_PressCtrl)
					{
						Select({{m_PressHit.Track, m_PressHit.Key}});
					}
					else if (m_PressHit.Kind != CurveHitKind::Key && m_PressHit.Kind != CurveHitKind::InTangent &&
					         m_PressHit.Kind != CurveHitKind::OutTangent && !m_PressShift && !m_PressCtrl)
					{
						m_Selection.clear();
					}
					m_Gesture = CurveGesture::Idle;
				}
				else if (moved)
				{
					m_Start = document;
					m_StartSelection = m_Selection;
					m_LastEdit.reset();
					switch (m_PressHit.Kind)
					{
						case CurveHitKind::Key:
							// A Ctrl press toggled the key: it moves nothing.
							m_Gesture = m_PressCtrl ? CurveGesture::Idle : CurveGesture::MoveKeys;
							break;
						case CurveHitKind::InTangent:
						case CurveHitKind::OutTangent:
							m_Gesture = CurveGesture::Tangent;
							break;
						case CurveHitKind::None:
						case CurveHitKind::EventLane:
						case CurveHitKind::Curve:
						case CurveHitKind::Empty:
							m_Gesture = CurveGesture::Marquee;
							break;
					}
					m_LastMouse = input.Mouse;
					if (m_Gesture == CurveGesture::Marquee)
					{
						UpdateMarquee(document);
					}
					else if (m_Gesture != CurveGesture::Idle)
					{
						UpdateDrag(input, title, intents);
					}
				}
				break;
			case CurveGesture::MoveKeys:
			case CurveGesture::Tangent:
				if (!input.Down[0])
				{
					if (m_LastEdit)
					{
						intents.push_back(Intent{CurveIntentKind::EndEdit, {}, {}});
					}
					m_LastEdit.reset();
					m_Gesture = CurveGesture::Idle;
				}
				else
				{
					UpdateDrag(input, title, intents);
				}
				break;
			case CurveGesture::Marquee:
				m_LastMouse = input.Mouse;
				if (!input.Down[0])
				{
					m_Gesture = CurveGesture::Idle;
				}
				else
				{
					UpdateMarquee(document);
				}
				break;
			case CurveGesture::PanPress:
				if (!input.Down[static_cast<size_t>(m_Button)])
				{
					// A right click on a key opens the menu of the selected keys, the key among them.
					if (m_Button == 1 && m_PressHit.Kind == CurveHitKind::Key)
					{
						if (!IsSelected(m_PressHit.Track, m_PressHit.Key))
						{
							Select({{m_PressHit.Track, m_PressHit.Key}});
						}
						intents.push_back(Intent{CurveIntentKind::OpenKeyMenu, {}, {}});
					}
					m_Gesture = CurveGesture::Idle;
				}
				else if (moved)
				{
					m_Gesture = CurveGesture::Pan;
					Pan(input.Mouse - m_PressMouse);
				}
				break;
			case CurveGesture::Pan:
				Pan(input.Mouse - m_LastMouse);
				if (!input.Down[static_cast<size_t>(m_Button)])
				{
					m_Gesture = CurveGesture::Idle;
				}
				break;
		}
		m_LastMouse = input.Mouse;
		return intents;
	}

	template<typename TDocument>
	void CurveView<TDocument>::BeginPress(CurveViewInput const& input, TDocument const& document, std::string_view title,
	                                      std::vector<Intent>& intents)
	{
		CurveHit const hit = HitTest(document, input.Mouse);
		if (input.DoubleClicked)
		{
			// A double-click on a curve adds a key with the curve's value there; on an event lane, an event.
			if (hit.Kind == CurveHitKind::Curve || hit.Kind == CurveHitKind::EventLane)
			{
				if (std::optional<Intent> added = AddKey(document, hit.Track, hit.Time, input.Alt, title))
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
		if (hit.Kind == CurveHitKind::Key)
		{
			CurveKeyRef const key(hit.Track, hit.Key);
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
		m_Gesture = CurveGesture::Press;
	}

	template<typename TDocument>
	void CurveView<TDocument>::UpdateDrag(CurveViewInput const& input, std::string_view title, std::vector<Intent>& intents)
	{
		uint32_t const track = m_PressHit.Track;
		uint32_t const key = m_PressHit.Key;
		uint32_t const component = m_PressHit.Component;
		if (!HasKey(m_Start, {track, key}))
		{
			return;
		}
		uint32_t const components = Traits::GetComponents(m_Start, track);
		float const grabbedTime = Traits::GetKeyTime(m_Start, track, key);
		TDocument edited = m_Start;
		std::string action;
		if (m_Gesture == CurveGesture::MoveKeys)
		{
			glm::vec2 const delta = input.Mouse - m_PressMouse;
			double timeOffset = delta.x / GetPixelsPerSecond();
			double valueOffset = components > 0 ? -delta.y / GetPixelsPerValue() : 0.0;
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
				timeOffset = Snap(grabbedTime + timeOffset) - grabbedTime;
			}
			glm::vec4 offset(0.0f);
			if (components > 0 && component < components)
			{
				offset[static_cast<glm::length_t>(component)] = CurveViewDetail::ToFloat(valueOffset);
			}
			if (!Traits::MoveKeys(edited, m_StartSelection, CurveViewDetail::ToFloat(timeOffset), offset))
			{
				return;
			}
			action = DescribeKeys(m_Start, m_StartSelection, "Move", title);
		}
		else
		{
			bool const out = m_PressHit.Kind == CurveHitKind::OutTangent;
			float const keyX = ToScreenX(grabbedTime);
			float const keyY = ToScreenY(Traits::GetKeyValue(m_Start, track, key, component));
			// The handle stays on its side of the key.
			float const dx = out ? std::max(input.Mouse.x - keyX, 1.0f) : std::min(input.Mouse.x - keyX, -1.0f);
			double const slope =
				(static_cast<double>(keyY - input.Mouse.y) / GetPixelsPerValue()) / (static_cast<double>(dx) / GetPixelsPerSecond());
			if (!std::isfinite(slope))
			{
				return;
			}
			std::pair<glm::vec4, glm::vec4> const tangents = Traits::GetKeySlopes(m_Start, track, key);
			glm::vec4 in = tangents.first;
			glm::vec4 outgoing = tangents.second;
			auto const index = static_cast<glm::length_t>(component);
			std::optional<glm::vec4> newIn = Traits::GetKeyIn(m_Start, track, key);
			std::optional<glm::vec4> newOut = Traits::GetKeyOut(m_Start, track, key);
			if (input.Alt)
			{
				// Broken: only the dragged side.
				(out ? outgoing : in)[index] = CurveViewDetail::ToFloat(slope);
				(out ? newOut : newIn) = out ? outgoing : in;
			}
			else
			{
				in[index] = CurveViewDetail::ToFloat(slope);
				outgoing[index] = CurveViewDetail::ToFloat(slope);
				newIn = in;
				newOut = outgoing;
			}
			if (!Traits::SetKeyTangents(edited, {track, key}, newIn, newOut))
			{
				return;
			}
			action = fmt::format("Edit Tangent of {} on {}", Traits::GetTrackName(m_Start, track), title);
		}
		if (m_LastEdit && *m_LastEdit == edited)
		{
			return;
		}
		m_LastEdit = edited;
		intents.push_back(Intent{CurveIntentKind::Edit, std::move(edited), std::move(action)});
	}

	template<typename TDocument>
	void CurveView<TDocument>::UpdateMarquee(TDocument const& document)
	{
		std::vector<CurveKeyRef> const inside =
			FindKeysIn(document, glm::min(m_PressMouse, m_LastMouse), glm::max(m_PressMouse, m_LastMouse));
		std::vector<CurveKeyRef> selection;
		if (m_PressShift)
		{
			selection = m_StartSelection;
			selection.insert(selection.end(), inside.begin(), inside.end());
		}
		else if (m_PressCtrl)
		{
			selection = m_StartSelection;
			std::erase_if(selection,
			              [&inside](CurveKeyRef const& key)
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

	template<typename TDocument>
	std::vector<CurveKeyRef> CurveView<TDocument>::FindKeysIn(TDocument const& document, glm::vec2 min, glm::vec2 max) const
	{
		std::vector<CurveKeyRef> keys;
		uint32_t const trackCount = Traits::GetTrackCount(document);
		for (uint32_t track = 0; track < trackCount; track++)
		{
			uint32_t const components = std::max<uint32_t>(Traits::GetComponents(document, track), 1);
			for (uint32_t key = 0; key < Traits::GetKeyCount(document, track); key++)
			{
				for (uint32_t component = 0; component < components; component++)
				{
					if (!IsVisible(document, track, component))
					{
						continue;
					}
					std::optional<glm::vec2> const position = GetKeyPosition(document, track, key, component);
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

	template<typename TDocument>
	void CurveView<TDocument>::SelectAll(TDocument const& document)
	{
		std::vector<CurveKeyRef> keys;
		uint32_t const trackCount = Traits::GetTrackCount(document);
		for (uint32_t track = 0; track < trackCount; track++)
		{
			bool visible = false;
			for (uint32_t component = 0; component < std::max<uint32_t>(Traits::GetComponents(document, track), 1); component++)
			{
				visible = visible || IsVisible(document, track, component);
			}
			if (!visible)
			{
				continue;
			}
			for (uint32_t key = 0; key < Traits::GetKeyCount(document, track); key++)
			{
				keys.emplace_back(track, key);
			}
		}
		Select(std::move(keys));
	}

	template<typename TDocument>
	std::vector<CurveKeyRef> CurveView<TDocument>::GetSelectedCurveKeys(TDocument const& document) const
	{
		std::vector<CurveKeyRef> curves;
		std::copy_if(m_Selection.begin(), m_Selection.end(), std::back_inserter(curves),
		             [&document](CurveKeyRef const& key)
		             {
						 return key.first < Traits::GetTrackCount(document) && Traits::GetComponents(document, key.first) > 0;
					 });
		return curves;
	}

	template<typename TDocument>
	std::optional<typename CurveView<TDocument>::Intent> CurveView<TDocument>::DeleteSelection(TDocument const& document,
	                                                                                           std::string_view title)
	{
		if (m_Selection.empty())
		{
			return std::nullopt;
		}
		TDocument edited = document;
		if (!Traits::RemoveKeys(edited, m_Selection))
		{
			return std::nullopt;
		}
		std::string action = DescribeKeys(document, m_Selection, "Delete", title);
		m_Selection.clear();
		return Intent{CurveIntentKind::Commit, std::move(edited), std::move(action)};
	}

	template<typename TDocument>
	std::optional<typename CurveView<TDocument>::Intent> CurveView<TDocument>::Nudge(TDocument const& document, int steps,
	                                                                                 std::string_view title)
	{
		if (m_Selection.empty() || steps == 0)
		{
			return std::nullopt;
		}
		TDocument edited = document;
		float offset = 0.0f;
		if constexpr (TimeStep > 0.0f)
		{
			offset = static_cast<float>(steps) * TimeStep;
		}
		else
		{
			offset = CurveViewDetail::ToFloat(static_cast<double>(steps) * GetTimeStep());
		}
		if (!Traits::MoveKeys(edited, m_Selection, offset, glm::vec4(0.0f)) || edited == document)
		{
			return std::nullopt;
		}
		return Intent{CurveIntentKind::Commit, std::move(edited), DescribeKeys(document, m_Selection, "Move", title)};
	}

	template<typename TDocument>
	std::optional<typename CurveView<TDocument>::Intent>
	CurveView<TDocument>::SetInterpolation(TDocument const& document, CurveInterpolation interpolation, std::string_view title)
	{
		TDocument edited = document;
		if (!Traits::SetKeyInterpolation(edited, m_Selection, interpolation))
		{
			return std::nullopt;
		}
		std::vector<CurveKeyRef> const curves = GetSelectedCurveKeys(document);
		return Intent{
			CurveIntentKind::Commit, std::move(edited),
			DescribeKeys(document, curves, "Set", title, fmt::format(" to {}", CurveViewDetail::GetInterpolationLabel(interpolation)))};
	}

	template<typename TDocument>
	std::optional<typename CurveView<TDocument>::Intent> CurveView<TDocument>::FlattenTangents(TDocument const& document,
	                                                                                           std::string_view title)
	{
		TDocument edited = document;
		if (!Traits::FlattenTangents(edited, m_Selection))
		{
			return std::nullopt;
		}
		std::vector<CurveKeyRef> const curves = GetSelectedCurveKeys(document);
		return Intent{CurveIntentKind::Commit, std::move(edited), DescribeKeys(document, curves, "Flatten Tangents of", title)};
	}

	template<typename TDocument>
	std::optional<typename CurveView<TDocument>::Intent> CurveView<TDocument>::AutoTangents(TDocument const& document,
	                                                                                        std::string_view title)
	{
		TDocument edited = document;
		if (!Traits::ResetTangents(edited, m_Selection))
		{
			return std::nullopt;
		}
		std::vector<CurveKeyRef> const curves = GetSelectedCurveKeys(document);
		return Intent{CurveIntentKind::Commit, std::move(edited), DescribeKeys(document, curves, "Set Tangents of", title, " to Auto")};
	}

	template<typename TDocument>
	std::optional<typename CurveView<TDocument>::Intent> CurveView<TDocument>::AddKey(TDocument const& document, uint32_t track,
	                                                                                  double time, bool free, std::string_view title)
	{
		if (track >= Traits::GetTrackCount(document))
		{
			return std::nullopt;
		}
		CurveDomain const domain = Traits::GetDomain(document);
		double const at = std::clamp(free ? time : Snap(time), domain.Start, domain.End);
		TDocument edited = document;
		Result<uint32_t> const added = Traits::AddKey(edited, track, static_cast<float>(at), Traits::Evaluate(document, track, at));
		if (!added)
		{
			return std::nullopt;
		}
		Select({{track, added.GetValue()}});
		return Intent{CurveIntentKind::Commit, std::move(edited),
		              fmt::format("Add Key to {} on {}", Traits::GetTrackName(document, track), title)};
	}

	template<typename TDocument>
	std::vector<typename CurveView<TDocument>::Intent> CurveView<TDocument>::CancelGesture()
	{
		std::vector<Intent> intents;
		switch (m_Gesture)
		{
			case CurveGesture::MoveKeys:
			case CurveGesture::Tangent:
				if (m_LastEdit)
				{
					intents.push_back(Intent{CurveIntentKind::CancelEdit, m_Start, {}});
				}
				break;
			case CurveGesture::Press:
			case CurveGesture::Marquee:
				// What the press or the marquee did to the selection is undone.
				m_Selection = m_StartSelection;
				break;
			case CurveGesture::Idle:
			case CurveGesture::PanPress:
			case CurveGesture::Pan:
				break;
		}
		m_LastEdit.reset();
		m_Gesture = CurveGesture::Idle;
		return intents;
	}

	template<typename TDocument>
	std::string CurveView<TDocument>::DescribeKeys(TDocument const& document, std::span<CurveKeyRef const> keys, std::string_view verb,
	                                               std::string_view title, std::string_view suffix)
	{
		if (keys.size() == 1 && keys.front().first < Traits::GetTrackCount(document))
		{
			return fmt::format("{} Key of {}{} on {}", verb, Traits::GetTrackName(document, keys.front().first), suffix, title);
		}
		return fmt::format("{} {} Keys{} on {}", verb, keys.size(), suffix, title);
	}
}
