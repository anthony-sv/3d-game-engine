#include "HeadlessImGui.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/GraphDebugCommands.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/GameInputRouter.h"
#include "Editor/Panels/VisualScriptDebuggerPanel.h"
#include "Editor/Panels/VisualScriptEditorPanel.h"
#include "Editor/PlayMode.h"
#include "Editor/VisualScripting/VisualScriptCanvasRenderer.h"
#include "Editor/VisualScripting/VisualScriptController.h"
#include "Editor/VisualScripting/VisualScriptDebugControls.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"
#include "Editor/VisualScripting/VisualScriptEditors.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace Strada;

// The debugger in the editor's UI (spec-slice4 §13.6-§13.8, §14): graph windows following stops, their strips, pills, markers
// and keys, the Debugger panel, and the debug controls of the toolbar, the Play menu and the global keys, with the scripting
// runtime and play mode as the editor runs them (headless ImGui).
namespace
{
	constexpr float FrameTime = 1.0f / 60.0f;

	VisualScriptNode MakeNode(uint32_t id, std::string type, glm::vec2 position, Json settings = Json::object(),
	                          Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = position;
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	// On every update: Count += 1 at node 11, then Steps += 1 at node 14 (each Set's Add and Get take the next two IDs).
	VisualScriptDocument MakeCounterDocument()
	{
		VisualScriptDocument document;
		for (uint32_t const id : {1u, 2u})
		{
			VisualScriptVariable variable;
			variable.ID = id;
			variable.Name = id == 1 ? "Count" : "Steps";
			variable.Type = "int";
			variable.Value = 0;
			variable.Public = true;
			document.Variables.push_back(std::move(variable));
		}
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update", glm::vec2(0.0f, 0.0f)));
		for (auto const& [set, variable, x] :
		     {std::tuple<uint32_t, uint32_t, float>{11, 1, 320.0f}, std::tuple<uint32_t, uint32_t, float>{14, 2, 760.0f}})
		{
			document.Graph.Nodes.push_back(MakeNode(set, "Variable.Set", glm::vec2(x, 0.0f), Json::object({{"Variable", variable}})));
			document.Graph.Nodes.push_back(
				MakeNode(set + 1, "Operator.Add", glm::vec2(x - 160.0f, 160.0f), Json::object(), Json::object({{"B", 1}})));
			document.Graph.Nodes.push_back(
				MakeNode(set + 2, "Variable.Get", glm::vec2(x - 320.0f, 160.0f), Json::object({{"Variable", variable}})));
			document.Graph.Links.push_back(MakeLink(set + 2, "Value", set + 1, "A"));
			document.Graph.Links.push_back(MakeLink(set + 1, "Result", set, "Value"));
		}
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(11, "Then", 14, "Execute"));
		return document;
	}

	VisualScriptBreakpoint MakeBreakpoint(uint32_t node)
	{
		VisualScriptBreakpoint breakpoint;
		breakpoint.Node = node;
		return breakpoint;
	}

	// The editor's debugger as EditorLayer wires it, with its graph windows, Debugger panel and debug controls drawn headless.
	struct DebuggerFixture
	{
		Testing::HeadlessImGui Gui;
		Testing::ScriptRuntimeScope Runtime;
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};
		VisualScriptDebugSession Session{Context, Play};
		EngineVisualScriptSemantics Semantics;
		VisualScriptViewSettings Settings{Directory.GetPath() / "UserData" / "Editor" / "VisualScriptViews.json"};
		VisualScriptEditors Editors{Operations, Semantics, Settings};
		GameInputRouter Input;
		VisualScriptDebugControls Controls{Session, Play, Input};
		VisualScriptDebuggerPanel Debugger{Session, Context};
		CommandRegistry Registry;
		bool GameViewVisible = false;
		// A modal popup drawn over everything while set (a dialog the keys must not act behind).
		bool ShowModal = false;
		VisualScriptDrawRecord Record;

