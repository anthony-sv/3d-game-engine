#include "HeadlessImGui.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"
#include "VisualScriptPanelTestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/Commands/AssetCommands.h"
#include "Editor/VisualScripting/VisualScriptBrushes.h"
#include "Editor/VisualScripting/VisualScriptCanvasRenderer.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptLiteralEditor.h"
#include "Editor/VisualScripting/VisualScriptThemeStyle.h"
#include "Editor/VisualScripting/VisualScriptView.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	// The graphs of spec §7.12: a 320 × 180 cu grid with 3 links per node and 50 comments. The three of T5 are corners of one
	// 100 × 100 grid whose links go to the next column's row and the two below it, so T1's view, and the margin the culling
	// query adds around it, hold the same nodes, links and comments in each: the 5,000-node graph is its 50 first rows, the
	// "500-node" graph its first 26 × 26 nodes (676: the fewest whose corner holds all of that).
	constexpr uint32_t FullColumns = 100;
	constexpr uint32_t FullRows = 100;
	constexpr uint32_t LinksPerNode = 3;
	constexpr uint32_t Comments = 50;
	constexpr float ColumnWidth = 320.0f;
	constexpr float RowHeight = 180.0f;
	constexpr uint32_t SmallSide = 26;

	enum GraphSize : size_t
	{
		Small = 0,
		Medium,
		Large,
		GraphCount
	};

	// The nodes of the first columns × rows of the grid, the links between them that go down (not those that wrap from the
	// last rows to the first) and the comments around them.
	VisualScriptDocument TakeCorner(VisualScriptDocument const& full, uint32_t columns, uint32_t rows)
	{
		auto const kept = [&](uint32_t id)
		{
			uint32_t const column = (id - 1) / FullRows;
			uint32_t const row = (id - 1) % FullRows;
			return id >= 1 && id <= FullColumns * FullRows && column < columns && row < rows;
		};
		VisualScriptDocument document;
		for (VisualScriptNode const& node : full.Graph.Nodes)
		{
			if (kept(node.ID))
			{
				document.Graph.Nodes.push_back(node);
			}
		}
		for (VisualScriptLink const& link : full.Graph.Links)
		{
			bool const down = (link.To.Node - 1) % FullRows >= (link.From.Node - 1) % FullRows;
			if (kept(link.From.Node) && kept(link.To.Node) && down)
			{
				document.Graph.Links.push_back(link);
			}
		}
		for (VisualScriptComment const& comment : full.Graph.Comments)
		{
			if (comment.Position.x + comment.Size.x <= static_cast<float>(columns) * ColumnWidth &&
			    comment.Position.y + comment.Size.y <= static_cast<float>(rows) * RowHeight)
			{
				document.Graph.Comments.push_back(comment);
			}
		}
		return document;
	}

	// The live widget as the graph window draws it (VisualScriptEditorPanel::DrawLiveChip): the literal editor's compact form
	// of the chip's pin type, as wide as the chip's cells, with the descriptor it builds on every draw (ruling R7.6).
	class LiteralChipHost final : public VisualScriptLiveChipHost
	{
	public:
		void DrawLiveChip(VisualScriptLiveChip const& chip) override
		{
			std::span<VisualScriptPinVisual const> const pins = Model->GetCanvas().GetPins();
			if (chip.Pin >= pins.size())
			{
				return;
			}
			VisualScriptLiveChipScope const scope(chip);
			ImGui::PushID("LiveChip");
			uint32_t const cells = std::max(chip.CellCount, 1u);
			float const width = cells > 1
			                        ? std::ceil(chip.CellWidth * static_cast<float>(cells) + chip.CellGap * static_cast<float>(cells - 1))
			                        : std::round(chip.CellWidth);
			ImGui::SetNextItemWidth(width);
			bool popupRequested = false;
			std::optional<Json> const changed =
				m_Editor.Draw(pins[chip.Pin].Type, Json(), Json(), Library, FieldInlineMode::Compact, &popupRequested);
			CHECK_FALSE(changed.has_value());
			ImGui::PopID();
		}

		VisualScriptEditorModel const* Model = nullptr;
		VisualScriptLibrary const* Library = nullptr;

	private:
		VisualScriptLiteralEditor m_Editor;
	};

	// The renderer-only baseline of the live widget: a plain drag per cell, which shows what the literal editor adds.
	class CellChipHost final : public VisualScriptLiveChipHost
	{
	public:
		void DrawLiveChip(VisualScriptLiveChip const& chip) override
		{
			VisualScriptLiveChipScope const scope(chip);
			ImGui::PushID("LiveChip");
			for (uint32_t cell = 0; cell < chip.CellCount; cell++)
			{
				if (cell > 0)
				{
					ImGui::SameLine();
				}
				ImGui::PushID(static_cast<int>(cell));
				ImGui::SetNextItemWidth(chip.CellWidth);
				ImGui::DragFloat("##Cell", &m_Value, 0.1f, 0.0f, 0.0f, "%g");
				ImGui::PopID();
			}
			ImGui::PopID();
		}

	private:
		float m_Value = 0.5f;
	};

	// A view of spec §7.12: the canvas's size in pixels and the zoom, the canvas point (-100, -100) at its top-left corner.
	struct Scenario
	{
		char const* Name = "";
		glm::vec2 Canvas{1600.0f, 900.0f};
		float Zoom = 1.0f;
		// Vertex budgets per visible node and wire at the scenario's level of detail.
		uint32_t NodeVertices = 0;
		uint32_t WireVertices = 0;
		bool LiveWidget = false;
		// The input whose chip the mouse rests on, with LiveWidget.
		std::string_view ChipPin = "Duration";

		glm::vec2 GetCenter() const { return glm::vec2(-100.0f) + Canvas * 0.5f / Zoom; }
	};

	constexpr Scenario T1{"T1", {1600.0f, 900.0f}, 0.224f, 160, 26, false};
	constexpr Scenario T2{"T2", {3600.0f, 2000.0f}, 0.70f, 800, 130, true};
	constexpr Scenario T3{"T3", {1600.0f, 900.0f}, 1.0f, 800, 130, false};
	constexpr Scenario T4{"T4", {1600.0f, 900.0f}, 0.10f, 8, 8, false};

	// The canvas drawn as a graph window draws it (document, display copy, model sync with the fixture library, renderer),
	// in a headless ImGui context with the editor's fonts, for the three graphs of T5 in one project.
	class PerformanceFixture
	{
	public:
		PerformanceFixture()
			: Full(TakeCorner(Testing::MakeGridGraph(FullColumns, FullRows, LinksPerNode, Comments), FullColumns, FullRows)),
			  Project(Testing::MakeGraphProject(Temporary, TakeCorner(Full, FullColumns, 50), "Scripts/Medium.sgraph")),
			  Settings(Temporary.GetPath() / "Views.json")
		{
			Gui.UseEditorStyle();
			Graphs[Medium] = Project.Graph;
			Graphs[Small] = Import(TakeCorner(Full, SmallSide, SmallSide), "Scripts/Small.sgraph");
			Graphs[Large] = Import(Full, "Scripts/Large.sgraph");
			Settings.SetProject(Project.Project->GetFilePath());
			Settings.SetReduceMotion(true);
			for (size_t graph = 0; graph < GraphCount; graph++)
			{
				Models[graph].emplace(Graphs[graph], Settings);
			}
		}

		// Shows a scenario's view of a graph (applied at the next frame; Reduce Motion makes it instant) and lets the canvas
		// settle: the first frames analyze and lay out.
		void Show(size_t graph, Scenario const& scenario)
		{
			VisualScriptOpenRequest request;
			request.Graph = Graphs[graph];
			request.Center = scenario.GetCenter();
			request.Zoom = scenario.Zoom;
			Models[graph]->ApplyOpenRequest(request);
			for (int frame = 0; frame < 4; frame++)
			{
				Frame(graph, scenario);
			}
		}

		// A frame of a graph's canvas: the seconds from the model's sync to the end of the renderer's draw (ImGui's NewFrame,
		// Render and the backend left out).
		double Frame(size_t graph, Scenario const& scenario, bool measurePhases = false)
		{
			Time += 1.0 / 60.0;
			double seconds = 0.0;
			Gui.Frame(
				[&]
				{
					ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
					ImGui::SetNextWindowSize(ImVec2(scenario.Canvas.x, scenario.Canvas.y));
					ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
					ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
					ImGui::Begin("Canvas", nullptr,
				                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
				                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
					ImGui::PopStyleVar(2);
					// The canvas may be larger than the null backend's 1920 × 1080 display: its whole rectangle is the clip.
					ImDrawList* drawList = ImGui::GetWindowDrawList();
					drawList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(scenario.Canvas.x, scenario.Canvas.y), false);
					ImGuiStyle const& style = ImGui::GetStyle();
					VisualScriptCanvasInput input;
					input.Mouse = Mouse.value_or(glm::vec2(-1000.0f));
					input.CanvasMin = glm::vec2(0.0f);
					input.CanvasSize = scenario.Canvas;
					input.UIScale = style.FontScaleDpi;
					input.FontSize = ImGui::GetFontSize();
					input.CurveTessellationTolerance = style.CurveTessellationTol;
					input.Hovered = Mouse.has_value();
					input.Focused = true;
					input.Time = Time;
					VisualScriptDrawInput.DrawList = drawList;
					VisualScriptDrawInput.Style = MakeVisualScriptCanvasStyle(style, ImGui::GetFontSize());
					VisualScriptDrawInput.Library = &Semantics.GetFixtureLibrary();
					VisualScriptDrawInput.Time = Time;
					VisualScriptDrawInput.Mouse = input.Mouse;
					VisualScriptDrawInput.Hovered = input.Hovered;
					VisualScriptDrawInput.ReduceMotion = true;
					VisualScriptDrawInput.Brushes = &Brushes;
					Host.Model = &*Models[graph];
					Host.Library = &Semantics.GetFixtureLibrary();
					VisualScriptDrawInput.LiveChips = !scenario.LiveWidget ? nullptr
				                                      : UseBaseline        ? static_cast<VisualScriptLiveChipHost*>(&Baseline)
				                                                           : static_cast<VisualScriptLiveChipHost*>(&Host);
					VisualScriptDrawInput.MeasurePhases = measurePhases;
					Theme = MakeVisualScriptThemeAnchors(style);

					auto const start = std::chrono::steady_clock::now();
					VisualScriptEditorModel& model = *Models[graph];
					model.Sync(Semantics, Measure, Theme, false, Time);
					std::vector<VisualScriptEditorIntent> const intents = model.Update(input);
					Renderers[graph].Draw(model, VisualScriptDrawInput);
					seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
					CHECK(intents.empty());

					drawList->PopClipRect();
					ImGui::End();
				},
				1.0f / 60.0f);
			return seconds;
		}

		VisualScriptRenderStats const& GetStats(size_t graph) const { return Renderers[graph].GetStats(); }

		// The middle of a value chip of an input (a Delay's Duration, an Add Force's Force or Mode) that T2 shows: an unlinked
		// one, wholly inside the view.
		glm::vec2 FindChip(size_t graph, std::string_view pinName) const
		{
			VisualScriptEditorModel const& model = *Models[graph];
			VisualScriptCanvasModel const& canvas = model.GetCanvas();
			VisualScriptRect const visible = model.GetView().GetVisibleRect();
			for (VisualScriptNodeVisual const& node : canvas.GetNodes())
			{
				std::optional<uint32_t> const index = canvas.FindNode(node.ID);
				std::optional<uint32_t> const pin =
					index ? canvas.FindPin(*index, pinName, VisualScriptPinDirection::In) : std::optional<uint32_t>();
				if (!pin)
				{
					continue;
				}
				VisualScriptRect const chip = VisualScriptGeometry::GetChipRect(node, canvas.GetPins()[*pin]);
				if (!VisualScriptGeometry::IsEmpty(chip) && chip.Min.x > visible.Min.x + 400.0f && chip.Min.y > visible.Min.y + 400.0f &&
				    chip.Max.x < visible.Max.x - 400.0f && chip.Max.y < visible.Max.y - 400.0f)
				{
					return model.GetView().ToScreen(chip.GetCenter());
				}
			}
			FAIL("no chip of an input ", pinName, " in view");
			return glm::vec2(0.0f);
		}

		Testing::HeadlessImGui Gui;
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Temporary;
		VisualScriptDocument Full;
		Testing::GraphProject Project;
		VisualScriptViewSettings Settings;
		Testing::FixtureVisualScriptSemantics Semantics;
		VisualScriptTextMeasurer Measure = Testing::FixedWidthMeasurer(0.5f);
		VisualScriptThemeAnchors Theme;
		std::array<AssetHandle, GraphCount> Graphs{};
		std::array<std::optional<VisualScriptEditorModel>, GraphCount> Models;
		std::array<VisualScriptCanvasRenderer, GraphCount> Renderers;
		VisualScriptBrushes Brushes;
		LiteralChipHost Host;
		CellChipHost Baseline;
		// The live widget is drawn by Baseline instead of Host.
		bool UseBaseline = false;
		VisualScriptCanvasDrawInput VisualScriptDrawInput;
		std::optional<glm::vec2> Mouse;
		double Time = 0.0;

	private:
		AssetHandle Import(VisualScriptDocument const& document, std::string_view path)
		{
			std::filesystem::path const file = Project.Project->GetAssetDirectory() / FileSystem::PathFromUtf8(path);
			REQUIRE(VisualScriptSerializer::SaveToFile(document, file).IsOk());
			Result<AssetHandle> imported = AssetManager::ImportFile(file);
			REQUIRE_MESSAGE(imported.IsOk(), (imported ? std::string() : imported.GetError()));
			return imported.GetValue();
		}
	};

	// The counters every configuration asserts (spec §15.4).
	void CheckCounters(VisualScriptRenderStats const& stats, Scenario const& scenario)
	{
		CAPTURE(scenario.Name);
		CAPTURE(stats.VisibleNodes);
		CAPTURE(stats.VisibleWires);
		CAPTURE(stats.Vertices);
		CAPTURE(stats.DrawCommands);
		CHECK(stats.Index.Candidates <= 2 * (stats.VisibleNodes + stats.VisibleWires) + 256);
		CHECK(stats.Vertices <= scenario.NodeVertices * stats.VisibleNodes + scenario.WireVertices * stats.VisibleWires + 4096);
		uint32_t const segments = (stats.Vertices + 65535) / 65536;
		CHECK(stats.DrawCommands <= segments + 2 * stats.LiveWidgets);
		CHECK(stats.TextSizes <= 6);
		CHECK(stats.LiveWidgets == (scenario.LiveWidget ? 1u : 0u));
	}

	double Median(std::vector<double> values)
	{
		std::sort(values.begin(), values.end());
		return values.empty() ? 0.0 : values[values.size() / 2];
	}

	double Percentile95(std::vector<double> values)
	{
		std::sort(values.begin(), values.end());
		return values.empty() ? 0.0 : values[std::min(values.size() - 1, values.size() * 95 / 100)];
	}

	// The medians the track measured on its machine (track U's report: step U4, and U-fix for T2 with the literal editor's
	// widget and the graph window's frames), in milliseconds; the wall-clock checks allow 3 times them (spec §15.7).
