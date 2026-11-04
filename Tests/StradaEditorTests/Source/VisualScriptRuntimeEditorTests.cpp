#include "Fuzzing.h"
#include "HeadlessImGui.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"
#include "VisualScriptPanelTestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/GraphDebugCommands.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/Commands/EntityCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorEditingKeys.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/Panels/VisualScriptEditorPanel.h"
#include "Editor/PlayMode.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptCanvasRenderer.h"
#include "Editor/VisualScripting/VisualScriptCurveEditor.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"
#include "Editor/VisualScripting/VisualScriptDetails.h"
#include "Editor/VisualScripting/VisualScriptDrawer.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptEditors.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptQuickFixes.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptTimeline.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Script/VisualScriptDebugger.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Script/VisualScriptLive.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace Strada;

// The graph window with the scripting runtime: the engine's node library and analysis, and play mode (spec §15.4's runtime
// tests). Undo is the editor's Ctrl+Z, EditorOperations::Undo.
namespace
{
	using Testing::PanelFixture;
	using Testing::ToImVec2;

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

	VisualScriptLink MakeLink(uint32_t fromNode, std::string fromPin, uint32_t toNode, std::string toPin)
	{
		return {{fromNode, std::move(fromPin)}, {toNode, std::move(toPin)}};
	}

	// Create -> Delay 0.5 s -> Log Info "Waited" -> Set Done to true.
	VisualScriptDocument MakeDelayDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Done", "bool", false));
		document.Graph.Nodes = {
			MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
			MakeNode(11, "Flow.Delay", glm::vec2(320.0f, 0.0f), Json::object(), Json::object({{"Duration", 0.5}})),
			MakeNode(13, "Call:Strada.Log.Info(string)", glm::vec2(640.0f, 0.0f), Json::object(), Json::object({{"Message", "Waited"}})),
			MakeNode(12, "Variable.Set", glm::vec2(960.0f, 0.0f), Json::object({{"Variable", 1}}), Json::object({{"Value", true}})),
		};
		document.Graph.Links = {MakeLink(10, "Then", 11, "Execute"), MakeLink(11, "Completed", 13, "Execute"),
		                        MakeLink(13, "Then", 12, "Execute")};
		return document;
	}

	VisualScriptDocument LoadTestDocument(std::string_view name)
	{
		Result<std::string> text = FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" /
		                                                    "Documents" / FileSystem::PathFromUtf8(name));
		REQUIRE_MESSAGE(text.IsOk(), (text ? std::string() : text.GetError()));
		Result<Json> json = ParseJson(text.GetValue());
		REQUIRE(json.IsOk());
		Result<VisualScriptDocument> document =
			VisualScriptSerializer::DeserializeDocument(json.GetValue(), AssetManager::CreateDeserializationContext());
		REQUIRE_MESSAGE(document.IsOk(), (document ? std::string() : document.GetError()));
		return document.TakeValue();
	}

	// The graph window of a project's script with the scripting runtime's semantics, and play mode as the editor runs it.
	struct RuntimeFixture
	{
		explicit RuntimeFixture(VisualScriptDocument const& document)
			: Window(document, &Semantics)
		{
		}

		// An entity of the edited scene that runs the graph.
		UUID CreateRunner(std::string name)
		{
			EntityCreateInfo info;
			info.Name = std::move(name);
			info.Components = Json::object({{"VisualScript", Json::object({{"Graph", Window.Graph.ToString()}})}});
			Result<UUID> const entity = Window.Operations.CreateEntity(info);
			REQUIRE_MESSAGE(entity.IsOk(), (entity ? std::string() : entity.GetError()));
			return entity.GetValue();
		}

		ScriptFieldValue GetVariable(UUID entity, std::string const& name) const
		{
			Result<ScriptFieldMap> const values = VisualScriptEngine::GetInstanceVariables(entity);
			REQUIRE_MESSAGE(values.IsOk(), (values ? std::string() : values.GetError()));
			auto const found = values.GetValue().find(name);
			REQUIRE(found != values.GetValue().end());
			return found->second;
		}

		VisualScriptEditorModel const& Model() const { return Window.Model(); }

		Testing::ScriptRuntimeScope Runtime;
		EngineVisualScriptSemantics Semantics;
		PanelFixture Window;
		EditorScripts Scripts{Window.Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Window.Context, Scripts};
	};
}

TEST_CASE("VisualScriptEditor: with the scripting runtime, wildcard pins are drawn with their resolved types and an implicit "
          "conversion draws a gradient")
{
	// Data.json converts with Convert nodes only: an int read into a float variable adds an implicit conversion.
	VisualScriptDocument document = LoadTestDocument("Data.json");
	uint32_t next = 0;
	for (VisualScriptNode const& node : document.Graph.Nodes)
	{
		next = std::max(next, node.ID + 1);
	}
	for (VisualScriptVariable const& variable : document.Variables)
	{
		next = std::max(next, variable.ID + 1);
	}
	auto const findVariable = [&document](std::string_view name)
	{
		auto const found = std::find_if(document.Variables.begin(), document.Variables.end(),
		                                [name](VisualScriptVariable const& variable)
		                                {
											return variable.Name == name;
										});
		REQUIRE(found != document.Variables.end());
		REQUIRE((found->Type == "int" || found->Type == "float"));
		return found->ID;
	};
	document.Graph.Nodes.push_back(
		MakeNode(next, "Variable.Get", glm::vec2(0.0f, -640.0f), Json::object({{"Variable", findVariable("Count")}})));
	document.Graph.Nodes.push_back(
		MakeNode(next + 1, "Variable.Set", glm::vec2(320.0f, -640.0f), Json::object({{"Variable", findVariable("Ratio")}})));
	document.Graph.Links.push_back(MakeLink(next, "Value", next + 1, "Value"));
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();
	REQUIRE(fixture.Model().GetState().Analyzed);
	VisualScriptCanvasModel const& canvas = fixture.Model().GetCanvas();
	VisualScriptLibrary const* library = fixture.Semantics.GetLibrary().get();
	REQUIRE(library != nullptr);

	// The data pins of reroutes and selects, wildcards in the library, are typed as the analysis resolved them.
	size_t resolved = 0;
	for (VisualScriptNodeVisual const& node : canvas.GetNodes())
	{
		VisualScriptNode const* stored = nullptr;
		for (VisualScriptNode const& candidate : fixture.Window.GetDocument().Graph.Nodes)
		{
			if (candidate.ID == node.ID)
			{
				stored = &candidate;
			}
		}
		REQUIRE(stored != nullptr);
		VisualScriptNodeTemplate const* entry = library->FindNode(stored->Type);
		if (entry == nullptr)
		{
			continue;
		}
		for (VisualScriptPinVisual const& pin : canvas.GetPins(node))
		{
			for (VisualScriptPin const& templatePin : entry->Pins)
			{
				if (templatePin.Name == canvas.GetText(pin.Name) && templatePin.Direction == pin.Direction &&
				    templatePin.Type == VisualScriptWildcardType && pin.HasFlag(VisualScriptPinFlags::Linked))
				{
					CAPTURE(stored->Type);
					CHECK(pin.Type != VisualScriptWildcardType);
					resolved++;
				}
			}
		}
	}
	CHECK(resolved > 0);

	// A link between pins of different types that convert: its wire fades from one type's colour to the other's.
	size_t converted = 0;
	for (VisualScriptLinkVisual const& link : canvas.GetLinks())
	{
		if (link.HasFlag(VisualScriptLinkFlags::Converted))
		{
			CHECK(link.FromColor != link.ToColor);
			converted++;
		}
	}
	CHECK(converted > 0);
	CHECK(fixture.Window.Panel->GetRenderer().GetStats().DrawnWires > 0);
}

TEST_CASE("VisualScriptEditor: a node the runtime resolves under another type is written canonical by the next edit, in that "
          "edit's step")
{
	VisualScriptDocument document;
	document.Graph.Nodes = {
		MakeNode(1, "Event.Create", glm::vec2(0.0f, 0.0f)),
		MakeNode(2, "Call:Strada.Log.Info", glm::vec2(320.0f, 0.0f), Json::object(), Json::object({{"Message", "Hi"}})),
		MakeNode(3, "Flow.Delay", glm::vec2(320.0f, 320.0f)),
	};
	document.Graph.Links = {MakeLink(1, "Then", 2, "Execute")};
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();
	std::string const bytes = fixture.Window.ReadFile();
	REQUIRE(fixture.Model().GetAnalysis() != nullptr);
	VisualScriptAnalyzedNode const* analyzed = fixture.Model().GetAnalysis()->FindNode(2);
	REQUIRE(analyzed != nullptr);
	REQUIRE(analyzed->Resolved == VisualScriptResolution::Unique);
	VisualScriptEditorState state = fixture.Window.Panel->GetState();
	CHECK(std::find(state.Notices.begin(), state.Notices.end(), VisualScriptNotice::Normalization) != state.Notices.end());
	size_t const undo = fixture.Window.GetUndoCount();

	// Moving another node: one step, which also stores Log Info under its full type, and says so.
	glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f));
	fixture.Window.Gui.DragPath(ToImVec2(from), ToImVec2(from + glm::vec2(64.0f, 64.0f)), fixture.Window.Drawer());
	REQUIRE(fixture.Window.GetUndoCount() == undo + 1);
	CHECK(fixture.Window.FindNode(2)->Type == "Call:Strada.Log.Info(string)");
	fixture.Window.Frames(1);
	state = fixture.Window.Panel->GetState();
	CHECK(std::any_of(state.Toasts.begin(), state.Toasts.end(),
	                  [](std::string const& toast)
	                  {
						  return toast.find("Call:Strada.Log.Info(string)") != std::string::npos;
					  }));
	CHECK(std::find(state.Notices.begin(), state.Notices.end(), VisualScriptNotice::Normalization) == state.Notices.end());

	// One undo gives the file its bytes back.
	REQUIRE(fixture.Window.Operations.Undo().IsOk());
	CHECK(fixture.Window.ReadFile() == bytes);
}