		DebuggerFixture()
		{
			Gui.UseEditorStyle(true);
			Settings.SetHintSeen(VisualScriptCanvasHint);
			Settings.SetReduceMotion(true);
			EditorCommandEnvironment environment;
			environment.GraphEditors = [this]() -> VisualScriptEditors&
			{
				return Editors;
			};
			environment.DebuggerPanel = [this]() -> VisualScriptDebuggerPanel&
			{
				return Debugger;
			};
			REQUIRE(RegisterEditorCommands(Registry, Operations, std::move(environment)).IsOk());
			REQUIRE(RegisterPlayCommands(Registry, Play, Context,
			                             []
			                             {
											 return glm::uvec2(640, 480);
										 })
			            .IsOk());
			REQUIRE(RegisterGraphDebugCommands(Registry, Session, Play, Context).IsOk());
			Context.SetAssetReferenceResolver(
				[](std::string_view reference)
				{
					return AssetManager::ResolveReference(reference);
				});
			REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			Play.SetObserver(&Session);
			// As EditorLayer::ConnectDebugger.
			Session.SetViewSettings(&Settings);
			Editors.SetDebugSession(&Session);
			Session.SetStopListener(
				[this](VisualScriptStop const& stop)
				{
					Editors.FollowStop(stop);
					Debugger.OnStop(stop);
					Controls.OnStop(stop);
				});
			Session.SetResumeListener(
				[this]
				{
					Controls.OnResume();
				});
			Session.SetStopEndedListener(
				[this](AssetHandle graph, std::string const& reason)
				{
					Editors.OnStopEnded(graph, reason);
				});
			Session.SetTargetListener(
				[this](AssetHandle graph, UUID entity)
				{
					Editors.OnTargetChanged(graph, entity);
				});
			Debugger.SetFollowFrame(
				[this](VisualScriptStop const& stop, uint32_t frame)
				{
					Editors.FollowFrame(stop, frame);
				});
			VisualScriptDebugControlsHost host;
			host.IsGameViewVisible = [this]
			{
				return GameViewVisible;
			};
			Controls.SetHost(std::move(host));
		}

		~DebuggerFixture()
		{
			Play.Stop();
			Play.SetObserver(nullptr);
			Editors.SetDebugSession(nullptr);
			Session.SetViewSettings(nullptr);
		}

		DebuggerFixture(DebuggerFixture const&) = delete;
		DebuggerFixture& operator=(DebuggerFixture const&) = delete;

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

		AssetHandle CreateGraph(std::string const& path, VisualScriptDocument const& document)
		{
			std::string const graph = Ok("graph.create", Json::object({{"path", path}}))["asset"].get<std::string>();
			Ok("graph.set", Json::object({{"graph", graph}, {"document", VisualScriptSerializer::SerializeDocument(document)}}));
			Result<AssetHandle> const handle = AssetManager::ResolveReference(graph);
			REQUIRE(handle.IsOk());
			return handle.GetValue();
		}

		UUID CreateRunner(std::string name, AssetHandle graph)
		{
			Json const created =
				Ok("entity.create",
			       Json::object({{"name", std::move(name)},
			                     {"components", Json::object({{"VisualScript", Json::object({{"Graph", graph.ToString()}})}})}}));
			std::optional<UUID> const id = UUID::FromString(created["id"].get<std::string>());
			REQUIRE(id.has_value());
			return *id;
		}

		void Open(AssetHandle graph)
		{
			VisualScriptOpenRequest request;
			request.Graph = graph;
			REQUIRE(Editors.Open(request).IsOk());
			Frames(3);
		}

		void Start()
		{
			Result<void> const started = Play.Start(EditorPlayState::Play, 640, 480, nullptr, PlayStartOptions());
			REQUIRE_MESSAGE(started.IsOk(), (started ? std::string() : started.GetError()));
		}

