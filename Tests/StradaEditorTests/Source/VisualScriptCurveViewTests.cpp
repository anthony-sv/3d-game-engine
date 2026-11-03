#include "Editor/VisualScripting/VisualScriptCurveView.h"
#include "Editor/VisualScripting/VisualScriptTimeline.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float FontSize = 10.0f;

	VisualScriptTimelineTrack MakeTrack(std::string name, std::string type, std::vector<std::pair<float, float>> const& keys)
	{
		VisualScriptTimelineTrack track;
		track.Name = std::move(name);
		track.Components = *GetVisualScriptTimelineComponents(type);
		track.Type = std::move(type);
		for (auto const& [time, value] : keys)
		{
			VisualScriptTimelineKey key;
			key.Time = time;
			key.Value = glm::vec4(value, value * 2.0f, 0.0f, 0.0f);
			track.Keys.push_back(key);
		}
		return track;
	}

	// A timeline of 2 s: a float curve A, a Vector2 curve B and an event track E; the view shows 0..2 s and the values -1..1
	// across 1000 x 600 pixels.
	struct Fixture
	{
		VisualScriptTimeline Timeline;
		VisualScriptCurveView View;
		std::vector<VisualScriptCurveIntent> Intents;

		Fixture()
		{
			Timeline.Length = 2.0f;
			Timeline.Tracks.push_back(MakeTrack("A", "float", {{0.0f, 0.0f}, {0.5f, 0.5f}, {1.0f, -0.5f}, {1.5f, 0.0f}}));
			Timeline.Tracks.push_back(MakeTrack("B", "Strada.Vector2", {{0.25f, 0.25f}}));
			Timeline.Tracks.push_back(MakeTrack("E", "exec", {{1.25f, 0.0f}}));
			View.SetRect(glm::vec2(0.0f), glm::vec2(1000.0f, 600.0f), FontSize);
			View.Sync(Timeline);
			View.SetTimeRange(0.0f, 2.0f);
			View.SetValueRange(-1.0f, 1.0f);
		}

		glm::vec2 Key(uint32_t track, uint32_t key, uint32_t component = 0) const
		{
			std::optional<glm::vec2> const position = View.GetKeyPosition(Timeline, track, key, component);
			REQUIRE(position.has_value());
			return *position;
		}

		// One frame of the mouse; the intents land in Intents, and Edit and Commit intents become the timeline shown.
		void Frame(VisualScriptCurveViewInput input)
		{
			input.Hovered = true;
			for (VisualScriptCurveIntent& intent : View.Update(input, Timeline, "Timeline"))
			{
				Intents.push_back(intent);
			}
		}

		void Press(glm::vec2 at, int button = 0, bool shift = false, bool ctrl = false, bool alt = false, bool doubleClick = false)
		{
			VisualScriptCurveViewInput input;
			input.Mouse = at;
			input.Pressed[static_cast<size_t>(button)] = true;
			input.Down[static_cast<size_t>(button)] = true;
			input.Shift = shift;
			input.Ctrl = ctrl;
			input.Alt = alt;
			input.DoubleClicked = doubleClick;
			Frame(input);
		}

		void Move(glm::vec2 at, int button = 0, bool shift = false, bool ctrl = false, bool alt = false)
		{
			VisualScriptCurveViewInput input;
			input.Mouse = at;
			input.Down[static_cast<size_t>(button)] = true;
			input.Shift = shift;
			input.Ctrl = ctrl;
			input.Alt = alt;
			Frame(input);
		}

		void Release(glm::vec2 at, int button = 0, bool shift = false, bool ctrl = false, bool alt = false)
		{
			VisualScriptCurveViewInput input;
			input.Mouse = at;
			input.Released[static_cast<size_t>(button)] = true;
			input.Shift = shift;
			input.Ctrl = ctrl;
			input.Alt = alt;
			Frame(input);
		}

		void Click(glm::vec2 at, bool shift = false, bool ctrl = false)
		{
			Press(at, 0, shift, ctrl);
			Release(at, 0, shift, ctrl);
		}

		void Wheel(glm::vec2 at, float notches, bool ctrl = false)
		{
			VisualScriptCurveViewInput input;
			input.Mouse = at;
			input.Wheel = notches;
			input.Ctrl = ctrl;
			Frame(input);
		}

		std::vector<VisualScriptTimelineKeyRef> Selection() const { return {View.GetSelection().begin(), View.GetSelection().end()}; }

		// The last Edit or Commit intent's timeline.
		VisualScriptTimeline const& LastTimeline() const
		{
			for (auto intent = Intents.rbegin(); intent != Intents.rend(); ++intent)
			{
				if (intent->Kind == VisualScriptCurveIntentKind::Edit || intent->Kind == VisualScriptCurveIntentKind::Commit)
				{
					return intent->Timeline;
				}
			}
			FAIL("no edit was made");
			return Timeline;
		}
	};

	using Refs = std::vector<VisualScriptTimelineKeyRef>;
}

