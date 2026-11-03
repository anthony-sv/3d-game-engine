#include "Fuzzing.h"
#include "VisualScriptTestDocuments.h"

#include "Editor/VisualScripting/VisualScriptCurveEditor.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptLanguageEditing.h"
#include "Editor/VisualScripting/VisualScriptTimeline.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	template<typename T>
	T RequireOk(Result<T> result)
	{
		REQUIRE_MESSAGE(result.IsOk(), result.GetError());
		return result.TakeValue();
	}

	void RequireOk(Result<void> const& result)
	{
		REQUIRE_MESSAGE(result.IsOk(), result.GetError());
	}

	Json ParseText(std::string_view text)
	{
		Result<Json> parsed = ParseJson(text);
		REQUIRE_MESSAGE(parsed.IsOk(), parsed.GetError());
		return parsed.TakeValue();
	}

	// spec-slice3 §3.8.2's example.
	Json MakeExampleSetting()
	{
		return ParseText(R"({
			"Length": 2.0, "Loop": false,
			"Tracks": [
				{ "Name": "Lift", "Type": "float", "Keys": [
					{ "Time": 0.0, "Value": 0.0 },
					{ "Time": 1.0, "Value": 2.5, "Interpolation": "Linear" },
					{ "Time": 2.0, "Value": 0.0, "In": 0.0, "Out": 0.0 } ] },
				{ "Name": "Tint", "Type": "Strada.Color", "Keys": [ { "Time": 0.0, "Value": [1, 1, 1, 1] }, { "Time": 2.0, "Value": [1, 0.25, 0.25, 1] } ] },
				{ "Name": "Step", "Type": "exec", "Keys": [ { "Time": 0.5 }, { "Time": 1.5 } ] }
			] })");
	}

	// The timeline of Documents/Language.json's Flow.Timeline node.
	Json LoadLanguageTimeline()
	{
		Result<std::string> const text =
			FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Documents" / "Language.json");
		REQUIRE(text.IsOk());
		Json const document = ParseText(text.GetValue());
		for (Json const& node : document["Graph"]["Nodes"])
		{
			if (node["Type"] == "Flow.Timeline")
			{
				return node["Settings"]["Timeline"];
			}
		}
		FAIL("Documents/Language.json has no Flow.Timeline node");
		return Json();
	}

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
			key.Value = glm::vec4(value, 0.0f, 0.0f, 0.0f);
			track.Keys.push_back(key);
		}
		return track;
	}

	std::string ParseError(std::string_view text)
	{
		Result<VisualScriptTimeline> const parsed = ParseVisualScriptTimeline(ParseText(text));
		REQUIRE_FALSE(parsed.IsOk());
		return parsed.GetError();
	}

	bool IsSorted(VisualScriptTimeline const& timeline)
	{
		for (VisualScriptTimelineTrack const& track : timeline.Tracks)
		{
			for (size_t key = 0; key < track.Keys.size(); key++)
			{
				if (track.Keys[key].Time < 0.0f || track.Keys[key].Time > timeline.Length ||
				    (key > 0 && track.Keys[key].Time < track.Keys[key - 1].Time))
				{
					return false;
				}
			}
		}
		return true;
	}

	std::vector<float> GetTimes(VisualScriptTimelineTrack const& track)
	{
		std::vector<float> times;
		for (VisualScriptTimelineKey const& key : track.Keys)
		{
			times.push_back(key.Time);
		}
		return times;
	}

	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = glm::vec2(0.0f);
		node.Settings = std::move(settings);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}
}