		// What the editor draws: the graph windows, the Debugger panel, a stand-in of the toolbar, the debug keys.
		void Draw()
		{
			ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoSavedSettings);
			ImGui::TextUnformatted("Play");
			Controls.DrawToolbarButtons();
			ImGui::End();
			Editors.OnImGuiRender();
			Debugger.OnImGuiRender(std::nullopt);
			if (ShowModal)
			{
				ImGui::OpenPopup("Modal");
			}
			if (ImGui::BeginPopupModal("Modal", nullptr, ImGuiWindowFlags_NoSavedSettings))
			{
				ImGui::TextUnformatted("A dialog");
				if (!ShowModal)
				{
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
			Controls.HandleKeys();
		}

		// An editor frame: the queued resumes, play mode's update, the session's end, then ImGui's draw.
		void EditorFrame()
		{
			Session.BeginFrame();
			Play.Update(Timestep(FrameTime));
			Session.EndFrame();
			Record.Clear();
			Gui.Frame(
				[this]
				{
					Draw();
				});
		}

		void Frames(int count)
		{
			for (int i = 0; i < count; i++)
			{
				EditorFrame();
			}
		}

		// Editor frames until an execution stops (at most 30).
		void FramesUntilStopped()
		{
			for (int i = 0; i < 30 && Session.GetSessionState() != VisualScriptDebugSessionState::Stopped; i++)
			{
				EditorFrame();
			}
			REQUIRE(Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
			// The window that came forward draws the stop.
			Frames(2);
		}

		VisualScriptEditorPanel& Window(AssetHandle graph)
		{
			VisualScriptEditorPanel* panel = Editors.FindPanel(graph);
			REQUIRE(panel != nullptr);
			return *panel;
		}

		std::function<void()> Drawer()
		{
			return [this]
			{
				Draw();
			};
		}

		// Where a node's top-left corner is on the screen (the breakpoint marker's centre).
		ImVec2 GetNodeCorner(VisualScriptEditorPanel const& panel, uint32_t node) const
		{
			VisualScriptCanvasModel const& canvas = panel.GetModel().GetCanvas();
			std::optional<uint32_t> const index = canvas.FindNode(node);
			REQUIRE(index.has_value());
			glm::vec2 const corner = panel.GetModel().GetView().ToScreen(canvas.GetNodes()[*index].Position);
			return ImVec2(corner.x, corner.y);
		}

		uint64_t GetFrame() const { return Context.GetScene().GetRuntimeFrame(); }
	};
}

TEST_CASE("VisualScriptDebug: a stop brings its window forward on the stopped node, drawn with its marker, instruction and strip")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Player", graph);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.Input.Grant();
	REQUIRE(fixture.Input.HasInput());
	fixture.FramesUntilStopped();

	// Following the stop opened the window, which shows the stop's node; the Debugger panel came forward on its call stack.
	VisualScriptEditorPanel& window = fixture.Window(graph);
	VisualScriptEditorState const state = window.GetState();
	CHECK(state.Visible);
	CHECK(state.DebugState == VisualScriptWindowDebugState::Stopped);
	CHECK(state.DebugNode == std::optional<uint32_t>(11));
	CHECK(state.DebugFrame == 0);
	CHECK(state.Breakpoints == 1);
	CHECK(std::find(state.Notices.begin(), state.Notices.end(), VisualScriptNotice::Debug) != state.Notices.end());
	CHECK(fixture.Debugger.IsOpen());
	CHECK(fixture.Debugger.GetTab() == VisualScriptDebuggerTab::CallStack);
	// The game gave the input back, so the debug keys reach the editor.
	CHECK_FALSE(fixture.Input.HasInput());
	CHECK(fixture.Controls.HasTakenInput());

	// The canvas view: the marker that caused the stop, the instruction at its exec input, and the computed path.
	VisualScriptCanvasDebugView const* view = window.GetDebugView();
	REQUIRE(view != nullptr);
	REQUIRE(view->Breakpoints.size() == 1);
	CHECK(view->Breakpoints[0].Node == 11);
	CHECK(view->Breakpoints[0].CausedStop);
	REQUIRE(view->Instruction.has_value());
	CHECK(view->Instruction->Node == 11);
	CHECK(view->Instruction->Place == VisualScriptStopPlace::Enter);
	CHECK_FALSE(view->Instruction->ReadOnly);
	CHECK(view->BreakpointChord == "F9");