TEST_CASE("VisualScriptCurveView: a click selects a key, Shift adds, Ctrl toggles, and a drag on empty space selects within a marquee")
{
	Fixture fixture;
	fixture.Click(fixture.Key(0, 1));
	CHECK(fixture.Selection() == Refs{{0, 1}});
	fixture.Click(fixture.Key(0, 3), true);
	CHECK(fixture.Selection() == Refs{{0, 1}, {0, 3}});
	fixture.Click(fixture.Key(0, 1), false, true);
	CHECK(fixture.Selection() == Refs{{0, 3}});
	fixture.Click(fixture.Key(0, 1), false, true);
	CHECK(fixture.Selection() == Refs{{0, 1}, {0, 3}});
	// An event's diamond on its lane, and a click on empty space clears.
	fixture.Click(fixture.Key(2, 0));
	CHECK(fixture.Selection() == Refs{{2, 0}});
	fixture.Click(glm::vec2(990.0f, 590.0f));
	CHECK(fixture.Selection().empty());

	// A marquee over A's first two keys (0 s and 0.5 s) and B's key (0.25 s), from empty space.
	glm::vec2 const from(fixture.Key(0, 0).x - 20.0f, 30.0f);
	glm::vec2 const to(fixture.Key(0, 1).x + 20.0f, 590.0f);
	fixture.Press(from);
	fixture.Move((from + to) * 0.5f);
	CHECK(fixture.View.GetGesture() == VisualScriptCurveGesture::Marquee);
	fixture.Move(to);
	REQUIRE(fixture.View.GetMarquee().has_value());
	fixture.Release(to);
	CHECK(fixture.Selection() == Refs{{0, 0}, {0, 1}, {1, 0}});
	CHECK(fixture.Intents.empty());
	// Shift adds what a marquee covers, Ctrl removes it.
	fixture.Press(glm::vec2(fixture.Key(0, 3).x - 10.0f, 30.0f), 0, true);
	fixture.Move(glm::vec2(fixture.Key(0, 3).x + 10.0f, 590.0f), 0, true);
	fixture.Release(glm::vec2(fixture.Key(0, 3).x + 10.0f, 590.0f), 0, true);
	CHECK(fixture.Selection() == Refs{{0, 0}, {0, 1}, {0, 3}, {1, 0}});
	fixture.Press(from, 0, false, true);
	fixture.Move(to, 0, false, true);
	fixture.Release(to, 0, false, true);
	CHECK(fixture.Selection() == Refs{{0, 3}});
}

TEST_CASE("VisualScriptCurveView: dragged keys move as a block from where the drag started, snapping to 0.01 s unless Alt is held")
{
	Fixture fixture;
	fixture.View.Select({{0, 1}, {0, 2}});
	glm::vec2 const grab = fixture.Key(0, 1);
	fixture.Press(grab);
	// 3 pixels are 0.006 s: the grabbed key snaps to 0.51 s, and the block moves with it.
	fixture.Move(grab + glm::vec2(3.0f, 0.0f));
	CHECK(fixture.View.GetGesture() == VisualScriptCurveGesture::MoveKeys);
	REQUIRE(fixture.Intents.size() == 1);
	CHECK(fixture.Intents[0].Kind == VisualScriptCurveIntentKind::Edit);
	CHECK(fixture.Intents[0].Action == "Move 2 Keys on Timeline");
	CHECK(fixture.LastTimeline().Tracks[0].Keys[1].Time == doctest::Approx(0.51f));
	CHECK(fixture.LastTimeline().Tracks[0].Keys[2].Time == doctest::Approx(1.01f));
	// Alt: free.
	fixture.Move(grab + glm::vec2(3.0f, 0.0f), 0, false, false, true);
	CHECK(fixture.LastTimeline().Tracks[0].Keys[1].Time == doctest::Approx(0.506f));
	// Each frame is computed from the start: back where it began, every key is where it was.
	fixture.Move(grab);
	CHECK(fixture.LastTimeline() == fixture.Timeline);
	fixture.Release(grab);
	CHECK(fixture.Intents.back().Kind == VisualScriptCurveIntentKind::EndEdit);
	CHECK(fixture.View.GetGesture() == VisualScriptCurveGesture::Idle);
}

