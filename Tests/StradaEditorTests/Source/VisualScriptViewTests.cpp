#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptEditorTypes.h"
#include "Editor/VisualScripting/VisualScriptView.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// A canvas away from the screen's origin, so offsets that ignore it show.
	constexpr glm::vec2 CanvasMin{100.0f, 50.0f};
	constexpr glm::vec2 CanvasSize{800.0f, 600.0f};
	constexpr glm::vec2 CanvasCenter = CanvasMin + CanvasSize * 0.5f;

	VisualScriptView MakeView(VisualScriptViewState const& state = {}, float uiScale = 1.0f)
	{
		VisualScriptView view(state);
		view.SetCanvas(CanvasMin, CanvasSize, uiScale);
		return view;
	}

	VisualScriptViewState MakeState(glm::vec2 center, float zoom, uint32_t function = 0)
	{
		VisualScriptViewState state;
		state.Function = function;
		state.Center = center;
		state.Zoom = zoom;
		return state;
	}

	bool IsNear(glm::vec2 actual, glm::vec2 expected, float tolerance = 1.0e-3f)
	{
		return std::abs(actual.x - expected.x) <= tolerance && std::abs(actual.y - expected.y) <= tolerance;
	}

	std::string Describe(glm::vec2 point)
	{
		return "(" + std::to_string(point.x) + ", " + std::to_string(point.y) + ")";
	}

	std::string Describe(VisualScriptDetailLevel level)
	{
		switch (level)
		{
			case VisualScriptDetailLevel::Far:
				return "Far";
			case VisualScriptDetailLevel::Overview:
				return "Overview";
			case VisualScriptDetailLevel::Compact:
				return "Compact";
			case VisualScriptDetailLevel::Detail:
				return "Detail";
		}
		return "?";
	}

	// The level of detail of each wheel level, as the table gives it.
	VisualScriptDetailLevel GetTableLevel(float zoom)
	{
		if (zoom >= 0.70f)
		{
			return VisualScriptDetailLevel::Detail;
		}
		if (zoom >= 0.50f)
		{
			return VisualScriptDetailLevel::Compact;
		}
		if (zoom >= 0.20f)
		{
			return VisualScriptDetailLevel::Overview;
		}
		return VisualScriptDetailLevel::Far;
	}

	constexpr std::array<VisualScriptDetailLevel, 4> DetailLevels = {VisualScriptDetailLevel::Far, VisualScriptDetailLevel::Overview,
	                                                                 VisualScriptDetailLevel::Compact, VisualScriptDetailLevel::Detail};
}

TEST_CASE("VisualScriptEditor: the view maps canvas points to the screen and back at every zoom and UI scale")
{
	for (float const zoom : {0.1f, 1.0f, 2.0f})
	{
		for (float const uiScale : {1.0f, 2.0f})
		{
			CAPTURE(zoom);
			CAPTURE(uiScale);
			glm::vec2 const center(120.0f, -40.0f);
			VisualScriptView const view = MakeView(MakeState(center, zoom), uiScale);
			CHECK(view.GetPixelsPerUnit() == doctest::Approx(zoom * uiScale));
			CHECK(view.GetScreenCenter() == CanvasCenter);
			// The centre is in the middle of the canvas, and a canvas unit is zoom × UI scale pixels.
			CHECK(IsNear(view.ToScreen(center), CanvasCenter));
			CHECK(IsNear(view.ToScreen(center + glm::vec2(1.0f, 1.0f)) - view.ToScreen(center), glm::vec2(zoom * uiScale)));

			for (glm::vec2 const point :
			     {glm::vec2(0.0f), glm::vec2(-5000.0f, 2500.0f), glm::vec2(333.25f, 77.5f), glm::vec2(100000.0f, -100000.0f)})
			{
				CAPTURE(Describe(point));
				// Relative to the distance travelled: far points keep fewer of a float's digits.
				float const tolerance = std::max(1.0e-3f, glm::length(point - center) * 1.0e-6f);
				CHECK(IsNear(view.ToCanvas(view.ToScreen(point)), point, tolerance));
			}
			for (glm::vec2 const screen : {CanvasMin, CanvasMin + CanvasSize, glm::vec2(517.5f, 222.25f), glm::vec2(-40.0f, 2000.0f)})
			{
				CAPTURE(Describe(screen));
				CHECK(IsNear(view.ToScreen(view.ToCanvas(screen)), screen, 1.0e-2f));
			}

			VisualScriptRect const visible = view.GetVisibleRect();
			CHECK(IsNear(visible.Min, view.ToCanvas(CanvasMin)));
			CHECK(IsNear(visible.GetSize(), CanvasSize / (zoom * uiScale), 1.0e-2f));
			CHECK(IsNear(visible.GetCenter(), center, 1.0e-2f));
		}
	}
}