TEST_CASE("VisualScriptEditor: a split Vector3 input shows three float pins that take links, and the runtime reads them")
{
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "Push", "float", 2.0));
	document.Graph.Nodes = {
		MakeNode(10, "Event.Update", glm::vec2(0.0f, 0.0f)),
		MakeNode(11, "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)", glm::vec2(480.0f, 0.0f)),
		MakeNode(12, "Variable.Get", glm::vec2(0.0f, 320.0f), Json::object({{"Variable", 1}})),
	};
	document.Graph.Nodes[1].Split.Inputs = {"Force"};
	document.Graph.Links = {MakeLink(10, "Then", 11, "Execute")};
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();

	VisualScriptCanvasModel const& canvas = fixture.Model().GetCanvas();
	std::optional<uint32_t> const node = canvas.FindNode(11);
	REQUIRE(node.has_value());
	for (std::string_view const field : {"Force.X", "Force.Y", "Force.Z"})
	{
		CAPTURE(field);
		std::optional<uint32_t> const pin = canvas.FindPin(*node, field, VisualScriptPinDirection::In);
		REQUIRE(pin.has_value());
		CHECK(canvas.GetPins()[*pin].Type == "float");
	}
	CHECK_FALSE(canvas.FindPin(*node, "Force", VisualScriptPinDirection::In).has_value());

	// A wire from the float variable to Force.Y: one step, stored with the field's path, and the runtime finds no problem in it.
	size_t const undo = fixture.Window.GetUndoCount();
	glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), 12, "Value", VisualScriptPinDirection::Out);
	glm::vec2 const to = Testing::CanvasPoint(fixture.Model(), 11, "Force.Y");
	fixture.Window.Gui.DragPath(ToImVec2(from), ToImVec2(to), fixture.Window.Drawer(), ImGuiMouseButton_Left, 8);
	REQUIRE(fixture.Window.GetUndoCount() == undo + 1);
	CHECK(fixture.Window.HasLink(MakeLink(12, "Value", 11, "Force.Y")));
	fixture.Window.Frames(2);
	REQUIRE(fixture.Model().GetAnalysis() != nullptr);
	for (VisualScriptDiagnostic const& diagnostic : fixture.Model().GetAnalysis()->Diagnostics)
	{
		CAPTURE(diagnostic.Message);
		CHECK(diagnostic.Severity != VisualScriptSeverity::Error);
	}
}

TEST_CASE("VisualScriptEditor: a node disabled from the canvas runs as a pass-through")
{
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "First", "bool", false));
	document.Variables.push_back(MakeVariable(2, "Second", "bool", false));
	document.Graph.Nodes = {
		MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
		MakeNode(11, "Variable.Set", glm::vec2(320.0f, 0.0f), Json::object({{"Variable", 1}}), Json::object({{"Value", true}})),
		MakeNode(12, "Variable.Set", glm::vec2(640.0f, 0.0f), Json::object({{"Variable", 2}}), Json::object({{"Value", true}})),
	};
	document.Graph.Links = {MakeLink(10, "Then", 11, "Execute"), MakeLink(11, "Then", 12, "Execute")};
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();
	UUID const entity = fixture.CreateRunner("Switch");

	// M on the first Set: one step.
	size_t const undo = fixture.Window.GetUndoCount();
	fixture.Window.Click(Testing::CanvasPoint(fixture.Model(), 11, glm::vec2(0.5f, 0.06f)));
	fixture.Window.Gui.Press(ImGuiKey_M, fixture.Window.Drawer());
	fixture.Window.Frames(1);
	REQUIRE(fixture.Window.GetUndoCount() == undo + 1);
	REQUIRE(fixture.Window.FindNode(11)->Enabled == VisualScriptNodeEnabled::Disabled);
	VisualScriptAnalyzedNode const* analyzed = fixture.Model().GetAnalysis()->FindNode(11);
	REQUIRE(analyzed != nullptr);
	CHECK(analyzed->PassThrough == "Then");

	// The flow goes through it: the second Set runs, the first does not.
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(2, Timestep(FrameTime));
	CHECK(fixture.GetVariable(entity, "Second") == ScriptFieldValue::FromBool(true));
	CHECK(fixture.GetVariable(entity, "First") == ScriptFieldValue::FromBool(false));
	fixture.Play.Stop();
}

TEST_CASE("VisualScriptEditor: a node of a type the runtime does not know can be disabled, which ends its chain and lets the "
          "graph run")
{
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "First", "bool", false));
	document.Variables.push_back(MakeVariable(2, "Second", "bool", false));
	document.Graph.Nodes = {
		MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
		MakeNode(11, "Variable.Set", glm::vec2(320.0f, 0.0f), Json::object({{"Variable", 1}}), Json::object({{"Value", true}})),
		MakeNode(12, "Flow.NoSuchNode", glm::vec2(640.0f, 0.0f)),
		MakeNode(13, "Variable.Set", glm::vec2(960.0f, 0.0f), Json::object({{"Variable", 2}}), Json::object({{"Value", true}})),
	};
	document.Graph.Links = {MakeLink(10, "Then", 11, "Execute"), MakeLink(11, "Then", 12, "Execute"), MakeLink(12, "Then", 13, "Execute")};
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();
	UUID const entity = fixture.CreateRunner("Switch");
	REQUIRE(fixture.Model().GetAnalysis() != nullptr);
	CHECK(fixture.Model().GetState().Errors > 0);

	// The canvas and Details offer the states for it (spec §21: disabling an unknown node is how its graph keeps running).
	VisualScriptNode const* unknown = fixture.Window.FindNode(12);
	REQUIRE(unknown != nullptr);
	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);
	CHECK(GetVisualScriptEnabledOffer(fixture.Model(), library.get(), *unknown) == VisualScriptEnabledOffer::Offered);
	size_t const undo = fixture.Window.GetUndoCount();
	fixture.Window.Click(Testing::CanvasPoint(fixture.Model(), 12, glm::vec2(0.5f, 0.06f)));
	fixture.Window.Gui.Press(ImGuiKey_M, fixture.Window.Drawer());
	fixture.Window.Frames(2);
	REQUIRE(fixture.Window.GetUndoCount() == undo + 1);
	REQUIRE(fixture.Window.FindNode(12)->Enabled == VisualScriptNodeEnabled::Disabled);
	REQUIRE(fixture.Model().GetAnalysis() != nullptr);
	for (VisualScriptDiagnostic const& diagnostic : fixture.Model().GetAnalysis()->Diagnostics)
	{
		CAPTURE(diagnostic.Message);
		CHECK(diagnostic.Severity != VisualScriptSeverity::Error);
	}

	// The graph runs up to the unknown node, whose chain ends there.
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(2, Timestep(FrameTime));
	CHECK(fixture.GetVariable(entity, "First") == ScriptFieldValue::FromBool(true));
	CHECK(fixture.GetVariable(entity, "Second") == ScriptFieldValue::FromBool(false));
	fixture.Play.Stop();
}

TEST_CASE("VisualScriptEditor: while playing, moving a node sends nothing and a literal changed on the canvas reaches the "
          "running graph, whose waiting Delay completes; after Stop undo restores the file")
{
	RuntimeFixture fixture(MakeDelayDocument());
	fixture.Window.CheckDrawingChangesNothing();
	UUID const waiter = fixture.CreateRunner("Waiter");
	std::string const bytes = fixture.Window.ReadFile();
	size_t const edited = fixture.Window.GetUndoCount();

	// Playing has a history of its own, which Stop hands back to the edited scene's.
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	size_t const undo = fixture.Window.GetUndoCount();
	fixture.Play.Advance(12, Timestep(FrameTime));
	fixture.Window.Frames(2);
	CHECK(fixture.Window.Panel->GetState().Live == VisualScriptLiveState::Live);
	CHECK(fixture.Window.Panel->GetContextEntity() == waiter);
	CHECK(fixture.GetVariable(waiter, "Done") == ScriptFieldValue::FromBool(false));
	uint64_t const generation = VisualScriptEngine::GetProgramGeneration(fixture.Window.Graph);

	// The context entity's values, read at most 4 times a second.
	uint64_t const reads = fixture.Window.Panel->GetActivity().LiveValueReads;
	fixture.Window.Frames(60, FrameTime);
	uint64_t const second = fixture.Window.Panel->GetActivity().LiveValueReads - reads;
	CHECK(second >= 4);
	CHECK(second <= 5);
	REQUIRE(fixture.Window.Panel->GetLiveValues().has_value());
	std::vector<VisualScriptDebugValue> const& variables = fixture.Window.Panel->GetLiveValues()->Variables;
	CHECK(std::any_of(variables.begin(), variables.end(),
	                  [](VisualScriptDebugValue const& value)
	                  {
						  return value.Name == "Done" && value.Value == Json(false);
					  }));
	// My Script shows them in its rows' value cells.
	CHECK(fixture.Window.Panel->GetSidebar().GetLiveValues().contains("Done"));
	CHECK(fixture.Window.Panel->GetActivity().LiveValueReads - reads == second);

	// A move: one step, the same program, before and after the next frame.
	glm::vec2 const header = Testing::CanvasPoint(fixture.Model(), 12, glm::vec2(0.5f, 0.06f));
	fixture.Window.Gui.DragPath(ToImVec2(header), ToImVec2(header + glm::vec2(64.0f, 64.0f)), fixture.Window.Drawer());
	REQUIRE(fixture.Window.GetUndoCount() == undo + 1);
	CHECK(VisualScriptEngine::GetProgramGeneration(fixture.Window.Graph) == generation);
	fixture.Play.Advance(1, Timestep(FrameTime));
	CHECK(VisualScriptEngine::GetProgramGeneration(fixture.Window.Graph) == generation);

	// The Log's message typed in its chip's live widget: one step, and a new program the running graph takes in the commit's
	// own frame (spec-slice4 section 8.1), before any update; the waiting Delay survives it.
	VisualScriptCanvasModel const& canvas = fixture.Model().GetCanvas();
	std::optional<uint32_t> const log = canvas.FindNode(13);
	REQUIRE(log.has_value());
	std::optional<uint32_t> const messagePin = canvas.FindPin(*log, "Message", VisualScriptPinDirection::In);
	REQUIRE(messagePin.has_value());
	glm::vec2 const chip = fixture.Model().GetView().ToScreen(
		VisualScriptGeometry::GetChipRect(canvas.GetNodes()[*log], canvas.GetPins()[*messagePin]).GetCenter());
	fixture.Window.MoveMouse(chip);
	fixture.Window.Frames(1);
	REQUIRE(fixture.Window.Panel->GetLiveWidget().has_value());
	fixture.Window.Click(chip);
	fixture.Window.Frames(1);
	REQUIRE(ImGui::GetIO().WantTextInput);
	fixture.Window.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Window.Drawer());
	ImGui::GetIO().AddInputCharactersUTF8("Bye");
	fixture.Window.Frames(1);
	fixture.Window.Gui.Press(ImGuiKey_Enter, fixture.Window.Drawer());
	fixture.Window.Frames(2);
	REQUIRE(fixture.Window.GetUndoCount() == undo + 2);
	CHECK(fixture.Window.FindNode(13)->Inputs["Message"] == "Bye");
	CHECK(VisualScriptEngine::GetProgramGeneration(fixture.Window.Graph) > generation);
	CHECK(fixture.GetVariable(waiter, "Done") == ScriptFieldValue::FromBool(false));
	fixture.Play.Advance(31, Timestep(FrameTime));
	CHECK(fixture.GetVariable(waiter, "Done") == ScriptFieldValue::FromBool(true));

	// An edit that breaks the graph: the game keeps the last working version, and the pill says so.
	VisualScriptDocument broken = fixture.Window.GetDocument();
	broken.Graph.Nodes.push_back(MakeNode(20, "Call:Strada.Nothing.Here()", glm::vec2(0.0f, 320.0f)));
	broken.Graph.Links.push_back(MakeLink(12, "Then", 20, "Execute"));
	REQUIRE(fixture.Window.Operations.SetVisualScriptDocument(fixture.Window.Graph, broken).IsOk());
	fixture.Play.Advance(1, Timestep(FrameTime));
	fixture.Window.Frames(2);
	REQUIRE(VisualScriptEngine::HasWorkingProgram(fixture.Window.Graph));
	CHECK(fixture.Window.Panel->GetState().Live == VisualScriptLiveState::LastWorking);

	// After Stop the steps made while playing stay undoable, and undoing them gives the file its bytes back.
	fixture.Play.Stop();
	fixture.Window.Frames(1);
	CHECK(fixture.Window.Panel->GetState().Live == VisualScriptLiveState::None);
	REQUIRE(fixture.Window.GetUndoCount() == edited + 3);
	for (int step = 0; step < 3; step++)
	{
		REQUIRE(fixture.Window.Operations.Undo().IsOk());
	}
	CHECK(fixture.Window.ReadFile() == bytes);
}

