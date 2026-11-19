#include "Core/TelemetryTestUtilities.h"
#include "HeadlessImGui.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/Automation/TelemetryCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/EditorTelemetry.h"
#include "Editor/Panels/TelemetryPanel.h"
#include "Editor/PlayMode.h"
#include "Editor/TelemetryPlot.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Telemetry.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;
using Testing::MakeTelemetryStep;
using Testing::TelemetryScope;

namespace
{
	constexpr float StepSeconds = 1.0f / 60.0f;

	// The panel in a window of its own at a fixed place and size.
	struct PanelFixture
	{
		Testing::HeadlessImGui ImGuiContext;
		EditorTelemetry Owner;
		TelemetryPanel Panel;
		Scene* ShownScene = nullptr;
		bool Open = true;

		void DrawPanel()
		{
			ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
			ImGui::SetNextWindowSize(ImVec2(1400.0f, 800.0f));
			Panel.OnImGuiRender(Owner, Open, ShownScene, {});
		}

		void Draw()
		{
			ImGuiContext.Frame(
				[this]
				{
					DrawPanel();
				});
		}

		// Clicks there, then draws once more: a check box changes the lanes from the draw after the one it was clicked in.
		void Click(std::array<float, 2> position)
		{
			ImGuiContext.Click(ImVec2(position[0], position[1]),
			                   [this]
			                   {
								   DrawPanel();
							   });
			Draw();
		}

		TelemetryPanel::DrawRecord const& Record() const { return Panel.GetLastDraw(); }

		std::vector<std::string> LaneChannels() const
		{
			std::vector<std::string> channels;
			for (TelemetryPanel::DrawRecord::Lane const& lane : Record().Lanes)
			{
				channels.push_back(lane.Channel);
			}
			return channels;
		}

		TelemetryPanel::DrawRecord::Lane const& Lane(std::string_view channel) const
		{
			auto const found = std::find_if(Record().Lanes.begin(), Record().Lanes.end(),
			                                [channel](TelemetryPanel::DrawRecord::Lane const& lane)
			                                {
												return lane.Channel == channel;
											});
			REQUIRE(found != Record().Lanes.end());
			return *found;
		}
	};

	// What drawing must leave as it was: every session's rows, channels, marks, bytes and drops, and the recorder's status.
	std::string DescribeRecorder()
	{
		TelemetryStatus const status = Telemetry::GetStatus();
		std::string text =
			fmt::format("recording {} sessions {} bytes {} live {} dropped {} {} {}\n", status.Recording, status.SessionsHeld,
		                status.BytesHeld, status.LiveSession, status.DroppedValues, status.DroppedMarks, status.DroppedSteps);
		for (TelemetrySession const& session : Telemetry::GetSessions())
		{
			text += fmt::format("session {} source {} first {} rows {} live {} bytes {} marks {}\n", session.Id, session.Source,
			                    session.FirstStep, session.Rows, session.Live, session.Bytes, session.Marks.size());
			for (TelemetryChannel const& channel : session.Channels)
			{
				text += fmt::format(" {} {} {} {} {} {} {}\n", channel.Name, channel.FirstRow, channel.Cells, channel.Count,
				                    channel.Minimum, channel.Maximum, channel.Last);
			}
		}
		return text;
	}

	// A race of source 1: two steps before the line, then Lap 1 (steps 3 to 8, six rows), Lap 2 (steps 9 to 12, four
	// rows: the best) and Lap 3 (steps 13 to 20, eight rows), finished at step 21 (Lap 4, then Lap end). Speed is the step's
	// number, Gear is recorded on even steps, the bike's engine speed on the first step, and Ride.Distance runs 0 to 100 m
	// along each lap: Lap 1's last row has already wrapped (5 m after 80 m), and Lap 3's first row still carries the
	// distance the run before ended at (100 m before 0 m).
	void RecordRace(UUID bike)
	{
		constexpr std::array<float, 21> Distances = {50.0f,  75.0f,                                            // before the line
		                                             0.0f,   20.0f, 40.0f, 60.0f,  80.0f, 5.0f,                // Lap 1
		                                             25.0f,  50.0f, 75.0f, 100.0f,                             // Lap 2
		                                             100.0f, 0.0f,  15.0f, 30.0f,  45.0f, 60.0f, 75.0f, 90.0f, // Lap 3
		                                             0.0f};                                                    // the finish
		std::string const rpm = "Vehicle." + std::to_string(bike.GetValue()) + ".Rpm";
		for (uint64_t step = 1; step <= Distances.size(); ++step)
		{
			Telemetry::Record("Speed", static_cast<float>(step));
			Telemetry::Record("Ride.Distance", Distances[step - 1]);
			if (step % 2 == 0)
			{
				Telemetry::Record("Gear", static_cast<float>(step / 2));
			}
			if (step == 1)
			{
				Telemetry::Record(rpm, 9000.0f);
			}
			switch (step)
			{
				case 3:
					Telemetry::Mark("Lap 1");
					break;
				case 9:
					Telemetry::Mark("Lap 2");
					break;
				case 13:
					Telemetry::Mark("Lap 3");
					break;
				case 21:
					Telemetry::Mark("Lap 4");
					Telemetry::Mark(TelemetryLapEndLabel);
					break;
				default:
					break;
			}
			Telemetry::EndStep(MakeTelemetryStep(step, 1, StepSeconds));
		}
	}