TEST_CASE("VisualScriptCurveView: Shift keeps a key drag on one axis")
{
	Fixture fixture;
	glm::vec2 const grab = fixture.Key(0, 1);
	// The pixels of 0.2 up the values.
	float const up = fixture.View.ToScreenY(0.0) - fixture.View.ToScreenY(0.2);
	fixture.Press(grab);
	// Mostly across: the values stay.
	fixture.Move(grab + glm::vec2(50.0f, 20.0f), 0, true);
	VisualScriptTimelineKey const across = fixture.LastTimeline().Tracks[0].Keys[1];
	CHECK(across.Time == doctest::Approx(0.6f));
	CHECK(across.Value.x == doctest::Approx(0.5f));
	// Mostly up: the times stay.
	fixture.Move(grab + glm::vec2(10.0f, -up), 0, true);
	VisualScriptTimelineKey const raised = fixture.LastTimeline().Tracks[0].Keys[1];
	CHECK(raised.Time == doctest::Approx(0.5f));
	CHECK(raised.Value.x == doctest::Approx(0.7f).epsilon(1e-3));
	// Without Shift, both.
	fixture.Move(grab + glm::vec2(50.0f, -up));
	CHECK(fixture.LastTimeline().Tracks[0].Keys[1].Time == doctest::Approx(0.6f));
	CHECK(fixture.LastTimeline().Tracks[0].Keys[1].Value.x == doctest::Approx(0.7f).epsilon(1e-3));
	fixture.Release(grab + glm::vec2(50.0f, -up));
}

TEST_CASE("VisualScriptCurveView: a block of keys stops at an unselected neighbour and at the timeline's ends, keeping its spacing")
{
	Fixture fixture;
	fixture.View.Select({{0, 0}, {0, 1}, {0, 2}});
	glm::vec2 const grab = fixture.Key(0, 1);
	fixture.Press(grab);
	// 0.75 s later: the last selected key (1 s) stops at the unselected key at 1.5 s.
	fixture.Move(grab + glm::vec2(375.0f, 0.0f));
	VisualScriptTimelineTrack const& moved = fixture.LastTimeline().Tracks[0];
	CHECK(moved.Keys[0].Time == doctest::Approx(0.5f));
	CHECK(moved.Keys[1].Time == doctest::Approx(1.0f));
	CHECK(moved.Keys[2].Time == doctest::Approx(1.5f));
	CHECK(moved.Keys[3].Time == doctest::Approx(1.5f));
	// Earlier than 0: the first key stops at 0, the block does not move.
	fixture.Move(grab - glm::vec2(200.0f, 0.0f));
	CHECK(fixture.LastTimeline() == fixture.Timeline);
	// Back to where it started: the keys regain their times.
	fixture.Move(grab + glm::vec2(375.0f, 0.0f));
	fixture.Move(grab);
	CHECK(fixture.LastTimeline() == fixture.Timeline);
	fixture.Release(grab);
}

TEST_CASE("VisualScriptCurveView: a tangent handle sets both tangents of a cubic key, and Alt only the one dragged")
{
	Fixture fixture;
	fixture.View.Select({{0, 1}});
	REQUIRE(fixture.View.HasTangentHandles(fixture.Timeline, 0, 1));
	std::optional<glm::vec2> const handle = fixture.View.GetTangentHandle(fixture.Timeline, 0, 1, 0, true);
	REQUIRE(handle.has_value());
	CHECK(fixture.View.HitTest(fixture.Timeline, *handle).Kind == VisualScriptCurveHitKind::OutTangent);
	glm::vec2 const key = fixture.Key(0, 1);
	// 100 pixels across are 0.2 s, and as many up as 0.2: a slope of 1.
	glm::vec2 const target = key + glm::vec2(100.0f, fixture.View.ToScreenY(0.2) - fixture.View.ToScreenY(0.0));

	SUBCASE("both")
	{
		fixture.Press(*handle);
		fixture.Move(target);
		CHECK(fixture.View.GetGesture() == VisualScriptCurveGesture::Tangent);
		VisualScriptTimelineKey const& edited = fixture.LastTimeline().Tracks[0].Keys[1];
		REQUIRE(edited.In.has_value());
		REQUIRE(edited.Out.has_value());
		CHECK(edited.Out->x == doctest::Approx(1.0f).epsilon(1e-3));
		CHECK(edited.In->x == doctest::Approx(1.0f).epsilon(1e-3));
		CHECK(fixture.Intents.back().Action == "Edit Tangent of A on Timeline");
		fixture.Release(target);
		CHECK(fixture.Intents.back().Kind == VisualScriptCurveIntentKind::EndEdit);
		// The selection stays.
		CHECK(fixture.Selection() == Refs{{0, 1}});
	}

	SUBCASE("Alt breaks the tangents")
	{
		fixture.Press(*handle, 0, false, false, true);
		fixture.Move(target, 0, false, false, true);
		VisualScriptTimelineKey const& edited = fixture.LastTimeline().Tracks[0].Keys[1];
		REQUIRE(edited.Out.has_value());
		CHECK(edited.Out->x == doctest::Approx(1.0f).epsilon(1e-3));
		CHECK_FALSE(edited.In.has_value());
		fixture.Release(target, 0, false, false, true);
	}

	// A linear key has no handles.
	fixture.Timeline.Tracks[0].Keys[1].Interpolation = VisualScriptKeyInterpolation::Linear;
	CHECK_FALSE(fixture.View.HasTangentHandles(fixture.Timeline, 0, 1));
}