TEST_CASE("VisualScriptTimeline: a Timeline setting reads with the runtime's rules and writes its canonical form")
{
	Json const setting = MakeExampleSetting();
	VisualScriptTimeline const timeline = RequireOk(ParseVisualScriptTimeline(setting));
	REQUIRE(timeline.Tracks.size() == 3);
	CHECK(timeline.Length == 2.0f);
	CHECK_FALSE(timeline.Loop);
	VisualScriptTimelineTrack const& lift = timeline.Tracks[0];
	CHECK(lift.Components == 1);
	CHECK(lift.Keys[1].Interpolation == VisualScriptKeyInterpolation::Linear);
	CHECK_FALSE(lift.Keys[0].In.has_value());
	CHECK(lift.Keys[2].In == glm::vec4(0.0f));
	CHECK(timeline.Tracks[1].Components == 4);
	CHECK(timeline.Tracks[1].Keys[1].Value == glm::vec4(1.0f, 0.25f, 0.25f, 1.0f));
	CHECK(timeline.Tracks[2].Components == 0);
	CHECK(GetTimes(timeline.Tracks[2]) == std::vector<float>{0.5f, 1.5f});

	// The canonical form: the members in the order of §3.8.2, Interpolation left out when Cubic, tangents when auto, numbers as
	// the literal codec writes floats (whole numbers without a fraction).
	Json const written = WriteVisualScriptTimeline(timeline);
	CHECK(written.dump() ==
	      R"({"Length":2,"Loop":false,"Tracks":[{"Name":"Lift","Type":"float","Keys":[{"Time":0,"Value":0},{"Time":1,"Value":2.5,)"
	      R"("Interpolation":"Linear"},{"Time":2,"Value":0,"In":0,"Out":0}]},{"Name":"Tint","Type":"Strada.Color","Keys":[{"Time":0,)"
	      R"("Value":[1,1,1,1]},{"Time":2,"Value":[1,0.25,0.25,1]}]},{"Name":"Step","Type":"exec","Keys":[{"Time":0.5},{"Time":1.5}]}]})");
	CHECK(RequireOk(ParseVisualScriptTimeline(written)) == timeline);

	// Members in another order, an explicit Cubic and a missing Keys read as the same timeline, written in the canonical order.
	Json const shuffled = ParseText(R"({"Tracks": [{"Keys": [{"Value": 1, "Interpolation": "Cubic", "Time": 0.25}], "Type": "float",
		"Name": "A"}, {"Type": "exec", "Name": "B"}], "Loop": true, "Length": 1})");
	Json const canonical = WriteVisualScriptTimeline(RequireOk(ParseVisualScriptTimeline(shuffled)));
	CHECK(canonical.dump() == R"({"Length":1,"Loop":true,"Tracks":[{"Name":"A","Type":"float","Keys":[{"Time":0.25,"Value":1}]},)"
	                          R"({"Name":"B","Type":"exec","Keys":[]}]})");

	// The default timeline: 1 s, no loop, no tracks, which a node without the setting has.
	CHECK(RequireOk(ParseVisualScriptTimeline(Json::object())) == VisualScriptTimeline{});
	CHECK(RequireOk(ReadVisualScriptTimeline(MakeNode(1, "Flow.Timeline"))) == VisualScriptTimeline{});
	CHECK(RequireOk(ParseVisualScriptTimeline(LoadLanguageTimeline())).Tracks.size() >= 4);
}

TEST_CASE("VisualScriptTimeline: a damaged setting is refused with the runtime's path and words")
{
	CHECK(ParseError(R"({"Length": 2, "Tracks": [{"Name": "A", "Type": "float", "Keys": [{"Time": 0, "Value": 0}, {"Time": 0.75,
		"Value": 0}, {"Time": 1, "Value": 0}, {"Time": 0.5, "Value": 0}]}, {"Name": "B", "Type": "float", "Keys": [{"Time": 0, "Value": 0},
		{"Time": 0.75, "Value": 0}, {"Time": 1, "Value": 0}, {"Time": 0.5, "Value": 0}]}]})") ==
	      "Timeline.Tracks[0].Keys[3].Time: keys are in time order: 0.5 comes after 1");
	CHECK(ParseError(R"({"Length": 0})") == "Timeline.Length: expected a number greater than 0 and at most 3600");
	CHECK(ParseError(R"({"Length": 3601})") == "Timeline.Length: expected a number greater than 0 and at most 3600");
	CHECK(ParseError(R"({"Tracks": [{"Name": "A", "Type": "int"}]})") ==
	      "Timeline.Tracks[0].Type: expected float, Strada.Vector2, Strada.Vector3, Strada.Vector4, Strada.Color or exec");
	CHECK(
		ParseError(R"({"Tracks": [{"Name": "A", "Type": "float"}, {"Name": "B", "Type": "exec"}, {"Name": "Update", "Type": "float"}]})") ==
		"Timeline.Tracks[2].Name: Update is a pin of the timeline: name the track otherwise");
	CHECK(ParseError(R"({"Tracks": [{"Name": "A", "Type": "float", "Keys": [{"Time": 0, "Value": 0}, {"Time": 0, "Value": 0,
		"Ease": 1}]}]})") == R"(Timeline.Tracks[0].Keys[1]: a key has only Time, Value, Interpolation, In and Out, not "Ease")");
	CHECK(ParseError(R"({"Tracks": [{"Name": "A", "Type": "Strada.Vector2", "Keys": [{"Time": 0, "Value": [1]}]}]})") ==
	      "Timeline.Tracks[0].Keys[0].Value: expected an array of 2 finite numbers, not [1]");
	CHECK(ParseError(R"({"Tracks": [{"Name": "Then", "Type": "float"}]})") ==
	      R"(Timeline.Tracks[0].Name "Then": Then is a reserved pin name (Execute, Then, Target, Self, Default))");
	CHECK(ParseError(R"({"Tracks": [{"Name": "A", "Type": "float"}, {"Name": "A", "Type": "exec"}]})") ==
	      "Timeline.Tracks[1].Name: a track named A comes earlier");
	CHECK(ParseError(R"({"Tracks": [{"Name": "A", "Type": "exec", "Keys": [{"Time": 0, "Value": 1}]}]})") ==
	      R"(Timeline.Tracks[0].Keys[0]: an event key has only Time, not "Value")");
	CHECK(ParseError(R"({"Length": 1, "Tracks": [{"Name": "A", "Type": "float", "Keys": [{"Time": 1.5, "Value": 0}]}]})") ==
	      "Timeline.Tracks[0].Keys[0].Time: expected a number from 0 to 1, not 1.5");
	CHECK(ParseError(R"({"Tracks": [{"Name": "A", "Type": "float", "Keys": [{"Time": 0, "Value": 0, "In": [1]}]}]})") ==
	      "Timeline.Tracks[0].Keys[0].In: expected a number, not [1]");
	CHECK(ParseError(R"({"Tracks": [{"Name": "A", "Type": "float", "Keys": [{"Time": 0, "Value": 0, "Interpolation": "Smooth"}]}]})") ==
	      R"(Timeline.Tracks[0].Keys[0].Interpolation: expected Cubic, Linear or Constant, not "Smooth")");
	CHECK(ParseError(R"({"Loop": 1})") == "Timeline.Loop: expected true or false, not 1");
	CHECK(ParseError(R"([])") == "Timeline: expected an object with Length, Loop and Tracks, not []");
	CHECK(ParseError(R"({"Speed": 1})") == R"(Timeline: a timeline has only Length, Loop and Tracks, not "Speed")");
}