	// Returns nothing: GCC refuses a reference bound to the result of a call given temporaries (-Wdangling-reference).
	void RecordSession(std::vector<float> const& values, std::vector<float> const& distances = {})
	{
		for (size_t row = 0; row < values.size(); ++row)
		{
			if (!std::isnan(values[row]))
			{
				Telemetry::Record("Value", values[row]);
			}
			if (row < distances.size())
			{
				Telemetry::Record("Ride.Distance", distances[row]);
			}
			Telemetry::EndStep(MakeTelemetryStep(row + 1));
		}
	}

	TelemetrySeries Reduce(TelemetrySession const& session, TelemetryAxis axis, uint32_t columns)
	{
		TelemetrySeriesRequest request;
		request.Session = &session;
		request.Span = GetTelemetrySessionSpan(session);
		request.Channel = FindTelemetryChannel(session, "Value");
		request.Axis = axis;
		request.Distance = FindTelemetryChannel(session, "Ride.Distance");
		request.Rows = PlaceTelemetryRows(session, request.Span, axis, request.Distance);
		request.Start = request.Rows.Start;
		request.End = request.Rows.End;
		request.Columns = columns;
		return ReduceTelemetrySeries(request);
	}
}

TEST_CASE("TelemetryPanel: engine channels are named by their entities, and a channel is reduced to two points a pixel column")
{
	// The entity of an engine channel, and how the panel names it.
	Scene scene("Main");
	Entity const bike = scene.CreateEntity("Bike");
	std::string const id = std::to_string(bike.GetUUID().GetValue());
	CHECK(ParseTelemetryChannelEntity("Vehicle." + id + ".Rpm") == bike.GetUUID());
	for (char const* const refused : {"Speed", "Ride.Distance", "Vehicle..Rpm", ".1.Rpm", "Vehicle.1.", "Vehicle.0.Rpm", "Vehicle.12a.Rpm",
	                                  "Vehicle.123456789012345678901.Rpm"})
	{
		CAPTURE(refused);
		CHECK_FALSE(ParseTelemetryChannelEntity(refused));
	}
	CHECK(DescribeTelemetryChannel("Vehicle." + id + ".Rpm", &scene) == "Vehicle.Bike.Rpm");
	CHECK(DescribeTelemetryChannel("Vehicle." + id + ".Rpm", nullptr) == "Vehicle." + id + ".Rpm");
	CHECK(DescribeTelemetryChannel("Vehicle.42.Rpm", &scene) == "Vehicle.42.Rpm");
	CHECK(DescribeTelemetryChannel("Ride.Distance", &scene) == "Ride.Distance");

	TelemetryScope const scope;
	REQUIRE(Telemetry::Start().IsOk());
	SUBCASE("by time, each column keeps its smallest and largest value in the order they came")
	{
		// 100 rows of a saw tooth (0 to 9 in each tenth) in 10 columns: each column holds one tooth.
		std::vector<float> values;
		for (int row = 0; row < 100; ++row)
		{
			values.push_back(static_cast<float>(row % 10));
		}
		RecordSession(values);
		TelemetrySession const& session = Testing::GetNewestTelemetrySession();
		TelemetrySeries const series = Reduce(session, TelemetryAxis::Time, 10);
		CHECK(series.Rows == 100);
		CHECK(series.Minimum == 0.0f);
		CHECK(series.Maximum == 9.0f);
		CHECK(series.Segments == std::vector<uint32_t>{0});
		REQUIRE(series.Points.size() == 20);
		for (size_t column = 0; column < 10; ++column)
		{
			CAPTURE(column);
			CHECK(series.Points[column * 2] == glm::vec2(static_cast<float>(column) + 0.5f, 0.0f));
			CHECK(series.Points[column * 2 + 1] == glm::vec2(static_cast<float>(column) + 0.5f, 9.0f));
		}
		// A lane wider than the rows gives a point a row.
		TelemetrySeries const wide = Reduce(session, TelemetryAxis::Time, 1000);
		CHECK(wide.Points.size() == 100);
	}
	SUBCASE("rows without a value are spanned by the line, and a wrapped distance begins a segment")
	{
		float const none = std::nanf("");
		RecordSession({1.0f, none, 3.0f, none, 5.0f, 6.0f}, {0.0f, 10.0f, 20.0f, 30.0f, 0.0f, 10.0f});
		TelemetrySession const& session = Testing::GetNewestTelemetrySession();
		TelemetrySeries const byTime = Reduce(session, TelemetryAxis::Time, 100);
		CHECK(byTime.Rows == 4);
		CHECK(byTime.Segments == std::vector<uint32_t>{0});
		CHECK(byTime.Points.size() == 4);
		// By distance the whole session is placed: its run goes back to 0 m at the fifth row, which begins a segment.
		TelemetrySpan const span = GetTelemetrySessionSpan(session);
		TelemetryAxisRows const rows =
			PlaceTelemetryRows(session, span, TelemetryAxis::Distance, FindTelemetryChannel(session, "Ride.Distance"));
		CHECK(rows.FirstRow == 0);
		CHECK(rows.EndRow == 6);
		CHECK(rows.Start == 0.0);
		CHECK(rows.End == 30.0);
		TelemetrySeries const byDistance = Reduce(session, TelemetryAxis::Distance, 30);
		CHECK(byDistance.Segments == std::vector<uint32_t>{0, 2});
		CHECK(byDistance.Points.size() == 4);
		CHECK(byDistance.Points[2] == glm::vec2(0.5f, 5.0f));
		// Nothing to plot against: no rows placed.
		CHECK(PlaceTelemetryRows(session, span, TelemetryAxis::Distance, nullptr).IsEmpty());
		CHECK(ReduceTelemetrySeries({}).Points.empty());
	}
	Telemetry::Stop();
}