TEST_CASE("VisualScriptEditor: the runtime's analysis offers Remove Split for an unused split and Enable Node for a disabled node "
          "that runs, and each fix removes its problem")
{
	Testing::ScriptRuntimeScope runtime;
	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);
	// A text input split (a string has no fields), and a pure Get disabled (it runs whenever it is read).
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "Speed", "float", 1.0));
	document.Graph.Nodes = {
		MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
		MakeNode(11, "Call:Strada.Log.Info(string)", glm::vec2(320.0f, 0.0f), Json::object(), Json::object({{"Message", "Hi"}})),
		MakeNode(12, "Variable.Get", glm::vec2(0.0f, 300.0f), Json::object({{"Variable", 1}})),
	};
	document.Graph.Nodes[1].Split.Inputs = {"Message"};
	document.Graph.Nodes[2].Enabled = VisualScriptNodeEnabled::Disabled;
	document.Graph.Links = {MakeLink(10, "Then", 11, "Execute")};

	auto const find = [](VisualScriptAnalysis const& analysis, std::string_view code) -> VisualScriptDiagnostic const*
	{
		auto const found = std::find_if(analysis.Diagnostics.begin(), analysis.Diagnostics.end(),
		                                [code](VisualScriptDiagnostic const& diagnostic)
		                                {
											return diagnostic.Code == code;
										});
		return found != analysis.Diagnostics.end() ? &*found : nullptr;
	};
	struct Case
	{
		char const* Code;
		char const* Label;
		VisualScriptFixKind Kind;
	};
	for (Case const& problem : {Case{"UnusedSplit", "Remove Split", VisualScriptFixKind::RemoveSplit},
	                            Case{"DisabledIgnored", "Enable Node", VisualScriptFixKind::EnableNode}})
	{
		CAPTURE(problem.Code);
		Result<Ref<VisualScriptAnalysis const>> const analysis = VisualScriptEngine::Analyze(document);
		REQUIRE_MESSAGE(analysis.IsOk(), (analysis ? std::string() : analysis.GetError()));
		VisualScriptDiagnostic const* diagnostic = find(*analysis.GetValue(), problem.Code);
		REQUIRE(diagnostic != nullptr);
		CHECK(diagnostic->Severity == VisualScriptSeverity::Warning);
		std::vector<VisualScriptQuickFix> const fixes = GetVisualScriptQuickFixes(document, *analysis.GetValue(), *diagnostic, *library);
		REQUIRE(fixes.size() == 1);
		CHECK(fixes[0].Label == problem.Label);
		CHECK(fixes[0].Kind == problem.Kind);
		VisualScriptDocument fixed = document;
		REQUIRE(ApplyVisualScriptQuickFix(fixed, *analysis.GetValue(), *diagnostic, fixes[0]).IsOk());
		Result<Ref<VisualScriptAnalysis const>> const after = VisualScriptEngine::Analyze(fixed);
		REQUIRE(after.IsOk());
		CHECK(find(*after.GetValue(), problem.Code) == nullptr);
	}
}

TEST_CASE("VisualScriptEditor: the problems drawer's Edit in Details of a setting the runtime refuses focuses that setting")
{
	// A Sequence with more outputs than it may have.
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
	                        MakeNode(11, "Flow.Sequence", glm::vec2(320.0f, 0.0f), Json::object({{"Count", 99}}))};
	document.Graph.Links = {MakeLink(10, "Then", 11, "Execute")};
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();
	VisualScriptOpenRequest request;
	request.Graph = fixture.Window.Graph;
	request.ShowProblems = true;
	fixture.Window.Panel->Apply(request);
	fixture.Window.Frames(3);
	VisualScriptDrawer const& drawer = fixture.Window.Panel->GetDrawer();
	REQUIRE(drawer.GetProblems().size() == 1);
	CHECK(drawer.GetProblems()[0].Code == "InvalidSetting");
	CHECK(drawer.GetProblems()[0].Node == std::optional<uint32_t>(11));
	std::optional<std::pair<glm::vec2, glm::vec2>> const fix = drawer.FindFixButton(0);
	REQUIRE(fix.has_value());

	// Navigating: the node is selected and Details gives its Count field the keyboard; nothing is edited.
	size_t const undo = fixture.Window.GetUndoCount();
	fixture.Window.Click((fix->first + fix->second) * 0.5f);
	fixture.Window.Frames(3);
	CHECK(fixture.Window.GetUndoCount() == undo);
	std::span<uint32_t const> const selection = fixture.Model().GetSelection();
	CHECK(std::vector<uint32_t>(selection.begin(), selection.end()) == std::vector<uint32_t>{11});
	VisualScriptDetails const& details = fixture.Window.Panel->GetDetails();
	REQUIRE(details.GetSubject() == VisualScriptDetailsSubject::Node);
	std::optional<std::pair<glm::vec2, glm::vec2>> const count = details.FindRow("Count");
	REQUIRE(count.has_value());
	ImGuiContext const& gui = *GImGui;
	REQUIRE(gui.ActiveId != 0);
	REQUIRE(gui.ActiveIdWindow != nullptr);
	CHECK(gui.InputTextState.ID == gui.ActiveId);
	// The active field is in Count's row.
	ImRect const field = ImGui::WindowRectRelToAbs(gui.ActiveIdWindow, gui.ActiveIdWindow->NavRectRel[0]);
	glm::vec2 const center((field.Min.x + field.Max.x) * 0.5f, (field.Min.y + field.Max.y) * 0.5f);
	CHECK(center.y >= count->first.y);
	CHECK(center.y <= count->second.y);
	CHECK_FALSE(fixture.Window.Panel->GetPaneShared().FocusSetting.has_value());
}

TEST_CASE("VisualScriptEditor: the runtime's formula diagnostics carry their span to Details' caret block, which lists the typed inputs")
{
	// A for loop's Index (an int) linked into b, a unlinked (a float): Inputs list both types. The second formula calls a
	// function the runtime does not have.
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)), MakeNode(11, "Flow.ForLoop", glm::vec2(320.0f, 0.0f)),
	                        MakeNode(12, "Expression", glm::vec2(640.0f, 0.0f), Json::object({{"Formula", "a * b"}})),
	                        MakeNode(13, "Expression", glm::vec2(640.0f, 320.0f), Json::object({{"Formula", "a * b + sni(t)"}}))};
	document.Graph.Links = {MakeLink(10, "Then", 11, "Execute"), MakeLink(11, "Index", 12, "b")};
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();

	// The C++ diagnostic has the setting and the span the runtime reported.
	VisualScriptAnalysis const* const analysis = fixture.Model().GetAnalysis();
	REQUIRE(analysis != nullptr);
	auto const unknown = std::find_if(analysis->Diagnostics.begin(), analysis->Diagnostics.end(),
	                                  [](VisualScriptDiagnostic const& diagnostic)
	                                  {
										  return diagnostic.Code == "InvalidExpression" && diagnostic.Node == std::optional<uint32_t>(13);
									  });
	REQUIRE(unknown != analysis->Diagnostics.end());
	CHECK(unknown->Setting == "Formula");
	CHECK(unknown->Span == std::optional<std::pair<uint32_t, uint32_t>>(std::pair<uint32_t, uint32_t>(8, 11)));
	CHECK(unknown->Message == "unknown function sni: did you mean sin?");

	fixture.Window.Click(Testing::CanvasPoint(fixture.Model(), 12, glm::vec2(0.5f, 0.06f)));
	fixture.Window.Frames(2);
	VisualScriptDetails const& details = fixture.Window.Panel->GetDetails();
	REQUIRE(details.GetSubject() == VisualScriptDetailsSubject::Node);
	CHECK(details.GetRowText("Inputs") == std::optional<std::string>("a \xC2\xB7 float, b \xC2\xB7 int"));
	CHECK_FALSE(details.GetRowText("Formula Problem 0").has_value());

	fixture.Window.Click(Testing::CanvasPoint(fixture.Model(), 13, glm::vec2(0.5f, 0.06f)));
	fixture.Window.Frames(2);
	CHECK(details.GetRowText("Formula Problem 0") ==
	      std::optional<std::string>("a * b + sni(t)\n        ^^^ unknown function sni: did you mean sin?"));
}

namespace
{
	// spec-slice3 §3.8.2's example, and the timeline of Documents/Language.json: the seeds of the Timeline parser's corpus.
	std::vector<Json> MakeTimelineSeeds()
	{
		Result<Json> example = ParseJson(R"({
			"Length": 2.0, "Loop": false,
			"Tracks": [
				{ "Name": "Lift", "Type": "float", "Keys": [
					{ "Time": 0.0, "Value": 0.0 },
					{ "Time": 1.0, "Value": 2.5, "Interpolation": "Linear" },
					{ "Time": 2.0, "Value": 0.0, "In": 0.0, "Out": 0.0 } ] },
				{ "Name": "Tint", "Type": "Strada.Color", "Keys": [ { "Time": 0.0, "Value": [1, 1, 1, 1] }, { "Time": 2.0, "Value": [1, 0.25, 0.25, 1] } ] },
				{ "Name": "Step", "Type": "exec", "Keys": [ { "Time": 0.5 }, { "Time": 1.5 } ] }
			] })");
		REQUIRE(example.IsOk());
		std::vector<Json> seeds = {example.TakeValue()};
		VisualScriptDocument const language = LoadTestDocument("Language.json");
		for (VisualScriptNode const& node : language.Graph.Nodes)
		{
			if (node.Type == "Flow.Timeline")
			{
				seeds.push_back(node.Settings["Timeline"]);
			}
		}
		REQUIRE(seeds.size() == 2);
		return seeds;
	}
}