#if defined(ST_DEBUG)
	constexpr double T1Milliseconds = 9.95;
	constexpr double T2Milliseconds = 7.88;
	constexpr double T3Milliseconds = 0.98;
	constexpr double T4Milliseconds = 22.9;
	// The commit frames of a move and of a new link, as track S measured them after its follow-ups (spec §5.8).
	constexpr double CommitFrameMilliseconds = 70.2;
	constexpr double LinkFrameMilliseconds = 3420.0;
	// T2 with the live widget of a vector and of an enum chip (track U's report, step U-fix).
	constexpr double T2VectorMilliseconds = 8.08;
	constexpr double T2EnumMilliseconds = 8.12;
	// The graph window's whole frame (ImGui's included) with the mouse on the live widget of a float, a vector and an enum chip.
	constexpr std::array<double, 3> WindowFrameMilliseconds = {3.08, 3.07, 3.07};
#else
	constexpr double T1Milliseconds = 1.03;
	constexpr double T2Milliseconds = 0.832;
	constexpr double T3Milliseconds = 0.104;
	constexpr double T4Milliseconds = 1.425;
	constexpr double CommitFrameMilliseconds = 13.6;
	constexpr double LinkFrameMilliseconds = 128.0;
	constexpr double T2VectorMilliseconds = 0.821;
	constexpr double T2EnumMilliseconds = 0.829;
	constexpr std::array<double, 3> WindowFrameMilliseconds = {0.447, 0.445, 0.462};