TEST_CASE("TelemetryPanel: laps by distance leave out the rows outside their own run, and the overlay defaults to the best other lap")
{
	TelemetryScope const scope;
	REQUIRE(Telemetry::Start().IsOk());
	RecordRace(UUID(7));
	Telemetry::EndSession();
	TelemetrySession const& race = Testing::GetNewestTelemetrySession();
	std::vector<TelemetrySpan> const laps = ListTelemetryLapSpans(race);
	REQUIRE(laps.size() == 3);
	CHECK(laps[0].Label == "Lap 1");
	CHECK(laps[0].FirstRow == 2);
	CHECK(laps[0].EndRow == 8);
	CHECK(DescribeTelemetrySpan(laps[1]) == fmt::format("Lap 2 ({:.3f} s)", 4.0 * static_cast<double>(StepSeconds)));
	TelemetryChannel const* const distance = FindTelemetryChannel(race, "Ride.Distance");
	REQUIRE(distance != nullptr);

	// Lap 1's last row has wrapped, Lap 3's first carries the run before: both are left out, Lap 2 keeps every row.
	TelemetryAxisRows const first = PlaceTelemetryRows(race, laps[0], TelemetryAxis::Distance, distance);
	CHECK(first.FirstRow == 2);
	CHECK(first.EndRow == 7);
	CHECK(first.End == 80.0);
	TelemetryAxisRows const second = PlaceTelemetryRows(race, laps[1], TelemetryAxis::Distance, distance);
	CHECK(second.FirstRow == 8);
	CHECK(second.EndRow == 12);
	CHECK(second.Start == 25.0);
	CHECK(second.End == 100.0);
	TelemetryAxisRows const third = PlaceTelemetryRows(race, laps[2], TelemetryAxis::Distance, distance);
	CHECK(third.FirstRow == 13);
	CHECK(third.EndRow == 20);
	CHECK(third.Start == 0.0);
	CHECK(third.End == 90.0);
	// By time every row of a lap, from 0 s at its first.
	TelemetryAxisRows const timed = PlaceTelemetryRows(race, laps[2], TelemetryAxis::Time, nullptr);
	CHECK(timed.FirstRow == 12);
	CHECK(timed.EndRow == 20);
	CHECK(timed.End == doctest::Approx(7.0 * static_cast<double>(StepSeconds)));
	CHECK(GetTelemetryRowPosition(race, laps[2], TelemetryAxis::Time, nullptr, 12) == 0.0);

	// The session is over: its last lap shows, over the best of the others.
	std::optional<TelemetrySpan> const shown = FindDefaultTelemetryLap(race);
	REQUIRE(shown);
	CHECK(shown->Label == "Lap 3");
	std::optional<TelemetrySpan> const reference = FindDefaultTelemetryReference(Telemetry::GetSessions(), race, *shown);
	REQUIRE(reference);
	CHECK(reference->Label == "Lap 2");
	// Shown, the best lap is overlaid by the best of the others.
	std::optional<TelemetrySpan> const overBest = FindDefaultTelemetryReference(Telemetry::GetSessions(), race, laps[1]);
	REQUIRE(overBest);
	CHECK(overBest->Label == "Lap 1");

	// A second play with its first lap in progress shows it, over the best lap of the race before; its whole session is its
	// last 30 seconds while it is live.
	uint32_t const raceId = race.Id;
	for (uint64_t step = 1; step <= 3; ++step)
	{
		if (step == 2)
		{
			Telemetry::Mark("Lap 1");
		}
		Telemetry::EndStep(MakeTelemetryStep(step, 2, StepSeconds));
	}
	TelemetrySession const& next = Testing::GetNewestTelemetrySession();
	REQUIRE(next.Live);
	std::optional<TelemetrySpan> const progress = FindDefaultTelemetryLap(next);
	REQUIRE(progress);
	CHECK(progress->InProgress);
	CHECK(progress->FirstRow == 1);
	CHECK(progress->EndRow == 3);
	std::optional<TelemetrySpan> const earlier = FindDefaultTelemetryReference(Telemetry::GetSessions(), next, *progress);
	REQUIRE(earlier);
	CHECK(earlier->Session == raceId);
	CHECK(earlier->Label == "Lap 2");
	CHECK(FindTelemetryLapSpan(Telemetry::GetSessions(), {raceId, 8})->Label == "Lap 2");
	CHECK_FALSE(FindTelemetryLapSpan(Telemetry::GetSessions(), {raceId, 9}));
	CHECK(GetTelemetrySessionSpan(next).FirstRow == 0);
	for (uint64_t step = 4; step <= 2000; ++step)
	{
		Telemetry::EndStep(MakeTelemetryStep(step, 2, StepSeconds));
	}
	TelemetrySpan const window = GetTelemetrySessionSpan(Testing::GetNewestTelemetrySession());
	CHECK(window.EndRow == 2000);
	CHECK(window.FirstRow == 2000 - 1800);
	Telemetry::Stop();
	CHECK(GetTelemetrySessionSpan(Testing::GetNewestTelemetrySession()).FirstRow == 0);
}