TEST_CASE("VisualScriptEditor: the editor's Timeline parser accepts a setting exactly when the runtime does, and refuses it in its words")
{
	Testing::ScriptRuntimeScope runtime;
	std::vector<Json> const seeds = MakeTimelineSeeds();
	Testing::DocumentMutator mutator(10900);
	int const rounds = Testing::GetFuzzRounds(300);
	int accepted = 0;
	int refused = 0;
	for (int round = 0; round < rounds; round++)
	{
		Json const damaged = mutator.Damage(seeds[static_cast<size_t>(round) % seeds.size()], round);
		CAPTURE(damaged.dump());
		VisualScriptDocument document;
		document.Graph.Nodes = {MakeNode(1, "Flow.Timeline", glm::vec2(0.0f), Json::object({{"Timeline", damaged}}))};
		Result<Ref<VisualScriptAnalysis const>> const analysis = VisualScriptEngine::Analyze(document);
		REQUIRE_MESSAGE(analysis.IsOk(), (analysis ? std::string() : analysis.GetError()));
		// The runtime's problems with the setting: every finding of the Timeline reader carries the setting (spec-slice4 §16.4),
		// whatever its code.
		std::vector<std::string> problems;
		for (VisualScriptDiagnostic const& diagnostic : analysis.GetValue()->Diagnostics)
		{
			if (diagnostic.Node == 1 && diagnostic.Setting == "Timeline")
			{
				problems.push_back(diagnostic.Message);
			}
		}
		Result<VisualScriptTimeline> const parsed = ParseVisualScriptTimeline(damaged);
		if (parsed)
		{
			accepted++;
			CHECK_MESSAGE(problems.empty(), problems.front());
		}
		else
		{
			refused++;
			REQUIRE_FALSE(problems.empty());
			CHECK(parsed.GetError() == problems.front());
		}
	}
	CHECK(accepted > 0);
	CHECK(refused > 0);
}

TEST_CASE("VisualScriptEditor: a timeline the runtime plays gives the values the editor's curves draw")
{
	// A Timeline of 2 s whose curve has every kind of segment: cubic with auto and given tangents, linear and constant.
	Result<Json> const setting = ParseJson(R"({"Length": 2, "Tracks": [{"Name": "Lift", "Type": "float", "Keys": [
		{"Time": 0, "Value": 0}, {"Time": 0.5, "Value": 1.25, "Out": 3}, {"Time": 1, "Value": -0.5, "In": -1, "Interpolation": "Linear"},
		{"Time": 1.5, "Value": 0.75, "Interpolation": "Constant"}, {"Time": 1.75, "Value": 0.25}, {"Time": 2, "Value": 2}]}]})");
	REQUIRE(setting.IsOk());
	Result<VisualScriptTimeline> const timeline = ParseVisualScriptTimeline(setting.GetValue());
	REQUIRE(timeline.IsOk());

	// Times at random (a fixed seed), some outside the length (SetTime clamps them).
	Testing::FuzzRandom random(10950);
	constexpr uint32_t Samples = 24;
	std::vector<double> times;
	for (uint32_t sample = 0; sample < Samples; sample++)
	{
		times.push_back(static_cast<double>(random.Pick(2400)) / 1000.0 - 0.2);
	}

	// Create -> Sequence: for each time, a Timeline's SetTime, then Samples = Add(Samples, its Lift); last, Text = Join(Samples).
	VisualScriptDocument document;
	VisualScriptVariable samples = MakeVariable(1, "Samples", "float[]", Json::array());
	samples.Public = false;
	document.Variables.push_back(std::move(samples));
	document.Variables.push_back(MakeVariable(2, "Text", "string", ""));
	document.Graph.Nodes = {MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
	                        MakeNode(11, "Flow.Sequence", glm::vec2(320.0f, 0.0f), Json::object({{"Count", 2 * Samples + 1}})),
	                        MakeNode(12, "Variable.Get", glm::vec2(320.0f, 400.0f), Json::object({{"Variable", 1}})),
	                        MakeNode(13, "Variable.Set", glm::vec2(640.0f, 0.0f), Json::object({{"Variable", 2}})),
	                        MakeNode(14, "String.Join", glm::vec2(640.0f, 200.0f), Json::object(), Json::object({{"Separator", " "}}))};
	document.Graph.Links = {MakeLink(10, "Then", 11, "Execute"), MakeLink(11, fmt::format("Then{}", 2 * Samples), 13, "Execute"),
	                        MakeLink(12, "Value", 14, "Parts"), MakeLink(14, "Result", 13, "Value")};
	for (uint32_t sample = 0; sample < Samples; sample++)
	{
		uint32_t const node = 100 + sample * 3;
		auto const offset = static_cast<float>(sample) * 200.0f;
		document.Graph.Nodes.push_back(MakeNode(node, "Flow.Timeline", glm::vec2(960.0f, offset),
		                                        Json::object({{"Timeline", setting.GetValue()}}),
		                                        Json::object({{"NewTime", times[sample]}})));
		document.Graph.Nodes.push_back(MakeNode(node + 1, "Array.Add", glm::vec2(1280.0f, offset)));
		document.Graph.Nodes.push_back(MakeNode(node + 2, "Variable.Set", glm::vec2(1600.0f, offset), Json::object({{"Variable", 1}})));
		document.Graph.Links.push_back(MakeLink(11, fmt::format("Then{}", 2 * sample), node, "SetTime"));
		document.Graph.Links.push_back(MakeLink(11, fmt::format("Then{}", 2 * sample + 1), node + 2, "Execute"));
		document.Graph.Links.push_back(MakeLink(12, "Value", node + 1, "Array"));
		document.Graph.Links.push_back(MakeLink(node, "Lift", node + 1, "Value"));
		document.Graph.Links.push_back(MakeLink(node + 1, "Result", node + 2, "Value"));
	}
	RuntimeFixture fixture(document);
	VisualScriptAnalysis const* analysis = fixture.Model().GetAnalysis();
	REQUIRE(analysis != nullptr);
	for (VisualScriptDiagnostic const& diagnostic : analysis->Diagnostics)
	{
		CAPTURE(diagnostic.Message);
		CHECK(diagnostic.Severity != VisualScriptSeverity::Error);
	}
	UUID const entity = fixture.CreateRunner("Sampler");
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(2, Timestep(FrameTime));
	ScriptFieldValue const text = fixture.GetVariable(entity, "Text");
	fixture.Play.Stop();
	std::string const joined = text.GetString();
	CAPTURE(joined);

	std::vector<float> read;
	for (size_t start = 0; start < joined.size();)
	{
		size_t const end = std::min(joined.find(' ', start), joined.size());
		// Read as documents are (VisualScriptLiterals.cpp): std::from_chars for floats is missing from some standard libraries.
		std::string_view const sample(joined.data() + start, end - start);
		CAPTURE(sample);
		Result<Json> const number = ParseJson(sample);
		REQUIRE(number.IsOk());
		std::optional<float> const value = VisualScriptSerializer::ReadFloat(number.GetValue());
		REQUIRE(value.has_value());
		read.push_back(*value);
		start = end + 1;
	}
	REQUIRE(read.size() == Samples);
	for (uint32_t sample = 0; sample < Samples; sample++)
	{
		double const time = std::clamp(times[sample], 0.0, 2.0);
		CAPTURE(time);
		float const expected = EvaluateVisualScriptTimelineTrack(timeline.GetValue().Tracks[0], time).x;
		CHECK(std::abs(read[sample] - expected) <= 1e-6f * std::max(1.0f, std::abs(expected)));
	}
}

namespace
{
	// A graph that runs, laid out as a grid: on every update (node 10) a chain of increments of Count, each a Get of Count
	// (12 + 3i), an Add of 1 (13 + 3i) and a Set (14 + 3i) in a row, the chain's rows one below the other in columns of rows;
	// node 11, a Get of Step (5), links nowhere. A link from its Value to the first increment's B (node 13) changes the
	// program: each update then adds increments + 4. 2 + 3 × increments nodes.
	VisualScriptDocument MakeIncrementGrid(uint32_t increments, uint32_t rows)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
		document.Variables.push_back(MakeVariable(2, "Step", "int", 5));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update", glm::vec2(0.0f, 0.0f)));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Get", glm::vec2(0.0f, 180.0f), Json::object({{"Variable", 2}})));
		uint32_t previous = 10;
		for (uint32_t index = 0; index < increments; index++)
		{
			uint32_t const get = 12 + 3 * index;
			glm::vec2 const at(320.0f + static_cast<float>(index / rows) * 960.0f, static_cast<float>(index % rows) * 180.0f);
			document.Graph.Nodes.push_back(MakeNode(get, "Variable.Get", at, Json::object({{"Variable", 1}})));
			document.Graph.Nodes.push_back(
				MakeNode(get + 1, "Operator.Add", at + glm::vec2(320.0f, 0.0f), Json::object(), Json::object({{"B", 1}})));
			document.Graph.Nodes.push_back(
				MakeNode(get + 2, "Variable.Set", at + glm::vec2(640.0f, 0.0f), Json::object({{"Variable", 1}})));
			document.Graph.Links.push_back(MakeLink(get, "Value", get + 1, "A"));
			document.Graph.Links.push_back(MakeLink(get + 1, "Result", get + 2, "Value"));
			document.Graph.Links.push_back(MakeLink(previous, "Then", get + 2, "Execute"));
			previous = get + 2;
		}
		return document;
	}

	double Median(std::vector<double> values)
	{
		std::sort(values.begin(), values.end());
		return values.empty() ? 0.0 : values[values.size() / 2];
	}

	// The medians track L measured on its machine for the play-time new-link frame of 200 and 2,000 nodes (spec-slice4
	// section 21; five runs in Release, three in Debug), in milliseconds; the wall-clock checks allow 3 times them. Release
	// has the targets, 12 and 100 ms (100 without the managed analysis memo, which was not taken: ruling S4-R9.2).
#if defined(ST_DEBUG)
	constexpr std::array<double, 2> PlayLinkFrameMilliseconds = {41.8, 373.0};
	constexpr std::array<double, 2> PlayLinkFrameTargets = {0.0, 0.0};
#else
	constexpr std::array<double, 2> PlayLinkFrameMilliseconds = {6.49, 58.9};
	constexpr std::array<double, 2> PlayLinkFrameTargets = {12.0, 100.0};
#endif
}