	// The strip's line and buttons, and the live pill's STOPPED segment with its tooltip.
	VisualScriptWindowDebugInput const* input = fixture.Editors.FindDebugInput(graph);
	REQUIRE(input != nullptr);
	REQUIRE_FALSE(input->Strip.empty());
	CHECK(input->Strip[0].Text.starts_with("Stopped at "));
	CHECK(input->Strip[0].Text.find("(node 11) on Player (") != std::string::npos);
	CHECK(input->Strip[0].Text.ends_with(": breakpoint"));
	REQUIRE(input->Strip[0].Buttons.size() == 4);
	CHECK(input->Strip[0].Buttons[0].Label == "Continue");
	CHECK(input->Strip[0].Buttons[0].Chord == "F5");
	CHECK(input->Strip[0].Buttons[3].Chord == "Shift+F11");
	CHECK(input->StoppedHere);
	CHECK(input->StoppedTooltip.ends_with("Edits apply at once and reach the stopped execution when they keep its nodes."));
	CHECK(window.GetItemRect(VisualScriptPanelItem::Stopped).has_value());
	CHECK(window.GetItemRect(VisualScriptPanelItem::StripContinue).has_value());
	CHECK(window.GetItemRect(VisualScriptPanelItem::StripStepOut).has_value());

	// In the draw record: the marker (Mark: its state), the instruction (Mark: its place), and the computed glows.
	window.SetDrawRecord(&fixture.Record);
	fixture.Frames(1);
	VisualScriptDrawRecordItem const* marker = fixture.Record.Find(VisualScriptDrawItemKind::Breakpoint, 11);
	REQUIRE(marker != nullptr);
	CHECK(marker->Mark == static_cast<uint8_t>(VisualScriptBreakpointMarkState::Enabled));
	VisualScriptDrawRecordItem const* instruction = fixture.Record.Find(VisualScriptDrawItemKind::Instruction, 11);
	REQUIRE(instruction != nullptr);
	CHECK(instruction->Mark == static_cast<uint8_t>(VisualScriptStopPlace::Enter));
	window.SetDrawRecord(nullptr);

	// graph.editors reports the same.
	Json const editors = fixture.Ok("graph.editors");
	REQUIRE(editors["editors"].size() == 1);
	Json const& described = editors["editors"][0];
	CHECK(described["debug"]["state"] == "Stopped");
	CHECK(described["debug"]["node"] == 11);
	CHECK(described["debug"]["frame"] == 0);
	CHECK(described["breakpoints"] == 1);
	CHECK(described["live"] == "Live");
	CHECK(std::find(described["notices"].begin(), described["notices"].end(), Json("Debug")) != described["notices"].end());
	CHECK(editors["debugPanel"]["open"] == true);
	CHECK(editors["debugPanel"]["tab"] == "CallStack");
	CHECK(editors["settings"]["wireValues"] == "WhileStopped");
	CHECK(editors["settings"]["showFlow"] == true);
}