TEST_CASE("VisualScriptTimeline: tracks evaluate as the runtime does (spec-slice3 §3.8.3)")
{
	auto const evaluate = [](VisualScriptTimelineTrack const& track, double time)
	{
		return EvaluateVisualScriptTimelineTrack(track, time).x;
	};
	VisualScriptTimelineTrack track = MakeTrack("A", "float", {{0.0f, 0.0f}, {1.0f, 1.0f}});
	std::array<double, 4> const times = {0.25, 0.5, 0.75, 1.0};
	std::array<float, 4> const cubic = {0.15625f, 0.5f, 0.84375f, 1.0f};
	for (size_t i = 0; i < times.size(); i++)
	{
		CHECK(evaluate(track, times[i]) == doctest::Approx(cubic[i]).epsilon(1e-6));
	}
	track.Keys[0].Interpolation = VisualScriptKeyInterpolation::Linear;
	std::array<float, 4> const linear = {0.25f, 0.5f, 0.75f, 1.0f};
	for (size_t i = 0; i < times.size(); i++)
	{
		CHECK(evaluate(track, times[i]) == doctest::Approx(linear[i]).epsilon(1e-6));
	}
	track.Keys[0].Interpolation = VisualScriptKeyInterpolation::Constant;
	std::array<float, 4> const constant = {0.0f, 0.0f, 0.0f, 1.0f};
	for (size_t i = 0; i < times.size(); i++)
	{
		CHECK(evaluate(track, times[i]) == doctest::Approx(constant[i]).epsilon(1e-6));
	}

	// Given tangents: (0, 0, Out 2), (1, 1, In 0).
	VisualScriptTimelineTrack tangents = MakeTrack("A", "float", {{0.0f, 0.0f}, {1.0f, 1.0f}});
	tangents.Keys[0].Out = glm::vec4(2.0f, 0.0f, 0.0f, 0.0f);
	tangents.Keys[1].In = glm::vec4(0.0f);
	CHECK(evaluate(tangents, 0.5) == doctest::Approx(0.75).epsilon(1e-6));

	// Auto tangents on uneven values: (0, 0), (1, 1), (2, 4).
	VisualScriptTimelineTrack const three = MakeTrack("A", "float", {{0.0f, 0.0f}, {1.0f, 1.0f}, {2.0f, 4.0f}});
	CHECK(evaluate(three, 0.5) == doctest::Approx(0.25).epsilon(1e-6));
	CHECK(evaluate(three, 1.5) == doctest::Approx(2.75).epsilon(1e-6));
	// Before the first key and after the last, their values; no key, the type's default.
	CHECK(evaluate(three, -1.0) == 0.0f);
	CHECK(evaluate(three, 3.0) == 4.0f);
	CHECK(evaluate(MakeTrack("A", "float", {}), 0.5) == 0.0f);
	CHECK(EvaluateVisualScriptTimelineTrack(MakeTrack("A", "Strada.Color", {}), 0.5) == glm::vec4(1.0f));

	// A Vector3 track per component, as the float tracks.
	VisualScriptTimelineTrack vector = MakeTrack("V", "Strada.Vector3", {{0.0f, 0.0f}, {1.0f, 0.0f}});
	vector.Keys[1].Value = glm::vec4(1.0f, 2.0f, -1.0f, 0.0f);
	glm::vec4 const value = EvaluateVisualScriptTimelineTrack(vector, 0.25);
	CHECK(value.x == doctest::Approx(0.15625).epsilon(1e-6));
	CHECK(value.y == doctest::Approx(0.3125).epsilon(1e-6));
	CHECK(value.z == doctest::Approx(-0.15625).epsilon(1e-6));
	CHECK(value.w == 0.0f);

	// Keys at one time make a jump: the left segment ends at the first, the right one starts at the last.
	VisualScriptTimelineTrack jump = MakeTrack("A", "float", {{0.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 5.0f}, {2.0f, 5.0f}});
	for (VisualScriptTimelineKey& key : jump.Keys)
	{
		key.Interpolation = VisualScriptKeyInterpolation::Linear;
	}
	CHECK(evaluate(jump, 0.5) == doctest::Approx(0.5));
	CHECK(evaluate(jump, 1.0) == 5.0f);
	CHECK(evaluate(jump, 1.5) == 5.0f);
}