TEST_CASE("VisualScriptEditor: zooming keeps the canvas point under its anchor and stays within the zoom range")
{
	VisualScriptView view = MakeView(MakeState({10.0f, 20.0f}, 1.0f));
	for (glm::vec2 const anchor : {CanvasCenter, glm::vec2(130.0f, 70.0f), CanvasMin + CanvasSize, glm::vec2(500.0f, 300.0f)})
	{
		for (float const zoom : {0.1f, 0.37f, 1.5f, 2.0f, 0.7f})
		{
			CAPTURE(Describe(anchor));
			CAPTURE(zoom);
			glm::vec2 const point = view.ToCanvas(anchor);
			view.ZoomAt(anchor, zoom);
			CHECK(view.GetState().Zoom == zoom);
			CHECK(IsNear(view.ToScreen(point), anchor, 1.0e-2f));
		}
	}

	view.ZoomAt(CanvasCenter, 5.0f);
	CHECK(view.GetState().Zoom == VisualScriptMaxZoom);
	view.ZoomAt(CanvasCenter, 0.01f);
	CHECK(view.GetState().Zoom == VisualScriptMinZoom);
	VisualScriptViewState const before = view.GetState();
	view.ZoomAt(CanvasCenter, std::numeric_limits<float>::quiet_NaN());
	view.ZoomAt(glm::vec2(std::numeric_limits<float>::infinity(), 0.0f), 1.0f);
	CHECK(view.GetState() == before);
}

TEST_CASE("VisualScriptEditor: the wheel zooms one level per notch about the cursor, and fractions of notches add up")
{
	glm::vec2 const mouse(300.0f, 200.0f);
	VisualScriptView view = MakeView(MakeState({40.0f, -25.0f}, 1.0f));
	glm::vec2 const point = view.ToCanvas(mouse);
	for (float const expected : {1.25f, 1.5f, 1.75f, 2.0f, 2.0f})
	{
		view.Scroll({0.0f, 1.0f}, mouse, VisualScriptScrollMode::Zoom, false, false);
		CHECK(view.GetState().Zoom == expected);
		CHECK(IsNear(view.ToScreen(point), mouse, 1.0e-2f));
	}
	for (float const expected : {1.75f, 1.5f, 1.25f, 1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.33f, 0.25f, 0.2f, 0.15f, 0.1f, 0.1f})
	{
		view.Scroll({0.0f, -1.0f}, mouse, VisualScriptScrollMode::Zoom, false, false);
		CHECK(view.GetState().Zoom == expected);
		CHECK(IsNear(view.ToScreen(point), mouse, 1.0e-2f));
	}

	SUBCASE("four quarter notches make one notch")
	{
		view.SetState(MakeState({}, 1.0f));
		for (int notch = 0; notch < 3; notch++)
		{
			view.ZoomByWheel(0.25f, mouse);
			CHECK(view.GetState().Zoom == 1.0f);
		}
		view.ZoomByWheel(0.25f, mouse);
		CHECK(view.GetState().Zoom == 1.25f);
	}
	SUBCASE("a trackpad's 120ths of a notch make one notch")
	{
		view.SetState(MakeState({}, 1.0f));
		for (int part = 0; part < 119; part++)
		{
			view.ZoomByWheel(1.0f / 120.0f, mouse);
		}
		CHECK(view.GetState().Zoom == 1.0f);
		view.ZoomByWheel(1.0f / 120.0f, mouse);
		CHECK(view.GetState().Zoom == 1.25f);
	}
	SUBCASE("whole notches step at once and the fraction is kept")
	{
		view.SetState(MakeState({}, 0.5f));
		view.ZoomByWheel(2.5f, mouse);
		CHECK(view.GetState().Zoom == 0.7f);
		view.ZoomByWheel(2.5f, mouse);
		CHECK(view.GetState().Zoom == 1.0f);
	}
	SUBCASE("turning the wheel the other way drops the fractions")
	{
		view.SetState(MakeState({}, 1.0f));
		view.ZoomByWheel(0.75f, mouse);
		view.ZoomByWheel(-0.5f, mouse);
		CHECK(view.GetState().Zoom == 1.0f);
		view.ZoomByWheel(-0.5f, mouse);
		CHECK(view.GetState().Zoom == 0.9f);
	}
	SUBCASE("a sub-graph switch drops a partial notch")
	{
		view.SetState(MakeState({}, 1.0f));
		view.ZoomByWheel(0.75f, mouse);
		view.SetState(MakeState({}, 1.0f, 4));
		view.ZoomByWheel(0.5f, mouse);
		CHECK(view.GetState().Zoom == 1.0f);
	}
}

