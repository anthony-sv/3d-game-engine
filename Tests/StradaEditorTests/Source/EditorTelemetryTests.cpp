#include "Core/TelemetryTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/Automation/TelemetryCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/EditorTelemetry.h"
#include "Editor/PlayMode.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Telemetry.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/TelemetryCsv.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <cstdint>
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
	// The telemetry commands as the editor registers them, beside play mode's, with an editor model whose project they
	// write exports in.
	struct CommandFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};
		EditorTelemetry TelemetryOwner;
		CommandRegistry Registry;

		CommandFixture()
		{
			REQUIRE(RegisterTelemetryCommands(Registry, TelemetryOwner, Context).IsOk());
			REQUIRE(RegisterPlayCommands(Registry, Play, Context,
			                             []
			                             {
											 return glm::uvec2(640, 480);
										 })
			            .IsOk());
		}

		CommandResult Run(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			return std::move(*result);
		}

		Json Ok(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
			return result.TakeValue();
		}

		AutomationErrorCode Fails(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE(result.IsError());
			return result.GetError().Code;
		}
	};

	constexpr std::string_view TelemetryCommandNames[] = {"telemetry.start", "telemetry.stop", "telemetry.channels", "telemetry.read",
	                                                      "telemetry.export"};

	// A session of ten steps (11 to 20) recorded by hand: Speed every step (the step's number), Vehicle.<entity>.Rpm on the
	// first step, Gear on even steps (so the channels come in that order), laps from step 12 to 15 (Lap 1), 15 to 17 (Lap 2, the best) and
	// 17 to 20 (Lap 3), Sector 1.1 at step 13, and Lap 4 in progress from step 20.
	void RecordLaps(UUID vehicle)
	{
		std::string const rpm = "Vehicle." + std::to_string(vehicle.GetValue()) + ".Rpm";
		for (uint64_t step = 11; step <= 20; ++step)
		{
			Telemetry::Record("Speed", static_cast<float>(step));
			if (step % 2 == 0)
			{
				Telemetry::Record("Gear", static_cast<float>(step / 2));
			}
			if (step == 11)
			{
				Telemetry::Record(rpm, 9000.0f);
			}
			switch (step)
			{
				case 12:
					Telemetry::Mark("Lap 1");
					break;
				case 13:
					Telemetry::Mark("Sector 1.1");
					break;
				case 15:
					Telemetry::Mark("Lap 2");
					break;
				case 17:
					Telemetry::Mark("Lap 3");
					break;
				case 20:
					Telemetry::Mark("Lap 4");
					break;
				default:
					break;
			}
			Telemetry::EndStep(MakeTelemetryStep(step));
		}
	}

	std::vector<std::string> SplitLines(std::string const& text)
	{
		std::vector<std::string> lines;
		size_t start = 0;
		while (start < text.size())
		{
			size_t const end = text.find("\r\n", start);
			REQUIRE(end != std::string::npos);
			lines.push_back(text.substr(start, end - start));
			start = end + 2;
		}
		return lines;
	}
}