TEST_CASE("TelemetryPanel: a session's channels are listed and plotted in lanes, chosen in the list, and drawing changes nothing")
{
	PanelFixture fixture;
	// Without the recorder the panel says why it does not record.
	fixture.Owner.Update(true);
	fixture.Draw();
	CHECK(fixture.Record().Drawn);
	CHECK(fixture.Record().Status.starts_with("Not recording"));
	CHECK(fixture.Record().Session == 0);
	fixture.Owner.Update(false);

	TelemetryScope const scope;
	Scene scene("Main");
	Entity const bike = scene.CreateEntity("Bike");
	fixture.ShownScene = &scene;
	// The open panel records.
	fixture.Owner.Update(fixture.Open);
	REQUIRE(fixture.Owner.IsPanelRecording());
	RecordRace(bike.GetUUID());
	Telemetry::EndSession();
	std::string const before = DescribeRecorder();

	fixture.Draw();
	TelemetryPanel::DrawRecord const& record = fixture.Record();
	CHECK(record.Recording);
	CHECK(record.Status.starts_with("Recording: 1 sessions held"));
	CHECK(record.Session == 1);
	CHECK_FALSE(record.Live);
	CHECK(record.Rows == 21);
	// The channels in the order they came, the bike's under its name.
	std::string const rpm = "Vehicle." + std::to_string(bike.GetUUID().GetValue()) + ".Rpm";
	REQUIRE(record.Channels.size() == 4);
	CHECK(record.Channels[0].Name == "Speed");
	CHECK(record.Channels[1].Name == "Ride.Distance");
	CHECK(record.Channels[2].Name == rpm);
	CHECK(record.Channels[2].Label == "Vehicle.Bike.Rpm");
	CHECK(record.Channels[2].Last == 9000.0f);
	CHECK(record.Channels[3].Name == "Gear");
	// Against Ride.Distance by default, which is no lane of its own then.
	CHECK(record.Axis == TelemetryAxis::Distance);
	CHECK(record.DistanceChannel == "Ride.Distance");
	CHECK_FALSE(record.Channels[1].Chosen);
	CHECK(fixture.LaneChannels() == std::vector<std::string>{"Speed", rpm, "Gear"});
	CHECK(fixture.Lane(rpm).Label == "Vehicle.Bike.Rpm");
	CHECK(record.Ticks > 0);
	// The bike's engine speed was recorded before the line: no row of either lap has it.
	CHECK(fixture.Lane(rpm).Shown.Rows == 0);
	CHECK(fixture.Lane(rpm).Shown.Points == 0);
	// Each lane spans its values: Speed is the step's number, Lap 3's placed rows 14 to 20 over Lap 2's 9 to 12.
	TelemetryPanel::DrawRecord::Lane const& speed = fixture.Lane("Speed");
	CHECK(speed.Minimum == 9.0f);
	CHECK(speed.Maximum == 20.0f);
	CHECK(speed.Shown.Rows == 7);
	REQUIRE(speed.Reference);
	CHECK(speed.Reference->Rows == 4);
	CHECK(speed.Bottom > speed.Top);
	CHECK(fixture.Lane("Gear").Top >= speed.Bottom);
	// The lanes are as wide as the panel's plot, and their lines lie within them.
	REQUIRE(speed.Shown.First);
	CHECK((*speed.Shown.First)[0] >= record.LanesLeft);
	CHECK((*speed.Shown.Last)[0] <= record.LanesRight);
	CHECK((*speed.Shown.First)[1] <= speed.Bottom);
	CHECK((*speed.Shown.First)[1] >= speed.Top);
	CHECK(DescribeRecorder() == before);

	// Unchecking a channel takes its lane away; checking one adds it at the end.
	std::array<float, 2> const gear = record.Channels[3].Center;
	fixture.Click(gear);
	CHECK(fixture.LaneChannels() == std::vector<std::string>{"Speed", rpm});
	CHECK_FALSE(fixture.Record().Channels[3].Chosen);
	fixture.Click(fixture.Record().Channels[1].Center);
	CHECK(fixture.LaneChannels() == std::vector<std::string>{"Speed", rpm, "Ride.Distance"});
	// Channels chosen through the panel's own choice; unchecking every one plots none.
	fixture.Panel.ChooseChannels({"Gear", "Missing", "Speed"});
	fixture.Draw();
	CHECK(fixture.LaneChannels() == std::vector<std::string>{"Gear", "Speed"});
	fixture.Click(fixture.Record().Channels[0].Center);
	fixture.Click(fixture.Record().Channels[3].Center);
	CHECK(fixture.Record().Lanes.empty());
	fixture.Panel.ChooseChannels({});
	fixture.Draw();
	CHECK(fixture.LaneChannels() == std::vector<std::string>{"Speed", rpm, "Gear"});
	CHECK(DescribeRecorder() == before);
	CHECK(scene.GetEntityByUUID(bike.GetUUID()).GetName() == "Bike");

	// Pause stops the recording once the frame is drawn; the sessions stay, and Record lets the panel record again.
	std::array<float, 2> const toggle = fixture.Record().ToggleCenter;
	fixture.Click(toggle);
	CHECK_FALSE(Telemetry::IsRecording());
	CHECK(fixture.Owner.IsHeld());
	fixture.Owner.Update(fixture.Open);
	fixture.Draw();
	CHECK(fixture.Record().Status == "Paused: 1 sessions held");
	CHECK(fixture.Record().Session == 1);
	fixture.Click(toggle);
	CHECK_FALSE(fixture.Owner.IsHeld());
	fixture.Owner.Update(fixture.Open);
	CHECK(fixture.Owner.IsPanelRecording());
	// Closing the window closes the panel, and its recording stops.
	fixture.Open = false;
	fixture.Owner.Update(fixture.Open);
	CHECK_FALSE(Telemetry::IsRecording());
}

