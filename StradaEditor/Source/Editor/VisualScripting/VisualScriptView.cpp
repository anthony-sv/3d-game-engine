#include "Editor/VisualScripting/VisualScriptView.h"

#include "Strada/Asset/VisualScriptSerializer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace Strada
{
	static_assert(VisualScriptMaxCenter == static_cast<float>(VisualScriptSerializer::MaxCoordinate));

	namespace
	{
		// A zoom within this of a wheel level counts as on it, so a zoom that lost a bit on its way (a framing, a view read back
		// from the settings) still steps to the next level instead of to the one it is on.
		constexpr float ZoomLevelTolerance = 1.0e-4f;
		// Wheel notches that add up to within this of a whole number make that number of notches: trackpads send fractions
		// such as 1/120 whose sums are not exact.
		constexpr double WheelNotchTolerance = 1.0e-4;

		struct DetailBand
		{
			VisualScriptDetailLevel Level;
			// The zooms of the level: [Lowest, Highest).
			float Lowest;
			float Highest;
		};

		constexpr std::array<DetailBand, 4> DetailBands = {{
			{VisualScriptDetailLevel::Far, 0.0f, VisualScriptOverviewZoom},
			{VisualScriptDetailLevel::Overview, VisualScriptOverviewZoom, VisualScriptCompactZoom},
			{VisualScriptDetailLevel::Compact, VisualScriptCompactZoom, VisualScriptDetailZoom},
			{VisualScriptDetailLevel::Detail, VisualScriptDetailZoom, VisualScriptMaxZoom},
		}};

		bool IsFinite(glm::vec2 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		float ClampZoom(float zoom)
		{
			return std::clamp(zoom, VisualScriptMinZoom, VisualScriptMaxZoom);
		}

		glm::vec2 ClampCenter(glm::dvec2 center)
		{
			glm::dvec2 const limit(static_cast<double>(VisualScriptMaxCenter));
			return glm::vec2(glm::clamp(center, -limit, limit));
		}

		// The state moved into its ranges; numbers that are not finite take the default state's.
		VisualScriptViewState Sanitize(VisualScriptViewState state)
		{
			VisualScriptViewState const defaults;
			state.Center.x = std::isfinite(state.Center.x) ? state.Center.x : defaults.Center.x;
			state.Center.y = std::isfinite(state.Center.y) ? state.Center.y : defaults.Center.y;
			state.Center = ClampCenter(glm::dvec2(state.Center));
			state.Zoom = std::isfinite(state.Zoom) ? ClampZoom(state.Zoom) : defaults.Zoom;
			return state;
		}

		// The wheel level next to zoom in a direction (+1 in, -1 out); the end of the range when there is none.
		float GetNextZoomLevel(float zoom, int direction)
		{
			if (direction > 0)
			{
				auto const next = std::find_if(VisualScriptZoomLevels.begin(), VisualScriptZoomLevels.end(),
				                               [zoom](float level)
				                               {
												   return level > zoom + ZoomLevelTolerance;
											   });
				return next != VisualScriptZoomLevels.end() ? *next : VisualScriptMaxZoom;
			}
			auto const next = std::find_if(VisualScriptZoomLevels.rbegin(), VisualScriptZoomLevels.rend(),
			                               [zoom](float level)
			                               {
											   return level < zoom - ZoomLevelTolerance;
										   });
			return next != VisualScriptZoomLevels.rend() ? *next : VisualScriptMinZoom;
		}
	}

	VisualScriptDetailLevel GetVisualScriptDetailLevel(float zoom)
	{
		if (zoom >= VisualScriptDetailZoom)
		{
			return VisualScriptDetailLevel::Detail;
		}
		if (zoom >= VisualScriptCompactZoom)
		{
			return VisualScriptDetailLevel::Compact;
		}
		if (zoom >= VisualScriptOverviewZoom)
		{
			return VisualScriptDetailLevel::Overview;
		}
		return VisualScriptDetailLevel::Far;
	}

	VisualScriptDetailLevel GetVisualScriptDetailLevel(float zoom, VisualScriptDetailLevel current)
	{
		for (DetailBand const& band : DetailBands)
		{
			if (band.Level != current)
			{
				continue;
			}
			// The lowest and highest levels extend without bound on their open side.
			bool const aboveLowest = band.Level == VisualScriptDetailLevel::Far || zoom >= band.Lowest - VisualScriptDetailHysteresis;
			bool const belowHighest = band.Level == VisualScriptDetailLevel::Detail || zoom < band.Highest + VisualScriptDetailHysteresis;
			if (aboveLowest && belowHighest)
			{
				return current;
			}
		}
		return GetVisualScriptDetailLevel(zoom);
	}

	VisualScriptView::VisualScriptView(VisualScriptViewState const& state)
		: m_State(Sanitize(state)),
		  m_Level(GetVisualScriptDetailLevel(m_State.Zoom))
	{
	}

	void VisualScriptView::SetCanvas(glm::vec2 canvasMin, glm::vec2 canvasSize, float uiScale)
	{
		m_CanvasMin = IsFinite(canvasMin) ? canvasMin : glm::vec2(0.0f);
		m_CanvasSize = IsFinite(canvasSize) ? glm::max(canvasSize, glm::vec2(0.0f)) : glm::vec2(0.0f);
		m_UIScale = std::isfinite(uiScale) && uiScale > 0.0f ? uiScale : 1.0f;
	}

	void VisualScriptView::SetState(VisualScriptViewState const& state)
	{
		// Copied before the animation ends: state may be its target (GetTargetState).
		VisualScriptViewState const sanitized = Sanitize(state);
		m_Animation.reset();
		m_WheelNotches = 0.0;
		m_State.Function = sanitized.Function;
		m_State.Center = sanitized.Center;
		SetZoom(sanitized.Zoom);
	}

	glm::vec2 VisualScriptView::ToScreen(glm::vec2 canvasPoint) const
	{
		return GetScreenCenter() + (canvasPoint - m_State.Center) * GetPixelsPerUnit();
	}

	glm::vec2 VisualScriptView::ToCanvas(glm::vec2 screenPoint) const
	{
		return m_State.Center + (screenPoint - GetScreenCenter()) / GetPixelsPerUnit();
	}

	VisualScriptRect VisualScriptView::GetVisibleRect() const
	{
		return VisualScriptRect{ToCanvas(m_CanvasMin), ToCanvas(m_CanvasMin + m_CanvasSize)};
	}

	void VisualScriptView::ZoomAt(glm::vec2 screenPoint, float zoom)
	{
		if (!IsFinite(screenPoint) || !std::isfinite(zoom))
		{
			return;
		}
		m_Animation.reset();
		// In double, so the anchor stays under the point to well below a pixel even far from the origin.
		glm::dvec2 const offset = glm::dvec2(screenPoint) - glm::dvec2(GetScreenCenter());
		glm::dvec2 const anchor = glm::dvec2(m_State.Center) + offset / static_cast<double>(GetPixelsPerUnit());
		SetZoom(ClampZoom(zoom));
		SetCenter(anchor - offset / static_cast<double>(GetPixelsPerUnit()));
	}

	void VisualScriptView::StepZoom(int levels, glm::vec2 screenPoint)
	{
		if (levels == 0)
		{
			return;
		}
		int const direction = levels > 0 ? 1 : -1;
		// More steps than there are levels end at the end of the range anyway.
		size_t const steps = std::min(static_cast<size_t>(std::abs(static_cast<int64_t>(levels))), VisualScriptZoomLevels.size());
		float zoom = m_State.Zoom;
		for (size_t step = 0; step < steps; step++)
		{
			zoom = GetNextZoomLevel(zoom, direction);
		}
		ZoomAt(screenPoint, zoom);
	}

	void VisualScriptView::ZoomByWheel(float notches, glm::vec2 screenPoint)
	{
		if (!std::isfinite(notches) || notches == 0.0f || !IsFinite(screenPoint))
		{
			return;
		}
		m_Animation.reset();
		if ((notches > 0.0f) != (m_WheelNotches > 0.0))
		{
			m_WheelNotches = 0.0;
		}
		m_WheelNotches += static_cast<double>(notches);
		double const whole = std::trunc(m_WheelNotches + std::copysign(WheelNotchTolerance, m_WheelNotches));
		if (whole == 0.0)
		{
			return;
		}
		m_WheelNotches -= whole;
		// Whole notches beyond the number of levels change nothing more (and must not overflow an int).
		double const levels =
			std::clamp(whole, -static_cast<double>(VisualScriptZoomLevels.size()), static_cast<double>(VisualScriptZoomLevels.size()));
		StepZoom(static_cast<int>(levels), screenPoint);
	}

	void VisualScriptView::Pan(glm::vec2 screenDelta)
	{
		if (!IsFinite(screenDelta) || screenDelta == glm::vec2(0.0f))
		{
			return;
		}
		m_Animation.reset();
		SetCenter(glm::dvec2(m_State.Center) - glm::dvec2(screenDelta) / static_cast<double>(GetPixelsPerUnit()));
	}

	void VisualScriptView::Scroll(glm::vec2 wheel, glm::vec2 mouse, VisualScriptScrollMode mode, bool ctrl, bool shift)
	{
		if (!IsFinite(wheel) || wheel == glm::vec2(0.0f))
		{
			return;
		}
		bool zooms = false;
		switch (mode)
		{
			case VisualScriptScrollMode::Zoom:
				zooms = !ctrl && !shift && wheel.x == 0.0f;
				break;
			case VisualScriptScrollMode::Pan:
				zooms = ctrl;
				break;
		}
		if (zooms)
		{
			// In the Pan mode, Ctrl with a purely horizontal scroll has nothing to zoom by.
			ZoomByWheel(wheel.y, mouse);
			return;
		}
		// ImGui's rule for scrolling windows (imgui.cpp, UpdateMouseInputs and UpdateMouseWheel): Shift moves the vertical
		// scroll to the horizontal axis. macOS does it itself, so the scroll then arrives horizontal already.
		glm::vec2 const delta = shift && wheel.y != 0.0f ? glm::vec2(wheel.y, 0.0f) : wheel;
		// A positive scroll moves the view up or left, so the canvas follows the scroll down or right.
		Pan(delta * WheelPanUnits * m_UIScale);
	}

	void VisualScriptView::AutoPan(glm::vec2 mouse, float bandPixels, float maxSpeedPixels, float deltaSeconds)
	{
		if (!IsFinite(mouse) || !(bandPixels > 0.0f) || !(maxSpeedPixels > 0.0f) || !(deltaSeconds > 0.0f) || !std::isfinite(bandPixels) ||
		    !std::isfinite(maxSpeedPixels) || !std::isfinite(deltaSeconds))
		{
			return;
		}
		// The speed toward each edge, in screen pixels per second: positive moves the view right or down.
		glm::vec2 speed(0.0f);
		for (glm::length_t axis = 0; axis < 2; axis++)
		{
			float const towardLow = mouse[axis] - m_CanvasMin[axis];
			float const towardHigh = m_CanvasMin[axis] + m_CanvasSize[axis] - mouse[axis];
			// A canvas narrower than two bands pans toward the nearer edge.
			if (towardLow < bandPixels && towardLow <= towardHigh)
			{
				speed[axis] = -maxSpeedPixels * std::min(bandPixels - towardLow, bandPixels) / bandPixels;
			}
			else if (towardHigh < bandPixels)
			{
				speed[axis] = maxSpeedPixels * std::min(bandPixels - towardHigh, bandPixels) / bandPixels;
			}
		}
		// The view moving toward an edge is the canvas moving away from it.
		Pan(-speed * deltaSeconds);
	}

	VisualScriptViewState VisualScriptView::GetFrameTarget(std::optional<VisualScriptRect> const& bounds) const
	{
		VisualScriptViewState target;
		target.Function = m_State.Function;
		if (!bounds || !IsFinite(bounds->Min) || !IsFinite(bounds->Max))
		{
			return target;
		}
		glm::vec2 const lowest = glm::min(bounds->Min, bounds->Max);
		glm::vec2 const highest = glm::max(bounds->Min, bounds->Max);
		glm::vec2 const framed = (highest - lowest + 2.0f * FrameMargin) * m_UIScale;
		// The zoom at which the framed size fills the canvas in each axis; an empty canvas fits nothing.
		glm::vec2 const fit = m_CanvasSize / framed;
		target.Center = ClampCenter((glm::dvec2(lowest) + glm::dvec2(highest)) * 0.5);
		target.Zoom = ClampZoom(std::min({fit.x, fit.y, MaxFrameZoom}));
		return target;
	}

	VisualScriptViewState VisualScriptView::GetOpenTarget(VisualScriptOpenRequest const& request,
	                                                      std::optional<VisualScriptRect> const& frameBounds) const
	{
		VisualScriptViewState target = request.Frame == VisualScriptFrame::Keep ? GetTargetState() : GetFrameTarget(frameBounds);
		if (request.Zoom && std::isfinite(*request.Zoom))
		{
			target.Zoom = ClampZoom(*request.Zoom);
		}
		if (request.Center && IsFinite(*request.Center))
		{
			target.Center = ClampCenter(glm::dvec2(*request.Center));
		}
		return target;
	}

	void VisualScriptView::Frame(std::optional<VisualScriptRect> const& bounds, double time, double duration)
	{
		AnimateTo(GetFrameTarget(bounds), time, duration);
	}

	void VisualScriptView::AnimateTo(VisualScriptViewState const& target, double time, double duration)
	{
		VisualScriptViewState const end = Sanitize(target);
		if (!(duration > 0.0) || !std::isfinite(duration) || !std::isfinite(time) || end.Function != m_State.Function)
		{
			// SetState would also drop a partial wheel notch, which a programmatic move has no reason to.
			m_Animation.reset();
			m_State.Function = end.Function;
			m_State.Center = end.Center;
			SetZoom(end.Zoom);
			return;
		}
		if (end == m_State)
		{
			m_Animation.reset();
			return;
		}
		m_Animation = Animation{m_State, end, time, duration, time + duration};
	}

	void VisualScriptView::Advance(double time)
	{
		if (!m_Animation)
		{
			return;
		}
		Animation const& animation = *m_Animation;
		// Also ends the move for a time that is not finite.
		if (!(time < animation.EndTime))
		{
			VisualScriptViewState const target = animation.Target;
			m_Animation.reset();
			m_State.Center = target.Center;
			SetZoom(target.Zoom);
			return;
		}
		double const progress = std::max(time - animation.StartTime, 0.0) / animation.Duration;
		double const remaining = 1.0 - progress;
		double const eased = 1.0 - remaining * remaining * remaining;
		glm::dvec2 const startCenter(animation.Start.Center);
		glm::dvec2 const targetCenter(animation.Target.Center);
		double const startZoom = std::log(static_cast<double>(animation.Start.Zoom));
		double const targetZoom = std::log(static_cast<double>(animation.Target.Zoom));
		SetCenter(startCenter + (targetCenter - startCenter) * eased);
		SetZoom(ClampZoom(static_cast<float>(std::exp(startZoom + (targetZoom - startZoom) * eased))));
	}

	void VisualScriptView::SetCenter(glm::dvec2 center)
	{
		m_State.Center = ClampCenter(center);
	}

	void VisualScriptView::SetZoom(float zoom)
	{
		m_State.Zoom = zoom;
		m_Level = GetVisualScriptDetailLevel(zoom, m_Level);
	}
}