TEST_CASE("VisualScriptTimeline: keys dragged as a block stop at an unselected neighbour and regain their places when dragged back")
{
	VisualScriptTimeline start;
	start.Length = 4.0f;
	start.Tracks.push_back(MakeTrack("A", "float", {{0.5f, 0.0f}, {1.0f, 1.0f}, {1.5f, 2.0f}, {2.5f, 3.0f}}));
	std::vector<VisualScriptTimelineKeyRef> const selection = {{0, 0}, {0, 1}, {0, 2}};

	// Dragged 2 s later from the start: the block stops where its last key reaches the unselected key at 2.5.
	VisualScriptTimeline dragged = start;
	RequireOk(MoveVisualScriptTimelineKeys(dragged, selection, 2.0f, glm::vec4(0.0f)));
	CHECK(GetTimes(dragged.Tracks[0]) == std::vector<float>{1.5f, 2.0f, 2.5f, 2.5f});
	CHECK(IsSorted(dragged));
	// Dragged back (the total offset from the start is 0 again): every key is where it was, spacing kept.
	VisualScriptTimeline back = start;
	RequireOk(MoveVisualScriptTimelineKeys(back, selection, 0.0f, glm::vec4(0.0f)));
	CHECK(back == start);
	// Before 0: the block stops at 0. The values move with the selected keys of curves.
	VisualScriptTimeline early = start;
	RequireOk(MoveVisualScriptTimelineKeys(early, selection, -3.0f, glm::vec4(1.0f)));
	CHECK(GetTimes(early.Tracks[0]) == std::vector<float>{0.0f, 0.5f, 1.0f, 2.5f});
	CHECK(early.Tracks[0].Keys[0].Value.x == 1.0f);
	CHECK(early.Tracks[0].Keys[3].Value.x == 3.0f);
	CHECK(IsSorted(early));
	// After the length.
	VisualScriptTimeline late = start;
	std::vector<VisualScriptTimelineKeyRef> const last = {{0, 3}};
	RequireOk(MoveVisualScriptTimelineKeys(late, last, 10.0f, glm::vec4(0.0f)));
	CHECK(late.Tracks[0].Keys[3].Time == 4.0f);

	// Refusals change nothing.
	VisualScriptTimeline refused = start;
	std::vector<VisualScriptTimelineKeyRef> const missing = {{0, 9}};
	CHECK_FALSE(MoveVisualScriptTimelineKeys(refused, missing, 0.5f, glm::vec4(0.0f)).IsOk());
	CHECK_FALSE(MoveVisualScriptTimelineKeys(refused, selection, std::numeric_limits<float>::infinity(), glm::vec4(0.0f)).IsOk());
	CHECK(refused == start);
}