#endif

	// The chips of T2's live widget: a float, a vector and an enum (the grid's Delay and Add Force).
	constexpr std::array<std::string_view, 3> ChipPins = {"Duration", "Force", "Mode"};

	// The grids of the commit frames: 2,000 nodes and 4,000 links always; 200 nodes and 400 links as well for the phase table
	// (spec §5.2) on request.
	struct CommitGrid
	{
		uint32_t Columns = 0;
		uint32_t Rows = 0;
		uint32_t Comments = 0;
	};

	std::vector<CommitGrid> GetCommitGrids()
	{
		std::vector<CommitGrid> grids = {{40, 50, 20}};
		if (Testing::ArePerformanceTestsEnabled())
		{
			grids.push_back({10, 20, 2});
		}
		return grids;
	}

	// A commit frame: its milliseconds and its phases, the preparation's (which the window keeps), the step's (the newest of
	// the history) and those of the model's Syncs in that frame.
	struct CommitFrame
	{
		double Milliseconds = 0.0;
		VisualScriptCommitTimings Phases;
	};

	CommitFrame MakeCommitFrame(Testing::PanelFixture const& fixture, double milliseconds)
	{
		CommitFrame frame;
		frame.Milliseconds = milliseconds;
		frame.Phases = fixture.Panel->GetLastCommitTimings();
		auto const* const step = dynamic_cast<SetVisualScriptCommand const*>(fixture.Context.GetHistory().GetUndoCommand());
		REQUIRE(step != nullptr);
		VisualScriptCommitTimings const& executed = step->GetLastTimings();
		frame.Phases.ReadBack = executed.ReadBack;
		frame.Phases.Compare = executed.Compare;
		frame.Phases.Write = executed.Write;
		frame.Phases.SetDocument = executed.SetDocument;
		VisualScriptCommitTimings const& synced = fixture.Model().GetSyncTimings();
		frame.Phases.Adopt = synced.Adopt;
		frame.Phases.Analysis = synced.Analysis;
		frame.Phases.Canvas = synced.Canvas;
		frame.Phases.Index = synced.Index;
		return frame;
	}

	// The medians of the frames and of each phase, and of what no phase covers (ImGui, the panes, the history).
	std::string DescribeCommitFrames(std::vector<CommitFrame> const& frames)
	{
		auto const median = [&frames](auto const& read)
		{
			std::vector<double> values;
			for (CommitFrame const& frame : frames)
			{
				values.push_back(read(frame));
			}
			return Median(std::move(values));
		};
		auto const phase = [&median](double VisualScriptCommitTimings::* member)
		{
			return median(
				[member](CommitFrame const& frame)
				{
					return frame.Phases.*member;
				});
		};
		double const other = median(
			[](CommitFrame const& frame)
			{
				VisualScriptCommitTimings const& phases = frame.Phases;
				return frame.Milliseconds -
			           (phases.Normalize + phases.Check + phases.Text + phases.ReadBack + phases.Compare + phases.Write +
			            phases.SetDocument + phases.Adopt + phases.Analysis + phases.Canvas + phases.Index);
			});
		return fmt::format("median {:.3f} ms; phases (medians, ms): Normalize {:.3f}, Check {:.3f}, Text {:.3f}, ReadBack {:.3f}, "
		                   "Compare {:.3f}, Write {:.3f}, SetDocument {:.3f}, Adopt {:.3f}, Analysis {:.3f}, Canvas {:.3f}, Index {:.3f}, "
		                   "other {:.3f}",
		                   median(
							   [](CommitFrame const& frame)
							   {
								   return frame.Milliseconds;
							   }),
		                   phase(&VisualScriptCommitTimings::Normalize), phase(&VisualScriptCommitTimings::Check),
		                   phase(&VisualScriptCommitTimings::Text), phase(&VisualScriptCommitTimings::ReadBack),
		                   phase(&VisualScriptCommitTimings::Compare), phase(&VisualScriptCommitTimings::Write),
		                   phase(&VisualScriptCommitTimings::SetDocument), phase(&VisualScriptCommitTimings::Adopt),
		                   phase(&VisualScriptCommitTimings::Analysis), phase(&VisualScriptCommitTimings::Canvas),
		                   phase(&VisualScriptCommitTimings::Index), other);
	}

	// Shows the grid's top-left corner at zoom 1, as the commit frames edit it.
	void ShowGridCorner(Testing::PanelFixture& fixture)
	{
		fixture.CheckDrawingChangesNothing();
		VisualScriptOpenRequest request;
		request.Graph = fixture.Graph;
		request.Center = glm::vec2(400.0f, 300.0f);
		request.Zoom = 1.0f;
		request.Focus = false;
		fixture.Panel->Apply(request);
		fixture.Frames(2);
	}

	// Times a scenario's frames: their median and 95th percentile, and the renderer's phases of the last one, printed.
	double TimeScenario(PerformanceFixture& fixture, size_t graph, Scenario const& scenario)
	{
		constexpr int Frames = 60;
		std::vector<double> times;
		times.reserve(Frames);
		for (int frame = 0; frame < Frames; frame++)
		{
			times.push_back(fixture.Frame(graph, scenario, true) * 1000.0);
		}
		VisualScriptRenderStats const& stats = fixture.GetStats(graph);
		double const median = Median(times);
		MESSAGE(std::string(scenario.Name), ": median ", median, " ms, p95 ", Percentile95(times), " ms; cull ", stats.CullMilliseconds,
		        ", comments ", stats.CommentsMilliseconds, ", wires ", stats.WiresMilliseconds, ", nodes ", stats.NodesMilliseconds,
		        ", overlay ", stats.OverlayMilliseconds, ", minimap ", stats.MinimapMilliseconds, ", renderer ", stats.TotalMilliseconds,
		        " ms; ", stats.VisibleNodes, " nodes, ", stats.VisibleWires, " wires, ", stats.Vertices, " vertices");
		return median;
	}
}

