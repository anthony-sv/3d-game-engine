#include "Editor/UI/CurveEditing.h"
#include "Editor/UI/CurveView.h"

#include "Strada/Math/Curve.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float FontSize = 10.0f;

	using CurveIntent = CurveDocumentTraits<Curve>::Intent;

	CurveKey MakeKey(float time, float value)
	{
		CurveKey key;
		key.Time = time;
		key.Value = value;
		return key;
	}

	// A torque curve over revolutions per minute: keys at 1000, 3000 and 5000, the first leaving with a set slope (so the
	// Linear pre-extrapolation slopes), the second holding an unset In slope's number (7, not used while HasIn is false).
	Curve MakeTorque()
	{
		Curve curve;
		curve.PreExtrapolation = CurveExtrapolation::Linear;
		curve.PostExtrapolation = CurveExtrapolation::Hold;
		curve.Keys = {MakeKey(1000.0f, 0.0f), MakeKey(3000.0f, 100.0f), MakeKey(5000.0f, 50.0f)};
		curve.Keys[0].HasOut = true;
		curve.Keys[0].Out = 0.05f;
		curve.Keys[1].In = 7.0f;
		return curve;
	}

	bool IsSorted(Curve const& curve)
	{
		for (size_t index = 1; index < curve.Keys.size(); index++)
		{
			if (curve.Keys[index].Time < curve.Keys[index - 1].Time)
			{
				return false;
			}
		}
		return true;
	}

	// The torque curve in a view of 1000 x 600 pixels showing 0 to 6000 and -50 to 150.
	struct Fixture
	{
		Curve Torque = MakeTorque();
		CurveView<Curve> View;
		std::vector<CurveIntent> Intents;

		Fixture()
		{
			View.SetRect(glm::vec2(0.0f), glm::vec2(1000.0f, 600.0f), FontSize);
			View.Sync(Torque);
			View.SetTimeRange(0.0, 6000.0);
			View.SetValueRange(-50.0, 150.0);
		}

		glm::vec2 Key(uint32_t key) const
		{
			std::optional<glm::vec2> const position = View.GetKeyPosition(Torque, 0, key, 0);
			REQUIRE(position.has_value());
			return *position;
		}

		void Frame(glm::vec2 mouse, bool pressed, bool down, bool alt = false, bool doubleClick = false)
		{
			CurveViewInput input;
			input.Mouse = mouse;
			input.Hovered = true;
			input.Pressed[0] = pressed;
			input.Down[0] = down;
			input.Released[0] = !down;
			input.Alt = alt;
			input.DoubleClicked = doubleClick;
			for (CurveIntent& intent : View.Update(input, Torque, "Torque"))
			{
				Intents.push_back(intent);
			}
		}

		// A left drag from one point to another in three frames, released at the target.
		void Drag(glm::vec2 from, glm::vec2 to, bool alt = false)
		{
			Frame(from, true, true, alt);
			Frame(from + (to - from) * 0.5f, false, true, alt);
			Frame(to, false, true, alt);
			Frame(to, false, false, alt);
		}
	};
}