TEST_CASE("VisualScriptTimeline: the length set from the interaction's start moves keys onto it and gives them back")
{
	VisualScriptTimeline start;
	start.Length = 2.0f;
	start.Tracks.push_back(MakeTrack("A", "float", {{0.0f, 0.0f}, {0.75f, 1.0f}, {1.5f, 2.0f}, {2.0f, 3.0f}}));
	start.Tracks.push_back(MakeTrack("E", "exec", {{1.0f, 0.0f}}));

	VisualScriptTimeline shorter = start;
	RequireOk(SetVisualScriptTimelineLength(shorter, 0.5f));
	CHECK(shorter.Length == 0.5f);
	CHECK(GetTimes(shorter.Tracks[0]) == std::vector<float>{0.0f, 0.5f, 0.5f, 0.5f});
	CHECK(GetTimes(shorter.Tracks[1]) == std::vector<float>{0.5f});
	CHECK(CountVisualScriptTimelineKeysAfter(start, 0.5f) == 4);
	CHECK(IsSorted(shorter));
	// Back to 2 from the start: every key where it was.
	VisualScriptTimeline restored = start;
	RequireOk(SetVisualScriptTimelineLength(restored, 2.0f));
	CHECK(restored == start);

	VisualScriptTimeline refused = start;
	CHECK_FALSE(SetVisualScriptTimelineLength(refused, 0.0f).IsOk());
	CHECK_FALSE(SetVisualScriptTimelineLength(refused, 3601.0f).IsOk());
	CHECK_FALSE(SetVisualScriptTimelineLength(refused, std::numeric_limits<float>::quiet_NaN()).IsOk());
	CHECK(refused == start);
}