TEST_CASE("VisualScriptEditor: the canvas's frames T1 to T4 stay within their counters and vertex budgets")
{
	PerformanceFixture fixture;
	std::array<Scenario, 4> const scenarios = {T1, T2, T3, T4};
	for (Scenario const& scenario : scenarios)
	{
		fixture.Mouse.reset();
		fixture.Show(Medium, scenario);
		if (scenario.LiveWidget)
		{
			// The mouse over a chip: its live widget.
			fixture.Mouse = fixture.FindChip(Medium, scenario.ChipPin);
			fixture.Frame(Medium, scenario);
		}
		fixture.Frame(Medium, scenario);
		VisualScriptRenderStats const& stats = fixture.GetStats(Medium);
		CheckCounters(stats, scenario);
		if (&scenario == &scenarios[0])
		{
			CHECK(stats.VisibleNodes >= 450);
			CHECK(stats.VisibleNodes <= 550);
		}
		if (&scenario == &scenarios[3])
		{
			// Far, with more wires in view than FarWireLimit: only exec wires are drawn.
			CHECK(stats.VisibleWires > VisualScriptCanvasRenderer::FarWireLimit);
			CHECK(stats.SkippedWires > 0);
		}
		if (Testing::ArePerformanceTestsEnabled())
		{
			double const median = TimeScenario(fixture, Medium, scenario);
			std::array<double, 4> const recorded = {T1Milliseconds, T2Milliseconds, T3Milliseconds, T4Milliseconds};
			CHECK(median <= 3.0 * recorded[static_cast<size_t>(&scenario - scenarios.data())]);
		}
	}
}