TEST_CASE("VisualScriptDebug: F9 and Ctrl+F9 on the selection, and a click on a marker, change breakpoints and nothing else")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.Open(graph);
	VisualScriptEditorPanel& window = fixture.Window(graph);
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	// graph.open focuses the window, whose canvas then has the keyboard.
	fixture.Ok("graph.open", Json::object({{"graph", graph.ToString()}, {"select", Json::array({11, 14})}}));
	fixture.Frames(2);
	REQUIRE(window.GetFocusedPart() == VisualScriptWindowPart::Canvas);

	fixture.Gui.Press(ImGuiKey_F9, fixture.Drawer());
	VisualScriptBreakpointStore const& store = fixture.Session.GetBreakpoints();
	CHECK(store.Find(graph, 11) != nullptr);
	CHECK(store.Find(graph, 14) != nullptr);
	// Ctrl+F9 disables them; again enables them.
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_F9, fixture.Drawer());
	REQUIRE(store.Find(graph, 11) != nullptr);
	CHECK_FALSE(store.Find(graph, 11)->Enabled);
	CHECK_FALSE(store.Find(graph, 14)->Enabled);
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_F9, fixture.Drawer());
	CHECK(store.Find(graph, 11)->Enabled);
	// F9 again removes them all.
	fixture.Gui.Press(ImGuiKey_F9, fixture.Drawer());
	CHECK(store.Get(graph).empty());

	// A click on node 11's marker corner adds its breakpoint once; a right click there opens its popup.
	fixture.Gui.Click(fixture.GetNodeCorner(window, 11), fixture.Drawer());
	fixture.Frames(1);
	CHECK(store.Find(graph, 11) != nullptr);
	CHECK(store.Get(graph).size() == 1);
	fixture.Gui.MouseClick(fixture.GetNodeCorner(window, 11), ImGuiMouseButton_Right, fixture.Drawer());
	fixture.Frames(1);
	CHECK(window.GetBreakpointPopupNode() == std::optional<uint32_t>(11));
	CHECK(store.Get(graph).size() == 1);

	// Breakpoints are view state: no undo step, and the views settings keep them per project.
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
	CHECK(fixture.Settings.GetBreakpoints(fixture.Context.GetProject()->GetFilePath(), graph).size() == 1);
}

TEST_CASE("VisualScriptDebug: the node menu offers the breakpoint items, Run to Here while playing")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.Open(graph);
	VisualScriptEditorPanel& window = fixture.Window(graph);
	fixture.Ok("graph.open", Json::object({{"graph", graph.ToString()}, {"select", Json::array({11})}}));
	fixture.Frames(2);

	VisualScriptEditContext context(window.GetModel());
	context.Debug = fixture.Editors.FindDebugInput(graph);
	REQUIRE(context.Debug != nullptr);
	auto const labels = [&context]
	{
		std::vector<std::pair<std::string, bool>> found;
		for (VisualScriptMenuItem const& item : BuildVisualScriptNodeMenu(context))
		{
			if (item.Action == VisualScriptEditorAction::ToggleBreakpoint ||
			    item.Action == VisualScriptEditorAction::ToggleBreakpointEnabled ||
			    item.Action == VisualScriptEditorAction::BreakpointSettings || item.Action == VisualScriptEditorAction::RunToNode)
			{
				found.emplace_back(item.Label, item.Enabled);
			}
		}
		return found;
	};
	using Items = std::vector<std::pair<std::string, bool>>;
	CHECK(labels() ==
	      Items{{"Add Breakpoint", true}, {"Disable Breakpoint", false}, {"Breakpoint Settings...", true}, {"Run to Here", false}});
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Frames(1);
	context.Debug = fixture.Editors.FindDebugInput(graph);
	CHECK(labels() ==
	      Items{{"Remove Breakpoint", true}, {"Disable Breakpoint", true}, {"Breakpoint Settings...", true}, {"Run to Here", false}});
	// Without a debug session the menu has none of them.
	context.Debug = nullptr;
	CHECK(labels().empty());

	// The popup lists what the condition reads, and its edits reach the session.
	window.OpenBreakpointPopup(11);
	fixture.Frames(2);
	CHECK(window.GetBreakpointPopupNode() == std::optional<uint32_t>(11));
}