TEST_CASE("TelemetryPanel: a lap is overlaid on another by distance and by time, the best other lap by default")
{
	PanelFixture fixture;
	TelemetryScope const scope;
	fixture.Owner.Update(fixture.Open);
	RecordRace(UUID(7));
	Telemetry::EndSession();

	// By distance: Lap 3's rows from 0 m to 90 m over Lap 2's from 25 m to 100 m.
	fixture.Draw();
	TelemetryPanel::DrawRecord const& record = fixture.Record();
	REQUIRE(record.Shown);
	CHECK(record.Shown->Label == "Lap 3");
	CHECK(record.Shown->FirstRow == 12);
	CHECK(record.Shown->EndRow == 20);
	CHECK(record.Shown->PlacedFirstRow == 13);
	CHECK(record.Shown->PlacedEndRow == 20);
	REQUIRE(record.Reference);
	CHECK(record.Reference->Label == "Lap 2");
	CHECK(record.Reference->Session == record.Session);
	CHECK(record.AxisStart == 0.0);
	CHECK(record.AxisEnd == 100.0);
	TelemetryPanel::DrawRecord::Lane const& speed = fixture.Lane("Speed");
	// Lap 3 reaches 90 m of 100: its line ends short of the right edge; Lap 2 starts a quarter in and runs to it.
	float const width = record.LanesRight - record.LanesLeft;
	REQUIRE(speed.Shown.First);
	REQUIRE(speed.Reference);
	REQUIRE(speed.Reference->First);
	CHECK((*speed.Shown.First)[0] == doctest::Approx(record.LanesLeft + 0.5f).epsilon(0.001));
	CHECK((*speed.Shown.Last)[0] == doctest::Approx(record.LanesLeft + width * 0.9f).epsilon(0.002));
	CHECK((*speed.Reference->First)[0] == doctest::Approx(record.LanesLeft + width * 0.25f).epsilon(0.002));
	CHECK((*speed.Reference->Last)[0] == doctest::Approx(record.LanesRight - 0.5f).epsilon(0.001));
	// A larger value reads higher in the lane: the shown lap's last Speed (20) above the overlay's last (12).
	CHECK((*speed.Shown.Last)[1] < (*speed.Reference->Last)[1]);

	// By time both start at 0 s: Lap 3's eight rows over Lap 2's four.
	fixture.Panel.PlotAgainstTime();
	fixture.Draw();
	CHECK(record.Axis == TelemetryAxis::Time);
	CHECK(record.AxisStart == 0.0);
	CHECK(record.AxisEnd == doctest::Approx(7.0 * static_cast<double>(StepSeconds)));
	CHECK(record.Shown->PlacedFirstRow == 12);
	TelemetryPanel::DrawRecord::Lane const& timed = fixture.Lane("Speed");
	CHECK(timed.Shown.Rows == 8);
	CHECK(timed.Reference->Rows == 4);
	CHECK((*timed.Shown.First)[0] == doctest::Approx((*timed.Reference->First)[0]));
	CHECK((*timed.Reference->Last)[0] == doctest::Approx(record.LanesLeft + width * 3.0f / 7.0f).epsilon(0.002));
	// Ride.Distance is a lane of its own when time is the axis.
	CHECK(fixture.LaneChannels() == std::vector<std::string>{"Speed", "Ride.Distance", "Vehicle.7.Rpm", "Gear"});

	// Another lap shown, another overlaid, or none; the whole session.
	fixture.Panel.ShowLap(2);
	fixture.Panel.OverlayLap({record.Session, 12});
	fixture.Draw();
	CHECK(record.Shown->Label == "Lap 1");
	CHECK(record.Reference->Label == "Lap 3");
	fixture.Panel.OverlayNone();
	fixture.Draw();
	CHECK_FALSE(record.Reference);
	CHECK_FALSE(fixture.Lane("Speed").Reference);
	fixture.Panel.ShowSession();
	fixture.Draw();
	CHECK(record.Shown->Label.empty());
	CHECK(record.Shown->FirstRow == 0);
	CHECK(record.Shown->EndRow == 21);
	CHECK(fixture.Lane("Speed").Shown.Rows == 21);
	// A lap the session does not have, and one no session holds, give the defaults again.
	fixture.Panel.ShowLap(5);
	fixture.Panel.OverlayLap({99, 8});
	fixture.Draw();
	CHECK(record.Shown->Label == "Lap 3");
	CHECK(record.Reference->Label == "Lap 2");

	// A second play's lap in progress shows live, over the best lap of the race before, and follows its rows.
	uint32_t const race = record.Session;
	fixture.Panel.PlotAgainstDefault();
	for (uint64_t step = 1; step <= 4; ++step)
	{
		Telemetry::Record("Speed", 30.0f);
		Telemetry::Record("Ride.Distance", static_cast<float>(step) * 10.0f);
		if (step == 1)
		{
			Telemetry::Mark("Lap 1");
		}
		Telemetry::EndStep(MakeTelemetryStep(step, 2, StepSeconds));
	}
	fixture.Draw();
	CHECK(record.Session == race + 1);
	CHECK(record.Live);
	CHECK(record.Shown->InProgress);
	CHECK(record.Reference->Session == race);
	CHECK(record.Reference->Label == "Lap 2");
	CHECK(fixture.Lane("Speed").Shown.Rows == 4);
	Telemetry::Record("Speed", 31.0f);
	Telemetry::Record("Ride.Distance", 50.0f);
	Telemetry::EndStep(MakeTelemetryStep(5, 2, StepSeconds));
	fixture.Draw();
	CHECK(fixture.Lane("Speed").Shown.Rows == 5);
	CHECK(fixture.Lane("Speed").Maximum == 31.0f);
	// The race chosen is shown, not followed past.
	fixture.Panel.SelectSession(race);
	fixture.Draw();
	CHECK(record.Session == race);
	fixture.Panel.SelectSession(std::nullopt);
	fixture.Draw();
	CHECK(record.Session == race + 1);
	fixture.Open = false;
	fixture.Owner.Update(fixture.Open);
}

