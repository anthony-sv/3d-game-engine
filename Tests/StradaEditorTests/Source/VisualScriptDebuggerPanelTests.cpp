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
#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptCanvasRenderer.h"
#include "Editor/VisualScripting/VisualScriptController.h"
#include "Editor/VisualScripting/VisualScriptDebugControls.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"
#include "Editor/VisualScripting/VisualScriptEditors.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
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
		// The Debugger panel docked with a stand-in of the Console, as the editor's default layout docks it.
		bool DockWithConsole = false;
		// The Debugger panel's size when it floats.
		ImVec2 DebuggerSize{900.0f, 900.0f};
		bool DockBuilt = false;
		ImGuiID Dockspace = 0;
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
			// As EditorLayer: values formatted with the node library, a breakpoint's double-click and an instance's click open
			// their windows.
			Debugger.SetSemantics(&Semantics);
			Debugger.SetOpenNode(
				[this](AssetHandle graph, uint32_t node)
				{
					VisualScriptOpenRequest request;
					request.Graph = graph;
					request.ShowNode = node;
					request.Select = {node};
					request.Function = 0;
					REQUIRE(Editors.Open(request).IsOk());
				});
			Debugger.SetOpenGraph(
				[this](AssetHandle graph, UUID entity)
				{
					VisualScriptOpenRequest request;
					request.Graph = graph;
					request.ContextEntity = entity;
					REQUIRE(Editors.Open(request).IsOk());
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
			if (DockWithConsole)
			{
				ImGuiID const dockspace = ImGui::DockSpaceOverViewport(ImGui::GetID("Dockspace"), ImGui::GetMainViewport());
				Dockspace = dockspace;
				if (!std::exchange(DockBuilt, true))
				{
					ImGui::DockBuilderRemoveNode(dockspace);
					ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
					ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
					ImGui::DockBuilderDockWindow("Console", dockspace);
					ImGui::DockBuilderDockWindow(VisualScriptDebuggerPanel::WindowName, dockspace);
					ImGui::DockBuilderFinish(dockspace);
				}
				ImGui::Begin("Console");
				ImGui::TextUnformatted("Log");
				ImGui::End();
			}
			ImGui::SetNextWindowPos(ImVec2(20.0f, 1000.0f), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(900.0f, 70.0f), ImGuiCond_Always);
			ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoSavedSettings);
			ImGui::TextUnformatted("Play");
			Controls.DrawToolbarButtons();
			ImGui::End();
			Editors.OnImGuiRender();
			if (!DockWithConsole)
			{
				ImGui::SetNextWindowPos(ImVec2(1000.0f, 20.0f), ImGuiCond_Always);
				ImGui::SetNextWindowSize(DebuggerSize, ImGuiCond_Always);
			}
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

		// Brings a window to the front (its tab, or above the floating windows), drawn so in the next frame.
		void Focus(char const* name)
		{
			Gui.Frame(
				[this, name]
				{
					Draw();
					ImGui::SetWindowFocus(name);
				});
			Frames(1);
		}

		// Clicks the middle of a rectangle the last draw recorded, in front of the window that holds it.
		void ClickAt(char const* window, ImVec2 min, ImVec2 max, int count = 1)
		{
			Focus(window);
			Gui.Click(ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), Drawer(), count);
		}

		void ClickPanelItem(VisualScriptDebuggerItem item, int count = 1)
		{
			std::optional<VisualScriptDebuggerPanel::ItemRect> const rect = Debugger.GetItemRect(item);
			REQUIRE(rect.has_value());
			ClickAt(VisualScriptDebuggerPanel::WindowName, rect->Min, rect->Max, count);
		}

		// A button of the Remove All confirmation, a modal in front of everything already.
		void ClickModalItem(VisualScriptDebuggerItem item)
		{
			std::optional<VisualScriptDebuggerPanel::ItemRect> const rect = Debugger.GetItemRect(item);
			REQUIRE(rect.has_value());
			Gui.Click(ImVec2((rect->Min.x + rect->Max.x) * 0.5f, (rect->Min.y + rect->Max.y) * 0.5f), Drawer());
		}

		void ClickToolbarButton(VisualScriptDebugButton button)
		{
			std::optional<VisualScriptDebugControls::ButtonRect> const rect = Controls.GetButtonRect(button);
			REQUIRE(rect.has_value());
			ClickAt("Toolbar", rect->Min, rect->Max);
		}

		void ClickWindowItem(AssetHandle graph, VisualScriptPanelItem item)
		{
			std::optional<VisualScriptRect> const rect = Window(graph).GetItemRect(item);
			REQUIRE(rect.has_value());
			std::string const name = VisualScriptEditorPanel::GetWindowName(graph, {});
			Focus(name.c_str());
			std::optional<VisualScriptRect> const shown = Window(graph).GetItemRect(item);
			REQUIRE(shown.has_value());
			Gui.Click(ImVec2((shown->Min.x + shown->Max.x) * 0.5f, (shown->Min.y + shown->Max.y) * 0.5f), Drawer());
		}

		// What the panel invariant compares (spec-slice4 §20): the undo count, every graph's file and asset version, and the
		// views file.
		struct Snapshot
		{
			size_t Undo = 0;
			std::vector<std::string> Files;
			std::vector<uint64_t> Versions;
			std::string Views;

			bool operator==(Snapshot const& other) const = default;
		};

		Snapshot TakeSnapshot(std::span<AssetHandle const> graphs) const
		{
			Snapshot snapshot;
			snapshot.Undo = Context.GetHistory().GetUndoCount();
			for (AssetHandle const graph : graphs)
			{
				Result<std::string> text = FileSystem::ReadTextFile(AssetManager::GetAbsolutePath(graph));
				REQUIRE(text.IsOk());
				snapshot.Files.push_back(text.TakeValue());
				snapshot.Versions.push_back(AssetManager::GetAsset<VisualScriptAsset>(graph)->GetVersion());
			}
			std::filesystem::path const views = Directory.GetPath() / "UserData" / "Editor" / "VisualScriptViews.json";
			if (std::filesystem::exists(views))
			{
				Result<std::string> text = FileSystem::ReadTextFile(views);
				REQUIRE(text.IsOk());
				snapshot.Views = text.TakeValue();
			}
			return snapshot;
		}

		// The panel invariant, first in every headless panel test: three frames of drawing (the windows, the strip, the
		// Debugger panel, the toolbar) change no undo count, no file byte and no asset version.
		void CheckDrawingChangesNothing(std::initializer_list<AssetHandle> graphs)
		{
			std::vector<AssetHandle> const list(graphs);
			Snapshot const before = TakeSnapshot(list);
			Frames(3);
			CHECK(TakeSnapshot(list) == before);
		}

		VisualScriptWindowDebugInput const& DebugInput(AssetHandle graph)
		{
			VisualScriptWindowDebugInput const* input = Editors.FindDebugInput(graph);
			REQUIRE(input != nullptr);
			return *input;
		}
	};

	VisualScriptDocument MakeVariableDocument(std::vector<VisualScriptVariable> variables)
	{
		VisualScriptDocument document = MakeCounterDocument();
		for (VisualScriptVariable& variable : variables)
		{
			document.Variables.push_back(std::move(variable));
		}
		return document;
	}

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		variable.Value = std::move(value);
		variable.Public = true;
		return variable;
	}

	// The Delay of 0.1 s at node 11 when the graph starts, whose Completed leads nowhere; on every update Count += 1 at node 21.
	VisualScriptDocument MakeWaitDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)));
		document.Graph.Nodes.push_back(
			MakeNode(11, "Flow.Delay", glm::vec2(320.0f, 0.0f), Json::object(), Json::object({{"Duration", 0.1}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Nodes.push_back(MakeNode(20, "Event.Update", glm::vec2(0.0f, 400.0f)));
		document.Graph.Nodes.push_back(MakeNode(21, "Variable.Set", glm::vec2(320.0f, 400.0f), Json::object({{"Variable", 1}})));
		document.Graph.Nodes.push_back(MakeNode(22, "Operator.Add", glm::vec2(160.0f, 560.0f), Json::object(), Json::object({{"B", 1}})));
		document.Graph.Nodes.push_back(MakeNode(23, "Variable.Get", glm::vec2(0.0f, 560.0f), Json::object({{"Variable", 1}})));
		document.Graph.Links.push_back(MakeLink(23, "Value", 22, "A"));
		document.Graph.Links.push_back(MakeLink(22, "Result", 21, "Value"));
		document.Graph.Links.push_back(MakeLink(20, "Then", 21, "Execute"));
		return document;
	}

	// On every update: Result = 1 / Zero at node 11, an integer division by zero.
	VisualScriptDocument MakeFailingDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Result", "int", 0));
		document.Variables.push_back(MakeVariable(2, "Zero", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update", glm::vec2(0.0f, 0.0f)));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", glm::vec2(400.0f, 0.0f), Json::object({{"Variable", 1}})));
		document.Graph.Nodes.push_back(
			MakeNode(12, "Operator.Divide", glm::vec2(200.0f, 160.0f), Json::object(), Json::object({{"A", 1}})));
		document.Graph.Nodes.push_back(MakeNode(13, "Variable.Get", glm::vec2(0.0f, 160.0f), Json::object({{"Variable", 2}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(13, "Value", 12, "B"));
		document.Graph.Links.push_back(MakeLink(12, "Result", 11, "Value"));
		return document;
	}

	// On every update the sender sends Ping (node 20) to the entity named Receiver (found by node 21).
	VisualScriptDocument MakeSenderDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update", glm::vec2(0.0f, 0.0f)));
		document.Graph.Nodes.push_back(MakeNode(20, "Event.Send", glm::vec2(400.0f, 0.0f), Json::object({{"Name", "Ping"}})));
		document.Graph.Nodes.push_back(MakeNode(21, "Call:Strada.Entity.FindByName(string)", glm::vec2(100.0f, 200.0f), Json::object(),
		                                        Json::object({{"Name", "Receiver"}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 20, "Execute"));
		document.Graph.Links.push_back(MakeLink(21, "Result", 20, "Target"));
		return document;
	}

	// Ping (node 10) adds one to Count at node 11.
	VisualScriptDocument MakeReceiverDocument()
	{
		VisualScriptDocument document = MakeCounterDocument();
		document.Graph.Nodes.front() = MakeNode(10, "Event.Custom", glm::vec2(0.0f, 0.0f), Json::object({{"Name", "Ping"}}));
		std::erase_if(document.Graph.Links,
		              [](VisualScriptLink const& link)
		              {
						  return link.From.Node == 11 && link.To.Node == 14;
					  });
		return document;
	}

	// A function library whose function 30 (Damage) logs at node 33 between its Entry (31) and Return (32).
	VisualScriptDocument MakeLibraryDocument()
	{
		VisualScriptDocument document;
		document.Kind = VisualScriptDocumentKind::Library;
		VisualScriptFunction function;
		function.ID = 30;
		function.Name = "Damage";
		function.Graph.Nodes = {
			MakeNode(31, "Function.Entry", glm::vec2(0.0f, 0.0f)),
			MakeNode(33, "Call:Strada.Log.Info(string)", glm::vec2(300.0f, 0.0f), Json::object(), Json::object({{"Message", "hit"}})),
			MakeNode(32, "Function.Return", glm::vec2(700.0f, 0.0f))};
		function.Graph.Links = {MakeLink(31, "Then", 33, "Execute"), MakeLink(33, "Then", 32, "Execute")};
		document.Functions.push_back(std::move(function));
		return document;
	}

	// On every update: the library's Damage at node 11, then a log at node 12.
	VisualScriptDocument MakeCallerDocument(AssetHandle library)
	{
		VisualScriptDocument document;
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update", glm::vec2(0.0f, 0.0f)));
		document.Graph.Nodes.push_back(
			MakeNode(11, "Function.Call", glm::vec2(400.0f, 0.0f), Json::object({{"Function", 30}, {"Graph", library.ToString()}})));
		document.Graph.Nodes.push_back(
			MakeNode(12, "Call:Strada.Log.Info(string)", glm::vec2(800.0f, 0.0f), Json::object(), Json::object({{"Message", "called"}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(11, "Then", 12, "Execute"));
		return document;
	}
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
	fixture.CheckDrawingChangesNothing({graph});

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
	fixture.CheckDrawingChangesNothing({graph});
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
	fixture.CheckDrawingChangesNothing({graph});
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

	// Breakpoint Settings opens the popup on the node (what it lists and does: the popup's own case).
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
	fixture.CheckDrawingChangesNothing({graph});
	uint64_t const frame = fixture.GetFrame();

	// While stopped the frame step is refused with the reason, and the Pause toggle says it continues.
	CHECK_FALSE(fixture.Controls.CanStepFrame());
	CHECK(fixture.Controls.GetStepRefusal() == std::optional<std::string>(std::string(VisualScriptDebugSession::RefusalForUI)));
	CHECK(fixture.Controls.GetPauseTooltip() == "Continue (F5): runs the stopped executions on, then the game");
	fixture.Controls.StepFrame();
	CHECK(fixture.GetFrame() == frame);

	// F10 and F5 behind a modal (any dialog; the Remove All confirmation has its own case) do nothing.
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
		fixture.CheckDrawingChangesNothing({graph});
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
	fixture.CheckDrawingChangesNothing({graph});

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

	// Breakpoints and Instances draw (what their rows do: each tab's own case).
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
	fixture.CheckDrawingChangesNothing({graph});
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

TEST_CASE("VisualScriptDebug: a waiting sender's window says what its receiver is stopped in, and Show brings the receiver forward")
{
	DebuggerFixture fixture;
	AssetHandle const sender = fixture.CreateGraph("Scripts/Sender.sgraph", MakeSenderDocument());
	AssetHandle const receiver = fixture.CreateGraph("Scripts/Receiver.sgraph", MakeReceiverDocument());
	fixture.CreateRunner("Sender", sender);
	UUID const target = fixture.CreateRunner("Receiver", receiver);
	REQUIRE(fixture.Session.SetBreakpoint(receiver, MakeBreakpoint(11)).IsOk());
	fixture.Open(sender);
	fixture.Start();
	fixture.FramesUntilStopped();
	fixture.CheckDrawingChangesNothing({sender, receiver});

	// The receiver stopped inside the sender's Send: its frames are the receiver's, then the sender's delivery.
	VisualScriptStop const* stop = fixture.Session.FindStop(*fixture.Session.GetSelectedStop());
	REQUIRE(stop != nullptr);
	REQUIRE(stop->Frames.size() == 2);
	REQUIRE(stop->Frames[1].Kind == VisualScriptStopFrameKind::Delivery);

	// The receiver's window holds the stop; the sender's says it waits, outlines its Send and offers to show the receiver.
	CHECK(fixture.Window(receiver).GetState().DebugState == VisualScriptWindowDebugState::Stopped);
	VisualScriptWindowDebugInput const& waiting = fixture.DebugInput(sender);
	CHECK(waiting.State == VisualScriptWindowDebugState::Waiting);
	CHECK(waiting.StoppedHere);
	CHECK(waiting.Frame == 1);
	CHECK(waiting.Callers == std::vector<std::pair<uint32_t, uint32_t>>{{0, 20}});
	REQUIRE(waiting.Strip.size() == 1);
	CHECK(waiting.Strip[0].Text.starts_with("Waiting in Send Ping (node 20): its receiver on Receiver (" + target.ToString() + ")"));
	CHECK(waiting.Strip[0].Text.find(" is stopped in Receiver") != std::string::npos);
	REQUIRE(waiting.Strip[0].Buttons.size() == 1);
	CHECK(waiting.Strip[0].Buttons[0].Action == VisualScriptDebugStripAction::Show);
	CHECK(waiting.Strip[0].Buttons[0].Frame == std::optional<uint32_t>(0));
	CHECK(fixture.Ok("graph.editors")["editors"][0]["debug"]["state"] == "Waiting");

	// Show (clicked in the sender's window, which comes to the front for it) selects the receiver's frame and brings its window
	// forward again.
	fixture.ClickWindowItem(sender, VisualScriptPanelItem::StripShow);
	fixture.Frames(3);
	CHECK(fixture.Session.GetSelectedFrame() == 0);
	CHECK(fixture.Window(receiver).GetState().Focused);
	CHECK_FALSE(fixture.Window(sender).GetState().Focused);
	// Selecting the sender's own frame makes its window the stopped one: its Send is the instruction, and the receiver's window
	// outlines nothing of it.
	fixture.Session.SelectFrame(1);
	fixture.Frames(2);
	CHECK(fixture.DebugInput(sender).State == VisualScriptWindowDebugState::Stopped);
	CHECK(fixture.DebugInput(sender).Instruction->Node == 20);
	fixture.Session.ClearBreakpoints(std::nullopt);
}

TEST_CASE("VisualScriptDebug: a stop in a library function draws its call outlined in the calling graph's window, without a strip")
{
	DebuggerFixture fixture;
	AssetHandle const library = fixture.CreateGraph("Scripts/Combat.sgraph", MakeLibraryDocument());
	AssetHandle const caller = fixture.CreateGraph("Scripts/Caller.sgraph", MakeCallerDocument(library));
	fixture.CreateRunner("Player", caller);
	REQUIRE(fixture.Session.SetBreakpoint(library, MakeBreakpoint(33)).IsOk());
	fixture.Open(caller);
	fixture.Start();
	fixture.FramesUntilStopped();
	fixture.CheckDrawingChangesNothing({library, caller});

	// The library's window came forward on its function, holding the stop.
	VisualScriptEditorState const stopped = fixture.Window(library).GetState();
	CHECK(stopped.DebugState == VisualScriptWindowDebugState::Stopped);
	CHECK(stopped.DebugNode == std::optional<uint32_t>(33));
	CHECK(stopped.Function == 30);
	// The caller's window holds a frame of the stop but no stop of its own: its call is outlined, no strip, no STOPPED segment.
	VisualScriptWindowDebugInput const& calling = fixture.DebugInput(caller);
	CHECK(calling.State == VisualScriptWindowDebugState::None);
	CHECK_FALSE(calling.StoppedHere);
	CHECK(calling.Strip.empty());
	CHECK(calling.Callers == std::vector<std::pair<uint32_t, uint32_t>>{{0, 11}});
	fixture.Focus(VisualScriptEditorPanel::GetWindowName(caller, {}).c_str());
	fixture.Window(caller).SetDrawRecord(&fixture.Record);
	fixture.Frames(1);
	VisualScriptDrawRecordItem const* outline = fixture.Record.Find(VisualScriptDrawItemKind::Caller, 11);
	CHECK(outline != nullptr);
	CHECK(fixture.Record.Find(VisualScriptDrawItemKind::Instruction, 11) == nullptr);
	fixture.Window(caller).SetDrawRecord(nullptr);
	CHECK_FALSE(fixture.Window(caller).GetItemRect(VisualScriptPanelItem::Stopped).has_value());
	Json const editors = fixture.Ok("graph.editors");
	for (Json const& editor : editors["editors"])
	{
		CHECK(editor["debug"]["state"] == (editor["asset"] == library.ToString() ? "Stopped" : "None"));
	}
	fixture.Session.ClearBreakpoints(std::nullopt);
}

TEST_CASE("VisualScriptDebug: Step Into, Step Out and the computed path from a real stop, by F11, Ctrl+F11 and Shift+F11")
{
	DebuggerFixture fixture;
	AssetHandle const library = fixture.CreateGraph("Scripts/Combat.sgraph", MakeLibraryDocument());
	AssetHandle const caller = fixture.CreateGraph("Scripts/Caller.sgraph", MakeCallerDocument(library));
	AssetHandle const counter = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Player", caller);
	fixture.CreateRunner("Counter", counter);
	REQUIRE(fixture.Session.SetBreakpoint(caller, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.FramesUntilStopped();
	fixture.CheckDrawingChangesNothing({library, caller, counter});
	REQUIRE(fixture.Window(caller).GetState().DebugNode == std::optional<uint32_t>(11));

	// F11 steps into the call: the library's window comes forward at its function's first node.
	fixture.Gui.Press(ImGuiKey_F11, fixture.Drawer());
	fixture.Frames(3);
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(fixture.Window(library).GetState().DebugNode == std::optional<uint32_t>(31));
	CHECK(fixture.Window(library).GetState().DebugFunction == 30);
	// Shift+F11 steps out to the caller, at its node after the call.
	fixture.Gui.Chord(ImGuiMod_Shift | ImGuiKey_F11, fixture.Drawer());
	fixture.Frames(3);
	CHECK(fixture.Window(library).GetState().DebugState == VisualScriptWindowDebugState::None);
	CHECK(fixture.Window(caller).GetState().DebugState == VisualScriptWindowDebugState::Stopped);
	CHECK(fixture.Window(caller).GetState().DebugNode == std::optional<uint32_t>(12));
	// Continue, and at the next frame's stop Ctrl+F11 (macOS's chord, which every platform has) steps into it too.
	fixture.Gui.Press(ImGuiKey_F5, fixture.Drawer());
	fixture.FramesUntilStopped();
	REQUIRE(fixture.Window(caller).GetState().DebugNode == std::optional<uint32_t>(11));
	fixture.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_F11, fixture.Drawer());
	fixture.Frames(3);
	CHECK(fixture.Window(library).GetState().DebugNode == std::optional<uint32_t>(31));
	fixture.Session.ClearBreakpoints(std::nullopt);
	fixture.Gui.Press(ImGuiKey_F5, fixture.Drawer());
	fixture.Frames(2);

	// The counter's stop at 14 after a step from 11: the path that led to it (11's Add and Get) glows in the current colour.
	REQUIRE(fixture.Session.SetBreakpoint(counter, MakeBreakpoint(11)).IsOk());
	fixture.FramesUntilStopped();
	fixture.Gui.Press(ImGuiKey_F10, fixture.Drawer());
	fixture.Frames(3);
	REQUIRE(fixture.Window(counter).GetState().DebugNode == std::optional<uint32_t>(14));
	VisualScriptCanvasDebugView const* view = fixture.Window(counter).GetDebugView();
	REQUIRE(view != nullptr);
	CHECK(std::find(view->Computed.begin(), view->Computed.end(), 12u) != view->Computed.end());
	fixture.Window(counter).SetDrawRecord(&fixture.Record);
	fixture.Frames(1);
	VisualScriptDrawRecordItem const* computed = fixture.Record.Find(VisualScriptDrawItemKind::RunGlow, 12);
	REQUIRE(computed != nullptr);
	CHECK(computed->Mark == 1);
	fixture.Window(counter).SetDrawRecord(nullptr);
	fixture.Session.ClearBreakpoints(std::nullopt);
}

TEST_CASE("VisualScriptDebug: stepped into a function's Entry, the Values tab lists its parameters once, not again as the Entry's outputs")
{
	DebuggerFixture fixture;
	VisualScriptDocument combat = MakeLibraryDocument();
	combat.Functions.front().Inputs.push_back({"Amount", "int", {}});
	AssetHandle const library = fixture.CreateGraph("Scripts/Combat.sgraph", combat);
	VisualScriptDocument calling = MakeCallerDocument(library);
	calling.Graph.Nodes[1].Inputs = Json::object({{"Amount", 5}});
	AssetHandle const caller = fixture.CreateGraph("Scripts/Caller.sgraph", calling);
	fixture.CreateRunner("Player", caller);
	REQUIRE(fixture.Session.SetBreakpoint(caller, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.FramesUntilStopped();
	fixture.CheckDrawingChangesNothing({library, caller});
	fixture.Gui.Press(ImGuiKey_F11, fixture.Drawer());
	fixture.Frames(3);
	REQUIRE(fixture.Window(library).GetState().DebugNode == std::optional<uint32_t>(31));
	fixture.Debugger.SetTab(VisualScriptDebuggerTab::Values);
	fixture.Frames(2);

	std::vector<VisualScriptDebuggerPanel::ValueRow> const& rows = fixture.Debugger.GetValueRows();
	auto const named = [&rows](std::string_view name, bool section)
	{
		return std::count_if(rows.begin(), rows.end(),
		                     [name, section](VisualScriptDebuggerPanel::ValueRow const& row)
		                     {
								 return row.Name == name && row.Cell.has_value() != section;
							 });
	};
	CHECK(named("Parameters", true) == 1);
	CHECK(named("Outputs", true) == 0);
	CHECK(named("Amount", false) == 1);
	auto const amount = std::find_if(rows.begin(), rows.end(),
	                                 [](VisualScriptDebuggerPanel::ValueRow const& row)
	                                 {
										 return row.Name == "Amount" && row.Cell;
									 });
	REQUIRE(amount != rows.end());
	CHECK(amount->Cell->Text == "5");
	fixture.Session.ClearBreakpoints(std::nullopt);
}

TEST_CASE("VisualScriptDebug: a step waiting in a Delay says so, then that the execution ended, and the keys reach the editor again")
{
	DebuggerFixture fixture;
	fixture.GameViewVisible = true;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Wait.sgraph", MakeWaitDocument());
	fixture.CreateRunner("Waiter", graph);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.Input.Grant();
	fixture.FramesUntilStopped();
	fixture.CheckDrawingChangesNothing({graph});
	CHECK_FALSE(fixture.Input.HasInput());

	// Step Over the Delay: the execution waits in it, the game resumes, and the strip offers Break.
	fixture.Gui.Press(ImGuiKey_F10, fixture.Drawer());
	fixture.Frames(1);
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stepping);
	CHECK(fixture.Input.HasInput());
	VisualScriptWindowDebugInput const& stepping = fixture.DebugInput(graph);
	CHECK(stepping.State == VisualScriptWindowDebugState::Stepping);
	REQUIRE(stepping.Strip.size() == 1);
	CHECK(stepping.Strip[0].Text == "Stepping: the execution waits in Delay (node 11) and stops when it completes");
	REQUIRE(stepping.Strip[0].Buttons.size() == 1);
	CHECK(stepping.Strip[0].Buttons[0].Action == VisualScriptDebugStripAction::Break);
	// In a scene an agent paused, frames must run for it to complete.
	REQUIRE(fixture.Play.SetPaused(true));
	fixture.Frames(1);
	CHECK(fixture.DebugInput(graph).Strip[0].Text.ends_with(". The game is paused: frames must run for it to complete"));
	REQUIRE(fixture.Play.SetPaused(false));
	fixture.Input.Grant();

	// The Delay completes into nothing: the step ended, the pause is held, and the controls take the game's input back so F5
	// and F10 reach the editor, though no new stop was made.
	for (int frame = 0; frame < 30 && fixture.Session.GetSessionState() != VisualScriptDebugSessionState::StepEnded; frame++)
	{
		fixture.EditorFrame();
	}
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::StepEnded);
	fixture.Frames(1);
	CHECK_FALSE(fixture.Input.HasInput());
	CHECK(fixture.Controls.HasTakenInput());
	VisualScriptWindowDebugInput const& ended = fixture.DebugInput(graph);
	CHECK(ended.State == VisualScriptWindowDebugState::StepEnded);
	REQUIRE(ended.Strip.size() == 1);
	CHECK(ended.Strip[0].Text == "The execution ended. Continue (F5) resumes the game; Step (F10) stops at this graph's next instruction");
	REQUIRE(ended.Strip[0].Buttons.size() == 2);
	CHECK(ended.Strip[0].Buttons[0].Action == VisualScriptDebugStripAction::Continue);
	CHECK(ended.Strip[0].Buttons[1].Action == VisualScriptDebugStripAction::StepOver);

	// F5 resumes the game, which gets its input back in a split layout.
	fixture.Gui.Press(ImGuiKey_F5, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	CHECK_FALSE(fixture.Play.IsPaused());
	CHECK(fixture.Input.HasInput());
	CHECK_FALSE(fixture.Controls.HasTakenInput());
}

TEST_CASE("VisualScriptDebug: a stop an edit cancels holds the pause, which its window's strip says, and the Pause toggle resumes the game")
{
	DebuggerFixture fixture;
	fixture.GameViewVisible = true;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Player", graph);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.FramesUntilStopped();
	fixture.CheckDrawingChangesNothing({graph});

	// An edit removes the stopped node: the stop ends without a resume, and the pause stays held.
	VisualScriptDocument edited = MakeCounterDocument();
	std::erase_if(edited.Graph.Nodes,
	              [](VisualScriptNode const& node)
	              {
					  return node.ID >= 11 && node.ID <= 13;
				  });
	std::erase_if(edited.Graph.Links,
	              [](VisualScriptLink const& link)
	              {
					  return link.From.Node <= 13 || link.To.Node <= 13;
				  });
	edited.Graph.Links.push_back(MakeLink(10, "Then", 14, "Execute"));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(graph, edited).IsOk());
	fixture.Frames(2);
	REQUIRE(fixture.Session.GetState().Stops.empty());
	REQUIRE(fixture.Session.HasPausedScene());
	VisualScriptWindowDebugInput const& held = fixture.DebugInput(graph);
	REQUIRE(held.Strip.size() == 1);
	CHECK(held.Strip[0].Text == "The stopped execution ended (cancelled by your edit). The game stays paused: Continue (F5) resumes it");
	REQUIRE(held.Strip[0].Buttons.size() == 1);
	CHECK(held.Strip[0].Buttons[0].Action == VisualScriptDebugStripAction::Continue);
	CHECK(fixture.Controls.GetPauseTooltip() == VisualScriptDebugControls::Texts::Resume);

	// While the pause is held the game gets no input: F5 must reach the editor.
	fixture.Input.Grant();
	fixture.Frames(1);
	CHECK_FALSE(fixture.Input.HasInput());
	CHECK(fixture.Controls.HasTakenInput());
	// The Pause toggle unpauses, as Continue would, and gives the game its input back where it shows.
	fixture.Controls.TogglePause();
	CHECK_FALSE(fixture.Play.IsPaused());
	CHECK(fixture.Input.HasInput());
	CHECK_FALSE(fixture.Controls.HasTakenInput());
	fixture.Frames(2);
	CHECK(fixture.DebugInput(graph).Strip.empty());
}

TEST_CASE("VisualScriptDebug: a passed breakpoint and an error stop draw read-only, with their strips")
{
	DebuggerFixture fixture;
	// A spawner instantiates a prefab whose graph's Create has a breakpoint: it runs inside the spawner's call, and passes.
	VisualScriptDocument child;
	child.Graph.Nodes.push_back(MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)));
	child.Graph.Nodes.push_back(
		MakeNode(11, "Set:Strada.Entity.Name", glm::vec2(400.0f, 0.0f), Json::object(), Json::object({{"Value", "Spawned"}})));
	child.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	AssetHandle const childGraph = fixture.CreateGraph("Scripts/Child.sgraph", child);
	Scene source("Source");
	Entity member = source.CreateEntity("Member");
	member.AddComponent<VisualScriptComponent>().Graph = childGraph;
	Result<Ref<PrefabAsset>> prefab = PrefabAsset::Create(PrefabSerializer::Serialize(source, member));
	REQUIRE(prefab.IsOk());
	AssetHandle const prefabHandle = AssetManager::AddMemoryAsset(prefab.GetValue(), "Member");
	VisualScriptDocument spawner;
	spawner.Graph.Nodes.push_back(MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)));
	spawner.Graph.Nodes.push_back(MakeNode(11,
	                                       "Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)",
	                                       glm::vec2(400.0f, 0.0f), Json::object(), Json::object({{"Prefab", prefabHandle.ToString()}})));
	spawner.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	AssetHandle const spawnerGraph = fixture.CreateGraph("Scripts/Spawner.sgraph", spawner);
	fixture.CreateRunner("Spawner", spawnerGraph);
	AssetHandle const failing = fixture.CreateGraph("Scripts/Failing.sgraph", MakeFailingDocument());
	REQUIRE(fixture.Session.SetBreakpoint(childGraph, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.FramesUntilStopped();
	fixture.CheckDrawingChangesNothing({childGraph, spawnerGraph, failing});

	VisualScriptWindowDebugInput const& passed = fixture.DebugInput(childGraph);
	CHECK(passed.State == VisualScriptWindowDebugState::Passed);
	REQUIRE(passed.Instruction.has_value());
	CHECK(passed.Instruction->ReadOnly);
	REQUIRE(passed.Strip.size() == 1);
	CHECK(passed.Strip[0].Text.starts_with("Passed breakpoint at "));
	CHECK(passed.Strip[0].Text.find("(node 11) on Spawned (") != std::string::npos);
	CHECK(passed.Strip[0].Text.ends_with(": it runs nested in engine code another execution called. These are its values as it passed"));
	REQUIRE(passed.Strip[0].Buttons.size() == 1);
	CHECK(passed.Strip[0].Buttons[0].Action == VisualScriptDebugStripAction::Continue);
	// The call stack names the engine code between the two graphs.
	std::vector<VisualScriptDebuggerPanel::CallStackRow> const& rows = fixture.Debugger.GetCallStackRows();
	REQUIRE(rows.size() >= 3);
	CHECK(rows[1].Text.starts_with("#1 \xE2\x80\x94 engine code ("));
	// Continue dismisses it.
	fixture.ClickWindowItem(childGraph, VisualScriptPanelItem::StripContinue);
	fixture.Frames(2);
	CHECK(fixture.Session.GetState().Stops.empty());
	fixture.Session.ClearBreakpoints(std::nullopt);

	// Break on errors, turned on in the Breakpoints tab: a failing graph stops read-only where it failed.
	fixture.Debugger.SetTab(VisualScriptDebuggerTab::Breakpoints);
	fixture.Frames(2);
	fixture.ClickPanelItem(VisualScriptDebuggerItem::BreakOnErrors);
	REQUIRE(fixture.Session.IsBreakOnErrors());
	UUID const failer = fixture.CreateRunner("Failer", failing);
	fixture.FramesUntilStopped();
	VisualScriptWindowDebugInput const& error = fixture.DebugInput(failing);
	CHECK(error.State == VisualScriptWindowDebugState::Error);
	REQUIRE(error.Strip.size() == 1);
	// The runtime's message without the place the line already names.
	CHECK(error.Strip[0].Text == "Failed at Divide (node 12) on Failer (" + failer.ToString() + "): integer division by zero");
	REQUIRE(error.Strip[0].Buttons.size() == 1);
	CHECK(error.Strip[0].Buttons[0].Action == VisualScriptDebugStripAction::Continue);
	fixture.Session.SetBreakOnErrors(false);
}

TEST_CASE("VisualScriptDebug: a stop on a disabled node passes through, and a broken edit while stopped adds the last working line")
{
	DebuggerFixture fixture;
	// The variable's name holds an arrow, which the strip draws as a shape.
	VisualScriptDocument document = MakeCounterDocument();
	document.Variables[0].Name = "Count\xE2\x86\x92Total";
	std::find_if(document.Graph.Nodes.begin(), document.Graph.Nodes.end(),
	             [](VisualScriptNode const& node)
	             {
					 return node.ID == 11;
				 })
		->Enabled = VisualScriptNodeEnabled::Disabled;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", document);
	fixture.CreateRunner("Player", graph);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.FramesUntilStopped();
	fixture.CheckDrawingChangesNothing({graph});

	VisualScriptEditorPanel& window = fixture.Window(graph);
	REQUIRE_FALSE(window.GetStripRows().empty());
	std::string const& line = window.GetStripRows()[0].Text;
	CHECK(line.starts_with("Stopped at Set Count\xE2\x86\x92Total (node 11) on Player ("));
	CHECK(line.ends_with(") (disabled: passes through): breakpoint"));
	CHECK(UI::HasArrows(line));
	REQUIRE(window.GetStripRows()[0].Buttons.size() == 4);
	CHECK(window.GetStripRows()[0].Buttons[2].Action == VisualScriptDebugStripAction::StepInto);
	// Named as the keyboard shows it: Cmd+F11 where ImGui's Ctrl shortcuts take Cmd (macOS).
	CHECK(window.GetStripRows()[0].Buttons[2].SecondChord == (ImGui::GetIO().ConfigMacOSXBehaviors ? "Cmd+F11" : "Ctrl+F11"));

	// A broken edit (a Get of a variable the graph lacks) while stopped: the stopped execution runs on in the last version that
	// worked, which the window's own strip line says.
	VisualScriptDocument broken = document;
	broken.Graph.Nodes.push_back(MakeNode(40, "Variable.Get", glm::vec2(0.0f, 900.0f), Json::object({{"Variable", 99}})));
	REQUIRE(fixture.Operations.SetVisualScriptDocument(graph, broken).IsOk());
	fixture.Frames(3);
	REQUIRE(window.GetState().Live == VisualScriptLiveState::LastWorking);
	std::vector<VisualScriptDebugStripRow> const& rows = window.GetStripRows();
	REQUIRE(rows.size() == 2);
	CHECK(rows[1].Text == "Your last edit has 1 error: this execution continues with the last version that worked");
	REQUIRE(rows[1].Buttons.size() == 1);
	CHECK(rows[1].Buttons[0].Action == VisualScriptDebugStripAction::ShowFirstError);
	CHECK(window.GetItemRect(VisualScriptPanelItem::Stopped).has_value());
	fixture.ClickWindowItem(graph, VisualScriptPanelItem::StripShowFirstError);
	fixture.Frames(2);
	CHECK(window.GetState().Drawer == VisualScriptDrawerMode::Problems);
	fixture.Session.ClearBreakpoints(std::nullopt);
}

TEST_CASE("VisualScriptDebug: the toolbar's buttons continue, step and break, each enabled by the session's state")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Player", graph);
	fixture.Open(graph);
	fixture.Start();
	fixture.Frames(2);
	fixture.CheckDrawingChangesNothing({graph});
	// Running: only Break is enabled; it stops the next stop point of any graph (no graph window is the edit target).
	for (VisualScriptDebugButton const button :
	     {VisualScriptDebugButton::Continue, VisualScriptDebugButton::StepOver, VisualScriptDebugButton::StepInto,
	      VisualScriptDebugButton::StepOut, VisualScriptDebugButton::Break})
	{
		CHECK(fixture.Controls.GetButtonRect(button).has_value());
	}
	fixture.ClickToolbarButton(VisualScriptDebugButton::Continue);
	fixture.Frames(1);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	fixture.ClickToolbarButton(VisualScriptDebugButton::Break);
	fixture.FramesUntilStopped();
	VisualScriptStop const* broken = fixture.Session.FindStop(*fixture.Session.GetSelectedStop());
	REQUIRE(broken != nullptr);
	CHECK(broken->Reason == VisualScriptStopReason::Break);
	CHECK(fixture.Window(graph).GetStripRows().front().Text.ends_with(": break"));

	// Stopped: Step Over stops at the next node, with no scene frame; Continue runs on.
	uint64_t const frame = fixture.GetFrame();
	std::optional<uint32_t> const before = fixture.Window(graph).GetState().DebugNode;
	fixture.ClickToolbarButton(VisualScriptDebugButton::StepOver);
	fixture.Frames(2);
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(fixture.Window(graph).GetState().DebugNode != before);
	CHECK(fixture.GetFrame() == frame);
	fixture.ClickToolbarButton(VisualScriptDebugButton::Continue);
	fixture.Frames(2);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
	CHECK(fixture.GetFrame() > frame);
	// Stopped again by Break: Step Into and Step Out step it too (no call here: each stops at the next instruction or ends).
	fixture.ClickToolbarButton(VisualScriptDebugButton::Break);
	fixture.FramesUntilStopped();
	fixture.ClickToolbarButton(VisualScriptDebugButton::StepInto);
	fixture.Frames(2);
	CHECK(fixture.Session.GetSessionState() != VisualScriptDebugSessionState::Running);
	fixture.ClickToolbarButton(VisualScriptDebugButton::Continue);
	fixture.Frames(2);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
}

TEST_CASE("VisualScriptDebug: the breakpoint popup lists the names a condition can read, and a click on one makes it the condition")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Player", graph);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(14)).IsOk());
	fixture.Open(graph);
	fixture.Start();
	fixture.Frames(2);
	fixture.CheckDrawingChangesNothing({graph});
	VisualScriptEditorPanel& window = fixture.Window(graph);
	window.OpenBreakpointPopup(14);
	fixture.Frames(3);
	REQUIRE(window.GetBreakpointPopupNode() == std::optional<uint32_t>(14));
	VisualScriptConditionCheck const* check = window.GetBreakpointPopupCheck();
	REQUIRE(check != nullptr);
	REQUIRE(check->Checked);
	// The graph's variables are among the names, with their identifier forms.
	auto const count = std::find_if(check->Scope.begin(), check->Scope.end(),
	                                [](VisualScriptConditionName const& name)
	                                {
										return name.Name == "Count";
									});
	REQUIRE(count != check->Scope.end());
	std::string const first = check->Scope.front().Form;
	std::optional<VisualScriptRect> const name = window.GetItemRect(VisualScriptPanelItem::BreakpointScopeName);
	REQUIRE(name.has_value());
	fixture.Gui.Click(ImVec2((name->Min.x + name->Max.x) * 0.5f, (name->Min.y + name->Max.y) * 0.5f), fixture.Drawer());
	fixture.Frames(2);
	VisualScriptBreakpoint const* breakpoint = fixture.Session.GetBreakpoints().Find(graph, 14);
	REQUIRE(breakpoint != nullptr);
	CHECK(breakpoint->Condition == first);
	// The popup's breakpoint is view state: no undo step.
	CHECK(fixture.Context.GetHistory().GetUndoCount() == 0);
}

TEST_CASE("VisualScriptDebug: the Breakpoints tab disables, removes and opens breakpoints, and asks before removing them all, behind "
          "which F5 and F10 do nothing")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Player", graph);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(14)).IsOk());
	fixture.Debugger.SetTab(VisualScriptDebuggerTab::Breakpoints);
	fixture.Frames(2);
	fixture.CheckDrawingChangesNothing({graph});
	VisualScriptBreakpointStore const& store = fixture.Session.GetBreakpoints();

	// Disable All, then the first row's box enables its breakpoint again.
	fixture.ClickPanelItem(VisualScriptDebuggerItem::DisableAll);
	CHECK_FALSE(store.Find(graph, 11)->Enabled);
	CHECK_FALSE(store.Find(graph, 14)->Enabled);
	fixture.ClickPanelItem(VisualScriptDebuggerItem::BreakpointEnabled);
	CHECK(store.Find(graph, 11)->Enabled);
	// A double-click on a row opens its graph's window on the node.
	fixture.ClickPanelItem(VisualScriptDebuggerItem::BreakpointRow, 2);
	fixture.Frames(3);
	REQUIRE(fixture.Editors.FindPanel(graph) != nullptr);
	CHECK(fixture.Window(graph).GetState().SelectedNodes == std::vector<uint32_t>{11});
	// The first row's cross removes its breakpoint.
	fixture.Debugger.Show(VisualScriptDebuggerTab::Breakpoints);
	fixture.Frames(2);
	fixture.ClickPanelItem(VisualScriptDebuggerItem::BreakpointRemove);
	CHECK(store.Find(graph, 11) == nullptr);
	REQUIRE(store.Find(graph, 14) != nullptr);

	// Stopped at 14, Remove All asks first: behind its confirmation F5 and F10 resume nothing; Cancel keeps the breakpoint.
	fixture.Session.SetBreakpointsEnabled(graph, std::array<uint32_t, 1>{14}, true);
	fixture.Start();
	fixture.FramesUntilStopped();
	fixture.Debugger.Show(VisualScriptDebuggerTab::Breakpoints);
	fixture.Frames(2);
	fixture.ClickPanelItem(VisualScriptDebuggerItem::RemoveAll);
	fixture.Frames(1);
	REQUIRE(fixture.Debugger.IsRemoveAllOpen());
	std::optional<uint64_t> const current = fixture.Session.GetState().Current;
	fixture.Gui.Press(ImGuiKey_F5, fixture.Drawer());
	fixture.Gui.Press(ImGuiKey_F10, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	CHECK(fixture.Session.GetState().Current == current);
	REQUIRE(fixture.Debugger.IsRemoveAllOpen());
	fixture.ClickModalItem(VisualScriptDebuggerItem::RemoveAllCancel);
	fixture.Frames(1);
	CHECK_FALSE(fixture.Debugger.IsRemoveAllOpen());
	CHECK(store.Find(graph, 14) != nullptr);
	// Confirmed, every breakpoint goes; F5 then continues.
	fixture.Debugger.RequestRemoveAll();
	fixture.Frames(2);
	REQUIRE(fixture.Debugger.IsRemoveAllOpen());
	fixture.ClickModalItem(VisualScriptDebuggerItem::RemoveAllConfirm);
	fixture.Frames(1);
	CHECK(store.GetGraphs().empty());
	CHECK_FALSE(fixture.Debugger.IsRemoveAllOpen());
	fixture.Gui.Press(ImGuiKey_F5, fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Running);
}

TEST_CASE("VisualScriptDebug: the Instances tab lists the running instances, and a click makes one its graph's target and opens its "
          "window")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Enemy 1", graph);
	fixture.CreateRunner("Enemy 2", graph);
	fixture.Start();
	fixture.Debugger.SetTab(VisualScriptDebuggerTab::Instances);
	fixture.Frames(3);
	fixture.CheckDrawingChangesNothing({graph});
	REQUIRE(fixture.Debugger.GetItemRect(VisualScriptDebuggerItem::InstanceRow).has_value());
	REQUIRE(fixture.Editors.FindPanel(graph) == nullptr);
	fixture.ClickPanelItem(VisualScriptDebuggerItem::InstanceRow);
	fixture.Frames(3);
	// The first row is one of the two enemies: it is the graph's target, and the window opened on it shows it.
	UUID const target = fixture.Session.GetTarget(graph);
	REQUIRE(target.IsValid());
	REQUIRE(fixture.Editors.FindPanel(graph) != nullptr);
	CHECK(fixture.Window(graph).GetDebugTarget() == target);
	CHECK(fixture.Session.GetTarget(graph) == target);
	CHECK(fixture.Ok("graph.debug")["targets"].size() == 1);
}

TEST_CASE("VisualScriptDebug: the Values tab formats colours, vectors, enums and assets as the canvas does, and Evaluate belongs to "
          "its frame")
{
	DebuggerFixture fixture;
	Scene source("Source");
	Entity member = source.CreateEntity("Member");
	Result<Ref<PrefabAsset>> prefab = PrefabAsset::Create(PrefabSerializer::Serialize(source, member));
	REQUIRE(prefab.IsOk());
	AssetHandle const prefabHandle = AssetManager::AddMemoryAsset(prefab.GetValue(), "Crate");
	AssetHandle const graph = fixture.CreateGraph(
		"Scripts/Values.sgraph", MakeVariableDocument({MakeVariable(3, "Tint", "Strada.Color", Json::array({1.0, 0.0, 0.0, 1.0})),
	                                                   MakeVariable(4, "Offset", "Strada.Vector3", Json::array({1.0, 2.5, -3.0})),
	                                                   MakeVariable(5, "Mode", "Strada.ForceMode", "Impulse"),
	                                                   MakeVariable(6, "Spawn", "Strada.Prefab", prefabHandle.ToString())}));
	fixture.CreateRunner("Player", graph);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.FramesUntilStopped();
	fixture.Debugger.SetTab(VisualScriptDebuggerTab::Values);
	fixture.Frames(2);
	fixture.CheckDrawingChangesNothing({graph});

	auto const cell = [&fixture](std::string_view name) -> VisualScriptValueCell
	{
		for (VisualScriptDebuggerPanel::ValueRow const& row : fixture.Debugger.GetValueRows())
		{
			if (row.Name == name && row.Cell)
			{
				return *row.Cell;
			}
		}
		FAIL("the Values tab has no row ", name);
		return {};
	};
	// A colour is a swatch, a vector its literal's text, an enum its enumerator, an asset its name: as on a wire's pill.
	VisualScriptValueCell const tint = cell("Tint");
	CHECK(tint.Kind == VisualScriptValueCellKind::Swatch);
	CHECK(tint.Color.has_value());
	VisualScriptValueCell const offset = cell("Offset");
	CHECK(offset.Kind == VisualScriptValueCellKind::Text);
	CHECK_FALSE(offset.Text.starts_with("["));
	CHECK(offset.Text.find("2.5") != std::string::npos);
	CHECK(cell("Mode").Text == "Impulse");
	CHECK(cell("Spawn").Text == "Crate");
	// The stop node's inputs at Enter are their sources' values, evaluated when it runs.
	auto const inputs = std::find_if(fixture.Debugger.GetValueRows().begin(), fixture.Debugger.GetValueRows().end(),
	                                 [](VisualScriptDebuggerPanel::ValueRow const& row)
	                                 {
										 return row.Name == "Inputs" && !row.Cell;
									 });
	REQUIRE(inputs != fixture.Debugger.GetValueRows().end());
	CHECK(inputs->Note == "evaluated when it runs");

	// The names the formula can read, from the frame's scope; a click inserts one.
	REQUIRE_FALSE(fixture.Debugger.GetEvaluateScope().empty());
	std::string const firstName = fixture.Debugger.GetEvaluateScope().front().Form;
	fixture.ClickPanelItem(VisualScriptDebuggerItem::EvaluateName);
	CHECK(fixture.Debugger.GetEvaluateText() == firstName);
	// Evaluate a vector: its cell is the literal's text, as the row's.
	fixture.Debugger.SetEvaluateText("Offset");
	fixture.Debugger.Evaluate();
	REQUIRE(fixture.Debugger.GetEvaluateCell().has_value());
	CHECK(fixture.Debugger.GetEvaluateCell()->Text == offset.Text);
	// An error says so, with the runtime's suggestion.
	fixture.Debugger.SetEvaluateText("offset");
	fixture.Debugger.Evaluate();
	CHECK_FALSE(fixture.Debugger.GetEvaluateCell().has_value());
	CHECK(fixture.Debugger.GetEvaluateResult().find("Offset") != std::string::npos);
	fixture.Debugger.SetEvaluateText("Count + 41");
	fixture.Debugger.Evaluate();
	CHECK(fixture.Debugger.GetEvaluateResult() == "41");
	// At the next stop the result goes: it was the last frame's.
	fixture.Gui.Press(ImGuiKey_F5, fixture.Drawer());
	fixture.FramesUntilStopped();
	CHECK(fixture.Debugger.GetEvaluateResult().empty());
	CHECK_FALSE(fixture.Debugger.GetEvaluateCell().has_value());
	CHECK(fixture.Debugger.GetEvaluateText() == "Count + 41");
	fixture.Session.ClearBreakpoints(std::nullopt);
}

TEST_CASE("VisualScriptDebug: in a short Debugger panel the Values tab keeps most of its height for the values, the names yielding")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph(
		"Scripts/Values.sgraph", MakeVariableDocument({MakeVariable(3, "Tint", "Strada.Color", Json::array({1.0, 0.0, 0.0, 1.0})),
	                                                   MakeVariable(4, "Offset", "Strada.Vector3", Json::array({1.0, 2.5, -3.0})),
	                                                   MakeVariable(5, "Lives", "int", 3), MakeVariable(6, "Speed", "float", 9.0),
	                                                   MakeVariable(7, "Level", "int", 1), MakeVariable(8, "Combo", "int", 0)}));
	fixture.CreateRunner("Player", graph);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.FramesUntilStopped();
	fixture.CheckDrawingChangesNothing({graph});
	fixture.Debugger.SetTab(VisualScriptDebuggerTab::Values);
	// A panel docked low in the editor, 300 pixels tall at the top of the screen (the fixture floats it at y = 20).
	fixture.DebuggerSize = ImVec2(900.0f, 300.0f);
	fixture.Frames(2);
	REQUIRE(fixture.Debugger.GetEvaluateScope().size() > 6);
	std::optional<VisualScriptDebuggerPanel::ItemRect> const table = fixture.Debugger.GetItemRect(VisualScriptDebuggerItem::ValuesTable);
	std::optional<VisualScriptDebuggerPanel::ItemRect> const name = fixture.Debugger.GetItemRect(VisualScriptDebuggerItem::EvaluateName);
	REQUIRE(table.has_value());
	REQUIRE(name.has_value());
	float const tableHeight = table->Max.y - table->Min.y;
	// The values are the tab's larger part: the names below Evaluate's row, from the first to the panel's bottom, take less.
	float const namesHeight = 20.0f + fixture.DebuggerSize.y - name->Min.y;
	CHECK(name->Min.y > table->Max.y);
	CHECK(tableHeight > namesHeight);

	// A tall panel lists six names, as before.
	fixture.DebuggerSize = ImVec2(900.0f, 900.0f);
	fixture.Frames(2);
	std::optional<VisualScriptDebuggerPanel::ItemRect> const tall = fixture.Debugger.GetItemRect(VisualScriptDebuggerItem::ValuesTable);
	REQUIRE(tall.has_value());
	CHECK(tall->Max.y - tall->Min.y > tableHeight);
	fixture.Session.ClearBreakpoints(std::nullopt);
}

TEST_CASE("VisualScriptDebug: a target an agent set while the graph's window was closed survives the window a stop opens")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Enemy.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Enemy 1", graph);
	fixture.CreateRunner("Enemy 2", graph);
	UUID const third = fixture.CreateRunner("Enemy 3", graph);
	fixture.Start();
	fixture.Frames(1);
	REQUIRE(fixture.Editors.FindPanel(graph) == nullptr);
	fixture.Ok("graph.target", Json::object({{"graph", graph.ToString()}, {"entity", third.ToString()}}));
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());

	// The stop opens the window, which adopts the target instead of replacing it with any instance.
	fixture.FramesUntilStopped();
	fixture.CheckDrawingChangesNothing({graph});
	CHECK(fixture.Window(graph).GetDebugTarget() == third);
	CHECK(fixture.Session.GetTarget(graph) == third);
	Json const debug = fixture.Ok("graph.debug");
	REQUIRE(debug["targets"].size() == 1);
	CHECK(debug["targets"][0]["entity"] == third.ToString());
	REQUIRE(debug["stops"].size() == 1);
	CHECK(debug["stops"][0]["entity"] == third.ToString());
	// Continue: the next frame stops that enemy alone again.
	fixture.Gui.Press(ImGuiKey_F5, fixture.Drawer());
	fixture.FramesUntilStopped();
	REQUIRE(fixture.Session.GetState().Stops.size() == 1);
	CHECK(fixture.Session.GetState().Stops[0].Entity == third);
	fixture.Session.ClearBreakpoints(std::nullopt);
}

TEST_CASE("VisualScriptDebug: graph.debug-panel brings the panel in front of its tab, picks each tab and a stop, and refuses frames "
          "while nothing is stopped")
{
	DebuggerFixture fixture;
	fixture.DockWithConsole = true;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Enemy 1", graph);
	fixture.CreateRunner("Enemy 2", graph);
	fixture.Frames(2);
	fixture.Focus("Console");
	fixture.CheckDrawingChangesNothing({graph});
	// Docked behind the Console, open brings it to the front.
	REQUIRE(fixture.Debugger.IsOpen());
	REQUIRE_FALSE(fixture.Debugger.IsVisible());
	CHECK(fixture.Ok("graph.debug-panel", Json::object({{"open", true}}))["open"] == true);
	fixture.Frames(2);
	CHECK(fixture.Debugger.IsVisible());
	CHECK(fixture.Ok("graph.editors")["debugPanel"]["visible"] == true);

	// Nothing is stopped: a stop or a frame is refused.
	for (Json const& params : {Json::object({{"frame", 0}}), Json::object({{"stop", 1}})})
	{
		CommandResult const refused = fixture.Run("graph.debug-panel", params);
		REQUIRE(refused.IsError());
		CHECK(refused.GetError().Code == AutomationErrorCode::InvalidOperation);
	}

	// Each tab.
	for (std::string_view const tab : {"CallStack", "Values", "Breakpoints", "Instances"})
	{
		CAPTURE(tab);
		CHECK(fixture.Ok("graph.debug-panel", Json::object({{"tab", std::string(tab)}}))["tab"] == std::string(tab));
		fixture.Frames(1);
		CHECK(GetVisualScriptDebuggerTabName(fixture.Debugger.GetTab()) == tab);
	}

	// Two stops: the second is selected, and the panel's call stack shows it.
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.FramesUntilStopped();
	REQUIRE(fixture.Session.GetState().Stops.size() == 2);
	uint64_t const second = fixture.Session.GetState().Stops[1].ID;
	Json const selected = fixture.Ok("graph.debug-panel", Json::object({{"tab", "CallStack"}, {"stop", second}}));
	CHECK(selected["stop"] == second);
	CHECK(selected["frame"] == 0);
	fixture.Frames(2);
	CHECK(fixture.Session.GetSelectedStop() == std::optional<uint64_t>(second));
	REQUIRE_FALSE(fixture.Debugger.GetCallStackRows().empty());
	VisualScriptStop const* stop = fixture.Session.FindStop(second);
	REQUIRE(stop != nullptr);
	std::string const entity = fixture.Context.GetScene().GetEntityByUUID(stop->Entity).GetName();
	CHECK(fixture.Debugger.GetCallStackRows()[0].Text.find("on " + entity) != std::string::npos);
	fixture.Session.ClearBreakpoints(std::nullopt);
}

TEST_CASE("VisualScriptDebug: the input the controls took is not owed back once play stops, nor in the next play session")
{
	DebuggerFixture fixture;
	fixture.GameViewVisible = true;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Counter.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Player", graph);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.Input.Grant();
	fixture.FramesUntilStopped();
	fixture.CheckDrawingChangesNothing({graph});
	REQUIRE(fixture.Controls.HasTakenInput());
	fixture.Ok("play.stop");
	fixture.Frames(1);
	CHECK_FALSE(fixture.Controls.HasTakenInput());
	// The next session starts with nothing owed: a resume gives nothing.
	fixture.Session.ClearBreakpoints(std::nullopt);
	fixture.Start();
	fixture.Frames(2);
	CHECK_FALSE(fixture.Controls.HasTakenInput());
	CHECK_FALSE(fixture.Input.HasInput());
}

TEST_CASE("VisualScriptDebug: the Instance combo offers any instance while playing, and names a chosen entity destroyed since")
{
	DebuggerFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Enemy.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Enemy 1", graph);
	UUID const second = fixture.CreateRunner("Enemy 2", graph);
	fixture.Open(graph);
	fixture.CheckDrawingChangesNothing({graph});
	VisualScriptEditorPanel& window = fixture.Window(graph);
	CHECK(window.GetInstanceText() == "Instance: None");
	fixture.Start();
	fixture.Frames(2);
	CHECK(window.GetInstanceText() == "Instance: Any instance");
	window.ChooseContextEntity(second);
	fixture.Frames(2);
	CHECK(window.GetInstanceText() == "Instance: Enemy 2");
	CHECK(fixture.Session.GetTarget(graph) == second);

	// Destroyed while playing: the choice is kept, and the debugger then stops none of the graph's executions.
	fixture.Ok("entity.delete", Json::object({{"entities", Json::array({second.ToString()})}}));
	fixture.Frames(2);
	CHECK(window.GetInstanceText() == "Instance: Enemy 2 (destroyed)");
	CHECK(window.GetDebugTarget() == second);
	CHECK(fixture.Session.GetTarget(graph) == second);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Frames(3);
	CHECK(fixture.Session.GetState().Stops.empty());
	CHECK(fixture.DebugInput(graph).Breakpoints.front().InactiveReason == "the target " + second.ToString() + " was destroyed");
	fixture.Session.ClearBreakpoints(std::nullopt);
}

TEST_CASE("VisualScriptDebug: with Wire Values set to Always, only the windows that show their canvas read live values")
{
	DebuggerFixture fixture;
	fixture.DockWithConsole = true;
	fixture.Frames(1);
	fixture.Editors.SetDefaultDockProvider(
		[&fixture]() -> std::optional<ImGuiID>
		{
			return fixture.Dockspace;
		});
	AssetHandle const behind = fixture.CreateGraph("Scripts/Behind.sgraph", MakeCounterDocument());
	AssetHandle const front = fixture.CreateGraph("Scripts/Front.sgraph", MakeCounterDocument());
	fixture.CreateRunner("Back", behind);
	fixture.CreateRunner("Front", front);
	fixture.Settings.SetWireValues(VisualScriptWireValues::Always);
	fixture.Open(behind);
	fixture.Open(front);
	fixture.Focus(VisualScriptEditorPanel::GetWindowName(front, {}).c_str());
	fixture.CheckDrawingChangesNothing({behind, front});
	REQUIRE(fixture.Window(front).IsVisible());
	REQUIRE_FALSE(fixture.Window(behind).IsVisible());
	fixture.Start();
	fixture.Frames(8);
	// The tab in front shows its instance's values on its wires; the one behind it reads none.
	CHECK_FALSE(fixture.DebugInput(front).Values.empty());
	CHECK(fixture.DebugInput(behind).Values.empty());
	fixture.Focus(VisualScriptEditorPanel::GetWindowName(behind, {}).c_str());
	fixture.Frames(8);
	CHECK_FALSE(fixture.DebugInput(behind).Values.empty());
}

TEST_CASE("VisualScriptDebug: the Values tab shows an active loop's position and the computed nodes by their titles")
{
	DebuggerFixture fixture;
	// On every update a For Loop (node 20) from 0 to 9 adds one to Count (node 11) in its Body.
	VisualScriptDocument document = MakeCounterDocument();
	std::erase_if(document.Graph.Links,
	              [](VisualScriptLink const& link)
	              {
					  return link.To.Pin == "Execute";
				  });
	document.Graph.Nodes.push_back(MakeNode(20, "Flow.ForLoop", glm::vec2(160.0f, -300.0f), Json::object(), Json::object({{"Last", 9}})));
	document.Graph.Links.push_back(MakeLink(10, "Then", 20, "Execute"));
	document.Graph.Links.push_back(MakeLink(20, "Body", 11, "Execute"));
	AssetHandle const graph = fixture.CreateGraph("Scripts/Loop.sgraph", document);
	fixture.CreateRunner("Player", graph);
	REQUIRE(fixture.Session.SetBreakpoint(graph, MakeBreakpoint(11)).IsOk());
	fixture.Start();
	fixture.FramesUntilStopped();
	// The third iteration.
	fixture.Gui.Press(ImGuiKey_F5, fixture.Drawer());
	fixture.FramesUntilStopped();
	fixture.Gui.Press(ImGuiKey_F5, fixture.Drawer());
	fixture.FramesUntilStopped();
	fixture.Debugger.SetTab(VisualScriptDebuggerTab::Values);
	fixture.Frames(2);
	fixture.CheckDrawingChangesNothing({graph});
	std::vector<VisualScriptDebuggerPanel::ValueRow> const& rows = fixture.Debugger.GetValueRows();
	auto const loop = std::find_if(rows.begin(), rows.end(),
	                               [](VisualScriptDebuggerPanel::ValueRow const& row)
	                               {
									   return row.Name == "For Loop (node 20)";
								   });
	REQUIRE(loop != rows.end());
	CHECK(loop->Note == "Index 2 of 0 \xE2\x80\xA6 9");
	// The Get and the Add the stopped execution computed, each section titled with its node.
	CHECK(std::any_of(rows.begin(), rows.end(),
	                  [](VisualScriptDebuggerPanel::ValueRow const& row)
	                  {
						  return !row.Cell && row.Name.starts_with("Computed by this execution: ") && row.Name.ends_with(" (node 20)");
					  }));
	fixture.Session.ClearBreakpoints(std::nullopt);
}