TEST_CASE("VisualScriptEditor: zoom steps from a zoom between levels go to the next level in their direction")
{
	VisualScriptView view = MakeView(MakeState({}, 0.83f));
	view.StepZoom(1, CanvasCenter);
	CHECK(view.GetState().Zoom == 0.9f);
	view.SetState(MakeState({}, 0.83f));
	view.StepZoom(-1, CanvasCenter);
	CHECK(view.GetState().Zoom == 0.8f);

	// A zoom a hair off a level (a float that lost a bit on its way) counts as on it.
	view.SetState(MakeState({}, 0.33f + 2.0e-6f));
	view.StepZoom(1, CanvasCenter);
	CHECK(view.GetState().Zoom == 0.4f);
	view.SetState(MakeState({}, 0.33f + 2.0e-6f));
	view.StepZoom(-1, CanvasCenter);
	CHECK(view.GetState().Zoom == 0.25f);

	view.SetState(MakeState({}, 1.0f));
	view.StepZoom(-3, CanvasCenter);
	CHECK(view.GetState().Zoom == 0.7f);
	view.StepZoom(1000, CanvasCenter);
	CHECK(view.GetState().Zoom == VisualScriptMaxZoom);
	view.StepZoom(-1000, CanvasCenter);
	CHECK(view.GetState().Zoom == VisualScriptMinZoom);
	view.ZoomByWheel(1.0e9f, CanvasCenter);
	CHECK(view.GetState().Zoom == VisualScriptMaxZoom);
	view.ZoomByWheel(std::numeric_limits<float>::infinity(), CanvasCenter);
	CHECK(view.GetState().Zoom == VisualScriptMaxZoom);
}

TEST_CASE("VisualScriptEditor: in the Zoom scroll mode, scrolls with Ctrl or Shift, horizontal scrolls and diagonal swipes pan")
{
	glm::vec2 const mouse(500.0f, 350.0f);
	for (float const uiScale : {1.0f, 2.0f})
	{
		CAPTURE(uiScale);
		VisualScriptView view = MakeView(MakeState({}, 1.0f), uiScale);
		// Ctrl: vertically, a scroll up moving the view up as a scrolled document does.
		view.Scroll({0.0f, 1.0f}, mouse, VisualScriptScrollMode::Zoom, true, false);
		CHECK(view.GetState().Zoom == 1.0f);
		CHECK(IsNear(view.GetState().Center, {0.0f, -64.0f}));
		// Shift: horizontally (on Windows and Linux the scroll arrives vertical).
		view.Scroll({0.0f, -1.0f}, mouse, VisualScriptScrollMode::Zoom, false, true);
		CHECK(IsNear(view.GetState().Center, {64.0f, -64.0f}));
		// The horizontal wheel: positive scrolls left.
		view.Scroll({1.0f, 0.0f}, mouse, VisualScriptScrollMode::Zoom, false, false);
		CHECK(IsNear(view.GetState().Center, {0.0f, -64.0f}));
		// A diagonal swipe pans by both axes, fractions in proportion.
		view.Scroll({0.5f, -0.25f}, mouse, VisualScriptScrollMode::Zoom, false, false);
		CHECK(IsNear(view.GetState().Center, {-32.0f, -48.0f}));
		CHECK(view.GetState().Zoom == 1.0f);
		// The same screen distance at every zoom: 64 canvas units per notch at zoom 1, 128 at 0.5.
		view.SetState(MakeState({}, 0.5f));
		view.Scroll({0.0f, 1.0f}, mouse, VisualScriptScrollMode::Zoom, true, false);
		CHECK(IsNear(view.GetState().Center, {0.0f, -128.0f}));
		// A plain vertical scroll zooms.
		view.Scroll({0.0f, 1.0f}, mouse, VisualScriptScrollMode::Zoom, false, false);
		CHECK(view.GetState().Zoom == 0.6f);
	}
}

TEST_CASE("VisualScriptEditor: in the Pan scroll mode, every scroll pans and Ctrl+wheel zooms to the cursor")
{
	glm::vec2 const mouse(300.0f, 500.0f);
	VisualScriptView view = MakeView(MakeState({}, 1.0f));
	view.Scroll({0.0f, 1.0f}, mouse, VisualScriptScrollMode::Pan, false, false);
	CHECK(IsNear(view.GetState().Center, {0.0f, -64.0f}));
	view.Scroll({-0.25f, 0.5f}, mouse, VisualScriptScrollMode::Pan, false, false);
	CHECK(IsNear(view.GetState().Center, {16.0f, -96.0f}));
	view.Scroll({0.0f, 1.0f}, mouse, VisualScriptScrollMode::Pan, false, true);
	CHECK(IsNear(view.GetState().Center, {-48.0f, -96.0f}));
	CHECK(view.GetState().Zoom == 1.0f);

	glm::vec2 const point = view.ToCanvas(mouse);
	view.Scroll({0.0f, 1.0f}, mouse, VisualScriptScrollMode::Pan, true, false);
	CHECK(view.GetState().Zoom == 1.25f);
	CHECK(IsNear(view.ToScreen(point), mouse, 1.0e-2f));
	view.Scroll({0.0f, 0.5f}, mouse, VisualScriptScrollMode::Pan, true, false);
	view.Scroll({0.0f, 0.5f}, mouse, VisualScriptScrollMode::Pan, true, false);
	CHECK(view.GetState().Zoom == 1.5f);
	// Ctrl with a purely horizontal scroll has nothing to zoom by.
	VisualScriptViewState const before = view.GetState();
	view.Scroll({1.0f, 0.0f}, mouse, VisualScriptScrollMode::Pan, true, false);
	CHECK(view.GetState() == before);

#if defined(ST_PLATFORM_MACOS)
	CHECK(GetDefaultVisualScriptScrollMode() == VisualScriptScrollMode::Pan);
#else
	CHECK(GetDefaultVisualScriptScrollMode() == VisualScriptScrollMode::Zoom);
#endif
}