TEST_CASE("EditorTelemetry: the open panel records, telemetry.start's recording outlives it, and Stop holds it until Resume")
{
	EditorTelemetry telemetry;
	// Without the recorder nothing records, and the panel is told why once.
	telemetry.Update(true);
	CHECK_FALSE(Telemetry::IsRecording());
	CHECK_FALSE(telemetry.GetRecordingError().empty());
	CHECK_FALSE(telemetry.Start({}).IsOk());
	telemetry.Update(false);

	TelemetryScope const scope;
	// Closed, the panel records nothing.
	telemetry.Update(false);
	CHECK_FALSE(Telemetry::IsRecording());
	// Open, it records with its settings, and closing it stops what it began.
	telemetry.Update(true);
	CHECK(Telemetry::IsRecording());
	CHECK(telemetry.IsPanelRecording());
	CHECK_FALSE(telemetry.IsCommandRecording());
	CHECK(Telemetry::GetStatus().Settings.MaxSteps == EditorTelemetry::GetPanelSettings().MaxSteps);
	Telemetry::Record("Speed", 1.0f);
	Telemetry::EndStep(MakeTelemetryStep(1));
	telemetry.Update(false);
	CHECK_FALSE(Telemetry::IsRecording());
	// The session stays after the panel stopped.
	CHECK(Telemetry::GetSessions().size() == 1);

	// A start restarts what runs with its settings, keeps the sessions held, and goes on when the panel closes.
	telemetry.Update(true);
	REQUIRE(telemetry.Start({.MaxSteps = 3}).IsOk());
	CHECK(telemetry.IsCommandRecording());
	CHECK(Telemetry::GetStatus().Settings.MaxSteps == 3);
	CHECK(Telemetry::GetSessions().size() == 1);
	telemetry.Update(false);
	CHECK(Telemetry::IsRecording());
	CHECK(telemetry.IsCommandRecording());
	// Settings out of bounds leave the recording as it is.
	CHECK_FALSE(telemetry.Start({.MaxChannels = 1}).IsOk());
	CHECK_FALSE(telemetry.Start({.MaxSteps = 0}).IsOk());
	CHECK_FALSE(telemetry.Start({.HistoryBytes = Telemetry::MaxHistoryBytes + 1}).IsOk());
	CHECK(telemetry.IsCommandRecording());
	CHECK(Telemetry::GetStatus().Settings.MaxSteps == 3);

	// Stop ends it whoever began it, and holds the open panel until Resume.
	CHECK(telemetry.Stop());
	CHECK_FALSE(telemetry.Stop());
	CHECK(telemetry.IsHeld());
	telemetry.Update(true);
	CHECK_FALSE(Telemetry::IsRecording());
	telemetry.Resume();
	telemetry.Update(true);
	CHECK(telemetry.IsPanelRecording());
	// Closing and opening the panel again lifts the hold too.
	CHECK(telemetry.Stop());
	telemetry.Update(false);
	telemetry.Update(true);
	CHECK(telemetry.IsPanelRecording());
	telemetry.Update(false);
	CHECK_FALSE(Telemetry::IsRecording());
}

TEST_CASE("TelemetryCommands: every command is documented, only channels and read change nothing, and none is an undo step")
{
	CommandFixture fixture;
	Result<std::string> reference = FileSystem::ReadTextFile(STRADA_AUTOMATION_DOC_PATH);
	REQUIRE(reference.IsOk());
	for (std::string_view const name : TelemetryCommandNames)
	{
		CAPTURE(name);
		CommandDefinition const* const command = fixture.Registry.Find(name);
		REQUIRE(command != nullptr);
		CHECK(command->ReadOnly == (name == "telemetry.channels" || name == "telemetry.read"));
		CHECK_FALSE(command->IsAsync());
		CHECK(reference.GetValue().find("`" + std::string(name) + "`") != std::string::npos);
	}
}

