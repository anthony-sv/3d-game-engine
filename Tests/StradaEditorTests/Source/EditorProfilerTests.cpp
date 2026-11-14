#include "Core/ProfilerTestUtilities.h"
#include "HeadlessImGui.h"
#include "TestUtilities.h"

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/ProfilerCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorProfiler.h"
#include "Editor/Panels/ProfilerPanel.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Serialization/ProfilerCaptureSerializer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	// A frame of two threads: on the main thread an orphan (its parent was collected by the frame before), a zone holding
	// two (the first holding one), and a zone with a count; on a worker one zone.
	Testing::CaptureBuilder MakeTreeCapture()
	{
		Testing::CaptureBuilder builder;
		uint32_t const worker = builder.AddThread("Worker");
		builder.AddFrame(0, 10000000);
		// No zone of a smaller depth comes before it: it sits at the thread's root.
		builder.AddZone(0, "Orphan", 1, 0, 500000);
		builder.AddZone(0, "Scene.Update", 0, 600000, 6000000);
		builder.AddZone(0, "Scene.UpdateScripts", 1, 700000, 3000000);
		builder.AddZone(0, "Script.Player", 2, 800000, 1000000);
		builder.AddZone(0, "Scene.UpdatePhysics", 1, 3800000, 2000000);
		ProfilerZone& graph = builder.AddZone(0, "Graph.Scripts/Player.sgraph", 0, 7000000, 1500000);
		builder.SetValue(graph, "Steps", 42);
		builder.AddZone(worker, "Audio.Mix", 0, 500000, 250000);
		return builder;
	}

	// The panel in a window of its own at a fixed place and size.
	struct PanelFixture
	{
		Testing::HeadlessImGui ImGuiContext;
		EditorProfiler Profiler;
		ProfilerPanel Panel;
		bool Open = true;

		void Draw()
		{
			ImGuiContext.Frame(
				[this]
				{
					ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
					ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f));
					Panel.OnImGuiRender(Profiler, Open, {});
				});
		}

		void Click(ImVec2 position)
		{
			ImGuiContext.Click(position,
			                   [this]
			                   {
								   ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
								   ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f));
								   Panel.OnImGuiRender(Profiler, Open, {});
							   });
		}

		std::vector<std::string> ZoneNames() const
		{
			std::vector<std::string> names;
			for (ProfilerPanel::DrawRecord::ZoneRow const& row : Panel.GetLastDraw().ZoneRows)
			{
				names.push_back(std::string(row.Depth, ' ') + row.Name);
			}
			return names;
		}
	};

	// Runs a frame of the recorder with a zone holding another, as the main loop would around the editor's frame.
	void RunRecordedFrame()
	{
		Profiler::BeginFrame();
		{
			ST_PROFILE_SCOPE("Test.Outer");
			ST_PROFILE_SCOPE("Test.Inner");
		}
		Profiler::EndFrame();
	}

	// The profiler commands as the editor registers them, with an editor model whose project they write captures in.
	struct CommandFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorProfiler Profiler;
		CommandRegistry Registry;
		int PanelShown = 0;

		explicit CommandFixture(bool panel = false)
		{
			std::function<void()> showPanel;
			if (panel)
			{
				showPanel = [this]
				{
					PanelShown++;
				};
			}
			REQUIRE(RegisterProfilerCommands(Registry, Profiler, Context, std::move(showPanel)).IsOk());
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
}

TEST_CASE("EditorProfiler: a frame's zones form a tree per thread by the depth rule, with self and busy times")
{
	Testing::CaptureBuilder const builder = MakeTreeCapture();
	ProfilerFrame const& frame = builder.Get().Frames.front();
	std::vector<ProfilerThreadTree> const trees = BuildProfilerFrameTree(frame);
	REQUIRE(trees.size() == 2);
	CHECK(trees[0].Thread == 0);
	CHECK(trees[1].Thread == 1);

	std::vector<ProfilerTreeRow> const& rows = trees[0].Rows;
	REQUIRE(rows.size() == 6);
	// The orphan is at the root; Scene.Update holds UpdateScripts (holding Script.Player) and UpdatePhysics; the graph is at the
	// root.
	std::vector<uint32_t> depths;
	std::vector<uint32_t> ends;
	std::vector<int64_t> selfTimes;
	for (ProfilerTreeRow const& row : rows)
	{
		depths.push_back(row.Depth);
		ends.push_back(row.SubtreeEnd);
		selfTimes.push_back(row.SelfNanoseconds);
	}
	CHECK(depths == std::vector<uint32_t>{0, 0, 1, 2, 1, 0});
	CHECK(ends == std::vector<uint32_t>{1, 5, 4, 4, 5, 6});
	CHECK(selfTimes == std::vector<int64_t>{500000, 1000000, 2000000, 1000000, 2000000, 1500000});
	// The root zones' durations: what the thread spent in zones.
	CHECK(trees[0].BusyNanoseconds == 6000000 + 500000 + 1500000);
	CHECK(trees[1].BusyNanoseconds == 250000);
	CHECK(trees[1].Rows.size() == 1);

	// A child longer than its parent (clocks of a dropped zone's siblings) leaves the parent no negative self time.
	ProfilerFrame skewed = frame;
	skewed.Zones[2].DurationNanoseconds = 9000000;
	CHECK(BuildProfilerFrameTree(skewed)[0].Rows[1].SelfNanoseconds == 0);
	CHECK(BuildProfilerFrameTree(ProfilerFrame()).empty());
}