TEST_CASE("VisualScriptEditor: panning moves the canvas with the mouse and keeps the centre within the documents' range")
{
	VisualScriptView view = MakeView(MakeState({50.0f, 50.0f}, 0.5f), 2.0f);
	glm::vec2 const point(80.0f, 90.0f);
	glm::vec2 const before = view.ToScreen(point);
	view.Pan({30.0f, -12.0f});
	CHECK(IsNear(view.ToScreen(point), before + glm::vec2(30.0f, -12.0f)));

	view.SetState(MakeState({9.99e6f, -9.99e6f}, 0.1f));
	view.Pan({-1.0e9f, 1.0e9f});
	CHECK(view.GetState().Center == glm::vec2(VisualScriptMaxCenter, -VisualScriptMaxCenter));
	view.Pan({std::numeric_limits<float>::quiet_NaN(), 0.0f});
	CHECK(view.GetState().Center == glm::vec2(VisualScriptMaxCenter, -VisualScriptMaxCenter));
}

TEST_CASE("VisualScriptEditor: dragging near the canvas's edge pans toward it, faster the deeper the mouse is")
{
	float const band = 30.0f;
	float const speed = 300.0f;
	float const seconds = 0.1f;
	auto const panned = [&](glm::vec2 mouse, float zoom = 1.0f, glm::vec2 canvasSize = CanvasSize)
	{
		VisualScriptView view(MakeState({}, zoom));
		view.SetCanvas(CanvasMin, canvasSize, 1.0f);
		view.AutoPan(mouse, band, speed, seconds);
		return view.GetState().Center;
	};
	float const right = CanvasMin.x + CanvasSize.x;
	float const bottom = CanvasMin.y + CanvasSize.y;

	CHECK(panned(CanvasCenter) == glm::vec2(0.0f));
	CHECK(panned({right - band - 1.0f, CanvasCenter.y}) == glm::vec2(0.0f));
	// Full speed at the edge and beyond it, half way into the band at half speed; the view moves toward the edge.
	CHECK(IsNear(panned({right, CanvasCenter.y}), {30.0f, 0.0f}));
	CHECK(IsNear(panned({right + 200.0f, CanvasCenter.y}), {30.0f, 0.0f}));
	CHECK(IsNear(panned({right - band * 0.5f, CanvasCenter.y}), {15.0f, 0.0f}));
	CHECK(IsNear(panned({CanvasMin.x, bottom - band * 0.5f}), {-30.0f, 15.0f}));
	CHECK(IsNear(panned({CanvasMin.x - 5.0f, CanvasMin.y - 5.0f}), {-30.0f, -30.0f}));
	// The same screen speed at every zoom.
	CHECK(IsNear(panned({right, CanvasCenter.y}, 0.5f), {60.0f, 0.0f}));
	// A canvas narrower than two bands pans toward the nearer edge.
	CHECK(IsNear(panned({CanvasMin.x + 10.0f, CanvasCenter.y}, 1.0f, {40.0f, 600.0f}), {-20.0f, 0.0f}));
	CHECK(IsNear(panned({CanvasMin.x + 30.0f, CanvasCenter.y}, 1.0f, {40.0f, 600.0f}), {20.0f, 0.0f}));
	// Nothing for a frame without time or a band without width.
	VisualScriptView view = MakeView();
	view.AutoPan({right, CanvasCenter.y}, band, speed, 0.0f);
	view.AutoPan({right, CanvasCenter.y}, 0.0f, speed, seconds);
	view.AutoPan({right, CanvasCenter.y}, band, std::numeric_limits<float>::quiet_NaN(), seconds);
	CHECK(view.GetState().Center == glm::vec2(0.0f));
}