TEST_CASE("TelemetryCommands: without the recorder they say so, and without sessions they say there are none")
{
	CommandFixture fixture;
	REQUIRE(fixture.Operations.CreateProject(fixture.Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	for (std::string_view const name : TelemetryCommandNames)
	{
		CAPTURE(name);
		Json params = Json::object();
		if (name == "telemetry.read")
		{
			params["channels"] = Json::array({"Speed"});
		}
		if (name == "telemetry.export")
		{
			params["path"] = "Run.csv";
		}
		CHECK(fixture.Fails(name, params) == AutomationErrorCode::Unavailable);
	}

	TelemetryScope const scope;
	Json const listed = fixture.Ok("telemetry.channels");
	CHECK(listed["recording"] == false);
	CHECK(listed["sessions"] == Json::array());
	CHECK(listed["session"].is_null());
	CHECK(fixture.Fails("telemetry.channels", Json::object({{"session", 1}})) == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("telemetry.read", Json::object({{"channels", Json::array({"Speed"})}})) == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("telemetry.export", Json::object({{"path", "Run.csv"}})) == AutomationErrorCode::InvalidOperation);
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Game" / "Run.csv"));
	Json const stopped = fixture.Ok("telemetry.stop");
	CHECK(stopped["stopped"] == false);
	CHECK(stopped["recording"] == false);
	CHECK(stopped["startedBy"].is_null());
}

TEST_CASE("TelemetryCommands: a recording is started, listed, read by steps, laps and the best lap, exported and stopped")
{
	TelemetryScope const scope;
	CommandFixture fixture;
	std::filesystem::path const project = fixture.Directory.GetPath() / "Game";
	REQUIRE(fixture.Operations.CreateProject(project, "Game", Scene("Main")).IsOk());
	EntityCreateInfo info;
	info.Name = "Bike";
	Result<UUID> const bike = fixture.Operations.CreateEntity(info);
	REQUIRE(bike.IsOk());
	size_t const undo = fixture.Context.GetHistory().GetUndoCount();
	bool const dirty = fixture.Context.IsDirty();

	Json const started = fixture.Ok("telemetry.start", Json::object({{"maxChannels", 64}, {"historySessions", 4}}));
	CHECK(started["recording"] == true);
	CHECK(started["startedBy"] == "command");
	CHECK(started["restarted"] == false);
	CHECK(started["settings"]["maxChannels"] == 64);
	CHECK(started["settings"]["historySessions"] == 4);
	CHECK(started["settings"]["maxSteps"] == TelemetrySettings().MaxSteps);
	CHECK(started["liveSession"].is_null());
	RecordLaps(bike.GetValue());

	// The sessions, and the newest one's channels, marks and laps.
	Json const listed = fixture.Ok("telemetry.channels");
	CAPTURE(listed.dump());
	CHECK(listed["recording"] == true);
	REQUIRE(listed["sessions"].size() == 1);
	CHECK(listed["sessions"][0]["live"] == true);
	CHECK(listed["sessions"][0]["firstStep"] == 11);
	CHECK(listed["sessions"][0]["lastStep"] == 20);
	CHECK(listed["sessions"][0]["rows"] == 10);
	CHECK(listed["sessions"][0]["laps"] == 3);
	Json const& session = listed["session"];
	uint32_t const id = session["session"].get<uint32_t>();
	REQUIRE(session["channels"].size() == 3);
	Json const& speed = session["channels"][0];
	CHECK(speed["name"] == "Speed");
	CHECK(speed["values"] == 10);
	CHECK(speed["firstStep"] == 11);
	CHECK(speed["lastStep"] == 20);
	CHECK(speed["minimum"] == 11.0);
	CHECK(speed["maximum"] == 20.0);
	CHECK(speed["last"] == 20.0);
	CHECK(speed["entity"].is_null());
	// An engine channel names its entity, which the edited scene has.
	Json const& rpm = session["channels"][1];
	CHECK(rpm["name"] == "Vehicle." + std::to_string(bike.GetValue().GetValue()) + ".Rpm");
	CHECK(rpm["entity"] == Json::object({{"id", std::to_string(bike.GetValue().GetValue())}, {"name", "Bike"}}));
	CHECK(session["marks"].size() == 5);
	CHECK(session["marks"][1] == Json::object({{"step", 13}, {"label", "Sector 1.1"}}));
	CHECK(session["marksOmitted"] == 0);
	REQUIRE(session["laps"].size() == 3);
	CHECK(session["laps"][0] == Json::object({{"label", "Lap 1"},
	                                          {"number", 1},
	                                          {"firstStep", 12},
	                                          {"lastStep", 14},
	                                          {"seconds", Json(3.0 * static_cast<double>(1.0f / 60.0f))},
	                                          {"inProgress", false}}));
	CHECK(session["bestLap"]["label"] == "Lap 2");
	CHECK(session["lapInProgress"]["label"] == "Lap 4");
	CHECK(session["lapInProgress"]["inProgress"] == true);
	CHECK(session["dropped"] == Json::object({{"values", 0}, {"marks", 0}, {"steps", 0}}));

	// A range of steps, in samples of the smallest stride that fits, with nulls where a step has no value.
	Json const range = fixture.Ok("telemetry.read", Json::object({{"channels", Json::array({"Gear", "Speed"})}, {"from", 12}, {"to", 16}}));
	CAPTURE(range.dump());
	CHECK(range["session"] == id);
	CHECK(range["from"] == 12);
	CHECK(range["to"] == 16);
	CHECK(range["rows"] == 5);
	CHECK(range["stride"] == 1);
	CHECK(range["steps"] == Json::array({12, 13, 14, 15, 16}));
	CHECK(range["lap"].is_null());
	CHECK(range["channels"][0]["name"] == "Gear");
	CHECK(range["channels"][0]["samples"] == Json::array({6.0, nullptr, 7.0, nullptr, 8.0}));
	CHECK(range["channels"][0]["summary"] == Json::object({{"values", 3},
	                                                       {"minimum", 6.0},
	                                                       {"maximum", 8.0},
	                                                       {"mean", 7.0},
	                                                       {"first", 6.0},
	                                                       {"last", 8.0},
	                                                       {"minimumStep", 12},
	                                                       {"maximumStep", 16}}));
	Json const strided = fixture.Ok("telemetry.read", Json::object({{"channels", Json::array({"Speed"})}, {"maxSamples", 4}}));
	CHECK(strided["rows"] == 10);
	CHECK(strided["stride"] == 3);
	CHECK(strided["steps"] == Json::array({11, 14, 17, 20}));
	CHECK(strided["channels"][0]["samples"] == Json::array({11.0, 14.0, 17.0, 20.0}));
	CHECK(strided["channels"][0]["summary"]["values"] == 10);
	CHECK(strided["channels"][0]["summary"]["mean"] == 15.5);
	// A range clipped to the session's steps; one without any of them is refused.
	CHECK(fixture.Ok("telemetry.read", Json::object({{"channels", Json::array({"Speed"})}, {"from", 1}, {"to", 12}}))["rows"] == 2);
	// A channel with no value in its range has a summary of nothing.
	Json const none = fixture.Ok("telemetry.read", Json::object({{"channels", Json::array({rpm["name"]})}, {"from", 15}}));
	CHECK(none["channels"][0]["summary"]["values"] == 0);
	CHECK(none["channels"][0]["summary"]["minimum"].is_null());

	// Laps: by label, the best, and the one in progress.
	Json const best = fixture.Ok("telemetry.read", Json::object({{"channels", Json::array({"Speed"})}, {"lap", "best"}}));
	CHECK(best["lap"]["label"] == "Lap 2");
	CHECK(best["from"] == 15);
	CHECK(best["to"] == 16);
	CHECK(best["channels"][0]["samples"] == Json::array({15.0, 16.0}));
	Json const third = fixture.Ok("telemetry.read", Json::object({{"channels", Json::array({"Speed"})}, {"lap", "Lap 3"}}));
	CHECK(third["from"] == 17);
	CHECK(third["to"] == 19);
	Json const progress = fixture.Ok("telemetry.read", Json::object({{"channels", Json::array({"Speed"})}, {"lap", "Lap 4"}}));
	CHECK(progress["lap"]["inProgress"] == true);
	CHECK(progress["from"] == 20);
	CHECK(progress["to"] == 20);

	// What read refuses.
	auto const refusedRead = [&fixture](Json params)
	{
		CAPTURE(params.dump());
		return fixture.Fails("telemetry.read", std::move(params));
	};
	CHECK(refusedRead(Json::object({{"channels", Json::array({"Missing"})}})) == AutomationErrorCode::InvalidParams);
	CHECK(refusedRead(Json::object({{"channels", Json::array({"Speed", "Speed"})}})) == AutomationErrorCode::InvalidParams);
	CHECK(refusedRead(Json::object({{"channels", Json::array({"Speed"})}, {"session", id + 1}})) == AutomationErrorCode::InvalidParams);
	CHECK(refusedRead(Json::object({{"channels", Json::array({"Speed"})}, {"lap", "Lap 9"}})) == AutomationErrorCode::InvalidParams);
	CHECK(refusedRead(Json::object({{"channels", Json::array({"Speed"})}, {"lap", "best"}, {"from", 12}})) ==
	      AutomationErrorCode::InvalidParams);
	CHECK(refusedRead(Json::object({{"channels", Json::array({"Speed"})}, {"from", 16}, {"to", 12}})) ==
	      AutomationErrorCode::InvalidParams);
	CHECK(refusedRead(Json::object({{"channels", Json::array({"Speed"})}, {"from", 21}})) == AutomationErrorCode::InvalidParams);
	CHECK(refusedRead(Json::object({{"channels", Json::array()}})) == AutomationErrorCode::InvalidParams);
	CHECK(refusedRead(Json::object({{"channels", Json::array({"Speed"})}, {"maxSamples", 0}})) == AutomationErrorCode::InvalidParams);

	// Exports: inside the project with its folders made, the best lap's rows, some channels, and an absolute path.
	Json const exported = fixture.Ok("telemetry.export", Json::object({{"path", ".strada/Captures/Run.csv"}}));
	std::filesystem::path const file = project / ".strada" / "Captures" / "Run.csv";
	CHECK(exported["path"] == FileSystem::PathToUtf8(file));
	CHECK(exported["rows"] == 10);
	CHECK(exported["channels"] == Json::array({"Speed", rpm["name"], "Gear"}));
	CHECK(exported["bytes"].get<uint64_t>() == FileSystem::GetFileSize(file).value_or(0));
	Result<std::string> const text = FileSystem::ReadTextFile(file);
	REQUIRE(text.IsOk());
	std::vector<std::string> const lines = SplitLines(text.GetValue());
	REQUIRE(lines.size() == 11);
	CHECK(lines[0] == "\"Step\",\"Time\",\"Speed\",\"" + rpm["name"].get<std::string>() + "\",\"Gear\",\"Marks\"");
	CHECK(lines[0].starts_with(TelemetryCsvWriter::HeaderStart));
	Json const lap =
		fixture.Ok("telemetry.export", Json::object({{"path", "Lap.csv"}, {"lap", "best"}, {"channels", Json::array({"Gear"})}}));
	CHECK(lap["from"] == 15);
	CHECK(lap["to"] == 16);
	CHECK(lap["lap"]["label"] == "Lap 2");
	Result<std::string> const lapText = FileSystem::ReadTextFile(project / "Lap.csv");
	REQUIRE(lapText.IsOk());
	std::vector<std::string> const lapLines = SplitLines(lapText.GetValue());
	REQUIRE(lapLines.size() == 3);
	CHECK(lapLines[0] == "\"Step\",\"Time\",\"Gear\",\"Marks\"");
	CHECK(lapLines[1].starts_with("15,"));
	CHECK(lapLines[1].ends_with(",\"Lap 2\""));
	std::filesystem::path const outside = fixture.Directory.GetPath() / "Outside" / "Run.csv";
	fixture.Ok("telemetry.export", Json::object({{"path", FileSystem::PathToUtf8(outside)}, {"from", 18}}));
	CHECK(FileSystem::Exists(outside));

	// An existing file is replaced only with overwrite, and only when it is an export.
	CHECK(fixture.Fails("telemetry.export", Json::object({{"path", "Lap.csv"}})) == AutomationErrorCode::InvalidOperation);
	fixture.Ok("telemetry.export", Json::object({{"path", "Lap.csv"}, {"overwrite", true}}));
	REQUIRE(FileSystem::WriteTextFile(project / "Table.csv", "Name,Score\r\n").IsOk());
	CHECK(fixture.Fails("telemetry.export", Json::object({{"path", "Table.csv"}, {"overwrite", true}})) ==
	      AutomationErrorCode::InvalidOperation);
	CHECK(FileSystem::ReadTextFile(project / "Table.csv").GetValue() == "Name,Score\r\n");
	// Relative paths stay in the project and end in .csv; folders are refused.
	for (char const* const refused : {"../Escape.csv", "Captures/../../Escape.csv", ".", "Run.json", "Captures", "C:Escape.csv"})
	{
		CAPTURE(refused);
		CHECK(fixture.Fails("telemetry.export", Json::object({{"path", refused}})) == AutomationErrorCode::InvalidParams);
	}
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Escape.csv"));
	REQUIRE(FileSystem::CreateDirectories(project / "Folder.csv").IsOk());
	CHECK(fixture.Fails("telemetry.export", Json::object({{"path", "Folder.csv"}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("telemetry.export", Json::object({{"path", "Bad.csv"}, {"channels", Json::array({"Missing"})}})) ==
	      AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("telemetry.export", Json::object({{"path", "Bad.csv"}, {"lap", "Lap 9"}})) == AutomationErrorCode::InvalidParams);
	CHECK_FALSE(FileSystem::Exists(project / "Bad.csv"));

	// A second start restarts the recording and keeps the session; stop ends it and keeps it too.
	Json const restarted = fixture.Ok("telemetry.start");
	CHECK(restarted["restarted"] == true);
	CHECK(restarted["sessionsHeld"] == 1);
	CHECK(restarted["settings"]["maxChannels"] == TelemetrySettings().MaxChannels);
	Json const stopped = fixture.Ok("telemetry.stop");
	CHECK(stopped["stopped"] == true);
	CHECK(stopped["recording"] == false);
	CHECK(stopped["panelHeld"] == true);
	CHECK(stopped["sessionsHeld"] == 1);
	CHECK(fixture.Ok("telemetry.stop")["stopped"] == false);
	CHECK(fixture.Ok("telemetry.channels", Json::object({{"session", id}}))["session"]["live"] == false);

	// None of them is an undo step, and the scene is as it was.
	CHECK(fixture.Context.GetHistory().GetUndoCount() == undo);
	CHECK(fixture.Context.IsDirty() == dirty);

	// Without a project, relative paths have nothing to be relative to.
	REQUIRE(fixture.Operations.CloseProject().IsOk());
	CHECK(fixture.Fails("telemetry.export", Json::object({{"path", "Run.csv"}})) == AutomationErrorCode::InvalidOperation);
}

TEST_CASE("TelemetryCommands: each play is a session that ends when play stops, before any editor frame")
{
	TelemetryScope const scope;
	CommandFixture fixture;
	fixture.Ok("telemetry.start");
	fixture.Ok("play.start");
	fixture.Ok("play.pause");
	fixture.Ok("play.advance", Json::object({{"frames", 5}}));
	Json const playing = fixture.Ok("telemetry.channels");
	CAPTURE(playing.dump());
	REQUIRE(playing["sessions"].size() == 1);
	CHECK(playing["session"]["live"] == true);
	CHECK(playing["session"]["firstStep"] == 1);
	CHECK(playing["session"]["rows"] == 5);
	uint32_t const first = playing["session"]["session"].get<uint32_t>();

	// A recording stopped and started again while the game plays begins a session at its next step.
	CHECK(fixture.Ok("telemetry.stop")["liveSession"].is_null());
	CHECK(fixture.Ok("telemetry.channels")["session"]["live"] == false);
	fixture.Ok("telemetry.start");
	fixture.Ok("play.advance", Json::object({{"frames", 2}}));
	Json const again = fixture.Ok("telemetry.channels");
	REQUIRE(again["sessions"].size() == 2);
	CHECK(again["session"]["live"] == true);
	CHECK(again["session"]["firstStep"] == 6);
	CHECK(again["session"]["rows"] == 2);
	CHECK(fixture.Ok("telemetry.stop")["stopped"] == true);
	fixture.Ok("telemetry.start");
	fixture.Ok("play.advance", Json::object({{"frames", 1}}));
	REQUIRE(fixture.Ok("telemetry.channels")["session"]["live"] == true);

	// Stopping the game ends its session at once: no editor frame runs in between.
	fixture.Ok("play.stop");
	Json const stopped = fixture.Ok("telemetry.channels");
	REQUIRE(stopped["sessions"].size() == 3);
	CHECK(stopped["session"]["live"] == false);
	CHECK(stopped["recording"] == true);

	// The next play is a scene of its own: another session, from its first step.
	fixture.Ok("play.start");
	fixture.Ok("play.pause");
	fixture.Ok("play.advance", Json::object({{"frames", 3}}));
	Json const next = fixture.Ok("telemetry.channels");
	REQUIRE(next["sessions"].size() == 4);
	CHECK(next["session"]["live"] == true);
	CHECK(next["session"]["firstStep"] == 1);
	CHECK(next["session"]["rows"] == 3);
	CHECK(next["sessions"][0]["session"] == first);
	fixture.Ok("play.stop");
	fixture.Ok("telemetry.stop");
}

TEST_CASE("TelemetryCommands: a playing FeatureTest scene is recorded, read back and exported with its laps")
{
	// The sample as a player gets it: its scenes and assets, with the scripts this build made of its sources.
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting(Testing::GetFeatureTestScriptsPath());
	TelemetryScope const scope;
	CommandFixture fixture;
	std::filesystem::path const sample = FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "FeatureTest";
	std::filesystem::path const project = fixture.Directory.GetPath() / "FeatureTest";
	REQUIRE(FileSystem::CopyDirectory(sample / "Assets", project / "Assets").IsOk());
	REQUIRE(FileSystem::Copy(sample / "FeatureTest.sproj", project / "FeatureTest.sproj", false).IsOk());
	REQUIRE(fixture.Operations.OpenProject(project / "FeatureTest.sproj").IsOk());

	fixture.Ok("telemetry.start");
	fixture.Ok("play.start");
	fixture.Ok("play.pause");
	// ApplicationTests rides its three laps in the first 73 steps; 90 frames of 1/60 s run about as many.
	fixture.Ok("play.advance", Json::object({{"frames", 90}}));
	Json const listed = fixture.Ok("telemetry.channels");
	CAPTURE(listed.dump());
	REQUIRE_FALSE(listed["sessions"].empty());
	uint32_t const id = listed["sessions"][0]["session"].get<uint32_t>();
	Json const session = fixture.Ok("telemetry.channels", Json::object({{"session", id}}))["session"];
	CHECK(session["firstStep"] == 1);
	uint64_t const rows = session["rows"].get<uint64_t>();
	CHECK(rows >= 80);
	std::vector<std::string> channels;
	for (Json const& channel : session["channels"])
	{
		channels.push_back(channel["name"].get<std::string>());
	}
	CHECK(std::find(channels.begin(), channels.end(), "FeatureTest.Distance") != channels.end());
	CHECK(std::find(channels.begin(), channels.end(), "Features.FixedSteps") != channels.end());
	// The script's laps; the graph's marks and the contact handlers' are no laps.
	REQUIRE(session["laps"].size() == 3);
	CHECK(session["laps"][0]["firstStep"] == 1);
	CHECK(session["laps"][1]["firstStep"] == 25);
	CHECK(session["laps"][2]["firstStep"] == 43);
	CHECK(session["laps"][2]["lastStep"] == 72);
	CHECK(session["bestLap"]["label"] == "Lap 2");
	CHECK(session["lapInProgress"].is_null());
	bool graphStep = false;
	bool graphMark = false;
	for (Json const& mark : session["marks"])
	{
		graphStep = graphStep || (mark["label"] == "Features.Step" && mark["step"] == 1);
		graphMark = graphMark || mark["label"] == "Features";
	}
	CHECK(graphStep);
	CHECK(graphMark);

	// The best lap read back: 18 steps of 120 m, from 0.
	Json const best = fixture.Ok(
		"telemetry.read",
		Json::object({{"session", id}, {"channels", Json::array({"FeatureTest.Distance", "Features.FixedSteps"})}, {"lap", "best"}}));
	CHECK(best["rows"] == 18);
	CHECK(best["from"] == 25);
	Json const& distance = best["channels"][0];
	CHECK(distance["samples"][0] == 0.0);
	CHECK(distance["summary"]["values"] == 18);
	CHECK(distance["summary"]["maximum"].get<double>() == doctest::Approx(120.0 * 17.0 / 18.0));
	CHECK(best["channels"][1]["summary"]["first"] == 25.0);
	CHECK(best["channels"][1]["summary"]["last"] == 42.0);

	// Exported, then the game stops and its session ends.
	Json const exported = fixture.Ok("telemetry.export", Json::object({{"session", id}, {"path", ".strada/Captures/FeatureTest.csv"}}));
	CHECK(exported["rows"] == rows);
	Result<std::string> const text = FileSystem::ReadTextFile(project / ".strada" / "Captures" / "FeatureTest.csv");
	REQUIRE(text.IsOk());
	CHECK(SplitLines(text.GetValue()).size() == rows + 1);
	CHECK(text.GetValue().find("\"Lap 4\nLap end\"") != std::string::npos);
	if (fixture.Play.IsPlaying())
	{
		fixture.Ok("play.stop");
	}
	CHECK(fixture.Ok("telemetry.channels", Json::object({{"session", id}}))["session"]["live"] == false);
	fixture.Ok("telemetry.stop");
}