TEST_CASE("VisualScriptEditor: T1's view costs the same on graphs of 676, 5,000 and 10,000 nodes, and idle frames reserve nothing "
          "more")
{
	PerformanceFixture fixture;
	// The minimap shows each graph whole, which differs by design (it is drawn from a grid of cells, whatever the graph's
	// size): the counters of the view are compared without it.
	VisualScriptPaneSettings panes = fixture.Settings.GetPanes();
	panes.ShowMinimap = false;
	fixture.Settings.SetPanes(panes);
	std::array<VisualScriptRenderStats, GraphCount> stats{};
	for (size_t graph = 0; graph < GraphCount; graph++)
	{
		fixture.Show(graph, T1);
		fixture.Frame(graph, T1);
		stats[graph] = fixture.GetStats(graph);
		CheckCounters(stats[graph], T1);
		MESSAGE("T5 graph ", graph, ": ", stats[graph].VisibleNodes, " nodes, ", stats[graph].VisibleWires, " wires, ",
		        stats[graph].VisibleComments, " comments, ", stats[graph].Vertices, " vertices, ", stats[graph].Index.Candidates,
		        " candidates");
	}
	// The same view holds the same elements, and draws them alike.
	auto const isNear = [](uint32_t value, uint32_t reference)
	{
		return std::abs(static_cast<double>(value) - static_cast<double>(reference)) <= 0.02 * static_cast<double>(reference);
	};
	for (size_t graph = 0; graph < GraphCount; graph++)
	{
		CAPTURE(graph);
		CHECK(stats[graph].VisibleNodes == stats[Medium].VisibleNodes);
		CHECK(stats[graph].VisibleWires == stats[Medium].VisibleWires);
		CHECK(stats[graph].VisibleComments == stats[Medium].VisibleComments);
		CHECK(isNear(stats[graph].Vertices, stats[Medium].Vertices));
		CHECK(isNear(stats[graph].Indices, stats[Medium].Indices));
		CHECK(isNear(stats[graph].Index.Candidates, stats[Medium].Index.Candidates));
		CHECK(stats[graph].DrawCommands == stats[Medium].DrawCommands);
	}

	// Ten idle frames of the 10,000-node graph, its minimap shown: the renderer, the model and its index reserve nothing more.
	panes.ShowMinimap = true;
	fixture.Settings.SetPanes(panes);
	for (size_t graph = 0; graph < GraphCount; graph++)
	{
		fixture.Show(graph, T1);
		CHECK(fixture.Models[graph]->GetMinimap().Alpha > 0.0f);
	}
	size_t const renderer = fixture.Renderers[Large].GetCapacityBytes();
	size_t const model = fixture.Models[Large]->GetCapacityBytes();
	size_t const index = fixture.Models[Large]->GetIndex().GetCapacityBytes();
	for (int frame = 0; frame < 10; frame++)
	{
		fixture.Frame(Large, T1);
	}
	CHECK(fixture.Renderers[Large].GetCapacityBytes() == renderer);
	CHECK(fixture.Models[Large]->GetCapacityBytes() == model);
	CHECK(fixture.Models[Large]->GetIndex().GetCapacityBytes() == index);

	if (Testing::ArePerformanceTestsEnabled())
	{
		// Interleaved frame by frame, so the machine's state weighs on the three alike.
		constexpr int Frames = 90;
		std::array<std::vector<double>, GraphCount> times;
		for (int frame = 0; frame < Frames; frame++)
		{
			for (size_t graph = 0; graph < GraphCount; graph++)
			{
				times[graph].push_back(fixture.Frame(graph, T1) * 1000.0);
			}
		}
		std::array<double, GraphCount> medians{};
		for (size_t graph = 0; graph < GraphCount; graph++)
		{
			medians[graph] = Median(times[graph]);
		}
		MESSAGE("T5: medians ", medians[Small], " / ", medians[Medium], " / ", medians[Large], " ms; p95 ", Percentile95(times[Small]),
		        " / ", Percentile95(times[Medium]), " / ", Percentile95(times[Large]), " ms");
		CHECK(medians[Large] <= 1.25 * medians[Small]);
		CHECK(medians[Medium] <= 1.25 * medians[Small]);
	}
}