TEST_CASE("VisualScriptDebug: the keys, the strip and Pause queue resumes for the next frame, and never act behind a modal")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Player", graph);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.FramesUntilStopped();
	uint64_t const frame = fixture.GetFrame();

	// While stopped the frame step is refused with the reason, and the Pause toggle says it continues.
	CHECK_FALSE(fixture.Controls.CanStepFrame());
	CHECK(fixture.Controls.GetStepRefusal() == std::optional<std::string>(std::string(VisualScriptDebugSession::RefusalForUI)));
	CHECK(fixture.Controls.GetPauseTooltip() == VisualScriptDebugControls::Texts::PauseWhileStopped);
	fixture.Controls.StepFrame();
	CHECK(fixture.GetFrame() == frame);

	// F10 behind a modal does nothing.
	fixture.ShowModal = true;
	fixture.Frames(2);
	fixture.Gui.Press(ImGuiKey_F10, fixture.Drawer());
	fixture.Gui.Press(ImGuiKey_F5, fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(fixture.Session.GetState().Current == fixture.Session.GetSelectedStop());
	fixture.ShowModal = false;
	fixture.Frames(2);

	// F10 steps over: queued while ImGui draws, run at the next frame's BeginFrame, which stops at node 14 with no scene frame.
	std::optional<uint64_t> const first = fixture.Session.GetState().Current;
	fixture.Gui.Press(ImGuiKey_F10, fixture.Drawer());
	CHECK(fixture.Session.GetState().Current == first);
	fixture.Frames(2);
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(fixture.Session.GetState().Current != first);
	CHECK(fixture.GetFrame() == frame);
	CHECK(fixture.Window(graph).GetState().DebugNode == std::optional<uint32_t>(14));
	// The step ran node 11: the frame's values show on the wire that brought its value (the Add's Result).
	VisualScriptCanvasDebugView const* stepped = fixture.Window(graph).GetDebugView();
	REQUIRE(stepped != nullptr);
	CHECK_FALSE(stepped->Values.empty());

	// The Pause toggle while stopped queues Continue then the unpause: nothing runs until the next BeginFrame.
	fixture.Controls.TogglePause();
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	fixture.Frames(1);
	// The breakpoint stops the next frame again: Continue resumed play, and the frame ran.
	fixture.FramesUntilStopped();
	CHECK(fixture.GetFrame() > frame);

	// The strip's Continue is F5's: every stop, then the game.
	VisualScriptEditorPanel& window = fixture.Window(graph);
	std::optional<VisualScriptRect> const button = window.GetItemRect(VisualScriptPanelItem::StripContinue);
	REQUIRE(button.has_value());
	std::array<uint32_t, 1> const nodes = {11};
	fixture.Session.SetBreakpointsEnabled(graph, nodes, false);
	fixture.Gui.Click(ImVec2((button->Min.x + button->Max.x) * 0.5f, (button->Min.y + button->Max.y) * 0.5f), fixture.Drawer());
	fixture.Frames(3);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	CHECK_FALSE(fixture.Play.IsPaused());
}

TEST_CASE("VisualScriptDebug: the game's input is taken at a stop and given back on Continue only where its view shows")
{
	for (bool const visible : {true, false})
	{
		CAPTURE(visible);
		DebuggerFixture fixture;
		fixture.GameViewVisible = visible;
		AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
		fixture.CreateRunner("Player", graph);
		REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
		fixture.Start();
		fixture.Input.Grant();
		fixture.FramesUntilStopped();
		CHECK_FALSE(fixture.Input.HasInput());
		fixture.Session.ClearBreakpoints(graph);
		fixture.Gui.Press(ImGuiKey_F5, fixture.Drawer());
		fixture.Frames(2);
		CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
		// Split layout: the game takes its input back. Shared dock node: the graph stays in front without it.
		CHECK(fixture.Input.HasInput() == visible);
		CHECK_FALSE(fixture.Controls.HasTakenInput());
	}
}