TEST_CASE("EditorProfiler: frames are found by their index, names by theirs")
{
	Testing::CaptureBuilder builder;
	builder.AddFrame(0, 10);
	builder.AddFrame(10, 20);
	builder.AddFrame(20, 30);
	ProfilerCapture capture = builder.Get();
	for (ProfilerFrame& frame : capture.Frames)
	{
		frame.Index += 40;
	}
	CHECK(FindProfilerFrame(capture, 40) == nullptr);
	REQUIRE(FindProfilerFrame(capture, 41) != nullptr);
	CHECK(FindProfilerFrame(capture, 42)->StartNanoseconds == 10);
	CHECK(FindProfilerFrame(capture, 43)->StartNanoseconds == 20);
	CHECK(FindProfilerFrame(capture, 44) == nullptr);
	CHECK(FindProfilerFrame(ProfilerCapture(), 1) == nullptr);
	capture.Names = {"Scene.Update"};
	CHECK(GetProfilerName(capture, 0) == "Scene.Update");
	CHECK(GetProfilerName(capture, 1) == "?");
	CHECK(GetProfilerName(capture, ProfilerNoName) == "?");
}

TEST_CASE("EditorProfiler: the open panel records, profiler.start's recording outlives it, and Stop holds it until Resume")
{
	EditorProfiler profiler;
	// Without the recorder nothing records, and the panel is told why once.
	profiler.Update(true);
	CHECK_FALSE(Profiler::IsRecording());
	CHECK_FALSE(profiler.GetRecordingError().empty());
	CHECK_FALSE(profiler.Start({}).IsOk());
	profiler.Update(false);

	Testing::ProfilerScope scope;
	if (!Profiler::GetStatus().Available)
	{
		// Dist builds record nothing.
		profiler.Update(true);
		CHECK_FALSE(Profiler::IsRecording());
		return;
	}
	// Closed, the panel records nothing.
	profiler.Update(false);
	CHECK_FALSE(Profiler::IsRecording());
	// Open, it records with its settings, and closing it stops what it began.
	profiler.Update(true);
	CHECK(Profiler::IsRecording());
	CHECK(profiler.IsPanelRecording());
	CHECK_FALSE(profiler.IsCommandRecording());
	CHECK(Profiler::GetStatus().HistoryFrames == EditorProfiler::GetPanelSettings().HistoryFrames);
	RunRecordedFrame();
	RunRecordedFrame();
	CHECK(Profiler::GetRecording().Frames.size() == 2);
	profiler.Update(false);
	CHECK_FALSE(Profiler::IsRecording());
	// The history stays after the panel stopped.
	CHECK(Profiler::GetRecording().Frames.size() == 2);

	// A start restarts what runs with its settings and goes on when the panel closes.
	profiler.Update(true);
	REQUIRE(profiler.Start({.HistoryFrames = 3}).IsOk());
	CHECK(profiler.IsCommandRecording());
	CHECK(Profiler::GetStatus().HistoryFrames == 3);
	for (int frame = 0; frame < 5; frame++)
	{
		RunRecordedFrame();
	}
	CHECK(Profiler::GetRecording().Frames.size() == 3);
	profiler.Update(false);
	CHECK(Profiler::IsRecording());
	CHECK(profiler.IsCommandRecording());
	// Settings out of bounds leave the recording as it is.
	CHECK_FALSE(profiler.Start({.HistoryFrames = 0}).IsOk());
	CHECK_FALSE(profiler.Start({.HistoryZones = Profiler::MaxHistoryZones + 1}).IsOk());
	CHECK_FALSE(profiler.Start({.ZonesPerThread = 1}).IsOk());
	CHECK(profiler.IsCommandRecording());

	// Stop ends it whoever began it, and holds the open panel until Resume.
	CHECK(profiler.Stop());
	CHECK_FALSE(profiler.Stop());
	CHECK(profiler.IsHeld());
	profiler.Update(true);
	CHECK_FALSE(Profiler::IsRecording());
	profiler.Resume();
	profiler.Update(true);
	CHECK(profiler.IsPanelRecording());
	// Closing and opening the panel again lifts the hold too.
	CHECK(profiler.Stop());
	profiler.Update(false);
	profiler.Update(true);
	CHECK(profiler.IsPanelRecording());
	profiler.Update(false);
	CHECK_FALSE(Profiler::IsRecording());
}