TEST_CASE("VisualScriptTimeline: key and track edits keep keys sorted and within the length")
{
	VisualScriptTimeline timeline;
	timeline.Length = 2.0f;
	uint32_t const lift = RequireOk(AddVisualScriptTimelineTrack(timeline, "Lift", "float"));
	uint32_t const step = RequireOk(AddVisualScriptTimelineTrack(timeline, "Step", "exec"));
	CHECK(lift == 0);
	CHECK(step == 1);

	SUBCASE("keys are added in time order, after the keys at their time")
	{
		CHECK(RequireOk(AddVisualScriptTimelineKey(timeline, lift, 1.0f, glm::vec4(1.0f))) == 0);
		CHECK(RequireOk(AddVisualScriptTimelineKey(timeline, lift, 0.5f, glm::vec4(2.0f))) == 0);
		CHECK(RequireOk(AddVisualScriptTimelineKey(timeline, lift, 1.0f, glm::vec4(3.0f))) == 2);
		CHECK(RequireOk(AddVisualScriptTimelineKey(timeline, step, 0.25f, glm::vec4(9.0f))) == 0);
		CHECK(GetTimes(timeline.Tracks[lift]) == std::vector<float>{0.5f, 1.0f, 1.0f});
		CHECK(timeline.Tracks[lift].Keys[2].Value == glm::vec4(3.0f, 0.0f, 0.0f, 0.0f));
		// An event key has no value.
		CHECK(timeline.Tracks[step].Keys[0].Value == glm::vec4(0.0f));
		CHECK(IsSorted(timeline));

		VisualScriptTimeline const before = timeline;
		CHECK_FALSE(AddVisualScriptTimelineKey(timeline, lift, 2.5f, glm::vec4(0.0f)).IsOk());
		CHECK_FALSE(AddVisualScriptTimelineKey(timeline, lift, -0.1f, glm::vec4(0.0f)).IsOk());
		CHECK_FALSE(AddVisualScriptTimelineKey(timeline, 7, 0.0f, glm::vec4(0.0f)).IsOk());
		CHECK(timeline == before);

		std::vector<VisualScriptTimelineKeyRef> const removed = {{lift, 0}, {step, 0}};
		RequireOk(RemoveVisualScriptTimelineKeys(timeline, removed));
		CHECK(GetTimes(timeline.Tracks[lift]) == std::vector<float>{1.0f, 1.0f});
		CHECK(timeline.Tracks[step].Keys.empty());
	}

	SUBCASE("interpolation and tangents are set on curve keys only")
	{
		(void)RequireOk(AddVisualScriptTimelineKey(timeline, lift, 0.0f, glm::vec4(0.0f)));
		(void)RequireOk(AddVisualScriptTimelineKey(timeline, lift, 1.0f, glm::vec4(1.0f)));
		(void)RequireOk(AddVisualScriptTimelineKey(timeline, step, 1.0f, glm::vec4(0.0f)));
		std::vector<VisualScriptTimelineKeyRef> const both = {{lift, 0}, {lift, 1}, {step, 0}};
		RequireOk(SetVisualScriptTimelineKeyInterpolation(timeline, both, VisualScriptKeyInterpolation::Constant));
		CHECK(timeline.Tracks[lift].Keys[0].Interpolation == VisualScriptKeyInterpolation::Constant);
		CHECK(timeline.Tracks[step].Keys[0].Interpolation == VisualScriptKeyInterpolation::Cubic);
		RequireOk(FlattenVisualScriptTimelineTangents(timeline, both));
		CHECK(timeline.Tracks[lift].Keys[1].In == glm::vec4(0.0f));
		CHECK(timeline.Tracks[lift].Keys[1].Out == glm::vec4(0.0f));
		RequireOk(ResetVisualScriptTimelineTangents(timeline, both));
		CHECK_FALSE(timeline.Tracks[lift].Keys[1].In.has_value());
		RequireOk(SetVisualScriptTimelineKeyTangents(timeline, {lift, 0}, std::nullopt, glm::vec4(2.0f)));
		CHECK(timeline.Tracks[lift].Keys[0].Out == glm::vec4(2.0f));
		RequireOk(SetVisualScriptTimelineKeyValue(timeline, {lift, 1}, glm::vec4(4.0f)));
		CHECK(timeline.Tracks[lift].Keys[1].Value.x == 4.0f);

		// Only event keys: nothing to set.
		std::vector<VisualScriptTimelineKeyRef> const events = {{step, 0}};
		VisualScriptTimeline const before = timeline;
		CHECK_FALSE(SetVisualScriptTimelineKeyInterpolation(timeline, events, VisualScriptKeyInterpolation::Linear).IsOk());
		CHECK_FALSE(FlattenVisualScriptTimelineTangents(timeline, events).IsOk());
		CHECK_FALSE(SetVisualScriptTimelineKeyValue(timeline, {step, 0}, glm::vec4(1.0f)).IsOk());
		CHECK_FALSE(SetVisualScriptTimelineKeyValue(timeline, {lift, 0}, glm::vec4(std::numeric_limits<float>::infinity())).IsOk());
		CHECK(timeline == before);
	}

	SUBCASE("tracks are named by the runtime's rules, renamed, retyped and removed")
	{
		CHECK_FALSE(AddVisualScriptTimelineTrack(timeline, "Lift", "float").IsOk());
		CHECK_FALSE(AddVisualScriptTimelineTrack(timeline, "Update", "float").IsOk());
		CHECK_FALSE(AddVisualScriptTimelineTrack(timeline, "Self", "float").IsOk());
		CHECK_FALSE(AddVisualScriptTimelineTrack(timeline, "A.B", "float").IsOk());
		CHECK_FALSE(AddVisualScriptTimelineTrack(timeline, "Other", "int").IsOk());
		CHECK(CheckVisualScriptTimelineTrackName(timeline, "Lift", lift) == std::nullopt);
		CHECK(CheckVisualScriptTimelineTrackName(timeline, "Step", lift) == "the timeline has a track named Step");
		CHECK(MakeVisualScriptTimelineTrackName(timeline, "Lift") == "Lift2");
		CHECK(MakeVisualScriptTimelineTrackName(timeline, "Curve") == "Curve");

		(void)RequireOk(AddVisualScriptTimelineKey(timeline, lift, 0.5f, glm::vec4(3.0f)));
		RequireOk(RenameVisualScriptTimelineTrack(timeline, lift, "Height"));
		CHECK(timeline.Tracks[lift].Name == "Height");
		CHECK_FALSE(RenameVisualScriptTimelineTrack(timeline, lift, "Step").IsOk());

		// To a colour: the shared component kept, the others white's; to an event: the times kept.
		RequireOk(SetVisualScriptTimelineTrackType(timeline, lift, "Strada.Color"));
		CHECK(timeline.Tracks[lift].Components == 4);
		CHECK(timeline.Tracks[lift].Keys[0].Value == glm::vec4(3.0f, 1.0f, 1.0f, 1.0f));
		RequireOk(SetVisualScriptTimelineTrackType(timeline, lift, "exec"));
		CHECK(timeline.Tracks[lift].Components == 0);
		CHECK(GetTimes(timeline.Tracks[lift]) == std::vector<float>{0.5f});
		CHECK(timeline.Tracks[lift].Keys[0].Value == glm::vec4(0.0f));
		RequireOk(SetVisualScriptTimelineTrackType(timeline, lift, "Strada.Vector2"));
		CHECK(timeline.Tracks[lift].Keys[0].Value == glm::vec4(0.0f));
		RequireOk(CheckVisualScriptTimeline(timeline));

		RequireOk(RemoveVisualScriptTimelineTrack(timeline, lift));
		REQUIRE(timeline.Tracks.size() == 1);
		CHECK(timeline.Tracks[0].Name == "Step");
		CHECK_FALSE(RemoveVisualScriptTimelineTrack(timeline, 3).IsOk());
	}
}