TEST_CASE("VisualScriptEditor: framing fits what it frames with a margin and never zooms in past 100 %")
{
	VisualScriptView view = MakeView(MakeState({500.0f, 500.0f}, 1.7f, 7));

	// Small: centred at 100 %, in the view's sub-graph.
	VisualScriptViewState target = view.GetFrameTarget(VisualScriptRect{{0.0f, 0.0f}, {200.0f, 100.0f}});
	CHECK(target == MakeState({100.0f, 50.0f}, 1.0f, 7));

	// Large: the bounds and the margin fill the canvas in the tighter axis.
	VisualScriptRect const large{{-1000.0f, 200.0f}, {1000.0f, 1200.0f}};
	target = view.GetFrameTarget(large);
	float const fit = std::min(CanvasSize.x / (2000.0f + 96.0f), CanvasSize.y / (1000.0f + 96.0f));
	CHECK(target.Zoom == doctest::Approx(fit));
	CHECK(IsNear(target.Center, {0.0f, 700.0f}));
	// At a UI scale of 2 the same bounds need half the zoom.
	VisualScriptView const scaled = MakeView(MakeState({}, 1.0f), 2.0f);
	CHECK(scaled.GetFrameTarget(large).Zoom == doctest::Approx(fit * 0.5f));
	// Min and max given the wrong way round frame the same.
	CHECK(view.GetFrameTarget(VisualScriptRect{large.Max, large.Min}) == target);

	// Huge: as far out as the zoom goes.
	CHECK(view.GetFrameTarget(VisualScriptRect{{-1.0e6f, -1.0e6f}, {1.0e6f, 1.0e6f}}).Zoom == VisualScriptMinZoom);
	// Nothing to frame: the origin at 100 %.
	CHECK(view.GetFrameTarget(std::nullopt) == MakeState({}, 1.0f, 7));
	CHECK(view.GetFrameTarget(VisualScriptRect{{std::numeric_limits<float>::quiet_NaN(), 0.0f}, {1.0f, 1.0f}}) == MakeState({}, 1.0f, 7));
	// Before the canvas has a size, nothing fits.
	VisualScriptView const unsized(MakeState({}, 1.0f));
	CHECK(unsized.GetFrameTarget(VisualScriptRect{{0.0f, 0.0f}, {10.0f, 10.0f}}).Zoom == VisualScriptMinZoom);
}

TEST_CASE("VisualScriptEditor: an open request frames first, then zooms about the framed centre, then takes its centre")
{
	VisualScriptView view = MakeView(MakeState({10.0f, 10.0f}, 0.8f, 3));
	VisualScriptRect const selection{{1000.0f, 1000.0f}, {1200.0f, 1100.0f}};
	VisualScriptViewState const framed = view.GetFrameTarget(selection);
	REQUIRE(framed == MakeState({1100.0f, 1050.0f}, 1.0f, 3));

	VisualScriptOpenRequest request;
	CHECK(view.GetOpenTarget(request, selection) == view.GetState());
	request.Zoom = 1.25f;
	CHECK(view.GetOpenTarget(request, selection) == MakeState({10.0f, 10.0f}, 1.25f, 3));

	request = {};
	request.Frame = VisualScriptFrame::Selection;
	CHECK(view.GetOpenTarget(request, selection) == framed);
	// An explicit zoom replaces the framed one about the framed centre, reaching what framing alone never does.
	request.Zoom = 1.25f;
	CHECK(view.GetOpenTarget(request, selection) == MakeState(framed.Center, 1.25f, 3));
	// An explicit centre then replaces the centre.
	request.Center = glm::vec2(-300.0f, 40.0f);
	CHECK(view.GetOpenTarget(request, selection) == MakeState({-300.0f, 40.0f}, 1.25f, 3));
	request.Zoom.reset();
	CHECK(view.GetOpenTarget(request, selection) == MakeState({-300.0f, 40.0f}, framed.Zoom, 3));

	// Framing everything of an empty sub-graph shows the origin at 100 %; numbers are clamped into their ranges.
	request = {};
	request.Frame = VisualScriptFrame::All;
	CHECK(view.GetOpenTarget(request, std::nullopt) == MakeState({}, 1.0f, 3));
	request.Zoom = 5.0f;
	request.Center = glm::vec2(2.0e7f, -2.0e7f);
	CHECK(view.GetOpenTarget(request, std::nullopt) == MakeState({VisualScriptMaxCenter, -VisualScriptMaxCenter}, 2.0f, 3));

	// Keeping the view during a move keeps where the view is going.
	view.AnimateTo(MakeState({400.0f, 400.0f}, 0.5f, 3), 1.0);
	request = {};
	request.Zoom = 0.7f;
	CHECK(view.GetOpenTarget(request, selection) == MakeState({400.0f, 400.0f}, 0.7f, 3));
}