TEST_CASE("VisualScriptEditor: the frame that commits a move in a graph of 2,000 nodes")
{
	// Spec §15.7 and ruling R3.3: the commit (pipeline, serializer, write) is the graph window's slowest frame; it is
	// measured here as the window draws it, ImGui's frame included. On request seven moves of each grid, with their phases.
	bool const performance = Testing::ArePerformanceTestsEnabled();
	for (CommitGrid const grid : GetCommitGrids())
	{
		uint32_t const nodes = grid.Columns * grid.Rows;
		CAPTURE(nodes);
		Testing::PanelFixture fixture(Testing::MakeGridGraph(grid.Columns, grid.Rows, 2, grid.Comments));
		ShowGridCorner(fixture);
		std::vector<CommitFrame> frames;
		for (int commit = 0, commits = performance ? 7 : 1; commit < commits; commit++)
		{
			size_t const undo = fixture.GetUndoCount();
			uint64_t const analyses = fixture.Model().GetAnalysisCount();
			uint64_t const positionSyncs = fixture.Model().GetCanvas().GetPositionSyncCount();
			glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), 1, glm::vec2(0.5f, 0.06f));
			fixture.MoveMouse(from);
			fixture.SetButton(ImGuiMouseButton_Left, true);
			for (int step = 1; step <= 4; step++)
			{
				fixture.MoveMouse(from + glm::vec2(30.0f, 20.0f) * static_cast<float>(step));
			}
			auto const start = std::chrono::steady_clock::now();
			fixture.SetButton(ImGuiMouseButton_Left, false);
			double const milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			REQUIRE(fixture.GetUndoCount() == undo + 1);
			frames.push_back(MakeCommitFrame(fixture, milliseconds));
			// A move leaves the program as it was (a layout-only commit): nothing is analyzed, and the canvas moves the node
			// instead of syncing.
			CHECK(fixture.Model().GetAnalysisCount() == analyses);
			CHECK(fixture.Model().GetCanvas().GetPositionSyncCount() == positionSyncs + 1);
			// Its phases are filled: the window's preparation, the step's file read and write, and the model's moving of the
			// node and its new index (the adopt and the analysis can take less than the clock's resolution).
			VisualScriptCommitTimings const& phases = frames.back().Phases;
			CHECK(phases.Normalize > 0.0);
			CHECK(phases.Check > 0.0);
			CHECK(phases.Text > 0.0);
			CHECK(phases.ReadBack > 0.0);
			CHECK(phases.Write > 0.0);
			CHECK(phases.Adopt >= 0.0);
			CHECK(phases.Analysis >= 0.0);
			CHECK(phases.Canvas > 0.0);
			CHECK(phases.Index > 0.0);
			// The model's start again with the next frame: one with nothing to lay out spends nothing on the canvas.
			fixture.Frames(1);
			CHECK(fixture.Model().GetSyncTimings().Canvas == 0.0);
			CHECK(fixture.Model().GetSyncTimings().Index == 0.0);
		}
		if (performance)
		{
			MESSAGE("commit frame of a move, ", nodes, " nodes and ", nodes * 2, " links: ", DescribeCommitFrames(frames));
			if (nodes == 2000)
			{
				std::vector<double> milliseconds;
				for (CommitFrame const& frame : frames)
				{
					milliseconds.push_back(frame.Milliseconds);
				}
				CHECK(Median(milliseconds) <= 3.0 * CommitFrameMilliseconds);
			}
		}
	}
}