TEST_CASE("VisualScriptTimeline: SetTimeline keeps the links of kept and renamed tracks and removes those of removed and retyped ones")
{
	Json const setting = MakeExampleSetting();
	VisualScriptDocument document;
	VisualScriptGraph& graph = document.Graph;
	graph.Nodes = {MakeNode(1, "Flow.Timeline", {{"Timeline", setting}}), MakeNode(2, "Call:Strada.Log.Info(string)"),
	               MakeNode(3, "Call:Strada.Log.Info(string)"), MakeNode(4, "Call:Strada.Log.Info(string)")};
	graph.Nodes[0].Split.Outputs = {"Tint"};
	graph.Links = {MakeLink(1, "Lift", 2, "Message"), MakeLink(1, "Tint.R", 3, "Message"), MakeLink(1, "Step", 4, "Execute"),
	               MakeLink(1, "Update", 2, "Execute")};
	VisualScriptTimeline timeline = RequireOk(ParseVisualScriptTimeline(setting));

	SUBCASE("a renamed track's links and split path follow it")
	{
		RequireOk(RenameVisualScriptTimelineTrack(timeline, 1, "Color"));
		std::array<VisualScriptEditing::VisualScriptTrackRename, 1> const renames = {{{"Tint", "Color"}}};
		CHECK(CountVisualScriptTrackLinks(document, 1, "Tint") == 1);
		CHECK(RequireOk(VisualScriptEditing::SetTimeline(document, 1, timeline, renames)) == 0);
		CHECK(graph.Links[1] == MakeLink(1, "Color.R", 3, "Message"));
		CHECK(graph.Nodes[0].Split.Outputs == std::vector<std::string>{"Color"});
		CHECK(graph.Nodes[0].Settings["Timeline"] == WriteVisualScriptTimeline(timeline));
		Testing::ExpectStrictRoundTrip(document);
		Testing::ExpectSplitConsistency(document);
	}

	SUBCASE("removed tracks and tracks retyped take their links with them")
	{
		RequireOk(SetVisualScriptTimelineTrackType(timeline, 0, "Strada.Vector2"));
		RequireOk(RemoveVisualScriptTimelineTrack(timeline, 2));
		CHECK(RequireOk(VisualScriptEditing::SetTimeline(document, 1, timeline, {})) == 2);
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(1, "Tint.R", 3, "Message"), MakeLink(1, "Update", 2, "Execute")});
		Testing::ExpectStrictRoundTrip(document);
	}

	SUBCASE("a track renamed to the name of a track that goes takes nothing of it")
	{
		// Lift and Step swap names: Lift's link goes with it under Step, Step's (an event's) would change kinds and goes.
		timeline.Tracks[0].Name = "Step";
		timeline.Tracks[2].Name = "Lift";
		std::array<VisualScriptEditing::VisualScriptTrackRename, 2> const renames = {{{"Lift", "Step"}, {"Step", "Lift"}}};
		CHECK(RequireOk(VisualScriptEditing::SetTimeline(document, 1, timeline, renames)) == 0);
		CHECK(graph.Links[0] == MakeLink(1, "Step", 2, "Message"));
		CHECK(graph.Links[2] == MakeLink(1, "Lift", 4, "Execute"));
	}

	SUBCASE("refusals change nothing")
	{
		VisualScriptDocument const before = document;
		CHECK_FALSE(VisualScriptEditing::SetTimeline(document, 2, timeline, {}).IsOk());
		CHECK_FALSE(VisualScriptEditing::SetTimeline(document, 9, timeline, {}).IsOk());
		std::array<VisualScriptEditing::VisualScriptTrackRename, 1> const unknown = {{{"Nope", "Lift"}}};
		CHECK_FALSE(VisualScriptEditing::SetTimeline(document, 1, timeline, unknown).IsOk());
		std::array<VisualScriptEditing::VisualScriptTrackRename, 1> const missing = {{{"Lift", "Height"}}};
		CHECK_FALSE(VisualScriptEditing::SetTimeline(document, 1, timeline, missing).IsOk());
		VisualScriptTimeline damaged = timeline;
		damaged.Tracks[0].Keys[0].Time = 5.0f;
		CHECK_FALSE(VisualScriptEditing::SetTimeline(document, 1, damaged, {}).IsOk());
		CHECK(document == before);
		// SetSetting names the operation.
		Result<void> const set = VisualScriptEditing::SetSetting(document, 1, "Timeline", Json::object());
		REQUIRE_FALSE(set.IsOk());
		CHECK(set.GetError() == "Timeline of node 1 (Flow.Timeline) changes its pins or its senders: set it with SetTimeline");
	}
}