TEST_CASE("TelemetryPanel: telemetry.start's panel opens the panel, and viewport.screenshot names its window")
{
	for (std::string_view const name : {"Editor", "SceneView", "GameView", "Hierarchy", "Inspector", "ContentBrowser", "Console",
	                                    "Debugger", "Graph", "Profiler", "InputActions", "Mixer", "Telemetry"})
	{
		CAPTURE(name);
		std::optional<ScreenshotWindow> const window = ParseScreenshotWindow(name);
		REQUIRE(window);
		CHECK(GetScreenshotWindowName(*window) == name);
	}
	CHECK(ParseScreenshotWindow("Telemetry") == ScreenshotWindow::Telemetry);
	CHECK_FALSE(ParseScreenshotWindow("telemetry"));

	TelemetryScope const scope;
	EditorContext context;
	EditorTelemetry telemetry;
	int shown = 0;
	CommandRegistry registry;
	REQUIRE(RegisterTelemetryCommands(registry, telemetry, context,
	                                  [&shown]
	                                  {
										  ++shown;
									  })
	            .IsOk());
	auto const start = [&registry](Json params)
	{
		std::optional<CommandResult> result;
		registry.Execute("telemetry.start", params,
		                 [&result](CommandResult value)
		                 {
							 result.emplace(std::move(value));
						 });
		REQUIRE(result);
		REQUIRE(result->IsOk());
		return result->TakeValue();
	};
	CHECK(start(Json::object())["shown"] == false);
	CHECK(shown == 0);
	Json const opened = start(Json::object({{"panel", true}}));
	CHECK(opened["shown"] == true);
	CHECK(opened["restarted"] == true);
	CHECK(shown == 1);
	CHECK(start(Json::object({{"panel", false}}))["shown"] == false);
	CHECK(shown == 1);
	telemetry.Stop();

	// Headless, nothing shows it.
	CommandRegistry headless;
	EditorTelemetry other;
	REQUIRE(RegisterTelemetryCommands(headless, other, context).IsOk());
	std::optional<CommandResult> headlessResult;
	headless.Execute("telemetry.start", Json::object({{"panel", true}}),
	                 [&headlessResult](CommandResult value)
	                 {
						 headlessResult.emplace(std::move(value));
					 });
	REQUIRE(headlessResult);
	REQUIRE(headlessResult->IsOk());
	CHECK(headlessResult->GetValue()["shown"] == false);
	other.Stop();
}