TEST_CASE("VisualScriptEditor: the frame that commits a new link while the game plays loads it into the running instance, with "
          "one receipt")
{
	// A program-changing commit while playing also loads the program into the running game, in the commit's own frame
	// (spec-slice4 section 8.1): one load and one receipt, Applied to the instance. 200 nodes, and 2,000 as well on request,
	// with seven commits each, undone between them.
	bool const performance = Testing::ArePerformanceTestsEnabled();
	struct Grid
	{
		uint32_t Increments = 0;
		uint32_t Rows = 0;
	};
	std::vector<Grid> grids = {{66, 20}};
	if (performance)
	{
		grids.push_back({666, 50});
	}
	VisualScriptLink const link{{11, "Value"}, {13, "B"}};
	for (size_t size = 0; size < grids.size(); size++)
	{
		uint32_t const nodes = 2 + 3 * grids[size].Increments;
		CAPTURE(nodes);
		RuntimeFixture fixture(MakeIncrementGrid(grids[size].Increments, grids[size].Rows));
		fixture.Window.CheckDrawingChangesNothing();
		VisualScriptOpenRequest request;
		request.Graph = fixture.Window.Graph;
		request.Center = glm::vec2(400.0f, 300.0f);
		request.Zoom = 1.0f;
		request.Focus = false;
		fixture.Window.Panel->Apply(request);
		fixture.Window.Frames(2);
		REQUIRE(fixture.Model().GetAnalysis() != nullptr);
		REQUIRE_FALSE(fixture.Window.HasLink(link));
		UUID const counter = fixture.CreateRunner("Counter");
		REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
		fixture.Play.Advance(2, Timestep(FrameTime));
		fixture.Window.Frames(2);
		REQUIRE(VisualScriptEngine::HasInstance(counter));

		std::vector<double> times;
		for (int commit = 0, commits = performance ? 7 : 1; commit < commits; commit++)
		{
			uint64_t const generation = VisualScriptEngine::GetProgramGeneration(fixture.Window.Graph);
			uint64_t const serial = VisualScriptEngine::GetLiveLog()->Serial;
			size_t const undo = fixture.Window.GetUndoCount();
			glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), link.From.Node, link.From.Pin, VisualScriptPinDirection::Out);
			glm::vec2 const to = Testing::CanvasPoint(fixture.Model(), link.To.Node, link.To.Pin);
			fixture.Window.MoveMouse(from);
			fixture.Window.SetButton(ImGuiMouseButton_Left, true);
			fixture.Window.MoveMouse((from + to) * 0.5f);
			fixture.Window.MoveMouse(to);
			auto const start = std::chrono::steady_clock::now();
			fixture.Window.SetButton(ImGuiMouseButton_Left, false);
			times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			REQUIRE(fixture.Window.GetUndoCount() == undo + 1);
			REQUIRE(fixture.Window.HasLink(link));

			// One load in the commit's frame, and its receipt.
			CHECK(VisualScriptEngine::GetProgramGeneration(fixture.Window.Graph) == generation + 1);
			Ref<VisualScriptLiveLog const> const log = VisualScriptEngine::GetLiveLog();
			std::vector<Ref<VisualScriptLiveEdit const>> receipts;
			std::copy_if(log->Edits.begin(), log->Edits.end(), std::back_inserter(receipts),
			             [serial](Ref<VisualScriptLiveEdit const> const& edit)
			             {
							 return edit->Serial > serial;
						 });
			REQUIRE(receipts.size() == 1);
			CHECK(receipts[0]->Graph == fixture.Window.Graph);
			CHECK_FALSE(receipts[0]->Cause.IsValid());
			CHECK(receipts[0]->Outcome == VisualScriptLiveOutcome::Applied);
			CHECK(receipts[0]->Instances == 1);
			// The instance runs it: the first increment adds Step.
			int32_t const before = fixture.GetVariable(counter, "Count").GetInt32();
			fixture.Play.Advance(1, Timestep(FrameTime));
			CHECK(fixture.GetVariable(counter, "Count").GetInt32() == before + static_cast<int32_t>(grids[size].Increments) + 4);

			// Undone for the next commit, which the window shows in its next frames.
			REQUIRE(fixture.Window.Operations.Undo().IsOk());
			fixture.Window.Frames(2);
			REQUIRE_FALSE(fixture.Window.HasLink(link));
		}
		fixture.Play.Stop();
		if (performance)
		{
			double const median = Median(times);
			MESSAGE("play-time commit frame of a new link, ", nodes, " nodes: median ", median, " ms");
			// A target is printed as met or missed, not asserted (spec-slice4 section 21).
			if (PlayLinkFrameTargets[size] > 0.0)
			{
				std::string const verdict = median <= PlayLinkFrameTargets[size] ? "met" : "missed";
				MESSAGE("target ", PlayLinkFrameTargets[size], " ms: ", verdict);
			}
			CHECK(median <= 3.0 * PlayLinkFrameMilliseconds[size]);
		}
	}
}

// --- The live channel in the window (spec-slice4 §15.1-§15.4, §15.7, §15.9) ---

namespace
{
	// On every update: Count += 1 (Set 11, its Add 12 and Get 13), then Steps += 1 (Set 14, its Add 15 and Get 16).
	VisualScriptDocument MakeLiveCounterDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
		document.Variables.push_back(MakeVariable(2, "Steps", "int", 0));
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

	// The counter with a call of a member no type has (node 20) after its last Set: it does not compile.
	VisualScriptDocument MakeBrokenLiveCounterDocument()
	{
		VisualScriptDocument document = MakeLiveCounterDocument();
		document.Graph.Nodes.push_back(MakeNode(20, "Call:Strada.Nothing.Here()", glm::vec2(480.0f, 320.0f)));
		document.Graph.Links.push_back(MakeLink(14, "Then", 20, "Execute"));
		return document;
	}

	// On every update, Result = 1 / Zero (the Divide is node 12): an integer division by zero every frame.
	VisualScriptDocument MakeLiveDivisionDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Result", "int", 0));
		document.Variables.push_back(MakeVariable(2, "Zero", "int", 0));
		document.Graph.Nodes = {MakeNode(10, "Event.Update", glm::vec2(0.0f, 0.0f)),
		                        MakeNode(11, "Variable.Set", glm::vec2(320.0f, 0.0f), Json::object({{"Variable", 1}})),
		                        MakeNode(12, "Operator.Divide", glm::vec2(160.0f, 160.0f), Json::object(), Json::object({{"A", 1}})),
		                        MakeNode(13, "Variable.Get", glm::vec2(0.0f, 160.0f), Json::object({{"Variable", 2}}))};
		document.Graph.Links = {MakeLink(10, "Then", 11, "Execute"), MakeLink(13, "Value", 12, "B"), MakeLink(12, "Result", 11, "Value")};
		return document;
	}

	// The graph windows with the debug session and play mode as EditorLayer wires them, and the commands that read them
	// (graph.editors), drawn headless frame by frame as the editor runs them.
	struct LiveFixture
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
		CommandRegistry Registry;
		VisualScriptDrawRecord Record;

		LiveFixture()
		{
			Gui.UseEditorStyle(true);
			Settings.SetHintSeen(VisualScriptCanvasHint);
			Settings.SetReduceMotion(true);
			EditorCommandEnvironment environment;
			environment.GraphEditors = [this]() -> VisualScriptEditors&
			{
				return Editors;
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
			Session.SetViewSettings(&Settings);
			Editors.SetDebugSession(&Session);
		}

		~LiveFixture()
		{
			Play.Stop();
			Play.SetObserver(nullptr);
			Editors.SetDebugSession(nullptr);
			Session.SetViewSettings(nullptr);
		}

		LiveFixture(LiveFixture const&) = delete;
		LiveFixture& operator=(LiveFixture const&) = delete;

		Json Ok(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			REQUIRE_MESSAGE(result->IsOk(), (result->IsError() ? result->GetError().Message : std::string()));
			return result->TakeValue();
		}

		AssetHandle CreateGraph(std::string const& path, VisualScriptDocument const& document)
		{
			std::string const graph = Ok("graph.create", Json::object({{"path", path}}))["asset"].get<std::string>();
			Result<AssetHandle> const handle = AssetManager::ResolveReference(graph);
			REQUIRE(handle.IsOk());
			Set(handle.GetValue(), document);
			return handle.GetValue();
		}

		void Set(AssetHandle graph, VisualScriptDocument const& document)
		{
			Ok("graph.set", Json::object({{"graph", graph.ToString()}, {"document", VisualScriptSerializer::SerializeDocument(document)}}));
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

		// An editor frame: the queued resumes, play mode's update, the session's end, then the windows drawn (seconds of editor time
		// after the last).
		void EditorFrame(float seconds = FrameTime)
		{
			Session.BeginFrame();
			Play.Update(Timestep(FrameTime));
			Session.EndFrame();
			Record.Clear();
			Gui.Frame(Drawer(), seconds);
		}

		void Frames(int count)
		{
			for (int i = 0; i < count; i++)
			{
				EditorFrame();
			}
		}

		std::function<void()> Drawer()
		{
			return [this]
			{
				Editors.OnImGuiRender();
			};
		}

		VisualScriptEditorPanel& Window(AssetHandle graph)
		{
			VisualScriptEditorPanel* panel = Editors.FindPanel(graph);
			REQUIRE(panel != nullptr);
			return *panel;
		}

		// graph.editors' entry of a graph's window.
		Json Editor(AssetHandle graph)
		{
			Json const editors = Ok("graph.editors")["editors"];
			for (Json const& editor : editors)
			{
				if (editor["asset"] == graph.ToString())
				{
					return editor;
				}
			}
			FAIL("no window of ", graph.ToString());
			return Json();
		}

		// The window's toasts that begin with a text.
		std::vector<std::string> Toasts(AssetHandle graph, std::string_view start = {})
		{
			std::vector<std::string> toasts;
			for (std::string const& toast : Window(graph).GetState().Toasts)
			{
				if (toast.starts_with(start))
				{
					toasts.push_back(toast);
				}
			}
			return toasts;
		}

		// Brings a window to the front (and makes it the edit target), drawn so in the next frame.
		void Focus(AssetHandle graph)
		{
			std::string const name = VisualScriptEditorPanel::GetWindowName(graph, {});
			Gui.Frame(
				[this, &name]
				{
					Editors.OnImGuiRender();
					ImGui::SetWindowFocus(name.c_str());
				});
			Gui.Frame(Drawer());
		}

		// Clicks the middle of a rectangle of a window's last draw, in front of the window.
		void Click(AssetHandle graph, glm::vec2 min, glm::vec2 max)
		{
			Focus(graph);
			Gui.Click(ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), Drawer());
		}

		void ClickItem(AssetHandle graph, VisualScriptPanelItem item)
		{
			Focus(graph);
			std::optional<VisualScriptRect> const rect = Window(graph).GetItemRect(item);
			REQUIRE(rect.has_value());
			Gui.Click(ImVec2((rect->Min.x + rect->Max.x) * 0.5f, (rect->Min.y + rect->Max.y) * 0.5f), Drawer());
		}

		// The live marks of a kind on a node, or on a link (the dashed wires of what the game does not run), in the last draw.
		size_t CountLiveMarks(VisualScriptLiveMarkKind kind, std::optional<uint32_t> node = std::nullopt, bool links = false) const
		{
			return static_cast<size_t>(std::count_if(Record.Items.begin(), Record.Items.end(),
			                                         [kind, node, links](VisualScriptDrawRecordItem const& item)
			                                         {
														 bool const link = item.Has(VisualScriptDrawElements::Dashed) &&
				                                                           !item.Has(VisualScriptDrawElements::Outline);
														 return item.Kind == VisualScriptDrawItemKind::LiveMark &&
				                                                item.Mark == static_cast<uint8_t>(kind) && link == links &&
				                                                (!node || item.Element == *node);
													 }));
		}
	};
}