TEST_CASE("VisualScriptCurveView: a double-click adds a key on a curve with its value there, and an event on a lane")
{
	Fixture fixture;
	// On A's curve at 0.75 s (between 0.5 and -0.5).
	double const time = 0.75;
	float const value = EvaluateVisualScriptTimelineTrack(fixture.Timeline.Tracks[0], time).x;
	glm::vec2 const onCurve(fixture.View.ToScreenX(time), fixture.View.ToScreenY(value));
	CHECK(fixture.View.HitTest(fixture.Timeline, onCurve).Kind == VisualScriptCurveHitKind::Curve);
	fixture.Press(onCurve, 0, false, false, false, true);
	fixture.Release(onCurve);
	REQUIRE(fixture.Intents.size() == 1);
	CHECK(fixture.Intents[0].Kind == VisualScriptCurveIntentKind::Commit);
	CHECK(fixture.Intents[0].Action == "Add Key to A on Timeline");
	VisualScriptTimelineTrack const& added = fixture.Intents[0].Timeline.Tracks[0];
	REQUIRE(added.Keys.size() == 5);
	CHECK(added.Keys[2].Time == doctest::Approx(0.75f));
	CHECK(added.Keys[2].Value.x == doctest::Approx(value));
	CHECK(fixture.Selection() == Refs{{0, 2}});

	// On E's lane at 0.5 s.
	std::optional<std::pair<float, float>> const lane = fixture.View.GetLane(2);
	REQUIRE(lane.has_value());
	glm::vec2 const onLane(fixture.View.ToScreenX(0.5), (lane->first + lane->second) * 0.5f);
	CHECK(fixture.View.HitTest(fixture.Timeline, onLane).Kind == VisualScriptCurveHitKind::EventLane);
	fixture.Press(onLane, 0, false, false, false, true);
	fixture.Release(onLane);
	REQUIRE(fixture.Intents.size() == 2);
	CHECK(fixture.Intents[1].Action == "Add Key to E on Timeline");
	CHECK(fixture.Intents[1].Timeline.Tracks[2].Keys.size() == 2);
	CHECK(fixture.Intents[1].Timeline.Tracks[2].Keys[0].Time == doctest::Approx(0.5f));
}

TEST_CASE("VisualScriptCurveView: a right click on a key opens the menu of the selected keys, whose actions are one step each")
{
	Fixture fixture;
	fixture.Press(fixture.Key(0, 2), 1);
	fixture.Release(fixture.Key(0, 2), 1);
	REQUIRE(fixture.Intents.size() == 1);
	CHECK(fixture.Intents[0].Kind == VisualScriptCurveIntentKind::OpenKeyMenu);
	CHECK(fixture.Selection() == Refs{{0, 2}});

	fixture.View.Select({{0, 1}, {0, 2}, {2, 0}});
	std::optional<VisualScriptCurveIntent> const linear =
		fixture.View.SetInterpolation(fixture.Timeline, VisualScriptKeyInterpolation::Linear, "Timeline");
	REQUIRE(linear.has_value());
	CHECK(linear->Action == "Set 2 Keys to Linear on Timeline");
	CHECK(linear->Timeline.Tracks[0].Keys[2].Interpolation == VisualScriptKeyInterpolation::Linear);
	std::optional<VisualScriptCurveIntent> const flat = fixture.View.FlattenTangents(fixture.Timeline, "Timeline");
	REQUIRE(flat.has_value());
	CHECK(flat->Action == "Flatten Tangents of 2 Keys on Timeline");
	CHECK(flat->Timeline.Tracks[0].Keys[1].Out == glm::vec4(0.0f));
	std::optional<VisualScriptCurveIntent> const automatic = fixture.View.AutoTangents(flat->Timeline, "Timeline");
	REQUIRE(automatic.has_value());
	CHECK_FALSE(automatic->Timeline.Tracks[0].Keys[1].Out.has_value());
	std::optional<VisualScriptCurveIntent> const removed = fixture.View.DeleteSelection(fixture.Timeline, "Timeline");
	REQUIRE(removed.has_value());
	CHECK(removed->Action == "Delete 3 Keys on Timeline");
	CHECK(removed->Timeline.Tracks[0].Keys.size() == 2);
	CHECK(removed->Timeline.Tracks[2].Keys.empty());
	CHECK(fixture.Selection().empty());
	// Nothing selected: nothing to delete.
	CHECK_FALSE(fixture.View.DeleteSelection(fixture.Timeline, "Timeline").has_value());
}