TEST_CASE("EditorProfiler: a capture opened shows in place of the live history; one that does not read leaves it")
{
	Testing::TemporaryDirectory directory;
	EditorProfiler profiler;
	CHECK(profiler.GetCapture() == nullptr);
	uint64_t const revision = profiler.GetCaptureRevision();
	std::filesystem::path const path = directory.GetPath() / "Run.json";
	REQUIRE(ProfilerCaptureSerializer::Save(MakeTreeCapture().Get(), path).IsOk());
	REQUIRE(profiler.Open(path).IsOk());
	REQUIRE(profiler.GetCapture() != nullptr);
	CHECK(profiler.GetCaptureName() == "Run.json");
	CHECK(profiler.GetCapturePath() == path);
	CHECK(profiler.GetCaptureRevision() != revision);
	CHECK(Testing::DescribeCapture(*profiler.GetCapture()) == Testing::DescribeCapture(MakeTreeCapture().Get()));

	REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Damaged.json", "{\"Strada\": {\"Version\": 2}}").IsOk());
	CHECK_FALSE(profiler.Open(directory.GetPath() / "Damaged.json").IsOk());
	CHECK_FALSE(profiler.Open(directory.GetPath() / "Missing.json").IsOk());
	CHECK(profiler.GetCaptureName() == "Run.json");

	uint64_t const opened = profiler.GetCaptureRevision();
	profiler.CloseCapture();
	CHECK(profiler.GetCapture() == nullptr);
	CHECK(profiler.GetCaptureName().empty());
	CHECK(profiler.GetCaptureRevision() != opened);
}

TEST_CASE("ProfilerCommands: every command is documented, and only profiler.status reads without changing anything")
{
	CommandFixture fixture;
	Result<std::string> reference = FileSystem::ReadTextFile(STRADA_AUTOMATION_DOC_PATH);
	REQUIRE(reference.IsOk());
	CHECK(fixture.Registry.GetCommandCount() == 5);
	for (char const* const name : {"profiler.start", "profiler.stop", "profiler.status", "profiler.capture", "profiler.open"})
	{
		CAPTURE(name);
		CommandDefinition const* const command = fixture.Registry.Find(name);
		REQUIRE(command != nullptr);
		CHECK(command->ReadOnly == (std::string_view(name) == "profiler.status"));
		CHECK_FALSE(command->IsAsync());
		CHECK(reference.GetValue().find("`" + std::string(name) + "`") != std::string::npos);
	}
}