TEST_CASE("VisualScriptEditor: a commit while playing toasts its receipt and graph.editors reports it in the commit's frame; a window "
          "drawn after it keeps its revision; an undo's toast says what it did")
{
	LiveFixture fixture;
	AssetHandle const counter = fixture.CreateGraph("Scripts/Counter.sgraph", MakeLiveCounterDocument());
	AssetHandle const other = fixture.CreateGraph("Scripts/Other.sgraph", MakeLiveCounterDocument());
	fixture.CreateRunner("Counter", counter);
	fixture.CreateRunner("Other", other);
	// Drawn in this order.
	fixture.Open(counter);
	fixture.Open(other);
	fixture.Start();
	fixture.Frames(3);
	// Play's first loads are nobody's edits: no toast, no receipt (S4-R11.3).
	CHECK(fixture.Toasts(counter, "Live: ").empty());
	CHECK(fixture.Editor(counter)["liveReceipt"].is_null());

	// The window deletes Set Steps: its receipt toasts in the frame of the commit, which graph.editors reports.
	fixture.Ok("graph.open", Json::object({{"graph", counter.ToString()}, {"select", Json::array({14})}}));
	fixture.Frames(2);
	uint64_t const before = fixture.Editors.GetLiveLog()->Serial;
	REQUIRE(fixture.Window(counter).RequestAction(VisualScriptEditorAction::Delete));
	fixture.EditorFrame();
	REQUIRE(fixture.Window(counter).GetModel().GetDisplayDocument().Graph.Nodes.size() == 6);
	CHECK(fixture.Toasts(counter, "Live: ") == std::vector<std::string>{"Live: applied to 1 instance"});
	Json const editor = fixture.Editor(counter);
	REQUIRE(editor["liveReceipt"].is_object());
	CHECK(editor["liveReceipt"]["outcome"] == "Applied");
	CHECK(editor["liveReceipt"]["instances"] == 1);
	CHECK(editor["liveReceipt"]["serial"].get<uint64_t>() > before);
	CHECK(editor["liveReceipt"]["cause"].is_null());
	CHECK(editor["live"] == "Live");
	// The committing window draws the rest of its frame from the newer revision; the window drawn after it keeps the one it was
	// given before the windows drew.
	Ref<VisualScriptLiveLog const> const given = fixture.Editors.GetLiveLog();
	CHECK(fixture.Window(other).GetLiveLog() == given);
	CHECK(fixture.Window(counter).GetLiveLog() != given);
	CHECK(fixture.Window(counter).GetLiveLog()->Serial > given->Serial);
	// The other window took no receipt of its own.
	CHECK(fixture.Toasts(other, "Live: ").empty());
	CHECK(fixture.Editor(other)["liveReceipt"].is_null());

	// An undo applies at once too: its history toast and its receipt are one toast.
	fixture.Focus(counter);
	REQUIRE(fixture.Editors.GetEditTarget() == &fixture.Window(counter));
	UndoEditorStep(fixture.Operations, &fixture.Editors);
	fixture.Frames(2);
	std::vector<std::string> const undone = fixture.Toasts(counter, "Undone: ");
	REQUIRE(undone.size() == 1);
	CHECK(undone.front().ends_with(" \xC2\xB7 applied to 1 instance"));
	CHECK(fixture.Toasts(counter, "Live: ").size() == 1);
	CHECK(fixture.Window(counter).GetModel().GetDisplayDocument().Graph.Nodes.size() == 7);
}

TEST_CASE("VisualScriptEditor: a broken edit while playing toasts that the game keeps the last version, shows LAST WORKING and outlines "
          "exactly what does not run; fixing it applies and the outline goes")
{
	LiveFixture fixture;
	AssetHandle const counter = fixture.CreateGraph("Scripts/Counter.sgraph", MakeLiveCounterDocument());
	fixture.CreateRunner("Counter", counter);
	fixture.Open(counter);
	fixture.Window(counter).SetDrawRecord(&fixture.Record);
	fixture.Start();
	fixture.Frames(3);
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::NotRunning) == 0);

	// An agent's edit the window adopts: toasted at its next draw.
	fixture.Set(counter, MakeBrokenLiveCounterDocument());
	fixture.Frames(2);
	// "not applied: 2 errors (first: unknown node type ... (node 20)); ...", the errors the log prints.
	std::vector<std::string> const kept = fixture.Toasts(counter, "Live: not applied: ");
	REQUIRE(kept.size() == 1);
	CHECK(kept.front().find(" (first: ") != std::string::npos);
	CHECK(kept.front().find("(node 20))") != std::string::npos);
	CHECK(kept.front().ends_with("; the game keeps running the last version that worked"));
	Json editor = fixture.Editor(counter);
	CHECK(editor["live"] == "LastWorking");
	CHECK(editor["notRunning"] == 1);
	CHECK(editor["liveReceipt"]["outcome"] == "KeptLastWorking");
	// The new node is outlined, and the link into it dashed: nothing else.
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::NotRunning, 20) == 1);
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::NotRunning) == 1);
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::NotRunning, std::nullopt, true) == 1);
	// The outline is the user's choice (graph.open sets it too).
	fixture.Ok("graph.open", Json::object({{"graph", counter.ToString()}, {"outlineNotRunning", false}}));
	fixture.Frames(1);
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::NotRunning) == 0);
	CHECK(fixture.Ok("graph.editors")["settings"]["outlineNotRunning"] == false);
	CHECK(fixture.Editor(counter)["notRunning"] == 1);
	fixture.Ok("graph.open", Json::object({{"graph", counter.ToString()}, {"outlineNotRunning", true}}));
	fixture.Frames(1);
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::NotRunning, 20) == 1);

	// Fixed (Steps grow by 2 now): applied, and the marks go.
	VisualScriptDocument fixed = MakeLiveCounterDocument();
	fixed.Graph.Nodes[5].Inputs["B"] = 2;
	fixture.Set(counter, fixed);
	fixture.Frames(2);
	CHECK(fixture.Toasts(counter, "Live: applied to 1 instance").size() == 1);
	editor = fixture.Editor(counter);
	CHECK(editor["live"] == "Live");
	CHECK(editor["notRunning"] == 0);
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::NotRunning) == 0);
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::NotRunning, std::nullopt, true) == 0);
}

TEST_CASE("VisualScriptEditor: a broken edit while stopped keeps the stop with LAST WORKING and STOPPED and the strip's line; fixing it "
          "reaches the stopped execution")
{
	LiveFixture fixture;
	AssetHandle const counter = fixture.CreateGraph("Scripts/Counter.sgraph", MakeLiveCounterDocument());
	fixture.CreateRunner("Counter", counter);
	fixture.Open(counter);
	fixture.Ok("graph.breakpoints", Json::object({{"graph", counter.ToString()}, {"add", Json::array({Json::object({{"node", 14}})})}}));
	fixture.Start();
	for (int frame = 0; frame < 30 && fixture.Session.GetSessionState() != VisualScriptDebugSessionState::Stopped; frame++)
	{
		fixture.EditorFrame();
	}
	REQUIRE(fixture.Session.GetSessionState() == VisualScriptDebugSessionState::Stopped);
	fixture.Frames(2);
	// My Script shows the stopped frame's instance's variables (spec-slice4 §13.6), which the session read: no read of the
	// running instance meanwhile.
	uint64_t const reads = fixture.Window(counter).GetActivity().LiveValueReads;
	fixture.Frames(20);
	REQUIRE(fixture.Window(counter).GetLiveValues().has_value());
	CHECK(fixture.Window(counter).GetActivity().LiveValueReads == reads);
	VisualScriptValueCell const* count = fixture.Window(counter).GetSidebar().FindLiveCell(1);
	REQUIRE(count != nullptr);
	VisualScriptStop const* stop = fixture.Session.FindStop(*fixture.Session.GetSelectedStop());
	REQUIRE(stop != nullptr);
	VisualScriptStopValues const* values = fixture.Session.GetStopValues(stop->ID, 0);
	REQUIRE(values != nullptr);
	auto const stopped = std::find_if(values->Variables.begin(), values->Variables.end(),
	                                  [](VisualScriptDebugValue const& value)
	                                  {
										  return value.Name == "Count";
									  });
	REQUIRE(stopped != values->Variables.end());
	CHECK(count->Text == stopped->Value.dump());

	fixture.Set(counter, MakeBrokenLiveCounterDocument());
	fixture.Frames(2);
	Json const editor = fixture.Editor(counter);
	CHECK(editor["live"] == "LastWorking");
	CHECK(editor["debug"]["state"] == "Stopped");
	// Both pill segments, and the strip's line.
	CHECK(fixture.Window(counter).GetItemRect(VisualScriptPanelItem::Live).has_value());
	CHECK(fixture.Window(counter).GetItemRect(VisualScriptPanelItem::Stopped).has_value());
	std::vector<VisualScriptDebugStripRow> const& rows = fixture.Window(counter).GetStripRows();
	CHECK(std::any_of(rows.begin(), rows.end(),
	                  [](VisualScriptDebugStripRow const& row)
	                  {
						  return row.Text.starts_with("Your last edit has ") &&
		                         row.Text.ends_with(": this execution continues with the last version that worked");
					  }));

	// Fixed (Steps grow by 2 now): the stopped execution goes on with the edit.
	VisualScriptDocument fixed = MakeLiveCounterDocument();
	fixed.Graph.Nodes[5].Inputs["B"] = 2;
	fixture.Set(counter, fixed);
	fixture.Frames(2);
	CHECK(fixture.Toasts(counter, "Live: applied to 1 instance \xC2\xB7 the stopped execution continues with the edit").size() == 1);
	CHECK(fixture.Editor(counter)["live"] == "Live");
	CHECK(fixture.Editor(counter)["debug"]["state"] == "Stopped");
	fixture.Ok("graph.continue");
}

TEST_CASE("VisualScriptEditor: an edit that cancels a waiting Delay toasts it and marks the node for 4 seconds")
{
	LiveFixture fixture;
	VisualScriptDocument document = MakeDelayDocument();
	document.Graph.Nodes[1].Inputs["Duration"] = 100.0;
	AssetHandle const waiter = fixture.CreateGraph("Scripts/Waiter.sgraph", document);
	fixture.CreateRunner("Waiter", waiter);
	fixture.Open(waiter);
	fixture.Window(waiter).SetDrawRecord(&fixture.Record);
	fixture.Start();
	fixture.Frames(3);

	// The Delay (node 11) becomes a Log under its ID: the wait in it is cancelled.
	VisualScriptDocument changed = document;
	changed.Graph.Nodes[1] =
		MakeNode(11, "Call:Strada.Log.Info(string)", glm::vec2(320.0f, 0.0f), Json::object(), Json::object({{"Message", "Now"}}));
	changed.Graph.Links = {MakeLink(10, "Then", 11, "Execute"), MakeLink(11, "Then", 13, "Execute"), MakeLink(13, "Then", 12, "Execute")};
	fixture.Set(waiter, changed);
	fixture.Frames(1);
	CHECK(fixture.Toasts(waiter, "Live: applied to 1 instance \xC2\xB7 1 wait cancelled").size() == 1);
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::Cancelled, 11) == 1);
	fixture.EditorFrame(3.0f);
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::Cancelled, 11) == 1);
	fixture.EditorFrame(1.5f);
	fixture.EditorFrame();
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::Cancelled) == 0);
}