TEST_CASE("VisualScriptCurveView: the wheel zooms time around the cursor, Ctrl+wheel the values, and right or middle drags pan")
{
	Fixture fixture;
	glm::vec2 const at(250.0f, 400.0f);
	double const timeAt = fixture.View.ToTime(at.x);
	double const valueAt = fixture.View.ToValue(at.y);
	fixture.Wheel(at, 1.0f);
	CHECK(fixture.View.ToTime(at.x) == doctest::Approx(timeAt));
	glm::dvec2 const zoomed = fixture.View.GetTimeRange();
	CHECK(zoomed.y - zoomed.x == doctest::Approx(2.0f / VisualScriptCurveView::ZoomStep));
	CHECK(fixture.View.GetValueRange() == glm::dvec2(-1.0, 1.0));

	fixture.Wheel(at, -1.0f, true);
	CHECK(fixture.View.ToValue(at.y) == doctest::Approx(valueAt));
	glm::dvec2 const values = fixture.View.GetValueRange();
	CHECK(values.y - values.x == doctest::Approx(2.0f * VisualScriptCurveView::ZoomStep));
	CHECK(fixture.View.GetTimeRange() == zoomed);

	for (int const button : {1, 2})
	{
		CAPTURE(button);
		glm::dvec2 const times = fixture.View.GetTimeRange();
		double const before = fixture.View.ToTime(500.0f);
		fixture.Press(glm::vec2(500.0f, 300.0f), button);
		fixture.Move(glm::vec2(550.0f, 300.0f), button);
		CHECK(fixture.View.GetGesture() == VisualScriptCurveGesture::Pan);
		fixture.Move(glm::vec2(600.0f, 300.0f), button);
		fixture.Release(glm::vec2(600.0f, 300.0f), button);
		// What was under 500 pixels is under 600 now.
		CHECK(fixture.View.ToTime(600.0f) == doctest::Approx(before));
		CHECK(fixture.View.GetTimeRange().y - fixture.View.GetTimeRange().x == doctest::Approx(times.y - times.x));
	}
	CHECK(fixture.Intents.empty());
}

TEST_CASE("VisualScriptCurveView: F frames everything, or the selection")
{
	Fixture fixture;
	fixture.View.SetTimeRange(10.0f, 20.0f);
	fixture.View.Frame(fixture.Timeline);
	glm::dvec2 const all = fixture.View.GetTimeRange();
	CHECK(all.x < 0.0f);
	CHECK(all.y > 2.0f);
	glm::dvec2 const values = fixture.View.GetValueRange();
	// B's Y reaches 0.5 and A's -0.5.
	CHECK(values.x < -0.5f);
	CHECK(values.y > 0.5f);

	fixture.View.Select({{0, 2}, {0, 3}});
	fixture.View.Frame(fixture.Timeline);
	glm::dvec2 const selected = fixture.View.GetTimeRange();
	CHECK(selected.x > 0.9f);
	CHECK(selected.x < 1.0f);
	CHECK(selected.y > 1.5f);
	CHECK(selected.y < 1.6f);
	// The values of the selected keys (-0.5 and 0) are framed around their middle.
	glm::dvec2 const framed = fixture.View.GetValueRange();
	CHECK((framed.x + framed.y) * 0.5f == doctest::Approx(-0.25f));
}

TEST_CASE("VisualScriptCurveView: Ctrl+A selects the keys of the visible tracks, Delete removes the selection and the arrows nudge it")
{
	Fixture fixture;
	fixture.View.SetTrackVisible("B", false);
	fixture.View.SelectAll(fixture.Timeline);
	CHECK(fixture.Selection() == Refs{{0, 0}, {0, 1}, {0, 2}, {0, 3}, {2, 0}});

	fixture.View.Select({{0, 1}, {2, 0}});
	std::optional<VisualScriptCurveIntent> const later = fixture.View.Nudge(fixture.Timeline, 1, "Timeline");
	REQUIRE(later.has_value());
	CHECK(later->Action == "Move 2 Keys on Timeline");
	CHECK(later->Timeline.Tracks[0].Keys[1].Time == doctest::Approx(0.51f));
	CHECK(later->Timeline.Tracks[2].Keys[0].Time == doctest::Approx(1.26f));
	std::optional<VisualScriptCurveIntent> const earlier = fixture.View.Nudge(fixture.Timeline, -10, "Timeline");
	REQUIRE(earlier.has_value());
	CHECK(earlier->Timeline.Tracks[0].Keys[1].Time == doctest::Approx(0.4f));
	// Against a neighbour: A's key 1 cannot pass the key at 0 s, so 100 steps back stop there.
	std::optional<VisualScriptCurveIntent> const blocked = fixture.View.Nudge(fixture.Timeline, -100, "Timeline");
	REQUIRE(blocked.has_value());
	CHECK(blocked->Timeline.Tracks[0].Keys[1].Time == doctest::Approx(0.0f));

	std::optional<VisualScriptCurveIntent> const removed = fixture.View.DeleteSelection(fixture.Timeline, "Timeline");
	REQUIRE(removed.has_value());
	CHECK(removed->Kind == VisualScriptCurveIntentKind::Commit);
	CHECK(removed->Timeline.Tracks[0].Keys.size() == 3);
	CHECK(removed->Timeline.Tracks[2].Keys.empty());
	// Nothing selected: the arrows do nothing.
	CHECK_FALSE(fixture.View.Nudge(fixture.Timeline, 1, "Timeline").has_value());
}