TEST_CASE("VisualScriptEditor: the frame that commits a new link in a graph of 2,000 nodes analyzes it with the scripting runtime")
{
	// Spec §5.2: a program-changing commit also pays the analysis (the document's text, the runtime, the parse of its
	// answer). A link from an addition's result to a delay's duration, undone between commits.
	Testing::ScriptRuntimeScope runtime;
	EngineVisualScriptSemantics engine;
	bool const performance = Testing::ArePerformanceTestsEnabled();
	VisualScriptLink const link{{2, "Result"}, {3, "Duration"}};
	for (CommitGrid const grid : GetCommitGrids())
	{
		uint32_t const nodes = grid.Columns * grid.Rows;
		CAPTURE(nodes);
		Testing::PanelFixture fixture(Testing::MakeGridGraph(grid.Columns, grid.Rows, 2, grid.Comments), &engine);
		ShowGridCorner(fixture);
		REQUIRE(fixture.Model().GetAnalysis() != nullptr);
		REQUIRE_FALSE(fixture.HasLink(link));
		std::vector<CommitFrame> frames;
		for (int commit = 0, commits = performance ? 7 : 1; commit < commits; commit++)
		{
			size_t const undo = fixture.GetUndoCount();
			uint64_t const analyses = fixture.Model().GetAnalysisCount();
			glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), link.From.Node, link.From.Pin, VisualScriptPinDirection::Out);
			glm::vec2 const to = Testing::CanvasPoint(fixture.Model(), link.To.Node, link.To.Pin);
			fixture.MoveMouse(from);
			fixture.SetButton(ImGuiMouseButton_Left, true);
			fixture.MoveMouse((from + to) * 0.5f);
			fixture.MoveMouse(to);
			auto const start = std::chrono::steady_clock::now();
			fixture.SetButton(ImGuiMouseButton_Left, false);
			double const milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			REQUIRE(fixture.GetUndoCount() == undo + 1);
			REQUIRE(fixture.HasLink(link));
			// The frame that commits analyzes the new program.
			CHECK(fixture.Model().GetAnalysisCount() == analyses + 1);
			frames.push_back(MakeCommitFrame(fixture, milliseconds));
			CHECK(frames.back().Phases.Check > 0.0);
			CHECK(frames.back().Phases.Text > 0.0);
			CHECK(frames.back().Phases.Adopt > 0.0);
			CHECK(frames.back().Phases.Analysis > 0.0);
			CHECK(frames.back().Phases.Canvas > 0.0);
			CHECK(frames.back().Phases.Index > 0.0);

			// Undone, which the window analyzes in its next frame, for the next commit.
			REQUIRE(fixture.Operations.Undo().IsOk());
			fixture.Frames(2);
			REQUIRE_FALSE(fixture.HasLink(link));
		}
		if (performance)
		{
			MESSAGE("commit frame of a new link, ", nodes, " nodes and ", nodes * 2, " links: ", DescribeCommitFrames(frames));
			if (nodes == 2000)
			{
				std::vector<double> milliseconds;
				for (CommitFrame const& frame : frames)
				{
					milliseconds.push_back(frame.Milliseconds);
				}
				CHECK(Median(milliseconds) <= 3.0 * LinkFrameMilliseconds);
			}
		}
	}
}