TEST_CASE("VisualScriptEditor: while playing the Timeline drawer draws the playhead at the context instance's time")
{
	LiveFixture fixture;
	VisualScriptDocument document;
	Json const timeline =
		Json::object({{"Length", 30.0},
	                  {"Loop", false},
	                  {"Tracks", Json::array({Json::object({{"Name", "Lift"},
	                                                        {"Type", "float"},
	                                                        {"Keys", Json::array({Json::object({{"Time", 0}, {"Value", 0}}),
	                                                                              Json::object({{"Time", 30}, {"Value", 1}})})}})})}});
	document.Graph.Nodes = {MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
	                        MakeNode(11, "Flow.Timeline", glm::vec2(320.0f, 0.0f), Json::object({{"Timeline", timeline}}))};
	document.Graph.Links = {MakeLink(10, "Then", 11, "PlayFromStart")};
	AssetHandle const graph = fixture.CreateGraph("Scripts/Rise.sgraph", document);
	UUID const riser = fixture.CreateRunner("Riser", graph);
	fixture.Open(graph);
	fixture.Ok("graph.open", Json::object({{"graph", graph.ToString()}, {"timeline", 11}}));
	fixture.Frames(2);
	VisualScriptCurveEditor const& curves = fixture.Window(graph).GetDrawer().GetCurveEditor();
	// Not playing: none.
	CHECK_FALSE(curves.GetPlayhead().has_value());
	CHECK_FALSE(curves.GetDrawRecord().Playhead);

	fixture.Start();
	fixture.Frames(20);
	REQUIRE(curves.GetPlayhead().has_value());
	VisualScriptValueRequest request;
	request.Entity = riser;
	request.Timelines = true;
	Result<VisualScriptLiveValues> const values = VisualScriptDebugger::ReadValues(request);
	REQUIRE(values.IsOk());
	REQUIRE(values.GetValue().Timelines.size() == 1);
	CHECK(*curves.GetPlayhead() == doctest::Approx(values.GetValue().Timelines.front().Time));
	CHECK(*curves.GetPlayhead() > 0.0);
	CHECK(curves.GetDrawRecord().Playhead);
	CHECK(curves.GetDrawRecord().PlayheadX == doctest::Approx(curves.GetView().ToScreenX(*curves.GetPlayhead())));
	// It moves with the game.
	double const earlier = *curves.GetPlayhead();
	fixture.Frames(10);
	CHECK(*curves.GetPlayhead() > earlier);
	fixture.Play.Stop();
	fixture.Frames(1);
	CHECK_FALSE(curves.GetPlayhead().has_value());
}

TEST_CASE("VisualScriptEditor: a runtime error badges its node with its count, toasts once, fills the Runtime section, and Go to frames "
          "it with its entity as the context entity")
{
	LiveFixture fixture;
	AssetHandle const divider = fixture.CreateGraph("Scripts/Divider.sgraph", MakeLiveDivisionDocument());
	UUID const first = fixture.CreateRunner("First", divider);
	fixture.CreateRunner("Second", divider);
	fixture.Open(divider);
	fixture.Window(divider).SetDrawRecord(&fixture.Record);
	fixture.Start();
	fixture.Frames(4);

	// One mark on the Divide for both instances, their counts summed.
	REQUIRE(fixture.CountLiveMarks(VisualScriptLiveMarkKind::RuntimeError, 12) == 1);
	auto const mark = std::find_if(fixture.Record.Items.begin(), fixture.Record.Items.end(),
	                               [](VisualScriptDrawRecordItem const& item)
	                               {
									   return item.Kind == VisualScriptDrawItemKind::LiveMark && item.Element == 12;
								   });
	REQUIRE(mark != fixture.Record.Items.end());
	CHECK(mark->Text.starts_with("\xC3\x97"));
	Json editor = fixture.Editor(divider);
	CHECK(editor["runtimeProblems"] == Json::object({{"errors", 2}, {"warnings", 0}}));
	// The node's first error toasted once, however often it recurs.
	std::vector<std::string> const failed = fixture.Toasts(divider, "Divide (node 12) failed on ");
	REQUIRE(failed.size() == 1);
	CHECK(failed.front().ends_with(": integer division by zero"));
	fixture.Frames(3);
	CHECK(fixture.Toasts(divider, "Divide (node 12) failed on ").size() == 1);

	// The Runtime section, above the analysis' problems.
	fixture.Ok("graph.open", Json::object({{"graph", divider.ToString()}, {"problems", true}}));
	fixture.Frames(2);
	VisualScriptDrawer const& drawer = fixture.Window(divider).GetDrawer();
	CHECK(drawer.GetRuntimeHeader() == "Runtime");
	REQUIRE(drawer.GetRuntimeRows().size() == 2);
	std::span<VisualScriptDrawerRuntimeRow const> const runtimeRows = drawer.GetRuntimeRows();
	auto const found = std::find_if(runtimeRows.begin(), runtimeRows.end(),
	                                [first](VisualScriptDrawerRuntimeRow const& candidate)
	                                {
										return candidate.Problem->Entity == first;
									});
	REQUIRE(found != runtimeRows.end());
	VisualScriptDrawerRuntimeRow const& row = *found;
	CHECK(row.Title == "Divide (node 12)");
	CHECK(row.Detail.starts_with("on First \xC2\xB7 \xC3\x97"));
	size_t const index = static_cast<size_t>(found - runtimeRows.begin());

	// Go to: the Divide selected and in view, First the context entity.
	fixture.Window(divider).ChooseContextEntity(UUID::Invalid());
	fixture.Frames(1);
	std::optional<std::pair<glm::vec2, glm::vec2>> const goTo = fixture.Window(divider).GetDrawer().FindRuntimeButton(index, "Go to");
	REQUIRE(goTo.has_value());
	fixture.Click(divider, goTo->first, goTo->second);
	fixture.Frames(2);
	std::span<uint32_t const> const selection = fixture.Window(divider).GetModel().GetSelection();
	CHECK(std::vector<uint32_t>(selection.begin(), selection.end()) == std::vector<uint32_t>{12});
	CHECK(fixture.Window(divider).GetContextEntity() == first);

	// After Stop: the last run's, until Clear.
	fixture.Play.Stop();
	fixture.Frames(2);
	CHECK(fixture.Window(divider).GetDrawer().GetRuntimeHeader() == "Runtime (last run)");
	std::optional<std::pair<glm::vec2, glm::vec2>> const clear = fixture.Window(divider).GetDrawer().FindRuntimeButton(SIZE_MAX, "Clear");
	REQUIRE(clear.has_value());
	fixture.Click(divider, clear->first, clear->second);
	fixture.Frames(2);
	CHECK(fixture.Window(divider).GetDrawer().GetRuntimeHeader().empty());
	CHECK(fixture.Editor(divider)["runtimeProblems"] == Json::object({{"errors", 0}, {"warnings", 0}}));
}

TEST_CASE("VisualScriptEditor: the Live popover says since when the graph runs, what the game runs, the last receipt in full and the "
          "runtime problems, and acts on them")
{
	LiveFixture fixture;
	VisualScriptDocument document = MakeDelayDocument();
	document.Graph.Nodes[1].Inputs["Duration"] = 100.0;
	AssetHandle const waiter = fixture.CreateGraph("Scripts/Waiter.sgraph", document);
	UUID const entity = fixture.CreateRunner("Waiter", waiter);
	fixture.Open(waiter);
	fixture.Window(waiter).SetDrawRecord(&fixture.Record);
	fixture.Start();
	fixture.Frames(3);

	// An edit that keeps the wait: the receipt's instance line.
	VisualScriptDocument longer = document;
	longer.Graph.Nodes[1].Inputs["Duration"] = 200.0;
	fixture.Set(waiter, longer);
	fixture.Frames(2);
	fixture.ClickItem(waiter, VisualScriptPanelItem::Live);
	fixture.Frames(1);
	VisualScriptEditorPanel const& window = fixture.Window(waiter);
	REQUIRE(window.IsLivePopoverOpen());
	std::vector<std::string> lines = window.GetLivePopoverLines();
	REQUIRE(lines.size() >= 3);
	CHECK(lines[0].starts_with("LIVE \xC2\xB7 running since "));
	CHECK(lines[0].ends_with(" ago)"));
	CHECK(lines[1] == "Last edit: applied to 1 instance");
	CHECK(std::find(lines.begin(), lines.end(), "Variables kept: Done") != lines.end());
	CHECK(std::find(lines.begin(), lines.end(), "Waiter: 1 wait kept") != lines.end());
	// An instance's line makes its entity the context entity.
	fixture.Window(waiter).ChooseContextEntity(UUID::Invalid());
	fixture.Frames(1);
	std::optional<VisualScriptRect> const instance = fixture.Window(waiter).GetItemRect(VisualScriptPanelItem::PopoverInstance);
	REQUIRE(instance.has_value());
	fixture.Gui.Click(ImVec2((instance->Min.x + instance->Max.x) * 0.5f, (instance->Min.y + instance->Max.y) * 0.5f), fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.Window(waiter).GetContextEntity() == entity);

	// Broken: what the game runs instead, with Show first error and the outline toggle.
	VisualScriptDocument broken = longer;
	broken.Graph.Nodes.push_back(MakeNode(20, "Call:Strada.Nothing.Here()", glm::vec2(640.0f, 200.0f)));
	broken.Graph.Links.push_back(MakeLink(12, "Then", 20, "Execute"));
	fixture.Set(waiter, broken);
	fixture.Frames(2);
	fixture.ClickItem(waiter, VisualScriptPanelItem::Live);
	fixture.Frames(1);
	REQUIRE(fixture.Window(waiter).IsLivePopoverOpen());
	lines = fixture.Window(waiter).GetLivePopoverLines();
	CHECK(lines[0].starts_with("LIVE \xC2\xB7 last working version \xC2\xB7 running since "));
	CHECK(std::find(lines.begin(), lines.end(), "The game runs the last version that worked: 1 node changed since is not running") !=
	      lines.end());
	CHECK(lines[2].starts_with("Last edit: not applied: "));
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::NotRunning, 20) == 1);
	std::optional<VisualScriptRect> const outline = fixture.Window(waiter).GetItemRect(VisualScriptPanelItem::PopoverOutline);
	REQUIRE(outline.has_value());
	fixture.Gui.Click(ImVec2((outline->Min.x + outline->Max.x) * 0.5f, (outline->Min.y + outline->Max.y) * 0.5f), fixture.Drawer());
	fixture.Frames(1);
	CHECK_FALSE(fixture.Settings.IsOutlineNotRunning());
	CHECK(fixture.CountLiveMarks(VisualScriptLiveMarkKind::NotRunning) == 0);
	fixture.Settings.SetOutlineNotRunning(true);
	std::optional<VisualScriptRect> const show = fixture.Window(waiter).GetItemRect(VisualScriptPanelItem::PopoverShowFirstError);
	REQUIRE(show.has_value());
	fixture.Gui.Click(ImVec2((show->Min.x + show->Max.x) * 0.5f, (show->Min.y + show->Max.y) * 0.5f), fixture.Drawer());
	fixture.Frames(2);
	CHECK(fixture.Editor(waiter)["drawer"] == "Problems");
	std::span<uint32_t const> const selection = fixture.Window(waiter).GetModel().GetSelection();
	CHECK(std::vector<uint32_t>(selection.begin(), selection.end()) == std::vector<uint32_t>{20});
	CHECK_FALSE(fixture.Window(waiter).IsLivePopoverOpen());
}