TEST_CASE("VisualScriptCurveView: hidden tracks and components are not hit, framed or selected")
{
	Fixture fixture;
	glm::vec2 const keyA = fixture.Key(0, 1);
	glm::vec2 const keyBy = fixture.Key(1, 0, 1);
	CHECK(fixture.View.HitTest(fixture.Timeline, keyA).Kind == VisualScriptCurveHitKind::Key);
	VisualScriptCurveHit const hitB = fixture.View.HitTest(fixture.Timeline, keyBy);
	CHECK(hitB.Kind == VisualScriptCurveHitKind::Key);
	CHECK(hitB.Component == 1);

	fixture.View.SetTrackVisible("A", false);
	fixture.View.SetComponentVisible("B", 1, false);
	CHECK_FALSE(fixture.View.GetTrackView("A").Visible);
	CHECK(fixture.View.HitTest(fixture.Timeline, keyA).Kind != VisualScriptCurveHitKind::Key);
	CHECK(fixture.View.HitTest(fixture.Timeline, keyBy).Kind != VisualScriptCurveHitKind::Key);
	// B's X component is still hit.
	CHECK(fixture.View.HitTest(fixture.Timeline, fixture.Key(1, 0, 0)).Kind == VisualScriptCurveHitKind::Key);

	// Framing everything leaves A and B's Y (0.5) out: only B's X (0.25) is framed among the values, around its value.
	fixture.View.Frame(fixture.Timeline);
	glm::dvec2 const values = fixture.View.GetValueRange();
	CHECK((values.x + values.y) * 0.5f == doctest::Approx(0.25f));
	// A marquee over everything selects the visible keys only.
	fixture.Press(glm::vec2(1.0f, 15.0f));
	fixture.Move(glm::vec2(999.0f, 599.0f));
	fixture.Release(glm::vec2(999.0f, 599.0f));
	CHECK(fixture.Selection() == Refs{{1, 0}, {2, 0}});
	// A hidden event track loses its lane.
	fixture.View.SetTrackVisible("E", false);
	fixture.View.Sync(fixture.Timeline);
	CHECK_FALSE(fixture.View.GetLane(2).has_value());
	// The eye and the toggles are kept by name.
	fixture.Timeline.Tracks.erase(fixture.Timeline.Tracks.begin());
	fixture.View.Sync(fixture.Timeline);
	CHECK_FALSE(fixture.View.IsVisible(fixture.Timeline, 0, 1));
	CHECK(fixture.View.IsVisible(fixture.Timeline, 0, 0));
}