TEST_CASE("ProfilerCommands: without the recorder they say so, and profiler.status reports it")
{
	CommandFixture fixture;
	CHECK(fixture.Fails("profiler.start") == AutomationErrorCode::Unavailable);
	CHECK(fixture.Fails("profiler.stop") == AutomationErrorCode::Unavailable);
	REQUIRE(fixture.Operations.CreateProject(fixture.Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	CHECK(fixture.Fails("profiler.capture", Json::object({{"path", "Run.json"}})) == AutomationErrorCode::Unavailable);
	Json const status = fixture.Ok("profiler.status", Json::object({{"medians", true}}));
	CHECK(status["initialized"] == false);
	CHECK(status["recording"] == false);
	CHECK(status["startedBy"].is_null());
	CHECK(status["capture"].is_null());
	CHECK(status["medians"].is_null());
}

TEST_CASE("ProfilerCommands: a recording is started, measured, written to a capture, opened and stopped, none an undo step")
{
	Testing::ProfilerScope scope;
	CommandFixture fixture(true);
	if (!Profiler::GetStatus().Available)
	{
		CHECK(fixture.Fails("profiler.start") == AutomationErrorCode::Unavailable);
		return;
	}
	std::filesystem::path const project = fixture.Directory.GetPath() / "Game";
	REQUIRE(fixture.Operations.CreateProject(project, "Game", Scene("Main")).IsOk());
	size_t const undo = fixture.Context.GetHistory().GetUndoCount();
	bool const dirty = fixture.Context.IsDirty();

	Json started = fixture.Ok("profiler.start", Json::object({{"historyFrames", 4}, {"zonesPerThread", 512}}));
	CHECK(started["recording"] == true);
	CHECK(started["startedBy"] == "command");
	CHECK(started["historyFrames"] == 4);
	CHECK(started["restarted"] == false);
	// Nothing was recorded yet: the recording begins with the next frame.
	CHECK(fixture.Fails("profiler.capture", Json::object({{"path", "Captures/Run.json"}})) == AutomationErrorCode::InvalidOperation);
	for (int frame = 0; frame < 6; frame++)
	{
		RunRecordedFrame();
	}
	Json const status = fixture.Ok("profiler.status", Json::object({{"medians", true}}));
	CHECK(status["framesHeld"] == 4);
	CHECK(status["zonesHeld"] == 8);
	REQUIRE(status["medians"]["subsystems"].size() == 1);
	CHECK(status["medians"]["subsystems"][0]["name"] == "Test");
	REQUIRE(status["medians"]["zones"].size() == 2);
	CHECK(status["medians"]["zones"][0]["callsPerFrame"] == 1.0);
	CHECK(status["medians"]["zonesOmitted"] == 0);
	CHECK_FALSE(fixture.Ok("profiler.status").contains("medians"));

	// Written inside the project (folders made), refused on an existing file but with overwrite, and only over a capture.
	Json const captured = fixture.Ok("profiler.capture", Json::object({{"path", "Captures/Run.json"}}));
	std::filesystem::path const file = project / "Captures" / "Run.json";
	CHECK(captured["path"] == FileSystem::PathToUtf8(file));
	CHECK(captured["frames"] == 4);
	CHECK(captured["zones"] == 8);
	CHECK(captured["threads"] == 1);
	CHECK(captured["bytes"].get<uint64_t>() == FileSystem::GetFileSize(file).value_or(0));
	CHECK(captured["medians"]["zones"].size() == 2);
	Result<ProfilerCapture> const written = ProfilerCaptureSerializer::Load(file);
	REQUIRE(written.IsOk());
	CHECK(Testing::FindZones(written.GetValue(), "Test.Inner").size() == 4);
	CHECK(fixture.Fails("profiler.capture", Json::object({{"path", "Captures/Run.json"}})) == AutomationErrorCode::InvalidOperation);
	fixture.Ok("profiler.capture", Json::object({{"path", "Captures/Run.json"}, {"overwrite", true}}));
	REQUIRE(FileSystem::WriteTextFile(project / "Settings.json", "{}").IsOk());
	CHECK(fixture.Fails("profiler.capture", Json::object({{"path", "Settings.json"}, {"overwrite", true}})) ==
	      AutomationErrorCode::InvalidOperation);
	CHECK(FileSystem::ReadTextFile(project / "Settings.json").GetValue() == "{}");
	// Absolute paths are written where they name; relative ones stay in the project and end in .json.
	std::filesystem::path const outside = fixture.Directory.GetPath() / "Outside" / "Run.json";
	fixture.Ok("profiler.capture", Json::object({{"path", FileSystem::PathToUtf8(outside)}}));
	CHECK(FileSystem::Exists(outside));
	for (char const* const refused : {"../Escape.json", "Captures/../../Escape.json", ".", "Run.txt", "Captures"})
	{
		CAPTURE(refused);
		CHECK(fixture.Fails("profiler.capture", Json::object({{"path", refused}})) == AutomationErrorCode::InvalidParams);
	}
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Escape.json"));
	REQUIRE(FileSystem::CreateDirectories(project / "Folder.json").IsOk());
	CHECK(fixture.Fails("profiler.capture", Json::object({{"path", "Folder.json"}})) == AutomationErrorCode::InvalidParams);

	// Opening shows the capture in the panel; the recording goes on.
	Json const opened = fixture.Ok("profiler.open", Json::object({{"path", "Captures/Run.json"}}));
	CHECK(opened["frames"] == 4);
	CHECK(opened["shown"] == true);
	CHECK(fixture.PanelShown == 1);
	REQUIRE(fixture.Profiler.GetCapture() != nullptr);
	CHECK(fixture.Profiler.GetCaptureName() == "Run.json");
	Json const showing = fixture.Ok("profiler.status");
	CHECK(showing["recording"] == true);
	CHECK(showing["capture"]["name"] == "Run.json");
	CHECK(showing["capture"]["frames"] == 4);
	CHECK(fixture.Fails("profiler.open", Json::object({{"path", "Missing.json"}})) == AutomationErrorCode::FileError);
	CHECK(fixture.Fails("profiler.open", Json::object({{"path", "Settings.json"}})) == AutomationErrorCode::FileError);
	CHECK(fixture.Fails("profiler.open", Json::object({{"path", "../Run.json"}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Profiler.GetCaptureName() == "Run.json");

	// A second start restarts the recording.
	Json const restarted = fixture.Ok("profiler.start");
	CHECK(restarted["restarted"] == true);
	CHECK(restarted["framesHeld"] == 0);
	CHECK(restarted["historyFrames"] == ProfilerSettings().HistoryFrames);

	Json stopped = fixture.Ok("profiler.stop");
	CHECK(stopped["stopped"] == true);
	CHECK(stopped["recording"] == false);
	CHECK(stopped["panelHeld"] == true);
	CHECK(fixture.Ok("profiler.stop")["stopped"] == false);

	// None of them is an undo step, and the scene is as it was.
	CHECK(fixture.Context.GetHistory().GetUndoCount() == undo);
	CHECK(fixture.Context.IsDirty() == dirty);

	// Without a project, relative paths have nothing to be relative to.
	REQUIRE(fixture.Operations.CloseProject().IsOk());
	CHECK(fixture.Fails("profiler.open", Json::object({{"path", "Captures/Run.json"}})) == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Ok("profiler.open", Json::object({{"path", FileSystem::PathToUtf8(file)}}))["frames"] == 4);
}

TEST_CASE("ProfilerPanel: a capture's frames, the shown frame's zones by thread with total and self times, its GPU passes")
{
	PanelFixture fixture;
	Testing::CaptureBuilder builder = MakeTreeCapture();
	builder.AddGpuPass("Viewport", "Shadows", 1250000);
	builder.AddGpuPass("Viewport", "Opaque", 2500000);
	// A second frame without GPU passes or zones on the worker.
	builder.AddFrame(10000000, 26000000);
	builder.AddZone(0, "Scene.Update", 0, 10000000, 4000000);
	ProfilerCapture const capture = builder.Get();
	fixture.Profiler.SetCapture(capture, "Run.json");
	fixture.Draw();
	fixture.Draw();

	ProfilerPanel::DrawRecord const& record = fixture.Panel.GetLastDraw();
	REQUIRE(record.Drawn);
	CHECK_FALSE(record.Live);
	CHECK(record.Status == "Capture Run.json");
	CHECK(record.Frames == 2);
	CHECK(record.FirstFrame == 1);
	// The latest frame shows until one is clicked.
	CHECK(record.Frame == 2u);
	CHECK(fixture.ZoneNames() == std::vector<std::string>{"Main", " Scene.Update"});
	CHECK(record.GpuRows.empty());

	// Clicking the first bar shows the first frame: its threads, open, with the zones nested as recorded.
	REQUIRE(record.BarWidth > 0.0f);
	fixture.Click(ImVec2(record.BarsLeft + record.BarWidth * 0.5f, record.BarsBottom - 2.0f));
	CHECK(fixture.Panel.GetSelectedFrame() == 1u);
	CHECK(fixture.Panel.GetLastDraw().Frame == 1u);
	// The zones at the root are open, those below them closed.
	CHECK(fixture.ZoneNames() == std::vector<std::string>{"Main", " Orphan", " Scene.Update", "  Scene.UpdateScripts",
	                                                      "  Scene.UpdatePhysics", " Graph.Scripts/Player.sgraph", "Worker", " Audio.Mix"});
	std::vector<ProfilerPanel::DrawRecord::ZoneRow> const& rows = fixture.Panel.GetLastDraw().ZoneRows;
	REQUIRE(rows.size() == 8);
	CHECK(rows[0].Thread);
	CHECK(rows[0].TotalMilliseconds == doctest::Approx(8.0));
	CHECK(rows[2].TotalMilliseconds == doctest::Approx(6.0));
	CHECK(rows[2].SelfMilliseconds == doctest::Approx(1.0));
	CHECK(rows[3].SelfMilliseconds == doctest::Approx(2.0));
	CHECK(rows[5].Value == "Steps 42");
	CHECK(rows[6].Thread);
	CHECK(rows[7].TotalMilliseconds == doctest::Approx(0.25));

	fixture.Panel.ShowTab(ProfilerPanelTab::GpuPasses);
	fixture.Draw();
	fixture.Draw();
	CHECK(fixture.Panel.GetLastDraw().Tab == ProfilerPanelTab::GpuPasses);
	std::vector<ProfilerPanel::DrawRecord::GpuRow> const& passes = fixture.Panel.GetLastDraw().GpuRows;
	REQUIRE(passes.size() == 2);
	CHECK(passes[0].View == "Viewport");
	CHECK(passes[0].Pass == "Shadows");
	CHECK(passes[0].Milliseconds == doctest::Approx(1.25));
	CHECK(passes[1].Pass == "Opaque");

	fixture.Panel.ShowTab(ProfilerPanelTab::Medians);
	fixture.Draw();
	fixture.Draw();
	CHECK(fixture.Panel.GetLastDraw().Tab == ProfilerPanelTab::Medians);
	CHECK(fixture.Panel.GetLastDraw().MedianRows > 0);

	// Drawing changed nothing of the capture; closing it goes back to the live history, from its latest frame.
	REQUIRE(fixture.Profiler.GetCapture() != nullptr);
	CHECK(Testing::DescribeCapture(*fixture.Profiler.GetCapture()) == Testing::DescribeCapture(capture));
	fixture.Profiler.CloseCapture();
	fixture.Draw();
	CHECK(fixture.Panel.GetLastDraw().Live);
	CHECK_FALSE(fixture.Panel.GetSelectedFrame().has_value());
}

TEST_CASE("ProfilerPanel: the live history shows the recording the open panel makes, paused by Stop")
{
	PanelFixture fixture;
	// Without the recorder the panel says why it does not record.
	fixture.Profiler.Update(true);
	fixture.Draw();
	CHECK(fixture.Panel.GetLastDraw().Live);
	CHECK(fixture.Panel.GetLastDraw().Status.starts_with("Not recording"));
	CHECK(fixture.Panel.GetLastDraw().Frames == 0);
	fixture.Profiler.Update(false);

	Testing::ProfilerScope scope;
	if (!Profiler::GetStatus().Available)
	{
		return;
	}
	// The editor's frames: the layer's update, then the panel's draw inside the recorded frame.
	for (int frame = 0; frame < 4; frame++)
	{
		fixture.Profiler.Update(fixture.Open);
		Profiler::BeginFrame();
		{
			ST_PROFILE_SCOPE("Test.Frame");
		}
		fixture.Draw();
		Profiler::EndFrame();
	}
	ProfilerPanel::DrawRecord const& record = fixture.Panel.GetLastDraw();
	CHECK(record.Live);
	CHECK(record.Status.starts_with("Recording"));
	// The panel opened before the first frame began, which is recorded; each draw sees the frames collected before its own.
	CHECK(record.Frames == 3);
	CHECK(fixture.ZoneNames() == std::vector<std::string>{"Main", " Test.Frame"});

	// Its Pause button stops the recording once the frame is drawn; the frames stay.
	std::array<float, 2> const toggle = fixture.Panel.GetLastDraw().ToggleCenter;
	fixture.Click(ImVec2(toggle[0], toggle[1]));
	CHECK_FALSE(Profiler::IsRecording());
	CHECK(fixture.Profiler.IsHeld());
	fixture.Profiler.Update(fixture.Open);
	fixture.Draw();
	CHECK(fixture.Panel.GetLastDraw().Status == "Paused: 4 frames held");
	CHECK(fixture.Panel.GetLastDraw().Frames == 4);
	// Record lets the panel record again from the layer's next update, a new recording.
	fixture.Click(ImVec2(toggle[0], toggle[1]));
	CHECK_FALSE(fixture.Profiler.IsHeld());
	fixture.Profiler.Update(fixture.Open);
	CHECK(fixture.Profiler.IsPanelRecording());
	fixture.Draw();
	CHECK(fixture.Panel.GetLastDraw().Frames == 0);
	// Closing the window closes the panel, and the layer's update then stops nothing more.
	fixture.Open = false;
	fixture.Profiler.Update(fixture.Open);
	CHECK_FALSE(Profiler::IsRecording());
}