TEST_CASE("VisualScriptEditor: the level of detail follows the zoom, and leaves a level only 0.02 past its threshold")
{
	// Every wheel level has its table's level, whatever the level before; no wheel level lies within a threshold's band.
	for (float const zoom : VisualScriptZoomLevels)
	{
		CAPTURE(zoom);
		CHECK(Describe(GetVisualScriptDetailLevel(zoom)) == Describe(GetTableLevel(zoom)));
		for (VisualScriptDetailLevel const before : DetailLevels)
		{
			CAPTURE(Describe(before));
			CHECK(Describe(GetVisualScriptDetailLevel(zoom, before)) == Describe(GetTableLevel(zoom)));
		}
		for (float const threshold : {VisualScriptOverviewZoom, VisualScriptCompactZoom, VisualScriptDetailZoom})
		{
			CHECK(std::abs(zoom - threshold) > VisualScriptDetailHysteresis);
		}
	}

	CHECK(GetVisualScriptDetailLevel(0.65f) == VisualScriptDetailLevel::Detail);
	CHECK(GetVisualScriptDetailLevel(0.649f) == VisualScriptDetailLevel::Compact);
	CHECK(GetVisualScriptDetailLevel(0.45f) == VisualScriptDetailLevel::Compact);
	CHECK(GetVisualScriptDetailLevel(0.449f) == VisualScriptDetailLevel::Overview);
	CHECK(GetVisualScriptDetailLevel(0.175f) == VisualScriptDetailLevel::Overview);
	CHECK(GetVisualScriptDetailLevel(0.174f) == VisualScriptDetailLevel::Far);

	struct Move
	{
		float Zoom;
		VisualScriptDetailLevel Before;
		VisualScriptDetailLevel After;
	};
	// Just inside each band widened by the hysteresis, and just outside it.
	constexpr std::array<Move, 14> moves = {{
		{0.631f, VisualScriptDetailLevel::Detail, VisualScriptDetailLevel::Detail},
		{0.629f, VisualScriptDetailLevel::Detail, VisualScriptDetailLevel::Compact},
		{0.669f, VisualScriptDetailLevel::Compact, VisualScriptDetailLevel::Compact},
		{0.671f, VisualScriptDetailLevel::Compact, VisualScriptDetailLevel::Detail},
		{0.431f, VisualScriptDetailLevel::Compact, VisualScriptDetailLevel::Compact},
		{0.429f, VisualScriptDetailLevel::Compact, VisualScriptDetailLevel::Overview},
		{0.469f, VisualScriptDetailLevel::Overview, VisualScriptDetailLevel::Overview},
		{0.471f, VisualScriptDetailLevel::Overview, VisualScriptDetailLevel::Compact},
		{0.156f, VisualScriptDetailLevel::Overview, VisualScriptDetailLevel::Overview},
		{0.154f, VisualScriptDetailLevel::Overview, VisualScriptDetailLevel::Far},
		{0.194f, VisualScriptDetailLevel::Far, VisualScriptDetailLevel::Far},
		{0.196f, VisualScriptDetailLevel::Far, VisualScriptDetailLevel::Overview},
		// A view that leaves its level takes the level of its zoom, even across a whole level.
		{0.44f, VisualScriptDetailLevel::Detail, VisualScriptDetailLevel::Overview},
		{1.0f, VisualScriptDetailLevel::Far, VisualScriptDetailLevel::Detail},
	}};
	for (Move const& move : moves)
	{
		CAPTURE(move.Zoom);
		CAPTURE(Describe(move.Before));
		CHECK(Describe(GetVisualScriptDetailLevel(move.Zoom, move.Before)) == Describe(move.After));
	}

	// A view starts at its zoom's level, then follows its zoom with the hysteresis.
	CHECK(VisualScriptView(MakeState({}, 0.64f)).GetDetailLevel() == VisualScriptDetailLevel::Compact);
	VisualScriptView view = MakeView(MakeState({}, 1.0f));
	CHECK(view.GetDetailLevel() == VisualScriptDetailLevel::Detail);
	view.ZoomAt(CanvasCenter, 0.64f);
	CHECK(view.GetDetailLevel() == VisualScriptDetailLevel::Detail);
	view.ZoomAt(CanvasCenter, 0.62f);
	CHECK(view.GetDetailLevel() == VisualScriptDetailLevel::Compact);
	view.ZoomAt(CanvasCenter, 0.66f);
	CHECK(view.GetDetailLevel() == VisualScriptDetailLevel::Compact);
	view.SetState(MakeState({}, 0.68f));
	CHECK(view.GetDetailLevel() == VisualScriptDetailLevel::Detail);
	view.SetState(MakeState({}, 0.1f));
	CHECK(view.GetDetailLevel() == VisualScriptDetailLevel::Far);
}