TEST_CASE("VisualScriptEditor: a library's Live popover lists what each graph that calls it runs, and its edits toast once in its own "
          "window")
{
	LiveFixture fixture;
	// A pure Twice(Value) = 2 × Value (function 20), called by two scripts that each run on an entity, and by one no entity runs.
	VisualScriptDocument library;
	library.Kind = VisualScriptDocumentKind::Library;
	VisualScriptFunction twice;
	twice.ID = 20;
	twice.Name = "Twice";
	twice.Pure = true;
	twice.Inputs.push_back({"Value", "int", {}});
	twice.Outputs.push_back({"Result", "int", {}});
	twice.Graph.Nodes = {MakeNode(21, "Function.Entry", glm::vec2(0.0f, 0.0f)),
	                     MakeNode(22, "Operator.Multiply", glm::vec2(200.0f, 0.0f), Json::object(), Json::object({{"B", 2}})),
	                     MakeNode(23, "Function.Return", glm::vec2(400.0f, 0.0f))};
	twice.Graph.Links = {MakeLink(21, "Value", 22, "A"), MakeLink(22, "Result", 23, "Result")};
	library.Functions.push_back(std::move(twice));
	AssetHandle const math = fixture.CreateGraph("Scripts/Math.sgraph", library);
	auto const caller = [&math]
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0));
		document.Graph.Nodes = {MakeNode(10, "Event.Update", glm::vec2(0.0f, 0.0f)),
		                        MakeNode(11, "Variable.Set", glm::vec2(320.0f, 0.0f), Json::object({{"Variable", 1}})),
		                        MakeNode(12, "Function.Call", glm::vec2(160.0f, 160.0f),
		                                 Json::object({{"Function", 20}, {"Graph", math.ToString()}}), Json::object({{"Value", 3}}))};
		document.Graph.Links = {MakeLink(10, "Then", 11, "Execute"), MakeLink(12, "Result", 11, "Value")};
		return document;
	};
	AssetHandle const first = fixture.CreateGraph("Scripts/First.sgraph", caller());
	AssetHandle const second = fixture.CreateGraph("Scripts/Second.sgraph", caller());
	fixture.CreateRunner("First", first);
	fixture.CreateRunner("Second", second);
	fixture.Open(first);
	fixture.Open(math);
	fixture.Start();
	fixture.Frames(3);

	// A body edit of the library: the graphs that call it compile again, said once in the library's window; the callers' windows
	// stay silent.
	VisualScriptDocument thrice = library;
	thrice.Functions.front().Graph.Nodes[1].Inputs["B"] = 3;
	fixture.Set(math, thrice);
	fixture.Frames(2);
	CHECK(fixture.Toasts(math, "Live: ") ==
	      std::vector<std::string>{"Live: 2 scripts that call Math compiled again; applied to 2 instances"});
	CHECK(fixture.Toasts(first, "Live: ").empty());
	CHECK(fixture.Editor(first)["liveReceipt"]["cause"] == math.ToString());

	fixture.ClickItem(math, VisualScriptPanelItem::Live);
	fixture.Frames(1);
	REQUIRE(fixture.Window(math).IsLivePopoverOpen());
	std::vector<std::string> const lines = fixture.Window(math).GetLivePopoverLines();
	CHECK(std::find(lines.begin(), lines.end(), "Graphs that call this library:") != lines.end());
	CHECK(std::find(lines.begin(), lines.end(), "Scripts/First.sgraph runs this version") != lines.end());
	CHECK(std::find(lines.begin(), lines.end(), "Scripts/Second.sgraph runs this version") != lines.end());
}

TEST_CASE("VisualScriptEditor: the runtime's finding of a damaged Timeline offers Edit in Details, which shows Details' Timeline section "
          "with its finding and gives Length the keyboard")
{
	// Spec-slice4 §15.9, the runtime half of S-R10.5: the runtime's finding carries Setting "Timeline".
	VisualScriptDocument document;
	Json timeline = Json::object({{"Length", 1.0}, {"Loop", false}, {"Tracks", Json::array()}});
	timeline["Tracks"].push_back(
		Json::object({{"Name", "Lift"},
	                  {"Type", "float"},
	                  {"Keys", Json::array({Json::object({{"Time", 0.5}, {"Value", 0}}), Json::object({{"Time", 4}, {"Value", 1}})})}}));
	document.Graph.Nodes = {MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
	                        MakeNode(11, "Flow.Timeline", glm::vec2(320.0f, 0.0f), Json::object({{"Timeline", timeline}}))};
	document.Graph.Links = {MakeLink(10, "Then", 11, "PlayFromStart")};
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();
	VisualScriptOpenRequest request;
	request.Graph = fixture.Window.Graph;
	request.ShowProblems = true;
	fixture.Window.Panel->Apply(request);
	fixture.Window.Frames(3);
	VisualScriptDrawer const& drawer = fixture.Window.Panel->GetDrawer();
	auto const damaged = std::find_if(drawer.GetProblems().begin(), drawer.GetProblems().end(),
	                                  [](VisualScriptDiagnostic const& diagnostic)
	                                  {
										  return diagnostic.Setting == "Timeline";
									  });
	REQUIRE(damaged != drawer.GetProblems().end());
	CHECK(damaged->Node == std::optional<uint32_t>(11));
	size_t const index = static_cast<size_t>(damaged - drawer.GetProblems().begin());
	std::optional<std::pair<glm::vec2, glm::vec2>> const fix = drawer.FindFixButton(index);
	REQUIRE(fix.has_value());

	// Edit in Details, the first fix: the node selected, Details' Timeline section from its header with the finding under it and
	// why the timeline cannot be edited there (what the runtime refuses, Details' parser refuses too); nothing edited.
	size_t const undo = fixture.Window.GetUndoCount();
	fixture.Window.Click((fix->first + fix->second) * 0.5f);
	fixture.Window.Frames(3);
	CHECK(fixture.Window.GetUndoCount() == undo);
	std::span<uint32_t const> const selection = fixture.Model().GetSelection();
	CHECK(std::vector<uint32_t>(selection.begin(), selection.end()) == std::vector<uint32_t>{11});
	VisualScriptDetails const& details = fixture.Window.Panel->GetDetails();
	REQUIRE(details.GetSubject() == VisualScriptDetailsSubject::Node);
	std::optional<std::pair<glm::vec2, glm::vec2>> const header = details.FindRow("Settings");
	std::optional<std::pair<glm::vec2, glm::vec2>> const problem = details.FindRow("Timeline Problem");
	std::optional<std::pair<glm::vec2, glm::vec2>> const refused = details.FindRow("Timeline");
	REQUIRE(header.has_value());
	REQUIRE(problem.has_value());
	REQUIRE(refused.has_value());
	CHECK(problem->first.y >= header->second.y);
	CHECK(refused->first.y >= problem->second.y);
	CHECK(details.GetRowText("Timeline").value_or("").starts_with("Cannot be edited here: Timeline.Tracks[0].Keys[1].Time"));
	CHECK_FALSE(fixture.Window.Panel->GetPaneShared().FocusSetting.has_value());
}

TEST_CASE("VisualScriptEditor: a value dragged in the canvas's live widget while playing is one step and one receipt toast, said anew in "
          "place at each commit")
{
	VisualScriptDocument document = MakeDelayDocument();
	document.Graph.Nodes[1].Inputs["Duration"] = 100.0;
	RuntimeFixture fixture(document);
	// As the window manager hands every window the frame's revision of the live log.
	fixture.Window.Probe = [&fixture]
	{
		fixture.Window.Panel->SetLiveLog(VisualScriptEngine::GetLiveLog());
	};
	fixture.Window.CheckDrawingChangesNothing();
	fixture.CreateRunner("Waiter");
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(2, Timestep(FrameTime));
	fixture.Window.Frames(2);
	size_t const undo = fixture.Window.GetUndoCount();
	auto const receipts = [&fixture]
	{
		Ref<VisualScriptLiveLog const> const log = VisualScriptEngine::GetLiveLog();
		return std::count_if(log->Edits.begin(), log->Edits.end(),
		                     [&fixture](Ref<VisualScriptLiveEdit const> const& edit)
		                     {
								 return edit->Graph == fixture.Window.Graph;
							 });
	};
	auto const before = receipts();

	// The Delay's Duration dragged over frames further apart than the commit cadence: several commits of one step.
	VisualScriptCanvasModel const& canvas = fixture.Model().GetCanvas();
	std::optional<uint32_t> const delay = canvas.FindNode(11);
	REQUIRE(delay.has_value());
	std::optional<uint32_t> const duration = canvas.FindPin(*delay, "Duration", VisualScriptPinDirection::In);
	REQUIRE(duration.has_value());
	glm::vec2 const chip = fixture.Model().GetView().ToScreen(
		VisualScriptGeometry::GetChipRect(canvas.GetNodes()[*delay], canvas.GetPins()[*duration]).GetCenter());
	fixture.Window.MoveMouse(chip);
	fixture.Window.Frames(1);
	REQUIRE(fixture.Window.Panel->GetLiveWidget().has_value());
	fixture.Window.SetButton(ImGuiMouseButton_Left, true);
	for (int step = 1; step <= 6; step++)
	{
		ImGui::GetIO().AddMousePosEvent(chip.x + 8.0f * static_cast<float>(step), chip.y);
		fixture.Window.Gui.Frame(fixture.Window.Drawer(), 0.15f);
	}
	fixture.Window.SetButton(ImGuiMouseButton_Left, false);
	fixture.Window.Frames(2);
	REQUIRE(fixture.Window.GetUndoCount() == undo + 1);
	CHECK(receipts() - before >= 2);
	// One toast for the step, its wait kept.
	std::vector<std::string> toasts;
	for (std::string const& toast : fixture.Window.Panel->GetState().Toasts)
	{
		if (toast.starts_with("Live: "))
		{
			toasts.push_back(toast);
		}
	}
	CHECK(toasts == std::vector<std::string>{"Live: applied to 1 instance"});
}