TEST_CASE("CurveEditing: edits of a curve keep its keys sorted and finite, keep the numbers of unset slopes, and refusals change nothing")
{
	Curve const start = MakeTorque();

	// Moved as a block from the start: the block stops where its last key reaches the unselected key at 5000.
	std::vector<CurveKeyRef> const block = {{0, 0}, {0, 1}};
	Curve moved = start;
	REQUIRE(MoveCurveKeys(moved, block, 5000.0f, 1.0f).IsOk());
	CHECK(moved.Keys[0].Time == 3000.0f);
	CHECK(moved.Keys[1].Time == 5000.0f);
	CHECK(moved.Keys[0].Value == 1.0f);
	CHECK(moved.Keys[2] == start.Keys[2]);
	CHECK(IsSorted(moved));
	CHECK(ValidateCurve(moved).IsOk());
	// Dragged back to an offset of 0: every key where it was.
	Curve back = start;
	REQUIRE(MoveCurveKeys(back, block, 0.0f, 0.0f).IsOk());
	CHECK(back == start);
	// A key's time is any finite number: before the first key the block goes as far as the floats.
	Curve early = start;
	REQUIRE(MoveCurveKeys(early, block, -1.0e6f, 0.0f).IsOk());
	CHECK(early.Keys[0].Time == doctest::Approx(1000.0f - 1.0e6f));
	CHECK(ValidateCurve(early).IsOk());

	// Refusals.
	Curve refused = start;
	std::vector<CurveKeyRef> const missing = {{0, 9}};
	std::vector<CurveKeyRef> const otherTrack = {{1, 0}};
	CHECK_FALSE(MoveCurveKeys(refused, missing, 1.0f, 0.0f).IsOk());
	CHECK_FALSE(MoveCurveKeys(refused, otherTrack, 1.0f, 0.0f).IsOk());
	CHECK_FALSE(MoveCurveKeys(refused, block, std::numeric_limits<float>::quiet_NaN(), 0.0f).IsOk());
	refused.Keys[1].Value = 3.0e38f;
	Curve const large = refused;
	Result<void> const overflow = MoveCurveKeys(refused, block, 0.0f, 1.0e38f);
	REQUIRE_FALSE(overflow.IsOk());
	CHECK(overflow.GetError() == "a key's value is finite: the move takes it past the largest float");
	CHECK(refused == large);

	// Keys are added in time order, after the keys at their time, up to MaxCurveKeys.
	Curve added = start;
	CHECK(AddCurveKey(added, 3000.0f, 9.0f).GetValue() == 2);
	CHECK(AddCurveKey(added, 0.0f, 1.0f).GetValue() == 0);
	CHECK(added.Keys.size() == 5);
	CHECK(IsSorted(added));
	CHECK_FALSE(added.Keys[3].HasIn);
	CHECK_FALSE(AddCurveKey(added, std::numeric_limits<float>::infinity(), 0.0f).IsOk());
	CHECK_FALSE(AddCurveKey(added, 0.0f, std::numeric_limits<float>::quiet_NaN()).IsOk());
	Curve full;
	for (size_t index = 0; index < MaxCurveKeys; index++)
	{
		full.Keys.push_back(MakeKey(static_cast<float>(index), 0.0f));
	}
	CHECK_FALSE(AddCurveKey(full, 0.5f, 0.0f).IsOk());
	CHECK(full.Keys.size() == MaxCurveKeys);

	// Removing, values, interpolation and slopes.
	Curve edited = start;
	std::vector<CurveKeyRef> const first = {{0, 0}};
	REQUIRE(RemoveCurveKeys(edited, first).IsOk());
	CHECK(edited.Keys.size() == 2);
	CHECK(edited.Keys[0] == start.Keys[1]);
	REQUIRE(SetCurveKeyValue(edited, 0, 4.0f).IsOk());
	CHECK(edited.Keys[0].Value == 4.0f);
	CHECK_FALSE(SetCurveKeyValue(edited, 0, std::numeric_limits<float>::infinity()).IsOk());
	std::vector<CurveKeyRef> const both = {{0, 0}, {0, 1}};
	REQUIRE(SetCurveKeyInterpolation(edited, both, CurveInterpolation::Constant).IsOk());
	CHECK(edited.Keys[1].Interpolation == CurveInterpolation::Constant);
	// Only the side set changes; the other keeps the number it holds while unset.
	REQUIRE(SetCurveKeyTangents(edited, 0, std::nullopt, 2.0f).IsOk());
	CHECK_FALSE(edited.Keys[0].HasIn);
	CHECK(edited.Keys[0].In == 7.0f);
	CHECK(edited.Keys[0].HasOut);
	CHECK(edited.Keys[0].Out == 2.0f);
	CHECK_FALSE(SetCurveKeyTangents(edited, 0, std::numeric_limits<float>::infinity(), std::nullopt).IsOk());
	REQUIRE(ResetCurveTangents(edited, both).IsOk());
	CHECK_FALSE(edited.Keys[0].HasOut);
	CHECK(edited.Keys[0].Out == 2.0f);
	REQUIRE(FlattenCurveTangents(edited, both).IsOk());
	CHECK(edited.Keys[1].HasIn);
	CHECK(edited.Keys[1].In == 0.0f);
	CHECK(edited.Keys[1].Out == 0.0f);
	std::vector<CurveKeyRef> const none;
	Curve const kept = edited;
	CHECK_FALSE(FlattenCurveTangents(edited, none).IsOk());
	CHECK_FALSE(SetCurveKeyInterpolation(edited, missing, CurveInterpolation::Linear).IsOk());
	CHECK(edited == kept);
	CHECK(ValidateCurve(edited).IsOk());
}