TEST_CASE("VisualScriptEditor: T2's live widget is the literal editor's compact form for a float, a vector and an enum chip")
{
	PerformanceFixture fixture;
	std::array<double, 3> const recorded = {T2Milliseconds, T2VectorMilliseconds, T2EnumMilliseconds};
	for (size_t chip = 0; chip < ChipPins.size(); chip++)
	{
		Scenario scenario = T2;
		scenario.ChipPin = ChipPins[chip];
		CAPTURE(scenario.ChipPin);
		fixture.Mouse.reset();
		fixture.Show(Medium, scenario);
		fixture.Mouse = fixture.FindChip(Medium, scenario.ChipPin);
		fixture.Frame(Medium, scenario);
		fixture.Frame(Medium, scenario);
		CheckCounters(fixture.GetStats(Medium), scenario);
		if (Testing::ArePerformanceTestsEnabled())
		{
			double const median = TimeScenario(fixture, Medium, scenario);
			fixture.UseBaseline = true;
			double const baseline = TimeScenario(fixture, Medium, scenario);
			fixture.UseBaseline = false;
			MESSAGE("T2 on ", scenario.ChipPin, ": the literal editor ", median, " ms, a plain drag per cell ", baseline, " ms");
			CHECK(median <= 3.0 * recorded[chip]);
		}
	}
}

namespace
{
	// The middle of a value chip of an input well inside the view and away from its bottom-right corner (the minimap's).
	std::optional<glm::vec2> FindChipInView(VisualScriptEditorModel const& model, std::string_view pinName)
	{
		VisualScriptCanvasModel const& canvas = model.GetCanvas();
		std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
		std::span<VisualScriptPinVisual const> const pins = canvas.GetPins();
		VisualScriptRect const visible = model.GetView().GetVisibleRect();
		glm::vec2 const lower = visible.Min + visible.GetSize() * 0.1f;
		glm::vec2 const upper = visible.Min + visible.GetSize() * 0.9f;
		glm::vec2 const corner = visible.Min + visible.GetSize() * 0.55f;
		for (uint32_t index = 0; index < nodes.size(); index++)
		{
			std::optional<uint32_t> const pin = canvas.FindPin(index, pinName, VisualScriptPinDirection::In);
			if (!pin)
			{
				continue;
			}
			VisualScriptRect const chip = VisualScriptGeometry::GetChipRect(nodes[index], pins[*pin]);
			bool const inside = chip.Min.x > lower.x && chip.Min.y > lower.y && chip.Max.x < upper.x && chip.Max.y < upper.y;
			bool const underMinimap = chip.Max.x > corner.x && chip.Max.y > corner.y;
			if (!VisualScriptGeometry::IsEmpty(chip) && inside && !underMinimap)
			{
				return model.GetView().ToScreen(chip.GetCenter());
			}
		}
		return std::nullopt;
	}
}

TEST_CASE("VisualScriptEditor: the graph window's frame with the mouse resting on the live widget of a float, a vector and an enum chip")
{
	// T2's zoom through the production path: the window with its panes, its live widget (the literal editor's compact form)
	// and what it resolves for it, ImGui's frame included.
	Testing::PanelFixture fixture(Testing::MakeGridGraph(30, 30, LinksPerNode, 10));
	fixture.WindowSize = ImVec2(1920.0f, 1080.0f);
	VisualScriptOpenRequest request;
	request.Graph = fixture.Graph;
	request.Center = glm::vec2(1600.0f, 1000.0f);
	request.Zoom = T2.Zoom;
	request.Focus = false;
	fixture.Panel->Apply(request);
	fixture.Frames(3);
	fixture.CheckDrawingChangesNothing();
	for (size_t chip = 0; chip < ChipPins.size(); chip++)
	{
		CAPTURE(ChipPins[chip]);
		std::optional<glm::vec2> const point = FindChipInView(fixture.Model(), ChipPins[chip]);
		REQUIRE(point.has_value());
		fixture.MoveMouse(*point);
		fixture.Frames(2);
		REQUIRE(fixture.Panel->GetLiveWidget().has_value());
		CHECK(fixture.Panel->GetRenderer().GetStats().LiveWidgets == 1);
		fixture.CheckDrawingChangesNothing();
		if (Testing::ArePerformanceTestsEnabled())
		{
			constexpr int Frames = 60;
			std::vector<double> times;
			times.reserve(Frames);
			for (int frame = 0; frame < Frames; frame++)
			{
				auto const start = std::chrono::steady_clock::now();
				fixture.Frames(1);
				times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			}
			double const median = Median(times);
			MESSAGE("the graph window's frame on the live widget of ", ChipPins[chip], ": median ", median, " ms, p95 ",
			        Percentile95(times), " ms");
			CHECK(median <= 3.0 * WindowFrameMilliseconds[chip]);
		}
	}
}
