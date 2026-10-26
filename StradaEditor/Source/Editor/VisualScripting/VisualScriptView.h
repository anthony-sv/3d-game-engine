#pragma once

#include "Editor/VisualScripting/VisualScriptEditorTypes.h"

#include "Strada/Core/PlatformDetection.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace Strada
{
	// What a graph window shows of one sub-graph: which one, and where its view is.
	struct VisualScriptViewState
	{
		// 0 is the event graph, else a function's ID.
		uint32_t Function = 0;
		// The canvas point at the centre of the view, in canvas units (the units of positions in documents).
		glm::vec2 Center{0.0f};
		// Screen pixels per canvas unit at a UI scale of 1.
		float Zoom = 1.0f;

		bool operator==(VisualScriptViewState const& other) const = default;
	};

	// The zoom levels a wheel notch steps between; every view's zoom lies within the first and the last.
	inline constexpr std::array<float, 16> VisualScriptZoomLevels = {0.10f, 0.15f, 0.20f, 0.25f, 0.33f, 0.40f, 0.50f, 0.60f,
	                                                                 0.70f, 0.80f, 0.90f, 1.00f, 1.25f, 1.50f, 1.75f, 2.00f};
	inline constexpr float VisualScriptMinZoom = VisualScriptZoomLevels.front();
	inline constexpr float VisualScriptMaxZoom = VisualScriptZoomLevels.back();
	// A view's centre may reach every point a document can place something at (VisualScriptSerializer::MaxCoordinate), and no
	// further.
	inline constexpr float VisualScriptMaxCenter = 1.0e7f;

	// What the mouse wheel and trackpad scrolling do on the canvas (a per-user choice, so a trackpad navigates as well as a
	// wheel mouse).
	enum class VisualScriptScrollMode : uint8_t
	{
		// The wheel zooms to the cursor; Ctrl+wheel pans vertically, Shift+wheel and the horizontal wheel horizontally, and
		// a diagonal trackpad swipe pans.
		Zoom = 0,
		// Wheel and trackpad deltas pan in both axes; Ctrl+wheel (Cmd on macOS) zooms to the cursor.
		Pan
	};

	// The mode of users who never chose one: Pan on macOS, where trackpads are the usual device, else Zoom.
	constexpr VisualScriptScrollMode GetDefaultVisualScriptScrollMode()
	{
#if defined(ST_PLATFORM_MACOS)
		return VisualScriptScrollMode::Pan;
#else
		return VisualScriptScrollMode::Zoom;
#endif
	}

	// An axis-aligned rectangle: in canvas units, or in screen pixels where a function says so. Min is the top-left corner
	// (y grows downwards on the canvas as on the screen), Max the bottom-right one.
	struct VisualScriptRect
	{
		glm::vec2 Min{0.0f};
		glm::vec2 Max{0.0f};

		glm::vec2 GetSize() const { return Max - Min; }
		glm::vec2 GetCenter() const { return (Min + Max) * 0.5f; }

		bool operator==(VisualScriptRect const& other) const = default;
	};

	// How much of each node the canvas draws (and how much the user can do there), chosen by the zoom alone.
	enum class VisualScriptDetailLevel : uint8_t
	{
		// Nodes are rectangles of their header's hue without titles; pins are neither drawn nor hit. Select and move.
		Far = 0,
		// Nodes are boxes with their title; pins are dots. Select, move and wire.
		Overview,
		// Full nodes without subtitles; labels and values are text (values are edited in Details).
		Compact,
		// Everything: subtitles, labels, value chips and the live widget that edits a value in place.
		Detail
	};

	// The lowest zoom of each level above Far. Each lies between two wheel levels, so a wheel notch never lands on one.
	inline constexpr float VisualScriptOverviewZoom = 0.175f;
	inline constexpr float VisualScriptCompactZoom = 0.45f;
	inline constexpr float VisualScriptDetailZoom = 0.65f;
	// A view leaves its level only once the zoom passes the level's threshold by this much, so framing zooms and trackpads
	// that end near a threshold do not flicker between two levels.
	inline constexpr float VisualScriptDetailHysteresis = 0.02f;

	// The level of a zoom, from the thresholds alone.
	VisualScriptDetailLevel GetVisualScriptDetailLevel(float zoom);
	// The level of a view at zoom that was at level current: current while the zoom stays within current's zooms widened by
	// VisualScriptDetailHysteresis on both sides, else the level of the zoom.
	VisualScriptDetailLevel GetVisualScriptDetailLevel(float zoom, VisualScriptDetailLevel current);

	// The view of a graph window's canvas: the sub-graph it shows and where (centre and zoom), where the canvas is on the
	// screen, its level of detail, the wheel's partial notches, and the animation of programmatic moves.
	//
	// The screen point of canvas point p is the canvas's centre + (p - Center) × Zoom × UI scale (screen and canvas y both
	// grow downwards); GetPixelsPerUnit is that factor. Zoom stays within VisualScriptMinZoom and VisualScriptMaxZoom and the
	// centre within ±VisualScriptMaxCenter whatever the input. Pure (no ImGui): the editor model sets the canvas rectangle
	// and passes the input and the time. Main thread only.
	class VisualScriptView
	{
	public:
		// How long a programmatic move (framing, bookmarks, back and forward, find, graph.open) takes: an ease-out cubic of
		// the centre, with the zoom moving along the same curve in log space.
		static constexpr double AnimationSeconds = 0.20;
		// A wheel notch pans this many canvas units at zoom 1, so at every zoom it moves the same distance on the screen.
		static constexpr float WheelPanUnits = 64.0f;
		// Framing leaves this many canvas units around what it frames, and never zooms in past MaxFrameZoom.
		static constexpr float FrameMargin = 48.0f;
		static constexpr float MaxFrameZoom = 1.0f;

		// The default state (the event graph, centred on the origin at 100 %) and no canvas rectangle yet.
		VisualScriptView() = default;
		// The state moved into its ranges, at the level of its zoom.
		explicit VisualScriptView(VisualScriptViewState const& state);

		// Where the canvas is on the screen (pixels) and the UI scale (ImGuiStyle::FontScaleDpi): set every frame before the
		// input is handled. A negative or non-finite size counts as empty; a UI scale that is not positive and finite as 1.
		void SetCanvas(glm::vec2 canvasMin, glm::vec2 canvasSize, float uiScale);
		glm::vec2 GetCanvasMin() const { return m_CanvasMin; }
		glm::vec2 GetCanvasSize() const { return m_CanvasSize; }
		float GetUIScale() const { return m_UIScale; }
		// The screen point at the middle of the canvas: the anchor of keyboard zooms and of programmatic moves.
		glm::vec2 GetScreenCenter() const { return m_CanvasMin + m_CanvasSize * 0.5f; }

		VisualScriptViewState const& GetState() const { return m_State; }
		// Moves the view at once, ending an animation and dropping a partial wheel notch: a sub-graph switch, a minimap
		// press, a view restored from the settings. Numbers outside their ranges are clamped; a coordinate of the centre that
		// is not finite becomes 0, a zoom that is not finite 1.
		void SetState(VisualScriptViewState const& state);
		// Screen pixels per canvas unit: zoom × UI scale.
		float GetPixelsPerUnit() const { return m_State.Zoom * m_UIScale; }
		// The level of detail, which follows every zoom change with VisualScriptDetailHysteresis.
		VisualScriptDetailLevel GetDetailLevel() const { return m_Level; }

		glm::vec2 ToScreen(glm::vec2 canvasPoint) const;
		glm::vec2 ToCanvas(glm::vec2 screenPoint) const;
		// The part of the canvas the view shows, in canvas units.
		VisualScriptRect GetVisibleRect() const;

		// User zooms and pans. Each one ends an animation in progress where it is; inputs that are not finite are ignored.
		//
		// Sets the zoom (clamped to the zoom range), keeping the canvas point under screenPoint under it.
		void ZoomAt(glm::vec2 screenPoint, float zoom);
		// Moves the zoom by levels of VisualScriptZoomLevels (positive zooms in) about screenPoint. From a zoom between two
		// levels, the first step goes to the next level in its direction.
		void StepZoom(int levels, glm::vec2 screenPoint);
		// Zooms by wheel notches about screenPoint: each whole notch is one level, and fractions (trackpads) add up until
		// they make a whole one. A notch in the other direction drops the fractions left over from the previous ones.
		void ZoomByWheel(float notches, glm::vec2 screenPoint);
		// Moves the view so the canvas follows the mouse by screenDelta pixels.
		void Pan(glm::vec2 screenDelta);
		// A wheel or trackpad scroll over the canvas (ImGuiIO::MouseWheelH and MouseWheel: positive scrolls left and up),
		// with the mouse at mouse and the Ctrl (Cmd on macOS) and Shift keys as given. Shift turns a vertical scroll into a
		// horizontal one, as ImGui does in windows. Pans move the view as a scrolled document moves, WheelPanUnits per notch
		// at zoom 1, fractions in proportion.
		// - Zoom mode: a vertical scroll zooms to the mouse. Everything else pans: scrolls with Ctrl (vertically) or Shift
		//   (horizontally), horizontal scrolls, and scrolls with both axes (a diagonal trackpad swipe; mouse wheels send one
		//   axis at a time).
		// - Pan mode: every scroll pans by both axes; with Ctrl, the vertical scroll zooms to the mouse instead.
		void Scroll(glm::vec2 wheel, glm::vec2 mouse, VisualScriptScrollMode mode, bool ctrl, bool shift);
		// Edge auto-pan while something is dragged, for a frame of deltaSeconds: when the mouse is within bandPixels of an edge
		// of the canvas, or outside it, the view moves toward that edge at up to maxSpeedPixels per second, in proportion to
		// how deep the mouse is in the band (full speed at the edge and beyond). The editor uses a band of 2 font sizes and
		// 20 font sizes per second.
		void AutoPan(glm::vec2 mouse, float bandPixels, float maxSpeedPixels, float deltaSeconds);

		// Framing and programmatic moves.
		//
		// The view of this sub-graph that frames bounds (canvas units, nodes and comments) with FrameMargin on every side:
		// zoomed out as far as the canvas needs, but never in past MaxFrameZoom nor out past the zoom range. With nothing to
		// frame (an empty sub-graph), the origin at 100 %.
		[[nodiscard]] VisualScriptViewState GetFrameTarget(std::optional<VisualScriptRect> const& bounds) const;
		// The view an open request asks for, in its fixed order: its framing first (frameBounds is what it frames, the
		// selection or everything; Keep starts from where the view is going), then its Zoom about the framed centre, then its
		// Center. The request's sub-graph is the caller's to show first.
		[[nodiscard]] VisualScriptViewState GetOpenTarget(VisualScriptOpenRequest const& request,
		                                                  std::optional<VisualScriptRect> const& frameBounds) const;
		// Frames bounds (GetFrameTarget) as a programmatic move (AnimateTo): F, Home, and jumps to find results and problems.
		void Frame(std::optional<VisualScriptRect> const& bounds, double time, double duration = AnimationSeconds);
		// Moves the view to target over duration seconds from time, the canvas's centre staying the anchor: the canvas point
		// under it moves along an ease-out cubic while the zoom moves along the same curve in log space. At once when the
		// duration is 0 (Reduce Motion), when the target shows another sub-graph, and for a time that is not finite. A move
		// already under way continues from where it is.
		void AnimateTo(VisualScriptViewState const& target, double time, double duration = AnimationSeconds);
		// Moves an animation in progress to time (seconds, the clock AnimateTo was given); its last frame lands exactly on the
		// target.
		void Advance(double time);
		bool IsAnimating() const { return m_Animation.has_value(); }
		// Where the view is going: the target of the animation in progress, else the current state.
		VisualScriptViewState const& GetTargetState() const { return m_Animation ? m_Animation->Target : m_State; }

	private:
		struct Animation
		{
			VisualScriptViewState Start;
			VisualScriptViewState Target;
			double StartTime = 0.0;
			double Duration = 0.0;
			// StartTime + Duration as the caller's clock adds them, so a frame at that time ends the move.
			double EndTime = 0.0;
		};

		void SetCenter(glm::dvec2 center);
		void SetZoom(float zoom);

		VisualScriptViewState m_State;
		VisualScriptDetailLevel m_Level = VisualScriptDetailLevel::Detail;
		glm::vec2 m_CanvasMin{0.0f};
		glm::vec2 m_CanvasSize{0.0f};
		float m_UIScale = 1.0f;
		// The fraction of a wheel notch not yet turned into a zoom step.
		double m_WheelNotches = 0.0;
		std::optional<Animation> m_Animation;
	};
}