TEST_CASE("VisualScriptDebug: the Debugger panel's tabs show the call stack, the frame's values with Evaluate, breakpoints and instances")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	UUID const player = fixture.CreateRunner("Player", graph);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.FramesUntilStopped();

	std::vector<VisualScriptDebuggerPanel::CallStackRow> const& rows = fixture.Debugger.GetCallStackRows();
	REQUIRE(rows.size() == 2);
	CHECK(rows[0].Selected);
	CHECK(rows[0].Text.starts_with("#0 "));
	CHECK(rows[0].Text.find("(node 11) \xC2\xB7 Counter.sgraph \xE2\x80\xBA Event Graph \xC2\xB7 on Player (" + player.ToString() + ")") !=
	      std::string::npos);
	CHECK(rows[1].Text.starts_with("Started by "));
	CHECK(rows[1].Text.ends_with("(node 10)"));

	// Values, with a one-shot Evaluate against the frame.
	fixture.Debugger.SetTab(VisualScriptDebuggerTab::Values);
	fixture.Frames(2);
	CHECK(fixture.Debugger.GetTab() == VisualScriptDebuggerTab::Values);
	fixture.Debugger.SetEvaluateText("Count + 41");
	fixture.Debugger.Evaluate();
	CHECK(fixture.Debugger.GetEvaluateResult() == "41");

	// Breakpoints and Instances draw; Remove All asks first, and F5 behind its confirmation resumes nothing.
	fixture.Debugger.SetTab(VisualScriptDebuggerTab::Breakpoints);
	fixture.Frames(2);
	fixture.Debugger.SetTab(VisualScriptDebuggerTab::Instances);
	fixture.Frames(2);
	CHECK(fixture.Debugger.GetTab() == VisualScriptDebuggerTab::Instances);

	// graph.debug-panel picks the tab, the stop and the frame, as the panel shows them.
	Json const described = fixture.Ok("graph.debug-panel", Json::object({{"tab", "CallStack"}, {"frame", 0}}));
	CHECK(described["open"] == true);
	CHECK(described["tab"] == "CallStack");
	CHECK(described["frame"] == 0);
	CHECK(described["stop"] == fixture.Session.GetSelectedStop().value_or(0));
	CommandResult const outOfRange = fixture.Run("graph.debug-panel", Json::object({{"frame", 9}}));
	REQUIRE(outOfRange.IsError());
	CHECK(outOfRange.GetError().Code == AutomationErrorCode::InvalidParams);
	CommandResult const noStop = fixture.Run("graph.debug-panel", Json::object({{"stop", 999}}));
	REQUIRE(noStop.IsError());
	CHECK(noStop.GetError().Code == AutomationErrorCode::InvalidParams);
	fixture.Frames(2);
	CHECK(fixture.Debugger.GetTab() == VisualScriptDebuggerTab::CallStack);
	CHECK(fixture.Ok("graph.debug-panel", Json::object({{"open", false}}))["open"] == false);
	fixture.Frames(1);
	CHECK_FALSE(fixture.Debugger.IsVisible());
}

TEST_CASE("VisualScriptDebug: a breakpoint that stopped many instances offers to keep only one, and flow lights while playing")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Enemy 1", graph);
	fixture.CreateRunner("Enemy 2", graph);
	fixture.Open(graph);
	fixture.Start();
	fixture.Frames(4);
	// Flow: the window watches its sub-graph, and the frames light its exec wires and count its nodes.
	VisualScriptEditorPanel& window = fixture.Window(graph);
	CHECK(window.GetState().FlowDrawn);
	CHECK(window.GetFlowModel().GetCount(11) >= 2);

	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.FramesUntilStopped();
	VisualScriptWindowDebugInput const* input = fixture.Editors.FindDebugInput(graph);
	REQUIRE(input != nullptr);
	REQUIRE(input->Strip.size() == 2);
	CHECK(input->Strip[1].Text == "+1 more instance stopped here");
	REQUIRE(input->Strip[1].Buttons.size() == 1);
	CHECK(input->Strip[1].Buttons[0].Action == VisualScriptDebugStripAction::OnlyInstance);
	CHECK(input->Strip[1].Buttons[0].Label.starts_with("Only Enemy "));
	// The held flow clock: the stop's frame stays lit, without dots.
	CHECK(window.GetState().FlowHeld);
}