TEST_CASE("CurveView: a curve frames on its keys' span, draws its extrapolations beyond them, and steps by a hundredth of the view")
{
	Curve const torque = MakeTorque();
	CurveView<Curve> view;
	view.SetRect(glm::vec2(0.0f), glm::vec2(1000.0f, 600.0f), FontSize);
	view.Sync(torque);
	// Without a selection, an unbounded domain frames the keys' span, padded.
	glm::dvec2 const times = view.GetTimeRange();
	CHECK(times.x < 1000.0);
	CHECK(times.x > 0.0);
	CHECK(times.y > 5000.0);
	CHECK(times.y < 6000.0);
	glm::dvec2 const values = view.GetValueRange();
	CHECK(values.x < 0.0);
	CHECK(values.y > 100.0);
	// Snapping and nudges follow the view: 1, 2 or 5 times a power of ten near a hundredth of the visible span.
	double const step = view.GetTimeStep();
	CHECK(step >= (times.y - times.x) * 0.01);
	CHECK(step <= (times.y - times.x) * 0.05);
	view.SetTimeRange(0.0, 6000.0);
	CHECK(view.GetTimeStep() == 100.0);
	// Wide spans an RPM axis needs are kept, which a timeline's 7200 s bound would refuse.
	view.SetTimeRange(-20000.0, 20000.0);
	CHECK(view.GetTimeRange() == glm::dvec2(-20000.0, 20000.0));
	view.SetTimeRange(0.0, 6000.0);
	view.SetValueRange(-50.0, 150.0);

	// Before the first key the curve goes on along its first slope (Linear), and the view hits it there.
	double const before = 500.0;
	float const extrapolated = torque.Evaluate(before);
	CHECK(extrapolated == doctest::Approx(-25.0f));
	CurveHit const hit = view.HitTest(torque, glm::vec2(view.ToScreenX(before), view.ToScreenY(extrapolated)));
	CHECK(hit.Kind == CurveHitKind::Curve);
	CHECK(hit.Track == 0);
	CHECK(CurveDocumentTraits<Curve>::Evaluate(torque, 0, before).x == extrapolated);
	CHECK_FALSE(CurveDocumentTraits<Curve>::GetDomain(torque).Bounded);
}