TEST_CASE("VisualScriptEditor: programmatic moves ease out and end exactly on their target")
{
	VisualScriptView view = MakeView(MakeState({}, 1.0f));
	VisualScriptViewState const target = MakeState({400.0f, -200.0f}, 0.25f);
	view.AnimateTo(target, 10.0);
	CHECK(view.IsAnimating());
	CHECK(view.GetTargetState() == target);
	CHECK(view.GetState() == MakeState({}, 1.0f));

	// Half the time: an ease-out cubic has covered 7/8 of the way, of the centre and of the zoom in log space.
	view.Advance(10.0 + VisualScriptView::AnimationSeconds * 0.5);
	CHECK(view.IsAnimating());
	CHECK(IsNear(view.GetState().Center, {350.0f, -175.0f}, 1.0e-2f));
	CHECK(view.GetState().Zoom == doctest::Approx(std::pow(0.25, 0.875)));
	CHECK(view.GetDetailLevel() == VisualScriptDetailLevel::Overview);
	// A time before the start holds the start.
	view.Advance(9.0);
	CHECK(view.GetState() == MakeState({}, 1.0f));

	view.Advance(10.0 + VisualScriptView::AnimationSeconds);
	CHECK(!view.IsAnimating());
	CHECK(view.GetState() == target);
	CHECK(view.GetTargetState() == target);
	CHECK(view.GetDetailLevel() == VisualScriptDetailLevel::Overview);

	SUBCASE("a new move continues from where the view is")
	{
		view.AnimateTo(MakeState({}, 1.0f), 20.0);
		view.Advance(20.1);
		VisualScriptViewState const middle = view.GetState();
		view.AnimateTo(MakeState({1000.0f, 1000.0f}, 2.0f), 20.1);
		view.Advance(20.1);
		CHECK(view.GetState() == middle);
		view.Advance(21.0);
		CHECK(view.GetState() == MakeState({1000.0f, 1000.0f}, 2.0f));
	}
	SUBCASE("moves are at once without a duration (Reduce Motion), to another sub-graph and for a time that is not finite")
	{
		view.AnimateTo(MakeState({1.0f, 2.0f}, 0.5f), 30.0, 0.0);
		CHECK(!view.IsAnimating());
		CHECK(view.GetState() == MakeState({1.0f, 2.0f}, 0.5f));
		view.AnimateTo(MakeState({3.0f, 4.0f}, 0.6f, 5), 30.0);
		CHECK(!view.IsAnimating());
		CHECK(view.GetState() == MakeState({3.0f, 4.0f}, 0.6f, 5));
		view.AnimateTo(MakeState({5.0f, 6.0f}, 0.7f, 5), std::numeric_limits<double>::infinity());
		CHECK(!view.IsAnimating());
		CHECK(view.GetState() == MakeState({5.0f, 6.0f}, 0.7f, 5));
	}
	SUBCASE("jumping to where a move goes ends it there")
	{
		view.AnimateTo(MakeState({-50.0f, 60.0f}, 1.5f), 50.0);
		view.Advance(50.05);
		view.SetState(view.GetTargetState());
		CHECK(!view.IsAnimating());
		CHECK(view.GetState() == MakeState({-50.0f, 60.0f}, 1.5f));
	}
	SUBCASE("framing is a programmatic move to the frame target")
	{
		VisualScriptRect const bounds{{-1000.0f, 200.0f}, {1000.0f, 1200.0f}};
		VisualScriptViewState const framed = view.GetFrameTarget(bounds);
		view.Frame(bounds, 60.0);
		CHECK(view.IsAnimating());
		CHECK(view.GetTargetState() == framed);
		view.Advance(61.0);
		CHECK(view.GetState() == framed);
		view.Frame(std::nullopt, 62.0, 0.0);
		CHECK(!view.IsAnimating());
		CHECK(view.GetState() == MakeState({}, 1.0f));
	}
	SUBCASE("a move to where the view is does not animate, and targets are clamped")
	{
		view.AnimateTo(view.GetState(), 40.0);
		CHECK(!view.IsAnimating());
		view.AnimateTo(MakeState({2.0e7f, 0.0f}, 9.0f), 40.0);
		CHECK(view.GetTargetState() == MakeState({VisualScriptMaxCenter, 0.0f}, VisualScriptMaxZoom));
		view.Advance(41.0);
		CHECK(view.GetState() == MakeState({VisualScriptMaxCenter, 0.0f}, VisualScriptMaxZoom));
	}
}

TEST_CASE("VisualScriptEditor: user zooms and pans end a programmatic move where it is")
{
	VisualScriptViewState const target = MakeState({800.0f, 800.0f}, 0.4f);
	std::vector<std::pair<std::string, std::function<void(VisualScriptView&)>>> const inputs = {
		{"pan",
	     [](VisualScriptView& view)
	     {
			 view.Pan({5.0f, 0.0f});
		 }},
		{"zoom at",
	     [](VisualScriptView& view)
	     {
			 view.ZoomAt(CanvasCenter, 1.5f);
		 }},
		{"zoom step",
	     [](VisualScriptView& view)
	     {
			 view.StepZoom(1, CanvasCenter);
		 }},
		{"part of a notch",
	     [](VisualScriptView& view)
	     {
			 view.ZoomByWheel(0.25f, CanvasCenter);
		 }},
		{"scroll",
	     [](VisualScriptView& view)
	     {
			 view.Scroll({0.0f, 1.0f}, CanvasCenter, VisualScriptScrollMode::Pan, false, false);
		 }},
		{"edge pan",
	     [](VisualScriptView& view)
	     {
			 view.AutoPan(CanvasMin, 30.0f, 300.0f, 0.1f);
		 }},
		{"jump",
	     [](VisualScriptView& view)
	     {
			 view.SetState(MakeState({}, 1.0f, 2));
		 }},
	};
	for (auto const& [name, input] : inputs)
	{
		CAPTURE(name);
		VisualScriptView view = MakeView(MakeState({}, 1.0f));
		view.AnimateTo(target, 0.0);
		view.Advance(0.05);
		input(view);
		CHECK(!view.IsAnimating());
		VisualScriptViewState const after = view.GetState();
		CHECK(view.GetTargetState() == after);
		view.Advance(1.0);
		CHECK(view.GetState() == after);
	}

	// Inputs of nothing are no input.
	VisualScriptView view = MakeView(MakeState({}, 1.0f));
	view.AnimateTo(target, 0.0);
	view.Pan({0.0f, 0.0f});
	view.Scroll({0.0f, 0.0f}, CanvasCenter, VisualScriptScrollMode::Zoom, false, false);
	view.StepZoom(0, CanvasCenter);
	view.AutoPan(CanvasCenter, 30.0f, 300.0f, 0.1f);
	CHECK(view.IsAnimating());
}