TEST_CASE("VisualScriptCurveView: tracks with huge values frame to a finite range in which their keys can be drawn and hit")
{
	struct Case
	{
		char const* Name;
		std::vector<std::pair<float, float>> Keys;
	};
	for (Case const& tested :
	     {Case{"one key at 1e8", {{0.5f, 1.0e8f}}}, Case{"one key at 1e16", {{0.5f, 1.0e16f}}}, Case{"one key at 1e17", {{0.5f, 1.0e17f}}},
	      Case{"keys at 0 and 3e17", {{0.0f, 0.0f}, {1.0f, 3.0e17f}}}, Case{"keys at -3e38 and 3e38", {{0.0f, -3.0e38f}, {1.0f, 3.0e38f}}},
	      Case{"one key at -3e20", {{0.5f, -3.0e20f}}}})
	{
		CAPTURE(tested.Name);
		VisualScriptTimeline timeline;
		timeline.Length = 1.0f;
		timeline.Tracks.push_back(MakeTrack("A", "float", tested.Keys));
		VisualScriptCurveView view;
		view.SetRect(glm::vec2(0.0f), glm::vec2(1000.0f, 600.0f), FontSize);
		// The first Sync frames everything, as the drawer's first frame does.
		view.Sync(timeline);
		glm::dvec2 const values = view.GetValueRange();
		REQUIRE(std::isfinite(values.x));
		REQUIRE(std::isfinite(values.y));
		REQUIRE(values.y > values.x);
		// Wide enough for the floats about its middle to step within it.
		CHECK(values.y - values.x >= 0.999 * VisualScriptCurveView::RelativeMinSpan * std::abs(values.x * 0.5 + values.y * 0.5));
		for (uint32_t key = 0; key < tested.Keys.size(); key++)
		{
			CAPTURE(key);
			std::optional<glm::vec2> const position = view.GetKeyPosition(timeline, 0, key, 0);
			REQUIRE(position.has_value());
			CHECK(std::isfinite(position->y));
			CHECK(position->y >= view.GetCurvesTop());
			CHECK(position->y <= view.GetMax().y);
			VisualScriptCurveHit const hit = view.HitTest(timeline, *position);
			CHECK(hit.Kind == VisualScriptCurveHitKind::Key);
			CHECK(hit.Key == key);
			CHECK(std::isfinite(hit.Value));
		}
		// Zooming in as far as it goes and panning keep the range finite and open; so does zooming out as far as it goes.
		for (int notch = 0; notch < 400; notch++)
		{
			view.ZoomValues(1.0f / VisualScriptCurveView::ZoomStep, 300.0f);
		}
		view.Pan(glm::vec2(0.0f, 1.0e6f));
		glm::dvec2 const zoomed = view.GetValueRange();
		CHECK(std::isfinite(zoomed.x));
		CHECK(zoomed.y > zoomed.x);
		for (int notch = 0; notch < 400; notch++)
		{
			view.ZoomValues(VisualScriptCurveView::ZoomStep, 300.0f);
		}
		glm::dvec2 const widened = view.GetValueRange();
		CHECK(std::isfinite(widened.x));
		CHECK(std::isfinite(widened.y));
		CHECK(widened.y - widened.x <= VisualScriptCurveView::MaxValueSpan);
		// A key dragged across the whole view moves by a finite amount, or not at all.
		view.Frame(timeline);
		std::optional<glm::vec2> const grabbed = view.GetKeyPosition(timeline, 0, 0, 0);
		REQUIRE(grabbed.has_value());
		VisualScriptCurveViewInput press;
		press.Mouse = *grabbed;
		press.Hovered = true;
		press.Pressed[0] = true;
		press.Down[0] = true;
		CHECK(view.Update(press, timeline, "Timeline").empty());
		VisualScriptCurveViewInput drag;
		drag.Mouse = glm::vec2(grabbed->x, view.GetCurvesTop());
		drag.Hovered = true;
		drag.Down[0] = true;
		for (VisualScriptCurveIntent const& intent : view.Update(drag, timeline, "Timeline"))
		{
			if (intent.Kind == VisualScriptCurveIntentKind::Edit)
			{
				CHECK(std::isfinite(intent.Timeline.Tracks[0].Keys[0].Value.x));
			}
		}
		CHECK(view.CancelGesture().size() <= 1);
	}
}

TEST_CASE("VisualScriptCurveView: Escape cancels a drag back to where it started and a marquee back to the selection before it")
{
	Fixture fixture;
	glm::vec2 const grab = fixture.Key(0, 1);
	fixture.Press(grab);
	fixture.Move(grab + glm::vec2(50.0f, -40.0f));
	REQUIRE(fixture.View.GetGesture() == VisualScriptCurveGesture::MoveKeys);
	REQUIRE_FALSE(fixture.Intents.empty());
	std::vector<VisualScriptCurveIntent> cancelled = fixture.View.CancelGesture();
	REQUIRE(cancelled.size() == 1);
	CHECK(cancelled[0].Kind == VisualScriptCurveIntentKind::CancelEdit);
	CHECK(cancelled[0].Timeline == fixture.Timeline);
	CHECK(fixture.View.GetGesture() == VisualScriptCurveGesture::Idle);
	// The mouse still held, then let go: nothing more.
	fixture.Intents.clear();
	fixture.Move(grab + glm::vec2(80.0f, -40.0f));
	fixture.Release(grab + glm::vec2(80.0f, -40.0f));
	CHECK(fixture.Intents.empty());
	CHECK(fixture.Selection() == Refs{{0, 1}});

	// A tangent handle's drag likewise.
	std::optional<glm::vec2> const handle = fixture.View.GetTangentHandle(fixture.Timeline, 0, 1, 0, true);
	REQUIRE(handle.has_value());
	fixture.Press(*handle);
	fixture.Move(*handle + glm::vec2(0.0f, -40.0f));
	REQUIRE(fixture.View.GetGesture() == VisualScriptCurveGesture::Tangent);
	cancelled = fixture.View.CancelGesture();
	REQUIRE(cancelled.size() == 1);
	CHECK(cancelled[0].Kind == VisualScriptCurveIntentKind::CancelEdit);
	CHECK(cancelled[0].Timeline == fixture.Timeline);
	fixture.Release(*handle + glm::vec2(0.0f, -40.0f));

	// A marquee that has selected other keys gives back the selection it started from.
	fixture.View.Select({{0, 3}});
	glm::vec2 const from(fixture.Key(0, 0).x - 20.0f, 30.0f);
	glm::vec2 const to(fixture.Key(0, 1).x + 20.0f, 590.0f);
	fixture.Press(from);
	fixture.Move(to);
	REQUIRE(fixture.View.GetGesture() == VisualScriptCurveGesture::Marquee);
	CHECK(fixture.Selection() == Refs{{0, 0}, {0, 1}, {1, 0}});
	CHECK(fixture.View.CancelGesture().empty());
	CHECK(fixture.Selection() == Refs{{0, 3}});
	CHECK_FALSE(fixture.View.GetMarquee().has_value());
	fixture.Release(to);
	CHECK(fixture.Selection() == Refs{{0, 3}});

	// A press that has not moved yet has nothing to take back but the selection it made.
	fixture.Press(fixture.Key(0, 2));
	CHECK(fixture.Selection() == Refs{{0, 2}});
	CHECK(fixture.View.CancelGesture().empty());
	CHECK(fixture.Selection() == Refs{{0, 3}});
}