TEST_CASE("CurveView: a curve's keys drag snapped to the view's step, and a broken tangent keeps the other side's unset number")
{
	Fixture fixture;
	// Key 1 (3000, 100) dragged 130 pixels later (780 at 6 per pixel): snapped to the step of 100, so 800 later.
	glm::vec2 const key = fixture.Key(1);
	fixture.Drag(key, key + glm::vec2(130.0f, 0.0f));
	REQUIRE(fixture.Intents.size() >= 2);
	CHECK(fixture.Intents.back().Kind == CurveIntentKind::EndEdit);
	CurveIntent const& dragged = fixture.Intents[fixture.Intents.size() - 2];
	CHECK(dragged.Kind == CurveIntentKind::Edit);
	CHECK(dragged.Action == "Move Key of Curve on Torque");
	CHECK(dragged.Document.Keys[1].Time == 3800.0f);
	CHECK(dragged.Document.Keys[1].Value == 100.0f);
	CHECK(IsSorted(dragged.Document));
	// Free with Alt: 780 exactly (to the pixel).
	fixture.Intents.clear();
	fixture.Drag(key, key + glm::vec2(130.0f, 0.0f), true);
	REQUIRE(fixture.Intents.size() >= 2);
	CHECK(fixture.Intents[fixture.Intents.size() - 2].Document.Keys[1].Time == doctest::Approx(3780.0f));

	// The selected cubic key's out handle, dragged with Alt: only Out is set; In stays unset with its number.
	REQUIRE(fixture.View.IsSelected(0, 1));
	std::optional<glm::vec2> const handle = fixture.View.GetTangentHandle(fixture.Torque, 0, 1, 0, true);
	REQUIRE(handle.has_value());
	CHECK(fixture.View.HitTest(fixture.Torque, *handle).Kind == CurveHitKind::OutTangent);
	fixture.Intents.clear();
	fixture.Drag(*handle, *handle + glm::vec2(0.0f, -30.0f), true);
	REQUIRE(fixture.Intents.size() >= 2);
	CurveIntent const& tangent = fixture.Intents[fixture.Intents.size() - 2];
	CHECK(tangent.Action == "Edit Tangent of Curve on Torque");
	CurveKey const& broken = tangent.Document.Keys[1];
	CHECK(broken.HasOut);
	CHECK(broken.Out > 0.0f);
	CHECK_FALSE(broken.HasIn);
	CHECK(broken.In == 7.0f);
}

TEST_CASE("CurveView: a double-click adds a key with the curve's value there, Delete removes the selection, the arrows nudge it")
{
	Fixture fixture;
	// On the curve at 2000: a key there (snapped), with the curve's value, selected.
	double const time = 2000.0;
	float const value = fixture.Torque.Evaluate(time);
	glm::vec2 const onCurve(fixture.View.ToScreenX(time), fixture.View.ToScreenY(value));
	REQUIRE(fixture.View.HitTest(fixture.Torque, onCurve).Kind == CurveHitKind::Curve);
	fixture.Frame(onCurve, true, true, false, true);
	fixture.Frame(onCurve, false, false);
	REQUIRE(fixture.Intents.size() == 1);
	CHECK(fixture.Intents[0].Kind == CurveIntentKind::Commit);
	CHECK(fixture.Intents[0].Action == "Add Key to Curve on Torque");
	Curve const added = fixture.Intents[0].Document;
	REQUIRE(added.Keys.size() == 4);
	CHECK(added.Keys[1].Time == 2000.0f);
	CHECK(added.Keys[1].Value == value);
	CHECK(fixture.View.IsSelected(0, 1));

	// The arrows move the selection by the view's step, and stop at a neighbour.
	fixture.Torque = added;
	fixture.View.Sync(fixture.Torque);
	std::optional<CurveIntent> const later = fixture.View.Nudge(fixture.Torque, 1, "Torque");
	REQUIRE(later.has_value());
	CHECK(later->Document.Keys[1].Time == 2100.0f);
	CHECK(later->Action == "Move Key of Curve on Torque");
	std::optional<CurveIntent> const earlier = fixture.View.Nudge(fixture.Torque, -100, "Torque");
	REQUIRE(earlier.has_value());
	CHECK(earlier->Document.Keys[1].Time == 1000.0f);

	// Delete removes the selected keys, one step.
	fixture.View.SelectAll(fixture.Torque);
	CHECK(fixture.View.GetSelection().size() == 4);
	std::optional<CurveIntent> const removed = fixture.View.DeleteSelection(fixture.Torque, "Torque");
	REQUIRE(removed.has_value());
	CHECK(removed->Action == "Delete 4 Keys on Torque");
	CHECK(removed->Document.Keys.empty());
	CHECK(fixture.View.GetSelection().empty());
	CHECK_FALSE(fixture.View.DeleteSelection(fixture.Torque, "Torque").has_value());
}