TEST_CASE("VisualScriptTimeline: track edits are described as the drawer and Details commit them")
{
	Json const setting = MakeExampleSetting();
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(1, "Flow.Timeline", {{"Timeline", setting}}), MakeNode(2, "Call:Strada.Log.Info(string)"),
	                        MakeNode(3, "Call:Strada.Log.Info(string)")};
	document.Graph.Links = {MakeLink(1, "Lift", 2, "Message"), MakeLink(1, "Lift", 3, "Message")};
	VisualScriptTimeline const timeline = RequireOk(ParseVisualScriptTimeline(setting));

	CHECK(RequireOk(AddVisualScriptTimelineTrackEdit(document, 1, timeline, "float", "Timeline")).Action == "Add Track Curve to Timeline");
	CHECK(RequireOk(AddVisualScriptTimelineTrackEdit(document, 1, timeline, "exec", "Timeline")).Action == "Add Track Event to Timeline");
	CHECK(RequireOk(RenameVisualScriptTimelineTrackEdit(document, 1, timeline, 0, "Height", "Timeline")).Action ==
	      "Rename Track Lift to Height on Timeline (and 2 links)");
	CHECK(RequireOk(RenameVisualScriptTimelineTrackEdit(document, 1, timeline, 2, "Beat", "Timeline")).Action ==
	      "Rename Track Step to Beat on Timeline");
	CHECK(RequireOk(RemoveVisualScriptTimelineTrackEdit(document, 1, timeline, 0, "Timeline")).Action ==
	      "Remove Track Lift from Timeline (removes 2 links)");
	CHECK(RequireOk(ChangeVisualScriptTimelineTrackTypeEdit(document, 1, timeline, 0, "Strada.Vector3", "Timeline")).Action ==
	      "Change Lift to Vector3 on Timeline (removes 2 links)");
	CHECK(RequireOk(SetVisualScriptTimelineLengthEdit(document, 1, timeline, 0.5f, "Timeline")).Action ==
	      "Set Length on Timeline (moves 4 keys)");
	CHECK(RequireOk(SetVisualScriptTimelineLengthEdit(document, 1, timeline, 3.0f, "Timeline")).Action == "Set Length on Timeline");
	CHECK(RequireOk(SetVisualScriptTimelineLoopEdit(document, 1, timeline, true, "Timeline")).Action == "Loop Timeline");
	CHECK(RequireOk(SetVisualScriptTimelineLoopEdit(document, 1, timeline, false, "Timeline")).Action == "Stop Looping Timeline");
	// A rename keeps the links, now from the new name.
	VisualScriptTimelineEdit const renamed = RequireOk(RenameVisualScriptTimelineTrackEdit(document, 1, timeline, 0, "Height", "Timeline"));
	CHECK(renamed.Document.Graph.Links[0] == MakeLink(1, "Height", 2, "Message"));
	CHECK_FALSE(RenameVisualScriptTimelineTrackEdit(document, 1, timeline, 0, "Step", "Timeline").IsOk());
}

TEST_CASE("VisualScriptTimeline: damaged Timeline settings are refused or written canonically")
{
	std::array<Json, 2> const seeds = {MakeExampleSetting(), LoadLanguageTimeline()};
	Testing::DocumentMutator mutator(10900);
	int const rounds = Testing::GetFuzzRounds(300);
	int parsed = 0;
	for (int round = 0; round < rounds; round++)
	{
		Json const& seed = seeds[static_cast<size_t>(round) % seeds.size()];
		Json const damaged = mutator.Damage(seed, round);
		CAPTURE(damaged.dump());
		Result<VisualScriptTimeline> const read = ParseVisualScriptTimeline(damaged);
		if (!read)
		{
			// A refusal names a path into the setting and says what is wrong.
			std::string const& error = read.GetError();
			CHECK(error.starts_with("Timeline"));
			CHECK(error.find(": ") != std::string::npos);
			continue;
		}
		parsed++;
		Json const written = WriteVisualScriptTimeline(read.GetValue());
		Result<VisualScriptTimeline> const again = ParseVisualScriptTimeline(written);
		REQUIRE_MESSAGE(again.IsOk(), again.GetError());
		CHECK(again.GetValue() == read.GetValue());
		CHECK(WriteVisualScriptTimeline(again.GetValue()) == written);
		CHECK(CheckVisualScriptTimeline(read.GetValue()).IsOk());
	}
	CHECK(parsed > 0);
}