TEST_CASE("VisualScriptEditor: the view keeps its numbers finite and within their ranges")
{
	float const nan = std::numeric_limits<float>::quiet_NaN();
	float const infinity = std::numeric_limits<float>::infinity();

	VisualScriptView view(MakeState({nan, infinity}, nan, 2));
	CHECK(view.GetState() == MakeState({}, 1.0f, 2));
	view.SetState(MakeState({2.0e7f, -2.0e7f}, 7.0f));
	CHECK(view.GetState() == MakeState({VisualScriptMaxCenter, -VisualScriptMaxCenter}, VisualScriptMaxZoom));
	view.SetState(MakeState({1.0f, 1.0f}, 0.0f));
	CHECK(view.GetState().Zoom == VisualScriptMinZoom);

	view.SetCanvas({nan, 0.0f}, {-5.0f, infinity}, 0.0f);
	CHECK(view.GetCanvasMin() == glm::vec2(0.0f));
	CHECK(view.GetCanvasSize() == glm::vec2(0.0f));
	CHECK(view.GetUIScale() == 1.0f);
	view.SetCanvas(CanvasMin, {-5.0f, 300.0f}, -2.0f);
	CHECK(view.GetCanvasSize() == glm::vec2(0.0f, 300.0f));
	CHECK(view.GetUIScale() == 1.0f);
	glm::vec2 const point = view.ToCanvas({1.0e6f, -1.0e6f});
	CHECK(std::isfinite(point.x));
	CHECK(std::isfinite(point.y));
}

TEST_CASE("VisualScriptEditor: canvas text is drawn at the largest ladder size that fits its layout, and only when readable")
{
	CHECK(VisualScriptTextLadder.front() == 6.0f);
	CHECK(VisualScriptTextLadder.back() == 82.0f);
	for (size_t index = 0; index < VisualScriptTextLadder.size(); index++)
	{
		float const size = VisualScriptTextLadder[index];
		CAPTURE(size);
		CHECK(size == std::floor(size));
		if (index == 0)
		{
			continue;
		}
		float const previous = VisualScriptTextLadder[index - 1];
		CHECK(size > previous);
		// Every whole size up to 20; steps of at most 10 % above.
		CHECK((size <= 20.0f ? size - previous == 1.0f : size <= previous * 1.1f));
	}

	using VisualScriptCanvasMetrics::LadderFloor;
	CHECK(LadderFloor(5.9f) == 0.0f);
	CHECK(LadderFloor(6.0f) == 6.0f);
	CHECK(LadderFloor(13.7f) == 13.0f);
	CHECK(LadderFloor(21.9f) == 20.0f);
	CHECK(LadderFloor(22.0f) == 22.0f);
	CHECK(LadderFloor(34.9f) == 32.0f);
	CHECK(LadderFloor(82.0f) == 82.0f);
	CHECK(LadderFloor(100.0f) == 82.0f);
	CHECK(LadderFloor(std::numeric_limits<float>::infinity()) == 82.0f);
	CHECK(LadderFloor(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
	static_assert(LadderFloor(15.0f) == 15.0f);
	// Never larger than the layout, and at most one ladder step smaller.
	for (int tenths = 60; tenths <= 900; tenths++)
	{
		float const pixels = static_cast<float>(tenths) / 10.0f;
		float const size = LadderFloor(pixels);
		CAPTURE(pixels);
		CHECK(size <= pixels);
		auto const next = std::upper_bound(VisualScriptTextLadder.begin(), VisualScriptTextLadder.end(), size);
		CHECK((next == VisualScriptTextLadder.end() || *next > pixels));
	}

	using VisualScriptCanvasMetrics::IsTextVisible;
	// Pin labels (13) show from about 0.54: at 0.60 (Compact) but not at 0.50.
	CHECK(IsTextVisible(VisualScriptCanvasMetrics::PinLabelTextSize, 0.6f));
	CHECK(!IsTextVisible(VisualScriptCanvasMetrics::PinLabelTextSize, 0.5f));
	CHECK(IsTextVisible(VisualScriptCanvasMetrics::TitleTextSize, 0.5f));
	CHECK(!IsTextVisible(VisualScriptCanvasMetrics::TitleTextSize, 0.4f));
	// Subtitles show from a zoom of 2/3, inside Detail.
	CHECK(VisualScriptCanvasMetrics::SubtitleTextSize * 0.7f >= VisualScriptCanvasMetrics::MinSubtitleLogicalPixels);
	CHECK(VisualScriptCanvasMetrics::SubtitleTextSize * 0.65f < VisualScriptCanvasMetrics::MinSubtitleLogicalPixels);
}