TEST_CASE("TelemetryPanel: a recording of a playing FeatureTest scene shows live, then its last lap over its best")
{
	// The sample as a player gets it: its scenes and assets, with the scripts this build made of its sources.
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting(Testing::GetFeatureTestScriptsPath());
	TelemetryScope const scope;
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory directory;
	EditorContext context;
	EditorOperations operations{context};
	EditorScripts scripts{context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
	PlayMode play{context, scripts};
	CommandRegistry registry;
	REQUIRE(RegisterPlayCommands(registry, play, context,
	                             []
	                             {
									 return glm::uvec2(640, 480);
								 })
	            .IsOk());
	auto const run = [&registry](std::string_view name, Json params)
	{
		std::optional<CommandResult> result;
		registry.Execute(name, params,
		                 [&result](CommandResult value)
		                 {
							 result.emplace(std::move(value));
						 });
		REQUIRE(result);
		REQUIRE_MESSAGE(result->IsOk(), (result->IsError() ? result->GetError().Message : std::string()));
	};
	std::filesystem::path const sample = FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "FeatureTest";
	std::filesystem::path const project = directory.GetPath() / "FeatureTest";
	REQUIRE(FileSystem::CopyDirectory(sample / "Assets", project / "Assets").IsOk());
	REQUIRE(FileSystem::Copy(sample / "FeatureTest.sproj", project / "FeatureTest.sproj", false).IsOk());
	REQUIRE(operations.OpenProject(project / "FeatureTest.sproj").IsOk());

	PanelFixture fixture;
	fixture.Panel.ChooseChannels({"FeatureTest.Distance", "Features.FixedSteps"});
	// The open panel records, as the editor's frame tells it before the panels draw.
	fixture.Owner.Update(fixture.Open);
	REQUIRE(fixture.Owner.IsPanelRecording());
	run("play.start", Json::object());
	run("play.pause", Json::object());
	fixture.ShownScene = &context.GetScene();
	// Live: the rows of the play show as they come, and its laps as they are made (ApplicationTests rides three laps of 24,
	// 18 and 30 steps from the first step on).
	uint64_t rows = 0;
	for (int chunk = 0; chunk < 3; ++chunk)
	{
		run("play.advance", Json::object({{"frames", 30}}));
		fixture.Owner.Update(fixture.Open);
		fixture.Draw();
		TelemetryPanel::DrawRecord const& live = fixture.Record();
		CAPTURE(chunk);
		CHECK(live.Live);
		CHECK(live.Rows > rows);
		rows = live.Rows;
		REQUIRE(live.Shown);
		CHECK(fixture.Lane("FeatureTest.Distance").Shown.Rows > 0);
	}
	TelemetryPanel::DrawRecord const& record = fixture.Record();
	// After the finish at step 73 nothing is in progress: Lap 3 (steps 43 to 72) shows over Lap 2 (steps 25 to 42), the best.
	REQUIRE(record.Shown);
	CHECK(record.Shown->Label == "Lap 3");
	CHECK_FALSE(record.Shown->InProgress);
	CHECK(record.Shown->FirstRow == 42);
	CHECK(record.Shown->EndRow == 72);
	REQUIRE(record.Reference);
	CHECK(record.Reference->Label == "Lap 2");
	CHECK(record.Reference->FirstRow == 24);
	CHECK(record.Reference->EndRow == 42);
	// FeatureTest's distance is not Ride.Distance: by time, both from 0 s, to Lap 3's 29 steps.
	CHECK(record.Axis == TelemetryAxis::Time);
	CHECK(record.AxisEnd == doctest::Approx(29.0 / 60.0).epsilon(0.001));

	// Against its distance (120 m a lap, from 0): both laps run from 0 m, Lap 3 to 116 m.
	fixture.Panel.PlotAgainstChannel("FeatureTest.Distance");
	fixture.Panel.ChooseChannels({"Features.FixedSteps", "FeatureTest.Distance"});
	fixture.Draw();
	CHECK(record.Axis == TelemetryAxis::Distance);
	CHECK(record.DistanceChannel == "FeatureTest.Distance");
	CHECK(record.AxisStart == 0.0);
	CHECK(record.AxisEnd == doctest::Approx(120.0 * 29.0 / 30.0).epsilon(0.001));
	TelemetryPanel::DrawRecord::Lane const& steps = fixture.Lane("Features.FixedSteps");
	CHECK(steps.Shown.Rows == 30);
	REQUIRE(steps.Reference);
	CHECK(steps.Reference->Rows == 18);
	CHECK(steps.Shown.Minimum == 43.0f);
	CHECK(steps.Reference->Maximum == 42.0f);
	REQUIRE(steps.Shown.First);
	REQUIRE(steps.Reference->First);
	CHECK((*steps.Shown.First)[0] == doctest::Approx((*steps.Reference->First)[0]));

	// Stopping the game ends its session; the panel still shows it.
	run("play.stop", Json::object());
	fixture.Owner.Update(fixture.Open);
	fixture.ShownScene = &context.GetScene();
	fixture.Draw();
	CHECK_FALSE(record.Live);
	CHECK(record.Shown->Label == "Lap 3");
	fixture.Open = false;
	fixture.Owner.Update(fixture.Open);
	CHECK_FALSE(Telemetry::IsRecording());
}