TEST_CASE("VisualScriptCurveView: the selection and the toggles follow tracks that are removed or renamed")
{
	Fixture fixture;
	fixture.View.Select({{1, 0}, {2, 0}});
	fixture.View.SetComponentVisible("B", 1, false);
	// A removed: B and E move up a place, and their keys stay selected.
	fixture.Timeline.Tracks.erase(fixture.Timeline.Tracks.begin());
	fixture.View.Sync(fixture.Timeline);
	CHECK(fixture.Selection() == Refs{{0, 0}, {1, 0}});
	CHECK(fixture.View.FollowTrack(0) == std::nullopt);
	CHECK(fixture.View.FollowTrack(1) == std::optional<uint32_t>(0));
	CHECK(fixture.View.FollowTrack(2) == std::optional<uint32_t>(1));
	// Delete then removes the keys that were selected, not other tracks' keys.
	std::optional<VisualScriptCurveIntent> const removed = fixture.View.DeleteSelection(fixture.Timeline, "Timeline");
	REQUIRE(removed.has_value());
	CHECK(removed->Action == "Delete 2 Keys on Timeline");
	CHECK(removed->Timeline.Tracks[0].Keys.empty());
	CHECK(removed->Timeline.Tracks[1].Keys.empty());

	// B renamed in its place: its toggles and its selected key go with it.
	fixture.View.Select({{0, 0}});
	fixture.Timeline.Tracks[0].Name = "Bee";
	fixture.View.Sync(fixture.Timeline);
	CHECK(fixture.Selection() == Refs{{0, 0}});
	CHECK(fixture.View.FollowTrack(0) == std::optional<uint32_t>(0));
	CHECK_FALSE(fixture.View.GetTrackView("Bee").Components[1]);
	CHECK_FALSE(fixture.View.IsVisible(fixture.Timeline, 0, 1));
	// Unchanged tracks stay where they are.
	fixture.View.Sync(fixture.Timeline);
	CHECK(fixture.View.FollowTrack(1) == std::optional<uint32_t>(1));
	CHECK(fixture.View.FollowTrack(2) == std::nullopt);
}

TEST_CASE("VisualScriptCurveView: framing skips selected keys the timeline it is given does not have")
{
	Fixture fixture;
	// A double-click adds a second event at 1.5 s and selects it, as (2, 1) of the timeline the commit will write.
	std::optional<std::pair<float, float>> const lane = fixture.View.GetLane(2);
	REQUIRE(lane.has_value());
	glm::vec2 const onLane(fixture.View.ToScreenX(1.5), (lane->first + lane->second) * 0.5f);
	fixture.Press(onLane, 0, false, false, false, true);
	fixture.Release(onLane);
	REQUIRE(fixture.Selection() == Refs{{2, 1}});
	// F in the same frame frames the timeline drawn, which has one event: everything is framed.
	fixture.View.SetTimeRange(10.0, 20.0);
	fixture.View.Frame(fixture.Timeline);
	glm::dvec2 const times = fixture.View.GetTimeRange();
	CHECK(times.x < 0.0);
	CHECK(times.y > 2.0);
	// With keys that exist among them, those are framed.
	fixture.View.Select({{0, 3}, {2, 1}, {7, 0}});
	fixture.View.Frame(fixture.Timeline);
	glm::dvec2 const selected = fixture.View.GetTimeRange();
	CHECK(selected.x < 1.5);
	CHECK(selected.y > 1.5);
	CHECK(selected.y - selected.x < 1.0);
}
